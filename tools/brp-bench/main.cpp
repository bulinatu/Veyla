// brp-bench — proves the presentation + post-process path on this machine
// without a PS5 attached. It feeds a synthetic 1080p "decoded frame" through
// the same contract the real decoder will use, then reports GPU/CPU cost.
//
// Usage:
//   brp-bench [--width 2560] [--height 1440] [--src-width 1920] [--src-height 1080]
//             [--mode 0|1|2] [--vsync] [--frames N] [--limit FPS]
//
// Keys: 1 = bilinear, 2 = catmull-rom, 3 = catmull-rom + sharpen, ESC = quit.

#include "media/pipeline.h"
#include "platform/win32_window.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

namespace {

int ArgInt(char** argv, int argc, const char* name, int fallback) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], name) == 0) {
            return std::atoi(argv[i + 1]);
        }
    }
    return fallback;
}

bool ArgFlag(char** argv, int argc, const char* name) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], name) == 0) {
            return true;
        }
    }
    return false;
}

std::wstring Widen(const std::string& s) {
    if (s.empty()) {
        return {};
    }
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

}  // namespace

int main(int argc, char** argv) {
    brp::PresenterConfig cfg{};
    cfg.output_width = ArgInt(argv, argc, "--width", 2560);
    cfg.output_height = ArgInt(argv, argc, "--height", 1440);
    cfg.source_width = ArgInt(argv, argc, "--src-width", 1920);
    cfg.source_height = ArgInt(argv, argc, "--src-height", 1080);
    cfg.upscale_mode = ArgInt(argv, argc, "--mode", 2);
    cfg.vsync = ArgFlag(argv, argc, "--vsync");

    const int max_frames = ArgInt(argv, argc, "--frames", 0);
    const int fps_limit = ArgInt(argv, argc, "--limit", 0);
    const char* shot_path = nullptr;
    int shot_frame = 60;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--shot") == 0) {
            shot_path = argv[i + 1];
        } else if (std::strcmp(argv[i], "--shot-frame") == 0) {
            shot_frame = std::atoi(argv[i + 1]);
        }
    }

    brp::Win32Window window;
    if (!window.Create(L"BetterRemotePlay - pipeline bench", 1280, 720)) {
        std::fprintf(stderr, "failed to create window\n");
        return 1;
    }

    brp::Presenter presenter;
    if (!presenter.Init(window.hwnd(), cfg)) {
        std::fprintf(stderr, "failed to init presenter\n");
        return 2;
    }

    // The WM_SIZE generated at window creation describes the default 1280x720
    // client area, not the requested output resolution. Swallow it: the
    // swapchain stays at the requested size and DXGI stretches it into the
    // window. Later resizes are real user resizes and are honored.
    window.ConsumeResize();

    const auto start = std::chrono::steady_clock::now();
    auto last_title = start;
    auto next_frame = start;
    const auto frame_budget = fps_limit > 0
                                  ? std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                        std::chrono::duration<double>(1.0 / fps_limit))
                                  : std::chrono::steady_clock::duration::zero();

    unsigned long long frames = 0;
    while (window.Pump()) {
        if (window.width() <= 0 || window.height() <= 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        if (window.ConsumeResize()) {
            presenter.Resize(window.width(), window.height());
        }

        const int key = window.ConsumeKey();
        if (key == VK_ESCAPE) {
            break;
        }
        if (key >= '1' && key <= '3') {
            presenter.SetUpscaleMode(key - '1');
        }

        const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        presenter.Render(t);
        if (shot_path && frames + 1 == static_cast<unsigned long long>(shot_frame)) {
            if (presenter.CaptureToBmp(shot_path)) {
                std::printf("captured %s\n", shot_path);
            }
        }
        presenter.Present();
        ++frames;

        if (fps_limit > 0) {
            next_frame += frame_budget;
            std::this_thread::sleep_until(next_frame);
        }

        const auto now = std::chrono::steady_clock::now();
        if (now - last_title > std::chrono::milliseconds(250)) {
            last_title = now;
            ::SetWindowTextW(window.hwnd(), Widen(presenter.Summary()).c_str());
        }

        if (max_frames > 0 && frames >= static_cast<unsigned long long>(max_frames)) {
            break;
        }
    }

    presenter.PrintFinalSummary();
    return 0;
}
