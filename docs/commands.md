# RCON command reference

AdmiralsPanel adds these commands to WindrosePlus's RCON layer. All are callable via the dashboard console, the `/api/rcon` HTTP endpoint, or from the `/admiral/` web panel.

## ap.setmult

```
ap.setmult <key> <value>
```

Set one server-wide multiplier. Writes the value to `R5GameMode` directly (for live effect), persists to `windrose_plus.json`, and reloads the WindrosePlus in-memory config.

**Valid keys** (match the keys in `windrose_plus.json → multipliers`):

| Key | Description | Candidate UE property |
|-----|-------------|-----------------------|
| `loot` | Drop multiplier | `LootMultiplier` |
| `xp` | Experience multiplier | `XPMultiplier`, `ExperienceMultiplier` |
| `weight` | Carry weight multiplier | `WeightMultiplier` |
| `craft_cost` | Crafting cost multiplier | `CraftCostMultiplier` |
| `stack_size` | Inventory stack size | `StackSizeMultiplier` |
| `crop_speed` | Crop growth speed | `CropGrowthMultiplier`, `CropSpeedMultiplier` |
| `cooking_speed` | Cooking speed | `CookingSpeedMultiplier`, `CookingSpeed` |
| `inventory_size` | Inventory size | `InventorySizeMultiplier` |
| `points_per_level` | Skill points per level | `PointsPerLevelMultiplier` |
| `harvest_yield` | Harvest yield | `HarvestMultiplier`, `HarvestYieldMultiplier` |

**Value range**: any non-negative number below 1000. Ranges inside the web UI are stricter; raw RCON bypasses those for events.

**Output**:
```
> ap.setmult xp 3
xp = 3  (live: XPMultiplier, ExperienceMultiplier)
```

The `(live: ...)` tag lists which UE properties accepted the write. A value of `(saved; applies on next restart)` means persistence worked but no live property took it — set the key once via RCON and restart to apply.

### Which multipliers are "live"?

Determined empirically per game build. As of Windrose 0.10.0.3.104:

- Confirmed live via R5GameMode property write: `loot`, `xp`.
- Others: not yet confirmed — the property writes return "live" (the setter accepted), but whether the engine actually reads that property at the right moment isn't verified. **Rule of thumb**: if a multiplier doesn't seem to take effect, restart the server.

Contributions welcome to document this matrix more thoroughly — open an issue with your findings.

## ap.preset

```
ap.preset <name>
```

Apply a named multiplier bundle from `mod/data/presets.json`. The preset file is re-read on each invocation, so edits apply without restarting the mod.

**Built-in presets**:

- `vanilla` — all multipliers reset to 1.0.
- `easy` — more loot / XP, lighter carry, bigger stacks.
- `hard` — scarcer loot, heavier carry, costlier crafting.
- `event-2xxp` — double XP only; other multipliers unchanged.
- `event-loot` — double loot only; other multipliers unchanged.
- `event-chill` — half craft cost / weight, double cooking / stacks / crop speed.

`vanilla` / `easy` / `hard` are **full bundles** (10 keys set). `event-*` are **partial** (only the listed keys; anything not mentioned keeps its current value).

**Adding your own presets**:

Edit `mod/data/presets.json`:

```json
{
    "my-brutal": {
        "description": "Like hard, but worse.",
        "multipliers": {
            "loot": 0.4, "xp": 0.5, "weight": 2, "craft_cost": 2
        }
    }
}
```

No restart or mod reload needed — the file is read fresh each time you run `ap.preset`.

To expose your custom preset in the web panel, also add an entry to the `PRESETS` array in `web/app.js` (hardcoded for v0.1).

## ap.say

```
ap.say <message>
```

Logs a broadcast message to the WindrosePlus server log and to `windrose_plus_data\events.log`.

**Limitations**: does NOT deliver to in-game chat. Windrose does not currently expose a chat broadcast UFunction to Lua. This is tracked in [roadmap.md](roadmap.md) under the C++ companion mod item.

The web panel's "Announce" tab shows recent `ap.say` entries (scraped from `ap.adminlog`).

## ap.bringall

```
ap.bringall
```

Sets `CheatMovementSpeedModifer = 1` on every online player's `CharacterMovement` component. Useful after testing `wp.speed` on multiple players to quickly normalize.

**Note**: This only resets the cheat modifier. If `wp.speed` was used on a player (which also writes `MaxWalkSpeed = base * mult`), that baseline stays. For a full reset per player, use `wp.speed <player> 1` which has WindrosePlus's baseline restoration logic.

## ap.adminlog

```
ap.adminlog [limit]
```

Show recent AdmiralsPanel admin actions. Default limit is 25, max 200. Entries are written by every other `ap.*` command and persisted to `windrose_plus_data\admiral_admin_log.json`.

**Output**:
```
Recent admin actions:
  [2026-04-22 15:10:28] ap.setmult loot 2 -> loot = 2  (live: LootMultiplier)
  [2026-04-22 15:10:35] ap.preset vanilla -> Applied preset 'vanilla': ...
  [2026-04-22 15:11:42] ap.say Weekend event starts! -> logged
```

## ap.tp / ap.tpxyz — teleport

```
ap.tp <player> <target>
ap.tpxyz <player> <x> <y> <z>
```

Server-authoritative teleport via `K2_TeleportTo`. No C++ required. Replicates to clients, no rubber-banding in practice.

## Native commands (require `AdmiralsPanelNative.dll`)

These call into the optional C++ companion mod. See [`native-mod.md`](native-mod.md). If the DLL isn't installed, each returns `"Native feature unavailable (install AdmiralsPanelNative.dll — see cpp/README.md)"`.

### ap.healn / ap.damagen / ap.killn / ap.feedn / ap.reviven

```
ap.healn <player> <amount>     -- heal, clamped to MaxHealth
ap.damagen <player> <amount>   -- actual damage (HP drops)
ap.killn <player>              -- Health = 0
ap.feedn <player>              -- Health + Stamina -> their max
ap.reviven <player>            -- Health -> MaxHealth, works on dead players
```

All write directly to `PlayerState.R5AttributeSet.{Health, Stamina, ...}` — server-authoritative, replicates to clients.

### ap.setattrn / ap.readattrn — generic

```
ap.setattrn <player> <attr> <value>
ap.readattrn <player> <attr>
```

Set or read any attribute on `R5AttributeSet`. Useful attributes:

- Survival: `Health`, `MaxHealth`, `Stamina`, `MaxStamina`, `StaminaRegenRate`
- Combat: `Damage`, `Armor`, `CriticalChanceBase`, `CriticalDamageDoneModifier`
- Damage-type multipliers: `FireDamageDoneModifier`, `BluntDamageTakenResist`, etc. (per type: Melee / Range / Cannon / Blunt / Slash / Pierce / Fire / Poison / Cursed / Corrupt / Holy / Crude / Bleed, each with Added/Modifier/Penalty/TakenResist/Weakness/BlockEffectiveness variants)

Full list: run `ap.inspectn <player>` — the "R5AttributeSet" block lists every attribute on your build.

**God-mode example:**
```
ap.setattrn Mancave MaxHealth 99999
ap.setattrn Mancave Armor 99999
ap.setattrn Mancave Damage 500
```

### ap.findn / ap.inspectn — debug

```
ap.findn <player>       -- returns pawn full-name or "not found"
ap.inspectn <player>    -- full property dump (R5Character + HealthComponent + R5AttributeSet + PlayerState + ASC). Big output.
```

Useful for discovering new fields after a game patch.

## Reused WindrosePlus commands (not ours)

These come from stock WindrosePlus and are listed here because the web UI calls them:

- `wp.speed [player] <multiplier>` — per-player speed.
- `wp.reload` — re-read `windrose_plus.json` from disk (our `ap.setmult` calls this internally).
- `wp.players`, `wp.pos`, `wp.health`, `wp.status`, `wp.uptime`, `wp.help`.

See WindrosePlus's [own command reference](https://github.com/HumanGenome/WindrosePlus/blob/main/docs/commands.md) for the full list.
