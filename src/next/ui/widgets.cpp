#include "ui/widgets.h"

#include "ui/ui_renderer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwchar>

namespace brp::next {
namespace {

Color Mix(const Color& a, const Color& b, float t) {
    const float k = std::clamp(t, 0.0f, 1.0f);
    return Color{
        static_cast<std::uint8_t>(a.r + (b.r - a.r) * k),
        static_cast<std::uint8_t>(a.g + (b.g - a.g) * k),
        static_cast<std::uint8_t>(a.b + (b.b - a.b) * k),
        static_cast<std::uint8_t>(a.a + (b.a - a.a) * k),
    };
}

Color WithAlpha(const Color& c, float alpha) {
    return Color{c.r, c.g, c.b, static_cast<std::uint8_t>(std::clamp(alpha, 0.0f, 1.0f) * 255)};
}

float Width(const D2D1_RECT_F& r) { return r.right - r.left; }
float Height(const D2D1_RECT_F& r) { return r.bottom - r.top; }

}  // namespace

D2D1_COLOR_F ToD2D(const Color& color) {
    return D2D1::ColorF(color.r / 255.0f, color.g / 255.0f, color.b / 255.0f,
                        color.a / 255.0f);
}

D2D1_RECT_F Rect(float left, float top, float right, float bottom) {
    return D2D1::RectF(left, top, right, bottom);
}

Widgets::Widgets(brp::ui::Renderer& renderer, const ThemeTokens& theme)
    : renderer_(renderer), theme_(theme) {}

void Widgets::Begin(const WidgetInput& input, double now) {
    input_ = input;
    now_ = now;
    hovered_ = false;
    interactable_hovered_ = false;
    if (!input_.down) {
        active_ = -1;
    }
}

void Widgets::End() {}

bool Widgets::Hit(const D2D1_RECT_F& rect) const {
    return input_.x >= rect.left && input_.x <= rect.right && input_.y >= rect.top &&
           input_.y <= rect.bottom;
}

bool Widgets::Hover(const D2D1_RECT_F& rect) const { return Hit(rect); }

bool Widgets::Pressed(int id, const D2D1_RECT_F& rect, bool enabled) {
    if (!enabled || !Hit(rect)) {
        return false;
    }
    interactable_hovered_ = true;
    if (input_.down) {
        active_ = id;
    }
    return input_.clicked;
}

bool Widgets::Button(int id, const D2D1_RECT_F& rect, const wchar_t* label, bool primary,
                     bool enabled) {
    const bool hover = enabled && Hover(rect);
    hovered_ = hovered_ || hover;
    const bool clicked = Pressed(id, rect, enabled);
    Color fill = primary ? theme_.accent : theme_.surface_alt;
    Color text = primary ? theme_.accent_text : theme_.text;
    if (!enabled) {
        fill = theme_.surface_alt;
        text = theme_.text_faint;
    } else if (hover) {
        fill = primary ? theme_.accent_hover : theme_.surface_hover;
    }
    renderer_.FillRounded(rect, static_cast<float>(theme_.radius_md), ToD2D(fill));
    if (!primary && enabled) {
        renderer_.StrokeRounded(rect, static_cast<float>(theme_.radius_md),
                               ToD2D(theme_.border), 1.0f);
    }
    renderer_.Text(label,
                   Rect(rect.left + 12, rect.top, rect.right - 12, rect.bottom),
                   static_cast<float>(theme_.font_body), ToD2D(text),
                   DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
    return clicked;
}

bool Widgets::Choice(int id, const D2D1_RECT_F& rect, const wchar_t* label,
                     const std::vector<std::wstring>& items, int& index, bool enabled) {
    hovered_ = hovered_ || Hover(rect);
    const float label_width = 300.0f;
    renderer_.Text(label, Rect(rect.left, rect.top, rect.left + label_width, rect.bottom),
                   static_cast<float>(theme_.font_body),
                   ToD2D(enabled ? theme_.text : theme_.text_faint),
                   DWRITE_FONT_WEIGHT_MEDIUM);

    const float gap = 6.0f;
    float x = rect.left + label_width;
    const float right = rect.right;
    bool changed = false;
    for (std::size_t i = 0; i < items.size(); ++i) {
        const float text_width = renderer_.TextWidth(
            items[i].c_str(), static_cast<float>(theme_.font_small),
            DWRITE_FONT_WEIGHT_MEDIUM);
        const float width = std::max(58.0f, text_width + 24.0f);
        if (x + width > right) {
            break;
        }
        const D2D1_RECT_F pill = Rect(x, rect.top + 6.0f, x + width, rect.bottom - 6.0f);
        const bool selected = static_cast<int>(i) == index;
        const bool hover = enabled && Hover(pill);
        hovered_ = hovered_ || hover;
        interactable_hovered_ = interactable_hovered_ || hover;
        Color fill = selected ? theme_.accent : theme_.surface_alt;
        Color text = selected ? theme_.accent_text : theme_.text_dim;
        if (!enabled) {
            fill = theme_.surface_alt;
            text = theme_.text_faint;
        } else if (hover && !selected) {
            fill = theme_.surface_hover;
            text = theme_.text;
        }
        renderer_.FillRounded(pill, static_cast<float>(theme_.radius_sm), ToD2D(fill));
        if (!selected) {
            renderer_.StrokeRounded(pill, static_cast<float>(theme_.radius_sm),
                                   ToD2D(theme_.border), 1.0f);
        }
        renderer_.Text(items[i].c_str(),
                       Rect(pill.left + 8, pill.top, pill.right - 8, pill.bottom),
                       static_cast<float>(theme_.font_small), ToD2D(text),
                       DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
        if (hover && input_.clicked) {
            index = static_cast<int>(i);
            changed = true;
        }
        x += width + gap;
    }
    (void)id;
    return changed;
}

bool Widgets::Toggle(int id, const D2D1_RECT_F& rect, const wchar_t* label, bool& value,
                     bool enabled) {
    hovered_ = hovered_ || Hover(rect);
    renderer_.Text(label, Rect(rect.left, rect.top, rect.right - 70, rect.bottom),
                   static_cast<float>(theme_.font_body),
                   ToD2D(enabled ? theme_.text : theme_.text_faint),
                   DWRITE_FONT_WEIGHT_MEDIUM);
    const float switch_w = 46.0f;
    const float switch_h = 24.0f;
    const D2D1_RECT_F switch_rect =
        Rect(rect.right - switch_w, (rect.top + rect.bottom) * 0.5f - switch_h * 0.5f,
             rect.right, (rect.top + rect.bottom) * 0.5f + switch_h * 0.5f);
    const bool hover = enabled && Hover(switch_rect);
    hovered_ = hovered_ || hover;
    interactable_hovered_ = interactable_hovered_ || hover;
    const bool clicked = Pressed(id, switch_rect, enabled);
    const Color track = value ? theme_.accent : theme_.surface_alt;
    renderer_.FillRounded(switch_rect, switch_h * 0.5f,
                          ToD2D(enabled ? track : WithAlpha(track, 0.4f)));
    if (!value) {
        renderer_.StrokeRounded(switch_rect, switch_h * 0.5f, ToD2D(theme_.border), 1.0f);
    }
    const float knob = switch_h - 6.0f;
    const float knob_x = value ? switch_rect.right - knob - 3.0f : switch_rect.left + 3.0f;
    renderer_.FillCircle(knob_x + knob * 0.5f, switch_rect.top + switch_h * 0.5f,
                         knob * 0.5f, ToD2D(Color{255, 255, 255, 255}));
    if (clicked) {
        value = !value;
        return true;
    }
    return false;
}

bool Widgets::Slider(int id, const D2D1_RECT_F& rect, const wchar_t* label, float& value,
                     float minimum, float maximum, float step, const wchar_t* format,
                     bool enabled) {
    hovered_ = hovered_ || Hover(rect);
    renderer_.Text(label, Rect(rect.left, rect.top, rect.left + 300, rect.bottom),
                   static_cast<float>(theme_.font_body),
                   ToD2D(enabled ? theme_.text : theme_.text_faint),
                   DWRITE_FONT_WEIGHT_MEDIUM);
    const float track_left = rect.left + 310.0f;
    const float track_right = rect.right - 86.0f;
    const float center_y = (rect.top + rect.bottom) * 0.5f;
    const D2D1_RECT_F track = Rect(track_left, center_y - 3.0f, track_right,
                                   center_y + 3.0f);
    const float span = std::max(1.0f, maximum - minimum);
    const float t = std::clamp((value - minimum) / span, 0.0f, 1.0f);
    const bool hover = enabled && (Hover(track) || Hover(rect));
    hovered_ = hovered_ || hover;
    interactable_hovered_ = interactable_hovered_ || hover;

    bool changed = false;
    if (enabled && input_.down && Hover(rect)) {
        active_ = id;
    }
    if (enabled && active_ == id && input_.down) {
        const float x = std::clamp(input_.x, track_left, track_right);
        float next = minimum + (x - track_left) / (track_right - track_left) * span;
        if (step > 0.0f) {
            next = std::round((next - minimum) / step) * step + minimum;
        }
        next = std::clamp(next, minimum, maximum);
        if (std::abs(next - value) > 1e-6f) {
            value = next;
            changed = true;
        }
    }

    renderer_.FillRounded(track, 3.0f, ToD2D(theme_.surface_alt));
    renderer_.FillRounded(Rect(track_left, track.top,
                               track_left + (track_right - track_left) * t, track.bottom),
                          3.0f, ToD2D(enabled ? theme_.accent : theme_.text_faint));
    const float knob_x = track_left + (track_right - track_left) * t;
    renderer_.FillCircle(knob_x, center_y, hover || active_ == id ? 8.0f : 7.0f,
                         ToD2D(enabled ? theme_.text : theme_.text_faint));

    wchar_t value_text[64] = {};
    std::swprintf(value_text, 64, format, value);
    renderer_.Text(value_text, Rect(track_right + 10.0f, rect.top, rect.right, rect.bottom),
                   static_cast<float>(theme_.font_small),
                   ToD2D(enabled ? theme_.text_dim : theme_.text_faint),
                   DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_TEXT_ALIGNMENT_TRAILING);
    return changed;
}

bool Widgets::NavItem(int id, const D2D1_RECT_F& rect, const wchar_t* label,
                      bool selected, int text_left, bool enabled) {
    const bool hover = enabled && Hover(rect);
    hovered_ = hovered_ || hover;
    interactable_hovered_ = interactable_hovered_ || hover;
    const bool clicked = Pressed(id, rect, enabled);
    if (selected) {
        renderer_.FillRounded(rect, static_cast<float>(theme_.radius_md),
                              ToD2D(WithAlpha(theme_.accent, 0.14f)));
        renderer_.FillRounded(Rect(rect.left, rect.top + 8, rect.left + 3, rect.bottom - 8),
                              1.5f, ToD2D(theme_.accent));
    } else if (hover) {
        renderer_.FillRounded(rect, static_cast<float>(theme_.radius_md),
                              ToD2D(theme_.surface_hover));
    }
    renderer_.Text(label, Rect(rect.left + static_cast<float>(text_left), rect.top,
                               rect.right - 8, rect.bottom),
                   static_cast<float>(theme_.font_body),
                   ToD2D(enabled ? (selected ? theme_.text : theme_.text_dim)
                                 : theme_.text_faint),
                   selected ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL);
    return clicked;
}

void Widgets::Card(const D2D1_RECT_F& rect) {    renderer_.FillRounded(rect, static_cast<float>(theme_.radius_lg),
                          ToD2D(theme_.surface));
    renderer_.StrokeRounded(rect, static_cast<float>(theme_.radius_lg),
                           ToD2D(theme_.border), 1.0f);
}

void Widgets::Section(const D2D1_RECT_F& rect, const wchar_t* label) {
    renderer_.Text(label, rect, static_cast<float>(theme_.font_caption),
                   ToD2D(theme_.text_faint), DWRITE_FONT_WEIGHT_SEMI_BOLD);
}

void Widgets::Label(const D2D1_RECT_F& rect, const wchar_t* text, const Color& color,
                    float size, DWRITE_FONT_WEIGHT weight, DWRITE_TEXT_ALIGNMENT align) {
    renderer_.Text(text, rect, size, ToD2D(color), weight, align);
}

void Widgets::StatusPill(const D2D1_RECT_F& rect, const wchar_t* text, const Color& color,
                         bool filled) {
    if (filled) {
        renderer_.FillRounded(rect, static_cast<float>(theme_.radius_pill),
                              ToD2D(WithAlpha(color, 0.16f)));
        renderer_.StrokeRounded(rect, static_cast<float>(theme_.radius_pill),
                               ToD2D(WithAlpha(color, 0.45f)), 1.0f);
    }
    renderer_.Text(text, rect, static_cast<float>(theme_.font_caption), ToD2D(color),
                   DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
}

void Widgets::Divider(float x0, float x1, float y) {
    renderer_.FillRect(Rect(x0, y, x1, y + 1.0f), ToD2D(theme_.border));
}

}  // namespace brp::next
