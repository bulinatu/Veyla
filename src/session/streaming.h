#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>

#include <chiaki/session.h>

#include "media/decoder.h"
#include "platform/gamepad.h"
#include "session/profile.h"

namespace brp::session {

enum class StreamState {
    Idle,
    Connecting,
    Streaming,
    Failed,
};

// Live Remote Play session: chiaki's transport + our FFmpeg decoder.
class Streaming {
public:
    Streaming() = default;
    ~Streaming();

    Streaming(const Streaming&) = delete;
    Streaming& operator=(const Streaming&) = delete;

    bool Start(const Profile& profile, int width, int height, int bitrate, std::string& error);
    void Stop();

    StreamState state() const { return state_.load(); }
    std::string message() const;

    // Newest decoded frame, if a new one arrived since the last call.
    bool TakeFrame(media::DecodedFrame& out);

    int frame_count() const;
    double fps() const;

    void SendController(const PadState& pad);

private:
    static bool VideoCallback(uint8_t* buf, size_t size, int32_t frames_lost, bool recovered,
                              void* user);
    static void EventCallback(ChiakiEvent* event, void* user);

    void OnVideo(uint8_t* buf, size_t size, int32_t frames_lost);
    void OnEvent(ChiakiEvent* event);

    ChiakiSession session_{};
    ChiakiLog log_{};
    ChiakiConnectInfo info_{};
    media::Decoder decoder_;

    std::string host_storage_;
    std::atomic<StreamState> state_{StreamState::Idle};
    mutable std::mutex mutex_;
    std::string message_;

    std::mutex frame_mutex_;
    media::DecodedFrame latest_;
    bool has_new_frame_ = false;

    std::atomic<int> frame_count_{0};
    std::chrono::steady_clock::time_point started_{};
    bool initialised_ = false;
};

}  // namespace brp::session
