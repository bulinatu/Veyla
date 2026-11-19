#pragma once

#include <windows.h>

#include <d2d1_1.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <filesystem>
#include <fstream>

#include "app/rife_setup.h"
#include "audio/audio_engine.h"
#include "media/nv_vfx.h"
#include "media/nv_dlss_sr.h"
#include "media/nv_dlssnr.h"
#include "media/nv_reflex.h"
#include "media/rife.h"
#include "media/pipeline.h"
#include "net/reachability.h"
#include "platform/gamepad.h"
#include "session/pairing.h"
#include "session/discovery.h"
#include "session/psn_auth.h"
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

class App {
public:
    ~App();
    bool Init(Win32Window* window);
    void Resize(int width, int height);
    void Frame(double time_seconds, const PointerState& pointer);
    void HandleKey(int vk);
    bool quit_requested() const { return quit_; }

    // Presentation cadence: the monitor's refresh rate.
    int present_hz() const;
    bool present_covered() const { return present_covered_; }

    // vblank-aligned frame clock exposed by the swapchain. Null means the
    // caller must pace the loop itself.
    HANDLE present_waitable() const;
    bool WaitForPresent(int timeout_ms);

    // CPU time spent building the last frame, excluding the blocking Present.
    double frame_work_ms() const { return frame_work_ms_; }

private:
    enum class Screen { Home, Settings, Stream, StreamSettings };
    enum class Tab { Display, Upscaling, Color, Performance, Neural, Audio, Controller, Connection, Debug, About };

    enum class Hit {
        None,
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
        PsnUrlField,
        PsnPanel,
        AddressField,
        AddressResult,
        AddressPanel,
        Minimize,
        Maximize,
        Close,
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
    void RenderBoot(double t);
    void DrawBrandMark(float cx, float cy, float reveal, float t, float alpha);
    void DrawBootLogo(float cx, float cy, float reveal, float t, float alpha);
    void DrawLoaderMark(float cx, float cy, float reveal, float t, float alpha);
    void DrawLoaderSteps(const std::vector<std::pair<std::wstring, int>>& steps, float x, float y, float w, float t, float alpha);

    void BuildBootTasks();
    void TickBoot(double t);
    void BootWorkerMain();

    void RenderTopBar();
    void RenderWindowChrome();

    void DrawSettingLabel(const wchar_t* label, float y, bool enabled = true);
    void DrawInfoRow(const wchar_t* label, const std::wstring& value, float y);
    void DrawDropdown(const wchar_t* label, const std::vector<std::wstring>& options, int selected,
                      float y, int id, bool enabled = true);
    void DrawSlider(const wchar_t* label, int& value, int min, int max, int step, float y,
                    int id, bool percent = true);
    void DrawButton(const wchar_t* label, const D2D1_RECT_F& rect, int id, bool primary);
    void DrawPinRow(float y);
    void DrawPopup();
    void RenderPsnModal();

    void StartPsnLogin();
    void SubmitPsnLogin();
    void ClosePsnModal();
    void PastePsnClipboard();
    void TickPsnLogin();

    void OpenAddressModal();
    void DiscoverConsoles();
    void SaveConsoleAddress();
    void CloseAddressModal();
    void PasteAddressClipboard();
    void TickAddress();
    void RenderAddressModal();
    bool ReadClipboardText(std::string& out);

    void AddHit(Hit hit, int index, const D2D1_RECT_F& rect);
    void DispatchClick();
    bool Contains(const D2D1_RECT_F& rect, int x, int y) const;

    void HandlePad();
    void ApplyDropdown(int id, int index);
    void ApplyRenderTarget();
    void ApplyEnhance();
    void TickEnhance();
    void ApplyLaunchPreset(int preset);
    void LogHealth();
    void OpenTestingLog();
    void LogTestingLine();
    void TestingEvent(const char* fmt, ...);
    void OpenAudioDevice();
    void ApplyControllerOutput();
    void TriggerRumble();
    void StartProbe();
    void ConnectConsole();
    void StartStream();
    void TickPendingConnect();
    void TickSession();
    bool VideoPaced() const;
    void SaveProfile();
    void StartPadThread();
    void StopPadThread();
    void PadThreadMain();
    void DisconnectStream();
    void ToggleFullscreen();

    bool FrameGenAvailable() const;
    bool EnsureFrameGen();
    std::wstring FindFrameGenRuntime() const;

    bool DlssAvailable() const;
    bool EnsureDlssSr(int in_width, int in_height);
    bool RunDlssSr(const media::DecodedFrame& frame, media::NvDlssSr::Frame& out);
    bool EnsureDlssSrForTarget();
    bool EnsureDlssDlaa(int width, int height);
    std::wstring FindDlssRuntime() const;
    void ApplyNeuralSettings();

    bool NrAvailable() const;
    bool EnsureNr();
    bool ReconfigureNr();
    void NrStageSize(int* width, int* height) const;
    void DlssInputSize(int src_w, int src_h, int* in_w, int* in_h) const;
    void ApplyReflex();
    void ApplyDlssFrameGen();
    bool AttachFgWindow();
    bool AttachFgWindowNow();
    void DetachFgWindow();
    std::wstring FindNrRuntime() const;
    void ApplyNrSettings();
    void DisableNr();
    void StartNrThread();
    void StopNrThread();
    void NrThreadMain();
    void SubmitNrFrame(media::DecodedFrame& frame);
    void SubmitNrFrameData(std::vector<uint8_t>&& rgba, int width, int height, bool pq);
    int FgTargetHz() const;
    int FgChosenMultiplier(double content_hz, int target) const;
    bool FrameIsDuplicate(const media::DecodedFrame& frame);
    bool ShouldDropDuplicate(const media::DecodedFrame& frame);
    void UpdateInterpolationMode();

    // RIFE (TensorRT) engine. Frames are generated on a worker thread and
    // presented from a small queue, so inference never blocks the frame loop.
    bool EnsureRife();
    void StopRife();
    void RifeThreadMain();
    void RifePush(media::DecodedFrame& frame, bool unique = false);
    void RifePresentDue();
    void RenderSetupModal();
    std::filesystem::path ExeDirPath() const;

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
    bool hud_ = true;
    bool hdr_ = false;
    int stream_buffer_ = 1;
    std::atomic<int> pad_rate_hz_{250};
    int render_target_ = 0;
    std::thread pad_thread_;
    std::atomic<bool> pad_run_{false};
    int upscale_ = 1;
    bool quit_ = false;
    // Esc is destructive (quit on Home, disconnect in the stream): require a
    // second press within two seconds so it cannot happen by reflex.
    double last_esc_at_ = -10.0;
    // Content frame for the shared settings widgets. The in-stream sheet is
    // narrower than the full-screen Settings page, so the helpers read these
    // instead of the window edges. 0 means "use the window".
    float panel_x_ = 344.0f;
    float panel_right_ = 0.0f;
    bool device_lost_handled_ = false;
    void RecoverFromDeviceLoss();
    bool capture_requested_ = false;
    double last_time_ = -1.0;
    double clock_ = 0.0;

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
    std::atomic<bool> console_rumble_{true};
    std::atomic<bool> console_triggers_{true};
    bool motion_ = true;
    uint32_t feedback_version_ = 0;
    PadConsoleFeedback pad_feedback_{};
    double last_haptics_at_ = -1.0;
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

    // startup / boot sequence
    struct BootTask {
        std::wstring label;
        std::function<bool()> run;
    };
    bool booting_ = true;
    bool boot_threads_started_ = false;
    bool boot_finalized_ = false;
    size_t boot_total_ = 0;
    size_t boot_done_steps_ = 0;
    double boot_started_ = -1.0;
    double boot_done_at_ = -1.0;
    double boot_last_ = -1.0;
    double boot_target_ = 0.0;
    double boot_shown_ = 0.0;
    double boot_status_alpha_ = 0.0;
    std::wstring boot_status_;
    std::vector<BootTask> boot_tasks_;
    std::thread boot_worker_;
    std::mutex boot_mutex_;
    std::atomic<bool> boot_cancel_{false};
    size_t boot_worker_index_ = 0;
    bool boot_worker_done_ = false;
    bool boot_fg_present_started_ = false;
    std::wstring boot_worker_label_;
    std::thread discover_thread_;
    std::atomic<bool> discover_done_{false};
    std::mutex discover_mutex_;
    std::vector<session::DiscoveredConsole> discovered_;
    bool boot_discovery_applied_ = false;

    // intro
    bool intro_ = true;
    double intro_start_ = -1.0;
    bool window_shown_ = false;

    // stream chrome idle state
    double mouse_active_at_ = -1.0;
    double fg_reveal_until_ = 0.0;
    int last_mouse_x_ = -100000;
    int last_mouse_y_ = -100000;
    bool cursor_hidden_ = false;

    // connection
    net::Reachability probe_;
    session::Streaming streaming_;
    media::DecodedFrame stream_frame_;
    std::atomic<bool> stream_active_{false};
    bool stream_connecting_ = false;
    std::thread session_thread_;
    std::atomic<bool> session_started_{false};
    std::atomic<bool> session_ok_{false};
    std::mutex session_mutex_;
    std::string session_error_;
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

    // Adaptive interpolation. A 30 fps game arrives as a 60 fps stream with
    // every other frame duplicated; an identical pair has nothing to
    // interpolate, so duplicates are skipped and the worker multiplier is
    // chosen from the measured content rate and the requested output rate.
    // 64x64 sample of the previous frame, compared with a tolerance: a
    // duplicated frame can differ by a few counts and still carry no motion.
    std::array<uint32_t, 4096> frame_samples_{};
    bool frame_sig_valid_ = false;
    int fg_mult_live_ = -1;      // multiplier currently sent to the worker
    double fg_content_hz_ = 0.0; // measured unique-frame rate
    double fg_dupes_ = 0.0;      // duplicates skipped in the current window
    double fg_window_at_ = -1.0;
    int fg_window_uniques_ = 0;
    int fg_pending_mult_ = -1;
    int fg_pending_streak_ = 0;

    // RIFE (TensorRT): an alternative interpolation engine. Frames are
    // generated on a worker thread and presented from a small queue, so
    // inference never blocks the frame loop.
    std::unique_ptr<media::Rife> rife_;
    std::thread rife_thread_;
    std::mutex rife_mutex_;
    std::condition_variable rife_cv_;
    struct RifePair {
        std::vector<uint8_t> a;
        std::vector<uint8_t> b;
        int width = 0;
        int height = 0;
        double t0 = 0.0;
        double t1 = 0.0;
        double base_due = 0.0;  // when the held frame is shown
    };
    struct RifeOut {
        std::vector<uint8_t> rgba;
        int width = 0;
        int height = 0;
        double due = 0.0;
        bool pq = false;
    };
    std::deque<RifePair> rife_jobs_;
    std::deque<RifeOut> rife_out_;
    bool rife_run_ = false;
    bool rife_has_prev_ = false;
    std::vector<uint8_t> rife_prev_;
    int rife_prev_w_ = 0;
    int rife_prev_h_ = 0;
    bool rife_prev_pq_ = false;
    double rife_prev_t_ = 0.0;
    double rife_ms_ = 0.0;
    int rife_generated_ = 0;
    int rife_shown_ = 0;
    double rife_rate_at_ = -1.0;
    double rife_out_fps_ = 0.0;
    double rife_last_show_ = -1.0;
    double rife_interval_sum_ = 0.0;
    double rife_interval_max_ = 0.0;
    int rife_interval_n_ = 0;
    int rife_dupes_ = 0;
    int rife_sim_skip_ = 0;
    std::wstring rife_note_;
    std::atomic<double> rife_headroom_ms_{0.0};
    std::array<double, 12> rife_samples_{};
    int rife_sample_pos_ = 0;
    int rife_sample_n_ = 0;
    double rife_gap_ema_ = 0.0;
    double rife_cadence_ms_ = 0.0;
    double rife_cadence_max_ms_ = 0.0;
    double last_frame_diff_ = 0.0;
    RifeSetup rife_setup_;
    bool setup_modal_ = false;

    // neural rendering (DLSS Super Resolution, D3D12)
    media::NvDlssSr dlss_sr_;
    bool dlss_present_ = false;
    bool dlss_ready_ = false;
    int dlss_in_w_ = 0;
    bool dlss_dlaa_ = false;
    int dlss_in_h_ = 0;
    std::wstring dlss_note_;
    media::NvDlssSr::Frame dlss_frame_;
    EnhanceParams enh_applied_{};
    bool enh_dirty_ = false;
    double enh_dirty_at_ = 0.0;
    double health_at_ = -1.0;
    std::string health_path_;
    std::ofstream testing_;
    double testing_at_ = -1.0;
    unsigned long long testing_misses_ = 0;


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
    std::vector<uint16_t> scale_tmp_;    media::NvReflex reflex_;
    bool reflex_loaded_ = false;
    int overlay_tab_ = 0;
    HWND fg_window_ = nullptr;
    bool fg_visible_ = false;
    int fg_w_ = 0;
    int fg_h_ = 0;
    bool borderless_ = false;
    RECT windowed_rect_{};
    LONG_PTR windowed_style_ = 0;
    LONG_PTR windowed_ex_style_ = 0;
    int refresh_hz_ = 60;
    std::deque<float> frame_ms_;
    int frame_stutters_ = 0;
    float stat_p50_ = 0.0f;
    float stat_p95_ = 0.0f;
    float stat_p99_ = 0.0f;
    float stat_max_ = 0.0f;
    float stat_fps_ = 0.0f;
    int stat_late_ = 0;
    int stat_stalls_ = 0;
    double stat_at_ = -1.0;
    double frame_work_ms_ = 0.0;
    double phase_render_ms_ = 0.0;
    double phase_ui_ms_ = 0.0;
    double phase_source_ms_ = 0.0;

    // Live reading from the DLSS Frame Generation worker's own log. NVIDIA's
    // worker tells us how many frames it actually displayed; without that
    // number a 6x setting that only reaches 165 fps looks like it works.
    double fg_worker_fps_ = 0.0;
    double fg_worker_at_ = -1.0;
    void SampleFgWorker();

    // Video cadence: how many refreshes each selected source frame actually
    // held. This is the number the eye sees, independent of the UI loop.
    std::array<uint8_t, 120> video_holds_{};
    int video_hold_pos_ = 0;
    int video_hold_fill_ = 0;
    std::array<uint8_t, 120> screen_holds_{};
    int screen_hold_pos_ = 0;
    int screen_hold_fill_ = 0;
    // Intervals between frames handed to the FG worker (diagnostic).
    std::deque<float> feed_ms_;
    double last_feed_at_ = -1.0;
    double video_phase_ = 0.0;
    double next_source_at_ = 0.0;
    bool source_updated_ = false;
    UINT last_source_refresh_ = 0;
    double last_source_at_ = -1.0;
    double video_probe_at_ = -1.0;
    unsigned long long last_present_misses_ = 0;
    bool present_covered_ = false;
    void NoteSourceFrame();
    void LogVideoCadence();

    // The stream HUD used to reformat five strings (and lay them out) on every
    // 240 Hz frame. The numbers are unreadable above ~10 Hz, so the text is
    // rebuilt on that cadence and the cached layouts are simply drawn.
    struct HudLine {
        std::wstring text;
        float alpha = 0.9f;
    };
    std::vector<HudLine> hud_lines_;
    double hud_lines_at_ = -1.0;
    int hud_lines_mask_ = -1;
    bool hud_lines_stream_ = false;
    bool rumble_was_testing_ = false;

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

    // PSN browser sign-in (account id retrieval)
    bool psn_modal_ = false;
    bool psn_url_focus_ = true;
    bool psn_busy_ = false;
    bool psn_result_ok_ = false;
    bool psn_note_ok_ = false;
    std::string psn_url_input_;
    std::wstring psn_note_;
    std::thread psn_thread_;
    std::atomic<bool> psn_done_{false};
    std::mutex psn_mutex_;
    session::PsnAccount psn_result_;
    std::string psn_detail_;

    // Console address modal / on-demand LAN discovery
    bool address_modal_ = false;
    bool address_focus_ = true;
    bool address_busy_ = false;
    int address_selected_ = -1;
    std::string address_input_;
    std::wstring address_note_;
    std::vector<session::DiscoveredConsole> address_results_;
    std::thread address_thread_;
    std::atomic<bool> address_done_{false};
    std::mutex address_mutex_;
};

}  // namespace brp
