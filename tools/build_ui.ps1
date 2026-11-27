# Builds the GPUI shell. GPUI's build script compiles its HLSL shaders with
# fxc.exe, which lives in the Windows SDK and is not on PATH by default.
$ErrorActionPreference = "Stop"
$sdk = Get-ChildItem "C:\Program Files (x86)\Windows Kits\10\bin" -Directory -ErrorAction SilentlyContinue |
    Where-Object { Test-Path (Join-Path $_.FullName "x64\fxc.exe") } |
    Sort-Object Name -Descending | Select-Object -First 1
if (-not $sdk) { throw "Windows SDK fxc.exe not found under Windows Kits\10\bin" }
$env:PATH = (Join-Path $sdk.FullName "x64") + ";" + $env:PATH
Push-Location (Join-Path $PSScriptRoot "..\ui")
try {
    & "$env:USERPROFILE\.cargo\bin\cargo.exe" build --release @args
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
} finally {
    Pop-Location
}
