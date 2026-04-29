# spawn-item

Server-side item spawn tool for Windrose dedicated servers. Inserts items
directly into a chest's inventory by modifying the world RocksDB save file.

## Why this tool exists

The natural give-item path (teleport a R5LootActor to a player + auto-pickup)
does not commit items to the server-side authoritative inventory — items
appear briefly in the HUD (client prediction) and are lost on next save tick.
Direct memory writes to inventory views also do not propagate to clients
because Windrose replication is via UPROPERTY/RPC channels, not raw memory.

The reliable path is to write items into the chest persistence layer
(`R5BLActor_BuildingBlock` column family in the world RocksDB), which the
engine reads when a chest is opened in-game.

## Requirements

- Python 3.11+
- `pip install rocksdict pymongo`
- Server must be stopped while modifying the DB (the tool handles this)

## Quick start

```powershell
# Spawn 999 Wood into a known chest (by 32-char hex key)
python spawn_item.py `
  --chest A19BDE2BCE1847084BB948AC1B79646A `
  --item-substr Wood_T01 --count 999

# Spawn 50 Healing Potions into a chest identified by a marker item
# (the chest's slot 0 must contain a "ScallopShell" item)
python spawn_item.py `
  --marker ScallopShell `
  --item-substr Healing_T01 --count 50

# Dry run — resolve targets, do not write or restart
python spawn_item.py --chest A19BDE2BCE... --item-substr Wood --count 999 --dry-run
```

## How chest selection works

| Mode | Flag | Use when |
|---|---|---|
| Exact key | `--chest <32-hex>` | You know the chest's BuildingBlock record key |
| Marker item | `--marker <substring> [--marker-slot N]` | You placed a marker item in a known slot of the target chest |

## Item resolution

| Mode | Flag |
|---|---|
| Exact asset path | `--item-path /R5BusinessRules/.../DA_X.DA_X` |
| Substring match (against paths in BuildingBlock CF) | `--item-substr <substr>` |

The substring resolver only finds items that already exist somewhere in any
chest in the world. If you need to spawn an item nobody has yet, use
`--item-path` with the full asset path. The full list of asset paths is in
`/R5BusinessRules/InventoryItems/...` in the game's pak files; you can also
list them via `ap.itemlist <substring>` from the AdmiralsPanel chat command
while the server is running.

## What the tool does (operationally)

1. Stops the server (force-kill — Windrose's RocksDB has Snappy disabled, so
   any running server holds an exclusive LOCK we need to release)
2. Opens the RocksDB with `compression=none`
3. Resolves target chest and item path
4. Decodes the chest's BSON value to a Python dict
5. Finds the first slot with `Count == 0`
6. Sets `Count`, `Item.ItemParams`, and `Item.ItemId` (a fresh UUID hex)
7. Re-encodes BSON, writes back to the CF
8. Cleans up rocksdict's config artifact
9. Restarts the server
10. Polls the panel until it responds

Total downtime: ~10 seconds per spawn.

## Environment overrides

| Variable | Default |
|---|---|
| `WR_GAME_DIR` | `C:\Program Files (x86)\Steam\steamapps\common\Windrose Dedicated Server` |
| `WR_WORLD_ID` | `622DBF23C4884997D28DE50FDFABC064` |
| `WR_PANEL_URL` | `http://127.0.0.1:8790/api/status` |

## Caveats

- **No compression**: writes MUST use `DBCompressionType.none()`. Any compressed
  write makes the next server start crash with `Not implemented: Snappy not
  supported in this build`.
- **Slot capacity**: each chest has 16 slots. If all are filled, the tool
  errors out — destroy/empty something first.
- **Item types vs. instances**: the `ItemId` field is per-instance (any random
  GUID is fine). The `ItemParams` asset path is what determines the item
  TYPE the engine renders.
- **Chest persistence cycle**: the chest writes its in-memory state back to
  the DB on chest-close, autosave, and shutdown. If a player has the chest
  open while we modify it, behavior is undefined — wait for everyone to
  close their chests first.
