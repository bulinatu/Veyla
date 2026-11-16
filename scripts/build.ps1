param(
    [switch]$Clean,
    [string]$Target = ""
)

# Builds with the Visual Studio generator so no vcvars bootstrap is needed.
# The C++ workload (MSVC + Windows SDK) must already be installed.

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root "build"

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    throw "vswhere.exe not found. Install Visual Studio Build Tools with 'Desktop development with C++'."
}

$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) {
    throw "No Visual Studio installation with C++ tools found."
}

if ($Clean -and (Test-Path $build)) {
    Remove-Item -Recurse -Force $build
}

Write-Host "configuring ($vsPath)"
& cmake -S $root -B $build -G "Visual Studio 17 2022" -A x64
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "building"
$buildArgs = @("--build", $build, "--config", "Release", "--parallel")
if ($Target) { $buildArgs += @("--target", $Target) }
& cmake @buildArgs
exit $LASTEXITCODE
