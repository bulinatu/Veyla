# 15 — Frame generation (NVIDIA VFG)

Status: **working, opt-in, 2× (60 → 120 fps)**, verified on an RTX 4090 with a
live PS5 stream. The runtime is proprietary and is **not distributed** with this
repository.

---

## Why not "DLSS Frame Generation" (DLSS-G)?

DLSS-G is an **engine-side** feature. It needs, for every rendered frame:

- depth buffer,
- per-pixel motion vectors,
- camera jitter/projection matrices,
- a D3D12/Streamline swapchain inside the game's render loop,
- an RTX 40/50 GPU.

A decoded Remote Play frame is a finished 2D image. There is no depth buffer and
no motion vectors to feed DLSS-G. Magpie's experimental fork works around this
by estimating optical flow and feeding DLSS-G fake guidance, but that path is
D3D12/NGX/Streamline-heavy and its own documentation calls the quality
"estimated", not native.

For **video** NVIDIA provides the right tool: **Video Frame Generation (VFG)**
in the Maxine Video Effects SDK. It takes two consecutive SDR frames and
synthesizes intermediate frames with NVIDIA's video models and optical flow.
This is the same generator Magpie exposes as the **NVIDIA VFG** effect. We ship
that path: it is simpler, purpose-built for 2D content, and higher quality than
fake-motion-vector DLSS-G on a video stream.

---

## Requirements

| Item | Requirement |
|---|---|
| GPU | NVIDIA Ada (SM 8.9) or Blackwell (10.x / 12.x); RTX 4090 verified |
| OS | Windows x64 |
| Runtime | NVIDIA Video Effects (Maxine) 1.3.0 runtime |
| Source | SDR 8-bit RGBA/BGRA; 1080p stream |

The runtime is proprietary. It is loaded from disk at runtime and is **not**
committed or redistributed here:

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
`<exe>/../../third_party/nvidia-vfx/runtime` (for the build tree). Obtain the
files from the NVIDIA VFX SDK, or copy them from a Magpie Experimental install
(its `Magpie-Experimental-x64` folder contains an exact 1.3.0 set). The helper
does the copy:

```powershell
.\scripts\install-vfx-runtime.ps1 -Source "C:\path\to\Magpie-Experimental-x64"
# optional, for a packaged build:
.\scripts\install-vfx-runtime.ps1 -Source ... -AppDir ".\build\Release"
```

The user is responsible for the NVIDIA license; keep the vendor license files
next to the runtime.

---

## Pipeline

```
decoded RGBA8 frame (CPU)
  -> D3D11 input texture (RGBA8, 4-slot ring)
  -> CUDA import (NvCVImage_MapResource + Transfer)
  -> VFG effect: previous + current, FrameMultiplier=M, FrameIndex=1..M-1
  -> CUDA export to D3D11 output texture (6-slot ring)
  -> existing upscaler (FSR1/Lanczos) -> render target -> display
```

Implementation: `src/media/nv_vfx.{h,cpp}`. Every VFX/CUDA call runs with the
CUDA primary context current; the D3D11 immediate context is flushed before each
import and the CUDA stream is synchronized after each export.

The first source frame is shown as-is. Each later frame produces one generated
frame (2×) which is presented first, followed by the real frame on the next
presentation tick. Presentation runs at **120 Hz** while active:
`App::present_hz()` feeds the main loop, which uses the high-resolution waitable
timer (a normal `sleep_for` would cap the loop near 64 Hz).

---

## Measured (RTX 4090, 1920×1080, Medium model)

| Metric | Result |
|---|---|
| Generated frames | 60 / s (one per source frame) |
| Generation time | 6–9 ms per generated frame |
| Presentation | 120 fps actual, stable |
| GPU post-process | ~0.1 ms (EASU to 1440p target) |
| Added latency | one presentation interval, ~8.3 ms at 2× |
| Input latency | unchanged (controller thread is independent) |

The isolated probe (`vfx_probe`) renders two synthetic frames at 1080p and saves
the original / generated / next frame as BMP. Medium model: ~10.9 ms for the
first pair incl. transfer, ~6.2 ms per later generated frame. A 4× request
(three generated frames in one burst) takes ~18.6 ms, which is over the 16.7 ms
source budget in the synchronous design; that is why only 2× is exposed.

---

## Settings and behaviour

- `Settings → Display → Frame generation`: **Off** / **NVIDIA VFG 2x (120 fps)**.
- Status line shows `NVIDIA VFG active - 60 -> 120 fps - last X ms`.
- Streaming HUD: `VFG 2x gen N`.
- If the runtime is missing or initialization fails, the app falls back to the
  normal 1080p60 path and explains why in Settings. No crash, no silent change.
- Requires an SDR 8-bit stage. Our decoded stream is SDR; the HDR10 PQ encode
  happens after upscaling, so HDR output still works with VFG.

---

## Not implemented / next

1. **3× / 4× at 180/240 Hz**: synchronous generation bursts (2–3 runs) do not
   fit a 16.7 ms source interval. Needs a worker thread or a per-tick
   "generate one intermediate" state machine. Measured 3× synchronous: 53 fps
   presentation, so it is deliberately not exposed.
2. **Async generation**: move import/run/export to a worker with a shared D3D11
   texture ring and a fence; would also remove the 6–9 ms stall on the arrival
   tick of the current 2× path.
3. **Model choice**: Low/Medium verified; Medium is used. High was retired by
   NVIDIA in VFG 1.3.0.
4. **Scene-cut handling**: automatic shot detection is enabled by the SDK; a
   generated duplicate at a hard cut is currently harmless but not suppressed.
5. **DLSSNR** (`nvngx_dlssnr.dll`) is neural rendering/upscaling, not frame
   generation. It is not used here; the VFG path does not need it.

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

Logs that indicate a problem:

- `invalid resource handle` → CUDA context not current during map (was fixed by
  pushing the context in `PushFrame`).
- `NVIDIA VFG needs an Ada or Blackwell GPU` → wrong GPU.
- `NVIDIA VFX runtime missing` → DLL folder not found.

---

## Licensing note

The VFX runtime is NVIDIA proprietary and is not part of this AGPL-3.0 source
tree or its releases. It is loaded as an optional, user-supplied vendor runtime;
the project does not redistribute it. Magpie's GPLv3 implementation was used as
a reference for the API shape (runtime loading, CUDA/D3D11 transfer, effect
parameters); the code here is written against NVIDIA's public VFX programming
guide.
