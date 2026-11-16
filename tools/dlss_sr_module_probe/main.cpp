// Validates media::NvDlssSr (D3D12 DLSS SR) with a D3D11 device.

#include "media/nv_dlss_sr.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {
constexpr int kInW = 1920;
constexpr int kInH = 1080;

std::vector<uint8_t> MakeFrame() {
    std::vector<uint8_t> rgba(static_cast<size_t>(kInW) * kInH * 4);
    for (int y = 0; y < kInH; ++y) {
        for (int x = 0; x < kInW; ++x) {
            const size_t i = (static_cast<size_t>(y) * kInW + x) * 4;
            uint8_t r = static_cast<uint8_t>(x * 255 / (kInW - 1));
            uint8_t g = static_cast<uint8_t>(y * 255 / (kInH - 1));
            uint8_t b = static_cast<uint8_t>(((x / 64 + y / 64) & 1) ? 190 : 60);
            if (((x + y * 2) % 37) == 0) { r = 250; g = 250; b = 250; }
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
    const std::wstring runtime =
        argc > 1 ? std::wstring(argv[1], argv[1] + std::strlen(argv[1]))
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
    brp::media::NvDlssSr sr;
    std::string detail;
    if (!sr.LoadRuntime(runtime, detail)) {
        std::fprintf(stderr, "runtime: %s\n", detail.c_str());
        return 2;
    }
    if (!sr.Initialize(device.Get(), context.Get(), kInW, kInH, 2560, 1440, 11, 0, detail)) {
        std::fprintf(stderr, "init: %s\n", detail.c_str());
        return 3;
    }
    const auto pixels = MakeFrame();
    brp::media::NvDlssSr::Frame out;
    double total = 0.0;
    for (int i = 0; i < 8; ++i) {
        if (!sr.Process(pixels.data(), kInW, kInH, out, detail)) {
            std::fprintf(stderr, "process: %s\n", detail.c_str());
            return 4;
        }
        total += sr.last_ms();
        std::printf("run %d: %.2f ms\n", i, sr.last_ms());
    }
    std::printf("average %.2f ms\n", total / 8.0);
    if (out.texture) {
        SaveBmp(device.Get(), context.Get(), out.texture.Get(), "build/shots/dlss-sr-module.bmp");
        std::printf("wrote build/shots/dlss-sr-module.bmp\n");
    }
    return 0;
}
