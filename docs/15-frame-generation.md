# 15 - Frame generation (NVIDIA VFG)

Status: **working, opt-in, 2x (60 -> 120 fps)**, verified on an RTX 4090 with a live
PS5 stream. The runtime is proprietary and is **not distributed** with this repo.

---

## Why not DLSS Frame Generation (DLSS-G)?

DLSS-G is an engine-side feature. It needs, for every rendered frame:

- depth buffer,
- per-pixel motion vectors,
- camera jitter/projection matrices,
- a D3D12/Streamline swapchain inside the game's render loop,
- an RTX 40/50 GPU.

A decoded Remote Play frame is a finished 2D image. There are no depth or motion
vectors to feed DLSS-G. Magpie's experimental fork works around this with estimated
optical flow and fake guidance, but its own docs call the result "estimated", not
native.

For video, NVIDIA provides **Video Frame Generation (VFG)** in the Maxine Video
Effects SDK. It takes two consecutive SDR frames and synthesizes intermediates with
NVIDIA's video models. This is the same generator Magpie exposes as "NVIDIA VFG". It
is purpose-built for 2D content and is higher quality than fake-MV DLSS-G on video.

---

## Requirements

| Item | Requirement |
|---|---|
| GPU | NVIDIA Ada (SM 8.9) or Blackwell (10.x / 12.x); RTX 4090 verified |
| OS | Windows x64 |
| Runtime | NVIDIA Video Effects (Maxine) 1.3.0 runtime |
| Source | SDR 8-bit RGBA/BGRA; 1080p stream |

The runtime is proprietary and is loaded from disk at runtime; it is not committed
or redistributed here:

```
third_party/nvidia-vfx/runtime/
  nvVFXVideoFrameGeneration.dll
  NVVideoEffects.dll
  NVCVImage.dll
  nvngxruntime.dll
  cudart64_12.dll
  nppc64_12.dll nppial64_12.dll nppicc64_12.dll nppidei64_12.dll
  nppif64_12.dll nppig64_12.dll nppim64_12.dll nppist64_12.dll nppitc64_12.dll
```

The app also searches `nvidia-vfx/runtime` next to `brp.exe`, and
`<exe>/../../third_party/nvidia-vfx/runtime` (build tree). Obtain the files from the
NVIDIA VFX SDK, or copy them from a Magpie Experimental install. The helper:

```powershell
.\scripts\install-vfx-runtime.ps1 -Source "C:\path\to\Magpie-Experimental-x64"
# optional, for a packaged build:
.\scripts\install-vfx-runtime.ps1 -Source ... -AppDir ".\build\Release"
```

The user is responsible for the NVIDIA license; keep the vendor license files next
to the runtime.

---

## Pipeline

```
decoded RGBA8 frame (CPU)
  -> D3D11 input texture (RGBA8, 4-slot ring)
  -> CUDA import (NvCVImage_MapResource + Transfer)
  -> VFG effect: previous + current, FrameMultiplier=M, FrameIndex=1..M-1
  -> CUDA export to D3D11 output texture (16-slot ring)
  -> existing upscaler (FSR1/Lanczos) -> render target -> display
```

Implementation: `src/media/nv_vfx.{h,cpp}`. Every VFX/CUDA call runs with the CUDA
primary context current; the D3D11 immediate context is flushed before each import
and the CUDA stream is synchronized after each export.

The first source frame is shown as-is. Each later frame produces one generated frame
(2x), presented first, followed by the real frame on the next presentation tick.
Presentation runs at **120 Hz** while active: `App::present_hz()` feeds the main loop,
which uses the high-resolution waitable timer (normal `sleep_for` would cap near
64 Hz).

---

## Measured (RTX 4090, 1920x1080, Medium model)

| Metric | Result |
|---|---|
| Generated frames | 60 / s (one per source frame) |
| Generation time | 6-9 ms per generated frame |
| Presentation | 120 fps actual, stable |
| GPU post-process | ~0.1 ms (EASU to 1440p target) |
| Added latency | one presentation interval, ~8.3 ms at 2x |
| Input latency | unchanged (controller thread is independent) |

The isolated probe (`vfx_probe`) renders two synthetic 1080p frames and saves the
original / generated / next frame as BMP. Medium model: ~10.9 ms for the first pair
incl. transfer, ~6.2 ms per later frame. A 4x request (three generated frames in one
burst) takes ~18.6 ms, over the 16.7 ms source budget in the synchronous design.

---

## Settings and behaviour

- `Settings -> Display -> Frame generation`: **Off** / **NVIDIA VFG 2x (120 fps)**.
- `Tab` during a stream opens the settings overlay; the FG change is live.
- Status line shows `NVIDIA VFG active - 60 -> 120 fps - last X ms`.
- Streaming HUD: `VFG 2x gen N` and `Decoder: <backend>`.
- If the runtime is missing or initialization fails, the app falls back to the normal
  1080p60 path and explains why in Settings. No crash, no silent change.
- VFG needs an 8-bit SDR stage. The decoded stream is SDR; PQ encoding happens after
  upscaling, so HDR output should still work (not tested together yet).

---

## Async attempt (2026-11-16) - not shipped

A same-device worker was implemented and tested: the UI uploaded the frame, a worker
thread ran CUDA import/VFG/export, and published frames to a queue. It failed with
`NvCVImage_InitFromD3D11Texture failed: invalid device ordinal` and eventually crashed
the process. Pre-initializing the wrappers on the main thread and calling CUDA
`cudaSetDevice` on the worker did not fix it.

Conclusion: CUDA/D3D11 interop on the presenter device is not safe while the
presenter's immediate context is used from another thread. The shipped path remains
synchronous 2x. The correct next attempt is a worker-owned D3D11 device on the same
adapter, with shared output textures (`D3D11_RESOURCE_MISC_SHARED`) and an ownership
ring; that avoids sharing the immediate context. It was not shipped to avoid a
crash/regression.

3x/4x are therefore not exposed. Even with async, VFG throughput at 1080p (~6 ms per
generated frame) makes 4x/240 Hz unrealistic; 3x/180 Hz would be borderline.

---

## Not implemented / next

1. **Worker-owned device async** for 3x/4x and to remove the 6-9 ms stall on the
   arrival tick of the 2x path.
2. **Model choice**: Low/Medium verified; Medium is used. High was retired in VFG
   1.3.0.
3. **Scene-cut duplicate suppression**: automatic shot detection is enabled by the
   SDK; a duplicate at a hard cut is harmless but not suppressed.
4. **HDR + VFG** and **2160p target + VFG** combinations need a dedicated test.
5. **DLSSNR** (`nvngx_dlssnr.dll`) is neural rendering/upscaling, not frame
   generation. See `docs/17-neural-rendering.md`; it is not used by this path.

---

## Verification

```powershell
.\scripts\build.ps1 -Target vfx_probe
.\build\Release\vfx_probe.exe
# writes build/shots/vfx-a.bmp, vfx-generated.bmp, vfx-b.bmp

$env:BRP_FG_PROBE=1
.\build\Release\brp.exe
# logs: [vfx] initialized 1920x1080, model 1
#       [fg] 2x target 120 Hz, actual 120 fps, generated N, last X ms
```

Problem markers:

- `invalid resource handle` -> CUDA context not current during map.
- `NVIDIA VFG needs an Ada or Blackwell GPU` -> wrong GPU.
- `NVIDIA VFX runtime missing` -> DLL folder not found.

---

## Relationship with RTX VSR

VFG and RTX VSR are mutually exclusive in the current pipeline: VFG needs 1080p
input/output pairs, while VSR produces a 1440p/2160p output and cannot be run on
every generated frame within a 60 fps budget. Selecting one in
`Settings > Display` turns the other off. See `docs/17-neural-rendering.md`.

---

## Licensing note

The VFX runtime is NVIDIA proprietary and is not part of this AGPL-3.0 source tree or
its releases. It is loaded as an optional, user-supplied vendor runtime. Magpie's
GPLv3 implementation was used as a reference for the API shape (runtime loading,
CUDA/D3D11 transfer, effect parameters); the code here is written against NVIDIA's
public VFX programming guide.
