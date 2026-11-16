#include "app/app.h"

#include "platform/win32_window.h"
#include "ui/theme.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

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

constexpr int kSettingCount = 9;
constexpr int kControllerCount = 7;
constexpr int kAudioCount = 4;

const wchar_t* kUpscalerNames[3] = {L"Bilinear", L"Catmull-Rom", L"Catmull-Rom + Sharpen"};

struct Rgb {
    uint8_t r, g, b;
};

const Rgb kLightbarColors[] = {
    {0x5A, 0xA9, 0xFF}, {0xFF, 0x4D, 0x4D}, {0x57, 0xD6, 0x8A}, {0xFF, 0xC1, 0x45},
    {0xB1, 0x4B, 0xFF}, {0x14, 0xB8, 0xA6}, {0xF4, 0xF7, 0xFC}, {0x00, 0x00, 0x00},
};
constexpr int kLightbarCount = static_cast<int>(std::size(kLightbarColors));

const wchar_t* kTriggerNames[] = {L"Off",   L"Rigid",  L"Pulse", L"Feedback",
                                  L"Weapon", L"Vibration", L"Bow",   L"Galloping"};
constexpr int kTriggerCount = static_cast<int>(std::size(kTriggerNames));

const wchar_t* kPlayerLedNames[] = {L"Off", L"Center", L"Inner", L"Outer", L"All", L"Mid"};
const uint8_t kPlayerLedMasks[] = {0x00, 0x04, 0x0A, 0x0F, 0x1F, 0x15};
constexpr int kPlayerLedCount = static_cast<int>(std::size(kPlayerLedMasks));

const wchar_t* kMicLedNames[] = {L"Off", L"On", L"Pulse"};

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

struct ButtonLabel {
    uint32_t mask;
    const wchar_t* label;
};

const ButtonLabel kButtonLabels[] = {
    {PadUp, L"Up"},       {PadDown, L"Down"},     {PadLeft, L"Left"},   {PadRight, L"Right"},
    {PadCross, L"Cross"}, {PadCircle, L"Circle"}, {PadSquare, L"Square"},
    {PadTriangle, L"Triangle"}, {PadL1, L"L1"},   {PadR1, L"R1"},       {PadL2, L"L2"},
    {PadR2, L"R2"},       {PadL3, L"L3"},         {PadR3, L"R3"},       {PadShare, L"Create"},
    {PadOptions, L"Options"}, {PadPS, L"PS"},     {PadTouchpad, L"Touchpad"}, {PadMute, L"Mute"},
};

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
            "Imported console identity and PSN account from PXPlay (its registration keys are "
            "encrypted). Pairing needs the 8-digit code from the console.";
    } else {
        session_note_ = "No console configured yet.";
    }

    audio_devices_ = audio::Engine::Enumerate();

    PresenterConfig cfg{};
    cfg.output_width = window->width();
    cfg.output_height = window->height();
    cfg.source_width = 1920;
    cfg.source_height = 1080;
    cfg.vsync = true;
    cfg.allow_tearing = true;
    cfg.upscale_mode = upscale_;

    if (!presenter_.Init(window->hwnd(), cfg)) {
        return false;
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
            case Hit::ControllerButton:
                screen_ = Screen::Controller;
                break;
            case Hit::Back:
                screen_ = Screen::Home;
                break;
            case Hit::SettingRow:
                setting_row_ = it->index;
                if (it->index == 7) {
                    screen_ = Screen::Audio;
                    audio_row_ = 0;
                    if (!audio_opened_) {
                        OpenAudioDevice();
                    }
                } else if (it->index == 8) {
                    screen_ = Screen::Controller;
                    controller_row_ = 0;
                }
                break;
            case Hit::SettingLeft:
                setting_row_ = it->index;
                ChangeSetting(-1);
                break;
            case Hit::SettingRight:
                setting_row_ = it->index;
                ChangeSetting(+1);
                break;
            case Hit::ControllerRow:
                controller_row_ = it->index;
                if (it->index == 6) {
                    screen_ = Screen::Home;
                }
                break;
            case Hit::ControllerLeft:
                controller_row_ = it->index;
                ChangeControllerValue(-1);
                break;
            case Hit::ControllerRight:
                controller_row_ = it->index;
                ChangeControllerValue(+1);
                break;
            case Hit::ControllerAction:
                controller_row_ = it->index;
                if (it->index == 3) {
                    TriggerRumble();
                }
                break;
            case Hit::AudioRow:
                audio_row_ = it->index;
                if (it->index == 3) {
                    screen_ = Screen::Home;
                }
                break;
            case Hit::AudioLeft:
                audio_row_ = it->index;
                ChangeAudioValue(-1);
                break;
            case Hit::AudioRight:
                audio_row_ = it->index;
                ChangeAudioValue(+1);
                break;
            case Hit::AudioAction:
                audio_row_ = it->index;
                if (it->index == 2) {
                    audio_.PlayTestTone(0.6);
                }
                break;
            case Hit::None:
                break;
        }
        return;
    }
}

void App::HandleKey(int vk) {
    if (vk == VK_F12) {
        capture_requested_ = true;
        return;
    }

    switch (screen_) {
        case Screen::Home:
            if (vk == VK_LEFT || vk == 'A') {
                selected_ = std::max(0, selected_ - 1);
            } else if (vk == VK_RIGHT || vk == 'D') {
                selected_ = std::min(static_cast<int>(games_.size()) - 1, selected_ + 1);
            } else if (vk == VK_RETURN) {
                screen_ = Screen::Stream;
            } else if (vk == 'S' || vk == VK_TAB) {
                screen_ = Screen::Settings;
                setting_row_ = 0;
            } else if (vk == 'C') {
                screen_ = Screen::Controller;
                controller_row_ = 0;
            } else if (vk == 'M') {
                screen_ = Screen::Audio;
                audio_row_ = 0;
                if (!audio_opened_) {
                    OpenAudioDevice();
                }
            } else if (vk == VK_ESCAPE) {
                quit_ = true;
            }
            break;

        case Screen::Settings:
            if (vk == VK_UP) {
                setting_row_ = std::max(0, setting_row_ - 1);
            } else if (vk == VK_DOWN) {
                setting_row_ = std::min(kSettingCount - 1, setting_row_ + 1);
            } else if (vk == VK_LEFT) {
                ChangeSetting(-1);
            } else if (vk == VK_RIGHT) {
                ChangeSetting(+1);
            } else if (vk == VK_RETURN) {
                if (setting_row_ == 7) {
                    screen_ = Screen::Audio;
                    audio_row_ = 0;
                    if (!audio_opened_) {
                        OpenAudioDevice();
                    }
                } else if (setting_row_ == 8) {
                    screen_ = Screen::Controller;
                    controller_row_ = 0;
                }
            } else if (vk == VK_ESCAPE || vk == 'S') {
                screen_ = Screen::Home;
            }
            break;

        case Screen::Controller:
            if (vk == VK_UP) {
                controller_row_ = std::max(0, controller_row_ - 1);
            } else if (vk == VK_DOWN) {
                controller_row_ = std::min(kControllerCount - 1, controller_row_ + 1);
            } else if (vk == VK_LEFT) {
                ChangeControllerValue(-1);
            } else if (vk == VK_RIGHT) {
                ChangeControllerValue(+1);
            } else if (vk == VK_RETURN) {
                if (controller_row_ == 3) {
                    TriggerRumble();
                }
            } else if (vk == VK_ESCAPE || vk == 'C') {
                screen_ = Screen::Home;
            }
            break;

        case Screen::Stream:
            if (vk == VK_ESCAPE) {
                screen_ = Screen::Home;
            } else if (vk == 'H') {
                hud_ = !hud_;
            } else if (vk >= '1' && vk <= '3') {
                upscale_ = vk - '1';
                presenter_.SetUpscaleMode(upscale_);
            }
            break;

        case Screen::Audio:
            if (vk == VK_UP) {
                audio_row_ = std::max(0, audio_row_ - 1);
            } else if (vk == VK_DOWN) {
                audio_row_ = std::min(kAudioCount - 1, audio_row_ + 1);
            } else if (vk == VK_LEFT) {
                ChangeAudioValue(-1);
            } else if (vk == VK_RIGHT) {
                ChangeAudioValue(+1);
            } else if (vk == VK_RETURN) {
                if (audio_row_ == 2) {
                    audio_.PlayTestTone(0.6);
                } else if (audio_row_ == 3) {
                    screen_ = Screen::Home;
                }
            } else if (vk == VK_ESCAPE || vk == 'M') {
                screen_ = Screen::Home;
            }
            break;
    }
}

void App::HandlePad() {
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
        if (screen_ == Screen::Home || screen_ == Screen::Settings) {
            HandleKey(VK_RETURN);
        } else if (screen_ == Screen::Controller) {
            TriggerRumble();
        }
    }
    if ((pressed & PadCircle) && screen_ != Screen::Home) {
        HandleKey(VK_ESCAPE);
    }
    if (pressed & PadOptions) {
        if (screen_ == Screen::Home) {
            screen_ = Screen::Settings;
            setting_row_ = 0;
        } else if (screen_ != Screen::Stream) {
            screen_ = Screen::Home;
        }
    }
    if ((pressed & PadL1) && screen_ == Screen::Home) {
        HandleKey(VK_LEFT);
    }
    if ((pressed & PadR1) && screen_ == Screen::Home) {
        HandleKey(VK_RIGHT);
    }
}

void App::ChangeSetting(int direction) {
    if (setting_row_ == 0) {
        upscale_ = (upscale_ + direction + 3) % 3;
        presenter_.SetUpscaleMode(upscale_);
    } else if (setting_row_ == 1) {
        if (direction != 0) {
            hud_ = !hud_;
        }
    } else if (setting_row_ == 7) {
        if (direction != 0) {
            screen_ = Screen::Audio;
            audio_row_ = 0;
            if (!audio_opened_) {
                OpenAudioDevice();
            }
        }
    } else if (setting_row_ == 8) {
        if (direction != 0) {
            screen_ = Screen::Controller;
            controller_row_ = 0;
        }
    }
}

void App::OpenAudioDevice() {
    audio_.Close();
    const std::string id =
        audio_devices_.empty() ? std::string() : audio_devices_[audio_device_].id;
    audio_failed_ = !audio_.Open(id, audio_buffer_ms_);
    audio_opened_ = true;
}

void App::ChangeAudioValue(int direction) {
    if (direction == 0) {
        return;
    }

    switch (audio_row_) {
        case 0:
            if (!audio_devices_.empty()) {
                const int count = static_cast<int>(audio_devices_.size());
                audio_device_ = (audio_device_ + direction + count) % count;
                OpenAudioDevice();
            }
            break;
        case 1: {
            const int sizes[] = {10, 20, 30, 40};
            int index = 0;
            for (int i = 0; i < 4; ++i) {
                if (sizes[i] == audio_buffer_ms_) {
                    index = i;
                }
            }
            audio_buffer_ms_ = sizes[(index + direction + 4) % 4];
            OpenAudioDevice();
            break;
        }
        case 2:
            audio_.PlayTestTone(0.6);
            break;
        default:
            break;
    }
}

void App::ChangeControllerValue(int direction) {
    switch (controller_row_) {
        case 0:
            lightbar_index_ = (lightbar_index_ + direction + kLightbarCount) % kLightbarCount;
            break;
        case 1:
            trigger_effect_ = (trigger_effect_ + direction + kTriggerCount) % kTriggerCount;
            break;
        case 2:
            trigger_strength_ = std::clamp(trigger_strength_ + direction, 1, 8);
            break;
        case 3:
            if (direction != 0) {
                TriggerRumble();
            }
            break;
        case 4:
            player_leds_ = (player_leds_ + direction + kPlayerLedCount) % kPlayerLedCount;
            break;
        case 5:
            mic_led_ = (mic_led_ + direction + 3) % 3;
            break;
        default:
            break;
    }

    if (controller_row_ != 3) {
        output_dirty_ = true;
    }
}

void App::TriggerRumble() {
    rumble_until_ = clock_ + 0.45;
    output_dirty_ = true;
}

void App::ApplyControllerOutput(bool force) {
    if (!pad_.is_sony()) {
        return;
    }

    PadOutput out;
    const Rgb& color = kLightbarColors[lightbar_index_];
    out.lightbar = true;
    out.r = color.r;
    out.g = color.g;
    out.b = color.b;

    out.player_leds = true;
    out.player_mask = kPlayerLedMasks[player_leds_];

    out.mic_led = true;
    out.mic_state = static_cast<uint8_t>(mic_led_);

    out.rumble = true;
    if (clock_ < rumble_until_) {
        out.weak = 200;
        out.strong = 240;
    }

    if (trigger_effect_ > 0) {
        out.left_trigger = true;
        out.right_trigger = true;
        out.left = BuildTriggerEffect(trigger_effect_, trigger_strength_);
        out.right = out.left;
    }

    last_send_ok_ = pad_.SendOutput(out);
    last_output_ = clock_;
    output_dirty_ = false;
    (void)force;
}

void App::Frame(double time_seconds, const PointerState& pointer) {
    pointer_ = pointer;
    clock_ = time_seconds;
    hits_.clear();
    hover_ = Hit::None;
    hover_index_ = -1;

    double dt = last_time_ < 0.0 ? 1.0 / 60.0 : time_seconds - last_time_;
    last_time_ = time_seconds;
    dt = std::clamp(dt, 0.0, 0.1);

    pad_.Poll();
    HandlePad();

    if (pointer_.wheel != 0.0f) {
        const int steps = static_cast<int>(pointer_.wheel);
        if (screen_ == Screen::Home) {
            selected_ = std::clamp(selected_ - steps, 0, static_cast<int>(games_.size()) - 1);
        } else if (screen_ == Screen::Settings) {
            setting_row_ = std::clamp(setting_row_ - steps, 0, kSettingCount - 1);
        } else if (screen_ == Screen::Controller) {
            controller_row_ = std::clamp(controller_row_ - steps, 0, kControllerCount - 1);
        }
    }

    const float target = static_cast<float>(selected_);
    scroll_ += (target - scroll_) * (1.0f - std::exp(-16.0f * static_cast<float>(dt)));

    if (screen_ == Screen::Stream) {
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
        case Screen::Controller:
            RenderController(time_seconds);
            break;
        case Screen::Audio:
            RenderAudio(time_seconds);
            break;
    }
    ui_.EndFrame();

    if (window_) {
        window_->set_hand_cursor(hover_ != Hit::None);
    }

    if (pointer_.clicked) {
        DispatchClick();
    }

    if (pad_.is_sony() &&
        (output_dirty_ || clock_ < rumble_until_ + 0.1 || clock_ - last_output_ > 0.2)) {
        ApplyControllerOutput(false);
    }

    if (capture_requested_) {
        capture_requested_ = false;
        const char* path = "app-home.bmp";
        if (screen_ == Screen::Settings) {
            path = "app-settings.bmp";
        } else if (screen_ == Screen::Stream) {
            path = "app-stream.bmp";
        } else if (screen_ == Screen::Controller) {
            path = "app-controller.bmp";
        } else if (screen_ == Screen::Audio) {
            path = "app-audio.bmp";
        }
        if (presenter_.CaptureToBmp(path)) {
            std::printf("captured %s\n", path);
        } else {
            std::fprintf(stderr, "capture failed: %s\n", path);
        }
    }

    presenter_.PresentNow(true);
}

void App::RenderTopBar() {
    const float w = static_cast<float>(window_->width());

    ui_.Text(L"BETTERREMOTEPLAY", {48.0f, 26.0f, 480.0f, 52.0f}, 14.0f, th::Rgba(th::kInk, 0.92f),
             DWRITE_FONT_WEIGHT_SEMI_BOLD);
    ui_.Text(L"DEMO LIBRARY", {48.0f, 52.0f, 480.0f, 72.0f}, 11.0f, th::Rgba(th::kMuted, 0.9f));

    const bool ready = profile_.has_console() && !profile_.needs_pairing();
    const std::wstring status = Widen(profile_.status_line());
    ui_.FillCircle(52.0f, 88.0f, 4.0f, th::Rgba(ready ? th::kOnline : th::kOffline));
    ui_.Text(status.c_str(), {64.0f, 76.0f, 520.0f, 100.0f}, 12.0f, th::Rgba(th::kMuted));

    // Settings button
    const D2D1_RECT_F settings_rect{w - 372.0f, 28.0f, w - 232.0f, 64.0f};
    const bool settings_hover = hover_ == Hit::SettingsButton;
    ui_.FillRounded(settings_rect, 18.0f,
                    th::Rgba(0xFFFFFF, settings_hover ? 0.12f : 0.06f));
    ui_.StrokeRounded(settings_rect, 18.0f, th::Rgba(0xFFFFFF, 0.10f), 1.0f);
    ui_.Text(L"Settings", {settings_rect.left, settings_rect.top, settings_rect.right,
                           settings_rect.bottom},
             13.0f, th::Rgba(th::kInk, 0.9f), DWRITE_FONT_WEIGHT_NORMAL,
             DWRITE_TEXT_ALIGNMENT_CENTER);
    AddHit(Hit::SettingsButton, -1, settings_rect);

    SYSTEMTIME st{};
    ::GetLocalTime(&st);
    wchar_t clock_text[16];
    swprintf_s(clock_text, L"%02d:%02d", st.wHour, st.wMinute);
    ui_.Text(clock_text, {w - 200.0f, 28.0f, w - 140.0f, 60.0f}, 20.0f,
             th::Rgba(th::kInk, 0.92f), DWRITE_FONT_WEIGHT_SEMI_BOLD,
             DWRITE_TEXT_ALIGNMENT_TRAILING);

    ui_.FillCircle(w - 72.0f, 44.0f, 18.0f, th::Rgba(0x253046));
    ui_.Text(L"C", {w - 90.0f, 26.0f, w - 54.0f, 62.0f}, 15.0f, th::Rgba(th::kInk),
             DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
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

void App::DrawPill(float center_x, float top, const wchar_t* label, float width, bool hovered,
                   Hit hit, int index) {
    const D2D1_RECT_F r{center_x - width * 0.5f, top, center_x + width * 0.5f, top + 46.0f};
    ui_.FillRounded(r, 23.0f, th::Rgba(th::kAccent, hovered ? 0.26f : 0.16f));
    ui_.StrokeRounded(r, 23.0f, th::Rgba(th::kAccent, hovered ? 1.0f : 0.85f), 1.5f);

    const float ty = top + 23.0f;
    const float tx = r.left + 30.0f;
    ui_.FillTriangle({tx, ty - 7.0f}, {tx, ty + 7.0f}, {tx + 11.0f, ty}, th::Rgba(th::kInk));

    ui_.Text(label, {r.left + 50.0f, r.top, r.right - 14.0f, r.bottom}, 15.0f, th::Rgba(th::kInk),
             DWRITE_FONT_WEIGHT_SEMI_BOLD);

    AddHit(hit, index, r);
}

void App::DrawHints(const wchar_t* hints) {
    ui_.Text(hints, {0.0f, static_cast<float>(window_->height()) - 44.0f,
                     static_cast<float>(window_->width()),
                     static_cast<float>(window_->height()) - 16.0f},
             12.0f, th::Rgba(th::kMuted, 0.85f), DWRITE_FONT_WEIGHT_NORMAL,
             DWRITE_TEXT_ALIGNMENT_CENTER);
}

void App::RenderHome(double t) {
    (void)t;
    const float w = static_cast<float>(window_->width());
    const float h = static_cast<float>(window_->height());

    ui_.FillLinearV({0.0f, 0.0f, w, h}, th::Rgba(th::kBgTop), th::Rgba(th::kBgBottom));

    const GameEntry& game = games_[static_cast<size_t>(selected_)];
    ui_.FillRadial(w * 0.50f, h * 0.55f, w * 0.62f, th::Rgba(game.accent, 0.20f),
                   th::Rgba(game.accent, 0.0f));
    ui_.FillRadial(w * 0.16f, h * 0.18f, w * 0.38f, th::Rgba(0x2B4C8C, 0.14f),
                   th::Rgba(0x2B4C8C, 0.0f));

    RenderTopBar();

    const float tile_h = std::clamp(h * 0.44f, 240.0f, 420.0f);
    const float tile_w = tile_h * 0.75f;
    const float gap = tile_h * 0.065f;
    const float center_y = h * 0.50f;

    std::vector<int> order(games_.size());
    for (size_t i = 0; i < order.size(); ++i) {
        order[i] = static_cast<int>(i);
    }
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        return std::fabs(static_cast<float>(a) - scroll_) > std::fabs(static_cast<float>(b) - scroll_);
    });

    for (int idx : order) {
        const float d = static_cast<float>(idx) - scroll_;
        if (std::fabs(d) > 3.2f) {
            continue;
        }
        const float k = std::min(std::fabs(d), 3.0f);
        const float scale = 1.0f - 0.055f * k;
        const float tw = tile_w * scale;
        const float th = tile_h * scale;
        const float x = w * 0.5f + d * (tile_w + gap) - tw * 0.5f;
        const float y = center_y - th * 0.5f;
        const D2D1_RECT_F rect{x, y, x + tw, y + th};
        DrawTile(rect, games_[static_cast<size_t>(idx)], 1.0f - 0.30f * k, idx == selected_,
                 hover_ == Hit::Tile && hover_index_ == idx);
        AddHit(Hit::Tile, idx, rect);
    }

    const float rail_bottom = center_y + tile_h * 0.5f;
    ui_.Text(game.title, {w * 0.5f - 600.0f, rail_bottom + 24.0f, w * 0.5f + 600.0f,
                          rail_bottom + 72.0f},
             36.0f, th::Rgba(th::kInk), DWRITE_FONT_WEIGHT_SEMI_BOLD,
             DWRITE_TEXT_ALIGNMENT_CENTER);
    ui_.Text(game.meta, {w * 0.5f - 600.0f, rail_bottom + 74.0f, w * 0.5f + 600.0f,
                         rail_bottom + 100.0f},
             15.0f, th::Rgba(th::kMuted), DWRITE_FONT_WEIGHT_NORMAL,
             DWRITE_TEXT_ALIGNMENT_CENTER);

    DrawPill(w * 0.5f, rail_bottom + 112.0f, L"Stream", 200.0f, hover_ == Hit::Stream, Hit::Stream,
             -1);

    if (pad_.state().connected) {
        ui_.FillCircle(52.0f, h - 74.0f, 4.0f, th::Rgba(th::kOnline));
        ui_.Text(Widen(pad_.state().name).c_str(), {64.0f, h - 86.0f, 420.0f, h - 62.0f}, 12.0f,
                 th::Rgba(th::kMuted));
    }

    DrawHints(L"Left / Right   Navigate      Enter   Stream      S   Settings      C   Controller"
              L"      M   Audio      Esc   Quit");
}

void App::RenderSettings(double t) {
    (void)t;
    const float w = static_cast<float>(window_->width());
    const float h = static_cast<float>(window_->height());

    ui_.FillLinearV({0.0f, 0.0f, w, h}, th::Rgba(0x0A1018), th::Rgba(th::kBgBottom));
    ui_.FillRadial(w * 0.5f, -h * 0.1f, w * 0.7f, th::Rgba(0x1B3A66, 0.28f),
                   th::Rgba(0x1B3A66, 0.0f));

    // Back button
    const D2D1_RECT_F back{w - 132.0f, 28.0f, w - 40.0f, 64.0f};
    ui_.FillRounded(back, 18.0f, th::Rgba(0xFFFFFF, hover_ == Hit::Back ? 0.12f : 0.06f));
    ui_.StrokeRounded(back, 18.0f, th::Rgba(0xFFFFFF, 0.10f), 1.0f);
    ui_.Text(L"Back", {back.left, back.top, back.right, back.bottom}, 13.0f,
             th::Rgba(th::kInk, 0.9f), DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    AddHit(Hit::Back, -1, back);

    const float panel_w = std::min(1040.0f, w - 96.0f);
    const float x0 = (w - panel_w) * 0.5f;

    ui_.Text(L"Settings", {x0, 84.0f, x0 + panel_w, 146.0f}, 40.0f, th::Rgba(th::kInk),
             DWRITE_FONT_WEIGHT_SEMI_BOLD);
    const std::wstring subtitle =
        session_note_.empty() ? L"Changes apply live. Console features unlock after pairing."
                              : Widen(session_note_);
    ui_.Text(subtitle.c_str(), {x0, 148.0f, x0 + panel_w, 174.0f}, 13.0f, th::Rgba(th::kMuted));

    struct Row {
        const wchar_t* label;
        std::wstring value;
        bool enabled;
    };

    const std::wstring console_value = Widen(
        profile_.has_console() ? profile_.status_line() : std::string("Offline · no console"));
    const std::wstring account_value = Widen(
        profile_.psn_account_id.empty() ? std::string("not set") : std::string("imported"));
    const std::wstring bitrate_value = Widen(std::to_string(profile_.bitrate_kbps / 1000) +
                                             " Mbps · waits for PS5");

    const Row rows[kSettingCount] = {
        {L"Upscaler", kUpscalerNames[upscale_], true},
        {L"Latency HUD", hud_ ? L"On" : L"Off", true},
        {L"Frame generation", L"Off  ·  needs RIFE model", false},
        {L"HDR output", L"Off  ·  waits for PS5", false},
        {L"Bitrate", bitrate_value, false},
        {L"Console", console_value, false},
        {L"PSN account", account_value, false},
        {L"Audio output",
         audio_.is_open() ? Widen(audio_.device_name())
                          : (audio_failed_ ? std::wstring(L"open failed") : std::wstring(L"Open  ›")),
         true},
        {L"Controller test", L"Open  ›", true},
    };

    const float row_h = 60.0f;
    float y = 206.0f;

    for (int i = 0; i < kSettingCount; ++i) {
        const D2D1_RECT_F row_rect{x0, y, x0 + panel_w, y + row_h - 6.0f};
        const bool selected = i == setting_row_;
        const bool hovered = hover_ == Hit::SettingRow && hover_index_ == i;

        if (selected || hovered) {
            ui_.FillRounded(row_rect, 12.0f, th::Rgba(0xFFFFFF, hovered ? 0.08f : 0.06f));
        }
        if (selected) {
            ui_.FillRounded({x0 + 10.0f, y + 10.0f, x0 + 14.0f, y + row_h - 16.0f}, 2.0f,
                            th::Rgba(th::kAccent, 0.95f));
        }

        const float text_alpha = rows[i].enabled ? 1.0f : 0.45f;
        ui_.Text(rows[i].label, {x0 + 28.0f, y, x0 + panel_w * 0.55f, y + row_h - 6.0f}, 17.0f,
                 th::Rgba(th::kInk, text_alpha), DWRITE_FONT_WEIGHT_NORMAL);
        ui_.Text(rows[i].value.c_str(),
                 {x0 + panel_w * 0.45f, y, x0 + panel_w - 28.0f, y + row_h - 6.0f}, 15.0f,
                 th::Rgba(rows[i].enabled ? th::kInkDim : th::kMuted, text_alpha),
                 DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING);

        AddHit(Hit::SettingRow, i, row_rect);
        if (rows[i].enabled) {
            AddHit(Hit::SettingLeft, i,
                   {row_rect.left, row_rect.top, row_rect.left + panel_w * 0.35f, row_rect.bottom});
            AddHit(Hit::SettingRight, i,
                   {row_rect.left + panel_w * 0.62f, row_rect.top, row_rect.right, row_rect.bottom});
        }

        y += row_h;
    }

    DrawHints(L"Up / Down   Select      Left / Right   Change      Enter   Open      S / Esc   Back");
}

void App::RenderController(double t) {
    (void)t;
    const float w = static_cast<float>(window_->width());
    const float h = static_cast<float>(window_->height());

    ui_.FillLinearV({0.0f, 0.0f, w, h}, th::Rgba(0x0A1018), th::Rgba(th::kBgBottom));
    ui_.FillRadial(w * 0.5f, -h * 0.1f, w * 0.7f, th::Rgba(0x243B63, 0.30f),
                   th::Rgba(0x243B63, 0.0f));

    const D2D1_RECT_F back{w - 132.0f, 28.0f, w - 40.0f, 64.0f};
    ui_.FillRounded(back, 18.0f, th::Rgba(0xFFFFFF, hover_ == Hit::Back ? 0.12f : 0.06f));
    ui_.StrokeRounded(back, 18.0f, th::Rgba(0xFFFFFF, 0.10f), 1.0f);
    ui_.Text(L"Back", {back.left, back.top, back.right, back.bottom}, 13.0f,
             th::Rgba(th::kInk, 0.9f), DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    AddHit(Hit::Back, -1, back);

    const float panel_w = std::min(1040.0f, w - 96.0f);
    const float x0 = (w - panel_w) * 0.5f;

    ui_.Text(L"Controller", {x0, 84.0f, x0 + panel_w, 146.0f}, 40.0f, th::Rgba(th::kInk),
             DWRITE_FONT_WEIGHT_SEMI_BOLD);
    std::wstring pad_status = PadStatus(pad_.state());
    if (pad_.is_sony()) {
        pad_status += last_send_ok_ ? L"   ·   output reports OK" : L"   ·   output reports failed";
    }
    ui_.Text(pad_status.c_str(), {x0, 148.0f, x0 + panel_w, 174.0f}, 13.0f,
             th::Rgba(th::kMuted));

    struct Row {
        const wchar_t* label;
        std::wstring value;
        bool enabled;
    };

    const std::wstring lightbar_value = Widen("Preset " + std::to_string(lightbar_index_ + 1));
    const std::wstring strength_value = Widen(std::to_string(trigger_strength_));

    const Row rows[kControllerCount] = {
        {L"Lightbar", lightbar_value, true},
        {L"Adaptive trigger", kTriggerNames[trigger_effect_], true},
        {L"Trigger strength", strength_value, true},
        {L"Rumble test", L"Press Enter", true},
        {L"Player LEDs", kPlayerLedNames[player_leds_], true},
        {L"Mic LED", kMicLedNames[mic_led_], true},
        {L"Back", L"", true},
    };

    const float row_h = 56.0f;
    float y = 204.0f;

    for (int i = 0; i < kControllerCount; ++i) {
        const D2D1_RECT_F row_rect{x0, y, x0 + panel_w, y + row_h - 6.0f};
        const bool selected = i == controller_row_;
        const bool hovered = hover_ == Hit::ControllerRow && hover_index_ == i;

        if (selected || hovered) {
            ui_.FillRounded(row_rect, 12.0f, th::Rgba(0xFFFFFF, hovered ? 0.08f : 0.06f));
        }
        if (selected) {
            ui_.FillRounded({x0 + 10.0f, y + 9.0f, x0 + 14.0f, y + row_h - 15.0f}, 2.0f,
                            th::Rgba(th::kAccent, 0.95f));
        }

        ui_.Text(rows[i].label, {x0 + 28.0f, y, x0 + panel_w * 0.55f, y + row_h - 6.0f}, 16.0f,
                 th::Rgba(th::kInk), DWRITE_FONT_WEIGHT_NORMAL);
        ui_.Text(rows[i].value.c_str(),
                 {x0 + panel_w * 0.45f, y, x0 + panel_w - 28.0f, y + row_h - 6.0f}, 15.0f,
                 th::Rgba(th::kInkDim), DWRITE_FONT_WEIGHT_NORMAL,
                 DWRITE_TEXT_ALIGNMENT_TRAILING);

        AddHit(Hit::ControllerRow, i, row_rect);
        if (i == 3) {
            AddHit(Hit::ControllerAction, i, row_rect);
        } else if (i < 6) {
            AddHit(Hit::ControllerLeft, i,
                   {row_rect.left, row_rect.top, row_rect.left + panel_w * 0.35f, row_rect.bottom});
            AddHit(Hit::ControllerRight, i,
                   {row_rect.left + panel_w * 0.62f, row_rect.top, row_rect.right, row_rect.bottom});
        }

        y += row_h;
    }

    // Live input panel
    const PadState& pad = pad_.state();
    const D2D1_RECT_F live{x0, y + 14.0f, x0 + panel_w, y + 14.0f + 132.0f};
    ui_.FillRounded(live, 14.0f, th::Rgba(0xFFFFFF, 0.04f));

    if (pad.connected) {
        std::wstring buttons;
        for (const ButtonLabel& label : kButtonLabels) {
            if (pad.buttons & label.mask) {
                if (!buttons.empty()) {
                    buttons += L"   ";
                }
                buttons += label.label;
            }
        }
        if (buttons.empty()) {
            buttons = L"none";
        }

        wchar_t line[256];
        swprintf_s(line, L"Buttons   %s", buttons.c_str());
        ui_.Text(line, {live.left + 24.0f, live.top + 10.0f, live.right - 24.0f, live.top + 40.0f},
                 13.0f, th::Rgba(th::kInkDim));

        swprintf_s(line, L"Sticks    L %.2f / %.2f      R %.2f / %.2f", pad.lx, pad.ly, pad.rx,
                   pad.ry);
        ui_.Text(line, {live.left + 24.0f, live.top + 38.0f, live.right - 24.0f, live.top + 64.0f},
                 13.0f, th::Rgba(th::kInkDim));

        swprintf_s(line, L"Triggers  L2 %.2f      R2 %.2f", pad.l2, pad.r2);
        ui_.Text(line, {live.left + 24.0f, live.top + 64.0f, live.right - 24.0f, live.top + 90.0f},
                 13.0f, th::Rgba(th::kInkDim));

        if (pad.touch[0].active) {
            swprintf_s(line, L"Touch     %u, %u      Touchpad %s", pad.touch[0].x,
                       pad.touch[0].y, (pad.buttons & PadTouchpad) ? L"pressed" : L"");
        } else {
            swprintf_s(line, L"Touch     -");
        }
        ui_.Text(line, {live.left + 24.0f, live.top + 90.0f, live.right - 24.0f, live.top + 116.0f},
                 13.0f, th::Rgba(th::kInkDim));
    } else {
        ui_.Text(L"Connect a DualSense (USB or Bluetooth) to test lightbar, triggers and rumble.",
                 {live.left + 24.0f, live.top + 10.0f, live.right - 24.0f, live.bottom - 10.0f},
                 13.0f, th::Rgba(th::kMuted));
    }

    DrawHints(L"Up / Down   Select      Left / Right   Change      Enter   Rumble      C / Esc   Back");
}

void App::RenderAudio(double t) {
    (void)t;
    const float w = static_cast<float>(window_->width());
    const float h = static_cast<float>(window_->height());

    ui_.FillLinearV({0.0f, 0.0f, w, h}, th::Rgba(0x0A1018), th::Rgba(th::kBgBottom));
    ui_.FillRadial(w * 0.5f, -h * 0.1f, w * 0.7f, th::Rgba(0x1E5A4A, 0.30f),
                   th::Rgba(0x1E5A4A, 0.0f));

    const D2D1_RECT_F back{w - 132.0f, 28.0f, w - 40.0f, 64.0f};
    ui_.FillRounded(back, 18.0f, th::Rgba(0xFFFFFF, hover_ == Hit::Back ? 0.12f : 0.06f));
    ui_.StrokeRounded(back, 18.0f, th::Rgba(0xFFFFFF, 0.10f), 1.0f);
    ui_.Text(L"Back", {back.left, back.top, back.right, back.bottom}, 13.0f,
             th::Rgba(th::kInk, 0.9f), DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    AddHit(Hit::Back, -1, back);

    const float panel_w = std::min(1040.0f, w - 96.0f);
    const float x0 = (w - panel_w) * 0.5f;

    ui_.Text(L"Audio", {x0, 84.0f, x0 + panel_w, 146.0f}, 40.0f, th::Rgba(th::kInk),
             DWRITE_FONT_WEIGHT_SEMI_BOLD);
    ui_.Text(L"WASAPI shared mode, in-process: no virtual cable, no driver. The DualSense "
             L"3.5 mm jack is a standard USB audio endpoint (USB only).",
             {x0, 148.0f, x0 + panel_w, 174.0f}, 13.0f, th::Rgba(th::kMuted));

    std::wstring device_value;
    if (audio_devices_.empty()) {
        device_value = L"System default";
    } else {
        const audio::DeviceInfo& info = audio_devices_[static_cast<size_t>(audio_device_)];
        device_value = Widen(info.name);
        if (info.is_default) {
            device_value += L"  (default)";
        } else if (info.is_controller) {
            device_value += L"  (controller)";
        }
    }

    struct Row {
        const wchar_t* label;
        std::wstring value;
        bool enabled;
    };

    const Row rows[kAudioCount] = {
        {L"Output device", device_value, true},
        {L"Buffer", Widen(std::to_string(audio_buffer_ms_) + " ms"), true},
        {L"Test tone", L"Enter", true},
        {L"Back", L"", true},
    };

    const float row_h = 56.0f;
    float y = 204.0f;

    for (int i = 0; i < kAudioCount; ++i) {
        const D2D1_RECT_F row_rect{x0, y, x0 + panel_w, y + row_h - 6.0f};
        const bool selected = i == audio_row_;
        const bool hovered = hover_ == Hit::AudioRow && hover_index_ == i;

        if (selected || hovered) {
            ui_.FillRounded(row_rect, 12.0f, th::Rgba(0xFFFFFF, hovered ? 0.08f : 0.06f));
        }
        if (selected) {
            ui_.FillRounded({x0 + 10.0f, y + 9.0f, x0 + 14.0f, y + row_h - 15.0f}, 2.0f,
                            th::Rgba(th::kOnline, 0.95f));
        }

        ui_.Text(rows[i].label, {x0 + 28.0f, y, x0 + panel_w * 0.45f, y + row_h - 6.0f}, 16.0f,
                 th::Rgba(th::kInk), DWRITE_FONT_WEIGHT_NORMAL);
        if (!rows[i].value.empty()) {
            ui_.Text(rows[i].value.c_str(),
                     {x0 + panel_w * 0.30f, y, x0 + panel_w - 28.0f, y + row_h - 6.0f}, 15.0f,
                     th::Rgba(th::kInkDim), DWRITE_FONT_WEIGHT_NORMAL,
                     DWRITE_TEXT_ALIGNMENT_TRAILING);
        }

        AddHit(Hit::AudioRow, i, row_rect);
        if (i == 2) {
            AddHit(Hit::AudioAction, i, row_rect);
        } else if (i == 0 || i == 1) {
            AddHit(Hit::AudioLeft, i,
                   {row_rect.left, row_rect.top, row_rect.left + panel_w * 0.30f, row_rect.bottom});
            AddHit(Hit::AudioRight, i,
                   {row_rect.left + panel_w * 0.62f, row_rect.top, row_rect.right, row_rect.bottom});
        }

        y += row_h;
    }

    const D2D1_RECT_F live{x0, y + 14.0f, x0 + panel_w, y + 14.0f + 150.0f};
    ui_.FillRounded(live, 14.0f, th::Rgba(0xFFFFFF, 0.04f));

    if (audio_.is_open()) {
        wchar_t line[512];
        swprintf_s(line, L"Device      %s", Widen(audio_.device_name()).c_str());
        ui_.Text(line, {live.left + 24.0f, live.top + 12.0f, live.right - 24.0f, live.top + 36.0f},
                 13.0f, th::Rgba(th::kInkDim));

        swprintf_s(line, L"Format      %s", Widen(audio_.format_text()).c_str());
        ui_.Text(line, {live.left + 24.0f, live.top + 38.0f, live.right - 24.0f, live.top + 62.0f},
                 13.0f, th::Rgba(th::kInkDim));

        swprintf_s(line, L"Latency     %.1f ms        Underruns  %d", audio_.latency_ms(),
                   audio_.underruns());
        ui_.Text(line, {live.left + 24.0f, live.top + 64.0f, live.right - 24.0f, live.top + 88.0f},
                 13.0f, th::Rgba(th::kInkDim));

        // Peak meter
        const D2D1_RECT_F meter{live.left + 24.0f, live.top + 104.0f, live.left + 24.0f + 320.0f,
                                live.top + 120.0f};
        ui_.FillRounded(meter, 4.0f, th::Rgba(0xFFFFFF, 0.08f));
        const float level = std::clamp(audio_.peak(), 0.0f, 1.0f);
        if (level > 0.001f) {
            ui_.FillRounded({meter.left, meter.top, meter.left + 320.0f * level, meter.bottom},
                            4.0f, th::Rgba(level > 0.98f ? 0xFF6B6B : th::kOnline, 0.9f));
        }
        ui_.Text(L"Peak", {meter.right + 16.0f, meter.top - 8.0f, meter.right + 90.0f,
                           meter.bottom + 8.0f},
                12.0f, th::Rgba(th::kMuted));
    } else {
        ui_.Text(audio_failed_
                     ? L"Could not open the device. It may be in use exclusively or was removed."
                     : L"Not started. Press Enter on Output device to open the endpoint.",
                 {live.left + 24.0f, live.top + 12.0f, live.right - 24.0f, live.bottom - 12.0f},
                 13.0f, th::Rgba(th::kMuted));
    }

    DrawHints(L"Up / Down   Select      Left / Right   Change      Enter   Play tone      M / Esc   Back");
}

void App::RenderStream(double t) {
    (void)t;
    const float w = static_cast<float>(window_->width());

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

        ui_.FillCircle(w - 40.0f, 44.0f, 5.0f, th::Rgba(th::kOnline));
        ui_.Text(L"DEMO", {w - 118.0f, 30.0f, w - 54.0f, 58.0f}, 12.0f,
                 th::Rgba(th::kInk, 0.85f), DWRITE_FONT_WEIGHT_SEMI_BOLD,
                 DWRITE_TEXT_ALIGNMENT_TRAILING);
    }

    const D2D1_RECT_F back{w - 132.0f, 78.0f, w - 40.0f, 114.0f};
    ui_.FillRounded(back, 18.0f, th::Rgba(0xFFFFFF, hover_ == Hit::Back ? 0.16f : 0.08f));
    ui_.Text(L"Back", {back.left, back.top, back.right, back.bottom}, 13.0f,
             th::Rgba(th::kInk, 0.9f), DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    AddHit(Hit::Back, -1, back);

    DrawHints(L"Esc   Back      H   Toggle HUD      1 / 2 / 3   Upscaler");
}
