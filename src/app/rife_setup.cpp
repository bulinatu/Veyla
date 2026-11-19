#include "app/rife_setup.h"

#include "media/rife.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace brp {
namespace {

struct ModelInfo {
    const wchar_t* url;
    const wchar_t* onnx;
    const wchar_t* engine;
    const wchar_t* shapes;
};

// Lite is the default: ~6 ms per frame on a 4090. Heavy 4.26 is about twice
// the compute but visibly cleaner on fast motion; it takes one 6-channel input
// (both frames concatenated) instead of img0/img1.
constexpr ModelInfo kModels[] = {
    {L"https://huggingface.co/t9749/rife-4.25-lite-onnx/resolve/main/rife_v425lite.onnx",
     L"rife425lite.onnx", L"rife425_1080.engine",
     L"img0:1x3x1088x1920,img1:1x3x1088x1920,timestep:1x1x1x1"},
    {L"https://huggingface.co/SOR2171/RIFE-ONNX/resolve/main/rife2_26h.onnx",
     L"rife426h.onnx", L"rife426h_1080.engine",
     L"input:1x6x1088x1920,timestep:1x1x1x1"},
};
constexpr int kModelCount = static_cast<int>(sizeof(kModels) / sizeof(kModels[0]));

const ModelInfo& Model(int model) {
    return kModels[std::clamp(model, 0, kModelCount - 1)];
}

// Official TensorRT 10.16 for Windows, CUDA 13.2. NVIDIA's page requires a
// browser click, but this file is served straight from developer.download.
constexpr wchar_t kTensorRtUrl[] =
    L"https://developer.download.nvidia.com/compute/machine-learning/tensorrt/"
    L"10.16.0/zip/TensorRT-10.16.0.72.Windows.amd64.cuda-13.2.zip";

std::string Utf8(const std::wstring& text) {
    if (text.empty()) {
        return std::string();
    }
    const int size =
        ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(size > 0 ? size - 1 : 0), '\0');
    if (size > 1) {
        ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, out.data(), size, nullptr, nullptr);
    }
    return out;
}

std::wstring Widen(const std::string& text) {
    if (text.empty()) {
        return std::wstring();
    }
    const int size = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    std::wstring out(static_cast<size_t>(size > 0 ? size - 1 : 0), L'\0');
    if (size > 1) {
        ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, out.data(), size);
    }
    return out;
}

std::filesystem::path ExeDir() {
    wchar_t path[MAX_PATH] = {};
    ::GetModuleFileNameW(nullptr, path, MAX_PATH);
    return std::filesystem::path(path).parent_path();
}

bool HasTensorRt(const std::filesystem::path& dir) {
    return std::filesystem::exists(dir / "nvinfer_10.dll");
}

// Look for a TensorRT 10.16 install that this GPU can use. The SDK zip is
// behind NVIDIA's click-through, so this only finds installs that already
// exist on the machine (vs-mlrt bundles one, as do the official SDK zips).
std::filesystem::path FindTensorRt(const std::filesystem::path& exe_dir,
                                   std::wstring& note) {
    std::vector<std::filesystem::path> candidates;
    if (const char* env = std::getenv("BRP_TENSORRT_DIR")) {
        candidates.emplace_back(Widen(env));
    }
    candidates.push_back(exe_dir / "rife" / "runtime");
    candidates.emplace_back(L"C:\\mpv-upscaler-video-player\\vs-plugins\\vsmlrt-cuda");

    const std::filesystem::path program_files = L"C:\\Program Files\\NVIDIA Corporation";
    std::error_code ec;
    if (std::filesystem::exists(program_files, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(program_files, ec)) {
            if (!entry.is_directory()) {
                continue;
            }
            const std::wstring name = entry.path().filename().wstring();
            if (name.rfind(L"TensorRT", 0) == 0) {
                candidates.push_back(entry.path() / "lib");
                candidates.push_back(entry.path() / "bin");
            }
        }
    }
    for (const wchar_t* root : {L"C:\\TensorRT", L"C:\\TensorRT-10.16.0.72"}) {
        if (std::filesystem::exists(root, ec)) {
            candidates.emplace_back(std::filesystem::path(root) / "lib");
            candidates.emplace_back(std::filesystem::path(root) / "bin");
            candidates.emplace_back(root);
        }
    }
    for (const auto& dir : candidates) {
        if (HasTensorRt(dir)) {
            return dir;
        }
    }
    note = L"TensorRT 10.16 was not found. NVIDIA's download needs a browser click: "
           L"developer.nvidia.com/tensorrt, take the Windows zip. Unpack it, then set "
           L"BRP_TENSORRT_DIR to its lib folder and press Start again.";
    return {};
}

bool Extract(const std::filesystem::path& zip, const std::filesystem::path& dest,
             const std::string& filter, std::string& detail) {
    std::error_code ec;
    std::filesystem::create_directories(dest, ec);
    wchar_t system_dir[MAX_PATH] = {};
    ::GetSystemDirectoryW(system_dir, MAX_PATH);
    const std::filesystem::path tar = std::filesystem::path(system_dir) / "tar.exe";
    if (!std::filesystem::exists(tar, ec)) {
        detail = "tar.exe is missing";
        return false;
    }
    std::wstring command = L"\"" + tar.wstring() + L"\" -xf \"" + zip.wstring() + L"\" -C \"" +
                           dest.wstring() + L"\"";
    if (!filter.empty()) {
        command += L" " + Widen(filter);
    }
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!::CreateProcessW(tar.wstring().c_str(), mutable_command.data(), nullptr, nullptr, FALSE,
                          CREATE_NO_WINDOW, nullptr, dest.wstring().c_str(), &startup,
                          &process)) {
        detail = "could not start tar.exe";
        return false;
    }
    const DWORD wait = ::WaitForSingleObject(process.hProcess, 15 * 60 * 1000);
    DWORD exit_code = 1;
    ::GetExitCodeProcess(process.hProcess, &exit_code);
    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);
    if (wait != WAIT_OBJECT_0 || exit_code != 0) {
        detail = "tar failed (exit " + std::to_string(exit_code) + ")";
        return false;
    }
    return true;
}

}  // namespace

RifeSetup::~RifeSetup() { Join(); }

void RifeSetup::Join() {
    if (thread_.joinable()) {
        thread_.join();
    }
}

std::wstring RifeSetup::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

void RifeSetup::SetStatus(const std::wstring& text, float progress) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        status_ = text;
    }
    progress_.store(progress);
}

bool RifeSetup::ready(const std::filesystem::path& exe_dir, int model) const {
    const std::filesystem::path rife = exe_dir / "rife";
    std::error_code ec;
    const auto engine = rife / Model(model).engine;
    if (!std::filesystem::exists(engine, ec) ||
        std::filesystem::file_size(engine, ec) < 1024u * 1024u) {
        return false;
    }
    bool runtime = std::filesystem::exists(rife / "runtime" / "nvinfer_10.dll", ec);
    if (!runtime) {
        // runtime.txt is a path on this PC, so validate it rather than trusting
        // the file: a package copied from another machine carries a stale one.
        std::ifstream marker(rife / "runtime.txt");
        std::string line;
        if (marker && std::getline(marker, line) && !line.empty()) {
            runtime = std::filesystem::exists(std::filesystem::path(line) / "nvinfer_10.dll",
                                              ec);
        }
    }
    if (!runtime && std::getenv("BRP_TENSORRT_DIR") != nullptr) {
        runtime = true;
    }
    return runtime;
}

void RifeSetup::Start(const std::filesystem::path& exe_dir, int model) {
    if (state_.load() == State::Running) {
        return;
    }
    Join();
    state_.store(State::Running);
    progress_.store(-1.0f);
    SetStatus(L"Starting...", -1.0f);
    thread_ = std::thread(&RifeSetup::Run, this, exe_dir, model);
}

void RifeSetup::Run(std::filesystem::path exe_dir, int model) {
    std::filesystem::path rife = exe_dir / "rife";
    std::error_code ec;
    std::filesystem::create_directories(rife, ec);

    SetStatus(L"Looking for TensorRT 10.16 on this PC...", -1.0f);
    std::wstring note;
    std::filesystem::path runtime = FindTensorRt(exe_dir, note);
    const bool force_download = std::getenv("BRP_FORCE_TRT_DOWNLOAD") != nullptr;
    if (force_download) {
        runtime.clear();
    }
    if (runtime.empty()) {
        // Nothing installed: fetch the official Windows zip. NVIDIA's page needs
        // a browser click, but the file itself is served without one.
        const std::filesystem::path downloads = rife / "downloads";
        std::filesystem::create_directories(downloads, ec);
        const std::filesystem::path zip = downloads / "TensorRT-10.16.0.72.zip";
        std::error_code zip_ec;
        const auto zip_size = std::filesystem::exists(zip, zip_ec)
                                  ? std::filesystem::file_size(zip, zip_ec)
                                  : 0ull;
        std::string detail;
        if (zip_size > 1500ull * 1024ull * 1024ull) {
            SetStatus(L"Using the TensorRT archive already downloaded.", 0.55f);
        } else {
            SetStatus(L"Downloading TensorRT 10.16 (1.9 GB, one time)...", 0.0f);
            if (!Download(kTensorRtUrl, zip, detail, 0.0f, 0.55f)) {
                SetStatus(L"TensorRT download failed: " + Widen(detail), -1.0f);
                state_.store(State::Failed);
                return;
            }
        }
        SetStatus(L"Extracting TensorRT...", 0.56f);
        const std::filesystem::path extract = rife / "tensorrt";
        if (!Extract(zip, extract, std::string("TensorRT-10.16.0.72/bin"), detail)) {
            SetStatus(L"TensorRT extraction failed: " + Widen(detail), -1.0f);
            state_.store(State::Failed);
            return;
        }
        runtime = extract / "TensorRT-10.16.0.72" / "bin";
        if (!HasTensorRt(runtime)) {
            SetStatus(L"TensorRT was extracted but nvinfer_10.dll is missing.", -1.0f);
            state_.store(State::Failed);
            return;
        }
        std::filesystem::remove(zip, ec);
        // The runtime needs cudart; the app ships the matching CUDA 13 runtime.
        const std::filesystem::path cudart = rife / "cudart64_13.dll";
        if (std::filesystem::exists(cudart, ec) &&
            !std::filesystem::exists(runtime / "cudart64_13.dll", ec)) {
            std::filesystem::copy_file(cudart, runtime / "cudart64_13.dll",
                                       std::filesystem::copy_options::overwrite_existing, ec);
        }
    }
    std::fprintf(stderr, "[setup] TensorRT: %s\n", Utf8(runtime.wstring()).c_str());

    // Remember the location instead of copying about 2 GB of DLLs. This is
    // what makes the setup fast; the app reads the file on start.
    {
        std::ofstream marker(rife / "runtime.txt", std::ios::binary | std::ios::trunc);
        marker << Utf8(runtime.wstring());
    }

    const ModelInfo& info = Model(model);
    const std::filesystem::path model_path = rife / info.onnx;
    if (!std::filesystem::exists(model_path, ec) ||
        std::filesystem::file_size(model_path, ec) < 10u * 1024u * 1024u) {
        SetStatus(L"Downloading the RIFE model (22 MB)...", 0.62f);
        std::string detail;
        if (!Download(info.url, model_path, detail, 0.62f, 0.10f)) {
            SetStatus(L"Model download failed: " + Widen(detail), -1.0f);
            state_.store(State::Failed);
            return;
        }
    }
    SetStatus(L"RIFE model ready.", 0.72f);
    std::fprintf(stderr, "[setup] model ready\n");

    const std::filesystem::path engine = rife / info.engine;
    bool engine_ok = false;
    if (std::filesystem::exists(engine, ec) &&
        std::filesystem::file_size(engine, ec) >= 1024u * 1024u) {
        // Presence is not enough: an engine built for another GPU architecture
        // (for example the shipped sm89 one on an RTX 5080) must be replaced.
        media::Rife probe;
        std::string probe_detail;
        engine_ok = probe.Load(runtime.string(), engine.string(), 1920, 1080, probe_detail);
        if (!engine_ok) {
            std::fprintf(stderr, "[setup] existing engine rejected: %s\n",
                         probe_detail.c_str());
            std::filesystem::remove(engine, ec);
        }
    }
    if (!engine_ok) {
        std::fprintf(stderr, "[setup] building engine\n");
        SetStatus(L"Building the engine for this GPU (2-4 minutes; the app stays "
                  L"responsive)...",
                  0.4f);
        std::string detail;
        if (!BuildEngine(exe_dir, runtime, model, detail)) {
            SetStatus(Widen(detail), -1.0f);
            state_.store(State::Failed);
            return;
        }
    }

    std::fprintf(stderr, "[setup] done\n");
    SetStatus(std::wstring(L"Ready. ") +
                  (model == 1 ? L"RIFE Heavy 4.26" : L"RIFE Lite 4.25") +
                  L" can be enabled in Settings > Upscaling.",
              1.0f);
    state_.store(State::Done);
}

bool RifeSetup::Download(const std::wstring& url, const std::filesystem::path& path,
                         std::string& detail, float base, float span) {
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = static_cast<DWORD>(-1);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    if (!::WinHttpCrackUrl(url.c_str(), 0, 0, &parts)) {
        detail = "bad URL";
        return false;
    }
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring target(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength > 0) {
        target.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    }

    HINTERNET session = ::WinHttpOpen(L"BetterRemotePlay/0.3",
                                      WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        detail = "WinHttpOpen failed";
        return false;
    }
    ::WinHttpSetTimeouts(session, 15000, 15000, 30000, 120000);
    HINTERNET connect = ::WinHttpConnect(session, host.c_str(), parts.nPort, 0);
    HINTERNET request = connect
                            ? ::WinHttpOpenRequest(connect, L"GET", target.c_str(), nullptr,
                                                   WINHTTP_NO_REFERER,
                                                   WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                   parts.nScheme == INTERNET_SCHEME_HTTPS
                                                       ? WINHTTP_FLAG_SECURE
                                                       : 0)
                            : nullptr;
    if (!request) {
        detail = "WinHttpConnect/OpenRequest failed";
        ::WinHttpCloseHandle(session);
        return false;
    }
    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    ::WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));

    bool ok = ::WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                   WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
              ::WinHttpReceiveResponse(request, nullptr);
    DWORD status = 0;
    DWORD status_size = sizeof(status);
    if (ok) {
        ok = ::WinHttpQueryHeaders(request,
                                   WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                   WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                                   WINHTTP_NO_HEADER_INDEX);
    }
    if (!ok || status != 200) {
        detail = "HTTP " + std::to_string(status);
        ::WinHttpCloseHandle(request);
        ::WinHttpCloseHandle(connect);
        ::WinHttpCloseHandle(session);
        return false;
    }

    DWORD length = 0;
    DWORD length_size = sizeof(length);
    ::WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                          WINHTTP_HEADER_NAME_BY_INDEX, &length, &length_size,
                          WINHTTP_NO_HEADER_INDEX);

    FILE* file = nullptr;
    _wfopen_s(&file, path.wstring().c_str(), L"wb");
    if (!file) {
        detail = "cannot open the model file for writing";
        ::WinHttpCloseHandle(request);
        ::WinHttpCloseHandle(connect);
        ::WinHttpCloseHandle(session);
        return false;
    }

    std::vector<char> buffer(256 * 1024);
    unsigned long long received = 0;
    while (ok) {
        DWORD available = 0;
        ok = ::WinHttpQueryDataAvailable(request, &available) && available > 0;
        if (!ok) {
            ok = true;
            break;
        }
        DWORD read = 0;
        if (!::WinHttpReadData(request, buffer.data(), available, &read)) {
            ok = false;
            break;
        }
        if (read == 0) {
            break;
        }
        std::fwrite(buffer.data(), 1, read, file);
        received += read;
        if (length > 0) {
            SetStatus(L"Downloading the RIFE model... " +
                          std::to_wstring(received * 100 / length) + L"%",
                      length > 0 ? base + span * static_cast<float>(received) /
                                               static_cast<float>(length)
                                 :  base);
        }
    }
    std::fclose(file);
    ::WinHttpCloseHandle(request);
    ::WinHttpCloseHandle(connect);
    ::WinHttpCloseHandle(session);
    if (!ok || received < 10ull * 1024ull * 1024ull) {
        detail = "incomplete download (" + std::to_string(received) + " bytes)";
        return false;
    }
    return true;
}

bool RifeSetup::BuildEngine(const std::filesystem::path& exe_dir,
                            const std::filesystem::path& runtime_dir, int model,
                            std::string& detail) {
    std::error_code ec;
    std::filesystem::path trtexec = runtime_dir / "trtexec.exe";
    if (!std::filesystem::exists(trtexec, ec)) {
        const std::filesystem::path parent = runtime_dir.parent_path();
        if (std::filesystem::exists(parent / "trtexec.exe", ec)) {
            trtexec = parent / "trtexec.exe";
        }
    }
    if (!std::filesystem::exists(trtexec, ec)) {
        detail = "TensorRT is present but trtexec.exe is not; unpack the full SDK zip "
                 "so the engine can be built.";
        return false;
    }

    const std::filesystem::path rife = exe_dir / "rife";
    const ModelInfo& info = Model(model);
    const std::filesystem::path model_path = rife / info.onnx;
    const std::filesystem::path engine = rife / info.engine;

    std::wstring command = L"\"" + trtexec.wstring() + L"\" --onnx=\"" + model_path.wstring() +
                           L"\" --fp16 --shapes=" + info.shapes + L" --saveEngine=\"" +
                           engine.wstring() + L"\"";
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!::CreateProcessW(trtexec.wstring().c_str(), mutable_command.data(), nullptr, nullptr,
                          FALSE, CREATE_NO_WINDOW, nullptr, runtime_dir.wstring().c_str(),
                          &startup, &process)) {
        detail = "could not start trtexec";
        return false;
    }
    const DWORD wait = ::WaitForSingleObject(process.hProcess, 20 * 60 * 1000);
    DWORD exit_code = 1;
    ::GetExitCodeProcess(process.hProcess, &exit_code);
    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);
    if (wait != WAIT_OBJECT_0 || exit_code != 0) {
        detail = "trtexec failed (exit " + std::to_string(exit_code) + ")";
        return false;
    }
    if (!std::filesystem::exists(engine, ec) ||
        std::filesystem::file_size(engine, ec) < 1024u * 1024u) {
        detail = "the engine file was not produced";
        return false;
    }
    return true;
}

}  // namespace brp
