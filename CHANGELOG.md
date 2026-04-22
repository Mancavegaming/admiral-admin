# Changelog

All notable changes to this project will be documented in this file. Format is loosely [Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow [SemVer](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.1.0] — 2026-04-22

Initial release.

### Added

- Lua mod (`admiral-admin`) loaded as a WindrosePlus sub-mod, registering RCON commands:
  - `ap.setmult <key> <value>` — set a world multiplier (loot, xp, weight, craft_cost, stack_size, crop_speed, cooking_speed, inventory_size, points_per_level). Writes the `R5GameMode` property for live effect where the engine permits, persists to `windrose_plus.json`, and calls `Config.reload()`.
  - `ap.preset <name>` — apply a preset bundle: `vanilla`, `easy`, `hard`, `event-2xxp`, `event-loot`, `event-chill`.
  - `ap.say <message>` — log broadcast (in-game chat is blocked on a future C++ companion mod; see roadmap).
  - `ap.bringall` — normalize all connected players' speed back to 1.0×.
  - `ap.adminlog` — show recent admin actions from the mod's own persisted log.
- Single-page web panel served by the WindrosePlus dashboard at `http://localhost:8780/admiral/`. Players / World / Presets / Announce / Server / Console tabs. Reuses the WindrosePlus login cookie — no extra auth.
- `install.ps1` — idempotent installer. Re-run after each WindrosePlus update to restore the web panel (which WindrosePlus's installer wipes).

### Known limits

- Heal / kill / feed / teleport / give-item are not in v0.1 — they require calling game UFunctions which pure Lua on Windrose cannot reach. Roadmap tracks a C++ UE4SS companion mod to close this gap.
- Some multipliers (e.g. stack_size) may be baked at world init and won't apply live without a server restart. `docs/commands.md` will track which fall into each bucket once live-tested.
