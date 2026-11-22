# 34 - Handover: pacing, latency and neural order (2.4.6 -> 2.4.7)

Status: 2.4.6 is released (`639f4ba`). 2.4.7 is in progress: the version bump
is uncommitted and the release build was still running when this was written.
Nothing below is released until the checklist at the end is done.

## 1. Where things stand

| Item | Value |
|---|---|
| Released | 2.4.6, commit `639f4ba`, standalone `FB7A2C26...8A318DE` |
| Update 2.4.5 -> 2.4.6 | `E1024D92...C0D6` (verified: version, 0 markers, 0 diffs) |
| In progress | 2.4.7 (version bump in 5 files, uncommitted) |
| Tests | 82 / 82 pass on the last build |

Commits since 2.4.6 (newest first):

- `7a7d3b2` NR runs inside the DLSS-G worker when NR and FG are both on.
- `7b0f9aa` Motion safety is RIFE-only; dimmed for DLSS-G.
- `45b7c3a` SR and NR synchronous; frame-age probe.
- `7e7209d` Non-blocking SR/NR readback (later reverted to sync in `45b7c3a`).
- `48d2abc` Time-based playout phase, queue PI trim, no spin.
- `3f8b36f` Adaptive buffer ceiling, drop hysteresis, consistent clamp.

## 2. Release 2.4.7: remaining steps

The background command bumped the version in `CMakeLists.txt`,
`src/next/shell/next_app.cpp`, `tools/package_release.ps1`, `ui/Cargo.toml`
and `ui/Cargo.lock`, then rebuilt the engine, shell and tests, and rebuilt the
Rust release. Check the result before going on:

1. `git grep -n "2.4.6" -- CMakeLists.txt src ui tools ":!docs"` returns nothing.
2. `brp_next_tests.exe` prints `ALL PASS`.
3. `build-perf\Release\brpnext-engine.exe` and `ui\target\release\brpnext-ui.exe`
   have fresh timestamps. If the Rust exe is old, kill `brpnext*` and rebuild.
4. `powershell -File tools\package_release.ps1` (about 922 MB, run in background).
5. `tools\make_update.ps1 -BaselineZip dist\BetterRemotePlay-2.4.6-standalone.zip
   -NewFolder dist\BetterRemotePlay -Version 2.4.7
   -Out dist\BetterRemotePlay-2.4.6-to-2.4.7-update.exe`.
6. Verify the patch: expand the 2.4.6 zip into `build\patchtest247`, run
   `update.exe --no-launch`, check `version=2.4.7`, `markers=0`,
   `files_differ=0`. Shell PE subsystem must be 2 (GUI).
7. Smoke: launch `dist\BetterRemotePlay\brpnext.exe`, check it stays up, close it.
8. Add the 2.4.7 hashes to `docs/29-handover.md` and `docs/V1_RELEASE_NOTES.md`.
9. Commit with `release: 2.4.7 - NR inside the DLSS-G worker`.

## 3. What changed in the engine (for release notes)

- **Playout:** frame phase follows real elapsed time, and a PI loop trims the
  consumer rate on queue depth. Queue setpoint is buffer+1 for buffer >= 1.
- **Buffer 0:** frames are delivered in order; only a backlog past four frames
  is trimmed. Previously the whole queue was discarded every tick.
- **SR and NR** are synchronous in the FG path. Frame age at presentation at
  buffer 1 dropped from 84 ms to 37 ms.
- **NR in the worker:** when NR, FG and DLSS-G are on, the DLSS-G worker runs
  NR on the frames it presents (`SetNeuralBypass(false)`). The engine does not
  run NR separately in that case. Measured loop p99 went from about 9-16 ms to
  2.3 ms; frames over 10 ms per second went from 121/151 to 1/141.
- **Motion safety** is effective only on RIFE. On DLSS-G the row is dimmed,
  like RIFE model and Blend are on DLSS-G.
- **Probe** (`BRP_NEXT_FRAME_PROBE=1`) prints queue, trim, wait and frame max,
  and frame age.

## 4. Decisions and reasoning

- **Order SR -> FG -> NR.** The user asked for this order and it matches
  NVIDIA's public description of DLSS 5 as the final stage. The engine cannot
  apply NR to frames that the closed DLSS-G worker generates, unless NR runs
  inside the worker. That is what 7a7d3b2 does. The user confirmed visually
  that the head-disappearing artefact is gone with this setting.
- **Fast-motion bypass was removed.** It was not measured to help and added
  frame stalls (55-68 ms frame max during bypass seconds).
- **Pipelined SR/NR helpers were removed** (`ProcessPipelined`,
  `ReadbackCompleted`) after measuring that sync was better for latency.

## 5. Open issues

1. **Latency at buffer 1 is about 84 ms of frame age.** Two frames of queue
   (~66 ms) are the main part. Buffer 0 gives low latency but a weaker cadence
   (p99 ~40 ms, sporadic 50+ ms outliers). This is a user choice, not a bug.
2. **Freezing on fast pans** was not reproduced. Likely causes, in order:
   the queue at buffer 0, the DLSS-G worker stalling on large scene changes,
   and bitrate spikes causing loss and keyframe waits. The probe logs loss and
   drops per second, so a repro with a timestamp can be correlated.
3. **Setting in use:** DLSS-G multiplier is 4x. Artefacts are more frequent
   at 3 generated frames per real frame. 2x is recommended.
4. **Input-to-photon latency** is not measured: the engine side cannot see
   the screen. An external timer (camera or on-screen counter) is needed.
5. **DLSS-G worker phase wobble** at 60 -> 240 Hz comes from the closed
   presenter. Not fixable in code. G-SYNC is the only untested option.
6. **Network jitter** was never tested with real jitter. Sessions used a clean
   link (33 ms constant jitter, 0% loss).
7. **Debug leftover:** `Frame()` in `next_app.cpp` writes
   `build\engine-frame.log` every 300 frames through a hardcoded path. Harmless,
   but should be removed.

## 6. Test harness (for the next measurement)

Sessions were run on an isolated copy so the user's settings were not touched:

- Hardlink copy of `dist\BetterRemotePlay` (only the engine exe replaced).
- `APPDATA` pointed to a folder holding copies of `next-settings.json` and
  `session.json`. The engine honours `%APPDATA%` from the environment.
- Engine started via `cmd /c "set APPDATA=...&& set BRP_NEXT_FRAME_PROBE=1&&
  engine --engine --bridge-port N 2> log"`, then driven over TCP:
  `connect` -> `watch` -> `disconnect_now` -> `shutdown`.
- Probe lines are summarised per second; the first 3 seconds are skipped.

The harness lives under `%LOCALAPPDATA%\Temp\opencode\` and was deleted after
each session, so recreate it when needed.

## 7. Pitfalls seen in this session

- In PowerShell, `"\n"` inside double quotes is not a newline. A `IndexOf("\n}\n")`
  returned -1 and duplicated a source file. Use `[char]10` or line arrays.
- `Start-Sleep` and polling of background output are not useful; wait for the
  completion notification instead.
- Engine stderr is discarded by the shell; use the `cmd /c ... 2> log` form to
  capture the probe.
- Rebuild and verify the exe timestamp after every source edit. A build that
  printed nothing once left the old exe in place.
