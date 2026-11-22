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

## 1.0.5 - once-per-second hitch

- The worker-log poll (1 Hz file I/O) no longer runs on the presentation or
  generator-feed thread.
- The DLSS-G worker's stderr goes to NUL by default, removing its per-second
  disk write; `BRP_FG_LOG=1` restores the log.
- GPU/monitor probing no longer runs during Watch.

## 1.0.6 - dedicated stream window

- Streaming now uses its own top-level window and presenter. The main
  application window minimizes while playing and returns on exit.
- The stream surface is video-only: no UI composition, no menu swapchain.
- DLSS SR rebuilds on the stream device; DLSS-G and the overlay follow the
  stream window. F10/F11/Esc and Alt+F4 are handled there.

## 1.0.7 - 30 fps cadence

- Frame pacing is phase-locked to the display and the decoded queue keeps one
  spare frame. At 30 fps / 240 Hz every frame is now held exactly 8 refreshes
  (measured `holds 8:30` per second) instead of a 7-9-7-9 wobble.
- `Settings > Streaming > Frame buffer` is back: Off (lowest latency) /
  1 / 2 / 3 frames. Default and the current profile use 1 frame.
- The DLSS-G feed uses the same clock; 30 fps + 2x measured 59.9-60.2 fps.
- Diagnostics shows the dominant hold, for example `holds 8v 100%`.

## 1.0.8 - SR/FG tuning for 30 fps

- At 30 fps with a frame buffer, DLSS SR evaluates synchronously and returns
  the current frame, removing the extra source interval the async path added.
  The 8-refresh hold cadence is unchanged.
- 4x DLSS-G at 1080p measured 92-94 fps on a 240 Hz panel, so 2x (60 fps) is
  the stable choice for 30 fps games; DLAA is the recommended generator input.
- Diagnostics keeps showing the dominant hold (`holds 8v 100%`).

## 1.0.9 - control center, Esc and input

- F10 works: system keys (F10/F11/F12) are read from WM_SYSKEYDOWN. The control
  center had never opened before.
- Esc arms disconnect with a visible banner and persists across Alt-Tab; a
  global Esc hook catches it even when the worker window has focus. Second Esc
  disconnects; F10 cancels; Alt+F4 leaves Watch.
- Controller poll rate up to 1000 Hz, live.
- Settings shows only General/Audio/Controller/Advanced; the other pages own
  their controls.
- The overlay is a side-docked panel on the chosen side, with Stream, Audio and
  Pad tabs and restart badges for session-only changes.

## 1.2.0 - stable

- DLSS-G frames are submitted on an exact source-rate grid with a due time;
  the worker no longer inherits UI loop jitter.
- F10 opens the side control center (system-key fix), Esc arms persistently
  and works after Alt-Tab through a foreground-only global hook.
- Controller poll rate up to 1000 Hz, live.
- Settings is reduced to General/Audio/Controller/Advanced; the other pages
  own their controls.
- The overlay is a side-docked panel with Stream/Audio/Pad tabs and restart
  badges; the NVIDIA App overlay is hidden while streaming.

## 2.0.0

- New Rust/GPUI shell (same platform family as the next Discord client), with
  a top-tab layout, Basic/Advanced switch, Inter + JetBrains Mono at larger
  sizes, animated aurora background, boot splash and a state-driven connect
  animation.
- The C++ engine now runs headless (`--engine --bridge-port`) and is driven
  over loopback TCP; the stream window, audio, input, DLSS and the in-stream
  overlay are unchanged.
- The shell minimizes while streaming, removing its animated surface from DWM
  composition, and the engine skips its hidden Direct2D pass when headless.
- Live telemetry now includes frame cadence p50/p99, scanout, present misses
  and shown fps.
- DLSS 5 is documented as unavailable for a 2D stream; Neural Render is the
  real depth + optical-flow path.

## 2.1.0

- The F10 control center is now a GPUI window (same theme, tabs, live data),
  with DLSS-G paused while it is open so it cannot steal focus.
- Live audio/monitor/GPU pickers from the engine; no indexes to remember.
- Settings apply automatically ~350 ms after the last edit; text size scales
  the whole type ramp.
- F10 works even when the DLSS-G presenter owns the foreground; Esc never arms
  a disconnect while the panel is open.
- Corrupt settings files are quarantined, never overwritten by defaults.
- Advanced shows a Pacing card (loop p99, cadence, scanout, present misses).

## 2.2.0

- Saved profiles are back: list, apply and save from Home or the in-stream
  control center, backed by the engine's profile store.
- Ctrl+1..8 switches tabs, Ctrl+Enter connects; tab labels are text-only so no
  machine falls back to tofu glyphs.
- The shell reloads settings changed by the engine or the overlay immediately.

## 2.3.0

- Only one shell can run: a second launch focuses the existing window.
- Panics are written to the shell log.
- Audio/monitor/GPU lists refresh while a picker is open.
- The control center opens on the display used for streaming.

## 2.4.0

- DLSS 5 Neural Rendering is back and real: the NGX feature-18 worker runs on
  the 2D stream with synthetic depth/motion (9.4 ms at 1080p), before DLSS SR.
- Neural page: style, intensity, passes, work scale, UI correction, auto mask;
  live status in Settings and in the F10 control center.
- HDR frames bypass the 8-bit worker; the worker stops on Leave Watch and on
  neural setting changes.

## 2.4.1

- Frosted-glass composition in the shell: translucent content sheet, soft
  gaussian card/top-bar shadows, softer aurora. GPUI has no true backdrop blur;
  this is a layered approximation, documented as such.