#pragma once

#include <windows.h>

namespace brp {

struct MouseState {
    int x = 0;
    int y = 0;
    bool down = false;
    bool clicked = false;     // edge, consumed by the frame
    float wheel = 0.0f;       // accumulated notches since last consume
};

// Thin Win32 window wrapper. Deliberately dumb: it owns a window and a message
// pump, nothing else. The renderer never knows Win32 exists beyond a HWND.
class Win32Window {
public:
    Win32Window() = default;
    ~Win32Window();

    Win32Window(const Win32Window&) = delete;
    Win32Window& operator=(const Win32Window&) = delete;

    bool Create(const wchar_t* title, int width, int height);

    // Pumps messages. Returns false once the window is closing.
    bool Pump();

    HWND hwnd() const { return hwnd_; }
    int width() const { return width_; }
    int height() const { return height_; }

    // True once after a client-area resize; clears the flag.
    bool ConsumeResize();

    // Last WM_KEYDOWN virtual-key code, or 0. Consumes the value.
    int ConsumeKey();

    // Last WM_CHAR character, or 0. Consumes the value.
    int ConsumeChar();

    const MouseState& mouse() const { return mouse_; }
    bool ConsumeClick();
    float ConsumeWheel();

    // Hand cursor while hovering something clickable.
    void set_hand_cursor(bool enabled) { hand_cursor_ = enabled; }

private:
    static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
    LRESULT WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

    HWND hwnd_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    bool quit_ = false;
    bool resized_ = false;
    int last_key_ = 0;
    int last_char_ = 0;
    MouseState mouse_{};
    bool hand_cursor_ = false;
    bool tracking_leave_ = false;
};

}  // namespace brp
