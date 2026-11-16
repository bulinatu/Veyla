// BetterRemotePlay — application entry point.

#include "app/app.h"
#include "platform/win32_window.h"

#include <chrono>
#include <thread>

int main() {
    brp::Win32Window window;
    if (!window.Create(L"BetterRemotePlay", 1600, 900)) {
        return 1;
    }

    brp::App app;
    if (!app.Init(window.hwnd(), window.width(), window.height())) {
        return 2;
    }

    const auto start = std::chrono::steady_clock::now();
    auto next_frame = start;
    const auto frame_budget = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(1.0 / 60.0));
    unsigned long long frames = 0;
    while (window.Pump()) {
        if (window.width() <= 0 || window.height() <= 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
            continue;
        }

        if (window.ConsumeResize()) {
            app.Resize(window.width(), window.height());
        }

        if (const int key = window.ConsumeKey(); key != 0) {
            std::printf("[key] vk=0x%02X\n", key);
            app.HandleKey(key);
            if (key == VK_ESCAPE && app.quit_requested()) {
                break;
            }
        }

        if (app.quit_requested()) {
            break;
        }

        const double t =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        app.Frame(t);
        ++frames;

        // Vsync is not a reliable frame limiter on virtual/remote displays, so
        // pace the UI loop explicitly. The stream path has its own pacing.
        next_frame += frame_budget;
        std::this_thread::sleep_until(next_frame);
    }

    std::printf("loop ended: frames=%llu quit=%d\n", frames, app.quit_requested() ? 1 : 0);
    return 0;
}
