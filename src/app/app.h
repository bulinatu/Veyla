#pragma once

#include <windows.h>

#include <d2d1_1.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "audio/audio_engine.h"
#include "media/nv_vfx.h"
#include "media/nv_vsr.h"
#include "media/nv_dlss_sr.h"
#include "media/nv_dlssnr.h"
#include "media/nv_reflex.h"
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

    // Presentation cadence; 120/180/240 while NVIDIA VFG is active.
    int present_hz() const;

private:
    enum class Screen { Home, Settings, Stream, StreamSettings };
    enum class Tab { Display, Upscaling, Performance, Neural, Legacy, Audio, Controller, Connection, About };

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
        Slider,
        OverlayTab,
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
    void RenderStreamSettings(double t);
    void RenderIntro(double t);

    void RenderTopBar();
    void DrawConsoleCard();
    void DrawTile(const D2D1_RECT_F& rect, const GameEntry& game, float alpha, bool selected,
                  bool hovered);
    void DrawHints(const wchar_t* hints, float alpha = 1.0f);

    void DrawSettingLabel(const wchar_t* label, float y, bool enabled = true);
    void DrawInfoRow(const wchar_t* label, const std::wstring& value, float y);
    void DrawDropdown(const wchar_t* label, const std::vector<std::wstring>& options, int selected,
                      float y, int id, bool enabled = true);
    void DrawSlider(const wchar_t* label, int& value, int min, int max, int step, float y,
                    int id, bool percent = true);
    void DrawButton(const wchar_t* label, const D2D1_RECT_F& rect, int id, bool primary);
    void DrawPinRow(float y);
    void DrawPopup();

    void AddHit(Hit hit, int index, const D2D1_RECT_F& rect);
    void DispatchClick();
    bool Contains(const D2D1_RECT_F& rect, int x, int y) const;

    void HandlePad();
    void ApplyDropdown(int id, int index);
    void ApplyRenderTarget();
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

    bool FrameGenAvailable() const;
    bool EnsureFrameGen();
    std::wstring FindFrameGenRuntime() const;

    bool VsrAvailable() const;
    bool EnsureVsr();
    std::wstring FindVsrRuntime() const;
    void DisableVsr();

    bool DlssAvailable() const;
    bool EnsureDlssSr(int in_width, int in_height);
    bool EnsureDlssSrForTarget();
    std::wstring FindDlssRuntime() const;
    void ApplyNeuralSettings();

    bool NrAvailable() const;
    bool EnsureNr();
    bool ReconfigureNr();
    void NrStageSize(int* width, int* height) const;
    void DlssInputSize(int src_w, int src_h, int* in_w, int* in_h) const;
    void ApplyReflex();
    std::wstring FindNrRuntime() const;
    void ApplyNrSettings();
    void DisableNr();
    void StartNrThread();
    void StopNrThread();
    void NrThreadMain();
    void SubmitNrFrame(media::DecodedFrame& frame);
    void SubmitNrFrameData(std::vector<uint8_t>&& rgba, int width, int height, bool pq);
    bool TakeNrResult(int& width, int& height, bool& pq);
    bool NrFailed(std::string& error);

    Win32Window* window_ = nullptr;
    Presenter presenter_;
    ui::Renderer ui_;
    float ui_scale_ = 1.0f;
    float ui_w_ = 1600.0f;
    float ui_h_ = 900.0f;

    Screen screen_ = Screen::Home;
    Tab tab_ = Tab::Display;
    int selected_ = 0;
    float scroll_ = 0.0f;
    bool hud_ = true;
    bool hdr_ = false;
    int stream_buffer_ = 1;
    int pad_rate_hz_ = 250;
    int render_target_ = 0;
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

    // intro
    bool intro_ = true;
    double intro_start_ = -1.0;
    bool window_shown_ = false;

    // stream chrome idle state
    double mouse_active_at_ = -1.0;
    int last_mouse_x_ = -100000;
    int last_mouse_y_ = -100000;
    bool cursor_hidden_ = false;

    // connection
    net::Reachability probe_;
    session::Streaming streaming_;
    media::DecodedFrame stream_frame_;
    bool stream_active_ = false;
    session::Streaming::StreamStats stream_stats_{};

    // frame generation (NVIDIA VFG)
    media::NvVfx nv_vfx_;
    bool nv_vfx_present_ = false;
    bool fg_ready_ = false;
    int frame_gen_ = 0;  // 0 off, 2/3/4 multiplier
    int fg_generated_ = 0;
    std::wstring fg_note_;
    std::deque<media::NvVfx::Frame> fg_queue_;
    media::NvVfx::Frame fg_last_;

    // neural upscaling (NVIDIA RTX VSR)
    media::NvVsr nv_vsr_;
    bool vsr_present_ = false;
    bool vsr_ready_ = false;
    int vsr_quality_ = 0;  // 0 off, else 1..4 / 21 / 23
    std::wstring vsr_note_;
    media::NvVsr::Frame vsr_frame_;

    // neural rendering (DLSS Super Resolution, D3D12)
    media::NvDlssSr dlss_sr_;
    bool dlss_present_ = false;
    bool dlss_ready_ = false;
    int dlss_in_w_ = 0;
    int dlss_in_h_ = 0;
    std::wstring dlss_note_;
    media::NvDlssSr::Frame dlss_frame_;


    // neural rendering (DLSSNR through the NGX worker)
    media::NvDlssnr nr_;
    bool nr_present_ = false;
    bool nr_ready_ = false;
    int nr_in_w_ = 0;
    int nr_in_h_ = 0;
    std::wstring nr_note_;
    std::vector<uint8_t> nr_out_;

    // Off-thread NR: a one-frame input queue and a latest-result slot. The
    // presentation loop never waits for the NGX worker; when the worker is
    // late its frame is skipped instead of stalling the UI.
    struct NrFrame {
        std::vector<uint8_t> rgba;
        int width = 0;
        int height = 0;
        bool pq = false;
        double ms = 0.0;
    };
    std::thread nr_thread_;
    std::mutex nr_mutex_;
    std::condition_variable nr_cv_;
    bool nr_run_ = false;
    bool nr_settings_dirty_ = false;
    std::optional<NrFrame> nr_pending_;
    std::optional<NrFrame> nr_latest_;
    bool nr_failed_ = false;
    std::string nr_error_;
    uint64_t nr_dropped_ = 0;
    uint64_t nr_shown_ = 0;
    double nr_last_ms_ = 0.0;
    std::vector<uint8_t> sr_readback_;
    std::vector<uint8_t> sr_scaled_;
    std::vector<uint16_t> scale_tmp_;
    media::NvReflex reflex_;
    bool reflex_loaded_ = false;
    int overlay_tab_ = 0;

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
