#Requires -Version 5.1
<#
.SYNOPSIS
  Reverts wifi-stability-on.ps1 from its backup.

.DESCRIPTION
  Restores the adapter's advanced power/roaming values, PnPCapabilities and the
  Wireless Adapter Settings of the active power plan. The backup file is
  removed after a successful restore.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\wifi-stability-off.ps1
#>
[CmdletBinding()]
param(
    [switch]$NoRestart,
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'

function Test-Admin {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

if (-not $DryRun -and -not (Test-Admin)) {
    $arguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"")
    if ($NoRestart) { $arguments += '-NoRestart' }
    Start-Process -FilePath 'powershell.exe' -Verb RunAs -ArgumentList $arguments
    exit
}

$backupFile = Join-Path (Join-Path $env:ProgramData 'Veyla') 'wifi-stability-backup.json'
if (-not (Test-Path $backupFile)) {
    throw "No backup found at $backupFile. Nothing to revert."
}
$backup = Get-Content -Path $backupFile -Raw | ConvertFrom-Json

$adapter = Get-NetAdapter -Physical -ErrorAction SilentlyContinue |
    Where-Object { $_.InterfaceGuid -eq $backup.InterfaceGuid } |
    Select-Object -First 1
if (-not $adapter) {
    $adapter = Get-NetAdapter -Physical -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -eq $backup.Adapter } |
        Select-Object -First 1
}
if (-not $adapter) {
    throw "Adapter from the backup is not present: $($backup.Adapter)"
}

Write-Host "Adapter: $($adapter.Name) - $($adapter.InterfaceDescription)"
Write-Host "Backup : $($backup.Created)"
Write-Host ''

foreach ($item in @($backup.Advanced)) {
    if (-not $item) { continue }
    Write-Host ("  {0}: -> '{1}'" -f $item.DisplayName, $item.DisplayValue)
    if (-not $DryRun) {
        try {
            Set-NetAdapterAdvancedProperty -Name $adapter.Name `
                -DisplayName $item.DisplayName -DisplayValue $item.DisplayValue -NoRestart
        } catch {
            Write-Warning "Could not restore '$($item.DisplayName)': $($_.Exception.Message)"
        }
    }
}

if ($backup.PnP -and $backup.PnP.Key) {
    if ($backup.PnP.Existed) {
        Write-Host ("  PnPCapabilities -> {0}" -f $backup.PnP.Value)
        if (-not $DryRun) {
            New-ItemProperty -Path $backup.PnP.Key -Name PnPCapabilities `
                -Value ([int]$backup.PnP.Value) -PropertyType DWord -Force | Out-Null
        }
    } else {
        Write-Host '  PnPCapabilities -> removed (was not set)'
        if (-not $DryRun) {
            Remove-ItemProperty -Path $backup.PnP.Key -Name PnPCapabilities `
                -ErrorAction SilentlyContinue
        }
    }
}

if ($backup.Power -and $backup.Power.Scheme) {
    $scheme = $backup.Power.Scheme
    $wirelessSub = '19cbb8fa-5279-450e-9fac-8a3d5fedd0c1'
    $wirelessSet = '12bbebe6-58d6-4636-95bb-3217ef867c1a'
    if ($backup.Power.ACExisted) {
        Write-Host ("  Wireless Adapter Settings AC -> {0}" -f $backup.Power.AC)
        if (-not $DryRun) {
            powercfg /setacvalueindex $scheme $wirelessSub $wirelessSet ([int]$backup.Power.AC) | Out-Null
        }
    }
    if ($backup.Power.DCExisted) {
        Write-Host ("  Wireless Adapter Settings DC -> {0}" -f $backup.Power.DC)
        if (-not $DryRun) {
            powercfg /setdcvalueindex $scheme $wirelessSub $wirelessSet ([int]$backup.Power.DC) | Out-Null
        }
    }
    if (-not $DryRun) {
        powercfg /setactive $scheme | Out-Null
    }
}

if ($DryRun) {
    Write-Host ''
    Write-Host 'Dry run: nothing written.'
    return
}

if (-not $NoRestart) {
    Write-Host 'Restarting the adapter...'
    Restart-NetAdapter -Name $adapter.Name -Confirm:$false
    Start-Sleep -Seconds 4
}

Remove-Item -Path $backupFile -Force -ErrorAction SilentlyContinue
Write-Host 'Original values restored.'
