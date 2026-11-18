# 21 - Standalone release package

`tools/package_release.ps1` builds a self-contained folder and zip from
`build/Release`:

```powershell
cmake --build build --config Release --target brp
powershell -ExecutionPolicy Bypass -File tools\package_release.ps1
```

Outputs:

- `dist/BetterRemotePlay/` - the runnable folder (about 909 MB).
- `dist/BetterRemotePlay-0.3-standalone.zip` - the same folder, zipped.

## What is inside

| Item | Purpose |
|---|---|
| `brp.exe` | The app (Windows GUI subsystem, no console). |
| `avcodec/avformat/avutil/swresample/swscale-*.dll` | FFmpeg decode/scale runtime. |
| `fonts/` | Noto Sans + Noto Sans Display (SIL OFL, `OFL.txt`). |
| `nvngx_dlss.dll` | DLSS Super Resolution feature runtime. |
| `nr/native/` | NeuralScreen worker (`nvngx.dll` is the worker EXE), DLSSNR model, DLSSG runtime, the SM86 frame-generation proxy and its kernels. |
| `nvidia-vfx/runtime/` | Optional legacy NVIDIA VFG runtime (VSR removed). |
| `vcruntime140*.dll`, `msvcp140*.dll`, `concrt140.dll` | MSVC runtime, so no VC redist install is needed. |
| `README.txt` | Friend-facing setup instructions. |

`dist/` is gitignored; the package is a build artifact, not committed.

## Personal data

The package contains **no** `session.json`, pairing keys, PSN account ID or
account token. The app stores its profile in
`%APPDATA%\BetterRemotePlay\session.json` on the machine that runs it. The
packaged app was tested with a clean `%APPDATA%`: it starts at Home and
discovers a console on the local LAN, with empty registration keys and no PSN
account.

## Runtime notes

- DLSS SR / DLSSNR / DLSS FG need an NVIDIA RTX GPU and a recent NVIDIA driver.
  Without one the app still streams using the regular upscalers.
- The NeuralScreen worker uses `nr/native/nvngx.dll` as its executable. It is
  deliberately named `.dll` by the upstream package; the app launches it with
  `CREATE_NO_WINDOW`, so it never shows a console.
- The optional `nvidia-vfx` runtime can be deleted if legacy VFG is not used.
  The UI reports it as unavailable instead of failing.
- `nvngx.dll` in `nr/native/` and `nvngx_dlss.dll` in the root are different
  NGX components: one is the NeuralScreen worker, the other the DLSS SR feature
  DLL.

## Build 0.3, 18 Nov 2026

- ZIP: `dist\BetterRemotePlay-0.3-standalone.zip`, 571.6 MB,
  SHA256 `C7866E1C23A101AE97E68056E7BDAB7B89AE7F0F716D5AD952EF5B4545DB6B72`.
- `dist\BetterRemotePlay\brp.exe` SHA256
  `70F9E2D5E68AFDB2817804FCFE7392E6ACEFD1F2A692398669B903C85EC89CBA`, identical
  to `build\Release\brp.exe` at packaging time.
- No `session.json` or pairing data in the package.
