//! Application shell: window chrome, top navigation, boot splash, page router.

use std::time::Duration;

use gpui::{
    div, prelude::*, px, relative, rgba, rgb, Animation, AnimationExt as _, AnyElement, App,
    Context, FocusHandle, Focusable, FontWeight, InteractiveElement, IntoElement, MouseButton, Render, SharedString,
    StatefulInteractiveElement, Transformation, percentage,
    Window,
};

use crate::model::{Preset, Settings, Status};
use crate::{background, controls, pages, theme, widgets};

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Tab {
    Home,
    Stream,
    Video,
    Audio,
    Pad,
    Neural,
    Overlay,
    Advanced,
}

impl Tab {
    pub const ALL: [Tab; 8] = [
        Tab::Home,
        Tab::Stream,
        Tab::Video,
        Tab::Audio,
        Tab::Pad,
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
            Tab::Neural => "Neural",
            Tab::Overlay => "Overlay",
            Tab::Advanced => "Advanced",
        }
    }

    pub fn icon(self) -> &'static str {
        match self {
            Tab::Home => "◆",
            Tab::Stream => "▷",
            Tab::Video => "▣",
            Tab::Audio => "♪",
            Tab::Pad => "◎",
            Tab::Neural => "✦",
            Tab::Overlay => "▤",
            Tab::Advanced => "⚙",
        }
    }
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
    logged_render: bool,
    boot_timer_started: bool,
}

impl Root {
    pub fn new(cx: &mut Context<Self>) -> Self {
        crate::model::log_line(&format!(
            "root new autoconnect={}",
            std::env::var("BRP_UI_AUTOCONNECT").is_ok()
        ));
        let mut settings = Settings::load();
        // The shell owns these keys; make sure an engine-written file that
        // predates them still opens with sane values.
        if !settings.dirty {
            let _ = settings.save();
        }
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
                    let Some(engine) = this.engine.as_ref() else {
                        return;
                    };
                    let (rev, status, online) = {
                        let state = match engine.shared.lock() {
                            Ok(state) => state,
                            Err(_) => return,
                        };
                        (state.rev, state.status.clone(), state.online)
                    };
                    if rev != this.last_rev || online != this.engine_online {
                        this.last_rev = rev;
                        this.status = status;
                        this.engine_online = online;
                        if this.shell_minimized && !this.status.streaming {
                            this.shell_minimized = false;
                            restore_shell_window();
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

        Self {
            settings,
            tab: Tab::Home,
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
            logged_render: false,
            boot_timer_started: false,
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
            self.toast("Connecting to console…", false, cx);
        }
    }

    pub(crate) fn action_disconnect(&mut self, cx: &mut Context<Self>) {
        if self.engine.is_none() {
            self.toast("Engine is not running", false, cx);
            return;
        }
        self.send("disconnect");
        self.toast("Disconnecting", false, cx);
    }

    pub(crate) fn action_apply(&mut self, cx: &mut Context<Self>) {
        if self.ensure_engine(cx) {
            self.send("apply");
            self.toast("Settings applied", false, cx);
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
        let show_labels = width > px(1110.0);
        let mut tabs = div().flex().items_center().gap(px(2.0));
        for tab in Tab::ALL {
            let selected = self.tab == tab;
            let icon = tab.icon();
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
                    .text_size(theme::T_SMALL)
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
                    .child(
                        div()
                            .text_size(px(12.0))
                            .text_color(if selected { theme::accent() } else { theme::muted() })
                            .child(icon),
                    )
                    .when(show_labels, |el| el.child(label)),
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
                    .on_mouse_down(MouseButton::Left, |_, window, _| window.start_window_move())
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
            .child(div().flex_1())
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
                                .text_size(theme::T_TINY)
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
                                .text_size(theme::T_TINY)
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
                            this.persist();
                            cx.notify();
                        }),
                    ))
                    .when(!tight, |el| {
                        el.child(
                            div()
                                .text_size(theme::T_TINY)
                                .text_color(if self.advanced { theme::accent() } else { theme::muted() })
                                .child("Advanced"),
                        )
                    }),
            )
            .child(div().w(px(8.0)))
            .child(widgets::button(
                if tight { "✓" } else { "Apply" },
                widgets::Tone::Subtle,
                cx.listener(|this, _: &gpui::ClickEvent, _: &mut Window, cx: &mut Context<Self>| {
                    this.action_apply(cx);
                }),
            ))
            .child(window_button("min", "–", false, |window| window.minimize_window()))
            .child(window_button("max", "▢", false, |window| window.zoom_window()))
            .child(window_button("close", "✕", true, |window| window.remove_window()))
            .into_any_element()
    }

    fn page(&mut self, cx: &mut Context<Self>) -> AnyElement {
        match self.tab {
            Tab::Home => self.page_home(cx),
            Tab::Stream => self.page_stream(cx),
            Tab::Video => self.page_video(cx),
            Tab::Audio => self.page_audio(cx),
            Tab::Pad => self.page_controller(cx),
            Tab::Neural => self.page_neural(cx),
            Tab::Overlay => self.page_overlay(cx),
            Tab::Advanced => self.page_advanced(cx),
        }
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
                            .text_size(theme::T_H3)
                            .font_weight(FontWeight::SEMIBOLD)
                            .text_color(theme::text())
                            .child("Connecting"),
                    )
                    .child(
                        div()
                            .text_size(theme::T_SMALL)
                            .text_color(theme::dim())
                            .child(message),
                    )
                    .child(
                        div()
                            .font_family(theme::MONO_FONT)
                            .text_size(theme::T_TINY)
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
                    .text_size(theme::T_SMALL)
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

    fn boot_overlay(&self) -> AnyElement {
        let splash = div()
            .absolute()
            .size_full()
            .bg(rgba(0x050509FF))
            .flex()
            .flex_col()
            .items_center()
            .justify_center()
            .gap(px(18.0))
            .child(widgets::logo_mark(74.0))
            .child(
                div()
                    .text_size(px(25.0))
                    .font_weight(FontWeight::SEMIBOLD)
                    .text_color(theme::text())
                    .child("BetterRemotePlay"),
            )
            .child(
                div()
                    .text_size(theme::T_SMALL)
                    .text_color(theme::muted())
                    .child("Remote Play, refined"),
            )
            .child(
                div()
                    .mt(px(10.0))
                    .w(px(220.0))
                    .h(px(3.0))
                    .rounded(px(2.0))
                    .bg(rgba(0xFFFFFF10))
                    .overflow_hidden()
                    .child(
                        div()
                            .h_full()
                            .rounded(px(2.0))
                            .bg(rgba(0x8B5CF6FF))
                            .with_animation(
                                "boot-progress",
                                Animation::new(Duration::from_millis(1350)),
                                |el, t| el.w(relative(0.04 + 0.96 * t)),
                            ),
                    ),
            );

        splash.into_any_element()
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
                            .text_size(theme::T_SMALL)
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
        if self.status.streaming && !self.shell_minimized {
            self.shell_minimized = true;
            self.booting = false;
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
        let animated = self.settings.b("ui.animated_background", true);
        let reduce = self.settings.b("ui.reduce_motion", false);
        let top = self.top_bar(window, cx);
        let page = self.page(cx);
        let toast = self.toast_overlay();
        let connecting = self.connect_overlay(cx);

        div()
            .relative()
            .size_full()
            .overflow_hidden()
            .font_family(theme::UI_FONT)
            .text_size(theme::T_BODY)
            .text_color(theme::text())
            .track_focus(&self.focus)
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
                            .child(
                                div()
                                    .w_full()
                                    .max_w(px(1180.0))
                                    .mx_auto()
                                    .px(px(26.0))
                                    .pt(px(26.0))
                                    .pb(px(44.0))
                                    .child(page),
                            ),
                    ),
            )
            .child(connecting)
            .child(toast)
            .into_any_element()
    }
}
