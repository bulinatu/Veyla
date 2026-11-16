#include "session/streaming.h"

#include <chiaki/base64.h>
#include <chiaki/common.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
}

#include <cstdio>
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

    chiaki_connect_video_profile_preset(&info_.video_profile,
                                        CHIAKI_VIDEO_RESOLUTION_PRESET_1080p,
                                        CHIAKI_VIDEO_FPS_PRESET_60);
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

    if (chiaki_session_start(&session_) != CHIAKI_ERR_SUCCESS) {
        error = "session start failed";
        chiaki_session_fini(&session_);
        decoder_.Close();
        return false;
    }

    initialised_ = true;
    started_ = std::chrono::steady_clock::now();
    frame_count_.store(0);
    audio_frames_.store(0);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        message_ = "Connecting to " + profile.host;
    }
    state_.store(StreamState::Connecting);
    return true;
}

void Streaming::Stop() {
    if (initialised_) {
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
    (void)frames_lost;
    if (!decoder_.Feed(buf, size)) {
        chiaki_session_request_idr(&session_);
        return;
    }

    media::DecodedFrame frame;
    if (decoder_.Receive(frame)) {
        {
            std::lock_guard<std::mutex> lock(frame_mutex_);
            latest_ = std::move(frame);
            has_new_frame_ = true;
        }
        frame_count_.fetch_add(1);
        if (state_.load() != StreamState::Streaming) {
            state_.store(StreamState::Streaming);
            std::lock_guard<std::mutex> lock(mutex_);
            message_ = "Streaming 1080p60";
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
    switch (event->type) {
        case CHIAKI_EVENT_CONNECTED: {
            std::lock_guard<std::mutex> lock(mutex_);
            message_ = "Connected, waiting for video";
            break;
        }
        case CHIAKI_EVENT_QUIT: {
            state_.store(StreamState::Failed);
            std::lock_guard<std::mutex> lock(mutex_);
            message_ = "Session ended (reason " + std::to_string(event->quit.reason) + ")";
            break;
        }
        default:
            break;
    }
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

void Streaming::OnAudio(uint8_t* buf, size_t size) {
    if (!opus_ctx_ || !audio_ || size == 0) {
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
        AVChannelLayout out_layout{};
        av_channel_layout_default(&out_layout, 2);

        auto** swr = reinterpret_cast<SwrContext**>(&swr_);
        if (swr_alloc_set_opts2(swr, &out_layout, AV_SAMPLE_FMT_FLT, 48000, &frame->ch_layout,
                                static_cast<AVSampleFormat>(frame->format), frame->sample_rate, 0,
                                nullptr) >= 0 &&
            swr_init(*swr) >= 0) {
            const int max_out = swr_get_out_samples(*swr, frame->nb_samples);
            std::vector<float> pcm(static_cast<size_t>(max_out) * 2);
            uint8_t* dst[1] = {reinterpret_cast<uint8_t*>(pcm.data())};
            const int converted = swr_convert(*swr, dst, max_out,
                                              const_cast<const uint8_t**>(frame->extended_data),
                                              frame->nb_samples);
            if (converted > 0) {
                audio_->Push(pcm.data(), converted);
                audio_frames_.fetch_add(converted);
            }
        }
        av_channel_layout_uninit(&out_layout);
        av_frame_unref(frame);
    }
}

std::string Streaming::message() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return message_;
}

bool Streaming::TakeFrame(media::DecodedFrame& out) {
    std::lock_guard<std::mutex> lock(frame_mutex_);
    if (!has_new_frame_) {
        return false;
    }
    out = std::move(latest_);
    latest_ = {};
    has_new_frame_ = false;
    return true;
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
    if (!initialised_) {
        return;
    }
    ChiakiControllerState state{};
    state.buttons = MapButtons(pad.buttons);
    state.l2_state = static_cast<uint8_t>(pad.l2 * 255.0f);
    state.r2_state = static_cast<uint8_t>(pad.r2 * 255.0f);
    state.left_x = static_cast<int16_t>(pad.lx * 32767.0f);
    state.left_y = static_cast<int16_t>(-pad.ly * 32767.0f);
    state.right_x = static_cast<int16_t>(pad.rx * 32767.0f);
    state.right_y = static_cast<int16_t>(-pad.ry * 32767.0f);
    chiaki_session_set_controller_state(&session_, &state);
}

}  // namespace brp::session
