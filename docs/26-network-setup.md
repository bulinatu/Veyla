# 26 - Network setup for low-latency Remote Play

Everything below was measured on this machine on 18 Nov 2026 between 10:20 and
10:40 EET. Values that could not be verified are marked **unverified** and
were not applied. The PC-side changes are in `tools/network_tweaks.ps1` (needs
elevation, logs every old -> new value to `build/network-tweaks.log`).

## 1. Topology

| Item | Value |
| --- | --- |
| Router / gateway | 192.168.1.1 (model/firmware: **fill in**) |
| PC | `Cosmin-PC`, 192.168.1.10, Realtek PCIe 2.5GbE, link **2.5 Gbps full duplex** |
| PS5 | 192.168.1.21, wired |
| Path | PC -> router/switch -> PS5, wired on both ends, no powerline, no repeater |

Remote Play traffic on this machine: control 9295 (probe/wake), video and audio
UDP streams. Local sessions do not need PSN, so ISP latency only matters for
sign-in and updates.

## 2. Measurements

### Baseline (before any change)

```
ping -n 5 192.168.1.21      -> 5/5, min 1, max 3, avg 1.4 ms
Get-NetAdapterStatistics    -> ReceivedDiscardedPackets 0, ReceivedPacketErrors 0
                               OutboundDiscardedPackets 0, OutboundPacketErrors 0
link                        -> 2.5 Gbps, full duplex, connected
```

### After the PC changes (same session, stream running at 100 Mbps)

```
ping -n 100 192.168.1.21    -> 100/100, 0% loss, min 0, max 1, avg 0 ms
[app]  fps 60.0, 15.1 Mbps ... lost 0, drops 0, decode 4.4/5.3 ms
[video] src 60.0 fps, select 4v 100%, arrival 16.7 ms, outliers 2
[fg]   displayed 120.0 FPS (2x)
```

The bitrate in the app was 100 Mbps; the console sent ~15 Mbps for this scene
(HEVC variable bitrate), which is why `[app]` shows 15 Mbps rather than 100.

## 3. Windows: changes applied

Adapter: **Ethernet** (Realtek PCIe 2.5GbE Family Controller, driver
1125.31.50.603, 2026-06-03). Applied at 10:22 and verified.

| Property | Before | After | Why |
| --- | --- | --- | --- |
| Green Ethernet | Enabled | **Disabled** | link power saving can add latency on idle |
| Gigabit Lite | Enabled | **Disabled** | reduced-signalling power mode |
| Power Saving Mode | Enabled | **Disabled** | stops the NIC from idling down |
| Large Send Offload v2 (IPv4) | Enabled | **Disabled** | consistent packet timing |
| Large Send Offload v2 (IPv6) | Enabled | **Disabled** | same |
| Recv Segment Coalescing (IPv4) | Enabled | **Disabled** | no merging delay on small UDP packets |
| Recv Segment Coalescing (IPv6) | Enabled | **Disabled** | same |

Already correct before the change, left alone:

| Property | Value |
| --- | --- |
| Energy-Efficient Ethernet | Disabled |
| Interrupt Moderation | Disabled |
| Flow Control | Disabled |
| Jumbo Frame | Disabled |
| Speed & Duplex | 2.5 Gbps Full Duplex |
| Priority & VLAN | Enabled (VLAN ID 0) |
| Receive / Transmit Buffers | 1024 / 512 |

Power plan: `GhostX (Ghost Optimizer)`, not the stock High performance plan.
Checked values:

| Setting | AC value |
| --- | --- |
| PCI Express ASPM | 0 (off) |
| USB selective suspend | 0 (off) |
| Hard disk idle | 0 (never) |

### Revert list (PC)

```powershell
# run elevated, one line per property that was changed
$n='Ethernet'
Set-NetAdapterAdvancedProperty -Name $n -DisplayName 'Green Ethernet' -DisplayValue 'Enabled'
Set-NetAdapterAdvancedProperty -Name $n -DisplayName 'Gigabit Lite' -DisplayValue 'Enabled'
Set-NetAdapterAdvancedProperty -Name $n -DisplayName 'Power Saving Mode' -DisplayValue 'Enabled'
Set-NetAdapterAdvancedProperty -Name $n -DisplayName 'Large Send Offload v2 (IPv4)' -DisplayValue 'Enabled'
Set-NetAdapterAdvancedProperty -Name $n -DisplayName 'Large Send Offload v2 (IPv6)' -DisplayValue 'Enabled'
Set-NetAdapterAdvancedProperty -Name $n -DisplayName 'Recv Segment Coalescing (IPv4)' -DisplayValue 'Enabled'
Set-NetAdapterAdvancedProperty -Name $n -DisplayName 'Recv Segment Coalescing (IPv6)' -DisplayValue 'Enabled'
Restart-NetAdapter -Name $n
```

## 4. Not applied (and why)

- **MMCSS registry** (`NetworkThrottlingIndex=0xffffffff`,
  `SystemResponsiveness=0`): needs a reboot to take effect and there was no
  measured need at 15-100 Mbps. Documented in `docs/25`; defaults are
  `NetworkThrottlingIndex=10`, `SystemResponsiveness=20`.
- **Windows Update pause / Delivery Optimization**: not touched (DoSvc is
  stopped, wuauserv is running but idle). The launch-night runbook still says
  "pause updates before playing".
- **DNS changes**: local streaming does not use DNS; left as is.
- **Router and PS5 settings**: no admin session was used here. Checklist below;
  fill the value column when done.

## 5. Router checklist (fill in)

| Setting | Value found | Decision |
| --- | --- | --- |
| Model / firmware | | |
| PC port link | | |
| PS5 port link | | |
| DHCP reservation PC | | |
| DHCP reservation PS5 | | |
| QoS / traffic shaping | | |
| DSCP priority for UDP 9295-9297 | | |
| IGMP snooping | | |
| EEE / Green Ethernet on ports | | |
| USB media server / SMB on router | | |
| Guest network isolation | | |
| Wi-Fi band used by other devices | | |

Guidance: both devices wired to the same switch; reservations so the stored
address 192.168.1.21 stays valid; disable traffic shaping that buffers; if the
router has DSCP QoS, mark the Remote Play UDP ports EF for both devices; keep
IGMP snooping on (LAN discovery uses SSDP multicast); no jumbo frames.

## 6. PS5 checklist (fill in)

| Setting | Path | Value |
| --- | --- | --- |
| Wired connection | Settings > Network > Set Up Internet Connection | LAN cable |
| Connection test | same | NAT Type 2 |
| Remote Play | Settings > System > Remote Play | On |
| Stay connected in rest mode | Settings > System > Power Saving | On |
| Turn on from network | same | On |
| 120 Hz output | Settings > Screen and Video > Video Output | Automatic |
| HDR | same | per launch decision (SDR by default) |
| Downloads paused | Downloads/Uploads | yes during play |

## 7. Bitrate matrix (fill in, 10 min each)

| Bitrate | lost | drops | decode avg/max | arrival outliers | worker fps | verdict |
| --- | --- | --- | --- | --- | --- | --- |
| 50 Mbps | | | | | | |
| 75 Mbps | | | | | | |
| 100 Mbps | 0 | 0 | 4.4/5.3 | 2 | 120.0 | tested, clean |

Note: the console sends what the scene needs; the bitrate setting is a cap.

## 8. Recovery test

Cable pull 5 s -> session drops to Home -> Connect. Recovery target <= 20 s.
Result: **fill in**.
