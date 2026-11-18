# 27 - Colour grading and sharpening

Implemented on 18 Nov 2026. One GPU pass, shared by every output path,
including DLSS Frame Generation (which only accepts CPU RGBA).

## 1. Where it runs

```
stream frame (1080p RGBA)
  -> DLSS SR / DLAA (optional, D3D12)          [src/media/nv_dlss_sr.cpp]
  -> Presenter post-process (D3D11)            [src/media/pipeline.cpp]
       upscale -> CAS sharpen -> colour grade -> HDR/SDR encode
  -> parent presenter                                  (window / screenshots)
  -> OR grade + readback -> FG worker (RGBA8)           (frame generation)
```

The FG worker cannot sample a texture; it reads CPU RGBA from shared memory.
So when frame generation is on, the same shader runs into an offscreen RGBA8
target which is copied to a staging texture and read back:
`Presenter::EnhanceViewReadback` (SR output) or
`Presenter::EnhanceFrameReadback` (raw stream). When frame generation is off,
`Presenter::Render` draws the shader straight to the backbuffer. Both use
`Presenter::SetPsParams` + `Presenter::DrawFullscreen`, so the pixels are the
same by construction.

The old `NvDlssSr::ReadbackLast` call in the FG path is gone: the presenter
copy is the readback (one copy instead of two).

## 2. Controls

| Control | Range | Default | Notes |
| --- | --- | --- | --- |
| Colour controls | Off / On | Off | off = sharpening only |
| Sharpening | 0-100 % | 35 | CAS, 1-texel cross, clamped to the local min/max |
| Exposure | -100..100 | 0 | +/-1 stop |
| Contrast | 50..150 | 100 | pivot 0.18 linear (18 % grey) |
| Highlights | -100..100 | 0 | mask above 45 % luma, +-35 % |
| Shadows | -100..100 | 0 | mask below 22 % luma, +-45 % |
| Saturation | 0..200 | 100 | Rec.601 luma preserved |
| Vibrance | 0..100 | 0 | extra saturation on low-chroma pixels |
| Hue | -180..180 deg | 0 | YIQ rotation, luma preserving |
| Temperature | -100..100 | 0 | red/blue gains, luma normalised |
| Tint | -100..100 | 0 | green/magenta gain |
| Gamma | 80..120 | 100 | display-space power |

Presets: Neutral, Vivid, Cinematic, Warm, plus Reset (all of the above).

Where: Settings > **Color** tab, and the in-stream overlay (Tab > **Colour**)
for live tuning without disconnecting. Values persist in
`%APPDATA%\BetterRemotePlay\session.json` with the `color_*` and `sharpness`
keys; profiles without them load neutral.

## 3. The pass, exactly

Order inside the pixel shader (`src/media/shaders.h`, `PSMain`):

1. upscale (bilinear / Catmull-Rom / Lanczos3 / FSR1 EASU, or a copy when the
   source is already at the target size);
2. contrast-adaptive sharpening in the source's own space;
3. tone mapping / HDR encode (existing code, unchanged);
4. colour grade when "Colour controls" is On:
   display -> linear, exposure, white balance, contrast, highlights, shadows,
   YIQ hue rotation, saturation + vibrance, back to display, gamma.

Sharpening before tone mapping keeps the centre and its four neighbours in the
same space (no second render target). Saturation 0 produces Rec.601 luma
greyscale. The pass is skipped entirely when sharpening is 0 and colour is
off; the FG path then still does the copy it always did.

HDR: with PQ in and PQ out the conversion uses the ST.2084 EOTF normalised so
1.0 == 100 nits; SDR paths use gamma 2.2. The FG worker is always fed SDR
(`format=28`), so with HDR on the worker path tone-maps first and the picture
on screen through FG is SDR while the parent window is HDR. SDR is the
recommended launch setting.

## 4. Measured

Machine: RTX 4090, 2560x1440 @ 240 Hz, PS5-580 wired, stream 1080p60 HEVC,
DLSS SR Ultra Quality + FG 2x at 1440p.

| Test | Result |
| --- | --- |
| FG worker, 10 min, colour off, sharpness 35 | `[fg] displayed 120.0 FPS (2x)` |
| Source cadence | `select 4v 100%`, `arrival outliers 2`, `queue 0-1`, `drops 0`, `lost 0` |
| Frame work | `work p99 5.7-6.1 ms` (budget 16.7 ms), `stalls 6/480` |
| Decode | 4.4 ms avg / 5.3 ms max |
| Sharpening 0 -> 95, same scene | Laplacian variance **+97.0%** at 2560x1440 (luma delta -0.19%) |
| Saturation 0 | mean saturation **0.000** |
| Hue +30 | visible rotation, saved value correct; numeric check inconclusive because the game scene changed between captures |
| Profile round-trip | restart restored sharpness/colour values |

The sharpening number is 0 -> 95 % because the slider test dragged to ~95.
The metric is the variance of a 4-neighbour Laplacian on luma, computed on the
full-resolution screenshot (`tools/measure_frame.ps1`).

## 5. Caveats / honest limits

- **Hue is a YIQ rotation, not HSV.** It preserves luma but the perceived hue
  shift depends on the colour: a +30 deg setting can look like ~40 deg on
  teals and less on skin. That is the price of preserving brightness; HSV-based
  rotation would shift luma. The slider is monotonic and wraps at the ends.
- **Sharpening runs before tone mapping.** On HDR material the sharpen amount
  therefore acts on PQ values, not on the tone-mapped image.
- **FG gets SDR.** HDR through the worker is not supported by the closed
  worker (RGBA8 only); the tone map makes it correct SDR instead of broken.
- **No 3D LUT / curves yet.** These are 11 scalar controls, not a grading
  suite; a LUT would be the next step and is documented as future work.
- Cost: the grade itself is one full-screen pass; on this GPU it is below the
  timer resolution of the frame probe. The readback is the same copy the FG
  path already paid before this change.

## 6. Files

- `src/media/pipeline.h`, `pipeline.cpp`: `EnhanceParams`, `SetEnhance`,
  `EnhanceFrameReadback`, `EnhanceViewReadback`, `SetPsParams`,
  `DrawFullscreen`, grade textures and staging.
- `src/media/shaders.h`: `gColor0..gColor2`, CAS, colour block.
- `src/session/profile.h`, `profile.cpp`: 12 new keys, defaults, clamps;
  also fixes `hdr` not being loaded (it was saved but ignored).
- `src/app/app.h`, `app.cpp`: Colour tab, overlay Colour tab, widget ids
  120-136, `ApplyEnhance`/`TickEnhance` (apply on change, save 1 s later),
  HUD `ENH xx ms` when FG is active.
- `tools/measure_frame.ps1`: screenshot metrics used for the numbers above.
