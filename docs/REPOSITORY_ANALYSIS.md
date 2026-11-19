# Repository Analysis - BetterRemotePlay (reference implementation)

Date: 20 November 2026
Reference commit: `1b269b6` (0.4.9)

This document is the factual inventory the rebuild starts from. Nothing here
is aspirational: counts and paths were taken from the working tree.

## 1. Size and shape

- 44 C++ source/header files under `src/`, 18,417 lines total.
- `src/app/app.cpp` is 6,981 lines; `src/app/app.h` is 556 lines. This single
  class owns UI, streaming glue, DLSS/RIFE orchestration, settings, presets,
  diagnostics and input. Splitting it is the main structural debt.
- Other large units: `media/pipeline.cpp` 835, `session/streaming.cpp` 821,
  `platform/gamepad.cpp` 802, `media/nv_vfx.cpp` 718, `media/nv_dlssnr.cpp`
  672, `media/nv_dlss_sr.cpp` 604, `ui/ui_renderer.cpp` 549,
  `session/profile.cpp` 522, `audio/audio_engine.cpp` 474,
  `app/rife_setup.cpp` 469, `platform/win32_window.cpp` 416,
  `media/decoder.cpp` 407, `media/rife.cpp` 336.
- 20+ probe executables under `tools/` are the current test/benchmark
  strategy. They are genuine hardware tests, not mocks, but they are manual.

## 2. Build and dependencies

CMake, C++20, MSVC on Windows x64. Existing static libraries:

| Target | Sources | Purpose |
| --- | --- | --- |
| `brp_media` | `src/media/*` (pipeline, decoder, DLSS SR/NR, RIFE, VFX, Reflex) | D3D11/DXGI/D2D, decode, AI, presentation |
| `brp_ui` | `src/ui/*` | Direct2D renderer, fonts, icons |
| `brp_session` | `src/session/*` (chiaki, discovery, pairing, PSN, profile) | PS5 protocol and state |
| `brp_rife` | `src/media/rife.cpp` | TensorRT RIFE loader |
| `brp` | `src/main_app.cpp` + `src/app/*` | the monolithic application |
| probes | `tools/*` | hardware validation |

External/vendored: FFmpeg 7/8 shared DLLs, mbedTLS (chiaki), chiaki-ng
session core (submodule/vendor), NVIDIA NGX + NeuralScreen worker
(`nr/native`), TensorRT 10.16 optional (runtime-loaded, never linked),
DirectX 11/12, D2D/DWrite.

## 3. What actually works today

### PS5 connectivity
- UDP discovery, PSN login (browser redirect paste), PIN pairing, session
  request, wake, chiaki session handshake.
- Console sleep/wake, reconnect after stream end, RST on disconnect.
- Proven against the reference PS5 (192.168.1.21) and a second PC (RTX 5080).

### Streaming
- H.264/HEVC/AV1 via FFmpeg; hardware decode choices D3D11VA, D3D12VA,
  Vulkan, DXVA2, CUDA/NVDEC, software. ~8-10 ms decode at 1080p in logs.
- Jitter buffer with adaptive target, RTP loss stats, queue drops.
- Audio device output with configurable buffer; Opus decode; test tone.
- DualSense over HID + BT: LEDs, lightbar, adaptive triggers, rumble,
  PS5 haptics (3 kHz vibration decoded and mapped to motors), 10 ms refresh.
- Motion, touchpad, player LEDs, mic LED, console rumble/trigger flags.

### Rendering and presentation
- D3D11 presenter, DXGI waitable swapchain, borderless fullscreen, HDR10,
  monitor-refresh pacing, F11 window toggle.
- Upscalers: Bilinear, Catmull-Rom, Catmull-Rom+sharpen, Lanczos+RCAS, FSR1,
  DLSS SR (NGX D3D12) with K/L/M presets and 33-100 % modes.
- Neural Rendering (DLSS NR through the NeuralScreen worker), optional,
  before RIFE.
- Frame generation:
  - RIFE 4.25 lite and 4.26 heavy via TensorRT, worker thread, 1080p,
    30 -> 60, blend and motion-safety controls. Measured 17.7 / 23-25 ms.
  - DLSS Frame Generation 2x through the worker, 60 -> 120, verified
    119.8-120.2 fps with 0 drops after 0.4.9.
  - Legacy NVIDIA VFG (optional, `nv_vfx`).
- Colour: exposure, contrast, highlights, shadows, saturation, vibrance,
  hue, temperature, tint, gamma; sharpening; HUD mask.
- Reflex markers (no sleep).

### Configuration and diagnostics
- Flat JSON `%APPDATA%\BetterRemotePlay\session.json` with ~100 keys, hand
  written serializer/parser in `session/profile.cpp`.
- `health.log` every 30 s and optional `testing.log` per second with frame,
  decode, RIFE, FG, audio and refresh metrics.
- Single-file updater (`update.exe`) with appended diff payload; standalone
  package script; no installer, no service.

## 4. Known structural debt

1. `App` is a god object. UI hit-testing, streaming callbacks, GPU state,
   settings persistence and AI scheduling live together.
2. Settings are saved as an untyped flat map; every screen reads raw fields.
3. The UI is immediate-mode D2D with per-screen coordinates. There is no
   design-token system, no navigation model, no view/view-model boundary.
4. The overlay is not independent: the in-stream sheet is part of `App`, and
   the DLSS-G presenter is a separate worker window.
5. AI backends are selected by ad-hoc `profile` integers, not by capability
   discovery. Availability is partly inferred from files on disk.
6. Tests are probe executables; there is no unit-test target for config,
   scheduling or profile validation.
7. `app.cpp` uses `goto`-free but deeply nested conditionals around the
   pipeline; adding a backend requires editing several unrelated blocks.

## 5. Reuse verdict

| Area | Verdict | Reason |
| --- | --- | --- |
| chiaki session/protocol | **Reuse behind an interface** | Hard-won, verified; rewriting it adds risk with no user value. |
| discovery/pairing/PSN | **Reuse behind an interface** | Same. |
| FFmpeg decoder | **Reuse**, wrap in `IVideoDecoder` | Works, hardware paths validated. |
| D3D11 presenter/swapchain | **Reuse**, wrap in `IPresenter` | Waitable pacing and HDR are solved. |
| DLSS SR / NR / FG | **Reuse**, wrap in `IAiBackend` | Real hardware paths with measured results. |
| RIFE/TensorRT | **Reuse**, wrap in `IFrameGenerator` | Working 30 -> 60; probes exist. |
| Audio engine | **Reuse**, wrap | Working. |
| DualSense HID | **Reuse**, wrap | Working, including haptics. |
| `App` UI/state | **Replace** | This is the monolith the rebuild exists to remove. |
| Flat profile JSON | **Replace** with typed settings + migration, import old file | Keep user data, not the schema. |
| Immediate-mode screens | **Replace** with design system + navigation + view models | Required by the brief. |
| Probe-only tests | **Replace/augment** with unit + integration + benchmark harness | Required by the brief. |

## 6. Risks

- Rebuilding the UI can regress input focus, borderless fullscreen, HDR and
  the overlay. Mitigation: keep `brp.exe` runnable during migration and port
  one screen at a time against the same services.
- The DLSS-G worker is closed-source and has a 2x ceiling; the new app must
  model capabilities honestly instead of promising 4x/6x.
- TensorRT engines are GPU-specific; any "model installed" UI must validate
  by loading, not by file presence.
- A second executable doubles packaging size unless shared DLLs/assets are
  reused; the release script must stay single-folder.
