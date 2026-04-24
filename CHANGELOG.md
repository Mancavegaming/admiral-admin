# Changelog

All notable changes to this project will be documented in this file. Format is loosely [Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow [SemVer](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.6.0] — 2026-04-24

Standalone mode — AdmiralsPanel no longer requires WindrosePlus.

### Added

- **Native-hosted HTTP server** on port 8790 (configurable via
  `admiralspanel.json`). Serves the web panel, login flow, and `/api/rcon`
  endpoint directly from `AdmiralsPanelNative.dll` via Winsock2. Session
  cookies, password login, and static file serving all implemented in
  the DLL — zero runtime dependencies beyond the Winsock stack.
- **Standalone UE4SS Lua mod** at `R5/Binaries/Win64/ue4ss/Mods/AdmiralsPanel/`.
  Defines its own command registry (`AdmiralsPanel.API`) that mirrors the
  subset of `WindrosePlus.API` we used (`log`, `registerCommand`). Runs
  under the standard UE4SS mod loader — no WindrosePlus required.
- **Command-spool bridge** — the native HTTP server writes
  `cmd_<id>.json` files into `admiralspanel_data/rcon/`; the Lua mod's
  `LoopAsync` poller dispatches them through the registered handlers and
  writes `res_<id>.json` back. Same pattern WindrosePlus used, our own
  implementation and spool directory.
- **`admiralspanel.json`** auto-generated on first run at the server root,
  containing a randomly-generated 24-char password + HTTP port. No config
  file was required under WindrosePlus because we leaned on theirs.
- **`install.ps1` defaults to standalone mode**. Pass `-WithWindrosePlus`
  for the legacy sub-mod install path (still supported for users who
  already run WP).

### Changed

- `mod/init.lua` detects which API is active (`AdmiralsPanel.API` for
  standalone, `WindrosePlus.API` for sub-mod) and routes through the
  appropriate command registrar. The same code now runs in both modes.
- Multiplier persistence (`ap.setmult`) writes to `admiralspanel.json`
  in standalone mode; continues to write to `windrose_plus.json` in
  sub-mod mode.

### Migration from v0.5

- Existing WindrosePlus sub-mod installs keep working untouched. To switch
  to standalone, run `install.ps1` without flags and optionally remove
  `ue4ss/Mods/WindrosePlus/Mods/admiral-admin/` after verifying the new
  setup works.
- The RCON password for standalone mode is separate from WindrosePlus's —
  check `admiralspanel.json` for the generated value (or set your own).

### Ports

- Sub-mod mode: `http://localhost:8780/admiral.html` (via WP dashboard)
- Standalone mode: `http://localhost:8790/` (our own HTTP server)

Both modes can run simultaneously during migration — they use separate
spool directories and don't conflict.

## [0.5.0] — 2026-04-24

The specific-item-give unlock.

### Added

- **`ap.giveitem <player> <search>`** — teleports a populated `R5LootActor`
  whose *source-actor class* matches `search` (case-insensitive substring).
  Sources are things like `BP_Segment_Coast_Jungle_Ficus_1200cm_C`, so
  `ap.giveitem Mancave ficus` grabs a Ficus-tree drop. When no specific
  source matches the search (gatherables and some drop types don't expose
  their source through DropView memory), the command falls back to
  teleporting a random populated loot actor — so it always delivers
  *something* when there's loot in the world.
- **`ap.lootitems [N]`** — list the first N populated `R5LootActor`s with
  their identified source class (or `<unknown source>` if we can't
  determine it). Use before `ap.giveitem` to see what's targetable.
- **`ap.itemlist [search]`** — browse known `UR5BLInventoryItem` data
  assets (~1200 of them, filter by substring like `ap.itemlist banana`).
  Informational only — we can't materialize arbitrary items from this
  list, but it's useful for knowing what the game has.
- **`ap.itemscan <target> [bytes]`** / **`ap.lootslots [N]`** —
  reverse-engineering helpers. Scan an object's raw memory for item
  references, or hex-dump slot structs of the first populated loot actor.

### How it works

Windrose loot-actor inventory data lives in a central C++ subsystem keyed
by record IDs (opaque strings like `622DBF23...|I|7|I|13650`) that isn't
reachable from DropView memory through reflected UFunctions. Rather than
decoding the private table, `ap.giveitem` identifies drops by the BP class
that produced them:

1. The DropView's memory holds pointers to source-related UObjects (for
   tree drops, these are the source tree's `StaticMeshComponent`s at
   `LootView+0x180`).
2. We validate every dereferenced pointer against a snapshot of the live
   UObject table (`gather_live_uobjects`), so we never call virtual
   methods on random memory.
3. For each valid candidate pointer, we walk the `Outer` chain up to 6
   hops, skipping generic container classes (`World`, `Level`,
   `GameEngine`, `R5BLIslandView`, `R5GOS_*`, etc) until we hit a
   specific source class.

### Coverage

- **Works for most tree/resource-break drops** — Ficus, Palm, and similar
  `BP_Segment_*` classes have a mesh-component chain that resolves cleanly.
- **Fallback path for everything else** — gatherables (record-ID suffix
  `|GA|`), mob kills, chest drops, and any loot-actor layout where the
  source-actor can't be reached from DropView memory fall back to the
  random-loot path. This is the same delivery mechanism as `ap.giveloot`
  — it still teleports one real populated drop to the player.
- **Crash-safe** — every pointer we dereference is validated against the
  live UObject table before we call any virtual method on it.
- **Post-teleport position** — loot drops 80cm ahead of the player at
  foot level (Z-80), not inside the player capsule, so auto-pickup can
  actually magnet it in.

### Known limitations

- Perfect source identification would require pattern-scanning the game
  binary to find the C++ rule dispatcher. Deferred; see
  `docs/native-mod.md` for the roadmap.
- "Give me *exactly* 10 bananas" with no tree currently broken is still
  not possible — the feature works with drops already in the world, not
  arbitrary item spawning.

## [0.4.0] — 2026-04-23

The give-item unlock.

### Added

- **`ap.giveloot <player> [count]`** — teleports up to `count` populated
  `R5LootActor` instances from the world to the player. The player's
  `R5Ability_Loot_AutoPickup` auto-collects them on proximity. Delivers
  whatever items are already on those loot actors (fiber, wood, food,
  depending on what mobs have died / resources broken).
- **`ap.yankactorn <player> <full_path>`** — teleport any actor (by full
  UObject path) to a player. Generic primitive.
- **`ap.spawnn <player> <class_path> [dx dy dz]`** — spawns any Actor
  subclass at player location + offset, using manual ProcessEvent with
  correctly-sized params buffers for `BeginDeferredActorSpawnFromClass` +
  `FinishSpawningActor`. Works for vanilla actors (tested with
  `/Script/Engine.StaticMeshActor`). **Does NOT work** for BP-derived
  pickups like `BP_WaterPickup_*_C` — their construction scripts crash
  `FinishSpawningActor` (likely `R5FoliageMeshComponent` init requires a
  world context the dedicated server doesn't have at arbitrary spawn
  points).
- **Reverse-engineering toolkit** — six new diagnostic commands:
  - `ap.rawdumpn <target> [bytes]` — hex+ASCII dump of a UObject's raw
    memory. Target can be a class short name (finds first live instance)
    or a full path. Used to decode non-reflected C++ field layouts.
  - `ap.dumpclassn <classname> [N]` — list first N live instances of a
    class with their addresses + paths.
  - `ap.funcparamsn <func_path>` — dump a UFunction's parameter layout
    (name / offset / size / type). Used to verify reflected param struct
    sizes when UE4SS's hardcoded wrappers are wrong for the build.
  - `ap.locn <player>` — print a player's world location.
  - `ap.findclassn <path>` — resolve a full object path, report class-of
    and address.
  - `ap.scanpath <substring>` — scan UObject full-paths (not just class
    names) for a substring. Finds assets under `/Game/...`, UFunctions
    by path like `Class:Function`, etc.
  - `ap.lootlistn [N]` — list populated R5LootActor instances in the
    world (those with non-null `LootView`).
  - `ap.lootinspectn [bytes]` — find the first populated R5LootActor and
    hex-dump its `R5BLActor_DropView` memory. Used in give-item RE.

### Changed

- **`ap.scan`** now matches both the object's own name AND the class
  name of its instances. Previously only matched by instance class,
  which missed UClass / UScriptStruct / UFunction definitions when
  searching by name.
- `ap.classprobe` falls back to a full UObject scan when the four
  hardcoded `/Script/...` package prefixes don't resolve the class.
  Classes in packages like `/Script/R5DataKeepers.`,
  `/Script/R5ActionManager.` now resolve automatically.

### Technical notes on give-item (deferred sub-work — task #22)

Full writeup in [`docs/native-mod.md`](docs/native-mod.md). Short version:
Windrose's inventory is rule-based (`R5BLInventory_AddItemsRule` + params
struct `R5BLInventory_AddItems` containing `FR5BLReward` containing
`TArray<R5BLItemsStackData>`) — but **rules are invoked via a C++-only
dispatcher, not reflected UFunctions**. We mapped every level of the
data structure (confirmed via `classprobe`) but none of the invoke paths
are callable from UE4SS-C++ without pattern-scanning the game binary.

The working give-item path uses existing world-spawned `R5LootActor`
instances (populated by mob deaths / resource breaks) and teleports
them to the player; auto-pickup delivers. Specific-item targeting would
require parsing the `R5BLActor_DropView`'s non-reflected internal
structure (5 levels of pointer chasing: DropView → slot map →
SlotView → FR5BLItem → TSoftObjectPtr<UR5BLInventoryItem>). Deferred
until the DropView layout is empirically decoded.



## [0.3.1] — 2026-04-23

### Added

- **Live stat bars** on the Players tab — HP / Stamina / Posture per player,
  polled every 2s via a new `ap.allstatsn` that batches all online players'
  attributes into one RCON call.
- **Multi-set attribute access**: `ap.readattrn` and `ap.setattrn` now walk
  `ASC.SpawnedAttributes` to find whichever set has the attribute (previously
  only checked `R5AttributeSet`). So you can now read/write Posture,
  Comfort, RangeWeapon attributes through the normal commands. Output also
  tells you which set the value was found on (e.g.
  `Comfort [on R5ComfortAttributeSet]: current=1 base=0`).
- **`ap.classprobe` / `ap.scan`** — reverse-engineering helpers for future
  work. Dumps a UClass's properties or scans all loaded UObjects for
  classes matching a substring.

### Changed

- `ap.feedn` no longer refills Comfort. On Windrose the Comfort attribute
  is tracked but doesn't drive the visible comfort/hunger UI (derived
  client-side from inventory/shelter/warmth) — setting it had no effect
  so we dropped it from the refill list to avoid confusion.
- Players tab row UI: fixed a flash-and-reset bug where the stat bars
  would momentarily empty on every status poll. Now the table only
  rebuilds when the player set changes; stats update in place.

### Documentation

- `docs/native-mod.md` gained a detailed "Give-item deferred" section
  with the full research trail: rule-based inventory architecture,
  findings from `R5BLInventory_AddItemsRule` / `R5AMTask_SpawnActor`, and
  the realistic path forward for anyone picking it up.

## [0.3.0] — 2026-04-23

### Added

- **Attributes tab** in the web UI — dropdown of ~150 `R5AttributeSet` fields grouped into Survival / Combat / Stamina / Damage Done / Damage Taken / Corruption. Read any current attribute or write any new value. Eight quick-preset buttons for common cheats (MaxHealth 9999, 100% crit, 5× damage out, 99% damage resist, MaxComfort 9999, etc.).
- **Per-player row actions**: Heal / Feed / Revive / Kill buttons + Teleport-to-other-player dropdown on the Players tab.
- `ap.feedn` now walks all spawned attribute sets via ASC.SpawnedAttributes and refills Health / Stamina / Comfort / Posture (Windrose uses `R5ComfortAttributeSet` for its "hunger" analogue). Also zeros CorruptionStatus as a bonus cleanse.
- Inspector (`ap.inspectn`) enhancements: enumerates `ASC.SpawnedAttributes` and scans all UObjects for inventory-related class names. Used during reverse engineering.

### Fixed

- `ap.setattrn` / `ap.healn` / `ap.damagen` / `ap.kill` now use the correct `FGameplayAttributeData` layout (vtable ptr + BaseValue + CurrentValue). Previously read the vtable as a float and got 0.
- `install.ps1` handles the case where the server is running and holds `main.dll`: compares hashes and silently skips if unchanged instead of erroring.

### Known limits

- Give-item is **deferred**. Windrose's inventory is rule-based (`R5BLInventory_AddItemsRule` and ~30 other R5BusinessRules rule classes) — items aren't added by a direct call; you construct a rule + params + apply. Multi-session reverse engineering job. Tracked in #16.
- Chat broadcast still requires a non-RPC path (`ClientMessage` crashes UE4SS marshalling). Tracked in #20.
- Windrose's hunger UI icon persists even after `ap.feedn` fills Comfort — the icon is tied to a separate GameplayEffect cleared only by eating a food item (which give-item would unlock).

## [0.2.0] — 2026-04-22

The C++ unlock.

### Added

- **`AdmiralsPanelNative.dll`** — optional C++ UE4SS companion mod. Exposes gameplay-level admin features that pure Lua can't safely reach, by writing directly to Windrose's GAS (Gameplay Ability System) `R5AttributeSet` struct on `PlayerState`. Source under [`cpp/`](cpp/), one-command build via [`cpp/build.ps1`](cpp/build.ps1), prebuilt DLL attached to each release.
- **Teleport** (pure Lua, no C++ needed):
  - `ap.tp <player> <target>` — move player to another player's position.
  - `ap.tpxyz <player> <x> <y> <z>` — move to coords.
  Uses the `K2_TeleportTo` UFunction — server-authoritative, replicates cleanly.
- **Native-backed `ap.*n` commands** (require `AdmiralsPanelNative.dll`):
  - `ap.healn <player> <amount>` — heal, clamped to MaxHealth.
  - `ap.damagen <player> <amount>` — real damage (HP drops).
  - `ap.killn <player>` — Health = 0.
  - `ap.feedn <player>` — refill Health + Stamina.
  - `ap.reviven <player>` — Health = MaxHealth (works on dead players).
  - `ap.setattrn <player> <attr> <value>` — write any of the ~100 `R5AttributeSet` fields (`MaxHealth`, `Armor`, `Damage`, damage-type multipliers, crit stats, etc.).
  - `ap.readattrn <player> <attr>` — diagnostic read.
  - `ap.findn <player>`, `ap.inspectn <player>` — debug helpers used during reverse engineering.
- [`docs/native-mod.md`](docs/native-mod.md) — full writeup of the GAS reverse engineering, struct layouts, and function surface.
- `install.ps1` auto-detects `cpp/dist/main.dll` and deploys it to the UE4SS native mods folder with `enabled.txt` + `mods.txt` registration.

### Changed

- All `ap.*n` commands have graceful fallback: if the native DLL isn't installed, they return an explanation instead of crashing.
- Re-running `install.ps1` remains idempotent; the native step is additive.

### Technical notes

The ApplyDamage / TakeDamage UFunction route was tested and abandoned — Windrose's override returns the damage amount but doesn't actually apply it. Health lives behind `FGameplayAttributeData` (vtable + 2 floats = 16 bytes) on `PlayerState.R5AttributeSet`, discovered via a reflection-based property inspector and unlocked by reinterpreting memory at the right offsets. See `docs/native-mod.md`.

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
