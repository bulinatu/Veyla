# Final day plan - 09:00-15:00 (19 Nov 2026, EET)

Scope for the day: **butter-smooth 120 fps output through DLSS Frame
Generation**, **customizable sharpening**, **fully customizable colour**
(exposure / contrast / highlights / shadows / saturation / vibrance / hue /
temperature / tint / gamma), an **atomic issue sweep**, and a documented
**router + PC + PS5 network setup**.

Hard stop **15:00**. After 15:00 only packaging, hashes and the archive.

This plan is deliberately ordered so that the riskiest thing (feeding a graded
frame to the closed FG worker) is proven before lunch, and the cheap wins
(UI, presets, health log) can be cut without losing the day.

It supersedes the 09:00-19:30 schedule in `docs/24-gta6-launch-plan.md` for
this day; `docs/24` stays as the launch-night runbook.

**Out of scope today** (documented, not attempted): shared-texture input for
the closed NeuralScreen worker, FG above 2x as a "smooth" claim, 4K streaming,
HDR *through* the FG worker, a GPU-native decode rewrite, Reflex, VSR,
self-update.

---

## 0. Ground rules

- **Nothing fake.** Every control here changes pixels; every number in the
  docs is measured. If a lever turns out to do nothing (e.g. the NGX
  `Sharpness` parameter), it is not exposed.
- **Do not touch**: waitable-swapchain pacing (`main_app.cpp::main`,
  `App::present_covered`), `SetMaximumFrameLatency(1)`, the decoder thread
  ownership (`Streaming::DecodeThreadMain`), the closed NeuralScreen worker
  ABI (`SHM`/`RNSZ`/`WNDO`), NVAPI Reflex mode, VSR.
- One commit per work package. If a test fails, revert that commit - do not
  patch forward under time pressure.
- Snapshot before the first edit and keep the ZIP + EXE + profile from that
  moment until 15:00.

### 0.1 Snapshot (09:00, 10 min)

```powershell
git tag pre-color-2026-11-19
Copy-Item dist\BetterRemotePlay-0.3-standalone.zip dist\known-good-$(Get-Date -Format HHmm).zip
Copy-Item build\Release\brp.exe build\brp-known-good.exe
Copy-Item "$env:APPDATA\BetterRemotePlay\session.json" "$env:APPDATA\BetterRemotePlay\session-backup.json"
Get-FileHash build\Release\brp.exe, dist\BetterRemotePlay\brp.exe | Format-Table Path,Hash
```

Rollback at any point: restore the EXE + profile, `git reset --hard pre-color-2026-11-19`.

---

## 1. Acceptance criteria for 15:00

| Area | Criterion | How it is measured |
| --- | --- | --- |
| Output rate | `[fg] displayed 120.0 FPS (2x)` for 30 min, no line < 118 | `build\Release\nr-worker.log` every 2 s |
| Source cadence | `select 4v` >= 98%, `queue` 0-1, `drops 0`, `lost 0` | `[video]`/`[app]` probes |
| Frame work | `work p99` <= 7 ms, `stalls` <= 8 per 480 | `BRP_FRAME_PROBE=1` |
| Enhance cost | colour+sharpen adds <= 0.4 ms GPU and <= 1.2 ms to the source tick | `enh_ms` probe (new) + A/B |
| Sharpening | 0 -> 100 raises Laplacian variance >= 25% | `tools\measure_frame.ps1` |
| Colour | saturation 0 -> mean chroma < 0.03; hue +30 deg -> circular mean +30 +/-5 deg; neutral -> identical to pre-change | same script |
| FG correctness | the graded stream is what the worker presents (screenshot shows the grade) | screen capture while covered |
| Network | RTT avg <= 1 ms, 0% loss, chosen bitrate 0 lost / 0 drops for 10 min | `ping`, `[app]`, `[video]` |
| Reconnect | cable pull 5 s -> Home -> Connect <= 20 s | stopwatch + logs |
| Package | `dist\BetterRemotePlay\brp.exe` hash == `build\Release\brp.exe`; ZIP extracts and runs | `Get-FileHash`, clean-profile launch |
| Docs | `docs/26-network-setup.md` + `docs/27-color-and-sharpening.md` filled with real values | read-back |

---

## 2. Verified baseline (evening before) and what changes

| Item | Now | After today |
| --- | --- | --- |
| Preset | DLSS SR UQ 77% + FG 2x, 1440p | same, plus colour/sharpen |
| Worker | 120.0 fps displayed | must stay 120.0 |
| Parent source tick | 5-6 ms (SR submit + 14 MB readback + worker submit) | + enhance pass, target <= +1.2 ms |
| Sharpening | hard-coded 0.35 (0.45 for Lanczos+RCAS), disabled when the source is final | one CAS pass, slider 0-100, works after DLSS too |
| Colour | none | 11 controls, neutral by default |
| FG feed | SR output read back raw; raw path passes CPU RGBA | graded readback (one implementation for both) |
| HDR + FG | PQ bytes handed to an RGBA8 worker (wrong colours) | tone-mapped SDR in the readback + documented |
| Network | defaults | documented, tested, revert list |

---

## 3. Code map (verified at commit `9eda52a`)

### 3.1 Presenter / shader (the natural home for colour + sharpen)

- `src/media/pipeline.h`
  - `PresenterConfig::upscale_mode`, `hdr`, `source_pq`, `source_final`.
  - `Presenter::SetUpscaleMode`, `SetUpscaleTarget`, `SetSourceFrame`,
    `SetSourceView`, `SetSourceFinal`, `Render`.
- `src/media/pipeline.cpp`
  - `PsParams` (line 17): `sharpen, mode, src_w, src_h, hdr, out_w, out_h,
    pad0, con0..con3`.
  - `Presenter::Render` (line 490): `set_params` lambda (538), `set_viewport`
    (575), `draw_fullscreen` (585), first pass (599-607), fit pass (609-627).
  - Sharpen amount today: `(cfg_.upscale_mode == 3) ? 0.45f : 0.35f`, forced to
    `0.0f` when `cfg_.source_final` (601-602).
  - `CreateSwapchainForFormat` (line ~400): format
    `R10G10B10A2_UNORM` when HDR, `SetColorSpace1(RGB_FULL_G2084_NONE_P2020)`.
  - `CreateTargetViews` (148): target texture format mirrors the swapchain.
- `src/media/shaders.h`
  - `kUpscalePS`: `CatmullRom`, `Lanczos3`, `Easu`, mode switch, then HDR
    encode / PQ tone map (322-349).
  - **`gCon1..gCon3` are declared but unused** (83-85) - free uniform space is
    available, but explicit `gEnh0..gEnh2` is clearer.

### 3.2 The FG path (the part that actually needs care)

- `src/app/app.cpp`
  - `App::Frame` -> stream block (3311 onward).
  - FG branch (3314-3363): `RunDlssSr` -> `SetSourceView` ->
    `dlss_sr_.ReadbackLast` -> `SubmitNrFrameData`; raw fallback ->
    `SetSourceFrame` + `SubmitNrFrame`.
  - `App::RunDlssSr` (new, ~1930): downscale to the DLSS input size, `Process`.
  - `App::NrStageSize` (~1923): FG stage = SR output when SR is on.
  - `App::ApplyDlssFrameGen` (~2072): ensures SR, restarts the worker when the
    stage size changes, `EnablePresent`, child window.
  - `Presenter::Render` is **skipped entirely while the FG child covers the
    parent** (`fg_covering`, 3163) - so the worker path cannot rely on it.
- `src/media/nv_dlss_sr.cpp`
  - `Process` (445): waits the previous submission, returns the **previous**
    completed slot (`out.texture`, `out.srv`) while the current one is in
    flight. `ReadbackLast` (556) copies that slot to a staging texture.
  - `NVSDK_NGX_Parameter_Sharpness` is set to `0.3` (494); whether it changes
    output must be measured before it is exposed.
- `src/media/nv_dlssnr.cpp`: worker protocol; present format 28 = RGBA8 (SDR);
  `EnablePresent(w,h)` sizes the shared memory once.

### 3.3 Settings, profile, UI

- `src/session/profile.h` / `profile.cpp`: flat JSON; `Save` writes one line
  per field (342-401), `Load` uses `ExtractNumber`/`ExtractString` (443-...).
- `src/app/app.cpp`
  - `enum WidgetId` (line ~196-265); highest used id is 104.
  - `enum class Tab` (74), tab bar (4665-4669, `for (int i = 0; i < 9; ++i)`),
    keyboard cycling `% 9` (1084/1087).
  - `RenderSettings` switch (4692); `Tab::Upscaling` (4795),
    `Tab::Performance` (4710).
  - `DrawSlider` (4344) writes the value while dragging; `DrawDropdown` (…),
    `DrawButton`.
  - Overlay: `RenderStreamSettings` (5416): tabs array (5429), loop count 5
    (5434), `switch (overlay_tab_)` (5451).
  - HUD chain line (5298-5320): `NR %.1f ms`, `DLSS %.1f ms`, `FG %.0f fps`.

---

## 4. WP-A - Enhance pass (core), 10:15-11:30

Goal: one GPU pass that grades and sharpens, usable by (a) the normal
presenter path and (b) the FG readback. Identical pixels in both.

### 4.1 API (pipeline.h)

```cpp
struct EnhanceParams {
    bool active = false;       // any non-neutral value -> run the pass
    bool color = false;        // colour controls on (sharpness can run alone)
    float sharpen = 0.0f;      // 0..1.2, 0.35 == today's preset
    float exposure = 0.0f;     // stops, -1..+1
    float contrast = 1.0f;     // 0.5..1.5, pivot 0.18 linear
    float highlights = 0.0f;   // -1..+1
    float shadows = 0.0f;      // -1..+1
    float saturation = 1.0f;   // 0..2
    float vibrance = 0.0f;     // 0..1 (protects saturated pixels)
    float hue_deg = 0.0f;      // -180..+180
    float temperature = 0.0f;  // -1..+1 (cool..warm)
    float tint = 0.0f;         // -1..+1 (green..magenta)
    float gamma = 1.0f;        // 0.8..1.2
};

void SetEnhance(const EnhanceParams& params);
const EnhanceParams& enhance() const;

// GPU: uploads `rgba`, grades, returns RGBA8 at w x h. Used by the FG path.
bool EnhanceFrameReadback(const uint8_t* rgba, int w, int h, bool src_pq,
                          std::vector<uint8_t>& out, double* ms);
// GPU: grades an existing view (DLSS SR output) and reads it back.
bool EnhanceViewReadback(ID3D11ShaderResourceView* view, int w, int h, bool src_pq,
                         std::vector<uint8_t>& out, double* ms);
```

Private state: `EnhanceParams enh_{};`, `ComPtr<ID3D11Texture2D> grade_tex_;`,
`ComPtr<ID3D11RenderTargetView> grade_rtv_;`,
`ComPtr<ID3D11Texture2D> grade_staging_;`, `int grade_w_, grade_h_;`,
`double enh_ms_;`.

### 4.2 Shader (`kUpscalePS`)

Add to `PsParams` (must stay 16-byte aligned):

```
float enh0[4];   // sharpen, exposure, contrast, saturation
float enh1[4];   // highlights, shadows, vibrance, hue_radians
float enh2[4];   // temperature, tint, gamma, color_active
```

Order inside `PSMain` (single pass, no extra target) - **as implemented**:

1. upscale (mode 0-4), unchanged;
2. sharpening (CAS, 1-texel cross on the upscaled image) when sharpening > 0;
3. tone-map / HDR encode (existing code), so `col` is display-referred;
4. colour block when the colour switch is on (`gColor1.w > 0.5`).

The uniform block ended up as `gColor0..gColor2` (exposure/contrast/saturation/
hue, highlights/shadows/vibrance/active, temperature/tint/gamma) instead of
`gEnh0..2`; the layout and the maths are identical. Sharpening runs **before**
the tone map so the centre and its four neighbours are in the same space (no
second render target). See `docs/27-color-and-sharpening.md` for the shipped
implementation and measured results.

The rest of this section is the original design sketch; the names map one to
one (`gEnh0.x` == sharpening, `gEnh0.y` == exposure, and so on).

```hlsl
// step 3: contrast-adaptive sharpening on display-space values
float3 blur = (n + s + e + w) * 0.25;           // 1-texel cross
float3 mn = min(min(n, s), min(e, w)), mx = max(max(n, s), max(e, w));
col = clamp(col + (col - blur) * gEnh0.x, mn, mx);
```

```hlsl
// step 4: colour grade
// gEnh0 = sharpen, exposure_stops, contrast, saturation
// gEnh1 = highlights, shadows, vibrance, hue_radians
// gEnh2 = temperature, tint, gamma, color_active

// Conversions: SDR display space <-> linear. When both the source and the
// output are PQ (HDR10), use the ST.2084 EOTF instead, normalised so that
// 1.0 == 100 nits (SDR white); values above 1.0 are real HDR highlights.
float3 ToLinear(float3 c) {
    if (gHdr > 0.5 && gSrcPq > 0.5) {
        float3 p = pow(saturate(c), 1.0 / 78.84375);
        float3 lin = pow(max(p - 0.8359375, 0.0) / max(18.8515625 - 18.6875 * p, 1e-6),
                         1.0 / 0.1593017578125);
        return lin * (10000.0 / 100.0);          // 1.0 == 100 nits
    }
    return pow(max(c, 0.0), 2.2);
}
float3 FromLinear(float3 lin) {
    if (gHdr > 0.5 && gSrcPq > 0.5) {
        lin *= (100.0 / 10000.0);
        float3 p = pow(max(lin, 0.0), 0.1593017578125);
        return pow((0.8359375 + 18.8515625 * p) / (1.0 + 18.6875 * p), 78.84375);
    }
    return pow(max(lin, 0.0), 1.0 / 2.2);
}

float3 lin = ToLinear(col);
lin *= exp2(gEnh0.y);                           // exposure, stops

// White balance: channel gains, normalised on luma so temperature does not
// change brightness.
float3 wb = float3(1.0 + 0.15 * gEnh2.x, 1.0 + 0.10 * gEnh2.y,
                   1.0 - 0.15 * gEnh2.x);
wb /= dot(wb, float3(0.2126, 0.7152, 0.0722));
lin *= wb;

float luma = dot(lin, float3(0.2126, 0.7152, 0.0722));
lin = max((lin - 0.18) * gEnh0.z + 0.18, 0.0);  // contrast, 18% grey pivot
lin *= 1.0 + gEnh1.x * smoothstep(0.45, 1.0, luma) * 0.35;          // highlights
lin *= 1.0 + gEnh1.y * (1.0 - smoothstep(0.02, 0.22, luma)) * 0.45; // shadows

// Hue: rotate in YIQ, keeps luma by construction.
luma = dot(lin, float3(0.2126, 0.7152, 0.0722));  // after exposure/contrast/shadow
float i = dot(lin, float3(0.595716, -0.274453, -0.321263));
float q = dot(lin, float3(0.211456, -0.522591, 0.311135));
float ca = cos(gEnh1.w), sa = sin(gEnh1.w);
float i2 = i * ca - q * sa;
float q2 = i * sa + q * ca;
lin = float3(luma, luma, luma) + i2 * float3(0.9563, -0.2721, -1.1070) +
      q2 * float3(0.6210, -0.6474, 1.7046);

// Saturation + vibrance (vibrance weights low-chroma pixels more).
float mx = max(lin.r, max(lin.g, lin.b));
float mn = min(lin.r, min(lin.g, lin.b));
float chroma = saturate(mx - mn);
float sat = gEnh0.w * (1.0 + gEnh1.z * (1.0 - chroma));
lin = lerp(float3(luma, luma, luma), lin, sat);

col = FromLinear(lin);                          // back to display space
col = pow(max(col, 0.0), 1.0 / gEnh2.z);        // gamma, display space
```

Notes that matter:
- The fit-to-window pass passes `color_active = 0`, `sharpen = 0`, `mode = 1`
  (otherwise the image is graded twice).
- `source_final` must now skip only the scaler, **not** the sharpening.
- The sharpen neighbours (`n, s, e, w`) are the upscaled source texels, sampled
  in the same pass, exactly like today's mode-3 branch.

### 4.3 Render path

`Presenter::Render`: build `PsParams` from `enh_` for the stage pass; the fit
pass uses neutral values. Factor the current `set_params`/`draw_fullscreen`
lambdas into private helpers so the readback path reuses them:

```cpp
void SetPsParams(int src_w, int src_h, int dst_w, int dst_h, int mode,
                 float sharpen, bool hdr_out, bool src_pq);
void DrawFullscreen(ID3D11ShaderResourceView* srv, ID3D11RenderTargetView* rtv,
                    int w, int h);
```

`cfg_.source_pq` is currently read inside `set_params`; make it a **parameter**
or the readback will tone-map based on the wrong flag.

### 4.4 Readback path

```cpp
bool Presenter::EnhanceViewReadback(ID3D11ShaderResourceView* view, int w, int h,
                                    bool src_pq, std::vector<uint8_t>& out, double* ms) {
    EnsureGradeTargets(w, h);                    // recreate on size change
    OMSetRenderTargets(grade_rtv_, nullptr);     // unbind previous
    SetPsParams(w, h, w, h, /*mode*/0, static_cast<float>(enh_.sharpen),
                /*hdr_out*/false, src_pq);       // worker is RGBA8/SDR
    DrawFullscreen(view, grade_rtv_, w, h);
    ID3D11RenderTargetView* none = nullptr; OMSetRenderTargets(1, &none, nullptr);
    ID3D11ShaderResourceView* no_srv = nullptr; PSSetShaderResources(0, 1, &no_srv);
    CopyResource(grade_staging_, grade_tex_);    // CPU wait starts here
    Map(READ) -> memcpy rows (respect RowPitch) -> Unmap
    out.resize(w*h*4);  *ms = QPC delta;
}
```

`EnhanceFrameReadback` = `SetSourceFrame(rgba, w, h, src_pq)` then
`EnhanceViewReadback(source_srv_, ...)`. One upload, one draw, one copy.

### 4.5 Cost budget and probe

- 1080p readback: 8.3 MB; 1440p: 14.2 MB (the SR path already pays this).
- Expect +0.1-0.3 ms GPU, +1-2 ms of blocking copy on the presentation thread
  (the same class as `ReadbackLast` today).
- Add `BRP_ENHANCE_PROBE=1`: print `enh_ms` with the `[app] fps` line, and add
  `ENH %.2f ms` to the HUD chain line after `FG` when active.
- **A/B rule**: if `work p99` rises above 7 ms or the worker drops below 118,
  the enhance readback moves off the presentation thread (do not attempt that
  today; fall back to Safe preset and document).

### 4.6 Why one implementation

The worker consumes CPU RGBA; the parent consumes a GPU view. If colour ran
twice with different math (GPU for the parent, CPU for the worker) the reveal
would visibly shift. So: the worker gets the graded readback, the parent grades
the same source with the same shader. No path ever receives already-graded
pixels.

Acceptance for WP-A: neutral params produce a pixel-identical screenshot to the
pre-change build (mean luma delta < 0.1%, Laplacian variance delta < 1%);
sharpen 100 raises Laplacian variance >= 25% on a frozen frame; cost within
budget.

---

## 5. WP-B - Colour and sharpening UI, 11:30-12:15

### 5.1 Profile fields (`profile.h`, `profile.cpp`)

```cpp
int  color_on = 0;              // 0 = colour controls bypassed
int  sharpness = 35;            // 0..100
int  color_exposure = 0;        // -100..100 -> +-1 stop
int  color_contrast = 100;      // 50..150
int  color_highlights = 0;      // -100..100
int  color_shadows = 0;         // -100..100
int  color_saturation = 100;    // 0..200
int  color_vibrance = 0;        // 0..100
int  color_hue = 0;             // -180..180
int  color_temperature = 0;     // -100..100
int  color_tint = 0;            // -100..100
int  color_gamma = 100;         // 80..120
```

`Save`: 12 lines after `"hdr"`; `Load`: 12 `ExtractNumber` calls. Older
profiles keep the defaults (neutral).

### 5.2 Mapping (single place, `App::ApplyEnhance`, declared in `app.h`)

```cpp
void App::ApplyEnhance() {
    EnhanceParams p;
    p.color = profile_.color_on > 0;
    p.sharpen = profile_.sharpness / 100.0f;
    p.exposure = profile_.color_exposure / 100.0f;
    p.contrast = profile_.color_contrast / 100.0f;
    p.highlights = profile_.color_highlights / 100.0f;
    p.shadows = profile_.color_shadows / 100.0f;
    p.saturation = profile_.color_saturation / 100.0f;
    p.vibrance = profile_.color_vibrance / 100.0f;
    p.hue_deg = static_cast<float>(profile_.color_hue);
    p.temperature = profile_.color_temperature / 100.0f;
    p.tint = profile_.color_tint / 100.0f;
    p.gamma = profile_.color_gamma / 100.0f;
    p.active = p.sharpen > 0.0f || p.color;
    presenter_.SetEnhance(p);
}
```

Called from `App::Init` after the profile loads and from every slider/click.
Cheap enough to call every frame; still, call it on change only.

### 5.3 Tab

`enum class Tab` (line 74) becomes
`Display, Upscaling, Color, Performance, Neural, Audio, Controller, Connection, Debug, About`.
Then:

- `tab_names` + `tab_icons` arrays (4665) get `L"Color"` / `Icon::Color` after
  Upscaling; add `Icon::Color` to the `Icon` enum and draw a small three-dot
  palette in `DrawIcon`.
- `for (int i = 0; i < 9; ++i)` -> `10` (4669).
- Keyboard cycle `% 9` -> `% 10`, `+8` -> `+9` (1084/1087).

`case Tab::Color` layout, starting at `y = 148`, rows 44 px:

| y | Control | Range | ID |
| --- | --- | --- | --- |
| 148 | `Colour controls` dropdown | Off / On | 131 |
| 192 | Exposure | -100..100 | 120 |
| 236 | Contrast | 50..150 | 121 |
| 280 | Highlights | -100..100 | 122 |
| 324 | Shadows | -100..100 | 123 |
| 368 | Saturation | 0..200 | 124 |
| 412 | Vibrance | 0..100 | 125 |
| 456 | Hue | -180..180 (deg) | 126 |
| 500 | Temperature | -100..100 | 127 |
| 544 | Tint | -100..100 | 128 |
| 588 | Gamma | 80..120 | 129 |
| 632 | Sharpening | 0..100 | 130 |
| 690 | Preset row (4 x 190 px) | Neutral / Vivid / Cinematic / Warm | 133-136 |
| 744 | Reset | - | 132 |

`DrawSlider` takes a `y` and draws a 46 px control; increment by 44. The last
row ends at 788, inside the 840 px content area at 900 px logical height. Verify
at 1280x720 too (the layout scales with `ui_scale_`, the rows do not).

Widget ID range: 120-136 (110-119 stay free for the launch presets from
`docs/24`, 140-143).

Note for `DrawSlider`: pass `percent = true` for percentages; for Hue pass
`percent = false` (prints `-30`; label says `Hue (deg)`). Negative values are
already handled by the drag math.

`Icon::Color`: three overlapping circles in accent/ink/muted, drawn with
`ui_.FillCircle`.

Also grey the SDR-only controls when `hdr_` is on: highlights, shadows,
gamma and vibrance are applied in the linear HDR path only if the test in WP-C
passes; otherwise add the note
`"Highlights/shadows/gamma are SDR-only; HDR gets exposure, white balance, saturation, hue and sharpening."`

### 5.4 Presets

```cpp
case 0: // Neutral
    profile_.color_exposure = 0; profile_.color_contrast = 100;
    profile_.color_highlights = 0; profile_.color_shadows = 0;
    profile_.color_saturation = 100; profile_.color_vibrance = 0;
    profile_.color_hue = 0; profile_.color_temperature = 0;
    profile_.color_tint = 0; profile_.color_gamma = 100; break;
case 1: // Vivid
    profile_.color_saturation = 118; profile_.color_vibrance = 35;
    profile_.color_contrast = 108; break;
case 2: // Cinematic
    profile_.color_contrast = 112; profile_.color_saturation = 92;
    profile_.color_highlights = -18; profile_.color_shadows = 10;
    profile_.color_temperature = -8; break;
case 3: // Warm
    profile_.color_temperature = 22; profile_.color_tint = 4;
    profile_.color_saturation = 105; break;
```

Presets set values, they are not a hidden mode: the sliders follow.

### 5.5 Overlay (`RenderStreamSettings`)

Tabs array (5429): `Stream, DLSS, Color, Neural, Audio, Controller`; loop count
5 -> 6. `case 2` becomes Color, `case 3` Neural, `case 4` Audio,
`case 5` Controller (shift the existing cases). Colour panel (6 controls):
Sharpening, Saturation, Hue, Contrast, Temperature, Reset.

Acceptance for WP-B: dragging every slider changes the picture live at 60 fps
source cadence, no worker restart (`nr-worker.log` RNSZ count unchanged), no
RNSZ spam, profile round-trips (restart restores values).

---

## 6. WP-C - Feed the FG worker correctly, 12:15-12:45

This is the only risky change. Replace the two feed paths with one graded
readback.

### 6.1 SR + FG (the launch preset)

In the FG branch (3314):

```cpp
if (profile_.neural_sr > 0 && dlss_ready_) {
    media::NvDlssSr::Frame sr_frame;
    if (RunDlssSr(stream_frame_, sr_frame)) {
        const int sr_w = dlss_sr_.out_width();
        const int sr_h = dlss_sr_.out_height();
        presenter_.SetSourceView(sr_frame.srv.Get(), sr_w, sr_h, stream_frame_.pq);
        // source is already at the target resolution: skip only the scaler
        presenter_.SetSourceFinal(true);
        std::string rb;
        if (presenter_.EnhanceViewReadback(sr_frame.srv.Get(), sr_w, sr_h,
                                           stream_frame_.pq, sr_readback_, &enh_ms_) &&
            sr_readback_.size() == static_cast<size_t>(sr_w) * sr_h * 4) {
            SubmitNrFrameData(std::move(sr_readback_), sr_w, sr_h, stream_frame_.pq);
            fed = true;
        }
    }
}
```

- `dlss_sr_.ReadbackLast` is no longer called in this branch: the presenter
  copy is the readback (one copy instead of two).
- The SR slot returned by `Process` is the previous completed slot; the
  current one is still in flight, so grading it on the D3D11 context is safe.
- Geometry assertion: the size passed to `SubmitNrFrameData` must equal the
  size given to `EnablePresent` (`nr_in_w_/nr_in_h_`); log once if not.

### 6.2 FG without SR (the Safe preset)

```cpp
if (presenter_.enhance().active) {
    if (presenter_.EnhanceFrameReadback(stream_frame_.rgba.data(), stream_frame_.width,
                                        stream_frame_.height, stream_frame_.pq,
                                        sr_readback_, &enh_ms_)) {
        SubmitNrFrameData(std::move(sr_readback_), stream_frame_.width,
                          stream_frame_.height, stream_frame_.pq);
    } else {
        // Grading failed: keep the ungraded CPU feed, never a black frame.
        presenter_.SetSourceFrame(stream_frame_.rgba.data(), stream_frame_.width,
                                  stream_frame_.height, stream_frame_.pq);
        SubmitNrFrame(stream_frame_);
    }
} else {
    // Nothing to grade: keep the zero-copy CPU feed this path has today.
    presenter_.SetSourceFrame(stream_frame_.rgba.data(), stream_frame_.width,
                              stream_frame_.height, stream_frame_.pq);
    SubmitNrFrame(stream_frame_);
}
```

`EnhanceFrameReadback` already uploads the frame for the parent window, so it
replaces the `SetSourceFrame` call: one upload, one draw, one readback.

### 6.3 HDR

The worker's present format is 28 (RGBA8, SDR). Grade the readback with
`hdr_out = false`, `src_pq = stream_frame_.pq`: the existing PQ->SDR tone map
runs before grading, so HDR + FG produces a **correctly toned SDR** picture
instead of PQ bytes in an 8-bit buffer. The parent still shows HDR.
Document the mismatch (HDR on reveal, SDR through the worker) and keep SDR as
the launch default.

### 6.4 Failure handling

`EnhanceViewReadback` false -> log once, disable colour for the session
(`profile_.color_on = 0`, `ApplyEnhance()`), keep sharpening, keep FG. Never
disable FG because grading failed.

Acceptance: worker log 120.0 for 2 minutes, screenshot (desktop capture, i.e.
what the worker shows) is visibly graded, and with neutral params the screen
equals the pre-change capture.

---

## 7. WP-D - 120 fps smoothness audit (13:15-13:45)

Not a feature; a hunt with a fixed list. Each item is measured, then left
alone if it is green.

1. **Worker rate**: `[fg] displayed` 120.0 +/-1 for 10 min. If it dips when the
   chrome is hidden, the cause is a missed feed, not the UI loop.
2. **Source tick**: `[video] ... src 5-6 ms` when covered. Budget is 16.7 ms;
   alert above 9 ms.
3. **Parent loop**: `[frame] late` is meaningless while covered (the FG child
   owns the display); use `work p99` and `stalls` instead.
4. **Arrival jitter**: `[video] arrival 16.7 ms outliers` <= 2 per 2 s.
   Outliers mean the decode thread was late, not the renderer.
5. **Queue**: `queue 0-1`, `drops 0`, `buffer 1`.
6. **Decode**: avg <= 6 ms, max <= 15 ms at the chosen bitrate.
7. **NR**: off for launch (`neural_nr=0`); if on, expect 102-113 fps.
8. **HUD**: hidden (`H`), cursor away from the top 124 px, chrome faded.
9. **Windows**: High performance plan, NVIDIA overlay off, Game Bar off,
   browsers/OneDrive closed, Windows Update paused.
10. **Display**: 2560x1440 @ 240 Hz, fixed refresh (VRR off for the test), HDR
    per decision, "Prefer maximum performance" in the NVIDIA profile.
11. **App priority**: `THREAD_PRIORITY_ABOVE_NORMAL` is already set in
    `main_app.cpp:20`; do not raise it further.
12. **Swapchain**: `SetMaximumFrameLatency(1)` present; do not touch.

If the worker is below 118 with all of that green, the fallback ladder is:
FG 2x without SR -> SR only -> raw. Record the choice in the health header.

---

## 8. WP-E - Network: router + PC + PS5 (09:15-10:15, re-measure 13:45-14:15)

Everything below is measured before/after and written into
`docs/26-network-setup.md` with old value, new value, effect, revert.

### 8.1 Baseline measurement (before touching anything)

```powershell
$ps5 = "192.168.1.21"
ping -n 100 $ps5                     # loss, min/avg/max
Get-NetAdapter -Physical | Format-Table Name,Status,LinkSpeed,FullDuplex,MacAddress
# then use the real adapter name below (often "Ethernet")
Get-NetAdapterStatistics -Name Ethernet | Format-List
Get-NetAdapterAdvancedProperty -Name Ethernet | Sort DisplayName | Format-Table DisplayName,DisplayValue
```

Pass: 0% loss, avg RTT <= 1 ms, max <= 3 ms, no RX/TX errors or discards.

### 8.2 Windows (PC)

1. Power: `powercfg /setactive SCHEME_MIN` (High performance); USB selective
   suspend off; PCIe ASPM off; disk sleep never.
2. NIC advanced properties (names vary by driver - match by wildcard, verify
   with `ValidDisplayValues`): Energy-Efficient Ethernet / Green Ethernet
   **off**, Interrupt Moderation **off**, Flow Control **off**, Large Send
   Offload v2 (IPv4) **off**, Jumbo Packet **off**, Receive Side Scaling
   **on**, Speed & Duplex **1.0 Gbps Full Duplex**.
3. Device Manager -> NIC -> Power Management: uncheck "Allow the computer to
   turn off this device".
4. MMCSS (optional, documented, reboot, revert if audio changes):
   ```
   reg add "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Multimedia\SystemProfile" /v NetworkThrottlingIndex /t REG_DWORD /d 0xffffffff /f
   reg add "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Multimedia\SystemProfile" /v SystemResponsiveness /t REG_DWORD /d 0 /f
   ```
   Defaults to restore: `NetworkThrottlingIndex=10`, `SystemResponsiveness=20`.
5. Windows Update paused 7 days; Delivery Optimization uploads off; Game Bar
   and captures off; NVIDIA overlay off; OneDrive/browser/downloads closed.
6. DNS 1.1.1.1 / 1.0.0.1 (only affects PSN sign-in latency, not the stream).
7. Firewall: app allowed on Private; no third-party firewall active.

### 8.3 Router

1. Record model, firmware, topology (ISP router in bridge or not), port map.
2. PC and PS5 wired to the **same** switch; no powerline, no repeater.
3. DHCP reservations for both (the profile stores 192.168.1.21).
4. Disable traffic shaping / "game boost" that buffers; if the router exposes
   DSCP QoS, put UDP 9295-9297 at EF for both devices.
5. IGMP snooping **on** (LAN discovery uses SSDP multicast).
6. Disable Energy Efficient Ethernet / Green Ethernet on the used ports;
   disable USB media server / SMB sharing on the router.
7. Wi-Fi (only for other devices): fixed 5 GHz channel, 80 MHz, band steering
   off, keep phones off the 5 GHz band during play.
8. Do not enable jumbo frames anywhere.

### 8.4 PS5

1. Settings > Network > Set Up Internet Connection > **Use a LAN Cable**;
   Test Internet Connection (NAT Type 2).
2. Settings > System > Remote Play > Enable Remote Play **on**.
3. Settings > System > Power Saving > Features Available in Rest Mode:
   "Stay Connected to the Internet" **on**, "Enable Turning On PS5 from
   Network" **on**.
4. Settings > Screen and Video > Video Output: 120 Hz Output **Automatic**,
   HDR per decision, "Adjust HDR" calibrated once.
5. Pause Downloads/Uploads; disable "Update in Rest Mode" during the session.
6. Game: performance mode; any in-game motion blur/RT toggles recorded.

### 8.5 Bitrate matrix (10 min each)

| Bitrate | lost | drops | decode avg/max | arrival outliers | verdict |
| --- | --- | --- | --- | --- | --- |
| 50 Mbps | | | | | |
| 75 Mbps | | | | | |
| 100 Mbps | | | | | |

Pick the highest with 0 lost / 0 drops and decode max <= 15 ms. Then a
cable-pull test: unplug the PC cable for 5 s, reconnect, Home > Connect,
measure time to first frame (<= 20 s).

Deliverable: `docs/26-network-setup.md` with the filled tables, every setting,
the commands, and a revert list.

---

## 9. WP-F - Objective picture checks (`tools\measure_frame.ps1`)

Screenshot the composited desktop (this includes the FG child window, so the
worker output is what is measured), downscale to 640x360, and print:

- mean luma, luma std;
- mean saturation, circular mean hue;
- Laplacian variance on luma (sharpness proxy).

```powershell
# capture + metrics
powershell -ExecutionPolicy Bypass -File tools\measure_frame.ps1 -Out build\shots\neutral.png
# compare two captures (prints deltas and PASS/FAIL against the thresholds)
powershell -ExecutionPolicy Bypass -File tools\measure_frame.ps1 -Compare build\shots\neutral.png build\shots\sharp100.png
```

Scene: freeze the game (pause/photo mode), camera still, HUD hidden. Thresholds:

| Test | Expected |
| --- | --- |
| Neutral vs pre-change build | dLuma < 0.5%, dLaplacian < 3% |
| Sharpness 0 -> 100 | dLaplacian >= +25% |
| Saturation 0 | mean sat < 0.03 |
| Hue 0 -> +30 | circular hue +30 deg +/-5 |
| Exposure 0 -> +50 | dLuma +20% +/-8% |
| Contrast 100 -> 150 | luma std +15% +/-10% |
| Temperature 0 -> +40 | R/B ratio up, luma unchanged +/-2% |

If a threshold fails, the control is either mis-mapped or the shader order is
wrong - fix before moving on.

---

## 10. WP-G - Optional, only if everything above is green by 13:15

- Launch preset buttons from `docs/24-gta6-launch-plan.md` (ids 140-143).
- Health log `%APPDATA%\BetterRemotePlay\health.log`, one line per 30 s:
  preset, worker fps, DLSS ms, enhance ms, frame p50/p99, late/stalls, source
  fps, 4v%, queue, drops, lost, decode avg/max, audio underruns, handles, VRAM
  (`IDXGIAdapter3::QueryVideoMemoryInfo`). Rotate at 2 MB.
- Both are droppable; never trade packaging time for them.

---

## 11. Atomic issue checklist

Run top to bottom before packaging. Every line has a verifiable check.

### Build and D3D

1. `PsParams` is a multiple of 16 bytes; HLSL `cbuffer` order matches exactly
   (`enh0`, `enh1`, `enh2` after `con3`).
2. Shader compiles with `D3DCOMPILE_ENABLE_STRICTNESS`; no warning in stderr.
3. `EnhanceViewReadback` unbinds the RTV before `CopyResource` (debug layer
   otherwise reports "resource still bound").
4. Grade target and staging are recreated when w/h change; the old pair is
   released (`grade_w_/grade_h_` guard).
5. Staging `RowPitch` is used when copying rows (not `w*4`).
6. On `Map` failure, return false with nothing bound and leave the frame
   ungraded (the caller falls back); never leave staging mapped.
7. No new D3D11 live-object growth over 10 minutes (debug layer or GPU
   memory in `nvidia-smi`).
8. `enh_ms_` is measured with QPC around copy+map, not around the whole frame.

### Shader math

9. Neutral params: the enhance maths is skipped (`color_active=0`,
   `sharpen=0`), output identical.
10. `gEnh2.w` is 0 in the fit-to-window pass (no double grading).
11. `cfg_.source_pq` is passed per call, not read from the presenter's config,
    in the readback path (HDR + FG tone map).
11a. PQ in + PQ out uses the ST.2084 decode/encode branch; SDR output is
    byte-identical to the non-PQ path.
12. Sharpening happens after tone mapping and before the colour grade.
13. Sharpening applies when `source_final` is true (DLSS present path).
14. White balance is luma-normalised (temperature must not change brightness).
15. Hue rotation preserves luma (rotation matrix applied on YIQ, not per
    channel).
16. Saturation 0 gives Rec.709 luma grayscale, no tint.
17. Contrast pivot is 0.18 linear; raising contrast does not clip >2% of pixels
    at 100 (histogram check).
18. Highlights/shadows masks are smooth (no visible banding on a gradient).
19. Gamma is applied in display space, after the linear round trip.
20. Values are clamped/saturated once, at the end; no NaN/Inf from pow of
    negative numbers (`max(x, 0)`).

### FG path

21. Graded buffer size == `nr_in_w_ * nr_in_h_ * 4`; log once otherwise.
22. The SR slot graded is the previous completed one; grading never touches the
    in-flight slot.
23. `ReadbackLast` is not called in the FG+SR branch (one copy, not two).
24. FG without SR still falls back to `SubmitNrFrame` if grading fails.
24a. Safe preset (no SR) with neutral enhance uses the CPU feed: no extra
    readback, no regression.
25. Grading failure never disables FG.
26. Worker fps stays 120.0 +/-1 for 10 min with grading on.
27. Worker gets SDR RGBA8 even with HDR on (format 28), with a correct tone
    map (screenshot check).
28. Parent reveal (cursor in the top strip) shows the same grade as the worker.
29. `ApplyDlssFrameGen` still restarts the worker exactly once on size change;
    no restart loops when toggling colour.
30. Screenshots taken while covered show the grade (proves the worker path).

### UI and profile

31. Slider drag updates the picture within one frame; no RNSZ in
    `nr-worker.log` per step.
32. Slider drag does not restart the FG worker (`EnablePresent` count stable).
33. New tab appears in Settings and in the overlay; keyboard Tab cycles 10
    tabs, not 9.
34. `% 9` / `+8` keyboard cycling updated to `%10` / `+9`.
35. Hue slider shows degrees, not percent; negative values display correctly.
36. Reset restores defaults and persists them.
37. Missing profile keys load neutral (old `session.json`).
38. Profile JSON stays valid; `Load` after `Save` round-trips every new field.
39. Colour controls are reachable with the controller (dpad/analog).
40. In fullscreen 900px logical height, the last row (Reset) is fully visible.

### Performance

41. Enhance adds <= 0.4 ms GPU and <= 1.2 ms to the source tick.
42. `work p99` <= 7 ms with grading + FG at 1440p.
43. VRAM delta <= 400 MB for the grade target + staging at 1440p.
44. CPU: no new thread, no per-frame allocation (`sr_readback_` reused).
45. 30-minute soak: handles +<50, worker 120, no drops.
46. Alt-tab out/in: no resize bug, worker returns to 120.
47. HDR toggle requires restart and comes back with the right colour space.

### Network and packaging

48. `ping` 0% loss; NIC errors/discards unchanged after settings.
49. Chosen bitrate 10 min: lost 0, drops 0, decode max <= 15 ms.
50. Cable pull -> reconnect <= 20 s.
51. Package hash matches; ZIP extracts and runs with a clean profile.
52. No `session.json`, health log or screenshots in the package.

---

## 12. Schedule (hard stop 15:00)

| Time | Work |
| --- | --- |
| 09:00 | Snapshot, tag, backups, hashes (WP-0). Baseline build. |
| 09:15 | Network baseline measurements; `tools\measure_frame.ps1`. |
| 09:30 | Windows + NIC + power settings; re-measure; note deltas. |
| 09:50 | Router + PS5 settings; re-measure; start `docs/26-network-setup.md`. |
| 10:15 | WP-A: shader + Presenter API + params; build; neutral A/B test. |
| 11:30 | WP-B: Colour tab + overlay + profile + presets; build; visual check. |
| 12:15 | WP-C: FG graded readback (SR + raw) + HDR tone map; verify 120 fps. |
| 12:45 | Lunch + 30-minute soak with the final config (collect numbers). |
| 13:15 | WP-D smoothness audit; at most two micro-fixes; re-test. |
| 13:45 | Bitrate matrix + cable pull; fill `docs/26`; pick bitrate. |
| 14:15 | Atomic checklist, all sections; fix only P0; re-run the failed items. |
| 14:35 | Package: build, package script, hash, clean-profile, ZIP test. |
| 14:50 | Docs: `docs/27-color-and-sharpening.md`, update 17/19/22, commit, tag. |
| 15:00 | **Freeze.** Archive known-good EXE + ZIP + profile. No more code. |

Cut lines, in order, if behind:
1. launch preset buttons and health log (WP-G);
2. overlay Color tab (keep Settings tab);
3. Vibrance and Tint sliders (keep the rest);
4. never cut: FG verification, packaging, network doc.

---

## 13. Rollback plan

| Trigger | Action |
| --- | --- |
| Enhance changes pixels when neutral | revert WP-A commit; keep the old sharpen |
| Worker < 118 with grading | `git revert` WP-C; Safe preset (FG, no SR) |
| `work p99` > 7 ms | disable colour for the session, keep sharpening only |
| Colour tab breaks layout/navigation | revert WP-B; keep the code behind the profile |
| Network change makes it worse | revert that single setting from the table |
| Build broken at 14:00 | `git reset --hard pre-color-2026-11-19`, rebuild known-good |

---

## 14. Test matrix (fill the result column)

| Area | Case | Expected | Result |
| --- | --- | --- | --- |
| Video | SR UQ + FG 2x, 1440p, 30 min | worker 120, 4v >=98% | |
| Video | FG 2x no SR | worker 120, safe path | |
| Video | SR only | 1440p, no worker | |
| Video | Raw | display target, no NN | |
| Colour | Neutral A/B | identical | |
| Colour | Sharp 0/35/100 | monotonic Laplacian | |
| Colour | Sat 0/100/200 | hue stable | |
| Colour | Hue -180/0/+180 | circular mean | |
| Colour | Temp/tint extremes | luma preserved | |
| HDR | SDR output + colour | expected | |
| HDR | HDR output + SR only | expected | |
| HDR | HDR output + FG | toned SDR, documented | |
| Audio | 10/20/30/40 ms buffer | 0 underruns | |
| Pad | USB + BT, triggers | all working | |
| Network | 50/75/100 Mbps | pick highest clean | |
| Recovery | cable pull, reconnect | <= 20 s | |
| Stability | 30 min (and 2 h overnight) | flat handles/VRAM | |

---

## 15. Decisions at 09:00

1. Colour defaults: **neutral** (recommended) or a mild Vivid out of the box?
2. Sharpening default: keep **35%** (today's look) or 25% under DLSS?
3. HDR: **SDR** for the session (recommended, worker is SDR) or test HDR first?
4. Network: do we have router admin access and is the PC + PS5 on the same
   switch? (If not, skip 8.3 and document the topology.)
5. Bitrate cap: test up to **100 Mbps** (recommended) or stay at 50?
6. Health log + launch presets: implement if green by 13:15 (recommended:
   preset buttons only).

---

## 16. Deliverables

- Code: enhance pass, Colour tab, FG graded feed, profile fields, optional
  presets/health log.
- `docs/26-network-setup.md` - topology, every router/PC/PS5 setting with
  old/new values, measurements, revert list.
- `docs/27-color-and-sharpening.md` - the exact maths, defaults, measured
  costs, screenshot metrics.
- Updated `docs/17-neural-rendering.md`, `docs/19-dlss-overview.md`,
  `docs/22-performance.md` with the new chain and numbers.
- Screenshots: `build/shots/neutral.png`, `sharp100.png`, `sat0.png`,
  `hue30.png`, `fg-graded.png`.
- Package: `dist/BetterRemotePlay-0.3-standalone.zip`, hash recorded,
  known-good EXE + profile archived.

---

## 17. Execution record (18 Nov, 09:00-12:00)

Shipped, verified on the reference machine:

- **Colour + sharpening** (Settings > Color, overlay > Colour): 11 controls,
  4 presets, Reset. Neutral = identity; the pass runs on the GPU and the FG
  worker receives the graded frame (`docs/27`).
- **Sharpening 0 -> 95**: Laplacian variance +97.0% at 2560x1440 (scene luma
  delta -0.19%). **Saturation 0**: mean saturation 0.000.
- **FG**: `displayed 120.0 FPS (2x)` across the tests; source 4v 96-100%,
  `drops 0`, `lost 0`, decode 4.3-5.0 ms, `work p99` 5.9-6.3 ms (grading on).
- **Health log**: `%APPDATA%\BetterRemotePlay\health.log`, 30 s cadence, with
  worker fps, enhance ms, frame p99, work, cadence, drops/lost, decode, audio,
  handles, VRAM.
- **Presets**: saved only; applied at the start of the next session. Live
  reconfiguration of DLSS/FG mid-stream broke the D3D12VA decoder once
  ("Failed to map D3D12 Buffer resource"), so the app refuses to do it and
  says "Disconnect and reconnect to apply it". Only verified ladders ship:
  Launch (SR + FG), SR only, Low latency. FG without SR is not offered
  because a worker restarted at 1080p without SR came up in overlay mode and
  never presented a frame.
- **Network**: PC NIC changes applied and verified (Green Ethernet, Gigabit
  Lite, Power Saving Mode, LSO v2, RSC disabled); 100-ping 0% loss, 0 ms;
  zero NIC errors/discards (`docs/26`). Router and PS5 items need the admin
  session and are listed as a fill-in checklist.
- **Package**: rebuilt after the last source change, hash compared, clean
  profile boot tested, ZIP inspected.

Not done today: router admin changes, PS5 settings, bitrate sweep 50/75,
cable-pull reconnect test (needs the user), HDR matrix.

