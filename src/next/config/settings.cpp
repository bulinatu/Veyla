#include "config/settings.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sstream>

namespace brp::next {
namespace {

int ClampInt(long long value, int low, int high) {
    return static_cast<int>(std::clamp<long long>(value, low, high));
}

Theme ReadTheme(const FlatMap& flat, const std::string& key, Theme fallback) {
    const std::string value = FlatJson::GetString(flat, key, "");
    if (value == "dark") return Theme::Dark;
    if (value == "light") return Theme::Light;
    return fallback;
}

Resolution ReadResolution(const FlatMap& flat, const std::string& key, Resolution fallback) {
    const std::string value = FlatJson::GetString(flat, key, "");
    if (value == "native") return Resolution::Native;
    if (value == "1080p") return Resolution::R1080;
    if (value == "1440p") return Resolution::R1440;
    if (value == "2160p") return Resolution::R2160;
    return fallback;
}

FrameRate ReadFrameRate(const FlatMap& flat, const std::string& key, FrameRate fallback) {
    const std::string value = FlatJson::GetString(flat, key, "");
    if (value == "match") return FrameRate::Match;
    if (value == "60") return FrameRate::F60;
    if (value == "90") return FrameRate::F90;
    if (value == "120") return FrameRate::F120;
    if (value == "144") return FrameRate::F144;
    if (value == "165") return FrameRate::F165;
    if (value == "240") return FrameRate::F240;
    return fallback;
}

AiBackend ReadBackend(const FlatMap& flat, const std::string& key, AiBackend fallback) {
    const std::string value = FlatJson::GetString(flat, key, "");
    if (value == "none") return AiBackend::None;
    if (value == "dlss-sr") return AiBackend::DlssSr;
    if (value == "dlss-fg") return AiBackend::DlssFg;
    if (value == "fsr") return AiBackend::Fsr;
    if (value == "fsr-fg") return AiBackend::FsrFg;
    if (value == "nvidia-vfg") return AiBackend::NvidiaVfg;
    return fallback;
}

LogLevel ReadLogLevel(const FlatMap& flat, const std::string& key, LogLevel fallback) {
    const std::string value = FlatJson::GetString(flat, key, "");
    if (value == "trace") return LogLevel::Trace;
    if (value == "debug") return LogLevel::Debug;
    if (value == "info") return LogLevel::Info;
    if (value == "warning") return LogLevel::Warning;
    if (value == "error") return LogLevel::Error;
    return fallback;
}

Resolution ResolutionFromLegacySize(int width, int height) {
    if (width >= 3840 || height >= 2160) return Resolution::R2160;
    if (width >= 2560 || height >= 1440) return Resolution::R1440;
    if (width >= 1920 || height >= 1080) return Resolution::R1080;
    return Resolution::Native;
}

std::string DecoderName(long long index) {
    switch (index) {
        case 1: return "d3d11va";
        case 2: return "d3d12va";
        case 3: return "vulkan";
        case 4: return "dxva2";
        case 5: return "cuda";
        case 6: return "software";
        default: return "auto";
    }
}

}  // namespace

void AppSettings::Validate(std::vector<std::string>& warnings,
                           std::vector<std::string>& errors) const {
    warnings.clear();
    errors.clear();

    if (schema != 1) {
        warnings.push_back("settings schema " + std::to_string(schema) +
                           " was migrated to 1");
    }
    if (streaming.fps < 24 || streaming.fps > 240) {
        errors.push_back("streaming.fps must be 24..240");
    }
    if (streaming.bitrate_kbps < 1000 || streaming.bitrate_kbps > 200000) {
        errors.push_back("streaming.bitrate_kbps must be 1000..200000");
    }
    if (streaming.frame_buffer < 0 || streaming.frame_buffer > 3) {
        errors.push_back("streaming.frame_buffer must be 0..3");
    }
    if (video.sharpening < 0 || video.sharpening > 100) {
        errors.push_back("video.sharpening must be 0..100");
    }
    if (video.reflex < 0 || video.reflex > 2) {
        errors.push_back("video.reflex must be 0..2");
    }
    if (video.render_target != 0 && video.render_target != 1080 &&
        video.render_target != 1440 && video.render_target != 2160) {
        errors.push_back("video.render_target must be 0, 1080, 1440 or 2160");
    }
    if (ai.sr_quality < 0 || ai.sr_quality > 5) {
        errors.push_back("ai.sr_quality must be 0..5");
    }
    if (ai.sr_preset < 0 || ai.sr_preset > 4) {
        errors.push_back("ai.dlss_preset must be 0..4");
    }
    if (ai.fsr_quality < 0 || ai.fsr_quality > 4) {
        errors.push_back("ai.fsr_quality must be 0..4");
    }
    if (ai.fsr_sharpness < 0 || ai.fsr_sharpness > 100) {
        errors.push_back("ai.fsr_sharpness must be 0..100");
    }
    if (ai.sr_model != "auto" && ai.sr_model != "dlss" && ai.sr_model != "fsr4" &&
        ai.sr_model != "fsr3") {
        errors.push_back("ai.sr_model must be auto, dlss, fsr4 or fsr3");
    }
    if (ai.fg_multiplier < 2 || ai.fg_multiplier > 6) {
        errors.push_back("ai.fg_multiplier must be 2..6");
    }
    if (overlay.opacity < 0.1f || overlay.opacity > 1.0f) {
        errors.push_back("overlay.opacity must be 0.1..1.0");
    }
    if (overlay.interval_ms < 50 || overlay.interval_ms > 2000) {
        errors.push_back("overlay.interval_ms must be 50..2000");
    }
    if (overlay.position < 0 || overlay.position > 3) {
        errors.push_back("overlay.position must be 0..3");
    }
    if (overlay.side < 0 || overlay.side > 1) {
        errors.push_back("overlay.side must be 0..1");
    }
    if (overlay.scale < 75 || overlay.scale > 200) {
        errors.push_back("overlay.scale must be 75..200");
    }
    if (audio.buffer_ms < 5 || audio.buffer_ms > 200) {
        errors.push_back("audio.buffer_ms must be 5..200");
    }
    if (audio.volume < 0 || audio.volume > 100) {
        errors.push_back("audio.volume must be 0..100");
    }
    if (controller.poll_hz < 60 || controller.poll_hz > 1000) {
        errors.push_back("controller.poll_hz must be 60..1000");
    }
    if (controller.rumble_strength < 0 || controller.rumble_strength > 5) {
        errors.push_back("controller.rumble_strength must be 0..5");
    }
    if (display.bit_depth != 8 && display.bit_depth != 10) {
        errors.push_back("display.bit_depth must be 8 or 10");
    }
    if (gpu.frame_buffers < 1 || gpu.frame_buffers > 4) {
        errors.push_back("gpu.frame_buffers must be 1..4");
    }

    if (ai.super_resolution && ai.sr_output == Resolution::R2160 &&
        video.output == Resolution::R1080) {
        warnings.push_back("super resolution is 2160p but the output is 1080p");
    }
    if (!ai.super_resolution && ai.sr_output != Resolution::Native) {
        warnings.push_back("super resolution output is set while SR is off");
    }
    if (general.telemetry) {
        warnings.push_back("telemetry is disabled in this product; the flag is ignored");
    }
}

FlatMap SettingsToFlat(const AppSettings& s) {
    FlatMap f;
    FlatJson::PutInt(f, "schema", s.schema);

    FlatJson::PutString(f, "general.theme", std::string(ToString(s.general.theme)));
    FlatJson::PutBool(f, "general.start_minimized", s.general.start_minimized);
    FlatJson::PutBool(f, "general.close_to_tray", s.general.close_to_tray);
    FlatJson::PutBool(f, "general.notifications", s.general.notifications);
    FlatJson::PutBool(f, "general.check_updates", s.general.check_updates);
    FlatJson::PutInt(f, "general.update_channel", s.general.update_channel);
    FlatJson::PutString(f, "general.update_repo", s.general.update_repo);
    FlatJson::PutBool(f, "general.telemetry", s.general.telemetry);

    FlatJson::PutString(f, "console.name", s.console.name);
    FlatJson::PutString(f, "console.address", s.console.address);
    FlatJson::PutBool(f, "console.auto_discover", s.console.auto_discover);
    FlatJson::PutBool(f, "console.auto_reconnect", s.console.auto_reconnect);
    FlatJson::PutInt(f, "console.reconnect_delay_ms", s.console.reconnect_delay_ms);

    FlatJson::PutString(f, "streaming.input", std::string(ToString(s.streaming.input)));
    FlatJson::PutInt(f, "streaming.fps", s.streaming.fps);
    FlatJson::PutString(f, "streaming.codec", s.streaming.codec);
    FlatJson::PutString(f, "streaming.decoder", s.streaming.decoder);
    FlatJson::PutInt(f, "streaming.bitrate_kbps", s.streaming.bitrate_kbps);
    FlatJson::PutInt(f, "streaming.frame_buffer", s.streaming.frame_buffer);
    FlatJson::PutBool(f, "streaming.hdr", s.streaming.hdr);

    FlatJson::PutString(f, "video.output", std::string(ToString(s.video.output)));
    FlatJson::PutBool(f, "video.vsync", s.video.vsync);
    FlatJson::PutInt(f, "video.reflex", s.video.reflex);
    FlatJson::PutBool(f, "video.vrr", s.video.vrr);
    FlatJson::PutBool(f, "video.hdr", s.video.hdr);
    FlatJson::PutString(f, "video.color_space", s.video.color_space);
    FlatJson::PutInt(f, "video.sharpening", s.video.sharpening);
    FlatJson::PutInt(f, "video.render_target", s.video.render_target);

    FlatJson::PutBool(f, "ai.super_resolution", s.ai.super_resolution);
    FlatJson::PutString(f, "ai.sr_output", std::string(ToString(s.ai.sr_output)));
    FlatJson::PutString(f, "ai.sr_model", s.ai.sr_model);
    FlatJson::PutInt(f, "ai.sr_quality", s.ai.sr_quality);
    FlatJson::PutInt(f, "ai.dlss_preset", s.ai.sr_preset);
    FlatJson::PutInt(f, "ai.fsr_quality", s.ai.fsr_quality);
    FlatJson::PutInt(f, "ai.fsr_sharpness", s.ai.fsr_sharpness);
    FlatJson::PutBool(f, "ai.temporal", s.ai.temporal);
    FlatJson::PutBool(f, "ai.frame_generation", s.ai.frame_generation);
    FlatJson::PutString(f, "ai.fg_backend", std::string(ToString(s.ai.fg_backend)));
    FlatJson::PutInt(f, "ai.fg_multiplier", s.ai.fg_multiplier);
    FlatJson::PutInt(f, "ai.quality", s.ai.quality);
    FlatJson::PutBool(f, "ai.occlusion", s.ai.occlusion);
    FlatJson::PutBool(f, "ai.adaptive", s.ai.adaptive);
    FlatJson::PutBool(f, "ai.neural_render", s.ai.neural_render);
    FlatJson::PutInt(f, "ai.neural_style", s.ai.neural_style);
    FlatJson::PutInt(f, "ai.neural_intensity", s.ai.neural_intensity);
    FlatJson::PutInt(f, "ai.neural_tone", s.ai.neural_tone);
    FlatJson::PutInt(f, "ai.neural_structure", s.ai.neural_structure);
    FlatJson::PutInt(f, "ai.neural_skin", s.ai.neural_skin);
    FlatJson::PutInt(f, "ai.neural_passes", s.ai.neural_passes);
    FlatJson::PutInt(f, "ai.neural_scale", s.ai.neural_scale);
    FlatJson::PutBool(f, "ai.neural_unlock", s.ai.neural_unlock);
    FlatJson::PutBool(f, "ai.neural_ui", s.ai.neural_ui);
    FlatJson::PutBool(f, "ai.neural_mask", s.ai.neural_mask);

    FlatJson::PutInt(f, "gpu.device", s.gpu.device);
    FlatJson::PutBool(f, "gpu.fp16", s.gpu.fp16);
    FlatJson::PutBool(f, "gpu.int8", s.gpu.int8);
    FlatJson::PutInt(f, "gpu.frame_buffers", s.gpu.frame_buffers);
    FlatJson::PutInt(f, "gpu.memory_limit_mb", s.gpu.memory_limit_mb);
    FlatJson::PutInt(f, "gpu.priority", s.gpu.priority);

    FlatJson::PutInt(f, "display.monitor", s.display.monitor);
    FlatJson::PutString(f, "display.refresh", std::string(ToString(s.display.refresh)));
    FlatJson::PutBool(f, "display.vsync", s.display.vsync);
    FlatJson::PutBool(f, "display.vrr", s.display.vrr);
    FlatJson::PutBool(f, "display.fullscreen_on_play", s.display.fullscreen_on_play);
    FlatJson::PutString(f, "display.output_format", s.display.output_format);
    FlatJson::PutInt(f, "display.bit_depth", s.display.bit_depth);

    FlatJson::PutBool(f, "overlay.enabled", s.overlay.enabled);
    FlatJson::PutBool(f, "overlay.compact", s.overlay.compact);
    FlatJson::PutDouble(f, "overlay.opacity", s.overlay.opacity);
    FlatJson::PutInt(f, "overlay.position", s.overlay.position);
    FlatJson::PutInt(f, "overlay.side", s.overlay.side);
    FlatJson::PutInt(f, "overlay.scale", s.overlay.scale);
    FlatJson::PutInt(f, "overlay.interval_ms", s.overlay.interval_ms);
    FlatJson::PutBool(f, "overlay.show_performance", s.overlay.show_performance);
    FlatJson::PutBool(f, "overlay.show_gpu", s.overlay.show_gpu);
    FlatJson::PutBool(f, "overlay.show_stream", s.overlay.show_stream);
    FlatJson::PutBool(f, "overlay.show_processing", s.overlay.show_processing);
    FlatJson::PutString(f, "overlay.hotkey", s.overlay.hotkey);

    FlatJson::PutString(f, "audio.device", s.audio.device);
    FlatJson::PutInt(f, "audio.buffer_ms", s.audio.buffer_ms);
    FlatJson::PutInt(f, "audio.volume", s.audio.volume);
    FlatJson::PutBool(f, "audio.mute", s.audio.mute);

    FlatJson::PutBool(f, "controller.enabled", s.controller.enabled);
    FlatJson::PutBool(f, "controller.haptics", s.controller.haptics);
    FlatJson::PutBool(f, "controller.adaptive_triggers", s.controller.adaptive_triggers);
    FlatJson::PutInt(f, "controller.rumble_strength", s.controller.rumble_strength);
    FlatJson::PutInt(f, "controller.poll_hz", s.controller.poll_hz);
    FlatJson::PutInt(f, "controller.lightbar", s.controller.lightbar);

    FlatJson::PutBool(f, "advanced.experimental", s.advanced.experimental);
    FlatJson::PutBool(f, "advanced.debug_log", s.advanced.debug_log);
    FlatJson::PutString(f, "advanced.log_level", std::string(ToString(s.advanced.log_level)));
    FlatJson::PutBool(f, "advanced.prefer_gpu_present", s.advanced.prefer_gpu_present);
    return f;
}

void SettingsFromFlat(const FlatMap& f, AppSettings& s) {
    s.schema = ClampInt(FlatJson::GetInt(f, "schema", s.schema), 1, 1);

    s.general.theme = ReadTheme(f, "general.theme", s.general.theme);
    s.general.start_minimized =
        FlatJson::GetBool(f, "general.start_minimized", s.general.start_minimized);
    s.general.close_to_tray =
        FlatJson::GetBool(f, "general.close_to_tray", s.general.close_to_tray);
    s.general.notifications =
        FlatJson::GetBool(f, "general.notifications", s.general.notifications);
    s.general.check_updates =
        FlatJson::GetBool(f, "general.check_updates", s.general.check_updates);
    s.general.update_channel = ClampInt(
        FlatJson::GetInt(f, "general.update_channel", s.general.update_channel), 0, 1);
    s.general.update_repo =
        FlatJson::GetString(f, "general.update_repo", s.general.update_repo);
    s.general.telemetry =
        FlatJson::GetBool(f, "general.telemetry", s.general.telemetry);

    s.console.name = FlatJson::GetString(f, "console.name", s.console.name);
    s.console.address = FlatJson::GetString(f, "console.address", s.console.address);
    s.console.auto_discover =
        FlatJson::GetBool(f, "console.auto_discover", s.console.auto_discover);
    s.console.auto_reconnect =
        FlatJson::GetBool(f, "console.auto_reconnect", s.console.auto_reconnect);
    s.console.reconnect_delay_ms = ClampInt(
        FlatJson::GetInt(f, "console.reconnect_delay_ms", s.console.reconnect_delay_ms),
        100, 10000);

    s.streaming.input = ReadResolution(f, "streaming.input", s.streaming.input);
    s.streaming.fps =
        ClampInt(FlatJson::GetInt(f, "streaming.fps", s.streaming.fps), 24, 240);
    s.streaming.codec = FlatJson::GetString(f, "streaming.codec", s.streaming.codec);
    s.streaming.decoder = FlatJson::GetString(f, "streaming.decoder", s.streaming.decoder);
    s.streaming.bitrate_kbps = ClampInt(
        FlatJson::GetInt(f, "streaming.bitrate_kbps", s.streaming.bitrate_kbps), 1000,
        200000);
    s.streaming.frame_buffer = ClampInt(
        FlatJson::GetInt(f, "streaming.frame_buffer", s.streaming.frame_buffer), 0, 3);
    s.streaming.hdr = FlatJson::GetBool(f, "streaming.hdr", s.streaming.hdr);

    s.video.output = ReadResolution(f, "video.output", s.video.output);
    s.video.vsync = FlatJson::GetBool(f, "video.vsync", s.video.vsync);
    s.video.reflex = ClampInt(FlatJson::GetInt(f, "video.reflex", s.video.reflex), 0, 2);
    s.video.vrr = FlatJson::GetBool(f, "video.vrr", s.video.vrr);
    s.video.hdr = FlatJson::GetBool(f, "video.hdr", s.video.hdr);
    s.video.color_space = FlatJson::GetString(f, "video.color_space", s.video.color_space);
    s.video.sharpening = ClampInt(
        FlatJson::GetInt(f, "video.sharpening", s.video.sharpening), 0, 100);
    const int render_target = ClampInt(
        FlatJson::GetInt(f, "video.render_target", s.video.render_target), 0, 2160);
    s.video.render_target = render_target == 1 ? 1080
                                : (render_target == 2 ? 1440
                                                     : (render_target == 3 ? 2160 : render_target));

    s.ai.super_resolution =
        FlatJson::GetBool(f, "ai.super_resolution", s.ai.super_resolution);
    s.ai.sr_output = ReadResolution(f, "ai.sr_output", s.ai.sr_output);
    s.ai.sr_model = FlatJson::GetString(f, "ai.sr_model", s.ai.sr_model);
    s.ai.sr_quality =
        ClampInt(FlatJson::GetInt(f, "ai.sr_quality", s.ai.sr_quality), 0, 5);
    s.ai.sr_preset =
        ClampInt(FlatJson::GetInt(f, "ai.dlss_preset", s.ai.sr_preset), 0, 4);
    s.ai.fsr_quality =
        ClampInt(FlatJson::GetInt(f, "ai.fsr_quality", s.ai.fsr_quality), 0, 4);
    s.ai.fsr_sharpness =
        ClampInt(FlatJson::GetInt(f, "ai.fsr_sharpness", s.ai.fsr_sharpness), 0, 100);
    if (s.ai.sr_quality >= 5) {
        // DLAA runs at the stream size; an SR target has no meaning.
        s.ai.sr_output = Resolution::Native;
    }
    s.ai.temporal = FlatJson::GetBool(f, "ai.temporal", s.ai.temporal);
    s.ai.frame_generation =
        FlatJson::GetBool(f, "ai.frame_generation", s.ai.frame_generation);
    s.ai.fg_backend = ReadBackend(f, "ai.fg_backend", s.ai.fg_backend);
    s.ai.fg_multiplier =
        ClampInt(FlatJson::GetInt(f, "ai.fg_multiplier", s.ai.fg_multiplier), 2, 6);
    s.ai.quality = ClampInt(FlatJson::GetInt(f, "ai.quality", s.ai.quality), 0, 3);
    s.ai.occlusion = FlatJson::GetBool(f, "ai.occlusion", s.ai.occlusion);
    s.ai.adaptive = FlatJson::GetBool(f, "ai.adaptive", s.ai.adaptive);
    s.ai.neural_render =
        FlatJson::GetBool(f, "ai.neural_render", s.ai.neural_render);
    s.ai.neural_style =
        ClampInt(FlatJson::GetInt(f, "ai.neural_style", s.ai.neural_style), 0, 2);
    // Neural limits are only enforced by the UI: the file may carry anything up
    // to the hard 9999 ceiling the "unlock limits" switch exposes.
    s.ai.neural_intensity = ClampInt(
        FlatJson::GetInt(f, "ai.neural_intensity", s.ai.neural_intensity), 0, 9999);
    s.ai.neural_tone =
        ClampInt(FlatJson::GetInt(f, "ai.neural_tone", s.ai.neural_tone), 0, 9999);
    s.ai.neural_structure = ClampInt(
        FlatJson::GetInt(f, "ai.neural_structure", s.ai.neural_structure), 0, 9999);
    s.ai.neural_skin = ClampInt(
        FlatJson::GetInt(f, "ai.neural_skin", s.ai.neural_skin), 0, 9999);
    s.ai.neural_passes =
        ClampInt(FlatJson::GetInt(f, "ai.neural_passes", s.ai.neural_passes), 1, 9999);
    s.ai.neural_scale =
        ClampInt(FlatJson::GetInt(f, "ai.neural_scale", s.ai.neural_scale), 1, 9999);
    s.ai.neural_unlock = FlatJson::GetBool(f, "ai.neural_unlock", s.ai.neural_unlock);
    s.ai.neural_ui = FlatJson::GetBool(f, "ai.neural_ui", s.ai.neural_ui);
    s.ai.neural_mask =
        FlatJson::GetBool(f, "ai.neural_mask", s.ai.neural_mask);

    s.gpu.device = ClampInt(FlatJson::GetInt(f, "gpu.device", s.gpu.device), 0, 15);
    s.gpu.fp16 = FlatJson::GetBool(f, "gpu.fp16", s.gpu.fp16);
    s.gpu.int8 = FlatJson::GetBool(f, "gpu.int8", s.gpu.int8);
    s.gpu.frame_buffers = ClampInt(
        FlatJson::GetInt(f, "gpu.frame_buffers", s.gpu.frame_buffers), 1, 4);
    s.gpu.memory_limit_mb = ClampInt(
        FlatJson::GetInt(f, "gpu.memory_limit_mb", s.gpu.memory_limit_mb), 0, 49152);
    s.gpu.priority = ClampInt(FlatJson::GetInt(f, "gpu.priority", s.gpu.priority), 0, 1);

    s.display.monitor =
        ClampInt(FlatJson::GetInt(f, "display.monitor", s.display.monitor), 0, 7);
    s.display.refresh = ReadFrameRate(f, "display.refresh", s.display.refresh);
    s.display.vsync = FlatJson::GetBool(f, "display.vsync", s.display.vsync);
    s.display.vrr = FlatJson::GetBool(f, "display.vrr", s.display.vrr);
    s.display.fullscreen_on_play =
        FlatJson::GetBool(f, "display.fullscreen_on_play", s.display.fullscreen_on_play);
    s.display.output_format =
        FlatJson::GetString(f, "display.output_format", s.display.output_format);
    s.display.bit_depth =
        ClampInt(FlatJson::GetInt(f, "display.bit_depth", s.display.bit_depth), 8, 10);

    s.overlay.enabled = FlatJson::GetBool(f, "overlay.enabled", s.overlay.enabled);
    s.overlay.compact = FlatJson::GetBool(f, "overlay.compact", s.overlay.compact);
    s.overlay.opacity = static_cast<float>(std::clamp(
        FlatJson::GetDouble(f, "overlay.opacity", s.overlay.opacity), 0.1, 1.0));
    s.overlay.position =
        ClampInt(FlatJson::GetInt(f, "overlay.position", s.overlay.position), 0, 3);
    s.overlay.side =
        ClampInt(FlatJson::GetInt(f, "overlay.side", s.overlay.side), 0, 1);
    s.overlay.scale =
        ClampInt(FlatJson::GetInt(f, "overlay.scale", s.overlay.scale), 75, 200);
    s.overlay.interval_ms = ClampInt(
        FlatJson::GetInt(f, "overlay.interval_ms", s.overlay.interval_ms), 50, 2000);
    s.overlay.show_performance = FlatJson::GetBool(
        f, "overlay.show_performance", s.overlay.show_performance);
    s.overlay.show_gpu =
        FlatJson::GetBool(f, "overlay.show_gpu", s.overlay.show_gpu);
    s.overlay.show_stream =
        FlatJson::GetBool(f, "overlay.show_stream", s.overlay.show_stream);
    s.overlay.show_processing = FlatJson::GetBool(
        f, "overlay.show_processing", s.overlay.show_processing);
    s.overlay.hotkey = FlatJson::GetString(f, "overlay.hotkey", s.overlay.hotkey);

    s.audio.device = FlatJson::GetString(f, "audio.device", s.audio.device);
    s.audio.buffer_ms =
        ClampInt(FlatJson::GetInt(f, "audio.buffer_ms", s.audio.buffer_ms), 5, 200);
    s.audio.volume =
        ClampInt(FlatJson::GetInt(f, "audio.volume", s.audio.volume), 0, 100);
    s.audio.mute = FlatJson::GetBool(f, "audio.mute", s.audio.mute);

    s.controller.enabled =
        FlatJson::GetBool(f, "controller.enabled", s.controller.enabled);
    s.controller.haptics =
        FlatJson::GetBool(f, "controller.haptics", s.controller.haptics);
    s.controller.adaptive_triggers = FlatJson::GetBool(
        f, "controller.adaptive_triggers", s.controller.adaptive_triggers);
    s.controller.rumble_strength = ClampInt(
        FlatJson::GetInt(f, "controller.rumble_strength", s.controller.rumble_strength), 0,
        5);
    s.controller.poll_hz = ClampInt(
        FlatJson::GetInt(f, "controller.poll_hz", s.controller.poll_hz), 60, 1000);
    s.controller.lightbar =
        ClampInt(FlatJson::GetInt(f, "controller.lightbar", s.controller.lightbar), 0, 4);

    s.advanced.experimental =
        FlatJson::GetBool(f, "advanced.experimental", s.advanced.experimental);
    s.advanced.debug_log =
        FlatJson::GetBool(f, "advanced.debug_log", s.advanced.debug_log);
    s.advanced.log_level = ReadLogLevel(f, "advanced.log_level", s.advanced.log_level);
    s.advanced.prefer_gpu_present = FlatJson::GetBool(
        f, "advanced.prefer_gpu_present", s.advanced.prefer_gpu_present);
}

Status ImportLegacySettings(const std::filesystem::path& path, AppSettings& out,
                            std::vector<std::string>& warnings) {
    warnings.clear();
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return Status::Error("legacy settings not found: " + path.string());
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    FlatMap flat;
    std::string error;
    if (!FlatJson::Parse(buffer.str(), flat, error)) {
        return Status::Error("legacy settings parse failed: " + error);
    }

    out = AppSettings{};
    out.console.name = FlatJson::GetString(flat, "name", "");
    out.console.address = FlatJson::GetString(flat, "host", "");
    const int width = ClampInt(FlatJson::GetInt(flat, "width", 1920), 0, 16384);
    const int height = ClampInt(FlatJson::GetInt(flat, "height", 1080), 0, 16384);
    out.streaming.input = ResolutionFromLegacySize(width, height);
    out.streaming.fps = ClampInt(FlatJson::GetInt(flat, "fps", 60), 24, 240);
    out.streaming.bitrate_kbps =
        ClampInt(FlatJson::GetInt(flat, "bitrate_kbps", 100000), 1000, 200000);
    out.streaming.codec = FlatJson::GetString(flat, "codec", "auto");
    out.streaming.hdr = FlatJson::GetBool(flat, "hdr", false);
    out.streaming.decoder = DecoderName(FlatJson::GetInt(flat, "decoder", 0));
    out.streaming.frame_buffer =
        ClampInt(FlatJson::GetInt(flat, "frame_buffer", 1), 0, 3);
    out.video.hdr = out.streaming.hdr;
    out.video.sharpening = ClampInt(FlatJson::GetInt(flat, "sharpness", 10), 0, 100);
    const int legacy_target = ClampInt(FlatJson::GetInt(flat, "render_target", 0), 0, 2160);
    out.video.render_target = legacy_target == 1 ? 1080
                              : (legacy_target == 2 ? 1440
                                                   : (legacy_target == 3 ? 2160 : legacy_target));

    out.ai.super_resolution = FlatJson::GetInt(flat, "neural_sr", 0) > 0;
    out.ai.sr_quality = ClampInt(FlatJson::GetInt(flat, "dlss_mode", 4), 0, 5);
    if (out.ai.sr_quality >= 5) {
        out.ai.sr_output = Resolution::Native;
    }
    out.ai.frame_generation = FlatJson::GetInt(flat, "dlss_fg", 0) >= 2;
    out.ai.fg_multiplier = ClampInt(FlatJson::GetInt(flat, "dlss_fg", 2), 2, 6);
    // Legacy profiles that selected the removed TensorRT interpolator fall
    // back to DLSS-G (or off when frame generation was off).
    out.ai.fg_backend = out.ai.frame_generation ? AiBackend::DlssFg : AiBackend::None;
    out.audio.buffer_ms =
        ClampInt(FlatJson::GetInt(flat, "audio_buffer_ms", 20), 5, 200);
    out.controller.haptics = FlatJson::GetInt(flat, "haptic_intensity", 3) > 0;
    out.controller.rumble_strength =
        ClampInt(FlatJson::GetInt(flat, "haptic_intensity", 3), 0, 5);
    out.advanced.debug_log = FlatJson::GetInt(flat, "testing_log", 0) != 0;

    warnings.push_back("legacy profile imported; credentials stay in the old file");
    if (flat.find("audio_device_id") != flat.end()) {
        warnings.push_back("legacy audio device id is not portable; using default");
    }
    if (flat.find("color_on") != flat.end()) {
        warnings.push_back("legacy colour grading is not part of the schema yet");
    }
    return Status::Ok();
}

SettingsStore::SettingsStore(std::filesystem::path file) : file_(std::move(file)) {}

Status SettingsStore::Load(AppSettings& out, std::vector<std::string>& warnings) {
    warnings.clear();
    out = AppSettings{};
    std::error_code ec;
    if (!std::filesystem::exists(file_, ec)) {
        warnings.push_back("no settings file; defaults created");
        return Status::Ok();
    }
    std::ifstream file(file_, std::ios::binary);
    if (!file) {
        return Status::Error("cannot open " + file_.string());
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    FlatMap flat;
    std::string error;
    if (!FlatJson::Parse(buffer.str(), flat, error)) {
        return Status::Error("settings parse failed: " + error);
    }
    SettingsFromFlat(flat, out);
    if (out.schema < 1) {
        out.schema = 1;
        warnings.push_back("settings migrated to schema 1");
    }
    std::vector<std::string> errors;
    out.Validate(warnings, errors);
    if (!errors.empty()) {
        std::string joined;
        for (const auto& message : errors) {
            if (!joined.empty()) joined += "; ";
            joined += message;
        }
        return Status::Error("settings invalid: " + joined);
    }
    return Status::Ok();
}

Status SettingsStore::Save(const AppSettings& settings) {
    if (file_.empty()) {
        return Status::Error("settings path is empty");
    }
    std::error_code ec;
    if (file_.has_parent_path()) {
        std::filesystem::create_directories(file_.parent_path(), ec);
    }
    // Merge instead of replace: the shell owns keys this store does not know
    // (ui.*, system.*), and a rewrite used to drop them whenever the engine
    // recreated the file.
    FlatMap merged;
    {
        std::ifstream existing(file_, std::ios::binary);
        if (existing) {
            const std::string text((std::istreambuf_iterator<char>(existing)),
                                   std::istreambuf_iterator<char>());
            std::string error;
            FlatJson::Parse(text, merged, error);
        }
    }
    for (const auto& [key, value] : SettingsToFlat(settings)) {
        merged[key] = value;
    }
    const std::filesystem::path temp = file_.string() + ".tmp";
    {
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        if (!file) {
            return Status::Error("cannot write " + temp.string());
        }
        file << FlatJson::Serialize(merged);
        if (!file) {
            return Status::Error("write failed: " + temp.string());
        }
    }
    const bool had_old = std::filesystem::exists(file_, ec);
    if (had_old) {
        std::filesystem::copy_file(file_, file_.string() + ".bak",
                                   std::filesystem::copy_options::overwrite_existing, ec);
        std::filesystem::remove(file_, ec);
    }
    std::filesystem::rename(temp, file_, ec);
    if (ec) {
        // Restore the previous valid file instead of leaving no settings.
        if (had_old) {
            std::error_code restore_ec;
            std::filesystem::copy_file(file_.string() + ".bak", file_,
                                       std::filesystem::copy_options::overwrite_existing,
                                       restore_ec);
        }
        return Status::Error("cannot replace settings: " + ec.message());
    }
    return Status::Ok();
}

Status SettingsStore::Reset(AppSettings& out) {
    out = AppSettings{};
    return Save(out);
}

std::string SettingsStore::Location() const {
    return file_.string();
}

std::string DefaultSettingsPath() {
    const char* appdata = std::getenv("APPDATA");
    const std::string root = appdata ? appdata : ".";
    return root + "\\BetterRemotePlayNext\\next-settings.json";
}

std::string DefaultProfilesDirectory() {
    const char* appdata = std::getenv("APPDATA");
    const std::string root = appdata ? appdata : ".";
    return root + "\\BetterRemotePlayNext\\profiles";
}

}  // namespace brp::next
