# 17 - Neural pipeline: DLSS only

RTX Video Super Resolution (VSR) was **removed** from the client. Its runtime
files (`nvngx_vsr.dll`, `nvVFXVideoSuperRes.dll`) are no longer loaded or
packaged. The neural paths are now:

```
stream (1080p RGBA)
  -> DLSS Super Resolution / DLAA (D3D12, optional)
  -> DLSS Neural Rendering (NGX worker, optional)
  -> presenter (1440p/2160p target, or the window)
```

`DLSS Frame Generation` is a separate output path: the NeuralScreen worker owns
the presented window and generates 2x-6x frames. It takes the same RGBA stream
and can also run DLSS NR internally (`SetNeuralBypass` is off when NR is
enabled). DLSS SR/DLAA and DLSS FG cannot be the final stage at the same time:
selecting DLSS SR turns FG off, and selecting FG turns SR off. The Performance
tab shows the worker's **actual** displayed rate; on the reference 4090 it is
about 120 fps at 2x, and 3x-6x land between refreshes (see
`docs/22-performance.md`).

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
