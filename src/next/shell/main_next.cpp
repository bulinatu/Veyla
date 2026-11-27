#include "shell/next_app.h"

#include <windows.h>

#include <string_view>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR command_line, int) {
    // This is a latency-sensitive client, not an idle desktop app: keep it out
    // of Windows' background execution throttling and give its threads a fair
    // share against the shell, browser and recording tools.
    PROCESS_POWER_THROTTLING_STATE power{};
    power.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    power.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    power.StateMask = 0;
    ::SetProcessInformation(::GetCurrentProcess(), ProcessPowerThrottling, &power,
                            sizeof(power));
    ::SetPriorityClass(::GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);

    // `--engine` starts the headless core controlled by the Rust/GPUI shell.
    // `--bridge-port N` is the loopback port the shell already picked; without
    // it the engine keeps its own window and the legacy C++ shell.
    bool engine_mode = false;
    unsigned short bridge_port = 0;
    if (command_line != nullptr) {
        const std::wstring_view args(command_line);
        if (args.find(L"--engine") != std::wstring_view::npos) {
            engine_mode = true;
        }
        const std::wstring_view key = L"--bridge-port";
        const std::size_t at = args.find(key);
        if (at != std::wstring_view::npos) {
            std::wstring_view rest = args.substr(at + key.size());
            while (!rest.empty() && rest.front() == L' ') {
                rest.remove_prefix(1);
            }
            unsigned long value = 0;
            for (const wchar_t ch : rest) {
                if (ch < L'0' || ch > L'9') {
                    break;
                }
                value = value * 10 + static_cast<unsigned long>(ch - L'0');
                if (value > 65535) {
                    break;
                }
            }
            if (value > 0 && value <= 65535) {
                bridge_port = static_cast<unsigned short>(value);
            }
        }
    }
    brp::next::NextApp app(engine_mode, bridge_port);
    return app.Run();
}
