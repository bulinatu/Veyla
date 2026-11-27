# Saved profiles

Saved profiles are snapshots of the whole settings file, stored one JSON file per
profile in `%APPDATA%\BetterRemotePlayNext\profiles`.

## Rules

| Rule | Where |
|---|---|
| At most **12** profiles. | `kMaxProfiles` in `src/next/shell/next_app.cpp`, `Root::PROFILE_LIMIT` in `ui/src/app.rs` |
| The engine owns naming: a bare `profile_save` picks the lowest free `Profile N`. | `NextApp::SaveProfileByName` |
| Deleting is two-step: click `Delete`, the button becomes `Confirm`, click again within 4 s. | `Root::request_delete_profile`, `OverlayView` Profiles tab |
| One delete per gesture: 1.5 s cooldown after a delete. | same |
| The list is creation-ordered, not alphabetical (`Profile 10` no longer sorts before `Profile 2`). | `ProfileStore::List` |
| Deleting the default profile promotes the oldest survivor. | `ProfileStore::Delete` |

## Why the guard

The list rows are compact and each row carries its own `Apply` / `Delete`
buttons with unique element ids (`widgets::button_with_id`). If a stall makes
the shell process a burst of queued clicks, the row under the pointer changes
after every delete, so a second click would land on the next profile. The
cooldown keeps a burst to a single deletion.

## UI

- **Home** – "Saved profiles" card: name, `Default` chip, `Apply`, `Delete`.
  The header shows `n / 12`; the card scrolls after five rows so twelve profiles
  cannot push the rest of the page away. At the cap the save button reads
  `Limit reached`.
- **Control centre (F10) → Profiles** – same actions, plus `Save current`.

## Wire protocol

```
profile_save             -> engine picks the next free "Profile N"
profile_save <name>      -> explicit name (overwrites an existing id)
profile_apply <id>
profile_delete <id>
profiles                 -> engine publishes the list to the shell
```
