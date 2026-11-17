# 19 - DLSS and neural rendering: what the technologies are, and what this app does

Status date: 2026-11-17. Research notes plus the mapping to BetterRemotePlay.
Sources are listed at the end; the NVIDIA statements quoted below are from
the DLSS 4.5 launch article (January 2026) and the DLSS 5 launch article.

## The DLSS family

| Technology | What it does | Inputs | Where it sits |
|---|---|---|---|
| DLSS Super Resolution (SR) | Reconstructs a higher-resolution image from a lower-res render | colour, depth, motion vectors, jitter, exposure | replaces the upscale |
| DLSS Ray Reconstruction (RR) | Neural denoiser for ray-traced lighting | colour, albedo, normals, depth, motion | replaces the denoisers |
| DLSS Frame Generation (FG / MFG) | Generates intermediate frames between rendered ones | two rendered frames + motion/depth, Optical Flow hardware | after SR, before present |
| DLSS 5 Neural Rendering (NR) | Adds AI-generated lighting/materials/detail anchored to the rendered frame | colour, albedo, lighting, normals, motion vectors | optional, works alongside SR / MFG / RR |
| RTX Video Super Resolution (VSR) | AI upscaler for **video playback** (Video Effects SDK) | video frame only | replaces the video upscaler |
| Reflex | Low-latency queue management | - | scheduling, not imaging |

### Super Resolution and its models

- SR has been temporal since DLSS 2: it accumulates sub-pixel samples over
  frames using engine motion vectors, so a lower internal render resolution
  becomes a stable, higher output resolution.
- DLSS 4 (2025) replaced the CNN with a transformer model.
- DLSS 4.5 (CES 2026) is a **second-generation transformer**, trained and
  inferred in linear light, with FP8 inference on RTX 40/50. NVIDIA states it
  cuts ghosting and improves stability, with the biggest gains in Performance
  and Ultra Performance modes.
- Model presets in DLSS 4.5:
  - **K** - the DLSS 4.0 transformer, the model for all modes before 4.5;
  - **L** - the 4.5 model tuned for Ultra Performance / 4K output;
  - **M** - the 4.5 model tuned for Performance mode (roughly a 2x2 upscale,
    e.g. 960x540 -> 1080p, 1280x720 -> 1440p, 1920x1080 -> 4K).
  - The NVIDIA app's "Recommended" maps M to Performance, L to Ultra
    Performance and K to the remaining modes. L and M carry a heavier cost on
    RTX 20/30 (no native FP8).
- In this app the three presets are exposed as **K / L / M** and can be changed
  live. At a 1080p -> 1440p video upscale they measured within noise of each
  other (~2.8 ms each after the timing fix), so the choice is about image
  character, not speed.

### DLSS 5 Neural Rendering

- DLSS 5 is marketed as **3D-Guided Neural Rendering**. Unlike SR, which
  reconstructs existing samples, NR *generates* lighting and material detail
  that real-time rendering had to strip away, anchored to the rendered frame
  and the engine's 3D data (colour, surface albedo, lighting, normals,
  motion vectors).
- It is deliberately **one frame in, one frame out** - not a video-generation
  model working on batches - and it uses the engine's motion vectors so the
  generated detail does not swim or drift.
- NVIDIA: "DLSS 5 operates as an optional and independent feature, giving
  studios flexibility to enable it alongside Super Resolution, Multi Frame
  Generation and Ray Reconstruction." In practice DLSS 5 titles use NR as the
  new renderer, but nothing in the technology forbids running SR afterwards.
- Runtime: `nvngx_dlssnr.dll` (NGX feature 18, ~158-166 MB of weights). This is
  the DLL the DLSSNR mods and the NeuralScreen worker use.

## What "neural rendering" means here

"Neural rendering" is the general idea of putting a trained network inside the
rendering pipeline instead of only in post. DLSS 5 NR is the consumer form of
that: a network that reads the frame plus scene guides and writes a richer
frame. It is **not** the same thing as an upscaler:

- an upscaler changes resolution (SR, VSR, FSR, Lanczos);
- NR changes the content at the same resolution (lighting, materials, detail).

That distinction is why BetterRemotePlay lists **DLSS SR under Upscaling** and
**NR on its own Neural page**: they do different jobs, and now they can run
one after the other.

## How BetterRemotePlay maps this to a remote-play stream

The console sends at most 1080p; the client owns everything after decode:

```
HEVC 1080p -> decode -> RGBA 1080p
                       -> [optional] NR (NGX worker, 1080p, own thread)
                       -> upscale to target:
                            DLSS SR (D3D12, 1080p -> 1440p/2160p)   <- can follow NR
                            FSR1 / Catmull-Rom / Lanczos           <- or instead
                       -> present
```

- **NR in video mode.** There is no engine in a video stream, so the worker is
  fed zero depth and a zero motion field and history is reset every frame.
  Tested with a bar moving 12 px/frame: no ghosting; the effect is real but
  subtle (mean |input-output| 2.6/255). This is a video enhancer, not DLSS 5
  in a game: no albedo, normals or engine motion vectors, and the network
  cannot invent scene detail the way a game's NR can.
- **SR in video mode.** The same captured-colour contract: zero depth, zero
  motion, reset every frame. Without motion vectors SR is effectively a
  spatial transformer upscale, which is exactly what a 2D video needs to avoid
  ghosting.
- **NR + SR.** Both are enabled together: NR enhances the 1080p frame, DLSS SR
  upscales that result. Measured 1080p -> 2560x1440: NR 10.3 ms + SR 2.8 ms =
  **13.2 ms/frame, 75.7 fps ceiling**, so the chain fits a 60 fps stream.
- **Exclusivity that remains.** RTX VSR replaces the whole upscale stage (it
  is a different video-effects runtime), and NVIDIA VFG replaces the frame
  cadence; both still turn NR off, as before. DLSS SR does not.
- **NR resolution.** The worker caps NGX NR at 2560x1440, so NR runs at the
  stream size or on the DLSS SR output when the NVIDIA order is selected.

## Performance notes from this machine (RTX 4090, 2560x1440)

| Stage | Time | Notes |
|---|---|---|
| NR, 1080p full network, 1 pass | ~10.3 ms | own thread, spikes to 20-25 ms |
| NR, Boost 70% + 2 passes | ~12 ms | cascade, same cost as full 1 pass |
| DLSS SR, 1080p -> 1440p | 2.8 ms probe, 3.8-4.2 ms live | was ~15 ms before the fence-wait fix |
| NR -> DLSS SR chain | ~13.2 ms probe / ~14-15 ms live | 75.7 fps ceiling; holds 60 fps live |
| DLSS SR + FG 2x, 1440p | SR async + 14 MB readback | 120 fps displayed; parent source tick ~5-6 ms |
| FSR1 (EASU) + bicubic chroma | ~2 ms | cheapest path |

The ~15 ms SR number that earlier builds reported was not the network: the
GPU fence was polled with `Sleep(1)`, which the default Windows timer rounds
up to 15.6 ms. Waiting on a fence event removed it. The same pattern was fixed
in the NR pipe reader.

## What this app does *not* do

- It does not provide DLSS 5's 3D guides (albedo, normals, real motion
  vectors); a video stream has none. NR here is the vendor runtime in its
  video/2D mode.
- It does not run Multi Frame Generation beyond what the NeuralScreen worker
  sustains; DLSS Frame Generation 2x is the stable path, and DLSS SR can feed
  it at the render resolution. NVIDIA VFG 2x is a separate, legacy path.
- It does not ship NVIDIA's DLLs. `nvngx_dlss.dll` and `nvngx_dlssnr.dll` are
  user-supplied; `scripts/install-ngx-runtime.ps1` and
  `scripts/install-nr-runtime.ps1` copy them into place.

## Sources

- NVIDIA, "NVIDIA DLSS 4.5 Delivers Major Upgrade With 2nd Gen Transformer
  Model For Super Resolution & 6X Dynamic Multi Frame Generation" (Jan 2026):
  https://www.nvidia.com/en-us/geforce/news/dlss-4-5-dynamic-multi-frame-gen-6x-2nd-gen-transformer-super-res/
- NVIDIA, "NVIDIA DLSS 5 Available Now: 3D-Guided Neural Rendering Debuts In
  NBA 2K27" (2026):
  https://www.nvidia.com/en-us/geforce/news/dlss-5-3d-guided-neural-rendering/
- NVIDIA DLSS technology page:
  https://www.nvidia.com/en-us/geforce/technologies/dlss/
- Digital Foundry, "DLSS 4.5 Review" (preset K/L/M behaviour):
  https://www.digitalfoundry.net/features/dlss-4-5-review-where-its-better-and-where-it-needs-work
- Wikipedia, "Deep Learning Super Sampling" (history, DLSS 5 summary):
  https://en.wikipedia.org/wiki/Deep_Learning_Super_Sampling
- OptiScaler DLSSNR and the NeuralScreen/NeuralScreen-style worker packages
  (third-party uses of `nvngx_dlssnr.dll`, feature 18).
