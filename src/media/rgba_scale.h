#pragma once

#include <cstdint>
#include <vector>

namespace brp::media {

// Area-average RGBA8 downscale (separable box filter). `temp` is scratch space
// of dw * sh * 4 uint16 values; the caller keeps it to avoid reallocating.
void DownscaleRgba(const uint8_t* src, int sw, int sh, uint8_t* dst, int dw, int dh,
                   std::vector<uint16_t>& temp);

}  // namespace brp::media
