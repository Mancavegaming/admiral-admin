# Roadmap

## v0.1 (current)

Feature set committed and shipping. See [CHANGELOG.md](../CHANGELOG.md).

## v0.2 — C++ companion mod *(the big one)*

The biggest gap in v0.1 is that we can't heal, give items, teleport, or broadcast to in-game chat. Root cause: Windrose's Lua surface doesn't expose the UFunctions that would do these things natively.

Solution: write a companion C++ UE4SS mod (call it `AdmiralsPanel-Native.dll`) that:

- Hooks into the game via UE4SS native C++ API (not Lua).
- Calls `TeleportTo`, `SetHealth`, `AddItemToInventory`, `BroadcastChatMessage`, etc. directly through the UE reflection system.
- Exposes Lua-callable functions our main Lua mod can use (same pattern the WindrosePlus repo uses for `HeightmapExporter.dll`).

**Reference starting point**: `cpp-mods/HeightmapExporter/` in the WindrosePlus repo is a working, minimal C++ UE4SS mod that compiles with CMake + MSVC and exposes one function to Lua. Use it as a template.

**What's needed**:

1. UE5 SDK header dump for Windrose (generate with UE4SS's dumper).
2. Identify the UFunctions: `TeleportTo`, a "set actor health" equivalent, `AddItemToInventory` (or similar) on the inventory component, and a broadcast chat function.
3. CMakeLists + source file that loads, hooks, and exposes each as a Lua-callable function.
4. Lua-side `ap.heal`, `ap.kill`, `ap.feed`, `ap.tp`, `ap.tpxyz`, `ap.give` commands that dispatch to the DLL.
5. UI wiring in `web/app.js` — add heal/kill/give/teleport buttons to the Players tab.

**Risk**: every game patch may shift function signatures or offsets. C++ mods need maintenance. We'd version-gate: the DLL would log `warn` and Lua commands would return "C++ extension missing or incompatible with this game build" if the hooks don't resolve.

**Who can help**: anyone with UE4SS + C++ experience. This is the #1 wanted contribution.

## v0.3 — quality of life

Smaller features that are entirely within v0.1's Lua surface:

- **Per-player speed persistence** — currently `wp.speed` doesn't persist across restarts. Our admin log could note and replay.
- **Scheduled server restarts** — a Lua tick callback that announces T-10 / T-5 / T-1 minute, then calls a graceful-shutdown helper.
- **Event timer**: apply a preset for N minutes, then auto-revert to `vanilla`. Uses tick callbacks + persisted state.
- **Custom preset CRUD** from the UI (instead of editing `presets.json` by hand).
- **UI polish**: sound on player join/leave, persistent theme picker, tooltips explaining "live" vs "restart-required" per multiplier.
- **Live-vs-restart matrix discovery** — a command that probes each multiplier write and reports which actually changed gameplay (would require an AI-run test mob and loot roll). More exploration than implementation, but lets us populate the per-multiplier UI labels with ground truth.

## v0.4 — integrations

- **Discord bridge** — an external process (Node or Python) that polls `/api/rcon/log` + `events.log`, forwards player join/leave and `ap.say` messages to a Discord webhook. Accepts slash commands back via an HTTP server. Runs outside the game server, same-machine or elsewhere.
- **Browser notifications** — the web panel pushes a notification when a player joins (requires the user to grant permission).
- **Export audit log as CSV** — from the Server tab.

## v0.5 — permissions

Right now, anyone with the RCON password has full admin. For servers with multiple caretakers:

- Per-admin Steam IDs with capability tiers (view-only / tuner / full admin).
- UI that prompts for a Steam ID on first visit (read from a cookie / localStorage) and scopes UI controls to their tier.
- Relies on the `admin.steam_ids` array that's already in `windrose_plus.json` — extend with role metadata.

## Indefinite / upstream-dependent

- **Inventory UI** (drag-and-drop give-item): depends on v0.2.
- **Mob spawn control** (spawn/despawn creatures, set density): depends on v0.2.
- **Terrain editing / world state tools** (weather, time of day writes that actually take effect): partially depends on v0.2, partially on whether Windrose replicates these values live.
- **World backup / restore** from the UI: feasible in pure Lua (file I/O works), just significant to implement carefully.

## Non-goals

- Building a full replacement for WindrosePlus. We're a plugin.
- Supporting games other than Windrose.
- Hosted / SaaS version — this is a self-hosted tool; we'll never phone home.

## How to propose / claim something

Open a GitHub issue with the `feature` template. Tag `[roadmap]` in the title if it maps to an item above. For big items (like v0.2), comment here first to avoid duplicate effort.
