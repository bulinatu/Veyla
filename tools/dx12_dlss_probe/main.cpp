// Smoke test: DLSS SR on D3D12 -> shared D3D11 texture -> pure DX12 surface.
// No console, no input, no clicks.

#include "media/dx12_video.h"
#include "media/nv_dlss_sr.h"

#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

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
    const int bar = (tick * 6) % (width + 300) - 150;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * width + x) * 4;
            std::uint8_t r = static_cast<std::uint8_t>(18 + (x * 90 / width));
            std::uint8_t g = static_cast<std::uint8_t>(22 + (y * 90 / height));
            std::uint8_t b = 52;
            if (x >= bar && x < bar + 120) {
                r = 200;
                g = 170;
                b = 255;
            }
            if (((x / 24) + (y / 24)) % 2 == 0) {
                r = static_cast<std::uint8_t>(r + 24);
                g = static_cast<std::uint8_t>(g + 24);
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
    const std::wstring runtime =
        argc > 1 ? std::wstring(argv[1], argv[1] + std::strlen(argv[1]))
                 : L"C:\\Users\\Cosmin\\Desktop\\BetterRemotePlay\\dist\\Veyla";

    Microsoft::WRL::ComPtr<ID3D11Device> d3d11;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL level{};
    if (FAILED(::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                   D3D11_SDK_VERSION, &d3d11, &level, &context))) {
        std::fprintf(stderr, "D3D11CreateDevice failed\n");
        return 1;
    }

    brp::media::NvDlssSr sr;
    std::string detail;
    if (!sr.LoadRuntime(runtime, detail)) {
        std::fprintf(stderr, "LoadRuntime: %s\n", detail.c_str());
        return 2;
    }
    const int in_w = 1920;
    const int in_h = 1080;
    const int out_w = 2560;
    const int out_h = 1440;
    if (!sr.Initialize(d3d11.Get(), context.Get(), in_w, in_h, out_w, out_h, 11, 2,
                       detail)) {
        std::fprintf(stderr, "Initialize: %s\n", detail.c_str());
        return 3;
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = L"VeylaDx12DlssProbe";
    ::RegisterClassExW(&wc);
    RECT rect{0, 0, 1280, 720};
    ::AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = ::CreateWindowExW(0, wc.lpszClassName, L"Veyla DX12 + DLSS probe",
                                  WS_OVERLAPPEDWINDOW, 80, 80, rect.right - rect.left,
                                  rect.bottom - rect.top, nullptr, nullptr, wc.hInstance,
                                  nullptr);
    if (!hwnd) {
        std::fprintf(stderr, "CreateWindowEx failed\n");
        return 4;
    }
    ::ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    ::UpdateWindow(hwnd);

    brp::Dx12Video video;
    if (!video.Init(hwnd, 1280, 720, d3d11.Get(), detail)) {
        std::fprintf(stderr, "Dx12Video: %s\n", detail.c_str());
        return 5;
    }

    int presented = 0;
    int sr_fail = 0;
    const auto started = std::chrono::steady_clock::now();
    for (int tick = 0; tick < 180; ++tick) {
        MSG msg{};
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) {
                tick = 100000;
                break;
            }
        }
        auto rgba = Frame(in_w, in_h, tick);
        brp::media::NvDlssSr::Frame frame;
        if (!sr.Process(rgba.data(), in_w, in_h, frame, detail)) {
            ++sr_fail;
            if (sr_fail < 3) {
                std::fprintf(stderr, "SR: %s\n", detail.c_str());
            }
            continue;
        }
        const HANDLE shared = sr.SharedHandle();
        if (!shared) {
            ++sr_fail;
            std::fprintf(stderr, "SR has no shared output\n");
            continue;
        }
        if (!video.DrawShared(shared, out_w, out_h, false)) {
            std::fprintf(stderr, "DrawShared: %s\n", video.status().c_str());
            break;
        }
        if (tick == 60) {
            video.CaptureToBmp("C:\\Users\\Cosmin\\AppData\\Local\\Temp\\opencode\\dx12_dlss_probe.bmp");
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
    std::printf("presented %d frames in %.2f s (%.1f fps), SR failures %d\n", presented,
                seconds, presented / seconds, sr_fail);
    ::DestroyWindow(hwnd);
    return presented > 0 ? 0 : 6;
}
