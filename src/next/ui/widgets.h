#pragma once

#include "ui/theme.h"

#include <d2d1.h>
#include <dwrite.h>

#include <string>
#include <vector>

namespace brp::ui {
class Renderer;
}

namespace brp::next {

struct WidgetInput {
    float x = 0.0f;
    float y = 0.0f;
    bool down = false;
    bool clicked = false;
};

// Immediate-mode widgets driven by the design tokens. No per-page styling.
class Widgets {
public:
    Widgets(brp::ui::Renderer& renderer, const ThemeTokens& theme);

    void Begin(const WidgetInput& input, double now);
    void End();

    bool Button(int id, const D2D1_RECT_F& rect, const wchar_t* label,
                bool primary = true, bool enabled = true);
    bool Choice(int id, const D2D1_RECT_F& rect, const wchar_t* label,
                const std::vector<std::wstring>& items, int& index, bool enabled = true,
                const std::vector<bool>* item_enabled = nullptr);
    bool Toggle(int id, const D2D1_RECT_F& rect, const wchar_t* label, bool& value,
                bool enabled = true);
    bool Slider(int id, const D2D1_RECT_F& rect, const wchar_t* label, float& value,
                float minimum, float maximum, float step,
                const wchar_t* format, bool enabled = true);
    bool NavItem(int id, const D2D1_RECT_F& rect, const wchar_t* label, bool selected,
                 int text_left = 14, bool enabled = true);
    // Invisible click target for custom-drawn controls (e.g. the PIN field).
    bool HitTarget(int id, const D2D1_RECT_F& rect, bool enabled = true);

    void Card(const D2D1_RECT_F& rect);
    void Section(const D2D1_RECT_F& rect, const wchar_t* label);
    void Label(const D2D1_RECT_F& rect, const wchar_t* text, const Color& color,
               float size = 14.0f, DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL,
               DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING);
    void StatusPill(const D2D1_RECT_F& rect, const wchar_t* text, const Color& color,
                    bool filled = false);
    void Divider(float x0, float x1, float y);

    bool hovered() const { return hovered_; }
    bool interactable_hovered() const { return interactable_hovered_; }
    const ThemeTokens& Tokens() const { return theme_; }

private:
    bool Hit(const D2D1_RECT_F& rect) const;
    bool Hover(const D2D1_RECT_F& rect) const;
    bool Pressed(int id, const D2D1_RECT_F& rect, bool enabled);

    brp::ui::Renderer& renderer_;
    const ThemeTokens& theme_;
    WidgetInput input_{};
    double now_ = 0.0;
    int active_ = -1;
    bool hovered_ = false;
    bool interactable_hovered_ = false;
};

D2D1_COLOR_F ToD2D(const Color& color);
D2D1_RECT_F Rect(float left, float top, float right, float bottom);

}  // namespace brp::next
