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
4. Registers the mod in `mods_registry.json`.

Re-running is safe — everything is idempotent.

> **After you update WindrosePlus, re-run `install.ps1`.** WindrosePlus's installer wipes `server/web/` on every update, which removes our panel. Re-running restores it in ~5 seconds.

## Use

1. Start your server with WindrosePlus's `StartWindrosePlusServer.bat` as usual.
2. Start the WindrosePlus dashboard with `windrose_plus\start_dashboard.bat`.
3. Open your browser to `http://localhost:8780/`, log in with your RCON password.
4. Navigate to `http://localhost:8780/admiral.html` (or the direct URL `http://localhost:8780/admiral/index.html`).

## What it can (and can't) do in v0.1

**Can do** (everything below works without a game patch or C++ mod):

- Per-player speed (the same `wp.speed` WindrosePlus ships — now with a slider UI).
- Server-wide multipliers: loot, XP, weight, craft cost, stack size, crop speed, cooking speed, inventory size, points per level. Saved to `windrose_plus.json` so they persist across restarts.
- Difficulty presets: Vanilla / Easy / Hard / Event: 2× XP / Event: 2× Loot / Event: Chill.
- Announcement log (broadcasts to server log and `events.log` — see below for why not in-game chat).
- Live player list with position.
- Raw RCON console for anything the UI doesn't expose yet.

**Can't do in v0.1** (genuinely blocked by the game's current Lua surface — not a missing feature, a hard limit):

- Heal, kill, feed (hunger / thirst / stamina), give item, teleport, in-game chat broadcast.
- These all require calling game UFunctions, which Windrose (in this build) doesn't expose to Lua. A future companion C++ UE4SS mod will unlock them. See [`docs/roadmap.md`](docs/roadmap.md).

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
