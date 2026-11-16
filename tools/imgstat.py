#!/usr/bin/env python3
"""Image metrics for the pipeline bench.

Reads a 32-bit BMP written by brp-bench --shot and reports numbers that map to
the product requirements:

  sharpness   mean |gradient| of luma, normalised per megapixel.
              Higher = more detail preserved by the scaler.
  crushed     % of pixels with luma <= 5/255   (black detail lost)
  blown       % of pixels with luma >= 250/255 (highlight detail lost)
  clipping    crushed + blown. "Washed out" and "crushed" are usually this.

The synthetic source is known: it has a dark field, a saturated magenta
checker, a bright disc, and white strokes. So any correct pipeline should show
low clipping and a sharpness that increases from bilinear -> catmull -> sharpen.

Usage:
    python tools/imgstat.py shot.bmp [shot2.bmp ...]
"""

from __future__ import annotations

import struct
import sys


def read_bmp32(path: str) -> tuple[int, int, list[tuple[float, float, float]]]:
    with open(path, "rb") as f:
        data = f.read()

    if data[:2] != b"BM":
        raise ValueError(f"{path}: not a BMP")

    pixel_offset = struct.unpack_from("<I", data, 10)[0]
    width = struct.unpack_from("<i", data, 18)[0]
    height = struct.unpack_from("<i", data, 22)[0]
    bpp = struct.unpack_from("<H", data, 28)[0]
    if bpp != 32:
        raise ValueError(f"{path}: expected 32bpp, got {bpp}")

    stride = width * 4
    pixels: list[tuple[float, float, float]] = []

    # Our writer emits bottom-up rows.
    for y in range(height - 1, -1, -1):
        row = pixel_offset + y * stride
        for x in range(width):
            o = row + x * 4
            b, g, r = data[o], data[o + 1], data[o + 2]
            pixels.append((r, g, b))

    return width, height, pixels


def luma(r: float, g: float, b: float) -> float:
    return 0.2126 * r + 0.7152 * g + 0.0722 * b


def metrics(path: str) -> dict[str, float]:
    width, height, px = read_bmp32(path)
    n = width * height

    total_grad = 0.0
    crushed = 0
    blown = 0
    mean_luma = 0.0

    for i, (r, g, b) in enumerate(px):
        y = luma(r, g, b)
        mean_luma += y
        if y <= 5:
            crushed += 1
        elif y >= 250:
            blown += 1

        x = i % width
        if x + 1 < width:
            nr, ng, nb = px[i + 1]
            total_grad += abs(y - luma(nr, ng, nb))
        if i + width < n:
            nr, ng, nb = px[i + width]
            total_grad += abs(y - luma(nr, ng, nb))

    megapixels = n / 1_000_000.0
    return {
        "width": float(width),
        "height": float(height),
        "sharpness_per_mp": total_grad / megapixels,
        "mean_luma": mean_luma / n,
        "crushed_pct": 100.0 * crushed / n,
        "blown_pct": 100.0 * blown / n,
        "clipping_pct": 100.0 * (crushed + blown) / n,
    }


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print(__doc__)
        return 2

    rows = []
    for path in argv[1:]:
        m = metrics(path)
        rows.append((path, m))

    header = f"{'image':<34}{'sharp/MP':>10}{'mean':>8}{'crushed%':>10}{'blown%':>9}{'clip%':>8}"
    print(header)
    print("-" * len(header))
    for path, m in rows:
        print(
            f"{path:<34}"
            f"{m['sharpness_per_mp']:>10.1f}"
            f"{m['mean_luma']:>8.1f}"
            f"{m['crushed_pct']:>10.3f}"
            f"{m['blown_pct']:>9.3f}"
            f"{m['clipping_pct']:>8.3f}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
