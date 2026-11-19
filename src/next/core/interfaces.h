#pragma once

#include "core/models.h"
#include "core/result.h"

#include <string>
#include <vector>

namespace brp::next {

// Service boundaries. Implementations live in the higher layers; this header
// is the only thing the UI and tests are allowed to depend on.

class ILogger {
public:
    virtual ~ILogger() = default;
    virtual void Log(LogLevel level, const std::string& message) = 0;
    virtual void Flush() = 0;
};

class IConfigStore {
public:
    virtual ~IConfigStore() = default;
    virtual Status Load(AppSettings& out, std::vector<std::string>& warnings) = 0;
    virtual Status Save(const AppSettings& settings) = 0;
    virtual Status Reset(AppSettings& out) = 0;
    virtual std::string Location() const = 0;
};

class IProfileStore {
public:
    virtual ~IProfileStore() = default;
    virtual Status List(std::vector<Profile>& out) = 0;
    virtual Status Save(const Profile& profile) = 0;
    virtual Status Create(const std::string& name, const AppSettings& settings,
                          Profile& out) = 0;
    virtual Status Delete(const std::string& id) = 0;
    virtual Status SetDefault(const std::string& id) = 0;
    virtual Status Import(const std::string& path, Profile& out) = 0;
    virtual Status Export(const std::string& id, const std::string& path) = 0;
};

class IStreamingService {
public:
    virtual ~IStreamingService() = default;
    virtual Status Connect(const ConsoleSettings& console) = 0;
    virtual void Disconnect() = 0;
    virtual StreamSnapshot Snapshot() const = 0;
};

class IAiEngine {
public:
    virtual ~IAiEngine() = default;
    virtual std::vector<AiCapabilities> Capabilities() const = 0;
    virtual Status Configure(const AiSettings& settings) = 0;
    virtual Status Start() = 0;
    virtual void Stop() = 0;
    virtual AiSnapshot Snapshot() const = 0;
};

class IOverlayDataProvider {
public:
    virtual ~IOverlayDataProvider() = default;
    virtual DiagnosticsSnapshot Diagnostics() const = 0;
    virtual StreamSnapshot Stream() const = 0;
    virtual GpuSnapshot Gpu() const = 0;
    virtual AiSnapshot Ai() const = 0;
};

class IDiagnosticsService {
public:
    virtual ~IDiagnosticsService() = default;
    virtual void Tick(double seconds) = 0;
    virtual DiagnosticsSnapshot Snapshot() const = 0;
    virtual Status WriteReport(const std::string& path) const = 0;
};

class IClock {
public:
    virtual ~IClock() = default;
    virtual double Seconds() const = 0;
};

}  // namespace brp::next
