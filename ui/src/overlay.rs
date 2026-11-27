//! In-game control center, rendered by the shell in its own topmost window.
//!
//! The engine keeps the compact stats card and the disconnect banner (GDI), but
//! the expanded panel is now GPUI so it matches the rest of 2.0. F10 with the
//! stream focused is forwarded by the engine as `E overlay-toggle`.

use std::sync::{Arc, Mutex};
use std::sync::atomic::{AtomicUsize, Ordering};
use std::time::Duration;

use gpui::{
    div, prelude::*, px, rgba, rgb, AnyElement, Context, FocusHandle, Focusable, FontWeight,
    InteractiveElement, IntoElement, Render, SharedString, StatefulInteractiveElement, Window,
};

use crate::bridge::{Devices, Engine, ProfileInfo, Shared};
use crate::model::{Settings, Status};
use crate::{theme, widgets};

const TABS: [&str; 9] = [
    "Info", "SR", "FG", "Neural", "Video", "Stream", "Audio", "Pad", "Profile",
];

// The focused tab survives closing and reopening the control center for the
// current game session; a disconnect resets it.
static OVERLAY_TAB: AtomicUsize = AtomicUsize::new(0);

pub(crate) fn reset_overlay_tab() {
    OVERLAY_TAB.store(0, Ordering::Relaxed);
}

pub struct OverlayView {
    engine: Engine,
    shared: Arc<Mutex<Shared>>,
    settings: Settings,
    status: Status,
    devices: Devices,
    profiles: Vec<ProfileInfo>,
    profiles_rev: u64,
    tab: usize,
    num_edit: Option<&'static str>,
    num_text: String,
    num_fresh: bool,
    dirty_at: Option<std::time::Instant>,
    /// (profile id, when armed) for the two-step delete in the Profiles tab.
    confirm_profile: Option<(String, std::time::Instant)>,
    /// One delete per gesture: queued clicks after a stall must not cascade.
    profile_delete_cooldown: Option<std::time::Instant>,
    pub(crate) focus: FocusHandle,
    last_rev: u64,
    last_devices_rev: u64,
}

impl OverlayView {
    pub fn new(engine: Engine, cx: &mut Context<Self>) -> Self {
        let shared = engine.shared.clone();
        let (status, devices, devices_rev, profiles, profiles_rev) = {
            let state = shared.lock().unwrap();
            (
                state.status.clone(),
                state.devices.clone().unwrap_or_default(),
                state.devices_rev,
                state.profiles.clone(),
                state.profiles_rev,
            )
        };
        let focus = cx.focus_handle();
        let view = Self {
            engine,
            shared,
            settings: Settings::load(),
            status,
            devices,
            profiles,
            profiles_rev,
            tab: OVERLAY_TAB.load(Ordering::Relaxed).min(TABS.len() - 1),
            num_edit: None,
            num_text: String::new(),
            num_fresh: false,
            dirty_at: None,
            confirm_profile: None,
            profile_delete_cooldown: None,
            focus,
            last_rev: 0,
            last_devices_rev: devices_rev,
        };
        cx.spawn(async move |this, cx| loop {
            cx.background_executor()
                .timer(Duration::from_millis(150))
                .await;
            let result = this.update(cx, |this, cx| {
                if let Some(at) = this.dirty_at {
                    if at.elapsed() >= Duration::from_millis(220) {
                        this.dirty_at = None;
                        let _ = this.settings.save();
                        this.engine.send("apply");
                    }
                }
                let (rev, status, devices_rev, devices, profiles_rev, profiles) = {
                    let state = match this.shared.lock() {
                        Ok(state) => state,
                        Err(_) => return,
                    };
                    (
                        state.rev,
                        state.status.clone(),
                        state.devices_rev,
                        state.devices.clone().unwrap_or_default(),
                        state.profiles_rev,
                        state.profiles.clone(),
                    )
                };
                if rev != this.last_rev
                    || devices_rev != this.last_devices_rev
                    || profiles_rev != this.profiles_rev
                {
                    this.last_rev = rev;
                    this.last_devices_rev = devices_rev;
                    this.profiles_rev = profiles_rev;
                    this.status = status;
                    this.devices = devices;
                    this.profiles = profiles;
                    cx.notify();
                }
            });
            if result.is_err() {
                break;
            }
        })
        .detach();
        view
    }

    fn commit(&mut self, cx: &mut Context<Self>) {
        let _ = self.settings.save();
        self.engine.send("apply");
        cx.notify();
    }

    fn toggle_row(
        &self,
        key: &'static str,
        label: &'static str,
        hint: &'static str,
        cx: &mut Context<Self>,
    ) -> AnyElement {
        let on = self.settings.b(key, false);
        widgets::row(
            label,
            hint,
            widgets::switch(
                SharedString::from(format!("ov-{key}")),
                on,
                cx.listener(
                    move |this, _: &gpui::ClickEvent, _: &mut Window, cx: &mut Context<Self>| {
                        this.settings.flip(key);
                        this.commit(cx);
                    },
                ),
            ),
        )
    }

    fn stepper_row(
        &self,
        key: &'static str,
        label: &'static str,
        hint: &'static str,
        min: i64,
        max: i64,
        step: i64,
        format: fn(i64) -> String,
        cx: &mut Context<Self>,
    ) -> AnyElement {
        let value = self.settings.i(key, min);
        let minus = div()
            .id(SharedString::from(format!("ov-{key}-m")))
            .flex()
            .items_center()
            .justify_center()
            .w(px(26.0))
            .h(px(26.0))
            .rounded(px(6.0))
            .bg(rgba(0xFFFFFF0F))
            .text_color(rgb(theme::TEXT_DIM))
            .cursor_pointer()
            .hover(|s| s.bg(rgba(0xFFFFFF1F)))
            .on_click(
                cx.listener(
                    move |this, _: &gpui::ClickEvent, _: &mut Window, cx: &mut Context<Self>| {
                        this.settings.bump(key, -step, min, max, min);
                        this.commit(cx);
                    },
                ),
            )
            .child("\u{2212}");
        let plus = div()
            .id(SharedString::from(format!("ov-{key}-p")))
            .flex()
            .items_center()
            .justify_center()
            .w(px(26.0))
            .h(px(26.0))
            .rounded(px(6.0))
            .bg(rgba(0xFFFFFF0F))
            .text_color(rgb(theme::TEXT_DIM))
            .cursor_pointer()
            .hover(|s| s.bg(rgba(0xFFFFFF1F)))
            .on_click(
                cx.listener(
                    move |this, _: &gpui::ClickEvent, _: &mut Window, cx: &mut Context<Self>| {
                        this.settings.bump(key, step, min, max, min);
                        this.commit(cx);
                    },
                ),
            )
            .child("+");
        let control = div()
            .flex()
            .items_center()
            .gap(px(2.0))
            .p(px(3.0))
            .rounded(px(9.0))
            .bg(rgba(0x00000040))
            .border_1()
            .border_color(rgba(0xFFFFFF10))
            .child(minus)
            .child(
                div()
                    .min_w(px(74.0))
                    .h(px(26.0))
                    .flex()
                    .items_center()
                    .justify_center()
                    .font_family(theme::MONO_FONT)
                    .text_size(theme::mono())
                    .text_color(theme::text())
                    .child(format(value)),
            )
            .child(plus);
        widgets::row(label, hint, control.into_any_element())
    }

    fn neural_range(&self, key: &str) -> (i64, i64) {
        let unlock = self.settings.b("ai.neural_unlock", false);
        match key {
            "ai.neural_intensity" | "ai.neural_tone" | "ai.neural_structure"
            | "ai.neural_skin" => {
                if unlock {
                    (0, 9999)
                } else {
                    (0, 200)
                }
            }
            "ai.neural_passes" => {
                if unlock {
                    (1, 9999)
                } else {
                    (1, 20)
                }
            }
            "ai.neural_scale" => {
                if unlock {
                    (1, 9999)
                } else {
                    (50, 100)
                }
            }
            _ => (0, 9999),
        }
    }

    fn slider_row(
        &self,
        key: &'static str,
        label: &'static str,
        hint: &'static str,
        suffix: &'static str,
        cx: &mut Context<Self>,
    ) -> AnyElement {
        let (min, max) = self.neural_range(key);
        let value = self.settings.i(key, min).clamp(min, max);
        let editing = self.num_edit == Some(key);
        let shown = if editing {
            format!("{}\u{258F}", self.num_text)
        } else {
            format!("{value}{suffix}")
        };
        let weak = cx.entity().downgrade();
        let slider = widgets::slider(key, value, min, max, 168.0, move |next, _window, cx| {
            let _ = weak.update(cx, |this, cx| {
                this.settings.set_int(key, next);
                this.dirty_at = Some(std::time::Instant::now());
                cx.notify();
            });
        });
        let number = div()
            .id(SharedString::from(format!("ov-num-{key}")))
            .flex()
            .items_center()
            .justify_center()
            .min_w(px(64.0))
            .h(px(26.0))
            .px(px(7.0))
            .rounded(px(7.0))
            .bg(if editing {
                theme::accent_rgba(0x26)
            } else {
                rgba(0x00000040)
            })
            .border_1()
            .border_color(if editing {
                theme::accent_rgba(0xCC)
            } else {
                rgba(0xFFFFFF10)
            })
            .font_family(theme::MONO_FONT)
            .text_size(theme::mono())
            .text_color(theme::text())
            .cursor_text()
            .on_click(
                cx.listener(move |this, _: &gpui::ClickEvent, window, cx| {
                    this.num_edit = Some(key);
                    this.num_text = this.settings.i(key, min).to_string();
                    this.num_fresh = true;
                    window.focus(&this.focus);
                    cx.notify();
                }),
            )
            .child(shown);
        let control = div()
            .flex()
            .items_center()
            .gap(px(8.0))
            .child(slider)
            .child(number);
        // Stacked, not side by side: at the smaller panel scales this row is
        // read in a 330 px card, and a 168 px slider plus the value box left
        // the label column about one character wide - text wrapped letter
        // under letter.
        div()
            .flex()
            .flex_col()
            .gap(px(7.0))
            .py(px(11.0))
            .child(
                div()
                    .flex()
                    .flex_col()
                    .gap(px(3.0))
                    .child(
                        div()
                            .text_size(theme::body())
                            .font_weight(FontWeight::MEDIUM)
                            .text_color(theme::text())
                            .child(label),
                    )
                    .child(
                        div()
                            .text_size(theme::small())
                            .text_color(theme::muted())
                            .child(hint),
                    ),
            )
            .child(control)
            .into_any_element()
    }

    fn unlock_box(&self, cx: &mut Context<Self>) -> AnyElement {
        let on = self.settings.b("ai.neural_unlock", false);
        div()
            .flex()
            .flex_col()
            .gap(px(6.0))
            .p(px(12.0))
            .rounded(px(10.0))
            .bg(rgba(0xF8717112))
            .border_1()
            .border_color(rgba(0xF8717140))
            .child(
                div()
                    .flex()
                    .items_center()
                    .gap(px(8.0))
                    .child(
                        div()
                            .text_size(theme::tiny())
                            .font_weight(FontWeight::SEMIBOLD)
                            .text_color(rgb(theme::RED))
                            .child("USE AT YOUR OWN RISK"),
                    )
                    .child(div().flex_1())
                    .child(widgets::switch(
                        "ov-ai.neural_unlock",
                        on,
                        cx.listener(
                            |this, _: &gpui::ClickEvent, _: &mut Window, cx: &mut Context<Self>| {
                                this.settings.flip("ai.neural_unlock");
                                this.commit(cx);
                            },
                        ),
                    )),
            )
            .child(
                div()
                    .text_size(theme::tiny())
                    .text_color(theme::dim())
                    .child("Lifts every neural limit to a hard 9999."),
            )
            .into_any_element()
    }

    fn pill(
        label: &'static str,
        selected: bool,
        key: &'static str,
        value: serde_json::Value,
        cx: &mut Context<Self>,
    ) -> AnyElement {
        div()
            .id(SharedString::from(format!("ov-pill-{key}-{value}")))
            .flex()
            .items_center()
            .justify_center()
            .h(px(26.0))
            .px(px(9.0))
            .rounded(px(6.0))
            .bg(if selected {
                theme::accent_rgba(0xFF)
            } else {
                rgba(0xFFFFFF0A)
            })
            .text_size(theme::tiny())
            .font_weight(if selected {
                FontWeight::SEMIBOLD
            } else {
                FontWeight::MEDIUM
            })
            .text_color(if selected { rgb(0xFFFFFF) } else { theme::dim() })
            .cursor_pointer()
            .hover(|s| s.bg(rgba(0xFFFFFF18)))
            .on_click(
                cx.listener(
                    move |this, _: &gpui::ClickEvent, _: &mut Window, cx: &mut Context<Self>| {
                        this.settings.set(key, value.clone());
                        this.commit(cx);
                    },
                ),
            )
            .child(label)
            .into_any_element()
    }

    fn seg_row(
        &self,
        key: &'static str,
        label: &'static str,
        hint: &'static str,
        options: &[(i64, &'static str)],
        cx: &mut Context<Self>,
    ) -> AnyElement {
        let current = self.settings.i(key, options[0].0);
        let mut row = div().flex().items_center().gap(px(3.0));
        for (value, text) in options {
            row = row.child(Self::pill(
                text,
                current == *value,
                key,
                serde_json::Value::from(*value),
                cx,
            ));
        }
        widgets::row(label, hint, row.into_any_element())
    }

    fn seg_str_row(
        &self,
        key: &'static str,
        label: &'static str,
        hint: &'static str,
        options: &[(&'static str, &'static str)],
        cx: &mut Context<Self>,
    ) -> AnyElement {
        let current = self.settings.s(key, options[0].1);
        let mut row = div().flex().items_center().gap(px(3.0));
        for (text, value) in options {
            row = row.child(Self::pill(
                text,
                current == *value,
                key,
                serde_json::Value::from(*value),
                cx,
            ));
        }
        widgets::row(label, hint, row.into_any_element())
    }

    fn footer_button(
        label: &'static str,
        tone: widgets::Tone,
        action: &'static str,
        cx: &mut Context<Self>,
    ) -> AnyElement {
        widgets::button(
            label,
            tone,
            cx.listener(
                move |this, _: &gpui::ClickEvent, window: &mut Window, cx: &mut Context<Self>| {
                    this.engine.send("overlay_close");
                    this.engine.send(action);
                    window.remove_window();
                    restore_focus_after(cx);
                },
            ),
        )
    }

    fn page(&self, cx: &mut Context<Self>) -> AnyElement {
        let mut rows: Vec<AnyElement> = Vec::new();
        match self.tab {
            0 => {
                let content = if self.status.content_fps > 1.0
                    && (self.status.fps - self.status.content_fps).abs() > 1.0
                {
                    format!(" ({:.0} content)", self.status.content_fps)
                } else {
                    String::new()
                };
                let input = format!(
                    "{}x{} @ {:.1} fps{}",
                    self.status.width, self.status.height, self.status.fps, content
                );
                let cadence = if self.status.cadence_p50 > 0.0 {
                    format!(
                        "{:.1} / {:.1} ms",
                        self.status.cadence_p50, self.status.cadence_p99
                    )
                } else {
                    "\u{2014}".to_string()
                };
                rows.push(widgets::kv("Input", input));
                rows.push(widgets::kv(
                    "Frame age",
                    format!("{:.1} ms", self.status.age_ms),
                ));
                rows.push(widgets::kv(
                    "Present lag",
                    format!("{:.1} ms", self.status.present_lag_ms),
                ));
                rows.push(widgets::kv("Decode", format!("{:.1} ms", self.status.decode_ms)));
                rows.push(widgets::kv("Cadence", cadence));
                rows.push(widgets::kv(
                    "Bitrate",
                    format!("{:.0} Mbps", self.status.bitrate_kbps / 1000.0),
                ));
                rows.push(widgets::kv("GPU", self.status.gpu.clone()));
                rows.push(widgets::kv(
                    "Super resolution",
                    if self.settings.b("ai.super_resolution", false) {
                        "DLSS"
                    } else {
                        "Off"
                    },
                ));
                rows.push(widgets::kv(
                    "Frame generation",
                    if self.settings.b("ai.frame_generation", false) {
                        format!("{}x", self.settings.i("ai.fg_multiplier", 2))
                    } else {
                        "Off".to_string()
                    },
                ));
                rows.push(widgets::kv(
                    "Neural render",
                    if self.status.nr_ready {
                        format!("on - {:.1} ms", self.status.nr_ms)
                    } else if self.settings.b("ai.neural_render", false) {
                        "starting".to_string()
                    } else {
                        "Off".to_string()
                    },
                ));
            }
            1 => {
                rows.push(self.toggle_row(
                    "ai.super_resolution",
                    "Super resolution",
                    "DLSS on the decoded frame",
                    cx,
                ));
                rows.push(self.stepper_row(
                    "ai.sr_quality",
                    "Quality",
                    "0 performance / 5 DLAA",
                    0,
                    5,
                    1,
                    |v| {
                        match v {
                            0 => "Performance",
                            1 => "Balanced",
                            2 => "Quality",
                            3 => "Ultra",
                            4 => "Max",
                            _ => "DLAA",
                        }
                        .to_string()
                    },
                    cx,
                ));
                rows.push(self.seg_str_row(
                    "ai.sr_output",
                    "Output",
                    "Reconstruction target",
                    &[("Native", "native"), ("1440p", "1440p"), ("4K", "2160p")],
                    cx,
                ));
                rows.push(self.seg_row(
                    "ai.dlss_preset",
                    "DLSS model",
                    "Auto / K / L / M / J",
                    &[(0, "Auto"), (1, "K"), (2, "L"), (3, "M"), (4, "J")],
                    cx,
                ));
            }
            2 => {
                rows.push(self.toggle_row(
                    "ai.frame_generation",
                    "Frame generation",
                    "Off for the lowest latency",
                    cx,
                ));
                rows.push(self.stepper_row(
                    "ai.fg_multiplier",
                    "Multiplier",
                    "2x = 1 held frame, 4x/6x = 3/5; 60 fps + 2x is lowest",
                    2,
                    6,
                    1,
                    |v| format!("{v}x"),
                    cx,
                ));
                rows.push(widgets::kv(
                    "Generated",
                    format!("{} frames", self.status.generated),
                ));
            }
            3 => {
                rows.push(self.toggle_row(
                    "ai.neural_render",
                    "Neural rendering",
                    "Run the network on every frame",
                    cx,
                ));
                rows.push(self.seg_row(
                    "ai.neural_style",
                    "Style",
                    "Network character",
                    &[(0, "Default"), (1, "Natural"), (2, "Cinematic")],
                    cx,
                ));
                rows.push(self.slider_row(
                    "ai.neural_intensity",
                    "Intensity",
                    "Drag, or click the value to type and press Enter",
                    "%",
                    cx,
                ));
                rows.push(self.slider_row(
                    "ai.neural_tone",
                    "Local tone",
                    "Drag, or click the value to type and press Enter",
                    "%",
                    cx,
                ));
                rows.push(self.slider_row(
                    "ai.neural_structure",
                    "Local structure",
                    "Drag, or click the value to type and press Enter",
                    "%",
                    cx,
                ));
                rows.push(self.slider_row(
                    "ai.neural_skin",
                    "Skin structure",
                    "Drag, or click the value to type and press Enter",
                    "%",
                    cx,
                ));
                rows.push(self.slider_row(
                    "ai.neural_passes",
                    "Passes",
                    "Above 1 needs Boost (scale < 100%)",
                    "",
                    cx,
                ));
                rows.push(self.slider_row(
                    "ai.neural_scale",
                    "Work scale",
                    "Drag, or click the value to type and press Enter",
                    "%",
                    cx,
                ));
                rows.push(self.toggle_row(
                    "ai.neural_ui",
                    "UI correction",
                    "Protect HUD edges",
                    cx,
                ));
                rows.push(self.toggle_row(
                    "ai.neural_mask",
                    "Auto mask",
                    "Soft mask from the frame",
                    cx,
                ));
                rows.push(self.unlock_box(cx));
            }
            4 => {
                rows.push(self.stepper_row(
                    "overlay.scale",
                    "Panel scale",
                    "Size of this panel",
                    75,
                    200,
                    5,
                    |v| format!("{v}%"),
                    cx,
                ));
                rows.push(self.stepper_row(
                    "video.sharpening",
                    "Sharpening",
                    "Applied after scaling",
                    0,
                    100,
                    5,
                    |v| format!("{v}%"),
                    cx,
                ));
                rows.push(self.seg_row(
                    "overlay.side",
                    "Dock side",
                    "Where the compact card sits",
                    &[(0, "Left"), (1, "Right")],
                    cx,
                ));
                rows.push(self.toggle_row(
                    "video.vsync",
                    "VSync",
                    "Lock presentation to the display",
                    cx,
                ));
                rows.push(self.seg_row(
                    "video.reflex",
                    "NVIDIA Reflex",
                    "Parent presenter; DLSS-G worker not covered",
                    &[(0, "Off"), (1, "On"), (2, "Boost")],
                    cx,
                ));
            }
            5 => {
                rows.push(self.seg_row(
                    "streaming.fps",
                    "Source fps",
                    "60 is lowest latency; 30 adds a console frame. Restart required",
                    &[(30, "30"), (60, "60"), (120, "120")],
                    cx,
                ));
                rows.push(self.stepper_row(
                    "streaming.bitrate_kbps",
                    "Bitrate",
                    "Stream restart required",
                    10000,
                    200000,
                    5000,
                    |v| format!("{} Mbps", v / 1000),
                    cx,
                ));
                rows.push(self.seg_row(
                    "streaming.frame_buffer",
                    "Frame buffer",
                    "0 lowest; 1 adds ~2 frames at 30 fps",
                    &[(0, "0"), (1, "1"), (2, "2")],
                    cx,
                ));
            }
            6 => {
                rows.push(self.stepper_row(
                    "audio.volume",
                    "Volume",
                    "Stream mixer",
                    0,
                    100,
                    5,
                    |v| format!("{v}%"),
                    cx,
                ));
                rows.push(self.toggle_row("audio.mute", "Mute", "Silence the stream", cx));
                rows.push(widgets::kv(
                    "Device",
                    if self.devices.audio.is_empty() {
                        self.settings.s("audio.device", "default")
                    } else {
                        self.devices
                            .audio
                            .iter()
                            .find(|d| d.id == self.settings.s("audio.device", ""))
                            .map(|d| d.name.clone())
                            .unwrap_or_else(|| "System default".to_string())
                    },
                ));
            }
            7 => {
                rows.push(self.stepper_row(
                    "controller.rumble_strength",
                    "Rumble",
                    "0 disables the motors",
                    0,
                    5,
                    1,
                    |v| v.to_string(),
                    cx,
                ));
                rows.push(self.seg_row(
                    "controller.poll_hz",
                    "Poll rate",
                    "Bluetooth can sustain 1000 Hz",
                    &[(125, "125"), (250, "250"), (500, "500"), (1000, "1000")],
                    cx,
                ));
            }
            _ => {
                if self.profiles.is_empty() {
                    rows.push(widgets::row(
                        "Saved profiles",
                        "None yet",
                        widgets::chip("Empty", theme::TEXT_MUTED),
                    ));
                }
                for profile in &self.profiles {
                    let id = profile.id.clone();
                    let armed = self
                        .confirm_profile
                        .as_ref()
                        .is_some_and(|(current, at)| current == &id && at.elapsed() < Duration::from_secs(4));
                    let apply_id = id.clone();
                    let delete_click = id.clone();
                    let controls = div()
                        .flex()
                        .items_center()
                        .gap(px(6.0))
                        .child(widgets::button_with_id(
                            format!("overlay-profile-apply-{id}"),
                            "Apply",
                            widgets::Tone::Subtle,
                            cx.listener(
                                move |this, _: &gpui::ClickEvent, _: &mut Window, _: &mut Context<Self>| {
                                    this.engine.send(&format!("profile_apply {apply_id}"));
                                },
                            ),
                        ))
                        .child(widgets::button_with_id(
                            format!("overlay-profile-delete-{id}"),
                            if armed { "Confirm" } else { "Delete" },
                            if armed { widgets::Tone::Danger } else { widgets::Tone::Subtle },
                            cx.listener(
                                move |this, _: &gpui::ClickEvent, _: &mut Window, cx: &mut Context<Self>| {
                                    let now = std::time::Instant::now();
                                    if this.profile_delete_cooldown.is_some_and(|at| {
                                        now.duration_since(at) < Duration::from_millis(1500)
                                    }) {
                                        this.confirm_profile = None;
                                        cx.notify();
                                        return;
                                    }
                                    let armed = this.confirm_profile.as_ref().is_some_and(
                                        |(current, at)| current == &delete_click
                                            && now.duration_since(*at) < Duration::from_secs(4),
                                    );
                                    if armed {
                                        this.confirm_profile = None;
                                        this.profile_delete_cooldown = Some(now);
                                        this.engine.send(&format!("profile_delete {delete_click}"));
                                    } else {
                                        this.confirm_profile = Some((delete_click.clone(), now));
                                    }
                                    cx.notify();
                                },
                            ),
                        ));
                    rows.push(widgets::row(
                        profile.name.clone(),
                        if profile.is_default { "Default" } else { "Saved configuration" },
                        controls.into_any_element(),
                    ));
                }
                let at_limit = self.profiles.len() >= 12;
                rows.push(widgets::row(
                    "Current settings",
                    if at_limit {
                        "Limit reached - delete one to make room"
                    } else {
                        "Save as a named profile"
                    },
                    widgets::button_with_id(
                        "overlay-profile-save",
                        if at_limit { "Limit reached" } else { "Save current" },
                        if at_limit { widgets::Tone::Subtle } else { widgets::Tone::Ghost },
                        cx.listener(
                            move |this, _: &gpui::ClickEvent, _: &mut Window, _: &mut Context<Self>| {
                                this.engine.send("profile_save");
                            },
                        ),
                    ),
                ));
            }
        }

        let mut card = widgets::card_pad().flex().flex_col();
        for (index, row) in rows.into_iter().enumerate() {
            if index > 0 {
                card = card.child(widgets::divider());
            }
            card = card.child(row);
        }
        card.into_any_element()
    }

    fn close(&self, window: &mut Window, cx: &mut Context<Self>) {
        self.engine.send("overlay_close");
        window.remove_window();
        restore_focus_after(cx);
    }
}

impl Focusable for OverlayView {
    fn focus_handle(&self, _: &gpui::App) -> FocusHandle {
        self.focus.clone()
    }
}

impl Render for OverlayView {
    fn render(&mut self, _window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {

        let live = self.status.streaming;
        let armed = self.status.armed;
        let mut tabs = div().flex().items_center().gap(px(2.0));
        for (index, name) in TABS.iter().enumerate() {
            let selected = self.tab == index;
            tabs = tabs.child(
                div()
                    .id(SharedString::from(format!("ov-tab-{index}")))
                    .flex()
                    .items_center()
                    .h(px(28.0))
                    .px(px(9.0))
                    .rounded(px(7.0))
                    .bg(if selected {
                        theme::accent_rgba(0x26)
                    } else {
                        rgba(0x00000000)
                    })
                    .text_size(theme::tiny())
                    .font_weight(if selected {
                        FontWeight::SEMIBOLD
                    } else {
                        FontWeight::MEDIUM
                    })
                    .text_color(if selected { theme::text() } else { theme::dim() })
                    .cursor_pointer()
                    .on_click(cx.listener(
                        move |this, _: &gpui::ClickEvent, _: &mut Window, cx: &mut Context<Self>| {
                            this.tab = index;
                            OVERLAY_TAB.store(index, Ordering::Relaxed);
                            cx.notify();
                        },
                    ))
                    .child(*name),
            );
        }
        let page = self.page(cx);

        div()
            .relative()
            .size_full()
            .rounded(px(16.0))
            .border_1()
            .border_color(theme::accent_rgba(0x3D))
            .bg(rgba(0x0B0B14F7))
            .font_family(theme::UI_FONT)
            .text_size(theme::body())
            .text_color(theme::text())
            .track_focus(&self.focus)
            .on_key_down(cx.listener(
                |this, event: &gpui::KeyDownEvent, window, cx| {
                    if let Some(key) = this.num_edit {
                        match event.keystroke.key.as_str() {
                            "escape" => {
                                this.num_edit = None;
                                cx.notify();
                                return;
                            }
                            "enter" => {
                                if let Ok(parsed) = this.num_text.trim().parse::<i64>() {
                                    let (min, max) = this.neural_range(key);
                                    this.settings.set_int(key, parsed.clamp(min, max));
                                    this.commit(cx);
                                }
                                this.num_edit = None;
                                cx.notify();
                                return;
                            }
                            "backspace" => {
                                if this.num_fresh {
                                    this.num_text.clear();
                                    this.num_fresh = false;
                                } else {
                                    this.num_text.pop();
                                }
                                cx.notify();
                                return;
                            }
                            _ => {
                                if let Some(ch) = event.keystroke.key_char.as_ref() {
                                    if !ch.is_empty()
                                        && ch.chars().all(|c| c.is_ascii_digit())
                                        && this.num_text.len() < 4
                                    {
                                        if this.num_fresh {
                                            this.num_text.clear();
                                            this.num_fresh = false;
                                        }
                                        this.num_text.push_str(ch);
                                        cx.notify();
                                    }
                                }
                                return;
                            }
                        }
                    }
                    if event.keystroke.key == "escape" || event.keystroke.key == "f10" {
                        this.close(window, cx);
                    }
                },
            ))
            .child(
                div()
                    .flex()
                    .flex_col()
                    .h_full()
                    .child(
                        div()
                            .flex()
                            .items_center()
                            .justify_between()
                            .gap(px(10.0))
                            .px(px(16.0))
                            .pt(px(14.0))
                            .pb(px(10.0))
                            .child(
                                div()
                                    .flex()
                                    .items_center()
                                    .gap(px(8.0))
                                    .child(
                                        div()
                                            .text_size(theme::h3())
                                            .font_weight(FontWeight::SEMIBOLD)
                                            .child("Control center"),
                                    )
                                    .child(widgets::dot(if armed {
                                        theme::AMBER
                                    } else if live {
                                        theme::GREEN
                                    } else {
                                        theme::TEXT_MUTED
                                    }))
                                    .when(armed, |el| {
                                        el.child(widgets::chip("DISCONNECT ARMED", theme::AMBER))
                                    }),
                            )
                            .child(
                                div()
                                    .flex()
                                    .items_center()
                                    .gap(px(6.0))
                                    .child(
                                        div()
                                            .font_family(theme::MONO_FONT)
                                            .text_size(theme::tiny())
                                            .text_color(theme::muted())
                                            .child(format!(
                                                "{}x{} - {}",
                                                self.status.width, self.status.height, self.status.codec
                                            )),
                                    )
                                    .child(widgets::button(
                                        "Close",
                                        widgets::Tone::Subtle,
                                        cx.listener(|this, _: &gpui::ClickEvent, window, cx| {
                                            this.close(window, cx);
                                        }),
                                    )),
                            ),
                    )
                    .child(div().px(px(10.0)).child(tabs))
                    .child(
                        div()
                            .id("ov-scroll")
                            .flex_1()
                            .min_h(px(0.0))
                            .overflow_y_scroll()
                            .px(px(12.0))
                            .py(px(10.0))
                            .child(page),
                    )
                    .child(
                        div()
                            .flex()
                            .items_center()
                            .gap(px(8.0))
                            .px(px(14.0))
                            .py(px(12.0))
                            .border_t_1()
                            .border_color(rgba(0xFFFFFF0D))
                            .child(Self::footer_button(
                                "Leave stream",
                                widgets::Tone::Ghost,
                                "leave_watch",
                                cx,
                            ))
                            .child(Self::footer_button(
                                "Disconnect",
                                widgets::Tone::Danger,
                                "disconnect_now",
                                cx,
                            ))
                            .child(div().flex_1())
                            .child(
                                div()
                                    .text_size(theme::tiny())
                                    .text_color(theme::muted())
                                    .child("F10 / Esc closes"),
                            ),
                    ),
            )
    }
}

#[link(name = "user32")]
extern "system" {
    fn FindWindowW(class: *const u16, title: *const u16) -> isize;
    fn FindWindowExW(parent: isize, after: isize, class: *const u16, title: *const u16) -> isize;
    fn SetWindowPos(hwnd: isize, after: isize, x: i32, y: i32, cx: i32, cy: i32, flags: u32) -> i32;
    fn SetForegroundWindow(hwnd: isize) -> i32;
    fn SetFocus(hwnd: isize) -> isize;
    fn GetWindowThreadProcessId(hwnd: isize, pid: *mut u32) -> u32;
    fn GetCurrentThreadId() -> u32;
    fn AttachThreadInput(attach: u32, to: u32, enable: i32) -> i32;
    fn GetForegroundWindow() -> isize;
    fn GetCurrentProcessId() -> u32;
}

fn wide(text: &str) -> Vec<u16> {
    text.encode_utf16().chain(std::iter::once(0)).collect()
}

/// Puts the overlay above the borderless stream window. GPUI has no topmost
/// option, so this uses the HWND directly.
pub(crate) fn activate_by_title(title: &str) {
    let name = wide(title);
    unsafe {
        let hwnd = FindWindowW(std::ptr::null(), name.as_ptr());
        if hwnd == 0 {
            return;
        }
        let foreground = GetForegroundWindow();
        let mut foreground_pid = 0u32;
        let foreground_thread = if foreground != 0 {
            GetWindowThreadProcessId(foreground, &mut foreground_pid)
        } else {
            0
        };
        let this_thread = GetCurrentThreadId();
        if foreground_thread != 0 && foreground_thread != this_thread {
            AttachThreadInput(this_thread, foreground_thread, 1);
            SetForegroundWindow(hwnd);
            SetFocus(hwnd);
            AttachThreadInput(this_thread, foreground_thread, 0);
        } else {
            SetForegroundWindow(hwnd);
            SetFocus(hwnd);
        }
    }
}

/// Removes WS_EX_TOPMOST from our own window with this title. A topmost shell
/// stays above every other application, which is exactly what a launcher must
/// not do. The engine owns a hidden window with the same title, so match on pid.
pub(crate) fn clear_topmost(title: &str) {
    let name = wide(title);
    unsafe {
        let me = GetCurrentProcessId();
        let mut hwnd = FindWindowW(std::ptr::null(), name.as_ptr());
        while hwnd != 0 {
            let mut pid = 0u32;
            GetWindowThreadProcessId(hwnd, &mut pid);
            if pid == me {
                SetWindowPos(hwnd, -2, 0, 0, 0, 0, 0x0002 | 0x0001 | 0x0010);
                return;
            }
            hwnd = FindWindowExW(0, hwnd, std::ptr::null(), name.as_ptr());
        }
    }
}

pub(crate) fn make_topmost(title: &str) {
    let name = wide(title);
    unsafe {
        let hwnd = FindWindowW(std::ptr::null(), name.as_ptr());
        if hwnd != 0 {
            SetWindowPos(hwnd, -1, 0, 0, 0, 0, 0x0002 | 0x0001 | 0x0010); // NOMOVE|NOSIZE|NOACTIVATE
        }
    }
}

/// Returns focus to the stream window (or the shell) after the panel closes.
pub(crate) fn restore_focus_after<T: 'static>(cx: &mut Context<T>) {
    cx.spawn(async move |_, cx| {
        cx.background_executor()
            .timer(Duration::from_millis(140))
            .await;
        restore_focus_now();
    })
    .detach();
}

pub(crate) fn restore_focus_now() {
    let stream = wide("Veyla Stream");
    let shell = wide("Veyla");
    unsafe {
        let mut hwnd = FindWindowW(std::ptr::null(), stream.as_ptr());
        if hwnd == 0 {
            hwnd = FindWindowW(std::ptr::null(), shell.as_ptr());
        }
        if hwnd != 0 {
            SetForegroundWindow(hwnd);
        }
    }
}
