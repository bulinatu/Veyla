#include "overlay/overlay_window.h"

#include <windowsx.h>

#include <algorithm>
#include <cwchar>
#include <cstdio>

namespace brp::next {
namespace {

constexpr wchar_t kClass[] = L"BetterRemotePlayNextOverlay";
constexpr int kCompactW = 400;
constexpr int kCompactH = 150;
constexpr int kExpandedW = 480;
constexpr int kExpandedH = 520;
constexpr int kMargin = 22;

COLORREF ToColor(std::uint32_t rgb) {
    return RGB((rgb >> 16) & 0xFFu, (rgb >> 8) & 0xFFu, rgb & 0xFFu);
}

void FillRound(HDC dc, const RECT& rect, int radius, COLORREF fill, COLORREF border) {
    HBRUSH brush = ::CreateSolidBrush(fill);
    HPEN pen = ::CreatePen(PS_SOLID, 1, border);
    const HGDIOBJ old_brush = ::SelectObject(dc, brush);
    const HGDIOBJ old_pen = ::SelectObject(dc, pen);
    ::RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
    ::SelectObject(dc, old_brush);
    ::SelectObject(dc, old_pen);
    ::DeleteObject(brush);
    ::DeleteObject(pen);
}

void DrawTextLine(HDC dc, const std::wstring& text, const RECT& rect, int size,
                  int weight, COLORREF color, UINT flags = DT_LEFT) {
    HFONT font = ::CreateFontW(-size, 0, 0, 0, weight, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                               CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    const HGDIOBJ old = ::SelectObject(dc, font);
    ::SetTextColor(dc, color);
    RECT target = rect;
    ::DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &target,
                flags | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    ::SelectObject(dc, old);
    ::DeleteObject(font);
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
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        kClass, L"BetterRemotePlay control center", WS_POPUP, 0, 0, kCompactW,
        kCompactH, nullptr, nullptr, wc.hInstance, this);
    if (!hwnd_) {
        std::fprintf(stderr, "[overlay] CreateWindowEx failed %lu\n",
                     static_cast<unsigned long>(::GetLastError()));
        return false;
    }
    width_ = kCompactW;
    height_ = kCompactH;
    last_alpha_ = static_cast<BYTE>(std::clamp(opacity_, 0.1f, 1.0f) * 255);
    ::SetLayeredWindowAttributes(hwnd_, RGB(0, 0, 0), last_alpha_,
                                 LWA_ALPHA | LWA_COLORKEY);
    SetInteractive(false);
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
    hover_ = -1;
}

void OverlayWindow::SetInteractive(bool interactive) {
    if (!hwnd_ || interactive_state_ == interactive) {
        return;
    }
    interactive_state_ = interactive;
    LONG_PTR ex = ::GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
    if (interactive) {
        ex &= ~static_cast<LONG_PTR>(WS_EX_TRANSPARENT);
    } else {
        ex |= WS_EX_TRANSPARENT;
    }
    ::SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, ex);
    ::SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                       SWP_FRAMECHANGED);
}

void OverlayWindow::SetTab(int tab) {
    const int count = static_cast<int>(view_.pages.size());
    tab_ = count > 0 ? std::clamp(tab, 0, count - 1) : 0;
    selected_ = 0;
    if (hwnd_) {
        ::InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

namespace {
bool WorkArea(HWND anchor, RECT& work) {
    work = RECT{};
    if (anchor && ::IsWindow(anchor)) {
        MONITORINFO monitor{};
        monitor.cbSize = sizeof(monitor);
        if (::GetMonitorInfoW(::MonitorFromWindow(anchor, MONITOR_DEFAULTTONEAREST),
                              &monitor)) {
            work = monitor.rcWork;
            return true;
        }
    }
    ::SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    return true;
}
}  // namespace
void OverlayWindow::Update(const OverlayView& view, float opacity, int position,
                           int side, float scale, HWND anchor) {
    anchor_ = anchor;
    if (!Create()) {
        return;
    }
    view_ = view;
    if (tab_ >= static_cast<int>(view_.pages.size())) {
        tab_ = 0;
    }
    opacity_ = std::clamp(opacity, 0.1f, 1.0f);
    position_ = std::clamp(position, 0, 3);
    side_ = std::clamp(side, 0, 1);
    scale_ = std::clamp(scale, 0.75f, 2.0f);

    RECT work{};
    WorkArea(anchor_, work);
    const int work_h = std::max(400, static_cast<int>(work.bottom - work.top));
    const int want_w = static_cast<int>((view_.expanded ? kExpandedW : kCompactW) * scale_);
    const int want_h = view_.expanded
                            ? std::max(static_cast<int>(kExpandedH * scale_),
                                       work_h - static_cast<int>(kMargin * 2 * scale_))
                            : static_cast<int>(kCompactH * scale_);
    if (want_w != width_ || want_h != height_) {
        width_ = want_w;
        height_ = want_h;
        last_x_ = -1;
        last_y_ = -1;
    }
    SetInteractive(view_.expanded);
    const BYTE alpha = static_cast<BYTE>(opacity_ * 255);
    if (alpha != last_alpha_) {
        last_alpha_ = alpha;
        ::SetLayeredWindowAttributes(hwnd_, RGB(0, 0, 0), alpha,
                                     LWA_ALPHA | LWA_COLORKEY);
    }
    Reposition(position_, side_);
    if (!visible_) {
        ::ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        visible_ = true;
    }
    ::InvalidateRect(hwnd_, nullptr, FALSE);
}

void OverlayWindow::Reposition(int position, int side) {
    RECT work{};
    WorkArea(anchor_, work);
    const int area_w = work.right - work.left;
    const int area_h = work.bottom - work.top;
    int x = side == 0 ? work.left + kMargin : work.left + area_w - width_ - kMargin;
    int y = work.top + kMargin;
    if (position == 1) {
        y = work.top + (area_h - height_) / 2;
    } else if (position >= 2) {
        y = work.top + area_h - height_ - kMargin;
    }
    if (x == last_x_ && y == last_y_) {
        return;
    }
    last_x_ = x;
    last_y_ = y;
    ::SetWindowPos(hwnd_, HWND_TOPMOST, x, y, width_, height_, SWP_NOACTIVATE);
}

void OverlayWindow::Activate(OverlayAction action) {
    if (action == OverlayAction::None) {
        return;
    }
    if (action == OverlayAction::Close) {
        Hide();
    }
    if (handler_) {
        handler_(action);
    }
}

void OverlayWindow::Key(int virtual_key) {
    if (!view_.expanded) {
        return;
    }
    const int count = static_cast<int>(view_.pages.size());
    switch (virtual_key) {
        case VK_LEFT:
            SetTab(tab_ - 1);
            break;
        case VK_RIGHT:
        case VK_TAB:
            SetTab(tab_ + 1);
            break;
        case VK_UP:
            selected_ = std::max(0, selected_ - 1);
            break;
        case VK_DOWN:
            if (count > 0) {
                selected_ = std::min(
                    static_cast<int>(view_.pages[tab_].rows.size()) - 1,
                    selected_ + 1);
            }
            break;
        case VK_RETURN:
        case VK_SPACE: {
            if (count == 0 || tab_ >= count) {
                break;
            }
            const auto& rows = view_.pages[tab_].rows;
            if (selected_ < 0 || selected_ >= static_cast<int>(rows.size())) {
                break;
            }
            const OverlayRow& row = rows[selected_];
            if (!row.enabled) {
                break;
            }
            Activate(row.stepper && row.next != OverlayAction::None ? row.next
                                                                   : row.action);
            break;
        }
        case VK_ESCAPE:
            Activate(OverlayAction::Close);
            break;
        case VK_F10:
            Activate(OverlayAction::ToggleControl);
            break;
        case VK_OEM_PLUS:
        case VK_ADD: {
            if (count == 0 || tab_ >= count) break;
            const auto& rows = view_.pages[tab_].rows;
            if (selected_ >= 0 && selected_ < static_cast<int>(rows.size())) {
                Activate(rows[selected_].next);
            }
            break;
        }
        case VK_OEM_MINUS:
        case VK_SUBTRACT: {
            if (count == 0 || tab_ >= count) break;
            const auto& rows = view_.pages[tab_].rows;
            if (selected_ >= 0 && selected_ < static_cast<int>(rows.size())) {
                Activate(rows[selected_].previous);
            }
            break;
        }
        default:
            break;
    }
    if (hwnd_) {
        ::InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

int OverlayWindow::HitTest(int x, int y) const {
    for (int i = 0; i < static_cast<int>(hits_.size()); ++i) {
        const RECT& r = hits_[i].rect;
        if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) {
            return i;
        }
    }
    return -1;
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
            return view_.expanded ? HTCLIENT : HTTRANSPARENT;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_SETCURSOR:
            if (LOWORD(lparam) == HTCLIENT && view_.expanded) {
                POINT p{};
                ::GetCursorPos(&p);
                ::ScreenToClient(hwnd, &p);
                const int hit = HitTest(p.x, p.y);
                ::SetCursor(::LoadCursorW(nullptr, hit >= 0 ? IDC_HAND : IDC_ARROW));
                return TRUE;
            }
            break;
        case WM_MOUSEMOVE: {
            if (!view_.expanded) {
                return 0;
            }
            const int hit = HitTest(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
            if (hit != hover_) {
                hover_ = hit;
                ::InvalidateRect(hwnd, nullptr, FALSE);
            }
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0};
            ::TrackMouseEvent(&track);
            return 0;
        }
        case WM_MOUSELEAVE:
            if (hover_ != -1) {
                hover_ = -1;
                ::InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_LBUTTONUP: {
            if (!view_.expanded) {
                return 0;
            }
            const int x = GET_X_LPARAM(lparam);
            const int y = GET_Y_LPARAM(lparam);
            const int hit = HitTest(x, y);
            if (hit >= 0) {
                const Hit h = hits_[static_cast<std::size_t>(hit)];
                if (h.tab) {
                    SetTab(h.row);
                } else {
                    selected_ = h.row;
                    Activate(h.action);
                }
                ::InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        default:
            return ::DefWindowProcW(hwnd, msg, wparam, lparam);
    }
    return ::DefWindowProcW(hwnd, msg, wparam, lparam);
}

void OverlayWindow::Paint(HDC dc) {
    RECT client{};
    ::GetClientRect(hwnd_, &client);
    ::FillRect(dc, &client, static_cast<HBRUSH>(::GetStockObject(BLACK_BRUSH)));
    hits_.clear();
    ::SetBkMode(dc, TRANSPARENT);
    if (view_.expanded) {
        PaintExpanded(dc, client);
    } else {
        PaintCompact(dc, client);
    }
}

void OverlayWindow::PaintCompact(HDC dc, const RECT& client) {
    const auto& p = view_.palette;
    RECT panel{1, 1, client.right - 1, client.bottom - 1};
    FillRound(dc, panel, 16, ToColor(p.panel), ToColor(p.border));

    const int s = static_cast<int>(scale_ * 10.0f);
    DrawTextLine(dc, view_.compact_title.empty() ? L"BetterRemotePlay"
                                                        : view_.compact_title.c_str(),
                 RECT{16, 8, 300, 32}, std::max(10, 13 * s / 10), FW_SEMIBOLD,
                 ToColor(p.text));
    DrawTextLine(dc, view_.live ? L"LIVE" : L"IDLE",
                 RECT{client.right - 74, 8, client.right - 14, 32},
                 std::max(8, 9 * s / 10), FW_BOLD,
                 ToColor(view_.live ? p.accent_warm : p.text_faint), DT_RIGHT);

    int y = 36;
    if (!view_.subtitle.empty()) {
        DrawTextLine(dc, view_.subtitle, RECT{16, y, client.right - 14, y + 20},
                     std::max(9, 11 * s / 10), FW_NORMAL, ToColor(p.text_dim));
        y += 22;
    }
    if (!view_.pages.empty()) {
        const auto& rows = view_.pages[0].rows;
        int shown = 0;
        for (const auto& row : rows) {
            if (shown >= 3 || y > client.bottom - 40) {
                break;
            }
            DrawTextLine(dc, row.label, RECT{16, y, client.right / 2, y + 18},
                         std::max(8, 10 * s / 10), FW_NORMAL, ToColor(p.text_faint));
            DrawTextLine(dc, row.value,
                         RECT{client.right / 2, y, client.right - 14, y + 18},
                         std::max(8, 10 * s / 10), FW_SEMIBOLD, ToColor(p.text_dim),
                         DT_RIGHT);
            y += 20;
            ++shown;
        }
    } else {
        DrawTextLine(dc, L"Waiting for stream metrics...",
                     RECT{16, y, client.right - 14, y + 18}, 10, FW_NORMAL,
                     ToColor(p.text_faint));
    }
    const wchar_t* compact_footer = view_.compact_title.empty()
                                        ? L"F10  control center"
                                        : (view_.status.empty() ? L"" : view_.status.c_str());
    DrawTextLine(dc, compact_footer,
                 RECT{16, client.bottom - 26, client.right - 14, client.bottom - 8},
                 std::max(7, 8 * s / 10), FW_NORMAL, ToColor(p.text_faint), DT_RIGHT);
}

void OverlayWindow::PaintExpanded(HDC dc, const RECT& client) {
    const auto& p = view_.palette;
    RECT panel{1, 1, client.right - 1, client.bottom - 1};
    FillRound(dc, panel, 18, ToColor(p.background), ToColor(p.border));

    const float sc = scale_;
    auto P = [sc](int v) { return static_cast<int>(v * sc); };
    const int pad = P(16);
    const int header_h = P(52);
    const int tabs_y = header_h;
    const int tabs_h = P(34);
    const int content_y = tabs_y + tabs_h + P(8);
    const int footer_h = P(30);

    DrawTextLine(dc, view_.title, RECT{pad, P(6), client.right - P(190), P(30)},
                 P(17), FW_SEMIBOLD, ToColor(p.text));
    DrawTextLine(dc, view_.subtitle, RECT{pad, P(29), client.right - P(150), P(48)},
                 P(11), FW_NORMAL, ToColor(p.text_dim));
    if (view_.live) {
        DrawTextLine(dc, L"LIVE", RECT{client.right - P(76), P(8), client.right - pad, P(28)},
                     P(9), FW_BOLD, ToColor(p.accent_warm), DT_RIGHT);
    }

    // Tabs.
    const int page_count = static_cast<int>(view_.pages.size());
    const int tabs_x = P(10);
    const int tabs_w = (client.right - tabs_x * 2) / std::max(1, page_count);
    for (int i = 0; i < page_count; ++i) {
        RECT r{tabs_x + tabs_w * i, tabs_y, tabs_x + tabs_w * (i + 1), tabs_y + tabs_h};
        const bool active = i == tab_;
        if (active) {
            RECT bg{r.left + P(3), r.top + P(3), r.right - P(3), r.bottom - P(3)};
            FillRound(dc, bg, P(8), ToColor(p.panel_alt), ToColor(p.panel_alt));
        }
        DrawTextLine(dc, view_.pages[static_cast<std::size_t>(i)].title,
                     RECT{r.left + P(6), r.top, r.right - P(6), r.bottom}, P(11),
                     active ? FW_SEMIBOLD : FW_NORMAL,
                     ToColor(active ? p.text : p.text_faint), DT_CENTER);
        hits_.push_back(Hit{r, OverlayAction::None, i, true});
    }

    const int footer_top = client.bottom - footer_h;
    if (page_count > 0 && tab_ >= 0 && tab_ < page_count) {
        const auto& page = view_.pages[static_cast<std::size_t>(tab_)];
        int y = content_y;
        const int row_h = P(42);
        const int max_rows = std::max(1, (footer_top - content_y - P(4)) / row_h);
        for (int i = 0; i < static_cast<int>(page.rows.size()) && i < max_rows; ++i) {
            const OverlayRow& row = page.rows[static_cast<std::size_t>(i)];
            RECT r{pad, y, client.right - pad, y + row_h - P(4)};
            const bool selected = i == selected_;
            if (selected || hover_ == i) {
                FillRound(dc, r, P(8), ToColor(p.panel_alt), ToColor(p.panel_alt));
            }
            const COLORREF label_color =
                ToColor(row.enabled ? p.text : p.text_faint);
            const int label_right =
                client.right - pad - (row.stepper ? P(126) : (row.toggle ? P(76) : P(18)));
            DrawTextLine(dc, row.label, RECT{r.left + P(10), r.top, label_right, r.top + P(22)},
                         P(12), FW_SEMIBOLD, label_color);
            if (!row.detail.empty()) {
                const bool restart_badge =
                    row.detail.find(L"RESTART") != std::wstring::npos ||
                    row.detail.find(L"NEXT SESSION") != std::wstring::npos;
                DrawTextLine(dc, row.detail,
                             RECT{r.left + P(10), r.top + P(20), label_right, r.top + P(38)},
                             P(9), restart_badge ? FW_BOLD : FW_NORMAL,
                             ToColor(restart_badge ? p.warning : p.text_dim));
            }

            if (row.stepper) {
                const int right = r.right - P(8);
                RECT prev{right - P(112), r.top + P(8), right - P(84), r.top + P(34)};
                RECT next{right - P(32), r.top + P(8), right - P(4), r.top + P(34)};
                FillRound(dc, prev, P(6), ToColor(p.panel), ToColor(p.border));
                FillRound(dc, next, P(6), ToColor(p.panel), ToColor(p.border));
                DrawTextLine(dc, L"<", prev, P(10), FW_BOLD,
                             ToColor(row.enabled ? p.text_dim : p.text_faint), DT_CENTER);
                DrawTextLine(dc, L">", next, P(10), FW_BOLD,
                             ToColor(row.enabled ? p.text_dim : p.text_faint), DT_CENTER);
                DrawTextLine(dc, row.value,
                             RECT{prev.right, r.top, next.left, r.top + P(42)}, P(10),
                             FW_SEMIBOLD, label_color, DT_CENTER);
                if (row.enabled) {
                    hits_.push_back(Hit{prev, row.previous, i, false});
                    hits_.push_back(Hit{next, row.next, i, false});
                }
            } else if (row.toggle) {
                const int right = r.right - P(10);
                RECT pill{right - P(58), r.top + P(9), right, r.top + P(33)};
                const COLORREF fill = row.toggle_value ? ToColor(p.accent)
                                                       : ToColor(p.panel);
                const COLORREF border = row.toggle_value ? ToColor(p.accent)
                                                         : ToColor(p.border);
                FillRound(dc, pill, P(12), fill, border);
                RECT knob{row.toggle_value ? pill.right - P(22) : pill.left + P(2),
                          pill.top + P(2),
                          row.toggle_value ? pill.right - P(2) : pill.left + P(22),
                          pill.bottom - P(2)};
                HBRUSH knob_brush = ::CreateSolidBrush(
                    ToColor(row.toggle_value ? p.background : p.text_faint));
                HGDIOBJ old = ::SelectObject(dc, knob_brush);
                HGDIOBJ old_pen = ::SelectObject(dc, ::GetStockObject(NULL_PEN));
                ::Ellipse(dc, knob.left, knob.top, knob.right, knob.bottom);
                ::SelectObject(dc, old_pen);
                ::SelectObject(dc, old);
                ::DeleteObject(knob_brush);
                if (row.enabled && row.action != OverlayAction::None) {
                    hits_.push_back(Hit{r, row.action, i, false});
                }
            } else {
                DrawTextLine(dc, row.value, RECT{r.left + P(10), r.top, r.right - P(10),
                                                 r.top + P(42)},
                             P(10), FW_SEMIBOLD,
                             ToColor(row.enabled ? p.text_dim : p.text_faint), DT_RIGHT);
                if (row.enabled && row.action != OverlayAction::None) {
                    hits_.push_back(Hit{r, row.action, i, false});
                }
            }
            y += row_h;
        }
    }

    // Footer.
    DrawTextLine(dc, view_.status.empty() ? L"F10 / Esc close  |  arrows select  |  Enter apply" : view_.status,
                 RECT{pad, footer_top, client.right - pad, client.bottom - P(6)}, P(10),
                 FW_NORMAL,
                 ToColor(view_.status_error ? p.error : p.text_faint));
}

}  // namespace brp::next
