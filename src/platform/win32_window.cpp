#include "platform/win32_window.h"

#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cstdlib>

namespace brp {
namespace {

constexpr wchar_t kClassName[] = L"BetterRemotePlayWindow";

// The app draws its own title bar, so the client area covers the entire window.
// WS_OVERLAPPEDWINDOW is still the window style: it keeps native resize, snap,
// minimize/maximize and taskbar behaviour, while WM_NCCALCSIZE removes the
// non-client frame.
constexpr int kResizeBorderDip = 6;
constexpr int kMinWidthDip = 900;
constexpr int kMinHeightDip = 600;
constexpr int kCornerRadiusDip = 9;

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWA_BORDER_COLOR
#define DWMWA_BORDER_COLOR 34
#endif

// DWMWCP_* values and DWMWA_COLOR_NONE from dwmapi.h.
constexpr int kDwmCornerDoNotRound = 1;
constexpr int kDwmCornerRoundSmall = 3;
constexpr DWORD kDwmColorNone = 0xFFFFFFFE;

int DipToPixels(HWND hwnd, int dip) {
    const UINT dpi = hwnd ? ::GetDpiForWindow(hwnd) : 96;
    return ::MulDiv(dip, static_cast<int>(dpi), 96);
}

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
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = &Win32Window::WndProcThunk;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    if (!RegisterClassExW(&wc)) {
        return false;
    }

    // No AdjustWindowRect: WM_NCCALCSIZE makes the client equal to the window,
    // so the requested size is the client size from the first frame on.
    hwnd_ = CreateWindowExW(0, kClassName, title, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                            CW_USEDEFAULT, width, height, nullptr, nullptr, instance, this);
    if (!hwnd_) {
        return false;
    }

    hidden_test_ = std::getenv("BRP_HIDDEN_TEST") != nullptr;
    if (hidden_test_) {
        ::SetWindowPos(hwnd_, nullptr, -32000, -32000, width, height,
                       SWP_NOZORDER | SWP_NOACTIVATE);
    }

    ApplyWindowAppearance();
    // Force the first WM_NCCALCSIZE before reading the client size; without
    // this, Windows reports the framed client (1584x861) for one frame.
    ::SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    UpdateRefreshRate();
    monitor_ = ::MonitorFromWindow(hwnd_, MONITOR_DEFAULTTOPRIMARY);

    RECT client{};
    GetClientRect(hwnd_, &client);
    width_ = client.right - client.left;
    height_ = client.bottom - client.top;

    // Deliberately hidden: App::Frame shows the window after the first frame
    // (the intro) has been presented.
    return true;
}

void Win32Window::Show() {
    if (!hwnd_ || hidden_test_) {
        return;
    }
    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);
}

void Win32Window::SetFrameMetrics(int caption_height, int caption_client_width) {
    caption_height_ = std::max(0, caption_height);
    caption_client_width_ = std::max(0, caption_client_width);
}

void Win32Window::SetFullscreenChrome(bool fullscreen) {
    if (fullscreen_chrome_ == fullscreen) {
        return;
    }
    fullscreen_chrome_ = fullscreen;
    ApplyWindowAppearance();
}

void Win32Window::ToggleFullscreen() {
    if (fullscreen_) {
        ExitFullscreen();
    } else {
        EnterFullscreen();
    }
}

void Win32Window::EnterFullscreen() {
    if (!hwnd_ || fullscreen_) {
        return;
    }
    saved_style_ = static_cast<LONG>(::GetWindowLongPtrW(hwnd_, GWL_STYLE));
    saved_ex_style_ = static_cast<LONG>(::GetWindowLongPtrW(hwnd_, GWL_EXSTYLE));
    saved_placement_.length = sizeof(saved_placement_);
    if (!::GetWindowPlacement(hwnd_, &saved_placement_)) {
        saved_placement_.length = 0;
    }

    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    if (!::GetMonitorInfoW(::MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST),
                           &monitor)) {
        return;
    }

    fullscreen_ = true;
    SetFullscreenChrome(true);
    ::SetWindowLongPtrW(hwnd_, GWL_STYLE,
                        (saved_style_ & ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX |
                                          WS_MAXIMIZEBOX | WS_SYSMENU)) |
                            WS_POPUP | WS_VISIBLE | WS_CLIPSIBLINGS);
    ::SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, saved_ex_style_);
    ::SetWindowPos(hwnd_, HWND_TOP, monitor.rcMonitor.left, monitor.rcMonitor.top,
                   monitor.rcMonitor.right - monitor.rcMonitor.left,
                   monitor.rcMonitor.bottom - monitor.rcMonitor.top,
                   SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    ApplyWindowAppearance();
    ::SetForegroundWindow(hwnd_);

    RECT client{};
    ::GetClientRect(hwnd_, &client);
    width_ = client.right - client.left;
    height_ = client.bottom - client.top;
    resized_ = true;
}

void Win32Window::ExitFullscreen() {
    if (!hwnd_ || !fullscreen_) {
        return;
    }
    fullscreen_ = false;
    SetFullscreenChrome(false);
    ::SetWindowLongPtrW(hwnd_, GWL_STYLE, saved_style_);
    ::SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, saved_ex_style_);
    if (saved_placement_.length == sizeof(saved_placement_)) {
        ::SetWindowPlacement(hwnd_, &saved_placement_);
    }
    ::SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    ApplyWindowAppearance();

    RECT client{};
    ::GetClientRect(hwnd_, &client);
    width_ = client.right - client.left;
    height_ = client.bottom - client.top;
    resized_ = true;
}

void Win32Window::Minimize() {
    if (hwnd_) {
        ShowWindow(hwnd_, SW_MINIMIZE);
    }
}

void Win32Window::ToggleMaximize() {
    if (!hwnd_) {
        return;
    }
    ShowWindow(hwnd_, IsZoomed(hwnd_) ? SW_RESTORE : SW_MAXIMIZE);
    ApplyWindowAppearance();
}

void Win32Window::Close() {
    if (hwnd_) {
        PostMessageW(hwnd_, WM_CLOSE, 0, 0);
    }
}

bool Win32Window::maximized() const {
    return hwnd_ && IsZoomed(hwnd_) != FALSE;
}

void Win32Window::ApplyWindowAppearance() {
    if (!hwnd_) {
        return;
    }

    // Dark DWM treatment with no visible border line; the app paints the frame.
    BOOL dark = TRUE;
    ::DwmSetWindowAttribute(hwnd_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    DWORD border = kDwmColorNone;
    ::DwmSetWindowAttribute(hwnd_, DWMWA_BORDER_COLOR, &border, sizeof(border));

    // Windowed: small radius. Maximized or fullscreen: square, so the stream
    // reaches the monitor edges.
    const int preference =
        (fullscreen_chrome_ || maximized()) ? kDwmCornerDoNotRound : kDwmCornerRoundSmall;
    const HRESULT hr =
        ::DwmSetWindowAttribute(hwnd_, DWMWA_WINDOW_CORNER_PREFERENCE, &preference,
                                sizeof(preference));
    dwm_corners_ = SUCCEEDED(hr);

    if (!dwm_corners_) {
        // Windows 10 fallback: clip the window to a rounded region. There is no
        // DWM corner API there, so this is the only way to get the same look.
        RECT rect{};
        ::GetWindowRect(hwnd_, &rect);
        const int w = rect.right - rect.left;
        const int h = rect.bottom - rect.top;
        if (!fullscreen_chrome_ && !maximized() && w > 0 && h > 0) {
            const int radius = DipToPixels(hwnd_, kCornerRadiusDip);
            HRGN region = ::CreateRoundRectRgn(0, 0, w + 1, h + 1, radius, radius);
            ::SetWindowRgn(hwnd_, region, TRUE);
        } else {
            ::SetWindowRgn(hwnd_, nullptr, TRUE);
        }
    }
}

void Win32Window::UpdateRefreshRate() {
    int hz = 0;

    MONITORINFOEXW monitor{};
    monitor.cbSize = sizeof(monitor);
    const HMONITOR handle = ::MonitorFromWindow(hwnd_, MONITOR_DEFAULTTOPRIMARY);
    if (::GetMonitorInfoW(handle, &monitor)) {
        DEVMODEW mode{};
        mode.dmSize = sizeof(mode);
        if (::EnumDisplaySettingsW(monitor.szDevice, ENUM_CURRENT_SETTINGS, &mode)) {
            hz = static_cast<int>(mode.dmDisplayFrequency);
        }
    }

    if (hz <= 1 && hwnd_) {
        HDC dc = ::GetDC(hwnd_);
        if (dc) {
            hz = ::GetDeviceCaps(dc, VREFRESH);
            ::ReleaseDC(hwnd_, dc);
        }
    }

    refresh_hz_ = std::clamp(hz > 1 ? hz : 60, 24, 500);
}

void Win32Window::UpdateMonitor() {
    if (!hwnd_) {
        return;
    }
    const HMONITOR monitor = ::MonitorFromWindow(hwnd_, MONITOR_DEFAULTTOPRIMARY);
    if (monitor != monitor_) {
        monitor_ = monitor;
        UpdateRefreshRate();
    }
}

void Win32Window::SetCursorHidden(bool hidden) {
    if (cursor_hidden_ == hidden) {
        return;
    }
    cursor_hidden_ = hidden;
    if (hidden) {
        ::SetCursor(nullptr);
    } else {
        ::SetCursor(::LoadCursorW(nullptr, hand_cursor_ ? IDC_HAND : IDC_ARROW));
    }
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

        case WM_NCCALCSIZE:
            if (wparam == TRUE) {
                // The app owns the whole window. When maximized, clamp the
                // client to the monitor work area so the taskbar stays visible
                // and the client is not pushed off-screen by the frame.
                auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lparam);
                if (IsZoomed(hwnd)) {
                    MONITORINFO monitor{};
                    monitor.cbSize = sizeof(monitor);
                    if (::GetMonitorInfoW(::MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST),
                                          &monitor)) {
                        params->rgrc[0] = monitor.rcWork;
                    }
                }
                return 0;
            }
            break;

        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
            info->ptMinTrackSize.x = DipToPixels(hwnd, kMinWidthDip);
            info->ptMinTrackSize.y = DipToPixels(hwnd, kMinHeightDip);

            MONITORINFO monitor{};
            monitor.cbSize = sizeof(monitor);
            if (::GetMonitorInfoW(::MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor)) {
                info->ptMaxPosition.x = monitor.rcWork.left - monitor.rcMonitor.left;
                info->ptMaxPosition.y = monitor.rcWork.top - monitor.rcMonitor.top;
                info->ptMaxSize.x = monitor.rcWork.right - monitor.rcWork.left;
                info->ptMaxSize.y = monitor.rcWork.bottom - monitor.rcWork.top;
            }
            return 0;
        }

        case WM_NCHITTEST: {
            if (fullscreen_chrome_) {
                return HTCLIENT;
            }

            RECT window{};
            ::GetWindowRect(hwnd, &window);
            const int x = GET_X_LPARAM(lparam) - window.left;
            const int y = GET_Y_LPARAM(lparam) - window.top;
            const int w = window.right - window.left;
            const int h = window.bottom - window.top;

            if (!IsZoomed(hwnd)) {
                const int border = std::max(4, DipToPixels(hwnd, kResizeBorderDip));
                const bool left = x < border;
                const bool right = x >= w - border;
                const bool top = y < border;
                const bool bottom = y >= h - border;
                if (top && left) return HTTOPLEFT;
                if (top && right) return HTTOPRIGHT;
                if (bottom && left) return HTBOTTOMLEFT;
                if (bottom && right) return HTBOTTOMRIGHT;
                if (left) return HTLEFT;
                if (right) return HTRIGHT;
                if (top) return HTTOP;
                if (bottom) return HTBOTTOM;
            }

            // Everything below the caption is client area. The right strip is
            // excluded so the app's buttons/dropdowns receive their clicks.
            if (y < caption_height_ && x < w - caption_client_width_) {
                return HTCAPTION;
            }
            return HTCLIENT;
        }

        case WM_NCACTIVATE:
            return TRUE;

        case WM_NCPAINT:
            return 0;

        case WM_SIZE: {
            ApplyWindowAppearance();
            UpdateMonitor();
            if (wparam == SIZE_MINIMIZED) {
                width_ = 0;
                height_ = 0;
                return 0;
            }
            const int w = LOWORD(lparam);
            const int h = HIWORD(lparam);
            if (w > 0 && h > 0) {
                width_ = w;
                height_ = h;
                resized_ = true;
            }
            return 0;
        }

        case WM_DPICHANGED: {
            if (fullscreen_) {
                return 0;
            }
            const RECT* suggested = reinterpret_cast<const RECT*>(lparam);
            ::SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                           suggested->right - suggested->left,
                           suggested->bottom - suggested->top,
                           SWP_NOZORDER | SWP_NOACTIVATE);
            ApplyWindowAppearance();
            UpdateMonitor();
            return 0;
        }

        case WM_MOVE:
            UpdateMonitor();
            break;

        case WM_DISPLAYCHANGE:
            UpdateRefreshRate();
            ApplyWindowAppearance();
            return 0;

        case WM_SETTINGCHANGE:
            // SPI_SETWORKAREA: the taskbar moved; re-clamp the maximized size.
            if (wparam == SPI_SETWORKAREA) {
                ApplyWindowAppearance();
                return 0;
            }
            break;

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
                if (cursor_hidden_) {
                    ::SetCursor(nullptr);
                } else {
                    ::SetCursor(::LoadCursorW(nullptr, hand_cursor_ ? IDC_HAND : IDC_ARROW));
                }
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
