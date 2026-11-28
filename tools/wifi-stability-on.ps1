#Requires -Version 5.1
<#
.SYNOPSIS
  Applies the Wi-Fi settings that reduce periodic video stalls, without
  disabling Wi-Fi 7 or MLO.

.DESCRIPTION
  Changes only:
    * adapter power saving (power-save / MIMO power-save properties)
    * Windows "allow the computer to turn off this device" (PnPCapabilities)
    * the active power plan's Wireless Adapter Settings -> Maximum Performance
    * roaming aggressiveness -> lowest

  It does NOT touch Wi-Fi 7, MLO, 802.11be, band, channel width or any other
  radio mode. Every original value is written to a backup file used by
  wifi-stability-off.ps1.

.PARAMETER AdapterName
  Optional adapter name (Get-NetAdapter -Name). Defaults to the active 802.11
  adapter.

.PARAMETER NoRestart
  Do not restart the adapter. A reboot is then required for PnPCapabilities.

.PARAMETER DryRun
  Print what would change and exit; nothing is written.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\wifi-stability-on.ps1
  powershell -ExecutionPolicy Bypass -File tools\wifi-stability-on.ps1 -DryRun
#>
[CmdletBinding()]
param(
    [string]$AdapterName = '',
    [switch]$NoRestart,
    [switch]$DryRun,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'

function Test-Admin {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Get-WifiAdapter {
    param([string]$Name)
    $adapters = @(Get-NetAdapter -Physical -ErrorAction SilentlyContinue |
        Where-Object {
            $_.Status -ne 'Not Present' -and
            ($_.MediaType -eq 'Native 802.11' -or $_.InterfaceDescription -match '(?i)wi-?fi|wireless|wlan')
        })
    if ($Name) {
        $adapters = @($adapters | Where-Object { $_.Name -eq $Name })
    }
    if ($adapters.Count -eq 0) {
        throw 'No 802.11 adapter found.'
    }
    $active = $adapters | Where-Object { $_.Status -eq 'Up' } | Select-Object -First 1
    if ($active) { return $active }
    return $adapters[0]
}

function Find-Prop {
    param($Properties, [string[]]$Patterns, [string]$Exclude)
    foreach ($p in $Properties) {
        if ($Exclude -and ($p.DisplayName -match $Exclude -or $p.RegistryKeyword -match $Exclude)) {
            continue
        }
        foreach ($pattern in $Patterns) {
            if ($p.DisplayName -match $pattern -or $p.RegistryKeyword -match $pattern) {
                return $p
            }
        }
    }
    return $null
}

function Pick-PowerValue {
    param([string[]]$Values)
    $value = $Values | Where-Object {
        $_ -match '(?i)disable|off|maximum performance|cam|no smps|none|highest'
    } | Select-Object -First 1
    if (-not $value -and $Values.Count -le 2) {
        $value = $Values | Where-Object { $_ -notmatch '(?i)enable|on' } | Select-Object -First 1
    }
    return $value
}

function Pick-RoamingValue {
    param([string[]]$Values)
    # Values are usually "1. Lowest" .. "5. Highest"; sort numerically so
    # "3. Medium-Low" cannot win just because it contains "low".
    $numbered = $Values | Where-Object { $_ -match '^\s*(\d+)' }
    if ($numbered) {
        return $numbered | Sort-Object {
            [int]([regex]'^\s*(\d+)').Match($_).Groups[1].Value
        } | Select-Object -First 1
    }
    $value = $Values | Where-Object { $_ -match '(?i)lowest|low|min' } | Select-Object -First 1
    return $value
}

if (-not $DryRun -and -not (Test-Admin)) {
    $arguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"")
    if ($AdapterName) { $arguments += @('-AdapterName', $AdapterName) }
    if ($NoRestart) { $arguments += '-NoRestart' }
    if ($Force) { $arguments += '-Force' }
    Start-Process -FilePath 'powershell.exe' -Verb RunAs -ArgumentList $arguments
    exit
}

$adapter = Get-WifiAdapter -Name $AdapterName
Write-Host "Adapter: $($adapter.Name) - $($adapter.InterfaceDescription)"
Write-Host "Status : $($adapter.Status) / $($adapter.LinkSpeed)"

$properties = @(Get-NetAdapterAdvancedProperty -Name $adapter.Name -ErrorAction SilentlyContinue)
$exclude = '(?i)wi-?fi\s*7|mlo|802\.11be|wi-?fi\s*6|802\.11ax|band|channel|width'

$powerProp = Find-Prop -Properties $properties -Patterns @(
    'Power Saving', 'Power Save', 'MIMO Power Save', 'U-APSD', '802\.11 Power'
) -Exclude $exclude
$roamProp = Find-Prop -Properties $properties -Patterns @(
    'Roaming Aggressiveness', 'Roam Aggressiveness', 'Roaming Preference', 'Roaming Mode'
) -Exclude $exclude

$changes = @()
if ($powerProp) {
    $values = @($powerProp.ValidDisplayValues)
    $target = Pick-PowerValue -Values $values
    if ($target -and $target -ne $powerProp.DisplayValue) {
        $changes += [pscustomobject]@{
            Kind        = 'Power'
            DisplayName = $powerProp.DisplayName
            From        = $powerProp.DisplayValue
            To          = $target
        }
    } elseif ($target) {
        Write-Host "Power : already '$($powerProp.DisplayValue)'"
    } else {
        Write-Host "Power : no suitable value for '$($powerProp.DisplayName)'"
    }
} else {
    Write-Host 'Power : no power-save property exposed by this driver'
}

if ($roamProp) {
    $values = @($roamProp.ValidDisplayValues)
    $target = Pick-RoamingValue -Values $values
    if ($target -and $target -ne $roamProp.DisplayValue) {
        $changes += [pscustomobject]@{
            Kind        = 'Roaming'
            DisplayName = $roamProp.DisplayName
            From        = $roamProp.DisplayValue
            To          = $target
        }
    } elseif ($target) {
        Write-Host "Roam  : already '$($roamProp.DisplayValue)'"
    } else {
        Write-Host "Roam  : no suitable value for '$($roamProp.DisplayName)'"
    }
} else {
    Write-Host 'Roam  : no roaming property exposed by this driver'
}

# Power plan: Wireless Adapter Settings -> Maximum Performance (AC and DC).
$schemeText = (powercfg /getactivescheme) -join ' '
$scheme = ([regex]'[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}').Match($schemeText).Value
$wirelessSub = '19cbb8fa-5279-450e-9fac-8a3d5fedd0c1'
$wirelessSet = '12bbebe6-58d6-4636-95bb-3217ef867c1a'
$powerKey = "HKLM:\SYSTEM\CurrentControlSet\Control\Power\User\PowerSchemes\$scheme\$wirelessSub\$wirelessSet"
$acValue = $null
$dcValue = $null
if (Test-Path $powerKey) {
    $current = Get-ItemProperty -Path $powerKey -ErrorAction SilentlyContinue
    if ($null -ne $current.ACSettingIndex) { $acValue = [int]$current.ACSettingIndex }
    if ($null -ne $current.DCSettingIndex) { $dcValue = [int]$current.DCSettingIndex }
}
# A hidden power setting may have no registry override yet; read the effective
# value from powercfg so reverting restores the real original.
if ($null -eq $acValue -or $null -eq $dcValue) {
    $query = (powercfg /query $scheme $wirelessSub $wirelessSet 2>$null) -join "`n"
    $hex = [regex]::Matches($query, '0x([0-9a-fA-F]{8})')
    if ($hex.Count -ge 2) {
        if ($null -eq $acValue) {
            $acValue = [Convert]::ToInt32($hex[0].Groups[1].Value, 16)
        }
        if ($null -eq $dcValue) {
            $dcValue = [Convert]::ToInt32($hex[1].Groups[1].Value, 16)
        }
    }
}

# PnPCapabilities: 0x18 removes "allow the computer to turn off this device".
$classKey = 'HKLM:\SYSTEM\CurrentControlSet\Control\Class\{4d36e972-e325-11ce-bfc1-08002be10318}'
$instanceKey = $null
foreach ($candidate in @(Get-ChildItem $classKey -ErrorAction SilentlyContinue)) {
    $props = Get-ItemProperty -Path $candidate.PSPath -ErrorAction SilentlyContinue
    if ($props.NetCfgInstanceId -eq $adapter.InterfaceGuid) {
        $instanceKey = $candidate.PSPath
        break
    }
}
$pnpExisted = $false
$pnpValue = $null
if ($instanceKey) {
    $pnp = Get-ItemProperty -Path $instanceKey -Name PnPCapabilities -ErrorAction SilentlyContinue
    if ($null -ne $pnp.PnPCapabilities) {
        $pnpExisted = $true
        $pnpValue = [int]$pnp.PnPCapabilities
    }
}

$powerNeedsChange = ($null -eq $acValue) -or ($null -eq $dcValue) -or
                    ($acValue -ne 0) -or ($dcValue -ne 0)
$pnpNeedsChange = ($pnpValue -ne 24)

if ($changes.Count -eq 0 -and -not $powerNeedsChange -and -not $pnpNeedsChange) {
    Write-Host 'Already tuned; nothing to change.'
    return
}

Write-Host ''
Write-Host 'Planned changes:'
foreach ($change in $changes) {
    Write-Host ("  {0,-18} {1}: '{2}' -> '{3}'" -f $change.Kind, $change.DisplayName, $change.From, $change.To)
}
if ($powerNeedsChange) {
    Write-Host ("  {0,-18} Wireless Adapter Settings: {1}/{2} -> 0/0 (Maximum Performance)" -f 'Power plan', $acValue, $dcValue)
}
if ($pnpNeedsChange) {
    Write-Host ("  {0,-18} PnPCapabilities: {1} -> 24" -f 'Device power', $(if ($pnpExisted) { $pnpValue } else { 'not set' }))
}
Write-Host ''
Write-Host 'Wi-Fi 7, MLO, 802.11be, band and channel width are left untouched.'

if ($DryRun) {
    Write-Host 'Dry run: nothing written.'
    return
}

$backupDir = Join-Path $env:ProgramData 'Veyla'
$backupFile = Join-Path $backupDir 'wifi-stability-backup.json'
if ((Test-Path $backupFile) -and -not $Force) {
    throw "Backup already exists: $backupFile. Run wifi-stability-off.ps1 first (or pass -Force)."
}
New-Item -ItemType Directory -Path $backupDir -Force | Out-Null

$backup = [ordered]@{
    Created       = (Get-Date).ToString('o')
    Adapter       = $adapter.Name
    InterfaceGuid = $adapter.InterfaceGuid
    Advanced      = @($changes | ForEach-Object {
        [ordered]@{ DisplayName = $_.DisplayName; DisplayValue = $_.From }
    })
    PnP           = [ordered]@{
        Key     = $instanceKey
        Existed = $pnpExisted
        Value   = $pnpValue
    }
    Power         = [ordered]@{
        Scheme    = $scheme
        AC        = $acValue
        DC        = $dcValue
        ACExisted = ($null -ne $acValue)
        DCExisted = ($null -ne $dcValue)
    }
}
$backup | ConvertTo-Json -Depth 6 | Set-Content -Path $backupFile -Encoding UTF8
Write-Host "Backup: $backupFile"

foreach ($change in $changes) {
    Set-NetAdapterAdvancedProperty -Name $adapter.Name `
        -DisplayName $change.DisplayName -DisplayValue $change.To -NoRestart
}

if ($instanceKey) {
    New-ItemProperty -Path $instanceKey -Name PnPCapabilities -Value 24 `
        -PropertyType DWord -Force | Out-Null
}

if ($scheme) {
    powercfg /setacvalueindex $scheme $wirelessSub $wirelessSet 0 | Out-Null
    powercfg /setdcvalueindex $scheme $wirelessSub $wirelessSet 0 | Out-Null
    powercfg /setactive $scheme | Out-Null
}

if (-not $NoRestart) {
    Write-Host 'Restarting the adapter...'
    Restart-NetAdapter -Name $adapter.Name -Confirm:$false
    Start-Sleep -Seconds 4
}

Write-Host 'Done. Run wifi-stability-off.ps1 to restore the original values.'
