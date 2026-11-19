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

    // Display enhance: sharpening always available, colour controls opt-in.
    // Neutral values are the identity; old profiles load neutral.
    int sharpness = 35;           // CAS amount 0..100
    int color_on = 0;             // 0 = colour controls bypassed
    int color_exposure = 0;       // -100..100 -> +-1 stop
    int color_contrast = 100;     // 50..150, 18% grey pivot
    int color_highlights = 0;     // -100..100
    int color_shadows = 0;        // -100..100
    int color_saturation = 100;   // 0..200
    int color_vibrance = 0;       // 0..100
    int color_hue = 0;            // -180..180 degrees
    int color_temperature = 0;    // -100..100 (cool..warm)
    int color_tint = 0;           // -100..100 (green..magenta)
    int color_gamma = 100;        // 80..120

    // UI settings
    int upscaler = 1;
    int render_target = 0;  // 0 window/display, 1 1080p, 2 1440p, 3 2160p
    bool hud = true;
    std::string audio_device_id;
    int audio_buffer_ms = 20;
    int input_source = 0;
    int frame_buffer = 1;
    int pad_rate_hz = 250;
    int frame_gen = 0;  // 0 off, 2/3/4 NVIDIA VFG multiplier
    int neural_sr = 0;  // 1 = DLSS Super Resolution (D3D12), exclusive with VFG
    int dlss_preset = 11;  // 11 K, 12 L, 13 M
    int dlss_mode = 4;     // 0 UP 33%, 1 Perf 50%, 2 Bal 58%, 3 Qual 66%, 4 UQ 77%, 5 DLAA
    int nr_order = 1;      // 0 NR before SR (upstream), 1 NR after SR (NVIDIA order)
    int dlss_fg = 0;       // 0 off, 2 = on; the multiplier is chosen live from content and target
    int fg_target = 0;     // 0 match display, else 60 / 90 / 120 output Hz
    int fg_engine = 0;     // 0 NVIDIA (DLSS FG through the worker), 1 RIFE (TensorRT)
    int rife_output = 60;  // RIFE: 60 = one generated frame per pair, 0 = pass-through
    int rife_model = 0;    // 0 = Lite 4.25 (fast), 1 = Heavy 4.26 (quality)
    int reflex = 0;        // 0 off, 1 on, 2 on + boost (NVAPI low-latency)
    int hud_mask = 31;     // debug HUD sections: frames/gpu/pad/stream/net
    int neural_nr = 0;     // DLSSNR on/off (NGX worker at stream size)
    int nr_passes = 1;     // cascade depth, 1..4, independent of the scale
    int nr_scale = 100;    // NGX work scale percent, 50..100 (slider)
    int nr_style = 2;      // 0 default, 1 natural, 2 cinematic
    int nr_tone = 100;     // percent 0..200
    int nr_structure = 100;
    int nr_skin = 75;
    int nr_mask = 0;
    int nr_intensity = 100;
    int nr_ui = 0;
    int decoder = 0;    // 0 auto, 1 d3d11va, 2 d3d12va, 3 vulkan, 4 dxva2, 5 cuda, 6 software
    int chroma = 1;     // 0 bilinear, 1 bicubic, 2 lanczos
    int lightbar = 0;
    int trigger_effect = 0;
    int trigger_strength = 5;
    int player_leds = 0;
    int mic_led = 0;
    int console_rumble = 1;
    int console_triggers = 1;
    int haptic_intensity = 3;  // 0 off, 1 very weak, 2 weak, 3 normal, 4 strong, 5 very strong
    int testing_log = 0;       // 1 = write testing.log every second
    int rife_blend = 10;       // percent of the previous real frame mixed into generated ones
    int rife_safe_motion = 1;  // skip interpolation on very large frame changes
    int motion = 1;

    bool has_console() const { return !target_id.empty() || !host.empty(); }
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
