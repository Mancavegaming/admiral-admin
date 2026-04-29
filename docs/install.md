# Install guide

AdmiralsPanel v0.6+ ships **standalone** — the panel's own native DLL hosts an HTTP server on port 8790. No WindrosePlus needed. Re-running the installer is always safe; it is idempotent.

## TL;DR

```powershell
git clone https://github.com/Mancavegaming/admiral-admin.git
cd admiral-admin
powershell -ExecutionPolicy Bypass -File install.ps1
# restart the server, then open http://localhost:8790/
```

The installer auto-detects your Windrose server folder in common Steam paths. If it cannot, pass `-GameDir`:

```powershell
powershell -ExecutionPolicy Bypass -File install.ps1 -GameDir "D:\SteamLibrary\steamapps\common\Windrose Dedicated Server"
```

## Prerequisites

1. **Windows server** running the Windrose Dedicated Server (Steam SKU).
2. **UE4SS** installed at `R5\Binaries\Win64\ue4ss\`. Latest release: <https://github.com/UE4SS-RE/RE-UE4SS/releases/latest>. Extract the contents of the release zip into `R5\Binaries\Win64\`. The installer fails with a friendly message and a link if UE4SS is missing.
3. **The native DLL** at `cpp\dist\main.dll` inside the cloned repo. Two ways to get it — see "Native DLL" below.
4. **PowerShell 5.1+** (ships with Windows Server 2019/2022/2025). The installer file is strict ASCII so PS 5.1's UTF-8-without-BOM quirk does not bite.
5. **Python 3.11+** (for the v0.8 Spawn feature). <https://www.python.org/downloads/> — tick "Add Python to PATH" during install. The installer detects Python, runs `pip install rocksdict pymongo`, and registers a Scheduled Task for the spawn sidecar. If Python is missing the installer warns and skips — the rest of the panel still works. To opt out explicitly, pass `-SkipSidecar` to `install.ps1`.

## What gets installed where (standalone)

| Source | Destination | Notes |
|--------|-------------|-------|
| `mod\init.lua` + `mod\Scripts\` + `mod\data\` + `mod\lib\` | `R5\Binaries\Win64\ue4ss\Mods\AdmiralsPanel\Scripts\` | Lua mod. Survives server restarts. |
| `cpp\dist\main.dll` | `R5\Binaries\Win64\ue4ss\Mods\AdmiralsPanelNative\dlls\main.dll` | Native HTTP server + GAS / loot bridges. |
| (writes) | `R5\Binaries\Win64\ue4ss\Mods\mods.txt` | Adds `AdmiralsPanelNative : 1` if absent. |
| `web\*` | `admiralspanel_data\web\` | Static web UI files served by the native DLL. |
| `tools\spawn-item\*.py` | `admiralspanel_data\tools\spawn-item\` | Spawn-feature CLI + sidecar (v0.8+). |
| (Scheduled Task) | `AdmiralsPanel-Spawn-Sidecar` | Auto-runs `sidecar.py` at boot + logon. |
| (generated on first server start) | `admiralspanel.json` (server root) | Login password + multiplier persistence. |

## Native DLL

The DLL is required — without it the HTTP server does not start and the panel does not load. Two ways:

- **Download (recommended for users):** grab `AdmiralsPanelNative-<version>.dll` from [the latest release](https://github.com/Mancavegaming/admiral-admin/releases/latest), rename to `main.dll`, and drop it at `cpp\dist\main.dll` in the cloned repo. Then run `install.ps1`.
- **Build from source (developers):** requires Visual Studio 2022 Build Tools with the C++ workload + Windows SDK + an Epic Games / GitHub account link (UEPseudo submodule). One command:
  ```powershell
  cd cpp
  powershell -ExecutionPolicy Bypass -File build.ps1
  ```
  See `cpp\README.md` for the full toolchain list.

## Verify the install

After running `install.ps1`, restart the server and wait ~30 seconds for the world to load. Then:

```powershell
curl http://localhost:8790/healthcheck
# expect: {"status":"ok","app":"AdmiralsPanel","version":"0.8.0"}
```

If you get `Unable to connect`, check in this order:

1. **Server actually started?** `tasklist /FI "IMAGENAME eq WindroseServer-Win64-Shipping.exe"` should show a process.
2. **DLL deployed?** Check `R5\Binaries\Win64\ue4ss\Mods\AdmiralsPanelNative\dlls\main.dll` exists and has a recent timestamp.
3. **Mod enabled?** `R5\Binaries\Win64\ue4ss\Mods\mods.txt` should contain `AdmiralsPanelNative : 1` (the installer adds it).
4. **Port collision?** If something else is on 8790, edit the `http_port` field in `admiralspanel.json` and restart the server.
5. **Server log:** `R5\Saved\Logs\R5.log` — search for `AdmiralsPanel`. Errors there will name the issue.

Once `/healthcheck` responds, open `http://localhost:8790/` and log in with the password from `admiralspanel.json`.

## Updating

```powershell
cd admiral-admin
git pull
powershell -ExecutionPolicy Bypass -File install.ps1
```

Re-running the installer overwrites Lua scripts and web assets in place. The DLL is overwritten too if you've staged a newer one; if the server is running it will detect the lock and either no-op (file unchanged) or warn (file differs — stop the server first).

Multiplier values in `admiralspanel.json` are preserved across updates.

## Sub-mod install (legacy, optional)

If you already run [WindrosePlus](https://github.com/HumanGenome/WindrosePlus) v1.0.7+ and want AdmiralsPanel served from WP's dashboard at `http://localhost:8780/admiral.html` instead of the standalone server:

```powershell
powershell -ExecutionPolicy Bypass -File install.ps1 -WithWindrosePlus
```

The Lua mod lands at `R5\Binaries\Win64\ue4ss\Mods\WindrosePlus\Mods\admiral-admin\` (registered in `mods_registry.json`) and the web UI at `windrose_plus\server\web\admiral\`. WindrosePlus's installer wipes `server\web\` on every WP update — re-run `install.ps1 -WithWindrosePlus` afterwards to restore the panel (~5 seconds).

## Troubleshooting

**Installer error: "UE4SS not installed"** — extract the latest UE4SS release from <https://github.com/UE4SS-RE/RE-UE4SS/releases/latest> into `R5\Binaries\Win64\`, then re-run the installer.

**`/healthcheck` returns "Unable to connect"** — see "Verify the install" above; usually the DLL didn't load. Check `R5\Saved\Logs\R5.log` for `AdmiralsPanel` lines.

**`/healthcheck` works but `/api/status` returns 401** — clear cookies and re-login. The session cookie expired or the server password rotated.

**Login page accepts password but redirects to login again** — the password in `admiralspanel.json` was changed and your browser cached the old one. Hard-refresh.

**Multipliers persist to JSON but nothing happens in-game** — check the slider's badge: `live` means the native impl ran, `unwired` means we don't have a hook for that key yet. For `live` keys, give the world ~3 seconds after server start so the startup re-apply has time to run.

**"Command timed out (25s)"** — usually transient (server is mid-level-transition or under heavy load). Re-try. The "Vanilla" preset in particular takes ~17s because it walks every UObject 8 times.

**Loot multiplier doesn't affect tree-chop drops** — the auto-pickup ability collects them faster than the 2-second multiplier tick. Use `harvest_yield` instead; it modifies the loot tables before they roll.

**Harvest yield set to 9 only gives ~3-4x** — the Windrose runtime applies its own post-roll scaling. The slider is capped at 4× in the UI to reflect the practical ceiling. The experimental `coop_scale` knob targets `Coop_StatsCorrectionModifier` but did not lift the cap in our testing.

## Uninstall

Stop the server, then:

```powershell
$g = "C:\Program Files (x86)\Steam\steamapps\common\Windrose Dedicated Server"
Remove-Item "$g\R5\Binaries\Win64\ue4ss\Mods\AdmiralsPanel" -Recurse -Force
Remove-Item "$g\R5\Binaries\Win64\ue4ss\Mods\AdmiralsPanelNative" -Recurse -Force
# optional: keep your password + multiplier settings
Remove-Item "$g\admiralspanel.json"
Remove-Item "$g\admiralspanel_data" -Recurse -Force
# remove from UE4SS mod list
(Get-Content "$g\R5\Binaries\Win64\ue4ss\Mods\mods.txt") -notmatch '^AdmiralsPanelNative' | Set-Content "$g\R5\Binaries\Win64\ue4ss\Mods\mods.txt"
```
