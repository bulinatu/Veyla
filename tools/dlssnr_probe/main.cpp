// Minimal DLSSNR host probe (diagnostic only).
//
// It reproduces the two non-obvious steps Magpie uses to make the community
// DLSSNR DLL work in a standalone process:
//   1. initialize the NGX core with Magpie's project id (CMS id 876232c),
//   2. load nvngx_dlssnr.dll and call its own NVSDK_NGX_D3D12_Init_Ext, after
//      patching the DLL's GetModuleFileNameW import so it reports "nvngx.dll"
//      as its caller (the signed snippet refuses any other caller).
//
// It then allocates NGX parameters, creates feature 18 and evaluates it on a
// synthetic 1080p frame through shared D3D11/D3D12 textures, writing
// build/shots/dlssnr-input.bmp and dlssnr-output.bmp.
//
// Usage: dlssnr_probe [nvngx_dlssnr.dll] [appid|project] [preset] [style] [guides]
//   guides: 0 none, 1 motion, 2 depth, 3 both (default)
//
// Build requires the NGX SDK headers:
//   cmake -DNGX_SDK_INC=C:/path/to/ngx/inc ...
//   cmake --build build --config Release --target dlssnr_probe

#include <windows.h>

#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <nvsdk_ngx.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

constexpr unsigned long long kAppId = 0x0876232Cull;
constexpr NVSDK_NGX_Feature kFeatureDlssNr = static_cast<NVSDK_NGX_Feature>(18);

using InitProjectIdFn = NVSDK_NGX_Result(NVSDK_CONV*)(
    const char*, NVSDK_NGX_EngineType, const char*, const wchar_t*, ID3D12Device*,
    const NVSDK_NGX_FeatureCommonInfo*, NVSDK_NGX_Version);
using InitAppIdFn = NVSDK_NGX_Result(NVSDK_CONV*)(unsigned long long, const wchar_t*,
                                                  ID3D12Device*, NVSDK_NGX_Version);
using AllocateParametersFn = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter**);
using InitExtFn = NVSDK_NGX_Result(NVSDK_CONV*)(unsigned long long, const wchar_t*,
                                                ID3D12Device*, NVSDK_NGX_Version,
                                                const NVSDK_NGX_Parameter*);
using CreateFeatureFn = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*,
                                                      NVSDK_NGX_Feature, NVSDK_NGX_Parameter*,
                                                      NVSDK_NGX_Handle**);
using EvaluateFeatureFn = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*,
                                                        const NVSDK_NGX_Handle*,
                                                        const NVSDK_NGX_Parameter*,
                                                        PFN_NVSDK_NGX_ProgressCallback);
using ReleaseFeatureFn = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Handle*);

HMODULE g_caller_module = nullptr;
decltype(&GetModuleFileNameW) g_original_get_module_file_name = nullptr;

DWORD WINAPI HookGetModuleFileNameW(HMODULE module, LPWSTR filename, DWORD size) {
    if (module == g_caller_module) {
        static constexpr wchar_t kAuthorized[] = L"nvngx.dll";
        constexpr DWORD kLength = 10;  // wcslen(L"nvngx.dll")
        if (!filename || !size) {
            ::SetLastError(ERROR_INSUFFICIENT_BUFFER);
            return 0;
        }
        if (size <= kLength) {
            if (size > 1) {
                std::memcpy(filename, kAuthorized, (size - 1) * sizeof(wchar_t));
            }
            filename[size - 1] = L'\0';
            ::SetLastError(ERROR_INSUFFICIENT_BUFFER);
            return size;
        }
        std::memcpy(filename, kAuthorized, sizeof(kAuthorized));
        return kLength;
    }
    if (g_original_get_module_file_name) {
        return g_original_get_module_file_name(module, filename, size);
    }
    ::SetLastError(ERROR_INVALID_FUNCTION);
    return 0;
}

void** FindImportedFunctionSlot(HMODULE module, const char* function_name) {
    if (!module || !function_name) {
        return nullptr;
    }
    auto* base = reinterpret_cast<std::byte*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) {
        return nullptr;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        return nullptr;
    }
    const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!directory.VirtualAddress || !directory.Size) {
        return nullptr;
    }
    auto* descriptor =
        reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress);
    const auto* end = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(
        base + directory.VirtualAddress + directory.Size);
    for (; descriptor < end && descriptor->Name; ++descriptor) {
        const char* library = reinterpret_cast<const char*>(base + descriptor->Name);
        if (_stricmp(library, "KERNEL32.dll") != 0 &&
            _stricmp(library, "api-ms-win-core-libraryloader-l1-2-0.dll") != 0 &&
            _stricmp(library, "api-ms-win-core-libraryloader-l1-1-0.dll") != 0) {
            continue;
        }
        if (!descriptor->OriginalFirstThunk || !descriptor->FirstThunk) {
            continue;
        }
        auto* name_thunk =
            reinterpret_cast<IMAGE_THUNK_DATA64*>(base + descriptor->OriginalFirstThunk);
        auto* address_thunk =
            reinterpret_cast<IMAGE_THUNK_DATA64*>(base + descriptor->FirstThunk);
        for (; name_thunk->u1.AddressOfData; ++name_thunk, ++address_thunk) {
            if (IMAGE_SNAP_BY_ORDINAL64(name_thunk->u1.Ordinal)) {
                continue;
            }
            const auto* import =
                reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + name_thunk->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(import->Name), function_name) == 0) {
                return reinterpret_cast<void**>(&address_thunk->u1.Function);
            }
        }
    }
    return nullptr;
}

bool InstallCallerHook(HMODULE snippet) {
    if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                  GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              reinterpret_cast<LPCWSTR>(&HookGetModuleFileNameW),
                              &g_caller_module)) {
        std::fprintf(stderr, "GetModuleHandleExW(caller) failed: %lu\n", ::GetLastError());
        return false;
    }
    void** slot = FindImportedFunctionSlot(snippet, "GetModuleFileNameW");
    if (!slot) {
        std::fprintf(stderr, "GetModuleFileNameW import not found in nvngx_dlssnr.dll\n");
        return false;
    }
    DWORD old = 0;
    if (!::VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
        std::fprintf(stderr, "VirtualProtect failed: %lu\n", ::GetLastError());
        return false;
    }
    void* original = ::InterlockedExchangePointer(
        reinterpret_cast<void* volatile*>(slot),
        reinterpret_cast<void*>(&HookGetModuleFileNameW));
    std::memcpy(&g_original_get_module_file_name, &original, sizeof(original));
    DWORD ignored = 0;
    ::VirtualProtect(slot, sizeof(void*), old, &ignored);
    ::FlushInstructionCache(::GetCurrentProcess(), slot, sizeof(void*));
    if (!g_original_get_module_file_name) {
        std::fprintf(stderr, "original GetModuleFileNameW was null\n");
        return false;
    }
    return true;
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

std::wstring ExeDirectory() {
    wchar_t path[MAX_PATH] = {};
    ::GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring text = path;
    const size_t slash = text.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : text.substr(0, slash);
}

struct Devices {
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D11Device> d11;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Device5> d11_5;
    ComPtr<ID3D11DeviceContext4> context4;
    ComPtr<ID3D12Device> d12;
};

bool CreateDevices(Devices& out) {
    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        return false;
    }
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
            continue;
        }
        std::printf("adapter %u: %ls (vendor 0x%04X)\n", i, desc.Description, desc.VendorId);
        if (desc.VendorId == 0x10DE && !out.adapter) {
            out.adapter = adapter;
        }
    }
    if (!out.adapter) {
        return false;
    }
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    HRESULT hr = D3D11CreateDevice(out.adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2,
                                   D3D11_SDK_VERSION, &out.d11, nullptr, &out.context);
    std::printf("D3D11CreateDevice hr=0x%08lX\n", static_cast<unsigned long>(hr));
    if (FAILED(hr)) {
        return false;
    }
    out.d11.As(&out.d11_5);
    out.context.As(&out.context4);
    if (!out.d11_5 || !out.context4) {
        std::fprintf(stderr, "D3D11.4 (fence) support is missing\n");
        return false;
    }
    std::printf("creating D3D12 device...\n");
    hr = D3D12CreateDevice(out.adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&out.d12));
    std::printf("D3D12CreateDevice hr=0x%08lX\n", static_cast<unsigned long>(hr));
    return SUCCEEDED(hr);
}

NVSDK_NGX_Result NVSDK_CONV ScalingRatioCallback(NVSDK_NGX_Parameter* parameters) {
    if (!parameters) {
        return NVSDK_NGX_Result_FAIL_InvalidParameter;
    }
    parameters->Set("DLSSNR.ScalingRatio", 1.0f);
    return NVSDK_NGX_Result_Success;
}

void PrintResult(const char* step, NVSDK_NGX_Result result) {
    std::printf("%-28s 0x%08X %s\n", step, static_cast<unsigned>(result),
                NVSDK_NGX_SUCCEED(result) ? "OK" : "FAIL");
}

std::vector<uint8_t> MakeFrame(int width, int height, int shift = 0) {
    std::vector<uint8_t> rgba(static_cast<size_t>(width) * height * 4);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const size_t i = (static_cast<size_t>(y) * width + x) * 4;
            const int sx = (x + shift) % width;
            uint8_t r = static_cast<uint8_t>(sx * 255 / (width - 1));
            uint8_t g = static_cast<uint8_t>(y * 255 / (height - 1));
            uint8_t b = static_cast<uint8_t>(((sx / 64 + y / 64) & 1) ? 190 : 60);
            if (((sx + y * 2) % 37) == 0) {
                r = 250;
                g = 250;
                b = 250;
            }
            const float dx = static_cast<float>(x - width / 2);
            const float dy = static_cast<float>(y - height / 2);
            if (static_cast<int>(std::sqrt(dx * dx + dy * dy)) % 23 == 0) {
                r = 20;
                g = 20;
                b = 20;
            }
            if (sx > width * 3 / 4 && y > 80 && y < 300) {
                const bool ink = ((sx / 6) % 4 == 0) || ((y / 8) % 5 == 0);
                r = ink ? 245 : 10;
                g = ink ? 245 : 10;
                b = ink ? 245 : 10;
            }
            // Deterministic compression-like noise so a denoiser has something
            // to remove even on the synthetic pattern.
            const int noise = ((sx * 73856093) ^ (y * 19349663)) % 41 - 20;
            auto clamp8 = [](int v) {
                return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
            };
            r = clamp8(r + noise);
            g = clamp8(g + noise);
            b = clamp8(b + noise);
            rgba[i + 0] = r;
            rgba[i + 1] = g;
            rgba[i + 2] = b;
            rgba[i + 3] = 255;
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
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&td, nullptr, &staging))) {
        return false;
    }
    context->CopyResource(staging.Get(), texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        return false;
    }
    const uint32_t width = td.Width;
    const uint32_t height = td.Height;
    const uint32_t row = width * 4;
    const uint32_t image_bytes = row * height;
    std::FILE* file = nullptr;
    if (fopen_s(&file, path, "wb") != 0 || !file) {
        context->Unmap(staging.Get(), 0);
        return false;
    }
    unsigned char header[54] = {};
    header[0] = 'B';
    header[1] = 'M';
    const uint32_t file_size = 54 + image_bytes;
    std::memcpy(header + 2, &file_size, 4);
    const uint32_t offset = 54;
    std::memcpy(header + 10, &offset, 4);
    const uint32_t info = 40;
    std::memcpy(header + 14, &info, 4);
    std::memcpy(header + 18, &width, 4);
    std::memcpy(header + 22, &height, 4);
    const uint16_t planes = 1;
    std::memcpy(header + 26, &planes, 2);
    const uint16_t bpp = 32;
    std::memcpy(header + 28, &bpp, 2);
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

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    std::error_code dir_ec;
    std::filesystem::create_directories("build/shots", dir_ec);
    const std::wstring exe_dir = ExeDirectory();
    const std::wstring core_dir = NgxCoreDirectory();
    const std::wstring snippet_path =
        argc > 1 ? std::wstring(argv[1], argv[1] + std::strlen(argv[1]))
                 : exe_dir + L"\\nvngx_dlssnr.dll";

    std::printf("exe dir:     %ls\n", exe_dir.c_str());
    std::printf("NGX core:    %ls\n", core_dir.c_str());
    std::printf("snippet:     %ls\n", snippet_path.c_str());

    ::SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_USER_DIRS);
    ::AddDllDirectory(exe_dir.c_str());
    if (!core_dir.empty()) {
        ::AddDllDirectory(core_dir.c_str());
    }

    // 1. Load the NGX core before any D3D12 device exists (Magpie import-links
    //    it, so it is mapped at process start).
    std::wstring core_path = core_dir + L"\\_nvngx.dll";
    if (core_dir.empty() || ::GetFileAttributesW(core_path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        core_path = core_dir + L"\\nvngx.dll";
    }
    if (argc > 8) {
        core_path = std::wstring(argv[8], argv[8] + std::strlen(argv[8]));
    }
    std::printf("loading NGX core %ls\n", core_path.c_str());
    HMODULE core = ::LoadLibraryExW(core_path.c_str(), nullptr, 0);
    if (!core) {
        std::fprintf(stderr, "cannot load NGX core %ls (error %lu)\n", core_path.c_str(),
                     ::GetLastError());
        return 2;
    }
    auto init_project = reinterpret_cast<InitProjectIdFn>(
        ::GetProcAddress(core, "NVSDK_NGX_D3D12_Init_ProjectID"));
    auto init_app_id = reinterpret_cast<InitAppIdFn>(
        ::GetProcAddress(core, "NVSDK_NGX_D3D12_Init"));
    auto allocate_parameters = reinterpret_cast<AllocateParametersFn>(
        ::GetProcAddress(core, "NVSDK_NGX_D3D12_AllocateParameters"));
    if (!allocate_parameters) {
        std::fprintf(stderr, "NGX core is missing AllocateParameters\n");
        return 3;
    }
    std::printf("NGX core loaded\n");

    Devices devices;
    if (!CreateDevices(devices)) {
        std::fprintf(stderr, "D3D11/D3D12 device creation failed\n");
        return 1;
    }
    std::printf("devices acquired: d3d11=%p d3d12=%p\n", devices.d11.Get(), devices.d12.Get());

    const wchar_t* paths[] = {exe_dir.c_str()};
    NVSDK_NGX_FeatureCommonInfo info{};
    info.PathListInfo.Path = paths;
    info.PathListInfo.Length = 1;
    info.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_OFF;
    info.LoggingInfo.DisableOtherLoggingSinks = false;
    info.LoggingInfo.LoggingCallback = nullptr;
    const bool null_info = argc > 2 && std::strcmp(argv[2], "null") == 0;
    const bool use_app_id = !(argc > 2 && std::strcmp(argv[2], "project") == 0);
    NVSDK_NGX_Result core_result;
    if (use_app_id) {
        std::printf("calling Init(app id 0x%08llX)...\n", kAppId);
        core_result = init_app_id ? init_app_id(kAppId, exe_dir.c_str(), devices.d12.Get(),
                                                NVSDK_NGX_Version_API)
                                  : NVSDK_NGX_Result_Fail;
        PrintResult("core Init(app id)", core_result);
    } else {
        std::printf("calling Init_ProjectID (featureInfo=%s)...\n",
                    null_info ? "null" : "app-dir");
        core_result = init_project
                           ? init_project("7c134ab9-9677-4af5-a2b2-bca943350861",
                                          NVSDK_NGX_ENGINE_TYPE_CUSTOM,
                                          "Magpie-Experimental-0.5.7", exe_dir.c_str(),
                                          devices.d12.Get(), null_info ? nullptr : &info,
                                          NVSDK_NGX_Version_API)
                           : NVSDK_NGX_Result_Fail;
        PrintResult("core Init_ProjectID", core_result);
    }
    if (!NVSDK_NGX_SUCCEED(core_result)) {
        return 4;
    }

    // 2. Load the signed NR snippet directly, patch its caller check, init it.
    HMODULE snippet = ::LoadLibraryExW(snippet_path.c_str(), nullptr,
                                       LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                           LOAD_LIBRARY_SEARCH_DEFAULT_DIRS |
                                           LOAD_LIBRARY_SEARCH_USER_DIRS);
    if (!snippet) {
        std::fprintf(stderr, "cannot load %ls (error %lu)\n", snippet_path.c_str(),
                     ::GetLastError());
        return 5;
    }
    auto snippet_init_ext =
        reinterpret_cast<InitExtFn>(::GetProcAddress(snippet, "NVSDK_NGX_D3D12_Init_Ext"));
    auto snippet_create_feature = reinterpret_cast<CreateFeatureFn>(
        ::GetProcAddress(snippet, "NVSDK_NGX_D3D12_CreateFeature"));
    auto snippet_evaluate = reinterpret_cast<EvaluateFeatureFn>(
        ::GetProcAddress(snippet, "NVSDK_NGX_D3D12_EvaluateFeature"));
    auto snippet_release_feature = reinterpret_cast<ReleaseFeatureFn>(
        ::GetProcAddress(snippet, "NVSDK_NGX_D3D12_ReleaseFeature"));
    if (!snippet_init_ext || !snippet_create_feature || !snippet_evaluate) {
        std::fprintf(stderr, "snippet is missing Init_Ext/CreateFeature/EvaluateFeature\n");
        return 6;
    }
    if (!InstallCallerHook(snippet)) {
        return 7;
    }
    const NVSDK_NGX_Result snippet_result =
        snippet_init_ext(kAppId, exe_dir.c_str(), devices.d12.Get(), NVSDK_NGX_Version_API,
                        nullptr);
    PrintResult("snippet Init_Ext", snippet_result);
    if (!NVSDK_NGX_SUCCEED(snippet_result)) {
        return 8;
    }

    // 3. Parameters and feature 18 creation.
    NVSDK_NGX_Parameter* parameters = nullptr;
    const NVSDK_NGX_Result alloc_result = allocate_parameters(&parameters);
    PrintResult("AllocateParameters", alloc_result);
    if (!NVSDK_NGX_SUCCEED(alloc_result) || !parameters) {
        return 9;
    }

    constexpr unsigned kWidth = 1920;
    constexpr unsigned kHeight = 1080;
    const unsigned model_preset =
        argc > 3 ? static_cast<unsigned>(std::atoi(argv[3])) : 0u;
    const unsigned style = argc > 4 ? static_cast<unsigned>(std::atoi(argv[4])) : 0u;
    parameters->Set("DLSSNR.Width", kWidth);
    parameters->Set("DLSSNR.Height", kHeight);
    parameters->Set("DLSSNR.InputWidth", kWidth);
    parameters->Set("DLSSNR.InputHeight", kHeight);
    parameters->Set("DLSSNR.OutputWidth", kWidth);
    parameters->Set("DLSSNR.OutputHeight", kHeight);
    parameters->Set("DLSSNR.Output.Width", kWidth);
    parameters->Set("DLSSNR.Output.Height", kHeight);
    parameters->Set("DLSSNR.Upscaling", 0u);
    parameters->Set("DLSSNR.Scale", 1.0f);
    parameters->Set("DLSSNR.ScalingRatio", 1.0f);
    parameters->Set("DLSSNRComputeScalingRatioCallback",
                    reinterpret_cast<void*>(&ScalingRatioCallback));
    parameters->Set("DLSSNR.Hint.Render.Preset", model_preset);
    parameters->Set(NVSDK_NGX_Parameter_Width, kWidth);
    parameters->Set(NVSDK_NGX_Parameter_Height, kHeight);
    parameters->Set(NVSDK_NGX_Parameter_PerfQualityValue,
                    static_cast<int>(NVSDK_NGX_PerfQuality_Value_Balanced));
    parameters->Set(NVSDK_NGX_Parameter_CreationNodeMask, 1u);
    parameters->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);

    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(devices.d12->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue))) ||
        FAILED(devices.d12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                              IID_PPV_ARGS(&allocator))) ||
        FAILED(devices.d12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(),
                                         nullptr, IID_PPV_ARGS(&list)))) {
        std::fprintf(stderr, "D3D12 command objects failed\n");
        return 10;
    }

    NVSDK_NGX_Handle* feature = nullptr;
    const NVSDK_NGX_Result create_result =
        snippet_create_feature(list.Get(), kFeatureDlssNr, parameters, &feature);
    PrintResult("CreateFeature(18)", create_result);
    std::printf("feature handle: %p\n", static_cast<void*>(feature));
    if (!NVSDK_NGX_SUCCEED(create_result) || !feature) {
        return 11;
    }

    // 4. Shared D3D11/D3D12 textures, zero guides, one evaluation.
    auto create_shared_rgba = [&](ComPtr<ID3D11Texture2D>& t11,
                                  ComPtr<ID3D12Resource>& t12, const void* init,
                                  UINT pitch) -> bool {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = kWidth;
        desc.Height = kHeight;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
        D3D11_SUBRESOURCE_DATA data{init, pitch, pitch * kHeight};
        HRESULT hr = devices.d11->CreateTexture2D(&desc, init ? &data : nullptr, &t11);
        if (FAILED(hr)) {
            std::fprintf(stderr, "shared RGBA texture failed: 0x%08lX\n",
                         static_cast<unsigned long>(hr));
            return false;
        }
        ComPtr<IDXGIResource1> resource;
        t11.As(&resource);
        HANDLE handle = nullptr;
        hr = resource->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &handle);
        if (SUCCEEDED(hr)) {
            hr = devices.d12->OpenSharedHandle(handle, IID_PPV_ARGS(&t12));
            ::CloseHandle(handle);
        }
        if (FAILED(hr)) {
            std::fprintf(stderr, "shared RGBA handoff failed: 0x%08lX\n",
                         static_cast<unsigned long>(hr));
            return false;
        }
        return true;
    };

    auto create_guide = [&](DXGI_FORMAT format, ComPtr<ID3D11Texture2D>& t11,
                            ComPtr<ID3D12Resource>& t12, const void* pattern,
                            size_t pattern_size) -> bool {
        const UINT pitch = kWidth * 4;
        std::vector<uint8_t> data(static_cast<size_t>(pitch) * kHeight, 0);
        for (size_t offset = 0; offset + pattern_size <= data.size(); offset += pattern_size) {
            std::memcpy(data.data() + offset, pattern, pattern_size);
        }
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = kWidth;
        desc.Height = kHeight;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
        D3D11_SUBRESOURCE_DATA initial{data.data(), pitch, pitch * kHeight};
        HRESULT hr = devices.d11->CreateTexture2D(&desc, &initial, &t11);
        if (FAILED(hr)) {
            std::fprintf(stderr, "guide texture failed: 0x%08lX\n",
                         static_cast<unsigned long>(hr));
            return false;
        }
        ComPtr<IDXGIResource1> resource;
        t11.As(&resource);
        HANDLE handle = nullptr;
        hr = resource->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &handle);
        if (SUCCEEDED(hr)) {
            hr = devices.d12->OpenSharedHandle(handle, IID_PPV_ARGS(&t12));
            ::CloseHandle(handle);
        }
        return SUCCEEDED(hr);
    };

    std::vector<uint8_t> magenta(static_cast<size_t>(kWidth) * kHeight * 4, 0);
    for (size_t i = 0; i < magenta.size(); i += 4) {
        magenta[i + 0] = 255;
        magenta[i + 2] = 255;
        magenta[i + 3] = 255;
    }
    ComPtr<ID3D11Texture2D> input11;
    ComPtr<ID3D12Resource> input12;
    ComPtr<ID3D11Texture2D> output11;
    ComPtr<ID3D12Resource> output12;
    ComPtr<ID3D11Texture2D> mid11;
    ComPtr<ID3D12Resource> mid12;
    ComPtr<ID3D11Texture2D> motion11;
    ComPtr<ID3D12Resource> motion12;
    ComPtr<ID3D11Texture2D> depth11;
    ComPtr<ID3D12Resource> depth12;
    const uint16_t motion_pattern[2] = {0x3800, 0x0000};  // half 0.5, half 0.0
    const uint16_t zero_motion_pattern[2] = {0x0000, 0x0000};
    const float depth_pattern = 0.5f;
    const float zero_depth_pattern = 0.0f;
    const int guide_mode = argc > 5 ? std::atoi(argv[5]) : 3;
    const bool animate = argc > 6 && std::atoi(argv[6]) != 0;
    std::vector<std::vector<uint8_t>> file_frames;
    std::vector<std::vector<uint8_t>> file_motion;
    if (argc > 7) {
        namespace fs = std::filesystem;
        for (int index = 1; index < 16; ++index) {
            char name[32];
            std::snprintf(name, sizeof(name), "frame%02d", index);
            const fs::path rgba_path = fs::path(argv[7]) / (std::string(name) + ".rgba");
            const fs::path mv_path = fs::path(argv[7]) / (std::string(name) + ".mv");
            const size_t expected = static_cast<size_t>(kWidth) * kHeight * 4;
            if (!fs::exists(rgba_path) || !fs::exists(mv_path)) {
                break;
            }
            std::vector<uint8_t> rgba(expected);
            std::vector<uint8_t> motion(expected);
            std::FILE* f = nullptr;
            if (fopen_s(&f, rgba_path.string().c_str(), "rb") == 0 && f) {
                const size_t got = fread(rgba.data(), 1, expected, f);
                fclose(f);
                if (got != expected) break;
            } else {
                break;
            }
            if (fopen_s(&f, mv_path.string().c_str(), "rb") == 0 && f) {
                const size_t got = fread(motion.data(), 1, expected, f);
                fclose(f);
                if (got != expected) break;
            } else {
                break;
            }
            file_frames.push_back(std::move(rgba));
            file_motion.push_back(std::move(motion));
        }
        std::printf("loaded %zu file frames from %s\n", file_frames.size(), argv[7]);
    }
    const bool guide_motion = (guide_mode & 1) != 0;
    const bool guide_depth = (guide_mode & 2) != 0;
    const auto frame_a = MakeFrame(kWidth, kHeight, 0);
    const auto frame_b = MakeFrame(kWidth, kHeight, 11);
    const auto input_pixels = frame_a;
    if (!create_shared_rgba(input11, input12, input_pixels.data(), kWidth * 4) ||
        !create_shared_rgba(output11, output12, magenta.data(), kWidth * 4) ||
        !create_shared_rgba(mid11, mid12, magenta.data(), kWidth * 4) ||
        !create_guide(DXGI_FORMAT_R16G16_FLOAT, motion11, motion12,
                      guide_motion ? motion_pattern : zero_motion_pattern,
                      sizeof(motion_pattern)) ||
        !create_guide(DXGI_FORMAT_R32_FLOAT, depth11, depth12,
                      guide_depth ? &depth_pattern : &zero_depth_pattern,
                      sizeof(depth_pattern))) {
        return 12;
    }

    ComPtr<ID3D11Fence> fence11;
    ComPtr<ID3D12Fence> fence12;
    HANDLE fence_handle = nullptr;
    if (FAILED(devices.d11_5->CreateFence(0, D3D11_FENCE_FLAG_SHARED,
                                          IID_PPV_ARGS(&fence11))) ||
        FAILED(fence11->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &fence_handle)) ||
        FAILED(devices.d12->OpenSharedHandle(fence_handle, IID_PPV_ARGS(&fence12)))) {
        std::fprintf(stderr, "shared fence failed\n");
        return 13;
    }
    ::CloseHandle(fence_handle);

    auto subrect = [&](const char* base_x, const char* base_y, const char* width,
                       const char* height) {
        parameters->Set(base_x, 0u);
        parameters->Set(base_y, 0u);
        parameters->Set(width, kWidth);
        parameters->Set(height, kHeight);
    };
    subrect("DLSSNR.ColorSubrectBaseX", "DLSSNR.ColorSubrectBaseY",
            "DLSSNR.ColorSubrectWidth", "DLSSNR.ColorSubrectHeight");
    subrect("DLSSNR.OutputSubrectBaseX", "DLSSNR.OutputSubrectBaseY",
            "DLSSNR.OutputSubrectWidth", "DLSSNR.OutputSubrectHeight");
    const bool omit_guides = guide_mode == 5;
    if (!omit_guides) {
        subrect("DLSSNR.MVecSubrectBaseX", "DLSSNR.MVecSubrectBaseY",
                "DLSSNR.MVecSubrectWidth", "DLSSNR.MVecSubrectHeight");
        subrect("DLSSNR.DepthSubrectBaseX", "DLSSNR.DepthSubrectBaseY",
                "DLSSNR.DepthSubrectWidth", "DLSSNR.DepthSubrectHeight");
        parameters->Set("DLSSNR.MVec", motion12.Get());
        parameters->Set("DLSSNR.Depth", depth12.Get());
    }
    parameters->Set("DLSSNR.Color", input12.Get());
    parameters->Set("DLSSNR.Output", output12.Get());
    parameters->Set("DLSSNR.MVecScaleX", 1.0f);
    parameters->Set("DLSSNR.MVecScaleY", 1.0f);
    parameters->Set("DLSSNR.DepthInverted", 1);
    parameters->Set("DLSS.Indicator.Invert.X.Axis", 0);
    parameters->Set("DLSS.Indicator.Invert.Y.Axis", 0);
    parameters->Set("DLSSNR.Enabled", 1);
    parameters->Set("DLSSNR.Style", static_cast<int>(style));
    parameters->Set("DLSSNR.Intensity", 1.0f);
    parameters->Set("DLSSNR.LocalToneStrength", 1.0f);
    parameters->Set("DLSSNR.LocalStructureStrength", 1.0f);
    parameters->Set("DLSSNR.SkinStructureStrength", 0.0f);
    parameters->Set("DLSSNR.UseAutoMask", 0);
    parameters->Set("DLSSNR.UICorrection", 0);

    auto barrier = [&](ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                       D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
        list->ResourceBarrier(1, &b);
    };

    const int kRuns = file_frames.empty() ? 4 : static_cast<int>(file_frames.size());
    for (int run = 0; run < kRuns; ++run) {
        if (!file_frames.empty()) {
            devices.context->UpdateSubresource(input11.Get(), 0, nullptr,
                                               file_frames[run].data(), kWidth * 4, 0);
            devices.context->UpdateSubresource(motion11.Get(), 0, nullptr,
                                               file_motion[run].data(), kWidth * 4, 0);
            devices.context4->Signal(fence11.Get(), 100 + static_cast<UINT64>(run));
            devices.context->Flush();
            queue->Wait(fence12.Get(), 100 + static_cast<UINT64>(run));
        } else if (animate) {
            const auto& pixels = (run & 1) ? frame_b : frame_a;
            devices.context->UpdateSubresource(input11.Get(), 0, nullptr, pixels.data(),
                                               kWidth * 4, 0);
            const UINT64 input_value = 100 + static_cast<UINT64>(run);
            devices.context4->Signal(fence11.Get(), input_value);
            devices.context->Flush();
            queue->Wait(fence12.Get(), input_value);
        }
        allocator->Reset();
        list->Reset(allocator.Get(), nullptr);
        barrier(input12.Get(), D3D12_RESOURCE_STATE_COMMON,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        barrier(mid12.Get(), D3D12_RESOURCE_STATE_COMMON,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        barrier(motion12.Get(), D3D12_RESOURCE_STATE_COMMON,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        barrier(depth12.Get(), D3D12_RESOURCE_STATE_COMMON,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        parameters->Set("DLSSNR.Color", input12.Get());
        parameters->Set("DLSSNR.Output", mid12.Get());
        parameters->Set("DLSSNR.Reset", run == 0 ? 1 : 0);
        NVSDK_NGX_Result evaluated =
            snippet_evaluate(list.Get(), feature, parameters, nullptr);
        PrintResult("EvaluateFeature(18) pass1", evaluated);
        if (!NVSDK_NGX_SUCCEED(evaluated)) {
            return 14;
        }
        barrier(input12.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_COMMON);
        barrier(mid12.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        barrier(output12.Get(), D3D12_RESOURCE_STATE_COMMON,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        parameters->Set("DLSSNR.Color", mid12.Get());
        parameters->Set("DLSSNR.Output", output12.Get());
        parameters->Set("DLSSNR.Reset", 0);
        evaluated = snippet_evaluate(list.Get(), feature, parameters, nullptr);
        PrintResult("EvaluateFeature(18) pass2", evaluated);
        barrier(mid12.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_COMMON);
        barrier(motion12.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_COMMON);
        barrier(depth12.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_COMMON);
        barrier(output12.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_COMMON);
        list->Close();
        ID3D12CommandList* lists[] = {list.Get()};
        queue->ExecuteCommandLists(1, lists);
        const UINT64 value = static_cast<UINT64>(run + 1);
        queue->Signal(fence12.Get(), value);
        for (int spin = 0; spin < 5000 && fence12->GetCompletedValue() < value; ++spin) {
            ::Sleep(1);
        }
        devices.context4->Wait(fence11.Get(), value);
        if (!NVSDK_NGX_SUCCEED(evaluated)) {
            return 14;
        }
    }

    SaveBmp(devices.d11.Get(), devices.context.Get(), input11.Get(),
            "build/shots/dlssnr-input.bmp");
    SaveBmp(devices.d11.Get(), devices.context.Get(), output11.Get(),
            "build/shots/dlssnr-output.bmp");
    {
        // Inspect the alpha channel: some DLSSNR modes carry a residual/mask
        // there instead of changing RGB.
        D3D11_TEXTURE2D_DESC td{};
        output11->GetDesc(&td);
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        td.MiscFlags = 0;
        ComPtr<ID3D11Texture2D> staging;
        if (SUCCEEDED(devices.d11->CreateTexture2D(&td, nullptr, &staging))) {
            devices.context->CopyResource(staging.Get(), output11.Get());
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (SUCCEEDED(devices.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
                int amin = 255, amax = 0;
                long long asum = 0;
                long long count = 0;
                for (int y = 0; y < static_cast<int>(td.Height); y += 4) {
                    const auto* row = static_cast<const uint8_t*>(mapped.pData) +
                                      static_cast<size_t>(y) * mapped.RowPitch;
                    for (int x = 0; x < static_cast<int>(td.Width); x += 4) {
                        const int a = row[x * 4 + 3];
                        if (a < amin) amin = a;
                        if (a > amax) amax = a;
                        asum += a;
                        ++count;
                    }
                }
                std::printf("output alpha: min %d max %d avg %.1f\n", amin, amax,
                            count ? static_cast<double>(asum) / count : 0.0);
                devices.context->Unmap(staging.Get(), 0);
            }
        }
    }
    std::printf("wrote build/shots/dlssnr-input.bmp and dlssnr-output.bmp\n");

    if (feature && snippet_release_feature) {
        snippet_release_feature(feature);
    }
    return NVSDK_NGX_SUCCEED(create_result) ? 0 : 11;
}
