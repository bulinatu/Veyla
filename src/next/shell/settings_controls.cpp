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
const std::vector<std::wstring> kRefresh = {L"Match", L"60", L"90", L"120",
                                             L"144",   L"165", L"240"};

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
    if (widgets_.Toggle(Next(), Row(x, y, width), L"Notifications",
                        settings_.general.notifications)) {
        Touch();
    }
    y += kRow + kGap;
    widgets_.Label(Rect(x, y, x + width, y + 40),
                   L"Settings are written to the application data folder and validated "
                   L"before they reach the running engines.",
                   widgets_.Tokens().text_faint, 10.5f, DWRITE_FONT_WEIGHT_MEDIUM);
    return y + 44.0f;
}
float SettingsControls::Streaming(float x, float y, float width) {
    int fps = settings_.streaming.fps == 30 ? 0 : 1;
    if (widgets_.Choice(Next(), Row(x, y, width), L"Source frame rate",
                        {L"30 fps", L"60 fps"}, fps)) {
        settings_.streaming.fps = fps == 0 ? 30 : 60;
        Touch();
    }
    y += kRow + kGap;

    int buffer = std::clamp(settings_.streaming.frame_buffer, 0, 3);
    if (widgets_.Choice(Next(), Row(x, y, width), L"Frame buffer",
                        {L"Off (lowest latency)", L"1 frame (smooth)",
                         L"2 frames", L"3 frames"},
                        buffer)) {
        settings_.streaming.frame_buffer = buffer;
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


    int decoder = DecoderIndex(settings_.streaming.decoder);
    if (widgets_.Choice(Next(), Row(x, y, width), L"Decoder",
                        {L"Auto", L"D3D11VA", L"D3D12VA", L"Vulkan", L"CUDA",
                         L"Software"},
                        decoder)) {
        settings_.streaming.decoder = DecoderAt(decoder);
        Touch();
    }
    y += kRow + kGap;

    widgets_.Label(Rect(x, y, x + width, y + 52),
                   L"60 fps is the low-latency choice: the camera and encoder update every "
                   L"16.7 ms instead of 33 ms. A one-frame buffer absorbs a late decode "
                   L"at the cost of one source interval. The decoder setting applies when "
                   L"the next session starts.",
                   widgets_.Tokens().text_faint, 10.5f, DWRITE_FONT_WEIGHT_MEDIUM);
    return y + 52.0f;
}
float SettingsControls::Video(float x, float y, float width) {
    int target = settings_.video.render_target == 1080
                     ? 1
                     : (settings_.video.render_target == 1440
                            ? 2
                            : (settings_.video.render_target == 2160 ? 3 : 0));
    if (widgets_.Choice(Next(), Row(x, y, width), L"Render target",
                        {L"Window / fullscreen", L"1080p", L"1440p", L"2160p"},
                        target)) {
        settings_.video.render_target =
            target == 1 ? 1080 : (target == 2 ? 1440 : (target == 3 ? 2160 : 0));
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

    widgets_.Label(Rect(x, y, x + width, y + 40),
                   L"The watch surface is V-Synced to the swapchain so the loop cannot "
                   L"spin and starve streaming threads. HDR output is not offered.",
                   widgets_.Tokens().text_faint, 10.5f, DWRITE_FONT_WEIGHT_MEDIUM);
    return y + 44.0f;
}
float SettingsControls::SuperResolution(float x, float y, float width) {
    const bool sr_available = system_.dlss_sr;
    if (widgets_.Toggle(Next(), Row(x, y, width), L"Super resolution",
                        settings_.ai.super_resolution, sr_available)) {
        Touch();
    }
    y += kRow + kGap;

    int sr_output = ResolutionIndex(settings_.ai.sr_output);
    if (widgets_.Choice(Next(), Row(x, y, width), L"SR output", kResolutions, sr_output,
                        settings_.ai.super_resolution &&
                            settings_.ai.sr_quality < 5)) {
        settings_.ai.sr_output = ResolutionAt(sr_output);
        Touch();
    }
    y += kRow + kGap;

    int quality = settings_.ai.sr_quality;
    if (widgets_.Choice(Next(), Row(x, y, width), L"SR quality",
                        {L"Ultra Performance", L"Performance", L"Balanced", L"Quality",
                         L"Ultra Quality", L"DLAA"},
                        quality, settings_.ai.super_resolution)) {
        settings_.ai.sr_quality = quality;
        if (quality >= 5) {
            settings_.ai.sr_output = Resolution::Native;
        }
        Touch();
    }
    y += kRow + kGap;

    widgets_.Label(Row(x, y, width),
                   sr_available ? L"Backend: DLSS Super Resolution (NGX)"
                                : L"Backend: not installed (no nvngx_dlss.dll)",
                   sr_available ? widgets_.Tokens().success : widgets_.Tokens().text_faint,
                   12.0f, DWRITE_FONT_WEIGHT_MEDIUM);
    y += kRow + kGap + 2.0f;

    if (settings_.ai.super_resolution && settings_.ai.sr_quality < 5) {
        static const int pct[6] = {33, 50, 58, 66, 77, 100};
        const int index = std::clamp(settings_.ai.sr_quality, 0, 5);
        const int out_w = settings_.ai.sr_output == Resolution::R2160 ? 3840 : 2560;
        const int out_h = settings_.ai.sr_output == Resolution::R2160 ? 2160 : 1440;
        const int in_w = std::max(2, (out_w * pct[index] / 100) / 2 * 2);
        const int in_h = std::max(2, (out_h * pct[index] / 100) / 2 * 2);
        wchar_t scale[160] = {};
        std::swprintf(scale, 160, L"Render scale %d%%  |  %dx%d -> %dx%d",
                      pct[index], in_w, in_h, out_w, out_h);
        widgets_.Label(Rect(x, y, x + width, y + 24), scale,
                       widgets_.Tokens().text_dim, 10.5f, DWRITE_FONT_WEIGHT_MEDIUM);
        y += 28.0f;
    }

    const bool dlaa = settings_.ai.sr_quality >= 5 ||
                      settings_.ai.sr_output == Resolution::R1080;
    widgets_.Label(
        Rect(x, y, x + width, y + 40),
        dlaa ? L"DLAA runs at the stream resolution and feeds the generator at "
               L"1080p: the lowest-latency DLSS-G path."
             : L"SR is applied to the decoded frame and read back before frame "
               L"generation. At 1440p expect about 100 generated fps.",
        widgets_.Tokens().text_faint, 10.5f, DWRITE_FONT_WEIGHT_MEDIUM);
    y += 44.0f;
    return y;
}

float SettingsControls::FrameGeneration(float x, float y, float width) {
    std::vector<AiBackend> backends = {AiBackend::DlssFg, AiBackend::Rife};
    std::vector<std::wstring> labels = {L"DLSS-G (NVIDIA)", L"RIFE (TensorRT)"};
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
        if (widgets_.Choice(Next(), Row(x, y, width), L"Backend", labels,
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
                       widgets_.Tokens().text_faint, 12.0f,
                       DWRITE_FONT_WEIGHT_MEDIUM);
        y += kRow + kGap;
    }

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
                           L"Heavy engine is not built; Lite is used until the heavy "
                           L"engine is generated.",
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

    if (settings_.ai.frame_generation) {
        widgets_.Label(Rect(x, y, x + width, y + 36),
                       L"Frame generation adds at least one source interval of latency. "
                       L"Disable it for the lowest input-to-camera delay.",
                       widgets_.Tokens().warning, 10.5f, DWRITE_FONT_WEIGHT_MEDIUM);
        y += 38.0f;
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

float SettingsControls::Ai(float x, float y, float width) {
    y = SuperResolution(x, y, width);
    y = FrameGeneration(x, y, width);
    return y;
}
float SettingsControls::Gpu(float x, float y, float width) {
    widgets_.Label(Row(x, y, width),
                   system_.gpu.present ? (L"Adapter: " + Widen(system_.gpu.name)).c_str()
                                       : L"Adapter: none detected",
                   widgets_.Tokens().text, 13.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    y += kRow + kGap - 6.0f;
    widgets_.Label(Row(x, y, width),
                   (std::to_wstring(system_.gpu.vram_mb / 1024) + L" GB VRAM  |  " +
                    (system_.gpu.driver.empty()
                         ? std::wstring(L"driver unknown")
                         : L"driver " + Widen(system_.gpu.driver)))
                       .c_str(),
                   widgets_.Tokens().text_dim, 11.0f, DWRITE_FONT_WEIGHT_MEDIUM);
    y += kRow + kGap - 6.0f;
    widgets_.Label(Row(x, y, width),
                   (std::wstring(L"CUDA: ") +
                    (system_.cuda ? L"driver present" : L"missing") +
                    L"  |  TensorRT: " +
                    (system_.tensorrt_runtime ? L"runtime present" : L"missing"))
                       .c_str(),
                   widgets_.Tokens().text_faint, 11.0f, DWRITE_FONT_WEIGHT_MEDIUM);
    y += kRow + kGap;
    widgets_.Label(Rect(x, y, x + width, y + 42),
                   L"Inference and presentation use the adapter owned by the process. "
                   L"Per-control adapter selection is not exposed in 1.0.",
                   widgets_.Tokens().text_faint, 10.5f, DWRITE_FONT_WEIGHT_MEDIUM);
    return y + 46.0f;
}
float SettingsControls::Display(float x, float y, float width) {
    if (widgets_.Toggle(Next(), Row(x, y, width), L"Fullscreen on play",
                        settings_.display.fullscreen_on_play)) {
        Touch();
    }
    y += kRow + kGap;
    widgets_.Label(Rect(x, y, x + width, y + 42),
                   L"Fullscreen uses the monitor that contains the window. F11 toggles "
                   L"borderless fullscreen at any time.",
                   widgets_.Tokens().text_faint, 10.5f, DWRITE_FONT_WEIGHT_MEDIUM);
    return y + 46.0f;
}
float SettingsControls::Overlay(float x, float y, float width) {
    if (widgets_.Toggle(Next(), Row(x, y, width), L"Overlay enabled",
                        settings_.overlay.enabled)) {
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

    float scale = static_cast<float>(settings_.overlay.scale);
    if (widgets_.Slider(Next(), Row(x, y, width), L"Scale", scale, 75.0f, 200.0f,
                        5.0f, L"%.0f%%")) {
        settings_.overlay.scale = static_cast<int>(std::lround(scale));
        Touch();
    }
    y += kRow + kGap;

    int side = settings_.overlay.side;
    if (widgets_.Choice(Next(), Row(x, y, width), L"Screen side",
                        {L"Left", L"Right"}, side)) {
        settings_.overlay.side = side;
        Touch();
    }
    y += kRow + kGap;

    int vertical = std::clamp(settings_.overlay.position, 0, 2);
    if (widgets_.Choice(Next(), Row(x, y, width), L"Vertical anchor",
                        {L"Top", L"Middle", L"Bottom"}, vertical)) {
        settings_.overlay.position = vertical;
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
    widgets_.Label(Row(x, y, width), L"Output device: Windows default endpoint",
                   widgets_.Tokens().text_dim, 12.0f, DWRITE_FONT_WEIGHT_MEDIUM);
    y += kRow + kGap;

    float buffer = static_cast<float>(settings_.audio.buffer_ms);
    if (widgets_.Slider(Next(), Row(x, y, width), L"Buffer", buffer, 5.0f, 200.0f, 1.0f,
                        L"%.0f ms")) {
        settings_.audio.buffer_ms = static_cast<int>(std::lround(buffer));
        Touch();
    }
    y += kRow + kGap;
    widgets_.Label(Rect(x, y, x + width, y + 42),
                   L"The buffer is applied when the session opens. Volume follows the "
                   L"Windows mixer; the stream does not apply a second gain stage.",
                   widgets_.Tokens().text_faint, 10.5f, DWRITE_FONT_WEIGHT_MEDIUM);
    return y + 46.0f;
}
float SettingsControls::Controller(float x, float y, float width) {
    float rumble = static_cast<float>(settings_.controller.rumble_strength);
    if (widgets_.Slider(Next(), Row(x, y, width), L"Rumble strength", rumble, 0.0f, 5.0f,
                        1.0f, L"%.0f")) {
        settings_.controller.rumble_strength = static_cast<int>(std::lround(rumble));
        Touch();
    }
    y += kRow + kGap;
    static const int kRates[4] = {125, 250, 500, 1000};
    int rate = 1;
    for (int i = 0; i < 4; ++i) {
        if (settings_.controller.poll_hz >= kRates[i]) {
            rate = i;
        }
    }
    if (widgets_.Choice(Next(), Row(x, y, width), L"Poll rate",
                        {L"125 Hz", L"250 Hz", L"500 Hz", L"1000 Hz"}, rate)) {
        settings_.controller.poll_hz = kRates[rate];
        Touch();
    }
    y += kRow + kGap;
    widgets_.Label(Rect(x, y, x + width, y + 46),
                   L"1000 Hz is only as fast as the controller and its HID stack; a "
                   L"DualSense on USB is the most consistent. Bluetooth can reach "
                   L"1000 Hz with a good host (DSX does), but the console still sees "
                   L"the network send rate.",
                   widgets_.Tokens().text_faint, 10.0f, DWRITE_FONT_WEIGHT_MEDIUM);
    return y + 50.0f;
}
float SettingsControls::Advanced(float x, float y, float width) {
    int level = static_cast<int>(settings_.advanced.log_level);
    if (widgets_.Choice(Next(), Row(x, y, width), L"Log level",
                        {L"Trace", L"Debug", L"Info", L"Warning", L"Error"}, level)) {
        settings_.advanced.log_level = static_cast<LogLevel>(std::clamp(level, 0, 4));
        Touch();
    }
    y += kRow + kGap;
    widgets_.Label(Rect(x, y, x + width, y + 42),
                   L"The log lives beside the settings file in the application data "
                   L"folder. Diagnostics export never includes credentials or tokens.",
                   widgets_.Tokens().text_faint, 10.5f, DWRITE_FONT_WEIGHT_MEDIUM);
    return y + 46.0f;
}
}  // namespace brp::next
