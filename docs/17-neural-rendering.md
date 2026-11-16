# 17 - Neural rendering (DLSSNR) - status and plan

Status: **not integrated yet.** The DLL is present, the feature works in Magpie on this
machine, but a standalone NGX host is the missing piece. This document records exactly
what is known so the next attempt does not repeat the failed experiments.

---

## What `nvngx_dlssnr.dll` is

- NVIDIA DLSS Neural Rendering (DLSSNR), file version 310.9.16, the user-supplied,
  RTX 20/30/40/50-compatible build.
- It is **not** frame generation. DLSS-G is `nvngx_dlssg.dll`; VFG is
  `nvVFXVideoFrameGeneration.dll` (see `docs/15-frame-generation.md`).
- DLSSNR is a same-resolution neural enhancement stage: tone, structure, shadow,
  reflection and glow controls, with residual/detail reconstruction.

It is not committed or redistributed: the root `.gitignore` excludes `/nvngx_*.dll`.
The file lives next to the app only for local use.

---

## Why a naive host fails

`C:\Users\Cosmin\Desktop\dlss5overlay\NGX-FINDINGS.md` (outside this repo; the user's
earlier C# probe) tested NGX init from a standalone process. Every combination failed
before NGX wrote a log line:

| Host attempt | Result |
|---|---|
| `nvngx_dlss` 310.9.16, D3D11/D3D12, feature DLL init | `0xBAD00001` FeatureNotSupported |
| `nvngx_dlssnr` 310.9.16, D3D12, app id attempts | `0xBAD00001` |
| `nvngx_dlss` 3.7.20 / 310.2.1 | `0xBAD00002` PlatformError |
| Core `nvngx.dll`/`_nvngx.dll` init | `0xC0000005` crash |

The same machine, same session, Magpie's native build logs:

```
NGXGetPathUsingQAI: Path to driverStore found using QAI: ...\nv_dispsi.inf_amd64_...
using path for models: C:\ProgramData\NVIDIA\NGX\models
DLSSNRFilter: Loaded DLSSNR: nvngx_dlssnr.dll / FileVersion: 310.9.16.0
DLSSNR STATUS: Feature=18 created=true path=signed-snippet ... depth=zero-contract
DLSSNR pass 1: frame=1 result=0x1 SEH=0x0
```

So the GPU, driver and DLL are fine; the **host integration** is the problem.

---

## What the working host does

Magpie's native C++ renderer (`src/Magpie.Core/NgxD3D12Core.*`, GPLv3) provides:

1. a native D3D12 device/queue owned by the same process as the renderer,
2. driver-store path discovery and the models directory,
3. NGX initialization through the core's signed-snippet path
   (`nvLoadSignedLibraryW`), not by calling the feature DLL directly,
4. feature 18 creation with zero depth and a captured colour texture,
5. evaluation and readback into the effect chain.

The DLSSNR feature is created successfully in that environment. The captured colour is
sufficient because no motion vectors or real depth are required for NR (feature 18);
a zero depth contract is accepted.

---

## Integration plan (not yet implemented)

A correct integration into BetterRemotePlay needs:

1. **D3D12 device + queue** on the same adapter as the D3D11 presenter (LUID match).
2. **NGX core loader** ported from Magpie's `NgxD3D12Core` (GPLv3, attribution kept):
   driver-store discovery, models path, signed-snippet init, feature search paths.
3. **D3D11 <-> D3D12 sharing** for the 1080p SDR source and the enhanced output
   (`D3D11_RESOURCE_MISC_SHARED` / shared handles; ownership ring).
4. **Feature 18 parameters** exposed in Settings:
   - preset (Default / A / B)
   - working scale (auto, 1.00, 0.75, 0.50, 0.33)
   - colour strength (auto, 1.00, 0.50, 0.00 keep hue)
   - tone / structure / shadow / reflection / glow (Magpie's DLSSNR controls)
   - passes (every frame / alternating frames)
5. **Pipeline placement**: apply DLSSNR to the decoded 1080p SDR frame **before** the
   FSR1/Lanczos upscale, then present normally. It is same-resolution; it does not
   replace the upscaler.
6. **HDR**: DLSSNR expects SDR; PQ encoding remains after upscaling.

Because the host must own a D3D12 device and NGX feature lifecycle, this is a separate
module (`media/dlssnr.*`), not a flag in the existing D3D11 presenter.

---

## Risks / open questions

- NGX feature 18 availability on the installed NGX core; Magpie's DLL/core pair is the
  known-good combination (feature DLL 310.9.16, core from the driver store).
- Per-frame evaluation cost at 1080p and the added latency; must be measured with the
  existing HUD before it is enabled by default.
- Model/preset differences between RTX 20/30/40/50; the DLL is community-compatible,
  not an official public SDK artifact.
- GPLv3/AGPL compatibility: Magpie's NGX core is GPLv3; this project is AGPL-3.0. The
  combined work must keep the notices and corresponding source.
- NVIDIA runtime redistribution is not granted; the user supplies the DLL locally.

---

## Next action

After the VFG async/worker-owned-device work, port `NgxD3D12Core` minimally, prove
feature 18 creation and one evaluation on a shared 1080p texture, then expose the
parameters above. Do not claim neural rendering before that probe passes.
