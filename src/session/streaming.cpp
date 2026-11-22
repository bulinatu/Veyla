#include "session/streaming.h"

#include <chiaki/base64.h>
#include <chiaki/common.h>
#include <chiaki/time.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace brp::session {
namespace {

void LogCallback(ChiakiLogLevel level, const char* msg, void* user) {
    (void)user;
    if (!msg) {
        return;
    }
    const char* tag = "debug";
    switch (level) {
        case CHIAKI_LOG_ERROR: tag = "error"; break;
        case CHIAKI_LOG_WARNING: tag = "warn"; break;
        case CHIAKI_LOG_INFO: tag = "info"; break;
        default: break;
    }
    std::fprintf(stderr, "[stream:%s] %s\n", tag, msg);
}

uint32_t MapButtons(uint32_t buttons) {
    uint32_t out = 0;
    if (buttons & PadCross) out |= CHIAKI_CONTROLLER_BUTTON_CROSS;
    if (buttons & PadCircle) out |= CHIAKI_CONTROLLER_BUTTON_MOON;
    if (buttons & PadSquare) out |= CHIAKI_CONTROLLER_BUTTON_BOX;
    if (buttons & PadTriangle) out |= CHIAKI_CONTROLLER_BUTTON_PYRAMID;
    if (buttons & PadL1) out |= CHIAKI_CONTROLLER_BUTTON_L1;
    if (buttons & PadR1) out |= CHIAKI_CONTROLLER_BUTTON_R1;
    if (buttons & PadL3) out |= CHIAKI_CONTROLLER_BUTTON_L3;
    if (buttons & PadR3) out |= CHIAKI_CONTROLLER_BUTTON_R3;
    if (buttons & PadShare) out |= CHIAKI_CONTROLLER_BUTTON_SHARE;
    if (buttons & PadOptions) out |= CHIAKI_CONTROLLER_BUTTON_OPTIONS;
    if (buttons & PadPS) out |= CHIAKI_CONTROLLER_BUTTON_PS;
    if (buttons & PadTouchpad) out |= CHIAKI_CONTROLLER_BUTTON_TOUCHPAD;
    if (buttons & PadUp) out |= CHIAKI_CONTROLLER_BUTTON_DPAD_UP;
    if (buttons & PadDown) out |= CHIAKI_CONTROLLER_BUTTON_DPAD_DOWN;
    if (buttons & PadLeft) out |= CHIAKI_CONTROLLER_BUTTON_DPAD_LEFT;
    if (buttons & PadRight) out |= CHIAKI_CONTROLLER_BUTTON_DPAD_RIGHT;
    return out;
}

}  // namespace

Streaming::~Streaming() {
    Stop();
}

bool Streaming::OpenAudioDecoder(std::string& error) {
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_OPUS);
    if (!codec) {
        error = "no opus decoder";
        return false;
    }
    auto* ctx = avcodec_alloc_context3(codec);
    if (!ctx) {
        error = "cannot allocate opus context";
        return false;
    }
    ctx->sample_rate = 48000;
    if (avcodec_open2(ctx, codec, nullptr) < 0) {
        avcodec_free_context(&ctx);
        error = "cannot open opus decoder";
        return false;
    }
    opus_ctx_ = ctx;

    auto* frame = av_frame_alloc();
    if (!frame) {
        error = "cannot allocate opus frame";
        return false;
    }
    opus_frame_ = frame;
    return true;
}

bool Streaming::Start(const Profile& profile, int width, int height, int bitrate,
                      std::string& error) {
    Stop();

    if (!profile.has_console() || profile.needs_pairing()) {
        error = "console is not paired";
        return false;
    }

    static bool lib_ready = false;
    if (!lib_ready) {
        if (chiaki_lib_init() != CHIAKI_ERR_SUCCESS) {
            error = "chiaki_lib_init failed";
            return false;
        }
        lib_ready = true;
    }

    chiaki_log_init(&log_, CHIAKI_LOG_ERROR | CHIAKI_LOG_WARNING | CHIAKI_LOG_INFO, &LogCallback,
                    this);

    if (audio_) {
        std::string audio_error;
        if (!OpenAudioDecoder(audio_error)) {
            std::fprintf(stderr, "[stream] audio disabled: %s\n", audio_error.c_str());
        }
    }

    host_storage_ = profile.host;
    std::memset(&info_, 0, sizeof(info_));
    info_.ps5 = profile.device_type != "PS4";
    info_.host = host_storage_.c_str();
    std::memcpy(info_.regist_key, profile.credentials.rp_regist_key.data(),
                sizeof(info_.regist_key));
    std::memcpy(info_.morning, profile.credentials.rp_key.data(), sizeof(info_.morning));

    // The requested stream rate follows the profile. This used to be hardcoded
    // to 60, which is why a 30 fps setting still arrived at 60 with duplicated
    // frames. The console may ignore the hint; the duplicate detector covers
    // that case.
    chiaki_connect_video_profile_preset(
        &info_.video_profile, CHIAKI_VIDEO_RESOLUTION_PRESET_1080p,
        profile.fps >= 50 ? CHIAKI_VIDEO_FPS_PRESET_60 : CHIAKI_VIDEO_FPS_PRESET_30);
    if (width > 1920 || height > 1080) {
        // The console's Remote Play encoder advertises 1920x1080 as its only
        // profile and aborts the stream ("AvCap failed to initialize video")
        // for anything larger. 1440p/4K is a client-side upscale, not a stream.
        std::fprintf(stderr,
                     "[stream] %dx%d requested; Remote Play caps at 1920x1080, clamping\n",
                     width, height);
        width = 1920;
        height = 1080;
    }
    if (width > 0 && height > 0) {
        info_.video_profile.width = static_cast<unsigned>(width);
        info_.video_profile.height = static_cast<unsigned>(height);
    }
    info_.video_profile.bitrate = static_cast<unsigned>(bitrate);
    info_.video_profile.codec = CHIAKI_CODEC_H265;
    info_.video_profile_auto_downgrade = true;
    info_.enable_dualsense = true;
    info_.enable_keyboard = false;
    info_.auto_regist = false;
    info_.packet_loss_max = 0.05;
    info_.enable_idr_on_fec_failure = true;

    std::vector<uint8_t> account(8, 0);
    size_t account_size = account.size();
    if (!profile.psn_account_id.empty()) {
        chiaki_base64_decode(profile.psn_account_id.c_str(), profile.psn_account_id.size(),
                             account.data(), &account_size);
    }
    std::memcpy(info_.psn_account_id, account.data(), 8);

    std::string decoder_error;
    decoder_.SetBackend(static_cast<media::DecodeBackend>(
        std::clamp(profile.decoder, 0, 6)));
    decoder_.SetChromaQuality(std::clamp(profile.chroma, 0, 2));
    if (!decoder_.OpenStream(media::StreamCodec::Hevc, decoder_error)) {
        error = "decoder: " + decoder_error;
        return false;
    }

    if (chiaki_session_init(&session_, &info_, &log_) != CHIAKI_ERR_SUCCESS) {
        error = "session init failed";
        decoder_.Close();
        return false;
    }

    chiaki_session_set_video_sample_cb(&session_, &Streaming::VideoCallback, this);
    chiaki_session_set_event_cb(&session_, &Streaming::EventCallback, this);

    if (opus_ctx_) {
        ChiakiAudioSink sink{};
        sink.user = this;
        sink.header_cb = &Streaming::AudioHeaderCallback;
        sink.frame_cb = &Streaming::AudioFrameCallback;
        chiaki_session_set_audio_sink(&session_, &sink);
    }

    // PS5 haptics arrive as their own 3 kHz audio stream. Without this sink the
    // frames are dropped and a DualSense never vibrates in PS5 games (classic
    // CHIAKI_EVENT_RUMBLE is a PS4-era path).
    {
        ChiakiAudioSink haptics{};
        haptics.user = this;
        haptics.header_cb = nullptr;
        haptics.frame_cb = &Streaming::HapticsFrameCallback;
        chiaki_session_set_haptics_sink(&session_, &haptics);
    }

    if (chiaki_session_start(&session_) != CHIAKI_ERR_SUCCESS) {
        error = "session start failed";
        chiaki_session_fini(&session_);
        decoder_.Close();
        return false;
    }

    initialised_ = true;
    started_ = std::chrono::steady_clock::now();
    stats_started_ = started_;
    frame_count_.store(0);
    audio_frames_.store(0);
    buffer_target_.store(1);
    buffer_drops_.store(0);
    primed_ = false;
    last_drought_ = -100.0;
    last_recovery_ = 0.0;
    last_queue_ = 0;
    stats_frames_.store(0);
    stats_lost_.store(0);
    stats_decode_us_.store(0);
    stats_decode_count_.store(0);
    stats_decode_max_us_.store(0);
    stats_bytes_.store(0);
    stats_arrival_us_.store(0);
    stats_arrival_count_.store(0);
    stats_arrival_outliers_.store(0);
    have_last_decoded_ = false;
    {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        frames_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(video_mutex_);
        video_run_ = true;
        video_queue_.clear();
        video_queue_drops_.store(0);
    }
    session_live_.store(true);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        message_ = "Connecting to " + profile.host;
    }
    state_.store(StreamState::Connecting);
    // Spawn last: the decode thread updates state_/message_ when the first
    // frame is ready, so the Connecting state must already be in place.
    decode_thread_ = std::thread(&Streaming::DecodeThreadMain, this);
    return true;
}

void Streaming::Stop() {
    session_live_.store(false);

    // Stop the decode thread first: it references decoder_ and may be in a
    // chiaki IDR request. Draining it before session teardown removes both
    // races.
    {
        std::lock_guard<std::mutex> lock(video_mutex_);
        video_run_ = false;
        video_queue_.clear();
    }
    video_cv_.notify_all();
    if (decode_thread_.joinable()) {
        decode_thread_.join();
    }

    std::lock_guard<std::mutex> api(api_mutex_);
    if (initialised_) {
        // Tell the console "no buttons held" explicitly, and give the feedback
        // thread one send interval to deliver it before the sockets close.
        // Without this, disconnecting while holding a trigger can leave the
        // console acting on a stuck input until its own timeout.
        ChiakiControllerState idle;
        chiaki_controller_state_set_idle(&idle);
        chiaki_session_set_controller_state(&session_, &idle);
        std::this_thread::sleep_for(std::chrono::milliseconds(24));

        // A polite FIN leaves the pairing's session reserved on the console
        // for 1-2 minutes, and reconnects during that window are refused.
        // Setting SO_LINGER(0) turns the control-socket close into a RST,
        // which frees the session immediately (verified behaviour, see
        // docs/09-session.md).
        if (!CHIAKI_SOCKET_IS_INVALID(session_.ctrl.sock)) {
            linger reset{};
            reset.l_onoff = 1;
            reset.l_linger = 0;
            ::setsockopt(session_.ctrl.sock, SOL_SOCKET, SO_LINGER,
                         reinterpret_cast<const char*>(&reset), sizeof(reset));
        }

        chiaki_session_stop(&session_);
        chiaki_session_join(&session_);
        chiaki_session_fini(&session_);
        initialised_ = false;
    }
    decoder_.Close();

    if (swr_) {
        swr_free(reinterpret_cast<SwrContext**>(&swr_));
    }
    if (opus_frame_) {
        av_frame_free(reinterpret_cast<AVFrame**>(&opus_frame_));
    }
    if (opus_ctx_) {
        avcodec_free_context(reinterpret_cast<AVCodecContext**>(&opus_ctx_));
    }

    state_.store(StreamState::Idle);

    {
        std::lock_guard<std::mutex> lock(feedback_mutex_);
        feedback_ = PadConsoleFeedback{};
        trigger_nibble_ = 0;
        haptic_nibble_ = 0;
        feedback_version_.store(0);
    }
    {
        std::lock_guard<std::mutex> lock(haptics_mutex_);
        haptics_left_ = 0;
        haptics_right_ = 0;
        haptics_seq_.store(0);
        haptics_seen_ = 0;
    }
    chiaki_controller_state_set_idle(&controller_state_);
    chiaki_orientation_tracker_init(&tracker_);
    chiaki_accel_new_zero_set_inactive(&accel_zero_, false);
    touch_ids_[0] = touch_ids_[1] = 0xFF;
    touch_active_[0] = touch_active_[1] = false;

    std::lock_guard<std::mutex> lock(mutex_);
    message_.clear();
}

bool Streaming::VideoCallback(uint8_t* buf, size_t size, int32_t frames_lost, bool recovered,
                              void* user) {
    (void)recovered;
    static_cast<Streaming*>(user)->OnVideo(buf, size, frames_lost);
    return true;
}

void Streaming::OnVideo(uint8_t* buf, size_t size, int32_t frames_lost) {
    stats_lost_.fetch_add(frames_lost > 0 ? frames_lost : 0);
    stats_bytes_.fetch_add(static_cast<long long>(size));
    if (!buf || size == 0) {
        return;
    }

    // Keep this callback short: it runs on chiaki's RTP receive thread. The
    // decode (hw transfer + swscale) happens on DecodeThreadMain.
    {
        std::lock_guard<std::mutex> lock(video_mutex_);
        if (!video_run_) {
            return;
        }
        // Bound latency: if the decoder falls behind, drop the oldest encoded
        // unit instead of building an ever-growing backlog. Dropping compressed
        // data breaks the reference chain, so ask the console for a fresh IDR.
        if (video_queue_.size() >= 6) {
            video_queue_.pop_front();
            video_queue_drops_.fetch_add(1);
            if (session_live_.load()) {
                chiaki_session_request_idr(&session_);
            }
        }
        EncodedUnit unit;
        unit.bytes.assign(buf, buf + size);
        video_queue_.push_back(std::move(unit));
    }
    video_cv_.notify_one();
}

void Streaming::DecodeThreadMain() {
    // Keep the producer one step ahead of presentation. A default-priority
    // decode thread loses its slice to ordinary background work exactly when a
    // hitch would be visible as a held frame.
    ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    for (;;) {
        EncodedUnit unit;
        {
            std::unique_lock<std::mutex> lock(video_mutex_);
            video_cv_.wait(lock, [this] { return !video_run_ || !video_queue_.empty(); });
            if (!video_run_ && video_queue_.empty()) {
                return;
            }
            unit = std::move(video_queue_.front());
            video_queue_.pop_front();
        }

        const auto decode_start = std::chrono::steady_clock::now();
        if (!decoder_.Feed(unit.bytes.data(), unit.bytes.size())) {
            if (session_live_.load()) {
                chiaki_session_request_idr(&session_);
            }
            continue;
        }

        media::DecodedFrame frame;
        int decoded = 0;
        while (decoder_.Receive(frame)) {
            const auto arrived = std::chrono::steady_clock::now();
            if (have_last_decoded_) {
                const double dt_ms = std::chrono::duration<double, std::milli>(
                                         arrived - last_decoded_at_)
                                         .count();
                // Ignore pathological gaps (session start, keyframe wait);
                // they are counted as lost/recovery elsewhere.
                if (dt_ms > 4.0 && dt_ms < 50.0) {
                    stats_arrival_us_.fetch_add(
                        static_cast<long long>(std::llround(dt_ms * 1000.0)));
                    stats_arrival_count_.fetch_add(1);
                    if (dt_ms < 13.5 || dt_ms > 20.0) {
                        stats_arrival_outliers_.fetch_add(1);
                    }
                }
            }
            last_decoded_at_ = arrived;
            have_last_decoded_ = true;

            {
                std::lock_guard<std::mutex> lock(frame_mutex_);
                frames_.push_back(std::move(frame));
                while (frames_.size() > 8) {
                    frames_.pop_front();
                }
            }
            frame_cv_.notify_one();
            ++decoded;
        }

        const long long decode_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                        std::chrono::steady_clock::now() - decode_start)
                                        .count();
        stats_decode_us_.fetch_add(decode_us);
        stats_decode_count_.fetch_add(1);
        long long previous_max = stats_decode_max_us_.load();
        while (decode_us > previous_max &&
               !stats_decode_max_us_.compare_exchange_weak(previous_max, decode_us)) {
        }
        if (decoded > 0) {
            frame_count_.fetch_add(decoded);
            stats_frames_.fetch_add(decoded);
            if (state_.load() != StreamState::Streaming) {
                state_.store(StreamState::Streaming);
                std::lock_guard<std::mutex> lock(mutex_);
                message_ = "Streaming 1080p60";
            }
        }
    }
}

void Streaming::EventCallback(ChiakiEvent* event, void* user) {
    static_cast<Streaming*>(user)->OnEvent(event);
}

void Streaming::OnEvent(ChiakiEvent* event) {
    if (!event) {
        return;
    }
    static const bool probe = std::getenv("BRP_PAD_PROBE") != nullptr;
    switch (event->type) {
        case CHIAKI_EVENT_CONNECTED: {
            std::lock_guard<std::mutex> lock(mutex_);
            message_ = "Connected, waiting for video";
            break;
        }
        case CHIAKI_EVENT_QUIT: {
            state_.store(StreamState::Failed);
            std::lock_guard<std::mutex> lock(mutex_);
            message_ = std::string("Session ended: ") + chiaki_quit_reason_string(event->quit.reason);
            break;
        }
        case CHIAKI_EVENT_RUMBLE: {
            std::lock_guard<std::mutex> lock(feedback_mutex_);
            feedback_.rumble = true;
            feedback_.strong = event->rumble.left;
            feedback_.weak = event->rumble.right;
            feedback_.version = feedback_version_.fetch_add(1) + 1;
            if (probe) {
                std::fprintf(stderr, "[pad] rumble left=%u right=%u\n", feedback_.strong,
                             feedback_.weak);
            }
            break;
        }
        case CHIAKI_EVENT_TRIGGER_EFFECTS: {
            std::lock_guard<std::mutex> lock(feedback_mutex_);
            feedback_.triggers = true;
            feedback_.left.bytes[0] = event->trigger_effects.type_left;
            std::memcpy(&feedback_.left.bytes[1], event->trigger_effects.left, 10);
            feedback_.right.bytes[0] = event->trigger_effects.type_right;
            std::memcpy(&feedback_.right.bytes[1], event->trigger_effects.right, 10);
            feedback_.version = feedback_version_.fetch_add(1) + 1;
            if (probe) {
                std::fprintf(stderr,
                             "[pad] trigger effects L=%02x [%02x %02x %02x %02x] R=%02x [%02x %02x "
                             "%02x %02x]\n",
                             feedback_.left.bytes[0], feedback_.left.bytes[1],
                             feedback_.left.bytes[2], feedback_.left.bytes[3],
                             feedback_.left.bytes[4], feedback_.right.bytes[0],
                             feedback_.right.bytes[1], feedback_.right.bytes[2],
                             feedback_.right.bytes[3], feedback_.right.bytes[4]);
            }
            break;
        }
        case CHIAKI_EVENT_LED_COLOR: {
            std::lock_guard<std::mutex> lock(feedback_mutex_);
            feedback_.led = true;
            feedback_.r = event->led_state[0];
            feedback_.g = event->led_state[1];
            feedback_.b = event->led_state[2];
            feedback_.version = feedback_version_.fetch_add(1) + 1;
            if (probe) {
                std::fprintf(stderr, "[pad] led %u %u %u\n", feedback_.r, feedback_.g,
                             feedback_.b);
            }
            break;
        }
        case CHIAKI_EVENT_PLAYER_INDEX: {
            static const uint8_t kMasks[5] = {0x04, 0x0A, 0x15, 0x1B, 0x1F};
            const uint8_t index = event->player_index < 5 ? event->player_index : 4;
            std::lock_guard<std::mutex> lock(feedback_mutex_);
            feedback_.player_leds = true;
            feedback_.player_mask = kMasks[index];
            feedback_.version = feedback_version_.fetch_add(1) + 1;
            if (probe) {
                std::fprintf(stderr, "[pad] player index %u\n", index);
            }
            break;
        }
        case CHIAKI_EVENT_HAPTIC_INTENSITY: {
            std::lock_guard<std::mutex> lock(feedback_mutex_);
            switch (event->intensity) {
                case Off: haptic_nibble_ = 0x0F; break;
                case Weak: haptic_nibble_ = 0x03; break;
                case Medium: haptic_nibble_ = 0x02; break;
                default: haptic_nibble_ = 0x00; break;
            }
            feedback_.intensity = static_cast<uint8_t>((trigger_nibble_ << 4) | haptic_nibble_);
            feedback_.version = feedback_version_.fetch_add(1) + 1;
            break;
        }
        case CHIAKI_EVENT_TRIGGER_INTENSITY: {
            std::lock_guard<std::mutex> lock(feedback_mutex_);
            switch (event->intensity) {
                case Off: trigger_nibble_ = 0x0F; break;
                case Weak: trigger_nibble_ = 0x09; break;
                case Medium: trigger_nibble_ = 0x06; break;
                default: trigger_nibble_ = 0x00; break;
            }
            feedback_.intensity = static_cast<uint8_t>((trigger_nibble_ << 4) | haptic_nibble_);
            feedback_.version = feedback_version_.fetch_add(1) + 1;
            break;
        }
        case CHIAKI_EVENT_MOTION_RESET: {            chiaki_accel_new_zero_set_active(&accel_zero_, tracker_.accel_x, tracker_.accel_y,
                                             tracker_.accel_z, false);
            break;
        }
        default:
            break;
    }
}

PadConsoleFeedback Streaming::feedback() const {
    std::lock_guard<std::mutex> lock(feedback_mutex_);
    return feedback_;
}

void Streaming::AudioHeaderCallback(ChiakiAudioHeader* header, void* user) {
    (void)user;
    if (header) {
        std::fprintf(stderr, "[stream] audio header: %u ch, %u Hz\n",
                     static_cast<unsigned>(header->channels),
                     static_cast<unsigned>(header->rate));
    }
}

void Streaming::AudioFrameCallback(uint8_t* buf, size_t size, void* user) {
    static_cast<Streaming*>(user)->OnAudio(buf, size);
}

void Streaming::HapticsFrameCallback(uint8_t* buf, size_t size, void* user) {
    static_cast<Streaming*>(user)->OnHaptics(buf, size);
}

void Streaming::OnHaptics(uint8_t* buf, size_t size) {
    // Stereo int16 at 3 kHz; a frame is about 10 ms. The average |sample| per
    // channel is what chiaki-ng feeds to rumble when full haptics are not
    // available (Bluetooth, or a non-DualSense pad).
    if (!buf || size == 0 || (size % (2 * sizeof(int16_t))) != 0) {
        return;
    }
    const size_t count = size / (2 * sizeof(int16_t));
    uint64_t sum_left = 0;
    uint64_t sum_right = 0;
    for (size_t i = 0; i < count; ++i) {
        int16_t left = 0;
        int16_t right = 0;
        std::memcpy(&left, buf + i * 4, sizeof(int16_t));
        std::memcpy(&right, buf + i * 4 + 2, sizeof(int16_t));
        sum_left += static_cast<uint64_t>(left < 0 ? -left : left) * 2;
        sum_right += static_cast<uint64_t>(right < 0 ? -right : right) * 2;
    }
    uint16_t left = static_cast<uint16_t>(std::min<uint64_t>(sum_left / count, 0xFFFF));
    uint16_t right = static_cast<uint16_t>(std::min<uint64_t>(sum_right / count, 0xFFFF));
    {
        std::lock_guard<std::mutex> lock(haptics_mutex_);
        haptics_left_ = left;
        haptics_right_ = right;
        haptics_seq_.fetch_add(1);
    }
    static const bool probe = std::getenv("BRP_PAD_PROBE") != nullptr;
    if (probe) {
        static uint64_t frames = 0;
        static uint64_t last_report = 0;
        static uint16_t peak_l = 0;
        static uint16_t peak_r = 0;
        ++frames;
        peak_l = std::max(peak_l, left);
        peak_r = std::max(peak_r, right);
        const uint64_t now = chiaki_time_now_monotonic_ms();
        if (now - last_report >= 1000) {
            std::fprintf(stderr, "[pad] haptics %llu frames/s  peak L=%u R=%u\n",
                         static_cast<unsigned long long>(frames), peak_l, peak_r);
            frames = 0;
            peak_l = 0;
            peak_r = 0;
            last_report = now;
        }
    }
}

bool Streaming::TakeHaptics(uint16_t& left, uint16_t& right) {
    const uint32_t seq = haptics_seq_.load();
    if (seq == haptics_seen_) {
        return false;
    }
    haptics_seen_ = seq;
    std::lock_guard<std::mutex> lock(haptics_mutex_);
    left = haptics_left_;
    right = haptics_right_;
    return true;
}

void Streaming::OnAudio(uint8_t* buf, size_t size) {
    audio::Engine* engine = audio_.load();
    if (!opus_ctx_ || !engine || size == 0) {
        return;
    }

    AVPacket* packet = av_packet_alloc();
    if (!packet) {
        return;
    }
    if (av_new_packet(packet, static_cast<int>(size)) < 0) {
        av_packet_free(&packet);
        return;
    }
    std::memcpy(packet->data, buf, size);

    const int sent = avcodec_send_packet(static_cast<AVCodecContext*>(opus_ctx_), packet);
    av_packet_free(&packet);
    if (sent < 0) {
        return;
    }

    auto* frame = static_cast<AVFrame*>(opus_frame_);
    while (avcodec_receive_frame(static_cast<AVCodecContext*>(opus_ctx_), frame) == 0) {
        // Opus is always 48 kHz stereo, so the resampler is a one-time setup:
        // rebuilding and reinitialising it for every 10 ms packet was pure
        // churn on chiaki's audio thread.
        auto** swr = reinterpret_cast<SwrContext**>(&swr_);
        if (!swr_) {
            AVChannelLayout out_layout{};
            av_channel_layout_default(&out_layout, 2);
            SwrContext* created = nullptr;
            if (swr_alloc_set_opts2(&created, &out_layout, AV_SAMPLE_FMT_FLT, 48000,
                                    &frame->ch_layout,
                                    static_cast<AVSampleFormat>(frame->format),
                                    frame->sample_rate, 0, nullptr) >= 0 &&
                swr_init(created) >= 0) {
                swr_ = created;
            } else if (created) {
                swr_free(&created);
            }
            av_channel_layout_uninit(&out_layout);
        }

        if (swr_) {
            const int max_out = swr_get_out_samples(*swr, frame->nb_samples);
            if (max_out > 0) {
                pcm_scratch_.resize(static_cast<size_t>(max_out) * 2);
                uint8_t* dst[1] = {reinterpret_cast<uint8_t*>(pcm_scratch_.data())};
                const int converted =
                    swr_convert(*swr, dst, max_out,
                                const_cast<const uint8_t**>(frame->extended_data),
                                frame->nb_samples);
                if (converted > 0) {
                    engine->Push(pcm_scratch_.data(), converted);
                    audio_frames_.fetch_add(converted);
                }
            }
        }
        av_frame_unref(frame);
    }
}

std::string Streaming::message() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return message_;
}

bool Streaming::WaitForFrame(int timeout_ms) {
    std::unique_lock<std::mutex> lock(frame_mutex_);
    if (!frames_.empty()) {
        return true;
    }
    return frame_cv_.wait_for(lock, std::chrono::milliseconds(std::max(1, timeout_ms)),
                              [this]() { return !frames_.empty(); });
}
bool Streaming::TakeNewestFrame(media::DecodedFrame& out) {
    std::lock_guard<std::mutex> lock(frame_mutex_);
    if (frames_.empty()) {
        last_queue_ = 0;
        return false;
    }
    if (frames_.size() > 1) {
        const size_t dropped = frames_.size() - 1;
        frames_.erase(frames_.begin(), frames_.begin() + dropped);
        buffer_drops_.fetch_add(static_cast<int>(dropped));
        primed_ = false;
    }
    out = std::move(frames_.front());
    frames_.pop_front();
    last_queue_ = static_cast<int>(frames_.size());
    return true;
}

bool Streaming::TakeFrameNow(media::DecodedFrame& out) {
    std::lock_guard<std::mutex> lock(frame_mutex_);
    if (frames_.empty()) {
        last_queue_ = 0;
        return false;
    }
    out = std::move(frames_.front());
    frames_.pop_front();
    last_queue_ = static_cast<int>(frames_.size());
    return true;
}

bool Streaming::TakeFrameOrdered(media::DecodedFrame& out, int max_queue) {
    std::lock_guard<std::mutex> lock(frame_mutex_);
    if (frames_.empty()) {
        last_queue_ = 0;
        return false;
    }
    const int limit = std::clamp(max_queue, 1, 8);
    while (static_cast<int>(frames_.size()) > limit) {
        frames_.pop_front();
        buffer_drops_.fetch_add(1);
    }
    out = std::move(frames_.front());
    frames_.pop_front();
    last_queue_ = static_cast<int>(frames_.size());
    return true;
}

bool Streaming::TakeFrame(media::DecodedFrame& out, int buffer, bool paced) {
    std::lock_guard<std::mutex> lock(frame_mutex_);
    const auto now = std::chrono::steady_clock::now();
    const double seconds =
        std::chrono::duration<double>(now - started_).count();
    const int max_buffer = std::clamp(buffer, 0, 6);
    // Adaptive ceiling: one step above the chosen buffer, never above 3, so a
    // jittery link can buy a frame of slack without touching the setting.
    const int ceiling = max_buffer == 0 ? 0 : std::min(max_buffer + 1, 3);

    if (frames_.empty()) {
        primed_ = false;
        // An empty queue while streaming means the producer is behind. Add one
        // frame of slack only when the previous drought was long ago (jitter),
        // not when the source is persistently slower than the display (raising
        // would just add repeats). At most one step per drought.
        if (state_.load() == StreamState::Streaming && max_buffer > 0 &&
            seconds - last_drought_ >= 3.0) {
            const int target = buffer_target_.load();
            if (target < ceiling) {
                buffer_target_.store(target + 1);
            }
            last_drought_ = seconds;
            last_recovery_ = seconds;
        }
        return false;
    }

    if (paced) {
        // The presentation loop owns the cadence (see App::video_phase_): it
        // calls this only on a frame slot, so all this has to do is hand over
        // the oldest frame and keep at most one spare. Emitting the oldest
        // preserves order; any deeper queue is latency without benefit.
        while (frames_.size() > 2) {
            frames_.pop_front();
            buffer_drops_.fetch_add(1);
        }
        out = std::move(frames_.front());
        frames_.pop_front();
        last_queue_ = static_cast<int>(frames_.size());
        return true;
    }

    // Stable for five seconds: give latency back, one frame at a time.
    if (seconds - last_recovery_ >= 5.0) {
        const int target = buffer_target_.load();
        if (target > 1) {
            buffer_target_.store(target - 1);
        }
        last_recovery_ = seconds;
    }

    const int target = max_buffer == 0
                           ? 0
                           : std::clamp(buffer_target_.load(), 1, ceiling);
    const int wanted = target + 1;
    // Prime the buffer once, then present every tick. Waiting on every tick
    // would halve the frame rate whenever the producer is slightly slower than
    // the display; waiting only while unprimed keeps playback at the producer
    // rate and rebuilds the buffer after a real dropout.
    if (!primed_) {
        if (static_cast<int>(frames_.size()) < wanted) {
            return false;
        }
        primed_ = true;
    }
    // Bound latency: drop at most one frame per presentation tick, and only
    // when the queue is a full frame past target. One extra frame of queue is
    // normal decode jitter; dropping on it is what read as a skip.
    if (static_cast<int>(frames_.size()) > wanted + 1) {
        frames_.pop_front();
        buffer_drops_.fetch_add(1);
    }

    out = std::move(frames_.front());
    frames_.pop_front();
    last_queue_ = static_cast<int>(frames_.size());
    return true;
}

Streaming::StreamStats Streaming::ConsumeStats() {
    StreamStats stats;
    const auto now = std::chrono::steady_clock::now();
    stats.elapsed = std::chrono::duration<double>(now - stats_started_).count();
    stats_started_ = now;
    stats.frames = stats_frames_.exchange(0);
    stats.lost = stats_lost_.exchange(0);
    const int count = stats_decode_count_.exchange(0);
    const long long total_us = stats_decode_us_.exchange(0);
    const long long max_us = stats_decode_max_us_.exchange(0);
    const long long bytes = stats_bytes_.exchange(0);
    stats.decode_ms_avg = count > 0 ? static_cast<double>(total_us) / count / 1000.0 : 0.0;
    stats.decode_ms_max = static_cast<double>(max_us) / 1000.0;
    stats.kbps = stats.elapsed > 0.0 ? static_cast<double>(bytes) * 8.0 / stats.elapsed / 1000.0
                                     : 0.0;
    const int arrivals = stats_arrival_count_.exchange(0);
    const long long arrival_us = stats_arrival_us_.exchange(0);
    stats.arrival_ms = arrivals > 0
                           ? static_cast<double>(arrival_us) / arrivals / 1000.0
                           : 0.0;
    stats.arrival_outliers = stats_arrival_outliers_.exchange(0);
    stats.buffer = buffer_target_.load();
    stats.buffer_drops = buffer_drops_.exchange(0);
    {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        stats.queue = last_queue_;
    }
    return stats;
}

int Streaming::frame_count() const {
    return frame_count_.load();
}

double Streaming::fps() const {
    const int frames = frame_count_.load();
    if (frames < 2) {
        return 0.0;
    }
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count();
    return seconds > 0.0 ? frames / seconds : 0.0;
}

void Streaming::SendController(const PadState& pad) {
    std::lock_guard<std::mutex> api(api_mutex_);
    if (!initialised_) {
        return;
    }

    ChiakiControllerState next = controller_state_;
    next.buttons = MapButtons(pad.buttons);
    next.l2_state = static_cast<uint8_t>(pad.l2 * 255.0f);
    next.r2_state = static_cast<uint8_t>(pad.r2 * 255.0f);
    next.left_x = static_cast<int16_t>(pad.lx * 32767.0f);
    next.left_y = static_cast<int16_t>(-pad.ly * 32767.0f);
    next.right_x = static_cast<int16_t>(pad.rx * 32767.0f);
    next.right_y = static_cast<int16_t>(-pad.ry * 32767.0f);

    // Touchpad: two fingers, tracked by report slot. Coordinates are already in
    // the console's native range (0..1919 / 0..1079).
    for (int i = 0; i < 2; ++i) {
        const TouchPoint& touch = pad.touch[i];
        if (touch.active) {
            if (!touch_active_[i]) {
                const int8_t id = chiaki_controller_state_start_touch(&next, touch.x, touch.y);
                if (id >= 0) {
                    touch_ids_[i] = static_cast<uint8_t>(id);
                    touch_active_[i] = true;
                }
            } else {
                chiaki_controller_state_set_touch_pos(&next, touch_ids_[i], touch.x, touch.y);
            }
        } else if (touch_active_[i]) {
            chiaki_controller_state_stop_touch(&next, touch_ids_[i]);
            touch_active_[i] = false;
            touch_ids_[i] = 0xFF;
        }
    }

    // Motion: DualSense reports 1/1024 deg/s gyro and 1/8192 g accelerometer;
    // chiaki expects rad/s and g, plus a Madgwick quaternion the console uses.
    if (motion_enabled_) {
        constexpr float kDegToRad = 0.017453292519943295f;
        chiaki_orientation_tracker_update(
            &tracker_, pad.gyro_dps[0] * kDegToRad, pad.gyro_dps[1] * kDegToRad,
            pad.gyro_dps[2] * kDegToRad, pad.accel_g[0], pad.accel_g[1], pad.accel_g[2],
            &accel_zero_, false, static_cast<uint32_t>(chiaki_time_now_monotonic_us()));
        chiaki_orientation_tracker_apply_to_controller_state(&tracker_, &next);
    }

    if (chiaki_controller_state_equals(&next, &controller_state_)) {
        return;
    }
    controller_state_ = next;
    chiaki_session_set_controller_state(&session_, &controller_state_);
}

}  // namespace brp::session
