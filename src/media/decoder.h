#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace brp::media {

struct DecodedFrame {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;  // tightly packed RGBA
};

// Minimal libavcodec decoder used to validate the decode path before the
// session layer feeds it live packets. Accepts a file or raw Annex-B stream.
class Decoder {
public:
    Decoder() = default;
    ~Decoder();

    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;

    bool Open(const std::string& path, std::string& error);
    void Close();

    bool Next(DecodedFrame& frame, std::string& error);

    std::string codec_name() const { return codec_name_; }
    int width() const { return width_; }
    int height() const { return height_; }
    int frame_count() const { return frame_count_; }

private:
    void* format_ = nullptr;       // AVFormatContext*
    void* codec_ = nullptr;        // AVCodecContext*
    void* packet_ = nullptr;       // AVPacket*
    void* frame_ = nullptr;        // AVFrame*
    void* scaler_ = nullptr;       // SwsContext*
    int stream_index_ = -1;
    int width_ = 0;
    int height_ = 0;
    int frame_count_ = 0;
    std::string codec_name_;
};

}  // namespace brp::media
