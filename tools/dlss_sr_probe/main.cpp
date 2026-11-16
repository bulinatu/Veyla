// DLSS Super Resolution probe on D3D11, using Magpie's captured-colour
// zero-depth/zero-motion contract. Loads the NGX core from the driver store
// and the official nvngx_dlss.dll supplied with Magpie.
//
// Usage: dlss_sr_probe [nvngx_dlss.dll] [out_w] [out_h] [preset] [quality]
//   preset: 11=K, 12=L, 13=M (default 11)
//   quality: 0 Balanced, 1 MaxQuality, 2 UltraQuality, 3 DLAA

#include <windows.h>

#include <d3d11.h>
#include <wrl/client.h>

#include <nvsdk_ngx.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

constexpr unsigned long long kAppId = 0x0876232Cull;
constexpr int kInW = 1920;
constexpr int kInH = 1080;

using InitFn = NVSDK_NGX_Result(NVSDK_CONV*)(unsigned long long, const wchar_t*, ID3D11Device*,
                                             NVSDK_NGX_Version);
using GetCapFn = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter**);
using CreateFn = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D11DeviceContext*, NVSDK_NGX_Feature,
                                                NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
using EvalFn = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D11DeviceContext*, NVSDK_NGX_Handle*,
                                              NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback);
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
    const std::wstring dll_path =
        argc > 1 ? std::wstring(argv[1], argv[1] + std::strlen(argv[1]))
                 : exe_dir + L"\\nvngx_dlss.dll";
    const int out_w = argc > 2 ? std::atoi(argv[2]) : 2560;
    const int out_h = argc > 3 ? std::atoi(argv[3]) : 1440;
    const int preset = argc > 4 ? std::atoi(argv[4]) : 11;
    const int quality = argc > 5 ? std::atoi(argv[5]) : 2;

    std::printf("exe %ls\ncore %ls\ndlss %ls\n", exe_dir.c_str(), core_dir.c_str(),
                dll_path.c_str());

    ::SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_USER_DIRS);
    ::AddDllDirectory(exe_dir.c_str());
    if (!core_dir.empty()) ::AddDllDirectory(core_dir.c_str());

    // Load the core before creating the graphics device.
    std::wstring core_path = core_dir + L"\\_nvngx.dll";
    if (::GetFileAttributesW(core_path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        core_path = core_dir + L"\\nvngx.dll";
    }
    std::printf("loading core %ls\n", core_path.c_str());
    HMODULE core = ::LoadLibraryExW(core_path.c_str(), nullptr, 0);
    if (!core) {
        std::fprintf(stderr, "core load failed %lu\n", ::GetLastError());
        return 1;
    }
    auto init = reinterpret_cast<InitFn>(::GetProcAddress(core, "NVSDK_NGX_D3D11_Init"));
    auto get_cap = reinterpret_cast<GetCapFn>(
        ::GetProcAddress(core, "NVSDK_NGX_D3D11_GetCapabilityParameters"));
    auto create_feature = reinterpret_cast<CreateFn>(
        ::GetProcAddress(core, "NVSDK_NGX_D3D11_CreateFeature"));
    auto evaluate = reinterpret_cast<EvalFn>(
        ::GetProcAddress(core, "NVSDK_NGX_D3D11_EvaluateFeature"));
    auto release_feature = reinterpret_cast<ReleaseFn>(
        ::GetProcAddress(core, "NVSDK_NGX_D3D11_ReleaseFeature"));
    if (!init || !get_cap || !create_feature || !evaluate) {
        std::fprintf(stderr, "core exports missing\n");
        return 2;
    }

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL level{};
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2,
                                   D3D11_SDK_VERSION, &device, &level, &context);
    if (FAILED(hr)) {
        std::fprintf(stderr, "D3D11CreateDevice 0x%08lX\n", static_cast<unsigned long>(hr));
        return 3;
    }

    PrintResult("NGX D3D11 Init", init(kAppId, exe_dir.c_str(), device.Get(),
                                       NVSDK_NGX_Version_API));
    NVSDK_NGX_Parameter* params = nullptr;
    PrintResult("GetCapabilityParameters", get_cap(&params));
    if (!params) return 4;

    params->Set(NVSDK_NGX_Parameter_Width, static_cast<unsigned>(kInW));
    params->Set(NVSDK_NGX_Parameter_Height, static_cast<unsigned>(kInH));
    params->Set(NVSDK_NGX_Parameter_OutWidth, static_cast<unsigned>(out_w));
    params->Set(NVSDK_NGX_Parameter_OutHeight, static_cast<unsigned>(out_h));
    static const int kQualities[] = {NVSDK_NGX_PerfQuality_Value_Balanced,
                                     NVSDK_NGX_PerfQuality_Value_MaxQuality,
                                     NVSDK_NGX_PerfQuality_Value_UltraQuality,
                                     NVSDK_NGX_PerfQuality_Value_DLAA};
    params->Set(NVSDK_NGX_Parameter_PerfQualityValue,
                kQualities[quality < 0 || quality > 3 ? 0 : quality]);
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
        params->Set(hint, static_cast<int>(preset));
    }

    D3D11_TEXTURE2D_DESC desc{};
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;

    const auto pixels = MakeFrame();
    ComPtr<ID3D11Texture2D> color;
    desc.Width = kInW; desc.Height = kInH; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA initial{pixels.data(), kInW * 4, kInW * 4 * kInH};
    hr = device->CreateTexture2D(&desc, &initial, &color);
    if (FAILED(hr)) { std::fprintf(stderr, "color texture failed\n"); return 5; }

    ComPtr<ID3D11Texture2D> output;
    desc.Width = out_w; desc.Height = out_h;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    hr = device->CreateTexture2D(&desc, nullptr, &output);
    if (FAILED(hr)) { std::fprintf(stderr, "output texture failed\n"); return 5; }

    // Zero motion vectors and depth: Magpie's captured-colour contract.
    ComPtr<ID3D11Texture2D> motion;
    ComPtr<ID3D11UnorderedAccessView> motion_uav;
    desc.Width = kInW; desc.Height = kInH; desc.Format = DXGI_FORMAT_R16G16_FLOAT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    hr = device->CreateTexture2D(&desc, nullptr, &motion);
    if (SUCCEEDED(hr)) hr = device->CreateUnorderedAccessView(motion.Get(), nullptr, &motion_uav);

    ComPtr<ID3D11Texture2D> depth;
    ComPtr<ID3D11UnorderedAccessView> depth_uav;
    desc.Format = DXGI_FORMAT_R32_FLOAT;
    hr = device->CreateTexture2D(&desc, nullptr, &depth);
    if (SUCCEEDED(hr)) hr = device->CreateUnorderedAccessView(depth.Get(), nullptr, &depth_uav);
    if (FAILED(hr)) { std::fprintf(stderr, "guide textures failed\n"); return 5; }

    const float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    context->ClearUnorderedAccessViewFloat(motion_uav.Get(), zero);
    context->ClearUnorderedAccessViewFloat(depth_uav.Get(), zero);
    context->Flush();

    NVSDK_NGX_Handle* feature = nullptr;
    PrintResult("CreateFeature(SR)", create_feature(context.Get(), NVSDK_NGX_Feature_SuperSampling,
                                                    params, &feature));
    if (!feature) return 6;

    double total = 0.0;
    constexpr int kRuns = 8;
    for (int run = 0; run < kRuns; ++run) {
        params->Set(NVSDK_NGX_Parameter_Color, static_cast<ID3D11Resource*>(color.Get()));
        params->Set(NVSDK_NGX_Parameter_Output, static_cast<ID3D11Resource*>(output.Get()));
        params->Set(NVSDK_NGX_Parameter_Depth, static_cast<ID3D11Resource*>(depth.Get()));
        params->Set(NVSDK_NGX_Parameter_MotionVectors,
                    static_cast<ID3D11Resource*>(motion.Get()));
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
        const NVSDK_NGX_Result result = evaluate(context.Get(), feature, params, nullptr);
        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - start)
                              .count();
        PrintResult("EvaluateFeature(SR)", result);
        total += ms;
        std::printf("run %d: %.2f ms\n", run, ms);
        if (!NVSDK_NGX_SUCCEED(result)) return 7;
    }
    std::printf("average %.2f ms\n", total / kRuns);

    SaveBmp(device.Get(), context.Get(), output.Get(), "build/shots/dlss-sr-output.bmp");
    std::printf("wrote build/shots/dlss-sr-output.bmp\n");
    if (release_feature) release_feature(feature);
    return 0;
}
