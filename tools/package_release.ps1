# Builds a self-contained BetterRemotePlay folder (and zip) from build/Release.
# The user profile is never copied: session.json lives in %APPDATA%, not here.

param(
    [switch]$NoZip
)

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$release = Join-Path $root "build\Release"
$distRoot = Join-Path $root "dist"
$dest = Join-Path $distRoot "BetterRemotePlay"
$version = "0.3"

function Require-File([string]$path) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing required file: $path"
    }
}

function Copy-Required([string]$source, [string]$destination) {
    Require-File $source
    Copy-Item -LiteralPath $source -Destination $destination -Force
}

if (-not (Test-Path (Join-Path $release "brp.exe"))) {
    throw "build\Release\brp.exe not found. Build the Release target first."
}

if (Test-Path $dest) {
    Remove-Item -LiteralPath $dest -Recurse -Force
}
New-Item -ItemType Directory -Path $dest -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $dest "fonts") -Force | Out-Null

# --- core executable and FFmpeg runtime ------------------------------------
Copy-Required (Join-Path $release "brp.exe") $dest
foreach ($dll in @("avcodec-63.dll", "avformat-63.dll", "avutil-61.dll",
                   "swresample-7.dll", "swscale-10.dll")) {
    Copy-Required (Join-Path $release $dll) $dest
}

# DLSS Super Resolution feature runtime (loaded from the executable folder).
Copy-Required (Join-Path $release "nvngx_dlss.dll") $dest

# --- fonts ------------------------------------------------------------------
$fonts = Join-Path $release "fonts"
if (-not (Test-Path $fonts)) {
    $fonts = Join-Path $root "assets\fonts"
}
Copy-Item -Path (Join-Path $fonts "*") -Destination (Join-Path $dest "fonts") `
    -Recurse -Force

# --- NeuralScreen NR + DLSS FG worker --------------------------------------
$native = Join-Path $root "third_party\neuralscreen\native"
$nrNative = Join-Path $dest "nr\native"
New-Item -ItemType Directory -Path $nrNative -Force | Out-Null

foreach ($file in @("nvngx.dll", "nvngx.dll_ns-forwarder.dll", "nvngx_dlssnr.dll",
                    "nvngx_dlssg.dll", "Spout.dll", "SpoutDX.dll")) {
    Copy-Required (Join-Path $native $file) $nrNative
}

$fg = Join-Path $native "fg"
$fgDest = Join-Path $nrNative "fg"
New-Item -ItemType Directory -Path $fgDest -Force | Out-Null
Copy-Required (Join-Path $fg "winmm.dll") $fgDest
Copy-Required (Join-Path $fg "dlssg_sm86.ini") $fgDest

$sm86 = Join-Path $fg "dlssg_sm86"
$sm86Dest = Join-Path $fgDest "dlssg_sm86"
foreach ($sub in @("runtime", "backend", "kernels", "logs")) {
    New-Item -ItemType Directory -Path (Join-Path $sm86Dest $sub) -Force | Out-Null
}
Copy-Required (Join-Path $sm86 "runtime\nvngx_dlssg.dll") (Join-Path $sm86Dest "runtime")
Copy-Required (Join-Path $sm86 "backend\sm86_backend.dll") (Join-Path $sm86Dest "backend")
Copy-Required (Join-Path $sm86 "kernels\fg_kernels.dll") (Join-Path $sm86Dest "kernels")

# --- optional NVIDIA VFX runtime (legacy VFG only) --------------------------
$vfx = Join-Path $root "third_party\nvidia-vfx\runtime"
if (Test-Path $vfx) {
    $vfxDest = Join-Path $dest "nvidia-vfx\runtime"
    Copy-Item -LiteralPath $vfx -Destination $vfxDest -Recurse -Force
    # VSR was removed from the client; do not ship its model/runtime.
    foreach ($vsrOnly in @("nvngx_vsr.dll", "nvVFXVideoSuperRes.dll")) {
        Remove-Item -LiteralPath (Join-Path $vfxDest $vsrOnly) -Force `
            -ErrorAction SilentlyContinue
    }
}

# --- MSVC runtime -----------------------------------------------------------
foreach ($dll in @("vcruntime140.dll", "vcruntime140_1.dll", "vcruntime140_threads.dll",
                   "msvcp140.dll", "msvcp140_1.dll", "msvcp140_2.dll",
                   "msvcp140_atomic_wait.dll", "msvcp140_codecvt_ids.dll",
                   "concrt140.dll")) {
    $path = Join-Path $env:WINDIR "System32\$dll"
    if (Test-Path $path) {
        Copy-Item -LiteralPath $path -Destination $dest -Force
    }
}

# --- optional RIFE engine (TensorRT, built for one GPU architecture) --------
$rife = Join-Path $release "rife"
if (Test-Path $rife) {
    Copy-Item -LiteralPath $rife -Destination (Join-Path $dest "rife") -Recurse -Force
}

# --- keep runtime logs from the build machine out of the package -------------
Get-ChildItem -LiteralPath $dest -Recurse -File -Include *.jsonl,*.log -ErrorAction SilentlyContinue |
    Remove-Item -Force -ErrorAction SilentlyContinue

# --- readme -----------------------------------------------------------------
$readme = @"
BetterRemotePlay $version - standalone build
=============================================

Run brp.exe. No installer, no console window, no network service.

Requirements
------------
- Windows 10/11 64-bit.
- A PS5 or PS4 on the same LAN.
- Optional: an NVIDIA RTX GPU for DLSS Super Resolution, Neural Rendering and
  DLSS Frame Generation. Without one the app still streams with the regular
  upscalers.

First run
---------
1. Open the gear icon (Settings) on Home, then the Connection category.
2. PSN account: click "Sign in with PSN", sign in in the browser, copy the
   redirect URL from the blank page, paste it into the app and save. This stores
   only your PSN account id in %APPDATA%\BetterRemotePlay\session.json.
3. Console address: usually found automatically at startup. If it is not:
   Settings > Connection > Address > Find console, press Discover, or enter the
   IP shown on the PS5 under Settings > Network > View Connection Status.
   Then Save address.
4. On the console: Settings > System > Remote Play > Link Device. Type the
   8-digit code into Pairing PIN, then click Pair.
5. Go back to Home and click Connect.

Your profile is stored in %APPDATA%\BetterRemotePlay\session.json, NOT in this
folder. You can copy the folder to another PC and it starts with a clean profile.
This build contains no pairing credentials or personal session data.

Features and runtimes
---------------------
- FFmpeg DLLs are included for video/audio decoding.
- fonts\ contains the Noto Sans / Noto Sans Display pairing (SIL OFL, see
  fonts\OFL.txt). Sony's SST is proprietary and is not included.
- nr\native\ contains the NeuralScreen NGX worker used for DLSSNR and the
  DLSS Frame Generation presenter.
- nvngx_dlss.dll is the DLSS Super Resolution feature runtime.
- nvidia-vfx\runtime\ contains the optional legacy NVIDIA VFG runtime. RTX VSR
  has been removed from the client; DLSS SR/DLAA, DLSS NR and DLSS Frame
  Generation are the supported neural paths.
- rife\ contains a TensorRT engine for RIFE 4.25 lite, built for the GPU of the
  machine this package came from. RIFE also needs TensorRT 10.16 and CUDA 13 on
  the PC: point BRP_TENSORRT_DIR at a folder with nvinfer_10.dll and
  cudart64_13.dll. Without them the app falls back to NVIDIA frame generation.
  An engine built for another GPU will not load; rebuild it with trtexec.
- The app is not affiliated with Sony Interactive Entertainment. No Sony
  artwork, code, keys or fonts are included.

Giving this to someone else
---------------------------
- Copy the whole folder. Nothing is installed, no admin rights, no service.
  The Visual C++ runtime and FFmpeg DLLs are already inside.
- Each person signs in to PSN and pairs their own console; session.json is not
  included and holds your own credentials, so keep it in %APPDATA%.
- NVIDIA RTX GPU + a current driver are needed for DLSS SR, Neural Rendering
  and DLSS Frame Generation. Without one the app still streams and upscales
  with FSR1/Lanczos, and RIFE is unavailable.
- RIFE is optional and not self-contained. It needs a TensorRT 10.16 + CUDA 13
  runtime on the PC: put nvinfer_10.dll and cudart64_13.dll in rife\runtime
  next to brp.exe, or set BRP_TENSORRT_DIR. The engine in rife\ was built for
  the GPU of the machine this package came from; another GPU needs its own
  engine built with trtexec (command in docs/15-frame-generation.md). Without
  these the app falls back to NVIDIA and says so.
- The console must be on the same LAN with Remote Play enabled.

Settings worth knowing
----------------------
- Settings > Color: sharpening plus exposure, contrast, highlights, shadows,
  saturation, vibrance, hue, temperature, tint and gamma. The in-stream overlay
  (Tab) has the most used controls and applies them live.
- Settings > Performance > Launch presets: "Launch" is DLSS SR Ultra Quality +
  DLSS Frame Generation 2x at 1440p; also SR only and Low latency. Presets are
  saved and applied at the start of the next session (reconnect), never while
  streaming.
- Settings > Upscaling > Interpolation engine: NVIDIA DLSS FG (fast) or RIFE
  4.25 lite (TensorRT). RIFE output is 60 fps (one generated frame per pair, so
  a 30 fps game plays at 60) or pass-through. Output frame rate (60/90/120) is
  the NVIDIA control and only appears for that engine.
- Health log: %APPDATA%\BetterRemotePlay\health.log, one line every 30 seconds
  (worker fps, frame timing, source cadence, drops, decode, audio, handles,
  VRAM). Rotates at 2 MB.
- DualSense: native HID works over USB and Bluetooth, including rumble, LEDs
  and adaptive triggers. On Bluetooth a 250 Hz report rate is plenty; 500/1000
  Hz are for USB.

Credits
-------
- Chiaki/chiaki-ng session layer and PSN OAuth client constants (GPL).
- NVIDIA NGX / DLSS / VFX runtimes, subject to NVIDIA's terms.
- NeuralScreen DLSSNR/DLSSG worker components.
- FFmpeg (https://ffmpeg.org), GPL build by gyan.dev.
- Noto Sans, SIL Open Font License 1.1.
"@
Set-Content -LiteralPath (Join-Path $dest "README.txt") -Value $readme -Encoding UTF8

# --- zip --------------------------------------------------------------------
$zip = Join-Path $distRoot "BetterRemotePlay-$version-standalone.zip"
if (-not $NoZip) {
    if (Test-Path $zip) {
        Remove-Item -LiteralPath $zip -Force
    }
    Push-Location $distRoot
    try {
        & tar.exe -a -c -f $zip "BetterRemotePlay"
        if ($LASTEXITCODE -ne 0) {
            throw "tar failed while creating $zip"
        }
    } finally {
        Pop-Location
    }
}

$size = (Get-ChildItem -LiteralPath $dest -Recurse -File | Measure-Object Length -Sum).Sum
Write-Host ("Package: {0}" -f $dest)
Write-Host ("Size:    {0:N1} MB" -f ($size / 1MB))
if (-not $NoZip) {
    Write-Host ("Zip:     {0}" -f $zip)
}
Write-Host "No session.json or personal pairing data was copied."
