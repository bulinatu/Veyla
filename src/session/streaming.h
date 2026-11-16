#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>

#include <chiaki/controller.h>
#include <chiaki/orientation.h>
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

    // Adaptive jitter buffer: `buffer` is the maximum number of decoded frames
    // to hold. The target starts at one frame (low latency), rises on an empty
    // queue while streaming, and falls back after a stable period.
    bool TakeFrame(media::DecodedFrame& out, int buffer);
    int buffer_target() const { return buffer_target_.load(); }
    int buffer_drops() const { return buffer_drops_.load(); }

    struct StreamStats {
        int frames = 0;
        int lost = 0;
        double decode_ms_avg = 0.0;
        double decode_ms_max = 0.0;
        double kbps = 0.0;
        int queue = 0;
        int buffer = 0;
        int buffer_drops = 0;
        double elapsed = 0.0;
    };
    // Returns the counters collected since the previous call and resets them.
    StreamStats ConsumeStats();

    int frame_count() const;
    int audio_frames() const { return audio_frames_.load(); }
    double fps() const;
    std::string decoder_name() const { return decoder_.backend_name(); }

    void SendController(const PadState& pad);
    void SetMotionEnabled(bool enabled) { motion_enabled_ = enabled; }

    // Latest state pushed by the console (rumble, trigger effects, LEDs).
    // version() changes only when the console actually sent something.
    PadConsoleFeedback feedback() const;
    uint32_t feedback_version() const { return feedback_version_.load(); }

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

    // adaptive jitter buffer
    std::atomic<int> buffer_target_{1};
    std::atomic<int> buffer_drops_{0};
    int last_queue_ = 0;
    double last_drought_ = -100.0;
    double last_recovery_ = 0.0;

    // per-interval video telemetry
    std::atomic<int> stats_frames_{0};
    std::atomic<int> stats_lost_{0};
    std::atomic<long long> stats_decode_us_{0};
    std::atomic<int> stats_decode_count_{0};
    std::atomic<long long> stats_decode_max_us_{0};
    std::atomic<long long> stats_bytes_{0};
    std::chrono::steady_clock::time_point stats_started_{};

    std::atomic<int> frame_count_{0};
    std::chrono::steady_clock::time_point started_{};
    bool initialised_ = false;

    // controller input
    ChiakiControllerState controller_state_{};
    ChiakiOrientationTracker tracker_{};
    ChiakiAccelNewZero accel_zero_{};
    uint8_t touch_ids_[2] = {0xFF, 0xFF};
    bool touch_active_[2] = {false, false};
    std::atomic<bool> motion_enabled_{true};

    // console feedback
    mutable std::mutex feedback_mutex_;
    PadConsoleFeedback feedback_;
    uint8_t trigger_nibble_ = 0x00;
    uint8_t haptic_nibble_ = 0x00;
    std::atomic<uint32_t> feedback_version_{0};

    // audio
    audio::Engine* audio_ = nullptr;
    void* opus_ctx_ = nullptr;    // AVCodecContext*
    void* opus_frame_ = nullptr;  // AVFrame*
    void* swr_ = nullptr;         // SwrContext*
    std::atomic<int> audio_frames_{0};
};

}  // namespace brp::session
