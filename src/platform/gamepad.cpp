#include "platform/gamepad.h"

#include <hidsdi.h>
#include <hidpi.h>
#include <setupapi.h>
#include <xinput.h>

#include <algorithm>
#include <cmath>
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

// Sony's Bluetooth output reports carry a CRC32 seeded with 0xA2.
uint32_t Crc32Seed(uint32_t seed, const uint8_t* data, size_t length) {
    InitCrc();
    uint32_t crc = ~seed;
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

        const bool is_dualsense = attributes.VendorID == 0x054C && attributes.ProductID == 0x0CE6;
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
        bluetooth_ = caps.InputReportByteLength != 64;
        std::fprintf(stderr, "hid: %s input=%u output=%u (%s)\n",
                     is_dualsense ? "DualSense" : "DualShock 4", caps.InputReportByteLength,
                     caps.OutputReportByteLength, bluetooth_ ? "bluetooth" : "usb");
        output_report_len_ = 0;
        buffer_.assign(std::max<uint32_t>(caps.InputReportByteLength, 78), 0);
        event_ = ::CreateEventW(nullptr, true, false, nullptr);
        write_event_ = ::CreateEventW(nullptr, true, false, nullptr);
        sony_active_ = true;
        name_ = is_dualsense ? "DualSense" : "DualShock 4";
        if (bluetooth_) {
            name_ += " (Bluetooth)";
        } else {
            name_ += " (USB)";
        }
        state_.connected = true;
        state_.name = name_;
        state_.buttons = 0;
        StartRead();
        opened = true;
    }

    ::SetupDiDestroyDeviceInfoList(set);
    return opened;
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

    // Motion and touch. Offsets are payload-relative: USB raw 16..27,
    // BT extended raw 17..28.
    for (int i = 0; i < 3; ++i) {
        next.gyro[i] = ReadI16(p, 15 + i * 2, length - base);
        next.accel[i] = ReadI16(p, 21 + i * 2, length - base);
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

uint32_t Gamepad::ConsumePressed() {
    const uint32_t pressed = pressed_;
    pressed_ = 0;
    return pressed;
}

bool Gamepad::WriteReport(const uint8_t* data, size_t size) {
    if (handle_ == INVALID_HANDLE_VALUE || !write_event_) {
        return false;
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

bool Gamepad::SendOutput(const PadOutput& output) {
    if (!sony_active_ || handle_ == INVALID_HANDLE_VALUE) {
        return false;
    }

    // USB output report 0x02, 47 payload bytes. Offsets verified against a
    // captured report: flags 1/2, rumble 3/4, triggers 11..21 and 22..32,
    // player LEDs ~41..43, lightbar 44..46.
    const size_t length = 48;  // report 0x02: 47 payload bytes + id
    std::vector<uint8_t> report(length, 0);
    report[0] = 0x02;
    report[1] = 0xFF;
    report[2] = 0xF7;

    if (output.rumble) {
        report[3] = output.weak;
        report[4] = output.strong;
    }
    if (output.mic_led) {
        report[9] = output.mic_state;
    }
    if (output.right_trigger) {
        std::memcpy(&report[11], output.right.bytes.data(), 11);
    }
    if (output.left_trigger) {
        std::memcpy(&report[22], output.left.bytes.data(), 11);
    }
    if (output.player_leds) {
        report[41] = 0x02;  // brightness
        report[43] = output.player_mask;
    }
    if (output.lightbar) {
        report[44] = output.r;
        report[45] = output.g;
        report[46] = output.b;
    }

    if (!bluetooth_) {
        return WriteReport(report.data(), report.size());
    }

    // Bluetooth output report 0x31: same payload shifted by one, CRC32(0xA2).
    // Bluetooth output report 0x31: 77 payload bytes + id.
    const size_t bt_length = 78;
    std::vector<uint8_t> bt(bt_length, 0);
    bt[0] = 0x31;
    bt[1] = 0x02;
    std::memcpy(&bt[2], &report[1], std::min<size_t>(47, report.size() - 1));
    const uint32_t crc = Crc32Seed(0xA2, bt.data(), 74);
    bt[74] = static_cast<uint8_t>(crc & 0xFF);
    bt[75] = static_cast<uint8_t>((crc >> 8) & 0xFF);
    bt[76] = static_cast<uint8_t>((crc >> 16) & 0xFF);
    bt[77] = static_cast<uint8_t>((crc >> 24) & 0xFF);

    return WriteReport(bt.data(), bt.size());
}

}  // namespace brp
