#include "media/nv_dlss_sr.h"

#include <dxgi.h>

#include <nvsdk_ngx.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>

namespace brp::media {
namespace {

constexpr unsigned long long kAppId = 0x0876232Cull;

using InitFn = NVSDK_NGX_Result(NVSDK_CONV*)(unsigned long long, const wchar_t*, ID3D12Device*,
                                             NVSDK_NGX_Version);
using GetCapFn = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter**);
using CreateFn = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature,
                                                NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
using EvalFn = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*,
                                              const NVSDK_NGX_Parameter*,
                                              PFN_NVSDK_NGX_ProgressCallback);
using ReleaseFn = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Handle*);
using DestroyParamsFn = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter*);

std::wstring ExeDirectory() {
    wchar_t path[MAX_PATH] = {};
    ::GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring text = path;
    const size_t slash = text.find_last_of(L"\\/");
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

double NowMs() {
    using clock = std::chrono::steady_clock;
    static const clock::time_point start = clock::now();
    return std::chrono::duration<double, std::milli>(clock::now() - start).count();
}

}  // namespace

struct NvDlssSr::Api {
    HMODULE core = nullptr;
    HMODULE feature_dll = nullptr;
    InitFn init = nullptr;
    GetCapFn get_cap = nullptr;
    CreateFn create = nullptr;
    EvalFn evaluate = nullptr;
    ReleaseFn release = nullptr;
    DestroyParamsFn destroy_params = nullptr;
};

NvDlssSr::NvDlssSr() = default;

NvDlssSr::~NvDlssSr() {
    Shutdown();
}

std::string NvDlssSr::ErrorText(int result) const {
    char text[64];
    std::snprintf(text, sizeof(text), "0x%08X", static_cast<unsigned>(result));
    return text;
}

bool NvDlssSr::LoadRuntime(const std::wstring& directory, std::string& detail) {
    if (runtime_loaded_) {
        return true;
    }
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dir = fs::absolute(directory, ec);
    if (ec || !fs::exists(dir / L"nvngx_dlss.dll")) {
        detail = "nvngx_dlss.dll not found in " + dir.string();
        status_ = detail;
        return false;
    }

    ::SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_USER_DIRS);
    ::AddDllDirectory(dir.c_str());

    api_ = std::make_unique<Api>();
    // Preload the feature DLL so the core finds this exact copy even when the
    // application directory is different.
    const std::wstring feature_path = dir.wstring() + L"\\nvngx_dlss.dll";
    api_->feature_dll =
        ::LoadLibraryExW(feature_path.c_str(), nullptr,
                         LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS |
                             LOAD_LIBRARY_SEARCH_USER_DIRS);
    if (!api_->feature_dll) {
        detail = "cannot load nvngx_dlss.dll (error " + std::to_string(::GetLastError()) + ")";
        status_ = detail;
        return false;
    }

    const std::wstring core_dir = NgxCoreDirectory();
    std::wstring core_path = core_dir + L"\\_nvngx.dll";
    if (core_dir.empty() || ::GetFileAttributesW(core_path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        core_path = dir.wstring() + L"\\_nvngx.dll";
    }
    if (::GetFileAttributesW(core_path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        core_path = core_dir + L"\\nvngx.dll";
    }
    if (!core_dir.empty()) {
        ::AddDllDirectory(core_dir.c_str());
    }
    api_->core = ::LoadLibraryExW(core_path.c_str(), nullptr, 0);
    if (!api_->core) {
        detail = "cannot load NGX core (error " + std::to_string(::GetLastError()) + ")";
        status_ = detail;
        return false;
    }

    api_->init =
        reinterpret_cast<InitFn>(::GetProcAddress(api_->core, "NVSDK_NGX_D3D12_Init"));
    api_->get_cap = reinterpret_cast<GetCapFn>(
        ::GetProcAddress(api_->core, "NVSDK_NGX_D3D12_GetCapabilityParameters"));
    api_->create = reinterpret_cast<CreateFn>(
        ::GetProcAddress(api_->core, "NVSDK_NGX_D3D12_CreateFeature"));
    api_->evaluate = reinterpret_cast<EvalFn>(
        ::GetProcAddress(api_->core, "NVSDK_NGX_D3D12_EvaluateFeature"));
    api_->release = reinterpret_cast<ReleaseFn>(
        ::GetProcAddress(api_->core, "NVSDK_NGX_D3D12_ReleaseFeature"));
    api_->destroy_params = reinterpret_cast<DestroyParamsFn>(
        ::GetProcAddress(api_->core, "NVSDK_NGX_D3D12_DestroyParameters"));
    if (!api_->init || !api_->get_cap || !api_->create || !api_->evaluate) {
        detail = "NGX core is missing DLSS exports";
        status_ = detail;
        return false;
    }
    runtime_loaded_ = true;
    status_ = "runtime loaded";
    return true;
}

bool NvDlssSr::EnsureDevice(std::string& detail) {
    if (device_ready_) {
        return true;
    }
    if (!device_ || !context_) {
        detail = "D3D11 device is not set";
        return false;
    }
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    device_->QueryInterface(IID_PPV_ARGS(&dxgi_device));
    if (dxgi_device) {
        dxgi_device->GetAdapter(&adapter);
    }
    if (!adapter) {
        detail = "cannot resolve the D3D11 adapter";
        return false;
    }
    const HRESULT hr =
        D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device12_));
    if (FAILED(hr)) {
        detail = "D3D12CreateDevice failed";
        return false;
    }
    device_.As(&device5_);
    context_.As(&context4_);

    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    device12_->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue_));
    device12_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                      IID_PPV_ARGS(&allocator_));
    device12_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator_.Get(), nullptr,
                                 IID_PPV_ARGS(&list_));

    HANDLE handle = nullptr;
    if (!device5_ || !context4_ ||
        FAILED(device5_->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence11_))) ||
        FAILED(fence11_->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &handle)) ||
        FAILED(device12_->OpenSharedHandle(handle, IID_PPV_ARGS(&fence12_)))) {
        detail = "shared D3D11/D3D12 fence creation failed";
        return false;
    }
    ::CloseHandle(handle);
    device_ready_ = true;
    return true;
}

bool NvDlssSr::EnsureCore(std::string& detail) {
    if (core_ready_) {
        return true;
    }
    if (!device12_ || !api_) {
        detail = "DLSS device is not ready";
        return false;
    }
    const std::wstring app_dir = ExeDirectory();
    const int result = api_->init(kAppId, app_dir.c_str(), device12_.Get(),
                                  NVSDK_NGX_Version_API);
    if (!NVSDK_NGX_SUCCEED(static_cast<NVSDK_NGX_Result>(result))) {
        detail = "NGX D3D12 Init failed " + ErrorText(result);
        return false;
    }
    NVSDK_NGX_Parameter* parameters = nullptr;
    const int cap = api_->get_cap(&parameters);
    if (!NVSDK_NGX_SUCCEED(static_cast<NVSDK_NGX_Result>(cap)) || !parameters) {
        detail = "GetCapabilityParameters failed " + ErrorText(cap);
        return false;
    }
    parameters_ = parameters;
    core_ready_ = true;
    return true;
}

bool NvDlssSr::Initialize(ID3D11Device* device, ID3D11DeviceContext* context, int in_width,
                          int in_height, int out_width, int out_height, int preset,
                          int quality, std::string& detail) {
    if (!runtime_loaded_ || !device || !context || in_width <= 0 || in_height <= 0 ||
        out_width <= 0 || out_height <= 0) {
        detail = "DLSS SR is not initialized";
        return false;
    }
    if (feature_ && in_width == in_width_ && in_height == in_height_ &&
        out_width == out_width_ && out_height == out_height_ && preset == preset_ &&
        quality == quality_) {
        return true;
    }
    device_ = device;
    context_ = context;
    Shutdown();
    if (!EnsureDevice(detail) || !EnsureCore(detail)) {
        return false;
    }
    in_width_ = in_width;
    in_height_ = in_height;
    out_width_ = out_width;
    out_height_ = out_height;
    if (!CreateTextures(in_width, in_height, out_width, out_height, detail)) {
        return false;
    }
    if (!ClearGuides(detail)) {
        return false;
    }
    if (!CreateFeature(preset, quality, detail)) {
        return false;
    }
    preset_ = preset;
    quality_ = quality;
    initialized_ = true;
    std::fprintf(stderr, "[dlss] SR initialized %dx%d -> %dx%d, preset %d, quality %d\n",
                 in_width, in_height, out_width, out_height, preset, quality);
    return true;
}

bool NvDlssSr::CreateTextures(int in_width, int in_height, int out_width, int out_height,
                              std::string& detail) {
    auto create_shared = [&](int w, int h, Microsoft::WRL::ComPtr<ID3D11Texture2D>& texture,
                             Microsoft::WRL::ComPtr<ID3D12Resource>& resource) -> bool {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = static_cast<UINT>(w);
        desc.Height = static_cast<UINT>(h);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
        if (FAILED(device_->CreateTexture2D(&desc, nullptr, &texture))) {
            return false;
        }
        Microsoft::WRL::ComPtr<IDXGIResource1> dxgi;
        texture.As(&dxgi);
        HANDLE handle = nullptr;
        if (!dxgi || FAILED(dxgi->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &handle))) {
            return false;
        }
        const HRESULT hr = device12_->OpenSharedHandle(handle, IID_PPV_ARGS(&resource));
        ::CloseHandle(handle);
        return SUCCEEDED(hr);
    };

    if (!create_shared(in_width, in_height, input11_, input12_) ||
        !create_shared(out_width, out_height, output11_, output12_)) {
        detail = "shared DLSS textures failed";
        return false;
    }
    if (FAILED(device_->CreateShaderResourceView(output11_.Get(), nullptr, &output_srv_))) {
        detail = "DLSS output SRV failed";
        return false;
    }

    auto create_guide = [&](int w, int h, DXGI_FORMAT format,
                            Microsoft::WRL::ComPtr<ID3D12Resource>& resource) -> bool {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = static_cast<UINT64>(w);
        desc.Height = static_cast<UINT>(h);
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        return SUCCEEDED(device12_->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
            IID_PPV_ARGS(&resource)));
    };
    if (!create_guide(in_width, in_height, DXGI_FORMAT_R16G16_FLOAT, motion12_) ||
        !create_guide(in_width, in_height, DXGI_FORMAT_R32_FLOAT, depth12_)) {
        detail = "DLSS zero-guide resources failed";
        return false;
    }
    return true;
}

bool NvDlssSr::ClearGuides(std::string& detail) {
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.NumDescriptors = 2;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device12_->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&guide_heap_)))) {
        detail = "guide descriptor heap failed";
        return false;
    }
    const UINT increment = device12_->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu0 = guide_heap_->GetCPUDescriptorHandleForHeapStart();
    D3D12_CPU_DESCRIPTOR_HANDLE cpu1 = cpu0;
    cpu1.ptr += increment;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu0 = guide_heap_->GetGPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gpu1 = gpu0;
    gpu1.ptr += increment;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc{};
    uav_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uav_desc.Format = DXGI_FORMAT_R16G16_FLOAT;
    device12_->CreateUnorderedAccessView(motion12_.Get(), nullptr, &uav_desc, cpu0);
    uav_desc.Format = DXGI_FORMAT_R32_FLOAT;
    device12_->CreateUnorderedAccessView(depth12_.Get(), nullptr, &uav_desc, cpu1);
    ID3D12DescriptorHeap* heaps[] = {guide_heap_.Get()};
    list_->SetDescriptorHeaps(1, heaps);
    const float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    list_->ClearUnorderedAccessViewFloat(gpu0, cpu0, motion12_.Get(), zero, 0, nullptr);
    list_->ClearUnorderedAccessViewFloat(gpu1, cpu1, depth12_.Get(), zero, 0, nullptr);
    return SubmitAndWait(detail);
}

bool NvDlssSr::CreateFeature(int preset, int quality, std::string& detail) {
    auto* params = static_cast<NVSDK_NGX_Parameter*>(parameters_);
    params->Set(NVSDK_NGX_Parameter_Width, static_cast<unsigned>(in_width_));
    params->Set(NVSDK_NGX_Parameter_Height, static_cast<unsigned>(in_height_));
    params->Set(NVSDK_NGX_Parameter_OutWidth, static_cast<unsigned>(out_width_));
    params->Set(NVSDK_NGX_Parameter_OutHeight, static_cast<unsigned>(out_height_));
    params->Set(NVSDK_NGX_Parameter_PerfQualityValue,
                     static_cast<NVSDK_NGX_PerfQuality_Value>(
                         std::clamp(quality, 0,
                                    static_cast<int>(NVSDK_NGX_PerfQuality_Value_DLAA))));
    params->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,
                     static_cast<int>(NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
                                      NVSDK_NGX_DLSS_Feature_Flags_AutoExposure));
    params->Set(NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects, 0);
    const char* hints[] = {
        NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA,
        NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
        NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced,
        NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
        NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance,
        NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality,
    };
    for (const char* hint : hints) {
        params->Set(hint, preset);
    }

    NVSDK_NGX_Handle* feature = nullptr;
    const int result = api_->create(list_.Get(), NVSDK_NGX_Feature_SuperSampling, params,
                                    &feature);
    if (!NVSDK_NGX_SUCCEED(static_cast<NVSDK_NGX_Result>(result)) || !feature) {
        detail = "CreateFeature(SR) failed " + ErrorText(result);
        return false;
    }
    feature_ = feature;
    if (!SubmitAndWait(detail)) {
        return false;
    }
    return true;
}

bool NvDlssSr::SubmitAndWait(std::string& detail) {
    list_->Close();
    ID3D12CommandList* lists[] = {list_.Get()};
    queue_->ExecuteCommandLists(1, lists);
    const UINT64 value = ++fence_value_;
    queue_->Signal(fence12_.Get(), value);
    // Wait on the fence event rather than polling with Sleep(1): the default
    // Windows timer granularity is 15.6 ms, so a Sleep-based poll reports the
    // GPU as ~15 ms slower than it is.
    if (!wait_event_) {
        wait_event_ = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }
    bool completed = false;
    if (wait_event_) {
        if (SUCCEEDED(fence12_->SetEventOnCompletion(value, wait_event_))) {
            completed = ::WaitForSingleObject(wait_event_, 5000) == WAIT_OBJECT_0;
        }
    } else {
        for (int spin = 0; spin < 5000 && fence12_->GetCompletedValue() < value; ++spin) {
            ::Sleep(0);
        }
        completed = fence12_->GetCompletedValue() >= value;
    }
    if (!completed) {
        detail = "DLSS GPU fence timed out";
        return false;
    }
    if (context4_) {
        context4_->Wait(fence11_.Get(), value);
    }
    allocator_->Reset();
    list_->Reset(allocator_.Get(), nullptr);
    return true;
}

bool NvDlssSr::Process(const uint8_t* rgba, int width, int height, Frame& out,
                       std::string& detail) {
    auto* params = static_cast<NVSDK_NGX_Parameter*>(parameters_);
    if (!feature_ || !rgba || width != in_width_ || height != in_height_) {
        detail = "DLSS SR is not initialized for this frame size";
        return false;
    }
    const double started = NowMs();
    context_->UpdateSubresource(input11_.Get(), 0, nullptr, rgba,
                                static_cast<UINT>(width) * 4u, 0);
    const UINT64 input_value = ++fence_value_;
    context4_->Signal(fence11_.Get(), input_value);
    context_->Flush();
    queue_->Wait(fence12_.Get(), input_value);

    auto barrier = [&](ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                       D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
        list_->ResourceBarrier(1, &b);
    };
    barrier(input12_.Get(), D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    barrier(motion12_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    barrier(depth12_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    barrier(output12_.Get(), D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    params->Set(NVSDK_NGX_Parameter_Color, input12_.Get());
    params->Set(NVSDK_NGX_Parameter_Output, output12_.Get());
    params->Set(NVSDK_NGX_Parameter_Depth, depth12_.Get());
    params->Set(NVSDK_NGX_Parameter_MotionVectors, motion12_.Get());
    params->Set(NVSDK_NGX_Parameter_Jitter_Offset_X, 0.0f);
    params->Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, 0.0f);
    params->Set(NVSDK_NGX_Parameter_Sharpness, 0.3f);
    params->Set(NVSDK_NGX_Parameter_Reset, 1);
    params->Set(NVSDK_NGX_Parameter_MV_Scale_X, 1.0f);
    params->Set(NVSDK_NGX_Parameter_MV_Scale_Y, 1.0f);
    params->Set("DLSS.Input.Color.Subrect.Base.X", 0u);
    params->Set("DLSS.Input.Color.Subrect.Base.Y", 0u);
    params->Set("DLSS.Input.Depth.Subrect.Base.X", 0u);
    params->Set("DLSS.Input.Depth.Subrect.Base.Y", 0u);
    params->Set("DLSS.Input.MV.Subrect.Base.X", 0u);
    params->Set("DLSS.Input.MV.Subrect.Base.Y", 0u);
    params->Set("DLSS.Output.Subrect.Base.X", 0u);
    params->Set("DLSS.Output.Subrect.Base.Y", 0u);
    params->Set("DLSS.Render.Subrect.Dimensions.Width",
                     static_cast<unsigned>(in_width_));
    params->Set("DLSS.Render.Subrect.Dimensions.Height",
                     static_cast<unsigned>(in_height_));

    const int result = api_->evaluate(list_.Get(), static_cast<NVSDK_NGX_Handle*>(feature_),
                                      params, nullptr);
    if (!NVSDK_NGX_SUCCEED(static_cast<NVSDK_NGX_Result>(result))) {
        detail = "EvaluateFeature(SR) failed " + ErrorText(result);
        return false;
    }
    barrier(input12_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_COMMON);
    barrier(motion12_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    barrier(depth12_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    barrier(output12_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_COMMON);
    if (!SubmitAndWait(detail)) {
        return false;
    }
    out.texture = output11_;
    out.srv = output_srv_;
    last_ms_ = NowMs() - started;
    status_ = "upscaling";
    return true;
}

bool NvDlssSr::Readback(std::vector<uint8_t>& out, std::string& detail) {
    if (!output11_ || !context_ || !device_) {
        detail = "DLSS SR has no output to read back";
        return false;
    }
    D3D11_TEXTURE2D_DESC desc{};
    output11_->GetDesc(&desc);
    if (!output_staging_) {
        D3D11_TEXTURE2D_DESC staging = desc;
        staging.Usage = D3D11_USAGE_STAGING;
        staging.BindFlags = 0;
        staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        staging.MiscFlags = 0;
        if (FAILED(device_->CreateTexture2D(&staging, nullptr, &output_staging_))) {
            detail = "cannot create the DLSS readback texture";
            return false;
        }
    }
    context_->CopyResource(output_staging_.Get(), output11_.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context_->Map(output_staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        detail = "cannot map the DLSS readback texture";
        return false;
    }
    const size_t row = static_cast<size_t>(desc.Width) * 4;
    out.resize(row * desc.Height);
    const auto* src = static_cast<const uint8_t*>(mapped.pData);
    for (UINT y = 0; y < desc.Height; ++y) {
        std::memcpy(out.data() + row * y, src + static_cast<size_t>(mapped.RowPitch) * y, row);
    }
    context_->Unmap(output_staging_.Get(), 0);
    return true;
}

void NvDlssSr::Shutdown() {    if (feature_ && api_ && api_->release) {
        if (queue_ && fence12_) {
            // Drain before releasing the feature.
            const UINT64 value = ++fence_value_;
            queue_->Signal(fence12_.Get(), value);
            if (wait_event_) {
                if (SUCCEEDED(fence12_->SetEventOnCompletion(value, wait_event_))) {
                    ::WaitForSingleObject(wait_event_, 5000);
                }
            } else {
                for (int spin = 0; spin < 5000 && fence12_->GetCompletedValue() < value; ++spin) {
                    ::Sleep(1);
                }
            }
        }
        api_->release(static_cast<NVSDK_NGX_Handle*>(feature_));
        feature_ = nullptr;
    }
    // Capability parameters belong to the core session and are reused when the
    // feature is re-created live (preset/quality/target changes).
    input11_.Reset();
    input12_.Reset();
    output11_.Reset();
    output12_.Reset();
    output_staging_.Reset();
    output_srv_.Reset();
    motion12_.Reset();
    depth12_.Reset();
    guide_heap_.Reset();
    if (wait_event_) {
        ::CloseHandle(wait_event_);
        wait_event_ = nullptr;
    }
    in_width_ = 0;
    in_height_ = 0;
    out_width_ = 0;
    out_height_ = 0;
    preset_ = -1;
    quality_ = -1;
    initialized_ = false;
    // The D3D12 device, queue and NGX core stay alive so a settings change can
    // re-create the feature without restarting the app.
}

}  // namespace brp::media
