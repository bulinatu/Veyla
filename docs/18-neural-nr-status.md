# 18 - Neural rendering (DLSSNR) - current state and next steps

Status date: 2026-11-17 (second session).

## What is working now

### DLSS Super Resolution (an upscaler)
- `Settings > Upscaling > Upscaler = DLSS Super Resolution (D3D12)`.
- Presets K / L / M and Balanced / Max Quality; live changes re-create the
  feature without restarting the session. The two rows are disabled unless
  DLSS SR is the selected upscaler.
- D3D12 NGX, zero depth + zero motion, history reset every frame (no ghosting
  on a 2D video). Output at the render target (1440p / 2160p).
- Verified: 1080p -> 2560x1440, preset L, clean output; module probe.

### DLSS Neural Rendering (NR)
- `Settings > Neural > Neural Rendering = On`.
- Engine: the vendor NGX NR worker from the NeuralScreen package
  (`native/nvngx.dll` + `native/nvngx_dlssnr.dll`), started by
  `media/NvDlssnr` as a hidden child process. The C++ client speaks the
  worker's shared-memory protocol directly: 1920x1080 RGBA in, NR RGBA out,
  4K capped at 2560x1440 by the runtime.
- **Off the UI thread.** `NvDlssnr::Process` runs on its own thread. The render
  loop hands over the newest decoded frame through a one-frame queue and
  presents the newest finished NR frame; when the worker is late the previous
  frame stays on screen and the source frame is counted as dropped. A 25 ms
  worker spike can no longer stall the UI.
- **Live settings, no worker restart.** Every control on the Neural page (and
  in the overlay) sends the worker's `RNSZ` command. Measured in the app:
  - style / strength / tone / structure / skin / mask / UI: **3 ms**
  - Boost 70% + 2 passes -> full 1080p + 1 pass: **131 ms**
  - full 1080p -> Boost 70% + 2 passes: **218-264 ms**, the first frame after
    the rebuild takes ~370 ms (one visible frame repeat, no freeze)
- **Network size / Stack passes.** This is the worker's own quality ladder
  (its "Boost" mode):
  - `Full 1080p (1 pass)`: network at the stream size, one pass.
  - `Boost 85/70/55% + passes`: network at that fraction, `Stack passes`
    (1..4) evaluated, result upscaled back to 1080p on the GPU by the worker.
  - Cost measured on synthetic 1080p content: full 1 pass ~12 ms,
    Boost 70% + 2 passes ~12 ms (the cascade buys detail back at the same
    cost). `Stack passes` is disabled at full size, where only one pass runs.
- Wired settings: style (Default/Natural/Cinematic), strength, local tone,
  structure, character/skin, character mask, UI correction, network size and
  stack passes. All persisted in the profile.
- Status line: `NR active - 1920x1080 - net 1344x756 (2 pass) - X ms -
  N frames - M dropped`.
- The NR result is 1080p; FSR1 (the selected Upscaler) upscales it to the
  render target. NR is exclusive with RTX VSR and VFG, but **not with DLSS
  SR**: when DLSS SR is the selected upscaler it runs on the NR result.

### NR + DLSS SR chain (the required combination)
- Pipeline: stream 1080p -> NR (NGX worker, own thread) -> DLSS SR
  (1080p -> 1440p/2160p) -> present. Both features stay enabled together;
  selecting DLSS SR does not turn NR off, and enabling NR does not turn
  DLSS SR off.
- Measured with `tools/nr_sr_chain_probe` at 1080p -> 2560x1440:
  **NR 10.3 ms + SR 2.8 ms = 13.2 ms per frame (75.7 fps ceiling)**.
- The SR number used to be ~15 ms because the fence wait polled with
  `Sleep(1)`, which the default Windows timer rounds to 15.6 ms. The wait is
  now a fence event; standalone DLSS SR is **~2.8 ms** instead of ~15 ms.
  The NR pipe reader got the same treatment (it waits on the pipe handle).
- DLSS SR still runs on the presentation thread (it shares the D3D11 device
  with the presenter). If live testing shows the chain dropping frames, the
  next step is to run SR on the NR thread with a double-buffered output.
- If DLSS SR fails mid-stream the app falls back to presenting the NR result
  and keeps NR running.
- NR adds at most one displayed frame of latency by design: the presentation
  loop shows the newest finished NR frame instead of waiting for the current
  one. That is the smoothness-first trade the project asked for.

### Motion field: what the worker receives
- The reference client computes an OpenCV DIS optical-flow field and sends it
  at 320x180; this client does not have OpenCV in the pipeline, so it sends a
  zero motion field (same as "no motion vectors").
- Tested with a high-contrast bar moving 12 px/frame over 24 frames
  (`tools/nr_motion_probe`): no trail, no double edge, clean single bar in
  every saved frame. NR does real work: mean |input - output| is 2.6/255
  (p99 7-9).
- Conclusion for now: zero motion is usable for the video path, the effect is
  subtle. A real flow field would likely let the network act more strongly;
  that is future work, not a correctness bug in the current build.

### Settings layout
- Tabs: Display / Upscaling / Performance / Neural / Audio / Controller /
  Connection / About.
- Display: HDR output, video decoder, chroma upsampling.
- Upscaling: Upscaler (incl. DLSS SR), DLSS preset/quality (only enabled for
  DLSS SR), target, RTX VSR, Neural Ultra preset.
- Performance: latency HUD, frame generation, frame buffer (adaptive: starts at
  1, rises on jitter, falls after 5 s).
- Neural: Neural Rendering, Network size, Stack passes, style, strength,
  local tone, structure, character/skin, character mask, UI correction.
- In-game overlay: Stream settings (Upscaler incl. DLSS SR + preset/quality,
  target, VSR, frame gen, HUD, buffer, decoder) + a Neural page with the same
  NR controls.
- Removed dead fields/widgets: `nr_hook`, `nr_resolution`, `W_NeuralSr`,
  `W_NrHook`, `W_NrResolution`. `nr_passes` is now wired (Boost cascade);
  `nr_scale` is new.

## Live test (2026-11-17, PS5-580, game running, 1080p60 stream)

Configuration: `render_target=1440p`, `neural_nr=1`, `neural_sr=1`, DLSS preset
M/L, quality Max Quality.

| Metric | Result |
|---|---|
| Stream | 1920x1080 @ 60 fps, ~10-50 Mbps, decode D3D12VA 4.2-5.4 ms |
| Present rate | 59.3-61.0 fps over the session |
| NR (Full 1080p, 1 pass) | **10.4-11.1 ms**, 0-1 source frames dropped |
| DLSS SR (1080p -> 1440p) | **3.8-4.2 ms** live |
| NR -> DLSS SR chain | holds 60 fps; queue 0-1, `lost 0` |
| Boost 70% + 2 passes, switched live | `RNSZ 1344x756 (2 pass) 213 ms`, NR 12.9 ms, 3 frames dropped during the rebuild, then 60 fps |
| Back to Full 1080p, live | `RNSZ 1920x1080 (1 pass) 118 ms` |
| NR Off -> On during the stream | worker restarts (~3 s), two frames dropped, one decode spike (46 ms), then 60 fps |
| Overlay Neural page | opens and edits live after the button fix below |

Visual check with F12 captures at 2560x1440 (`build/shots/live-chain-full.bmp`,
`live-chain-boost.bmp`, `live-sr-only.bmp`): no ghosting, no halo, edges and
the character keep the same sharpness with NR on or off; Boost looks the same
as Full on this content. The NR effect stays subtle, as the synthetic test
predicted, which is expected with zero motion guides.

### Bug found and fixed during the live test

The overlay's `Neural rendering` button was drawn but did nothing:
`Hit::Button` only dispatched a fixed list of ids and never fell through to
`ApplyDropdown`, so `W_NeuralOverlay`, `W_NeuralOverlayBack` and
`W_NeuralMax` (the Neural Ultra preset) were dead. Buttons now fall through to
the widget switch when they are not one of the special-cased ones.

## Runtime / packaging
- `nvngx_dlss.dll` for DLSS SR: `third_party/nvidia-ngx/runtime` (gitignored);
  CMake copies it next to `brp.exe`; `scripts/install-ngx-runtime.ps1`.
- NR worker: `third_party/neuralscreen/native` (gitignored; worker +
  `nvngx_dlssnr.dll` + forwarder + Spout DLLs); `scripts/install-nr-runtime.ps1`.
- The worker is user-supplied/proprietary and is not redistributed.
- The worker log goes to `nr-worker.log` next to `brp.exe`. If that file cannot
  be opened the process writes to `NUL`, never into the protocol pipe (a log
  line on stdout desynchronises every reply).

## Not yet verified / next steps (in order)

1. **Done: live NR test.** See the live-test section above. What remains is a
   longer session (30+ minutes) to watch the dropped counter and thermals.
2. **Done: overlay live test.** The Neural page, Network size, Stack passes and
   the DLSS entry were exercised during the stream (the page was unreachable
   until the button-dispatch fix).
3. **Install/packaging test.** Run `install-ngx-runtime.ps1` and
   `install-nr-runtime.ps1` into a clean app folder; confirm both find their
   runtimes through the exe-relative search paths.
4. **Optional: real motion field.** A cheap C++ flow (block matching at
   320x180) could replace the zero field and let the network act more
   strongly. Measure first; do not add cost without a visible gain.
5. **NR resolution.** The worker is fed 1080p and outputs 1080p; 4K NR is not
   available (the protocol caps work at 2560x1440 and the NGX NR feature goes
   silent at 4K). Keep FSR1 / VSR for the final upscale.
6. **Provenance note.** The working NR worker is NeuralScreen's vendor NGX
   engine, not Magpie's compiled filter. Magpie's DLSSNRFilter was the
   reference for the native attempt; the vendor worker is used because it is
   the engine that actually produces a real NR image, and the DLLs are
   user-supplied.

## Verification commands
```powershell
# DLSS SR / NR module probes
.\build\Release\dlss_sr_module_probe.exe third_party\nvidia-ngx\runtime
.\build\Release\nr_module_probe.exe
# Moving-bar test, optional args: <root> <scale percent> <passes>
.\build\Release\nr_motion_probe.exe
.\build\Release\nr_motion_probe.exe "" 70 2
# App logs
$env:BRP_DECODE_PROBE=1
.\build\Release\brp.exe
# worker log: build\Release\nr-worker.log
```
