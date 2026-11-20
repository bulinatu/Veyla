# BetterRemotePlay Next 1.0 - user guide

## First run

1. Start `brpnext.exe` from the packaged folder.
2. Open **Console**. Discovery, PSN login and pairing are on this page.
3. For a new console: `Discover`, sign in to PSN, then enter the 8-digit PIN from
   `PS5 > Settings > System > Remote Play > Link Device`.
4. `Connect` starts a real session. Watch opens automatically.

## Watch

- `F11` toggles borderless fullscreen; `Display > Fullscreen on play` enters it
  automatically when Watch starts and restores the window on exit.
- `F10` opens the live **control center** over the stream.
- `Esc` leaves Watch. A second `Esc` within 2.5 s disconnects the session.
- In the control center: arrow keys switch tabs/rows, `Enter` toggles or steps
  the selected row, mouse clicks work directly. `Esc`/`F10` close it.

## Control center

| Tab | Live controls |
| --- | --- |
| Overview | input/output, decode, cadence, SR/FG state, profile, stream |
| Super resolution | on/off, quality (DLAA included), output size |
| Frame generation | on/off, DLSS-G/RIFE backend, 2x-6x multiplier, measured rate |
| Display | borderless fullscreen, overlay stats, opacity, scale, position, theme |
| Performance | real source/displayed/decode/cadence/UI timing, reset counters |
| Profile | select, apply, save current as a new profile |

Changes go through the same settings transaction as the main window. The footer
reports what actually happened: `Applied immediately`, `Processing pipeline
rebuilt`, `Saved; reconnect to apply`, or an error. Transport changes never
claim to apply mid-session.

## Processing

### Super resolution

- **Off**: presenter upscale only.
- **DLAA**: native-resolution AA at 1080p; the lowest-latency DLSS-G input.
- **DLSS SR**: upscales to 1440p/2160p. 2160p is not a valid DLSS-G input (the
  worker is capped at 1440p) and is rejected for that path.

### Frame generation

- **DLSS-G (NVIDIA)**: the worker presents real + generated frames. 2x uses the
  NVIDIA-signed runtime; 3x-6x use the SM86 proxy if installed.
- **RIFE (TensorRT)**: one midpoint per pair, used below 45 fps. Blend keeps a
  percentage of the previous real frame; motion safety skips very fast cuts.

## Profiles and settings

- Settings save to `%APPDATA%\BetterRemotePlayNext\next-settings.json`.
- Profiles live in `%APPDATA%\BetterRemotePlayNext\profiles`.
- Invalid values are rejected before they reach an engine; the previous valid
  file is restored if a replacement fails.
- `About` has data paths, runtime availability, diagnostics export and the
  session report.

## Troubleshooting

- **"Remote is already in use"**: wait 20-30 s after a previous client was
  killed, then reconnect. The app no longer sends duplicate session requests.
- **DLSS-G missing**: check `nr/native/nvngx.dll` and `nvngx_dlssg.dll` in the
  application folder. 3x-6x also need `nr/native/fg/winmm.dll`.
- **RIFE missing**: the TensorRT engine and runtime are optional and separate
  from the DLSS path.
- **No overlay**: it is only shown while a session is active and the app window
  is in the foreground.
- Diagnostics: `Diagnostics > Write report`, or `About > Export diagnostics`.
  Credentials and tokens are never included.
