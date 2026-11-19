# 30 - Updates

Status: **working** (19 Nov 2026). Two artefacts per release:

| Artefact | Use |
| --- | --- |
| `BetterRemotePlay-<version>-standalone.zip` | Clean install; the whole app. |
| `BetterRemotePlay-<version>-update.zip` | Patch for someone who already has the previous version. |

## For the person receiving it

1. Close BetterRemotePlay.
2. Extract the update zip **into the app folder** (where `brp.exe` is).
3. Run `update.exe`. It copies the changed files over, removes obsolete ones,
   writes `version.txt` and starts the app.

`update.exe --no-launch` applies the patch without starting the app.
`update.exe --dir=<path>` targets another folder.

The updater refuses to run while `brp.exe` is open and says so instead of
half-copying files onto a locked executable.

## For the person building it

After packaging a release:

```
powershell -File tools\make_update.ps1 `
    -BaselineZip dist\BetterRemotePlay-0.3-standalone.zip `
    -NewFolder   dist\BetterRemotePlay `
    -Version     0.4
```

It extracts the baseline zip, compares every file by SHA-256, and writes:

- `update.exe` (built from `tools/brp_update`, target `brp_update`)
- `payload\` with only new and changed files, relative paths kept
- `delete.txt` for files that no longer exist (omitted when empty)
- `version.txt`

The full package now also contains `update.exe` at the root, so future patches
only need `payload`, `delete.txt` and `version.txt`; bundling it again is
harmless.

## Measured

0.3 (the 14:28 package) -> 0.4:

```
Added 0, changed 2 (brp.exe, README.txt), deleted 0
Patch size: 0.6 MB
```

Applied to an extracted 0.3 folder: copied 2 files, wrote version 0.4, removed
`payload` and `delete.txt`, and every file matched the 0.4 package byte for byte
(`files_differ=0`).

## Limits

- A patch only makes sense from the exact baseline it was built against. From a
  much older build the patch simply contains more files; from a newer build it
  may revert changes, so say which version the patch is for.
- `update.exe` does not verify signatures or roll back. Keep the previous full
  zip if you need to recover.
- Files are compared by size first and hash second, so a same-size change is
  still caught.
