// Isolated NVIDIA VFG probe: renders two synthetic frames, generates the
// in-between frame, and writes all three as BMP so motion/colour can be judged
// without the whole app.

#include "media/nv_vfx.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

constexpr int kWidth = 1920;
constexpr int kHeight = 1080;

std::vector<uint8_t> MakeFrame(int bar_x, int shift) {
    std::vector<uint8_t> rgba(static_cast<size_t>(kWidth) * kHeight * 4);
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            const size_t i = (static_cast<size_t>(y) * kWidth + x) * 4;
            const int sx = (x + shift) % kWidth;
            uint8_t r = static_cast<uint8_t>(sx * 255 / (kWidth - 1));
            uint8_t g = static_cast<uint8_t>(y * 255 / (kHeight - 1));
            uint8_t b = static_cast<uint8_t>(((sx / 32 + y / 32) & 1) ? 200 : 40);
            if (std::abs(x - bar_x) < 18) {
                r = 245;
                g = 245;
                b = 250;
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

    brp::media::NvVfx vfx;
    std::string detail;
    if (!vfx.LoadRuntime(runtime, detail)) {
        std::fprintf(stderr, "runtime: %s\n", detail.c_str());
        return 2;
    }
    if (!vfx.Initialize(device.Get(), context.Get(), kWidth, kHeight, 1, detail)) {
        std::fprintf(stderr, "initialize: %s\n", detail.c_str());
        return 3;
    }

    std::vector<brp::media::NvVfx::Frame> first;
    std::vector<brp::media::NvVfx::Frame> second;
    const auto a = MakeFrame(200, 0);
    const auto b = MakeFrame(240, 0);

    if (!vfx.PushFrame(a.data(), kWidth, kHeight, 2, first, detail)) {
        std::fprintf(stderr, "push A: %s\n", detail.c_str());
        return 4;
    }
    if (!vfx.PushFrame(b.data(), kWidth, kHeight, 2, second, detail)) {
        std::fprintf(stderr, "push B: %s\n", detail.c_str());
        return 5;
    }

    std::printf("first sequence: %zu, second: %zu, generate %.2f ms\n", first.size(),
                second.size(), vfx.last_generate_ms());
    std::printf("status: %s\n", vfx.status().c_str());

    if (!first.empty() && first[0].texture) {
        SaveBmp(device.Get(), context.Get(), first[0].texture.Get(), "build/shots/vfx-a.bmp");
    }
    if (second.size() >= 2) {
        if (second[0].texture) {
            SaveBmp(device.Get(), context.Get(), second[0].texture.Get(),
                    "build/shots/vfx-generated.bmp");
        }
        if (second[1].texture) {
            SaveBmp(device.Get(), context.Get(), second[1].texture.Get(),
                    "build/shots/vfx-b.bmp");
        }
    }
    return 0;
}
