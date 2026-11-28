#include "diagnostics/log.h"

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <utility>

namespace brp::next {
namespace {

std::atomic<Logger*> g_logger{nullptr};

// A release log can span days. The bare HH:MM:SS made a multi-day file look
// like the clock was jumping backwards.
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
    char buffer[40];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
                  local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                  local.tm_hour, local.tm_min, local.tm_sec,
                  static_cast<int>(ms.count()));
    return buffer;
}

}  // namespace

Logger::Logger(std::filesystem::path path, LogLevel level, std::size_t max_bytes)
    : path_(std::move(path)), level_(level), max_bytes_(max_bytes) {
    g_logger.store(this);
    std::lock_guard<std::mutex> lock(mutex_);
    OpenLocked();
}

Logger::~Logger() {
    if (g_logger.load() == this) {
        g_logger.store(nullptr);
    }
}

void LogMessage(LogLevel level, const std::string& message) {
    if (Logger* logger = g_logger.load()) {
        logger->Log(level, message);
    }
}

LogLevel CurrentLogLevel() {
    if (Logger* logger = g_logger.load()) {
        return logger->Level();
    }
    return LogLevel::Info;
}

void LogPrintf(LogLevel level, const char* format, ...) {
    if (g_logger.load() == nullptr || format == nullptr) {
        return;
    }
    char buffer[1024] = {};
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    LogMessage(level, buffer);
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
    if (static_cast<int>(level) < static_cast<int>(level_.load())) {
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
            // A crash or a force-kill used to lose the tail of the log because
            // the ofstream stayed buffered. Info lines are rare, so flushing
            // each one costs nothing and keeps the diagnostics after a hang.
            file_.flush();
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
