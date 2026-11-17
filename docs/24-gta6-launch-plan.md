# GTA 6 launch plan - BetterRemotePlay

Goal: at 00:00 the client must feel like a native PC game: steady cadence,
no stutter, audio in sync, controller features working, and a one-click fallback
if anything misbehaves. This is the plan for the day; no risky rewrite is
allowed after the code freeze.

## 1. Acceptance criteria (what "ready" means)

- DLSS FG worker reports **120.0 fps ±1** for 10 minutes at 2x, with DLSS SR
  feeding it at 1440p.
- Source cadence: `select 4v` >= 98%, queue 0-1, drops 0, lost 0.
- No crash, hang or device-loss restart for a 2-hour soak.
- VRAM and handle count flat over the soak (handles +<50, VRAM +<400 MB).
- Audio: 0 underruns, no crackle; dialogue lip-sync acceptable.
- Controller: buttons, sticks, touchpad, rumble and adaptive triggers work;
  wired and wireless both tested.
- Network: 0 lost packets / 0 decoder drops over 10 minutes at the chosen
  bitrate, and a cable-pull reconnect succeeds in under 20 seconds.
- Fullscreen 240 Hz, alt-tab and back returns to streaming without resize bugs.
- Package has no personal data, hash verified, known-good copy archived.

## 2. Verified baseline (evening before)

- RTX 4090, 2560x1440 @ 239/240 Hz, PS5-580 on 192.168.1.21.
- DLSS SR Ultra Quality (77%) + DLSS FG 2x, 2560x1440: worker **120.0 fps**,
  HUD `DLSS 0.7 ms   FG 120 fps`, source `4v 99-100%`.
- Parent UI is covered by the FG child; its source tick costs 5-6 ms
  (SR upload + submit + 14 MB readback + worker submit). The FG worker is
  independent and holds 120.
- Chrome revealed: both swapchains present, worker temporarily ~105 fps;
  returns to 120 when the cursor leaves the header. This is expected.
- Reflex off (driver mode breaks the 240 Hz present clock).
- VSR removed. DLSS NR available but costs 7-18 fps of FG output; default off.
- Package: `dist\BetterRemotePlay-0.3-standalone.zip`, 571.6 MB, hash checked.

## 3. Risk register

### P0 - must be resolved or have a fallback before midnight

1. **SR+FG feed stability.** The source tick is 5-6 ms on 25% of frames. The
   worker currently holds 120, but a fast GTA 6 scene may expose jitter.
   - Test: 10-min 1440p SR+FG soak with a fast game; watch worker fps and
     `arrival` outliers.
   - If worker < 118 sustained: switch to FG 2x without SR, or DLAA + FG at
     1080p, or SR only. Do not rewrite the feed thread on launch day.
2. **GTA 6 frame-rate mode.** If the game runs at 30 fps, the console still
   encodes 60 fps with duplicates. FG on duplicated frames can produce
   artifacts.
   - Test both performance and fidelity modes.
   - If 30 fps: FG off (SR or raw); motion will be 30 fps but clean.
3. **HDR.** The FG worker presents format 28 (RGBA8, SDR). HDR through the
   worker is unproven.
   - Test HDR on/off for color, black level and UI.
   - Safe default: **SDR**. If HDR is required, use SR-only (no FG) and verify.
4. **Audio sync and device.** The video chain adds 1-2 frames; audio has a
   20 ms WASAPI buffer.
   - Test TV output vs DualSense jack vs USB headset, 10/20/30 ms buffer.
   - Pick the lowest buffer with 0 underruns. Do not change mid-session.
5. **Network / PSN launch load.** Local Remote Play does not need PSN auth, but
   the console may be downloading the game or rest-mode waking.
   - Preload/pre-install. Wired Ethernet on both ends. Test bitrate 50/80/100
     Mbps; pick the highest with 0 lost/drops.
6. **Long-session stability.** FG/SR/NR workers and frame queues must not leak.
   - 2-hour soak with HUD stats and Task Manager/`nvidia-smi`; check handles.

### P1 - polish if time allows

7. **Preset buttons**: "Launch (SR+FG 2x)", "Safe (FG 2x)", "SR only",
   "Low latency (raw)". One click, saved to profile.
8. **Health log**: every 30 s write one line with worker fps, frame p99,
   late/stalls, queue/drops, decode ms, process handles and VRAM.
9. **Reconnect flow**: verify Home -> Connect after a network drop; the
   session thread and device-loss restart already handle the blockers.
10. **Overlay noise**: disable Windows Game Bar notifications and the NVIDIA
    overlay hotkey for the session.

### P2 - only if everything above is green

11. Move the SR readback + FG submit off the presentation thread (a real
    worker). High risk; not for launch day unless the soak proves it necessary.
12. DLSS NR + SR + FG combo tuning (costs generated fps).
13. Reflex marker experiment (do not enable; it breaks pacing).

## 4. Schedule (launch day)

| Time | Work |
| --- | --- |
| 09:00 | Snapshot: hash exe/ZIP, copy `session.json` to a backup, note current preset. |
| 09:15 | Boot and 10-min SR+FG soak; record worker fps, late/stalls, queue/drops. |
| 10:00 | GTA 6 frame-rate mode test (60 vs 30); HDR on/off test. |
| 11:00 | Preset buttons + health log (small, contained changes); rebuild. |
| 12:30 | Audio matrix: devices x buffers; pick one; lip-sync test. |
| 13:30 | Controller matrix: USB vs Bluetooth, rumble, triggers, touchpad, battery. |
| 14:30 | Network matrix: bitrate, cable pull, console rest/wake, reconnect. |
| 16:00 | 2-hour soak with the chosen preset and health log; watch VRAM/handles. |
| 18:00 | **Code freeze.** Only config and packaging after this. |
| 18:30 | Final package, clean-profile test, ZIP extraction test, hash. |
| 19:30 | Print/attach runbook; archive known-good EXE + ZIP + profile. |
| 22:30 | Desktop prep: power plan, overlays off, 240 Hz, HDR decision, audio, pad. |
| 23:00 | Launch app, Connect, F11, verify HUD; 10-min soak. |
| 23:45 | Final check; HUD off; play |
| 00:00 | GTA 6. Monitor the first 30 minutes; fallback preset ready. |

## 5. Test matrix

| Area | Cases |
| --- | --- |
| Video | 1440p fullscreen; windowed 1600x900; SR off/DLAA/Quality/Ultra Quality; FG off/2x; NR off/on; 30 vs 60 fps content |
| HDR | SDR; HDR10 on; FG on/off; black level, UI color, tone mapping |
| Audio | TV/HDMI, DualSense jack (USB), USB headset; buffer 10/20/30/40 ms; underruns; dialogue sync |
| Controller | DualSense USB and Bluetooth; 250/1000 Hz; rumble, adaptive triggers, touchpad, PS button, battery |
| Network | 50/80/100 Mbps; 10-min stability; cable pull; Wi-Fi vs Ethernet; console rest/wake |
| Stability | 2-hour soak; alt-tab; chrome reveal; settings changes; suspend/resume |
| Recovery | App restart mid-session; GPU device-loss path; worker failure fallback |

## 6. Launch-night runbook

1. Close browsers/downloads/overlays; power plan High performance; display
   2560x1440 @ 240 Hz.
2. Confirm audio device and controller (prefer USB for latency).
3. Start BetterRemotePlay; Home -> Connect; press F11.
4. HUD check for 30 s: `FG 120 fps`, `DLSS` line present, `select 4v` ~100%,
   `queue` 0-1, `drops 0`, `lost 0`, `audio peak` moving.
5. Press H to hide the HUD. Move the mouse away from the top strip; the FG
   child covers the client.
6. If stutter: reveal the HUD and check `late/stalls`, `FG fps`, `arrival`.
   Then Performance -> preset:
   - FG 2x (no SR) if SR/readback is late.
   - SR only if FG artifacts.
   - Raw low-latency if the GPU is loaded.
7. If the session drops: Home -> Connect; if the app hangs, restart it (profile
   is saved); if the GPU device is lost, the app auto-restarts once.
8. If the picture is 30 fps and FG shows artifacts: FG off; keep SR or raw.

## 7. Fallback presets (to implement as buttons)

| Preset | SR | FG | NR | Target | When |
| --- | --- | --- | --- | --- | --- |
| Launch | Ultra Quality 77% | 2x | off | 1440p | default, verified 120 fps |
| Safe | off | 2x | off | 1440p | SR/readback causes jitter |
| SR only | Ultra Quality 77% | off | off | 1440p | FG artifacts or 30 fps content |
| Low latency | off | off | off | display | maximum responsiveness |

## 8. Do not touch on launch day

- Reflex driver mode/Sleep (breaks the 240 Hz present clock).
- VSR (removed; do not reintroduce).
- Swapchain waitable/pacing logic.
- Decoder thread ownership.
- The closed NeuralScreen worker ABI.
- Any rewrite after 18:00.

## 9. Open decisions for the morning

1. Default preset: Launch (SR+FG) or Safe (FG only)?
2. HDR: SDR safe, or test/require HDR?
3. Audio: TV/HDMI, DualSense jack, or USB headset?
4. Controller: USB or Bluetooth for the session?
5. Bitrate: 50, 80 or 100 Mbps (network test decides)?
6. Do you want the preset buttons in the app, or only documented?
