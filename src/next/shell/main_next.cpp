#include "shell/next_app.h"

#include <windows.h>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    brp::next::NextApp app;
    return app.Run();
}
