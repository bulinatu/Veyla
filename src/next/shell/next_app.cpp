#include "shell/next_app.h"

#include "ai/dlss_fg.h"
#include "overlay/overlay_view.h"
#include "diagnostics/metrics.h"
#include "media/nv_dlss_sr.h"
#include "media/amd_fsr.h"
#include "media/nv_dlss_fg.h"
#include "media/nv_dlssnr.h"
#include "media/nv_reflex.h"
#include "media/pipeline.h"
#include "media/rgba_scale.h"
#include "platform/win32_window.h"
#include "shell/settings_controls.h"
#include "streaming/streaming_service.h"
#include "ui/ui_renderer.h"

#include <windows.h>
#include <commdlg.h>
#include <d3d11.h>
#include <d3d11_4.h>
#include <iphlpapi.h>
#include <netioapi.h>
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
NextApp* g_hook_app = nullptr;

// One source of truth for the NR parameters: the standalone NR worker and the
// NR stage inside the DLSS-G worker must see exactly the same values.
brp::media::NvDlssnr::Options NeuralOptionsFrom(const AiSettings& ai) {
    brp::media::NvDlssnr::Options options;
    options.style = std::clamp(ai.neural_style, 0, 2);
    options.intensity = std::clamp(ai.neural_intensity, 0, 9999) / 100.0f;
    options.local_tone = std::clamp(ai.neural_tone, 0, 9999) / 100.0f;
    options.local_structure = std::clamp(ai.neural_structure, 0, 9999) / 100.0f;
    options.skin_structure = std::clamp(ai.neural_skin, 0, 9999) / 100.0f;
    options.auto_mask = ai.neural_mask;
    options.ui_correction = ai.neural_ui;
    // Passes only exist with Boost (a work scale below 100 %); at full size the
    // worker runs one effective pass anyway, so do not ask it to allocate a
    // cascade that cannot be used.
    const bool boost = ai.neural_render && ai.neural_scale < 100;
    options.passes = std::clamp(boost ? ai.neural_passes : 1, 1, 9999);
    options.work_scale =
        std::clamp(ai.neural_render ? ai.neural_scale : 100, 1, 9999) / 100.0f;
    options.inject_fg_proxy = false;
    return options;
}

void HideNvidiaOverlay() {
    // The NVIDIA App overlay is a full-screen CEF window. It reappears when a
    // game-like window starts, so hide it while we own the stream surface.
    if (HWND overlay = ::FindWindowW(L"CEF-OSC-WIDGET", L"NVIDIA GeForce Overlay")) {
        ::ShowWindow(overlay, SW_HIDE);
    }
}
constexpr wchar_t kVersion[] = L"3.0.0-beta8";
// Saved profiles are a convenience, not a library. Twelve keeps the list one
// glance long; the shell and the control centre both show "n / 12".
constexpr std::size_t kMaxProfiles = 12;

// Minimal JSON string escaping for the small events the bridge publishes.
std::string JsonEscape(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (const char ch : text) {
        if (ch == '"' || ch == '\\') {
            out += '\\';
            out += ch;
        } else if (static_cast<unsigned char>(ch) >= 0x20) {
            out += ch;
        }
    }
    return out;
}

// Type of the adapter that carries the default route. Wi-Fi and Ethernet need
// different bitrates: 100 Mbps over the air queues up in the access point and
// shows up as input delay, which is the one thing this app must not add.
std::string NetworkLinkName() {
    IN_ADDR dest{};
    dest.S_un.S_addr = 0x08080808;  // 8.8.8.8; used for routing only, no packet
    DWORD index = 0;
    if (::GetBestInterface(dest.S_un.S_addr, &index) != NO_ERROR || index == 0) {
        return "unknown";
    }
    MIB_IF_ROW2 row{};
    row.InterfaceIndex = static_cast<NET_IFINDEX>(index);
    if (::GetIfEntry2(&row) != NO_ERROR) {
        return "unknown";
    }
    if (row.Type == IF_TYPE_IEEE80211) {
        return "wifi";
    }
    if (row.Type == IF_TYPE_ETHERNET_CSMACD) {
        return "ethernet";
    }
    return "other";
}

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
    static const wchar_t* names[] = {L"Ultra Performance", L"Performance", L"Balanced",
                                     L"Quality", L"Ultra Quality", L"DLAA"};
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
    }
    return fps;
}
std::wstring BackendLabel(AiBackend backend) {
    switch (backend) {
        case AiBackend::DlssFg: return L"DLSS-G";
        case AiBackend::NvidiaVfg: return L"NVIDIA VFG";
        case AiBackend::DlssSr: return L"DLSS SR";
        case AiBackend::Fsr: return L"FSR";
        case AiBackend::FsrFg: return L"FSR FG";
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

NextApp::NextApp(bool engine_mode, unsigned short bridge_port)
    : engine_mode_(engine_mode), bridge_port_(bridge_port) {
    overlay_.SetActionHandler(
        [this](OverlayAction action) { HandleOverlayAction(action); });
    paths_ = AppPaths::Default();
    host_ = std::make_unique<AppHost>(paths_);
}

NextApp::~NextApp() {
    StopNr();
    StopDlssFg();
    if (decode_context_) {
        decode_context_->Release();
        decode_context_ = nullptr;
    }
    if (decode_device_) {
        decode_device_->Release();
        decode_device_ = nullptr;
    }
}

bool NextApp::CreateDecodeDevice() {
    // One device for d3d11va, the video processor and the stream presenter.
    // VIDEO_SUPPORT lets FFmpeg create its decoder and video-processor
    // interfaces on this device; multithread protection lets the decode thread
    // and the presentation thread share the immediate context.
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
#if defined(_DEBUG)
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL got{};
    HRESULT hr = ::D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels,
        static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, &decode_device_, &got,
        &decode_context_);
    if (FAILED(hr)) {
        LogPrintf(LogLevel::Warning, "[decode] shared device unavailable: 0x%08lX",
                  static_cast<unsigned long>(hr));
        decode_device_ = nullptr;
        decode_context_ = nullptr;
        return false;
    }
    Microsoft::WRL::ComPtr<ID3D11Multithread> multithread;
    if (SUCCEEDED(decode_device_->QueryInterface(IID_PPV_ARGS(&multithread)))) {
        multithread->SetMultithreadProtected(TRUE);
    }
    return true;
}

bool NextApp::NeedsCpuPixels() const {
    // Every AI stage consumes tightly packed RGBA in system memory: DLSS SR,
    // NR and DLSS-G in the worker.
    return settings_.ai.super_resolution || settings_.ai.neural_render ||
           settings_.ai.frame_generation;
}

void NextApp::ApplyReflex() {
    const int mode = std::clamp(settings_.video.reflex, 0, 2);
    if (mode <= 0) {
        if (reflex_ && reflex_applied_ > 0 && reflex_device_) {
            std::string detail;
            reflex_->SetMode(static_cast<ID3D11Device*>(reflex_device_), 0, detail);
        }
        reflex_applied_ = -1;
        reflex_device_ = nullptr;
        return;
    }
    if (reflex_failed_ || !stream_presenter_) {
        return;
    }
    ID3D11Device* device = stream_presenter_->device();
    if (!device) {
        return;
    }
    if (!reflex_) {
        reflex_ = std::make_unique<brp::media::NvReflex>();
    }
    if (!reflex_->available()) {
        std::string detail;
        if (!reflex_->Load(detail)) {
            reflex_failed_ = true;
            Toast(L"Reflex: " + Widen(detail), true);
            return;
        }
    }
    if (reflex_applied_ == mode && reflex_device_ == device) {
        return;
    }
    std::string detail;
    if (!reflex_->EnableMarkers(device, mode == 2, detail)) {
        reflex_failed_ = true;
        Toast(L"Reflex: " + Widen(detail), true);
        return;
    }
    reflex_applied_ = mode;
    reflex_device_ = device;
}

int NextApp::Run() {
    if (!Init()) {
        return 1;
    }
    // The presentation thread must not lose its vblank slot to background work.
    // Above-normal is enough to beat ordinary helpers without starving audio,
    // decode or the OS. The reference app does the same.
    ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    ::timeBeginPeriod(1);
    g_hook_app = this;
    keyboard_hook_ = ::SetWindowsHookExW(WH_KEYBOARD_LL, &NextApp::LowLevelKeyboardProc,
                                              ::GetModuleHandleW(nullptr), 0);
    if (engine_mode_) {
        // Headless shell: the Rust/GPUI window is the only UI. Start the
        // bridge and keep the legacy window hidden but alive for its message
        // pump, presenters and the stream surface.
        bridge_.Start(bridge_port_);
        ::ShowWindow(window_->hwnd(), SW_HIDE);
    } else {
        window_->Show();
    }
    if (settings_.general.check_updates) {
        const std::string repo = settings_.general.update_repo.empty()
                                     ? std::string("bulinatu/Veyla")
                                     : settings_.general.update_repo;
        update_.Check(repo, settings_.general.update_channel == 1, AppVersion());
    }
    while (!quit_ && window_->Pump()) {
        if (!watching_ && window_->ConsumeResize()) {
            Resize();
        }
        if (stream_window_ && stream_window_->ConsumeResize()) {
            ResizeStream();
        }
        now_ = SecondsNow();
        const auto wait_started = std::chrono::steady_clock::now();
        if (watching_) {
            if (fg_covering_) {
                // The generator owns presentation. Wake on a decoded frame
                // instead of polling: the wait returns immediately when the
                // decoder queues one, and the timeout only keeps the overlay
                // and window fit alive when the source is idle.
                if (streaming_) {
                    // WaitForFrame returns at once while frames are queued, so
                    // without a pause the loop spins until the next slot is due.
                    // The timer is at 1 ms (timeBeginPeriod), so this adds at most
                    // about one millisecond of pickup jitter.
                    streaming_->WaitForFrame(4);
                    // Buffer 0 pulls on every tick and consumes what arrived;
                    // the pause only exists for the phase-gated modes, where
                    // the loop would otherwise spin against the gate.
                    if (settings_.streaming.frame_buffer > 0) {
                        std::this_thread::sleep_for(std::chrono::microseconds(500));
                    }
                } else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(4));
                }
            } else if (stream_dx12_ && !fg_pending_.empty()) {
                // Native DLSS-G has a generated frame due between inputs.
                // Wake at ~1 ms so the generated cadence is not quantised to
                // the decoded-frame arrival.
                if (streaming_) {
                    streaming_->WaitForFrame(1);
                }
                std::this_thread::sleep_for(std::chrono::microseconds(500));
            } else if (streaming_ && !settings_.video.vsync) {
                // Lowest-latency path: wake on the decoded frame itself and
                // present it right away. Waiting for the swapchain's
                // frame-latency object first made the pull happen on the vblank
                // grid, so a frame that arrived mid-interval waited up to a
                // full refresh before it was even picked up. Present() still
                // throttles when the flip queue is full, and Render() only
                // presents when something was actually drawn.
                streaming_->WaitForFrame(2);
            } else if (stream_dx12_ && stream_dx12_->WaitForFrame(
                           static_cast<DWORD>(std::clamp(2000 / RefreshHz(), 8, 40)))) {
                // vblank-aligned cadence on the DX12 surface
            } else if (ActivePresenter() && ActivePresenter()->frame_waitable()) {
                // One iteration per vblank. A short timeout (the previous 8 ms)
                // made the loop run twice per refresh: the second pass presented
                // a backbuffer that had not been drawn, so every other vblank
                // showed a frame from two frames ago. That is exactly how a
                // perfectly decoded stream looks like it is skipping.
                const int hz = RefreshHz();
                const DWORD timeout =
                    static_cast<DWORD>(std::clamp(2000 / hz, 8, 40));
                ::WaitForSingleObject(ActivePresenter()->frame_waitable(), timeout);
            } else {
                // No waitable swapchain (old driver): Present(1) blocks on the
                // vblank, so a short sleep is only there to avoid a spin.
                std::this_thread::sleep_for(std::chrono::milliseconds(4));
            }
        }
        const auto frame_started = std::chrono::steady_clock::now();
        probe_wait_ms_ = std::max(
            probe_wait_ms_,
            std::chrono::duration<double, std::milli>(frame_started - wait_started).count());
        Frame();
        probe_frame_ms_ = std::max(
            probe_frame_ms_,
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - frame_started)
                .count());
        if (!watching_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
    }
    ::timeEndPeriod(1);
    bridge_.Stop();
    if (keyboard_hook_) {
        ::UnhookWindowsHookEx(keyboard_hook_);
        keyboard_hook_ = nullptr;
    }
    g_hook_app = nullptr;
    StopPadThread();
    StopNr();
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

LRESULT CALLBACK NextApp::LowLevelKeyboardProc(int code, WPARAM wparam, LPARAM lparam) {
    if (code == HC_ACTION && (wparam == WM_KEYDOWN || wparam == WM_SYSKEYDOWN)) {
        const auto* info = reinterpret_cast<KBDLLHOOKSTRUCT*>(lparam);
        if (info && g_hook_app && g_hook_app->engine_mode_ &&
            g_hook_app->watching_ && info->vkCode == VK_F10) {
            // The DLSS-G presenter can hold the foreground instead of the
            // stream window; route F10 from either to the GPUI shell.
            const HWND foreground = ::GetForegroundWindow();
            DWORD foreground_pid = 0;
            if (foreground) {
                ::GetWindowThreadProcessId(foreground, &foreground_pid);
            }
            if (foreground_pid == ::GetCurrentProcessId() ||
                (g_hook_app->dlss_fg_ &&
                 g_hook_app->dlss_fg_->worker_pid() == foreground_pid)) {
                ::AllowSetForegroundWindow(ASFW_ANY);
                g_hook_app->bridge_.Publish("E overlay-toggle");
                return 1;
            }
        }
        if (info && g_hook_app && g_hook_app->overlay_pause_fg_ &&
            (info->vkCode == VK_ESCAPE || info->vkCode == VK_F10)) {
            // The GPUI control center is open but may not own the foreground
            // (the DLSS-G presenter is topmost). Route Esc/F10 to it and keep
            // the key away from the stream, so Esc never arms by accident.
            const HWND foreground = ::GetForegroundWindow();
            DWORD foreground_pid = 0;
            if (foreground) {
                ::GetWindowThreadProcessId(foreground, &foreground_pid);
            }
            if (foreground_pid == ::GetCurrentProcessId() ||
                (g_hook_app->dlss_fg_ &&
                 g_hook_app->dlss_fg_->worker_pid() == foreground_pid)) {
                g_hook_app->bridge_.Publish("E overlay-toggle");
                return 1;
            }
        }
        if (info && info->vkCode == VK_ESCAPE && g_hook_app &&
            g_hook_app->ShouldCaptureEsc()) {
            if (brp::Win32Window* window = g_hook_app->ActiveWindow()) {
                window->InjectKey(VK_ESCAPE);
            }
            return 1;
        }
    }
    return ::CallNextHookEx(nullptr, code, wparam, lparam);
}

bool NextApp::ShouldCaptureEsc() const {
    // Only steal Esc while the user is in, or arming, a session, and only when
    // the foreground window belongs to this process or to the DLSS-G worker.
    // Alt-Tabbing to another application keeps Esc there.
    if (overlay_pause_fg_) {
        // The GPUI control center owns the keyboard; Esc belongs to it.
        return false;
    }
    if (!watching_ && !disconnect_armed_) {
        return false;
    }
    const HWND foreground = ::GetForegroundWindow();
    if (!foreground) {
        return false;
    }
    DWORD pid = 0;
    ::GetWindowThreadProcessId(foreground, &pid);
    if (pid == ::GetCurrentProcessId()) {
        return true;
    }
    if (dlss_fg_ && dlss_fg_->worker_pid() == pid) {
        return true;
    }
    return false;
}
bool NextApp::Init() {
    window_ = std::make_unique<brp::Win32Window>();
    if (!window_->Create(L"Veyla", 1600, 900)) {
        return false;
    }
    presenter_ = std::make_unique<brp::Presenter>();
    CreateDecodeDevice();
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
                    L"Veyla", MB_OK | MB_ICONERROR);
        return false;
    }
    settings_ = host_->Settings();
    ApplyEnhance();
    if (auto* logger = dynamic_cast<Logger*>(host_->Log())) {
        logger->SetLevel(settings_.advanced.log_level);
    }
    system_ = ProbeSystem();
    if (AppVersion().find('-') == std::string::npos) {
        // A stable build reached the desktop: the beta rollback snapshot is no
        // longer needed.
        std::error_code backup_ec;
        std::filesystem::remove_all(
            std::filesystem::path(system_.exe_dir) / "_update_backup", backup_ec);
    }

    streaming_ = std::make_shared<StreamingService>(paths_.session_file,
                                                    paths_.legacy_settings);
    streaming_->Load();
    if (decode_device_ && decode_context_) {
        streaming_->SetExternalDevice(decode_device_, decode_context_);
        streaming_->SetCpuPixels(NeedsCpuPixels());
    }
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
    pad_rate_hz_.store(std::clamp(settings_.controller.poll_hz, 60, 1000));
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
    LogPrintf(LogLevel::Debug, "[next] window %dx%d scale %.3f logical %.1fx%.1f",
              width, height, scale_, logical_width_, logical_height_);
}

void NextApp::ResizeStream() {
    if (!watching_ || !stream_window_) {
        return;
    }
    const int width = stream_window_->width();
    const int height = stream_window_->height();
    if (width <= 0 || height <= 0) {
        return;
    }
    if (stream_dx12_) {
        stream_dx12_->Resize(width, height);
        redraw_needed_ = true;
        LogPrintf(LogLevel::Debug, "[next] stream window %dx%d (DX12)", width, height);
        return;
    }
    if (!stream_presenter_) {
        return;
    }
    stream_presenter_->Resize(width, height);
    ApplyTargetTo(stream_presenter_.get());
    ApplyEnhance();
    redraw_needed_ = true;
    LogPrintf(LogLevel::Debug, "[next] stream window %dx%d", width, height);
}

brp::Win32Window* NextApp::ActiveWindow() const {
    if (watching_ && stream_window_) {
        return stream_window_.get();
    }
    return window_.get();
}

brp::Presenter* NextApp::ActivePresenter() const {
    if (watching_) {
        return stream_presenter_.get();
    }
    return presenter_.get();
}

void NextApp::ApplyTargetTo(brp::Presenter* presenter) const {
    if (!presenter) {
        return;
    }
    const int target = settings_.video.render_target;
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
    presenter->SetUpscaleTarget(width, height);
}

bool NextApp::PresentVsync() const {
    // Buffer 0 is the explicit lowest-latency mode: tearing beats waiting for
    // vblank. Vsync returns as soon as the user picks a real buffer.
    return settings_.video.vsync &&
           std::clamp(settings_.streaming.frame_buffer, 0, 3) > 0;
}

void NextApp::ApplyEnhance() {
    EnhanceParams enhance;
    enhance.active = settings_.video.sharpening > 0;
    enhance.sharpen = std::clamp(settings_.video.sharpening / 100.0f, 0.0f, 1.0f);
    if (presenter_) {
        presenter_->SetEnhance(enhance);
    }
    if (stream_presenter_) {
        stream_presenter_->SetEnhance(enhance);
    }
}

bool NextApp::EnsureStreamSurface() {
    if (!window_) {
        return false;
    }
    if (!stream_window_) {
        stream_window_ = std::make_unique<brp::Win32Window>();
        if (!stream_window_->Create(L"Veyla Stream", 1600, 900)) {
            stream_window_.reset();
            Toast(L"Cannot create the stream window", true);
            return false;
        }
        stream_window_->SetQuitOnDestroy(false);
        stream_window_->SetCloseHandler([this]() {
            if (watching_) {
                ToggleWatch(false);
            }
        });
        stream_window_->SetFrameMetrics(0, 0);
    }
    if (!stream_dx12_ && !stream_presenter_) {
        // Watch runs on the pure DX12 surface, including native DLSS-G.
        const bool want_dx12 = true;
        if (want_dx12) {
            auto dx12 = std::make_unique<brp::Dx12Video>();
            std::string detail;
            if (dx12->Init(stream_window_->hwnd(), stream_window_->width(),
                           stream_window_->height(), decode_device_, detail)) {
                stream_dx12_ = std::move(dx12);
                redraw_needed_ = true;
            } else {
                LogPrintf(LogLevel::Warning, "[next] DX12 surface unavailable: %s",
                          detail.c_str());
            }
        }
        if (!stream_dx12_) {
            auto presenter = std::make_unique<brp::Presenter>();
            brp::PresenterConfig config;
            config.output_width = stream_window_->width();
            config.output_height = stream_window_->height();
            config.source_width = 1920;
            config.source_height = 1080;
            config.vsync = PresentVsync();
            config.allow_tearing = !config.vsync;
            config.hdr = false;
            const bool shared = decode_device_ && decode_context_;
            const bool ok =
                shared ? presenter->InitWithDevice(stream_window_->hwnd(), config,
                                                   decode_device_, decode_context_)
                       : presenter->Init(stream_window_->hwnd(), config);
            if (!ok) {
                Toast(L"Cannot create the stream presenter", true);
                return false;
            }
            presenter->SetUpscaleMode(2);
            stream_presenter_ = std::move(presenter);
            ApplyTargetTo(stream_presenter_.get());
            ApplyEnhance();
            ApplyReflex();
        }
    }

    // The stream opens on the display chosen in Video > Display (index clamped
    // to the enumerated list), not on whatever monitor the shell happens to be.
    RECT target{};
    bool have_target = false;
    if (settings_.display.monitor >= 0 &&
        settings_.display.monitor < static_cast<int>(system_.displays.size())) {
        target = system_.displays[static_cast<std::size_t>(settings_.display.monitor)].bounds;
        have_target = target.right > target.left && target.bottom > target.top;
    }
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    if (!have_target &&
        !::GetMonitorInfoW(::MonitorFromWindow(window_->hwnd(), MONITOR_DEFAULTTONEAREST),
                           &monitor)) {
        return false;
    }
    if (!have_target) {
        target = monitor.rcMonitor;
    }
    if (settings_.display.fullscreen_on_play) {
        const int width = target.right - target.left;
        const int height = target.bottom - target.top;
        ::SetWindowPos(stream_window_->hwnd(), nullptr, target.left,
                       target.top, width, height,
                       SWP_NOZORDER | SWP_NOACTIVATE);
        if (!stream_window_->fullscreen()) {
            stream_window_->EnterFullscreen();
        }
        watch_fullscreen_ = true;
    } else {
        if (stream_window_->fullscreen()) {
            stream_window_->ExitFullscreen();
        }
        MONITORINFO work{};
        work.cbSize = sizeof(work);
        ::GetMonitorInfoW(::MonitorFromRect(&target, MONITOR_DEFAULTTONEAREST), &work);
        const int work_w = work.rcWork.right - work.rcWork.left;
        const int work_h = work.rcWork.bottom - work.rcWork.top;
        const int width = std::min(1600, work_w);
        const int height = std::min(900, work_h);
        const int x = work.rcWork.left + (work_w - width) / 2;
        const int y = work.rcWork.top + (work_h - height) / 2;
        ::SetWindowPos(stream_window_->hwnd(), HWND_TOP, x, y, width, height,
                       SWP_SHOWWINDOW);
        stream_window_->Show();
        ::SetForegroundWindow(stream_window_->hwnd());
        watch_fullscreen_ = false;
    }
    stream_window_->SetCursorHidden(true);
    ResizeStream();
    ::SetForegroundWindow(stream_window_->hwnd());
    return true;
}

void NextApp::Frame() {
    if (engine_mode_) {
        std::string command;
        while (bridge_.TakeCommand(command)) {
            HandleBridgeCommand(command);
        }
        if (bridge_status_at_ < 0.0 || now_ - bridge_status_at_ >= 0.25) {
            PublishBridgeStatus();
            bridge_status_at_ = now_;
        }
        if (publish_devices_) {
            PublishDevices();
            publish_devices_ = false;
        }
        if (publish_profiles_) {
            PublishProfiles();
            publish_profiles_ = false;
        }
    }
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
    if (presenter_ && (presenter_->device_lost() ||
                       (stream_presenter_ && stream_presenter_->device_lost()) ||
                       (stream_dx12_ && stream_dx12_->device_lost()))) {
        watching_ = false;
        StopDlssFg();
        StopNr();
        streaming_->Disconnect();
        Toast(L"GPU device lost; restart the app", true);
    }
    HandlePinInput();
    const bool app_foreground = ::GetForegroundWindow() == ActiveWindow()->hwnd();
    const bool show_overlay =
        watching_ && app_foreground &&
        (settings_.overlay.enabled || (!engine_mode_ && overlay_control_) ||
         disconnect_armed_);
    if (std::getenv("BRP_OVERLAY_PROBE")) {
        std::fprintf(stderr, "[next] overlay show=%d watching=%d fg=%d active=%p foreground=%p\n",
                     show_overlay ? 1 : 0, watching_ ? 1 : 0, app_foreground ? 1 : 0,
                     reinterpret_cast<void*>(ActiveWindow()->hwnd()),
                     reinterpret_cast<void*>(::GetForegroundWindow()));
    }
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
    }
    if (stats_at_ < 0.0 || now_ - stats_at_ >= 1.0) {
        const int shown = display_shown_.load();
        display_rate_ = shown - display_shown_last_;
        display_shown_last_ = shown;
        cadence_p50_ = video_intervals_.Percentile(0.50);
        cadence_p99_ = video_intervals_.Percentile(0.99);
        loop_p99_ = loop_intervals_.Percentile(0.99);
        const double span = stats_at_ >= 0.0 ? std::max(0.001, now_ - stats_at_) : 1.0;
        stats_at_ = now_;
        if (presenter_) {
            brp::Presenter* stats_presenter = ActivePresenter();
            const unsigned long long misses =
                stream_dx12_ ? stream_dx12_->present_misses()
                             : (stats_presenter ? stats_presenter->present_misses() : 0);
            present_misses_rate_ =
                misses >= present_misses_seen_ ? misses - present_misses_seen_ : 0;
            present_misses_seen_ = misses;
            const unsigned refresh =
                stream_dx12_ ? stream_dx12_->present_refresh_count()
                             : (stats_presenter ? stats_presenter->present_refresh_count() : 0);
            const unsigned refreshes = refresh >= present_refresh_seen_
                                           ? refresh - present_refresh_seen_
                                           : 0;
            present_refresh_seen_ = refresh;
            // Measured scanout rate, not the value advertised by the monitor
            // (which can be 240 Hz while a game mode runs at 60).
            present_refresh_hz_ = static_cast<double>(refreshes) / span;
            if (std::getenv("BRP_NEXT_FRAME_PROBE")) {
                const StreamSnapshot probe =
                    streaming_ ? streaming_->Snapshot() : StreamSnapshot{};
                unsigned h7 = 0, h8 = 0, h9 = 0, h10 = 0, h16 = 0;
                if (hold_counts_[7] >= hold_seen_[7]) h7 = hold_counts_[7] - hold_seen_[7];
                if (hold_counts_[8] >= hold_seen_[8]) h8 = hold_counts_[8] - hold_seen_[8];
                if (hold_counts_[9] >= hold_seen_[9]) h9 = hold_counts_[9] - hold_seen_[9];
                if (hold_counts_[10] >= hold_seen_[10]) h10 = hold_counts_[10] - hold_seen_[10];
                if (hold_counts_[16] >= hold_seen_[16]) h16 = hold_counts_[16] - hold_seen_[16];
                hold_seen_ = hold_counts_;
                std::fprintf(stderr,
                             "[next] loop p50 %.2f p99 %.2f max %.2f  |  cadence p50 %.1f "
                             "p99 %.1f  |  scanout %.1f Hz  |  present misses %llu  |  "
                             "present q %d lag %.1f ms  |  "
                             "shown %d  |  content %.0f fps  |  holds 7:%u 8:%u 9:%u 10:%u 16:%u  |  sr submit %.2f ms  |  "
                             "sr cpu %.2f ms  |  dec %.2f/%.2f ms  |  "
                             "net jitter %.2f ms loss %.2f%% drops %d  |  queue %d  trim %+.3f%%  |  "
                             "wait max %.1f ms  frame max %.1f ms  |  age avg %.1f max %.1f ms  |  "
                             "input %.1f/%.1f ms  |  "
                             "nr %.1f ms  |  fg %.1f ms %dx drops %llu out %.0f fps\n",
                             loop_intervals_.Percentile(0.50), loop_p99_,
                             loop_intervals_.Max(), cadence_p50_, cadence_p99_,
                             present_refresh_hz_, present_misses_rate_,
                             stream_dx12_ ? stream_dx12_->present_queue()
                                          : (stats_presenter ? stats_presenter->present_queue() : 0),
                             stream_dx12_ ? stream_dx12_->present_lag_ms()
                                          : (stats_presenter ? stats_presenter->present_lag_ms() : 0.0),                             display_rate_,
                             content_fps_, h7, h8, h9, h10, h16,
                             dlss_sr_ ? dlss_sr_->last_ms() : 0.0, sr_cpu_ms_,
                             probe.decode_ms, probe.decode_ms_max,
                             probe.jitter_ms, probe.loss_percent, probe.decoder_drops,
                             playout_depth_, playout_trim_ * 100.0, probe_wait_ms_,
                             probe_frame_ms_, probe_age_n_ > 0 ? probe_age_sum_ / probe_age_n_ : 0.0,
                             probe_age_max_, nr_ms_,
                             streaming_ ? streaming_->InputResponseMs() : 0.0,
                             streaming_ ? streaming_->InputResponseAvgMs() : 0.0,
                             dlss_fg_ ? dlss_fg_->process_ms() : 0.0,
                             dlss_fg_ ? dlss_fg_->actual_multiplier() : 0,
                             dlss_fg_ ? dlss_fg_->dropped() : 0,
                             dlss_fg_ ? dlss_fg_->displayed_fps() : 0.0);
                probe_wait_ms_ = 0.0;
                probe_frame_ms_ = 0.0;
                probe_age_sum_ = 0.0;
                probe_age_max_ = 0.0;
                probe_age_n_ = 0;
            }
        }
    }
    if (!engine_mode_ || watching_) {
        Render();
    }
    const double delta = std::max(0.0, now_ - last_frame_);
    last_frame_ = now_;
    if (delta > 0.0 && delta < 1.0) {
        loop_intervals_.Add(delta * 1000.0);
    }
    if (auto* diagnostics = dynamic_cast<DiagnosticsCollector*>(host_->Diagnostics())) {
        diagnostics->AddFrame(delta * 1000.0, 0.0, 0.0, 0.0, 0.0);
        diagnostics->Tick(delta);
    }
    ++ui_frames_;
}

void NextApp::Render() {
    present_frame_ = false;
    const ThemeTokens& theme = ThemeFor(settings_.general.theme);
    if (watching_) {
        if (stream_dx12_) {
            PresentVideo();
            if (!present_frame_) {
                TickDlssFgDx12();
            }
            if (!present_frame_) {
                return;
            }
            stream_dx12_->Present(PresentVsync());
            ActiveWindow()->set_hand_cursor(false);
            return;
        }
        brp::Presenter* video = ActivePresenter();
        const bool reflex_active =
            video && !fg_covering_ && reflex_ && reflex_->markers_enabled();
        ID3D11Device* reflex_device = reflex_active ? video->device() : nullptr;
        if (reflex_active) {
            // Markers only: NvAPI_D3D_Sleep is deliberately not called. The
            // legacy measurement showed the driver's sleep stage fights the
            // waitable swapchain (225 Hz, 56+ missed refreshes/s).
            reflex_->BeginFrame(reflex_device);
            reflex_->EndSimulation(reflex_device);
            reflex_->RenderSubmitStart(reflex_device);
        }
        PresentVideo();
        if (reflex_active) {
            reflex_->RenderSubmitEnd(reflex_device);
        }
        if (fg_covering_) {
            // The worker's window is the only visible surface. Presenting the
            // stream window here would cost a full-size DWM composition per
            // vblank.
            ActiveWindow()->set_hand_cursor(false);
            return;
        }
        if (!present_frame_) {
            // Nothing was drawn this tick (a repeated frame or an idle
            // decoder). Leave the last presented image on screen: presenting an
            // untouched backbuffer would flip in a stale frame.
            return;
        }
        // Video-only surface: no Direct2D pass, no UI composition.
        if (reflex_active) {
            reflex_->PresentStart(reflex_device);
        }
        video->PresentNow(PresentVsync());
        if (reflex_active) {
            reflex_->PresentEnd(reflex_device);
        }
        if (content_changed_) {
            content_changed_ = false;
            const unsigned refresh = ActivePresenter()->present_refresh_count();
            if (last_source_refresh_ != 0 && refresh > last_source_refresh_) {
                const unsigned delta =
                    std::clamp(refresh - last_source_refresh_, 1u, 31u);
                ++hold_counts_[delta];
                ++hold_total_;
            }
            last_source_refresh_ = refresh;
        }
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

    if (disconnect_armed_ && !watching_) {
        const float bw = 680.0f;
        const float bh = 84.0f;
        const D2D1_RECT_F rect = Rect((logical_width_ - bw) * 0.5f, 54,
                                      (logical_width_ + bw) * 0.5f, 54 + bh);
        const Color color = theme.warning;
        renderer_->FillRounded(rect, 14.0f, ToD2D(theme.surface_alt));
        renderer_->StrokeRounded(rect, 14.0f, ToD2D(color), 1.4f);
        renderer_->Text(L"DISCONNECT ARMED", Rect(rect.left + 20, 64, rect.right - 20, 88),
                        13.0f, ToD2D(color), DWRITE_FONT_WEIGHT_BOLD);
        renderer_->Text(L"Press Esc again at any time, or click Disconnect.",
                        Rect(rect.left + 20, 88, rect.right - 20, 108), 11.0f,
                        ToD2D(theme.text_dim));
        if (widgets.Button(7791, Rect(rect.right - 300, 104, rect.right - 170, 130),
                           L"Disconnect")) {
            disconnect_armed_ = false;
            DisconnectStream();
        }
        if (widgets.Button(7792, Rect(rect.right - 160, 104, rect.right - 20, 130),
                           L"Cancel", false)) {
            disconnect_armed_ = false;
        }
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
    renderer_->Text(L"Veyla", Rect(46, 2, 220, 21), 12.5f,
                    ToD2D(theme.text), DWRITE_FONT_WEIGHT_SEMI_BOLD);
    renderer_->Text(L"NEXT  ·  2.7.3", Rect(46, 19, 220, 35), 9.0f,
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
                      settings_.ai.frame_generation &&
                              settings_.ai.fg_backend == AiBackend::DlssFg &&
                              !settings_.ai.SrWantsFsr(system_.gpu.nvidia)
                          ? L"DLSS-G"
                          : L"native");
        renderer_->Text(line, Rect(18, card_top + 54, width - 20, card_top + 74),
                        9.5f, ToD2D(theme.text_faint), DWRITE_FONT_WEIGHT_MEDIUM);
        renderer_->Text(L"F10  control center", Rect(18, card_top + 72, width - 20,
                                                    card_top + 90),
                        9.0f, ToD2D(theme.accent), DWRITE_FONT_WEIGHT_MEDIUM);

        renderer_->Text(L"Veyla 2.7.3",
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

int NextApp::RefreshHz() const {
    if (!system_.displays.empty() && system_.displays.front().refresh_hz > 0) {
        return std::clamp(system_.displays.front().refresh_hz, 24, 240);
    }
    if (settings_.display.refresh == FrameRate::F90) {
        return 90;
    }
    if (settings_.display.refresh == FrameRate::F120) {
        return 120;
    }
    if (settings_.display.refresh == FrameRate::F144) {
        return 144;
    }
    if (settings_.display.refresh == FrameRate::F165) {
        return 165;
    }
    if (settings_.display.refresh == FrameRate::F240) {
        return 240;
    }
    return 60;
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
    pipeline += fg_on ? (L"DLSS-G " + std::to_wstring(settings_.ai.fg_multiplier) + L"x")
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
        {L"DECODE", snapshot.connected && !snapshot.decoder.empty()
                            ? Widen(snapshot.decoder)
                            : L"idle",
         std::to_wstring(snapshot.source_width) + L"x" +
             std::to_wstring(snapshot.source_height),
         snapshot.connected},
        {L"SUPER RESOLUTION", sr_on ? (dlaa ? L"DLAA" : Widen(std::string(ToString(settings_.ai.sr_output))))
                                    : L"Off",
         sr_on ? L"NGX on the decoded frame" : L"presenter upscale", sr_on},
        {L"FRAME GENERATION",
         fg_on ? (L"DLSS-G " + std::to_wstring(settings_.ai.fg_multiplier) + L"x") : L"Off",
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
    const int cap_count = static_cast<int>(sizeof(caps) / sizeof(caps[0]));
    const float cap_w = (w - 32.0f - cap_gap * (cap_count - 1)) / cap_count;
    const float cap_y = ai_y + 38.0f;
    for (int i = 0; i < cap_count; ++i) {
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
    renderer_->Text(L"Capabilities are file checks. DLSS SR and DLSS-G run in Watch.",
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
    available += system_.nr_worker ? 1 : 0;
    PageHeader(widgets, L"PROCESSING", L"AI Video Engine",
               L"Super resolution, frame generation and inference runtimes",
               available >= 2 ? L"READY" : L"LIMITED", available >= 2);

    const float gap = 10.0f;
    const float card_w = (w - gap * 2.0f) / 3.0f;
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
    capability_card(2, L"Neural Rendering", system_.nr_worker ? L"AVAILABLE" : L"MISSING",
                    system_.nr_worker, L"ngx + DLSS NR");

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
        p.ai.sr_model = "dlss";  // the pair: DLSS-G only ever runs with DLSS SR
        p.ai.frame_generation = true;
        p.ai.fg_backend = AiBackend::DlssFg;
        p.ai.fg_multiplier = 2;
        presets.push_back({L"1440p DLSS-G", L"DLSS-G 2x at 1440p", p});
    }
    {
        AppSettings p = base;
        p.video.output = Resolution::Native;
        p.video.render_target = 0;
        p.ai.super_resolution = false;
        p.ai.sr_model = "dlss";  // the pair: DLSS-G only ever runs with DLSS SR
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
        renderer_->Text((std::wstring(L"VRAM: ") +
                         std::to_wstring(system_.gpu.vram_mb / 1024) + L" GB" +
                         L"  |  DLSS: " +
                         (system_.dlss_sr ? L"available" : L"missing"))
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
    widgets.Card(Rect(x, y, x + w, y + 196));
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

    wchar_t scanout[240] = {};
    if (stream.connected && watching_ && !fg_covering_) {
        unsigned best_v = 0;
        unsigned best_n = 0;
        for (unsigned i = 1; i < hold_counts_.size(); ++i) {
            if (hold_counts_[i] > best_n) {
                best_n = hold_counts_[i];
                best_v = i;
            }
        }
        const double best_pct =
            hold_total_ > 0 ? 100.0 * static_cast<double>(best_n) / hold_total_ : 0.0;
        std::swprintf(scanout, 240,
                      L"Scanout: %.1f Hz measured  |  present misses %llu/s  |  "
                      L"holds %uv %.0f%%  |  loop p99 %.2f ms  |  target %.1f Hz",
                      present_refresh_hz_, present_misses_rate_, best_v, best_pct,
                      loop_p99_, static_cast<double>(RefreshHz()));
    } else if (stream.connected && fg_covering_) {
        if (fg_fps_ > 0.5) {
            std::swprintf(scanout, 240,
                          L"Scanout: generator window  |  %.1f fps shown  |  source %.1f fps  |  "
                          L"target %.1f Hz",
                          fg_fps_, stream.source_fps, static_cast<double>(RefreshHz()));
        } else {
            std::swprintf(scanout, 240,
                          L"Scanout: generator window  |  DLSS-G %dx  |  source %.1f fps  |  "
                          L"target %.1f Hz",
                          settings_.ai.fg_multiplier, stream.source_fps,
                          static_cast<double>(RefreshHz()));
        }
    } else if (stream.connected) {
        std::swprintf(scanout, 240,
                      L"Scanout: %.1f Hz desktop  |  not in Watch  |  target %.1f Hz",
                      present_refresh_hz_, static_cast<double>(RefreshHz()));
    } else {
        std::swprintf(scanout, 240,
                      L"Scanout: %.1f Hz desktop  |  loop p99 %.2f ms  |  target %.1f Hz",
                      present_refresh_hz_, loop_p99_, static_cast<double>(RefreshHz()));
    }
    renderer_->Text(scanout, Rect(x + 16, y + 152, x + w - 16, y + 176), 10.5f,
                    ToD2D(theme.text_faint));

    y += 212.0f;
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
    PageHeader(widgets, L"APPLICATION", L"Settings", L"Central configuration, validated and saved on every change");

    static const wchar_t* categories[] = {
        L"General", L"Audio", L"Controller", L"Advanced"};
    const float list_w = 238.0f;
    float y = 138.0f;
    for (int i = 0; i < static_cast<int>(std::size(categories)); ++i) {
        const D2D1_RECT_F row = Rect(x, y, x + list_w, y + 38);
        if (widgets.NavItem(9400 + i, row, categories[i], settings_category_ == i, 14)) {
            settings_category_ = i;
        }
        y += 40.0f;
    }
    renderer_->Text(L"Streaming, AI, Display and Overlay",
                    Rect(x, y + 10, x + list_w, y + 28), 10.0f,
                    ToD2D(theme.text_faint));
    renderer_->Text(L"now keep their own pages. This page",
                    Rect(x, y + 28, x + list_w, y + 46), 10.0f,
                    ToD2D(theme.text_faint));
    renderer_->Text(L"holds only the extras.",
                    Rect(x, y + 46, x + list_w, y + 64), 10.0f,
                    ToD2D(theme.text_faint));

    const float px = x + list_w + 24.0f;
    const float pw = x + w - px;
    widgets.Card(Rect(px, 138, px + pw, logical_height_ - 40));
    SettingsControls controls(widgets, settings_, system_, true);
    controls.Reset();
    float cy = 156.0f;
    switch (settings_category_) {
        case 0: cy = controls.General(px + 18, cy, pw - 36); break;
        case 1: cy = controls.Audio(px + 18, cy, pw - 36); break;
        case 2: cy = controls.Controller(px + 18, cy, pw - 36); break;
        case 3: cy = controls.Advanced(px + 18, cy, pw - 36); break;
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
               L"Veyla 2.7.3  |  stable release", L"STABLE", true);

    widgets.Card(Rect(x, 138, x + w, 238));
    renderer_->FillRounded(Rect(x + 18, 154, x + 66, 202), 12.0f,
                           ToD2D(theme.accent));
    renderer_->Text(L"B", Rect(x + 18, 154, x + 66, 202), 24.0f, ToD2D(theme.text),
                    DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
    renderer_->Text(L"Veyla", Rect(x + 80, 152, x + w - 20, 182),
                    20.0f, ToD2D(theme.text), DWRITE_FONT_WEIGHT_SEMI_BOLD);
    renderer_->Text(L"Version 2.7.3  |  win64  |  C++20 / Win32 / Direct2D / D3D11",
                    Rect(x + 80, 182, x + w - 20, 204), 11.5f,
                    ToD2D(theme.text_dim));
    renderer_->Text(L"PS5 Remote Play with native DualSense input, DLSS Super Resolution, "
                    L"DLSS-G through the NVIDIA worker.",
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
    runtime(L"Neural Rendering", system_.nr_worker, L"NGX + DLSS NR worker");

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
                    L"NeuralScreen worker. No RTX VSR. "
                    L"Noto Sans is SIL OFL.",
                    Rect(x, 706, x + w, 726), 10.0f, ToD2D(theme.text_faint));
}
bool NextApp::Apply() {
    // A generator only ever runs next to its own upscaler. Profiles and old
    // config files can still carry a mixed pair; drop the generator here so
    // every path into the engine follows the same rule. "auto" is resolved
    // against the adapter at present time and is checked there.
    if ((settings_.ai.SrIsFsr() && settings_.ai.fg_backend == AiBackend::DlssFg) ||
        (settings_.ai.SrIsDlss() && settings_.ai.fg_backend == AiBackend::FsrFg)) {
        settings_.ai.fg_backend = AiBackend::None;
        settings_.ai.frame_generation = false;
    }
    const AppSettings before = host_->Settings();
    const ApplyScope scope = ClassifySettingsChange(before, settings_);
    // The "previous" values must come from the settings the engine is actually
    // running with (host), not from `settings_`, which the caller has already
    // replaced with the new file. With settings_ on both sides every AI
    // comparison was false and live changes were ignored until a reconnect.
    const AiSettings previous_ai = before.ai;
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
    if (before.general.update_channel != settings_.general.update_channel &&
        settings_.general.check_updates) {
        const std::string repo = settings_.general.update_repo.empty()
                                     ? std::string("bulinatu/Veyla")
                                     : settings_.general.update_repo;
        update_.Check(repo, settings_.general.update_channel == 1, AppVersion());
    }
    if (streaming_) {
        streaming_->SetCpuPixels(NeedsCpuPixels());
    }
    if (before.video.reflex != settings_.video.reflex) {
        reflex_failed_ = false;
    }
    ApplyReflex();
    haptic_strength_.store(settings_.controller.rumble_strength);
    pad_rate_hz_.store(std::clamp(settings_.controller.poll_hz, 60, 1000));
    ApplyPresenterTarget();
    ApplyEnhance();
    const bool surface_change =
        before.video.vsync != settings_.video.vsync ||
        (before.streaming.frame_buffer == 0) !=
            (settings_.streaming.frame_buffer == 0) ||
        before.ai.frame_generation != settings_.ai.frame_generation;
    if (surface_change && watching_ && (stream_presenter_ || stream_dx12_)) {
        // ALLOW_TEARING and the surface type are creation flags. Recreate the
        // surface so a live vsync/AI change takes effect now. The SR context is
        // bound to the old device and must be dropped with it.
        stream_dx12_.reset();
        stream_presenter_.reset();
        if (dlss_fg_dx12_) {
            dlss_fg_dx12_->Stop();
            dlss_fg_dx12_.reset();
        }
        fg_pending_.clear();
        dlss_ready_ = false;
        dlss_failed_ = false;
        if (dlss_sr_) {
            dlss_sr_->Shutdown();
        }
        EnsureStreamSurface();
        redraw_needed_ = true;
    }
    if (auto* logger = dynamic_cast<Logger*>(host_->Log())) {
        logger->SetLevel(settings_.advanced.log_level);
    }
    if (previous_ai.super_resolution != settings_.ai.super_resolution ||
        previous_ai.sr_output != settings_.ai.sr_output ||
        previous_ai.sr_quality != settings_.ai.sr_quality ||
        previous_ai.sr_preset != settings_.ai.sr_preset ||
        previous_ai.fsr_quality != settings_.ai.fsr_quality ||
        previous_ai.fsr_sharpness != settings_.ai.fsr_sharpness ||
        previous_ai.sr_model != settings_.ai.sr_model) {
        dlss_ready_ = false;
        dlss_failed_ = false;
        if (dlss_sr_) {
            dlss_sr_->Shutdown();
        }
        fsr_ready_ = false;
        fsr_failed_ = false;
        if (fsr_) {
            fsr_->Shutdown();
        }
    }
    if (previous_ai.frame_generation != settings_.ai.frame_generation ||
        previous_ai.fg_backend != settings_.ai.fg_backend ||
        previous_ai.fg_multiplier != settings_.ai.fg_multiplier) {
        StopDlssFg();
        dlss_fg_failed_ = false;
        if (dlss_fg_dx12_) {
            dlss_fg_dx12_->Stop();
            dlss_fg_dx12_.reset();
        }
        fg_dx12_failed_ = false;
        fg_dx12_reset_ = true;
        fg_pending_.clear();
    }
    if (previous_ai.neural_render != settings_.ai.neural_render ||
        previous_ai.neural_style != settings_.ai.neural_style ||
        previous_ai.neural_intensity != settings_.ai.neural_intensity ||
        previous_ai.neural_tone != settings_.ai.neural_tone ||
        previous_ai.neural_structure != settings_.ai.neural_structure ||
        previous_ai.neural_skin != settings_.ai.neural_skin ||
        previous_ai.neural_passes != settings_.ai.neural_passes ||
        previous_ai.neural_scale != settings_.ai.neural_scale ||
        previous_ai.neural_unlock != settings_.ai.neural_unlock ||
        previous_ai.neural_ui != settings_.ai.neural_ui ||
        previous_ai.neural_mask != settings_.ai.neural_mask) {
        // The standalone worker is rebuilt on the next frame; the worker that
        // runs NR inside DLSS-G gets the new values live, so a slider move does
        // not restart frame generation.
        StopNr();
        nr_failed_ = false;
        if (dlss_fg_) {
            dlss_fg_->SetNeuralInWorker(NeuralInWorkerWanted());
            dlss_fg_->SetNeuralOptions(NeuralOptionsFrom(settings_.ai));
        }
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

    if (disconnect_armed_ && !overlay_control_) {
        OverlayView banner;
        banner.expanded = false;
        banner.live = true;
        banner.palette = view.palette;
        banner.compact_title = L"DISCONNECT ARMED";
        banner.subtitle = L"Press Esc again to disconnect";
        banner.status = L"F10 cancels";
        OverlayPage page;
        page.title = L"Disconnect";
        OverlayRow again;
        again.label = L"ESC AGAIN";
        again.value = L"DISCONNECT";
        again.enabled = false;
        OverlayRow cancel;
        cancel.label = L"F10";
        cancel.value = L"CANCEL";
        cancel.enabled = false;
        page.rows.push_back(std::move(again));
        page.rows.push_back(std::move(cancel));
        banner.pages.push_back(std::move(page));
        overlay_.Update(banner, settings_.overlay.opacity, settings_.overlay.position,
                        settings_.overlay.side, settings_.overlay.scale / 100.0f,
                        ActiveWindow()->hwnd());
        return;
    }

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
    overview.title = L"Info";
    {
        wchar_t fps_text[96] = {};
        if (content_fps_ > 10.0 && std::abs(content_fps_ - snapshot.source_fps) > 1.0) {
            std::swprintf(fps_text, 96, L"%.0f fps  (%.0f content)", snapshot.source_fps,
                          content_fps_);
        } else {
            std::swprintf(fps_text, 96, L"%.0f fps", snapshot.source_fps);
        }
        wchar_t latency[96] = {};
        std::swprintf(latency, 96, L"age %.1f ms  |  %.1f ms to screen", last_age_ms_,
                      ActivePresenter() ? ActivePresenter()->present_lag_ms() : 0.0);
        overview.rows.push_back(info(L"FRAME", fps_text, latency));
    }
    if (settings_.overlay.show_performance) {
        std::swprintf(buffer, 220, L"%.1f / %.1f ms", cadence_p50_, cadence_p99_);
        overview.rows.push_back(info(L"CADENCE", buffer, L"p50 / p99 source interval"));
    }
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
            fg_on && dlss_fg
                ? std::wstring(L"DLSS-G ") +
                      std::to_wstring(settings_.ai.fg_multiplier) + L"x"
                : L"Off",
            dlss_fg ? (fg_fps_ > 0.5
                           ? std::to_wstring(static_cast<int>(fg_fps_)) + L" shown fps"
                           : std::to_wstring(OutputFps(settings_)) + L" fps requested")
                    : L"source cadence only"));
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
    sr.title = L"SR";
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
    {
        static const int pct[6] = {33, 50, 58, 66, 77, 100};
        const int index = std::clamp(settings_.ai.sr_quality, 0, 5);
        wchar_t scale[64] = {};
        if (sr_on && settings_.ai.sr_quality < 5) {
            std::swprintf(scale, 64, L"%d%%  |  %dx%d -> %dx%d", pct[index],
                          (int)((settings_.ai.sr_output == Resolution::R2160 ? 3840 : 2560) *
                                pct[index] / 100),
                          (int)((settings_.ai.sr_output == Resolution::R2160 ? 2160 : 1440) *
                                pct[index] / 100),
                          settings_.ai.sr_output == Resolution::R2160 ? 3840 : 2560,
                          settings_.ai.sr_output == Resolution::R2160 ? 2160 : 1440);
        } else {
            std::swprintf(scale, 64, L"100%%  |  native AA");
        }
        sr.rows.push_back(info(L"RENDER SCALE", scale));
    }
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
    fg.title = L"FG";
    fg.rows.push_back(toggle(L"Frame generation", fg_on, OverlayAction::ToggleFg,
                             L"generator owns presentation", system_.dlss_fg));
    fg.rows.push_back(action_row(
        L"Backend", dlss_fg ? L"DLSS-G" : L"Off",
        OverlayAction::FgBackendNext,
        dlss_fg ? L"NVIDIA worker presenter" : L"select a generator",
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
                : L"none",
        dlss_fg ? (settings_.ai.fg_multiplier > 2 ? L"up to 6x, modded kernels"
                                                  : L"2x, NVIDIA-signed runtime")
                : L""));
    fg.rows.push_back(info(L"DISPLAYED",
                           std::to_wstring(static_cast<int>(
                               dlss_fg ? (fg_fps_ > 0.5 ? fg_fps_
                                                       : static_cast<double>(OutputFps(settings_)))
                                       : static_cast<double>(display_rate_))) +
                               L" fps",
                           L"measured, not requested"));
    fg.rows.push_back(info(L"NOTE", dlss_fg ? L"flat depth + estimated motion" : L"",
                           L"video has no engine motion vectors"));
    view.pages.push_back(std::move(fg));

    OverlayPage display;
    display.title = L"Video";
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
    display.rows.push_back(stepper(
        L"Sharpening", std::to_wstring(settings_.video.sharpening) + L"%",
        OverlayAction::SharpnessPrev, OverlayAction::SharpnessNext, L"live"));
    display.rows.push_back(action_row(
        L"Overlay side", settings_.overlay.side == 0 ? L"Left" : L"Right",
        OverlayAction::SideNext, L"side-docked panel"));
    display.rows.push_back(action_row(
        L"Vertical", settings_.overlay.position == 0
                        ? L"Top"
                        : (settings_.overlay.position == 1 ? L"Middle" : L"Bottom"),
        OverlayAction::VerticalNext, L"overlay anchor"));
    display.rows.push_back(action_row(
        L"Theme", settings_.general.theme == Theme::Dark ? L"Dark" : L"Light",
        OverlayAction::ToggleTheme, L"applies to the whole application"));
    std::swprintf(buffer, 220, L"%d Hz",
                  system_.displays.empty() ? 0 : system_.displays[0].refresh_hz);
    display.rows.push_back(info(L"REFRESH", buffer, L"current monitor"));
    view.pages.push_back(std::move(display));

    OverlayPage performance;
    performance.title = L"Perf";
    std::swprintf(buffer, 220, L"%.1f fps", snapshot.source_fps);
    performance.rows.push_back(info(L"SOURCE", buffer,
                                    content_fps_ > 10.0
                                        ? std::to_wstring(static_cast<int>(content_fps_)) +
                                              L" fps unique content"
                                        : L""));
    {
        wchar_t age_text[96] = {};
        std::swprintf(age_text, 96, L"%.1f ms", last_age_ms_);
        performance.rows.push_back(info(L"FRAME AGE", age_text,
                                        L"packet to pull; 0 is the target"));
        wchar_t present_text[96] = {};
        std::swprintf(present_text, 96, L"%.1f ms",
                      ActivePresenter() ? ActivePresenter()->present_lag_ms() : 0.0);
        performance.rows.push_back(info(L"PRESENT", present_text,
                                        L"until the next scanout, below one refresh"));
    }
    performance.rows.push_back(info(
        L"DISPLAYED",
        std::to_wstring(static_cast<int>(
            dlss_fg ? (fg_fps_ > 0.5 ? fg_fps_
                                    : static_cast<double>(OutputFps(settings_)))
                    : static_cast<double>(display_rate_))) +
            L" fps"));
    std::swprintf(buffer, 220, L"%.2f ms", snapshot.decode_ms);
    performance.rows.push_back(info(L"DECODE", buffer));
    if (!dlss_fg && watching_) {
        wchar_t scanout_detail[96] = {};
        std::swprintf(buffer, 220, L"%.1f Hz", present_refresh_hz_);
        // A held refresh is normal when the source is slower than the display.
        // Subtract that baseline so this reads zero when pacing is healthy.
        const double expected_holds =
            std::max(0.0, present_refresh_hz_ - static_cast<double>(display_rate_));
        const double real_misses =
            static_cast<double>(present_misses_rate_) - expected_holds;
        std::swprintf(scanout_detail, 96, L"%llu real misses/s",
                      static_cast<unsigned long long>(std::max(0.0, real_misses)));
        performance.rows.push_back(info(L"SCANOUT", buffer, scanout_detail));
    }
    std::swprintf(buffer, 220, L"%.1f / %.1f ms", cadence_p50_, cadence_p99_);
    performance.rows.push_back(info(L"CADENCE", buffer, L"p50 / p99"));
    performance.rows.push_back(info(L"GENERATED",
                                    dlss_fg ? std::to_wstring(fg_dropped_) + L" dropped"
                                            : L"—",
                                    dlss_fg ? L"worker queue" : L"frame generation off"));
    std::swprintf(buffer, 220, L"%.2f / %.2f ms", diagnostics.frame_p50_ms,
                  diagnostics.frame_p99_ms);
    performance.rows.push_back(info(L"UI FRAME", buffer, L"p50 / p99"));
    performance.rows.push_back(action_row(L"Statistics", L"Reset",
                                          OverlayAction::ResetFrameStats,
                                          L"source, generated and cadence counters"));
    view.pages.push_back(std::move(performance));

    OverlayPage stream;
    stream.title = L"Stream";
    stream.rows.push_back(stepper(
        L"Source fps", std::to_wstring(settings_.streaming.fps) + L" fps",
        OverlayAction::FpsPrev, OverlayAction::FpsNext,
        L"STREAM RESTART required"));
    stream.rows.push_back(stepper(
        L"Bitrate",
        std::to_wstring(settings_.streaming.bitrate_kbps / 1000) + L" Mbps",
        OverlayAction::BitratePrev, OverlayAction::BitrateNext,
        L"STREAM RESTART required"));
    stream.rows.push_back(stepper(
        L"Decoder", Widen(settings_.streaming.decoder), OverlayAction::DecoderPrev,
        OverlayAction::DecoderNext, L"STREAM RESTART required"));
    stream.rows.push_back(stepper(
        L"Frame buffer",
        settings_.streaming.frame_buffer == 0
            ? std::wstring(L"Off")
            : std::to_wstring(settings_.streaming.frame_buffer) + L" frame(s)",
        OverlayAction::FrameBufferPrev, OverlayAction::FrameBufferNext,
        settings_.streaming.fps <= 30 ? L"live; 1 = even 8-refresh cadence"
                                       : L"live; 1 = one spare frame"));
    stream.rows.push_back(info(L"TRANSPORT", L"Reconnect to apply",
                               L"fps, bitrate and decoder are session options"));
    view.pages.push_back(std::move(stream));

    OverlayPage audio_page;
    audio_page.title = L"Audio";
    audio_page.rows.push_back(stepper(
        L"Buffer", std::to_wstring(settings_.audio.buffer_ms) + L" ms",
        OverlayAction::AudioBufferPrev, OverlayAction::AudioBufferNext,
        L"NEXT SESSION"));
    audio_page.rows.push_back(info(L"OUTPUT", L"Windows default",
                                   L"volume follows the Windows mixer"));
    view.pages.push_back(std::move(audio_page));

    OverlayPage pad_page;
    pad_page.title = L"Pad";
    pad_page.rows.push_back(stepper(
        L"Rumble", std::to_wstring(settings_.controller.rumble_strength) + L" / 5",
        OverlayAction::RumblePrev, OverlayAction::RumbleNext, L"live"));
    pad_page.rows.push_back(stepper(
        L"Poll rate", std::to_wstring(settings_.controller.poll_hz) + L" Hz",
        OverlayAction::PadRatePrev, OverlayAction::PadRateNext,
        L"live; USB is the most consistent at 1000 Hz"));
    view.pages.push_back(std::move(pad_page));
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
    profile.rows.push_back(action_row(L"Leave Watch", L"Back to app",
                                      OverlayAction::LeaveWatch,
                                      L"Alt+F4 also leaves without disconnecting"));
    profile.rows.push_back(action_row(L"Disconnect", L"Disconnect",
                                      OverlayAction::DisconnectNow,
                                      L"ends the console session"));
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
                    settings_.overlay.side, settings_.overlay.scale / 100.0f,
                    ActiveWindow()->hwnd());
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
            // Cycle only inside the SR family that is selected. With FSR SR
            // the only choice today is off; DLSS-G stays blocked.
            if (settings_.ai.SrWantsFsr(system_.gpu.nvidia)) {
                settings_.ai.fg_backend = AiBackend::None;
            } else {
                settings_.ai.fg_backend =
                    settings_.ai.fg_backend == AiBackend::DlssFg
                        ? AiBackend::None
                        : AiBackend::DlssFg;
            }
            settings_.ai.frame_generation =
                settings_.ai.fg_backend == AiBackend::DlssFg;
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
            video_intervals_.Reset();
            cadence_p50_ = 0.0;
            cadence_p99_ = 0.0;
            overlay_status_ = L"Frame statistics reset";
            overlay_status_error_ = false;
            last_overlay_update_ = -1.0;
            break;
        case OverlayAction::FpsPrev:
        case OverlayAction::FpsNext:
            settings_.streaming.fps = settings_.streaming.fps == 30 ? 60 : 30;
            changed();
            break;
        case OverlayAction::BitratePrev:
            settings_.streaming.bitrate_kbps =
                std::clamp(settings_.streaming.bitrate_kbps - 10000, 5000, 200000);
            changed();
            break;
        case OverlayAction::BitrateNext:
            settings_.streaming.bitrate_kbps =
                std::clamp(settings_.streaming.bitrate_kbps + 10000, 5000, 200000);
            changed();
            break;
        case OverlayAction::DecoderPrev:
        case OverlayAction::DecoderNext: {
            static const char* kDecoders[] = {"auto", "d3d11va", "d3d12va",
                                              "vulkan", "cuda", "software"};
            int index = 0;
            for (int i = 0; i < 6; ++i) {
                if (settings_.streaming.decoder == kDecoders[i]) {
                    index = i;
                }
            }
            index = (index + (action == OverlayAction::DecoderNext ? 1 : 5)) % 6;
            settings_.streaming.decoder = kDecoders[index];
            changed();
            break;
        }
        case OverlayAction::FrameBufferPrev:
            settings_.streaming.frame_buffer =
                std::clamp(settings_.streaming.frame_buffer - 1, 0, 3);
            changed();
            break;
        case OverlayAction::FrameBufferNext:
            settings_.streaming.frame_buffer =
                std::clamp(settings_.streaming.frame_buffer + 1, 0, 3);
            changed();
            break;
        case OverlayAction::RumblePrev:
            settings_.controller.rumble_strength =
                std::clamp(settings_.controller.rumble_strength - 1, 0, 5);
            haptic_strength_.store(settings_.controller.rumble_strength);
            changed();
            break;
        case OverlayAction::RumbleNext:
            settings_.controller.rumble_strength =
                std::clamp(settings_.controller.rumble_strength + 1, 0, 5);
            haptic_strength_.store(settings_.controller.rumble_strength);
            changed();
            break;
        case OverlayAction::PadRatePrev:
        case OverlayAction::PadRateNext: {
            static const int kRates[4] = {125, 250, 500, 1000};
            int index = 0;
            for (int i = 0; i < 4; ++i) {
                if (settings_.controller.poll_hz >= kRates[i]) {
                    index = i;
                }
            }
            index = (index + (action == OverlayAction::PadRateNext ? 1 : 3)) % 4;
            settings_.controller.poll_hz = kRates[index];
            pad_rate_hz_.store(kRates[index]);
            changed();
            break;
        }
        case OverlayAction::AudioBufferPrev:
            settings_.audio.buffer_ms = std::clamp(settings_.audio.buffer_ms - 5, 5, 200);
            changed();
            break;
        case OverlayAction::AudioBufferNext:
            settings_.audio.buffer_ms = std::clamp(settings_.audio.buffer_ms + 5, 5, 200);
            changed();
            break;
        case OverlayAction::SharpnessPrev:
            settings_.video.sharpening = std::clamp(settings_.video.sharpening - 5, 0, 100);
            changed();
            break;
        case OverlayAction::SharpnessNext:
            settings_.video.sharpening = std::clamp(settings_.video.sharpening + 5, 0, 100);
            changed();
            break;
        case OverlayAction::SideNext:
            settings_.overlay.side = settings_.overlay.side == 0 ? 1 : 0;
            changed();
            break;
        case OverlayAction::VerticalNext:
            settings_.overlay.position = (std::clamp(settings_.overlay.position, 0, 2) + 1) % 3;
            changed();
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
    // Enumerating adapters and monitors can take tens of milliseconds. Never do
    // it while a stream is presenting: a full system probe on the presentation
    // thread is a visible hitch, and the startup snapshot is already complete.
    if (watching_) {
        return;
    }
    if (last_probe_ < 0.0 || now - last_probe_ > 30.0) {
        system_ = ProbeSystem();
        last_probe_ = now;
    }
}

void NextApp::HandlePinInput() {
    brp::Win32Window* input_window = ActiveWindow();
    if (!input_window) {
        return;
    }
    const int key = input_window->ConsumeKey();
    const int character = input_window->ConsumeChar();
    if (overlay_control_) {
        if (key == VK_F10) {
            SetOverlayControl(false);
            return;
        }
        if (key == VK_ESCAPE) {
            SetOverlayControl(false);
            disconnect_armed_ = true;
            last_overlay_update_ = -1.0;
            return;
        }
        if (key != 0) {
            overlay_.Key(key);
        }
        return;
    }
    if (key == VK_F10 && watching_) {
        if (engine_mode_) {
            // The expanded control center lives in the GPUI shell. Tell it to
            // toggle and keep the GDI overlay for compact stats only.
            if (disconnect_armed_) {
                disconnect_armed_ = false;
            }
            // We own the foreground right now, so grant the GPUI shell the
            // right to take it for the control center window.
            ::AllowSetForegroundWindow(ASFW_ANY);
            bridge_.Publish("E overlay-toggle");
            return;
        }
        if (disconnect_armed_) {
            disconnect_armed_ = false;
        }
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
        // Esc is a two-step, persistent action: the first press arms the
        // disconnect and shows a banner over the stream; the second press, at
        // any time later, disconnects. No 2.5 s timeout, so Alt-Tab and coming
        // back does not lose the second press.
        if (disconnect_armed_) {
            disconnect_armed_ = false;
            DisconnectStream();
            return;
        }
        if (watching_) {
            disconnect_armed_ = true;
            last_overlay_update_ = -1.0;
            return;
        }
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

bool NextApp::PullFrame(media::DecodedFrame& out) {
    if (!PullFrameRaw(out)) {
        return false;
    }
    // Age of the frame's pixels since the video packet arrived: decode, queue
    // and pipelining time that the picture carries before it reaches the screen.
    const double now_ms =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();
    const double age = now_ms - out.arrived_ms;
    if (age >= 0.0 && age < 2000.0) {
        probe_age_sum_ += age;
        probe_age_max_ = std::max(probe_age_max_, age);
        ++probe_age_n_;
        // Smoothed copy for the UI: a single frame's age is noisy because the
        // pull can land anywhere between two decode threads.
        last_age_ms_ = last_age_ms_ * 0.8 + age * 0.2;
    }
    // Unique content rate: a 30 fps game streamed at 60 fps repeats every other
    // frame. Counting non-duplicates gives the real cadence for the UI and for
    // the AI stages.
    if (!out.duplicate) {
        if (content_window_ < 0.0) {
            content_window_ = now_;
        }
        ++content_unique_;
    }
    if (content_window_ >= 0.0 && now_ - content_window_ >= 1.0) {
        content_fps_ = content_unique_ / std::max(0.001, now_ - content_window_);
        content_unique_ = 0;
        content_window_ = now_;
    }
    return true;
}

bool NextApp::PullFrameRaw(media::DecodedFrame& out) {
    const int frame_buffer = std::clamp(settings_.streaming.frame_buffer, 0, 3);
    if (frame_buffer <= 0) {
        // Lowest latency without skipping: every decoded frame is delivered in
        // order. Only a backlog past two frames is trimmed, so even a decode
        // burst cannot leave more than ~one frame of age in the queue.
        return streaming_->TakeFrameOrdered(out, 2);
    }
    // Phase-locked pull: one source frame per source interval, on the display's
    // own tick grid. The decoded queue is primed and keeps one spare, so a
    // frame that arrives a few milliseconds late is covered instead of
    // shifting the whole cadence.
    const double nominal_fps =
        std::clamp(static_cast<double>(settings_.streaming.fps), 24.0, 240.0);
    // Phase advances with real elapsed time, not with the number of loop
    // iterations: the loop runs at ~244 Hz on a 240 Hz panel, so counting calls
    // ran every frame about 2 % fast and drained the queue.
    const double source_fps = nominal_fps * (1.0 + playout_trim_);
    const double elapsed = playout_clock_ < 0.0 ? 0.0 : std::clamp(now_ - playout_clock_, 0.0, 0.1);
    playout_clock_ = now_;
    video_phase_ += elapsed * source_fps;
    if (video_phase_ < 1.0 - 1e-9) {
        if (video_phase_ < 0.0) {
            video_phase_ = 0.0;
        }
        return false;
    }
    video_phase_ -= 1.0;
    if (video_phase_ > 1.0) {
        // A long stall must not turn into a fast-forward burst: one frame per call.
        video_phase_ = 1.0;
    }
    if (!streaming_->TakeFrame(out, frame_buffer, false)) {
        return false;
    }
    // Once per frame: the queue error moves the consumer rate toward the
    // producer's real rate (29.7 fps streams used to drain and refill the
    // buffer, which read as repeats and skips). Clamped to +-2 %.
    playout_depth_ = streaming_->QueueDepth();
    // Setpoint one frame above the pop target: with a single spare the queue
    // ran empty about a quarter of the time, and each empty slot is a repeat.
    const double target = static_cast<double>(streaming_->BufferTarget() + 1);
    const double error = static_cast<double>(playout_depth_) - target;
    playout_integral_ = std::clamp(playout_integral_ + 0.00005 * error, -0.02, 0.02);
    playout_trim_ = std::clamp(0.001 * error + playout_integral_, -0.02, 0.02);
    return true;
}

void NextApp::PresentVideo() {
    if (!streaming_) {
        return;
    }
    if (stream_dx12_) {
        PresentVideoDx12();
        return;
    }
    present_frame_ = false;
    brp::Presenter* video = ActivePresenter();

    const bool fg_mode = settings_.ai.frame_generation &&
                         settings_.ai.fg_backend == AiBackend::DlssFg &&
                         !settings_.ai.SrWantsFsr(system_.gpu.nvidia) &&
                         !dlss_fg_failed_;
    if (fg_mode) {
        const float background[4] = {0.01f, 0.008f, 0.016f, 1.0f};

        // If the worker is already attached but not covering (lost focus, the
        // NVIDIA overlay stole the foreground, Alt-Tab), keep the stream window
        // alive from the parent presenter. Skipping this froze on the last
        // presented image while audio continued.
        if (dlss_fg_ && dlss_fg_->ready() && dlss_fg_->attached()) {
            if (!overlay_pause_fg_) {
            dlss_fg_->FrameWindow(ActiveWindow()->hwnd(), now_);
        }
            fg_covering_ = dlss_fg_->covering();
            if (!fg_covering_) {
                media::DecodedFrame fallback;
                if (streaming_->TakeNewestFrame(fallback)) {
                    video_frame_ = std::move(fallback);
                    have_frame_ = true;
                    RecordVideoShown();
                }
                if (have_frame_ && !video_frame_.rgba.empty()) {
                    video->SetSourceFrame(video_frame_.rgba.data(), video_frame_.width,
                                          video_frame_.height, video_frame_.pq);
                    video->SetSourceFinal(false);
                    video->Render(now_);
                    present_frame_ = true;
                } else {
                    video->Clear(background);
                    present_frame_ = true;
                }
                return;
            }
        }

        bool new_frame = false;
        // The generator uses the same primed, phase-locked pull as the parent
        // path, plus a dedicated submit slot so its input cadence is exact.
        media::DecodedFrame frame;
        if (PullFrame(frame)) {
            video_frame_ = std::move(frame);
            have_frame_ = true;
            new_frame = true;
            EnhanceNeural(video_frame_);
        }

        auto publish = [&](std::vector<std::uint8_t>&& rgba, int width, int height) {
            if (!EnsureDlssFg(width, height)) {
                return false;
            }
            // The worker creates its presenter on the first Process(); submit
            // before looking for the window or the first frame never arrives
            // and the window never becomes visible.
            if (overlay_pause_fg_) {
                // The control center is open: keep the worker quiet so it
                // cannot raise itself over the panel or steal the foreground.
                return true;
            }
            dlss_fg_->Submit(std::move(rgba), width, height);
            dlss_fg_->FrameWindow(ActiveWindow()->hwnd(), now_);
            RecordVideoShown();
            return true;
        };

        auto show_standing = [&]() {
            if (have_frame_ && !video_frame_.rgba.empty()) {
                video->SetSourceFrame(video_frame_.rgba.data(), video_frame_.width,
                                           video_frame_.height, video_frame_.pq);
                video->SetSourceFinal(false);
                video->Render(now_);
            } else if (have_frame_ && video_frame_.gpu_view) {
                // GPU-only frame: the picture is already an RGBA texture on the
                // presenter's device.
                video->SetSourceView(static_cast<ID3D11ShaderResourceView*>(video_frame_.gpu_view),
                                          video_frame_.width, video_frame_.height, false);
                video->SetSourceFinal(false);
                video->Render(now_);
            } else {
                video->Clear(background);
            }
            present_frame_ = true;
        };

        if (new_frame && have_frame_ && !video_frame_.rgba.empty() &&
            !video_frame_.duplicate) {
            int feed_w = video_frame_.width;
            int feed_h = video_frame_.height;
            std::vector<std::uint8_t> rgba;
            bool have_rgba = false;
            const bool sr_can_feed_fg =
                settings_.ai.sr_output != Resolution::R2160 ||
                settings_.ai.sr_quality >= 5;
            const auto fg_sr_started = std::chrono::steady_clock::now();
            if (settings_.ai.super_resolution && sr_can_feed_fg) {
                if (EnsureDlssSr(feed_w, feed_h)) {
                    brp::media::NvDlssSr::Frame sr_frame;
                    std::string detail;
                    const int sw = feed_w;
                    const int sh = feed_h;
                    const int iw = dlss_sr_->in_width();
                    const int ih = dlss_sr_->in_height();
                    const std::uint8_t* input = video_frame_.rgba.data();
                    if (iw != sw || ih != sh) {
                        sr_scaled_.resize(static_cast<std::size_t>(iw) * ih * 4);
                        brp::media::DownscaleRgba(video_frame_.rgba.data(), sw, sh,
                                                  sr_scaled_.data(), iw, ih,
                                                  sr_scale_temp_);
                        input = sr_scaled_.data();
                    }
                    const bool sr_ok = dlss_sr_->Process(input, iw, ih, sr_frame, detail);
                    if (sr_ok && dlss_sr_->Readback(rgba, detail)) {
                        feed_w = dlss_sr_->out_width();
                        feed_h = dlss_sr_->out_height();
                        have_rgba = true;
                    } else {
                        dlss_failed_ = true;
                        LogPrintf(LogLevel::Warning, "[next] DLSS SR frame %dx%d: %s", iw,
                                  ih, detail.c_str());
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
            sr_cpu_ms_ = std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - fg_sr_started)
                             .count();
        }

        if (!dlss_fg_ || !dlss_fg_->ready()) {
            if (new_frame) {
                RecordVideoShown();
            }
            show_standing();
            return;
        }

        if (!overlay_pause_fg_) {
            dlss_fg_->FrameWindow(ActiveWindow()->hwnd(), now_);
        }
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
            show_standing();
            return;
        }
        if (!dlss_fg_->attached()) {
            // The worker is running but its presenter window is not up yet.
            show_standing();
            return;
        }
        if (!fg_covering_) {
            // The worker is attached but hidden (lost focus, Alt-Tab, another
            // full-screen overlay). Keep presenting the stream window from the
            // parent; otherwise the picture freezes on the last presented
            // frame while audio continues.
            show_standing();
            return;
        }
        // The worker's topmost window is the picture. Do not present the
        // parent swapchain while it covers the screen: that makes DWM compose
        // a second full-size surface every vblank and adds a frame of latency
        // for no visible gain.
        return;
    }

    bool new_frame = false;
    media::DecodedFrame frame;
    if (PullFrame(frame)) {
        video_frame_ = std::move(frame);
        have_frame_ = true;
        new_frame = true;
        EnhanceNeural(video_frame_);
    }
    // A repeated 30 fps frame or an idle decoder is not new content: leave the
    // last presented image on screen instead of pushing an untouched backbuffer
    // through the flip chain.
    if (!new_frame && !redraw_needed_) {
        return;
    }
    redraw_needed_ = false;
    if (!have_frame_ || (video_frame_.rgba.empty() && !video_frame_.gpu_view)) {
        const float background[4] = {0.015f, 0.017f, 0.022f, 1.0f};
        video->Clear(background);
        present_frame_ = true;
        return;
    }

    const int width = video_frame_.width;
    const int height = video_frame_.height;
    if (new_frame && !video_frame_.duplicate) {
        bool sr_drawn = false;
        const auto sr_started = std::chrono::steady_clock::now();
        if (video_frame_.gpu_view && video_frame_.rgba.empty()) {
            // No AI stage needs system memory: present the video-processor
            // output in place, with no readback and no swscale.
            video->SetSourceView(static_cast<ID3D11ShaderResourceView*>(video_frame_.gpu_view),
                                      width, height, false);
            video->SetSourceFinal(false);
            sr_drawn = true;
        } else if (EnsureDlssSr(width, height)) {
            brp::media::NvDlssSr::Frame sr_frame;
            std::string detail;
            const int iw = dlss_sr_->in_width();
            const int ih = dlss_sr_->in_height();
            const std::uint8_t* input = video_frame_.rgba.data();
            if (iw != width || ih != height) {
                sr_scaled_.resize(static_cast<std::size_t>(iw) * ih * 4);
                brp::media::DownscaleRgba(video_frame_.rgba.data(), width, height,
                                          sr_scaled_.data(), iw, ih,
                                          sr_scale_temp_);
                input = sr_scaled_.data();
            }
            const bool sync_sr =
                settings_.streaming.frame_buffer > 0 && settings_.streaming.fps <= 30;
            if (dlss_sr_->Process(input, iw, ih, sr_frame, detail, sync_sr)) {
                video->SetSourceView(sr_frame.srv.Get(), dlss_sr_->out_width(),
                                          dlss_sr_->out_height(), video_frame_.pq);
                video->SetSourceFinal(false);
                sr_drawn = true;
            } else {
                dlss_failed_ = true;
                Toast(L"DLSS SR: " + Widen(detail), true);
            }
        }
        if (!sr_drawn) {
            video->SetSourceFrame(video_frame_.rgba.data(), width, height,
                                       video_frame_.pq);
            video->SetSourceFinal(false);
        }
        sr_cpu_ms_ = std::chrono::duration<double, std::milli>(
                         std::chrono::steady_clock::now() - sr_started)
                         .count();
        RecordVideoShown();
    }
    // Redraw the standing frame only when a present is actually due: a new
    // unique frame, or a resize that left the backbuffer blank. Skipping the
    // idle presents keeps the flip queue from holding a frame nobody asked for.
    video->Render(now_);
    present_frame_ = true;
}

bool NextApp::PresentVideoDx12() {
    if (!streaming_ || !stream_dx12_) {
        return false;
    }
    present_frame_ = false;
    media::DecodedFrame frame;
    const bool new_frame = PullFrame(frame);
    if (new_frame) {
        video_frame_ = std::move(frame);
        have_frame_ = true;
        EnhanceNeural(video_frame_);
    }
    if (!new_frame && !redraw_needed_) {
        return false;
    }
    if (!have_frame_ || (video_frame_.rgba.empty() && !video_frame_.gpu_shared)) {
        return false;
    }
    // A repeated console frame is not new picture: leaving the last present on
    // screen is both cheaper and more stable than re-uploading it.
    if (video_frame_.duplicate && !redraw_needed_) {
        return false;
    }
    redraw_needed_ = false;

    const int width = video_frame_.width;
    const int height = video_frame_.height;
    // One upscaler family per frame. Frame generation is checked against the
    // same answer, so DLSS-G can never run on FSR SR (or the reverse).
    const bool want_fsr = settings_.ai.SrWantsFsr(system_.gpu.nvidia);
    const bool want_dlss = !want_fsr;
    bool drawn = false;
    HANDLE sr_handle = nullptr;
    int sr_width = width;
    int sr_height = height;
    auto run_sr = [&]() -> bool {
        if (!settings_.ai.super_resolution || video_frame_.rgba.empty()) {
            return false;
        }
        if (want_fsr) {
            if (!EnsureFsr(width, height)) {
                return false;
            }
            std::string detail;
            const int fiw = fsr_->in_width();
            const int fih = fsr_->in_height();
            const std::uint8_t* input = video_frame_.rgba.data();
            if (fiw != width || fih != height) {
                sr_scaled_.resize(static_cast<std::size_t>(fiw) * fih * 4);
                brp::media::DownscaleRgba(video_frame_.rgba.data(), width, height,
                                          sr_scaled_.data(), fiw, fih, sr_scale_temp_);
                input = sr_scaled_.data();
            }
            fsr_->SetSharpness(static_cast<float>(settings_.ai.fsr_sharpness) / 100.0f);
            if (!fsr_->Process(input, fiw, fih,
                               static_cast<float>(1000.0 / std::max(1, settings_.streaming.fps)),
                               detail)) {
                Toast(L"FSR: " + Widen(detail), true);
                return false;
            }
            sr_handle = fsr_->SharedHandle();
            sr_width = fsr_->out_width();
            sr_height = fsr_->out_height();
            return sr_handle != nullptr;
        }
        if (!EnsureDlssSr(width, height)) {
            return false;
        }
        brp::media::NvDlssSr::Frame sr_frame;
        std::string detail;
        const int iw = dlss_sr_->in_width();
        const int ih = dlss_sr_->in_height();
        const std::uint8_t* input = video_frame_.rgba.data();
        if (iw != width || ih != height) {
            sr_scaled_.resize(static_cast<std::size_t>(iw) * ih * 4);
            brp::media::DownscaleRgba(video_frame_.rgba.data(), width, height,
                                      sr_scaled_.data(), iw, ih, sr_scale_temp_);
            input = sr_scaled_.data();
        }
        if (!dlss_sr_->Process(input, iw, ih, sr_frame, detail)) {
            dlss_failed_ = true;
            Toast(L"DLSS SR: " + Widen(detail), true);
            return false;
        }
        sr_handle = dlss_sr_->SharedHandle();
        sr_width = dlss_sr_->out_width();
        sr_height = dlss_sr_->out_height();
        return sr_handle != nullptr;
    };

    const bool want_fg_dx12 =
        settings_.ai.frame_generation && settings_.ai.fg_backend == AiBackend::DlssFg &&
        want_dlss && !fg_dx12_failed_;
    if (want_fg_dx12 && !video_frame_.duplicate) {
        const bool sr_ok = run_sr();
        const int fg_width = sr_ok ? sr_width : width;
        const int fg_height = sr_ok ? sr_height : height;
        if (EnsureDlssFgDx12(fg_width, fg_height)) {
            std::string detail;
            const bool processed =
                sr_ok ? dlss_fg_dx12_->ProcessShared(sr_handle, fg_width, fg_height,
                                                     fg_dx12_reset_, detail)
                      : dlss_fg_dx12_->Process(video_frame_.rgba.data(), fg_width,
                                               fg_height, fg_dx12_reset_, detail);
            if (processed) {
                fg_dx12_reset_ = false;
                const auto& frames = dlss_fg_dx12_->Frames();
                if (!frames.empty()) {
                    const double content = content_fps_ > 10.0
                                               ? std::clamp(content_fps_, 24.0, 60.0)
                                               : static_cast<double>(settings_.streaming.fps);
                    const double interval = 1.0 / std::max(24.0, content);
                    const int generated = static_cast<int>(frames.size()) - 1;
                    for (int i = 1; i <= generated; ++i) {
                        fg_pending_.push_back(
                            {frames[static_cast<std::size_t>(i)].shared, true,
                             now_ + interval * static_cast<double>(i) /
                                        static_cast<double>(generated)});
                    }
                    drawn = stream_dx12_->DrawShared(frames[0].shared, fg_width,
                                                     fg_height, video_frame_.pq);
                }
            } else {
                Toast(L"DLSS-G: " + Widen(detail), true);
            }
        }
    }
    if (!drawn && !video_frame_.duplicate) {
        if (!sr_handle) {
            run_sr();
        }
        if (sr_handle) {
            drawn = stream_dx12_->DrawShared(sr_handle, sr_width, sr_height,
                                             video_frame_.pq);
        }
    }
    if (!drawn) {
        if (video_frame_.gpu_shared && video_frame_.rgba.empty()) {
            drawn = stream_dx12_->DrawShared(
                static_cast<HANDLE>(video_frame_.gpu_shared), width, height,
                video_frame_.pq);
        } else if (!video_frame_.rgba.empty()) {
            drawn = stream_dx12_->DrawCpu(video_frame_.rgba.data(), width, height,
                                          video_frame_.pq);
        }
    }
    if (!drawn) {
        if (!stream_dx12_->status().empty()) {
            static double last_note = -1.0;
            if (now_ - last_note > 5.0) {
                last_note = now_;
                Toast(Widen(stream_dx12_->status()), true);
            }
        }
        return false;
    }
    present_frame_ = true;
    RecordVideoShown();
    return true;
}

bool NextApp::TickDlssFgDx12() {
    if (!stream_dx12_ || fg_pending_.empty()) {
        return false;
    }
    if (fg_pending_.front().due > now_ + 0.0005) {
        return false;
    }
    const FgPending frame = fg_pending_.front();
    fg_pending_.pop_front();
    if (!frame.shared) {
        return false;
    }
    const int width = fg_dx12_w_ > 0 ? fg_dx12_w_ : video_frame_.width;
    const int height = fg_dx12_h_ > 0 ? fg_dx12_h_ : video_frame_.height;
    if (width <= 0 || height <= 0) {
        return false;
    }
    if (!stream_dx12_->DrawShared(frame.shared, width, height, video_frame_.pq)) {
        return false;
    }
    present_frame_ = true;
    RecordVideoShown();
    return true;
}

bool NextApp::EnsureNr(int width, int height) {
    if (nr_failed_) {
        return false;
    }
    if (nr_ready_ && width == nr_w_ && height == nr_h_) {
        return true;
    }
    if (nr_ready_) {
        StopNr();
    }
    if (!settings_.ai.neural_render) {
        return false;
    }
    if (width > 2560 || height > 1440) {
        Toast(L"Neural render is capped at 2560x1440", true);
        nr_failed_ = true;
        return false;
    }
    std::filesystem::path dir;
    if (const char* env = std::getenv("BRP_NR_DIR")) {
        dir = std::filesystem::path(env);
    }
    const std::filesystem::path exe_dir(system_.exe_dir);
    if (dir.empty()) {
        const std::filesystem::path candidates[] = {
            exe_dir / "nr" / "native",
            exe_dir / "neuralscreen" / "native",
            exe_dir / ".." / "third_party" / "neuralscreen" / "native",
        };
        for (const auto& candidate : candidates) {
            std::error_code ec;
            if (std::filesystem::exists(candidate / "nvngx.dll", ec) &&
                std::filesystem::exists(candidate / "nvngx_dlssnr.dll", ec)) {
                dir = candidate;
                break;
            }
        }
    }
    if (dir.empty()) {
        std::error_code ec;
        const std::filesystem::path cwd = std::filesystem::current_path(ec);
        if (!ec) {
            const std::filesystem::path candidate =
                cwd / "third_party" / "neuralscreen" / "native";
            if (std::filesystem::exists(candidate / "nvngx.dll", ec) &&
                std::filesystem::exists(candidate / "nvngx_dlssnr.dll", ec)) {
                dir = candidate;
            }
        }
    }
    if (dir.empty()) {
        Toast(L"Neural render runtime missing (nr\\native)", true);
        nr_failed_ = true;
        return false;
    }
    brp::media::NvDlssnr::Options options = NeuralOptionsFrom(settings_.ai);

    dlssnr_ = std::make_unique<brp::media::NvDlssnr>();
    std::string detail;
    if (!dlssnr_->Start((dir / "nvngx.dll").wstring(), dir.wstring(), width, height,
                        options, detail)) {
        Toast(L"Neural render: " + Widen(detail), true);
        dlssnr_.reset();
        nr_failed_ = true;
        return false;
    }
    nr_ready_ = true;
    nr_w_ = width;
    nr_h_ = height;
    nr_ms_ = 0.0;
    return true;
}

void NextApp::StopNr() {
    if (dlssnr_) {
        dlssnr_->Stop();
        dlssnr_.reset();
    }
    nr_ready_ = false;
    nr_w_ = 0;
    nr_h_ = 0;
}

bool NextApp::NeuralInWorkerWanted() const {
    if (std::getenv("BRP_NR_MAIN")) {
        return false;  // diagnostic: force the standalone NR path
    }
    return settings_.ai.neural_render && settings_.ai.frame_generation &&
           settings_.ai.fg_backend == AiBackend::DlssFg &&
           !settings_.ai.SrWantsFsr(system_.gpu.nvidia);
}

void NextApp::EnhanceNeural(media::DecodedFrame& frame) {
    if (dlss_fg_ && dlss_fg_->ready() && dlss_fg_->neural_in_worker()) {
        // NR runs inside the DLSS-G worker; the standalone probe is stale and
        // must not be shown as the live NR time. The worker's `fg` field is
        // the combined NR + frame-generation process time.
        nr_ms_ = 0.0;
        return;
    }
    if (!settings_.ai.neural_render || nr_failed_ || frame.rgba.empty() || frame.pq ||
        frame.duplicate) {
        // HDR frames go straight through, and a repeated content frame has no
        // new detail for the network.
        return;
    }
    if (!EnsureNr(frame.width, frame.height)) {
        return;
    }
    std::vector<std::uint8_t> out;
    std::string detail;
    if (dlssnr_->Process(frame.rgba.data(), frame.width, frame.height, out, detail) &&
        !out.empty()) {
        frame.rgba = std::move(out);
        nr_ms_ = dlssnr_->last_ms();
    } else {
        Toast(L"Neural render stopped: " + Widen(detail), true);
        nr_failed_ = true;
        StopNr();
    }
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
        disconnect_armed_ = false;
        have_frame_ = false;
        redraw_needed_ = true;
        video_phase_ = 0.0;
        last_source_refresh_ = 0;
        hold_counts_.fill(0);
        hold_seen_.fill(0);
        hold_total_ = 0;
        content_changed_ = false;
        video_intervals_.Reset();
        last_video_shown_at_ = -1.0;
        // Keep the DLSS-G worker alive between sessions. Starting and stopping
        // it costs seconds (worker, NGX context and job object); hiding its
        // window is instant, and the next Watch resumes the same worker.
        if (dlss_fg_) {
            dlss_fg_->Hide();
        }
        fg_covering_ = false;
        fg_fps_ = 0.0;
        fg_actual_mult_ = 0;
        dlss_fg_failed_ = false;
        // DLSS SR is bound to the D3D11 device of the presenter. The stream
        // window has its own device, so the feature must be created again.
        if (dlss_sr_) {
            dlss_sr_->Shutdown();
        }
        if (fsr_) {
            fsr_->Shutdown();
        }
        fsr_ready_ = false;
        fsr_failed_ = false;
        dlss_ready_ = false;
        dlss_failed_ = false;
        dlss_in_w_ = 0;
        dlss_in_h_ = 0;
        HideNvidiaOverlay();
        if (!EnsureStreamSurface()) {
            watching_ = false;
            if (!engine_mode_) {
                window_->Show();
            }
            return;
        }
        // The app window steps out of the way: the stream window is the only
        // video surface, with no D2D UI, no menu swapchain and no title bar.
        window_->SetCursorHidden(false);
        // In engine mode this window is the hidden legacy surface. Minimizing
        // it gave it a taskbar button, and after a disconnect that blank window
        // stayed around and could take the foreground from the shell.
        if (!engine_mode_) {
            window_->Minimize();
        }
    } else {
        watching_ = false;
        have_frame_ = false;
        SetOverlayControl(false);
        StopNr();
        if (dlss_fg_dx12_) {
            dlss_fg_dx12_->Stop();
            dlss_fg_dx12_.reset();
        }
        fg_pending_.clear();
        if (dlss_fg_) {
            dlss_fg_->Hide();
        }
        fg_covering_ = false;
        fg_fps_ = 0.0;
        fg_actual_mult_ = 0;
        if (stream_window_) {
            if (stream_window_->fullscreen()) {
                stream_window_->ExitFullscreen();
            }
            stream_window_->SetCursorHidden(false);
            stream_window_->Hide();
        }
        watch_fullscreen_ = false;
        // In engine mode the shell owns the UI and this window stays hidden:
        // restoring it here left a blank 1600x900 window over the desktop that
        // swallowed clicks after a disconnect.
        if (!engine_mode_) {
            window_->Restore();
            window_->Show();
        } else if (window_->hwnd()) {
            // A minimise request from the OS (Alt+Tab, taskbar) can still
            // surface it; make sure the stream end puts it back out of sight.
            ::ShowWindow(window_->hwnd(), SW_HIDE);
        }
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
        int timer_hz = 0;
        auto apply_rate = [&](int hz) {
            if (!timer || hz == timer_hz) {
                return;
            }
            timer_hz = hz;
            LARGE_INTEGER due{};
            due.QuadPart = -static_cast<LONGLONG>(10000000LL / hz);
            ::SetWaitableTimer(timer, &due, std::max(1, 1000 / hz), nullptr, nullptr,
                              FALSE);
        };
        while (pad_run_.load()) {
            const int poll_hz = std::clamp(pad_rate_hz_.load(), 60, 1000);
            apply_rate(poll_hz);
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
                std::this_thread::sleep_for(std::chrono::microseconds(1000000 / poll_hz));
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
    // NVIDIA's quality ladder is a render-scale ladder. The console frame is
    // downscaled to the selected percentage first, then DLSS reconstructs the
    // target. DLAA stays at 100% and an explicit 1080p output is native AA.
    const bool dlaa = settings_.ai.sr_quality >= 5 ||
                      settings_.ai.sr_output == Resolution::R1080;
    int out_w = input_width;
    int out_h = input_height;
    int in_w = input_width;
    int in_h = input_height;
    int quality = 5;
    if (!dlaa) {
        if (settings_.ai.sr_output == Resolution::R2160) {
            out_w = 3840;
            out_h = 2160;
        } else if (settings_.ai.sr_output == Resolution::R1440) {
            out_w = 2560;
            out_h = 1440;
        } else if (stream_window_) {
            out_w = std::max(stream_window_->width(), input_width);
            out_h = std::max(stream_window_->height(), input_height);
        }
        if (out_w <= input_width || out_h <= input_height) {
            out_w = input_width;
            out_h = input_height;
        }
        static const int kPercent[6] = {33, 50, 58, 66, 77, 100};
        // NVSDK_NGX_PerfQuality_Value 4 (Ultra Quality) was retired with DLSS
        // 2.x: CreateFeature rejects it with FAIL_UnsupportedParameter, which
        // silently disabled SR for the "Max" preset. The explicit sizes set on
        // the feature decide the ratio, so MaxQuality is the right hint for
        // both 66 % and 77 %.
        static const int kQuality[6] = {3, 0, 1, 2, 2, 5};
        const int index = std::clamp(settings_.ai.sr_quality, 0, 5);
        const int pct = kPercent[index];
        quality = kQuality[index];
        in_w = std::max(2, (out_w * pct / 100) / 2 * 2);
        in_h = std::max(2, (out_h * pct / 100) / 2 * 2);
        in_w = std::min(in_w, input_width);
        in_h = std::min(in_h, input_height);
        // The console frame is 1920x1080. When the requested render size is
        // within 75 % of it, feed the decoded frame directly instead of
        // spending 3-4 ms on a CPU downscale: DLSS uses the explicit feature
        // sizes, not the percentage label, to decide the ratio.
        if (in_w * 4 >= input_width * 3 && in_h * 4 >= input_height * 3) {
            in_w = input_width;
            in_h = input_height;
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
            LogPrintf(LogLevel::Warning, "[next] DLSS runtime: %s", detail.c_str());
            Toast(L"DLSS runtime: " + Widen(detail), true);
            return false;
        }
    }
    std::string detail;
    // DLSS model preset. K is the transformer default for DLAA/Quality, M is
    // the DLSS 4.5 performance model, L the ultra-performance model; J is the
    // older transformer for less ghosting. Auto follows NVIDIA's mapping.
    int preset = 11;
    switch (settings_.ai.sr_preset) {
        case 1: preset = 11; break;  // K
        case 2: preset = 12; break;  // L
        case 3: preset = 13; break;  // M
        case 4: preset = 10; break;  // J
        default:
            if (quality == 3) {
                preset = 12;  // Ultra Performance
            } else if (quality == 0) {
                preset = 13;  // Performance
            } else {
                preset = 11;  // DLAA / Balanced / Quality
            }
            break;
    }
    ID3D11Device* sr_device =
        decode_device_ ? decode_device_ : (ActivePresenter() ? ActivePresenter()->device() : nullptr);
    ID3D11DeviceContext* sr_context =
        decode_context_ ? decode_context_
                        : (ActivePresenter() ? ActivePresenter()->context() : nullptr);
    if (!sr_device || !sr_context) {
        dlss_failed_ = true;
        Toast(L"DLSS SR: no D3D11 device for the shared textures", true);
        return false;
    }
    if (!dlss_sr_->Initialize(sr_device, sr_context, in_w,
                              in_h, out_w, out_h, preset, quality, detail)) {
        dlss_failed_ = true;
        LogPrintf(LogLevel::Warning,
                  "[next] DLSS SR init %dx%d -> %dx%d preset %d quality %d: %s", in_w,
                  in_h, out_w, out_h, preset, quality, detail.c_str());
        Toast(L"DLSS SR: " + Widen(detail), true);
        return false;
    }
    dlss_ready_ = true;
    dlss_in_w_ = input_width;
    dlss_in_h_ = input_height;
    return true;
}

bool NextApp::EnsureDlssFgDx12(int width, int height) {
    if (!settings_.ai.frame_generation ||
        settings_.ai.fg_backend != AiBackend::DlssFg) {
        return false;
    }
    if (settings_.ai.SrWantsFsr(system_.gpu.nvidia)) {
        // DLSS-G is only valid next to DLSS SR. The UI blocks the pair, but a
        // config from an older build must not reach the generator either.
        return false;
    }
    if (std::getenv("VEYLA_NATIVE_FG") == nullptr) {
        // DLSS-G 310.x does not write its offscreen OutputInterpolated texture
        // without the swapchain path; generated frames are black. Keep the
        // picture on SR until the swapchain integration lands. Opt in only for
        // development.
        static bool announced = false;
        if (!announced) {
            announced = true;
            Toast(L"DLSS-G offscreen output is disabled in this beta; using SR", true);
        }
        return false;
    }
    if (fg_dx12_failed_) {
        return false;
    }
    const int multiplier = std::clamp(settings_.ai.fg_multiplier, 2, 6);
    if (dlss_fg_dx12_ && fg_dx12_w_ == width && fg_dx12_h_ == height &&
        dlss_fg_dx12_->requested_multiplier() == multiplier) {
        return true;
    }
    if (width <= 0 || height <= 0) {
        return false;
    }
    if (dlss_fg_dx12_) {
        dlss_fg_dx12_->Stop();
        dlss_fg_dx12_.reset();
    }
    if (!system_.gpu.nvidia || !system_.dlss_fg || system_.nr_dir.empty()) {
        fg_dx12_failed_ = true;
        Toast(L"DLSS-G runtime missing", true);
        return false;
    }
    if (!decode_device_) {
        fg_dx12_failed_ = true;
        Toast(L"DLSS-G needs the decode device", true);
        return false;
    }
    dlss_fg_dx12_ = std::make_unique<brp::media::NvDlssFg>();
    std::string detail;
    if (!dlss_fg_dx12_->Start(decode_device_, system_.nr_dir, width, height, multiplier,
                              detail)) {
        dlss_fg_dx12_.reset();
        fg_dx12_failed_ = true;
        LogPrintf(LogLevel::Warning, "[next] native DLSS-G: %s", detail.c_str());
        Toast(L"DLSS-G: " + Widen(detail), true);
        return false;
    }
    fg_dx12_w_ = width;
    fg_dx12_h_ = height;
    fg_pending_.clear();
    Toast(std::to_wstring(dlss_fg_dx12_->requested_multiplier()) +
          L"x DLSS-G active (DX12)");
    return true;
}

bool NextApp::EnsureFsr(int input_width, int input_height) {
    if (!settings_.ai.super_resolution || !system_.fsr_sr || fsr_failed_) {
        return false;
    }
    if (fsr_ready_ && fsr_in_w_ == input_width && fsr_in_h_ == input_height) {
        return true;
    }
    int out_w = input_width;
    int out_h = input_height;
    if (settings_.ai.sr_output == Resolution::R2160) {
        out_w = 3840;
        out_h = 2160;
    } else if (settings_.ai.sr_output == Resolution::R1440) {
        out_w = 2560;
        out_h = 1440;
    } else if (stream_window_) {
        // Native = the stream window (or the fullscreen scanout) size.
        out_w = std::max(stream_window_->width(), input_width);
        out_h = std::max(stream_window_->height(), input_height);
    }
    if (out_w <= input_width || out_h <= input_height) {
        out_w = input_width;
        out_h = input_height;
    }
    const int quality = std::clamp(settings_.ai.fsr_quality, 0, 4);
    int in_w = out_w;
    int in_h = out_h;
    if (quality == 0) {
        // Native AA: no upscale, input and output are the decoded frame.
        out_w = input_width;
        out_h = input_height;
        in_w = input_width;
        in_h = input_height;
    } else {
        static const float kRatio[5] = {1.0f, 1.5f, 1.7f, 2.0f, 3.0f};
        const float ratio = kRatio[quality];
        in_w = std::max(64, static_cast<int>(out_w / ratio) / 2 * 2);
        in_h = std::max(64, static_cast<int>(out_h / ratio) / 2 * 2);
        in_w = std::min(in_w, input_width);
        in_h = std::min(in_h, input_height);
    }
    if (fsr_ready_ && fsr_in_w_ == input_width && fsr_in_h_ == input_height &&
        fsr_out_w_ == out_w && fsr_out_h_ == out_h && fsr_quality_ == quality) {
        return true;
    }
    if (!fsr_) {
        fsr_ = std::make_unique<brp::media::AmdFsr>();
    }
    if (!fsr_->runtime_loaded()) {
        std::string detail;
        if (!fsr_->LoadRuntime(system_.fsr_dir, detail)) {
            fsr_failed_ = true;
            LogPrintf(LogLevel::Warning, "[next] FSR runtime: %s", detail.c_str());
            Toast(L"FSR runtime: " + Widen(detail), true);
            return false;
        }
    }
    brp::media::AmdFsr::Variant variant = brp::media::AmdFsr::Variant::Auto;
    if (settings_.ai.sr_model == "fsr4") {
        variant = brp::media::AmdFsr::Variant::Fsr4;
    } else if (settings_.ai.sr_model == "fsr3") {
        variant = brp::media::AmdFsr::Variant::Fsr3;
    }
    ID3D11Device* sr_device =
        decode_device_ ? decode_device_
                       : (ActivePresenter() ? ActivePresenter()->device() : nullptr);
    ID3D11DeviceContext* sr_context =
        decode_context_ ? decode_context_
                        : (ActivePresenter() ? ActivePresenter()->context() : nullptr);
    if (!sr_device || !sr_context) {
        fsr_failed_ = true;
        Toast(L"FSR: no D3D11 device for the adapter", true);
        return false;
    }
    std::string detail;
    if (!fsr_->Initialize(sr_device, sr_context, in_w, in_h, out_w, out_h, variant, false,
                          detail)) {
        fsr_failed_ = true;
        LogPrintf(LogLevel::Warning, "[next] FSR init %dx%d -> %dx%d: %s", in_w, in_h,
                  out_w, out_h, detail.c_str());
        Toast(L"FSR: " + Widen(detail), true);
        return false;
    }
    fsr_->SetSharpness(static_cast<float>(settings_.ai.fsr_sharpness) / 100.0f);
    fsr_ready_ = true;
    fsr_in_w_ = input_width;
    fsr_in_h_ = input_height;
    fsr_out_w_ = out_w;
    fsr_out_h_ = out_h;
    fsr_quality_ = quality;
    fsr_provider_ = fsr_->provider();
    return true;
}

bool NextApp::EnsureDlssFg(int width, int height) {
    if (!settings_.ai.frame_generation ||
        settings_.ai.fg_backend != AiBackend::DlssFg) {
        return false;
    }
    if (settings_.ai.SrWantsFsr(system_.gpu.nvidia)) {
        return false;  // DLSS-G never pairs with FSR SR
    }
    if (dlss_fg_failed_) {
        return false;
    }
    const int requested = std::clamp(settings_.ai.fg_multiplier, 2, 6);
    // Forced multipliers: 3x-6x load the SM86 proxy below and use whatever the
    // user asked for. On an older card the worker may fail; that is the
    // documented trade-off, not silently stepped back to 2x.
    const int multiplier = requested;
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
    dlss_fg_->SetNeuralInWorker(NeuralInWorkerWanted());
    dlss_fg_->SetNeuralOptions(NeuralOptionsFrom(settings_.ai));
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
    disconnect_armed_ = false;
    content_fps_ = 0.0;
    content_unique_ = 0;
    content_window_ = -1.0;
    ConsoleSettings console = settings_.console;
    console.fps = settings_.streaming.fps;
    if (console.address.empty()) {
        console.address = streaming_->Profile().host;
    }
    OpenAudio();
    streaming_->SetAudio(audio_.is_open() ? &audio_ : nullptr);
    streaming_->SetStreamSettings(settings_.streaming);
    streaming_->SetCpuPixels(NeedsCpuPixels());
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
    disconnect_armed_ = false;
    if (!streaming_) {
        return;
    }
    if (watching_) {
        ToggleWatch(false);
    }
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

void NextApp::HandleBridgeCommand(const std::string& command) {
    if (command == "connect") {
        if (!streaming_ || !streaming_->Connected()) {
            ConnectStream();
        }
    } else if (command == "watch") {
        if (streaming_ && streaming_->Connected() && !watching_) {
            ToggleWatch(true);
        }
    } else if (command == "stop_watch") {
        if (watching_) {
            ToggleWatch(false);
        }
    } else if (command == "disconnect") {
        DisconnectStream();
    } else if (command == "apply") {
        ReloadSettingsFromDisk();
    } else if (command == "check-update") {
        // The repository can be blank in an old settings file; that used to
        // make the check fail with a confusing message. Fall back to the
        // shipped default instead.
        ReloadSettingsFromDisk();
        std::string repo = settings_.general.update_repo;
        if (repo.empty()) {
            repo = "bulinatu/Veyla";
        }
        update_.Check(repo, settings_.general.update_channel == 1, AppVersion());
    } else if (command == "download-update") {
        update_.Download();
    } else if (command == "install-update") {
        std::string update_error;
        if (!update_.Install(std::filesystem::path(system_.exe_dir),
                             ::GetCurrentProcessId(), update_error)) {
            Toast(L"Update: " + Widen(update_error), true);
        }
    } else if (command == "devices") {
        publish_devices_ = true;
    } else if (command == "discover") {
        if (streaming_) {
            const Status status = streaming_->StartDiscovery();
            if (!status.ok()) {
                Toast(Widen(status.message()), true);
            }
        }
    } else if (command.rfind("console_use ", 0) == 0) {
        const std::string address = command.substr(12);
        if (streaming_) {
            for (const auto& candidate : streaming_->Discovered()) {
                if (candidate.address == address) {
                    const Status status = streaming_->SetConsole(candidate);
                    if (!status.ok()) {
                        Toast(Widen(status.message()), true);
                    }
                    break;
                }
            }
        }
    } else if (command.rfind("pair ", 0) == 0) {
        if (streaming_) {
            const unsigned long pin = std::strtoul(command.c_str() + 5, nullptr, 10);
            if (pin == 0) {
                Toast(L"Enter the PIN shown on the console", true);
            } else {
                const Status status = streaming_->Pair(static_cast<std::uint32_t>(pin));
                Toast(status.ok() ? std::wstring(L"Pairing...") : Widen(status.message()),
                      !status.ok());
            }
        }
    } else if (command == "psn-open") {
        if (streaming_) {
            std::string detail;
            const Status status = streaming_->OpenPsnLogin(detail);
            if (status.ok() && !detail.empty()) {
                bridge_.Publish("E {\"kind\":\"psn_url\",\"url\":\"" + JsonEscape(detail) +
                                "\"}");
            } else {
                Toast(status.ok() ? std::wstring(L"PSN login unavailable")
                                  : Widen(status.message()),
                      true);
            }
        }
    } else if (command.rfind("psn-complete ", 0) == 0) {
        if (streaming_) {
            std::string detail;
            const Status status = streaming_->CompletePsnLogin(command.substr(13), detail);
            Toast(status.ok() ? std::wstring(L"PSN account linked") : Widen(detail),
                  !status.ok());
        }
    } else if (command == "profiles") {
        publish_profiles_ = true;
    } else if (command.rfind("profile_apply ", 0) == 0) {
        ApplyProfileById(command.substr(14));
    } else if (command == "profile_save") {
        SaveProfileByName({});
    } else if (command.rfind("profile_save ", 0) == 0) {
        SaveProfileByName(command.substr(13));
    } else if (command.rfind("profile_delete ", 0) == 0) {
        DeleteProfileById(command.substr(15));
    } else if (command == "overlay_open") {
        overlay_pause_fg_ = true;
        if (dlss_fg_) {
            dlss_fg_->Hide();
        }
        if (overlay_.visible()) {
            overlay_.Hide();
        }
    } else if (command == "overlay_close") {
        overlay_pause_fg_ = false;
    } else if (command == "leave_watch") {
        if (watching_) {
            ToggleWatch(false);
        }
    } else if (command == "disconnect_now") {
        DisconnectStream();
    } else if (command == "shutdown") {
        if (window_) {
            ::PostMessageW(window_->hwnd(), WM_CLOSE, 0, 0);
        }
    } else if (command == "fullscreen") {
        ToggleBorderlessFullscreen();
    } else if (command == "ping") {
        if (window_) {
            bridge_.Publish(std::string("S {\"state\":\"pong\"}"));
        }
    }
}

void NextApp::ReloadSettingsFromDisk() {
    AppSettings fresh;
    std::vector<std::string> warnings;
    if (host_->Config() && host_->Config()->Load(fresh, warnings).ok()) {
        settings_ = fresh;
        Apply();
    }
}

void NextApp::PublishBridgeStatus() {
    if (++link_ticks_ >= 40) {
        link_ticks_ = 0;
        const std::string link = NetworkLinkName();
        if (link != link_type_) {
            link_type_ = link;
            LogPrintf(LogLevel::Info, "[net] default route link=%s",
                      link_type_.c_str());
        }
    }
    const StreamSnapshot stream = streaming_ ? streaming_->Snapshot() : StreamSnapshot{};
    const bool streaming_now = watching_ && stream.connected;
    // The shell side of the story: what the user actually sees. Every ~5 s at
    // Debug so a stutter can be tied to network loss, decoder time or
    // presentation misses instead of guesswork.
    if (++stats_ticks_ >= 50) {
        stats_ticks_ = 0;
        char line[360] = {};
        std::snprintf(
            line, sizeof(line),
            "present: state=%s connected=%d, source %.1f fps, shown %d fps, "
            "loss %.2f%%, drops %d, decode %.1f/%.1f ms, age %.1f ms, "
            "cadence %.1f/%.1f ms, misses %llu/s, link=%s, input %.1f/%.1f ms",
            stream.state.c_str(), stream.connected ? 1 : 0, stream.source_fps,
            display_rate_, stream.loss_percent, stream.decoder_drops,
            stream.decode_ms, stream.decode_ms_max, last_age_ms_, cadence_p50_,
            cadence_p99_, present_misses_rate_,
            link_type_.empty() ? "unknown" : link_type_.c_str(),
            streaming_ ? streaming_->InputResponseMs() : 0.0,
            streaming_ ? streaming_->InputResponseAvgMs() : 0.0);
        LogDebug(line);
    }
    std::string json;
    json.reserve(512);
    json += "S {";
    auto text = [&json](const char* key, const std::string& value, bool comma) {
        json += '"';
        json += key;
        json += "\":\"";
        for (const char ch : value) {
            if (ch == '"' || ch == '\\') {
                json += '\\';
                json += ch;
            } else if (ch == '\n') {
                // Release notes are multi-line: keep the line breaks so the
                // shell can lay them out instead of showing one long run.
                json += "\\n";
            } else if (ch == '\r') {
                continue;
            } else if (ch == '\t') {
                json += "\\t";
            } else if (static_cast<unsigned char>(ch) >= 0x20) {
                json += ch;
            } else {
                json += ' ';
            }
        }
        json += '"';
        if (comma) {
            json += ',';
        }
    };
    auto number = [&json](const char* key, double value, bool comma) {
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%s\":%.3f", key, value);
        json += '"';
        json += buffer;
        if (comma) {
            json += ',';
        }
    };
    auto integer = [&json](const char* key, long long value, bool comma) {
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%s\":%lld", key, value);
        json += '"';
        json += buffer;
        if (comma) {
            json += ',';
        }
    };
    auto flag = [&json](const char* key, bool value, bool comma) {
        json += '"';
        json += key;
        json += value ? "\":true" : "\":false";
        if (comma) {
            json += ',';
        }
    };

    text("state", stream.state.empty() ? "idle" : stream.state, true);
    flag("connected", stream.connected, true);
    flag("streaming", streaming_now, true);
    flag("connecting", stream.connecting, true);
    flag("armed", disconnect_armed_, true);
    flag("has_credentials", stream.has_credentials, true);
    flag("discovering", stream.discovering, true);
    flag("pairing", stream.pairing, true);
    text("pairing_message", stream.pairing_message, true);
    text("psn_id", streaming_ ? streaming_->Profile().psn_numeric_id : std::string(), true);
    text("message", stream.message, true);
    const std::string console_name =
        streaming_ && !streaming_->Profile().name.empty() ? streaming_->Profile().name
                                                          : settings_.console.name;
    text("console_name", console_name, true);
    text("console_address",
         !settings_.console.address.empty()
             ? settings_.console.address
             : (streaming_ ? streaming_->Profile().host : std::string()), true);
    text("codec", stream.codec, true);
    {
        const UpdateStatus update = update_.Snapshot();
        text("update_state", UpdateStateName(update.phase), true);
        text("update_version", update.version, true);
        text("update_notes", update.notes, true);
        text("update_url", update.page_url, true);
        text("update_error", update.error, true);
        text("update_asset", update.asset_path, true);
        number("update_progress", update.progress, true);
        if (update.phase != last_update_phase_) {
            last_update_phase_ = update.phase;
            LogPrintf(LogLevel::Info, "[update] %s version=%s error=%s",
                      UpdateStateName(update.phase).c_str(), update.version.c_str(),
                      update.error.c_str());
        }
    }
    integer("width", stream.source_width, true);
    integer("height", stream.source_height, true);
    number("fps", stream.source_fps, true);
    number("content_fps", content_fps_, true);
    number("bitrate", stream.bitrate_kbps, true);
    number("loss", stream.loss_percent, true);
    number("jitter", stream.jitter_ms, true);
    number("decode_ms", stream.decode_ms, true);
    number("decode_max", stream.decode_ms_max, true);
    number("loop_p99", loop_p99_, true);
    integer("drops", stream.decoder_drops, true);
    number("cadence_p50", cadence_p50_, true);
    number("cadence_p99", cadence_p99_, true);
    number("scanout_hz", present_refresh_hz_, true);
    number("present_misses", static_cast<double>(present_misses_rate_), true);
    number("shown_fps", static_cast<double>(display_rate_), true);
    number("age_ms", last_age_ms_, true);
    number("present_lag_ms",
           stream_dx12_ ? stream_dx12_->present_lag_ms()
                        : (ActivePresenter() ? ActivePresenter()->present_lag_ms() : 0.0),
           true);
    number("input_ms", streaming_ ? streaming_->InputResponseMs() : 0.0, true);
    number("input_avg_ms",
           streaming_ ? streaming_->InputResponseAvgMs() : 0.0, true);
    flag("restart_pending", stream_restart_pending_, true);
    number("sr_ms", dlss_sr_ ? dlss_sr_->last_ms() : (fsr_ ? fsr_->last_ms() : 0.0), true);
    number("fg_ms", dlss_fg_dx12_ ? dlss_fg_dx12_->last_ms()
                                  : (dlss_fg_ ? dlss_fg_->process_ms() : 0.0),
           true);
    flag("dlss_fg_ready", system_.dlss_fg, true);
    flag("dlss_fg_multi", system_.dlss_fg_multi, true);
    flag("dlss_sr_ready", system_.dlss_sr, true);
    flag("fsr_sr_ready", system_.fsr_sr, true);
    flag("fsr_fg_ready", system_.fsr_fg, true);
    flag("dx12_surface", stream_dx12_ != nullptr, true);
    text("sr_backend", settings_.ai.sr_model, true);
    text("fg_backend", std::string(ToString(settings_.ai.fg_backend)), true);
    text("sr_provider", fsr_provider_.empty() ? (dlss_ready_ ? "DLSS" : "") : fsr_provider_, true);
    text("fsr_provider", fsr_provider_, true);
    flag("nr_runtime", system_.nr_worker, true);
    flag("nr_ready", nr_ready_, true);
    number("nr_ms", nr_ms_, true);
    text("gpu", system_.gpu.name, true);
    text("driver", system_.gpu.driver, true);
    text("link", link_type_, true);
    integer("vram_mb", static_cast<long long>(system_.gpu.vram_mb), false);
    json += '}';
    bridge_.Publish(std::move(json));
    if (streaming_ && !stream.discovering) {
        const auto consoles = streaming_->Discovered();
        std::string sig;
        for (const auto& candidate : consoles) {
            sig += candidate.address + "|" + candidate.name + "|" + candidate.status + ";";
        }
        if (sig != consoles_sig_) {
            consoles_sig_ = sig;
            PublishConsoles();
        }
    }
}

void NextApp::PublishConsoles() {
    if (!streaming_) {
        return;
    }
    const auto consoles = streaming_->Discovered();
    std::string json = "E {\"kind\":\"consoles\",\"items\":[";
    for (std::size_t i = 0; i < consoles.size(); ++i) {
        if (i > 0) {
            json += ',';
        }
        json += "{\"name\":\"" + JsonEscape(consoles[i].name) + "\",\"address\":\"" +
                JsonEscape(consoles[i].address) + "\",\"type\":\"" +
                JsonEscape(consoles[i].device_type) + "\",\"status\":\"" +
                JsonEscape(consoles[i].status) + "\"}";
    }
    json += "]}";
    bridge_.Publish(std::move(json));
}

void NextApp::PublishDevices() {
    auto escape = [](const std::string& text) {
        std::string out;
        out.reserve(text.size() + 8);
        for (const char ch : text) {
            if (ch == '"' || ch == '\\') {
                out += '\\';
                out += ch;
            } else if (static_cast<unsigned char>(ch) >= 0x20) {
                out += ch;
            }
        }
        return out;
    };

    std::string json = "E {\"kind\":\"devices\",\"audio\":[";
    const std::vector<brp::audio::DeviceInfo> audio_devices =
        brp::audio::Engine::Enumerate();
    for (std::size_t i = 0; i < audio_devices.size(); ++i) {
        if (i != 0) {
            json += ',';
        }
        json += "{\"id\":\"";
        json += escape(audio_devices[i].id);
        json += "\",\"name\":\"";
        json += escape(audio_devices[i].name);
        json += "\",\"default\":";
        json += audio_devices[i].is_default ? "true" : "false";
        json += '}';
    }

    json += "],\"monitors\":[";
    for (std::size_t i = 0; i < system_.displays.size(); ++i) {
        const DisplayInfo& display = system_.displays[i];
        if (i != 0) {
            json += ',';
        }
        char buffer[512];
        std::snprintf(buffer, sizeof(buffer),
                      "{\"index\":%zu,\"name\":\"%s\",\"width\":%d,\"height\":%d,"
                      "\"hz\":%d,\"primary\":%s}",
                      i, escape(Narrow(display.name)).c_str(), display.width,
                      display.height, display.refresh_hz,
                      display.primary ? "true" : "false");
        json += buffer;
    }

    json += "],\"gpus\":[";
    const std::vector<std::string> adapters = EnumerateAdapterNames();
    for (std::size_t i = 0; i < adapters.size(); ++i) {
        if (i != 0) {
            json += ',';
        }
        json += '"';
        json += escape(adapters[i]);
        json += '"';
    }
    json += "]}";
    bridge_.Publish(std::move(json));
}

void NextApp::PublishProfiles() {
    auto escape = [](const std::string& text) {
        std::string out;
        for (const char ch : text) {
            if (ch == '"' || ch == '\\') {
                out += '\\';
                out += ch;
            } else if (static_cast<unsigned char>(ch) >= 0x20) {
                out += ch;
            }
        }
        return out;
    };
    std::vector<Profile> profiles;
    if (host_->Profiles()) {
        host_->Profiles()->List(profiles);
    }
    std::string json = "E {\"kind\":\"profiles\",\"items\":[";
    for (std::size_t i = 0; i < profiles.size(); ++i) {
        if (i != 0) {
            json += ',';
        }
        json += "{\"id\":\"";
        json += escape(profiles[i].id);
        json += "\",\"name\":\"";
        json += escape(profiles[i].name);
        json += "\",\"default\":";
        json += profiles[i].is_default ? "true" : "false";
        json += '}';
    }
    json += "]}";
    bridge_.Publish(std::move(json));
}

void NextApp::ApplyProfileById(const std::string& id) {
    if (!host_->Profiles()) {
        return;
    }
    std::vector<Profile> profiles;
    host_->Profiles()->List(profiles);
    for (const Profile& profile : profiles) {
        if (profile.id == id) {
            settings_ = profile.settings;
            Apply();
            overlay_status_ = L"Profile applied: " + Widen(profile.name);
            overlay_status_error_ = false;
            last_overlay_update_ = -1.0;
            break;
        }
    }
}

void NextApp::SaveProfileByName(const std::string& requested) {
    if (!host_->Profiles()) {
        return;
    }
    std::vector<Profile> existing;
    host_->Profiles()->List(existing);
    if (existing.size() >= kMaxProfiles) {
        overlay_status_ = L"Profile limit reached (" + std::to_wstring(kMaxProfiles) +
                          L"). Delete one first.";
        overlay_status_error_ = true;
        last_overlay_update_ = -1.0;
        Toast(L"Profile limit reached. Delete one first.", true);
        return;
    }
    // The shell sends a bare `profile_save`: the engine owns uniqueness so two
    // clicks can never overwrite the same "Profile 2" file.
    std::string name = requested;
    if (name.empty()) {
        for (std::size_t index = 1; name.empty(); ++index) {
            const std::string candidate = "Profile " + std::to_string(index);
            const bool taken = std::any_of(
                existing.begin(), existing.end(),
                [&candidate](const Profile& profile) { return profile.name == candidate; });
            if (!taken) {
                name = candidate;
            }
        }
    }
    Profile profile;
    profile.id = MakeProfileId(name);
    profile.name = name;
    profile.settings = host_->Settings();
    profile.created_at = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    profile.updated_at = profile.created_at;
    if (host_->Profiles()->Save(profile).ok()) {
        publish_profiles_ = true;
        overlay_status_ = L"Profile saved: " + Widen(name);
        overlay_status_error_ = false;
        last_overlay_update_ = -1.0;
    }
}

void NextApp::DeleteProfileById(const std::string& id) {
    if (!host_->Profiles() || id.empty()) {
        return;
    }
    std::vector<Profile> existing;
    host_->Profiles()->List(existing);
    std::string name;
    for (const Profile& profile : existing) {
        if (profile.id == id) {
            name = profile.name;
            break;
        }
    }
    const Status status = host_->Profiles()->Delete(id);
    if (!status.ok()) {
        overlay_status_ = L"Delete failed: " + Widen(status.message());
        overlay_status_error_ = true;
        last_overlay_update_ = -1.0;
        Toast(L"Could not delete the profile", true);
        return;
    }
    publish_profiles_ = true;
    overlay_status_ = name.empty() ? L"Profile deleted" : L"Profile deleted: " + Widen(name);
    overlay_status_error_ = false;
    last_overlay_update_ = -1.0;
    Toast(L"Profile deleted");
}

void NextApp::ToggleBorderlessFullscreen() {
    if (watching_ && stream_window_) {
        stream_window_->ToggleFullscreen();
        watch_fullscreen_ = stream_window_->fullscreen();
        ResizeStream();
        return;
    }
    if (!window_) {
        return;
    }
    if (window_->fullscreen()) {
        window_->ExitFullscreen();
    } else {
        window_->EnterFullscreen();
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
    file << "  \"app\": \"Veyla\",\n";
    file << "  \"version\": \"" << Narrow(kVersion).c_str() << "\",\n";
    file << "  \"source\": \"" << stream.console_address << "\",\n";
    file << "  \"state\": \"" << stream.state << "\",\n";
    file << "  \"connected\": " << (stream.connected ? "true" : "false") << ",\n";
    file << "  \"source_fps\": " << stream.source_fps << ",\n";
    file << "  \"decode_ms\": " << stream.decode_ms << ",\n";
    file << "  \"bitrate_kbps\": " << stream.bitrate_kbps << ",\n";
    file << "  \"loss_percent\": " << stream.loss_percent << ",\n";
    file << "  \"displayed_per_s\": " << display_rate_ << ",\n";
    file << "  \"input_loop_ms\": " << (streaming_ ? streaming_->InputResponseAvgMs() : 0.0)
         << ",\n";
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
    file << "  \"ui_frames\": " << ui_frames_ << "\n";
    file << "}\n";
    Toast(L"Session report: " + path.wstring());
}
void NextApp::ApplyPresenterTarget() {
    if (!presenter_) {
        return;
    }
    // The stream presenter is created later than the main one; apply the target
    // to it every time so a newly created surface is never left at 1:1.
    ApplyTargetTo(stream_presenter_.get());
    const int target = settings_.video.render_target;
    if (target == applied_render_target_) {
        return;
    }
    ApplyTargetTo(presenter_.get());
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
    result.generated = 0;
    bench_results_.push_back(result);
    ToggleWatch(false);
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
    file << "  \"app\": \"Veyla\",\n";
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
    content_changed_ = true;
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


