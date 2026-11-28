#include "platform/wifi_info.h"

#include <windows.h>
#include <wlanapi.h>

#include <cstdio>

#pragma comment(lib, "wlanapi.lib")

namespace brp::next {
namespace {

std::string NarrowAscii(const wchar_t* text) {
    if (text == nullptr) {
        return {};
    }
    std::string out;
    for (const wchar_t* ch = text; *ch != L'\0'; ++ch) {
        out.push_back(*ch < 128 ? static_cast<char>(*ch) : '?');
    }
    return out;
}

std::string PhyName(DOT11_PHY_TYPE type) {
    switch (type) {
        case dot11_phy_type_he: return "802.11ax";
        case dot11_phy_type_eht: return "802.11be";
        case dot11_phy_type_vht: return "802.11ac";
        case dot11_phy_type_ht: return "802.11n";
        case dot11_phy_type_erp: return "802.11g";
        case dot11_phy_type_ofdm: return "802.11a";
        case dot11_phy_type_dsss: return "802.11b";
        default: return "unknown";
    }
}

std::string BandName(int channel) {
    // 6 GHz repeats low channel numbers, but the common 2.4/5 GHz ranges are
    // unambiguous. The channel is logged as well, so a 6 GHz low channel can be
    // identified from the rate and PHY.
    if (channel <= 0) {
        return {};
    }
    if (channel <= 14) {
        return "2.4GHz";
    }
    if (channel <= 196) {
        return "5GHz";
    }
    return "6GHz";
}

std::string BssidText(const DOT11_MAC_ADDRESS& mac) {
    char text[24] = {};
    std::snprintf(text, sizeof(text), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0],
                  mac[1], mac[2], mac[3], mac[4], mac[5]);
    return text;
}

}  // namespace

WifiInfo ProbeWifi() {
    WifiInfo info;
    DWORD negotiated = 0;
    HANDLE client = nullptr;
    if (WlanOpenHandle(2, nullptr, &negotiated, &client) != ERROR_SUCCESS) {
        return info;
    }

    PWLAN_INTERFACE_INFO_LIST interfaces = nullptr;
    if (WlanEnumInterfaces(client, nullptr, &interfaces) == ERROR_SUCCESS &&
        interfaces != nullptr) {
        for (DWORD i = 0; i < interfaces->dwNumberOfItems; ++i) {
            const WLAN_INTERFACE_INFO& iface = interfaces->InterfaceInfo[i];
            if (iface.isState != wlan_interface_state_connected) {
                continue;
            }
            DWORD size = 0;
            void* data = nullptr;
            if (WlanQueryInterface(client, &iface.InterfaceGuid,
                                   wlan_intf_opcode_current_connection, nullptr, &size,
                                   &data, nullptr) != ERROR_SUCCESS ||
                data == nullptr) {
                continue;
            }
            const auto* connection = static_cast<const WLAN_CONNECTION_ATTRIBUTES*>(data);
            const auto& assoc = connection->wlanAssociationAttributes;
            if (connection->isState == wlan_interface_state_connected) {
                info.connected = true;
                info.adapter = NarrowAscii(iface.strInterfaceDescription);
                info.ssid.assign(
                    reinterpret_cast<const char*>(assoc.dot11Ssid.ucSSID),
                    assoc.dot11Ssid.uSSIDLength);
                info.bssid = BssidText(assoc.dot11Bssid);
                info.phy = PhyName(assoc.dot11PhyType);
                info.signal_percent = static_cast<int>(assoc.wlanSignalQuality);
                info.rx_rate_kbps = static_cast<int>(assoc.ulRxRate);
                info.tx_rate_kbps = static_cast<int>(assoc.ulTxRate);
            }
            WlanFreeMemory(data);

            if (info.connected) {
                DWORD rssi_size = 0;
                void* rssi_data = nullptr;
                if (WlanQueryInterface(client, &iface.InterfaceGuid, wlan_intf_opcode_rssi,
                                       nullptr, &rssi_size, &rssi_data,
                                       nullptr) == ERROR_SUCCESS &&
                    rssi_data != nullptr) {
                    info.rssi_dbm = *static_cast<const LONG*>(rssi_data);
                    WlanFreeMemory(rssi_data);
                }
                DWORD channel_size = 0;
                void* channel_data = nullptr;
                if (WlanQueryInterface(client, &iface.InterfaceGuid,
                                       wlan_intf_opcode_channel_number, nullptr,
                                       &channel_size, &channel_data,
                                       nullptr) == ERROR_SUCCESS &&
                    channel_data != nullptr) {
                    info.channel = static_cast<int>(*static_cast<const ULONG*>(channel_data));
                    WlanFreeMemory(channel_data);
                }
                info.band = BandName(info.channel);
            }
            break;
        }
        WlanFreeMemory(interfaces);
    }
    WlanCloseHandle(client, nullptr);
    return info;
}

std::string FormatWifi(const WifiInfo& info) {
    if (!info.connected) {
        return "wifi: no wireless connection on this machine";
    }
    char text[512] = {};
    std::snprintf(
        text, sizeof(text),
        "wifi: ssid=\"%s\" bssid=%s rssi=%ddBm signal=%d%% rx=%dMbps tx=%dMbps "
        "ch=%d band=%s phy=%s adapter=\"%s\"",
        info.ssid.c_str(), info.bssid.c_str(), info.rssi_dbm, info.signal_percent,
        info.rx_rate_kbps / 1000, info.tx_rate_kbps / 1000, info.channel,
        info.band.empty() ? "?" : info.band.c_str(), info.phy.c_str(),
        info.adapter.c_str());
    return text;
}

}  // namespace brp::next
