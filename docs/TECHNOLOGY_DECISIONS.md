# Technology Decisions (ADR log)

## ADR-001 - Keep C++20 / Win32 / Direct2D / D3D11 as the product stack

**Status:** accepted.

**Context.** The streaming core, decoder, session protocol, DualSense HID and
every GPU path already exist in C++ with measured latency. A managed rewrite
(.NET/WPF/WinUI) would need P/Invoke or C++/CLI for chiaki, FFmpeg, NGX,
TensorRT and DXGI; the UI would be pleasant but the latency-critical path
would gain a boundary that the brief explicitly does not want.

**Decision.** New application is C++20, CMake, Win32 window, Direct2D UI and
the existing D3D11/D3D12 media path. The "new product" is a new executable and
new modules, not a new language.

**Consequences.** Faster to a working product, no interop tax, reuse of
verified code. Cost: UI productivity is lower than XAML; mitigated by a
component library and design tokens.

## ADR-002 - Reuse chiaki-ng session code behind `IStreamingService`

**Status:** accepted.

**Context.** PS5 discovery, PSN auth, PIN pairing, session request and the
chiaki handshake are hard-won and verified on two machines. The brief forbids
rewriting protocol functionality without understanding it.

**Decision.** Wrap the existing `session/` code in a service. Do not fork it
or reimplement the protocol. Replace the app-level glue, not the protocol.

**Consequences.** Low risk, preserves compatibility. The service must expose
snapshots/events so the new UI never sees chiaki types.

## ADR-003 - New executable `brpnext.exe`; old `brp.exe` remains the reference

**Status:** accepted.

**Context.** A big-bang replacement of the 7,000-line `App` would make the
product unusable during development and impossible to compare.

**Decision.** Build the new shell in parallel, sharing the media/session
libraries. Ship both until the feature matrix is green, then remove the old
exe from the package.

**Consequences.** Clear rollback, honest comparison, no regression in the
working app. Cost: temporary double packaging and duplicated UI code during
migration.

## ADR-004 - Typed settings with schema version, not the legacy flat map

**Status:** accepted.

**Context.** `session.json` is an untyped flat map parsed by hand in
`profile.cpp`. Every screen reads raw keys; validation is ad hoc.

**Decision.** `AppSettings` is a typed model with defaults, `Validate()`,
versioned JSON, atomic write, `.bak`, and a one-way import of the legacy file.
Profiles store a full settings snapshot.

**Consequences.** Safe defaults and migrations, testable validation. The old
file stays readable by the old app during migration.

## ADR-005 - AI backends publish capabilities; no invented modes

**Status:** accepted.

**Context.** The DLSS-G runtime behind the worker refuses multipliers above
2x (`0xBAD00005`); RIFE is 30 -> 60; TensorRT engines are GPU-specific. The
old UI let the user ask for 120 from 30 fps with NVIDIA, which could only
produce 60.

**Decision.** `AiCapabilities` is queried before a backend is selectable. The
UI shows actual max output and the reason when a mode is unavailable. The
backend list only contains installed/validated engines.

**Consequences.** Some combinations simply disappear from the UI. That is
correct: no fake 4x, no fake 4K source detail, no VSR.

## ADR-006 - DLSS SR is the primary neural SR; TensorRT SR is optional

**Status:** accepted.

**Context.** The brief asks for an independent TensorRT SR engine and excludes
VSR. The existing DLSS SR is a real neural reconstructor, faster and cleaner
than a CPU-round-trip Real-ESRGAN at 1440p/4K, and it already runs with frame
generation.

**Decision.** Ship DLSS SR as the primary SR backend. Add a TensorRT SR
backend only as an explicitly labelled optional/experimental backend for
machines where DLSS SR is unavailable, with measured cost. Never use VSR.

**Consequences.** The product meets the visual goal without a slower custom
model. The brief's wording is respected by documenting the divergence rather
than pretending a TensorRT SR exists.

## ADR-007 - Probes plus a real unit-test target, no test framework dependency

**Status:** accepted.

**Context.** The repo has 20+ hardware probe executables, no unit tests. CMake
+ MSVC is the only build system; adding GoogleTest is possible but unnecessary
for the first foundation.

**Decision.** Keep the probes for hardware. Add `brp_next_tests`, a console
executable with deterministic assertions for config, profiles, navigation,
metrics and scheduling. It runs in CI or locally with exit code 0/1.

**Consequences.** Tests are fast and dependency-free. Later, if needed,
GoogleTest can replace the harness without changing the tested code.

## ADR-008 - Independent overlay window, not part of the video swapchain

**Status:** accepted.

**Context.** The DLSS-G worker owns its presenter window; the old in-stream
sheet is part of the app. Coupling the overlay to the video swapchain risks
pacing and composition bugs.

**Decision.** The overlay is a top-level layered click-through window with its
own D2D surface and a data provider. It can be hidden without touching video
presentation.

**Consequences.** Clean separation, but two windows must be positioned and
focus-managed. Already proven by the 0.4.9 worker overlay fix.

## ADR-009 - Windows 11 x64 + NVIDIA only

**Status:** accepted.

**Context.** The brief targets RTX 4090/Windows 11. Cross-platform support
would dilute the GPU path and the session core.

**Decision.** No portability abstraction beyond what already exists. Detect
non-NVIDIA and degrade to the existing non-neural upscalers, but do not
promise feature parity.

**Consequences.** Simpler code, honest system requirements.
