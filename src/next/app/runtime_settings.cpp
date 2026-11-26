#include "app/runtime_settings.h"

#include <algorithm>

namespace brp::next {

const wchar_t* ApplyScopeLabel(ApplyScope scope) {
    switch (scope) {
        case ApplyScope::Immediate: return L"Applied immediately";
        case ApplyScope::FrameBoundary: return L"Applied at the next frame";
        case ApplyScope::PipelineReconfiguration: return L"Processing pipeline rebuilt";
        case ApplyScope::StreamRestart: return L"Saved; reconnect to apply";
        case ApplyScope::ApplicationRestart: return L"Restart the application";
    }
    return L"Applied";
}

bool ScopeAppliesLive(ApplyScope scope) {
    return scope == ApplyScope::Immediate || scope == ApplyScope::FrameBoundary ||
           scope == ApplyScope::PipelineReconfiguration;
}

ApplyScope ClassifySettingsChange(const AppSettings& before, const AppSettings& after) {
    const auto& a = before;
    const auto& b = after;
    int severity = static_cast<int>(ApplyScope::Immediate);
    auto bump = [&severity](ApplyScope scope) {
        severity = std::max(severity, static_cast<int>(scope));
    };

    // Console/session and stream transport are negotiated when the session
    // starts. Changing them during a stream cannot take effect live.
    if (a.console.name != b.console.name || a.console.address != b.console.address ||
        a.console.fps != b.console.fps ||
        a.console.auto_discover != b.console.auto_discover ||
        a.console.auto_reconnect != b.console.auto_reconnect ||
        a.console.reconnect_delay_ms != b.console.reconnect_delay_ms) {
        bump(ApplyScope::StreamRestart);
    }
    if (a.streaming.input != b.streaming.input || a.streaming.fps != b.streaming.fps ||
        a.streaming.decoder != b.streaming.decoder ||
        a.streaming.bitrate_kbps != b.streaming.bitrate_kbps ||
        a.streaming.hdr != b.streaming.hdr) {
        bump(ApplyScope::StreamRestart);
    }

    // AI stages are rebuilt in place without touching the session.
    if (a.ai.super_resolution != b.ai.super_resolution ||
        a.ai.sr_output != b.ai.sr_output || a.ai.sr_model != b.ai.sr_model ||
        a.ai.sr_quality != b.ai.sr_quality ||
        a.ai.sr_preset != b.ai.sr_preset ||
        a.ai.frame_generation != b.ai.frame_generation ||
        a.ai.fg_backend != b.ai.fg_backend ||
        a.ai.fg_multiplier != b.ai.fg_multiplier ||
        a.ai.rife_model != b.ai.rife_model || a.ai.blend != b.ai.blend ||
        a.ai.motion_safety != b.ai.motion_safety) {
        bump(ApplyScope::PipelineReconfiguration);
    }

    // Presenter target and colour path are rebuilt on the current session.
    if (a.video.output != b.video.output ||
        a.video.render_target != b.video.render_target ||
        a.video.vsync != b.video.vsync || a.video.vrr != b.video.vrr ||
        a.video.hdr != b.video.hdr ||
        a.video.sharpening != b.video.sharpening ||
        a.video.color_space != b.video.color_space) {
        bump(ApplyScope::PipelineReconfiguration);
    }

    if (a.display.monitor != b.display.monitor ||
        a.display.refresh != b.display.refresh ||
        a.display.vsync != b.display.vsync || a.display.vrr != b.display.vrr ||
        a.display.output_format != b.display.output_format ||
        a.display.bit_depth != b.display.bit_depth) {
        bump(ApplyScope::PipelineReconfiguration);
    }
    if (a.display.fullscreen_on_play != b.display.fullscreen_on_play) {
        bump(ApplyScope::Immediate);
    }

    if (a.audio.buffer_ms != b.audio.buffer_ms || a.audio.device != b.audio.device) {
        bump(ApplyScope::StreamRestart);
    }

    // Overlay, theme, notifications and controller feedback are live.
    if (a.overlay.enabled != b.overlay.enabled ||
        a.overlay.compact != b.overlay.compact ||
        a.overlay.opacity != b.overlay.opacity ||
        a.overlay.position != b.overlay.position ||
        a.overlay.scale != b.overlay.scale ||
        a.overlay.interval_ms != b.overlay.interval_ms ||
        a.overlay.show_performance != b.overlay.show_performance ||
        a.overlay.show_gpu != b.overlay.show_gpu ||
        a.overlay.show_stream != b.overlay.show_stream ||
        a.overlay.show_processing != b.overlay.show_processing ||
        a.general.theme != b.general.theme ||
        a.controller.enabled != b.controller.enabled ||
        a.controller.haptics != b.controller.haptics ||
        a.controller.adaptive_triggers != b.controller.adaptive_triggers ||
        a.controller.rumble_strength != b.controller.rumble_strength ||
        a.controller.lightbar != b.controller.lightbar ||
        a.audio.volume != b.audio.volume || a.audio.mute != b.audio.mute) {
        bump(ApplyScope::Immediate);
    }

    if (a.advanced.log_level != b.advanced.log_level) {
        bump(ApplyScope::Immediate);
    }

    bump(ApplyScope::Immediate);
    return static_cast<ApplyScope>(severity);
}

}  // namespace brp::next
