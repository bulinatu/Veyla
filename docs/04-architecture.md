# 04 — Architecture

## Decision

**Fork chiaki-ng's core, keep its protocol and input layers, replace the video presentation path, replace the UI.**

Rationale: the protocol is the expensive, risky part and chiaki-ng already has it working, including HDR, PS5, PSN-mediated internet play, and all the FFmpeg decoders. Our differentiators are all above the wire: color, scaling, interpolation, latency, and the shell. Rewriting discovery/pairing/session from scratch would burn months and add zero user-visible quality.

---

## Module map

```
┌──────────────────────────────────────────────────────────────────────┐
│  shell/            PS5-style launcher, settings, overlay, profiles   │
│                    (UI only; must not touch the video path)          │
├──────────────────────────────────────────────────────────────────────┤
│  session/          fork of chiaki-ng: discovery, pairing, PSN OAuth,  │
│                    session negotiation, RTP depacketize, input send,  │
│                    haptics, wake                                     │
├──────────────────────────────────────────────────────────────────────┤
│  media/            our code:                                         │
│    decoder.cpp     FFmpeg hwaccel abstraction (d3d11va/d3d12va/      │
│                    dxva2/cuda/vulkan/software)                       │
│    color.cpp       YUV→RGB, range, matrix, chroma upsample, PQ/BT.2020│
│    upscale/        NIS, FSR1(EASU+RCAS), Lanczos, [RTX VSR], none     │
│    interp/         optical-flow interpolation (opt-in)                │
│    present/        DXGI flip model, 8/10-bit, HDR metadata, pacing    │
│    audio.cpp       Opus decode → WASAPI low-latency, A/V clock        │
├──────────────────────────────────────────────────────────────────────┤
│  platform/         Windows: DXGI, WASAPI, GameInput/XInput, DPAPI     │
└──────────────────────────────────────────────────────────────────────┘
```

**Rule:** `shell/` never sits in the video path. The overlay must be composited by the presenter, not drawn by a browser or a second process over the window. That is how you keep "minimal delay" while still having a nice UI.

---

## Frame pipeline

```
 RTP (UDP)
    │
    ▼
 depacketize ──► access unit queue (depth 1, drop-late)
    │
    ▼
 hardware decode  ──► NV12 / P010  (D3D11 texture, zero-copy)
    │
    ▼
 color stage      ──► RGB16F: range expand, BT.709/BT.2020 matrix,
    │                 Catmull-Rom chroma, PQ→scRGB for HDR
    ▼
 upscale stage    ──► NIS / FSR1 / Lanczos / [RTX VSR] → 1440p / 2160p
    │
    ▼
 [interp stage]   ──► optional optical-flow frame generation (opt-in)
    │
    ▼
 present          ──► DXGI flip model, 8-bit or R10G10B10A2 + HDR meta,
                     no vsync wait, ALLOW_TEARING, frame pacing
```

Buffers on GPU throughout. No CPU readback, ever. If a stage needs the CPU, it's the wrong stage.

---

## Why not just patch chiaki-ng's renderer?

chiaki-ng's renderers (Vulkan/OpenGL/Metal) already get frames on screen and support HDR. But:

- Inserting scaling + interpolation *inside* a renderer that wasn't designed for a post-chain means touching its swapchain code, its frame pacing, and its platform layers — and fighting upstream diffs forever.
- Our presenter wants: flip model, mailbox, tear-free-off, per-frame latency stats, HDR metadata control, and an overlay composited in the same pass. That is a different design, not a tweak.

So: take the engine, write `media/` and `platform/` ourselves, and keep the fork surface to `session/` only. Small, mergeable upstream diffs.

---

## Decoder matrix

Probed at startup, user-overridable, benchmarkable in-app:

| Priority | Decoder | When chosen |
|---|---|---|
| 1 | `d3d11va` | Default on Windows |
| 2 | `d3d12va` | If D3D12 presenter is active and supported |
| 3 | `cuda` | NVIDIA + user preference |
| 4 | `vulkan` | Vulkan presenter or non-D3D path |
| 5 | `dxva2` | Legacy GPU / DXVA-only |
| 6 | software | Fallback, always works, always slowest |

Decode must target the **same device** as the presenter (`D3D11Device` shared with DXGI swapchain) so the color stage reads a texture directly.

---

## Audio

- Opus decode (FFmpeg or libopus), 48 kHz.
- **WASAPI shared low-latency** by default; exclusive mode as an option for the last few ms (fragile, hence not default).
- Audio is the **master clock**. Video is presented as close to audio as pacing allows; drift corrected by dropping or holding frames, never by resampling audio.
- Buffer target: 4–6 ms shared, 2–3 ms exclusive, adaptive under jitter.

---

## Input

- DualSense wired (USB) or Bluetooth via SDL/GameInput; touchpad, gyro, adaptive triggers where available.
- Send reports immediately on the control channel — do not wait for a video frame.
- Local haptics from console rumble events; audio-haptics pass-through if the OS supports it.
- Latency readout for input separately from video: input is the number that actually determines "feel".

---

## PSN OAuth

```
launcher → system browser (PKCE auth code flow)
         → redirect to loopback listener (127.0.0.1:<random>)
         → exchange code → access/refresh token
         → fetch Account ID + online ID (+ optional library)
         → store in Windows Credential Manager (DPAPI), refresh silently
```

- Tokens never touch a config file or logs.
- Logout wipes tokens and pairing keys.
- If the user skips PSN login, fall back to manual Account ID entry (today's chiaki flow) so the app is never unusable.

---

## Latency budget (targets, not guesses)

| Stage | Target | How we verify |
|---|---|---|
| Decode | ≤ 6 ms @ 1080p | In-app per-frame stats |
| Color + upscale | ≤ 3 ms | GPU timestamp queries |
| Present queue | ≤ 1 frame | Presenter stats |
| Audio buffer | ≤ 6 ms | WASAPI stats |
| Input → console | ≤ 2 ms LAN | Loopback ping + report timing |
| **Client-side total** | **≤ 15 ms + 1 frame** | Sum, shown in overlay |

Glass-to-glass target: **≤ 55 ms wired** at 1080p60 with interpolation off. Verify with a high-speed camera or LDAT against a local reference, not with a stopwatch.

---

## Tech choices

| Concern | Choice | Why |
|---|---|---|
| Language (core) | C++20 | Same as chiaki-ng; zero-cost interop with its C protocol lib; FFmpeg/DXGI are C/C++ |
| Graphics | D3D11 first, D3D12 later | D3D11VA zero-copy + flip model is the shortest path to a correct picture; D3D12 only when it buys measured latency |
| Video | FFmpeg 9.x (hwaccel) | Same version chiaki-ng ships; wide codec coverage |
| Audio | WASAPI + libopus | Lowest-latency Windows path |
| Input | SDL3 / GameInput | DualSense features, hotplug |
| Shell | Tauri (Rust + web UI) or Qt6/QML | Fast iteration for PS5-style motion design without putting a browser in the video path |
| Config | TOML, per-profile | Human-editable, diffable |
| Packaging | MSIX or WiX + auto-update | Windows-native install/update |

**Shell caveat:** if the shell is web-based, it must be a *separate window* (launcher/settings) and the stream window must be pure native. No Electron over the video.

---

## Risks

| Risk | Mitigation |
|---|---|
| chiaki-ng upstream refactors break the fork | Keep the diff to `session/`; sync monthly; upstream generic fixes |
| Firmware changes the protocol | `session/` isolation; version-detect; keep a fallback decoder path |
| RTX VSR licensing blocks integration | Default to FSR1/NIS; VSR is an optional plugin |
| Interpolation artifacts embarrass the product | Off by default; labeled "experimental"; per-game override |
| AGPL obligations misunderstood by users | Publish source, link it in About, keep notices |
| Sony sends a takedown | Ship no Sony assets/code; be ready to rename/relocate; not much else we can do |
