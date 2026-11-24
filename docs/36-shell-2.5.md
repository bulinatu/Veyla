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
| Decode (d3d11va) | 8.2-8.5 ms = 1.4 transfer + 0.85 resize + 5.8 swscale | expected; bicubic is the fastest filter here |
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
