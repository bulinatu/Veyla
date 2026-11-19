#pragma once

#include "app/navigation.h"
#include "app/service_registry.h"
#include "config/profiles.h"
#include "config/settings.h"
#include "diagnostics/log.h"
#include "diagnostics/metrics.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace brp::next {

struct AppPaths {
    std::filesystem::path settings_file;
    std::filesystem::path profiles_dir;
    std::filesystem::path log_file;
    std::filesystem::path legacy_settings;

    static AppPaths Default();
};

struct StartupReport {
    bool settings_created = false;
    bool legacy_imported = false;
    std::vector<std::string> warnings;
};

// Owns the foundation services and their lifetime. The UI receives interfaces
// from here; it never constructs stores or reads files directly.
class AppHost {
public:
    explicit AppHost(AppPaths paths);
    ~AppHost();

    AppHost(const AppHost&) = delete;
    AppHost& operator=(const AppHost&) = delete;

    Status Initialize(StartupReport& report);
    void Shutdown();

    const AppSettings& Settings() const { return settings_; }
    Status ApplySettings(const AppSettings& settings);
    Status ResetSettings();

    ILogger* Log() const { return logger_.get(); }
    IConfigStore* Config() const { return settings_store_.get(); }
    IProfileStore* Profiles() const { return profiles_.get(); }
    IDiagnosticsService* Diagnostics() const { return diagnostics_.get(); }
    NavigationModel& Navigation() { return navigation_; }
    const NavigationModel& Navigation() const { return navigation_; }
    ServiceRegistry& Services() { return services_; }

private:
    AppPaths paths_;
    std::shared_ptr<Logger> logger_;
    std::shared_ptr<SettingsStore> settings_store_;
    std::shared_ptr<ProfileStore> profiles_;
    std::shared_ptr<DiagnosticsCollector> diagnostics_;
    NavigationModel navigation_;
    ServiceRegistry services_;
    AppSettings settings_;
    bool initialized_ = false;
};

}  // namespace brp::next
