# Contributing

Issues and PRs welcome. This is a small project; process is light.

## Before opening an issue

- Search existing issues first.
- For bugs: include your Windrose build version (`wp.status` shows it), WindrosePlus version, and `UE4SS.log` excerpt (not the whole file).
- For "X multiplier doesn't apply live": read [commands.md](commands.md) first — this is an expected gap for some multipliers.

## Dev setup

You need a live Windrose server with WindrosePlus to test against. There is no unit-test harness — everything is verified against a running game.

### Editing Lua

`mod/init.lua` is loaded by WindrosePlus's mod loader. To test changes locally:

```powershell
# From the repo root after editing
powershell -ExecutionPolicy Bypass -File install.ps1
```

WindrosePlus polls for mod changes every 30 seconds and hot-reloads. For certainty, restart the server after a change.

**Caveat**: WindrosePlus's hot-reload signature is `(length, first_byte, last_byte)` — edits that preserve all three won't trigger. If your change doesn't take effect after 30 seconds, restart the server.

### Editing the web UI

`web/*.html/.js/.css` are static. Edit, then re-run `install.ps1` to copy into the dashboard's serve directory. Browser hard-refresh (Ctrl+F5) to bust cache.

There's no build step. Tailwind loads via CDN. If you want to introduce a build tool, please discuss in an issue first — we'd like to keep the "download, run one script, no tooling" install story.

### Testing an RCON command

From the dashboard console (`http://localhost:8780/` → Console tab) or via curl with a cookie jar:

```powershell
$pw = "<your RCON password>"
$s = New-Object Microsoft.PowerShell.Commands.WebRequestSession
$null = Invoke-WebRequest -Uri "http://localhost:8780/login" -Method Post `
    -Body "password=$pw" -ContentType "application/x-www-form-urlencoded" `
    -WebSession $s -UseBasicParsing -MaximumRedirection 0 -ErrorAction SilentlyContinue

Invoke-RestMethod -Uri "http://localhost:8780/api/rcon" -Method Post `
    -Body (@{command="ap.setmult xp 2"} | ConvertTo-Json) `
    -ContentType "application/json" -WebSession $s
```

## Code style

- Lua: two-space indent, snake_case for variables, pcall anything that touches the game.
- JS: two-space indent, no semicolons on statement boundaries is fine; use const/let, no var. Vanilla DOM — no framework.
- PowerShell: verb-noun cmdlet style, **ASCII only in .ps1 files** (PS 5.1 chokes on UTF-8 em-dashes without BOM).

## Commit messages

No strict convention. Clear and concise:

```
ap.preset: fix log entry showing "err" on success

The `a and b or c` idiom returned the "error" branch when b=nil.
Replaced with an explicit if/else.
```

## Things that need help

See [roadmap.md](roadmap.md). The v0.2 C++ companion mod is the highest-impact contribution.

## License

MIT. Contributions are licensed under the same.
