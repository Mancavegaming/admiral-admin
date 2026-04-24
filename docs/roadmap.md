# Roadmap

## Shipped

- **v0.1 (2026-04-22)** — Lua-only: tunings, presets, admin log, web UI.
- **v0.2 (2026-04-22)** — C++ companion mod (`AdmiralsPanelNative.dll`): GAS attribute writes. Heal / damage / kill / feed / revive / setattr / readattr / teleport.
- **v0.3 (2026-04-23)** — Attributes tab, per-player row actions, live stat bars, multi-set attribute access (Posture, Comfort, RangeWeapon).
- **v0.4 (2026-04-23)** — Random-loot give: `ap.giveloot` teleports populated loot actors, auto-pickup delivers whatever they hold. Plus a reverse-engineering toolkit (`ap.scan`, `ap.rawdumpn`, `ap.dumpclassn`, etc.).
- **v0.5 (2026-04-24)** — Specific-item give: `ap.giveitem <player> <search>` identifies items in populated loot actors by scanning LootView memory for references to known `UR5BLInventoryItem` data assets. Supporting commands: `ap.lootitems`, `ap.itemlist`, `ap.itemscan`.
- **v0.6 (2026-04-24)** — Standalone mode. AdmiralsPanel no longer requires WindrosePlus. Native DLL hosts its own HTTP server (port 8790), session/login, and RCON dispatch. Standalone UE4SS Lua mod with own command registry + spool poller. Legacy sub-mod install still supported via `install.ps1 -WithWindrosePlus`.

See [CHANGELOG.md](../CHANGELOG.md) for the full feature list per release.

## Next up

- **Chat broadcast** (`ap.say` → real in-game message). `ClientMessage` is an RPC and UE4SS argument marshalling crashes; needs a non-RPC path from C++ ProcessEvent, or a server-side broadcast UFunction on GameMode that isn't an RPC.
- **Specific-item give with arbitrary count / any item in the game** (not just what's currently in the world). Requires decoding the C++-only rule dispatcher (`R5BLInventory_AddItemsRule`). Multi-session RE, probably pattern-scanning the game binary.
- **UI buttons for native commands**. `ap.giveitem`, `ap.giveloot`, `ap.healn`/`damagen`/`killn` per-row, `ap.attr*` presets — currently console-only. Pure web-panel work.
- **Scheduled server actions** — cron-style restarts, timed announcements, multiplier swaps. Pure Lua; WindrosePlus tick hook + persisted state.

## Later

- **Discord bridge** — external process polls `/api/rcon/log` + `events.log`, forwards player join/leave and `ap.say` messages to a webhook; accepts slash commands back via HTTP.
- **Browser notifications** on player join (web-panel side).
- **Export audit log as CSV** from the Server tab.
- **Permissions / Steam-ID tiers** — per-admin capability scoping. `windrose_plus.json`'s `admin.steam_ids` array is the starting point.
- **Mob admin** — extend the native attribute surface to AI characters (same pattern, different attribute set).
- **World backup / restore** from the UI.

## Non-goals

- Building a full replacement for WindrosePlus. We're a plugin.
- Supporting games other than Windrose.
- Hosted / SaaS version — this is a self-hosted tool; we'll never phone home.

## How to propose / claim something

Open a GitHub issue with the `feature` template. Tag `[roadmap]` in the title if it maps to an item above. For big items (like v0.2), comment here first to avoid duplicate effort.
