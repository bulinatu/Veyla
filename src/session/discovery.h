#pragma once

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

}  // namespace brp::session
