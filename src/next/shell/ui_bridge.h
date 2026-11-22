#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace brp::next {

// Line-oriented bridge between the C++ stream engine and the Rust/GPUI shell.
//
// The shell owns the window and the settings UI; the engine owns streaming,
// decoding, audio, input, DLSS and the in-stream overlay. They talk over a
// loopback TCP socket on a port the shell picks and passes on the command line,
// so no firewall prompt is involved and a crash in either side cannot take the
// other down.
//
// Protocol: UTF-8 lines. Shell -> engine: one command per line. Engine ->
// shell: `S <json>` snapshots, at most four per second.
class UiBridge {
public:
    UiBridge() = default;
    ~UiBridge();

    UiBridge(const UiBridge&) = delete;
    UiBridge& operator=(const UiBridge&) = delete;

    void Start(unsigned short port);
    void Stop();

    // Non-blocking: pops one command from the queue.
    bool TakeCommand(std::string& out);

    // Replaces the pending status snapshot. Cheap to call every frame.
    void Publish(std::string status);

    bool connected() const { return link_.load(); }

private:
    void ServerMain(unsigned short port);
    void ReaderMain();
    void WriterMain();
    void CloseClient();

    std::thread server_;
    std::thread reader_;
    std::thread writer_;

    SOCKET listen_ = INVALID_SOCKET;
    SOCKET client_ = INVALID_SOCKET;
    std::mutex socket_mutex_;

    std::atomic<bool> running_{false};
    std::atomic<bool> link_{false};

    std::mutex queue_mutex_;
    std::vector<std::string> commands_;

    std::mutex out_mutex_;
    std::condition_variable out_cv_;
    std::deque<std::string> outbox_;
    bool out_pending_ = false;
};

}  // namespace brp::next
