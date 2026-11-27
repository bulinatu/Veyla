# Builds the artifacts for a GitHub release.
#
#   powershell -File tools\publish_release.ps1 -Version 2.6.0 -Repo owner/name
#
# Produces:
#   dist\Veyla-<version>-update.exe     (full update, any older install)
#   dist\Veyla-<version>-standalone.zip (fresh install)
#   dist\release-<version>.md                      (release notes template)
#
# The in-app updater reads https://api.github.com/repos/<repo>/releases and
# picks the first .exe asset whose name contains "update". The full update is
# built against an empty baseline, so it does not depend on the version the
# user has installed. If the GitHub CLI is available and GH_TOKEN is set, the
# release is created; otherwise the exact command is printed.

param(
    [Parameter(Mandatory = $true)][string]$Version,
    [string]$Repo = "",
    [string]$Title = "",
    [string]$Notes = "",
    [switch]$SkipPackage,
    [switch]$DryRun
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$dist = Join-Path $root "dist"
$release = Join-Path $dist "Veyla"
$updateOut = Join-Path $dist "Veyla-$Version-update.exe"
$notesPath = Join-Path $dist "release-$Version.md"

if (-not $SkipPackage) {
    Write-Host "Packaging dist\Veyla ..."
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "package_release.ps1") -Version $Version
    if ($LASTEXITCODE -ne 0) { throw "package_release.ps1 failed" }
}

if (-not (Test-Path -LiteralPath (Join-Path $release "veyla.exe"))) {
    throw "dist\Veyla\veyla.exe is missing. Package first."
}

# Stamp the version the updater writes into version.txt.
Set-Content -LiteralPath (Join-Path $release "version.txt") -Value $Version -Encoding ASCII

$empty = Join-Path $env:TEMP ("brp_empty_" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $empty -Force | Out-Null
try {
    Write-Host "Building the full update payload ..."
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "make_update.ps1") `
        -BaselineFolder $empty -NewFolder $release -Version $Version -Out $updateOut
    if ($LASTEXITCODE -ne 0) { throw "make_update.ps1 failed" }
} finally {
    Remove-Item -LiteralPath $empty -Recurse -Force -ErrorAction SilentlyContinue
}

$zip = Join-Path $dist "Veyla-$Version-standalone.zip"
if (-not (Test-Path -LiteralPath $zip)) {
    throw "Standalone archive not found: $zip"
}

$notesText = if ($Notes.Trim().Length -gt 0) {
    $Notes
} else {
    "BetterRemotePlay $Version`n`n- See the commit history for details.`n"
}
Set-Content -LiteralPath $notesPath -Value $notesText -Encoding UTF8

Write-Host ""
Write-Host "Artifacts:"
Get-Item -LiteralPath $updateOut, $zip | ForEach-Object {
    Write-Host ("  {0}  ({1:N1} MB)" -f $_.Name, ($_.Length / 1MB))
}
Write-Host ("  {0}  (release notes)" -f (Split-Path -Leaf $notesPath))

$tag = "v$Version"
if ([string]::IsNullOrWhiteSpace($Title)) { $Title = "BetterRemotePlay $Version" }

$gh = Get-Command gh -ErrorAction SilentlyContinue
if ($gh -and $Repo -and $env:GH_TOKEN -and -not $DryRun) {
    Write-Host "Creating the GitHub release ..."
    & $gh.Source release create $tag $updateOut $zip `
        --repo $Repo --title $Title --notes-file $notesPath
    if ($LASTEXITCODE -ne 0) { throw "gh release create failed" }
    Write-Host "Published $tag on $Repo"
} else {
    Write-Host ""
    Write-Host "Publish manually (tag $tag):"
    if ($Repo) {
        Write-Host "  gh release create $tag `"$updateOut`" `"$zip`" --repo $Repo --title `"$Title`" --notes-file `"$notesPath`""
    } else {
        Write-Host "  1. Create the repository release with tag $tag"
        Write-Host "  2. Upload $([System.IO.Path]::GetFileName($updateOut)) as the update asset"
        Write-Host "  3. Upload $([System.IO.Path]::GetFileName($zip)) for fresh installs"
        Write-Host "  4. Set general.update_repo to owner/name in Settings > Advanced > Updates"
    }
    if ($Repo) {
        Write-Host ""
        Write-Host "Then set general.update_repo to '$Repo' (Settings > Advanced > Updates)."
    }
}
