# 36 - 2.5.0: system optimization, neural panel, overlay tabs

2.5.0 is the 2.4.7 engine plus the shell work below and the latency fixes from
`35-latency-audit.md`. Engine, shell and tests were rebuilt; `ALL PASS`.

## System optimization

`Advanced > System` shows the detected GPU, VRAM, display and DLSS flags, and
the `Optimize` button. The same routine runs once on a fresh install: the marker
`system.auto_optimized` is written to the settings file so it never overrides a
user again.

What it reads: the engine's first status snapshot (GPU name, VRAM, DLSS SR/FG
runtime presence, NR runtime) and the device list (primary monitor size and
refresh). What it writes:

| Key | Value |
|---|---|
| `streaming.input` / `fps` / `frame_buffer` | 1080p / 60 / 0 |
| `streaming.bitrate_kbps` | 100000 on a 1440p-or-higher panel, else 60000 |
| `video.output` | monitor height, capped to the shipped presets |
| `video.vsync` / `vrr`, `display.*` | on / on for 100 Hz+, match refresh, fullscreen on play |
| `controller.poll_hz`, `audio.buffer_ms` | 1000 / 10 |
| DLSS-G | on, 2x (only when the FG runtime is present) |
| DLSS SR | on; 1440p Max on a 1440p+ panel, otherwise native DLAA |
| NR | on; at 1440p SR it uses Boost (work scale 75 %) and one pass |

The optimizer waits for `online && status rev > 0 && devices rev > 0` before it
runs; without that gate it saw the default (all-false) capability flags and
disabled AI on capable machines. If the engine never reports hardware the pass
is abandoned and retried on the next launch (the marker is not written).

## Neural rendering panel

The old single Neural page is split into **Upscaling**, **Frame Gen** and
**Neural**. The Neural page exposes every parameter the NGX feature-18 worker
accepts:

- style (Default / Natural / Cinematic);
- intensity, local tone, local structure, skin structure (percent);
- passes (1..20, slider);
- work scale (50..100 %, slider);
- UI correction, auto mask (toggles).

Numeric rows are sliders with an editable value: click the number, type, Enter.
The first keystroke replaces the prefilled value (select-all behaviour), Escape
cancels. The same settings apply to NR when it runs inside the DLSS-G worker:
that worker now receives the parameters on Start and reconfigures them live
through the RNSZ command instead of being restarted for every change.

### Boost and passes

The worker's multi-pass cascade only engages with Boost (work scale below
100 %). At 100 % the worker logs `Boost is off: the network runs on the whole
frame` and executes a single pass, so passes above 1 have no effect there.
At 1440p the tested combination is **work scale 75 % + 1 pass**: the network
runs at 1080p on the 1440p frame, the worker takes 15.6-17.3 ms per input and
holds 119.7-120.1 fps out. Raising passes multiplies the cost (75 % + 4 passes
measured 33 ms), and 100 % at 1440p is 16-19 ms with dropped inputs.

### Unlock limits

A red `USE AT YOUR OWN RISK` box at the bottom of the Neural page (shell and
overlay) unlocks every neural slider and typed value up to a hard **9999**
ceiling (`ai.neural_unlock`). The engine accepts up to 9999 in the settings
file regardless of the switch; the UI is what normally keeps 0..200 / 1..20 /
50..100. Values above what the model can hold are passed through: stutter,
initialisation failure or VRAM exhaustion are expected.

## In-game control center

- New **Neural** tab with the same controls and the unlock box.
- The focused tab is remembered for the current game session: closing and
  reopening the panel returns to it. It resets on disconnect (and when the
  stream ends), via a process-wide tab index in the shell.

## Cards

Every panel, tile and preset on every page is built from one `card()`: a
translucent frost over the aurora, a hairline border and a soft gaussian rim.
The stat tiles were the reference; the hero, presets, group panels and the
saved-profiles card now match them.

## Fixes carried in

- **DLSS SR "Max"** sent `NVSDK_NGX_PerfQuality_Value` 4, retired in DLSS 2.x;
  `CreateFeature` failed with `0xBAD00010` and SR was silently disabled for the
  session. Mapped to `MaxQuality` with explicit sizes; 1920x1080 -> 2560x1440
  now initialises.
- **Settings clobbering:** the engine's `SettingsStore::Save` replaced the whole
  file, dropping shell-only keys (`ui.*`, `system.*`) whenever it recreated it.
  It now merges into the existing file.
- Frame buffer 0 removes the primed queue (58-60 ms of frame age in the 30 fps
  test), and the DLSS-G worker's own process time is printed by the probe.

## Packaging

`tools/package_release.ps1` at version 2.5.0: standalone folder + zip and the
`brp_update.exe` patch path. Version bumped in `CMakeLists.txt`,
`next_app.cpp` (four strings), `ui/Cargo.toml`, `ui/Cargo.lock` and the
packaging script.

## Final pipeline audit (2.5.0, live)

Measured on the user's PS5 through the harness: 1920x1080p60 HEVC at
100 Mbit/s, buffer 0, output 1440p, 240 Hz panel.

| Stage | Measurement | Verdict |
|---|---|---|
| Console profile | always 1920x1080p60 HEVC (Remote Play's only profile) | input-resolution setting was dead; row removed |
| Network | loss 0.00 %, jitter 16.67-16.70 ms, 0 decoder drops steady | clean |
| Decode (d3d11va) | 3.0-3.5 ms with AI on (GPU video processor + one readback); 0.4 ms and no readback with AI off | GPU end-to-end; swscale gone |
| Frame queue / age | queue 0, age 0-1.4 ms (buffer 0) | lowest-latency path |
| DLSS SR DLAA | submit 0.55 ms, output view used directly (no readback) | clean |
| DLSS SR 1440p Max | submit 0.5-0.9 ms, 5.5-8.7 ms per frame incl. worker readback | GPU-bound, correct |
| DLSS-G 1440p, NR off | worker 6.9-7.7 ms, worker log: `displayed 119.8 FPS (real + generated, 2x)` | exactly 2 refreshes per frame on 240 Hz |
| DLSS-G 1440p, NR on (scale 100 %) | worker 16-19 ms, ~50 dropped inputs/s | full-size NR does not fit 60 fps |
| DLSS-G 1440p, NR on (Boost 75 %, 1 pass) | worker 15.6-17.3 ms, log: `displayed 119.7-120.1 FPS (2x)` | NR at 1440p works; this is the shipped default |
| Presentation | scanout 239.8 Hz, present misses 0 steady, cadence p50 16.6 ms | clean |
| Startup | one-time NGX SR init 5.2 s + worker start; frames queued meanwhile are trimmed | known cost, no steady-state effect |

Wired while auditing: `display.monitor` now places the stream on the chosen
display (it used to follow the shell's monitor), and `video.vsync` now reaches
the presenter (`Present(1)` vs `Present(0, ALLOW_TEARING)`). Removed as dead:
input resolution, HDR transport, colour space, VRR, refresh rate, bit depth and
colour format.

## Live neural tuning (fix)

Sliders in the Neural page/overlay did nothing until a reconnect. The root
cause was in `NextApp::Apply()`: it captured `previous_ai` from `settings_`,
which the caller (`ReloadSettingsFromDisk`) had already replaced with the new
file, instead of from `host_->Settings()` (the configuration the engine is
running with). Every `previous_ai.neural_* != settings_.ai.neural_*` comparison
was therefore false and the SR/FG/NR rebuild blocks never executed. This also
silently delayed SR-output and frame-generation changes until a reconnect.

Fixed by taking `previous_ai` from `before.ai`, and by updating the DLSS-G
worker in place: `SetNeuralInWorker` now marks the worker dirty so the feed
thread applies `SetNeuralBypass` together with the RNSZ reconfigure.

Verified live: intensity 200 -> 0 -> 200 while streaming; the engine log shows
one `[nr] RNSZ 1920x1080 (full 2560x1440, 1 pass, 0 ms)` per change, and screen
captures of the worker output change accordingly (A-B-A). Fine tuning (tone,
structure, skin, passes) travels in the same command; style changes rebuild the
network style inside the worker.

## Leaving a session leaves no windows behind

Two leftovers were found with a window-enumeration probe around a real
disconnect (engine mode, overlay open, then `disconnect_now`):

- `NextApp::ToggleWatch(false)` restored and showed the engine's own 1600x900
  window even in engine mode, where the shell is the UI. Since the engine skips
  `Render()` when it is not watching, the restored window was blank but still
  sat over the desktop and took the foreground. It now stays minimized in
  engine mode; the classic (non-engine) build still restores it.
- The GPUI control center is a separate topmost window, and the shell did not
  close it when the stream ended. It now closes in the same poll where the
  shell is restored, so a disconnect never leaves a topmost panel over the
  desktop.

Verified after the fix: right after disconnect the DLSS-G presenter
(`NeuralScreenPresent`) is hidden, the stream window is hidden, the engine
window stays at -32000,-32000 minimized, and the foreground returns to the
window the user had before the session.

## Latency research

`docs/37-pxplay-latency.md` compares the PXPlay / PS Remote Play client paths
with ours and lists the remaining decode and worker headroom.

## GPU decode and presentation

The shell now creates one D3D11 device with `VIDEO_SUPPORT` and multithread
protection. d3d11va decodes on it, the video processor converts NV12/P010 to
RGBA on it, and the stream presenter is created on it:

- **AI off:** the RGBA texture is presented in place. No GPU->CPU transfer, no
  swscale, no upload, no readback. Probe: `dec 0.37-0.48 ms`, loop p50
  `0.67 ms`, 240 Hz scanout, zero present misses.
- **AI on:** one staging readback feeds the existing RGBA consumers. Probe:
  `dec 2.8-3.5 ms` (was 8.2-8.5), worker unchanged, zero drops.
- PQ/BT.2020 and any unsupported driver/format fall back to the old swscale
  path automatically; `BRP_DECODE_VP=0` forces the fallback.
- Turning AI off live returns to the 0.1 ms path on the next frame; turning it
  on pays the known one-time NGX/worker start (~5 s).

## DLSS 4.5 models

The package ships the 310.9.16 DLSS Super Resolution runtime (was 310.7) and
exposes the model preset on the Upscaling page / overlay:

| Setting | Preset | Intended mode |
|---|---|---|
| Auto | K | DLAA, Quality, Balanced |
| Auto | M | Performance |
| Auto | L | Ultra Performance |
| K | 11 | transformer, best quality |
| L | 12 | second-gen transformer, ultra performance |
| M | 13 | second-gen transformer, performance |
| J | 10 | transformer, slightly less ghosting |

Auto replaces the old hard-coded preset 12: L was being used for DLAA and
Quality, where K is the correct default. The 310.9.16 runtime is required for L
and M (DLSS 4.5); with the older runtime those presets fall back to defaults.

## Shell navigation

- The header has six entries only: Home, Stream, Video, Audio, Controller,
  Overlay.
- Home has Overview / Advanced; Video has Output / Upscaling / Frame Gen /
  Neural. The sub-navigation sits under the header so the top bar stays
  compact.
- Advanced is always on: the Basic/Advanced switch and the Apply button are
  gone. Every widget writes its key through `persist()` when it changes, and
  the engine auto-applies about a poll tick later. Sliders save at the poll
  cadence so a drag does not write one file per pixel.
- The top bar has full-width drag strips in its 12 px top and bottom padding,
  plus the logo, the empty middle and the status pill. The tab hitboxes stay
  untouched.
- The window has a 1040x700 minimum, the tab row does not shrink, and the
  minimize/maximize/close cluster is fixed at the right. Verified at the
  minimum size: all three controls remain visible.

## NVIDIA Reflex (markers + boost, no sleep)

`video.reflex` adds Off / On / Boost on the Video page and the in-game Video
tab. The implementation sends the NVIDIA latency markers around the parent
presenter (`SIMULATION_START/END`, `RENDERSUBMIT_START/END`,
`PRESENT_START/END`) and asks the driver for marker use and, in Boost mode, the
low-latency clock boost.

The driver's sleep stage (`NvAPI_D3D_Sleep`) is deliberately **not** called:
measured on the reference machine it changes the waitable present cadence to
~225 Hz with 56+ missed refreshes per second, which costs more than it can ever
save on a remote stream. With markers only, the presenter stays at 240.0 Hz
with zero missed refreshes. Reflex cannot touch the console's input path; on a
video stream the useful part is the boost and the driver-side frame bookkeeping,
not a lower button-to-photon number.

**Scope.** The markers are set on the engine's parent presenter, which covers
DLSS SR and standalone NR (they still end in that swapchain). With DLSS-G the
worker owns a separate swapchain in its own process; Reflex must be initialised
there to cover generated frames, and the worker does not do that today. The
setting is therefore a no-op for the FG-presented path, not a hidden one.


## Multi Frame Generation 5x/6x

The multiplier selector offers 2x-6x on every RTX card. 3x-6x load the bundled
SM86 proxy; 5x/6x are the DLSS 4.5 MFG modes that are native on RTX 50 and
forced through the proxy on older cards. Nothing is stepped back silently: the
UI note says that non-50 GPUs may stutter or fail above 4x, and the engine uses
exactly what is selected. Measured on the 5070 Ti at 1440p with NR on, 6x
enabled (`[fg] 6x enabled`, `displayed ... (6x)`) but the worker settled around
109 FPS, while 4x holds ~120; 4x remains the recommended multiplier at that
load, 5x/6x are there for lighter NR scales and 1080p.
