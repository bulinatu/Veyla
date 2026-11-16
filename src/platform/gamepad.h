#pragma once

#include <windows.h>

#include <array>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace brp {

enum PadButton : uint32_t {
    PadUp = 1u << 0,
    PadDown = 1u << 1,
    PadLeft = 1u << 2,
    PadRight = 1u << 3,
    PadCross = 1u << 4,
    PadCircle = 1u << 5,
    PadSquare = 1u << 6,
    PadTriangle = 1u << 7,
    PadL1 = 1u << 8,
    PadR1 = 1u << 9,
    PadL2 = 1u << 10,
    PadR2 = 1u << 11,
    PadL3 = 1u << 12,
    PadR3 = 1u << 13,
    PadShare = 1u << 14,
    PadOptions = 1u << 15,
    PadPS = 1u << 16,
    PadTouchpad = 1u << 17,
    PadMute = 1u << 18,
};

struct TouchPoint {
    bool active = false;
    uint8_t id = 0;
    uint16_t x = 0;
    uint16_t y = 0;
};

struct PadState {
    bool connected = false;
    std::string name;
    bool bluetooth = false;
    uint32_t buttons = 0;
    float lx = 0.0f, ly = 0.0f;  // -1..1, up = positive
    float rx = 0.0f, ry = 0.0f;
    float l2 = 0.0f, r2 = 0.0f;  // 0..1

    // DualSense extras; zero when the source cannot provide them.
    TouchPoint touch[2];
    int16_t gyro[3]{};
    int16_t accel[3]{};
    int battery_percent = -1;
    bool charging = false;
    bool headset = false;
    bool mic_muted = false;
};

// Adaptive trigger effects. Encodings follow the community-documented DualSense
// effect model; the 0x26 layout is cross-checked against a captured USB report.
struct TriggerEffect {
    std::array<uint8_t, 11> bytes{};

    static TriggerEffect Off();
    static TriggerEffect Rigid(uint8_t start, uint8_t force);
    static TriggerEffect Pulse(uint8_t start, uint8_t end, uint8_t force);
    static TriggerEffect Feedback(uint8_t zone, uint8_t strength);
    static TriggerEffect Weapon(uint8_t start, uint8_t end, uint8_t strength);
    static TriggerEffect Vibration(uint8_t frequency, uint8_t amplitude);
    static TriggerEffect Bow(uint8_t start, uint8_t end, uint8_t force, uint8_t snap);
    static TriggerEffect Galloping(uint8_t start, uint8_t end, uint8_t foot1, uint8_t foot2,
                                   uint8_t frequency);
};

struct PadOutput {
    bool lightbar = false;
    uint8_t r = 0, g = 0, b = 0;

    bool player_leds = false;
    uint8_t player_mask = 0;  // bit 0..4

    bool mic_led = false;
    uint8_t mic_state = 0;  // 0 off, 1 on, 2 pulse

    bool rumble = false;
    uint8_t weak = 0;    // right actuator
    uint8_t strong = 0;  // left actuator

    bool left_trigger = false;
    bool right_trigger = false;
    TriggerEffect left;
    TriggerEffect right;

    // Combined DualSense effect-intensity byte (high nibble triggers, low
    // nibble haptics). 0x00 = strong/strong.
    uint8_t intensity = 0x00;
};

// Everything the console pushes back to the pad while streaming: lightbar,
// player LEDs, rumble and adaptive-trigger effects. Values are the raw
// protocol payloads; the pad firmware does the rest.
struct PadConsoleFeedback {
    bool led = false;
    uint8_t r = 0, g = 0, b = 0;

    bool player_leds = false;
    uint8_t player_mask = 0;

    bool rumble = false;
    uint8_t weak = 0;    // right / high frequency
    uint8_t strong = 0;  // left / low frequency

    bool triggers = false;
    TriggerEffect left;
    TriggerEffect right;

    // Nibbles used by newer DualSense firmware: high = trigger intensity,
    // low = haptic intensity. 0x00 = both strong, 0xF = off.
    uint8_t intensity = 0x00;

    uint32_t version = 0;
};

// Controller input plus DualSense output. Two input backends, one state:
//   * raw HID for Sony pads (DualSense / DualShock 4), USB and Bluetooth,
//   * XInput fallback for Xbox pads, virtual pads and Steam Input's Xbox
//     emulation.
class Gamepad {
public:
    enum class Backend {
        Auto,    // native HID first, XInput if no Sony pad
        SonyHid, // native HID only
        XInput,  // Steam Input's Xbox emulation / Xbox pads
    };

    Gamepad() = default;
    ~Gamepad();

    Gamepad(const Gamepad&) = delete;
    Gamepad& operator=(const Gamepad&) = delete;

    void SetBackend(Backend backend);
    Backend backend() const { return backend_; }

    void Poll();

    const PadState& state() const { return state_; }
    PadState snapshot() const;
    uint32_t ConsumePressed();
    bool held(uint32_t mask) const { return (state_.buttons & mask) != 0; }

    bool is_sony() const { return sony_active_; }
    bool is_bluetooth() const { return bluetooth_; }

    // Desired output from the app (settings + local tests). Stored, merged with
    // console feedback and written by PumpOutput().
    void SetOutput(const PadOutput& output);
    void SetConsoleFeedback(const PadConsoleFeedback& feedback);

    // Merges user settings with console feedback and writes a report when
    // something changed or while rumble is active. Cheap when idle.
    bool PumpOutput();

    // Sends lightbar / player LEDs / mic LED / rumble / adaptive trigger state.
    // Returns false when no Sony pad is open.
    bool SendOutput(const PadOutput& output);

private:
    bool TryOpenSony();
    void CloseDevice();
    void StartRead();
    void PumpRead();
    bool WriteReport(const uint8_t* data, size_t size);
    void ParseSony(const uint8_t* data, DWORD length);
    void ApplyState(const PadState& state);
    void PollXInput();

    HANDLE handle_ = INVALID_HANDLE_VALUE;
    HANDLE event_ = nullptr;
    OVERLAPPED overlapped_{};
    HANDLE write_event_ = nullptr;
    OVERLAPPED write_overlapped_{};
    std::vector<uint8_t> buffer_;
    bool read_pending_ = false;
    bool sony_active_ = false;
    bool bluetooth_ = false;
    uint16_t product_id_ = 0;
    uint32_t output_report_len_ = 0;
    std::string name_;
    double retry_at_ = 0.0;

    mutable std::mutex mutex_;
    PadState state_;
    uint32_t pressed_ = 0;
    Backend backend_ = Backend::Auto;

    PadOutput user_output_;
    PadConsoleFeedback console_;
    bool output_dirty_ = true;
    double last_output_ = 0.0;
};

}  // namespace brp
