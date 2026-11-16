// Minimal Direct2D interop probe. Not part of the app: this exists to find out
// whether a failure is our code or the machine/driver.

#include <windows.h>

#include <d2d1_1.h>
#include <d2d1_1helper.h>
#include <d3d11.h>
#include <cstdio>

namespace {

void Report(const char* what, HRESULT hr) {
    std::printf("  %-46s 0x%08lX\n", what, static_cast<unsigned long>(hr));
}

ID3D11Texture2D* MakeTexture(ID3D11Device* dev, DXGI_FORMAT format, UINT bind) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = 64;
    td.Height = 64;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = format;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = bind;
    ID3D11Texture2D* tex = nullptr;
    return SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &tex)) ? tex : nullptr;
}

}  // namespace

int main() {
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL fl{};
    HRESULT hr = ::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                     D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                     D3D11_SDK_VERSION, &dev, &fl, &ctx);
    std::printf("D3D11CreateDevice  0x%08lX fl=0x%X\n", static_cast<unsigned long>(hr), fl);
    if (FAILED(hr)) return 1;

    IDXGIDevice* dxgi_device = nullptr;
    dev->QueryInterface(IID_PPV_ARGS(&dxgi_device));

    IDXGIAdapter* adapter = nullptr;
    dxgi_device->GetAdapter(&adapter);
    DXGI_ADAPTER_DESC desc{};
    adapter->GetDesc(&desc);
    std::printf("adapter: %ls\n", desc.Description);

    ID2D1Factory1* factory = nullptr;
    D2D1_FACTORY_OPTIONS options{};
    hr = ::D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &options,
                             reinterpret_cast<void**>(&factory));
    Report("D2D1CreateFactory(ID2D1Factory1)", hr);
    if (FAILED(hr)) return 1;

    ID2D1Device* d2d_device = nullptr;
    Report("factory1->CreateDevice", factory->CreateDevice(dxgi_device, &d2d_device));

    ID2D1DeviceContext* dc = nullptr;
    if (d2d_device) {
        Report("d2dDevice->CreateDeviceContext",
               d2d_device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc));
    }

    // Alternative: the global D2D1CreateDevice entry point.
    ID2D1Device* d2d_device2 = nullptr;
    Report("D2D1CreateDevice", ::D2D1CreateDevice(dxgi_device, nullptr, &d2d_device2));
    ID2D1DeviceContext* dc2 = nullptr;
    if (d2d_device2) {
        Report("alt d2dDevice->CreateDeviceContext",
               d2d_device2->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc2));
    }

    const DXGI_FORMAT formats[] = {DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM};
    const UINT binds[] = {D3D11_BIND_RENDER_TARGET,
                          D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE};

    for (DXGI_FORMAT format : formats) {
        for (UINT bind : binds) {
            ID3D11Texture2D* tex = MakeTexture(dev, format, bind);
            if (!tex) continue;
            IDXGISurface* surface = nullptr;
            tex->QueryInterface(IID_PPV_ARGS(&surface));

            char label[160];

            // 1. Target bitmap (D2D 1.1)
            if (dc) {
                const auto props = D2D1::BitmapProperties1(
                    D2D1_BITMAP_OPTIONS_TARGET,
                    D2D1::PixelFormat(format, D2D1_ALPHA_MODE_PREMULTIPLIED));
                ID2D1Bitmap1* bmp = nullptr;
                hr = dc->CreateBitmapFromDxgiSurface(surface, &props, &bmp);
                std::snprintf(label, sizeof(label), "1.1 target fmt=%d bind=0x%X", format, bind);
                Report(label, hr);
                if (bmp) bmp->Release();
            }

            // 2. Non-target bitmap (just an interop copy)
            if (dc) {
                const auto props = D2D1::BitmapProperties1(
                    D2D1_BITMAP_OPTIONS_NONE,
                    D2D1::PixelFormat(format, D2D1_ALPHA_MODE_PREMULTIPLIED));
                ID2D1Bitmap1* bmp = nullptr;
                hr = dc->CreateBitmapFromDxgiSurface(surface, &props, &bmp);
                std::snprintf(label, sizeof(label), "1.1 plain  fmt=%d bind=0x%X", format, bind);
                Report(label, hr);
                if (bmp) bmp->Release();
            }

            // 3. Legacy D2D 1.0 render target
            const auto rt_props =
                D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                                             D2D1::PixelFormat(format, D2D1_ALPHA_MODE_PREMULTIPLIED));
            ID2D1RenderTarget* rt = nullptr;
            hr = factory->CreateDxgiSurfaceRenderTarget(surface, &rt_props, &rt);
            std::snprintf(label, sizeof(label), "1.0 RT     fmt=%d bind=0x%X", format, bind);
            Report(label, hr);
            if (rt) {
                rt->BeginDraw();
                rt->Clear(D2D1::ColorF(0.2f, 0.4f, 0.8f));
                const HRESULT end = rt->EndDraw();
                std::snprintf(label, sizeof(label), "1.0 RT draw fmt=%d bind=0x%X", format, bind);
                Report(label, end);
                rt->Release();
            }

            surface->Release();
            tex->Release();
        }
    }

    // Windows build, for the record.
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    if (HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll")) {
        auto fn = reinterpret_cast<RtlGetVersionFn>(
            ::GetProcAddress(ntdll, "RtlGetVersion"));
        if (fn) {
            RTL_OSVERSIONINFOW vi{};
            vi.dwOSVersionInfoSize = sizeof(vi);
            if (fn(&vi) == 0) {
                std::printf("windows build %lu.%lu.%lu\n", vi.dwMajorVersion, vi.dwMinorVersion,
                            vi.dwBuildNumber);
            }
        }
    }

    return 0;
}
