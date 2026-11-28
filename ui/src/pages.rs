//! Settings pages. Every control writes the same flat settings keys the engine
//! already reads, so the new shell is a drop-in front-end.

use std::time::Duration;

use gpui::{
    div, prelude::*, px, relative, rgba, rgb, Animation, AnimationExt as _, AnyElement,
    Context, FontWeight, IntoElement, SharedString, Window,
};

use crate::app::{Root, SetupField};
use crate::model::Preset;
use crate::theme;
use crate::widgets::{self, Tone};

fn plain(value: i64) -> String {
    value.to_string()
}

fn pct(value: i64) -> String {
    format!("{value}%")
}

fn ms(value: i64) -> String {
    format!("{value} ms")
}

fn mbps(value: i64) -> String {
    format!("{:.0} Mbps", value as f64 / 1000.0)
}

fn lightbar(value: i64) -> String {
    match value {
        0 => "Off",
        1 => "Blue",
        2 => "Red",
        3 => "Green",
        4 => "Pink",
        5 => "Purple",
        6 => "Cyan",
        _ => "White",
    }
    .to_string()
}

fn group(title: &'static str, hint: &'static str, rows: Vec<AnyElement>) -> AnyElement {
    let mut card = widgets::card_pad().flex().flex_col();
    card = card.child(widgets::section(title, hint));
    for row in rows {
        card = card.child(widgets::divider()).child(row);
    }
    card.into_any_element()
}

const SETTINGS_PAGES: [(&str, &str); 6] = [
    ("Appearance", "Theme, type and motion"),
    ("Pairing", "Console discovery, PIN and PSN account"),
    ("Updates", "GitHub releases"),
    ("System", "Hardware, shell and pacing"),
    ("Diagnostics", "Logging, measurements and reset"),
    ("About", "What this build is"),
];

fn swatch(color: u32) -> AnyElement {
    div()
        .w(px(18.0))
        .h(px(18.0))
        .rounded(px(9.0))
        .bg(rgba((color << 8) | 0xFF))
        .into_any_element()
}

impl Root {
    /// First-run console setup: discovery, PIN pairing and the PSN account link.
    fn console_setup(&self, cx: &mut Context<Self>) -> AnyElement {
        let consoles = self.consoles();
        let mut discovered = div().flex().flex_col().gap(px(8.0));
        if self.status.discovering {
            discovered = discovered.child(
                div()
                    .text_size(theme::small())
                    .text_color(theme::muted())
                    .child("Searching the LAN for consoles..."),
            );
        } else if consoles.is_empty() {
            discovered = discovered.child(
                div()
                    .text_size(theme::small())
                    .text_color(theme::muted())
                    .child("No consoles found yet. Wake the PS5 and press Discover."),
            );
        } else {
            for console in consoles {
                let address = console.address.clone();
                let label = if console.name.is_empty() {
                    address.clone()
                } else {
                    console.name.clone()
                };
                discovered = discovered.child(
                    div()
                        .flex()
                        .items_center()
                        .justify_between()
                        .gap(px(12.0))
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
                                        .font_weight(FontWeight::SEMIBOLD)
                                        .text_color(theme::text())
                                        .child(label),
                                )
                                .child(
                                    div()
                                        .font_family(theme::MONO_FONT)
                                        .text_size(theme::tiny())
                                        .text_color(theme::muted())
                                        .child(address.clone()),
                                ),
                        )
                        .child(widgets::button_with_id(
                            format!("use-{address}"),
                            "Use",
                            Tone::Ghost,
                            cx.listener(
                                move |this, _: &gpui::ClickEvent, _: &mut Window, cx: &mut Context<Self>| {
                                    this.setup_use(&address, cx);
                                },
                            ),
                        )),
                );
            }
        }

        let pin_focused = self.setup_field == Some(SetupField::Pin);
        let pin_value: SharedString = if self.pin_text.is_empty() {
            "8-digit PIN".into()
        } else {
            self.pin_text.clone().into()
        };
        let pin_box = div()
            .id("pin-field")
            .flex()
            .items_center()
            .w(px(150.0))
            .h(px(38.0))
            .px(px(12.0))
            .rounded(px(9.0))
            .bg(rgba(0x00000059))
            .border_1()
            .border_color(if pin_focused {
                theme::accent_rgba(0xCC)
            } else {
                rgba(0xFFFFFF14)
            })
            .font_family(theme::MONO_FONT)
            .text_size(theme::body())
            .text_color(if self.pin_text.is_empty() {
                theme::muted()
            } else {
                theme::text()
            })
            .cursor_text()
            .on_click(cx.listener(
                |this, _: &gpui::ClickEvent, _: &mut Window, cx: &mut Context<Self>| {
                    this.setup_field = Some(SetupField::Pin);
                    cx.notify();
                },
            ))
            .child(pin_value);

        let psn_focused = self.setup_field == Some(SetupField::Psn);
        let psn_value: SharedString = if self.psn_redirect.is_empty() {
            "Paste redirect URL".into()
        } else {
            self.psn_redirect.clone().into()
        };
        let psn_box = div()
            .id("psn-redirect")
            .flex()
            .items_center()
            .w(px(330.0))
            .h(px(38.0))
            .px(px(12.0))
            .rounded(px(9.0))
            .bg(rgba(0x00000059))
            .border_1()
            .border_color(if psn_focused {
                theme::accent_rgba(0xCC)
            } else {
                rgba(0xFFFFFF14)
            })
            .text_size(theme::small())
            .text_color(if self.psn_redirect.is_empty() {
                theme::muted()
            } else {
                theme::text()
            })
            .cursor_text()
            .overflow_hidden()
            .on_click(cx.listener(
                |this, _: &gpui::ClickEvent, _: &mut Window, cx: &mut Context<Self>| {
                    this.setup_field = Some(SetupField::Psn);
                    cx.notify();
                },
            ))
            .child(psn_value);

        let psn_state: SharedString = if self.status.psn_id.is_empty() {
            "No PSN account linked".into()
        } else {
            format!("Linked: {}", self.status.psn_id).into()
        };

        widgets::card_pad()
            .flex()
            .flex_col()
            .gap(px(14.0))
            .child(
                div()
                    .flex()
                    .items_center()
                    .justify_between()
                    .gap(px(12.0))
                    .child(widgets::section("Set up a console", "Discovery, pairing, account"))
                    .child(widgets::button(
                        if self.status.discovering {
                            "Searching..."
                        } else {
                            "Discover"
                        },
                        Tone::Ghost,
                        cx.listener(
                            |this, _: &gpui::ClickEvent, _: &mut Window, cx: &mut Context<Self>| {
                                this.setup_discover(cx);
                            },
                        ),
                    )),
            )
            .child(discovered)
            .child(widgets::divider())
            .child(widgets::row(
                "Pairing PIN",
                "PS5: Settings > System > Remote Play > Link Device",
                div()
                    .flex()
                    .items_center()
                    .gap(px(8.0))
                    .child(pin_box)
                    .child(widgets::button(
                        "Pair",
                        Tone::Primary,
                        cx.listener(
                            |this, _: &gpui::ClickEvent, _: &mut Window, cx: &mut Context<Self>| {
                                this.setup_pair(cx);
                            },
                        ),
                    ))
                    .into_any_element(),
            ))
            .child(widgets::divider())
            .child(widgets::row(
                "PSN account",
                psn_state,
                widgets::button(
                    "Open login",
                    Tone::Ghost,
                    cx.listener(
                        |this, _: &gpui::ClickEvent, _: &mut Window, cx: &mut Context<Self>| {
                            this.setup_psn_open(cx);
                        },
                    ),
                ),
            ))
            .child(widgets::row(
                "Redirect URL",
                "Paste what the browser lands on, then Link",
                div()
                    .flex()
                    .items_center()
                    .gap(px(8.0))
                    .child(psn_box)
                    .child(widgets::button(
                        "Link",
                        Tone::Primary,
                        cx.listener(
                            |this, _: &gpui::ClickEvent, _: &mut Window, cx: &mut Context<Self>| {
                                this.setup_psn_complete(cx);
                            },
                        ),
                    ))
                    .into_any_element(),
            ))
            .into_any_element()
    }

    pub(crate) fn page_home(&mut self, cx: &mut Context<Self>) -> AnyElement {
        let (session_name, session_host) = crate::model::session_identity();
        let name: SharedString = if !self.status.console_name.is_empty() &&
            self.status.console_name != "PS5"
        {
            self.status.console_name.clone().into()
        } else if let Some(session_name) = session_name {
            session_name.into()
        } else if !self.status.console_name.is_empty() {
            self.status.console_name.clone().into()
        } else {
            self.settings.s("console.name", "PS5").into()
        };
        let address: SharedString = {
            let a = if !self.status.console_address.is_empty() {
                self.status.console_address.clone()
            } else if let Some(host) = session_host {
                host
            } else {
                self.settings.s("console.address", "")
            };
            if a.is_empty() {
                "Not configured".into()
            } else {
                a.into()
            }
        };
        let streaming = self.status.streaming;
        let connected = self.status.connected;

        let source = if self.status.width > 0 {
            format!("{}x{}", self.status.width, self.status.height)
        } else {
            self.settings.s("streaming.input", "1080p")
        };
        let fps = if self.status.fps > 0.0 {
            if self.status.content_fps > 1.0 &&
                (self.status.fps - self.status.content_fps).abs() > 1.0
            {
                format!(
                    "{:.0} fps ({:.0} content)",
                    self.status.fps, self.status.content_fps
                )
            } else {
                format!("{:.0} fps", self.status.fps)
            }
        } else {
            format!("{} fps", self.settings.i("streaming.fps", 60))
        };

        let connect_label: SharedString = if connected || streaming {
            "\u{25B6}  Watch".into()
        } else {
            "\u{25B6}  Connect & Play".into()
        };
        let connect = div()
            .id("hero-connect")
            .flex()
            .items_center()
            .justify_center()
            .h(px(52.0))
            .px(px(28.0))
            .rounded(px(13.0))
            .bg(theme::accent_rgba(0xFF))
            .text_size(px(16.0))
            .font_weight(FontWeight::SEMIBOLD)
            .text_color(rgb(0xFFFFFF))
            .cursor_pointer()
            .shadow_lg()
            .hover(|s| s.bg(rgba(0x9D74FFFF)))
            .active(|s| s.opacity(0.85))
            .on_click(cx.listener(|this, _: &gpui::ClickEvent, _: &mut Window, cx: &mut Context<Self>| {
                this.action_connect(cx);
            }))
            .child(connect_label);

        let disconnect = widgets::button(
            "Disconnect",
            Tone::Ghost,
            cx.listener(|this, _: &gpui::ClickEvent, _: &mut Window, cx: &mut Context<Self>| {
                this.action_disconnect(cx);
            }),
        );

        // One click for an apples-to-apples comparison with a plain low-latency
        // client: 60 fps on the wire, no frame buffer, no vblank sync, no AI.
        let latency = widgets::button(
            "Lowest latency",
            Tone::Ghost,
            cx.listener(|this, _: &gpui::ClickEvent, _: &mut Window, cx: &mut Context<Self>| {
                this.settings.set_int("streaming.fps", 60);
                this.settings.set_int("streaming.bitrate_kbps", 50000);
                this.settings.set_int("streaming.frame_buffer", 0);
                this.settings.set_bool("video.vsync", false);
                this.settings.set_bool("ai.frame_generation", false);
                this.settings.set_bool("ai.super_resolution", false);
                this.settings.set_bool("ai.neural_render", false);
                this.persist();
                cx.notify();
            }),
        );

        let hero = widgets::card()
            .flex()
            .flex_col()
            .gap(px(16.0))
            .p(px(24.0))
            .child(
                div()
                    .flex()
                    .items_center()
                    .gap(px(9.0))
                    .child(widgets::dot(if streaming {
                        theme::GREEN
                    } else if connected {
                        theme::AMBER
                    } else {
                        theme::TEXT_MUTED
                    }))
                    .child(
                        div()
                            .text_size(px(24.0))
                            .font_weight(FontWeight::SEMIBOLD)
                            .text_color(theme::text())
                            .child(name),
                    )
                    .child(widgets::chip(
                        if streaming { "Live" } else { "Ready" },
                        if streaming { theme::GREEN } else { theme::accent_bright_u32() },
                    ))
                    .when(self.status.restart_pending, |el| {
                        el.child(widgets::chip("Stream restart required", theme::AMBER))
                    }),
            )
            .child(
                div()
                    .font_family(theme::MONO_FONT)
                    .text_size(theme::small())
                    .text_color(theme::dim())
                    .child(address),
            )
            .child(
                div()
                    .flex()
                    .items_center()
                    .gap(px(10.0))
                    .mt(px(4.0))
                    .child(connect)
                    .child(disconnect)
                    .child(latency),
            )
            .child(
                div()
                    .text_size(theme::tiny())
                    .text_color(theme::muted())
                    .child("Lowest latency: 60 fps, 50 Mbps, buffer 0, vsync off, AI off"),
            )
            .child(
                div()
                    .text_size(theme::tiny())
                    .text_color(theme::muted())
                    .child("Esc arms disconnect  -  F10 control center  -  Alt+F4 leave stream"),
            );

        let stats = div()
            .flex()
            .gap(px(12.0))
            .child(widgets::stat_tile("Source", source, theme::TEXT))
            .child(widgets::stat_tile("Frame rate", fps, theme::TEXT))
            .child(widgets::stat_tile(
                "Bitrate",
                if self.status.bitrate_kbps > 0.0 {
                    format!("{:.0} Mbps", self.status.bitrate_kbps / 1000.0)
                } else {
                    format!("{} Mbps", self.settings.i("streaming.bitrate_kbps", 50000) / 1000)
                },
                theme::TEXT,
            ))
            .child(widgets::stat_tile(
                "Latency",
                if self.status.connected || self.status.streaming {
                    format!(
                        "{:.1} + {:.1} ms",
                        self.status.age_ms, self.status.present_lag_ms
                    )
                } else {
                    "\u{2014}".to_string()
                },
                if self.status.age_ms + self.status.present_lag_ms > 12.0 {
                    theme::AMBER
                } else {
                    theme::GREEN
                },
            ));

        let presets = {
            let mut column = div().flex().flex_col().gap(px(8.0));
            let all = [
                Preset::LowLatency,
                Preset::Performance,
                Preset::Balanced,
                Preset::Cinematic,
            ];
            for pair in all.chunks(2) {
                let mut row = div().flex().gap(px(8.0));
                for &preset in pair {
                    row = row.child(
                        widgets::card()
                            .id(SharedString::from(format!("preset-{}", preset.label())))
                            .flex()
                            .flex_col()
                            .gap(px(4.0))
                            .flex_1()
                            .min_w(px(0.0))
                            .p(px(12.0))
                            .cursor_pointer()
                            .hover(|s| s.bg(rgba(0xFFFFFF16)).border_color(theme::accent_rgba(0x66)))
                            .on_click(cx.listener(move |this, _: &gpui::ClickEvent, _: &mut Window, cx: &mut Context<Self>| {
                                this.preset(preset, cx);                            }))
                            .child(
                                div()
                                    .text_size(theme::small())
                                    .font_weight(FontWeight::SEMIBOLD)
                                    .text_color(theme::text())
                                    .child(preset.label()),
                            )
                            .child(
                                div()
                                    .text_size(px(10.5))
                                    .text_color(theme::muted())
                                    .child(preset.hint()),
                            ),
                    );
                }
                column = column.child(row);
            }
            column
        };

        let profile_group = group("Profiles", "One click", vec![presets.into_any_element()]);

        let saved_profiles = self.profiles();
        let saved_count = saved_profiles.len();
        let at_limit = saved_count >= Root::PROFILE_LIMIT;
        let mut saved_rows = div()
            .id("saved-profiles")
            .flex()
            .flex_col()
            .gap(px(4.0))
            .when(saved_count > 5, |el| {
                // Cap the card: twelve rows would otherwise push Live and the
                // console card off the page.
                el.max_h(px(232.0)).overflow_y_scroll()
            });
        if saved_profiles.is_empty() {
            saved_rows = saved_rows.child(
                div()
                    .text_size(theme::small())
                    .text_color(theme::muted())
                    .child("No saved profiles yet"),
            );
        } else {
            for profile in saved_profiles {
                let id = profile.id.clone();
                let is_default = profile.is_default;
                let armed = self.profile_delete_armed(&id);
                let apply_id = format!("profile-apply-{id}");
                let delete_id = format!("profile-delete-{id}");
                let delete_id_click = id.clone();
                let apply_click = id.clone();
                saved_rows = saved_rows.child(
                    div()
                        .flex()
                        .items_center()
                        .gap(px(8.0))
                        .child(
                            div()
                                .flex()
                                .flex_1()
                                .min_w(px(0.0))
                                .items_center()
                                .gap(px(6.0))
                                .child(
                                    div()
                                        .text_size(theme::small())
                                        .text_color(theme::text())
                                        .child(profile.name),
                                )
                                .when(is_default, |el| {
                                    el.child(widgets::chip("Default", theme::accent_bright_u32()))
                                }),
                        )
                        .child(widgets::button_with_id(
                            apply_id,
                            "Apply",
                            Tone::Subtle,
                            cx.listener(
                                move |this, _: &gpui::ClickEvent, _: &mut Window, cx: &mut Context<Self>| {
                                    this.apply_profile(&apply_click, cx);
                                },
                            ),
                        ))
                        .child(widgets::button_with_id(
                            delete_id,
                            if armed { "Confirm" } else { "Delete" },
                            if armed { Tone::Danger } else { Tone::Subtle },
                            cx.listener(
                                move |this, _: &gpui::ClickEvent, _: &mut Window, cx: &mut Context<Self>| {
                                    this.request_delete_profile(&delete_id_click, cx);
                                },
                            ),
                        )),
                );
            }
        }
        let profiles_group = widgets::card_pad()
            .flex()
            .flex_col()
            .gap(px(12.0))
            .child(
                div()
                    .flex()
                    .items_center()
                    .justify_between()
                    .gap(px(14.0))
                    .child(
                        div()
                            .flex_1()
                            .min_w(px(0.0))
                            .child(widgets::section(
                                "Saved profiles",
                                if saved_count == 0 {
                                    "Engine configuration".to_string()
                                } else {
                                    format!(
                                        "Engine configuration · {} / {}",
                                        saved_count,
                                        Root::PROFILE_LIMIT
                                    )
                                },
                            )),
                    )
                    .child(widgets::button_with_id(
                        "profile-save-current",
                        if at_limit { "Limit reached" } else { "Save current" },
                        if at_limit { Tone::Subtle } else { Tone::Ghost },
                        cx.listener(
                            |this, _: &gpui::ClickEvent, _: &mut Window, cx: &mut Context<Self>| {
                                this.save_profile(cx);
                            },
                        ),
                    )),
            )
            .child(saved_rows);

        let live_group = group(
            "Live",
            "Engine telemetry",
            vec![
                widgets::kv(
                    "Codec",
                    if self.status.codec.is_empty() {
                        self.settings.s("streaming.codec", "auto")
                    } else {
                        self.status.codec.clone()
                    },
                ),
                widgets::kv(
                    "Frame cadence",
                    if self.status.cadence_p50 > 0.0 {
                        format!(
                            "{:.1} / {:.1} ms",
                            self.status.cadence_p50, self.status.cadence_p99
                        )
                    } else {
                        "\u{2014}".to_string()
                    },
                ),
                widgets::kv(
                    "Scanout",
                    if self.status.scanout_hz > 0.0 {
                        format!("{:.0} Hz", self.status.scanout_hz)
                    } else {
                        "\u{2014}".to_string()
                    },
                ),
                widgets::kv("Loss", format!("{:.2}%", self.status.loss_percent)),
                widgets::kv("Jitter", format!("{:.1} ms", self.status.jitter_ms)),
                widgets::kv("Drops", plain(self.status.drops)),
            ],
        );

        let footer = div()
            .flex()
            .items_center()
            .justify_between()
            .pt(px(4.0))
            .child(
                div()
                    .text_size(theme::tiny())
                    .text_color(theme::muted())
                    .child(format!("Veyla {}  -  original project: BetterRemotePlay", crate::model::APP_VERSION)),
            )
            .child(
                div()
                    .flex()
                    .items_center()
                    .gap(px(8.0))
                    .child(widgets::chip(
                        format!("{} fps", self.settings.s("streaming.fps", "60")),
                        theme::accent_bright_u32(),
                    ))
                    .child(widgets::chip(
                        format!("Buffer {}", self.settings.i("streaming.frame_buffer", 0)),
                        theme::cyan_u32(),
                    ))
                    .child(widgets::chip(
                        format!("{} Hz pad", self.settings.i("controller.poll_hz", 250)),
                        theme::pink_u32(),
                    ))
                    .when(!self.status.gpu.is_empty(), |el| {
                        el.child(widgets::chip(
                            self.status
                                .gpu
                                .chars()
                                .take(30)
                                .collect::<String>(),
                            theme::GREEN,
                        ))
                    }),
            );

        let update_banner = {
            let state = self.status.update_state.as_str();
            if !matches!(state, "available" | "downloading" | "ready") {
                div().into_any_element()
            } else {
                let progress = self.status.update_progress.clamp(0.0, 1.0) as f32;
                let (accent, headline, detail) = match state {
                    "ready" => (
                        theme::GREEN,
                        format!("v{} is ready to install", self.status.update_version),
                        "Install and restart to apply it".to_string(),
                    ),
                    "downloading" => (
                        theme::accent_bright_u32(),
                        format!("Downloading v{}", self.status.update_version),
                        format!("{:.0}%", progress * 100.0),
                    ),
                    _ => (
                        theme::AMBER,
                        format!("v{} is available", self.status.update_version),
                        self.status
                            .update_notes
                            .lines()
                            .next()
                            .unwrap_or("Download the updater when you are ready")
                            .to_string(),
                    ),
                };
                let mut action = div().flex().items_center().gap(px(8.0));
                if state == "available" {
                    action = action.child(widgets::button(
                        "Download",
                        Tone::Primary,
                        cx.listener(
                            |this, _: &gpui::ClickEvent, _: &mut Window, cx: &mut Context<Self>| {
                                if let Some(engine) = &this.engine {
                                    engine.send("download-update");
                                }
                                cx.notify();
                            },
                        ),
                    ));
                }
                if state == "ready" {
                    action = action.child(widgets::button(
                        "Install & restart",
                        Tone::Primary,
                        cx.listener(
                            |this, _: &gpui::ClickEvent, _: &mut Window, cx: &mut Context<Self>| {
                                if let Some(engine) = &this.engine {
                                    engine.send("install-update");
                                }
                                // Do not quit on the spot: the command travels
                                // over the bridge, and quitting first could drop
                                // it, leaving the user with no app and no update.
                                cx.spawn(async move |this, cx| {
                                    cx.background_executor()
                                        .timer(Duration::from_millis(1500))
                                        .await;
                                    let _ = this.update(cx, |_, cx| cx.quit());
                                })
                                .detach();
                            },
                        ),
                    ));
                }
                let mut card = widgets::card()
                    .flex()
                    .flex_col()
                    .gap(px(10.0))
                    .p(px(16.0))
                    .child(
                        div()
                            .flex()
                            .items_center()
                            .gap(px(14.0))
                            .child(update_orb(accent, false, 2))
                            .child(
                                div()
                                    .flex()
                                    .flex_col()
                                    .gap(px(3.0))
                                    .flex_1()
                                    .min_w(px(0.0))
                                    .overflow_hidden()
                                    .child(
                                        div()
                                            .text_size(theme::body())
                                            .font_weight(FontWeight::SEMIBOLD)
                                            .text_color(theme::text())
                                            .overflow_hidden()
                                            .child(SharedString::from(headline)),
                                    )
                                    .child(
                                        div()
                                            .text_size(theme::small())
                                            .text_color(theme::dim())
                                            .overflow_hidden()
                                            .child(SharedString::from(detail)),
                                    ),
                            )
                            .child(action),
                    );
                if state == "downloading" {
                    card = card.child(
                        div()
                            .w_full()
                            .h(px(5.0))
                            .rounded(px(3.0))
                            .bg(rgba(0xFFFFFF14))
                            .child(
                                div()
                                    .h_full()
                                    .w(relative(progress))
                                    .rounded(px(3.0))
                                    .bg(gpui::linear_gradient(
                                        90.0,
                                        gpui::linear_color_stop(
                                            rgba((theme::accent_u32() << 8) | 0xFF),
                                            0.0,
                                        ),
                                        gpui::linear_color_stop(
                                            rgba((theme::pink_u32() << 8) | 0xFF),
                                            1.0,
                                        ),
                                    )),
                            ),
                    );
                }
                card.into_any_element()
            }
        };

        div()
            .flex()
            .flex_col()
            .gap(px(20.0))
            .child(widgets::page_title(
                "Home",
                "",
            ))
            .child(
                div()
                    .flex()
                    .gap(px(16.0))
                    .child(div().flex_1().min_w(px(0.0)).child(hero))
                    .child(
                        div()
                            .w(px(330.0))
                            .flex()
                            .flex_col()
                            .gap(px(16.0))
                            .child(profile_group),
                    ),
            )
            .child(stats)
            .child(update_banner)
            .child(profiles_group)
            .child(live_group)
            .child(footer)
            .into_any_element()
    }

    pub(crate) fn page_stream(&self, cx: &mut Context<Self>) -> AnyElement {
        let console = group(
            "Console",
            "Pairing and reconnect",
            vec![
                self.text_row(
                    "console.name",
                    "Console name",
                    "Shown on Home and in the overlay",
                    "PS5",
                    220.0,
                    cx,
                ),
                self.text_row(
                    "console.address",
                    "Address",
                    "IPv4 address of the console",
                    "192.168.1.21",
                    220.0,
                    cx,
                ),
                self.toggle_row(
                    "console.auto_discover",
                    "Auto discovery",
                    "Find consoles on the local network",
                    cx,
                ),
                self.toggle_row(
                    "console.auto_reconnect",
                    "Auto reconnect",
                    "Reconnect when the link drops",
                    cx,
                ),
            ],
        );

        let streaming = group(
            "Streaming",
            "Transport and decode",
            vec![
                self.seg_int_row(
                    "streaming.fps",
                    "Frame rate",
                    "60 is the low-latency stream; 30 makes the console hold a frame",
                    &[(30, "30"), (60, "60"), (120, "120")],
                    cx,
                ),
                self.stepper_row(
                    "streaming.bitrate_kbps",
                    "Bitrate",
                    "Sharper, but above ~50 Mbps the link queues more",
                    10000,
                    200000,
                    5000,
                    mbps,
                    cx,
                ),
                self.seg_int_row(
                    "streaming.frame_buffer",
                    "Frame buffer",
                    "0 is lowest latency; 1+ adds one source interval per step and absorbs link jitter (auto-grows on drops)",
                    &[(0, "0"), (1, "1"), (2, "2"), (3, "3")],
                    cx,
                ),
            ],
        );

        let expert = group(
            "Expert",
            "Transport details",
            vec![
                self.seg_row(
                    "streaming.decoder",
                    "Decoder",
                    "D3D12VA uses the fixed-function engine",
                    &[
                        ("Auto", "auto"),
                        ("D3D11VA", "d3d11va"),
                        ("D3D12VA", "d3d12va"),
                        ("CPU", "software"),
                    ],
                    cx,
                ),
                self.stepper_row(
                    "console.reconnect_delay_ms",
                    "Reconnect delay",
                    "Wait before retrying after a drop",
                    250,
                    10000,
                    250,
                    ms,
                    cx,
                ),
            ],
        );

        div()
            .flex()
            .flex_col()
            .gap(px(20.0))
            .child(widgets::page_title(
                "Stream",
                "Console, transport and decode",
            ))
            .child(console)
            .child(streaming)
            .when(self.advanced, |el| el.child(expert))
            .into_any_element()
    }

    pub(crate) fn page_video(&self, cx: &mut Context<Self>) -> AnyElement {
        let output = group(
            "Output",
            "Presentation and scaling",
            vec![
                self.seg_row(
                    "video.output",
                    "Output resolution",
                    "Native keeps the source untouched",
                    &[
                        ("Native", "native"),
                        ("1080p", "1080p"),
                        ("1440p", "1440p"),
                        ("4K", "2160p"),
                    ],
                    cx,
                ),
                self.seg_int_row(
                    "video.render_target",
                    "Render target",
                    "Internal target before presentation",
                    &[(0, "Window"), (1080, "1080"), (1440, "1440"), (2160, "2160")],
                    cx,
                ),
                self.toggle_row("video.vsync", "VSync", "Lock presentation to the display", cx),
                self.seg_int_row(
                    "video.reflex",
                    "NVIDIA Reflex",
                    "Parent presenter only; DLSS-G owns its own swapchain",
                    &[(0, "Off"), (1, "On"), (2, "Boost")],
                    cx,
                ),
                self.stepper_row(
                    "video.sharpening",
                    "Sharpening",
                    "Applied after scaling",
                    0,
                    100,
                    5,
                    pct,
                    cx,
                ),
            ],
        );

        let display = group(
            "Display",
            "Monitor and fullscreen",
            vec![
                widgets::row(
                    "Monitor",
                    "Where the stream window opens",
                    widgets::button(
                        self.monitor_label(),
                        Tone::Ghost,
                        cx.listener(|this, _: &gpui::ClickEvent, _: &mut Window, cx: &mut Context<Self>| {
                            this.open_picker(crate::app::PickerKind::Monitor, cx);
                        }),
                    ),
                ),
                self.toggle_row(
                    "display.fullscreen_on_play",
                    "Fullscreen on play",
                    "Enter the stream without a window frame",
                    cx,
                ),
            ],
        );

        div()
            .flex()
            .flex_col()
            .gap(px(20.0))
            .child(widgets::page_title(
                "Video",
                "Presentation, scaling and display",
            ))
            .child(output)
            .child(display)
            .into_any_element()
    }

    pub(crate) fn page_audio(&self, cx: &mut Context<Self>) -> AnyElement {
        let card = group(
            "Audio",
            "Playback and latency",
            vec![
                widgets::row(
                    "Output device",
                    "WASAPI render endpoint",
                    widgets::button(
                        self.audio_label(),
                        Tone::Ghost,
                        cx.listener(|this, _: &gpui::ClickEvent, _: &mut Window, cx: &mut Context<Self>| {
                            this.open_picker(crate::app::PickerKind::Audio, cx);
                        }),
                    ),
                ),
                self.stepper_row(
                    "audio.buffer_ms",
                    "Buffer",
                    "Lower is tighter, higher is safer",
                    5,
                    60,
                    5,
                    ms,
                    cx,
                ),
                self.stepper_row(
                    "audio.volume",
                    "Volume",
                    "Applied to the stream mixer",
                    0,
                    100,
                    5,
                    pct,
                    cx,
                ),
                self.toggle_row("audio.mute", "Mute", "Silence the stream audio", cx),
            ],
        );
        div()
            .flex()
            .flex_col()
            .gap(px(20.0))
            .child(widgets::page_title("Audio", "Device, buffer and volume"))
            .child(card)
            .into_any_element()
    }

    pub(crate) fn page_controller(&self, cx: &mut Context<Self>) -> AnyElement {
        let card = group(
            "Controller",
            "Mapping, feedback and polling",
            vec![
                self.toggle_row("controller.enabled", "Controller input", "Send pad input to the console", cx),
                self.toggle_row("controller.haptics", "Haptics", "DualSense rumble passthrough", cx),
                self.toggle_row(
                    "controller.adaptive_triggers",
                    "Adaptive triggers",
                    "Trigger resistance from the console",
                    cx,
                ),
                self.stepper_row(
                    "controller.rumble_strength",
                    "Rumble strength",
                    "0 disables the motors",
                    0,
                    5,
                    1,
                    plain,
                    cx,
                ),
                self.seg_int_row(
                    "controller.poll_hz",
                    "Poll rate",
                    "Bluetooth can sustain 1000 Hz",
                    &[(125, "125"), (250, "250"), (500, "500"), (1000, "1000")],
                    cx,
                ),
                self.stepper_row(
                    "controller.lightbar",
                    "Lightbar",
                    "Colour reported to the pad",
                    0,
                    7,
                    1,
                    lightbar,
                    cx,
                ),
            ],
        );
        div()
            .flex()
            .flex_col()
            .gap(px(20.0))
            .child(widgets::page_title(
                "Controller",
                "DualSense features and 1000 Hz input",
            ))
            .child(card)
            .into_any_element()
    }

    pub(crate) fn page_upscaling(&self, cx: &mut Context<Self>) -> AnyElement {
        let sr = group(
            "Super resolution",
            "Per-frame reconstruction",
            vec![
                self.toggle_row(
                    "ai.super_resolution",
                    "Super resolution",
                    "Run DLSS on the decoded frame",
                    cx,
                ),
                self.seg_int_row(
                    "ai.sr_quality",
                    "Quality",
                    "Higher quality costs GPU time",
                    &[
                        (0, "Perf"),
                        (1, "Balanced"),
                        (2, "Quality"),
                        (3, "Ultra"),
                        (4, "Max"),
                        (5, "DLAA"),
                    ],
                    cx,
                ),
                self.seg_row(
                    "ai.sr_output",
                    "SR output",
                    "Target size for the reconstructed frame",
                    &[
                        ("Native", "native"),
                        ("1440p", "1440p"),
                        ("4K", "2160p"),
                    ],
                    cx,
                ),
                self.seg_int_row(
                    "ai.dlss_preset",
                    "DLSS model",
                    "Auto: K for DLAA/Quality, M for Perf, L for Ultra Perf",
                    &[
                        (0, "Auto"),
                        (1, "K"),
                        (2, "L"),
                        (3, "M"),
                        (4, "J"),
                    ],
                    cx,
                ),
            ],
        );

        div()
            .flex()
            .flex_col()
            .gap(px(20.0))
            .child(widgets::page_title(
                "Upscaling",
                "DLSS Super Resolution and DLAA",
            ))
            .child(sr)
            .into_any_element()
    }
    pub(crate) fn page_frame_gen(&self, cx: &mut Context<Self>) -> AnyElement {
        let dlss_backend = self.settings.s("ai.fg_backend", "none") == "dlss-fg";
        let mut fg_rows: Vec<AnyElement> = vec![
            self.toggle_row(
                "ai.frame_generation",
                "Frame generation",
                "Off for the lowest possible latency",
                cx,
            ),
            self.seg_row(
                "ai.fg_backend",
                "Backend",
                "DLSS-G needs an RTX 40/50",
                &[("None", "none"), ("DLSS-G", "dlss-fg")],
                cx,
            ),
        ];
        if dlss_backend {
            fg_rows.push(self.seg_int_row(
                "ai.fg_multiplier",
                "Multiplier",
                if self.status.dlss_fg_multi {
                    "2x holds 1 frame, 4x/6x hold 3/5; 60 fps + 2x is lowest. 5x/6x: RTX 50 MFG"
                } else {
                    "2x holds 1 frame, 4x/6x hold 3/5; 60 fps + 2x is lowest. 5x/6x forced"
                },
                &[
                    (2, "2x"),
                    (3, "3x"),
                    (4, "4x"),
                    (5, "5x"),
                    (6, "6x"),
                ],
                cx,
            ));
            if self.status.content_fps > 1.0 && self.status.content_fps < 45.0 {
                let c = self.status.content_fps;
                fg_rows.push(widgets::kv(
                    "Detected content",
                    format!("{c:.0} fps — 2x {:.0}, 4x {:.0}", c * 2.0, c * 4.0),
                ));
            }
        } else {
            fg_rows.push(self.disabled_row(
                "Multiplier",
                "Only with the DLSS-G backend",
                format!("{}x", self.settings.i("ai.fg_multiplier", 2)),
            ));
        }
        let fg = group(
            "Frame generation",
            "Generated frames between decoded ones",
            fg_rows,
        );

        div()
            .flex()
            .flex_col()
            .gap(px(20.0))
            .child(widgets::page_title(
                "Frame generation",
                "Generated frames between the decoded ones",
            ))
            .child(fg)
            .into_any_element()
    }
    pub(crate) fn page_neural(&self, cx: &mut Context<Self>) -> AnyElement {
        let runtime_text = if self.status.nr_runtime {
            if self.status.nr_ready {
                format!("worker active - {:.1} ms", self.status.nr_ms)
            } else {
                "runtime present".to_string()
            }
        } else {
            "runtime missing (nr\\native)".to_string()
        };
        let neural = widgets::card_pad()
            .flex()
            .flex_col()
            .gap(px(10.0))
            .child(widgets::section(
                "DLSS 5 neural rendering",
                "NGX feature 18 - video mode",
            ))
            .child(
                div()
                    .flex()
                    .items_center()
                    .gap(px(8.0))
                    .child(widgets::dot(if self.status.nr_runtime {
                        theme::GREEN
                    } else {
                        theme::AMBER
                    }))
                    .child(
                        div()
                            .text_size(theme::small())
                            .text_color(theme::text())
                            .child(runtime_text),
                    )
                    .child(widgets::chip("2D guides", theme::AMBER)),
            )
            .child(self.toggle_row(
                "ai.neural_render",
                "Neural rendering",
                "Run the network on every presented frame",
                cx,
            ))
            .child(self.seg_int_row(
                "ai.neural_style",
                "Style",
                "Network character",
                &[(0, "Default"), (1, "Natural"), (2, "Cinematic")],
                cx,
            ))
            .child(self.slider_row(
                "ai.neural_intensity",
                "Intensity",
                "Mix of the enhanced frame",
                "%",
                cx,
            ))
            .child(self.slider_row(
                "ai.neural_tone",
                "Local tone",
                "How much the local tone curve is applied",
                "%",
                cx,
            ))
            .child(self.slider_row(
                "ai.neural_structure",
                "Local structure",
                "Edge and texture emphasis",
                "%",
                cx,
            ))
            .child(self.slider_row(
                "ai.neural_skin",
                "Skin structure",
                "Detail preserved on skin",
                "%",
                cx,
            ))
            .child(self.slider_row(
                "ai.neural_passes",
                "Passes",
                "Cascade depth; above 1 needs Boost (work scale below 100%) and multiplies the cost",
                "",
                cx,
            ))
            .when(self.settings.i("ai.neural_scale", 100) >= 100, |el| {
                el.child(widgets::kv(
                    "Passes in use",
                    "1 — lower Work scale below 100% to enable Boost",
                ))
            })
            .child(self.slider_row(
                "ai.neural_scale",
                "Work scale",
                "75% runs the network at 1080p on a 1440p frame and keeps frame generation at 120 fps",
                "%",
                cx,
            ))
            .child(self.toggle_row(
                "ai.neural_ui",
                "UI correction",
                "Protect HUD edges from the network",
                cx,
            ))
            .child(self.toggle_row(
                "ai.neural_mask",
                "Auto mask",
                "Derive a soft mask from the frame",
                cx,
            ))
            .into_any_element();        let dlss5 = div()
            .flex()
            .flex_col()
            .gap(px(8.0))
            .p(px(18.0))
            .rounded(px(16.0))
            .bg(rgba(0xFBBF2410))
            .border_1()
            .border_color(rgba(0xFBBF2440))
            .child(
                div()
                    .flex()
                    .items_center()
                    .gap(px(9.0))
                    .child(widgets::dot(theme::AMBER))
                    .child(
                        div()
                            .text_size(theme::body())
                            .font_weight(FontWeight::SEMIBOLD)
                            .text_color(theme::text())
                            .child("DLSS 5 Neural Rendering"),
                    )
                    .child(widgets::chip("Video mode", theme::accent_bright_u32())),
            )
            .child(
                div()
                    .text_size(theme::small())
                    .text_color(theme::dim())
                    .child(
                        "The vendor runtime is running, but a Remote Play frame has no G-buffer: \
                         depth and motion are synthetic, history is reset every frame, and the \
                         network can only enrich lighting and material detail, not invent scene \
                         structure. In-game DLSS 5 guides (albedo, normals, camera matrices) are \
                         unavailable by definition here.",
                    ),
            );

        let unlock = div()
            .flex()
            .flex_col()
            .gap(px(8.0))
            .p(px(16.0))
            .rounded(px(14.0))
            .bg(rgba(0xF8717112))
            .border_1()
            .border_color(rgba(0xF8717140))
            .child(
                div()
                    .flex()
                    .items_center()
                    .gap(px(10.0))
                    .child(
                        div()
                            .text_size(theme::tiny())
                            .font_weight(FontWeight::SEMIBOLD)
                            .text_color(rgb(theme::RED))
                            .child("USE AT YOUR OWN RISK"),
                    )
                    .child(div().flex_1())
                    .child(self.toggle("ai.neural_unlock", cx)),
            )
            .child(
                div()
                    .text_size(theme::small())
                    .text_color(theme::dim())
                    .child(
                        "Unlocks every neural slider and typed value up to a hard 9999 \
                         ceiling. Anything above what the network can hold is accepted: it can \
                         stutter, fail to initialise or exhaust GPU memory.",
                    ),
            );

        div()
            .flex()
            .flex_col()
            .gap(px(20.0))
            .child(widgets::page_title(
                "Neural rendering",
                "DLSS 5 Neural Rendering - video mode",
            ))
            .child(neural)
            .child(dlss5)
            .child(unlock)
            .into_any_element()
    }
    pub(crate) fn page_overlay(&self, cx: &mut Context<Self>) -> AnyElement {
        let layout = group(
            "Layout",
            "Side-docked control center",
            vec![
                self.seg_int_row(
                    "overlay.side",
                    "Side",
                    "Which edge the panel docks to",
                    &[(0, "Left"), (1, "Right")],
                    cx,
                ),
                self.seg_int_row(
                    "overlay.position",
                    "Vertical position",
                    "Anchor on the chosen side",
                    &[(0, "Top"), (1, "Middle"), (2, "Bottom")],
                    cx,
                ),
                self.stepper_row(
                    "overlay.scale",
                    "Scale",
                    "Panel size relative to the stream",
                    75,
                    200,
                    5,
                    pct,
                    cx,
                ),
                self.stepper_row(
                    "overlay.opacity",
                    "Opacity",
                    "Backdrop transparency",
                    20,
                    100,
                    5,
                    pct,
                    cx,
                ),
                self.stepper_row(
                    "overlay.interval_ms",
                    "Refresh interval",
                    "Telemetry update period while open",
                    100,
                    1000,
                    50,
                    ms,
                    cx,
                ),
            ],
        );

        let content = group(
            "Content",
            "Sections shown in the panel",
            vec![
                self.toggle_row("overlay.show_stream", "Stream", "Codec, fps, bitrate, loss", cx),
                self.toggle_row("overlay.show_gpu", "GPU", "Utilisation, power, temperature", cx),
                self.toggle_row(
                    "overlay.show_processing",
                    "Processing",
                    "SR/FG timings and generated frames",
                    cx,
                ),
                self.toggle_row(
                    "overlay.show_performance",
                    "Performance",
                    "Frame pacing and decode histogram",
                    cx,
                ),
            ],
        );

        let hotkey = group(
            "Behaviour",
            "While streaming",
            vec![
                self.toggle_row("overlay.enabled", "Enable overlay", "Panel available in the stream", cx),
                widgets::row(
                    "Hotkey",
                    "Fixed F10 while streaming; Esc arms disconnect",
                    widgets::chip("F10", theme::accent_bright_u32()),
                ),
            ],
        );

        div()
            .flex()
            .flex_col()
            .gap(px(20.0))
            .child(widgets::page_title(
                "Overlay",
                "The in-game control center",
            ))
            .child(layout)
            .child(content)
            .child(hotkey)
            .into_any_element()
    }

    fn update_panel(&self, cx: &mut Context<Self>) -> AnyElement {
        let state = self.status.update_state.as_str();
        let progress = self.status.update_progress.clamp(0.0, 1.0) as f32;
        let notes = self.status.update_notes.clone();
        let (accent, headline, detail) = match state {
            "checking" => (
                theme::accent_bright_u32(),
                "Checking GitHub releases".to_string(),
                "Reading the latest tag and its assets".to_string(),
            ),
            "available" => (
                theme::AMBER,
                format!("v{} is available", self.status.update_version),
                "Download it when you are ready; the app restarts into the new version".to_string(),
            ),
            "downloading" => (
                theme::accent_bright_u32(),
                format!("Downloading v{}", self.status.update_version),
                format!("{:.0}% of the updater", progress * 100.0),
            ),
            "ready" => (
                theme::GREEN,
                format!("v{} is ready", self.status.update_version),
                "Install and restart to apply it".to_string(),
            ),
            "up-to-date" => (
                theme::GREEN,
                "You are on the latest release".to_string(),
                format!("Installed version {}", crate::model::APP_VERSION),
            ),
            "error" => (
                theme::RED,
                "Update check failed".to_string(),
                if self.status.update_error.is_empty() {
                    "Try again in a moment".to_string()
                } else {
                    self.status.update_error.clone()
                },
            ),
            _ => (
                theme::TEXT_MUTED,
                "Updates".to_string(),
                "Check GitHub for a newer release".to_string(),
            ),
        };

        let checking = state == "checking";
        let downloading = state == "downloading";
        let available = state == "available";
        let ready = state == "ready";

        // Aurora orbit: a small satellite loops around a soft core, tinted by
        // the update phase. The same vocabulary as the shell background, at
        // 52 px, so it reads as part of the app rather than a spinner.
        let orb = update_orb(accent, checking, 1);
        let mut actions = div().flex().items_center().gap(px(8.0));
        if available {
            actions = actions.child(widgets::button(
                "Download",
                Tone::Primary,
                cx.listener(|this, _: &gpui::ClickEvent, _: &mut Window, cx: &mut Context<Self>| {
                    if let Some(engine) = &this.engine {
                        engine.send("download-update");
                    }
                    cx.notify();
                }),
            ));
        }
        if ready {
            actions = actions.child(widgets::button(
                "Install & restart",
                Tone::Primary,
                cx.listener(|this, _: &gpui::ClickEvent, _: &mut Window, cx: &mut Context<Self>| {
                    if let Some(engine) = &this.engine {
                        engine.send("install-update");
                    }
                    // The updater waits for this process, then restarts the app.
                    // Quit a moment later so the command is on the wire first.
                    cx.spawn(async move |this, cx| {
                        cx.background_executor()
                            .timer(Duration::from_millis(1500))
                            .await;
                        let _ = this.update(cx, |_, cx| cx.quit());
                    })
                    .detach();
                }),
            ));
        }
        if !downloading {
            actions = actions.child(widgets::button(
                if checking { "Checking..." } else { "Check now" },
                Tone::Ghost,
                cx.listener(|this, _: &gpui::ClickEvent, _: &mut Window, cx: &mut Context<Self>| {
                    // One check at a time: spamming the button used to queue a
                    // burst of requests whose results raced each other.
                    if this.status.update_state == "checking"
                        || this.status.update_state == "downloading"
                    {
                        return;
                    }
                    if let Some(engine) = &this.engine {
                        engine.send("check-update");
                    }
                    cx.notify();
                }),
            ));
        }

        let header = div()
            .flex()
            .items_start()
            .gap(px(14.0))
            .child(orb)
            .child(
                div()
                    .flex()
                    .flex_col()
                    .gap(px(4.0))
                    .flex_1()
                    .min_w(px(0.0))
                    .child(
                        div()
                            .text_size(theme::body())
                            .font_weight(FontWeight::SEMIBOLD)
                            .text_color(theme::text())
                            .child(SharedString::from(headline)),
                    )
                    .child(
                        div()
                            .text_size(theme::small())
                            .text_color(theme::dim())
                            .child(SharedString::from(detail)),
                    ),
            );

        // Lay the note out line by line. The card grows with the text, so the
        // whole summary is visible instead of a clipped single line.
        let mut notes_block = div().flex().flex_col().gap(px(5.0));
        if available && !notes.is_empty() {
            for raw in notes.lines() {
                let line = raw.trim();
                if line.is_empty() {
                    notes_block = notes_block.child(div().h(px(2.0)));
                } else if let Some(item) = line
                    .strip_prefix("- ")
                    .or_else(|| line.strip_prefix("\u{2022} "))
                {
                    notes_block = notes_block.child(
                        div()
                            .flex()
                            .items_start()
                            .gap(px(8.0))
                            .child(
                                div()
                                    .mt(px(7.0))
                                    .w(px(4.0))
                                    .h(px(4.0))
                                    .rounded(px(2.0))
                                    .bg(theme::accent_rgba(0xFF)),
                            )
                            .child(
                                div()
                                    .flex_1()
                                    .min_w(px(0.0))
                                    .text_size(theme::small())
                                    .text_color(theme::text())
                                    .child(SharedString::from(item.to_string())),
                            ),
                    );
                } else {
                    notes_block = notes_block.child(
                        div()
                            .text_size(theme::small())
                            .text_color(theme::text())
                            .child(SharedString::from(line.to_string())),
                    );
                }
            }
        }

        let mut column = div().flex().flex_col().gap(px(12.0)).child(header);

        if available && !notes.is_empty() {
            column = column.child(
                div()
                    .flex()
                    .flex_col()
                    .gap(px(8.0))
                    .p(px(12.0))
                    .rounded(px(10.0))
                    .bg(rgba(0xFFFFFF08))
                    .border_1()
                    .border_color(rgba(0xFFFFFF12))
                    .child(notes_block),
            );
        }

        if downloading {
            let shimmer = div()
                .absolute()
                .top(px(0.0))
                .left(px(0.0))
                .h(px(6.0))
                .w(px(46.0))
                .bg(gpui::linear_gradient(
                    90.0,
                    gpui::linear_color_stop(rgba(0xFFFFFF00), 0.0),
                    gpui::linear_color_stop(rgba(0xFFFFFF66), 1.0),
                ))
                .with_animation(
                    "update-shimmer",
                    Animation::new(Duration::from_millis(1600)).repeat(),
                    |el, t| el.left(px(-46.0 + t * 320.0)),
                );
            column = column.child(
                div().flex().flex_col().gap(px(8.0)).child(div().h(px(2.0))).child(
                    div()
                        .relative()
                        .w_full()
                        .h(px(6.0))
                        .rounded(px(3.0))
                        .overflow_hidden()
                        .bg(rgba(0xFFFFFF14))
                        .child(
                            div()
                                .h_full()
                                .w(relative(progress))
                                .rounded(px(3.0))
                                .bg(gpui::linear_gradient(
                                    90.0,
                                    gpui::linear_color_stop(rgba((theme::accent_u32() << 8) | 0xFF), 0.0),
                                    gpui::linear_color_stop(rgba((theme::pink_u32() << 8) | 0xFF), 1.0),
                                )),
                        )
                        .child(shimmer),
                ),
            );
        }

        let link = if !self.status.update_url.is_empty() {
            widgets::button(
                "See full patch notes on GitHub",
                Tone::Ghost,
                cx.listener(
                    |this, _: &gpui::ClickEvent, _: &mut Window, cx: &mut Context<Self>| {
                        let url = this.status.update_url.clone();
                        if !url.is_empty() {
                            let _ = std::process::Command::new("rundll32")
                                .args(["url.dll,FileProtocolHandler", &url])
                                .spawn();
                            this.toast("Opened the release page in your browser", false, cx);
                        }
                    },
                ),
            )
        } else {
            div().into_any_element()
        };

        column = column.child(
            div()
                .flex()
                .items_center()
                .justify_between()
                .gap(px(12.0))
                .flex_wrap()
                .child(link)
                .child(actions),
        );

        widgets::card_pad().child(column).into_any_element()
    }

    fn theme_group(&self, cx: &mut Context<Self>) -> AnyElement {
        let current = self.settings.i("ui.palette", 0).max(0) as usize;
        let mut grid = div().flex().flex_wrap().gap(px(10.0));
        for (index, palette) in theme::PALETTES.iter().enumerate() {
            let active = index == current;
            grid = grid.child(
                div()
                    .id(("theme", index))
                    .flex()
                    .flex_col()
                    .gap(px(9.0))
                    .w(px(152.0))
                    .p(px(12.0))
                    .rounded(px(12.0))
                    .bg(if active {
                        theme::accent_rgba(0x1F)
                    } else {
                        rgba(0xFFFFFF08)
                    })
                    .border_1()
                    .border_color(if active {
                        theme::accent_rgba(0x99)
                    } else {
                        rgba(0xFFFFFF14)
                    })
                    .cursor_pointer()
                    .hover(|style| style.bg(rgba(0xFFFFFF10)))
                    .on_click(cx.listener(
                        move |this, _: &gpui::ClickEvent, _: &mut Window, cx: &mut Context<Self>| {
                            this.settings.set_int("ui.palette", index as i64);
                            this.persist();
                            cx.notify();
                        },
                    ))
                    .child(
                        div()
                            .flex()
                            .items_center()
                            .gap(px(5.0))
                            .child(swatch(palette.accent))
                            .child(swatch(palette.accent_bright))
                            .child(swatch(palette.accent_deep))
                            .child(swatch(palette.pink)),
                    )
                    .child(
                        div()
                            .flex()
                            .items_center()
                            .justify_between()
                            .child(
                                div()
                                    .text_size(theme::small())
                                    .font_weight(FontWeight::SEMIBOLD)
                                    .text_color(theme::text())
                                    .child(theme::PALETTE_NAMES[index]),
                            )
                            .when(active, |el| el.child(widgets::dot(theme::accent_u32()))),
                    ),
            );
        }
        group(
            "Theme",
            "Colour scheme for the shell and the control center",
            vec![grid.into_any_element()],
        )
    }

    pub(crate) fn page_advanced(&self, cx: &mut Context<Self>) -> AnyElement {
        let updates = group(
            "Updates",
            "GitHub releases",
            vec![
                self.text_row(
                    "general.update_repo",
                    "Repository",
                    "owner/name; releases carry a single-file update exe",
                    "owner/repo",
                    220.0,
                    cx,
                ),
                self.seg_int_row(
                    "general.update_channel",
                    "Channel",
                    "Stable skips prereleases",
                    &[(0, "Stable"), (1, "Beta")],
                    cx,
                ),
                self.update_panel(cx),
            ],
        );
        let shell = group(
            "Shell",
            "Appearance and startup",
            vec![
                self.seg_int_row(
                    "ui.scale",
                    "Text size",
                    "Scales every label and value in the shell",
                    &[(90, "90%"), (100, "100%"), (110, "110%"), (125, "125%")],
                    cx,
                ),
                self.toggle_row(
                    "ui.animated_background",
                    "Animated background",
                    "Aurora layer in the shell (never in the stream)",
                    cx,
                ),
                self.toggle_row(
                    "ui.reduce_motion",
                    "Reduce motion",
                    "Disable the boot animation and drift",
                    cx,
                ),
                self.toggle_row(
                    "ui.boot_animation",
                    "Boot animation",
                    "Short splash on launch",
                    cx,
                ),
                self.toggle_row(
                    "general.start_minimized",
                    "Start minimized",
                    "Open the shell minimized; use Connect from the taskbar",
                    cx,
                ),
            ],
        );

        let system = group(
            "System",
            "Detected hardware",
            vec![
                widgets::kv(
                    "GPU",
                    if self.status.gpu.is_empty() {
                        "Not detected".to_string()
                    } else {
                        self.status.gpu.clone()
                    },
                ),
                widgets::kv(
                    "VRAM",
                    if self.status.vram_mb > 0 {
                        format!("{} MB", self.status.vram_mb)
                    } else {
                        "\u{2014}".to_string()
                    },
                ),
                widgets::kv("Display", self.monitor_label()),
                widgets::kv(
                    "DLSS",
                    format!(
                        "SR {} / Frame Gen {}",
                        yes_no(self.status.dlss_sr_ready),
                        yes_no(self.status.dlss_fg_ready)
                    ),
                ),
                widgets::row(
                    "Optimize for this PC",
                    "Sets transport, output and the AI stages to what this machine can hold",
                    widgets::button(
                        "Optimize",
                        Tone::Primary,
                        cx.listener(
                            |this, _: &gpui::ClickEvent, _: &mut Window, cx: &mut Context<Self>| {
                                this.start_optimize(cx);
                            },
                        ),
                    ),
                ),
            ],
        );

        let pacing = group(
            "Pacing",
            "Live measurements",
            vec![
                widgets::kv("UI loop p99", format!("{:.2} ms", self.status.loop_p99)),
                widgets::kv(
                    "Cadence p50 / p99",
                    format!(
                        "{:.1} / {:.1} ms",
                        self.status.cadence_p50, self.status.cadence_p99
                    ),
                ),
                widgets::kv(
                    "Scanout",
                    if self.status.scanout_hz > 0.0 {
                        format!("{:.0} Hz", self.status.scanout_hz)
                    } else {
                        "\u{2014}".to_string()
                    },
                ),
                widgets::kv(
                    "Present misses",
                    // A refresh that repeats the last picture is expected when the
                    // source is slower than the display (30 fps on 240 Hz = 210/s).
                    // Only the repeats beyond that are real misses.
                    {
                        let expected = (self.status.scanout_hz - self.status.shown_fps).max(0.0);
                        format!("{:.0}/s", (self.status.present_misses - expected).max(0.0))
                    },
                ),
                widgets::kv("Shown", format!("{:.0} fps", self.status.shown_fps)),
                widgets::kv("Frame age", format!("{:.1} ms", self.status.age_ms)),
                widgets::kv("Present lag", format!("{:.1} ms", self.status.present_lag_ms)),
                widgets::kv(
                    "Input loop",
                    if self.status.input_avg_ms > 0.0 {
                        format!("{:.0} ms", self.status.input_avg_ms)
                    } else {
                        "\u{2014}".to_string()
                    },
                ),
                widgets::kv(
                    "Decode avg / max",
                    format!("{:.1} / {:.1} ms", self.status.decode_ms, self.status.decode_max),
                ),
                widgets::kv(
                    "Frame buffer",
                    format!("{}", self.settings.i("streaming.frame_buffer", 0)),
                ),
            ],
        );

        let diagnostics = group(
            "Diagnostics",
            "Logging",
            vec![
                self.seg_row(
                    "advanced.log_level",
                    "Log level",
                    "Applied on next engine start",
                    &[
                        ("Error", "error"),
                        ("Warn", "warn"),
                        ("Info", "info"),
                    ],
                    cx,
                ),
                widgets::row(
                    "Open settings folder",
                    crate::model::config_dir().to_string_lossy().into_owned(),
                    widgets::button("Open", Tone::Ghost, |_: &gpui::ClickEvent, _: &mut Window, _: &mut gpui::App| {
                        let _ = std::process::Command::new("explorer")
                            .arg(crate::model::config_dir())
                            .spawn();
                    }),
                ),
            ],
        );

        let reset = widgets::row(
            "Reset all settings",
            "Restores engine defaults; the current file is backed up first",
            widgets::button(
                "Reset",
                Tone::Danger,
                cx.listener(|this, _: &gpui::ClickEvent, _: &mut Window, cx: &mut Context<Self>| {
                    this.settings.reset();
                    this.persist();
                    cx.notify();
                    this.toast("Settings reset to defaults", false, cx);
                }),
            ),
        );

        let brand = group(
            "About",
            "Veyla 2.6.2",
            vec![
                widgets::kv("Product", "Veyla"),
                widgets::kv(
                    "Original project",
                    "BetterRemotePlay (trademark of the author)",
                ),
                widgets::kv("Engine", "veyla-engine.exe"),
            ],
        );

        let page = self.settings_page.min(SETTINGS_PAGES.len() - 1);
        let mut nav = div().flex().items_center().gap(px(4.0));
        for (index, (label, _)) in SETTINGS_PAGES.iter().enumerate() {
            let active = index == page;
            nav = nav.child(
                div()
                    .id(("settings-tab", index))
                    .flex()
                    .items_center()
                    .h(px(30.0))
                    .px(px(13.0))
                    .rounded(px(8.0))
                    .bg(if active {
                        theme::accent_rgba(0x26)
                    } else {
                        rgba(0x00000000)
                    })
                    .text_size(theme::small())
                    .font_weight(if active {
                        FontWeight::BOLD
                    } else {
                        FontWeight::SEMIBOLD
                    })
                    .text_color(if active { theme::text() } else { theme::muted() })
                    .cursor_pointer()
                    .hover(|style| style.bg(rgba(0xFFFFFF0D)).text_color(theme::text()))
                    .on_click(cx.listener(
                        move |this, _: &gpui::ClickEvent, _: &mut Window, cx: &mut Context<Self>| {
                            this.settings_page = index;
                            cx.notify();
                        },
                    ))
                    .child(*label),
            );
        }

        let theme_picker = self.theme_group(cx);
        let mut column = div()
            .flex()
            .flex_col()
            .gap(px(20.0))
            .child(widgets::page_title("Settings", SETTINGS_PAGES[page].1))
            .child(nav);
        column = match page {
            0 => column.child(theme_picker).child(shell),
            1 => column.child(self.console_setup(cx)),
            2 => column.child(updates),
            3 => column.child(system).child(pacing),
            4 => column.child(diagnostics).child(widgets::card_pad().child(reset)),
            _ => column.child(brand),
        };
        column.into_any_element()
    }
}

fn update_orb(accent: u32, fast: bool, seed: u32) -> AnyElement {
    div()
        .relative()
        .flex_none()
        .w(px(52.0))
        .h(px(52.0))
        .child(
            div()
                .absolute()
                .left(px(3.0))
                .top(px(3.0))
                .w(px(46.0))
                .h(px(46.0))
                .rounded(px(23.0))
                .bg(rgba((accent << 8) | 0x1E)),
        )
        .child(
            div()
                .absolute()
                .left(px(22.0))
                .top(px(22.0))
                .w(px(8.0))
                .h(px(8.0))
                .rounded(px(4.0))
                .bg(rgba((accent << 8) | 0xF0))
                .with_animation(
                    SharedString::from(format!("update-orbit-{seed}-{accent}")),
                    Animation::new(Duration::from_millis(if fast { 1100 } else { 2400 }))
                        .repeat(),
                    move |el, t| {
                        let angle = t * std::f32::consts::TAU;
                        el.left(px(22.0 + angle.cos() * 18.0))
                            .top(px(22.0 + angle.sin() * 18.0))
                    },
                ),
        )
        .child(
            div()
                .absolute()
                .left(px(19.0))
                .top(px(19.0))
                .w(px(14.0))
                .h(px(14.0))
                .rounded(px(7.0))
                .bg(rgba((accent << 8) | 0xCC))
                .with_animation(
                    SharedString::from(format!("update-core-{seed}-{accent}")),
                    Animation::new(Duration::from_millis(if fast { 800 } else { 3000 }))
                        .repeat(),
                    |el, t| {
                        let pulse = 0.5 + 0.5 * (t * std::f32::consts::TAU).sin();
                        el.opacity(0.55 + 0.45 * pulse)
                    },
                ),
        )
        .into_any_element()
}

fn address_for(settings: &crate::model::Settings) -> String {
    let address = settings.s("console.address", "");
    if address.is_empty() {
        "Not configured".to_string()
    } else {
        address
    }
}

fn yes_no(value: bool) -> &'static str {
    if value {
        "On"
    } else {
        "Off"
    }
}
