# 07 — Bench log

Numbers, not adjectives. Every entry records machine, method, and what it means for the design.

---

## Run 1 — 2026-11-16 — presentation + shader upscale baseline

**Machine**

| Part | Value |
|---|---|
| GPU | NVIDIA GeForce RTX 4090 (24 GB) |
| CPU | AMD Ryzen 9 9950X3D (16C/32T) |
| RAM | 63.6 GB |
| OS | Windows 10.0.26300 |
| Toolchain | MSVC 19.4x, CMake 4.4.4, Windows SDK 10.0.22621 |
| Present path | D3D11 flip model (`FLIP_DISCARD`), 2 buffers, `ALLOW_TEARING`, vsync off |

**Method**

`brp-bench` generates a synthetic 1080p frame on the GPU (compute shader), upscales it to the output resolution (pixel shader), and presents. 800 frames per cell. GPU time is measured with `D3D11_QUERY_TIMESTAMP` + `TIMESTAMP_DISJOINT` pairs, reported as p50/p95/p99. This measures **generation + upscale submit**, not network or decode.

**GPU time, ms**

| Output | Mode | p50 | p95 | p99 |
|---|---|---|---|---|
| 1920×1080 | bilinear | 0.055 | 0.279 | 0.774 |
| 1920×1080 | Catmull-Rom | 0.080 | 0.393 | 0.810 |
| 1920×1080 | Catmull-Rom + unsharp | 0.087 | 0.386 | 0.711 |
| 2560×1440 | bilinear | 0.063 | 0.310 | 0.784 |
| 2560×1440 | Catmull-Rom | 0.098 | 0.422 | 0.764 |
| 2560×1440 | Catmull-Rom + unsharp | 0.108 | 0.428 | 0.830 |
| 3840×2160 | bilinear | 0.083 | 0.391 | 0.739 |
| 3840×2160 | Catmull-Rom | 0.148 | 0.495 | 0.910 |
| 3840×2160 | Catmull-Rom + unsharp | 0.187 | 0.554 | 0.998 |
| 7680×4320 | Catmull-Rom + unsharp | 0.612 | 1.504 | 1.711 |

**What this means**

1. **The shader path is free.** At 4K the whole chain costs 0.19 ms p50. A 60 fps frame budget is 16.7 ms. That is ~1.1% of the budget.
2. **There is ~15 ms of headroom for a neural upscaler at 4K** and still stay inside 60 fps. That budget is what a TensorRT super-resolution model would spend — this run says the rest of the pipeline will not get in its way.
3. **Scaling is near-linear with output pixels** (4K → 8K is 3.4× time for 4× pixels), so the measurements are real and not query artifacts.
4. **Present cost is not in these numbers.** This is submit-side GPU time. Present queue depth and pacing land in Phase 2 alongside the decoder.

---

## Run 1b — image metrics (why naive sharpening is wrong)

`tools/imgstat.py` reads the same frames and reports detail and clipping. Synthetic source is known: dark field, saturated magenta checker, bright disc, white strokes.

| Image | Sharpness/MP | Mean luma | Crushed % | Blown % |
|---|---|---|---|---|
| bilinear | 9.78 M | 34.7 | 0.000 | 0.000 |
| Catmull-Rom | 11.81 M | 34.8 | **2.03** | 0.000 |
| Catmull-Rom + unsharp 0.35 | 15.22 M | 35.4 | **10.33** | 0.000 |

**Finding:** unsharp masking buys 30% more apparent detail and destroys 10% of the frame into pure black. Catmull-Rom alone already rings 2% into black. That is the "crushed shadows" failure mode in another costume.

**Decision:** the shipped upscaler is **not** naive unsharp. Use a contrast-adaptive sharpener (CAS/RCAS-style, which limits the halo to the local contrast) or a neural upscaler (VSR/ESRGAN), both of which keep dark detail. Mode 2 stays in the bench as the negative control it is.

Verification screenshot ([full size](images/bench-mode2-1440p.png)): checker detail, smooth disc edge, stroke band, luminance sweep. If it renders like this, the device, swapchain, and shader chain are all correct.

![bench output at 2560x1440](images/bench-mode2-1440p.png)

---

## Upscale target 1440p / 2160p (2026-11-16)

Two-pass path: 1080p source → EASU to the chosen target → Catmull-Rom resolve
to the window/display (aspect-preserving, no stretch). When the target equals
the output size the second pass is skipped.

Live numbers (PS5 stream, EASU mode 4, `BRP_PAD_PROBE=1`, HUD):

| Output | Render target | GPU p50 | GPU p95 | FPS |
|---|---|---|---|---|
| 1600x900 window | 3840x2160 | 0.50 ms | 0.17 ms | 59.9 |
| 2560x1440 fullscreen | 3840x2160 | 0.15 ms | 0.19 ms | 59.9 |
| 1600x900 window | 2560x1440 | 0.09 ms | 0.10 ms | 59.9 |

(Percentiles invert when the sample count in the window is tiny; treat both as
"well under 1 ms".) The 4K target costs less than a millisecond, so it is safe to
leave on even on integrated GPUs — but it does not create detail; the source is
still 1080p. F12 captures the target resolution.

---

## Controller loop pacing (2026-11-16)
`Sleep`/`sleep_for` on Windows quantises to the 15.6 ms system tick, so a
"250 Hz" loop would really run at ~64 Hz. The pad thread now uses
`CreateWaitableTimerExW(CREATE_WAITABLE_TIMER_HIGH_RESOLUTION)` and MMCSS
("Games" task), with a drift-corrected absolute deadline.

Measured with `BRP_PAD_PROBE=1` at a 1000 Hz target over 5 seconds:

| Second | Loop iterations |
|---|---|
| 1 | 1001 |
| 2 | 936 |
| 3 | 1000 |
| 4 | 936 |
| 5 | 1000 |

The 936 values are the probe window straddling a timer boundary, not missed
deadlines. Effective ceiling is the controller's report rate: USB DualSense
reports at 1000 Hz, Bluetooth at ~250 Hz, so on Bluetooth values above 250 Hz
only affect polling/output timing, not new input samples.

---

## Reproduction

```powershell
.\scripts\build.ps1
.\scripts\bench.ps1            # full matrix, CSV out
python tools\imgstat.py build\shots\m0.bmp build\shots\m1.bmp build\shots\m2.bmp
```

---

## Open items

- [ ] Add HEVC 1080p60 decode to the loop (NVDEC) so the number includes the real first stage.
- [ ] Instrument present queue depth and drop-late behaviour.
- [ ] Implement CAS/NIS and re-run imgstat to confirm clipping stays under 1%.
- [ ] Benchmark a TensorRT super-resolution model at 1080p→2160p and record its ms/frame.
- [ ] Same for RIFE interpolation (cost of one generated frame).
