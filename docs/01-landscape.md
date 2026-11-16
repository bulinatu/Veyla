# 01 — Landscape: who has already solved what

The goal of this doc is not to admire other projects, it's to decide what we **fork**, what we **reimplement**, and what we **copy as an idea** (not as code).

---

## Open source PlayStation Remote Play

### Chiaki (original)
- Repo: `thestr4ng3r/chiaki`
- License: **GPL-3.0**
- Language: C (protocol in `lib/`, later extracted as `setsu`), Qt GUI, SDL for input/render.
- Scope: PS4 + PS5 Remote Play, all major desktop platforms, Android, Switch.
- Status: **maintenance mode**. Still the reference implementation for the wire protocol.

**Why it matters to us:** it is the clean-room proof that the protocol is implementable without Sony cooperation. Its `setsu` library is the canonical protocol implementation: discovery, PIN registration, session request, RTP video/audio, input reports.

**What we take:** protocol knowledge. Potentially the library itself (GPL is compatible with our plan to be AGPL).

**What we don't take:** the renderer. Its OpenGL path is old and is the main source of the "washed out / green ghosting" complaints.

---

### chiaki-ng
- Repo: `chiaki-ng/chiaki-ng`
- License: **AGPL-3.0**
- Language: C, CMake, Qt + SDL, FFmpeg 9.x, Vulkan/OpenGL/Metal renderers.
- Status: **active**, the de-facto community client (v1.10.1 as of Nov 2026). Steam Deck is its main platform, Windows is supported.
- Feature set that matters to us:
  - PS5 **HDR** via the `H265 HDR (PS5 only)` codec option (10-bit HEVC).
  - Configurable **bitrate** (community runs 30,000–50,000 kbps; defaults lower).
  - Multiple **renderers** (Vulkan is the modern one; Metal on macOS brings HDR back there).
  - **FFmpeg hwaccel** decoders: `d3d11va`, `dxva2`, `cuda`, `vaapi`, `vulkan`, `videotoolbox`.
  - Controller support incl. DualSense features, touchpad, gyro.
  - LAN + internet (PSN-mediated) play.

**Why it matters to us:** this is 80% of our engine, already working. Forking it means Phase 1 is "build and measure", not "write a protocol stack from scratch".

**What we take:** the whole core, initially. Then we replace the renderer/presenter path with ours.

**What we must accept:** AGPL-3.0. If we distribute binaries, we publish source. For a project like this that's a feature, not a cost.

---

### PSPlay / MirrorPlay
- Closed source. Android/iOS/Windows (MirrorPlay is the rebrand).
- Notable because it already does much of your list: **PSN login**, high bitrate, custom resolution/fps requests, HDR, low-latency tuning, third-party controller support.
- No code to read — but it is **proof of feasibility** for PSN OAuth pairing and high-bitrate sessions. Treat it as a functional spec, not a source.

---

### Greenlight
- Repo: `unknownskl/greenlight`
- License: **MIT**
- Scope: **Xbox** cloud + remote play, not PlayStation. Electron/TypeScript.
- Why it matters: best-in-class *client UX* for a console-streaming app on desktop (library, recently played, session handling, overlay). The protocol is different, the product design is directly stealable (with attribution).

---

### Steam Link / `jmelovich/Steam-Link-PS5`
- Open source, GPL-3.0, but it's the **opposite direction** (PS5 receives a PC stream, needs a jailbroken console).
- Why it matters: it proves a 1080p60 hardware-decode + tiny frame buffer pipeline on a constrained device, and its pairing UX (LAN discovery → PIN → remembered key) is a good model. Also the only public project confidently documenting Steam's own high-bitrate codec work.

---

## Open source PC/console streaming (adjacent, very instructive)

### Moonlight
- Repo: `moonlight-stream/moonlight-qt` — **GPL-3.0**.
- The gold standard for low-latency desktop streaming: 4K120, HDR, AV1/HEVC/H.264, frame pacing, VRR, per-frame latency stats, decoder selection UI.
- **What we take:** the *latency architecture*. Flip-model presentation, decoder queue depth of 1, audio as the clock, frame-pacing heuristics, and the stats overlay ("decode 3.2 ms, network 4.1 ms, render 1.8 ms"). This is exactly the discipline that makes "minimal delay" real.

### Sunshine
- Repo: `LizardByte/Sunshine` — **GPL-3.0**.
- Host side. Relevant for understanding encoder-side tradeoffs: CBR vs VBR, low-latency presets, intra-refresh, slice count. Our host is a PS5 we don't control, but the vocabulary matters when we tune the client's requested bitrate.

### PyroWave (Themaister)
- Intra-only wavelet codec for ~200–500 Mbps wired streaming, MIT. Not usable with a PS5, but it's the interesting end-state of "bitrate is cheaper than compute".

---

## Closed, but relevant

- **Official PS Remote Play (Windows/macOS/iOS/Android):** the baseline we're beating. Quality presets are conservative, bitrate is low, and color handling is the usual complaint.
- **PS Portal bitrate mods:** community work shows the *console* will accept higher targets than the official clients request, and that a Portal can be pushed toward ~97 Mbps target / ~100 Mbps peaks depending on scene. So the ceiling is higher than Sony's UI suggests — but it is still a ceiling, and still 1080p60.

---

## Decision

| Option | Effort | Risk | Verdict |
|---|---|---|---|
| Fork **chiaki-ng** core, replace presenter + UI | Low | AGPL obligations; fork drift | **Recommended** |
| Link **setsu** (chiaki protocol lib) only, write our own app | Medium | Reimplement everything above the protocol | Fallback if chiaki-ng's structure fights us |
| Clean-room from scratch | Very high | Months before first frame | No |
| Wrap the official app | Low | No bitrate/codec control, no color control | No |

Details and the module map: [04-architecture.md](04-architecture.md).
