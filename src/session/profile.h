#pragma once

#include <array>
#include <cstdint>
#include <string>

// Session = everything needed to talk to one console.
//
// Fields are split by confidentiality because the sources differ:
//   * metadata (name, console id, account id, last address) is plaintext and can
//     be imported from other clients,
//   * credentials (rp_key, rp_regist_key) are secrets produced by registration
//     and are never recoverable from an app that encrypts them.
namespace brp::session {

struct Credentials {
    std::array<uint8_t, 16> rp_key{};
    std::array<uint8_t, 16> rp_regist_key{};
    uint32_t rp_key_type = 0;
    uint32_t console_pin = 0;

    bool present() const;
};

struct Profile {
    // identity
    std::string name;          // "PS5-580"
    std::string device_type;   // "PS5" / "PS4"
    std::string target_id;     // console unique id, e.g. "2840ddf38ed9"
    std::string host;          // last known address

    // PSN
    std::string psn_numeric_id;  // decimal, as stored by PXPlay
    std::string psn_account_id;  // base64 of the 8-byte account id (what pairing uses)

    // registration
    Credentials credentials;

    // stream defaults
    int width = 1920;
    int height = 1080;
    int fps = 60;
    int bitrate_kbps = 50000;
    std::string codec = "hevc";
    bool hdr = false;

    // UI settings
    int upscaler = 1;
    int render_target = 0;  // 0 window/display, 1 1080p, 2 1440p, 3 2160p
    bool hud = true;
    std::string audio_device_id;
    int audio_buffer_ms = 20;
    int input_source = 0;
    int frame_buffer = 1;
    int pad_rate_hz = 250;
    int lightbar = 0;
    int trigger_effect = 0;
    int trigger_strength = 5;
    int player_leds = 0;
    int mic_led = 0;
    int console_rumble = 1;
    int console_triggers = 1;
    int motion = 1;

    bool has_console() const { return !target_id.empty(); }
    bool needs_pairing() const { return !credentials.present(); }
    std::string display_name() const;
    std::string status_line() const;
};

// Base64 of the 8-byte big-endian PSN account id. This is the value the
// registration handshake expects, not the decimal number users see.
std::string AccountIdBase64(uint64_t numeric);

// Imports the plaintext parts of a PXPlay installation: console identity, PSN
// account id, cached address. Deliberately does NOT touch secure.json, whose
// contents are machine-bound ciphertext.
bool ImportPXPlay(const std::string& roaming_appdata, Profile& out, std::string& detail);

std::string DefaultProfilePath();
bool Save(const Profile& profile, const std::string& path, std::string& detail);
bool Load(const std::string& path, Profile& profile, std::string& detail);

}  // namespace brp::session
