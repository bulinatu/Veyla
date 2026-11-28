// Standalone smoke test for the DX12 video surface. Creates a window, draws a
// moving pattern for a few seconds and exits; no input, no clicks.

#include "media/dx12_video.h"

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

// Agility SDK opt-in: load D3D12Core.dll from .\D3D12\ so shader model 6.6+
// and the debug layer are available regardless of the inbox runtime.
extern "C" {
__declspec(dllexport) extern const UINT D3D12SDKVersion = 620;
__declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\";
}

namespace {

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_DESTROY) {
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(hwnd, msg, wparam, lparam);
}

std::vector<std::uint8_t> Frame(int width, int height, int tick) {
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(width) * height * 4);
    const int bar = (tick * 8) % (width + 200) - 100;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * width + x) * 4;
            std::uint8_t r = static_cast<std::uint8_t>(20 + (x * 120 / width));
            std::uint8_t g = static_cast<std::uint8_t>(24 + (y * 120 / height));
            std::uint8_t b = 48;
            if (x >= bar && x < bar + 140) {
                r = 190;
                g = 160;
                b = 255;
            }
            if (((x / 40) + (y / 40) + tick / 20) % 2 == 0) {
                r = static_cast<std::uint8_t>(r + 20);
                g = static_cast<std::uint8_t>(g + 20);
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

int main() {
    ::SetProcessDPIAware();
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = L"VeylaDx12Probe";
    if (!::RegisterClassExW(&wc)) {
        std::fprintf(stderr, "RegisterClassEx failed\n");
        return 1;
    }
    RECT rect{0, 0, 1280, 720};
    ::AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = ::CreateWindowExW(0, wc.lpszClassName, L"Veyla DX12 probe",
                                  WS_OVERLAPPEDWINDOW, 80, 80, rect.right - rect.left,
                                  rect.bottom - rect.top, nullptr, nullptr, wc.hInstance,
                                  nullptr);
    if (!hwnd) {
        std::fprintf(stderr, "CreateWindowEx failed\n");
        return 1;
    }
    ::ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    ::UpdateWindow(hwnd);

    brp::Dx12Video video;
    std::string detail;
    if (!video.Init(hwnd, 1280, 720, nullptr, detail)) {
        std::fprintf(stderr, "Dx12Video::Init: %s\n", detail.c_str());
        return 1;
    }
    std::printf("shader model %s\n", video.shader_model().c_str());

    const int width = 1920;
    const int height = 1080;
    const auto started = std::chrono::steady_clock::now();
    int presented = 0;
    for (int tick = 0; tick < 240; ++tick) {
        MSG msg{};
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) {
                tick = 100000;
                break;
            }
        }
        auto rgba = Frame(width, height, tick);
        if (!video.DrawCpu(rgba.data(), width, height, false)) {
            std::fprintf(stderr, "DrawCpu failed: %s\n", video.status().c_str());
            break;
        }
        if (!video.Present(false)) {
            std::fprintf(stderr, "Present failed\n");
            break;
        }
        ++presented;
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    std::printf("presented %d frames in %.2f s = %.1f fps, misses %llu, lag %.2f ms\n",
                presented, seconds, presented / seconds, video.present_misses(),
                video.present_lag_ms());
    ::DestroyWindow(hwnd);
    return presented > 0 ? 0 : 2;
}
