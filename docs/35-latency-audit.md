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
