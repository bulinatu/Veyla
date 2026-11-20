#include "config/profiles.h"

#include "config/settings.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <sstream>

namespace brp::next {
namespace {

std::uint64_t NowSeconds() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

bool SafeId(const std::string& id) {
    if (id.empty() || id.size() > 96) {
        return false;
    }
    for (const char c : id) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (!std::isalnum(u) && c != '-' && c != '_') {
            return false;
        }
    }
    return true;
}

}  // namespace

std::string MakeProfileId(const std::string& name) {
    std::string id;
    id.reserve(name.size());
    bool last_dash = false;
    for (const char c : name) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (std::isalnum(u)) {
            id.push_back(static_cast<char>(std::tolower(u)));
            last_dash = false;
        } else if (!last_dash && !id.empty()) {
            id.push_back('-');
            last_dash = true;
        }
    }
    while (!id.empty() && id.back() == '-') {
        id.pop_back();
    }
    if (id.empty()) {
        id = "profile";
    }
    return id;
}

FlatMap ProfileToFlat(const Profile& profile) {
    FlatMap flat;
    FlatJson::PutString(flat, "id", profile.id);
    FlatJson::PutString(flat, "name", profile.name);
    FlatJson::PutBool(flat, "default", profile.is_default);
    FlatJson::PutInt(flat, "created_at", static_cast<long long>(profile.created_at));
    FlatJson::PutInt(flat, "updated_at", static_cast<long long>(profile.updated_at));
    for (const auto& [key, value] : SettingsToFlat(profile.settings)) {
        flat["settings." + key] = value;
    }
    return flat;
}

bool ProfileFromFlat(const FlatMap& flat, Profile& out, std::string& error) {
    out = Profile{};
    out.id = FlatJson::GetString(flat, "id", "");
    out.name = FlatJson::GetString(flat, "name", "");
    if (out.name.empty()) {
        error = "profile name is empty";
        return false;
    }
    if (!SafeId(out.id)) {
        out.id = MakeProfileId(out.name);
    }
    out.is_default = FlatJson::GetBool(flat, "default", false);
    out.created_at =
        static_cast<std::uint64_t>(FlatJson::GetInt(flat, "created_at", 0));
    out.updated_at =
        static_cast<std::uint64_t>(FlatJson::GetInt(flat, "updated_at", 0));

    FlatMap settings_flat;
    const std::string prefix = "settings.";
    for (const auto& [key, value] : flat) {
        if (key.rfind(prefix, 0) == 0) {
            settings_flat[key.substr(prefix.size())] = value;
        }
    }
    SettingsFromFlat(settings_flat, out.settings);

    std::vector<std::string> warnings;
    std::vector<std::string> errors;
    out.settings.Validate(warnings, errors);
    if (!errors.empty()) {
        std::string joined;
        for (const auto& message : errors) {
            if (!joined.empty()) joined += "; ";
            joined += message;
        }
        error = "profile settings invalid: " + joined;
        return false;
    }
    return true;
}

ProfileStore::ProfileStore(std::filesystem::path directory)
    : directory_(std::move(directory)) {}

std::filesystem::path ProfileStore::FileFor(const std::string& id) const {
    return directory_ / (id + ".json");
}

Status ProfileStore::ReadFile(const std::filesystem::path& path, Profile& out) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return Status::Error("cannot open " + path.string());
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    FlatMap flat;
    std::string error;
    if (!FlatJson::Parse(buffer.str(), flat, error)) {
        return Status::Error("profile parse failed: " + error);
    }
    if (!ProfileFromFlat(flat, out, error)) {
        return Status::Error(error);
    }
    return Status::Ok();
}

Status ProfileStore::WriteFile(const std::filesystem::path& path, const Profile& profile) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    const std::filesystem::path temp = path.string() + ".tmp";
    {
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        if (!file) {
            return Status::Error("cannot write " + temp.string());
        }
        file << FlatJson::Serialize(ProfileToFlat(profile));
        if (!file) {
            return Status::Error("profile write failed: " + temp.string());
        }
    }
    const bool had_old = std::filesystem::exists(path, ec);
    if (had_old) {
        std::filesystem::copy_file(path, path.string() + ".bak",
                                   std::filesystem::copy_options::overwrite_existing, ec);
        std::filesystem::remove(path, ec);
    }
    std::filesystem::rename(temp, path, ec);
    if (ec) {
        if (had_old) {
            std::error_code restore_ec;
            std::filesystem::copy_file(path.string() + ".bak", path,
                                       std::filesystem::copy_options::overwrite_existing,
                                       restore_ec);
        }
        return Status::Error("cannot replace profile: " + ec.message());
    }
    return Status::Ok();
}

Status ProfileStore::List(std::vector<Profile>& out) {
    out.clear();
    std::error_code ec;
    if (!std::filesystem::exists(directory_, ec)) {
        return Status::Ok();
    }
    for (const auto& entry : std::filesystem::directory_iterator(directory_, ec)) {
        if (ec) {
            break;
        }
        if (!entry.is_regular_file() || entry.path().extension() != ".json") {
            continue;
        }
        Profile profile;
        if (ReadFile(entry.path(), profile).ok()) {
            out.push_back(std::move(profile));
        }
    }
    std::sort(out.begin(), out.end(), [](const Profile& a, const Profile& b) {
        return a.name < b.name;
    });
    return Status::Ok();
}

Status ProfileStore::Get(const std::string& id, Profile& out) {
    if (!SafeId(id)) {
        return Status::Error("invalid profile id");
    }
    return ReadFile(FileFor(id), out);
}

Status ProfileStore::Save(const Profile& profile) {
    if (!SafeId(profile.id)) {
        return Status::Error("invalid profile id");
    }
    if (profile.name.empty()) {
        return Status::Error("profile name is empty");
    }
    return WriteFile(FileFor(profile.id), profile);
}

Status ProfileStore::Delete(const std::string& id) {
    if (!SafeId(id)) {
        return Status::Error("invalid profile id");
    }
    std::error_code ec;
    const auto path = FileFor(id);
    if (!std::filesystem::exists(path, ec)) {
        return Status::Error("profile not found");
    }
    Profile deleted;
    const bool was_default = ReadFile(path, deleted).ok() && deleted.is_default;
    std::filesystem::remove(path, ec);
    if (ec) {
        return Status::Error("cannot delete profile: " + ec.message());
    }
    if (was_default) {
        std::vector<Profile> remaining;
        List(remaining);
        if (!remaining.empty()) {
            remaining.front().is_default = true;
            Save(remaining.front());
        }
    }
    return Status::Ok();
}

Status ProfileStore::SetDefault(const std::string& id) {
    std::vector<Profile> profiles;
    Status status = List(profiles);
    if (!status) {
        return status;
    }
    bool found = false;
    for (auto& profile : profiles) {
        const bool match = profile.id == id;
        found = found || match;
        if (profile.is_default != match) {
            profile.is_default = match;
            profile.updated_at = NowSeconds();
            status = Save(profile);
            if (!status) {
                return status;
            }
        }
    }
    return found ? Status::Ok() : Status::Error("profile not found");
}

Status ProfileStore::Import(const std::string& path, Profile& out) {
    Status status = ReadFile(path, out);
    if (!status) {
        return status;
    }
    std::error_code ec;
    if (std::filesystem::exists(FileFor(out.id), ec)) {
        out.id = out.id + "-" + std::to_string(NowSeconds());
    }
    out.is_default = false;
    out.updated_at = NowSeconds();
    if (out.created_at == 0) {
        out.created_at = out.updated_at;
    }
    return Status::Ok();
}

Status ProfileStore::Export(const std::string& id, const std::string& path) {
    Profile profile;
    Status status = Get(id, profile);
    if (!status) {
        return status;
    }
    return WriteFile(path, profile);
}

Status ProfileStore::Create(const std::string& name, const AppSettings& settings,
                            Profile& out) {
    if (name.empty()) {
        return Status::Error("profile name is empty");
    }
    out = Profile{};
    out.name = name;
    out.id = MakeProfileId(name);
    std::error_code ec;
    int suffix = 1;
    while (std::filesystem::exists(FileFor(out.id), ec)) {
        out.id = MakeProfileId(name) + "-" + std::to_string(suffix++);
    }
    out.settings = settings;
    out.created_at = NowSeconds();
    out.updated_at = out.created_at;
    return Save(out);
}

Status ProfileStore::Duplicate(const std::string& id, const std::string& name,
                               Profile& out) {
    Profile source;
    Status status = Get(id, source);
    if (!status) {
        return status;
    }
    return Create(name.empty() ? source.name + " copy" : name, source.settings, out);
}

}  // namespace brp::next
