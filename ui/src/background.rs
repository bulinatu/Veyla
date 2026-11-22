//! Animated aurora backdrop.
//!
//! GPUI has no blur pass, so each blob is a stack of concentric translucent
//! circles: the falloff reads as a soft glow and costs only quads on the GPU.
//! The whole layer is skipped when the user disables it or asks for reduced
//! motion, and it never runs while the stream window has focus (the shell is
//! minimized then anyway).

use std::time::Duration;

use gpui::{div, prelude::*, px, relative, rgba, Animation, AnimationExt as _, AnyElement, Div};

use crate::theme::{self, BG_DEEP};

struct Blob {
    color: u32,
    size: f32,
    x: f32,
    y: f32,
    alpha: f32,
    phase: f32,
    seconds: f32,
    drift: f32,
}

const BLOBS: [Blob; 5] = [
    Blob {
        color: 0x7C3AED,
        size: 780.0,
        x: -180.0,
        y: -220.0,
        alpha: 78.0,
        phase: 0.0,
        seconds: 34.0,
        drift: 46.0,
    },
    Blob {
        color: 0x4F46E5,
        size: 640.0,
        x: 900.0,
        y: -160.0,
        alpha: 62.0,
        phase: 1.7,
        seconds: 41.0,
        drift: 38.0,
    },
    Blob {
        color: 0xDB2777,
        size: 560.0,
        x: 520.0,
        y: 560.0,
        alpha: 34.0,
        phase: 3.1,
        seconds: 47.0,
        drift: 30.0,
    },
    Blob {
        color: 0x0891B2,
        size: 520.0,
        x: -120.0,
        y: 520.0,
        alpha: 30.0,
        phase: 4.6,
        seconds: 39.0,
        drift: 34.0,
    },
    Blob {
        color: 0xA855F7,
        size: 420.0,
        x: 760.0,
        y: 260.0,
        alpha: 26.0,
        phase: 5.9,
        seconds: 29.0,
        drift: 26.0,
    },
];

fn blob_stack(blob: &Blob) -> Div {
    let mut stack = div()
        .absolute()
        .left(px(blob.x))
        .top(px(blob.y))
        .w(px(blob.size))
        .h(px(blob.size));

    for step in 0..8 {
        let scale = 1.0 - step as f32 * 0.085;
        let inner = blob.size * scale;
        let offset = (blob.size - inner) * 0.5;
        let alpha = (blob.alpha * (1.0 - step as f32 * 0.105)).max(2.0) as u32;
        stack = stack.child(
            div()
                .absolute()
                .left(px(offset))
                .top(px(offset))
                .w(px(inner))
                .h(px(inner))
                .rounded(px(inner * 0.5))
                .bg(rgba((blob.color << 8) | alpha)),
        );
    }

    stack
}

fn animated_blob(blob: &'static Blob) -> AnyElement {
    let base_x = blob.x;
    let base_y = blob.y;
    let drift = blob.drift;
    blob_stack(blob)
        .with_animation(
            ("aurora", (blob.phase * 10.0) as u32),
            Animation::new(Duration::from_secs_f32(blob.seconds)).repeat(),
            move |el, t| {
                let angle = t * std::f32::consts::TAU + blob.phase;
                el.left(px(base_x + angle.cos() * drift))
                    .top(px(base_y + (angle * 1.3).sin() * drift * 0.8))
            },
        )
        .into_any_element()
}

pub fn aurora(animated: bool, reduce_motion: bool) -> AnyElement {
    let mut root = div()
        .absolute()
        .size_full()
        .overflow_hidden()
        .bg(rgba(0x050509FF));

    // Base wash: a deep violet-to-black gradient gives the black surfaces
    // something to sit on instead of flat #000.
    root = root.child(
        div().absolute().size_full().bg(gpui::linear_gradient(
            155.0,
            gpui::linear_color_stop(rgba(0x160B33FF), 0.0),
            gpui::linear_color_stop(rgba(0x050509FF), 1.0),
        )),
    );

    if animated && !reduce_motion {
        for blob in BLOBS.iter() {
            root = root.child(animated_blob(blob));
        }
    } else {
        for blob in BLOBS.iter() {
            root = root.child(blob_stack(blob));
        }
    }

    // Readability scrim: dark at the bottom, clear at the top.
    root = root.child(
        div().absolute().size_full().bg(gpui::linear_gradient(
            180.0,
            gpui::linear_color_stop(rgba(0x07070D00), 0.35),
            gpui::linear_color_stop(rgba(BG_DEEP << 8 | 0xF2), 1.0),
        )),
    );

    // Fine top highlight, like a glass edge.
    root = root.child(
        div()
            .absolute()
            .top(px(0.0))
            .left(px(0.0))
            .right(px(0.0))
            .h(px(1.0))
            .bg(rgba(0xFFFFFF12)),
    );

    let _ = relative;
    let _ = theme::body();
    root.into_any_element()
}
