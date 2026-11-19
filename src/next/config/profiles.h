#pragma once

#include "config/flat_json.h"
#include "core/interfaces.h"
#include "core/models.h"

#include <filesystem>
#include <string>
#include <vector>

namespace brp::next {

FlatMap ProfileToFlat(const Profile& profile);
bool ProfileFromFlat(const FlatMap& flat, Profile& out, std::string& error);

class ProfileStore final : public IProfileStore {
public:
    explicit ProfileStore(std::filesystem::path directory);

    Status List(std::vector<Profile>& out) override;
    Status Save(const Profile& profile) override;
    Status Delete(const std::string& id) override;
    Status SetDefault(const std::string& id) override;
    Status Import(const std::string& path, Profile& out) override;
    Status Export(const std::string& id, const std::string& path) override;

    Status Create(const std::string& name, const AppSettings& settings, Profile& out);
    Status Duplicate(const std::string& id, const std::string& name, Profile& out);
    Status Get(const std::string& id, Profile& out);

    std::string Directory() const { return directory_.string(); }

private:
    std::filesystem::path FileFor(const std::string& id) const;
    Status ReadFile(const std::filesystem::path& path, Profile& out);
    Status WriteFile(const std::filesystem::path& path, const Profile& profile);

    std::filesystem::path directory_;
};

std::string MakeProfileId(const std::string& name);

}  // namespace brp::next
