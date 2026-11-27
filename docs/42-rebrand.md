# Rebrand: Veyla

The product is now **Veyla**. This was a *visible-surface* rebrand: the names a
user sees changed, while internal target names, settings keys and the data
folder were kept to avoid breaking existing installs and pairings.

## What the user sees

- Window titles: `Veyla`, `Veyla Stream`, `Veyla Overlay`, `Veyla control center`.
- Shell header, Home, Advanced → About, the classic shell wordmark, updater
  messages and diagnostics: Veyla.
- Header: the logo tile draws a bold **V**, `Veyla` sits beside it at 17 px, and
  the version line under the name is gone. The tab row still starts with Home.
- Icon and logo are the same mark: a 28%-rounded violet→indigo tile
  (`#8B5CF6` → `#4F46E5`, 140°) with a white Inter Bold V at 46% of the tile.
  `tools/make_logo_icon.py` generates `assets/app.ico`, `assets/app.png` and the
  copy embedded by the shell (`ui/assets/app.ico`), and the header draws the
  same geometry in `widgets::logo_mark`. Both apply the same 0.6% downward
  optical nudge, so the taskbar icon and the in-app mark cannot drift apart.
- Executables in the package:
  - `veyla.exe` - the GPUI shell (Rust).
  - `veyla-engine.exe` - headless stream engine started by the shell.
  - `veyla-classic.exe` - the previous C++ shell (fallback).
  - `veyla-legacy.exe` - the original 0.5 reference shell.
  - `update.exe` - the single-file updater.
- Release artifacts: `Veyla-<version>-update.exe`,
  `Veyla-<version>-standalone.zip`.
- The in-app updater looks for a `.exe` asset with `update` in the name and
  stores it as `%TEMP%\Veyla-<version>-update.exe`.

## What stayed internal (on purpose)

| Internal | Why |
|---|---|
| CMake targets `brpnext`, `brpnext-engine`, `brp` | renaming every target would churn the build and probe tools for no user benefit |
| `%APPDATA%\BetterRemotePlayNext` | keeps the existing settings, profiles and pairing; no migration risk |
| `next-settings.json`, `session.json` | same files the old shell used |
| `brp_update.exe` target | packaged as `update.exe`; the target name is not visible |
| `brpnext-engine.exe` bridge fallback | lets a new shell run beside an older dev engine |

The shell still falls back to `brpnext-engine.exe` when `veyla-engine.exe` is
not present, and the updater accepts `brpnext.exe`/`brp.exe` as launch targets,
so mixed dev folders keep working.

## The BetterRemotePlay name

**BetterRemotePlay is the original project name and remains a trademark of its
author.** It is kept in compatibility paths and legacy files, not as the
current product brand. The package README and Advanced → About say this
explicitly. All code and branding are original work.

## Name note

`Veil` was the working title; it is a registered word mark in the UK/EU in
classes 9/41/42 (software/games), so the shipped name is the coined **Veyla**,
which keeps the sound but is a distinct mark.

## Not changed

- The app icon (an abstract aurora controller) is brand-neutral and still fits.
- Settings keys (`ai.*`, `streaming.*`, ...) are unchanged.
- The updater payload format and footer magic are unchanged.
