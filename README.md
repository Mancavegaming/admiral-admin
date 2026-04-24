# AdmiralsPanel

> An open-source admin panel for Windrose dedicated servers. Built on top of [WindrosePlus](https://github.com/HumanGenome/WindrosePlus).

AdmiralsPanel gives you a web UI with buttons, sliders, and presets so you never have to type `wp.speed mancave 1.5` again. Live player list, per-player speed, server multipliers, difficulty presets, announcements, and a raw console fallback.

![screenshot placeholder](docs/screenshots/panel.png)

## Prerequisites

- A Windrose Dedicated Server install on Windows.
- [WindrosePlus](https://github.com/HumanGenome/WindrosePlus) v1.0.7 or later, installed and running. (AdmiralsPanel extends it — it does not replace it.)
- An RCON password set in `windrose_plus.json` (not `changeme`).

## Install

From the server machine (PowerShell, Admin):

```powershell
git clone https://github.com/Mancavegaming/admiral-admin.git
cd admiral-admin
powershell -ExecutionPolicy Bypass -File install.ps1
```

The installer:

1. Finds your Windrose server directory (or use `-GameDir "C:\path\to\server"`).
2. Copies the Lua mod into `R5\Binaries\Win64\ue4ss\Mods\WindrosePlus\Mods\admiral-admin\`.
3. Copies the web panel into `windrose_plus\server\web\admiral\`.
4. If `cpp\dist\main.dll` exists (either prebuilt from a release or built via `cpp\build.ps1`), deploys it to `R5\Binaries\Win64\ue4ss\Mods\AdmiralsPanelNative\` with `enabled.txt` + `mods.txt` registration. Skips gracefully otherwise.

Re-running is safe — everything is idempotent.

### Getting the native DLL

Two options:

- **Download** — grab `AdmiralsPanelNative-<version>.dll` from the [latest release](https://github.com/Mancavegaming/admiral-admin/releases/latest), drop at `cpp/dist/main.dll`, re-run `install.ps1`.
- **Build** — requires VS 2022 Build Tools + an Epic Games ↔ GitHub account link (for the UEPseudo submodule). One command:
  ```powershell
  cd cpp; powershell -ExecutionPolicy Bypass -File build.ps1
  ```
  See [`cpp/README.md`](cpp/README.md) for details.

> **After you update WindrosePlus, re-run `install.ps1`.** WindrosePlus's installer wipes `server/web/` on every update, which removes our panel. Re-running restores it in ~5 seconds.

## Use

1. Start your server with WindrosePlus's `StartWindrosePlusServer.bat` as usual.
2. Start the WindrosePlus dashboard with `windrose_plus\start_dashboard.bat`.
3. Open your browser to `http://localhost:8780/`, log in with your RCON password.
4. Navigate to `http://localhost:8780/admiral.html` (or the direct URL `http://localhost:8780/admiral/index.html`).

## What it can do (v0.2)

**Pure Lua** (no extra install):

- Per-player speed slider (via WindrosePlus's `wp.speed`).
- Server-wide multipliers: loot, XP, weight, craft cost, stack size, crop/cooking speed, inventory size, points per level, harvest yield. Persisted to `windrose_plus.json`.
- Difficulty presets: Vanilla / Easy / Hard / Event: 2× XP / Event: 2× Loot / Event: Chill.
- **Teleport** (`ap.tp`, `ap.tpxyz`) — server-authoritative via `K2_TeleportTo`.
- Announcements, live player list, admin log, raw RCON console.

**With the optional `AdmiralsPanelNative.dll`** (C++ companion, see [`cpp/`](cpp/)):

- `ap.healn` / `ap.damagen` / `ap.killn` / `ap.feedn` / `ap.reviven` — real health mods via direct GAS attribute writes (server-authoritative, replicates to clients).
- `ap.setattrn` / `ap.readattrn` — touch any of the ~100 `R5AttributeSet` fields (MaxHealth, Armor, Damage, all damage-type multipliers, crit stats, etc.). "God mode" and "500 damage/hit" are one command each.
- `ap.giveloot <player> [count]` — teleport populated `R5LootActor` instances in the world to a player; auto-pickup delivers their contents (fiber, wood, food, etc. — whatever was in the loot actor).
- `ap.yankactorn` / `ap.spawnn` / `ap.lootlistn` / `ap.lootinspectn` and a full RE toolkit (`rawdumpn` / `dumpclassn` / `funcparamsn` / `scanpath` / `findclassn` / `locn`) for further reverse-engineering.
- See [`docs/native-mod.md`](docs/native-mod.md) for the technical writeup, including the give-item architecture and what's needed for specific-item targeting.

**Still not done** (v0.4 roadmap):

- Specific-item give (`ap.giveitem Mancave bread`) — `ap.giveloot` delivers random loot; specific-item requires parsing the `R5BLActor_DropView` non-reflected memory to identify what each loot actor holds. Partially mapped; deferred.
- In-game chat broadcast — `ClientMessage` is an RPC and UE4SS struggles with it. Looking for a non-RPC path.
- UI buttons for the native commands — right now they work via the dashboard console only.

## How it works

AdmiralsPanel is two pieces:

- **Lua mod** — loaded by WindrosePlus. Adds new `ap.*` RCON commands.
- **Web UI** — a single HTML page (no build step; Tailwind via CDN). Served by the WindrosePlus dashboard at `/admiral/`. Reuses the dashboard's login cookie — no auth to configure.

See [`docs/architecture.md`](docs/architecture.md) for details.

## Contributing

Issues and PRs welcome. Read [`docs/contributing.md`](docs/contributing.md) and [`docs/roadmap.md`](docs/roadmap.md) first.

The biggest open item is the **C++ companion mod** — whoever gets `Teleport` and `AddItem` working through UE4SS native hooks first unblocks the rest of the feature set.

## License

MIT. See [LICENSE](LICENSE).

## Acknowledgements

- [WindrosePlus](https://github.com/HumanGenome/WindrosePlus) by HumanGenome — AdmiralsPanel is effectively a plugin on top of WindrosePlus. Without it we'd be writing our own RCON, HTTP server, and mod loader. Go star it.
- [UE4SS-RE/RE-UE4SS](https://github.com/UE4SS-RE/RE-UE4SS) — the injection layer that makes all of this possible.
- Kraken Express for shipping a Dedicated Server SKU that permits this kind of self-hosting.
