# Copies the optional, proprietary NVIDIA NGX feature runtime used by DLSS
# Super Resolution. The runtime is NOT redistributable by this project; obtain
# nvngx_dlss.dll from the matching NVIDIA DLSS package (the Magpie-Experimental
# folder ships a known-good copy).
#
# Usage:
#   .\scripts\install-ngx-runtime.ps1 -Source "C:\path\to\Magpie-Experimental-x64"
#   .\scripts\install-ngx-runtime.ps1 -Source ... -AppDir ".\build\Release"

param(
    [Parameter(Mandatory = $true)][string]$Source,
    [string]$Dest = "",
    [string]$AppDir = ""
)

$ErrorActionPreference = "Stop"
if (-not (Test-Path $Source)) {
    throw "source folder not found: $Source"
}

$files = @("nvngx_dlss.dll")
if ([string]::IsNullOrWhiteSpace($Dest)) {
    $Dest = Join-Path $PSScriptRoot "..\third_party\nvidia-ngx\runtime"
}
$Dest = [System.IO.Path]::GetFullPath($Dest)
New-Item -ItemType Directory -Force -Path $Dest | Out-Null

foreach ($file in $files) {
    $path = Join-Path $Source $file
    if (-not (Test-Path $path)) {
        throw "$file not found in $Source"
    }
    Copy-Item $path $Dest -Force
}

Write-Host "NVIDIA NGX DLSS runtime installed to $Dest"

if (-not [string]::IsNullOrWhiteSpace($AppDir)) {
    New-Item -ItemType Directory -Force -Path $AppDir | Out-Null
    Copy-Item (Join-Path $Dest "nvngx_dlss.dll") $AppDir -Force
    # The NGX feature discovery looks in the application directory.
    Write-Host "Copied next to the app: $AppDir\nvngx_dlss.dll"
}
