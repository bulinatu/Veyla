// Validates the NR -> DLSS SR chain: the NGX worker enhances a 1080p frame,
// then DLSS SR upscales that result to 1440p. Measures both stages.

#include "media/nv_dlss_sr.h"
#include "media/nv_dlssnr.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {
constexpr int kInW = 1920;
constexpr int kInH = 1080;

std::vector<uint8_t> MakeFrame(int shift) {
    std::vector<uint8_t> rgba(static_cast<size_t>(kInW) * kInH * 4);
    for (int y = 0; y < kInH; ++y) {
        for (int x = 0; x < kInW; ++x) {
            const size_t i = (static_cast<size_t>(y) * kInW + x) * 4;
            const int sx = (x + shift) % kInW;
            uint8_t r = static_cast<uint8_t>(sx * 255 / (kInW - 1));
            uint8_t g = static_cast<uint8_t>(y * 255 / (kInH - 1));
            uint8_t b = static_cast<uint8_t>(((sx / 64 + y / 64) & 1) ? 190 : 60);
            if (((sx + y * 2) % 37) == 0) { r = 250; g = 250; b = 250; }
            if (sx > kInW * 3 / 4 && y > 80 && y < 300) {
                const bool ink = ((sx / 6) % 4 == 0) || ((y / 8) % 5 == 0);
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
    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ; td.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&td, nullptr, &staging))) return false;
    context->CopyResource(staging.Get(), texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;
    const uint32_t width = td.Width, height = td.Height;
    const uint32_t row = width * 4, image_bytes = row * height;
    std::FILE* file = nullptr;
    if (fopen_s(&file, path, "wb") != 0 || !file) { context->Unmap(staging.Get(), 0); return false; }
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
            line[x * 4 + 0] = in[x * 4 + 2]; line[x * 4 + 1] = in[x * 4 + 1];
            line[x * 4 + 2] = in[x * 4 + 0]; line[x * 4 + 3] = 255;
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
    const std::wstring nr_root =
        argc > 1 ? std::wstring(argv[1], argv[1] + std::strlen(argv[1]))
                 : L"C:\\Users\\Cosmin\\Desktop\\dlss5overlay\\neuralscreen-v2.6.0-full";
    const std::wstring sr_runtime =
        argc > 2 ? std::wstring(argv[2], argv[2] + std::strlen(argv[2]))
                 : L"third_party\\nvidia-ngx\\runtime";

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL level{};
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                 D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2,
                                 D3D11_SDK_VERSION, &device, &level, &context))) {
        std::fprintf(stderr, "D3D11 device failed\n");
        return 1;
    }

    brp::media::NvDlssnr::Options options;
    brp::media::NvDlssnr nr;
    brp::media::NvDlssSr sr;
    std::string detail;
    if (!nr.Start(nr_root + L"\\native\\nvngx.dll", nr_root + L"\\native", kInW, kInH,
                  options, detail)) {
        std::fprintf(stderr, "nr start: %s\n", detail.c_str());
        return 2;
    }
    if (!sr.LoadRuntime(sr_runtime, detail)) {
        std::fprintf(stderr, "sr runtime: %s\n", detail.c_str());
        return 3;
    }
    const int preset = argc > 3 ? std::atoi(argv[3]) : 11;
    const int quality = argc > 4 ? std::atoi(argv[4]) : 0;
    if (!sr.Initialize(device.Get(), context.Get(), kInW, kInH, 2560, 1440, preset, quality,
                       detail)) {
        std::fprintf(stderr, "sr init: %s\n", detail.c_str());
        return 4;
    }

    brp::media::NvDlssSr::Frame out;
    double nr_total = 0.0;
    double sr_total = 0.0;
    const int frames = 12;
    for (int i = 0; i < frames; ++i) {
        auto frame = MakeFrame(i * 3);
        if (!nr.Process(frame.data(), kInW, kInH, frame, detail)) {
            std::fprintf(stderr, "nr %d: %s\n", i, detail.c_str());
            return 5;
        }
        nr_total += nr.last_ms();
        if (!sr.Process(frame.data(), kInW, kInH, out, detail)) {
            std::fprintf(stderr, "sr %d: %s\n", i, detail.c_str());
            return 6;
        }
        sr_total += sr.last_ms();
        std::printf("frame %d: NR %.2f ms + SR %.2f ms = %.2f ms\n", i, nr.last_ms(),
                    sr.last_ms(), nr.last_ms() + sr.last_ms());
    }
    std::printf("average: NR %.2f ms + SR %.2f ms = %.2f ms (%.1f fps ceiling)\n",
                nr_total / frames, sr_total / frames, (nr_total + sr_total) / frames,
                1000.0 / ((nr_total + sr_total) / frames));
    if (out.texture) {
        SaveBmp(device.Get(), context.Get(), out.texture.Get(), "build/shots/nr-sr-chain.bmp");
        std::printf("wrote build/shots/nr-sr-chain.bmp\n");
    }
    sr.Shutdown();
    nr.Stop();
    return 0;
}
