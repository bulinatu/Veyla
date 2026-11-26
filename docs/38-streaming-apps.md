# 38 - What other streaming clients do, and what is left for us

A short comparison of PXPlay (PSPlay), chiaki-ng, Moonlight/Sunshine and
Parsec against the current 2.5.0 pipeline. Only techniques that can apply to a
PS5 Remote Play client are listed; console-side encode and the LAN hop are
outside our control.

## What they do

### PXPlay PC

- Hardware decode straight into a GPU texture; the renderer samples it. No
  CPU RGBA conversion, no upload.
- D3D11/D3D12/Vulkan backends for GPU-side colour/scaling/HDR.
- "Lowest Latency" and "Balanced" frame pacing; per-console override.
- Decoder profiles ("Ultra-Low Latency" first) that trade reordering and
  robustness for immediate output.
- FSR 1.0 and sharpening as shader passes.
- Smart frame recovery from the bitstream when packets are lost.

### chiaki-ng

- FFmpeg hardware decode on D3D11VA/NVDEC/VAAPI/VideoToolbox.
- FEC and an IDR request when a frame is lost; adaptive bitrate based on
  measured throughput.
- Controller polling and DualSense haptics/triggers on their own path.
- Audio queue with a configurable buffer (9,600 bytes is the common low-latency
  setting).
- Frame queue with a small fixed depth; newest-frame selection is an option.

### Moonlight / Sunshine

- Hardware encode/decode, AV1/HEVC, HDR passthrough.
- Frame pacing tied to the display; "never drop" vs "lowest latency" modes.
- Input over a separate low-latency channel, 1 ms polling, absolute mouse.
- Adaptive bitrate and packet-loss recovery.
- Virtual display on the host so the game renders at the client's mode.

### Parsec

- UDP transport with its own congestion control, hardware decode, low-latency
  audio, and a presentation path that favours the newest frame.
- Host-side capture/encode tuning; not much of it is applicable to a console
  stream.

## What we already have

| Technique | 2.5.0 |
|---|---|
| Hardware decode on the presenter's device | yes, d3d11va + shared device |
| Zero-copy GPU presentation | yes when AI is off |
| CPU YUV conversion | no; video processor |
| One queued frame / maximum latency 1 | yes |
| Tearing and vsync control | yes |
| Buffer 0, newest-frame pull | yes |
| Decode thread priority | above normal |
| DLSS-G feed thread priority | above normal (2.5.0) |
| Input polling | up to 1000 Hz |
| WASAPI event callback, 5-200 ms buffer | yes, 10 ms default |
| FEC / IDR on loss | through chiaki-ng |
| 1440p DLSS SR + NR + FG | yes |
| DLSS 4.5 presets / forced MFG | yes |

## Gaps worth considering (not implemented)

1. **Adaptive bitrate from measured loss/jitter.** chiaki-ng exposes the
   transport; a controller that lowers the requested bitrate when jitter or
   loss rises would help Wi-Fi. The console's session profile may not accept a
   live change without a restart, so this needs a reconnect path.
2. **DSCP/QoS marking on the UDP socket.** Mark the video/audio socket
   expedited forwarding on managed networks. The Winsock code is in the
   vendored chiaki layer; a small `setsockopt(IP_TOS)` would be low risk but
   only helps when the router honours it.
3. **Socket receive buffer.** A larger `SO_RCVBUF` absorbs short bursts at
   100 Mbit/s; chiaki sets it already, verify the value on the target machine.
4. **Bitstream-level recovery.** PXPlay parses the bitstream to resume after a
   short stall. chiaki's IDR request covers the common case; a full parser is a
   large change.
5. **AV1 / HDR / VRR.** The PS5 Remote Play profile is 1080p60 HEVC SDR; none
   of these are available from the console, and HDR/VRR were deliberately
   removed from the UI.
6. **Game-style Reflex sleep.** Measured harmful on the waitable chain; we keep
   markers/boost only.

## What was deliberately not copied

- **A second presentation buffer** for smoothness. It costs a full frame at
  60 fps and was the 58-60 ms mistake found in the 2.4.7 audit.
- **Frame generation as a latency feature.** It smooths; it does not make the
  real frame arrive earlier.
- **Vulkan/D3D12 backends** for their own sake. The D3D11 path already runs at
  240 Hz with zero missed presents; a second backend is maintenance cost.

## References

- PXPlay releases: https://github.com/streamingdv/PSPlay-Application-Hosting/releases
- chiaki-ng: https://git.sr.ht/~thestr4ng3r/chiaki
- Moonlight: https://moonlight-stream.org/
- Sunshine: https://github.com/LizardByte/Sunshine
- Parsec: https://parsec.app/
