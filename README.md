# AdmiralsPanel

> An open-source admin panel for Windrose dedicated servers. **v0.6+ runs
> standalone**; no WindrosePlus dependency required. Legacy sub-mod install
> still supported for existing users.

AdmiralsPanel gives you a web UI with buttons, sliders, and presets so you never have to type RCON commands again. Live player list, per-player speed, server multipliers (xp / weight / loot / stack / craft cost / harvest yield / crop speed / points-per-level), difficulty presets, announcements, heal/feed/kill/teleport buttons, item give, and a raw console fallback.

## Prerequisites

- A Windrose Dedicated Server install on Windows (Steam DS SKU).
- **UE4SS** installed at `R5/Binaries/Win64/ue4ss/`. Get the latest release from <https://github.com/UE4SS-RE/RE-UE4SS/releases/latest> and extract the contents into `R5/Binaries/Win64/`. The installer below will fail with a friendly message if UE4SS is missing.
- That's it for the standalone (default) install. The native DLL hosts its own HTTP server on port 8790 and writes `admiralspanel.json` with a fresh random password on first run.
- **Optional, legacy**: if you already run [WindrosePlus](https://github.com/HumanGenome/WindrosePlus) v1.0.7+ and want AdmiralsPanel served from its dashboard instead, pass `-WithWindrosePlus` to `install.ps1`.

## Install

From the server machine (PowerShell, Admin):

```powershell
git clone https://github.com/Mancavegaming/admiral-admin.git
cd admiral-admin
powershell -ExecutionPolicy Bypass -File install.ps1
```

Standalone install (default):

1. Finds your Windrose server directory (or use `-GameDir "C:\path\to\server"`).
2. Installs the Lua mod at `R5\Binaries\Win64\ue4ss\Mods\AdmiralsPanel\`.
3. Installs the web panel at `admiralspanel_data\web\` (served by the native HTTP server).
4. Installs `main.dll` (native HTTP server + UFunction bridge) at `R5\Binaries\Win64\ue4ss\Mods\AdmiralsPanelNative\`.
5. First server start writes `admiralspanel.json` with a random password. Log in at **<http://localhost:8790/>**.

Sub-mod install (legacy, under WindrosePlus):

```powershell
powershell -ExecutionPolicy Bypass -File install.ps1 -WithWindrosePlus
```

Drops into `WindrosePlus\Mods\admiral-admin\` and serves via WP's dashboard at `http://localhost:8780/admiral.html`. Use this if you already run WindrosePlus and want everything in one place.

Re-running is safe in either mode — idempotent.

### Getting the native DLL

Two options:

- **Download** — grab `AdmiralsPanelNative-<version>.dll` from the [latest release](https://github.com/Mancavegaming/admiral-admin/releases/latest), drop at `cpp/dist/main.dll`, re-run `install.ps1`.
- **Build** — requires VS 2022 Build Tools + an Epic Games ↔ GitHub account link (for the UEPseudo submodule). One command:
  ```powershell
  cd cpp; powershell -ExecutionPolicy Bypass -File build.ps1
  ```
  See [`cpp/README.md`](cpp/README.md) for details.

> **After you update WindrosePlus, re-run `install.ps1`.** WindrosePlus's installer wipes `server/web/` on every update, which removes our panel. Re-running restores it in ~5 seconds.

## Use (standalone, default)

1. Start (or restart) your Windrose dedicated server with your usual startup script (e.g. `StartServerForeground.bat`). The native DLL spins up its own HTTP server on port 8790.
2. The first server start writes `admiralspanel.json` at the server root with a random password.
3. Open `http://localhost:8790/` in a browser on the server (or from another machine if port 8790 is allowed through your firewall).
4. Log in with the password from `admiralspanel.json` ("password" field).

Verify the panel is up before opening the browser:
```powershell
curl http://localhost:8790/healthcheck
# expect: {"status":"ok","app":"AdmiralsPanel","version":"0.7.0"}
```

## What it can do (v0.7)

**World-tab multipliers** — all live, write directly to the game's reflected data assets:

| Slider | Targets |
|---|---|
| **XP** | `R5BLQuestParams.ExperienceCount` — 232 quests on a stock world |
| **Weight** | `R5WeightAttributeSet.MaxWeightCapacity` per online player |
| **Loot** | Slot counts on populated `R5LootActor` instances (post-spawn, 2s tick) |
| **Stack size** | `R5BLInventoryItemGPP.MaxCountInSlot` — 1268 item assets |
| **Craft cost** | `R5BLRecipeData.RecipeCost` array — 2257 recipes, ~3500 cost entries |
| **Harvest yield** | `R5BLLootData` + 5 other variants — 1500+ loot tables |
| **Crop speed** | `R5BLCropParams.GrowthDuration` — 22 crops |
| **Points / level** | `TalentPointsReward` + `StatPointsReward` per level entry |
| **Coop scale** | `Coop_StatsCorrectionModifier` WDS param (experimental knob) |

Each captures originals on first sight and restores cleanly when set to 1.0. **Loot and harvest_yield are capped at 4× in the UI** because the game runtime applies its own post-roll scaling; values higher than that don't take effect.

**Difficulty presets** — Vanilla, Easy, Hard, plus three weekend-event presets (2× XP, 2× Loot, 3× Points) and "Event: Chill" (relaxed build mode). One click applies a multiplier bundle; partial presets only touch their own keys.

**Players tab**:
- Live player list with positions, speed slider, "TP to last death" button.
- `ap.healn` / `ap.damagen` / `ap.killn` / `ap.feedn` / `ap.reviven` — direct GAS attribute writes.
- `ap.setattrn` / `ap.readattrn` — read/write any of ~150 `R5AttributeSet` fields (MaxHealth, Armor, Damage, all damage-type modifiers, crit stats, etc.).
- `ap.giveloot <player> [count]` — teleport populated `R5LootActor` instances to the player; their auto-pickup ability delivers the contents.
- `ap.giveitem <player> <search>` — teleport a loot actor whose contents match a substring (e.g. `ap.giveitem Mancave banana`).
- `ap.tp` / `ap.tpxyz` — server-authoritative teleport.

**Admin tools**:
- Announcements (writes to server log + `events.log`).
- Admin action log (every `ap.*` command persisted).
- Raw RCON console fallback.
- Full RE toolkit (`rawdumpn` / `dumpclassn` / `funcparamsn` / `scanpath` / `classprobe` / `findclassn` / `lootitems` / `itemlist` / `inspect`) for digging into `UObject` reflection.

**Known limitations**:
- In-game chat broadcast — `ClientMessage` is an RPC; we can't fire it cleanly through UE4SS yet.
- "Inventory size" and "Cooking speed" multipliers — the underlying systems either need TArray growth (crash risk) or live in non-reflected data; neither is shipped.
- Loot multiplier on tree-chop drops — auto-pickup races our 2s tick, so quick tree chops don't multiply. Use `harvest_yield` for that path; it modifies the loot table before it rolls.

## How it works

AdmiralsPanel (v0.6+) is three pieces:

- **`AdmiralsPanelNative.dll`** — UE4SS C++ mod. Hosts the HTTP server on port 8790, serves the web UI, handles login + session cookies, dispatches RCON commands into the Lua mod via a small file-based spool, AND does the heavy lifting (GAS attribute writes, loot-actor teleport, multiplier writebacks, UObject scans).
- **Lua mod** (`AdmiralsPanel`) — UE4SS Lua mod loaded by UE4SS itself. Registers the `ap.*` commands, handles the spool tick, persists multiplier values to `admiralspanel.json`.
- **Web UI** — single HTML/CSS/JS page (no build step, Tailwind via CDN), served by the native DLL.

No WindrosePlus dependency. No external dashboard. One process, one port, one config file.

See [`docs/architecture.md`](docs/architecture.md) for the details on the spool protocol and the native bridge.

## Contributing

Issues and PRs welcome. Read [`docs/contributing.md`](docs/contributing.md) and [`docs/roadmap.md`](docs/roadmap.md) first.

The biggest open item is the **C++ companion mod** — whoever gets `Teleport` and `AddItem` working through UE4SS native hooks first unblocks the rest of the feature set.

## License

MIT. See [LICENSE](LICENSE).

## Acknowledgements

- [WindrosePlus](https://github.com/HumanGenome/WindrosePlus) by HumanGenome — AdmiralsPanel is effectively a plugin on top of WindrosePlus. Without it we'd be writing our own RCON, HTTP server, and mod loader. Go star it.
- [UE4SS-RE/RE-UE4SS](https://github.com/UE4SS-RE/RE-UE4SS) — the injection layer that makes all of this possible.
- Kraken Express for shipping a Dedicated Server SKU that permits this kind of self-hosting.
