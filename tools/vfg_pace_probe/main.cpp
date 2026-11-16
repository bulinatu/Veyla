// Simulates the app's VFG pull/present cadence without a console:
// a 60 Hz source producer, a 120 Hz presentation loop, and the same
// "pull a source frame only when the generated queue is low" policy used by
// App::Frame. Reports generated frames and presentation cadence.
//
// Usage: vfg_pace_probe [runtime_dir] [seconds]

#include "media/nv_vfx.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {
constexpr int kWidth = 1920;
constexpr int kHeight = 1080;

std::vector<uint8_t> MakeFrame(int shift) {
    std::vector<uint8_t> rgba(static_cast<size_t>(kWidth) * kHeight * 4);
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            const size_t i = (static_cast<size_t>(y) * kWidth + x) * 4;
            const int sx = (x + shift) % kWidth;
            rgba[i + 0] = static_cast<uint8_t>(sx * 255 / (kWidth - 1));
            rgba[i + 1] = static_cast<uint8_t>(y * 255 / (kHeight - 1));
            rgba[i + 2] = static_cast<uint8_t>(((sx / 64 + y / 64) & 1) ? 190 : 60);
            rgba[i + 3] = 255;
        }
    }
    return rgba;
}
}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const std::wstring runtime =
        argc > 1 ? std::wstring(argv[1], argv[1] + std::strlen(argv[1]))
                 : L"third_party\\nvidia-vfx\\runtime";
    const double seconds = argc > 2 ? std::atof(argv[2]) : 10.0;

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL level{};
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    const HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                         D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2,
                                         D3D11_SDK_VERSION, &device, &level, &context);
    if (FAILED(hr)) {
        std::fprintf(stderr, "D3D11CreateDevice failed: 0x%08lX\n",
                     static_cast<unsigned long>(hr));
        return 1;
    }

    brp::media::NvVfx vfx;
    std::string detail;
    if (!vfx.LoadRuntime(runtime, detail) ||
        !vfx.Initialize(device.Get(), context.Get(), kWidth, kHeight, 1, detail)) {
        std::fprintf(stderr, "VFG init failed: %s\n", detail.c_str());
        return 2;
    }

    const auto frame_a = MakeFrame(0);
    const auto frame_b = MakeFrame(7);

    std::deque<int> decoded;  // 0 = frame A, 1 = frame B
    std::deque<brp::media::NvVfx::Frame> generated;
    int next_source = 0;
    int presented = 0;
    int generated_total = 0;
    int max_generated_queue = 0;
    double generate_ms = 0.0;

    const int ticks = static_cast<int>(seconds * 120.0);
    for (int tick = 0; tick < ticks; ++tick) {
        // 60 Hz console: one decoded frame every second tick.
        if ((tick % 2) == 0) {
            decoded.push_back(next_source);
            next_source ^= 1;
            while (decoded.size() > 8) {
                decoded.pop_front();
            }
        }

        // App policy: only pull when the generated queue is low.
        if (generated.size() <= 2 && !decoded.empty()) {
            const auto& pixels = decoded.front() == 0 ? frame_a : frame_b;
            decoded.pop_front();
            std::vector<brp::media::NvVfx::Frame> sequence;
            if (vfx.PushFrame(pixels.data(), kWidth, kHeight, 2, sequence, detail)) {
                for (auto& frame : sequence) {
                    generated.push_back(std::move(frame));
                }
                const size_t cap = 6;
                while (generated.size() > cap) {
                    generated.pop_front();
                }
                generated_total += static_cast<int>(sequence.size()) - 1;
                generate_ms += vfx.last_generate_ms();
            } else {
                std::fprintf(stderr, "PushFrame failed: %s\n", detail.c_str());
                return 3;
            }
        }

        // 120 Hz presentation.
        if (!generated.empty()) {
            generated.pop_front();
            ++presented;
            max_generated_queue =
                std::max(max_generated_queue, static_cast<int>(generated.size()));
        }
    }

    const double elapsed = ticks / 120.0;
    std::printf("ticks %.0f, presented %d (%.1f/s), generated %d (%.1f/s), avg %.2f ms\n",
                static_cast<double>(ticks), presented, presented / elapsed, generated_total,
                generated_total / elapsed,
                generated_total > 0 ? generate_ms / generated_total : 0.0);
    std::printf("max generated queue %d\n", max_generated_queue);
    return 0;
}
