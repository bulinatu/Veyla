# 20 - The neural pipeline order, Reflex, and the settings refresh

Status date: 2026-11-17. This is the order the project asked for, and it is the
one the app implements.

## Order

```
1. video (the console's 1080p stream - "game rendering")
2. (Ray Reconstruction: not used by this app)
3. DLSS Super Resolution / DLAA
4. DLSS Frame Generation / Multi Frame Generation
5. DLSS 5 Neural Rendering
6. display output
```

### What that means in code

With the **NVIDIA order** selected (`Settings > Neural > Pipeline`), the frame
loop is:

- decode 1080p -> `NvDlssSr` (input scaled by the DLSS mode) -> 1440p GPU frame
- `NvDlssSr::Readback` copies that 1440p frame back to CPU RGBA
- the NR thread runs the NGX worker at 2560x1440 (the target size)
- the newest NR result is presented with `source_final = true`

So NR is a **post-upscale** stage here, exactly as in the order above. It needs
a GPU->CPU readback because the vendor worker speaks RGBA.

The alternative, kept as `Upstream: NR -> SR`, runs NR on the 1080p stream and
lets DLSS SR upscale its result afterwards. It is cheaper (NR at 1080p), but it
is not the NVIDIA order.

### Limits

- NR is capped by the worker at 2560x1440, so the NVIDIA order works at 1080p
  and 1440p targets. A 2160p target leaves NR unavailable (the settings page
  says why) rather than silently running it at the wrong size.
- Frame Generation stays exclusive with NR for now: VFG wants 120 frames/s and
  the NR worker costs 9-18 ms per frame. The order is documented as SR -> FG ->
  NR; running both is a future step that needs an NR worker fast enough for the
  generated cadence.
- When DLSS SR is off, NR runs at the stream size and the regular upscaler
  finishes the frame (there is no target-resolution frame on the CPU to feed).

### Measured on this machine (RTX 4090)

| Stage | Size | Time |
|---|---|---|
| NR, full network, 1 pass | 1920x1080 | 8.8-10.5 ms |
| NR, full network, 1 pass | 2560x1440 | 13.4-18.2 ms |
| DLSS SR, 1080p -> 1440p | - | 1.9-2.8 ms |
| DLSS SR, 950p -> 1440p (66% mode) | - | 1.8-2.1 ms |
| GPU->CPU readback, 1440p | 14.7 MB | ~2-3 ms |

The chain still presents at the display cadence because NR runs on its own
thread with a one-frame queue; when the worker is late the last result stays on
screen and the counter shows the skipped source frames.

## NVIDIA Reflex

`Settings > Performance > NVIDIA Reflex`: **Off / On / On + Boost**.

This is the real NVAPI low-latency interface, not a re-label of the frame
buffer:

- `nvapi64.dll` is loaded at runtime and the entry points are resolved through
  `nvapi_QueryInterface`:
  - `NvAPI_Initialize` 0x0150E828
  - `NvAPI_D3D_SetSleepMode` 0xAC1CA9E0
  - `NvAPI_D3D_Sleep` 0x852CD1D2
- On/Boost calls `NvAPI_D3D_SetSleepMode` with the documented
  `NV_SET_SLEEP_MODE_PARAMS_V1` layout (44 bytes, version `44 | (1 << 16)`);
  Boost sets `bLowLatencyBoost`.
- Each presented frame calls `NvAPI_D3D_Sleep` right before the frame is
  submitted.

Reflex is not a stage in the list above: it is CPU/GPU latency management and it
works together with Frame Generation. On this machine the driver accepts it
(`[reflex] NVAPI loaded`, `[reflex] Reflex on + boost`).

## Settings refresh in this batch

- **Purple theme.** The palette moved from blue to violet (`#B072FF` accent,
  violet-tinted panels and background); the startup animation uses the same
  tokens, so it changed with the rest.
- **Sliders** instead of fixed numeric dropdowns for Strength, Local tone,
  Structure and Character/skin, plus a **Render scale** slider (50-100%).
  Sliders apply on release, so dragging does not send one RNSZ per pixel.
- **Render scale and Stack passes are independent.** The pass count no longer
  resets when the scale changes, and the control is always enabled.
- **DLSS mode** now mirrors NVIDIA's ladder: Ultra Performance 33%, Performance
  50%, Balanced 58%, Quality 66%, Ultra Quality 77%, DLAA 100%. The percentage
  is the resolution DLSS receives relative to the output; the app downscales
  the 1080p stream first when the mode asks for fewer pixels. **DLAA** is
  DLSS at 100%: the feature is created with input == output (the frame
  size), the network does the anti-aliasing and the regular upscaler takes
  the result to the target. It no longer falls back to FSR.
- **Legacy settings page** (between Neural and Audio): RTX VSR and the Neural
  Ultra preset, with "not maintained any more, but still functional".
- **Overlay** rebuilt as categories like the settings screen: Stream / DLSS /
  Neural / Audio / Controller / Legacy, sharing the same widgets (sliders
  included) and applying every change live while the stream runs.

## DLSS Frame Generation (implemented)

`Settings > Performance > DLSS Frame Generation`: **Off / 2x / 3x / 4x / 5x / 6x**.

This is the real DLSS-G runtime, not a re-label of VFG:

- the vendor NGX worker loads `nvngx_dlssg.dll` (its own strings: `[fg] ...
  Init_Ext`, `[fg] 2x enabled at 1920x1080, format=28; flat depth and estimated
  motion (experimental)`);
- our client asks the worker to present itself (`WNDO`, capturable window),
  sends frames with the FG flag bits (enabled, multiplier in bits 9-10/15),
  and makes the worker's window the video surface: it is re-parented into the
  app window and hidden while the overlay/settings are open;
- no depth or motion vectors exist in a video stream, so the worker feeds the
  FG runtime flat depth and estimated motion - the same experimental path the
  vendor uses for video. This is why the log calls it "experimental
  flat-depth guides".
- Live result: `[fg] displayed 120.0 FPS (real + generated, 2x)` with the
  stream at 60 fps, while NR stays enabled inside the worker (`direct feature
  18 ready`, NR then FG).
- **Up to 6x.** The worker alone reports a 2x multiplier ceiling on RTX 40.
  The `DLSSG SM86` mod (Aion2 package: `winmm.dll` proxy + `dlssg_sm86\\`
  backend/kernels/runtime, `MaxGeneratedFrames=6`) raises it: the app injects
  the proxy into the worker right after CreateProcess, the worker then logs
  `the runtime reports a 6x multiplier ceiling` and runs
  `[fg] 6x enabled ... displayed ~170 FPS (real + generated, 6x)`.
  The proxy is user-supplied and lives in `third_party/neuralscreen/native/fg`
  (gitignored); without it the ceiling stays at 2x and the UI simply does
  nothing above it.
- The multiplier the user picks is sent in the frame flags (bits 9-10 plus
  bit 15); the worker steps down if the runtime refuses a value.

Constraints, stated in the UI and here:

- FG needs the worker, so RTX VSR, DLSS SR and NVIDIA VFG are off in this
  mode; the worker owns the display.
- The worker presents at the stream size (1080p); the window is scaled by the
  compositor to our client area.
- Our HUD is covered by the worker window while streaming; opening the overlay
  (Tab) hides it so the UI is usable, and resuming shows it again.
- The pipeline inside this mode is the vendor's NR -> FG, because that is how
  its presenter is built. DLSS SR cannot be stacked under it in this mode.

DLSS-G still is not a post-process on a texture: a home-grown integration
would require our own D3D12 swapchain. The worker path is the real thing
without that rewrite.

### Why VFG still exists

VFG (NVIDIA Video Effects) is the video-specific frame generator: it returns
pixels, so our own presenter keeps the HUD and the overlay. DLSS FG gives the
NVIDIA DLSS-G look at 2x through the worker presenter. They are alternatives,
not two stages.