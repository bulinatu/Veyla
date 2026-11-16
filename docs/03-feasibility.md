# 03 — Feasibility: your feature list, line by line

Legend:

- **✅ Wire** — the PS5 can actually deliver this.
- **🟡 Post** — the PS5 cannot, but the PC can add it after decode. Real, with caveats.
- **🔴 Not as described** — impossible in the form asked, with the closest honest workaround.
- **⚪ Unverified** — depends on hardware/firmware/DLSS-version specifics; must be tested.

---

## Summary table

| # | Feature | Verdict | Reality in one line |
|---|---|---|---|
| 1 | Minimal to zero delay | 🟡 Post | ~40–70 ms floor; zero is physics-impossible |
| 2 | Not washed out | ✅ Wire + 🟡 Post | Fixable: full-range + correct matrix + high bitrate |
| 3 | High/ultimate bitrate | ✅ Wire | 30–50 Mbps requested; encoder caps it, but it helps a lot |
| 4 | Upscaling (DLSS/DLAA) | 🟡 Post | DLSS no; RTX VSR / NIS / FSR1 yes |
| 5 | 720/1080 default, 1440p/4K via upscaling | ✅ Wire + 🟡 Post | 1080p in, 1440p/2160p out |
| 6 | 30/60 default, 120 via FG, 240 via MFG | 🔴 Native / 🟡 Post | 60 in, interpolated up; latency cost |
| 7 | FrameGen (DLSS) | 🔴 Not as described | DLSS FG needs engine data; interpolation is the analog |
| 8 | Neural rendering / DLSS 5 | ⚪ Unverified | Engine-side; video-side analog is RTX VSR; keep pluggable |
| 9 | PS5-like interface | 🟡 Post | Pure UI work, fully doable |
| 10 | Full HDR | ✅ Wire + 🟡 Post | HEVC 10-bit HDR exists; needs a correct 10-bit pipeline |
| 11 | PSN login for pairing | ✅ Wire | PSN OAuth client-side; PSPlay proves it |
| 12 | AV1 / H.264 / H.265 | 🔴 AV1 / ✅ H.26x | Console offers AVC + HEVC only |
| 13 | CUDA, Vulkan, D3D11, D3D12, DXVA decoders | ✅ Post | All available through FFmpeg hwaccel + custom presenter |

---

## 1. Minimal to zero delay

**Reality:** the console encodes at least one frame before transmitting. At 60 fps that is 16.7 ms of pure physics. Add encode, network, decode, present. A wired LAN floor is ~40 ms; the official app lands around 60–90 ms.

**What we do:**
- Wired-first defaults (the app should tell the user when it detects Wi-Fi).
- Decoder queue depth 1; drop late frames instead of queueing them.
- DXGI flip model, mailbox/no-vsync present, `ALLOW_TEARING`.
- Audio as the clock source, not video.
- Controller input sent immediately on the control channel (it does not wait for video).
- Per-frame latency instrumentation, visible in an overlay — same discipline as Moonlight.

**Cannot do:** zero. Anyone promising zero is measuring incorrectly.

---

## 2. Washed-out colors

**Why it happens:** Remote Play SDR is BT.709 **limited range** (16–235). Every stage that treats it as full-range, or applies the wrong matrix (BT.601 vs BT.709), or does a cheap bilinear chroma upsample, flattens contrast. Two more causes: streaming gamma mismatch, and the console's "HDR always on" putting SDR content inside a PQ container.

**Fix (all client-side):**
- Explicit YUV → RGB with correct matrix, range expansion in FP16, **full-range RGB output**.
- High-quality chroma upsampling (Catmull-Rom or better, not bilinear).
- 10-bit surface end-to-end; no 8-bit round trip.
- For SDR-in-HDR consoles: detect PQ content vs SDR-in-PQ and tone-map deliberately, not accidentally.
- Bitrate: chroma is the first thing to die at low bitrate — 1080p at 30–40 Mbps fixes perceived color saturation.

**Verdict:** fully solvable. This is a pipeline-correctness problem, not a protocol problem, and it's one of the top reasons this project is worth building.

---

## 3. High to "ultimate" bitrate

**Reality:** the client requests a bitrate; the console's hardware encoder emits what it can. Community measurements: 15–30 Mbps typical, 30–50 Mbps requests accepted with visible improvement, diminishing returns past ~40 Mbps at 1080p. Portal mods show session-layer acceptance near 100 Mbps, but that's a target, not guaranteed output — the scene content determines actual bits.

**Plan:**
- Expose bitrate as a first-class setting (10–100 Mbps), default **35 Mbps** for 1080p60 on wired LAN.
- Show *actual* received bitrate live, next to requested — because the gap is the interesting number.
- Adaptive mode that backs off on loss, but never below a user floor.

**Cannot do:** force the encoder past its quality ceiling. "Ultimate bitrate" is real up to the point where more bits buy nothing.

---

## 4. Upscaling — DLSS / DLAA

**Why DLSS is impossible here:** DLSS reconstructs a frame from a *lower-resolution render* plus **motion vectors, depth, jitter offsets, and exposure** provided by the game engine. A video stream is a finished 2D image. There is no lower-res render, no vectors, no depth. Same for DLAA (which is DLSS at native res — still needs engine data).

**Honest equivalents, all of which work on a video stream:**

| Technique | What it is | Quality at 1080p→1440p | Cost |
|---|---|---|---|
| **NVIDIA RTX Video Super Resolution** | NVIDIA's AI video upscaler (NGX `VideoSuperRes`) | Best in class for video, built for exactly this | Needs NVIDIA RTX; app integration requires the NGX video SDK + license terms |
| **NIS** (NVIDIA Image Scaling) | Spatial sharpening scaler | Good, sharp, slightly crunchy | Trivial, any GPU |
| **FSR 1 (EASU + RCAS)** | Spatial upscaler + sharpener | Good, widely used in video players | Trivial, any GPU |
| **FSR 2/3** | Temporal upscaler | **No** — needs motion vectors/depth | — |
| Lanczos/Catmull-Rom + CAS | Classic filtered scale + contrast-adaptive sharpen | Fine at 1.33×, soft at 2× | Trivial |

**Plan:** a pluggable upscale stage. Default: FSR1/NIS (works everywhere). Optional: RTX VSR on RTX GPUs if licensing allows, with a clean fallback. Design the stage as a chain, so if NVIDIA ships a video-domain DLSS successor we drop it in without touching the rest.

**Cannot do:** DLSS or DLAA proper.

---

## 5. Resolutions

- **Wire:** 720p or 1080p only. 1080p is strictly better even on a 720p display (chroma subsampling), which is why chiaki-ng's docs recommend it.
- **Post:** display at 1080p, 1440p (2560×1440), or 2160p (3840×2160). Note: "4K" is 2160p; 2560 is the horizontal size of 1440p. Your "4k 2560p" is two different resolutions — both are supported as output modes.
- Upscaling factor: 1.33× (1080p→1440p) and 2.0× (1080p→2160p). 1.33× looks great; 2.0× needs a good algorithm (VSR/NIS).

---

## 6. Frame rates — 30/60 native, 120/240 via frame generation

**Wire:** 30 and 60 only. The console offers no 120 mode.

**Post (frame interpolation):**

| Mode | Method | Result | Latency cost | Honest verdict |
|---|---|---|---|---|
| 60 → 120 | Optical-flow interpolation (RIFE-class, or Lossless Scaling LSFG) | Smooth motion, occasional UI artifacts | +1–2 frames (17–33 ms) | **Works, genuinely nice** for slow/medium motion |
| 60 → 240 | 4× interpolation | Artifact-prone, "soap opera" judder | +3–4 frames (50–67 ms) | Technically possible, **not recommended** |
| 30 → 60 | 2× interpolation | Big improvement | +1–2 frames | Works well |

**Design:** interpolation is per-profile opt-in, shows its latency cost, and can be toggled with a hotkey mid-session. Default off — because "minimal delay" was requirement #1 and these two requirements fight each other. We let the user choose which one they want, per game. That's the right call: MLB The Show and a turn-based RPG do not want the same settings.

**Cannot do:** real 120/240 from the console.

---

## 7. DLSS Frame Generation

**Why impossible:** DLSS FG is not interpolation of two images. It needs the engine's motion vectors, depth, and optical flow to synthesize an intermediate frame, and it runs inside the game's swapchain. The console renders and encodes; we receive frames with no engine metadata.

**Workarounds, ranked by honesty:**
1. **Optical-flow interpolation** — what everyone actually means when they say "frame gen on a stream". Works, adds latency. (See §6.)
2. **Driver-level frame generation** (NVIDIA "Smooth Motion", DLSS-4-era driver feature that generates frames for DX11/DX12 apps without engine integration). It is applied at the driver/display level, so it can in principle apply to our fullscreen swapchain. Adds latency, benefits from a real display refresh. ⚪ **Must be tested** against our presenter — do not promise it in the UI until measured.
3. **"Workaround the workaround":** if a game is available on PC too, stream the PC version and use real DLSS FG. Not our problem to solve, but worth saying out loud.

---

## 8. Neural rendering / "DLSS 5"

**Status:** ⚪ Unverified. I can't confirm what DLSS 5 does or whether it has any video-domain API. What I can say with confidence:

- Neural *rendering* is a game-engine concept (denoising, neural materials, neural radiance caching). It requires engine integration. It cannot be applied to a decoded H.265 frame.
- The video-domain neural stack that **does** exist and is applicable: **RTX Video Super Resolution** (upscaling), **RTX Video HDR** (SDR→HDR via neural net), and neural deblocking/denoising in NVIDIA's video pipeline.
- RTX Video HDR is actually a great fit for your "full HDR" requirement when the source is SDR: it gives a plausible HDR presentation of an SDR stream. It is not "real" HDR, and the UI must say so.

**Plan:** architect the post-process graph as **named, replaceable stages** (`decode → color → upscale → tonemap → interpolate → present`). If a future DLSS/video neural API appears, it becomes one more stage. No promises in the UI beyond what's implemented and measured.

---

## 9. PS5-like interface

Pure UI work, no protocol implications. Doable at any fidelity:

- Boot: PS5-style startup animation + sound (own assets, or user-supplied).
- Home: horizontal game rail with large tiles, background art, "switcher"-style top bar.
- Per-game tiles: last played, play time, "Stream" button.
- In-stream overlay: PS button equivalent, quick settings (bitrate, codec, upscale, interpolation), latency readout.
- Sound design: subtle UI ticks (again: own assets — do not ship Sony's).

**Caveat:** game library/artwork comes from PSN. With PSN OAuth (item 11) we can fetch the user's library metadata. Without it, the UI falls back to locally added shortcuts. Don't scrape store pages.

---

## 10. Full HDR

**Wire:** yes — PS5 streams HDR as HEVC Main10 (PQ, BT.2020) when the console is set to HDR and the client requests the HDR codec profile.

**Post — what "fully supported" requires:**
1. 10-bit decode (NVDEC/D3D11VA/D3D12VA all handle Main10).
2. FP16 processing, BT.2020 → display primaries, PQ → scRGB/FP16 for Windows HDR.
3. **10-bit HDR swapchain** (DXGI `R10G10B10A2` + HDR metadata), not an 8-bit window with a filter.
4. Correct peak luminance from the display's EDID, not a hardcoded 1000 nits.
5. SDR content inside the HDR session handled deliberately (tone-map, don't stretch).

**Reality check:** chiaki-ng already ships an HDR path (`H265 HDR`), so this is proven feasible. Our job is to make it *correct* on Windows HDR displays — which is where most clients are weakest.

---

## 11. PSN login for pairing

**Feasible:** yes. Client-side PSN OAuth (authorization code + PKCE, same client ID family the PS App uses) gives us:
- Account ID (the thing the pairing actually needs),
- online ID and avatar,
- optionally the game library and play history for the UI.

**What it does not remove:** the console PIN. `Link Device` is a physical-presence check on the console and must stay. "Easier pairing" = no manual base64 copy-paste, plus remembered keys afterward.

**Risk:** ToS. Doing OAuth against Sony's endpoints outside their app is the same gray area as PSPlay. Document it, don't hide it, let the user opt in. Also: store tokens in the OS credential vault (DPAPI/Credential Manager), never in a config file.

---

## 12. AV1 / H.264 / H.265

- **H.264 (AVC):** ✅ supported, 8-bit, lowest latency, widest decoder support.
- **H.265 (HEVC):** ✅ supported, 8-bit SDR or 10-bit HDR, better quality per bit — **the default choice**.
- **AV1:** 🔴 the PS5's Remote Play encoder does not offer it. There is no negotiation flag that makes it appear. Workarounds that make sense:
  - **AV1 decode** on the PC for *recording/re-streaming* our own output (e.g., to another device), where we control the encoder.
  - Nothing else. Don't advertise AV1 in the connection UI as if it were selectable.

---

## 13. Decoders: CUDA, Vulkan, D3D11, D3D12, DXVA

All available via FFmpeg hardware acceleration on Windows:

| Decoder | FFmpeg hwaccel | Notes | Priority |
|---|---|---|---|
| **D3D11VA** | `d3d11va` | Best default on Windows; zero-copy into D3D11 textures, works with flip-model present | **Default** |
| **D3D12VA** | `d3d12va` | Available in FFmpeg 7.x+; zero-copy into D3D12, best if we build a D3D12 presenter | Optional |
| **DXVA2** | `dxva2` | Legacy, D3D9 interop, slower path | Compatibility only |
| **CUDA / NVDEC** | `cuda` | Great on NVIDIA; slightly more plumbing for presentation | Advanced option |
| **Vulkan Video** | `vulkan` | Growing support (NVIDIA/AMD/Intel); natural if the presenter is Vulkan | Advanced option |
| **Software** | — | Fallback for VMs/old GPUs | Fallback |

**Plan:** one decoder abstraction, capability-probed at startup, user-overridable, with a **benchmark button** that measures decode time per available decoder on the actual machine. That's more useful than a settings dropdown with no feedback.

---

## What this adds up to

**Achievable and worth building:** correct color, high bitrate, 1080p60 HEVC/HDR, GPU upscaling to 1440p/4K, optional interpolation, PSN pairing, all major decoders, PS5-style UI, honest latency instrumentation.

**Not achievable, and we say so in the UI:** 4K/120 from the console, AV1 on the wire, DLSS/DLAA/DLSS-FG as such.
