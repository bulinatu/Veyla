# BetterRemotePlay

A better PlayStation 5 → PC Remote Play client.

**Status:** Phase 1 — the app runs. Home / Settings / Stream screens are live, the post-process pipeline is measured on real hardware, the chiaki-ng core is vendored. No console attached yet.

**What exists right now**

- **`brp.exe` — the app.** PS5-inspired rail, settings, and a stream screen running the real post-process pipeline with a live latency HUD. Keyboard-driven, no browser in the video path. It also imports console identity + PSN account from a local PXPlay install (metadata only — PXPlay encrypts its registration keys; see [docs/09-session.md](docs/09-session.md)). See [docs/08-app.md](docs/08-app.md).
- **`brp-bench`** — headless-ish benchmark of the same media path, with per-frame GPU timestamps. 0.19 ms p50 at 1080p→4K on the target machine. See [docs/07-bench-log.md](docs/07-bench-log.md).
- **`d2d_probe`** — isolates Direct2D/D3D11 interop failures from app code (it earned its keep: see the gotchas in 08).
- **`tools/imgstat.py`** — detail/clipping metrics for captured frames.
- **`vendor/chiaki-ng`** — forked protocol core, pinned as a submodule (42 C sources ready to become `brp-session`).
- Docs: protocol limits, feasibility matrix, architecture, roadmap, ADRs, bench log, app notes.

**Not yet:** decode, RTP wiring, PSN OAuth, neural stages, HDR output.

```powershell
.\scripts\build.ps1
.\build\Release\brp.exe
```

| Screen | Keys |
|---|---|
| Home | `Left`/`Right` select · `Enter` stream · `S` settings · `Esc` quit |
| Settings | `Up`/`Down` · `Left`/`Right` change · `S`/`Esc` back |
| Stream | `Esc` back · `H` HUD · `1`/`2`/`3` upscaler · `F12` screenshot |

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

## Legal note

Reverse-engineered Remote Play clients live in a gray zone. Sony's PSN terms prohibit reverse engineering, and the protocol is proprietary. Projects like chiaki/chiaki-ng have existed for years without action, but that is not a guarantee. This project ships no Sony code, no keys, and no piracy tooling. Use it on hardware and games you own. AGPL-3.0 if we fork chiaki-ng — see [docs/04-architecture.md](docs/04-architecture.md).
