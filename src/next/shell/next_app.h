#pragma once

#include "app/app_host.h"
#include "app/runtime_settings.h"
#include "app/navigation.h"
#include "audio/audio_engine.h"
#include "core/models.h"
#include "media/decoder.h"
#include "overlay/overlay_window.h"
#include "platform/gamepad.h"
#include "platform/system_info.h"
#include "ui/theme.h"
#include "ui/widgets.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace brp {
class Win32Window;
class Presenter;
namespace ui {
class Renderer;
}
namespace media {
class NvDlssSr;
class Rife;
}
}  // namespace brp

namespace brp::next {

enum class OverlayAction;
class SettingsControls;
class StreamingService;
class DlssFg;

class NextApp {
public:
    NextApp();
    ~NextApp();

    int Run();

private:
    bool Init();
    void Resize();
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
    void RenderWatchOverlay();
    void PresentVideo();
    void StartPadThread();
    void StopPadThread();
    void ToggleWatch(bool watching);
    bool OpenAudio();
    bool EnsureDlssSr(int input_width, int input_height);
    bool EnsureDlssFg(int width, int height);
    void StopDlssFg();
    bool EnsureRife();
    void StartRifeThread();
    void StopRifeThread();
    void RifeWorkerMain();
    void ScheduleRifeFrame(const media::DecodedFrame& frame);
    void PushDisplayFrame(media::DecodedFrame frame, double due);
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
    bool Apply();
    void RefreshProfiles();
    void Toast(const std::wstring& text, bool error = false);
    void ProbeIfNeeded(double now);
    void HandlePinInput();
    std::wstring ClipboardText() const;
    std::wstring W(const std::string& text) const;
    std::wstring Summary(const Profile& profile) const;
    std::wstring ProfileWarning(const Profile& profile) const;

    std::unique_ptr<brp::Win32Window> window_;
    std::unique_ptr<brp::Presenter> presenter_;
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
    media::DecodedFrame video_frame_;
    bool have_frame_ = false;
    bool watching_ = false;
    bool watch_on_connect_ = false;
    bool watch_fullscreen_ = false;
    double esc_at_ = -1.0;
    double video_phase_ = 0.0;
    double last_phase_at_ = -1.0;
    std::wstring audio_note_;
    std::unique_ptr<brp::media::NvDlssSr> dlss_sr_;
    bool dlss_ready_ = false;
    bool dlss_failed_ = false;
    int dlss_in_w_ = 0;
    int dlss_in_h_ = 0;
    std::unique_ptr<DlssFg> dlss_fg_;
    bool dlss_fg_failed_ = false;
    bool fg_covering_ = false;
    int fg_actual_mult_ = 0;
    double fg_fps_ = 0.0;
    unsigned long long fg_dropped_ = 0;
    OverlayWindow overlay_;
    double last_overlay_update_ = -1.0;
    bool overlay_control_ = false;
    int overlay_tab_ = 0;
    std::wstring overlay_status_;
    bool overlay_status_error_ = false;
    ApplyScope last_apply_scope_ = ApplyScope::Immediate;
    bool stream_restart_pending_ = false;

    struct DisplayFrame {
        media::DecodedFrame frame;
        double due = 0.0;
        bool generated = false;
    };
    struct RifeJob {
        std::vector<uint8_t> a;
        std::vector<uint8_t> b;
        double mid_due = 0.0;
        bool pq = false;
        float blend = 0.0f;
        bool motion_safety = true;
    };
    std::unique_ptr<brp::media::Rife> rife_;
    std::thread rife_thread_;
    std::mutex rife_mutex_;
    std::condition_variable rife_cv_;
    std::deque<RifeJob> rife_jobs_;
    std::deque<DisplayFrame> display_queue_;
    std::mutex display_mutex_;
    std::vector<uint8_t> rife_prev_;
    int rife_prev_w_ = 0;
    int rife_prev_h_ = 0;
    bool rife_prev_pq_ = false;
    bool rife_prev_valid_ = false;
    bool rife_prev_pushed_ = false;
    bool rife_ready_ = false;
    std::atomic<bool> rife_failed_{false};
    std::atomic<bool> rife_run_{false};
    double rife_interval_ = 1.0 / 30.0;
    media::DecodedFrame rife_last_shown_;
    bool rife_have_shown_ = false;
    std::atomic<int> rife_generated_{0};
    int rife_generated_last_ = 0;
    int rife_generated_rate_ = 0;
    double rife_generated_at_ = -1.0;
    std::atomic<int> display_shown_{0};
    RollingStats video_intervals_{512};
    double last_video_shown_at_ = -1.0;
    double cadence_p50_ = 0.0;
    double cadence_p99_ = 0.0;
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
