#include "media/rgba_scale.h"

#include <algorithm>
#include <cstring>

namespace brp::media {

void DownscaleRgba(const uint8_t* src, int sw, int sh, uint8_t* dst, int dw, int dh,
                   std::vector<uint16_t>& temp) {
    if (!src || !dst || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) {
        return;
    }
    if (sw == dw && sh == dh) {
        std::memcpy(dst, src, static_cast<size_t>(sw) * sh * 4);
        return;
    }
    temp.resize(static_cast<size_t>(dw) * sh * 4);

    // Horizontal pass: sum the source columns that fall into each dst column.
    for (int y = 0; y < sh; ++y) {
        const uint8_t* row = src + static_cast<size_t>(y) * sw * 4;
        uint16_t* out = temp.data() + static_cast<size_t>(y) * dw * 4;
        for (int x = 0; x < dw; ++x) {
            const int sx0 = x * sw / dw;
            const int sx1 = std::max(sx0 + 1, (x + 1) * sw / dw);
            uint32_t r = 0;
            uint32_t g = 0;
            uint32_t b = 0;
            uint32_t a = 0;
            const uint8_t* p = row + static_cast<size_t>(sx0) * 4;
            for (int sx = sx0; sx < sx1; ++sx) {
                r += p[0];
                g += p[1];
                b += p[2];
                a += p[3];
                p += 4;
            }
            out[x * 4 + 0] = static_cast<uint16_t>(r);
            out[x * 4 + 1] = static_cast<uint16_t>(g);
            out[x * 4 + 2] = static_cast<uint16_t>(b);
            out[x * 4 + 3] = static_cast<uint16_t>(a);
        }
    }

    // Vertical pass: average the summed rows, divide by the whole box.
    for (int y = 0; y < dh; ++y) {
        const int sy0 = y * sh / dh;
        const int sy1 = std::max(sy0 + 1, (y + 1) * sh / dh);
        uint8_t* out = dst + static_cast<size_t>(y) * dw * 4;
        for (int x = 0; x < dw; ++x) {
            const int sx0 = x * sw / dw;
            const int sx1 = std::max(sx0 + 1, (x + 1) * sw / dw);
            uint32_t r = 0;
            uint32_t g = 0;
            uint32_t b = 0;
            uint32_t a = 0;
            for (int sy = sy0; sy < sy1; ++sy) {
                const uint16_t* p = temp.data() + (static_cast<size_t>(sy) * dw + x) * 4;
                r += p[0];
                g += p[1];
                b += p[2];
                a += p[3];
            }
            const uint32_t n = static_cast<uint32_t>(sx1 - sx0) * (sy1 - sy0);
            out[x * 4 + 0] = static_cast<uint8_t>((r + n / 2) / n);
            out[x * 4 + 1] = static_cast<uint8_t>((g + n / 2) / n);
            out[x * 4 + 2] = static_cast<uint8_t>((b + n / 2) / n);
            out[x * 4 + 3] = static_cast<uint8_t>((a + n / 2) / n);
        }
    }
}

}  // namespace brp::media
