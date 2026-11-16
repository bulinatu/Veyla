# 17 - Neural rendering: RTX VSR shipped, DLSSNR host proven

Status:

- **NVIDIA RTX Video Super Resolution (VSR): implemented and verified live** on the
  RTX 4090 at 1080p -> 1440p and 1080p -> 2160p. It is the shipped neural upscaler.
- **DLSS Neural Rendering (`nvngx_dlssnr.dll`): host proven, not integrated.** Init,
  feature 18 creation and evaluation now run in a standalone probe on this machine
  (`tools/dlssnr_probe`). The remaining work is the residual composite/temporal path
  and app integration; the exact working recipe is below.

---

## RTX VSR (shipped)

VSR is an AI upscaler/denoiser exposed by NVIDIA's Video Effects runtime as the
`VideoSuperRes` effect. It uses the same proprietary runtime as VFG:
`NVVideoEffects.dll`, `NVCVImage.dll`, `nvngxruntime.dll`, plus the effect and
model DLLs `nvVFXVideoSuperRes.dll` and `nvngx_vsr.dll`.

Pipeline:

```
HEVC -> hw decode -> RGBA8 (BT.709/2020, limited range)
     -> RTX VSR (1080p -> render target: 1440p or 2160p)
     -> present (presenter source_final = no second upscale)
```

Quality levels exposed in `Settings > Display > Neural upscaling (RTX VSR)`:

| UI | NvVFX `QualityLevel` |
|---|---|
| AI Low / Medium / High / Ultra | 1 / 2 / 3 / 4 |
| AI Streaming Medium / Ultra | 21 / 23 |
| AI High (recommended default) | 3 |

Strength is fixed at 1.0. Input and output are 8-bit RGBA today; the effect also
accepts 10-bit RGB10A2 (`ImageEncodingMode = 1`) and that becomes useful once the
decoder keeps a 10-bit surface (currently swscale writes RGBA8).

Measured on the RTX 4090 (driver 32.0.16.2124), live PS5 stream, D3D12VA decode:

| Path | VSR time | App frame rate |
|---|---|---|
| 1080p -> 1440p, quality 3 | 7.5 - 9.6 ms | 54 - 56 fps |
| 1080p -> 2160p, quality 3 | 9.9 - 11.2 ms | 54 - 56 fps |
| 1080p -> 2160p, quality 2 | 9.3 - 9.7 ms | 58.7 fps |

`vsr_probe` isolates the effect (no decode, no socket) and writes
`build/shots/vsr-input.bmp` + `vsr-output.bmp`:

```powershell
.\scripts\build.ps1 -Target vsr_probe
.\build\Release\vsr_probe.exe third_party\nvidia-vfx\runtime 3 2560 1440
```

### VSR vs VFG are mutually exclusive

VFG needs a 1080p input pair and produces 1080p generated frames; VSR needs an
output larger than its input. Running VSR on every generated frame would need
`120 x ~8 ms` of GPU per second, which does not fit. The UI therefore turns one
off when the other is enabled:

- selecting RTX VSR turns frame generation off (and writes `frame_gen: 0`);
- selecting NVIDIA VFG 2x turns VSR off (and writes `vsr: 0`).

The setting is persisted as `"vsr"` in `session.json`.

### Failure handling

- Runtime lookup: `<exe>/nvidia-vfx/runtime`, `<exe>/nvidia-vfx`,
  `<exe>/../../third_party/nvidia-vfx/runtime`, `./third_party/nvidia-vfx/runtime`.
  Both `nvVFXVideoSuperRes.dll` and `nvngx_vsr.dll` must exist.
- Init failure (missing DLLs, no NVIDIA GPU, target smaller than the stream) leaves
  the regular upscaler active and shows the reason in Settings > Display.
- A mid-session `Process` failure disables VSR, persists `vsr: 0` and immediately
  falls back to `SetSourceFrame` so the stream never freezes.

---

## DLSSNR (host proven, not integrated)

### Working recipe (verified 2026-11-16)

`tools/dlssnr_probe` now completes Init, `CreateFeature(18)` and
`EvaluateFeature(18)` on the RTX 4090. The non-obvious parts:

1. **Load the core before creating any D3D device.** Read
   `HKLM\SOFTWARE\NVIDIA Corporation\Global\NGXCore\FullPath`, then
   `LoadLibraryExW(<dir>\_nvngx.dll, nullptr, 0)`. Loading it after
   `D3D12CreateDevice` crashed inside the core.
2. **Initialize the core by application id, not by project id.**
   `NVSDK_NGX_D3D12_Init(0x0876232C, exeDir, device12, NVSDK_NGX_Version_API)` works.
   `NVSDK_NGX_D3D12_Init_ProjectID(Magpie's GUID, ...)` consistently crashed in
   `_nvngx.dll` on this driver (0xC0000005, `r11=0xCB00000015`). The app id is the
   same value as the custom-engine CMS id (876232C).
3. **Load `nvngx_dlssnr.dll` directly and patch its caller check.** The signed
   snippet calls `GetModuleFileNameW` on its caller and only accepts `"nvngx.dll"`.
   The probe finds the DLL's `GetModuleFileNameW` IAT slot and replaces it with a
   hook that returns `L"nvngx.dll"` when `module == GetModuleHandleEx(FROM_ADDRESS,
   our_hook)`, otherwise calls the original.
4. Resolve `NVSDK_NGX_D3D12_Init_Ext`, `..._CreateFeature`, `..._EvaluateFeature`,
   `..._ReleaseFeature` from the snippet and call
   `Init_Ext(0x0876232C, exeDir, device12, version, nullptr)`.
   Allocate parameters from the **core**
   (`NVSDK_NGX_D3D12_AllocateParameters`).
5. Creation parameters: `DLSSNR.Width/Height/Input*/Output*`,
   `DLSSNR.Output.Width/Height`, `DLSSNR.Upscaling=0`, `DLSSNR.Scale=1`,
   `DLSSNR.ScalingRatio=1`, `DLSSNR.Hint.Render.Preset`,
   `NVSDK_NGX_Parameter_Width/Height`, `PerfQualityValue=Balanced`,
   `CreationNodeMask=VisibilityNodeMask=1`.
   **Also set `DLSSNRComputeScalingRatioCallback`** to a function that writes
   `DLSSNR.ScalingRatio=1`; without it evaluation returns success but never writes
   the output texture.
6. Evaluation: bind `DLSSNR.Color`, `DLSSNR.Output`, `DLSSNR.MVec`, `DLSSNR.Depth`
   plus the four `*Subrect*` groups, `MVecScaleX/Y=1`, `DepthInverted=1`,
   `Enabled=1`, `Reset=1` on the first frame, style/intensity/tone/structure.
   Transition Color/MVec/Depth `COMMON -> NON_PIXEL_SHADER_RESOURCE`, Output
   `COMMON -> UNORDERED_ACCESS`, evaluate, then back to `COMMON`.
7. **Wait on the D3D12 fence value on the CPU before the D3D11 readback.** A
   D3D11 `ID3D11DeviceContext4::Wait` on the shared fence alone raced: the probe
   sometimes read the pre-evaluation texture. Spinning until
   `ID3D12Fence::GetCompletedValue() >= value`, then copying through D3D11, makes
   the result deterministic.
8. Shared textures: D3D11 `R8G8B8A8_UNORM` with
   `D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE`,
   `BIND_SHADER_RESOURCE | BIND_UNORDERED_ACCESS`, exported with
   `IDXGIResource1::CreateSharedHandle` and opened with
   `ID3D12Device::OpenSharedHandle`. Guides are `R16G16_FLOAT` (motion) and
   `R32_FLOAT` (depth), as in Magpie's zero-guidance provider.

### What still blocks app integration

The raw feature output is 1920x1080 and is **not the final image** used by Magpie.
With neutral settings and zero guides it is a pass-through copy; with synthetic
non-zero guides the probe shows the model running (diffuse color/stripe artifacts on
the synthetic pattern). Magpie post-processes the raw result with
`DLSSNRColorShader` / `DLSSNRDetailShader`, temporal stabilization and its
`residualMultiplier`/`residualSaturation`/... controls. Those shaders are not ported
yet, and a real video stream has no motion vectors or depth buffer to feed the
model.

Probe results on this machine (2026-11-16, animated input at 60 Hz):

- **Zero motion + zero depth, or the MVec/Depth parameters omitted:** `EvaluateFeature`
  returns success but the output texture is not written (it keeps its previous
  contents, e.g. the magenta prefill). Presets 0/1, styles 0/1 behave this way.
- **Style 2 with zero guides:** writes only partially / with artifacts; not usable.
- **Non-zero synthetic guides (0.5 px motion, depth 0.5):** writes a full frame that
  differs from the input, but shows striping/chroma artifacts because the guides are
  fake. This proves the model expects genuine motion vectors.

Conclusion: DLSSNR on a decoded 2D video stream needs real motion vectors (game
motion, optical flow, or HEVC block motion converted to screen space) plus the
residual composite. VSR remains the shipped neural path.

Realistic next steps:

1. Port the residual composite (start with `residualTransfer = InPlace`) and
   validate on real PS5 frames at 1080p.
2. Decide the guide contract: zero motion + zero depth (Magpie's zero contract),
   or optional optical flow (heavy).
3. Only then add `media/dlssnr.*` to the app, applied before FSR1/Lanczos.
   DLSSNR is same-resolution, so it composes with the existing upscaler and is
   independent of RTX VSR.

### Why a naive host fails

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
