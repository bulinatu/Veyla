#pragma once

#include <d2d1_1.h>

#include <cmath>
#include <cstdint>

// Palette and small colour helpers. Values are 0xRRGGBB; alpha is separate.
namespace brp::theme {

inline bool& HdrFlag() {
    static bool hdr = false;
    return hdr;
}

// UI colours must be PQ-encoded when the swapchain is HDR10.
inline void SetHdr(bool enabled) {
    HdrFlag() = enabled;
}

inline float PqEncode(float srgb) {
    float lin = std::pow(srgb, 2.2f) * (100.0f / 10000.0f);
    const float m1 = 0.1593017578125f;
    const float m2 = 78.84375f;
    const float c1 = 0.8359375f;
    const float c2 = 18.8515625f;
    const float c3 = 18.6875f;
    const float p = std::pow(lin, m1);
    return std::pow((c1 + c2 * p) / (1.0f + c3 * p), m2);
}

inline D2D1_COLOR_F Rgba(uint32_t rgb, float alpha = 1.0f) {
    float r = static_cast<float>((rgb >> 16) & 0xFFu) / 255.0f;
    float g = static_cast<float>((rgb >> 8) & 0xFFu) / 255.0f;
    float b = static_cast<float>(rgb & 0xFFu) / 255.0f;
    if (HdrFlag()) {
        r = PqEncode(r);
        g = PqEncode(g);
        b = PqEncode(b);
    }
    return D2D1::ColorF(r, g, b, alpha);
}

inline uint32_t Scale(uint32_t rgb, float factor) {
    auto clamp8 = [](float v) {
        if (v < 0.0f) return 0;
        if (v > 255.0f) return 255;
        return static_cast<int>(v + 0.5f);
    };
    const int r = clamp8(static_cast<float>((rgb >> 16) & 0xFFu) * factor);
    const int g = clamp8(static_cast<float>((rgb >> 8) & 0xFFu) * factor);
    const int b = clamp8(static_cast<float>(rgb & 0xFFu) * factor);
    return (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) |
           static_cast<uint32_t>(b);
}

// Surfaces (dark, cold, slightly blue — PS5-adjacent without copying it).
inline constexpr uint32_t kBgTop = 0x0B1220;
inline constexpr uint32_t kBgBottom = 0x04060B;
inline constexpr uint32_t kPanel = 0x121A28;
inline constexpr uint32_t kPanelRaised = 0x1A2333;
inline constexpr uint32_t kHairline = 0x2A3444;

// Text
inline constexpr uint32_t kInk = 0xF4F7FC;
inline constexpr uint32_t kInkDim = 0xC2CBDA;
inline constexpr uint32_t kMuted = 0x7E8AA0;

// Selection
inline constexpr uint32_t kAccent = 0x5AA9FF;
inline constexpr uint32_t kAccentWarm = 0xFF7A45;
inline constexpr uint32_t kOnline = 0x57D68A;
inline constexpr uint32_t kOffline = 0xE0A23A;

}  // namespace brp::theme
