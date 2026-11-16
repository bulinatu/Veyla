#include "net/reachability.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <chrono>
#include <cstdio>
#include <cstring>

namespace brp::net {

Reachability::~Reachability() {
    if (thread_.joinable()) {
        thread_.join();
    }
}

void Reachability::Start(const std::string& host, int port) {
    Poll();

    if (state_.load() == ProbeState::Checking) {
        return;
    }
    if (thread_.joinable()) {
        thread_.join();
    }

    if (host.empty()) {
        state_.store(ProbeState::NoAddress);
        std::lock_guard<std::mutex> lock(mutex_);
        detail_ = "No address configured";
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        detail_ = "Connecting to " + host + ":" + std::to_string(port) + "...";
    }
    state_.store(ProbeState::Checking);
    thread_ = std::thread(&Reachability::Run, this, host, port);
}

void Reachability::Poll() {
    if (thread_.joinable() && state_.load() != ProbeState::Checking) {
        thread_.join();
    }
}

void Reachability::Run(std::string host, int port) {
    WSADATA wsa{};
    if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        state_.store(ProbeState::Unreachable);
        std::lock_guard<std::mutex> lock(mutex_);
        detail_ = "Winsock init failed";
        return;
    }

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    const std::string port_text = std::to_string(port);
    addrinfo* result = nullptr;
    const int gai = ::getaddrinfo(host.c_str(), port_text.c_str(), &hints, &result);
    if (gai != 0 || !result) {
        state_.store(ProbeState::Unreachable);
        std::lock_guard<std::mutex> lock(mutex_);
        detail_ = "Cannot resolve host";
        ::WSACleanup();
        return;
    }

    bool ok = false;

    for (addrinfo* entry = result; entry && !ok; entry = entry->ai_next) {
        SOCKET sock = ::socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
        if (sock == INVALID_SOCKET) {
            continue;
        }

        u_long nonblocking = 1;
        ::ioctlsocket(sock, FIONBIO, &nonblocking);

        const int rc = ::connect(sock, entry->ai_addr, static_cast<int>(entry->ai_addrlen));
        if (rc == 0) {
            ok = true;
        } else if (::WSAGetLastError() == WSAEWOULDBLOCK) {
            fd_set write_set;
            FD_ZERO(&write_set);
            FD_SET(sock, &write_set);
            timeval timeout{};
            timeout.tv_sec = 1;
            timeout.tv_usec = 200000;

            if (::select(0, nullptr, &write_set, nullptr, &timeout) > 0) {
                int error = 0;
                int length = sizeof(error);
                if (::getsockopt(sock, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error),
                                 &length) == 0 &&
                    error == 0) {
                    ok = true;
                }
            }
        }

        ::closesocket(sock);
    }

    ::freeaddrinfo(result);
    ::WSACleanup();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        detail_ = ok ? "Console answered on the control port"
                     : "No answer on the control port";
    }
    state_.store(ok ? ProbeState::Reachable : ProbeState::Unreachable);
}

std::string Reachability::detail() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return detail_;
}

}  // namespace brp::net
