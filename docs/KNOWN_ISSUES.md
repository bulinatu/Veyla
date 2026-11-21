# Known issues - 1.0

These are the defects still open in 1.0. They are not hidden behind the UI.

## High impact

1. **Connecting/disconnecting can block the UI thread.**
   `StreamingService::JoinConnect` waits for the wake/connect worker, which can
   spend up to eight 2-second wake attempts. Closing the app or pressing
   Disconnect during "Waking console..." can freeze for several seconds.
   Workaround: wait for the wake attempt to finish before closing.
2. **Winsock is started and cleaned up per probe.**
   `HostReachable`, discovery and wake each call `WSAStartup`/`WSACleanup`.
   Concurrent discovery and connection can race the refcount. A process-wide
   initialization is required.
3. **HDR is not implemented.** The presenter is created SDR; there is no
   runtime HDR swapchain recreation or tone-map control. HDR controls were
   removed from the UI.
4. **Monitor/mode switching and per-adapter GPU selection are not implemented.**
   Fullscreen covers the monitor that contains the window. Inference uses the
   adapter owned by the process.
5. **`DlssFg::Stop()` can wait up to 4 s** if the worker is inside its pipe
   timeout. Pipeline changes from the overlay therefore block the message
   pump briefly.

## Medium impact

- The DLSS-G worker presents in its **own top-level window**, so its output
  cadence is owned by that process. At 2x from 30 fps (60 fps out) it measures
  59.9-60.1 fps here; rates that do not divide the monitor refresh (for example
  90 fps on a 240 Hz panel) cannot be made perfectly even by the parent app.
  The parent path itself now measures 0 present misses/s.
- DLSS-G uses the closed NeuralScreen worker with **flat depth and estimated
  optical flow**. Video has no engine motion vectors; fast parallax, HUD edges
  and disocclusions can look "painted". This is a model limitation, not frame
  pacing (phase telemetry shows 0 late/superseded frames).
- A 30 fps source reaches a clean 60 with 2x. 4x at 30 fps measured ~94 fps with
  48-53 late generated frames, so it is not advertised as smooth.
- RIFE is only used below 45 fps, at 1080p; 60 -> 120 RIFE is not possible with
  the 4.25 Lite engine at this latency budget.
- Profile delete has no confirmation; rename/duplicate are not exposed.
- The benchmark has no cancel button and can run while no console is reachable.
- The session-report JSON writer interpolates strings without full JSON
  escaping.
- `WSA`/connect cancellation, per-monitor refresh detection and the 1-slot
  keyboard queue are still pragmatic, not fully event-driven.

## Low impact

- The overlay creates GDI fonts per paint (4 Hz); this is not measurable in the
  stream but is avoidable.
- `RenderWatchOverlay` is dead code.
- Some legacy settings fields (`ai.quality`, `ai.occlusion`, `ai.adaptive`,
  `gpu.*`, `display.vsync/vrr/bit_depth`) remain in the model so old profiles
  load without data loss, but they are no longer exposed.
- The compact overlay uses a black colour key; ClearType edges can show a dark
  fringe over very bright scenes.
