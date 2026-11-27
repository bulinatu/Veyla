#pragma once

#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

namespace brp::next {

enum class UpdatePhase {
    Idle,
    Checking,
    Available,
    Downloading,
    Ready,
    UpToDate,
    Error,
};

struct UpdateStatus {
    UpdatePhase phase = UpdatePhase::Idle;
    std::string version;      // release tag without the leading v
    std::string notes;        // release body, truncated for the panel
    std::string page_url;     // html_url of the release
    std::string asset_path;   // downloaded update.exe, when Ready
    std::string error;
    double progress = 0.0;    // 0..1 while downloading
};

// GitHub Releases updater. The release is expected to carry a single-file
// update executable built by tools/make_update.ps1 (for example
// Veyla-2.6.0-update.exe); the shell starts it after quitting.
class UpdateService {
public:
    UpdateService();
    ~UpdateService();

    UpdateService(const UpdateService&) = delete;
    UpdateService& operator=(const UpdateService&) = delete;

    // Starts a background check. repo is "owner/name"; an empty repo reports a
    // configuration error instead of touching the network.
    void Check(const std::string& repo, bool include_prerelease,
               const std::string& current_version);

    // Downloads the asset found by Check(). Only valid in Available phase.
    void Download();

    // Spawns the updater for app_dir and returns true when it started. The
    // caller exits the shell right after this.
    bool Install(const std::filesystem::path& app_dir, unsigned long pid,
                 std::string& error);

    UpdateStatus Snapshot() const;
    bool Busy() const;

private:
    void JoinWorker();
    void RunCheck(std::string repo, bool include_prerelease, std::string current);
    void RunDownload(const std::string& url, const std::string& version,
                     long long expected);

    mutable std::mutex mutex_;
    UpdateStatus status_;
    std::string asset_url_;
    long long asset_size_ = 0;
    std::thread worker_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> busy_{false};
};

std::string AppVersion();
std::string UpdateStateName(UpdatePhase phase);

}  // namespace brp::next
