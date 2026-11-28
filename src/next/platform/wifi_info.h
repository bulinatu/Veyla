#pragma once

#include <string>

namespace brp::next {

// Live Wi-Fi telemetry for the diagnostic log. The WLAN API reports the
// association the machine is actually using, not the nominal adapter speed.
struct WifiInfo {
    bool connected = false;
    std::string adapter;
    std::string ssid;
    std::string bssid;
    std::string phy;   // 802.11n/ac/ax/be
    std::string band;  // 2.4GHz / 5GHz / 6GHz
    int signal_percent = 0;
    int rssi_dbm = 0;
    int rx_rate_kbps = 0;
    int tx_rate_kbps = 0;
    int channel = 0;
};

WifiInfo ProbeWifi();

// One-line form used by the log:
//   wifi: ssid="X" bssid=.. rssi=-52dBm signal=86% rx=1201Mbps tx=1201Mbps ch=36 band=5GHz phy=802.11ax
std::string FormatWifi(const WifiInfo& info);

}  // namespace brp::next
