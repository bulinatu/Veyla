# 05 — Roadmap

Each phase has a **measurable** exit criterion. No phase is "done" because it compiles.

---

## Phase 0 — Research (done)
- [x] Survey open-source clients and their licenses
- [x] Document protocol limits (1080p60, AVC/HEVC, Opus)
- [x] Feasibility matrix for the feature list
- [x] Architecture decision: fork chiaki-ng core
- [ ] **Verify on real hardware** the items marked `VERIFY` in [02-protocol.md](02-protocol.md)

---

## Phase 1 — Baseline (2–3 weeks)

Build chiaki-ng on Windows, stream a PS5, and measure before changing anything.

**Deliverables**
- Reproducible build (CMake + vcpkg manifest pinned).
- A measurement harness: per-frame decode time, network jitter, dropped frames, actual received bitrate, glass-to-glass estimate.
- Baseline numbers at 1080p60 H.264 and HEVC, 15/30/50 Mbps, HDR on/off.

**Exit criterion:** a table of numbers for the official app vs chiaki-ng vs our build on the same network, same game, same scene. If we can't measure it, we can't claim it.

---

## Phase 2 — The picture (3–4 weeks)

This is the phase that makes the product worth existing.

- New `media/` pipeline: `d3d11va` decode → FP16 color stage → flip-model present.
- Full-range RGB output, correct BT.709 matrix, Catmull-Rom chroma, no 8-bit round trip.
- Bitrate exposed 10–100 Mbps, default 35 Mbps at 1080p60 wired.
- Live readout: requested vs actual bitrate, decode ms, present queue depth.
- 10-bit HDR path: `R10G10B10A2` swapchain, PQ→scRGB, EDID peak luminance.

**Exit criterion:**
- Side-by-side screenshots (SDR, same frame) show no crushed blacks / washed highlights vs a reference capture.
- HDR: Windows HDR toggle produces a correctly bright image with no double tone-map.
- Client-side latency (decode+scale+present) ≤ 15 ms at 1080p60.

---

## Phase 3 — Decoders and presenter (2 weeks)

- Capability probe + in-app benchmark for `d3d11va`, `d3d12va`, `dxva2`, `cuda`, `vulkan`, software.
- Auto-select best, allow manual override, show measured decode time per option.
- Present queue depth 1, drop-late, `ALLOW_TEARING`, optional vsync.
- Optional D3D12 presenter behind a flag.

**Exit criterion:** every listed decoder produces a frame on a machine that supports it, and the benchmark ranks them plausibly (hardware < software).

---

## Phase 4 — Upscaling (2 weeks)

- Stage API: `upscale(frame, target) -> frame`.
- Implement NIS, FSR1 (EASU+RCAS), Lanczos, none.
- 1080p → 1440p and 1080p → 2160p presets, plus arbitrary output size.
- Investigate RTX VSR integration (license review first). If blocked, document why and stop.

**Exit criterion:** 1080p→1440p and →2160p at ≤ 3 ms GPU, visually better than bilinear (sharpness measurement + eyeball test), and no ringing on UI text.

---

## Phase 5 — Frame interpolation (2–3 weeks)

- Opt-in stage, per-profile, hotkey toggle, latency cost shown in UI.
- Start with a proven optical-flow implementation (RIFE-class model or LSFG-equivalent) — do not train anything.
- Modes: 30→60, 60→120. 60→240 only if it doesn't look broken.
- Artifact guard: disable on static UI/menus if detectable.

**Exit criterion:** 60→120 is visibly smoother in a panning-camera test, adds ≤ 2 frames of latency, and the setting is clearly labeled experimental.

---

## Phase 6 — Pairing and PSN (2 weeks)

- PSN OAuth (PKCE, loopback redirect), token storage in Credential Manager.
- Account ID auto-fill; console PIN entry UI; remembered keys; multi-console.
- Fallback: manual Account ID (chiaki flow) when the user declines login.

**Exit criterion:** fresh install → console PIN → streaming, with zero manual ID copying, and a working "forget this console" path.

---

## Phase 7 — The shell (3–4 weeks)

- PS5-style launcher: boot animation, horizontal game rail, background art, switcher bar.
- Game tiles from PSN library (or local shortcuts), last-played, stream button.
- In-stream overlay: quick settings, latency HUD, exit.
- Audio: own UI sounds. No Sony assets.

**Exit criterion:** someone who has used a PS5 can navigate it without instructions, and the overlay costs ≤ 1 ms in the presenter.

---

## Phase 8 — Hardening

- Per-game profiles, import/export.
- Wi-Fi detection warning, network quality graph.
- Crash-safe session teardown, reconnect without relaunching.
- Installer, auto-update, signed binaries.
- Docs: setup, troubleshooting, what each setting actually does.

---

## Stretch (only if measured to be worth it)

- **AV1 re-stream/record:** encode our output to AV1 for a second screen or capture. Never on the wire from PS5.
- **Driver-level frame gen:** measure NVIDIA Smooth Motion against our swapchain; ship only if it beats interpolation.
- **Neural video stages:** if a video-domain DLSS/NGX API exists and is licensable, add it as a stage. Pluggable by design.
- **Linux/Steam Deck:** the core is portable; presenter is the work.

---

## Definition of done for v1.0

| Metric | Target |
|---|---|
| Glass-to-glass, wired, 1080p60, no interp | ≤ 55 ms |
| Client-side processing | ≤ 15 ms |
| Default bitrate | 35 Mbps, actual reported live |
| Color accuracy (SDR) | No range/matrix errors; verified against reference capture |
| HDR | 10-bit output, correct peak luminance from EDID |
| Decoders working | 6 of 6 listed |
| Setup time, fresh install | ≤ 3 minutes with PSN login |
| Installer | signed, auto-updating |

---

## Non-goals

- Beating the console's encoder limits.
- Shipping anything that bypasses PS5 security or enables piracy.
- A web/Electron app in the video path.
