#include "platform/wifi_info.h"

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>
#include <wlanapi.h>

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <sstream>
#include <vector>

#pragma comment(lib, "wlanapi.lib")
#pragma comment(lib, "iphlpapi.lib")

namespace brp::next {
namespace {

std::string NarrowUtf8(const wchar_t* text) {
    if (text == nullptr || *text == L'\0') {
        return {};
    }
    const int size =
        ::WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) {
        return {};
    }
    std::string out(static_cast<std::size_t>(size - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), size, nullptr, nullptr);
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

// When the WLAN API cannot report an association, at least identify the adapter
// that carries the default route and its nominal link rate. This distinguishes
// "the probe failed" from "there is no wireless link".
std::string TrimText(std::string value) {
    std::size_t begin = 0;
    while (begin < value.size() &&
           std::isspace(static_cast<unsigned char>(value[begin]))) {
        ++begin;
    }
    std::size_t end = value.size();
    while (end > begin &&
           std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }
    return value.substr(begin, end - begin);
}

std::string RunHiddenCommand(const wchar_t* command) {
    SECURITY_ATTRIBUTES attrs{};
    attrs.nLength = sizeof(attrs);
    attrs.bInheritHandle = TRUE;
    HANDLE read_pipe = nullptr;
    HANDLE write_pipe = nullptr;
    if (!::CreatePipe(&read_pipe, &write_pipe, &attrs, 0)) {
        return {};
    }
    ::SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    startup.hStdOutput = write_pipe;
    startup.hStdError = write_pipe;
    PROCESS_INFORMATION process{};
    std::wstring line = command;
    const BOOL started =
        ::CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE,
                         CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    ::CloseHandle(write_pipe);
    std::string out;
    if (started) {
        char buffer[512];
        DWORD read = 0;
        while (::ReadFile(read_pipe, buffer, sizeof(buffer), &read, nullptr) &&
               read > 0) {
            out.append(buffer, read);
        }
        ::WaitForSingleObject(process.hProcess, 3000);
        ::CloseHandle(process.hProcess);
        ::CloseHandle(process.hThread);
    }
    ::CloseHandle(read_pipe);
    return out;
}

// netsh is localized, so parse by shape (SSID/BSSID keys, a % value, Mbps
// values) instead of English labels.
void ParseNetsh(const std::string& text, WifiInfo& info) {
    std::istringstream stream(text);
    std::string line;
    int rates = 0;
    while (std::getline(stream, line)) {
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        std::string key = TrimText(line.substr(0, colon));
        std::string value = TrimText(line.substr(colon + 1));
        for (char& ch : key) {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        if (value.empty()) {
            continue;
        }
        if (key.find("bssid") != std::string::npos) {
            info.bssid = value;
        } else if (key.find("ssid") != std::string::npos) {
            info.ssid = value;
        }
        if (value.find('%') != std::string::npos) {
            const int percent = std::atoi(value.c_str());
            if (percent > 0) {
                info.signal_percent = percent;
            }
        }
        if (key.find("mbps") != std::string::npos) {
            const int mbps = std::atoi(value.c_str());
            if (mbps > 0) {
                if (rates == 0) {
                    info.rx_rate_kbps = mbps * 1000;
                } else if (rates == 1) {
                    info.tx_rate_kbps = mbps * 1000;
                }
                ++rates;
            }
        }
        if (key.find("channel") != std::string::npos ||
            key.find("canal") != std::string::npos) {
            info.channel = std::atoi(value.c_str());
        }
        if (value.find("connected") != std::string::npos ||
            value.find("conectat") != std::string::npos) {
            info.connected = true;
        }
    }
    if (!info.ssid.empty()) {
        info.connected = true;
    }
    if (info.connected) {
        info.band = BandName(info.channel);
        info.source = "netsh";
    }
}

void FillAdapterFallback(WifiInfo& info) {
    ULONG size = 16u * 1024u;
    std::vector<unsigned char> buffer(size);
    auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                        GAA_FLAG_SKIP_DNS_SERVER;
    ULONG result = ::GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, adapters, &size);
    if (result == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(size);
        adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
        result = ::GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, adapters, &size);
    }
    if (result != ERROR_SUCCESS) {
        return;
    }
    for (auto* adapter = adapters; adapter != nullptr; adapter = adapter->Next) {
        if (adapter->IfType != IF_TYPE_IEEE80211 ||
            adapter->OperStatus != IfOperStatusUp) {
            continue;
        }
        if (info.adapter.empty()) {
            info.adapter = NarrowUtf8(adapter->Description);
        }
        if (info.rx_rate_kbps == 0 && adapter->TransmitLinkSpeed > 0) {
            info.rx_rate_kbps = static_cast<int>(adapter->TransmitLinkSpeed / 1000);
        }
        if (info.tx_rate_kbps == 0 && adapter->TransmitLinkSpeed > 0) {
            info.tx_rate_kbps = static_cast<int>(adapter->TransmitLinkSpeed / 1000);
        }
        if (info.source.empty()) {
            info.source = "ifentry";
        }
        break;
    }
}

}  // namespace

WifiInfo ProbeWifi() {
    WifiInfo info;
    DWORD negotiated = 0;
    HANDLE client = nullptr;
    const DWORD opened = WlanOpenHandle(2, nullptr, &negotiated, &client);
    if (opened != ERROR_SUCCESS) {
        info.error = "WlanOpenHandle=" + std::to_string(opened);
        FillAdapterFallback(info);
        return info;
    }

    PWLAN_INTERFACE_INFO_LIST interfaces = nullptr;
    const DWORD enumerated = WlanEnumInterfaces(client, nullptr, &interfaces);
    if (enumerated != ERROR_SUCCESS || interfaces == nullptr) {
        info.error = "WlanEnumInterfaces=" + std::to_string(enumerated);
        WlanCloseHandle(client, nullptr);
        FillAdapterFallback(info);
        return info;
    }

    bool saw_connected = false;
    for (DWORD i = 0; i < interfaces->dwNumberOfItems; ++i) {
        const WLAN_INTERFACE_INFO& iface = interfaces->InterfaceInfo[i];
        if (iface.isState != wlan_interface_state_connected) {
            continue;
        }
        saw_connected = true;
        DWORD size = 0;
        void* data = nullptr;
        const DWORD queried = WlanQueryInterface(
            client, &iface.InterfaceGuid, wlan_intf_opcode_current_connection, nullptr,
            &size, &data, nullptr);
        if (queried != ERROR_SUCCESS || data == nullptr) {
            info.error = "WlanQueryInterface=" + std::to_string(queried);
            continue;
        }
        const auto* connection = static_cast<const WLAN_CONNECTION_ATTRIBUTES*>(data);
        const auto& assoc = connection->wlanAssociationAttributes;
        if (connection->isState == wlan_interface_state_connected) {
            info.connected = true;
            info.adapter = NarrowUtf8(iface.strInterfaceDescription);
            info.ssid.assign(reinterpret_cast<const char*>(assoc.dot11Ssid.ucSSID),
                             assoc.dot11Ssid.uSSIDLength);
            info.bssid = BssidText(assoc.dot11Bssid);
            info.phy = PhyName(assoc.dot11PhyType);
            info.signal_percent = static_cast<int>(assoc.wlanSignalQuality);
            info.rx_rate_kbps = static_cast<int>(assoc.ulRxRate);
            info.tx_rate_kbps = static_cast<int>(assoc.ulTxRate);
        } else {
            info.error = "interface state not connected";
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
    WlanCloseHandle(client, nullptr);

    if (info.connected) {
        info.source = "wlanapi";
    } else {
        if (info.error.empty()) {
            info.error = saw_connected ? "association query failed"
                                       : "no connected WLAN interface";
        }
        FillAdapterFallback(info);
        // Some Wi-Fi 7 drivers answer the association query badly. netsh uses a
        // different path and still reports SSID/signal/rate; probe it at most
        // every 30 s so a failed API does not spawn a process every 5 s.
        static std::mutex netsh_mutex;
        static std::chrono::steady_clock::time_point last_netsh{};
        const auto now = std::chrono::steady_clock::now();
        bool run_netsh = false;
        {
            std::lock_guard<std::mutex> lock(netsh_mutex);
            if (last_netsh.time_since_epoch().count() == 0 ||
                now - last_netsh > std::chrono::seconds(30)) {
                last_netsh = now;
                run_netsh = true;
            }
        }
        if (run_netsh) {
            ParseNetsh(RunHiddenCommand(L"cmd.exe /c netsh wlan show interfaces"),
                       info);
        }
    }
    return info;
}

std::string FormatWifi(const WifiInfo& info) {
    char text[640] = {};
    if (!info.connected) {
        std::snprintf(text, sizeof(text),
                      "wifi: not connected (%s) adapter=\"%s\" link=%d/%dMbps "
                      "source=%s",
                      info.error.empty() ? "unknown" : info.error.c_str(),
                      info.adapter.c_str(), info.rx_rate_kbps / 1000,
                      info.tx_rate_kbps / 1000,
                      info.source.empty() ? "?" : info.source.c_str());
        return text;
    }
    std::snprintf(
        text, sizeof(text),
        "wifi: ssid=\"%s\" bssid=%s rssi=%ddBm signal=%d%% rx=%dMbps tx=%dMbps "
        "ch=%d band=%s phy=%s adapter=\"%s\" source=%s",
        info.ssid.c_str(), info.bssid.c_str(), info.rssi_dbm, info.signal_percent,
        info.rx_rate_kbps / 1000, info.tx_rate_kbps / 1000, info.channel,
        info.band.empty() ? "?" : info.band.c_str(), info.phy.c_str(),
        info.adapter.c_str(), info.source.empty() ? "?" : info.source.c_str());
    return text;
}

}  // namespace brp::next
