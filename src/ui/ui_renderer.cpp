#include "ui/ui_renderer.h"

#include <d2d1_1helper.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>

using Microsoft::WRL::ComPtr;

namespace brp::ui {
namespace {

ComPtr<ID2D1GradientStopCollection1> MakeStops(ID2D1DeviceContext* dc,
                                               const D2D1_GRADIENT_STOP (&stops)[2]) {
    ComPtr<ID2D1GradientStopCollection1> collection;
    dc->CreateGradientStopCollection(stops, 2, D2D1_COLOR_SPACE_SRGB, D2D1_COLOR_SPACE_SRGB,
                                     D2D1_BUFFER_PRECISION_8BPC_UNORM, D2D1_EXTEND_MODE_CLAMP,
                                     D2D1_COLOR_INTERPOLATION_MODE_PREMULTIPLIED, &collection);
    return collection;
}

}  // namespace

bool Renderer::Init(ID3D11Device* device) {
    D2D1_FACTORY_OPTIONS options{};
    HRESULT hr = ::D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1),
                                     &options, reinterpret_cast<void**>(
                                                   factory_.ReleaseAndGetAddressOf()));
    if (FAILED(hr)) {
        std::fprintf(stderr, "d2d: D2D1CreateFactory 0x%08lX\n", static_cast<unsigned long>(hr));
        return false;
    }

    ComPtr<IDXGIDevice> dxgi_device;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgi_device)))) {
        std::fprintf(stderr, "d2d: QueryInterface(IDXGIDevice) failed\n");
        return false;
    }
    if (FAILED(factory_->CreateDevice(dxgi_device.Get(), &device_))) {
        std::fprintf(stderr, "d2d: CreateDevice failed\n");
        return false;
    }
    if (FAILED(device_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc_))) {
        std::fprintf(stderr, "d2d: CreateDeviceContext failed\n");
        return false;
    }

    dc_->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
    dc_->SetDpi(96.0f, 96.0f);
    dc_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    dc_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

    if (FAILED(dc_->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &brush_))) {
        return false;
    }

    hr = ::DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                               reinterpret_cast<IUnknown**>(dwrite_.ReleaseAndGetAddressOf()));
    return SUCCEEDED(hr);
}

void Renderer::ReleaseTarget() {
    if (dc_) {
        dc_->SetTarget(nullptr);
    }
    target_.Reset();
}

bool Renderer::BindSwapchain(ID3D11Texture2D* backbuffer) {
    target_.Reset();

    ComPtr<IDXGISurface> surface;
    HRESULT hr = backbuffer->QueryInterface(IID_PPV_ARGS(&surface));
    if (FAILED(hr)) {
        std::fprintf(stderr, "d2d: QueryInterface(IDXGISurface) 0x%08lX\n",
                     static_cast<unsigned long>(hr));
        return false;
    }

    DXGI_SURFACE_DESC surface_desc{};
    surface->GetDesc(&surface_desc);
    std::fprintf(stderr, "[d2d] bind surface %ux%u\n", surface_desc.Width,
                 surface_desc.Height);

    // D2D is picky about how the target bitmap is described. Probe the legal
    // combinations instead of hard-coding one and failing on a driver.
    struct Candidate {
        DXGI_FORMAT format;
        D2D1_ALPHA_MODE alpha;
    };
    const Candidate candidates[] = {
        {DXGI_FORMAT_R8G8B8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED},
        {DXGI_FORMAT_R8G8B8A8_UNORM, D2D1_ALPHA_MODE_IGNORE},
        {DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_PREMULTIPLIED},
        {DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_IGNORE},
        {DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED},
    };

    for (const Candidate& candidate : candidates) {
        const auto props = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET, D2D1::PixelFormat(candidate.format, candidate.alpha));
        hr = dc_->CreateBitmapFromDxgiSurface(surface.Get(), &props, &target_);
        if (SUCCEEDED(hr)) {
            std::printf("d2d: target bitmap fmt=%d alpha=%d\n", static_cast<int>(candidate.format),
                        static_cast<int>(candidate.alpha));
            dc_->SetTarget(target_.Get());
            return true;
        }
    }

    std::fprintf(stderr, "d2d: CreateBitmapFromDxgiSurface 0x%08lX\n",
                 static_cast<unsigned long>(hr));
    return false;
}

void Renderer::BeginFrame() {
    dc_->BeginDraw();
    dc_->SetTransform(D2D1::Matrix3x2F::Scale(scale_, scale_));
}

void Renderer::EndFrame() {
    while (!layers_.empty()) {
        PopClip();
    }
    dc_->EndDraw();
}

void Renderer::FillRect(const D2D1_RECT_F& rect, D2D1_COLOR_F color) {
    brush_->SetColor(color);
    dc_->FillRectangle(rect, brush_.Get());
}

void Renderer::FillRounded(const D2D1_RECT_F& rect, float radius, D2D1_COLOR_F color) {
    brush_->SetColor(color);
    dc_->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush_.Get());
}

void Renderer::StrokeRounded(const D2D1_RECT_F& rect, float radius, D2D1_COLOR_F color,
                             float stroke) {
    brush_->SetColor(color);
    dc_->DrawRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush_.Get(), stroke);
}

void Renderer::FillCircle(float cx, float cy, float radius, D2D1_COLOR_F color) {
    brush_->SetColor(color);
    dc_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), radius, radius), brush_.Get());
}

void Renderer::Line(D2D1_POINT_2F a, D2D1_POINT_2F b, D2D1_COLOR_F color, float stroke) {
    brush_->SetColor(color);
    dc_->DrawLine(a, b, brush_.Get(), stroke);
}

void Renderer::FillTriangle(D2D1_POINT_2F a, D2D1_POINT_2F b, D2D1_POINT_2F c,
                            D2D1_COLOR_F color) {
    ComPtr<ID2D1PathGeometry> path;
    if (FAILED(factory_->CreatePathGeometry(&path))) {
        return;
    }
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(path->Open(&sink))) {
        return;
    }
    sink->BeginFigure(a, D2D1_FIGURE_BEGIN_FILLED);
    sink->AddLine(b);
    sink->AddLine(c);
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    sink->Close();

    brush_->SetColor(color);
    dc_->FillGeometry(path.Get(), brush_.Get());
}

void Renderer::FillLinearV(const D2D1_RECT_F& rect, D2D1_COLOR_F top, D2D1_COLOR_F bottom) {
    const D2D1_GRADIENT_STOP stops[2] = {{0.0f, top}, {1.0f, bottom}};
    ComPtr<ID2D1GradientStopCollection1> collection = MakeStops(dc_.Get(), stops);
    if (!collection) {
        return;
    }

    ComPtr<ID2D1LinearGradientBrush> brush;
    const auto props = D2D1::LinearGradientBrushProperties(
        D2D1::Point2F(rect.left, rect.top), D2D1::Point2F(rect.left, rect.bottom));
    if (FAILED(dc_->CreateLinearGradientBrush(props, collection.Get(), &brush))) {
        return;
    }
    dc_->FillRectangle(rect, brush.Get());
}

void Renderer::FillRadial(float cx, float cy, float radius, D2D1_COLOR_F inner,
                          D2D1_COLOR_F outer) {
    const D2D1_GRADIENT_STOP stops[2] = {{0.0f, inner}, {1.0f, outer}};
    ComPtr<ID2D1GradientStopCollection1> collection = MakeStops(dc_.Get(), stops);
    if (!collection) {
        return;
    }

    D2D1_RADIAL_GRADIENT_BRUSH_PROPERTIES props{};
    props.center = D2D1::Point2F(cx, cy);
    props.gradientOriginOffset = D2D1::Point2F(0.0f, 0.0f);
    props.radiusX = radius;
    props.radiusY = radius;

    ComPtr<ID2D1RadialGradientBrush> brush;
    if (FAILED(dc_->CreateRadialGradientBrush(props, collection.Get(), &brush))) {
        return;
    }

    const D2D1_RECT_F area{cx - radius, cy - radius, cx + radius, cy + radius};
    dc_->FillRectangle(area, brush.Get());
}

IDWriteTextFormat* Renderer::Format(float size, DWRITE_FONT_WEIGHT weight,
                                    DWRITE_TEXT_ALIGNMENT align,
                                    DWRITE_PARAGRAPH_ALIGNMENT valign) {
    const uint64_t key = (static_cast<uint64_t>(static_cast<int>(std::lround(size * 2.0f))) << 32) |
                         (static_cast<uint64_t>(weight) << 16) |
                         (static_cast<uint64_t>(align) << 8) | static_cast<uint64_t>(valign);

    auto it = formats_.find(key);
    if (it != formats_.end()) {
        return it->second.Get();
    }

    ComPtr<IDWriteTextFormat> format;
    if (FAILED(dwrite_->CreateTextFormat(L"Segoe UI", nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                         DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &format))) {
        return nullptr;
    }
    format->SetTextAlignment(align);
    format->SetParagraphAlignment(valign);
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    auto [pos, inserted] = formats_.emplace(key, std::move(format));
    return pos->second.Get();
}

void Renderer::Text(const wchar_t* text, const D2D1_RECT_F& rect, float size, D2D1_COLOR_F color,
                    DWRITE_FONT_WEIGHT weight, DWRITE_TEXT_ALIGNMENT align,
                    DWRITE_PARAGRAPH_ALIGNMENT valign) {
    IDWriteTextFormat* format = Format(size, weight, align, valign);
    if (!format || !text) {
        return;
    }
    brush_->SetColor(color);
    dc_->DrawTextW(text, static_cast<UINT32>(std::wcslen(text)), format, rect, brush_.Get(),
                   D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
}

void Renderer::TextEllipsis(const wchar_t* text, const D2D1_RECT_F& rect, float size,
                            D2D1_COLOR_F color, DWRITE_FONT_WEIGHT weight,
                            DWRITE_TEXT_ALIGNMENT align) {
    IDWriteTextFormat* format = Format(size, weight, align, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    if (!format || !text) {
        return;
    }

    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dwrite_->CreateTextLayout(text, static_cast<UINT32>(std::wcslen(text)), format,
                                         rect.right - rect.left, rect.bottom - rect.top,
                                         &layout))) {
        return;
    }

    DWRITE_TRIMMING trimming{};
    trimming.granularity = DWRITE_TRIMMING_GRANULARITY_CHARACTER;
    ComPtr<IDWriteInlineObject> ellipsis;
    if (SUCCEEDED(dwrite_->CreateEllipsisTrimmingSign(layout.Get(), &ellipsis))) {
        layout->SetTrimming(&trimming, ellipsis.Get());
    }

    brush_->SetColor(color);
    dc_->DrawTextLayout(D2D1::Point2F(rect.left, rect.top), layout.Get(), brush_.Get(),
                        D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

float Renderer::TextWidth(const wchar_t* text, float size, DWRITE_FONT_WEIGHT weight) {    IDWriteTextFormat* format = Format(size, weight, DWRITE_TEXT_ALIGNMENT_LEADING,
                                       DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    if (!format || !text) {
        return 0.0f;
    }

    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dwrite_->CreateTextLayout(text, static_cast<UINT32>(std::wcslen(text)), format,
                                         4096.0f, 4096.0f, &layout))) {
        return 0.0f;
    }
    DWRITE_TEXT_METRICS metrics{};
    if (FAILED(layout->GetMetrics(&metrics))) {
        return 0.0f;
    }
    return metrics.widthIncludingTrailingWhitespace;
}

void Renderer::PushRoundedClip(const D2D1_RECT_F& rect, float radius) {
    ComPtr<ID2D1RoundedRectangleGeometry> geometry;
    if (FAILED(factory_->CreateRoundedRectangleGeometry(D2D1::RoundedRect(rect, radius, radius),
                                                        &geometry))) {
        return;
    }

    ComPtr<ID2D1Layer> layer;
    if (FAILED(dc_->CreateLayer(nullptr, &layer))) {
        return;
    }

    D2D1_LAYER_PARAMETERS1 params{};
    params.contentBounds = D2D1::InfiniteRect();
    params.geometricMask = geometry.Get();
    params.maskAntialiasMode = D2D1_ANTIALIAS_MODE_PER_PRIMITIVE;
    params.maskTransform = D2D1::Matrix3x2F::Identity();
    params.opacity = 1.0f;
    params.opacityBrush = nullptr;
    params.layerOptions = D2D1_LAYER_OPTIONS1_NONE;

    dc_->PushLayer(params, layer.Get());
    layers_.push_back({layer, geometry});
}

void Renderer::PopClip() {
    if (layers_.empty()) {
        return;
    }
    layers_.pop_back();
    dc_->PopLayer();
}

}  // namespace brp::ui
