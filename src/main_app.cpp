// BetterRemotePlay - application entry point.

#include "app/app.h"
#include "platform/win32_window.h"

#include <mmsystem.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

int main() {
    // The presentation thread must not lose its vblank slot to background
    // work. Above-normal is enough to win against ordinary helpers without
    // starving audio, decode or the OS (which run at normal).
    ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

    // The multimedia timer period also governs how precisely a plain sleep can
    // land. The high-resolution waitable timer below is preferred, but every
    // measurement / wait helper in the streaming path benefits from 1 ms
    // granularity being active for the lifetime of the process.
    ::timeBeginPeriod(1);

    brp::Win32Window window;
    if (!window.Create(L"BetterRemotePlay", 1600, 900)) {
        ::timeEndPeriod(1);
        return 1;
    }

    brp::App app;
    if (!app.Init(&window)) {
        ::timeEndPeriod(1);
        return 2;
    }

    const auto start = std::chrono::steady_clock::now();
    auto next_frame = start;
    const bool loop_probe = std::getenv("BRP_LOOP_PROBE") != nullptr;
    const bool frame_probe = std::getenv("BRP_FRAME_PROBE") != nullptr;
    double bench_seconds = 0.0;
    if (const char* bench = std::getenv("BRP_BENCH_SECONDS")) {
        bench_seconds = std::atof(bench);
    }
    int probe_frames = 0;
    auto probe_last = start;
    auto frame_probe_last = start;
    std::vector<double> interval_ms;
    std::vector<double> work_ms;
    interval_ms.reserve(4096);
    work_ms.reserve(4096);

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

        // The swapchain waitable is the real frame clock: it fires when the
        // previous present is retired, i.e. on the display's vblank. Waiting
        // here (instead of in Present) removes the phase drift between our
        // software timer and the monitor, which was showing up as an
        // occasional 8.3 ms frame at 240 Hz.
        const bool swapchain_clock = app.present_waitable() != nullptr;
        if (swapchain_clock) {
            const int hz = std::max(24, app.present_hz());
            const DWORD timeout =
                static_cast<DWORD>(std::clamp(2000 / hz, 8, 40));
            app.WaitForPresent(timeout);
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
        if (bench_seconds > 0.0 && t >= bench_seconds) {
            break;
        }
        app.Frame(t, pointer);

        if (loop_probe) {
            ++probe_frames;
            const auto probe_now = std::chrono::steady_clock::now();
            const double interval =
                std::chrono::duration<double>(probe_now - probe_last).count();
            if (interval >= 2.0) {
                std::fprintf(stderr, "[loop] %.1f fps (target %d Hz)\n",
                             probe_frames / interval, app.present_hz());
                probe_frames = 0;
                probe_last = probe_now;
            }
        }

        if (frame_probe) {
            const auto probe_now = std::chrono::steady_clock::now();
            const double ms =
                std::chrono::duration<double, std::milli>(probe_now - frame_probe_last).count();
            interval_ms.push_back(ms);
            work_ms.push_back(app.frame_work_ms());
            if (interval_ms.size() >= 480) {
                const int hz = std::max(24, app.present_hz());
                const double budget_ms = 1000.0 / static_cast<double>(hz);
                std::vector<double> sorted(interval_ms);
                std::sort(sorted.begin(), sorted.end());
                std::vector<double> sorted_work(work_ms);
                std::sort(sorted_work.begin(), sorted_work.end());
                const auto pick = [](const std::vector<double>& v, double q) {
                    return v[std::min(v.size() - 1, static_cast<size_t>(q * (v.size() - 1)))];
                };
                int late = 0;
                int stalls = 0;
                for (double sample : interval_ms) {
                    if (sample > budget_ms * 1.5) {
                        ++late;
                    }
                    if (sample > budget_ms * 2.5) {
                        ++stalls;
                    }
                }
                std::fprintf(stderr,
                             "[frame] %d Hz  interval p50 %.2f  p99 %.2f  max %.2f ms  |  "
                             "work p50 %.2f  p99 %.2f  max %.2f ms  |  late %d  stalls %d "
                             "(of %zu)\n",
                             hz, pick(sorted, 0.50), pick(sorted, 0.99), sorted.back(),
                             pick(sorted_work, 0.50), pick(sorted_work, 0.99),
                             sorted_work.back(), late, stalls, interval_ms.size());
                interval_ms.clear();
                work_ms.clear();
            }
            frame_probe_last = probe_now;
        }

        // Without a swapchain waitable, pace the loop explicitly at the
        // monitor's refresh rate. With one, Present(1) is the limiter and a
        // second timer only reintroduces jitter.
        if (!swapchain_clock) {
            const auto budget = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(1.0 / app.present_hz()));
            next_frame += budget;
            const auto now = std::chrono::steady_clock::now();
            if (next_frame < now - budget) {
                next_frame = now + budget;
            }
            wait_until(next_frame);
        }
    }

    if (timer) {
        ::CloseHandle(timer);
    }
    ::timeEndPeriod(1);

    return 0;
}
