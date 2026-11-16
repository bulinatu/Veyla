// Prints consoles found on the local network.
#include "session/discovery.h"

#include <cstdio>

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const auto consoles = brp::session::DiscoverLocal(2500);
    if (consoles.empty()) {
        std::printf("no consoles found\n");
        return 1;
    }
    for (const auto& console : consoles) {
        std::printf("%-16s %-12s %-10s %s\n", console.address.c_str(), console.target_id.c_str(),
                    console.device_type.c_str(), console.name.c_str());
    }
    return 0;
}
