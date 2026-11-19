# Builds a single self-contained update.exe between two packaged versions.
#
#   powershell -File tools\make_update.ps1 `
#       -BaselineZip dist\BetterRemotePlay-0.4-standalone.zip `
#       -NewFolder dist\BetterRemotePlay -Version 0.4.1
#
# The output is one file: update.exe. The changed files are packed in a zip and
# appended to the updater, followed by a 24-byte footer (magic, offset, size).
# Run it inside the app folder; it extracts itself over brp.exe.

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
New-Item -ItemType Directory -Path $baseDir, $stage -Force | Out-Null

try {
    Expand-Archive -LiteralPath $baseline -DestinationPath $baseDir -Force
    $inner = @(Get-ChildItem -LiteralPath $baseDir)
    if ($inner.Count -eq 1 -and $inner[0].PSIsContainer) {
        $baseDir = $inner[0].FullName
    }

    $skip = @("update.exe", "payload", "delete.txt", "_delete.txt", "version.txt",
              "_version.txt")
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

        $target = Join-Path $stage $rel
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
        Set-Content -LiteralPath (Join-Path $stage "_delete.txt") -Value $deletes -Encoding ASCII
    }
    Set-Content -LiteralPath (Join-Path $stage "_version.txt") -Value $Version -Encoding ASCII

    $payloadZip = Join-Path $temp "payload.zip"
    Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $payloadZip -CompressionLevel Optimal

    if ([string]::IsNullOrWhiteSpace($Out)) {
        $Out = Join-Path $root ("dist\BetterRemotePlay-$Version-update.exe")
    } else {
        $Out = Resolve-UnderRoot $Out
    }
    if (Test-Path -LiteralPath $Out) { Remove-Item -LiteralPath $Out -Force }

    $baseBytes = [System.IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $updateExe).Path)
    $zipBytes = [System.IO.File]::ReadAllBytes($payloadZip)
    $stream = [System.IO.File]::Create($Out)
    try {
        $stream.Write($baseBytes, 0, $baseBytes.Length)
        $stream.Write($zipBytes, 0, $zipBytes.Length)
        $magic = [System.Text.Encoding]::ASCII.GetBytes("BRPUP01")
        $stream.Write($magic, 0, 8)                       # 7 chars + the NUL byte
        $stream.Write([BitConverter]::GetBytes([uint64]$baseBytes.Length), 0, 8)
        $stream.Write([BitConverter]::GetBytes([uint64]$zipBytes.Length), 0, 8)
    } finally {
        $stream.Close()
    }

    $size = [math]::Round((Get-Item -LiteralPath $Out).Length / 1MB, 2)
    "Update:  $Out"
    "Version: $Version"
    "Added:   $added file(s)"
    "Changed: $changed file(s)"
    "Deleted: $($deletes.Count) file(s)"
    "Size:    $size MB (single file)"
} finally {
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}
