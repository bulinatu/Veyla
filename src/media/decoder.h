#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace brp::media {

struct DecodedFrame {
    int width = 0;
    int height = 0;
    bool pq = false;             // source transfer is PQ (HDR10)
    std::vector<uint8_t> rgba;  // tightly packed RGBA
};

enum class StreamCodec { H264, Hevc };

// Selectable decode backend. Auto prefers D3D11VA, then D3D12VA/Vulkan; an
// explicit backend falls back to software (with a note) if the driver or the
// FFmpeg build cannot provide it.
enum class DecodeBackend : int {
    Auto = 0,
    D3D11VA = 1,
    D3D12VA = 2,
    Vulkan = 3,
    DXVA2 = 4,
    CUDA = 5,
    Software = 6,
};

// Minimal libavcodec decoder with two modes:
//   * file/container mode (Open + Next) for validation and tooling,
//   * live mode (OpenStream + Feed + Receive) for Remote Play access units.
class Decoder {
public:
    Decoder() = default;
    ~Decoder();

    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;

    // --- file mode ---
    bool Open(const std::string& path, std::string& error);
    bool Next(DecodedFrame& frame, std::string& error);

    // --- live mode ---
    bool OpenStream(StreamCodec codec, std::string& error);
    // Feeds one access unit (Annex-B). Returns true if accepted.
    bool Feed(const uint8_t* data, size_t size);
    // Drains decoded frames. Returns false when no frame is ready.
    bool Receive(DecodedFrame& frame);

    void Close();

    void SetBackend(DecodeBackend backend) { requested_backend_ = backend; }
    DecodeBackend requested_backend() const { return requested_backend_; }
    DecodeBackend active_backend() const { return active_backend_; }
    std::string backend_name() const { return backend_name_; }
    std::string backend_note() const { return backend_note_; }

    // 0 bilinear, 1 bicubic, 2 lanczos. Applies to the YUV->RGB/chroma stage.
    void SetChromaQuality(int quality) { chroma_quality_ = quality; }
    int chroma_quality() const { return chroma_quality_; }

    std::string codec_name() const { return codec_name_; }
    int width() const { return width_; }
    int height() const { return height_; }
    int frame_count() const { return frame_count_; }
    bool hardware() const { return hardware_; }

    // Requests a keyframe path; Remote Play resends an IDR when the decoder
    // rejects a sample.
    bool need_keyframe() const { return need_keyframe_; }
    void clear_keyframe_request() { need_keyframe_ = false; }

private:
    bool InitCodec(const void* codec, const void* params, std::string& error);
    bool Drain(DecodedFrame& frame);
    bool Convert(const void* frame, DecodedFrame& out);

    void* format_ = nullptr;       // AVFormatContext*
    void* codec_ = nullptr;        // AVCodecContext*
    void* packet_ = nullptr;       // AVPacket*
    void* frame_ = nullptr;        // AVFrame*
    void* scaler_ = nullptr;       // SwsContext*
    void* hw_device_ = nullptr;    // AVBufferRef* (D3D11VA)
    void* sw_frame_ = nullptr;     // AVFrame* for hw->system transfer
    void* parser_ = nullptr;       // AVCodecParserContext*
    bool streaming_ = false;
    bool hardware_ = false;
    bool need_keyframe_ = false;
    int stream_index_ = -1;
    int width_ = 0;
    int height_ = 0;
    int frame_count_ = 0;
    DecodeBackend requested_backend_ = DecodeBackend::Auto;
    DecodeBackend active_backend_ = DecodeBackend::Software;
    std::string backend_name_ = "software";
    std::string backend_note_;
    int chroma_quality_ = 1;
    std::string codec_name_;
};

}  // namespace brp::media
