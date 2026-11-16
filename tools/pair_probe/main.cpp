// Exercises the real registration stack without the UI. Useful for checking
// the session layer on a machine where the console is reachable.
//
//   pair_probe <pin> [host]

#include "session/pairing.h"
#include "session/profile.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    brp::session::Profile profile;
    std::string detail;
    if (!brp::session::Load(brp::session::DefaultProfilePath(), profile, detail)) {
        std::printf("no session file: %s\n", detail.c_str());
        return 1;
    }

    if (argc < 2) {
        std::printf("usage: pair_probe <pin> [host]\n");
        return 2;
    }

    const uint32_t pin = static_cast<uint32_t>(std::strtoul(argv[1], nullptr, 10));
    if (argc >= 3) {
        profile.host = argv[2];
    }

    std::printf("console   %s\ntarget    %s\nhost      %s\naccount   %s\n",
                profile.display_name().c_str(), profile.target_id.c_str(), profile.host.c_str(),
                profile.psn_account_id.c_str());
    brp::session::Pairing pairing;
    if (!pairing.Start(profile, pin)) {
        std::printf("start failed: %s\n", pairing.message().c_str());
        return 3;
    }

    for (int i = 0; i < 150; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        pairing.Poll();
        const auto state = pairing.state();
        if (state == brp::session::PairState::Success ||
            state == brp::session::PairState::Failed) {
            std::printf("result    %s (%s)\n",
                        state == brp::session::PairState::Success ? "success" : "failed",
                        pairing.message().c_str());
            if (state == brp::session::PairState::Success) {
                brp::session::Credentials credentials;
                if (pairing.TakeCredentials(credentials)) {
                    profile.credentials = credentials;
                    std::string save_detail;
                    if (brp::session::Save(profile, brp::session::DefaultProfilePath(), save_detail)) {
                        std::printf("saved     %s\n", save_detail.c_str());
                    }
                    std::printf("rp_key    %02x%02x... (%zu bytes)\n", credentials.rp_key[0],
                                credentials.rp_key[1], credentials.rp_key.size());
                }
            }
            return state == brp::session::PairState::Success ? 0 : 4;
        }
    }

    std::printf("timeout\n");
    return 5;
}
