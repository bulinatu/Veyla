# Architecture - BetterRemotePlay Next

## 1. Decision in one paragraph

BetterRemotePlay Next is a new C++20 application (`brpnext.exe`) built in the
same repository, next to the working `brp.exe`. It keeps the proven
chiaki-ng session core, FFmpeg decoder, D3D11 presenter, DualSense HID and the
NVIDIA/ TensorRT backends, but places them behind interfaces and gives them a
new application shell, a token-based UI, typed configuration, real profiles,
a diagnostics service and an independent overlay. The old `App` class is not
reused; its behaviour is the reference. The old executable stays runnable
until the new one reaches feature parity, so no protocol work is risked.

## 2. Module map

```
brpnext.exe
  └─ brp_app_next        application shell, DI container, navigation, lifecycle
       ├─ brp_ui_next    views, components, theme tokens, layout
       ├─ brp_overlay_next  independent overlay windows/widgets
       ├─ brp_diag_next  logging, metrics, benchmark reports
       ├─ brp_config_next typed settings, profiles, migration, import/export
       ├─ brp_ai_next    SR/FG/OF backends and capability discovery
       ├─ brp_present_next frame scheduler, swapchain, display detection
       ├─ brp_media_next decoder, colour, frame buffers (re-exports brp_media)
       ├─ brp_stream_next chiaki session, auth, transport (wraps brp_session)
       └─ brp_core_next  interfaces, models, events, Result, no OS/UI deps
```

Rules:

- `brp_core_next` includes no Windows, D3D, FFmpeg or UI headers.
- `brp_ui_next` never calls inference or the PS5 protocol; it talks to view
  models, which talk to service interfaces.
- `brp_ai_next` never includes `brp_ui_next`.
- `brp_stream_next` never includes `brp_ui_next` or `brp_ai_next`.
- Settings are represented by `brp_config_next` models; screens bind to them,
  they do not own raw fields.
- Every service has a narrow interface in `brp_core_next`; the DI container in
  `brp_app_next` binds one implementation per interface.
- Asynchronous work is submitted to owned executors with bounded queues and
  cancellation; no service calls `Sleep` on the UI thread.

## 3. Layer responsibilities

### Core
`Result`, `Error`, IDs, enums (`Page`, `Theme`, `Resolution`, `FrameRate`,
`AiBackend`, `QualityPreset`), service interfaces, immutable snapshots
(`StreamSnapshot`, `GpuSnapshot`, `AiSnapshot`, `DiagnosticsSnapshot`),
`Notification`, `IClock`.

### Configuration
`GeneralSettings`, `ConsoleSettings`, `StreamingSettings`, `VideoSettings`,
`AiSettings`, `GpuSettings`, `DisplaySettings`, `OverlaySettings`,
`AudioSettings`, `ControllerSettings`, `AdvancedSettings`, `AppSettings`.
Validation clamps every value and reports incompatible combinations.
Persistence is versioned JSON with atomic replace, backup and migration from
the legacy `session.json`. Named `Profile` objects store a full `AppSettings`
plus metadata; import/export is a single JSON document.

### Streaming
`IStreamingService` exposes `Connect`, `Disconnect`, `State`, `Snapshot` and
events. The implementation wraps the existing chiaki session, discovery,
pairing and PSN code. Media consumption is separated: the service delivers
decoded frames through `IFrameSink` rather than owning the renderer.

### Media
`IVideoDecoder`, `IAudioSink`, frame pool, colour conversion, HDR metadata.
Wraps FFmpeg and keeps hardware decode selection.

### AI
`IAiEngine` with capability discovery and independent sub-interfaces:
`ISuperResolution`, `IFrameGenerator`, `IOpticalFlow`, `ITemporalFilter`.
Each backend reports `Capabilities` (max multiplier, supported resolutions,
precision, estimated cost) before it is selectable. DLSS SR/FG, RIFE and an
optional TensorRT SR backend implement these interfaces. VSR is absent.

### Presentation
`IPresenter`, `IFrameScheduler`, `IDisplayService`. Waitable swapchain,
borderless/windowed, HDR, VRR detection, per-frame due times. The scheduler
accepts original and generated frames with explicit `kind` and `timestamp`;
generated frames are never presented as originals.

### UI
A retained-ish layer over Direct2D: `Theme` tokens, `Component` primitives
(button, toggle, dropdown, slider, status, tooltip, modal), `View` + `ViewModel`
pairs, `Navigator` with a persistent sidebar and collapsed mode, and pages:
Home, Console, Streaming, AI Video Engine, Display, Overlay, Profiles,
Diagnostics, Settings. Theme switching (dark default, light) is a token swap,
not a second style sheet per page.

### Overlay
A separate top-level layered window with its own D3D/D2D surface and a data
provider (`IOverlayData`) fed by the diagnostics/metrics service. Widgets are
independent drawables with enable/position/opacity/layout persisted in
`OverlaySettings`. It does not enter the video presenter's swapchain.

### Diagnostics
Thread-safe logger with rotation, metrics with rolling p50/p95/p99, benchmark
runner and report writer (JSON + human-readable). The benchmark runner drives
the real pipeline, not a simulation.

## 4. Threading model

| Thread | Owns | Never does |
| --- | --- | --- |
| UI/present | window, D2D, navigation, swapchain wait | inference, network, blocking I/O |
| Network/session | chiaki receive/send, auth | UI or GPU calls |
| Decode | FFmpeg/NVDEC, frame pool | UI |
| AI submission | one per backend, bounded queue | UI |
| Diagnostics | file I/O, metric snapshots | UI |
| Audio | WASAPI/AVRT | UI |

Queues are bounded; the newest frame wins only where latency demands it
(frame generation input), otherwise order is preserved. Cancellation is a
`stop_token`-style flag checked at queue boundaries.

## 5. Configuration and profiles

- `next-settings.json` (schema version 1) in `%APPDATA%\BetterRemotePlayNext`.
- `next-profiles.json` for named profiles.
- On first run, a one-way import of the legacy `session.json` maps known keys
  and leaves the old file untouched.
- Every write is temp-file + `MoveFileEx(REPLACE_EXISTING)` with a `.bak`.
- `Validate()` returns warnings (unsupported resolution, missing model,
  unavailable backend) and errors (invalid enum, impossible refresh rate).
- Applying a profile validates against `IHardwareCapabilities`; unsupported
  fields are disabled with an explicit message, never silently ignored.

## 6. AI capability model

Each backend publishes:

```cpp
struct AiCapabilities {
    bool available;
    std::string reason;       // why not, when false
    int max_multiplier;       // 2 for this DLSS-G runtime, 2 for RIFE
    std::vector<Resolution> outputs;
    bool fp16;
    bool int8;
    bool temporal;
    double estimated_ms;
};
```

The UI only lists backends whose `available` is true. Selecting an unavailable
backend is impossible; the reason is shown. The DLSS-G 2x ceiling is a
property of the backend, not a hidden app cap.

## 7. Migration plan (old -> new)

1. Build `brpnext.exe` alongside `brp.exe`; both share `brp_media`,
   `brp_session` and runtime folders.
2. Implement the new shell and Settings/Profiles/Diagnostics first, reading
   and writing only the new config. No streaming yet.
3. Wrap streaming behind `IStreamingService`; connect/disconnect and a
   diagnostics-only preview (no AI) must work before any AI UI is enabled.
4. Add decode/presenter and the Home/Streaming pages at native quality.
5. Port DLSS SR, then RIFE, then DLSS-G, each behind `IAiEngine` with
   capability checks.
6. Port the overlay as an independent window.
7. Only when the new app passes the feature matrix may `brp.exe` be removed
   from the package.

Until step 7, packaging ships both executables and a single `update.exe`.

## 8. Explicit non-goals

- Rewriting chiaki-ng or the PSN auth flow.
- RTX VSR, in any form.
- Claiming 4x/6x DLSS-G on this runtime; the worker refuses it.
- A UI that exposes settings without an implementation behind them.
- A cross-platform abstraction; the product is Windows 11 x64 with NVIDIA.
