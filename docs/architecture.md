# Architecture

A short tour for contributors.

## The stack

```
              +------------------------------------------+
              |    Browser  (http://localhost:8780/      |
              |     admiral.html)                        |
              +------------------+-----------------------+
                                 |
                            HTTP + cookie auth
                                 |
+------------------+  serves web  +-----------------------+
| windrose_plus /  |<----/admiral/---- server/web/admiral/|
| server (PowerShell HTTP listener, port 8780)            |
|                                                         |
|   /api/status     /api/config     /api/rcon            |
+------------------+-------+--------+--------------------+
                                |
                    spool files (RCON bridge)
                                |
            +-------------------v-------------------+
            |  UE4SS Lua state (inside server exe)  |
            |                                       |
            |  WindrosePlus.Scripts.modules.*       |
            |  WindrosePlus.Mods.admiral-admin      |
            |     init.lua  (this project)          |
            |                                       |
            |  FindAllOf("R5GameMode") etc          |
            +---------------------------------------+
```

## Layers we author

### 1. Lua mod (`mod/init.lua`)

Registered with WindrosePlus via `Mods/admiral-admin/mod.json`. Loaded at server boot (the name's leading letter happens to sort after `02-admin`/`example-welcome` alphabetically — reload order doesn't matter for us).

Responsibilities:

- Register `ap.*` RCON commands through `WindrosePlus.API.registerCommand`.
- When a multiplier changes: (a) write the `R5GameMode` UE property directly via UE4SS Lua (mirrors what `wp.speed` does for `MaxWalkSpeed`), (b) edit `windrose_plus.json` atomically (`.tmp + os.rename`), (c) call `require("modules.config").reload()`.
- Append every admin action to `windrose_plus_data\admiral_admin_log.json` (capped at 200 entries).
- Expose `ap.adminlog` to read the log back.

Everything is `pcall`-wrapped so a UE4SS oddity doesn't take down the mod. State lives on disk, not in module-local tables — hot-reload is safe.

### 2. Web UI (`web/`)

Served by WindrosePlus's dashboard (port 8780 by default) from `server/web/admiral/`.

**Crucial property**: the WindrosePlus dashboard serves arbitrary files under `server/web/` via a default `switch` case. We do NOT need to patch the PowerShell server — dropping files in is enough.

The UI is three static files (`index.html`, `app.js`, `app.css`) plus a redirect helper (`admiral.html`) at `server/web/` root. Tailwind is loaded via CDN; no build step.

Data flow per tab:
- **Players**: `/api/status` poll (every 3s) → render table. Slider change → debounced `POST /api/rcon: wp.speed <name> <mult>`.
- **World**: `/api/status` poll → sync slider positions to current multipliers. Slider change → debounced `POST /api/rcon: ap.setmult <key> <value>`.
- **Presets**: click → `POST /api/rcon: ap.preset <name>`.
- **Announce**: submit → `POST /api/rcon: ap.say <msg>`.
- **Server**: `/api/status` + `ap.adminlog 100`.
- **Console**: submit → `POST /api/rcon: <raw>`.

### 3. Installer (`install.ps1`)

Idempotent PowerShell script. Ships the mod into `WindrosePlus/Mods/`, appends to `mods_registry.json`, copies the web panel into `server/web/admiral/`, and drops a redirect helper. Re-runnable — each step tolerates pre-existing state.

## Layers we DON'T touch

### WindrosePlus server (`windrose_plus/server/windrose_plus_server.ps1`)

The PowerShell HTTP listener. We rely on three behaviors:

- Session auth via `wp_session` HttpOnly cookie (`Test-SessionToken`, line 109+).
- `/api/rcon` accepts JSON `{command}` and returns `{id, status, message}` (line 547-594).
- The `default` route serves static files from `server/web/` (line 595-618).

**We do not patch this file.** Our installer copies into `server/web/` only.

### WindrosePlus Scripts (`R5/Binaries/Win64/ue4ss/Mods/WindrosePlus/Scripts/`)

Their `Config`, `RCON`, `Query`, `LiveMap`, `MapGen`, `Mods`, `Admin` modules. We consume two from our mod:

- `require("modules.json")` for JSON encode/decode.
- `require("modules.config")` for `Config._path` (absolute path to `windrose_plus.json`) and `Config.reload()`.

We rely on `WindrosePlus.API` (documented in their `scripting-guide.md`) for `registerCommand`, `log`, `getPlayers`, `onPlayerJoin`, etc.

## UE4SS access notes

Windrose's C++ gameplay exposes very little to Lua — no UFunctions are callable (this is not a UE4SS limitation; it's how the game is compiled). What DOES work from Lua:

- `FindAllOf("ClassName")` to enumerate UObject instances.
- Reading simple scalar UProperties (`actor.MaxWalkSpeed`, `mult.XPMultiplier`).
- Writing simple scalar UProperties — **replicates server-to-client for properties the engine marks as replicated and re-reads live**.

What doesn't work:
- Calling any UFunction (e.g., `actor:K2_SetActorLocation(...)`, `inventory:AddItem(...)`). Silent no-op.
- Reading complex attribute-set-wrapped stats (e.g., `HealthComponent.CurrentHealth` returns an opaque UObject on this build).
- Writing position (client-authoritative — server writes are overwritten by `ServerSaveMoveInput` replays).

The [roadmap](roadmap.md) covers the C++ UE4SS companion mod that would close these gaps.

## Why not a separate HTTP service?

We considered running a standalone admin HTTP server on port 8781 that proxies RCON. Rejected because:

- The WindrosePlus dashboard already serves arbitrary files under `server/web/` — zero-patching integration beats cross-origin coordination.
- The dashboard's session cookie works out of the box for same-origin calls.
- One less process to manage.

Cost: we have to re-run `install.ps1` after every WindrosePlus update (which wipes `server/web/`). Acceptable — it's 5 seconds.

## Hot-reload caveat

WindrosePlus's file-watcher detects mod changes by a coarse signature: `(length, first_byte, last_byte)` of each tracked file. Edits that preserve all three (rare but possible) won't trigger a reload. If you edit the mod and your change doesn't take effect, touch the file to shift its length (e.g., add a trailing comment line), or restart the server.

Our `install.ps1` avoids this by doing a `Remove-Item` + `Copy-Item` — the file's mtime changes, but the signature comparator doesn't use mtime. In practice reinstalls trigger reload reliably because the file contents differ between versions.
