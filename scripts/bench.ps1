param(
    [int]$Frames = 800,
    [string]$Out = "build\bench.csv"
)

# Runs the full resolution x upscaler matrix and writes CSV.
# Requires a release build: .\scripts\build.ps1

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root "build\Release\brp-bench.exe"
if (-not (Test-Path $exe)) { throw "missing $exe - run scripts\build.ps1 first" }

$modes = @{ 0 = "bilinear"; 1 = "catmull-rom"; 2 = "catmull-rom+unsharp" }
$rows = @()

foreach ($res in @(@(1920, 1080), @(2560, 1440), @(3840, 2160))) {
    foreach ($m in 0, 1, 2) {
        $out = & $exe --width $res[0] --height $res[1] --mode $m --frames $Frames 2>&1 | Out-String
        $g = [regex]::Match($out, 'gpu ms\s+: min ([\d\.]+)\s+p50 ([\d\.]+)\s+p95 ([\d\.]+)\s+p99 ([\d\.]+)')
        $rows += [pscustomobject]@{
            output = "$($res[0])x$($res[1])"
            mode   = $modes[$m]
            min_ms = $g.Groups[1].Value
            p50_ms = $g.Groups[2].Value
            p95_ms = $g.Groups[3].Value
            p99_ms = $g.Groups[4].Value
        }
    }
}

$csv = Join-Path $root $Out
New-Item -ItemType Directory -Force -Path (Split-Path $csv) | Out-Null
$rows | Export-Csv -NoTypeInformation -Path $csv
$rows | Format-Table -AutoSize
Write-Host "wrote $csv"
