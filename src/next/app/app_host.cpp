#include "app/app_host.h"

#include <cstdlib>

namespace brp::next {
namespace {

std::string AppDataRoot() {
    const char* appdata = std::getenv("APPDATA");
    return appdata ? appdata : ".";
}

}  // namespace

AppPaths AppPaths::Default() {
    const std::string root = AppDataRoot();
    AppPaths paths;
    paths.settings_file = std::filesystem::path(root) / "BetterRemotePlayNext" /
                          "next-settings.json";
    paths.profiles_dir =
        std::filesystem::path(root) / "BetterRemotePlayNext" / "profiles";
    paths.log_file =
        std::filesystem::path(root) / "BetterRemotePlayNext" / "next.log";
    paths.legacy_settings =
        std::filesystem::path(root) / "BetterRemotePlay" / "session.json";
    paths.session_file =
        std::filesystem::path(root) / "BetterRemotePlayNext" / "session.json";
    return paths;
}

AppHost::AppHost(AppPaths paths) : paths_(std::move(paths)) {}

AppHost::~AppHost() {
    Shutdown();
}

Status AppHost::Initialize(StartupReport& report) {
    report = StartupReport{};
    if (initialized_) {
        return Status::Ok();
    }
    logger_ = std::make_shared<Logger>(paths_.log_file, LogLevel::Info);
    settings_store_ = std::make_shared<SettingsStore>(paths_.settings_file);
    profiles_ = std::make_shared<ProfileStore>(paths_.profiles_dir);
    diagnostics_ = std::make_shared<DiagnosticsCollector>();

    std::error_code ec;
    const bool settings_exist = std::filesystem::exists(paths_.settings_file, ec);
    std::vector<std::string> warnings;
    Status status = settings_store_->Load(settings_, warnings);
    report.warnings = warnings;
    if (!settings_exist) {
        report.settings_created = true;
        if (std::filesystem::exists(paths_.legacy_settings, ec)) {
            AppSettings imported;
            std::vector<std::string> import_warnings;
            if (ImportLegacySettings(paths_.legacy_settings, imported, import_warnings)
                    .ok()) {
                settings_ = imported;
                report.legacy_imported = true;
                report.warnings.insert(report.warnings.end(), import_warnings.begin(),
                                       import_warnings.end());
                status = settings_store_->Save(settings_);
            }
        } else {
            status = settings_store_->Save(settings_);
        }
    }
    if (!status.ok()) {
        logger_->Log(LogLevel::Error, "settings: " + status.message());
        return status;
    }

    std::vector<std::string> errors;
    settings_.Validate(report.warnings, errors);
    if (!errors.empty()) {
        std::string joined;
        for (const auto& message : errors) {
            if (!joined.empty()) joined += "; ";
            joined += message;
        }
        return Status::Error("settings invalid: " + joined);
    }

    std::filesystem::create_directories(paths_.profiles_dir, ec);

    services_.Set<ILogger>(std::static_pointer_cast<ILogger>(logger_));
    services_.Set<IConfigStore>(std::static_pointer_cast<IConfigStore>(settings_store_));
    services_.Set<IProfileStore>(std::static_pointer_cast<IProfileStore>(profiles_));
    services_.Set<IDiagnosticsService>(
        std::static_pointer_cast<IDiagnosticsService>(diagnostics_));

    initialized_ = true;
    logger_->Log(LogLevel::Info,
                 "host initialized; settings=" + paths_.settings_file.string());
    if (report.legacy_imported) {
        logger_->Log(LogLevel::Info, "legacy settings imported");
    }
    for (const auto& warning : report.warnings) {
        logger_->Log(LogLevel::Warning, warning);
    }
    return Status::Ok();
}

void AppHost::Shutdown() {
    if (!initialized_) {
        return;
    }
    logger_->Log(LogLevel::Info, "host shutdown");
    logger_->Flush();
    services_.Clear();
    initialized_ = false;
}

Status AppHost::ApplySettings(const AppSettings& settings) {
    if (!initialized_) {
        return Status::Error("host is not initialized");
    }
    AppSettings candidate = settings;
    std::vector<std::string> warnings;
    std::vector<std::string> errors;
    candidate.Validate(warnings, errors);
    if (!errors.empty()) {
        std::string joined;
        for (const auto& message : errors) {
            if (!joined.empty()) joined += "; ";
            joined += message;
        }
        return Status::Error("settings invalid: " + joined);
    }
    Status status = settings_store_->Save(candidate);
    if (!status.ok()) {
        logger_->Log(LogLevel::Error, "settings save: " + status.message());
        return status;
    }
    settings_ = std::move(candidate);
    for (const auto& warning : warnings) {
        logger_->Log(LogLevel::Warning, warning);
    }
    logger_->Log(LogLevel::Info, "settings applied");
    return Status::Ok();
}

Status AppHost::ResetSettings() {
    if (!initialized_) {
        return Status::Error("host is not initialized");
    }
    AppSettings defaults;
    Status status = settings_store_->Save(defaults);
    if (!status.ok()) {
        return status;
    }
    settings_ = std::move(defaults);
    logger_->Log(LogLevel::Info, "settings reset to defaults");
    return Status::Ok();
}

}  // namespace brp::next
