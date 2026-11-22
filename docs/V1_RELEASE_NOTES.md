# BetterRemotePlay Next 1.0 - release notes

## Highlights

- First stable release of `brpnext.exe`, packaged with the reference `brp.exe`.
- Complete UI pass: new title bar, grouped sidebar, page headers, pipeline flow,
  dedicated About page, redesigned Home, Profiles presets and AI sub-tabs.
- Interactive overlay control center: Overview, SR, FG, Display, Performance and
  Profile tabs, mouse + keyboard, live application through the settings
  transaction, compact click-through stats mode.
- Runtime settings classification: immediate, frame-boundary, pipeline
  reconfiguration, stream restart and application restart.
- DLSS SR (including true 1080p DLAA), DLSS-G 2x through the NVIDIA-signed
  runtime, optional SM86 proxy for 3x-6x, and RIFE 30 -> 60 with blend/motion
  safety.
- Latency work: newest-frame feed, no parent swapchain while the generator
  covers the screen, event-driven frame wake, high-priority worker GPU queues,
  and 1 ms-free input gating.
- Borderless fullscreen with F11 and fullscreen-on-play, exact window restore.
- Settings/profile replacement now restores the previous file on failure.
- Removed dead UI controls that were not connected to an engine.

## Verified in this session

- `brp_next_tests`: ALL PASS (82 assertions).
- PS5 session request, decode, audio/controller binding, Watch and disconnect.
- DLSS SR 1080p -> 1440p; DLAA 1080p -> 1080p.
- DLSS-G 2x displayed 60 fps from a 30 fps source (119-120 fps on 60 fps
  sources in earlier runs), 0 late/superseded generated frames.
- Overlay control center opens with F10, switches tabs, toggles SR live and
  reports `Processing pipeline rebuilt`.
- Borderless fullscreen and restore; double-Esc disconnect.
- Package update 0.4.9 -> 1.0.0: `files_differ=0`, 0 markers.

## Not verified here

- HDR display, 4K/120 game content, secondary-monitor fullscreen, multi-hour
  soak, RTX 5080.
- DLSS-G 3x-6x quality; measured only that the proxy raises the ceiling.

See `KNOWN_LIMITATIONS.md` and `KNOWN_ISSUES.md`.

## 1.0.1 - latency fix

- Newest decoded frame every tick, no phase gate, no presentation buffer, no
  redundant re-upload.
- Controller thread uses MMCSS, highest priority and a 4 ms high-resolution
  waitable timer, matching the reference app.
- Watch presents V-Synced so the loop cannot spin and starve streaming threads.
- Update from 1.0.0 is a 0.73 MB single-file patch; `files_differ=0`.

## 1.0.2 - upscale / FG / streaming polish

- DLSS SR quality presets now drive real render-scale percentages
  (33/50/58/66/77/100), with a CPU area-downscale before NGX and the effective
  scale shown in the AI page and overlay.
- DLAA stays 100% native.
- Live snapshots report the actual decoder; Home uses it.
- Frame generation remains the 2x official runtime with the SM86 proxy optional
  for 3x-6x, fed from the newest frame, capped at 1440p.

## 1.0.3 - smoothness fix

- Watch now presents exactly once per vblank. The previous 8 ms waitable
  timeout ran the loop twice per refresh on a 240 Hz display; the extra pass
  presented a backbuffer that was never drawn, which the flip chain shows as
  the frame from two presents ago. That was the visible "frame skip".
- The standing frame is redrawn on every tick, including ticks with no new
  decoded frame, so both swapchain buffers always contain the newest image.
- The CPU area downscale used for DLSS render scales was rewritten: 15.9 ms ->
  4.0 ms for 1080p -> 1688x950.
- Presentation and decode threads run above normal priority.
- DLSS-G + SR skips a GPU fence wait when feeding the worker.
- Diagnostics shows measured scanout, present misses and loop p99.

## 1.0.4 - latency and in-order delivery

- Removed the one-source-frame delay that 1.0.3 added to the DLSS-G + SR path
  (the "previous completed slot" readback).
- Video frames are delivered in order and only dropped above a bounded
  backlog, so a network burst is played out instead of skipped.
- The parent path pulls on a rational display phase (fps / refresh), keeping
  content changes on an even refresh grid without a wall-clock timer.
- The rigid generator feed grid tested in development made the worker slower
  and is not shipped.