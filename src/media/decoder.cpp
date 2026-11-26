#include "media/decoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include <d3d11.h>
#include <libavutil/hwcontext_d3d11va.h>

namespace brp::media {
namespace {

AVFormatContext* Fmt(void* p) { return static_cast<AVFormatContext*>(p); }
AVCodecContext* Ctx(void* p) { return static_cast<AVCodecContext*>(p); }
AVPacket* Pkt(void* p) { return static_cast<AVPacket*>(p); }
AVFrame* Frm(void* p) { return static_cast<AVFrame*>(p); }
SwsContext* Sws(void* p) { return static_cast<SwsContext*>(p); }
AVCodecParserContext* Parser(void* p) { return static_cast<AVCodecParserContext*>(p); }

namespace {

// Optional per-stage decode timing (BRP_DECODE_PROBE=1). The live path is
// single-threaded, so plain counters are enough; the probe is compiled in but
// costs one steady_clock read per stage only when the variable is set.
struct DecodeProbe {
    bool enabled = std::getenv("BRP_DECODE_PROBE") != nullptr;
    int frames = 0;
    double receive = 0.0;
    double transfer = 0.0;
    double resize = 0.0;
    double scale = 0.0;
    double vp = 0.0;

    static double Ms(std::chrono::steady_clock::time_point from,
                     std::chrono::steady_clock::time_point to) {
        return std::chrono::duration<double, std::milli>(to - from).count();
    }

    void Report() {
        if (!enabled) {
            return;
        }
        if (++frames < 60) {
            return;
        }
        const double n = static_cast<double>(frames);
        std::fprintf(stderr,
                     "[decode-probe] recv %.2f transfer %.2f resize %.2f scale %.2f vp %.2f ms\n",
                     receive / n, transfer / n, resize / n, scale / n, vp / n);
        frames = 0;
        receive = transfer = resize = scale = vp = 0.0;
    }
};

DecodeProbe g_decode_probe;

}  // namespace
}  // namespace

Decoder::~Decoder() {
    Close();
}

void Decoder::Close() {
    ReleaseVideoProcessor();
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

    auto apply_params = [&](AVCodecContext* context) {
        if (!params_ptr) {
            return true;
        }
        return avcodec_parameters_to_context(
                   context, static_cast<const AVCodecParameters*>(params_ptr)) >= 0;
    };

    codec_ = avcodec_alloc_context3(decoder);
    if (!codec_) {
        error = "cannot create decoder context";
        return false;
    }
    if (!apply_params(Ctx(codec_))) {
        error = "cannot apply stream parameters";
        return false;
    }

    auto backend_name = [](AVHWDeviceType type) -> const char* {
        switch (type) {
            case AV_HWDEVICE_TYPE_D3D11VA: return "d3d11va";
            case AV_HWDEVICE_TYPE_D3D12VA: return "d3d12va";
            case AV_HWDEVICE_TYPE_VULKAN: return "vulkan";
            case AV_HWDEVICE_TYPE_DXVA2: return "dxva2";
            case AV_HWDEVICE_TYPE_CUDA: return "cuda";
            default: return "software";
        }
    };
    auto backend_kind = [](AVHWDeviceType type) -> DecodeBackend {
        switch (type) {
            case AV_HWDEVICE_TYPE_D3D11VA: return DecodeBackend::D3D11VA;
            case AV_HWDEVICE_TYPE_D3D12VA: return DecodeBackend::D3D12VA;
            case AV_HWDEVICE_TYPE_VULKAN: return DecodeBackend::Vulkan;
            case AV_HWDEVICE_TYPE_DXVA2: return DecodeBackend::DXVA2;
            case AV_HWDEVICE_TYPE_CUDA: return DecodeBackend::CUDA;
            default: return DecodeBackend::Software;
        }
    };
    auto codec_supports = [&](AVHWDeviceType type) {
        for (int i = 0;; ++i) {
            const AVCodecHWConfig* config = avcodec_get_hw_config(decoder, i);
            if (!config) {
                return false;
            }
            if ((config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) &&
                config->device_type == type) {
                return true;
            }
        }
    };

    std::vector<AVHWDeviceType> order;
    switch (requested_backend_) {
        case DecodeBackend::Auto:
            order = {AV_HWDEVICE_TYPE_D3D11VA, AV_HWDEVICE_TYPE_D3D12VA,
                     AV_HWDEVICE_TYPE_CUDA, AV_HWDEVICE_TYPE_VULKAN,
                     AV_HWDEVICE_TYPE_DXVA2};
            break;
        case DecodeBackend::D3D11VA:
            order = {AV_HWDEVICE_TYPE_D3D11VA};
            break;
        case DecodeBackend::D3D12VA:
            order = {AV_HWDEVICE_TYPE_D3D12VA};
            break;
        case DecodeBackend::Vulkan:
            order = {AV_HWDEVICE_TYPE_VULKAN};
            break;
        case DecodeBackend::DXVA2:
            order = {AV_HWDEVICE_TYPE_DXVA2};
            break;
        case DecodeBackend::CUDA:
            order = {AV_HWDEVICE_TYPE_CUDA};
            break;
        case DecodeBackend::Software:
            break;
    }

    backend_note_.clear();
    AVBufferRef* hw = nullptr;
    AVHWDeviceType chosen = AV_HWDEVICE_TYPE_NONE;
    auto create_device = [&](AVHWDeviceType type, AVBufferRef** out) -> int {
        if (type == AV_HWDEVICE_TYPE_D3D11VA && external_device_) {
            AVBufferRef* ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
            if (!ref) {
                return AVERROR(ENOMEM);
            }
            auto* device_ctx = reinterpret_cast<AVHWDeviceContext*>(ref->data);
            auto* d3d = static_cast<AVD3D11VADeviceContext*>(device_ctx->hwctx);
            d3d->device = static_cast<ID3D11Device*>(external_device_);
            d3d->device->AddRef();
            if (external_context_) {
                d3d->device_context = static_cast<ID3D11DeviceContext*>(external_context_);
                d3d->device_context->AddRef();
            }
            const int err = av_hwdevice_ctx_init(ref);
            if (err < 0) {
                av_buffer_unref(&ref);
                return err;
            }
            *out = ref;
            return 0;
        }
        return av_hwdevice_ctx_create(out, type, nullptr, nullptr, 0);
    };
    for (AVHWDeviceType type : order) {
        if (!codec_supports(type)) {
            backend_note_ += std::string(backend_name(type)) + " not offered by codec; ";
            continue;
        }
        if (create_device(type, &hw) < 0) {
            backend_note_ += std::string(backend_name(type)) + " device creation failed; ";
            hw = nullptr;
            continue;
        }
        Ctx(codec_)->hw_device_ctx = av_buffer_ref(hw);
        chosen = type;
        break;
    }
    if (chosen != AV_HWDEVICE_TYPE_NONE) {
        hw_device_ = hw;
    } else if (requested_backend_ != DecodeBackend::Software) {
        backend_note_ += "falling back to software";
    }

    int open_result = avcodec_open2(Ctx(codec_), decoder, nullptr);
    bool using_hw = chosen != AV_HWDEVICE_TYPE_NONE;
    if (open_result < 0 && using_hw) {
        // The device exists but this codec/FFmpeg build cannot drive it: retry
        // with a clean software context instead of failing the whole session.
        if (hw_device_) {
            av_buffer_unref(reinterpret_cast<AVBufferRef**>(&hw_device_));
        }
        avcodec_free_context(reinterpret_cast<AVCodecContext**>(&codec_));
        codec_ = avcodec_alloc_context3(decoder);
        if (!codec_ || !apply_params(Ctx(codec_))) {
            error = "cannot create software fallback context";
            return false;
        }
        open_result = avcodec_open2(Ctx(codec_), decoder, nullptr);
        using_hw = false;
        chosen = AV_HWDEVICE_TYPE_NONE;
        backend_note_ += "hardware open failed; software";
    }
    if (open_result < 0) {
        error = "cannot open decoder";
        return false;
    }

    hardware_ = using_hw;
    active_backend_ = using_hw ? backend_kind(chosen) : DecodeBackend::Software;
    backend_name_ = backend_name(chosen);
    std::fprintf(stderr, "[decode] backend=%s requested=%d%s%s\n", backend_name_.c_str(),
                 static_cast<int>(requested_backend_),
                 backend_note_.empty() ? "" : " note=", backend_note_.c_str());

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
        const auto receive_started = std::chrono::steady_clock::now();
        const int received = avcodec_receive_frame(Ctx(codec_), Frm(frame_));
        if (g_decode_probe.enabled) {
            g_decode_probe.receive +=
                DecodeProbe::Ms(receive_started, std::chrono::steady_clock::now());
        }
        if (received == 0) {
            const bool ok = Convert(Frm(frame_), out);
            g_decode_probe.Report();
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

void Decoder::ReleaseVideoProcessor() {
    for (auto& srv : vp_gpu_srv_) {
        if (srv) {
            static_cast<ID3D11ShaderResourceView*>(srv)->Release();
            srv = nullptr;
        }
    }
    for (auto& texture : vp_gpu_tex_) {
        if (texture) {
            static_cast<ID3D11Texture2D*>(texture)->Release();
            texture = nullptr;
        }
    }
    vp_gpu_slot_ = 0;
    if (vp_stage_) {
        static_cast<ID3D11Texture2D*>(vp_stage_)->Release();
        vp_stage_ = nullptr;
    }
    if (vp_out_) {
        static_cast<ID3D11Texture2D*>(vp_out_)->Release();
        vp_out_ = nullptr;
    }
    if (vp_processor_) {
        static_cast<ID3D11VideoProcessor*>(vp_processor_)->Release();
        vp_processor_ = nullptr;
    }
    if (vp_enum_) {
        static_cast<ID3D11VideoProcessorEnumerator*>(vp_enum_)->Release();
        vp_enum_ = nullptr;
    }
    vp_device_ = nullptr;
    vp_context_ = nullptr;
    vp_immediate_ = nullptr;
    vp_in_w_ = 0;
    vp_in_h_ = 0;
    vp_out_w_ = 0;
    vp_out_h_ = 0;
    vp_in_format_ = 0;
}

// Converts the decoder's own D3D11 NV12/P010 surface to tightly packed RGBA on
// the GPU. swscale costs ~5.8 ms per 1080p frame and the GPU->CPU NV12 transfer
// another 1.4 ms; this path does both in one video-processor blit and reads
// back only the RGBA result that the AI worker needs. Returns false on any
// unsupported configuration so Convert() can fall back to swscale.
bool Decoder::ConvertWithVideoProcessor(const void* frame_ptr, DecodedFrame& out) {
    auto* frame = static_cast<AVFrame*>(const_cast<void*>(frame_ptr));
    if (frame->format != AV_PIX_FMT_D3D11 || !frame->hw_frames_ctx || !frame->data[0]) {
        return false;
    }
    auto* frames = reinterpret_cast<AVHWFramesContext*>(frame->hw_frames_ctx->data);
    if (!frames || !frames->device_ctx) {
        return false;
    }
    auto* hw = frames->device_ctx;
    auto* dev = hw ? static_cast<AVD3D11VADeviceContext*>(hw->hwctx) : nullptr;
    if (!dev || !dev->device || !dev->device_context || !dev->video_device ||
        !dev->video_context) {
        return false;
    }
    auto* texture = reinterpret_cast<ID3D11Texture2D*>(frame->data[0]);
    D3D11_TEXTURE2D_DESC td{};
    texture->GetDesc(&td);
    if (td.Format != DXGI_FORMAT_NV12 && td.Format != DXGI_FORMAT_P010) {
        return false;
    }
    const int out_w = frame->width;
    const int out_h = frame->height;
    if (out_w <= 0 || out_h <= 0) {
        return false;
    }

    if (vp_enum_ && vp_in_w_ == static_cast<int>(td.Width) &&
        vp_in_h_ == static_cast<int>(td.Height) && vp_out_w_ == out_w &&
        vp_out_h_ == out_h && vp_in_format_ == static_cast<int>(td.Format)) {
        // Reuse the processor and its output textures.
    } else {
        ReleaseVideoProcessor();
        vp_device_ = dev->video_device;
        vp_context_ = dev->video_context;
        vp_immediate_ = dev->device_context;

        D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
        content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        content.InputWidth = td.Width;
        content.InputHeight = td.Height;
        content.OutputWidth = static_cast<UINT>(out_w);
        content.OutputHeight = static_cast<UINT>(out_h);
        content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
        auto* video_device = static_cast<ID3D11VideoDevice*>(vp_device_);
        HRESULT hr = video_device->CreateVideoProcessorEnumerator(
            &content, reinterpret_cast<ID3D11VideoProcessorEnumerator**>(&vp_enum_));
        if (FAILED(hr) || !vp_enum_) {
            ReleaseVideoProcessor();
            return false;
        }
        auto* vp_enum = static_cast<ID3D11VideoProcessorEnumerator*>(vp_enum_);
        UINT in_support = 0;
        UINT out_support = 0;
        if (FAILED(vp_enum->CheckVideoProcessorFormat(td.Format, &in_support)) ||
            !(in_support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) ||
            FAILED(vp_enum->CheckVideoProcessorFormat(DXGI_FORMAT_R8G8B8A8_UNORM, &out_support)) ||
            !(out_support & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT)) {
            ReleaseVideoProcessor();
            return false;
        }
        hr = video_device->CreateVideoProcessor(
            vp_enum, 0, reinterpret_cast<ID3D11VideoProcessor**>(&vp_processor_));
        if (FAILED(hr) || !vp_processor_) {
            ReleaseVideoProcessor();
            return false;
        }

        D3D11_TEXTURE2D_DESC od{};
        od.Width = static_cast<UINT>(out_w);
        od.Height = static_cast<UINT>(out_h);
        od.MipLevels = 1;
        od.ArraySize = 1;
        od.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        od.SampleDesc.Count = 1;
        od.Usage = D3D11_USAGE_DEFAULT;
        od.BindFlags = D3D11_BIND_RENDER_TARGET;
        hr = dev->device->CreateTexture2D(
            &od, nullptr, reinterpret_cast<ID3D11Texture2D**>(&vp_out_));
        if (FAILED(hr) || !vp_out_) {
            ReleaseVideoProcessor();
            return false;
        }
        D3D11_TEXTURE2D_DESC sd = od;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.BindFlags = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        hr = dev->device->CreateTexture2D(
            &sd, nullptr, reinterpret_cast<ID3D11Texture2D**>(&vp_stage_));
        if (FAILED(hr) || !vp_stage_) {
            ReleaseVideoProcessor();
            return false;
        }
        // Two GPU-resident targets for the AI-off path. The presenter samples
        // these directly, so the picture never returns to system memory.
        D3D11_TEXTURE2D_DESC gd = od;
        gd.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        for (int i = 0; i < 2; ++i) {
            hr = dev->device->CreateTexture2D(
                &gd, nullptr, reinterpret_cast<ID3D11Texture2D**>(&vp_gpu_tex_[i]));
            if (FAILED(hr) || !vp_gpu_tex_[i]) {
                ReleaseVideoProcessor();
                return false;
            }
            hr = dev->device->CreateShaderResourceView(
                reinterpret_cast<ID3D11Texture2D*>(vp_gpu_tex_[i]), nullptr,
                reinterpret_cast<ID3D11ShaderResourceView**>(&vp_gpu_srv_[i]));
            if (FAILED(hr) || !vp_gpu_srv_[i]) {
                ReleaseVideoProcessor();
                return false;
            }
        }
        vp_gpu_slot_ = 0;
        vp_in_w_ = static_cast<int>(td.Width);
        vp_in_h_ = static_cast<int>(td.Height);
        vp_out_w_ = out_w;
        vp_out_h_ = out_h;
        vp_in_format_ = static_cast<int>(td.Format);
        if (!vp_logged_) {
            std::fprintf(stderr,
                         "[decode] video-processor %s %ux%u -> RGBA %dx%d\n",
                         td.Format == DXGI_FORMAT_P010 ? "P010" : "NV12", td.Width,
                         td.Height, out_w, out_h);
            vp_logged_ = true;
        }
    }

    auto* video_device = static_cast<ID3D11VideoDevice*>(vp_device_);
    auto* video_context = static_cast<ID3D11VideoContext*>(vp_context_);
    auto* vp_enum = static_cast<ID3D11VideoProcessorEnumerator*>(vp_enum_);
    auto* vp = static_cast<ID3D11VideoProcessor*>(vp_processor_);
    const bool gpu_only = !cpu_pixels_ && vp_gpu_tex_[0] != nullptr;
    const int gpu_slot = vp_gpu_slot_ & 1;
    auto* out_tex = gpu_only ? static_cast<ID3D11Texture2D*>(vp_gpu_tex_[gpu_slot])
                             : static_cast<ID3D11Texture2D*>(vp_out_);
    auto* stage = static_cast<ID3D11Texture2D*>(vp_stage_);

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd{};
    ivd.FourCC = 0;
    ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    ivd.Texture2D.MipSlice = 0;
    ivd.Texture2D.ArraySlice = static_cast<UINT>(reinterpret_cast<uintptr_t>(frame->data[1]));
    ID3D11VideoProcessorInputView* in_view = nullptr;
    HRESULT hr = video_device->CreateVideoProcessorInputView(texture, vp_enum, &ivd, &in_view);
    if (FAILED(hr) || !in_view) {
        return false;
    }
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd{};
    ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    ovd.Texture2D.MipSlice = 0;
    ID3D11VideoProcessorOutputView* out_view = nullptr;
    hr = video_device->CreateVideoProcessorOutputView(out_tex, vp_enum, &ovd, &out_view);
    if (FAILED(hr) || !out_view) {
        in_view->Release();
        return false;
    }

    RECT source_rect{0, 0, out_w, out_h};
    RECT dest_rect{0, 0, out_w, out_h};
    video_context->VideoProcessorSetStreamFrameFormat(
        vp, 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    video_context->VideoProcessorSetStreamSourceRect(vp, 0, TRUE, &source_rect);
    video_context->VideoProcessorSetOutputTargetRect(vp, TRUE, &dest_rect);
    video_context->VideoProcessorSetStreamOutputRate(
        vp, 0, D3D11_VIDEO_PROCESSOR_OUTPUT_RATE_NORMAL, FALSE, nullptr);
    video_context->VideoProcessorSetStreamAutoProcessingMode(vp, 0, FALSE);

    D3D11_VIDEO_PROCESSOR_COLOR_SPACE in_cs{};
    in_cs.Usage = 0;   // playback
    in_cs.RGB_Range = 0;
    in_cs.YCbCr_Matrix =
        (frame->colorspace == AVCOL_SPC_BT709 || frame->colorspace == AVCOL_SPC_BT2020_NCL ||
         frame->colorspace == AVCOL_SPC_BT2020_CL)
            ? 1
            : 0;
    in_cs.YCbCr_xvYCC = 0;
    in_cs.Nominal_Range = frame->color_range == AVCOL_RANGE_JPEG
                              ? D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255
                              : D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
    video_context->VideoProcessorSetStreamColorSpace(vp, 0, &in_cs);

    D3D11_VIDEO_PROCESSOR_COLOR_SPACE out_cs{};
    out_cs.Usage = 0;
    out_cs.RGB_Range = 0;  // full-range RGB
    out_cs.YCbCr_Matrix = 0;
    out_cs.YCbCr_xvYCC = 0;
    out_cs.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
    video_context->VideoProcessorSetOutputColorSpace(vp, &out_cs);

    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.OutputIndex = 0;
    stream.InputFrameOrField = 0;
    stream.PastFrames = 0;
    stream.FutureFrames = 0;
    stream.pInputSurface = in_view;
    hr = video_context->VideoProcessorBlt(vp, out_view, 0, 1, &stream);
    out_view->Release();
    in_view->Release();
    if (FAILED(hr)) {
        return false;
    }

    if (gpu_only) {
        // The presenter samples this texture on the same device. The two-slot
        // ring keeps the previous frame alive while the next one is decoded.
        out.rgba.clear();
        out.gpu_view = vp_gpu_srv_[gpu_slot];
        vp_gpu_slot_ = (gpu_slot + 1) & 1;
        out.width = out_w;
        out.height = out_h;
        out.pq = false;
        width_ = out_w;
        height_ = out_h;
        ++frame_count_;
        return true;
    }

    out.gpu_view = nullptr;
    auto* immediate = static_cast<ID3D11DeviceContext*>(vp_immediate_);
    immediate->CopyResource(stage, out_tex);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr = immediate->Map(stage, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        return false;
    }
    const size_t row = static_cast<size_t>(out_w) * 4;
    out.rgba.resize(row * static_cast<size_t>(out_h));
    const auto* src = static_cast<const uint8_t*>(mapped.pData);
    for (int y = 0; y < out_h; ++y) {
        std::memcpy(out.rgba.data() + row * static_cast<size_t>(y),
                    src + static_cast<size_t>(mapped.RowPitch) * static_cast<size_t>(y), row);
    }
    immediate->Unmap(stage, 0);
    out.width = out_w;
    out.height = out_h;
    out.pq = false;
    width_ = out_w;
    height_ = out_h;
    ++frame_count_;
    return true;
}

bool Decoder::Convert(const void* frame_ptr, DecodedFrame& out) {
    auto* frame = static_cast<AVFrame*>(const_cast<void*>(frame_ptr));
    AVFrame* source = frame;

    if (const char* vp_env = std::getenv("BRP_DECODE_VP")) {
        vp_enabled_ = std::strcmp(vp_env, "0") != 0;
    }
    if (vp_enabled_ && !vp_failed_ && frame->hw_frames_ctx &&
        frame->format == AV_PIX_FMT_D3D11 && frame->color_trc != AVCOL_TRC_SMPTE2084 &&
        frame->colorspace != AVCOL_SPC_BT2020_NCL && frame->colorspace != AVCOL_SPC_BT2020_CL) {
        const auto vp_started = std::chrono::steady_clock::now();
        if (ConvertWithVideoProcessor(frame, out)) {
            if (g_decode_probe.enabled) {
                g_decode_probe.vp += DecodeProbe::Ms(vp_started,
                                                      std::chrono::steady_clock::now());
            }
            return true;
        }
        // One failure is enough: swscale stays the safe path for this session.
        vp_failed_ = true;
        ReleaseVideoProcessor();
        std::fprintf(stderr, "[decode] video-processor unavailable, using swscale\n");
    }

    const auto transfer_started = std::chrono::steady_clock::now();
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
    const auto transfer_done = std::chrono::steady_clock::now();
    if (g_decode_probe.enabled) {
        g_decode_probe.transfer += DecodeProbe::Ms(transfer_started, transfer_done);
    }

    out.width = source->width;
    out.height = source->height;
    out.pq = source->color_trc == AVCOL_TRC_SMPTE2084;
    out.rgba.resize(static_cast<size_t>(source->width) * source->height * 4);
    const auto resize_done = std::chrono::steady_clock::now();
    if (g_decode_probe.enabled) {
        g_decode_probe.resize += DecodeProbe::Ms(transfer_done, resize_done);
    }

    // Chroma upsampling and the YUV->RGBA colour conversion are pure per-row
    // work, and swscale is single threaded: at 1080p this box spends ~5.8 ms
    // per frame in it, on the latency path of every frame. The frame is split
    // into even horizontal slices; each worker owns its own SwsContext (created
    // for the full frame, so the vertical filter reads across slice edges) and
    // scales its band with srcSliceY.
    int sws_flags = SWS_BICUBIC;
    if (chroma_quality_ == 0) {
        sws_flags = SWS_BILINEAR;
    } else if (chroma_quality_ == 2) {
        sws_flags = SWS_LANCZOS | SWS_ACCURATE_RND;
    }

    scaler_ = sws_getCachedContext(Sws(scaler_), source->width, source->height,
                                   static_cast<AVPixelFormat>(source->format), source->width,
                                   source->height, AV_PIX_FMT_RGBA, sws_flags, nullptr,
                                   nullptr, nullptr);
    if (!scaler_) {
        return false;
    }

    // Explicit colour handling: the stream is limited-range BT.709 (BT.2020 for
    // HDR). Without this swscale assumes BT.601 limited and colours wash out.
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
    auto apply_colorspace = [&](SwsContext* ctx) {
        sws_setColorspaceDetails(ctx, src_coeffs, src_range,
                                 sws_getCoefficients(SWS_CS_DEFAULT), 1, 0, 1 << 16, 1 << 16);
    };
    apply_colorspace(Sws(scaler_));

    // sws_scale is single threaded, and its slice API is not safe to drive from
    // several contexts at once: a slice that starts in the middle of the frame
    // reads across the slice edge, and a slice-local buffer loses the frame edge
    // handling at the bottom (measured both, so the parallel path was removed).
    // Bicubic is also the fastest option on this machine (bilinear and lanczos
    // both cost more), so one call is what stays.
    uint8_t* dst[4] = {out.rgba.data(), nullptr, nullptr, nullptr};
    int dst_linesize[4] = {source->width * 4, 0, 0, 0};
    sws_scale(Sws(scaler_), source->data, source->linesize, 0, source->height, dst, dst_linesize);
    out.gpu_view = nullptr;
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
