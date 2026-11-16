# 14 — Handoff / current state (read this first after a context reset)

Last updated: pairing + decode milestone.

---

## Where the project stands

| Area | State |
|---|---|
| Pairing with the real PS5 | **WORKS** — console paired, keys stored |
| LAN discovery | **WORKS** — finds `192.168.1.21` automatically |
| Video decode (file/stream) | **WORKS** — libavcodec, HEVC 10-bit, 5.8 ms/frame software 1080p |
| Presentation pipeline | **WORKS** — D3D11 flip model, Catmull-Rom upscale, 0.04–0.19 ms at 4K |
| Fullscreen | **WORKS** — F11, exact output resolution |
| Audio engine (WASAPI) | **WORKS** — test tone, device selection, 0 underruns |
| DualSense | HID input + lightbar/triggers/rumble/LEDs over BT |
| Live streaming session | **NOT STARTED** — the next big block |
| HDR / FP16 color | Not started |
| FSR1/NIS | Not started |
| Frame gen / neural | Not started (user deferred neural) |

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

## Next block: live streaming session (the big one)

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
1. YUV->RGB in GPU (FP16) + BT.709/BT.2020 + PQ for #3/#11; swapchain R10G10B10A2 + HDR metadata.
2. FSR1 (EASU+RCAS) or NIS as upscale modes 4/5.
3. Frame generation (RIFE/TensorRT) opt-in, latency shown.
4. DualSense haptic audio (USB audio-haptic path).


---

## Colour + HDR

- swscale now receives explicit BT.709/BT.2020 + limited-range info (fixes washed-out image).
- HDR10 path implemented: swapchain R10G10B10A2 + PQ color space, shader PQ encode, UI colours PQ-encoded via theme::SetHdr.
- HDR is applied at STARTUP (profile.hdr); live swapchain recreation fails with E_ACCESSDENIED on the same HWND, so the dropdown writes the setting and says restart.
- Verified SDR streaming still works after the changes.


## Upscaling

Mode 4 is FSR1 EASU (12-tap, ported from AMD's MIT reference). Measured 0.18 ms p50 at 1440p.
Modes: 0 bilinear, 1 Catmull-Rom, 2 Catmull+unsharp, 3 Lanczos+RCAS, 4 FSR1 EASU.
Frame generation: no in-app model yet; recommended path is NVIDIA Smooth Motion (driver, applies to our DX11 swapchain).

