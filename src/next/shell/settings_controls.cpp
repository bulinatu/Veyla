#include "shell/settings_controls.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <string>
#include <vector>

namespace brp::next {
namespace {

constexpr float kRow = 50.0f;
constexpr float kGap = 6.0f;

std::wstring Widen(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int size = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (size <= 1) {
        return {};
    }
    std::wstring out(static_cast<std::size_t>(size - 1), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, out.data(), size);
    return out;
}

D2D1_RECT_F Row(float x, float y, float width) {
    return Rect(x, y, x + width, y + kRow);
}

int ResolutionIndex(Resolution value) {
    switch (value) {
        case Resolution::Native: return 0;
        case Resolution::R1080: return 1;
        case Resolution::R1440: return 2;
        case Resolution::R2160: return 3;
    }
    return 0;
}

Resolution ResolutionAt(int index) {
    switch (index) {
        case 1: return Resolution::R1080;
        case 2: return Resolution::R1440;
        case 3: return Resolution::R2160;
        default: return Resolution::Native;
    }
}

const std::vector<std::wstring> kResolutions = {L"Native", L"1080p", L"1440p", L"2160p"};
const std::vector<std::wstring> kTargets = {L"60", L"90", L"120", L"144", L"165", L"240"};
const std::vector<std::wstring> kRefresh = {L"Match", L"60", L"90", L"120",
                                             L"144",   L"165", L"240"};

int TargetIndex(int fps) {
    switch (fps) {
        case 60: return 0;
        case 90: return 1;
        case 120: return 2;
        case 144: return 3;
        case 165: return 4;
        case 240: return 5;
        default: return 2;
    }
}

int TargetAt(int index) {
    static const int values[] = {60, 90, 120, 144, 165, 240};
    return values[std::clamp(index, 0, 5)];
}

int CodecIndex(const std::string& codec) {
    if (codec == "h264") return 1;
    if (codec == "hevc") return 2;
    if (codec == "av1") return 3;
    return 0;
}

const char* CodecAt(int index) {
    switch (index) {
        case 1: return "h264";
        case 2: return "hevc";
        case 3: return "av1";
        default: return "auto";
    }
}

int DecoderIndex(const std::string& decoder) {
    if (decoder == "d3d11va") return 1;
    if (decoder == "d3d12va") return 2;
    if (decoder == "vulkan") return 3;
    if (decoder == "cuda") return 4;
    if (decoder == "software") return 5;
    return 0;
}

const char* DecoderAt(int index) {
    switch (index) {
        case 1: return "d3d11va";
        case 2: return "d3d12va";
        case 3: return "vulkan";
        case 4: return "cuda";
        case 5: return "software";
        default: return "auto";
    }
}

int BitDepthIndex(int depth) { return depth == 10 ? 1 : 0; }

int FormatIndex(const std::string& format) {
    if (format == "sdr") return 1;
    if (format == "hdr10") return 2;
    return 0;
}

const char* FormatAt(int index) {
    switch (index) {
        case 1: return "sdr";
        case 2: return "hdr10";
        default: return "auto";
    }
}

std::wstring MonitorLabel(const DisplayInfo& display, int index) {
    std::wstring label = L"Display " + std::to_wstring(index + 1);
    if (display.width > 0) {
        label += L" (" + std::to_wstring(display.width) + L"x" +
                 std::to_wstring(display.height) + L")";
    }
    if (display.primary) {
        label += L" primary";
    }
    return label;
}

}  // namespace

SettingsControls::SettingsControls(Widgets& widgets, AppSettings& settings,
                                   const SystemInfo& system,
                                   bool frame_generation_wired)
    : widgets_(widgets),
      settings_(settings),
      system_(system),
      frame_generation_wired_(frame_generation_wired) {}

void SettingsControls::Reset() {
    id_ = 0;
    changed_ = false;
}

float SettingsControls::General(float x, float y, float width) {
    int theme = settings_.general.theme == Theme::Light ? 1 : 0;
    if (widgets_.Choice(Next(), Row(x, y, width), L"Theme", {L"Dark", L"Light"}, theme)) {
        settings_.general.theme = theme == 1 ? Theme::Light : Theme::Dark;
        Touch();
    }
    y += kRow + kGap;
    if (widgets_.Toggle(Next(), Row(x, y, width), L"Start minimized",
                        settings_.general.start_minimized)) {
        Touch();
    }
    y += kRow + kGap;
    if (widgets_.Toggle(Next(), Row(x, y, width), L"Notifications",
                        settings_.general.notifications)) {
        Touch();
    }
    y += kRow + kGap;
    if (widgets_.Toggle(Next(), Row(x, y, width), L"Check for updates",
                        settings_.general.check_updates)) {
        Touch();
    }
    y += kRow + kGap;
    if (widgets_.Toggle(Next(), Row(x, y, width), L"Close to tray",
                        settings_.general.close_to_tray)) {
        Touch();
    }
    return y + kRow;
}

float SettingsControls::Streaming(float x, float y, float width) {
    int input = ResolutionIndex(settings_.streaming.input);
    if (widgets_.Choice(Next(), Row(x, y, width), L"Source resolution", kResolutions,
                        input)) {
        settings_.streaming.input = ResolutionAt(input);
        Touch();
    }
    y += kRow + kGap;

    float fps = static_cast<float>(settings_.streaming.fps);
    if (widgets_.Slider(Next(), Row(x, y, width), L"Source frame rate", fps, 24.0f,
                        240.0f, 1.0f, L"%.0f fps")) {
        settings_.streaming.fps = static_cast<int>(std::lround(fps));
        Touch();
    }
    y += kRow + kGap;

    float bitrate = static_cast<float>(settings_.streaming.bitrate_kbps);
    if (widgets_.Slider(Next(), Row(x, y, width), L"Bitrate", bitrate, 5000.0f,
                        200000.0f, 1000.0f, L"%.0f kbps")) {
        settings_.streaming.bitrate_kbps = static_cast<int>(std::lround(bitrate));
        Touch();
    }
    y += kRow + kGap;

    int buffer = settings_.streaming.frame_buffer;
    if (widgets_.Choice(Next(), Row(x, y, width), L"Frame buffer",
                        {L"0", L"1", L"2", L"3"}, buffer)) {
        settings_.streaming.frame_buffer = buffer;
        Touch();
    }
    y += kRow + kGap;

    int codec = CodecIndex(settings_.streaming.codec);
    if (widgets_.Choice(Next(), Row(x, y, width), L"Codec",
                        {L"Auto", L"H.264", L"HEVC", L"AV1"}, codec)) {
        settings_.streaming.codec = CodecAt(codec);
        Touch();
    }
    y += kRow + kGap;

    int decoder = DecoderIndex(settings_.streaming.decoder);
    if (widgets_.Choice(Next(), Row(x, y, width), L"Decoder",
                        {L"Auto", L"D3D11VA", L"D3D12VA", L"Vulkan", L"CUDA",
                         L"Software"},
                        decoder)) {
        settings_.streaming.decoder = DecoderAt(decoder);
        Touch();
    }
    y += kRow + kGap;

    if (widgets_.Toggle(Next(), Row(x, y, width), L"HDR stream",
                        settings_.streaming.hdr)) {
        Touch();
    }
    return y + kRow;
}

float SettingsControls::Video(float x, float y, float width) {
    int output = ResolutionIndex(settings_.video.output);
    if (widgets_.Choice(Next(), Row(x, y, width), L"Output resolution", kResolutions,
                        output)) {
        settings_.video.output = ResolutionAt(output);
        Touch();
    }
    y += kRow + kGap;

    int target = settings_.video.render_target == 1080
                     ? 1
                     : (settings_.video.render_target == 1440
                            ? 2
                            : (settings_.video.render_target == 2160 ? 3 : 0));
    if (widgets_.Choice(Next(), Row(x, y, width), L"Render target",
                        {L"Window", L"1080p", L"1440p", L"2160p"}, target)) {
        settings_.video.render_target =
            target == 1 ? 1080 : (target == 2 ? 1440 : (target == 3 ? 2160 : 0));
        Touch();
    }
    y += kRow + kGap;

    if (widgets_.Toggle(Next(), Row(x, y, width), L"V-Sync", settings_.video.vsync)) {
        Touch();
    }
    y += kRow + kGap;
    if (widgets_.Toggle(Next(), Row(x, y, width), L"VRR / G-Sync", settings_.video.vrr)) {
        Touch();
    }
    y += kRow + kGap;

    float sharpening = static_cast<float>(settings_.video.sharpening);
    if (widgets_.Slider(Next(), Row(x, y, width), L"Sharpening", sharpening, 0.0f,
                        100.0f, 1.0f, L"%.0f%%")) {
        settings_.video.sharpening = static_cast<int>(std::lround(sharpening));
        Touch();
    }
    y += kRow + kGap;

    if (widgets_.Toggle(Next(), Row(x, y, width), L"HDR output", settings_.video.hdr)) {
        Touch();
    }
    return y + kRow;
}

float SettingsControls::Ai(float x, float y, float width) {
    const bool sr_available = system_.dlss_sr;
    if (widgets_.Toggle(Next(), Row(x, y, width), L"Super resolution",
                        settings_.ai.super_resolution, sr_available)) {
        Touch();
    }
    y += kRow + kGap;

    int sr_output = ResolutionIndex(settings_.ai.sr_output);
    if (widgets_.Choice(Next(), Row(x, y, width), L"SR output", kResolutions, sr_output,
                        settings_.ai.super_resolution)) {
        settings_.ai.sr_output = ResolutionAt(sr_output);
        Touch();
    }
    y += kRow + kGap;

    int quality = settings_.ai.sr_quality;
    if (widgets_.Choice(Next(), Row(x, y, width), L"SR quality",
                        {L"Performance", L"Balanced", L"Quality", L"Ultra",
                         L"Ultra Quality", L"DLAA"},
                        quality, settings_.ai.super_resolution)) {
        settings_.ai.sr_quality = quality;
        Touch();
    }
    y += kRow + kGap;

    widgets_.Label(Row(x, y, width),
                   sr_available ? L"SR backend: DLSS Super Resolution (NGX)"
                                : L"SR backend: not installed (no nvngx_dlss.dll)",
                   sr_available ? widgets_.Tokens().success : widgets_.Tokens().text_faint,
                   13.0f, DWRITE_FONT_WEIGHT_MEDIUM);
    y += kRow + kGap;

    std::vector<AiBackend> backends = {AiBackend::DlssFg, AiBackend::Rife};
    std::vector<std::wstring> labels = {L"DLSS FG (worker)", L"RIFE (TensorRT)"};
    std::vector<bool> item_enabled = {system_.dlss_fg, system_.rife_lite};
    const bool fg_available =
        frame_generation_wired_ && (item_enabled[0] || item_enabled[1]);
    if (widgets_.Toggle(Next(), Row(x, y, width), L"Frame generation",
                        settings_.ai.frame_generation, fg_available)) {
        Touch();
    }
    y += kRow + kGap;

    if (fg_available) {
        int backend_index = 0;
        for (std::size_t i = 0; i < backends.size(); ++i) {
            if (backends[i] == settings_.ai.fg_backend) {
                backend_index = static_cast<int>(i);
                break;
            }
        }
        if (widgets_.Choice(Next(), Row(x, y, width), L"FG backend", labels,
                            backend_index, settings_.ai.frame_generation,
                            &item_enabled)) {
            if (item_enabled[static_cast<std::size_t>(backend_index)]) {
                settings_.ai.fg_backend =
                    backends[static_cast<std::size_t>(backend_index)];
                Touch();
            }
        }
        y += kRow + kGap;

        if (settings_.ai.fg_backend == AiBackend::DlssFg) {
            int multiplier = std::clamp(settings_.ai.fg_multiplier, 2, 6) - 2;
            const std::vector<std::wstring> mult_labels = {L"2x", L"3x", L"4x",
                                                           L"5x", L"6x"};
            std::vector<bool> mult_enabled(5, system_.dlss_fg_multi);
            mult_enabled[0] = true;
            if (widgets_.Choice(Next(), Row(x, y, width), L"DLSS-G multiplier",
                                mult_labels, multiplier,
                                settings_.ai.frame_generation, &mult_enabled)) {
                settings_.ai.fg_multiplier = multiplier + 2;
                Touch();
            }
            y += kRow + kGap;
        }
    } else {
        widgets_.Label(Row(x, y, width),
                       system_.nr_worker
                           ? L"FG unavailable: nvngx_dlssg.dll is missing"
                           : L"FG unavailable: worker and RIFE engine are not installed",
                       widgets_.Tokens().text_faint, 13.0f,
                       DWRITE_FONT_WEIGHT_MEDIUM);
        y += kRow + kGap;
    }

    int target = TargetIndex(settings_.ai.target_fps);
    if (widgets_.Choice(Next(), Row(x, y, width), L"Target frame rate", kTargets, target,
                        settings_.ai.frame_generation)) {
        settings_.ai.target_fps = TargetAt(target);
        Touch();
    }
    y += kRow + kGap;

    if (settings_.ai.fg_backend == AiBackend::Rife &&
        settings_.ai.frame_generation) {
        int model = settings_.ai.rife_model;
        const std::vector<bool> model_enabled = {true, system_.rife_heavy};
        if (widgets_.Choice(Next(), Row(x, y, width), L"RIFE model",
                            {L"Lite 4.25", L"Heavy 4.26"}, model,
                            settings_.ai.frame_generation, &model_enabled)) {
            if (model_enabled[static_cast<std::size_t>(model)]) {
                settings_.ai.rife_model = model;
                Touch();
            }
        }
        y += kRow + kGap;
        if (settings_.ai.rife_model == 1 && !system_.rife_heavy) {
            widgets_.Label(Rect(x, y, x + width, y + 34),
                           L"Heavy engine is not built; Lite is used until Set up RIFE "
                           L"builds it in the reference app.",
                           widgets_.Tokens().warning, 11.5f, DWRITE_FONT_WEIGHT_MEDIUM);
            y += 36.0f;
        }

        float blend = static_cast<float>(settings_.ai.blend);
        if (widgets_.Slider(Next(), Row(x, y, width), L"RIFE blend", blend, 0.0f, 40.0f,
                            10.0f, L"%.0f%%")) {
            settings_.ai.blend = static_cast<int>(std::lround(blend));
            Touch();
        }
        y += kRow + kGap;

        if (widgets_.Toggle(Next(), Row(x, y, width), L"Motion safety",
                            settings_.ai.motion_safety)) {
            Touch();
        }
        y += kRow + kGap;
    }

    if (settings_.ai.frame_generation &&
        settings_.ai.fg_backend == AiBackend::DlssFg) {
        const int source = std::clamp(settings_.streaming.fps, 24, 240);
        const int multiplier = std::clamp(settings_.ai.fg_multiplier, 2, 6);
        const int out = source * multiplier;
        wchar_t note[260] = {};
        if (multiplier == 2) {
            std::swprintf(note, 260,
                          L"%d fps source -> %d fps with 2x. The worker presents in "
                          L"its own window; this is the clean cadence.",
                          source, out);
        } else {
            std::swprintf(note, 260,
                          L"%dx asks for up to %d fps. The worker sustains about "
                          L"150-165 generated fps; the overlay shows the real rate.",
                          multiplier, out);
        }
        widgets_.Label(Rect(x, y, x + width, y + 40), note,
                       widgets_.Tokens().text_faint, 11.5f,
                       DWRITE_FONT_WEIGHT_MEDIUM);
        y += 42.0f;
        if (settings_.ai.super_resolution) {
            widgets_.Label(Rect(x, y, x + width, y + 36),
                           settings_.ai.sr_output == Resolution::R2160
                               ? L"DLSS-G is capped at 1440p. Use 1440p SR or turn SR off."
                               : L"SR feeds the worker through a CPU readback; at "
                                 L"1440p expect about 100 generated fps.",
                           settings_.ai.sr_output == Resolution::R2160
                               ? widgets_.Tokens().warning
                               : widgets_.Tokens().text_faint,
                           11.5f, DWRITE_FONT_WEIGHT_MEDIUM);
            y += 38.0f;
        }
        if (!system_.dlss_fg_multi && settings_.ai.fg_multiplier > 2) {
            widgets_.Label(Rect(x, y, x + width, y + 36),
                           L"The SM86 FG proxy is not installed; the runtime steps "
                           L"back to 2x.",
                           widgets_.Tokens().warning, 11.5f,
                           DWRITE_FONT_WEIGHT_MEDIUM);
            y += 38.0f;
        }
    }
    return y;
}
float SettingsControls::Gpu(float x, float y, float width) {
    widgets_.Label(Row(x, y, width),
                   system_.gpu.present ? (L"Adapter: " + Widen(system_.gpu.name)).c_str()
                                       : L"Adapter: none detected",
                   widgets_.Tokens().text_dim, 13.0f, DWRITE_FONT_WEIGHT_MEDIUM);
    y += kRow + kGap;
    if (widgets_.Toggle(Next(), Row(x, y, width), L"FP16 inference",
                        settings_.gpu.fp16)) {
        Touch();
    }
    y += kRow + kGap;
    if (widgets_.Toggle(Next(), Row(x, y, width), L"INT8 inference (experimental)",
                        settings_.gpu.int8, settings_.advanced.experimental)) {
        Touch();
    }
    y += kRow + kGap;

    float buffers = static_cast<float>(settings_.gpu.frame_buffers);
    if (widgets_.Slider(Next(), Row(x, y, width), L"Frame buffers", buffers, 1.0f, 4.0f,
                        1.0f, L"%.0f")) {
        settings_.gpu.frame_buffers = static_cast<int>(std::lround(buffers));
        Touch();
    }
    y += kRow + kGap;

    float limit = static_cast<float>(settings_.gpu.memory_limit_mb);
    const float max_limit = system_.gpu.vram_mb > 1024
                                ? static_cast<float>(system_.gpu.vram_mb)
                                : 24576.0f;
    if (widgets_.Slider(Next(), Row(x, y, width), L"VRAM limit (0 = auto)", limit, 0.0f,
                        max_limit, 512.0f, L"%.0f MB")) {
        settings_.gpu.memory_limit_mb = static_cast<int>(std::lround(limit));
        Touch();
    }
    y += kRow + kGap;

    int priority = settings_.gpu.priority;
    if (widgets_.Choice(Next(), Row(x, y, width), L"Workload priority",
                        {L"Normal", L"High"}, priority)) {
        settings_.gpu.priority = priority;
        Touch();
    }
    return y + kRow;
}

float SettingsControls::Display(float x, float y, float width) {
    std::vector<std::wstring> monitors;
    for (std::size_t i = 0; i < system_.displays.size(); ++i) {
        monitors.push_back(MonitorLabel(system_.displays[i], static_cast<int>(i)));
    }
    if (monitors.empty()) {
        monitors.push_back(L"No display detected");
    }
    int monitor = std::clamp(settings_.display.monitor, 0,
                             static_cast<int>(monitors.size()) - 1);
    if (widgets_.Choice(Next(), Row(x, y, width), L"Monitor", monitors, monitor)) {
        settings_.display.monitor = monitor;
        Touch();
    }
    y += kRow + kGap;

    int refresh = static_cast<int>(settings_.display.refresh);
    if (widgets_.Choice(Next(), Row(x, y, width), L"Refresh rate", kRefresh, refresh)) {
        settings_.display.refresh = static_cast<FrameRate>(std::clamp(refresh, 0, 6));
        Touch();
    }
    y += kRow + kGap;

    if (widgets_.Toggle(Next(), Row(x, y, width), L"V-Sync", settings_.display.vsync)) {
        Touch();
    }
    y += kRow + kGap;
    if (widgets_.Toggle(Next(), Row(x, y, width), L"VRR / G-Sync", settings_.display.vrr)) {
        Touch();
    }
    y += kRow + kGap;

    int depth = BitDepthIndex(settings_.display.bit_depth);
    if (widgets_.Choice(Next(), Row(x, y, width), L"Bit depth", {L"8-bit", L"10-bit"},
                        depth)) {
        settings_.display.bit_depth = depth == 1 ? 10 : 8;
        Touch();
    }
    y += kRow + kGap;

    int format = FormatIndex(settings_.display.output_format);
    if (widgets_.Choice(Next(), Row(x, y, width), L"Colour output",
                        {L"Auto", L"SDR", L"HDR10"}, format)) {
        settings_.display.output_format = FormatAt(format);
        Touch();
    }
    return y + kRow;
}

float SettingsControls::Overlay(float x, float y, float width) {
    if (widgets_.Toggle(Next(), Row(x, y, width), L"Overlay enabled",
                        settings_.overlay.enabled)) {
        Touch();
    }
    y += kRow + kGap;
    if (widgets_.Toggle(Next(), Row(x, y, width), L"Compact layout",
                        settings_.overlay.compact)) {
        Touch();
    }
    y += kRow + kGap;

    float opacity = settings_.overlay.opacity * 100.0f;
    if (widgets_.Slider(Next(), Row(x, y, width), L"Opacity", opacity, 10.0f, 100.0f,
                        1.0f, L"%.0f%%")) {
        settings_.overlay.opacity = opacity / 100.0f;
        Touch();
    }
    y += kRow + kGap;

    int position = settings_.overlay.position;
    if (widgets_.Choice(Next(), Row(x, y, width), L"Position",
                        {L"Top left", L"Top right", L"Bottom left", L"Bottom right"},
                        position)) {
        settings_.overlay.position = position;
        Touch();
    }
    y += kRow + kGap;

    float interval = static_cast<float>(settings_.overlay.interval_ms);
    if (widgets_.Slider(Next(), Row(x, y, width), L"Refresh interval", interval, 50.0f,
                        2000.0f, 50.0f, L"%.0f ms")) {
        settings_.overlay.interval_ms = static_cast<int>(std::lround(interval));
        Touch();
    }
    y += kRow + kGap;

    if (widgets_.Toggle(Next(), Row(x, y, width), L"Performance widget",
                        settings_.overlay.show_performance)) {
        Touch();
    }
    y += kRow + kGap;
    if (widgets_.Toggle(Next(), Row(x, y, width), L"GPU widget",
                        settings_.overlay.show_gpu)) {
        Touch();
    }
    y += kRow + kGap;
    if (widgets_.Toggle(Next(), Row(x, y, width), L"Streaming widget",
                        settings_.overlay.show_stream)) {
        Touch();
    }
    y += kRow + kGap;
    if (widgets_.Toggle(Next(), Row(x, y, width), L"Processing widget",
                        settings_.overlay.show_processing)) {
        Touch();
    }
    return y + kRow;
}

float SettingsControls::Audio(float x, float y, float width) {
    widgets_.Label(Row(x, y, width), L"Output device: Default endpoint",
                   widgets_.Tokens().text_dim, 13.0f, DWRITE_FONT_WEIGHT_MEDIUM);
    y += kRow + kGap;

    float buffer = static_cast<float>(settings_.audio.buffer_ms);
    if (widgets_.Slider(Next(), Row(x, y, width), L"Buffer", buffer, 5.0f, 200.0f, 1.0f,
                        L"%.0f ms")) {
        settings_.audio.buffer_ms = static_cast<int>(std::lround(buffer));
        Touch();
    }
    y += kRow + kGap;

    float volume = static_cast<float>(settings_.audio.volume);
    if (widgets_.Slider(Next(), Row(x, y, width), L"Volume", volume, 0.0f, 100.0f, 1.0f,
                        L"%.0f%%")) {
        settings_.audio.volume = static_cast<int>(std::lround(volume));
        Touch();
    }
    y += kRow + kGap;

    if (widgets_.Toggle(Next(), Row(x, y, width), L"Mute", settings_.audio.mute)) {
        Touch();
    }
    return y + kRow;
}

float SettingsControls::Controller(float x, float y, float width) {
    if (widgets_.Toggle(Next(), Row(x, y, width), L"Controller input",
                        settings_.controller.enabled)) {
        Touch();
    }
    y += kRow + kGap;
    if (widgets_.Toggle(Next(), Row(x, y, width), L"Haptics",
                        settings_.controller.haptics)) {
        Touch();
    }
    y += kRow + kGap;
    if (widgets_.Toggle(Next(), Row(x, y, width), L"Adaptive triggers",
                        settings_.controller.adaptive_triggers)) {
        Touch();
    }
    y += kRow + kGap;

    float rumble = static_cast<float>(settings_.controller.rumble_strength);
    if (widgets_.Slider(Next(), Row(x, y, width), L"Rumble strength", rumble, 0.0f, 5.0f,
                        1.0f, L"%.0f")) {
        settings_.controller.rumble_strength = static_cast<int>(std::lround(rumble));
        Touch();
    }
    y += kRow + kGap;

    int lightbar = settings_.controller.lightbar;
    if (widgets_.Choice(Next(), Row(x, y, width), L"Lightbar",
                        {L"Off", L"Blue", L"Red", L"Green", L"White"}, lightbar)) {
        settings_.controller.lightbar = lightbar;
        Touch();
    }
    return y + kRow;
}

float SettingsControls::Advanced(float x, float y, float width) {
    if (widgets_.Toggle(Next(), Row(x, y, width), L"Experimental features",
                        settings_.advanced.experimental)) {
        Touch();
    }
    y += kRow + kGap;
    if (widgets_.Toggle(Next(), Row(x, y, width), L"Debug log",
                        settings_.advanced.debug_log)) {
        Touch();
    }
    y += kRow + kGap;

    int level = static_cast<int>(settings_.advanced.log_level);
    if (widgets_.Choice(Next(), Row(x, y, width), L"Log level",
                        {L"Trace", L"Debug", L"Info", L"Warning", L"Error"}, level)) {
        settings_.advanced.log_level = static_cast<LogLevel>(std::clamp(level, 0, 4));
        Touch();
    }
    y += kRow + kGap;

    if (widgets_.Toggle(Next(), Row(x, y, width), L"GPU presentation",
                        settings_.advanced.prefer_gpu_present)) {
        Touch();
    }
    return y + kRow;
}

}  // namespace brp::next
