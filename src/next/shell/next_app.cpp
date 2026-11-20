#include "shell/next_app.h"

#include "ai/dlss_fg.h"
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

Color Alpha(const Color& color, float alpha) {
    return Color{color.r, color.g, color.b,
                 static_cast<std::uint8_t>(std::clamp(alpha, 0.0f, 1.0f) * 255)};
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
        if (watching_ && presenter_->frame_waitable()) {
            ::WaitForSingleObject(presenter_->frame_waitable(), 8);
        }
        Frame();
        if (!watching_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
    }
    ::timeEndPeriod(1);
    StopPadThread();
    StopDlssFg();
    overlay_.Destroy();
    audio_.Close();
    if (streaming_) {
        streaming_->SetAudio(nullptr);
        streaming_->Disconnect();
    }
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
                             watching_ ? 0 : static_cast<int>(150.0f * scale_));
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
        streaming_->Disconnect();
        Toast(L"GPU device lost; restart the app", true);
    }
    HandlePinInput();
    const bool fg_watch = watching_ && dlss_fg_ && dlss_fg_->ready() &&
                          settings_.overlay.enabled;
    if (fg_watch) {
        const double interval = std::max(0.05, settings_.overlay.interval_ms / 1000.0);
        if (last_overlay_update_ < 0.0 || now_ - last_overlay_update_ >= interval) {
            const StreamSnapshot snapshot = streaming_ ? streaming_->Snapshot()
                                                       : StreamSnapshot{};
            std::vector<std::wstring> lines;
            wchar_t buffer[200] = {};
            const int shown_mult =
                fg_actual_mult_ > 1 ? fg_actual_mult_ : settings_.ai.fg_multiplier;
            std::swprintf(buffer, 200, L"DLSS-G %dx  |  %.1f fps", shown_mult, fg_fps_);
            lines.emplace_back(buffer);
            std::swprintf(buffer, 200,
                          L"source %.1f fps  |  decode %.2f ms  |  %.0f kbps",
                          snapshot.source_fps, snapshot.decode_ms, snapshot.bitrate_kbps);
            lines.emplace_back(buffer);
            lines.emplace_back(L"ESC  Back to console");
            overlay_.Update(lines, std::max(0.90f, settings_.overlay.opacity),
                            settings_.overlay.position);
            overlay_.Raise();
            last_overlay_update_ = now_;
        }
    } else if (watching_ && streaming_ && streaming_->Connected() &&
               settings_.overlay.enabled) {
        const double interval = std::max(0.05, settings_.overlay.interval_ms / 1000.0);
        if (last_overlay_update_ < 0.0 || now_ - last_overlay_update_ >= interval) {
            const StreamSnapshot snapshot = streaming_->Snapshot();
            std::vector<std::wstring> lines;
            wchar_t buffer[160] = {};
            std::swprintf(buffer, 160, L"%dx%d  |  %.1f fps", snapshot.source_width,
                          snapshot.source_height, snapshot.source_fps);
            lines.emplace_back(buffer);
            std::swprintf(buffer, 160,
                          L"decode %.2f ms  |  cad %.1f/%.1f ms  |  %.0f kbps",
                          snapshot.decode_ms, cadence_p50_, cadence_p99_,
                          snapshot.bitrate_kbps);
            lines.emplace_back(buffer);
            if (rife_ready_ && !rife_failed_) {
                wchar_t rife_line[96] = {};
                std::swprintf(rife_line, 96, L"RIFE  |  %d gen/s  |  %d shown/s",
                              rife_generated_rate_, display_rate_);
                lines.emplace_back(rife_line);
            } else {
                lines.emplace_back(dlss_ready_ ? L"DLSS SR 1440p active" : L"native presentation");
            }
            if (watching_) {
                lines.emplace_back(L"ESC  Back to console");
            }
            overlay_.Update(lines, settings_.overlay.opacity, settings_.overlay.position);
            last_overlay_update_ = now_;
        }
    } else if (overlay_.visible()) {
        overlay_.Hide();
    }
    if (rife_generated_at_ < 0.0 || now_ - rife_generated_at_ >= 1.0) {
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
            // The worker's window owns the visible picture. The parent still
            // presents an empty frame so the waitable keeps the 240 Hz loop
            // cadence that gates the source frames.
            renderer_->BeginFrame();
            renderer_->EndFrame();
            presenter_->PresentNow(true);
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
    widgets.StatusPill(Rect(330, 11, 430, kTitleHeight - 11), L"0.5.0",
                       theme.accent, true);
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
        renderer_->Text(L"Next 0.5.0", Rect(20, logical_height_ - 56, width - 16,
                                           logical_height_ - 40),
                        10.5f, ToD2D(theme.text_faint), DWRITE_FONT_WEIGHT_SEMI_BOLD);
        renderer_->Text(L"streaming / AI / overlay", Rect(20, logical_height_ - 40,
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
           settings_.ai.frame_generation ? BackendLabel(settings_.ai.fg_backend)
                                          : L"Off",
           settings_.ai.frame_generation
               ? (settings_.ai.fg_backend == AiBackend::DlssFg
                      ? std::to_wstring(
                            std::clamp(settings_.ai.fg_multiplier, 2, 6)) +
                            L"x  |  " + std::to_wstring(settings_.ai.target_fps) +
                            L" fps target"
                      : L"30 -> 60  |  " +
                            std::to_wstring(settings_.ai.target_fps) + L" fps target")
               : L"source only");

    y += 142.0f + gap;
    widgets.Card(Rect(x, y, x + w, y + 176.0f));
    widgets.Section(Rect(x + 18, y + 14, x + w - 260, y + 30), L"AI CAPABILITIES");

    struct CapabilityTile {
        const wchar_t* title;
        bool available;
        bool reference;
        const wchar_t* status;
        const wchar_t* detail;
    };
    const CapabilityTile tiles[] = {
        {L"DLSS Super Resolution", system_.dlss_sr, false,
         system_.dlss_sr ? L"AVAILABLE" : L"MISSING", L"nvngx_dlss.dll"},
        {L"DLSS Frame Generation", system_.dlss_fg, false,
         system_.dlss_fg ? L"AVAILABLE" : L"MISSING",
         system_.dlss_fg ? (system_.dlss_fg_multi ? L"worker 2x-6x" : L"worker 2x")
                         : L"nvngx_dlssg.dll"},
        {L"RIFE Lite 4.25", system_.rife_lite, false,
         system_.rife_lite ? L"AVAILABLE" : L"MISSING", L"TensorRT 1080p"},
        {L"RIFE Heavy 4.26", system_.rife_heavy, false,
         system_.rife_heavy ? L"AVAILABLE" : L"MISSING", L"engine built on demand"},
        {L"TensorRT", system_.tensorrt_runtime, false,
         system_.tensorrt_runtime ? L"AVAILABLE" : L"MISSING", L"nvinfer_10.dll"},
    };
    int available_count = 0;
    for (const auto& tile : tiles) {
        if (tile.available) {
            ++available_count;
        }
    }
    renderer_->Text((std::to_wstring(available_count) + L" available").c_str(),
                    Rect(x + w - 240, y + 14, x + w - 18, y + 30), 10.5f,
                    ToD2D(theme.text_faint), DWRITE_FONT_WEIGHT_MEDIUM,
                    DWRITE_TEXT_ALIGNMENT_TRAILING);

    const float tile_gap = 10.0f;
    const float tile_w = (w - 36.0f - tile_gap * 4.0f) / 5.0f;
    const float tile_h = 92.0f;
    const float tile_y = y + 38.0f;
    for (int i = 0; i < 5; ++i) {
        const CapabilityTile& tile = tiles[i];
        const float tx = x + 18.0f + (tile_w + tile_gap) * i;
        const D2D1_RECT_F rect = Rect(tx, tile_y, tx + tile_w, tile_y + tile_h);
        const Color accent = tile.available ? theme.success : theme.warning;
        renderer_->FillRounded(rect, 12.0f, ToD2D(theme.surface_alt));
        renderer_->StrokeRounded(rect, 12.0f,
                                ToD2D(tile.available
                                          ? Alpha(theme.success, 0.28f)
                                          : theme.border),
                                1.0f);
        renderer_->FillCircle(tx + 16.0f, tile_y + 22.0f, 4.0f, ToD2D(accent));
        renderer_->TextEllipsis(tile.title, Rect(tx + 28.0f, tile_y + 10.0f,
                                                tx + tile_w - 10.0f, tile_y + 34.0f),
                                12.5f, ToD2D(theme.text),
                                DWRITE_FONT_WEIGHT_SEMI_BOLD);
        renderer_->Text(tile.status, Rect(tx + 14.0f, tile_y + 42.0f,
                                         tx + tile_w - 10.0f, tile_y + 58.0f),
                        9.5f, ToD2D(accent), DWRITE_FONT_WEIGHT_BOLD);
        renderer_->TextEllipsis(tile.detail, Rect(tx + 14.0f, tile_y + 62.0f,
                                                 tx + tile_w - 10.0f, tile_y + 80.0f),
                                9.5f, ToD2D(theme.text_faint));
    }
    renderer_->Text(
        L"Capabilities are file/SDK checks. DLSS SR, DLSS-G and RIFE run in the "
        L"Watch view.",
        Rect(x + 18, y + 146, x + w - 18, y + 164), 10.5f, ToD2D(theme.text_faint));
    y += 176.0f + 18.0f;
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
    renderer_->Text(L"Discovery, PSN account, pairing and session control",
                    Rect(x, 92, x + w, 112), 12.0f, ToD2D(theme.text_dim));

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

    float y = 124.0f;
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

    SettingsControls controls(widgets, settings_, system_, true);
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
    auto capability_card = [&](int index, const wchar_t* title, const wchar_t* status,
                               bool good, const wchar_t* detail) {
        const float cx = x + (card_w + gap) * index;
        widgets.Card(Rect(cx, 124, cx + card_w, 204));
        renderer_->Text(title, Rect(cx + 14, 136, cx + card_w - 14, 160), 13.0f,
                        ToD2D(theme.text), DWRITE_FONT_WEIGHT_SEMI_BOLD);
        widgets.StatusPill(Rect(cx + 14, 166, cx + 150, 194), status,
                           good ? theme.success : theme.warning, true);
        renderer_->Text(detail, Rect(cx + 156, 166, cx + card_w - 10, 194), 10.0f,
                        ToD2D(theme.text_faint));
    };
    capability_card(0, L"DLSS Super Resolution",
                    system_.dlss_sr ? L"AVAILABLE" : L"MISSING", system_.dlss_sr,
                    L"nvngx_dlss.dll");
    capability_card(1, L"DLSS Frame Generation",
                    system_.dlss_fg ? L"AVAILABLE" : L"MISSING", system_.dlss_fg,
                    system_.dlss_fg
                        ? (system_.dlss_fg_multi ? L"worker - up to 6x"
                                                 : L"worker - 2x")
                        : L"nvngx_dlssg.dll");
    capability_card(2, L"RIFE TensorRT", system_.rife_lite ? L"AVAILABLE" : L"MISSING",
                    system_.rife_lite, L"30 -> 60");
    capability_card(3, L"TensorRT runtime",
                    system_.tensorrt_runtime ? L"AVAILABLE" : L"MISSING",
                    system_.tensorrt_runtime, L"nvinfer_10");
    renderer_->Text(L"DLSS-G runs through the NVIDIA worker; 2x is the clean cadence. "
                    L"RIFE is the in-process 30 -> 60 path.",
                    Rect(x, 210, x + w, 230), 10.5f, ToD2D(theme.text_faint));
    SettingsControls controls(widgets, settings_, system_, true);
    controls.Reset();
    controls.Ai(x, 232, w);
    if (controls.Changed()) {
        Apply();
    }

}

void NextApp::RenderDisplay(Widgets& widgets) {
    const ThemeTokens& theme = widgets.Tokens();
    const float x = ContentX();
    const float w = ContentWidth();
    renderer_->Text(L"Display", Rect(x, 58, x + w, 92), 26.0f, ToD2D(theme.text),
                    DWRITE_FONT_WEIGHT_SEMI_BOLD);
    renderer_->Text(L"Detected monitors, modes and presentation preferences",
                    Rect(x, 92, x + w, 112), 12.0f, ToD2D(theme.text_dim));

    widgets.Card(Rect(x, 128, x + w, 300));
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

    SettingsControls controls(widgets, settings_, system_, true);
    controls.Reset();
    const float end = controls.Display(x, 322, w);
    if (controls.Changed()) {
        Apply();
    }
    renderer_->Text(L"F11 toggles borderless fullscreen. Fullscreen on play enters it "
                    L"automatically when Watch starts.",
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

    SettingsControls controls(widgets, settings_, system_, true);
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

    widgets.Card(Rect(x, 124, x + w, 216));
    widgets.Section(Rect(x + 18, 136, x + w - 18, 152), L"QUICK PRESETS");
    auto apply_preset = [&](const wchar_t* name, AppSettings preset) {
        settings_ = std::move(preset);
        Apply();
        Toast(std::wstring(L"Preset applied: ") + name);
    };
    const float preset_y = 160.0f;
    if (widgets.Button(3110, Rect(x + 18, preset_y, x + 158, preset_y + 38), L"Native",
                       false)) {
        AppSettings preset = settings_;
        preset.video.output = Resolution::Native;
        preset.video.render_target = 0;
        preset.video.vsync = true;
        preset.streaming.frame_buffer = 1;
        preset.ai.super_resolution = false;
        preset.ai.frame_generation = false;
        apply_preset(L"Native", preset);
    }
    if (widgets.Button(3111, Rect(x + 168, preset_y, x + 348, preset_y + 38),
                       L"1440p Sharp", false)) {
        AppSettings preset = settings_;
        preset.video.output = Resolution::R1440;
        preset.video.render_target = 1440;
        preset.ai.super_resolution = true;
        preset.ai.sr_output = Resolution::R1440;
        preset.ai.sr_quality = 3;
        preset.ai.frame_generation = false;
        apply_preset(L"1440p Sharp", preset);
    }
    if (widgets.Button(3112, Rect(x + 358, preset_y, x + 548, preset_y + 38),
                       L"1440p RIFE", true)) {
        AppSettings preset = settings_;
        preset.video.output = Resolution::R1440;
        preset.video.render_target = 1440;
        preset.ai.super_resolution = false;
        preset.ai.frame_generation = true;
        preset.ai.fg_backend = AiBackend::Rife;
        preset.ai.target_fps = 60;
        preset.ai.rife_model = 0;
        apply_preset(L"1440p RIFE", preset);
    }
    if (widgets.Button(3113, Rect(x + 558, preset_y, x + 738, preset_y + 38),
                       L"Low latency", false)) {
        AppSettings preset = settings_;
        preset.video.output = Resolution::Native;
        preset.video.render_target = 0;
        preset.video.vsync = false;
        preset.streaming.frame_buffer = 0;
        preset.ai.super_resolution = false;
        preset.ai.frame_generation = false;
        apply_preset(L"Low latency", preset);
    }

    if (profiles_.empty()) {
        widgets.Card(Rect(x, 228, x + w, 324));
        renderer_->Text(L"No profiles yet.", Rect(x + 18, 246, x + w - 18, 274), 15.0f,
                        ToD2D(theme.text), DWRITE_FONT_WEIGHT_SEMI_BOLD);
        renderer_->Text(L"Save the current configuration as the first profile.",
                        Rect(x + 18, 276, x + w - 18, 302), 11.5f,
                        ToD2D(theme.text_faint));
        return;
    }

    float y = 228.0f;
    int index = 0;
    for (const auto& profile : profiles_) {
        if (index >= 8) {
            break;
        }
        widgets.Card(Rect(x, y, x + w, y + 72));
        renderer_->Text(Widen(profile.name).c_str(),
                        Rect(x + 18, y + 10, x + w - 460, y + 34), 15.0f,
                        ToD2D(theme.text), DWRITE_FONT_WEIGHT_SEMI_BOLD);
        const std::wstring warning = ProfileWarning(profile);
        const std::wstring summary = Summary(profile) +
                                     (warning.empty() ? std::wstring()
                                                      : L"  |  needs " + warning);
        renderer_->TextEllipsis(summary.c_str(),
                                Rect(x + 18, y + 36, x + w - 460, y + 60), 11.0f,
                                ToD2D(warning.empty() ? theme.text_dim : theme.warning));
        if (profile.is_default) {
            widgets.StatusPill(Rect(x + w - 450, y + 20, x + w - 370, y + 48), L"DEFAULT",
                               theme.accent, true);
        }
        const float bx = x + w - 356.0f;
        if (widgets.Button(3200 + index * 10, Rect(bx, y + 18, bx + 78, y + 54), L"Apply",
                           true)) {
            settings_ = profile.settings;
            Apply();
            Toast(warning.empty() ? std::wstring(L"Profile applied")
                                  : L"Applied; missing: " + warning,
                  !warning.empty());
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
    SettingsControls controls(widgets, settings_, system_, true);
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
    if (widgets.Button(9502, Rect(x + 210, ly + 8, x + 410, ly + 46), L"Reset settings",
                       false)) {
        const Status status = host_->ResetSettings();
        if (status.ok()) {
            settings_ = host_->Settings();
            haptic_strength_.store(settings_.controller.rumble_strength);
            dlss_ready_ = false;
            dlss_failed_ = false;
            if (dlss_sr_) {
                dlss_sr_->Shutdown();
            }
            Toast(L"Settings reset to defaults");
        } else {
            Toast(Widen(status.message()), true);
        }
    }
}

void NextApp::Apply() {
    const AiSettings previous_ai = settings_.ai;
    const Status status = host_->ApplySettings(settings_);
    if (!status.ok()) {
        Toast(Widen(status.message()), true);
        settings_ = host_->Settings();
        return;
    }
    settings_ = host_->Settings();
    haptic_strength_.store(settings_.controller.rumble_strength);
    ApplyPresenterTarget();
    if (previous_ai.super_resolution != settings_.ai.super_resolution ||
        previous_ai.sr_output != settings_.ai.sr_output ||
        previous_ai.sr_quality != settings_.ai.sr_quality) {
        dlss_ready_ = false;
        dlss_failed_ = false;
        if (dlss_sr_) {
            dlss_sr_->Shutdown();
        }
    }
    if (previous_ai.frame_generation != settings_.ai.frame_generation ||
        previous_ai.fg_backend != settings_.ai.fg_backend ||
        previous_ai.fg_multiplier != settings_.ai.fg_multiplier) {
        StopDlssFg();
        dlss_fg_failed_ = false;
    }
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

void NextApp::HandlePinInput() {
    const int key = window_->ConsumeKey();
    const int character = window_->ConsumeChar();
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
        std::to_wstring(s.ai.target_fps) + L" fps";
    if (s.ai.frame_generation) {
        summary += L"  |  " + BackendLabel(s.ai.fg_backend);
    }
    if (s.ai.super_resolution) {
        summary += L"  |  SR " + Widen(std::string(ToString(s.ai.sr_output)));
    }
    return summary;
}

void NextApp::PresentVideo() {
    if (!streaming_) {
        return;
    }
    // Use the requested source cadence, not the instantaneous measured rate.
    // The measured fps jitters with decode/queue timing; feeding it into the
    // phase accumulator made one slot drift every ~1 s and read as a small
    // "teleport". The requested rate is the cadence the console is asked for
    // and divides the refresh exactly at 30/60 on a 240 Hz panel.
    const double content =
        std::clamp(static_cast<double>(settings_.streaming.fps), 24.0, 240.0);
    const int refresh = std::max(24, window_->refresh_hz());
    video_phase_ += content / static_cast<double>(refresh);
    bool due = video_phase_ >= 1.0 - 1e-9;
    if (due) {
        video_phase_ -= 1.0;
    } else {
        video_phase_ = std::max(0.0, video_phase_);
    }

    const bool fg_mode = settings_.ai.frame_generation &&
                         settings_.ai.fg_backend == AiBackend::DlssFg &&
                         !dlss_fg_failed_;
    if (fg_mode) {
        const float background[4] = {0.01f, 0.008f, 0.016f, 1.0f};
        bool new_frame = false;
        if (due) {
            media::DecodedFrame frame;
            if (streaming_->TakeFrame(frame, settings_.streaming.frame_buffer, true)) {
                video_frame_ = std::move(frame);
                have_frame_ = true;
                new_frame = true;
            }
        }

        bool fed = false;
        if (new_frame && have_frame_ && !video_frame_.rgba.empty()) {
            int feed_w = video_frame_.width;
            int feed_h = video_frame_.height;
            std::vector<std::uint8_t> rgba;
            bool have_rgba = false;
            if (settings_.ai.super_resolution) {
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
            if (EnsureDlssFg(feed_w, feed_h)) {
                dlss_fg_->Submit(have_rgba ? rgba.data() : video_frame_.rgba.data(),
                                 feed_w, feed_h);
                fed = true;
                RecordVideoShown();
            } else if (have_rgba &&
                       EnsureDlssFg(video_frame_.width, video_frame_.height)) {
                // SR output is too large for the worker (2160p); feed the
                // stream frame at its own size rather than inventing a scale.
                dlss_fg_->Submit(video_frame_.rgba.data(), video_frame_.width,
                                 video_frame_.height);
                fed = true;
                RecordVideoShown();
            }
        }
        (void)fed;

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
            // Keep showing the source so the first seconds are not black.
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
        presenter_->Clear(background);
        return;
    }
    const bool rife_mode =
        settings_.ai.frame_generation && settings_.ai.fg_backend == AiBackend::Rife &&
        content < 45.0 && EnsureRife();
    if (rife_mode) {
        rife_interval_ = 1.0 / std::clamp(content, 24.0, 60.0);
        bool new_frame = false;
        if (due) {
            media::DecodedFrame frame;
            if (streaming_->TakeFrame(frame, settings_.streaming.frame_buffer, true)) {
                video_frame_ = std::move(frame);
                have_frame_ = true;
                new_frame = true;
            }
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
    if (due) {
        media::DecodedFrame frame;
        if (streaming_->TakeFrame(frame, settings_.streaming.frame_buffer, true)) {
            video_frame_ = std::move(frame);
            have_frame_ = true;
            new_frame = true;
        }
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
        Toast(L"RIFE engine not built; run Set up RIFE in the reference app", true);
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
        Toast(L"RIFE engine missing; build it in the reference app first", true);
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
        std::vector<uint8_t> mid;
        std::string detail;
        if (!rife_ || !rife_->Process(job.a.data(), job.b.data(), 0.5f, mid, detail)) {
            std::fprintf(stderr, "[next] RIFE process failed: %s\n", detail.c_str());
            rife_failed_ = true;
            continue;
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
void NextApp::RenderWatchOverlay() {
    const ThemeTokens& theme = ThemeFor(settings_.general.theme);
    const StreamSnapshot snapshot = streaming_ ? streaming_->Snapshot() : StreamSnapshot{};
    if (settings_.overlay.enabled) {
        renderer_->Text(L"ESC  Back to console",
                        Rect(logical_width_ - 240, logical_height_ - 34,
                             logical_width_ - 16, logical_height_ - 10),
                        10.5f, ToD2D(theme.text_dim), DWRITE_FONT_WEIGHT_MEDIUM,
                        DWRITE_TEXT_ALIGNMENT_TRAILING);
        return;
    }
    renderer_->FillRect(Rect(0, 0, logical_width_, 42.0f),
                        ToD2D(Color{theme.surface.r, theme.surface.g, theme.surface.b, 225}));
    renderer_->FillRect(Rect(0, 41.0f, logical_width_, 42.0f), ToD2D(theme.accent));
    renderer_->Text(L"LIVE", Rect(16, 0, 66, 42), 11.0f, ToD2D(theme.accent),
                    DWRITE_FONT_WEIGHT_BOLD);
    float stats_left = 72.0f;
    const bool fg_running = dlss_fg_ && dlss_fg_->ready();
    if (fg_running) {
        renderer_->Text(L"DLSS-G", Rect(70, 0, 140, 42), 10.5f, ToD2D(theme.success),
                        DWRITE_FONT_WEIGHT_BOLD);
        stats_left = 142.0f;
    } else if (dlss_ready_) {
        renderer_->Text(L"DLSS", Rect(70, 0, 122, 42), 10.5f, ToD2D(theme.success),
                        DWRITE_FONT_WEIGHT_BOLD);
        stats_left = 126.0f;
    }
    wchar_t stats[256] = {};
    if (fg_running) {
        std::swprintf(stats, 256,
                      L"%dx%d  |  %.1f generated fps  |  %dx  |  %.0f kbps",
                      snapshot.source_width, snapshot.source_height, fg_fps_,
                      fg_actual_mult_ > 1 ? fg_actual_mult_
                                          : settings_.ai.fg_multiplier,
                      snapshot.bitrate_kbps);
    } else {
        std::swprintf(stats, 256,
                      L"%dx%d  |  %.1f fps  |  cad %.1f/%.1f ms  |  decode %.2f ms  |  %.0f kbps",
                      snapshot.source_width, snapshot.source_height, snapshot.source_fps,
                      cadence_p50_, cadence_p99_, snapshot.decode_ms, snapshot.bitrate_kbps);
    }
    renderer_->Text(stats, Rect(stats_left, 0, logical_width_ - 250, 42), 11.5f,
                    ToD2D(theme.text), DWRITE_FONT_WEIGHT_MEDIUM);
    renderer_->Text(L"ESC  Back to console", Rect(logical_width_ - 230, 0,
                                                 logical_width_ - 16, 42),
                    11.0f, ToD2D(theme.text_dim), DWRITE_FONT_WEIGHT_MEDIUM,
                    DWRITE_TEXT_ALIGNMENT_TRAILING);
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
        video_phase_ = 0.0;
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
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
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
    int out_w = 2560;
    int out_h = 1440;
    if (settings_.ai.sr_output == Resolution::R2160) {
        out_w = 3840;
        out_h = 2160;
    } else if (settings_.ai.sr_output == Resolution::R1080) {
        out_w = 1920;
        out_h = 1080;
    }
    if (out_w <= input_width || out_h <= input_height) {
        out_w = 2560;
        out_h = 1440;
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
    int quality = 2;
    switch (settings_.ai.sr_quality) {
        case 0:
        case 1: quality = 0; break;
        case 2: quality = 1; break;
        default: quality = 2; break;
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
    const int multiplier = std::clamp(settings_.ai.fg_multiplier, 2, 6);
    if (dlss_fg_ && dlss_fg_->ready() && dlss_fg_->width() == width &&
        dlss_fg_->height() == height) {
        dlss_fg_->SetMultiplier(multiplier);
        return true;
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
    const Status status = streaming_->Connect(console);
    if (status.ok()) {
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


