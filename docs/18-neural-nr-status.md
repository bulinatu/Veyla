# 18 - Neural rendering (DLSSNR) - current state and next steps

Status date: 2026-11-17.

## What is working now

### DLSS Super Resolution (an upscaler)
- `Settings > Upscaling > Upscaler = DLSS Super Resolution (D3D12)`.
- Presets K / L / M and Balanced / Max Quality; live changes re-create the
  feature without restarting the session.
- D3D12 NGX, zero depth + zero motion, history reset every frame (no ghosting
  on a 2D video). Output at the render target (1440p / 2160p).
- Verified: 1080p -> 2560x1440, preset L, clean output; module probe.

### DLSS Neural Rendering (DLSSNR, same resolution)
- `Settings > Neural > Neural Rendering = On`.
- Engine: the vendor NGX NR worker from the NeuralScreen package
  (`native/nvngx.dll` + `native/nvngx_dlssnr.dll`), started by
  `media/NvDlssnr` as a hidden child process. The C++ client speaks the
  worker's shared-memory protocol directly: 1920x1080 RGBA in, NR RGBA out.
- Wired settings: style (Default/Natural/Cinematic), strength, local tone,
  structure, character/skin, character mask, UI correction. They are persisted
  in the profile and restart the worker when changed.
- Status: worker init at app startup logs `[app] NR worker ready 1920x1080,
  style 2`; the Neural page shows `NR active - 1920x1080 - last X ms`.
- Direct module probe: ~9.5 ms typical per 1080p frame, spikes 20-25 ms.
- The NR result is 1080p; FSR1 (the selected Upscaler) upscales it to the
  render target. DLSSNR is exclusive with DLSS SR, RTX VSR and VFG.

### Settings layout
- Tabs: Display / Upscaling / Performance / Neural / Audio / Controller /
  Connection / About.
- Display: HDR output, video decoder, chroma upsampling.
- Upscaling: Upscaler (incl. DLSS SR), DLSS preset/quality, target, RTX VSR,
  Neural Ultra preset.
- Performance: latency HUD, frame generation, frame buffer (adaptive: starts at
  1, rises on jitter, falls after 5 s).
- Neural: DLSSNR (Neural Rendering, style, strength, tone, structure, skin,
  character mask, UI correction).
- In-game overlay: Stream settings (Upscaler incl. DLSS SR + preset/quality,
  target, VSR, frame gen, HUD, buffer, decoder) + a Neural page (DLSSNR).
- The `Stack passes` / hook / resolution controls were removed from the NR
  page because the worker currently receives only the initial one-pass header;
  they will return when the PPRM/RNSZ commands are wired.

## Runtime / packaging
- `nvngx_dlss.dll` for DLSS SR: `third_party/nvidia-ngx/runtime` (gitignored);
  CMake copies it next to `brp.exe`; `scripts/install-ngx-runtime.ps1`.
- NR worker: `third_party/neuralscreen/native` (gitignored; worker +
  `nvngx_dlssnr.dll` + forwarder + Spout DLLs); `scripts/install-nr-runtime.ps1`.
- The worker is user-supplied/proprietary and is not redistributed.

## Not yet verified / next steps (in order)

1. **Live NR test with the console.** The console was in rest mode, so only
   startup and the synthetic module probe were tested. Connect, stream, check
   `BRP_DECODE_PROBE=1`: fps, `[app] NR`/`NR active` ms, visual artifacts.
   Capture a frame with F12 and compare NR on/off.
2. **Move NR off the UI thread.** `NvDlssnr::Process` is synchronous on the
   render thread; a 25 ms spike stalls the UI. Add a dedicated NR thread with
   a one-frame input queue and a latest-result slot; the UI presents the newest
   NR frame and never blocks on the worker.
3. **Stack passes (2/3/4).** The worker supports `PPRM`/`RNSZ`; currently only
   the initial header (one pass) is sent. Implement the per-pass command, then
   re-add the `Stack passes` control (it was removed because it was not wired).
4. **Live NR settings without a restart.** Today a style/strength change
   restarts the worker (~3 s). Send the worker's per-pass/resize command or
   debounce; target is <200 ms.
5. **Settings cleanup (partly done).** The Performance tab and the overlay
   DLSS preset/quality rows are in place. Still to do: delete the now-unused
   profile/handler fields for `nr_passes` / `nr_hook` / `nr_resolution` and the
   `W_NeuralSr` widget, then do a final duplicate/label pass over all tabs.
6. **Overlay live test.** The Neural overlay page and the DLSS entry in the
   overlay Upscaler list have not been exercised during a live stream.
7. **Install/packaging test.** Run `install-ngx-runtime.ps1` and
   `install-nr-runtime.ps1` into a clean app folder; confirm both find their
   runtimes via the exe-relative search paths.
8. **NR resolution.** The worker is fed 1080p and outputs 1080p; 4K NR is not
   available (the protocol caps work at 2560x1440 and the NGX NR feature goes
   silent at 4K). Keep FSR1/ VSR for the final upscale.
9. **Provenance note.** The working NR worker is NeuralScreen's vendor NGX
   engine, not Magpie's compiled filter. Magpie's DLSSNRFilter is the reference
   for a native port; the worker was used because it is the working engine and
   the vendor DLLs are user-supplied.

## Verification commands
```powershell
# DLSS SR / NR module probes
.\build\Release\dlss_sr_module_probe.exe third_party\nvidia-ngx\runtime
.\build\Release\nr_module_probe.exe
# App logs
$env:BRP_DECODE_PROBE=1
.\build\Release\brp.exe
# worker log: build\Release\nr-worker.log
```
