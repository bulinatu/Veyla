#include "platform/gamepad.h"

#include <hidsdi.h>
#include <hidpi.h>
#include <setupapi.h>
#include <xinput.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace brp {
namespace {

double NowSeconds() {
    return static_cast<double>(::GetTickCount64()) / 1000.0;
}

float StickAxis(uint8_t raw) {
    return std::clamp((static_cast<float>(raw) - 128.0f) / 127.0f, -1.0f, 1.0f);
}

float Deadzone(float value, float dead) {
    if (std::fabs(value) <= dead) {
        return 0.0f;
    }
    const float sign = value < 0.0f ? -1.0f : 1.0f;
    return sign * ((std::fabs(value) - dead) / (1.0f - dead));
}

int16_t ReadI16(const uint8_t* data, int offset, DWORD length) {
    if (offset + 1 >= static_cast<int>(length)) {
        return 0;
    }
    return static_cast<int16_t>(data[offset] | (data[offset + 1] << 8));
}

// Console haptic intensity nibble: 0 strong, 2 medium, 3 weak, 0xF off.
uint8_t ScaleHaptic(uint8_t value, uint8_t level) {
    switch (level) {
        case 0x0: return value;
        case 0x2: return static_cast<uint8_t>(value / 2);
        case 0x3: return static_cast<uint8_t>(value / 3);
        case 0xF: return 0;
        default: return value;
    }
}

uint32_t Crc32Table[256];
bool crc_ready = false;

void InitCrc() {
    if (crc_ready) {
        return;
    }
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) {
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        Crc32Table[i] = c;
    }
    crc_ready = true;
}

// Sony's Bluetooth output reports sign the first 74 bytes with a CRC32 whose
// seed is the byte 0xA2 fed through the raw table after an 0xFFFFFFFF init,
// then inverted. This is exactly hid-playstation.c:
//   crc = crc32_le(0xFFFFFFFF, &seed, 1);
//   crc = ~crc32_le(crc, data, len - 4);
// Using the zlib-style "init = ~seed" instead produced a valid-looking but
// wrong CRC, and the controller silently ignored every Bluetooth report, so
// rumble/LED/trigger effects only worked over USB.
uint32_t Crc32Seed(uint32_t seed, const uint8_t* data, size_t length) {
    InitCrc();
    uint32_t crc = 0xFFFFFFFFu;
    crc = (crc >> 8) ^ Crc32Table[(crc ^ static_cast<uint8_t>(seed)) & 0xFF];
    for (size_t i = 0; i < length; ++i) {
        crc = (crc >> 8) ^ Crc32Table[(crc ^ data[i]) & 0xFF];
    }
    return ~crc;
}

}  // namespace

TriggerEffect TriggerEffect::Off() {
    return {};
}

TriggerEffect TriggerEffect::Rigid(uint8_t start, uint8_t force) {
    TriggerEffect e;
    e.bytes = {0x01, 0x00, start, force, 0, 0, 0, 0, 0, 0, 0};
    return e;
}

TriggerEffect TriggerEffect::Pulse(uint8_t start, uint8_t end, uint8_t force) {
    TriggerEffect e;
    e.bytes = {0x02, 0x00, start, end, force, 0, 0, 0, 0, 0, 0};
    return e;
}

TriggerEffect TriggerEffect::Feedback(uint8_t zone, uint8_t strength) {
    TriggerEffect e;
    e.bytes = {0x21, zone, strength, 0, 0, 0, 0, 0, 0, 0, 0};
    return e;
}

TriggerEffect TriggerEffect::Weapon(uint8_t start, uint8_t end, uint8_t strength) {
    TriggerEffect e;
    e.bytes = {0x25, start, end, strength, 0, 0, 0, 0, 0, 0, 0};
    return e;
}

TriggerEffect TriggerEffect::Vibration(uint8_t frequency, uint8_t amplitude) {
    TriggerEffect e;
    e.bytes = {0x26, frequency, amplitude, 0xFF, 0, 0, 0, 0, 0, 0, 0};
    return e;
}

TriggerEffect TriggerEffect::Bow(uint8_t start, uint8_t end, uint8_t force, uint8_t snap) {
    TriggerEffect e;
    e.bytes = {0x27, start, end, force, snap, 0, 0, 0, 0, 0, 0};
    return e;
}

TriggerEffect TriggerEffect::Galloping(uint8_t start, uint8_t end, uint8_t foot1, uint8_t foot2,
                                       uint8_t frequency) {
    TriggerEffect e;
    e.bytes = {0x23, start, end, foot1, foot2, frequency, 0, 0, 0, 0, 0};
    return e;
}

Gamepad::~Gamepad() {
    CloseDevice();
}

void Gamepad::CloseDevice() {
    read_pending_ = false;
    sony_active_ = false;
    bluetooth_ = false;
    if (handle_ != INVALID_HANDLE_VALUE) {
        ::CancelIoEx(handle_, &overlapped_);
        ::CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
    }
    if (event_) {
        ::CloseHandle(event_);
        event_ = nullptr;
    }
    if (write_event_) {
        ::CloseHandle(write_event_);
        write_event_ = nullptr;
    }
    state_.connected = false;
    state_.buttons = 0;
    state_.name.clear();
    is_edge_ = false;
    firmware_version_ = 0;
    calibration_valid_ = false;
    for (AxisCalibration& axis : calibration_) {
        axis = {};
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        console_ = PadConsoleFeedback{};
        output_dirty_ = true;
    }
}

bool Gamepad::TryOpenSony() {
    GUID hid_guid{};
    ::HidD_GetHidGuid(&hid_guid);

    HDEVINFO set = ::SetupDiGetClassDevsW(&hid_guid, nullptr, nullptr,
                                          DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) {
        return false;
    }

    bool opened = false;

    for (DWORD index = 0; !opened; ++index) {
        SP_DEVICE_INTERFACE_DATA interface_data{};
        interface_data.cbSize = sizeof(interface_data);
        if (!::SetupDiEnumDeviceInterfaces(set, nullptr, &hid_guid, index, &interface_data)) {
            break;
        }

        DWORD required = 0;
        ::SetupDiGetDeviceInterfaceDetailW(set, &interface_data, nullptr, 0, &required, nullptr);
        if (required == 0) {
            continue;
        }

        std::vector<uint8_t> detail_buffer(required);
        auto* detail =
            reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(detail_buffer.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!::SetupDiGetDeviceInterfaceDetailW(set, &interface_data, detail, required, nullptr,
                                                nullptr)) {
            continue;
        }

        HANDLE handle = ::CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                      FILE_FLAG_OVERLAPPED, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            continue;
        }

        HIDD_ATTRIBUTES attributes{};
        attributes.Size = sizeof(attributes);
        if (!::HidD_GetAttributes(handle, &attributes)) {
            ::CloseHandle(handle);
            continue;
        }

        const bool is_dualsense =
            attributes.VendorID == 0x054C &&
            (attributes.ProductID == 0x0CE6 || attributes.ProductID == 0x0DF2);
        const bool is_ds4 = attributes.VendorID == 0x054C &&
                            (attributes.ProductID == 0x05C4 || attributes.ProductID == 0x09CC);
        if (!is_dualsense && !is_ds4) {
            ::CloseHandle(handle);
            continue;
        }

        PHIDP_PREPARSED_DATA preparsed = nullptr;
        if (!::HidD_GetPreparsedData(handle, &preparsed)) {
            ::CloseHandle(handle);
            continue;
        }
        HIDP_CAPS caps{};
        ::HidP_GetCaps(preparsed, &caps);
        ::HidD_FreePreparsedData(preparsed);

        if (caps.InputReportByteLength == 0) {
            ::CloseHandle(handle);
            continue;
        }

        ::HidD_SetNumInputBuffers(handle, 64);
        ::SetFileCompletionNotificationModes(handle, FILE_SKIP_SET_EVENT_ON_HANDLE);

        handle_ = handle;
        product_id_ = attributes.ProductID;
        is_edge_ = is_dualsense && attributes.ProductID == 0x0DF2;
        bluetooth_ = caps.InputReportByteLength != 64;
        output_report_len_ = caps.OutputReportByteLength;
        buffer_.assign(std::max<uint32_t>(caps.InputReportByteLength, 78), 0);
        event_ = ::CreateEventW(nullptr, true, false, nullptr);
        write_event_ = ::CreateEventW(nullptr, true, false, nullptr);
        sony_active_ = true;
        firmware_version_ = 0;
        calibration_valid_ = false;
        name_ = is_dualsense ? (is_edge_ ? "DualSense Edge" : "DualSense") : "DualShock 4";
        if (bluetooth_) {
            name_ += " (Bluetooth)";
        } else {
            name_ += " (USB)";
        }
        if (is_dualsense) {
            ReadFirmware();
            ReadCalibration();
        }
        std::fprintf(stderr,
                     "hid: %s input=%u output=%u (%s) firmware=0x%04X calibration=%s%s\n",
                     name_.c_str(), caps.InputReportByteLength, caps.OutputReportByteLength,
                     bluetooth_ ? "bluetooth" : "usb", firmware_version_,
                     calibration_valid_ ? "ok" : "default", is_edge_ ? " edge" : "");
        state_.connected = true;
        state_.name = name_;
        state_.buttons = 0;
        StartRead();
        opened = true;
    }

    ::SetupDiDestroyDeviceInfoList(set);
    return opened;
}

void Gamepad::ReadFirmware() {
    firmware_version_ = 0;
    if (handle_ == INVALID_HANDLE_VALUE) {
        return;
    }
    uint8_t data[64]{};
    data[0] = 0x20;  // feature report: firmware info
    if (!::HidD_GetFeature(handle_, data, sizeof(data))) {
        std::fprintf(stderr, "hid: firmware feature report failed (%lu)\n",
                     static_cast<unsigned long>(::GetLastError()));
        return;
    }
    firmware_version_ = static_cast<uint16_t>(data[44] | (data[45] << 8));
}

void Gamepad::ReadCalibration() {
    calibration_valid_ = false;
    for (AxisCalibration& axis : calibration_) {
        axis = {};
    }
    if (handle_ == INVALID_HANDLE_VALUE) {
        return;
    }

    uint8_t data[64]{};
    data[0] = 0x05;  // feature report: IMU calibration
    if (!::HidD_GetFeature(handle_, data, sizeof(data))) {
        std::fprintf(stderr, "hid: calibration feature report failed (%lu)\n",
                     static_cast<unsigned long>(::GetLastError()));
        return;
    }

    auto load16 = [&](int offset) {
        return static_cast<int16_t>(data[offset] | (data[offset + 1] << 8));
    };
    const int16_t pitch_bias = load16(1);
    const int16_t yaw_bias = load16(3);
    const int16_t roll_bias = load16(5);
    const int16_t pitch_plus = load16(7);
    const int16_t pitch_minus = load16(9);
    const int16_t yaw_plus = load16(11);
    const int16_t yaw_minus = load16(13);
    const int16_t roll_plus = load16(15);
    const int16_t roll_minus = load16(17);
    const int16_t gyro_speed_plus = load16(19);
    const int16_t gyro_speed_minus = load16(21);
    const int16_t acc_x_plus = load16(23);
    const int16_t acc_x_minus = load16(25);
    const int16_t acc_y_plus = load16(27);
    const int16_t acc_y_minus = load16(29);
    const int16_t acc_z_plus = load16(31);
    const int16_t acc_z_minus = load16(33);

    const int divisions[3] = {pitch_plus - pitch_minus, yaw_plus - yaw_minus,
                              roll_plus - roll_minus};
    if (divisions[0] == 0 || divisions[1] == 0 || divisions[2] == 0) {
        return;
    }
    const float numerator = static_cast<float>(gyro_speed_plus + gyro_speed_minus) * 1024.0f;
    calibration_[0] = {pitch_bias, numerator / divisions[0]};
    calibration_[1] = {yaw_bias, numerator / divisions[1]};
    calibration_[2] = {roll_bias, numerator / divisions[2]};

    auto set_accel = [&](int index, int plus, int minus) {
        const int range = plus - minus;
        if (range == 0) {
            return false;
        }
        calibration_[index].bias = static_cast<int16_t>(plus - range / 2);
        calibration_[index].sensitivity = 2.0f * 8192.0f / static_cast<float>(range);
        return true;
    };
    if (!set_accel(3, acc_x_plus, acc_x_minus) || !set_accel(4, acc_y_plus, acc_y_minus) ||
        !set_accel(5, acc_z_plus, acc_z_minus)) {
        return;
    }

    bool valid = true;
    for (int i = 0; i < 6; ++i) {
        const float divisor = (i < 3) ? 64.0f : 1.0f;
        if (std::abs(calibration_[i].bias) > 1024 ||
            std::fabs(1.0f - calibration_[i].sensitivity / divisor) > 0.5f) {
            valid = false;
            break;
        }
    }
    if (!valid) {
        std::fprintf(stderr, "hid: calibration data rejected (out of range)\n");
        for (AxisCalibration& axis : calibration_) {
            axis = {};
        }
        return;
    }
    calibration_valid_ = true;
    std::fprintf(stderr, "hid: IMU calibrated (gyro %.2f/%.2f/%.2f, accel %.2f/%.2f/%.2f)\n",
                 calibration_[0].sensitivity, calibration_[1].sensitivity,
                 calibration_[2].sensitivity, calibration_[3].sensitivity,
                 calibration_[4].sensitivity, calibration_[5].sensitivity);
}

void Gamepad::StartRead() {
    if (handle_ == INVALID_HANDLE_VALUE || !event_) {
        return;
    }
    ::ZeroMemory(&overlapped_, sizeof(overlapped_));
    overlapped_.hEvent = event_;
    ::ResetEvent(event_);

    const DWORD length = static_cast<DWORD>(buffer_.size());
    if (::ReadFile(handle_, buffer_.data(), length, nullptr, &overlapped_)) {
        read_pending_ = true;
        return;
    }
    if (::GetLastError() == ERROR_IO_PENDING) {
        read_pending_ = true;
        return;
    }
    CloseDevice();
}

void Gamepad::PumpRead() {
    if (handle_ == INVALID_HANDLE_VALUE || !read_pending_) {
        return;
    }

    DWORD bytes = 0;
    if (!::GetOverlappedResult(handle_, &overlapped_, &bytes, FALSE)) {
        if (::GetLastError() == ERROR_IO_INCOMPLETE) {
            return;
        }
        CloseDevice();
        return;
    }

    read_pending_ = false;
    if (bytes > 0) {
        ParseSony(buffer_.data(), bytes);
    }
    StartRead();
}

void Gamepad::ParseSony(const uint8_t* data, DWORD length) {
    const uint8_t report_id = data[0];

    // Three framings exist:
    //   USB 0x01, 64 bytes      — full data
    //   BT  0x01, 10 bytes      — simple mode, no touch/gyro/battery
    //   BT  0x31, 78 bytes      — extended mode, full data
    int base = 0;
    PadState next;

    if (report_id == 0x01 && length >= 60) {
        base = 1;  // USB
        bluetooth_ = false;
    } else if (report_id == 0x01) {
        base = 1;  // BT simple: same relative layout, fewer fields
        bluetooth_ = true;
    } else if (report_id == 0x31) {
        base = 2;  // BT extended
        bluetooth_ = true;
    } else {
        return;
    }

    if (static_cast<int>(length) < base + 10) {
        return;
    }

    const uint8_t* p = data + base;

    next.connected = true;
    next.name = name_;
    next.bluetooth = bluetooth_;
    next.lx = StickAxis(p[0]);
    next.ly = -StickAxis(p[1]);
    next.rx = StickAxis(p[2]);
    next.ry = -StickAxis(p[3]);

    if (base == 1 && bluetooth_ && length <= 11) {
        // Simple BT report: hat/buttons at 4..6, triggers at 7..8.
        const uint8_t b0 = p[4];
        const uint8_t b1 = p[5];
        const uint8_t b2 = p[6];
        next.l2 = static_cast<float>(p[7]) / 255.0f;
        next.r2 = static_cast<float>(p[8]) / 255.0f;

        const int hat = b0 & 0x0F;
        if (hat == 0 || hat == 1 || hat == 7) next.buttons |= PadUp;
        if (hat == 3 || hat == 4 || hat == 5) next.buttons |= PadDown;
        if (hat == 5 || hat == 6 || hat == 7) next.buttons |= PadLeft;
        if (hat == 1 || hat == 2 || hat == 3) next.buttons |= PadRight;
        if (b0 & 0x10) next.buttons |= PadSquare;
        if (b0 & 0x20) next.buttons |= PadCross;
        if (b0 & 0x40) next.buttons |= PadCircle;
        if (b0 & 0x80) next.buttons |= PadTriangle;
        if (b1 & 0x01) next.buttons |= PadL1;
        if (b1 & 0x02) next.buttons |= PadR1;
        if (b1 & 0x04) next.buttons |= PadL2;
        if (b1 & 0x08) next.buttons |= PadR2;
        if (b1 & 0x10) next.buttons |= PadShare;
        if (b1 & 0x20) next.buttons |= PadOptions;
        if (b1 & 0x40) next.buttons |= PadL3;
        if (b1 & 0x80) next.buttons |= PadR3;
        if (b2 & 0x01) next.buttons |= PadPS;
        if (b2 & 0x02) next.buttons |= PadTouchpad;

        ApplyState(next);
        return;
    }

    if (static_cast<int>(length) < base + 52) {
        return;
    }

    next.l2 = static_cast<float>(p[4]) / 255.0f;
    next.r2 = static_cast<float>(p[5]) / 255.0f;

    const uint8_t b0 = p[7];
    const uint8_t b1 = p[8];
    const uint8_t b2 = p[9];

    const int hat = b0 & 0x0F;
    if (hat == 0 || hat == 1 || hat == 7) next.buttons |= PadUp;
    if (hat == 3 || hat == 4 || hat == 5) next.buttons |= PadDown;
    if (hat == 5 || hat == 6 || hat == 7) next.buttons |= PadLeft;
    if (hat == 1 || hat == 2 || hat == 3) next.buttons |= PadRight;

    if (b0 & 0x10) next.buttons |= PadSquare;
    if (b0 & 0x20) next.buttons |= PadCross;
    if (b0 & 0x40) next.buttons |= PadCircle;
    if (b0 & 0x80) next.buttons |= PadTriangle;

    if (b1 & 0x01) next.buttons |= PadL1;
    if (b1 & 0x02) next.buttons |= PadR1;
    if (b1 & 0x04) next.buttons |= PadL2;
    if (b1 & 0x08) next.buttons |= PadR2;
    if (b1 & 0x10) next.buttons |= PadShare;
    if (b1 & 0x20) next.buttons |= PadOptions;
    if (b1 & 0x40) next.buttons |= PadL3;
    if (b1 & 0x80) next.buttons |= PadR3;

    if (b2 & 0x01) next.buttons |= PadPS;
    if (b2 & 0x02) next.buttons |= PadTouchpad;
    if (b2 & 0x04) next.buttons |= PadMute;
    if (is_edge_) {
        if (b2 & 0x10) next.buttons |= PadFnLeft;
        if (b2 & 0x20) next.buttons |= PadFnRight;
        if (b2 & 0x40) next.buttons |= PadPaddleLeft;
        if (b2 & 0x80) next.buttons |= PadPaddleRight;
    }

    // Motion and touch. Offsets are payload-relative: USB raw 16..27,
    // BT extended raw 17..28. The firmware calibration is applied here; the
    // raw values stay available for diagnostics.
    for (int i = 0; i < 3; ++i) {
        const int16_t gyro = ReadI16(p, 15 + i * 2, length - base);
        const int16_t accel = ReadI16(p, 21 + i * 2, length - base);
        next.gyro[i] = gyro;
        next.accel[i] = accel;
        if (calibration_valid_) {
            next.gyro_dps[i] =
                (gyro - calibration_[i].bias) * calibration_[i].sensitivity / 1024.0f;
            next.accel_g[i] = (accel - calibration_[3 + i].bias) *
                              calibration_[3 + i].sensitivity / 8192.0f;
        } else {
            next.gyro_dps[i] = static_cast<float>(gyro) / 1024.0f;
            next.accel_g[i] = static_cast<float>(accel) / 8192.0f;
        }
    }

    for (int i = 0; i < 2; ++i) {
        const int o = 32 + i * 4;
        const uint8_t flags = p[o];
        TouchPoint& point = next.touch[i];
        point.active = (flags & 0x80) == 0;
        point.id = flags & 0x7F;
        point.x = static_cast<uint16_t>(((p[o + 2] & 0x0F) << 8) | p[o + 1]);
        point.y = static_cast<uint16_t>((p[o + 3] << 4) | ((p[o + 2] & 0xF0) >> 4));
    }

    const uint8_t battery = p[52];
    next.battery_percent = (battery & 0x0F) * 10;
    next.charging = ((battery & 0xF0) == 0x10);

    ApplyState(next);
}

void Gamepad::ApplyState(const PadState& next) {
    std::lock_guard<std::mutex> lock(mutex_);
    pressed_ |= (next.buttons & ~state_.buttons);
    state_ = next;
}

void Gamepad::PollXInput() {
    XINPUT_STATE xinput_state{};
    if (::XInputGetState(0, &xinput_state) != ERROR_SUCCESS) {
        if (!sony_active_) {
            state_.connected = false;
            state_.buttons = 0;
        }
        return;
    }

    PadState next;
    next.connected = true;
    next.name = "XInput pad (Steam-compatible)";
    next.buttons = 0;

    const WORD buttons = xinput_state.Gamepad.wButtons;
    if (buttons & XINPUT_GAMEPAD_DPAD_UP) next.buttons |= PadUp;
    if (buttons & XINPUT_GAMEPAD_DPAD_DOWN) next.buttons |= PadDown;
    if (buttons & XINPUT_GAMEPAD_DPAD_LEFT) next.buttons |= PadLeft;
    if (buttons & XINPUT_GAMEPAD_DPAD_RIGHT) next.buttons |= PadRight;
    if (buttons & XINPUT_GAMEPAD_A) next.buttons |= PadCross;
    if (buttons & XINPUT_GAMEPAD_B) next.buttons |= PadCircle;
    if (buttons & XINPUT_GAMEPAD_X) next.buttons |= PadSquare;
    if (buttons & XINPUT_GAMEPAD_Y) next.buttons |= PadTriangle;
    if (buttons & XINPUT_GAMEPAD_LEFT_SHOULDER) next.buttons |= PadL1;
    if (buttons & XINPUT_GAMEPAD_RIGHT_SHOULDER) next.buttons |= PadR1;
    if (buttons & XINPUT_GAMEPAD_LEFT_THUMB) next.buttons |= PadL3;
    if (buttons & XINPUT_GAMEPAD_RIGHT_THUMB) next.buttons |= PadR3;
    if (buttons & XINPUT_GAMEPAD_BACK) next.buttons |= PadShare;
    if (buttons & XINPUT_GAMEPAD_START) next.buttons |= PadOptions;
    // XINPUT_GAMEPAD_GUIDE is not exposed by the public header; value is 0x0400.
    if (buttons & 0x0400) next.buttons |= PadPS;

    next.lx = Deadzone(static_cast<float>(xinput_state.Gamepad.sThumbLX) / 32767.0f, 0.20f);
    next.ly = Deadzone(static_cast<float>(xinput_state.Gamepad.sThumbLY) / 32767.0f, 0.20f);
    next.rx = Deadzone(static_cast<float>(xinput_state.Gamepad.sThumbRX) / 32767.0f, 0.20f);
    next.ry = Deadzone(static_cast<float>(xinput_state.Gamepad.sThumbRY) / 32767.0f, 0.20f);
    next.l2 = static_cast<float>(xinput_state.Gamepad.bLeftTrigger) / 255.0f;
    next.r2 = static_cast<float>(xinput_state.Gamepad.bRightTrigger) / 255.0f;

    if (next.l2 > 0.5f) next.buttons |= PadL2;
    if (next.r2 > 0.5f) next.buttons |= PadR2;

    ApplyState(next);
}

void Gamepad::SetBackend(Backend backend) {
    if (backend_ == backend) {
        return;
    }
    backend_ = backend;
    CloseDevice();
    retry_at_ = 0.0;
}

void Gamepad::Poll() {
    const double now = NowSeconds();

    if (backend_ == Backend::XInput) {
        if (sony_active_) {
            CloseDevice();
        }
        PollXInput();
        return;
    }

    if (handle_ != INVALID_HANDLE_VALUE) {
        PumpRead();
    } else if (now >= retry_at_) {
        TryOpenSony();
        retry_at_ = now + 2.0;
    }

    if (!sony_active_ && backend_ == Backend::Auto) {
        PollXInput();
    }
}

PadState Gamepad::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

uint32_t Gamepad::ConsumePressed() {
    std::lock_guard<std::mutex> lock(mutex_);
    const uint32_t pressed = pressed_;
    pressed_ = 0;
    return pressed;
}

bool Gamepad::WriteReport(const uint8_t* data, size_t size) {
    if (handle_ == INVALID_HANDLE_VALUE || !write_event_) {
        return false;
    }

    // Windows HID requires a write whose length matches the device's output
    // report length. The DualSense Bluetooth interface reports 547 bytes while
    // the actual 0x31 report is 78, so the rest has to be zero padding.
    std::vector<uint8_t> padded;
    if (output_report_len_ > size) {
        padded.assign(output_report_len_, 0);
        std::memcpy(padded.data(), data, size);
        static bool logged_padding = false;
        if (!logged_padding) {
            std::fprintf(stderr, "[hid] output report %zu -> %u bytes (padded)\n", size,
                         output_report_len_);
            logged_padding = true;
        }
        data = padded.data();
        size = output_report_len_;
    }

    ::ZeroMemory(&write_overlapped_, sizeof(write_overlapped_));
    write_overlapped_.hEvent = write_event_;
    ::ResetEvent(write_event_);

    DWORD written = 0;
    BOOL ok = ::WriteFile(handle_, data, static_cast<DWORD>(size), nullptr, &write_overlapped_);
    if (!ok) {
        const DWORD error = ::GetLastError();
        if (error == ERROR_IO_PENDING) {
            if (::WaitForSingleObject(write_event_, 100) == WAIT_OBJECT_0) {
                ok = ::GetOverlappedResult(handle_, &write_overlapped_, &written, FALSE);
            } else {
                ::CancelIoEx(handle_, &write_overlapped_);
                return false;
            }
        } else {
            std::fprintf(stderr, "hid: WriteFile failed (%lu)\n", error);
            return false;
        }
    } else {
        ok = ::GetOverlappedResult(handle_, &write_overlapped_, &written, TRUE);
    }

    return ok != FALSE && written > 0;
}

void Gamepad::SetOutput(const PadOutput& output) {
    std::lock_guard<std::mutex> lock(mutex_);
    user_output_ = output;
    output_dirty_ = true;
}

void Gamepad::SetConsoleFeedback(const PadConsoleFeedback& feedback) {
    std::lock_guard<std::mutex> lock(mutex_);
    console_ = feedback;
    output_dirty_ = true;
}

bool Gamepad::PumpOutput() {
    if (!sony_active_ || handle_ == INVALID_HANDLE_VALUE) {
        return false;
    }

    const double now = NowSeconds();
    PadOutput out;
    bool rumble_active = false;
    bool due = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        out = user_output_;

        // A fixed colour chosen in Settings wins; otherwise follow the console.
        if (!out.lightbar && console_.led) {
            out.lightbar = true;
            out.r = console_.r;
            out.g = console_.g;
            out.b = console_.b;
        }
        if (!out.player_leds && console_.player_leds) {
            out.player_leds = true;
            out.player_mask = console_.player_mask;
        }

        const uint8_t haptic_level = console_.intensity & 0x0F;
        if (!out.rumble && console_.rumble && haptic_level != 0x0F) {
            out.rumble = true;
            if (console_.haptics) {
                // Already scaled from the haptics envelope; only the console's
                // own off switch (0xF) applies.
                out.weak = console_.weak;
                out.strong = console_.strong;
            } else {
                out.weak = ScaleHaptic(console_.weak, haptic_level);
                out.strong = ScaleHaptic(console_.strong, haptic_level);
            }
        }

        const uint8_t trigger_level = console_.intensity >> 4;
        if (!out.left_trigger && !out.right_trigger && console_.triggers &&
            trigger_level != 0x0F) {
            out.left_trigger = true;
            out.right_trigger = true;
            out.left = console_.left;
            out.right = console_.right;
        }
        out.intensity = console_.intensity;

        rumble_active = out.rumble && (out.weak != 0 || out.strong != 0);
        // Rumble motors need periodic refresh, but never at input rate.
        if (!output_dirty_) {
            if (rumble_active) {
                due = now - last_output_ >= 0.01;
            } else {
                due = now - last_output_ >= 0.2;
            }
            if (!due) {
                return true;
            }
        }
        output_dirty_ = false;
        last_output_ = now;
    }

    return SendOutput(out);
}

bool Gamepad::SendOutput(const PadOutput& output) {
    if (!sony_active_ || handle_ == INVALID_HANDLE_VALUE) {
        return false;
    }

    // USB output report 0x02, 47 payload bytes. Offsets verified against a
    // captured report and SDL's DS5EffectsState_t: flags 1/2, rumble 3/4,
    // triggers 11..32, intensity 37, player LEDs 43/44, lightbar 45..47.
    const size_t length = 48;  // report 0x02: 47 payload bytes + id
    std::vector<uint8_t> report(length, 0);
    report[0] = 0x02;
    report[2] |= 0x40;  // effect intensity byte is valid
    report[37] = output.intensity;

    if (output.rumble) {
        report[1] |= 0x02;  // disable audio haptics while emulating rumble
        if (!is_edge_ && firmware_version_ != 0 && firmware_version_ < 0x0224) {
            // Older DualSense firmware: emulated rumble, half-scale values.
            report[1] |= 0x01;
            report[3] = static_cast<uint8_t>(output.weak >> 1);
            report[4] = static_cast<uint8_t>(output.strong >> 1);
        } else {
            // 2.24+ firmware (and the Edge): improved rumble path.
            report[39] |= 0x04;
            report[3] = output.weak;
            report[4] = output.strong;
        }
    }
    if (output.mic_led) {
        report[2] |= 0x01;
        report[9] = output.mic_state;
    }
    if (output.right_trigger) {
        report[1] |= 0x08;
        std::memcpy(&report[11], output.right.bytes.data(), 11);
    }
    if (output.left_trigger) {
        report[1] |= 0x04;
        std::memcpy(&report[22], output.left.bytes.data(), 11);
    }
    if (output.player_leds) {
        report[2] |= 0x10;
        report[43] = 0x02;  // brightness
        report[44] = static_cast<uint8_t>(output.player_mask | 0x20);  // instant update
    }
    if (output.lightbar) {
        report[2] |= 0x04;
        report[45] = output.r;
        report[46] = output.g;
        report[47] = output.b;
    }

    static const bool probe = std::getenv("BRP_PAD_PROBE") != nullptr;
    if (probe && (output.left_trigger || output.right_trigger)) {
        static ULONGLONG last_trig = 0;
        const ULONGLONG now = ::GetTickCount64();
        if (now - last_trig > 500) {
            last_trig = now;
            std::fprintf(stderr,
                         "[hid] triggers bt=%d L=%02x [%02x %02x %02x] R=%02x [%02x %02x %02x] "
                         "intensity=0x%02X\n",
                         bluetooth_ ? 1 : 0, output.left.bytes[0], output.left.bytes[1],
                         output.left.bytes[2], output.left.bytes[3], output.right.bytes[0],
                         output.right.bytes[1], output.right.bytes[2], output.right.bytes[3],
                         output.intensity);
        }
    }
    if (probe && output.rumble) {
        static int last_weak = -1;
        static int last_strong = -1;
        static ULONGLONG last_log = 0;
        const ULONGLONG now = ::GetTickCount64();
        if (output.weak != last_weak || output.strong != last_strong || now - last_log > 500) {
            std::fprintf(stderr,
                         "[hid] rumble weak=%u strong=%u fw=0x%04X edge=%d path=%s\n",
                         output.weak, output.strong, firmware_version_, is_edge_ ? 1 : 0,
                         (!is_edge_ && firmware_version_ != 0 && firmware_version_ < 0x0224)
                             ? "old"
                             : "new");
            last_weak = output.weak;
            last_strong = output.strong;
            last_log = now;
        }
    }

    if (!bluetooth_) {
        return WriteReport(report.data(), report.size());
    }

    // Bluetooth output report 0x31:
    //   [0] report id, [1] sequence/tag, [2] 0x10 magic tag,
    //   [3..] the same 47-byte effects payload as USB report[1..], CRC32(0xA2)
    //   over the first 74 bytes.
    // The old code copied the payload at [2] and used 0x02 as the magic byte,
    // which lost valid_flag0 entirely, so rumble/LED effects did nothing over
    // Bluetooth.
    const size_t bt_length = 78;
    std::vector<uint8_t> bt(bt_length, 0);
    bt[0] = 0x31;
    bt[1] = static_cast<uint8_t>((bt_sequence_ & 0x0F) << 4);
    bt[2] = 0x10;  // DS_OUTPUT_TAG
    std::memcpy(&bt[3], &report[1], report.size() - 1);
    bt_sequence_ = static_cast<uint8_t>((bt_sequence_ + 1) & 0x0F);
    const uint32_t crc = Crc32Seed(0xA2, bt.data(), 74);
    bt[74] = static_cast<uint8_t>(crc & 0xFF);
    bt[75] = static_cast<uint8_t>((crc >> 8) & 0xFF);
    bt[76] = static_cast<uint8_t>((crc >> 16) & 0xFF);
    bt[77] = static_cast<uint8_t>((crc >> 24) & 0xFF);

    return WriteReport(bt.data(), bt.size());
}

}  // namespace brp
