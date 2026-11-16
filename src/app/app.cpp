#include "app/app.h"

#include "ui/theme.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace brp;
namespace th = brp::theme;

namespace {

std::wstring Widen(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                        nullptr, 0);
    std::wstring wide(static_cast<size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), wide.data(), n);
    return wide;
}

constexpr int kSettingCount = 6;

const wchar_t* kUpscalerNames[3] = {L"Bilinear", L"Catmull-Rom", L"Catmull-Rom + Sharpen"};

}  // namespace

bool App::Init(HWND hwnd, int width, int height) {
    hwnd_ = hwnd;
    width_ = width;
    height_ = height;

    games_ = {
        {L"Neon Drift", L"Racing  ·  4h played", 0x2E6BFF},
        {L"Starfall", L"Action RPG  ·  12h played", 0xB14BFF},
        {L"Iron Vale", L"Adventure  ·  2h played", 0xFF7A45},
        {L"Deep Signal", L"Horror  ·  6h played", 0x14B8A6},
        {L"Velocity Zero", L"Arcade  ·  1h played", 0xFFC145},
        {L"Hollow Crown", L"Strategy  ·  21h played", 0xEF476F},
        {L"Skyline 9", L"Shooter  ·  9h played", 0x4DA3FF},
        {L"Paper Moons", L"Puzzle  ·  30m played", 0x8AC926},
    };

    PresenterConfig cfg{};
    cfg.output_width = width;
    cfg.output_height = height;
    cfg.source_width = 1920;
    cfg.source_height = 1080;
    cfg.vsync = true;
    cfg.allow_tearing = true;
    cfg.upscale_mode = upscale_;

    if (!presenter_.Init(hwnd, cfg)) {
        return false;
    }
    if (!ui_.Init(presenter_.device())) {
        return false;
    }
    return ui_.BindSwapchain(presenter_.backbuffer());
}

void App::Resize(int width, int height) {
    if (width <= 0 || height <= 0) {
        return;
    }

    // Order matters: D2D holds a reference to the backbuffer.
    ui_.ReleaseTarget();
    if (!presenter_.Resize(width, height)) {
        return;
    }
    width_ = width;
    height_ = height;
    ui_.BindSwapchain(presenter_.backbuffer());
}

void App::HandleKey(int vk) {
    if (vk == VK_F12) {
        capture_requested_ = true;
        return;
    }

    switch (screen_) {
        case Screen::Home:
            if (vk == VK_LEFT || vk == 'A') {
                selected_ = std::max(0, selected_ - 1);
            } else if (vk == VK_RIGHT || vk == 'D') {
                selected_ = std::min(static_cast<int>(games_.size()) - 1, selected_ + 1);
            } else if (vk == VK_RETURN) {
                screen_ = Screen::Stream;
            } else if (vk == 'S' || vk == VK_TAB) {
                screen_ = Screen::Settings;
                setting_row_ = 0;
            } else if (vk == VK_ESCAPE) {
                quit_ = true;
            }
            break;

        case Screen::Settings:
            if (vk == VK_UP) {
                setting_row_ = std::max(0, setting_row_ - 1);
            } else if (vk == VK_DOWN) {
                setting_row_ = std::min(kSettingCount - 1, setting_row_ + 1);
            } else if (vk == VK_LEFT) {
                ChangeSetting(-1);
            } else if (vk == VK_RIGHT) {
                ChangeSetting(+1);
            } else if (vk == VK_ESCAPE || vk == 'S') {
                screen_ = Screen::Home;
            }
            break;

        case Screen::Stream:
            if (vk == VK_ESCAPE) {
                screen_ = Screen::Home;
            } else if (vk == 'H') {
                hud_ = !hud_;
            } else if (vk >= '1' && vk <= '3') {
                upscale_ = vk - '1';
                presenter_.SetUpscaleMode(upscale_);
            }
            break;
    }
}

void App::ChangeSetting(int direction) {
    if (setting_row_ == 0) {
        upscale_ = (upscale_ + direction + 3) % 3;
        presenter_.SetUpscaleMode(upscale_);
    } else if (setting_row_ == 1) {
        if (direction != 0) {
            hud_ = !hud_;
        }
    }
}

void App::Frame(double time_seconds) {
    double dt = last_time_ < 0.0 ? 1.0 / 60.0 : time_seconds - last_time_;
    last_time_ = time_seconds;
    dt = std::clamp(dt, 0.0, 0.1);

    const float target = static_cast<float>(selected_);
    scroll_ += (target - scroll_) * (1.0f - std::exp(-16.0f * static_cast<float>(dt)));

    if (screen_ == Screen::Stream) {
        presenter_.Render(time_seconds);
    } else {
        const float bg[4] = {0.010f, 0.016f, 0.028f, 1.0f};
        presenter_.Clear(bg);
    }

    ui_.BeginFrame();
    switch (screen_) {
        case Screen::Home:
            RenderHome(time_seconds);
            break;
        case Screen::Settings:
            RenderSettings(time_seconds);
            break;
        case Screen::Stream:
            RenderStream(time_seconds);
            break;
    }
    ui_.EndFrame();

    if (capture_requested_) {
        capture_requested_ = false;
        const char* path = "app-home.bmp";
        if (screen_ == Screen::Settings) {
            path = "app-settings.bmp";
        } else if (screen_ == Screen::Stream) {
            path = "app-stream.bmp";
        }
        if (presenter_.CaptureToBmp(path)) {
            std::printf("captured %s\n", path);
        } else {
            std::fprintf(stderr, "capture failed: %s\n", path);
        }
    }
    presenter_.PresentNow(true);
}

void App::RenderTopBar() {
    const float w = static_cast<float>(width_);

    ui_.Text(L"BETTERREMOTEPLAY", {48.0f, 26.0f, 480.0f, 52.0f}, 14.0f, th::Rgba(th::kInk, 0.92f),
             DWRITE_FONT_WEIGHT_SEMI_BOLD);
    ui_.Text(L"DEMO LIBRARY", {48.0f, 52.0f, 480.0f, 74.0f}, 11.0f, th::Rgba(th::kMuted, 0.9f));

    ui_.FillCircle(w - 238.0f, 44.0f, 4.0f, th::Rgba(th::kOffline));
    ui_.Text(L"Console offline", {w - 226.0f, 30.0f, w - 120.0f, 58.0f}, 12.0f,
             th::Rgba(th::kMuted));

    SYSTEMTIME st{};
    ::GetLocalTime(&st);
    wchar_t clock[16];
    swprintf_s(clock, L"%02d:%02d", st.wHour, st.wMinute);
    ui_.Text(clock, {w - 140.0f, 28.0f, w - 90.0f, 60.0f}, 20.0f, th::Rgba(th::kInk, 0.92f),
             DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_TRAILING);

    ui_.FillCircle(w - 54.0f, 44.0f, 18.0f, th::Rgba(0x253046));
    ui_.Text(L"C", {w - 72.0f, 26.0f, w - 36.0f, 62.0f}, 15.0f, th::Rgba(th::kInk),
             DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
}

void App::DrawTile(const D2D1_RECT_F& rect, const GameEntry& game, float alpha, bool selected) {
    // Drop shadow
    ui_.FillRounded({rect.left + 4.0f, rect.top + 12.0f, rect.right + 4.0f, rect.bottom + 12.0f},
                    22.0f, th::Rgba(0x000000, 0.40f * alpha));

    const float w = rect.right - rect.left;
    const float h = rect.bottom - rect.top;

    ui_.PushRoundedClip(rect, 22.0f);
    ui_.FillLinearV(rect, th::Rgba(th::Scale(game.accent, 0.62f), alpha),
                    th::Rgba(0x05070D, alpha));

    ui_.FillCircle(rect.left + w * 0.80f, rect.top + h * 0.24f, w * 0.58f,
                   th::Rgba(game.accent, 0.18f * alpha));
    ui_.FillCircle(rect.left + w * 0.16f, rect.top + h * 0.74f, w * 0.42f,
                   th::Rgba(0xFFFFFF, 0.05f * alpha));
    ui_.FillCircle(rect.left + w * 0.52f, rect.top + h * 0.50f, w * 0.10f,
                   th::Rgba(0xFFFFFF, 0.10f * alpha));

    wchar_t index[8];
    swprintf_s(index, L"%02d", static_cast<int>(&game - games_.data()) + 1);
    ui_.Text(index, {rect.left + 22.0f, rect.top + 12.0f, rect.right - 22.0f, rect.top + 66.0f},
             44.0f, th::Rgba(0xFFFFFF, 0.10f * alpha), DWRITE_FONT_WEIGHT_SEMI_BOLD);

    ui_.Text(game.title, {rect.left + 22.0f, rect.bottom - 62.0f, rect.right - 22.0f,
                          rect.bottom - 26.0f},
             16.0f, th::Rgba(th::kInk, alpha), DWRITE_FONT_WEIGHT_SEMI_BOLD);
    ui_.PopClip();

    ui_.StrokeRounded(rect, 22.0f, th::Rgba(0xFFFFFF, (selected ? 0.30f : 0.09f) * alpha), 1.0f);

    if (selected) {
        ui_.StrokeRounded({rect.left - 6.0f, rect.top - 6.0f, rect.right + 6.0f, rect.bottom + 6.0f},
                          28.0f, th::Rgba(game.accent, 0.60f), 2.0f);
    }
}

void App::DrawPill(float center_x, float top, const wchar_t* label, float width) {
    const D2D1_RECT_F r{center_x - width * 0.5f, top, center_x + width * 0.5f, top + 46.0f};
    ui_.FillRounded(r, 23.0f, th::Rgba(th::kAccent, 0.16f));
    ui_.StrokeRounded(r, 23.0f, th::Rgba(th::kAccent, 0.85f), 1.5f);

    const float ty = top + 23.0f;
    const float tx = r.left + 30.0f;
    ui_.FillTriangle({tx, ty - 7.0f}, {tx, ty + 7.0f}, {tx + 11.0f, ty}, th::Rgba(th::kInk));

    ui_.Text(label, {r.left + 50.0f, r.top, r.right - 14.0f, r.bottom}, 15.0f, th::Rgba(th::kInk),
             DWRITE_FONT_WEIGHT_SEMI_BOLD);
}

void App::DrawHints(const wchar_t* hints) {
    ui_.Text(hints, {0.0f, static_cast<float>(height_) - 44.0f, static_cast<float>(width_),
                     static_cast<float>(height_) - 16.0f},
             12.0f, th::Rgba(th::kMuted, 0.85f), DWRITE_FONT_WEIGHT_NORMAL,
             DWRITE_TEXT_ALIGNMENT_CENTER);
}

void App::RenderHome(double t) {
    (void)t;
    const float w = static_cast<float>(width_);
    const float h = static_cast<float>(height_);

    ui_.FillLinearV({0.0f, 0.0f, w, h}, th::Rgba(th::kBgTop), th::Rgba(th::kBgBottom));

    const GameEntry& game = games_[static_cast<size_t>(selected_)];
    ui_.FillRadial(w * 0.50f, h * 0.55f, w * 0.62f, th::Rgba(game.accent, 0.20f),
                   th::Rgba(game.accent, 0.0f));
    ui_.FillRadial(w * 0.16f, h * 0.18f, w * 0.38f, th::Rgba(0x2B4C8C, 0.14f),
                   th::Rgba(0x2B4C8C, 0.0f));

    RenderTopBar();

    const float tile_h = std::clamp(h * 0.44f, 240.0f, 420.0f);
    const float tile_w = tile_h * 0.75f;
    const float gap = tile_h * 0.065f;
    const float center_y = h * 0.50f;

    std::vector<int> order(games_.size());
    for (size_t i = 0; i < order.size(); ++i) {
        order[i] = static_cast<int>(i);
    }
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        return std::fabs(static_cast<float>(a) - scroll_) > std::fabs(static_cast<float>(b) - scroll_);
    });

    for (int idx : order) {
        const float d = static_cast<float>(idx) - scroll_;
        if (std::fabs(d) > 3.2f) {
            continue;
        }
        const float k = std::min(std::fabs(d), 3.0f);
        const float scale = 1.0f - 0.055f * k;
        const float tw = tile_w * scale;
        const float th = tile_h * scale;
        const float x = w * 0.5f + d * (tile_w + gap) - tw * 0.5f;
        const float y = center_y - th * 0.5f;
        DrawTile({x, y, x + tw, y + th}, games_[static_cast<size_t>(idx)], 1.0f - 0.30f * k,
                 idx == selected_);
    }

    const float rail_bottom = center_y + tile_h * 0.5f;
    ui_.Text(game.title, {w * 0.5f - 600.0f, rail_bottom + 24.0f, w * 0.5f + 600.0f,
                          rail_bottom + 72.0f},
             36.0f, th::Rgba(th::kInk), DWRITE_FONT_WEIGHT_SEMI_BOLD,
             DWRITE_TEXT_ALIGNMENT_CENTER);
    ui_.Text(game.meta, {w * 0.5f - 600.0f, rail_bottom + 74.0f, w * 0.5f + 600.0f,
                         rail_bottom + 100.0f},
             15.0f, th::Rgba(th::kMuted), DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);

    DrawPill(w * 0.5f, rail_bottom + 112.0f, L"Stream", 200.0f);

    DrawHints(L"Left / Right   Navigate      Enter   Stream      S   Settings      Esc   Quit");
}

void App::RenderSettings(double t) {
    (void)t;
    const float w = static_cast<float>(width_);
    const float h = static_cast<float>(height_);

    ui_.FillLinearV({0.0f, 0.0f, w, h}, th::Rgba(0x0A1018), th::Rgba(th::kBgBottom));
    ui_.FillRadial(w * 0.5f, -h * 0.1f, w * 0.7f, th::Rgba(0x1B3A66, 0.28f), th::Rgba(0x1B3A66, 0.0f));

    const float panel_w = std::min(1040.0f, w - 96.0f);
    const float x0 = (w - panel_w) * 0.5f;

    ui_.Text(L"Settings", {x0, 84.0f, x0 + panel_w, 146.0f}, 40.0f, th::Rgba(th::kInk),
             DWRITE_FONT_WEIGHT_SEMI_BOLD);
    ui_.Text(L"Changes apply live. Console features unlock after pairing.",
             {x0, 148.0f, x0 + panel_w, 174.0f}, 13.0f, th::Rgba(th::kMuted));

    struct Row {
        const wchar_t* label;
        std::wstring value;
        bool enabled;
    };

    const Row rows[kSettingCount] = {
        {L"Upscaler", kUpscalerNames[upscale_], true},
        {L"Latency HUD", hud_ ? L"On" : L"Off", true},
        {L"Frame generation", L"Off  ·  needs RIFE model", false},
        {L"HDR output", L"Off  ·  waits for PS5", false},
        {L"Bitrate", L"Auto  ·  waits for PS5", false},
        {L"Console", L"Offline  ·  pair in Phase 3", false},
    };

    const float row_h = 62.0f;
    float y = 214.0f;

    for (int i = 0; i < kSettingCount; ++i) {
        const D2D1_RECT_F row_rect{x0, y, x0 + panel_w, y + row_h - 6.0f};
        const bool selected = i == setting_row_;

        if (selected) {
            ui_.FillRounded(row_rect, 12.0f, th::Rgba(0xFFFFFF, 0.06f));
            ui_.FillRounded({x0 + 10.0f, y + 10.0f, x0 + 14.0f, y + row_h - 16.0f}, 2.0f,
                            th::Rgba(th::kAccent, 0.95f));
        }

        const float text_alpha = rows[i].enabled ? 1.0f : 0.45f;
        ui_.Text(rows[i].label, {x0 + 28.0f, y, x0 + panel_w * 0.55f, y + row_h - 6.0f}, 17.0f,
                 th::Rgba(th::kInk, text_alpha), DWRITE_FONT_WEIGHT_NORMAL);
        ui_.Text(rows[i].value.c_str(),
                 {x0 + panel_w * 0.45f, y, x0 + panel_w - 28.0f, y + row_h - 6.0f}, 15.0f,
                 th::Rgba(rows[i].enabled ? th::kInkDim : th::kMuted, text_alpha),
                 DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING);

        y += row_h;
    }

    DrawHints(L"Up / Down   Select      Left / Right   Change      S / Esc   Back");
}

void App::RenderStream(double t) {
    (void)t;
    const float w = static_cast<float>(width_);

    if (hud_) {
        ui_.FillLinearV({0.0f, 0.0f, w, 150.0f}, th::Rgba(0x000000, 0.55f), th::Rgba(0x000000, 0.0f));

        const std::wstring summary = Widen(presenter_.Summary());
        const D2D1_RECT_F chip{24.0f, 24.0f, 24.0f + 620.0f, 24.0f + 46.0f};
        ui_.FillRounded(chip, 10.0f, th::Rgba(0x000000, 0.55f));
        ui_.StrokeRounded(chip, 10.0f, th::Rgba(0xFFFFFF, 0.10f), 1.0f);
        ui_.Text(summary.c_str(),
                 {chip.left + 16.0f, chip.top, chip.right - 16.0f, chip.bottom}, 12.5f,
                 th::Rgba(th::kInk, 0.92f));

        ui_.FillCircle(w - 40.0f, 44.0f, 5.0f, th::Rgba(th::kOnline));
        ui_.Text(L"DEMO", {w - 118.0f, 30.0f, w - 54.0f, 58.0f}, 12.0f, th::Rgba(th::kInk, 0.85f),
                 DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_TRAILING);
    }

    DrawHints(L"Esc   Back      H   Toggle HUD      1 / 2 / 3   Upscaler");
}
