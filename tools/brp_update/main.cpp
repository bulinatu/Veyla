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

void Relaunch(const fs::path& dir) {
    for (int attempt = 0; attempt < 3; ++attempt) {
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
                    // still locked by a lingering process.
                    bool extracted = false;
                    for (int attempt = 0; attempt < 3 && !extracted; ++attempt) {
                        extracted = ExtractZip(temp_zip, dir);
                        if (!extracted) {
                            Log("extract attempt " + std::to_string(attempt + 1) + " failed");
                            ::Sleep(1500);
                            KillProcessImage(L"veyla.exe");
                            KillProcessImage(L"veyla-engine.exe");
                        }
                    }
                    std::error_code ec;
                    fs::remove(temp_zip, ec);
                    if (!extracted) {
                        ReportFailure(dir, "Could not unpack the update payload.");
                        return 1;
                    }
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
        const fs::path deletes = dir / "_delete.txt";
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
        const fs::path version_marker = dir / "_version.txt";
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
        Log("update complete" + (version.empty() ? std::string() : ": version " + version));
        if (launch) {
            Relaunch(dir);
        }
        return 0;
    }

    // Folder form, kept for the release tooling.
    const fs::path payload = dir / "payload";
    int copied = 0;
    int failed = 0;
    std::error_code ec;
    for (const auto& entry : fs::recursive_directory_iterator(payload, ec)) {
        if (ec || !entry.is_regular_file()) {
            continue;
        }
        const fs::path relative = fs::relative(entry.path(), payload, ec);
        const fs::path target = dir / relative;
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
        ReportFailure(dir, "Could not copy " + std::to_string(failed) + " file(s).");
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
