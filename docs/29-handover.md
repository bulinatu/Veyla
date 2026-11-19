# 29 - Handover (18 Nov 2026)

Windows C++ PS5 Remote Play client on the chiaki-ng session core. DLSS Super
Resolution, DLSS Frame Generation (through the closed NeuralScreen worker),
GPU colour and sharpening, a custom PS5-style UI and native DualSense support.
Build `0.3`. Run `git log -1` for the current commit.

Version 0.4.7 (19 Nov 2026).

- Standalone: `dist\BetterRemotePlay-0.4.7-standalone.zip`, 602.2 MB, SHA256
  `47D35EE7D6980108A794813D5A97F37AC022E4E8D7A294C55081DB8755653212`.
- Update from 0.4.5 or 0.4.6: `dist\BetterRemotePlay-0.4.7-update.exe`,
  **0.71 MB, one file**, SHA256
  `0F550E89EF53FFFC281595ED540D8852DD5C5F954D9D160286CBD7425CA42792`.
- `dist\BetterRemotePlay\brp.exe` matches `build\Release\brp.exe`
  (SHA256 `49F04BBD88EE129D084CD3A36AA3EB26CC79133535E78E59225EBC477E3CFD36`).
- The package includes `rife\rife425_1080.engine` (12.5 MB, built for the
  reference RTX 4090), the RIFE model, `cudart64_13.dll` and `update.exe`.
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
