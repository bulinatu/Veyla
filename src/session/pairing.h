#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

#include "session/profile.h"

#include <chiaki/regist.h>

namespace brp::session {

enum class PairState {
    Idle,
    Running,
    Success,
    Failed,
};

// Thin wrapper around chiaki-ng's local (PIN) registration. It performs the
// real handshake against the console and returns the rp_key / rp_regist_key,
// which are then written to the session profile.
class Pairing {
public:
    Pairing() = default;
    ~Pairing();

    Pairing(const Pairing&) = delete;
    Pairing& operator=(const Pairing&) = delete;

    bool Start(const Profile& profile, uint32_t pin);

    // Joins a finished worker and updates state; call once per frame.
    void Poll();

    PairState state() const { return state_.load(); }
    std::string message() const;

    // Moves the credentials out on success. Returns false if not successful.
    bool TakeCredentials(Credentials& out);

    void Cancel();

private:
    static void OnEvent(ChiakiRegistEvent* event, void* user);

    std::atomic<PairState> state_{PairState::Idle};
    std::atomic<bool> credentials_ready_{false};
    mutable std::mutex mutex_;
    std::string message_;
    Credentials credentials_;

    // opaque chiaki state, allocated in the .cpp
    struct Impl;
    Impl* impl_ = nullptr;
};

}  // namespace brp::session
