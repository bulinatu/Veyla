#include "session/profile.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace brp::session {
namespace {

// --- tiny JSON helpers -----------------------------------------------------
// The files we read are flat, machine-generated objects. A full JSON parser
// would be more code than the feature, so this handles exactly what is needed:
// quoted strings, numbers, and one level of nested object for a single value.

bool SkipWs(const std::string& json, size_t& pos) {
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) {
        ++pos;
    }
    return pos < json.size();
}

bool FindKey(const std::string& json, const std::string& key, size_t& pos) {
    const std::string needle = "\"" + key + "\"";
    pos = json.find(needle);
    if (pos == std::string::npos) {
        return false;
    }
    pos += needle.size();
    if (!SkipWs(json, pos) || json[pos] != ':') {
        return false;
    }
    ++pos;
    return SkipWs(json, pos);
}

bool ReadStringAt(const std::string& json, size_t pos, std::string& out) {
    if (pos >= json.size() || json[pos] != '"') {
        return false;
    }
    ++pos;
    out.clear();
    while (pos < json.size()) {
        const char c = json[pos++];
        if (c == '"') {
            return true;
        }
        if (c == '\\' && pos < json.size()) {
            const char e = json[pos++];
            switch (e) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'u':
                    // Good enough for paths/names: keep a placeholder.
                    if (pos + 4 <= json.size()) {
                        pos += 4;
                    }
                    out += '?';
                    break;
                default: out += e; break;
            }
        } else {
            out += c;
        }
    }
    return false;
}

bool ExtractString(const std::string& json, const std::string& key, std::string& out) {
    size_t pos = 0;
    return FindKey(json, key, pos) && ReadStringAt(json, pos, out);
}

bool ExtractNumber(const std::string& json, const std::string& key, long long& out) {
    size_t pos = 0;
    if (!FindKey(json, key, pos)) {
        return false;
    }
    char* end = nullptr;
    const long long value = std::strtoll(json.c_str() + pos, &end, 10);
    if (end == json.c_str() + pos) {
        // Might be encoded as a string.
        std::string text;
        if (ReadStringAt(json, pos, text)) {
            out = std::strtoll(text.c_str(), nullptr, 10);
            return true;
        }
        return false;
    }
    out = value;
    return true;
}

// For {"savedIpAddresses":{"PS5_x":"1.2.3.4"}} — first value inside the object.
bool ExtractFirstNestedString(const std::string& json, const std::string& anchor,
                              std::string& out) {
    size_t pos = 0;
    if (!FindKey(json, anchor, pos) || json[pos] != '{') {
        return false;
    }
    const size_t colon = json.find(':', pos);
    if (colon == std::string::npos) {
        return false;
    }
    const size_t quote = json.find('"', colon);
    if (quote == std::string::npos) {
        return false;
    }
    return ReadStringAt(json, quote, out);
}

bool ExtractBool(const std::string& json, const std::string& key, bool& out) {
    size_t pos = 0;
    if (!FindKey(json, key, pos)) {
        return false;
    }
    out = json.compare(pos, 4, "true") == 0;
    return true;
}
std::string EscapeJson(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (char c : value) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c; break;
        }
    }
    return out;
}

std::string ReadFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// --- hex -------------------------------------------------------------------

const char* kHex = "0123456789abcdef";

std::string ToHex(const uint8_t* data, size_t size) {
    std::string out;
    out.resize(size * 2);
    for (size_t i = 0; i < size; ++i) {
        out[i * 2] = kHex[data[i] >> 4];
        out[i * 2 + 1] = kHex[data[i] & 0x0F];
    }
    return out;
}

int HexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool FromHex(const std::string& text, uint8_t* out, size_t size) {
    if (text.size() != size * 2) {
        return false;
    }
    for (size_t i = 0; i < size; ++i) {
        const int hi = HexValue(text[i * 2]);
        const int lo = HexValue(text[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}

std::string EnvOrEmpty(const char* name) {
    const char* value = std::getenv(name);
    return value ? std::string(value) : std::string();
}

}  // namespace

bool Credentials::present() const {
    for (uint8_t byte : rp_key) {
        if (byte != 0) {
            return true;
        }
    }
    return false;
}

std::string Profile::display_name() const {
    if (!name.empty()) {
        return name;
    }
    if (!target_id.empty()) {
        return target_id;
    }
    return "Console";
}

std::string Profile::status_line() const {
    if (!has_console()) {
        return "Console offline";
    }
    return display_name() + (needs_pairing() ? " · needs pairing" : " · ready");
}

std::string AccountIdBase64(uint64_t numeric) {
    uint8_t bytes[8];
    for (int i = 0; i < 8; ++i) {
        bytes[i] = static_cast<uint8_t>(numeric & 0xFFu);
        numeric >>= 8;
    }

    static const char* table =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(12);
    for (size_t i = 0; i < sizeof(bytes); i += 3) {
        uint32_t chunk = 0;
        int count = 0;
        for (int j = 0; j < 3 && i + static_cast<size_t>(j) < sizeof(bytes); ++j) {
            chunk |= static_cast<uint32_t>(bytes[i + j]) << (16 - 8 * j);
            ++count;
        }
        out += table[(chunk >> 18) & 63];
        out += table[(chunk >> 12) & 63];
        out += count > 1 ? table[(chunk >> 6) & 63] : '=';
        out += count > 2 ? table[chunk & 63] : '=';
    }
    return out;
}

bool ImportPXPlay(const std::string& roaming_appdata, Profile& out, std::string& detail) {
    if (roaming_appdata.empty()) {
        detail = "APPDATA is not set";
        return false;
    }

    const std::filesystem::path root(roaming_appdata);
    const std::filesystem::path prefs_path = root / "PXPlay" / "Data" / "prefs.json";
    if (!std::filesystem::exists(prefs_path)) {
        detail = "PXPlay installation not found";
        return false;
    }

    const std::string prefs = ReadFile(prefs_path.string());
    if (prefs.empty()) {
        detail = "PXPlay prefs.json is empty";
        return false;
    }

    bool found_any = false;

    long long account_numeric = 0;
    if (ExtractNumber(prefs, "SELECTED_PSN_PROFILE_ACCOUNT_ID", account_numeric) &&
        account_numeric > 0) {
        out.psn_numeric_id = std::to_string(account_numeric);
        out.psn_account_id =
            AccountIdBase64(static_cast<uint64_t>(account_numeric));
        found_any = true;
    }

    std::string cached_ip;
    // Stored as a JSON-encoded string inside the value, e.g.
    // "cached_direct_ip_addresses": "{\"savedIpAddresses\":{\"PS5_x\":\"1.2.3.4\"}}"
    std::string cached_blob;
    if (ExtractString(prefs, "cached_direct_ip_addresses", cached_blob) && !cached_blob.empty()) {
        ExtractFirstNestedString(cached_blob, "savedIpAddresses", cached_ip);
    }
    if (cached_ip.empty()) {
        ExtractFirstNestedString(prefs, "savedIpAddresses", cached_ip);
    }
    if (!cached_ip.empty()) {
        out.host = cached_ip;
        found_any = true;
    }

    // Console list: an integration-facing export, plaintext by design.
    const std::filesystem::path consoles_path = root / "PXPlay" / "integration" / "consoles.json";
    const std::string consoles = ReadFile(consoles_path.string());
    if (!consoles.empty()) {
        std::string value;
        if (ExtractString(consoles, "consoleId", value)) {
            out.target_id = value;
            found_any = true;
        }
        if (ExtractString(consoles, "displayName", value)) {
            out.name = value;
        } else if (ExtractString(consoles, "nickname", value)) {
            out.name = value;
        }
        if (ExtractString(consoles, "deviceType", value)) {
            out.device_type = value;
        }
    }

    if (!found_any) {
        detail = "PXPlay files present but no usable fields";
        return false;
    }

    detail = "Imported console identity and PSN account from PXPlay";
    return true;
}

std::string DefaultProfilePath() {
    const std::string appdata = EnvOrEmpty("APPDATA");
    if (appdata.empty()) {
        return "session.json";
    }
    return (std::filesystem::path(appdata) / "BetterRemotePlay" / "session.json").string();
}

bool Save(const Profile& profile, const std::string& path, std::string& detail) {
    namespace fs = std::filesystem;

    std::error_code ec;
    const fs::path parent = fs::path(path).parent_path();
    if (!parent.empty()) {
        fs::create_directories(parent, ec);
        if (ec) {
            detail = "cannot create " + parent.string() + ": " + ec.message();
            return false;
        }
    }

    std::ostringstream json;
    json << "{\n";
    json << "  \"name\": \"" << EscapeJson(profile.name) << "\",\n";
    json << "  \"device_type\": \"" << EscapeJson(profile.device_type) << "\",\n";
    json << "  \"target_id\": \"" << EscapeJson(profile.target_id) << "\",\n";
    json << "  \"host\": \"" << EscapeJson(profile.host) << "\",\n";
    json << "  \"psn_numeric_id\": \"" << EscapeJson(profile.psn_numeric_id) << "\",\n";
    json << "  \"psn_account_id\": \"" << EscapeJson(profile.psn_account_id) << "\",\n";
    json << "  \"rp_key\": \""
         << ToHex(profile.credentials.rp_key.data(), profile.credentials.rp_key.size())
         << "\",\n";
    json << "  \"rp_regist_key\": \""
         << ToHex(profile.credentials.rp_regist_key.data(),
                  profile.credentials.rp_regist_key.size())
         << "\",\n";
    json << "  \"rp_key_type\": " << profile.credentials.rp_key_type << ",\n";
    json << "  \"console_pin\": " << profile.credentials.console_pin << ",\n";
    json << "  \"width\": " << profile.width << ",\n";
    json << "  \"height\": " << profile.height << ",\n";
    json << "  \"fps\": " << profile.fps << ",\n";
    json << "  \"bitrate_kbps\": " << profile.bitrate_kbps << ",\n";
    json << "  \"codec\": \"" << EscapeJson(profile.codec) << "\",\n";
    json << "  \"hdr\": " << (profile.hdr ? "true" : "false") << ",\n";
    json << "  \"upscaler\": " << profile.upscaler << ",\n";
    json << "  \"render_target\": " << profile.render_target << ",\n";
    json << "  \"hud\": " << (profile.hud ? "true" : "false") << ",\n";
    json << "  \"audio_device_id\": \"" << EscapeJson(profile.audio_device_id) << "\",\n";
    json << "  \"audio_buffer_ms\": " << profile.audio_buffer_ms << ",\n";
    json << "  \"input_source\": " << profile.input_source << ",\n";
    json << "  \"frame_buffer\": " << profile.frame_buffer << ",\n";
    json << "  \"pad_rate_hz\": " << profile.pad_rate_hz << ",\n";
    json << "  \"frame_gen\": " << profile.frame_gen << ",\n";
    json << "  \"vsr\": " << profile.vsr << ",\n";
    json << "  \"neural_sr\": " << profile.neural_sr << ",\n";
    json << "  \"dlss_preset\": " << profile.dlss_preset << ",\n";
    json << "  \"dlss_quality\": " << profile.dlss_quality << ",\n";
    json << "  \"decoder\": " << profile.decoder << ",\n";
    json << "  \"chroma\": " << profile.chroma << ",\n";
    json << "  \"lightbar\": " << profile.lightbar << ",\n";
    json << "  \"trigger_effect\": " << profile.trigger_effect << ",\n";
    json << "  \"trigger_strength\": " << profile.trigger_strength << ",\n";
    json << "  \"player_leds\": " << profile.player_leds << ",\n";
    json << "  \"mic_led\": " << profile.mic_led << ",\n";
    json << "  \"console_rumble\": " << profile.console_rumble << ",\n";
    json << "  \"console_triggers\": " << profile.console_triggers << ",\n";
    json << "  \"motion\": " << profile.motion << "\n";
    json << "}\n";

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        detail = "cannot write " + path;
        return false;
    }
    file << json.str();
    detail = path;
    return true;
}

bool Load(const std::string& path, Profile& profile, std::string& detail) {
    const std::string json = ReadFile(path);
    if (json.empty()) {
        detail = "no session file";
        return false;
    }

    Profile p;
    ExtractString(json, "name", p.name);
    ExtractString(json, "device_type", p.device_type);
    ExtractString(json, "target_id", p.target_id);
    ExtractString(json, "host", p.host);
    ExtractString(json, "psn_numeric_id", p.psn_numeric_id);
    ExtractString(json, "psn_account_id", p.psn_account_id);

    std::string hex;
    if (ExtractString(json, "rp_key", hex)) {
        FromHex(hex, p.credentials.rp_key.data(), p.credentials.rp_key.size());
    }
    if (ExtractString(json, "rp_regist_key", hex)) {
        FromHex(hex, p.credentials.rp_regist_key.data(), p.credentials.rp_regist_key.size());
    }

    long long number = 0;
    if (ExtractNumber(json, "rp_key_type", number)) {
        p.credentials.rp_key_type = static_cast<uint32_t>(number);
    }
    if (ExtractNumber(json, "console_pin", number)) {
        p.credentials.console_pin = static_cast<uint32_t>(number);
    }
    if (ExtractNumber(json, "width", number)) p.width = static_cast<int>(number);
    if (ExtractNumber(json, "height", number)) p.height = static_cast<int>(number);
    if (ExtractNumber(json, "fps", number)) p.fps = static_cast<int>(number);
    if (ExtractNumber(json, "bitrate_kbps", number)) p.bitrate_kbps = static_cast<int>(number);

    std::string text;
    if (ExtractString(json, "codec", text)) p.codec = text;
    if (ExtractString(json, "audio_device_id", text)) p.audio_device_id = text;
    if (ExtractNumber(json, "upscaler", number)) p.upscaler = static_cast<int>(number);
    if (ExtractNumber(json, "render_target", number)) p.render_target = static_cast<int>(number);
    if (ExtractNumber(json, "audio_buffer_ms", number)) p.audio_buffer_ms = static_cast<int>(number);
    if (ExtractNumber(json, "input_source", number)) p.input_source = static_cast<int>(number);
    if (ExtractNumber(json, "frame_buffer", number)) p.frame_buffer = static_cast<int>(number);
    if (ExtractNumber(json, "pad_rate_hz", number)) p.pad_rate_hz = static_cast<int>(number);
    if (ExtractNumber(json, "frame_gen", number)) p.frame_gen = static_cast<int>(number);
    if (ExtractNumber(json, "vsr", number)) p.vsr = static_cast<int>(number);
    if (ExtractNumber(json, "neural_sr", number)) p.neural_sr = static_cast<int>(number);
    if (ExtractNumber(json, "dlss_preset", number)) p.dlss_preset = static_cast<int>(number);
    if (ExtractNumber(json, "dlss_quality", number)) p.dlss_quality = static_cast<int>(number);
    if (ExtractNumber(json, "decoder", number)) p.decoder = static_cast<int>(number);
    if (ExtractNumber(json, "chroma", number)) p.chroma = static_cast<int>(number);
    if (ExtractNumber(json, "lightbar", number)) p.lightbar = static_cast<int>(number);
    if (ExtractNumber(json, "trigger_effect", number)) p.trigger_effect = static_cast<int>(number);
    if (ExtractNumber(json, "trigger_strength", number)) p.trigger_strength = static_cast<int>(number);
    if (ExtractNumber(json, "player_leds", number)) p.player_leds = static_cast<int>(number);
    if (ExtractNumber(json, "mic_led", number)) p.mic_led = static_cast<int>(number);
    if (ExtractNumber(json, "console_rumble", number)) p.console_rumble = static_cast<int>(number);
    if (ExtractNumber(json, "console_triggers", number)) p.console_triggers = static_cast<int>(number);
    if (ExtractNumber(json, "motion", number)) p.motion = static_cast<int>(number);
    ExtractBool(json, "hud", p.hud);

    profile = std::move(p);
    detail = path;
    return true;
}

}  // namespace brp::session
