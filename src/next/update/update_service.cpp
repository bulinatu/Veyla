#include "update/update_service.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <map>
#include <system_error>
#include <memory>
#include <string>
#include <vector>

namespace brp::next {
namespace {

// --- tiny JSON reader (objects, arrays, strings, numbers, bool, null) --------

struct Json {
    enum class Type { Null, Bool, Number, String, Array, Object };
    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string text;
    std::vector<Json> items;
    std::vector<std::pair<std::string, Json>> members;

    const Json* Get(const std::string& key) const {
        for (const auto& member : members) {
            if (member.first == key) {
                return &member.second;
            }
        }
        return nullptr;
    }
    std::string String(const std::string& key, const std::string& fallback = {}) const {
        const Json* value = Get(key);
        return value && value->type == Type::String ? value->text : fallback;
    }
    bool Bool(const std::string& key, bool fallback = false) const {
        const Json* value = Get(key);
        return value && value->type == Type::Bool ? value->boolean : fallback;
    }
    long long Integer(const std::string& key, long long fallback = 0) const {
        const Json* value = Get(key);
        return value && value->type == Type::Number
                   ? static_cast<long long>(value->number)
                   : fallback;
    }
};

class JsonReader {
public:
    explicit JsonReader(const std::string& text) : text_(text) {}
    bool Parse(Json& out) {
        Skip();
        if (!Value(out)) {
            return false;
        }
        Skip();
        return true;
    }

private:
    const std::string& text_;
    std::size_t pos_ = 0;

    void Skip() {
        while (pos_ < text_.size() &&
               (text_[pos_] == ' ' || text_[pos_] == '\n' || text_[pos_] == '\r' ||
                text_[pos_] == '\t')) {
            ++pos_;
        }
    }
    bool Literal(const char* word) {
        const std::size_t length = std::char_traits<char>::length(word);
        if (text_.compare(pos_, length, word) != 0) {
            return false;
        }
        pos_ += length;
        return true;
    }
    bool String(std::string& out) {
        if (pos_ >= text_.size() || text_[pos_] != '"') {
            return false;
        }
        ++pos_;
        out.clear();
        while (pos_ < text_.size()) {
            const char ch = text_[pos_++];
            if (ch == '"') {
                return true;
            }
            if (ch != '\\') {
                out.push_back(ch);
                continue;
            }
            if (pos_ >= text_.size()) {
                return false;
            }
            const char esc = text_[pos_++];
            switch (esc) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    if (pos_ + 4 > text_.size()) {
                        return false;
                    }
                    unsigned code = 0;
                    for (int i = 0; i < 4; ++i) {
                        const char digit = text_[pos_++];
                        code <<= 4;
                        if (digit >= '0' && digit <= '9') {
                            code |= static_cast<unsigned>(digit - '0');
                        } else if (digit >= 'a' && digit <= 'f') {
                            code |= static_cast<unsigned>(digit - 'a' + 10);
                        } else if (digit >= 'A' && digit <= 'F') {
                            code |= static_cast<unsigned>(digit - 'A' + 10);
                        } else {
                            return false;
                        }
                    }
                    if (code < 0x80) {
                        out.push_back(static_cast<char>(code));
                    } else if (code < 0x800) {
                        out.push_back(static_cast<char>(0xC0 | (code >> 6)));
                        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                    } else {
                        out.push_back(static_cast<char>(0xE0 | (code >> 12)));
                        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                    }
                    break;
                }
                default: return false;
            }
        }
        return false;
    }
    bool Number(Json& out) {
        const std::size_t start = pos_;
        if (pos_ < text_.size() && (text_[pos_] == '-' || text_[pos_] == '+')) {
            ++pos_;
        }
        bool any = false;
        while (pos_ < text_.size()) {
            const char ch = text_[pos_];
            if ((ch >= '0' && ch <= '9') || ch == '.' || ch == 'e' || ch == 'E' ||
                ch == '+' || ch == '-') {
                any = true;
                ++pos_;
            } else {
                break;
            }
        }
        if (!any) {
            return false;
        }
        out.type = Json::Type::Number;
        out.number = std::strtod(text_.substr(start, pos_ - start).c_str(), nullptr);
        return true;
    }
    bool Value(Json& out) {
        if (pos_ >= text_.size()) {
            return false;
        }
        const char ch = text_[pos_];
        if (ch == '{') {
            ++pos_;
            out.type = Json::Type::Object;
            Skip();
            if (pos_ < text_.size() && text_[pos_] == '}') {
                ++pos_;
                return true;
            }
            for (;;) {
                Skip();
                std::string key;
                if (!String(key)) {
                    return false;
                }
                Skip();
                if (pos_ >= text_.size() || text_[pos_] != ':') {
                    return false;
                }
                ++pos_;
                Skip();
                Json value;
                if (!Value(value)) {
                    return false;
                }
                out.members.emplace_back(std::move(key), std::move(value));
                Skip();
                if (pos_ < text_.size() && text_[pos_] == ',') {
                    ++pos_;
                    continue;
                }
                if (pos_ < text_.size() && text_[pos_] == '}') {
                    ++pos_;
                    return true;
                }
                return false;
            }
        }
        if (ch == '[') {
            ++pos_;
            out.type = Json::Type::Array;
            Skip();
            if (pos_ < text_.size() && text_[pos_] == ']') {
                ++pos_;
                return true;
            }
            for (;;) {
                Skip();
                Json value;
                if (!Value(value)) {
                    return false;
                }
                out.items.push_back(std::move(value));
                Skip();
                if (pos_ < text_.size() && text_[pos_] == ',') {
                    ++pos_;
                    continue;
                }
                if (pos_ < text_.size() && text_[pos_] == ']') {
                    ++pos_;
                    return true;
                }
                return false;
            }
        }
        if (ch == '"') {
            out.type = Json::Type::String;
            return String(out.text);
        }
        if (Literal("true")) {
            out.type = Json::Type::Bool;
            out.boolean = true;
            return true;
        }
        if (Literal("false")) {
            out.type = Json::Type::Bool;
            out.boolean = false;
            return true;
        }
        if (Literal("null")) {
            out.type = Json::Type::Null;
            return true;
        }
        return Number(out);
    }
};

std::wstring Widen(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int size =
        ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                              nullptr, 0);
    std::wstring out(static_cast<std::size_t>(size), L'\0');
    if (size > 0) {
        ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                              out.data(), size);
    }
    return out;
}

std::string Narrow(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }
    const int size = ::WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                           static_cast<int>(text.size()), nullptr, 0,
                                           nullptr, nullptr);
    std::string out(static_cast<std::size_t>(size), '\0');
    if (size > 0) {
        ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                              out.data(), size, nullptr, nullptr);
    }
    return out;
}

struct Url {
    std::wstring host;
    std::wstring path;
    std::wstring extra;
    INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
    bool secure = true;
};

bool CrackUrl(const std::wstring& url, Url& out) {
    std::vector<wchar_t> buffer(url.begin(), url.end());
    buffer.push_back(L'\0');
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!::WinHttpCrackUrl(buffer.data(), static_cast<DWORD>(url.size()), 0, &parts)) {
        return false;
    }
    out.host.assign(parts.lpszHostName, parts.dwHostNameLength);
    out.path.assign(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.lpszExtraInfo && parts.dwExtraInfoLength > 0) {
        out.extra.assign(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    }
    out.port = parts.nPort;
    out.secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
    return true;
}

// GET url into sink (body or file). Returns false with a human error.
bool HttpGet(const std::wstring& url, const std::wstring& headers,
             const std::function<bool(const char*, std::size_t)>& sink,
             std::string& error, std::atomic<bool>* stop) {
    Url parts;
    if (!CrackUrl(url, parts)) {
        error = "invalid URL";
        return false;
    }
    HINTERNET session = ::WinHttpOpen(
        L"Veyla/2.5 (Windows)", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        error = "WinHTTP session failed";
        return false;
    }
    ::WinHttpSetTimeouts(session, 10000, 10000, 20000, 60000);
    HINTERNET connect =
        ::WinHttpConnect(session, parts.host.c_str(), parts.port, 0);
    if (!connect) {
        error = "cannot reach " + Narrow(parts.host);
        ::WinHttpCloseHandle(session);
        return false;
    }
    std::wstring target = parts.path + parts.extra;
    HINTERNET request = ::WinHttpOpenRequest(
        connect, L"GET", target.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, parts.secure ? WINHTTP_FLAG_SECURE : 0);
    if (!request) {
        error = "cannot open request";
        ::WinHttpCloseHandle(connect);
        ::WinHttpCloseHandle(session);
        return false;
    }
    DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    ::WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &redirect,
                       sizeof(redirect));
    if (!headers.empty()) {
        ::WinHttpAddRequestHeaders(request, headers.c_str(),
                                   static_cast<DWORD>(headers.size()),
                                   WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
    }
    bool ok = false;
    if (!::WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                              WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        error = "send failed";
    } else if (!::WinHttpReceiveResponse(request, nullptr)) {
        error = "no response";
    } else {
        DWORD status = 0;
        DWORD size = sizeof(status);
        ::WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                              WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                              WINHTTP_NO_HEADER_INDEX);
        if (status >= 400) {
            error = "HTTP " + std::to_string(status);
            if (status == 404) {
                error += " (repository or release not found)";
            } else if (status == 403) {
                error += " (GitHub rate limit; try later)";
            }
        } else {
            ok = true;
            std::vector<char> buffer(128 * 1024);
            for (;;) {
                if (stop && stop->load()) {
                    error = "cancelled";
                    ok = false;
                    break;
                }
                DWORD available = 0;
                if (!::WinHttpQueryDataAvailable(request, &available)) {
                    error = "read failed";
                    ok = false;
                    break;
                }
                if (available == 0) {
                    break;
                }
                const DWORD want =
                    std::min<DWORD>(available, static_cast<DWORD>(buffer.size()));
                DWORD read = 0;
                if (!::WinHttpReadData(request, buffer.data(), want, &read) || read == 0) {
                    error = "read failed";
                    ok = false;
                    break;
                }
                if (sink && !sink(buffer.data(), read)) {
                    error = "write failed";
                    ok = false;
                    break;
                }
            }
        }
    }
    ::WinHttpCloseHandle(request);
    ::WinHttpCloseHandle(connect);
    ::WinHttpCloseHandle(session);
    return ok;
}

std::vector<int> VersionParts(const std::string& tag) {
    std::vector<int> parts;
    std::size_t i = 0;
    if (i < tag.size() && (tag[i] == 'v' || tag[i] == 'V')) {
        ++i;
    }
    int value = 0;
    bool any = false;
    for (; i < tag.size(); ++i) {
        const char ch = tag[i];
        if (ch >= '0' && ch <= '9') {
            value = value * 10 + (ch - '0');
            any = true;
        } else if (ch == '.' || ch == '-' || ch == '_' || ch == '+') {
            if (any) {
                parts.push_back(value);
                value = 0;
                any = false;
            }
        } else {
            break;
        }
    }
    if (any) {
        parts.push_back(value);
    }
    return parts;
}

int CompareVersions(const std::string& a, const std::string& b) {
    const auto left = VersionParts(a);
    const auto right = VersionParts(b);
    const std::size_t count = std::max(left.size(), right.size());
    for (std::size_t i = 0; i < count; ++i) {
        const int l = i < left.size() ? left[i] : 0;
        const int r = i < right.size() ? right[i] : 0;
        if (l != r) {
            return l < r ? -1 : 1;
        }
    }
    return 0;
}

std::string TrimNotes(std::string body) {
    for (char& ch : body) {
        if (ch == '\r' || ch == '\n' || ch == '\t') {
            ch = ' ';
        }
    }
    while (!body.empty() && body.front() == ' ') {
        body.erase(body.begin());
    }
    while (!body.empty() && body.back() == ' ') {
        body.pop_back();
    }
    // Collapse whitespace runs: GitHub release bodies are markdown with long
    // indentation runs, which reads as a wall of gaps in a single line.
    std::string compact;
    compact.reserve(body.size());
    bool space = false;
    for (char ch : body) {
        if (ch == ' ') {
            if (!space && !compact.empty()) {
                compact.push_back(' ');
            }
            space = true;
        } else {
            compact.push_back(ch);
            space = false;
        }
    }
    body = std::move(compact);
    if (body.size() > 190) {
        body.resize(187);
        body += "...";
    }
    return body;
}

std::string UpdatePhaseName(UpdatePhase phase) {
    switch (phase) {
        case UpdatePhase::Idle: return "idle";
        case UpdatePhase::Checking: return "checking";
        case UpdatePhase::Available: return "available";
        case UpdatePhase::Downloading: return "downloading";
        case UpdatePhase::Ready: return "ready";
        case UpdatePhase::UpToDate: return "up-to-date";
        case UpdatePhase::Error: return "error";
    }
    return "idle";
}

}  // namespace

#ifndef BRP_APP_VERSION
#define BRP_APP_VERSION "2.5.0"
#endif

std::string AppVersion() { return BRP_APP_VERSION; }

UpdateService::UpdateService() = default;

UpdateService::~UpdateService() {
    stop_.store(true);
    JoinWorker();
}

void UpdateService::JoinWorker() {
    if (worker_.joinable()) {
        worker_.join();
    }
}

UpdateStatus UpdateService::Snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

bool UpdateService::Busy() const { return busy_.load(); }

void UpdateService::Check(const std::string& repo, bool include_prerelease,
                          const std::string& current_version) {
    if (busy_.exchange(true)) {
        return;
    }
    stop_.store(false);
    JoinWorker();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        status_ = {};
        status_.phase = UpdatePhase::Checking;
        asset_url_.clear();
        asset_size_ = 0;
    }
    worker_ = std::thread([this, repo, include_prerelease, current_version]() {
        RunCheck(repo, include_prerelease, current_version);
        busy_.store(false);
    });
}

void UpdateService::RunCheck(std::string repo, bool include_prerelease,
                             std::string current) {
    auto fail = [this](const std::string& message) {
        std::lock_guard<std::mutex> lock(mutex_);
        status_.phase = UpdatePhase::Error;
        status_.error = message;
    };
    if (repo.empty()) {
        fail("Set a GitHub repository (owner/name) first");
        return;
    }
    if (repo.find('/') == std::string::npos || repo.find(' ') != std::string::npos) {
        fail("Repository must look like owner/name");
        return;
    }
    const std::wstring url =
        L"https://api.github.com/repos/" + Widen(repo) + L"/releases?per_page=30";
    const std::wstring headers =
        L"Accept: application/vnd.github+json\r\n"
        L"X-GitHub-Api-Version: 2022-11-28\r\n";
    std::string body;
    body.reserve(256 * 1024);
    std::string error;
    const bool ok = HttpGet(url, headers,
                            [&body](const char* data, std::size_t size) {
                                if (body.size() + size > 4 * 1024 * 1024) {
                                    return false;
                                }
                                body.append(data, size);
                                return true;
                            },
                            error, &stop_);
    if (!ok) {
        fail(error.empty() ? "GitHub request failed" : error);
        return;
    }
    Json root;
    JsonReader reader(body);
    if (!reader.Parse(root) || root.type != Json::Type::Array) {
        fail("GitHub returned an unreadable release list");
        return;
    }
    const Json* best = nullptr;
    int best_kind = 0;  // 2 stable, 1 beta, 0 none
    for (const Json& release : root.items) {
        if (release.type != Json::Type::Object) {
            continue;
        }
        if (release.Bool("draft", false)) {
            continue;
        }
        const bool pre = release.Bool("prerelease", false);
        if (pre && !include_prerelease) {
            continue;
        }
        const std::string tag = release.String("tag_name");
        if (tag.empty() || CompareVersions(tag, current) <= 0) {
            continue;
        }
        const int kind = pre ? 1 : 2;
        if (!best || kind > best_kind ||
            (kind == best_kind && CompareVersions(tag, best->String("tag_name")) > 0)) {
            best = &release;
            best_kind = kind;
        }
    }
    if (!best) {
        std::lock_guard<std::mutex> lock(mutex_);
        status_.phase = UpdatePhase::UpToDate;
        status_.version = current;
        return;
    }

    const std::string tag = best->String("tag_name");
    std::string version = tag;
    if (!version.empty() && (version[0] == 'v' || version[0] == 'V')) {
        version.erase(version.begin());
    }
    std::string asset_url;
    std::string asset_name;
    long long asset_size = 0;
    const Json* assets = best->Get("assets");
    if (assets && assets->type == Json::Type::Array) {
        const Json* fallback = nullptr;
        for (const Json& asset : assets->items) {
            const std::string name = asset.String("name");
            const std::string url = asset.String("browser_download_url");
            if (url.empty()) {
                continue;
            }
            std::string lower = name;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            const bool exe = lower.size() > 4 &&
                             lower.compare(lower.size() - 4, 4, ".exe") == 0;
            if (!exe) {
                continue;
            }
            if (lower.find("update") != std::string::npos) {
                asset_url = url;
                asset_name = name;
                asset_size = asset.Integer("size");
                break;
            }
            if (!fallback) {
                fallback = &asset;
            }
        }
        if (asset_url.empty() && fallback) {
            asset_url = fallback->String("browser_download_url");
            asset_name = fallback->String("name");
            asset_size = fallback->Integer("size");
        }
    }
    if (asset_url.empty()) {
        fail("Release " + tag + " has no update .exe asset");
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    status_.phase = UpdatePhase::Available;
    status_.version = version;
    status_.notes = TrimNotes(best->String("body"));
    status_.page_url = best->String("html_url");
    status_.error.clear();
    status_.progress = 0.0;
    asset_url_ = asset_url;
    asset_size_ = asset_size;
    (void)asset_name;
}

void UpdateService::Download() {
    if (busy_.exchange(true)) {
        return;
    }
    stop_.store(false);
    JoinWorker();
    std::string url;
    std::string version;
    long long expected = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (status_.phase != UpdatePhase::Available || asset_url_.empty()) {
            busy_.store(false);
            return;
        }
        url = asset_url_;
        version = status_.version;
        expected = asset_size_;
        status_.phase = UpdatePhase::Downloading;
        status_.progress = 0.0;
        status_.error.clear();
    }
    worker_ = std::thread([this, url, version, expected]() {
        RunDownload(url, version, expected);
        busy_.store(false);
    });
}

void UpdateService::RunDownload(const std::string& url, const std::string& version,
                                long long expected) {
    auto fail = [this](const std::string& message) {
        std::lock_guard<std::mutex> lock(mutex_);
        status_.phase = UpdatePhase::Error;
        status_.error = message;
    };
    wchar_t temp[MAX_PATH] = {};
    ::GetTempPathW(MAX_PATH, temp);
    std::filesystem::path dir(temp);
    std::string safe = version;
    for (char& ch : safe) {
        if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '.' && ch != '-') {
            ch = '_';
        }
    }
    const std::filesystem::path target =
        dir / ("Veyla-" + safe + "-update.exe");

    // A previous session may already have the exact asset; reuse it.
    std::error_code ec;
    if (std::filesystem::exists(target, ec) && expected > 0 &&
        static_cast<long long>(std::filesystem::file_size(target, ec)) == expected) {
        std::lock_guard<std::mutex> lock(mutex_);
        status_.phase = UpdatePhase::Ready;
        status_.asset_path = target.string();
        status_.progress = 1.0;
        return;
    }

    std::ofstream file(target, std::ios::binary | std::ios::trunc);
    if (!file) {
        fail("Cannot write to the temp folder");
        return;
    }
    long long written = 0;
    std::string error;
    const bool ok = HttpGet(
        Widen(url),
        L"Accept: application/octet-stream\r\n",
        [&](const char* data, std::size_t size) {
            file.write(data, static_cast<std::streamsize>(size));
            if (!file) {
                return false;
            }
            written += static_cast<long long>(size);
            if (expected > 0) {
                std::lock_guard<std::mutex> lock(mutex_);
                status_.progress =
                    std::clamp(static_cast<double>(written) / expected, 0.0, 1.0);
            }
            return true;
        },
        error, &stop_);
    file.close();
    if (!ok) {
        std::filesystem::remove(target, ec);
        fail(error.empty() ? "Download failed" : error);
        return;
    }
    if (expected > 0 && written < expected) {
        std::filesystem::remove(target, ec);
        fail("Download truncated");
        return;
    }
    if (written < 200 * 1024) {
        std::filesystem::remove(target, ec);
        fail("Downloaded file is too small to be the updater");
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    status_.phase = UpdatePhase::Ready;
    status_.asset_path = target.string();
    status_.progress = 1.0;
}

bool UpdateService::Install(const std::filesystem::path& app_dir, unsigned long pid,
                            std::string& error) {
    std::string asset;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (status_.phase != UpdatePhase::Ready || status_.asset_path.empty()) {
            error = "No downloaded update";
            return false;
        }
        asset = status_.asset_path;
    }
    std::wstring command = L"\"" + Widen(asset) + L"\" --auto --dir=\"" +
                           app_dir.wstring() + L"\" --wait-pid=" +
                           std::to_wstring(pid);
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!::CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, FALSE,
                          CREATE_NO_WINDOW, nullptr, app_dir.wstring().c_str(),
                          &startup, &process)) {
        error = "Cannot start the updater (error " +
                std::to_string(::GetLastError()) + ")";
        return false;
    }
    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);
    return true;
}

// Kept here so the bridge can expose a stable string even if the enum grows.
std::string UpdateStateName(UpdatePhase phase) { return UpdatePhaseName(phase); }

}  // namespace brp::next
