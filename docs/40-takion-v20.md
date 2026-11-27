# Takion 20 - research notes and the gated experiment

The reference client negotiates Takion protocol **20** on PS5; the vendored
chiaki-ng core speaks 7/9/12. This note records what was learned and what is
already in the tree behind `BRP_TAKION20`.

## What the experiment proved (live PS5, 2026-11-27)

With `BRP_TAKION20=1` the console accepted version 20 at the Takion layer:

- Senkusha advertised `{9, 20}` and `client_version=20`.
- `Takion connecting (version 20)` -> init ack, cookie ack, connected.
- Then `StreamConnection failed to decode data protobuf` and the Bang timed
  out. The console also reported `0x80108b10 (Remote is already in use)` after
  the failed attempt.

Conclusion: version 20 is not only a video header change. The **control/Bang
protobuf framing** differs too, and the feedback packets grow an extended
header (the reference client's `FeedbackMessage`/`FeedbackHistoryMessage` call
`setExtendedHeader` and compute different write offsets for v20). Input and
rumble must be ported as well or the session is unusable.

## What is implemented (gated)

- `chiaki_takion_v20_av_packet_parse`: A/V header map derived from the
  reference client's `VideoMessage`/`AudioMessage` v20 decoders.
  - packet id: u32 at +4, frame id: u16 at +10, unit dword at +12
  - video: codec at +16, key pos at +21, video word at +25, adaptive at +27,
    slice descriptor at +28 when `buf[0]` bit 4 is set, payload at +28/+31
  - audio: same prefix, codec at +16, crypto +17, key pos +21, two reserved
    bytes, optional slice at +27, payload at +27/+30; unit size and source/FEC
    counts are derived from the payload length
- `streamconnection.c` uses version 20 only with the env var; v12 stays default.
- `senkusha.c` advertises 20 and sets `client_version=20` only with the env var.

## Next steps for a real v20 port

1. Control/Bang: map the v20 `ControlMessage` header and protobuf wrapper from
   the reference client; the current failure is exactly here.
2. Feedback: add the v20 extended header (timestamp + packet id) to
   `FeedbackMessage` and `FeedbackHistoryMessage`; keep the v12 layout for
   version <= 12.
3. Audio/haptics: confirm the haptics stream id in v20 (the current heuristic
   reuses the v12 `0x02` payload check).
4. Re-test video, audio, input and rumble with `BRP_TAKION20=1`, then decide
   whether to make it the default for PS5.

Until then, keep the env var off: the default path is v12 and was verified at
60 fps (age 0.0 ms, zero drops) with the v20 code compiled in.
