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

## Build 0.4, 19 Nov 2026 (14:43)

- ZIP: `dist\BetterRemotePlay-0.3-standalone.zip`, 602.0 MB,
  SHA256 `B40E215C4B02F9180C4AA7210EF566709E5B36DAB40F84E773ABA60C8BC31C22`.
- Unpacked: 887.8 MB, 56 files: engine, model and cudart64_13.dll in rife\. Includes the RIFE model so another PC can build its own engine.
- `dist\BetterRemotePlay\brp.exe` SHA256
  `5F4EB657512FB4FE98338644E715881BDED59BD96998C4BDB2CAC5B97CFBCB9A`, identical
  to `build\Release\brp.exe` at packaging time.
- Includes `rife\rife425_1080.engine` (12.5 MB) when present in the release
  folder. Written for one GPU architecture; another GPU needs its own engine.
- No `session.json` or pairing data in the package. Runtime logs from the
  build machine (*.log, *.jsonl) are stripped.

## Optional RIFE runtime (TensorRT)

The RIFE probe (`rife_probe.exe`) and, later, the RIFE engine in the app need a
TensorRT 10.16 + CUDA 13 runtime on the PC. Nothing is bundled or linked:

- Point `BRP_TENSORRT_DIR` (or `rife_probe --runtime=`) at a folder that contains
  `nvinfer_10.dll` and `cudart64_13.dll`.
- On the reference PC those come from an existing vs-mlrt install at
  `...\vs-plugins\vsmlrt-cuda`, so no extra download was needed.
- Build one engine per GPU and shape with `trtexec` (command in
  `docs/15-frame-generation.md`). An engine built for another TensorRT version or
  GPU architecture will not deserialize.
- Without the runtime the app keeps using DLSS Frame Generation; the probe exits
  with a clear message.

This is the same arrangement as the NVIDIA VFX runtime: a user-supplied vendor
runtime, not part of the AGPL source tree or the standalone ZIP.
