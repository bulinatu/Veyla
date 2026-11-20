#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace brp::next {

// A separate top-level layered window. It never enters the video swapchain and
// never takes focus; it paints with GDI only when the text changes.
class OverlayWindow {
public:
    OverlayWindow() = default;
    ~OverlayWindow();

    OverlayWindow(const OverlayWindow&) = delete;
    OverlayWindow& operator=(const OverlayWindow&) = delete;

    bool Create();
    void Destroy();
    void Update(const std::vector<std::wstring>& lines, float opacity, int position);
    void Hide();

    bool visible() const { return visible_; }

private:
    static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT msg, WPARAM wparam,
                                         LPARAM lparam);
    LRESULT WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
    void Paint(HDC dc) const;
    void Reposition(int position);

    HWND hwnd_ = nullptr;
    std::vector<std::wstring> lines_;
    bool visible_ = false;
    float opacity_ = 0.85f;
    int position_ = 1;
    int last_x_ = -1;
    int last_y_ = -1;
    BYTE last_alpha_ = 0;
};

}  // namespace brp::next
