# Feature Matrix - BetterRemotePlay Next

Legend: **R** = reuse from the reference implementation, **N** = new module,
**P** = port/rewrite, **-** = not planned.

| Feature | Reference today | Next target | Plan | Stage | Notes |
| --- | --- | --- | --- | --- | --- |
| PS5 discovery | Working | **Working in brpnext** | R behind `IStreamingService` | 4 | UDP, Console page, verified on LAN |
| PSN login | Working | **Working in brpnext** | R behind `IStreamingService` | 4 | browser + clipboard redirect |
| PIN pairing | Working | **Working in brpnext** | R behind `IStreamingService` | 4 | 8-digit PIN field |
| Session request/wake | Working | **Working in brpnext** | R behind `IStreamingService` | 4 | chiaki-ng, rest-mode wake |
| Reconnect | Working | Same | R + new state machine | 4 | keep RST behaviour |
| Video decode | D3D11VA/D3D12VA/Vulkan/DXVA2/NVDEC/SW | Same | R behind `IVideoDecoder` | 5 | 8-10 ms at 1080p |
| HDR10 | Working | Same | R + validated metadata | 5 | no tone-map recreation |
| Audio | WASAPI, Opus, buffer | Same | R behind `IAudioSink` | 5 | device hot-swap |
| DualSense HID | LEDs, triggers, haptics, rumble | Same | R behind `IControllerService` | 5 | 10 ms haptic refresh |
| Controller mapping | Basic | Editable | P | 8 | only if protocol supports |
| D3D11 presenter | Waitable, 240 Hz | Same | R behind `IPresenter` | 5 | keep borderless behaviour |
| Borderless/windowed | Working | **Working in brpnext** | R | 5 | F11 toggles borderless fullscreen; fullscreen on play enters it for Watch |
| VRR/G-Sync detection | Not implemented | Detect + report | N | 10 | never force unsupported |
| Bilinear/Catmull/Lanczos | Working | Same | R | 5 | |
| FSR1 | Working | Same | R | 5 | |
| DLSS SR | Working (NGX, K/L/M, 33-100 %) | Same | R behind `ISuperResolution` | 6 | primary SR |
| DLSS NR | Working (NGX) | Same | R behind `ITemporalFilter` | 6 | optional |
| Custom TensorRT SR | Not implemented | Optional/experimental | N | 6 | only if measured worthwhile |
| RTX VSR | Removed | **Excluded** | - | - | explicit requirement |
| RIFE 30->60 | Working (4.25 lite / 4.26 heavy) | **Working in brpnext** | R behind `IFrameGenerator` | 6 | 29 gen/s, 58 shown/s verified |
| DLSS-G 60->120 | Working after 0.4.9 | **Working in brpnext** | R | 6c | worker presenter, 2x at 120.0 fps verified; 3x-6x with the SM86 proxy |
| DLSS-G 30->120 | Impossible on this runtime | Not offered | - | - | runtime refuses >2x |
| NVIDIA VFG legacy | Optional, working | Keep or drop | R | 6 | only if it beats DLSS-G |
| NVOF/NvOFFRUC backend | Not implemented | Evaluate | N | 6 | quality win required |
| Hybrid OF + refinement | Not implemented | Evaluate | N | 6 | no fake backend |
| SR + FG simultaneous | Working (DLAA + 120 verified) | **Working in brpnext** | R | 6c | DLAA at 1080p feeds the worker at 1080p (low-latency path); 1440p SR -> worker reaches ~101 generated fps |
| Colour grading | Working | Same | R behind `IColorPipeline` | 5 | 10 controls |
| Sharpening | Working | Same | R | 5 | |
| Main UI | PS5 purple, immediate mode | New NVIDIA-style design system | N | 3 | dark default + light |
| Sidebar navigation | Absent | Persistent + collapsed | N | 3 | 9 pages |
| Dashboard | Hero card | Real system overview | N | 3 | no fake stats |
| Console page | Settings tab | **Working** | N | 4 | discovery, PSN, pairing, connect, live stats |
| Streaming page | Settings tab | Dedicated page | N | 5 | groups, live state |
| AI Video Engine page | Upscaling tab | Dedicated page (SR/FG/GPU) | N | 6 | capability-driven |
| Display page | Settings tab | Dedicated page | N | 5 | real modes only |
| Overlay | In-app sheet + worker window | **Independent overlay working** | N | 7 | stats, opacity, 4 positions, interval |
| Settings | Scattered tabs | Central typed categories | N | 3/8 | validation + migrations |
| Profiles | Presets only | Named, import/export, default | N | 8 | compatibility check |
| Diagnostics page | Logs + HUD | Live metrics + benchmarks | N | 7/9 | p50/p95/p99 |
| Notifications | Inline notes | Central toast/stack | N | 3 | deduplicated |
| Updater | Single `update.exe` | Same, both executables | R | 11 | one patch |
| Packaging | Standalone zip | Same layout | R | 11 | shared runtimes |
| Unit tests | None | `brp_next_tests` | N | 2 | config/nav/metrics |
| Hardware probes | 20+ manual | Keep | R | 2 | GPU truth |
| Benchmark matrix | Manual | **4-case baseline matrix working** | N | 9 | 1440p/2160p; 4K display validation pending |
| Long soak/device loss | Partial | Auto reconnect + device-loss stop; soak pending | P | 10 | 5080 too |
| Telemetry/cloud | None | None | - | - | local only |

## Status at the start of the rebuild

- Reference app 0.4.9 is working, including the NVIDIA 60/120 fix.
- Stage 1 documents: this file, `ARCHITECTURE.md`, `REPOSITORY_ANALYSIS.md`,
  `TECHNOLOGY_DECISIONS.md`, `IMPLEMENTATION_ROADMAP.md`.
- Stage 2 foundation is built and tested: `src/next/core` (interfaces/models),
  `config` (typed settings, legacy import, profiles), `diagnostics` (logger,
  rolling p50/p95/p99, report), `app` (navigation, DI registry, AppHost),
  `ui/theme` (dark/light tokens) and `brp_next_tests` (82 assertions).
- Stage 3 shell is working as `brpnext.exe`.
- Stage 4 streaming service is working: discovery, PSN, pairing, wake,
  connect/disconnect and live stats.
- Stage 5 media presentation is working: full-window video, audio and
  controller input on the real session.
- Stage 6 is working: DLSS SR 1080p -> 1440p, RIFE 30 -> 60
  (29 gen/s, 58 shown/s) and DLSS-G 60 -> 120 through the worker are verified
  in `brpnext`.
- Stage 7 independent overlay and diagnostics are working (basic widget set).
- Stage 8 settings/profiles/reset/import/export/compatibility warnings work.
- Stage 9 has both a session report and an automated four-case baseline matrix;
  true 4K display validation is pending.
- Stage 10 has auto reconnect and device-loss handling; soak tests are pending.
- `brp.exe` 0.4.9 still builds and runs.

- Stage 11: 0.5.0 package with both executables and a 1.29 MB update from
  0.4.9; clean-package launch verified.
- DLSS-G in `brpnext`: 60 fps source -> 120.0 generated fps at 1080p; SR 1440p
  -> worker reaches about 101 generated fps.
