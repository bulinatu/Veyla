// Validates media::NvDlssnr (NeuralScreen NGX NR worker) on synthetic frames.

#include "media/nv_dlssnr.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

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

bool SaveBmp(const std::vector<uint8_t>& rgba, const char* path) {
    const uint32_t width = kInW, height = kInH;
    const uint32_t row = width * 4, image_bytes = row * height;
    std::FILE* file = nullptr;
    if (fopen_s(&file, path, "wb") != 0 || !file) return false;
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
    for (int y = height - 1; y >= 0; --y) {
        const uint8_t* in = rgba.data() + static_cast<size_t>(y) * row;
        for (uint32_t x = 0; x < width; ++x) {
            line[x * 4 + 0] = in[x * 4 + 2]; line[x * 4 + 1] = in[x * 4 + 1];
            line[x * 4 + 2] = in[x * 4 + 0]; line[x * 4 + 3] = 255;
        }
        std::fwrite(line.data(), 1, row, file);
    }
    std::fclose(file);
    return true;
}
}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const std::wstring root =
        argc > 1 ? std::wstring(argv[1], argv[1] + std::strlen(argv[1]))
                 : L"C:\\Users\\Cosmin\\Desktop\\dlss5overlay\\neuralscreen-v2.6.0-full";
    const std::wstring worker = root + L"\\native\\nvngx.dll";
    const std::wstring working = root + L"\\native";

    brp::media::NvDlssnr nr;
    std::string detail;
    if (!nr.Start(worker, working, kInW, kInH, 2, 1.0f, 0.5f, 0.75f, false, false, 1.0f,
                  detail)) {
        std::fprintf(stderr, "start: %s\n", detail.c_str());
        return 1;
    }
    std::printf("NR ready\n");
    std::vector<uint8_t> out;
    double total = 0.0;
    for (int i = 0; i < 20; ++i) {
        const auto frame = MakeFrame(i * 3);
        if (!nr.Process(frame.data(), kInW, kInH, out, detail)) {
            std::fprintf(stderr, "process: %s\n", detail.c_str());
            return 2;
        }
        total += nr.last_ms();
        std::printf("run %d: %.2f ms\n", i, nr.last_ms());
    }
    std::printf("average %.2f ms\n", total / 20.0);
    SaveBmp(out, "build/shots/nr-module.bmp");
    std::printf("wrote build/shots/nr-module.bmp\n");
    nr.Stop();
    return 0;
}
