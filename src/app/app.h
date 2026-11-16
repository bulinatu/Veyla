#pragma once

#include <windows.h>

#include <d2d1_1.h>

#include <string>
#include <vector>

#include "audio/audio_engine.h"
#include "media/pipeline.h"
#include "platform/gamepad.h"
#include "session/profile.h"
#include "ui/ui_renderer.h"

namespace brp {

class Win32Window;

struct PointerState {
    int x = 0;
    int y = 0;
    bool down = false;
    bool clicked = false;
    float wheel = 0.0f;
};

struct GameEntry {
    const wchar_t* title;
    const wchar_t* meta;
    unsigned accent;  // 0xRRGGBB
};

class App {
public:
    bool Init(Win32Window* window);
    void Resize(int width, int height);
    void Frame(double time_seconds, const PointerState& pointer);
    void HandleKey(int vk);
    bool quit_requested() const { return quit_; }

private:
    enum class Screen { Home, Settings, Stream, Controller, Audio };

    enum class Hit {
        None,
        Tile,
        Stream,
        SettingsButton,
        ControllerButton,
        Back,
        SettingRow,
        SettingLeft,
        SettingRight,
        ControllerRow,
        ControllerLeft,
        ControllerRight,
        ControllerAction,
        AudioRow,
        AudioLeft,
        AudioRight,
        AudioAction,
    };

    struct HitRegion {
        D2D1_RECT_F rect;
        Hit hit;
        int index;
    };

    void RenderHome(double t);
    void RenderSettings(double t);
    void RenderStream(double t);
    void RenderController(double t);
    void RenderAudio(double t);

    void RenderTopBar();
    void DrawTile(const D2D1_RECT_F& rect, const GameEntry& game, float alpha, bool selected,
                  bool hovered);
    void DrawPill(float center_x, float top, const wchar_t* label, float width, bool hovered,
                  Hit hit, int index);
    void DrawHints(const wchar_t* hints);

    void AddHit(Hit hit, int index, const D2D1_RECT_F& rect);
    void DispatchClick();
    bool Contains(const D2D1_RECT_F& rect, int x, int y) const;

    void HandlePad();
    void ChangeSetting(int direction);
    void ChangeControllerValue(int direction);
    void ChangeAudioValue(int direction);
    void OpenAudioDevice();
    void ApplyControllerOutput(bool force);
    void TriggerRumble();

    Win32Window* window_ = nullptr;
    Presenter presenter_;
    ui::Renderer ui_;

    Screen screen_ = Screen::Home;
    int selected_ = 0;
    float scroll_ = 0.0f;
    int setting_row_ = 0;
    int controller_row_ = 0;
    int audio_row_ = 0;
    int input_source_ = 0;  // 0 Auto, 1 native HID, 2 Steam Input / XInput
    bool hud_ = true;
    int upscale_ = 1;
    bool quit_ = false;
    bool capture_requested_ = false;
    double last_time_ = -1.0;
    double clock_ = 0.0;
    std::vector<GameEntry> games_;

    session::Profile profile_;
    std::string session_note_;

    // input
    Gamepad pad_;
    PointerState pointer_{};
    std::vector<HitRegion> hits_;
    Hit hover_ = Hit::None;
    int hover_index_ = -1;
    double repeat_at_[4] = {0.0, 0.0, 0.0, 0.0};
    bool repeat_was_[4] = {false, false, false, false};

    // DualSense test state
    int lightbar_index_ = 0;
    int trigger_effect_ = 0;
    int trigger_strength_ = 5;
    int player_leds_ = 0;
    int mic_led_ = 0;
    double rumble_until_ = 0.0;
    double last_output_ = 0.0;
    bool output_dirty_ = true;
    bool last_send_ok_ = false;

    // audio
    audio::Engine audio_;
    std::vector<audio::DeviceInfo> audio_devices_;
    int audio_device_ = 0;
    int audio_buffer_ms_ = 20;
    bool audio_failed_ = false;
    bool audio_opened_ = false;
};

}  // namespace brp
