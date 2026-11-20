#pragma once

#include "core/types.h"

#include <cstdint>

namespace brp::next {

struct Color {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    std::uint8_t a = 255;
};

// Design tokens are the only place colours, spacing and type sizes are
// defined. Screens and components consume these; they never hardcode values.
struct ThemeTokens {
    Color background;
    Color background_bottom;
    Color surface;
    Color surface_alt;
    Color surface_hover;
    Color border;
    Color border_strong;
    Color text;
    Color text_dim;
    Color text_faint;
    Color accent;
    Color accent_hover;
    Color accent_warm;
    Color accent_text;
    Color success;
    Color warning;
    Color error;

    int space_xs = 4;
    int space_sm = 8;
    int space_md = 12;
    int space_lg = 18;
    int space_xl = 28;
    int space_2xl = 40;

    int radius_sm = 6;
    int radius_md = 10;
    int radius_lg = 14;
    int radius_pill = 999;

    int font_caption = 11;
    int font_small = 12;
    int font_body = 14;
    int font_subtitle = 16;
    int font_title = 20;
    int font_display = 28;

    int sidebar_width = 236;
    int sidebar_collapsed = 72;
};

const ThemeTokens& ThemeFor(Theme theme);

}  // namespace brp::next
