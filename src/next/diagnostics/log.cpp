#include "diagnostics/log.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <utility>

namespace brp::next {
namespace {

std::string Timestamp() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const std::time_t seconds = system_clock::to_time_t(now);
    const auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &seconds);
#else
    localtime_r(&seconds, &local);
#endif
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d.%03d", local.tm_hour,
                  local.tm_min, local.tm_sec, static_cast<int>(ms.count()));
    return buffer;
}

}  // namespace

Logger::Logger(std::filesystem::path path, LogLevel level, std::size_t max_bytes)
    : path_(std::move(path)), level_(level), max_bytes_(max_bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    OpenLocked();
}

void Logger::OpenLocked() {
    std::error_code ec;
    if (path_.has_parent_path()) {
        std::filesystem::create_directories(path_.parent_path(), ec);
    }
    file_.open(path_, std::ios::binary | std::ios::app);
}

void Logger::RotateLocked() {
    if (!file_.is_open()) {
        return;
    }
    std::error_code ec;
    const auto size = std::filesystem::file_size(path_, ec);
    if (ec || size < max_bytes_) {
        return;
    }
    file_.close();
    const std::filesystem::path old = path_.string() + ".1";
    std::filesystem::remove(old, ec);
    std::filesystem::rename(path_, old, ec);
    OpenLocked();
}

void Logger::Log(LogLevel level, const std::string& message) {
    if (static_cast<int>(level) < static_cast<int>(level_)) {
        return;
    }
    const std::string line =
        Timestamp() + " [" + std::string(ToString(level)) + "] " + message;
    Sink sink;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        RotateLocked();
        if (file_.is_open()) {
            file_ << line << '\n';
        }
        sink = sink_;
    }
    if (sink) {
        sink(level, message);
    }
}

void Logger::Flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_.is_open()) {
        file_.flush();
    }
}

void Logger::SetSink(Sink sink) {
    std::lock_guard<std::mutex> lock(mutex_);
    sink_ = std::move(sink);
}

}  // namespace brp::next
