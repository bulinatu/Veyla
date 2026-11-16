# 08 — The application shell

Status: **v0.5 — live.** Pairing, streaming (video + audio), categorized settings, FSR1/Lanczos upscaling, HDR-capable presenter.

---

## Screens

### Home

- Horizontal game rail, spring-animated selection, dynamic accent glow.
- **Console card** (top right): console name, status, address, and a `Connect` button.
- `Stream` opens the stream screen for the selected game.
- Bottom-left shows the connected controller.

![Home](images/app-home.png)

### Settings

Five categories in a left sidebar, each with its own settings — nothing is lumped together:

| Category | Contents |
|---|---|
| Display | Upscaler, upscale target (window/1080p/1440p/2160p), latency HUD, frame generation (reserved), HDR (reserved) |
| Audio | Output device, buffer size, test tone, live format/latency/underruns |
| Controller | Input source, lightbar, adaptive trigger, strength, player LEDs, mic LED, rumble |
| Connection | Console, address, pairing, PSN account, stream defaults, reachability test |
| About | Version, description, legal note |

Every setting is a **dropdown** (or a button), not a key cycle. Dropdowns open a popup list, close on outside click, and show the current value with ellipsis when it does not fit.

![Settings — Display with an open dropdown](images/app-settings-display.png)

![Settings — Audio](images/app-settings-audio.png)

![Settings — Controller](images/app-settings-controller.png)

![Settings — Connection after a probe](images/app-settings-connection.png)

### Stream

The measured post-process pipeline running full-window, with a live latency HUD and a controller chip.

---

## Controls

| Where | Action |
|---|---|
| Mouse | Hover highlights, click selects/activates, wheel moves the rail |
| Home | `Left`/`Right` select · `Enter` stream · `S` settings · `Esc` quit |
| Settings | `Tab` / `Left` / `Right` switch category · `Esc` closes a dropdown, then leaves |
| Stream | `Esc` back · `H` HUD · `1`/`2`/`3` upscaler |
| Any | `F12` capture the current frame to `app-<screen>.bmp` |

Controller navigation still works on Home (d-pad/stick, Cross, Circle, Options).

---

## Code map

```
src/main_app.cpp              message loop, 60 Hz pacing
src/app/app.{h,cpp}           screens, hit regions, dropdowns, layout, animation
src/ui/ui_renderer.*          Direct2D + DirectWrite (rects, rounded clips,
                              gradients, text cache, ellipsis trimming)
src/ui/theme.h                palette
src/media/pipeline.*          D3D11 device, flip-model swapchain, stream surface
src/audio/audio_engine.*      WASAPI render engine (in-process, no driver)
src/net/reachability.*        TCP probe for the console control port
src/session/profile.*         session file, PXPlay metadata import
src/platform/gamepad.*        HID (DualSense/DS4) + XInput
src/platform/win32_window.*   window, mouse/keyboard, per-monitor DPI
```

---

## Design rules

1. **No browser in the video path.** Direct2D onto the same swapchain; only the HUD composites over the stream.
2. **One device.** Decoder, post-process, presenter and UI share one `ID3D11Device`.
3. **Hit regions are built while drawing**, then tested in reverse draw order, so a click can never miss what is visible. Dropdown items are registered last, which is why a popup always wins over the control beneath it.
4. **No fake controls.** Rows that are not implemented are disabled and labelled (for example `Off · needs RIFE model`), and `Connect` performs a real reachability probe rather than pretending to pair.

---

## What is demo data

| Thing | Now | When wired |
|---|---|---|
| Game library | 8 invented titles with procedural art | PSN library via OAuth |
| Console card | Real metadata imported from PXPlay | Live discovery + session state |
| `Connect` | TCP probe of port 9295 | Pairing + session handshake |
| Stream source | Synthetic 1080p generator | HEVC decode output |
| Bitrate / codec / HDR | Stored in the session profile, shown read-only | Negotiated with the console |

---

## Next

1. Wire the chiaki-ng core into `brp-session` and turn `Connect` into a real pairing flow.
2. Feed decoded Opus into the audio engine and decoded video into the presenter.
3. Replace the rail's demo library with the user's PSN library.
