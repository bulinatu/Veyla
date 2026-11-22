#include "shell/ui_bridge.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace brp::next {
namespace {

void BridgeLog(const char* text) {
    if (std::getenv("BRP_BRIDGE_LOG") == nullptr) {
        return;
    }
    wchar_t appdata[MAX_PATH]{};
    const DWORD length = ::GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return;
    }
    std::wstring path(appdata);
    path += L"\\BetterRemotePlayNext\\bridge.log";
    FILE* file = nullptr;
    if (::_wfopen_s(&file, path.c_str(), L"a") == 0 && file != nullptr) {
        std::fprintf(file, "%s\n", text);
        std::fclose(file);
    }
}

}  // namespace

UiBridge::~UiBridge() {
    Stop();
}

void UiBridge::Start(unsigned short port) {
    if (running_.exchange(true)) {
        return;
    }
    BridgeLog("bridge start");
    server_ = std::thread([this, port]() { ServerMain(port); });
}

void UiBridge::Stop() {
    if (!running_.exchange(false)) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(socket_mutex_);
        if (client_ != INVALID_SOCKET) {
            ::shutdown(client_, SD_BOTH);
            ::closesocket(client_);
            client_ = INVALID_SOCKET;
        }
        if (listen_ != INVALID_SOCKET) {
            ::closesocket(listen_);
            listen_ = INVALID_SOCKET;
        }
    }
    out_cv_.notify_all();
    if (server_.joinable()) {
        server_.join();
    }
    ::WSACleanup();
    BridgeLog("bridge stopped");
}

bool UiBridge::TakeCommand(std::string& out) {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    if (commands_.empty()) {
        return false;
    }
    out = std::move(commands_.front());
    commands_.erase(commands_.begin());
    return true;
}

void UiBridge::Publish(std::string status) {
    {
        std::lock_guard<std::mutex> lock(out_mutex_);
        outbox_.push_back(std::move(status));
        // Status snapshots are replacable; events are not. Bound the queue so a
        // stalled shell cannot grow it without limit, and keep the newest.
        while (outbox_.size() > 16) {
            outbox_.pop_front();
        }
        out_pending_ = true;
    }
    out_cv_.notify_one();
}

void UiBridge::ServerMain(unsigned short port) {
    WSADATA wsa{};
    if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        return;
    }

    const SOCKET listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) {
        return;
    }
    BOOL exclusive = FALSE;
    ::setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                 reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = ::htons(port);
    address.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    if (::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
        ::listen(listener, 1) != 0) {
        ::closesocket(listener);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(socket_mutex_);
        listen_ = listener;
    }
    BridgeLog(("bridge listening on " + std::to_string(port)).c_str());

    while (running_.load()) {
        const SOCKET client = ::accept(listener, nullptr, nullptr);
        if (client == INVALID_SOCKET) {
            break;
        }
        BOOL nodelay = TRUE;
        ::setsockopt(client, IPPROTO_TCP, TCP_NODELAY,
                     reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));
        {
            std::lock_guard<std::mutex> lock(socket_mutex_);
            client_ = client;
        }
        BridgeLog("client connected");
        link_.store(true);
        reader_ = std::thread([this]() { ReaderMain(); });
        writer_ = std::thread([this]() { WriterMain(); });

        if (reader_.joinable()) {
            reader_.join();
        }
        link_.store(false);
        out_cv_.notify_all();
        if (writer_.joinable()) {
            writer_.join();
        }
        CloseClient();
    }

    {
        std::lock_guard<std::mutex> lock(socket_mutex_);
        if (listen_ == listener) {
            listen_ = INVALID_SOCKET;
        }
    }
    ::closesocket(listener);
}

void UiBridge::CloseClient() {
    std::lock_guard<std::mutex> lock(socket_mutex_);
    if (client_ != INVALID_SOCKET) {
        ::shutdown(client_, SD_BOTH);
        ::closesocket(client_);
        client_ = INVALID_SOCKET;
    }
}

void UiBridge::ReaderMain() {
    SOCKET socket = INVALID_SOCKET;
    {
        std::lock_guard<std::mutex> lock(socket_mutex_);
        socket = client_;
    }
    if (socket == INVALID_SOCKET) {
        return;
    }

    BridgeLog("reader start");
    std::string accumulator;
    char buffer[4096];
    while (running_.load() && link_.load()) {
        const int read = ::recv(socket, buffer, sizeof(buffer), 0);
        if (read <= 0) {
            BridgeLog("reader end");
            break;
        }
        accumulator.append(buffer, static_cast<std::size_t>(read));
        for (;;) {
            const std::size_t newline = accumulator.find('\n');
            if (newline == std::string::npos) {
                break;
            }
            std::string line = accumulator.substr(0, newline);
            accumulator.erase(0, newline + 1);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (line.empty()) {
                continue;
            }
            if (line.size() > 160) {
                line.resize(160);
            }
            BridgeLog(("cmd: " + line).c_str());
            {
                std::lock_guard<std::mutex> lock(queue_mutex_);
                commands_.push_back(std::move(line));
            }
        }
    }
}

void UiBridge::WriterMain() {
    SOCKET socket = INVALID_SOCKET;
    {
        std::lock_guard<std::mutex> lock(socket_mutex_);
        socket = client_;
    }
    if (socket == INVALID_SOCKET) {
        return;
    }

    BridgeLog("writer start");
    while (running_.load() && link_.load()) {
        std::string line;
        {
            std::unique_lock<std::mutex> lock(out_mutex_);
            out_cv_.wait_for(lock, std::chrono::milliseconds(250), [this]() {
                return !running_.load() || !link_.load() || out_pending_;
            });
            if (!running_.load() || !link_.load()) {
                break;
            }
            if (outbox_.empty()) {
                out_pending_ = false;
                continue;
            }
            line = std::move(outbox_.front());
            outbox_.pop_front();
            out_pending_ = !outbox_.empty();
        }
        line.push_back('\n');
        std::size_t offset = 0;
        bool failed = false;
        while (offset < line.size()) {
            const int sent = ::send(socket, line.data() + offset,
                                    static_cast<int>(line.size() - offset), 0);
            if (sent <= 0) {
                failed = true;
                break;
            }
            offset += static_cast<std::size_t>(sent);
        }
        if (failed) {
            BridgeLog("writer failed");
            break;
        }
    }
    BridgeLog("writer end");
}

}  // namespace brp::next
