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
};

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

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = static_cast<UINT>(cfg_.output_width);
    desc.Height = static_cast<UINT>(cfg_.output_height);
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.Stereo = FALSE;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_SHADER_INPUT;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    desc.Flags = (tearing_ && cfg_.allow_tearing) ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;

    hr = factory->CreateSwapChainForHwnd(device_.Get(), hwnd_, &desc, nullptr, nullptr, &swapchain_);
    if (FAILED(hr)) {
        std::fprintf(stderr, "CreateSwapChainForHwnd failed: 0x%08lX\n",
                     static_cast<unsigned long>(hr));
        return false;
    }
    factory->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);

    if (!CreateBackbufferViews() || !CreateSourceTexture() || !CreateShaders() ||
        !CreateTimingQueries()) {
        return false;
    }

    start_time_ = std::chrono::steady_clock::now();
    std::printf("presenter: %dx%d <- %dx%d, tearing=%s, vsync=%s\n", cfg_.output_width,
                cfg_.output_height, cfg_.source_width, cfg_.source_height,
                tearing_ ? "on" : "off", cfg_.vsync ? "on" : "off");
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

    ID3D11RenderTargetView* null_rtv = nullptr;
    context_->OMSetRenderTargets(1, &null_rtv, nullptr);

    rtv_.Reset();
    backbuffer_.Reset();

    const UINT flags = (tearing_ && cfg_.allow_tearing) ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    HRESULT hr = swapchain_->ResizeBuffers(0, static_cast<UINT>(width), static_cast<UINT>(height),
                                           DXGI_FORMAT_UNKNOWN, flags);
    if (FAILED(hr)) {
        std::fprintf(stderr, "ResizeBuffers failed: 0x%08lX\n", static_cast<unsigned long>(hr));
        return false;
    }

    cfg_.output_width = width;
    cfg_.output_height = height;
    return CreateBackbufferViews();
}

void Presenter::SetUpscaleMode(int mode) {
    cfg_.upscale_mode = std::clamp(mode, 0, 3);
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

void Presenter::Render(double time_seconds) {
    const auto cpu_start = std::chrono::steady_clock::now();
    BeginTiming();

    D3D11_MAPPED_SUBRESOURCE mapped{};

    // 1. Synthetic source frame (stands in for hw decode output).
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

    // 2. Upscale + optional sharpen into the backbuffer.
    PsParams ps{};
    ps.sharpen = (cfg_.upscale_mode == 3) ? 0.45f : 0.35f;
    ps.mode = static_cast<float>(cfg_.upscale_mode);
    ps.src_w = static_cast<float>(cfg_.source_width);
    ps.src_h = static_cast<float>(cfg_.source_height);
    if (SUCCEEDED(context_->Map(ps_cb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        std::memcpy(mapped.pData, &ps, sizeof(ps));
        context_->Unmap(ps_cb_.Get(), 0);
    }

    ID3D11RenderTargetView* rtv = rtv_.Get();
    context_->OMSetRenderTargets(1, &rtv, nullptr);

    D3D11_VIEWPORT vp{};
    vp.Width = static_cast<float>(cfg_.output_width);
    vp.Height = static_cast<float>(cfg_.output_height);
    vp.MaxDepth = 1.0f;
    context_->RSSetViewports(1, &vp);

    ID3D11ShaderResourceView* srv = source_srv_.Get();
    context_->PSSetShaderResources(0, 1, &srv);
    ID3D11SamplerState* sampler = sampler_.Get();
    context_->PSSetSamplers(0, 1, &sampler);
    ID3D11Buffer* ps_buffers[] = {ps_cb_.Get()};
    context_->PSSetConstantBuffers(0, 1, ps_buffers);
    context_->VSSetShader(vs_.Get(), nullptr, 0);
    context_->PSSetShader(ps_.Get(), nullptr, 0);
    context_->IASetInputLayout(nullptr);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->Draw(3, 0);

    EndTiming();

    cpu_ms_.push_back(std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - cpu_start)
                          .count());
    if (cpu_ms_.size() > 4000) {
        cpu_ms_.erase(cpu_ms_.begin(), cpu_ms_.begin() + 1000);
    }
}

void Presenter::Clear(const float rgba[4]) {
    ID3D11RenderTargetView* rtv = rtv_.Get();
    context_->OMSetRenderTargets(1, &rtv, nullptr);
    context_->ClearRenderTargetView(rtv, rgba);
}

void Presenter::PresentNow(bool vsync) {
    const UINT present_flags =
        (tearing_ && cfg_.allow_tearing && !vsync) ? DXGI_PRESENT_ALLOW_TEARING : 0;
    swapchain_->Present(vsync ? 1 : 0, present_flags);

    CollectTiming();
    ++frames_;
}

void Presenter::Present() {
    PresentNow(cfg_.vsync);
}

bool Presenter::SetFullscreen(bool enabled) {
    if (fullscreen_ == enabled) {
        return true;
    }
    ID3D11RenderTargetView* null_rtv = nullptr;
    context_->OMSetRenderTargets(1, &null_rtv, nullptr);
    rtv_.Reset();
    backbuffer_.Reset();

    if (FAILED(swapchain_->SetFullscreenState(enabled ? TRUE : FALSE, nullptr))) {
        return false;
    }
    fullscreen_ = enabled;

    DXGI_SWAP_CHAIN_DESC1 desc{};
    if (SUCCEEDED(swapchain_->GetDesc1(&desc))) {
        cfg_.output_width = static_cast<int>(desc.Width);
        cfg_.output_height = static_cast<int>(desc.Height);
    }
    return CreateBackbufferViews();
}

bool Presenter::CaptureToBmp(const char* path) {
    if (!backbuffer_) {
        return false;
    }
    D3D11_TEXTURE2D_DESC td{};
    backbuffer_->GetDesc(&td);
    backbuffer_->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.MiscFlags = 0;

    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device_->CreateTexture2D(&td, nullptr, &staging))) {
        std::fprintf(stderr, "[capture] staging texture failed\n");
        return false;
    }

    context_->CopyResource(staging.Get(), backbuffer_.Get());

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

std::string Presenter::Summary() const {    const double p50 = Percentile(gpu_ms_, 0.50);
    const double p95 = Percentile(gpu_ms_, 0.95);
    const double cpu = cpu_ms_.empty() ? 0.0 : cpu_ms_.back();

    double fps = 0.0;
    if (frames_ > 1) {
        const double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                                        start_time_)
                              .count();
        if (dt > 0.0) {
            fps = static_cast<double>(frames_ - 1) / dt;
        }
    }

    char buf[320];
    std::snprintf(buf, sizeof(buf),
                  "gpu p50 %.2f  p95 %.2f ms | cpu %.2f ms | %.1f fps | %dx%d <- %dx%d | mode %d",
                  p50, p95, cpu, fps, cfg_.output_width, cfg_.output_height, cfg_.source_width,
                  cfg_.source_height, cfg_.upscale_mode);
    return buf;
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
