# 13 — Connection (what the button does today)

`Connect` is an automatic two-step flow: check the control port, wake the console
if it is in rest mode, then start the session. No manual step and no fake
progress.

---

## Flow

| Step | What happens | Shown as |
|---|---|---|
| Probing | TCP connect to `host:9295` on a worker thread (1.2 s) | `Checking 192.168.1.21 ...` |
| Reachable | Port answers → session starts immediately | stream |
| Unreachable | Sends the Remote Play wake-up packet, retries the probe every 2.5 s for 30 s | `Waking console... (attempt N)` |
| Woke up | Probe turns reachable → session starts | `Console woke up. Connecting...` |
| Wake timeout | Nothing answered after 30 s | rest-mode hint (see below) |

Home and Settings show the same detail line.

The wake timeout message names the setting that actually matters:
`Settings → System → Power Saving → Features Available in Rest Mode → Stay
Connected to the Internet`. Without it a rest-mode PS5 accepts discovery but
not TCP 9295.

---

## Wake-up packet

A console in rest mode still answers UDP discovery (9302 for PS5, 987 for PS4)
but drops TCP 9295, which looks exactly like a hung machine. The same SRCH
mechanism carries a wake command.

- Sent with `chiaki_discovery_wakeup` (already vendored in `brp_session`).
- Credential = the registration key's ASCII hex text parsed as a number. Our
  stored `rp_regist_key` starts with `31 63 36 62 30 34 62 35` = `"1c6b04b5"`,
  so the credential is `0x1c6b04b5`. Passing the binary bytes instead would send
  a wrong credential.
- Diagnostics: `wake_probe [host]` loads `session.json` and sends one packet:
  `[wake] sent to 192.168.1.21 (credential 1c6b04b5, PS5)`.

The probe/wake loop runs inside the normal UI frame (monitor refresh rate), so the
UI never blocks for the 30 s window.

---

## Why port 9295

Chiaki's protocol uses a TLS control channel on 9295 for session and input
traffic. A successful TCP handshake proves the address is routable, the console
is awake, and no firewall is silently dropping the port. It proves nothing about
authentication; that is the session request's job.

Discovery supplies the LAN address automatically at startup and overwrites a
stale (often WAN) address imported from PXPlay.

---

## Error messages

Session failures are no longer reported as a number. `CHIAKI_EVENT_QUIT` is
passed through `chiaki_quit_reason_string`, so the Home card shows e.g.
`Session ended: Session request timed out`, `Remote Play on Console is already in
use`, or `Connection Refused in Session Request`.

---

## Deliberate omissions

- **No fake progress bar.** The button reports real probe/wake results.
- **No retry after a session failure.** Reconnect is explicit.
- **WAN is not supported.** Discovery is LAN-only by design.

---

## Files

- `src/net/reachability.{h,cpp}` — non-blocking connect on a worker thread.
- `src/session/discovery.{h,cpp}` — SRCH discovery + `WakeConsole`.
- `src/app/app.cpp` — `ConnectConsole` / `TickPendingConnect` / `StartStream`.
- `tools/wake_probe` — sends a single wake packet for diagnostics.
