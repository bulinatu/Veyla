// DLSS Super Resolution probe on D3D12, modeled on Magpie's captured-colour
// adapter: zero depth + zero motion vectors, optional preset K/L/M.
//
// Usage: dlss_sr_d3d12_probe [dlss_dir] [out_w] [out_h] [preset] [quality]
//   quality: 0 Balanced, 1 MaxQuality

#include <windows.h>

#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <nvsdk_ngx.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

constexpr unsigned long long kAppId = 0x0876232Cull;
constexpr int kInW = 1920;
constexpr int kInH = 1080;

using InitFn = NVSDK_NGX_Result(NVSDK_CONV*)(unsigned long long, const wchar_t*, ID3D12Device*,
                                             NVSDK_NGX_Version);
using GetCapFn = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter**);
using CreateFn = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature,
                                                NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
using EvalFn = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*,
                                              const NVSDK_NGX_Parameter*,
                                              PFN_NVSDK_NGX_ProgressCallback);
using ReleaseFn = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Handle*);

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

std::vector<uint8_t> MakeFrame() {
    std::vector<uint8_t> rgba(static_cast<size_t>(kInW) * kInH * 4);
    for (int y = 0; y < kInH; ++y) {
        for (int x = 0; x < kInW; ++x) {
            const size_t i = (static_cast<size_t>(y) * kInW + x) * 4;
            uint8_t r = static_cast<uint8_t>(x * 255 / (kInW - 1));
            uint8_t g = static_cast<uint8_t>(y * 255 / (kInH - 1));
            uint8_t b = static_cast<uint8_t>(((x / 64 + y / 64) & 1) ? 190 : 60);
            if (((x + y * 2) % 37) == 0) {
                r = 250; g = 250; b = 250;
            }
            if (x > kInW * 3 / 4 && y > 80 && y < 300) {
                const bool ink = ((x / 6) % 4 == 0) || ((y / 8) % 5 == 0);
                r = ink ? 245 : 10; g = ink ? 245 : 10; b = ink ? 245 : 10;
            }
            rgba[i + 0] = r; rgba[i + 1] = g; rgba[i + 2] = b; rgba[i + 3] = 255;
        }
    }
    return rgba;
}

bool SaveBmp(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* texture,
             const char* path) {
    D3D11_TEXTURE2D_DESC td{};
    texture->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&td, nullptr, &staging))) return false;
    context->CopyResource(staging.Get(), texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;
    const uint32_t width = td.Width, height = td.Height;
    const uint32_t row = width * 4, image_bytes = row * height;
    std::FILE* file = nullptr;
    if (fopen_s(&file, path, "wb") != 0 || !file) {
        context->Unmap(staging.Get(), 0);
        return false;
    }
    unsigned char header[54] = {};
    header[0] = 'B'; header[1] = 'M';
    const uint32_t file_size = 54 + image_bytes;
    std::memcpy(header + 2, &file_size, 4);
    const uint32_t offset = 54; std::memcpy(header + 10, &offset, 4);
    const uint32_t info = 40; std::memcpy(header + 14, &info, 4);
    std::memcpy(header + 18, &width, 4); std::memcpy(header + 22, &height, 4);
    const uint16_t planes = 1; std::memcpy(header + 26, &planes, 2);
    const uint16_t bpp = 32; std::memcpy(header + 28, &bpp, 2);
    std::memcpy(header + 34, &image_bytes, 4);
    std::fwrite(header, 1, sizeof(header), file);
    std::vector<unsigned char> line(row);
    const auto* src = static_cast<const unsigned char*>(mapped.pData);
    for (int y = static_cast<int>(height) - 1; y >= 0; --y) {
        const unsigned char* in = src + static_cast<size_t>(y) * mapped.RowPitch;
        for (uint32_t x = 0; x < width; ++x) {
            line[x * 4 + 0] = in[x * 4 + 2];
            line[x * 4 + 1] = in[x * 4 + 1];
            line[x * 4 + 2] = in[x * 4 + 0];
            line[x * 4 + 3] = 255;
        }
        std::fwrite(line.data(), 1, row, file);
    }
    std::fclose(file);
    context->Unmap(staging.Get(), 0);
    return true;
}

void PrintResult(const char* step, NVSDK_NGX_Result result) {
    std::printf("%-26s 0x%08X %s\n", step, static_cast<unsigned>(result),
                NVSDK_NGX_SUCCEED(result) ? "OK" : "FAIL");
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const std::wstring exe_dir = ExeDirectory();
    const std::wstring core_dir = NgxCoreDirectory();
    const std::wstring runtime_dir =
        argc > 1 ? std::wstring(argv[1], argv[1] + std::strlen(argv[1])) : exe_dir;
    const int out_w = argc > 2 ? std::atoi(argv[2]) : 2560;
    const int out_h = argc > 3 ? std::atoi(argv[3]) : 1440;
    const int preset = argc > 4 ? std::atoi(argv[4]) : 11;
    const int quality = argc > 5 ? std::atoi(argv[5]) : 0;

    std::printf("exe %ls\ncore %ls\nruntime %ls\n", exe_dir.c_str(), core_dir.c_str(),
                runtime_dir.c_str());
    ::SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_USER_DIRS);
    ::AddDllDirectory(exe_dir.c_str());
    ::AddDllDirectory(runtime_dir.c_str());
    if (!core_dir.empty()) ::AddDllDirectory(core_dir.c_str());

    // 1. Core before any device.
    std::wstring core_path = core_dir + L"\\_nvngx.dll";
    if (::GetFileAttributesW(core_path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        core_path = core_dir + L"\\nvngx.dll";
    }
    HMODULE core = ::LoadLibraryExW(core_path.c_str(), nullptr, 0);
    if (!core) {
        std::fprintf(stderr, "core load failed %lu\n", ::GetLastError());
        return 1;
    }
    auto init = reinterpret_cast<InitFn>(::GetProcAddress(core, "NVSDK_NGX_D3D12_Init"));
    auto get_cap = reinterpret_cast<GetCapFn>(
        ::GetProcAddress(core, "NVSDK_NGX_D3D12_GetCapabilityParameters"));
    auto create_feature = reinterpret_cast<CreateFn>(
        ::GetProcAddress(core, "NVSDK_NGX_D3D12_CreateFeature"));
    auto evaluate = reinterpret_cast<EvalFn>(
        ::GetProcAddress(core, "NVSDK_NGX_D3D12_EvaluateFeature"));
    auto release_feature = reinterpret_cast<ReleaseFn>(
        ::GetProcAddress(core, "NVSDK_NGX_D3D12_ReleaseFeature"));
    if (!init || !get_cap || !create_feature || !evaluate) {
        std::fprintf(stderr, "core exports missing\n");
        return 2;
    }

    // 2. Devices on the same adapter.
    ComPtr<IDXGIFactory4> factory;
    CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && desc.VendorId == 0x10DE) break;
    }
    ComPtr<ID3D11Device> d11;
    ComPtr<ID3D11DeviceContext> context11;
    ComPtr<ID3D11Device5> d11_5;
    ComPtr<ID3D11DeviceContext4> context4;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    HRESULT hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2,
                                   D3D11_SDK_VERSION, &d11, nullptr, &context11);
    if (FAILED(hr)) { std::fprintf(stderr, "D3D11 device failed\n"); return 3; }
    d11.As(&d11_5);
    context11.As(&context4);

    ComPtr<ID3D12Device> d12;
    hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d12));
    if (FAILED(hr)) { std::fprintf(stderr, "D3D12 device failed\n"); return 3; }

    PrintResult("NGX D3D12 Init", init(kAppId, exe_dir.c_str(), d12.Get(), NVSDK_NGX_Version_API));
    NVSDK_NGX_Parameter* params = nullptr;
    PrintResult("GetCapabilityParameters", get_cap(&params));
    if (!params) return 4;

    params->Set(NVSDK_NGX_Parameter_Width, static_cast<unsigned>(kInW));
    params->Set(NVSDK_NGX_Parameter_Height, static_cast<unsigned>(kInH));
    params->Set(NVSDK_NGX_Parameter_OutWidth, static_cast<unsigned>(out_w));
    params->Set(NVSDK_NGX_Parameter_OutHeight, static_cast<unsigned>(out_h));
    params->Set(NVSDK_NGX_Parameter_PerfQualityValue,
                quality == 1 ? NVSDK_NGX_PerfQuality_Value_MaxQuality
                             : NVSDK_NGX_PerfQuality_Value_Balanced);
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
    for (const char* hint : hints) params->Set(hint, preset);

    // 3. Shared color/output + zero guides.
    auto create_shared = [&](int w, int h, DXGI_FORMAT format, const void* init_data,
                             UINT pitch, bool uav, ComPtr<ID3D11Texture2D>& t11,
                             ComPtr<ID3D12Resource>& t12) -> bool {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = w; desc.Height = h; desc.MipLevels = 1; desc.ArraySize = 1;
        desc.Format = format; desc.SampleDesc.Count = 1; desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | (uav ? D3D11_BIND_UNORDERED_ACCESS : 0);
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
        D3D11_SUBRESOURCE_DATA data{init_data, pitch, pitch * static_cast<UINT>(h)};
        HRESULT r = d11->CreateTexture2D(&desc, init_data ? &data : nullptr, &t11);
        if (FAILED(r)) return false;
        ComPtr<IDXGIResource1> resource;
        t11.As(&resource);
        HANDLE handle = nullptr;
        r = resource->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &handle);
        if (SUCCEEDED(r)) {
            r = d12->OpenSharedHandle(handle, IID_PPV_ARGS(&t12));
            ::CloseHandle(handle);
        }
        return SUCCEEDED(r);
    };

    const auto pixels = MakeFrame();
    ComPtr<ID3D11Texture2D> color11, output11;
    ComPtr<ID3D12Resource> color12, output12;
    if (!create_shared(kInW, kInH, DXGI_FORMAT_R8G8B8A8_UNORM, pixels.data(), kInW * 4, true,
                       color11, color12) ||
        !create_shared(out_w, out_h, DXGI_FORMAT_R8G8B8A8_UNORM, nullptr, 0, true, output11,
                       output12)) {
        std::fprintf(stderr, "shared textures failed\n");
        return 5;
    }

    // Zero MV/depth as committed D3D12 resources (no sharing needed).
    auto create_tex12 = [&](int w, int h, DXGI_FORMAT format, ComPtr<ID3D12Resource>& out) {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = w; desc.Height = h; desc.DepthOrArraySize = 1; desc.MipLevels = 1;
        desc.Format = format; desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        return d12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                            IID_PPV_ARGS(&out));
    };
    ComPtr<ID3D12Resource> motion12, depth12;
    if (FAILED(create_tex12(kInW, kInH, DXGI_FORMAT_R16G16_FLOAT, motion12)) ||
        FAILED(create_tex12(kInW, kInH, DXGI_FORMAT_R32_FLOAT, depth12))) {
        std::fprintf(stderr, "guide resources failed\n");
        return 5;
    }

    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    d12->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue));
    d12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
    d12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                           IID_PPV_ARGS(&list));

    // Zero guides need a descriptor heap for the clear.
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.NumDescriptors = 2;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> uav_heap;
    d12->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&uav_heap));
    const UINT increment = d12->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu0 = uav_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_CPU_DESCRIPTOR_HANDLE cpu1 = cpu0;
    cpu1.ptr += increment;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu0 = uav_heap->GetGPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gpu1 = gpu0;
    gpu1.ptr += increment;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc{};
    uav_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uav_desc.Format = DXGI_FORMAT_R16G16_FLOAT;
    d12->CreateUnorderedAccessView(motion12.Get(), nullptr, &uav_desc, cpu0);
    uav_desc.Format = DXGI_FORMAT_R32_FLOAT;
    d12->CreateUnorderedAccessView(depth12.Get(), nullptr, &uav_desc, cpu1);
    ID3D12DescriptorHeap* heaps[] = {uav_heap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    const float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    list->ClearUnorderedAccessViewFloat(gpu0, cpu0, motion12.Get(), zero, 0, nullptr);
    list->ClearUnorderedAccessViewFloat(gpu1, cpu1, depth12.Get(), zero, 0, nullptr);

    ComPtr<ID3D12Fence> fence;
    d12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    UINT64 fence_value = 0;
    auto submit_and_wait = [&]() {
        list->Close();
        ID3D12CommandList* lists[] = {list.Get()};
        queue->ExecuteCommandLists(1, lists);
        const UINT64 value = ++fence_value;
        queue->Signal(fence.Get(), value);
        while (fence->GetCompletedValue() < value) {
            ::Sleep(1);
        }
        allocator->Reset();
        list->Reset(allocator.Get(), nullptr);
    };
    submit_and_wait();

    NVSDK_NGX_Handle* feature = nullptr;
    PrintResult("CreateFeature(SR)", create_feature(list.Get(), NVSDK_NGX_Feature_SuperSampling,
                                                    params, &feature));
    submit_and_wait();
    if (!feature) return 6;

    auto barrier = [&](ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                       D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
        list->ResourceBarrier(1, &b);
    };

    constexpr int kRuns = 8;
    for (int run = 0; run < kRuns; ++run) {
        barrier(color12.Get(), D3D12_RESOURCE_STATE_COMMON,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        barrier(motion12.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        barrier(depth12.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        barrier(output12.Get(), D3D12_RESOURCE_STATE_COMMON,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        params->Set(NVSDK_NGX_Parameter_Color, color12.Get());
        params->Set(NVSDK_NGX_Parameter_Output, output12.Get());
        params->Set(NVSDK_NGX_Parameter_Depth, depth12.Get());
        params->Set(NVSDK_NGX_Parameter_MotionVectors, motion12.Get());
        params->Set(NVSDK_NGX_Parameter_Jitter_Offset_X, 0.0f);
        params->Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, 0.0f);
        params->Set(NVSDK_NGX_Parameter_Sharpness, 0.3f);
        params->Set(NVSDK_NGX_Parameter_Reset, run == 0 ? 1 : 0);
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
        params->Set("DLSS.Render.Subrect.Dimensions.Width", static_cast<unsigned>(kInW));
        params->Set("DLSS.Render.Subrect.Dimensions.Height", static_cast<unsigned>(kInH));
        const auto start = std::chrono::steady_clock::now();
        const NVSDK_NGX_Result result = evaluate(list.Get(), feature, params, nullptr);
        PrintResult("EvaluateFeature(SR)", result);
        barrier(color12.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_COMMON);
        barrier(motion12.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        barrier(depth12.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        barrier(output12.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_COMMON);
        submit_and_wait();
        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - start)
                              .count();
        std::printf("run %d: %.2f ms\n", run, ms);
        if (!NVSDK_NGX_SUCCEED(result)) return 7;
    }

    SaveBmp(d11.Get(), context11.Get(), output11.Get(), "build/shots/dlss-sr-d3d12.bmp");
    std::printf("wrote build/shots/dlss-sr-d3d12.bmp\n");
    if (release_feature) release_feature(feature);
    return 0;
}
