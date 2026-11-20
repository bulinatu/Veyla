#pragma once

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace brp::media {
class NvDlssnr;
}

namespace brp::next {

// DLSS Frame Generation through the NVIDIA NGX worker (NeuralScreen).
//
// The worker is the only component that can run the real DLSS-G runtime here:
// a video stream has no motion vectors or depth, so the worker feeds the
// feature flat depth and estimated motion and presents the generated frames in
// its own top-level window. This class owns the worker, feeds it RGBA frames at
// the source cadence, and keeps that window fitted over the client area.
//
// It is deliberately a separate object from the presenter: with frame
// generation active the worker owns the picture, and the app's swapchain only
// keeps the loop cadence alive underneath it.
class DlssFg {
public:
    DlssFg();
    ~DlssFg();

    DlssFg(const DlssFg&) = delete;
    DlssFg& operator=(const DlssFg&) = delete;

    // runtime_dir contains nvngx.dll, nvngx_dlssg.dll and (optionally) the
    // fg/winmm.dll SM86 proxy. width/height is the frame the worker receives.
    bool Start(const std::wstring& runtime_dir, int width, int height, int multiplier,
               std::string& detail);
    void Stop();

    // Applies on the worker thread, never from the UI thread.
    void SetMultiplier(int multiplier);

    // Moves the frame into the one-slot queue; the newest frame wins. Moving
    // avoids a second 8 MB copy on the presentation thread.
    void Submit(std::vector<std::uint8_t> rgba, int width, int height);

    // Positions/fits the worker's window over the app client. Returns true when
    // the worker window is visible and therefore covers the app's picture.
    void FrameWindow(HWND app, double now);

    // Hides the worker window without destroying the worker.
    void Hide();

    bool ready() const { return ready_; }
    bool failed() const { return failed_.load(); }
    bool covering() const { return covering_; }
    bool attached() const { return worker_window_ != nullptr; }
    bool proxy_runtime() const { return proxy_runtime_; }
    int width() const { return width_; }
    int height() const { return height_; }
    int requested_multiplier() const { return multiplier_.load(); }
    int actual_multiplier() const;
    double displayed_fps() const;
    unsigned long long dropped() const { return dropped_.load(); }
    DWORD worker_pid() const;
    std::wstring note() const;

private:
    struct Job {
        std::vector<std::uint8_t> rgba;
        int width = 0;
        int height = 0;
    };

    void WorkerMain();
    bool Attach(HWND app);
    void PollLog(double now);
    void CloseWindow();

    std::unique_ptr<brp::media::NvDlssnr> nr_;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::optional<Job> pending_;
    bool run_ = false;

    std::atomic<bool> failed_{false};
    std::atomic<int> multiplier_{2};
    std::atomic<unsigned long long> dropped_{0};
    bool ready_ = false;
    bool proxy_runtime_ = false;
    int width_ = 0;
    int height_ = 0;

    HWND worker_window_ = nullptr;
    bool visible_ = false;
    bool covering_ = false;
    int last_x_ = 0;
    int last_y_ = 0;
    int last_w_ = 0;
    int last_h_ = 0;
    double next_attach_ = 0.0;
    double next_log_ = 0.0;

    double displayed_fps_ = 0.0;
    int actual_multiplier_ = 0;
    mutable std::mutex note_mutex_;
    std::wstring note_;
};

}  // namespace brp::next
