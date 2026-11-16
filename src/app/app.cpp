#include "app/app.h"

#include "session/discovery.h"
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

enum WidgetId {
    W_Upscaler = 1,
    W_Hud,
    W_FrameGen,
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
    W_PairHome = 42,
};

constexpr float kContentX = 344.0f;
constexpr float kRowH = 66.0f;

const wchar_t* kTriggerNames[] = {L"Off",    L"Rigid",     L"Pulse", L"Feedback",
                                  L"Weapon", L"Vibration", L"Bow",   L"Galloping"};
constexpr int kTriggerCount = static_cast<int>(std::size(kTriggerNames));

const wchar_t* kLightbarNames[] = {L"Blue", L"Red", L"Green", L"Amber",
                                   L"Violet", L"Teal", L"White", L"Off"};
constexpr int kLightbarCount = static_cast<int>(std::size(kLightbarNames));

struct Rgb {
    uint8_t r, g, b;
};

const Rgb kLightbarColors[] = {
    {0x5A, 0xA9, 0xFF}, {0xFF, 0x4D, 0x4D}, {0x57, 0xD6, 0x8A}, {0xFF, 0xC1, 0x45},
    {0xB1, 0x4B, 0xFF}, {0x14, 0xB8, 0xA6}, {0xF4, 0xF7, 0xFC}, {0x00, 0x00, 0x00},
};

const wchar_t* kPlayerLedNames[] = {L"Off", L"Center", L"Inner", L"Outer", L"All", L"Mid"};
const uint8_t kPlayerLedMasks[] = {0x00, 0x04, 0x0A, 0x0F, 0x1F, 0x15};
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
    std::fprintf(stderr, "[click] %d,%d\n", pointer_.x, pointer_.y);
    for (auto it = hits_.rbegin(); it != hits_.rend(); ++it) {
        if (!Contains(it->rect, pointer_.x, pointer_.y)) {
            continue;
        }
        std::fprintf(stderr, "[hit] %d %d\n", static_cast<int>(it->hit), it->index);

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
                screen_ = Screen::Home;
                break;
            case Hit::Tab:
                tab_ = static_cast<Tab>(it->index);
                open_dropdown_ = -1;
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
                    StartProbe();
                } else if (it->index == W_PairHome) {
                    screen_ = Screen::Settings;
                    tab_ = Tab::Connection;
                } else if (it->index == W_Pair) {
                    if (pin_.size() == 8) {
                        pairing_.Start(profile_, static_cast<uint32_t>(std::stoul(pin_)));
                    }
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
    if (vk == VK_F12) {
        capture_requested_ = true;
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
                tab_ = static_cast<Tab>((static_cast<int>(tab_) + 1) % 5);
                open_dropdown_ = -1;
            } else if (vk == VK_LEFT) {
                tab_ = static_cast<Tab>((static_cast<int>(tab_) + 4) % 5);
                open_dropdown_ = -1;
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
            upscale_ = std::clamp(index, 0, 2);
            presenter_.SetUpscaleMode(upscale_);
            break;
        case W_Hud:
            hud_ = index == 1;
            break;
        case W_AudioDevice:
            if (!audio_devices_.empty()) {
                audio_device_ = std::clamp(index, 0, static_cast<int>(audio_devices_.size()) - 1);
                OpenAudioDevice();
            }
            break;
        case W_AudioBuffer:
            audio_buffer_ms_ = kBufferSizes[std::clamp(index, 0, 3)];
            OpenAudioDevice();
            break;
        case W_InputSource:
            input_source_ = std::clamp(index, 0, 2);
            pad_.SetBackend(kInputBackends[input_source_]);
            break;
        case W_Lightbar:
            lightbar_index_ = std::clamp(index, 0, kLightbarCount - 1);
            output_dirty_ = true;
            break;
        case W_Trigger:
            trigger_effect_ = std::clamp(index, 0, kTriggerCount - 1);
            output_dirty_ = true;
            break;
        case W_Strength:
            trigger_strength_ = std::clamp(index + 1, 1, 8);
            output_dirty_ = true;
            break;
        case W_PlayerLeds:
            player_leds_ = std::clamp(index, 0, kPlayerLedCount - 1);
            output_dirty_ = true;
            break;
        case W_MicLed:
            mic_led_ = std::clamp(index, 0, 2);
            output_dirty_ = true;
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
    rumble_until_ = clock_ + 0.45;
    output_dirty_ = true;
}

void App::StartProbe() {
    probe_.Start(profile_.host, 9295);
}

void App::ApplyControllerOutput(bool force) {
    (void)force;
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
    probe_.Poll();
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

    ui_.FillCircle(w - 60.0f, 44.0f, 18.0f, th::Rgba(0x253046));
    ui_.Text(L"C", {w - 78.0f, 26.0f, w - 42.0f, 62.0f}, 15.0f, th::Rgba(th::kInk),
             DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
}

void App::DrawConsoleCard() {
    const float w = static_cast<float>(window_->width());
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
            status = Widen(profile_.status_line());
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

void App::DrawHints(const wchar_t* hints) {
    ui_.Text(hints, {0.0f, static_cast<float>(window_->height()) - 40.0f,
                     static_cast<float>(window_->width()),
                     static_cast<float>(window_->height()) - 14.0f},
             12.0f, th::Rgba(th::kMuted, 0.85f), DWRITE_FONT_WEIGHT_NORMAL,
             DWRITE_TEXT_ALIGNMENT_CENTER);
}

void App::DrawSettingLabel(const wchar_t* label, float y, bool enabled) {
    ui_.Text(label, {kContentX, y, kContentX + 460.0f, y + 56.0f}, 16.0f,
             th::Rgba(enabled ? th::kInk : th::kMuted, enabled ? 0.95f : 0.6f),
             DWRITE_FONT_WEIGHT_NORMAL);
}

void App::DrawInfoRow(const wchar_t* label, const std::wstring& value, float y) {
    const float right = static_cast<float>(window_->width()) - 64.0f;
    DrawSettingLabel(label, y, true);
    ui_.TextEllipsis(value.c_str(), {right - 420.0f, y, right, y + 56.0f}, 15.0f,
                     th::Rgba(th::kInkDim), DWRITE_FONT_WEIGHT_NORMAL,
                     DWRITE_TEXT_ALIGNMENT_TRAILING);
}

void App::DrawDropdown(const wchar_t* label, const std::vector<std::wstring>& options, int selected,
                       float y, int id, bool enabled) {
    const float right = static_cast<float>(window_->width()) - 64.0f;
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
    const float right = static_cast<float>(window_->width()) - 64.0f;
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
    if (top + height + 8.0f > static_cast<float>(window_->height()) - 40.0f) {
        top = popup_.anchor.top - 6.0f - height - 8.0f;
    }

    const D2D1_RECT_F panel{popup_.anchor.left, top, popup_.anchor.right, top + height + 8.0f};
    ui_.FillRounded(panel, 12.0f, th::Rgba(0x141B28, 0.98f));
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

void App::RenderHome(double t) {
    (void)t;
    const float w = static_cast<float>(window_->width());
    const float h = static_cast<float>(window_->height());

    ui_.FillLinearV({0.0f, 0.0f, w, h}, th::Rgba(th::kBgTop), th::Rgba(th::kBgBottom));
    ui_.FillRadial(w * 0.5f, h * 0.42f, w * 0.55f, th::Rgba(0x2E6BFF, 0.18f),
                   th::Rgba(0x2E6BFF, 0.0f));
    ui_.FillRadial(w * 0.14f, h * 0.12f, w * 0.34f, th::Rgba(0x2B4C8C, 0.14f),
                   th::Rgba(0x2B4C8C, 0.0f));

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
            status = Widen(profile_.status_line());
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
    const float w = static_cast<float>(window_->width());
    const float h = static_cast<float>(window_->height());

    ui_.FillLinearV({0.0f, 0.0f, w, h}, th::Rgba(0x0A1018), th::Rgba(th::kBgBottom));
    ui_.FillRadial(w * 0.5f, -h * 0.1f, w * 0.7f, th::Rgba(0x1B3A66, 0.25f),
                   th::Rgba(0x1B3A66, 0.0f));

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

    const wchar_t* tab_names[] = {L"Display", L"Audio", L"Controller", L"Connection", L"About"};
    for (int i = 0; i < 5; ++i) {
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
            DrawDropdown(L"Upscaler",
                         {L"Bilinear", L"Catmull-Rom", L"Catmull-Rom + Sharpen"}, upscale_, y,
                         W_Upscaler);
            y += kRowH;
            DrawDropdown(L"Latency HUD", {L"Off", L"On"}, hud_ ? 1 : 0, y, W_Hud);
            y += kRowH;
            DrawDropdown(L"Frame generation", {L"Off  ·  needs RIFE model"}, 0, y, W_FrameGen,
                         false);
            y += kRowH;
            DrawDropdown(L"HDR output", {L"Off  ·  waits for PS5"}, 0, y, W_Hdr, false);
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
            const int selected_device =
                std::clamp(audio_device_, 0, static_cast<int>(devices.size()) - 1);
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
            DrawDropdown(L"Input source", {kInputSourceNames[0], kInputSourceNames[1],
                                           kInputSourceNames[2]},
                         input_source_, y, W_InputSource);
            y += kRowH;
            DrawDropdown(L"Lightbar", {kLightbarNames[0], kLightbarNames[1], kLightbarNames[2],
                                       kLightbarNames[3], kLightbarNames[4], kLightbarNames[5],
                                       kLightbarNames[6], kLightbarNames[7]},
                         lightbar_index_, y, W_Lightbar);
            y += kRowH;
            DrawDropdown(L"Adaptive trigger",
                         {kTriggerNames[0], kTriggerNames[1], kTriggerNames[2], kTriggerNames[3],
                          kTriggerNames[4], kTriggerNames[5], kTriggerNames[6], kTriggerNames[7]},
                         trigger_effect_, y, W_Trigger);
            y += kRowH;
            DrawDropdown(L"Trigger strength",
                         {L"1", L"2", L"3", L"4", L"5", L"6", L"7", L"8"}, trigger_strength_ - 1,
                         y, W_Strength);
            y += kRowH;
            DrawDropdown(L"Player LEDs",
                         {kPlayerLedNames[0], kPlayerLedNames[1], kPlayerLedNames[2],
                          kPlayerLedNames[3], kPlayerLedNames[4], kPlayerLedNames[5]},
                         player_leds_, y, W_PlayerLeds);
            y += kRowH;
            DrawDropdown(L"Mic LED", {kMicLedNames[0], kMicLedNames[1], kMicLedNames[2]}, mic_led_,
                         y, W_MicLed);
            y += kRowH;

            DrawButton(L"Rumble test", {kContentX, y + 6.0f, kContentX + 220.0f, y + 50.0f},
                       W_Rumble, false);
            y += kRowH;

            std::wstring status = PadStatus(pad_.state());
            if (pad_.is_sony()) {
                status += last_send_ok_ ? L"   ·   output reports OK" : L"   ·   output reports failed";
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

    DrawHints(L"Esc   back      H   toggle HUD      1 / 2 / 3   upscaler");
}
