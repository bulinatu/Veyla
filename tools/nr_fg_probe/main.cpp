// Validates the worker's own DLSS Frame Generation presenter (WNDO + FG).
// Run it, watch build/Release/nr-worker.log for [fg] lines.

#include "media/nv_dlssnr.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {
constexpr int kW = 1920;
constexpr int kH = 1080;

std::vector<uint8_t> Scene(int frame) {
    std::vector<uint8_t> rgba(static_cast<size_t>(kW) * kH * 4);
    const int bar = 100 + frame * 10;
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            const size_t i = (static_cast<size_t>(y) * kW + x) * 4;
            const int grain = ((x * 7 + y * 13) % 23) * 3;
            uint8_t r = static_cast<uint8_t>(30 + grain);
            uint8_t g = static_cast<uint8_t>(34 + grain);
            uint8_t b = static_cast<uint8_t>(40 + grain);
            if (x > 1300 && x < 1800 && y > 80 && y < 260) {
                const bool ink = ((y / 9) % 3 == 0) && ((x / 7) % 3 != 0);
                r = ink ? 235 : 20;
                g = ink ? 235 : 20;
                b = ink ? 235 : 20;
            }
            if (x >= bar && x < bar + 60) {
                r = 220;
                g = 200;
                b = 240;
            }
            rgba[i + 0] = r;
            rgba[i + 1] = g;
            rgba[i + 2] = b;
            rgba[i + 3] = 255;
        }
    }
    return rgba;
}
}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const std::wstring root =
        argc > 1 ? std::wstring(argv[1], argv[1] + std::strlen(argv[1]))
                 : L"C:\\Users\\Cosmin\\Desktop\\dlss5overlay\\neuralscreen-v2.6.0-full";
    const int multiplier = argc > 2 ? std::atoi(argv[2]) : 2;
    const int sleep_ms = argc > 3 ? std::atoi(argv[3]) : 16;
    const int frames = argc > 4 ? std::atoi(argv[4]) : 120;

    brp::media::NvDlssnr::Options options;
    options.work_scale = 1.0f;
    brp::media::NvDlssnr nr;
    std::string detail;
    if (!nr.Start(root + L"\\native\\nvngx.dll", root + L"\\native", kW, kH, options,
                  detail)) {
        std::fprintf(stderr, "start: %s\n", detail.c_str());
        return 1;
    }
    if (!nr.EnablePresent(kW, kH, detail)) {
        std::fprintf(stderr, "WNDO refused: %s\n", detail.c_str());
        return 2;
    }
    std::printf("present window requested; worker pid %lu\n",
                static_cast<unsigned long>(nr.worker_pid()));
    nr.SetFrameGeneration(multiplier);

    std::vector<uint8_t> out;
    const ULONGLONG started = ::GetTickCount64();
    for (int i = 0; i < frames; ++i) {
        auto frame = Scene(i);
        if (!nr.Process(frame.data(), kW, kH, out, detail)) {
            std::fprintf(stderr, "process %d: %s\n", i, detail.c_str());
            return 3;
        }
        if ((i % 20) == 0) {
            std::printf("frame %d submitted\n", i);
        }
        if (sleep_ms > 0) {
            ::Sleep(sleep_ms);
        }
    }
    const double seconds = (::GetTickCount64() - started) / 1000.0;
    std::printf("fed %d frames in %.2f s = %.1f input/s\n", frames, seconds,
                frames / seconds);
    std::printf("done, leaving the window up for 3 s\n");
    ::Sleep(3000);
    nr.Stop();
    return 0;
}
