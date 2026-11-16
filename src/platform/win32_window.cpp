#include "platform/win32_window.h"

#include <windowsx.h>

namespace brp {
namespace {
constexpr wchar_t kClassName[] = L"BetterRemotePlayWindow";
}  // namespace

Win32Window::~Win32Window() {
    if (hwnd_ && IsWindow(hwnd_)) {
        SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
        DestroyWindow(hwnd_);
    }
    UnregisterClassW(kClassName, GetModuleHandleW(nullptr));
}

bool Win32Window::Create(const wchar_t* title, int width, int height) {
    HINSTANCE instance = GetModuleHandleW(nullptr);

    // Per-monitor DPI so the UI is drawn in real pixels on every display.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &Win32Window::WndProcThunk;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    if (!RegisterClassExW(&wc)) {
        return false;
    }

    RECT rc{0, 0, width, height};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);

    hwnd_ = CreateWindowExW(0, kClassName, title, WS_OVERLAPPEDWINDOW,
                            CW_USEDEFAULT, CW_USEDEFAULT,
                            rc.right - rc.left, rc.bottom - rc.top,
                            nullptr, nullptr, instance, this);
    if (!hwnd_) {
        return false;
    }

    RECT client{};
    GetClientRect(hwnd_, &client);
    width_ = client.right - client.left;
    height_ = client.bottom - client.top;

    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);
    return true;
}

bool Win32Window::Pump() {
    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            quit_ = true;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return !quit_;
}

bool Win32Window::ConsumeResize() {
    bool r = resized_;
    resized_ = false;
    return r;
}

int Win32Window::ConsumeKey() {
    int k = last_key_;
    last_key_ = 0;
    return k;
}

int Win32Window::ConsumeChar() {
    int c = last_char_;
    last_char_ = 0;
    return c;
}

bool Win32Window::ConsumeClick() {
    const bool clicked = mouse_.clicked;
    mouse_.clicked = false;
    return clicked;
}

float Win32Window::ConsumeWheel() {
    const float wheel = mouse_.wheel;
    mouse_.wheel = 0.0f;
    return wheel;
}

LRESULT CALLBACK Win32Window::WndProcThunk(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    auto* self = reinterpret_cast<Win32Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self) {
        return self->WndProc(hwnd, msg, wparam, lparam);
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

LRESULT Win32Window::WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            quit_ = true;
            hwnd_ = nullptr;
            PostQuitMessage(0);
            return 0;

        case WM_SIZE: {
            if (wparam == SIZE_MINIMIZED) {
                width_ = 0;
                height_ = 0;
                return 0;
            }
            int w = LOWORD(lparam);
            int h = HIWORD(lparam);
            if (w > 0 && h > 0) {
                width_ = w;
                height_ = h;
                resized_ = true;
            }
            return 0;
        }

        case WM_KEYDOWN:
            last_key_ = static_cast<int>(wparam);
            return 0;

        case WM_CHAR:
            last_char_ = static_cast<int>(wparam);
            return 0;

        case WM_MOUSEMOVE: {
            mouse_.x = GET_X_LPARAM(lparam);
            mouse_.y = GET_Y_LPARAM(lparam);
            if (!tracking_leave_) {
                TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0};
                ::TrackMouseEvent(&track);
                tracking_leave_ = true;
            }
            return 0;
        }

        case WM_MOUSELEAVE:
            tracking_leave_ = false;
            mouse_.x = -1;
            mouse_.y = -1;
            mouse_.down = false;
            return 0;

        case WM_LBUTTONDOWN:
            mouse_.x = GET_X_LPARAM(lparam);
            mouse_.y = GET_Y_LPARAM(lparam);
            mouse_.down = true;
            mouse_.clicked = true;
            ::SetCapture(hwnd);
            return 0;

        case WM_LBUTTONUP:
            mouse_.down = false;
            if (::GetCapture() == hwnd) {
                ::ReleaseCapture();
            }
            return 0;

        case WM_MOUSEWHEEL:
            mouse_.wheel += static_cast<float>(GET_WHEEL_DELTA_WPARAM(wparam)) / WHEEL_DELTA;
            return 0;

        case WM_SETCURSOR:
            if (LOWORD(lparam) == HTCLIENT) {
                ::SetCursor(::LoadCursorW(nullptr, hand_cursor_ ? IDC_HAND : IDC_ARROW));
                return TRUE;
            }
            break;

        case WM_ERASEBKGND:
            return 1;  // we own the client area

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

}  // namespace brp
