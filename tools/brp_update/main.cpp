// BetterRemotePlay updater.
//
// Applies a patch produced by tools/make_update.ps1. The patch is a folder
// layout:
//
//   update.exe        this program
//   payload\          files to copy over the app, relative paths preserved
//   delete.txt        files to remove (one relative path per line)
//   version.txt       new version label
//
// Extract the patch into the app folder and run update.exe. By default it
// starts brp.exe afterwards; pass --no-launch to skip that.

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

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
        ::WaitForSingleObject(process.hProcess, 5000);
        ::CloseHandle(process.hThread);
        ::CloseHandle(process.hProcess);
    }
}

void PruneEmptyParents(const fs::path& root, const fs::path& file) {
    // A delete list often leaves the folder behind (the optional rife\ tree is
    // the common case). Remove every parent that is now empty, stopping at the
    // app root and at the first non-empty folder.
    for (fs::path parent = file.parent_path(); parent != root && !parent.empty();
         parent = parent.parent_path()) {
        std::error_code ec;
        if (!fs::remove(parent, ec)) {
            break;
        }
    }
}

constexpr char kFooterMagic[8] = {'B', 'R', 'P', 'U', 'P', '0', '1', '\0'};

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

bool CopyTree(const fs::path& from, const fs::path& to, int& copied) {
    std::error_code ec;
    for (const auto& entry : fs::recursive_directory_iterator(from, ec)) {
        if (ec) {
            std::printf("cannot list %s: %s\n", Utf8(from.wstring()).c_str(), ec.message().c_str());
            return false;
        }
        if (!entry.is_regular_file()) {
            continue;
        }
        const fs::path relative = fs::relative(entry.path(), from, ec);
        const fs::path target = to / relative;
        fs::create_directories(target.parent_path(), ec);
        if (!::CopyFileW(entry.path().wstring().c_str(), target.wstring().c_str(), FALSE)) {
            std::printf("copy failed: %s (%lu)\n", Utf8(relative.wstring()).c_str(),
                        ::GetLastError());
            return false;
        }
        ++copied;
    }
    return true;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    bool launch = true;
    bool silent = false;
    bool auto_update = false;
    DWORD wait_pid = 0;
    fs::path dir = ExeDir();
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
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

    // Auto-update path: the app starts this with its own PID and exits. Wait
    // for it so no file is replaced while the process still has it mapped.
    if (wait_pid != 0) {
        HANDLE process = ::OpenProcess(SYNCHRONIZE, FALSE, wait_pid);
        if (process) {
            ::WaitForSingleObject(process, 30000);
            ::CloseHandle(process);
        }
        ::Sleep(600);
    }

    bool embedded_payload = false;
    if (auto_update) {
        // The in-app updater starts this file while the shell still runs. Stop
        // both images before touching files; taskkill also waits for them.
        KillProcessImage(L"veyla.exe");
        KillProcessImage(L"veyla-engine.exe");
        KillProcessImage(L"brpnext.exe");
        KillProcessImage(L"brpnext-engine.exe");
        ::Sleep(800);
    }

    if (!silent) {
        std::printf("Veyla update\nTarget: %s\n\n", Utf8(dir.wstring()).c_str());
    }
    if (!fs::exists(AppExe(dir))) {
        std::printf("veyla.exe/brpnext.exe is not in this folder.\n");
        if (!silent) {
            std::printf("Press Enter to close.");
            std::getchar();
        }
        return 1;
    }
    if (AppRunning(dir)) {
        std::printf("Close BetterRemotePlay first, then run update.exe again.\n");
        if (!silent) {
            std::printf("Press Enter to close.");
            std::getchar();
        }
        return 1;
    }

    // Single-file form: the patch zip is appended to this exe, followed by a
    // 24-byte footer. Extract it, then fall through to the same delete/version
    // handling the folder form uses.
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
                    const fs::path temp_zip = dir / "_update_payload.zip";
                    std::vector<char> buffer(256 * 1024);
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
                    if (left != 0) {
                        std::printf("Embedded payload is truncated.\n");
                        return 1;
                    }
                    wchar_t system_dir[MAX_PATH] = {};
                    ::GetSystemDirectoryW(system_dir, MAX_PATH);
                    const fs::path tar = fs::path(system_dir) / "tar.exe";
                    std::wstring command = L"\"" + tar.wstring() + L"\" -xf \"" +
                                           temp_zip.wstring() + L"\" -C \"" +
                                           dir.wstring() + L"\"";
                    std::vector<wchar_t> mutable_command(command.begin(), command.end());
                    mutable_command.push_back(L'\0');
                    STARTUPINFOW startup{};
                    startup.cb = sizeof(startup);
                    PROCESS_INFORMATION process{};
                    const BOOL started =
                        ::CreateProcessW(tar.wstring().c_str(), mutable_command.data(), nullptr,
                                         nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                                         dir.wstring().c_str(), &startup, &process);
                    if (!started) {
                        std::printf("Could not start tar.exe.\n");
                        return 1;
                    }
                    ::WaitForSingleObject(process.hProcess, 5 * 60 * 1000);
                    DWORD exit_code = 1;
                    ::GetExitCodeProcess(process.hProcess, &exit_code);
                    ::CloseHandle(process.hThread);
                    ::CloseHandle(process.hProcess);
                    std::error_code ec;
                    fs::remove(temp_zip, ec);
                    if (exit_code != 0) {
                        std::printf("Extracting the payload failed (tar exit %lu).\n",
                                    exit_code);
                        return 1;
                    }
                    std::printf("Applied embedded payload.\n");
                    embedded_payload = true;
                }
            }
        }
    }

    if (!fs::exists(dir / "payload")) {
        if (!embedded_payload) {
            std::printf("No update payload found in this file.\n");
            if (!silent) {
                std::printf("Press Enter to close.");
                std::getchar();
            }
            return 1;
        }
        // Single-file updates unpack straight into the folder, so there is no
        // payload\ to copy. Only the delete list and version remain.
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
            // Removing an open file fails on Windows (the stream has no
            // FILE_SHARE_DELETE), so close the list before deleting it.
            list.close();
            std::error_code ec;
            fs::remove(deletes, ec);
            std::printf("Removed %d obsolete file(s).\n", removed);
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
        std::printf("\nUpdate complete%s%s.\n", version.empty() ? "" : ": version ",
                    version.c_str());
        if (launch) {
            const fs::path exe = AppExe(dir);
            ::ShellExecuteW(nullptr, L"open", exe.wstring().c_str(), nullptr,
                            dir.wstring().c_str(), SW_SHOWNORMAL);
        }
        return 0;
    }

    const fs::path payload = dir / "payload";
    if (!fs::exists(payload)) {
        std::printf("payload\\ is missing. Extract the whole update zip here.\n");
        if (!silent) {
            std::printf("Press Enter to close.");
            std::getchar();
        }
        return 1;
    }

    int copied = 0;
    if (!CopyTree(payload, dir, copied)) {
        std::printf("\nUpdate failed. The app may be partially updated; run it again.\n");
        if (!silent) {
            std::printf("Press Enter to close.");
            std::getchar();
        }
        return 1;
    }
    std::printf("Copied %d file(s).\n", copied);

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
            std::error_code ec;
            const fs::path victim = dir / fs::path(line);
            if (fs::remove(victim, ec)) {
                ++removed;
                PruneEmptyParents(dir, victim);
            }
        }
        std::printf("Removed %d obsolete file(s).\n", removed);
    }

    std::error_code ec;
    const fs::path version_src = dir / "version.txt";
    std::string version;
    if (fs::exists(version_src)) {
        std::ifstream in(version_src);
        std::getline(in, version);
    }

    fs::remove_all(payload, ec);
    fs::remove(deletes, ec);
    std::printf("\nUpdate complete%s%s.\n", version.empty() ? "" : ": version ",
                version.c_str());

    if (launch) {
        const fs::path exe = AppExe(dir);
        ::ShellExecuteW(nullptr, L"open", exe.wstring().c_str(), nullptr,
                        dir.wstring().c_str(), SW_SHOWNORMAL);
    }
    return 0;
}
