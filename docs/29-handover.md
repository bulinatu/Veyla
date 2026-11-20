# 29 - Handover (18 Nov 2026)

Windows C++ PS5 Remote Play client on the chiaki-ng session core. DLSS Super
Resolution, DLSS Frame Generation (through the closed NeuralScreen worker),
GPU colour and sharpening, a custom PS5-style UI and native DualSense support.
Build `0.3`. Run `git log -1` for the current commit.

Version 0.5.0 (20 Nov 2026).

- Standalone: `dist\BetterRemotePlay-0.5.0-standalone.zip`, 623.2 MB, SHA256
  `B5740523BC8CE4F3676E627F98732018D66337B87264BC71EB58F67C75417958`.
- Update from 0.4.9: `dist\BetterRemotePlay-0.5.0-update.exe`, **1.29 MB, one
  file**, SHA256
  `3413A1BE77AE9B651562D2963DCEBE42BCD81C3F9A56328E8F4C85DEE927449D`.
- `dist\BetterRemotePlay\brp.exe` SHA256
  `90AF1EBF8106E271A7BE44876FB321032BF9FDBDE74F6BC50F0EE6051FB246BD`.
- `dist\BetterRemotePlay\brpnext.exe` SHA256
  `C3A6B8E92E0D696FB32970AD5EB2B97AFC42386DB810195134F184941480862C`.
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
- DLSS-G 60 -> 120 is a visible but disabled reference option in `brpnext`
  (Stage 6c skipped by decision); use `brp.exe` for 60 -> 120.

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
  RIFE Heavy, TensorRT) with a status dot, AVAILABLE/REFERENCE/MISSING and a
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
