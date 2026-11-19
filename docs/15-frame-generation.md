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
`App::present_hz()` now follows the monitor refresh rate (240 Hz on this machine)
and feeds the main loop, which uses the high-resolution waitable timer (normal
`sleep_for` would cap near 64 Hz). The DLSS Frame Generation worker presents in its
own child window and does not share this loop.

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

- `Settings -> Performance -> Frame generation`: **Off** / **NVIDIA VFG 2x (120 fps)**.
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

## Pull pacing (smoothness)

With VFG the UI presents at 120 Hz but the console produces 60 source frames per
second. The app now asks for a new source frame only when the generated queue has
two or fewer entries, instead of on every 120 Hz tick. This fills the generated
queue instead of draining the decoder and roughly doubles generation throughput.

`vfg_pace_probe` simulates the producer/consumer cadence on synthetic frames:

```powershell
.\scripts\build.ps1 -Target vfg_pace_probe
.\build\Release\vfg_pace_probe.exe third_party\nvidia-vfx\runtime 10
# ticks 1200, presented 1199 (119.9/s), generated 599 (59.9/s), avg 9.20 ms
```

The previous cadence generated ~31 frames/s (measured live); 60/s is the correct
2x cadence and visibly smoother in motion.

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

## Relationship with RTX VSR (removed)

RTX Video Super Resolution was removed from the client. VFG and DLSS Frame Generation remain; VFG is the legacy path and requires 1080p input/output pairs.
---

## Licensing note

The VFX runtime is NVIDIA proprietary and is not part of this AGPL-3.0 source tree or
its releases. It is loaded as an optional, user-supplied vendor runtime. Magpie's
GPLv3 implementation was used as a reference for the API shape (runtime loading,
CUDA/D3D11 transfer, effect parameters); the code here is written against NVIDIA's
public VFX programming guide.

## Adaptive interpolation and output targets (19 Nov 2026)

- **Output frame rate** is now a setting: Match display, 60, 90 or 120 fps. The
  multiplier is no longer fixed. In the worker protocol the multiplier travels
  in the per-frame flags, so it can change live; that was verified on a real
  session (the worker accepted pass-through and 4x in the same run).
- **Duplicate skipping.** A 30 fps game arrives as a 60 fps stream with every
  other frame duplicated. An identical pair has nothing to interpolate, so a
  64x64 sample hash detects duplicates and they are not fed to the worker. A
  still image still feeds once every 80 ms so the worker is never starved.
- **Multiplier choice** from the measured unique-frame rate and the target:
  60 fps content to 90 or 120 fps is pass-through (no gaps to fill); 30 fps
  content to 90 fps is 3x; 30 fps content to 120 fps is 4x.
- **Shortfall guard:** if the worker reports less than 88 % of the expected
  rate, the multiplier steps down on the next decision window. Measured at
  1440p: 4x from 30 fps content sustained about 92 fps, so 3x (90) is the
  honest choice there.
- **Rates and the display.** 60 and 120 divide a 240 Hz screen; 90 does not.
  The settings page says so instead of pretending 90 is even on every display.
- HUD: `FG <worker fps> fps  content <unique fps> fps  <pass-through|Nx>`.
- Settings: frame generation is Off/On; the old 2x-6x list is gone.

### Measured on the reference machine (RTX 4090, 1440p, 240 Hz)

| Case | Result |
| --- | --- |
| 60 fps content, 120 target, 2x | 120.0 fps displayed (earlier runs and this one) |
| 30 fps content, 120 target, 4x | ~92 fps displayed |
| 30 fps content, 120 target, 3x (after the guard) | ~80 fps displayed |
| 30 fps content, 90 target, 3x | same ~80 fps; the worker, not the app, is the limit |

So duplicate skipping and the live multiplier work, but the worker cannot hold
120 fps for 30 fps content at 1440p. That is a worker limit, not a pacing bug
in the app. Worth noting: decode sat at 8-9 ms and the jitter buffer reported
29 drops during the 30 fps run, which does not happen in the 60 fps path.

### RIFE, measured

RIFE 4.25 lite (ONNX, TensorRT 10, RTX 4090, fp16, `trtexec`, static shapes):

| Resolution | GPU compute mean | p99 | Host round trip |
| --- | --- | --- | --- |
| 1080p (1088x1920) | 6.4 ms | 8.6 ms | 12.3 ms |
| 1440p (1472x2560) | 11.0 ms | 12.5 ms | 21.8 ms |

What that allows: 30 -> 60 fps at either size (one frame per 33 ms), and
30 -> 90 at 1080p (two frames per 33 ms, 13 ms); 30 -> 90 at 1440p needs 22 ms
per 33 ms window, which leaves little for decode, SR and present. 60 -> 120 at
1440p needs one frame every 8.3 ms, so it is out of reach.

RIFE is **not** wired into the app. It would need a TensorRT/CUDA/cuDNN runtime
(~2 GB), the inference wrapper, and an engine built per GPU; DLSS Frame
Generation does the same job in about 1 ms through NGX. The NVIDIA path stays
the default. RIFE remains a candidate only as an experimental 1080p engine for
30 fps games, and that is a product decision, not a missing line of code.

### The two NVIDIA methods

- **DLSS Frame Generation** (NGX, through the worker): the default. ~1 ms.
- **NVIDIA VFG** (nvidia-vfx runtime, the same family SmoovPlay offers): kept as
  the legacy alternate path in Settings > Upscaling. It is queue-based and
  presents through the parent, so it is slower to react than the NGX worker.

## RIFE through TensorRT, working probe (19 Nov 2026)

`tools/rife_probe` loads a TensorRT engine through `src/media/rife.cpp` and times
interpolation on synthetic frames. Nothing is linked against the TensorRT or
CUDA SDKs: `nvinfer_10.dll` and `cudart64_13.dll` are loaded from a directory the
user points at, and the factory symbols are resolved with `GetProcAddress`.

```
rife_probe.exe --runtime=<dir> --engine=<file.engine> [--iters=40] [--ts=0.5] [--reverse=1]
```

**Runtime requirement.** The machine needs the TensorRT 10.16 and CUDA 13
runtime DLLs. On the reference PC they come from an existing vs-mlrt install
(`...\vs-plugins\vsmlrt-cuda`), which is why the app treats them as a
user-supplied runtime, like the NVIDIA VFX runtime. Point `BRP_TENSORRT_DIR` (or
`--runtime`) at a folder containing `nvinfer_10.dll` and `cudart64_13.dll`.

**Engine requirement.** Engines are built once per GPU and shape:

```
trtexec --onnx=rife_v425lite.onnx --fp16 \
  --shapes="img0:1x3x1088x1920,img1:1x3x1088x1920,timestep:1x1x1x1" \
  --saveEngine=rife425_1080.engine
```

Headers are vendored from the TensorRT v10.16 tag (Apache-2.0) under
`third_party/tensorrt/include`, with a minimal `cuda_runtime_api.h` shim.

**Correctness.** The timestep is honoured: on a synthetic pair, `ts=0.1` returns
almost the first frame, `ts=0.9` almost the second. A real-clip quality test is
still needed.

**Latency with page-locked host buffers (RTX 4090):**

| Engine | Total p50 | p95 | p99 | Conversion |
| --- | --- | --- | --- | --- |
| 1080p (1920x1088) | **19.0 ms** | 20.4 | 20.5 | 3.0 ms |
| 1440p (2560x1472) | **34.3 ms** | 34.9 | 35.9 | 5.6 ms |

Pinning the buffers did not help much (19.4 -> 19.0 ms), so the cost is not the
PCIe staging copy: it is TensorRT inference plus the per-call transfer and
synchronisation of 180 MB at 1440p. fp16 IO tensors would halve that.

**What a 50 ms budget allows.** The user accepts up to 50 ms of added delay for
quality, so the *latency* is fine. The limit is throughput: a 30 fps source needs
one generated frame every 33.3 ms for 60 fps output, which 1080p meets (19 ms)
and 1440p just misses (34 ms); two generated frames per source (30 -> 90) need
16.6 ms each and are out of reach at either size. So RIFE's honest mode is
**1080p, 30 -> 60**, with 1440p 30 -> 60 as a stretch. NVIDIA DLSS FG stays the
engine for 120 fps.

**Still not wired into the stream.** The app does not yet build or present RIFE
frames; the probe proves the runtime path and gives the numbers. Integration
means a render thread that owns presentation pacing (which would also remove the
closed worker's 120-on-240 wobble), or feeding the existing presenter.

## RIFE in the app (19 Nov 2026)

The engine is wired in as an alternative to the NVIDIA worker:

- Settings > Upscaling > **Interpolation engine**: NVIDIA DLSS FG (fast) or
  RIFE 4.25 lite (TensorRT). The choice is saved and takes effect at the next
  session.
- `EnsureRife` finds the runtime (`BRP_TENSORRT_DIR`, else the reference PC's
  vs-mlrt folder) and the engine (`<exe>\rife\rife425_1080.engine`, or
  `BRP_RIFE_ENGINE`). If either is missing the app falls back to NVIDIA and says
  so.
- A worker thread owns interpolation; the frame loop pushes pairs and presents
  from a small queue, so inference never blocks the 240 Hz loop.
- Pairs are formed from unique frames only (duplicate detection), so a 30 fps
  game produces one generated frame per pair.
- The held frame is shown when its successor arrives, then the midpoint half an
  interval later. That is one source interval of added latency, by design.
- Audio is opened with a 40 ms buffer in RIFE mode so sound stays with the
  delayed picture (pushing silence instead caused underruns, measured 30/s).

**Measured in a live session (1080p engine, 30 fps game, 1440p target):**

```
[rife] 60 fps shown, 30 generated, 24.3 ms/frame
health: frame_p99 5.54  late 0  stalls 0  audio underruns 0
```

A still from that session shows the stream sharp at 1440p. The HUD reports
`RIFE <shown> fps  <ms>/frame`, and the settings page shows the engine size and
the measured content rate. Quality versus DLSS FG on the same scene has not been
compared side by side yet.

### Where the settings are

Settings > **Upscaling**:

| Control | What it does |
| --- | --- |
| Interpolation engine | NVIDIA DLSS FG (fast) or RIFE 4.25 lite (TensorRT) |
| RIFE output | 60 fps (one frame between each pair) or Pass-through |
| Output frame rate | NVIDIA only: Match display / 60 / 90 / 120 |
| Upscaler, Upscale target | Apply to both engines (FSR1/Lanczos/etc.) |
| Colour > Sharpening | Applies to both; the clearness control for RIFE |

RIFE has no quality slider because only one model is wired (4.25 lite). The
model is fixed; clarity comes from the upscaler, the target and sharpening.
DLSS SR is bypassed in RIFE mode: RIFE takes the 1080p stream frames.

### Why it felt like 30 fps (fixed 19 Nov 2026)

Two separate faults:

1. **Duplicate detection was exact-hash.** A 30 fps game arrives as 60 fps with
   every other frame duplicated, but the two copies can differ by a count or two
   after decoding. The hash missed them, so the app measured 60 fps of content
   and passed every frame through: on screen that is 30 fps of motion repeated
   twice, which reads as 30 fps.
   Fix: compare a 64x64 sample with a small tolerance (mean < 1.2 counts, under
   ~1.5 % of channels above 8). Now content reads ~29.8 fps and ~30 duplicates a
   second are skipped.
2. **The presentation schedule was uneven.** The held frame was shown when its
   successor arrived, and the generated midpoint only after inference (~22 ms),
   so intervals were 22 ms then 11 ms. Fix: delay the held frame by
   `max(0, inference_ms - half_interval)`, then place the midpoint exactly half
   an interval after it. Measured cadence is now **16.69 ms average** (60 fps)
   with a 20-25 ms tail.

Measured after both fixes, live session, 30 fps game:

```
[rife] 60 fps shown, 30 generated, 21.5 ms/frame,
       cadence avg 16.69 ms max 24.97 ms, content 29.8 fps, dupes 30
```

Test hooks: `BRP_RIFE_SIM30` halves a 60 fps source to exercise the 30 -> 60
path; `BRP_RIFE_FORCE_GEN` generates on every pair regardless of content.

### The 30 fps setting was ignored (fixed 19 Nov 2026)

The session always requested `CHIAKI_VIDEO_FPS_PRESET_60`, whatever the
Connection tab said. A 30 fps choice therefore still arrived at 60 fps with
every other frame duplicated. The request now follows `profile.fps`.

Measured after the fix, Connection FPS = 30, RIFE on:

```
[rife] 60 fps shown, 30 generated, 21.2 ms/frame,
       cadence avg 16.68 ms max 20.90 ms, content 30.0 fps, dupes 0
health: src=28.6  drops=0  lost=0  audio underruns=0
```

The console honours the request: 28.6 fps arrive, no duplicates, half the
frames to decode, and RIFE makes the 60. The duplicate detector still matters
for games that ignore the hint or for the 60 fps setting.

## Neural Rendering in the RIFE path (19 Nov 2026)

Settings > Upscaling > **Neural rendering (DLSS 5)** now works with either
engine. In RIFE mode the NR worker runs on the 1080p stream **before**
interpolation:

- Duplicate detection runs on the raw frame; only unique frames enter NR, so
  the worker does not waste passes on repeated frames and its output does not
  confuse the comparison.
- The NR stage is pinned to the stream size (1920x1080) when RIFE is selected,
  so its output matches the engine. `NrStageSize` returns the stream size for
  the RIFE engine.
- The worker must not enter FG present mode in this path; `EnsureNr` skips
  `EnablePresent` when `fg_engine == 1`. Without that it presented frames itself
  and RIFE received nothing (measured: 0 fps shown).
- Toggle it live from the same dropdown (Off/On); the existing Neural tab still
  holds the style, passes, tone and mask controls.

Measured with NR on (4 passes) + RIFE, 30 fps stream:

```
[rife] 60 fps shown, 30 generated, 23-29 ms/frame,
       cadence avg 16.6-16.9 ms, content 30.0 fps, dupes 0
```

The inference cost rises (22 -> 23-29 ms) because NR and RIFE share the GPU, and
one window dipped to 51 fps with a 40 ms frame. Passes and strength are the
controls for that trade-off.

## One-click setup (19 Nov 2026)

Home, top right, next to the gear: **Set up RIFE** (it turns into **RIFE ready**
when the pieces are in place). One click starts a worker:

1. Looks for TensorRT 10.16 on the PC: `BRP_TENSORRT_DIR`, `rife\runtime`,
   the vs-mlrt install, `Program Files\NVIDIA Corporation\TensorRT*`,
   `C:\TensorRT*`. The first folder with `nvinfer_10.dll` wins.
2. If nothing is installed, downloads the **official TensorRT 10.16 Windows zip**
   (1.9 GB) from
   `developer.download.nvidia.com/.../TensorRT-10.16.0.72.Windows.amd64.cuda-13.2.zip`,
   extracts only `bin\` under `rife\tensorrt` with `tar.exe`, deletes the zip and
   copies the bundled `cudart64_13.dll` next to it. NVIDIA's page needs a browser
   click for the *page*, but this file is served without one, which is what makes
   one click possible.
3. Writes `rife\runtime.txt` with the TensorRT path instead of copying about
   2 GB of DLLs. The app reads it on start; the file is stripped from packages
   because it is a path on the machine that built them.
4. Downloads the RIFE 4.25 lite model (22 MB) from Hugging Face when missing.
5. Builds `rife\rife425_1080.engine` with `trtexec --fp16` for the GPU in the
   machine, 2-4 minutes, without blocking the UI.

Measured: a clean build using only the official zip's `bin\` plus
`cudart64_13.dll` produced a 12.6 MB engine in 148 s, so **cuDNN is not needed**.
The full setup path (archive present, engine present) extracted the SDK, wrote
the marker, removed the zip and finished in about 40 s; `rife_probe` loads the
result at 18.2 ms p50.

Needs about **6 GB free** while the zip and the extracted SDK coexist. For a
different GPU, delete `rife\rife425_1080.engine` and press Set up RIFE; the
engine is architecture-specific (the shipped one is sm89, RTX 4090). The manual
link remains available for an offline install:
`https://developer.download.nvidia.com/compute/machine-learning/tensorrt/10.16.0/zip/TensorRT-10.16.0.72.Windows.amd64.cuda-13.2.zip`.

### Mismatched engine handling

If the engine was built for another TensorRT version or GPU, loading fails with a
serialization error. Instead of failing every session, the app deletes that
engine, falls back to NVIDIA for the current session, and Home offers
**Set up RIFE** again. Verified by replacing the engine with 2 MB of random
bytes: detected, removed, stream started on NVIDIA.

### 0.4.1 - another GPU (RTX 5080) and late inferences

- **Engine validation.** Setup used to skip building when
  `rife425_1080.engine` existed, even if it was the shipped sm89 engine. On an
  RTX 5080 (sm120) that meant a "ready" setup with an engine that could never
  load. Setup now opens the engine with the local runtime first; a rejected
  engine is deleted and rebuilt for the GPU in the machine. `EnsureRife` still
  removes one that fails at session start.
- **Spike headroom.** The presentation offset used the last inference time, so a
  single slow frame moved the midpoint late (a visible dropped frame on cards
  whose clocks spike). It now uses the p90 of the last 12 inferences, which
  spreads the delay slightly instead of missing a slot. The extra delay stays
  inside the 50 ms budget.
- Measured path on the reference 4090 is unchanged (cadence 16.6-16.9 ms); the
  changes are architecture-agnostic and take effect when setup rebuilds.

### 0.4.2 - gaps and fast turns (RTX 5080 reports)

Two artifact guards, both aimed at what a 5080 user described as "the head
disappears when rotating":

- **Pair-gap guard.** If the interval between pair frames jumps above 1.7x the
  running average, a frame was dropped (NR slot, jitter buffer). Interpolating
  across it invents a pose between non-consecutive frames. The app now shows
  those frames directly and restarts the pair.
- **Fast-motion guard.** The 64x64 sample already used for duplicate detection
  also yields a mean frame difference. Above 12.0 (a fast turn, where
  disocclusion makes interpolation invent background behind the character), the
  frame is shown without a generated midpoint. `BRP_RIFE_SAFE_MOTION=0` turns
  this off for comparison.
- **Health line** now records which engine ran and its numbers:
  `eng=RIFE rife=<shown fps>/<ms> cad=<ms>`, so a report from another PC says
  immediately whether RIFE or NVIDIA was active.

These do not change the 4090 cadence (16.6-16.9 ms); they trade occasional
smoothness for correctness exactly on the frames that would look wrong.

### 0.4.6 - vanishing head: blend and motion safety

Disabling interpolation entirely (0.4.2) fixed the head but showed up as dropped
frames. The guard is now a tunable pair of controls in Settings > Upscaling,
visible only with the RIFE engine:

- **RIFE blend** (Off/10/20/30/40 %, default 20): each generated frame is mixed
  with the previous real frame. RIFE invents background over a fast-moving
  subject; keeping a slice of the real frame turns a hole into a soft ghost.
  Costs about 3-5 ms per generated frame (measured 22-27 ms total at 1080p).
- **Motion safety** (default On): with a mean frame difference above 22 (a very
  fast turn), the frame is shown without a midpoint. 12 was far too low and
  stopped generation during normal camera motion.

Values are saved in the profile. If the head still flickers, raise the blend;
if drops return, lower it or turn motion safety off.
