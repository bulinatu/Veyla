#pragma once

#include "overlay/overlay_view.h"

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

namespace brp::next {

// Independent top-level overlay. Compact mode is a click-through stats box;
// control-center mode becomes interactive (mouse + forwarded keyboard) and
// emits OverlayAction values instead of touching the engine directly.
class OverlayWindow {
public:
    using ActionHandler = std::function<void(OverlayAction)>;

    OverlayWindow() = default;
    ~OverlayWindow();

    OverlayWindow(const OverlayWindow&) = delete;
    OverlayWindow& operator=(const OverlayWindow&) = delete;

    void SetActionHandler(ActionHandler handler) { handler_ = std::move(handler); }

    bool Create();
    void Destroy();
    void Update(const OverlayView& view, float opacity, int position, int side, float scale,
                HWND anchor);
    void Raise();
    void Hide();
    void Key(int virtual_key);
    void SetTab(int tab);

    bool visible() const { return visible_; }
    bool interactive() const { return view_.expanded; }

private:
    static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT msg, WPARAM wparam,
                                         LPARAM lparam);
    LRESULT WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
    void Paint(HDC dc);
    void PaintCompact(HDC dc, const RECT& client);
    void PaintExpanded(HDC dc, const RECT& client);
    void Reposition(int position, int side);
    void SetInteractive(bool interactive);
    void Activate(OverlayAction action);
    int HitTest(int x, int y) const;

    HWND hwnd_ = nullptr;
    ActionHandler handler_;
    OverlayView view_;
    bool visible_ = false;
    bool interactive_state_ = false;
    float opacity_ = 0.85f;
    int position_ = 1;
    int side_ = 1;
    float scale_ = 1.0f;
    int width_ = 0;
    int height_ = 0;
    int last_x_ = -1;
    int last_y_ = -1;
    HWND anchor_ = nullptr;
    int selected_ = 0;
    int tab_ = 0;
    int hover_ = -1;
    BYTE last_alpha_ = 0;

    struct Hit {
        RECT rect{};
        OverlayAction action = OverlayAction::None;
        int row = -1;
        bool tab = false;
    };
    std::vector<Hit> hits_;
};

}  // namespace brp::next
