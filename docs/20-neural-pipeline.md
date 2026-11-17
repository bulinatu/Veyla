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
  the 1080p stream first when the mode asks for fewer pixels. DLAA cannot
  upscale a 1080p source, so choosing it turns SR off with a note.
- **Legacy settings page** (between Neural and Audio): RTX VSR and the Neural
  Ultra preset, with "not maintained any more, but still functional".
- **Overlay** rebuilt as categories like the settings screen: Stream / DLSS /
  Neural / Legacy, sharing the same widgets (sliders included).

## DLSS Frame Generation (the honest answer)

Having `nvngx_dlss.dll` and `nvngx_dlssnr.dll` does not give Frame Generation
for free. DLSS-G is a swapchain-level feature:

- it needs two **consecutively rendered** frames with depth and motion vectors,
  plus a swapchain it can present into (it inserts generated frames between the
  real ones);
- it is integrated with Reflex latency markers and expects the rendering app to
  own the present queue;
- for a video client the "real frames" are decoded frames with no depth or
  motion vectors, which is exactly why the video path uses **NVIDIA VFG**
  (Video Effects Frame Generation) instead - it is the API meant for video and
  it is already shipped;
- VFG and NR are mutually exclusive today because NR needs 13-18 ms per frame
  and generated frames arrive at 120/s.

A real DLSS-G path would mean owning a D3D12 swapchain in the app, feeding it
two frames + guides, and handling UI/HDR around generated frames. That is a
separate project, not a checkbox; it is on the list, after the current chain is
live-verified.
