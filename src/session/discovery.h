#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace brp::session {

struct DiscoveredConsole {
    std::string address;
    std::string name;
    std::string target_id;
    std::string device_type;
    std::string status;
};

// PS5/PS4 LAN discovery: UDP broadcast "SRCH" and parse the console's reply.
// PS5 listens on 9302, PS4 on 987; both are probed.
std::vector<DiscoveredConsole> DiscoverLocal(int timeout_ms = 1500);

// Sends the Remote Play wake-up packet. `regist_key` is the raw 16-byte
// registration key from pairing; its first bytes are an ASCII hex credential.
bool WakeConsole(const std::string& host, const uint8_t* regist_key, size_t regist_key_size,
                 bool ps5);

}  // namespace brp::session
