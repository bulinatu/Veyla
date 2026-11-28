#pragma once

#include "core/interfaces.h"
#include "core/models.h"
#include "media/decoder.h"
#include "session/pairing.h"
#include "session/profile.h"
#include "session/streaming.h"

namespace brp::audio {
class Engine;
}

#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace brp::next {

// Real PS5 session boundary for the new application. It wraps the proven
// chiaki-ng code; no protocol logic is reimplemented here.
class StreamingService final : public IStreamingService {
public:
    StreamingService(std::filesystem::path next_profile,
                     std::filesystem::path legacy_profile);
    ~StreamingService();

    StreamingService(const StreamingService&) = delete;
    StreamingService& operator=(const StreamingService&) = delete;

    Status Load();
    bool profile_loaded() const { return loaded_; }

    void SetStreamSettings(const StreamingSettings& settings);
    Status Connect(const ConsoleSettings& console) override;
    void Disconnect() override;
    StreamSnapshot Snapshot() const override;
    Status Pair(std::uint32_t pin) override;
    Status StartDiscovery() override;
    std::vector<ConsoleCandidate> Discovered() const override;
    bool HasCredentials() const override;
    void Poll() override;

    Status SetConsole(const ConsoleCandidate& candidate);
    Status SaveProfile();
    Status OpenPsnLogin(std::string& detail);
    Status CompletePsnLogin(const std::string& redirect, std::string& detail);
    session::Profile Profile() const;
    std::string ProfilePath() const { return next_path_.string(); }

    // Media and input surface used by the Watch view.
    void SetAudio(brp::audio::Engine* engine);
    // Optional shared D3D11 device for zero-copy presentation, and whether the
    // AI path needs CPU RGBA this frame.
    void SetExternalDevice(void* device, void* context) {
        streaming_.SetExternalDevice(device, context);
    }
    void SetCpuPixels(bool cpu) { streaming_.SetCpuPixels(cpu); }
    bool SharedDecodeDevice() const { return streaming_.shared_device(); }
    void SetMotionEnabled(bool enabled);
    bool TakeFrame(media::DecodedFrame& out, int buffer, bool paced);
    bool TakeNewestFrame(media::DecodedFrame& out);
    bool TakeFrameOrdered(media::DecodedFrame& out, int max_queue);
    bool WaitForFrame(int timeout_ms);
    int QueueDepth();
    int BufferTarget() const;
    void SendController(const brp::PadState& state);
    brp::PadConsoleFeedback Feedback() const;
    bool TakeHaptics(std::uint16_t& left, std::uint16_t& right);
    bool Connected() const;
    // Console + encode + network + decode loop measured from a controller change
    // to the first decoded frame (0 until a change has been seen).
    double InputResponseMs() const { return streaming_.input_response_ms(); }
    double InputResponseAvgMs() const { return streaming_.input_response_avg_ms(); }

private:
    void JoinConnect();
    void JoinDiscovery();
    void UpdateStats();

    mutable std::mutex mutex_;
    session::Profile profile_;
    session::Streaming streaming_;
    StreamingSettings stream_options_{};
    session::Pairing pairing_;
    std::thread connect_thread_;
    std::thread discovery_thread_;
    std::atomic<bool> connecting_{false};
    std::atomic<bool> discovering_{false};
    std::vector<ConsoleCandidate> discovered_;
    std::string message_;
    std::string pairing_message_;
    session::Streaming::StreamStats stats_{};
    std::chrono::steady_clock::time_point last_stats_{};
    // Session totals: the disconnect summary is the quickest way to see what a
    // whole run looked like without reading every second.
    double session_seconds_ = 0.0;
    long long session_frames_ = 0;
    long long session_lost_ = 0;
    long long session_drops_ = 0;
    double session_kbps_sum_ = 0.0;
    int session_fps_min_ = 0;
    int session_fps_max_ = 0;
    int session_fps_samples_ = 0;
    // Drop/stutter warnings are throttled so a bad second does not fill the log.
    std::chrono::steady_clock::time_point last_warn_{};
    std::filesystem::path next_path_;
    std::filesystem::path legacy_path_;
    bool loaded_ = false;
};

}  // namespace brp::next
