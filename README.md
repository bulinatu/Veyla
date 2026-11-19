# BetterRemotePlay

> **BetterRemotePlay Next (rebuild in progress).** The working product is still
> `brp.exe` 0.4.9. The new application is `brpnext.exe`: Stage 3 shell with the
> new design system, typed settings, profiles, diagnostics, AI capability
> checks and real system facts. Streaming and AI execution are Stages 4-7 and
> are not claimed by the UI. Foundation tests: `brp_next_tests` (79 assertions).
> Start with `docs/ARCHITECTURE.md`, `docs/IMPLEMENTATION_ROADMAP.md` and
> `docs/FEATURE_MATRIX.md`.

A better PlayStation 5 → PC Remote Play client.

**Status:** **Live** — pairs with a real PS5, streams 1080p60 video (HEVC/D3D11VA) and Opus audio through the post-process pipeline.

**What exists right now**

- **`brp.exe` — the app.** Home with a game rail and a console card, five settings categories with dropdowns, and a stream screen running the real post-process pipeline. Mouse-first, no browser in the video path. See [docs/08-app.md](docs/08-app.md).
- **Connection.** `Connect` runs a real TCP probe of the console control port (9295) and reports reachable/unreachable; pairing is the next step. See [docs/13-connection.md](docs/13-connection.md).
- **DualSense support.** Raw HID (no DS4Windows) + XInput fallback. Parse: buttons, sticks, triggers, touch points, gyro/accel, battery. Output: lightbar RGB, 8 adaptive-trigger effects, rumble, player LEDs, mic LED. Bluetooth output uses the `0x31` report with CRC32. Input source is selectable: native HID, or XInput (which is how Steam Input's Xbox emulation is consumed).
- **Audio.** WASAPI shared-mode engine inside the app: 48 kHz float stereo in, endpoint out. No virtual cable, no driver, no external DLL. The DualSense 3.5 mm jack is a normal USB audio endpoint — select it and audio goes to the controller. See [docs/11-audio.md](docs/11-audio.md).
- **Session import.** Console identity + PSN account from a local PXPlay install (metadata only — PXPlay encrypts its keys; see [docs/09-session.md](docs/09-session.md)).
- **`brp-bench`** — benchmark of the media path. 0.19 ms p50 at 1080p→4K on the target machine. See [docs/07-bench-log.md](docs/07-bench-log.md).
- **`d2d_probe`**, **`tools/imgstat.py`** — diagnostics that earned their keep.
- **`vendor/chiaki-ng`** — forked protocol core, pinned as a submodule (42 C sources ready to become `brp-session`).

**Not yet:** frame generation model, neural video stages, DualSense haptic audio, PSN OAuth.

```powershell
.\scripts\build.ps1
.\build\Release\brp.exe
```

| Screen | Keys |
|---|---|
| Home | `Left`/`Right` select · `Enter` stream · `S` settings · `Esc` quit |
| Settings | `Tab` / `Left` / `Right` switch category · `Esc` close dropdown / back |
| Stream | `Esc` back · `H` HUD · `1`/`2`/`3` upscaler · `F12` screenshot (at the upscale target) |
| Mouse | Click categories, dropdowns, items and buttons; wheel moves the rail |

---

## The one-paragraph version

Sony's official app and every open-source client are limited by the same thing: **the PS5's own video encoder**. It will only ever send you 720p or 1080p, 30 or 60 fps, in H.264 or H.265, at a bitrate it decides. Nothing we write on the PC changes that. What we *can* change is everything after the bytes arrive: correct color, high bitrate, GPU upscaling, frame interpolation, HDR passthrough, low-latency presentation, and a launcher that feels like a PS5.

That split is the whole design of this project:

```
  PS5 (fixed)                          PC (ours, wide open)
┌──────────────────┐   LAN / WAN   ┌──────────────────────────────────────────┐
│ capture 1080p60  │ ────────────► │ decode → color → upscale → interp → HDR  │
│ H.264 / H.265    │   15–100 Mbps │ D3D11/12 · Vulkan · CUDA · DXVA          │
│ 8/10-bit, SDR/HDR│               │ + PS5-style shell, PSN pairing, tuning   │
└──────────────────┘               └──────────────────────────────────────────┘
```

## What this is

- A Windows-first Remote Play client for PS5.
- Bitrate-first quality: default 1080p60 H.265 at high bitrate, not Sony's "good enough" preset.
- A real color pipeline: full-range output, correct BT.709/BT.2020 matrices, no washed-out image.
- GPU post-processing: spatial upscaling to 1440p/4K, optional frame interpolation, HDR10 output.
- A launcher shell that looks and sounds like a PS5.
- PSN login for pairing, so setup isn't a scavenger hunt for a base64 Account ID.

## What this is *not*

- Not 4K from the console. Remote Play re-encodes at 1080p max. 1440p/4K here means **display-side upscaling**.
- Not 120/240 fps from the console. It's 60 in, interpolated up. That is a real tradeoff, documented in [docs/03-feasibility.md](docs/03-feasibility.md).
- Not AV1 on the wire. The PS5 encoder doesn't do it. AV1 only makes sense if we re-stream or record.
- Not DLSS. DLSS needs engine motion vectors and depth buffers. A video stream has neither. The video-domain equivalents are RTX Video Super Resolution, FSR/NIS, and driver-level frame generation.
- Not affiliated with Sony. No Sony SDK, no leaked keys, clean-room protocol work from the open-source community.

## Build

```powershell
.\scripts\build.ps1          # configure + build (Release, VS 2022 generator)
.\scripts\bench.ps1          # resolution x upscaler matrix -> build\bench.csv
python tools\imgstat.py build\shots\m0.bmp
```

Keys in `brp-bench`: `1` bilinear, `2` Catmull-Rom, `3` Catmull-Rom + sharpen, `ESC` quit.
Useful flags: `--width/--height` output size, `--src-width/--src-height` source size, `--frames N` run-then-exit, `--limit FPS`, `--shot path.bmp`.

## Docs

| Doc | Contents |
|---|---|
| [docs/01-landscape.md](docs/01-landscape.md) | Survey of open-source (and closed) remote play clients: licenses, architecture, what to borrow |
| [docs/02-protocol.md](docs/02-protocol.md) | What the PS5 Remote Play protocol actually does: pairing, negotiation, codecs, limits, latency |
| [docs/03-feasibility.md](docs/03-feasibility.md) | Your feature list, one line at a time: possible / possible-with-caveats / not-as-described + workaround |
| [docs/04-architecture.md](docs/04-architecture.md) | Proposed stack, module map, post-process graph, decoder matrix, latency budget |
| [docs/05-roadmap.md](docs/05-roadmap.md) | Phased plan with acceptance criteria and measurable targets |
| [docs/06-decisions.md](docs/06-decisions.md) | ADR log: fork, neural stages, network defaults, UI, measurement policy |
| [docs/07-bench-log.md](docs/07-bench-log.md) | Measured results on the target machine, with interpretation |
| [docs/08-app.md](docs/08-app.md) | The app: screens, keys, code map, design rules, interop gotchas |
| [docs/09-session.md](docs/09-session.md) | Session data: what PXPlay exposes, what is encrypted, our session file format |
| [docs/10-input.md](docs/10-input.md) | Mouse, keyboard, controller navigation, DualSense input/output matrix |
| [docs/11-audio.md](docs/11-audio.md) | Audio: WASAPI in-process, DualSense jack over USB, latency, A/V clock plan |
| [docs/12-steam-input.md](docs/12-steam-input.md) | Steam Input: Xbox emulation vs Steamworks API, trade-offs, setup |
| [docs/13-connection.md](docs/13-connection.md) | What Connect does today: reachability probe, states, what pairing adds |
| [docs/15-frame-generation.md](docs/15-frame-generation.md) | NVIDIA VFG frame generation: why not DLSS-G, runtime, measurements, limits |
| [docs/17-neural-rendering.md](docs/17-neural-rendering.md) | DLSSNR (neural rendering): what the DLL is, why a naive NGX host fails, integration plan |

## Legal note

Reverse-engineered Remote Play clients live in a gray zone. Sony's PSN terms prohibit reverse engineering, and the protocol is proprietary. Projects like chiaki/chiaki-ng have existed for years without action, but that is not a guarantee. This project ships no Sony code, no keys, and no piracy tooling. Use it on hardware and games you own. AGPL-3.0 if we fork chiaki-ng — see [docs/04-architecture.md](docs/04-architecture.md).
