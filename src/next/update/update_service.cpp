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

struct SemVer {
    std::vector<long long> numeric;
    std::vector<std::string> prerelease;
    bool has_prerelease = false;
};

SemVer ParseVersion(const std::string& tag) {
    SemVer version;
    std::size_t i = 0;
    if (i < tag.size() && (tag[i] == 'v' || tag[i] == 'V')) {
        ++i;
    }
    long long value = 0;
    bool any = false;
    auto push_numeric = [&]() {
        if (any) {
            version.numeric.push_back(value);
            value = 0;
            any = false;
        }
    };
    for (; i < tag.size(); ++i) {
        const char ch = tag[i];
        if (ch >= '0' && ch <= '9') {
            value = value * 10 + (ch - '0');
            any = true;
        } else if (ch == '.') {
            push_numeric();
        } else {
            break;
        }
    }
    push_numeric();
    if (i < tag.size() && tag[i] == '-') {
        ++i;
        version.has_prerelease = true;
        std::string part;
        for (; i < tag.size(); ++i) {
            if (tag[i] == '.') {
                version.prerelease.push_back(part);
                part.clear();
            } else if (tag[i] == '+') {
                break;
            } else {
                part.push_back(static_cast<char>(std::tolower(
                    static_cast<unsigned char>(tag[i]))));
            }
        }
        if (!part.empty()) {
            version.prerelease.push_back(part);
        }
    }
    return version;
}

int ComparePrerelease(const std::vector<std::string>& a,
                      const std::vector<std::string>& b) {
    const std::size_t count = std::max(a.size(), b.size());
    for (std::size_t i = 0; i < count; ++i) {
        if (i >= a.size()) return -1;  // shorter prerelease is older
        if (i >= b.size()) return 1;
        const std::string& left = a[i];
        const std::string& right = b[i];
        const bool left_number =
            !left.empty() && std::all_of(left.begin(), left.end(), [](unsigned char ch) {
                return std::isdigit(ch) != 0;
            });
        const bool right_number =
            !right.empty() && std::all_of(right.begin(), right.end(), [](unsigned char ch) {
                return std::isdigit(ch) != 0;
            });
        if (left_number && right_number) {
            const long long l = std::strtoll(left.c_str(), nullptr, 10);
            const long long r = std::strtoll(right.c_str(), nullptr, 10);
            if (l != r) return l < r ? -1 : 1;
        } else if (left_number != right_number) {
            // numeric identifiers have lower precedence than alphanumeric ones
            return left_number ? -1 : 1;
        } else {
            // "beta2" vs "beta10": same alpha prefix, numeric suffix compared
            // as a number, not as text.
            std::size_t left_digits = left.size();
            while (left_digits > 0 &&
                   std::isdigit(static_cast<unsigned char>(left[left_digits - 1]))) {
                --left_digits;
            }
            std::size_t right_digits = right.size();
            while (right_digits > 0 &&
                   std::isdigit(static_cast<unsigned char>(right[right_digits - 1]))) {
                --right_digits;
            }
            const std::string left_prefix = left.substr(0, left_digits);
            const std::string right_prefix = right.substr(0, right_digits);
            if (left_prefix != right_prefix) {
                return left_prefix < right_prefix ? -1 : 1;
            }
            const bool left_has_number = left_digits < left.size();
            const bool right_has_number = right_digits < right.size();
            if (left_has_number && right_has_number) {
                const long long l = std::strtoll(left.c_str() + left_digits, nullptr, 10);
                const long long r = std::strtoll(right.c_str() + right_digits, nullptr, 10);
                if (l != r) return l < r ? -1 : 1;
            } else if (left_has_number != right_has_number) {
                return left_has_number ? 1 : -1;
            } else if (left != right) {
                return left < right ? -1 : 1;
            }
        }
    }
    return 0;
}

int CompareVersions(const std::string& a, const std::string& b) {
    const SemVer left = ParseVersion(a);
    const SemVer right = ParseVersion(b);
    const std::size_t count = std::max(left.numeric.size(), right.numeric.size());
    for (std::size_t i = 0; i < count; ++i) {
        const long long l = i < left.numeric.size() ? left.numeric[i] : 0;
        const long long r = i < right.numeric.size() ? right.numeric[i] : 0;
        if (l != r) {
            return l < r ? -1 : 1;
        }
    }
    if (left.has_prerelease != right.has_prerelease) {
        return left.has_prerelease ? -1 : 1;  // 3.0.0 > 3.0.0-beta2
    }
    if (!left.has_prerelease) {
        return 0;
    }
    return ComparePrerelease(left.prerelease, right.prerelease);
}

std::string TrimAscii(std::string value) {
    std::size_t begin = 0;
    while (begin < value.size() &&
           std::isspace(static_cast<unsigned char>(value[begin]))) {
        ++begin;
    }
    std::size_t end = value.size();
    while (end > begin &&
           std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }
    return value.substr(begin, end - begin);
}

// Drops the markdown that reads as noise in a small native card: heading
// hashes, emphasis markers, code ticks and quote markers. Links keep their
// label, so "[notes](https://...)" becomes "notes".
std::string CleanNotesLine(std::string line) {
    std::string out;
    out.reserve(line.size());
    for (std::size_t i = 0; i < line.size();) {
        if (line[i] == '[') {
            const std::size_t close = line.find(']', i + 1);
            if (close != std::string::npos && close + 1 < line.size() &&
                line[close + 1] == '(') {
                const std::size_t end = line.find(')', close + 2);
                if (end != std::string::npos) {
                    out += CleanNotesLine(line.substr(i + 1, close - i - 1));
                    i = end + 1;
                    continue;
                }
            }
        }
        const char ch = line[i];
        if (ch == '#' || ch == '*' || ch == '_' || ch == '`' || ch == '>') {
            ++i;
            continue;
        }
        out.push_back(ch);
        ++i;
    }
    return TrimAscii(std::move(out));
}

// The app shows a friendly, multi-line summary. Release notes carry it in a
// hidden HTML comment so the GitHub page keeps its technical body:
//
//   <!-- app-notes -->
//   Frame generation is now locked to its upscaler.
//   - DLSS-G only with DLSS SR
//   <!-- /app-notes -->
//
// Older releases have no block; their markdown body is stripped down to plain
// text instead of being shown raw.
std::string ExtractAppNotes(const std::string& body) {
    std::string text = body;
    const std::string open = "<!-- app-notes -->";
    const std::string close = "<!-- /app-notes -->";
    const std::size_t start = body.find(open);
    if (start != std::string::npos) {
        const std::size_t from = start + open.size();
        const std::size_t end = body.find(close, from);
        text = body.substr(from, end == std::string::npos ? std::string::npos : end - from);
    } else {
        for (;;) {
            const std::size_t comment = text.find("<!--");
            if (comment == std::string::npos) {
                break;
            }
            const std::size_t comment_end = text.find("-->", comment + 4);
            text.erase(comment, (comment_end == std::string::npos ? text.size()
                                                                 : comment_end + 3) -
                                    comment);
        }
    }

    std::string out;
    out.reserve(text.size());
    bool last_blank = true;
    std::size_t i = 0;
    while (i <= text.size()) {
        const std::size_t nl = text.find('\n', i);
        std::string line =
            text.substr(i, nl == std::string::npos ? std::string::npos : nl - i);
        line = CleanNotesLine(std::move(line));
        if (line.empty()) {
            if (!last_blank && !out.empty()) {
                out.push_back('\n');
            }
            last_blank = true;
        } else {
            out += line;
            out.push_back('\n');
            last_blank = false;
        }
        if (nl == std::string::npos) {
            break;
        }
        i = nl + 1;
    }
    while (!out.empty() && out.back() == '\n') {
        out.pop_back();
    }
    constexpr std::size_t kLimit = 4000;
    if (out.size() > kLimit) {
        const std::size_t cut = out.rfind('\n', kLimit);
        out.resize(cut == std::string::npos ? kLimit : cut);
        out += "\n...";
    }
    return out;
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
    for (const Json& release : root.items) {
        if (release.type != Json::Type::Object) {
            continue;
        }
        if (release.Bool("draft", false)) {
            continue;
        }
        const bool pre = release.Bool("prerelease", false);
        if (!include_prerelease && pre) {
            continue;
        }
        const std::string tag = release.String("tag_name");
        if (!include_prerelease && tag.find('-') != std::string::npos) {
            // A prerelease tag must never reach a stable user even if the
            // GitHub flag was forgotten.
            continue;
        }
        if (tag.empty() || CompareVersions(tag, current) <= 0) {
            continue;
        }
        // On the beta channel the newest version wins, whether it is a beta or
        // a stable. On stable, prereleases were filtered above.
        if (!best || CompareVersions(tag, best->String("tag_name")) > 0) {
            best = &release;
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
    status_.notes = ExtractAppNotes(best->String("body"));
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
    // The updater keeps a log in %TEMP%; write the hand-off there too so a
    // failed self-update can be explained without guessing.
    std::string asset_narrow;
    {
        const int size = ::WideCharToMultiByte(CP_UTF8, 0, Widen(asset).c_str(), -1, nullptr,
                                               0, nullptr, nullptr);
        if (size > 1) {
            asset_narrow.resize(static_cast<std::size_t>(size - 1));
            ::WideCharToMultiByte(CP_UTF8, 0, Widen(asset).c_str(), -1, asset_narrow.data(),
                                  size, nullptr, nullptr);
        }
    }
    {
        wchar_t temp[MAX_PATH] = {};
        ::GetTempPathW(MAX_PATH, temp);
        std::ofstream log(std::filesystem::path(temp) / "veyla-update.log",
                          std::ios::out | std::ios::app);
        if (log) {
            log << "engine: install requested, asset=" << asset_narrow << "\n";
        }
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
