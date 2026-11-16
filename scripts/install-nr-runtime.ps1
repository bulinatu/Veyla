# Copies the optional, proprietary NGX Neural Rendering worker used by
# DLSSNR. The runtime is NOT redistributable by this project; obtain it from
# the matching NeuralScreen/Magpie package (the folder that contains
# native\nvngx.dll and native\nvngx_dlssnr.dll).
#
# Usage:
#   .\scripts\install-nr-runtime.ps1 -Source "C:\path\to\neuralscreen-v2.6.0-full"
#   .\scripts\install-nr-runtime.ps1 -Source ... -AppDir ".\build\Release"

param(
    [Parameter(Mandatory = $true)][string]$Source,
    [string]$Dest = "",
    [string]$AppDir = ""
)

$ErrorActionPreference = "Stop"
if (-not (Test-Path $Source)) {
    throw "source folder not found: $Source"
}

$native = $Source
if (Test-Path (Join-Path $Source "native\nvngx.dll")) {
    $native = Join-Path $Source "native"
}
if (-not (Test-Path (Join-Path $native "nvngx.dll")) -or
    -not (Test-Path (Join-Path $native "nvngx_dlssnr.dll"))) {
    throw "native\nvngx.dll and native\nvngx_dlssnr.dll not found under $Source"
}

if ([string]::IsNullOrWhiteSpace($Dest)) {
    $Dest = Join-Path $PSScriptRoot "..\third_party\neuralscreen\native"
}
$Dest = [System.IO.Path]::GetFullPath($Dest)
New-Item -ItemType Directory -Force -Path $Dest | Out-Null

# The worker also loads the forwarder and, depending on the build, Spout and
# the FG snippet. Copy the whole native folder to preserve the vendor layout.
Copy-Item (Join-Path $native "*") $Dest -Recurse -Force
Write-Host "NGX NR runtime installed to $Dest"

if (-not [string]::IsNullOrWhiteSpace($AppDir)) {
    $target = Join-Path $AppDir "nr\native"
    New-Item -ItemType Directory -Force -Path $target | Out-Null
    Copy-Item (Join-Path $Dest "*") $target -Recurse -Force
    Write-Host "Copied next to the app: $target"
}
