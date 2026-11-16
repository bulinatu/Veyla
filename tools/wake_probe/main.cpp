// Sends the Remote Play wake-up packet for the configured console.
// Usage: wake_probe [host]

#include "session/discovery.h"
#include "session/profile.h"

#include <cstdio>
#include <string>

int main(int argc, char** argv) {
    brp::session::Profile profile;
    std::string detail;
    if (!brp::session::Load(brp::session::DefaultProfilePath(), profile, detail)) {
        std::fprintf(stderr, "cannot load session: %s\n", detail.c_str());
        return 1;
    }

    const std::string host = argc > 1 ? argv[1] : profile.host;
    if (host.empty()) {
        std::fprintf(stderr, "no host: pass one or configure session.json\n");
        return 1;
    }

    const bool ps5 = profile.device_type != "PS4";
    const bool ok = brp::session::WakeConsole(host, profile.credentials.rp_regist_key.data(),
                                              profile.credentials.rp_regist_key.size(), ps5);
    std::printf("wake %s -> %s\n", host.c_str(), ok ? "sent" : "failed");
    return ok ? 0 : 1;
}
