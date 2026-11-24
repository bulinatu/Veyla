//! Application shell: window chrome, top navigation, boot splash, page router.

use std::time::Duration;

use gpui::{
    div, hsla, point, prelude::*, px, relative, rgba, rgb, size, Animation, AnimationExt as _,
    AnyElement, AnyWindowHandle, App, Bounds, BoxShadow, Context, FocusHandle, Focusable,
    FontWeight, InteractiveElement, IntoElement, Render, SharedString, StatefulInteractiveElement,
    Window, WindowBackgroundAppearance, WindowBounds, WindowControlArea, WindowKind, WindowOptions,
};

use crate::model::{Preset, Settings, Status};
use crate::{background, theme, widgets};

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Tab {
    Home,
    Stream,
    Video,
    Audio,
    Pad,
    Upscaling,
    FrameGen,
    Neural,
    Overlay,
    Advanced,
}

impl Tab {
    pub const ALL: [Tab; 10] = [
        Tab::Home,
        Tab::Stream,
        Tab::Video,
        Tab::Audio,
        Tab::Pad,
        Tab::Upscaling,
        Tab::FrameGen,
        Tab::Neural,
        Tab::Overlay,
        Tab::Advanced,
    ];

    pub fn label(self) -> &'static str {
        match self {
            Tab::Home => "Home",
            Tab::Stream => "Stream",
            Tab::Video => "Video",
            Tab::Audio => "Audio",
            Tab::Pad => "Controller",
            Tab::Upscaling => "Upscaling",
            Tab::FrameGen => "Frame Gen",
            Tab::Neural => "Neural",
            Tab::Overlay => "Overlay",
            Tab::Advanced => "Advanced",
        }
    }

}

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum PickerKind {
    Audio,
    Monitor,
    Gpu,
}

pub struct Root {
    pub(crate) settings: Settings,
    pub(crate) tab: Tab,
    pub(crate) advanced: bool,
    pub(crate) booting: bool,
    pub(crate) focus: FocusHandle,
    pub(crate) editing: Option<&'static str>,
    pub(crate) status: Status,
    pub(crate) toast: Option<(SharedString, bool)>,
    pub(crate) engine: Option<crate::bridge::Engine>,
    pub(crate) last_rev: u64,
    pub(crate) engine_online: bool,
    pub(crate) shell_minimized: bool,
    pub(crate) picker: Option<PickerKind>,
    pub(crate) devices_rev: u64,
    pub(crate) profiles_rev: u64,
    pub(crate) devices_tick: u8,
    pub(crate) settings_mtime: Option<std::time::SystemTime>,
    pub(crate) num_edit: Option<&'static str>,
    pub(crate) num_text: String,
    pub(crate) num_fresh: bool,
    pub(crate) overlay_tab: usize,
    pub(crate) pending_optimize: bool,
    pub(crate) optimize_ticks: u8,
    pub(crate) overlay_handle: Option<AnyWindowHandle>,
    pub(crate) settings_rev: u64,
    applied_rev: u64,
    apply_ticks: u8,
    focus_initialized: bool,
    boot_timer_started: bool,
    start_min_done: bool,
}

pub(crate) fn log_panic(message: &str) {
    crate::model::log_line(&format!("PANIC: {message}"));
}

impl Root {
    pub fn new(cx: &mut Context<Self>) -> Self {
        crate::model::log_line(&format!(
            "root new autoconnect={}",
            std::env::var("BRP_UI_AUTOCONNECT").is_ok()
        ));
        // Never write on load: a parse failure must not replace the user file.
        let settings = Settings::load();
        let booting = settings.b("ui.boot_animation", true)
            && !settings.b("ui.reduce_motion", false);
        crate::model::log_line(&format!("root booting={booting}"));
        let advanced = settings.b("ui.advanced", false);
        let focus = cx.focus_handle();


        // Engine status poll: the bridge reader thread updates a shared
        // snapshot; the UI picks it up here at ~8 Hz.
        cx.spawn(async move |this, cx| {
            loop {
                cx.background_executor()
                    .timer(Duration::from_millis(120))
                    .await;
                let result = this.update(cx, |this, cx| {
                    // Slider drags only touch memory; write the file at the poll
                    // rate so a drag does not produce one write per pixel.
                    if this.settings.dirty && this.num_edit.is_none() {
                        let _ = this.settings.save();
                    }
                    let Some(engine) = this.engine.as_ref() else {
                        return;
                    };
                    let (rev, status, online, devices_rev, profiles_rev, events) = {
                        let mut state = match engine.shared.lock() {
                            Ok(state) => state,
                            Err(_) => return,
                        };
                        (
                            state.rev,
                            state.status.clone(),
                            state.online,
                            state.devices_rev,
                            state.profiles_rev,
                            std::mem::take(&mut state.events),
                        )
                    };
                    if let Some(handle) = this.overlay_handle {
                        if handle.update(cx, |_, _, _| {}).is_err() {
                            this.overlay_handle = None;
                            this.send("overlay_close");
                        }
                    }
                    for event in events {
                        if event == "overlay-toggle" {
                            this.toggle_overlay(cx);
                        }
                    }
                    // Debounced auto-apply: a settings change reaches the
                    // engine about 350 ms after the last edit.
                    if this.engine_online && this.settings_rev != this.applied_rev {
                        if this.apply_ticks == 0 {
                            this.apply_ticks = 3;
                        } else {
                            this.apply_ticks -= 1;
                            if this.apply_ticks == 0 {
                                this.applied_rev = this.settings_rev;
                                let _ = this.settings.save();
                                this.send("apply");
                            }
                        }
                    } else {
                        this.apply_ticks = 0;
                        this.applied_rev = this.settings_rev;
                    }
                    if this.picker.is_some() {
                        this.devices_tick += 1;
                        if this.devices_tick >= 16 {
                            this.devices_tick = 0;
                            this.send("devices");
                        }
                    } else {
                        this.devices_tick = 0;
                    }
                    let came_online = online && !this.engine_online;
                    let devices_changed = devices_rev != this.devices_rev;
                    this.devices_rev = devices_rev;
                    let profiles_changed = profiles_rev != this.profiles_rev;
                    this.profiles_rev = profiles_rev;
                    if this.pending_optimize {
                        this.optimize_ticks = this.optimize_ticks.saturating_add(1);
                        // Wait for the engine's first status snapshot: the DLSS
                        // capability flags live there, and optimizing before they
                        // arrive would disable AI on a capable machine.
                        let hardware_known = online && rev > 0 && devices_rev > 0;
                        if hardware_known && (devices_changed || this.optimize_ticks >= 12) {
                            this.pending_optimize = false;
                            // The capability flags live in this snapshot; the
                            // status assignment below happens later in the loop.
                            this.status = status.clone();
                            this.engine_online = online;
                            this.optimize_for_pc(cx);
                        } else if this.optimize_ticks >= 60 {
                            this.pending_optimize = false;
                            this.toast("Optimize: the engine did not report hardware", true, cx);
                        }
                    }
                    if devices_changed || profiles_changed {
                        cx.notify();
                    }
                    if let Some(mtime) = this.settings.file_mtime() {
                        if this.settings_mtime != Some(mtime) {
                            this.settings_mtime = Some(mtime);
                            this.settings = Settings::load();
                            cx.notify();
                        }
                    }
                    if rev != this.last_rev || online != this.engine_online {
                        this.last_rev = rev;
                        this.status = status;
                        this.engine_online = online;
                        if this.shell_minimized && !this.status.streaming {
                            this.shell_minimized = false;
                            crate::model::log_line("shell restore");
                            // A new game session starts the control center on
                            // its first tab again.
                            crate::overlay::reset_overlay_tab();
                            restore_shell_window();
                        }
                        if came_online {
                            this.send("devices");
                            this.send("profiles");
                        }
                        cx.notify();
                    }
                });
                if result.is_err() {
                    break;
                }
            }
        })
        .detach();

        if let Ok(kind) = std::env::var("BRP_UI_PICKER") {
            cx.spawn(async move |this, cx| {
                cx.background_executor()
                    .timer(Duration::from_millis(1800))
                    .await;
                let kind = match kind.as_str() {
                    "audio" => PickerKind::Audio,
                    "monitor" => PickerKind::Monitor,
                    _ => PickerKind::Gpu,
                };
                let _ = this.update(cx, |this, cx| this.open_picker(kind, cx));
            })
            .detach();
        }
        if std::env::var("BRP_UI_AUTOCONNECT").is_ok() {
            cx.spawn(async move |this, cx| {
                cx.background_executor()
                    .timer(Duration::from_millis(900))
                    .await;
                crate::model::log_line("autoconnect timer fired");
                let _ = this.update(cx, |this, cx| this.action_connect(cx));
            })
            .detach();
        }

        let initial_tab = match std::env::var("BRP_UI_TAB").unwrap_or_default().as_str() {
            "stream" => Tab::Stream,
            "video" => Tab::Video,
            "audio" => Tab::Audio,
            "pad" => Tab::Pad,
            "upscaling" => Tab::Upscaling,
            "framegen" => Tab::FrameGen,
            "neural" => Tab::Neural,
            "overlay" => Tab::Overlay,
            "advanced" => Tab::Advanced,
            _ => Tab::Home,
        };
        // First run on a new machine: probe the hardware once and pick a sane
        // profile. The result is stored in `system.auto_optimized` so it never
        // overrides the user again; the Advanced page has the on-demand button.
        if !settings.b("system.auto_optimized", false) {
            cx.spawn(async move |this, cx| {
                cx.background_executor()
                    .timer(Duration::from_millis(700))
                    .await;
                let _ = this.update(cx, |this, cx| this.start_optimize(cx));
            })
            .detach();
        }
        let settings_mtime = settings.file_mtime();
        Self {
            settings,
            tab: initial_tab,
            advanced,
            booting,
            focus,
            editing: None,
            status: Status::default(),
            toast: None,
            engine: None,
            last_rev: 0,
            engine_online: false,
            shell_minimized: false,
            picker: None,
            devices_rev: 0,
            profiles_rev: 0,
            devices_tick: 0,
            settings_mtime,
            num_edit: None,
            num_text: String::new(),
            num_fresh: false,
            overlay_tab: 0,
            pending_optimize: false,
            optimize_ticks: 0,
            overlay_handle: None,
            settings_rev: 0,
            applied_rev: 0,
            apply_ticks: 0,
            focus_initialized: false,
            boot_timer_started: false,
            start_min_done: false,
        }
    }

    fn ensure_engine(&mut self, cx: &mut Context<Self>) -> bool {
        crate::model::log_line("ensure_engine");
        if self.engine.is_some() {
            return true;
        }
        let Some(path) = crate::bridge::engine_path() else {
            crate::model::log_line("engine not found");
            self.toast(
                "Engine not found (brpnext-engine.exe next to the shell)",
                true,
                cx,
            );
            return false;
        };
        crate::model::log_line(&format!("spawning engine: {}", path.display()));
        match crate::bridge::Engine::start(&path) {
            Some(engine) => {
                crate::model::log_line("engine spawned");
                self.engine = Some(engine);
                true
            }
            None => {
                crate::model::log_line("engine spawn failed");
                self.toast("Could not start the stream engine", true, cx);
                false
            }
        }
    }

    fn send(&self, command: &str) {
        if let Some(engine) = self.engine.as_ref() {
            engine.send(command);
        }
    }

    pub(crate) fn action_connect(&mut self, cx: &mut Context<Self>) {
        if !self.ensure_engine(cx) {
            return;
        }
        self.send("apply");
        if self.status.streaming {
            self.send("stop_watch");
            return;
        }
        if self.status.connected {
            self.send("watch");
            self.toast("Entering stream", false, cx);
        } else {
            self.send("connect");
            self.toast("Connecting to console...", false, cx);
        }
    }

    pub(crate) fn action_disconnect(&mut self, cx: &mut Context<Self>) {
        if self.engine.is_none() {
            self.toast("Engine is not running", false, cx);
            return;
        }
        self.send("disconnect");
        crate::overlay::reset_overlay_tab();
        self.toast("Disconnecting", false, cx);
    }

    pub(crate) fn action_apply(&mut self, cx: &mut Context<Self>) {
        if self.ensure_engine(cx) {
            self.send("apply");
            self.toast("Settings applied", false, cx);
        }
    }

    /// Spawns the engine if needed and asks for a fresh device list. The poll
    /// loop runs `optimize_for_pc` as soon as the devices arrive.
    pub(crate) fn start_optimize(&mut self, cx: &mut Context<Self>) {
        if self.engine.is_none() && !self.ensure_engine(cx) {
            return;
        }
        self.send("devices");
        self.pending_optimize = true;
        self.optimize_ticks = 0;
        self.toast("Detecting hardware...", false, cx);
    }

    /// Picks transport, output and AI stages from the actual machine. This is
    /// the same routine for the first-run pass and the Advanced button, so both
    /// end at the same configuration.
    pub(crate) fn optimize_for_pc(&mut self, cx: &mut Context<Self>) {
        let devices = self.devices();
        let monitor = devices
            .monitors
            .iter()
            .find(|monitor| monitor.primary)
            .or_else(|| devices.monitors.first());
        let (width, height, hz) = monitor
            .map(|monitor| (monitor.width, monitor.height, monitor.hz))
            .unwrap_or((1920, 1080, 60));
        let gpu = if !self.status.gpu.is_empty() {
            self.status.gpu.clone()
        } else {
            devices.gpus.first().cloned().unwrap_or_default()
        };
        let nvidia = gpu.to_lowercase().contains("nvidia");
        let sr_ok = nvidia && self.status.dlss_sr_ready;
        let fg_ok = nvidia && self.status.dlss_fg_ready;
        let nr_ok = self.status.nr_runtime;
        let hd_plus = height >= 1300;

        let settings = &mut self.settings;
        // The console caps at 1080p; 60 fps halves the input interval.
        settings.set_str("streaming.input", "1080p");
        settings.set_int("streaming.fps", 60);
        // Buffer 0 measured 58 ms less frame age than the primed queue.
        settings.set_int("streaming.frame_buffer", 0);
        settings.set_int("streaming.bitrate_kbps", if hd_plus { 100000 } else { 60000 });
        settings.set_str("streaming.decoder", "auto");
        settings.set_str("streaming.codec", "hevc");
        settings.set_bool("streaming.hdr", false);

        let output = if height >= 2000 {
            "2160p"
        } else if hd_plus {
            "1440p"
        } else if height >= 900 {
            "1080p"
        } else {
            "720p"
        };
        settings.set_str("video.output", output);
        settings.set_int("video.render_target", 0);
        settings.set_bool("video.vsync", true);
        settings.set_bool("video.vrr", hz >= 100);
        settings.set_bool("display.vsync", true);
        settings.set_bool("display.vrr", hz >= 100);
        settings.set_bool("display.fullscreen_on_play", true);
        settings.set_str("display.refresh", "match");
        settings.set_int("controller.poll_hz", 1000);
        settings.set_int("audio.buffer_ms", 10);

        if fg_ok {
            settings.set_bool("ai.frame_generation", true);
            settings.set_str("ai.fg_backend", "dlss-fg");
            // 2x is the only cadence that stays clean at 60 fps on 240 Hz.
            settings.set_int("ai.fg_multiplier", 2);
            settings.set_bool("ai.super_resolution", sr_ok);
            if sr_ok {
                settings.set_str("ai.sr_model", "dlss");
                if hd_plus {
                    // SR 1440p fills the worker: NR must stay off there.
                    settings.set_str("ai.sr_output", "1440p");
                    settings.set_int("ai.sr_quality", 4);
                    settings.set_bool("ai.neural_render", false);
                } else {
                    settings.set_str("ai.sr_output", "native");
                    settings.set_int("ai.sr_quality", 5);
                    settings.set_bool("ai.neural_render", nr_ok);
                }
            } else {
                settings.set_bool("ai.super_resolution", false);
                settings.set_bool("ai.neural_render", nr_ok);
            }
        } else if sr_ok {
            settings.set_bool("ai.frame_generation", false);
            settings.set_bool("ai.super_resolution", true);
            settings.set_str("ai.sr_model", "dlss");
            settings.set_str("ai.sr_output", "native");
            settings.set_int("ai.sr_quality", 5);
            settings.set_bool("ai.neural_render", nr_ok);
        } else {
            settings.set_bool("ai.frame_generation", false);
            settings.set_bool("ai.super_resolution", false);
            settings.set_bool("ai.neural_render", false);
        }
        settings.set_bool("system.auto_optimized", true);
        self.persist();
        self.send("apply");

        let label = if gpu.is_empty() { "auto".to_string() } else { gpu };
        let ai = if fg_ok {
            "DLSS-G 2x"
        } else if sr_ok {
            "DLSS SR"
        } else {
            "no AI"
        };
        self.toast(
            format!(
                "Optimized: {} | {}x{} @ {} Hz | {}",
                label, width, height, hz, ai
            ),
            false,
            cx,
        );
    }

    /// Opens the expanded control center as a second, topmost GPUI window on
    /// the edge chosen in Overlay settings, or closes it if already open.
    pub(crate) fn toggle_overlay(&mut self, cx: &mut Context<Self>) {
        crate::model::log_line("toggle_overlay");
        if let Some(handle) = self.overlay_handle.take() {
            if handle
                .update(cx, |_, window, _| window.remove_window())
                .is_ok()
            {
                self.send("overlay_close");
                crate::overlay::restore_focus_after(cx);
                return;
            }
            // The window closed itself (Esc or deactivation); fall through and
            // open a fresh one instead of swallowing the toggle.
        }
        let Some(engine) = self.engine.clone() else {
            self.toast("Engine is not running", true, cx);
            return;
        };
        // Give the engine a moment to hide the DLSS-G worker before the window
        // appears; the worker's topmost presenter would otherwise steal focus.
        self.send("overlay_open");
        cx.spawn(async move |this, cx| {
            cx.background_executor()
                .timer(Duration::from_millis(160))
                .await;
            let _ = this.update(cx, |this, cx| this.open_overlay_window(cx));
        })
        .detach();
    }

    fn open_overlay_window(&mut self, cx: &mut Context<Self>) {
        let Some(engine) = self.engine.clone() else {
            return;
        };
        if self.overlay_handle.is_some() {
            return;
        }
        let scale = self.settings.i("overlay.scale", 125).clamp(75, 200) as f32 / 100.0;
        let width = 440.0 * scale;
        let display = cx.primary_display();
        let (x, y, w, h) = if let Some(display) = display {
            let bounds = display.bounds();
            (
                bounds.origin.x + bounds.size.width - px(width) - px(18.0),
                bounds.origin.y + px(18.0),
                px(width),
                bounds.size.height - px(36.0),
            )
        } else {
            (px(1000.0), px(30.0), px(width), px(800.0))
        };
        let options = WindowOptions {
            window_bounds: Some(WindowBounds::Windowed(Bounds::new(
                point(x, y),
                size(w, h),
            ))),
            titlebar: None,
            kind: WindowKind::PopUp,
            is_movable: false,
            is_resizable: false,
            is_minimizable: false,
            window_background: WindowBackgroundAppearance::Opaque,
            ..Default::default()
        };
        let handle = cx.open_window(options, |window, cx| {
            let view = cx.new(|cx| crate::overlay::OverlayView::new(engine.clone(), cx));
            window.set_window_title("BetterRemotePlay Overlay");
            let focus = view.read(cx).focus.clone();
            window.focus(&focus);
            view
        });
        match handle {
            Ok(handle) => {
                crate::model::log_line("overlay opened");
                self.overlay_handle = Some(handle.into());
                crate::overlay::make_topmost("BetterRemotePlay Overlay");
                let any: AnyWindowHandle = handle.into();
                cx.spawn(async move |_, cx| {
                    cx.background_executor()
                        .timer(Duration::from_millis(90))
                        .await;
                    let _ = any.update(cx, |_, window, _| window.activate_window());
                    crate::overlay::activate_by_title("BetterRemotePlay Overlay");
                })
                .detach();
            }
            Err(error) => {
                crate::model::log_line(&format!("overlay open failed: {error}"));
                self.send("overlay_close");
                self.toast(format!("Control center failed: {error}"), true, cx);
            }
        }
    }

    pub(crate) fn select(&mut self, tab: Tab) {
        self.tab = tab;
    }

    pub(crate) fn toast(&mut self, message: impl Into<SharedString>, error: bool, cx: &mut Context<Self>) {
        self.toast = Some((message.into(), error));
        cx.notify();
        cx.spawn(async move |this, cx| {
            cx.background_executor()
                .timer(Duration::from_millis(3200))
                .await;
            let _ = this.update(cx, |this, cx| {
                this.toast = None;
                cx.notify();
            });
        })
        .detach();
    }

    pub(crate) fn preset(&mut self, preset: Preset, cx: &mut Context<Self>) {
        self.settings.apply_preset(preset);
        self.persist();
        self.toast(format!("Profile applied: {}", preset.label()), false, cx);
    }

    fn top_bar(&mut self, window: &mut Window, cx: &mut Context<Self>) -> AnyElement {
        let width = window.viewport_size().width;
        let tight = width < px(1230.0);
        let mut tabs = div().flex().items_center().gap(px(2.0));
        for tab in Tab::ALL {
            if tab == Tab::Advanced && !self.advanced {
                continue;
            }
            let selected = self.tab == tab;
            let label = tab.label();
            tabs = tabs.child(
                div()
                    .id(("tab", tab as usize))
                    .flex()
                    .items_center()
                    .gap(px(7.0))
                    .h(px(34.0))
                    .px(px(if tight { 8.0 } else { 12.0 }))
                    .rounded(px(9.0))
                    .bg(if selected {
                        rgba(0x8B5CF626)
                    } else {
                        rgba(0x00000000)
                    })
                    .text_size(theme::small())
                    .font_weight(if selected {
                        FontWeight::SEMIBOLD
                    } else {
                        FontWeight::MEDIUM
                    })
                    .text_color(if selected { theme::text() } else { theme::dim() })
                    .cursor_pointer()
                    .hover(move |s| {
                        if selected {
                            s
                        } else {
                            s.bg(rgba(0xFFFFFF0D)).text_color(theme::text())
                        }
                    })
                    .on_click(cx.listener(move |this, _: &gpui::ClickEvent, _: &mut Window, cx: &mut Context<Self>| {
                        this.select(tab);
                        cx.notify();
                    }))
                    .child(label),
            );
        }

        let status_color = if self.status.streaming {
            theme::GREEN
        } else if self.status.connected {
            theme::AMBER
        } else {
            theme::TEXT_MUTED
        };
        let status_text: SharedString = self.status.headline().into();

        div()
            .h(px(60.0))
            .flex_none()
            .flex()
            .items_center()
            .gap(px(18.0))
            .px(px(18.0))
            .border_b_1()
            .border_color(rgba(0xFFFFFF0A))
            .bg(rgba(0x07070DD9))
            .child(
                div()
                    .flex()
                    .items_center()
                    .gap(px(11.0))
                    .cursor_move()
                    // HTCAPTION, not start_window_move(): the Windows platform
                    // layer in gpui 0.2 has no implementation for the latter, so
                    // the custom title bar could not move the window at all.
                    .window_control_area(WindowControlArea::Drag)
                    .child(widgets::logo_mark(30.0))
                    .when(!tight, |el| {
                        el.child(
                            div()
                                .flex()
                                .flex_col()
                                .gap(px(1.0))
                                .child(
                                    div()
                                        .text_size(px(14.0))
                                        .font_weight(FontWeight::SEMIBOLD)
                                        .text_color(theme::text())
                                        .child("BetterRemotePlay"),
                                )
                                .child(
                                    div()
                                        .text_size(px(10.5))
                                        .text_color(theme::muted())
                                        .child("Next"),
                                ),
                        )
                    }),
            )
            .child(tabs)
            // The empty middle of the bar is a second grab handle, so the
            // window can be dragged from anywhere that is not a control.
            .child(
                div()
                    .flex_1()
                    .h(px(60.0))
                    .cursor_move()
                    .window_control_area(WindowControlArea::Drag),
            )
            .child(
                div()
                    .flex()
                    .items_center()
                    .gap(px(7.0))
                    .h(px(28.0))
                    .px(px(10.0))
                    .rounded(px(14.0))
                    .bg(rgba(0xFFFFFF0A))
                    .border_1()
                    .border_color(rgba(0xFFFFFF12))
                    .child(widgets::dot(status_color))
                    .when(!tight, |el| {
                        el.child(
                            div()
                                .text_size(theme::tiny())
                                .text_color(theme::dim())
                                .child(status_text),
                        )
                    }),
            )
            .child(
                div()
                    .flex()
                    .items_center()
                    .gap(px(6.0))
                    .when(!tight, |el| {
                        el.child(
                            div()
                                .text_size(theme::tiny())
                                .text_color(theme::muted())
                                .child("Basic"),
                        )
                    })
                    .child(widgets::switch(
                        "advanced-toggle",
                        self.advanced,
                        cx.listener(move |this, _: &gpui::ClickEvent, _: &mut Window, cx: &mut Context<Self>| {
                            this.advanced = !this.advanced;
                            this.settings.set_bool("ui.advanced", this.advanced);
                            if !this.advanced && this.tab == Tab::Advanced {
                                this.tab = Tab::Home;
                            }
                            this.persist();
                            cx.notify();
                        }),
                    ))
                    .when(!tight, |el| {
                        el.child(
                            div()
                                .text_size(theme::tiny())
                                .text_color(if self.advanced { theme::accent() } else { theme::muted() })
                                .child("Advanced"),
                        )
                    }),
            )
            .child(div().w(px(8.0)))
            .child(widgets::button(
                if tight { "OK" } else { "Apply" },
                widgets::Tone::Subtle,
                cx.listener(|this, _: &gpui::ClickEvent, _: &mut Window, cx: &mut Context<Self>| {
                    this.action_apply(cx);
                }),
            ))
            .child(window_button("min", "\u{2013}", false, |window| window.minimize_window()))
            .child(window_button("max", "\u{25A2}", false, |window| window.zoom_window()))
            .child(window_button("close", "\u{2715}", true, |window| window.remove_window()))
            .into_any_element()
    }

    fn page(&mut self, cx: &mut Context<Self>) -> AnyElement {
        match self.tab {
            Tab::Home => self.page_home(cx),
            Tab::Stream => self.page_stream(cx),
            Tab::Video => self.page_video(cx),
            Tab::Audio => self.page_audio(cx),
            Tab::Pad => self.page_controller(cx),
            Tab::Upscaling => self.page_upscaling(cx),
            Tab::FrameGen => self.page_frame_gen(cx),
            Tab::Neural => self.page_neural(cx),
            Tab::Overlay => self.page_overlay(cx),
            Tab::Advanced => self.page_advanced(cx),
        }
    }

    pub(crate) fn devices(&self) -> crate::bridge::Devices {
        if let Some(engine) = self.engine.as_ref() {
            if let Ok(state) = engine.shared.lock() {
                if let Some(devices) = state.devices.as_ref() {
                    return devices.clone();
                }
            }
        }
        crate::bridge::Devices::default()
    }

    pub(crate) fn profiles(&self) -> Vec<crate::bridge::ProfileInfo> {
        if let Some(engine) = self.engine.as_ref() {
            if let Ok(state) = engine.shared.lock() {
                return state.profiles.clone();
            }
        }
        Vec::new()
    }

    pub(crate) fn apply_profile(&mut self, id: &str, cx: &mut Context<Self>) {
        self.send(&format!("profile_apply {id}"));
        self.toast("Profile applied", false, cx);
    }

    pub(crate) fn save_profile(&mut self, cx: &mut Context<Self>) {
        if self.engine.is_none() {
            self.toast("Engine is not running", true, cx);
            return;
        }
        let name = format!("Profile {}", self.profiles().len() + 1);
        self.send(&format!("profile_save {name}"));
        self.toast("Profile saved", false, cx);
    }

    pub(crate) fn audio_label(&self) -> String {
        let value = self.settings.s("audio.device", "default");
        if value.is_empty() || value == "default" {
            return "System default".to_string();
        }
        let devices = self.devices();
        devices
            .audio
            .iter()
            .find(|device| device.id == value)
            .map(|device| device.name.clone())
            .unwrap_or(value)
    }

    pub(crate) fn monitor_label(&self) -> String {
        let index = self.settings.i("display.monitor", 0);
        let devices = self.devices();
        devices
            .monitors
            .iter()
            .find(|monitor| monitor.index == index)
            .map(|monitor| {
                format!(
                    "{}  {}x{}  {} Hz",
                    monitor.name, monitor.width, monitor.height, monitor.hz
                )
            })
            .unwrap_or_else(|| format!("Display {}", index + 1))
    }

    pub(crate) fn gpu_label(&self) -> String {
        let index = self.settings.i("gpu.device", 0).max(0) as usize;
        let devices = self.devices();
        devices
            .gpus
            .get(index)
            .cloned()
            .unwrap_or_else(|| format!("GPU {index}"))
    }

    pub(crate) fn open_picker(&mut self, kind: PickerKind, cx: &mut Context<Self>) {
        if self.engine.is_none() {
            self.ensure_engine(cx);
        }
        self.send("devices");
        self.picker = Some(kind);
        cx.notify();
    }

    fn apply_pick(&mut self, kind: PickerKind, value: &str, cx: &mut Context<Self>) {
        match kind {
            PickerKind::Audio => self.settings.set_str("audio.device", value),
            PickerKind::Monitor => self
                .settings
                .set_int("display.monitor", value.parse().unwrap_or(0)),
            PickerKind::Gpu => self
                .settings
                .set_int("gpu.device", value.parse().unwrap_or(0)),
        }
        self.picker = None;
        self.persist();
        self.send("apply");
        cx.notify();
    }

    fn pick_row(
        &self,
        title: &str,
        detail: &str,
        value: &str,
        selected: bool,
        kind: PickerKind,
        cx: &mut Context<Self>,
    ) -> AnyElement {
        let owned = value.to_string();
        div()
            .id(SharedString::from(format!("pick-{title}-{value}")))
            .flex()
            .items_center()
            .gap(px(10.0))
            .p(px(10.0))
            .rounded(px(9.0))
            .bg(if selected {
                rgba(0x8B5CF626)
            } else {
                rgba(0xFFFFFF08)
            })
            .border_1()
            .border_color(if selected {
                rgba(0x8B5CF666)
            } else {
                rgba(0xFFFFFF0F)
            })
            .cursor_pointer()
            .hover(|s| s.bg(rgba(0xFFFFFF14)))
            .on_click(
                cx.listener(move |this, _: &gpui::ClickEvent, _: &mut Window, cx: &mut Context<Self>| {
                    this.apply_pick(kind, &owned, cx);
                }),
            )
            .child(widgets::dot(if selected { theme::GREEN } else { theme::TEXT_MUTED }))
            .child(
                div()
                    .flex()
                    .flex_col()
                    .gap(px(2.0))
                    .flex_1()
                    .min_w(px(0.0))
                    .child(
                        div()
                            .text_size(theme::small())
                            .font_weight(FontWeight::MEDIUM)
                            .text_color(theme::text())
                            .child(title.to_string()),
                    )
                    .when(!detail.is_empty(), |el| {
                        el.child(
                            div()
                                .text_size(theme::tiny())
                                .text_color(theme::muted())
                                .child(detail.to_string()),
                        )
                    }),
            )
            .when(selected, |el| el.child(widgets::chip("Selected", theme::GREEN)))
            .into_any_element()
    }

    fn picker_overlay(&mut self, cx: &mut Context<Self>) -> AnyElement {
        let Some(kind) = self.picker else {
            return div().into_any_element();
        };
        let devices = self.devices();
        let (title, hint) = match kind {
            PickerKind::Audio => ("Audio output", "WASAPI render endpoints"),
            PickerKind::Monitor => ("Display", "Where the stream window opens"),
            PickerKind::Gpu => ("GPU", "Adapter used by decode and AI"),
        };
        let mut rows = div().flex().flex_col().gap(px(5.0));
        match kind {
            PickerKind::Audio => {
                let current = self.settings.s("audio.device", "default");
                let selected = current.is_empty() || current == "default";
                rows = rows.child(self.pick_row(
                    "System default",
                    "Follows the Windows default device",
                    "default",
                    selected,
                    kind,
                    cx,
                ));
                for device in &devices.audio {
                    let detail = if device.is_default {
                        "Windows default endpoint"
                    } else {
                        ""
                    };
                    rows = rows.child(self.pick_row(
                        &device.name,
                        detail,
                        &device.id,
                        current == device.id,
                        kind,
                        cx,
                    ));
                }
                if devices.audio.is_empty() {
                    rows = rows.child(
                        div()
                            .text_size(theme::small())
                            .text_color(theme::muted())
                            .child("No endpoints reported yet. Is the engine running?"),
                    );
                }
            }
            PickerKind::Monitor => {
                let current = self.settings.i("display.monitor", 0);
                for monitor in &devices.monitors {
                    let detail = format!(
                        "{}x{}  /  {} Hz{}",
                        monitor.width,
                        monitor.height,
                        monitor.hz,
                        if monitor.primary { "  /  Primary" } else { "" }
                    );
                    rows = rows.child(self.pick_row(
                        &monitor.name,
                        &detail,
                        &monitor.index.to_string(),
                        current == monitor.index,
                        kind,
                        cx,
                    ));
                }
                if devices.monitors.is_empty() {
                    rows = rows.child(
                        div()
                            .text_size(theme::small())
                            .text_color(theme::muted())
                            .child("No displays reported yet."),
                    );
                }
            }
            PickerKind::Gpu => {
                let current = self.settings.i("gpu.device", 0);
                for (index, name) in devices.gpus.iter().enumerate() {
                    rows = rows.child(self.pick_row(
                        name,
                        &format!("Adapter {index}"),
                        &index.to_string(),
                        current == index as i64,
                        kind,
                        cx,
                    ));
                }
                if devices.gpus.is_empty() {
                    rows = rows.child(
                        div()
                            .text_size(theme::small())
                            .text_color(theme::muted())
                            .child("No adapters reported yet."),
                    );
                }
            }
        }

        div()
            .absolute()
            .size_full()
            .bg(rgba(0x050509C8))
            .flex()
            .items_center()
            .justify_center()
            .child(
                widgets::card_pad()
                    .w(px(560.0))
                    .max_h(px(560.0))
                    .flex()
                    .flex_col()
                    .gap(px(10.0))
                    .child(
                        div()
                            .flex()
                            .items_center()
                            .justify_between()
                            .child(
                                div()
                                    .flex()
                                    .flex_col()
                                    .gap(px(2.0))
                                    .child(
                                        div()
                                            .text_size(theme::h3())
                                            .font_weight(FontWeight::SEMIBOLD)
                                            .text_color(theme::text())
                                            .child(title),
                                    )
                                    .child(
                                        div()
                                            .text_size(theme::tiny())
                                            .text_color(theme::muted())
                                            .child(hint),
                                    ),
                            )
                            .child(widgets::button(
                                "Close",
                                widgets::Tone::Subtle,
                                cx.listener(|this, _: &gpui::ClickEvent, _: &mut Window, cx: &mut Context<Self>| {
                                    this.picker = None;
                                    cx.notify();
                                }),
                            )),
                    )
                    .child(
                        div()
                            .id("picker-scroll")
                            .flex()
                            .flex_col()
                            .gap(px(5.0))
                            .max_h(px(430.0))
                            .overflow_y_scroll()
                            .child(rows),
                    ),
            )
            .into_any_element()
    }

    fn connect_overlay(&self, cx: &mut Context<Self>) -> AnyElement {
        if !self.status.connecting {
            return div().into_any_element();
        }
        let message: SharedString = if self.status.message.is_empty() {
            "Connecting to the console".into()
        } else {
            self.status.message.clone().into()
        };
        let spinner = div()
            .relative()
            .w(px(190.0))
            .h(px(4.0))
            .rounded(px(2.0))
            .bg(rgba(0xFFFFFF14))
            .overflow_hidden()
            .child(
                div()
                    .absolute()
                    .top(px(0.0))
                    .w(px(64.0))
                    .h(px(4.0))
                    .rounded(px(2.0))
                    .bg(rgba(0x8B5CF6FF))
                    .with_animation(
                        "connect-slide",
                        Animation::new(Duration::from_millis(1100)).repeat(),
                        |el, t| el.left(px(-64.0 + 254.0 * t)),
                    ),
            );

        div()
            .absolute()
            .size_full()
            .bg(rgba(0x050509C2))
            .flex()
            .items_center()
            .justify_center()
            .child(
                widgets::card_pad()
                    .w(px(430.0))
                    .flex()
                    .flex_col()
                    .items_center()
                    .gap(px(14.0))
                    .child(spinner)
                    .child(
                        div()
                            .text_size(theme::h3())
                            .font_weight(FontWeight::SEMIBOLD)
                            .text_color(theme::text())
                            .child("Connecting"),
                    )
                    .child(
                        div()
                            .text_size(theme::small())
                            .text_color(theme::dim())
                            .child(message),
                    )
                    .child(
                        div()
                            .font_family(theme::MONO_FONT)
                            .text_size(theme::tiny())
                            .text_color(theme::muted())
                            .child(format!("state: {}", self.status.state)),
                    )
                    .child(widgets::button(
                        "Cancel",
                        widgets::Tone::Ghost,
                        cx.listener(|this, _: &gpui::ClickEvent, _: &mut Window, cx: &mut Context<Self>| {
                            this.action_disconnect(cx);
                        }),
                    )),
            )
            .into_any_element()
    }

    fn boot_screen(&self) -> AnyElement {
        div()
            .size_full()
            .bg(rgba(0x050509FF))
            .flex()
            .flex_col()
            .items_center()
            .justify_center()
            .gap(px(18.0))
            .child(widgets::logo_mark(76.0))
            .child(
                div()
                    .text_size(px(26.0))
                    .font_weight(FontWeight::SEMIBOLD)
                    .text_color(theme::text())
                    .child("BetterRemotePlay"),
            )
            .child(
                div()
                    .text_size(theme::small())
                    .text_color(theme::muted())
                    .child("Remote Play, refined"),
            )
            .child(
                div()
                    .mt(px(12.0))
                    .w(px(220.0))
                    .h(px(3.0))
                    .rounded(px(2.0))
                    .bg(rgba(0xFFFFFF12))
                    .overflow_hidden()
                    .child(
                        div()
                            .relative()
                            .h_full()
                            .rounded(px(2.0))
                            .bg(rgba(0x8B5CF6FF))
                            .with_animation(
                                "boot-bar",
                                Animation::new(Duration::from_millis(1350)),
                                |el, t| el.w(relative(0.05 + 0.95 * t)),
                            ),
                    ),
            )
            .into_any_element()
    }


    fn toast_overlay(&self) -> AnyElement {
        let Some((message, error)) = self.toast.clone() else {
            return div().into_any_element();
        };
        div()
            .absolute()
            .bottom(px(26.0))
            .left(px(0.0))
            .right(px(0.0))
            .flex()
            .justify_center()
            .child(
                div()
                    .flex()
                    .items_center()
                    .gap(px(9.0))
                    .h(px(40.0))
                    .px(px(16.0))
                    .rounded(px(10.0))
                    .bg(rgba(0x12121FFF))
                    .border_1()
                    .border_color(if error {
                        rgba(0xF8717166)
                    } else {
                        rgba(0x8B5CF666)
                    })
                    .shadow_lg()
                    .child(widgets::dot(if error { theme::RED } else { theme::GREEN }))
                    .child(
                        div()
                            .text_size(theme::small())
                            .text_color(theme::text())
                            .child(message),
                    ),
            )
            .into_any_element()
    }
}

#[link(name = "user32")]
extern "system" {
    fn FindWindowW(class: *const u16, title: *const u16) -> isize;
    fn ShowWindow(hwnd: isize, command: i32) -> i32;
    fn SetForegroundWindow(hwnd: isize) -> i32;
}

/// Restores the shell after a stream ends. The window is minimized while the
/// stream owns the screen, so GPUI will not render and cannot restore itself.
pub(crate) fn restore_shell_window() {
    let title: Vec<u16> = "BetterRemotePlay\0".encode_utf16().collect();
    unsafe {
        let hwnd = FindWindowW(std::ptr::null(), title.as_ptr());
        if hwnd != 0 {
            ShowWindow(hwnd, 9); // SW_RESTORE
            SetForegroundWindow(hwnd);
        }
    }
}

fn window_button(
    element_id: &'static str,
    glyph: &'static str,
    danger: bool,
    action: impl Fn(&mut Window) + 'static,
) -> AnyElement {
    div()
        .id(element_id)
        .flex()
        .items_center()
        .justify_center()
        .w(px(34.0))
        .h(px(28.0))
        .rounded(px(7.0))
        .text_size(px(12.0))
        .text_color(theme::muted())
        .cursor_pointer()
        .hover(move |s| {
            if danger {
                s.bg(rgba(0xF8717133)).text_color(rgb(theme::RED))
            } else {
                s.bg(rgba(0xFFFFFF14)).text_color(rgb(theme::TEXT))
            }
        })
        .on_click(move |_: &gpui::ClickEvent, window: &mut Window, _: &mut App| action(window))
        .child(glyph)
        .into_any_element()
}

impl Focusable for Root {
    fn focus_handle(&self, _: &App) -> FocusHandle {
        self.focus.clone()
    }
}

impl Render for Root {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> AnyElement {
        // The stream window is the only surface that should drive the GPU
        // while playing. Minimizing the shell removes a second animating
        // layer from DWM composition, which is exactly the kind of hidden
        // jitter that shows up as the last few percent of judder.
        if !self.focus_initialized {
            self.focus_initialized = true;
            window.focus(&self.focus);
        }
        if self.status.streaming && !self.shell_minimized {
            self.shell_minimized = true;
            self.booting = false;
            crate::model::log_line("shell minimize");
            window.minimize_window();
        }
        if self.booting {
            if !self.boot_timer_started {
                self.boot_timer_started = true;
                crate::model::log_line("boot timer started");
                cx.spawn(async move |this, cx| {
                    cx.background_executor()
                        .timer(Duration::from_millis(1600))
                        .await;
                    crate::model::log_line("boot timer fired");
                    let _ = this.update(cx, |this, cx| {
                        this.booting = false;
                        cx.notify();
                    });
                })
                .detach();
            }
            return self.boot_screen();
        }
        theme::set_text_scale(self.settings.i("ui.scale", 100));
        if !self.start_min_done {
            self.start_min_done = true;
            if self.settings.b("general.start_minimized", false) {
                window.minimize_window();
            }
        }
        let animated = self.settings.b("ui.animated_background", true);
        let reduce = self.settings.b("ui.reduce_motion", false);
        let top = self.top_bar(window, cx);
        let page = self.page(cx);
        let toast = self.toast_overlay();
        let connecting = self.connect_overlay(cx);
        let picker = self.picker_overlay(cx);

        div()
            .relative()
            .size_full()
            .overflow_hidden()
            .font_family(theme::UI_FONT)
            .text_size(theme::body())
            .text_color(theme::text())
            .on_key_down(cx.listener(|this, event, window, cx| this.on_key(event, window, cx)))
            .child(background::aurora(animated, reduce))
            .child(
                div()
                    .relative()
                    .size_full()
                    .flex()
                    .flex_col()
                    .child(top)
                    .child(
                        div()
                            .id("page-scroll")
                            .flex_1()
                            .min_h(px(0.0))
                            .overflow_y_scroll()
                            // Focus lives on the page, not on the whole
                            // window: track_focus() registers a mouse-down
                            // handler that calls prevent_default(), and on
                            // Windows that silently cancels the HTCAPTION
                            // drag of the custom title bar.
                            .track_focus(&self.focus)
                            .child(
                                div()
                                    .w_full()
                                    .max_w(px(1220.0))
                                    .mx_auto()
                                    .px(px(18.0))
                                    .pt(px(16.0))
                                    .pb(px(40.0))
                                    .child(
                                        // The page floats directly on the
                                        // aurora. The old opaque slab cut a hard
                                        // rectangle out of the animation; what
                                        // is left is a gaussian rim only — no
                                        // fill, no border, no seam — so the text
                                        // keeps its contrast while the circles
                                        // keep moving underneath.
                                        div()
                                            .relative()
                                            .rounded(px(26.0))
                                            .shadow(vec![
                                                BoxShadow {
                                                    color: hsla(0.0, 0.0, 0.0, 0.13),
                                                    offset: point(px(0.0), px(18.0)),
                                                    blur_radius: px(180.0),
                                                    spread_radius: px(-34.0),
                                                },
                                                BoxShadow {
                                                    color: hsla(0.72, 0.70, 0.62, 0.04),
                                                    offset: point(px(0.0), px(0.0)),
                                                    blur_radius: px(140.0),
                                                    spread_radius: px(-46.0),
                                                },
                                            ])
                                            .p(px(22.0))
                                            .child(page),
                                    ),
                            ),
                    ),
            )
            .child(connecting)
            .child(picker)
            .child(toast)
            .into_any_element()
    }
}
