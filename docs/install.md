# Install guide

## Prerequisites

1. **Windrose Dedicated Server** running on Windows. Get it via Steam (AppID for the dedicated server variant).
2. **[WindrosePlus](https://github.com/HumanGenome/WindrosePlus) v1.0.7 or later**, installed. AdmiralsPanel is a plugin on top of WindrosePlus — it does not replace it. If you haven't installed WindrosePlus yet:
   ```powershell
   # From the WindrosePlus zip, extract into your server folder and run:
   powershell -ExecutionPolicy Bypass -File install.ps1
   ```
3. **An RCON password set** in `windrose_plus.json` — not the default `changeme`. Edit the file, set a password, restart the server.

## Install AdmiralsPanel

```powershell
# 1. Clone or download the repo
git clone https://github.com/Mancavegaming/admiral-admin.git
cd admiral-admin

# 2. Run the installer
powershell -ExecutionPolicy Bypass -File install.ps1
```

The installer auto-detects your Windrose server folder in common Steam paths. If it can't find it, pass `-GameDir` explicitly:

```powershell
powershell -ExecutionPolicy Bypass -File install.ps1 -GameDir "D:\SteamLibrary\steamapps\common\Windrose Dedicated Server"
```

## What gets installed where

| Source | Destination | Notes |
|--------|-------------|-------|
| `mod/` | `R5\Binaries\Win64\ue4ss\Mods\WindrosePlus\Mods\admiral-admin\` | Lua mod. Registered in `mods_registry.json`. **Survives WindrosePlus updates.** |
| `web/` | `windrose_plus\server\web\admiral\` | Web panel static files. **WIPED by WindrosePlus updates — re-run install.ps1.** |
| (generated) | `windrose_plus\server\web\admiral.html` | Redirect helper for the shorter URL. |

## Verify install

1. Restart the server (or wait up to 30 seconds for the WindrosePlus file-watcher to hot-reload the mod).
2. Inspect `R5\Binaries\Win64\ue4ss\UE4SS.log` — you should see:
   ```
   [WindrosePlus:API] INFO: Mod command registered: ap.setmult
   [WindrosePlus:API] INFO: Mod command registered: ap.preset
   [WindrosePlus:API] INFO: Mod command registered: ap.say
   [WindrosePlus:API] INFO: Mod command registered: ap.bringall
   [WindrosePlus:API] INFO: Mod command registered: ap.adminlog
   [WindrosePlus:AdmiralsPanel] INFO: AdmiralsPanel v0.1.0 loaded - ...
   [WindrosePlus:Mods] INFO: Loaded: AdmiralsPanel v0.1.0
   ```
3. Start the WindrosePlus dashboard:
   ```
   <server_folder>\windrose_plus\start_dashboard.bat
   ```
4. Open `http://localhost:8780/` in a browser, log in with your RCON password.
5. Navigate to `http://localhost:8780/admiral.html` — you should see the panel.

## After updating WindrosePlus

The WindrosePlus installer wipes `server/web/` on every update. To restore the panel:

```powershell
cd admiral-admin
powershell -ExecutionPolicy Bypass -File install.ps1
```

Re-running is safe — everything is idempotent. Takes ~5 seconds. The Lua mod under `ue4ss\Mods\WindrosePlus\Mods\admiral-admin\` is NOT wiped by WindrosePlus updates, but re-running picks up any updates to the mod itself too.

## Uninstall

```powershell
# Remove the mod
Remove-Item -Recurse -Force "<server>\R5\Binaries\Win64\ue4ss\Mods\WindrosePlus\Mods\admiral-admin"

# Remove from registry
$path = "<server>\R5\Binaries\Win64\ue4ss\Mods\WindrosePlus\Mods\mods_registry.json"
$reg = @(Get-Content $path -Raw | ConvertFrom-Json) | Where-Object { $_ -ne "admiral-admin" }
ConvertTo-Json $reg -Compress | Set-Content $path

# Remove the web panel
Remove-Item -Recurse -Force "<server>\windrose_plus\server\web\admiral"
Remove-Item -Force "<server>\windrose_plus\server\web\admiral.html"
```

Restart the server to complete the unload.

## Troubleshooting

**The mod doesn't show up in `UE4SS.log`.**
Check `R5\Binaries\Win64\ue4ss\Mods\WindrosePlus\Mods\mods_registry.json` — it should include `"admiral-admin"`. If not, the installer had a problem — re-run it.

**`/admiral.html` returns 404.**
You installed the Lua mod but the web panel didn't copy (perhaps you ran install.ps1 from a partial clone). Re-run install.ps1 from the full repo.

**Commands return "Player 'X' not found" but I AM connected.**
Check the exact case — WindrosePlus's RCON is case-insensitive on player names via our helper, but if you've reconnected recently, WindrosePlus may briefly show a stale PlayerController. Wait 5-10 seconds after reconnect.

**"Authentication required" on every API call.**
The dashboard session cookie is missing or expired. Log into the dashboard at `http://localhost:8780/` first, then reload `/admiral.html`.

**Multiplier doesn't seem to apply in-game.**
Some multipliers may be baked at world load or level load rather than read per-event. Try a full server restart. If the value persists across restarts (check `windrose_plus.json`) but the in-game effect is still missing, that particular multiplier is in the "not live" bucket — open a GitHub issue with which one.

**"Command timed out (25s)".**
Windrose + WindrosePlus queues commands in a spool file the game Lua state polls. Under heavy load or mid-level-transition, processing can lag. Usually transient — re-try.
