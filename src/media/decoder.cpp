#include "media/decoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libswscale/swscale.h>
}

#include <cstring>

namespace brp::media {
namespace {

AVFormatContext* Fmt(void* p) { return static_cast<AVFormatContext*>(p); }
AVCodecContext* Ctx(void* p) { return static_cast<AVCodecContext*>(p); }
AVPacket* Pkt(void* p) { return static_cast<AVPacket*>(p); }
AVFrame* Frm(void* p) { return static_cast<AVFrame*>(p); }
SwsContext* Sws(void* p) { return static_cast<SwsContext*>(p); }
AVCodecParserContext* Parser(void* p) { return static_cast<AVCodecParserContext*>(p); }

}  // namespace

Decoder::~Decoder() {
    Close();
}

void Decoder::Close() {
    if (parser_) {
        av_parser_close(Parser(parser_));
        parser_ = nullptr;
    }
    if (scaler_) {
        sws_freeContext(Sws(scaler_));
        scaler_ = nullptr;
    }
    if (sw_frame_) {
        av_frame_free(reinterpret_cast<AVFrame**>(&sw_frame_));
    }
    if (frame_) {
        av_frame_free(reinterpret_cast<AVFrame**>(&frame_));
    }
    if (packet_) {
        av_packet_free(reinterpret_cast<AVPacket**>(&packet_));
    }
    if (codec_) {
        avcodec_free_context(reinterpret_cast<AVCodecContext**>(&codec_));
    }
    if (hw_device_) {
        av_buffer_unref(reinterpret_cast<AVBufferRef**>(&hw_device_));
    }
    if (format_) {
        avformat_close_input(reinterpret_cast<AVFormatContext**>(&format_));
    }
    streaming_ = false;
    hardware_ = false;
    need_keyframe_ = false;
    stream_index_ = -1;
}

bool Decoder::InitCodec(const void* codec_ptr, const void* params_ptr, std::string& error) {
    const AVCodec* decoder = static_cast<const AVCodec*>(codec_ptr);

    codec_ = avcodec_alloc_context3(decoder);
    if (!codec_) {
        error = "cannot create decoder context";
        return false;
    }

    if (params_ptr) {
        if (avcodec_parameters_to_context(Ctx(codec_), static_cast<const AVCodecParameters*>(params_ptr)) < 0) {
            error = "cannot apply stream parameters";
            return false;
        }
    }

    // D3D11VA when the codec offers it; frames are copied back to system memory.
    AVBufferRef* hw = nullptr;
    if (av_hwdevice_ctx_create(&hw, AV_HWDEVICE_TYPE_D3D11VA, nullptr, nullptr, 0) >= 0) {
        for (int i = 0;; ++i) {
            const AVCodecHWConfig* config = avcodec_get_hw_config(decoder, i);
            if (!config) {
                break;
            }
            if ((config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) &&
                config->device_type == AV_HWDEVICE_TYPE_D3D11VA) {
                Ctx(codec_)->hw_device_ctx = av_buffer_ref(hw);
                hardware_ = true;
                break;
            }
        }
        hw_device_ = hw;
    }

    if (avcodec_open2(Ctx(codec_), decoder, nullptr) < 0) {
        error = "cannot open decoder";
        return false;
    }

    codec_name_ = decoder->name;
    width_ = Ctx(codec_)->width;
    height_ = Ctx(codec_)->height;

    if (!packet_) {
        packet_ = av_packet_alloc();
    }
    if (!frame_) {
        frame_ = av_frame_alloc();
    }
    if (!packet_ || !frame_) {
        error = "cannot allocate frame/packet";
        return false;
    }
    return true;
}

bool Decoder::Open(const std::string& path, std::string& error) {
    Close();

    if (avformat_open_input(reinterpret_cast<AVFormatContext**>(&format_), path.c_str(), nullptr,
                            nullptr) < 0) {
        error = "cannot open input";
        return false;
    }
    if (avformat_find_stream_info(Fmt(format_), nullptr) < 0) {
        error = "cannot read stream info";
        Close();
        return false;
    }

    stream_index_ = av_find_best_stream(Fmt(format_), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (stream_index_ < 0) {
        error = "no video stream";
        Close();
        return false;
    }

    AVCodecParameters* params = Fmt(format_)->streams[stream_index_]->codecpar;
    const AVCodec* decoder = avcodec_find_decoder(params->codec_id);
    if (!decoder) {
        error = "no decoder for this codec";
        Close();
        return false;
    }
    if (!InitCodec(decoder, params, error)) {
        Close();
        return false;
    }
    if (avcodec_parameters_to_context(Ctx(codec_), params) < 0) {
        error = "cannot apply stream parameters";
        Close();
        return false;
    }
    return true;
}

bool Decoder::OpenStream(StreamCodec codec, std::string& error) {
    Close();
    streaming_ = true;

    const AVCodecID id = codec == StreamCodec::Hevc ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264;
    const AVCodec* decoder = avcodec_find_decoder(id);
    if (!decoder) {
        error = "decoder not available";
        return false;
    }
    if (!InitCodec(decoder, nullptr, error)) {
        Close();
        return false;
    }

    parser_ = av_parser_init(id);
    if (!parser_) {
        error = "cannot create parser";
        Close();
        return false;
    }
    return true;
}

bool Decoder::Feed(const uint8_t* data, size_t size) {
    if (!streaming_ || !parser_ || !codec_ || !data || size == 0) {
        return false;
    }

    size_t offset = 0;
    bool accepted = true;

    while (offset < size) {
        uint8_t* parsed = nullptr;
        int parsed_size = 0;
        const int consumed = av_parser_parse2(Parser(parser_), Ctx(codec_), &parsed, &parsed_size,
                                              data + offset, static_cast<int>(size - offset),
                                              AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
        if (consumed < 0) {
            need_keyframe_ = true;
            return false;
        }
        offset += static_cast<size_t>(consumed);

        if (parsed_size > 0) {
            AVPacket* packet = av_packet_alloc();
            if (!packet) {
                return false;
            }
            if (av_new_packet(packet, parsed_size) < 0) {
                av_packet_free(&packet);
                return false;
            }
            std::memcpy(packet->data, parsed, parsed_size);
            packet->pts = AV_NOPTS_VALUE;
            packet->dts = AV_NOPTS_VALUE;

            const int sent = avcodec_send_packet(Ctx(codec_), packet);
            av_packet_free(&packet);

            if (sent < 0 && sent != AVERROR(EAGAIN)) {
                need_keyframe_ = true;
                accepted = false;
            }
        }

        if (consumed == 0 && parsed_size == 0) {
            break;
        }
    }
    return accepted;
}

bool Decoder::Receive(DecodedFrame& frame) {
    if (!codec_) {
        return false;
    }
    return Drain(frame);
}

bool Decoder::Drain(DecodedFrame& out) {
    while (true) {
        const int received = avcodec_receive_frame(Ctx(codec_), Frm(frame_));
        if (received == 0) {
            const bool ok = Convert(Frm(frame_), out);
            av_frame_unref(Frm(frame_));
            return ok;
        }
        if (received == AVERROR(EAGAIN) || received == AVERROR_EOF) {
            return false;
        }
        need_keyframe_ = true;
        return false;
    }
}

bool Decoder::Convert(const void* frame_ptr, DecodedFrame& out) {
    auto* frame = static_cast<AVFrame*>(const_cast<void*>(frame_ptr));
    AVFrame* source = frame;

    if (frame->hw_frames_ctx) {
        if (!sw_frame_) {
            sw_frame_ = av_frame_alloc();
        }
        av_frame_unref(Frm(sw_frame_));
        if (av_hwframe_transfer_data(Frm(sw_frame_), frame, 0) < 0) {
            return false;
        }
        source = Frm(sw_frame_);
    }

    out.width = source->width;
    out.height = source->height;
    out.rgba.resize(static_cast<size_t>(source->width) * source->height * 4);

    scaler_ = sws_getCachedContext(Sws(scaler_), source->width, source->height,
                                   static_cast<AVPixelFormat>(source->format), source->width,
                                   source->height, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr,
                                   nullptr, nullptr);
    if (!scaler_) {
        return false;
    }

        // Explicit colour handling: the stream is limited-range BT.709 (BT.2020 for
    // HDR). Without this swscale assumes BT.601 limited and colours wash out.
    {
        const int* src_coeffs = sws_getCoefficients(SWS_CS_ITU709);
        switch (source->colorspace) {
            case AVCOL_SPC_BT2020_NCL:
            case AVCOL_SPC_BT2020_CL:
                src_coeffs = sws_getCoefficients(SWS_CS_BT2020);
                break;
            case AVCOL_SPC_SMPTE170M:
            case AVCOL_SPC_BT470BG:
                src_coeffs = sws_getCoefficients(SWS_CS_ITU601);
                break;
            default:
                break;
        }
        const int src_range = source->color_range == AVCOL_RANGE_JPEG ? 1 : 0;
        sws_setColorspaceDetails(Sws(scaler_), src_coeffs, src_range,
                                 sws_getCoefficients(SWS_CS_DEFAULT), 1, 0, 1 << 16, 1 << 16);
    }
uint8_t* dst[4] = {out.rgba.data(), nullptr, nullptr, nullptr};
    int dst_linesize[4] = {source->width * 4, 0, 0, 0};
    sws_scale(Sws(scaler_), source->data, source->linesize, 0, source->height, dst, dst_linesize);

    width_ = out.width;
    height_ = out.height;
    ++frame_count_;
    return true;
}

bool Decoder::Next(DecodedFrame& out, std::string& error) {
    if (!codec_) {
        error = "decoder not open";
        return false;
    }

    while (true) {
        if (Drain(out)) {
            return true;
        }

        const int read = av_read_frame(Fmt(format_), Pkt(packet_));
        if (read < 0) {
            avcodec_send_packet(Ctx(codec_), nullptr);
            if (Drain(out)) {
                return true;
            }
            error = "end of stream";
            return false;
        }

        if (Pkt(packet_)->stream_index == stream_index_) {
            if (avcodec_send_packet(Ctx(codec_), Pkt(packet_)) < 0) {
                av_packet_unref(Pkt(packet_));
                error = "send packet failed";
                return false;
            }
        }
        av_packet_unref(Pkt(packet_));
    }
}

}  // namespace brp::media
