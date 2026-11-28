#pragma once

#include "core/interfaces.h"

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>

namespace brp::next {

class Logger final : public ILogger {
public:
    using Sink = std::function<void(LogLevel, const std::string&)>;

    explicit Logger(std::filesystem::path path, LogLevel level = LogLevel::Info,
                    std::size_t max_bytes = 8u * 1024u * 1024u);
    ~Logger() override;

    void Log(LogLevel level, const std::string& message) override;
    void Flush() override;

    void SetLevel(LogLevel level) { level_ = level; }
    LogLevel Level() const { return level_; }
    void SetSink(Sink sink);
    const std::filesystem::path& Path() const { return path_; }

private:
    void OpenLocked();
    void RotateLocked();

    std::filesystem::path path_;
    std::ofstream file_;
    std::mutex mutex_;
    LogLevel level_ = LogLevel::Info;
    std::size_t max_bytes_ = 8u * 1024u * 1024u;
    Sink sink_;
};

// The engine has one logger. Streaming, decoder and frame-generation code are
// not wired to it directly; these helpers send their diagnostics to the same
// next.log instead of a stderr that a GUI build cannot show.
void LogMessage(LogLevel level, const std::string& message);
// printf-shaped variant for the existing diagnostic call sites.
void LogPrintf(LogLevel level, const char* format, ...);
inline void LogDebug(const std::string& message) {
    LogMessage(LogLevel::Debug, message);
}
inline void LogInfo(const std::string& message) {
    LogMessage(LogLevel::Info, message);
}
inline void LogWarning(const std::string& message) {
    LogMessage(LogLevel::Warning, message);
}
inline void LogError(const std::string& message) {
    LogMessage(LogLevel::Error, message);
}

}  // namespace brp::next
