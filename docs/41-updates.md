# In-app updates through GitHub Releases

The shell checks a GitHub repository's releases, compares the tag with its own
version, downloads a single-file updater and can install it without leaving the
app. The stream engine does the network work (WinHTTP); the GPUI shell owns the
animation and the buttons.

## Configure

Settings are flat keys in `%APPDATA%\BetterRemotePlayNext\next-settings.json`:

| Key | Meaning |
|---|---|
| `general.update_repo` | `owner/name` of the GitHub repository (empty = disabled) |
| `general.update_channel` | `0` stable (skips prereleases), `1` beta |
| `general.check_updates` | check once at startup when a repo is set |

The same fields are under **Home > Advanced > Updates** (repository field,
channel, animated update card).

## Publish a release

1. Bump the version passed to packaging, then run:

   ```powershell
   powershell -File tools\publish_release.ps1 -Version 2.6.0 -Repo owner/name
   ```

   The script packages `dist\BetterRemotePlay`, stamps `version.txt`, builds a
   **full** update against an empty baseline so any older install can update,
   writes `dist\release-2.6.0.md` and prints the exact `gh release create`
   command.

2. Create the GitHub release with tag `v2.6.0` and attach:

   - `Veyla-2.6.0-update.exe` (required; the app looks for a `.exe`
     asset whose name contains `update`)
   - `Veyla-2.6.0-standalone.zip` (fresh installs)

3. Prereleases are only offered on the **Beta** channel.

The update executable is the same single-file payload used by the local update
flow: it contains `payload\`, `_delete.txt` and `_version.txt` appended to the
updater, supports `--dir=`, `--no-launch`, `--silent`, `--auto` and
`--wait-pid=`, and extracts with the Windows `tar.exe` (no external tools).

## Flow in the app

- **Check**: `GET https://api.github.com/repos/<repo>/releases?per_page=30`,
  pick the highest non-draft tag above the running version (prereleases only on
  Beta), and prefer an asset ending in `.exe` with `update` in the name.
- **Download**: the asset goes to `%TEMP%\Veyla-<version>-update.exe`
  with progress reported to the UI.
- **Install**: the updater is started with `--auto --dir=<app dir>
  --wait-pid=<engine pid>`. `--auto` waits for the engine, stops
  `brpnext.exe`/`brpnext-engine.exe`, applies the payload, updates
  `version.txt` and restarts `brpnext.exe`.

No token is used, so GitHub's anonymous limit applies (60 requests/hour per
IP); one startup check plus manual checks stay well inside it. Only HTTPS asset
URLs from the release are used. The binaries are not code-signed; signing is a
separate release step.
