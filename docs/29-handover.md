# 29 - Handover (18 Nov 2026)

Windows C++ PS5 Remote Play client on the chiaki-ng session core. DLSS Super
Resolution, DLSS Frame Generation (through the closed NeuralScreen worker),
GPU colour and sharpening, a custom PS5-style UI and native DualSense support.
Build `0.3`. Run `git log -1` for the current commit.

Version 0.5.0 (20 Nov 2026).

- Standalone: `dist\BetterRemotePlay-0.5.0-standalone.zip`, 623.2 MB, SHA256
  `8C497B5F6C53CEF5BF24B3E3ECD15DD1BEFE4A521CB3D4655CBB2EAD4BC773B3`.
- Update from 0.4.9: `dist\BetterRemotePlay-0.5.0-update.exe`, **1.29 MB, one
  file**, SHA256
  `74A056AC161A7E215204F197DF2EBD37EC3572986D7AB87C3E6248D578B83005`.
- `dist\BetterRemotePlay\brp.exe` SHA256
  `90AF1EBF8106E271A7BE44876FB321032BF9FDBDE74F6BC50F0EE6051FB246BD`.
- `dist\BetterRemotePlay\brpnext.exe` SHA256
  `8447F7BB166A67ED591CDF7B67F40A9E4FAE5DF3E4170844DDEBC7F27541E44F`.
- The package includes both executables, `rife\rife425_1080.engine` (12.5 MB,
  built for the reference RTX 4090), both RIFE models, `cudart64_13.dll` and
  `update.exe`.
- No `session.json`, no pairing data, no runtime logs.

## What works

| Area | State |
| --- | --- |
| Video | HEVC, 720p/1080p negotiated, D3D11VA/D3D12VA/Vulkan/DXVA2/NVDEC/software. The 30/60 fps setting now reaches the console (it was hardcoded to 60): 30 requested gives src 28.6, 0 lost, 0 drops. |
| Stream smoothness | Streams start borderless fullscreen (F11 returns to the window). With SR on, 60 fps source on 240 Hz: 4 refreshes on 100 % of frames, 0 late ticks, interval p99 4.4 ms. |
| Audio | Opus 48 kHz stereo to WASAPI, device picker, 10-40 ms buffer, test tone. |
| Upscalers | Bilinear, Catmull-Rom, Lanczos+RCAS, FSR1, DLSS SR (33-77 %) and DLAA with K/L/M presets. |
| Frame generation | Two engines. NVIDIA DLSS FG through the worker: 120.0 fps displayed on 240 Hz. RIFE 4.25 lite through TensorRT: a 30 fps stream plays at 60, cadence 16.69 ms average, 22 ms/frame, 0 underruns. Output targets 60/90/120 with duplicate-aware adaptive multiplier. |
| Launch presets | Launch (SR UQ + FG 2x), SR only, Low latency. Saved while streaming; the chain applies at the next start. |
| Colour | 11 controls and 4 presets, applied on the GPU before the FG worker. |
| Controller | DualSense over USB and Bluetooth: buttons, sticks, touchpad, motion, lightbar, LEDs, adaptive triggers, console feedback. PS5 **haptics** (3 kHz stereo) decoded into motor rumble with an intensity setting; classic rumble kept for PS4. |
| Session | PSN sign-in, pairing, LAN discovery, wake, probe. Disconnect sends an idle pad state and an RST, so the console frees the session at once. No automatic re-dial. |
| UI | Start-up intro, boot loader, Home, Settings (10 tabs), in-stream sheet (Tab), double-Esc confirmation, connecting loader. |
| Diagnostics | `%APPDATA%\BetterRemotePlay\health.log` every 30 s, `nr-worker.log`, probes `BRP_FRAME_PROBE`, `BRP_DECODE_PROBE`, `BRP_PAD_PROBE`. |

## Changes in this session (since `ab5ecf5`)

**Stream smoothness**
- `38d6f5a` Streams start borderless fullscreen. Cause: windowed presentation
  goes through DWM composition and held 4 refreshes on 62-84 % of frames;
  fullscreen gave 95-100 %.
- `f3d52d4` Presets only save while a session runs; the chain is built at the
  next start. The old code applied the chain mid-stream, contrary to its comment.
- Profile `fps` was 30 in the test machine's `session.json` (halves the source).
  Set to 60; the code default is also 60. Nothing in the app forced it.

**Settings** (`7de3310`)
- Display: HDR, decoder, chroma, Latency HUD (the only master switch).
- Upscaling: upscaler, target, DLSS mode and preset, DLSS Frame Generation,
  NVIDIA VFG (legacy).
- Performance: frame buffer, Reflex, launch presets.
- Debug: HUD sections only. Map in `docs/08-app.md`.

**Visual pass**
- `54e9819` Home hero with a console silhouette and accent line; Settings content
  on one glass card; refined dropdown, slider and primary button states.
- `bfe9887` Stream sheet uses the same tab names and grouping as Settings, with a
  Performance tab, glass panel and 124 px tabs.
- `1730ba1` Boot screen: the mark draws in, and a step card lists the real
  start-up tasks. Connect loader shows from the first click, with the real phases
  (check, wake, session, first frame).
- `b9b2667` Home: three info cards from the profile (stream, enhancement,
  game library). Invented demo titles, `GameEntry`, `DrawTile` and the dead
  rail code are removed. `Renderer::TextWrapped` added for paragraphs.
- `9ceca24`, `61310a8` Start-up intro ("RemotePlay but Better"): 3.6 s, four
  acts (converging particles, letter-by-letter reveal, shock ring and rule,
  glint and fade to black). Skippable with a click.

**Rejected and reverted**
- MMCSS "Games" on the main thread: median 16 late ticks per 480 vs 9 baseline.
- Display mode switch to 120 Hz for FG: user declined.
- Jitter buffer 2: no gain over 1 (4v 52-81 % across windows vs 70-76 % at 1).

## Known problems and limits

1. **FG 2x on 240 Hz still has a small phase wobble.** The worker presents on
   its own clock and is closed, so the app cannot align it. G-SYNC is the
   remaining untested option (a Windows setting, not a code change).
2. **Game library is not shown.** Remote Play does not list installed games.
   The PSN library endpoint needs the `psn:mobile.v2.core` scope, which belongs
   to the official mobile client. The project ships no Sony keys or client
   identities (About says so), so this is a product decision, not a bug.
3. **FG without SR is not offered.** A worker restarted at 1080p without SR
   came up in overlay mode and never presented.
4. **HDR through FG is SDR.** The worker accepts only RGBA8 (format 28).
5. **DLSS NR costs 7-18 fps at 2x.** NR runs in the worker.
6. **Bluetooth triggers** depend on the console sending effect packets.
7. **Network**: PC side measured (`docs/26`). Router and PS5 items are a
   checklist. Bitrate 100 Mbps tested clean; 50/75 not swept.
8. **Chroma and bitrate quality are not compared.** Lanczos chroma and 75 vs 100
   Mbps need a side-by-side test on a real game.
9. **Untested after the last edits:** the intro's final fade and the hand-off to
   boot. Screenshots were taken from process start, which lags the intro by the
   start-up time, so the 3.25 s shot still showed the settled mark rather than
   the fade. The boot step card, and the Upscaling and Performance tabs of the
   in-stream sheet, were captured and look correct.
10. **RIFE needs a user-supplied runtime.** TensorRT 10.16 + CUDA 13 and an engine built for the GPU (ife\rife425_1080.engine or `BRP_RIFE_ENGINE`). Without them the app falls back to NVIDIA. RIFE adds one source interval of video delay and uses a 40 ms audio buffer to match. Quality against DLSS FG is not compared side by side yet.
11. **No self-update.** Distribution is the ZIP.
11. **Structure debt**: `app.cpp` is 6.3k lines. Settings and the stream sheet
    still repeat control code. The VFG and Reflex paths stay on purpose.

## Where things are

| File | Purpose |
| --- | --- |
| `src/app/app.cpp` / `.h` | Screens, intro, boot, loaders, Home, Settings, stream sheet, presets, health log, recovery |
| `src/ui/ui_renderer.cpp` / `.h` | D2D/DirectWrite drawing, text (`Text`, `TextEllipsis`, `TextWrapped`), caches |
| `src/media/pipeline.cpp` | Swapchain, pacing, upscale + sharpen + colour, graded readback |
| `src/media/shaders.h` | HLSL: upscalers, CAS, colour grade, HDR encode |
| `src/media/nv_dlss_sr.cpp` | DLSS SR/DLAA on D3D12, two output slots, async submit |
| `src/media/nv_dlssnr.cpp` | Closed NeuralScreen worker: SHM/RNSZ/WNDO protocol |
| `src/session/streaming.cpp` | chiaki session, decode thread, jitter buffer, feedback, disconnect |
| `src/platform/gamepad.cpp` | DualSense HID input/output, USB/BT reports, CRC |
| `src/platform/win32_window.cpp` | Window, DPI, refresh rate, fullscreen chrome |
| `src/session/profile.cpp` | JSON profile, defaults, clamps |
| `docs/08-app.md` | UI: tab map, Home, loaders, intro choreography |
| `docs/09-session.md` | Connect, disconnect, reconnect |
| `docs/26-network-setup.md` | Network measurements and checklist |
| `docs/27-color-and-sharpening.md` | Colour math and costs |
| `docs/28-code-audit-polish.md` | Code audit and polish record |
| `docs/15-frame-generation.md`, `docs/19-dlss-overview.md` | FG and DLSS design notes |

## Run and diagnose

```
build\Release\brp.exe                     # needs the DLLs next to it
%APPDATA%\BetterRemotePlay\session.json   # profile (back it up)
%APPDATA%\BetterRemotePlay\health.log     # 30 s telemetry
build\Release\nr-worker.log               # FG worker truth ([fg] displayed)
```

Rebuild: `cmake --build build-perf --config Release --target brp --parallel 8`,
then copy `build-perf\Release\brp.exe` to `build\Release\brp.exe`. Close the
running app first, or the copy fails with "being used by another process".

Frame measurement: `BRP_FRAME_PROBE=1` prints `[frame]` lines; `late` counts
ticks over 1.5 x budget per 480 frames.

## Next, in priority order

1. Test G-SYNC for FG 2x on 240 Hz (Windows setting only).
2. Capture the intro fade and its hand-off to boot.
3. Side-by-side quality test: Lanczos vs bicubic chroma, 75 vs 100 Mbps.
4. Decide on the game library: a separate client identity and terms, or no library.
5. Split `app.cpp` and share the Settings and sheet controls.

## RIFE setup

Home has a **Set up RIFE** pill (RIFE ready once done). It finds TensorRT 10.16,
records its path in `rife\runtime.txt`, downloads the 22 MB model and builds the
engine for the local GPU with trtexec. TensorRT itself still needs a browser
download from NVIDIA; the tool explains that instead of pretending otherwise.
Packages strip `runtime.txt` and include the model so another machine can
rebuild its engine.

## 0.4 - controller haptics (19 Nov 2026)

- Root cause of "no vibration": PS5 games use the haptics stream (3 kHz stereo
  int16 on a separate sink), not `CHIAKI_EVENT_RUMBLE`. The app never set the
  haptics sink, so the frames were dropped.
- Fix: `Streaming` subscribes with `chiaki_session_set_haptics_sink`, computes
  the per-channel envelope, and `App` converts it with chiaki-ng's arithmetic
  (threshold 100, intensity scale, minimum 512, `>> 8`). Values are marked as
  haptics so the gamepad layer does not scale twice.
- New setting: Settings > Controller > Haptics intensity (Off..Very strong).
- Motors are released 100 ms after the last frame; HID refresh while rumbling is
  10 ms.
- Measured over Bluetooth (DualSense fw 0x0630): 17 haptics frames/s, peak
  L=1023 R=924, `[hid] rumble` 2..12; vibration felt in game.
- Still absent: full DualSense haptics through its audio endpoint (USB-only).

## Updates (0.4)

- `update.exe` (`tools/brp_update`) applies a patch zip: `payload\`, `delete.txt`,
  `version.txt`. It refuses to run while `brp.exe` is open, copies changed files,
  removes obsolete ones, writes the version and relaunches.
- `tools\make_update.ps1 -BaselineZip <old zip> -NewFolder dist\BetterRemotePlay
  -Version 0.4` builds the patch by SHA-256 comparison.
- Measured 0.3 -> 0.4: 0.6 MB, 2 changed files; the patched folder matched the
  full package byte for byte.
- Documented in `docs/30-updates.md`.

## 0.4.2 - 5080 artifacts

- Pair-gap guard: no interpolation across a dropped frame (1.7x interval).
- Fast-motion guard: mean frame difference above 12 shows the real frame
  instead of a warped midpoint (the "vanishing head" on turns).
- Health log records `eng=RIFE|NVIDIA`, `rife=<fps>/<ms>` and `cad=<ms>`.
- `BRP_RIFE_SAFE_MOTION=0` disables the fast-motion guard for comparison.

## 0.4.3 - testing.log

- `%APPDATA%\BetterRemotePlay\testing.log`: header with OS, CPU, GPU, display,
  profile and RIFE paths, then one line per second while streaming (src,
  content, RIFE shown/ms, cadence avg/max, generated, queue, FG worker, late,
  stalls, present misses, decode, drops, lost, buffer, audio underruns) with a
  `!!` marker on anomalies. Rotates at 4 MB.
- Settings > About > Open diagnostics folder.
- Field reference in `docs/31-diagnostics.md`.

## 0.4.4 - guard correction from a real 5080 log

- The fast-motion guard (threshold 12) skipped generation during normal camera
  motion; `gen` fell to 3-14/s and output read as drops. Now opt-in
  (`BRP_RIFE_SAFE_MOTION=1`, threshold 18); the gap guard remains.
- `testing.log` adds `skip=gap/motion/pass` counters and `fs`.
- A 5080 report showed RIFE active with a 39.5 MB sm120 engine, 19-25 ms
  inference, cadence peaks 66-100 ms and growing audio underruns; delivery and
  audio are the open items, not the neural chain.

## 0.4.5 - final fixes from the 5080 report

- Testing log is optional (Settings > Debug, default off).
- Cadence stats are per second (they were cumulative without the probe env var).
- `ref=` refresh delta and `aud=` per-second underruns in the log.
- RIFE presentation takes one due frame per tick; a stall no longer skips one.
- From the report: motion guard disabled by default (0.4.4), RIFE 19-25 ms on the
  5080 with a 39.5 MB sm120 engine, `gen` back to 24-30/s; the remaining visual
  hitches line up with windowed DWM and audio underruns, not the neural chain.

## 0.4.6 - head artifact controls

- RIFE blend (default 20 %): generated frames mixed with the previous real
  frame, +3-5 ms. Reduces the "vanishing head" on fast turns.
- Motion safety (default On, threshold 22) only at very large frame changes.
- Both in Settings > Upscaling when the RIFE engine is selected.

## 0.4.7 - frozen NPCs

- Duplicate detection tightened from mean < 1.2 / 96 outliers to < 0.45 / 24:
  small independent motion in a static scene was being skipped as a duplicate.
- Default RIFE blend lowered 20 -> 10 % (ghosting of moving bodies); note in the
  UI explains the head/ghost trade-off.

## 0.4.8 - selectable RIFE model

- Settings > Upscaling > RIFE model: Lite 4.25 (default) or Heavy 4.26.
- Heavy needs one 6-channel input; the loader detects that layout from the
  engine and writes frame B into planes 3..5 of the same pinned buffer.
- Heavy engine `rife426h_1080.engine` is built on demand by "Set up RIFE"; the
  22 MB heavy ONNX is bundled in the package.
- Measured on a 4090 at 1080p: Heavy 23-25 ms per generated frame through the
  loader (12.4 ms GPU compute, p99 13.6 ms) versus 17.7 ms for Lite with the
  same loader. Still inside the 33 ms budget for 30 -> 60.
- Health log marks the model: `eng=RIFE-H`.

## 0.4.9 - NVIDIA DLSS FG actually smooth (verified 60 and 120)

Three app-side causes, all fixed:

- **Feed starvation:** covered mode used a wall-clock grid and skipped a slot
  whenever the decode queue was empty; static scenes were also duplicate-skipped
  to 12.5 fps. The worker received ~10-15 fps and throttled. It now gets every
  decoded frame immediately via `Streaming::TakeFrameNow`.
- **Multiplier chase:** the app asked for 4-6x, but the DLSS-G runtime refuses
  more than 2x (`4x refused 0xBAD00005`). It oscillated 3x-6x every 1.5 s. Now
  capped at 2 and changed at most every 2 s.
- **Overlay throttling:** reparenting the worker as a child made it think the
  app covered it and self-throttle. It is now a top-level topmost no-activate
  overlay, hidden when the app loses focus.

Measured (4090, 1080p, DLAA + FG, 1440p240 display):

- 30 -> 60: 59.4-60.6 fps, 0 drops/late/stalls, decode 8.4 ms, DLSS 0.74 ms.
- 60 -> 120: 119.8-120.2 fps, 0 drops/late/stalls, decode 7.9-8.6 ms.
- Worker log: `displayed 120.0 FPS (real + generated, 2x)`.

Honest limit: DLSS-G here is 2x only. 30 fps -> 60 with NVIDIA; 60 fps -> 120.
RIFE remains the better 30 -> 60 midpoint. Status map for the full AI engine
spec is in `docs/32-nvidia-ai-engine.md`.

## 0.5.0 - BetterRemotePlay Next packaged (Stages 5-11)

The package now contains both executables:

- `brp.exe` 0.5.0: the reference product (version bump only; feature set is
  0.4.9, including DLSS-G 60 -> 120 and the proven RIFE path).
- `brpnext.exe` 0.5.0-next: the rebuilt application.

`brpnext` working on the reference PS5:

- Discovery, PSN browser login + clipboard redirect, PIN pairing, rest-mode
  wake, connect/disconnect, auto reconnect (5 attempts) and device-loss stop.
- Full-window video through D3D11, WASAPI audio, DualSense input at 250 Hz.
- DLSS SR 1920x1080 -> 2560x1440.
- RIFE 30 -> 60: 29 generated/s, 58 shown/s from a 28.8 fps source.
- Independent layered overlay, typed settings, profiles, reset/import/export,
  session report.
- DLSS-G 60 -> 120 runs in `brpnext` through the NVIDIA worker
  (2x verified at 120.0 fps; see the DLSS-G section below).

Verified: update 0.4.9 -> 0.5.0, `files_differ=0`, 0 markers; both executables
launch from the clean package. Not done: automated four-case benchmark matrix,
4K display validation, multi-hour soak, and the RTX 5080 second-machine pass
for `brpnext`.

### 0.5.0 benchmark matrix (baseline, real PS5)

`Run benchmark` in Diagnostics (or `BRP_NEXT_BENCH=1`) runs four baseline cases
with 20 s between console sessions and writes `benchmark-<epoch>.json`:

| Requested | Target | Source fps | Shown fps | Decode | Loss |
| --- | --- | --- | --- | --- | --- |
| 30 | 1440p | 26.5 | 26.5 | 7.9 ms | 0.00 % |
| 30 | 2160p | 27.2 | 27.2 | 7.9 ms | 0.00 % |
| 60 | 1440p | 54.1 | 54.1 | 8.2 ms | 0.00 % |
| 60 | 2160p | 53.6 | 53.6 | 7.9 ms | 0.00 % |

Source is the console menu, so the 60 request yields ~54 unique fps; the 2160p
targets are rendered and fitted to the 1440p display. This is a real baseline
matrix, not a 4K/120 game validation.

### 0.5.0 UI pass

- Palette switched back to the reference app's PS5 violet family:
  background 120A20 -> 05030A, panel 1A1029, ink F7F3FF, accent B072FF,
  warm accent FF6BD6. The previous green NVIDIA theme is gone.
- Primary buttons use the reference style: translucent violet fill, warm
  border and ink text. Secondary buttons keep the glass surface.
- Home "AI capabilities" is now a five-tile row (DLSS SR, DLSS-G, RIFE Lite,
  RIFE Heavy, TensorRT) with a status dot, AVAILABLE/MISSING and a
  short detail; no more oversized pills.
- Profiles has a Quick Presets row: Native, 1440p Sharp, 1440p RIFE and Low
  latency, using the same primary/secondary buttons as the rest of the app.
- Title pill reads 0.5.0; sidebar footer reads Next 0.5.0.

### 0.5.0 frame-timing fix

Two causes of the "frame teleports once a second" report:

- `PresentVideo` advanced `video_phase_` with the *instantaneous measured*
  source fps (which jitters with decode/queue timing) instead of the requested
  source cadence. The phase slowly gained a slot and then skipped one, about
  once per second. It now uses `settings.streaming.fps`, which divides 240 Hz
  exactly at 30/60.
- The independent overlay was forcing `SetWindowPos(HWND_TOPMOST)` and
  `UpdateWindow` every 250 ms. It now only repositions when the corner/size
  changes and paints asynchronously.

Measured after the fix, native presentation, no SR/FG: `cad p50 16.7 ms,
p99 16.9 ms` at 57 fps source. The overlay now shows cadence live as
`cad p50/p99`.

### 0.5.0 manual Connect "already in use" fix

Pressing Connect manually could end with the console saying "Remote is already
in use" a few seconds later, even though Next had just established the session.
Cause: after the worker finished `Streaming::Start`, the state was `Connecting`
but `Snapshot.connecting` only reflected the worker flag, which was already
false. Auto-reconnect therefore thought the session had ended and issued a
second request, tearing down the first handshake.

Fixes:

- `Snapshot.connecting` is true for both the worker flag and the chiaki
  `Connecting` state.
- Auto-reconnect only runs in `idle` or `failed`, never while connecting.
- `StreamingService::Connect` refuses a second request while a session is
  connecting or streaming.
- "already in use" stops the automatic retry loop (manual retry still works
  after the console releases the old session).

### 0.5.0 DLSS Frame Generation in brpnext

`DlssFg` (`src/next/ai/dlss_fg.*`) owns the NVIDIA NeuralScreen worker and
speaks its present-mode protocol:

- it starts `nr/native/nvngx.dll` with `nvngx_dlssg.dll` beside it, asks for
  `WNDO` present mode, disables Neural Rendering (bypass) and sends the
  DLSS-G flags with the selected multiplier;
- a one-slot worker thread feeds it RGBA at the source cadence, so decode
  never blocks on the GPU worker;
- the worker's window is a top-level, topmost, never-activated overlay fitted
  to the client; `Esc` sends `WNDO` disable and stops the worker;
- the overlay reads `nr-worker.log` once a second, so the number shown is the
  worker's own `[fg] displayed ... FPS`, not a requested value. The AI page
  states that 2x is the clean cadence and that 3x-6x land near 150-165 fps.

Measured on the reference PS5, 1080p source, no SR:

```
[fg] 2x enabled at 1920x1080, format=28; flat depth and estimated motion (experimental)
[fg] displayed 120.0 FPS (real + generated, 2x)
source 57.8 fps | decode 8.52 ms | 100000 kbps
```

SR + DLSS-G: the 1440p SR output is read back to RGBA and fed to a worker
sized 2560x1440. Verified at `101.3 fps` generated from a 58.3 fps source;
the 1440p worker is the limit, so the UI does not promise 120 there.

The 1440p path costs the SR pass plus a 1440p readback per source frame; the
1080p path exists so the common case (stream at 1080p, FG at 2x) stays at the
clean 120. 30 fps sources reach 60, not 120, with the same 2x runtime.

Files: `src/next/ai/dlss_fg.h/.cpp`, `src/next/shell/next_app.cpp`
(`EnsureDlssFg`, `PresentVideo`, Watch overlay), `src/next/platform/system_info.*`
(worker/dll/proxy discovery), `src/next/shell/settings_controls.cpp` (backend,
multiplier, honest limits).

### 0.5.0 borderless fullscreen, clean Watch, double Esc

- `Win32Window` now owns borderless fullscreen: it saves the window style,
  placement and client size, switches to `WS_POPUP` on the current monitor,
  removes the DWM rounding/caption hit test, and restores the exact placement
  on exit. `F11` toggles it on any page.
- `Display > Fullscreen on play` (default on) enters fullscreen when Watch
  starts and restores the previous placement when Watch ends. If the window was
  already fullscreen before Watch, it stays fullscreen afterwards.
- The Watch view no longer paints the in-app top bar. The video is the whole
  surface; live stats are only in the optional independent overlay, and the
  DLSS-G worker window is refitted on every client-size change.
- `Esc` leaves Watch and shows `Esc again to disconnect`. A second `Esc` within
  2.5 s disconnects the session (`StreamConnection sending Disconnect`,
  `Session has quit`), stops RIFE/DLSS-G, closes audio and cancels reconnect.
- The Console page uses the same `DisconnectStream()` path, so the button and
  the shortcut cannot drift apart.

### 0.5.0 DLAA + DLSS-G latency pass

The DLAA selection had been mapped to a 1440p upscale: `EnsureDlssSr` always
returned 2560x1440, so "DLAA + DLSS-G 2x" fed the worker a 1440p frame, paid a
1440p readback and capped the generator near 100 fps. DLAA now means
input == output: 1920x1080 -> 1920x1080, NGX quality 5. The worker runs at its
clean 1080p case. Log: `[dlss] SR initialized 1920x1080 -> 1920x1080, quality 5`.

Three latency cuts on the same path:

- **Every decoded frame, newest first.** `Streaming::TakeNewestFrame` drops
  older queued frames and the DLSS-G path feeds each new frame as soon as it is
  decoded, without waiting for the 60 Hz source gate. The generator owns its
  output cadence, so the gate only added up to one source interval of delay.
- **No parent composition.** While the worker window covers the client the app
  no longer presents its own swapchain. DWM was composing a second full-size
  surface every vblank for nothing.
- **Wall-clock source gate.** Frames are due every `1/settings.streaming.fps`
  seconds rather than counting loop ticks, so the loop can be paced by a 1 ms
  timer when there is no parent vblank. Cadence is unchanged at 30/60.

Borderless fullscreen also got the small wins: `SWP_NOCOPYBITS` on the monitor
switch, no redundant foreground call, and the display card does not overlap
the mode note on multi-monitor machines.

Measured with the game in its 30 fps mode: `source 28.0 fps`, overlay
`DLSS-G 2x | 60.0 fps`, worker `2x enabled at 1920x1080`, no parent presents.
The same path on a 60 fps source is the earlier 120 fps case.

### 0.5.0 target frame rate removed

`ai.target_fps` was a leftover from the early design: it was displayed as
"target frame rate" and used in warnings, but it never drove execution.
DLSS-G output is `source fps * fg_multiplier`; RIFE is one midpoint (2x). The
control, the model field, serialization, legacy `fg_target` import and the
warnings around it were removed. Home and profile summaries now derive the
output from the source rate and the selected generator.

### 0.5.0 DLSS-G 2x uses the official runtime

The SM86 proxy is a quality trade: it raises the multiplier ceiling, but it
replaces the NVIDIA-signed `nvngx_dlssg.dll` and applies its own (optionally
lossy) kernels. For 2x, where the ceiling is not needed, the client now skips
the injection and lets the worker load the stock runtime:

```
[fg] the runtime reports a 2x multiplier ceiling
[fg] 2x enabled at 1920x1080, format=28; flat depth and estimated motion (experimental)
[fg] displayed 60.0 FPS (real + generated, 2x)
```

The proxy is injected only when the user asks for 3x-6x. This keeps the common
2x path on NVIDIA's own blending numerics. Phase telemetry with `NS_PHASE=1`
shows `generated shown 60 of 60 (late 0, superseded 0)`, so the remaining
"painted" look is the flat-depth/estimated-motion model, not pacing.

Tested 4x from a 30 fps source: it asks for 120, but the worker displayed
~94 fps with 48-53 late generated frames per 2 s, so 4x is not a smooth path.
A 30 fps source tops out at a clean 60 with 2x; 120 needs a 60 fps source.

### 0.5.0 remaining latency cuts

- The DLSS-G loop no longer polls with a 1 ms sleep. `Streaming` exposes a
  frame condition variable; while the worker covers the screen the loop wakes
  on the decoded frame (4 ms timeout only for overlay/window upkeep). The
  measured CPU dropped from ~2.0 % to ~1.8 % of a 32-thread machine, and the
  wake-to-submit path lost its polling delay.
- The worker's GPU queues are created at high priority
  (`NS_GPU_QUEUE_PRIORITY=high`, confirmed by `[gpu] command queue priority
  high`), so motion/FG work goes ahead of ordinary desktop GPU work. It does
  not request the privileged realtime class.
- Phase telemetry stays clean after both changes:
  `generated shown 60 of 60 (late 0, superseded 0)`.

What is left is not app-side queueing: the DLAA pass plus its 8 MB readback
(~one GPU pass) before the generator, the generator's own one-frame compositor
latency, and DWM composition for the worker window. Turning super resolution
off removes the readback; it is the only remaining selectable cost.

## 1.0.0 - First stable release

- Version bumped to 1.0.0 in the application, CMake project and package.
- Visual redesign: flat caption/toolbar icon buttons, a new title bar, grouped sidebar with session card, page header
  system, pipeline flow on Home, AI sub-tabs (Super Resolution / Frame
  Generation / GPU), preset cards in Profiles and a first-class About page.
- Interactive overlay control center (F10): Overview, Super Resolution, Frame
  Generation, Display, Performance and Profile tabs. Mouse and forwarded
  keyboard input; every action goes through `NextApp::Apply` and reports the
  scope that actually applied. Compact mode remains click-through.
- Central runtime classification (`app/runtime_settings.*`) for immediate,
  frame-boundary, pipeline, stream-restart and application-restart changes.
  Transport changes show `RECONNECT REQUIRED`; they do not claim to apply live.
- Fixed: RIFE thread lifetime on exit/leave Watch/device loss; DLSS SR command
  list recovery after a failed evaluation; unchecked D3D12 queue creation;
  RIFE failure loop and blend/motion safety; audio pointer race and shutdown
  order; settings/profile restore on failed replacement.
- Removed inert UI controls (codec/HDR/source resolution, VRR, audio volume,
  controller toggles, GPU tuning, monitor/mode controls, start-minimized/tray).
  Streaming bitrate/decoder/buffer/fps, V-Sync and sharpening are now wired.
- DLSS-G 2x uses the NVIDIA-signed runtime; the SM86 proxy is injected only for
  3x-6x and only when present. SR 2160p cannot feed a capped DLSS-G worker.
- Release documentation: `RELEASE_AUDIT.md`, `KNOWN_ISSUES.md`,
  `KNOWN_LIMITATIONS.md`, `V1_RELEASE_NOTES.md`, `V1_RELEASE_CHECKLIST.md`,
  `USER_GUIDE.md`, `ARCHITECTURE.md`.
- Tests: `brp_next_tests` ALL PASS (82 assertions). Package hashes are in the
  release section below.

### 1.0.0 package

- Standalone: `dist/BetterRemotePlay-1.0.0-standalone.zip`
  SHA256 `5C244C6F6C6BCD527CBBF648D41A57EA3695D688D8857A45E1774BF37C581EBA`
- Update from 0.4.9: `dist/BetterRemotePlay-1.0.0-update.exe` (1.34 MB)
  SHA256 `2AAB793E0C862858C75D4A64AF78AE16C3AE6DA5C27E2DA2759729C87BCDEF45`
- `brpnext.exe` SHA256
  `B9EE2F3AA967C8110B47F093DD7047B985EBFCE9D189AD1D072C9C50692E1CB4`
- `brp.exe` SHA256
  `7256F12F553A7F76E99CEED378B4C839FA8EEFCEF337AE9785132A32F9D8B3E8`
- Patch verification: `version=1.0.0`, 0 markers, `files_differ=0`.

### 1.0.0 input-to-camera latency pass

Watch no longer uses a source phase gate or a presentation buffer. Every decoded
frame is taken as the newest available on the next shell tick and rendered
immediately; if no new frame exists, the last surface is presented without
re-uploading pixels. This removes up to two source intervals of app-side queue
at 30/60 fps. The frame-buffer control was removed from the UI because it no
longer describes a real presentation queue.

What remains is not app-side queueing: console encode + network, the console's
input processing, one source interval for any frame generator, and DWM
composition for the worker/window. The AI page now states that frame generation
adds at least one source interval and that disabling it gives the lowest
input-to-camera delay.

## 1.0.1 - Input-to-camera latency fix

- Watch no longer uses a source phase gate or a presentation buffer: the newest
  decoded frame is taken every shell tick and shown immediately.
- The controller thread matches the reference app: MMCSS `Games`,
  `THREAD_PRIORITY_HIGHEST` and a high-resolution 4 ms waitable timer instead of
  `sleep_for`.
- Watch presents with V-Sync (`PresentNow(true)`) again, matching `brp.exe` and
  preventing the waitable loop from running unthrottled when the old V-Sync
  setting was off.
- The frame-buffer control was removed from the UI; it no longer described a
  real queue.
- Frame generation still adds at least one source interval; the AI page says so.

### 1.0.1 package

- Standalone: `dist/BetterRemotePlay-1.0.1-standalone.zip`
  SHA256 `DC84A167DCD6C5EF705DCE75D4507FEBA052E6A4DC3A7079084018FCDC0E2282`
- Update from **1.0.0**: `dist/BetterRemotePlay-1.0.1-update.exe` (0.73 MB)
  SHA256 `4214391F29F3EA7E9E4B735CA8663CFA924CD0836C1DAFB5EC6D5A0C4C406FD4`
- `brpnext.exe` SHA256
  `EE842D3F92254541B1359A22B50D5C6B82FF749E56C02BA94F2F0DB17023F27F`
- Patch verification: `version=1.0.1`, 0 markers, `files_differ=0`.

## 1.0.2 - Upscale / frame-generation / streaming polish

- DLSS Super Resolution now uses NVIDIA's real render-scale ladder instead of a
  cosmetic preset hint:
  Ultra Performance 33%, Performance 50%, Balanced 58%, Quality 66%,
  Ultra Quality 77%, DLAA 100%.
  The 1080p stream is area-downscaled to the selected input size before NGX,
  and DLSS reconstructs 1440p/2160p from it. Verified live:
  `[dlss] SR initialized 1688x950 -> 2560x1440, quality 2`.
- The AI page and overlay show the effective render scale and both dimensions.
  DLAA remains native 1080p AA and the lowest-latency generator input.
- Streaming snapshots now report the actual decoder (D3D11VA/D3D12VA/Vulkan/
  CUDA/Software) instead of only the settings value; the Home decode step uses
  the live value.
- Frame generation keeps the 2x NVIDIA-signed runtime, the optional SM86 proxy
  for 3x-6x, immediate newest-frame feeding and the 1440p worker cap.
- Tests: brp_next_tests ALL PASS (82).

### 1.0.2 package

- Standalone: `dist/BetterRemotePlay-1.0.2-standalone.zip`
  SHA256 `F62C1F0145A98231A4507B936861130A8CFFA8E0A4869070449D9636B143C282`
- Update **1.0.1 -> 1.0.2**:
  `dist/BetterRemotePlay-1.0.1-to-1.0.2-update.exe` (1.35 MB)
  SHA256 `56CC6A18C6775A15B7F6D549520C82052A2AF20B2778F61730E59C4A50EDE548`
- Update **1.0.0 -> 1.0.2**:
  `dist/BetterRemotePlay-1.0.0-to-1.0.2-update.exe` (1.35 MB)
  SHA256 `C95433CF5BF8EBB5C5CB0E2CFFD13C53F3D1A526464018E172551405BD4892DB`
- `brpnext.exe` SHA256
  `EB3E957FD9FFB6CB22EF927F665794A11FD0B5E222E7AF21D61B09283F525445`
- Patch 1.0.1 -> 1.0.2 verified: `version=1.0.2`, 0 markers,
  `files_differ=0`.

## 1.0.3 - real-time presentation fix (the "frame skip" pass)

The 1.0.2 decoder and cadence were fine; the presentation loop was not. Two
faults combined into exactly the "it skips even though the FPS counter is
right" feeling:

1. **Two presents per vblank.** Watch waited on the swapchain waitable with a
   fixed 8 ms timeout. On a 240 Hz display one vblank is 4.17 ms, so the loop
   ran twice per refresh. The second iteration had no new decoded frame and
   called `Present()` without drawing. With a flip chain that is not a repeat
   of the current image: it is the backbuffer from two presents ago. Every
   other vblank showed an older frame.
2. **No redraw on an empty tick.** Even at 60 Hz the old path returned before
   `Presenter::Render()` when the decoder had nothing new, so the same
   stale-backbuffer flip happened on every missed decode tick.

Both are fixed by presenting once per vblank and redrawing the standing frame
on every tick. The loop now uses `clamp(2000/refresh_hz, 8, 40)` ms as the
waitable timeout, exactly like the reference app, instead of a fixed 8 ms.

Also in this pass:

- `DownscaleRgba` rewritten from two passes with per-pixel integer divisions
  and a 14 MB intermediate to a one-pass row accumulator with precomputed spans
  and reciprocal-table rounding. The 1080p -> 1688x950 (66%) step measured
  **15.9 ms -> 4.0 ms** of presentation-thread CPU.
- The presentation thread runs `THREAD_PRIORITY_ABOVE_NORMAL`; the decode
  thread does the same, so a decoded frame is not delayed by background work.
- DLSS-G + SR no longer waits for the just-submitted NGX evaluation before the
  readback. `ReadbackCompleted()` copies the already-finished slot, removing a
  GPU fence wait from every generated frame (measured 8.5 ms total CPU per
  source frame for SR -> 1440p readback -> worker submit).
- Diagnostics and the overlay Performance page now show the **measured scanout
  rate**, `present misses/s` and the loop p99, so a stutter can be separated
  from a low frame rate instead of guessed.
- `BRP_NEXT_FRAME_PROBE=1` prints the same numbers once per second to stderr.

Measured live on the reference machine (RTX 4090, 2560x1440 @ 240 Hz,
1080p HEVC, DLSS SR Quality 66% -> 1440p):

```
60 fps source: cadence p50 16.7  p99 16.9 ms | scanout 240.0 Hz | present misses 0/s
30 fps source: cadence p50 33.3  p99 38.2 ms | scanout 240.0 Hz | present misses 0/s
SR CPU per frame: 3.9-4.3 ms (was 15.6-16.1 ms)
DLSS-G worker: 59.9-60.1 fps displayed, 2x, 30 fps source
```

### 1.0.3 package

- Standalone: `dist/BetterRemotePlay-1.0.3-standalone.zip`
  SHA256 `C7DE1022CBA3F971EEEF73A10FA2B76CB31542F2A2B30B3DF22AA68E0EFDCE9F`
- Update **1.0.2 -> 1.0.3**:
  `dist/BetterRemotePlay-1.0.2-to-1.0.3-update.exe` (1.35 MB)
  SHA256 `19A072D36FD59EA539564CCB4C707ECC9EFB24F0340F7A5168F1C340C2D15447`
- Update **1.0.1 -> 1.0.3**:
  `dist/BetterRemotePlay-1.0.1-to-1.0.3-update.exe` (1.35 MB)
  SHA256 `249CF705B6B84E3DF287B17B3E7D4E5975454B8253206759CDB011A963CFC37C`
- Update **1.0.0 -> 1.0.3**:
  `dist/BetterRemotePlay-1.0.0-to-1.0.3-update.exe` (1.35 MB)
  SHA256 `C22C0D7CCE105D051A736DE83D6DD3B37C9BA0D56220E6ED458DF301AAB63FC1`
- `brpnext.exe` SHA256
  `62F8A3D841E60536FB789D31B95855559C5A60D23028233E47E6D8F619824ACA`
- `brp.exe` SHA256
  `7BBAC7002E9F68800FE2ACF29ABDCCD5193A9C57D7D8CC09D12A5BBB7C0CA482`
- Patch 1.0.2 -> 1.0.3 verified: `version=1.0.3`, 0 markers,
  `files_differ=0`.
- Tests: `brp_next_tests` ALL PASS (82).

## 1.0.4 - latency and in-order delivery pass

1.0.3 fixed the presentation cadence but introduced one regression and still
used the wrong frame-selection policy for unstable links:

- **The DLSS-G + SR readback added one source interval.** 1.0.3 copied the
  "previous completed" SR slot without waiting. That slot is one frame older
  than the frame just submitted, so the generator input moved back by 16.7 ms
  at 60 fps (33 ms at 30 fps). The FG path is back on `Readback()`, which waits
  for the frame it just submitted. This is the delay that 1.0.3 added.
- **The video path now plays frames in order, not "newest wins".** Both the
  parent path and the generator feed pop the oldest decoded frame. A short
  network or decode burst is played out instead of being skipped; a backlog
  above the cap (2 in the parent, 3 for the generator) is dropped so latency
  cannot grow. This matches the reference app's delivery policy.
- **The parent path pulls on a rational display phase** (`fps / refresh`), so
  at 240 Hz and 60 fps it changes content every four refreshes with no
  wall-clock drift. The standing frame is still redrawn on every vblank.
- **A rigid generator grid was measured and rejected.** Holding one frame and
  feeding the DLSS-G worker on a fixed slot starved it (117-118 fps displayed)
  and made the cadence worse. The immediate ordered feed is kept: 119.7-119.9
  fps displayed at 60 fps input, 2x, DLAA, 1440p.
- Frame-generation input remains the synchronous newest SR output; the
  async "completed slot" experiment does not ship.

Measured on the reference machine after this pass (2560x1440 @ 240 Hz,
1080p HEVC, DLAA):

```
no generator: cadence p50 16.7  p99 17.0 ms | scanout 239.8 Hz | present misses 0/s
DLSS-G 2x:    fed 16.6 ms avg | worker 119.7 fps displayed | 0 feed drops
```

### 1.0.4 package

- Standalone: `dist/BetterRemotePlay-1.0.4-standalone.zip`
  SHA256 `2DC214BB5B1A09C617E3A0BF930379F9C43D29E264CA81099A5D6CF73BF88993`
- Update **1.0.3 -> 1.0.4**:
  `dist/BetterRemotePlay-1.0.3-to-1.0.4-update.exe` (1.35 MB)
  SHA256 `02A9501FA442653433B7962C8B6EE684AB00376105825D4BEE5A3DFC07E2EC53`
- Update **1.0.2 -> 1.0.4**:
  `dist/BetterRemotePlay-1.0.2-to-1.0.4-update.exe` (1.35 MB)
  SHA256 `B04D998FEE5DB9BF4F18C4AE8AF853D59FC8BC4CA9589A733A9546BD44228F32`
- `brpnext.exe` SHA256
  `CB29FDAC5D87BF5608285672A84F5EE7413482118AD558BB99F9A8495B795DF2`
- Patch 1.0.3 -> 1.0.4 verified: `version=1.0.4`, 0 markers,
  `files_differ=0`.
- Tests: `brp_next_tests` ALL PASS (82).
- Live checks: 60 fps source, no generator: cadence p50 16.7 / p99 17.0 ms,
  scanout 239.8 Hz, 0 present misses/s. 30 fps source + DLSS-G 2x: worker
  59.9-60.1 fps steady. 60 fps source + DLSS-G 2x: worker 118-120 fps; the
  dips are the worker at the edge, not the parent presenter.

## 1.0.5 - once-per-second hitch pass

The reported symptom was one hitch roughly every second, not continuous
jitter. Three 1 Hz operations were sharing a thread with presentation or with
the generator feed:

- `DlssFg::FrameWindow()` polled `nr-worker.log` once per second **on the
  presentation/feed thread** (`last_write_time`, open, seek, read 8 KB). The
  read now runs on its own monitor thread and only publishes atomics.
- The worker's stderr was `nr-worker.log`, so the worker itself wrote to a
  growing text file once per second (`[fg] displayed ...`), plus a
  `[video] delivered frame` line twice per second. By default stderr now goes
  to `NUL`; set `BRP_FG_LOG=1` to restore `nr-worker.log` for diagnostics.
  With the log on, the worker measured 120.0/119.6 fps; the UI falls back to
  the requested output when no log is present.
- `ProbeIfNeeded()` enumerated the GPU/monitors every 30 seconds on the
  presentation thread. It is now skipped for the whole Watch session.

Measured after the change (60 fps source, DLAA, DLSS-G 2x, 1440p window):

```
parent loop: p50 1.7 ms, p99 20.4 ms (SR frame), max 20.8 ms, no 1 Hz spike
feed: 60 frames/s held, worker 1920x1080, 2x
```

### 1.0.5 package

- Standalone: `dist/BetterRemotePlay-1.0.5-standalone.zip`
  SHA256 `2C81F8E3E7325216EC53FB19CC7F0EE1B00EAAA7C4CF95C53F3D7853F0CE8382`
- Update **1.0.4 -> 1.0.5**:
  `dist/BetterRemotePlay-1.0.4-to-1.0.5-update.exe` (1.35 MB)
  SHA256 `6E153FD4D143022C39774F35E99E80D31EB60858C98D15BABEF092EBE6A1B131`
- Update **1.0.3 -> 1.0.5**:
  `dist/BetterRemotePlay-1.0.3-to-1.0.5-update.exe` (1.35 MB)
  SHA256 `9DBA28BF88E4B06CD84B6D0AEACBE15DDD1E5475E8DBAEBCA3E2EC256702E247`
- `brpnext.exe` SHA256
  `7437A124F5DE2ADD69631D43A490CF64CD090F093CB74901A56C3D81CAFB2C7E`
- Patch 1.0.4 -> 1.0.5 verified: `version=1.0.5`, 0 markers,
  `files_differ=0`.
- Tests: `brp_next_tests` ALL PASS (82).

## 1.0.6 - dedicated stream window

The stream no longer shares the application window. Watch now creates a
separate top-level window with its own D3D11 presenter and swapchain; the main
application window minimizes for the duration of the session and returns when
Watch ends.

- `Win32Window` can create more than one window. The class registration is now
  idempotent (`ERROR_CLASS_ALREADY_EXISTS` is not a failure).
- The stream window is video-only: no Direct2D pass, no menu swapchain, no
  custom title bar, borderless fullscreen by default (or a centred window when
  `fullscreen_on_play` is off).
- DLSS SR is created on the stream presenter's device. On entering Watch the
  old feature is shut down and rebuilt for that device.
- DLSS-G keeps its own presenter window, now fitted to the stream window; the
  overlay anchors to the stream window and F10/F11/Esc are read from it.
- Closing the stream window (Alt+F4) exits Watch and restores the main window;
  it never terminates the process. `Esc` exits Watch, a second `Esc` within
  2.5 s disconnects.
- The main window keeps its own presenter/renderer for the menus, so returning
  from a session does not rebuild the UI.

Verified live: main window minimized (`IsIconic=true`), separate
`BetterRemotePlay Stream` window visible at 2560x1440, 60 fps source, DLAA,
DLSS-G 2x; Esc restored the main window and hid the stream window.

### 1.0.6 follow-up details

- Entering Watch creates the stream surface once. Exiting hides it and keeps the
  DLSS-G worker alive; the next Watch resumes the same worker instead of paying
  the worker start/stop cost. Re-entry measured ~0.3 s to the stream window.
- `NvDlssnr::ReadExact()` now waits on the worker process handle as well as the
  pipe. A terminated worker wakes a blocked `Process()` immediately instead of
  spinning until the 4 s timeout.
- `NvDlssnr::Stop()` gives the worker 100 ms and then terminates it; the old
  5 s wait was the reason leaving Watch could freeze the UI.
- The stream window closes with Alt+F4/WM_CLOSE by exiting Watch, not by
  terminating the app (`quit_on_destroy=false` + close handler).

Measured: Watch -> Esc -> main window restored and stream hidden in **339 ms**
(previous build 2853 ms), with the worker process intentionally left running.

### 1.0.6 package

- Standalone: `dist/BetterRemotePlay-1.0.6-standalone.zip`
  SHA256 `CD1C965B04877387961E6CB724C4B596C515F7A695B0248F2A830B5484A1F0D9`
- Update **1.0.5 -> 1.0.6**:
  `dist/BetterRemotePlay-1.0.5-to-1.0.6-update.exe` (1.35 MB)
  SHA256 `420AD361C2CAF49CC001107693C9D23B0BD6C7FE69D6BB0AD2E49AE8E265E103`
- Update **1.0.4 -> 1.0.6**:
  `dist/BetterRemotePlay-1.0.4-to-1.0.6-update.exe` (1.35 MB)
  SHA256 `2CF3188F799209BE9FA5E2205FFB89A24BF8C813A365EA90F94A0529FE91930B`
- `brpnext.exe` SHA256
  `AB4C033488CC69BE07F4D2D8E074E66D4DF62B04F2C57C10FFE875280BD1D261`
- Patch 1.0.5 -> 1.0.6 verified: `version=1.0.6`, 0 markers,
  `files_differ=0`.
- Tests: `brp_next_tests` ALL PASS (82).

## 1.0.7 - 30 fps cadence and the frame buffer

GTA VI is locked at 30 fps, so the stream runs at a 30 fps source. At 30 fps on
a 240 Hz display each frame must stay on screen for exactly **8 refreshes**.
The telemetry now records that hold distribution directly
(`screen hold = PresentedRefreshCount delta between content changes`).

Measured before this pass, 30 fps / 240 Hz / no generator:

```
holds 7:8  8:15  9:7      <- ~50% of frames were one refresh short or long
cadence p50 33.4  p99 37.6 ms
```

That 7-9-7-9 wobble is visible as uneven motion even though the average is
30 fps. Two policies were measured:

- **Newest frame every tick** reacts to arrival timing, so network/decode
  jitter (p99 37.6 ms) is copied to the screen.
- **Unprimed phase gate** (1.0.4-1.0.6) pulls on the source slot but has no
  spare frame; a frame a few milliseconds late leaves the screen on the
  previous image for a full interval (16 refreshes).

The fixed policy is a **primed, phase-locked queue**: the decoded queue keeps
one spare frame, and one frame is presented per source slot on the display
grid. A late frame is covered by the spare; the content still changes exactly
every 8 refreshes. Frame-buffer 0 keeps the old newest-frame behavior.

Measured after, same scene:

```
holds 7:0  8:30  9:0  10:0  16:0       (every second)
cadence p50 33.4  p99 33.6 ms
dec 9.0-9.3 / max 10.0 ms
```

The DLSS-G feed uses the same `PullFrame()` clock. 30 fps source + buffer 1 +
DLSS-G 2x: feed cadence p99 35.6 ms, worker 59.9-60.2 fps.

Settings: `Streaming > Frame buffer` is back (Off / 1 / 2 / 3 frames). Default
and the profile value are **1 frame**; it costs one source interval (33 ms at
30 fps) and is what removes the wobble. The in-app Diagnostics line now shows
the dominant hold, e.g. `holds 8v 100%`.

### 1.0.7 package

- Standalone: `dist/BetterRemotePlay-1.0.7-standalone.zip`
  SHA256 `487B674FB5328390D44735F70464E4E01ED9ECEC2CF471C940249A1132511E68`
- Update **1.0.6 -> 1.0.7**:
  `dist/BetterRemotePlay-1.0.6-to-1.0.7-update.exe` (0.74 MB)
  SHA256 `FD9A93F8A28814A049A6E45FE25653840AD4F2B29B88CA926B19CD92DBBFC08C`
- `brpnext.exe` SHA256
  `671F90B64B458A36AFC9DF5BDA98B4317C1C2F3EAAE2212B14D7D9CF786165C4`
- Patch 1.0.6 -> 1.0.7 verified: `version=1.0.7`, 0 markers,
  `files_differ=0`.
- Tests: `brp_next_tests` ALL PASS (82).
- Profile after the pass: source 30 fps, frame buffer 1, DLAA, DLSS-G 2x.

## 1.0.8 - SR latency at 30 fps and the SR/FG matrix

With the frame buffer doing the smoothing, the asynchronous SR submit (which
returns the previous output to avoid blocking) is no longer the right trade at
30 fps: the buffer already adds one interval, and the async SR adds a second.
When `frame_buffer > 0` and the source is 30 fps or lower, SR now evaluates and
waits for the current output (`Process(..., synchronous=true)`). The 10 ms NGX
wait fits the 33 ms budget, and the hold distribution stays `8:30`.

At 60 fps the synchronous path is not used (the wait would eat most of the
16.7 ms budget); the two-slot async path stays.

Measured 30 fps / 240 Hz / DLAA / buffer 1 / no generator:

```
holds 8:30 every second, cadence p99 33.9 ms, present misses 0/s
SR (synchronous) 10.2-11.0 ms per frame, decode 7.7-8.4 ms
```

SR/FG matrix measured with the current fixes (1080p HEVC, DLAA, 240 Hz):

| Case | Result |
| --- | --- |
| 30 fps + buffer 1, no FG | holds 8:30, even 30 fps |
| 30 fps + buffer 1 + DLSS-G 2x | worker 59.9-60.2 fps, feed p99 35.7 ms |
| 30 fps + buffer 1 + DLSS-G 4x (SM86) | worker 92-94 fps, does not divide 240 Hz |
| 60 fps + buffer 1, no FG | holds 4:30, even 60 fps |
| 60 fps + buffer 1 + DLSS-G 2x | worker 118-120 fps |

Guidance now encoded in the UI notes: for a 30 fps game use DLSS-G **2x**
(60 fps out), not 4x; DLAA is the cleanest generator input; keep the frame
buffer at 1. 4x is only worth it at 1080p on a 120/144 Hz target, where it can
sustain ~120, and it is not offered as smooth on 240 Hz.

Remaining last-percent judder with DLSS-G comes from the closed worker's own
presenter (it is compositor-paced, latency 1) on a 240 Hz panel. A full-screen
`CEF-OSC-WIDGET` capture overlay was present in several test runs and can also
add periodic composition hitches; close OBS/Streamlabs/browser overlays when
measuring.

### 1.0.8 package

- Standalone: `dist/BetterRemotePlay-1.0.8-standalone.zip`
  SHA256 `C1E0AB23E513E55DD159B42822F1CF24465A4A3B8D73DFCB2B84FBDB976AE5AE`
- Update **1.0.7 -> 1.0.8**:
  `dist/BetterRemotePlay-1.0.7-to-1.0.8-update.exe` (1.35 MB)
  SHA256 `BFD0E16AA7E00D428C5355991C179A50789C1D6CF30F46174D76209B557623F8`
- `brpnext.exe` SHA256
  `C2FD28DCAB8E5C3734CAE2A95A2ECC13C391C168E54C9815A4C0AEFF0AC6907A`
- Patch 1.0.7 -> 1.0.8 verified: `version=1.0.8`, 0 markers,
  `files_differ=0`.
- Tests: `brp_next_tests` ALL PASS (82).
- Profile: 30 fps, buffer 1, DLAA, DLSS-G 2x.

## 1.0.9 - control center, Esc, input rate and settings cleanup

The F10 control center never opened from 1.0.0 through 1.0.8. The cause was in
`Win32Window`: F10 is a system key and arrives as `WM_SYSKEYDOWN`, while the
message handler only recorded `WM_KEYDOWN`. F10, F11 and F12 are now accepted
from `WM_SYSKEYDOWN`; the panel opens in every build after this.

### Esc / disconnect

- The first Esc **arms** disconnect and shows a compact banner over the stream
  (`DISCONNECT ARMED`, `ESC AGAIN / DISCONNECT`, `F10 / CANCEL`). It no longer
  leaves Watch, so the banner is visible in the picture.
- The second Esc disconnects, with **no 2.5 s timeout**. Alt-Tab and returning
  cannot lose the arm.
- A `WH_KEYBOARD_LL` Esc hook is active while watching or armed. It consumes
  Esc only when the foreground window is ours or the DLSS-G worker, so Esc in
  another application is untouched. This fixes the case where the worker's
  topmost window owns the keyboard focus.
- Esc while the control center is open closes the panel **and arms** disconnect
  in the same press.
- F10 cancels the arm and opens the panel. Leave Watch without disconnecting
  with **Alt+F4** or `Profile > Leave Watch`.

### Controller rate

`controller.poll_hz` is selectable 125 / 250 / 500 / 1000 Hz in
Settings > Controller and in the overlay Pad tab. The pad thread uses a
high-resolution waitable timer whose period is recomputed live. 1000 Hz is
bounded by the controller/HID stack; USB is the most consistent, Bluetooth
needs a good host (DSX reaches 1000 Hz).

### Settings and overlay

- Settings now contains only the application-wide extras: **General, Audio,
  Controller, Advanced**. Streaming, AI Video Engine, Display and Overlay live
  on their own pages, so the same control no longer appears twice.
- The control center is a **side-docked full-height panel** on the chosen side
  (`overlay.side`: Left/Right) and vertical anchor (Top/Middle/Bottom). Compact
  mode and the disconnect banner use the same side.
- Tabs: Info, SR, FG, Video, Perf, Stream, Audio, Pad, Profile. New live rows:
  sharpening, overlay side/anchor, source fps, bitrate, decoder, frame buffer,
  audio buffer, rumble, poll rate. Rows that cannot apply mid-session are
  marked in warning colour with `STREAM RESTART required` or `NEXT SESSION`.
- 30 fps SR uses the synchronous evaluation from 1.0.8; the frame buffer keeps
  the 8-refresh cadence.

### 1.0.9 package

- Standalone: `dist/BetterRemotePlay-1.0.9-standalone.zip`
  SHA256 `A2B2A8B0DBFD9B0DED807805647BD2C2547057242F1D8245C013FE28551AA044`
- Update **1.0.8 -> 1.0.9**:
  `dist/BetterRemotePlay-1.0.8-to-1.0.9-update.exe` (1.36 MB)
  SHA256 `6EE0551312D8058F2DDD7DFA51758D886DB4359B11B9924D11E3A6CC223129DD`
- `brpnext.exe` SHA256
  `A5C6734B292091A4B1E1C9DE7555CE368180664BD4C2ECB56849DDE65E039CCE`
- Patch 1.0.8 -> 1.0.9 verified: `version=1.0.9`, 0 markers,
  `files_differ=0`.
- Tests: `brp_next_tests` ALL PASS (82).
- Profile: 30 fps, frame buffer 1, DLAA, DLSS-G 2x, controller poll 1000 Hz.

## 1.2.0 - stable: final frame pacing pass

- **Generator submit pacer: implemented, then removed.** A due-time submit
  slot accumulated drift when the source clock was slightly slower than the
  nominal rate (GTA VI is 29.97 fps): the target moved ahead of the real clock,
  the worker held frames longer and longer, and after a few minutes the focused
  worker window looked frozen while audio continued. The final build uses the
  primed, phase-locked pull (one spare frame) with an immediate `Submit`.
  Focused 45 s run: feed p99 35.6-35.8 ms, worker 59.9-60.0 fps, UI loop p99
  5.5-6.0 ms.
- **Focus loss fallback.** When the worker is attached but no longer covering
  (Alt-Tab, another full-screen overlay), the parent presenter keeps drawing
  the newest decoded frame instead of freezing on the last presented image.
  `Alt-Tab` now leaves a live stream window behind the other application.
- **NVIDIA App overlay** (`NVIDIA Overlay.exe`, class `CEF-OSC-WIDGET`) is
  hidden at Watch start and re-checked once per second while streaming. It is
  a full-screen CEF window that reappeared after a manual hide and can add
  composition hitches.
- **F10** now works (system key via `WM_SYSKEYDOWN`); **Esc** arms persistently
  with a banner and a global foreground-only hook, so Alt-Tab cannot lose it.
- **Controller poll** is selectable 125/250/500/1000 Hz, applied live by the pad
  thread's high-resolution timer.
- **Settings** holds only General/Audio/Controller/Advanced; the other pages
  own their controls.
- **Control center** is side-docked (Left/Right, Top/Middle/Bottom) with tabs
  Info, SR, FG, Video, Perf, Stream, Audio, Pad, Profile. Session-only changes
  are marked `STREAM RESTART required` / `NEXT SESSION` in warning colour.
- 30 fps SR is synchronous with the buffer; the base path keeps `holds 8:30`.
- Final profile: 30 fps, buffer 1, DLAA, DLSS-G 2x, controller 1000 Hz.

### 1.2.0 package

- Standalone: `dist/BetterRemotePlay-1.2.0-standalone.zip`
  SHA256 `773376474A7D5187EFB9719DFF850AE0B34BEE2D70241D8E130D35514E53369C`
- Update **1.0.9 -> 1.2.0**:
  `dist/BetterRemotePlay-1.0.9-to-1.2.0-update.exe` (0.75 MB)
  SHA256 `38C2E5574136B5382C3944202AF3B9D7F6D95AE84EC14570BE44D68F287CEC2E`
- `brpnext.exe` SHA256
  `13338189ADD18657BE0550E7BA98C4FCB6076523C3EAE44B69C56D2CE89B522F`
- Patch 1.0.9 -> 1.2.0 verified: `version=1.2.0`, 0 markers,
  `files_differ=0`.
- Tests: `brp_next_tests` ALL PASS (82).
- Final profile: 30 fps, frame buffer 1, DLAA, DLSS-G 2x, controller 1000 Hz.

## 2.0.0 - Rust/GPUI shell, headless C++ engine

- **Platform move.** The shell is now Rust + GPUI (`ui/`, crate `brpnext-ui`),
  the same Rust/GPU-accelerated UI family the new Discord client is being
  ported to. Streaming, decode, audio, input, DLSS and the in-stream overlay
  stay in C++: correctness and pacing are unchanged, only the front-end is new.
- **Process split.** `brpnext.exe` (shell) spawns `brpnext-engine.exe --engine
  --bridge-port N`. The engine hides its legacy window, keeps its message pump,
  stream window and overlay, and talks to the shell over a loopback TCP line
  protocol (port chosen by the shell, so no fixed port and no firewall prompt
  for the bridge). `brpnext-classic.exe` keeps the 1.2 C++ shell as a fallback.
- **Single settings file.** The shell reads and writes the same flat
  `next-settings.json` (dotted keys). Unknown keys are preserved, so the engine
  never loses a setting the shell does not know about.
- **UI.** Top tabs (Home, Stream, Video, Audio, Controller, Neural, Overlay,
  Advanced), Basic/Advanced switch, Inter + JetBrains Mono at 15/13.5/11.5 px,
  animated violet aurora backdrop, boot splash and a connect overlay driven by
  real engine state. Stream presets write real settings bundles.
- **Pacing.** The shell minimizes itself while `streaming` is true, so the
  animated shell stops driving DWM composition behind the fullscreen stream
  window. That removes a second animating surface from the last-percent judder
  budget. The engine skips Direct2D rendering entirely when it is headless and
  not watching.
- **Telemetry.** Status snapshots now carry cadence p50/p99, scanout, present
  misses, shown fps, decode, loss, jitter and SR time; Home shows them live.
- **DLSS 5.** Displayed as unavailable with the reason (3D-guided; needs depth,
  motion vectors and camera matrices from the renderer). The Neural Render
  section is the honest path: monocular depth + optical flow + temporal pass,
  to be wired to TensorRT/NVOFA in the next milestone.
- **Overlay.** Font family moved to Segoe UI Variable Text with slightly larger
  sizes; colours already match the shell.
- **Neural Render is marked Preview.** The depth/optical-flow runtime is not
  wired yet, so the page states that the values are stored for the next
  milestone instead of pretending the toggles do something.

### 2.0.0 notes

- Bridge: `S <json>` status at 4 Hz, commands `connect`, `watch`,
  `stop_watch`, `disconnect`, `apply`, `fullscreen`, `shutdown`.
- `BRP_BRIDGE_LOG=1` enables `%APPDATA%\BetterRemotePlayNext\bridge.log`.
- The shell logs to `%APPDATA%\BetterRemotePlayNext\ui.log`.
- Engine path resolution: `BRP_ENGINE` env override, then
  `brpnext-engine.exe` next to the shell, then the development build tree.

### 2.0.0 package

- Standalone: `dist/BetterRemotePlay-2.0.0-standalone.zip` (921.6 MB)
  SHA256 `BFFB1AADFEAA487A626CEBBAED3D32693F1098B7BFD48385C5B08062E8DED5CD`
- Update **1.2.0 -> 2.0.0**: `dist/BetterRemotePlay-1.2.0-to-2.0.0-update.exe`
  (5.18 MB) SHA256
  `5AC98C864FA1D32A10E90FCB7A2E6C82F6980C2D426A2CACCFB1E839957BA81E`
- `brpnext.exe` (Rust/GPUI shell)
  `0D6834E52DA3DB951719DF7AFB7CEA40629D8319AE82EADFFABFC14E90770FF2`
- `brpnext-engine.exe` (headless C++ engine)
  `79B91897F7BCBAD72A351F840F3AF25D5A10777FDA56ECA297CC0F96E0B07725`
- `brpnext-classic.exe` (1.2 C++ shell)
  `3956F223EC26C83D416ECE09EC0076A0C06CACD01C22AD7B24164E757A6F45DA`
- Patch 1.2.0 -> 2.0.0 verified: `version=2.0.0`, 0 markers,
  `files_differ=0`. Added 2 files (engine, classic), changed 2 (shell, version).
- Packaged smoke test: `dist/BetterRemotePlay/brpnext.exe` starts
  `brpnext-engine.exe --engine --bridge-port`, the bridge connects, the
  engine reaches `connecting` and opens the stream window.
- Tests: `brp_next_tests` ALL PASS (82).

## 2.1.0 - control center in GPUI, live devices, safer settings

- **F10 control center is a GPUI window.** The engine still owns the compact
  stats card and the disconnect banner, but the expanded panel is a second,
  frameless, topmost GPUI window docked to the configured side. Tabs: Info, SR,
  FG, Video, Stream, Audio, Pad. It reads live status at 6-7 Hz and writes the
  same settings keys, then applies them.
- **Foreground handling.** While the panel is open the engine hides the DLSS-G
  worker and stops submitting frames (`overlay_pause_fg_`), because the worker's
  topmost presenter otherwise steals the foreground and closes the panel. The
  engine also grants `AllowSetForegroundWindow(ASFW_ANY)` and the shell
  activates its window with an AttachThreadInput fallback.
- **Key routing.** F10 is routed by the engine's low-level hook whenever the
  engine or the worker has the foreground, so it keeps working when the stream
  window is not focused. While the panel is open the hook routes Esc/F10 to the
  shell and never arms a disconnect. `ShouldCaptureEsc()` returns false while
  the panel is open.
- **Live device lists.** New bridge command `devices` and event
  `E {"kind":"devices",...}` with WASAPI endpoints, monitors and DXGI adapters.
  Audio, Monitor and GPU rows open a picker dialog instead of free text/index.
- **Auto-apply.** A settings edit reaches the engine about 350 ms after the last
  change (tick-based debounce in the poll loop). Apply still forces immediately.
- **Text size.** `ui.scale` now scales the whole type scale (90/100/110/125%)
  and is applied before building a frame.
- **Settings safety.** A file that fails to parse is quarantined as
  `next-settings.json.corrupt-<stamp>` instead of being overwritten by defaults,
  and the shell no longer writes on load.
- **Pacing telemetry.** Status now carries `loop_p99`, `decode_max`, capability
  flags (`cuda`, `tensorrt`, `dlss_sr_ready`, `dlss_fg_ready`) and the Advanced
  page shows a Pacing card with loop p99, cadence, scanout, misses and buffer.
- **Honesty.** Neural Render is read-only with a capability line; DLSS 5 is
  still marked unavailable with the 3D-data reason.
- Bridge protocol additions: `overlay_open`, `overlay_close`, `leave_watch`,
  `disconnect_now`, `devices`; event `overlay-toggle` (bare line, not JSON).

### Settings audit (2.1.0)

Every control the new shell exposes is read by the engine, except the explicitly
read-only preview. Removed because nothing consumed them:
`ai.temporal`, `ai.occlusion`, `ai.adaptive`, `ai.quality`, `overlay.compact`
and the editable `overlay.hotkey` (F10 is fixed in the stream and now shown as a
badge). `general.close_to_tray`, `general.update_channel`,
`general.notifications`, `general.check_updates`, `general.telemetry` and
`ui.scale`-as-layout were dropped or replaced by working controls. The Neural
Render card is read-only with a capability line (TensorRT/CUDA/DLSS presence).
The keys still exist in the JSON for the classic shell; only the new shell
stopped presenting them as if they did something.

### 2.1.0 package

- Standalone: `dist/BetterRemotePlay-2.1.0-standalone.zip` (921.9 MB)
  SHA256 `5E4979A90F54511F2C44CB07BABDCBE6FDF40CE4D4E262FDAF4843DAFDAF5AB1`
- Update **2.0.0 -> 2.1.0**:
  `dist/BetterRemotePlay-2.0.0-to-2.1.0-update.exe` (5.27 MB)
  SHA256 `A3821FE1A8F54911E69803AEB2E9071CA39120B161815A887E78C2FFF7EE63A1`
- `brpnext.exe` (shell)
  `7FA94639B0C36BA2F0AEB6FA199F5E3EB0FCCB420674B33DC8A1A37B73F1EF90`
- `brpnext-engine.exe`
  `E2D887ED9190DFEB4307A30464F8203B4145B594F2C1F02A8C30077B3F21375C`
- Patch verified: `version=2.1.0`, 0 markers, `files_differ=0`, 4 files changed.
- Packaged smoke test: shell starts the engine, bridge connects, devices
  (`audio=5 monitors=3 gpus=4`), stream window opens, shell minimizes.

## 2.2.0 - profiles, keyboard, settings sync

- **Profiles.** Bridge commands `profiles`, `profile_apply <id>`,
  `profile_save <name>`, `profile_delete <id>`; engine event
  `E {"kind":"profiles",...}`. Home shows a Saved Profiles card with Apply and
  Save current; the GPUI control center gained a Profile tab. The bridge command
  limit went from 64 to 160 bytes for names.
- **Keyboard.** Ctrl+1..8 switches the visible tabs (Advanced counts only when
  enabled), Ctrl+Enter connects/watches. F10 from the shell window toggles the
  GPUI panel; Esc closes a picker or the panel before anything else.
- **Settings sync.** Root watches the settings file mtime and reloads on
  external changes, so profile applies and overlay edits show up immediately.
  Writes made by the shell update the stored mtime to avoid a reload loop.
- **Tabs** lost their Unicode icons: Inter has no glyph coverage for them on
  every machine and the fallback rendered tofu. Text-only labels are wider and
  unambiguous; the compact layout still fits at 1040 px.
- Version strings and package metadata moved to 2.2.0.

## 2.3.0 - single instance, hotplug, placement

- **Single instance.** A named mutex (`BetterRemotePlayNextShell`) makes a
  second launch bring the existing shell forward and exit, instead of spawning a
  second engine that fights for the bridge port.
- **Panic log.** A panic hook writes `PANIC: ...` into `ui.log` before the
  process aborts, so a field crash leaves a trace.
- **Hotplug.** While a device picker is open the shell re-requests `devices`
  every ~2 s, so headsets and monitors plugged in after launch appear.
- **Placement.** The control center opens on `display.monitor` (the display
  index used for the stream) instead of always the primary display.
- **Home** shows the GPU adapter as a chip once the engine reports it.
- Version metadata moved to 2.3.0.

### 2.3.0 package

- Standalone: `dist/BetterRemotePlay-2.3.0-standalone.zip` (922.0 MB)
  SHA256 `0CAC77F2570E62D2F449EFFA05D81DB57EEC789268C7852A39F1B4174D28E134`
- Update **2.0.0 -> 2.3.0** (works from 2.1.0 too):
  `dist/BetterRemotePlay-2.0.0-to-2.3.0-update.exe` (5.3 MB)
  SHA256 `7D1173A322ADB5E0EA337CBFDAED712252913179B58DAB54E91D3DC9494C8724`
- `brpnext.exe` (shell)
  `99F7DCEF81A2BD6370B60C0D51ACF9845FB0103FE8575834546D3E7FDD10C5E0`
- `brpnext-engine.exe`
  `4EC104986C3832980D03151935679710548E8755BD596B965445A852D255A171`
- Patch verified: `version=2.3.0`, 0 markers, `files_differ=0`.
- Packaged single-instance check: second launch exits 0, one shell remains.

## 2.4.0 - DLSS 5 Neural Rendering returns

- **Correction to 2.0-2.3.** DLSS 5 NR *does* run on a 2D stream through the
  vendor runtime in video mode, exactly as the legacy `brp.exe` did. The earlier
  "unavailable" card was wrong: NGX feature 18 (`nvngx_dlssnr.dll`, NeuralScreen
  worker) accepts a frame with synthetic depth/motion and history reset each
  frame. It is a real video enhancer, measured at **9.4 ms** for 1080p on this
  RTX 4090, not full 3D-guided NR.
- **Engine integration.** `NextApp` owns `media::NvDlssnr`:
  `EnsureNr()` discovers `nr/native` (packaged), `neuralscreen/native`,
  `third_party/neuralscreen/native` or `BRP_NR_DIR`, starts the worker at the
  frame size (capped 2560x1440) and `EnhanceNeural()` runs before DLSS SR on
  every path: DLSS-G feed, RIFE feed and the parent presenter. HDR (`pq`) frames
  bypass it because the worker is 8-bit. The worker is stopped on leaving Watch,
  on a GPU reset and on a neural settings change.
- **Settings.** `ai.neural_render`, `ai.neural_style` (0 default/1 natural/2
  cinematic), `ai.neural_intensity` (0-200%), `ai.neural_passes` (1-4),
  `ai.neural_scale` (50-100%), `ai.neural_ui`, `ai.neural_mask`, serialized and
  validated like the rest.
- **UI.** The Neural page now has real controls (style, intensity, passes, work
  scale, UI correction, auto mask) and a status line coming from the engine
  (`nr_ready`, `nr_ms`, `nr_runtime`). The control center's Info page shows
  `Neural render on · X ms` live.
- **Cost and guidance.** ~9-10 ms per 1080p frame, spikes to 20-25 ms. At 30 fps
  it fits comfortably; at 60 fps it competes with SR and FG. The toggle is
  opt-in; the page says so. The effect is subtle (mean delta ~2.6/255), which is
  expected without real guides.
- **Honesty.** The card now says "Video mode": the vendor network runs, but
  albedo/normals/camera matrices are unavailable, so it cannot invent scene
  detail like an in-game DLSS 5 title.

### 2.4.0 package

- Standalone: `dist/BetterRemotePlay-2.4.0-standalone.zip` (922.0 MB)
  SHA256 `77ABA5A5401D36309EC6EDFA293C7787A4433F1251E0B188D8193232C026C28A`
- Update **2.0.0 -> 2.4.0** (universal; also fine from 2.1-2.3):
  `dist/BetterRemotePlay-2.0.0-to-2.4.0-update.exe` (5.3 MB)
  SHA256 `C6EA8E7DBD21B00B70C3A2EC3A153B165DE7E1160DF1B72A1D4A0B75A562DD78`
- `brpnext.exe`
  `A6E5D46F5190AF69D9FFEC90E17B4B086741AD210627B1079DF4B31718EA9825`
- `brpnext-engine.exe`
  `D6E99A5B7F95758C99BE4B4CB05622C36C73E40F7469001E28923F873EE51471`
- Patch verified: `version=2.4.0`, 0 markers, `files_differ=0`.
- Packaged smoke test: stream up, control center shows
  `Neural render on · 10.1 ms`, cadence 33.4/49.7 ms.
- Note: the user's settings had `ai.neural_render` enabled during the test and
  were left enabled on purpose; it is the feature they asked for.

## 2.4.1 - frosted glass composition

- **No backdrop blur in GPUI 0.2.** The framework has no filter/backdrop pass; the
  only GPU blur available is the gaussian used by box shadows. The look is
  therefore a composition, not a filter, and the code says so.
- **Frosted content sheet.** The page column now sits on a translucent slab
  (`rgba(8,8,15,0.72)`, 26 px radius, 1 px highlight) with a two-layer wide
  shadow: a neutral 70 px gaussian and a faint violet 90 px halo. The aurora
  stays animated around and softly through it.
- **Card edges.** `widgets::card()` uses a vertical gradient plus the same
  two-layer shadow (40 px dark, 60 px violet) instead of a flat rectangle, so
  cards melt into the sheet rather than cutting hard holes.
- **Aurora softening.** 16 concentric layers per blob (was 8), gentler alpha
  falloff and a sixth 1500 px wash blob; per-blob alpha reduced so the field
  reads as one smooth glow instead of separate circles.
- **Top bar** gets a soft 26 px shadow to separate it from the sheet.
- The stream, overlay panel and engine are untouched.

### 2.4.1 package

- Standalone: `dist/BetterRemotePlay-2.4.1-standalone.zip` (922.0 MB)
  SHA256 `8EDC228BA505561C539CB9CBC1B03A9FB4930530CFE4A315B15AB601592E993C`
- Update **2.4.0 -> 2.4.1**:
  `dist/BetterRemotePlay-2.4.0-to-2.4.1-update.exe` (5.3 MB)
  SHA256 `02A25B9AB754979BDB60385E467FC807D46128AC48F4382C7538479D09DA9B19`
- `brpnext.exe`
  `11287F92B3FE90155D274EBBF6B87877969C4138EB1B90DAA3920EDB75986D47`
- `brpnext-engine.exe`
  `623B7D7222E491D23722575C99DA98DA5273AF219A92289E36268C7A9EBB69C7`
- Patch verified: `version=2.4.1`, 0 markers, `files_differ=0`.
- Packaged visual check: frosted sheet, soft card halos, aurora in the gutters.

## 2.4.2 - Home cleanup

- Removed the redundant "Console / This session" card from Home (address,
  auto-reconnect, fullscreen, overlay hotkey were already in the hero and the
  dedicated pages). The Live telemetry card now spans the full content width.
- No engine or overlay change.

### 2.4.2 package

- Standalone: `dist/BetterRemotePlay-2.4.2-standalone.zip` (922.0 MB)
  SHA256 `A3CA3D76CA3EB97AE139678F8474D1BC4624578BB6806B6BAB226EAD053F7DD8`
- Update **2.4.1 -> 2.4.2**:
  `dist/BetterRemotePlay-2.4.1-to-2.4.2-update.exe` (5.3 MB)
  SHA256 `362CC0C9314D230FD723DF217D78A2AD325EC1F0CE14AF290292EC38B6E6F7B0`
- `brpnext.exe`
  `6CE32A41326D03ABB3F4EC3C96677E7C34A1E4320F0EC972AF59FF24A0D846DC`
- Patch verified: `version=2.4.2`, 0 markers, `files_differ=0`.

## 2.4.3 - no console window

- The Rust shell is now built as a Windows GUI application in release
  (`#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]`).
  Double-clicking `brpnext.exe` no longer opens a terminal window; the engine
  was already `WIN32_EXECUTABLE`.
- Debug builds keep the console for panics and `eprintln!`; the shell still
  logs to `%APPDATA%\BetterRemotePlayNext\ui.log`.
- Verified with the PE optional header: subsystem 2 (GUI), not 3 (console).

### 2.4.3 package

- Standalone: `dist/BetterRemotePlay-2.4.3-standalone.zip` (922.0 MB)
  SHA256 `CE2E513EF2871BEC346D24E689F66FEFA28D17435773CF6E505792CCBA0F28AC`
- Update **2.4.2 -> 2.4.3**:
  `dist/BetterRemotePlay-2.4.2-to-2.4.3-update.exe` (5.3 MB)
  SHA256 `63B1BB84D2E59C7B64BA52FFC79B7B2FA668A0C6836D9559841153B89A346395`
- `brpnext.exe`
  `8D6327AA81A9C4C93C50AC74744BF09FBE275957DBC83DF09638A4769BFC59E2`
- Patch verified: `version=2.4.3`, 0 markers, `files_differ=0`.
- Packaged PE subsystem = 2 (GUI); launch test leaves the window alive and no
  console host is created.

## 2.4.4 - glyph repair

- The window buttons showed `a-e-, a-?T, aoe.` because earlier line-based
  PowerShell surgery read UTF-8 files as ANSI and re-saved them, double-encoding
  every non-ASCII literal. All `ui/src` files are now ASCII-only: the glyphs are
  Rust escapes (`\u{2013}`, `\u{25A2}`, `\u{2715}`, `\u{25B6}`, `\u{00B7}`,
  `\u{2212}`, `\u{2014}`), so no future edit can corrupt them again.
- Removed the dead `Tab::icon` method whose glyphs were part of the corruption.
- Visible result: minimise/maximise/close render as dash, square and x; the hero
  button shows the play mark; hints use `-` instead of a broken middle dot.

### 2.4.4 package

- Standalone: `dist/BetterRemotePlay-2.4.4-standalone.zip` (922.0 MB)
  SHA256 `8ED277D2BEC33723153780328AE31DDFB7A77C731F5F71D93A96D0F005AA0A07`
- Update **2.4.3 -> 2.4.4**:
  `dist/BetterRemotePlay-2.4.3-to-2.4.4-update.exe` (5.3 MB)
  SHA256 `1D7BA33D5ED804F4DB627135E237A2FB32D2BEBF9246647F7C9CE9573E701912`
- `brpnext.exe`
  `FF8F0AE46029D6C00002CAA9D64274296B6656A34C841E85E764318E5286761B`
- Patch verified: `version=2.4.4`, 0 markers, `files_differ=0`.
- Packaged visual check: dash/square/x buttons and the play mark render.

## 2.4.5 - backend-aware controls

- Neural > Frame generation now dims what the selected backend cannot use:
  RIFE model and Blend are washed out (opacity 0.38, read-only value chip, no
  handlers) unless the backend is RIFE; Multiplier is washed out unless the
  backend is DLSS-G.
- The dimmed rows still show the stored value, so nothing looks missing, but a
  click cannot change a setting that would be ignored.

### 2.4.5 package

- Standalone: `dist/BetterRemotePlay-2.4.5-standalone.zip` (922.0 MB)
  SHA256 `397FC38700A78C383C7DD22AB276E62914DC85B9995460173C2DED57B2163F18`
- Update **2.4.4 -> 2.4.5**:
  `dist/BetterRemotePlay-2.4.4-to-2.4.5-update.exe` (5.3 MB)
  SHA256 `476707D7D36196323808F64E08F972425EAFCD1D5216DC2D0811B63ABA4F7175`
- `brpnext.exe`
  `84771956BBB27AA186DF0F395E91616A5746290D1D83686E4DA063E171747A18`
- Patch verified: `version=2.4.5`, 0 markers, `files_differ=0`.

3.x forward handover: docs/33-handover-3x.md (architecture decisions, workstreams, release checklist, open decisions).

## 2.4.6 - latency, pacing and motion-safety honesty

- Pacing: playout phase follows real elapsed time with a queue PI trim, so a
  29.7 fps source no longer drains the buffer; no spin while frames are queued.
- SR and NR run synchronously in the FG path (one frame less latency; frame
  age at presentation 84 ms -> 37 ms at buffer 1, measured).
- `frame_buffer=0` delivers frames in order and trims only past four frames
  (it used to throw the queue away on each tick).
- Probe prints queue, trim, wait/frame max and frame age.
- Motion safety is RIFE-only and shown dimmed on DLSS-G (it had no effect
  there). The fast-motion bypass tried for DLSS-G was not kept: it was not
  measured to help and added frame stalls.
- Standalone: `dist/BetterRemotePlay-2.4.6-standalone.zip` (922.0 MB)
  SHA256 `FB7A2C263728318ABBFA7936276E297BEB4D272EF16AB8C018FCA68DC8A318DE`
- Update **2.4.5 -> 2.4.6**: `dist/BetterRemotePlay-2.4.5-to-2.4.6-update.exe` (5.3 MB)
  SHA256 `E1024D92B1C0E196559E23E225343C50D1DB2EAEDAFDB119E302A806BC72C0D6`
- `brpnext.exe` `F6F53A8B7648206F80ED5A60DD1347CFE8E05D72B265F49688BD07814851D4A9`
- `brpnext-engine.exe` `2134918A56B0EC5BA73EB62DD0C0D03FC291891156F099B0706A3E8C44838C12`
- Patch verified: `version=2.4.6`, 0 markers, `files_differ=0`. Shell PE subsystem 2 (GUI).
- Known open: DLSS-G interpolation artefacts on fast pans (no real motion
  vectors on a video stream), freeze during fast pans not reproduced,
  `frame_buffer=0` cadence weaker than 1, DLSS-G worker phase wobble.

## 2.4.7 - NR inside the DLSS-G worker

- Neural Render runs inside the DLSS-G worker when NR, FG and DLSS-G are on;
  loop p99 16 ms -> 2.3 ms, frames over 10 ms per second 121/151 -> 1/141.
- Head-disappearing artefact on fast pans gone with this setting (user-confirmed).
- Standalone: dist/BetterRemotePlay-2.4.7-standalone.zip (922.0 MB)
  SHA256 718EC19F82419E43F999EB3240C1209E34723D6FEC43FBDAFF1E666636314EA8
- Update 2.4.6 -> 2.4.7: dist/BetterRemotePlay-2.4.6-to-2.4.7-update.exe (5.3 MB)
  SHA256 F708C39C0DF679C96B3E8991DE23288B5888CFF4708C412FC42FF5EEC8B502D8
- brpnext.exe 1C2C38F829C72F446F1F166063BAE3D8604EA85ED2C1BC445F8D6D8CAF9D056A
- brpnext-engine.exe D69F5E2C45158EA8F0AC0A306D4E92FAA1B758A3B17BD9F8AD3F36B51D296E4D
- Patch verified: version=2.4.7, 0 markers, files_differ=0. Shell subsystem 2 (GUI). Smoke: shell alive after 6 s.
## 2.5.0 - shell: system optimization, split AI pages, neural panel, overlay tabs

- System optimizer: first launch reads GPU/VRAM/DLSS runtimes + primary display
  and writes a profile (1080p60, buffer 0, output by panel, DLSS-G 2x + SR, NR
  only where it fits); `system.auto_optimized` keeps it one-time. Advanced >
  System has the on-demand button and the detected hardware. Waits for the
  engine status before deciding (otherwise it saw false capabilities).
- Neural Rendering has its own page; Upscaling and Frame Gen are separate.
  Every NGX feature-18 parameter is exposed as a slider with an editable number
  (click the value, type, Enter). Passes 1..20 normally; a red "USE AT YOUR OWN
  RISK" switch lifts every neural limit to a hard 9999.
- Control center: Neural tab, and the focused tab persists across close/open
  for the current session, reset on disconnect.
- One card look on every page: translucent frost over the aurora, hairline
  border, soft gaussian rim.
- DLSS SR "Max" now initialises (NGX PerfQuality 4 was retired and silently
  disabled SR); the engine's settings writer merges instead of replacing, so
  shell-only keys survive.
- Pipeline audit fixes: the Video > Monitor selection now actually places the
  stream on that display, VSync reaches the presenter, and the dead controls
  (input resolution, HDR transport, colour space, VRR, refresh, bit depth,
  colour format) were removed.
- Standalone (no RIFE): `dist/BetterRemotePlay-2.5.0-standalone-norife.zip`
  (865.5 MB)
  SHA256 `CCFD3214E4237109E5A2CC2AC2D168DF2D1CEB0190413B28B902EA15398232C7`
- Update **2.4.7 -> 2.5.0**: `dist/BetterRemotePlay-2.4.7-to-2.5.0-update.exe` (5.3 MB)
  SHA256 `7E91296F3FEA3911BA6B3505ED97A5AC17E294758A943625EA52B1CBE2718ABF`
- `brpnext.exe` `6695474CA1A1B3556E663166F067F77AE7A89C1646A3B280CCBF66C5CF6F353F`
- `brpnext-engine.exe` `B30E695F84DDB521F5FE1C3924200959423A6855461F1862814194B5DF870E74`
- Patch verified: `version=2.5.0`, 0 markers, RIFE files removed, files identical to dist.
- Details: docs/36-shell-2.5.md and docs/35-latency-audit.md.

- Hotfix for earlier 2.5.0 builds (live neural tuning + worker NR):
  `dist/BetterRemotePlay-2.5.0-hotfix-update.exe` (5.34 MB)
  SHA256 `C199BA3BADFA9D8E73CE88F1AAA8D57DEDB0165A604062079A1BA371088F52AB`
  Built with `tools/make_update.ps1 -BaselineFolder` against a hardlinked
  copy of the current tree with placeholder app files and the old RIFE folder:
  it replaces brpnext.exe, brpnext-engine.exe, brpnext-classic.exe and
  README.txt and removes the four RIFE files. Verified on a simulated earlier
  2.5.0 (version 2.5.0, 0 markers, RIFE removed, files identical to dist).

## 2.5.0 hotfix 2 - disconnect cleanup and update tooling

- Leaving a session no longer leaves windows behind: in engine mode the
  engine's own window stays minimized instead of being restored blank, and the
  GPUI control center closes when the stream ends instead of staying topmost
  over the desktop. Verified with a window-enumeration probe around a real
  disconnect; the DLSS-G presenter and stream window are hidden and the
  foreground returns to the previous app.
- `brp_update.exe` now closes `_delete.txt` before removing it (Windows cannot
  delete an open file) and prunes empty folders after deletions, so a patch
  that removes RIFE no longer leaves `rife\` or the markers behind.
- New artifacts:
  - `dist/BetterRemotePlay-2.5.0-standalone-norife.zip` SHA256
    `EAFFD6A2398C019CA6F65645E0B485656B352355ECA2F3F955F7058FDAC07705`
  - `dist/BetterRemotePlay-2.5.0-hotfix-update.exe` (48.85 MB) SHA256
    `A0D2767B166C1BAD329FCC45E09554F8BD9A8A4256B1CD2B106F4469B90F85DE`
- `brpnext.exe` `9982A30CDAA2BC07814C735B60A2CBF6D6BE09022A26DBD7221462DC0C4D9B10`
- `brpnext-engine.exe` `BD90D1D070743B5D00740CDE5A0098A4A4B0745EF9E20E2569AAFB55753109F2`
- `nvngx_dlss.dll` `858CCFF96360C3098DB9264CD6F13D04A027E09B6CE7AD5BBC297570E824B4F2`
  (310.9.16, DLSS 4.5)
- `app.ico` `0D25A89AB2C9E0EF50F38B642B5EB60BB791FE3654E4024E7D712A80A62224D2`
- Patch verified on a copy of the earlier 2.5.0: version 2.5.0, no markers,
  `rife\` and its logs removed, every file identical to dist except the
  updater itself (endpoint skips `update.exe`; single-file patches embed their
  own copy).
- Research: `docs/37-pxplay-latency.md` (PXPlay / PS Remote Play paths and the
  remaining decode and worker headroom).

## 2.5.0 - GPU decode and presentation

- One D3D11 device (VIDEO_SUPPORT + multithread protection) is now shared by
  the d3d11va decoder, the video processor and the stream presenter. The CPU
  YUV->RGBA swscale path is gone for d3d11va.
- **AI off:** the RGBA result stays a texture and the presenter samples it;
  no transfer, no upload, no readback. Probe: `dec 0.37-0.48 ms`, loop p50
  `0.67 ms`, 240 Hz scanout, zero present misses.
- **AI on:** one staging readback feeds the RGBA consumers. Probe:
  `dec 2.8-3.5 ms` (was 8.2-8.5) with SR + NR + FG active, zero drops.
- PQ/BT.2020 and unsupported drivers fall back to swscale; `BRP_DECODE_VP=0`
  forces the old path.
- Details and measurements: `docs/35-latency-audit.md` (Round 3).

## 2.5.0 - DLSS 4.5 models and forced MFG

- Package ships DLSS Super Resolution 310.9.16 (was 310.7): the second-gen
  transformer models (K/L/M) are available on RTX 40/50.
- `ai.dlss_preset` (Auto/K/L/M/J). Auto maps K to DLAA/Quality/Balanced, M to
  Performance, L to Ultra Performance; the old build sent preset 12 (L) for
  everything.
- Frame-generation multiplier offers 2x-6x on every RTX card. 5x/6x are DLSS
  4.5 MFG: native on RTX 50, forced through the bundled SM86 proxy on older
  cards; no silent 2x clamp. The UI warns that non-50 GPUs may stutter or fail.
- Measured on the 5070 Ti at 1440p + NR: 6x enables and displays 6x, but settles
  near 109 FPS; 4x holds ~120. 4x stays the recommendation at that load.
- UI: Upscaling page and in-game overlay expose the model; the Frame Gen page
  and overlay expose 5x/6x.

## 2.5.0 - NVIDIA Reflex (markers + boost)

- `video.reflex` Off/On/Boost on the Video page and overlay. NVAPI is loaded
  at runtime; markers wrap the parent presenter and Boost sets the low-latency
  clock bit.
- `NvAPI_D3D_Sleep` is not called: the measured driver sleep changed the
  waitable cadence to ~225 Hz with 56+ missed refreshes/s. Markers-only keeps
  240.0 Hz with zero misses.
- Scope: DLSS SR and standalone NR end in the parent presenter and are covered.
  With DLSS-G the worker owns a separate swapchain; Reflex there needs worker
  side integration, which NeuralScreen does not expose today. The UI says so.

## 2.5.0 - application icon

- `assets/app.ico` / `assets/app.png`: rounded dark aurora tile with a light
  dual-sense controller, generated by `tools/make_icon.py` (Pillow).
- Resource id 1 is embedded in `brpnext.exe` (Rust/GPUI via
  `embed-resource`), `brpnext-engine.exe` and `brpnext-classic.exe` (MSVC
  resource script). GPUI's Windows backend loads id 1 for its window class, and
  `Win32Window` sets the same icon on the C++ windows.
- A loose `app.ico` ships in the package root for manual shortcuts; Explorer,
  the taskbar and Task Manager use the embedded icon.
- Sizes in the ICO: 16, 24, 32, 48, 64, 128, 256. The 32 px screenshot from
  the packaged shell was used to check it survives Windows' downscale.

## 2.5.0 - compact shell navigation

- Header is six entries: Home, Stream, Video, Audio, Controller, Overlay.
  Home carries Overview/Advanced; Video carries Output/Upscaling/Frame
  Gen/Neural in a sub-bar under the header.
- Advanced is always on: Basic/Advanced switch and Apply button removed. Every
  widget persists immediately; the engine auto-applies on the next poll.
- The top bar is draggable from full-width 12 px strips in its top/bottom
  padding, the logo, the empty middle and the status pill. Tabs keep their
  own hitboxes.
- Window resize: tab row and window controls are non-shrinking; minimum
  1040x700 enforced. Checked at the minimum size with all three buttons
  visible.

## 2.5.0 - SR/FG timing fixes

- **NR off still built a three-pass cascade** in the DLSS-G worker:
  `NeuralOptionsFrom()` forwarded the saved passes even when NR was disabled.
  The worker took 113-120 ms and dropped inputs; forcing one pass when NR is
  off brings the 60 fps/2x case to 7.2 ms.
- **SR 1440p Quality paid a CPU downscale**: the 1080p frame was resized to
  1706x960 before DLSS. When the requested render size is within 75 % of the
  source, the decoded frame is fed directly; SR-only `sr cpu` went from
  3.61 ms to 0.77 ms.
- SR-only is now an SRV hand-off with no readback. SR->FG still needs one
  1440p RGBA readback (~7 ms) for the worker protocol.
- FG holds the real frame for `multiplier - 1` output intervals: 2x = one,
  4x = three. 60 fps + 2x is the low-latency 120 Hz path; 30 fps + 4x is
  ~17 ms later. The multiplier hint now states this.
- In FG mode the standalone NR probe is zeroed; the live neural time is the
  worker's `fg` field (NR + generation together).
