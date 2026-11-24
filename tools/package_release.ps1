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
$version = "2.5.0"

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

$running = Get-Process brp -ErrorAction SilentlyContinue |
    Where-Object { $_.Path -and $_.Path.StartsWith($dest, [System.StringComparison]::OrdinalIgnoreCase) }
if ($running) {
    throw "brp.exe is running from $dest. Close it before packaging."
}
if (Test-Path $dest) {
    Remove-Item -LiteralPath $dest -Recurse -Force
}
New-Item -ItemType Directory -Path $dest -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $dest "fonts") -Force | Out-Null

# --- core executable and FFmpeg runtime ------------------------------------
Copy-Required (Join-Path $release "brp.exe") $dest
# 2.0 shell (Rust/GPUI) plus the headless engine it starts.
$uiRelease = Join-Path $root "ui\target\release\brpnext-ui.exe"
Copy-Required $uiRelease (Join-Path $dest "brpnext.exe")
Copy-Required (Join-Path $release "brpnext-engine.exe") $dest
Copy-Required (Join-Path $release "brpnext.exe") (Join-Path $dest "brpnext-classic.exe")

# Updater: lets a small patch zip replace only the files that changed.
$updater = Join-Path $release "brp_update.exe"
if (Test-Path -LiteralPath $updater) {
    Copy-Item -LiteralPath $updater -Destination (Join-Path $dest "update.exe") -Force
}
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
    # Only the portable pieces: engine, model and the CUDA runtime. The
    # downloaded zip, the extracted SDK and runtime.txt stay on this machine.
    $rifeDest = Join-Path $dest "rife"
    New-Item -ItemType Directory -Path $rifeDest -Force | Out-Null
    foreach ($name in @("rife425_1080.engine", "rife425lite.onnx", "rife426h.onnx", "cudart64_13.dll")) {
        $source = Join-Path $rife $name
        if (Test-Path -LiteralPath $source) {
            Copy-Item -LiteralPath $source -Destination $rifeDest -Force
        }
    }
}
# --- readme -----------------------------------------------------------------
$readme = @"
BetterRemotePlay 2.5 - standalone build
=======================================

Run brpnext.exe. No installer, no console window, no background service.

What changed in 2.5
-------------------
- The shell now picks a profile from the actual machine: first launch optimizes
  once, and Advanced > System > Optimize runs it again on demand. It reads the
  GPU, the DLSS runtimes and the primary display, then sets the source (1080p60,
  buffer 0), the output, VSync/VRR and the AI stages (DLSS-G 2x + SR, or DLSS SR
  alone) accordingly.
- Upscaling, Frame Gen and Neural Rendering have separate pages; the Neural tab
  exposes every parameter of the NGX feature-18 worker (style, intensity, local
  tone, local structure, skin structure, passes, work scale, UI correction, auto
  mask) as sliders with editable numbers. Passes go up to 20 normally.
- "Use at your own risk" at the bottom of the Neural page lifts every neural
  limit to a hard 9999 (sliders and typed values).
- The in-game control center has a Neural tab and remembers the tab you were on
  while the game session lasts (it resets on disconnect).
- All cards on all pages share one frost look: translucent over the aurora, a
  hairline border and a soft gaussian rim.
- Latency work: buffer 0, 60 fps, DLSS-G 2x; the DLSS SR "Max" preset now
  actually initializes (it used to disable SR silently).

What changed in 2.0
-------------------
- New shell built in Rust with GPUI, the GPU-accelerated UI framework the new
  Discord client is being ported to (same platform family: Rust + GPUI).
- Inter for text and JetBrains Mono for numbers, at larger sizes than 1.2.
- Animated violet aurora backdrop, boot animation, and a real connect animation
  driven by the engine state, not a fake timer.
- Top navigation with a Basic/Advanced switch. Every setting from 1.2 is here,
  grouped by concern, and still stored in the same next-settings.json.
- The in-stream control center (F10) is now a second GPUI window docked to the
  chosen side, with live telemetry, SR/FG/stream/audio/pad controls and the
  same visual language as the shell. The compact stats card and the disconnect
  banner stay in the engine so they keep working with the panel closed.
- Audio endpoints, displays and GPU adapters are listed live and picked from
  a dialog; nothing has to be typed by index.
- Text size 90-125%, and settings apply about 350 ms after the last edit
  (Apply still forces it immediately).
- Saved profiles from the engine store are listed on Home and in the control
  center, with Apply and Save current.
- Ctrl+1..9 and Ctrl+0 switch tabs, Ctrl+Enter connects.
- The stream window and all media work stay in the C++ engine. The shell
  minimizes itself while you play so it cannot add composition jitter.

Executables
-----------
- brpnext.exe: the 2.0 shell (Rust/GPUI). Start here.
- brpnext-engine.exe: headless stream engine, started by the shell with
  --engine --bridge-port. Do not run it by hand.
- brpnext-classic.exe: the previous 1.2 C++ shell, kept as a fallback.
- brp.exe: the reference product, unchanged.

First run
---------
1. Windows may ask to allow brpnext-engine.exe on the network. Click Allow:
   the console discovery and the video/control streams need it. The shell
   itself only talks to the engine over 127.0.0.1.
2. Home shows the configured console. Connect & Play signs in to the PS5 and
   opens the stream automatically.
3. Esc arms disconnect (banner), Esc again disconnects. F10 opens the
   side-docked control center. Alt+F4 leaves the stream.

Connection and pairing
----------------------
- The address and credentials are read from
  %APPDATA%\BetterRemotePlayNext (the same file the 1.2 shell used), so an
  existing pairing keeps working.
- If the console is missing: Stream > Address. Pairing uses the 8-digit code
  from the PS5 under Settings > System > Remote Play > Link Device.

Video and AI
------------
- Streaming: resolution, fps, codec, decoder, bitrate, frame buffer, HDR.
- Video: output resolution, render target, VSync, VRR, colour space,
  sharpening, display, bit depth, fullscreen on play.
- Neural: DLSS Super Resolution (including DLAA), temporal history, DLSS
  Frame Generation 2x/3x/4x, RIFE, occlusion and motion safety, blend.
- Neural Render is the real depth + optical-flow path. Until its runtime is
  installed the toggles are labelled and do not claim to be active.
- DLSS 5 Neural Rendering runs in video mode through the vendor NGX feature-18
  worker: synthetic depth/motion, history reset each frame, about 10 ms at
  1080p. It enhances lighting and material detail but cannot invent scene
  structure without the game's albedo, normals and camera matrices. HDR frames
  bypass it (8-bit worker). Turn it on under Neural; recommended at 30 fps.

Audio, controller, overlay
--------------------------
- Audio: device, buffer, volume, mute.
- Controller: DualSense haptics, adaptive triggers, rumble, lightbar, poll rate
  125/250/500/1000 Hz.
- Overlay: side (left/right), vertical anchor, scale, opacity, refresh interval,
  compact mode and which sections are shown. F10 toggles the GPUI control
  center; the compact stats card is the engine-side fallback.

Requirements
------------
- Windows 10/11 64-bit.
- A PS5 or PS4 on the same LAN with Remote Play enabled.
- Optional: an NVIDIA RTX GPU for DLSS SR and DLSS Frame Generation. Without
  one the app still streams and upscales.

Updating
--------
- Close the app, extract the patch into this folder and run update.exe. It
  copies only changed files, removes obsolete ones, writes version.txt and
  starts the app again.
- Use the full standalone zip for a clean install.

Your profile
------------
%APPDATA%\BetterRemotePlay holds the legacy profile and session.json for
brp.exe. %APPDATA%\BetterRemotePlayNext holds the Next settings. Neither is
copied into this folder, and this build contains no pairing credentials.

Runtimes
--------
- FFmpeg DLLs for video/audio decoding.
- fonts\ for the engine's own text rendering.
- nr\native\ for the NeuralScreen NGX worker (DLSS NR and DLSS-G presenter).
- nvngx_dlss.dll for DLSS Super Resolution.
- The app is not affiliated with Sony Interactive Entertainment.

Credits
-------
GPUI by Zed Industries (Apache-2.0). Inter and JetBrains Mono under the SIL
Open Font License.
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
