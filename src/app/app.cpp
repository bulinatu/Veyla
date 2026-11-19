#include "app/app.h"

#include "media/rgba_scale.h"
#include "session/discovery.h"
#include "platform/win32_window.h"
#include "ui/theme.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <avrt.h>

using namespace brp;
namespace th = brp::theme;

namespace {

std::wstring Widen(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                        nullptr, 0);
    std::wstring wide(static_cast<size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), wide.data(), n);
    return wide;
}

std::string EnvString(const char* name) {
    const char* value = std::getenv(name);
    return value ? std::string(value) : std::string();
}

double NowMs() {
    using clock = std::chrono::steady_clock;
    static const clock::time_point start = clock::now();
    return std::chrono::duration<double, std::milli>(clock::now() - start).count();
}

// RTX VSR has been removed: the app now uses DLSS Super Resolution/DLAA,
// DLSS Neural Rendering and DLSS Frame Generation only.

int DlssPerfQuality(int mode) {
    // The NGX PerfQuality value is only a hint, and not every value is valid
    // together with the model preset hints (UltraQuality rejected preset 13
    // with 0xBAD00010). The mode's real effect on this app is the input scale
    // computed in DlssInputSize, so keep the three tested values.
    switch (mode) {
        case 0:             // Ultra Performance 33%
        case 1: return 0;   // Performance 50%
        case 2: return 1;   // Balanced 58%
        default: return 2;  // Quality 66%, Ultra Quality 77%, DLAA
    }
}

enum class Icon {
    Gear,
    Back,
    Power,
    Display,
    Upscaling,
    Performance,
    Neural,
    Audio,
    Controller,
    Connection,
    Debug,
    About,
    Color,
};

// Small geometric icon set. Everything is drawn with D2D primitives so the
// navigation stays crisp at every UI scale and no image assets are needed.
void DrawIcon(ui::Renderer& ui, Icon icon, float cx, float cy, float s, D2D1_COLOR_F color) {
    const float weight = std::max(1.2f, s * 0.16f);
    switch (icon) {
        case Icon::Gear:
            // Standard gear silhouette: thick body ring, eight wide teeth and
            // an inner hole. Thin radial rays read as a sun, so the teeth are
            // short and nearly as thick as the body.
            for (int i = 0; i < 8; ++i) {
                const float angle = static_cast<float>(i) * 3.14159265f / 4.0f;
                const float inner = s * 0.52f;
                const float outer = s * 1.02f;
                ui.Line({cx + std::cos(angle) * inner, cy + std::sin(angle) * inner},
                        {cx + std::cos(angle) * outer, cy + std::sin(angle) * outer}, color,
                        std::max(2.0f, s * 0.34f));
            }
            ui.StrokeCircle(cx, cy, s * 0.60f, color, std::max(2.2f, s * 0.34f));
            ui.StrokeCircle(cx, cy, s * 0.27f, color, std::max(1.2f, s * 0.13f));
            break;
        case Icon::Back:
            ui.Line({cx + s * 0.35f, cy - s * 0.55f}, {cx - s * 0.35f, cy}, color,
                    std::max(1.4f, s * 0.20f));
            ui.Line({cx - s * 0.35f, cy}, {cx + s * 0.35f, cy + s * 0.55f}, color,
                    std::max(1.4f, s * 0.20f));
            break;
        case Icon::Power:
            ui.StrokeArc(cx, cy, s * 0.72f, -1.047f, 5.236f, color, std::max(1.4f, s * 0.20f));
            ui.Line({cx, cy - s * 0.95f}, {cx, cy - s * 0.20f}, color,
                    std::max(1.4f, s * 0.20f));
            break;
        case Icon::Display:
            ui.StrokeRounded({cx - s, cy - s * 0.72f, cx + s, cy + s * 0.42f}, s * 0.18f, color,
                             weight);
            ui.Line({cx, cy + s * 0.42f}, {cx, cy + s * 0.72f}, color, weight);
            ui.Line({cx - s * 0.45f, cy + s * 0.72f}, {cx + s * 0.45f, cy + s * 0.72f}, color,
                    weight);
            break;
        case Icon::Upscaling: {
            const float l = s * 0.38f;
            const float w = weight;
            ui.Line({cx - s, cy - s + l}, {cx - s, cy - s}, color, w);
            ui.Line({cx - s, cy - s}, {cx - s + l, cy - s}, color, w);
            ui.Line({cx + s, cy - s + l}, {cx + s, cy - s}, color, w);
            ui.Line({cx + s, cy - s}, {cx + s - l, cy - s}, color, w);
            ui.Line({cx - s, cy + s - l}, {cx - s, cy + s}, color, w);
            ui.Line({cx - s, cy + s}, {cx - s + l, cy + s}, color, w);
            ui.Line({cx + s, cy + s - l}, {cx + s, cy + s}, color, w);
            ui.Line({cx + s, cy + s}, {cx + s - l, cy + s}, color, w);
            break;
        }
        case Icon::Performance:
            ui.StrokeArc(cx, cy, s * 0.78f, 2.4f, 3.3f, color, std::max(1.4f, s * 0.20f));
            ui.Line({cx, cy}, {cx + s * 0.45f, cy - s * 0.35f}, color, std::max(1.2f, s * 0.16f));
            ui.FillCircle(cx, cy, s * 0.13f, color);
            break;
        case Icon::Neural:
            ui.FillTriangle({cx, cy - s}, {cx + s * 0.25f, cy - s * 0.15f}, {cx, cy + s * 0.15f},
                            color);
            ui.FillTriangle({cx, cy - s}, {cx - s * 0.25f, cy - s * 0.15f}, {cx, cy + s * 0.15f},
                            color);
            ui.FillTriangle({cx - s, cy}, {cx - s * 0.15f, cy - s * 0.25f},
                            {cx - s * 0.15f, cy + s * 0.25f}, color);
            ui.FillTriangle({cx + s, cy}, {cx + s * 0.15f, cy - s * 0.25f},
                            {cx + s * 0.15f, cy + s * 0.25f}, color);
            break;
        case Icon::Audio:
            ui.FillRect({cx - s * 0.78f, cy - s * 0.28f, cx - s * 0.38f, cy + s * 0.28f}, color);
            ui.FillTriangle({cx - s * 0.38f, cy - s * 0.28f}, {cx, cy - s * 0.66f},
                            {cx, cy + s * 0.66f}, color);
            ui.StrokeArc(cx + s * 0.05f, cy, s * 0.42f, -0.9f, 1.8f, color, std::max(1.1f, s * 0.15f));
            ui.StrokeArc(cx + s * 0.05f, cy, s * 0.70f, -0.9f, 1.8f, color, std::max(1.0f, s * 0.13f));
            break;
        case Icon::Controller:
            ui.StrokeRounded({cx - s, cy - s * 0.58f, cx + s, cy + s * 0.58f}, s * 0.35f, color,
                             weight);
            ui.Line({cx - s * 0.55f, cy - s * 0.18f}, {cx - s * 0.55f, cy + s * 0.18f}, color,
                    weight);
            ui.Line({cx - s * 0.73f, cy}, {cx - s * 0.37f, cy}, color, weight);
            ui.FillCircle(cx + s * 0.50f, cy - s * 0.14f, s * 0.12f, color);
            ui.FillCircle(cx + s * 0.72f, cy + s * 0.14f, s * 0.12f, color);
            break;
        case Icon::Connection:
            ui.FillCircle(cx, cy + s * 0.55f, s * 0.13f, color);
            ui.StrokeArc(cx, cy + s * 0.55f, s * 0.42f, -2.2f, 1.25f, color,
                         std::max(1.1f, s * 0.15f));
            ui.StrokeArc(cx, cy + s * 0.55f, s * 0.72f, -2.2f, 1.25f, color,
                         std::max(1.0f, s * 0.13f));
            break;
        case Icon::Debug:
            ui.StrokeRounded({cx - s, cy - s * 0.70f, cx + s, cy + s * 0.70f}, s * 0.20f, color,
                             std::max(1.2f, s * 0.14f));
            ui.Line({cx - s * 0.55f, cy - s * 0.22f}, {cx - s * 0.20f, cy}, color, weight);
            ui.Line({cx - s * 0.20f, cy}, {cx - s * 0.55f, cy + s * 0.22f}, color, weight);
            ui.Line({cx - s * 0.02f, cy + s * 0.25f}, {cx + s * 0.45f, cy + s * 0.25f}, color,
                    weight);
            break;
        case Icon::About:
            ui.StrokeCircle(cx, cy, s * 0.80f, color, weight);
            ui.FillCircle(cx, cy - s * 0.32f, s * 0.10f, color);
            ui.Line({cx, cy - s * 0.05f}, {cx, cy + s * 0.45f}, color, weight);
            break;
        case Icon::Color:
            // Three overlapping discs: a palette without an image asset.
            ui.FillCircle(cx - s * 0.34f, cy - s * 0.18f, s * 0.42f, color);
            ui.FillCircle(cx + s * 0.34f, cy - s * 0.18f, s * 0.42f, color);
            ui.FillCircle(cx, cy + s * 0.30f, s * 0.42f, color);
            break;
    }
}

const std::vector<std::wstring>& UpscalerLabels() {
    static const std::vector<std::wstring> labels = {
        L"Bilinear", L"Catmull-Rom", L"Catmull-Rom + Sharpen", L"Lanczos + RCAS",
        L"FSR1 (EASU)", L"DLSS Super Resolution (D3D12)"};
    return labels;
}

enum WidgetId {
    W_Upscaler = 1,
    W_Hud,
    W_FrameGen,
    W_Decoder = 53,
    W_Chroma = 55,
    W_Hdr,
    W_AudioDevice = 10,
    W_AudioBuffer,
    W_TestTone,
    W_InputSource = 20,
    W_Lightbar,
    W_Trigger,
    W_Strength,
    W_Rumble,
    W_PlayerLeds,
    W_MicLed,
    W_Probe = 40,
    W_Pair = 41,
    W_Bitrate = 43,
    W_Resolution = 44,
    W_Fps = 45,
    W_Disconnect = 46,
    W_FrameBuffer = 47,
    W_PadRate = 48,
    W_ConsoleRumble = 49,
    W_ConsoleTriggers = 50,
    W_Motion = 51,
    W_RenderTarget = 52,
    // One-click launch fallbacks (Performance tab).
    W_PresetLaunch = 110,
    W_PresetSafe,
    W_PresetSr,
    W_PresetLow,
    // Display enhance (Colour tab). 120-136; 110-119 stay free for presets.
    W_ColorOn = 120,    W_ColorExposure,
    W_ColorContrast,
    W_ColorHighlights,
    W_ColorShadows,
    W_ColorSaturation,
    W_ColorVibrance,
    W_ColorHue,
    W_ColorTemperature,
    W_ColorTint,
    W_ColorGamma,
    W_Sharpness,
    W_ColorReset,
    W_ColorNeutral,
    W_ColorVivid,
    W_ColorCinematic,
    W_ColorWarm,
    W_PairHome = 42,
    W_DlssPreset = 63,
    W_DlssMode = 64,
    W_Reflex = 65,
    W_DlssFg = 79,
    W_FgTarget = 78,
    W_FgEngine = 86,
    W_RifeOutput = 87,
    W_NrOrder = 66,
    W_NeuralNr = 67,
    W_NrPasses = 68,
    W_NrStyle = 69,
    W_NrTone = 70,
    W_NrStructure = 71,
    W_NrSkin = 72,
    W_NrUi = 73,
    W_NrScale = 74,
    W_NrMask = 76,
    W_NrIntensity = 77,
    W_ResumeOverlay = 60,
    W_HudFrames = 80,
    W_HudGpu = 81,
    W_HudPad = 82,
    W_HudStream = 83,
    W_HudNet = 84,
    W_HudReset = 85,
    // RIFE one-click setup (Home pill and its modal).
    W_SetupRife = 96,
    W_SetupStart,
    W_SetupClose,
    W_HapticIntensity = 99,
    W_PsnLogin = 90,
    W_PsnOpen,
    W_PsnPaste,
    W_PsnSave,
    W_PsnCancel,
    W_AddressOpen = 100,
    W_AddressDiscover,
    W_AddressPaste,
    W_AddressSave,
    W_AddressCancel,
};

constexpr float kContentX = 344.0f;
constexpr float kRowH = 66.0f;

// Custom window chrome (windowed mode only; fullscreen draws none).
constexpr float kTitleBarH = 48.0f;
constexpr float kTitleBarButtonW = 46.0f;
constexpr float kCornerButton = 44.0f;
constexpr float kCornerMargin = 24.0f;
// The app's own buttons stay clickable; everything left of this strip drags
// the window through WM_NCHITTEST.
constexpr float kTitleBarClientRight = 344.0f;

const wchar_t* kTriggerNames[] = {L"Off",    L"Rigid",     L"Pulse", L"Feedback",
                                  L"Weapon", L"Vibration", L"Bow",   L"Galloping"};
constexpr int kTriggerCount = static_cast<int>(std::size(kTriggerNames));

const wchar_t* kLightbarNames[] = {L"Blue", L"Red", L"Green", L"Amber",
                                   L"Violet", L"Teal", L"White", L"Off",
                                   L"Console (PS5)"};
constexpr int kLightbarCount = static_cast<int>(std::size(kLightbarNames));

struct Rgb {
    uint8_t r, g, b;
};

const Rgb kLightbarColors[] = {
    {0x5A, 0xA9, 0xFF}, {0xFF, 0x4D, 0x4D}, {0x57, 0xD6, 0x8A}, {0xFF, 0xC1, 0x45},
    {0xB1, 0x4B, 0xFF}, {0x14, 0xB8, 0xA6}, {0xF4, 0xF7, 0xFC}, {0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00},
};

const wchar_t* kPlayerLedNames[] = {L"Off",   L"Center", L"Inner",
                                    L"Outer", L"All",    L"Mid",
                                    L"Console (PS5)"};
const uint8_t kPlayerLedMasks[] = {0x00, 0x04, 0x0A, 0x0F, 0x1F, 0x15, 0x00};
constexpr int kPlayerLedCount = static_cast<int>(std::size(kPlayerLedMasks));

const wchar_t* kMicLedNames[] = {L"Off", L"On", L"Pulse"};

const wchar_t* kInputSourceNames[] = {L"Auto (native first)", L"Native HID only",
                                      L"Steam Input / XInput"};
const Gamepad::Backend kInputBackends[] = {Gamepad::Backend::Auto, Gamepad::Backend::SonyHid,
                                           Gamepad::Backend::XInput};

const int kBufferSizes[] = {10, 20, 30, 40};

int BufferIndex(int ms) {
    for (int i = 0; i < 4; ++i) {
        if (kBufferSizes[i] == ms) {
            return i;
        }
    }
    return 1;
}

// Shared by the dropdown label and the click handler so the two can never
// drift apart (they used to be two hand-written arrays).
constexpr int kBitrates[] = {20000, 35000, 50000, 75000, 100000};
constexpr int kBitrateCount = static_cast<int>(std::size(kBitrates));

int BitrateIndex(int kbps) {
    for (int i = 0; i < kBitrateCount; ++i) {
        if (kBitrates[i] == kbps) {
            return i;
        }
    }
    return 2;  // 50 Mbps
}

constexpr int kPadRates[] = {60, 125, 250, 500, 1000};
constexpr int kPadRateCount = static_cast<int>(std::size(kPadRates));

int PadRateIndex(int hz) {
    for (int i = 0; i < kPadRateCount; ++i) {
        if (kPadRates[i] == hz) {
            return i;
        }
    }
    return 2;  // 250 Hz
}

TriggerEffect BuildTriggerEffect(int effect, int strength) {
    switch (effect) {
        case 1: return TriggerEffect::Rigid(0, static_cast<uint8_t>(strength));
        case 2: return TriggerEffect::Pulse(0, 9, static_cast<uint8_t>(strength));
        case 3: return TriggerEffect::Feedback(2, static_cast<uint8_t>(strength));
        case 4: return TriggerEffect::Weapon(2, 8, static_cast<uint8_t>(strength));
        case 5:
            return TriggerEffect::Vibration(0x90, static_cast<uint8_t>(strength * 28));
        case 6:
            return TriggerEffect::Bow(2, 8, static_cast<uint8_t>(strength),
                                      static_cast<uint8_t>(std::max(1, strength / 2)));
        case 7:
            return TriggerEffect::Galloping(0, 9, 2, 7, static_cast<uint8_t>(strength * 30));
        default: return TriggerEffect::Off();
    }
}

std::wstring PadStatus(const PadState& pad) {
    if (!pad.connected) {
        return L"No controller detected";
    }
    std::wstring text = Widen(pad.name);
    if (pad.battery_percent >= 0) {
        wchar_t buffer[64];
        swprintf_s(buffer, L"   ·   %d%%%s", pad.battery_percent,
                   pad.charging ? L" (charging)" : L"");
        text += buffer;
    }
    return text;
}

}  // namespace

bool App::Init(Win32Window* window) {
    window_ = window;
    if (window_) {
        refresh_hz_ = window_->refresh_hz();
        std::fprintf(stderr, "[app] display refresh %d Hz\n", refresh_hz_);
    }


    const std::string session_path = session::DefaultProfilePath();
    std::string session_detail;
    if (session::Load(session_path, profile_, session_detail)) {
        session_note_ = "Loaded session file: " + profile_.display_name();
    } else if (session::ImportPXPlay(EnvString("APPDATA"), profile_, session_detail)) {
        std::string save_detail;
        session::Save(profile_, session_path, save_detail);
        session_note_ =
            "Imported console identity and PSN account from PXPlay. Registration keys stay "
            "encrypted in PXPlay; pairing needs the 8-digit console code.";
    } else {
        session_note_ = "No console configured yet.";
    }

    // Apply persisted UI settings.
    hdr_ = profile_.hdr;
    upscale_ = std::clamp(profile_.upscaler, 0, 4);
    hud_ = profile_.hud;
    audio_buffer_ms_ = std::clamp(profile_.audio_buffer_ms, 10, 100);
    input_source_ = std::clamp(profile_.input_source, 0, 2);
    stream_buffer_ = std::clamp(profile_.frame_buffer, 0, 3);
    pad_rate_hz_ = std::clamp(profile_.pad_rate_hz, 60, 1000);
    lightbar_index_ = std::clamp(profile_.lightbar, 0, kLightbarCount - 1);
    trigger_effect_ = std::clamp(profile_.trigger_effect, 0, kTriggerCount - 1);
    trigger_strength_ = std::clamp(profile_.trigger_strength, 1, 8);
    player_leds_ = std::clamp(profile_.player_leds, 0, kPlayerLedCount - 1);
    mic_led_ = std::clamp(profile_.mic_led, 0, 2);
    console_rumble_ = profile_.console_rumble != 0;
    console_triggers_ = profile_.console_triggers != 0;
    motion_ = profile_.motion != 0;
    streaming_.SetMotionEnabled(motion_);
    th::SetHdr(hdr_);
    ApplyEnhance();
    {
        const std::filesystem::path profile_path = session::DefaultProfilePath();
        health_path_ = (profile_path.parent_path() / "health.log").string();
    }

    pad_.SetBackend(kInputBackends[input_source_]);

    PresenterConfig cfg{};
    cfg.output_width = window->width();
    cfg.output_height = window->height();
    cfg.source_width = 1920;
    cfg.source_height = 1080;
    cfg.vsync = true;
    cfg.allow_tearing = true;
    cfg.hdr = hdr_;
    cfg.upscale_mode = upscale_;

    if (!presenter_.Init(window->hwnd(), cfg)) {
        return false;
    }
    if (!ui_.Init(presenter_.device())) {
        return false;
    }
    if (!ui_.BindSwapchain(presenter_.backbuffer())) {
        return false;
    }

    // HID device enumeration (every two seconds while no controller is
    // attached) can block for ~60 ms. That work must never run on the
    // presentation thread, so the pad loop runs for the whole app lifetime,
    // not just during a stream.
    StartPadThread();

    // The logo intro plays first. Discovery and the NVIDIA/NGX startup are
    // kicked off when the boot screen appears, after the logo.
    return true;
}

void App::Resize(int width, int height) {
    if (width <= 0 || height <= 0) {
        return;
    }
    std::fprintf(stderr, "[app] client resize %dx%d borderless=%d\n", width, height,
                 borderless_ ? 1 : 0);
    ui_.ReleaseTarget();
    if (!presenter_.Resize(width, height)) {
        return;
    }
    ui_.BindSwapchain(presenter_.backbuffer());
}

void App::BuildBootTasks() {
    boot_tasks_.clear();

    boot_tasks_.push_back({L"Preparing audio", [this] {
        audio_devices_ = audio::Engine::Enumerate();
        if (!profile_.audio_device_id.empty()) {
            for (int i = 0; i < static_cast<int>(audio_devices_.size()); ++i) {
                if (audio_devices_[i].id == profile_.audio_device_id) {
                    audio_device_ = i;
                    break;
                }
            }
        }
        return true;
    }});

    boot_tasks_.push_back({L"Bringing up the video path", [this] {
        render_target_ = std::clamp(profile_.render_target, 0, 3);
        ApplyRenderTarget();
        ApplyReflex();
        return true;
    }});

    boot_tasks_.push_back({L"Checking NVIDIA runtimes", [this] {
        frame_gen_ = profile_.frame_gen >= 2 ? 2 : 0;
        nv_vfx_present_ = !FindFrameGenRuntime().empty();
        dlss_present_ = !FindDlssRuntime().empty();
        nr_present_ = !FindNrRuntime().empty();
        return true;
    }});

    if (profile_.dlss_fg >= 2) {
        boot_tasks_.push_back({L"Starting DLSS Frame Generation", [this] {
            ApplyDlssFrameGen();
            // The worker window is attached and hidden on the presentation
            // thread as soon as it exists; SetParent stays off the worker.
            {
                std::lock_guard<std::mutex> lock(boot_mutex_);
                boot_fg_present_started_ = nr_.present_mode();
            }
            return true;
        }});
    }

    if (profile_.neural_sr > 0) {
        boot_tasks_.push_back({L"Starting DLSS Super Resolution", [this] {
            if (profile_.neural_sr > 0) {
                if (dlss_present_ && EnsureDlssSrForTarget()) {
                    frame_gen_ = 0;
                    profile_.frame_gen = 0;
                } else {
                    profile_.neural_sr = 0;
                }
            }
            return true;
        }});
    }

    if (profile_.frame_gen >= 2 && profile_.dlss_fg < 2) {
        boot_tasks_.push_back({L"Starting NVIDIA VFG", [this] {
            if (profile_.neural_sr > 0) {
                frame_gen_ = 0;
            }
            if (frame_gen_ >= 2 && (!nv_vfx_present_ || !EnsureFrameGen())) {
                frame_gen_ = 0;
            }
            return true;
        }});
    }

    if (profile_.neural_nr > 0) {
        boot_tasks_.push_back({L"Starting Neural Rendering", [this] {
            if (profile_.neural_nr > 0) {
                if (nr_present_ && EnsureNr()) {
                    // NR is an enhancement stage; DLSS SR may run after it.
                    frame_gen_ = 0;
                    profile_.frame_gen = 0;
                } else {
                    profile_.neural_nr = 0;
                }
            }
            return true;
        }});
    }

    boot_tasks_.push_back({L"Finishing up", [] { return true; }});
    // +1 for the parallel LAN discovery step.
    boot_total_ = boot_tasks_.size() + 1;
}

void App::TickBoot(double t) {
    if (!boot_threads_started_) {
        boot_threads_started_ = true;
        boot_status_ = L"Loading session";

        discover_thread_ = std::thread([this] {
            std::vector<session::DiscoveredConsole> found = session::DiscoverLocal(1400);
            {
                std::lock_guard<std::mutex> lock(discover_mutex_);
                discovered_ = std::move(found);
            }
            discover_done_.store(true);
        });

        // Device creation, NGX/NR startup and file I/O never run on the
        // presentation thread; the boot screen keeps animating at full rate.
        BuildBootTasks();
        boot_worker_ = std::thread(&App::BootWorkerMain, this);
    }

    if (boot_started_ < 0.0) {
        boot_started_ = t;
        boot_last_ = t;
        boot_target_ = 0.05;
    }

    const double dt = boot_last_ < 0.0 ? 1.0 / 60.0 : std::clamp(t - boot_last_, 0.0, 0.1);
    boot_last_ = t;
    boot_status_alpha_ = std::min(1.0, boot_status_alpha_ + dt / 0.20);
    if (!pad_run_.load()) {
        pad_.Poll();
    }

    size_t worker_index = 0;
    bool worker_done = false;
    bool fg_started = false;
    std::wstring label;
    {
        std::lock_guard<std::mutex> lock(boot_mutex_);
        worker_index = boot_worker_index_;
        worker_done = boot_worker_done_;
        fg_started = boot_fg_present_started_;
        label = boot_worker_label_;
    }

    if (fg_started && !fg_window_) {
        // The worker's presenter exists now. Re-parent and hide it here, on the
        // UI thread, so it cannot flash over the boot screen.
        AttachFgWindowNow();
        if (fg_window_) {
            ::ShowWindow(fg_window_, SW_HIDE);
        }
    }

    const bool discovery_done = discover_done_.load() || (t - boot_started_ >= 6.0);
    const size_t completed =
        std::min(worker_index, boot_tasks_.size()) + (discovery_done ? 1 : 0);
    boot_done_steps_ = std::max(boot_done_steps_, completed);

    std::wstring status = label.empty() ? std::wstring(L"Loading session") : label;
    if (worker_done && !discovery_done) {
        status = L"Scanning for your console";
    }
    if (status != boot_status_) {
        boot_status_ = status;
        boot_status_alpha_ = 0.0;
    }

    if (!boot_finalized_) {
        if (worker_done && discovery_done && t - boot_started_ >= 2.1) {
            // The worker is done; joining is instant. The fade-out below keeps
            // the loader on screen while the intro takes over.
            if (boot_worker_.joinable()) {
                boot_worker_.join();
            }
            if (!boot_discovery_applied_) {
                boot_discovery_applied_ = true;
                std::vector<session::DiscoveredConsole> found;
                {
                    std::lock_guard<std::mutex> lock(discover_mutex_);
                    found = std::move(discovered_);
                }
                if (!found.empty()) {
                    profile_.host = found.front().address;
                    if (profile_.target_id.empty()) {
                        profile_.target_id = found.front().target_id;
                    }
                    if (profile_.name.empty()) {
                        profile_.name = found.front().name;
                    }
                    std::string save_detail;
                    session::Save(profile_, session::DefaultProfilePath(), save_detail);
                }
            }
            // Final FG presenter check, still on the UI thread.
            if (nr_.present_mode() && !fg_window_) {
                AttachFgWindowNow();
            }
            if (fg_window_) {
                ::ShowWindow(fg_window_, SW_HIDE);
            }
            if (std::getenv("BRP_START_FULLSCREEN") != nullptr && !borderless_) {
                ToggleFullscreen();
            }
            boot_done_at_ = t;
            boot_target_ = 1.0;
            boot_done_steps_ = boot_total_;
            boot_status_ = L"Ready";
            boot_status_alpha_ = 1.0;
            boot_finalized_ = true;
        } else {
            const size_t total = std::max<size_t>(1, boot_total_);
            const double fraction =
                static_cast<double>(completed) / static_cast<double>(total);
            boot_target_ = 0.06 + 0.86 * fraction;
            if (completed < total) {
                const double next = 0.06 + 0.86 * (static_cast<double>(completed + 1) /
                                                   static_cast<double>(total));
                boot_target_ = std::min(next - 0.02, boot_target_ + dt * 0.03);
            }
        }
    }

    if (boot_finalized_ && t - boot_done_at_ >= 0.45) {
        if (boot_worker_.joinable()) {
            boot_worker_.join();
        }
        booting_ = false;
        // The logo intro already played before the boot screen; Home follows.
        intro_ = false;
        intro_start_ = -1.0;
    }

    const float k = 1.0f - std::exp(-7.0f * static_cast<float>(dt));
    boot_shown_ += (boot_target_ - boot_shown_) * k;
}

void App::BootWorkerMain() {
    for (size_t i = 0; i < boot_tasks_.size(); ++i) {
        if (boot_cancel_.load()) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(boot_mutex_);
            boot_worker_index_ = i;
            boot_worker_label_ = boot_tasks_[i].label;
        }

        // Give the presentation thread a moment to show this status before a
        // heavy call (NGX init, worker process start).
        std::this_thread::sleep_for(std::chrono::milliseconds(70));

        bool done = false;
        while (!done) {
            if (boot_cancel_.load()) {
                return;
            }
            done = boot_tasks_[i].run();
            if (!done) {
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
            }
        }

        {
            std::lock_guard<std::mutex> lock(boot_mutex_);
            boot_worker_index_ = i + 1;
        }
    }

    {
        std::lock_guard<std::mutex> lock(boot_mutex_);
        boot_worker_done_ = true;
        boot_worker_label_ = L"Ready";
    }
}

void App::DrawLoaderMark(float cx, float cy, float reveal, float t, float alpha) {
    const float spin = static_cast<float>(t) * 1.15f;
    const float draw = std::clamp(reveal, 0.0f, 1.0f);

    ui_.FillRadial(cx, cy, 190.0f * draw, th::Rgba(0x6B32D9, 0.26f * alpha * draw),
                   th::Rgba(0x6B32D9, 0.0f));
    ui_.FillCircle(cx, cy, 92.0f + 6.0f * std::sin(static_cast<float>(t) * 2.0f),
                   th::Rgba(th::kAccent, 0.06f * alpha * draw));
    ui_.StrokeArc(cx, cy, 62.0f, spin, 4.55f * draw, th::Rgba(th::kAccent, 0.92f * alpha), 2.4f);
    ui_.StrokeArc(cx, cy, 76.0f, -spin * 0.65f + 0.8f, 2.15f * draw,
                  th::Rgba(th::kInk, 0.22f * alpha), 1.2f);
    ui_.StrokeCircle(cx, cy, 45.0f, th::Rgba(0xFFFFFF, 0.08f * alpha * draw), 1.0f);

    const D2D1_RECT_F tile{cx - 22.0f, cy - 22.0f, cx + 22.0f, cy + 22.0f};
    ui_.FillRounded(tile, 12.0f, th::Rgba(th::kAccent, 0.18f * alpha * draw));
    ui_.StrokeRounded(tile, 12.0f, th::Rgba(th::kAccentWarm, 0.45f * alpha * draw), 1.0f);
    ui_.Text(L"BRP", {cx - 30.0f, cy - 17.0f, cx + 30.0f, cy + 17.0f}, 14.0f,
             th::Rgba(th::kInk, 0.95f * alpha * draw), DWRITE_FONT_WEIGHT_SEMI_BOLD,
             DWRITE_TEXT_ALIGNMENT_CENTER);
}

void App::DrawLoaderSteps(const std::vector<std::pair<std::wstring, int>>& steps, float x,
                          float y, float w, float t, float alpha) {
    constexpr float kRow = 32.0f;
    for (size_t i = 0; i < steps.size(); ++i) {
        const float cy = y + kRow * static_cast<float>(i) + kRow * 0.5f;
        const int state = steps[i].second;  // 0 pending, 1 active, 2 done
        const float dx = x + 9.0f;
        if (state == 2) {
            ui_.FillCircle(dx, cy, 9.0f, th::Rgba(th::kAccent, 0.85f * alpha));
            ui_.FillCircle(dx, cy, 3.2f, th::Rgba(th::kInk, 0.95f * alpha));
        } else if (state == 1) {
            ui_.StrokeCircle(dx, cy, 9.0f, th::Rgba(th::kAccent, 0.22f * alpha), 2.0f);
            ui_.StrokeArc(dx, cy, 9.0f, static_cast<float>(t) * 7.0f, 4.2f,
                          th::Rgba(th::kAccent, 0.98f * alpha), 2.2f);
        } else {
            ui_.StrokeCircle(dx, cy, 9.0f, th::Rgba(0xFFFFFF, 0.16f * alpha), 1.0f);
        }
        ui_.Text(steps[i].first.c_str(), {x + 28.0f, cy - 13.0f, x + w, cy + 13.0f}, 13.0f,
                 th::Rgba(state == 0 ? th::kMuted : th::kInk,
                          (state == 0 ? 0.70f : 0.96f) * alpha),
                 state == 1 ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL);
    }
}

void App::DrawBrandMark(float cx, float cy, float reveal, float t, float alpha) {
    const float r = std::clamp(reveal, 0.0f, 1.0f);
    const float ease = 1.0f - (1.0f - r) * (1.0f - r) * (1.0f - r);
    const float k = alpha * ease;

    // Halo that swells as the mark arrives.
    ui_.FillRadial(cx, cy, 250.0f * ease, th::Rgba(0x7B3CFF, 0.34f * k),
                   th::Rgba(0x7B3CFF, 0.0f));

    // Three orbit rings turning at different speeds, with a rotating tick ring.
    const float spin = t * 0.9f;
    ui_.StrokeCircle(cx, cy, 104.0f, th::Rgba(0xFFFFFF, 0.05f * k), 1.0f);
    ui_.StrokeArc(cx, cy, 104.0f, spin, 1.10f * 2.0f * ease * 1.4f,
                  th::Rgba(th::kAccent, 0.95f * k), 2.6f);
    ui_.StrokeArc(cx, cy, 120.0f, -spin * 0.6f + 0.8f, 0.7f * 2.0f * ease * 1.4f,
                  th::Rgba(th::kAccentWarm, 0.75f * k), 1.6f);
    ui_.StrokeArc(cx, cy, 138.0f, spin * 0.3f, 2.2f * ease, th::Rgba(th::kInk, 0.16f * k), 1.0f);
    for (int i = 0; i < 12; ++i) {
        const float a = spin * 0.5f + static_cast<float>(i) * 0.5235988f;
        ui_.StrokeArc(cx, cy, 104.0f, a, 0.07f * ease, th::Rgba(th::kInk, 0.55f * k), 2.0f);
    }

    // Emblem: a rounded tile with a play mark that scales in.
    const float tile = 44.0f * ease + 2.0f;
    const D2D1_RECT_F tile_rect{cx - tile, cy - tile, cx + tile, cy + tile};
    ui_.FillRounded(tile_rect, 22.0f * ease + 2.0f, th::Rgba(th::kAccent, 0.24f * k));
    ui_.StrokeRounded(tile_rect, 22.0f * ease + 2.0f, th::Rgba(th::kAccentWarm, 0.65f * k), 1.2f);
    ui_.FillCircle(cx, cy, 30.0f * ease, th::Rgba(th::kAccent, 0.18f * k));
    const float s = 26.0f * ease;
    ui_.FillTriangle({cx - s * 0.45f, cy - s * 0.62f}, {cx + s * 0.62f, cy},
                     {cx - s * 0.45f, cy + s * 0.62f}, th::Rgba(th::kInk, 0.96f * k));

    // Sparks drifting on an orbit around the mark.
    for (int i = 0; i < 18; ++i) {
        const float fi = static_cast<float>(i);
        const float a = fi * 0.349f + t * (0.12f + 0.03f * static_cast<float>(i % 3));
        const float rad = 168.0f + 10.0f * std::sin(t * 1.3f + fi);
        const float pulse = 0.5f + 0.5f * std::sin(t * 2.0f + fi * 1.7f);
        ui_.FillCircle(cx + std::cos(a) * rad, cy + std::sin(a) * rad, 1.4f + pulse * 0.8f,
                       th::Rgba(i % 2 == 0 ? th::kAccent : th::kAccentWarm, (0.25f + 0.45f * pulse) * k));
    }

}

void App::DrawBootLogo(float cx, float cy, float reveal, float t, float alpha) {
    const float r = std::clamp(reveal, 0.0f, 1.0f);
    const float ease = 1.0f - (1.0f - r) * (1.0f - r) * (1.0f - r);
    const float k = alpha * ease;
    DrawBrandMark(cx, cy, reveal, t, alpha);

    // Wordmark, with a light glint that crosses it once the mark has settled.
    const float word_y = cy + 116.0f;
    ui_.Text(L"BetterRemotePlay", {cx - 320.0f, word_y, cx + 320.0f, word_y + 40.0f}, 30.0f,
             th::Rgba(th::kInk, 0.98f * k), DWRITE_FONT_WEIGHT_SEMI_BOLD,
             DWRITE_TEXT_ALIGNMENT_CENTER);
    if (r >= 0.95f) {
        const float phase = std::fmod(t * 0.38f, 1.9f);
        const float gx = cx - 330.0f + phase * 360.0f;
        ui_.FillRadial(gx, word_y + 20.0f, 90.0f, th::Rgba(0xFFFFFF, 0.20f * alpha),
                       th::Rgba(0xFFFFFF, 0.0f));
    }

    const float line_w = 260.0f * ease;
    ui_.FillRounded({cx - line_w * 0.5f, word_y + 52.0f, cx + line_w * 0.5f, word_y + 54.0f},
                    1.0f, th::Rgba(th::kAccent, 0.85f * k));
    ui_.Text(L"REMOTE PLAY, BETTER", {cx - 300.0f, word_y + 62.0f, cx + 300.0f, word_y + 82.0f},
             10.5f, th::Rgba(th::kMuted, 0.85f * k), DWRITE_FONT_WEIGHT_SEMI_BOLD,
             DWRITE_TEXT_ALIGNMENT_CENTER);
}

void App::RenderBoot(double t) {
    const float w = ui_w_;
    const float h = ui_h_;

    const float fade_in = std::clamp(static_cast<float>((t - boot_started_) / 0.40), 0.0f, 1.0f);
    const float fade_out =
        boot_done_at_ < 0.0
            ? 1.0f
            : 1.0f - std::clamp(static_cast<float>((t - boot_done_at_) / 0.45), 0.0f, 1.0f);
    const float alpha = fade_in * fade_out;
    // The mark draws itself in over the first second of the boot.
    const float reveal = std::clamp(static_cast<float>((t - boot_started_) / 1.0), 0.0f, 1.0f);
    const float ease = 1.0f - (1.0f - reveal) * (1.0f - reveal);

    ui_.FillLinearV({0.0f, 0.0f, w, h}, th::Rgba(th::kBgTop), th::Rgba(th::kBgBottom));
    ui_.FillRadial(w * 0.5f, h * 0.40f, w * 0.60f, th::Rgba(0x6B32D9, 0.22f * alpha),
                   th::Rgba(0x6B32D9, 0.0f));
    ui_.FillRadial(w * 0.12f, h * 0.10f, w * 0.30f, th::Rgba(0x3E2170, 0.18f * alpha),
                   th::Rgba(0x3E2170, 0.0f));

    const float cx = w * 0.5f;
    const float cy = h * 0.34f;
    DrawBootLogo(cx, cy, reveal, static_cast<float>(t), alpha);

    // Step card: the real start-up tasks, in order. Step 0 is console discovery.
    std::vector<std::pair<std::wstring, int>> steps;
    const size_t done = std::min(boot_done_steps_, boot_total_);
    for (size_t i = 0; i < boot_total_; ++i) {
        std::wstring label = i == 0 ? std::wstring(L"Looking for consoles")
                                    : (i - 1 < boot_tasks_.size() ? boot_tasks_[i - 1].label
                                                                  : std::wstring(L"Finishing up"));
        const int state = i < done ? 2 : (i == done ? 1 : 0);
        steps.emplace_back(std::move(label), state);
    }

    const float card_w = 520.0f;
    const float card_h = 74.0f + 32.0f * static_cast<float>(steps.size());
    const float card_t = cy + 204.0f;
    const D2D1_RECT_F card{cx - card_w * 0.5f, card_t, cx + card_w * 0.5f, card_t + card_h};
    ui_.FillRounded(card, 18.0f, th::Rgba(th::kPanel, 0.72f * alpha * ease));
    ui_.StrokeRounded(card, 18.0f, th::Rgba(th::kAccent, 0.20f * alpha), 1.0f);
    ui_.FillRounded({card.left + 28.0f, card.top, card.right - 28.0f, card.top + 2.0f}, 1.0f,
                    th::Rgba(th::kAccent, 0.75f * alpha));

    DrawLoaderSteps(steps, card.left + 28.0f, card.top + 18.0f, card_w - 56.0f, static_cast<float>(t), alpha * ease);

    const float bar_w = card_w - 56.0f;
    const float bx = card.left + 28.0f;
    const float by = card.bottom - 36.0f;
    ui_.FillRounded({bx, by, bx + bar_w, by + 4.0f}, 2.0f, th::Rgba(0xFFFFFF, 0.08f * alpha));
    const float filled = bar_w * static_cast<float>(std::clamp(boot_shown_, 0.0, 1.0));
    if (filled > 1.0f) {
        ui_.FillRounded({bx, by, bx + filled, by + 4.0f}, 2.0f,
                        th::Rgba(th::kAccent, 0.92f * alpha));
        ui_.FillCircle(bx + filled, by + 2.0f, 3.2f, th::Rgba(th::kInk, 0.95f * alpha));
    }
    ui_.Text(boot_status_.c_str(), {bx, by + 10.0f, bx + bar_w - 60.0f, by + 30.0f}, 12.0f,
             th::Rgba(th::kInkDim, 0.88f * alpha * std::max(0.60f, static_cast<float>(boot_status_alpha_))));
    wchar_t percent[16];
    swprintf_s(percent, L"%.0f%%", boot_shown_ * 100.0);
    ui_.Text(percent, {bx + bar_w - 56.0f, by + 10.0f, bx + bar_w, by + 30.0f}, 12.0f,
             th::Rgba(th::kMuted, 0.85f * alpha), DWRITE_FONT_WEIGHT_NORMAL,
             DWRITE_TEXT_ALIGNMENT_TRAILING);

    ui_.Text(L"v0.4", {cx - 100.0f, h - 34.0f, cx + 100.0f, h - 14.0f}, 11.0f,
             th::Rgba(th::kMuted, 0.55f * alpha), DWRITE_FONT_WEIGHT_NORMAL,
             DWRITE_TEXT_ALIGNMENT_CENTER);
}

bool App::Contains(const D2D1_RECT_F& rect, int x, int y) const {
    return x >= rect.left && x <= rect.right && y >= rect.top && y <= rect.bottom;
}

void App::AddHit(Hit hit, int index, const D2D1_RECT_F& rect) {
    hits_.push_back({rect, hit, index});
    if (Contains(rect, pointer_.x, pointer_.y)) {
        hover_ = hit;
        hover_index_ = index;
    }
}

void App::DispatchClick() {
    for (auto it = hits_.rbegin(); it != hits_.rend(); ++it) {
        if (!Contains(it->rect, pointer_.x, pointer_.y)) {
            continue;
        }

        if (psn_modal_) {
            const bool modal_hit =
                it->hit == Hit::PsnUrlField || it->hit == Hit::PsnPanel ||
                (it->hit == Hit::Button &&
                 (it->index == W_PsnOpen || it->index == W_PsnPaste ||
                  it->index == W_PsnSave || it->index == W_PsnCancel)) ||
                it->hit == Hit::Minimize || it->hit == Hit::Maximize ||
                it->hit == Hit::Close;
            if (!modal_hit) {
                ClosePsnModal();
                return;
            }
        }

        if (address_modal_) {
            const bool modal_hit =
                it->hit == Hit::AddressField || it->hit == Hit::AddressResult ||
                it->hit == Hit::AddressPanel ||
                (it->hit == Hit::Button &&
                 (it->index == W_AddressDiscover || it->index == W_AddressPaste ||
                  it->index == W_AddressSave || it->index == W_AddressCancel)) ||
                it->hit == Hit::Minimize || it->hit == Hit::Maximize ||
                it->hit == Hit::Close;
            if (!modal_hit) {
                CloseAddressModal();
                return;
            }
        }

        if (setup_modal_) {
            const bool modal_hit =
                (it->hit == Hit::Button &&
                 (it->index == W_SetupStart || it->index == W_SetupClose)) ||
                it->hit == Hit::Minimize || it->hit == Hit::Maximize ||
                it->hit == Hit::Close;
            if (!modal_hit) {
                return;
            }
        }

        if (open_dropdown_ >= 0 && it->hit != Hit::DropdownItem) {
            if (it->hit == Hit::Dropdown) {
                open_dropdown_ = (open_dropdown_ == it->index) ? -1 : it->index;
            } else {
                open_dropdown_ = -1;
            }
            return;
        }

        if (it->hit != Hit::PinField) {
            pin_focus_ = false;
        }

        switch (it->hit) {
            case Hit::Stream:
                screen_ = Screen::Stream;
                break;
            case Hit::SettingsButton:
                screen_ = Screen::Settings;
                break;
            case Hit::Back:
                if (screen_ == Screen::StreamSettings) {
                    screen_ = Screen::Stream;
                    open_dropdown_ = -1;
                } else {
                    screen_ = Screen::Home;
                }
                break;
            case Hit::Tab:
                tab_ = static_cast<Tab>(it->index);
                open_dropdown_ = -1;
                break;
            case Hit::OverlayTab:
                overlay_tab_ = it->index;
                open_dropdown_ = -1;
                break;
            case Hit::Slider:
                break;
            case Hit::ConsoleConnect:
                StartProbe();
                break;
            case Hit::Dropdown:
                open_dropdown_ = it->index;
                break;
            case Hit::DropdownItem:
                ApplyDropdown(it->index / 1000, it->index % 1000);
                open_dropdown_ = -1;
                break;
            case Hit::Button:
                if (it->index == W_TestTone) {
                    if (!audio_.is_open()) {
                        OpenAudioDevice();
                    }
                    audio_.PlayTestTone(0.6);
                } else if (it->index == W_Rumble) {
                    TriggerRumble();
                } else if (it->index == W_SetupRife) {
                    setup_modal_ = true;
                    if (rife_setup_.state() != RifeSetup::State::Running &&
                        rife_setup_.state() != RifeSetup::State::Done) {
                        rife_setup_.Start(ExeDirPath());
                    }
                } else if (it->index == W_SetupStart) {
                    rife_setup_.Start(ExeDirPath());
                } else if (it->index == W_SetupClose) {
                    setup_modal_ = false;
                } else if (it->index == W_Probe) {
                    ConnectConsole();
                } else if (it->index == W_PairHome) {
                    screen_ = Screen::Settings;
                    tab_ = Tab::Connection;
                } else if (it->index == W_Disconnect) {
                    DisconnectStream();
                } else if (it->index == W_ResumeOverlay) {
                    screen_ = Screen::Stream;
                    overlay_tab_ = 0;
                    open_dropdown_ = -1;
                } else if (it->index == W_Pair) {
                    if (pin_.size() == 8) {
                        pairing_.Start(profile_, static_cast<uint32_t>(std::stoul(pin_)));
                    }
                } else if (it->index == W_PsnLogin) {
                    StartPsnLogin();
                } else if (it->index == W_PsnOpen) {
                    std::string detail;
                    if (!session::OpenPsnLoginPage(detail)) {
                        psn_note_ = Widen(detail);
                        psn_note_ok_ = false;
                    } else {
                        psn_note_ = L"Browser opened. Sign in, then paste the final URL.";
                        psn_note_ok_ = false;
                    }
                } else if (it->index == W_PsnPaste) {
                    PastePsnClipboard();
                } else if (it->index == W_PsnSave) {
                    SubmitPsnLogin();
                } else if (it->index == W_PsnCancel) {
                    ClosePsnModal();
                } else if (it->index == W_AddressOpen) {
                    OpenAddressModal();
                } else if (it->index == W_AddressDiscover) {
                    DiscoverConsoles();
                } else if (it->index == W_AddressPaste) {
                    PasteAddressClipboard();
                } else if (it->index == W_AddressSave) {
                    SaveConsoleAddress();
                } else if (it->index == W_AddressCancel) {
                    CloseAddressModal();
                } else {
                    // Buttons whose whole action lives in the widget switch
                    // (overlay pages, the Neural Ultra preset). Without this a
                    // drawn button was a placeholder: the overlay's Neural
                    // button never opened the page.
                    ApplyDropdown(it->index, 0);
                }
                break;
            case Hit::PinField:
                pin_focus_ = true;
                break;
            case Hit::PsnUrlField:
                psn_url_focus_ = true;
                break;
            case Hit::PsnPanel:
                break;
            case Hit::AddressField:
                address_focus_ = true;
                break;
            case Hit::AddressPanel:
                break;
            case Hit::AddressResult: {
                std::lock_guard<std::mutex> lock(address_mutex_);
                if (it->index >= 0 &&
                    it->index < static_cast<int>(address_results_.size())) {
                    const session::DiscoveredConsole& found = address_results_[it->index];
                    address_input_ = found.address;
                    address_selected_ = it->index;
                    address_focus_ = true;
                    address_note_ = L"Selected " +
                                    Widen(found.name.empty() ? found.address : found.name) +
                                    L". Press Save address.";
                }
                break;
            }
            case Hit::Minimize:
                if (window_) {
                    window_->Minimize();
                }
                break;
            case Hit::Maximize:
                if (window_) {
                    window_->ToggleMaximize();
                }
                break;
            case Hit::Close:
                if (window_) {
                    window_->Close();
                }
                break;
            case Hit::None:
                break;
        }
        return;
    }
    if (psn_modal_) {
        ClosePsnModal();
    }
    if (address_modal_) {
        CloseAddressModal();
    }
    open_dropdown_ = -1;
    pin_focus_ = false;
}

void App::HandleKey(int vk) {
    if (intro_) {
        intro_ = false;
        return;
    }
    if (booting_) {
        if (vk == VK_ESCAPE) {
            quit_ = true;
        }
        return;
    }
    if (setup_modal_) {
        if (vk == VK_ESCAPE) {
            setup_modal_ = false;
        }
        return;
    }
    if (address_modal_) {
        if (vk == VK_ESCAPE) {
            CloseAddressModal();
        } else if (vk == VK_RETURN) {
            SaveConsoleAddress();
        } else if (vk == 'V' && (::GetKeyState(VK_CONTROL) & 0x8000) != 0) {
            PasteAddressClipboard();
        }
        return;
    }
    if (psn_modal_) {
        if (vk == VK_ESCAPE) {
            ClosePsnModal();
        } else if (vk == VK_RETURN) {
            if (psn_note_ok_) {
                ClosePsnModal();
            } else {
                SubmitPsnLogin();
            }
        } else if (vk == 'V' && (::GetKeyState(VK_CONTROL) & 0x8000) != 0) {
            PastePsnClipboard();
        }
        return;
    }
    if (vk == VK_F12) {
        capture_requested_ = true;
        return;
    }
    if (vk == VK_F11) {
        ToggleFullscreen();
        return;
    }

    switch (screen_) {
        case Screen::Home:
            if (vk == VK_RETURN) {
                screen_ = Screen::Stream;
            } else if (vk == 'S' || vk == VK_TAB) {
                screen_ = Screen::Settings;
            } else if (vk == VK_ESCAPE) {
                if (clock_ - last_esc_at_ < 2.0) {
                    quit_ = true;
                } else {
                    last_esc_at_ = clock_;
                    stream_note_ = "Press Esc again to quit.";
                }
            }
            break;

        case Screen::Settings:
            if (vk == VK_ESCAPE) {
                if (open_dropdown_ >= 0) {
                    open_dropdown_ = -1;
                } else if (pin_focus_) {
                    pin_focus_ = false;
                } else {
                    screen_ = Screen::Home;
                }
            } else if (vk == 'S') {
                screen_ = Screen::Home;
            } else if (vk == VK_TAB || vk == VK_RIGHT) {
                tab_ = static_cast<Tab>((static_cast<int>(tab_) + 1) % 10);
                open_dropdown_ = -1;
            } else if (vk == VK_LEFT) {
                tab_ = static_cast<Tab>((static_cast<int>(tab_) + 9) % 10);
                open_dropdown_ = -1;
            }
            break;

        case Screen::Stream:
            if (vk == VK_TAB) {
                screen_ = Screen::StreamSettings;
                overlay_tab_ = 0;
                open_dropdown_ = -1;
                mouse_active_at_ = clock_;
            } else if (vk == VK_ESCAPE) {
                if (clock_ - last_esc_at_ < 2.0) {
                    last_esc_at_ = -10.0;
                    DisconnectStream();
                } else {
                    last_esc_at_ = clock_;
                    stream_note_ = "Press Esc again to disconnect.";
                }
            } else if (vk == 'H') {
                hud_ = !hud_;
            } else if (vk >= '1' && vk <= '5') {
                upscale_ = vk - '1';
                presenter_.SetUpscaleMode(upscale_);
            }
            break;

        case Screen::StreamSettings:
            if (vk == VK_ESCAPE) {
                if (open_dropdown_ >= 0) {
                    open_dropdown_ = -1;
                } else {
                    screen_ = Screen::Stream;
                    mouse_active_at_ = clock_;
                }
            } else if (vk == VK_TAB) {
                screen_ = Screen::Stream;
                open_dropdown_ = -1;
                mouse_active_at_ = clock_;
            } else if (vk == 'H') {
                hud_ = !hud_;
            } else if (vk >= '1' && vk <= '5') {
                upscale_ = vk - '1';
                presenter_.SetUpscaleMode(upscale_);
            }
            break;
    }
}

void App::HandlePad() {
    // While the session is live the pad belongs to the console, not the UI.
    if (stream_active_) {
        pad_.ConsumePressed();
        return;
    }

    const PadState pad = pad_.snapshot();
    if (!pad.connected) {
        return;
    }

    const bool directions[4] = {
        (pad.buttons & PadUp) != 0 || pad.ly > 0.55f,
        (pad.buttons & PadDown) != 0 || pad.ly < -0.55f,
        (pad.buttons & PadLeft) != 0 || pad.lx < -0.55f,
        (pad.buttons & PadRight) != 0 || pad.lx > 0.55f,
    };
    const int keys[4] = {VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT};

    for (int i = 0; i < 4; ++i) {
        if (directions[i]) {
            if (!repeat_was_[i]) {
                repeat_was_[i] = true;
                repeat_at_[i] = clock_ + 0.35;
                HandleKey(keys[i]);
            } else if (clock_ >= repeat_at_[i]) {
                repeat_at_[i] = clock_ + 0.11;
                HandleKey(keys[i]);
            }
        } else {
            repeat_was_[i] = false;
        }
    }

    const uint32_t pressed = pad_.ConsumePressed();

    if (pressed & PadCross) {
        if (screen_ == Screen::Home) {
            screen_ = Screen::Stream;
        }
    }
    if ((pressed & PadCircle) && screen_ != Screen::Home) {
        screen_ = Screen::Home;
    }
    if (pressed & PadOptions) {
        if (screen_ == Screen::Home) {
            screen_ = Screen::Settings;
        } else if (screen_ != Screen::Stream) {
            screen_ = Screen::Home;
        }
    }
}

void App::ApplyDropdown(int id, int index) {
    switch (id) {
        case W_Upscaler:
            if (index == 5) {
                if (!DlssAvailable()) {
                    stream_note_ = "DLSS SR unavailable: nvngx_dlss.dll not found.";
                    break;
                }
                profile_.neural_sr = 1;
                ApplyNeuralSettings();
                stream_note_ = profile_.neural_sr
                                   ? "DLSS Super Resolution selected (D3D12)."
                                   : "DLSS SR unavailable; using the regular upscaler.";
                break;
            }
            upscale_ = std::clamp(index, 0, 4);
            presenter_.SetUpscaleMode(upscale_);
            profile_.upscaler = upscale_;
            if (profile_.neural_sr > 0) {
                profile_.neural_sr = 0;
                dlss_ready_ = false;
                dlss_sr_.Shutdown();
            }
            SaveProfile();
            break;
        case W_RenderTarget:
            render_target_ = std::clamp(index, 0, 3);
            profile_.render_target = render_target_;
            ApplyRenderTarget();
            SaveProfile();
            break;
        case W_FrameGen: {
            static const int kMultipliers[] = {0, 2};
            const int value = kMultipliers[std::clamp(index, 0, 1)];
            if (value > 0 && !EnsureFrameGen()) {
                frame_gen_ = 0;
                profile_.frame_gen = 0;
                stream_note_ = "Frame generation unavailable: NVIDIA VFX runtime (Settings > Display)";
                SaveProfile();
                break;
            }
            if (value > 0 && profile_.dlss_fg >= 2) {
                profile_.dlss_fg = 0;
                ApplyDlssFrameGen();
                stream_note_ = "Frame generation: NVIDIA VFG replaced DLSS FG.";
            }
            if (value > 0 && profile_.neural_nr > 0) {
                profile_.neural_nr = 0;
                DisableNr();
                stream_note_ = "Frame generation replaced DLSSNR.";
            }
            if (value > 0 && profile_.neural_sr > 0) {
                profile_.neural_sr = 0;
                dlss_ready_ = false;
                dlss_sr_.Shutdown();
                stream_note_ = "Frame generation replaced DLSS Super Resolution.";
            }
            frame_gen_ = value;
            profile_.frame_gen = value;
            fg_queue_.clear();
            fg_last_ = {};
            fg_generated_ = 0;
            if (value == 0 && stream_active_ && !stream_frame_.rgba.empty()) {
                presenter_.SetSourceFrame(stream_frame_.rgba.data(), stream_frame_.width,
                                          stream_frame_.height, stream_frame_.pq);
            }
            SaveProfile();
            break;
        }
        case W_Decoder:
            profile_.decoder = std::clamp(index, 0, 6);
            SaveProfile();
            stream_note_ = "Decoder selected; it applies to the next session.";
            break;
        case W_Chroma:
            profile_.chroma = std::clamp(index, 0, 2);
            SaveProfile();
            stream_note_ = "Chroma upsampling applies to the next session.";
            break;
        case W_DlssPreset: {
            static const int kPresets[] = {11, 12, 13};
            profile_.dlss_preset = kPresets[std::clamp(index, 0, 2)];
            if (profile_.neural_sr > 0) {
                ApplyNeuralSettings();
                stream_note_ = "DLSS model preset applied live.";
            } else {
                SaveProfile();
            }
            break;
        }
        case W_DlssMode: {
            profile_.dlss_mode = std::clamp(index, 0, 5);
            if (profile_.neural_sr > 0) {
                ApplyNeuralSettings();
                stream_note_ = "DLSS mode applied live.";
            } else {
                SaveProfile();
            }
            break;
        }
        case W_HudFrames:
            profile_.hud_mask ^= 0x1;
            SaveProfile();
            break;
        case W_HudGpu:
            profile_.hud_mask ^= 0x2;
            SaveProfile();
            break;
        case W_HudPad:
            profile_.hud_mask ^= 0x4;
            SaveProfile();
            break;
        case W_HudStream:
            profile_.hud_mask ^= 0x8;
            SaveProfile();
            break;
        case W_HudNet:
            profile_.hud_mask ^= 0x10;
            SaveProfile();
            break;
        case W_HudReset:
            frame_ms_.clear();
            frame_stutters_ = 0;
            stat_at_ = -1.0;
            stat_p50_ = stat_p95_ = stat_p99_ = stat_max_ = 0.0f;
            break;
        case W_Reflex: {            profile_.reflex = std::clamp(index, 0, 2);
            ApplyReflex();
            SaveProfile();
            break;
        }
        case W_DlssFg: {
            profile_.dlss_fg = index <= 0 ? 0 : 2;
            if (profile_.dlss_fg >= 2) {
                frame_gen_ = 0;
                profile_.frame_gen = 0;
                fg_queue_.clear();
                fg_last_ = {};
            }
            ApplyDlssFrameGen();
            stream_note_ = profile_.dlss_fg >= 2
                               ? "Frame generation on; the multiplier follows the content and the target."
                               : "DLSS Frame Generation off.";
            break;
        }
        case W_RifeOutput: {
            profile_.rife_output = index == 1 ? 0 : 60;
            SaveProfile();
            stream_note_ = profile_.rife_output == 0
                               ? "RIFE pass-through: source frames only."
                               : "RIFE: one generated frame between each pair.";
            break;
        }
        case W_HapticIntensity: {
            profile_.haptic_intensity = std::clamp(index, 0, 5);
            SaveProfile();
            stream_note_ = profile_.haptic_intensity == 0
                               ? "Haptics off."
                               : "Haptics intensity set.";
            break;
        }
        case W_FgEngine: {
            profile_.fg_engine = index == 1 ? 1 : 0;
            SaveProfile();
            stream_note_ = profile_.fg_engine == 1
                               ? "RIFE engine selected; it loads when the session starts."
                               : "NVIDIA DLSS Frame Generation selected.";
            break;
        }
        case W_FgTarget: {
            static const int kTargets[] = {0, 60, 90, 120};
            profile_.fg_target = kTargets[std::clamp(index, 0, 3)];
            SaveProfile();
            stream_note_ = profile_.fg_target == 0
                               ? "Output follows the display."
                               : "Output target " + std::to_string(profile_.fg_target) + " fps.";
            break;
        }
        case W_NrOrder: {
            profile_.nr_order = std::clamp(index, 0, 1);
            // The NR worker's frame size depends on the order: restart it.
            if (nr_.ready()) {
                DisableNr();
            }
            if (profile_.neural_nr > 0) {
                ApplyNrSettings();
            } else {
                SaveProfile();
            }
            break;
        }
        case W_NeuralNr:
            profile_.neural_nr = index == 1 ? 1 : 0;
            ApplyNrSettings();
            stream_note_ = profile_.neural_nr
                               ? "DLSS Neural Rendering enabled (NGX worker)."
                               : "DLSSNR off.";
            break;
        case W_NrPasses:
            profile_.nr_passes = std::clamp(index, 0, 3) + 1;
            if (profile_.neural_nr > 0) ApplyNrSettings(); else SaveProfile();
            break;
        case W_NrStyle:
            profile_.nr_style = std::clamp(index, 0, 2);
            if (profile_.neural_nr > 0) ApplyNrSettings(); else SaveProfile();
            break;
        case W_NrTone:
            profile_.nr_tone = std::clamp(index, 0, 8) * 25;
            if (profile_.neural_nr > 0) ApplyNrSettings(); else SaveProfile();
            break;
        case W_NrStructure:
            profile_.nr_structure = std::clamp(index, 0, 8) * 25;
            if (profile_.neural_nr > 0) ApplyNrSettings(); else SaveProfile();
            break;
        case W_NrSkin:
            profile_.nr_skin = std::clamp(index, 0, 8) * 25;
            if (profile_.neural_nr > 0) ApplyNrSettings(); else SaveProfile();
            break;
        case W_NrMask:
            profile_.nr_mask = index == 1 ? 1 : 0;
            if (profile_.neural_nr > 0) ApplyNrSettings(); else SaveProfile();
            break;
        case W_NrIntensity:
            profile_.nr_intensity = std::clamp(index, 0, 8) * 25;
            if (profile_.neural_nr > 0) ApplyNrSettings(); else SaveProfile();
            break;
        case W_NrUi:
            profile_.nr_ui = index == 1 ? 1 : 0;
            if (profile_.neural_nr > 0) ApplyNrSettings(); else SaveProfile();
            break;
        case W_Hud:
            hud_ = index == 1;
            profile_.hud = hud_;
            SaveProfile();
            break;
        case W_AudioDevice:
            if (index <= 0) {
                profile_.audio_device_id.clear();
                audio_device_ = 0;
                SaveProfile();
                OpenAudioDevice();
            } else if (!audio_devices_.empty()) {
                audio_device_ = std::clamp(index - 1, 0, static_cast<int>(audio_devices_.size()) - 1);
                profile_.audio_device_id = audio_devices_[audio_device_].id;
                SaveProfile();
                OpenAudioDevice();
            }
            break;
        case W_AudioBuffer:
            audio_buffer_ms_ = kBufferSizes[std::clamp(index, 0, 3)];
            profile_.audio_buffer_ms = audio_buffer_ms_;
            SaveProfile();
            OpenAudioDevice();
            break;
        case W_Hdr: {
            hdr_ = index == 1;
            profile_.hdr = hdr_;
            {
                std::string save_detail;
                session::Save(profile_, session::DefaultProfilePath(), save_detail);
            }
            break;
            break;
        }
        case W_Resolution: {
            profile_.width = index == 0 ? 1280 : 1920;
            profile_.height = index == 0 ? 720 : 1080;
            std::string save_detail;
            session::Save(profile_, session::DefaultProfilePath(), save_detail);
            break;
        }
        case W_Fps: {
            profile_.fps = index == 0 ? 30 : 60;
            std::string save_detail;
            session::Save(profile_, session::DefaultProfilePath(), save_detail);
            break;
        }
        case W_FrameBuffer: {
            static const int kBuffers[] = {0, 1, 2, 3};
            stream_buffer_ = kBuffers[std::clamp(index, 0, 3)];
            profile_.frame_buffer = stream_buffer_;
            SaveProfile();
            break;
        }
        case W_PadRate: {
            static const int kRates[] = {60, 125, 250, 500, 1000};
            pad_rate_hz_ = kRates[std::clamp(index, 0, 4)];
            profile_.pad_rate_hz = pad_rate_hz_;
            SaveProfile();
            break;
        }
        case W_ConsoleRumble:
            console_rumble_ = index == 0;
            profile_.console_rumble = console_rumble_ ? 1 : 0;
            if (!console_rumble_) {
                pad_.SetConsoleFeedback(PadConsoleFeedback{});
            }
            SaveProfile();
            break;
        case W_ConsoleTriggers:
            console_triggers_ = index == 0;
            profile_.console_triggers = console_triggers_ ? 1 : 0;
            if (!console_triggers_) {
                pad_.SetConsoleFeedback(PadConsoleFeedback{});
            }
            SaveProfile();
            break;
        case W_Motion:
            motion_ = index == 0;
            profile_.motion = motion_ ? 1 : 0;
            streaming_.SetMotionEnabled(motion_);
            SaveProfile();
            break;
        case W_Bitrate:
            profile_.bitrate_kbps = kBitrates[std::clamp(index, 0, kBitrateCount - 1)];
            {
                std::string save_detail;
                session::Save(profile_, session::DefaultProfilePath(), save_detail);
            }
            break;
        case W_InputSource:
            input_source_ = std::clamp(index, 0, 2);
            pad_.SetBackend(kInputBackends[input_source_]);
            profile_.input_source = input_source_;
            SaveProfile();
            break;
        case W_Lightbar:
            lightbar_index_ = std::clamp(index, 0, kLightbarCount - 1);
            profile_.lightbar = lightbar_index_;
            output_dirty_ = true;
            SaveProfile();
            break;
        case W_Trigger:
            trigger_effect_ = std::clamp(index, 0, kTriggerCount - 1);
            profile_.trigger_effect = trigger_effect_;
            output_dirty_ = true;
            SaveProfile();
            break;
        case W_Strength:
            trigger_strength_ = std::clamp(index + 1, 1, 8);
            profile_.trigger_strength = trigger_strength_;
            output_dirty_ = true;
            SaveProfile();
            break;
        case W_PlayerLeds:
            player_leds_ = std::clamp(index, 0, kPlayerLedCount - 1);
            profile_.player_leds = player_leds_;
            output_dirty_ = true;
            SaveProfile();
            break;
        case W_MicLed:
            mic_led_ = std::clamp(index, 0, 2);
            profile_.mic_led = mic_led_;
            output_dirty_ = true;
            SaveProfile();
            break;
        case W_ColorOn:
            profile_.color_on = index == 1 ? 1 : 0;
            ApplyEnhance();
            SaveProfile();
            break;
        case W_PresetLaunch:
            ApplyLaunchPreset(0);
            break;
        case W_PresetSafe:
            ApplyLaunchPreset(1);
            break;
        case W_PresetSr:
            ApplyLaunchPreset(2);
            break;
        case W_PresetLow:
            ApplyLaunchPreset(3);
            break;
        case W_ColorReset:
            profile_.sharpness = 35;
            profile_.color_exposure = 0;
            profile_.color_contrast = 100;
            profile_.color_highlights = 0;
            profile_.color_shadows = 0;
            profile_.color_saturation = 100;
            profile_.color_vibrance = 0;
            profile_.color_hue = 0;
            profile_.color_temperature = 0;
            profile_.color_tint = 0;
            profile_.color_gamma = 100;
            ApplyEnhance();
            SaveProfile();
            break;
        case W_ColorNeutral:
            profile_.color_exposure = 0;
            profile_.color_contrast = 100;
            profile_.color_highlights = 0;
            profile_.color_shadows = 0;
            profile_.color_saturation = 100;
            profile_.color_vibrance = 0;
            profile_.color_hue = 0;
            profile_.color_temperature = 0;
            profile_.color_tint = 0;
            profile_.color_gamma = 100;
            ApplyEnhance();
            SaveProfile();
            break;
        case W_ColorVivid:
            profile_.color_saturation = 118;
            profile_.color_vibrance = 35;
            profile_.color_contrast = 108;
            ApplyEnhance();
            SaveProfile();
            break;
        case W_ColorCinematic:
            profile_.color_contrast = 112;
            profile_.color_saturation = 92;
            profile_.color_highlights = -18;
            profile_.color_shadows = 10;
            profile_.color_temperature = -8;
            ApplyEnhance();
            SaveProfile();
            break;
        case W_ColorWarm:
            profile_.color_temperature = 22;
            profile_.color_tint = 4;
            profile_.color_saturation = 105;
            ApplyEnhance();
            SaveProfile();
            break;
        default:
            break;
    }
}

void App::OpenAudioDevice() {
    audio_.Close();
    const std::string id =
        audio_devices_.empty() ? std::string() : audio_devices_[audio_device_].id;
    audio_failed_ = !audio_.Open(id, audio_buffer_ms_);
}

void App::TriggerRumble() {
    rumble_until_ = clock_ + 0.80;
    output_dirty_ = true;
    std::fprintf(stderr, "[pad] rumble test 800 ms (fw 0x%04X, %s)\n",
                 pad_.firmware_version(), pad_.calibrated() ? "calibrated" : "default");
}

void App::ApplyRenderTarget() {
    static const int kTargets[4][2] = {{0, 0}, {1920, 1080}, {2560, 1440}, {3840, 2160}};
    const int index = std::clamp(render_target_, 0, 3);
    presenter_.SetUpscaleTarget(kTargets[index][0], kTargets[index][1]);

    if (profile_.neural_sr > 0) {
        // Keep the selection even when the feature cannot be created right
        // now (DLAA on an odd size, a driver hiccup): the user must always be
        // able to switch modes, and the loop retries.
        dlss_ready_ = EnsureDlssSrForTarget();
        if (!dlss_ready_) {
            dlss_sr_.Shutdown();
        }
    }
    // With the NVIDIA order the NR stage size follows the target, so a target
    // change needs a fresh worker (the shared memory is sized once).
    if (profile_.neural_nr > 0 && nr_.ready()) {
        int stage_w = 0;
        int stage_h = 0;
        NrStageSize(&stage_w, &stage_h);
        if (stage_w != nr_in_w_ || stage_h != nr_in_h_) {
            ApplyNrSettings();
        }
    }
    // SR output size changed while FG is active: the worker's shared memory is
    // sized once, so rebuild it for the new stage.
    if (profile_.dlss_fg >= 2 && dlss_ready_) {
        ApplyDlssFrameGen();
    }
}

int App::present_hz() const {
    // The app itself runs at the monitor's refresh rate. Frame generation is
    // presented by the NVIDIA worker/presenter on its own thread, so it no
    // longer dictates the UI cadence.
    return std::clamp(refresh_hz_, 24, 500);
}

void App::ApplyEnhance() {
    EnhanceParams p{};
    p.color = profile_.color_on > 0;
    p.sharpen = static_cast<float>(std::clamp(profile_.sharpness, 0, 100)) / 100.0f;
    p.exposure = static_cast<float>(std::clamp(profile_.color_exposure, -100, 100)) / 100.0f;
    p.contrast = static_cast<float>(std::clamp(profile_.color_contrast, 50, 150)) / 100.0f;
    p.highlights = static_cast<float>(std::clamp(profile_.color_highlights, -100, 100)) / 100.0f;
    p.shadows = static_cast<float>(std::clamp(profile_.color_shadows, -100, 100)) / 100.0f;
    p.saturation = static_cast<float>(std::clamp(profile_.color_saturation, 0, 200)) / 100.0f;
    p.vibrance = static_cast<float>(std::clamp(profile_.color_vibrance, 0, 100)) / 100.0f;
    p.hue_deg = static_cast<float>(std::clamp(profile_.color_hue, -180, 180));
    p.temperature = static_cast<float>(std::clamp(profile_.color_temperature, -100, 100)) / 100.0f;
    p.tint = static_cast<float>(std::clamp(profile_.color_tint, -100, 100)) / 100.0f;
    p.gamma = static_cast<float>(std::clamp(profile_.color_gamma, 80, 120)) / 100.0f;
    p.active = p.sharpen > 0.0f || p.color;

    if (std::memcmp(&p, &enh_applied_, sizeof(p)) == 0) {
        return;
    }
    presenter_.SetEnhance(p);
    enh_applied_ = p;
    enh_dirty_ = true;
    enh_dirty_at_ = clock_;
}

void App::TickEnhance() {
    ApplyEnhance();
    if (enh_dirty_ && clock_ - enh_dirty_at_ >= 1.0) {
        enh_dirty_ = false;
        SaveProfile();
    }
}

void App::ApplyLaunchPreset(int preset) {
    // While a session runs, a preset only changes the saved selection. Building
    // the chain mid-session restarts the FG worker and has been observed to
    // upset the D3D12VA decoder, so the chain is built on the next start.
    const bool live = !stream_active_;
    const char* note = nullptr;
    switch (preset) {
        case 0:  // Launch: DLSS SR Ultra Quality + FG 2x at 1440p
            profile_.neural_sr = 1;
            profile_.dlss_mode = 4;
            profile_.dlss_fg = 2;
            profile_.neural_nr = 0;
            profile_.render_target = 2;
            profile_.frame_buffer = 1;
            if (live) {
                render_target_ = 2;
                stream_buffer_ = 1;
                ApplyRenderTarget();
                ApplyNeuralSettings();
                ApplyDlssFrameGen();
            }
            note = "Launch preset: DLSS SR Ultra Quality + FG 2x.";
            break;
        case 1:  // FG 2x without SR: not offered until the worker restart path
                 // is proven; keep the profile untouched.
            stream_note_ = "FG without SR is not offered as a preset in this build.";
            return;
        case 2:  // SR only
            profile_.neural_sr = 1;
            profile_.dlss_mode = 4;
            profile_.dlss_fg = 0;
            profile_.neural_nr = 0;
            if (live) {
                ApplyNeuralSettings();
                ApplyDlssFrameGen();
            }
            note = "SR-only preset: no frame generation.";
            break;
        case 3:  // Low latency: raw stream, no neural stages
            profile_.neural_sr = 0;
            profile_.dlss_fg = 0;
            profile_.neural_nr = 0;
            profile_.upscaler = 0;
            profile_.render_target = 0;
            profile_.frame_buffer = 0;
            if (live) {
                upscale_ = 0;
                presenter_.SetUpscaleMode(0);
                render_target_ = 0;
                stream_buffer_ = 0;
                ApplyNeuralSettings();
                ApplyDlssFrameGen();
                ApplyRenderTarget();
            }
            note = "Low-latency preset: raw stream, no neural stages.";
            break;
        default:
            return;
    }
    SaveProfile();
    stream_note_ = std::string(note) + (live ? " Applied." : " Applies when the session starts.");
}

void App::LogHealth() {
    if (health_path_.empty()) {
        return;
    }
    namespace fs = std::filesystem;
    std::error_code ec;
    if (fs::exists(health_path_, ec) && fs::file_size(health_path_, ec) > 2ull * 1024 * 1024) {
        fs::rename(health_path_, health_path_ + ".1", ec);
        ec.clear();
    }
    std::ofstream file(health_path_, std::ios::app | std::ios::binary);
    if (!file) {
        return;
    }

    SYSTEMTIME st{};
    ::GetLocalTime(&st);
    int four = 0;
    const int held = std::clamp(video_hold_fill_, 0, static_cast<int>(video_holds_.size()));
    for (int i = 0; i < held; ++i) {
        if (video_holds_[static_cast<size_t>(i)] == 4) {
            ++four;
        }
    }
    const int four_pct = held > 0 ? (four * 100) / held : 0;
    DWORD handles = 0;
    ::GetProcessHandleCount(::GetCurrentProcess(), &handles);
    uint64_t vram = 0;
    uint64_t vram_budget = 0;
    if (!presenter_.video_memory_used_mb(vram, vram_budget)) {
        vram = vram_budget = 0;
    }
    const double src_fps = streaming_.fps();
    const float budget_ms = 1000.0f / static_cast<float>(present_hz());

    char line[512];
    std::snprintf(line, sizeof(line),
                  "%04d-%02d-%02dT%02d:%02d:%02d sr=%d mode=%d fg=%d nr=%d hdr=%d tgt=%d buf=%d | "
                  "worker=%.1f dlss=%.2f enh=%.2f | frame_p99=%.2f work=%.2f late=%d stalls=%d | "
                  "src=%.1f 4v=%d q=%d drops=%d lost=%d dec=%.2f/%.2f out=%d | "
                  "audio=%d handles=%lu vram=%llu/%llu\n",
                  st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                  profile_.neural_sr, profile_.dlss_mode, profile_.dlss_fg, profile_.neural_nr,
                  hdr_ ? 1 : 0, render_target_, stream_buffer_, fg_worker_fps_,
                  dlss_sr_.last_ms(), presenter_.enhance_ms(), static_cast<double>(stat_p99_),
                  frame_work_ms_, stat_late_, stat_stalls_, src_fps, four_pct,
                  stream_stats_.queue, stream_stats_.buffer_drops + streaming_.video_queue_drops(),
                  stream_stats_.lost, stream_stats_.decode_ms_avg, stream_stats_.decode_ms_max,
                  stream_stats_.arrival_outliers, audio_.underruns(),
                  static_cast<unsigned long>(handles),
                  static_cast<unsigned long long>(vram),
                  static_cast<unsigned long long>(vram_budget));
    file << line;
    file.flush();
    (void)budget_ms;
}

HANDLE App::present_waitable() const {
    return presenter_.frame_waitable();
}

bool App::WaitForPresent(int timeout_ms) {
    return presenter_.WaitForFrame(static_cast<DWORD>(timeout_ms));
}

void App::RecoverFromDeviceLoss() {
    std::fprintf(stderr, "[app] graphics device lost; restarting the client\n");
    SaveProfile();

    wchar_t exe_path[MAX_PATH] = {};
    if (::GetModuleFileNameW(nullptr, exe_path, MAX_PATH) == 0) {
        quit_ = true;
        return;
    }

    wchar_t already[8] = {};
    const bool retried = ::GetEnvironmentVariableW(L"BRP_RESTARTED", already, 8) > 0;
    if (!retried) {
        ::SetEnvironmentVariableW(L"BRP_RESTARTED", L"1");
        std::wstring command = L"\"" + std::wstring(exe_path) + L"\"";
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (::CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                             nullptr, &si, &pi)) {
            ::CloseHandle(pi.hThread);
            ::CloseHandle(pi.hProcess);
            std::fprintf(stderr, "[app] restarted as pid %lu\n", pi.dwProcessId);
            quit_ = true;
            return;
        }
    }

    stream_note_ = "The graphics device was lost. Restart BetterRemotePlay.";
    quit_ = true;
}

void App::NoteSourceFrame() {
    source_updated_ = true;
    if (last_source_at_ > 0.0) {
        const double ms = (clock_ - last_source_at_) * 1000.0;
        if (ms > 1.0 && ms < 250.0) {
            int vblanks = static_cast<int>(std::lround(ms * present_hz() / 1000.0));
            vblanks = std::clamp(vblanks, 1, 11);
            video_holds_[video_hold_pos_] = static_cast<uint8_t>(vblanks);
            video_hold_pos_ = (video_hold_pos_ + 1) % static_cast<int>(video_holds_.size());
            if (video_hold_fill_ < static_cast<int>(video_holds_.size())) {
                ++video_hold_fill_;
            }
        }
    }
    last_source_at_ = clock_;
}

void App::SampleFgWorker() {
    if (profile_.dlss_fg < 2 || !stream_active_) {
        fg_worker_fps_ = 0.0;
        return;
    }
    if (clock_ - fg_worker_at_ < 1.0) {
        return;
    }
    fg_worker_at_ = clock_;

    wchar_t exe_path[MAX_PATH] = {};
    if (::GetModuleFileNameW(nullptr, exe_path, MAX_PATH) == 0) {
        return;
    }
    const std::filesystem::path log =
        std::filesystem::path(exe_path).parent_path() / L"nr-worker.log";

    // Only trust a fresh line: the worker keeps the last value in its log
    // after the session ends, and showing that as a live reading is a lie.
    std::error_code ec;
    const auto modified = std::filesystem::last_write_time(log, ec);
    if (ec || std::filesystem::file_time_type::clock::now() - modified >
                  std::chrono::seconds(3)) {
        fg_worker_fps_ = 0.0;
        return;
    }

    std::ifstream file(log, std::ios::binary);
    if (!file) {
        return;
    }
    file.seekg(0, std::ios::end);
    const std::streamoff size = file.tellg();
    const std::streamoff start = size > 8192 ? size - 8192 : 0;
    file.seekg(start);
    const std::string text((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());
    const size_t pos = text.rfind("[fg] displayed ");
    if (pos == std::string::npos) {
        return;
    }
    double fps = 0.0;
    if (std::sscanf(text.c_str() + pos, "[fg] displayed %lf FPS", &fps) == 1) {
        fg_worker_fps_ = fps;
    }
}

void App::LogVideoCadence() {
    static const bool probe = std::getenv("BRP_FRAME_PROBE") != nullptr;
    if (!probe || !stream_active_) {
        return;
    }
    if (video_probe_at_ < 0.0) {
        video_probe_at_ = clock_;
    }
    if (clock_ - video_probe_at_ < 2.0) {
        return;
    }
    video_probe_at_ = clock_;

    int counts[12] = {};
    for (int i = 0; i < video_hold_fill_; ++i) {
        ++counts[video_holds_[i]];
    }
    int screen_counts[12] = {};
    for (int i = 0; i < screen_hold_fill_; ++i) {
        ++screen_counts[screen_holds_[i]];
    }
    const int n = std::max(1, video_hold_fill_);
    const int sn = std::max(1, screen_hold_fill_);
    const auto pct = [&](int v) { return counts[v] * 100 / n; };
    const auto spct = [&](int v) { return screen_counts[v] * 100 / sn; };
    const double source_fps = stream_stats_.elapsed > 0.0
                                  ? static_cast<double>(stream_stats_.frames) /
                                        stream_stats_.elapsed
                                  : 0.0;
    const unsigned long long misses = presenter_.present_misses();

    float feed_p50 = 0.0f;
    float feed_p99 = 0.0f;
    float feed_max = 0.0f;
    const int feed_n = static_cast<int>(feed_ms_.size());
    if (feed_n > 0) {
        std::vector<float> sorted(feed_ms_.begin(), feed_ms_.end());
        std::sort(sorted.begin(), sorted.end());
        const auto pick = [&](double q) {
            return sorted[std::min(sorted.size() - 1, static_cast<size_t>(q * (sorted.size() - 1)))];
        };
        feed_p50 = pick(0.50);
        feed_p99 = pick(0.99);
        feed_max = sorted.back();
        feed_ms_.clear();
    }
    std::fprintf(stderr,
                 "[video] src %.1f fps  select 3v %d%%  4v %d%%  5v %d%%  |  screen 3v %d%%  "
                 "4v %d%%  5v %d%%  (n=%d, stats=%s)  |  arrival %.1f ms outliers %d  |  "
                 "feed p50 %.1f p99 %.1f max %.1f (n=%d)  |  present misses +%llu (total %llu)"
                 "  |  render %.2f ui %.2f src %.2f ms\n",
                 source_fps, pct(3), pct(4), pct(5), spct(3), spct(4), spct(5),
                 screen_hold_fill_, presenter_.present_stats_available() ? "yes" : "no",
                 stream_stats_.arrival_ms, stream_stats_.arrival_outliers, feed_p50, feed_p99,
                 feed_max, feed_n, misses - last_present_misses_, misses, phase_render_ms_,
                 phase_ui_ms_, phase_source_ms_);
    last_present_misses_ = misses;
}

bool App::VideoPaced() const {
    // Buffer 0 is the explicit "lowest latency" choice: show a frame the
    // moment it decodes. Otherwise the presentation loop gates frames on the
    // display's own tick counter (4 ticks per 60 fps frame at 240 Hz), which
    // is exact and immune to the drift of a wall-clock grid. DLSS Frame
    // Generation also gets a paced source: the generator expects a steady
    // 60 Hz input, not network jitter. The legacy NVIDIA VFG path has its own
    // generated queue and is fed as fast as it asks.
    return stream_buffer_ > 0 && frame_gen_ < 2;
}

std::wstring App::FindFrameGenRuntime() const {
    namespace fs = std::filesystem;
    std::error_code ec;
    std::vector<fs::path> candidates;

    wchar_t exe_path[MAX_PATH] = {};
    if (::GetModuleFileNameW(nullptr, exe_path, MAX_PATH) > 0) {
        const fs::path exe_dir = fs::path(exe_path).parent_path();
        candidates.push_back(exe_dir / "nvidia-vfx" / "runtime");
        candidates.push_back(exe_dir / "nvidia-vfx");
        candidates.push_back(exe_dir / ".." / ".." / "third_party" / "nvidia-vfx" / "runtime");
    }
    const fs::path cwd = fs::current_path(ec);
    if (!ec) {
        candidates.push_back(cwd / "third_party" / "nvidia-vfx" / "runtime");
        candidates.push_back(cwd / "nvidia-vfx" / "runtime");
    }

    for (const fs::path& dir : candidates) {
        const fs::path dll = dir / "nvVFXVideoFrameGeneration.dll";
        if (fs::exists(dll, ec) && !ec) {
            return fs::absolute(dir, ec).wstring();
        }
        ec.clear();
    }
    return {};
}

bool App::FrameGenAvailable() const {
    return nv_vfx_present_ || nv_vfx_.runtime_loaded();
}

bool App::EnsureFrameGen() {    std::string detail;
    if (!nv_vfx_.runtime_loaded()) {
        const std::wstring dir = FindFrameGenRuntime();
        if (dir.empty()) {
            fg_note_ =
                L"NVIDIA VFX runtime missing: put nvVFXVideoFrameGeneration.dll and its "
                L"dependencies in third_party\\nvidia-vfx\\runtime";
            return false;
        }
        if (!nv_vfx_.LoadRuntime(dir, detail)) {
            fg_note_ = L"NVIDIA VFX runtime: " + Widen(detail);
            return false;
        }
    }
    const int width = profile_.width > 0 ? profile_.width : 1920;
    const int height = profile_.height > 0 ? profile_.height : 1080;
    if (!nv_vfx_.Initialize(presenter_.device(), presenter_.context(), width, height, 1,
                            detail)) {
        fg_note_ = L"NVIDIA VFG init: " + Widen(detail);
        return false;
    }
    fg_ready_ = true;
    fg_note_ = L"NVIDIA VFG ready";
    return true;
}

std::wstring App::FindDlssRuntime() const {
    namespace fs = std::filesystem;
    std::error_code ec;
    std::vector<fs::path> candidates;

    wchar_t exe_path[MAX_PATH] = {};
    if (::GetModuleFileNameW(nullptr, exe_path, MAX_PATH) > 0) {
        const fs::path exe_dir = fs::path(exe_path).parent_path();
        candidates.push_back(exe_dir / "nvidia-ngx" / "runtime");
        candidates.push_back(exe_dir);
        candidates.push_back(exe_dir / ".." / ".." / "third_party" / "nvidia-ngx" / "runtime");
    }
    const fs::path cwd = fs::current_path(ec);
    if (!ec) {
        candidates.push_back(cwd / "third_party" / "nvidia-ngx" / "runtime");
        candidates.push_back(cwd / "nvidia-ngx" / "runtime");
    }
    for (const fs::path& dir : candidates) {
        if (fs::exists(dir / "nvngx_dlss.dll", ec) && !ec) {
            return fs::absolute(dir, ec).wstring();
        }
        ec.clear();
    }
    return {};
}

bool App::DlssAvailable() const {
    return dlss_present_ || dlss_sr_.runtime_loaded();
}

bool App::EnsureDlssSr(int in_width, int in_height) {
    std::string detail;
    if (!dlss_sr_.runtime_loaded()) {
        const std::wstring dir = FindDlssRuntime();
        if (dir.empty()) {
            dlss_note_ =
                L"DLSS runtime missing: put nvngx_dlss.dll in third_party\\nvidia-ngx\\runtime";
            return false;
        }
        if (!dlss_sr_.LoadRuntime(dir, detail)) {
            dlss_note_ = L"DLSS runtime: " + Widen(detail);
            std::fprintf(stderr, "[app] DLSS runtime failed: %s\n", detail.c_str());
            return false;
        }
    }
    const int out_w = presenter_.render_width();
    const int out_h = presenter_.render_height();
    if (in_width <= 0 || in_height <= 0 || out_w <= in_width || out_h <= in_height) {
        dlss_note_ = L"DLSS SR needs an upscale target larger than the stream (1440p or 2160p)";
        std::fprintf(stderr, "[app] DLSS size rejected: %dx%d -> %dx%d\n", in_width, in_height,
                     out_w, out_h);
        return false;
    }
    if (!dlss_sr_.Initialize(presenter_.device(), presenter_.context(), in_width, in_height,
                             out_w, out_h, profile_.dlss_preset,
                             DlssPerfQuality(profile_.dlss_mode),
                             detail)) {
        dlss_note_ = L"DLSS SR init: " + Widen(detail);
        std::fprintf(stderr, "[app] DLSS init failed: %s\n", detail.c_str());
        return false;
    }
    dlss_ready_ = true;
    dlss_dlaa_ = false;
    dlss_in_w_ = in_width;
    dlss_in_h_ = in_height;
    dlss_note_ = L"DLSS SR ready";
    return true;
}

void App::ApplyNeuralSettings() {
    if (profile_.neural_sr > 0) {
        if (!DlssAvailable()) {
            profile_.neural_sr = 0;
            dlss_note_ = L"DLSS SR unavailable: runtime not found";
            SaveProfile();
            return;
        }
        frame_gen_ = 0;
        profile_.frame_gen = 0;
        fg_queue_.clear();
        fg_last_ = {};
        dlss_ready_ = EnsureDlssSrForTarget();
        if (!dlss_ready_) {
            dlss_sr_.Shutdown();
        } else if (profile_.dlss_fg >= 2) {
            // SR feeds the FG worker. Recreate it at the new output size.
            ApplyDlssFrameGen();
            stream_note_ = "DLSS SR + Frame Generation active.";
        }
    } else {
        dlss_ready_ = false;
        dlss_in_w_ = 0;
        dlss_in_h_ = 0;
        dlss_sr_.Shutdown();
        // The FG worker was sized to the SR output. Put it back on the raw
        // stream size instead of feeding it a 1080p frame it cannot accept.
        if (profile_.dlss_fg >= 2) {
            ApplyDlssFrameGen();
        }
    }
    SaveProfile();
}

std::wstring App::FindNrRuntime() const {
    namespace fs = std::filesystem;
    std::error_code ec;
    std::vector<fs::path> candidates;

    wchar_t exe_path[MAX_PATH] = {};
    if (::GetModuleFileNameW(nullptr, exe_path, MAX_PATH) > 0) {
        const fs::path exe_dir = fs::path(exe_path).parent_path();
        candidates.push_back(exe_dir / "nr" / "native");
        candidates.push_back(exe_dir / "neuralscreen" / "native");
        candidates.push_back(exe_dir / ".." / ".." / "third_party" / "neuralscreen" / "native");
    }
    const fs::path cwd = fs::current_path(ec);
    if (!ec) {
        candidates.push_back(cwd / "third_party" / "neuralscreen" / "native");
    }
    // Local development fallback: the NeuralScreen package on this machine.
    if (const char* env = std::getenv("BRP_NR_ROOT")) {
        candidates.push_back(fs::path(env) / "native");
    }
    candidates.push_back(
        fs::path("C:\\Users\\Cosmin\\Desktop\\dlss5overlay\\neuralscreen-v2.6.0-full\\native"));
    for (const fs::path& dir : candidates) {
        if (fs::exists(dir / "nvngx.dll", ec) && !ec &&
            fs::exists(dir / "nvngx_dlssnr.dll", ec) && !ec) {
            return fs::absolute(dir, ec).wstring();
        }
        ec.clear();
    }
    return {};
}

bool App::NrAvailable() const {
    return nr_present_ || nr_.ready();
}

void App::NrStageSize(int* width, int* height) const {
    if (profile_.fg_engine == 1) {
        // RIFE consumes 1080p stream frames; the NR stage must match the engine.
        *width = profile_.width > 0 ? profile_.width : 1920;
        *height = profile_.height > 0 ? profile_.height : 1080;
        return;
    }
    if (profile_.dlss_fg >= 2) {
        // The FG worker consumes the DLSS SR output when both are enabled, so
        // it must run at the SR output size, exactly like a game's render
        // resolution feeding DLSS-G.
        if (profile_.neural_sr > 0 && dlss_ready_ && dlss_sr_.out_width() > 0) {
            *width = dlss_sr_.out_width();
            *height = dlss_sr_.out_height();
        } else {
            *width = profile_.width;
            *height = profile_.height;
        }
    } else {
    const bool sr_stage =
        profile_.neural_sr > 0 && profile_.dlss_mode != 5 && DlssAvailable();
    if (profile_.nr_order == 1 && sr_stage) {
        // NVIDIA order: NR runs after DLSS SR, on the upscaled frame.
        *width = presenter_.render_width();
        *height = presenter_.render_height();
    } else {
        *width = profile_.width;
        *height = profile_.height;
    }
    if (*width <= 0) {
        *width = 1920;
    }
    if (*height <= 0) {
        *height = 1080;
    }
    }
}

void App::DlssInputSize(int src_w, int src_h, int* in_w, int* in_h) const {
    static const int kPercent[6] = {33, 50, 58, 66, 77, 100};
    const int pct = kPercent[std::clamp(profile_.dlss_mode, 0, 5)];
    const int out_w = presenter_.render_width();
    const int out_h = presenter_.render_height();
    if (out_w <= src_w || out_h <= src_h || pct >= 100) {
        *in_w = src_w;
        *in_h = src_h;
        return;
    }
    int w = std::max(2, out_w * pct / 100 / 2 * 2);
    int h = std::max(2, out_h * pct / 100 / 2 * 2);
    *in_w = std::min(w, src_w);
    *in_h = std::min(h, src_h);
}

bool App::EnsureDlssDlaa(int width, int height) {
    if (width <= 0 || height <= 0) {
        dlss_note_ = L"DLAA size is invalid";
        return false;
    }
    std::string detail;
    if (!dlss_sr_.runtime_loaded()) {
        const std::wstring dir = FindDlssRuntime();
        if (dir.empty()) {
            dlss_note_ = L"DLSS runtime missing: nvngx_dlss.dll";
            return false;
        }
        if (!dlss_sr_.LoadRuntime(dir, detail)) {
            dlss_note_ = L"DLSS runtime: " + Widen(detail);
            return false;
        }
    }
    // DLAA is DLSS at 100%: the feature runs at the frame's own size and the
    // network does the anti-aliasing, there is no upscale.
    if (!dlss_sr_.Initialize(presenter_.device(), presenter_.context(), width, height, width,
                             height, profile_.dlss_preset,
                             media::NvDlssSr::PerfQuality::Dlaa, detail)) {
        dlss_note_ = L"DLAA init: " + Widen(detail);
        std::fprintf(stderr, "[app] DLAA init failed: %s\n", detail.c_str());
        return false;
    }
    dlss_ready_ = true;
    dlss_dlaa_ = true;
    dlss_in_w_ = width;
    dlss_in_h_ = height;
    dlss_note_ = L"DLAA ready";
    return true;
}
bool App::EnsureDlssSrForTarget() {
    if (profile_.dlss_mode == 5) {
        return EnsureDlssDlaa(profile_.width > 0 ? profile_.width : 1920,
                              profile_.height > 0 ? profile_.height : 1080);
    }
    int in_w = 0;
    int in_h = 0;
    DlssInputSize(profile_.width > 0 ? profile_.width : 1920,
                  profile_.height > 0 ? profile_.height : 1080, &in_w, &in_h);
    return EnsureDlssSr(in_w, in_h);
}

bool App::RunDlssSr(const media::DecodedFrame& frame, media::NvDlssSr::Frame& out) {
    if (profile_.neural_sr <= 0 || frame.rgba.empty()) {
        return false;
    }
    const bool dlaa = profile_.dlss_mode == 5;
    int in_w = frame.width;
    int in_h = frame.height;
    const uint8_t* input = frame.rgba.data();
    if (!dlaa) {
        DlssInputSize(frame.width, frame.height, &in_w, &in_h);
        if (in_w != frame.width || in_h != frame.height) {
            sr_scaled_.resize(static_cast<size_t>(in_w) * in_h * 4);
            media::DownscaleRgba(frame.rgba.data(), frame.width, frame.height, sr_scaled_.data(),
                                 in_w, in_h, scale_tmp_);
            input = sr_scaled_.data();
        }
    }

    std::string detail;
    if (dlaa) {
        if (!dlss_ready_ || !dlss_dlaa_ || dlss_in_w_ != frame.width ||
            dlss_in_h_ != frame.height) {
            dlss_ready_ = EnsureDlssDlaa(frame.width, frame.height);
        }
        if (dlss_ready_ && dlss_sr_.Process(input, frame.width, frame.height, out, detail)) {
            return true;
        }
    } else {
        if (!dlss_ready_ || dlss_dlaa_ || dlss_in_w_ != in_w || dlss_in_h_ != in_h) {
            dlss_ready_ = EnsureDlssSr(in_w, in_h);
        }
        if (dlss_ready_ && dlss_sr_.Process(input, in_w, in_h, out, detail)) {
            return true;
        }
    }

    dlss_note_ = L"DLSS: " + Widen(detail);
    dlss_ready_ = false;
    dlss_sr_.Shutdown();
    return false;
}

void App::DetachFgWindow() {
    if (fg_window_) {
        ::ShowWindow(fg_window_, SW_HIDE);
        ::SetParent(fg_window_, nullptr);
        fg_window_ = nullptr;
    }
    fg_visible_ = false;
}

bool App::AttachFgWindow() {
    // Win32 window re-parenting must stay on the UI thread; the boot worker
    // prepares the presenter but the normal frame attaches it after boot.
    if (booting_) {
        return false;
    }
    return AttachFgWindowNow();
}

bool App::AttachFgWindowNow() {
    if (!nr_.present_mode() || nr_.worker_pid() == 0 || !window_) {
        return false;
    }
    if (fg_window_) {
        return true;
    }
    struct Find {
        DWORD pid;
        HWND hwnd;
    } find{nr_.worker_pid(), nullptr};
    ::EnumWindows(
        [](HWND hwnd, LPARAM param) -> BOOL {
            auto* f = reinterpret_cast<Find*>(param);
            DWORD pid = 0;
            ::GetWindowThreadProcessId(hwnd, &pid);
            if (pid == f->pid && ::IsWindowVisible(hwnd) && ::GetWindow(hwnd, GW_OWNER) == nullptr) {
                f->hwnd = hwnd;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&find));
    if (!find.hwnd) {
        return false;
    }
    ::SetParent(find.hwnd, window_->hwnd());
    LONG_PTR style = ::GetWindowLongPtrW(find.hwnd, GWL_STYLE);
    style &= ~(WS_POPUP | WS_CAPTION | WS_THICKFRAME);
    style |= WS_CHILD | WS_VISIBLE;
    ::SetWindowLongPtrW(find.hwnd, GWL_STYLE, style);
    ::SetWindowPos(find.hwnd, HWND_TOP, 0, 0, static_cast<int>(ui_w_),
                   static_cast<int>(ui_h_), SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_FRAMECHANGED);
    fg_window_ = find.hwnd;
    fg_visible_ = true;
    return true;
}

void App::ApplyDlssFrameGen() {
    if (profile_.fg_engine == 1) {
        // RIFE runs in-process; the worker must stay out of the way.
        return;
    }
    std::string detail;
    if (profile_.dlss_fg >= 2) {
        // The worker's presenter owns the screen: everything else that also
        // presents must step aside.
        frame_gen_ = 0;
        profile_.frame_gen = 0;
        fg_queue_.clear();
        fg_last_ = {};
        // DLSS SR can feed the FG worker, exactly like a game's render
        // resolution feeding DLSS-G. Make sure the SR feature exists first so
        // the worker can be sized to its output.
        if (profile_.neural_sr > 0) {
            if (!EnsureDlssSrForTarget()) {
                const std::wstring note = dlss_note_;
                profile_.neural_sr = 0;
                dlss_ready_ = false;
                dlss_sr_.Shutdown();
                dlss_note_ = note;
                SaveProfile();
            }
        }
        int fw = 0;
        int fh = 0;
        NrStageSize(&fw, &fh);
        if (nr_.ready() && (nr_in_w_ != fw || nr_in_h_ != fh)) {
            // The shared memory is sized once; a new SR output size needs a
            // fresh worker.
            StopNrThread();
            nr_.Stop();
            nr_ready_ = false;
            DetachFgWindow();
        }
        if (!EnsureNr()) {
            profile_.dlss_fg = 0;
            SaveProfile();
            return;
        }
        nr_.SetFrameGeneration(profile_.dlss_fg);
        fg_mult_live_ = profile_.dlss_fg;
        nr_.SetNeuralBypass(profile_.neural_nr == 0);
        if (!nr_.present_mode() && !nr_.EnablePresent(fw, fh, detail)) {
            stream_note_ = "DLSS Frame Generation: " + detail;
            nr_.SetFrameGeneration(0);
            profile_.dlss_fg = 0;
            SaveProfile();
            return;
        }
        AttachFgWindow();
    } else {
        nr_.SetFrameGeneration(0);
        nr_.DisablePresent();
        DetachFgWindow();
        if (nr_.ready()) {
            ReconfigureNr();
        }
    }
    SaveProfile();
}

void App::ApplyReflex() {    std::string detail;
    if (profile_.reflex > 0 && !reflex_loaded_) {
        reflex_loaded_ = reflex_.Load(detail);
        if (!reflex_loaded_) {
            stream_note_ = "Reflex unavailable: " + detail;
            profile_.reflex = 0;
            SaveProfile();
            return;
        }
    }
    if (!reflex_.SetMode(presenter_.device(), profile_.reflex, detail)) {
        stream_note_ = "Reflex: " + detail;
        profile_.reflex = 0;
        SaveProfile();
    }
}

bool App::EnsureNr() {
    std::string detail;
    if (nr_.ready()) {
        return true;
    }
    const std::wstring dir = FindNrRuntime();
    if (dir.empty()) {
        nr_note_ = L"NR runtime missing: native\\nvngx.dll + nvngx_dlssnr.dll";
        return false;
    }
    int width = 0;
    int height = 0;
    NrStageSize(&width, &height);
    if (width > 2560 || height > 1440) {
        nr_note_ = L"NR is capped at 2560x1440; the target is too large for a post-DLSS stage";
        return false;
    }
    media::NvDlssnr::Options options;
    options.style = profile_.nr_style;
    options.intensity = profile_.nr_intensity / 100.0f;
    options.local_tone = profile_.nr_tone / 100.0f;
    options.local_structure = profile_.nr_structure / 100.0f;
    options.skin_structure = profile_.nr_skin / 100.0f;
    options.auto_mask = profile_.nr_mask != 0;
    options.ui_correction = profile_.nr_ui != 0;
    options.passes = std::clamp(profile_.nr_passes, 1, 4);
    options.work_scale = std::clamp(profile_.nr_scale, 0, 100) / 100.0f;
    if (!nr_.Start(dir + L"\\nvngx.dll", dir, width, height, options, detail)) {
        nr_note_ = L"NR init: " + Widen(detail);
        std::fprintf(stderr, "[app] NR init failed: %s\n", detail.c_str());
        return false;
    }
    nr_ready_ = true;
    nr_in_w_ = width;
    nr_in_h_ = height;
    nr_note_ = L"NR active";
    nr_dropped_ = 0;
    nr_shown_ = 0;
    nr_last_ms_ = 0.0;
    StartNrThread();
    if (profile_.dlss_fg >= 2 && profile_.fg_engine != 1) {
        std::string fg_detail;
        nr_.SetFrameGeneration(profile_.dlss_fg);
        fg_mult_live_ = profile_.dlss_fg;
        nr_.SetNeuralBypass(profile_.neural_nr == 0);
        if (!nr_.EnablePresent(width, height, fg_detail)) {
            nr_note_ = L"FG presenter: " + Widen(fg_detail);
            std::fprintf(stderr, "[app] FG presenter refused: %s\n", fg_detail.c_str());
        } else {
            AttachFgWindow();
        }
    }
    std::fprintf(stderr, "[app] NR worker ready %dx%d, style %d, scale %d%%, passes %d\n", width,
                 height, profile_.nr_style, profile_.nr_scale, profile_.nr_passes);
    return true;
}

void App::DisableNr() {
    // The worker thread is the only caller of nr_.Process; join it before the
    // worker handles are closed.
    StopNrThread();
    nr_.Stop();
    nr_ready_ = false;
    nr_out_.clear();
    nr_dropped_ = 0;
    nr_shown_ = 0;
    nr_last_ms_ = 0.0;
}

void App::StartNrThread() {
    StopNrThread();
    {
        std::lock_guard<std::mutex> lock(nr_mutex_);
        nr_run_ = true;
        nr_pending_.reset();
        nr_latest_.reset();
        nr_failed_ = false;
        nr_error_.clear();
    }
    nr_thread_ = std::thread(&App::NrThreadMain, this);
}

void App::StopNrThread() {
    {
        std::lock_guard<std::mutex> lock(nr_mutex_);
        nr_run_ = false;
        nr_pending_.reset();
    }
    nr_cv_.notify_all();
    if (nr_thread_.joinable()) {
        nr_thread_.join();
    }
    {
        std::lock_guard<std::mutex> lock(nr_mutex_);
        nr_latest_.reset();
    }
}

void App::NrThreadMain() {
    for (;;) {
        NrFrame job;
        {
            std::unique_lock<std::mutex> lock(nr_mutex_);
            nr_cv_.wait(lock, [this] { return !nr_run_ || nr_pending_.has_value(); });
            if (!nr_run_) {
                return;
            }
            job = std::move(*nr_pending_);
            nr_pending_.reset();
        }

        std::string detail;
        const double started = NowMs();
        // Process() copies the pixels into the worker's mapping before it
        // writes the result, so passing the job buffer as both input and
        // output is safe and avoids a second 8 MB allocation per frame.
        if (!nr_.Process(job.rgba.data(), job.width, job.height, job.rgba, detail)) {
            std::lock_guard<std::mutex> lock(nr_mutex_);
            nr_failed_ = true;
            nr_error_ = detail;
            return;
        }
        job.ms = NowMs() - started;
        if (!nr_.present_mode()) {
            std::lock_guard<std::mutex> lock(nr_mutex_);
            nr_latest_ = std::move(job);
        }
    }
}

void App::SubmitNrFrame(media::DecodedFrame& frame) {
    if (frame.rgba.empty()) {
        return;
    }
    SubmitNrFrameData(std::move(frame.rgba), frame.width, frame.height, frame.pq);
}

void App::SubmitNrFrameData(std::vector<uint8_t>&& rgba, int width, int height, bool pq) {
    if (rgba.empty() || width <= 0 || height <= 0) {
        return;
    }
    if (last_feed_at_ >= 0.0) {
        feed_ms_.push_back(static_cast<float>((clock_ - last_feed_at_) * 1000.0));
        while (feed_ms_.size() > 900) {
            feed_ms_.pop_front();
        }
    }
    last_feed_at_ = clock_;
    if (profile_.dlss_fg >= 2 && nr_.ready() && nr_.present_mode()) {
        ++fg_window_uniques_;
    }
    NrFrame job;
    job.rgba = std::move(rgba);
    job.width = width;
    job.height = height;
    job.pq = pq;
    {
        std::lock_guard<std::mutex> lock(nr_mutex_);
        // One-frame queue: the newest frame wins. The replaced one was not
        // processed, so it is a dropped source frame, not a lost result.
        if (nr_pending_) {
            ++nr_dropped_;
        }
        nr_pending_ = std::move(job);
    }
    nr_cv_.notify_one();
}

int App::FgTargetHz() const {
    const int hz = std::max(24, present_hz());
    int target = profile_.fg_target;
    if (target <= 0) {
        target = hz >= 120 ? 120 : (hz >= 90 ? 90 : 60);
    }
    return std::min(target, hz);
}

int App::FgChosenMultiplier(double content_hz, int target) const {
    if (content_hz <= 1.0 || target <= 0) {
        return 0;
    }
    // No gaps to fill: the content already reaches the requested rate.
    if (content_hz >= target * 0.95) {
        return 0;
    }
    int best = 0;
    for (int m = 2; m <= 6; ++m) {
        if (content_hz * m <= target * 1.08 + 0.5) {
            best = m;
        }
    }
    return best;
}

bool App::FrameIsDuplicate(const media::DecodedFrame& frame) {
    if (frame.rgba.empty() || frame.width <= 0 || frame.height <= 0) {
        return false;
    }
    // A duplicate from a 30 fps game is coded as skipped blocks, so the decoded
    // picture carries no motion. Compare a 64x64 sample with a small tolerance:
    // an exact hash missed pairs whose decoder output differed by a count or two.
    constexpr int kCols = 64;
    constexpr int kRows = 64;
    std::array<uint32_t, 4096> samples{};
    const size_t stride = static_cast<size_t>(frame.width) * 4;
    for (int gy = 0; gy < kRows; ++gy) {
        const size_t y = (static_cast<size_t>(gy) * frame.height) / kRows;
        const uint8_t* row = frame.rgba.data() + y * stride;
        for (int gx = 0; gx < kCols; ++gx) {
            const size_t x = (static_cast<size_t>(gx) * frame.width) / kCols;
            std::memcpy(&samples[static_cast<size_t>(gy) * kCols + gx],
                        row + x * 4, sizeof(uint32_t));
        }
    }
    bool duplicate = false;
    if (frame_sig_valid_) {
        long long sum = 0;
        int outliers = 0;
        for (size_t i = 0; i < samples.size(); ++i) {
            const uint32_t a = samples[i];
            const uint32_t b = frame_samples_[i];
            for (int c = 0; c < 3; ++c) {
                const int d = std::abs(static_cast<int>((a >> (8 * c)) & 0xFF) -
                                       static_cast<int>((b >> (8 * c)) & 0xFF));
                sum += d;
                if (d > 8) {
                    ++outliers;
                }
            }
        }
        const double mean = static_cast<double>(sum) /
                            (static_cast<double>(samples.size()) * 3.0);
        duplicate = mean < 1.2 && outliers < 96;  // under ~1.5 % of channels
    }
    frame_samples_ = samples;
    frame_sig_valid_ = true;
    return duplicate;
}
bool App::ShouldDropDuplicate(const media::DecodedFrame& frame) {
    UpdateInterpolationMode();
    if (profile_.dlss_fg < 2 || !nr_ready_ || !nr_.present_mode()) {
        return false;
    }
    const bool duplicate = FrameIsDuplicate(frame);
    if (!duplicate || fg_mult_live_ < 2) {
        return false;
    }
    ++fg_dupes_;
    // A still image must not starve the worker: feed at least every 80 ms.
    return last_feed_at_ >= 0.0 && clock_ - last_feed_at_ < 0.08;
}

void App::UpdateInterpolationMode() {
    if (profile_.dlss_fg < 2 || !nr_ready_ || !nr_.present_mode()) {
        return;
    }
    if (fg_window_at_ < 0.0) {
        fg_window_at_ = clock_;
        fg_window_uniques_ = 0;
        return;
    }
    const double elapsed = clock_ - fg_window_at_;
    if (elapsed < 0.75) {
        return;
    }
    const double hz = static_cast<double>(fg_window_uniques_) / elapsed;
    fg_content_hz_ = fg_content_hz_ > 0.0 ? fg_content_hz_ * 0.45 + hz * 0.55 : hz;
    fg_window_uniques_ = 0;
    fg_window_at_ = clock_;

    const int target = FgTargetHz();
    int candidate = FgChosenMultiplier(fg_content_hz_, target);
    // If the worker cannot hold the requested rate, step the multiplier down.
    // At 1440p, 4x from 30 fps content sustains about 92 fps, so 3x (90) is the
    // honest choice; the next window confirms the new rate before going lower.
    if (candidate > 2 && fg_worker_fps_ > 0.0 &&
        fg_worker_fps_ < fg_content_hz_ * candidate * 0.88) {
        --candidate;
    }
    static const bool probe = std::getenv("BRP_FG_MULT_PROBE") != nullptr;
    static double probe_at = 0.0;
    if (probe) {
        // Test hook: force a live change every 4 s to prove the worker accepts
        // a new multiplier mid-session.
        if (clock_ - probe_at < 4.0) {
            return;
        }
        probe_at = clock_;
        candidate = fg_mult_live_ >= 2 ? 0 : 4;
    }
    if (candidate == fg_mult_live_) {
        fg_pending_streak_ = 0;
        return;
    }
    // Two windows in a row, so a menu that briefly runs at 60 fps does not
    // flip the multiplier back and forth.
    if (candidate == fg_pending_mult_) {
        ++fg_pending_streak_;
    } else {
        fg_pending_mult_ = candidate;
        fg_pending_streak_ = 1;
    }
    if (fg_pending_streak_ < 2) {
        return;
    }
    fg_pending_streak_ = 0;
    fg_mult_live_ = candidate;
    nr_.SetFrameGeneration(candidate);
    std::fprintf(stderr, "[fg] content %.1f fps -> %s for %d fps output\n",
                 fg_content_hz_, candidate < 2 ? "pass-through" : "interpolation",
                 FgTargetHz());
}
bool App::EnsureRife() {
    if (rife_ && rife_->ready()) {
        return true;
    }
    StopRife();
    std::string runtime = EnvString("BRP_TENSORRT_DIR");
    wchar_t exe_path[MAX_PATH] = {};
    ::GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    const std::filesystem::path exe_dir = std::filesystem::path(exe_path).parent_path();
    if (runtime.empty()) {
        // Portable location: drop the TensorRT/CUDA DLLs in rife\runtime next to
        // the exe. On the reference machine they live in the vs-mlrt install,
        // which stays a fallback; on another PC it simply will not exist.
        const std::filesystem::path local = exe_dir / "rife" / "runtime";
        if (std::filesystem::exists(local / "nvinfer_10.dll")) {
            runtime = local.string();
        }
    }
    if (runtime.empty()) {
        // Written by the one-click setup: the TensorRT install it found.
        std::ifstream marker(exe_dir / "rife" / "runtime.txt");
        std::string line;
        if (marker && std::getline(marker, line) && !line.empty() &&
            std::filesystem::exists(std::filesystem::path(line) / "nvinfer_10.dll")) {
            runtime = line;
        }
    }
    if (runtime.empty()) {
        const std::string fallback =
            "C:\\mpv-upscaler-video-player\\vs-plugins\\vsmlrt-cuda";
        if (std::filesystem::exists(fallback + "\\nvinfer_10.dll")) {
            runtime = fallback;
        }
    }
    std::string engine = EnvString("BRP_RIFE_ENGINE");
    if (engine.empty()) {
        const std::filesystem::path candidate = exe_dir / "rife" / "rife425_1080.engine";
        if (std::filesystem::exists(candidate)) {
            engine = candidate.string();
        }
    }
    std::fprintf(stderr, "[rife] runtime '%s' engine '%s'\n", runtime.c_str(),
                 engine.c_str());
    if (runtime.empty() || engine.empty()) {
        rife_note_ = L"RIFE needs BRP_TENSORRT_DIR and an engine (rife\\rife425_1080.engine "
                     L"next to the exe, or BRP_RIFE_ENGINE)";
        std::fprintf(stderr, "[rife] missing paths; not loading\n");
        return false;
    }

    auto instance = std::make_unique<media::Rife>();
    std::string detail;
    if (!instance->Load(runtime, engine, profile_.width, profile_.height, detail)) {
        rife_note_ = L"RIFE: " + Widen(detail);
        std::fprintf(stderr, "[rife] load failed: %s\n", detail.c_str());
        // An engine built for another TensorRT version or GPU cannot be fixed
        // in place. Remove it so Home offers "Set up RIFE" and rebuilds one for
        // this machine instead of failing every session.
        if (detail.find("deserialize") != std::string::npos ||
            detail.find("another TensorRT") != std::string::npos) {
            std::error_code ec;
            std::filesystem::remove(std::filesystem::path(engine), ec);
            rife_note_ = L"RIFE engine does not match this GPU; press Set up RIFE to "
                         L"rebuild it.";
        }
        return false;
    }
    rife_ = std::move(instance);
    {
        std::lock_guard<std::mutex> lock(rife_mutex_);
        rife_run_ = true;
        rife_jobs_.clear();
        rife_out_.clear();
    }
    rife_has_prev_ = false;
    rife_prev_.clear();
    rife_thread_ = std::thread(&App::RifeThreadMain, this);
    rife_note_ = L"RIFE " + std::to_wstring(rife_->width()) + L"x" +
                 std::to_wstring(rife_->height()) + L" ready";
    std::fprintf(stderr, "[rife] engine %dx%d ready\n", rife_->width(), rife_->height());
    return true;
}

void App::StopRife() {
    {
        std::lock_guard<std::mutex> lock(rife_mutex_);
        rife_run_ = false;
        rife_jobs_.clear();
        rife_out_.clear();
    }
    rife_cv_.notify_all();
    if (rife_thread_.joinable()) {
        rife_thread_.join();
    }
    rife_.reset();
    rife_has_prev_ = false;
    rife_prev_.clear();
}

void App::RifeThreadMain() {
    for (;;) {
        RifePair job;
        {
            std::unique_lock<std::mutex> lock(rife_mutex_);
            rife_cv_.wait(lock, [this] { return !rife_run_ || !rife_jobs_.empty(); });
            if (!rife_run_) {
                return;
            }
            job = std::move(rife_jobs_.front());
            rife_jobs_.pop_front();
        }
        if (!rife_) {
            continue;
        }
        std::vector<uint8_t> mid;
        std::string detail;
        if (!rife_->Process(job.a.data(), job.b.data(), 0.5f, mid, detail)) {
            std::fprintf(stderr, "[rife] %s\n", detail.c_str());
            continue;
        }
        RifeOut out;
        out.rgba = std::move(mid);
        out.width = rife_->width();
        out.height = rife_->height();
        out.pq = false;
        // The held frame was shown when its successor arrived; the midpoint
        // belongs half an interval after that.
        out.due = job.base_due + std::max(0.0, (job.t1 - job.t0) * 0.5);
        {
            std::lock_guard<std::mutex> lock(rife_mutex_);
            rife_out_.push_back(std::move(out));
            while (rife_out_.size() > 4) {
                rife_out_.pop_front();
            }
        }
        rife_ms_ = rife_->last_ms();
        ++rife_generated_;
    }
}

void App::RifePush(media::DecodedFrame& frame, bool unique) {
    if (!rife_ || !rife_->ready() || frame.rgba.empty()) {
        return;
    }
    if (!unique && FrameIsDuplicate(frame)) {
        ++rife_dupes_;
        return;  // an identical pair has nothing to interpolate
    }
    static const bool sim30 = std::getenv("BRP_RIFE_SIM30") != nullptr;
    if (sim30 && (++rife_sim_skip_ % 2) != 0) {
        return;  // test hook: halve a 60 fps source so the 30 -> 60 path runs
    }
    // Content rate, for the status text: unique frames over a 0.75 s window.
    if (fg_window_at_ < 0.0) {
        fg_window_at_ = clock_;
        fg_window_uniques_ = 0;
    }
    ++fg_window_uniques_;
    if (clock_ - fg_window_at_ >= 0.75) {
        const double hz =
            static_cast<double>(fg_window_uniques_) / (clock_ - fg_window_at_);
        fg_content_hz_ = fg_content_hz_ > 0.0 ? fg_content_hz_ * 0.4 + hz * 0.6 : hz;
        fg_window_uniques_ = 0;
        fg_window_at_ = clock_;
    }
    if (!rife_has_prev_) {
        rife_prev_ = frame.rgba;
        rife_prev_w_ = frame.width;
        rife_prev_h_ = frame.height;
        rife_prev_pq_ = frame.pq;
        rife_prev_t_ = clock_;
        rife_has_prev_ = true;
        return;
    }

    const double t1 = clock_;
    const double interval = t1 - rife_prev_t_;
    // Pass-through, or 60 fps content where a generated mid-frame adds nothing
    // (and would not fit the 8 ms budget). Show the frame as it arrives.
    static const bool force_gen = std::getenv("BRP_RIFE_FORCE_GEN") != nullptr;
    if (!force_gen && (profile_.rife_output == 0 || (interval > 0.0 && interval < 0.025))) {
        RifeOut direct;
        direct.rgba = frame.rgba;
        direct.width = frame.width;
        direct.height = frame.height;
        direct.pq = frame.pq;
        direct.due = t1;
        {
            std::lock_guard<std::mutex> lock(rife_mutex_);
            rife_out_.push_back(std::move(direct));
            while (rife_out_.size() > 3) {
                rife_out_.pop_front();
            }
        }
        rife_prev_ = frame.rgba;
        rife_prev_w_ = frame.width;
        rife_prev_h_ = frame.height;
        rife_prev_pq_ = frame.pq;
        rife_prev_t_ = t1;
        return;
    }

    RifePair pair;
    pair.a = std::move(rife_prev_);
    pair.b = frame.rgba;
    pair.width = frame.width;
    pair.height = frame.height;
    pair.t0 = rife_prev_t_;
    pair.t1 = t1;

    // The frame we were holding is shown now: this is the one source interval
    // of delay that lets RIFE see both ends of the pair.
    // Schedule the held frame late enough that the midpoint, which cannot be
    // ready before the engine finishes (rife_ms_), lands exactly half an
    // interval after it. Without this the cadence is 22 ms then 11 ms, which
    // reads as 30 fps with a stutter.
    const double half = std::max(0.0, (t1 - pair.t0) * 0.5);
    const double offset = std::max(0.0, rife_ms_ / 1000.0 - half);
    RifeOut held;
    held.rgba = pair.a;
    held.width = pair.width;
    held.height = pair.height;
    held.pq = rife_prev_pq_;
    held.due = t1 + offset;
    pair.base_due = held.due;
    {
        std::lock_guard<std::mutex> lock(rife_mutex_);
        rife_out_.push_back(std::move(held));
        while (rife_out_.size() > 4) {
            rife_out_.pop_front();
        }
        rife_jobs_.push_back(std::move(pair));
        while (rife_jobs_.size() > 1) {
            rife_jobs_.pop_front();
        }
    }
    rife_cv_.notify_one();

    rife_prev_ = frame.rgba;
    rife_prev_w_ = frame.width;
    rife_prev_h_ = frame.height;
    rife_prev_pq_ = frame.pq;
    rife_prev_t_ = t1;
}

void App::RifePresentDue() {
    std::vector<uint8_t> pick;
    int w = 0;
    int h = 0;
    bool pq = false;
    {
        std::lock_guard<std::mutex> lock(rife_mutex_);
        while (!rife_out_.empty() && rife_out_.front().due <= clock_) {
            pick = std::move(rife_out_.front().rgba);
            w = rife_out_.front().width;
            h = rife_out_.front().height;
            pq = rife_out_.front().pq;
            rife_out_.pop_front();
        }
    }
    if (!pick.empty()) {
        presenter_.SetSourceFinal(false);
        presenter_.SetSourceFrame(pick.data(), w, h, pq);
        ++rife_shown_;
        if (rife_last_show_ >= 0.0) {
            const double dt = clock_ - rife_last_show_;
            rife_interval_sum_ += dt;
            rife_interval_max_ = std::max(rife_interval_max_, dt);
            ++rife_interval_n_;
        }
        rife_last_show_ = clock_;
    }
    if (rife_rate_at_ < 0.0) {
        rife_rate_at_ = clock_;
        rife_shown_ = 0;
    } else if (clock_ - rife_rate_at_ >= 1.0) {
        rife_out_fps_ = static_cast<double>(rife_shown_) / (clock_ - rife_rate_at_);
        rife_rate_at_ = clock_;
        rife_shown_ = 0;
        if (std::getenv("BRP_FG_PROBE")) {
            const double avg = rife_interval_n_ > 0
                                   ? rife_interval_sum_ * 1000.0 / rife_interval_n_
                                   : 0.0;
            std::fprintf(stderr,
                         "[rife] %.0f fps shown, %d generated, %.1f ms/frame, "
                         "cadence avg %.2f ms max %.2f ms, content %.1f fps, dupes %d\n",
                         rife_out_fps_, rife_generated_, rife_ms_, avg,
                         rife_interval_max_ * 1000.0, fg_content_hz_, rife_dupes_);
            rife_generated_ = 0;
            rife_interval_sum_ = 0.0;
            rife_interval_max_ = 0.0;
            rife_interval_n_ = 0;
            rife_dupes_ = 0;
        }
    }
}

bool App::TakeNrResult(int& width, int& height, bool& pq) {
    std::lock_guard<std::mutex> lock(nr_mutex_);
    if (!nr_latest_) {
        return false;
    }
    nr_out_.swap(nr_latest_->rgba);
    width = nr_latest_->width;
    height = nr_latest_->height;
    pq = nr_latest_->pq;
    nr_last_ms_ = nr_latest_->ms;
    nr_latest_.reset();
    ++nr_shown_;
    return true;
}

bool App::NrFailed(std::string& error) {
    std::lock_guard<std::mutex> lock(nr_mutex_);
    if (!nr_failed_) {
        return false;
    }
    error = nr_error_;
    return true;
}

bool App::ReconfigureNr() {
    if (!nr_.ready()) {
        return EnsureNr();
    }
    media::NvDlssnr::Options options;
    options.style = profile_.nr_style;
    options.intensity = profile_.nr_intensity / 100.0f;
    options.local_tone = profile_.nr_tone / 100.0f;
    options.local_structure = profile_.nr_structure / 100.0f;
    options.skin_structure = profile_.nr_skin / 100.0f;
    options.auto_mask = profile_.nr_mask != 0;
    options.ui_correction = profile_.nr_ui != 0;
    options.passes = std::clamp(profile_.nr_passes, 1, 4);
    options.work_scale = std::clamp(profile_.nr_scale, 0, 100) / 100.0f;
    nr_.SetNeuralBypass(profile_.neural_nr == 0);

    int stage_w = 0;
    int stage_h = 0;
    NrStageSize(&stage_w, &stage_h);
    if (stage_w > 2560 || stage_h > 1440) {
        nr_note_ = L"NR is capped at 2560x1440; the target is too large for a post-DLSS stage";
        return false;
    }
    if (stage_w != nr_in_w_ || stage_h != nr_in_h_) {
        // The frame size changes the shared memory, which RNSZ cannot resize:
        // restart the worker once instead.
        StopNrThread();
        nr_.Stop();
        nr_ready_ = false;
        return EnsureNr();
    }

    // The worker rebuilds or retunes the feature inside RNSZ; no Process call
    // may be in flight while it does. Pausing the NR thread costs one frame,
    // a full worker restart costs seconds of frozen picture.
    StopNrThread();
    std::string detail;
    const bool ok = nr_.Reconfigure(options, detail);
    StartNrThread();
    if (!ok) {
        nr_note_ = L"NR reconfigure: " + Widen(detail);
        std::fprintf(stderr, "[app] NR reconfigure failed: %s\n", detail.c_str());
        return false;
    }
    nr_note_ = L"NR active";
    return true;
}

void App::ApplyNrSettings() {
    if (profile_.neural_nr > 0) {
        if (!NrAvailable()) {
            profile_.neural_nr = 0;
            nr_note_ = L"NR unavailable: runtime not found";
            SaveProfile();
            return;
        }
        frame_gen_ = 0;
        profile_.frame_gen = 0;
        fg_queue_.clear();
        fg_last_ = {};
        // DLSS SR is not a rival of NR: it can run on the NR result. Keep the
        // selected upscaler and make sure its feature matches this geometry.
        if (profile_.neural_sr > 0 && DlssAvailable()) {
            dlss_ready_ = EnsureDlssSrForTarget();
            if (!dlss_ready_) {
                dlss_sr_.Shutdown();
            }
        }
        if (!ReconfigureNr()) {
            profile_.neural_nr = 0;
            DisableNr();
        }
    } else {
        DisableNr();
    }
    SaveProfile();
}

App::~App() {
    StopRife();
    boot_cancel_.store(true);
    if (boot_worker_.joinable()) {
        boot_worker_.join();
    }
    if (discover_thread_.joinable()) {
        discover_thread_.join();
    }
    if (psn_thread_.joinable()) {
        psn_thread_.join();
    }
    if (address_thread_.joinable()) {
        address_thread_.join();
    }
    if (session_thread_.joinable()) {
        session_thread_.join();
    }
    StopNrThread();
    StopPadThread();
}

void App::StartPadThread() {
    if (pad_run_.load()) {
        return;
    }
    pad_run_.store(true);
    pad_thread_ = std::thread(&App::PadThreadMain, this);
}

void App::StopPadThread() {
    if (!pad_run_.load()) {
        return;
    }
    pad_run_.store(false);
    if (pad_thread_.joinable()) {
        pad_thread_.join();
    }
}

void App::PadThreadMain() {
    DWORD task_index = 0;
    HANDLE mmcss = ::AvSetMmThreadCharacteristicsW(L"Games", &task_index);

    // High-resolution waitable timer: Sleep()/sleep_for() quantise to the
    // 15.6 ms tick unless the timer resolution is raised, which would turn a
    // "250 Hz" loop into a ~64 Hz one.
    HANDLE timer = ::CreateWaitableTimerExW(nullptr, nullptr,
                                            CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                            TIMER_ALL_ACCESS);

    auto next = std::chrono::steady_clock::now();
    const bool probe = std::getenv("BRP_PAD_PROBE") != nullptr;
    auto probe_at = std::chrono::steady_clock::now();
    int probe_loops = 0;
    while (pad_run_.load()) {
        const int hz = std::clamp(pad_rate_hz_.load(), 60, 1000);
        const auto period = std::chrono::microseconds(1000000 / hz);
        next += period;
        const auto now = std::chrono::steady_clock::now();
        if (next < now - period) {
            next = now + period;
        }

        pad_.Poll();
        if (stream_active_) {
            streaming_.SendController(pad_.snapshot());

            const uint32_t version = streaming_.feedback_version();
            bool feedback_changed = false;
            if (version != feedback_version_) {
                feedback_version_ = version;
                pad_feedback_ = streaming_.feedback();
                feedback_changed = true;
            }

            // PS5 haptics: convert the 3 kHz stereo envelope to motor values the
            // same way chiaki-ng does (threshold 100, intensity scale, min 512,
            // byte via >> 8). Without this a DualSense stays silent in PS5 games.
            uint16_t haptics_l = 0;
            uint16_t haptics_r = 0;
            if (streaming_.TakeHaptics(haptics_l, haptics_r)) {
                last_haptics_at_ = clock_;
                if (profile_.haptic_intensity > 0) {
                    double scale = 1.0;
                    switch (profile_.haptic_intensity) {
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
                            scaled = 512.0;  // chiaki's minimum for 9-bit shift
                        }
                        const int value = static_cast<int>(scaled) >> 8;
                        return static_cast<uint8_t>(std::clamp(value, 0, 255));
                    };
                    pad_feedback_.rumble = true;
                    pad_feedback_.haptics = true;
                    pad_feedback_.strong = to_motor(haptics_l);
                    pad_feedback_.weak = to_motor(haptics_r);
                    feedback_changed = true;
                }
            } else if (pad_feedback_.haptics && last_haptics_at_ >= 0.0 &&
                       clock_ - last_haptics_at_ > 0.1) {
                // The haptics stream stopped: release the motors instead of
                // leaving them at the last value.
                pad_feedback_.rumble = false;
                pad_feedback_.haptics = false;
                pad_feedback_.strong = 0;
                pad_feedback_.weak = 0;
                last_haptics_at_ = -1.0;
                feedback_changed = true;
            }

            if (feedback_changed) {
                if (!console_rumble_) {
                    pad_feedback_.rumble = false;
                }
                if (!console_triggers_) {
                    pad_feedback_.triggers = false;
                }
                pad_.SetConsoleFeedback(pad_feedback_);
            }
        }
        last_send_ok_ = pad_.PumpOutput();

        if (probe) {
            ++probe_loops;
            const auto probe_now = std::chrono::steady_clock::now();
            if (probe_now - probe_at >= std::chrono::seconds(1)) {
                const double seconds =
                    std::chrono::duration<double>(probe_now - probe_at).count();
                const PadState state = pad_.snapshot();
                std::fprintf(stderr, "[pad] %.0f loops/s (target %d Hz, %s)\n",
                             probe_loops / seconds, hz,
                             pad_.is_bluetooth() ? "bluetooth" : "usb");
                std::fprintf(stderr,
                             "[pad] gyro %.1f %.1f %.1f dps | accel %.2f %.2f %.2f g\n",
                             state.gyro_dps[0], state.gyro_dps[1], state.gyro_dps[2],
                             state.accel_g[0], state.accel_g[1], state.accel_g[2]);
                probe_loops = 0;
                probe_at = probe_now;
            }
        }

        if (timer) {
            const auto remaining = std::chrono::duration_cast<std::chrono::nanoseconds>(
                next - std::chrono::steady_clock::now());
            LARGE_INTEGER due;
            due.QuadPart = -static_cast<LONGLONG>(
                std::max<long long>(remaining.count() / 100, 1));
            if (::SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
                ::WaitForSingleObject(timer, INFINITE);
            } else {
                std::this_thread::sleep_until(next);
            }
        } else {
            std::this_thread::sleep_until(next);
        }
    }

    if (timer) {
        ::CloseHandle(timer);
    }
    if (mmcss) {
        ::AvRevertMmThreadCharacteristics(mmcss);
    }
}

void App::SaveProfile() {
    if (std::getenv("BRP_PROFILE_PROBE")) {
        std::fprintf(stderr, "[profile] save engine=%d rife_out=%d target=%d\n",
                     profile_.fg_engine, profile_.rife_output, profile_.fg_target);
    }
    std::string detail;
    session::Save(profile_, session::DefaultProfilePath(), detail);
}

void App::ConnectConsole() {
    if (profile_.host.empty()) {
        stream_note_ = "Set the console address first.";
        OpenAddressModal();
        return;
    }
    if (profile_.needs_pairing()) {
        screen_ = Screen::Settings;
        tab_ = Tab::Connection;
        stream_note_ = "Pair the console first (8-digit code from the PS5).";
        return;
    }

    pending_connect_ = PendingConnect::Probing;
    next_connect_step_ = clock_;
    stream_note_ = "Checking " + profile_.host + " ...";
    probe_.Start(profile_.host, 9295);
}

void App::StartStream() {
    if (!audio_.is_open()) {
        OpenAudioDevice();
    }
    streaming_.SetAudio(&audio_);
    pad_.SetConsoleFeedback(PadConsoleFeedback{});
    feedback_version_ = 0;

    if (profile_.neural_nr > 0) {
        if (!EnsureNr()) {
            const std::wstring note = nr_note_;
            profile_.neural_nr = 0;
            DisableNr();
            SaveProfile();
            nr_note_ = note;
            stream_note_ = "DLSSNR unavailable; using the regular upscaler.";
            std::fprintf(stderr, "[app] NR disabled: %ls\n", nr_note_.c_str());
        } else {
            frame_gen_ = 0;
        }
    }

    if (profile_.neural_sr > 0) {
        if (!EnsureDlssSrForTarget()) {
            const std::wstring note = dlss_note_;
            dlss_ready_ = false;
            dlss_sr_.Shutdown();
            SaveProfile();
            dlss_note_ = note;
            stream_note_ = "DLSS SR unavailable; using the regular upscaler.";
            std::fprintf(stderr, "[app] DLSS SR disabled: %ls\n", dlss_note_.c_str());
        }
        frame_gen_ = 0;
    }

    if (frame_gen_ >= 2 && !EnsureFrameGen()) {
        frame_gen_ = 0;
        stream_note_ = "Frame generation unavailable: NVIDIA VFX runtime (Settings > Display)";
        std::fprintf(stderr, "[app] frame generation disabled: %ls\n", fg_note_.c_str());
    }
    fg_queue_.clear();
    fg_last_ = {};
    fg_generated_ = 0;

    // The chiaki session setup does network I/O and can take hundreds of
    // milliseconds. Keep the presentation thread free: show a connecting
    // screen and let TickSession pick up the result.
    if (session_thread_.joinable()) {
        session_thread_.join();
    }
    stream_connecting_ = true;
    stream_active_ = false;
    session_started_.store(false);
    session_ok_.store(false);
    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        session_error_.clear();
    }
    screen_ = Screen::Stream;
    stream_note_ = "Connecting to " + profile_.host + " ...";

    // Build the persisted chain before any frames flow. Changing DLSS/worker
    // state while D3D12VA is decoding has been observed to upset the decoder,
    // so presets are only saved and land here.
    ApplyRenderTarget();
    if (profile_.neural_sr > 0 || profile_.dlss_fg >= 2) {
        ApplyNeuralSettings();
        ApplyDlssFrameGen();
    }

    const session::Profile profile_copy = profile_;
    const int width = profile_copy.width;
    const int height = profile_copy.height;
    const int bitrate = profile_copy.bitrate_kbps;
    session_thread_ = std::thread([this, profile_copy, width, height, bitrate] {
        std::string error;
        const bool ok = streaming_.Start(profile_copy, width, height, bitrate, error);
        {
            std::lock_guard<std::mutex> lock(session_mutex_);
            session_error_ = error;
        }
        session_ok_.store(ok);
        session_started_.store(true);
    });
}

void App::TickSession() {
    if (!stream_connecting_ || !session_started_.load()) {
        return;
    }
    if (session_thread_.joinable()) {
        session_thread_.join();
    }
    stream_connecting_ = false;

    const bool ok = session_ok_.load();
    std::string error;
    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        error = session_error_;
    }
    if (!ok) {
        stream_active_ = false;
        screen_ = Screen::Home;
        last_esc_at_ = -10.0;
        const bool reserved =
            error.find("already in use") != std::string::npos ||
            error.find("Connection Refused in Session Request") != std::string::npos;
        stream_note_ = reserved
                           ? "The console still has the previous session. Wait about "
                             "30 seconds, then press Connect again."
                           : (error.empty() ? "Could not start the session." : error);
        std::fprintf(stderr, "[app] start failed: %s\n", error.c_str());
        return;
    }

    stream_active_ = true;
    // Windowed presentation goes through DWM composition, which adds a refresh
    // of jitter: measured 62-84% of source frames at 4 refreshes windowed, and
    // 95-100% fullscreen. A stream therefore starts borderless fullscreen; F11
    // returns to the window.
    if (!borderless_) {
        ToggleFullscreen();
    }
    // RIFE engine: load it before the first frame, and delay the audio by the
    // one source interval the interpolation needs so sound stays with picture.
    if (profile_.fg_engine == 1) {
        if (!EnsureRife()) {
            profile_.fg_engine = 0;
            SaveProfile();
            stream_note_ = "RIFE unavailable; using NVIDIA frame generation.";
        } else {
            // RIFE holds the picture back by one source interval. A 40 ms
            // playback buffer delays the audio by the same amount; pushing
            // silence instead caused underruns.
            if (audio_buffer_ms_ < 40) {
                audio_buffer_ms_ = 40;
                if (audio_.is_open()) {
                    audio_.Close();
                    audio_failed_ = !audio_.Open(
                        audio_devices_.empty() ? std::string()
                                               : audio_devices_[audio_device_].id,
                        audio_buffer_ms_);
                }
            }
        }
    }
    video_holds_.fill(0);
    video_hold_pos_ = 0;
    video_hold_fill_ = 0;
    screen_holds_.fill(0);
    screen_hold_pos_ = 0;
    screen_hold_fill_ = 0;
    video_phase_ = 0.0;
    next_source_at_ = 0.0;
    source_updated_ = false;
    last_source_refresh_ = 0;
    last_source_at_ = -1.0;
    video_probe_at_ = clock_;
    last_present_misses_ = presenter_.present_misses();
    StartPadThread();
    mouse_active_at_ = clock_;
    cursor_hidden_ = false;
    if (window_) {
        window_->SetCursorHidden(false);
    }
    screen_ = Screen::Stream;
    stream_note_.clear();
    std::fprintf(stderr, "[app] streaming started\n");
}

void App::TickPendingConnect() {
    if (pending_connect_ == PendingConnect::None) {
        return;
    }

    probe_.Poll();
    const net::ProbeState state = probe_.state();

    if (pending_connect_ == PendingConnect::Probing) {
        if (state == net::ProbeState::Checking) {
            return;
        }
        if (state == net::ProbeState::Reachable) {
            pending_connect_ = PendingConnect::None;
            StartStream();
            return;
        }
        // Nothing listening on 9295: the console is almost always in rest mode.
        pending_connect_ = PendingConnect::Waking;
        wake_deadline_ = clock_ + 30.0;
        wake_packets_ = 0;
        next_connect_step_ = clock_;
        stream_note_ = "Console is not answering the control port. Sending wake-up...";
        return;
    }

    if (state == net::ProbeState::Reachable) {
        pending_connect_ = PendingConnect::None;
        stream_note_ = "Console woke up. Connecting...";
        StartStream();
        return;
    }

    if (clock_ >= wake_deadline_) {
        pending_connect_ = PendingConnect::None;
        stream_note_ =
            "Console did not wake up. Enable Settings > System > Power Saving > "
            "Features Available in Rest Mode > Stay Connected to the Internet.";
        return;
    }

    if (clock_ >= next_connect_step_) {
        const bool ps5 = profile_.device_type != "PS4";
        session::WakeConsole(profile_.host, profile_.credentials.rp_regist_key.data(),
                             profile_.credentials.rp_regist_key.size(), ps5);
        ++wake_packets_;
        stream_note_ = "Waking console... (attempt " + std::to_string(wake_packets_) + ")";
        next_connect_step_ = clock_ + 2.5;
        if (probe_.state() != net::ProbeState::Checking) {
            probe_.Start(profile_.host, 9295);
        }
    }
}
void App::ToggleFullscreen() {
    if (!window_) {
        return;
    }
    HWND hwnd = window_->hwnd();
    if (!borderless_) {
        ::GetWindowRect(hwnd, &windowed_rect_);
        windowed_style_ = ::GetWindowLongPtrW(hwnd, GWL_STYLE);
        windowed_ex_style_ = ::GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        const HMONITOR monitor = ::MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        if (!::GetMonitorInfoW(monitor, &mi)) {
            return;
        }
        ::SetWindowLongPtrW(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        ::SetWindowLongPtrW(hwnd, GWL_EXSTYLE, windowed_ex_style_ & ~WS_EX_TOPMOST);
        ::SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                       mi.rcMonitor.right - mi.rcMonitor.left,
                       mi.rcMonitor.bottom - mi.rcMonitor.top,
                       SWP_FRAMECHANGED | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
        borderless_ = true;
        window_->SetFullscreenChrome(true);
        RECT client{};
        ::GetClientRect(hwnd, &client);
        std::fprintf(stderr, "[app] fullscreen client %ldx%ld\n", client.right - client.left,
                     client.bottom - client.top);
        // Resize now, not on the next loop tick: the first frame after the
        // toggle already has to be drawn at the new size.
        ui_.ReleaseTarget();
        if (presenter_.Resize(static_cast<int>(client.right - client.left),
                              static_cast<int>(client.bottom - client.top))) {
            ui_.BindSwapchain(presenter_.backbuffer());
        }
    } else {
        ::SetWindowLongPtrW(hwnd, GWL_STYLE, windowed_style_);
        ::SetWindowLongPtrW(hwnd, GWL_EXSTYLE, windowed_ex_style_);
        ::SetWindowPos(hwnd, nullptr, windowed_rect_.left, windowed_rect_.top,
                       windowed_rect_.right - windowed_rect_.left,
                       windowed_rect_.bottom - windowed_rect_.top,
                       SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOOWNERZORDER);
        borderless_ = false;
        window_->SetFullscreenChrome(false);
        RECT client{};
        ::GetClientRect(hwnd, &client);
        ui_.ReleaseTarget();
        if (presenter_.Resize(static_cast<int>(client.right - client.left),
                              static_cast<int>(client.bottom - client.top))) {
            ui_.BindSwapchain(presenter_.backbuffer());
        }
    }
    // The swap chain follows the client size through WM_SIZE -> Resize(); the
    // FG child window is re-fitted below when its size is checked in Frame().
    ::SetForegroundWindow(hwnd);
}
void App::DisconnectStream() {
    if (session_thread_.joinable()) {
        session_thread_.join();
    }
    stream_connecting_ = false;
    // Mark the session dead before stopping it: the pad thread keeps running
    // (it owns HID polling for the whole app) and must not send into a session
    // that is being torn down.
    stream_active_ = false;
    streaming_.Stop();
    StopRife();
    pending_connect_ = PendingConnect::None;
    last_source_at_ = -1.0;
    last_source_refresh_ = 0;
    video_phase_ = 0.0;
    feedback_version_ = 0;
    pad_.SetConsoleFeedback(PadConsoleFeedback{});
    fg_queue_.clear();
    fg_last_ = {};
    cursor_hidden_ = false;
    if (window_) {
        window_->SetCursorHidden(false);
    }
    stream_note_ = "Disconnected. The console session is closed and freed; press Connect "
                   "to start again.";
    screen_ = Screen::Home;
}

void App::StartProbe() {
    probe_.Start(profile_.host, 9295);
}

void App::ApplyControllerOutput() {
    PadOutput out;

    // Index 8 is "Console (PS5)": leave the lightbar to the console.
    if (lightbar_index_ < 8) {
        const Rgb& color = kLightbarColors[lightbar_index_];
        out.lightbar = true;
        out.r = color.r;
        out.g = color.g;
        out.b = color.b;
    }

    // Index 6 is "Console (PS5)": leave the player LEDs to the console.
    if (player_leds_ < 6) {
        out.player_leds = true;
        out.player_mask = kPlayerLedMasks[player_leds_];
    }

    out.mic_led = true;
    out.mic_state = static_cast<uint8_t>(mic_led_);

    // Local test tone only; otherwise the console drives the motors.
    if (clock_ < rumble_until_) {
        out.rumble = true;
        out.weak = 200;
        out.strong = 240;
    }

    // A manual effect overrides the console; "Off" means pass-through.
    if (trigger_effect_ > 0) {
        out.left_trigger = true;
        out.right_trigger = true;
        out.left = BuildTriggerEffect(trigger_effect_, trigger_strength_);
        out.right = out.left;
    }

    pad_.SetOutput(out);
    last_output_ = clock_;
    output_dirty_ = false;
}

void App::Frame(double time_seconds, const PointerState& pointer) {
    const auto frame_started = std::chrono::steady_clock::now();

    if (presenter_.device_lost() && !device_lost_handled_) {
        device_lost_handled_ = true;
        RecoverFromDeviceLoss();
        return;
    }

    // Ask Win32 for the client size every frame: the cached value can lag a
    // borderless transition by a frame, and that frame would lay the whole UI
    // out at the old scale (the "half-scaled" fullscreen look).
    RECT client_rect{};
    ::GetClientRect(window_->hwnd(), &client_rect);
    const float physical_w = static_cast<float>(client_rect.right - client_rect.left);
    const float physical_h = static_cast<float>(client_rect.bottom - client_rect.top);
    if (physical_w <= 0.0f || physical_h <= 0.0f) {
        return;
    }
    ui_scale_ = std::clamp(physical_h / 900.0f, 1.0f, 2.5f);
    ui_w_ = physical_w / ui_scale_;
    ui_h_ = physical_h / ui_scale_;
    ui_.SetScale(ui_scale_);

    // Follow monitor changes without restarting (WM_DISPLAYCHANGE already
    // refreshes the window's cached value).
    if (window_->refresh_hz() != refresh_hz_) {
        refresh_hz_ = window_->refresh_hz();
        std::fprintf(stderr, "[app] display refresh %d Hz\n", refresh_hz_);
    }
    // Self-heal: the swap chain must always match the client area. If a
    // window resize message was missed (fullscreen transitions), the video
    // would otherwise be drawn at the wrong size - cropped or stretched.
    if (presenter_.output_width() != static_cast<int>(physical_w) ||
        presenter_.output_height() != static_cast<int>(physical_h)) {
        ui_.ReleaseTarget();
        if (presenter_.Resize(static_cast<int>(physical_w), static_cast<int>(physical_h))) {
            ui_.BindSwapchain(presenter_.backbuffer());
        }
    }

    pointer_ = pointer;
    if (ui_scale_ != 1.0f) {
        pointer_.x = static_cast<int>(std::lround(pointer.x / ui_scale_));
        pointer_.y = static_cast<int>(std::lround(pointer.y / ui_scale_));
    }
    clock_ = time_seconds;
    source_updated_ = false;
    hits_.clear();
    hover_ = Hit::None;
    hover_index_ = -1;

    // Colour/sharpening sliders write straight into the profile; apply them
    // (and persist a second after the last change) without touching the
    // capture or the colour itself.
    TickEnhance();
    if (clock_ - health_at_ >= 30.0) {
        health_at_ = clock_;
        LogHealth();
    }

    // Mouse activity drives the stream chrome and the OS cursor.
    if (pointer.x != last_mouse_x_ || pointer.y != last_mouse_y_ || pointer.clicked ||
        pointer.wheel != 0.0f) {
        mouse_active_at_ = clock_;
        last_mouse_x_ = pointer.x;
        last_mouse_y_ = pointer.y;
    }

    // Startup logo animation. Runs first and is skippable with a click or a key;
    // the window is shown only after its first frame.
    if (intro_) {
        if (intro_start_ < 0.0) {
            intro_start_ = clock_;
        }
        const double elapsed = clock_ - intro_start_;
        const bool skip = pointer_.clicked || elapsed > 3.6;
        if (!skip) {
            RenderIntro(elapsed);
            window_->SetCursorHidden(false);
            frame_work_ms_ = std::chrono::duration<double, std::milli>(
                                 std::chrono::steady_clock::now() - frame_started)
                                 .count();
            presenter_.PresentNow(true);
            if (!window_shown_) {
                window_->Show();
                window_shown_ = true;
            }
            return;
        }
        intro_ = false;
        pointer_.clicked = false;  // a skip click must not hit the Home UI
        mouse_active_at_ = clock_;
    }

    // Boot sequence: everything the app needs is prepared while this screen
    // animates, after the logo and before Home.
    if (booting_) {
        TickBoot(time_seconds);
        ui_.BeginFrame();
        RenderBoot(time_seconds);
        RenderWindowChrome();
        ui_.EndFrame();

        if (window_) {
            window_->set_hand_cursor(hover_ != Hit::None);
        }
        if (pointer_.clicked) {
            DispatchClick();
        }
        frame_work_ms_ = std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - frame_started)
                             .count();
        presenter_.PresentNow(true);
        if (!window_shown_) {
            window_->Show();
            window_shown_ = true;
        }
        return;
    }

    double dt = last_time_ < 0.0 ? 1.0 / 60.0 : time_seconds - last_time_;
    last_time_ = time_seconds;
    dt = std::clamp(dt, 0.0, 0.1);

    // Presentation interval history: the HUD shows the distribution, which is
    // how a stutter is told apart from a low frame rate.
    if (time_seconds > 0.0) {
        const float ms = static_cast<float>(dt * 1000.0);
        frame_ms_.push_back(ms);
        if (ms > 25.0f) {
            ++frame_stutters_;
        }
        while (frame_ms_.size() > 240) {
            frame_ms_.pop_front();
        }
    }

    if (!pad_run_.load()) {
        pad_.Poll();
    }
    probe_.Poll();
    TickPendingConnect();
    HandlePad();

    if (window_) {
        const int ch = window_->ConsumeChar();
        if (ch != 0) {
            if (address_modal_) {
                if (!address_busy_) {
                    if (ch == 8) {
                        if (!address_input_.empty()) {
                            address_input_.pop_back();
                            address_selected_ = -1;
                        }
                    } else if (ch >= 32 && ch < 127 && address_input_.size() < 255) {
                        address_input_.push_back(static_cast<char>(ch));
                        address_selected_ = -1;
                    }
                }
            } else if (psn_modal_) {
                if (!psn_busy_) {
                    if (ch == 8) {
                        if (!psn_url_input_.empty()) {
                            psn_url_input_.pop_back();
                            psn_note_ok_ = false;
                        }
                    } else if (ch >= 32 && ch < 127 && psn_url_input_.size() < 8192) {
                        psn_url_input_.push_back(static_cast<char>(ch));
                        psn_note_ok_ = false;
                    }
                }
            } else if (pin_focus_) {
                if (ch >= '0' && ch <= '9' && pin_.size() < 8) {
                    pin_ += static_cast<char>(ch);
                } else if (ch == 8 && !pin_.empty()) {
                    pin_.pop_back();
                }
            }
        }
    }

    TickPsnLogin();
    TickAddress();
    SampleFgWorker();
    TickSession();

    pairing_.Poll();
    if (pairing_.state() == session::PairState::Success) {
        session::Credentials credentials;
        if (pairing_.TakeCredentials(credentials)) {
            profile_.credentials = credentials;
            std::string detail;
            session::Save(profile_, session::DefaultProfilePath(), detail);
            pin_.clear();
            pin_focus_ = false;
        }
    }

    if (profile_.dlss_fg >= 2 && nr_.present_mode() && !fg_window_) {
        AttachFgWindow();
    }

    // The worker presents in its own child window, which sits above our
    // Direct2D surface. When it covers the client the parent pass is invisible,
    // so skip it (this is the expensive path at 240 Hz). Moving the pointer to
    // the top strip hides the child again so the chrome/HUD can be rendered and
    // used.
    bool fg_covering = false;
    if (fg_window_) {
        if (screen_ != Screen::Stream) {
            fg_reveal_until_ = 0.0;
        }
        const int want_w = static_cast<int>(ui_w_ * ui_scale_);
        const int want_h = static_cast<int>(ui_h_ * ui_scale_);
        if (want_w != fg_w_ || want_h != fg_h_) {
            ::SetWindowPos(fg_window_, HWND_TOP, 0, 0, want_w, want_h,
                           SWP_NOACTIVATE | SWP_NOOWNERZORDER);
            fg_w_ = want_w;
            fg_h_ = want_h;
        }

        POINT cursor{};
        ::GetCursorPos(&cursor);
        ::ScreenToClient(window_->hwnd(), &cursor);
        RECT client{};
        ::GetClientRect(window_->hwnd(), &client);

        const float chrome_trigger = 124.0f * ui_scale_;
        const bool inside = cursor.x >= 0 && cursor.y >= 0 && cursor.x < client.right &&
                            cursor.y < client.bottom;
        const bool in_chrome = inside && cursor.y < static_cast<LONG>(chrome_trigger);

        if (in_chrome) {
            // Keep the chrome reachable: leaving the header starts a 2 s grace
            // period, so the pointer can travel from the title bar to the
            // Disconnect button without the FG child covering it again.
            mouse_active_at_ = clock_;
            fg_reveal_until_ = clock_ + 2.0;
        }

        const bool reveal =
            screen_ == Screen::Stream && inside && (in_chrome || clock_ < fg_reveal_until_);
        fg_covering = screen_ == Screen::Stream && !reveal;
        // ShowWindow is a synchronous window-manager call; doing it every
        // 240 Hz tick made the worker window re-enter composition for no
        // reason. Only touch it on an actual visibility change.
        if (fg_visible_ != fg_covering) {
            ::ShowWindow(fg_window_, fg_covering ? SW_SHOW : SW_HIDE);
            fg_visible_ = fg_covering;
        }
    }

    const bool reflex_active = profile_.reflex > 0 && reflex_.markers_available() &&
                               stream_active_ && screen_ == Screen::Stream;
    const auto render_started = std::chrono::steady_clock::now();
    if (!fg_covering) {
        if (reflex_active) {
            // Markers keep the driver informed, but NvAPI_D3D_Sleep is not
            // called: it targets a game-style simulation window and, measured
            // here, sleeps into the next vblank (4 ms/frame, 56+ missed
            // refreshes/s). The swapchain clock is the pacer.
            reflex_.BeginFrame(presenter_.device());
            reflex_.EndSimulation(presenter_.device());
            reflex_.RenderSubmitStart(presenter_.device());
        }
        if (screen_ == Screen::Stream || screen_ == Screen::StreamSettings) {
            // NvAPI_D3D_Sleep is only called above, between SIMULATION_END and
            // RENDERSUBMIT_START. Without markers it is not called at all.
            presenter_.Render(time_seconds);
        } else if (screen_ != Screen::Home && screen_ != Screen::Settings) {
            const float bg[4] = {0.010f, 0.016f, 0.028f, 1.0f};
            presenter_.Clear(bg);
        }
        // Home and Settings paint an opaque full-window gradient themselves;
        // clearing the backbuffer first would be a second full-screen write.

        const auto ui_started = std::chrono::steady_clock::now();
        phase_render_ms_ = std::chrono::duration<double, std::milli>(
                               ui_started - render_started)
                               .count();
        ui_.BeginFrame();
        switch (screen_) {
            case Screen::Home:
                RenderHome(time_seconds);
                break;
            case Screen::Settings:
                RenderSettings(time_seconds);
                break;
            case Screen::Stream:
                RenderStream(time_seconds);
                break;
            case Screen::StreamSettings:
                RenderStreamSettings(time_seconds);
                break;
        }
        if (psn_modal_) {
            RenderPsnModal();
        } else if (address_modal_) {
            RenderAddressModal();
        }
        RenderWindowChrome();
        ui_.EndFrame();
        if (reflex_active) {
            reflex_.RenderSubmitEnd(presenter_.device());
        }
        phase_ui_ms_ = std::chrono::duration<double, std::milli>(
                           std::chrono::steady_clock::now() - ui_started)
                           .count();
    }
    const auto source_started = std::chrono::steady_clock::now();

    if (window_) {
        window_->set_hand_cursor(hover_ != Hit::None);
    }

    if (pointer_.clicked) {
        DispatchClick();
    }

    if (nr_settings_dirty_ && !pointer_.down) {
        nr_settings_dirty_ = false;
        if (profile_.neural_nr > 0) {
            ApplyNrSettings();
        } else {
            SaveProfile();
        }
    }

    if (stream_active_) {
        // Controller input is sent by the dedicated pad thread, independent of
        // the presentation loop.
        //
        // With VFG the UI runs at 120/240 Hz but the console only produces 60
        // source frames per second. Pulling a source frame on every tick would
        // drain the decoder and make the adaptive buffer grow for no reason, so
        // VFG requests a new source only when its generated queue is low.
        const bool video_paced = VideoPaced();
        bool video_due = true;
        if (video_paced) {
            if (fg_covering) {
                // The FG worker presents in its own window and the parent no
                // longer presents, so the swapchain tick is not a valid clock
                // here. Feed the worker on a plain 60 Hz wall-clock grid.
                const double interval =
                    1.0 / std::clamp(static_cast<double>(profile_.fps), 24.0, 240.0);
                if (next_source_at_ <= 0.0) {
                    next_source_at_ = clock_;
                }
                video_due = clock_ >= next_source_at_;
                if (video_due) {
                    next_source_at_ += interval;
                }
                if (next_source_at_ < clock_ - interval) {
                    next_source_at_ = clock_ + interval;
                }
            } else {
                next_source_at_ = 0.0;
                // The swapchain waitable gives this loop exactly one iteration
                // per refresh. Advance a rational phase by fps/refresh: at
                // 240 Hz and 60 fps that is a frame slot every fourth tick,
                // with no wall-clock drift and no 3-refresh snap when a tick
                // lands early.
                const double source_fps =
                    std::clamp(static_cast<double>(profile_.fps), 24.0, 240.0);
                video_phase_ += source_fps / static_cast<double>(present_hz());
                if (video_phase_ >= 1.0 - 1e-9) {
                    video_phase_ -= 1.0;
                } else {
                    video_due = false;
                }
                if (video_phase_ < 0.0) {
                    video_phase_ = 0.0;
                }
            }
        }
        const bool vfg_active = frame_gen_ >= 2 && fg_ready_;
        const bool rife_mode = rife_ && rife_->ready() && profile_.fg_engine == 1;
        if (rife_mode) {
            const bool nr_on = profile_.neural_nr > 0 && nr_ready_;
            if (video_due &&
                streaming_.TakeFrame(stream_frame_, stream_buffer_, video_paced)) {
                NoteSourceFrame();
                if (nr_on) {
                    // Detect duplicates on the raw frame: the NR worker's output
                    // can differ by a few counts and defeat the comparison.
                    if (!FrameIsDuplicate(stream_frame_)) {
                        SubmitNrFrame(stream_frame_);
                    }
                } else {
                    RifePush(stream_frame_);
                }
            }
            if (nr_on) {
                int nw = 0;
                int nh = 0;
                bool npq = false;
                if (TakeNrResult(nw, nh, npq)) {
                    media::DecodedFrame nrf;
                    nrf.rgba = std::move(nr_out_);
                    nrf.width = nw;
                    nrf.height = nh;
                    nrf.pq = npq;
                    RifePush(nrf, true);
                }
            }
        } else if (vfg_active) {
            if (fg_queue_.size() <= 2 &&
                streaming_.TakeFrame(stream_frame_, stream_buffer_)) {
                NoteSourceFrame();
                std::vector<media::NvVfx::Frame> sequence;
                std::string detail;
                if (nv_vfx_.PushFrame(stream_frame_.rgba.data(), stream_frame_.width,
                                      stream_frame_.height, frame_gen_, sequence, detail)) {
                    for (auto& frame : sequence) {
                        fg_queue_.push_back(std::move(frame));
                    }
                    const size_t cap = static_cast<size_t>(frame_gen_) * 3;
                    while (fg_queue_.size() > cap) {
                        fg_queue_.pop_front();
                    }
                    fg_generated_ += static_cast<int>(sequence.size()) - 1;
                } else {
                    fg_note_ = L"VFG: " + Widen(detail);
                    std::fprintf(stderr, "[app] frame generation failed: %s\n", detail.c_str());
                    frame_gen_ = 0;
                    fg_ready_ = false;
                    fg_queue_.clear();
                    fg_last_ = {};
                    presenter_.SetSourceFrame(stream_frame_.rgba.data(), stream_frame_.width,
                                              stream_frame_.height, stream_frame_.pq);
                }
            }
        } else if (video_due &&
                   streaming_.TakeFrame(stream_frame_, stream_buffer_, video_paced)) {
            NoteSourceFrame();
            if (profile_.dlss_fg >= 2 && nr_ready_ && nr_.present_mode()) {
                // DLSS Frame Generation: the worker presents generated frames
                // in its own window. When SR is also enabled it receives the
                // SR output (render-resolution -> FG, as in a game); otherwise
                // it receives the raw stream.
                const bool drop_duplicate = ShouldDropDuplicate(stream_frame_);
                bool fed = false;
                if (!drop_duplicate && profile_.neural_sr > 0 && dlss_ready_) {
                    media::NvDlssSr::Frame sr_frame;
                    if (RunDlssSr(stream_frame_, sr_frame)) {
                        const int sr_w = dlss_sr_.out_width();
                        const int sr_h = dlss_sr_.out_height();
                        presenter_.SetSourceView(sr_frame.srv.Get(), sr_w, sr_h,
                                                 stream_frame_.pq);
                        // Already at the render target: skip only the scaler.
                        presenter_.SetSourceFinal(true);
                        if (presenter_.EnhanceViewReadback(sr_frame.srv.Get(), sr_w, sr_h,
                                                           stream_frame_.pq, sr_readback_,
                                                           nullptr) &&
                            sr_readback_.size() == static_cast<size_t>(sr_w) * sr_h * 4) {
                            SubmitNrFrameData(std::move(sr_readback_), sr_w, sr_h,
                                              stream_frame_.pq);
                            fed = true;
                        } else {
                            dlss_note_ = L"FG enhance readback failed";
                            std::fprintf(stderr, "[app] SR->FG graded readback failed\n");
                        }
                    } else {
                        std::fprintf(stderr, "[app] SR->FG stopped: %ls\n",
                                     dlss_note_.c_str());
                        profile_.neural_sr = 0;
                        SaveProfile();
                        ApplyDlssFrameGen();
                        stream_note_ = "DLSS SR failed; frame generation uses the raw stream.";
                    }
                }
                if (!drop_duplicate && !fed) {
                    // Raw stream into the worker. Grade it there and then when a
                    // control is active; otherwise keep the zero-copy feed.
                    if (presenter_.enhance().active) {
                        if (presenter_.EnhanceFrameReadback(
                                stream_frame_.rgba.data(), stream_frame_.width,
                                stream_frame_.height, stream_frame_.pq, sr_readback_, nullptr) &&
                            sr_readback_.size() ==
                                static_cast<size_t>(stream_frame_.width) * stream_frame_.height * 4) {
                            SubmitNrFrameData(std::move(sr_readback_), stream_frame_.width,
                                              stream_frame_.height, stream_frame_.pq);
                            fed = true;
                        }
                    }
                    if (!fed) {
                        presenter_.SetSourceFrame(stream_frame_.rgba.data(), stream_frame_.width,
                                                  stream_frame_.height, stream_frame_.pq);
                        SubmitNrFrame(stream_frame_);
                    }
                }
                std::string fg_error;
                if (NrFailed(fg_error)) {
                    nr_note_ = L"DLSS FG: " + Widen(fg_error);
                    std::fprintf(stderr, "[app] DLSS FG worker failed: %s\n", fg_error.c_str());
                    profile_.dlss_fg = 0;
                    profile_.neural_nr = 0;
                    DisableNr();
                    DetachFgWindow();
                    SaveProfile();
                }
            } else {
            const bool dlaa = profile_.neural_sr > 0 && profile_.dlss_mode == 5;
            const bool sr_wanted = profile_.neural_sr > 0;
            const bool sr_stage = sr_wanted && !dlaa && DlssAvailable();
            const bool nr_on = profile_.neural_nr > 0 && nr_ready_;
            const bool nvidia_order = profile_.nr_order == 1 && sr_stage;
            const bool sr_on_raw = sr_wanted && (!nr_on || nvidia_order);

            // Stage 3: DLSS SR, with the mode's input scale. When the mode asks
            // for fewer pixels than the stream has, the frame is downscaled
            // first - that is what the NVIDIA percentage means.
            bool sr_presented = false;
            if (sr_on_raw && dlaa) {
                const int dw = stream_frame_.width;
                const int dh = stream_frame_.height;
                if (!dlss_ready_ || !dlss_dlaa_ || dlss_in_w_ != dw || dlss_in_h_ != dh) {
                    dlss_ready_ = EnsureDlssDlaa(dw, dh);
                }
                std::string detail;
                if (dlss_ready_ &&
                    dlss_sr_.Process(stream_frame_.rgba.data(), dw, dh, dlss_frame_, detail)) {
                    sr_presented = true;
                    presenter_.SetSourceView(dlss_frame_.srv.Get(), dw, dh, stream_frame_.pq);
                    // DLAA runs at the frame size: the upscaler still takes it
                    // to the target.
                    presenter_.SetSourceFinal(false);
                } else if (dlss_ready_) {
                    dlss_note_ = L"DLAA: " + Widen(detail);
                    std::fprintf(stderr, "[app] DLAA failed: %s\n", detail.c_str());
                    dlss_ready_ = false;
                    dlss_sr_.Shutdown();
                }
            } else if (sr_on_raw) {
                int in_w = 0;
                int in_h = 0;
                DlssInputSize(stream_frame_.width, stream_frame_.height, &in_w, &in_h);
                const uint8_t* sr_input = stream_frame_.rgba.data();
                if (in_w != stream_frame_.width || in_h != stream_frame_.height) {
                    sr_scaled_.resize(static_cast<size_t>(in_w) * in_h * 4);
                    media::DownscaleRgba(stream_frame_.rgba.data(), stream_frame_.width,
                                         stream_frame_.height, sr_scaled_.data(), in_w, in_h,
                                         scale_tmp_);
                    sr_input = sr_scaled_.data();
                }
                if (!dlss_ready_ || dlss_in_w_ != in_w || dlss_in_h_ != in_h) {
                    dlss_ready_ = EnsureDlssSr(in_w, in_h);
                }
                std::string detail;
                if (dlss_ready_ &&
                    dlss_sr_.Process(sr_input, in_w, in_h, dlss_frame_, detail)) {
                    sr_presented = true;
                    presenter_.SetSourceView(dlss_frame_.srv.Get(), dlss_sr_.out_width(),
                                             dlss_sr_.out_height(), stream_frame_.pq);
                    presenter_.SetSourceFinal(true);
                } else if (dlss_ready_) {
                    dlss_note_ = L"DLSS SR: " + Widen(detail);
                    std::fprintf(stderr, "[app] DLSS SR failed: %s\n", detail.c_str());
                    dlss_ready_ = false;
                    dlss_sr_.Shutdown();
                    SaveProfile();
                }
            }

            if (nr_on && nvidia_order && sr_presented) {
                // Stage 5: NR on the upscaled frame. The worker speaks RGBA, so
                // the GPU result comes back to the CPU first.
                const int out_w = dlss_sr_.out_width();
                const int out_h = dlss_sr_.out_height();
                std::string rb_detail;
                if (dlss_sr_.Readback(sr_readback_, rb_detail) &&
                    sr_readback_.size() == static_cast<size_t>(out_w) * out_h * 4) {
                    SubmitNrFrameData(std::move(sr_readback_), out_w, out_h,
                                      stream_frame_.pq);
                } else if (!rb_detail.empty()) {
                    nr_note_ = L"NR readback: " + Widen(rb_detail);
                    std::fprintf(stderr, "[app] readback failed: %s\n", rb_detail.c_str());
                }
            }

            if (nr_on && !nvidia_order) {
                // Upstream order: NR first, then DLSS SR consumes its result.
                SubmitNrFrame(stream_frame_);
            }

            std::string nr_error;
            if (nr_on && NrFailed(nr_error)) {
                nr_note_ = L"NR: " + Widen(nr_error);
                std::fprintf(stderr, "[app] NR failed: %s\n", nr_error.c_str());
                profile_.neural_nr = 0;
                DisableNr();
                SaveProfile();
            } else {
                int nr_w = 0;
                int nr_h = 0;
                bool nr_pq = false;
                if (TakeNrResult(nr_w, nr_h, nr_pq)) {
                    if (nvidia_order) {
                        // NR already ran at the target resolution.
                        presenter_.SetSourceFrame(nr_out_.data(), nr_w, nr_h, nr_pq);
                        presenter_.SetSourceFinal(true);
                    } else {
                        const bool want_sr = profile_.neural_sr > 0;
                        const bool nr_dlaa = want_sr && profile_.dlss_mode == 5;
                        int in_w = nr_w;
                        int in_h = nr_h;
                        const uint8_t* sr_input = nr_out_.data();
                        if (want_sr && !nr_dlaa) {
                            DlssInputSize(nr_w, nr_h, &in_w, &in_h);
                            if (in_w != nr_w || in_h != nr_h) {
                                sr_scaled_.resize(static_cast<size_t>(in_w) * in_h * 4);
                                media::DownscaleRgba(nr_out_.data(), nr_w, nr_h,
                                                     sr_scaled_.data(), in_w, in_h,
                                                     scale_tmp_);
                                sr_input = sr_scaled_.data();
                            }
                        }
                        if (want_sr && nr_dlaa) {
                            if (!dlss_ready_ || !dlss_dlaa_ || dlss_in_w_ != nr_w ||
                                dlss_in_h_ != nr_h) {
                                dlss_ready_ = EnsureDlssDlaa(nr_w, nr_h);
                            }
                        } else if (want_sr &&
                                   (!dlss_ready_ || dlss_dlaa_ || dlss_in_w_ != in_w ||
                                    dlss_in_h_ != in_h)) {
                            dlss_ready_ = EnsureDlssSr(in_w, in_h);
                        }
                        std::string sr_detail;
                        const bool sr_ok =
                            want_sr && dlss_ready_ &&
                            dlss_sr_.Process(sr_input, in_w, in_h, dlss_frame_, sr_detail);
                        if (sr_ok) {
                            presenter_.SetSourceView(dlss_frame_.srv.Get(), dlss_sr_.out_width(),
                                                     dlss_sr_.out_height(), nr_pq);
                            presenter_.SetSourceFinal(!nr_dlaa);
                        } else {
                            if (want_sr && dlss_ready_) {
                                dlss_note_ = (nr_dlaa ? L"DLAA: " : L"DLSS SR: ") +
                                             Widen(sr_detail);
                                dlss_ready_ = false;
                                dlss_sr_.Shutdown();
                            }
                            presenter_.SetSourceFinal(false);
                            presenter_.SetSourceFrame(nr_out_.data(), nr_w, nr_h, nr_pq);
                        }
                    }
                }
            }

            if (!nr_on && !sr_presented) {
                presenter_.SetSourceFinal(false);
                presenter_.SetSourceFrame(stream_frame_.rgba.data(), stream_frame_.width,
                                          stream_frame_.height, stream_frame_.pq);
            }
            }
        }
        if (frame_gen_ >= 2 && fg_ready_) {
            if (!fg_queue_.empty()) {
                fg_last_ = std::move(fg_queue_.front());
                fg_queue_.pop_front();
            }
            if (fg_last_.srv) {
                presenter_.SetSourceFinal(false);
                presenter_.SetSourceView(fg_last_.srv.Get(), stream_frame_.width,
                                         stream_frame_.height, stream_frame_.pq);
            }
        }

        if (rife_mode) {
            RifePresentDue();
        }

        if (streaming_.state() == session::StreamState::Failed) {
            const std::string reason = streaming_.message();
            stream_active_ = false;
            streaming_.Stop();
            pad_.SetConsoleFeedback(PadConsoleFeedback{});
            cursor_hidden_ = false;
            window_->SetCursorHidden(false);
            stream_note_ = reason.empty() ? "Session ended." : reason;
            screen_ = Screen::Home;
        }
    }
    phase_source_ms_ = std::chrono::duration<double, std::milli>(
                           std::chrono::steady_clock::now() - source_started)
                           .count();

    static const bool fg_probe = std::getenv("BRP_FG_PROBE") != nullptr;
    static double fg_probe_at = 0.0;
    static int fg_probe_frames = 0;
    if (fg_probe && frame_gen_ >= 2) {
        ++fg_probe_frames;
        if (clock_ - fg_probe_at >= 1.0) {
            const double elapsed = clock_ - fg_probe_at;
            std::fprintf(stderr,
                         "[fg] %dx target %d Hz, actual %.0f fps, generated %d, last %.2f ms\n",
                         frame_gen_, present_hz(),
                         elapsed > 0.0 ? fg_probe_frames / elapsed : 0.0, fg_generated_,
                         nv_vfx_.last_generate_ms());
            fg_probe_at = clock_;
            fg_probe_frames = 0;
        }
    }

    static const bool decode_probe = std::getenv("BRP_DECODE_PROBE") != nullptr;
    static double decode_probe_at = 0.0;
    static session::Streaming::StreamStats last_stats{};
    if (stream_active_ && clock_ - decode_probe_at >= 1.0) {
        // Consume every second so decode_ms_max is a true per-second peak.
        last_stats = streaming_.ConsumeStats();
        stream_stats_ = last_stats;
        decode_probe_at = clock_;
        if (decode_probe) {
            const double interval_fps =
                last_stats.elapsed > 0.0 ? last_stats.frames / last_stats.elapsed : 0.0;
            std::fprintf(
                stderr,
                "[app] fps %.1f dec=%s audio=%d vfg=%d | %0.f kbps lost %d | "
                "decode %.2f/%.2f ms | queue %d buf %d drops %d\n",
                interval_fps, streaming_.decoder_name().c_str(), streaming_.audio_frames(),
                frame_gen_, last_stats.kbps, last_stats.lost,
                last_stats.decode_ms_avg, last_stats.decode_ms_max, last_stats.queue,
                last_stats.buffer, last_stats.buffer_drops);
        }
    }

    const bool rumble_testing = clock_ < rumble_until_;
    if (pad_.is_sony() &&
        (output_dirty_ || rumble_testing != rumble_was_testing_ ||
         clock_ - last_output_ > 0.2)) {
        ApplyControllerOutput();
    }
    rumble_was_testing_ = rumble_testing;
    if (!pad_run_.load()) {
        last_send_ok_ = pad_.PumpOutput();
    }

    if (capture_requested_) {
        capture_requested_ = false;
        const char* path = "app-home.bmp";
        if (screen_ == Screen::Settings) {
            path = "app-settings.bmp";
        } else if (screen_ == Screen::Stream || screen_ == Screen::StreamSettings) {
            path = "app-stream.bmp";
        }
        if (presenter_.CaptureToBmp(path)) {
            std::printf("captured %s\n", path);
        } else {
            std::fprintf(stderr, "capture failed: %s\n", path);
        }
    }

    // Hide the OS cursor together with the stream chrome.
    const bool want_cursor_hidden =
        screen_ == Screen::Stream && stream_active_ && clock_ - mouse_active_at_ > 3.0;
    if (want_cursor_hidden != cursor_hidden_) {
        cursor_hidden_ = want_cursor_hidden;
        window_->SetCursorHidden(want_cursor_hidden);
        if (want_cursor_hidden) {
            hover_ = Hit::None;
        }
    }

    frame_work_ms_ = std::chrono::duration<double, std::milli>(
                         std::chrono::steady_clock::now() - frame_started)
                         .count();

    // When the DLSS FG worker covers the client there is nothing of ours to
    // show. Presenting a stale parent frame every vblank would still enter DWM
    // composition and compete with the worker's own swapchain for scanout.
    const bool parent_covered = stream_active_ && fg_covering;
    present_covered_ = parent_covered;
    if (!parent_covered) {
        if (reflex_active) {
            reflex_.PresentStart(presenter_.device());
        }
        presenter_.PresentNow(true);
        if (reflex_active) {
            reflex_.PresentEnd(presenter_.device());
        }

        // Ground truth: the refresh counter difference between two new-source
        // presents is exactly how many vblanks the previous source frame was on
        // screen. This catches judder from the GPU/compositor even when the CPU
        // loop is perfectly paced.
        if (stream_active_ && presenter_.present_stats_available()) {
            const UINT refresh = presenter_.present_refresh_count();
            if (refresh != 0) {
                if (source_updated_ && last_source_refresh_ != 0 &&
                    refresh > last_source_refresh_) {
                    const UINT delta = refresh - last_source_refresh_;
                    const int vblanks = std::clamp(static_cast<int>(delta), 1, 11);
                    screen_holds_[screen_hold_pos_] = static_cast<uint8_t>(vblanks);
                    screen_hold_pos_ =
                        (screen_hold_pos_ + 1) % static_cast<int>(screen_holds_.size());
                    if (screen_hold_fill_ < static_cast<int>(screen_holds_.size())) {
                        ++screen_hold_fill_;
                    }
                }
                if (source_updated_) {
                    last_source_refresh_ = refresh;
                }
            }
        }
    }

    LogVideoCadence();
    if (!window_shown_) {
        window_->Show();
        window_shown_ = true;
    }
}

void App::StartPsnLogin() {
    if (psn_busy_) {
        return;
    }
    psn_modal_ = true;
    address_modal_ = false;
    psn_url_focus_ = true;
    psn_url_input_.clear();
    psn_note_.clear();
    psn_note_ok_ = false;
    psn_result_ok_ = false;
    pin_focus_ = false;
    open_dropdown_ = -1;

    std::string detail;
    if (!session::OpenPsnLoginPage(detail)) {
        psn_note_ = Widen(detail);
        psn_note_ok_ = false;
    } else {
        psn_note_ = L"Sign in in the browser, then paste the final redirect URL.";
    }
}

void App::ClosePsnModal() {
    psn_modal_ = false;
    psn_url_focus_ = false;
}

bool App::ReadClipboardText(std::string& text) {
    text.clear();
    if (!window_ || !::OpenClipboard(window_->hwnd())) {
        return false;
    }

    if (HANDLE data = ::GetClipboardData(CF_UNICODETEXT)) {
        if (auto* wide = static_cast<const wchar_t*>(::GlobalLock(data))) {
            const int size = ::WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr,
                                                  nullptr);
            if (size > 1) {
                text.resize(static_cast<size_t>(size - 1));
                ::WideCharToMultiByte(CP_UTF8, 0, wide, -1, text.data(), size, nullptr,
                                      nullptr);
            }
            ::GlobalUnlock(data);
        }
    }
    ::CloseClipboard();

    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        text.clear();
        return false;
    }
    const auto last = text.find_last_not_of(" \t\r\n");
    text = text.substr(first, last - first + 1);
    return true;
}

void App::PastePsnClipboard() {
    std::string text;
    if (!ReadClipboardText(text)) {
        psn_note_ = L"Clipboard is empty.";
        psn_note_ok_ = false;
        return;
    }
    psn_url_input_ = std::move(text);
    psn_url_focus_ = true;
    psn_note_ = L"Link pasted. Press Save to fetch the account ID.";
    psn_note_ok_ = false;
}

void App::PasteAddressClipboard() {
    if (address_busy_) {
        return;
    }
    std::string text;
    if (!ReadClipboardText(text)) {
        address_note_ = L"Clipboard is empty.";
        return;
    }
    address_input_ = std::move(text);
    address_selected_ = -1;
    address_focus_ = true;
    address_note_ = L"Address pasted. Press Save address.";
}

void App::SubmitPsnLogin() {
    if (psn_busy_) {
        return;
    }
    if (psn_url_input_.empty()) {
        psn_note_ = L"Paste the redirect URL first.";
        psn_note_ok_ = false;
        return;
    }
    if (psn_thread_.joinable()) {
        psn_thread_.join();
    }

    psn_busy_ = true;
    psn_done_.store(false);
    psn_result_ok_ = false;
    psn_note_ = L"Contacting PlayStation...";
    psn_note_ok_ = false;

    const std::string input = psn_url_input_;
    psn_thread_ = std::thread([this, input] {
        session::PsnAccount account;
        std::string detail;
        const bool ok = session::PsnCompleteLogin(input, account, detail);
        {
            std::lock_guard<std::mutex> lock(psn_mutex_);
            psn_result_ = account;
            psn_detail_ = detail;
            psn_result_ok_ = ok;
        }
        psn_done_.store(true);
    });
}

void App::TickPsnLogin() {
    if (!psn_busy_ || !psn_done_.load()) {
        return;
    }
    if (psn_thread_.joinable()) {
        psn_thread_.join();
    }
    psn_busy_ = false;
    psn_done_.store(false);

    session::PsnAccount account;
    std::string detail;
    bool ok = false;
    {
        std::lock_guard<std::mutex> lock(psn_mutex_);
        account = psn_result_;
        detail = psn_detail_;
        ok = psn_result_ok_;
    }

    if (ok) {
        profile_.psn_account_id = account.account_id;
        profile_.psn_numeric_id = account.numeric_id;
        SaveProfile();
        psn_note_ = L"PSN account linked: " + Widen(account.numeric_id) +
                    L". The ID is saved permanently.";
        psn_note_ok_ = true;
        session_note_ = "PSN account linked: " + account.numeric_id;
    } else {
        psn_note_ = Widen(detail);
        psn_note_ok_ = false;
    }
}

void App::RenderPsnModal() {
    const float w = ui_w_;
    const float h = ui_h_;
    ui_.FillRect({0.0f, 0.0f, w, h}, th::Rgba(0x000000, 0.62f));

    const float panel_w = 760.0f;
    const float panel_h = 470.0f;
    const D2D1_RECT_F panel{(w - panel_w) * 0.5f, (h - panel_h) * 0.5f,
                            (w + panel_w) * 0.5f, (h + panel_h) * 0.5f};
    ui_.FillRounded(panel, 18.0f, th::Rgba(0x120A20, 0.98f));
    ui_.StrokeRounded(panel, 18.0f, th::Rgba(0xFFFFFF, 0.12f), 1.0f);
    AddHit(Hit::PsnPanel, -1, panel);

    ui_.Text(L"PSN account", {panel.left + 36.0f, panel.top + 24.0f, panel.right - 36.0f,
                              panel.top + 60.0f},
             24.0f, th::Rgba(th::kInk), DWRITE_FONT_WEIGHT_SEMI_BOLD);
    ui_.Text(L"Sign in with your PlayStation account. After the blank redirect page opens, "
             L"copy its URL and paste it below.",
             {panel.left + 36.0f, panel.top + 68.0f, panel.right - 36.0f, panel.top + 116.0f},
             12.5f, th::Rgba(th::kMuted));

    DrawButton(L"Open browser", {panel.left + 36.0f, panel.top + 128.0f,
                                 panel.left + 196.0f, panel.top + 172.0f},
               W_PsnOpen, false);
    DrawButton(L"Paste link", {panel.left + 212.0f, panel.top + 128.0f,
                               panel.left + 356.0f, panel.top + 172.0f},
               W_PsnPaste, false);

    ui_.Text(L"Redirect URL", {panel.left + 36.0f, panel.top + 184.0f,
                               panel.right - 36.0f, panel.top + 204.0f},
             10.5f, th::Rgba(th::kMuted, 0.9f), DWRITE_FONT_WEIGHT_SEMI_BOLD);

    const D2D1_RECT_F field{panel.left + 36.0f, panel.top + 208.0f, panel.right - 36.0f,
                            panel.top + 256.0f};
    ui_.FillRounded(field, 10.0f,
                    th::Rgba(0xFFFFFF, psn_url_focus_ ? 0.10f : 0.06f));
    ui_.StrokeRounded(field, 10.0f,
                      th::Rgba(psn_url_focus_ ? th::kAccent : 0xFFFFFF,
                               psn_url_focus_ ? 0.55f : 0.10f),
                      1.0f);
    AddHit(Hit::PsnUrlField, -1, field);

    std::wstring shown = Widen(psn_url_input_);
    if (shown.size() > 86) {
        shown = L"..." + shown.substr(shown.size() - 86);
    }
    if (shown.empty()) {
        ui_.Text(L"Paste the URL from the blank redirect page here",
                 {field.left + 14.0f, field.top, field.right - 14.0f, field.bottom}, 12.0f,
                 th::Rgba(th::kMuted, 0.75f));
    } else {
        ui_.Text(shown.c_str(), {field.left + 14.0f, field.top, field.right - 14.0f,
                                 field.bottom},
                 12.0f, th::Rgba(th::kInk, 0.95f));
    }
    if (psn_url_focus_ && !psn_busy_ && std::fmod(clock_, 1.0) < 0.55) {
        const float caret_x =
            std::min(field.right - 12.0f,
                     field.left + 14.0f + ui_.TextWidth(shown.c_str(), 12.0f));
        ui_.Line({caret_x, field.top + 9.0f}, {caret_x, field.bottom - 9.0f},
                 th::Rgba(th::kAccent, 0.9f), 1.2f);
    }

    if (psn_busy_) {
        ui_.StrokeArc(panel.right - 56.0f, panel.top + 232.0f, 10.0f,
                      static_cast<float>(clock_ * 5.0), 4.6f,
                      th::Rgba(th::kAccent, 0.9f), 2.0f);
    }

    ui_.Text(psn_note_.c_str(), {panel.left + 36.0f, panel.top + 270.0f,
                                 panel.right - 36.0f, panel.top + 318.0f},
             12.0f, th::Rgba(psn_note_ok_ ? th::kOnline : th::kMuted, 0.95f));

    ui_.Text(L"Only the PSN account ID is stored locally, in session.json. The browser "
             L"token is never saved.",
             {panel.left + 36.0f, panel.bottom - 104.0f, panel.right - 36.0f,
              panel.bottom - 82.0f},
             10.5f, th::Rgba(th::kMuted, 0.7f));

    const float buttons_y = panel.bottom - 72.0f;
    DrawButton(psn_busy_ ? L"Checking..."
                         : (psn_note_ok_ ? L"Done" : L"Save account ID"),
               {panel.left + 36.0f, buttons_y, panel.left + 260.0f, buttons_y + 44.0f},
               psn_note_ok_ ? W_PsnCancel : W_PsnSave, true);
    DrawButton(L"Cancel",
               {panel.left + 276.0f, buttons_y, panel.left + 420.0f, buttons_y + 44.0f},
               W_PsnCancel, false);
}

void App::OpenAddressModal() {
    address_modal_ = true;
    psn_modal_ = false;
    address_focus_ = true;
    address_selected_ = -1;
    address_input_ = profile_.host;
    address_note_ = profile_.host.empty()
                        ? L"Enter the console IP address or press Discover."
                        : L"";
    pin_focus_ = false;
    open_dropdown_ = -1;
}

void App::CloseAddressModal() {
    address_modal_ = false;
    address_focus_ = false;
}

void App::DiscoverConsoles() {
    if (address_busy_) {
        return;
    }
    if (address_thread_.joinable()) {
        address_thread_.join();
    }
    address_busy_ = true;
    address_done_.store(false);
    address_selected_ = -1;
    {
        std::lock_guard<std::mutex> lock(address_mutex_);
        address_results_.clear();
    }
    address_note_ = L"Scanning the LAN for PS5/PS4 consoles...";

    address_thread_ = std::thread([this] {
        std::vector<session::DiscoveredConsole> found = session::DiscoverLocal(2500);
        {
            std::lock_guard<std::mutex> lock(address_mutex_);
            address_results_ = std::move(found);
        }
        address_done_.store(true);
    });
}

void App::TickAddress() {
    if (!address_busy_ || !address_done_.load()) {
        return;
    }
    if (address_thread_.joinable()) {
        address_thread_.join();
    }
    address_busy_ = false;
    address_done_.store(false);

    std::vector<session::DiscoveredConsole> found;
    {
        std::lock_guard<std::mutex> lock(address_mutex_);
        found = address_results_;
    }

    if (found.empty()) {
        address_note_ = L"No console answered. Enter its IP manually "
                        L"(PS5: Settings > Network).";
    } else if (found.size() == 1) {
        address_input_ = found.front().address;
        address_selected_ = 0;
        address_note_ = L"Found " + Widen(found.front().name.empty() ? found.front().address
                                                                     : found.front().name) +
                        L" at " + Widen(found.front().address) + L". Press Save address.";
    } else {
        address_note_ = L"Found " + std::to_wstring(found.size()) +
                        L" consoles. Pick one below, then Save address.";
    }
}

void App::SaveConsoleAddress() {
    if (address_busy_) {
        return;
    }
    std::string host = address_input_;
    const auto first = host.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        address_note_ = L"Enter the console IP address first.";
        return;
    }
    const auto last = host.find_last_not_of(" \t\r\n");
    host = host.substr(first, last - first + 1);

    for (const char* prefix : {"http://", "https://"}) {
        const size_t length = std::strlen(prefix);
        if (host.rfind(prefix, 0) == 0) {
            host.erase(0, length);
        }
    }
    const size_t slash = host.find('/');
    if (slash != std::string::npos) {
        host.erase(slash);
    }
    if (host.empty()) {
        address_note_ = L"Enter the console IP address first.";
        return;
    }

    if (address_selected_ >= 0) {
        std::lock_guard<std::mutex> lock(address_mutex_);
        if (address_selected_ < static_cast<int>(address_results_.size())) {
            const session::DiscoveredConsole& found = address_results_[address_selected_];
            if (!found.name.empty()) {
                profile_.name = found.name;
            }
            if (!found.target_id.empty()) {
                profile_.target_id = found.target_id;
            }
            if (!found.device_type.empty()) {
                profile_.device_type = found.device_type;
            }
        }
    }

    profile_.host = host;
    SaveProfile();
    probe_.Start(host, 9295);
    stream_note_ = "Console address saved: " + host + ". Checking the control port...";
    CloseAddressModal();
}

void App::RenderAddressModal() {
    const float w = ui_w_;
    const float h = ui_h_;
    ui_.FillRect({0.0f, 0.0f, w, h}, th::Rgba(0x000000, 0.62f));

    const float panel_w = 720.0f;
    const float panel_h = 580.0f;
    const D2D1_RECT_F panel{(w - panel_w) * 0.5f, (h - panel_h) * 0.5f,
                            (w + panel_w) * 0.5f, (h + panel_h) * 0.5f};
    ui_.FillRounded(panel, 18.0f, th::Rgba(0x120A20, 0.98f));
    ui_.StrokeRounded(panel, 18.0f, th::Rgba(0xFFFFFF, 0.12f), 1.0f);
    AddHit(Hit::AddressPanel, -1, panel);

    ui_.Text(L"Console connection", {panel.left + 36.0f, panel.top + 24.0f,
                                     panel.right - 36.0f, panel.top + 58.0f},
             24.0f, th::Rgba(th::kInk), DWRITE_FONT_WEIGHT_SEMI_BOLD);
    ui_.Text(L"PS5: Settings > Network > View Connection Status shows the console IP.",
             {panel.left + 36.0f, panel.top + 64.0f, panel.right - 36.0f, panel.top + 86.0f},
             12.5f, th::Rgba(th::kMuted));
    ui_.Text(L"The PC and console must be on the same network. VPNs or guest Wi-Fi can "
             L"hide discovery.",
             {panel.left + 36.0f, panel.top + 88.0f, panel.right - 36.0f, panel.top + 110.0f},
             12.5f, th::Rgba(th::kMuted));

    ui_.Text(L"Console address (IP or hostname)",
             {panel.left + 36.0f, panel.top + 122.0f, panel.right - 36.0f,
              panel.top + 142.0f},
             10.5f, th::Rgba(th::kMuted, 0.9f), DWRITE_FONT_WEIGHT_SEMI_BOLD);

    const D2D1_RECT_F field{panel.left + 36.0f, panel.top + 146.0f, panel.right - 36.0f,
                            panel.top + 192.0f};
    ui_.FillRounded(field, 10.0f,
                    th::Rgba(0xFFFFFF, address_focus_ ? 0.10f : 0.06f));
    ui_.StrokeRounded(field, 10.0f,
                      th::Rgba(address_focus_ ? th::kAccent : 0xFFFFFF,
                               address_focus_ ? 0.55f : 0.10f),
                      1.0f);
    AddHit(Hit::AddressField, -1, field);

    if (address_input_.empty()) {
        ui_.Text(L"192.168.1.21 or console hostname",
                 {field.left + 14.0f, field.top, field.right - 14.0f, field.bottom}, 12.0f,
                 th::Rgba(th::kMuted, 0.75f));
    } else {
        ui_.Text(Widen(address_input_).c_str(),
                 {field.left + 14.0f, field.top, field.right - 14.0f, field.bottom}, 12.0f,
                 th::Rgba(th::kInk, 0.95f));
    }
    if (address_focus_ && !address_busy_ && std::fmod(clock_, 1.0) < 0.55) {
        const float caret_x =
            std::min(field.right - 12.0f,
                     field.left + 14.0f + ui_.TextWidth(Widen(address_input_).c_str(), 12.0f));
        ui_.Line({caret_x, field.top + 9.0f}, {caret_x, field.bottom - 9.0f},
                 th::Rgba(th::kAccent, 0.9f), 1.2f);
    }

    DrawButton(L"Discover", {panel.left + 36.0f, panel.top + 202.0f,
                             panel.left + 176.0f, panel.top + 246.0f},
               W_AddressDiscover, false);
    DrawButton(L"Paste IP", {panel.left + 192.0f, panel.top + 202.0f,
                             panel.left + 322.0f, panel.top + 246.0f},
               W_AddressPaste, false);

    std::vector<session::DiscoveredConsole> results;
    {
        std::lock_guard<std::mutex> lock(address_mutex_);
        results = address_results_;
    }

    float list_y = panel.top + 260.0f;
    if (address_busy_) {
        ui_.StrokeArc(panel.right - 56.0f, panel.top + 170.0f, 10.0f,
                      static_cast<float>(clock_ * 5.0), 4.6f,
                      th::Rgba(th::kAccent, 0.9f), 2.0f);
        ui_.Text(L"Scanning UDP 9302/987...",
                 {panel.left + 36.0f, list_y, panel.right - 36.0f, list_y + 32.0f}, 12.0f,
                 th::Rgba(th::kMuted));
    } else if (results.empty()) {
        ui_.Text(L"No discovered console. Enter the IP manually or press Discover.",
                 {panel.left + 36.0f, list_y, panel.right - 36.0f, list_y + 32.0f}, 12.0f,
                 th::Rgba(th::kMuted, 0.8f));
    } else {
        ui_.Text(L"Discovered consoles",
                 {panel.left + 36.0f, list_y, panel.right - 36.0f, list_y + 22.0f}, 10.5f,
                 th::Rgba(th::kMuted, 0.9f), DWRITE_FONT_WEIGHT_SEMI_BOLD);
        list_y += 26.0f;
        for (size_t i = 0; i < results.size() && i < 4; ++i) {
            const D2D1_RECT_F row{panel.left + 36.0f, list_y, panel.right - 36.0f,
                                  list_y + 36.0f};
            const bool selected = static_cast<int>(i) == address_selected_;
            if (selected) {
                ui_.FillRounded(row, 8.0f, th::Rgba(th::kAccent, 0.18f));
            }
            const std::wstring label =
                Widen(results[i].name.empty() ? results[i].address : results[i].name) +
                L"   " + Widen(results[i].address);
            ui_.Text(label.c_str(), {row.left + 12.0f, row.top, row.right - 12.0f,
                                     row.bottom},
                     12.0f, th::Rgba(th::kInk, selected ? 0.98f : 0.82f));
            AddHit(Hit::AddressResult, static_cast<int>(i), row);
            list_y += 40.0f;
        }
    }

    ui_.Text(address_note_.c_str(), {panel.left + 36.0f, panel.bottom - 132.0f,
                                     panel.right - 36.0f, panel.bottom - 82.0f},
             12.0f, th::Rgba(th::kMuted, 0.95f));

    const float buttons_y = panel.bottom - 68.0f;
    DrawButton(address_busy_ ? L"Scanning..." : L"Save address",
               {panel.left + 36.0f, buttons_y, panel.left + 248.0f, buttons_y + 44.0f},
               W_AddressSave, true);
    DrawButton(L"Cancel",
               {panel.left + 264.0f, buttons_y, panel.left + 404.0f, buttons_y + 44.0f},
               W_AddressCancel, false);
}

void App::RenderTopBar() {
    const float w = ui_w_;

    // In fullscreen there is no custom title bar, so the brand stays here.
    if (borderless_) {
        ui_.Text(L"BETTERREMOTEPLAY", {48.0f, 24.0f, 480.0f, 54.0f}, 14.0f,
                 th::Rgba(th::kInk, 0.92f), DWRITE_FONT_WEIGHT_SEMI_BOLD);
    }

    // Below the title bar in windowed mode, and at its original height in
    // fullscreen. The clock/avatar decorations were removed: the custom frame
    // already shows the app identity and the window state.
    const float button_y = borderless_ ? 28.0f : kTitleBarH + 12.0f;
    const D2D1_RECT_F gear{w - kCornerMargin - kCornerButton, button_y, w - kCornerMargin,
                           button_y + kCornerButton};
    const bool settings_hover = hover_ == Hit::SettingsButton;
    ui_.FillRounded(gear, 14.0f, th::Rgba(0xFFFFFF, settings_hover ? 0.14f : 0.06f));
    ui_.StrokeRounded(gear, 14.0f, th::Rgba(0xFFFFFF, 0.10f), 1.0f);
    DrawIcon(ui_, Icon::Gear, gear.left + 22.0f, gear.top + 22.0f, 10.0f,
             th::Rgba(th::kInk, settings_hover ? 1.0f : 0.88f));
    AddHit(Hit::SettingsButton, -1, gear);
}

void App::RenderWindowChrome() {
    if (borderless_ || !window_) {
        return;
    }

    const float w = ui_w_;
    const float h = kTitleBarH;

    // The app draws the title bar on the backbuffer, so it must also tell
    // Win32 which part is draggable and which part is client area.
    window_->SetFrameMetrics(static_cast<int>(std::lround(h * ui_scale_)),
                             static_cast<int>(std::lround(kTitleBarClientRight * ui_scale_)));

    ui_.FillLinearV({0.0f, 0.0f, w, h}, th::Rgba(0x0B0617, 0.97f), th::Rgba(0x0B0617, 0.88f));
    ui_.FillRect({0.0f, h - 1.0f, w, h}, th::Rgba(0xFFFFFF, 0.07f));

    // Small accent mark + brand. The mark is the only piece of "logo" the app
    // needs; it stays crisp at every UI scale because it is drawn by D2D.
    ui_.FillRounded({18.0f, h * 0.5f - 7.0f, 32.0f, h * 0.5f + 7.0f}, 4.0f,
                    th::Rgba(th::kAccent, 0.95f));
    ui_.Text(L"BETTERREMOTEPLAY", {44.0f, 0.0f, 420.0f, h}, 12.5f,
             th::Rgba(th::kInk, 0.90f), DWRITE_FONT_WEIGHT_SEMI_BOLD);

    const float bw = kTitleBarButtonW;
    const float cy = h * 0.5f;

    const Hit buttons[] = {Hit::Minimize, Hit::Maximize, Hit::Close};

    for (int i = 0; i < 3; ++i) {
        const D2D1_RECT_F rect{w - static_cast<float>(3 - i) * bw, 0.0f,
                               w - static_cast<float>(2 - i) * bw, h};
        const bool hovered = hover_ == buttons[i];
        const bool close = buttons[i] == Hit::Close;

        if (hovered) {
            ui_.FillRect(rect, close ? th::Rgba(0xE81123, 0.92f)
                                     : th::Rgba(0xFFFFFF, 0.10f));
        }

        const D2D1_COLOR_F ink =
            th::Rgba(th::kInk, hovered && close ? 1.0f : 0.82f);
        const float cx = rect.left + bw * 0.5f;

        switch (buttons[i]) {
            case Hit::Minimize:
                ui_.FillRect({cx - 6.0f, cy, cx + 6.0f, cy + 1.4f}, ink);
                break;
            case Hit::Maximize:
                if (window_->maximized()) {
                    ui_.StrokeRounded({cx - 6.0f, cy - 6.0f, cx + 4.0f, cy + 4.0f}, 1.5f, ink,
                                      1.15f);
                    ui_.StrokeRounded({cx - 3.0f, cy - 3.0f, cx + 7.0f, cy + 7.0f}, 1.5f, ink,
                                      1.15f);
                } else {
                    ui_.StrokeRounded({cx - 6.0f, cy - 6.0f, cx + 6.0f, cy + 6.0f}, 1.5f, ink,
                                      1.15f);
                }
                break;
            case Hit::Close:
                ui_.Line({cx - 5.5f, cy - 5.5f}, {cx + 5.5f, cy + 5.5f}, ink, 1.35f);
                ui_.Line({cx + 5.5f, cy - 5.5f}, {cx - 5.5f, cy + 5.5f}, ink, 1.35f);
                break;
            default:
                break;
        }

        AddHit(buttons[i], i, rect);
    }
}

void App::DrawSettingLabel(const wchar_t* label, float y, bool enabled) {
    ui_.Text(label, {panel_x_, y, panel_x_ + 460.0f, y + 56.0f}, 16.0f,
             th::Rgba(enabled ? th::kInk : th::kMuted, enabled ? 0.95f : 0.6f),
             DWRITE_FONT_WEIGHT_NORMAL);
}

void App::DrawInfoRow(const wchar_t* label, const std::wstring& value, float y) {
    const float right = panel_right_ > 0.0f ? panel_right_ : ui_w_ - 64.0f;
    DrawSettingLabel(label, y, true);
    ui_.TextEllipsis(value.c_str(), {right - 420.0f, y, right, y + 56.0f}, 15.0f,
                     th::Rgba(th::kInkDim), DWRITE_FONT_WEIGHT_NORMAL,
                     DWRITE_TEXT_ALIGNMENT_TRAILING);
}

void App::DrawDropdown(const wchar_t* label, const std::vector<std::wstring>& options, int selected,
                       float y, int id, bool enabled) {
    const float right = panel_right_ > 0.0f ? panel_right_ : ui_w_ - 64.0f;
    DrawSettingLabel(label, y, enabled);

    const D2D1_RECT_F control{right - 340.0f, y + 8.0f, right, y + 54.0f};
    const bool hovered = hover_ == Hit::Dropdown && hover_index_ == id;

    ui_.FillRounded(control, 12.0f,
                    th::Rgba(0xFFFFFF, hovered ? 0.10f : (enabled ? 0.06f : 0.03f)));
    ui_.StrokeRounded(control, 12.0f,
                      hovered && enabled ? th::Rgba(th::kAccent, 0.55f) : th::Rgba(0xFFFFFF, 0.10f),
                      1.0f);

    std::wstring value;
    if (!options.empty() && selected >= 0 && selected < static_cast<int>(options.size())) {
        value = options[static_cast<size_t>(selected)];
    }
    ui_.TextEllipsis(value.c_str(),
                     {control.left + 16.0f, control.top, control.right - 44.0f, control.bottom},
                     14.5f, th::Rgba(enabled ? th::kInk : th::kMuted, 0.95f));

    const float cx = control.right - 22.0f;
    const float cy = (control.top + control.bottom) * 0.5f;
    ui_.FillTriangle({cx - 5.0f, cy - 2.5f}, {cx + 5.0f, cy - 2.5f}, {cx, cy + 3.5f},
                     th::Rgba(th::kMuted, 0.9f));

    if (!enabled) {
        return;
    }

    AddHit(Hit::Dropdown, id, control);

    if (open_dropdown_ == id) {
        popup_.id = id;
        popup_.anchor = control;
        popup_.options = options;
        popup_.selected = selected;
    }
}

void App::DrawSlider(const wchar_t* label, int& value, int min, int max, int step, float y,
                     int id, bool percent) {
    const float right = panel_right_ > 0.0f ? panel_right_ : ui_w_ - 64.0f;
    DrawSettingLabel(label, y, true);
    const D2D1_RECT_F control{right - 340.0f, y + 8.0f, right, y + 54.0f};
    const float track_l = control.left + 18.0f;
    const float track_r = control.right - 92.0f;
    const float cy = (control.top + control.bottom) * 0.5f;

    // Dragging writes the value straight into the profile but only applies it
    // on release: every step would otherwise send an RNSZ to the NR worker.
    if (pointer_.down && open_dropdown_ < 0 &&
        Contains({track_l - 10.0f, control.top, track_r + 10.0f, control.bottom}, pointer_.x,
                 pointer_.y)) {
        const float t = std::clamp((pointer_.x - track_l) / (track_r - track_l), 0.0f, 1.0f);
        int next = min + static_cast<int>(std::lround(t * (max - min) / step)) * step;
        next = std::clamp(next, min, max);
        if (next != value) {
            value = next;
            if (id == W_NrIntensity || id == W_NrTone || id == W_NrStructure ||
                id == W_NrSkin || id == W_NrScale || id == W_NrPasses) {
                nr_settings_dirty_ = true;
            }
        }
    }

    const float t = max > min ? static_cast<float>(value - min) / static_cast<float>(max - min)
                              : 0.0f;
    const float knob = track_l + t * (track_r - track_l);
    ui_.FillRounded({track_l, cy - 2.0f, track_r, cy + 2.0f}, 2.0f, th::Rgba(0xFFFFFF, 0.10f));
    ui_.FillRounded({track_l, cy - 2.0f, knob, cy + 2.0f}, 2.0f, th::Rgba(th::kAccent, 0.95f));
    ui_.FillCircle(knob, cy, 13.0f, th::Rgba(th::kAccent, 0.16f));
    ui_.FillCircle(knob, cy, 8.0f, th::Rgba(th::kInk, 0.95f));
    ui_.FillCircle(knob, cy, 5.0f, th::Rgba(th::kAccent, 1.0f));

    wchar_t text[32];
    if (percent) {
        swprintf_s(text, L"%d%%", value);
    } else {
        swprintf_s(text, L"%d", value);
    }
    ui_.Text(text, {track_r + 12.0f, control.top, control.right, control.bottom}, 14.0f,
             th::Rgba(th::kInk, 0.95f), DWRITE_FONT_WEIGHT_NORMAL,
             DWRITE_TEXT_ALIGNMENT_TRAILING);
    AddHit(Hit::Slider, id, control);
}

void App::DrawButton(const wchar_t* label, const D2D1_RECT_F& rect, int id, bool primary) {
    const bool hovered = hover_ == Hit::Button && hover_index_ == id;
    const float alpha = hovered ? 1.0f : 0.85f;
    if (primary) {
        ui_.FillRounded(rect, 12.0f, th::Rgba(th::kAccent, hovered ? 0.38f : 0.26f));
        ui_.StrokeRounded(rect, 12.0f, th::Rgba(th::kAccentWarm, hovered ? 0.55f : 0.35f), 1.0f);
    } else {
        ui_.FillRounded(rect, 12.0f, th::Rgba(0xFFFFFF, hovered ? 0.12f : 0.06f));
        ui_.StrokeRounded(rect, 12.0f, th::Rgba(0xFFFFFF, 0.12f), 1.0f);
    }
    ui_.Text(label, {rect.left, rect.top, rect.right, rect.bottom}, 13.5f,
             th::Rgba(th::kInk, alpha), DWRITE_FONT_WEIGHT_SEMI_BOLD,
             DWRITE_TEXT_ALIGNMENT_CENTER);
    AddHit(Hit::Button, id, rect);
}

void App::DrawPinRow(float y) {
    const float right = ui_w_ - 64.0f;
    DrawSettingLabel(L"Pairing PIN", y, true);

    const float box = 38.0f;
    const float gap = 6.0f;
    const float total = 8.0f * box + 7.0f * gap;
    float x = right - total;

    for (int i = 0; i < 8; ++i) {
        const D2D1_RECT_F r{x, y + 10.0f, x + box, y + 54.0f};
        ui_.FillRounded(r, 8.0f, th::Rgba(0xFFFFFF, pin_focus_ ? 0.10f : 0.06f));
        ui_.StrokeRounded(r, 8.0f,
                          pin_focus_ ? th::Rgba(th::kAccent, 0.8f) : th::Rgba(0xFFFFFF, 0.10f),
                          1.0f);
        wchar_t glyph[2] = {
            i < static_cast<int>(pin_.size()) ? static_cast<wchar_t>(pin_[static_cast<size_t>(i)])
                                              : static_cast<wchar_t>(L'\u00B7'),
            0};
        ui_.Text(glyph, {r.left, r.top, r.right, r.bottom}, 18.0f, th::Rgba(th::kInk),
                 DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
        x += box + gap;
    }

    AddHit(Hit::PinField, -1, {right - total, y + 6.0f, right, y + 58.0f});
}

void App::DrawPopup() {
    if (popup_.id < 0 || open_dropdown_ != popup_.id || popup_.options.empty()) {
        popup_.id = -1;
        return;
    }

    const float item_h = 38.0f;
    const float height = item_h * static_cast<float>(popup_.options.size());
    float top = popup_.anchor.bottom + 6.0f;
    if (top + height + 8.0f > ui_h_ - 40.0f) {
        top = popup_.anchor.top - 6.0f - height - 8.0f;
    }

    const D2D1_RECT_F panel{popup_.anchor.left, top, popup_.anchor.right, top + height + 8.0f};
    ui_.FillRounded(panel, 12.0f, th::Rgba(0x1A1226, 0.98f));
    ui_.StrokeRounded(panel, 12.0f, th::Rgba(0xFFFFFF, 0.14f), 1.0f);

    for (int i = 0; i < static_cast<int>(popup_.options.size()); ++i) {
        const D2D1_RECT_F item{panel.left + 6.0f, panel.top + 4.0f + i * item_h,
                               panel.right - 6.0f, panel.top + 4.0f + (i + 1) * item_h};
        const int index = popup_.id * 1000 + i;
        const bool hovered = hover_ == Hit::DropdownItem && hover_index_ == index;
        if (hovered) {
            ui_.FillRounded(item, 8.0f, th::Rgba(0xFFFFFF, 0.08f));
        }
        ui_.Text(popup_.options[static_cast<size_t>(i)].c_str(),
                 {item.left + 12.0f, item.top, item.right - 12.0f, item.bottom}, 14.0f,
                 th::Rgba(i == popup_.selected ? th::kAccent : th::kInk, 0.95f));
        AddHit(Hit::DropdownItem, index, item);
    }

    popup_.id = -1;
}

void App::RenderIntro(double t) {
    const float w = ui_w_;
    const float h = ui_h_;
    const float tf = static_cast<float>(t);
    const float cx = w * 0.5f;

    auto clamp01 = [](float x) { return std::clamp(x, 0.0f, 1.0f); };
    auto ease_out = [](float x) {
        x = std::clamp(x, 0.0f, 1.0f);
        return 1.0f - std::pow(1.0f - x, 3.0f);
    };

    // Everything fades in from black at the start and out to black at the end,
    // so the hand-off to the boot screen is a cut to a dark frame, not a jump.
    const float outro = clamp01((tf - 3.00f) / 0.60f);
    const float fade = clamp01(tf / 0.45f) * (1.0f - outro);

    ui_.BeginFrame();
    ui_.FillLinearV({0.0f, 0.0f, w, h}, th::Rgba(th::kBgTop), th::Rgba(th::kBgBottom));
    ui_.FillRadial(cx + std::sin(tf * 0.4f) * w * 0.12f, h * 0.36f, w * 0.46f,
                   th::Rgba(th::kAccent, 0.14f * fade), th::Rgba(th::kAccent, 0.0f));
    ui_.FillRadial(cx - std::cos(tf * 0.3f) * w * 0.16f, h * 0.70f, w * 0.38f,
                   th::Rgba(0x3E2170, 0.22f * fade), th::Rgba(0x3E2170, 0.0f));

    // Act 1: particles converge from the edges onto the mark and vanish into it.
    const float mx = cx;
    const float my = h * 0.30f;
    const float reach = std::hypot(w, h) * 0.62f;
    for (int i = 0; i < 64; ++i) {
        const float fi = static_cast<float>(i);
        const float delay = (fi / 64.0f) * 0.45f;
        const float p = ease_out((tf - delay) / 1.10f);
        if (p <= 0.0f) {
            continue;
        }
        const float a = fi * 2.39996f;  // golden angle: an even spread without clumps
        const float r = reach * (1.0f - p) + 6.0f;
        const float alpha = std::sin(p * 3.14159f) * 0.85f * fade;
        ui_.FillCircle(mx + std::cos(a) * r, my + std::sin(a) * r, 1.2f + static_cast<float>(i % 3) * 0.5f,
                       th::Rgba(i % 4 == 0 ? th::kAccentWarm : th::kAccent, alpha));
    }

    // The mark draws itself in, then a shock ring leaves it.
    DrawBrandMark(mx, my, clamp01(tf / 0.90f), tf, fade);
    const float shock = clamp01((tf - 1.95f) / 0.90f);
    if (shock > 0.0f && shock < 1.0f) {
        ui_.StrokeCircle(mx, my, 60.0f + 300.0f * ease_out(shock),
                         th::Rgba(th::kAccent, (1.0f - shock) * 0.6f * fade), 2.0f);
    }

    const float size1 = std::clamp(h * 0.064f, 42.0f, 88.0f);
    const float size2 = size1 * 0.62f;
    const float gap = size2 * 0.30f;
    const float y1 = h * 0.62f;
    const float y2 = y1 + size1 * 0.95f;

    const float w_remote_word = ui_.TextWidth(L"Remote", size1, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    const float w_play = ui_.TextWidth(L"Play", size1, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    const float w_remote = w_remote_word + w_play;
    const float w_but = ui_.TextWidth(L"but", size2, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    const float w_better = ui_.TextWidth(L"Better", size2, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    const float x_remote = cx - w_remote * 0.5f;
    const float x_but = cx - (w_but + gap + w_better) * 0.5f;
    const float x_better = x_but + w_but + gap;

    // Act 2: each letter rises and fades in, staggered from the left. Positions
    // come from the prefix widths, so kerning matches the static layout.
    auto reveal_word = [&](const wchar_t* word, float x, float cy, float size, float start,
                           uint32_t rgb, float lift) {
        const std::wstring text(word);
        for (size_t i = 0; i < text.size(); ++i) {
            const float local = ease_out((tf - start - static_cast<float>(i) * 0.045f) / 0.35f);
            if (local <= 0.0f) {
                continue;
            }
            const float ox = i == 0 ? 0.0f
                                    : ui_.TextWidth(text.substr(0, i).c_str(), size,
                                                    DWRITE_FONT_WEIGHT_SEMI_BOLD);
            const std::wstring ch = text.substr(i, 1);
            const float cw = ui_.TextWidth(ch.c_str(), size, DWRITE_FONT_WEIGHT_SEMI_BOLD);
            const float dy = (1.0f - local) * lift;
            ui_.Text(ch.c_str(), {x + ox, cy - size + dy, x + ox + cw + 2.0f, cy + size + dy}, size,
                     th::Rgba(rgb, local * fade), DWRITE_FONT_WEIGHT_SEMI_BOLD);
        }
    };
    reveal_word(L"Remote", x_remote, y1, size1, 0.70f, th::kInk, size1 * 0.25f);
    reveal_word(L"Play", x_remote + w_remote_word, y1, size1, 1.05f, th::kAccent, size1 * 0.25f);
    reveal_word(L"but", x_but, y2, size2, 1.95f, th::kAccent, size2 * 0.25f);
    reveal_word(L"Better", x_better, y2, size2, 2.25f, th::kInk, size2 * 0.25f);

    // Act 3: an accent rule grows under the lockup, then a glint crosses it.
    const float rule = 260.0f * ease_out((tf - 2.35f) / 0.55f);
    ui_.FillRounded({cx - rule * 0.5f, y2 + size2 + 12.0f, cx + rule * 0.5f, y2 + size2 + 14.0f},
                    1.0f, th::Rgba(th::kAccent, 0.85f * fade));
    const float glint = clamp01((tf - 2.55f) / 0.70f);
    if (glint > 0.0f && glint < 1.0f) {
        const float gx = x_remote - 80.0f + glint * (w_remote + 160.0f);
        ui_.FillRadial(gx, y1, size1 * 0.9f, th::Rgba(0xFFFFFF, 0.25f * fade),
                       th::Rgba(0xFFFFFF, 0.0f));
    }

    ui_.EndFrame();
}

std::filesystem::path App::ExeDirPath() const {
    wchar_t path[MAX_PATH] = {};
    ::GetModuleFileNameW(nullptr, path, MAX_PATH);
    return std::filesystem::path(path).parent_path();
}

void App::RenderSetupModal() {
    if (!setup_modal_) {
        return;
    }
    const float w = ui_w_;
    const float h = ui_h_;
    ui_.FillRect({0.0f, 0.0f, w, h}, th::Rgba(0x000000, 0.58f));
    const float panel_w = 720.0f;
    const float panel_h = 300.0f;
    const D2D1_RECT_F panel{(w - panel_w) * 0.5f, (h - panel_h) * 0.5f,
                            (w + panel_w) * 0.5f, (h + panel_h) * 0.5f};
    ui_.FillRounded(panel, 18.0f, th::Rgba(th::kPanel, 0.98f));
    ui_.StrokeRounded(panel, 18.0f, th::Rgba(th::kAccent, 0.25f), 1.0f);
    ui_.Text(L"Frame generation setup",
             {panel.left + 28.0f, panel.top + 22.0f, panel.right - 28.0f, panel.top + 56.0f},
             20.0f, th::Rgba(th::kInk), DWRITE_FONT_WEIGHT_SEMI_BOLD);

    const RifeSetup::State state = rife_setup_.state();
    const std::wstring status = rife_setup_.status();
    ui_.TextWrapped(status.c_str(),
                    {panel.left + 28.0f, panel.top + 68.0f, panel.right - 28.0f,
                     panel.top + 176.0f},
                    13.0f, th::Rgba(th::kInkDim));

    const float progress = rife_setup_.progress();
    if (progress >= 0.0f && state == RifeSetup::State::Running) {
        const float bar_l = panel.left + 28.0f;
        const float bar_r = panel.right - 28.0f;
        const float bar_y = panel.top + 190.0f;
        ui_.FillRounded({bar_l, bar_y, bar_r, bar_y + 5.0f}, 2.5f, th::Rgba(0xFFFFFF, 0.08f));
        ui_.FillRounded({bar_l, bar_y, bar_l + (bar_r - bar_l) * std::clamp(progress, 0.0f, 1.0f),
                         bar_y + 5.0f},
                        2.5f, th::Rgba(th::kAccent, 0.9f));
    }

    const float button_y = panel.bottom - 64.0f;
    if (state != RifeSetup::State::Running) {
        DrawButton(L"Start", {panel.left + 28.0f, button_y, panel.left + 188.0f, button_y + 44.0f},
                   W_SetupStart, true);
    }
    DrawButton(L"Close", {panel.right - 168.0f, button_y, panel.right - 28.0f, button_y + 44.0f},
               W_SetupClose, false);

}

void App::RenderHome(double t) {
    (void)t;
    const float w = ui_w_;
    const float h = ui_h_;

    ui_.FillLinearV({0.0f, 0.0f, w, h}, th::Rgba(th::kBgTop), th::Rgba(th::kBgBottom));
    ui_.FillRadial(w * 0.5f, h * 0.42f, w * 0.55f, th::Rgba(0x7B3CFF, 0.20f),
                   th::Rgba(0x7B3CFF, 0.0f));
    ui_.FillRadial(w * 0.14f, h * 0.12f, w * 0.34f, th::Rgba(0x4A2B8C, 0.16f),
                   th::Rgba(0x4A2B8C, 0.0f));

    RenderTopBar();

    // RIFE one-click setup: a pill next to the gear. It starts immediately and
    // shows progress in a modal.
    {
        const float button_y = borderless_ ? 28.0f : kTitleBarH + 12.0f;
        const D2D1_RECT_F gear{w - kCornerMargin - kCornerButton, button_y,
                               w - kCornerMargin, button_y + kCornerButton};
        const bool ready = rife_setup_.ready(ExeDirPath());
        const wchar_t* label = rife_setup_.state() == RifeSetup::State::Running
                                   ? L"Setting up RIFE..."
                                   : (ready ? L"RIFE ready" : L"Set up RIFE");
        DrawButton(label, {gear.left - 190.0f, gear.top, gear.left - 12.0f, gear.bottom},
                   W_SetupRife, !ready);
    }

    const float card_w = std::min(920.0f, w - 96.0f);
    const float card_h = 340.0f;
    const float x0 = (w - card_w) * 0.5f;
    const float y0 = h * 0.46f - card_h * 0.5f;

    const D2D1_RECT_F card{x0, y0, x0 + card_w, y0 + card_h};
    ui_.FillRounded(card, 22.0f, th::Rgba(0xFFFFFF, 0.05f));
    ui_.StrokeRounded(card, 22.0f, th::Rgba(0xFFFFFF, 0.09f), 1.0f);
    ui_.FillRounded({x0 + 36.0f, y0, x0 + card_w - 36.0f, y0 + 2.0f}, 1.0f, th::Rgba(th::kAccent, 0.75f));

    // Console silhouette on the right of the hero card, with its glow.
    const float cxl = x0 + card_w - 152.0f;
    ui_.FillRadial(cxl, y0 + 176.0f, 170.0f, th::Rgba(th::kAccent, 0.20f), th::Rgba(th::kAccent, 0.0f));
    const D2D1_RECT_F body{cxl - 86.0f, y0 + 44.0f, cxl + 86.0f, y0 + 298.0f};
    ui_.FillRounded(body, 30.0f, th::Rgba(0xFFFFFF, 0.08f));
    ui_.StrokeRounded(body, 30.0f, th::Rgba(th::kAccent, 0.40f), 1.2f);
    ui_.FillRounded({cxl - 46.0f, y0 + 82.0f, cxl + 46.0f, y0 + 86.0f}, 2.0f, th::Rgba(th::kAccent, 0.9f));
    ui_.FillCircle(cxl, y0 + 186.0f, 42.0f, th::Rgba(0xFFFFFF, 0.04f));
    ui_.StrokeRounded({cxl - 42.0f, y0 + 144.0f, cxl + 42.0f, y0 + 228.0f}, 42.0f, th::Rgba(0xFFFFFF, 0.08f), 1.0f);

    ui_.Text(L"PLAYSTATION 5", {x0 + 36.0f, y0 + 26.0f, x0 + card_w - 36.0f, y0 + 48.0f}, 10.5f,
             th::Rgba(th::kMuted, 0.9f), DWRITE_FONT_WEIGHT_SEMI_BOLD);

    const std::wstring name =
        profile_.has_console() ? Widen(profile_.display_name()) : std::wstring(L"No console");
    ui_.Text(name.c_str(), {x0 + 36.0f, y0 + 54.0f, x0 + card_w - 290.0f, y0 + 116.0f}, 42.0f,
             th::Rgba(th::kInk), DWRITE_FONT_WEIGHT_SEMI_BOLD);

    uint32_t dot = th::kOffline;
    std::wstring status;
    switch (probe_.state()) {
        case net::ProbeState::Checking:
            dot = th::kAccent;
            status = L"Checking the control port...";
            break;
        case net::ProbeState::Reachable:
            dot = th::kOnline;
            status = L"Reachable";
            break;
        case net::ProbeState::Unreachable:
            dot = th::kOffline;
            status = L"No answer on the control port";
            break;
        case net::ProbeState::NoAddress:
            dot = th::kMuted;
            status = L"No address configured";
            break;
        case net::ProbeState::Idle:
        default:
            status = stream_note_.empty() ? Widen(profile_.status_line())
                                          : Widen(stream_note_);
            break;
    }

    ui_.FillCircle(x0 + 42.0f, y0 + 140.0f, 4.5f, th::Rgba(dot));
    ui_.Text(status.c_str(), {x0 + 56.0f, y0 + 126.0f, x0 + card_w - 290.0f, y0 + 154.0f}, 14.0f,
             th::Rgba(th::kInkDim));

    std::wstring address = profile_.host.empty() ? L"No address" : Widen(profile_.host);
    address += profile_.needs_pairing() ? L"   ·   pairing needed" : L"   ·   paired";
    ui_.Text(address.c_str(), {x0 + 36.0f, y0 + 158.0f, x0 + card_w - 290.0f, y0 + 182.0f}, 12.5f,
             th::Rgba(th::kMuted));

    const PadState home_pad = pad_.snapshot();
    ui_.FillCircle(x0 + 42.0f, y0 + 201.0f, 4.0f,
                   th::Rgba(home_pad.connected ? th::kOnline : th::kMuted));
    ui_.Text(home_pad.connected ? Widen(home_pad.name).c_str() : L"No controller",
             {x0 + 56.0f, y0 + 188.0f, x0 + card_w - 290.0f, y0 + 214.0f}, 12.5f,
             th::Rgba(th::kMuted));

    const float button_y = y0 + 224.0f;
    const bool no_host = profile_.host.empty();
    DrawButton(no_host ? L"Find console"
                       : (probe_.state() == net::ProbeState::Checking ? L"Checking..."
                                                                    : L"Connect"),
               {x0 + 36.0f, button_y, x0 + 196.0f, button_y + 52.0f}, W_Probe, true);

    const D2D1_RECT_F pair_rect{x0 + 212.0f, button_y, x0 + 372.0f, button_y + 52.0f};
    const bool pair_hover = hover_ == Hit::Button && hover_index_ == W_PairHome;
    ui_.FillRounded(pair_rect, 12.0f, th::Rgba(0xFFFFFF, pair_hover ? 0.14f : 0.07f));
    ui_.StrokeRounded(pair_rect, 12.0f, th::Rgba(0xFFFFFF, 0.12f), 1.0f);
    ui_.Text(L"Pair", {pair_rect.left, pair_rect.top, pair_rect.right, pair_rect.bottom}, 13.5f,
             th::Rgba(th::kInk, 0.92f), DWRITE_FONT_WEIGHT_SEMI_BOLD,
             DWRITE_TEXT_ALIGNMENT_CENTER);
    AddHit(Hit::Button, W_PairHome, pair_rect);

    const D2D1_RECT_F preview_rect{x0 + 388.0f, button_y, x0 + 552.0f, button_y + 52.0f};
    const bool preview_hover = hover_ == Hit::Stream;
    ui_.FillRounded(preview_rect, 12.0f, th::Rgba(0xFFFFFF, preview_hover ? 0.12f : 0.05f));
    ui_.StrokeRounded(preview_rect, 12.0f, th::Rgba(0xFFFFFF, 0.10f), 1.0f);
    ui_.Text(L"Preview pipeline", {preview_rect.left, preview_rect.top, preview_rect.right,
                                   preview_rect.bottom},
             13.0f, th::Rgba(th::kInk, 0.85f), DWRITE_FONT_WEIGHT_NORMAL,
             DWRITE_TEXT_ALIGNMENT_CENTER);
    AddHit(Hit::Stream, -1, preview_rect);

    ui_.Text(no_host
                 ? L"No console address yet. Click Find console to scan the LAN or enter the "
                   L"console IP manually."
                 : L"Connect checks the console control port (9295). Pairing uses the 8-digit "
                   L"code from Settings > System > Remote Play > Link Device.",
             {x0 + 36.0f, y0 + 288.0f, x0 + card_w - 36.0f, y0 + 316.0f}, 11.5f,
             th::Rgba(th::kMuted, 0.85f));

    // Three cards with what a session will use. Every value comes from the saved
    // profile; the library card states what this build cannot show.
    const float row_y = y0 + card_h + 22.0f;
    const float gap = 16.0f;
    const float cw = (card_w - 2.0f * gap) / 3.0f;
    const float rh = 140.0f;
    const auto info_card = [&](float left, const wchar_t* kicker, const std::wstring& title,
                               const std::wstring& body) {
        const D2D1_RECT_F r{left, row_y, left + cw, row_y + rh};
        ui_.FillRounded(r, 16.0f, th::Rgba(0xFFFFFF, 0.04f));
        ui_.StrokeRounded(r, 16.0f, th::Rgba(0xFFFFFF, 0.08f), 1.0f);
        ui_.Text(kicker, {r.left + 20.0f, r.top + 16.0f, r.right - 20.0f, r.top + 34.0f}, 10.0f,
                 th::Rgba(th::kMuted, 0.9f), DWRITE_FONT_WEIGHT_SEMI_BOLD);
        ui_.Text(title.c_str(), {r.left + 20.0f, r.top + 38.0f, r.right - 20.0f, r.top + 66.0f},
                 17.0f, th::Rgba(th::kInk), DWRITE_FONT_WEIGHT_SEMI_BOLD);
        ui_.TextWrapped(body.c_str(), {r.left + 20.0f, r.top + 72.0f, r.right - 20.0f, r.bottom - 10.0f},
                        12.0f, th::Rgba(th::kInkDim), DWRITE_FONT_WEIGHT_NORMAL);
    };

    const wchar_t* res_label = profile_.height >= 1440 ? L"1440p"
                              : (profile_.height >= 1080 ? L"1080p" : L"720p");
    wchar_t stream_title[64];
    swprintf_s(stream_title, L"%s  -  %d fps", res_label, profile_.fps);
    wchar_t stream_body[160];
    std::wstring codec = Widen(profile_.codec);
    std::transform(codec.begin(), codec.end(), codec.begin(), ::towupper);
    swprintf_s(stream_body, L"%s  -  %.0f Mbps  -  %s",
               codec.c_str(), profile_.bitrate_kbps / 1000.0,
               profile_.hdr ? L"HDR" : L"SDR");
    info_card(x0, L"STREAM", stream_title, stream_body);

    const std::vector<std::wstring> upscalers = UpscalerLabels();
    const std::wstring upscale_name =
        profile_.neural_sr > 0 ? std::wstring(L"DLSS Super Resolution")
                               : upscalers[static_cast<size_t>(std::clamp(upscale_, 0, static_cast<int>(upscalers.size()) - 1))];
    std::wstring enh_body = profile_.dlss_fg >= 2
                                ? L"Frame generation " + std::to_wstring(profile_.dlss_fg) + L"x"
                                : std::wstring(L"Frame generation off");
    enh_body += L"  -  sharpening " + std::to_wstring(profile_.sharpness) + L"%";
    info_card(x0 + cw + gap, L"ENHANCEMENT", upscale_name, enh_body);

    info_card(x0 + 2.0f * (cw + gap), L"GAME LIBRARY", L"Not in this build",
              L"Installed titles are not exposed to remote clients. Launch games from the "
              L"console screen.");

    RenderSetupModal();
}

void App::RenderSettings(double t) {
    (void)t;
    const float w = ui_w_;
    const float h = ui_h_;

    ui_.FillLinearV({0.0f, 0.0f, w, h}, th::Rgba(0x0E0818), th::Rgba(th::kBgBottom));
    ui_.FillRadial(w * 0.5f, -h * 0.1f, w * 0.7f, th::Rgba(0x5A2E9E, 0.28f),
                   th::Rgba(0x5A2E9E, 0.0f));

    const float back_y = borderless_ ? 28.0f : kTitleBarH + 12.0f;
    const D2D1_RECT_F back{w - kCornerMargin - kCornerButton, back_y, w - kCornerMargin,
                           back_y + kCornerButton};
    ui_.FillRounded(back, 14.0f, th::Rgba(0xFFFFFF, hover_ == Hit::Back ? 0.14f : 0.06f));
    ui_.StrokeRounded(back, 14.0f, th::Rgba(0xFFFFFF, 0.10f), 1.0f);
    DrawIcon(ui_, Icon::Back, back.left + 22.0f, back.top + 22.0f, 9.5f,
             th::Rgba(th::kInk, hover_ == Hit::Back ? 1.0f : 0.88f));
    AddHit(Hit::Back, -1, back);

    const float header_y = borderless_ ? 44.0f : kTitleBarH + 16.0f;
    ui_.Text(L"Settings", {48.0f, header_y, 420.0f, header_y + 52.0f}, 34.0f,
             th::Rgba(th::kInk), DWRITE_FONT_WEIGHT_SEMI_BOLD);
    if (!session_note_.empty()) {
        ui_.Text(Widen(session_note_).c_str(), {48.0f, header_y + 54.0f, w - 64.0f, header_y + 78.0f},
                 12.0f, th::Rgba(th::kMuted));
    }

    const wchar_t* tab_names[] = {L"Display", L"Upscaling", L"Color", L"Performance", L"Neural", L"Audio", L"Controller", L"Connection", L"Debug", L"About"};
    const Icon tab_icons[] = {Icon::Display, Icon::Upscaling, Icon::Color, Icon::Performance, Icon::Neural,
                              Icon::Audio,   Icon::Controller, Icon::Connection,
                              Icon::Debug,   Icon::About};
    for (int i = 0; i < 10; ++i) {
        const D2D1_RECT_F tab{48.0f, 148.0f + i * 54.0f, 300.0f, 148.0f + i * 54.0f + 44.0f};
        const bool selected = static_cast<int>(tab_) == i;
        const bool hovered = hover_ == Hit::Tab && hover_index_ == i;

        if (selected || hovered) {
            ui_.FillRounded(tab, 12.0f, th::Rgba(0xFFFFFF, hovered ? 0.10f : 0.07f));
        }
        if (selected) {
            ui_.FillRounded({tab.left + 2.0f, tab.top + 9.0f, tab.left + 6.0f, tab.bottom - 9.0f},
                            2.0f, th::Rgba(th::kAccent, 0.95f));
        }
        DrawIcon(ui_, tab_icons[i], tab.left + 26.0f, (tab.top + tab.bottom) * 0.5f, 8.5f,
                 th::Rgba(selected ? th::kInk : th::kMuted, selected ? 0.95f : 0.85f));
        ui_.Text(tab_names[i], {tab.left + 48.0f, tab.top, tab.right - 10.0f, tab.bottom},
                 14.5f, th::Rgba(selected ? th::kInk : th::kInkDim));
        AddHit(Hit::Tab, i, tab);
    }

    ui_.FillRect({324.0f, 140.0f, 325.0f, h - 60.0f}, th::Rgba(0xFFFFFF, 0.06f));
    {
        // The content sits on one glass card so the groups read as sections.
        const D2D1_RECT_F card{336.0f, 132.0f, w - 40.0f, h - 56.0f};
        ui_.FillRounded(card, 18.0f, th::Rgba(th::kPanel, 0.46f));
        ui_.StrokeRounded(card, 18.0f, th::Rgba(0xFFFFFF, 0.07f), 1.0f);
    }

    float y = 148.0f;

    switch (tab_) {
        case Tab::Display: {
            DrawDropdown(L"HDR output", {L"Off", L"On (HDR10, restart)"}, hdr_ ? 1 : 0, y, W_Hdr, true);
            y += kRowH;
            DrawDropdown(L"Video decoder (next session)",
                         {L"Auto", L"D3D11VA", L"D3D12VA", L"Vulkan", L"DXVA2",
                          L"CUDA (NVDEC)", L"Software"},
                         std::clamp(profile_.decoder, 0, 6), y, W_Decoder);
            y += kRowH;
            DrawDropdown(L"Chroma upsampling (next session)",
                         {L"Bilinear (fast)", L"Bicubic", L"Lanczos (sharpest)"},
                         std::clamp(profile_.chroma, 0, 2), y, W_Chroma);
            y += kRowH;
            DrawDropdown(L"Latency HUD", {L"Off", L"On"}, hud_ ? 1 : 0, y, W_Hud);
            y += kRowH;
            ui_.Text(L"DLSS, frame generation and neural rendering have their own tabs; frame buffer and Reflex are under Performance.",
                     {kContentX, y + 2.0f, w - 64.0f, y + 26.0f}, 12.0f, th::Rgba(th::kMuted));
            break;
        }

        case Tab::Color: {
            DrawDropdown(L"Colour controls", {L"Off (sharpening only)", L"On"},
                         profile_.color_on > 0 ? 1 : 0, y, W_ColorOn);
            y += 44.0f;
            DrawSlider(L"Sharpening", profile_.sharpness, 0, 100, 5, y, W_Sharpness);
            y += 44.0f;
            DrawSlider(L"Exposure", profile_.color_exposure, -100, 100, 5, y, W_ColorExposure);
            y += 44.0f;
            DrawSlider(L"Contrast", profile_.color_contrast, 50, 150, 1, y, W_ColorContrast);
            y += 44.0f;
            DrawSlider(L"Highlights", profile_.color_highlights, -100, 100, 5, y,
                       W_ColorHighlights);
            y += 44.0f;
            DrawSlider(L"Shadows", profile_.color_shadows, -100, 100, 5, y, W_ColorShadows);
            y += 44.0f;
            DrawSlider(L"Saturation", profile_.color_saturation, 0, 200, 5, y, W_ColorSaturation);
            y += 44.0f;
            DrawSlider(L"Vibrance", profile_.color_vibrance, 0, 100, 5, y, W_ColorVibrance);
            y += 44.0f;
            DrawSlider(L"Hue (degrees)", profile_.color_hue, -180, 180, 5, y, W_ColorHue, false);
            y += 44.0f;
            DrawSlider(L"Temperature", profile_.color_temperature, -100, 100, 5, y,
                       W_ColorTemperature);
            y += 44.0f;
            DrawSlider(L"Tint", profile_.color_tint, -100, 100, 5, y, W_ColorTint);
            y += 44.0f;
            DrawSlider(L"Gamma", profile_.color_gamma, 80, 120, 1, y, W_ColorGamma);
            y += 56.0f;

            const float bw = 190.0f;
            const float gap = 10.0f;
            DrawButton(L"Neutral", {kContentX, y, kContentX + bw, y + 40.0f}, W_ColorNeutral,
                       false);
            DrawButton(L"Vivid", {kContentX + bw + gap, y, kContentX + 2 * bw + gap, y + 40.0f},
                       W_ColorVivid, false);
            DrawButton(L"Cinematic",
                       {kContentX + 2 * (bw + gap), y, kContentX + 3 * bw + 2 * gap, y + 40.0f},
                       W_ColorCinematic, false);
            DrawButton(L"Warm",
                       {kContentX + 3 * (bw + gap), y, kContentX + 4 * bw + 3 * gap, y + 40.0f},
                       W_ColorWarm, false);
            y += 50.0f;
            DrawButton(L"Reset", {kContentX, y, kContentX + bw, y + 40.0f}, W_ColorReset, false);
            y += 52.0f;

            wchar_t status[192];
            swprintf_s(status, L"Sharpening %d%%   -   colour %s   -   %s", profile_.sharpness,
                       profile_.color_on > 0 ? L"on" : L"off",
                       enh_applied_.active ? L"applied before the FG worker"
                                           : L"bypassed (neutral)");
            ui_.Text(status, {kContentX, y, w - 64.0f, y + 24.0f}, 12.0f, th::Rgba(th::kMuted));
            y += 28.0f;
            ui_.Text(hdr_ ? L"HDR output: exposure, white balance, saturation, hue and "
                            L"sharpening are graded in linear light; highlights, shadows, gamma "
                            L"and vibrance act on the SDR-referred curve."
                          : L"Grading happens on the GPU before frame generation, so the worker "
                            L"presents the same pixels the window would show.",
                     {kContentX, y, w - 64.0f, y + 40.0f}, 11.5f, th::Rgba(th::kMuted));
            break;
        }

        case Tab::Performance: {
            DrawDropdown(L"Frame buffer",
                         {L"0 (fixed, lowest)", L"1 (adaptive)", L"2 (adaptive)",
                          L"3 (adaptive max)"},
                         stream_buffer_, y, W_FrameBuffer);
            y += kRowH;
            DrawDropdown(L"NVIDIA Reflex", {L"Off", L"On", L"On + Boost"}, profile_.reflex, y,
                         W_Reflex);
            y += kRowH;
            if (profile_.reflex > 0) {
                ui_.Text(L"Reflex markers are active. Its sleep stage is not used: it "
                         L"targets a game-style simulation window and fights vblank pacing.",
                         {kContentX, y + 2.0f, w - 64.0f, y + 28.0f}, 11.5f,
                         th::Rgba(th::kMuted));
                y += 30.0f;
            }

            // One-click fallbacks for launch night. "FG without SR" is not
            // offered: a worker restarted at 1080p without SR was measured to
            // come up without presenting, so the ladder uses only verified
            // paths.
            y += 24.0f;
            ui_.Text(L"Launch presets", {kContentX, y, w - 64.0f, y + 30.0f}, 16.0f,
                     th::Rgba(th::kInk), DWRITE_FONT_WEIGHT_SEMI_BOLD);
            y += 38.0f;
            DrawButton(L"Launch: SR + FG 2x",
                       {kContentX, y, kContentX + 250.0f, y + 44.0f}, W_PresetLaunch, true);
            DrawButton(L"SR only", {kContentX + 266.0f, y, kContentX + 516.0f, y + 44.0f},
                       W_PresetSr, false);
            y += 54.0f;
            DrawButton(L"Low latency", {kContentX, y, kContentX + 250.0f, y + 44.0f},
                       W_PresetLow, false);
            y += 56.0f;
            ui_.Text(L"Presets change the whole chain; they apply at the start of the next "
                     L"session (reconnect), never mid-stream.",
                     {kContentX, y, w - 64.0f, y + 28.0f}, 11.5f, th::Rgba(th::kMuted));
            break;
        }

        case Tab::Upscaling: {
            const bool dlss = DlssAvailable();
            DrawDropdown(L"Upscaler", UpscalerLabels(),
                         profile_.neural_sr > 0 ? 5 : upscale_, y, W_Upscaler);
            y += kRowH;
            DrawDropdown(L"DLSS model preset",
                         {L"K (Transformer 1, stable)", L"L (Transformer 2, best)",
                          L"M (Transformer 2, sharp)"},
                         profile_.dlss_preset == 12 ? 1 : (profile_.dlss_preset == 13 ? 2 : 0),
                         y, W_DlssPreset, dlss && profile_.neural_sr > 0);
            y += kRowH;
            DrawDropdown(L"DLSS mode",
                         {L"Ultra Performance (33%)", L"Performance (50%)",
                          L"Balanced (58%)", L"Quality (66%)", L"Ultra Quality (77%)",
                          L"DLAA (100% - AI anti-aliasing)"},
                         std::clamp(profile_.dlss_mode, 0, 5), y, W_DlssMode,
                         dlss && profile_.neural_sr > 0);
            y += kRowH;
            DrawDropdown(L"Upscale target",
                         {L"Window / display", L"1080p", L"1440p", L"2160p"}, render_target_, y,
                         W_RenderTarget);
            y += kRowH;

            ui_.Text(L"The percentage is the resolution DLSS receives, taken from the output. "
                     L"Below the 1080p stream it downscales first (faster, softer); above it the "
                     L"stream is the limit. DLAA runs at the frame size with AI AA, then the "
                     L"upscaler takes it to the target.",
                     {kContentX, y + 2.0f, w - 64.0f, y + 26.0f}, 11.5f, th::Rgba(th::kMuted));
            y += 30.0f;

            if (profile_.neural_sr > 0 && dlss_ready_ && profile_.fg_engine != 1) {
                wchar_t text[224];
                swprintf_s(text,
                           L"DLSS SR %s active  -  %dx%d -> %dx%d  -  %.1f ms  -  %s",
                           profile_.dlss_preset == 12 ? L"L" : (profile_.dlss_preset == 13 ? L"M" : L"K"),
                           dlss_in_w_, dlss_in_h_, dlss_sr_.out_width(),
                           dlss_sr_.out_height(), dlss_sr_.last_ms(),
                           profile_.neural_nr > 0 ? L"NR after SR" : L"");
                ui_.Text(text, {kContentX, y + 2.0f, w - 64.0f, y + 26.0f}, 12.0f,
                         th::Rgba(th::kMuted));
                y += 24.0f;
            } else if (!dlss) {
                ui_.Text(L"DLSS runtime missing: nvngx_dlss.dll",
                         {kContentX, y + 2.0f, w - 64.0f, y + 26.0f}, 12.0f,
                         th::Rgba(th::kMuted));
                y += 24.0f;
            } else if (!dlss_note_.empty()) {
                ui_.Text(dlss_note_.c_str(), {kContentX, y + 2.0f, w - 64.0f, y + 26.0f},
                         12.0f, th::Rgba(th::kMuted));
                y += 24.0f;
            }

            y += 18.0f;
            ui_.Text(L"Frame generation", {kContentX, y, w - 64.0f, y + 30.0f}, 16.0f,
                     th::Rgba(th::kInk), DWRITE_FONT_WEIGHT_SEMI_BOLD);
            y += 36.0f;
            {
                const bool available = FrameGenAvailable();
                std::vector<std::wstring> options;
                if (available) {
                    options = {L"Off", L"NVIDIA VFG 2x (120 fps)"};
                } else {
                    options = {L"Off  -  NVIDIA VFX runtime missing"};
                }
                int selected = 0;
                if (frame_gen_ == 2) {
                    selected = 1;
                }
                DrawDropdown(L"NVIDIA VFG (legacy)", options, selected, y, W_FrameGen, available);
            }
            y += kRowH;
            DrawDropdown(L"DLSS Frame Generation", {L"Off", L"On"},
                         profile_.dlss_fg >= 2 ? 1 : 0, y, W_DlssFg);
            y += kRowH;
            DrawDropdown(L"Interpolation engine",
                         {L"NVIDIA DLSS FG (fast)", L"RIFE 4.25 lite (TensorRT)"},
                         profile_.fg_engine == 1 ? 1 : 0, y, W_FgEngine,
                         profile_.dlss_fg >= 2);
            y += kRowH;
            DrawDropdown(L"RIFE output",
                         {L"60 fps (one frame between each pair)", L"Pass-through"},
                         profile_.rife_output == 0 ? 1 : 0, y, W_RifeOutput,
                         profile_.dlss_fg >= 2 && profile_.fg_engine == 1);
            y += kRowH;
            DrawDropdown(L"Neural rendering (DLSS 5)", {L"Off", L"On"},
                         profile_.neural_nr > 0 ? 1 : 0, y, W_NeuralNr, NrAvailable());
            y += kRowH;
            if (profile_.fg_engine == 1) {
                ui_.Text(L"RIFE generates one frame per pair: a 30 fps game plays at 60. "
                         L"90/120 need the NVIDIA engine. The model is fixed (4.25 lite). "
                         L"DLSS SR is bypassed here; neural rendering, when on, runs on the 1080p "
                         L"stream before RIFE. Clarity comes from the upscaler and "
                         L"sharpening below.",
                         {kContentX, y + 2.0f, w - 64.0f, y + 46.0f}, 11.5f,
                         th::Rgba(th::kMuted));
                y += 50.0f;
            }
            if (profile_.fg_engine != 1) {
                DrawDropdown(L"Output frame rate",
                             {L"Match display", L"60 fps", L"90 fps", L"120 fps"},
                             profile_.fg_target == 60
                                 ? 1
                                 : (profile_.fg_target == 90
                                        ? 2
                                        : (profile_.fg_target == 120 ? 3 : 0)),
                             y, W_FgTarget, profile_.dlss_fg >= 2);
                y += kRowH;
            }

            if (frame_gen_ >= 2 && fg_ready_) {
                wchar_t text[192];
                swprintf_s(text, L"NVIDIA VFG active  -  60 -> %d fps  -  last %.1f ms",
                           60 * frame_gen_, nv_vfx_.last_generate_ms());
                ui_.Text(text, {kContentX, y + 2.0f, w - 64.0f, y + 28.0f}, 12.0f,
                         th::Rgba(th::kMuted));
            } else if (profile_.dlss_fg >= 2 && profile_.fg_engine == 1) {
                wchar_t text[256];
                if (!stream_active_) {
                    swprintf_s(text,
                               L"RIFE selected  -  loads at the next session  -  one frame "
                               L"between each pair (one source interval of delay)");
                } else if (rife_ && rife_->ready()) {
                    swprintf_s(text,
                               L"RIFE %.0f fps shown  -  %.1f ms/frame  -  %dx%d engine  -  "
                               L"content %.1f fps  -  %s",
                               rife_out_fps_, rife_ms_, rife_->width(), rife_->height(),
                               fg_content_hz_,
                               profile_.neural_nr > 0 && nr_ready_
                                   ? L"Neural Rendering before RIFE"
                                   : L"Neural Rendering off");
                } else {
                    swprintf_s(text, L"RIFE engine not loaded. %s", rife_note_.c_str());
                }
                ui_.Text(text, {kContentX, y + 2.0f, w - 64.0f, y + 28.0f}, 12.0f,
                         th::Rgba(th::kMuted, 0.95f));
                ui_.Text(L"RIFE needs TensorRT 10.16 + CUDA 13 on the PC and an engine built for "
                         L"this GPU (rife\\rife425_1080.engine next to the exe, BRP_RIFE_ENGINE "
                         L"to override).",
                         {kContentX, y + 28.0f, w - 64.0f, y + 54.0f}, 11.5f,
                         th::Rgba(th::kMuted));
            } else if (profile_.dlss_fg >= 2) {            } else if (!FrameGenAvailable()) {
                ui_.Text(L"NVIDIA VFX runtime not found: third_party\\nvidia-vfx\\runtime",
                         {kContentX, y + 2.0f, w - 64.0f, y + 28.0f}, 12.0f,
                         th::Rgba(th::kMuted));
            } else if (!fg_note_.empty()) {
                ui_.Text(fg_note_.c_str(), {kContentX, y + 2.0f, w - 64.0f, y + 28.0f}, 12.0f,
                         th::Rgba(th::kMuted));
            } else {
                ui_.Text(L"NVIDIA VFG ready - select a multiplier to enable",
                         {kContentX, y + 2.0f, w - 64.0f, y + 28.0f}, 12.0f,
                         th::Rgba(th::kMuted));
            }
            break;
        }
        case Tab::Neural: {
            const float row = 52.0f;
            ui_.Text(L"DLSS Neural Rendering (NR)",
                     {kContentX, y, w - 64.0f, y + 30.0f}, 16.0f, th::Rgba(th::kInk),
                     DWRITE_FONT_WEIGHT_SEMI_BOLD);
            y += 36.0f;

            DrawDropdown(L"Neural Rendering", {L"Off", L"On"}, profile_.neural_nr > 0 ? 1 : 0, y,
                         W_NeuralNr);
            y += row;
            DrawDropdown(L"Pipeline",
                         {L"Upstream: NR -> SR", L"NVIDIA order: SR -> NR"},
                         std::clamp(profile_.nr_order, 0, 1), y, W_NrOrder);
            y += row;
            DrawSlider(L"Render scale", profile_.nr_scale, 50, 100, 1, y, W_NrScale);
            y += row;
            DrawDropdown(L"Stack passes", {L"1", L"2", L"3", L"4"},
                         std::clamp(profile_.nr_passes - 1, 0, 3), y, W_NrPasses);
            y += row;
            DrawDropdown(L"Style", {L"Default", L"Natural", L"Cinematic"},
                         std::clamp(profile_.nr_style, 0, 2), y, W_NrStyle);
            y += row;
            DrawSlider(L"Strength", profile_.nr_intensity, 0, 200, 5, y, W_NrIntensity);
            y += row;
            DrawSlider(L"Local tone", profile_.nr_tone, 0, 200, 5, y, W_NrTone);
            y += row;
            DrawSlider(L"Structure", profile_.nr_structure, 0, 200, 5, y, W_NrStructure);
            y += row;
            DrawSlider(L"Character / skin", profile_.nr_skin, 0, 200, 5, y, W_NrSkin);
            y += row;
            DrawDropdown(L"Character mask", {L"Off", L"On"}, profile_.nr_mask > 0 ? 1 : 0, y,
                         W_NrMask);
            y += row;
            DrawDropdown(L"UI correction", {L"Off", L"On"}, profile_.nr_ui > 0 ? 1 : 0, y,
                         W_NrUi);
            y += row;

            if (profile_.neural_nr > 0 && nr_ready_) {
                wchar_t text[256];
                swprintf_s(text,
                           L"NR active  -  %dx%d  -  net %dx%d (%d pass)  -  %.1f ms  -  %llu frames  -  %llu dropped",
                           nr_in_w_, nr_in_h_, nr_.work_width(), nr_.work_height(),
                           profile_.nr_passes, nr_last_ms_,
                           static_cast<unsigned long long>(nr_shown_),
                           static_cast<unsigned long long>(nr_dropped_));
                if (profile_.neural_sr > 0 && dlss_ready_) {
                    wcscat_s(text, profile_.nr_order == 1 ? L"  -  after DLSS SR"
                                                          : L"  -  before DLSS SR");
                }
                ui_.Text(text, {kContentX, y + 2.0f, w - 64.0f, y + 26.0f}, 12.0f,
                         th::Rgba(th::kMuted));
            } else if (!NrAvailable()) {
                ui_.Text(L"NR runtime missing: native\\nvngx.dll + nvngx_dlssnr.dll",
                         {kContentX, y + 2.0f, w - 64.0f, y + 26.0f}, 12.0f,
                         th::Rgba(th::kMuted));
            } else if (profile_.neural_nr > 0 && !nr_note_.empty()) {
                ui_.Text(nr_note_.c_str(), {kContentX, y + 2.0f, w - 64.0f, y + 26.0f}, 12.0f,
                         th::Rgba(th::kMuted));
            } else {
                ui_.Text(L"DLSSNR off. DLSS Super Resolution is under Upscaling.",
                         {kContentX, y + 2.0f, w - 64.0f, y + 26.0f}, 12.0f,
                         th::Rgba(th::kMuted));
            }
            y += 28.0f;
            ui_.Text(L"Render scale is the network size as a % of the NR stage; passes are "
                     L"independent. NVIDIA order runs NR on the DLSS SR frame (needs SR and a "
                     L"target of at most 1440p).",
                     {kContentX, y + 2.0f, w - 64.0f, y + 26.0f}, 11.5f, th::Rgba(th::kMuted));
            break;
        }

        case Tab::Audio: {
            std::vector<std::wstring> devices;
            if (audio_devices_.empty()) {
                devices.push_back(L"System default");
            }
            for (const audio::DeviceInfo& device : audio_devices_) {
                std::wstring name = Widen(device.name);
                if (device.is_default) {
                    name += L"  (default)";
                } else if (device.is_controller) {
                    name += L"  (controller)";
                }
                devices.push_back(name);
            }
            const int selected_device = profile_.audio_device_id.empty()
                                           ? 0
                                           : std::clamp(audio_device_ + 1, 0,
                                                        static_cast<int>(devices.size()) - 1);
            DrawDropdown(L"Output device", devices, selected_device, y, W_AudioDevice);
            y += kRowH;
            DrawDropdown(L"Buffer", {L"10 ms", L"20 ms", L"30 ms", L"40 ms"},
                         BufferIndex(audio_buffer_ms_), y, W_AudioBuffer);
            y += kRowH;

            DrawButton(L"Play test tone", {kContentX, y + 6.0f, kContentX + 220.0f, y + 50.0f},
                       W_TestTone, false);
            y += kRowH;

            std::wstring info;
            if (audio_.is_open()) {
                wchar_t buffer[256];
                swprintf_s(buffer, L"%s   ·   %.0f ms   ·   underruns %d",
                           Widen(audio_.format_text()).c_str(), audio_.latency_ms(),
                           audio_.underruns());
                info = buffer;
            } else {
                info = audio_failed_ ? L"Could not open the device (busy or removed)"
                                     : L"Not started";
            }
            ui_.Text(info.c_str(), {kContentX, y, w - 64.0f, y + 40.0f}, 12.0f,
                     th::Rgba(th::kMuted));
            break;
        }

        case Tab::Controller: {
            const float row = 56.0f;
            DrawDropdown(L"Input source", {kInputSourceNames[0], kInputSourceNames[1],
                                           kInputSourceNames[2]},
                         input_source_, y, W_InputSource);
            y += row;
            {
                int rate_index = 2;
                const int rates[] = {60, 125, 250, 500, 1000};
                for (int i = 0; i < 5; ++i) {
                    if (rates[i] == pad_rate_hz_) rate_index = i;
                }
                DrawDropdown(L"Report rate (USB up to 1000 Hz)",
                             {L"60 Hz", L"125 Hz", L"250 Hz", L"500 Hz", L"1000 Hz"},
                             rate_index, y, W_PadRate);
            }
            y += row;
            DrawDropdown(L"Console rumble", {L"On", L"Off"}, console_rumble_ ? 0 : 1, y,
                         W_ConsoleRumble);
            y += row;
            DrawDropdown(L"Haptics intensity",
                         {L"Off", L"Very weak", L"Weak", L"Normal", L"Strong", L"Very strong"},
                         std::clamp(profile_.haptic_intensity, 0, 5), y, W_HapticIntensity);
            y += row;
            DrawDropdown(L"Console adaptive triggers", {L"On", L"Off"},
                         console_triggers_ ? 0 : 1, y, W_ConsoleTriggers);
            y += row;
            DrawDropdown(L"Motion (gyro/accel)", {L"On", L"Off"}, motion_ ? 0 : 1, y, W_Motion);
            y += row;
            DrawDropdown(L"Lightbar",
                         {kLightbarNames[0], kLightbarNames[1], kLightbarNames[2],
                          kLightbarNames[3], kLightbarNames[4], kLightbarNames[5],
                          kLightbarNames[6], kLightbarNames[7], kLightbarNames[8]},
                         lightbar_index_, y, W_Lightbar);
            y += row;
            DrawDropdown(L"Adaptive trigger",
                         {kTriggerNames[0], kTriggerNames[1], kTriggerNames[2], kTriggerNames[3],
                          kTriggerNames[4], kTriggerNames[5], kTriggerNames[6], kTriggerNames[7]},
                         trigger_effect_, y, W_Trigger);
            y += row;
            DrawDropdown(L"Trigger strength",
                         {L"1", L"2", L"3", L"4", L"5", L"6", L"7", L"8"}, trigger_strength_ - 1,
                         y, W_Strength);
            y += row;
            DrawDropdown(L"Player LEDs",
                         {kPlayerLedNames[0], kPlayerLedNames[1], kPlayerLedNames[2],
                          kPlayerLedNames[3], kPlayerLedNames[4], kPlayerLedNames[5],
                          kPlayerLedNames[6]},
                         player_leds_, y, W_PlayerLeds);
            y += row;
            DrawDropdown(L"Mic LED", {kMicLedNames[0], kMicLedNames[1], kMicLedNames[2]}, mic_led_,
                         y, W_MicLed);
            y += row;

            DrawButton(L"Rumble test", {kContentX, y + 6.0f, kContentX + 220.0f, y + 50.0f},
                       W_Rumble, false);
            y += row;

            std::wstring status = PadStatus(pad_.snapshot());
            if (pad_.is_sony()) {
                wchar_t extra[160];
                swprintf_s(extra, L"   -   fw 0x%04X   -   gyro %s%s", pad_.firmware_version(),
                           pad_.calibrated() ? L"calibrated" : L"default",
                           pad_.is_edge() ? L"   -   Edge" : L"");
                status += extra;
                status += last_send_ok_ ? L"   -   output OK" : L"   -   output FAILED";
            }
            ui_.Text(status.c_str(), {kContentX, y, w - 64.0f, y + 40.0f}, 12.0f,
                     th::Rgba(th::kMuted));

            if (pad_.is_sony()) {
                const PadState pad_state = pad_.snapshot();
                if (pad_state.connected) {
                    wchar_t motion[192];
                    swprintf_s(motion,
                               L"Gyro %.1f / %.1f / %.1f dps      Accel %.2f / %.2f / %.2f g",
                               pad_state.gyro_dps[0], pad_state.gyro_dps[1], pad_state.gyro_dps[2],
                               pad_state.accel_g[0], pad_state.accel_g[1], pad_state.accel_g[2]);
                    ui_.Text(motion, {kContentX, y + 42.0f, w - 64.0f, y + 66.0f}, 11.5f,
                             th::Rgba(th::kMuted, 0.85f));
                }
            }
            break;
        }

        case Tab::Connection: {
            DrawInfoRow(L"Console",
                        profile_.has_console() ? Widen(profile_.display_name())
                                               : std::wstring(L"None"),
                        y);
            y += kRowH;
            DrawSettingLabel(L"Address", y, true);
            {
                const float right = w - 64.0f;
                ui_.TextEllipsis(profile_.host.empty() ? L"Not set"
                                                       : Widen(profile_.host).c_str(),
                                 {right - 620.0f, y, right - 260.0f, y + 56.0f}, 15.0f,
                                 th::Rgba(th::kInkDim), DWRITE_FONT_WEIGHT_NORMAL,
                                 DWRITE_TEXT_ALIGNMENT_TRAILING);
                DrawButton(profile_.host.empty() ? L"Find console" : L"Change address",
                           {right - 240.0f, y + 8.0f, right, y + 52.0f}, W_AddressOpen,
                           profile_.host.empty());
            }
            y += kRowH;
            DrawInfoRow(L"Pairing",
                        profile_.needs_pairing() ? L"Needed (8-digit console code)" : L"Paired", y);
            y += kRowH;
            DrawSettingLabel(L"PSN account", y, true);
            {
                const float right = w - 64.0f;
                const bool linked = !profile_.psn_account_id.empty();
                if (linked) {
                    const std::wstring linked_text =
                        profile_.psn_numeric_id.empty()
                            ? std::wstring(L"Linked")
                            : L"Linked: " + Widen(profile_.psn_numeric_id);
                    ui_.Text(linked_text.c_str(), {kContentX, y + 30.0f, right - 260.0f,
                                                   y + 54.0f},
                             11.5f, th::Rgba(th::kMuted));
                }
                DrawButton(linked ? L"Re-link PSN" : L"Sign in with PSN",
                           {right - 240.0f, y + 8.0f, right, y + 52.0f}, W_PsnLogin,
                           !linked);
            }
            y += kRowH;
            DrawInfoRow(L"Stream defaults",
                        Widen(std::to_string(profile_.bitrate_kbps / 1000) + " Mbps · " +
                              profile_.codec + (profile_.hdr ? " · HDR" : " · SDR")),
                        y);
            y += kRowH;

            {
                int bitrate_index = 2;
                bitrate_index = BitrateIndex(profile_.bitrate_kbps);
                DrawDropdown(L"Bitrate",
                             {L"20 Mbps", L"35 Mbps", L"50 Mbps", L"75 Mbps", L"100 Mbps"},
                             bitrate_index, y, W_Bitrate);
            }
            y += kRowH;

            DrawDropdown(L"Stream resolution", {L"720p", L"1080p"},
                         profile_.width <= 1280 ? 0 : 1, y, W_Resolution);
            y += kRowH;

            DrawDropdown(L"Stream FPS", {L"30", L"60"}, profile_.fps == 30 ? 0 : 1, y, W_Fps);
            y += kRowH;

            DrawPinRow(y);
            y += kRowH;

            DrawButton(L"Test reachability", {kContentX, y + 6.0f, kContentX + 220.0f, y + 50.0f},
                       W_Probe, true);
            DrawButton(pairing_.state() == session::PairState::Running ? L"Pairing..." : L"Pair",
                       {kContentX + 240.0f, y + 6.0f, kContentX + 400.0f, y + 50.0f}, W_Pair, true);
            y += kRowH;

            std::wstring detail;
            if (pairing_.state() != session::PairState::Idle) {
                detail = Widen(pairing_.message());
            } else if (!probe_.detail().empty()) {
                detail = Widen(probe_.detail());
            } else {
                detail = L"Click the PIN boxes and type the 8-digit code from the console "
                         L"(Settings > System > Remote Play > Link Device).";
            }
            ui_.Text(detail.c_str(), {kContentX, y, w - 64.0f, y + 60.0f}, 12.0f,
                     th::Rgba(th::kMuted));
            break;
        }

        case Tab::Debug: {
            const float row = 52.0f;
            ui_.Text(L"Debug overlay",
                     {kContentX, y, w - 64.0f, y + 30.0f}, 16.0f, th::Rgba(th::kInk),
                     DWRITE_FONT_WEIGHT_SEMI_BOLD);
            y += 36.0f;
            ui_.Text(L"Sections of the latency HUD. Turn the HUD on under Display; these apply live and are saved.",
                     {kContentX, y + 2.0f, w - 64.0f, y + 26.0f}, 11.5f, th::Rgba(th::kMuted));
            y += 30.0f;

            DrawDropdown(L"Frame times + stutters", {L"Off", L"On"},
                         (profile_.hud_mask & 0x1) ? 1 : 0, y, W_HudFrames);
            y += row;
            DrawDropdown(L"GPU / render summary", {L"Off", L"On"},
                         (profile_.hud_mask & 0x2) ? 1 : 0, y, W_HudGpu);
            y += row;
            DrawDropdown(L"Controller status", {L"Off", L"On"},
                         (profile_.hud_mask & 0x4) ? 1 : 0, y, W_HudPad);
            y += row;
            DrawDropdown(L"Stream / chain timings", {L"Off", L"On"},
                         (profile_.hud_mask & 0x8) ? 1 : 0, y, W_HudStream);
            y += row;
            DrawDropdown(L"Network / decoder line", {L"Off", L"On"},
                         (profile_.hud_mask & 0x10) ? 1 : 0, y, W_HudNet);
            y += row;

            DrawButton(L"Reset frame statistics", {kContentX, y + 6.0f, kContentX + 280.0f,
                                                   y + 50.0f},
                       W_HudReset, false);
            break;
        }
        case Tab::About: {
            ui_.Text(L"BetterRemotePlay 0.4", {kContentX, y, w - 64.0f, y + 40.0f}, 20.0f,
                     th::Rgba(th::kInk), DWRITE_FONT_WEIGHT_SEMI_BOLD);
            y += 48.0f;
            ui_.Text(L"A PS5 to PC Remote Play client with a real post-process pipeline: "
                     L"color, upscaling, latency instrumentation, DualSense output and "
                     L"in-process audio.",
                     {kContentX, y, w - 64.0f, y + 56.0f}, 13.0f, th::Rgba(th::kInkDim));
            y += 70.0f;
            ui_.Text(L"Not affiliated with Sony Interactive Entertainment. No Sony code, keys "
                     L"or assets are included.",
                     {kContentX, y, w - 64.0f, y + 56.0f}, 13.0f, th::Rgba(th::kMuted));
            y += 60.0f;
            ui_.Text(L"Docs: docs/01-landscape.md … docs/12-steam-input.md",
                     {kContentX, y, w - 64.0f, y + 40.0f}, 13.0f, th::Rgba(th::kMuted));
            break;
        }
    }

    DrawPopup();

}

void App::RenderStream(double t) {
    (void)t;
    const float w = ui_w_;

    // Connecting screen: the boot loader, with the real session phases.
    if (stream_connecting_ || pending_connect_ != PendingConnect::None ||
        (stream_active_ && last_source_at_ < 0.0)) {
        const float h = ui_h_;
        const float ct = static_cast<float>(clock_);
        ui_.FillLinearV({0.0f, 0.0f, w, h}, th::Rgba(th::kBgTop), th::Rgba(th::kBgBottom));
        ui_.FillRadial(w * 0.5f, h * 0.40f, w * 0.60f, th::Rgba(0x6B32D9, 0.22f),
                       th::Rgba(0x6B32D9, 0.0f));
        ui_.FillRadial(w * 0.12f, h * 0.10f, w * 0.30f, th::Rgba(0x3E2170, 0.18f),
                       th::Rgba(0x3E2170, 0.0f));

        const float cx = w * 0.5f;
        const float cy = h * 0.34f;
        DrawLoaderMark(cx, cy, 1.0f, ct, 1.0f);

        ui_.Text(L"Connecting", {cx - 300.0f, cy + 104.0f, cx + 300.0f, cy + 132.0f}, 20.0f,
                 th::Rgba(th::kInk), DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
        const std::wstring subtitle = Widen(profile_.display_name());
        ui_.Text(subtitle.c_str(), {cx - 300.0f, cy + 134.0f, cx + 300.0f, cy + 156.0f}, 12.5f,
                 th::Rgba(th::kInkDim), DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);

        // Phases are read from the real state: probe, wake, session, first frame.
        int phase = 3;
        if (pending_connect_ == PendingConnect::Probing) {
            phase = 0;
        } else if (pending_connect_ == PendingConnect::Waking) {
            phase = 1;
        } else if (stream_connecting_) {
            phase = 2;
        }
        const wchar_t* labels[] = {L"Checking the console", L"Waking the console",
                                   L"Opening the session", L"Waiting for the first frame"};
        std::vector<std::pair<std::wstring, int>> steps;
        for (int i = 0; i < 4; ++i) {
            steps.emplace_back(labels[i], i < phase ? 2 : (i == phase ? 1 : 0));
        }

        const float card_w = 520.0f;
        const float card_h = 74.0f + 32.0f * static_cast<float>(steps.size());
        const float card_t = cy + 180.0f;
        const D2D1_RECT_F card{cx - card_w * 0.5f, card_t, cx + card_w * 0.5f, card_t + card_h};
        ui_.FillRounded(card, 18.0f, th::Rgba(th::kPanel, 0.72f));
        ui_.StrokeRounded(card, 18.0f, th::Rgba(th::kAccent, 0.20f), 1.0f);
        ui_.FillRounded({card.left + 28.0f, card.top, card.right - 28.0f, card.top + 2.0f}, 1.0f,
                        th::Rgba(th::kAccent, 0.75f));
        DrawLoaderSteps(steps, card.left + 28.0f, card.top + 18.0f, card_w - 56.0f, ct, 1.0f);

        const float bar_w = card_w - 56.0f;
        const float bx = card.left + 28.0f;
        const float by = card.bottom - 36.0f;
        const float prog = 0.18f + 0.64f * (0.5f + 0.5f * std::sin(ct * 1.7f));
        ui_.FillRounded({bx, by, bx + bar_w, by + 4.0f}, 2.0f, th::Rgba(0xFFFFFF, 0.08f));
        ui_.FillRounded({bx, by, bx + bar_w * prog, by + 4.0f}, 2.0f, th::Rgba(th::kAccent, 0.85f));
        ui_.Text(stream_note_.empty() ? Widen(std::string("Working...")).c_str()
                                      : Widen(stream_note_).c_str(),
                 {bx, by + 10.0f, bx + bar_w, by + 30.0f}, 12.0f, th::Rgba(th::kInkDim, 0.9f));
        return;
    }

    // The DLSS Frame Generation worker covers the client with its own child
    // window. When it is hidden to reveal the chrome, dim the stale parent
    // frame so the HUD and buttons stay readable.
    if (fg_window_) {
        ui_.FillRect({0.0f, 0.0f, w, ui_h_}, th::Rgba(0x05030A, 0.70f));
    }

    if (hud_) {
        const int mask = profile_.hud_mask;

        // Percentiles are stable enough to be recomputed a few times a second;
        // sorting 240 samples on every 239 Hz frame was pure UI overhead.
        if (clock_ - stat_at_ > 0.25 && !frame_ms_.empty()) {
            std::vector<float> sorted(frame_ms_.begin(), frame_ms_.end());
            std::sort(sorted.begin(), sorted.end());
            const auto pick = [&sorted](float q) {
                const size_t index = static_cast<size_t>(q * (sorted.size() - 1));
                return sorted[std::min(index, sorted.size() - 1)];
            };
            stat_p50_ = pick(0.50f);
            stat_p95_ = pick(0.95f);
            stat_p99_ = pick(0.99f);
            stat_max_ = sorted.back();

            const float budget_ms = 1000.0f / static_cast<float>(present_hz());
            float total_ms = 0.0f;
            int late = 0;
            int stalls = 0;
            for (float sample : frame_ms_) {
                total_ms += sample;
                if (sample > budget_ms * 1.5f) {
                    ++late;
                }
                if (sample > budget_ms * 2.5f) {
                    ++stalls;
                }
            }
            stat_fps_ = total_ms > 0.0f
                            ? static_cast<float>(frame_ms_.size()) * 1000.0f / total_ms
                            : 0.0f;
            stat_late_ = late;
            stat_stalls_ = stalls;
            stat_at_ = clock_;
        }

        // Numbers change far slower than the display refreshes. Rebuild the
        // strings and their DirectWrite layouts at 10 Hz, draw the cached
        // layouts every frame.
        if (clock_ - hud_lines_at_ >= 0.10 || hud_lines_mask_ != mask ||
            hud_lines_stream_ != stream_active_) {
            hud_lines_at_ = clock_;
            hud_lines_mask_ = mask;
            hud_lines_stream_ = stream_active_;
            hud_lines_.clear();

            auto add_line = [this](std::wstring text, float alpha) {
                hud_lines_.push_back({std::move(text), alpha});
            };

            if (mask & 0x1) {
                wchar_t header[256];
                swprintf_s(header,
                           L"%.1f fps    frame  p50 %.1f   p95 %.1f   p99 %.1f   max %.1f ms    "
                           L"late %d   stalls %d",
                           stat_fps_, stat_p50_, stat_p95_, stat_p99_, stat_max_, stat_late_,
                           stat_stalls_);
                add_line(header, 0.95f);
            }

            if (mask & 0x2) {
                add_line(Widen(presenter_.Summary()), 0.88f);
            }

            if (mask & 0x4) {
                const PadState hud_pad = pad_.snapshot();
                if (hud_pad.connected) {
                    add_line(PadStatus(hud_pad), 0.88f);
                }
            }

            if ((mask & 0x8) && stream_active_) {
                const double source_fps =
                    stream_stats_.elapsed > 0.0
                        ? static_cast<double>(stream_stats_.frames) / stream_stats_.elapsed
                        : 0.0;
                std::wstring chain;
                if (profile_.neural_nr > 0 && nr_ready_) {
                    wchar_t part[96];
                    swprintf_s(part, L"NR %.1f ms   ", nr_last_ms_);
                    chain += part;
                }
                if (profile_.neural_sr > 0 && dlss_ready_) {
                    wchar_t part[96];
                    swprintf_s(part,
                               profile_.dlss_mode == 5 ? L"DLAA %.1f ms   " : L"DLSS %.1f ms   ",
                               dlss_sr_.last_ms());
                    chain += part;
                }
                if (frame_gen_ >= 2 && fg_ready_) {
                    wchar_t part[96];
                    swprintf_s(part, L"VFG %dx   ", frame_gen_);
                    chain += part;
                } else if (profile_.dlss_fg >= 2 && profile_.fg_engine == 1) {
                    wchar_t part[160];
                    swprintf_s(part, L"RIFE %.0f fps   %.1f ms/frame   ", rife_out_fps_,
                               rife_ms_);
                    chain += part;
                } else if (profile_.dlss_fg >= 2) {
                    wchar_t part[160];
                    swprintf_s(part, L"FG %.0f fps   content %.1f fps   %s   ", fg_worker_fps_,
                               fg_content_hz_,
                               fg_mult_live_ < 2
                                   ? L"pass-through"
                                   : (std::to_wstring(fg_mult_live_) + L"x").c_str());
                    chain += part;
                }
                if (profile_.dlss_fg >= 2 && enh_applied_.active) {
                    wchar_t part[96];
                    swprintf_s(part, L"ENH %.2f ms   ", presenter_.enhance_ms());
                    chain += part;
                }
                wchar_t stream_line[320];
                swprintf_s(stream_line, L"PS5 %.1f fps   %s audio peak %.2f   %s",
                           source_fps, chain.c_str(), audio_.peak(),
                           Widen(streaming_.message()).c_str());
                add_line(stream_line, 0.88f);

                int counts[12] = {};
                const bool have_screen = screen_hold_fill_ > 0;
                const auto& ring = have_screen ? screen_holds_ : video_holds_;
                const int ring_fill = have_screen ? screen_hold_fill_ : video_hold_fill_;
                for (int i = 0; i < ring_fill; ++i) {
                    ++counts[ring[i]];
                }
                const int n = std::max(1, ring_fill);
                const auto pct = [&](int v) { return counts[v] * 100 / n; };
                wchar_t cadence[256];
                swprintf_s(cadence,
                           L"%s 3v %d%%   4v %d%%   5v %d%%   6v+ %d%%   arrival %.1f ms   "
                           L"outliers %d",
                           have_screen ? L"screen" : L"select", pct(3), pct(4), pct(5),
                           counts[6] + counts[7] + counts[8] + counts[9] + counts[10] +
                               counts[11],
                           stream_stats_.arrival_ms, stream_stats_.arrival_outliers);
                add_line(cadence, 0.86f);
            }

            if ((mask & 0x10) && stream_active_) {
                std::wstring decoder = Widen(streaming_.decoder_name());
                if (decoder.empty()) {
                    decoder = L"-";
                }
                wchar_t tail[256];
                swprintf_s(tail,
                           L"decoder %s   buf %d   drops %d   vq %d   lost %d   %.0f kbps",
                           decoder.c_str(), stream_stats_.buffer, stream_stats_.buffer_drops,
                           streaming_.video_queue_drops(), stream_stats_.lost,
                           stream_stats_.kbps);
                add_line(tail, 0.82f);
            }
        }

        const float hud_y = borderless_ ? 24.0f : kTitleBarH + 12.0f;
        const D2D1_RECT_F panel{
            24.0f, hud_y, 24.0f + 700.0f,
            hud_y + 20.0f + static_cast<float>(hud_lines_.size()) * 22.0f};
        ui_.FillRounded(panel, 12.0f, th::Rgba(0x08040F, 0.72f));
        ui_.StrokeRounded(panel, 12.0f, th::Rgba(th::kAccent, 0.28f), 1.0f);

        float row_y = panel.top + 10.0f;
        for (const HudLine& line : hud_lines_) {
            ui_.Text(line.text.c_str(),
                     {panel.left + 14.0f, row_y, panel.right - 14.0f, row_y + 22.0f}, 11.5f,
                     th::Rgba(th::kInk, line.alpha));
            row_y += 22.0f;
        }
    }

    // Chrome fades out after 3 s of mouse inactivity and returns on movement.
    const float idle = static_cast<float>(clock_ - mouse_active_at_);
    const float chrome_alpha =
        stream_active_ ? std::clamp((3.0f - idle) / 0.35f, 0.0f, 1.0f) : 1.0f;

    if (chrome_alpha > 0.01f) {
        const float chip = kCornerButton;

        if (stream_active_) {
            const D2D1_RECT_F quit{w - kCornerMargin - chip, 70.0f, w - kCornerMargin,
                                   70.0f + chip};
            const bool quit_hover = hover_ == Hit::Button && hover_index_ == W_Disconnect;
            ui_.FillRounded(quit, 14.0f,
                            th::Rgba(0xFF6B6B, (quit_hover ? 0.30f : 0.16f) * chrome_alpha));
            ui_.StrokeRounded(quit, 14.0f, th::Rgba(0xFF6B6B, 0.85f * chrome_alpha), 1.0f);
            DrawIcon(ui_, Icon::Power, quit.left + chip * 0.5f, quit.top + chip * 0.5f, 9.0f,
                     th::Rgba(th::kInk, 0.95f * chrome_alpha));
            if (chrome_alpha > 0.5f) {
                AddHit(Hit::Button, W_Disconnect, quit);
            }
        }

    }

    // Double-Esc confirmation toast.
    if (clock_ - last_esc_at_ < 2.0) {
        const float fade =
            std::clamp(static_cast<float>(2.0 - (clock_ - last_esc_at_)) * 2.0f, 0.0f, 1.0f);
        const float pill_w = 340.0f;
        const D2D1_RECT_F pill{w * 0.5f - pill_w * 0.5f, ui_h_ - 104.0f,
                               w * 0.5f + pill_w * 0.5f, ui_h_ - 56.0f};
        ui_.FillRounded(pill, 14.0f, th::Rgba(0x05030A, 0.80f * fade));
        ui_.StrokeRounded(pill, 14.0f, th::Rgba(th::kAccent, 0.65f * fade), 1.0f);
        ui_.Text(L"Press Esc again to disconnect", {pill.left, pill.top, pill.right, pill.bottom},
                 13.0f, th::Rgba(th::kInk, 0.95f * fade), DWRITE_FONT_WEIGHT_SEMI_BOLD,
                 DWRITE_TEXT_ALIGNMENT_CENTER);
    }
}

void App::RenderStreamSettings(double t) {
    (void)t;
    const float w = ui_w_;
    const float h = ui_h_;

    ui_.FillRect({0.0f, 0.0f, w, h}, th::Rgba(0x000000, 0.66f));
    const float sheet_w = std::min(1020.0f, w - 120.0f);
    const float sheet_l = (w - sheet_w) * 0.5f;
    const float sheet_r = sheet_l + sheet_w;
    const D2D1_RECT_F panel{sheet_l, 70.0f, sheet_r, h - 70.0f};
    panel_x_ = sheet_l + 32.0f;
    panel_right_ = sheet_r - 32.0f;
    ui_.FillRounded(panel, 18.0f, th::Rgba(th::kPanel, 0.94f));
    ui_.StrokeRounded(panel, 18.0f, th::Rgba(th::kAccent, 0.18f), 1.0f);
    ui_.FillRounded({panel.left + 32.0f, panel.top, panel.right - 32.0f, panel.top + 2.0f}, 1.0f,
                    th::Rgba(th::kAccent, 0.75f));
    ui_.Text(L"Stream settings", {panel_x_, 84.0f, panel_right_, 120.0f}, 22.0f,
             th::Rgba(th::kInk), DWRITE_FONT_WEIGHT_SEMI_BOLD);

    // The same categories as Settings; every control applies live.
    const wchar_t* tabs[] = {L"Display", L"Upscaling", L"Colour", L"Neural",
                             L"Audio",  L"Controller", L"Performance"};
    const Icon tab_icons[] = {Icon::Display, Icon::Upscaling, Icon::Color, Icon::Neural,
                              Icon::Audio,   Icon::Controller, Icon::Performance};
    float tx = panel_x_;
    for (int i = 0; i < 7; ++i) {
        const D2D1_RECT_F r{tx, 132.0f, tx + 124.0f, 166.0f};
        const bool selected = overlay_tab_ == i;
        if (selected) {
            ui_.FillRounded(r, 10.0f, th::Rgba(th::kAccent, 0.22f));
        }
        DrawIcon(ui_, tab_icons[i], r.left + 16.0f, (r.top + r.bottom) * 0.5f, 7.5f,
                 th::Rgba(selected ? th::kInk : th::kMuted, selected ? 0.95f : 0.85f));
        ui_.Text(tabs[i], {r.left + 29.0f, r.top, r.right - 6.0f, r.bottom}, 11.5f,
                 th::Rgba(selected ? th::kInk : th::kInkDim), DWRITE_FONT_WEIGHT_SEMI_BOLD);
        AddHit(Hit::OverlayTab, i, r);
        tx += 128.0f;
    }

    const float row = 46.0f;
    float y = 184.0f;

    switch (overlay_tab_) {
        case 1: {  // Upscaling
            DrawDropdown(L"Upscaler", UpscalerLabels(),
                         profile_.neural_sr > 0 ? 5 : upscale_, y, W_Upscaler);
            y += row;
            DrawDropdown(L"Upscale target",
                         {L"Window / display", L"1080p", L"1440p", L"2160p"}, render_target_,
                         y, W_RenderTarget);
            y += row;
            DrawDropdown(L"DLSS model preset",
                         {L"K (Transformer 1, stable)", L"L (Transformer 2, best)",
                          L"M (Transformer 2, sharp)"},
                         profile_.dlss_preset == 12 ? 1 : (profile_.dlss_preset == 13 ? 2 : 0),
                         y, W_DlssPreset, DlssAvailable());
            y += row;
            DrawDropdown(L"DLSS mode",
                         {L"Ultra Performance (33%)", L"Performance (50%)",
                          L"Balanced (58%)", L"Quality (66%)", L"Ultra Quality (77%)",
                          L"DLAA (100% - AI anti-aliasing)"},
                         std::clamp(profile_.dlss_mode, 0, 5), y, W_DlssMode,
                         DlssAvailable());
            y += row;
            if (profile_.neural_sr > 0 && dlss_ready_) {
                wchar_t text[224];
                swprintf_s(text, L"DLSS %s active  -  %dx%d -> %dx%d  -  %.1f ms",
                           profile_.dlss_mode == 5 ? L"DLAA" : L"SR", dlss_in_w_,
                           dlss_in_h_, dlss_sr_.out_width(), dlss_sr_.out_height(),
                           dlss_sr_.last_ms());
                ui_.Text(text, {panel_x_, y + 2.0f, panel_right_, y + 26.0f}, 12.0f,
                         th::Rgba(th::kMuted));
                y += 26.0f;
            }
            y += 14.0f;
            {
                const bool available = FrameGenAvailable();
                std::vector<std::wstring> options;
                if (available) {
                    options = {L"Off", L"NVIDIA VFG 2x (120 fps)"};
                } else {
                    options = {L"Off  -  NVIDIA VFX runtime missing"};
                }
                DrawDropdown(L"NVIDIA VFG (legacy)", options, frame_gen_ == 2 ? 1 : 0, y,
                             W_FrameGen, available);
            }
            y += row;
            DrawDropdown(L"DLSS Frame Generation", {L"Off", L"On"},
                         profile_.dlss_fg >= 2 ? 1 : 0, y, W_DlssFg);
            y += row;
            DrawDropdown(L"Interpolation engine",
                         {L"NVIDIA DLSS FG (fast)", L"RIFE 4.25 lite (TensorRT)"},
                         profile_.fg_engine == 1 ? 1 : 0, y, W_FgEngine,
                         profile_.dlss_fg >= 2);
            y += row;
            DrawDropdown(L"RIFE output",
                         {L"60 fps (one frame between each pair)", L"Pass-through"},
                         profile_.rife_output == 0 ? 1 : 0, y, W_RifeOutput,
                         profile_.dlss_fg >= 2 && profile_.fg_engine == 1);
            y += row;
            DrawDropdown(L"Neural rendering (DLSS 5)", {L"Off", L"On"},
                         profile_.neural_nr > 0 ? 1 : 0, y, W_NeuralNr, NrAvailable());
            y += row;
            if (profile_.fg_engine != 1) {
                DrawDropdown(L"Output frame rate",
                             {L"Match display", L"60 fps", L"90 fps", L"120 fps"},
                             profile_.fg_target == 60
                                 ? 1
                                 : (profile_.fg_target == 90
                                        ? 2
                                        : (profile_.fg_target == 120 ? 3 : 0)),
                             y, W_FgTarget, profile_.dlss_fg >= 2);
            }
            y += row;
            break;
        }

        case 2: {  // Colour
            DrawDropdown(L"Colour controls", {L"Off (sharpening only)", L"On"},
                         profile_.color_on > 0 ? 1 : 0, y, W_ColorOn);
            y += row;
            DrawSlider(L"Sharpening", profile_.sharpness, 0, 100, 5, y, W_Sharpness);
            y += row;
            DrawSlider(L"Saturation", profile_.color_saturation, 0, 200, 5, y, W_ColorSaturation);
            y += row;
            DrawSlider(L"Hue (degrees)", profile_.color_hue, -180, 180, 5, y, W_ColorHue, false);
            y += row;
            DrawSlider(L"Contrast", profile_.color_contrast, 50, 150, 1, y, W_ColorContrast);
            y += row;
            DrawSlider(L"Temperature", profile_.color_temperature, -100, 100, 5, y,
                       W_ColorTemperature);
            y += row;
            break;
        }

        case 3: {  // Neural
            DrawDropdown(L"Neural Rendering", {L"Off", L"On"}, profile_.neural_nr > 0 ? 1 : 0,
                         y, W_NeuralNr);
            y += row;
            DrawDropdown(L"Pipeline",
                         {L"Upstream: NR -> SR", L"NVIDIA order: SR -> NR"},
                         std::clamp(profile_.nr_order, 0, 1), y, W_NrOrder);
            y += row;
            DrawSlider(L"Render scale", profile_.nr_scale, 50, 100, 1, y, W_NrScale);
            y += row;
            DrawDropdown(L"Stack passes", {L"1", L"2", L"3", L"4"},
                         std::clamp(profile_.nr_passes - 1, 0, 3), y, W_NrPasses);
            y += row;
            DrawDropdown(L"Style", {L"Default", L"Natural", L"Cinematic"},
                         std::clamp(profile_.nr_style, 0, 2), y, W_NrStyle);
            y += row;
            DrawSlider(L"Strength", profile_.nr_intensity, 0, 200, 5, y, W_NrIntensity);
            y += row;
            DrawSlider(L"Local tone", profile_.nr_tone, 0, 200, 5, y, W_NrTone);
            y += row;
            DrawSlider(L"Structure", profile_.nr_structure, 0, 200, 5, y, W_NrStructure);
            y += row;
            DrawSlider(L"Character / skin", profile_.nr_skin, 0, 200, 5, y, W_NrSkin);
            y += row;
            DrawDropdown(L"Character mask", {L"Off", L"On"}, profile_.nr_mask > 0 ? 1 : 0, y,
                         W_NrMask);
            y += row;
            DrawDropdown(L"UI correction", {L"Off", L"On"}, profile_.nr_ui > 0 ? 1 : 0, y,
                         W_NrUi);
            y += row;

            if (profile_.neural_nr > 0 && nr_ready_) {
                wchar_t text[256];
                swprintf_s(text,
                           L"NR active  -  %dx%d  -  net %dx%d (%d pass)  -  %.1f ms  -  %llu dropped",
                           nr_in_w_, nr_in_h_, nr_.work_width(), nr_.work_height(),
                           profile_.nr_passes, nr_last_ms_,
                           static_cast<unsigned long long>(nr_dropped_));
                if (profile_.neural_sr > 0 && dlss_ready_) {
                    wcscat_s(text, profile_.nr_order == 1 ? L"  -  after DLSS SR"
                                                          : L"  -  before DLSS SR");
                }
                ui_.Text(text, {panel_x_, y + 2.0f, panel_right_, y + 26.0f}, 12.0f,
                         th::Rgba(th::kMuted));
                y += 26.0f;
            } else if (!NrAvailable()) {
                ui_.Text(L"NR runtime missing: native\\nvngx.dll + nvngx_dlssnr.dll",
                         {panel_x_, y + 2.0f, panel_right_, y + 26.0f}, 12.0f,
                         th::Rgba(th::kMuted));
                y += 26.0f;
            }
            break;
        }
        case 4: {  // Audio
            std::vector<std::wstring> devices;
            if (audio_devices_.empty()) {
                devices.push_back(L"System default");
            }
            for (const audio::DeviceInfo& device : audio_devices_) {
                std::wstring name = Widen(device.name);
                if (device.is_default) {
                    name += L"  (default)";
                } else if (device.is_controller) {
                    name += L"  (controller)";
                }
                devices.push_back(name);
            }
            const int selected_device =
                profile_.audio_device_id.empty()
                    ? 0
                    : std::clamp(audio_device_ + 1, 0, static_cast<int>(devices.size()) - 1);
            DrawDropdown(L"Output device", devices, selected_device, y, W_AudioDevice);
            y += row;
            DrawDropdown(L"Buffer", {L"10 ms", L"20 ms", L"30 ms", L"40 ms"},
                         BufferIndex(audio_buffer_ms_), y, W_AudioBuffer);
            y += row;
            DrawButton(L"Play test tone", {panel_x_, y + 6.0f, panel_x_ + 220.0f, y + 46.0f},
                       W_TestTone, false);
            y += row;
            std::wstring info;
            if (audio_.is_open()) {
                wchar_t buffer[256];
                swprintf_s(buffer, L"%s   -   %.0f ms   -   underruns %d",
                           Widen(audio_.format_text()).c_str(), audio_.latency_ms(),
                           audio_.underruns());
                info = buffer;
            } else {
                info = audio_failed_ ? L"Could not open the device (busy or removed)"
                                     : L"Not started";
            }
            ui_.Text(info.c_str(), {panel_x_, y, panel_right_, y + 40.0f}, 12.0f,
                     th::Rgba(th::kMuted));
            y += 26.0f;
            break;
        }
        case 5: {  // Controller
            DrawDropdown(L"Input source", {kInputSourceNames[0], kInputSourceNames[1],
                                           kInputSourceNames[2]},
                         input_source_, y, W_InputSource);
            y += row;
            {
                int rate_index = 2;
                rate_index = PadRateIndex(pad_rate_hz_);
                DrawDropdown(L"Report rate", {L"60 Hz", L"125 Hz", L"250 Hz", L"500 Hz",
                                              L"1000 Hz"},
                             rate_index, y, W_PadRate);
            }
            y += row;
            DrawDropdown(L"Console rumble", {L"On", L"Off"}, console_rumble_ ? 0 : 1, y,
                         W_ConsoleRumble);
            y += row;
            DrawDropdown(L"Haptics intensity",
                         {L"Off", L"Very weak", L"Weak", L"Normal", L"Strong", L"Very strong"},
                         std::clamp(profile_.haptic_intensity, 0, 5), y, W_HapticIntensity);
            y += row;
            DrawDropdown(L"Console triggers", {L"On", L"Off"}, console_triggers_ ? 0 : 1, y,
                         W_ConsoleTriggers);
            y += row;
            DrawDropdown(L"Motion", {L"On", L"Off"}, motion_ ? 0 : 1, y, W_Motion);
            y += row;
            DrawDropdown(L"Lightbar",
                         {kLightbarNames[0], kLightbarNames[1], kLightbarNames[2],
                          kLightbarNames[3], kLightbarNames[4], kLightbarNames[5],
                          kLightbarNames[6], kLightbarNames[7], kLightbarNames[8]},
                         lightbar_index_, y, W_Lightbar);
            y += row;
            DrawDropdown(L"Adaptive trigger",
                         {kTriggerNames[0], kTriggerNames[1], kTriggerNames[2], kTriggerNames[3],
                          kTriggerNames[4], kTriggerNames[5], kTriggerNames[6], kTriggerNames[7]},
                         trigger_effect_, y, W_Trigger);
            y += row;
            DrawDropdown(L"Trigger strength", {L"1", L"2", L"3", L"4", L"5", L"6", L"7", L"8"},
                         trigger_strength_ - 1, y, W_Strength);
            y += row;
            DrawDropdown(L"Player LEDs",
                         {kPlayerLedNames[0], kPlayerLedNames[1], kPlayerLedNames[2],
                          kPlayerLedNames[3], kPlayerLedNames[4], kPlayerLedNames[5],
                          kPlayerLedNames[6]},
                         player_leds_, y, W_PlayerLeds);
            y += row;
            DrawDropdown(L"Mic LED", {kMicLedNames[0], kMicLedNames[1], kMicLedNames[2]},
                         mic_led_, y, W_MicLed);
            y += row;
            DrawButton(L"Rumble test", {panel_x_, y + 4.0f, panel_x_ + 220.0f, y + 40.0f},
                       W_Rumble, false);
            y += row;
            ui_.Text(PadStatus(pad_.snapshot()).c_str(), {panel_x_, y, panel_right_, y + 24.0f},
                     12.0f, th::Rgba(th::kMuted));
            y += 24.0f;
            break;
        }
        case 0:
        default: {  // Display
            DrawDropdown(L"Video decoder (next session)",
                         {L"Auto", L"D3D11VA", L"D3D12VA", L"Vulkan", L"DXVA2",
                          L"CUDA (NVDEC)", L"Software"},
                         std::clamp(profile_.decoder, 0, 6), y, W_Decoder);
            y += row;
            DrawDropdown(L"Chroma upsampling (next session)",
                         {L"Bilinear (fast)", L"Bicubic", L"Lanczos (sharpest)"},
                         std::clamp(profile_.chroma, 0, 2), y, W_Chroma);
            y += row;
            DrawDropdown(L"Latency HUD", {L"Off", L"On"}, hud_ ? 1 : 0, y, W_Hud);
            y += row;
            break;
        }

        case 6: {  // Performance
            DrawDropdown(L"Frame buffer",
                         {L"0 (fixed, lowest)", L"1 (adaptive)", L"2 (adaptive)",
                          L"3 (adaptive max)"},
                         stream_buffer_, y, W_FrameBuffer);
            y += row;
            DrawDropdown(L"NVIDIA Reflex", {L"Off", L"On", L"On + Boost"}, profile_.reflex, y,
                         W_Reflex);
            y += row;
            break;
        }
    }

    y += 20.0f;
    DrawButton(L"Resume", {panel_x_, y + 6.0f, panel_x_ + 200.0f, y + 50.0f},
               W_ResumeOverlay, true);
    DrawButton(L"Disconnect", {panel_x_ + 220.0f, y + 6.0f, panel_x_ + 420.0f, y + 50.0f},
               W_Disconnect, false);
    DrawPopup();
    panel_x_ = 344.0f;
    panel_right_ = 0.0f;
}
