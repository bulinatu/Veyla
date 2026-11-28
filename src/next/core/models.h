#pragma once

#include "core/types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace brp::next {

// ---------------------------------------------------------------------------
// Settings model. One owner per concern; no screen reads raw keys.
// ---------------------------------------------------------------------------

struct GeneralSettings {
    Theme theme = Theme::Dark;
    bool start_minimized = false;
    bool close_to_tray = false;
    bool notifications = true;
    bool check_updates = true;
    int update_channel = 0;  // 0 stable, 1 beta
    std::string update_repo = "bulinatu/Veyla";  // GitHub owner/name for releases
    bool telemetry = false;  // always local-only in this product
};

struct ConsoleSettings {
    std::string name;
    std::string address;
    int fps = 60;
    bool auto_discover = true;
    bool auto_reconnect = true;
    int reconnect_delay_ms = 1500;
};

struct StreamingSettings {
    Resolution input = Resolution::Native;
    int fps = 60;
    std::string codec = "auto";
    std::string decoder = "auto";
    int bitrate_kbps = 100000;
    int frame_buffer = 1;
    bool hdr = false;
};

struct VideoSettings {
    Resolution output = Resolution::Native;
    bool vsync = true;
    bool vrr = true;
    bool hdr = false;
    std::string color_space = "auto";
    int sharpening = 10;
    int reflex = 0;  // 0 off, 1 markers, 2 markers + boost
    int render_target = 0;  // 0 window, 1080/1440/2160
};

struct AiSettings {
    bool super_resolution = false;
    Resolution sr_output = Resolution::Native;
    std::string sr_model = "auto";
    int sr_quality = 4;  // 0 performance .. 5 dlaa
    int sr_preset = 0;   // 0 auto, 1 K, 2 L, 3 M, 4 J (DLSS model)
    // AMD FSR upscaling: 0 native AA, 1 quality, 2 balanced, 3 performance,
    // 4 ultra performance. Separate from the DLSS ladder because the names
    // and ratios do not line up.
    int fsr_quality = 2;
    int fsr_sharpness = 30;  // 0..100, RCAS strength
    bool temporal = false;
    bool frame_generation = false;
    AiBackend fg_backend = AiBackend::None;
    int fg_multiplier = 2;  // DLSS-G only: 2x .. 6x
    int quality = 1;  // QualityPreset index
    bool occlusion = true;
    bool adaptive = true;
    // DLSS Neural Rendering (NGX feature 18) through the NeuralScreen worker.
    // A video has no 3D guides, so the worker runs with synthetic depth/motion
    // and resets history each frame: a real but subtle video enhancer.
    bool neural_render = false;
    int neural_style = 2;        // 0 default, 1 natural, 2 cinematic
    int neural_intensity = 100;  // 0..200 %
    int neural_tone = 100;       // 0..200 %, local tone
    int neural_structure = 50;   // 0..200 %, local structure
    int neural_skin = 75;        // 0..200 %, skin structure
    int neural_passes = 1;       // 1..20 cascade depth
    int neural_scale = 100;      // 50..100 % network work size (up to 9999 unlocked)
    bool neural_unlock = false;  // lift every neural limit to 9999 (use at your own risk)
    bool neural_ui = false;
    bool neural_mask = false;

    bool SrIsFsr() const { return sr_model == "fsr4" || sr_model == "fsr3"; }
    bool SrIsDlss() const { return sr_model == "dlss"; }
    // Auto follows the adapter: non-NVIDIA is the FSR family.
    bool SrWantsFsr(bool nvidia) const {
        return SrIsFsr() || (sr_model == "auto" && !nvidia);
    }
    // Frame generation runs only on its own upscaler: DLSS-G with DLSS SR,
    // FSR-G with FSR SR. The pair is never mixed.
    bool FgMatchesSr(bool nvidia) const {
        switch (fg_backend) {
            case AiBackend::None: return true;
            case AiBackend::DlssFg: return !SrWantsFsr(nvidia);
            case AiBackend::FsrFg: return SrWantsFsr(nvidia);
            default: return false;
        }
    }
};

struct GpuSettings {
    int device = 0;
    bool fp16 = true;
    bool int8 = false;
    int frame_buffers = 2;
    int memory_limit_mb = 0;  // 0 = driver budget
    int priority = 0;         // 0 normal, 1 above normal
};

struct DisplaySettings {
    int monitor = 0;
    FrameRate refresh = FrameRate::Match;
    bool vsync = true;
    bool vrr = true;
    bool fullscreen_on_play = true;
    std::string output_format = "auto";
    int bit_depth = 8;
};

struct OverlaySettings {
    bool enabled = false;
    bool compact = false;
    float opacity = 0.85f;
    int position = 1;     // 0 top, 1 middle, 2 bottom (vertical)
    int side = 1;         // 0 left, 1 right
    int scale = 125;      // 75..200 percent
    int interval_ms = 250;
    bool show_performance = true;
    bool show_gpu = true;
    bool show_stream = true;
    bool show_processing = true;
    std::string hotkey = "F10";
};

struct AudioSettings {
    std::string device = "default";
    int buffer_ms = 20;
    int volume = 100;
    bool mute = false;
};

struct ControllerSettings {
    bool enabled = true;
    bool haptics = true;
    bool adaptive_triggers = true;
    int rumble_strength = 5;  // 0..5
    int poll_hz = 250;        // 125..1000
    int lightbar = 0;
};

struct AdvancedSettings {
    bool experimental = false;
    bool debug_log = false;
    LogLevel log_level = LogLevel::Info;
    bool prefer_gpu_present = true;
};

struct AppSettings {
    int schema = 1;
    GeneralSettings general;
    ConsoleSettings console;
    StreamingSettings streaming;
    VideoSettings video;
    AiSettings ai;
    GpuSettings gpu;
    DisplaySettings display;
    OverlaySettings overlay;
    AudioSettings audio;
    ControllerSettings controller;
    AdvancedSettings advanced;

    // Clamps ranges and reports combinations that cannot work. Warnings are
    // non-fatal; errors mean the configuration must not be applied as-is.
    void Validate(std::vector<std::string>& warnings,
                  std::vector<std::string>& errors) const;
};

// ---------------------------------------------------------------------------
// Profiles
// ---------------------------------------------------------------------------

struct Profile {
    std::string id;
    std::string name;
    bool is_default = false;
    std::uint64_t created_at = 0;
    std::uint64_t updated_at = 0;
    AppSettings settings;
};

// ---------------------------------------------------------------------------
// Read-only snapshots. Services publish these; UI never reads live internals.
// ---------------------------------------------------------------------------

struct ConsoleCandidate {
    std::string address;
    std::string name;
    std::string target_id;
    std::string device_type;
    std::string status;
};

struct StreamSnapshot {
    bool connected = false;
    bool streaming = false;
    bool connecting = false;
    bool pairing = false;
    bool discovering = false;
    bool has_credentials = false;
    std::string console_name;
    std::string console_address;
    std::string target_id;
    std::string state;  // idle, connecting, streaming, failed
    std::string message;
    std::string pairing_message;
    std::string codec;
    std::string decoder;
    int source_width = 0;
    int source_height = 0;
    double source_fps = 0.0;
    double bitrate_kbps = 0.0;
    double loss_percent = 0.0;
    double jitter_ms = 0.0;
    int decoder_drops = 0;
    double decode_ms = 0.0;
    double decode_ms_max = 0.0;
};

struct GpuSnapshot {
    bool present = false;
    bool nvdec = false;
    bool optical_flow = false;
    std::string name;
    std::string driver;
    std::uint64_t vram_mb = 0;
    std::uint64_t vram_used_mb = 0;
    double utilization = 0.0;
    double temperature_c = 0.0;
    double power_w = 0.0;
};

struct AiCapabilities {
    AiBackend backend = AiBackend::None;
    bool available = false;
    std::string reason;
    int max_multiplier = 0;
    bool fp16 = false;
    bool int8 = false;
    bool temporal = false;
    double estimated_ms = 0.0;
    std::vector<Resolution> outputs;
};

struct AiSnapshot {
    bool sr_active = false;
    bool fg_active = false;
    std::string sr_model;
    std::string fg_backend;
    int input_fps = 0;
    int output_fps = 0;
    int multiplier = 0;
    double sr_ms = 0.0;
    double fg_ms = 0.0;
    double present_ms = 0.0;
    std::uint64_t generated = 0;
    std::uint64_t dropped = 0;
};

struct DiagnosticsSnapshot {
    double decode_ms = 0.0;
    double sr_ms = 0.0;
    double fg_ms = 0.0;
    double present_ms = 0.0;
    double frame_p50_ms = 0.0;
    double frame_p95_ms = 0.0;
    double frame_p99_ms = 0.0;
    std::uint64_t late_frames = 0;
    std::uint64_t dropped_frames = 0;
    std::uint64_t generated_frames = 0;
};

struct Notification {
    NotificationKind kind = NotificationKind::Info;
    std::string title;
    std::string message;
    std::uint64_t created_at = 0;
    bool persistent = false;
};

struct HardwareCapabilities {
    bool nvidia = false;
    bool nvdec = false;
    bool optical_flow = false;
    bool hdr = false;
    int max_refresh_hz = 60;
    int vram_mb = 0;
    std::string gpu_name;
    std::string driver;
};

}  // namespace brp::next
