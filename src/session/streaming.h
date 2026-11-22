#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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
    void SetAudio(audio::Engine* engine) { audio_.store(engine); }

    bool Start(const Profile& profile, int width, int height, int bitrate, std::string& error);
    void Stop();

    StreamState state() const { return state_.load(); }
    std::string message() const;

    // Adaptive jitter buffer: `buffer` is the maximum number of decoded frames
    // to hold. The target starts at one frame (low latency), rises on an empty
    // queue while streaming, and falls back after a stable period.
    // `paced` is used when the caller already decides the display cadence: the
    // frame is taken from the front and at most two frames of latency are kept.
    bool TakeFrame(media::DecodedFrame& out, int buffer, bool paced = false);
    // Pops the oldest decoded frame immediately, keeping every frame in order.
    // Used for frame generation, where a skipped input slot starves the
    // generator and shows up as a dropped output frame.
    // Pops the newest decoded frame and discards older ones. Used by the
    // frame generator, where a stale queued frame is pure latency.
    bool TakeNewestFrame(media::DecodedFrame& out);
    // Pops the oldest decoded frame, preserving order. Frames are dropped only
    // when the backlog exceeds `max_queue`, so a short network burst is played
    // out instead of skipped. Used by the generator feed and the video path.
    bool TakeFrameOrdered(media::DecodedFrame& out, int max_queue);
    bool TakeFrameNow(media::DecodedFrame& out);
    // Blocks until a decoded frame is queued or the timeout expires. Used by
    // the frame generator so it wakes on a frame instead of polling.
    bool WaitForFrame(int timeout_ms);
    int buffer_target() const { return buffer_target_.load(); }
    int queue_depth();
    int buffer_drops() const { return buffer_drops_.load(); }
    int video_queue_drops() const { return video_queue_drops_.load(); }

    struct StreamStats {
        int frames = 0;
        int lost = 0;
        double decode_ms_avg = 0.0;
        double decode_ms_max = 0.0;
        double kbps = 0.0;
        int queue = 0;
        int buffer = 0;
        int buffer_drops = 0;
        // Interval between decoded frames: this is the source cadence the
        // presentation grid has to follow. `arrival_outliers` counts frames
        // that arrived more than ~3.5 ms off the nominal 16.7 ms.
        double arrival_ms = 0.0;
        int arrival_outliers = 0;
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

    // Haptics: the PS5 sends 3 kHz stereo int16 on its own sink. Returns true
    // when a new frame arrived, with the average |sample| per channel.
    bool TakeHaptics(uint16_t& left, uint16_t& right);

private:
    static bool VideoCallback(uint8_t* buf, size_t size, int32_t frames_lost, bool recovered,
                              void* user);
    static void EventCallback(ChiakiEvent* event, void* user);
    static void AudioHeaderCallback(ChiakiAudioHeader* header, void* user);
    static void AudioFrameCallback(uint8_t* buf, size_t size, void* user);
    static void HapticsFrameCallback(uint8_t* buf, size_t size, void* user);

    void OnVideo(uint8_t* buf, size_t size, int32_t frames_lost);
    void OnEvent(ChiakiEvent* event);
    void OnAudio(uint8_t* buf, size_t size);
    void OnHaptics(uint8_t* buf, size_t size);

    // Compressed access units are copied off chiaki's receive thread and
    // decoded here. Hardware-to-system transfer plus swscale costs several
    // milliseconds per frame; doing that inline delayed RTP reads and turned
    // decoder spikes into packet loss.
    void DecodeThreadMain();

    bool OpenAudioDecoder(std::string& error);

    ChiakiSession session_{};
    ChiakiLog log_{};
    ChiakiConnectInfo info_{};
    media::Decoder decoder_;

    // Serialises session start/stop against controller sends from the pad
    // thread. Without it, a disconnect could tear the session down while the
    // 250 Hz pad loop is inside chiaki_session_set_controller_state.
    std::mutex api_mutex_;

    std::string host_storage_;
    std::atomic<StreamState> state_{StreamState::Idle};
    mutable std::mutex mutex_;
    std::string message_;

    std::mutex frame_mutex_;
    std::condition_variable frame_cv_;
    std::deque<media::DecodedFrame> frames_;

    // Decode pipeline: encoded units in, decoded frames out.
    struct EncodedUnit {
        std::vector<uint8_t> bytes;
    };
    std::thread decode_thread_;
    std::mutex video_mutex_;
    std::condition_variable video_cv_;
    std::deque<EncodedUnit> video_queue_;
    bool video_run_ = false;
    std::atomic<bool> session_live_{false};
    std::atomic<int> video_queue_drops_{0};

    // adaptive jitter buffer
    std::atomic<int> buffer_target_{1};
    std::atomic<int> buffer_drops_{0};
    bool primed_ = false;
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
    std::atomic<long long> stats_arrival_us_{0};
    std::atomic<int> stats_arrival_count_{0};
    std::atomic<int> stats_arrival_outliers_{0};
    std::chrono::steady_clock::time_point last_decoded_at_{};
    bool have_last_decoded_ = false;
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

    // haptics (separate 3 kHz stream)
    mutable std::mutex haptics_mutex_;
    uint16_t haptics_left_ = 0;
    uint16_t haptics_right_ = 0;
    std::atomic<uint32_t> haptics_seq_{0};
    uint32_t haptics_seen_ = 0;

    // audio
    std::atomic<audio::Engine*> audio_{nullptr};
    void* opus_ctx_ = nullptr;    // AVCodecContext*
    void* opus_frame_ = nullptr;  // AVFrame*
    void* swr_ = nullptr;         // SwrContext*
    std::vector<float> pcm_scratch_;
    std::atomic<int> audio_frames_{0};
};

}  // namespace brp::session
