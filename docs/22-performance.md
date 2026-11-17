# Frame pacing, presentation and diagnostics

The client targets the monitor's refresh rate (240 Hz on the reference
machine) but the console still sends 60 fps. Two independent jobs follow from
that:

1. make every 60 fps source frame hold for the same number of refreshes, and
2. keep the UI/video submission itself off the critical path.

## Presentation loop

`Presenter` creates its flip-model swapchain with
`DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT` and
`SetMaximumFrameLatency(1)`. The main loop waits on that handle before it builds
the next frame, so rendering is phase-locked to the display's vblank instead of
racing a software timer. `Present(1)` is the limiter; the old
`CreateWaitableTimer` pacing is only a fallback for drivers that reject the
waitable swapchain (the log line reports `waitable=on/off`).

`timeBeginPeriod(1)` is held for the life of the process. It also benefits the
1 ms waits used by the audio and stream threads.

## Video cadence

`Streaming::TakeFrame` accepts an optional `pace_interval`. When the frame
buffer is 1 or higher (the default), decoded 60 fps frames are delivered on a
fixed 1/60 s grid. A frame that arrives early waits for its slot; if the queue
built up, the newest frame wins so pacing never adds more than one interval of
latency. Frame buffer `0` keeps the old lowest-latency behaviour: whatever
decoded most recently is shown immediately.

DLSS/NVIDIA frame generation bypasses the grid: those paths must be fed as soon
as a frame exists, and the generator owns the output cadence.

## Decode is off the receive thread

Hardware decode (`av_hwframe_transfer_data`) plus swscale costs several
milliseconds per frame. It now runs on a dedicated decode thread; chiaki's RTP
receive callback only copies the compressed access unit into a bounded queue.
A slow frame can no longer stall socket reads and turn into packet loss.

The Opus resampler is created once per session instead of once per 10 ms packet.

## UI cost

- DirectWrite text layouts are cached by text, format and box. Static labels no
  longer build a layout (and its internal glyph run) every frame.
- Linear/radial gradient brushes are cached by colour pair and moved with
  `SetStartPoint`/`SetEndPoint`/`SetCenter`.
- Rounded-clip geometries are cached, and the D2D layer is bounded to the widget
  instead of `InfiniteRect()` (which forced a full-screen intermediate surface
  for every tile).
- The stream HUD rebuilds its strings at 10 Hz and draws cached layouts.
- Home/Settings no longer clear the backbuffer before painting their own opaque
  background.

## Measuring

Run with `BRP_FRAME_PROBE=1`:

```powershell
$env:BRP_FRAME_PROBE = "1"
.\build\Release\brp.exe
```

Every 480 iterations it prints the loop interval percentiles and the CPU work
time (`App::Frame` up to, but excluding, the blocking Present):

```
[frame] 240 Hz  interval p50 4.17  p99 4.19  max 4.31 ms  |
        work p50 0.82  p99 1.63  max 3.90 ms  |  late 0  stalls 0 (of 480)
```

- `interval` is the real presentation cadence. At 240 Hz it should sit at
  4.17 ms with a `max` close to 8.3 ms.
- `work` is how much of that budget the CPU (UI + video submission) used.
  Spikes here that stay below 4.17 ms are harmless; spikes above it drop a
  refresh.
- `late` counts intervals over 1.5x the budget, `stalls` over 2.5x.

The in-stream HUD shows the same frame percentiles plus `late`/`stalls` for the
last 240 samples. `BRP_LOOP_PROBE=1` still prints the average fps every two
seconds.

## What still costs time

- **DLSS NR after DLSS SR** (`NR after SR` order) performs a GPU readback of the
  upscaled frame, copies it to the NGX worker, and copies the result back. That
  is a bandwidth and latency path, not a rendering bug; the HUD reports the
  worker time.
- **NVIDIA VFG** runs `NvVFX_Run` on the presentation thread. It is the legacy
  generation path; DLSS Frame Generation runs in its own worker window and does
  not block the UI.
- **LAN jitter** is visible as `queue`/`drops` changes. Increase the frame
  buffer one step if `drops` keeps climbing with a high `p99` decode time.
