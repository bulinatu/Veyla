# 32 - NVIDIA AI engine: implementation status

This is the honest map between the NVIDIA AI engine specification and what
BetterRemotePlay actually runs. It is a status document, not a wish list:
every "working" line was exercised on the reference RTX 4090 and has a log
behind it; every gap says why it is a gap.

## What the pipeline is

```
PS5 -> RTP -> FFmpeg / NVDEC -> RGBA frame
    -> optional DLSS SR (NGX, D3D12) or FSR1/Lanczos/Catmull-Rom
    -> optional DLSS NR (NGX) on 1080p
    -> frame generation:
         RIFE 4.25 lite / 4.26 heavy (TensorRT, 30 -> 60)
         DLSS Frame Generation 2x (NGX worker, 60 -> 120)
         NVIDIA VFG (legacy, optional)
    -> colour / sharpen -> waitable swapchain at the display refresh
```

The UI and settings are Direct2D; video presentation is D3D11/DXGI with a
waitable swapchain. The decoder, session, audio, controller and DualSense
paths are unchanged.

## Status matrix

| Specification item | Status | Reality |
| --- | --- | --- |
| Input detection (resolution, fps, codec, HDR) | Working | Decoder metadata + stream stats; HDR10 path present. |
| Hardware decode (NVDEC) | Working | D3D11VA/D3D12VA/Vulkan/CUDA decoder choices; 8.0-9.8 ms at 1080p in the verified runs. |
| Independent neural SR | **Different choice** | DLSS SR (NGX) is the SR backend, not RTX VSR. It is a real neural reconstructor, faster and cleaner than a custom TensorRT SR at 1440p/4K. No VSR code or dependency exists. |
| Custom TensorRT SR (Real-ESRGAN etc.) | Not implemented | A CPU round-trip TensorRT SR at 1080p->2160p costs roughly 10-20 ms plus 66 MB/frame of transfers at 60 fps; it would compete with RIFE/FG for the same GPU. Kept as an optional future backend, not silently faked. |
| 1080p -> 1440p | Working | DLSS SR presets K/L/M, quality modes 33-100 %. |
| 1080p -> 2160p | Working setting | Render target 2160p is supported; the reference display is 1440p, so 4K was not visually validated on this desk. |
| 30 -> 60 frame generation | Working | RIFE lite 17.7 ms / heavy 23-25 ms per generated frame; NVIDIA DLSS-G also reaches 60 (2x) and is verified drop-free. |
| 60 -> 120 frame generation | Working | DLSS-G 2x: 119.8-120.2 fps, 0 drops/late/stalls, with DLAA SR in the same run. |
| SR + FG simultaneously | Working | 1080p60 DLAA -> DLSS-G -> 120 fps; 1080p30 DLAA -> 60 fps. Verified 20+ s each. |
| NvOFFRUC / NVIDIA Optical Flow SDK backend | Not implemented | The hardware OFA path needs the Optical Flow SDK, D3D/CUDA interop and a warp/occlusion shader. DLSS-G already uses the GPU's motion estimation internally, and RIFE covers 30->60; a separate NVOF backend is a research item, not shipped. |
| Hybrid AI interpolation | Not implemented | Would combine NVOF with a refinement network; the quality/effort trade-off does not beat RIFE heavy today. |
| Official DLSS Frame Generation | Working, honestly labelled | Uses the NeuralScreen worker with estimated motion (no game depth/motion vectors); the UI calls it DLSS FG and does not pretend it is game-engine DLSS-G. The runtime is **2x only**. |
| Adaptive multiplier | Working, capped | `FgChosenMultiplier` is capped at 2 (the runtime refuses 3-6x). 30 fps content -> 60, 60 fps content -> 120. No oscillation. |
| Frame pacing | Working | Waitable swapchain at 240 Hz; RIFE one due frame per tick; DLSS-G paced by the worker overlay. `late=0 stalls=0 miss=0` in the verified runs. |
| Input latency | Measured, not claimed | Decode ~8 ms, DLSS SR 0.74 ms, enhance ~3.4 ms, RIFE 17.7/23-25 ms per generated frame. Total input-to-photon is not instrumented end-to-end and is not claimed. |
| Colour / HDR | Working | Existing GPU colour pipeline, PQ/HDR10 output, 8-bit SDR path. |
| Runtime diagnostics | Working | `health.log` every 30 s; optional `testing.log` per second (`fg=`, `fgm=`, `content=`, `skip=`, `ref=`, `aud=`). |
| Benchmark harness | Partial | Manual probes: `rife_probe`, `nr_fg_probe`, `vfg_pace_probe`. No automated 4-case matrix yet. |
| Device loss / long soak | Partial | Device-loss recovery exists; multi-hour soak on the 5080 is not done. |

## Verified numbers (reference 4090, 2560x1440 240 Hz display)

- **1080p30 + DLAA + DLSS-G**: 59.4-60.6 fps shown, `content=30`, `fgm=2`,
  decode 8.4 ms, DLSS 0.74 ms, enhance 3.4 ms, 0 drops/late/stalls.
- **1080p60 + DLAA + DLSS-G**: 119.8-120.2 fps shown, `content=60`, `fgm=2`,
  decode 7.9-8.6 ms, 0 drops/late/stalls. Worker log:
  `displayed 120.0 FPS (real + generated, 2x)`.
- **RIFE 4.25 lite** through `rife_probe`: 17.7 ms p50 at 1088x1920.
- **RIFE 4.26 heavy**: 23-25 ms p50, 12.4 ms GPU compute; 0.25/255 from the
  true midpoint in the blobs test.

## What was fixed in 0.4.9

- Feed every decoded frame immediately; no rigid grid slots and no duplicate
  skipping. The worker was receiving ~10-15 fps on a static menu and throttling.
- Cap DLSS-G at 2x and change it at most once every 2 s; the app no longer
  asks for 3-6x, which the runtime refuses (`0xBAD00005`).
- Keep the worker overlay top-level/topmost/no-activate instead of a child
  window. As a child it believed the app covered it and throttled to 20-90 fps.
- Hide the overlay when the app loses focus.
- Log `fgm=` in both health and testing logs.

## Deliberate non-goals

- RTX Video Super Resolution (excluded by requirement).
- Claiming 30 fps -> 120 fps with DLSS-G: the runtime is 2x only.
- Claiming reconstructed 4K contains native 4K detail.
- Calling RIFE or DLSS-G output "original PS5 frames".
- Shipping a prebuilt TensorRT engine for every GPU: engines are built per
  architecture by the one-click setup.

## Next increments, in value order

1. Automated four-case benchmark (1080p30/60 -> 1440p/2160p) writing a single
   CSV/JSON with p50/p95/p99, drops, VRAM and worker fps, so regressions are
   caught without a human watching.
2. Optional TensorRT SR backend with a compact model (for machines where DLSS
   SR is unavailable), behind an explicit "Experimental" label.
3. NVOF + warp backend only if a real quality win over RIFE is demonstrated;
   otherwise it adds risk without a visible benefit.
4. Multi-hour soak and device-loss tests on the RTX 5080.
