# Copies the optional, proprietary NVIDIA VFX runtime used by the frame
# generator into the project (and optionally next to brp.exe).
#
# The runtime is NOT redistributable by this project. Obtain it from the NVIDIA
# Video Effects (Maxine) SDK or from a Magpie Experimental install and pass its
# folder here. Source files are gitignored.
#
# Usage:
#   .\scripts\install-vfx-runtime.ps1 -Source "C:\path\to\Magpie-Experimental-x64"
#   .\scripts\install-vfx-runtime.ps1 -Source ... -AppDir ".\build\Release"

param(
    [Parameter(Mandatory = $true)][string]$Source,
    [string]$Dest = "",
    [string]$AppDir = ""
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $Source)) {
    throw "source folder not found: $Source"
}

$files = @(
    "nvngxruntime.dll", "NVCVImage.dll", "NVVideoEffects.dll",
    "nvVFXVideoFrameGeneration.dll", "cudart64_12.dll",
    "nppc64_12.dll", "nppial64_12.dll", "nppicc64_12.dll", "nppidei64_12.dll",
    "nppif64_12.dll", "nppig64_12.dll", "nppim64_12.dll", "nppist64_12.dll",
    "nppitc64_12.dll"
)

if ([string]::IsNullOrWhiteSpace($Dest)) {
    $Dest = Join-Path $PSScriptRoot "..\third_party\nvidia-vfx\runtime"
}
$Dest = [System.IO.Path]::GetFullPath($Dest)
New-Item -ItemType Directory -Force -Path $Dest | Out-Null

$missing = @()
foreach ($file in $files) {
    $path = Join-Path $Source $file
    if (-not (Test-Path $path)) {
        $missing += $file
        continue
    }
    Copy-Item $path $Dest -Force
}

foreach ($extra in @("nvidia-vfx-runtime-manifest.json")) {
    $path = Join-Path $Source $extra
    if (Test-Path $path) {
        Copy-Item $path $Dest -Force
    }
}
$licenseSource = Join-Path $Source "NVIDIA-VFX-Licenses"
if (Test-Path $licenseSource) {
    Copy-Item $licenseSource (Join-Path $Dest "..") -Recurse -Force
}

if ($missing.Count -gt 0) {
    Write-Warning ("missing from source: " + ($missing -join ", "))
}
if (-not (Test-Path (Join-Path $Dest "nvVFXVideoFrameGeneration.dll"))) {
    throw "nvVFXVideoFrameGeneration.dll was not copied; check -Source"
}

Write-Host "NVIDIA VFX runtime installed to $Dest"

if (-not [string]::IsNullOrWhiteSpace($AppDir)) {
    $target = Join-Path $AppDir "nvidia-vfx\runtime"
    New-Item -ItemType Directory -Force -Path $target | Out-Null
    Copy-Item (Join-Path $Dest "*") $target -Recurse -Force
    Write-Host "Copied next to the app: $target"
}
