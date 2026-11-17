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

    void Stop();

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
};

}  // namespace brp::media
