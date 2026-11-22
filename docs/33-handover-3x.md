# 33 - Handover: BetterRemotePlay Next 3.x

Status: forward handover. 3.x does not exist yet; this document defines what it
must contain, what it inherits from 2.4.5, and how to build, test and release it
without regressing the parts that already work.

Audience: whoever picks up the codebase next, human or agent. Read this top to
bottom once, then use the checklists.

---

## 1. Baseline: where 3.x starts

| Item | Value |
|---|---|
| Product | BetterRemotePlay Next **2.4.5** |
| Commit | `83897d1` (release), previous majors `87499b8` 2.1.0, `af7fd26` 2.0.0 |
| Shell | `brpnext.exe` - Rust 1.100 + GPUI 0.2.2, GUI subsystem |
| Engine | `brpnext-engine.exe` - C++20, Win32, D3D11/D3D12, headless |
| Legacy | `brpnext-classic.exe` (2.x C++ shell), `brp.exe` (reference, untouched) |
| Settings | `%APPDATA%\BetterRemotePlayNext\next-settings.json`, flat dotted keys, schema 1 |
| Bridge | TCP loopback, port chosen by the shell, `--engine --bridge-port N` |
| Tests | `brp_next_tests`: **82 / 82 ALL PASS** |
| Standalone | `dist/BetterRemotePlay-2.4.5-standalone.zip` (922.0 MB) |
| SHA256 | `397FC38700A78C383C7DD22AB276E62914DC85B9995460173C2DED57B2163F18` |
| Update 2.4.4 -> 2.4.5 | `dist/BetterRemotePlay-2.4.4-to-2.4.5-update.exe` (5.3 MB) |
| Update SHA256 | `476707D7D36196323808F64E08F972425EAFCD1D5216DC2D0811B63ABA4F7175` |
| Shell SHA256 | `84771956BBB27AA186DF0F395E91616A5746290D1D83686E4DA063E171747A18` |
| Engine SHA256 | `7EBB2E6C1C46E1ADA45AE0EFDAE38F002BED3ED17F156AB6A3AD435573DEA8B6` |
| Classic SHA256 | `FA8562B81BAF2BD2CD420462A9431FA2B654B0527F8736FA0FD855A814997F36` |

What works today, and must not regress:

- PS5 streaming through chiaki-ng, 30/60/120 fps, H.264/HEVC/AV1, D3D11VA/D3D12VA.
- DLSS SR / DLAA (NGX, 2D mode with flat guides), DLSS-G 2x through the
  NeuralScreen worker, RIFE 30->60, DLSS 5 NR in video mode (~9-10 ms at 1080p).
- F10 opens a GPUI control center (Info/SR/FG/Video/Stream/Audio/Pad/Profile),
  Esc arms/disconnects, Alt+F4 leaves, 1000 Hz pad, side-docked compact card.
- Profiles (engine store), live audio/monitor/GPU pickers, Ctrl+1..8 tabs,
  auto-apply ~350 ms, text size 90-125%, settings quarantine on parse errors.
- Frame pacing: 30 fps on 240 Hz holds 8 refreshes/frame, present misses 0,
  UI loop p99 5.5-6.0 ms, feed p99 35.6-35.8 ms, worker 59.9-60.0 fps.
- Packaging, patch generation and patch verification (0 markers, 0 diffs).

---

## 2. What 3.x is for

2.x proved the platform: new shell, real streaming, honest AI. 3.x is about
finishing the architecture and the product, not adding surface area.

Definition of done for **3.0.0**:

1. One installable product with a stable contract between UI and engine.
2. No fake controls anywhere; every visible setting is read by the engine.
3. Pacing and latency measured and guarded by a regression budget.
4. Neural rendering validated on HDR and at 60 fps, or explicitly disabled there
   with a reason in the UI.
5. Release engineering that a stranger can run from a clean checkout.
6. Localization and onboarding good enough to hand to a non-technical user.

Non-goals / hard constraints:

- No Sony keys, client identities or game-library scraping.
- DLSS-G stays a closed worker: we cannot phase-lock its presenter.
- A video stream has no G-buffer; no feature may claim full 3D-guided DLSS 5.
- Do not break `brp.exe`, `%APPDATA%\BetterRemotePlay`, or the legacy profile.
- Do not regress Esc/F10, the settings JSON, or the 8:30 cadence.

---

## 3. Architecture

### 3.1 Current shape (keep for 3.0)

```
brpnext.exe (Rust/GPUI, GUI)
  |  TCP 127.0.0.1:<ephemeral>  line protocol
  v
brpnext-engine.exe --engine --bridge-port N   (C++, WIN32 subsystem)
  |- chiaki-ng session thread -> decode queue (D3D11/D3D12VA)
  |- presentation loop (own thread, vblank-aware)
  |    -> [DLSS 5 NR] -> [DLSS SR] -> [parent presenter | DLSS-G worker | RIFE]
  |- NeuralScreen workers (NR process; DLSS-G process, topmost presenter)
  |- WASAPI audio, DualSense HID, overlay compact card (GDI), global Esc/F10 hook
  `- settings file + profiles dir (shared with the shell)
```

The shell owns: main window, animations, settings UI, GPUI control center,
device pickers, profiles, localization, updates.
The engine owns: everything real-time. It never draws the settings UI in engine
mode.

### 3.2 Decision to make for 3.1: process split vs in-process

Options:

A. **Keep the split (recommended for 3.0).** Loopback bridge, crash isolation,
   independent restarts, no ABI risk. Cost: two processes, one extra hop of
   latency (microseconds), IPC code to maintain.
B. **Static-link the engine into the shell.** One process, no IPC, but the
   engine's blocking connect thread, GPU device loss and worker failures can
   take the UI down; the build becomes a CMake-inside-Cargo nightmare.
C. **Engine as a DLL (C ABI).** Clean contract, one process, but the engine
   currently owns windows and a message pump; exporting it is a substantial
   refactor with real pacing risk.

3.0 ships A. 3.1 evaluates C behind a feature flag only if there is a measured
latency or UX reason; otherwise stay on A.

### 3.3 Bridge protocol (frozen for 3.x, additive only)

Shell -> engine, one UTF-8 line per command, max 160 bytes:

```
connect | watch | stop_watch | disconnect | apply | devices | profiles
profile_apply <id> | profile_save <name> | profile_delete <id>
overlay_open | overlay_close | leave_watch | disconnect_now
fullscreen | shutdown | ping
```

Engine -> shell:

```
S {json}                      status snapshot, ~4 Hz
E overlay-toggle              F10 routed from the engine/worker
E {"kind":"devices",...}      audio endpoints, monitors, DXGI adapters
E {"kind":"profiles",...}     id, name, default
```

Rules:

- `S` snapshots are replaceable; `E` events are queued (the outbox is a deque,
  newest 16 kept). Never put an event in a replaceable slot.
- Unknown lines are ignored by both sides. New fields are additive.
- Status keys currently include: state, connected, streaming, connecting, armed,
  message, console_name, codec, width, height, fps, bitrate, loss, jitter,
  decode_ms, decode_max, loop_p99, drops, cadence_p50, cadence_p99, scanout_hz,
  present_misses, shown_fps, restart_pending, sr_ms, fg_ms, generated, cuda,
  tensorrt, dlss_fg_ready, dlss_sr_ready, nr_runtime, nr_ready, nr_ms, gpu,
  driver, vram_mb.

### 3.4 Pacing contract

- The shell must never render while streaming; it minimizes itself and stops
  its animation. Do not add a second composited surface behind the stream.
- The engine must present only through the active presenter, and must not
  present the parent swapchain while the DLSS-G worker covers the screen.
- Opening the control center pauses DLSS-G submit and hides the worker; on close
  the worker resumes. This is not optional, it protects focus and pacing.
- Frame path: decode -> optional NR -> optional SR -> FG -> present. NR is
  before SR so SR can use the enhanced frame.

---

## 4. Workstreams for 3.x

### A. Frame pacing and latency

- [ ] Add a pacing regression gate to the test suite: parse a recorded probe log
      and fail if holds differ from 8:30 at 30 fps or present misses > 0.
- [ ] Add a UI pacing graph (sparkline of loop p99, cadence, misses), not just
      numbers. Keep it in Advanced/Perf and in the control center.
- [ ] Evaluate a RIFE-owned 120 Hz path as the smoothness alternative to DLSS-G:
      phase-locked queue, measured judder, A/B instructions in the UI.
- [ ] Adaptive frame buffer: only if measurements show it helps; never change
      `streaming.frame_buffer` silently.
- [ ] Audit the 29.97 vs 30.00 clock path once more; any submit pacer must use
      the measured source interval, never the nominal fps.
- Acceptance: 30 fps base = 8:30 holds, 0 misses, loop p99 < 6 ms over 10 min;
  DLSS-G feed p99 <= 36 ms; no freeze after 30 min.

### B. Neural rendering (DLSS 5 NR)

- [ ] Ship a real model/guide story or keep video mode. Current runtime is the
      vendor NGX feature 18 with synthetic depth/motion; effect is subtle.
- [ ] HDR: NR is 8-bit and bypassed for PQ frames. Either add a 10-bit path or
      keep the UI explicit that HDR uses SR/FG only.
- [ ] 60 fps: NR costs ~10 ms; measure with SR async and decide whether to allow
      it, warn, or auto-disable above 30 fps.
- [ ] Model licensing: if a monocular depth model is added, use a permissive
      licence (MiDaS/Depth Anything Small) and document weights provenance.
      Do not ship NVIDIA DLLs; keep the user-supplied runtime convention.
- [ ] Runtime installer: a signed check + copy for `nr/native` and `rife/`, with
      hashes and a rollback.
- Acceptance: NR on/off measurable, no frame drops at 30 fps, status live in both
  the page and the control center, no claim of 3D-guided NR.

### C. Control center and UI

- [ ] Localization: RO/EN at minimum. Strings in a resource file, no literals in
      layout code. Keep ASCII-only sources with escapes.
- [ ] Compact stats in the GPUI panel (currently the engine's GDI card): one
      renderer, one font stack, click-through option.
- [ ] Profile editor: rename/duplicate/delete/import/export; the bridge already
      has list/apply/save/delete.
- [ ] Settings search, "changed from default" markers, and a reset per section.
- [ ] Tray: close-to-tray with a real icon and menu, or remove the idea.
- [ ] Accessibility: focus ring, keyboard-only navigation, hit targets >= 32 px,
      contrast >= 4.5:1 for body text.
- Acceptance: every page reachable by keyboard; no control without an effect;
  no mojibake; 100% of strings localized.

### D. Robustness and diagnostics

- [ ] Replace the blocking connect-thread join with a cancellable connect.
- [ ] Per-probe Winsock init cleanup.
- [ ] Crash reporting: keep the panic hook, add an engine minidump on device
      loss and a "Report" button that zips logs + settings (no credentials).
- [ ] Log viewer in the shell (tail `ui.log`, `next.log`, `bridge.log`).
- [ ] Config schema migration: versioned migrations, not ad-hoc defaults.
- [ ] Single-instance per user session is done; verify across fast user switch.
- Acceptance: killing the engine leaves the shell alive with a clear state and a
  Restart engine action; no silent config reset.

### E. Release engineering

- [ ] CI: Windows runner, CMake + Rust, fxc from the Windows SDK, run 82 tests,
      build release, package, generate update from the previous version, verify.
- [ ] Reproducible build notes: toolchain versions, SDK path, Rust target.
- [ ] Code signing for `brpnext.exe`, `brpnext-engine.exe`, the updater and the
      installer; document the certificate flow.
- [ ] Installer (Inno Setup or MSIX) with per-user install and uninstall that
      keeps `%APPDATA%`.
- [ ] Update channel: at least a manifest URL and a manual "Check now"; the
      current shell only has the patch executable.
- [ ] Keep `tools/package_release.ps1` and `tools/make_update.ps1` as the source
      of truth; the installer wraps them, it does not replace them.
- Acceptance: a clean VM can install, stream, update and uninstall without
  editing files by hand.

---

## 5. Known issues and technical debt inherited from 2.x

1. DLSS-G 2x on 240 Hz has a small phase wobble from the closed worker.
2. Game library is not shown (PSN scope/product decision).
3. FG without SR is refused by the worker in overlay mode.
4. HDR through FG is SDR (RGBA8 only).
5. NR costs 7-18 fps at 2x and is 8-bit.
6. Bluetooth trigger effects depend on console packets.
7. Bitrate/chroma quality not swept (50/75/100 Mbps).
8. No true backdrop blur in GPUI; the shell uses a frosted composition.
9. GPUI 0.2.2 lacks topmost/set_bounds/no-activate; workarounds use Win32 on the
   HWND (title lookup, AttachThreadInput, SetWindowPos).
10. Many `dead_code` warnings in the shell; harmless but noisy.
11. `MapAlias`, `Status::is_live`, `Engine::online`, several theme constants are
    unused.
12. The classic C++ shell still exposes the old controls (including the ones the
    new shell removed as inert). It is a fallback, documented as such.
13. First run triggers a Windows Firewall prompt for `brpnext-engine.exe`
    (inbound UDP for Remote Play). The bridge itself is loopback and never
    prompts.

---

## 6. Metrics and how to reproduce

Environment probes (engine, stderr):

- `BRP_NEXT_FRAME_PROBE=1` - loop/cadence/holds/decode/present misses.
- `BRP_FG_LOG=1` - `nr-worker.log` from the DLSS-G worker.
- `BRP_OVERLAY_PROBE=1` - overlay focus state.
- `BRP_BRIDGE_LOG=1` - `bridge.log` for the engine side.
- `BRP_ENGINE` - override the engine path in development.
- `BRP_UI_AUTOCONNECT=1` - connect on launch (tests only).
- `BRP_UI_PICKER=audio|monitor|gpu` - open a picker on launch (tests only).
- `BRP_UI_TAB=home|stream|...` - initial tab (tests only).

Budgets:

| Metric | Budget |
|---|---|
| 30 fps base holds at 240 Hz | 8 refreshes per frame, 8:30, misses 0 |
| UI loop p99 | < 6 ms |
| Feed p99 to DLSS-G | <= 36 ms at 29.97/30 fps |
| Worker displayed fps (2x) | 59.5-60.0 |
| NR time at 1080p | <= 12 ms typical |
| Startup to first frame | < 4 s |

---

## 7. Test plan

Existing: 82 unit tests in `brp_next_tests` (settings, profiles, pacing helpers,
reports). Run:

```
cmake --build build-perf --config Release --target brp_next_tests --parallel 8
build-perf\Release\brp_next_tests.exe
```

Add for 3.x:

- [ ] Bridge protocol round-trip: every command, every event, malformed lines,
      queue overflow, shell disconnect/reconnect.
- [ ] Settings round-trip: unknown keys preserved, corrupt file quarantined,
      schema migration, all defaults valid.
- [ ] Pacing gate from a recorded probe log.
- [ ] NR fallback: missing runtime, worker crash, HDR bypass, size cap.
- [ ] Overlay lifecycle: open/close, focus restore, DLSS-G pause/resume, profile
      apply from the panel.
- [ ] Packaging verify script as a test (apply patch to baseline, compare).
- [ ] UI smoke: launch, tab through all pages, type in a field, open pickers.

---

## 8. File map

Shell (`ui/src`):

- `main.rs` - GUI subsystem, fonts, single instance, window, panic hook.
- `app.rs` - root view, tabs, chrome, boot, connect modal, pickers, overlay
  lifecycle, poll loop, auto-apply, settings mtime sync.
- `pages.rs` - Home/Stream/Video/Audio/Controller/Neural/Overlay/Advanced.
- `controls.rs` - toggles, segmented, steppers, text fields, disabled rows,
  key routing.
- `widgets.rs` - cards, rows, buttons, switches, chips, stat tiles, logo.
- `overlay.rs` - GPUI control center, topmost/activation, focus restore.
- `background.rs` - aurora (16-layer blobs, 6 blobs, gradient wash).
- `theme.rs` - colors, type scale (text size setting), fonts.
- `model.rs` - flat settings JSON, defaults, presets, status, logging.
- `bridge.rs` - TCP client, status/event parsing, process lifecycle.

Engine (`src/next`):

- `shell/next_app.*` - application, frame loop, presentation, NR/SR/FG wiring,
  bridge commands, overlay model, settings apply.
- `shell/ui_bridge.*` - TCP server, deque outbox.
- `shell/settings_controls.cpp` - classic shell controls (fallback).
- `ai/dlss_fg.*` - DLSS-G worker ownership and submit.
- `overlay/*` - GDI compact card and view model.
- `config/*` - flat JSON, settings, profiles.
- `media/*` - decoder, DLSS SR, NR worker, RIFE, presenter pipeline.
- `platform/*` - window, gamepad, system info.
- `tests/*` - 82 assertions.

Tools: `tools/package_release.ps1`, `tools/make_update.ps1`, probes under
`tools/*_probe`.

---

## 9. Release checklist (repeat for every 3.x build)

1. Bump version in `CMakeLists.txt`, `src/next/shell/next_app.cpp`,
   `tools/package_release.ps1`, `ui/Cargo.toml`, the Home footer string.
2. Kill every `brpnext*` process; a locked exe silently keeps the old binary.
3. Build C++:
   `cmake --build build-perf --config Release --target brpnext-engine brpnext brp_next_tests --parallel 8`
4. Copy `build-perf\Release\brpnext-engine.exe` and `brpnext.exe` into
   `build\Release`.
5. Build Rust release with the Windows SDK on PATH (gpui needs `fxc.exe`):
   `cargo build --release` in `ui`. Verify `target\release\brpnext-ui.exe`
   timestamp changed; if not, build again.
6. Run tests: 82/82.
7. `powershell -File tools\package_release.ps1` (zips ~922 MB).
8. `powershell -File tools\make_update.ps1 -BaselineZip dist\<previous>-standalone.zip -NewFolder dist\BetterRemotePlay -Version <new> -Out dist\<previous>-to-<new>-update.exe`
9. Verify the patch: expand the baseline, apply, check `version.txt`,
   `markers=0`, `files_differ=0`, and the PE subsystem is 2 for the shell.
10. Smoke: launch the packaged shell, confirm no console window, engine starts,
    control center opens, then close.
11. Update `docs/29-handover.md` and `docs/V1_RELEASE_NOTES.md` with hashes.
12. Commit `release: <version> - <one line>`.

Pitfalls learned the hard way:

- PowerShell `Get-Content` without `-Encoding UTF8` reads ANSI and can corrupt
  UTF-8 sources. The UI sources are ASCII-only on purpose; keep them that way.
- `cargo build` may not relink if the exe is running; kill first and verify the
  timestamp.
- `gpui` release builds need `fxc.exe` from
  `C:\Program Files (x86)\Windows Kits\10\bin\<ver>\x64`.
- The installer/bridge must never use a fixed port; the shell picks one.
- Do not enable DLSS-G while the GPUI panel is open; the worker steals focus.

---

## 10. Milestones

**3.0.0 - platform completion (recommended first)**
- Pacing regression gate, pacing graph, NR HDR/60 fps decision, localization
  scaffold, installer + signing, CI, protocol/settings tests, remove dead code
  and inert controls in the classic shell or hide them.

**3.1.0 - neural and smoothness**
- Real depth/flow experiment behind a flag with a permissive model, HDR NR or an
  explicit block, RIFE 120 Hz A/B path, adaptive buffer only with data.

**3.2.0 - product**
- Profile editor and import/export, update channel + check now, tray option,
  log viewer, onboarding, full localization, accessibility pass.

Standards for any milestone: no fake controls, measured acceptance criteria,
patch verification, and a one-paragraph rollback note in the release entry.

---

## 11. Open decisions (need the product owner)

1. Stay on the process split (A) or invest in the in-process C ABI (C)?
2. Ship a monocular depth model for NR, or keep vendor video mode only? Which
   licence is acceptable?
3. HDR policy for NR: block with a clear label, or invest in a 10-bit path?
4. Languages for 3.0 (RO/EN only, or more)?
5. Code-signing certificate and update hosting: who provides them?
6. Keep `brpnext-classic.exe` and its old UI, or drop it from the package?
7. Tray behaviour: close-to-tray or hard exit?

---

## Appendix A - runtime layout of the package

```
BetterRemotePlay\
  brpnext.exe               Rust/GPUI shell (GUI subsystem)
  brpnext-engine.exe        C++ headless engine (WIN32 subsystem)
  brpnext-classic.exe       2.x C++ shell fallback
  brp.exe                   reference product, untouched
  update.exe                patch applier (generated)
  avcodec-63.dll ...        FFmpeg runtime
  nvngx_dlss.dll            DLSS SR feature runtime (user-supplied)
  fonts\                    engine text fonts
  nr\native\                NeuralScreen: nvngx.dll, nvngx_dlssnr.dll, dlssg
  rife\                     TensorRT engine + ONNX models + CUDA runtime
  nvidia-vfx\runtime\       optional legacy VFG
  README.txt, version.txt
```

## Appendix B - settings key groups (flat JSON, schema 1)

`general.*`, `console.*`, `streaming.*`, `video.*`, `ai.*` (SR/FG/NR), `gpu.*`,
`display.*`, `overlay.*`, `audio.*`, `controller.*`, `advanced.*`, `ui.*`.
Unknown keys are preserved. `ui.scale` is text size; `ui.advanced` is the
Basic/Advanced switch; `ui.animated_background`, `ui.reduce_motion`,
`ui.boot_animation` control the shell only.

## Appendix C - the one-paragraph version

BetterRemotePlay Next is a PS5 Remote Play client split into a Rust/GPUI shell
and a headless C++ engine that talk over a loopback line protocol. The engine
owns chiaki-ng streaming, decode, audio, input, DLSS SR, DLSS-G, RIFE and DLSS 5
NR; the shell owns the window, the frosted design, settings, profiles, device
pickers and the F10 control center. 2.4.5 is the stable baseline with 82 tests,
verified patches and measured pacing (8:30 at 30 fps on 240 Hz). 3.x must finish
the release engineering and the neural story without ever faking a feature or
touching the legacy `brp.exe`.
