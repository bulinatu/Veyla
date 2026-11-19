#pragma once

#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

namespace brp {

// One-click setup for the RIFE engine.
//
// It automates everything that can be automated on the machine: finding a
// TensorRT 10.16 install, copying the DLLs next to the app, downloading the
// RIFE model, and building the engine for the GPU that is present. TensorRT
// itself cannot be fetched silently: NVIDIA's download page needs a browser
// click-through, so when nothing is found the tool says so and offers Retry
// after the user installs or unpacks it.
class RifeSetup {
public:
    enum class State { Idle, Running, Done, Failed };

    RifeSetup() = default;
    ~RifeSetup();
    RifeSetup(const RifeSetup&) = delete;
    RifeSetup& operator=(const RifeSetup&) = delete;

    void Start(const std::filesystem::path& exe_dir);
    void Join();

    State state() const { return state_.load(); }
    std::wstring status() const;
    float progress() const { return progress_.load(); }
    bool ready(const std::filesystem::path& exe_dir) const;

private:
    void Run(std::filesystem::path exe_dir);
    void SetStatus(const std::wstring& text, float progress);
    bool Download(const std::wstring& url, const std::filesystem::path& path,
                  std::string& detail);
    bool BuildEngine(const std::filesystem::path& exe_dir,
                     const std::filesystem::path& runtime_dir, std::string& detail);

    std::thread thread_;
    std::atomic<State> state_{State::Idle};
    std::atomic<float> progress_{-1.0f};
    mutable std::mutex mutex_;
    std::wstring status_;
};

}  // namespace brp
