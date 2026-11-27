//! Animated aurora backdrop.
//!
//! GPUI has no blur pass, so each blob is a stack of concentric translucent
//! circles: the alpha of every ring is solved from a smoothstep coverage curve,
//! so the rim melts into the backdrop the way an out-of-focus disc does and
//! costs only quads on the GPU. The whole layer is skipped when the user
//! disables it or asks for reduced motion, and it never runs while the stream
//! window has focus (the shell is minimized then anyway).

use std::time::Duration;

use gpui::{div, prelude::*, px, relative, rgba, Animation, AnimationExt as _, AnyElement, Div};

use crate::theme::{self, BG_DEEP};

#[derive(Clone, Copy)]
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

fn blobs() -> [Blob; 6] {
    let p = theme::palette();
    [
    Blob {
        color: p.blobs[0],
        size: 780.0,
        x: -180.0,
        y: -220.0,
        alpha: 54.0,
        phase: 0.0,
        seconds: 34.0,
        drift: 46.0,
    },
    Blob {
        color: p.blobs[1],
        size: 640.0,
        x: 900.0,
        y: -160.0,
        alpha: 42.0,
        phase: 1.7,
        seconds: 41.0,
        drift: 38.0,
    },
    Blob {
        color: p.blobs[2],
        size: 560.0,
        x: 520.0,
        y: 560.0,
        alpha: 22.0,
        phase: 3.1,
        seconds: 47.0,
        drift: 30.0,
    },
    Blob {
        color: p.blobs[3],
        size: 520.0,
        x: -120.0,
        y: 520.0,
        alpha: 20.0,
        phase: 4.6,
        seconds: 39.0,
        drift: 34.0,
    },
    Blob {
        color: p.blobs[4],
        size: 420.0,
        x: 760.0,
        y: 260.0,
        alpha: 10.0,
        phase: 5.9,
        seconds: 29.0,
        drift: 26.0,
    },
    // Very large, very weak wash: fills the middle so the background reads as
    // one soft field instead of separate circles.
    Blob {
        color: p.blobs[5],
        size: 1500.0,
        x: 120.0,
        y: -140.0,
        alpha: 10.0,
        phase: 2.2,
        seconds: 58.0,
        drift: 18.0,
    },
    ]
}

fn blob_stack(blob: &Blob) -> Div {
    let mut stack = div()
        .absolute()
        .left(px(blob.x))
        .top(px(blob.y))
        .w(px(blob.size))
        .h(px(blob.size));

    // Out-of-focus disc. The rings are not a linear ramp any more: their
    // per-ring alpha is solved from a smoothstep coverage curve, so the rim
    // melts into the backdrop over half the radius instead of showing a
    // stack of hard concentric edges. That soft shoulder is what the eye
    // reads as blur.
    const STEPS: usize = 44;
    const SHOULDER: f32 = 0.58;
    let peak = (blob.alpha as f32 / 255.0) * 3.6;
    let mut prev = 0.0f32;
    for step in 0..STEPS {
        let s = (step + 1) as f32 / STEPS as f32;
        let t = (s / SHOULDER).clamp(0.0, 1.0);
        let coverage = peak * t * t * (3.0 - 2.0 * t);
        let alpha = ((coverage - prev) / (1.0 - prev)).clamp(0.0, 1.0);
        prev = coverage;
        if alpha <= 0.003 {
            continue;
        }
        let scale = 1.0 - (1.0 - s) * 0.985;
        let inner = blob.size * scale;
        let offset = (blob.size - inner) * 0.5;
        let value = (alpha * 255.0).round().max(1.0) as u32;
        stack = stack.child(
            div()
                .absolute()
                .left(px(offset))
                .top(px(offset))
                .w(px(inner))
                .h(px(inner))
                .rounded(px(inner * 0.5))
                .bg(rgba((blob.color << 8) | value)),
        );
    }

    stack
}

fn blobs_animated(blob: Blob) -> AnyElement {
    let base_x = blob.x;
    let base_y = blob.y;
    let drift = blob.drift;
    let phase = blob.phase;
    blob_stack(&blob)
        .with_animation(
            ("aurora", (blob.phase * 10.0) as u32),
            Animation::new(Duration::from_secs_f32(blob.seconds)).repeat(),
            move |el, t| {
                let angle = t * std::f32::consts::TAU + phase;
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

    let palette_blobs = blobs();
    if animated && !reduce_motion {
        for blob in palette_blobs {
            root = root.child(blobs_animated(blob));
        }
    } else {
        for blob in palette_blobs.iter() {
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
