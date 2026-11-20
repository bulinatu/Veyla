#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace brp::next {

enum class OverlayAction {
    None,
    Close,
    ToggleControl,
    ToggleSr,
    SrQualityPrev,
    SrQualityNext,
    SrOutputPrev,
    SrOutputNext,
    ToggleFg,
    FgBackendNext,
    FgMultPrev,
    FgMultNext,
    ToggleStats,
    OpacityDown,
    OpacityUp,
    PositionNext,
    ToggleFullscreen,
    ProfilePrev,
    ProfileNext,
    ProfileApply,
    SaveProfile,
    ToggleTheme,
    ScaleDown,
    ScaleUp,
    ResetFrameStats,
};

struct OverlayRow {
    std::wstring label;
    std::wstring value;
    std::wstring detail;
    bool enabled = true;
    bool toggle = false;
    bool toggle_value = false;
    bool stepper = false;
    OverlayAction action = OverlayAction::None;
    OverlayAction previous = OverlayAction::None;
    OverlayAction next = OverlayAction::None;
};

struct OverlayPage {
    std::wstring title;
    std::vector<OverlayRow> rows;
};

struct OverlayPalette {
    std::uint32_t background = 0x120A20;
    std::uint32_t panel = 0x1A1029;
    std::uint32_t panel_alt = 0x241634;
    std::uint32_t border = 0x3A2A4E;
    std::uint32_t text = 0xF7F3FF;
    std::uint32_t text_dim = 0xD3C8E6;
    std::uint32_t text_faint = 0x9A8CB4;
    std::uint32_t accent = 0xB072FF;
    std::uint32_t accent_warm = 0xFF6BD6;
    std::uint32_t success = 0x57D68A;
    std::uint32_t warning = 0xE0A23A;
    std::uint32_t error = 0xE0617A;
};

// The overlay is a view over the same AppSettings/runtime snapshot the main
// window uses. It never touches GPU or session objects: every interactive row
// carries an OverlayAction which the shell applies through its normal settings
// transaction.
struct OverlayView {
    std::vector<OverlayPage> pages;
    int tab = 0;
    std::wstring title = L"Control center";
    std::wstring subtitle;
    std::wstring status;
    bool status_error = false;
    bool live = false;
    bool expanded = false;
    OverlayPalette palette;
};

}  // namespace brp::next
