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

