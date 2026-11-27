# 35 - Latency audit: decode, DLSS SR and DLSS-G

Measured on 2026-11-23 with the 2.4.7 engine, the user's own session profile
(PS5-580 at 192.168.1.21, HEVC, 100 Mbit/s, 1080p capture) and the frame probe
(`BRP_NEXT_FRAME_PROBE=1`). Three isolated sessions, each ~22 s, same scene,
same PC. The console allows one session at a time and was free for all three.

## What the probe fields mean

| Field | Meaning |
|---|---|
| `dec p50/max` | Time from feeding a packet to the decoder until the RGBA frame exists (hardware decode + GPU->CPU transfer + NV12->RGBA swscale). Runs on the decode thread. |
| `queue` | Decoded frames waiting in the presentation queue. |
| `age avg/max` | Age of the frame's pixels when it is pulled for presentation: queue wait and pipelining, excluding decode. |
| `sr cpu` | DLSS SR submit + synchronous readback + worker hand-off for one frame. |
| `loop p50/p99` | Time spent inside one engine iteration (pull, SR, submit). |

`age + dec` is the latency the client adds after the console's packets arrive.
The remaining visible delay is the network/console encode plus the DLSS-G
worker's own display window.

## Results (steady state)

| Session | Source | Buffer | FG | age avg | dec p50 | sr cpu | loop p50/p99 | cadence p50/p99 |
|---|---|---|---|---|---|---|---|---|
| A (current) | 30 fps | 2 | 4x | **58-60 ms** | 8.4 ms | 4.5-4.9 ms | 1.5 / 6.8 ms | 33.3 / 34.8 ms |
| B | 30 fps | 0 | 4x | **1.3 ms** | 8.0 ms | 4.4 ms | 6.0 / 11.0 ms | 33.3 / 38.2 ms |
| C (new) | 60 fps | 0 | 2x | **1.2 ms** | 8.4 ms | 4.3 ms | 6.9 / 11.2 ms | 16.6 / 22.4 ms |

Zero packet loss and zero drops in all three sessions. Scanout reads 0 Hz while
the DLSS-G worker covers the stream window; the parent presenter is idle by
design in that mode.

## Findings

1. **The presentation queue was the latency.** At buffer 2 the phase-locked
   pull keeps two frames primed, which at 30 fps is 58-60 ms of age before the
   frame is even submitted. Buffer 0 pulls the oldest decoded frame as soon as
   it exists: age drops to 1.2-1.3 ms with the same clean cadence.
2. **30 fps doubles everything.** A frame interval is 33.3 ms, so every buffered
   frame costs twice as much as at 60 fps. 60 fps + DLSS-G 2x reaches the same
   120 Hz output as 30 fps + 4x, with half the input interval.
3. **Decode is the largest per-frame cost: 8.4 ms.** Hardware decode plus the
   GPU->CPU transfer and NV12->RGBA conversion is half the 60 fps budget. It is
   not a bug: the presenter consumes CPU RGBA, and DLSS SR/NR need it too.
   Software decode would be slower; H.264 is not an option on this machine
   (hardware decode produced the grey-block artefact).
4. **DLSS SR (DLAA) costs ~4.5 ms per frame**, almost all of it the synchronous
   readback that feeds the DLSS-G worker. Turning SR off would save that and
   soften the image; it is a visual choice, not a defect.
5. **The loop is 6.9 ms p50 / 11.2 ms p99 at 60 fps** (41-67 % of a frame
   interval). There is headroom, but not much: a slower CPU would start
   missing the 16.7 ms submission window in buffer 0.

## Removed while auditing

- `Frame()` wrote `build\engine-frame.log` through a hardcoded absolute path
  every 300 frames. Removed.
- The Stream page exposed a **Codec** selector (Auto / H.264 / H.265 / AV1)
  that did nothing: the session always negotiates H.265 and the decoder always
  opens HEVC. The row is gone; HEVC stays the only transport.
- The Advanced page exposed a GPU group (`fp16`, `int8`, frame buffers, memory
  limit, priority, adapter) that no engine path reads; changing any of them
  only asked for an application restart. Removed, together with the dead
  restart classification entries.
- Removed the dead `Debug log`, `Experimental` and `GPU present` toggles.

## Settings applied

`streaming.fps = 60`, `streaming.frame_buffer = 0`, `ai.fg_multiplier = 2`,
`streaming.codec = hevc` (informational; the transport is hardcoded). SR
(DLAA) and NR stay on. Reverting to 30 fps + buffer 1 brings back the smoother
cadence at the cost of roughly 60 ms.

## Next candidates (not done)

- Pipelined/async SR readback: removes the GPU wait from the presentation
  thread, but 2.4.7 measured sync as better for frame age.
- Keeping decoded frames on the GPU and converting NV12 in the presenter shader
  would remove both the transfer and swscale (~8 ms). It needs FFmpeg's D3D11VA
  device to share the presenter's ID3D11Device and a YUV shader path in the
  presenter, plus a CPU path still for SR/NR.
- Buffer 0 has no spare frame; a single late decode is a repeated frame. If
  that shows up, buffer 1 is the middle ground (+16.7 ms at 60 fps).

## Round 2: decode breakdown and the DLSS "Max" preset

Measured again with `BRP_DECODE_PROBE=1` (per-stage timing inside the decoder)
and `fg <ms> <mult>x drops <n>` appended to the frame probe.

### Where the 8.4 ms of decode goes (1080p HEVC, d3d11va)

| Stage | ms |
|---|---|
| `avcodec_receive_frame` (hardware decode wait) | 0.00 |
| `av_hwframe_transfer_data` (GPU -> CPU NV12) | 1.4 |
| `out.rgba.resize` (8 MB zero fill) | 0.9 |
| `sws_scale` NV12 -> RGBA | 5.8 |

Chroma filter comparison on the same clip (software decode, ms/frame):
bilinear 11.17, **bicubic 10.57**, lanczos 14.18. Bicubic is already the best
choice; there is no cheaper filter to switch to.

Parallel `sws_scale` was implemented and rejected. Two variants were tested
against a bit-exact reference:

- slicing with `srcSliceY` and per-thread contexts: swscale warns "Slices
  start in the middle!" and the result differs from row `height/workers`
  onward (max 255), so it cannot be used;
- slice-local source pointers with an overlap: correct except the last three
  rows, because the scaler keeps the full-frame height and reads past the
  sub-image. The path was removed again; the conversion stays one call.

### DLSS SR "Max" was silently disabling SR

`NVSDK_NGX_PerfQuality_Value` 4 (Ultra Quality) no longer exists in the DLSS
2.x+ API. `CreateFeature` returned `0xBAD00010` (`FAIL_UnsupportedParameter`)
for the "Max" quality row, which set `dlss_failed_` and turned SR off for the
whole session with only a toast. The mapping now uses `MaxQuality` (2) for both
66 % and 77 % and relies on the explicit sizes: with SR 1440p the feature is
created at 1920x1080 -> 2560x1440 and runs.

### Neural Render inside the DLSS-G worker has a 1440p ceiling

`fg <ms>` is the worker's own `Process` time. At 60 fps that must stay under
16.7 ms or the newest-frame slot starts dropping inputs:

| Worker input | NR | Process ms | Inputs dropped |
|---|---|---|---|
| 1080p (DLAA) | off | 7-8 | none |
| 1080p (DLAA) | on | 8.5 | none |
| 1440p (SR Max) | **on** | **16-19** | ~50/s |
| 1440p (SR Max) | off | 6.5-7.8 | none |

NR costs ~10 ms at 1440p and is not reduced by halving `neural_passes`
(measured 16-17 ms at 2 passes). So 1440p and NR do not fit together at 60 fps
into a 120 Hz FG output. The applied configuration is **SR 1440p + NR off**;
the alternative that keeps NR is **SR 1080p (DLAA)**, where the worker has
headroom.

## Round 2 settings

`streaming.fps = 60`, `streaming.frame_buffer = 0`, `ai.fg_multiplier = 2`,
`ai.sr_output = 1440p`, `ai.sr_quality = 4` (Max), `ai.neural_render = false`.

Remaining latency is the console's capture/encode, the LAN hop, the 8.4 ms
decode, the ~6 ms SR readback at 1440p and the DLSS-G worker's own display
window. The next real lever is keeping the frame on the GPU end to end:
D3D11VA decode into a texture shared with the presenter, a YUV->RGB shader
pass, and a GPU path into DLSS SR. That removes the 1.4 ms transfer and the
5.8 ms swscale from every frame.

## Round 3: the GPU end-to-end path (2.5.0)

Done. The shell now creates one D3D11 device with `VIDEO_SUPPORT` and
multithread protection. FFmpeg decodes d3d11va on that device, the video
processor converts its NV12/P010 surface to RGBA on the same device, and the
stream presenter is created on it too. What happens next depends on whether an
AI stage needs CPU pixels:

- **No AI (NR/SR/FG all off):** the video-processor output stays a texture on
  the shared device and the presenter samples it through an SRV. No GPU->CPU
  transfer, no swscale, no RGBA upload and no readback. `DecodedFrame::gpu_view`
  carries the SRV; `rgba` stays empty.
- **Any AI on:** the same video-processor blit writes to a staging texture and
  the CPU reads the RGBA result, because DLSS SR/RIFE and the NR/FG worker all
  take system-memory RGBA. This is the minimum the current worker protocol
  allows.

Measured with the frame probe on the same console and PC:

| Mode | Before | After |
|---|---|---|
| AI off, 1080p60 in, 1440p out | dec 8.2-8.5 ms, loop p50 ~6.9 ms | **dec 0.37-0.48 ms, loop p50 0.67-0.70 ms** |
| AI off, presentation | 239.8 Hz, 0 misses | 240.0 Hz, 0 misses, age 0.2-0.3 ms |
| AI on (SR + NR 3 passes + FG 4x) | dec 8.2-8.5 ms | **dec 2.8-3.5 ms** |
| AI on, steady state | n/a | loop p50 4.5 ms, worker 9.9-12.5 ms, 0 drops |

Visual verification: screen captures in both modes show correct BT.709 colours
(red car, green grass, skin tones) and no crop/offset; the coded surface is
1920x1152, the stream is 1920x1080, and the video-processor source rect crops
the padding.

Turning AI off live drops the next frames back to 0.1 ms decode and 240 Hz
scanout; turning it on pays the known one-time NGX/worker start (~5 s) and then
settles at 3 ms decode. The DLSS-G worker stop no longer logs a spurious
`pipe closed` failure: an intentional `Stop()` is distinguished from a runtime
failure.

The worker itself is still fed RGBA over shared memory. A fully GPU NR/FG path
would need a texture-input protocol in `nr/native/nvngx.dll` (or in-process
NGX); the worker's existing WGC capture mode is not a latency win because it
adds a window-capture frame before the neural pass.

## Round 4: SR/FG timing audit (after the GPU path)

Two real costs were found and fixed:

1. **NR off still built a three-pass cascade.** `NeuralOptionsFrom()` sent the
   saved `neural_passes` even when Neural Render was disabled. The worker
   logged `NR cascade: asked=3, effective=1` and its `Process` took **113-120
   ms**, dropping inputs. Forcing one pass when NR is off brings the same
   1080p FG 2x case to **7.2 ms**.
2. **SR 1440p Quality paid a CPU downscale.** The 1920x1080 console frame was
   downscaled to 1706x960 before DLSS, costing 3-4 ms. When the requested
   render size is within 75 % of the source, the decoded frame is now fed
   directly (1920x1080 -> 2560x1440, 75 %). SR-only `sr cpu` dropped from
   **3.61 ms to 0.77 ms**; the SR texture is still handed to the presenter as
   an SRV with no readback.

Measured with 1080p60 in / 1440p out:

| Path | SR submit | SR readback / CPU | Worker | Added frame hold |
|---|---|---|---|---|
| SR only, FG off | 0.77 ms | **0** (SRV direct) | - | 0 |
| FG 2x, 60 fps, NR off | 0.52 ms | 7.2 ms (1440p RGBA) | 7.2 ms | 1 output frame (~8.3 ms) |
| FG 4x, 30 fps, NR off | 0.58 ms | ~7 ms (1440p) | ~10 ms | 3 output frames (~25 ms) |
| FG 4x, 30 fps, DLAA 1080p, NR 3 passes / 100 % | 0.46 ms | 3.5 ms | 8.3 ms | 3 output frames + NR |
| AI off | - | - | - | **0.4 ms decode, 0.2 ms age** |

The SR-only path is effectively free. With FG on, the 1440p RGBA readback is
the SR cost (the worker protocol needs system memory), and the multiplier is
the larger term: 2x holds the real frame for one output interval, 4x for three.
**60 fps + 2x is the low-latency way to reach 120 Hz**; 30 fps + 4x reaches the
same rate with ~17 ms more hold. NR at 100 % adds a full-frame pass on top; the
75 % Boost / one pass setting is the one that fits.

Reflex markers do not change any of these numbers (they only wrap the parent
presenter; with DLSS-G the worker owns its swapchain).

## Round 5: the 100 ms report was the frame buffer

A live A/B on the user's settings (30 fps, FG 4x, 1920x1080 DLAA) showed:

| Setting | Queue | Age at pull |
|---|---|---|
| `frame_buffer = 1` | 2 | **92.6 ms** |
| `frame_buffer = 0` | 0 | **0.0 ms** |

The phase-locked path was letting the queue drift to four frames, and at 30 fps
that is three source intervals before the AI or presenter sees the picture. The
queue cap now drops when the depth exceeds the target instead of allowing two
extra frames; the same buffer-1 test fell to 61 ms. That is still two source
intervals, so the real fix is **buffer 0**.

The user profile was migrated to the low-latency combination:

- 60 fps (halves the input interval)
- `frame_buffer = 0` (0.0 ms age)
- FG 2x (one held output frame instead of three)
- vsync off (tearing path)
- NR 75 % / one pass

Measured after the migration: `age 0.0 ms`, SR readback 3.8 ms, worker 5.1 ms,
cadence 16.6 ms. The 100 ms was almost entirely the queue, not the decoder and
not the controller path.

## Round 6: 30 fps games and duplicate input frames

A 30 fps game can be streamed at 60 fps with every other picture repeated. The
decoder now samples a 64x64 RGB signature of every RGBA frame and marks a frame
`duplicate` when it matches the previous one. The probe prints the unique
content rate (`content 60 fps`). Effects:

- NR is skipped on repeated frames (no new detail for the network).
- SR is skipped and the previous SRV is reused.
- DLSS-G is not fed a repeated frame; the worker keeps presenting its generated
  sequence. On a 30 fps game streamed at 60, 2x now means 60 real output frames
  and 4x means 120, instead of processing twice the frames for the same picture.
- The UI shows `fps (content fps)` on Home and a "Detected content" row on the
  Frame Gen page, so a 30 fps game is visible even when the stream is set to
  60.

The signature is a sample (64 rows x 64 columns), not a full-frame hash; it
costs a few microseconds and does not touch the 8 MB RGBA buffer again.

## Round 7: compared against a reference Java/Vulkan client

The reference client runs the same console protocol but decodes VVC/HEVC on a
Vulkan device and presents through libplacebo on the same device, with frame
pacing set to its lowest-latency mode (no wait before the decoded frame is
submitted). Its logged stream settings are the interesting part:

| Setting | Reference client | Ours before this round |
|---|---|---|
| Source fps | 60 | **30** |
| Bitrate | 25 Mbps | **100 Mbps** |
| Frame buffer | reorder window, in-order frames pass through | 0 |
| Present pacing | submit on decode, no vblank gate | waited on the swapchain first |

Two of those were ours to lose. At 30 fps the console itself holds a frame
before it encodes (and the client can only display a new picture every 33 ms);
requesting 100 Mbps makes the VBV/link queues larger than a 25 Mbps stream.
Round 5's queue fix removed the client-side frame age, but the 30 fps request
kept roughly two source intervals on the console side, which is exactly the
"playing in the past" difference against a 60 fps client.

Changes:

- The stream defaults to and recommends 60 fps. The setting stays available for
  compatibility, but the UI now states that 30 makes the console hold a frame.
  30 fps games should keep 60 on the wire (repeated frames are detected and
  skipped) and use frame generation for cadence.
- The profile was migrated to 60 fps / 50 Mbps / buffer 0 / vsync off.
- The render loop no longer waits on the swapchain's frame-latency object
  before pulling a decoded frame. It wakes on the frame itself, draws it, and
  presents; `Present()` still throttles when the flip queue is full. A frame that
  arrived mid-refresh no longer waits for the next vblank to even be picked up.
- Only drawn frames are presented. A repeated frame or an idle decoder leaves
  the last image on screen instead of flipping in an untouched backbuffer.
- The probe now prints `present q N lag X ms`, derived from
  `DXGI_FRAME_STATISTICS` (`PresentRefreshCount`, `SyncRefreshCount`,
  `SyncQPCTime`) with the refresh period learned from consecutive vblanks. That
  is ground truth for how many refreshes the flip queue adds.

What we deliberately did not copy: the reference client's Vulkan/libplacebo
renderer and its per-frame Java object churn. Our D3D11 video-processor path is
already zero-copy when AI is off; the latency was in pacing and in the stream
request, not in the GPU API.
