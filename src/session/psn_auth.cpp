#include "session/psn_auth.h"

#include "session/profile.h"

#include <windows.h>
#include <bcrypt.h>
#include <shellapi.h>
#include <winhttp.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <utility>

namespace brp::session {
namespace {

constexpr char kClientId[] = "ba495a24-818c-472b-b12d-ff231c1b5745";
constexpr char kClientSecret[] = "mvaiZkRsAsI1IBkY";
// Public Remote Play OAuth client constants, as used by chiaki-ng. They are
// application identifiers, not user credentials, and are required for PSN
// sign-in interoperability.
constexpr wchar_t kAuthHost[] = L"auth.api.sonyentertainmentnetwork.com";
constexpr char kRedirectUri[] = "https://remoteplay.dl.playstation.net/remoteplay/redirect";
constexpr char kScope[] =
    "psn:clientapp referenceDataService:countryConfig.read "
    "pushNotification:webSocket.desktop.connect "
    "sessionManager:remotePlaySession.system.update";

std::string Trim(std::string text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

std::wstring Widen(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int size = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                                           static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(size), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                          out.data(), size);
    return out;
}

std::string UrlEncode(const std::string& text) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(text.size() * 3);
    for (unsigned char c : text) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0x0F]);
        }
    }
    return out;
}

int HexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string UrlDecode(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size()) {
            const int hi = HexDigit(text[i + 1]);
            const int lo = HexDigit(text[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(text[i]);
    }
    return out;
}

std::string Base64(const std::string& bytes) {
    static const char* table =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    for (size_t i = 0; i < bytes.size(); i += 3) {
        uint32_t chunk = 0;
        int count = 0;
        for (int j = 0; j < 3 && i + static_cast<size_t>(j) < bytes.size(); ++j) {
            chunk |= static_cast<uint32_t>(static_cast<unsigned char>(bytes[i + j]))
                     << (16 - 8 * j);
            ++count;
        }
        out.push_back(table[(chunk >> 18) & 63]);
        out.push_back(table[(chunk >> 12) & 63]);
        out.push_back(count > 1 ? table[(chunk >> 6) & 63] : '=');
        out.push_back(count > 2 ? table[chunk & 63] : '=');
    }
    return out;
}

std::string BasicAuth() {
    return Base64(std::string(kClientId) + ":" + kClientSecret);
}

std::string RandomDuid() {
    uint8_t bytes[16] = {};
    if (BCryptGenRandom(nullptr, bytes, sizeof(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        std::random_device rd;
        for (uint8_t& byte : bytes) {
            byte = static_cast<uint8_t>(rd() & 0xFF);
        }
    }
    static const char* hex = "0123456789abcdef";
    std::string out = "0000000700410080";
    for (uint8_t byte : bytes) {
        out.push_back(hex[byte >> 4]);
        out.push_back(hex[byte & 0x0F]);
    }
    return out;
}

bool JsonValue(const std::string& json, const char* key, std::string& out) {
    const std::string needle = std::string("\"") + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) {
        return false;
    }
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) {
        return false;
    }
    ++pos;
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) {
        ++pos;
    }
    if (pos >= json.size()) {
        return false;
    }

    if (json[pos] == '"') {
        ++pos;
        std::string value;
        while (pos < json.size() && json[pos] != '"') {
            if (json[pos] == '\\' && pos + 1 < json.size()) {
                ++pos;
            }
            value.push_back(json[pos++]);
        }
        out = std::move(value);
        return true;
    }

    const size_t start = pos;
    while (pos < json.size() &&
           (std::isdigit(static_cast<unsigned char>(json[pos])) || json[pos] == '-' ||
            json[pos] == '+')) {
        ++pos;
    }
    if (pos == start) {
        return false;
    }
    out = json.substr(start, pos - start);
    return true;
}

bool HttpRequest(const wchar_t* method, const std::wstring& path,
                 const std::wstring& headers, const std::string& body, std::string& response,
                 std::string& detail) {
    HINTERNET session = ::WinHttpOpen(L"Veyla/0.3",
                                      WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        detail = "WinHttpOpen failed (" + std::to_string(::GetLastError()) + ")";
        return false;
    }
    ::WinHttpSetTimeouts(session, 15000, 15000, 15000, 30000);

    HINTERNET connect = ::WinHttpConnect(session, kAuthHost, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!connect) {
        detail = "WinHttpConnect failed (" + std::to_string(::GetLastError()) + ")";
        ::WinHttpCloseHandle(session);
        return false;
    }

    HINTERNET request = ::WinHttpOpenRequest(
        connect, method, path.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!request) {
        detail = "WinHttpOpenRequest failed (" + std::to_string(::GetLastError()) + ")";
        ::WinHttpCloseHandle(connect);
        ::WinHttpCloseHandle(session);
        return false;
    }

    DWORD decompress = WINHTTP_DECOMPRESSION_FLAG_GZIP | WINHTTP_DECOMPRESSION_FLAG_DEFLATE;
    ::WinHttpSetOption(request, WINHTTP_OPTION_DECOMPRESSION, &decompress, sizeof(decompress));

    const DWORD header_length = headers.empty() ? 0 : static_cast<DWORD>(-1L);
    const LPVOID body_ptr = body.empty() ? WINHTTP_NO_REQUEST_DATA
                                         : const_cast<char*>(body.data());
    const DWORD body_length = static_cast<DWORD>(body.size());
    BOOL ok = ::WinHttpSendRequest(
        request, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
        header_length, body_ptr, body_length, body_length, 0);
    if (!ok) {
        detail = "WinHttpSendRequest failed (" + std::to_string(::GetLastError()) + ")";
    } else {
        ok = ::WinHttpReceiveResponse(request, nullptr);
        if (!ok) {
            detail = "WinHttpReceiveResponse failed (" + std::to_string(::GetLastError()) + ")";
        }
    }

    DWORD status = 0;
    DWORD status_size = sizeof(status);
    if (ok) {
        ok = ::WinHttpQueryHeaders(request,
                                   WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                   WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                                   WINHTTP_NO_HEADER_INDEX);
        if (!ok) {
            detail = "WinHttpQueryHeaders failed (" + std::to_string(::GetLastError()) + ")";
        }
    }

    response.clear();
    if (ok) {
        for (;;) {
            DWORD available = 0;
            if (!::WinHttpQueryDataAvailable(request, &available)) {
                detail = "WinHttpQueryDataAvailable failed (" +
                         std::to_string(::GetLastError()) + ")";
                ok = FALSE;
                break;
            }
            if (available == 0) {
                break;
            }
            std::string chunk(static_cast<size_t>(available), '\0');
            DWORD read = 0;
            if (!::WinHttpReadData(request, chunk.data(), available, &read)) {
                detail = "WinHttpReadData failed (" + std::to_string(::GetLastError()) + ")";
                ok = FALSE;
                break;
            }
            chunk.resize(read);
            response += chunk;
        }
    }

    ::WinHttpCloseHandle(request);
    ::WinHttpCloseHandle(connect);
    ::WinHttpCloseHandle(session);

    if (!ok) {
        return false;
    }
    if (status != 200) {
        std::string description;
        if (JsonValue(response, "error_description", description) && !description.empty()) {
            if (description == "Bad request") {
                detail = "PSN rejected the link. Reopen the browser and paste a fresh URL.";
            } else {
                detail = "PSN: " + description;
            }
        } else if (JsonValue(response, "error", description) && !description.empty()) {
            detail = "PSN: " + description;
        } else {
            detail = "PSN returned HTTP " + std::to_string(status);
        }
        return false;
    }
    return true;
}

std::string ExtractCode(const std::string& input) {
    std::string text = Trim(input);
    if (text.empty()) {
        return {};
    }

    const size_t pos = text.find("code=");
    if (pos != std::string::npos) {
        size_t begin = pos + 5;
        size_t end = text.find_first_of("&# \t\r\n", begin);
        text = text.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
    }
    return UrlDecode(Trim(text));
}

}  // namespace

std::wstring PsnLoginUrl() {
    std::string url =
        "https://auth.api.sonyentertainmentnetwork.com/2.0/oauth/authorize"
        "?service_entity=urn:service-entity:psn&response_type=code&client_id=";
    url += kClientId;
    url += "&redirect_uri=";
    url += kRedirectUri;
    url += "&scope=psn:clientapp%20referenceDataService:countryConfig.read%20"
           "pushNotification:webSocket.desktop.connect%20"
           "sessionManager:remotePlaySession.system.update"
           "&request_locale=en_US&ui=pr&service_logo=ps&layout_type=popup"
           "&smcid=remoteplay&prompt=always&PlatformPrivacyWs1=minimal&duid=";
    url += RandomDuid();
    url += "&";
    return Widen(url);
}

bool OpenPsnLoginPage(std::string& detail) {
    const std::wstring url = PsnLoginUrl();
    const HINSTANCE result =
        ::ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32) {
        detail = "Could not open the default browser";
        return false;
    }
    detail = "Browser opened. Sign in, then paste the final redirect URL.";
    return true;
}

bool PsnCompleteLogin(const std::string& redirect_url_or_code, PsnAccount& out,
                      std::string& detail) {
    const std::string code = ExtractCode(redirect_url_or_code);
    if (code.empty()) {
        detail = "Paste the URL from the blank redirect page (it contains ?code=...).";
        return false;
    }

    const std::wstring auth_header =
        L"Authorization: Basic " + Widen(BasicAuth()) + L"\r\n";
    const std::string token_body =
        "grant_type=authorization_code&code=" + UrlEncode(code) +
        "&scope=" + UrlEncode(kScope) +
        "&redirect_uri=" + UrlEncode(kRedirectUri) + "&";

    std::string token_response;
    if (!HttpRequest(L"POST", L"/2.0/oauth/token",
                     auth_header + L"Content-Type: application/x-www-form-urlencoded\r\n",
                     token_body, token_response, detail)) {
        return false;
    }

    std::string access_token;
    if (!JsonValue(token_response, "access_token", access_token) || access_token.empty()) {
        detail = "PSN token response did not contain an access token";
        return false;
    }

    const std::wstring info_path = L"/2.0/oauth/token/" + Widen(access_token);
    std::string info_response;
    if (!HttpRequest(L"GET", info_path, auth_header, {}, info_response, detail)) {
        return false;
    }

    std::string user_id;
    if (!JsonValue(info_response, "user_id", user_id) || user_id.empty()) {
        detail = "PSN account response did not contain a user id";
        return false;
    }

    uint64_t numeric = 0;
    try {
        numeric = std::stoull(user_id);
    } catch (...) {
        detail = "PSN returned an invalid account id";
        return false;
    }

    out.account_id = AccountIdBase64(numeric);
    out.numeric_id = std::to_string(numeric);
    detail = "PSN account linked: " + out.numeric_id;
    return true;
}

}  // namespace brp::session
