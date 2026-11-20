#pragma once

#include "core/models.h"
#include "platform/system_info.h"
#include "ui/widgets.h"

namespace brp::next {

// Shared settings groups used by the dedicated pages and the Settings hub.
// Each function draws a group at (x, y, width) and returns the next y.
class SettingsControls {
public:
    SettingsControls(Widgets& widgets, AppSettings& settings, const SystemInfo& system,
                     bool frame_generation_wired = true);

    void Reset();
    bool Changed() const { return changed_; }

    float General(float x, float y, float width);
    float Streaming(float x, float y, float width);
    float Video(float x, float y, float width);
    float Ai(float x, float y, float width);
    float SuperResolution(float x, float y, float width);
    float FrameGeneration(float x, float y, float width);
    float Gpu(float x, float y, float width);
    float Display(float x, float y, float width);
    float Overlay(float x, float y, float width);
    float Audio(float x, float y, float width);
    float Controller(float x, float y, float width);
    float Advanced(float x, float y, float width);

private:
    int Next() { return ++id_; }
    void Touch() { changed_ = true; }

    Widgets& widgets_;
    AppSettings& settings_;
    const SystemInfo& system_;
    int id_ = 0;
    bool changed_ = false;
    bool frame_generation_wired_ = true;
};

}  // namespace brp::next
