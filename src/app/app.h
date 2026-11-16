#pragma once

#include <windows.h>

#include <d2d1_1.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "audio/audio_engine.h"
#include "media/pipeline.h"
#include "net/reachability.h"
#include "platform/gamepad.h"
#include "session/pairing.h"
#include "session/streaming.h"
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
    ~App();
    bool Init(Win32Window* window);
    void Resize(int width, int height);
    void Frame(double time_seconds, const PointerState& pointer);
    void HandleKey(int vk);
    bool quit_requested() const { return quit_; }

private:
    enum class Screen { Home, Settings, Stream };
    enum class Tab { Display, Audio, Controller, Connection, About };

    enum class Hit {
        None,
        Tile,
        Stream,
        SettingsButton,
        Back,
        Tab,
        ConsoleConnect,
        Dropdown,
        DropdownItem,
        Button,
        PinField,
    };

    struct HitRegion {
        D2D1_RECT_F rect;
        Hit hit;
        int index;
    };

    struct Popup {
        int id = -1;
        D2D1_RECT_F anchor{};
        std::vector<std::wstring> options;
        int selected = 0;
    };

    void RenderHome(double t);
    void RenderSettings(double t);
    void RenderStream(double t);

    void RenderTopBar();
    void DrawConsoleCard();
    void DrawTile(const D2D1_RECT_F& rect, const GameEntry& game, float alpha, bool selected,
                  bool hovered);
    void DrawHints(const wchar_t* hints);

    void DrawSettingLabel(const wchar_t* label, float y, bool enabled = true);
    void DrawInfoRow(const wchar_t* label, const std::wstring& value, float y);
    void DrawDropdown(const wchar_t* label, const std::vector<std::wstring>& options, int selected,
                      float y, int id, bool enabled = true);
    void DrawButton(const wchar_t* label, const D2D1_RECT_F& rect, int id, bool primary);
    void DrawPinRow(float y);
    void DrawPopup();

    void AddHit(Hit hit, int index, const D2D1_RECT_F& rect);
    void DispatchClick();
    bool Contains(const D2D1_RECT_F& rect, int x, int y) const;

    void HandlePad();
    void ApplyDropdown(int id, int index);
    void OpenAudioDevice();
    void ApplyControllerOutput(bool force);
    void TriggerRumble();
    void StartProbe();
    void ConnectConsole();
    void StartStream();
    void TickPendingConnect();
    void SaveProfile();
    void StartPadThread();
    void StopPadThread();
    void PadThreadMain();
    void DisconnectStream();

    Win32Window* window_ = nullptr;
    Presenter presenter_;
    ui::Renderer ui_;

    Screen screen_ = Screen::Home;
    Tab tab_ = Tab::Display;
    int selected_ = 0;
    float scroll_ = 0.0f;
    bool hud_ = true;
    bool hdr_ = false;
    int stream_buffer_ = 1;
    int pad_rate_hz_ = 250;
    std::thread pad_thread_;
    std::atomic<bool> pad_run_{false};
    int upscale_ = 1;
    bool quit_ = false;
    bool capture_requested_ = false;
    double last_time_ = -1.0;
    double clock_ = 0.0;
    std::vector<GameEntry> games_;

    session::Profile profile_;
    std::string session_note_;
    std::string stream_note_;

    Gamepad pad_;
    PointerState pointer_{};
    std::vector<HitRegion> hits_;
    Hit hover_ = Hit::None;
    int hover_index_ = -1;
    double repeat_at_[4] = {0.0, 0.0, 0.0, 0.0};
    bool repeat_was_[4] = {false, false, false, false};

    int open_dropdown_ = -1;
    Popup popup_;

    // controller
    int input_source_ = 0;
    int lightbar_index_ = 0;
    int trigger_effect_ = 0;
    int trigger_strength_ = 5;
    int player_leds_ = 0;
    int mic_led_ = 0;
    bool console_rumble_ = true;
    bool console_triggers_ = true;
    bool motion_ = true;
    uint32_t feedback_version_ = 0;
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

    // connection
    net::Reachability probe_;
    session::Streaming streaming_;
    media::DecodedFrame stream_frame_;
    bool stream_active_ = false;

    // Connect flow: check the control port, wake the console if it is asleep,
    // then start the session.
    enum class PendingConnect { None, Probing, Waking };
    PendingConnect pending_connect_ = PendingConnect::None;
    double next_connect_step_ = 0.0;
    double wake_deadline_ = 0.0;
    int wake_packets_ = 0;

    session::Pairing pairing_;
    std::string pin_;
    bool pin_focus_ = false;
};

}  // namespace brp
