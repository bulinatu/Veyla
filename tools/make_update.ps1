# Builds a small update zip between two packaged versions.
#
#   powershell -File tools\make_update.ps1 `
#       -BaselineZip dist\BetterRemotePlay-0.3-standalone.zip `
#       -NewFolder dist\BetterRemotePlay -Version 0.4
#
# The result contains update.exe, payload\ (changed and new files only),
# delete.txt (files that no longer exist) and version.txt. The friend extracts
# it into the app folder and runs update.exe.

param(
    [Parameter(Mandatory = $true)][string]$BaselineZip,
    [string]$NewFolder = "dist\BetterRemotePlay",
    [Parameter(Mandatory = $true)][string]$Version,
    [string]$Out = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

function Resolve-UnderRoot([string]$path) {
    if ([System.IO.Path]::IsPathRooted($path)) { return $path }
    return (Join-Path $root $path)
}

$baseline = Resolve-UnderRoot $BaselineZip
$new = Resolve-UnderRoot $NewFolder
if (-not (Test-Path -LiteralPath $baseline)) { throw "Baseline zip not found: $baseline" }
if (-not (Test-Path -LiteralPath $new)) { throw "New folder not found: $new" }

$updateExe = Join-Path $root "build\Release\brp_update.exe"
if (-not (Test-Path -LiteralPath $updateExe)) {
    throw "build\Release\brp_update.exe missing. Build the brp_update target first."
}

$temp = Join-Path $env:TEMP ("brp_patch_" + [guid]::NewGuid().ToString("N"))
$baseDir = Join-Path $temp "base"
$stage = Join-Path $temp "stage"
$payload = Join-Path $stage "payload"
New-Item -ItemType Directory -Path $baseDir, $payload -Force | Out-Null

try {
    Expand-Archive -LiteralPath $baseline -DestinationPath $baseDir -Force
    # The standalone zips wrap everything in a "BetterRemotePlay" folder.
    $inner = @(Get-ChildItem -LiteralPath $baseDir)
    if ($inner.Count -eq 1 -and $inner[0].PSIsContainer) {
        $baseDir = $inner[0].FullName
    }

    Copy-Item -LiteralPath $updateExe -Destination (Join-Path $stage "update.exe") -Force
    Set-Content -LiteralPath (Join-Path $stage "version.txt") -Value $Version -Encoding ASCII

    $skip = @("update.exe", "payload", "delete.txt", "version.txt")
    $deletes = New-Object System.Collections.Generic.List[string]
    $added = 0
    $changed = 0

    $newRoot = (Resolve-Path -LiteralPath $new).Path
    Get-ChildItem -LiteralPath $newRoot -Recurse -File | ForEach-Object {
        $rel = $_.FullName.Substring($newRoot.Length).TrimStart('\')
        $first = ($rel -split '\\')[0]
        if ($skip -contains $first) { return }

        $oldFile = Join-Path $baseDir $rel
        $same = $false
        if (Test-Path -LiteralPath $oldFile) {
            $oldLen = (Get-Item -LiteralPath $oldFile).Length
            if ($oldLen -eq $_.Length) {
                $same = (Get-FileHash -LiteralPath $oldFile -Algorithm SHA256).Hash -eq
                        (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
            }
        }
        if ($same) { return }

        $target = Join-Path $payload $rel
        New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
        Copy-Item -LiteralPath $_.FullName -Destination $target -Force
        if (Test-Path -LiteralPath $oldFile) { $changed++ } else { $added++ }
    }

    Get-ChildItem -LiteralPath $baseDir -Recurse -File | ForEach-Object {
        $rel = $_.FullName.Substring((Resolve-Path -LiteralPath $baseDir).Path.Length).TrimStart('\')
        if (-not (Test-Path -LiteralPath (Join-Path $newRoot $rel))) {
            $deletes.Add($rel)
        }
    }
    if ($deletes.Count -gt 0) {
        Set-Content -LiteralPath (Join-Path $stage "delete.txt") -Value $deletes -Encoding ASCII
    }

    if ([string]::IsNullOrWhiteSpace($Out)) {
        $Out = Join-Path $root ("dist\BetterRemotePlay-$Version-update.zip")
    } else {
        $Out = Resolve-UnderRoot $Out
    }
    if (Test-Path -LiteralPath $Out) { Remove-Item -LiteralPath $Out -Force }
    Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $Out -CompressionLevel Optimal

    $size = [math]::Round((Get-Item -LiteralPath $Out).Length / 1MB, 1)
    "Patch:   $Out"
    "Version: $Version"
    "Added:   $added file(s)"
    "Changed: $changed file(s)"
    "Deleted: $($deletes.Count) file(s)"
    "Size:    $size MB"
} finally {
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}
