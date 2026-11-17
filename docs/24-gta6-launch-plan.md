# GTA 6 launch plan - atomic engineering runbook

Target: at 00:00 the client must behave like a native PC game: 120 fps output
from DLSS SR + FG, no stutter, audio in sync, controller features working, and
a one-click fallback. This document is the day-of plan at file/function/member
level. No risky rewrite after 18:00.

## 0. Ground rules

- **Code freeze at 18:00.** After that: only presets/config, packaging and tests.
- **Do not touch**: NVAPI Reflex mode/Sleep, the waitable swapchain pacing,
  the decoder thread ownership, the closed NeuralScreen worker ABI, VSR.
- **Known-good snapshot** before any edit: git tag + copy of the current ZIP,
  EXE and `%APPDATA%\BetterRemotePlay\session.json`.
- Every change is a separate commit with one purpose. If a test fails, revert
  that commit; do not patch forward under time pressure.
- The FG worker is the source of truth for smoothness: `[fg] displayed X FPS`
  in `nr-worker.log`. The parent `[frame]` probe is diagnostic only while the
  FG child covers the window.

## 1. Verified baseline (evening before)

| Item | Value |
| --- | --- |
| GPU / display | RTX 4090, 2560x1440 @ 239/240 Hz |
| Console | PS5-580, 192.168.1.21, paired |
| Preset | DLSS SR Ultra Quality (77%) + DLSS FG 2x, 1440p |
| Worker | `[fg] displayed 120.0 FPS (real + generated, 2x)`, present 2560x1440 |
| HUD | `DLSS 0.7 ms   FG 120 fps`, `select 4v 99-100%`, `lost 0`, `drops 0` |
| Parent source tick | ~5-6 ms (SR upload + submit + 14 MB readback + worker submit) |
| Chrome revealed | worker temporarily ~105 fps, returns to 120 when covered |
| Reflex | off (driver mode breaks the 240 Hz present clock) |
| VSR | removed |
| Package | `dist\BetterRemotePlay-0.3-standalone.zip`, 571.6 MB, hash verified |

## 2. Code map (what touches what)

### Presentation and pacing

- `src/main_app.cpp`
  - `main()`: high-resolution waitable timer, `timeBeginPeriod(1)`, thread
    priority, `WaitForPresent` when the FG child does **not** cover the parent,
    timer pacing when it does, `BRP_FRAME_PROBE`.
- `src/media/pipeline.cpp`
  - `Presenter::CreateSwapchainForFormat`: flip-discard, ALLOW_TEARING,
    `FRAME_LATENCY_WAITABLE_OBJECT`, max latency 1, `swapchain_flags_`.
  - `Presenter::Resize`: passes `swapchain_flags_`, recreates on failure.
  - `Presenter::PresentNow`: Present(1), frame statistics, device-loss flag.
  - `Presenter::Render`: source texture -> upscale/fit -> backbuffer.
- `src/app/app.cpp`
  - `App::Frame`: per-frame input, `TickSession`, `VideoPaced`/`video_phase_`,
    `TakeFrame`, DLSS SR/NR/FG chain, UI, Present, `LogVideoCadence`.
  - `App::present_covered()`: main loop uses it to choose the frame clock.
  - `App::VideoPaced()`: true when `stream_buffer_ > 0 && frame_gen_ < 2`;
    the source is emitted on a `profile_.fps / present_hz` rational phase.
  - `App::NoteSourceFrame()` / `LogVideoCadence()`: cadence histograms.
  - `App::RenderStream`: HUD cache, chrome fade, FG reveal logic.
- `src/session/streaming.cpp`
  - `DecodeThreadMain`: compressed-unit queue -> decode -> `frames_`.
  - `TakeFrame(out, buffer, paced)`: one/two-frame cushion, fixed-grid emit.
  - `Streaming::fps()` / `ConsumeStats()`: source cadence and decode telemetry.

### DLSS

- `src/media/nv_dlss_sr.cpp`
  - `Initialize`: D3D12 device on the presenter's adapter, shared D3D11/D3D12
    textures, two output slots, guide resources.
  - `Process`: upload RGBA, submit D3D12 evaluation, return the **previous**
    completed output; `Submit` + `WaitPending` implement the one-frame async
    pipeline.
  - `Readback` (wait for pending, current) and `ReadbackLast` (previous,
    no wait, used to feed FG).
- `src/media/nv_dlssnr.cpp`
  - `Start`: worker process, shared memory, present mode, RNSZ.
  - `EnablePresent(w,h)`: child window at the requested frame size.
  - `Process`: RGBA -> worker shared memory; in present mode the worker
    presents, no readback.
  - `Reconfigure`: live NR settings; `SetFrameGeneration` / `SetNeuralBypass`.
- `src/app/app.cpp`
  - `EnsureDlssSr` / `EnsureDlssDlaa` / `EnsureDlssSrForTarget`:
    feature creation for the current mode and target.
  - `RunDlssSr`: SR/DLAA input scaling + `dlss_sr_.Process`.
  - `ApplyNeuralSettings`: SR on/off; when FG is on, keeps it and re-sizes.
  - `ApplyDlssFrameGen`: ensures SR, `NrStageSize`, worker restart on size
    change, `EnablePresent`, child window attach.
  - `NrStageSize`: SR output size when FG+SR, otherwise the stream size.
  - `ApplyRenderTarget`: target change -> SR re-init -> FG worker re-size.
  - `SubmitNrFrame` / `SubmitNrFrameData` -> `NrThreadMain`.
- `src/media/nv_reflex.cpp`: mode rejected (documented tradeoff).

### Input, audio, recovery

- `src/platform/gamepad.cpp`: HID read thread, 547-byte Bluetooth output,
  triggers/rumble/lightbar, `PadThreadMain` 250 Hz.
- `src/audio/audio_engine.cpp`: WASAPI render thread, ring buffer, underruns.
- `src/app/app.cpp`: `StartPadThread`, `ApplyControllerOutput`, `HandlePad`.
- `App::StartStream` / `TickSession` / `DisconnectStream`: session setup and
  teardown on a worker thread.
- `App::RecoverFromDeviceLoss`: one automatic relaunch with `BRP_RESTARTED`.

## 3. Work packages

Each WP has: goal, exact touch points, change, test, expected output, rollback.

### WP-0 - Snapshot and rollback (no code, 20 min)

1. `git tag gta6-preflight-<date>`.
2. Copy `dist\BetterRemotePlay-0.3-standalone.zip` to a safe folder.
3. Copy `build\Release\brp.exe` to `brp-known-good.exe`.
4. Copy `%APPDATA%\BetterRemotePlay\session.json` to `session-backup.json`.
5. Record the current preset and hashes in the health log header.

Rollback: restore the EXE + profile; `git reset --hard gta6-preflight`.

### WP-1 - 10-minute SR+FG soak and go/no-go (no code, 40 min)

1. Launch with `BRP_FRAME_PROBE=1`, `BRP_DECODE_PROBE=1`.
2. Connect, F11, move the cursor away from the top strip.
3. Watch `build\Release\nr-worker.log`: `[fg] displayed` should be 119-121.
4. Watch `[video]`: `select 4v >= 98%`, `queue 0-1`, `drops 0`, `lost 0`.
5. Watch `[app]`: `fps 59-61`, decode avg < 7 ms, max < 20 ms.
6. `nvidia-smi` every minute: power, temp, VRAM.

**Go** if the worker holds >= 118 for 10 minutes. **No-go**: apply a preset:
- FG 2x without SR (Safe), or
- SR only, or
- raw low latency.

Record the decision in the health log and in `docs/25-launch-decision.md`.

### WP-2 - GTA 6 frame-rate / HDR decision (no code, 60 min)

1. Start GTA 6 in **Performance** mode; confirm 60 fps in the HUD (`PS5 60.0`).
2. If cutscenes/quality mode run at 30: set **Stream FPS = 30** (Display tab,
   W_Fps), keep FG 2x (worker receives 30, outputs 60) or turn FG off.
   - The console still sends 60 duplicates; `VideoPaced` at 30 drops one of
     each pair and the worker gets a clean 30 fps sequence.
   - Test both; if FG artifacts on the duplicated content, use FG off + 30.
3. HDR matrix:
   - SDR baseline: capture a screenshot, note colors/black level.
   - HDR on: restart the app (the setting says restart), stream, capture.
   - Check the worker present format in `nr-worker.log` (`format=28` = RGBA8,
     no HDR). If HDR is wrong through FG, use **SDR** for launch or SR-only HDR.
4. Record the final HDR/30fps decision.

### WP-3 - Launch preset buttons (code, ~60-90 min)

**Files**: `src/app/app.h`, `src/app/app.cpp`.

**IDs**: `W_PresetLaunch = 110`, `W_PresetSafe = 111`, `W_PresetSr = 112`,
`W_PresetLow = 113` in `enum WidgetId` (110+ is free; 90-104 are used).

**App members**:

```cpp
void ApplyLaunchPreset(int preset);   // 0 launch, 1 safe, 2 sr, 3 low
int active_preset_ = -1;
```

**Dispatch**: in `DispatchClick`, `case Hit::Button`, add before the generic
`ApplyDropdown` fallback:

```cpp
else if (it->index == W_PresetLaunch) ApplyLaunchPreset(0);
else if (it->index == W_PresetSafe)   ApplyLaunchPreset(1);
else if (it->index == W_PresetSr)     ApplyLaunchPreset(2);
else if (it->index == W_PresetLow)    ApplyLaunchPreset(3);
```

**Implementation**:

```cpp
void App::ApplyLaunchPreset(int preset) {
    active_preset_ = preset;
    switch (preset) {
        case 0:  // Launch: SR Ultra Quality + FG 2x, 1440p
            profile_.neural_sr = 1; profile_.dlss_mode = 4; profile_.dlss_fg = 2;
            profile_.neural_nr = 0; profile_.render_target = 2;
            render_target_ = 2; profile_.frame_buffer = 1; stream_buffer_ = 1;
            ApplyRenderTarget();       // ensures SR, re-sizes FG
            ApplyNeuralSettings();     // keeps FG, re-checks SR
            ApplyDlssFrameGen();       // idempotent if size matches
            break;
        case 1:  // Safe: FG 2x without SR
            profile_.neural_sr = 0; profile_.dlss_fg = 2; profile_.neural_nr = 0;
            ApplyNeuralSettings();     // else branch reconfigures FG to 1080p
            ApplyDlssFrameGen();
            break;
        case 2:  // SR only
            profile_.neural_sr = 1; profile_.dlss_mode = 4; profile_.dlss_fg = 0;
            profile_.neural_nr = 0;
            ApplyNeuralSettings();     // SR, no FG
            ApplyDlssFrameGen();       // stops the worker
            break;
        case 3:  // Low latency raw
            profile_.neural_sr = 0; profile_.dlss_fg = 0; profile_.neural_nr = 0;
            profile_.upscaler = 0; upscale_ = 0;
            presenter_.SetUpscaleMode(0);
            profile_.render_target = 0; render_target_ = 0;
            profile_.frame_buffer = 0; stream_buffer_ = 0;
            ApplyNeuralSettings(); ApplyDlssFrameGen(); ApplyRenderTarget();
            break;
    }
    SaveProfile();
    stream_note_ = "Preset applied.";
}
```

Order notes:
- `ApplyRenderTarget()` may call `ApplyDlssFrameGen()` when SR is ready; the
  subsequent calls are idempotent if the stage size did not change.
- `ApplyNeuralSettings()` else branch calls `ApplyDlssFrameGen()` when SR is
  turned off and FG is on; this is what resizes the worker to 1080p.
- Each call can restart the worker; do it **before** playing, not mid-game.

**UI**: in `RenderSettings`, `Tab::Performance`, after the Reflex note
(`y + 58`) add:

```cpp
y += 92.0f;
ui_.Text(L"Launch presets", ...);
y += 26.0f;
DrawButton(L"Launch: SR + FG 2x", {kContentX, y, kContentX + 230, y + 44},
           W_PresetLaunch, true);
DrawButton(L"Safe: FG 2x", {kContentX + 246, y, kContentX + 446, y + 44},
           W_PresetSafe, false);
y += 52.0f;
DrawButton(L"SR only", {kContentX, y, kContentX + 230, y + 44}, W_PresetSr, false);
DrawButton(L"Low latency", {kContentX + 246, y, kContentX + 446, y + 44},
           W_PresetLow, false);
```

Test each preset while streaming: watch the worker log and HUD; ensure a single
worker restart, no crash, and 120 fps for Launch/Safe. Rollback: revert the commit.

### WP-4 - Health log (code, ~60 min)

**Goal**: one line every 30 s in `%APPDATA%\BetterRemotePlay\health.log` so a
post-launch stutter can be diagnosed without the HUD.

**Files**: `src/app/app.h`, `src/app/app.cpp`, `src/media/pipeline.h/cpp`.

**App members**:

```cpp
void LogHealth();
double health_at_ = -1.0;
```

**Presenter VRAM accessor** (`pipeline.h/cpp`):

```cpp
Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter3_;
bool video_memory_used_mb(uint64_t& used, uint64_t& budget) const;
```

`Init`: `adapter.As(&adapter3_)`; `video_memory_used_mb` calls
`adapter3_->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)`.

**Log line** (append, flush):

```
2026-11-19T00:05:00 preset=launch sr=uq77 fg=2 nr=0 hdr=0 target=1440 fb=1
  worker=120.0 dlss=0.7 fg_mult=2 | frame=239.9 p50=4.17 p99=4.44 late=0 stalls=0
  | src=60.0 sel4=100 q=1 drops=0 lost=0 dec=4.6/6.2 out=3 | audio=0
  handles=812 vram=3120/24000
```

Fields:
- `worker`: `fg_worker_fps_`; `dlss`: `dlss_sr_.last_ms()`.
- `frame`: `stat_fps_`, `stat_p50_/p95_/p99_`, `stat_late_/stalls_`.
- `src`: `stream_stats_.frames/elapsed`; `sel4`: percentage of 4v in
  `video_holds_`/`screen_holds_`; `q`: `stream_stats_.queue`; `drops/lost`;
  `dec` avg/max; `out` arrival outliers.
- `audio`: `audio_.underruns()`.
- `handles`: `GetProcessHandleCount(GetCurrentProcess(), &n)`.
- `vram`: `presenter_.video_memory_used_mb`.

Call from `App::Frame` after the 1 s `stream_stats_` block, gated by
`clock_ - health_at_ >= 30.0`; also write one header on session start/preset.

Test: `Get-Content health.log -Tail 5`; compare `work p99` with and without
the log (should be unchanged, 30 s I/O only). Rollback: revert.

### WP-5 - HDR (test; guard only if needed)

- Test as in WP-2. If HDR via FG is wrong:
  - Set SDR for the launch (`profile_.hdr=0`), restart.
  - Add a note in Display: "HDR is not carried by the FG worker; use SDR or
    SR-only HDR."
  - Optional guard: if `profile_.hdr && profile_.dlss_fg>=2`, show a warning
    in Performance; do not silently change the user's setting.
- Acceptance: either HDR correct with FG, or SDR chosen and documented.

### WP-6 - Audio matrix (no code, 60 min)

1. Devices: TV/HDMI, DualSense jack (USB), USB headset.
2. Buffers: 10/20/30/40 ms via Audio tab.
3. 10 minutes each at the chosen stream preset; check `underruns` in HUD/health.
4. Lip sync: GTA 6 dialogue/cutscene; note audio lead/lag.
5. Pick the lowest buffer with 0 underruns. Do not change during play.
6. If crackle: raise buffer; if audio leads, raise buffer slightly; if lags,
   lower.

### WP-7 - Controller matrix (no code, 60 min)

1. DualSense USB and Bluetooth; check `[hid]` logs and `is_bluetooth()`.
2. Buttons, sticks, touchpad (map/phone), PS button, rumble, adaptive triggers
   in GTA 6 (weapons/driving).
3. Report rate 250/1000 Hz; battery.
4. Prefer USB for latency; Bluetooth as backup.
5. Verify the pad thread survives disconnects/reconnects.

### WP-8 - Network and recovery (no code, 60 min)

1. Wired Ethernet both ends; test 50/80/100 Mbps (Connection tab).
2. 10 minutes each: `[app]` `lost`, `queue`, `drops`, kbps, decode max.
3. Pull the PC cable for 5 s: session should fail to Home; Connect again;
   measure recovery.
4. Console rest/wake: Home -> Connect; verify the wake path and the control
   port probe.
5. PSN offline: local Remote Play should still connect with the saved account
   ID/regist key.

### WP-9 - 2-hour soak (no code, 120 min)

Run the final preset with health logging. Every 15 minutes:
- `[fg] displayed` (120.0), `[video]` select/4v, drops/lost.
- `Get-Process brp | Select Id,HandleCount,WorkingSet64,CPU`.
- `nvidia-smi --query-gpu=memory.used,temperature.gpu,power.draw,clocks.sm`.
- Check `health.log` VRAM/handles trend.
Pass: worker >= 118 throughout, handles +<50, VRAM +<400 MB, no crashes,
no device-loss restarts, no session drops.

### WP-10 - Package and freeze (no code, 60 min)

1. Build Release; copy to `build\Release`; run the package script.
2. Verify `dist\BetterRemotePlay\brp.exe` hash matches.
3. Clean-profile test: temp APPDATA, launch, no personal data.
4. Extract the ZIP to a temp folder and launch it once.
5. Update `README.txt`/docs with presets + health log location.
6. Archive the final ZIP/EXE/profile; **freeze**.

## 4. Failure-mode table

| Symptom | Likely code point | Check | Action |
| --- | --- | --- | --- |
| Worker < 118 while covered | source tick (`RunDlssSr` + `ReadbackLast` + `SubmitNrFrameData`) | `[video] src`, `work p99`, `nr_dropped_` | Safe preset (FG only) or SR only |
| Worker ~105 only when chrome visible | `fg_reveal_until_`, `ShowWindow` | cursor in top strip | move cursor away; normal |
| HUD `DLSS 0.0 ms` with SR on | `RunDlssSr` / `dlss_sr_.Process` | `dlss_note_`, `[app]` | check runtime/`nvngx_dlss.dll`; fallback SR off |
| Black screen after enabling FG | `AttachFgWindowNow`, `present_covered_` | `fg_window_`, `[presenter]` | Esc/Home; restart; Safe preset |
| Stutter at 30 fps content | duplicated source + FG | `PS5 fps`, Stream FPS | Stream FPS 30; FG off/2x test |
| Audio crackle | `audio_.underruns` | Audio tab | raise buffer; change device |
| Audio leads video | FG/SR latency | lip sync | raise audio buffer one step |
| Input lag | Bluetooth/output rate/FG | `[hid]`, pad rate | USB, 1000 Hz, FG off |
| Session drop / no connect | `Probe`/`Wake`/`Streaming::Start` | `stream_note_`, `[app] start failed` | Home -> Connect; check console rest mode |
| Resize/fullscreen broken | `Presenter::Resize` flags | `[presenter] resize` | restart; already fixed |
| Device lost | `Present`/`Resize` hr | `[presenter] device lost` | app auto-restarts once |
| Worker failed | `NrFailed` | `nr-worker.log` | app disables FG; SR/raw |
| HDR colors wrong | present format 28 | `nr-worker.log format=28` | SDR / SR-only HDR |
| Health log missing | `LogHealth` path | `%APPDATA%\BetterRemotePlay\health.log` | check permissions |

## 5. Test commands

```powershell
# stop any running instance
Get-Process brp -ErrorAction SilentlyContinue | Stop-Process -Force

# build + install the test exe
cmake --build build-perf --config Release --target brp --parallel 8
Copy-Item build-perf\Release\brp.exe build\Release\brp.exe -Force

# probes
$env:BRP_FRAME_PROBE = "1"
$env:BRP_DECODE_PROBE = "1"
$log = "build\run-$(Get-Date -Format HHmm).log"
Start-Process .\build\Release\brp.exe -WorkingDirectory (Resolve-Path .\build\Release) `
  -RedirectStandardError $log

# telemetry
Get-Content $log | Select-String "\[frame\]|\[video\]|\[app\] fps|DLSS|NR worker|failed"
Get-Content build\Release\nr-worker.log | Select-String "\[fg\]|RNSZ|present|window"
Get-Process brp | Select-Object Id,HandleCount,WorkingSet64,CPU
nvidia-smi --query-gpu=memory.used,temperature.gpu,power.draw,clocks.sm `
  --format=csv
Get-Content (Join-Path $env:APPDATA "BetterRemotePlay\health.log") -Tail 10

# package
powershell -NoProfile -ExecutionPolicy Bypass -File tools\package_release.ps1
Get-FileHash build\Release\brp.exe, dist\BetterRemotePlay\brp.exe
```

## 6. Launch-night runbook (exact)

**T-90 (22:30)**

1. Close browsers, downloads, Discord/overlays; disable Windows Game Bar and
   NVIDIA overlay notifications.
2. Power plan High performance; display 2560x1440 @ 240 Hz; HDR per WP-2.
3. Audio device and controller (USB preferred); check battery.
4. PS5: performance mode, game preloaded/updated, Remote Play linked.

**T-60 (23:00)**

5. Start the app; Home -> **Connect**; wait for streaming; press **F11**.
6. HUD check:
   - `FG 120 fps`
   - `DLSS xx ms` (not 0.0)
   - `select 4v` ~100%, `queue 0-1`, `drops 0`, `lost 0`
   - `audio peak` moving, `buf 1`
7. Press **H** to hide the HUD; move the cursor away from the top 124 px.

**T-30 (23:30)**

8. 10-minute soak; watch worker log and health log. If worker <118:
   - Settings -> Performance -> **Safe: FG 2x** (SR off), or **SR only**.
9. Do not open Settings after this unless there is a problem; preset changes
   restart the worker.

**T-0 (00:00)**

10. Play. The chrome fades after 3 s; the FG child covers the client.
11. If the HUD is needed: move to the top strip; the worker hides for 2 s after
    the cursor leaves; it returns automatically.
12. If the session drops: Home -> Connect; if the app hangs, restart; the
    profile and pairing are saved.

**Post-launch (00:00-00:30)**

13. Check `health.log` and `nr-worker.log` every 10 min for worker fps,
    drops/lost, handles/VRAM.
14. If stutter appears: reveal HUD, record `late/stalls`, `FG fps`, `arrival`;
    switch preset; do not change multiple settings at once.

## 7. Fallback presets

| Preset | SR | FG | NR | Target | Buffer | Use |
| --- | --- | --- | --- | --- | --- | --- |
| Launch | Ultra Quality 77% | 2x | off | 1440p | 1 | default, 120 fps |
| Safe | off | 2x | off | 1440p | 1 | SR/readback jitter |
| SR only | Ultra Quality 77% | off | off | 1440p | 1 | FG artifacts / 30 fps |
| Low latency | off | off | off | display | 0 | maximum responsiveness |

## 8. Do-not-touch list

- Reflex driver mode/Sleep: breaks the 240 Hz present clock (measured).
- VSR: removed; do not reintroduce.
- Waitable swapchain / `Present(1)` / `present_covered_` timer: current pacing.
- Decoder thread and compressed queue: no inline decode on the RTP thread.
- NeuralScreen worker ABI and FG multiplier >2 as a "smooth" claim.
- No rewrite after 18:00.

## 9. Open decisions for the morning

1. Default preset: Launch (SR+FG) or Safe (FG only)?
2. HDR: SDR safe, or test/require HDR?
3. Audio: TV/HDMI, DualSense jack, or USB headset?
4. Controller: USB or Bluetooth for the session?
5. Bitrate: 50 / 80 / 100 Mbps (network test decides)?
6. Preset buttons + health log: implement (recommended) or docs only?
