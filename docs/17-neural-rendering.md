# 17 - Neural pipeline: DLSS only

RTX Video Super Resolution (VSR) was **removed** from the client. Its runtime
files (`nvngx_vsr.dll`, `nvVFXVideoSuperRes.dll`) are no longer loaded or
packaged. The neural paths are now:

```
stream (1080p RGBA)
  -> DLSS Super Resolution / DLAA (D3D12, optional)
  -> [GPU grade + CPU readback of the SR output, when FG is enabled]
  -> DLSS Frame Generation worker (2x-6x, presents its own window)
  -> display
```

Without frame generation the SR output goes straight to the parent presenter.
With frame generation the SR output (render resolution, e.g. 2560x1440) is read
back and fed to the NeuralScreen worker, exactly like a game feeding DLSS-G at
its render resolution. The worker generates and presents the final frames; the
parent's window is covered while it does. Measured with SR Ultra Quality +
FG 2x at 2560x1440: **120 fps displayed**, source cadence 4 refreshes. The
parent's source tick costs ~5-6 ms (upload + SR submit + readback + worker
submit) but the worker is on its own thread and is not blocked by it. The
readback is now also the colour/sharpening pass (`docs/27`), so the graded
pixels are exactly what the worker presents.

DLSS NR can still run inside the worker (`SetNeuralBypass` is off when NR is
enabled). NR + FG 2x costs roughly 7-18 fps of output (120 -> 102-113 measured);
NR off is the default for a stable 120.

## DLSS Super Resolution / DLAA

- Input is the 1080p stream (or a downscaled 33/50/58/66/77% of the target when
  the mode asks for fewer pixels). DLAA runs at the frame's own size with the
  AI anti-aliasing network.
- Presets K/L/M map to the NGX model preset hints. Sharpness is 0.3.
- There are no depth or motion-vector buffers in a Remote Play stream, so the
  feature is driven with a constant zero jitter and `Reset=1` per frame. It is
  used as a high-quality spatial upscaler/AA pass, not as a temporal
  reconstruction; this is honest and avoids smearing.
- The status line reports the real processing time; a `0.0 ms` reading while DLSS
  is selected means the feature did not run.

### Asynchronous submit

The D3D12 evaluation is submitted and the call returns the **previous** output;
the next source frame waits on the shared fence, which by then has had a full
display interval to finish. On the reference machine the synchronous path
blocked the presentation thread for **10-15 ms per source frame** (25% of ticks
were late). With the two-slot asynchronous path the loop stays at
`p99 4.44 ms`, `late 0`, `stalls 0`, and the source keeps its four-refresh
cadence. The cost is one source frame (~16 ms) of extra latency.

## DLSS Neural Rendering (NR)

The NGX worker (`nr/native/nvngx.dll`) applies the NR model at the stream size
or the upscaled size, depending on the pipeline order:

- `Upstream: NR -> SR` runs NR on the source, then DLSS SR reads its result.
- `NVIDIA order: SR -> NR` runs DLSS SR first and NR on the upscaled frame (needs
  SR and a target no larger than 1440p).

Controls: render scale (network size), passes, style, strength, local tone,
structure, skin, character mask and UI correction. These are the worker's
options, not emulated.

## Frame generation

- **DLSS Frame Generation**: the NeuralScreen worker presents generated frames in
  its own child window. It uses flat-depth guides (no true depth/motion vectors)
  and sustains ~100-120 generated fps at 1080p on the reference machine.
  Enabling DLSS NR inside the same worker costs roughly 15-18 fps of output
  (120 -> ~102-113 measured), so NR + FG 2x is available but not the stable
  120 fps path; NR off is the default for 240 Hz displays.
- **NVIDIA VFG (legacy)**: the NVIDIA Video Effects frame-generation runtime.
  It runs on the presentation thread and is kept only as a comparison path.

## Runtime requirements

- `nvngx_dlss.dll` next to the executable for DLSS SR.
- `nr/native/` for the NeuralScreen worker and its FG/DLSSG runtimes.
- `third_party/nvidia-vfx/runtime/` only for the legacy VFG path (optional).

Without these the client still streams using the regular upscalers; the UI
reports the missing component instead of failing.
