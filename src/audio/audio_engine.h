#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace brp::audio {

struct DeviceInfo {
    std::string id;
    std::string name;
    bool is_default = false;
    bool is_controller = false;  // name heuristic: DualSense / Wireless Controller
};

// WASAPI render engine. Everything is in-process: no virtual audio cable, no
// driver, no external DLL. The DualSense's 3.5 mm jack shows up as a normal USB
// audio endpoint over USB, so routing to it is device selection, not a driver.
//
// Producer contract: interleaved stereo float at 48 kHz.
class Engine {
public:
    Engine() = default;
    ~Engine();

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    static std::vector<DeviceInfo> Enumerate();

    // Opens (or reopens) an endpoint. Empty id = system default.
    bool Open(const std::string& device_id, int buffer_ms);
    void Close();

    bool is_open() const { return running_.load(); }
    std::string device_name() const;
    std::string format_text() const;
    int buffer_ms() const { return buffer_ms_; }
    double latency_ms() const;

    int underruns() const { return underruns_.load(); }
    float peak() const { return peak_.load(); }
    uint64_t played_frames() const { return played_frames_.load(); }
    // Queue interleaved stereo frames (48 kHz).
    void Push(const float* interleaved, int frames);
    void PlayTestTone(double seconds);

private:
    void ThreadMain();
    void RenderChunk(float* dst, int frames);
    void ConvertAndWrite(BYTE* dst, const float* source, int source_frames, int out_frames);

    // configuration
    std::string device_id_;
    int buffer_ms_ = 20;

    // worker state
    std::thread thread_;
    HANDLE ready_event_ = nullptr;
    HANDLE render_event_ = nullptr;
    std::atomic<bool> running_{false};
    std::atomic<bool> opened_ok_{false};

    // device description
    mutable std::mutex name_mutex_;
    std::string device_name_;
    std::string format_text_;
    int mix_rate_ = 48000;
    int mix_channels_ = 2;
    bool mix_float_ = true;
    int mix_bits_ = 32;

    // ring buffer (monotonic frame counters, power-of-two masked)
    static constexpr size_t kRingFrames = 1u << 15;  // ~0.68 s at 48 kHz
    std::vector<float> ring_;
    std::atomic<uint64_t> write_frame_{0};
    std::atomic<uint64_t> read_frame_{0};
    std::atomic<uint64_t> dropped_frames_{0};
    std::atomic<uint64_t> played_frames_{0};
    std::atomic<int> underruns_{0};
    std::atomic<float> peak_{0.0f};

    // resampling state for the non-autoconvert fallback
    double src_pos_ = 0.0;
    uint64_t last_data_ms_ = 0;  // render thread only
};

}  // namespace brp::audio
