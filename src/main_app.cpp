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
    if (!app.Init(&window)) {
        return 2;
    }

    const auto start = std::chrono::steady_clock::now();
    auto next_frame = start;

    // A normal sleep_until() quantises to the 15.6 ms system tick, which would
    // cap a 120/180/240 Hz presentation loop at ~64 Hz. Use the high-resolution
    // waitable timer (Windows 10 1803+), with a plain sleep fallback.
    HANDLE timer = ::CreateWaitableTimerExW(nullptr, nullptr,
                                            CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                            TIMER_ALL_ACCESS);
    auto wait_until = [&](std::chrono::steady_clock::time_point deadline) {
        const auto now = std::chrono::steady_clock::now();
        if (deadline <= now) {
            return;
        }
        if (!timer) {
            std::this_thread::sleep_until(deadline);
            return;
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now);
        LARGE_INTEGER due;
        due.QuadPart = -static_cast<LONGLONG>(std::max<long long>(remaining.count() / 100, 1));
        if (::SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
            ::WaitForSingleObject(timer, INFINITE);
        } else {
            std::this_thread::sleep_until(deadline);
        }
    };

    while (window.Pump()) {
        if (window.width() <= 0 || window.height() <= 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
            continue;
        }

        if (window.ConsumeResize()) {
            app.Resize(window.width(), window.height());
        }

        if (const int key = window.ConsumeKey(); key != 0) {
            app.HandleKey(key);
            if (key == VK_ESCAPE && app.quit_requested()) {
                break;
            }
        }

        if (app.quit_requested()) {
            break;
        }

        brp::PointerState pointer;
        pointer.x = window.mouse().x;
        pointer.y = window.mouse().y;
        pointer.down = window.mouse().down;
        pointer.clicked = window.ConsumeClick();
        pointer.wheel = window.ConsumeWheel();

        const double t =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        app.Frame(t, pointer);

        // Vsync is not a reliable frame limiter on virtual/remote displays, so
        // pace the UI loop explicitly. Frame generation raises the target to
        // 120/180/240 Hz.
        const auto budget = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(1.0 / app.present_hz()));
        next_frame += budget;
        const auto now = std::chrono::steady_clock::now();
        if (next_frame < now - budget) {
            next_frame = now + budget;
        }
        wait_until(next_frame);
    }

    if (timer) {
        ::CloseHandle(timer);
    }

    return 0;
}
