# Version 1.0 release checklist

## Build and tests

- [x] `brpnext` Release build.
- [x] `brp_next_tests` 82 assertions, exit 0.
- [x] `brp.exe` still builds and is packaged.
- [x] No compiler errors in the 1.0 tree.

## Functional verification (reference PS5)

- [x] Session request and stable streaming.
- [x] Video decode, audio, DualSense input.
- [x] DLSS SR 1080p -> 1440p.
- [x] DLAA 1080p -> 1080p.
- [x] DLSS-G 2x on the official runtime (30 fps source -> 60).
- [x] RIFE 30 -> 60 path present; not re-measured in this final pass.
- [x] Borderless fullscreen and restore.
- [x] Double-Esc disconnect.
- [x] F10 overlay control center; tab switching; live SR toggle.

## Settings

- [x] One `AppSettings` instance shared by pages and overlay.
- [x] Validation before persistence.
- [x] Runtime scope classification.
- [x] Failed save restores the previous settings file.
- [x] Stream-restart-required banner and status.
- [x] Dead controls removed from the UI.

## Overlay

- [x] Independent layered window.
- [x] Compact click-through stats mode.
- [x] Expanded interactive control center.
- [x] Mouse and keyboard navigation.
- [x] Tabs for Overview/SR/FG/Display/Performance/Profile.
- [x] Follows the app monitor.
- [x] Hidden when the app is not foreground.
- [x] Scale/opacity/position/interval applied.

## Packaging

- [x] Version 1.0.0 in application, package and about.
- [x] Both executables and the worker/runtime files copied.
- [x] No session.json or credentials copied.
- [x] Update executable generated from 0.4.9.
- [x] Patch install verified with `files_differ=0`.

## Documentation

- [x] `docs/RELEASE_AUDIT.md`
- [x] `docs/KNOWN_ISSUES.md`
- [x] `docs/KNOWN_LIMITATIONS.md`
- [x] `docs/V1_RELEASE_NOTES.md`
- [x] `docs/V1_RELEASE_CHECKLIST.md`
- [x] `docs/USER_GUIDE.md`
- [x] `docs/ARCHITECTURE.md`
- [x] `docs/29-handover.md` updated with release hashes.

## Outstanding (documented)

- [ ] HDR presentation.
- [ ] Per-adapter GPU selection / monitor mode switching.
- [ ] Cooperative cancellation of the connect thread.
- [ ] Process-wide Winsock initialization.
- [ ] 4K/120 game validation, multi-hour soak, RTX 5080.
- [ ] Profile rename/duplicate UI and delete confirmation.
