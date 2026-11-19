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
- `brp_next_tests`: 79 deterministic assertions, exit code 0/1.
- CMake targets; no changes to the existing `brp` target.

Acceptance (met):

- `cmake --build build-perf --target brp_next_tests` succeeds.
- `brp_next_tests` prints `ALL PASS` (79 assertions).
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

## Stage 4 - Streaming service (next)

- `IStreamingService` wrapper over chiaki-ng; discovery, PSN, pairing,
  connect/disconnect, state snapshots.
- Console page with real discovery/auth/pairing/status.
- Diagnostics-only decoded-frame preview at native quality (no AI).
- Old app still the only way to stream until parity tests pass.

Acceptance: connect, stream, audio, controller and reconnect on the reference
PS5; no protocol edits.

## Stage 5 - Media and presentation

- `IVideoDecoder` wrapper, frame pool, HDR metadata.
- `IPresenter` wrapper with waitable pacing, borderless/windowed, F11.
- Streaming page with live input/output/network/audio/controller sections.
- Display page with real modes only.

Acceptance: native 1080p60 and 30 fps streams, audio sync, HDR on/off,
240 Hz pacing, same latency envelope as the reference.

## Stage 6 - AI Video Engine

- `IAiEngine` + `ISuperResolution` + `IFrameGenerator` + `IOpticalFlow`.
- DLSS SR first, then RIFE, then DLSS-G with capability reporting.
- AI page driven by capabilities; no unsupported mode visible.
- Simultaneous SR + FG, with the 2x ceiling explicit.
- Optional TensorRT SR backend only if measured and labelled.

Acceptance: 1080p30 -> 1440p60, 1080p60 -> 1440p120, SR + FG with 0
drops/late/stalls in a 10-minute run; RIFE 30 -> 60 parity; worker logs
clean.

## Stage 7 - Overlay and diagnostics

- Independent layered overlay window with widgets and layouts.
- Diagnostics page: live series, p50/p95/p99, GPU/VRAM/temp, decode/SR/FG/
  present timings, queue depths.
- Benchmark runner and JSON/CSV report.

Acceptance: overlay does not change cadence within noise; diagnostics match
`testing.log`; report reproducible.

## Stage 8 - Settings and profiles completion

- Full settings tree, reset-to-default, import/export, migration from 0.4.9.
- Profiles with compatibility validation and default profile.
- Change notifications and restart-required indicators.
- Notifications center with deduplication.

Acceptance: every setting maps to a real field and effect; invalid
combinations are blocked with a reason.

## Stage 9 - Integration and benchmark matrix

- Automated 1080p30/60 -> 1440p/2160p runs, p50/p95/p99, drops, VRAM.
- Fix regressions and document honest limits (4K source detail, 2x FG).
- Compare old and new executables on the same content.

Acceptance: all four cases measured; no uncontrolled latency; results stored
in docs.

## Stage 10 - Stability

- Multi-hour soak, network interruption, device loss, model failure, VRAM
  pressure, resolution change, shutdown while streaming.
- RTX 5080 second-machine pass.

Acceptance: recovery without restart where possible; no leaked GPU resources;
old app unaffected.

## Stage 11 - Release

- Package both executables during migration; then retire the old one.
- Single `update.exe`, versioning, model/engine deployment, notices/licenses.
- Final feature matrix, known issues, system requirements.

Acceptance: clean install on a machine without the dev tree, update from
0.4.9, rollback documented.

## Current position

- Stage 0: done (0.4.9).
- Stage 1: done (five documents).
- Stage 2: done (foundation library, AppHost, theme tokens, 79 tests).
- Stage 3: shell working (`brpnext.exe`), with streaming/AI execution and the
  independent overlay deferred to Stages 4-7. No page claims a connection it
  does not have.
- Stage 4 onward: not started.

## Guardrails

- Never break `brp.exe` while building `brpnext.exe`.
- Never add VSR.
- Never expose 4x/6x DLSS-G or 30 -> 120 on this runtime.
- Never populate statistics with constants.
- Every new module compiles independently and has an owner interface.
