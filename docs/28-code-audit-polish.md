# 28 - Code audit and polish pass

Scope: every file under `src/` (23k lines), plus the build. Method: a clean
`/W4 /permissive-` build with warnings surfaced, a definition/reference scan for
unused functions and members, and a duplication review of the UI code.

## 1. Build warnings

Only two warnings came from our code (the rest are third-party FFmpeg and
gf-complete headers):

| File | Warning | Fix |
| --- | --- | --- |
| `nv_dlss_sr.cpp:403` | C4100 `detail` unreferenced | dropped the parameter from `Submit()` and its call sites |
| `discovery.cpp:64` | C4996 `inet_addr` deprecated | `InetPtonA(AF_INET, "255.255.255.255", ...)` |

After the pass the build is clean for `src/` with `/W4`.

## 2. Deleted code

| Symbol | Why |
| --- | --- |
| `PercentLabels()`, `PercentIndex()` | never referenced |
| `Presenter::upscale_mode()` | never referenced; the value is read from `PresenterConfig` |
| `NvDlssSr::ReadbackLast()` | replaced by the graded presenter readback; no callers left |
| `App::fg_worker_mult_` | written by the log sampler, never read |
| `App::audio_opened_` | written when the device opens, never read |
| `NvVfx::effect_ready()` | never referenced |

Stale comments removed/rewritten:

- `Presenter` class comment still described the Phase-1 "synthetic source ->
  upscale -> present" shape; it now documents the real chain (upscale ->
  sharpen -> grade -> HDR encode, and the graded readback for FG).
- `NvDlssSr::Submit` no longer takes a `detail` it never used.

## 3. De-duplication

Two tables existed twice, once for the label and once for the click handler:

- bitrates `{20, 35, 50, 75, 100} Mbps`
- pad report rates `{60, 125, 250, 500, 1000} Hz`

They are now single `constexpr` arrays with `BitrateIndex()` / `PadRateIndex()`
helpers, so the dropdown and the handler can never drift apart.

## 4. Thread safety

The pad thread (250-1000 Hz) reads four flags that the UI thread writes:
`stream_active_`, `pad_rate_hz_`, `console_rumble_`, `console_triggers_`. They
were plain `bool`/`int` (a data race, benign on x86 but undefined behaviour).
They are now `std::atomic` with the one `std::clamp()` site using `.load()`.
No behaviour change; the compiler needs no fences on x86 for these flags.

## 5. Deliberately kept

- **Legacy NVIDIA VFG** (`nv_vfx`, ~800 lines): a real, selectable feature
  (Frame generation dropdown). Removing it would be a product decision, not a
  cleanup; it is documented as the legacy path.
- **NVAPI Reflex** (~120 lines): selectable, documented as not recommended; the
  markers path is used when it is on.
- **1000 Hz pad option**: USB-only advice in the label, but the value is valid.
- **`App::Frame` and `RenderSettings` size**: splitting them is a refactor with
  real regression risk and no user-visible gain; left for a quiet day.
- **Settings vs in-stream overlay panels**: the overlay duplicates several
  controls on purpose (live tuning); a shared panel renderer is future work.

## 6. Known issues worth a future pass (not fixed today)

1. **Plain 60 fps path**: measured occasional 3-refresh/5-refresh holds and a
   handful of late 240 Hz ticks even with every neural stage off. The cause is
   in the parent pacing (waitable swapchain / DWM), not in the decode path.
2. **FG 120 on 240**: the worker's presents are not vblank-phase locked, so
   some frames hold 1 or 3 refreshes. A 120 Hz desktop (or G-SYNC) removes the
   ambiguity; this is documented in `docs/27`.
3. **Bluetooth triggers**: the app now signs the BT report correctly (rumble
   verified), but whether adaptive-trigger effects arrive at all depends on the
   console sending non-zero effect packets; the parser logs show the type byte.
4. **BT output report** is zero-padded to Windows' 547-byte
   `OutputReportByteLength`; the 78-byte report itself is correct.

## 7. Verification

- Clean `/W4` build, no warnings from `src/`.
- Runtime: session starts, source 60 fps, `4v` 91-100%, `drops 0`, `lost 0`,
  pad on Bluetooth with `output OK`.
- Diff: 7 files, ~47 insertions / 62 deletions before the atomic change; the
  atomic conversion touches `app.h` plus one `.load()` site.
