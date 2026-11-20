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