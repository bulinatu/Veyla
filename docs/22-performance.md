# Frame pacing, presentation and diagnostics

The client targets the monitor's refresh rate (240 Hz on the reference
machine) while the console sends 60 fps, and DLSS Frame Generation runs in a
separate worker window. Three clocks have to agree: the display's vblank, the
60 fps source, and the frame generator. This document records what was
measured, what was wrong, and how it is paced now.

## Presentation loop

`Presenter` creates its flip-model swapchain with
`DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT` and
`SetMaximumFrameLatency(1)`. The main loop waits on that handle before it builds
the next frame, so rendering is phase-locked to the display instead of racing a
software timer.

`ResizeBuffers` validates its flags against the ones used at creation. The
waitable flag is part of that set: passing only `ALLOW_TEARING` made every
resize/fullscreen transition fail with `E_INVALIDARG`. The presenter now stores
`swapchain_flags_`, passes them back, and if a driver still rejects them it
recreates the swapchain at the new size. Fullscreen and window resizing are
part of the normal path again.

When the DLSS FG worker covers the client, the parent has nothing to present.
It skips `Present` (no stale swapchain in DWM composition) and the loop is paced
by the high-resolution waitable timer at the refresh rate instead of waiting on
a vblank that will never arrive.

`timeBeginPeriod(1)` is held for the life of the process.

## Video cadence

The source is 60 fps, so on 240 Hz each frame must hold exactly four refreshes.
The presentation loop advances a rational phase by `source_fps / refresh_hz`
every tick and takes a frame only when the phase crosses one. At 240/60 this is
every fourth tick, with no wall-clock drift and no "early" release that would
produce a three-refresh frame followed by a five-refresh frame.

`Streaming::TakeFrame(..., paced=true)` pops the oldest queued frame and keeps at
most two frames (one spare). The spare absorbs network jitter; a deeper queue
only adds latency. Frame buffer `0` keeps the old lowest-latency behaviour:
whatever decoded most recently is shown immediately.

When the FG worker covers the client the swapchain tick is not a valid clock
(the parent is not presenting), so the same gate uses a 60 Hz wall clock to keep
the worker fed.

DLSS/NVIDIA VFG bypass the gate: the generator owns the output cadence and must
be fed as soon as frames exist. The legacy VFG path has its own generated queue.

## Reflex

Setting NVAPI's driver low-latency mode changed the present cadence even with
correct SIMULATION/RENDER/PRESENT markers: the waitable chain settled at
~225 Hz and DXGI reported **56+ missed refreshes per second**. `NvAPI_D3D_Sleep`
without markers added **~4 ms/frame**; with markers it still slept into the next
vblank because our simulation window is not a game-style render loop. Both are
worse than the latency they could save on a remote stream, so the mode is
rejected and the setting stays off. The markers are implemented but unused.

The low-latency path in this client is `SetMaximumFrameLatency(1)` plus the
vblank waitable, which gives one queued frame and no extra sleep stage.

## DLSS Frame Generation is the limiter, not the display

The NeuralScreen worker reports how many frames it actually displays:

```
[fg] displayed 150.6 FPS (real + generated, 4x)
[fg] displayed 140.9 FPS (real + generated, 3x)
[fg] displayed 120.1 FPS (real + generated, 2x)
[fg] displayed 165.0 FPS (real + generated, 6x)
```

At 1080p with flat-depth guides the worker sustains about **100-120 generated
fps**. Asking for 4x/6x does not produce 240/360 fps: it produces a fluctuating
150-165 fps, which is the worst possible cadence on a 240 Hz panel. **2x is the
only clean setting**: 120 fps is exactly two refreshes per frame. The
Performance tab reads the worker's own log once per second and shows the real
number, in amber when it is below the requested target. 3x-6x remain selectable
but are labelled as landing between refreshes. FG Lite (`Preset=C`) was tested
and did not raise the ceiling.

A rewrite of this component would mean owning the FG presenter and feeding it
real depth/motion vectors; the closed worker's flat-depth heuristic is the
ceiling today.

## UI cost

- DirectWrite text layouts are cached by text, format and box.
- Linear/radial gradient brushes are cached by colour pair.
- Rounded-clip geometries are cached and D2D layers are bounded to the widget
  instead of `InfiniteRect()`.
- The stream HUD rebuilds strings at 10 Hz.
- Home/Settings do not clear the backbuffer before their opaque background.
- HID enumeration moved to the permanent pad thread; it blocked the
  presentation thread for ~60 ms every two seconds when no controller was
  attached.

## Measuring

`BRP_FRAME_PROBE=1` prints two lines every two seconds:

```
[frame] 240 Hz  interval p50 4.16  p99 4.41  max 5.69 ms |
        work p50 0.31  p99 0.48  max 1.98 ms | late 0 stalls 0 (of 480)

[video] src 60.0 fps  select 4v 100% | screen 4v 100% (n=120, stats=yes) |
        arrival 16.7 ms outliers 2 | present misses +0 (total 14) |
        render 0.01 ui 0.13 src 0.00 ms
```

- `interval`: real presentation cadence. At 240 Hz it should sit at 4.17 ms.
- `work`: CPU time inside `App::Frame`, excluding the blocking Present.
- `select`: how many refreshes each source frame was *selected* for.
- `screen`: the same measured from DXGI's refresh counter. This is the number
  the eye sees; 4v should be ~100% in fullscreen.
- `present misses`: presents whose refresh count advanced by more than the
  present count.
- `arrival`: decoded frame interval and how many were outside 13.5-20 ms.
- `render/ui/src`: time split inside the frame, useful to separate the video
  pass, Direct2D and the source upload.

The in-stream HUD shows the frame percentiles, `late`/`stalls`, source cadence,
FG output rate, and decode/queue counters.

## What still costs time

- **DLSS NR after DLSS SR** performs a GPU readback, copies 8 MB to the worker
  and back. That is a bandwidth and latency path, not a rendering bug.
- **NVIDIA VFG** runs `NvVFX_Run` on the presentation thread. It is the legacy
  path; DLSS FG runs in its own worker.
- **LAN jitter** appears as `queue`/`drops` and `arrival` outliers. Raise the
  frame buffer one step if drops climb with a high p99 decode time.
- **Windowed DWM composition** is measurably less even than borderless
  fullscreen (about 90% four-refresh holds vs 100%). Fullscreen (F11) is the
  reference path.
