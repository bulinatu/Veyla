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
// its shared-memory protocol directly: RGBA in, RGBA out, 1920x1080.
class NvDlssnr {
public:
    NvDlssnr() = default;
    ~NvDlssnr();

    NvDlssnr(const NvDlssnr&) = delete;
    NvDlssnr& operator=(const NvDlssnr&) = delete;

    // worker_exe is the nvngx.dll worker; working_dir is the folder that
    // contains it and nvngx_dlssnr.dll.
    bool Start(const std::wstring& worker_exe, const std::wstring& working_dir, int width,
               int height, int style, float tone, float structure, float skin, bool mask,
               bool ui, float intensity, std::string& detail);

    bool Process(const uint8_t* rgba, int width, int height, std::vector<uint8_t>& out,
                 std::string& detail);

    bool ready() const { return ready_; }
    double last_ms() const { return last_ms_; }
    const std::string& status() const { return status_; }
    int width() const { return width_; }
    int height() const { return height_; }
    int frame_count() const { return frames_; }

    void Stop();

private:
    bool ReadExact(void* data, size_t bytes, DWORD timeout_ms, std::string& detail);
    bool WriteAll(const void* data, size_t bytes, std::string& detail);
    bool WaitForMagic(uint32_t expected, uint32_t* extra, std::string& detail);
    bool ReadOutput(std::string& detail);
    void CloseHandles();

    bool ready_ = false;
    int width_ = 0;
    int height_ = 0;
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
