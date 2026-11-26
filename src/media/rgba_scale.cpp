#include "media/rgba_scale.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace brp::media {
namespace {

// 65536 / n for the small box sizes produced by realistic stream scales.
// Anything larger falls back to a real division.
uint32_t Reciprocal16(uint32_t n) {
    static const uint32_t kTable[33] = {
        0,      65536, 32768, 21845, 16384, 13107, 10922, 9362, 8192, 7281,
        6553,   5957,  5461,  5041,  4681,  4369,  4096,  3855, 3640, 3449,
        3276,   3120,  2978,  2849,  2730,  2621,  2520,  2427, 2340, 2259,
        2184,   2114,  2048};
    return n < 33 ? kTable[n] : (65536u + n / 2u) / n;
}

}  // namespace

void DownscaleRgba(const uint8_t* src, int sw, int sh, uint8_t* dst, int dw, int dh,
                   std::vector<uint16_t>& temp) {
    if (!src || !dst || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) {
        return;
    }
    if (sw == dw && sh == dh) {
        std::memcpy(dst, src, static_cast<size_t>(sw) * sh * 4);
        return;
    }
    // The caller's scratch buffer is kept for ABI compatibility with the older
    // two-pass implementation, but this version accumulates one output row at a
    // time and needs no full-frame intermediate.
    (void)temp;

    // Column layout: exact area spans, computed once with an accumulator so
    // there is no integer division in the pixel loops. The scratch vectors are
    // thread-local: this runs once per reconstructed frame, and re-allocating
    // them on every call was pure allocator churn.
    static thread_local std::vector<int> col_start;
    static thread_local std::vector<uint8_t> col_span;
    static thread_local std::vector<uint32_t> sums;
    col_start.resize(static_cast<size_t>(dw));
    col_span.resize(static_cast<size_t>(dw));
    {
        const int base = sw / dw;
        const int extra = sw % dw;
        int pos = 0;
        int rem = 0;
        for (int x = 0; x < dw; ++x) {
            int span = base;
            rem += extra;
            if (rem >= dw) {
                rem -= dw;
                ++span;
            }
            if (span < 1) {
                span = 1;
            }
            if (span > 255) {
                span = 255;
            }
            col_start[static_cast<size_t>(x)] = pos;
            col_span[static_cast<size_t>(x)] = static_cast<uint8_t>(span);
            pos += span;
            if (pos > sw) {
                pos = sw;
            }
        }
    }

    sums.resize(static_cast<size_t>(dw) * 4);
    for (int y = 0; y < dh; ++y) {
        // Row span for this output row.
        int row_start = y * sh / dh;
        int row_end = (y + 1) * sh / dh;
        if (row_end <= row_start) {
            row_end = row_start + 1;
        }
        if (row_start >= sh) {
            row_start = sh - 1;
        }
        if (row_end > sh) {
            row_end = sh;
        }
        const uint32_t rows = static_cast<uint32_t>(row_end - row_start);

        std::fill(sums.begin(), sums.end(), 0u);
        for (int sy = row_start; sy < row_end; ++sy) {
            const uint8_t* srow = src + static_cast<size_t>(sy) * sw * 4;
            for (int x = 0; x < dw; ++x) {
                const uint8_t* p = srow + static_cast<size_t>(col_start[static_cast<size_t>(x)]) * 4;
                const int n = col_span[static_cast<size_t>(x)];
                uint32_t r = 0;
                uint32_t g = 0;
                uint32_t b = 0;
                uint32_t a = 0;
                if (n == 1) {
                    r = p[0];
                    g = p[1];
                    b = p[2];
                    a = p[3];
                } else if (n == 2) {
                    r = static_cast<uint32_t>(p[0]) + p[4];
                    g = static_cast<uint32_t>(p[1]) + p[5];
                    b = static_cast<uint32_t>(p[2]) + p[6];
                    a = static_cast<uint32_t>(p[3]) + p[7];
                } else {
                    for (int i = 0; i < n; ++i) {
                        r += p[0];
                        g += p[1];
                        b += p[2];
                        a += p[3];
                        p += 4;
                    }
                }
                uint32_t* q = sums.data() + static_cast<size_t>(x) * 4;
                q[0] += r;
                q[1] += g;
                q[2] += b;
                q[3] += a;
            }
        }

        uint8_t* drow = dst + static_cast<size_t>(y) * dw * 4;
        for (int x = 0; x < dw; ++x) {
            const uint32_t n = static_cast<uint32_t>(col_span[static_cast<size_t>(x)]) * rows;
            const uint32_t inv = Reciprocal16(n);
            const uint32_t* q = sums.data() + static_cast<size_t>(x) * 4;
            const size_t o = static_cast<size_t>(x) * 4;
            drow[o + 0] = static_cast<uint8_t>((q[0] * inv + 32768u) >> 16);
            drow[o + 1] = static_cast<uint8_t>((q[1] * inv + 32768u) >> 16);
            drow[o + 2] = static_cast<uint8_t>((q[2] * inv + 32768u) >> 16);
            drow[o + 3] = static_cast<uint8_t>((q[3] * inv + 32768u) >> 16);
        }
    }
}

}  // namespace brp::media
