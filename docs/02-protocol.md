# 02 — The PS5 Remote Play protocol, as far as it is publicly known

> Everything here comes from open-source reverse engineering (Chiaki → chiaki-ng → setsu) and community measurement. It is **not** an official spec. Anything marked `VERIFY` must be confirmed on real hardware/firmware before we depend on it.

---

## 1. Roles

```
 PS5 (host)                                        PC (client)
 ──────────                                        ───────────
 discovery responder  ◄── UDP discovery ─────────► discovery
 wake-on-lan target   ◄── magic packet ─────────── wake
 TLS control server   ◄── TCP control ──────────── session / input / auth
 RTP sender (video)   ──► UDP video ─────────────► decode
 RTP sender (audio)   ──► UDP audio ─────────────► decode
 encoder (fixed)      ── requests ───────────────► resolution / fps / bitrate / codec
```

The console is a **server**; the PC initiates everything. There is no way to make the console push anything better than what its encoder produces.

---

## 2. Discovery and wake

- Console advertises itself on the LAN via UDP broadcast; the client can also probe a known IP.
- `VERIFY`: exact discovery port/format (historically UDP 987 in the Chiaki family). Discovery is a convenience — a manually entered IP always works.
- Wake-on-LAN: magic packet to the console's MAC. Works when the console is in rest mode and "Enable Remote Play" is on.

---

## 3. Pairing

Two-step, and this is where most users get stuck today:

1. **Client identity.** The client proves it belongs to a PSN account by presenting the account's **Account ID** (base64 of the numeric PSN ID).
   - Today's open-source flow: user manually obtains the Account ID from a third-party helper site or the `psn-api`-style OAuth flow.
   - PSPlay's flow: full **PSN OAuth login** in-app, Account ID derived automatically.
   - **Our plan:** PSN OAuth in-app. This is a client-side OAuth flow against Sony's public PSN auth endpoints — the same thing the PS App does. It is not console hacking, but it *is* a ToS gray area; see the legal note.
2. **Console PIN.** On the console: `Settings → System → Remote Play → Link Device` shows an 8-digit code. The client registers with that PIN over the TLS control channel; the console stores the client's key, and subsequent connections don't need a PIN.

`VERIFY`: TLS is a self-signed console certificate; the client accepts/pins it. Registration yields a long-lived client key stored locally.

---

## 4. Session negotiation

Before any video, the client sends a session request that includes the stream parameters it wants:

| Parameter | What a PS5 accepts | Notes |
|---|---|---|
| Resolution | **720p, 1080p** | No 1440p, no 2160p. The console mirrors its output at 1080p and re-encodes. This is the single most important fact in this repo. |
| Frame rate | **30, 60** | 120 is not offered. `VERIFY` on PS5 Pro firmware; no public evidence of 120. |
| Codec | **H.264 (AVC)**, **H.265 (HEVC)** | HEVC is **required** for 10-bit/HDR. AV1 is not offered. |
| Bit depth | 8-bit (AVC/SDR HEVC), **10-bit (HEVC HDR)** | HDR = HEVC Main10, PQ, BT.2020. |
| Bitrate | client *requests*; console's encoder decides what it will actually emit | Community: 15–30 Mbps typical from the encoder; requests of 30–50 Mbps are accepted and improve quality until the encoder saturates. Higher targets are accepted by the session layer (Portal mods push ~100 Mbps) but effective quality plateaus. `VERIFY` per firmware. |
| Audio | **Opus**, 48 kHz stereo | `VERIFY` sample rate/channels. |
| Color | BT.709 limited-range for SDR | The classic "washed out" cause. HDR path is BT.2020 PQ. |

**Consequence:** our quality ceiling is 1080p60 HEVC 10-bit at whatever bitrate the encoder will give us. That's a genuinely good picture — 1080p HEVC at 40 Mbps is far better than the official app — but it is not 4K and it is not 120 fps at the source.

**Verified 2026-11-16 (PS5 `PS5-580`, firmware current):** requesting 2560×1440 in the launch spec makes the console abort the stream with `Nagare did not init! AvCap failed to initialize video: [InitResult:-6]`. Its advertised profile list contains a single entry, `1920x1080`. There is no 1440p/4K on the wire; both exist only as client-side upscaling (FSR1/Lanczos) of the 1080p stream. The app now clamps any larger request and logs why instead of failing the session.

---

## 5. Stream

- **Video:** RTP, HEVC/AVC elementary stream, **no B-frames** (low-latency encoding), one slice per frame typical. Loss shows up as smearing/artifacts rather than a freeze, because the console encodes for resilience.
- **Audio:** RTP, Opus, small frames for low latency.
- **Input:** sent over the TLS control channel as periodic controller reports (buttons, sticks, triggers, touchpad, gyro, battery). Vibration/haptics come back on the same channel. This is why input latency ≠ video latency — input can be *faster* than the picture, which is what makes the stream feel responsive.
- **Encryption/pacing:** transport is encrypted; the client must not add buffering. `VERIFY` exact crypto and RTP header extension layout against chiaki-ng source before implementing anything from scratch.

---

## 6. Latency, honestly

Glass-to-glass, wired, 1080p60:

| Stage | Typical | Notes |
|---|---|---|
| PS5 capture + encode | 10–25 ms | Dominated by one frame period (16.7 ms at 60 fps) + encoder |
| Network (LAN, wired) | 1–5 ms | Wi-Fi 5 GHz: 3–15 ms with jitter. This is the biggest variable in the whole system. |
| Decode (GPU) | 3–10 ms | NVDEC/D3D11VA; ~5 ms at 1080p on a modern GPU |
| Color convert + upscale | 1–3 ms | Compute shader in FP16 |
| Present (flip model, no vsync wait) | 1–2 frames | Must use DXGI flip model + low-latency present |
| **Total** | **~40–70 ms** | The official app sits around 60–90 ms. |

"Zero delay" is physically impossible: the console must encode at least one frame (16.7 ms) before anything leaves the box. What we can do is shave every millisecond on *our* side and never add a buffer we don't need. Targets in [05-roadmap.md](05-roadmap.md) are stated as measurable numbers, not vibes.

Frame interpolation (your 120/240 fps request) **adds** latency — typically 1–2 frames. That's a direct conflict with "zero delay". Design decision: interpolation is a per-profile opt-in, off by default, with the latency cost shown in the UI.

---

## 7. Things we must not assume

- That bitrate requests are honored exactly.
- That HDR is available on all displays/firmware (it depends on the console's own HDR settings too).
- That controller reports map 1:1 on every firmware.
- That discovery works on a segmented/VLAN network (manual IP must always be an option).
- That the protocol is stable across firmware updates. It isn't guaranteed, and it has broken before.
