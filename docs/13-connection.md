# 13 — Connection (what the button does today)

The `Connect` button on the Home console card performs a **reachability probe**: a TCP connection attempt to the console's Remote Play control port (9295) with a 1.2 s timeout. That is real work, and it is deliberately not dressed up as pairing.

---

## States

| State | Meaning | Shown as |
|---|---|---|
| Idle | Nothing has been tried | profile status, e.g. `PS5-580 · needs pairing` |
| Checking | Probe running | amber dot, `Checking the control port...`, button reads `Checking` |
| Reachable | Something accepted the TCP connection | green dot, `Reachable` |
| Unreachable | Timeout or refused | amber dot, `No answer on the control port` |
| No address | Session has no host | `No address configured` |

The result and a detail line are also visible under **Settings → Connection → Test reachability**, so the card and the settings page agree.

---

## Why port 9295

Chiaki's protocol uses a TLS control channel on 9295 for session and input traffic. A successful TCP handshake proves:

- the address is routable from this PC,
- the console is awake (or at least something is listening),
- firewall/NAT is not silently dropping the port.

It proves nothing about authentication. A closed port is a genuine "cannot reach it"; an open port is a genuine "there is a console there". Both are useful before pairing exists.

The address comes from the imported PXPlay profile (`cached_direct_ip_addresses`). If it is a WAN address, the probe will usually fail on a home network — that is information, not a bug. A local address or manual entry will make it pass, and manual entry arrives with the pairing flow.

---

## What pairing will add

The full handshake needs three things this build does not have yet:

1. **PSN account id** — already imported (base64 derived from the decimal id).
2. **Console PIN** — 8 digits shown on the console under `Settings → System → Remote Play → Link Device`.
3. **Session layer** — chiaki-ng's registration/client code compiled into `brp-session`, which stores `rp_key` / `rp_regist_key` in `session.json`.

Once those exist, `Connect` becomes: discover → register (PIN) → session request → stream. The UI already has the states, the profile file and the card; only the transport is missing.

---

## Deliberate omissions

- **No fake progress bar.** The button reports the probe result, not a pretend handshake.
- **No silent retries.** Every attempt is user-initiated.
- **No discovery broadcast yet.** A UDP discovery probe without the protocol's key exchange would produce misleading results, so it waits for `brp-session`.

---

## Files

- `src/net/reachability.{h,cpp}` — non-blocking connect with `select`, runs on a worker thread, polled each frame.
- `src/session/profile.{h,cpp}` — console identity, account id, address, stream defaults.
- `src/app/app.cpp` — console card, `Connect` action, Connection tab.
