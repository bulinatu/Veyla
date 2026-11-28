#include "media/nv_dlss_fg.h"
#include "media/ngx_state.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

#include <nvsdk_ngx.h>

namespace brp::media {
namespace {

constexpr unsigned long long kAppId = 0x0876232Cull;

double NowMs() {
    using clock = std::chrono::steady_clock;
    static const clock::time_point start = clock::now();
    return std::chrono::duration<double, std::milli>(clock::now() - start).count();
}

std::wstring ExeDirectory() {
    wchar_t path[MAX_PATH] = {};
    if (::GetModuleFileNameW(nullptr, path, MAX_PATH) == 0) {
        return {};
    }
    std::wstring text = path;
    const std::size_t slash = text.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : text.substr(0, slash);
}

std::wstring NgxCoreDirectory() {
    wchar_t path[1024] = {};
    DWORD size = sizeof(path);
    if (::RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore",
                       L"FullPath", RRF_RT_REG_SZ, nullptr, path, &size) == ERROR_SUCCESS &&
        path[0]) {
        return path;
    }
    return {};
}

using InitFn = NVSDK_NGX_Result(NVSDK_CONV*)(unsigned long long, const wchar_t*,
                                             ID3D12Device*,
                                             const NVSDK_NGX_FeatureCommonInfo*,
                                             NVSDK_NGX_Version);
using InitProjectFn = NVSDK_NGX_Result(NVSDK_CONV*)(
    const char*, NVSDK_NGX_EngineType, const char*, const wchar_t*, ID3D12Device*,
    const NVSDK_NGX_FeatureCommonInfo*, NVSDK_NGX_Version);
using GetCapFn = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter**);
using CreateFn = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*,
                                                NVSDK_NGX_Feature, NVSDK_NGX_Parameter*,
                                                NVSDK_NGX_Handle**);
using EvalFn = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*,
                                              const NVSDK_NGX_Handle*,
                                              const NVSDK_NGX_Parameter*,
                                              PFN_NVSDK_NGX_ProgressCallback);
using ReleaseFn = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Handle*);

}  // namespace

struct NvDlssFg::Api {
    HMODULE module = nullptr;
    InitFn init = nullptr;
    InitProjectFn init_project = nullptr;
    GetCapFn get_cap = nullptr;
    CreateFn create = nullptr;
    EvalFn evaluate = nullptr;
    ReleaseFn release = nullptr;
};

NvDlssFg::NvDlssFg() = default;

NvDlssFg::~NvDlssFg() {
    Stop();
}

bool NvDlssFg::Start(ID3D11Device* device, const std::wstring& feature_dir, int width,
                     int height, int multiplier, std::string& detail,
                     ID3D12Device* shared_device12) {
    Stop();
    if (!device || width <= 0 || height <= 0) {
        detail = "DLSS-G needs a device and a frame size";
        return false;
    }
    device_ = device;
    shared_device12_ = shared_device12;
    width_ = width;
    height_ = height;
    multiplier_ = std::clamp(multiplier, 2, 6);
    requested_multiplier_ = multiplier_;
    const std::filesystem::path dir(feature_dir);
    const std::wstring core_dir = NgxCoreDirectory();
    std::filesystem::path feature = dir / L"nvngx_dlssg.dll";
    std::wstring feature_root = dir.wstring();
    std::error_code ec;
    if (!core_dir.empty()) {
        const std::filesystem::path driver_feature =
            std::filesystem::path(core_dir) / L"nvngx_dlssg.dll";
        if (std::filesystem::exists(driver_feature, ec)) {
            feature = driver_feature;
            feature_root = core_dir;
        }
    }
    if (!std::filesystem::exists(feature, ec)) {
        detail = "nvngx_dlssg.dll not found in " + dir.string();
        return false;
    }
    feature_dir_ = feature_root;
    if (!EnsureDevice(detail)) {
        return false;
    }
    ::SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_USER_DIRS);
    ::AddDllDirectory(dir.c_str());
    if (!core_dir.empty()) {
        ::AddDllDirectory(core_dir.c_str());
    }
    std::wstring core_path = core_dir + L"\\_nvngx.dll";
    if (core_dir.empty() ||
        ::GetFileAttributesW(core_path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        core_path = dir.wstring() + L"\\_nvngx.dll";
    }
    if (::GetFileAttributesW(core_path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        core_path = core_dir + L"\\nvngx.dll";
    }
    if (::GetFileAttributesW(core_path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        detail = "NGX core (_nvngx.dll) not found";
        return false;
    }
    api_ = std::make_unique<Api>();
    api_->module = ::LoadLibraryExW(core_path.c_str(), nullptr,
                                    LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                        LOAD_LIBRARY_SEARCH_DEFAULT_DIRS |
                                        LOAD_LIBRARY_SEARCH_USER_DIRS);
    if (!api_->module) {
        detail = "cannot load the NGX core (error " +
                 std::to_string(::GetLastError()) + ")";
        return false;
    }
    api_->init = reinterpret_cast<InitFn>(
        ::GetProcAddress(api_->module, "NVSDK_NGX_D3D12_Init"));
    api_->init_project = reinterpret_cast<InitProjectFn>(
        ::GetProcAddress(api_->module, "NVSDK_NGX_D3D12_Init_with_ProjectID"));
    api_->get_cap = reinterpret_cast<GetCapFn>(
        ::GetProcAddress(api_->module, "NVSDK_NGX_D3D12_GetCapabilityParameters"));
    api_->create = reinterpret_cast<CreateFn>(
        ::GetProcAddress(api_->module, "NVSDK_NGX_D3D12_CreateFeature"));
    api_->evaluate = reinterpret_cast<EvalFn>(
        ::GetProcAddress(api_->module, "NVSDK_NGX_D3D12_EvaluateFeature"));
    api_->release = reinterpret_cast<ReleaseFn>(
        ::GetProcAddress(api_->module, "NVSDK_NGX_D3D12_ReleaseFeature"));
    if (!api_->init || !api_->get_cap || !api_->create || !api_->evaluate) {
        detail = "nvngx_dlssg.dll is missing NGX exports";
        return false;
    }
    if (!EnsureCore(detail)) {
        // The NGX core can report OutOfDate for a few hundred milliseconds
        // after a driver/session transition. Retry before giving up.
        bool ok = false;
        for (int attempt = 0; attempt < 3 && !ok; ++attempt) {
            ::Sleep(200);
            ok = EnsureCore(detail);
        }
        if (!ok) {
            return false;
        }
    }
    if (!CreateResources(detail)) {
        return false;
    }
    if (!CreateFeature(detail)) {
        // Same transient window as Init; one retry is enough.
        ::Sleep(150);
        if (!CreateFeature(detail)) {
            return false;
        }
    }
    of_ = std::make_unique<NvOf>();
    std::string of_detail;
    if (!of_->Start(device12_.Get(), queue_.Get(), width_, height_, of_detail)) {
        std::fprintf(stderr, "[dlssg] NVOFA unavailable (%s); zero motion\n",
                     of_detail.c_str());
        of_.reset();
        of_failed_ = true;
    }
    if (std::getenv("VEYLA_NVOF") == nullptr) {
        // The driver's D3D12 optical-flow command list still fails to Close on
        // this runtime. Keep it opt-in for development; the global estimate is
        // the production fallback.
        of_.reset();
        of_failed_ = true;
    }
    motion_source_ = motion_.Get();
    feature_event_ = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    ready_ = true;
    frames_.clear();
    status_ = "DLSS-G ready";
    std::fprintf(stderr, "[dlssg] native %dx%d %dx, generated %d, max %d\n", width_, height_,
                 multiplier_, generated_, max_generated_);
    return true;
}

bool NvDlssFg::EnsureDevice(std::string& detail) {
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi;
    if (FAILED(device_.As(&dxgi))) {
        detail = "cannot resolve the D3D11 adapter for DLSS-G";
        return false;
    }
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    dxgi->GetAdapter(&adapter);
    if (adapter) {
        DXGI_ADAPTER_DESC desc{};
        adapter->GetDesc(&desc);
        std::fprintf(stderr, "[dlssg] adapter: %ls (vendor 0x%04X)\n", desc.Description,
                     desc.VendorId);
    }
    HRESULT created = E_FAIL;
    // NGX is already initialized by DLSS SR on one D3D12 device. Reuse that
    // exact device: NGX rejects a second Init with a different device.
    ID3D12Device* existing =
        shared_device12_ ? shared_device12_ : NgxInitializedDevice();
    if (existing) {
        device12_ = existing;
        created = S_OK;
    } else if (adapter) {
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_12_1,
                                            D3D_FEATURE_LEVEL_12_0,
                                            D3D_FEATURE_LEVEL_11_0};
        for (const auto level : levels) {
            created = ::D3D12CreateDevice(adapter.Get(), level, IID_PPV_ARGS(&device12_));
            if (SUCCEEDED(created)) {
                break;
            }
        }
    }
    if (FAILED(created) || !device12_) {
        detail = "D3D12CreateDevice failed for DLSS-G";
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
        detail = "DLSS-G command objects failed";
        return false;
    }
    list_->Close();
    fence_event_ = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!fence_event_) {
        detail = "DLSS-G fence event failed";
        return false;
    }
    return true;
}

bool NvDlssFg::EnsureCore(std::string& detail) {
    NVSDK_NGX_FeatureCommonInfo common{};
    const wchar_t* paths[1] = {feature_dir_.c_str()};
    common.PathListInfo.Path = paths;
    common.PathListInfo.Length = 1;
    const std::wstring app_dir = ExeDirectory();
    NVSDK_NGX_Result result = NVSDK_NGX_Result_Fail;
    const bool already_initialized = NgxInitialized();
    if (!already_initialized) {
        if (api_->init_project) {
            result = api_->init_project(
                "veyla", NVSDK_NGX_ENGINE_TYPE_CUSTOM, "3.0", app_dir.c_str(),
                device12_.Get(), &common, NVSDK_NGX_Version_API);
            if (NVSDK_NGX_FAILED(result)) {
                std::fprintf(stderr, "[dlssg] Init_with_ProjectID 0x%X, trying Init\n",
                             static_cast<unsigned>(result));
            }
        }
        if (NVSDK_NGX_FAILED(result)) {
            result = api_->init(kAppId, app_dir.c_str(), device12_.Get(), &common,
                                NVSDK_NGX_Version_API);
        }
        if (NVSDK_NGX_FAILED(result)) {
            detail = "NGX Init for DLSS-G failed (0x" + std::to_string(result) + ")";
            return false;
        }
        MarkNgxInitialized(device12_.Get());
    }
    NVSDK_NGX_Parameter* params = nullptr;
    if (NVSDK_NGX_FAILED(api_->get_cap(&params)) || !params) {
        detail = "NGX capability parameters failed";
        return false;
    }
    parameters_ = params;
    unsigned available = 0;
    params->Get("FrameGeneration.Available", &available);
    unsigned max_generated = 1;
    params->Get("DLSSG.MultiFrameCountMax", &max_generated);
    if (!available) {
        detail = "NVIDIA DLSS-G is not available on this GPU/driver";
        return false;
    }
    max_generated_ = std::max(1u, max_generated);
    generated_ = std::clamp(multiplier_ - 1, 1, max_generated_);
    if (generated_ != multiplier_ - 1) {
        std::fprintf(stderr, "[dlssg] multiplier %d clamped to %dx by the driver\n",
                     multiplier_, generated_ + 1);
        multiplier_ = generated_ + 1;
    }
    return true;
}

bool NvDlssFg::CreateResources(std::string& detail) {
    auto texture = [&](DXGI_FORMAT format, int width, int height,
                       D3D12_RESOURCE_STATES state, D3D12_RESOURCE_FLAGS flags,
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
        return SUCCEEDED(device12_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
                                                            &desc, state, nullptr,
                                                            IID_PPV_ARGS(&out)));
    };
    if (!texture(DXGI_FORMAT_R8G8B8A8_UNORM, width_, height_,
                 D3D12_RESOURCE_STATE_COMMON,
                 D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, input_) ||
        !texture(DXGI_FORMAT_R8G8B8A8_UNORM, width_, height_,
                 D3D12_RESOURCE_STATE_COMMON,
                 D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, previous_input_)) {
        detail = "DLSS-G input texture failed";
        return false;
    }
    const UINT row = (static_cast<UINT>(width_) * 4u + 255u) & ~255u;
    D3D12_HEAP_PROPERTIES up{};
    up.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC ub{};
    ub.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    ub.Width = static_cast<UINT64>(row) * static_cast<UINT>(height_);
    ub.Height = 1;
    ub.DepthOrArraySize = 1;
    ub.MipLevels = 1;
    ub.Format = DXGI_FORMAT_UNKNOWN;
    ub.SampleDesc.Count = 1;
    ub.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(device12_->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &ub,
                                                  D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                  IID_PPV_ARGS(&upload_)))) {
        detail = "DLSS-G upload buffer failed";
        return false;
    }
    upload_->Map(0, nullptr, reinterpret_cast<void**>(&upload_ptr_));

    if (!texture(DXGI_FORMAT_R32_FLOAT, width_, height_,
                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                 D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, depth_) ||
        !texture(DXGI_FORMAT_R16G16_FLOAT, width_, height_,
                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                 D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, motion_) ||
        !texture(DXGI_FORMAT_R16G16_FLOAT, width_, height_,
                 D3D12_RESOURCE_STATE_COMMON,
                 D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, motion_of_)) {
        detail = "DLSS-G guide textures failed";
        return false;
    }
    // Persistent UAV for the per-frame global motion fill.
    D3D12_DESCRIPTOR_HEAP_DESC motion_heap{};
    motion_heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    motion_heap.NumDescriptors = 1;
    motion_heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device12_->CreateDescriptorHeap(&motion_heap,
                                               IID_PPV_ARGS(&motion_heap_)))) {
        detail = "DLSS-G motion descriptor heap failed";
        return false;
    }
    motion_cpu_ = motion_heap_->GetCPUDescriptorHandleForHeapStart();
    motion_gpu_ = motion_heap_->GetGPUDescriptorHandleForHeapStart();
    D3D12_UNORDERED_ACCESS_VIEW_DESC motion_uav{};
    motion_uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    motion_uav.Format = DXGI_FORMAT_R16G16_FLOAT;
    device12_->CreateUnorderedAccessView(motion_.Get(), nullptr, &motion_uav, motion_cpu_);

    auto output = [&](Microsoft::WRL::ComPtr<ID3D12Resource>& tex, HANDLE& handle) -> bool {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = static_cast<UINT64>(width_);
        desc.Height = static_cast<UINT>(height_);
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (FAILED(device12_->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_SHARED, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                IID_PPV_ARGS(&tex)))) {
            return false;
        }
        return SUCCEEDED(device12_->CreateSharedHandle(tex.Get(), nullptr, GENERIC_ALL,
                                                       nullptr, &handle));
    };
    if (!output(real_, real_handle_)) {
        detail = "DLSS-G real output failed";
        return false;
    }
    generated_tex_.resize(max_generated_);
    generated_handles_.resize(max_generated_, nullptr);
    for (int i = 0; i < max_generated_; ++i) {
        if (!output(generated_tex_[i], generated_handles_[i])) {
            detail = "DLSS-G generated output failed";
            return false;
        }
    }
    return ClearGuides(detail);
}

bool NvDlssFg::ClearGuides(std::string& detail) {
    if (FAILED(allocator_->Reset()) || FAILED(list_->Reset(allocator_.Get(), nullptr))) {
        detail = "DLSS-G guide reset failed";
        return false;
    }
    D3D12_DESCRIPTOR_HEAP_DESC heap{};
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap.NumDescriptors = 2;
    heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> dh;
    if (FAILED(device12_->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&dh)))) {
        detail = "DLSS-G guide descriptor heap failed";
        return false;
    }
    const UINT inc = device12_->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = dh->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = dh->GetGPUDescriptorHandleForHeapStart();
    D3D12_CPU_DESCRIPTOR_HANDLE cpu2 = cpu;
    cpu2.ptr += inc;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu2 = gpu;
    gpu2.ptr += inc;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uav.Format = DXGI_FORMAT_R32_FLOAT;
    device12_->CreateUnorderedAccessView(depth_.Get(), nullptr, &uav, cpu);
    uav.Format = DXGI_FORMAT_R16G16_FLOAT;
    device12_->CreateUnorderedAccessView(motion_.Get(), nullptr, &uav, cpu2);
    ID3D12DescriptorHeap* heaps[] = {dh.Get()};
    list_->SetDescriptorHeaps(1, heaps);
    const float zero[4] = {0, 0, 0, 0};
    const float far_depth[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    list_->ClearUnorderedAccessViewFloat(gpu, cpu, depth_.Get(), far_depth, 0, nullptr);
    list_->ClearUnorderedAccessViewFloat(gpu2, cpu2, motion_.Get(), zero, 0, nullptr);
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

bool NvDlssFg::CreateFeature(std::string& detail) {
    if (FAILED(allocator_->Reset()) || FAILED(list_->Reset(allocator_.Get(), nullptr))) {
        detail = "DLSS-G feature list reset failed";
        return false;
    }
    auto* params = static_cast<NVSDK_NGX_Parameter*>(parameters_);
    params->Set(NVSDK_NGX_Parameter_CreationNodeMask, 1u);
    params->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
    params->Set(NVSDK_NGX_Parameter_Width, static_cast<unsigned>(width_));
    params->Set(NVSDK_NGX_Parameter_Height, static_cast<unsigned>(height_));
    params->Set("DLSSG.BackbufferFormat",
                static_cast<unsigned>(DXGI_FORMAT_R8G8B8A8_UNORM));
    params->Set("DLSSG.InternalWidth", 0u);
    params->Set("DLSSG.InternalHeight", 0u);
    params->Set("DLSSG.DynamicResolution", 0u);
    params->Set("DLSSG.MustCallEval", 1u);
    params->Set("DLSSG.NumFrames", 1);
    params->Set("DLSSG.MultiFrameCount", static_cast<unsigned>(generated_));
    // Undocumented but required for the offscreen (recording) path: without
    // these two the feature is created and evaluated but writes no pixels.
    params->Set("Enable.OFA", 1u);
    params->Set("DLSSG.EnableInterp", 1u);
    params->Set("DLSSG.OrthoProjection", 0u);
    params->Set("DLSSG.CameraPinholeOffsetX", 0.0f);
    params->Set("DLSSG.CameraPinholeOffsetY", 0.0f);
    NVSDK_NGX_Handle* feature = nullptr;
    const NVSDK_NGX_Result result =
        api_->create(list_.Get(), NVSDK_NGX_Feature_FrameGeneration, params, &feature);
    const HRESULT close = list_->Close();
    (void)close;
    if (NVSDK_NGX_FAILED(result) || !feature) {
        detail = "DLSS-G CreateFeature failed (0x" + std::to_string(result) + ")";
        return false;
    }
    feature_ = feature;
    return true;
}

bool NvDlssFg::Process(const std::uint8_t* rgba, int width, int height, bool reset,
                       std::string& detail) {
    if (!ready_ || !rgba || width != width_ || height != height_) {
        detail = "DLSS-G is not initialized for this frame size";
        return false;
    }
    if (!Wait(detail)) {
        return false;
    }
    // The two completed inputs are in the slots that the next upload is not
    // about to touch. One frame of motion age is a better guide than zero.
    if (frames_fed_ >= 2 && of_) {
        ID3D12Resource* const completed =
            input_slot_ == 0 ? previous_input_.Get() : input_.Get();
        ID3D12Resource* const older =
            input_slot_ == 0 ? input_.Get() : previous_input_.Get();
        if (of_->Execute(completed, older, motion_of_.Get(), detail)) {
            motion_source_ = motion_of_.Get();
        } else {
            static bool logged = false;
            if (!logged) {
                logged = true;
                std::fprintf(stderr, "[dlssg] NVOFA execute failed: %s\n",
                             detail.c_str());
            }
            motion_source_ = motion_.Get();
        }
    } else {
        motion_source_ = motion_.Get();
    }
    if (FAILED(allocator_->Reset()) || FAILED(list_->Reset(allocator_.Get(), nullptr))) {
        detail = "DLSS-G command reset failed";
        return false;
    }
    const double started = NowMs();
    ID3D12Resource* const current =
        input_slot_ == 0 ? input_.Get() : previous_input_.Get();
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = current;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    list_->ResourceBarrier(1, &barrier);
    const UINT row = static_cast<UINT>(width) * 4u;
    const UINT pitch = (row + 255u) & ~255u;
    for (int y = 0; y < height; ++y) {
        std::memcpy(upload_ptr_ + static_cast<std::size_t>(pitch) * y,
                    rgba + static_cast<std::size_t>(row) * y, row);
    }
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = current;
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
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    list_->ResourceBarrier(1, &barrier);
    eval_input_ = current;
    if (!Evaluate(reset, detail)) {
        return false;
    }
    input_slot_ ^= 1;
    have_previous_ = true;
    ++frames_fed_;
    last_ms_ = NowMs() - started;
    return true;
}

bool NvDlssFg::ProcessShared(HANDLE shared, int width, int height, bool reset,
                             std::string& detail) {
    if (!ready_ || !shared || width != width_ || height != height_) {
        detail = "DLSS-G shared input is not initialized for this frame size";
        return false;
    }
    if (!Wait(detail)) {
        return false;
    }
    ID3D12Resource* input = nullptr;
    for (auto& entry : shared_inputs_) {
        if (entry.handle == shared) {
            input = entry.resource.Get();
            break;
        }
    }
    if (!input) {
        // Keep the last few SR slots; the producer rotates two textures.
        if (shared_inputs_.size() >= 4) {
            shared_inputs_.erase(shared_inputs_.begin());
        }
        SharedInput entry;
        entry.handle = shared;
        if (FAILED(device12_->OpenSharedHandle(shared, IID_PPV_ARGS(&entry.resource)))) {
            detail = "DLSS-G cannot open the shared SR output";
            return false;
        }
        input = entry.resource.Get();
        shared_inputs_.push_back(std::move(entry));
    }
    if (FAILED(allocator_->Reset()) || FAILED(list_->Reset(allocator_.Get(), nullptr))) {
        detail = "DLSS-G shared command reset failed";
        return false;
    }
    const double started = NowMs();
    eval_input_ = input;
    motion_source_ = motion_.Get();
    if (!Evaluate(reset, detail)) {
        return false;
    }
    ++frames_fed_;
    last_ms_ = NowMs() - started;
    return true;
}

bool NvDlssFg::ProcessTexture(ID3D12Resource* color, int width, int height, bool reset,
                              std::string& detail) {
    (void)color;
    (void)width;
    (void)height;
    (void)reset;
    detail = "DLSS-G texture input is not implemented yet";
    return false;
}

bool NvDlssFg::Evaluate(bool reset, std::string& detail) {
    D3D12_RESOURCE_BARRIER barriers[8]{};
    UINT count = 0;
    auto add = [&](ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                   D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER& b = barriers[count++];
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = resource;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = before;
        b.Transition.StateAfter = after;
    };
    add(real_.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    for (int i = 0; i < generated_; ++i) {
        add(generated_tex_[i].Get(), D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    add(eval_input_, D3D12_RESOURCE_STATE_COMMON,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (motion_source_ && motion_source_ != motion_.Get()) {
        add(motion_source_, D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    list_->ResourceBarrier(count, barriers);

    // Fill the whole motion field with the estimated global shift. ClearUAV
    // writes every texel, which is exactly a constant screen-space motion.
    const bool global_motion =
        motion_source_ == nullptr || motion_source_ == motion_.Get();
    if (global_motion && motion_heap_) {
        D3D12_RESOURCE_BARRIER motion_barrier{};
        motion_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        motion_barrier.Transition.pResource = motion_.Get();
        motion_barrier.Transition.Subresource =
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        motion_barrier.Transition.StateBefore =
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        motion_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        list_->ResourceBarrier(1, &motion_barrier);
        const float shift[4] = {motion_x_, motion_y_, 0.0f, 0.0f};
        list_->ClearUnorderedAccessViewFloat(motion_gpu_, motion_cpu_, motion_.Get(),
                                              shift, 0, nullptr);
        std::swap(motion_barrier.Transition.StateBefore,
                  motion_barrier.Transition.StateAfter);
        list_->ResourceBarrier(1, &motion_barrier);
    }

    auto* params = static_cast<NVSDK_NGX_Parameter*>(parameters_);
    params->Set("DLSSG.CmdQueue", static_cast<void*>(queue_.Get()));
    params->Set("DLSSG.CmdAlloc", static_cast<void*>(allocator_.Get()));
    params->Set("DLSSG.FenceEvent", static_cast<void*>(feature_event_));
    // Temporal history key: without a changing frame id the runtime treats
    // every call as the first frame and cannot interpolate.
    params->Set("DLSSG.BackbufferFrameID",
                reinterpret_cast<void*>(static_cast<uintptr_t>(++frame_id_)));
    params->Set("DLSSG.HUDLess", eval_input_);
    params->Set("DLSSG.Backbuffer", eval_input_);
    params->Set("DLSSG.MVecs", motion_source_ ? motion_source_ : motion_.Get());
    params->Set("DLSSG.Depth", depth_.Get());
    params->Set("DLSSG.OutputReal", real_.Get());
    params->Set("DLSSG.MultiFrameCount", static_cast<unsigned>(generated_));
    params->Set("DLSSG.Reset", reset ? 1u : 0u);
    params->Set("DLSSG.IsRecording", 1u);
    params->Set("DLSSG.MustCallEval", 1u);
    params->Set("DLSSG.NumFrames", 1);
    params->Set("DLSSG.CameraMotionIncluded", 0u);
    params->Set("DLSSG.ColorBuffersHDR", 0u);
    params->Set("DLSSG.DepthInverted", 0u);
    params->Set("DLSSG.OrthoProjection", 0u);
    params->Set("DLSSG.CameraPinholeOffsetX", 0.0f);
    params->Set("DLSSG.CameraPinholeOffsetY", 0.0f);
    params->Set("DLSSG.UI", static_cast<ID3D12Resource*>(nullptr));
    params->Set("DLSSG.MvecDilated", 0u);
    // The shell repeats the real frame when the estimate is unreliable; the
    // flag also asks the runtime not to warp a generated frame if one is used.
    params->Set("DLSSG.OutputDisableInterpolation", interpolation_ok_ ? 0u : 1u);
    params->Set("DLSSG.NotRenderingGameFrames", 0u);
    params->Set("DLSSG.DynamicResolution", 0u);
    // Global motion is expressed in pixels; tell the runtime the scale.
    params->Set("DLSSG.MvecScaleX", static_cast<float>(width_));
    params->Set("DLSSG.MvecScaleY", static_cast<float>(height_));
    params->Set("DLSSG.MVecsSubrectBaseX", 0u);
    params->Set("DLSSG.MVecsSubrectBaseY", 0u);
    params->Set("DLSSG.MVecsSubrectWidth", static_cast<unsigned>(width_));
    params->Set("DLSSG.MVecsSubrectHeight", static_cast<unsigned>(height_));
    params->Set("DLSSG.DepthSubrectBaseX", 0u);
    params->Set("DLSSG.DepthSubrectBaseY", 0u);
    params->Set("DLSSG.DepthSubrectWidth", static_cast<unsigned>(width_));
    params->Set("DLSSG.DepthSubrectHeight", static_cast<unsigned>(height_));
    params->Set("DLSSG.HUDLessSubrectBaseX", 0u);
    params->Set("DLSSG.HUDLessSubrectBaseY", 0u);
    params->Set("DLSSG.HUDLessSubrectWidth", static_cast<unsigned>(width_));
    params->Set("DLSSG.HUDLessSubrectHeight", static_cast<unsigned>(height_));
    params->Set("DLSSG.CameraNear", 0.1f);
    params->Set("DLSSG.CameraFar", 1000.0f);
    params->Set("DLSSG.CameraFOV", 1.0f);
    params->Set("DLSSG.CameraAspectRatio",
                static_cast<float>(width_) / static_cast<float>(height_));
    params->Set("DLSSG.CameraPosX", 0.0f);
    params->Set("DLSSG.CameraPosY", 0.0f);
    params->Set("DLSSG.CameraPosZ", 0.0f);
    params->Set("DLSSG.CameraRightX", 1.0f);
    params->Set("DLSSG.CameraRightY", 0.0f);
    params->Set("DLSSG.CameraRightZ", 0.0f);
    params->Set("DLSSG.CameraUpX", 0.0f);
    params->Set("DLSSG.CameraUpY", 1.0f);
    params->Set("DLSSG.CameraUpZ", 0.0f);
    // The 310.x runtime exposes CameraFwd*, not CameraForward*.
    params->Set("DLSSG.CameraFwdX", 0.0f);
    params->Set("DLSSG.CameraFwdY", 0.0f);
    params->Set("DLSSG.CameraFwdZ", 1.0f);
    params->Set("DLSSG.JitterOffsetX", 0.0f);
    params->Set("DLSSG.JitterOffsetY", 0.0f);
    params->Set(NVSDK_NGX_Parameter_Width, static_cast<unsigned>(width_));
    params->Set(NVSDK_NGX_Parameter_Height, static_cast<unsigned>(height_));

    for (int i = 0; i < generated_; ++i) {
        params->Set("DLSSG.OutputInterpolated", generated_tex_[i].Get());
        params->Set("DLSSG.MultiFrameIndex", static_cast<unsigned>(i + 1));
        const NVSDK_NGX_Result result =
            api_->evaluate(list_.Get(), static_cast<NVSDK_NGX_Handle*>(feature_), params,
                           nullptr);
        if (NVSDK_NGX_FAILED(result)) {
            detail = "DLSS-G EvaluateFeature failed (0x" + std::to_string(result) + ")";
            return false;
        }
    }
    // The offscreen mode can leave OutputReal empty; the real frame is the
    // current color frame, so copy it explicitly. This is also what makes the
    // first frame visible before any interpolation exists.
    //
    // feature_event_ is signalled by the app's own fence after the submit, not
    // by the runtime: waiting on it here blocked every frame for 50 ms.

    for (UINT i = 0; i < count; ++i) {
        std::swap(barriers[i].Transition.StateBefore,
                  barriers[i].Transition.StateAfter);
    }
    list_->ResourceBarrier(count, barriers);
    D3D12_RESOURCE_BARRIER copy_barriers[4]{};
    copy_barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    copy_barriers[0].Transition.pResource = real_.Get();
    copy_barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    copy_barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    copy_barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    copy_barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    copy_barriers[1].Transition.pResource = eval_input_;
    copy_barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    copy_barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    copy_barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    list_->ResourceBarrier(2, copy_barriers);
    list_->CopyResource(real_.Get(), eval_input_);
    copy_barriers[2] = copy_barriers[0];
    copy_barriers[2].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    copy_barriers[2].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    copy_barriers[3] = copy_barriers[1];
    copy_barriers[3].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    copy_barriers[3].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    list_->ResourceBarrier(2, copy_barriers + 2);
    if (!Submit(detail) || !Wait(detail)) {
        return false;
    }
    frames_.clear();
    frames_.push_back({real_handle_, false});
    for (int i = 0; i < generated_; ++i) {
        frames_.push_back({generated_handles_[i], true});
    }
    status_ = "generating";
    return true;
}

bool NvDlssFg::Submit(std::string& detail) {
    const HRESULT close = list_->Close();
    if (FAILED(close)) {
        char text[96];
        std::snprintf(text, sizeof(text), "DLSS-G command close failed 0x%08lX",
                      static_cast<unsigned long>(close));
        detail = text;
        return false;
    }
    ID3D12CommandList* lists[] = {list_.Get()};
    queue_->ExecuteCommandLists(1, lists);
    submitted_ = ++fence_value_;
    if (FAILED(queue_->Signal(fence_.Get(), submitted_))) {
        detail = "DLSS-G queue signal failed";
        return false;
    }
    return true;
}

bool NvDlssFg::Wait(std::string& detail) {
    if (!submitted_ || !fence_event_) {
        return true;
    }
    if (fence_->GetCompletedValue() < submitted_) {
        if (FAILED(fence_->SetEventOnCompletion(submitted_, fence_event_)) ||
            ::WaitForSingleObject(fence_event_, 5000) != WAIT_OBJECT_0) {
            detail = "DLSS-G fence timed out";
            return false;
        }
    }
    return true;
}

void NvDlssFg::Stop() {
    ready_ = false;
    if (feature_ && api_ && api_->release) {
        api_->release(static_cast<NVSDK_NGX_Handle*>(feature_));
        feature_ = nullptr;
    }
    if (fence_event_) {
        ::CloseHandle(fence_event_);
        fence_event_ = nullptr;
    }
    if (feature_event_) {
        ::CloseHandle(feature_event_);
        feature_event_ = nullptr;
    }
    for (HANDLE& handle : generated_handles_) {
        if (handle) {
            ::CloseHandle(handle);
            handle = nullptr;
        }
    }
    if (real_handle_) {
        ::CloseHandle(real_handle_);
        real_handle_ = nullptr;
    }
    generated_handles_.clear();
    generated_tex_.clear();
    real_.Reset();
    motion_of_.Reset();
    motion_source_ = nullptr;
    shared_inputs_.clear();
    of_.reset();
    previous_input_.Reset();
    motion_.Reset();
    depth_.Reset();
    upload_.Reset();
    input_.Reset();
    upload_ptr_ = nullptr;
    frames_.clear();
    input_ready_ = false;
    submitted_ = 0;
    parameters_ = nullptr;
}

}  // namespace brp::media
