#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>

#include <chiaki/session.h>

#include "audio/audio_engine.h"
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

class Streaming {
public:
    Streaming() = default;
    ~Streaming();

    Streaming(const Streaming&) = delete;
    Streaming& operator=(const Streaming&) = delete;

    // Audio sink target (optional).
    void SetAudio(audio::Engine* engine) { audio_ = engine; }

    bool Start(const Profile& profile, int width, int height, int bitrate, std::string& error);
    void Stop();

    StreamState state() const { return state_.load(); }
    std::string message() const;

    // buffer: how many decoded frames to hold before presenting (jitter buffer).
    bool TakeFrame(media::DecodedFrame& out, int buffer);
    int frame_count() const;
    int audio_frames() const { return audio_frames_.load(); }
    double fps() const;

    void SendController(const PadState& pad);

private:
    static bool VideoCallback(uint8_t* buf, size_t size, int32_t frames_lost, bool recovered,
                              void* user);
    static void EventCallback(ChiakiEvent* event, void* user);
    static void AudioHeaderCallback(ChiakiAudioHeader* header, void* user);
    static void AudioFrameCallback(uint8_t* buf, size_t size, void* user);

    void OnVideo(uint8_t* buf, size_t size, int32_t frames_lost);
    void OnEvent(ChiakiEvent* event);
    void OnAudio(uint8_t* buf, size_t size);

    bool OpenAudioDecoder(std::string& error);

    ChiakiSession session_{};
    ChiakiLog log_{};
    ChiakiConnectInfo info_{};
    media::Decoder decoder_;

    std::string host_storage_;
    std::atomic<StreamState> state_{StreamState::Idle};
    mutable std::mutex mutex_;
    std::string message_;

    std::mutex frame_mutex_;
    std::deque<media::DecodedFrame> frames_;

    std::atomic<int> frame_count_{0};
    std::chrono::steady_clock::time_point started_{};
    bool initialised_ = false;

    // audio
    audio::Engine* audio_ = nullptr;
    void* opus_ctx_ = nullptr;    // AVCodecContext*
    void* opus_frame_ = nullptr;  // AVFrame*
    void* swr_ = nullptr;         // SwrContext*
    std::atomic<int> audio_frames_{0};
};

}  // namespace brp::session
