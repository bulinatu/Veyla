//! Small styled building blocks shared by every page.

use gpui::{
    div, hsla, point, prelude::*, px, rgba, rgb, BoxShadow, Div, FontWeight, InteractiveElement,
    IntoElement, SharedString, StatefulInteractiveElement,
};
use gpui::{AnyElement, App, ClickEvent, Window};

use crate::theme;

pub fn card() -> Div {
    div()
        .rounded(px(18.0))
        .border_1()
        .border_color(rgba(0xFFFFFF12))
        .bg(gpui::linear_gradient(
            165.0,
            gpui::linear_color_stop(rgba(0x16161FF2), 0.0),
            gpui::linear_color_stop(rgba(0x090910F7), 1.0),
        ))
        // The shadow is a real GPU gaussian pass. A wide, weak shadow plus a
        // violet halo melts the card edge into the aurora instead of cutting a
        // hard rectangle out of it.
        .shadow(vec![
            BoxShadow {
                color: hsla(0.0, 0.0, 0.0, 0.45),
                offset: point(px(0.0), px(14.0)),
                blur_radius: px(40.0),
                spread_radius: px(-10.0),
            },
            BoxShadow {
                color: hsla(0.72, 0.72, 0.62, 0.06),
                offset: point(px(0.0), px(0.0)),
                blur_radius: px(60.0),
                spread_radius: px(-18.0),
            },
        ])
}

pub fn card_pad() -> Div {
    card().p(px(20.0))
}

pub fn divider() -> AnyElement {
    div().h(px(1.0)).w_full().bg(rgba(0xFFFFFF0F)).into_any_element()
}

pub fn page_title(title: &'static str, subtitle: &'static str) -> AnyElement {
    div()
        .flex()
        .flex_col()
        .gap(px(6.0))
        .child(
            div()
                .text_size(theme::h1())
                .font_weight(FontWeight::SEMIBOLD)
                .text_color(theme::text())
                .child(title),
        )
        .child(
            div()
                .text_size(theme::small())
                .text_color(theme::dim())
                .child(subtitle),
        )
        .into_any_element()
}

pub fn section(title: &'static str, hint: &'static str) -> AnyElement {
    div()
        .flex()
        .items_center()
        .justify_between()
        .gap(px(12.0))
        .child(
            div()
                .flex()
                .items_center()
                .gap(px(10.0))
                .child(
                    div()
                        .text_size(theme::tiny())
                        .font_weight(FontWeight::SEMIBOLD)
                        .text_color(theme::accent())
                        .child(title.to_uppercase()),
                )
                .child(div().h(px(1.0)).w(px(28.0)).bg(rgba(0x8B5CF640))),
        )
        .child(
            div()
                .text_size(theme::tiny())
                .text_color(theme::muted())
                .child(hint),
        )
        .into_any_element()
}

/// One settings row: label + explanation on the left, control on the right.
pub fn row(label: impl Into<SharedString>, hint: impl Into<SharedString>, control: AnyElement) -> AnyElement {
    div()
        .flex()
        .items_center()
        .justify_between()
        .gap(px(24.0))
        .py(px(12.0))
        .child(
            div()
                .flex()
                .flex_col()
                .gap(px(3.0))
                .flex_1()
                .min_w(px(0.0))
                .child(
                    div()
                        .text_size(theme::body())
                        .font_weight(FontWeight::MEDIUM)
                        .text_color(theme::text())
                        .child(label.into()),
                )
                .child(
                    div()
                        .text_size(theme::small())
                        .text_color(theme::muted())
                        .child(hint.into()),
                ),
        )
        .child(div().flex_none().child(control))
        .into_any_element()
}

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum Tone {
    Primary,
    Ghost,
    Danger,
    Subtle,
}

pub fn button(
    label: impl Into<SharedString>,
    tone: Tone,
    on_click: impl Fn(&ClickEvent, &mut Window, &mut App) + 'static,
) -> AnyElement {
    let label: SharedString = label.into();
    let element_id = SharedString::from(format!("btn-{label}"));
    let (bg, fg, border) = match tone {
        Tone::Primary => (0x8B5CF6FF, 0xFFFFFFFF, 0x00000000),
        Tone::Ghost => (0xFFFFFF0F, theme::TEXT, theme::BORDER),
        Tone::Danger => (0xF871711F, 0xF87171, 0xF8717155),
        Tone::Subtle => (0x00000000, theme::TEXT_DIM, 0x00000000),
    };
    let hover = match tone {
        Tone::Primary => 0x9D74FFFF,
        Tone::Ghost => 0xFFFFFF1A,
        Tone::Danger => 0xF8717133,
        Tone::Subtle => 0xFFFFFF0F,
    };
    div()
        .id(element_id)
        .flex()
        .items_center()
        .justify_center()
        .gap(px(8.0))
        .h(px(40.0))
        .px(px(18.0))
        .rounded(px(11.0))
        .bg(rgba(bg))
        .when(border != 0, |el| el.border_1().border_color(rgba(border)))
        .text_size(theme::small())
        .font_weight(FontWeight::SEMIBOLD)
        .text_color(rgb(fg))
        .cursor_pointer()
        .hover(move |s| s.bg(rgba(hover)))
        .active(|s| s.opacity(0.82))
        .on_click(on_click)
        .child(label)
        .into_any_element()
}

pub fn switch(
    id: impl Into<SharedString>,
    on: bool,
    on_click: impl Fn(&ClickEvent, &mut Window, &mut App) + 'static,
) -> AnyElement {
    div()
        .id(id.into())
        .w(px(46.0))
        .h(px(26.0))
        .rounded(px(13.0))
        .bg(if on { rgba(0x8B5CF6FF) } else { rgba(0xFFFFFF1F) })
        .cursor_pointer()
        .hover(move |s| {
            if on {
                s.bg(rgba(0x9D74FFFF))
            } else {
                s.bg(rgba(0xFFFFFF2A))
            }
        })
        .on_click(on_click)
        .child(
            div()
                .w(px(20.0))
                .h(px(20.0))
                .mt(px(3.0))
                .ml(px(if on { 23.0 } else { 3.0 }))
                .rounded(px(10.0))
                .bg(rgba(0xFFFFFFFF))
                .when(on, |el| el.shadow_sm()),
        )
        .into_any_element()
}

pub fn chip(label: impl Into<SharedString>, color: u32) -> AnyElement {
    div()
        .flex()
        .items_center()
        .h(px(24.0))
        .px(px(10.0))
        .rounded(px(6.0))
        .bg(rgba((color << 8) | 0x22))
        .border_1()
        .border_color(rgba((color << 8) | 0x44))
        .text_size(theme::tiny())
        .font_weight(FontWeight::MEDIUM)
        .text_color(rgb(color))
        .child(label.into())
        .into_any_element()
}

pub fn dot(color: u32) -> AnyElement {
    div()
        .w(px(7.0))
        .h(px(7.0))
        .rounded(px(4.0))
        .bg(rgb(color))
        .into_any_element()
}

pub fn stat_tile(label: &'static str, value: impl Into<SharedString>, color: u32) -> AnyElement {
    div()
        .flex()
        .flex_col()
        .gap(px(6.0))
        .flex_1()
        .min_w(px(0.0))
        .p(px(14.0))
        .rounded(px(13.0))
        .bg(rgba(0xFFFFFF08))
        .border_1()
        .border_color(rgba(0xFFFFFF0D))
        .child(
            div()
                .text_size(theme::tiny())
                .text_color(theme::muted())
                .child(label.to_uppercase()),
        )
        .child(
            div()
                .font_family(theme::MONO_FONT)
                .text_size(px(17.0))
                .font_weight(FontWeight::MEDIUM)
                .text_color(rgb(color))
                .child(value.into()),
        )
        .into_any_element()
}

pub fn kv(label: &'static str, value: impl Into<SharedString>) -> AnyElement {
    div()
        .flex()
        .items_center()
        .justify_between()
        .py(px(6.0))
        .child(
            div()
                .text_size(theme::small())
                .text_color(theme::muted())
                .child(label),
        )
        .child(
            div()
                .font_family(theme::MONO_FONT)
                .text_size(theme::mono())
                .text_color(theme::dim())
                .child(value.into()),
        )
        .into_any_element()
}

pub fn logo_mark(size: f32) -> AnyElement {
    div()
        .w(px(size))
        .h(px(size))
        .rounded(px(size * 0.28))
        .bg(gpui::linear_gradient(
            140.0,
            gpui::linear_color_stop(rgba(0x8B5CF6FF), 0.0),
            gpui::linear_color_stop(rgba(0x4F46E5FF), 1.0),
        ))
        .flex()
        .items_center()
        .justify_center()
        .shadow_lg()
        .child(
            div()
                .text_size(px(size * 0.46))
                .font_weight(FontWeight::BOLD)
                .text_color(rgba(0xFFFFFFFF))
                .child("B"),
        )
        .into_any_element()
}
