# 14 — Handoff / current state (read this first after a context reset)

Last updated: live streaming + full controller pipeline.

---

## Where the project stands

| Area | State |
|---|---|
| Pairing with the real PS5 | **WORKS** — console paired, keys stored |
| LAN discovery | **WORKS** — finds `192.168.1.21` automatically |
| Live streaming session | **WORKS** — 1080p60 HEVC, ~56–60 fps |
| Connect while asleep | Implemented — wake packet verified; needs one rest-mode test |
| Video decode (live) | **WORKS** — selectable D3D11VA / D3D12VA / Vulkan / DXVA2 / CUDA / software |
| Presentation pipeline | **WORKS** — D3D11 flip model, FSR1/EASU, 0.18 ms at 1440p |
| Fullscreen / HDR10 | **WORKS** — F11; HDR at startup (PQ swapchain) |
| Audio (WASAPI, in-process) | **WORKS** — Opus → FFmpeg → WASAPI, 0 underruns |
| Controller input | **WORKS** — buttons, sticks, triggers, touchpad, calibrated gyro/accel |
| Controller output | **WORKS** — firmware-aware rumble, adaptive triggers, lightbar, player LEDs |
| User interface | **WORKS** — 1440p/4K UI scaling, Tab stream-settings overlay |
| Frame generation | **WORKS** — NVIDIA VFG 2× at 120 fps (synchronous; async blocked) |
| Input rate | 60–1000 Hz, dedicated thread + high-resolution timer |
| Neural upscaling | **REMOVED** - replaced by DLSS SR/DLAA and DLSS NR |
| DLSSNR (same-res neural) | **WORKS via the NGX NR worker at 1080p**, on its own thread, live settings; DLSS SR can run on the NR result (13.2 ms chain); live stream test pending (docs/18, docs/19) |
| Colour pipeline | **WORKS** — chroma bilinear/bicubic/Lanczos + PQ detect/pass-through/tone-map |

---

## Credentials / machine facts

- Console: `PS5-580`, target id `2840DDF38ED9`, LAN `192.168.1.21`
- Session file: `%APPDATA%\BetterRemotePlay\session.json` — contains `rp_key`,
  `rp_regist_key` (16 bytes each, hex), `rp_key_type=2`, `console_pin`.
- PSN account id must be **little-endian** base64 of the 8-byte account id:
  `7022189752615828203` → `6/oN9l/Uc2E=` (big-endian `YXPUX/YN+us=` is rejected
  by the console with HTTP 403 / `0x80108b02`).
- Do **not** need a PIN anymore; the console remembers this client.

---

## Build / run

```powershell
.\scripts\build.ps1              # everything
.\scripts\build.ps1 -Target pair_probe
.\build\Release\brp.exe
```

Targets:
- `brp` — the app
- `brp-bench` — pipeline benchmark
- `pair_probe <pin> [host]` — registration against a console
- `discover_probe` — LAN discovery
- `wake_probe [host]` — sends the rest-mode wake-up packet
- `vfx_probe` — NVIDIA VFG synthetic frame test (writes BMPs)
- `decode_probe <file>` — decode timing
- `d2d_probe` — D2D/D3D11 interop diagnostic

FFmpeg: `third_party/ffmpeg/{include,lib,bin}` (from winget `Gyan.FFmpeg.Shared`,
copied; DLLs are copied next to each exe by a POST_BUILD rule). Not committed.
OpenSSL 4.1 installed at `C:\Program Files\OpenSSL-Win64` but unused (mbedTLS
path is fixed and working).

---

## Source map

```
src/app/app.{h,cpp}            screens (Home/Settings/Stream), dropdown UI, hit regions
src/media/pipeline.*           D3D11 device, flip-model swapchain, stream surface, fullscreen
src/media/decoder.*            libavcodec decoder (sw scale to RGBA)
src/media/nv_vfx.*             NVIDIA Video Effects frame generation (VFG)
src/audio/audio_engine.*       WASAPI shared render engine, 48 kHz float stereo ring
src/session/profile.*          session.json load/save, PXPlay import, AccountIdBase64
src/session/pairing.*          chiaki registration wrapper (thread, callbacks, creds)
src/session/discovery.*        UDP 9302/987 SRCH discovery (PS5/PS4)
src/session/rp_strings.c       chiaki_rp_version_string / reason string
src/session/aia_stub.c         AIA stubs (libcurl not built)
src/session/shim/              curl.h + galois.h shims for MSVC
src/net/reachability.*         TCP 9295 probe (informational)
src/platform/gamepad.*         HID (DualSense/DS4, BT 0x31 + CRC32) + XInput
src/platform/win32_window.*    window, mouse, WM_CHAR, DPI
src/ui/ui_renderer.*           D2D/DWrite wrapper incl. TextEllipsis
vendor/chiaki-ng               forked chiaki, submodule (patched, see below)
third_party/ffmpeg             FFmpeg shared (gitignored)
```

---

## Vendored chiaki patches (inside the submodule, committed there)

1. `lib/src/regist.c`, `lib/src/remote/rudp.c` — C99 VLAs replaced with fixed
   buffers (MSVC has no VLA support).
2. `lib/src/rpcrypt.c` — **the pairing crash fix**: mbedTLS branch declared
   `uint8_t hmac[CHIAKI_RPCRYPT_KEY_SIZE]` (16) but `mbedtls_md_hmac_finish`
   writes SHA-256's 32 bytes → `/GS` stack-cookie failure inside
   `bright_ambassador`. Now `hmac[32]`.
3. `src/session/aia_stub.c` + `src/session/shim/{curl.h,galois.h}` compensate
   for not building libcurl/gf-complete.

`brp_session` currently compiles only the registration subset: common, base64,
log, thread, time, random, sock, http, rpcrypt, ecdh, regist, stoppipe, rudp,
rudpsendbuffer + mbedTLS (FetchContent v2.28.0) + stubs.

---

## How live streaming was built (reference)

Goal: connect to the paired console and feed `media::Decoder` with live packets.

What must be added to `brp_session` (from `vendor/chiaki-ng/lib/src`):
`session.c`, `takion.c`, `ctrl.c`, `streamconnection.c`, `videoreceiver.c`,
`audioreceiver.c`, `feedback.c`, `feedbacksender.c`, `frameprocessor.c`,
`fec.c`, `reorderqueue.c`, `congestioncontrol.c`, `takionsendbuffer.c`,
`packetstats.c`, `audio.c`, `video.c`, `bitstream.c`, `gkcrypt.c`,
`senkusha.c`, `launchspec.c`, `discovery.c`, `discoveryservice.c`.

Dependencies to solve:
- **protobuf/nanopb** — `lib/protobuf` target in chiaki's CMake; needs the
  vendored `third-party/nanopb` submodule plus `protoc`. Easiest path:
  build chiaki's `chiaki-pb` target or vendor the generated `.pb.c` files.
- **Opus** — `audio.c`/`opusdecoder.c` need libopus. Alternative: skip chiaki's
  opus decoder and decode Opus with FFmpeg's native decoder into
  `audio::Engine::Push()`.
- **FFmpeg** — already available.

Recommended approach: add a `StreamSession` wrapper in `src/session/streaming.*`
that mirrors `session.c`'s flow (session init → request → video/audio callbacks)
but decodes with FFmpeg. If chiaki's `session.c` is used directly, its
`ChiakiVideoDecoder` interface can be backed by our `media::Decoder`.

Key chiaki APIs: `chiaki_session_init`, `chiaki_session_set_video_sample_cb`,
`chiaki_session_set_audio_sample_cb`, `chiaki_session_start`,
`chiaki_session_join`, `chiaki_session_set_controller_state`,
`chiaki_session_set_rumble_cb` (check exact names in `include/chiaki/session.h`).

---

## Known gotchas (hard-won)

- Direct2D refuses a target unless the texture has `D3D11_BIND_SHADER_RESOURCE`:
  create the swapchain with `DXGI_USAGE_RENDER_TARGET_OUTPUT | SHADER_INPUT`.
- D2D holds a reference to the backbuffer; call `ui_.ReleaseTarget()` before
  `ResizeBuffers` / fullscreen toggles, then rebind.
- HID handle opened with `FILE_FLAG_OVERLAPPED` needs an `OVERLAPPED` for
  `WriteFile`; `OutputReportByteLength` (547) is the largest report, not the
  78-byte state report actually sent over Bluetooth.
- Vsync does not throttle on virtual/remote displays; the loop paces itself.
- XInput never sees a DualSense; Sony pads need raw HID.
- Account id is little-endian (see above) — this cost a full debugging session.
- **NVIDIA VFG needs the CUDA primary context current for every map/run/export.**
  Without it `NvCVImage_MapResource` returns "invalid resource handle" even
  though `NvCVImage_InitFromD3D11Texture` succeeded.
- **DualSense firmware 0x0630 ignores the old rumble emulation bit.** The report
  now uses `enable3 |= 0x04` for firmware >= 0x0224 (full values), and the old
  `enable1 |= 0x01` + half values below that. Verified with the Rumble test; see
  `docs/10-input.md`.
- **1440p is rejected by the console, not by us.** Asking for 2560x1440 in the
  launch spec kills the stream with `AvCap failed to initialize video:
  [InitResult:-6]`; the advertised profile list is only `1920x1080`. `Start()`
  clamps larger requests. 1440p/4K is client-side upscaling only.

---

## Verification commands

```powershell
.\build\Release\discover_probe.exe                 # should print 192.168.1.21 PS5-580
.\build\Release\pair_probe.exe <pin> 192.168.1.21  # only if re-pairing
.\build\Release\decode_probe.exe third_party\sample_hevc.mp4
```

Pipeline numbers are in `docs/07-bench-log.md`.

---

## Latest increments (after the block above)

- Fullscreen: F11 toggles Presenter::SetFullscreen, D2D target released first.
- media::Decoder now uses **D3D11VA** (hw decode + system transfer): 4.83 ms/frame
  for 1080p HEVC 10-bit (sample: 	hird_party/sample_hevc.mp4, gitignored).
- Upscaler mode 3 = **Lanczos3 + clamped contrast-adaptive sharpening** (RCAS-style).
  Measured at 1440p: crushed blacks 10.1% (mode 2 unsharp) -> 3.7% (mode 3).
  UI dropdown has 4 options; SetUpscaleMode clamps 0..3; keys 1..4.
- FFmpeg DLL copy is a POST_BUILD rule on decode_probe.

---

## AUDIO LIVE

Opus sink -> FFmpeg decoder -> swresample 48k float stereo -> audio::Engine::Push.
Verified: audio header 2ch/48kHz received during a live session; video 1080p60 at ~60 fps.

## Next (in order)
1. User hardware test: rumble + adaptive triggers + touchpad + gyro in-game.
   Watch `BRP_PAD_PROBE=1` for `[pad] rumble`, `[hid] rumble`, trigger effects and
   the Rumble test; firmware 0x0630 now uses the new rumble path.
2. DLSS SR/DLAA visual test at 1440p;
   RGB10A2 path once the decoder keeps 10-bit surfaces (docs/17).
3. DLSSNR: live stream test (NR worker is connected at 1080p, on its own thread,
   with live RNSZ settings and the Boost/stack-pass ladder; see
   `docs/18-neural-nr-status.md`). Check NR on/off and full vs Boost during a
   real stream; the overlay Neural page also needs a live pass.
4. Frame generation: worker-owned D3D11 device + shared textures (the same-device
   worker failed with a CUDA interop device-ordinal crash; see docs/15).
5. DualSense haptic audio (USB only): decode the console haptics Opus stream and
   route it to the controller's 4-channel endpoint.
6. Vblank pacing (`DwmFlush`) and zero-copy decode.
7. Verify VFG + 1440p/2160p target and VFG + HDR combinations.

## Decoder backends (selectable)

`Settings → Display → Video decoder (next session)`:

| Value | Backend | Live test |
|---|---|---|
| Auto | D3D11VA first, then D3D12VA/CUDA/Vulkan/DXVA2 | picks d3d11va here |
| D3D11VA | proven default | 50–56 fps |
| D3D12VA | newer D3D12 video decode | 53–56 fps |
| Vulkan | Vulkan Video decode | 54–56 fps |
| DXVA2 | legacy D3D9 video decode | 54–56 fps |
| CUDA (NVDEC) | NVIDIA NVDEC | 54–56 fps |
| Software | FFmpeg CPU | 50–54 fps |

All six were verified live against the PS5 stream. The decoder always downloads
the decoded surface to CPU RGBA because the presenter is D3D11-based; the
backend choice is therefore about driver path and stability, not a large frame
rate difference yet. The active backend is printed in stderr (`[decode]
backend=...`) and shown in the streaming HUD. An explicit backend that cannot be
created falls back to software with a note; Auto tries the next type.

An actual zero-copy handoff (decoder texture straight into the presenter) is the
next performance step, not a decoder selection issue.



## Controller pipeline (implemented earlier)

- `src/platform/gamepad.*`: added `SetOutput` / `SetConsoleFeedback` /
  `PumpOutput`. The app declares the *desired* output; the pad owns merging,
  priority (local test > console) and report pacing (never above 50 Hz while
  rumbling, 200 ms refresh otherwise).
- Fixed the DualSense output report offsets: intensity at 37, brightness 43,
  player LEDs 44, lightbar 45–47 (previously one byte early).
- `src/session/streaming.*`: persistent `ChiakiControllerState`, touch-id
  tracking, gyro/accel scaled to rad/s + g, Madgwick orientation quaternion,
  state-change-only forwarding. Console events (`RUMBLE`, `TRIGGER_EFFECTS`,
  `LED_COLOR`, `PLAYER_INDEX`, `*_INTENSITY`) are captured as `PadConsoleFeedback`.
- Settings now persist: lightbar, manual trigger effect/strength, player LEDs,
  mic LED, console rumble/trigger toggles, motion toggle, report rate (up to
  1000 Hz).
- Measured pacing: 936–1001 loop iterations/s at a 1000 Hz target on this
  machine (Windows 10+ high-resolution waitable timer; `sleep_for` would have
  capped at ~64 Hz because of the 15.6 ms timer tick).


---

## Colour + HDR

- swscale now receives explicit BT.709/BT.2020 + limited-range info (fixes washed-out image).
- Chroma upsampling is selectable per session (`Settings -> Display -> Chroma
  upsampling`): bilinear (fast), **bicubic (default)** or Lanczos+accurate.
  Measured at 1080p NV12 -> RGBA on this machine:
  bilinear 2.6 ms, **bicubic 2.1 ms**, lanczos+accurate 5.6 ms,
  bicubic + `SWS_FULL_CHR_H_INT` **10.0 ms**. `FULL_CHR_H_INT` was removed: it
  ran synchronously on chiaki's video receive thread and pushed high-motion
  frames past the 16.7 ms budget, which drained the socket and caused dropped
  frames when moving the camera. Plain bicubic is both faster and sharper than
  bilinear; the difference to full-chroma is subtle on video.
- Source transfer is inspected (`AVFrame.color_trc == SMPTE2084`). A PQ/BT.2020
  stream is passed through untouched on an HDR10 swapchain and tone-mapped
  (PQ decode -> 100 nits SDR) on an SDR swapchain. SDR into an HDR container still
  goes through the sRGB->PQ encode.
- HDR10 path implemented: swapchain R10G10B10A2 + PQ color space, shader PQ encode, UI colours PQ-encoded via theme::SetHdr.
- HDR is applied at STARTUP (profile.hdr); live swapchain recreation fails with E_ACCESSDENIED on the same HWND, so the dropdown writes the setting and says restart.
- Verified SDR streaming still works after the changes.


## Upscaling

Mode 4 is FSR1 EASU (12-tap, ported from AMD's MIT reference). Measured 0.18 ms p50 at 1440p.
Modes: 0 bilinear, 1 Catmull-Rom, 2 Catmull+unsharp, 3 Lanczos+RCAS, 4 FSR1 EASU.

### Explicit 1080p / 1440p / 2160p render target

`Settings -> Upscaling -> Upscale target` picks the resolution the video chain
renders at, independently of the window/display:

- **Window / display** — direct upscale into the swapchain (previous behaviour).
- **1080p / 1440p / 2160p** — the upscaler writes an offscreen target of that
  size, then a Catmull-Rom resolve fits it to the window/display with black bars
  instead of stretching. The pass is skipped when the target equals the output
  (e.g. 1440p target on a 1440p fullscreen monitor), so there is no extra cost.
- F12 captures the render target, not the window: 2160p target gives a clean
  3840x2160 BMP even from a 1600x900 window.

Measured with EASU on this machine (GTX-class, 1080p source): windowed
1600x900 with a 2160p target, `gpu p50 0.50 ms / p95 0.17 ms`, 59.9 fps; in
1440p fullscreen with the same 2160p target, `p95 0.19 ms`. Both passes are
negligible next to decode.

The 1440p/2160p options are **client-side supersampling/upscaling of the 1080p
stream**. They cannot add detail the console did not send; they trade a little
GPU time for a resolution-matched present (and clean high-res captures).

### Neural upscaling (RTX VSR) - removed

RTX Video Super Resolution was removed from the client. The neural paths are now DLSS Super Resolution/DLAA (Upscaling), DLSS Neural Rendering (Neural) and DLSS Frame Generation (Performance). See `docs/17-neural-rendering.md`.

### Frame pacing / smoothness

The decoder + YUV->RGBA conversion run on chiaki's video receive thread, so any
per-frame cost above the frame budget is visible as motion drops. Two changes:

- conversion cost cut from 10.0 ms to 2.1 ms (above);
- `Streaming::TakeFrame` now keeps an **adaptive jitter buffer**: the target
  starts at one frame, stays low while the producer is stable, rises by one (up
  to the "Frame buffer" maximum) when the queue runs dry, and falls back after
  five stable seconds. It also skips at most one stale frame per tick when the
  queue is long, so latency is bounded without a multi-frame jump.

`BRP_DECODE_PROBE=1` logs per second:
`fps ... | kbps lost N | decode avg/max ms | queue N buf N drops N`.
`lost > 0` means network/FEC, a rising `buf` means jitter, high `decode` means
CPU. Verified live after the fix: decode 4.5 ms avg / 5.4 ms max at 60 fps,
`lost 0`, `queue 1`.

Frame generation now pulls a source frame only when its generated queue is low.
`vfg_pace_probe` (synthetic 60 Hz source, 120 Hz presentation) reports
**59.9 generated/s and 119.9 presented/s**; the earlier cadence generated ~31/s.

Frame generation: see `docs/15-frame-generation.md` (NVIDIA VFG 2×).

