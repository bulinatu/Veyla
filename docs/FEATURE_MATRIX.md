# Feature Matrix - BetterRemotePlay Next

Legend: **R** = reuse from the reference implementation, **N** = new module,
**P** = port/rewrite, **-** = not planned.

| Feature | Reference today | Next target | Plan | Stage | Notes |
| --- | --- | --- | --- | --- | --- |
| PS5 discovery | Working | Same | R behind `IStreamingService` | 4 | UDP, verified on LAN |
| PSN login | Working | Same | R behind `IAuthService` | 4 | browser redirect paste |
| PIN pairing | Working | Same | R behind `IPairingService` | 4 | 8-digit PIN |
| Session request/wake | Working | Same | R behind `IStreamingService` | 4 | chiaki-ng |
| Reconnect | Working | Same | R + new state machine | 4 | keep RST behaviour |
| Video decode | D3D11VA/D3D12VA/Vulkan/DXVA2/NVDEC/SW | Same | R behind `IVideoDecoder` | 5 | 8-10 ms at 1080p |
| HDR10 | Working | Same | R + validated metadata | 5 | no tone-map recreation |
| Audio | WASAPI, Opus, buffer | Same | R behind `IAudioSink` | 5 | device hot-swap |
| DualSense HID | LEDs, triggers, haptics, rumble | Same | R behind `IControllerService` | 5 | 10 ms haptic refresh |
| Controller mapping | Basic | Editable | P | 8 | only if protocol supports |
| D3D11 presenter | Waitable, 240 Hz | Same | R behind `IPresenter` | 5 | keep borderless behaviour |
| Borderless/windowed | Working | Same | R | 5 | F11 |
| VRR/G-Sync detection | Not implemented | Detect + report | N | 10 | never force unsupported |
| Bilinear/Catmull/Lanczos | Working | Same | R | 5 | |
| FSR1 | Working | Same | R | 5 | |
| DLSS SR | Working (NGX, K/L/M, 33-100 %) | Same | R behind `ISuperResolution` | 6 | primary SR |
| DLSS NR | Working (NGX) | Same | R behind `ITemporalFilter` | 6 | optional |
| Custom TensorRT SR | Not implemented | Optional/experimental | N | 6 | only if measured worthwhile |
| RTX VSR | Removed | **Excluded** | - | - | explicit requirement |
| RIFE 30->60 | Working (4.25 lite / 4.26 heavy) | Same | R behind `IFrameGenerator` | 6 | 17.7 / 23-25 ms |
| DLSS-G 60->120 | Working after 0.4.9 | Same | R behind `IFrameGenerator` | 6 | 2x ceiling, verified |
| DLSS-G 30->120 | Impossible on this runtime | Not offered | - | - | runtime refuses >2x |
| NVIDIA VFG legacy | Optional, working | Keep or drop | R | 6 | only if it beats DLSS-G |
| NVOF/NvOFFRUC backend | Not implemented | Evaluate | N | 6 | quality win required |
| Hybrid OF + refinement | Not implemented | Evaluate | N | 6 | no fake backend |
| SR + FG simultaneous | Working (DLAA + 120 verified) | Same, better scheduling | R/N | 6 | 0 drops at 60->120 |
| Colour grading | Working | Same | R behind `IColorPipeline` | 5 | 10 controls |
| Sharpening | Working | Same | R | 5 | |
| Main UI | PS5 purple, immediate mode | New NVIDIA-style design system | N | 3 | dark default + light |
| Sidebar navigation | Absent | Persistent + collapsed | N | 3 | 9 pages |
| Dashboard | Hero card | Real system overview | N | 3 | no fake stats |
| Console page | Settings tab | Dedicated page | N | 4 | connection + auth |
| Streaming page | Settings tab | Dedicated page | N | 5 | groups, live state |
| AI Video Engine page | Upscaling tab | Dedicated page (SR/FG/GPU) | N | 6 | capability-driven |
| Display page | Settings tab | Dedicated page | N | 5 | real modes only |
| Overlay | In-app sheet + worker window | Independent overlay engine | N | 7 | widgets, layouts |
| Settings | Scattered tabs | Central typed categories | N | 3/8 | validation + migrations |
| Profiles | Presets only | Named, import/export, default | N | 8 | compatibility check |
| Diagnostics page | Logs + HUD | Live metrics + benchmarks | N | 7/9 | p50/p95/p99 |
| Notifications | Inline notes | Central toast/stack | N | 3 | deduplicated |
| Updater | Single `update.exe` | Same, both executables | R | 11 | one patch |
| Packaging | Standalone zip | Same layout | R | 11 | shared runtimes |
| Unit tests | None | `brp_next_tests` | N | 2 | config/nav/metrics |
| Hardware probes | 20+ manual | Keep | R | 2 | GPU truth |
| Benchmark matrix | Manual | Automated 4 cases | N | 9 | 1440p/2160p |
| Long soak/device loss | Partial | Full | P | 10 | 5080 too |
| Telemetry/cloud | None | None | - | - | local only |

## Status at the start of the rebuild

- Reference app 0.4.9 is working, including the NVIDIA 60/120 fix.
- Stage 1 documents: this file, `ARCHITECTURE.md`, `REPOSITORY_ANALYSIS.md`,
  `TECHNOLOGY_DECISIONS.md`, `IMPLEMENTATION_ROADMAP.md`.
- Stage 2 foundation is built and tested: `src/next/core` (interfaces/models),
  `config` (typed settings, legacy import, profiles), `diagnostics` (logger,
  rolling p50/p95/p99, report), `app` (navigation, DI registry, AppHost),
  `ui/theme` (dark/light tokens) and `brp_next_tests` (79 assertions).
- No streaming, AI or UI code has been moved yet; nothing in `brp.exe` was
  removed. The old app still builds and runs.
