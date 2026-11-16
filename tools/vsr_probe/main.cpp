// Isolated RTX Video Super Resolution probe: builds a synthetic 1080p frame
// with sharp detail, upscales it with NVIDIA VSR, and writes input + output as
// BMP plus timings. Usage: vsr_probe [runtime_dir] [quality] [out_w] [out_h]

#include "media/nv_vsr.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

constexpr int kWidth = 1920;
constexpr int kHeight = 1080;

std::vector<uint8_t> MakeFrame() {
    std::vector<uint8_t> rgba(static_cast<size_t>(kWidth) * kHeight * 4);
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            const size_t i = (static_cast<size_t>(y) * kWidth + x) * 4;
            // Gradient background.
            uint8_t r = static_cast<uint8_t>(x * 255 / (kWidth - 1));
            uint8_t g = static_cast<uint8_t>(y * 255 / (kHeight - 1));
            uint8_t b = static_cast<uint8_t>(((x / 64 + y / 64) & 1) ? 190 : 60);
            // Thin diagonal lines: the classic upscaler stress pattern.
            if (((x + y * 2) % 37) == 0) {
                r = 250;
                g = 250;
                b = 250;
            }
            // Concentric circles for aliasing.
            const float dx = static_cast<float>(x - kWidth / 2);
            const float dy = static_cast<float>(y - kHeight / 2);
            const float radius = std::sqrt(dx * dx + dy * dy);
            const int ring = static_cast<int>(radius) % 23;
            if (ring == 0) {
                r = 20;
                g = 20;
                b = 20;
            }
            // High-contrast text-like blocks in the corner.
            if (x > 1480 && x < 1840 && y > 80 && y < 300) {
                const bool ink = ((x / 6) % 4 == 0) || ((y / 8) % 5 == 0);
                r = ink ? 245 : 10;
                g = ink ? 245 : 10;
                b = ink ? 245 : 10;
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
    const std::wstring runtime =
        argc > 1 ? std::wstring(argv[1], argv[1] + std::strlen(argv[1]))
                 : L"third_party\\nvidia-vfx\\runtime";
    const int quality = argc > 2 ? std::atoi(argv[2]) : 3;
    const int out_w = argc > 3 ? std::atoi(argv[3]) : 2560;
    const int out_h = argc > 4 ? std::atoi(argv[4]) : 1440;

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL level{};
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    const HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                         levels, 2, D3D11_SDK_VERSION, &device, &level, &context);
    if (FAILED(hr)) {
        std::fprintf(stderr, "D3D11CreateDevice failed: 0x%08lX\n",
                     static_cast<unsigned long>(hr));
        return 1;
    }

    brp::media::NvVsr vsr;
    std::string detail;
    if (!vsr.LoadRuntime(runtime, detail)) {
        std::fprintf(stderr, "runtime: %s\n", detail.c_str());
        return 2;
    }
    if (!vsr.Initialize(device.Get(), context.Get(), kWidth, kHeight, out_w, out_h, quality, 1.0f,
                        detail)) {
        std::fprintf(stderr, "initialize: %s\n", detail.c_str());
        return 3;
    }

    const auto input = MakeFrame();
    brp::media::NvVsr::Frame out;
    double total = 0.0;
    constexpr int kRuns = 10;
    for (int i = 0; i < kRuns; ++i) {
        if (!vsr.Process(input.data(), kWidth, kHeight, out, detail)) {
            std::fprintf(stderr, "process: %s\n", detail.c_str());
            return 4;
        }
        total += vsr.last_ms();
        std::printf("run %d: %.2f ms\n", i, vsr.last_ms());
    }
    std::printf("average %.2f ms, status: %s\n", total / kRuns, vsr.status().c_str());

    if (out.texture) {
        SaveBmp(device.Get(), context.Get(), out.texture.Get(), "build/shots/vsr-output.bmp");
        std::printf("wrote build/shots/vsr-output.bmp\n");
    }

    // Save the input for side-by-side comparison.
    D3D11_TEXTURE2D_DESC td{};
    vsr.Initialize(device.Get(), context.Get(), kWidth, kHeight, out_w, out_h, quality, 1.0f,
                   detail);
    ComPtr<ID3D11Texture2D> input_texture;
    td.Width = kWidth;
    td.Height = kHeight;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (SUCCEEDED(device->CreateTexture2D(&td, nullptr, &input_texture))) {
        context->UpdateSubresource(input_texture.Get(), 0, nullptr, input.data(), kWidth * 4, 0);
        SaveBmp(device.Get(), context.Get(), input_texture.Get(), "build/shots/vsr-input.bmp");
        std::printf("wrote build/shots/vsr-input.bmp\n");
    }
    return 0;
}
