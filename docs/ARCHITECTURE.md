# BetterRemotePlay Next 1.0 - Architecture

## Runtime layers

```
src/next/shell        Win32 shell, pages, input, Watch presentation, overlay host
src/next/overlay      independent layered window + control-center view model
src/next/app          AppHost, service registry, navigation, settings classification
src/next/streaming    StreamingService over the chiaki-ng session core
src/next/config       typed settings, flat JSON, profiles, migrations
src/next/core         types, models, interfaces, no Windows dependencies
src/next/diagnostics  logger, rolling metrics, reports
src/next/platform     window, gamepad, system probe
src/media             presenter, decoder, DLSS SR, DLSS-G worker client, RIFE
src/session           chiaki session, discovery, pairing, PSN, profile
```

`brpnext` links `brp_next_core`, `brp_next_streaming`, `brp_ui` (presenter,
renderer), `brp_media` (NGX/worker), and `brp_rife` (TensorRT, loaded at run
time). `brp.exe` keeps its own pipeline and is still packaged alongside it.

## Settings transaction

1. A page or the overlay mutates `AppSettings`.
2. `NextApp::Apply` calls `AppHost::ApplySettings`, which validates and saves.
3. `ClassifySettingsChange` labels the change as immediate, frame-boundary,
   pipeline, stream restart or application restart.
4. `NextApp` reconfigures only the affected components:
   - AI stages: DLSS SR feature shutdown / DLSS-G worker restart / RIFE thread.
   - presenter target and sharpening: applied on the same session.
   - transport, codec, decoder: a session restart is required and reported.
5. The overlay and every page read the same `AppSettings` and the same live
   `StreamSnapshot`; there is no second configuration store.

The overlay never touches GPU resources. `BuildOverlayView` produces pure data
(`OverlayView`/`OverlayRow`) and `HandleOverlayAction` routes the resulting
`OverlayAction` through the same transaction as the main window.

## Video path

```
network -> chiaki decode (D3D11VA/D3D12VA/...) -> DecodedFrame queue
        -> optional DLSS SR (1080p -> 1080p DLAA or 1440p/2160p)
        -> optional DLSS-G worker (RGBA, present window) or RIFE midpoint
        -> presenter (window / borderless fullscreen)
```

- `TakeNewestFrame` feeds the generator immediately and drops stale queued
  frames.
- With DLSS-G active the presenter does not present the parent surface; the
  worker window is the picture.
- With RIFE, the midpoint queue is paced by the display and failure disables the
  stage instead of looping silently.

## Threading

- UI/presenter/SR: UI thread only.
- Decoder: session decode thread; frames guarded by one mutex.
- DLSS-G: one `DlssFg` worker thread owning `NvDlssnr`; one-slot newest-frame
  queue.
- RIFE: one worker thread owning the TensorRT engine.
- Pad: permanent 250 Hz thread.
- Shutdown joins RIFE and DLSS-G before destroying the presenter or audio.

## Independent overlay

Compact mode is a click-through GDI stats panel. `F10` opens the control center,
which removes `WS_EX_TRANSPARENT`, accepts mouse input, and forwards keyboard
input from the shell. Tabs: Overview, Super resolution, Frame generation,
Display, Performance, Profile. The panel follows the monitor of the app window.

## Known architectural limits

- The DLSS-G worker is a closed process; it owns its presenter and receives
  flat depth + estimated optical flow. Real engine motion vectors do not exist
  for a decoded video stream.
- The presenter is D3D11; DLSS SR is the D3D12 NGX feature bridged through a
  shared texture.
- HDR presentation, monitor mode switching and per-adapter selection are not
  implemented in 1.0 and are not exposed as working controls.
