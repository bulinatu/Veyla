#include "ui/ui_renderer.h"

#include <d2d1_1helper.h>
#include <dwrite_3.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <filesystem>

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

uint64_t MixHash(uint64_t hash, uint64_t value) {
    hash ^= value + 0x9E3779B97F4A7C15ull + (hash << 6) + (hash >> 2);
    return hash;
}

uint64_t HashFloat(uint64_t hash, float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return MixHash(hash, bits);
}

uint64_t HashColor(uint64_t hash, D2D1_COLOR_F color) {
    hash = HashFloat(hash, color.r);
    hash = HashFloat(hash, color.g);
    hash = HashFloat(hash, color.b);
    hash = HashFloat(hash, color.a);
    return hash;
}

bool SameColor(const D2D1_COLOR_F& a, const D2D1_COLOR_F& b) {
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

constexpr size_t kTextCacheBudget = 1024;
constexpr size_t kGradientCacheBudget = 48;
constexpr size_t kClipCacheBudget = 96;

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
    if (FAILED(hr)) {
        return false;
    }

    // PlayStation's own SST typeface is proprietary. Use the bundled Noto Sans
    // pairing instead: Display for headings, Text for body and small copy.
    // Noto Sans is the open family used in PlayStation's OSS stack. SST itself
    // is not redistributable, so this is the honest closest match.
    ComPtr<IDWriteFactory5> factory5;
    const HRESULT f5hr = dwrite_.As(&factory5);
    if (SUCCEEDED(f5hr)) {
        ComPtr<IDWriteFontSetBuilder1> builder;
        const HRESULT bhr = factory5->CreateFontSetBuilder(&builder);
        if (SUCCEEDED(bhr)) {
            wchar_t exe_path[MAX_PATH] = {};
            ::GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
            const std::filesystem::path font_dir =
                std::filesystem::path(exe_path).parent_path() / L"fonts";

            bool added = false;
            const wchar_t* files[] = {
                L"NotoSans-Regular.ttf",         L"NotoSans-Medium.ttf",
                L"NotoSans-SemiBold.ttf",        L"NotoSans-Bold.ttf",
                L"NotoSansDisplay-Regular.ttf",  L"NotoSansDisplay-Medium.ttf",
                L"NotoSansDisplay-SemiBold.ttf", L"NotoSansDisplay-Bold.ttf"};
            for (const wchar_t* name : files) {
                const std::wstring path = (font_dir / name).wstring();
                ComPtr<IDWriteFontFile> file;
                const HRESULT fhr = factory5->CreateFontFileReference(path.c_str(), nullptr, &file);
                const HRESULT ahr = SUCCEEDED(fhr) ? builder->AddFontFile(file.Get()) : fhr;
                if (SUCCEEDED(ahr)) {
                    added = true;
                }
            }

            if (added) {
                ComPtr<IDWriteFontSet> set;
                ComPtr<IDWriteFontCollection1> collection;
                const HRESULT sethr = builder->CreateFontSet(&set);
                const HRESULT colhr =
                    SUCCEEDED(sethr)
                        ? factory5->CreateFontCollectionFromFontSet(set.Get(), &collection)
                        : sethr;
                if (SUCCEEDED(colhr)) {
                    UINT32 body_index = 0;
                    UINT32 display_index = 0;
                    BOOL body_found = FALSE;
                    BOOL display_found = FALSE;
                    collection->FindFamilyName(L"Noto Sans", &body_index, &body_found);
                    collection->FindFamilyName(L"Noto Sans Display", &display_index,
                                               &display_found);
                    if (body_found && display_found) {
                        font_collection_ = collection;
                        body_font_ = L"Noto Sans";
                        display_font_ = L"Noto Sans Display";
                    }
                }
            }
        }
    }

    if (!font_collection_) {
        // Development builds without the copied font folder: fall back to the
        // closest modern system pairing instead of failing to draw text.
        ComPtr<IDWriteFontCollection> system;
        if (SUCCEEDED(dwrite_->GetSystemFontCollection(&system, FALSE))) {
            UINT32 index = 0;
            BOOL found = FALSE;
            if (SUCCEEDED(system->FindFamilyName(L"Segoe UI Variable Text", &index, &found)) &&
                found) {
                body_font_ = L"Segoe UI Variable Text";
            }
            if (SUCCEEDED(system->FindFamilyName(L"Segoe UI Variable Display", &index, &found)) &&
                found) {
                display_font_ = L"Segoe UI Variable Display";
            }
        }
    }

    std::fprintf(stderr, "d2d: fonts %ls / %ls (%s)\n", body_font_.c_str(),
                 display_font_.c_str(), font_collection_ ? "bundled" : "system");
    return true;
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

void Renderer::StrokeCircle(float cx, float cy, float radius, D2D1_COLOR_F color, float stroke) {
    brush_->SetColor(color);
    dc_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), radius, radius), brush_.Get(), stroke);
}

void Renderer::StrokeArc(float cx, float cy, float radius, float start_radians, float sweep_radians,
                         D2D1_COLOR_F color, float stroke) {
    ComPtr<ID2D1PathGeometry> path;
    if (FAILED(factory_->CreatePathGeometry(&path))) {
        return;
    }
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(path->Open(&sink))) {
        return;
    }

    const D2D1_POINT_2F start =
        D2D1::Point2F(cx + std::cos(start_radians) * radius,
                      cy + std::sin(start_radians) * radius);
    const float end_angle = start_radians + sweep_radians;
    const D2D1_POINT_2F end =
        D2D1::Point2F(cx + std::cos(end_angle) * radius, cy + std::sin(end_angle) * radius);

    sink->BeginFigure(start, D2D1_FIGURE_BEGIN_HOLLOW);
    const D2D1_ARC_SEGMENT arc{
        end,
        D2D1::SizeF(radius, radius),
        0.0f,
        sweep_radians >= 0.0f ? D2D1_SWEEP_DIRECTION_CLOCKWISE
                              : D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE,
        std::fabs(sweep_radians) > 3.14159265f ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL,
    };
    sink->AddArc(arc);
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();

    brush_->SetColor(color);
    dc_->DrawGeometry(path.Get(), brush_.Get(), stroke, nullptr);
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
    // The UI paints the same two-stop gradients every frame. Rebuilding the
    // stop collection and brush per call is pure resource churn; keep one
    // brush per colour pair and just move its endpoints.
    const uint64_t key = HashColor(HashColor(0x84222325ull, top), bottom);
    auto it = linear_cache_.find(key);
    if (it != linear_cache_.end() && SameColor(it->second.top, top) &&
        SameColor(it->second.bottom, bottom)) {
        ID2D1LinearGradientBrush* brush = it->second.brush.Get();
        if (brush) {
            brush->SetStartPoint(D2D1::Point2F(rect.left, rect.top));
            brush->SetEndPoint(D2D1::Point2F(rect.left, rect.bottom));
            dc_->FillRectangle(rect, brush);
            return;
        }
    }

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

    if (linear_cache_.size() >= kGradientCacheBudget) {
        linear_cache_.clear();
    }
    LinearGradientEntry entry;
    entry.top = top;
    entry.bottom = bottom;
    entry.stops = std::move(collection);
    entry.brush = std::move(brush);
    linear_cache_.emplace(key, std::move(entry));
}

void Renderer::FillRadial(float cx, float cy, float radius, D2D1_COLOR_F inner,
                          D2D1_COLOR_F outer) {
    const uint64_t key = HashColor(HashColor(0x5BD1E995ull, inner), outer);
    auto it = radial_cache_.find(key);
    if (it != radial_cache_.end() && SameColor(it->second.inner, inner) &&
        SameColor(it->second.outer, outer)) {
        ID2D1RadialGradientBrush* brush = it->second.brush.Get();
        if (brush) {
            brush->SetCenter(D2D1::Point2F(cx, cy));
            brush->SetGradientOriginOffset(D2D1::Point2F(0.0f, 0.0f));
            brush->SetRadiusX(radius);
            brush->SetRadiusY(radius);
            const D2D1_RECT_F area{cx - radius, cy - radius, cx + radius, cy + radius};
            dc_->FillRectangle(area, brush);
            return;
        }
    }

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

    if (radial_cache_.size() >= kGradientCacheBudget) {
        radial_cache_.clear();
    }
    RadialGradientEntry entry;
    entry.inner = inner;
    entry.outer = outer;
    entry.stops = std::move(collection);
    entry.brush = std::move(brush);
    radial_cache_.emplace(key, std::move(entry));
}

IDWriteTextFormat* Renderer::Format(float size, DWRITE_FONT_WEIGHT weight,
                                    DWRITE_TEXT_ALIGNMENT align,
                                    DWRITE_PARAGRAPH_ALIGNMENT valign, bool wrap) {
    const uint64_t key = (static_cast<uint64_t>(static_cast<int>(std::lround(size * 2.0f))) << 32) |
                         (static_cast<uint64_t>(weight) << 16) |
                         (static_cast<uint64_t>(align) << 8) | static_cast<uint64_t>(valign) |
                         (static_cast<uint64_t>(wrap) << 40);

    auto it = formats_.find(key);
    if (it != formats_.end()) {
        return it->second.Get();
    }

    ComPtr<IDWriteTextFormat> format;
    const wchar_t* family = size >= 22.0f ? display_font_.c_str() : body_font_.c_str();
    if (FAILED(dwrite_->CreateTextFormat(family, font_collection_.Get(), weight,
                                         DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                         size, L"en-us", &format))) {
        return nullptr;
    }
    format->SetTextAlignment(align);
    format->SetParagraphAlignment(valign);
    format->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);

    auto [pos, inserted] = formats_.emplace(key, std::move(format));
    return pos->second.Get();
}

void Renderer::ClearTextCache() {
    text_cache_.clear();
}

IDWriteTextLayout* Renderer::Layout(const wchar_t* text, float size,
                                    DWRITE_FONT_WEIGHT weight, DWRITE_TEXT_ALIGNMENT align,
                                    DWRITE_PARAGRAPH_ALIGNMENT valign, float width, float height,
                                    bool ellipsis, bool wrap) {
    IDWriteTextFormat* format = Format(size, weight, align, valign, wrap);
    if (!format || !text) {
        return nullptr;
    }

    const size_t length = std::wcslen(text);
    uint64_t key = 1469598103934665603ull;
    for (size_t i = 0; i < length; ++i) {
        key = (key ^ static_cast<uint64_t>(text[i])) * 1099511628211ull;
    }
    key = MixHash(key, static_cast<uint64_t>(std::lround(size * 2.0f)));
    key = MixHash(key, static_cast<uint64_t>(weight) << 32 | static_cast<uint64_t>(align) << 16 |
                           static_cast<uint64_t>(valign));
    key = HashFloat(key, width);
    key = HashFloat(key, height);
    key = MixHash(key, ellipsis ? 0xABCDEFull : 0x123456ull);
    key = MixHash(key, wrap ? 0x5A5A5Aull : 0x0ull);

    auto it = text_cache_.find(key);
    if (it != text_cache_.end()) {
        for (TextEntry& entry : it->second) {
            if (entry.layout && entry.text.size() == length &&
                std::wmemcmp(entry.text.c_str(), text, length) == 0) {
                return entry.layout.Get();
            }
        }
    }

    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dwrite_->CreateTextLayout(text, static_cast<UINT32>(length), format, width,
                                         height, &layout))) {
        return nullptr;
    }

    if (ellipsis) {
        DWRITE_TRIMMING trimming{};
        trimming.granularity = DWRITE_TRIMMING_GRANULARITY_CHARACTER;
        ComPtr<IDWriteInlineObject> sign;
        if (SUCCEEDED(dwrite_->CreateEllipsisTrimmingSign(layout.Get(), &sign))) {
            layout->SetTrimming(&trimming, sign.Get());
        }
    }

    if (text_cache_.size() >= kTextCacheBudget) {
        text_cache_.clear();
    }

    TextEntry& stored = text_cache_[key].emplace_back();
    stored.text.assign(text, length);
    stored.layout = std::move(layout);
    return stored.layout.Get();
}

void Renderer::Text(const wchar_t* text, const D2D1_RECT_F& rect, float size, D2D1_COLOR_F color,
                    DWRITE_FONT_WEIGHT weight, DWRITE_TEXT_ALIGNMENT align,
                    DWRITE_PARAGRAPH_ALIGNMENT valign) {
    IDWriteTextLayout* layout =
        Layout(text, size, weight, align, valign, rect.right - rect.left,
               rect.bottom - rect.top, false, false);
    if (!layout) {
        return;
    }
    brush_->SetColor(color);
    dc_->DrawTextLayout(D2D1::Point2F(rect.left, rect.top), layout, brush_.Get(),
                        D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void Renderer::TextWrapped(const wchar_t* text, const D2D1_RECT_F& rect, float size,
                            D2D1_COLOR_F color, DWRITE_FONT_WEIGHT weight,
                            DWRITE_TEXT_ALIGNMENT align) {
    IDWriteTextLayout* layout =
        Layout(text, size, weight, align, DWRITE_PARAGRAPH_ALIGNMENT_NEAR, rect.right - rect.left,
               rect.bottom - rect.top, false, true);
    if (!layout) {
        return;
    }
    brush_->SetColor(color);
    dc_->DrawTextLayout(D2D1::Point2F(rect.left, rect.top), layout, brush_.Get(),
                        D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void Renderer::TextEllipsis(const wchar_t* text, const D2D1_RECT_F& rect, float size,
                            D2D1_COLOR_F color, DWRITE_FONT_WEIGHT weight,
                            DWRITE_TEXT_ALIGNMENT align) {
    IDWriteTextLayout* layout =
        Layout(text, size, weight, align, DWRITE_PARAGRAPH_ALIGNMENT_CENTER,
               rect.right - rect.left, rect.bottom - rect.top, true);
    if (!layout) {
        return;
    }

    brush_->SetColor(color);
    dc_->DrawTextLayout(D2D1::Point2F(rect.left, rect.top), layout, brush_.Get(),
                        D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

float Renderer::TextWidth(const wchar_t* text, float size, DWRITE_FONT_WEIGHT weight) {
    IDWriteTextLayout* layout = Layout(text, size, weight, DWRITE_TEXT_ALIGNMENT_LEADING,
                                       DWRITE_PARAGRAPH_ALIGNMENT_CENTER, 4096.0f, 4096.0f,
                                       false);
    if (!layout) {
        return 0.0f;
    }

    DWRITE_TEXT_METRICS metrics{};
    if (FAILED(layout->GetMetrics(&metrics))) {
        return 0.0f;
    }
    return metrics.widthIncludingTrailingWhitespace;
}

void Renderer::PushRoundedClip(const D2D1_RECT_F& rect, float radius) {
    const uint64_t key = HashFloat(
        HashFloat(HashFloat(HashFloat(HashFloat(0xC2B2AE35ull, rect.left), rect.top), rect.right),
                  rect.bottom),
        radius);
    ComPtr<ID2D1RoundedRectangleGeometry> geometry;
    auto found = clip_cache_.find(key);
    if (found != clip_cache_.end()) {
        for (ClipEntry& entry : found->second) {
            if (entry.radius == radius && entry.rect.left == rect.left &&
                entry.rect.top == rect.top && entry.rect.right == rect.right &&
                entry.rect.bottom == rect.bottom && entry.geometry) {
                geometry = entry.geometry;
                break;
            }
        }
    }
    if (!geometry) {
        if (FAILED(factory_->CreateRoundedRectangleGeometry(
                D2D1::RoundedRect(rect, radius, radius), &geometry))) {
            return;
        }
        if (clip_cache_.size() >= kClipCacheBudget) {
            clip_cache_.clear();
        }
        ClipEntry& stored = clip_cache_[key].emplace_back();
        stored.rect = rect;
        stored.radius = radius;
        stored.geometry = geometry;
    }

    ComPtr<ID2D1Layer> layer;
    if (FAILED(dc_->CreateLayer(nullptr, &layer))) {
        return;
    }

    D2D1_LAYER_PARAMETERS1 params{};
    // Constrain the layer to the clipped widget. With InfiniteRect() Direct2D
    // sizes the intermediate surface to the whole 1440p/2160p backbuffer for
    // every tile, which is both a fill-rate and memory hit at 240 Hz.
    params.contentBounds = {rect.left - 2.0f, rect.top - 2.0f, rect.right + 2.0f,
                            rect.bottom + 2.0f};
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
