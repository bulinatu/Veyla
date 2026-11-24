//! Interactive settings controls. All handlers mutate the shared `Settings`
//! map, persist it, and notify the view.

use gpui::{
    div, prelude::*, px, rgba, rgb, AnyElement, ClickEvent, Context, FontWeight, MouseButton,
    InteractiveElement, SharedString, StatefulInteractiveElement, Window,
};
use serde_json::Value;

use crate::app::Root;
use crate::theme;
use crate::widgets;

impl Root {
    /// Numeric slider row used by the Neural page. The range depends on the
    /// "unlock limits" switch; the value itself is free up to the hard 9999 cap.
    pub fn neural_range(&self, key: &str) -> (i64, i64) {
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

    pub fn slider_row(
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
        let slider = widgets::slider(key, value, min, max, 210.0, move |next, _window, cx| {
            let _ = weak.update(cx, |this, cx| {
                this.settings.set_int(key, next);
                this.settings_rev = this.settings_rev.wrapping_add(1);
                cx.notify();
            });
        });
        let number = div()
            .id(SharedString::from(format!("num-{key}")))
            .flex()
            .items_center()
            .justify_center()
            .min_w(px(78.0))
            .h(px(30.0))
            .px(px(9.0))
            .rounded(px(8.0))
            .bg(if editing {
                rgba(0x8B5CF626)
            } else {
                rgba(0x00000040)
            })
            .border_1()
            .border_color(if editing {
                rgba(0x8B5CF6CC)
            } else {
                rgba(0xFFFFFF10)
            })
            .font_family(theme::MONO_FONT)
            .text_size(theme::mono())
            .text_color(theme::text())
            .cursor_text()
            .on_click(
                cx.listener(move |this, _: &ClickEvent, window, cx| {
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
            .gap(px(10.0))
            .child(slider)
            .child(number)
            .into_any_element();
        widgets::row(label, hint, control)
    }

    pub fn persist(&mut self) {
        let _ = self.settings.save();
        self.settings_mtime = self.settings.file_mtime();
        self.settings_rev = self.settings_rev.wrapping_add(1);
    }

    pub fn toggle(&self, key: &'static str, cx: &mut Context<Self>) -> AnyElement {
        let on = self.settings.b(key, false);
        widgets::switch(
            key,
            on,
            cx.listener(move |this, _: &ClickEvent, _: &mut Window, cx: &mut Context<Self>| {
                this.settings.flip(key);
                this.persist();
                cx.notify();
            }),
        )
    }

    pub fn toggle_row(
        &self,
        key: &'static str,
        label: &'static str,
        hint: &'static str,
        cx: &mut Context<Self>,
    ) -> AnyElement {
        let control = self.toggle(key, cx);
        widgets::row(label, hint, control)
    }

    fn pill(
        label: &'static str,
        selected: bool,
        key: &'static str,
        value: Value,
        cx: &mut Context<Self>,
    ) -> AnyElement {
        div()
            .id(SharedString::from(format!("pill-{key}-{value}")))
            .flex()
            .items_center()
            .justify_center()
            .h(px(30.0))
            .px(px(12.0))
            .rounded(px(8.0))
            .bg(if selected {
                rgba(0x8B5CF6FF)
            } else {
                rgba(0xFFFFFF0A)
            })
            .border_1()
            .border_color(if selected {
                rgba(0x9D74FFFF)
            } else {
                rgba(0xFFFFFF14)
            })
            .text_size(theme::tiny())
            .font_weight(if selected {
                FontWeight::SEMIBOLD
            } else {
                FontWeight::MEDIUM
            })
            .text_color(if selected {
                rgb(0xFFFFFF)
            } else {
                theme::dim()
            })
            .cursor_pointer()
            .hover(move |s| {
                if selected {
                    s
                } else {
                    s.bg(rgba(0xFFFFFF16))
                }
            })
            .on_click(
                cx.listener(move |this, _: &ClickEvent, _: &mut Window, cx: &mut Context<Self>| {
                    this.settings.set(key, value.clone());
                    this.persist();
                    cx.notify();
                }),
            )
            .child(label)
            .into_any_element()
    }

    pub fn seg(
        &self,
        key: &'static str,
        options: &[(&'static str, &'static str)],
        cx: &mut Context<Self>,
    ) -> AnyElement {
        let current = self.settings.s(key, options[0].1);
        let mut row = div().flex().items_center().gap(px(4.0));
        for (label, value) in options {
            let selected = current == *value;
            row = row.child(Self::pill(
                label,
                selected,
                key,
                Value::from(*value),
                cx,
            ));
        }
        row.into_any_element()
    }

    pub fn seg_int(
        &self,
        key: &'static str,
        options: &[(i64, &'static str)],
        cx: &mut Context<Self>,
    ) -> AnyElement {
        let current = self.settings.i(key, options[0].0);
        let mut row = div().flex().items_center().gap(px(4.0));
        for (value, label) in options {
            let selected = current == *value;
            row = row.child(Self::pill(
                label,
                selected,
                key,
                Value::from(*value),
                cx,
            ));
        }
        row.into_any_element()
    }

    pub fn seg_row(
        &self,
        key: &'static str,
        label: &'static str,
        hint: &'static str,
        options: &[(&'static str, &'static str)],
        cx: &mut Context<Self>,
    ) -> AnyElement {
        let control = self.seg(key, options, cx);
        widgets::row(label, hint, control)
    }

    pub fn seg_int_row(
        &self,
        key: &'static str,
        label: &'static str,
        hint: &'static str,
        options: &[(i64, &'static str)],
        cx: &mut Context<Self>,
    ) -> AnyElement {
        let control = self.seg_int(key, options, cx);
        widgets::row(label, hint, control)
    }

    pub fn stepper(
        &self,
        key: &'static str,
        min: i64,
        max: i64,
        step: i64,
        format: fn(i64) -> String,
        cx: &mut Context<Self>,
    ) -> AnyElement {
        let value = self.settings.i(key, min);
        let label = format(value);

        let minus = {
            let disabled = value <= min;
            div()
                .id(SharedString::from(format!("{key}-minus")))
                .flex()
                .items_center()
                .justify_center()
                .w(px(30.0))
                .h(px(30.0))
                .rounded(px(8.0))
                .bg(rgba(0xFFFFFF0A))
                .text_size(px(17.0))
                .text_color(if disabled { rgb(0x4A4A5E) } else { theme::dim() })
                .cursor_pointer()
                .hover(move |s| {
                    if disabled {
                        s
                    } else {
                        s.bg(rgba(0xFFFFFF1A))
                    }
                })
                .when(!disabled, |el| {
                    el.on_click(cx.listener(
                        move |this, _: &ClickEvent, _: &mut Window, cx: &mut Context<Self>| {
                            this.settings.bump(key, -step, min, max, min);
                            this.persist();
                            cx.notify();
                        },
                    ))
                })
                .child("\u{2212}")
                .into_any_element()
        };

        let plus = {
            let disabled = value >= max;
            div()
                .id(SharedString::from(format!("{key}-plus")))
                .flex()
                .items_center()
                .justify_center()
                .w(px(30.0))
                .h(px(30.0))
                .rounded(px(8.0))
                .bg(rgba(0xFFFFFF0A))
                .text_size(px(17.0))
                .text_color(if disabled { rgb(0x4A4A5E) } else { theme::dim() })
                .cursor_pointer()
                .hover(move |s| {
                    if disabled {
                        s
                    } else {
                        s.bg(rgba(0xFFFFFF1A))
                    }
                })
                .when(!disabled, |el| {
                    el.on_click(cx.listener(
                        move |this, _: &ClickEvent, _: &mut Window, cx: &mut Context<Self>| {
                            this.settings.bump(key, step, min, max, min);
                            this.persist();
                            cx.notify();
                        },
                    ))
                })
                .child("+")
                .into_any_element()
        };

        div()
            .flex()
            .items_center()
            .gap(px(2.0))
            .p(px(3.0))
            .rounded(px(10.0))
            .bg(rgba(0x00000040))
            .border_1()
            .border_color(rgba(0xFFFFFF10))
            .child(minus)
            .child(
                div()
                    .min_w(px(86.0))
                    .h(px(30.0))
                    .flex()
                    .items_center()
                    .justify_center()
                    .font_family(theme::MONO_FONT)
                    .text_size(theme::mono())
                    .text_color(theme::text())
                    .child(label),
            )
            .child(plus)
            .into_any_element()
    }

    pub fn disabled_row(
        &self,
        label: &'static str,
        hint: &'static str,
        value: impl Into<SharedString>,
    ) -> AnyElement {
        let chip = div()
            .flex()
            .items_center()
            .justify_center()
            .h(px(30.0))
            .px(px(12.0))
            .rounded(px(8.0))
            .bg(rgba(0xFFFFFF08))
            .border_1()
            .border_color(rgba(0xFFFFFF12))
            .text_size(theme::tiny())
            .text_color(theme::dim())
            .child(value.into())
            .into_any_element();
        div()
            .opacity(0.38)
            .child(widgets::row(label, hint, chip))
            .into_any_element()
    }

    pub fn stepper_row(
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
        let control = self.stepper(key, min, max, step, format, cx);
        widgets::row(label, hint, control)
    }

    pub fn text_field(
        &self,
        key: &'static str,
        placeholder: &'static str,
        width: f32,
        cx: &mut Context<Self>,
    ) -> AnyElement {
        let value = self.settings.s(key, "");
        let editing = self.editing == Some(key);
        let focus = self.focus.clone();
        let shown: SharedString = if value.is_empty() {
            placeholder.into()
        } else {
            value.clone().into()
        };
        div()
            .id(SharedString::from(format!("field-{key}")))
            .flex()
            .items_center()
            .w(px(width))
            .h(px(36.0))
            .px(px(12.0))
            .rounded(px(9.0))
            .bg(rgba(theme::FIELD))
            .border_1()
            .border_color(if editing {
                rgba(0x8B5CF6CC)
            } else {
                rgba(0xFFFFFF14)
            })
            .font_family(theme::MONO_FONT)
            .text_size(theme::mono())
            .text_color(if value.is_empty() {
                rgba(0xFFFFFF45)
            } else {
                theme::text()
            })
            .cursor_text()
            .on_mouse_down(
                MouseButton::Left,
                cx.listener(move |this, _, window, cx| {
                    this.editing = Some(key);
                    window.focus(&focus);
                    cx.notify();
                }),
            )
            .child(shown)
            .when(editing, |el| {
                el.child(
                    div()
                        .w(px(1.5))
                        .h(px(16.0))
                        .ml(px(1.0))
                        .bg(rgba(0x8B5CF6FF)),
                )
            })
            .into_any_element()
    }

    pub fn text_row(
        &self,
        key: &'static str,
        label: &'static str,
        hint: &'static str,
        placeholder: &'static str,
        width: f32,
        cx: &mut Context<Self>,
    ) -> AnyElement {
        let control = self.text_field(key, placeholder, width, cx);
        widgets::row(label, hint, control)
    }

    /// Keyboard routing for the single active inline editor.
    pub fn on_key(&mut self, event: &gpui::KeyDownEvent, window: &mut Window, cx: &mut Context<Self>) {
        if let Some(key) = self.num_edit {
            match event.keystroke.key.as_str() {
                "escape" => {
                    self.num_edit = None;
                    cx.notify();
                    return;
                }
                "enter" => {
                    if let Ok(parsed) = self.num_text.trim().parse::<i64>() {
                        let (min, max) = self.neural_range(key);
                        self.settings.set_int(key, parsed.clamp(min, max));
                        self.persist();
                    }
                    self.num_edit = None;
                    cx.notify();
                    return;
                }
                "backspace" => {
                    if self.num_fresh {
                        self.num_text.clear();
                        self.num_fresh = false;
                    } else {
                        self.num_text.pop();
                    }
                    cx.notify();
                    return;
                }
                _ => {
                    if let Some(ch) = event.keystroke.key_char.as_ref() {
                        if !ch.is_empty()
                            && ch.chars().all(|c| c.is_ascii_digit())
                            && self.num_text.len() < 4
                        {
                            if self.num_fresh {
                                self.num_text.clear();
                                self.num_fresh = false;
                            }
                            self.num_text.push_str(ch);
                            cx.notify();
                        }
                    }
                    return;
                }
            }
        }
        let Some(key) = self.editing else {
            if event.keystroke.modifiers.control {
                match event.keystroke.key.as_str() {
                    "1" | "2" | "3" | "4" | "5" | "6" | "7" | "8" | "9" | "0" => {
                        // 1..9 pick the first nine visible tabs, 0 picks the last.
                        let index = if event.keystroke.key == "0" {
                            crate::app::Tab::ALL.len() - 1
                        } else {
                            event
                                .keystroke
                                .key
                                .parse::<usize>()
                                .unwrap_or(1)
                                .saturating_sub(1)
                        };
                        let visible: Vec<crate::app::Tab> = crate::app::Tab::ALL
                            .iter()
                            .copied()
                            .filter(|tab| {
                                *tab != crate::app::Tab::Advanced || self.advanced
                            })
                            .collect();
                        let index = if event.keystroke.key == "0" {
                            visible.len().saturating_sub(1)
                        } else {
                            index
                        };
                        if let Some(tab) = visible.get(index).copied() {
                            self.tab = tab;
                            cx.notify();
                        }
                        return;
                    }
                    "enter" => {
                        self.action_connect(cx);
                        return;
                    }
                    _ => {}
                }
            }
            if event.keystroke.key == "f10" && self.status.streaming && self.picker.is_none() {
                self.toggle_overlay(cx);
                return;
            }
            if event.keystroke.key == "escape" {
                if self.picker.is_some() {
                    self.picker = None;
                    cx.notify();
                } else {
                    self.editing = None;
                }
            }
            return;
        };
        let stroke = &event.keystroke;
        match stroke.key.as_str() {
            "escape" | "enter" | "tab" => {
                self.editing = None;
            }
            "backspace" => {
                let mut text = self.settings.s(key, "");
                text.pop();
                self.settings.set_str(key, &text);
                self.persist();
            }
            "space" => {
                let mut text = self.settings.s(key, "");
                text.push(' ');
                self.settings.set_str(key, &text);
                self.persist();
            }
            _ => {
                if stroke.modifiers.control || stroke.modifiers.platform {
                    return;
                }
                if let Some(ch) = stroke.key_char.as_ref() {
                    if !ch.is_empty() && ch.chars().all(|c| !c.is_control()) {
                        let mut text = self.settings.s(key, "");
                        text.push_str(ch);
                        self.settings.set_str(key, &text);
                        self.persist();
                    }
                }
            }
        }
        let _ = window;
        cx.notify();
    }
}
