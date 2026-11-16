# 10 — Input: mouse, keyboard, controller, DualSense

Status: **mouse and controller navigation work; DualSense input and output work over Bluetooth.**

---

## Mouse

| Action | Result |
|---|---|
| Hover a tile | Tile border brightens, hand cursor |
| Click a far tile | Selects it (rail slides) |
| Click the selected tile | Opens the stream |
| Click a settings row | Selects it |
| Click the left/right zone of a row | Changes the value |
| Click `Settings` pill / `Back` | Navigates |
| Wheel | Moves the selection |

Hit regions are rebuilt every frame while drawing, then tested in reverse draw order, so the topmost element always wins. No separate layout pass, so a region can never drift from what is on screen.

---

## Keyboard

| Screen | Keys |
|---|---|
| Home | `Left`/`Right` or `A`/`D` select · `Enter` stream · `S`/`Tab` settings · `C` controller · `Esc` quit |
| Settings | `Up`/`Down` select · `Left`/`Right` change · `Enter` open · `S`/`Esc` back |
| Settings → Controller | Dropdowns and buttons; `Tab` switches category |
| Stream | `Esc` back · `H` HUD · `1`/`2`/`3` upscaler |
| Any | `F12` capture frame |

---

## Controller

Two backends, one state:

```
DualSense / DS4  ──raw HID──┐
                            ├──► PadState ──► UI navigation
Xbox / virtual   ──XInput───┘             └──► (future) PS5 report forwarding
```

HID takes priority. Sony pads are found by VID/PID (`054C:0CE6`, `054C:05C4`, `054C:09CC`) through SetupAPI, with automatic reconnect every 2 s.

### Navigation mapping

| Input | Action |
|---|---|
| D-pad, left stick | Move (350 ms initial repeat, 110 ms after) |
| Cross | Confirm |
| Circle | Back |
| Options | Settings / back |
| L1 / R1 | Previous / next tile on Home |

### Supported reports

| Connection | Input report | Output report |
|---|---|---|
| USB | `0x01`, 64 bytes | `0x02`, 48 bytes |
| Bluetooth | `0x31`, 78 bytes (`0x01` 10-byte simple mode also parsed) | `0x31`, 78 bytes + CRC32 seeded `0xA2` |

Detection is by `HIDP_CAPS.InputReportByteLength`: 64 = USB, 78 = Bluetooth extended, ≤11 = Bluetooth simple.

---

## DualSense feature matrix

| Feature | State | Notes |
|---|---|---|
| Buttons, sticks, triggers | ✅ | USB + Bluetooth, forwarded live |
| D-pad as hat | ✅ | decoded from the 4-bit hat |
| Touchpad touch points | ✅ forwarded | 2 points, native 1920×1080 coordinates |
| Touchpad click | ✅ | `PadTouchpad` → protocol button |
| Gyro / accelerometer | ✅ forwarded | scaled to rad/s + g, Madgwick quaternion |
| Battery % + charging | ✅ | from the status byte |
| Mute button | ✅ | |
| Lightbar RGB | ✅ output | bytes 45–47 of the `0x02` report (fixed in this pass) |
| Player LEDs | ✅ output | byte 43/44, or console-driven |
| Mic LED | ✅ output | off / on / pulse |
| Adaptive triggers | ✅ both ways | 8 manual effects + console effects passed through |
| Rumble (2 actuators) | ✅ both ways | local test + console rumble with intensity scaling |
| CRC32 for Bluetooth | ✅ | polynomial `0xEDB88320`, seed `0xA2` |
| Input/report rate | ✅ | 60 / 125 / 250 / 500 / 1000 Hz, high-resolution timer |
| Trigger effect calibration | ⚪ | firmware-provided ranges not applied |
| Haptic voice-coil stream | ⚪ | needs the audio-haptic path over USB (see below) |
| Speaker / headset audio | ⚪ | separate USB audio interface |
| Gyro calibration | ⚪ | feature report 0x05 not read; raw scale assumed |
| Console-driven effects | ✅ | rumble, trigger effects, lightbar, player LEDs, intensities |

### Adaptive trigger effects implemented

| Effect | Encoding |
|---|---|
| Off | all zero |
| Rigid | `01 00 start force` |
| Pulse | `02 00 start end force` |
| Feedback | `21 zone strength` |
| Weapon | `25 start end strength` |
| Vibration | `26 frequency amplitude FF` |
| Bow | `27 start end force snap` |
| Galloping | `23 start end foot1 foot2 frequency` |

The `0x26` layout is cross-checked against a captured PlayStation Accessories USB report (`26 90 a0 ff …`), which is why the trigger block offsets (11–21, 22–32) are trusted. The 0x2x family comes from community documentation; effects are exposed as a tester, not as a promise of console-identical feel.

---

## Testing it

Settings → Controller (the Controller category).

- Lightbar: 8 presets, applied live.
- Adaptive trigger: 8 effects; strength 1–8; applies to both triggers.
- Rumble: `Enter` fires a 450 ms pulse.
- Player LEDs / Mic LED: cycle masks.
- Live readout: buttons, sticks, triggers, touch point, battery.
- Subtitle reports `output reports OK` / `failed`, i.e. whether the write actually reached the device.

![Controller settings](images/app-settings-controller.png)

Verified on this machine: DualSense over **Bluetooth**, input 78 bytes, output writes accepted (78-byte `0x31` + CRC).

---

## Gotchas that cost time

1. **A HID handle opened with `FILE_FLAG_OVERLAPPED` cannot be written with a null `OVERLAPPED`.** `WriteFile` returns `ERROR_INVALID_PARAMETER` (87). Output needs its own event + `GetOverlappedResult`.
2. **`HIDP_CAPS.OutputReportByteLength` is the largest output report, not the one you are sending.** For the DualSense it reports 547 (report `0x39`), while the state report is 78. Using the cap as buffer size fails. Each report ID has its own length.
3. **Bluetooth input is 78 bytes, not 10.** The 10-byte `0x01` simple mode is a fallback; the pad on this machine sends `0x31` at 78 bytes. Detecting by "≤11 means Bluetooth" was wrong — compare against 64 (USB) instead.
4. **XInput never sees a DualSense.** `XINPUT_GAMEPAD_GUIDE` is also not in the public header (value `0x0400`). Sony pads need HID.

---

## Next

- Read the calibration feature report `0x05` so gyro becomes calibrated rad/s
  (currently the standard 1024 LSB per deg/s scale is assumed).
- USB audio-haptic path: decode the console's separate haptics Opus stream and
  route it to the DualSense 4-channel USB audio endpoint. Over Bluetooth there
  is no audio endpoint, so this cannot work wirelessly.
- Trigger effect calibration from firmware ranges (currently pass-through).
