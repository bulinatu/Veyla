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
                    std::size_t max_bytes = 2u * 1024u * 1024u);

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
    std::size_t max_bytes_ = 2u * 1024u * 1024u;
    Sink sink_;
};

}  // namespace brp::next
