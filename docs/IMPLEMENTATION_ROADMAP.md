# Implementation Roadmap - BetterRemotePlay Next

The rebuild is staged. A stage is "done" only when its acceptance checks pass
on the reference machine and the old executable still works. No stage may be
represented as complete from a UI mockup.

## Stage 0 - Baseline lock (done)

- Freeze reference at 0.4.9; record hashes in `docs/29-handover.md`.
- Keep the existing probes and packaging runnable.

Acceptance: `brp.exe` 0.4.9 starts, streams, NVIDIA 60/120 verified.

## Stage 1 - Discovery and documents (done in this increment)

Deliverables: `REPOSITORY_ANALYSIS.md`, `ARCHITECTURE.md`,
`TECHNOLOGY_DECISIONS.md`, `FEATURE_MATRIX.md`, this file.

Acceptance: every claim in the documents is traceable to the working tree or
a measured log. No invented SDKs or features.

## Stage 2 - Foundation library (done in this increment)

Deliverables:

- `brp_core_next`: interfaces, models, enums, `Result`, snapshots.
- `brp_config_next`: typed `AppSettings`, validation, JSON store, profiles,
  import/export, schema migration.
- `brp_diag_next`: logger, rolling metrics (p50/p95/p99), benchmark report
  types.
- `brp_app_next` (navigation + host): page model, grouping, collapsed state,
  history, service registry, bootstrap.
- `ui/theme`: 15 colour tokens, spacing, radii and type scale for dark/light.
- `brp_next_tests`: 83 deterministic assertions, exit code 0/1.
- CMake targets; no changes to the existing `brp` target.

Acceptance (met):

- `cmake --build build-perf --target brp_next_tests` succeeds.
- `brp_next_tests` prints `ALL PASS` (83 assertions).
- Config round-trips, invalid values are rejected, legacy import works,
  profiles create/duplicate/default/export/import/delete, navigation is
  stable, AppHost wires the services.
- The reference `brp` target still builds unchanged.

## Stage 3 - New UI shell (working preview)

Deliverables present:

- `brpnext.exe`: Win32 + D2D shell with the new dark/light design tokens.
- Persistent sidebar (9 pages, collapsed mode), custom title bar, toasts.
- Home with real OS/CPU/GPU/display facts and active configuration.
- Settings hub with 11 categories, typed controls and immediate save.
- Profiles page: create from current, apply, set default, delete, export and
  import (JSON file dialogs).
- Diagnostics page: real system facts, UI frame p50/p95/p99, report writer.
- AI Video Engine page: capability checks for DLSS SR, DLSS-G worker, RIFE
  engines and TensorRT runtime; unavailable backends are labelled missing.
- Display/Overlay/Console/Streaming pages expose real settings and honest
  stage notes where execution is not wired.

Not yet in this stage:

- The streaming service and media pipeline (Stage 4/5). Console/Streaming
  pages do not connect yet and say so.
- AI execution (Stage 6). The AI page persists choices and shows capabilities,
  but does not run SR/FG.
- The independent overlay window (Stage 7); the Overlay page is a layout
  preview driven by saved values.

Acceptance for the shell: window opens/resizes, navigation and theme work,
settings survive restart, profiles and diagnostics work, no fake connected
state. Verified manually on the reference 4090; `brp.exe` still builds and
runs unchanged.

## Stage 4 - Streaming service (working preview)

Deliverables present:

- `StreamingService` (`brp_next_streaming`) wraps the real chiaki-ng session,
  profile, discovery, PSN auth and pairing code. No protocol rewrite.
- Loads the legacy `session.json` once into
  `%APPDATA%\BetterRemotePlayNext\session.json`, so the old app is untouched.
- Asynchronous discovery (UDP), PSN browser login + clipboard redirect import,
  8-digit PIN pairing, wake-on-LAN/rest-mode wake, TCP reachability check and
  a background session start.
- Console page driven by real state: idle/connecting/streaming/failed,
  credentials, address, discovery results, PSN account, PIN field, connect and
  disconnect, live source fps/decode/bitrate/loss.
- Graceful disconnect sends the session teardown (RST) on normal exit.

Measured on the reference PS5: session request, streaminfo, 1080p60 profile,
decode ~8.1 ms, loss 0.00 %, source cadence ~20-30 fps on the menu. Video
presentation is Stage 5; frames are decoded and drained for stats, not shown.

Not yet: the media pipeline and presenter (Stage 5), controller/audio binding
in the new app (Stage 5), AI execution (Stage 6).

## Stage 5 - Media and presentation (working)

- `brpnext` opens the real decoder and presents decoded frames through the
  D3D11 presenter in a full-window Watch view (Esc returns to Console).
- Audio engine is opened on Connect and attached to the session; the DualSense
  input runs on a 250 Hz worker (buttons/sticks/triggers) and console
  feedback/haptics are written back through the existing HID path.
- Console page shows live source fps, decode, bitrate, loss, audio device and
  controller presence.
- DLSS SR is applied per decoded frame inside the Watch path (see Stage 6).
- Verified on the PS5: 1920x1080, decode ~7.9-8.8 ms, loss 0.00 %, 28-30 fps
  menu content, full-window video with the overlay.

Not yet: HDR toggle in the new presenter and measured A/V sync under load.
Borderless fullscreen is implemented: `F11` toggles it and Watch enters it
automatically when `Display > Fullscreen on play` is set.

## Stage 6 - AI Video Engine (DLSS SR + RIFE 30 -> 60 + DLSS-G working)

Working:

- DLSS Super Resolution: 1920x1080 -> 2560x1440, NGX preset L, quality from
  the typed AI settings; log line `[dlss] SR initialized 1920x1080 -> 2560x1440`.
- Capability checks for DLSS SR, the DLSS-G worker, RIFE lite/heavy and the
  TensorRT runtime; unavailable backends are labelled missing.
- AI settings are persisted; SR can be changed live and reinitializes.
- RIFE 30 -> 60 pair scheduler is working in `brpnext`: real frame at the
  source cadence, one TensorRT midpoint per pair, paced display queue;
  measured **29 generated/s and 58 shown/s** from a 28.8 fps source.
  Overlay: `RIFE | 29 gen/s | 58 shown/s`.

- DLSS-G 60 -> 120 through the NVIDIA worker (`NvDlssnr` present mode):
  `DlssFg` owns the worker, feeds it at the source cadence and fits the
  worker's topmost window over the client. Verified in `brpnext`:
  `[fg] displayed 120.0 FPS (real + generated, 2x)` at 1080p, with the source
  at 57-58 fps. `Esc` stops the worker and returns to the console page.
- DLSS SR + DLSS-G: the SR output is read back to RGBA and fed to the worker.
  DLAA (input == output, 1080p) is the low-latency path; a 1440p SR output
  feeds a 2560x1440 worker and reaches about **101 generated fps** (worker
  limit at 1440p, not a claimed 120). The generator consumes the newest
  decoded frame and the parent swapchain is not presented while it covers the
  screen.
- The multiplier is selectable 2x-6x when `fg/winmm.dll` (SM86 proxy) is
  installed; without it only 2x is enabled. The UI says 3x-6x land near
  150-165 generated fps and the overlay reads the worker's own log, so the real
  rate is never replaced by the requested one.

Not claimed: simultaneous SR + RIFE, and 30 -> 120 (the runtime inserts at most
one midpoint for a 30 fps source).

## Stage 7 - Overlay and diagnostics (working, basic)

- Independent layered top-level overlay (`OverlayWindow`, GDI, click-through,
  no focus): stats lines, opacity, four positions, refresh interval.
- It never enters the video swapchain; updating it does not touch cadence.
- Diagnostics page has real UI frame p50/p95/p99, system facts and a report
  writer.
- The overlay shows source resolution/fps, decode, bitrate, loss, DLSS SR state
  and the Esc hint.

Not yet: drag/rearrange widgets, per-widget toggles beyond the current lines,
GPU temperature/power series, and the automated benchmark report.

## Stage 8 - Settings and profiles (working)

- 11 categories, typed controls, atomic save, reset to defaults.
- Profiles: create from current, apply, default, delete, import/export JSON,
  and a compatibility warning when a profile needs a missing backend.
- Notifications via toasts with error/success colours.
- Legacy `session.json` and settings import; separate next-app data folder.

Not yet: automatic profile selection by console/game, notification history
center, and full restart-required classification.
## Stage 9 - Integration and benchmark matrix (working)

- Diagnostics page can write a real `session-report.json` (source fps, decode,
  bitrate, loss, RIFE gen/s, shown/s, DLSS state, UI p50/p95/p99).
- `Run benchmark` (or `BRP_NEXT_BENCH=1`) runs the four baseline cases and
  writes `benchmark-<epoch>.json`:
  30/60 requested -> 1440p/2160p, with 20 s between console sessions.
- Measured on the reference PS5 menu (4090, 1440p240 display):
  - 30 -> 1440: source 26.5, shown 26.5, decode 7.9 ms, loss 0.
  - 30 -> 2160: source 27.2, shown 27.2, decode 7.9 ms, loss 0.
  - 60 -> 1440: source 54.1, shown 54.1, decode 8.2 ms, loss 0.
  - 60 -> 2160: source 53.6, shown 53.6, decode 7.9 ms, loss 0.
- RIFE 30 -> 60 was measured separately: 29 gen/s, 58 shown/s.
- Limits: the source is the console menu, so requested 60 yields ~54 unique
  fps; 2160p renders are fitted to the 1440p display. A true 4K display and a
  60 fps game are still needed for final 4K/120 validation.

## Stage 10 - Stability (partial)

- Automatic reconnect after an unexpected end: up to 5 attempts, configurable
  delay, only after a user-initiated session; explicit Disconnect disables it.
- GPU device-loss detection stops the Watch view and tears the session down.
- Graceful shutdown sends the session teardown, closes audio, joins the pad
  and RIFE workers.
- RIFE load failure falls back to the raw/SR path instead of crashing.
- Not yet: multi-hour soak, forced network interruption, VRAM pressure, and the
  RTX 5080 second-machine pass.

## Stage 11 - Release (packaged, migration still dual-exe)

- 0.5.0 package contains both `brp.exe` and `brpnext.exe`.
- Single `update.exe` (1.29 MB) from 0.4.9; verified `files_differ=0`.
- Both executables launch from the clean package.
- Model/engine deployment unchanged: the package ships the lite engine and
  both ONNX models; TensorRT/heavy engines are built on demand by brp.exe.
- Not done: retiring brp.exe, signing, and a public release channel.

## Current position

- Stage 0: done (0.4.9).
- Stage 1: done (five documents).
- Stage 2: done (foundation library, AppHost, theme tokens, 83 tests).
- Stage 3: shell working (`brpnext.exe`), with streaming/AI execution and the
  independent overlay deferred to Stages 4-7. No page claims a connection it
  does not have.
- Stage 4: streaming service working — discovery, PSN, pairing, wake, connect
  and live session stats. Video presentation is Stage 5.
- Stage 5 onward: not started.

## Guardrails

- Never break `brp.exe` while building `brpnext.exe`.
- Never add VSR.
- Never claim 30 -> 120. 3x-6x DLSS-G is offered only with the SM86 proxy and the overlay reports the worker record rate, not the requested one.
- Never populate statistics with constants.
- Every new module compiles independently and has an owner interface.

## Current position update (after Stages 5-8)

- Stage 5: video, audio and controller are live in `brpnext`; DLSS SR is
  applied to the decoded 1080p frame. Borderless fullscreen follows the
  monitor (F11, or automatically on Watch).
- Stage 6: DLSS SR, RIFE 30 -> 60 and DLSS-G 60 -> 120 are working in
  `brpnext`. DLSS-G uses the NVIDIA worker presenter; `Esc` stops it.
- Stage 7: independent layered overlay and diagnostics are working.
- Stage 8: settings, profiles, reset, import/export and compatibility warnings
  are working.
