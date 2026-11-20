#pragma once

#include "core/models.h"

namespace brp::next {

// How a settings change can actually reach the running engines. The UI and the
// overlay use the same classification, so neither can claim a live change when
// the component only reads the value at stream start.
enum class ApplyScope {
    Immediate,
    FrameBoundary,
    PipelineReconfiguration,
    StreamRestart,
    ApplicationRestart,
};

const wchar_t* ApplyScopeLabel(ApplyScope scope);
bool ScopeAppliesLive(ApplyScope scope);

ApplyScope ClassifySettingsChange(const AppSettings& before, const AppSettings& after);

}  // namespace brp::next
