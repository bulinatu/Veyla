# Network tweaks for low-latency LAN streaming (requires elevation).
# Logs every change (old -> new) so docs/26-network-setup.md can quote facts.
$log = Join-Path (Split-Path -Parent $PSScriptRoot) 'build\network-tweaks.log'
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $log) | Out-Null
Start-Transcript -Path $log -Force | Out-Null

$n = (Get-NetAdapter -Physical | Where-Object Status -eq 'Up' | Select-Object -First 1).Name
Write-Output "adapter=$n"
Write-Output ("time=" + (Get-Date -Format o))

function Set-Adv([string]$name, [string]$value) {
    $p = Get-NetAdapterAdvancedProperty -Name $n -DisplayName $name -ErrorAction SilentlyContinue
    if (-not $p) { Write-Output "MISSING: $name"; return }
    $old = $p.DisplayValue
    if ($old -eq $value) { Write-Output "ALREADY: $name = $value"; return }
    try {
        Set-NetAdapterAdvancedProperty -Name $n -DisplayName $name -DisplayValue $value -NoRestart -ErrorAction Stop
        Write-Output "SET: $name : '$old' -> '$value'"
    } catch {
        Write-Output "FAIL: $name : $($_.Exception.Message)"
    }
}

Set-Adv 'Green Ethernet' 'Disabled'
Set-Adv 'Gigabit Lite' 'Disabled'
Set-Adv 'Power Saving Mode' 'Disabled'
Set-Adv 'Large Send Offload v2 (IPv4)' 'Disabled'
Set-Adv 'Large Send Offload v2 (IPv6)' 'Disabled'
Set-Adv 'Recv Segment Coalescing (IPv4)' 'Disabled'
Set-Adv 'Recv Segment Coalescing (IPv6)' 'Disabled'

Write-Output "restarting adapter"
Restart-NetAdapter -Name $n
Start-Sleep -Seconds 8

Get-NetAdapter -Name $n | Format-List Name, Status, LinkSpeed, FullDuplex
Get-NetAdapterAdvancedProperty -Name $n |
    Where-Object { $_.DisplayName -match 'Green|Gigabit Lite|Power Saving|Large Send|Recv Segment|Interrupt|Energy|Flow Control|Jumbo' } |
    Select-Object DisplayName, DisplayValue | Format-Table -AutoSize
Write-Output "ping:"
Test-Connection 192.168.1.21 -Count 10 | Select-Object Address, ResponseTime | Format-Table -AutoSize

Stop-Transcript | Out-Null
