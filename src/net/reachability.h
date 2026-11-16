#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

namespace brp::net {

enum class ProbeState {
    Idle,
    Checking,
    Reachable,
    Unreachable,
    NoAddress,
};

// TCP reachability probe for a console's Remote Play control port.
// This is deliberately not a session: it only answers "is something listening
// at host:port", which is enough to make the Connect button honest before the
// protocol layer exists.
class Reachability {
public:
    Reachability() = default;
    ~Reachability();

    Reachability(const Reachability&) = delete;
    Reachability& operator=(const Reachability&) = delete;

    void Start(const std::string& host, int port = 9295);
    void Poll();

    ProbeState state() const { return state_.load(); }
    std::string detail() const;

private:
    void Run(std::string host, int port);

    std::atomic<ProbeState> state_{ProbeState::Idle};
    std::thread thread_;
    mutable std::mutex mutex_;
    std::string detail_;
};

}  // namespace brp::net
