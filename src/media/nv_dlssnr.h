#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace brp::media {

// DLSS Neural Rendering through the vendor NGX NR worker (NeuralScreen).
//
// The worker is an executable named nvngx.dll (with nvngx_dlssnr.dll beside
// it). It owns the real multi-pass NGX feature-18 pipeline; this class speaks
// its shared-memory protocol directly: RGBA in, RGBA out.
//
// Two size pairs matter:
//   work  - the resolution the network evaluates at (Boost can lower it);
//   full  - the frame that goes in and comes out. With work < full the worker
//           resizes on the GPU (NGX Upscaling), so callers always see full
//           frames and only the cost changes.
class NvDlssnr {
public:
    struct Options {
        int style = 2;              // 0 default, 1 natural, 2 cinematic
        float intensity = 1.0f;     // 0..2
        float local_tone = 1.0f;    // 0..2
        float local_structure = 0.5f;
        float skin_structure = 0.75f;
        bool auto_mask = false;
        bool ui_correction = false;
        int passes = 1;             // 1..4, the cascade depth (Boost)
        float work_scale = 1.0f;    // 1.0 = full-res network, < 1 = Boost
        // Inject the user-supplied DLSSG-SM86 proxy. It raises the multiplier
        // ceiling above 2x, but replaces the official runtime. Callers that
        // only need 2x can keep the NVIDIA-signed runtime for its numerics.
        bool inject_fg_proxy = true;
    };

    NvDlssnr() = default;
    ~NvDlssnr();

    NvDlssnr(const NvDlssnr&) = delete;
    NvDlssnr& operator=(const NvDlssnr&) = delete;

    // worker_exe is the nvngx.dll worker; working_dir is the folder that
    // contains it and nvngx_dlssnr.dll. width/height are the full frame size.
    bool Start(const std::wstring& worker_exe, const std::wstring& working_dir, int width,
               int height, const Options& options, std::string& detail);

    // RNSZ: new settings and/or a new work size inside the running worker.
    // Must not be called while Process() is running.
    bool Reconfigure(const Options& options, std::string& detail);

    // WNDO: the worker presents (and, with FG, generates) frames in its own
    // window instead of returning pixels. DLSS-G lives there.
    bool EnablePresent(int width, int height, std::string& detail);
    void DisablePresent();

    // DLSS Frame Generation inside the worker: 0 off, else the multiplier.
    void SetFrameGeneration(int multiplier) { fg_multiplier_ = multiplier; }    // FRAME_FLAG_BYPASS: the worker presents the raw frame (FG can still run).
    void SetNeuralBypass(bool bypass) { nr_bypass_ = bypass; }

    bool Process(const uint8_t* rgba, int width, int height, std::vector<uint8_t>& out,
                 std::string& detail);

    bool ready() const { return ready_; }
    double last_ms() const { return last_ms_; }
    const std::string& status() const { return status_; }
    int width() const { return full_w_; }
    int height() const { return full_h_; }
    int work_width() const { return work_w_; }
    int work_height() const { return work_h_; }
    int frame_count() const { return frames_; }
    DWORD worker_pid() const { return worker_pid_; }
    bool present_mode() const { return present_mode_; }

    void Stop();

    // Terminates the child immediately without closing the pipe handles. Safe
    // to call while another thread is blocked in Process(); the failed read
    // wakes it, then Stop() can join and release the handles.
    void TerminateWorker();

private:
    bool ReadExact(void* data, size_t bytes, DWORD timeout_ms, std::string& detail);
    bool WriteAll(const void* data, size_t bytes, std::string& detail);
    bool WaitForMagic(uint32_t expected, uint32_t* extra, std::string& detail);
    bool SendResize(const Options& options, int work_w, int work_h, std::string& detail);
    void CloseHandles();

    bool ready_ = false;
    Options options_{};
    int full_w_ = 0;
    int full_h_ = 0;
    int work_w_ = 0;
    int work_h_ = 0;
    int frames_ = 0;
    double last_ms_ = 0.0;
    bool pending_ = false;
    double submitted_at_ = 0.0;
    bool Submit(const uint8_t* rgba, int width, int height, std::string& detail);
    bool Collect(std::vector<uint8_t>& out, std::string& detail);
    std::string status_;

    HANDLE process_ = nullptr;
    HANDLE job_ = nullptr;
    HANDLE stdin_write_ = nullptr;
    HANDLE stdout_read_ = nullptr;
    HANDLE log_file_ = nullptr;

    HANDLE in_map_ = nullptr;
    HANDLE out_map_ = nullptr;
    uint8_t* in_view_ = nullptr;
    uint8_t* out_view_ = nullptr;
    size_t in_size_ = 0;
    size_t out_size_ = 0;
    uint32_t frame_index_ = 0;
    int fg_multiplier_ = 0;
    bool nr_bypass_ = false;
    bool present_mode_ = false;
    DWORD worker_pid_ = 0;
};

}  // namespace brp::media
