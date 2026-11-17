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
#include <filesystem>
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

// RTX VSR quality levels exposed in the UI (0 = off).
const int kVsrLevels[] = {0, 1, 2, 3, 4, 21, 23};

const std::vector<std::wstring>& VsrLabels() {
    static const std::vector<std::wstring> labels = {
        L"Off",          L"AI Low",   L"AI Medium", L"AI High",
        L"AI Ultra",     L"AI Streaming Medium", L"AI Streaming Ultra"};
    return labels;
}

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
int VsrIndex(int quality) {
    for (int i = 0; i < 7; ++i) {
        if (kVsrLevels[i] == quality) {
            return i;
        }
    }
    return 0;
}

const wchar_t* VsrQualityLabel(int quality) {
    switch (quality) {
        case 1:
            return L"AI Low";
        case 2:
            return L"AI Medium";
        case 3:
            return L"AI High";
        case 4:
            return L"AI Ultra";
        case 21:
            return L"AI Stream M";
        case 23:
            return L"AI Stream U";
        default:
            return L"off";
    }
}

const std::vector<std::wstring>& PercentLabels() {
    static const std::vector<std::wstring> labels = {
        L"0.00", L"0.25", L"0.50", L"0.75", L"1.00",
        L"1.25", L"1.50", L"1.75", L"2.00"};
    return labels;
}

int PercentIndex(int value) {
    return std::clamp(value, 0, 200) / 25;
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
    W_Vsr = 56,
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
    W_PairHome = 42,
    W_NeuralMax = 61,
    W_DlssPreset = 63,
    W_DlssMode = 64,
    W_Reflex = 65,
    W_DlssFg = 79,
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
    W_Legacy = 78,
};

constexpr float kContentX = 344.0f;
constexpr float kRowH = 66.0f;

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

    games_ = {
        {L"Neon Drift", L"Racing  ·  4h played", 0x2E6BFF},
        {L"Starfall", L"Action RPG  ·  12h played", 0xB14BFF},
        {L"Iron Vale", L"Adventure  ·  2h played", 0xFF7A45},
        {L"Deep Signal", L"Horror  ·  6h played", 0x14B8A6},
        {L"Velocity Zero", L"Arcade  ·  1h played", 0xFFC145},
        {L"Hollow Crown", L"Strategy  ·  21h played", 0xEF476F},
        {L"Skyline 9", L"Shooter  ·  9h played", 0x4DA3FF},
        {L"Paper Moons", L"Puzzle  ·  30m played", 0x8AC926},
    };

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

    // Prefer a console found on the LAN over the cached (often WAN) address.
    const auto discovered = session::DiscoverLocal(1400);
    if (!discovered.empty()) {
        profile_.host = discovered.front().address;
        if (profile_.target_id.empty()) profile_.target_id = discovered.front().target_id;
        if (profile_.name.empty()) profile_.name = discovered.front().name;
        std::string save_detail;
        session::Save(profile_, session::DefaultProfilePath(), save_detail);
    }

    audio_devices_ = audio::Engine::Enumerate();
    if (!profile_.audio_device_id.empty()) {
        for (int i = 0; i < static_cast<int>(audio_devices_.size()); ++i) {
            if (audio_devices_[i].id == profile_.audio_device_id) {
                audio_device_ = i;
                break;
            }
        }
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
    render_target_ = std::clamp(profile_.render_target, 0, 3);
    ApplyRenderTarget();
    ApplyReflex();
    if (profile_.dlss_fg >= 2) {
        ApplyDlssFrameGen();
    }

    frame_gen_ = profile_.frame_gen >= 2 ? 2 : 0;
    nv_vfx_present_ = !FindFrameGenRuntime().empty();

    // DLSS SR is the strongest neural path: it replaces VSR and VFG, which
    // both consume the same decoded frame.
    dlss_present_ = !FindDlssRuntime().empty();
    if (profile_.neural_sr > 0) {
        if (dlss_present_ && EnsureDlssSrForTarget()) {
            vsr_quality_ = 0;
            profile_.vsr = 0;
            frame_gen_ = 0;
            profile_.frame_gen = 0;
        } else {
            profile_.neural_sr = 0;
        }
    }

    // RTX VSR and VFG both consume the decoded frame; VSR wins at startup and
    // turns frame generation off, matching the UI exclusivity rule.
    vsr_quality_ = profile_.neural_sr > 0 ? 0 : profile_.vsr;
    bool valid_vsr = false;
    for (int level : kVsrLevels) {
        if (level != 0 && level == vsr_quality_) {
            valid_vsr = true;
        }
    }
    if (!valid_vsr) {
        vsr_quality_ = 0;
    }
    vsr_present_ = !FindVsrRuntime().empty();
    if (vsr_quality_ > 0) {
        if (vsr_present_ && EnsureVsr()) {
            // VSR replaces VFG: keep the persisted profile consistent.
            frame_gen_ = 0;
            profile_.frame_gen = 0;
        } else {
            vsr_quality_ = 0;
            profile_.vsr = 0;
        }
    }
    if (profile_.neural_sr > 0) {
        frame_gen_ = 0;
    }
    if (frame_gen_ >= 2 && (!nv_vfx_present_ || !EnsureFrameGen())) {
        frame_gen_ = 0;
    }
    nr_present_ = !FindNrRuntime().empty();
    if (profile_.neural_nr > 0) {
        if (nr_present_ && EnsureNr()) {
            // NR is an enhancement stage; DLSS SR may run after it. VSR and
            // frame generation still replace the whole chain.
            DisableVsr();
            profile_.vsr = 0;
            frame_gen_ = 0;
            profile_.frame_gen = 0;
        } else {
            profile_.neural_nr = 0;
        }
    }
    if (!ui_.Init(presenter_.device())) {
        return false;
    }
    return ui_.BindSwapchain(presenter_.backbuffer());
}

void App::Resize(int width, int height) {
    if (width <= 0 || height <= 0) {
        return;
    }
    ui_.ReleaseTarget();
    if (!presenter_.Resize(width, height)) {
        return;
    }
    ui_.BindSwapchain(presenter_.backbuffer());
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
            case Hit::Tile:
                if (selected_ == it->index) {
                    screen_ = Screen::Stream;
                }
                selected_ = it->index;
                break;
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
            case Hit::None:
                break;
        }
        return;
    }
    open_dropdown_ = -1;
    pin_focus_ = false;
}

void App::HandleKey(int vk) {
    if (intro_) {
        intro_ = false;
        return;
    }
    if (vk == VK_F12) {
        capture_requested_ = true;
        return;
    }
    if (vk == VK_F11) {
        const bool target = !presenter_.is_fullscreen();
        ui_.ReleaseTarget();
        presenter_.SetFullscreen(target);
        ui_.BindSwapchain(presenter_.backbuffer());
        return;
    }

    switch (screen_) {
        case Screen::Home:
            if (vk == VK_LEFT) {
                selected_ = std::max(0, selected_ - 1);
            } else if (vk == VK_RIGHT) {
                selected_ = std::min(static_cast<int>(games_.size()) - 1, selected_ + 1);
            } else if (vk == VK_RETURN) {
                screen_ = Screen::Stream;
            } else if (vk == 'S' || vk == VK_TAB) {
                screen_ = Screen::Settings;
            } else if (vk == VK_ESCAPE) {
                quit_ = true;
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
                tab_ = static_cast<Tab>((static_cast<int>(tab_) + 1) % 9);
                open_dropdown_ = -1;
            } else if (vk == VK_LEFT) {
                tab_ = static_cast<Tab>((static_cast<int>(tab_) + 8) % 9);
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
                DisconnectStream();
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

    const PadState& pad = pad_.state();
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
    if ((pressed & PadL1) && screen_ == Screen::Home) {
        selected_ = std::max(0, selected_ - 1);
    }
    if ((pressed & PadR1) && screen_ == Screen::Home) {
        selected_ = std::min(static_cast<int>(games_.size()) - 1, selected_ + 1);
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
            if (value > 0 && vsr_quality_ > 0) {
                DisableVsr();
                profile_.vsr = 0;
                stream_note_ = "Frame generation replaced RTX VSR.";
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
        case W_Vsr: {
            const int level = kVsrLevels[std::clamp(index, 0, 6)];
            if (level == 0) {
                DisableVsr();
                profile_.vsr = 0;
                if (stream_active_ && !stream_frame_.rgba.empty()) {
                    presenter_.SetSourceFrame(stream_frame_.rgba.data(), stream_frame_.width,
                                              stream_frame_.height, stream_frame_.pq);
                }
                SaveProfile();
                break;
            }
            if (profile_.neural_nr > 0) {
                profile_.neural_nr = 0;
                DisableNr();
            }
            vsr_quality_ = level;
            if (!EnsureVsr()) {
                vsr_quality_ = 0;
                profile_.vsr = 0;
                stream_note_ = "RTX VSR unavailable: " + std::string("see Settings > Display");
                SaveProfile();
                break;
            }
            if (profile_.neural_sr > 0) {
                profile_.neural_sr = 0;
                dlss_ready_ = false;
                dlss_sr_.Shutdown();
                stream_note_ = "RTX VSR replaced DLSS Super Resolution.";
            }
            if (frame_gen_ >= 2 || profile_.frame_gen >= 2) {
                frame_gen_ = 0;
                profile_.frame_gen = 0;
                fg_queue_.clear();
                fg_last_ = {};
                fg_generated_ = 0;
            }
            profile_.vsr = level;
            stream_note_ = "RTX VSR enabled; it replaces frame generation.";
            SaveProfile();
            break;
        }
        case W_NeuralMax: {
            if (profile_.neural_nr > 0) {
                profile_.neural_nr = 0;
                DisableNr();
            }
            if (profile_.neural_sr > 0) {
                profile_.neural_sr = 0;
                dlss_ready_ = false;
                dlss_sr_.Shutdown();
            }
            vsr_quality_ = 4;  // AI Ultra
            render_target_ = 3;  // 2160p
            profile_.render_target = 3;
            ApplyRenderTarget();
            frame_gen_ = 0;
            profile_.frame_gen = 0;
            fg_queue_.clear();
            fg_last_ = {};
            if (vsr_quality_ > 0 && vsr_ready_) {
                profile_.vsr = 4;
                stream_note_ = "Neural Ultra: RTX VSR AI Ultra at 2160p (max GPU).";
            } else {
                profile_.vsr = 0;
                stream_note_ = "Neural Ultra unavailable; using the regular upscaler.";
            }
            SaveProfile();
            break;
        }
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
        case W_Reflex: {
            profile_.reflex = std::clamp(index, 0, 2);
            ApplyReflex();
            SaveProfile();
            break;
        }
        case W_DlssFg: {
            profile_.dlss_fg = index >= 1 ? 2 : 0;
            if (profile_.dlss_fg >= 2) {
                frame_gen_ = 0;
                profile_.frame_gen = 0;
                fg_queue_.clear();
                fg_last_ = {};
            }
            ApplyDlssFrameGen();
            stream_note_ = profile_.dlss_fg >= 2
                               ? "DLSS Frame Generation 2x enabled (worker presenter)."
                               : "DLSS Frame Generation off.";
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
        case W_Bitrate: {
            static const int kBitrates[] = {20000, 35000, 50000, 75000, 100000};
            profile_.bitrate_kbps = kBitrates[std::clamp(index, 0, 4)];
            std::string save_detail;
            session::Save(profile_, session::DefaultProfilePath(), save_detail);
            break;
        }
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
        default:
            break;
    }
}

void App::OpenAudioDevice() {
    audio_.Close();
    const std::string id =
        audio_devices_.empty() ? std::string() : audio_devices_[audio_device_].id;
    audio_failed_ = !audio_.Open(id, audio_buffer_ms_);
    audio_opened_ = true;
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

    // VSR output follows the render target, so a live target change rebuilds it.
    if (vsr_quality_ > 0) {
        if (!EnsureVsr()) {
            const std::wstring note = vsr_note_;
            DisableVsr();
            profile_.vsr = 0;
            SaveProfile();
            vsr_note_ = note;
        }
    }
    if (profile_.neural_sr > 0) {
        if (!EnsureDlssSrForTarget()) {
            profile_.neural_sr = 0;
            dlss_ready_ = false;
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
}

int App::present_hz() const {
    return (stream_active_ && frame_gen_ >= 2 && fg_ready_) ? 60 * frame_gen_ : 60;
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

std::wstring App::FindVsrRuntime() const {
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
        const fs::path effect = dir / "nvVFXVideoSuperRes.dll";
        const fs::path model = dir / "nvngx_vsr.dll";
        if (fs::exists(effect, ec) && !ec && fs::exists(model, ec) && !ec) {
            return fs::absolute(dir, ec).wstring();
        }
        ec.clear();
    }
    return {};
}

bool App::VsrAvailable() const {
    return vsr_present_ || nv_vsr_.runtime_loaded();
}

bool App::EnsureVsr() {
    std::string detail;
    if (!nv_vsr_.runtime_loaded()) {
        const std::wstring dir = FindVsrRuntime();
        if (dir.empty()) {
            vsr_note_ =
                L"RTX VSR runtime missing: put nvVFXVideoSuperRes.dll + nvngx_vsr.dll in "
                L"third_party\\nvidia-vfx\\runtime";
            return false;
        }
        if (!nv_vsr_.LoadRuntime(dir, detail)) {
            vsr_note_ = L"RTX VSR runtime: " + Widen(detail);
            return false;
        }
    }
    const int width = profile_.width > 0 ? profile_.width : 1920;
    const int height = profile_.height > 0 ? profile_.height : 1080;
    const int out_w = presenter_.render_width();
    const int out_h = presenter_.render_height();
    if (out_w < width || out_h < height) {
        vsr_note_ = L"RTX VSR cannot downscale: pick a 1440p/2160p upscale target";
        return false;
    }
    if (!nv_vsr_.Initialize(presenter_.device(), presenter_.context(), width, height, out_w,
                            out_h, vsr_quality_, 1.0f, detail)) {
        vsr_note_ = L"RTX VSR init: " + Widen(detail);
        return false;
    }
    vsr_ready_ = true;
    vsr_note_ = L"RTX VSR ready";
    return true;
}

void App::DisableVsr() {
    vsr_quality_ = 0;
    vsr_ready_ = false;
    vsr_frame_ = {};
    nv_vsr_.Shutdown();
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
        DisableVsr();
        profile_.vsr = 0;
        frame_gen_ = 0;
        profile_.frame_gen = 0;
        fg_queue_.clear();
        fg_last_ = {};
        if (!EnsureDlssSrForTarget()) {
            profile_.neural_sr = 0;
            dlss_ready_ = false;
            dlss_sr_.Shutdown();
        }
    } else {
        dlss_ready_ = false;
        dlss_in_w_ = 0;
        dlss_in_h_ = 0;
        dlss_sr_.Shutdown();
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
    if (profile_.dlss_fg >= 2) {
        // Frame Generation presents the worker window at the stream size.
        *width = profile_.width;
        *height = profile_.height;
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

bool App::EnsureDlssSrForTarget() {
    if (profile_.dlss_mode == 5) {
        // DLAA is native-resolution AA: there is nothing to upscale in a
        // 1080p stream, so the feature is not created.
        dlss_note_ = L"DLAA needs a native-resolution render; a 1080p stream has none";
        return false;
    }
    int in_w = 0;
    int in_h = 0;
    DlssInputSize(profile_.width > 0 ? profile_.width : 1920,
                  profile_.height > 0 ? profile_.height : 1080, &in_w, &in_h);
    return EnsureDlssSr(in_w, in_h);
}

void App::DetachFgWindow() {
    if (fg_window_) {
        ::ShowWindow(fg_window_, SW_HIDE);
        ::SetParent(fg_window_, nullptr);
        fg_window_ = nullptr;
    }
}

bool App::AttachFgWindow() {
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
    return true;
}

void App::ApplyDlssFrameGen() {
    std::string detail;
    if (profile_.dlss_fg >= 2) {
        // The worker's presenter owns the screen: everything else that also
        // presents must step aside.
        frame_gen_ = 0;
        profile_.frame_gen = 0;
        fg_queue_.clear();
        fg_last_ = {};
        if (vsr_quality_ > 0) {
            DisableVsr();
            vsr_quality_ = 0;
            profile_.vsr = 0;
        }
        if (profile_.neural_sr > 0) {
            profile_.neural_sr = 0;
            dlss_ready_ = false;
            dlss_sr_.Shutdown();
        }
        if (!EnsureNr()) {
            profile_.dlss_fg = 0;
            SaveProfile();
            return;
        }
        nr_.SetFrameGeneration(profile_.dlss_fg);
        nr_.SetNeuralBypass(profile_.neural_nr == 0);
        if (!nr_.present_mode() &&
            !nr_.EnablePresent(profile_.width > 0 ? profile_.width : 1920,
                               profile_.height > 0 ? profile_.height : 1080, detail)) {
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
    if (profile_.dlss_fg >= 2) {
        std::string fg_detail;
        nr_.SetFrameGeneration(profile_.dlss_fg);
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
        DisableVsr();
        profile_.vsr = 0;
        frame_gen_ = 0;
        profile_.frame_gen = 0;
        fg_queue_.clear();
        fg_last_ = {};
        // DLSS SR is not a rival of NR: it can run on the NR result. Keep the
        // selected upscaler and make sure its feature matches this geometry.
        if (profile_.neural_sr > 0 && DlssAvailable() &&
            !EnsureDlssSrForTarget()) {
            profile_.neural_sr = 0;
            dlss_ready_ = false;
            dlss_sr_.Shutdown();
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
        const int hz = std::clamp(pad_rate_hz_, 60, 1000);
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
            if (version != feedback_version_) {
                feedback_version_ = version;
                PadConsoleFeedback feedback = streaming_.feedback();
                if (!console_rumble_) {
                    feedback.rumble = false;
                }
                if (!console_triggers_) {
                    feedback.triggers = false;
                }
                pad_.SetConsoleFeedback(feedback);
            }
        }
        last_send_ok_ = pad_.PumpOutput();

        if (probe) {
            ++probe_loops;
            const auto probe_now = std::chrono::steady_clock::now();
            if (probe_now - probe_at >= std::chrono::seconds(1)) {
                const double seconds =
                    std::chrono::duration<double>(probe_now - probe_at).count();
                std::fprintf(stderr, "[pad] %.0f loops/s (target %d Hz, %s)\n",
                             probe_loops / seconds, hz,
                             pad_.is_bluetooth() ? "bluetooth" : "usb");
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
    std::string detail;
    session::Save(profile_, session::DefaultProfilePath(), detail);
}

void App::ConnectConsole() {
    if (!profile_.has_console() || profile_.needs_pairing() || profile_.host.empty()) {
        StartProbe();
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
            profile_.vsr = 0;
            vsr_quality_ = 0;
            frame_gen_ = 0;
        }
    }

    if (profile_.neural_sr > 0) {
        if (!EnsureDlssSrForTarget()) {
            const std::wstring note = dlss_note_;
            profile_.neural_sr = 0;
            dlss_ready_ = false;
            dlss_sr_.Shutdown();
            SaveProfile();
            dlss_note_ = note;
            stream_note_ = "DLSS SR unavailable; using the regular upscaler.";
            std::fprintf(stderr, "[app] DLSS SR disabled: %ls\n", dlss_note_.c_str());
        }
        frame_gen_ = 0;
    }
    if (vsr_quality_ > 0 && !EnsureVsr()) {
        const std::wstring note = vsr_note_;
        DisableVsr();
        profile_.vsr = 0;
        SaveProfile();
        vsr_note_ = note;
        stream_note_ = "RTX VSR unavailable; using the regular upscaler.";
        std::fprintf(stderr, "[app] RTX VSR disabled: %ls\n", vsr_note_.c_str());
    }
    if (vsr_quality_ > 0) {
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

    std::string error;
    if (streaming_.Start(profile_, profile_.width, profile_.height, profile_.bitrate_kbps, error)) {
        stream_active_ = true;
        StartPadThread();
        mouse_active_at_ = clock_;
        cursor_hidden_ = false;
        window_->SetCursorHidden(false);
        screen_ = Screen::Stream;
        std::fprintf(stderr, "[app] streaming started\n");
    } else {
        stream_note_ = "Could not start the session: " + error;
        std::fprintf(stderr, "[app] start failed: %s\n", error.c_str());
    }
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
void App::DisconnectStream() {
    StopPadThread();
    streaming_.Stop();
    stream_active_ = false;
    pending_connect_ = PendingConnect::None;
    feedback_version_ = 0;
    pad_.SetConsoleFeedback(PadConsoleFeedback{});
    fg_queue_.clear();
    fg_last_ = {};
    cursor_hidden_ = false;
    if (window_) {
        window_->SetCursorHidden(false);
    }
    stream_note_ = "Disconnected. The console may take a few seconds to release the session.";
    screen_ = Screen::Home;
}

void App::StartProbe() {
    probe_.Start(profile_.host, 9295);
}

void App::ApplyControllerOutput(bool force) {
    (void)force;

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
    const float physical_w = static_cast<float>(window_->width());
    const float physical_h = static_cast<float>(window_->height());
    ui_scale_ = std::clamp(physical_h / 900.0f, 1.0f, 2.5f);
    ui_w_ = physical_w / ui_scale_;
    ui_h_ = physical_h / ui_scale_;
    ui_.SetScale(ui_scale_);

    pointer_ = pointer;
    if (ui_scale_ != 1.0f) {
        pointer_.x = static_cast<int>(std::lround(pointer.x / ui_scale_));
        pointer_.y = static_cast<int>(std::lround(pointer.y / ui_scale_));
    }
    clock_ = time_seconds;
    hits_.clear();
    hover_ = Hit::None;
    hover_index_ = -1;

    // Mouse activity drives the stream chrome and the OS cursor.
    if (pointer.x != last_mouse_x_ || pointer.y != last_mouse_y_ || pointer.clicked ||
        pointer.wheel != 0.0f) {
        mouse_active_at_ = clock_;
        last_mouse_x_ = pointer.x;
        last_mouse_y_ = pointer.y;
    }

    // Startup logo animation. Runs before any other UI and is skippable with a
    // click or a key; the window is shown only after its first frame.
    if (intro_) {
        if (intro_start_ < 0.0) {
            intro_start_ = clock_;
        }
        const double elapsed = clock_ - intro_start_;
        const bool skip = pointer_.clicked || elapsed > 3.6;
        if (!skip) {
            RenderIntro(elapsed);
            window_->SetCursorHidden(false);
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

    double dt = last_time_ < 0.0 ? 1.0 / 60.0 : time_seconds - last_time_;
    last_time_ = time_seconds;
    dt = std::clamp(dt, 0.0, 0.1);

    if (!pad_run_.load()) {
        pad_.Poll();
    }
    probe_.Poll();
    TickPendingConnect();
    HandlePad();

    if (window_) {
        const int ch = window_->ConsumeChar();
        if (ch != 0 && pin_focus_) {
            if (ch >= '0' && ch <= '9' && pin_.size() < 8) {
                pin_ += static_cast<char>(ch);
            } else if (ch == 8 && !pin_.empty()) {
                pin_.pop_back();
            }
        }
    }

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

    if (pointer_.wheel != 0.0f && screen_ == Screen::Home) {
        const int steps = static_cast<int>(pointer_.wheel);
        selected_ = std::clamp(selected_ - steps, 0, static_cast<int>(games_.size()) - 1);
    }

    const float target = static_cast<float>(selected_);
    scroll_ += (target - scroll_) * (1.0f - std::exp(-16.0f * static_cast<float>(dt)));

    if (screen_ == Screen::Stream || screen_ == Screen::StreamSettings) {
        // Reflex low-latency sleep, right before the frame is submitted.
        reflex_.Sleep(presenter_.device());
        presenter_.Render(time_seconds);
    } else {
        const float bg[4] = {0.010f, 0.016f, 0.028f, 1.0f};
        presenter_.Clear(bg);
    }

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
    ui_.EndFrame();

    if (window_) {
        window_->set_hand_cursor(hover_ != Hit::None);
    }

    if (pointer_.clicked) {
        DispatchClick();
    }

    if (profile_.dlss_fg >= 2 && nr_.present_mode() && !fg_window_) {
        AttachFgWindow();
    }
    if (fg_window_) {
        ::ShowWindow(fg_window_, screen_ == Screen::Stream ? SW_SHOW : SW_HIDE);
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
        const bool vfg_active = frame_gen_ >= 2 && fg_ready_;
        if (vfg_active) {
            if (fg_queue_.size() <= 2 &&
                streaming_.TakeFrame(stream_frame_, stream_buffer_)) {
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
        } else if (streaming_.TakeFrame(stream_frame_, stream_buffer_)) {
            if (profile_.dlss_fg >= 2 && nr_ready_ && nr_.present_mode()) {
                // DLSS Frame Generation: the worker presents NR + generated
                // frames in its own window; we only feed it.
                SubmitNrFrame(stream_frame_);
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
            const bool sr_wanted = profile_.neural_sr > 0 && profile_.dlss_mode != 5;
            const bool sr_stage = sr_wanted && DlssAvailable();
            const bool nr_on = profile_.neural_nr > 0 && nr_ready_;
            const bool nvidia_order = profile_.nr_order == 1 && sr_stage;

            // Stage 3: DLSS SR, with the mode's input scale. When the mode asks
            // for fewer pixels than the stream has, the frame is downscaled
            // first - that is what the NVIDIA percentage means.
            bool sr_presented = false;
            if (sr_wanted) {
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
                    profile_.neural_sr = 0;
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
                        const bool want_sr =
                            profile_.neural_sr > 0 && profile_.dlss_mode != 5;
                        int in_w = 0;
                        int in_h = 0;
                        DlssInputSize(nr_w, nr_h, &in_w, &in_h);
                        const uint8_t* sr_input = nr_out_.data();
                        if (in_w != nr_w || in_h != nr_h) {
                            sr_scaled_.resize(static_cast<size_t>(in_w) * in_h * 4);
                            media::DownscaleRgba(nr_out_.data(), nr_w, nr_h,
                                                 sr_scaled_.data(), in_w, in_h, scale_tmp_);
                            sr_input = sr_scaled_.data();
                        }
                        if (want_sr && (!dlss_ready_ || dlss_in_w_ != in_w ||
                                        dlss_in_h_ != in_h)) {
                            dlss_ready_ = EnsureDlssSr(in_w, in_h);
                        }
                        std::string sr_detail;
                        const bool sr_ok =
                            want_sr && dlss_ready_ &&
                            dlss_sr_.Process(sr_input, in_w, in_h, dlss_frame_, sr_detail);
                        if (sr_ok) {
                            presenter_.SetSourceView(
                                dlss_frame_.srv.Get(), dlss_sr_.out_width(),
                                dlss_sr_.out_height(), nr_pq);
                            presenter_.SetSourceFinal(true);
                        } else {
                            if (want_sr && dlss_ready_) {
                                dlss_note_ = L"DLSS SR: " + Widen(sr_detail);
                                profile_.neural_sr = 0;
                                dlss_ready_ = false;
                                dlss_sr_.Shutdown();
                                SaveProfile();
                            }
                            presenter_.SetSourceFinal(false);
                            presenter_.SetSourceFrame(nr_out_.data(), nr_w, nr_h, nr_pq);
                        }
                    }
                }
            }

            if (!nr_on && !sr_presented) {
                if (vsr_quality_ > 0 && vsr_ready_) {
                    std::string detail;
                    if (nv_vsr_.Process(stream_frame_.rgba.data(), stream_frame_.width,
                                        stream_frame_.height, vsr_frame_, detail)) {
                        presenter_.SetSourceView(vsr_frame_.srv.Get(), nv_vsr_.out_width(),
                                                 nv_vsr_.out_height(), stream_frame_.pq);
                        presenter_.SetSourceFinal(true);
                    } else {
                        vsr_note_ = L"RTX VSR: " + Widen(detail);
                        DisableVsr();
                        profile_.vsr = 0;
                        presenter_.SetSourceFinal(false);
                        presenter_.SetSourceFrame(stream_frame_.rgba.data(),
                                                  stream_frame_.width, stream_frame_.height,
                                                  stream_frame_.pq);
                    }
                } else {
                    presenter_.SetSourceFinal(false);
                    presenter_.SetSourceFrame(stream_frame_.rgba.data(), stream_frame_.width,
                                              stream_frame_.height, stream_frame_.pq);
                }
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

        if (streaming_.state() == session::StreamState::Failed) {
            const std::string reason = streaming_.message();
            StopPadThread();
            streaming_.Stop();
            stream_active_ = false;
            pad_.SetConsoleFeedback(PadConsoleFeedback{});
            cursor_hidden_ = false;
            window_->SetCursorHidden(false);
            stream_note_ = reason.empty() ? "Session ended." : reason;
            screen_ = Screen::Home;
        }
    }

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

    static const bool vsr_probe = std::getenv("BRP_FG_PROBE") != nullptr;
    static double vsr_probe_at = 0.0;
    if (vsr_probe && stream_active_ && vsr_quality_ > 0 && vsr_ready_ &&
        clock_ - vsr_probe_at >= 1.0) {
        vsr_probe_at = clock_;
        std::fprintf(stderr, "[vsr] quality %d, %dx%d -> %dx%d, last %.2f ms\n", vsr_quality_,
                     stream_frame_.width, stream_frame_.height, nv_vsr_.out_width(),
                     nv_vsr_.out_height(), nv_vsr_.last_ms());
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
                "[app] fps %.1f dec=%s audio=%d vfg=%d vsr=%d | %0.f kbps lost %d | "
                "decode %.2f/%.2f ms | queue %d buf %d drops %d\n",
                interval_fps, streaming_.decoder_name().c_str(), streaming_.audio_frames(),
                frame_gen_, vsr_quality_, last_stats.kbps, last_stats.lost,
                last_stats.decode_ms_avg, last_stats.decode_ms_max, last_stats.queue,
                last_stats.buffer, last_stats.buffer_drops);
        }
    }

    if (pad_.is_sony() &&
        (output_dirty_ || clock_ < rumble_until_ + 0.1 || clock_ - last_output_ > 0.2)) {
        ApplyControllerOutput(false);
    }
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

    presenter_.PresentNow(true);
    if (!window_shown_) {
        window_->Show();
        window_shown_ = true;
    }
}

void App::RenderTopBar() {
    const float w = ui_w_;

    ui_.Text(L"BETTERREMOTEPLAY", {48.0f, 26.0f, 480.0f, 52.0f}, 14.0f, th::Rgba(th::kInk, 0.92f),
             DWRITE_FONT_WEIGHT_SEMI_BOLD);

    const D2D1_RECT_F settings_rect{w - 200.0f, 28.0f, w - 96.0f, 64.0f};
    const bool settings_hover = hover_ == Hit::SettingsButton;
    ui_.FillRounded(settings_rect, 18.0f, th::Rgba(0xFFFFFF, settings_hover ? 0.12f : 0.06f));
    ui_.StrokeRounded(settings_rect, 18.0f, th::Rgba(0xFFFFFF, 0.10f), 1.0f);
    ui_.Text(L"Settings",
             {settings_rect.left, settings_rect.top, settings_rect.right, settings_rect.bottom},
             13.0f, th::Rgba(th::kInk, 0.9f), DWRITE_FONT_WEIGHT_NORMAL,
             DWRITE_TEXT_ALIGNMENT_CENTER);
    AddHit(Hit::SettingsButton, -1, settings_rect);

    SYSTEMTIME st{};
    ::GetLocalTime(&st);
    wchar_t clock_text[16];
    swprintf_s(clock_text, L"%02d:%02d", st.wHour, st.wMinute);
    ui_.Text(clock_text, {w - 340.0f, 28.0f, w - 290.0f, 60.0f}, 20.0f,
             th::Rgba(th::kInk, 0.92f), DWRITE_FONT_WEIGHT_SEMI_BOLD,
             DWRITE_TEXT_ALIGNMENT_TRAILING);

    ui_.FillCircle(w - 60.0f, 44.0f, 18.0f, th::Rgba(0x332846));
    ui_.Text(L"C", {w - 78.0f, 26.0f, w - 42.0f, 62.0f}, 15.0f, th::Rgba(th::kInk),
             DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
}

void App::DrawConsoleCard() {
    const float w = ui_w_;
    const D2D1_RECT_F card{w - 392.0f, 96.0f, w - 40.0f, 96.0f + 156.0f};

    ui_.FillRounded(card, 18.0f, th::Rgba(0xFFFFFF, 0.05f));
    ui_.StrokeRounded(card, 18.0f, th::Rgba(0xFFFFFF, 0.09f), 1.0f);

    ui_.Text(L"CONSOLE", {card.left + 20.0f, card.top + 14.0f, card.right - 20.0f,
                          card.top + 34.0f},
             10.5f, th::Rgba(th::kMuted, 0.9f), DWRITE_FONT_WEIGHT_SEMI_BOLD);

    const std::wstring name =
        profile_.has_console() ? Widen(profile_.display_name()) : std::wstring(L"No console");
    ui_.Text(name.c_str(), {card.left + 20.0f, card.top + 34.0f, card.right - 20.0f,
                            card.top + 68.0f},
             21.0f, th::Rgba(th::kInk), DWRITE_FONT_WEIGHT_SEMI_BOLD);

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

    ui_.FillCircle(card.left + 26.0f, card.top + 88.0f, 4.0f, th::Rgba(dot));
    ui_.Text(status.c_str(), {card.left + 38.0f, card.top + 76.0f, card.right - 160.0f,
                              card.top + 100.0f},
             12.0f, th::Rgba(th::kMuted));

    if (!profile_.host.empty()) {
        ui_.Text(Widen(profile_.host).c_str(),
                 {card.left + 20.0f, card.top + 104.0f, card.right - 160.0f, card.top + 126.0f},
                 11.0f, th::Rgba(th::kMuted, 0.75f));
    }

    const bool checking = probe_.state() == net::ProbeState::Checking;
    DrawButton(checking ? L"Checking" : L"Connect",
               {card.right - 148.0f, card.top + 102.0f, card.right - 16.0f, card.top + 142.0f},
               W_Probe, true);
}

void App::DrawTile(const D2D1_RECT_F& rect, const GameEntry& game, float alpha, bool selected,
                   bool hovered) {
    ui_.FillRounded({rect.left + 4.0f, rect.top + 12.0f, rect.right + 4.0f, rect.bottom + 12.0f},
                    22.0f, th::Rgba(0x000000, 0.40f * alpha));

    const float w = rect.right - rect.left;
    const float h = rect.bottom - rect.top;

    ui_.PushRoundedClip(rect, 22.0f);
    ui_.FillLinearV(rect, th::Rgba(th::Scale(game.accent, 0.62f), alpha),
                    th::Rgba(0x05070D, alpha));

    ui_.FillCircle(rect.left + w * 0.80f, rect.top + h * 0.24f, w * 0.58f,
                   th::Rgba(game.accent, 0.18f * alpha));
    ui_.FillCircle(rect.left + w * 0.16f, rect.top + h * 0.74f, w * 0.42f,
                   th::Rgba(0xFFFFFF, 0.05f * alpha));
    ui_.FillCircle(rect.left + w * 0.52f, rect.top + h * 0.50f, w * 0.10f,
                   th::Rgba(0xFFFFFF, 0.10f * alpha));

    wchar_t index[8];
    swprintf_s(index, L"%02d", static_cast<int>(&game - games_.data()) + 1);
    ui_.Text(index, {rect.left + 22.0f, rect.top + 12.0f, rect.right - 22.0f, rect.top + 66.0f},
             44.0f, th::Rgba(0xFFFFFF, 0.10f * alpha), DWRITE_FONT_WEIGHT_SEMI_BOLD);

    ui_.Text(game.title, {rect.left + 22.0f, rect.bottom - 62.0f, rect.right - 22.0f,
                          rect.bottom - 26.0f},
             16.0f, th::Rgba(th::kInk, alpha), DWRITE_FONT_WEIGHT_SEMI_BOLD);
    ui_.PopClip();

    const float border_alpha = selected ? 0.30f : (hovered ? 0.22f : 0.09f);
    ui_.StrokeRounded(rect, 22.0f, th::Rgba(0xFFFFFF, border_alpha * alpha), 1.0f);

    if (selected) {
        ui_.StrokeRounded({rect.left - 6.0f, rect.top - 6.0f, rect.right + 6.0f, rect.bottom + 6.0f},
                          28.0f, th::Rgba(game.accent, 0.60f), 2.0f);
    }
}

void App::DrawHints(const wchar_t* hints, float alpha) {
    ui_.Text(hints, {0.0f, ui_h_ - 40.0f,
                     ui_w_,
                     ui_h_ - 14.0f},
             12.0f, th::Rgba(th::kMuted, 0.85f * alpha), DWRITE_FONT_WEIGHT_NORMAL,
             DWRITE_TEXT_ALIGNMENT_CENTER);
}

void App::DrawSettingLabel(const wchar_t* label, float y, bool enabled) {
    ui_.Text(label, {kContentX, y, kContentX + 460.0f, y + 56.0f}, 16.0f,
             th::Rgba(enabled ? th::kInk : th::kMuted, enabled ? 0.95f : 0.6f),
             DWRITE_FONT_WEIGHT_NORMAL);
}

void App::DrawInfoRow(const wchar_t* label, const std::wstring& value, float y) {
    const float right = ui_w_ - 64.0f;
    DrawSettingLabel(label, y, true);
    ui_.TextEllipsis(value.c_str(), {right - 420.0f, y, right, y + 56.0f}, 15.0f,
                     th::Rgba(th::kInkDim), DWRITE_FONT_WEIGHT_NORMAL,
                     DWRITE_TEXT_ALIGNMENT_TRAILING);
}

void App::DrawDropdown(const wchar_t* label, const std::vector<std::wstring>& options, int selected,
                       float y, int id, bool enabled) {
    const float right = ui_w_ - 64.0f;
    DrawSettingLabel(label, y, enabled);

    const D2D1_RECT_F control{right - 340.0f, y + 8.0f, right, y + 54.0f};
    const bool hovered = hover_ == Hit::Dropdown && hover_index_ == id;

    ui_.FillRounded(control, 10.0f,
                    th::Rgba(0xFFFFFF, hovered ? 0.10f : (enabled ? 0.06f : 0.03f)));
    ui_.StrokeRounded(control, 10.0f, th::Rgba(0xFFFFFF, 0.10f), 1.0f);

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
    const float right = ui_w_ - 64.0f;
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
    ui_.FillRounded({track_l, cy - 3.0f, track_r, cy + 3.0f}, 3.0f, th::Rgba(0xFFFFFF, 0.10f));
    ui_.FillRounded({track_l, cy - 3.0f, knob, cy + 3.0f}, 3.0f, th::Rgba(th::kAccent, 0.85f));
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
        ui_.FillRounded(rect, 12.0f, th::Rgba(th::kAccent, hovered ? 0.30f : 0.20f));
        ui_.StrokeRounded(rect, 12.0f, th::Rgba(th::kAccent, 0.9f), 1.2f);
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

    ui_.BeginFrame();

    ui_.FillLinearV({0.0f, 0.0f, w, h}, th::Rgba(th::kBgTop), th::Rgba(th::kBgBottom));
    ui_.FillRadial(w * 0.5f, h * 0.5f, w * 0.44f, th::Rgba(th::kAccent, 0.10f),
                   th::Rgba(th::kAccent, 0.0f));

    auto ease = [](float x) {
        x = std::clamp(x, 0.0f, 1.0f);
        return 1.0f - std::pow(1.0f - x, 3.0f);
    };

    // RemotePlay first, then "but" appears under it, then "Better" completes
    // the line to the right; the pair re-centres as it appears.
    const float remote = ease(static_cast<float>((t - 0.05) / 0.75));
    const float but = ease(static_cast<float>((t - 1.00) / 0.60));
    const float better = ease(static_cast<float>((t - 1.70) / 0.55));
    const float outro = static_cast<float>(std::clamp((t - 3.05) / 0.50, 0.0, 1.0));
    const float fade = 1.0f - outro;

    const float size1 = std::clamp(h * 0.064f, 42.0f, 88.0f);
    const float size2 = size1 * 0.62f;
    const float gap = size2 * 0.30f;

    const float w_remote = ui_.TextWidth(L"RemotePlay", size1, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    const float w_better = ui_.TextWidth(L"Better", size2, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    const float w_but = ui_.TextWidth(L"but", size2, DWRITE_FONT_WEIGHT_SEMI_BOLD);

    // Centre the two-line lockup as one block, both horizontally and vertically.
    const float step = size1 * 0.95f;
    const float y1 = h * 0.5f - step * 0.5f + (1.0f - remote) * 10.0f;
    const float y2 = y1 + step + (1.0f - but) * 12.0f;

    ui_.Text(L"RemotePlay",
             {w * 0.5f - w_remote * 0.5f, y1 - size1, w * 0.5f + w_remote * 0.5f, y1 + size1},
             size1, th::Rgba(th::kInk, remote * fade), DWRITE_FONT_WEIGHT_SEMI_BOLD);

    // Width-weighted centring: as each word appears the line stays centred.
    const float wb = w_but * but;
    const float wbe = w_better * better;
    const float total2 = wb + wbe + gap * better;
    float x2 = w * 0.5f - total2 * 0.5f;
    if (but > 0.001f) {
        ui_.Text(L"but", {x2, y2 - size2, x2 + wb, y2 + size2}, size2,
                 th::Rgba(th::kAccent, but * fade), DWRITE_FONT_WEIGHT_SEMI_BOLD);
        x2 += wb + gap * better;
    }
    if (better > 0.001f) {
        ui_.Text(L"Better", {x2, y2 - size2, x2 + wbe, y2 + size2}, size2,
                 th::Rgba(th::kInk, better * fade), DWRITE_FONT_WEIGHT_SEMI_BOLD);
    }

    ui_.EndFrame();
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

    const float card_w = std::min(920.0f, w - 96.0f);
    const float card_h = 340.0f;
    const float x0 = (w - card_w) * 0.5f;
    const float y0 = h * 0.46f - card_h * 0.5f;

    const D2D1_RECT_F card{x0, y0, x0 + card_w, y0 + card_h};
    ui_.FillRounded(card, 22.0f, th::Rgba(0xFFFFFF, 0.05f));
    ui_.StrokeRounded(card, 22.0f, th::Rgba(0xFFFFFF, 0.09f), 1.0f);

    ui_.Text(L"PLAYSTATION 5", {x0 + 36.0f, y0 + 26.0f, x0 + card_w - 36.0f, y0 + 48.0f}, 10.5f,
             th::Rgba(th::kMuted, 0.9f), DWRITE_FONT_WEIGHT_SEMI_BOLD);

    const std::wstring name =
        profile_.has_console() ? Widen(profile_.display_name()) : std::wstring(L"No console");
    ui_.Text(name.c_str(), {x0 + 36.0f, y0 + 54.0f, x0 + card_w - 36.0f, y0 + 116.0f}, 42.0f,
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
    ui_.Text(status.c_str(), {x0 + 56.0f, y0 + 126.0f, x0 + card_w - 36.0f, y0 + 154.0f}, 14.0f,
             th::Rgba(th::kInkDim));

    std::wstring address = profile_.host.empty() ? L"No address" : Widen(profile_.host);
    address += profile_.needs_pairing() ? L"   ·   pairing needed" : L"   ·   paired";
    ui_.Text(address.c_str(), {x0 + 36.0f, y0 + 158.0f, x0 + card_w - 36.0f, y0 + 182.0f}, 12.5f,
             th::Rgba(th::kMuted));

    const float button_y = y0 + 224.0f;
    DrawButton(probe_.state() == net::ProbeState::Checking ? L"Checking..." : L"Connect",
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

    ui_.Text(L"Connect checks the console control port (9295). Pairing uses the 8-digit code "
             L"from Settings > System > Remote Play > Link Device.",
             {x0 + 36.0f, y0 + 288.0f, x0 + card_w - 36.0f, y0 + 316.0f}, 11.5f,
             th::Rgba(th::kMuted, 0.85f));

    if (pad_.state().connected) {
        ui_.FillCircle(52.0f, h - 70.0f, 4.0f, th::Rgba(th::kOnline));
        ui_.Text(Widen(pad_.state().name).c_str(), {64.0f, h - 82.0f, 420.0f, h - 58.0f}, 12.0f,
                 th::Rgba(th::kMuted));
    }

    DrawHints(L"Connect   reachability check      Pair   Settings > Connection      Esc   quit");
}

void App::RenderSettings(double t) {
    (void)t;
    const float w = ui_w_;
    const float h = ui_h_;

    ui_.FillLinearV({0.0f, 0.0f, w, h}, th::Rgba(0x0E0818), th::Rgba(th::kBgBottom));
    ui_.FillRadial(w * 0.5f, -h * 0.1f, w * 0.7f, th::Rgba(0x5A2E9E, 0.28f),
                   th::Rgba(0x5A2E9E, 0.0f));

    const D2D1_RECT_F back{w - 132.0f, 28.0f, w - 40.0f, 64.0f};
    ui_.FillRounded(back, 18.0f, th::Rgba(0xFFFFFF, hover_ == Hit::Back ? 0.12f : 0.06f));
    ui_.StrokeRounded(back, 18.0f, th::Rgba(0xFFFFFF, 0.10f), 1.0f);
    ui_.Text(L"Back", {back.left, back.top, back.right, back.bottom}, 13.0f,
             th::Rgba(th::kInk, 0.9f), DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    AddHit(Hit::Back, -1, back);

    ui_.Text(L"Settings", {48.0f, 44.0f, 420.0f, 96.0f}, 34.0f, th::Rgba(th::kInk),
             DWRITE_FONT_WEIGHT_SEMI_BOLD);
    if (!session_note_.empty()) {
        ui_.Text(Widen(session_note_).c_str(), {48.0f, 98.0f, w - 64.0f, 122.0f}, 12.0f,
                 th::Rgba(th::kMuted));
    }

    const wchar_t* tab_names[] = {L"Display", L"Upscaling", L"Performance", L"Neural", L"Legacy", L"Audio", L"Controller", L"Connection", L"About"};
    for (int i = 0; i < 9; ++i) {
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
        ui_.Text(tab_names[i], {tab.left + 26.0f, tab.top, tab.right - 10.0f, tab.bottom}, 15.0f,
                 th::Rgba(selected ? th::kInk : th::kInkDim));
        AddHit(Hit::Tab, i, tab);
    }

    ui_.FillRect({324.0f, 140.0f, 325.0f, h - 60.0f}, th::Rgba(0xFFFFFF, 0.06f));

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
            ui_.Text(L"Upscaling and neural settings are in their own tabs; frame pacing and generation are under Performance.",
                     {kContentX, y + 2.0f, w - 64.0f, y + 26.0f}, 12.0f, th::Rgba(th::kMuted));
            break;
        }

        case Tab::Performance: {
            DrawDropdown(L"Latency HUD", {L"Off", L"On"}, hud_ ? 1 : 0, y, W_Hud);
            y += kRowH;
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
                DrawDropdown(L"Frame generation", options, selected, y, W_FrameGen, available);
            }
            y += kRowH;
            DrawDropdown(L"DLSS Frame Generation",
                         {L"Off", L"2x (worker presenter)"}, profile_.dlss_fg >= 2 ? 1 : 0, y,
                         W_DlssFg);
            y += kRowH;
            DrawDropdown(L"Frame buffer",
                         {L"0 (fixed, lowest)", L"1 (adaptive)", L"2 (adaptive)",
                          L"3 (adaptive max)"},
                         stream_buffer_, y, W_FrameBuffer);
            y += kRowH;
            DrawDropdown(L"NVIDIA Reflex", {L"Off", L"On", L"On + Boost"}, profile_.reflex, y,
                         W_Reflex);
            y += kRowH;

            if (frame_gen_ >= 2 && fg_ready_) {
                wchar_t text[192];
                swprintf_s(text, L"NVIDIA VFG active  -  60 -> %d fps  -  last %.1f ms",
                           60 * frame_gen_, nv_vfx_.last_generate_ms());
                ui_.Text(text, {kContentX, y + 2.0f, w - 64.0f, y + 28.0f}, 12.0f,
                         th::Rgba(th::kMuted));
            } else if (!FrameGenAvailable()) {
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
                          L"DLAA (100% - no upscale)"},
                         std::clamp(profile_.dlss_mode, 0, 5), y, W_DlssMode,
                         dlss && profile_.neural_sr > 0);
            y += kRowH;
            DrawDropdown(L"Upscale target",
                         {L"Window / display", L"1080p", L"1440p", L"2160p"}, render_target_, y,
                         W_RenderTarget);
            y += kRowH;

            ui_.Text(L"The percentage is the resolution DLSS receives, taken from the output. "
                     L"Below the 1080p stream it downscales first (faster, softer); above it the "
                     L"stream is the limit.",
                     {kContentX, y + 2.0f, w - 64.0f, y + 26.0f}, 11.5f, th::Rgba(th::kMuted));
            y += 30.0f;

            if (profile_.neural_sr > 0 && dlss_ready_) {
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
                         {L"Upstream: NR -> SR", L"NVIDIA order: SR -> FG -> NR"},
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
            } else if (!nr_note_.empty()) {
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

        case Tab::Legacy: {
            ui_.Text(L"Legacy",
                     {kContentX, y, w - 64.0f, y + 30.0f}, 16.0f, th::Rgba(th::kInk),
                     DWRITE_FONT_WEIGHT_SEMI_BOLD);
            y += 36.0f;
            ui_.Text(
                L"Not maintained any more, but still functional. These paths are kept for "
                L"comparison and for machines where DLSS is unavailable.",
                {kContentX, y + 2.0f, w - 64.0f, y + 46.0f}, 12.0f, th::Rgba(th::kMuted));
            y += 50.0f;

            DrawDropdown(L"Neural upscaling (RTX VSR)", VsrLabels(), VsrIndex(vsr_quality_), y,
                         W_Vsr, VsrAvailable());
            y += kRowH;

            if (vsr_quality_ > 0 && vsr_ready_) {
                wchar_t text[192];
                swprintf_s(text, L"RTX VSR %s active  -  %dx%d  -  last %.1f ms",
                           VsrQualityLabel(vsr_quality_), nv_vsr_.out_width(),
                           nv_vsr_.out_height(), nv_vsr_.last_ms());
                ui_.Text(text, {kContentX, y + 2.0f, w - 64.0f, y + 26.0f}, 12.0f,
                         th::Rgba(th::kMuted));
                y += 26.0f;
            } else if (!VsrAvailable()) {
                ui_.Text(L"VSR runtime missing: third_party\\nvidia-vfx\\runtime",
                         {kContentX, y + 2.0f, w - 64.0f, y + 26.0f}, 12.0f,
                         th::Rgba(th::kMuted));
                y += 26.0f;
            } else if (!vsr_note_.empty()) {
                ui_.Text(vsr_note_.c_str(), {kContentX, y + 2.0f, w - 64.0f, y + 26.0f},
                         12.0f, th::Rgba(th::kMuted));
                y += 26.0f;
            }
            y += 10.0f;

            DrawButton(L"Neural Ultra preset (VSR Ultra + 2160p)",
                       {kContentX, y + 6.0f, kContentX + 380.0f, y + 50.0f}, W_NeuralMax, false);
            y += kRowH;
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

            std::wstring status = PadStatus(pad_.state());
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
            break;
        }

        case Tab::Connection: {
            DrawInfoRow(L"Console",
                        profile_.has_console() ? Widen(profile_.display_name())
                                               : std::wstring(L"None"),
                        y);
            y += kRowH;
            DrawInfoRow(L"Address", profile_.host.empty() ? L"Not set" : Widen(profile_.host), y);
            y += kRowH;
            DrawInfoRow(L"Pairing",
                        profile_.needs_pairing() ? L"Needed (8-digit console code)" : L"Paired", y);
            y += kRowH;
            DrawInfoRow(L"PSN account",
                        profile_.psn_account_id.empty() ? L"Not set" : L"Imported", y);
            y += kRowH;
            DrawInfoRow(L"Stream defaults",
                        Widen(std::to_string(profile_.bitrate_kbps / 1000) + " Mbps · " +
                              profile_.codec + (profile_.hdr ? " · HDR" : " · SDR")),
                        y);
            y += kRowH;

            {
                int bitrate_index = 2;
                const int rates[] = {20000, 35000, 50000, 75000, 100000};
                for (int i = 0; i < 5; ++i) {
                    if (rates[i] == profile_.bitrate_kbps) {
                        bitrate_index = i;
                    }
                }
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

        case Tab::About: {
            ui_.Text(L"BetterRemotePlay 0.3", {kContentX, y, w - 64.0f, y + 40.0f}, 20.0f,
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

    DrawHints(L"Click a category, then a dropdown      Tab / Left / Right   switch category"
              L"      Esc   back");
}

void App::RenderStream(double t) {
    (void)t;
    const float w = ui_w_;

    if (hud_) {
        ui_.FillLinearV({0.0f, 0.0f, w, 150.0f}, th::Rgba(0x000000, 0.55f),
                        th::Rgba(0x000000, 0.0f));

        const std::wstring summary = Widen(presenter_.Summary());
        const D2D1_RECT_F chip{24.0f, 24.0f, 24.0f + 620.0f, 24.0f + 46.0f};
        ui_.FillRounded(chip, 10.0f, th::Rgba(0x000000, 0.55f));
        ui_.StrokeRounded(chip, 10.0f, th::Rgba(0xFFFFFF, 0.10f), 1.0f);
        ui_.Text(summary.c_str(), {chip.left + 16.0f, chip.top, chip.right - 16.0f, chip.bottom},
                 12.5f, th::Rgba(th::kInk, 0.92f));

        if (pad_.state().connected) {
            const D2D1_RECT_F pad_chip{24.0f, 78.0f, 24.0f + 420.0f, 78.0f + 36.0f};
            ui_.FillRounded(pad_chip, 10.0f, th::Rgba(0x000000, 0.50f));
            ui_.Text(PadStatus(pad_.state()).c_str(),
                     {pad_chip.left + 14.0f, pad_chip.top, pad_chip.right - 14.0f,
                      pad_chip.bottom},
                     12.0f, th::Rgba(th::kInk, 0.9f));
        }

        if (stream_active_) {
            wchar_t stats[256];
            wchar_t fg_text[64] = L"";
            if (profile_.neural_sr > 0 && dlss_ready_) {
                swprintf_s(fg_text, L"  -  DLSS SR %.1f ms", dlss_sr_.last_ms());
            } else if (vsr_quality_ > 0 && vsr_ready_) {
                swprintf_s(fg_text, L"  -  VSR %s %.1f ms", VsrQualityLabel(vsr_quality_),
                           nv_vsr_.last_ms());
            } else if (frame_gen_ >= 2 && fg_ready_) {
                swprintf_s(fg_text, L"  -  VFG %dx gen %d", frame_gen_, fg_generated_);
            }
            swprintf_s(stats, L"PS5  -  %.1f fps%s  -  audio %d  -  peak %.2f  -  %s",
                       streaming_.fps(), fg_text, streaming_.audio_frames(), audio_.peak(),
                       Widen(streaming_.message()).c_str());
            const D2D1_RECT_F live{24.0f, 132.0f, 24.0f + 620.0f, 132.0f + 34.0f};
            ui_.FillRounded(live, 10.0f, th::Rgba(0x000000, 0.55f));
            ui_.Text(stats, {live.left + 14.0f, live.top, live.right - 14.0f, live.bottom}, 12.0f,
                     th::Rgba(th::kInk, 0.92f));

            const std::wstring decoder = Widen(streaming_.decoder_name());
            if (!decoder.empty()) {
                const std::wstring text = L"Decoder: " + decoder;
                const D2D1_RECT_F dec{24.0f, 174.0f, 24.0f + 300.0f, 174.0f + 30.0f};
                ui_.FillRounded(dec, 9.0f, th::Rgba(0x000000, 0.50f));
                ui_.Text(text.c_str(), {dec.left + 12.0f, dec.top, dec.right - 12.0f, dec.bottom},
                         11.5f, th::Rgba(th::kInk, 0.9f));
            }

            // Live pacing telemetry: a rising buffer means jitter, lost > 0
            // means network/FEC, drops are skipped stale frames.
            wchar_t net[160];
            swprintf_s(net, L"buf %d  -  drops %d  -  lost %d  -  %.0f kbps",
                       stream_stats_.buffer, stream_stats_.buffer_drops, stream_stats_.lost,
                       stream_stats_.kbps);
            const D2D1_RECT_F net_chip{24.0f, 210.0f, 24.0f + 360.0f, 210.0f + 30.0f};
            ui_.FillRounded(net_chip, 9.0f, th::Rgba(0x000000, 0.50f));
            ui_.Text(net, {net_chip.left + 12.0f, net_chip.top, net_chip.right - 12.0f,
                           net_chip.bottom},
                     11.5f, th::Rgba(th::kInk, 0.9f));
        }

        ui_.FillCircle(w - 40.0f, 44.0f, 5.0f, th::Rgba(th::kOnline));
        ui_.Text(L"DEMO", {w - 118.0f, 30.0f, w - 54.0f, 58.0f}, 12.0f,
                 th::Rgba(th::kInk, 0.85f), DWRITE_FONT_WEIGHT_SEMI_BOLD,
                 DWRITE_TEXT_ALIGNMENT_TRAILING);
    }

    // Chrome fades out after 3 s of mouse inactivity and returns on movement.
    const float idle = static_cast<float>(clock_ - mouse_active_at_);
    const float chrome_alpha =
        stream_active_ ? std::clamp((3.0f - idle) / 0.35f, 0.0f, 1.0f) : 1.0f;

    if (chrome_alpha > 0.01f) {
        const D2D1_RECT_F back{w - 132.0f, 78.0f, w - 40.0f, 114.0f};
        ui_.FillRounded(back, 18.0f,
                        th::Rgba(0xFFFFFF, (hover_ == Hit::Back ? 0.16f : 0.08f) * chrome_alpha));
        ui_.Text(L"Back", {back.left, back.top, back.right, back.bottom}, 13.0f,
                 th::Rgba(th::kInk, 0.9f * chrome_alpha), DWRITE_FONT_WEIGHT_NORMAL,
                 DWRITE_TEXT_ALIGNMENT_CENTER);
        if (chrome_alpha > 0.5f) {
            AddHit(Hit::Back, -1, back);
        }

        if (stream_active_) {
            const D2D1_RECT_F quit{w - 300.0f, 78.0f, w - 148.0f, 114.0f};
            const bool quit_hover = hover_ == Hit::Button && hover_index_ == W_Disconnect;
            ui_.FillRounded(quit, 18.0f,
                            th::Rgba(0xFF6B6B, (quit_hover ? 0.30f : 0.18f) * chrome_alpha));
            ui_.StrokeRounded(quit, 18.0f, th::Rgba(0xFF6B6B, 0.85f * chrome_alpha), 1.0f);
            ui_.Text(L"Disconnect", {quit.left, quit.top, quit.right, quit.bottom}, 12.5f,
                     th::Rgba(th::kInk, 0.95f * chrome_alpha), DWRITE_FONT_WEIGHT_SEMI_BOLD,
                     DWRITE_TEXT_ALIGNMENT_CENTER);
            if (chrome_alpha > 0.5f) {
                AddHit(Hit::Button, W_Disconnect, quit);
            }
        }

        DrawHints(L"Tab   settings      Esc   disconnect      H   HUD      1..5   upscaler",
                  chrome_alpha);
    }
}

void App::RenderStreamSettings(double t) {
    (void)t;
    const float w = ui_w_;
    const float h = ui_h_;

    ui_.FillRect({0.0f, 0.0f, w, h}, th::Rgba(0x000000, 0.66f));
    const D2D1_RECT_F panel{300.0f, 70.0f, w - 40.0f, h - 70.0f};
    ui_.FillRounded(panel, 18.0f, th::Rgba(0x120A20, 0.96f));
    ui_.StrokeRounded(panel, 18.0f, th::Rgba(0xFFFFFF, 0.10f), 1.0f);
    ui_.Text(L"Stream settings", {kContentX, 84.0f, w - 64.0f, 120.0f}, 22.0f,
             th::Rgba(th::kInk), DWRITE_FONT_WEIGHT_SEMI_BOLD);

    // Categories, like the settings screen: one strip, one page at a time.
    const wchar_t* tabs[] = {L"Stream", L"DLSS", L"Neural", L"Legacy"};
    float tx = kContentX;
    for (int i = 0; i < 4; ++i) {
        const D2D1_RECT_F r{tx, 132.0f, tx + 118.0f, 166.0f};
        const bool selected = overlay_tab_ == i;
        if (selected) {
            ui_.FillRounded(r, 10.0f, th::Rgba(th::kAccent, 0.22f));
        }
        ui_.Text(tabs[i], {r.left, r.top, r.right, r.bottom}, 13.0f,
                 th::Rgba(selected ? th::kInk : th::kInkDim), DWRITE_FONT_WEIGHT_SEMI_BOLD,
                 DWRITE_TEXT_ALIGNMENT_CENTER);
        AddHit(Hit::OverlayTab, i, r);
        tx += 126.0f;
    }

    const float row = 48.0f;
    float y = 184.0f;

    switch (overlay_tab_) {
        case 1: {  // DLSS
            DrawDropdown(L"DLSS model preset",
                         {L"K (Transformer 1, stable)", L"L (Transformer 2, best)",
                          L"M (Transformer 2, sharp)"},
                         profile_.dlss_preset == 12 ? 1 : (profile_.dlss_preset == 13 ? 2 : 0),
                         y, W_DlssPreset, DlssAvailable() && profile_.neural_sr > 0);
            y += row;
            DrawDropdown(L"DLSS mode",
                         {L"Ultra Performance (33%)", L"Performance (50%)",
                          L"Balanced (58%)", L"Quality (66%)", L"Ultra Quality (77%)",
                          L"DLAA (100% - no upscale)"},
                         std::clamp(profile_.dlss_mode, 0, 5), y, W_DlssMode,
                         DlssAvailable() && profile_.neural_sr > 0);
            y += row;
            ui_.Text(L"Percentage of the output resolution fed to DLSS; the 1080p stream is "
                     L"the upper limit.",
                     {kContentX, y + 2.0f, w - 64.0f, y + 26.0f}, 11.5f, th::Rgba(th::kMuted));
            y += 28.0f;
            if (profile_.neural_sr > 0 && dlss_ready_) {
                wchar_t text[224];
                swprintf_s(text, L"DLSS SR active  -  %dx%d -> %dx%d  -  %.1f ms",
                           dlss_in_w_, dlss_in_h_, dlss_sr_.out_width(),
                           dlss_sr_.out_height(), dlss_sr_.last_ms());
                ui_.Text(text, {kContentX, y + 2.0f, w - 64.0f, y + 26.0f}, 12.0f,
                         th::Rgba(th::kMuted));
                y += 26.0f;
            } else if (!dlss_note_.empty()) {
                ui_.Text(dlss_note_.c_str(), {kContentX, y + 2.0f, w - 64.0f, y + 26.0f},
                         12.0f, th::Rgba(th::kMuted));
                y += 26.0f;
            }
            break;
        }
        case 2: {  // Neural
            DrawDropdown(L"Neural Rendering", {L"Off", L"On"}, profile_.neural_nr > 0 ? 1 : 0,
                         y, W_NeuralNr);
            y += row;
            DrawDropdown(L"Pipeline",
                         {L"Upstream: NR -> SR", L"NVIDIA order: SR -> FG -> NR"},
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
                ui_.Text(text, {kContentX, y + 2.0f, w - 64.0f, y + 26.0f}, 12.0f,
                         th::Rgba(th::kMuted));
                y += 26.0f;
            } else if (!NrAvailable()) {
                ui_.Text(L"NR runtime missing: native\\nvngx.dll + nvngx_dlssnr.dll",
                         {kContentX, y + 2.0f, w - 64.0f, y + 26.0f}, 12.0f,
                         th::Rgba(th::kMuted));
                y += 26.0f;
            }
            break;
        }
        case 3: {  // Legacy
            ui_.Text(L"Not maintained, still functional: RTX VSR and the VSR preset.",
                     {kContentX, y + 2.0f, w - 64.0f, y + 40.0f}, 12.0f, th::Rgba(th::kMuted));
            y += 44.0f;
            DrawDropdown(L"Neural upscaling (RTX VSR)", VsrLabels(), VsrIndex(vsr_quality_), y,
                         W_Vsr, VsrAvailable());
            y += row;
            DrawButton(L"Neural Ultra preset (VSR Ultra + 2160p)",
                       {kContentX, y + 6.0f, kContentX + 380.0f, y + 50.0f}, W_NeuralMax, false);
            y += row;
            break;
        }
        default: {  // Stream
            DrawDropdown(L"Upscaler", UpscalerLabels(),
                         profile_.neural_sr > 0 ? 5 : upscale_, y, W_Upscaler);
            y += row;
            DrawDropdown(L"Upscale target",
                         {L"Window / display", L"1080p", L"1440p", L"2160p"}, render_target_,
                         y, W_RenderTarget);
            y += row;
            {
                const bool available = FrameGenAvailable();
                std::vector<std::wstring> options;
                if (available) {
                    options = {L"Off", L"NVIDIA VFG 2x (120 fps)"};
                } else {
                    options = {L"Off  -  NVIDIA VFX runtime missing"};
                }
                DrawDropdown(L"Frame generation", options, frame_gen_ == 2 ? 1 : 0, y,
                             W_FrameGen, available);
            }
            y += row;
            DrawDropdown(L"DLSS Frame Generation", {L"Off", L"2x (worker presenter)"},
                         profile_.dlss_fg >= 2 ? 1 : 0, y, W_DlssFg);
            y += row;
            DrawDropdown(L"Latency HUD", {L"Off", L"On"}, hud_ ? 1 : 0, y, W_Hud);
            y += row;
            DrawDropdown(L"Frame buffer",
                         {L"0 (fixed, lowest)", L"1 (adaptive)", L"2 (adaptive)",
                          L"3 (adaptive max)"},
                         stream_buffer_, y, W_FrameBuffer);
            y += row;
            DrawDropdown(L"NVIDIA Reflex", {L"Off", L"On", L"On + Boost"}, profile_.reflex, y,
                         W_Reflex);
            y += row;
            DrawDropdown(L"Video decoder (next session)",
                         {L"Auto", L"D3D11VA", L"D3D12VA", L"Vulkan", L"DXVA2",
                          L"CUDA (NVDEC)", L"Software"},
                         std::clamp(profile_.decoder, 0, 6), y, W_Decoder);
            y += row;
            break;
        }
    }

    DrawButton(L"Resume", {kContentX, y + 8.0f, kContentX + 200.0f, y + 52.0f},
               W_ResumeOverlay, true);
    DrawButton(L"Disconnect", {kContentX + 220.0f, y + 8.0f, kContentX + 420.0f, y + 52.0f},
               W_Disconnect, false);
    DrawHints(L"Tab / Esc   resume      categories above      sliders drag", 1.0f);
    DrawPopup();
}