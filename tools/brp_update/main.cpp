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
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

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

bool AppRunning(const fs::path& dir) {
    const fs::path exe = dir / "brp.exe";
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
    fs::path dir = ExeDir();
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--no-launch") {
            launch = false;
        } else if (arg.rfind(L"--dir=", 0) == 0) {
            dir = arg.substr(6);
        }
    }

    std::printf("BetterRemotePlay update\nTarget: %s\n\n", Utf8(dir.wstring()).c_str());
    if (!fs::exists(dir / "brp.exe")) {
        std::printf("brp.exe is not in this folder. Put update.exe next to it.\n");
        std::printf("Press Enter to close.");
        std::getchar();
        return 1;
    }
    if (AppRunning(dir)) {
        std::printf("Close BetterRemotePlay first, then run update.exe again.\n");
        std::printf("Press Enter to close.");
        std::getchar();
        return 1;
    }

    const fs::path payload = dir / "payload";
    if (!fs::exists(payload)) {
        std::printf("payload\\ is missing. Extract the whole update zip here.\n");
        std::printf("Press Enter to close.");
        std::getchar();
        return 1;
    }

    int copied = 0;
    if (!CopyTree(payload, dir, copied)) {
        std::printf("\nUpdate failed. The app may be partially updated; run it again.\n");
        std::printf("Press Enter to close.");
        std::getchar();
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
        const fs::path exe = dir / "brp.exe";
        ::ShellExecuteW(nullptr, L"open", exe.wstring().c_str(), nullptr,
                        dir.wstring().c_str(), SW_SHOWNORMAL);
    }
    return 0;
}
