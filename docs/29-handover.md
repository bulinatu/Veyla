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
