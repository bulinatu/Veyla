//! Visual language for the shell.
//!
//! Near-black base, soft glass panels, one accent colour. The accent (buttons,
//! sliders, active dock slot, chips) and the aurora blobs come from one of six
//! palettes selected in Settings > Appearance; everything else stays neutral so
//! the themes differ in mood without moving the layout.

use gpui::{px, rgb, rgba, Rgba, FontWeight, Pixels};
use std::sync::atomic::{AtomicUsize, Ordering};

pub const UI_FONT: &str = "Inter";
pub const MONO_FONT: &str = "JetBrains Mono";
/// Windows icon font for the dock glyphs. Shipped with Windows 10 and 11.
pub const ICON_FONT: &str = "Segoe MDL2 Assets";

// Base surfaces (theme independent).
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

// Fixed status colours.
pub const GREEN: u32 = 0x34D399;
pub const AMBER: u32 = 0xFBBF24;
pub const RED: u32 = 0xF87171;

/// One selectable colour scheme.
pub struct Palette {
    pub accent: u32,
    pub accent_bright: u32,
    pub accent_deep: u32,
    pub pink: u32,
    pub cyan: u32,
    /// Aurora blob colours, back to front.
    pub blobs: [u32; 6],
}

pub const PALETTES: [Palette; 6] = [
    Palette {
        accent: 0x8B5CF6,
        accent_bright: 0xA78BFA,
        accent_deep: 0x4F46E5,
        pink: 0xF472B6,
        cyan: 0x22D3EE,
        blobs: [0x7C3AED, 0x4F46E5, 0xDB2777, 0x0891B2, 0xA855F7, 0x6D28D9],
    },
    Palette {
        accent: 0x3B82F6,
        accent_bright: 0x60A5FA,
        accent_deep: 0x1D4ED8,
        pink: 0x38BDF8,
        cyan: 0x22D3EE,
        blobs: [0x1D4ED8, 0x0369A1, 0x0EA5E9, 0x22D3EE, 0x2563EB, 0x1E3A8A],
    },
    Palette {
        accent: 0x10B981,
        accent_bright: 0x34D399,
        accent_deep: 0x047857,
        pink: 0x5EEAD4,
        cyan: 0x22D3EE,
        blobs: [0x059669, 0x047857, 0x0D9488, 0x22D3EE, 0x34D399, 0x065F46],
    },
    Palette {
        accent: 0xF97316,
        accent_bright: 0xFB923C,
        accent_deep: 0xC2410C,
        pink: 0xF87171,
        cyan: 0xFBBF24,
        blobs: [0xEA580C, 0xC2410C, 0xDC2626, 0xF59E0B, 0xFB923C, 0x9A3412],
    },
    Palette {
        accent: 0xEC4899,
        accent_bright: 0xF472B6,
        accent_deep: 0xBE185D,
        pink: 0xF9A8D4,
        cyan: 0xFB7185,
        blobs: [0xDB2777, 0xBE185D, 0xE11D48, 0xF472B6, 0xEC4899, 0x831843],
    },
    Palette {
        accent: 0x64748B,
        accent_bright: 0xCBD5E1,
        accent_deep: 0x334155,
        pink: 0x94A3B8,
        cyan: 0x38BDF8,
        blobs: [0x334155, 0x1E293B, 0x475569, 0x0EA5E9, 0x64748B, 0x0F172A],
    },
];

pub const PALETTE_NAMES: [&str; 6] = [
    "Aurora", "Ocean", "Emerald", "Ember", "Rose", "Graphite",
];

static PALETTE_INDEX: AtomicUsize = AtomicUsize::new(0);

pub fn set_palette(index: usize) {
    PALETTE_INDEX.store(index.min(PALETTES.len() - 1), Ordering::Relaxed);
}

pub fn palette_index() -> usize {
    PALETTE_INDEX.load(Ordering::Relaxed)
}

pub fn palette() -> &'static Palette {
    &PALETTES[palette_index()]
}

pub fn accent_u32() -> u32 {
    palette().accent
}

pub fn accent_bright_u32() -> u32 {
    palette().accent_bright
}

pub fn pink_u32() -> u32 {
    palette().pink
}

pub fn cyan_u32() -> u32 {
    palette().cyan
}

fn with_alpha(color: u32, alpha: u8) -> Rgba {
    rgba((color << 8) | alpha as u32)
}

pub fn accent_packed(alpha: u8) -> u32 {
    (palette().accent << 8) | alpha as u32
}

pub fn accent_bright_packed(alpha: u8) -> u32 {
    (palette().accent_bright << 8) | alpha as u32
}

pub fn accent_rgba(alpha: u8) -> Rgba {
    with_alpha(palette().accent, alpha)
}

pub fn accent_bright_rgba(alpha: u8) -> Rgba {
    with_alpha(palette().accent_bright, alpha)
}

pub fn accent_deep_rgba(alpha: u8) -> Rgba {
    with_alpha(palette().accent_deep, alpha)
}

pub fn pink_rgba(alpha: u8) -> Rgba {
    with_alpha(palette().pink, alpha)
}

pub fn cyan_rgba(alpha: u8) -> Rgba {
    with_alpha(palette().cyan, alpha)
}

// Type scale. Deliberately larger than the old shell: 15px body, 13.5 small.
// The whole scale follows the "Text size" setting (75-150%).
use std::sync::atomic::AtomicU32;

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

pub fn panel() -> Rgba {
    rgba(PANEL)
}

pub fn border() -> Rgba {
    rgba(BORDER)
}

pub fn accent() -> Rgba {
    rgb(palette().accent)
}

pub fn text() -> Rgba {
    rgb(TEXT)
}

pub fn dim() -> Rgba {
    rgb(TEXT_DIM)
}

pub fn muted() -> Rgba {
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
