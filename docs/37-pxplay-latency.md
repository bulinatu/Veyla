# 37 - How PXPlay and PS Remote Play stay fast, and what is left for us

Written after the 2.5.0 pipeline audit (`35-latency-audit.md`). The question was
how PXPlay (formerly PSPlay) and Sony's PS Remote Play feel so close to local
play, and which of those techniques BetterRemotePlay has not used yet.

Short answer: neither product does anything magic, and neither is actually at
zero latency. A local Remote Play session is roughly 40-70 ms end to end on a
wired LAN. What they do is avoid **adding** latency on the client: the decoded
picture reaches the screen without a CPU colour conversion or a queue, and the
presentation path is paced to the display instead of to a timer.

## What the two clients actually do

### PXPlay PC

PXPlay's public release notes describe a "GPU-first rendering architecture with
zero-copy rendering where supported" across D3D11, D3D12 and Vulkan. In
practice that means:

- **Hardware decode straight into a GPU texture.** The decoder's output surface
  is sampled by the renderer. The frame never visits system memory and is never
  converted to RGBA on the CPU.
- **A GPU render pass does colour, scaling and HDR.** FSR 1.0 runs in about
  0.1 ms, tone mapping and debanding are shader passes. There is no CPU image
  processing step between decode and present.
- **"Lowest Latency" frame pacing.** A fresh frame is presented immediately;
  the "Balanced" mode adds pacing only when the stream and display drift.
  PXPlay also offers per-console frame-pacing overrides.
- **Decoder performance profiles.** "Ultra-Low Latency" favours immediate
  output over reordering/buffering, then "Low Latency", "Standard" and
  "Compatibility" trade latency for robustness.
- **A D3D12/Vulkan backend** exists mainly to make the GPU video-processing
  path (upscaling, enhancement, HDR) cheap, not because the API itself is
  faster.

### PS Remote Play

Sony's client is simpler and more fixed:

- HEVC/H.264 hardware decode to a platform surface, composited directly.
- One fixed stream profile (1080p60 on PS5), no client-side scaling or
  enhancement, no frame queue beyond what the compositor needs.
- UDP transport, a separate low-latency input channel, and hardware encode on
  the console. The console and the LAN dominate the latency budget.

### What is common to both

| Technique | PXPlay | PS Remote Play | BetterRemotePlay 2.5.0 |
|---|---|---|---|
| Hardware video decode | yes | yes | yes (d3d11va) |
| Decode output stays on the GPU | yes | yes | **no** - transferred to CPU |
| CPU YUV -> RGBA conversion | no | no | **yes**, ~5.8 ms/frame |
| CPU RGBA -> GPU upload | no | no | yes, `UpdateSubresource` |
| Frame queue | 0-1, configurable | minimal | 0 (buffer 0) |
| Present pacing | immediate/waitable | compositor | waitable, max latency 1 |
| Vsync / tearing control | yes | platform | yes (`video.vsync`) |
| Client-side AI | optional (FSR, FG) | no | DLSS SR / NR / DLSS-G |

The interesting part is the third row. BetterRemotePlay is the only one of the
three that needs pixels in system memory: DLSS Neural Render and the DLSS-G
worker both take CPU RGBA buffers, and the presenter itself was written around
`SetSourceFrame(rgba)`. That design is what costs the ~7 ms decode tail.

## Our measured pipeline (1080p60 HEVC, wired)

From `35-latency-audit.md`, per frame:

| Stage | ms | Notes |
|---|---|---|
| `avcodec_receive_frame` (d3d11va wait) | ~0.0 | overlapped with network |
| `av_hwframe_transfer_data` (GPU -> CPU NV12/P010) | 1.4 | unavoidable while the CPU needs pixels |
| RGBA buffer allocation/zero fill | 0.9 | happens because the buffer was moved out |
| `sws_scale` NV12/P010 -> RGBA | 5.8 | single-threaded; bicubic is the fastest filter measured |
| `UpdateSubresource` to the presenter texture | ~1-2 | 8 MB per frame |
| DLSS SR 1440p submit + readback | 5.5-8.7 | only when SR output is above source size |
| DLSS-G worker, NR off / NR Boost | 6.9-7.7 / 15.6-17.3 | worker process, runs in parallel with present |

So a plain (AI-off) frame spends about 9 ms in client-side decode work, of
which 7.2 ms is the CPU round-trip. With DLSS-G the worker hides most of that
behind the 16.7 ms frame interval, but it is still frame age, not throughput.

## Actionable items, in order of value

### 1. A GPU-only path when AI is off (saves ~7-9 ms of frame age)

This is the one change that closes the gap to PXPlay for plain streaming:

- Create FFmpeg's D3D11VA device on the presenter's `ID3D11Device` (or share
  the decoded texture through a keyed mutex).
- Keep the decoder output as NV12/P010 and expose it to the presenter as a
  shader resource.
- Add a YUV -> RGB shader pass (or `ID3D11VideoProcessorBlt`) with the same
  BT.709/BT.2020, limited-range handling that `sws_scale` does today.
- Keep the existing CPU conversion as the path for DLSS SR/NR/FG, because those
  take RGBA buffers.

Expected: removes the 1.4 ms transfer and the 5.8 ms swscale, and replaces the
CPU upload with a GPU copy. Frame age drops by roughly 7-9 ms in the mode that
does not use AI at all.

### 2. Stop reallocating and zero-filling the RGBA buffer (saves ~0.9 ms)

`Decoder::Convert` calls `out.rgba.resize(8 MB)` every frame, and the AI path
moves the buffer into the worker, so the next frame grows a fresh vector and
zero-fills it before `sws_scale` overwrites every byte. A small buffer pool
(three or four buffers, returned by the worker) removes the allocation, the
zero fill and the allocator jitter. This is independent of item 1 and cheap.

### 3. Upload the RGBA frame without a full `UpdateSubresource` (saves ~1 ms)

When the CPU path stays, a `D3D11_USAGE_DYNAMIC` source texture written with
`Map(WRITE_DISCARD)` is usually faster than `UpdateSubresource` on a default
texture for an 8 MB frame, and it avoids a driver-side staging copy. Measure
both on the target GPU before switching; the win is driver-dependent.

### 4. Keep the presenter settings as they are

The presenter already does what PXPlay's "Lowest Latency" mode does:
`SetMaximumFrameLatency(1)`, a frame-latency waitable object, tearing allowed
when vsync is off, and a single queued frame. Do not add a second buffer to
"smooth" the output; that is exactly the 58-60 ms mistake the audit found in
2.4.7.

### 5. Decoder flags: check, do not guess

The stream is 1080p60 HEVC with no packet loss, so the only possible decoder
win is reorder delay. `AV_CODEC_FLAG_LOW_DELAY` is safe only if the stream has
no B-frames; forcing it on a stream that has them corrupts output. The useful
next step is to log `codecpar->video_delay` and `AVCodecContext.has_b_frames`
once at open. If both are zero, there is nothing to gain and the flag should
stay off.

## Remaining headroom in FG / DLSS / NR

The worker, not the decoder, is the limit in the AI modes. Measured at 240 Hz
output (2 refreshes per generated frame at 2x):

| Configuration | Worker `Process` | Input budget | Verdict |
|---|---|---|---|
| 1440p, NR off, 2x | 6.9-7.7 ms | 16.7 ms (60 fps input) | comfortable |
| 1080p DLAA, NR on, 2x | 8.5 ms | 16.7 ms | comfortable |
| 1440p, NR Boost 75 %, 1 pass, 4x | 15.6-17.3 ms | 33.3 ms (30 fps input) | fits at 30 fps in |
| 1440p, NR 100 %, 1 pass | 16-19 ms | 16.7 ms (60 fps input) | drops ~50 inputs/s |
| 1440p, NR Boost 75 %, 4 passes | ~33 ms | 33.3 ms (30 fps input) | at the edge |

What that means:

- **NR at 1440p has no headroom at 60 fps input.** The 75 % work scale and one
  pass are already the minimum that keeps the feature recognisable. A 60 fps
  stream plus NR at 1440p needs either a smaller work scale (~66 %), fewer
  pixels into the network, or accepting dropped inputs. At 30 fps input the
  same configuration fits with ~16 ms of slack.
- **The 4x multiplier is not free even at 30 fps.** The worker generates three
  frames per input inside the same 33 ms window; `Process` at 15.6-17.3 ms
  leaves room, but a slower GPU or a larger NR scale crosses the line. 2x at
  60 fps input is the lower-latency way to reach 120 Hz, which the audit
  already showed.
- **DLSS SR at 1440p costs 5.5-8.7 ms, almost all readback.** The submit itself
  is under 1 ms. A pipelined readback would move that cost off the presentation
  thread but adds one frame of age; the 2.4.7 measurement said synchronous is
  better for latency, so this stays synchronous.
- **FG off is the fastest AI configuration.** Nothing in the worker is needed
  to present the real frame; the presenter can show it as soon as SR (if any)
  is done. That is the mode item 1 above optimises.

## What was deliberately not copied

- **Vulkan/D3D12 backends.** They buy GPU video-processing features and driver
  robustness, not lower latency by themselves. The current D3D11 presenter
  already runs at 239.8 Hz scanout with zero present misses.
- **Frame generation as a latency feature.** It smooths motion; it does not
  make input arrive sooner. PXPlay's own notes describe it as smoothness, and
  the extra generated frames hold the real frame back.
- **"Smart frame recovery".** Useful on lossy Wi-Fi, but the measured session
  has 0.00 % loss; adding bitstream recovery logic would only add risk here.

## References

- PXPlay PC release notes (GPU-first rendering, zero-copy, D3D12/Vulkan,
  frame pacing modes, decoder profiles):
  https://github.com/streamingdv/PSPlay-Application-Hosting/releases
- Sony, "How to optimise PS Remote Play streaming quality":
  https://www.sony.co.uk/electronics/support/articles/SX365201
- chiaki-ng hardware decoder guidance (d3d11va on Windows):
  https://git.sr.ht/~thestr4ng3r/chiaki
