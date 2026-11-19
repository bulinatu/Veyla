#pragma once

#include "config/flat_json.h"
#include "core/interfaces.h"
#include "core/models.h"

#include <filesystem>
#include <string>
#include <vector>

namespace brp::next {

// Typed configuration. This is the only place that knows the on-disk key
// names, which keeps migration and validation in one module.

FlatMap SettingsToFlat(const AppSettings& settings);
void SettingsFromFlat(const FlatMap& flat, AppSettings& settings);

// Imports the legacy %APPDATA%\BetterRemotePlay\session.json. Unknown keys are
// ignored, missing keys keep their defaults, and every mapping that cannot be
// represented is reported as a warning. The legacy file is never modified.
Status ImportLegacySettings(const std::filesystem::path& path, AppSettings& out,
                            std::vector<std::string>& warnings);

class SettingsStore final : public IConfigStore {
public:
    explicit SettingsStore(std::filesystem::path file);

    Status Load(AppSettings& out, std::vector<std::string>& warnings) override;
    Status Save(const AppSettings& settings) override;
    Status Reset(AppSettings& out) override;
    std::string Location() const override;

private:
    std::filesystem::path file_;
};

std::string DefaultSettingsPath();
std::string DefaultProfilesDirectory();

}  // namespace brp::next
