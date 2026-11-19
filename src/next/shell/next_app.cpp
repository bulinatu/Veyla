#include "shell/next_app.h"

#include "diagnostics/metrics.h"
#include "media/pipeline.h"
#include "platform/win32_window.h"
#include "shell/settings_controls.h"
#include "ui/ui_renderer.h"

#include <windows.h>
#include <commdlg.h>
#include <mmsystem.h>
#include <shellapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <iterator>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace brp::next {
namespace {

constexpr float kTitleHeight = 40.0f;
constexpr float kSidebar = 236.0f;
constexpr float kSidebarCollapsed = 72.0f;
constexpr float kPad = 28.0f;
constexpr wchar_t kVersion[] = L"0.5.0-next";

std::wstring Widen(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int size = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (size <= 1) {
        return {};
    }
    std::wstring out(static_cast<std::size_t>(size - 1), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, out.data(), size);
    return out;
}

std::wstring PathW(const std::filesystem::path& path) {
    return path.wstring();
}

std::string Narrow(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }
    const int size = ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0,
                                           nullptr, nullptr);
    if (size <= 1) {
        return {};
    }
    std::string out(static_cast<std::size_t>(size - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, out.data(), size, nullptr,
                          nullptr);
    return out;
}

double SecondsNow() {
    using Clock = std::chrono::steady_clock;
    static const Clock::time_point start = Clock::now();
    return std::chrono::duration<double>(Clock::now() - start).count();
}

void DrawNavIcon(brp::ui::Renderer& renderer, Page page, float cx, float cy,
                 const Color& color) {
    const auto c = ToD2D(color);
    const float s = 8.0f;
    switch (page) {
        case Page::Home:
            renderer.Line({cx - s, cy - 1}, {cx, cy - s}, c, 1.4f);
            renderer.Line({cx, cy - s}, {cx + s, cy - 1}, c, 1.4f);
            renderer.StrokeRounded(Rect(cx - s + 1, cy - 1, cx + s - 1, cy + s), 2.0f,
                                   c, 1.4f);
            break;
        case Page::Console:
            renderer.StrokeRounded(Rect(cx - s, cy - s + 2, cx + s, cy + s - 2), 3.0f,
                                   c, 1.4f);
            renderer.FillCircle(cx - 4, cy, 1.4f, c);
            renderer.FillCircle(cx + 4, cy, 1.4f, c);
            break;
        case Page::Streaming:
            renderer.FillTriangle({cx - 4, cy - 7}, {cx - 4, cy + 7}, {cx + 7, cy}, c);
            break;
        case Page::AiVideo:
            renderer.Line({cx - s, cy}, {cx + s, cy}, c, 1.3f);
            renderer.Line({cx, cy - s}, {cx, cy + s}, c, 1.3f);
            renderer.Line({cx - 5, cy - 5}, {cx + 5, cy + 5}, c, 1.0f);
            renderer.Line({cx + 5, cy - 5}, {cx - 5, cy + 5}, c, 1.0f);
            break;
        case Page::Display:
            renderer.StrokeRounded(Rect(cx - s, cy - 6, cx + s, cy + 4), 2.0f, c, 1.4f);
            renderer.Line({cx - 4, cy + 8}, {cx + 4, cy + 8}, c, 1.4f);
            renderer.Line({cx, cy + 4}, {cx, cy + 8}, c, 1.4f);
            break;
        case Page::Overlay:
            renderer.StrokeRounded(Rect(cx - s, cy - s, cx + 3, cy + 3), 2.0f, c, 1.2f);
            renderer.StrokeRounded(Rect(cx - 3, cy - 3, cx + s, cy + s), 2.0f, c, 1.2f);
            break;
        case Page::Profiles:
            renderer.FillCircle(cx - 4, cy - 4, 3.0f, c);
            renderer.FillCircle(cx + 4, cy - 2, 2.6f, c);
            renderer.StrokeRounded(Rect(cx - 8, cy + 2, cx + 1, cy + 8), 4.0f, c, 1.2f);
            break;
        case Page::Diagnostics:
            renderer.Line({cx - s, cy}, {cx - 4, cy}, c, 1.4f);
            renderer.Line({cx - 4, cy}, {cx - 1, cy - 5}, c, 1.4f);
            renderer.Line({cx - 1, cy - 5}, {cx + 2, cy + 5}, c, 1.4f);
            renderer.Line({cx + 2, cy + 5}, {cx + 5, cy}, c, 1.4f);
            renderer.Line({cx + 5, cy}, {cx + s, cy}, c, 1.4f);
            break;
        case Page::Settings:
            renderer.StrokeCircle(cx, cy, 7.0f, c, 1.4f);
            renderer.FillCircle(cx, cy, 2.2f, c);
            for (int i = 0; i < 6; ++i) {
                const float a = static_cast<float>(i) * 3.14159265f / 3.0f;
                renderer.Line({cx + std::cos(a) * 7.0f, cy + std::sin(a) * 7.0f},
                              {cx + std::cos(a) * 9.0f, cy + std::sin(a) * 9.0f}, c, 1.2f);
            }
            break;
    }
}

bool PickFile(HWND owner, bool save, std::wstring& path, const wchar_t* filter) {
    wchar_t buffer[MAX_PATH * 2] = {};
    if (!path.empty()) {
        wcsncpy_s(buffer, path.c_str(), _TRUNCATE);
    }
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = owner;
    dialog.lpstrFilter = filter;
    dialog.lpstrFile = buffer;
    dialog.nMaxFile = static_cast<DWORD>(std::size(buffer));
    dialog.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST |
                   (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    const BOOL ok = save ? ::GetSaveFileNameW(&dialog) : ::GetOpenFileNameW(&dialog);
    if (!ok) {
        return false;
    }
    path = buffer;
    return true;
}

}  // namespace

NextApp::NextApp() {
    paths_ = AppPaths::Default();
    host_ = std::make_unique<AppHost>(paths_);
}

NextApp::~NextApp() = default;

int NextApp::Run() {
    if (!Init()) {
        return 1;
    }
    ::timeBeginPeriod(1);
    window_->Show();
    while (window_->Pump()) {
        if (window_->ConsumeResize()) {
            Resize();
        }
        now_ = SecondsNow();
        Frame();
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    ::timeEndPeriod(1);
    host_->Shutdown();
    return 0;
}

bool NextApp::Init() {
    window_ = std::make_unique<brp::Win32Window>();
    if (!window_->Create(L"BetterRemotePlay Next", 1600, 900)) {
        return false;
    }
    presenter_ = std::make_unique<brp::Presenter>();
    brp::PresenterConfig config;
    config.output_width = window_->width();
    config.output_height = window_->height();
    config.source_width = 1920;
    config.source_height = 1080;
    config.vsync = false;
    config.allow_tearing = true;
    config.hdr = false;
    if (!presenter_->Init(window_->hwnd(), config)) {
        return false;
    }
    renderer_ = std::make_unique<brp::ui::Renderer>();
    if (!renderer_->Init(presenter_->device())) {
        return false;
    }
    if (!renderer_->BindSwapchain(presenter_->backbuffer())) {
        return false;
    }

    StartupReport report;
    const Status status = host_->Initialize(report);
    if (!status.ok()) {
        MessageBoxW(window_->hwnd(), Widen(status.message()).c_str(),
                    L"BetterRemotePlay Next", MB_OK | MB_ICONERROR);
        return false;
    }
    settings_ = host_->Settings();
    system_ = ProbeSystem();
    RefreshProfiles();
    Resize();
    started_ = SecondsNow();
    last_frame_ = started_;
    if (report.legacy_imported) {
        Toast(L"Legacy profile imported into the new configuration.");
    }
    return true;
}

void NextApp::Resize() {
    if (!window_ || !presenter_ || !renderer_) {
        return;
    }
    const int width = window_->width();
    const int height = window_->height();
    if (width <= 0 || height <= 0) {
        return;
    }
    renderer_->ReleaseTarget();
    presenter_->Resize(width, height);
    renderer_->BindSwapchain(presenter_->backbuffer());
    renderer_->ClearTextCache();
    scale_ = std::clamp(std::min(static_cast<float>(width) / 1600.0f,
                                 static_cast<float>(height) / 900.0f),
                        0.7f, 2.0f);
    logical_width_ = static_cast<float>(width) / scale_;
    logical_height_ = static_cast<float>(height) / scale_;
    renderer_->SetScale(scale_);
    window_->SetFrameMetrics(static_cast<int>(kTitleHeight * scale_),
                             static_cast<int>(150.0f * scale_));
    std::fprintf(stderr, "[next] window %dx%d scale %.3f logical %.1fx%.1f\n",
                 width, height, scale_, logical_width_, logical_height_);
}

void NextApp::Frame() {
    ProbeIfNeeded(now_);
    Render();
    const double delta = std::max(0.0, now_ - last_frame_);
    last_frame_ = now_;
    if (auto* diagnostics = dynamic_cast<DiagnosticsCollector*>(host_->Diagnostics())) {
        diagnostics->AddFrame(delta * 1000.0, 0.0, 0.0, 0.0, 0.0);
        diagnostics->Tick(delta);
    }
    ++ui_frames_;
}

void NextApp::Render() {
    const ThemeTokens& theme = ThemeFor(settings_.general.theme);
    const float clear[4] = {theme.background.r / 255.0f, theme.background.g / 255.0f,
                            theme.background.b / 255.0f, 1.0f};
    presenter_->Clear(clear);
    renderer_->BeginFrame();
    renderer_->FillRect(Rect(0, 0, logical_width_, logical_height_),
                        ToD2D(theme.background));

    const auto& mouse = window_->mouse();
    WidgetInput input;
    input.x = static_cast<float>(mouse.x) / scale_;
    input.y = static_cast<float>(mouse.y) / scale_;
    input.down = mouse.down;
    input.clicked = window_->ConsumeClick();
    Widgets widgets(*renderer_, theme);
    widgets.Begin(input, now_);

    RenderTitleBar(widgets);
    RenderSidebar(widgets);
    switch (navigation_.Current()) {
        case Page::Home: RenderHome(widgets); break;
        case Page::Console: RenderConsole(widgets); break;
        case Page::Streaming: RenderStreaming(widgets); break;
        case Page::AiVideo: RenderAi(widgets); break;
        case Page::Display: RenderDisplay(widgets); break;
        case Page::Overlay: RenderOverlay(widgets); break;
        case Page::Profiles: RenderProfiles(widgets); break;
        case Page::Diagnostics: RenderDiagnostics(widgets); break;
        case Page::Settings: RenderSettings(widgets); break;
    }

    if (now_ < toast_until_ && !toast_text_.empty()) {
        const float width = 460.0f;
        const D2D1_RECT_F rect = Rect((logical_width_ - width) * 0.5f,
                                      logical_height_ - 74.0f,
                                      (logical_width_ + width) * 0.5f,
                                      logical_height_ - 34.0f);
        const Color color = toast_error_ ? theme.error : theme.success;
        renderer_->FillRounded(rect, 10.0f, ToD2D(theme.surface_alt));
        renderer_->StrokeRounded(rect, 10.0f, ToD2D(color), 1.2f);
        renderer_->Text(toast_text_.c_str(), rect, 13.0f, ToD2D(theme.text),
                        DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_TEXT_ALIGNMENT_CENTER);
    }

    widgets.End();
    renderer_->EndFrame();
    presenter_->PresentNow(true);
    window_->set_hand_cursor(widgets.interactable_hovered());
}

void NextApp::RenderTitleBar(Widgets& widgets) {
    const ThemeTokens& theme = widgets.Tokens();
    renderer_->FillRect(Rect(0, 0, logical_width_, kTitleHeight), ToD2D(theme.surface));
    renderer_->FillRect(Rect(0, kTitleHeight - 1, logical_width_, kTitleHeight),
                        ToD2D(theme.border));
    renderer_->Text(L"BetterRemotePlay", Rect(18, 0, 180, kTitleHeight), 13.0f,
                    ToD2D(theme.text), DWRITE_FONT_WEIGHT_SEMI_BOLD);
    renderer_->Text(L"NEXT", Rect(154, 0, 210, kTitleHeight), 11.0f,
                    ToD2D(theme.accent), DWRITE_FONT_WEIGHT_BOLD);
    renderer_->Text(kVersion, Rect(214, 0, 320, kTitleHeight), 10.5f,
                    ToD2D(theme.text_faint), DWRITE_FONT_WEIGHT_MEDIUM);

    const float button = 34.0f;
    const float close_x = logical_width_ - 8.0f - button;
    const float max_x = close_x - button - 2.0f;
    const float min_x = max_x - button - 2.0f;
    const D2D1_RECT_F min_rect = Rect(min_x, 6, min_x + button, kTitleHeight - 6);
    const D2D1_RECT_F max_rect = Rect(max_x, 6, max_x + button, kTitleHeight - 6);
    const D2D1_RECT_F close_rect = Rect(close_x, 6, close_x + button, kTitleHeight - 6);
    if (widgets.Button(9001, min_rect, L"-", false)) {
        window_->Minimize();
    }
    if (widgets.Button(9002, max_rect, L"[]", false)) {
        window_->ToggleMaximize();
    }
    if (widgets.Button(9003, close_rect, L"X", false)) {
        window_->Close();
    }

    const D2D1_RECT_F theme_rect = Rect(min_x - 86.0f, 6, min_x - 8.0f,
                                        kTitleHeight - 6);
    if (widgets.Button(9004, theme_rect,
                       settings_.general.theme == Theme::Dark ? L"Light" : L"Dark",
                       false)) {
        settings_.general.theme =
            settings_.general.theme == Theme::Dark ? Theme::Light : Theme::Dark;
        Apply();
    }
    widgets.StatusPill(Rect(330, 11, 420, kTitleHeight - 11), L"STAGE 3",
                       theme.warning, true);
}

void NextApp::RenderSidebar(Widgets& widgets) {
    const ThemeTokens& theme = widgets.Tokens();
    const float width = SidebarWidth();
    renderer_->FillRect(Rect(0, kTitleHeight, width, logical_height_),
                        ToD2D(theme.surface));
    renderer_->FillRect(Rect(width - 1, kTitleHeight, width, logical_height_),
                        ToD2D(theme.border));

    const D2D1_RECT_F collapse = Rect(12, kTitleHeight + 12, 44, kTitleHeight + 40);
    if (widgets.Button(9100, collapse, collapsed_ ? L">>" : L"<<", false)) {
        collapsed_ = !collapsed_;
    }

    float y = kTitleHeight + 54.0f;
    std::string last_group;
    int index = 0;
    for (const auto& page : navigation_.Pages()) {
        if (!collapsed_ && page.group != last_group) {
            last_group = page.group;
            widgets.Section(Rect(20, y, width - 16, y + 16),
                            Widen(page.group).c_str());
            y += 20.0f;
        }
        const D2D1_RECT_F item = Rect(8, y, width - 8, y + 38);
        const bool selected = navigation_.Current() == page.page;
        const int text_left = collapsed_ ? 0 : 48;
        if (widgets.NavItem(9200 + index, item, Widen(page.title).c_str(), selected,
                            text_left)) {
            navigation_.Select(page.page);
        }
        DrawNavIcon(*renderer_, page.page, collapsed_ ? width * 0.5f : 27.0f,
                    y + 19.0f, selected ? theme.accent : theme.text_faint);
        y += 40.0f;
        ++index;
    }

    if (!collapsed_) {
        renderer_->Text(L"Foundation", Rect(20, logical_height_ - 56, width - 16,
                                            logical_height_ - 40),
                        10.5f, ToD2D(theme.text_faint), DWRITE_FONT_WEIGHT_SEMI_BOLD);
        renderer_->Text(L"config / profiles / diagnostics", Rect(20, logical_height_ - 40,
                                                                width - 16,
                                                                logical_height_ - 22),
                        10.0f, ToD2D(theme.text_faint), DWRITE_FONT_WEIGHT_NORMAL);
    }
}

float NextApp::SidebarWidth() const {
    return collapsed_ ? kSidebarCollapsed : kSidebar;
}

float NextApp::ContentX() const { return SidebarWidth() + kPad; }

float NextApp::ContentWidth() const {
    return logical_width_ - ContentX() - kPad;
}

void NextApp::RenderHome(Widgets& widgets) {
    const ThemeTokens& theme = widgets.Tokens();
    const float x = ContentX();
    const float w = ContentWidth();
    float y = 58.0f;
    renderer_->Text(L"Home", Rect(x, y, x + w, y + 34), 26.0f, ToD2D(theme.text),
                    DWRITE_FONT_WEIGHT_SEMI_BOLD);
    renderer_->Text(L"System overview and active configuration", Rect(x, y + 34, x + w, y + 54),
                    12.0f, ToD2D(theme.text_dim), DWRITE_FONT_WEIGHT_NORMAL);

    y = 128.0f;
    const float gap = 16.0f;
    const float card_w = (w - gap * 2.0f) / 3.0f;
    const float card_h = 164.0f;

    auto info_card = [&](float cx, const wchar_t* caption, const std::wstring& title,
                         const std::vector<std::wstring>& lines) {
        widgets.Card(Rect(cx, y, cx + card_w, y + card_h));
        widgets.Section(Rect(cx + 18, y + 14, cx + card_w - 18, y + 30), caption);
        renderer_->TextEllipsis(title.c_str(),
                                Rect(cx + 18, y + 38, cx + card_w - 18, y + 66), 17.0f,
                                ToD2D(theme.text), DWRITE_FONT_WEIGHT_SEMI_BOLD);
        float ly = y + 76.0f;
        for (const auto& line : lines) {
            renderer_->TextEllipsis(line.c_str(),
                                    Rect(cx + 18, ly, cx + card_w - 18, ly + 22), 11.5f,
                                    ToD2D(theme.text_dim));
            ly += 22.0f;
        }
    };

    const std::wstring os = Widen(system_.os);
    const std::wstring cpu = Widen(system_.cpu);
    info_card(x, L"SYSTEM",
              os.empty() ? L"Windows" : os,
              {cpu.empty() ? L"Unknown CPU" : cpu,
               std::to_wstring(system_.logical_cores) + L" logical cores",
               std::to_wstring(system_.ram_mb / 1024) + L" GB RAM"});

    const std::wstring gpu = Widen(system_.gpu.name);
    info_card(x + card_w + gap, L"GPU",
              system_.gpu.present ? gpu : L"No DXGI adapter",
              {system_.gpu.present
                   ? std::to_wstring(system_.gpu.vram_mb / 1024) + L" GB VRAM"
                   : std::wstring(),
               system_.gpu.driver.empty()
                   ? std::wstring(L"Driver: unknown")
                   : std::wstring(L"Driver ") + Widen(system_.gpu.driver),
               system_.gpu.nvidia ? L"NVIDIA" : L"Non-NVIDIA"});

    std::wstring display_title = L"No display";
    std::vector<std::wstring> display_lines;
    for (const auto& display : system_.displays) {
        if (display.primary) {
            display_title = std::to_wstring(display.width) + L"x" +
                            std::to_wstring(display.height) + L" @ " +
                            std::to_wstring(display.refresh_hz) + L" Hz";
        }
    }
    display_lines.push_back(std::to_wstring(system_.displays.size()) + L" monitor(s)");
    display_lines.push_back(L"Output: " + Widen(std::string(ToString(settings_.video.output))));
    info_card(x + (card_w + gap) * 2.0f, L"DISPLAY", display_title, display_lines);

    y += card_h + gap;
    widgets.Card(Rect(x, y, x + w, y + 142.0f));
    widgets.Section(Rect(x + 18, y + 14, x + w - 18, y + 30), L"ACTIVE CONFIGURATION");
    const float col = w / 4.0f;
    auto metric = [&](int index, const wchar_t* caption, const std::wstring& value,
                      const std::wstring& detail) {
        const float cx = x + 18.0f + col * index;
        renderer_->Text(caption, Rect(cx, y + 42, cx + col - 20, y + 58), 10.5f,
                        ToD2D(theme.text_faint), DWRITE_FONT_WEIGHT_SEMI_BOLD);
        renderer_->TextEllipsis(value.c_str(), Rect(cx, y + 62, cx + col - 20, y + 88),
                                18.0f, ToD2D(theme.text), DWRITE_FONT_WEIGHT_SEMI_BOLD);
        renderer_->Text(detail.c_str(), Rect(cx, y + 90, cx + col - 20, y + 110), 11.0f,
                        ToD2D(theme.text_dim));
    };
    metric(0, L"INPUT",
           Widen(std::string(ToString(settings_.streaming.input))) + L" " +
               std::to_wstring(settings_.streaming.fps) + L" fps",
           Widen(settings_.streaming.codec));
    metric(1, L"OUTPUT",
           Widen(std::string(ToString(settings_.video.output))) + L" " +
               std::to_wstring(settings_.ai.target_fps) + L" fps",
           Widen(settings_.video.color_space));
    metric(2, L"SUPER RESOLUTION",
           settings_.ai.super_resolution ? L"DLSS" : L"Off",
           settings_.ai.super_resolution
               ? Widen(std::string(ToString(settings_.ai.sr_output)))
               : L"native");
    metric(3, L"FRAME GENERATION",
           settings_.ai.frame_generation
               ? Widen(std::string(ToString(settings_.ai.fg_backend)))
               : L"Off",
           settings_.ai.frame_generation
               ? std::to_wstring(settings_.ai.target_fps) + L" fps target"
               : L"source only");

    y += 142.0f + gap;
    widgets.Card(Rect(x, y, x + w, y + 122.0f));
    widgets.Section(Rect(x + 18, y + 14, x + w - 18, y + 30), L"AI CAPABILITIES");
    const float pill_w = 170.0f;
    const float pill_gap = 10.0f;
    float px = x + 18.0f;
    const float py = y + 44.0f;
    auto capability = [&](bool available, const wchar_t* label, const wchar_t* reason) {
        const Color color = available ? theme.success : theme.warning;
        widgets.StatusPill(Rect(px, py, px + pill_w, py + 30), label, color, true);
        px += pill_w + pill_gap;
        if (!available && reason) {
            renderer_->Text(reason, Rect(px - pill_w - pill_gap, py + 34,
                                         px - pill_gap, py + 54),
                            10.0f, ToD2D(theme.text_faint));
        }
    };
    capability(system_.dlss_sr, L"DLSS SR", L"nvngx_dlss.dll");
    capability(system_.nr_worker, L"DLSS FG 2x", L"nr worker");
    capability(system_.rife_lite, L"RIFE Lite", L"engine");
    capability(system_.rife_heavy, L"RIFE Heavy", L"engine");
    capability(system_.tensorrt_runtime, L"TensorRT", L"runtime");
    renderer_->Text(
        L"Capabilities are file/SDK checks, not promises. Inference wiring lands in Stage 6.",
        Rect(x + 18, y + 96, x + w - 18, y + 114), 10.5f, ToD2D(theme.text_faint));

    y += 122.0f + 18.0f;
    const float button_w = 190.0f;
    if (widgets.Button(1001, Rect(x, y, x + button_w, y + 38), L"AI Video Engine")) {
        navigation_.Select(Page::AiVideo);
    }
    if (widgets.Button(1002, Rect(x + button_w + 10, y, x + button_w * 2 + 10, y + 38),
                       L"Profiles", false)) {
        navigation_.Select(Page::Profiles);
    }
    if (widgets.Button(1003, Rect(x + (button_w + 10) * 2, y,
                                  x + (button_w + 10) * 2 + button_w, y + 38),
                       L"Diagnostics", false)) {
        navigation_.Select(Page::Diagnostics);
    }
    if (widgets.Button(1004, Rect(x + (button_w + 10) * 3, y,
                                  x + (button_w + 10) * 3 + button_w, y + 38),
                       L"Settings", false)) {
        navigation_.Select(Page::Settings);
    }
}

void NextApp::RenderConsole(Widgets& widgets) {
    const ThemeTokens& theme = widgets.Tokens();
    const float x = ContentX();
    const float w = ContentWidth();
    renderer_->Text(L"Console", Rect(x, 58, x + w, 92), 26.0f, ToD2D(theme.text),
                    DWRITE_FONT_WEIGHT_SEMI_BOLD);
    renderer_->Text(L"Saved console identity and connection behaviour",
                    Rect(x, 92, x + w, 112), 12.0f, ToD2D(theme.text_dim));

    float y = 132.0f;
    widgets.Card(Rect(x, y, x + w, y + 250.0f));
    widgets.Section(Rect(x + 18, y + 14, x + w - 18, y + 30), L"SAVED CONSOLE");
    renderer_->Text(settings_.console.name.empty()
                        ? L"No console saved yet"
                        : Widen(settings_.console.name).c_str(),
                    Rect(x + 18, y + 40, x + w - 18, y + 70), 20.0f, ToD2D(theme.text),
                    DWRITE_FONT_WEIGHT_SEMI_BOLD);
    renderer_->Text(settings_.console.address.empty()
                        ? L"Address: not set"
                        : (L"Address: " + Widen(settings_.console.address)).c_str(),
                    Rect(x + 18, y + 72, x + w - 18, y + 94), 12.0f,
                    ToD2D(theme.text_dim));
    renderer_->Text(
        L"PSN identity and pairing credentials stay in the legacy session until the "
        L"streaming service migration in Stage 4.",
        Rect(x + 18, y + 100, x + w - 18, y + 122), 11.0f, ToD2D(theme.text_faint));

    bool changed = false;
    if (widgets.Toggle(2101, Rect(x + 18, y + 132, x + 360, y + 180),
                       L"Discover on startup", settings_.console.auto_discover)) {
        changed = true;
    }
    if (widgets.Toggle(2102, Rect(x + 18, y + 182, x + 360, y + 230),
                       L"Reconnect automatically", settings_.console.auto_reconnect)) {
        changed = true;
    }
    float delay = static_cast<float>(settings_.console.reconnect_delay_ms);
    if (widgets.Slider(2103, Rect(x + 380, y + 160, x + w - 18, y + 210),
                       L"Reconnect delay", delay, 100.0f, 10000.0f, 100.0f, L"%.0f ms")) {
        settings_.console.reconnect_delay_ms = static_cast<int>(std::lround(delay));
        changed = true;
    }
    if (changed) {
        Apply();
    }

    y += 266.0f;
    widgets.Card(Rect(x, y, x + w, y + 94.0f));
    widgets.StatusPill(Rect(x + 18, y + 16, x + 150, y + 44), L"STAGE 4",
                       theme.warning, true);
    renderer_->Text(L"Discovery, PSN sign-in, pairing and session control move here next.",
                    Rect(x + 162, y + 16, x + w - 18, y + 44), 12.0f,
                    ToD2D(theme.text_dim));
    renderer_->Text(L"The working brp.exe still owns those paths until the new service "
                    L"passes the same tests.",
                    Rect(x + 18, y + 50, x + w - 18, y + 74), 11.0f,
                    ToD2D(theme.text_faint));
}

void NextApp::RenderStreaming(Widgets& widgets) {
    const ThemeTokens& theme = widgets.Tokens();
    const float x = ContentX();
    const float w = ContentWidth();
    renderer_->Text(L"Streaming", Rect(x, 58, x + w, 92), 26.0f, ToD2D(theme.text),
                    DWRITE_FONT_WEIGHT_SEMI_BOLD);
    renderer_->Text(L"Input, network and decode configuration",
                    Rect(x, 92, x + w, 112), 12.0f, ToD2D(theme.text_dim));

    widgets.Card(Rect(x, 128, x + w, 214));
    widgets.StatusPill(Rect(x + 18, 144, x + 150, 174), L"NOT CONNECTED",
                       theme.warning, true);
    renderer_->Text(L"Streaming service: idle", Rect(x + 166, 144, x + w - 18, 174),
                    14.0f, ToD2D(theme.text), DWRITE_FONT_WEIGHT_MEDIUM);
    renderer_->Text(L"Media pipeline and session host are wired in Stage 5. The settings "
                    L"below are already persistent.",
                    Rect(x + 18, 180, x + w - 18, 204), 11.0f, ToD2D(theme.text_faint));

    SettingsControls controls(widgets, settings_, system_);
    controls.Reset();
    const float end = controls.Streaming(x, 238, w);
    if (controls.Changed()) {
        Apply();
    }
    renderer_->Text(L"Codec, decoder, bitrate and buffer map to the existing chiaki/FFmpeg "
                    L"paths during the migration.",
                    Rect(x, end + 4, x + w, end + 24), 10.5f, ToD2D(theme.text_faint));
}

void NextApp::RenderAi(Widgets& widgets) {
    const ThemeTokens& theme = widgets.Tokens();
    const float x = ContentX();
    const float w = ContentWidth();
    renderer_->Text(L"AI Video Engine", Rect(x, 58, x + w, 92), 26.0f,
                    ToD2D(theme.text), DWRITE_FONT_WEIGHT_SEMI_BOLD);
    renderer_->Text(L"Super resolution, frame generation and backend capability",
                    Rect(x, 92, x + w, 112), 12.0f, ToD2D(theme.text_dim));

    const float gap = 12.0f;
    const float card_w = (w - gap * 3.0f) / 4.0f;
    auto capability_card = [&](int index, const wchar_t* title, bool available,
                               const wchar_t* detail) {
        const float cx = x + (card_w + gap) * index;
        widgets.Card(Rect(cx, 124, cx + card_w, 204));
        renderer_->Text(title, Rect(cx + 14, 136, cx + card_w - 14, 160), 13.0f,
                        ToD2D(theme.text), DWRITE_FONT_WEIGHT_SEMI_BOLD);
        widgets.StatusPill(Rect(cx + 14, 166, cx + 120, 194),
                           available ? L"AVAILABLE" : L"MISSING",
                           available ? theme.success : theme.warning, true);
        renderer_->Text(detail, Rect(cx + 126, 166, cx + card_w - 10, 194), 10.0f,
                        ToD2D(theme.text_faint));
    };
    capability_card(0, L"DLSS Super Resolution", system_.dlss_sr, L"nvngx_dlss.dll");
    capability_card(1, L"DLSS Frame Generation", system_.nr_worker, L"nr worker");
    capability_card(2, L"RIFE TensorRT", system_.rife_lite, L"engine");
    capability_card(3, L"TensorRT runtime", system_.tensorrt_runtime, L"nvinfer_10");

    SettingsControls controls(widgets, settings_, system_);
    controls.Reset();
    const float end = controls.Ai(x, 220, w);
    if (controls.Changed()) {
        Apply();
    }
    renderer_->Text(L"Inference is wired in Stage 6. This page only exposes capabilities "
                    L"that are actually present and persists the selection.",
                    Rect(x, end + 4, x + w, end + 24), 10.5f, ToD2D(theme.text_faint));
}

void NextApp::RenderDisplay(Widgets& widgets) {
    const ThemeTokens& theme = widgets.Tokens();
    const float x = ContentX();
    const float w = ContentWidth();
    renderer_->Text(L"Display", Rect(x, 58, x + w, 92), 26.0f, ToD2D(theme.text),
                    DWRITE_FONT_WEIGHT_SEMI_BOLD);
    renderer_->Text(L"Detected monitors, modes and presentation preferences",
                    Rect(x, 92, x + w, 112), 12.0f, ToD2D(theme.text_dim));

    widgets.Card(Rect(x, 128, x + w, 270));
    widgets.Section(Rect(x + 18, 142, x + w - 18, 158), L"DETECTED DISPLAYS");
    float y = 168.0f;
    if (system_.displays.empty()) {
        renderer_->Text(L"No display reported by the OS.", Rect(x + 18, y, x + w - 18, y + 24),
                        12.0f, ToD2D(theme.text_faint));
    }
    for (std::size_t i = 0; i < system_.displays.size() && i < 3; ++i) {
        const auto& display = system_.displays[i];
        const std::wstring line =
            L"Display " + std::to_wstring(i + 1) + L": " + std::to_wstring(display.width) +
            L"x" + std::to_wstring(display.height) + L" @ " +
            std::to_wstring(display.refresh_hz) + L" Hz" +
            (display.primary ? L"  (primary)" : L"");
        renderer_->Text(line.c_str(), Rect(x + 18, y, x + w - 18, y + 26), 13.0f,
                        ToD2D(display.primary ? theme.text : theme.text_dim),
                        display.primary ? DWRITE_FONT_WEIGHT_SEMI_BOLD
                                        : DWRITE_FONT_WEIGHT_NORMAL);
        y += 28.0f;
    }
    renderer_->Text(L"Mode switching is Stage 5; this page reads the real current modes.",
                    Rect(x + 18, 236, x + w - 18, 258), 10.5f, ToD2D(theme.text_faint));

    SettingsControls controls(widgets, settings_, system_);
    controls.Reset();
    const float end = controls.Display(x, 292, w);
    if (controls.Changed()) {
        Apply();
    }
    renderer_->Text(L"V-Sync and VRR are saved now and applied by the presenter in Stage 5.",
                    Rect(x, end + 4, x + w, end + 24), 10.5f, ToD2D(theme.text_faint));
}

void NextApp::RenderOverlay(Widgets& widgets) {
    const ThemeTokens& theme = widgets.Tokens();
    const float x = ContentX();
    const float w = ContentWidth();
    renderer_->Text(L"Overlay", Rect(x, 58, x + w, 92), 26.0f, ToD2D(theme.text),
                    DWRITE_FONT_WEIGHT_SEMI_BOLD);
    renderer_->Text(L"Independent performance overlay configuration",
                    Rect(x, 92, x + w, 112), 12.0f, ToD2D(theme.text_dim));

    const D2D1_RECT_F preview = Rect(x, 128, x + 540, 310);
    widgets.Card(preview);
    renderer_->FillRounded(Rect(preview.left + 18, preview.top + 18, preview.right - 18,
                                preview.bottom - 18),
                           10.0f, ToD2D(theme.background));
    renderer_->Text(L"OVERLAY PREVIEW", Rect(preview.left + 34, preview.top + 34,
                                            preview.left + 250, preview.top + 56),
                    11.0f, ToD2D(theme.text_faint), DWRITE_FONT_WEIGHT_SEMI_BOLD);
    const float box_w = 220.0f;
    const float box_h = 96.0f;
    float ox = preview.left + 24;
    float oy = preview.top + 24;
    if ((settings_.overlay.position & 1) != 0) {
        ox = preview.right - box_w - 24;
    }
    if (settings_.overlay.position >= 2) {
        oy = preview.bottom - box_h - 24;
    }
    const Color panel = theme.surface_alt;
    renderer_->FillRounded(Rect(ox, oy, ox + box_w, oy + box_h), 8.0f,
                           ToD2D(Color{panel.r, panel.g, panel.b,
                                       static_cast<std::uint8_t>(
                                           std::clamp(settings_.overlay.opacity, 0.1f, 1.0f) *
                                           255)}));
    renderer_->StrokeRounded(Rect(ox, oy, ox + box_w, oy + box_h), 8.0f,
                            ToD2D(theme.accent), 1.0f);
    renderer_->Text(L"120 fps", Rect(ox + 12, oy + 8, ox + box_w - 12, oy + 34), 18.0f,
                    ToD2D(theme.text), DWRITE_FONT_WEIGHT_SEMI_BOLD);
    renderer_->Text(L"GPU 62%  |  FG 2x", Rect(ox + 12, oy + 40, ox + box_w - 12, oy + 62),
                    10.5f, ToD2D(theme.text_dim));
    renderer_->Text(L"decode 8.4 ms  |  present 6.1 ms",
                    Rect(ox + 12, oy + 64, ox + box_w - 12, oy + 86), 10.0f,
                    ToD2D(theme.text_faint));

    SettingsControls controls(widgets, settings_, system_);
    controls.Reset();
    const float end = controls.Overlay(x, 328, w);
    if (controls.Changed()) {
        Apply();
    }
    renderer_->Text(L"The independent layered overlay window lands in Stage 7; this is a "
                    L"layout preview driven by the saved values.",
                    Rect(x, end + 4, x + w, end + 24), 10.5f, ToD2D(theme.text_faint));
}

void NextApp::RenderProfiles(Widgets& widgets) {
    const ThemeTokens& theme = widgets.Tokens();
    const float x = ContentX();
    const float w = ContentWidth();
    renderer_->Text(L"Profiles", Rect(x, 58, x + w, 92), 26.0f, ToD2D(theme.text),
                    DWRITE_FONT_WEIGHT_SEMI_BOLD);
    renderer_->Text(L"Named configurations with compatibility validation",
                    Rect(x, 92, x + w, 112), 12.0f, ToD2D(theme.text_dim));

    const float button_w = 170.0f;
    if (widgets.Button(3101, Rect(x + w - button_w * 2 - 10, 62,
                                  x + w - button_w - 10, 100),
                       L"New from current")) {
        Profile created;
        const Status status = host_->Profiles()->Create(
            "Profile " + std::to_string(profiles_.size() + 1), settings_, created);
        if (status.ok()) {
            RefreshProfiles();
            Toast(L"Profile created");
        } else {
            Toast(Widen(status.message()), true);
        }
    }
    if (widgets.Button(3102, Rect(x + w - button_w, 62, x + w, 100), L"Import...", false)) {
        std::wstring path;
        if (PickFile(window_->hwnd(), false, path, L"Profile JSON\0*.json\0All files\0*.*\0")) {
            Profile imported;
            const Status status = host_->Profiles()->Import(Narrow(path), imported);
            if (status.ok() && host_->Profiles()->Save(imported).ok()) {
                RefreshProfiles();
                Toast(L"Profile imported");
            } else {
                Toast(Widen(status.message()), true);
            }
        }
    }

    if (profiles_.empty()) {
        widgets.Card(Rect(x, 124, x + w, 220));
        renderer_->Text(L"No profiles yet.", Rect(x + 18, 142, x + w - 18, 170), 15.0f,
                        ToD2D(theme.text), DWRITE_FONT_WEIGHT_SEMI_BOLD);
        renderer_->Text(L"Save the current configuration as the first profile.",
                        Rect(x + 18, 172, x + w - 18, 198), 11.5f,
                        ToD2D(theme.text_faint));
        return;
    }

    float y = 124.0f;
    int index = 0;
    for (const auto& profile : profiles_) {
        if (index >= 8) {
            break;
        }
        widgets.Card(Rect(x, y, x + w, y + 72));
        renderer_->Text(Widen(profile.name).c_str(),
                        Rect(x + 18, y + 10, x + w - 460, y + 34), 15.0f,
                        ToD2D(theme.text), DWRITE_FONT_WEIGHT_SEMI_BOLD);
        renderer_->Text(Summary(profile).c_str(),
                        Rect(x + 18, y + 36, x + w - 460, y + 60), 11.0f,
                        ToD2D(theme.text_dim));
        if (profile.is_default) {
            widgets.StatusPill(Rect(x + w - 450, y + 20, x + w - 370, y + 48), L"DEFAULT",
                               theme.accent, true);
        }
        const float bx = x + w - 356.0f;
        if (widgets.Button(3200 + index * 10, Rect(bx, y + 18, bx + 78, y + 54), L"Apply",
                           true)) {
            settings_ = profile.settings;
            Apply();
            Toast(L"Profile applied");
        }
        if (widgets.Button(3201 + index * 10, Rect(bx + 84, y + 18, bx + 162, y + 54),
                           L"Default", false)) {
            const Status status = host_->Profiles()->SetDefault(profile.id);
            if (status.ok()) {
                RefreshProfiles();
                Toast(L"Default profile set");
            } else {
                Toast(Widen(status.message()), true);
            }
        }
        if (widgets.Button(3202 + index * 10, Rect(bx + 168, y + 18, bx + 246, y + 54),
                           L"Export", false)) {
            std::wstring path = Widen(profile.id) + L".json";
            if (PickFile(window_->hwnd(), true, path,
                         L"Profile JSON\0*.json\0All files\0*.*\0")) {
                const Status status = host_->Profiles()->Export(profile.id, Narrow(path));
                Toast(status.ok() ? std::wstring(L"Profile exported")
                                  : Widen(status.message()),
                      !status.ok());
            }
        }
        if (widgets.Button(3203 + index * 10, Rect(bx + 252, y + 18, bx + 330, y + 54),
                           L"Delete", false)) {
            const Status status = host_->Profiles()->Delete(profile.id);
            if (status.ok()) {
                RefreshProfiles();
                Toast(L"Profile deleted");
            } else {
                Toast(Widen(status.message()), true);
            }
        }
        y += 80.0f;
        ++index;
    }
}

void NextApp::RenderDiagnostics(Widgets& widgets) {
    const ThemeTokens& theme = widgets.Tokens();
    const float x = ContentX();
    const float w = ContentWidth();
    renderer_->Text(L"Diagnostics", Rect(x, 58, x + w, 92), 26.0f, ToD2D(theme.text),
                    DWRITE_FONT_WEIGHT_SEMI_BOLD);
    renderer_->Text(L"Live counters, system facts and report output",
                    Rect(x, 92, x + w, 112), 12.0f, ToD2D(theme.text_dim));

    const float gap = 14.0f;
    const float card_w = (w - gap) * 0.5f;
    const float card_h = 136.0f;
    auto card = [&](int column, int row, const wchar_t* caption) {
        const float cx = x + (card_w + gap) * column;
        const float cy = 124.0f + (card_h + gap) * row;
        widgets.Card(Rect(cx, cy, cx + card_w, cy + card_h));
        widgets.Section(Rect(cx + 16, cy + 12, cx + card_w - 16, cy + 28), caption);
        return std::pair<float, float>(cx + 16, cy + 38);
    };
    {
        const auto [cx, cy] = card(0, 0, L"SYSTEM");
        renderer_->Text(Widen(system_.os).c_str(), Rect(cx, cy, cx + card_w - 32, cy + 22),
                        13.0f, ToD2D(theme.text), DWRITE_FONT_WEIGHT_SEMI_BOLD);
        renderer_->TextEllipsis(Widen(system_.cpu).c_str(),
                                Rect(cx, cy + 26, cx + card_w - 32, cy + 46), 11.0f,
                                ToD2D(theme.text_dim));
        renderer_->Text((std::to_wstring(system_.logical_cores) + L" threads  |  " +
                         std::to_wstring(system_.ram_mb / 1024) + L" GB RAM").c_str(),
                        Rect(cx, cy + 50, cx + card_w - 32, cy + 72), 11.0f,
                        ToD2D(theme.text_faint));
    }
    {
        const auto [cx, cy] = card(1, 0, L"GPU / BACKENDS");
        renderer_->TextEllipsis(Widen(system_.gpu.name).c_str(),
                                Rect(cx, cy, cx + card_w - 32, cy + 22), 13.0f,
                                ToD2D(theme.text), DWRITE_FONT_WEIGHT_SEMI_BOLD);
        renderer_->Text((std::to_wstring(system_.gpu.vram_mb / 1024) + L" GB VRAM  |  " +
                         (system_.gpu.driver.empty()
                              ? std::wstring(L"driver unknown")
                              : std::wstring(L"driver ") + Widen(system_.gpu.driver)))
                            .c_str(),
                        Rect(cx, cy + 26, cx + card_w - 32, cy + 46), 11.0f,
                        ToD2D(theme.text_dim));
        renderer_->Text((std::wstring(L"CUDA: ") +
                         (system_.cuda ? L"driver present" : L"missing") +
                         L"  |  TensorRT: " +
                         (system_.tensorrt_runtime ? L"runtime present" : L"runtime missing"))
                            .c_str(),
                        Rect(cx, cy + 50, cx + card_w - 32, cy + 72), 11.0f,
                        ToD2D(theme.text_faint));
    }
    {
        const auto [cx, cy] = card(0, 1, L"DISPLAY");
        if (!system_.displays.empty()) {
            const auto& display = system_.displays.front();
            renderer_->Text((std::to_wstring(display.width) + L"x" +
                             std::to_wstring(display.height) + L" @ " +
                             std::to_wstring(display.refresh_hz) + L" Hz")
                                .c_str(),
                            Rect(cx, cy, cx + card_w - 32, cy + 22), 13.0f,
                            ToD2D(theme.text), DWRITE_FONT_WEIGHT_SEMI_BOLD);
        } else {
            renderer_->Text(L"No display detected", Rect(cx, cy, cx + card_w - 32, cy + 22),
                            13.0f, ToD2D(theme.text));
        }
        renderer_->Text((std::to_wstring(system_.displays.size()) + L" monitor(s)  |  " +
                         Widen(std::string(ToString(settings_.display.refresh))))
                            .c_str(),
                        Rect(cx, cy + 26, cx + card_w - 32, cy + 46), 11.0f,
                        ToD2D(theme.text_dim));
    }
    {
        const auto [cx, cy] = card(1, 1, L"FILES");
        const std::wstring settings_path = Widen(paths_.settings_file.string());
        const std::wstring log_path = Widen(paths_.log_file.string());
        renderer_->TextEllipsis(settings_path.c_str(),
                                Rect(cx, cy, cx + card_w - 32, cy + 20), 10.5f,
                                ToD2D(theme.text_dim));
        renderer_->TextEllipsis(log_path.c_str(),
                                Rect(cx, cy + 24, cx + card_w - 32, cy + 44), 10.5f,
                                ToD2D(theme.text_dim));
        renderer_->Text(L"Profiles and exports live in the same data folder.",
                        Rect(cx, cy + 50, cx + card_w - 32, cy + 72), 10.5f,
                        ToD2D(theme.text_faint));
    }

    float y = 124.0f + (card_h + gap) * 2.0f;
    widgets.Card(Rect(x, y, x + w, y + 164));
    widgets.Section(Rect(x + 16, y + 12, x + w - 16, y + 28), L"UI FRAME TIMING (REAL)");
    const DiagnosticsSnapshot snapshot = host_->Diagnostics()->Snapshot();
    double uptime = 0.0;
    if (auto* collector = dynamic_cast<DiagnosticsCollector*>(host_->Diagnostics())) {
        uptime = collector->UptimeSeconds();
    }
    const float col = w / 4.0f;
    auto metric = [&](int index, const wchar_t* caption, const std::wstring& value) {
        const float cx = x + 16.0f + col * index;
        renderer_->Text(caption, Rect(cx, y + 42, cx + col - 24, y + 58), 10.0f,
                        ToD2D(theme.text_faint), DWRITE_FONT_WEIGHT_SEMI_BOLD);
        renderer_->Text(value.c_str(), Rect(cx, y + 62, cx + col - 24, y + 92), 18.0f,
                        ToD2D(theme.text), DWRITE_FONT_WEIGHT_SEMI_BOLD);
    };
    wchar_t buffer[64] = {};
    std::swprintf(buffer, 64, L"%.2f ms", snapshot.frame_p50_ms);
    metric(0, L"P50", buffer);
    std::swprintf(buffer, 64, L"%.2f ms", snapshot.frame_p95_ms);
    metric(1, L"P95", buffer);
    std::swprintf(buffer, 64, L"%.2f ms", snapshot.frame_p99_ms);
    metric(2, L"P99", buffer);
    metric(3, L"UPTIME", std::to_wstring(static_cast<int>(uptime)) + L" s");
    renderer_->Text(L"Stream decode/SR/FG timings appear here once the media pipeline is "
                    L"connected (Stage 5-6).",
                    Rect(x + 16, y + 104, x + w - 16, y + 126), 10.5f,
                    ToD2D(theme.text_faint));
    renderer_->Text((L"UI frames: " + std::to_wstring(ui_frames_)).c_str(),
                    Rect(x + 16, y + 128, x + w - 16, y + 148), 10.5f,
                    ToD2D(theme.text_faint));

    y += 180.0f;
    if (widgets.Button(3301, Rect(x, y, x + 190, y + 38), L"Write report")) {
        const auto report = paths_.settings_file.parent_path() / "diagnostics.txt";
        const Status status = host_->Diagnostics()->WriteReport(report.string());
        Toast(status.ok() ? std::wstring(L"Report written: ") + PathW(report)
                          : Widen(status.message()),
              !status.ok());
    }
    if (widgets.Button(3302, Rect(x + 200, y, x + 390, y + 38), L"Open data folder",
                       false)) {
        const std::wstring folder = paths_.settings_file.parent_path().wstring();
        ::ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

void NextApp::RenderSettings(Widgets& widgets) {
    const ThemeTokens& theme = widgets.Tokens();
    const float x = ContentX();
    const float w = ContentWidth();
    renderer_->Text(L"Settings", Rect(x, 58, x + w, 92), 26.0f, ToD2D(theme.text),
                    DWRITE_FONT_WEIGHT_SEMI_BOLD);
    renderer_->Text(L"Central configuration, saved atomically on every change",
                    Rect(x, 92, x + w, 112), 12.0f, ToD2D(theme.text_dim));

    static const wchar_t* categories[] = {
        L"General", L"Streaming", L"Video", L"AI Video Engine", L"GPU", L"Display",
        L"Overlay", L"Audio", L"Controller", L"Advanced", L"About"};
    const float list_w = 238.0f;
    float y = 128.0f;
    for (int i = 0; i < static_cast<int>(std::size(categories)); ++i) {
        const D2D1_RECT_F row = Rect(x, y, x + list_w, y + 38);
        if (widgets.NavItem(9400 + i, row, categories[i], settings_category_ == i, 14)) {
            settings_category_ = i;
        }
        y += 40.0f;
    }

    const float px = x + list_w + 24.0f;
    const float pw = x + w - px;
    widgets.Card(Rect(px, 128, px + pw, logical_height_ - 40));
    if (settings_category_ == 10) {
        RenderAbout(widgets, px + 18, 146, pw - 36);
        return;
    }
    SettingsControls controls(widgets, settings_, system_);
    controls.Reset();
    float cy = 146.0f;
    switch (settings_category_) {
        case 0: cy = controls.General(px + 18, cy, pw - 36); break;
        case 1: cy = controls.Streaming(px + 18, cy, pw - 36); break;
        case 2: cy = controls.Video(px + 18, cy, pw - 36); break;
        case 3: cy = controls.Ai(px + 18, cy, pw - 36); break;
        case 4: cy = controls.Gpu(px + 18, cy, pw - 36); break;
        case 5: cy = controls.Display(px + 18, cy, pw - 36); break;
        case 6: cy = controls.Overlay(px + 18, cy, pw - 36); break;
        case 7: cy = controls.Audio(px + 18, cy, pw - 36); break;
        case 8: cy = controls.Controller(px + 18, cy, pw - 36); break;
        case 9: cy = controls.Advanced(px + 18, cy, pw - 36); break;
        default: break;
    }
    if (controls.Changed()) {
        Apply();
    }
    if (cy < logical_height_ - 60) {
        renderer_->Text(L"Changes save to next-settings.json immediately.",
                        Rect(px + 18, cy + 4, px + pw - 18, cy + 24), 10.5f,
                        ToD2D(theme.text_faint));
    }
}

void NextApp::RenderAbout(Widgets& widgets, float x, float y, float width) {
    const ThemeTokens& theme = widgets.Tokens();
    renderer_->Text(L"BetterRemotePlay Next", Rect(x, y, x + width, y + 30), 20.0f,
                    ToD2D(theme.text), DWRITE_FONT_WEIGHT_SEMI_BOLD);
    renderer_->Text((std::wstring(L"Version ") + kVersion + L"  |  Stage 3 shell").c_str(),
                    Rect(x, y + 32, x + width, y + 52), 11.5f, ToD2D(theme.text_dim));

    float ly = y + 70.0f;
    auto line = [&](const std::wstring& text, const Color& color) {
        renderer_->TextEllipsis(text.c_str(), Rect(x, ly, x + width, ly + 22), 11.5f,
                                ToD2D(color));
        ly += 24.0f;
    };
    line(L"C++20 / Win32 / Direct2D / D3D11 / CMake", theme.text_dim);
    line(L"Foundation modules: core, config, diagnostics, navigation, AppHost, theme",
         theme.text_dim);
    line(L"Reference product: brp.exe 0.4.9 (kept runnable during migration)",
         theme.text_dim);
    line(L"NVIDIA path: DLSS SR, DLSS FG 2x, TensorRT RIFE (wired in Stage 6)",
         theme.text_dim);
    line(L"No RTX VSR. No Sony code or fonts. Noto Sans is SIL OFL.", theme.text_faint);
    ly += 6.0f;
    line(L"Settings: " + Widen(paths_.settings_file.string()), theme.text_faint);
    line(L"Profiles: " + Widen(paths_.profiles_dir.string()), theme.text_faint);
    line(L"Log: " + Widen(paths_.log_file.string()), theme.text_faint);

    if (widgets.Button(9501, Rect(x, ly + 8, x + 200, ly + 46), L"Open data folder")) {
        const std::wstring folder = paths_.settings_file.parent_path().wstring();
        ::ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

void NextApp::Apply() {
    const Status status = host_->ApplySettings(settings_);
    if (!status.ok()) {
        Toast(Widen(status.message()), true);
        settings_ = host_->Settings();
        return;
    }
    settings_ = host_->Settings();
    Toast(L"Saved");
}

void NextApp::RefreshProfiles() {
    profiles_.clear();
    host_->Profiles()->List(profiles_);
    if (selected_profile_ >= static_cast<int>(profiles_.size())) {
        selected_profile_ = static_cast<int>(profiles_.size()) - 1;
    }
}

void NextApp::Toast(const std::wstring& text, bool error) {
    toast_text_ = text;
    toast_error_ = error;
    toast_until_ = now_ + 2.6;
}

void NextApp::ProbeIfNeeded(double now) {
    if (last_probe_ < 0.0 || now - last_probe_ > 10.0) {
        system_ = ProbeSystem();
        last_probe_ = now;
    }
}

std::wstring NextApp::W(const std::string& text) const { return Widen(text); }

std::wstring NextApp::Summary(const Profile& profile) const {
    const auto& s = profile.settings;
    std::wstring summary =
        Widen(std::string(ToString(s.streaming.input))) + L" " +
        std::to_wstring(s.streaming.fps) + L" fps  ->  " +
        Widen(std::string(ToString(s.video.output))) + L" " +
        std::to_wstring(s.ai.target_fps) + L" fps";
    if (s.ai.frame_generation) {
        summary += L"  |  " + Widen(std::string(ToString(s.ai.fg_backend)));
    }
    if (s.ai.super_resolution) {
        summary += L"  |  SR " + Widen(std::string(ToString(s.ai.sr_output)));
    }
    return summary;
}

}  // namespace brp::next
