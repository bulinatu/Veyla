# 29 - Handover (18 Nov 2026)

Windows C++ PS5/PS4 Remote Play client built on the chiaki-ng session core, with
DLSS Super Resolution, DLSS Frame Generation, GPU colour/sharpening, a PS5-like
custom UI and native DualSense support. Roughly 15.9k lines under `src/`.
Current build: `0.3`, standalone ZIP at `dist\BetterRemotePlay-0.3-standalone.zip`.

## What works today

| Area | State |
| --- | --- |
| Video | 720p/1080p negotiated, HEVC, D3D11VA/D3D12VA/Vulkan/DXVA2/NVDEC/software, chroma bilinear/bicubic/lanczos. The 30/60 fps setting paces the client output; the console always encodes at its own rate (60). 1080p60 measured with 0 lost / 0 drops on LAN. |
| Audio | Opus 48 kHz stereo -> WASAPI, device picker, 10-40 ms buffer, test tone, underrun counter. |
| Presenter | Flip-discard swapchain, waitable vblank clock, 4-refresh cadence at 60 fps, F11 fullscreen, F12 screenshot, device-loss recovery. |
| Upscalers | Bilinear, Catmull-Rom, Lanczos+RCAS, FSR1 EASU, DLSS SR (33/50/58/66/77 %) and DLAA with K/L/M presets. |
| Frame generation | DLSS FG 2x through the NeuralScreen worker at 120 fps on 240 Hz, fed by DLSS SR at render resolution. Measured 120.0 fps displayed. |
| Colour | Exposure, contrast, highlights, shadows, saturation, vibrance, hue, temperature, tint, gamma, sharpening; 4 presets + reset; applied before the FG worker. |
| Controller | Native DualSense HID over USB and Bluetooth: buttons, sticks, touchpad, motion, lightbar, player LEDs, mic LED, rumble, adaptive triggers, console feedback relay. Steam Input/XInput fallback. |
| Session | PSN sign-in, pairing, LAN discovery, wake from rest, probe, explicit disconnect (RST) that frees the console session. A remote failure returns Home with the reason; there is no automatic re-dial. |
| UI | Home, Settings (10 tabs), in-stream sheet (Tab), HUD toggles, double-Esc confirmation, boot-style connecting animation. |
| Diagnostics | `%APPDATA%\BetterRemotePlay\health.log` every 30 s; `nr-worker.log`; probes `BRP_FRAME_PROBE`, `BRP_DECODE_PROBE`, `BRP_PAD_PROBE`. |

## Known problems / limits

1. **FG 120 on 240 Hz is not vblank-phase locked.** Some frames hold 1 or 3
   refreshes instead of 2, which is felt as a slight speed wobble while panning.
   The worker presents on its own clock and is closed, so the app cannot align
   it. Possible workaround: enable G-SYNC. Changing the monitor to 120 Hz was
   rejected by the user and is not offered.
2. **Windowed presentation judders at 60 fps (fixed: streams start fullscreen).**
   Windowed, 62-84 % of source frames held 4 refreshes; fullscreen, 95-100 %.
   The cause is DWM composition, not decode, SR or buffering. A stream now
   enters borderless fullscreen on start; F11 returns to the window. Measured
   with SR on: 4v 100 %, late 0, interval p99 4.4 ms.
   Rejected along the way: MMCSS "Games" on the main thread (median late 16 vs 9,
   reverted); jitter buffer 2 (no gain over 1); fps 30 in the profile, which
   halves the source and was reset to 60.
3. **FG without SR is not offered.** A worker restarted at 1080p without SR came
   up in overlay mode and never presented. Safe fallbacks are SR only or raw.
4. **Presets apply at the next session only (intended).** While streaming, a
   preset saves the selection and the note says it applies on the next start.
   Building the chain mid-session once broke the D3D12VA decoder ("Failed to map
   D3D12 Buffer resource"). Note: the preset code previously applied the chain
   even while streaming, contrary to its comment; fixed, build-verified only.
5. **HDR through FG is SDR.** The worker only accepts RGBA8 (format 28), so the
   graded readback tone-maps. Parent window can still be HDR; the worker cannot.
6. **DLSS NR** works inside the worker but costs ~7-18 fps at 2x; **FG 3x-6x**
   land between refreshes (~140-165 fps measured), so 2x is the only smooth claim.
7. **Bluetooth triggers** depend on the console sending non-zero effect packets;
   the 0x31 report CRC was fixed and rumble verified. Report is zero-padded to
   Windows' 547-byte output length; the 78-byte report is correct.
8. **Network**: PC side tuned and measured (docs/26); router and PS5 items are
   still a fill-in checklist. Bitrate 100 Mbps tested clean; 50/75 not swept.
9. **No self-update.** Distribution is the ZIP; updating is manual.
10. **Structure debt**: `app.cpp` is ~5.7k lines, the Settings page and the
    in-stream sheet duplicate several panels, and the legacy NVIDIA VFG and
    Reflex paths are kept on purpose (features, not dead code).

## Where things are

| File | Purpose |
| --- | --- |
| `src/app/app.cpp` / `.h` | Screens, settings, frame loop, DLSS/NR/FG chain, presets, health log, recovery |
| `src/media/pipeline.cpp` | Swapchain, pacing, upscale + sharpen + colour, graded readback |
| `src/media/shaders.h` | HLSL: upscalers, CAS, colour grade, HDR encode |
| `src/media/nv_dlss_sr.cpp` | DLSS SR/DLAA on D3D12, two output slots, async submit |
| `src/media/nv_dlssnr.cpp` | Closed NeuralScreen worker: SHM/RNSZ/WNDO protocol, present mode |
| `src/session/streaming.cpp` | chiaki session, decode thread, jitter buffer, feedback, disconnect |
| `src/platform/gamepad.cpp` | DualSense HID input/output, USB/BT reports, CRC |
| `src/session/profile.cpp` | JSON profile, defaults, clamps |
| `tools/package_release.ps1` | Standalone ZIP |
| `tools/measure_frame.ps1`, `measure_presents.ps1` | Screenshot/presentation measurements |

## Run and diagnose

```
build\Release\brp.exe                     # needs the DLLs next to it
%APPDATA%\BetterRemotePlay\session.json   # profile (back it up)
%APPDATA%\BetterRemotePlay\health.log     # 30 s telemetry
build\Release\nr-worker.log               # FG worker truth ([fg] displayed)
```

Probes: `set BRP_FRAME_PROBE=1`, `BRP_DECODE_PROBE=1`, `BRP_PAD_PROBE=1`, then
run from a console so stderr is visible.

Rebuild: `cmake --build build-perf --config Release --target brp --parallel 8`
then copy `build-perf\Release\brp.exe` to `build\Release`.

## Next, in priority order

1. Evaluate G-SYNC for FG 2x on 240 Hz (no display mode change). Worker-side pacing stays out of scope.
2. Finish router + PS5 settings and the bitrate/reconnect matrix (docs/26).
3. Decide whether the legacy VFG and Reflex paths stay (maintenance vs features).
4. Split `app.cpp` and share the Settings/overlay panels.
5. Self-update (private repo + per-user key, or GitHub device flow).
