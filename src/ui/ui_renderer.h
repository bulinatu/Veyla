#pragma once

#include <windows.h>

#include <d2d1_1.h>
#include <d3d11.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace brp::ui {

// Thin Direct2D + DirectWrite wrapper. Everything is in pixels; the renderer
// assumes a single-threaded D2D factory and a 96 DPI virtual surface.
class Renderer {
public:
    bool Init(ID3D11Device* device);

    // Recreates the D2D target from the swapchain backbuffer. Call after every
    // ResizeBuffers.
    bool BindSwapchain(ID3D11Texture2D* backbuffer);

    // Drops the D2D reference to the backbuffer. MUST be called before
    // IDXGISwapChain::ResizeBuffers, or the buffers stay referenced and the
    // resize fails with DXGI_ERROR_INVALID_CALL.
    void ReleaseTarget();

    void BeginFrame();
    void EndFrame();

    // Global UI scale. All drawing is in a virtual 1600x900-ish space; on a
    // 1440p/4K backbuffer the transform keeps text and hit areas readable.
    void SetScale(float scale) { scale_ = scale > 0.0f ? scale : 1.0f; }
    float scale() const { return scale_; }

    void FillRect(const D2D1_RECT_F& rect, D2D1_COLOR_F color);
    void FillRounded(const D2D1_RECT_F& rect, float radius, D2D1_COLOR_F color);
    void StrokeRounded(const D2D1_RECT_F& rect, float radius, D2D1_COLOR_F color, float stroke);
    void FillCircle(float cx, float cy, float radius, D2D1_COLOR_F color);
    void FillTriangle(D2D1_POINT_2F a, D2D1_POINT_2F b, D2D1_POINT_2F c, D2D1_COLOR_F color);
    void Line(D2D1_POINT_2F a, D2D1_POINT_2F b, D2D1_COLOR_F color, float stroke);
    void StrokeCircle(float cx, float cy, float radius, D2D1_COLOR_F color, float stroke);
    void StrokeArc(float cx, float cy, float radius, float start_radians, float sweep_radians,
                   D2D1_COLOR_F color, float stroke);

    void FillLinearV(const D2D1_RECT_F& rect, D2D1_COLOR_F top, D2D1_COLOR_F bottom);
    void FillRadial(float cx, float cy, float radius, D2D1_COLOR_F inner, D2D1_COLOR_F outer);

    void Text(const wchar_t* text, const D2D1_RECT_F& rect, float size, D2D1_COLOR_F color,
              DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL,
              DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING,
              DWRITE_PARAGRAPH_ALIGNMENT valign = DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    // Same as Text, but truncates with an ellipsis instead of clipping mid-glyph.
    void TextEllipsis(const wchar_t* text, const D2D1_RECT_F& rect, float size,
                      D2D1_COLOR_F color,
                      DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL,
                      DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING);
    float TextWidth(const wchar_t* text, float size,
                    DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL);

    void PushRoundedClip(const D2D1_RECT_F& rect, float radius);
    void PopClip();

private:
    IDWriteTextFormat* Format(float size, DWRITE_FONT_WEIGHT weight,
                              DWRITE_TEXT_ALIGNMENT align,
                              DWRITE_PARAGRAPH_ALIGNMENT valign);

    Microsoft::WRL::ComPtr<ID2D1Factory1> factory_;
    Microsoft::WRL::ComPtr<ID2D1Device> device_;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> dc_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> target_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_;
    Microsoft::WRL::ComPtr<IDWriteFactory> dwrite_;

    struct LayerEntry {
        Microsoft::WRL::ComPtr<ID2D1Layer> layer;
        Microsoft::WRL::ComPtr<ID2D1RoundedRectangleGeometry> geometry;
    };
    std::vector<LayerEntry> layers_;

    std::unordered_map<uint64_t, Microsoft::WRL::ComPtr<IDWriteTextFormat>> formats_;
    Microsoft::WRL::ComPtr<IDWriteFontCollection> font_collection_;
    std::wstring body_font_ = L"Noto Sans";
    std::wstring display_font_ = L"Noto Sans Display";
    float scale_ = 1.0f;
};

}  // namespace brp::ui
