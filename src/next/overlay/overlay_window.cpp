#include "overlay/overlay_window.h"

#include <algorithm>
#include <cwchar>

namespace brp::next {
namespace {

constexpr wchar_t kClass[] = L"BetterRemotePlayNextOverlay";
constexpr int kWidth = 390;
constexpr int kHeight = 128;
constexpr int kMargin = 22;

std::wstring Widen(const char* text) {
    if (!text) {
        return {};
    }
    const int size = ::MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (size <= 1) {
        return {};
    }
    std::wstring out(static_cast<std::size_t>(size - 1), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text, -1, out.data(), size);
    return out;
}

}  // namespace

OverlayWindow::~OverlayWindow() { Destroy(); }

bool OverlayWindow::Create() {
    if (hwnd_) {
        return true;
    }
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &OverlayWindow::WndProcThunk;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kClass;
    ::RegisterClassExW(&wc);

    hwnd_ = ::CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_NOACTIVATE |
            WS_EX_TOOLWINDOW,
        kClass, L"BetterRemotePlay overlay", WS_POPUP, 0, 0, kWidth, kHeight, nullptr,
        nullptr, wc.hInstance, this);
    if (!hwnd_) {
        return false;
    }
    last_alpha_ = static_cast<BYTE>(std::clamp(opacity_, 0.1f, 1.0f) * 255);
    ::SetLayeredWindowAttributes(hwnd_, RGB(0, 0, 0), last_alpha_,
                                 LWA_ALPHA | LWA_COLORKEY);
    return true;
}

void OverlayWindow::Destroy() {
    if (hwnd_) {
        ::DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    visible_ = false;
}

void OverlayWindow::Raise() {
    if (!hwnd_ || !visible_) {
        return;
    }
    ::SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

void OverlayWindow::Hide() {
    if (hwnd_) {
        ::ShowWindow(hwnd_, SW_HIDE);
    }
    visible_ = false;
}

void OverlayWindow::Update(const std::vector<std::wstring>& lines, float opacity,
                           int position) {
    if (!Create()) {
        return;
    }
    lines_ = lines;
    opacity_ = std::clamp(opacity, 0.1f, 1.0f);
    position_ = std::clamp(position, 0, 3);
    const BYTE alpha = static_cast<BYTE>(opacity_ * 255);
    if (alpha != last_alpha_) {
        last_alpha_ = alpha;
        ::SetLayeredWindowAttributes(hwnd_, RGB(0, 0, 0), alpha,
                                     LWA_ALPHA | LWA_COLORKEY);
    }
    Reposition(position_);
    if (!visible_) {
        ::ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        visible_ = true;
    }
    ::InvalidateRect(hwnd_, nullptr, FALSE);
}

void OverlayWindow::Reposition(int position) {
    RECT work{};
    ::SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int width = work.right - work.left;
    const int height = work.bottom - work.top;
    int x = work.left + kMargin;
    int y = work.top + kMargin;
    if ((position & 1) != 0) {
        x = work.left + width - kWidth - kMargin;
    }
    if (position >= 2) {
        y = work.top + height - kHeight - kMargin;
    }
    if (x == last_x_ && y == last_y_) {
        return;
    }
    last_x_ = x;
    last_y_ = y;
    ::SetWindowPos(hwnd_, HWND_TOPMOST, x, y, kWidth, kHeight,
                   SWP_NOACTIVATE);
}

LRESULT CALLBACK OverlayWindow::WndProcThunk(HWND hwnd, UINT msg, WPARAM wparam,
                                             LPARAM lparam) {
    if (msg == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        auto* self = static_cast<OverlayWindow*>(create->lpCreateParams);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    auto* self = reinterpret_cast<OverlayWindow*>(
        ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self) {
        return self->WndProc(hwnd, msg, wparam, lparam);
    }
    return ::DefWindowProcW(hwnd, msg, wparam, lparam);
}

LRESULT OverlayWindow::WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC dc = ::BeginPaint(hwnd, &paint);
            Paint(dc);
            ::EndPaint(hwnd, &paint);
            return 0;
        }
        case WM_NCHITTEST:
            return HTTRANSPARENT;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        default:
            return ::DefWindowProcW(hwnd, msg, wparam, lparam);
    }
}

void OverlayWindow::Paint(HDC dc) const {
    RECT client{};
    ::GetClientRect(hwnd_, &client);
    ::FillRect(dc, &client, static_cast<HBRUSH>(::GetStockObject(BLACK_BRUSH)));

    HBRUSH panel = ::CreateSolidBrush(RGB(20, 16, 30));
    HPEN border = ::CreatePen(PS_SOLID, 1, RGB(176, 114, 255));
    HGDIOBJ old_brush = ::SelectObject(dc, panel);
    HGDIOBJ old_pen = ::SelectObject(dc, border);
    ::RoundRect(dc, 1, 1, client.right - 1, client.bottom - 1, 14, 14);
    ::SelectObject(dc, old_brush);
    ::SelectObject(dc, old_pen);
    ::DeleteObject(panel);
    ::DeleteObject(border);

    ::SetBkMode(dc, TRANSPARENT);
    HFONT font = ::CreateFontW(-14, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                               CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HGDIOBJ old_font = ::SelectObject(dc, font);
    ::SetTextColor(dc, RGB(247, 243, 255));
    ::TextOutW(dc, 16, 12, L"BetterRemotePlay", 16);
    ::SetTextColor(dc, RGB(255, 107, 214));
    ::TextOutW(dc, 148, 12, L"LIVE", 4);

    int y = 38;
    for (const auto& line : lines_) {
        if (y > client.bottom - 20) {
            break;
        }
        ::SetTextColor(dc, RGB(211, 200, 230));
        ::TextOutW(dc, 16, y, line.c_str(), static_cast<int>(line.size()));
        y += 20;
    }
    if (lines_.empty()) {
        ::SetTextColor(dc, RGB(154, 140, 180));
        ::TextOutW(dc, 16, 38, L"Waiting for stream metrics...", 29);
    }
    ::SelectObject(dc, old_font);
    ::DeleteObject(font);
}

}  // namespace brp::next
