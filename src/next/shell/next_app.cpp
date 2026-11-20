#include "shell/next_app.h"

#include "ai/dlss_fg.h"
#include "overlay/overlay_view.h"
#include "diagnostics/metrics.h"
#include "media/nv_dlss_sr.h"
#include "media/pipeline.h"
#include "media/rife.h"
#include "platform/win32_window.h"
#include "shell/settings_controls.h"
#include "streaming/streaming_service.h"
#include "ui/ui_renderer.h"

#include <windows.h>
#include <commdlg.h>
#include <mmsystem.h>
#include <shellapi.h>
#include <avrt.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cwchar>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace brp::next {
namespace {

constexpr float kTitleHeight = 40.0f;
constexpr float kSidebar = 224.0f;
constexpr float kSidebarCollapsed = 68.0f;
constexpr float kPad = 28.0f;
constexpr wchar_t kVersion[] = L"1.0.0";

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

Color Alpha(const Color& color, float alpha) {
    return Color{color.r, color.g, color.b,
                 static_cast<std::uint8_t>(std::clamp(alpha, 0.0f, 1.0f) * 255)};
}

const wchar_t* QualityLabel(int quality) {
    static const wchar_t* names[] = {L"Performance", L"Balanced", L"Quality", L"Ultra",
                                     L"Ultra Quality", L"DLAA"};
    return names[std::clamp(quality, 0, 5)];
}

const wchar_t* ResolutionLabel(Resolution resolution) {
    switch (resolution) {
        case Resolution::Native: return L"Native";
        case Resolution::R1080: return L"1080p";
        case Resolution::R1440: return L"1440p";
        case Resolution::R2160: return L"2160p";
    }
    return L"Native";
}

const wchar_t* OverlayPositionLabel(int position) {
    static const wchar_t* names[] = {L"Top left", L"Top right", L"Bottom left",
                                     L"Bottom right"};
    return names[std::clamp(position, 0, 3)];
}

std::uint32_t RgbOf(const Color& color) {
    return (static_cast<std::uint32_t>(color.r) << 16) |
           (static_cast<std::uint32_t>(color.g) << 8) | color.b;
}
int OutputFps(const AppSettings& settings) {
    int fps = std::clamp(settings.streaming.fps, 24, 240);
    if (!settings.ai.frame_generation) {
        return fps;
    }
    if (settings.ai.fg_backend == AiBackend::DlssFg) {
        fps *= std::clamp(settings.ai.fg_multiplier, 2, 6);
    } else if (settings.ai.fg_backend == AiBackend::Rife) {
        fps *= 2;
    }
    return fps;
}
std::wstring BackendLabel(AiBackend backend) {
    switch (backend) {
        case AiBackend::DlssFg: return L"DLSS-G";
        case AiBackend::Rife: return L"RIFE";
        case AiBackend::NvidiaVfg: return L"NVIDIA VFG";
        case AiBackend::DlssSr: return L"DLSS SR";
        case AiBackend::TensorRtSr: return L"TensorRT SR";
        default: return L"Off";
    }
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
    overlay_.SetActionHandler(
        [this](OverlayAction action) { HandleOverlayAction(action); });
    paths_ = AppPaths::Default();
    host_ = std::make_unique<AppHost>(paths_);
}

NextApp::~NextApp() {
    StopRifeThread();
    StopDlssFg();
}

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
        if (watching_) {
            if (fg_covering_) {
                // The generator owns presentation. Wake on a decoded frame
                // instead of polling: the wait returns immediately when the
                // decoder queues one, and the timeout only keeps the overlay
                // and window fit alive when the source is idle.
                if (streaming_) {
                    streaming_->WaitForFrame(4);
                } else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(4));
                }
            } else if (presenter_->frame_waitable()) {
                ::WaitForSingleObject(presenter_->frame_waitable(), 8);
            }
        }
        Frame();
        if (!watching_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
    }
    ::timeEndPeriod(1);
    StopPadThread();
    StopRifeThread();
    StopDlssFg();
    overlay_.Destroy();
    if (streaming_) {
        streaming_->SetAudio(nullptr);
        streaming_->Disconnect();
    }
    audio_.Close();
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
    presenter_->SetUpscaleMode(2);
    ApplyPresenterTarget();

    StartupReport report;
    const Status status = host_->Initialize(report);
    if (!status.ok()) {
        MessageBoxW(window_->hwnd(), Widen(status.message()).c_str(),
                    L"BetterRemotePlay Next", MB_OK | MB_ICONERROR);
        return false;
    }
    settings_ = host_->Settings();
    {
        EnhanceParams enhance;
        enhance.active = settings_.video.sharpening > 0;
        enhance.sharpen =
            std::clamp(settings_.video.sharpening / 100.0f, 0.0f, 1.0f);
        presenter_->SetEnhance(enhance);
        if (auto* logger = dynamic_cast<Logger*>(host_->Log())) {
            logger->SetLevel(settings_.advanced.log_level);
        }
    }
    system_ = ProbeSystem();

    streaming_ = std::make_shared<StreamingService>(paths_.session_file,
                                                    paths_.legacy_settings);
    streaming_->Load();
    host_->Services().Set<IStreamingService>(
        std::static_pointer_cast<IStreamingService>(streaming_));
    {
        const session::Profile profile = streaming_->Profile();
        if (settings_.console.address.empty()) {
            settings_.console.address = profile.host;
        }
        if (settings_.console.name.empty()) {
            settings_.console.name = profile.name;
        }
    }

    RefreshProfiles();
    Resize();
    started_ = SecondsNow();
    last_frame_ = started_;
    haptic_strength_.store(settings_.controller.rumble_strength);
    StartPadThread();
    if (std::getenv("BRP_NEXT_BENCH")) {
        bench_exit_ = true;
        StartBenchmark();
    }
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
    window_->SetFrameMetrics(watching_ ? 0 : static_cast<int>(kTitleHeight * scale_),
                             watching_ ? 0 : static_cast<int>(370.0f * scale_));
    std::fprintf(stderr, "[next] window %dx%d scale %.3f logical %.1fx%.1f\n",
                 width, height, scale_, logical_width_, logical_height_);
}

void NextApp::Frame() {
    ProbeIfNeeded(now_);
    if (bench_active_) {
        TickBenchmark();
    }
    if (streaming_) {
        streaming_->Poll();
        if (watch_on_connect_ && streaming_->Connected()) {
            watch_on_connect_ = false;
            reconnect_attempts_ = 0;
            ToggleWatch(true);
        }
        if (watching_ && !streaming_->Connected()) {
            ToggleWatch(false);
            StopRifeThread();
            Toast(L"Stream ended", true);
        }
        const StreamSnapshot snapshot = streaming_->Snapshot();
        if (snapshot.connected) {
            reconnect_at_ = -1.0;
            reconnect_attempts_ = 0;
        } else if (!bench_active_ && !user_disconnect_ && connect_requested_ &&
                   snapshot.has_credentials &&
                   (snapshot.state == "idle" || snapshot.state == "failed") &&
                   snapshot.message.find("already in use") == std::string::npos &&
                   settings_.console.auto_reconnect && reconnect_attempts_ < 5) {
            if (reconnect_at_ < 0.0) {
                reconnect_at_ = now_ + settings_.console.reconnect_delay_ms / 1000.0;
            } else if (now_ >= reconnect_at_) {
                reconnect_at_ = -1.0;
                ++reconnect_attempts_;
                ConnectStream();
            }
        }
    }
    if (presenter_ && presenter_->device_lost()) {
        watching_ = false;
        StopRifeThread();
        StopDlssFg();
        streaming_->Disconnect();
        Toast(L"GPU device lost; restart the app", true);
    }
    HandlePinInput();
    const bool app_foreground = ::GetForegroundWindow() == window_->hwnd();
    const bool show_overlay =
        watching_ && app_foreground &&
        (settings_.overlay.enabled || overlay_control_);
    if (show_overlay) {
        const double interval =
            std::max(0.05, settings_.overlay.interval_ms / 1000.0);
        if (last_overlay_update_ < 0.0 || now_ - last_overlay_update_ >= interval) {
            BuildOverlayView();
            overlay_.Raise();
            last_overlay_update_ = now_;
        }
    } else {
        if (overlay_.visible()) {
            overlay_.Hide();
        }
        if (overlay_control_) {
            overlay_control_ = false;
        }
    }    if (rife_generated_at_ < 0.0 || now_ - rife_generated_at_ >= 1.0) {
        const int total = rife_generated_.load();
        rife_generated_rate_ = total - rife_generated_last_;
        rife_generated_last_ = total;
        rife_generated_at_ = now_;
        const int shown = display_shown_.load();
        display_rate_ = shown - display_shown_last_;
        display_shown_last_ = shown;
        cadence_p50_ = video_intervals_.Percentile(0.50);
        cadence_p99_ = video_intervals_.Percentile(0.99);
    }
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
    if (watching_) {
        PresentVideo();
        if (fg_covering_) {
            // The worker's window is the only visible surface. Presenting the
            // parent here would cost a full-size DWM composition per vblank.
            window_->set_hand_cursor(false);
            return;
        }
        renderer_->BeginFrame();
        renderer_->EndFrame();
        presenter_->PresentNow(true);
        return;
    }
    const float clear[4] = {theme.background.r / 255.0f, theme.background.g / 255.0f,
                            theme.background.b / 255.0f, 1.0f};
    presenter_->Clear(clear);
    renderer_->BeginFrame();
    renderer_->FillLinearV(Rect(0, 0, logical_width_, logical_height_),
                           ToD2D(theme.background), ToD2D(theme.background_bottom));

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
        case Page::About: RenderAboutPage(widgets); break;
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

float NextApp::PageHeader(Widgets& widgets, const wchar_t* eyebrow, const wchar_t* title,
                           const wchar_t* subtitle, const wchar_t* status, bool good) {
    const ThemeTokens& theme = widgets.Tokens();
    const float x = ContentX();
    const float w = ContentWidth();
    renderer_->Text(eyebrow, Rect(x, 52, x + w, 68), 10.0f, ToD2D(theme.accent),
                    DWRITE_FONT_WEIGHT_BOLD);
    renderer_->Text(title, Rect(x, 68, x + w - 220, 102), 26.0f, ToD2D(theme.text),
                    DWRITE_FONT_WEIGHT_SEMI_BOLD);
    renderer_->Text(subtitle, Rect(x, 102, x + w - 220, 122), 12.0f,
                    ToD2D(theme.text_dim));
    if (status) {
        widgets.StatusPill(Rect(x + w - 190, 76, x + w, 104), status,
                           good ? theme.success : theme.warning, true);
    }
    return 130.0f;
}
void NextApp::RenderTitleBar(Widgets& widgets) {
    const ThemeTokens& theme = widgets.Tokens();
    renderer_->FillRect(Rect(0, 0, logical_width_, kTitleHeight), ToD2D(theme.surface));
    renderer_->FillRect(Rect(0, kTitleHeight - 1, logical_width_, kTitleHeight),
                        ToD2D(theme.border));

    renderer_->FillRounded(Rect(12, 8, 36, 32), 7.0f, ToD2D(theme.accent));
    renderer_->Text(L"B", Rect(12, 8, 36, 32), 12.0f, ToD2D(theme.text),
                    DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
    renderer_->Text(L"BetterRemotePlay", Rect(46, 2, 220, 21), 12.5f,
                    ToD2D(theme.text), DWRITE_FONT_WEIGHT_SEMI_BOLD);
    renderer_->Text(L"NEXT  ·  1.0.0", Rect(46, 19, 220, 35), 9.0f,
                    ToD2D(theme.text_faint), DWRITE_FONT_WEIGHT_MEDIUM);

    const PageInfo* page = navigation_.Find(navigation_.Current());
    if (page) {
        const std::wstring crumb = Widen(page->group) + L"   /   " + Widen(page->title);
        renderer_->Text(crumb.c_str(), Rect(300, 0, logical_width_ - 560, kTitleHeight),
                        10.0f, ToD2D(theme.text_faint), DWRITE_FONT_WEIGHT_MEDIUM);
    }

    const float button = 36.0f;
    const float close_x = logical_width_ - 8.0f - button;
    const float max_x = close_x - button - 2.0f;
    const float min_x = max_x - button - 2.0f;
    const float theme_x = min_x - button - 8.0f;
    if (widgets.IconButton(9001, Rect(min_x, 4, min_x + button, kTitleHeight - 4),
                           L"\u2013")) {
        window_->Minimize();
    }
    if (widgets.IconButton(9002, Rect(max_x, 4, max_x + button, kTitleHeight - 4),
                           L"\u25A1", false, 13.0f)) {
        window_->ToggleMaximize();
    }
    if (widgets.IconButton(9003, Rect(close_x, 4, close_x + button, kTitleHeight - 4),
                           L"\u2715", true, 14.0f)) {
        window_->Close();
    }
    if (widgets.IconButton(9004, Rect(theme_x, 4, theme_x + button, kTitleHeight - 4),
                           settings_.general.theme == Theme::Dark ? L"\u263C"
                                                                  : L"\u263E",
                           false, 15.0f)) {
        settings_.general.theme =
            settings_.general.theme == Theme::Dark ? Theme::Light : Theme::Dark;
        Apply();
    }
    const StreamSnapshot snapshot =
        streaming_ ? streaming_->Snapshot() : StreamSnapshot{};
    const bool connected = snapshot.connected;
    const bool connecting = snapshot.connecting;
    const wchar_t* status = connected ? L"CONNECTED" : (connecting ? L"CONNECTING"
                                                                  : L"OFFLINE");
    const Color status_color = connected ? theme.success
                               : (connecting ? theme.warning : theme.text_faint);
    const float status_x = theme_x - 176.0f;
    renderer_->FillCircle(status_x + 5.0f, kTitleHeight * 0.5f, 3.5f,
                          ToD2D(status_color));
    renderer_->Text(status, Rect(status_x + 15.0f, 0, theme_x - 10.0f, kTitleHeight),
                    9.0f, ToD2D(status_color), DWRITE_FONT_WEIGHT_SEMI_BOLD);
}

void NextApp::RenderSidebar(Widgets& widgets) {
    const ThemeTokens& theme = widgets.Tokens();
    const float width = SidebarWidth();
    renderer_->FillRect(Rect(0, kTitleHeight, width, logical_height_),
                        ToD2D(theme.surface));
    renderer_->FillRect(Rect(width - 1, kTitleHeight, width, logical_height_),
                        ToD2D(theme.border));

    const D2D1_RECT_F collapse = Rect(10, kTitleHeight + 10, 40, kTitleHeight + 38);
    if (widgets.Button(9100, collapse, collapsed_ ? L">" : L"<", false)) {
        collapsed_ = !collapsed_;
    }

    float y = kTitleHeight + 52.0f;
    std::string last_group;
    int index = 0;
    for (const auto& page : navigation_.Pages()) {
        if (!collapsed_ && page.group != last_group) {
            last_group = page.group;
            widgets.Section(Rect(18, y, width - 14, y + 16),
                            Widen(page.group).c_str());
            y += 19.0f;
        }
        const D2D1_RECT_F item = Rect(8, y, width - 8, y + 34);
        const bool selected = navigation_.Current() == page.page;
        if (widgets.NavItem(9200 + index, item, Widen(page.title).c_str(), selected,
                            44)) {
            navigation_.Select(page.page);
        }
        DrawNavIcon(*renderer_, page.page, 26.0f, y + 17.0f,
                    selected ? theme.accent : theme.text_faint);
        y += 36.0f;
        ++index;
    }

    if (!collapsed_) {
        const StreamSnapshot snapshot =
            streaming_ ? streaming_->Snapshot() : StreamSnapshot{};
        const float card_top = logical_height_ - 132.0f;
        widgets.Card(Rect(12, card_top, width - 12, logical_height_ - 46));
        const bool connected = snapshot.connected;
        renderer_->FillCircle(24, card_top + 18, 3.5f,
                              ToD2D(connected ? theme.success : theme.text_faint));
        renderer_->Text(connected ? L"LIVE SESSION" : L"NO SESSION",
                        Rect(34, card_top + 8, width - 20, card_top + 30), 9.0f,
                        ToD2D(connected ? theme.success : theme.text_faint),
                        DWRITE_FONT_WEIGHT_BOLD);
        renderer_->TextEllipsis(
            snapshot.console_name.empty() ? L"Not connected"
                                          : Widen(snapshot.console_name).c_str(),
            Rect(18, card_top + 34, width - 20, card_top + 54), 11.5f,
            ToD2D(theme.text), DWRITE_FONT_WEIGHT_SEMI_BOLD);
        wchar_t line[96] = {};
        std::swprintf(line, 96, L"%d fps  |  %s", OutputFps(settings_),
                      settings_.ai.frame_generation
                          ? (settings_.ai.fg_backend == AiBackend::DlssFg ? L"DLSS-G"
                                                                          : L"RIFE")
                          : L"native");
        renderer_->Text(line, Rect(18, card_top + 54, width - 20, card_top + 74),
                        9.5f, ToD2D(theme.text_faint), DWRITE_FONT_WEIGHT_MEDIUM);
        renderer_->Text(L"F10  control center", Rect(18, card_top + 72, width - 20,
                                                    card_top + 90),
                        9.0f, ToD2D(theme.accent), DWRITE_FONT_WEIGHT_MEDIUM);

        renderer_->Text(L"BetterRemotePlay Next 1.0.0",
                        Rect(18, logical_height_ - 38, width - 14, logical_height_ - 22),
                        9.0f, ToD2D(theme.text_faint), DWRITE_FONT_WEIGHT_MEDIUM);
        renderer_->Text(L"stable release", Rect(18, logical_height_ - 22, width - 14,
                                                logical_height_ - 8),
                        8.5f, ToD2D(theme.text_faint));
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
    const StreamSnapshot snapshot =
        streaming_ ? streaming_->Snapshot() : StreamSnapshot{};

    const bool connected = snapshot.connected;
    PageHeader(widgets, L"OVERVIEW", L"Home",
               L"Session, pipeline and hardware at a glance",
               connected ? L"LIVE" : nullptr, true);

    const float gap = 14.0f;
    const float tile_w = (w - gap * 2.0f) / 3.0f;
    const float tile_y = 138.0f;
    const float tile_h = 104.0f;
    auto tile = [&](int index, const wchar_t* caption, const wchar_t* value,
                    const std::wstring& detail, const Color& color) {
        const float cx = x + (tile_w + gap) * index;
        widgets.Card(Rect(cx, tile_y, cx + tile_w, tile_y + tile_h));
        widgets.Section(Rect(cx + 16, tile_y + 12, cx + tile_w - 16, tile_y + 28),
                        caption);
        renderer_->TextEllipsis(value, Rect(cx + 16, tile_y + 32, cx + tile_w - 70,
                                            tile_y + 60),
                                18.0f, ToD2D(theme.text), DWRITE_FONT_WEIGHT_SEMI_BOLD);
        renderer_->FillCircle(cx + tile_w - 26.0f, tile_y + 44.0f, 5.0f, ToD2D(color));
        renderer_->TextEllipsis(detail.c_str(),
                                Rect(cx + 16, tile_y + 66, cx + tile_w - 16, tile_y + 88),
                                10.5f, ToD2D(theme.text_faint));
    };
    tile(0, L"SESSION",
         snapshot.console_name.empty() ? L"No console" : Widen(snapshot.console_name).c_str(),
         connected ? (std::wstring(L"Connected  |  ") +
                      Widen(snapshot.console_address))
                   : L"Press Discover on the Console page",
         connected ? theme.success : theme.text_faint);
    const bool sr_on = settings_.ai.super_resolution;
    const bool dlaa = sr_on && (settings_.ai.sr_quality >= 5 ||
                                settings_.ai.sr_output == Resolution::R1080);
    const bool fg_on = settings_.ai.frame_generation;
    std::wstring pipeline = sr_on ? (dlaa ? L"DLAA" : L"DLSS SR") : L"Native";
    pipeline += L"  +  ";
    pipeline += fg_on ? (settings_.ai.fg_backend == AiBackend::DlssFg
                             ? L"DLSS-G " + std::to_wstring(settings_.ai.fg_multiplier) +
                                   L"x"
                             : L"RIFE 2x")
                      : L"No frame generation";
    tile(1, L"PIPELINE", pipeline.c_str(),
         std::to_wstring(OutputFps(settings_)) + L" fps output  |  " +
             Widen(std::string(ToString(settings_.video.output))),
         fg_on ? theme.accent : theme.text_faint);
    wchar_t perf[160] = {};
    std::swprintf(perf, 160, L"%.1f / %.1f ms cadence  |  %.1f ms decode",
                  cadence_p50_, cadence_p99_, snapshot.decode_ms);
    tile(2, L"PERFORMANCE",
         (std::to_wstring(static_cast<int>(snapshot.source_fps)) + L" fps source").c_str(),
         connected ? perf : L"Waiting for a session", theme.accent);

    const float flow_y = 256.0f;
    const float flow_h = 118.0f;
    widgets.Card(Rect(x, flow_y, x + w, flow_y + flow_h));
    widgets.Section(Rect(x + 16, flow_y + 12, x + w - 16, flow_y + 28), L"ACTIVE PIPELINE");
    const float seg_gap = 18.0f;
    const float seg_w = (w - 32.0f - seg_gap * 3.0f) / 4.0f;
    const float seg_y = flow_y + 38.0f;
    const float seg_h = 62.0f;
    struct Step {
        const wchar_t* label;
        std::wstring value;
        std::wstring detail;
        bool active;
    };
    const Step steps[4] = {
        {L"DECODE", snapshot.connected ? Widen(settings_.streaming.decoder)
                                       : L"idle",
         std::to_wstring(snapshot.source_width) + L"x" +
             std::to_wstring(snapshot.source_height),
         snapshot.connected},
        {L"SUPER RESOLUTION", sr_on ? (dlaa ? L"DLAA" : Widen(std::string(ToString(settings_.ai.sr_output))))
                                    : L"Off",
         sr_on ? L"NGX on the decoded frame" : L"presenter upscale", sr_on},
        {L"FRAME GENERATION",
         fg_on ? (settings_.ai.fg_backend == AiBackend::DlssFg
                      ? L"DLSS-G " + std::to_wstring(settings_.ai.fg_multiplier) + L"x"
                      : L"RIFE 2x")
               : L"Off",
         fg_on ? (std::to_wstring(OutputFps(settings_)) + L" fps") : L"source cadence",
         fg_on},
        {L"PRESENT", (std::to_wstring(OutputFps(settings_)) + L" fps").c_str(),
         window_ && window_->fullscreen() ? L"borderless fullscreen" : L"windowed",
         true},
    };
    for (int i = 0; i < 4; ++i) {
        const float cx = x + 16.0f + (seg_w + seg_gap) * i;
        renderer_->FillRounded(Rect(cx, seg_y, cx + seg_w, seg_y + seg_h), 10.0f,
                               ToD2D(theme.surface_alt));
        if (steps[i].active) {
            renderer_->StrokeRounded(Rect(cx, seg_y, cx + seg_w, seg_y + seg_h), 10.0f,
                                     ToD2D(Alpha(theme.accent, 0.35f)),
                                     1.0f);
        }
        renderer_->FillCircle(cx + 16.0f, seg_y + 16.0f, 4.0f,
                              ToD2D(steps[i].active ? theme.accent : theme.text_faint));
        renderer_->Text(steps[i].label, Rect(cx + 26, seg_y + 6, cx + seg_w - 10,
                                             seg_y + 26),
                        8.5f, ToD2D(theme.text_faint), DWRITE_FONT_WEIGHT_BOLD);
        renderer_->TextEllipsis(steps[i].value.c_str(),
                                Rect(cx + 12, seg_y + 26, cx + seg_w - 10, seg_y + 46),
                                12.5f, ToD2D(theme.text), DWRITE_FONT_WEIGHT_SEMI_BOLD);
        renderer_->TextEllipsis(steps[i].detail.c_str(),
                                Rect(cx + 12, seg_y + 45, cx + seg_w - 10, seg_y + 60),
                                9.5f, ToD2D(theme.text_faint));
    }

    const float sys_y = 388.0f;
    const float sys_h = 142.0f;
    widgets.Card(Rect(x, sys_y, x + w, sys_y + sys_h));
    widgets.Section(Rect(x + 16, sys_y + 12, x + w - 16, sys_y + 28), L"SYSTEM");
    const float col = w / 3.0f;
    auto sys_col = [&](int index, const wchar_t* caption, const std::wstring& title,
                       const std::vector<std::wstring>& lines) {
        const float cx = x + 16.0f + col * index;
        renderer_->Text(caption, Rect(cx, sys_y + 38, cx + col - 24, sys_y + 54), 8.5f,
                        ToD2D(theme.text_faint), DWRITE_FONT_WEIGHT_BOLD);
        renderer_->TextEllipsis(title.c_str(), Rect(cx, sys_y + 56, cx + col - 24,
                                                   sys_y + 78),
                                13.0f, ToD2D(theme.text), DWRITE_FONT_WEIGHT_SEMI_BOLD);
        float ly = sys_y + 82.0f;
        for (const auto& line : lines) {
            renderer_->TextEllipsis(line.c_str(), Rect(cx, ly, cx + col - 24, ly + 18),
                                    9.5f, ToD2D(theme.text_dim));
            ly += 18.0f;
        }
    };
    sys_col(0, L"OPERATING SYSTEM",
            Widen(system_.os).empty() ? L"Windows" : Widen(system_.os),
            {Widen(system_.cpu), std::to_wstring(system_.logical_cores) + L" logical cores",
             std::to_wstring(system_.ram_mb / 1024) + L" GB RAM"});
    sys_col(1, L"GRAPHICS",
            system_.gpu.present ? Widen(system_.gpu.name) : L"Not detected",
            {std::to_wstring(system_.gpu.vram_mb / 1024) + L" GB VRAM",
             system_.gpu.driver.empty() ? L"Driver unknown"
                                        : L"Driver " + Widen(system_.gpu.driver),
             system_.gpu.nvidia ? L"NVIDIA" : L"Non-NVIDIA"});
    std::wstring display_title = L"No display";
    if (!system_.displays.empty()) {
        display_title = std::to_wstring(system_.displays[0].width) + L"x" +
                        std::to_wstring(system_.displays[0].height) + L" @ " +
                        std::to_wstring(system_.displays[0].refresh_hz) + L" Hz";
    }
    sys_col(2, L"DISPLAY", display_title,
            {std::to_wstring(system_.displays.size()) + L" monitor(s)",
             L"Output: " + Widen(std::string(ToString(settings_.video.output))),
             window_ && window_->fullscreen() ? L"Fullscreen" : L"Windowed"});

    const float ai_y = 548.0f;
    widgets.Card(Rect(x, ai_y, x + w, ai_y + 168.0f));
    widgets.Section(Rect(x + 16, ai_y + 12, x + w - 240, ai_y + 28), L"AI CAPABILITIES");
    struct Capability {
        const wchar_t* title;
        bool available;
        const wchar_t* status;
        const wchar_t* detail;
    };
    const Capability caps[] = {
        {L"DLSS Super Resolution", system_.dlss_sr,
         system_.dlss_sr ? L"AVAILABLE" : L"MISSING", L"nvngx_dlss.dll"},
        {L"DLSS Frame Generation", system_.dlss_fg,
         system_.dlss_fg ? L"AVAILABLE" : L"MISSING",
         system_.dlss_fg_multi ? L"up to 6x" : L"2x"},
        {L"RIFE Lite 4.25", system_.rife_lite,
         system_.rife_lite ? L"AVAILABLE" : L"MISSING", L"TensorRT 1080p"},
        {L"RIFE Heavy 4.26", system_.rife_heavy,
         system_.rife_heavy ? L"AVAILABLE" : L"MISSING", L"engine on demand"},
        {L"TensorRT", system_.tensorrt_runtime,
         system_.tensorrt_runtime ? L"AVAILABLE" : L"MISSING", L"nvinfer_10"},
    };
    int available = 0;
    for (const auto& cap : caps) {
        available += cap.available ? 1 : 0;
    }
    renderer_->Text((std::to_wstring(available) + L" available").c_str(),
                    Rect(x + w - 220, ai_y + 12, x + w - 16, ai_y + 28), 9.5f,
                    ToD2D(theme.text_faint), DWRITE_FONT_WEIGHT_MEDIUM,
                    DWRITE_TEXT_ALIGNMENT_TRAILING);
    const float cap_gap = 10.0f;
    const float cap_w = (w - 32.0f - cap_gap * 4.0f) / 5.0f;
    const float cap_y = ai_y + 38.0f;
    for (int i = 0; i < 5; ++i) {
        const float cx = x + 16.0f + (cap_w + cap_gap) * i;
        const D2D1_RECT_F r = Rect(cx, cap_y, cx + cap_w, cap_y + 94.0f);
        renderer_->FillRounded(r, 10.0f, ToD2D(theme.surface_alt));
        renderer_->FillCircle(cx + 15.0f, cap_y + 18.0f, 4.0f,
                              ToD2D(caps[i].available ? theme.success : theme.warning));
        renderer_->TextEllipsis(caps[i].title, Rect(cx + 26, cap_y + 8, cx + cap_w - 10,
                                                   cap_y + 30),
                                10.5f, ToD2D(theme.text), DWRITE_FONT_WEIGHT_SEMI_BOLD);
        renderer_->Text(caps[i].status, Rect(cx + 12, cap_y + 38, cx + cap_w - 10,
                                             cap_y + 54),
                        8.5f, ToD2D(caps[i].available ? theme.success : theme.warning),
                        DWRITE_FONT_WEIGHT_BOLD);
        renderer_->TextEllipsis(caps[i].detail, Rect(cx + 12, cap_y + 56, cx + cap_w - 10,
                                                     cap_y + 76),
                                8.5f, ToD2D(theme.text_faint));
    }
    renderer_->Text(L"Capabilities are file checks. DLSS SR and DLSS-G run in Watch; "
                    L"RIFE is the in-process 30 -> 60 path.",
                    Rect(x + 16, ai_y + 142, x + w - 16, ai_y + 158), 9.0f,
                    ToD2D(theme.text_faint));

    const float actions_y = 730.0f;
    const float action_w = (w - 12.0f * 4.0f) / 5.0f;
    if (widgets.Button(9301, Rect(x, actions_y, x + action_w, actions_y + 38),
                       L"AI Video Engine")) {
        navigation_.Select(Page::AiVideo);
    }
    if (widgets.Button(9302, Rect(x + action_w + 12, actions_y,
                                  x + action_w * 2 + 12, actions_y + 38),
                       L"Profiles", false)) {
        navigation_.Select(Page::Profiles);
    }
    if (widgets.Button(9303, Rect(x + (action_w + 12) * 2, actions_y,
                                  x + action_w * 3 + 24, actions_y + 38),
                       L"Diagnostics", false)) {
        navigation_.Select(Page::Diagnostics);
    }
    if (widgets.Button(9304, Rect(x + (action_w + 12) * 3, actions_y,
                                  x + action_w * 4 + 36, actions_y + 38),
                       L"Settings", false)) {
        navigation_.Select(Page::Settings);
    }
    if (widgets.Button(9305, Rect(x + (action_w + 12) * 4, actions_y,
                                  x + action_w * 5 + 48, actions_y + 38),
                       L"About", false)) {
        navigation_.Select(Page::About);
    }
}
void NextApp::RenderConsole(Widgets& widgets) {
    const ThemeTokens& theme = widgets.Tokens();
    const float x = ContentX();
    const float w = ContentWidth();
    PageHeader(widgets, L"STREAMING", L"Console", L"Discovery, account, pairing and session control");

    const StreamSnapshot snapshot = streaming_ ? streaming_->Snapshot() : StreamSnapshot{};
    const session::Profile profile = streaming_ ? streaming_->Profile() : session::Profile{};
    Color state_color = theme.text_dim;
    if (snapshot.connected) {
        state_color = theme.success;
    } else if (snapshot.state == "connecting" || snapshot.pairing) {
        state_color = theme.warning;
    } else if (snapshot.state == "failed") {
        state_color = theme.error;
    }

    float y = 136.0f;
    widgets.Card(Rect(x, y, x + w, y + 180.0f));
    widgets.StatusPill(Rect(x + 18, y + 14, x + 150, y + 44),
                       Widen(snapshot.state).c_str(), state_color, true);
    widgets.StatusPill(Rect(x + w - 160, y + 14, x + w - 18, y + 44),
                       snapshot.has_credentials ? L"PAIRED" : L"NOT PAIRED",
                       snapshot.has_credentials ? theme.success : theme.warning, true);
    const std::wstring name = snapshot.console_name.empty()
                                  ? L"No console selected"
                                  : Widen(snapshot.console_name);
    renderer_->TextEllipsis(name.c_str(), Rect(x + 18, y + 50, x + w - 200, y + 78),
                            18.0f, ToD2D(theme.text), DWRITE_FONT_WEIGHT_SEMI_BOLD);
    renderer_->Text(snapshot.console_address.empty()
                        ? L"Address: not set"
                        : (L"Address: " + Widen(snapshot.console_address)).c_str(),
                    Rect(x + 18, y + 80, x + w - 200, y + 102), 11.5f,
                    ToD2D(theme.text_dim));
    renderer_->TextEllipsis(Widen(snapshot.message).c_str(),
                            Rect(x + 18, y + 104, x + w - 18, y + 126), 11.0f,
                            ToD2D(snapshot.connected ? theme.success : theme.text_dim));

    const float button_w = 158.0f;
    if (widgets.Button(2501, Rect(x + 18, y + 132, x + 18 + button_w, y + 170),
                       snapshot.discovering ? L"Searching..." : L"Discover", false,
                       !snapshot.discovering)) {
        const Status status = streaming_->StartDiscovery();
        if (!status.ok()) {
            Toast(Widen(status.message()), true);
        } else {
            Toast(L"Searching for consoles...");
        }
    }
    if (snapshot.connected) {
        if (widgets.Button(2502, Rect(x + 18 + button_w + 10, y + 132,
                                      x + 18 + button_w * 2 + 10, y + 170),
                           L"Disconnect", false)) {
            DisconnectStream();
        }
    } else {
        if (widgets.Button(2502, Rect(x + 18 + button_w + 10, y + 132,
                                      x + 18 + button_w * 2 + 10, y + 170),
                           L"Connect", true,
                           !snapshot.connecting && snapshot.has_credentials)) {
            ConnectStream();
        }
    }
    if (widgets.Button(2503, Rect(x + (button_w + 10) * 2 + 18, y + 132,
                                  x + (button_w + 10) * 2 + 138, y + 170),
                       L"Save session", false)) {
        const Status status = streaming_->SaveProfile();
        Toast(status.ok() ? L"Session saved" : Widen(status.message()), !status.ok());
    }
    if (snapshot.connected && !watching_) {
        if (widgets.Button(2504, Rect(x + (button_w + 10) * 3 + 18, y + 132,
                                      x + (button_w + 10) * 3 + 158, y + 170),
                           L"Watch stream")) {
            ToggleWatch(true);
        }
    }

    y += 194.0f;
    widgets.Card(Rect(x, y, x + w, y + 142.0f));
    widgets.Section(Rect(x + 18, y + 12, x + w - 18, y + 28), L"DISCOVERY");
    const auto candidates = streaming_ ? streaming_->Discovered() : std::vector<ConsoleCandidate>{};
    if (snapshot.discovering) {
        renderer_->Text(L"Broadcasting on the LAN; this takes about 1.5 s.",
                        Rect(x + 18, y + 40, x + w - 18, y + 66), 11.5f,
                        ToD2D(theme.text_dim));
    } else if (candidates.empty()) {
        renderer_->Text(L"No consoles found yet. Press Discover while the PS5 is awake.",
                        Rect(x + 18, y + 40, x + w - 18, y + 66), 11.5f,
                        ToD2D(theme.text_faint));
    } else {
        float cy = y + 38.0f;
        for (std::size_t i = 0; i < candidates.size() && i < 3; ++i) {
            const auto& console = candidates[i];
            const std::wstring line = Widen(console.name.empty() ? console.address
                                                                 : console.name) +
                                      L"  -  " + Widen(console.address);
            renderer_->TextEllipsis(line.c_str(), Rect(x + 18, cy, x + w - 200, cy + 26),
                                    12.5f, ToD2D(theme.text));
            if (widgets.Button(2510 + static_cast<int>(i),
                               Rect(x + w - 170, cy - 4, x + w - 18, cy + 28),
                               L"Use", false)) {
                if (streaming_->SetConsole(console).ok()) {
                    settings_.console.name = console.name;
                    settings_.console.address = console.address;
                    Apply();
                    Toast(L"Console selected");
                } else {
                    Toast(L"Could not save the console", true);
                }
            }
            cy += 32.0f;
        }
    }

    y += 156.0f;
    widgets.Card(Rect(x, y, x + w, y + 106.0f));
    widgets.Section(Rect(x + 18, y + 12, x + w - 18, y + 28), L"PSN ACCOUNT");
    renderer_->Text(profile.psn_numeric_id.empty()
                        ? L"No PSN account linked"
                        : (L"Linked: numeric id " + Widen(profile.psn_numeric_id)).c_str(),
                    Rect(x + 18, y + 38, x + w - 380, y + 62), 13.0f,
                    ToD2D(profile.psn_numeric_id.empty() ? theme.warning : theme.success),
                    DWRITE_FONT_WEIGHT_MEDIUM);
    if (widgets.Button(2520, Rect(x + w - 360, y + 34, x + w - 190, y + 72),
                       L"Open PSN login", false)) {
        std::string detail;
        const Status status = streaming_->OpenPsnLogin(detail);
        Toast(status.ok() ? L"Browser opened; sign in and copy the final URL"
                          : Widen(detail),
              !status.ok());
    }
    if (widgets.Button(2521, Rect(x + w - 180, y + 34, x + w - 18, y + 72),
                       L"Paste redirect", false)) {
        const std::wstring text = ClipboardText();
        if (text.empty()) {
            Toast(L"Clipboard is empty", true);
        } else {
            std::string detail;
            const Status status = streaming_->CompletePsnLogin(Narrow(text), detail);
            Toast(status.ok() ? L"PSN account linked" : Widen(detail), !status.ok());
        }
    }

    y += 120.0f;
    widgets.Card(Rect(x, y, x + w, y + 110.0f));
    widgets.Section(Rect(x + 18, y + 12, x + w - 18, y + 28), L"PAIRING PIN");
    const D2D1_RECT_F pin_rect = Rect(x + 18, y + 48, x + 238, y + 88);
    renderer_->FillRounded(pin_rect, 8.0f, ToD2D(theme.surface_alt));
    renderer_->StrokeRounded(pin_rect, 8.0f,
                            ToD2D(pin_focus_ ? theme.accent : theme.border), 1.2f);
    std::wstring pin_text = Widen(pin_);
    while (pin_text.size() < 8) {
        pin_text += L"_";
    }
    renderer_->Text(pin_text.c_str(), Rect(pin_rect.left + 12, pin_rect.top,
                                          pin_rect.right - 12, pin_rect.bottom),
                    16.0f, ToD2D(theme.text), DWRITE_FONT_WEIGHT_SEMI_BOLD);
    if (widgets.HitTarget(2523, pin_rect)) {
        pin_focus_ = true;
    }
    if (widgets.Button(2522, Rect(x + 252, y + 48, x + 420, y + 88), L"Pair",
                       true, pin_.size() == 8)) {
        try {
            const Status status = streaming_->Pair(static_cast<std::uint32_t>(
                std::stoul(pin_)));
            Toast(status.ok() ? L"Pairing..." : Widen(status.message()), !status.ok());
        } catch (...) {
            Toast(L"The PIN must be 8 digits", true);
        }
    }
    renderer_->Text((L"PS5: Settings > System > Remote Play > Link Device. "
                     + Widen(snapshot.pairing_message)).c_str(),
                    Rect(x + 18, y + 92, x + w - 18, y + 110), 10.5f,
                    ToD2D(theme.text_faint));

    y += 124.0f;
    widgets.Card(Rect(x, y, x + w, y + 124.0f));
    widgets.Section(Rect(x + 18, y + 12, x + w - 18, y + 28), L"STREAM");
    if (snapshot.connected) {
        wchar_t line[256] = {};
        std::swprintf(line, 256,
                      L"Source %.1f fps  |  decode %.2f ms  |  bitrate %.0f kbps  |  "
                      L"loss %.2f%%",
                      snapshot.source_fps, snapshot.decode_ms, snapshot.bitrate_kbps,
                      snapshot.loss_percent);
        renderer_->Text(line, Rect(x + 18, y + 38, x + w - 18, y + 64), 12.5f,
                        ToD2D(theme.success), DWRITE_FONT_WEIGHT_MEDIUM);
        const brp::PadState pad_state = pad_.snapshot();
        const std::wstring audio = audio_.is_open()
                                       ? L"Audio: " + Widen(audio_.device_name()) +
                                             L" (" +
                                             std::to_wstring(settings_.audio.buffer_ms) +
                                             L" ms)"
                                       : (audio_note_.empty() ? L"Audio: unavailable"
                                                              : audio_note_);
        renderer_->Text(audio.c_str(), Rect(x + 18, y + 66, x + w - 18, y + 88), 10.5f,
                        ToD2D(theme.text_dim));
        renderer_->Text((pad_state.connected
                             ? L"Controller: " + Widen(pad_state.name)
                             : L"Controller: not detected")
                            .c_str(),
                        Rect(x + 18, y + 88, x + w - 18, y + 110), 10.5f,
                        ToD2D(theme.text_dim));
    } else {
        renderer_->Text(L"Not streaming. Connect to start a real session; video then opens "
                        L"in the Watch view.",
                        Rect(x + 18, y + 38, x + w - 18, y + 66), 11.5f,
                        ToD2D(theme.text_faint));
    }
}
void NextApp::RenderStreaming(Widgets& widgets) {
    const ThemeTokens& theme = widgets.Tokens();
    const float x = ContentX();
    const float w = ContentWidth();
    PageHeader(widgets, L"STREAMING", L"Streaming", L"Transport, decoder, audio and input configuration");

    const StreamSnapshot snapshot =
        streaming_ ? streaming_->Snapshot() : StreamSnapshot{};
    const bool connected = snapshot.connected;
    const bool connecting = snapshot.connecting;
    widgets.Card(Rect(x, 138, x + w, 224));
    widgets.StatusPill(Rect(x + 18, 154, x + 150, 184),
                       connected ? L"CONNECTED" : (connecting ? L"CONNECTING"
                                                              : L"IDLE"),
                       connected ? theme.success
                                 : (connecting ? theme.warning : theme.text_faint),
                       true);
    renderer_->Text(connected ? L"Streaming service: live"
                              : L"Streaming service: waiting",
                    Rect(x + 166, 154, x + w - 18, 184), 13.0f, ToD2D(theme.text),
                    DWRITE_FONT_WEIGHT_MEDIUM);
    wchar_t stream_line[180] = {};
    std::swprintf(stream_line, 180, L"%dx%d @ %.1f fps  |  decode %.2f ms  |  %.0f kbps",
                  snapshot.source_width, snapshot.source_height, snapshot.source_fps,
                  snapshot.decode_ms, snapshot.bitrate_kbps);
    renderer_->Text(connected ? stream_line : L"Connect from the Console page to "
                                                L"start a session.",
                    Rect(x + 18, 188, x + w - 18, 212), 10.5f,
                    ToD2D(theme.text_faint));

    SettingsControls controls(widgets, settings_, system_, true);
    controls.Reset();
    float controls_y = 238.0f;
    if (stream_restart_pending_) {
        widgets.StatusPill(Rect(x, 232, x + 330, 262), L"RECONNECT REQUIRED",
                           theme.warning, true);
        renderer_->Text(L"Transport settings changed; reconnect from Console to apply.",
                        Rect(x + 342, 232, x + w, 262), 10.5f, ToD2D(theme.warning),
                        DWRITE_FONT_WEIGHT_MEDIUM);
        controls_y = 274.0f;
    }
    controls.Streaming(x, controls_y, w);
    if (controls.Changed()) {
        Apply();
    }
}

void NextApp::RenderAi(Widgets& widgets) {
    const ThemeTokens& theme = widgets.Tokens();
    const float x = ContentX();
    const float w = ContentWidth();
    int available = 0;
    available += system_.dlss_sr ? 1 : 0;
    available += system_.dlss_fg ? 1 : 0;
    available += system_.rife_lite ? 1 : 0;
    available += system_.tensorrt_runtime ? 1 : 0;
    PageHeader(widgets, L"PROCESSING", L"AI Video Engine",
               L"Super resolution, frame generation and inference runtimes",
               available >= 3 ? L"READY" : L"LIMITED", available >= 3);

    const float gap = 10.0f;
    const float card_w = (w - gap * 3.0f) / 4.0f;
    auto capability_card = [&](int index, const wchar_t* title, const wchar_t* status,
                               bool good, const wchar_t* detail) {
        const float cx = x + (card_w + gap) * index;
        widgets.Card(Rect(cx, 138, cx + card_w, 214));
        renderer_->FillCircle(cx + 16, 156, 4.0f,
                              ToD2D(good ? theme.success : theme.warning));
        renderer_->TextEllipsis(title, Rect(cx + 28, 146, cx + card_w - 12, 168),
                                11.5f, ToD2D(theme.text), DWRITE_FONT_WEIGHT_SEMI_BOLD);
        renderer_->Text(status, Rect(cx + 14, 176, cx + card_w - 12, 192), 8.5f,
                        ToD2D(good ? theme.success : theme.warning),
                        DWRITE_FONT_WEIGHT_BOLD);
        renderer_->TextEllipsis(detail, Rect(cx + 14, 192, cx + card_w - 12, 208), 9.0f,
                                ToD2D(theme.text_faint));
    };
    capability_card(0, L"DLSS Super Resolution", system_.dlss_sr ? L"AVAILABLE" : L"MISSING",
                    system_.dlss_sr, L"nvngx_dlss.dll");
    capability_card(1, L"DLSS Frame Generation", system_.dlss_fg ? L"AVAILABLE" : L"MISSING",
                    system_.dlss_fg,
                    system_.dlss_fg_multi ? L"NVIDIA + SM86 proxy" : L"NVIDIA 2x");
    capability_card(2, L"RIFE TensorRT", system_.rife_lite ? L"AVAILABLE" : L"MISSING",
                    system_.rife_lite, L"30 -> 60 in-process");
    capability_card(3, L"TensorRT runtime", system_.tensorrt_runtime ? L"AVAILABLE"
                                                                      : L"MISSING",
                    system_.tensorrt_runtime, L"nvinfer_10");

    const float tab_y = 228.0f;
    const float tab_w = (w - 16.0f) / 3.0f;
    const wchar_t* tabs[] = {L"Super Resolution", L"Frame Generation", L"GPU & Memory"};
    for (int i = 0; i < 3; ++i) {
        const D2D1_RECT_F r = Rect(x + (tab_w + 8.0f) * i, tab_y,
                                   x + (tab_w + 8.0f) * i + tab_w, tab_y + 38);
        if (widgets.Button(9700 + i, r, tabs[i], ai_tab_ == i)) {
            ai_tab_ = i;
        }
    }

    widgets.Card(Rect(x, 280, x + w, logical_height_ - 28));
    SettingsControls controls(widgets, settings_, system_, true);
    controls.Reset();
    float end = 302.0f;
    if (ai_tab_ == 0) {
        end = controls.SuperResolution(x + 18, end, w - 36);
    } else if (ai_tab_ == 1) {
        end = controls.FrameGeneration(x + 18, end, w - 36);
    } else {
        end = controls.Gpu(x + 18, end, w - 36);
    }
    if (controls.Changed()) {
        Apply();
    }
    if (end < logical_height_ - 70) {
        renderer_->Text(ai_tab_ == 0
                            ? L"DLAA is native AA at the stream size. An SR upscale "
                              L"adds a readback before the generator."
                            : (ai_tab_ == 1
                                   ? L"2x is the clean cadence. 3x-6x need the SM86 "
                                     L"proxy and land between display refreshes."
                                   : L"Inference runs on the adapter above. FP16 is "
                                     L"the validated path on this machine."),
                        Rect(x + 18, end + 6, x + w - 18, end + 26), 10.0f,
                        ToD2D(theme.text_faint));
    }
}
void NextApp::RenderDisplay(Widgets& widgets) {
    const ThemeTokens& theme = widgets.Tokens();
    const float x = ContentX();
    const float w = ContentWidth();
    PageHeader(widgets, L"PROCESSING", L"Display", L"Detected monitors, presentation and fullscreen behaviour");

    widgets.Card(Rect(x, 138, x + w, 310));
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
    renderer_->Text(L"These are the modes Windows reports for the current monitor.",
                    Rect(x + 18, y + 4, x + w - 18, y + 26), 10.5f,
                    ToD2D(theme.text_faint));

    widgets.Card(Rect(x, 332, x + w, 462));
    widgets.Section(Rect(x + 18, 346, x + w - 18, 362), L"PRESENTATION");
    SettingsControls controls(widgets, settings_, system_, true);
    controls.Reset();
    controls.Display(x + 18, 372, w - 36);
    if (controls.Changed()) {
        Apply();
    }
}

void NextApp::RenderOverlay(Widgets& widgets) {
    const ThemeTokens& theme = widgets.Tokens();
    const float x = ContentX();
    const float w = ContentWidth();
    PageHeader(widgets, L"WORKSPACE", L"Overlay", L"Independent statistics panel and live control center");

    const D2D1_RECT_F preview = Rect(x, 138, x + 540, 320);
    widgets.Card(preview);
    renderer_->FillRounded(Rect(preview.left + 18, preview.top + 18, preview.right - 18,
                                preview.bottom - 18),
                           10.0f, ToD2D(theme.background));
    renderer_->Text(L"CONTROL CENTER PREVIEW",
                    Rect(preview.left + 22, preview.top + 14,
                         preview.left + 280, preview.top + 34),
                    11.0f, ToD2D(theme.text_faint), DWRITE_FONT_WEIGHT_SEMI_BOLD);
    const float box_w = 220.0f;
    const float box_h = 96.0f;
    float ox = preview.left + 24;
    float oy = preview.top + 40;
    if ((settings_.overlay.position & 1) != 0) {
        ox = preview.right - box_w - 24;
    }
    if (settings_.overlay.position >= 2) {
        oy = preview.bottom - box_h - 20;
    }
    const Color panel = theme.surface_alt;
    renderer_->FillRounded(Rect(ox, oy, ox + box_w, oy + box_h), 8.0f,
                           ToD2D(Color{panel.r, panel.g, panel.b,
                                       static_cast<std::uint8_t>(
                                           std::clamp(settings_.overlay.opacity, 0.1f, 1.0f) *
                                           255)}));
    renderer_->StrokeRounded(Rect(ox, oy, ox + box_w, oy + box_h), 8.0f,
                            ToD2D(theme.accent), 1.0f);
    const StreamSnapshot snapshot =
        streaming_ ? streaming_->Snapshot() : StreamSnapshot{};
    wchar_t preview_fps[64] = {};
    std::swprintf(preview_fps, 64, L"%d fps", OutputFps(settings_));
    renderer_->Text(preview_fps, Rect(ox + 12, oy + 8, ox + box_w - 12, oy + 34),
                    18.0f, ToD2D(theme.text), DWRITE_FONT_WEIGHT_SEMI_BOLD);
    std::swprintf(preview_fps, 64, L"source %.1f fps  |  %.2f ms decode",
                  snapshot.source_fps, snapshot.decode_ms);
    renderer_->Text(preview_fps, Rect(ox + 12, oy + 40, ox + box_w - 12, oy + 62),
                    10.0f, ToD2D(theme.text_dim));
    std::swprintf(preview_fps, 64, L"cadence %.1f / %.1f ms  |  F10 controls",
                  cadence_p50_, cadence_p99_);
    renderer_->Text(preview_fps, Rect(ox + 12, oy + 64, ox + box_w - 12, oy + 86),
                    9.5f, ToD2D(theme.text_faint));

    widgets.Card(Rect(x, 332, x + w, logical_height_ - 34));
    widgets.Section(Rect(x + 18, 346, x + w - 18, 362), L"OVERLAY BEHAVIOUR");
    SettingsControls controls(widgets, settings_, system_, true);
    controls.Reset();
    const float end = controls.Overlay(x + 18, 372, w - 36);
    if (controls.Changed()) {
        Apply();
    }
    renderer_->Text(L"F10 opens the interactive control center while streaming. Compact mode "
                    L"is click-through; the control center is mouse and keyboard driven.",
                    Rect(x, end + 4, x + w, end + 24), 10.5f, ToD2D(theme.text_faint));
}

void NextApp::RenderProfiles(Widgets& widgets) {
    const ThemeTokens& theme = widgets.Tokens();
    const float x = ContentX();
    const float w = ContentWidth();
    PageHeader(widgets, L"WORKSPACE", L"Profiles",
               L"Named configurations, presets and compatibility validation",
               profiles_.empty() ? L"NO PROFILES" : L"LOCAL",
               !profiles_.empty());

    const float action_w = 150.0f;
    if (widgets.Button(3101, Rect(x + w - action_w * 2 - 10, 76,
                                  x + w - action_w - 10, 106),
                       L"New from current", false)) {
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
    if (widgets.Button(3102, Rect(x + w - action_w, 76, x + w, 106), L"Import...",
                       false)) {
        std::wstring path;
        if (PickFile(window_->hwnd(), false, path,
                     L"Profile JSON\0*.json\0All files\0*.*\0")) {
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

    widgets.Section(Rect(x, 136, x + w, 152), L"QUICK PRESETS");
    struct Preset {
        const wchar_t* name;
        const wchar_t* detail;
        AppSettings settings;
    };
    auto base = settings_;
    std::vector<Preset> presets;
    {
        AppSettings p = base;
        p.video.output = Resolution::Native;
        p.video.render_target = 0;
        p.ai.super_resolution = false;
        p.ai.frame_generation = false;
        presets.push_back({L"Native", L"No AI processing; lowest input latency", p});
    }
    {
        AppSettings p = base;
        p.video.output = Resolution::R1440;
        p.video.render_target = 1440;
        p.video.vsync = true;
        p.ai.super_resolution = true;
        p.ai.sr_output = Resolution::R1440;
        p.ai.sr_quality = 3;
        p.ai.frame_generation = false;
        presets.push_back({L"1440p Sharp", L"DLSS SR to 1440p, no generator", p});
    }
    {
        AppSettings p = base;
        p.video.output = Resolution::R1440;
        p.video.render_target = 1440;
        p.ai.super_resolution = false;
        p.ai.frame_generation = true;
        p.ai.fg_backend = AiBackend::Rife;
        p.ai.rife_model = 0;
        presets.push_back({L"1440p RIFE", L"RIFE 30 -> 60 at 1440p", p});
    }
    {
        AppSettings p = base;
        p.video.output = Resolution::Native;
        p.video.render_target = 0;
        p.ai.super_resolution = false;
        p.ai.frame_generation = true;
        p.ai.fg_backend = AiBackend::DlssFg;
        p.ai.fg_multiplier = 2;
        presets.push_back({L"DLSS-G 2x", L"NVIDIA DLSS-G 2x, no SR readback", p});
    }

    const float preset_gap = 12.0f;
    const float preset_w = (w - preset_gap * 3.0f) / 4.0f;
    for (int i = 0; i < static_cast<int>(presets.size()); ++i) {
        const float cx = x + (preset_w + preset_gap) * i;
        const D2D1_RECT_F card = Rect(cx, 160, cx + preset_w, 258);
        widgets.Card(card);
        renderer_->FillCircle(cx + 16, 178, 4.0f, ToD2D(i == 3 ? theme.accent
                                                                : theme.text_faint));
        renderer_->Text(presets[i].name, Rect(cx + 28, 166, cx + preset_w - 14, 190),
                        12.5f, ToD2D(theme.text), DWRITE_FONT_WEIGHT_SEMI_BOLD);
        renderer_->TextEllipsis(presets[i].detail,
                                Rect(cx + 14, 194, cx + preset_w - 14, 214), 9.5f,
                                ToD2D(theme.text_faint));
        if (widgets.Button(3110 + i, Rect(cx + 14, 220, cx + preset_w - 14, 252),
                           L"Apply preset", i == 3)) {
            settings_ = presets[i].settings;
            if (Apply()) {
                Toast(std::wstring(L"Preset applied: ") + presets[i].name);
            }
        }
    }

    widgets.Section(Rect(x, 278, x + w, 294), L"SAVED PROFILES");
    if (profiles_.empty()) {
        widgets.Card(Rect(x, 304, x + w, 392));
        renderer_->Text(L"No profiles yet.", Rect(x + 18, 320, x + w - 18, 344),
                        14.0f, ToD2D(theme.text), DWRITE_FONT_WEIGHT_SEMI_BOLD);
        renderer_->Text(L"Create one from the current configuration, or import a "
                        L"profile JSON.",
                        Rect(x + 18, 346, x + w - 18, 368), 10.5f,
                        ToD2D(theme.text_faint));
        return;
    }

    float y = 304.0f;
    int index = 0;
    for (const auto& profile : profiles_) {
        if (index >= 6) {
            break;
        }
        widgets.Card(Rect(x, y, x + w, y + 70));
        renderer_->Text(Widen(profile.name).c_str(),
                        Rect(x + 18, y + 10, x + w - 470, y + 32), 13.5f,
                        ToD2D(theme.text), DWRITE_FONT_WEIGHT_SEMI_BOLD);
        const std::wstring warning = ProfileWarning(profile);
        const std::wstring summary =
            Summary(profile) +
            (warning.empty() ? std::wstring() : L"  |  needs " + warning);
        renderer_->TextEllipsis(summary.c_str(),
                                Rect(x + 18, y + 36, x + w - 470, y + 58), 10.0f,
                                ToD2D(warning.empty() ? theme.text_dim : theme.warning));
        if (profile.is_default) {
            widgets.StatusPill(Rect(x + w - 458, y + 20, x + w - 384, y + 46),
                               L"DEFAULT", theme.accent, true);
        }
        const float bx = x + w - 372.0f;
        if (widgets.Button(3200 + index * 10, Rect(bx, y + 18, bx + 70, y + 52),
                           L"Apply", true)) {
            settings_ = profile.settings;
            if (Apply()) {
                Toast(warning.empty() ? std::wstring(L"Profile applied")
                                      : L"Applied; missing: " + warning,
                      !warning.empty());
            }
        }
        if (widgets.Button(3201 + index * 10, Rect(bx + 76, y + 18, bx + 146, y + 52),
                           L"Default", false)) {
            const Status status = host_->Profiles()->SetDefault(profile.id);
            if (status.ok()) {
                RefreshProfiles();
                Toast(L"Default profile set");
            } else {
                Toast(Widen(status.message()), true);
            }
        }
        if (widgets.Button(3202 + index * 10, Rect(bx + 152, y + 18, bx + 222, y + 52),
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
        if (widgets.Button(3203 + index * 10, Rect(bx + 228, y + 18, bx + 298, y + 52),
                           L"Delete", false)) {
            const Status status = host_->Profiles()->Delete(profile.id);
            if (status.ok()) {
                RefreshProfiles();
                Toast(L"Profile deleted");
            } else {
                Toast(Widen(status.message()), true);
            }
        }
        y += 78.0f;
        ++index;
    }
}
void NextApp::RenderDiagnostics(Widgets& widgets) {
    const ThemeTokens& theme = widgets.Tokens();
    const float x = ContentX();
    const float w = ContentWidth();
    PageHeader(widgets, L"WORKSPACE", L"Diagnostics", L"Live counters, system facts and report output");

    const float gap = 14.0f;
    const float card_w = (w - gap) * 0.5f;
    const float card_h = 136.0f;
    auto card = [&](int column, int row, const wchar_t* caption) {
        const float cx = x + (card_w + gap) * column;
        const float cy = 138.0f + (card_h + gap) * row;
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

    float y = 138.0f + (card_h + gap) * 2.0f;
    widgets.Card(Rect(x, y, x + w, y + 164));
    widgets.Section(Rect(x + 16, y + 12, x + w - 16, y + 28), L"UI LOOP TIMING");
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
    const StreamSnapshot stream =
        streaming_ ? streaming_->Snapshot() : StreamSnapshot{};
    wchar_t stream_line[220] = {};
    if (stream.connected) {
        std::swprintf(stream_line, 220,
                      L"Stream: %dx%d @ %.1f fps  |  decode %.2f ms  |  cadence %.1f / %.1f ms  |  %.0f kbps",
                      stream.source_width, stream.source_height, stream.source_fps,
                      stream.decode_ms, cadence_p50_, cadence_p99_,
                      stream.bitrate_kbps);
    } else {
        std::swprintf(stream_line, 220,
                      L"No active session. UI loop and diagnostics are still live.");
    }
    renderer_->Text(stream_line, Rect(x + 16, y + 104, x + w - 16, y + 126), 10.5f,
                    ToD2D(stream.connected ? theme.text_dim : theme.text_faint));
    renderer_->Text((L"UI frames: " + std::to_wstring(ui_frames_) + L"  |  " +
                     std::to_wstring(static_cast<int>(uptime)) + L" s uptime")
                        .c_str(),
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
    if (widgets.Button(3303, Rect(x + 400, y, x + 590, y + 38), L"Session report",
                       false)) {
        WriteSessionReport();
    }
    if (widgets.Button(3304, Rect(x + 600, y, x + 790, y + 38), L"Run benchmark",
                       false)) {
        StartBenchmark();
    }
}

void NextApp::RenderSettings(Widgets& widgets) {
    const float x = ContentX();
    const float w = ContentWidth();
    PageHeader(widgets, L"APPLICATION", L"Settings", L"Central configuration, validated and saved on every change");

    static const wchar_t* categories[] = {
        L"General", L"Streaming", L"Video", L"AI Video Engine", L"GPU", L"Display",
        L"Overlay", L"Audio", L"Controller", L"Advanced"};
    const float list_w = 238.0f;
    float y = 138.0f;
    for (int i = 0; i < static_cast<int>(std::size(categories)); ++i) {
        const D2D1_RECT_F row = Rect(x, y, x + list_w, y + 38);
        if (widgets.NavItem(9400 + i, row, categories[i], settings_category_ == i, 14)) {
            settings_category_ = i;
        }
        y += 40.0f;
    }

    const float px = x + list_w + 24.0f;
    const float pw = x + w - px;
    widgets.Card(Rect(px, 138, px + pw, logical_height_ - 40));
    SettingsControls controls(widgets, settings_, system_, true);
    controls.Reset();
    float cy = 156.0f;
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
    (void)cy;
}

void NextApp::RenderAboutPage(Widgets& widgets) {
    const ThemeTokens& theme = widgets.Tokens();
    const float x = ContentX();
    const float w = ContentWidth();
    PageHeader(widgets, L"APPLICATION", L"About",
               L"BetterRemotePlay Next 1.0.0  |  stable release", L"STABLE", true);

    widgets.Card(Rect(x, 138, x + w, 238));
    renderer_->FillRounded(Rect(x + 18, 154, x + 66, 202), 12.0f,
                           ToD2D(theme.accent));
    renderer_->Text(L"B", Rect(x + 18, 154, x + 66, 202), 24.0f, ToD2D(theme.text),
                    DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
    renderer_->Text(L"BetterRemotePlay Next", Rect(x + 80, 152, x + w - 20, 182),
                    20.0f, ToD2D(theme.text), DWRITE_FONT_WEIGHT_SEMI_BOLD);
    renderer_->Text(L"Version 1.0.0  |  win64  |  C++20 / Win32 / Direct2D / D3D11",
                    Rect(x + 80, 182, x + w - 20, 204), 11.5f,
                    ToD2D(theme.text_dim));
    renderer_->Text(L"PS5 Remote Play with native DualSense input, DLSS Super Resolution, "
                    L"DLSS-G through the NVIDIA worker, and TensorRT RIFE.",
                    Rect(x + 80, 204, x + w - 20, 226), 11.5f,
                    ToD2D(theme.text_faint));

    const float gap = 14.0f;
    const float col = (w - gap) * 0.5f;
    widgets.Card(Rect(x, 252, x + col, 484));
    widgets.Section(Rect(x + 18, 266, x + col - 18, 282), L"RUNTIMES");
    float ly = 294.0f;
    auto runtime = [&](const wchar_t* name, bool present, const wchar_t* detail) {
        renderer_->FillCircle(x + 26, ly + 8, 3.2f,
                              ToD2D(present ? theme.success : theme.warning));
        renderer_->Text(name, Rect(x + 38, ly, x + col - 16, ly + 18), 12.0f,
                        ToD2D(theme.text), DWRITE_FONT_WEIGHT_SEMI_BOLD);
        renderer_->Text(present ? L"available" : L"missing",
                        Rect(x + col - 90, ly, x + col - 16, ly + 18), 10.0f,
                        ToD2D(present ? theme.success : theme.warning),
                        DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_TRAILING);
        renderer_->Text(detail, Rect(x + 38, ly + 17, x + col - 16, ly + 34), 9.5f,
                        ToD2D(theme.text_faint));
        ly += 34.0f;
    };
    runtime(L"DLSS Super Resolution", system_.dlss_sr, L"nvngx_dlss.dll");
    runtime(L"DLSS Frame Generation", system_.dlss_fg,
            system_.dlss_fg_multi ? L"NVIDIA runtime + SM86 proxy (up to 6x)"
                                  : L"NVIDIA runtime (2x)");
    runtime(L"RIFE Lite 4.25", system_.rife_lite, L"TensorRT 1080p engine");
    runtime(L"RIFE Heavy 4.26", system_.rife_heavy, L"TensorRT 1080p engine");
    runtime(L"TensorRT", system_.tensorrt_runtime, L"nvinfer_10 runtime");

    const float rx = x + col + gap;
    widgets.Card(Rect(rx, 252, rx + col, 484));
    widgets.Section(Rect(rx + 18, 266, rx + col - 18, 282), L"SYSTEM");
    float ry = 294.0f;
    auto row = [&](const wchar_t* name, const std::wstring& value) {
        renderer_->Text(name, Rect(rx + 18, ry, rx + 110, ry + 18), 10.0f,
                        ToD2D(theme.text_faint), DWRITE_FONT_WEIGHT_SEMI_BOLD);
        renderer_->TextEllipsis(value.c_str(), Rect(rx + 120, ry, rx + col - 18, ry + 18),
                                11.0f, ToD2D(theme.text));
        ry += 28.0f;
    };
    row(L"OS", Widen(system_.os));
    row(L"CPU", Widen(system_.cpu));
    row(L"GPU", system_.gpu.present ? Widen(system_.gpu.name) : L"not detected");
    row(L"VRAM", std::to_wstring(system_.gpu.vram_mb / 1024) + L" GB");
    row(L"RAM", std::to_wstring(system_.ram_mb / 1024) + L" GB");
    row(L"Display", system_.displays.empty()
                        ? L"not reported"
                        : std::to_wstring(system_.displays[0].width) + L"x" +
                              std::to_wstring(system_.displays[0].height) + L" @ " +
                              std::to_wstring(system_.displays[0].refresh_hz) + L" Hz");

    widgets.Card(Rect(x, 498, x + w, 690));
    widgets.Section(Rect(x + 18, 512, x + w - 18, 528), L"DATA & DIAGNOSTICS");
    float py = 538.0f;
    auto path = [&](const wchar_t* name, const std::filesystem::path& value) {
        renderer_->Text(name, Rect(x + 18, py, x + 130, py + 18), 10.0f,
                        ToD2D(theme.text_faint), DWRITE_FONT_WEIGHT_SEMI_BOLD);
        renderer_->TextEllipsis(value.wstring().c_str(),
                                Rect(x + 138, py, x + w - 18, py + 18), 10.5f,
                                ToD2D(theme.text_dim));
        py += 24.0f;
    };
    path(L"Settings", paths_.settings_file);
    path(L"Profiles", paths_.profiles_dir);
    path(L"Log", paths_.log_file);
    path(L"Executable", std::filesystem::path(system_.exe_dir));

    if (widgets.Button(9601, Rect(x + 18, py + 10, x + 206, py + 48),
                       L"Open data folder")) {
        const std::wstring folder = paths_.settings_file.parent_path().wstring();
        ::ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    if (widgets.Button(9602, Rect(x + 216, py + 10, x + 404, py + 48),
                       L"Export diagnostics", false)) {
        const auto report = paths_.settings_file.parent_path() / "diagnostics.txt";
        const Status status = host_->Diagnostics()->WriteReport(report.string());
        Toast(status.ok() ? std::wstring(L"Diagnostics: ") + PathW(report)
                          : Widen(status.message()),
              !status.ok());
    }
    if (widgets.Button(9603, Rect(x + 414, py + 10, x + 602, py + 48),
                       L"Session report", false)) {
        WriteSessionReport();
    }

    renderer_->Text(L"Components: chiaki-ng session core, FFmpeg, NVIDIA NGX/DLSS, "
                    L"TensorRT, RIFE, NeuralScreen worker. No RTX VSR. "
                    L"Noto Sans is SIL OFL.",
                    Rect(x, 706, x + w, 726), 10.0f, ToD2D(theme.text_faint));
}
bool NextApp::Apply() {
    const AppSettings before = host_->Settings();
    const ApplyScope scope = ClassifySettingsChange(before, settings_);
    const AiSettings previous_ai = settings_.ai;
    const Status status = host_->ApplySettings(settings_);
    if (!status.ok()) {
        Toast(Widen(status.message()), true);
        settings_ = host_->Settings();
        overlay_status_ = L"Change rejected";
        overlay_status_error_ = true;
        return false;
    }
    settings_ = host_->Settings();
    last_apply_scope_ = scope;
    haptic_strength_.store(settings_.controller.rumble_strength);
    ApplyPresenterTarget();
    {
        EnhanceParams enhance;
        enhance.active = settings_.video.sharpening > 0;
        enhance.sharpen =
            std::clamp(settings_.video.sharpening / 100.0f, 0.0f, 1.0f);
        presenter_->SetEnhance(enhance);
    }
    if (auto* logger = dynamic_cast<Logger*>(host_->Log())) {
        logger->SetLevel(settings_.advanced.log_level);
    }
    if (previous_ai.super_resolution != settings_.ai.super_resolution ||
        previous_ai.sr_output != settings_.ai.sr_output ||
        previous_ai.sr_quality != settings_.ai.sr_quality ||
        previous_ai.sr_model != settings_.ai.sr_model) {
        dlss_ready_ = false;
        dlss_failed_ = false;
        if (dlss_sr_) {
            dlss_sr_->Shutdown();
        }
    }
    if (previous_ai.frame_generation != settings_.ai.frame_generation ||
        previous_ai.fg_backend != settings_.ai.fg_backend ||
        previous_ai.fg_multiplier != settings_.ai.fg_multiplier ||
        previous_ai.rife_model != settings_.ai.rife_model ||
        previous_ai.blend != settings_.ai.blend ||
        previous_ai.motion_safety != settings_.ai.motion_safety) {
        StopDlssFg();
        StopRifeThread();
        rife_failed_ = false;
        dlss_fg_failed_ = false;
    }
    if (scope == ApplyScope::StreamRestart && streaming_ && streaming_->Connected()) {
        stream_restart_pending_ = true;
    }
    overlay_status_ = ApplyScopeLabel(scope);
    overlay_status_error_ = false;
    last_overlay_update_ = -1.0;
    Toast(overlay_status_, scope == ApplyScope::ApplicationRestart);
    return true;
}

void NextApp::ApplySelectedProfile() {
    if (selected_profile_ < 0 ||
        selected_profile_ >= static_cast<int>(profiles_.size())) {
        overlay_status_ = L"No profile selected";
        overlay_status_error_ = true;
        return;
    }
    settings_ = profiles_[static_cast<std::size_t>(selected_profile_)].settings;
    if (Apply()) {
        overlay_status_ = L"Profile applied: " +
                          Widen(profiles_[static_cast<std::size_t>(selected_profile_)].name);
        overlay_status_error_ = false;
        last_overlay_update_ = -1.0;
    }
}

void NextApp::SetOverlayControl(bool open) {
    overlay_control_ = open;
    if (open) {
        overlay_.SetTab(0);
        overlay_status_ = L"Arrows / mouse change tabs and values";
        overlay_status_error_ = false;
    } else {
        overlay_.Hide();
    }
    last_overlay_update_ = -1.0;
}

void NextApp::BuildOverlayView() {
    const ThemeTokens& theme = ThemeFor(settings_.general.theme);
    OverlayView view;
    view.expanded = overlay_control_;
    view.live = streaming_ && streaming_->Connected();
    view.palette.background = RgbOf(theme.background);
    view.palette.panel = RgbOf(theme.surface);
    view.palette.panel_alt = RgbOf(theme.surface_alt);
    view.palette.border = RgbOf(theme.border);
    view.palette.text = RgbOf(theme.text);
    view.palette.text_dim = RgbOf(theme.text_dim);
    view.palette.text_faint = RgbOf(theme.text_faint);
    view.palette.accent = RgbOf(theme.accent);
    view.palette.accent_warm = RgbOf(theme.accent_warm);
    view.palette.success = RgbOf(theme.success);
    view.palette.warning = RgbOf(theme.warning);
    view.palette.error = RgbOf(theme.error);

    const StreamSnapshot snapshot =
        streaming_ ? streaming_->Snapshot() : StreamSnapshot{};
    const DiagnosticsSnapshot diagnostics = host_->Diagnostics()->Snapshot();
    const int output_fps = OutputFps(settings_);
    int out_w = snapshot.source_width;
    int out_h = snapshot.source_height;
    if (dlss_ready_ && dlss_sr_) {
        out_w = dlss_sr_->out_width();
        out_h = dlss_sr_->out_height();
    } else if (settings_.video.render_target == 1080) {
        out_w = 1920;
        out_h = 1080;
    } else if (settings_.video.render_target == 1440) {
        out_w = 2560;
        out_h = 1440;
    } else if (settings_.video.render_target == 2160) {
        out_w = 3840;
        out_h = 2160;
    }

    const std::wstring profile_name =
        selected_profile_ >= 0 &&
                selected_profile_ < static_cast<int>(profiles_.size())
            ? Widen(profiles_[static_cast<std::size_t>(selected_profile_)].name)
            : L"Unsaved configuration";
    wchar_t subtitle[220] = {};
    std::swprintf(subtitle, 220,
                  L"%dx%d @ %.1f fps  ->  %dx%d @ %d fps requested  |  %s",
                  snapshot.source_width, snapshot.source_height, snapshot.source_fps,
                  out_w, out_h, output_fps, profile_name.c_str());
    view.subtitle = subtitle;
    view.title = L"Control center";
    view.status = overlay_status_.empty()
                      ? L"F10 / Esc close  |  arrows select  |  Enter apply"
                      : overlay_status_;
    view.status_error = overlay_status_error_;

    const bool sr_on = settings_.ai.super_resolution;
    const bool fg_on = settings_.ai.frame_generation;
    const bool dlss_fg = fg_on && settings_.ai.fg_backend == AiBackend::DlssFg;
    const bool rife_fg = fg_on && settings_.ai.fg_backend == AiBackend::Rife;
    const bool dlaa = sr_on && (settings_.ai.sr_quality >= 5 ||
                                settings_.ai.sr_output == Resolution::R1080);

    auto info = [](const wchar_t* label, std::wstring value,
                   std::wstring detail = {}) {
        OverlayRow row;
        row.label = label;
        row.value = std::move(value);
        row.detail = std::move(detail);
        row.enabled = false;
        return row;
    };
    auto toggle = [](const wchar_t* label, bool value, OverlayAction action,
                     std::wstring detail = {}, bool enabled = true) {
        OverlayRow row;
        row.label = label;
        row.value = value ? L"On" : L"Off";
        row.detail = std::move(detail);
        row.enabled = enabled;
        row.toggle = true;
        row.toggle_value = value;
        row.action = action;
        return row;
    };
    auto stepper = [](const wchar_t* label, std::wstring value, OverlayAction previous,
                      OverlayAction next, std::wstring detail = {},
                      bool enabled = true) {
        OverlayRow row;
        row.label = label;
        row.value = std::move(value);
        row.detail = std::move(detail);
        row.enabled = enabled;
        row.stepper = true;
        row.previous = previous;
        row.next = next;
        return row;
    };
    auto action_row = [](const wchar_t* label, std::wstring value, OverlayAction action,
                         std::wstring detail = {}, bool enabled = true) {
        OverlayRow row;
        row.label = label;
        row.value = std::move(value);
        row.detail = std::move(detail);
        row.enabled = enabled;
        row.action = action;
        return row;
    };

    wchar_t buffer[220] = {};

    OverlayPage overview;
    overview.title = L"Overview";
    std::swprintf(buffer, 220, L"%dx%d @ %.1f fps", snapshot.source_width,
                  snapshot.source_height, snapshot.source_fps);
    overview.rows.push_back(info(L"INPUT", buffer, L"decoded source"));
    std::swprintf(buffer, 220, L"%dx%d @ %d fps", out_w, out_h, output_fps);
    overview.rows.push_back(info(L"OUTPUT", buffer,
                                 settings_.video.render_target == 0
                                     ? L"native window / fullscreen"
                                     : L"fixed render target"));
    if (settings_.overlay.show_performance) {
        std::swprintf(buffer, 220, L"%.2f ms", snapshot.decode_ms);
        overview.rows.push_back(info(L"DECODE", buffer, L"network to RGBA"));
        std::swprintf(buffer, 220, L"%.1f / %.1f ms", cadence_p50_, cadence_p99_);
        overview.rows.push_back(info(L"CADENCE", buffer, L"p50 / p99 source interval"));
    }
    if (settings_.overlay.show_processing) {
        overview.rows.push_back(info(
            L"SUPER RESOLUTION",
            sr_on ? (dlaa ? L"DLAA" : Widen(std::string(ToString(settings_.ai.sr_output))))
                  : L"Off",
            sr_on ? (dlaa ? L"native AA at the stream size" : L"NGX upscale")
                  : L"presenter upscale only"));
        overview.rows.push_back(info(
            L"FRAME GENERATION",
            fg_on ? (dlss_fg ? std::wstring(L"DLSS-G ") +
                                   std::to_wstring(settings_.ai.fg_multiplier) + L"x"
                             : std::wstring(L"RIFE"))
                  : L"Off",
            dlss_fg ? std::to_wstring(static_cast<int>(fg_fps_)) + L" shown fps"
                    : (rife_fg ? std::to_wstring(rife_generated_rate_) + L" generated/s"
                               : L"source cadence only")));
    }
    if (settings_.overlay.show_stream) {
        std::swprintf(buffer, 220, L"%.0f kbps  |  loss %.2f%%", snapshot.bitrate_kbps,
                      snapshot.loss_percent);
        overview.rows.push_back(info(L"STREAM", buffer, Widen(snapshot.console_name)));
    }
    if (settings_.overlay.show_gpu) {
        overview.rows.push_back(info(L"GPU", Widen(system_.gpu.name),
                                     std::to_wstring(system_.gpu.vram_mb / 1024) +
                                         L" GB VRAM"));
    }
    view.pages.push_back(std::move(overview));

    OverlayPage sr;
    sr.title = L"Super resolution";
    sr.rows.push_back(toggle(L"Super resolution", sr_on, OverlayAction::ToggleSr,
                             L"DLSS Super Resolution (NGX)", system_.dlss_sr));
    sr.rows.push_back(stepper(L"Quality", QualityLabel(settings_.ai.sr_quality),
                              OverlayAction::SrQualityPrev,
                              OverlayAction::SrQualityNext,
                              L"DLAA stays at the stream size", sr_on));
    sr.rows.push_back(stepper(
        L"Output", settings_.ai.sr_quality >= 5
                       ? std::wstring(L"Native (DLAA)")
                       : Widen(std::string(ToString(settings_.ai.sr_output))),
        OverlayAction::SrOutputPrev, OverlayAction::SrOutputNext,
        L"1080p / 1440p / 2160p; 2160p cannot feed DLSS-G",
        sr_on && settings_.ai.sr_quality < 5));
    sr.rows.push_back(info(L"BACKEND", L"DLSS Super Resolution",
                           system_.dlss_sr ? L"nvngx_dlss.dll" : L"runtime missing"));
    sr.rows.push_back(info(L"ENGINE", dlss_ready_ ? L"Active" : L"Idle",
                           dlss_ready_ ? L"feature created" : L"starts on next frame"));
    sr.rows.push_back(info(L"EFFECT",
                           std::wstring(ResolutionLabel(settings_.ai.sr_output)),
                           dlaa ? L"native AA; lowest-latency DLSS-G path"
                                : L"upscale before frame generation"));
    view.pages.push_back(std::move(sr));

    OverlayPage fg;
    fg.title = L"Frame generation";
    fg.rows.push_back(toggle(L"Frame generation", fg_on, OverlayAction::ToggleFg,
                             L"generator owns presentation", system_.dlss_fg ||
                                 system_.rife_lite));
    fg.rows.push_back(action_row(
        L"Backend", dlss_fg ? L"DLSS-G" : (rife_fg ? L"RIFE" : L"Off"),
        OverlayAction::FgBackendNext,
        dlss_fg ? L"NVIDIA worker presenter"
                : (rife_fg ? L"TensorRT, one midpoint" : L"select a generator"),
        fg_on));
    fg.rows.push_back(stepper(
        L"Multiplier",
        std::to_wstring(std::clamp(settings_.ai.fg_multiplier, 2, 6)) + L"x",
        OverlayAction::FgMultPrev, OverlayAction::FgMultNext,
        L"2x is the clean cadence; 3x-6x need the SM86 proxy", dlss_fg));
    fg.rows.push_back(info(L"OUTPUT", std::to_wstring(output_fps) + L" fps",
                           L"source rate x multiplier"));
    fg.rows.push_back(info(
        L"RUNTIME",
        dlss_fg ? (settings_.ai.fg_multiplier > 2 ? L"SM86 proxy" : L"NVIDIA official")
                : (rife_fg ? L"RIFE 4.25" : L"none"),
        dlss_fg ? (settings_.ai.fg_multiplier > 2 ? L"up to 6x, modded kernels"
                                                  : L"2x, NVIDIA-signed runtime")
                : L""));
    fg.rows.push_back(info(L"DISPLAYED",
                           std::to_wstring(static_cast<int>(
                               dlss_fg ? fg_fps_ : static_cast<double>(display_rate_))) +
                               L" fps",
                           L"measured, not requested"));
    fg.rows.push_back(info(L"NOTE", dlss_fg ? L"flat depth + estimated motion" : L"",
                           L"video has no engine motion vectors"));
    view.pages.push_back(std::move(fg));

    OverlayPage display;
    display.title = L"Display";
    display.rows.push_back(toggle(L"Borderless fullscreen",
                                  window_ && window_->fullscreen(),
                                  OverlayAction::ToggleFullscreen,
                                  L"F11 also toggles it"));
    display.rows.push_back(toggle(L"Overlay statistics", settings_.overlay.enabled,
                                  OverlayAction::ToggleStats,
                                  L"compact mode stays click-through"));
    display.rows.push_back(stepper(
        L"Opacity",
        std::to_wstring(static_cast<int>(settings_.overlay.opacity * 100.0f)) + L"%",
        OverlayAction::OpacityDown, OverlayAction::OpacityUp, L"5% steps"));
    display.rows.push_back(stepper(
        L"Scale", std::to_wstring(settings_.overlay.scale) + L"%",
        OverlayAction::ScaleDown, OverlayAction::ScaleUp, L"75-200%"));
    display.rows.push_back(action_row(L"Position",
                                      OverlayPositionLabel(settings_.overlay.position),
                                      OverlayAction::PositionNext, L"cycles corners"));
    display.rows.push_back(action_row(
        L"Theme", settings_.general.theme == Theme::Dark ? L"Dark" : L"Light",
        OverlayAction::ToggleTheme, L"applies to the whole application"));
    std::swprintf(buffer, 220, L"%d Hz",
                  system_.displays.empty() ? 0 : system_.displays[0].refresh_hz);
    display.rows.push_back(info(L"REFRESH", buffer, L"current monitor"));
    view.pages.push_back(std::move(display));

    OverlayPage performance;
    performance.title = L"Performance";
    std::swprintf(buffer, 220, L"%.1f fps", snapshot.source_fps);
    performance.rows.push_back(info(L"SOURCE", buffer));
    performance.rows.push_back(info(
        L"DISPLAYED",
        std::to_wstring(static_cast<int>(dlss_fg ? fg_fps_
                                                : static_cast<double>(display_rate_))) +
            L" fps"));
    std::swprintf(buffer, 220, L"%.2f ms", snapshot.decode_ms);
    performance.rows.push_back(info(L"DECODE", buffer));
    std::swprintf(buffer, 220, L"%.1f / %.1f ms", cadence_p50_, cadence_p99_);
    performance.rows.push_back(info(L"CADENCE", buffer, L"p50 / p99"));
    performance.rows.push_back(info(L"GENERATED",
                                    dlss_fg ? std::to_wstring(fg_dropped_) + L" dropped"
                                            : std::to_wstring(rife_generated_rate_) +
                                                  L" /s",
                                    dlss_fg ? L"worker queue" : L"RIFE midpoints"));
    std::swprintf(buffer, 220, L"%.2f / %.2f ms", diagnostics.frame_p50_ms,
                  diagnostics.frame_p99_ms);
    performance.rows.push_back(info(L"UI FRAME", buffer, L"p50 / p99"));
    performance.rows.push_back(action_row(L"Statistics", L"Reset",
                                          OverlayAction::ResetFrameStats,
                                          L"source, generated and cadence counters"));
    view.pages.push_back(std::move(performance));

    OverlayPage profile;
    profile.title = L"Profile";
    profile.rows.push_back(stepper(L"Profile", profile_name, OverlayAction::ProfilePrev,
                                   OverlayAction::ProfileNext,
                                   L"select, then apply", !profiles_.empty()));
    profile.rows.push_back(action_row(L"Apply profile", L"Apply",
                                      OverlayAction::ProfileApply,
                                      L"settings and pipeline update live",
                                      !profiles_.empty()));
    profile.rows.push_back(action_row(L"Save configuration", L"New profile",
                                      OverlayAction::SaveProfile,
                                      L"captures the effective settings"));
    profile.rows.push_back(info(L"AVAILABLE",
                                std::to_wstring(profiles_.size()) + L" profile(s)"));
    profile.rows.push_back(info(L"STREAM CHANGE",
                                stream_restart_pending_ ? L"Reconnect required"
                                                        : L"None",
                                L"transport changes never apply mid-session"));
    profile.rows.push_back(info(L"DEFAULT",
                                settings_.console.name.empty()
                                    ? L"Not set"
                                    : Widen(settings_.console.name)));
    view.pages.push_back(std::move(profile));

    const float opacity = overlay_control_ ? 1.0f : settings_.overlay.opacity;
    overlay_.Update(view, opacity, settings_.overlay.position,
                    settings_.overlay.scale / 100.0f, window_->hwnd());
}
void NextApp::HandleOverlayAction(OverlayAction action) {
    const StreamSnapshot snapshot = streaming_ ? streaming_->Snapshot() : StreamSnapshot{};
    auto changed = [&]() {
        if (settings_.ai.sr_quality >= 5) {
            settings_.ai.sr_output = Resolution::Native;
        }
        Apply();
        last_overlay_update_ = -1.0;
    };

    switch (action) {
        case OverlayAction::Close:
            SetOverlayControl(false);
            return;
        case OverlayAction::ToggleControl:
            SetOverlayControl(!overlay_control_);
            return;
        case OverlayAction::ToggleSr:
            settings_.ai.super_resolution = !settings_.ai.super_resolution;
            changed();
            break;
        case OverlayAction::SrQualityPrev:
        case OverlayAction::SrQualityNext: {
            int q = std::clamp(settings_.ai.sr_quality, 0, 5);
            q += action == OverlayAction::SrQualityNext ? 1 : -1;
            if (q < 0) q = 5;
            if (q > 5) q = 0;
            settings_.ai.sr_quality = q;
            changed();
            break;
        }
        case OverlayAction::SrOutputPrev:
        case OverlayAction::SrOutputNext: {
            if (settings_.ai.sr_quality >= 5) {
                break;
            }
            int index = 1;
            if (settings_.ai.sr_output == Resolution::R1440) index = 2;
            if (settings_.ai.sr_output == Resolution::R2160) index = 3;
            index += action == OverlayAction::SrOutputNext ? 1 : -1;
            if (index < 1) index = 3;
            if (index > 3) index = 1;
            settings_.ai.sr_output = index == 1   ? Resolution::R1080
                                     : index == 2 ? Resolution::R1440
                                                  : Resolution::R2160;
            changed();
            break;
        }
        case OverlayAction::ToggleFg:
            settings_.ai.frame_generation = !settings_.ai.frame_generation;
            changed();
            break;
        case OverlayAction::FgBackendNext:
            settings_.ai.fg_backend = settings_.ai.fg_backend == AiBackend::Rife
                                          ? AiBackend::DlssFg
                                          : AiBackend::Rife;
            if (settings_.ai.fg_backend == AiBackend::Rife) {
                settings_.ai.fg_multiplier = 2;
            }
            changed();
            break;
        case OverlayAction::FgMultPrev:
        case OverlayAction::FgMultNext:
            if (settings_.ai.fg_backend == AiBackend::DlssFg) {
                int m = std::clamp(settings_.ai.fg_multiplier, 2, 6);
                m += action == OverlayAction::FgMultNext ? 1 : -1;
                settings_.ai.fg_multiplier = std::clamp(m, 2, 6);
                changed();
            }
            break;
        case OverlayAction::ToggleStats:
            settings_.overlay.enabled = !settings_.overlay.enabled;
            changed();
            break;
        case OverlayAction::OpacityDown:
            settings_.overlay.opacity =
                std::max(0.10f, settings_.overlay.opacity - 0.05f);
            changed();
            break;
        case OverlayAction::OpacityUp:
            settings_.overlay.opacity =
                std::min(1.0f, settings_.overlay.opacity + 0.05f);
            changed();
            break;
        case OverlayAction::ScaleDown:
            settings_.overlay.scale = std::max(75, settings_.overlay.scale - 5);
            changed();
            break;
        case OverlayAction::ScaleUp:
            settings_.overlay.scale = std::min(200, settings_.overlay.scale + 5);
            changed();
            break;
        case OverlayAction::PositionNext:
            settings_.overlay.position = (settings_.overlay.position + 1) & 3;
            changed();
            break;
        case OverlayAction::ToggleFullscreen:
            ToggleBorderlessFullscreen();
            overlay_status_ = window_ && window_->fullscreen() ? L"Borderless fullscreen"
                                                               : L"Windowed";
            overlay_status_error_ = false;
            last_overlay_update_ = -1.0;
            break;
        case OverlayAction::ToggleTheme:
            settings_.general.theme = settings_.general.theme == Theme::Dark
                                          ? Theme::Light
                                          : Theme::Dark;
            changed();
            break;
        case OverlayAction::ProfilePrev:
        case OverlayAction::ProfileNext: {
            if (profiles_.empty()) {
                break;
            }
            const int count = static_cast<int>(profiles_.size());
            selected_profile_ += action == OverlayAction::ProfileNext ? 1 : -1;
            if (selected_profile_ < 0) selected_profile_ = count - 1;
            if (selected_profile_ >= count) selected_profile_ = 0;
            overlay_status_ = L"Profile: " +
                              Widen(profiles_[static_cast<std::size_t>(selected_profile_)].name);
            overlay_status_error_ = false;
            last_overlay_update_ = -1.0;
            break;
        }
        case OverlayAction::ProfileApply:
            ApplySelectedProfile();
            break;
        case OverlayAction::SaveProfile: {
            Profile created;
            const Status status = host_->Profiles()->Create(
                "Profile " + std::to_string(profiles_.size() + 1), settings_, created);
            if (status.ok()) {
                RefreshProfiles();
                overlay_status_ = L"Profile saved";
                overlay_status_error_ = false;
            } else {
                overlay_status_ = Widen(status.message());
                overlay_status_error_ = true;
            }
            last_overlay_update_ = -1.0;
            break;
        }
        case OverlayAction::ResetFrameStats:
            display_shown_.store(0);
            display_shown_last_ = 0;
            display_rate_ = 0;
            rife_generated_.store(0);
            rife_generated_last_ = 0;
            rife_generated_rate_ = 0;
            video_intervals_.Reset();
            cadence_p50_ = 0.0;
            cadence_p99_ = 0.0;
            overlay_status_ = L"Frame statistics reset";
            overlay_status_error_ = false;
            last_overlay_update_ = -1.0;
            break;
        default:
            break;
    }
    (void)snapshot;
}
void NextApp::RefreshProfiles() {
    profiles_.clear();
    host_->Profiles()->List(profiles_);
    if (selected_profile_ >= static_cast<int>(profiles_.size())) {
        selected_profile_ = static_cast<int>(profiles_.size()) - 1;
    }
}

void NextApp::Toast(const std::wstring& text, bool error) {
    if (!settings_.general.notifications && !error) {
        return;
    }
    toast_text_ = text;
    toast_error_ = error;
    toast_until_ = now_ + 2.6;
}

void NextApp::ProbeIfNeeded(double now) {
    if (last_probe_ < 0.0 || now - last_probe_ > 30.0) {
        system_ = ProbeSystem();
        last_probe_ = now;
    }
}

void NextApp::HandlePinInput() {
    const int key = window_->ConsumeKey();
    const int character = window_->ConsumeChar();
    if (overlay_control_) {
        if (key == VK_F10) {
            SetOverlayControl(false);
            return;
        }
        if (key != 0) {
            overlay_.Key(key);
        }
        return;
    }
    if (key == VK_F10 && watching_) {
        SetOverlayControl(true);
        return;
    }
    if (key == VK_F11) {
        ToggleBorderlessFullscreen();
        return;
    }
    if (key == VK_ESCAPE) {
        if (pin_focus_) {
            pin_focus_ = false;
            return;
        }
        if (watching_) {
            // First Esc leaves Watch and arms the disconnect shortcut.
            ToggleWatch(false);
            esc_at_ = now_;
            Toast(L"Esc again to disconnect");
            return;
        }
        if (esc_at_ >= 0.0 && now_ - esc_at_ <= 2.5) {
            esc_at_ = -1.0;
            DisconnectStream();
            return;
        }
        esc_at_ = -1.0;
        return;
    }
    if (!pin_focus_) {
        return;
    }
    if (key == VK_BACK && !pin_.empty()) {
        pin_.pop_back();
    }
    if (character >= '0' && character <= '9' && pin_.size() < 8) {
        pin_.push_back(static_cast<char>(character));
    }
}
std::wstring NextApp::ClipboardText() const {
    std::wstring text;
    if (!window_ || !::OpenClipboard(window_->hwnd())) {
        return text;
    }
    if (HANDLE data = ::GetClipboardData(CF_UNICODETEXT)) {
        if (const auto* chars = static_cast<const wchar_t*>(::GlobalLock(data))) {
            text = chars;
            ::GlobalUnlock(data);
        }
    }
    ::CloseClipboard();
    return text;
}

std::wstring NextApp::W(const std::string& text) const { return Widen(text); }

std::wstring NextApp::ProfileWarning(const Profile& profile) const {
    const AiSettings& ai = profile.settings.ai;
    if (ai.super_resolution && !system_.dlss_sr) {
        return L"DLSS SR runtime missing";
    }
    if (ai.frame_generation) {
        if (ai.fg_backend == AiBackend::DlssFg) {
            if (!system_.dlss_fg) {
                return L"DLSS-G worker runtime missing";
            }
        }
        if (ai.fg_backend == AiBackend::Rife && !system_.rife_lite) {
            return L"RIFE engine missing";
        }
    }
    return {};
}
std::wstring NextApp::Summary(const Profile& profile) const {
    const auto& s = profile.settings;
    std::wstring summary =
        Widen(std::string(ToString(s.streaming.input))) + L" " +
        std::to_wstring(s.streaming.fps) + L" fps  ->  " +
        Widen(std::string(ToString(s.video.output))) + L" " +
        std::to_wstring(OutputFps(s)) + L" fps";
    if (s.ai.frame_generation) {
        summary += L"  |  " + BackendLabel(s.ai.fg_backend);
    }
    if (s.ai.super_resolution) {
        const bool dlaa = s.ai.sr_quality >= 5 ||
                          s.ai.sr_output == Resolution::R1080;
        summary += dlaa ? L"  |  DLAA"
                        : L"  |  SR " + Widen(std::string(ToString(s.ai.sr_output)));
    }
    return summary;
}

void NextApp::PresentVideo() {
    if (!streaming_) {
        return;
    }
    const double content =
        std::clamp(static_cast<double>(settings_.streaming.fps), 24.0, 240.0);
    const bool fg_mode = settings_.ai.frame_generation &&
                         settings_.ai.fg_backend == AiBackend::DlssFg &&
                         !dlss_fg_failed_;
    if (fg_mode) {
        const float background[4] = {0.01f, 0.008f, 0.016f, 1.0f};
        bool new_frame = false;
        // The generator owns its output cadence: feed every decoded frame as
        // soon as it exists instead of waiting for the 60 Hz source slot. The
        // worker queues/generates, and a gate here would add up to one source
        // interval of pure latency.
        media::DecodedFrame frame;
        if (streaming_->TakeNewestFrame(frame)) {
            video_frame_ = std::move(frame);
            have_frame_ = true;
            new_frame = true;
        }

        auto publish = [&](std::vector<std::uint8_t>&& rgba, int width, int height) {
            if (!EnsureDlssFg(width, height)) {
                return false;
            }
            // The worker creates its presenter on the first Process(); submit
            // before looking for the window or the first frame never arrives
            // and the window never becomes visible.
            dlss_fg_->Submit(std::move(rgba), width, height);
            dlss_fg_->FrameWindow(window_->hwnd(), now_);
            RecordVideoShown();
            return true;
        };

        if (new_frame && have_frame_ && !video_frame_.rgba.empty()) {
            int feed_w = video_frame_.width;
            int feed_h = video_frame_.height;
            std::vector<std::uint8_t> rgba;
            bool have_rgba = false;
            const bool sr_can_feed_fg =
                settings_.ai.sr_output != Resolution::R2160 ||
                settings_.ai.sr_quality >= 5;
            if (settings_.ai.super_resolution && sr_can_feed_fg) {
                if (EnsureDlssSr(feed_w, feed_h)) {
                    brp::media::NvDlssSr::Frame sr_frame;
                    std::string detail;
                    if (dlss_sr_->Process(video_frame_.rgba.data(), feed_w, feed_h,
                                          sr_frame, detail) &&
                        dlss_sr_->Readback(rgba, detail)) {
                        feed_w = dlss_sr_->out_width();
                        feed_h = dlss_sr_->out_height();
                        have_rgba = true;
                    } else {
                        dlss_failed_ = true;
                        Toast(L"DLSS SR readback: " + Widen(detail), true);
                    }
                }
            }
            bool fed = false;
            if (have_rgba) {
                fed = publish(std::move(rgba), feed_w, feed_h);
            }
            if (!fed) {
                // No SR, or the worker cannot take the SR size: the stream
                // frame itself is the generator input.
                publish(std::move(video_frame_.rgba), video_frame_.width,
                        video_frame_.height);
            }
        }

        if (!dlss_fg_ || !dlss_fg_->ready()) {
            if (new_frame) {
                RecordVideoShown();
            }
            if (have_frame_ && !video_frame_.rgba.empty()) {
                presenter_->SetSourceFrame(video_frame_.rgba.data(), video_frame_.width,
                                           video_frame_.height, video_frame_.pq);
                presenter_->SetSourceFinal(false);
                presenter_->Render(now_);
            } else {
                presenter_->Clear(background);
            }
            return;
        }

        dlss_fg_->FrameWindow(window_->hwnd(), now_);
        fg_covering_ = dlss_fg_->covering();
        fg_fps_ = dlss_fg_->displayed_fps();
        fg_actual_mult_ = dlss_fg_->actual_multiplier();
        fg_dropped_ = dlss_fg_->dropped();
        if (dlss_fg_->failed()) {
            const std::wstring note = dlss_fg_->note();
            StopDlssFg();
            dlss_fg_failed_ = true;
            fg_covering_ = false;
            Toast(note.empty() ? L"DLSS-G stopped" : note, true);
            if (have_frame_ && !video_frame_.rgba.empty()) {
                presenter_->SetSourceFrame(video_frame_.rgba.data(), video_frame_.width,
                                           video_frame_.height, video_frame_.pq);
                presenter_->SetSourceFinal(false);
                presenter_->Render(now_);
            } else {
                presenter_->Clear(background);
            }
            return;
        }
        if (!dlss_fg_->attached()) {
            // The worker is running but its presenter window is not up yet.
            if (have_frame_ && !video_frame_.rgba.empty()) {
                presenter_->SetSourceFrame(video_frame_.rgba.data(), video_frame_.width,
                                           video_frame_.height, video_frame_.pq);
                presenter_->SetSourceFinal(false);
                presenter_->Render(now_);
            } else {
                presenter_->Clear(background);
            }
            return;
        }
        // The worker's topmost window is the picture. Do not present the
        // parent swapchain while it covers the screen: that makes DWM compose
        // a second full-size surface every vblank and adds a frame of latency
        // for no visible gain.
        return;
    }
    const bool rife_mode =
        settings_.ai.frame_generation && settings_.ai.fg_backend == AiBackend::Rife &&
        content < 45.0 && EnsureRife();
    if (rife_mode) {
        rife_interval_ = 1.0 / std::clamp(content, 24.0, 60.0);
        bool new_frame = false;
        media::DecodedFrame frame;
        if (streaming_->TakeNewestFrame(frame)) {
            video_frame_ = std::move(frame);
            have_frame_ = true;
            new_frame = true;
        }
        if (new_frame) {
            ScheduleRifeFrame(video_frame_);
        }
        DisplayFrame shown;
        bool have_shown = false;
        {
            std::lock_guard<std::mutex> lock(display_mutex_);
            if (!display_queue_.empty() && display_queue_.front().due <= now_) {
                shown = std::move(display_queue_.front());
                display_queue_.pop_front();
                have_shown = true;
            }
        }
        if (have_shown) {
            rife_last_shown_ = std::move(shown.frame);
            rife_have_shown_ = true;
            RecordVideoShown();
        }
        if (!have_shown && rife_have_shown_) {
            return;
        }
        if (rife_have_shown_ && !rife_last_shown_.rgba.empty()) {
            presenter_->SetSourceFrame(rife_last_shown_.rgba.data(),
                                       rife_last_shown_.width, rife_last_shown_.height,
                                       rife_last_shown_.pq);
            presenter_->SetSourceFinal(false);
            presenter_->Render(now_);
        } else {
            const float background[4] = {0.015f, 0.017f, 0.022f, 1.0f};
            presenter_->Clear(background);
        }
        return;
    }

    bool new_frame = false;
    media::DecodedFrame frame;
    if (streaming_->TakeNewestFrame(frame)) {
        video_frame_ = std::move(frame);
        have_frame_ = true;
        new_frame = true;
    }
    if (!new_frame) {
        return;
    }
    if (have_frame_ && !video_frame_.rgba.empty()) {
        const int width = video_frame_.width;
        const int height = video_frame_.height;
        if (EnsureDlssSr(width, height)) {
            brp::media::NvDlssSr::Frame sr_frame;
            std::string detail;
            if (dlss_sr_->Process(video_frame_.rgba.data(), width, height, sr_frame,
                                  detail)) {
                presenter_->SetSourceView(sr_frame.srv.Get(), dlss_sr_->out_width(),
                                          dlss_sr_->out_height(), video_frame_.pq);
                presenter_->SetSourceFinal(false);
                presenter_->Render(now_);
                if (new_frame) {
                    RecordVideoShown();
                }
                return;
            }
            dlss_failed_ = true;
            Toast(L"DLSS SR: " + Widen(detail), true);
        }
        presenter_->SetSourceFrame(video_frame_.rgba.data(), width, height,
                                   video_frame_.pq);
        presenter_->SetSourceFinal(false);
        presenter_->Render(now_);
        if (new_frame) {
            RecordVideoShown();
        }
    } else {
        const float background[4] = {0.015f, 0.017f, 0.022f, 1.0f};
        presenter_->Clear(background);
    }
}

bool NextApp::EnsureRife() {
    if (rife_failed_) {
        return false;
    }
    if (rife_ready_) {
        StartRifeThread();
        return true;
    }
    if (rife_failed_ || !settings_.ai.frame_generation ||
        settings_.ai.fg_backend != AiBackend::Rife) {
        return false;
    }
    if (!system_.rife_lite) {
        rife_failed_ = true;
        Toast(L"RIFE engine not built; run the RIFE setup tool first", true);
        return false;
    }
    std::wstring runtime;
    if (const char* env = std::getenv("BRP_TENSORRT_DIR")) {
        runtime = Widen(env);
    }
    const std::filesystem::path exe_dir(system_.exe_dir);
    if (runtime.empty()) {
        const std::filesystem::path local = exe_dir / "rife" / "runtime";
        if (std::filesystem::exists(local / "nvinfer_10.dll")) {
            runtime = local.wstring();
        }
    }
    if (runtime.empty()) {
        std::ifstream marker(exe_dir / "rife" / "runtime.txt");
        std::string line;
        if (marker && std::getline(marker, line) && !line.empty()) {
            const std::filesystem::path candidate(line);
            if (std::filesystem::exists(candidate / "nvinfer_10.dll")) {
                runtime = candidate.wstring();
            }
        }
    }
    if (runtime.empty()) {
        const std::filesystem::path fallback =
            L"C:\\mpv-upscaler-video-player\\vs-plugins\\vsmlrt-cuda";
        if (std::filesystem::exists(fallback / "nvinfer_10.dll")) {
            runtime = fallback.wstring();
        }
    }
    if (runtime.empty()) {
        rife_failed_ = true;
        Toast(L"RIFE needs TensorRT and an engine next to the app", true);
        return false;
    }
    const bool heavy = settings_.ai.rife_model == 1 && system_.rife_heavy;
    const std::filesystem::path engine =
        exe_dir / "rife" / (heavy ? "rife426h_1080.engine" : "rife425_1080.engine");
    if (!std::filesystem::exists(engine)) {
        rife_failed_ = true;
        Toast(L"RIFE engine missing; build it before enabling RIFE", true);
        return false;
    }
    rife_ = std::make_unique<brp::media::Rife>();
    std::string detail;
    std::fprintf(stderr, "[next] RIFE runtime=%ls engine=%ls\n", runtime.c_str(),
                 engine.c_str());
    if (!rife_->Load(Narrow(runtime), engine.string(), 1920, 1080, detail)) {
        rife_failed_ = true;
        Toast(L"RIFE: " + Widen(detail), true);
        return false;
    }
    rife_ready_ = true;
    std::fprintf(stderr, "[next] RIFE ready: %s\n", detail.c_str());
    StartRifeThread();
    return true;
}

void NextApp::StartRifeThread() {
    if (rife_run_.load()) {
        return;
    }
    rife_run_ = true;
    rife_thread_ = std::thread([this]() { RifeWorkerMain(); });
}

void NextApp::StopRifeThread() {
    {
        std::lock_guard<std::mutex> lock(rife_mutex_);
        rife_run_ = false;
        rife_jobs_.clear();
    }
    rife_cv_.notify_all();
    if (rife_thread_.joinable()) {
        rife_thread_.join();
    }
    {
        std::lock_guard<std::mutex> lock(display_mutex_);
        display_queue_.clear();
    }
    rife_prev_valid_ = false;
    rife_prev_pushed_ = false;
    rife_have_shown_ = false;
}

void NextApp::RifeWorkerMain() {
    for (;;) {
        RifeJob job;
        {
            std::unique_lock<std::mutex> lock(rife_mutex_);
            rife_cv_.wait(lock, [this]() { return !rife_run_ || !rife_jobs_.empty(); });
            if (!rife_run_) {
                return;
            }
            job = std::move(rife_jobs_.front());
            rife_jobs_.pop_front();
        }
        if (job.motion_safety) {
            double diff = 0.0;
            int samples = 0;
            for (std::size_t i = 0; i + 3 < job.a.size() && i + 3 < job.b.size();
                 i += 256) {
                diff += std::abs(static_cast<int>(job.a[i]) - job.b[i]);
                diff += std::abs(static_cast<int>(job.a[i + 1]) - job.b[i + 1]);
                diff += std::abs(static_cast<int>(job.a[i + 2]) - job.b[i + 2]);
                samples += 3;
            }
            if (samples > 0 && diff / samples > 22.0) {
                // Very fast motion: skip the invented midpoint.
                continue;
            }
        }
        std::vector<uint8_t> mid;
        std::string detail;
        if (!rife_ || !rife_->Process(job.a.data(), job.b.data(), 0.5f, mid, detail)) {
            std::fprintf(stderr, "[next] RIFE process failed: %s\n", detail.c_str());
            rife_failed_ = true;
            rife_run_.store(false);
            return;
        }
        if (job.blend > 0.0f && mid.size() == job.a.size()) {
            const float keep = 1.0f - job.blend;
            for (std::size_t i = 0; i + 3 < mid.size(); i += 4) {
                mid[i] = static_cast<std::uint8_t>(mid[i] * keep + job.a[i] * job.blend);
                mid[i + 1] = static_cast<std::uint8_t>(mid[i + 1] * keep +
                                                       job.a[i + 1] * job.blend);
                mid[i + 2] = static_cast<std::uint8_t>(mid[i + 2] * keep +
                                                       job.a[i + 2] * job.blend);
                mid[i + 3] = 255;
            }
        }
        rife_generated_.fetch_add(1);
        media::DecodedFrame frame;
        frame.rgba = std::move(mid);
        frame.width = rife_->width();
        frame.height = rife_->height();
        frame.pq = job.pq;
        PushDisplayFrame(std::move(frame), job.mid_due);
    }
}

void NextApp::PushDisplayFrame(media::DecodedFrame frame, double due) {
    std::lock_guard<std::mutex> lock(display_mutex_);
    DisplayFrame entry;
    entry.frame = std::move(frame);
    entry.due = due;
    display_queue_.push_back(std::move(entry));
    std::sort(display_queue_.begin(), display_queue_.end(),
              [](const DisplayFrame& a, const DisplayFrame& b) { return a.due < b.due; });
    while (display_queue_.size() > 8) {
        display_queue_.pop_front();
    }
}

void NextApp::ScheduleRifeFrame(const media::DecodedFrame& frame) {
    if (frame.rgba.empty()) {
        return;
    }
    const double interval = rife_interval_;
    if (!rife_prev_valid_ || rife_prev_.size() != frame.rgba.size()) {
        rife_prev_ = frame.rgba;
        rife_prev_w_ = frame.width;
        rife_prev_h_ = frame.height;
        rife_prev_pq_ = frame.pq;
        rife_prev_valid_ = true;
        rife_prev_pushed_ = false;
        return;
    }
    if (!rife_prev_pushed_) {
        media::DecodedFrame previous;
        previous.rgba = rife_prev_;
        previous.width = rife_prev_w_;
        previous.height = rife_prev_h_;
        previous.pq = rife_prev_pq_;
        PushDisplayFrame(std::move(previous), now_);
        rife_prev_pushed_ = true;
    }
    RifeJob job;
    job.a = rife_prev_;
    job.b = frame.rgba;
    job.pq = frame.pq;
    job.mid_due = now_ + interval * 0.5;
    job.blend = std::clamp(settings_.ai.blend, 0, 40) / 100.0f;
    job.motion_safety = settings_.ai.motion_safety;
    {
        std::lock_guard<std::mutex> lock(rife_mutex_);
        if (rife_jobs_.size() < 4) {
            rife_jobs_.push_back(std::move(job));
        }
    }
    rife_cv_.notify_one();

    media::DecodedFrame current;
    current.rgba = frame.rgba;
    current.width = frame.width;
    current.height = frame.height;
    current.pq = frame.pq;
    PushDisplayFrame(std::move(current), now_ + interval);

    rife_prev_ = frame.rgba;
    rife_prev_w_ = frame.width;
    rife_prev_h_ = frame.height;
    rife_prev_pq_ = frame.pq;
    rife_prev_pushed_ = true;
}
void NextApp::ToggleWatch(bool watching) {
    if (watching) {
        if (!streaming_ || !streaming_->Connected()) {
            Toast(L"Connect to a console first", true);
            return;
        }
        media::DecodedFrame stale;
        for (int i = 0; i < 8 && streaming_->TakeFrame(stale, 0, false); ++i) {
            // Drop pre-watch frames so the first shown frame is current.
        }
        watching_ = true;
        have_frame_ = false;
        video_intervals_.Reset();
        last_video_shown_at_ = -1.0;
        StopRifeThread();
        StopDlssFg();
        dlss_fg_failed_ = false;
        if (settings_.display.fullscreen_on_play && !window_->fullscreen()) {
            window_->EnterFullscreen();
            watch_fullscreen_ = true;
        }
        window_->SetCursorHidden(true);
        presenter_->SetUpscaleMode(2);
        ApplyPresenterTarget();
        Resize();
    } else {
        watching_ = false;
        have_frame_ = false;
        SetOverlayControl(false);
        StopRifeThread();
        StopDlssFg();
        if (watch_fullscreen_) {
            window_->ExitFullscreen();
            watch_fullscreen_ = false;
        }
        window_->SetCursorHidden(false);
        Resize();
    }
}

void NextApp::StartPadThread() {
    if (pad_run_.load()) {
        return;
    }
    pad_.SetBackend(brp::Gamepad::Backend::Auto);
    pad_run_.store(true);
    pad_thread_ = std::thread([this]() {
        using Clock = std::chrono::steady_clock;
        Clock::time_point last_haptics = Clock::now();
        bool haptics_on = false;
        DWORD task_index = 0;
        HANDLE mmcss = ::AvSetMmThreadCharacteristicsW(L"Games", &task_index);
        ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        HANDLE timer = ::CreateWaitableTimerExW(
            nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (timer) {
            LARGE_INTEGER due{};
            due.QuadPart = -40000;  // 4 ms in 100 ns units
            ::SetWaitableTimer(timer, &due, 4, nullptr, nullptr, FALSE);
        }
        while (pad_run_.load()) {
            pad_.Poll();
            if (streaming_ && streaming_->Connected()) {
                streaming_->SendController(pad_.snapshot());
                brp::PadConsoleFeedback feedback = streaming_->Feedback();
                uint16_t left = 0;
                uint16_t right = 0;
                if (streaming_->TakeHaptics(left, right)) {
                    last_haptics = Clock::now();
                    haptics_on = true;
                    const int strength = haptic_strength_.load();
                    if (strength > 0) {
                        double scale = 1.0;
                        switch (strength) {
                            case 1: scale = 0.2; break;
                            case 2: scale = 0.5; break;
                            case 4: scale = 2.0; break;
                            case 5: scale = 5.0; break;
                            default: break;
                        }
                        const auto to_motor = [scale](uint16_t amplitude) -> uint8_t {
                            if (amplitude < 100) {
                                return 0;
                            }
                            double scaled = amplitude * scale;
                            if (scaled < 512.0) {
                                scaled = 512.0;
                            }
                            return static_cast<uint8_t>(
                                std::clamp(static_cast<int>(scaled) >> 8, 0, 255));
                        };
                        feedback.rumble = true;
                        feedback.haptics = true;
                        feedback.strong = to_motor(left);
                        feedback.weak = to_motor(right);
                        ++feedback.version;
                    }
                } else if (haptics_on &&
                           Clock::now() - last_haptics > std::chrono::milliseconds(100)) {
                    feedback.rumble = false;
                    feedback.haptics = false;
                    feedback.strong = 0;
                    feedback.weak = 0;
                    ++feedback.version;
                    haptics_on = false;
                }
                pad_.SetConsoleFeedback(feedback);
            }
            pad_.PumpOutput();
            if (timer) {
                ::WaitForSingleObject(timer, 8);
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(4));
            }
        }
        if (timer) {
            ::CloseHandle(timer);
        }
        if (mmcss) {
            ::AvRevertMmThreadCharacteristics(mmcss);
        }
    });
}

void NextApp::StopPadThread() {
    pad_run_.store(false);
    if (pad_thread_.joinable()) {
        pad_thread_.join();
    }
    pad_.SetConsoleFeedback(brp::PadConsoleFeedback{});
    pad_.PumpOutput();
}

bool NextApp::OpenAudio() {
    if (audio_.is_open()) {
        return true;
    }
    const std::string device =
        settings_.audio.device == "default" ? std::string() : settings_.audio.device;
    if (audio_.Open(device, settings_.audio.buffer_ms)) {
        audio_note_.clear();
        return true;
    }
    audio_note_ = L"Audio device unavailable";
    return false;
}

bool NextApp::EnsureDlssSr(int input_width, int input_height) {
    if (!settings_.ai.super_resolution || !system_.dlss_sr || dlss_failed_) {
        return false;
    }
    if (dlss_ready_ && dlss_in_w_ == input_width && dlss_in_h_ == input_height) {
        return true;
    }
    // DLAA and an explicit 1080p SR output stay at the stream resolution:
    // there is no upscale to pay for, and the DLSS-G worker can run at 1080p
    // (its clean 120 fps case) instead of 1440p.
    const bool dlaa = settings_.ai.sr_quality >= 5 ||
                      settings_.ai.sr_output == Resolution::R1080;
    int out_w = input_width;
    int out_h = input_height;
    int quality = 5;
    if (!dlaa) {
        out_w = 2560;
        out_h = 1440;
        if (settings_.ai.sr_output == Resolution::R2160) {
            out_w = 3840;
            out_h = 2160;
        }
        if (out_w <= input_width || out_h <= input_height) {
            out_w = 2560;
            out_h = 1440;
        }
        switch (settings_.ai.sr_quality) {
            case 0:
            case 1: quality = 0; break;
            case 2: quality = 1; break;
            case 3: quality = 2; break;
            default: quality = 4; break;
        }
    }
    if (!dlss_sr_) {
        dlss_sr_ = std::make_unique<brp::media::NvDlssSr>();
    }
    if (!dlss_sr_->runtime_loaded()) {
        std::filesystem::path directory(system_.exe_dir);
        if (!std::filesystem::exists(directory / "nvngx_dlss.dll")) {
            const std::filesystem::path nested =
                directory / "nvidia-ngx" / "runtime";
            if (std::filesystem::exists(nested / "nvngx_dlss.dll")) {
                directory = nested;
            }
        }
        std::string detail;
        if (!dlss_sr_->LoadRuntime(directory.wstring(), detail)) {
            dlss_failed_ = true;
            Toast(L"DLSS runtime: " + Widen(detail), true);
            return false;
        }
    }
    std::string detail;
    if (!dlss_sr_->Initialize(presenter_->device(), presenter_->context(), input_width,
                              input_height, out_w, out_h, 12, quality, detail)) {
        dlss_failed_ = true;
        Toast(L"DLSS SR: " + Widen(detail), true);
        return false;
    }
    dlss_ready_ = true;
    dlss_in_w_ = input_width;
    dlss_in_h_ = input_height;
    return true;
}

bool NextApp::EnsureDlssFg(int width, int height) {
    if (!settings_.ai.frame_generation ||
        settings_.ai.fg_backend != AiBackend::DlssFg) {
        return false;
    }
    if (dlss_fg_failed_) {
        return false;
    }
    const int requested = std::clamp(settings_.ai.fg_multiplier, 2, 6);
    const int multiplier =
        (requested > 2 && !system_.dlss_fg_multi) ? 2 : requested;
    if (dlss_fg_ && dlss_fg_->ready() && dlss_fg_->width() == width &&
        dlss_fg_->height() == height) {
        if ((multiplier > 2) != dlss_fg_->proxy_runtime()) {
            // Crossing the 2x boundary changes which runtime is loaded:
            // official NVIDIA below it, SM86 proxy above it.
            StopDlssFg();
        } else {
            dlss_fg_->SetMultiplier(multiplier);
            return true;
        }
    }
    if (width <= 0 || height <= 0) {
        return false;
    }
    // The printable window is the worker's own presenter; anything larger is
    // refused instead of being silently scaled.
    if (width > 2560 || height > 1440) {
        return false;
    }
    if (dlss_fg_) {
        StopDlssFg();
    }
    if (!system_.dlss_fg || system_.nr_dir.empty()) {
        dlss_fg_failed_ = true;
        Toast(L"DLSS-G runtime missing: nr/native/nvngx_dlssg.dll", true);
        return false;
    }
    dlss_fg_ = std::make_unique<DlssFg>();
    std::string detail;
    if (!dlss_fg_->Start(system_.nr_dir, width, height, multiplier, detail)) {
        dlss_fg_.reset();
        dlss_fg_failed_ = true;
        Toast(L"DLSS-G: " + Widen(detail), true);
        return false;
    }
    fg_covering_ = false;
    fg_fps_ = 0.0;
    fg_actual_mult_ = 0;
    fg_dropped_ = 0;
    Toast(std::to_wstring(multiplier) + L"x DLSS-G active at " +
          std::to_wstring(width) + L"x" + std::to_wstring(height));
    return true;
}

void NextApp::StopDlssFg() {
    if (dlss_fg_) {
        dlss_fg_->Stop();
        dlss_fg_.reset();
    }
    fg_covering_ = false;
    fg_fps_ = 0.0;
    fg_actual_mult_ = 0;
}
void NextApp::ConnectStream() {
    if (!streaming_) {
        return;
    }
    ConsoleSettings console = settings_.console;
    console.fps = settings_.streaming.fps;
    if (console.address.empty()) {
        console.address = streaming_->Profile().host;
    }
    OpenAudio();
    streaming_->SetAudio(audio_.is_open() ? &audio_ : nullptr);
    streaming_->SetStreamSettings(settings_.streaming);
    const Status status = streaming_->Connect(console);
    if (status.ok()) {
        stream_restart_pending_ = false;
        watch_on_connect_ = true;
        user_disconnect_ = false;
        connect_requested_ = true;
        reconnect_at_ = -1.0;
        Toast(L"Connecting...");
    } else {
        Toast(Widen(status.message()), true);
    }
}

void NextApp::DisconnectStream() {
    if (!streaming_) {
        return;
    }
    if (watching_) {
        ToggleWatch(false);
    }
    StopRifeThread();
    streaming_->Disconnect();
    streaming_->SetAudio(nullptr);
    audio_.Close();
    watch_on_connect_ = false;
    user_disconnect_ = true;
    connect_requested_ = false;
    reconnect_attempts_ = 0;
    stream_restart_pending_ = false;
    esc_at_ = -1.0;
    Toast(L"Disconnected");
}

void NextApp::ToggleBorderlessFullscreen() {
    if (!window_) {
        return;
    }
    if (window_->fullscreen()) {
        window_->ExitFullscreen();
        watch_fullscreen_ = false;
    } else {
        window_->EnterFullscreen();
        if (watching_) {
            watch_fullscreen_ = true;
        }
    }
    Resize();
}
void NextApp::WriteSessionReport() {
    AppPaths paths = AppPaths::Default();
    const auto path = paths.settings_file.parent_path() / "session-report.json";
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        Toast(L"Cannot write session report", true);
        return;
    }
    const StreamSnapshot stream = streaming_ ? streaming_->Snapshot() : StreamSnapshot{};
    const DiagnosticsSnapshot diagnostics = host_->Diagnostics()->Snapshot();
    file << "{\n";
    file << "  \"app\": \"BetterRemotePlayNext\",\n";
    file << "  \"version\": \"" << Narrow(kVersion).c_str() << "\",\n";
    file << "  \"source\": \"" << stream.console_address << "\",\n";
    file << "  \"state\": \"" << stream.state << "\",\n";
    file << "  \"connected\": " << (stream.connected ? "true" : "false") << ",\n";
    file << "  \"source_fps\": " << stream.source_fps << ",\n";
    file << "  \"decode_ms\": " << stream.decode_ms << ",\n";
    file << "  \"bitrate_kbps\": " << stream.bitrate_kbps << ",\n";
    file << "  \"loss_percent\": " << stream.loss_percent << ",\n";
    file << "  \"rife_ready\": " << (rife_ready_ ? "true" : "false") << ",\n";
    file << "  \"rife_generated_per_s\": " << rife_generated_rate_ << ",\n";
    file << "  \"displayed_per_s\": " << display_rate_ << ",\n";
    file << "  \"dlss_sr\": " << (dlss_ready_ ? "true" : "false") << ",\n";
    file << "  \"dlss_fg\": " << ((dlss_fg_ && dlss_fg_->ready()) ? "true" : "false")
         << ",\n";
    file << "  \"fg_multiplier\": " << settings_.ai.fg_multiplier << ",\n";
    file << "  \"fg_actual_multiplier\": " << fg_actual_mult_ << ",\n";
    file << "  \"fg_displayed_fps\": " << fg_fps_ << ",\n";
    file << "  \"fg_dropped\": " << fg_dropped_ << ",\n";
    file << "  \"ui_p50_ms\": " << diagnostics.frame_p50_ms << ",\n";
    file << "  \"ui_p95_ms\": " << diagnostics.frame_p95_ms << ",\n";
    file << "  \"ui_p99_ms\": " << diagnostics.frame_p99_ms << ",\n";
    file << "  \"generated_total\": " << rife_generated_.load() << ",\n";
    file << "  \"ui_frames\": " << ui_frames_ << "\n";
    file << "}\n";
    Toast(L"Session report: " + path.wstring());
}
void NextApp::ApplyPresenterTarget() {
    if (!presenter_) {
        return;
    }
    const int target = settings_.video.render_target;
    if (target == applied_render_target_) {
        return;
    }
    int width = 0;
    int height = 0;
    if (target == 1080) {
        width = 1920;
        height = 1080;
    } else if (target == 1440) {
        width = 2560;
        height = 1440;
    } else if (target == 2160) {
        width = 3840;
        height = 2160;
    }
    presenter_->SetUpscaleTarget(width, height);
    applied_render_target_ = target;
}
void NextApp::StartBenchmark() {
    if (bench_active_) {
        return;
    }
    bench_saved_ = settings_;
    bench_active_ = true;
    bench_case_ = 0;
    bench_phase_ = 0;
    bench_at_ = now_;
    bench_results_.clear();
    Toast(L"Benchmark: 4 cases, about one minute");
}

void NextApp::TickBenchmark() {
    struct Case {
        int fps;
        int target;
    };
    static const Case cases[] = {{30, 1440}, {30, 2160}, {60, 1440}, {60, 2160}};
    if (!streaming_ || !presenter_) {
        bench_active_ = false;
        return;
    }
    if (bench_case_ >= 4) {
        WriteBenchReport();
        bench_active_ = false;
        settings_ = bench_saved_;
        Apply();
        if (bench_exit_) {
            window_->Close();
        }
        return;
    }
    const Case& current = cases[bench_case_];
    const StreamSnapshot snapshot = streaming_->Snapshot();

    if (bench_phase_ == 0) {
        if (bench_case_ > 0 && now_ - bench_at_ < 20.0) {
            return;
        }
        settings_.streaming.fps = current.fps;
        settings_.video.render_target = current.target;
        settings_.ai.super_resolution = false;
        settings_.ai.frame_generation = false;
        settings_.overlay.enabled = false;
        Apply();
        ApplyPresenterTarget();
        if (snapshot.connected || snapshot.connecting) {
            streaming_->Disconnect();
            StopRifeThread();
        }
        ConnectStream();
        bench_phase_ = 1;
        bench_at_ = now_;
        return;
    }

    if (bench_phase_ == 1) {
        if (snapshot.connected) {
            if (!watching_) {
                ToggleWatch(true);
            }
            if (now_ - bench_at_ >= 3.0) {
                bench_phase_ = 2;
                bench_at_ = now_;
                bench_shown_start_ = display_shown_.load();
                bench_source_sum_ = 0.0;
                bench_decode_sum_ = 0.0;
                bench_samples_ = 0;
            }
        } else if (now_ - bench_at_ > 20.0) {
            BenchResult result;
            result.requested_fps = current.fps;
            result.target = current.target;
            result.ok = false;
            bench_results_.push_back(result);
            ToggleWatch(false);
            StopRifeThread();
            streaming_->Disconnect();
            audio_.Close();
            bench_case_++;
            bench_phase_ = 0;
            bench_at_ = now_;
        }
        return;
    }

    // Measurement.
    bench_source_sum_ += snapshot.source_fps;
    bench_decode_sum_ += snapshot.decode_ms;
    ++bench_samples_;
    if (now_ - bench_at_ < 10.0) {
        return;
    }
    const double elapsed = std::max(0.001, now_ - bench_at_);
    const DiagnosticsSnapshot diagnostics = host_->Diagnostics()->Snapshot();
    BenchResult result;
    result.requested_fps = current.fps;
    result.target = current.target;
    result.ok = snapshot.connected;
    result.source_fps =
        bench_samples_ > 0 ? bench_source_sum_ / bench_samples_ : snapshot.source_fps;
    result.decode_ms =
        bench_samples_ > 0 ? bench_decode_sum_ / bench_samples_ : snapshot.decode_ms;
    result.shown_fps =
        static_cast<double>(display_shown_.load() - bench_shown_start_) / elapsed;
    result.loss_percent = snapshot.loss_percent;
    result.ui_p50 = diagnostics.frame_p50_ms;
    result.ui_p95 = diagnostics.frame_p95_ms;
    result.ui_p99 = diagnostics.frame_p99_ms;
    result.generated = rife_generated_.load();
    bench_results_.push_back(result);
    ToggleWatch(false);
    StopRifeThread();
    streaming_->Disconnect();
    audio_.Close();
    bench_case_++;
    bench_phase_ = 0;
    bench_at_ = now_;
}

void NextApp::WriteBenchReport() {
    const AppPaths paths = AppPaths::Default();
    const auto folder = paths.settings_file.parent_path();
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    const auto path = folder / ("benchmark-" + std::to_string(std::time(nullptr)) + ".json");
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        Toast(L"Cannot write benchmark report", true);
        return;
    }
    file << "{\n";
    file << "  \"app\": \"BetterRemotePlayNext\",\n";
    file << "  \"gpu\": \"" << system_.gpu.name << "\",\n";
    file << "  \"driver\": \"" << system_.gpu.driver << "\",\n";
    if (!system_.displays.empty()) {
        file << "  \"display\": \"" << system_.displays.front().width << "x"
             << system_.displays.front().height << " @ "
             << system_.displays.front().refresh_hz << " Hz\",\n";
    }
    file << "  \"cases\": [\n";
    for (std::size_t i = 0; i < bench_results_.size(); ++i) {
        const auto& r = bench_results_[i];
        file << "    {\"requested_fps\": " << r.requested_fps
             << ", \"target\": " << r.target << ", \"ok\": " << (r.ok ? "true" : "false")
             << ", \"source_fps\": " << r.source_fps
             << ", \"shown_fps\": " << r.shown_fps << ", \"decode_ms\": " << r.decode_ms
             << ", \"loss_percent\": " << r.loss_percent << ", \"ui_p50_ms\": " << r.ui_p50
             << ", \"ui_p95_ms\": " << r.ui_p95 << ", \"ui_p99_ms\": " << r.ui_p99
             << ", \"generated\": " << r.generated << "}";
        if (i + 1 != bench_results_.size()) {
            file << ',';
        }
        file << '\n';
    }
    file << "  ],\n";
    file << "  \"note\": \"2160p targets are rendered and fitted to the desktop display\"\n";
    file << "}\n";
    Toast(L"Benchmark report: " + path.wstring());
}
void NextApp::RecordVideoShown() {
    display_shown_.fetch_add(1);
    if (last_video_shown_at_ >= 0.0) {
        const double interval_ms = (now_ - last_video_shown_at_) * 1000.0;
        if (interval_ms > 0.0 && interval_ms < 1000.0) {
            video_intervals_.Add(interval_ms);
        }
    }
    last_video_shown_at_ = now_;
}
}  // namespace brp::next


