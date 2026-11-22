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
    void SetMotionEnabled(bool enabled);
    bool TakeFrame(media::DecodedFrame& out, int buffer, bool paced);
    bool TakeNewestFrame(media::DecodedFrame& out);
    bool TakeFrameOrdered(media::DecodedFrame& out, int max_queue);
    bool WaitForFrame(int timeout_ms);
    void SendController(const brp::PadState& state);
    brp::PadConsoleFeedback Feedback() const;
    bool TakeHaptics(std::uint16_t& left, std::uint16_t& right);
    bool Connected() const;

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
    std::filesystem::path next_path_;
    std::filesystem::path legacy_path_;
    bool loaded_ = false;
};

}  // namespace brp::next
