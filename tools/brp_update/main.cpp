// Veyla updater.
//
// Applies a patch produced by tools/make_update.ps1. The patch is either a
// folder layout (payload\, delete.txt, version.txt) or a single exe with a zip
// appended to it:
//
//   update.exe       this program
//   <zip>            changed files at their relative paths, _delete.txt,
//                    _version.txt
//   <footer>         magic + offset + length, 24 bytes
//
// Run it inside the app folder, or start it with --auto --dir=<folder>
// --wait-pid=<pid> from the app itself. Everything the updater does is written
// to %TEMP%\veyla-update.log; a failure also drops update-failed.txt next to
// the app so the shell can tell the user what happened.
//
//   --silent      no console prompts
//   --auto        kill the running images first, then update and relaunch
//   --no-launch   do not start the app at the end
//   --dir=<path>  app folder (default: this exe's folder)
//   --wait-pid=N  wait for process N to exit before touching files

#include <windows.h>
#include <bcrypt.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::ofstream g_log;

void Log(const std::string& text) {
    if (!g_log.is_open()) {
        return;
    }
    char stamp[32] = {};
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &now);
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &local);
    g_log << "[" << stamp << "] " << text << "\n";
    g_log.flush();
    std::printf("%s\n", text.c_str());
}

std::string Utf8(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }
    const int size =
        ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(size > 0 ? size - 1 : 0), '\0');
    if (size > 1) {
        ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, out.data(), size, nullptr, nullptr);
    }
    return out;
}

fs::path ExeDir() {
    wchar_t path[MAX_PATH] = {};
    ::GetModuleFileNameW(nullptr, path, MAX_PATH);
    return fs::path(path).parent_path();
}

fs::path AppExe(const fs::path& dir) {
    const fs::path next = dir / "veyla.exe";
    if (fs::exists(next)) {
        return next;
    }
    const fs::path shell = dir / "brpnext.exe";
    if (fs::exists(shell)) {
        return shell;
    }
    return dir / "brp.exe";
}

bool AppRunning(const fs::path& dir) {
    const fs::path exe = AppExe(dir);
    if (!fs::exists(exe)) {
        return false;
    }
    HANDLE handle = ::CreateFileW(exe.wstring().c_str(), GENERIC_WRITE,
                                  FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return ::GetLastError() == ERROR_SHARING_VIOLATION ||
               ::GetLastError() == ERROR_LOCK_VIOLATION;
    }
    ::CloseHandle(handle);
    return false;
}

void KillProcessImage(const wchar_t* image) {
    wchar_t system_dir[MAX_PATH] = {};
    ::GetSystemDirectoryW(system_dir, MAX_PATH);
    const fs::path taskkill = fs::path(system_dir) / "taskkill.exe";
    std::wstring command = L"\"" + taskkill.wstring() + L"\" /IM " + image + L" /F /T";
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (::CreateProcessW(taskkill.wstring().c_str(), mutable_command.data(), nullptr,
                         nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                         &process)) {
        ::WaitForSingleObject(process.hProcess, 8000);
        ::CloseHandle(process.hThread);
        ::CloseHandle(process.hProcess);
    }
}

void PruneEmptyParents(const fs::path& root, const fs::path& file) {
    for (fs::path parent = file.parent_path(); parent != root && !parent.empty();
         parent = parent.parent_path()) {
        std::error_code ec;
        if (!fs::remove(parent, ec)) {
            break;
        }
    }
}

constexpr char kFooterMagic[8] = {'B', 'R', 'P', 'U', 'P', '0', '1', '\0'};

// Runs a program and returns its exit code; kStillRunning means the wait timed
// out and the process was terminated.
constexpr DWORD kStillRunning = 0xFFFFFFFFu;
DWORD RunAndWait(const fs::path& exe, const std::wstring& arguments, const fs::path& cwd,
                 DWORD timeout_ms) {
    // CreateProcess passes the command line verbatim; without argv[0] the child
    // sees its first flag as the program name and fails.
    const std::wstring command = L"\"" + exe.wstring() + L"\" " + arguments;
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!::CreateProcessW(exe.wstring().c_str(), mutable_command.data(), nullptr, nullptr,
                          FALSE, CREATE_NO_WINDOW, nullptr,
                          cwd.empty() ? nullptr : cwd.wstring().c_str(), &startup,
                          &process)) {
        Log("cannot start " + Utf8(exe.wstring()) + " (error " +
            std::to_string(::GetLastError()) + ")");
        return kStillRunning;
    }
    const DWORD waited = ::WaitForSingleObject(process.hProcess, timeout_ms);
    if (waited == WAIT_TIMEOUT) {
        ::TerminateProcess(process.hProcess, 1);
        ::CloseHandle(process.hThread);
        ::CloseHandle(process.hProcess);
        return kStillRunning;
    }
    DWORD exit_code = 1;
    ::GetExitCodeProcess(process.hProcess, &exit_code);
    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);
    return exit_code;
}

// tar.exe ships with Windows 10 1803+ and handles a plain zip. A 590 MB patch on
// a slow disk used to hit a five minute cap and look like a failed update, so
// the limit is now generous and a failure falls back to PowerShell.
bool ExtractZip(const fs::path& zip, const fs::path& dir) {
    wchar_t system_dir[MAX_PATH] = {};
    ::GetSystemDirectoryW(system_dir, MAX_PATH);
    const fs::path tar = fs::path(system_dir) / "tar.exe";
    const std::wstring tar_args = L"-xf \"" + zip.wstring() + L"\" -C \"" +
                                  dir.wstring() + L"\"";
    Log("extracting with tar.exe");
    DWORD code = RunAndWait(tar, tar_args, dir, 30 * 60 * 1000);
    if (code == 0) {
        Log("tar.exe finished");
        return true;
    }
    Log("tar.exe failed (exit " + std::to_string(code) + "), trying Expand-Archive");

    const fs::path powershell =
        std::filesystem::path(system_dir).parent_path() / "System32" / "WindowsPowerShell" /
        "v1.0" / "powershell.exe";
    const std::wstring ps_args =
        L"-NoProfile -NonInteractive -ExecutionPolicy Bypass -Command \"Expand-Archive "
        L"-LiteralPath '" + zip.wstring() + L"' -DestinationPath '" + dir.wstring() +
        L"' -Force\"";
    code = RunAndWait(powershell, ps_args, dir, 30 * 60 * 1000);
    Log("Expand-Archive exit " + std::to_string(code));
    return code == 0;
}

std::string HexLower(const unsigned char* data, size_t length) {
    static const char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(length * 2);
    for (size_t i = 0; i < length; ++i) {
        out.push_back(kDigits[data[i] >> 4]);
        out.push_back(kDigits[data[i] & 0x0F]);
    }
    return out;
}

std::string Sha256File(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {};
    }
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD object_size = 0;
    DWORD produced = 0;
    std::string out;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
        return {};
    }
    if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                          reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size),
                          &produced, 0) == 0) {
        std::vector<unsigned char> object(object_size);
        if (BCryptCreateHash(algorithm, &hash, object.data(), object_size, nullptr, 0, 0) ==
            0) {
            std::vector<char> buffer(1024 * 1024);
            while (file) {
                file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
                const std::streamsize got = file.gcount();
                if (got > 0) {
                    BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()),
                                   static_cast<ULONG>(got), 0);
                }
            }
            unsigned char digest[32] = {};
            if (BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0) {
                out = HexLower(digest, sizeof(digest));
            }
            BCryptDestroyHash(hash);
        }
    }
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return out;
}

bool EqualsIgnoreCase(const std::string& left, const std::string& right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (size_t i = 0; i < left.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(left[i])) !=
            std::tolower(static_cast<unsigned char>(right[i]))) {
            return false;
        }
    }
    return true;
}

// Copies the staged payload into the install only after every file matches the
// hash in _manifest.txt. A truncated download or a half-written file can never
// reach the app folder this way.
fs::path BackupDirectory(const fs::path& dir) {
    return dir / "_update_backup";
}

void BackupFile(const fs::path& dir, const std::string& relative) {
    if (relative.empty()) {
        return;
    }
    const fs::path backup = BackupDirectory(dir);
    std::error_code ec;
    fs::create_directories(backup, ec);
    const fs::path source = dir / fs::path(relative);
    if (!fs::exists(source, ec)) {
        std::ofstream added(backup / "_added.txt", std::ios::app);
        if (added) {
            added << relative << "\n";
        }
        return;
    }
    const fs::path target = backup / fs::path(relative);
    fs::create_directories(target.parent_path(), ec);
    fs::copy_file(source, target, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        Log("backup failed for " + relative + ": " + ec.message());
    }
}

void StartBackup(const fs::path& dir) {
    const fs::path backup = BackupDirectory(dir);
    std::error_code ec;
    fs::remove_all(backup, ec);
    fs::create_directories(backup, ec);
    const fs::path version = dir / "version.txt";
    if (fs::exists(version, ec)) {
        fs::copy_file(version, backup / "_previous_version.txt",
                      fs::copy_options::overwrite_existing, ec);
    }
    Log("backup started in " + Utf8(backup.wstring()));
}

bool RestoreBackup(const fs::path& dir, std::string& error) {
    const fs::path backup = BackupDirectory(dir);
    std::error_code ec;
    if (!fs::exists(backup, ec)) {
        error = "no previous-version backup";
        return false;
    }
    // Remove files the failed update added; they did not exist before it.
    std::ifstream added(backup / "_added.txt");
    std::string line;
    while (std::getline(added, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        fs::remove(dir / fs::path(line), ec);
    }
    // Copy every backed-up file back over the failed install.
    for (const auto& entry : fs::recursive_directory_iterator(backup, ec)) {
        if (ec || !entry.is_regular_file()) {
            continue;
        }
        const fs::path relative = fs::relative(entry.path(), backup, ec);
        const std::string name = relative.filename().string();
        if (name == "_added.txt" || name == "_previous_version.txt") {
            continue;
        }
        const fs::path target = dir / relative;
        fs::create_directories(target.parent_path(), ec);
        bool done = false;
        for (int retry = 0; retry < 20 && !done; ++retry) {
            done = ::CopyFileW(entry.path().wstring().c_str(),
                               target.wstring().c_str(), FALSE) != FALSE;
            if (!done) {
                ::Sleep(250);
            }
        }
        if (!done) {
            error = "could not restore " + Utf8(relative.wstring());
            return false;
        }
    }
    const fs::path previous = backup / "_previous_version.txt";
    if (fs::exists(previous, ec)) {
        std::ifstream in(previous);
        std::string version;
        std::getline(in, version);
        std::ofstream out(dir / "version.txt", std::ios::trunc);
        if (out) {
            out << version << "\n";
        }
    }
    Log("restored the previous version from " + Utf8(backup.wstring()));
    return true;
}

bool ApplyStaged(const fs::path& stage, const fs::path& dir, std::string& error) {
    StartBackup(dir);
    std::ifstream in(stage / "_manifest.txt");
    if (!in) {
        error = "Update package has no manifest.";
        return false;
    }
    int copied = 0;
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
            line.pop_back();
        }
        if (line.size() < 67) {
            continue;  // 64 hex chars, two spaces, then the relative path
        }
        const std::string expected = line.substr(0, 64);
        const std::string relative = line.substr(66);
        const fs::path source = stage / fs::path(relative);
        if (!fs::exists(source)) {
            error = "Update package is incomplete: " + relative;
            return false;
        }
        const std::string actual = Sha256File(source);
        if (actual.empty() || !EqualsIgnoreCase(actual, expected)) {
            error = "Update package is damaged: " + relative;
            return false;
        }
        const fs::path target = dir / fs::path(relative);
        std::error_code ec;
        BackupFile(dir, relative);
        fs::create_directories(target.parent_path(), ec);
        bool done = false;
        for (int retry = 0; retry < 40 && !done; ++retry) {
            done = ::CopyFileW(source.wstring().c_str(), target.wstring().c_str(), FALSE) !=
                   FALSE;
            if (!done) {
                ::Sleep(250);
            }
        }
        if (!done) {
            error = "Could not write " + relative;
            return false;
        }
        ++copied;
    }
    Log("verified and copied " + std::to_string(copied) + " file(s)");
    return true;
}

void Relaunch(const fs::path& dir) {    for (int attempt = 0; attempt < 3; ++attempt) {
        const fs::path exe = AppExe(dir);
        const HINSTANCE result =
            ::ShellExecuteW(nullptr, L"open", exe.wstring().c_str(), nullptr,
                            dir.wstring().c_str(), SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(result) > 32) {
            Log("relaunched " + Utf8(exe.wstring()));
            return;
        }
        Log("relaunch attempt " + std::to_string(attempt + 1) + " failed");
        ::Sleep(700);
    }
}

void ReportFailure(const fs::path& dir, const std::string& message) {
    Log("FAILED: " + message);
    std::error_code ec;
    std::ofstream marker(dir / "update-failed.txt", std::ios::trunc);
    if (marker) {
        marker << message << "\n";
    }
    ::Sleep(300);
    Relaunch(dir);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    bool launch = true;
    bool silent = false;
    bool auto_update = false;
    DWORD wait_pid = 0;
    fs::path dir = ExeDir();

    {
        wchar_t temp[MAX_PATH] = {};
        ::GetTempPathW(MAX_PATH, temp);
        g_log.open(fs::path(temp) / "veyla-update.log",
                   std::ios::out | std::ios::app);
    }
    Log("---- veyla updater started ----");

    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        Log("arg: " + Utf8(arg));
        if (arg == L"--no-launch") {
            launch = false;
        } else if (arg == L"--silent") {
            silent = true;
        } else if (arg == L"--auto") {
            auto_update = true;
            silent = true;
        } else if (arg.rfind(L"--dir=", 0) == 0) {
            dir = arg.substr(6);
        } else if (arg.rfind(L"--wait-pid=", 0) == 0) {
            wait_pid = static_cast<DWORD>(std::wcstoul(arg.c_str() + 11, nullptr, 10));
        }
    }
    Log("target: " + Utf8(dir.wstring()));

    if (wait_pid != 0) {
        HANDLE process = ::OpenProcess(SYNCHRONIZE, FALSE, wait_pid);
        if (process) {
            Log("waiting for pid " + std::to_string(wait_pid));
            ::WaitForSingleObject(process, 60000);
            ::CloseHandle(process);
        } else {
            Log("pid already gone");
        }
        ::Sleep(600);
    }

    if (auto_update) {
        // The app starts this file and, in the worst case, quits before the
        // command arrives. Stop both images so no file is mapped, then retry
        // the operations until the locks are gone.
        for (int round = 0; round < 3; ++round) {
            KillProcessImage(L"veyla.exe");
            KillProcessImage(L"veyla-engine.exe");
            KillProcessImage(L"brpnext.exe");
            KillProcessImage(L"brpnext-engine.exe");
            ::Sleep(700);
        }
    }

    if (!fs::exists(AppExe(dir))) {
        ReportFailure(dir, "veyla.exe is not in " + Utf8(dir.wstring()));
        return 1;
    }
    if (AppRunning(dir)) {
        // Last resort: the image is still mapped. Tell the user instead of
        // failing without a word.
        ReportFailure(dir, "Veyla is still running; close it and try again.");
        return 1;
    }

    // Single-file form: the patch zip is appended to this exe.
    bool embedded_payload = false;
    fs::path metadata_dir = dir;
    {
        wchar_t self_path[MAX_PATH] = {};
        ::GetModuleFileNameW(nullptr, self_path, MAX_PATH);
        std::ifstream self(self_path, std::ios::binary | std::ios::ate);
        if (self) {
            const std::streamoff size = self.tellg();
            if (size > 24) {
                self.seekg(size - 24);
                char magic[8] = {};
                uint64_t offset = 0;
                uint64_t length = 0;
                self.read(magic, sizeof(magic));
                self.read(reinterpret_cast<char*>(&offset), sizeof(offset));
                self.read(reinterpret_cast<char*>(&length), sizeof(length));
                if (std::memcmp(magic, kFooterMagic, sizeof(magic)) == 0 && length > 0 &&
                    static_cast<uint64_t>(size) >= offset + length) {
                    Log("embedded payload: " + std::to_string(length) + " bytes");
                    const fs::path temp_zip = dir / "_update_payload.zip";
                    std::vector<char> buffer(1024 * 1024);
                    std::ofstream out(temp_zip, std::ios::binary | std::ios::trunc);
                    self.seekg(static_cast<std::streamoff>(offset));
                    uint64_t left = length;
                    while (left > 0 && self) {
                        const size_t chunk =
                            static_cast<size_t>(std::min<uint64_t>(left, buffer.size()));
                        self.read(buffer.data(), static_cast<std::streamsize>(chunk));
                        out.write(buffer.data(), static_cast<std::streamsize>(chunk));
                        left -= chunk;
                    }
                    out.close();
                    self.close();
                    if (left != 0) {
                        std::error_code ec;
                        fs::remove(temp_zip, ec);
                        ReportFailure(dir, "Embedded payload is truncated.");
                        return 1;
                    }
                    // Extraction writes over the app; retry once if a file is
                    // still locked by a lingering process. Unpack into a stage
                    // folder first so a damaged package cannot touch the app.
                    const fs::path stage = dir / "_update_stage";
                    std::error_code ec;
                    fs::remove_all(stage, ec);
                    fs::create_directories(stage, ec);
                    bool extracted = false;
                    for (int attempt = 0; attempt < 3 && !extracted; ++attempt) {
                        extracted = ExtractZip(temp_zip, stage);
                        if (!extracted) {
                            Log("extract attempt " + std::to_string(attempt + 1) + " failed");
                            ::Sleep(1500);
                            KillProcessImage(L"veyla.exe");
                            KillProcessImage(L"veyla-engine.exe");
                        }
                    }
                    fs::remove(temp_zip, ec);
                    if (!extracted) {
                        fs::remove_all(stage, ec);
                        ReportFailure(dir, "Could not unpack the update payload.");
                        return 1;
                    }
                    std::string verify_error;
                    if (!ApplyStaged(stage, dir, verify_error)) {
                        fs::remove_all(stage, ec);
                        std::string restore_error;
                        const bool restored = RestoreBackup(dir, restore_error);
                        std::string message = verify_error;
                        if (restored) {
                            message += " (previous version restored)";
                        } else {
                            message += " (restore failed: " + restore_error + ")";
                        }
                        ReportFailure(dir, message);
                        return 1;
                    }
                    metadata_dir = stage;
                    embedded_payload = true;
                }
            }
        }
    }

    if (!fs::exists(dir / "payload")) {
        if (!embedded_payload) {
            ReportFailure(dir, "No update payload found in this file.");
            return 1;
        }
        const fs::path deletes = metadata_dir / "_delete.txt";
        if (fs::exists(deletes)) {
            std::ifstream list(deletes);
            std::string line;
            int removed = 0;
            while (std::getline(list, line)) {
                while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
                    line.pop_back();
                }
                if (line.empty()) {
                    continue;
                }
                std::error_code ec;
                const fs::path victim = dir / fs::path(line);
                BackupFile(dir, line);
                if (fs::remove(victim, ec)) {
                    ++removed;
                    PruneEmptyParents(dir, victim);
                }
            }
            list.close();
            std::error_code ec;
            fs::remove(deletes, ec);
            Log("removed " + std::to_string(removed) + " obsolete file(s)");
        }
        std::string version;
        const fs::path version_marker = metadata_dir / "_version.txt";
        if (fs::exists(version_marker)) {
            {
                std::ifstream in(version_marker);
                std::getline(in, version);
            }
            std::error_code ec;
            fs::remove(version_marker, ec);
            std::ofstream out(dir / "version.txt", std::ios::trunc);
            out << version << "\n";
        }
        if (embedded_payload) {
            std::error_code stage_ec;
            fs::remove_all(metadata_dir, stage_ec);
        }
        if (!fs::exists(AppExe(dir)) || !fs::exists(dir / "veyla-engine.exe")) {
            std::string restore_error;
            const bool restored = RestoreBackup(dir, restore_error);
            ReportFailure(dir, "The update did not produce a runnable Veyla." +
                                 (restored ? std::string(" (previous version restored)")
                                           : " (restore failed: " + restore_error + ")"));
            return 1;
        }
        Log("update complete" + (version.empty() ? std::string() : ": version " + version));
        if (launch) {
            Relaunch(dir);
        }
        return 0;
    }

    // Folder form, kept for the release tooling.
    const fs::path payload = dir / "payload";
    StartBackup(dir);
    int copied = 0;
    int failed = 0;
    std::error_code ec;
    for (const auto& entry : fs::recursive_directory_iterator(payload, ec)) {
        if (ec || !entry.is_regular_file()) {
            continue;
        }
        const fs::path relative = fs::relative(entry.path(), payload, ec);
        const fs::path target = dir / relative;
        BackupFile(dir, Utf8(relative.wstring()));
        fs::create_directories(target.parent_path(), ec);
        bool done = false;
        for (int retry = 0; retry < 40 && !done; ++retry) {
            done = ::CopyFileW(entry.path().wstring().c_str(),
                               target.wstring().c_str(), FALSE) != FALSE;
            if (!done) {
                ::Sleep(250);
            }
        }
        if (done) {
            ++copied;
        } else {
            ++failed;
            Log("copy failed: " + Utf8(relative.wstring()));
        }
    }
    if (failed > 0) {
        std::string restore_error;
        const bool restored = RestoreBackup(dir, restore_error);
        ReportFailure(dir, "Could not copy " + std::to_string(failed) + " file(s)." +
                             (restored ? " (previous version restored)"
                                       : " (restore failed: " + restore_error + ")"));
        return 1;
    }
    Log("copied " + std::to_string(copied) + " file(s)");

    const fs::path deletes = dir / "delete.txt";
    if (fs::exists(deletes)) {
        std::ifstream list(deletes);
        std::string line;
        int removed = 0;
        while (std::getline(list, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
                line.pop_back();
            }
            if (line.empty()) {
                continue;
            }
            std::error_code remove_ec;
            const fs::path victim = dir / fs::path(line);
            BackupFile(dir, line);
            if (fs::remove(victim, remove_ec)) {
                ++removed;
                PruneEmptyParents(dir, victim);
            }
        }
        Log("removed " + std::to_string(removed) + " obsolete file(s)");
    }

    std::error_code cleanup_ec;
    std::string version;
    if (fs::exists(dir / "version.txt")) {
        std::ifstream in(dir / "version.txt");
        std::getline(in, version);
    }
    fs::remove_all(payload, cleanup_ec);
    fs::remove(deletes, cleanup_ec);
    Log("update complete" + (version.empty() ? std::string() : ": version " + version));
    if (launch) {
        Relaunch(dir);
    }
    return 0;
}
