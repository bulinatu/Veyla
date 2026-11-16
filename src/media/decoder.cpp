#include "media/decoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
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

}  // namespace

Decoder::~Decoder() {
    Close();
}

void Decoder::Close() {
    if (scaler_) {
        sws_freeContext(Sws(scaler_));
        scaler_ = nullptr;
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
    if (format_) {
        avformat_close_input(reinterpret_cast<AVFormatContext**>(&format_));
    }
    stream_index_ = -1;
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

    codec_ = avcodec_alloc_context3(decoder);
    if (!codec_ || avcodec_parameters_to_context(Ctx(codec_), params) < 0) {
        error = "cannot create decoder context";
        Close();
        return false;
    }
    if (avcodec_open2(Ctx(codec_), decoder, nullptr) < 0) {
        error = "cannot open decoder";
        Close();
        return false;
    }

    codec_name_ = decoder->name;
    width_ = Ctx(codec_)->width;
    height_ = Ctx(codec_)->height;

    packet_ = av_packet_alloc();
    frame_ = av_frame_alloc();
    if (!packet_ || !frame_) {
        error = "cannot allocate frame/packet";
        Close();
        return false;
    }
    return true;
}

bool Decoder::Next(DecodedFrame& out, std::string& error) {
    if (!codec_) {
        error = "decoder not open";
        return false;
    }

    auto fill = [&](AVFrame* frame) {
        out.width = frame->width;
        out.height = frame->height;
        out.rgba.resize(static_cast<size_t>(frame->width) * frame->height * 4);

        scaler_ = sws_getCachedContext(Sws(scaler_), frame->width, frame->height,
                                       static_cast<AVPixelFormat>(frame->format), frame->width,
                                       frame->height, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr,
                                       nullptr, nullptr);
        uint8_t* dst[4] = {out.rgba.data(), nullptr, nullptr, nullptr};
        int dst_linesize[4] = {frame->width * 4, 0, 0, 0};
        sws_scale(Sws(scaler_), frame->data, frame->linesize, 0, frame->height, dst,
                  dst_linesize);
        ++frame_count_;
    };

    while (true) {
        const int received = avcodec_receive_frame(Ctx(codec_), Frm(frame_));
        if (received == 0) {
            fill(Frm(frame_));
            av_frame_unref(Frm(frame_));
            return true;
        }
        if (received == AVERROR_EOF) {
            error = "end of stream";
            return false;
        }
        if (received != AVERROR(EAGAIN)) {
            error = "decode error";
            return false;
        }

        const int read = av_read_frame(Fmt(format_), Pkt(packet_));
        if (read < 0) {
            avcodec_send_packet(Ctx(codec_), nullptr);
            continue;
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
