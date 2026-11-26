# 39 - Repository organization

The working tree is split into what the current 2.5.0 build actually uses and a
kept-for-later area.

## Used by the build

| Path | What it is |
|---|---|
| `src/next/**` | GPUI shell, headless engine, settings, AI glue |
| `src/media/**` | decoder, presenter, DLSS SR/NR, Reflex, shaders |
| `src/session/**` | chiaki-ng session wrapper, profile, PSN |
| `src/platform/**` | Win32 window, gamepad |
| `src/audio/**` | WASAPI event-driven output |
| `src/app/**` | legacy `brp.exe` UI (still a build target) |
| `ui/**` | Rust/GPUI shell |
| `third_party/**` | FFmpeg libs/headers, NGX headers, NeuralScreen runtime |
| `vendor/chiaki-ng/**` | vendored protocol core |
| `tools/**` | release scripts, probes, icon generator |
| `docs/**` | design and audit notes |
| `assets/**` | fonts and app icon |
| `dist/BetterRemotePlay/` | current packaged tree |
| `dist/BetterRemotePlay-2.5.0-*` | current release artifacts |

## Moved to `attic/` (not used by CMake)

| Path | Why |
|---|---|
| `attic/third-party-src/` | reference copies of FFmpeg source, libplacebo and the lsfg-vk tree; the build uses `third_party/` instead |
| `dist/archive/` | all pre-2.5.0 zips and update executables (23 files) |

`attic/` is git-ignored so the source tree stays small. The old releases are
kept for rollback. Do not move `third_party/` (underscore): it is the FFmpeg
link/runtime directory used by CMake and the packager.

## Generated, not source

`build/`, `build-perf/`, `ui/target/`, `dist/` and `attic/` are build or
archive output. The package script reads `build/Release` and
`ui/target/release/brpnext-ui.exe`; do not delete those while packaging.
