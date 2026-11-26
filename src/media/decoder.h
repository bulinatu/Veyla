#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <atomic>

namespace brp::media {

struct DecodedFrame {
    int width = 0;
    int height = 0;
    bool pq = false;             // source transfer is PQ (HDR10)
    double arrived_ms = 0.0;     // steady_clock, when the video packet arrived
    std::vector<uint8_t> rgba;  // tightly packed RGBA
    // When set, the picture already exists as an RGBA texture on the shared
    // device and rgba is empty. The view is owned by the decoder's ring and
    // stays valid across frames.
    void* gpu_view = nullptr;   // ID3D11ShaderResourceView*
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
    // Decode on an existing D3D11 device (the presenter's). When set, the
    // video-processor output can be presented directly, without a CPU copy.
    void SetExternalDevice(void* device, void* context) {
        external_device_ = device;
        external_context_ = context;
    }
    // True: always produce CPU RGBA (AI features need it). False: keep the
    // picture on the GPU and expose DecodedFrame::gpu_view, unless no shared
    // device was supplied.
    void SetCpuPixels(bool cpu) { cpu_pixels_.store(cpu || external_device_ == nullptr); }
    bool shared_device() const { return external_device_ != nullptr; }
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
    void ReleaseVideoProcessor();
    bool ConvertWithVideoProcessor(const void* frame_ptr, DecodedFrame& out);

    void* format_ = nullptr;       // AVFormatContext*
    void* codec_ = nullptr;        // AVCodecContext*
    void* packet_ = nullptr;       // AVPacket*
    void* frame_ = nullptr;        // AVFrame*
    void* scaler_ = nullptr;       // SwsContext*
    void* hw_device_ = nullptr;    // AVBufferRef* (D3D11VA)
    void* sw_frame_ = nullptr;     // AVFrame* for hw->system transfer
    void* parser_ = nullptr;       // AVCodecParserContext*
    // D3D11 video-processor colour conversion (borrowed device, owned objects).
    void* vp_device_ = nullptr;    // ID3D11VideoDevice* (owned by the hw device)
    void* vp_context_ = nullptr;   // ID3D11VideoContext* (owned by the hw device)
    void* vp_immediate_ = nullptr; // ID3D11DeviceContext* (owned by the hw device)
    void* vp_enum_ = nullptr;      // ID3D11VideoProcessorEnumerator*
    void* vp_processor_ = nullptr; // ID3D11VideoProcessor*
    void* vp_out_ = nullptr;       // ID3D11Texture2D* (RGBA render target)
    void* vp_stage_ = nullptr;     // ID3D11Texture2D* (RGBA staging)
    void* vp_gpu_tex_[2] = {nullptr, nullptr};
    void* vp_gpu_srv_[2] = {nullptr, nullptr};
    int vp_gpu_slot_ = 0;
    void* external_device_ = nullptr;   // borrowed ID3D11Device*
    void* external_context_ = nullptr;  // borrowed ID3D11DeviceContext*
    std::atomic<bool> cpu_pixels_{true};
    int vp_in_w_ = 0;
    int vp_in_h_ = 0;
    int vp_out_w_ = 0;
    int vp_out_h_ = 0;
    int vp_in_format_ = 0;
    bool vp_failed_ = false;
    bool vp_enabled_ = true;
    bool vp_logged_ = false;
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
