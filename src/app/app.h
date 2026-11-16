#pragma once

#include <windows.h>

#include <d2d1_1.h>

#include <vector>

#include "media/pipeline.h"
#include "session/profile.h"
#include "ui/ui_renderer.h"

namespace brp {

struct GameEntry {
    const wchar_t* title;
    const wchar_t* meta;
    unsigned accent;  // 0xRRGGBB
};

class App {
public:
    bool Init(HWND hwnd, int width, int height);
    void Resize(int width, int height);
    void Frame(double time_seconds);
    void HandleKey(int vk);
    bool quit_requested() const { return quit_; }

private:
    enum class Screen { Home, Settings, Stream };

    void RenderHome(double t);
    void RenderSettings(double t);
    void RenderStream(double t);

    void RenderTopBar();
    void DrawTile(const D2D1_RECT_F& rect, const GameEntry& game, float alpha, bool selected);
    void DrawPill(float center_x, float top, const wchar_t* label, float width);
    void DrawHints(const wchar_t* hints);
    void ChangeSetting(int direction);

    HWND hwnd_ = nullptr;
    int width_ = 0;
    int height_ = 0;

    Presenter presenter_;
    ui::Renderer ui_;

    Screen screen_ = Screen::Home;
    int selected_ = 0;
    float scroll_ = 0.0f;
    int setting_row_ = 0;
    bool hud_ = true;
    int upscale_ = 1;
    bool quit_ = false;
    bool capture_requested_ = false;
    double last_time_ = -1.0;
    std::vector<GameEntry> games_;

    session::Profile profile_;
    std::string session_note_;
};

}  // namespace brp
