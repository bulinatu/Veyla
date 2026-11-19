#pragma once

#include "app/app_host.h"
#include "app/navigation.h"
#include "core/models.h"
#include "platform/system_info.h"
#include "ui/theme.h"
#include "ui/widgets.h"

#include <memory>
#include <string>
#include <vector>

namespace brp {
class Win32Window;
class Presenter;
namespace ui {
class Renderer;
}
}  // namespace brp

namespace brp::next {

class SettingsControls;
class StreamingService;

class NextApp {
public:
    NextApp();
    ~NextApp();

    int Run();

private:
    bool Init();
    void Resize();
    void Frame();
    void Render();
    void RenderTitleBar(Widgets& widgets);
    void RenderSidebar(Widgets& widgets);
    void RenderHome(Widgets& widgets);
    void RenderConsole(Widgets& widgets);
    void RenderStreaming(Widgets& widgets);
    void RenderAi(Widgets& widgets);
    void RenderDisplay(Widgets& widgets);
    void RenderOverlay(Widgets& widgets);
    void RenderProfiles(Widgets& widgets);
    void RenderDiagnostics(Widgets& widgets);
    void RenderSettings(Widgets& widgets);
    void RenderAbout(Widgets& widgets, float x, float y, float width);

    float ContentX() const;
    float ContentWidth() const;
    float SidebarWidth() const;
    void Apply();
    void RefreshProfiles();
    void Toast(const std::wstring& text, bool error = false);
    void ProbeIfNeeded(double now);
    void HandlePinInput();
    std::wstring ClipboardText() const;
    std::wstring W(const std::string& text) const;
    std::wstring Summary(const Profile& profile) const;

    std::unique_ptr<brp::Win32Window> window_;
    std::unique_ptr<brp::Presenter> presenter_;
    std::unique_ptr<brp::ui::Renderer> renderer_;
    std::unique_ptr<AppHost> host_;
    std::shared_ptr<StreamingService> streaming_;
    AppPaths paths_;
    SystemInfo system_;
    AppSettings settings_;
    NavigationModel navigation_;
    std::vector<Profile> profiles_;

    int settings_category_ = 0;
    int selected_profile_ = -1;
    bool collapsed_ = false;
    std::string pin_;
    bool pin_focus_ = false;
    float scale_ = 1.0f;
    float logical_width_ = 1600.0f;
    float logical_height_ = 900.0f;
    double now_ = 0.0;
    double last_frame_ = 0.0;
    double last_probe_ = -1.0;
    double started_ = 0.0;
    std::wstring toast_text_;
    bool toast_error_ = false;
    double toast_until_ = 0.0;
    std::uint64_t ui_frames_ = 0;
};

}  // namespace brp::next
