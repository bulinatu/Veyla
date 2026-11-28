# Builds a self-contained BetterRemotePlay folder (and zip) from build/Release.
# The user profile is never copied: session.json lives in %APPDATA%, not here.

param(
    [switch]$NoZip,
    [string]$Version = ""
)

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$release = Join-Path $root "build\Release"
$distRoot = Join-Path $root "dist"
$dest = Join-Path $distRoot "Veyla"
$version = $Version
if ([string]::IsNullOrWhiteSpace($version)) {
    $cmakeText = Get-Content -LiteralPath (Join-Path $root "CMakeLists.txt") -Raw
    if ($cmakeText -match "project\s*\(\s*BetterRemotePlay\s+VERSION\s+([0-9.]+)") {
        $version = $Matches[1]
    }
}
if ([string]::IsNullOrWhiteSpace($version)) { $version = "0.0.0" }

function Require-File([string]$path) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing required file: $path"
    }
}

function Copy-Required([string]$source, [string]$destination) {
    Require-File $source
    Copy-Item -LiteralPath $source -Destination $destination -Force
}

if (-not (Test-Path (Join-Path $release "veyla-legacy.exe"))) {
    throw "build\Release\veyla-legacy.exe not found. Build the Release target first."
}

$running = Get-Process veyla,veyla-engine,veyla-classic,veyla-legacy,brp,brpnext,brpnext-engine -ErrorAction SilentlyContinue |
    Where-Object { $_.Path -and $_.Path.StartsWith($dest, [System.StringComparison]::OrdinalIgnoreCase) }
if ($running) {
    throw "Veyla is running from $dest. Close it before packaging."
}
if (Test-Path $dest) {
    Remove-Item -LiteralPath $dest -Recurse -Force
}
New-Item -ItemType Directory -Path $dest -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $dest "fonts") -Force | Out-Null

# --- core executable and FFmpeg runtime ------------------------------------
Copy-Required (Join-Path $release "veyla-legacy.exe") (Join-Path $dest "veyla-legacy.exe")
# 2.0 shell (Rust/GPUI) plus the headless engine it starts.
$uiRelease = Join-Path $root "ui\target\release\veyla.exe"
Copy-Required $uiRelease (Join-Path $dest "veyla.exe")
Copy-Required (Join-Path $release "veyla-engine.exe") $dest
Copy-Required (Join-Path $release "veyla-classic.exe") (Join-Path $dest "veyla-classic.exe")

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

# The executables embed this icon (resource id 1); a loose copy lets a user
# point a manual shortcut at it as well.
Copy-Required (Join-Path $root "assets\app.ico") (Join-Path $dest "app.ico")

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

# --- readme -----------------------------------------------------------------
$readme = @"
Veyla 2.7.5 - standalone build
=======================================

Run veyla.exe. No installer, no console window, no background service.

What changed in 2.7.5
---------------------
- Settings > System > Logging has a new "All" level, next to Error / Warn /
  Info / Debug. It records everything the app and the stream can report.
- Stream diagnostics now reach next.log: connect/start/failure events, one line
  per second with fps, lost frames, decode time, queue, buffer drops, arrival
  jitter and bitrate, plus presentation stats (shown fps, age, cadence, misses).
- Decoder, DX12 surface, DLSS/FSR/DLSS-G failures and shell/engine bridge
  messages are written to the same log instead of a stderr no GUI build shows.
- Log lines include the date, and the file rotates at 8 MiB.

What changed in 2.7.1
---------------------
- The window no longer stays above other applications: any inherited
  always-on-top flag is cleared at startup and whenever the control center
  closes.
- The first-run "Set up your PS5" chip sits centred just above the dock instead
  of under the logo, so it is harder to miss.

What changed in 2.7.0
---------------------
- New users can set up a console inside the app: a "Set up your PS5" chip under
  the logo opens Settings > Pairing, with LAN discovery, PIN pairing and the PSN
  account link. Before 2.7.0 there was no setup screen at all and pairing had to
  be done in veyla-classic.exe (Streaming > Console).
- The chip only appears while no console is paired.

What changed in 2.6.2
---------------------
- Navigation moved to a dock at the bottom of the window, with icons and
  labels; the top bar is gone and the window buttons float in place.
- Updates are delivered as a delta: update.exe carries only the files that
  changed, instead of the whole application.
- The updater writes %TEMP%\veyla-update.log, retries when a file is still
  locked, and relaunches the existing build if it cannot apply a patch instead
  of leaving no window at all. A failure also shows up in the app on next start.
- The control center no longer squeezes row text: the Passes hint is a hint, and
  sliders sit under their label.
- F10 opens the control center even before connecting.

What changed in 2.6.1
---------------------
- Wi-Fi: Optimize detects the link and uses 30 Mbps instead of 100 Mbps, and
  keeps frame generation off there, so the access point stops queueing.
- Decode queue is capped at 2 frames with frame buffer 0 (was 6); a slow decode
  can no longer add up to 100 ms of backlog.
- Optimize retries the device list and falls back to the status snapshot, so it
  no longer fails with "the engine did not report hardware".
- The update repository is no longer written empty, which used to make
  "Check now" fail with "Set a GitHub repository first".

What changed in 2.6
-------------------
- The product is now Veyla. Window titles, the shell header, About, the updater
  and the executables use the new name; settings and pairings are untouched
  because they still live in %APPDATA%\BetterRemotePlayNext.
- New icon and logo: a violet-to-indigo tile with a bold V, identical in the
  taskbar and in the shell header.
- Updates come from GitHub Releases. The shell checks at launch and Advanced >
  Updates shows the same card; the update installs silently and reopens Veyla.
- Saved profiles are capped at 12, can be deleted (Delete then Confirm), and
  the Home card shows "n / 12" and scrolls instead of growing forever.
- PS4 is detected and paired, but video is PS5-only for now: the stream is
  HEVC, and PS4 Remote Play is H.264.

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
- veyla.exe: the shell (Rust/GPUI). Start here.
- veyla-engine.exe: headless stream engine, started by the shell with
  --engine --bridge-port. Do not run it by hand.
- veyla-classic.exe: the previous C++ shell, kept as a fallback.
- veyla-legacy.exe: the original 0.5 reference shell.

First run
---------
1. Windows may ask to allow veyla-engine.exe on the network. Click Allow:
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
  Frame Generation 2x/3x/4x, occlusion and motion safety.
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

Brand
-----
Veyla is the product name. BetterRemotePlay is the original project name and
remains a trademark of its author; it is kept in compatibility paths such as
%APPDATA%\BetterRemotePlayNext. All code and branding in this package are
original work.

Credits
-------
GPUI by Zed Industries (Apache-2.0). Inter and JetBrains Mono under the SIL
Open Font License.
"@
Set-Content -LiteralPath (Join-Path $dest "README.txt") -Value $readme -Encoding UTF8
# --- zip --------------------------------------------------------------------
$suffix = ""
$zip = Join-Path $distRoot "Veyla-$version-standalone$suffix.zip"
if (-not $NoZip) {
    if (Test-Path $zip) {
        Remove-Item -LiteralPath $zip -Force
    }
    Push-Location $distRoot
    try {
        & tar.exe -a -c -f $zip "Veyla"
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
