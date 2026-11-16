# 06 — Decisions (ADR log)

Short, dated records of choices that are expensive to reverse. Append, don't rewrite.

---

## ADR-001 — Fork chiaki-ng, keep the session layer, replace presenter + shell
**Date:** 2026-11-16
**Status:** accepted

Fork `chiaki-ng` (AGPL-3.0) as `vendor/chiaki-ng`. Keep discovery, pairing, session negotiation, RTP depacketization, input, haptics. Write our own `media/` (decode → color → upscale → interpolate → present) and `shell/`.

**Why:** the protocol is the risky, expensive part and it already works. Every differentiator we care about is above the wire.

**Consequence:** distributed builds are AGPL-3.0. Fork surface stays limited to `session/` so upstream fixes remain mergeable.

---

## ADR-002 — Neural post-processing: real AI stages, honest names
**Date:** 2026-11-16
**Status:** accepted

We implement **AI super-resolution** and **AI frame generation** as real neural stages on the 4090's Tensor cores. We do not call them "DLSS" in UI copy, because DLSS is engine-integrated and the name is NVIDIA's. Internally the stages are:

| Stage | Implementation | Notes |
|---|---|---|
| `upscale.ngx_vsr` | NVIDIA NGX `VideoSuperRes` (RTX Video Super Resolution) | Best quality; requires NVIDIA app licensing/allowlist. Verify before promising. |
| `upscale.trt_esrgan` | Compact Real-ESRGAN (ONNX → TensorRT FP16, tiled) | Our fallback that always works on RTX. Measured per-GPU, engine cached. |
| `upscale.nis` / `upscale.fsr1` | Spatial shaders | Universal, ≤0.5 ms, no model. |
| `fg.trt_rife` | RIFE v4.x (ONNX → TensorRT FP16) | 60→120 interpolation. Costs +1 frame minimum. |
| `fg.driver` | NVIDIA driver-level frame generation (Smooth Motion) | Experiment only; applied to our swapchain. Ship only if it measures better than RIFE. |

**Why:** "DLSS on a video stream" is impossible because DLSS needs engine motion vectors, depth, jitter, and exposure. AI video super-resolution and AI optical-flow interpolation are the same *class* of technology applied to pixels, and they run on the same hardware. This is the workaround that actually works.

**Consequence:** the post-process graph is pluggable (`decode → color → upscale → fg → print`), so any future video-domain neural API drops in as one more stage without touching the rest. Every stage is benchmarked in-app and can be disabled per profile.

---

## ADR-003 — Network profile defaults for 2.5 GbE client / 1 GbE console
**Date:** 2026-11-16
**Status:** accepted

Default profile when wired is detected:

| Setting | Default |
|---|---|
| Resolution | 1080p |
| FPS | 60 |
| Codec | HEVC (H.265), 10-bit when HDR |
| Bitrate | **50 Mbps** (ceiling 100 Mbps) |
| Decoder | `d3d11va`, fallback `cuda` |
| Upscale | `fsr1` at 1440p, `trt_esrgan`/`ngx_vsr` at 2160p |
| Frame gen | off |

**Why:** the console's encoder saturates well below the link capacity; 50 Mbps is near the point of diminishing returns at 1080p, and overshooting only adds bufferbloat risk. Adaptive mode backs off on loss with a user-set floor, never below 20 Mbps.

---

## ADR-004 — PS5-inspired shell, own assets only
**Date:** 2026-11-16
**Status:** accepted

The launcher is a **lookalike**: layout, motion language, and information hierarchy inspired by the PS5 home screen. No Sony trademarks, logos, fonts, icons, sounds, or copied artwork. All assets are original or openly licensed. Game art comes from the user's own PSN library via their login, or from local shortcut art the user supplies.

**Why:** aesthetic familiarity is the goal; legal exposure is not.

---

## ADR-005 — Measurement before pixels
**Date:** 2026-11-16
**Status:** accepted

No quality claim ships without a number behind it. Every stage is instrumented: per-frame GPU timestamps, present queue depth, actual received bitrate, input round-trip. The bench tool (`brp-bench`) exists specifically so the post-process chain can be validated on this machine without a PS5 attached.

**Why:** "minimal delay" and "not washed out" are measurable properties. If we can't show the number, we can't claim it.
