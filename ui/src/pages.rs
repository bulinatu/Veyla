//! Settings pages. Every control writes the same flat settings keys the engine
//! already reads, so the new shell is a drop-in front-end.

use gpui::{
    div, prelude::*, px, rgba, rgb, AnyElement, Context, FontWeight, IntoElement,
    SharedString, Window,
};

use crate::app::Root;
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

impl Root {
    pub(crate) fn page_home(&mut self, cx: &mut Context<Self>) -> AnyElement {
        let name: SharedString = if self.status.console_name.is_empty() {
            self.settings.s("console.name", "PS5").into()
        } else {
            self.status.console_name.clone().into()
        };
        let address: SharedString = {
            let a = self.settings.s("console.address", "");
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
            format!("{:.0} fps", self.status.fps)
        } else {
            format!("{} fps", self.settings.i("streaming.fps", 60))
        };

        let connect_label: SharedString = if connected || streaming {
            "▶  Watch".into()
        } else {
            "▶  Connect & Play".into()
        };
        let connect = div()
            .id("hero-connect")
            .flex()
            .items_center()
            .justify_center()
            .h(px(52.0))
            .px(px(28.0))
            .rounded(px(13.0))
            .bg(rgba(0x8B5CF6FF))
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

        let hero = div()
            .flex()
            .flex_col()
            .gap(px(16.0))
            .p(px(24.0))
            .rounded(px(20.0))
            .border_1()
            .border_color(rgba(0x8B5CF63D))
            .bg(gpui::linear_gradient(
                135.0,
                gpui::linear_color_stop(rgba(0x2A1655FF), 0.0),
                gpui::linear_color_stop(rgba(0x0B0B14FF), 1.0),
            ))
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
                        if streaming { theme::GREEN } else { theme::ACCENT_BRIGHT },
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
                    .child(disconnect),
            )
            .child(
                div()
                    .text_size(theme::tiny())
                    .text_color(theme::muted())
                    .child("Esc arms disconnect  ·  F10 control center  ·  Alt+F4 leave stream"),
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
                "Decode",
                if self.status.decode_ms > 0.0 {
                    format!("{:.1} ms", self.status.decode_ms)
                } else {
                    "—".to_string()
                },
                if self.status.decode_ms > 12.0 {
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
                        div()
                            .id(SharedString::from(format!("preset-{}", preset.label())))
                            .flex()
                            .flex_col()
                            .gap(px(4.0))
                            .flex_1()
                            .min_w(px(0.0))
                            .p(px(12.0))
                            .rounded(px(12.0))
                            .bg(rgba(0xFFFFFF08))
                            .border_1()
                            .border_color(rgba(0xFFFFFF12))
                            .cursor_pointer()
                            .hover(|s| s.bg(rgba(0xFFFFFF12)).border_color(rgba(0x8B5CF666)))
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
        let mut saved_rows = div().flex().flex_col().gap(px(6.0));
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
                saved_rows = saved_rows.child(
                    div()
                        .flex()
                        .items_center()
                        .justify_between()
                        .gap(px(8.0))
                        .child(
                            div()
                                .flex()
                                .items_center()
                                .gap(px(6.0))
                                .child(
                                    div()
                                        .text_size(theme::small())
                                        .text_color(theme::text())
                                        .child(profile.name),
                                )
                                .when(is_default, |el| {
                                    el.child(widgets::chip("Default", theme::ACCENT_BRIGHT))
                                }),
                        )
                        .child(widgets::button(
                            "Apply",
                            Tone::Subtle,
                            cx.listener(
                                move |this, _: &gpui::ClickEvent, _: &mut Window, cx: &mut Context<Self>| {
                                    this.apply_profile(&id, cx);
                                },
                            ),
                        )),
                );
            }
        }
        let profiles_group = widgets::card_pad()
            .flex()
            .flex_col()
            .gap(px(10.0))
            .child(widgets::section("Saved profiles", "Engine configuration"))
            .child(saved_rows)
            .child(widgets::button(
                "Save current",
                Tone::Ghost,
                cx.listener(
                    |this, _: &gpui::ClickEvent, _: &mut Window, cx: &mut Context<Self>| {
                        this.save_profile(cx);
                    },
                ),
            ));

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
                        "—".to_string()
                    },
                ),
                widgets::kv(
                    "Scanout",
                    if self.status.scanout_hz > 0.0 {
                        format!("{:.0} Hz", self.status.scanout_hz)
                    } else {
                        "—".to_string()
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
                    .child("BetterRemotePlay Next 2.4 · GPUI shell"),
            )
            .child(
                div()
                    .flex()
                    .items_center()
                    .gap(px(8.0))
                    .child(widgets::chip(
                        format!("{} · {}", self.settings.s("streaming.fps", "60"), "fps"),
                        theme::ACCENT_BRIGHT,
                    ))
                    .child(widgets::chip(
                        format!("Buffer {}", self.settings.i("streaming.frame_buffer", 1)),
                        theme::CYAN,
                    ))
                    .child(widgets::chip(
                        format!("{} Hz pad", self.settings.i("controller.poll_hz", 250)),
                        theme::PINK,
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

        div()
            .flex()
            .flex_col()
            .gap(px(20.0))
            .child(widgets::page_title(
                "Home",
                "One console, one click, no menus in the way",
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
                            .child(profile_group)
                            .child(profiles_group),
                    ),
            )
            .child(stats)
            .child(
                div()
                    .flex()
                    .gap(px(16.0))
                    .child(div().flex_1().child(live_group))
                    .child(div().w(px(330.0)).child(
                        widgets::card_pad()
                            .flex()
                            .flex_col()
                            .gap(px(10.0))
                            .child(widgets::section("Console", "This session"))
                            .child(widgets::kv("Address", address_for(&self.settings)))
                            .child(widgets::kv("Auto reconnect", yes_no(self.settings.b("console.auto_reconnect", true))))
                            .child(widgets::kv("Fullscreen", yes_no(self.settings.b("display.fullscreen_on_play", true))))
                            .child(widgets::kv("Overlay", self.settings.s("overlay.hotkey", "F10"))),
                    )),
            )
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
                self.seg_row(
                    "streaming.input",
                    "Input resolution",
                    "Requested console capture size",
                    &[
                        ("Native", "native"),
                        ("720p", "720p"),
                        ("1080p", "1080p"),
                        ("1440p", "1440p"),
                        ("4K", "2160p"),
                    ],
                    cx,
                ),
                self.seg_int_row(
                    "streaming.fps",
                    "Frame rate",
                    "30 locks to a cinematic cadence; 60 halves latency",
                    &[(30, "30"), (60, "60"), (120, "120")],
                    cx,
                ),
                self.seg_row(
                    "streaming.codec",
                    "Codec",
                    "Auto negotiates with the console",
                    &[
                        ("Auto", "auto"),
                        ("H.264", "h264"),
                        ("H.265", "h265"),
                        ("AV1", "av1"),
                    ],
                    cx,
                ),
                self.stepper_row(
                    "streaming.bitrate_kbps",
                    "Bitrate",
                    "Higher is sharper, needs headroom",
                    10000,
                    200000,
                    5000,
                    mbps,
                    cx,
                ),
                self.seg_int_row(
                    "streaming.frame_buffer",
                    "Frame buffer",
                    "0 is lowest latency; 1-2 is smoother",
                    &[(0, "0"), (1, "1"), (2, "2"), (3, "3")],
                    cx,
                ),
                self.toggle_row(
                    "streaming.hdr",
                    "HDR transport",
                    "Requires an HDR display and console support",
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
                self.toggle_row("video.vrr", "VRR", "Let the display follow the stream", cx),
                self.seg_row(
                    "video.color_space",
                    "Colour space",
                    "Auto follows the decoded signal",
                    &[
                        ("Auto", "auto"),
                        ("BT.709", "bt709"),
                        ("BT.2020", "bt2020"),
                    ],
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
                self.seg_row(
                    "display.refresh",
                    "Refresh rate",
                    "Match follows the current display mode",
                    &[
                        ("Match", "match"),
                        ("60", "60"),
                        ("120", "120"),
                        ("144", "144"),
                        ("240", "240"),
                    ],
                    cx,
                ),
                self.seg_int_row(
                    "display.bit_depth",
                    "Bit depth",
                    "10-bit for HDR",
                    &[(8, "8-bit"), (10, "10-bit"), (12, "12-bit")],
                    cx,
                ),
                self.seg_row(
                    "display.output_format",
                    "Colour format",
                    "Auto negotiates chroma",
                    &[
                        ("Auto", "auto"),
                        ("RGB", "rgb"),
                        ("4:4:4", "444"),
                        ("4:2:2", "422"),
                    ],
                    cx,
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

    pub(crate) fn page_neural(&self, cx: &mut Context<Self>) -> AnyElement {
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
            ],
        );

        let fg = group(
            "Frame generation",
            "Generated frames between decoded ones",
            vec![
                self.toggle_row(
                    "ai.frame_generation",
                    "Frame generation",
                    "Off for the lowest possible latency",
                    cx,
                ),
                self.seg_row(
                    "ai.fg_backend",
                    "Backend",
                    "DLSS-G needs an RTX 40/50; RIFE runs on the app's presenter",
                    &[
                        ("None", "none"),
                        ("DLSS-G", "dlss-fg"),
                        ("RIFE", "rife"),
                    ],
                    cx,
                ),
                self.seg_int_row(
                    "ai.fg_multiplier",
                    "Multiplier",
                    "2x is the stable multiplier at 30 fps",
                    &[(2, "2x"), (3, "3x"), (4, "4x")],
                    cx,
                ),
                self.toggle_row(
                    "ai.motion_safety",
                    "Motion safety",
                    "Fall back to real frames on fast cuts",
                    cx,
                ),
                self.seg_int_row(
                    "ai.rife_model",
                    "RIFE model",
                    "Only used by the RIFE backend",
                    &[(0, "Lite"), (1, "Heavy")],
                    cx,
                ),
                self.stepper_row(
                    "ai.blend",
                    "Blend",
                    "Mix generated and real frames",
                    0,
                    100,
                    5,
                    pct,
                    cx,
                ),
            ],
        );

        let runtime_text = if self.status.nr_runtime {
            if self.status.nr_ready {
                format!("worker active Â· {:.1} ms", self.status.nr_ms)
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
                "NGX feature 18 Â· video mode",
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
                "Enhances the frame before SR; adds about 10 ms",
                cx,
            ))
            .child(self.seg_int_row(
                "ai.neural_style",
                "Style",
                "Network character",
                &[(0, "Default"), (1, "Natural"), (2, "Cinematic")],
                cx,
            ))
            .child(self.stepper_row(
                "ai.neural_intensity",
                "Intensity",
                "Mix of the enhanced frame",
                0,
                200,
                10,
                pct,
                cx,
            ))
            .child(self.seg_int_row(
                "ai.neural_passes",
                "Passes",
                "Cascade depth (Boost)",
                &[(1, "1"), (2, "2"), (3, "3"), (4, "4")],
                cx,
            ))
            .child(self.stepper_row(
                "ai.neural_scale",
                "Work scale",
                "Network resolution; lower is cheaper",
                50,
                100,
                5,
                pct,
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
            .into_any_element();
        let dlss5 = div()
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
                    .child(widgets::chip("Video mode", theme::ACCENT_BRIGHT)),
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

        div()
            .flex()
            .flex_col()
            .gap(px(20.0))
            .child(widgets::page_title(
                "Neural",
                "Super resolution, frame generation and neural render",
            ))
            .child(sr)
            .child(fg)
            .child(neural)
            .child(dlss5)
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
                    widgets::chip("F10", theme::ACCENT_BRIGHT),
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

    pub(crate) fn page_advanced(&self, cx: &mut Context<Self>) -> AnyElement {
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
                        "—".to_string()
                    },
                ),
                widgets::kv(
                    "Present misses",
                    format!("{:.0}/s", self.status.present_misses),
                ),
                widgets::kv("Shown", format!("{:.0} fps", self.status.shown_fps)),
                widgets::kv(
                    "Decode avg / max",
                    format!("{:.1} / {:.1} ms", self.status.decode_ms, self.status.decode_max),
                ),
                widgets::kv(
                    "Frame buffer",
                    format!("{}", self.settings.i("streaming.frame_buffer", 1)),
                ),
            ],
        );

        let diagnostics = group(
            "Diagnostics",
            "Logging and compatibility",
            vec![
                self.toggle_row("advanced.debug_log", "Debug log", "Verbose engine log file", cx),
                self.toggle_row(
                    "advanced.experimental",
                    "Experimental",
                    "Enable unfinished paths",
                    cx,
                ),
                self.toggle_row(
                    "advanced.prefer_gpu_present",
                    "GPU present",
                    "Prefer the GPU present path",
                    cx,
                ),
                self.seg_row(
                    "advanced.log_level",
                    "Log level",
                    "Applied on next engine start",
                    &[
                        ("Error", "error"),
                        ("Warn", "warn"),
                        ("Info", "info"),
                        ("Debug", "debug"),
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

        let gpu = group(
            "GPU",
            "Device selection",
            vec![
                widgets::row(
                    "Device",
                    "Adapter used by decode and AI",
                    widgets::button(
                        self.gpu_label(),
                        Tone::Ghost,
                        cx.listener(|this, _: &gpui::ClickEvent, _: &mut Window, cx: &mut Context<Self>| {
                            this.open_picker(crate::app::PickerKind::Gpu, cx);
                        }),
                    ),
                ),
                self.toggle_row("gpu.fp16", "FP16", "Half precision inference", cx),
                self.toggle_row("gpu.int8", "INT8", "Quantised inference where supported", cx),
                self.stepper_row(
                    "gpu.frame_buffers",
                    "GPU buffers",
                    "Pool size for the decoder and AI passes",
                    1,
                    8,
                    1,
                    plain,
                    cx,
                ),
                self.stepper_row(
                    "gpu.memory_limit_mb",
                    "Memory limit",
                    "0 uses the driver budget",
                    0,
                    24576,
                    512,
                    |v| {
                        if v == 0 {
                            "Auto".to_string()
                        } else {
                            format!("{} MB", v)
                        }
                    },
                    cx,
                ),
                self.seg_int_row(
                    "gpu.priority",
                    "Priority",
                    "Above normal can reduce hitches",
                    &[(0, "Normal"), (1, "Above")],
                    cx,
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

        div()
            .flex()
            .flex_col()
            .gap(px(20.0))
            .child(widgets::page_title(
                "Advanced",
                "Shell, diagnostics, GPU and reset",
            ))
            .child(shell)
            .child(pacing)
            .child(diagnostics)
            .child(gpu)
            .child(widgets::card_pad().child(reset))
            .into_any_element()
    }
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
