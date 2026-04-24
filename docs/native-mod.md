# AdmiralsPanelNative (C++ companion mod)

## What it is

A thin UE4SS C++ mod loaded alongside the Lua `AdmiralsPanel` mod. It exposes a few functions that Windrose's Lua surface can't safely execute — mostly direct writes to GAS (Gameplay Ability System) attribute structs, plus helpers that need native reflection.

Without it, AdmiralsPanel still works — you just lose the `ap.*n` commands (heal, damage, kill, feed, revive, setattr, readattr, find, inspect).

## Why it exists

Windrose uses Unreal's GAS. Player Health, Stamina, and ~100 other attributes live on a struct called `R5AttributeSet`, attached to `PlayerState`. Each attribute is an `FGameplayAttributeData`:

```cpp
struct FGameplayAttributeData {
    void* _vtable;      // offset 0 — virtual destructor's vtable pointer
    float BaseValue;    // offset 8
    float CurrentValue; // offset 12
};  // 16 bytes
```

From UE4SS-Lua, property reads on these surface as an opaque `UObject` proxy. You can't reliably read or write the floats without raw memory access, which a Lua mod can't safely do.

A small C++ mod can. It:

1. Iterates UObjects via `UObjectGlobals::ForEachUObject` to find a `PlayerController` whose `PlayerState.PlayerNamePrivate` matches the target.
2. Gets the pawn (`PlayerController.Pawn`) and the attribute set (`PlayerState.R5AttributeSet`).
3. Uses property reflection (`GetPropertyByNameInChain("Health")->GetOffset_Internal()`) to get the runtime offset for the requested attribute, for forward-compat across patches.
4. Reinterprets the bytes at that offset as `FGameplayAttributeData` and writes `CurrentValue` (and `BaseValue`). The change is server-authoritative and replicates to clients automatically.

## What it doesn't do

- It does NOT call `ApplyDamage` / `TakeDamage`. Those UFunctions return a damage amount but Windrose's `TakeDamage` override is effectively a no-op — HP doesn't move.
- It does NOT call `ClientMessage` or other Client-prefixed RPCs. UE4SS's argument marshalling for RPCs is fragile and has crashed the server in tests.
- It does NOT modify position — `K2_TeleportTo` works fine from pure Lua and we use that in `ap.tp` / `ap.tpxyz`.
- It does NOT modify inventory. Not yet.

## Function surface (what the DLL exports to Lua)

All of these live in whichever Lua state loaded the mod (registered in `on_lua_start` for every mod that starts).

| Lua global                            | Arguments                     | Returns (string)                                       |
|---------------------------------------|-------------------------------|--------------------------------------------------------|
| `admiralspanel_native_version()`      | —                             | `"0.5.0"`                                              |
| `admiralspanel_native_find(name)`     | player name                   | `"found: <pawn full name>"` or `"not found"`           |
| `admiralspanel_native_inspect(name)`  | player name                   | Dumps R5Character + HealthComponent + R5AttributeSet + a few other classes with offsets and CPP types — big output |
| `admiralspanel_native_heal(name, amt)`   | name, float                | `"Health X -> Y (max Z) OK"`                           |
| `admiralspanel_native_damage(name, amt)` | name, float                | `"Health X -> Y (took N damage) OK"`                   |
| `admiralspanel_native_kill(name)`     | name                          | `"Health set to 0 ..."`                                |
| `admiralspanel_native_feed(name)`     | name                          | `"Health X -> Max; Stamina Y -> Max; "`                |
| `admiralspanel_native_revive(name)`   | name                          | `"Health X -> Max (full revive) OK"`                   |
| `admiralspanel_native_setattr(name, attr, value)`   | name, string, float | `"<attr>: before -> after OK"`                |
| `admiralspanel_native_readattr(name, attr)`         | name, string       | `"<attr>: current=X base=Y"`                   |

The Lua `ap.*n` commands in `mod/init.lua` are thin wrappers around these, with graceful fallback messages if the DLL isn't loaded.

## Attribute names

Whatever exists on `UR5AttributeSet` is valid. Interesting ones:

- **Survival**: `Health`, `MaxHealth`, `TemporalHealth`, `PassiveHealthRegen`, `Stamina`, `MaxStamina`, `StaminaRegenRate`.
- **Combat**: `Damage`, `Heal`, `Armor`, `ArmorModifier`, `FinalDamageReductionByArmor`, `CriticalChanceBase`, `CriticalDamageDoneModifier`.
- **Damage types** (each has `Added` / `Modifier` / `Penalty` / `TakenResist` / `TakenWeakness` / `TakenBlockEffectiveness` variants): `Melee`, `Range`, `Cannon`, `Blunt`, `Slash`, `Pierce`, `Fire`, `Poison`, `Cursed`, `Corrupt`, `Holy`, `Crude`, `Bleed`.
- **Corruption**: `CorruptionStatus`, `MaxCorruptionStatus`, `CorruptionStatusDamage`.

Use `ap.inspectn <player>` to dump the full list on your server's build.

Posture attributes live on a different set (`PostureAttributeSet`); v0.2.0 only reads/writes `R5AttributeSet`. Adding Posture is trivial — follow the same pattern.

## Build

Building from source requires:

- Visual Studio 2022 Build Tools with C++ workload
- Git on PATH
- An Epic Games ↔ GitHub account link (UEPseudo is gated; see `cpp/README.md`)

Then:

```powershell
cd cpp
powershell -ExecutionPolicy Bypass -File build.ps1
```

Output: `cpp/dist/main.dll`. Run `install.ps1` from the repo root to deploy it.

Or skip the build — the prebuilt `main.dll` is attached to each GitHub release. Download, drop at `cpp/dist/main.dll`, run `install.ps1`.

## Safety / fragility

- Relies on `FGameplayAttributeData`'s layout (vtable + 2 floats). If UE patches the struct to add/remove fields, writes go to the wrong offset. Very unlikely; this struct has been stable in UE5.
- Relies on `UR5AttributeSet` existing and having the field names we reference. Windrose patches could add/rename. Writes via `GetPropertyByNameInChain` (dynamic lookup) rather than hardcoded offsets mean renames are the bigger risk than layout shifts.
- Relies on UE4SS's `UObjectGlobals::ForEachUObject` and `GetPropertyByNameInChain` / `ContainerPtrToValuePtr`. These are UE4SS public API; very stable.

If a Windrose patch breaks it: run `ap.inspectn <player>` with an updated inspector; the property names revealed tell us exactly what to adjust.

## Roadmap additions

### Give-item — random-loot variant shipped in v0.4.0

**Working path**: `ap.giveloot <player> [count]` teleports up to `count`
populated `R5LootActor` instances from the world to the player. The
player's `R5Ability_Loot_AutoPickup` auto-collects them on proximity.
Superseded for targeting use cases by `ap.giveitem` in v0.5.0 — keep
`ap.giveloot` when you want a quick bulk fetch of whatever's lying
around, use `ap.giveitem` when you want to hand over a specific item.

**How it works**:

1. `R5LootActor` is a native C++ class in `/Script/R5.` (not a BP), with
   a `LootView: TObjectPtr<UR5BLActor_DropView>` field at +0x310.
2. The world has 5-30 populated R5LootActors at any time — generated
   whenever mobs die or resource nodes break. Their `LootView` is
   non-null and references items in the central inventory subsystem.
3. Teleporting one to the player via its `K2_TeleportTo` UFunction
   fires the normal pickup flow: `R5Ability_Loot_AutoPickup` (granted
   to every player) magnets the actor toward them and calls the
   inventory-add chain internally. Items flow into the player's
   inventory, the loot actor despawns.
4. `ap.lootlistn` filters out empty (LootView=null) actors;
   `ap.giveloot` only teleports populated ones.

**What this does NOT give you**: targeting of a specific item (e.g. "give
Mancave 3x Bread"). We get whatever that specific loot actor held — in
testing, fiber, logs, plant fiber. For hunger-admin use cases you'd want
to target food specifically — see *deferred work* below.

### Give-item — source-class matching (shipped v0.5.0 — task #22)

**Working commands**: `ap.giveitem`, `ap.lootitems`, `ap.itemlist`, `ap.itemscan`, `ap.lootslots`.

We explored two approaches before landing on the shipped one:

1. **Decode `R5BLActor_DropView`'s slot chain**: abandoned — DropView slots
   don't contain direct item references. The chain is opaque from reflection.
2. **Scan LootView memory for `UR5BLInventoryItem` pointers / InternalIndex
   values**: tested empirically. Didn't find matches on live DropViews —
   the game's central inventory subsystem keeps item refs in a private
   table keyed by record IDs (strings like `622DBF23...|I|7|I|13650`)
   that isn't serialized through DropView memory.

**Shipped approach — source-class matching**:

1. The DropView's memory holds pointers to source-related UObjects. For
   tree drops, these are the source tree's `StaticMeshComponent`s at
   `LootView+0x180` (an 11-element pointer array). For other drop types
   it's a different layout.
2. We walk the DropView's `+0x28..+0x2C0` region looking for UObject-shaped
   pointers, validated against a snapshot of the live UObject table
   (`gather_live_uobjects`). This gives us crash-safe dereferencing — we
   never call virtual methods on random memory.
3. For each valid candidate, we walk its `Outer` chain (up to 6 hops)
   skipping generic container classes (`World`, `Level`, `GameEngine`,
   `R5BLIslandView`, `R5GOS_*`, etc) until we hit a specific source class.
4. If any candidate resolves to a non-generic class whose name matches the
   user's search (substring, case-insensitive), teleport that loot actor
   to the player's feet (offset 80cm ahead, 80cm down so it's outside the
   player capsule and auto-pickup can magnet it).
5. **Fallback**: if no candidate matches, teleport a random populated
   loot actor — same delivery mechanism as `ap.giveloot`.

### Coverage

- Confirmed to work for tree drops (Ficus, Palm variants).
- Doesn't work for gatherables (record-ID suffix `|GA|`) or drop types
  where the Outer chain only hits generic containers. Those fall through
  to the random-loot fallback.
- **Full coverage** would require finding the C++ rule dispatcher via
  pattern-scanning the game binary — see `project_giveitem_future_work`
  memory note. Deferred; not needed for v1 of the feature.

**Rule-system background (for anyone extending this)**: Windrose inventory
is rule-based. Direct add goes through `R5BLInventory_AddItemsRule` with
params struct `R5BLInventory_AddItems`:
```
R5BLInventory_AddItems (0x28):
  +0x00 InventoriesPaths : TArray (targets)
  +0x10 Reward : FR5BLReward
  +0x20 bShouldAddAllItems : bool
  +0x21 bDropExtraItems : bool
FR5BLReward (0x10):
  +0x00 ItemsStacks : TArray<R5BLItemsStackData>
R5BLItemsStackData (0x60):
  +0x00 Item : FR5BLItem
  +0x58 Count : int32
FR5BLItem (0x58):
  +0x00 ItemParams : TSoftObjectPtr<UR5BLInventoryItem>
  +0x28 Attributes : TArray
  +0x38 ItemId : FR5BLRecordId
  +0x48 Effects : TArray
```
— but the rule dispatcher is C++-only (no reflected UFunction executes a
rule). `R5BLInventory_AddItemsRule`, every `R5BL*View` class, and
`R5DataKeeperForServerCoop` all expose 0 UFunctions. Pattern-scanning the
game binary for the rule executor would unlock unconstrained
give-item-with-count, but wasn't needed for the shipped feature.

**BP pickup spawn (still broken)**: `BP_WaterPickup_*_C` classes crash in
`FinishSpawningActor` inside their construction script — the
`R5FoliageMeshComponent` init can't complete without a foliage-system
world context on the dedicated server. Vanilla UE classes
(`StaticMeshActor`) spawn cleanly, proving the spawn primitive is sound.
If we ever need to *create* populated loot actors (as opposed to re-using
ones the world already produced), this is the remaining blocker.

### Chat broadcast

RPC via `ClientMessage` crashes UE4SS-Lua argument marshalling. From C++ it
may work but needs careful `ProcessEvent` with an FString argument
struct. Alternatively: find a server-side broadcast UFunction (GameMode
level) that isn't an RPC.

### Mob admin / second attribute sets

Posture and RangeWeapon attribute sets are now reachable via
`ap.readattrn` / `ap.setattrn` (v0.3.1 walks `ASC.SpawnedAttributes`).
Extending to mobs (AI characters) follows the same pattern — they have
their own attribute sets on their `AR5AICharacter` state.
