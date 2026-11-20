# Release audit - BetterRemotePlay Next 1.0

Audit target: the `src/next`, `src/media` and `src/session` paths used by
`brpnext.exe`, plus the Win32 shell and overlay. This is an engineering audit,
not a marketing report. Items are only called fixed when the change was built
and, where possible, exercised on the reference PS5.

## Critical issues fixed

| Issue | Fix |
| --- | --- |
| RIFE worker thread could be joinable at destruction (std::terminate on exit after using RIFE) | `Run()` now stops RIFE before DLSS-G and destroys the presenter; `~NextApp` is defensive; leaving Watch and device-loss also join the thread |
| DLSS SR left a recorded D3D12 command list after a failed EvaluateFeature | the allocator/list are reset on the failure path; the next frame starts clean |
| Unchecked D3D12 queue/allocator/list creation | each HRESULT is validated; `Submit` aborts safely and resets the list on Close/Signal failure |

## High-priority issues fixed

- **Inert settings removed from the UI.** Codec, HDR stream/output, source
  resolution, VRR, audio volume/mute, controller enabled/haptics/adaptive/
  lightbar, GPU fp16/int8/buffers/priority, monitor/refresh/bit-depth/colour
  output, start-minimized/close-to-tray/update-check and experimental toggles
  were persisted but never consumed. They are no longer offered as working
  controls. The settings model keeps the fields for profile compatibility.
- **Streaming settings now reach the session.** Bitrate, decoder, frame buffer
  and source fps are copied into the session profile before `Start`; the source
  is documented as 1080p HEVC (the console cap).
- **V-Sync and sharpening are applied.** Watch presentation uses
  `PresentNow(settings.video.vsync)`; sharpening goes through
  `Presenter::SetEnhance`.
- **DLSS-G multiplier is clamped.** Without the SM86 proxy the effective
  multiplier is 2x; the proxy is injected only when the file exists and the
  request is above 2x.
- **SR 2160p cannot silently burn a 4K readback before a capped DLSS-G worker.**
  The FG path refuses to run SR 2160p (unless DLAA forces native size) and
  feeds the stream frame instead.
- **RIFE failure stops the stage**, blend is applied to the generated midpoint
  and motion safety skips midpoints on very large frame changes. RIFE is only
  used below 45 fps.
- **Overlay settings work.** Widget visibility filters the Overview rows,
  scale/opacity/position/interval are applied, and the interactive control
  center uses the same settings transaction.
- **Overlay is foreground-scoped** so it does not stay clickable over another
  application after Alt-Tab; it follows the app's monitor.
- **Shutdown order fixed**: transport is disconnected and audio detached before
  the audio engine is closed; the audio pointer is atomic.
- **Settings/profile replacement restores the backup** if the rename fails.

## Medium issues addressed

- Removed stale "Stage N", "migration" and "reference app" copy from the release
  UI.
- Diagnostics now labels the panel "UI loop timing" and shows the real stream
  decode/cadence line instead of promising zero-valued stage timings.
- About is a first-class page; the duplicate Settings category was removed.
- Overlay window style changes are cached, avoiding a framechanged call on every
  250 ms update.
- Home no longer shows a fabricated "READY" warning pill while offline.

## Remaining known issues

See `KNOWN_ISSUES.md` and `KNOWN_LIMITATIONS.md`. The important ones are the
closed DLSS-G worker's flat-depth quality, the blocking connect-thread join,
per-process Winsock initialization, HDR presentation, and hardware validation
that cannot be performed on this machine.
