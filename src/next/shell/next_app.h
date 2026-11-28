#pragma once

#include "app/app_host.h"
#include "app/runtime_settings.h"
#include "app/navigation.h"
#include "audio/audio_engine.h"
#include "core/models.h"
#include "media/decoder.h"
#include "media/dx12_video.h"
#include "overlay/overlay_window.h"
#include "platform/gamepad.h"
#include "platform/system_info.h"
#include "shell/ui_bridge.h"
#include "ui/theme.h"
#include "ui/widgets.h"
#include "update/update_service.h"

#include <atomic>
#include <array>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct ID3D11Device;
struct ID3D11DeviceContext;

namespace brp {
class Win32Window;
class Presenter;
namespace ui {
class Renderer;
}
namespace media {
class AmdFsr;
class NvDlssFg;
class NvDlssSr;
class NvDlssnr;
class NvReflex;
}
}  // namespace brp

namespace brp::next {

enum class OverlayAction;
class SettingsControls;
class StreamingService;
class DlssFg;

class NextApp {
public:
    explicit NextApp(bool engine_mode = false, unsigned short bridge_port = 0);
    ~NextApp();

    int Run();

private:
    bool Init();
    void Resize();
    void ResizeStream();
    bool EnsureStreamSurface();
    brp::Win32Window* ActiveWindow() const;
    brp::Presenter* ActivePresenter() const;
    bool PresentVideoDx12();
    void ApplyTargetTo(brp::Presenter* presenter) const;
    void ApplyEnhance();
    void Frame();
    void Render();
    void RenderTitleBar(Widgets& widgets);
    void RenderSidebar(Widgets& widgets);
    void RenderHome(Widgets& widgets);
    void RenderConsole(Widgets& widgets);
    void RenderStreaming(Widgets& widgets);
    void RenderAi(Widgets& widgets);
    void RenderDisplay(Widgets& widgets);
    void RenderOverlay(Widgets& widgets);
    void RenderProfiles(Widgets& widgets);
    void RenderDiagnostics(Widgets& widgets);
    void RenderSettings(Widgets& widgets);
    void RenderAboutPage(Widgets& widgets);
    float PageHeader(Widgets& widgets, const wchar_t* eyebrow, const wchar_t* title,
                      const wchar_t* subtitle, const wchar_t* status = nullptr,
                      bool good = true);
    void PresentVideo();
    bool PullFrame(media::DecodedFrame& out);
    bool PullFrameRaw(media::DecodedFrame& out);
    void StartPadThread();
    void StopPadThread();
    void ToggleWatch(bool watching);
    bool OpenAudio();
    bool EnsureDlssSr(int input_width, int input_height);
    bool EnsureFsr(int input_width, int input_height);
    bool EnsureDlssFgDx12(int width, int height);
    bool TickDlssFgDx12();
    bool EnsureDlssFg(int width, int height);
    void StopDlssFg();
    void RecordVideoShown();
    void ConnectStream();
    void DisconnectStream();
    void ToggleBorderlessFullscreen();
    void ApplyPresenterTarget();
    void BuildOverlayView();
    void HandleOverlayAction(OverlayAction action);
    void SetOverlayControl(bool open);
    void ApplySelectedProfile();
    void StartBenchmark();
    void TickBenchmark();
    void WriteBenchReport();
    void WriteSessionReport();

    float ContentX() const;
    float ContentWidth() const;
    float SidebarWidth() const;
    int RefreshHz() const;
    bool Apply();
    void RefreshProfiles();
    void Toast(const std::wstring& text, bool error = false);
    void ProbeIfNeeded(double now);
    void HandlePinInput();
    static LRESULT CALLBACK LowLevelKeyboardProc(int code, WPARAM wparam, LPARAM lparam);
    bool ShouldCaptureEsc() const;
    std::wstring ClipboardText() const;
    std::wstring W(const std::string& text) const;
    std::wstring Summary(const Profile& profile) const;
    std::wstring ProfileWarning(const Profile& profile) const;

    std::unique_ptr<brp::Win32Window> window_;
    std::unique_ptr<brp::Presenter> presenter_;
    // Shared decode/present device. When it exists, d3d11va decodes on it and
    // the stream presenter samples the video-processor output in place.
    ID3D11Device* decode_device_ = nullptr;
    ID3D11DeviceContext* decode_context_ = nullptr;
    bool CreateDecodeDevice();
    bool NeedsCpuPixels() const;
    void ApplyReflex();
    std::unique_ptr<brp::Win32Window> stream_window_;
    std::unique_ptr<brp::Presenter> stream_presenter_;
    std::unique_ptr<brp::Dx12Video> stream_dx12_;
    std::unique_ptr<brp::ui::Renderer> renderer_;
    std::unique_ptr<AppHost> host_;
    std::shared_ptr<StreamingService> streaming_;
    AppPaths paths_;
    SystemInfo system_;
    AppSettings settings_;
    NavigationModel navigation_;
    std::vector<Profile> profiles_;

    int settings_category_ = 0;
    int ai_tab_ = 0;
    int selected_profile_ = -1;
    bool collapsed_ = false;
    std::string pin_;
    bool pin_focus_ = false;
    float scale_ = 1.0f;
    float logical_width_ = 1600.0f;
    float logical_height_ = 900.0f;
    double now_ = 0.0;
    double last_frame_ = 0.0;
    double last_probe_ = -1.0;
    double started_ = 0.0;
    std::wstring toast_text_;
    bool toast_error_ = false;
    double toast_until_ = 0.0;
    std::uint64_t ui_frames_ = 0;

    brp::audio::Engine audio_;
    brp::Gamepad pad_;
    std::thread pad_thread_;
    std::atomic<bool> pad_run_{false};
    std::atomic<int> haptic_strength_{3};
    std::atomic<int> pad_rate_hz_{250};
    media::DecodedFrame video_frame_;
    bool have_frame_ = false;
    double video_phase_ = 0.0;
    // Playout clock trim: the consumer follows the producer's real rate. A
    // proportional-integral loop on the decoded queue depth keeps it at target.
    double playout_trim_ = 0.0;
    double playout_integral_ = 0.0;
    double playout_clock_ = -1.0;
    int playout_depth_ = 0;
    double probe_age_sum_ = 0.0;
    double probe_age_max_ = 0.0;
    int probe_age_n_ = 0;
    double probe_wait_ms_ = 0.0;
    double probe_frame_ms_ = 0.0;
    double content_fps_ = 0.0;
    double last_age_ms_ = 0.0;
    int content_unique_ = 0;
    double content_window_ = -1.0;
    bool watching_ = false;
    bool quit_ = false;
    bool watch_on_connect_ = false;
    bool watch_fullscreen_ = false;
    double esc_at_ = -1.0;
    bool disconnect_armed_ = false;
    HHOOK keyboard_hook_ = nullptr;
    std::wstring audio_note_;
    std::unique_ptr<brp::media::NvDlssSr> dlss_sr_;
    std::unique_ptr<brp::media::AmdFsr> fsr_;
    bool fsr_ready_ = false;
    bool fsr_failed_ = false;
    int fsr_in_w_ = 0;
    int fsr_in_h_ = 0;
    int fsr_out_w_ = 0;
    int fsr_out_h_ = 0;
    int fsr_quality_ = -1;
    std::string fsr_provider_;
    std::unique_ptr<brp::media::NvDlssFg> dlss_fg_dx12_;
    bool fg_dx12_failed_ = false;
    int fg_dx12_w_ = 0;
    int fg_dx12_h_ = 0;
    bool fg_dx12_reset_ = true;
    struct FgPending {
        HANDLE shared = nullptr;
        bool generated = false;
        double due = 0.0;
    };
    std::deque<FgPending> fg_pending_;
    std::unique_ptr<brp::media::NvReflex> reflex_;
    int reflex_applied_ = -1;
    bool reflex_failed_ = false;
    void* reflex_device_ = nullptr;
    bool dlss_ready_ = false;
    bool dlss_failed_ = false;
    int dlss_in_w_ = 0;
    int dlss_in_h_ = 0;
    std::vector<std::uint8_t> sr_scaled_;
    std::vector<std::uint16_t> sr_scale_temp_;
    std::unique_ptr<DlssFg> dlss_fg_;
    std::unique_ptr<brp::media::NvDlssnr> dlssnr_;
    bool nr_ready_ = false;
    bool nr_failed_ = false;
    int nr_w_ = 0;
    int nr_h_ = 0;
    double nr_ms_ = 0.0;
    bool EnsureNr(int width, int height);
    void StopNr();
    void EnhanceNeural(media::DecodedFrame& frame);
    bool NeuralInWorkerWanted() const;
    // Frame buffer 0 is the explicit lowest-latency mode; 1+ keeps vsync.
    bool PresentVsync() const;
    bool dlss_fg_failed_ = false;
    bool fg_covering_ = false;
    int fg_actual_mult_ = 0;
    double fg_fps_ = 0.0;
    unsigned long long fg_dropped_ = 0;
    // Engine mode: no legacy shell window, commands from the Rust shell over
    // the named pipe. The stream window and the in-game overlay stay here.
    bool engine_mode_ = false;
    unsigned short bridge_port_ = 0;
    UiBridge bridge_;
    UpdateService update_;
    UpdatePhase last_update_phase_ = UpdatePhase::Idle;
    // Link type of the default route, refreshed every few seconds: Wi-Fi needs
    // a much lower bitrate than Ethernet before it starts queuing packets.
    std::string link_type_ = "unknown";
    int link_ticks_ = 39;  // compute on the first status publish
    int stats_ticks_ = 0;  // presentation stats line every ~5 s at Debug
    double bridge_status_at_ = -1.0;
    bool publish_devices_ = false;
    bool overlay_pause_fg_ = false;
    bool publish_profiles_ = false;
    void PublishProfiles();
    void ApplyProfileById(const std::string& id);
    void SaveProfileByName(const std::string& name);
    void DeleteProfileById(const std::string& id);
    void HandleBridgeCommand(const std::string& command);
    void PublishBridgeStatus();
    void PublishDevices();
    void PublishConsoles();
    std::string consoles_sig_;
    void ReloadSettingsFromDisk();
    OverlayWindow overlay_;
    double last_overlay_update_ = -1.0;
    bool overlay_control_ = false;
    int overlay_tab_ = 0;
    std::wstring overlay_status_;
    bool overlay_status_error_ = false;
    ApplyScope last_apply_scope_ = ApplyScope::Immediate;
    bool stream_restart_pending_ = false;

    std::atomic<int> display_shown_{0};
    RollingStats video_intervals_{512};
    RollingStats loop_intervals_{512};
    double last_video_shown_at_ = -1.0;
    double cadence_p50_ = 0.0;
    double cadence_p99_ = 0.0;
    double loop_p99_ = 0.0;
    unsigned long long present_misses_seen_ = 0;
    unsigned long long present_misses_rate_ = 0;
    unsigned present_refresh_seen_ = 0;
    double present_refresh_hz_ = 0.0;
    unsigned last_source_refresh_ = 0;
    std::array<unsigned, 32> hold_counts_{};
    std::array<unsigned, 32> hold_seen_{};
    unsigned hold_total_ = 0;
    bool content_changed_ = false;
    // Set when a frame (or a fresh background after a resize) was drawn into
    // the stream backbuffer. Present() is skipped otherwise: with the vblank
    // gate gone, presenting an untouched backbuffer with a flip chain would
    // show the frame from two presents ago.
    bool present_frame_ = false;
    bool redraw_needed_ = false;
    double stats_at_ = -1.0;
    double sr_cpu_ms_ = 0.0;
    int display_shown_last_ = 0;
    int display_rate_ = 0;
    bool user_disconnect_ = false;
    bool connect_requested_ = false;
    double reconnect_at_ = -1.0;
    int reconnect_attempts_ = 0;
    int applied_render_target_ = -1;

    struct BenchResult {
        int requested_fps = 0;
        int target = 0;
        bool ok = false;
        double source_fps = 0.0;
        double shown_fps = 0.0;
        double decode_ms = 0.0;
        double loss_percent = 0.0;
        double ui_p50 = 0.0;
        double ui_p95 = 0.0;
        double ui_p99 = 0.0;
        std::uint64_t generated = 0;
    };
    bool bench_active_ = false;
    bool bench_exit_ = false;
    int bench_case_ = 0;
    int bench_phase_ = 0;
    double bench_at_ = 0.0;
    std::uint64_t bench_shown_start_ = 0;
    double bench_source_sum_ = 0.0;
    double bench_decode_sum_ = 0.0;
    int bench_samples_ = 0;
    std::vector<BenchResult> bench_results_;
    AppSettings bench_saved_;
};

}  // namespace brp::next
