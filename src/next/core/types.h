#pragma once

// Foundation types for BetterRemotePlay Next.
// This module must stay free of Windows, D3D, FFmpeg and UI headers.

#include <cstdint>
#include <string>
#include <string_view>

namespace brp::next {

enum class Page {
    Home,
    Console,
    Streaming,
    AiVideo,
    Display,
    Overlay,
    Profiles,
    Diagnostics,
    Settings,
    About,
};

enum class Theme { Dark, Light };

enum class Resolution {
    Native,
    R1080,
    R1440,
    R2160,
};

enum class FrameRate {
    Match,
    F60,
    F90,
    F120,
    F144,
    F165,
    F240,
};

enum class AiBackend {
    None,
    DlssSr,
    DlssFg,
    NvidiaVfg,
};

enum class QualityPreset {
    Performance,
    Balanced,
    Quality,
    Ultra,
};

enum class LogLevel {
    Trace,
    Debug,
    Info,
    Warning,
    Error,
};

enum class NotificationKind {
    Info,
    Success,
    Warning,
    Error,
};

inline std::string_view ToString(Page page) {
    switch (page) {
        case Page::Home: return "home";
        case Page::Console: return "console";
        case Page::Streaming: return "streaming";
        case Page::AiVideo: return "ai-video";
        case Page::Display: return "display";
        case Page::Overlay: return "overlay";
        case Page::Profiles: return "profiles";
        case Page::Diagnostics: return "diagnostics";
        case Page::Settings: return "settings";
        case Page::About: return "about";
    }
    return "home";
}

inline std::string_view ToString(Theme theme) {
    return theme == Theme::Dark ? "dark" : "light";
}

inline std::string_view ToString(Resolution resolution) {
    switch (resolution) {
        case Resolution::Native: return "native";
        case Resolution::R1080: return "1080p";
        case Resolution::R1440: return "1440p";
        case Resolution::R2160: return "2160p";
    }
    return "native";
}

inline std::string_view ToString(FrameRate rate) {
    switch (rate) {
        case FrameRate::Match: return "match";
        case FrameRate::F60: return "60";
        case FrameRate::F90: return "90";
        case FrameRate::F120: return "120";
        case FrameRate::F144: return "144";
        case FrameRate::F165: return "165";
        case FrameRate::F240: return "240";
    }
    return "match";
}

inline std::string_view ToString(AiBackend backend) {
    switch (backend) {
        case AiBackend::None: return "none";
        case AiBackend::DlssSr: return "dlss-sr";
        case AiBackend::DlssFg: return "dlss-fg";
        case AiBackend::NvidiaVfg: return "nvidia-vfg";
    }
    return "none";
}

inline std::string_view ToString(QualityPreset quality) {
    switch (quality) {
        case QualityPreset::Performance: return "performance";
        case QualityPreset::Balanced: return "balanced";
        case QualityPreset::Quality: return "quality";
        case QualityPreset::Ultra: return "ultra";
    }
    return "balanced";
}

inline std::string_view ToString(LogLevel level) {
    switch (level) {
        case LogLevel::Trace: return "trace";
        case LogLevel::Debug: return "debug";
        case LogLevel::Info: return "info";
        case LogLevel::Warning: return "warning";
        case LogLevel::Error: return "error";
    }
    return "info";
}

}  // namespace brp::next
