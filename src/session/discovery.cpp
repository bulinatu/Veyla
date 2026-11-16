#include "session/discovery.h"

#include <chiaki/discovery.h>
#include <chiaki/log.h>

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

namespace brp::session {
namespace {

std::map<std::string, std::string> ParseHeaders(const char* buffer, int length) {
    std::map<std::string, std::string> headers;
    std::string text(buffer, static_cast<size_t>(length));

    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) {
            end = text.size();
        }
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;

        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const size_t colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        std::string key = line.substr(0, colon);
        std::string value = line.substr(colon + 1);
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
            value.erase(value.begin());
        }
        headers[key] = value;
    }
    return headers;
}

void ProbePort(uint16_t port, const char* version, int timeout_ms,
               std::vector<DiscoveredConsole>& out) {
    SOCKET sock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        return;
    }

    BOOL broadcast = TRUE;
    ::setsockopt(sock, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&broadcast),
                 sizeof(broadcast));

    sockaddr_in target{};
    target.sin_family = AF_INET;
    target.sin_port = ::htons(port);
    target.sin_addr.s_addr = ::inet_addr("255.255.255.255");

    char packet[128];
    const int packet_len = std::snprintf(packet, sizeof(packet),
                                         "SRCH * HTTP/1.1\ndevice-discovery-protocol-version:%s\n\n",
                                         version);
    ::sendto(sock, packet, packet_len, 0, reinterpret_cast<sockaddr*>(&target), sizeof(target));

    const DWORD start = ::GetTickCount();
    while (static_cast<int>(::GetTickCount() - start) < timeout_ms) {
        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(sock, &read_set);

        timeval wait{};
        wait.tv_sec = 0;
        wait.tv_usec = 100000;

        const int ready = ::select(0, &read_set, nullptr, nullptr, &wait);
        if (ready <= 0) {
            continue;
        }

        char buffer[2048];
        sockaddr_in from{};
        int from_len = sizeof(from);
        const int received =
            ::recvfrom(sock, buffer, sizeof(buffer), 0, reinterpret_cast<sockaddr*>(&from),
                       &from_len);
        if (received <= 0) {
            continue;
        }

        const auto headers = ParseHeaders(buffer, received);

        char address[INET_ADDRSTRLEN] = {};
        ::inet_ntop(AF_INET, &from.sin_addr, address, sizeof(address));

        DiscoveredConsole console;
        console.address = address;
        if (headers.count("host-id")) {
            console.target_id = headers.at("host-id");
        }
        if (headers.count("host-name")) {
            console.name = headers.at("host-name");
        }
        if (headers.count("host-type")) {
            console.device_type = headers.at("host-type");
        }
        if (headers.count("host-status")) {
            console.status = headers.at("host-status");
        }

        bool duplicate = false;
        for (const auto& existing : out) {
            if (existing.target_id == console.target_id && existing.address == console.address) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            out.push_back(console);
        }
    }

    ::closesocket(sock);
}

}  // namespace

std::vector<DiscoveredConsole> DiscoverLocal(int timeout_ms) {
    std::vector<DiscoveredConsole> results;

    WSADATA wsa{};
    if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        return results;
    }

    // PS5 first (longer window), then PS4 on the remaining time.
    ProbePort(9302, "00030010", timeout_ms, results);
    if (results.empty()) {
        ProbePort(987, "00020020", timeout_ms, results);
    }

    ::WSACleanup();
    return results;
}

bool WakeConsole(const std::string& host, const uint8_t* regist_key, size_t regist_key_size,
                 bool ps5) {
    if (host.empty() || !regist_key || regist_key_size == 0) {
        return false;
    }

    // The wake credential is the registration key's ASCII hex text interpreted
    // as a number ("1c6b04b5" -> 0x1c6b04b5), not its binary bytes.
    char text[17] = {};
    const size_t count = std::min<size_t>(regist_key_size, 16);
    std::memcpy(text, regist_key, count);
    text[count] = '\0';
    const size_t text_len = std::strlen(text);
    if (text_len == 0 || text_len > 8) {
        return false;
    }
    for (size_t i = 0; i < text_len; ++i) {
        if (!std::isxdigit(static_cast<unsigned char>(text[i]))) {
            return false;
        }
    }
    const uint64_t credential = std::strtoull(text, nullptr, 16);

    WSADATA wsa{};
    if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        return false;
    }

    ChiakiLog log{};
    chiaki_log_init(&log, 0, nullptr, nullptr);
    const ChiakiErrorCode error =
        chiaki_discovery_wakeup(&log, nullptr, host.c_str(), credential, ps5);

    ::WSACleanup();
    if (error != CHIAKI_ERR_SUCCESS) {
        std::fprintf(stderr, "[wake] failed for %s: %s\n", host.c_str(),
                     chiaki_error_string(error));
        return false;
    }
    std::fprintf(stderr, "[wake] sent to %s (credential %08llx, %s)\n", host.c_str(),
                 static_cast<unsigned long long>(credential), ps5 ? "PS5" : "PS4");
    return true;
}

}  // namespace brp::session
