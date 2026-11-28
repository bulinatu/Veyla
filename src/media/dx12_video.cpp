#include "media/dx12_video.h"

#include "media/dx12_shaders.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace brp {
namespace {

constexpr UINT kMaxBuffers = 3;

std::wstring AdapterName(IDXGIAdapter* adapter) {
    if (!adapter) {
        return L"default adapter";
    }
    DXGI_ADAPTER_DESC desc{};
    if (FAILED(adapter->GetDesc(&desc))) {
        return L"unknown adapter";
    }
    return desc.Description;
}

void SetDebugName(ID3D12Object* object, const wchar_t* name) {
    if (object && name) {
        object->SetName(name);
    }
}

}  // namespace

Dx12Video::~Dx12Video() {
    WaitGpu();
    ReleaseShared();
    if (frame_waitable_) {
        ::CloseHandle(frame_waitable_);
        frame_waitable_ = nullptr;
    }
    if (fence_event_) {
        ::CloseHandle(fence_event_);
        fence_event_ = nullptr;
    }
}

bool Dx12Video::Init(HWND hwnd, int width, int height, ID3D11Device* decode_device,
                     std::string& detail) {
    hwnd_ = hwnd;
    width_ = std::max(1, width);
    height_ = std::max(1, height);
    if (!CreateDevice(decode_device, detail)) {
        return false;
    }
    if (!CreateSwapchain(detail)) {
        return false;
    }
    if (!CreatePipeline(detail)) {
        return false;
    }
    status_ = "DX12 surface ready";
    std::fprintf(stderr, "[dx12] %dx%d, shader model %s, tearing=%s, waitable=%s\n",
                 width_, height_, shader_model_.c_str(), tearing_ ? "on" : "off",
                 waitable_ ? "on" : "off");
    return true;
}

bool Dx12Video::CreateDevice(ID3D11Device* decode_device, std::string& detail) {
#if defined(_DEBUG)
    {
        Microsoft::WRL::ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(::D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
            debug->EnableDebugLayer();
        }
    }
#endif
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    if (decode_device) {
        Microsoft::WRL::ComPtr<IDXGIDevice> dxgi;
        if (SUCCEEDED(decode_device->QueryInterface(IID_PPV_ARGS(&dxgi)))) {
            dxgi->GetAdapter(&adapter);
        }
    }
    const HRESULT hr = ::D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                                           IID_PPV_ARGS(&device_));
    if (FAILED(hr)) {
        detail = "D3D12CreateDevice failed (0x" + std::to_string(static_cast<unsigned>(hr)) + ")";
        return false;
    }

    D3D12_FEATURE_DATA_SHADER_MODEL model{};
    model.HighestShaderModel = static_cast<D3D_SHADER_MODEL>(0x68);  // 6.8
    if (SUCCEEDED(device_->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &model,
                                               sizeof(model)))) {
        char text[32];
        std::snprintf(text, sizeof(text), "6.%u",
                      static_cast<unsigned>(model.HighestShaderModel & 0x0F));
        shader_model_ = text;
    } else {
        shader_model_ = "unknown";
    }

    D3D12_COMMAND_QUEUE_DESC queue{};
    queue.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queue.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
    if (FAILED(device_->CreateCommandQueue(&queue, IID_PPV_ARGS(&queue_))) ||
        FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                               IID_PPV_ARGS(&allocator_))) ||
        FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                          allocator_.Get(), nullptr,
                                          IID_PPV_ARGS(&list_))) ||
        FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)))) {
        detail = "DX12 command objects failed";
        return false;
    }
    list_->Close();
    fence_event_ = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!fence_event_) {
        detail = "DX12 fence event failed";
        return false;
    }
    return true;
}

bool Dx12Video::CreateSwapchain(std::string& detail) {
    Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
    if (FAILED(::CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) {
        detail = "CreateDXGIFactory2 failed";
        return false;
    }
    Microsoft::WRL::ComPtr<IDXGIFactory5> factory5;
    if (SUCCEEDED(factory.As(&factory5))) {
        BOOL allow = FALSE;
        if (SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,
                                                    &allow, sizeof(allow)))) {
            tearing_ = allow == TRUE;
        }
    }

    const DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = static_cast<UINT>(width_);
    desc.Height = static_cast<UINT>(height_);
    desc.Format = format;
    desc.Stereo = FALSE;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = buffer_count_ = 2;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    if (tearing_) {
        desc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    }
    swapchain_flags_ = desc.Flags;

    Microsoft::WRL::ComPtr<IDXGISwapChain1> swap1;
    HRESULT hr = factory->CreateSwapChainForHwnd(queue_.Get(), hwnd_, &desc, nullptr,
                                                 nullptr, &swap1);
    if (FAILED(hr)) {
        desc.Flags = tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
        swapchain_flags_ = desc.Flags;
        hr = factory->CreateSwapChainForHwnd(queue_.Get(), hwnd_, &desc, nullptr, nullptr,
                                             &swap1);
    }
    if (FAILED(hr)) {
        char text[96];
        std::snprintf(text, sizeof(text), "CreateSwapChainForHwnd failed 0x%08lX",
                      static_cast<unsigned long>(hr));
        detail = text;
        return false;
    }
    swap1.As(&swapchain_);
    if (!swapchain_) {
        detail = "swapchain is not IDXGISwapChain3";
        return false;
    }
    factory->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);
    if (SUCCEEDED(swapchain_->SetMaximumFrameLatency(1))) {
        frame_waitable_ = swapchain_->GetFrameLatencyWaitableObject();
        waitable_ = frame_waitable_ != nullptr;
    }
    return CreateRtvHeap(detail);
}

bool Dx12Video::CreateRtvHeap(std::string& detail) {
    for (auto& buffer : backbuffers_) {
        buffer.Reset();
    }
    rtv_heap_.Reset();

    D3D12_DESCRIPTOR_HEAP_DESC heap{};
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heap.NumDescriptors = kMaxBuffers;
    heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (FAILED(device_->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&rtv_heap_)))) {
        detail = "RTV descriptor heap failed";
        return false;
    }
    rtv_stride_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE handle = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < buffer_count_; ++i) {
        if (FAILED(swapchain_->GetBuffer(i, IID_PPV_ARGS(&backbuffers_[i])))) {
            detail = "swapchain GetBuffer failed";
            return false;
        }
        device_->CreateRenderTargetView(backbuffers_[i].Get(), nullptr, handle);
        handle.ptr += rtv_stride_;
    }
    return true;
}

bool Dx12Video::CreatePipeline(std::string& detail) {
    D3D12_DESCRIPTOR_RANGE1 range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    range.BaseShaderRegister = 0;
    range.RegisterSpace = 0;
    range.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_NONE;
    range.OffsetInDescriptorsFromTableStart = 0;

    D3D12_ROOT_PARAMETER1 parameter{};
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.DescriptorTable.NumDescriptorRanges = 1;
    parameter.DescriptorTable.pDescriptorRanges = &range;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MipLODBias = 0.0f;
    sampler.MaxAnisotropy = 1;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    sampler.MinLOD = 0.0f;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.RegisterSpace = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{};
    desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    desc.Desc_1_1.NumParameters = 1;
    desc.Desc_1_1.pParameters = &parameter;
    desc.Desc_1_1.NumStaticSamplers = 1;
    desc.Desc_1_1.pStaticSamplers = &sampler;
    desc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    Microsoft::WRL::ComPtr<ID3DBlob> signature;
    Microsoft::WRL::ComPtr<ID3DBlob> error;
    HRESULT hr = ::D3D12SerializeVersionedRootSignature(&desc, &signature, &error);
    if (FAILED(hr)) {
        detail = "SerializeVersionedRootSignature failed";
        return false;
    }
    if (FAILED(device_->CreateRootSignature(0, signature->GetBufferPointer(),
                                            signature->GetBufferSize(),
                                            IID_PPV_ARGS(&root_signature_)))) {
        detail = "CreateRootSignature failed";
        return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = root_signature_.Get();
    pso.VS = {kDx12BlitVs, kDx12BlitVs_size};
    pso.PS = {kDx12BlitPs, kDx12BlitPs_size};
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.SampleMask = UINT_MAX;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.DepthStencilState.DepthEnable = FALSE;
    pso.DepthStencilState.StencilEnable = FALSE;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso.SampleDesc.Count = 1;
    if (FAILED(device_->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&pipeline_)))) {
        detail = "CreateGraphicsPipelineState failed";
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC heap{};
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap.NumDescriptors = 1;
    heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device_->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&srv_heap_)))) {
        detail = "SRV descriptor heap failed";
        return false;
    }
    srv_stride_ = device_->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    return true;
}

bool Dx12Video::EnsureSource(int width, int height, std::string& detail) {
    if (source_ && source_w_ == width && source_h_ == height) {
        return true;
    }
    source_.Reset();
    upload_.Reset();
    upload_ptr_ = nullptr;

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = static_cast<UINT64>(width);
    desc.Height = static_cast<UINT>(height);
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                IID_PPV_ARGS(&source_)))) {
        detail = "DX12 source texture failed";
        return false;
    }
    SetDebugName(source_.Get(), L"Veyla source");

    const UINT row = (static_cast<UINT>(width) * 4u + 255u) & ~255u;
    D3D12_HEAP_PROPERTIES up{};
    up.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC ub{};
    ub.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    ub.Width = static_cast<UINT64>(row) * static_cast<UINT>(height);
    ub.Height = 1;
    ub.DepthOrArraySize = 1;
    ub.MipLevels = 1;
    ub.Format = DXGI_FORMAT_UNKNOWN;
    ub.SampleDesc.Count = 1;
    ub.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(device_->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &ub,
                                                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                IID_PPV_ARGS(&upload_)))) {
        detail = "DX12 upload buffer failed";
        return false;
    }
    if (FAILED(upload_->Map(0, nullptr, reinterpret_cast<void**>(&upload_ptr_)))) {
        detail = "DX12 upload map failed";
        return false;
    }
    source_w_ = width;
    source_h_ = height;
    source_drawn_ = false;
    return true;
}

void Dx12Video::WaitGpu() {
    if (!fence_ || !fence_event_ || submitted_value_ == 0) {
        return;
    }
    if (fence_->GetCompletedValue() < submitted_value_) {
        if (SUCCEEDED(fence_->SetEventOnCompletion(submitted_value_, fence_event_))) {
            ::WaitForSingleObject(fence_event_, 2000);
        }
    }
}

bool Dx12Video::BeginFrame(bool clear) {
    WaitGpu();
    const HRESULT ar = allocator_->Reset();
    const HRESULT lr = FAILED(ar) ? E_FAIL : list_->Reset(allocator_.Get(), nullptr);
    if (FAILED(ar) || FAILED(lr)) {
        status_ = "DX12 command reset failed";
        return false;
    }
    const UINT index = swapchain_->GetCurrentBackBufferIndex();
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = backbuffers_[index].Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    list_->ResourceBarrier(1, &barrier);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(rtv_stride_) * index;
    list_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    if (clear) {
        const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        list_->ClearRenderTargetView(rtv, black, 0, nullptr);
    }
    return true;
}

void Dx12Video::DrawFullscreen(ID3D12Resource* source, int width, int height) {
    if (!source || width <= 0 || height <= 0) {
        return;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = srv_heap_->GetCPUDescriptorHandleForHeapStart();
    DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
    if (source) {
        format = source->GetDesc().Format;
        if (format == DXGI_FORMAT_B8G8R8A8_TYPELESS) {
            format = DXGI_FORMAT_B8G8R8A8_UNORM;
        } else if (format == DXGI_FORMAT_R8G8B8A8_TYPELESS) {
            format = DXGI_FORMAT_R8G8B8A8_UNORM;
        } else if (format == DXGI_FORMAT_UNKNOWN) {
            format = DXGI_FORMAT_R8G8B8A8_UNORM;
        }
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = format;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    device_->CreateShaderResourceView(source, &srv, cpu);

    ID3D12DescriptorHeap* heaps[] = {srv_heap_.Get()};
    list_->SetGraphicsRootSignature(root_signature_.Get());
    list_->SetPipelineState(pipeline_.Get());
    list_->SetDescriptorHeaps(1, heaps);
    list_->SetGraphicsRootDescriptorTable(
        0, srv_heap_->GetGPUDescriptorHandleForHeapStart());
    list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list_->DrawInstanced(3, 1, 0, 0);
}

bool Dx12Video::EndFrame() {
    const UINT index = swapchain_->GetCurrentBackBufferIndex();
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = backbuffers_[index].Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    list_->ResourceBarrier(1, &barrier);
    const HRESULT close_hr = list_->Close();
    if (FAILED(close_hr)) {
        char text[128];
        std::snprintf(text, sizeof(text), "DX12 command list close failed 0x%08lX",
                      static_cast<unsigned long>(close_hr));
        status_ = text;
        return false;
    }
    ID3D12CommandList* lists[] = {list_.Get()};
    queue_->ExecuteCommandLists(1, lists);
    submitted_value_ = ++fence_value_;
    if (FAILED(queue_->Signal(fence_.Get(), submitted_value_))) {
        status_ = "DX12 queue signal failed";
        return false;
    }
    has_frame_ = true;
    return true;
}

bool Dx12Video::DrawCpu(const std::uint8_t* rgba, int width, int height, bool pq) {
    (void)pq;
    if (!rgba || width <= 0 || height <= 0) {
        return false;
    }
    std::string detail;
    if (!EnsureSource(width, height, detail)) {
        status_ = detail;
        return false;
    }
    if (!BeginFrame(true)) {
        return false;
    }

    // Previous frame left the source in PIXEL_SHADER_RESOURCE.
    D3D12_RESOURCE_BARRIER barrier{};
    if (source_drawn_) {
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = source_.Get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        list_->ResourceBarrier(1, &barrier);
    }

    const UINT row = static_cast<UINT>(width) * 4u;
    const UINT pitch = (row + 255u) & ~255u;
    for (int y = 0; y < height; ++y) {
        std::memcpy(upload_ptr_ + static_cast<std::size_t>(pitch) * y,
                    rgba + static_cast<std::size_t>(row) * y, row);
    }
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = source_.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = upload_.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    src.PlacedFootprint.Footprint.Width = static_cast<UINT>(width);
    src.PlacedFootprint.Footprint.Height = static_cast<UINT>(height);
    src.PlacedFootprint.Footprint.Depth = 1;
    src.PlacedFootprint.Footprint.RowPitch = pitch;
    list_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    source_drawn_ = true;

    barrier.Transition.pResource = source_.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    list_->ResourceBarrier(1, &barrier);

    D3D12_VIEWPORT viewport{};
    const float scale = std::min(static_cast<float>(width_) / width,
                                 static_cast<float>(height_) / height);
    const float vw = std::max(1.0f, width * scale);
    const float vh = std::max(1.0f, height * scale);
    viewport.TopLeftX = (width_ - vw) * 0.5f;
    viewport.TopLeftY = (height_ - vh) * 0.5f;
    viewport.Width = vw;
    viewport.Height = vh;
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    list_->RSSetViewports(1, &viewport);
    D3D12_RECT scissor{static_cast<LONG>(viewport.TopLeftX),
                       static_cast<LONG>(viewport.TopLeftY),
                       static_cast<LONG>(viewport.TopLeftX + vw),
                       static_cast<LONG>(viewport.TopLeftY + vh)};
    list_->RSSetScissorRects(1, &scissor);

    DrawFullscreen(source_.Get(), width, height);
    return EndFrame();
}

bool Dx12Video::OpenShared(HANDLE handle, std::string& detail) {
    if (!handle) {
        detail = "shared handle is null";
        return false;
    }
    for (auto& entry : shared_sources_) {
        if (entry.handle == handle) {
            shared_resource_ = entry.resource;
            return true;
        }
    }
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    if (FAILED(device_->OpenSharedHandle(handle, IID_PPV_ARGS(&resource)))) {
        detail = "OpenSharedHandle failed";
        return false;
    }
    SetDebugName(resource.Get(), L"Veyla shared source");
    if (shared_sources_.size() >= 4) {
        shared_sources_.erase(shared_sources_.begin());
    }
    shared_sources_.push_back({handle, resource});
    shared_resource_ = std::move(resource);
    return true;
}

void Dx12Video::ReleaseShared() {
    shared_sources_.clear();
    shared_resource_.Reset();
}

bool Dx12Video::DrawShared(HANDLE shared, int width, int height, bool pq) {
    (void)pq;
    if (!shared || width <= 0 || height <= 0) {
        return false;
    }
    std::string detail;
    if (!OpenShared(shared, detail)) {
        status_ = detail;
        return false;
    }
    if (!BeginFrame(true)) {
        return false;
    }
    // The producer transitions the texture to COMMON before signalling.
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = shared_resource_.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    list_->ResourceBarrier(1, &barrier);

    D3D12_VIEWPORT viewport{};
    const float scale = std::min(static_cast<float>(width_) / width,
                                 static_cast<float>(height_) / height);
    const float vw = std::max(1.0f, width * scale);
    const float vh = std::max(1.0f, height * scale);
    viewport.TopLeftX = (width_ - vw) * 0.5f;
    viewport.TopLeftY = (height_ - vh) * 0.5f;
    viewport.Width = vw;
    viewport.Height = vh;
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    list_->RSSetViewports(1, &viewport);
    D3D12_RECT scissor{static_cast<LONG>(viewport.TopLeftX),
                       static_cast<LONG>(viewport.TopLeftY),
                       static_cast<LONG>(viewport.TopLeftX + vw),
                       static_cast<LONG>(viewport.TopLeftY + vh)};
    list_->RSSetScissorRects(1, &scissor);

    DrawFullscreen(shared_resource_.Get(), width, height);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    list_->ResourceBarrier(1, &barrier);
    return EndFrame();
}

bool Dx12Video::DrawTexture(ID3D12Resource* texture, int width, int height) {
    if (!texture || width <= 0 || height <= 0) {
        return false;
    }
    if (!BeginFrame(true)) {
        return false;
    }
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = texture;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    list_->ResourceBarrier(1, &barrier);

    D3D12_VIEWPORT viewport{};
    const float scale = std::min(static_cast<float>(width_) / width,
                                 static_cast<float>(height_) / height);
    const float vw = std::max(1.0f, width * scale);
    const float vh = std::max(1.0f, height * scale);
    viewport.TopLeftX = (width_ - vw) * 0.5f;
    viewport.TopLeftY = (height_ - vh) * 0.5f;
    viewport.Width = vw;
    viewport.Height = vh;
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    list_->RSSetViewports(1, &viewport);
    D3D12_RECT scissor{static_cast<LONG>(viewport.TopLeftX),
                       static_cast<LONG>(viewport.TopLeftY),
                       static_cast<LONG>(viewport.TopLeftX + vw),
                       static_cast<LONG>(viewport.TopLeftY + vh)};
    list_->RSSetScissorRects(1, &scissor);
    DrawFullscreen(texture, width, height);
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    list_->ResourceBarrier(1, &barrier);
    return EndFrame();
}

bool Dx12Video::CaptureToBmp(const char* path) {
    if (!swapchain_ || !device_ || !path) {
        return false;
    }
    WaitGpu();
    const UINT index = swapchain_->GetCurrentBackBufferIndex();
    ID3D12Resource* backbuffer = backbuffers_[index].Get();
    if (!backbuffer) {
        return false;
    }
    D3D12_RESOURCE_DESC desc = backbuffer->GetDesc();
    const UINT width = static_cast<UINT>(desc.Width);
    const UINT height = desc.Height;
    const UINT pitch = (width * 4u + 255u) & ~255u;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC buffer_desc{};
    buffer_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer_desc.Width = static_cast<UINT64>(pitch) * height;
    buffer_desc.Height = 1;
    buffer_desc.DepthOrArraySize = 1;
    buffer_desc.MipLevels = 1;
    buffer_desc.Format = DXGI_FORMAT_UNKNOWN;
    buffer_desc.SampleDesc.Count = 1;
    buffer_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    Microsoft::WRL::ComPtr<ID3D12Resource> staging;
    if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer_desc,
                                                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                IID_PPV_ARGS(&staging)))) {
        return false;
    }
    if (FAILED(allocator_->Reset()) || FAILED(list_->Reset(allocator_.Get(), nullptr))) {
        return false;
    }
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = backbuffer;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    list_->ResourceBarrier(1, &barrier);
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = backbuffer;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = staging.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    dst.PlacedFootprint.Footprint.Width = width;
    dst.PlacedFootprint.Footprint.Height = height;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = pitch;
    list_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    list_->ResourceBarrier(1, &barrier);
    if (FAILED(list_->Close())) {
        return false;
    }
    ID3D12CommandList* lists[] = {list_.Get()};
    queue_->ExecuteCommandLists(1, lists);
    submitted_value_ = ++fence_value_;
    if (FAILED(queue_->Signal(fence_.Get(), submitted_value_))) {
        return false;
    }
    WaitGpu();

    D3D12_RANGE range{0, static_cast<SIZE_T>(buffer_desc.Width)};
    void* mapped = nullptr;
    if (FAILED(staging->Map(0, &range, &mapped)) || !mapped) {
        return false;
    }
    std::FILE* file = nullptr;
    if (fopen_s(&file, path, "wb") != 0 || !file) {
        staging->Unmap(0, nullptr);
        return false;
    }
    const std::uint32_t image_bytes = width * height * 4u;
    unsigned char header[54] = {};
    header[0] = 'B';
    header[1] = 'M';
    const std::uint32_t file_size = 54 + image_bytes;
    std::memcpy(header + 2, &file_size, 4);
    const std::uint32_t offset = 54;
    std::memcpy(header + 10, &offset, 4);
    const std::uint32_t info = 40;
    std::memcpy(header + 14, &info, 4);
    std::memcpy(header + 18, &width, 4);
    std::memcpy(header + 22, &height, 4);
    const std::uint16_t planes = 1;
    std::memcpy(header + 26, &planes, 2);
    const std::uint16_t bpp = 32;
    std::memcpy(header + 28, &bpp, 2);
    std::memcpy(header + 34, &image_bytes, 4);
    std::fwrite(header, 1, sizeof(header), file);
    const auto* pixels = static_cast<const unsigned char*>(mapped);
    std::vector<unsigned char> line(width * 4u);
    for (int y = static_cast<int>(height) - 1; y >= 0; --y) {
        const unsigned char* in = pixels + static_cast<std::size_t>(pitch) * y;
        for (UINT x = 0; x < width; ++x) {
            line[x * 4 + 0] = in[x * 4 + 2];
            line[x * 4 + 1] = in[x * 4 + 1];
            line[x * 4 + 2] = in[x * 4 + 0];
            line[x * 4 + 3] = 0xFF;
        }
        std::fwrite(line.data(), 1, line.size(), file);
    }
    std::fclose(file);
    staging->Unmap(0, nullptr);
    return true;
}

bool Dx12Video::Present(bool vsync) {
    if (!swapchain_ || device_lost_) {
        return false;
    }
    UINT flags = 0;
    if (!vsync && tearing_) {
        flags |= DXGI_PRESENT_ALLOW_TEARING;
    }
    const HRESULT hr = swapchain_->Present(vsync ? 1 : 0, flags);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        device_lost_ = true;
        std::fprintf(stderr, "[dx12] device lost (0x%08lX)\n",
                     static_cast<unsigned long>(hr));
        return false;
    }
    UpdatePresentStats();
    return true;
}

void Dx12Video::UpdatePresentStats() {
    DXGI_FRAME_STATISTICS stats{};
    if (FAILED(swapchain_->GetFrameStatistics(&stats))) {
        return;
    }
    if (have_stats_ && last_stats_.PresentCount != 0) {
        const UINT presents = stats.PresentCount - last_stats_.PresentCount;
        const UINT refreshes = stats.PresentRefreshCount - last_stats_.PresentRefreshCount;
        if (refreshes > presents) {
            present_misses_ += refreshes - presents;
        }
    }
    last_stats_ = stats;
    last_refresh_ = stats.PresentRefreshCount;
    have_stats_ = true;

    LARGE_INTEGER frequency{};
    LARGE_INTEGER now{};
    ::QueryPerformanceFrequency(&frequency);
    ::QueryPerformanceCounter(&now);
    const double qpc_ms = 1000.0 / static_cast<double>(frequency.QuadPart);
    if (frequency.QuadPart > 0) {
        if (have_sync_ && stats.SyncRefreshCount > last_sync_refresh_ &&
            stats.SyncQPCTime.QuadPart > last_sync_qpc_.QuadPart) {
            const double vblanks =
                static_cast<double>(stats.SyncRefreshCount - last_sync_refresh_);
            refresh_period_ms_ = static_cast<double>(stats.SyncQPCTime.QuadPart -
                                                     last_sync_qpc_.QuadPart) *
                                 qpc_ms / vblanks;
        }
        if (stats.SyncQPCTime.QuadPart > 0) {
            last_sync_refresh_ = stats.SyncRefreshCount;
            last_sync_qpc_ = stats.SyncQPCTime;
            have_sync_ = true;
        }
        if (have_sync_ && refresh_period_ms_ > 0.5) {
            const long long ahead = static_cast<long long>(stats.PresentRefreshCount) -
                                    static_cast<long long>(stats.SyncRefreshCount);
            const double sync_ms =
                static_cast<double>(stats.SyncQPCTime.QuadPart) * qpc_ms;
            double display_ms = sync_ms + static_cast<double>(ahead) * refresh_period_ms_;
            if (present_qpc_ != 0) {
                const double present_ms = static_cast<double>(present_qpc_) * qpc_ms;
                if (display_ms < present_ms) {
                    display_ms += refresh_period_ms_;
                }
                present_lag_ms_ = display_ms - present_ms;
                present_queue_ = static_cast<int>(ahead > 0 ? ahead : 0);
            }
        }
        present_qpc_ = static_cast<UINT64>(now.QuadPart);
    }
}

bool Dx12Video::WaitForFrame(DWORD timeout_ms) {
    if (!frame_waitable_) {
        return true;
    }
    return ::WaitForSingleObject(frame_waitable_, timeout_ms) == WAIT_OBJECT_0;
}

bool Dx12Video::Resize(int width, int height) {
    if (!swapchain_) {
        return false;
    }
    width = std::max(1, width);
    height = std::max(1, height);
    if (width == width_ && height == height_) {
        return true;
    }
    WaitGpu();
    for (auto& buffer : backbuffers_) {
        buffer.Reset();
    }
    rtv_heap_.Reset();
    width_ = width;
    height_ = height;
    HRESULT hr = swapchain_->ResizeBuffers(0, static_cast<UINT>(width),
                                           static_cast<UINT>(height), DXGI_FORMAT_UNKNOWN,
                                           swapchain_flags_);
    if (FAILED(hr)) {
        // Some drivers reject the waitable flag in ResizeBuffers even though
        // the swapchain was created with it. Retry without changing formats.
        const UINT flags = swapchain_flags_ & ~DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        hr = swapchain_->ResizeBuffers(0, static_cast<UINT>(width),
                                       static_cast<UINT>(height), DXGI_FORMAT_UNKNOWN, flags);
    }
    if (FAILED(hr)) {
        status_ = "ResizeBuffers failed";
        return false;
    }
    std::string detail;
    return CreateRtvHeap(detail);
}

}  // namespace brp
