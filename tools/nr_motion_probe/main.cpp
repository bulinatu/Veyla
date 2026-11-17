// Feeds a moving scene to media::NvDlssnr and saves frames, to see what the
// worker does with the motion field. The client currently sends zeros; a
// moving high-contrast bar makes any temporal smear obvious.

#include "media/nv_dlssnr.h"

#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {
constexpr int kW = 1920;
constexpr int kH = 1080;
constexpr int kBarW = 56;
constexpr int kStepPx = 12;

std::vector<uint8_t> Scene(int frame) {
    std::vector<uint8_t> rgba(static_cast<size_t>(kW) * kH * 4);
    const int bar_x = 120 + frame * kStepPx;
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            const size_t i = (static_cast<size_t>(y) * kW + x) * 4;
            // Fine static texture, so detail loss and smear both show.
            const int grain = ((x * 7 + y * 13) % 23) * 3;
            uint8_t r = static_cast<uint8_t>(28 + grain);
            uint8_t g = static_cast<uint8_t>(30 + grain);
            uint8_t b = static_cast<uint8_t>(36 + grain);
            // Static high-frequency "UI text" at the top right.
            if (x > 1200 && x < 1800 && y > 60 && y < 200) {
                const bool ink = ((y / 9) % 3 == 0) && ((x / 7) % 3 != 0);
                r = ink ? 235 : 18;
                g = ink ? 235 : 18;
                b = ink ? 235 : 18;
            }
            // The moving bar: a hard edge with a soft inner gradient.
            const int dx = x - bar_x;
            if (dx >= 0 && dx < kBarW) {
                const int level = 200 - dx * 3;
                r = static_cast<uint8_t>(level);
                g = static_cast<uint8_t>(level);
                b = static_cast<uint8_t>(level + 20);
            }
            rgba[i + 0] = r;
            rgba[i + 1] = g;
            rgba[i + 2] = b;
            rgba[i + 3] = 255;
        }
    }
    return rgba;
}

bool SaveBmp(const std::vector<uint8_t>& rgba, const char* path) {
    const uint32_t width = kW, height = kH;
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

    brp::media::NvDlssnr::Options options;
    if (argc > 2) {
        options.work_scale = std::atoi(argv[2]) / 100.0f;
    }
    if (argc > 3) {
        options.passes = std::atoi(argv[3]);
    }

    brp::media::NvDlssnr nr;
    std::string detail;
    if (!nr.Start(worker, working, kW, kH, options, detail)) {
        std::fprintf(stderr, "start: %s\n", detail.c_str());
        return 1;
    }
    std::printf("NR ready: work %dx%d, %d pass\n", nr.work_width(), nr.work_height(),
                options.passes);
    for (int i = 0; i < 10; ++i) {
        auto frame = Scene(i);
        if (!nr.Process(frame.data(), kW, kH, frame, detail)) {
            std::fprintf(stderr, "process %d: %s\n", i, detail.c_str());
            return 2;
        }
        std::printf("phase1 frame %d %.2f ms\n", i, nr.last_ms());
        if (i == 1 || i == 9) {
            char path[128];
            std::snprintf(path, sizeof(path), "build/shots/nr-motion-%s-%02d.bmp",
                          options.work_scale >= 1.0f ? "full" : "boost", i);
            SaveBmp(frame, path);
        }
    }

    // Live retune: cross to the other side of the quality ladder mid-run.
    brp::media::NvDlssnr::Options next = options;
    if (options.work_scale >= 1.0f) {
        next.work_scale = 0.70f;
        next.passes = 2;
    } else {
        next.work_scale = 1.0f;
        next.passes = 1;
    }
    const double t0 = static_cast<double>(::GetTickCount64());
    if (!nr.Reconfigure(next, detail)) {
        std::fprintf(stderr, "reconfigure: %s\n", detail.c_str());
        return 3;
    }
    std::printf("reconfigured to work %dx%d, %d pass in %.0f ms\n", nr.work_width(),
                nr.work_height(), next.passes,
                static_cast<double>(::GetTickCount64()) - t0);
    for (int i = 10; i < 20; ++i) {
        auto frame = Scene(i);
        if (!nr.Process(frame.data(), kW, kH, frame, detail)) {
            std::fprintf(stderr, "process %d: %s\n", i, detail.c_str());
            return 4;
        }
        std::printf("phase2 frame %d %.2f ms\n", i, nr.last_ms());
        if (i == 11 || i == 19) {
            char path[128];
            std::snprintf(path, sizeof(path), "build/shots/nr-motion-%s-%02d.bmp",
                          next.work_scale >= 1.0f ? "full" : "boost", i);
            SaveBmp(frame, path);
        }
    }
    nr.Stop();
    return 0;
}
