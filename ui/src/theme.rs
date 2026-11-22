//! Visual language for the new shell.
//!
//! PS5-adjacent violet identity: near-black base, soft glass panels, violet as
//! the single accent, cyan/pink only as ambient light in the background.

use gpui::{px, rgb, rgba, FontWeight, Pixels};

pub const UI_FONT: &str = "Inter";
pub const MONO_FONT: &str = "JetBrains Mono";

// Base surfaces.
pub const BG: u32 = 0x07070D;
pub const BG_DEEP: u32 = 0x050509;
pub const PANEL: u32 = 0x0C0C16E6;
pub const PANEL_STRONG: u32 = 0x161622E6;
pub const PANEL_HOVER: u32 = 0xFFFFFF14;
pub const FIELD: u32 = 0x00000059;
pub const BORDER: u32 = 0xFFFFFF14;
pub const BORDER_SOFT: u32 = 0xFFFFFF0A;

// Text.
pub const TEXT: u32 = 0xF4F4FA;
pub const TEXT_DIM: u32 = 0xA6A6BD;
pub const TEXT_MUTED: u32 = 0x6C6C85;

// Accents.
pub const ACCENT: u32 = 0x8B5CF6;
pub const ACCENT_BRIGHT: u32 = 0xA78BFA;
pub const ACCENT_SOFT: u32 = 0x8B5CF624;
pub const CYAN: u32 = 0x22D3EE;
pub const PINK: u32 = 0xF472B6;
pub const GREEN: u32 = 0x34D399;
pub const AMBER: u32 = 0xFBBF24;
pub const RED: u32 = 0xF87171;

// Type scale. Deliberately larger than the old shell: 15px body, 13.5 small.
// The whole scale follows the "Text size" setting (75-150%).
use std::sync::atomic::{AtomicU32, Ordering};

static TEXT_SCALE: AtomicU32 = AtomicU32::new(100);

pub fn set_text_scale(percent: i64) {
    TEXT_SCALE.store(percent.clamp(75, 150) as u32, Ordering::Relaxed);
}

pub fn sp(value: f32) -> Pixels {
    px(value * TEXT_SCALE.load(Ordering::Relaxed) as f32 / 100.0)
}

pub fn hero() -> Pixels { sp(40.0) }
pub fn h1() -> Pixels { sp(26.0) }
pub fn h2() -> Pixels { sp(20.0) }
pub fn h3() -> Pixels { sp(16.5) }
pub fn body() -> Pixels { sp(15.0) }
pub fn small() -> Pixels { sp(13.5) }
pub fn tiny() -> Pixels { sp(11.5) }
pub fn mono() -> Pixels { sp(13.0) }

pub const RADIUS: Pixels = px(16.0);
pub const RADIUS_SM: Pixels = px(10.0);

pub fn weight_semibold() -> FontWeight {
    FontWeight::SEMIBOLD
}

pub fn weight_medium() -> FontWeight {
    FontWeight::MEDIUM
}

pub fn panel() -> gpui::Rgba {
    rgba(PANEL)
}

pub fn border() -> gpui::Rgba {
    rgba(BORDER)
}

pub fn accent() -> gpui::Rgba {
    rgb(ACCENT)
}

pub fn text() -> gpui::Rgba {
    rgb(TEXT)
}

pub fn dim() -> gpui::Rgba {
    rgb(TEXT_DIM)
}

pub fn muted() -> gpui::Rgba {
    rgb(TEXT_MUTED)
}

/// Backdrop for an aurora blob: several concentric rings approximate a soft
/// radial falloff without a blur pass.
pub fn blob_layers(color: u32, strength: f32) -> [(u32, u32); 5] {
    let alpha = (strength * 255.0) as u32;
    [
        (color, alpha),
        (color, alpha * 2 / 3),
        (color, alpha / 2),
        (color, alpha / 3),
        (color, alpha / 5),
    ]
}
