#include "media/pipeline.h"

#include "media/shaders.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace brp {
namespace {

struct PsParams {
    float sharpen;
    float mode;
    float src_w;
    float src_h;
    float hdr;
    float out_w;
    float out_h;
    float pad0;
    float con0[4];
    float con1[4];
    float con2[4];
    float con3[4];
    float color0[4];  // exposure, contrast, saturation, hue (radians)
    float color1[4];  // highlights, shadows, vibrance, colour active
    float color2[4];  // temperature, tint, gamma, reserved
};
static_assert(sizeof(PsParams) % 16 == 0, "constant buffer must stay 16-byte aligned");

struct CsParams {
    float time;
    float pad0;
    float pad1;
    float pad2;
};

ComPtr<ID3DBlob> CompileShader(const char* source, const char* entry, const char* target) {
    ComPtr<ID3DBlob> blob;
    ComPtr<ID3DBlob> errors;
    const UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_ENABLE_STRICTNESS;
    HRESULT hr = ::D3DCompile(source, std::strlen(source), nullptr, nullptr, nullptr, entry,
                              target, flags, 0, &blob, &errors);
    if (FAILED(hr)) {
        if (errors) {
            std::fprintf(stderr, "[shader:%s] %s\n", entry,
                         static_cast<const char*>(errors->GetBufferPointer()));
        }
        return nullptr;
    }
    return blob;
}

double Percentile(const std::vector<double>& values, double p) {
    if (values.empty()) {
        return 0.0;
    }
    std::vector<double> v = values;
    std::sort(v.begin(), v.end());
    const double idx = p * static_cast<double>(v.size() - 1);
    const auto lo = static_cast<size_t>(idx);
    const size_t hi = std::min(lo + 1, v.size() - 1);
    const double f = idx - static_cast<double>(lo);
    return v[lo] * (1.0 - f) + v[hi] * f;
}

}  // namespace

Presenter::~Presenter() {
    if (frame_waitable_) {
        ::CloseHandle(frame_waitable_);
        frame_waitable_ = nullptr;
    }
}

bool Presenter::Init(HWND hwnd, const PresenterConfig& cfg) {
    hwnd_ = hwnd;
    cfg_ = cfg;

    UINT device_flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#if defined(_DEBUG)
    device_flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                                        D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL got{};
    HRESULT hr = ::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, device_flags,
                                     levels, static_cast<UINT>(std::size(levels)),
                                     D3D11_SDK_VERSION, &device_, &got, &context_);
    if (FAILED(hr)) {
        std::fprintf(stderr, "D3D11CreateDevice failed: 0x%08lX\n", static_cast<unsigned long>(hr));
        return false;
    }

    ComPtr<IDXGIDevice> dxgi_device;
    if (FAILED(device_.As(&dxgi_device))) {
        return false;
    }
    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgi_device->GetAdapter(&adapter))) {
        return false;
    }
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
        return false;
    }

    ComPtr<IDXGIFactory5> factory5;
    if (SUCCEEDED(factory.As(&factory5))) {
        BOOL allow = FALSE;
        if (SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow,
                                                    sizeof(allow)))) {
            tearing_ = allow == TRUE;
        }
    }

        factory_ = factory;
    if (!CreateSwapchainForFormat()) {
        return false;
    }
    factory_->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);

    if (!CreateBackbufferViews() || !CreateTargetViews() || !CreateSourceTexture() ||
        !CreateShaders() || !CreateTimingQueries()) {
        return false;
    }

    start_time_ = std::chrono::steady_clock::now();
    std::printf("presenter: %dx%d <- %dx%d, tearing=%s, vsync=%s, waitable=%s\n",
                cfg_.output_width, cfg_.output_height, cfg_.source_width, cfg_.source_height,
                tearing_ ? "on" : "off", cfg_.vsync ? "on" : "off",
                waitable_ ? "on" : "off");
    return true;
}

bool Presenter::CreateBackbufferViews() {
    backbuffer_.Reset();
    rtv_.Reset();
    HRESULT hr = swapchain_->GetBuffer(0, IID_PPV_ARGS(&backbuffer_));
    if (FAILED(hr)) {
        return false;
    }
    return SUCCEEDED(device_->CreateRenderTargetView(backbuffer_.Get(), nullptr, &rtv_));
}

bool Presenter::CreateTargetViews() {
    target_tex_.Reset();
    target_rtv_.Reset();
    target_srv_.Reset();

    const int rw = cfg_.render_width;
    const int rh = cfg_.render_height;
    if (rw <= 0 || rh <= 0) {
        return true;  // direct rendering
    }
    if (rw == cfg_.output_width && rh == cfg_.output_height) {
        return true;  // the backbuffer already is the target
    }

    D3D11_TEXTURE2D_DESC td{};
    td.Width = static_cast<UINT>(rw);
    td.Height = static_cast<UINT>(rh);
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = cfg_.hdr ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device_->CreateTexture2D(&td, nullptr, &target_tex_))) {
        std::fprintf(stderr, "render target %dx%d creation failed\n", rw, rh);
        return false;
    }
    if (FAILED(device_->CreateRenderTargetView(target_tex_.Get(), nullptr, &target_rtv_))) {
        return false;
    }
    return SUCCEEDED(device_->CreateShaderResourceView(target_tex_.Get(), nullptr, &target_srv_));
}

bool Presenter::CreateSourceTexture() {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = static_cast<UINT>(cfg_.source_width);
    td.Height = static_cast<UINT>(cfg_.source_height);
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

    if (FAILED(device_->CreateTexture2D(&td, nullptr, &source_))) {
        return false;
    }
    if (FAILED(device_->CreateShaderResourceView(source_.Get(), nullptr, &source_srv_))) {
        return false;
    }
    return SUCCEEDED(device_->CreateUnorderedAccessView(source_.Get(), nullptr, &source_uav_));
}

bool Presenter::CreateShaders() {
    ComPtr<ID3DBlob> cs_blob = CompileShader(shaders::kGenCS, "CSMain", "cs_5_0");
    ComPtr<ID3DBlob> vs_blob = CompileShader(shaders::kFullscreenVS, "VSMain", "vs_5_0");
    ComPtr<ID3DBlob> ps_blob = CompileShader(shaders::kUpscalePS, "PSMain", "ps_5_0");
    if (!cs_blob || !vs_blob || !ps_blob) {
        return false;
    }

    if (FAILED(device_->CreateComputeShader(cs_blob->GetBufferPointer(), cs_blob->GetBufferSize(),
                                            nullptr, &gen_cs_))) {
        return false;
    }
    if (FAILED(device_->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(),
                                           nullptr, &vs_))) {
        return false;
    }
    if (FAILED(device_->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(),
                                          nullptr, &ps_))) {
        return false;
    }

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device_->CreateSamplerState(&sd, &sampler_))) {
        return false;
    }

    D3D11_BUFFER_DESC bd{};
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    bd.ByteWidth = sizeof(CsParams);
    if (FAILED(device_->CreateBuffer(&bd, nullptr, &cs_cb_))) {
        return false;
    }
    bd.ByteWidth = sizeof(PsParams);
    return SUCCEEDED(device_->CreateBuffer(&bd, nullptr, &ps_cb_));
}

bool Presenter::CreateTimingQueries() {
    D3D11_QUERY_DESC disjoint_desc{};
    disjoint_desc.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;

    D3D11_QUERY_DESC ts_desc{};
    ts_desc.Query = D3D11_QUERY_TIMESTAMP;

    for (int i = 0; i < kTimingSlots; ++i) {
        if (FAILED(device_->CreateQuery(&disjoint_desc, &slots_[i].disjoint)) ||
            FAILED(device_->CreateQuery(&ts_desc, &slots_[i].begin)) ||
            FAILED(device_->CreateQuery(&ts_desc, &slots_[i].end))) {
            return false;
        }
    }
    return true;
}

bool Presenter::Resize(int width, int height) {
    if (width <= 0 || height <= 0) {
        return false;
    }
    if (width == cfg_.output_width && height == cfg_.output_height) {
        return true;  // already this size: no buffer churn, no flash
    }

    ID3D11RenderTargetView* null_rtv = nullptr;
    context_->OMSetRenderTargets(1, &null_rtv, nullptr);

    rtv_.Reset();
    backbuffer_.Reset();

    // ResizeBuffers validates the flags against the ones the swapchain was
    // created with. The frame-latency waitable flag is part of that set, so
    // omitting it fails with E_INVALIDARG (the previous build could not enter
    // fullscreen at all).
    const UINT flags = swapchain_flags_;
    HRESULT hr = swapchain_->ResizeBuffers(0, static_cast<UINT>(width), static_cast<UINT>(height),
                                           DXGI_FORMAT_UNKNOWN, flags);
    std::fprintf(stderr, "[presenter] resize %dx%d -> %dx%d hr=0x%08lX\n", cfg_.output_width,
                 cfg_.output_height, width, height, static_cast<unsigned long>(hr));
    if (FAILED(hr)) {
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
            device_lost_ = true;
            return false;
        }
        // Some drivers reject the waitable flag in ResizeBuffers even though
        // they accepted it at creation. Recreate the swapchain instead of
        // leaving the window stuck at the old size.
        std::fprintf(stderr, "[presenter] recreating swapchain %dx%d\n", width, height);
        cfg_.output_width = width;
        cfg_.output_height = height;
        rtv_.Reset();
        backbuffer_.Reset();
        swapchain2_.Reset();
        swapchain_.Reset();
        if (!CreateSwapchainForFormat() || !CreateBackbufferViews()) {
            return false;
        }
        return CreateTargetViews();
    }

    cfg_.output_width = width;
    cfg_.output_height = height;
    if (!CreateBackbufferViews()) {
        return false;
    }
    return CreateTargetViews();
}

bool Presenter::SetUpscaleTarget(int width, int height) {
    cfg_.render_width = width;
    cfg_.render_height = height;

    // Unbind before replacing the textures.
    ID3D11RenderTargetView* null_rtv = nullptr;
    context_->OMSetRenderTargets(1, &null_rtv, nullptr);
    ID3D11ShaderResourceView* null_srv = nullptr;
    context_->PSSetShaderResources(0, 1, &null_srv);

    return CreateTargetViews();
}

void Presenter::SetUpscaleMode(int mode) {
    cfg_.upscale_mode = std::clamp(mode, 0, 4);
}

void Presenter::BeginTiming() {
    current_slot_ = -1;
    for (int i = 0; i < kTimingSlots; ++i) {
        const int idx = (next_slot_ + i) % kTimingSlots;
        if (!slots_[idx].active) {
            context_->Begin(slots_[idx].disjoint.Get());
            context_->End(slots_[idx].begin.Get());
            slots_[idx].active = true;
            current_slot_ = idx;
            next_slot_ = (idx + 1) % kTimingSlots;
            return;
        }
    }
}

void Presenter::EndTiming() {
    if (current_slot_ < 0) {
        return;
    }
    context_->End(slots_[current_slot_].end.Get());
    context_->End(slots_[current_slot_].disjoint.Get());
}

void Presenter::CollectTiming() {
    for (auto& slot : slots_) {
        if (!slot.active) {
            continue;
        }

        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
        if (context_->GetData(slot.disjoint.Get(), &dj, sizeof(dj), 0) != S_OK) {
            continue;
        }

        UINT64 begin = 0;
        UINT64 end = 0;
        if (context_->GetData(slot.begin.Get(), &begin, sizeof(begin), 0) != S_OK ||
            context_->GetData(slot.end.Get(), &end, sizeof(end), 0) != S_OK) {
            continue;
        }

        slot.active = false;
        if (!dj.Disjoint && dj.Frequency != 0 && end >= begin) {
            gpu_ms_.push_back(static_cast<double>(end - begin) * 1000.0 /
                              static_cast<double>(dj.Frequency));
            if (gpu_ms_.size() > 4000) {
                gpu_ms_.erase(gpu_ms_.begin(), gpu_ms_.begin() + 1000);
            }
        }
    }
}

bool Presenter::SetSourceFrame(const uint8_t* rgba, int width, int height, bool source_pq) {
    if (!rgba || width <= 0 || height <= 0) {
        return false;
    }
    source_override_.Reset();
    cfg_.source_pq = source_pq;
    cfg_.source_final = false;
    if (!source_ || cfg_.source_width != width || cfg_.source_height != height) {
        cfg_.source_width = width;
        cfg_.source_height = height;
        source_.Reset();
        source_srv_.Reset();
        source_uav_.Reset();
        if (!CreateSourceTexture()) {
            return false;
        }
    }
    context_->UpdateSubresource(source_.Get(), 0, nullptr, rgba, static_cast<UINT>(width) * 4, 0);
    external_source_ = true;
    return true;
}

void Presenter::SetSourceView(ID3D11ShaderResourceView* view, int width, int height,
                              bool source_pq) {
    if (!view) {
        return;
    }
    source_override_ = view;
    cfg_.source_pq = source_pq;
    if (width > 0 && height > 0) {
        cfg_.source_width = width;
        cfg_.source_height = height;
    }
    external_source_ = true;
}
bool Presenter::CreateSwapchainForFormat() {
    const DXGI_FORMAT format =
        cfg_.hdr ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = static_cast<UINT>(cfg_.output_width);
    desc.Height = static_cast<UINT>(cfg_.output_height);
    desc.Format = format;
    desc.Stereo = FALSE;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_SHADER_INPUT;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    const UINT tearing_flags = (tearing_ && cfg_.allow_tearing) ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;

    if (frame_waitable_) {
        ::CloseHandle(frame_waitable_);
        frame_waitable_ = nullptr;
    }
    swapchain2_.Reset();
    swapchain_.Reset();

    // Waitable object: the loop blocks on the swapchain before building the
    // next frame, so rendering is phase-locked to the display instead of
    // competing with a software timer. Without it the two clocks drift apart
    // and every few seconds a frame misses its vblank even though the CPU and
    // GPU had plenty of time.
    desc.Flags = tearing_flags | DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    HRESULT hr = factory_->CreateSwapChainForHwnd(device_.Get(), hwnd_, &desc, nullptr,
                                                  nullptr, &swapchain_);
    if (FAILED(hr)) {
        // Older drivers can reject the waitable flag; a normal flip swapchain
        // still works and the caller falls back to timer pacing.
        desc.Flags = tearing_flags;
        hr = factory_->CreateSwapChainForHwnd(device_.Get(), hwnd_, &desc, nullptr, nullptr,
                                              &swapchain_);
    }
    if (FAILED(hr)) {
        std::fprintf(stderr, "CreateSwapChainForHwnd failed: 0x%08lX\n",
                     static_cast<unsigned long>(hr));
        return false;
    }
    swapchain_flags_ = desc.Flags;

    if (SUCCEEDED(swapchain_.As(&swapchain2_))) {
        // One queued frame: lowest latency, and the waitable handle signals
        // once per refresh.
        if (SUCCEEDED(swapchain2_->SetMaximumFrameLatency(1))) {
            frame_waitable_ = swapchain2_->GetFrameLatencyWaitableObject();
            waitable_ = frame_waitable_ != nullptr;
        }
    }

    if (cfg_.hdr) {
        ComPtr<IDXGISwapChain3> swap3;
        if (SUCCEEDED(swapchain_.As(&swap3))) {
            swap3->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
        }
    }
    return true;
}

bool Presenter::WaitForFrame(DWORD timeout_ms) {
    if (!frame_waitable_) {
        return true;
    }
    return ::WaitForSingleObject(frame_waitable_, timeout_ms) == WAIT_OBJECT_0;
}

void Presenter::Render(double time_seconds) {
    const auto cpu_start = std::chrono::steady_clock::now();
    BeginTiming();

    D3D11_MAPPED_SUBRESOURCE mapped{};

    // 1. Source frame: decoded stream texture if present, else synthetic.
    if (!external_source_) {
    CsParams cs{};
    cs.time = static_cast<float>(time_seconds);
    if (SUCCEEDED(context_->Map(cs_cb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        std::memcpy(mapped.pData, &cs, sizeof(cs));
        context_->Unmap(cs_cb_.Get(), 0);
    }

    context_->CSSetShader(gen_cs_.Get(), nullptr, 0);
    ID3D11Buffer* cs_buffers[] = {cs_cb_.Get()};
    context_->CSSetConstantBuffers(0, 1, cs_buffers);

    ID3D11UnorderedAccessView* uavs[] = {source_uav_.Get()};
    context_->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
    context_->Dispatch((static_cast<UINT>(cfg_.source_width) + 15) / 16,
                       (static_cast<UINT>(cfg_.source_height) + 15) / 16, 1);

    ID3D11UnorderedAccessView* no_uavs[] = {nullptr};
    context_->CSSetUnorderedAccessViews(0, 1, no_uavs, nullptr);
    }

    // 2. Upscale + optional sharpen, optionally into an explicit render target
    //    (1080p/1440p/2160p) and then fitted to the window/display.
    const bool use_target =
        target_rtv_ && (cfg_.render_width != cfg_.output_width ||
                        cfg_.render_height != cfg_.output_height);
    const int stage_w = use_target ? cfg_.render_width : cfg_.output_width;
    const int stage_h = use_target ? cfg_.render_height : cfg_.output_height;

    ID3D11ShaderResourceView* source_view =
        source_override_ ? source_override_.Get() : source_srv_.Get();
    if (!source_view) {
        ID3D11RenderTargetView* clear_rtv = use_target ? target_rtv_.Get() : rtv_.Get();
        if (clear_rtv) {
            const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            context_->ClearRenderTargetView(clear_rtv, black);
        }
        EndTiming();
        return;
    }

    SetPsParams(cfg_.source_width, cfg_.source_height, stage_w, stage_h,
                cfg_.source_final ? 0 : cfg_.upscale_mode, cfg_.hdr, cfg_.source_pq, true);
    ID3D11RenderTargetView* stage_rtv = use_target ? target_rtv_.Get() : rtv_.Get();
    DrawFullscreen(source_view, stage_rtv, 0, 0, stage_w, stage_h);

    if (use_target) {
        const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        context_->ClearRenderTargetView(rtv_.Get(), black);

        // Preserve the stream's aspect ratio: black bars instead of stretching.
        const float scale = std::min(static_cast<float>(cfg_.output_width) / stage_w,
                                     static_cast<float>(cfg_.output_height) / stage_h);
        const int fitted_w = std::max(1, static_cast<int>(std::lround(stage_w * scale)));
        const int fitted_h = std::max(1, static_cast<int>(std::lround(stage_h * scale)));
        const int fitted_x = (cfg_.output_width - fitted_w) / 2;
        const int fitted_y = (cfg_.output_height - fitted_h) / 2;

        // Already upscaled and graded: this pass only fits it to the window.
        SetPsParams(stage_w, stage_h, fitted_w, fitted_h, 1, false, false, false);
        DrawFullscreen(target_srv_.Get(), rtv_.Get(), fitted_x, fitted_y, fitted_w, fitted_h);
    }

    EndTiming();

    cpu_ms_.push_back(std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - cpu_start)
                          .count());
    if (cpu_ms_.size() > 4000) {
        cpu_ms_.erase(cpu_ms_.begin(), cpu_ms_.begin() + 1000);
    }
}

void Presenter::SetPsParams(int src_w, int src_h, int dst_w, int dst_h, int mode, bool hdr_out,
                            bool src_pq, bool enhance) {
    PsParams ps{};
    ps.sharpen = enhance ? enh_.sharpen : 0.0f;
    ps.mode = static_cast<float>(mode);
    ps.src_w = static_cast<float>(src_w);
    ps.src_h = static_cast<float>(src_h);
    ps.out_w = static_cast<float>(dst_w);
    ps.out_h = static_cast<float>(dst_h);
    ps.hdr = hdr_out ? 1.0f : 0.0f;
    ps.pad0 = src_pq ? 1.0f : 0.0f;

    const float in_w = ps.src_w;
    const float in_h = ps.src_h;
    const float out_w = ps.out_w;
    const float out_h = ps.out_h;
    ps.con0[0] = in_w / out_w;
    ps.con0[1] = in_h / out_h;
    ps.con0[2] = 0.5f * in_w / out_w - 0.5f;
    ps.con0[3] = 0.5f * in_h / out_h - 0.5f;
    ps.con1[0] = 1.0f / in_w;
    ps.con1[1] = 1.0f / in_h;
    ps.con1[2] = 1.0f / in_w;
    ps.con1[3] = -1.0f / in_h;
    ps.con2[0] = -1.0f / in_w;
    ps.con2[1] = 2.0f / in_h;
    ps.con2[2] = 1.0f / in_w;
    ps.con2[3] = 2.0f / in_h;
    ps.con3[0] = 0.0f;
    ps.con3[1] = 4.0f / in_h;
    ps.con3[2] = 0.0f;
    ps.con3[3] = 0.0f;

    if (enhance) {
        ps.color0[0] = enh_.exposure;
        ps.color0[1] = enh_.contrast;
        ps.color0[2] = enh_.saturation;
        ps.color0[3] = enh_.hue_deg * 3.14159265f / 180.0f;
        ps.color1[0] = enh_.highlights;
        ps.color1[1] = enh_.shadows;
        ps.color1[2] = enh_.vibrance;
        ps.color1[3] = enh_.color ? 1.0f : 0.0f;
        ps.color2[0] = enh_.temperature;
        ps.color2[1] = enh_.tint;
        ps.color2[2] = enh_.gamma;
        ps.color2[3] = 0.0f;
    }

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(context_->Map(ps_cb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        std::memcpy(mapped.pData, &ps, sizeof(ps));
        context_->Unmap(ps_cb_.Get(), 0);
    }
}

void Presenter::DrawFullscreen(ID3D11ShaderResourceView* srv, ID3D11RenderTargetView* rtv, int x,
                               int y, int w, int h) {
    if (!srv || !rtv || w <= 0 || h <= 0) {
        return;
    }
    D3D11_VIEWPORT vp{};
    vp.TopLeftX = static_cast<float>(x);
    vp.TopLeftY = static_cast<float>(y);
    vp.Width = static_cast<float>(w);
    vp.Height = static_cast<float>(h);
    vp.MaxDepth = 1.0f;
    context_->RSSetViewports(1, &vp);
    context_->OMSetRenderTargets(1, &rtv, nullptr);

    ID3D11ShaderResourceView* views[] = {srv};
    context_->PSSetShaderResources(0, 1, views);
    ID3D11SamplerState* sampler = sampler_.Get();
    context_->PSSetSamplers(0, 1, &sampler);
    ID3D11Buffer* ps_buffers[] = {ps_cb_.Get()};
    context_->PSSetConstantBuffers(0, 1, ps_buffers);
    context_->VSSetShader(vs_.Get(), nullptr, 0);
    context_->PSSetShader(ps_.Get(), nullptr, 0);
    context_->IASetInputLayout(nullptr);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->Draw(3, 0);
}

bool Presenter::EnsureGradeTargets(int w, int h) {
    if (w <= 0 || h <= 0) {
        return false;
    }
    if (grade_tex_ && grade_rtv_ && grade_staging_ && grade_w_ == w && grade_h_ == h) {
        return true;
    }
    ID3D11RenderTargetView* no_rtv = nullptr;
    context_->OMSetRenderTargets(1, &no_rtv, nullptr);
    ID3D11ShaderResourceView* no_srv = nullptr;
    context_->PSSetShaderResources(0, 1, &no_srv);
    grade_rtv_.Reset();
    grade_tex_.Reset();
    grade_staging_.Reset();
    grade_w_ = 0;
    grade_h_ = 0;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = static_cast<UINT>(w);
    td.Height = static_cast<UINT>(h);
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device_->CreateTexture2D(&td, nullptr, &grade_tex_))) {
        return false;
    }
    if (FAILED(device_->CreateRenderTargetView(grade_tex_.Get(), nullptr, &grade_rtv_))) {
        return false;
    }
    D3D11_TEXTURE2D_DESC sd = td;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(device_->CreateTexture2D(&sd, nullptr, &grade_staging_))) {
        return false;
    }
    grade_w_ = w;
    grade_h_ = h;
    return true;
}

bool Presenter::EnhanceViewReadback(ID3D11ShaderResourceView* view, int w, int h, bool src_pq,
                                    std::vector<uint8_t>& out, double* ms) {
    if (!view || !EnsureGradeTargets(w, h)) {
        return false;
    }
    const auto started = std::chrono::steady_clock::now();

    // Always SDR out: this is the CPU RGBA contract of the FG worker.
    SetPsParams(w, h, w, h, 0, false, src_pq, true);
    DrawFullscreen(view, grade_rtv_.Get(), 0, 0, w, h);

    // Unbind before the copy; the debug layer flags a still-bound resource.
    ID3D11RenderTargetView* no_rtv = nullptr;
    context_->OMSetRenderTargets(1, &no_rtv, nullptr);
    ID3D11ShaderResourceView* no_srv = nullptr;
    context_->PSSetShaderResources(0, 1, &no_srv);

    context_->CopyResource(grade_staging_.Get(), grade_tex_.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context_->Map(grade_staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        return false;
    }
    const size_t row = static_cast<size_t>(w) * 4;
    out.resize(row * static_cast<size_t>(h));
    const auto* src = static_cast<const uint8_t*>(mapped.pData);
    for (int y = 0; y < h; ++y) {
        std::memcpy(out.data() + row * static_cast<size_t>(y),
                    src + static_cast<size_t>(mapped.RowPitch) * static_cast<size_t>(y), row);
    }
    context_->Unmap(grade_staging_.Get(), 0);

    enh_ms_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
                  .count();
    if (ms) {
        *ms = enh_ms_;
    }
    return true;
}

bool Presenter::EnhanceFrameReadback(const uint8_t* rgba, int w, int h, bool src_pq,
                                     std::vector<uint8_t>& out, double* ms) {
    if (!rgba || !SetSourceFrame(rgba, w, h, src_pq)) {
        return false;
    }
    return EnhanceViewReadback(source_srv_.Get(), w, h, src_pq, out, ms);
}

bool Presenter::video_memory_used_mb(uint64_t& used, uint64_t& budget) const {
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter3;
    if (!device_ || FAILED(device_.As(&dxgi_device)) || FAILED(dxgi_device->GetAdapter(&adapter)) ||
        FAILED(adapter.As(&adapter3))) {
        return false;
    }
    DXGI_QUERY_VIDEO_MEMORY_INFO info{};
    if (FAILED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
        return false;
    }
    used = info.CurrentUsage / (1024ull * 1024ull);
    budget = info.Budget / (1024ull * 1024ull);
    return true;
}

void Presenter::Clear(const float rgba[4]) {
    ID3D11RenderTargetView* rtv = rtv_.Get();
    context_->OMSetRenderTargets(1, &rtv, nullptr);
    context_->ClearRenderTargetView(rtv, rgba);
}

void Presenter::PresentNow(bool vsync) {
    if (!swapchain_) {
        return;
    }
    const UINT present_flags =
        (tearing_ && cfg_.allow_tearing && !vsync) ? DXGI_PRESENT_ALLOW_TEARING : 0;
    const HRESULT present_hr = swapchain_->Present(vsync ? 1 : 0, present_flags);
    if (present_hr == DXGI_ERROR_DEVICE_REMOVED || present_hr == DXGI_ERROR_DEVICE_RESET) {
        device_lost_ = true;
        std::fprintf(stderr, "[presenter] device lost (0x%08lX)\n",
                     static_cast<unsigned long>(present_hr));
        return;
    }

    // Ground truth for scanout: if the refresh counter advances faster than
    // the present counter, the display repeated a frame even though the CPU
    // loop looked perfect.
    DXGI_FRAME_STATISTICS stats{};
    const HRESULT stats_hr = swapchain_->GetFrameStatistics(&stats);
    if (SUCCEEDED(stats_hr)) {
        have_present_stats_ = true;
        if (have_present_stats_ && last_present_stats_.PresentCount != 0) {
            const UINT presents = stats.PresentCount - last_present_stats_.PresentCount;
            const UINT refreshes = stats.PresentRefreshCount - last_present_stats_.PresentRefreshCount;
            if (refreshes > presents) {
                present_misses_ += refreshes - presents;
            }
        }
        last_present_stats_ = stats;
        last_present_refresh_ = stats.PresentRefreshCount;
    } else if (!present_stats_logged_) {
        present_stats_logged_ = true;
        std::fprintf(stderr, "[presenter] frame statistics unavailable (0x%08lX)\n",
                     static_cast<unsigned long>(stats_hr));
    }

    CollectTiming();
    ++frames_;
}

void Presenter::Present() {
    PresentNow(cfg_.vsync);
}

bool Presenter::CaptureToBmp(const char* path) {
    // Prefer the explicit render target so F12 produces a 1440p/2160p frame
    // even when the window is smaller.
    ID3D11Texture2D* source_texture = target_tex_ ? target_tex_.Get() : backbuffer_.Get();
    if (!source_texture) {
        return false;
    }
    D3D11_TEXTURE2D_DESC td{};
    source_texture->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.MiscFlags = 0;

    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device_->CreateTexture2D(&td, nullptr, &staging))) {
        std::fprintf(stderr, "[capture] staging texture failed\n");
        return false;
    }

    context_->CopyResource(staging.Get(), source_texture);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        std::fprintf(stderr, "[capture] map failed\n");
        return false;
    }

    const int32_t width = static_cast<int32_t>(td.Width);
    const int32_t height = static_cast<int32_t>(td.Height);
    const uint32_t row_bytes = static_cast<uint32_t>(width) * 4u;
    const uint32_t image_bytes = row_bytes * static_cast<uint32_t>(height);

    std::FILE* f = nullptr;
    if (::fopen_s(&f, path, "wb") != 0 || f == nullptr) {
        std::fprintf(stderr, "[capture] fopen failed\n");
        context_->Unmap(staging.Get(), 0);
        return false;
    }

    // BMP headers, packed. 32-bit BI_RGB, bottom-up.
    unsigned char header[54] = {};
    header[0] = 'B';
    header[1] = 'M';
    const uint32_t file_size = 54u + image_bytes;
    std::memcpy(header + 2, &file_size, 4);
    const uint32_t pixel_offset = 54u;
    std::memcpy(header + 10, &pixel_offset, 4);
    const uint32_t info_size = 40u;
    std::memcpy(header + 14, &info_size, 4);
    std::memcpy(header + 18, &width, 4);
    std::memcpy(header + 22, &height, 4);
    const uint16_t planes = 1;
    std::memcpy(header + 26, &planes, 2);
    const uint16_t bpp = 32;
    std::memcpy(header + 28, &bpp, 2);
    std::memcpy(header + 34, &image_bytes, 4);
    std::fwrite(header, 1, sizeof(header), f);

    const auto* src = static_cast<const unsigned char*>(mapped.pData);
    std::vector<unsigned char> row(row_bytes);
    for (int32_t y = height - 1; y >= 0; --y) {
        const unsigned char* in = src + static_cast<size_t>(y) * mapped.RowPitch;
        for (int32_t x = 0; x < width; ++x) {
            // DXGI R8G8B8A8 is R,G,B,A in memory; BMP wants B,G,R,A.
            row[x * 4 + 0] = in[x * 4 + 2];
            row[x * 4 + 1] = in[x * 4 + 1];
            row[x * 4 + 2] = in[x * 4 + 0];
            row[x * 4 + 3] = 255;
        }
        std::fwrite(row.data(), 1, row_bytes, f);
    }

    std::fclose(f);
    context_->Unmap(staging.Get(), 0);
    return true;
}

std::string Presenter::Summary() const {
    const auto now = std::chrono::steady_clock::now();
    if (summary_frames_ == frames_ &&
        now - summary_at_ < std::chrono::milliseconds(250)) {
        return summary_cache_;
    }

    const double p50 = Percentile(gpu_ms_, 0.50);
    const double p95 = Percentile(gpu_ms_, 0.95);
    const double cpu = cpu_ms_.empty() ? 0.0 : cpu_ms_.back();

    char buf[400];
    if (target_tex_) {
        std::snprintf(buf, sizeof(buf),
                      "gpu p50 %.2f  p95 %.2f ms | cpu %.2f ms | %dx%d, render "
                      "%dx%d <- %dx%d | mode %d",
                      p50, p95, cpu, cfg_.output_width, cfg_.output_height,
                      cfg_.render_width, cfg_.render_height, cfg_.source_width,
                      cfg_.source_height, cfg_.upscale_mode);
    } else {
        std::snprintf(buf, sizeof(buf),
                      "gpu p50 %.2f  p95 %.2f ms | cpu %.2f ms | %dx%d <- %dx%d | mode %d",
                      p50, p95, cpu, cfg_.output_width, cfg_.output_height,
                      cfg_.source_width, cfg_.source_height, cfg_.upscale_mode);
    }

    summary_cache_ = buf;
    summary_at_ = now;
    summary_frames_ = frames_;
    return summary_cache_;
}

void Presenter::PrintFinalSummary() const {
    char buf[640];
    std::snprintf(buf, sizeof(buf),
                  "\n=== brp-bench ===\n"
                  "path      : %dx%d <- %dx%d, mode %d, vsync %s, tearing %s\n"
                  "gpu ms    : min %.3f  p50 %.3f  p95 %.3f  p99 %.3f  max %.3f  (n=%zu)\n"
                  "cpu ms    : p50 %.3f  p95 %.3f  (n=%zu)\n"
                  "frames    : %llu\n",
                  cfg_.output_width, cfg_.output_height, cfg_.source_width, cfg_.source_height,
                  cfg_.upscale_mode, cfg_.vsync ? "on" : "off", tearing_ ? "on" : "off",
                  Percentile(gpu_ms_, 0.00), Percentile(gpu_ms_, 0.50), Percentile(gpu_ms_, 0.95),
                  Percentile(gpu_ms_, 0.99), Percentile(gpu_ms_, 1.00), gpu_ms_.size(),
                  Percentile(cpu_ms_, 0.50), Percentile(cpu_ms_, 0.95), cpu_ms_.size(), frames_);
    std::printf("%s", buf);
}

}  // namespace brp
