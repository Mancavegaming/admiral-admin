# AdmiralsPanel installer
# Run from inside the repo root after cloning:
#   powershell -ExecutionPolicy Bypass -File install.ps1
# Optionally pass -GameDir to skip auto-detection:
#   powershell -ExecutionPolicy Bypass -File install.ps1 -GameDir "C:\path\to\Windrose Dedicated Server"

param(
    [string]$GameDir = ""
)

$ErrorActionPreference = "Stop"

function Write-Step($num, $msg) { Write-Host "  [$num] $msg" }
function Write-OK($msg)         { Write-Host "      $msg" -ForegroundColor Green }
function Write-Info($msg)       { Write-Host "      $msg" -ForegroundColor DarkGray }
function Write-Warn($msg)       { Write-Host "      WARNING: $msg" -ForegroundColor Yellow }
function Write-Fail($msg)       { Write-Host "  ERROR: $msg" -ForegroundColor Red }

Write-Host ""
Write-Host "  AdmiralsPanel installer" -ForegroundColor Cyan
Write-Host ""

# --- Locate game dir -------------------------------------------------------

if ($GameDir -eq "") {
    # Try common Steam default paths
    $candidates = @(
        "C:\Program Files (x86)\Steam\steamapps\common\Windrose Dedicated Server",
        "C:\Program Files\Steam\steamapps\common\Windrose Dedicated Server",
        "D:\SteamLibrary\steamapps\common\Windrose Dedicated Server",
        "E:\SteamLibrary\steamapps\common\Windrose Dedicated Server"
    )
    foreach ($c in $candidates) {
        if (Test-Path -LiteralPath (Join-Path $c "WindroseServer.exe")) {
            $GameDir = $c
            break
        }
    }
}

if ($GameDir -eq "" -or -not (Test-Path -LiteralPath (Join-Path $GameDir "WindroseServer.exe"))) {
    Write-Fail "Could not locate the Windrose Dedicated Server folder."
    Write-Host "  Pass it explicitly: -GameDir `"C:\path\to\Windrose Dedicated Server`"" -ForegroundColor Yellow
    exit 1
}
$GameDir = (Resolve-Path $GameDir).Path
Write-Host "  Server folder: $GameDir" -ForegroundColor Green
Write-Host ""

# --- Validate WindrosePlus is installed ------------------------------------

$wpModDir  = Join-Path $GameDir "R5\Binaries\Win64\ue4ss\Mods\WindrosePlus"
$wpSubMods = Join-Path $wpModDir "Mods"
$wpWebDir  = Join-Path $GameDir "windrose_plus\server\web"

Write-Step "1/5" "Checking prerequisites..."
if (-not (Test-Path -LiteralPath $wpModDir)) {
    Write-Fail "WindrosePlus is not installed at $wpModDir"
    Write-Host "  Install WindrosePlus first: https://github.com/HumanGenome/WindrosePlus" -ForegroundColor Yellow
    exit 1
}
if (-not (Test-Path -LiteralPath $wpWebDir)) {
    Write-Warn "windrose_plus\server\web does not exist yet (has the dashboard ever started?)."
    Write-Info "Creating it now; re-run WindrosePlus install if the dashboard fails to find assets."
    New-Item -ItemType Directory -Path $wpWebDir -Force | Out-Null
}
Write-OK "WindrosePlus detected."

# --- Copy mod --------------------------------------------------------------

Write-Step "2/5" "Installing Lua mod..."

$repoRoot = $PSScriptRoot
if ([string]::IsNullOrEmpty($repoRoot)) { $repoRoot = $PWD.Path }

$modSrc = Join-Path $repoRoot "mod"
if (-not (Test-Path -LiteralPath $modSrc)) {
    Write-Fail "mod\ folder not found in $repoRoot - are you running from the repo root?"
    exit 1
}

$modDst = Join-Path $wpSubMods "admiral-admin"
if (Test-Path -LiteralPath $modDst) { Remove-Item $modDst -Recurse -Force }
Copy-Item $modSrc $modDst -Recurse -Force
Write-OK "Copied to $modDst"

# --- Update mods_registry.json --------------------------------------------

$registryPath = Join-Path $wpSubMods "mods_registry.json"
$registry = @()
if (Test-Path -LiteralPath $registryPath) {
    try {
        $existing = Get-Content $registryPath -Raw | ConvertFrom-Json
        if ($existing -is [array]) {
            $registry = @($existing)
        } elseif ($existing) {
            # ConvertFrom-Json turns a single-element array into a scalar
            $registry = @($existing)
        }
    } catch {
        Write-Warn "Existing mods_registry.json failed to parse - rebuilding."
    }
}
if (-not ($registry -contains "admiral-admin")) {
    $registry += "admiral-admin"
}
# Normalise + write back
$jsonOut = ConvertTo-Json $registry -Compress
$tmpReg = $registryPath + ".tmp"
Set-Content -Path $tmpReg -Value $jsonOut -Encoding ASCII -NoNewline
if (Test-Path -LiteralPath $registryPath) { Remove-Item $registryPath -Force }
Move-Item $tmpReg $registryPath
Write-OK "mods_registry.json updated: $jsonOut"

# --- Copy web panel -------------------------------------------------------

Write-Step "3/5" "Installing web panel..."

$webSrc = Join-Path $repoRoot "web"
if (-not (Test-Path -LiteralPath $webSrc)) {
    Write-Warn "web\ folder not found - skipping UI install (backend-only install)."
} else {
    $webDst = Join-Path $wpWebDir "admiral"
    if (Test-Path -LiteralPath $webDst) { Remove-Item $webDst -Recurse -Force }
    Copy-Item $webSrc $webDst -Recurse -Force
    Write-OK "Copied to $webDst"

    # Drop a redirect helper at server/web/admiral.html so users can bookmark
    # http://localhost:8780/admiral.html (the dashboard does not auto-serve
    # index.html for directory requests without an explicit route).
    $redirectPath = Join-Path $wpWebDir "admiral.html"
    $redirectBody = @"
<!DOCTYPE html>
<html><head>
<meta http-equiv="refresh" content="0;url=/admiral/index.html">
<title>AdmiralsPanel</title>
</head><body>
Redirecting to <a href="/admiral/index.html">AdmiralsPanel</a>...
</body></html>
"@
    [System.IO.File]::WriteAllText($redirectPath, $redirectBody, [System.Text.Encoding]::ASCII)
    Write-OK "Redirect helper placed at /admiral.html"
}

# --- Install native companion DLL if present -------------------------------

Write-Step "4/5" "Installing native companion (optional C++ DLL)..."

$nativeDll = Join-Path $repoRoot "cpp\dist\main.dll"
if (-not (Test-Path -LiteralPath $nativeDll)) {
    Write-Info "No cpp\dist\main.dll found - skipping native install."
    Write-Info "Pure-Lua features (teleport, multipliers, presets) still work."
    Write-Info "To enable heal/kill/feed/revive: build via cpp\build.ps1 or download the DLL from the release page."
} else {
    $ue4ssMods  = Join-Path $GameDir "R5\Binaries\Win64\ue4ss\Mods"
    $nativeDir  = Join-Path $ue4ssMods "AdmiralsPanelNative"
    $nativeDlls = Join-Path $nativeDir "dlls"
    if (-not (Test-Path -LiteralPath $nativeDlls)) {
        New-Item -ItemType Directory -Path $nativeDlls -Force | Out-Null
    }
    Copy-Item $nativeDll (Join-Path $nativeDlls "main.dll") -Force
    Set-Content -Path (Join-Path $nativeDir "enabled.txt") -Value "1" -Encoding ASCII -NoNewline
    Write-OK "Deployed main.dll to $nativeDlls"

    # Register in ue4ss\Mods\mods.txt if not already there
    $modsTxt = Join-Path $ue4ssMods "mods.txt"
    $alreadyRegistered = $false
    if (Test-Path -LiteralPath $modsTxt) {
        $content = Get-Content $modsTxt -Raw
        if ($content -match "AdmiralsPanelNative") { $alreadyRegistered = $true }
    }
    if (-not $alreadyRegistered) {
        Add-Content -Path $modsTxt -Value "AdmiralsPanelNative : 1"
        Write-OK "Registered AdmiralsPanelNative in mods.txt"
    } else {
        Write-Info "AdmiralsPanelNative already registered in mods.txt"
    }
}

# --- Next steps -----------------------------------------------------------

Write-Step "5/5" "Done."
Write-Host ""
Write-Host "  Next steps:" -ForegroundColor Cyan
Write-Host "    1. If the server is running, wait ~30s for the hot-reload file-watcher"
Write-Host "       to pick up the mod (you will see 'Loaded: AdmiralsPanel' in UE4SS.log)."
Write-Host "       For certainty, restart the server."
Write-Host "    2. If not already running, start the WindrosePlus dashboard:"
Write-Host "         $GameDir\windrose_plus\start_dashboard.bat"
Write-Host "    3. Log into the dashboard at http://localhost:8780/ with your RCON password."
Write-Host "    4. Navigate to: http://localhost:8780/admiral.html"
Write-Host "       (direct URL: http://localhost:8780/admiral/index.html)"
Write-Host ""
Write-Host "  After any future WindrosePlus update, re-run this installer to" -ForegroundColor DarkGray
Write-Host "  restore the web panel (WindrosePlus wipes server/ on update)." -ForegroundColor DarkGray
Write-Host ""
