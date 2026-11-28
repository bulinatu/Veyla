#include "media/amd_fsr.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>

#include "ffx_api_loader.h"
#include "dx12/ffx_api_dx12.h"
#include "ffx_upscale.h"

namespace brp::media {
namespace {

double NowMs() {
    using clock = std::chrono::steady_clock;
    static const clock::time_point start = clock::now();
    return std::chrono::duration<double, std::milli>(clock::now() - start).count();
}

std::string Lower(std::string text) {
    for (char& ch : text) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return text;
}

}  // namespace

struct AmdFsr::Api {
    HMODULE module = nullptr;
    PfnFfxCreateContext create = nullptr;
    PfnFfxDestroyContext destroy = nullptr;
    PfnFfxConfigure configure = nullptr;
    PfnFfxQuery query = nullptr;
    PfnFfxDispatch dispatch = nullptr;
};

AmdFsr::AmdFsr() = default;

AmdFsr::~AmdFsr() {
    Shutdown();
}

bool AmdFsr::LoadRuntime(const std::wstring& directory, std::string& detail) {
    if (runtime_loaded_) {
        return true;
    }
    const std::filesystem::path dir(directory);
    const std::filesystem::path loader = dir / L"amd_fidelityfx_loader_dx12.dll";
    const std::filesystem::path upscaler = dir / L"amd_fidelityfx_upscaler_dx12.dll";
    std::error_code ec;
    if (!std::filesystem::exists(loader, ec)) {
        detail = "amd_fidelityfx_loader_dx12.dll not found in " + dir.string();
        return false;
    }
    if (!std::filesystem::exists(upscaler, ec)) {
        detail = "amd_fidelityfx_upscaler_dx12.dll not found in " + dir.string();
        return false;
    }

    ::SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_USER_DIRS);
    ::AddDllDirectory(dir.c_str());
    api_ = std::make_unique<Api>();
    api_->module = ::LoadLibraryExW(loader.c_str(), nullptr,
                                    LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                        LOAD_LIBRARY_SEARCH_DEFAULT_DIRS |
                                        LOAD_LIBRARY_SEARCH_USER_DIRS);
    if (!api_->module) {
        detail = "cannot load the FSR loader (error " + std::to_string(::GetLastError()) + ")";
        return false;
    }
    api_->create = reinterpret_cast<PfnFfxCreateContext>(
        ::GetProcAddress(api_->module, "ffxCreateContext"));
    api_->destroy = reinterpret_cast<PfnFfxDestroyContext>(
        ::GetProcAddress(api_->module, "ffxDestroyContext"));
    api_->configure = reinterpret_cast<PfnFfxConfigure>(
        ::GetProcAddress(api_->module, "ffxConfigure"));
    api_->query =
        reinterpret_cast<PfnFfxQuery>(::GetProcAddress(api_->module, "ffxQuery"));
    api_->dispatch = reinterpret_cast<PfnFfxDispatch>(
        ::GetProcAddress(api_->module, "ffxDispatch"));
    if (!api_->create || !api_->destroy || !api_->query || !api_->dispatch) {
        detail = "FSR loader is missing exports";
        return false;
    }
    runtime_loaded_ = true;
    status_ = "FSR loader loaded";
    return true;
}

bool AmdFsr::EnsureDevice(std::string& detail) {
    if (device_ready_) {
        return true;
    }
    if (!device_) {
        detail = "FSR needs a D3D11 device for the adapter";
        return false;
    }
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    if (FAILED(device_.As(&dxgi_device))) {
        detail = "cannot resolve the D3D11 adapter";
        return false;
    }
    dxgi_device->GetAdapter(&adapter);
    if (!adapter) {
        detail = "cannot resolve the adapter";
        return false;
    }
    if (FAILED(::D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                                   IID_PPV_ARGS(&device12_)))) {
        detail = "D3D12CreateDevice failed for FSR";
        return false;
    }
    D3D12_COMMAND_QUEUE_DESC q{};
    q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    q.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
    if (FAILED(device12_->CreateCommandQueue(&q, IID_PPV_ARGS(&queue_))) ||
        FAILED(device12_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                 IID_PPV_ARGS(&allocator_))) ||
        FAILED(device12_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                            allocator_.Get(), nullptr,
                                            IID_PPV_ARGS(&list_))) ||
        FAILED(device12_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)))) {
        detail = "FSR command objects failed";
        return false;
    }
    list_->Close();
    fence_event_ = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!fence_event_) {
        detail = "FSR fence event failed";
        return false;
    }
    device_ready_ = true;
    return QueryVersions();
}

bool AmdFsr::QueryVersions() {
    ffxQueryDescGetVersions query{};
    query.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    query.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    query.device = device12_.Get();
    uint64_t count = 0;
    query.outputCount = &count;
    if (api_->query(nullptr, &query.header) != FFX_API_RETURN_OK || count == 0) {
        return true;  // no enumeration: the API default still works
    }
    std::vector<uint64_t> ids(count);
    std::vector<const char*> names(count, nullptr);
    query.outputCount = &count;
    query.versionIds = ids.data();
    query.versionNames = names.data();
    if (api_->query(nullptr, &query.header) != FFX_API_RETURN_OK) {
        return true;
    }
    version_ids_.assign(ids.begin(), ids.begin() + count);
    version_names_.clear();
    for (uint64_t i = 0; i < count; ++i) {
        version_names_.push_back(names[i] ? names[i] : "");
        std::fprintf(stderr, "[fsr] version %llu: %s\n",
                     static_cast<unsigned long long>(version_ids_[i]),
                     version_names_.back().c_str());
    }
    return true;
}

bool AmdFsr::CreateContext(Variant variant, std::string& detail) {
    ffxCreateBackendDX12Desc backend{};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backend.device = device12_.Get();

    ffxCreateContextDescUpscaleVersion version{};
    version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
    version.version = FFX_UPSCALER_VERSION;

    ffxCreateContextDescUpscale desc{};
    desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    desc.flags = FFX_UPSCALE_ENABLE_AUTO_EXPOSURE |
                 FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE;
    if (hdr_input_) {
        desc.flags = FFX_UPSCALE_ENABLE_AUTO_EXPOSURE |
                     FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE;
    }
    desc.maxRenderSize = {static_cast<uint32_t>(in_width_),
                          static_cast<uint32_t>(in_height_)};
    desc.maxUpscaleSize = {static_cast<uint32_t>(out_width_),
                           static_cast<uint32_t>(out_height_)};
    desc.header.pNext = &version.header;

    // Pick a provider only when the caller asked for a family. Leaving no
    // override is what enables the AMD driver's automatic newest-version path.
    std::string wanted;
    if (variant == Variant::Fsr3) {
        wanted = "3.";
    } else if (variant == Variant::Fsr4) {
        wanted = "4.";
    }
    ffxOverrideVersion override_desc{};
    bool has_override = false;
    if (!wanted.empty()) {
        for (std::size_t i = 0; i < version_names_.size(); ++i) {
            const std::string name = Lower(version_names_[i]);
            if (name.find(wanted) != std::string::npos) {
                override_desc.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
                override_desc.versionId = version_ids_[i];
                has_override = true;
                chosen_version_ = version_names_[i];
                break;
            }
        }
    }
    if (has_override) {
        version.header.pNext = &override_desc.header;
        override_desc.header.pNext = &backend.header;
    } else {
        version.header.pNext = &backend.header;
    }

    const double started = NowMs();
    ffxReturnCode_t result = api_->create(&context_handle_, &desc.header, nullptr);
    if (result != FFX_API_RETURN_OK && has_override && variant == Variant::Fsr4) {
        // Pinning 4.x on hardware that cannot run it is a hard failure; fall
        // back to the API's automatic choice (3.1.6 on NVIDIA).
        std::fprintf(stderr, "[fsr] pinned 4.x failed (%u), falling back to auto\n",
                     result);
        context_handle_ = nullptr;
        version.header.pNext = &backend.header;
        result = api_->create(&context_handle_, &desc.header, nullptr);
    }
    if (result != FFX_API_RETURN_OK || !context_handle_) {
        detail = "ffxCreateContext failed (" + std::to_string(result) + ")";
        status_ = detail;
        return false;
    }

    ffxQueryGetProviderVersion provider{};
    provider.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    if (api_->query(&context_handle_, &provider.header) == FFX_API_RETURN_OK &&
        provider.versionName) {
        provider_ = provider.versionName;
    } else {
        provider_ = chosen_version_.empty() ? "FSR" : chosen_version_;
    }
    std::fprintf(stderr, "[fsr] context %dx%d -> %dx%d, provider %s, %.0f ms\n",
                 in_width_, in_height_, out_width_, out_height_, provider_.c_str(),
                 NowMs() - started);
    status_ = "FSR ready: " + provider_;
    return true;
}

bool AmdFsr::CreateResources(std::string& detail) {
    auto create_texture = [&](DXGI_FORMAT format, int width, int height,
                              D3D12_RESOURCE_STATES state, D3D12_RESOURCE_FLAGS flags,
                              D3D12_HEAP_FLAGS heap_flags,
                              Microsoft::WRL::ComPtr<ID3D12Resource>& out) -> bool {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = static_cast<UINT64>(width);
        desc.Height = static_cast<UINT>(height);
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Flags = flags;
        return SUCCEEDED(device12_->CreateCommittedResource(
            &heap, heap_flags, &desc, state, nullptr, IID_PPV_ARGS(&out)));
    };

    if (!create_texture(DXGI_FORMAT_R8G8B8A8_UNORM, in_width_, in_height_,
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                        D3D12_RESOURCE_FLAG_NONE, D3D12_HEAP_FLAG_NONE, input_)) {
        detail = "FSR input texture failed";
        return false;
    }
    const UINT row = (static_cast<UINT>(in_width_) * 4u + 255u) & ~255u;
    D3D12_HEAP_PROPERTIES up{};
    up.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC ub{};
    ub.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    ub.Width = static_cast<UINT64>(row) * static_cast<UINT>(in_height_);
    ub.Height = 1;
    ub.DepthOrArraySize = 1;
    ub.MipLevels = 1;
    ub.Format = DXGI_FORMAT_UNKNOWN;
    ub.SampleDesc.Count = 1;
    ub.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(device12_->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &ub,
                                                  D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                  IID_PPV_ARGS(&upload_)))) {
        detail = "FSR upload buffer failed";
        return false;
    }
    if (FAILED(upload_->Map(0, nullptr, reinterpret_cast<void**>(&upload_ptr_)))) {
        detail = "FSR upload map failed";
        return false;
    }

    if (!create_texture(DXGI_FORMAT_R32_FLOAT, in_width_, in_height_,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                        D3D12_HEAP_FLAG_NONE, depth_) ||
        !create_texture(DXGI_FORMAT_R16G16_FLOAT, in_width_, in_height_,
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                        D3D12_HEAP_FLAG_NONE, motion_)) {
        detail = "FSR guide textures failed";
        return false;
    }
    if (!create_texture(DXGI_FORMAT_R8G8B8A8_UNORM, out_width_, out_height_,
                        D3D12_RESOURCE_STATE_COMMON,
                        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                        D3D12_HEAP_FLAG_SHARED, output_)) {
        detail = "FSR output texture failed";
        return false;
    }
    if (FAILED(device12_->CreateSharedHandle(output_.Get(), nullptr, GENERIC_ALL,
                                             nullptr, &output_handle_))) {
        detail = "FSR output shared handle failed";
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC heap{};
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap.NumDescriptors = 2;
    heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device12_->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&guide_heap_)))) {
        detail = "FSR guide heap failed";
        return false;
    }
    return ClearGuides(detail);
}

bool AmdFsr::ClearGuides(std::string& detail) {
    if (FAILED(allocator_->Reset()) || FAILED(list_->Reset(allocator_.Get(), nullptr))) {
        detail = "FSR guide list reset failed";
        return false;
    }
    const UINT stride = device12_->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = guide_heap_->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = guide_heap_->GetGPUDescriptorHandleForHeapStart();
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_motion = cpu;
    cpu_motion.ptr += stride;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_motion = gpu;
    gpu_motion.ptr += stride;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uav.Format = DXGI_FORMAT_R32_FLOAT;
    device12_->CreateUnorderedAccessView(depth_.Get(), nullptr, &uav, cpu);
    uav.Format = DXGI_FORMAT_R16G16_FLOAT;
    device12_->CreateUnorderedAccessView(motion_.Get(), nullptr, &uav, cpu_motion);
    ID3D12DescriptorHeap* heaps[] = {guide_heap_.Get()};
    list_->SetDescriptorHeaps(1, heaps);
    const float zero[4] = {0, 0, 0, 0};
    list_->ClearUnorderedAccessViewFloat(gpu, cpu, depth_.Get(), zero, 0, nullptr);
    list_->ClearUnorderedAccessViewFloat(gpu_motion, cpu_motion, motion_.Get(), zero, 0,
                                         nullptr);
    D3D12_RESOURCE_BARRIER barriers[2]{};
    barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[0].Transition.pResource = depth_.Get();
    barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[1].Transition.pResource = motion_.Get();
    barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    list_->ResourceBarrier(2, barriers);
    return Submit(detail) && Wait(detail);
}

bool AmdFsr::Initialize(ID3D11Device* device, ID3D11DeviceContext* context, int in_width,
                        int in_height, int out_width, int out_height, Variant variant,
                        bool hdr_input, std::string& detail) {
    if (!runtime_loaded_ || !device || !context || in_width <= 0 || in_height <= 0 ||
        out_width <= 0 || out_height <= 0) {
        detail = "FSR is not initialized";
        return false;
    }
    if (context_handle_ && in_width == in_width_ && in_height == in_height_ &&
        out_width == out_width_ && out_height == out_height_) {
        return true;
    }
    Shutdown();
    device_ = device;
    context_ = context;
    in_width_ = in_width;
    in_height_ = in_height;
    out_width_ = out_width;
    out_height_ = out_height;
    hdr_input_ = hdr_input;
    if (!EnsureDevice(detail)) {
        return false;
    }
    if (!CreateContext(variant, detail)) {
        return false;
    }
    if (!CreateResources(detail)) {
        Shutdown();
        return false;
    }
    return true;
}

void AmdFsr::Shutdown() {
    if (fence_event_) {
        ::CloseHandle(fence_event_);
        fence_event_ = nullptr;
    }
    output_.Reset();
    motion_.Reset();
    depth_.Reset();
    upload_.Reset();
    upload_ptr_ = nullptr;
    input_.Reset();
    guide_heap_.Reset();
    if (output_handle_) {
        ::CloseHandle(output_handle_);
        output_handle_ = nullptr;
    }
    if (context_handle_ && api_ && api_->destroy) {
        api_->destroy(&context_handle_, nullptr);
        context_handle_ = nullptr;
    }
    provider_.clear();
    in_width_ = in_height_ = out_width_ = out_height_ = 0;
    // Keep the D3D12 device/queue alive for a re-init on the next frame size.
}

bool AmdFsr::Submit(std::string& detail) {
    if (FAILED(list_->Close())) {
        detail = "FSR command list close failed";
        return false;
    }
    ID3D12CommandList* lists[] = {list_.Get()};
    queue_->ExecuteCommandLists(1, lists);
    submitted_ = ++fence_value_;
    if (FAILED(queue_->Signal(fence_.Get(), submitted_))) {
        detail = "FSR queue signal failed";
        return false;
    }
    return true;
}

bool AmdFsr::Wait(std::string& detail) {
    if (!submitted_ || !fence_event_) {
        return true;
    }
    if (fence_->GetCompletedValue() < submitted_) {
        if (FAILED(fence_->SetEventOnCompletion(submitted_, fence_event_))) {
            detail = "FSR fence wait setup failed";
            return false;
        }
        if (::WaitForSingleObject(fence_event_, 5000) != WAIT_OBJECT_0) {
            detail = "FSR fence timed out";
            return false;
        }
    }
    return true;
}

bool AmdFsr::Process(const std::uint8_t* rgba, int width, int height,
                     float frame_time_ms, std::string& detail) {
    if (!context_handle_ || !rgba || width != in_width_ || height != in_height_) {
        detail = "FSR is not initialized for this frame size";
        return false;
    }
    if (!Wait(detail)) {
        return false;
    }
    if (FAILED(allocator_->Reset()) || FAILED(list_->Reset(allocator_.Get(), nullptr))) {
        detail = "FSR command reset failed";
        return false;
    }
    const double started = NowMs();

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = input_.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    list_->ResourceBarrier(1, &barrier);

    const UINT row = static_cast<UINT>(width) * 4u;
    const UINT pitch = (row + 255u) & ~255u;
    for (int y = 0; y < height; ++y) {
        std::memcpy(upload_ptr_ + static_cast<std::size_t>(pitch) * y,
                    rgba + static_cast<std::size_t>(row) * y, row);
    }
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = input_.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = upload_.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    src.PlacedFootprint.Footprint.Width = static_cast<UINT>(width);
    src.PlacedFootprint.Footprint.Height = static_cast<UINT>(height);
    src.PlacedFootprint.Footprint.Depth = 1;
    src.PlacedFootprint.Footprint.RowPitch = pitch;
    list_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    list_->ResourceBarrier(1, &barrier);

    D3D12_RESOURCE_BARRIER out{};
    out.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    out.Transition.pResource = output_.Get();
    out.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    out.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    out.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    list_->ResourceBarrier(1, &out);

    ffxDispatchDescUpscale dispatch{};
    dispatch.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    dispatch.commandList = list_.Get();
    dispatch.color = ffxApiGetResourceDX12(input_.Get(),
                                           FFX_API_RESOURCE_STATE_COMPUTE_READ);
    dispatch.depth = ffxApiGetResourceDX12(depth_.Get(),
                                           FFX_API_RESOURCE_STATE_COMPUTE_READ);
    dispatch.motionVectors = ffxApiGetResourceDX12(
        motion_.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
    dispatch.output = ffxApiGetResourceDX12(output_.Get(),
                                            FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    dispatch.jitterOffset = {0.0f, 0.0f};
    dispatch.motionVectorScale = {1.0f, 1.0f};
    dispatch.renderSize = {static_cast<uint32_t>(in_width_),
                           static_cast<uint32_t>(in_height_)};
    dispatch.upscaleSize = {static_cast<uint32_t>(out_width_),
                            static_cast<uint32_t>(out_height_)};
    dispatch.enableSharpening = sharpness_ > 0.001f;
    dispatch.sharpness = sharpness_;
    dispatch.frameTimeDelta = frame_time_ms > 0.0f ? frame_time_ms : 16.67f;
    dispatch.preExposure = 1.0f;
    dispatch.reset = reset_each_frame_;
    dispatch.cameraNear = 0.1f;
    dispatch.cameraFar = 1000.0f;
    dispatch.cameraFovAngleVertical = 1.0f;
    dispatch.viewSpaceToMetersFactor = 1.0f;
    dispatch.flags = hdr_input_ ? 0u : FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB;

    const ffxReturnCode_t result = api_->dispatch(&context_handle_, &dispatch.header);
    if (result != FFX_API_RETURN_OK) {
        detail = "ffxDispatch failed (" + std::to_string(result) + ")";
        status_ = detail;
        return false;
    }
    out.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    out.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    list_->ResourceBarrier(1, &out);

    if (!Submit(detail) || !Wait(detail)) {
        return false;
    }
    last_ms_ = NowMs() - started;
    status_ = "upscaling";
    return true;
}

}  // namespace brp::media
