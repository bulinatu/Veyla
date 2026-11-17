# End-to-end audit and rewrite map

This is the result of a full pass over the client with frame probes, DXGI
present statistics, the DLSS FG worker log and GPU measurements. It separates
what was actually broken from what is architectural debt, and says which parts
are worth a rewrite instead of another patch.

## Fixed in this pass

| Area | Finding | Fix |
| --- | --- | --- |
| Input | HID enumeration ran on the presentation thread every 2 s when no controller was attached: a **64 ms stall** every two seconds. | Permanent pad thread; UI reads snapshots. |
| UI | Every DirectWrite label rebuilt its layout every frame; gradients and clip geometries were recreated; D2D layers used `InfiniteRect()`. | Layout / gradient / geometry caches, bounded layers, 10 Hz HUD text. |
| Presentation | A software timer and `Present(1)` both paced the loop; phase drift produced periodic 8.3 ms frames. | Waitable swapchain; one vblank-aligned clock. |
| Resize | `ResizeBuffers` omitted the waitable flag and failed with `E_INVALIDARG`; fullscreen/maximize could never resize. | Pass the creation flags; recreate the swapchain as a fallback. |
| Fullscreen + Reflex | The NVAPI driver low-latency mode, with or without latency markers, capped the loop at **225 fps** and produced **56+ missed refreshes/s**; `NvAPI_D3D_Sleep` added ~4 ms/frame. | Reflex mode rejected (setting stays off); markers implemented but unused. Vblank pacing is the low-latency path. |
| DLSS FG | 6x/4x/3x displayed 165/150/140 fps, not 360/240/180. The worker caps at ~100-120 generated fps and the result was severe judder. | 2x default (stable 120 fps), live worker fps readout and warning. |
| Video cadence | Wall-clock grid released frames early, producing 3/4/5-refresh selections. | Tick-phase gate: frame every fourth vblank at 240/60; one-frame cushion; no early release. |
| Decode | hw transfer + swscale ran inline on chiaki's RTP receive thread; a slow frame delayed socket reads. | Dedicated decode thread with a bounded compressed-frame queue. |
| Audio | Opus resampler recreated per 10 ms packet. | One-time resampler, reused PCM buffer. |

Verified numbers on the reference machine (RTX 4090, 2560x1440 @ 240 Hz):

- Fullscreen, FG off: `screen 4v 100%`, `present misses +0`, frame work ~0.2 ms.
- Windowed: `select 4v 100%`, `screen 4v ~90%` (DWM composition), no misses.
- FG 2x: worker 119-120 fps consistently.
- FG 4x/6x: worker 150-165 fps and visible judder.

## Remaining hot spots

### 1. Video path is CPU-centric (biggest rewrite)

Today: hardware decode → `av_hwframe_transfer_data` to system memory → swscale
to RGBA on the decode thread (measured **4.5-5.5 ms/frame**, peaks 7-8 ms) →
8 MB `UpdateSubresource` upload each frame → shader upscale.

A GPU-native path would decode to NV12/P010 textures, run chroma/colour
conversion and upscaling in the presenter shader, and hand the same texture to
DLSS/NR. It removes the CPU conversion and upload, cuts a frame of latency,
makes HDR exact, and frees the decode thread. This touches `media/decoder`,
`media/pipeline` and every consumer that assumes RGBA. **Rewrite, not a patch.**
Phase it: NV12 presenter first, then DLSS SR on the native texture, then NR.

### 2. NR worker protocol copies RGBA through shared memory

NR receives/returns 8 MB RGBA buffers; NR-after-SR additionally does a GPU
readback per frame. That is 16 MB+ of copies per frame and a hard sync point.
Shared D3D11 textures (or a shared NT handle) would remove both copies. The
worker's protocol already uses shared-memory magic handshakes, so the change is
contained but requires the worker side. **Rewrite of the worker ABI.**

### 3. DLSS SR runs on the presentation thread

`NvDlssSr::Process` uploads, submits and waits on a D3D12 fence synchronously
inside `App::Frame`. It is 0 ms when disabled, but with SR on it can consume a
significant part of the 4.17 ms budget. A two-slot worker (as NR already has)
would keep the UI at full rate. **Improve with a worker; rewrite if combined
with #1.**

### 4. Frame generation ceiling

The third-party `vngx.dll` worker generates ~100-120 fps of output at 1080p with
flat-depth guides. 2x is the only cadence that maps cleanly to 240 Hz. Owning a
presenter/FG path would require real depth and motion vectors; without them the
heuristic is the ceiling. Keep all multipliers selectable, but the UI now states
the measured reality. **Do not advertise 4x/6x as smooth on this content.**

### 5. Audio/video sync has no shared clock

Video is scheduled on the display grid; audio runs on the WASAPI clock, with a
fixed 20 ms buffer. There is no PTS and no feedback between them. Lip sync is
close on LAN but can drift on a loaded link. A proper fix uses the audio clock
to schedule video presentation. **Rewrite of the presentation timing model.**

### 6. Session start/stop is synchronous on the UI thread

`Streaming::Start` / `Stop` call chiaki session setup and teardown on the UI
thread. Probe and pairing are already threaded; the session itself is not. A
session state machine on its own thread would remove the remaining hitch on
Connect/Disconnect. **Improve (contained).**

### 7. Legacy NVIDIA VFG runs on the presentation thread

`NvVfx::PushFrame` runs GPU work and a flush inline. It is the legacy path; DLSS
FG is isolated in a worker. Either move VFG to a worker or treat it as
diagnostic-only. **Improve/retire.**

### 8. Robustness

Device-removed / `EndDraw` failures / worker death currently degrade to a frozen
surface. Add a device-loss path that recreates the presenter and restarts the
worker. **Improve.**

## What is fine as-is

- Custom Win32 frame, DWM corners, DPI handling.
- UI text rendering and the settings/overlay structure; work per frame is now
  ~0.1-0.3 ms.
- Pad input (250-1000 Hz thread) and DualSense output.
- PSN/console discovery and the profile format.
- The FFmpeg/chiaki session layer; only the decode output handoff needs #1.

## Suggested order

1. **Keep the current build stable** (2x FG, Reflex on, fullscreen) and collect
   user feedback.
2. **GPU-native decode/colour** (#1). Biggest latency and CPU win.
3. **NR shared-texture ABI** (#2), then **DLSS SR worker** (#3).
4. **Audio-clock video scheduling** (#5).
5. **Session thread** (#6) and device-loss recovery (#8).

Each step has a measurable acceptance test with the probes in
`docs/22-performance.md`: fullscreen `screen 4v 100%`, `present misses 0`,
`work p99` below ~2 ms, and the FG worker at its requested clean rate.
