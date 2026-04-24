# AdmiralsPanel installer
# Usage:
#   powershell -ExecutionPolicy Bypass -File install.ps1            (standalone, default)
#   powershell -ExecutionPolicy Bypass -File install.ps1 -WithWindrosePlus
#   powershell -ExecutionPolicy Bypass -File install.ps1 -GameDir "C:\path"
#
# Default (standalone, v0.6+): installs under UE4SS Mods\AdmiralsPanel and
# Mods\AdmiralsPanelNative. Native DLL runs its own HTTP server on port 8790
# and handles login / web-panel serving / RCON dispatch. No WindrosePlus
# required.
#
# -WithWindrosePlus: legacy sub-mod install for users who still want to run
# alongside WindrosePlus. Drops into WindrosePlus\Mods\admiral-admin\ and
# uses WP's dashboard on port 8780.

param(
    [string]$GameDir = "",
    [switch]$WithWindrosePlus
)

$ErrorActionPreference = "Stop"

function Write-Step($num, $msg) { Write-Host "  [$num] $msg" }
function Write-OK($msg)         { Write-Host "      $msg" -ForegroundColor Green }
function Write-Info($msg)       { Write-Host "      $msg" -ForegroundColor DarkGray }
function Write-Warn($msg)       { Write-Host "      WARNING: $msg" -ForegroundColor Yellow }
function Write-Fail($msg)       { Write-Host "  ERROR: $msg" -ForegroundColor Red }

Write-Host ""
Write-Host "  AdmiralsPanel installer" -ForegroundColor Cyan
Write-Host "  Mode: $(if ($WithWindrosePlus) { 'Sub-mod (WindrosePlus)' } else { 'Standalone' })" -ForegroundColor Cyan
Write-Host ""

# --- Locate game dir -------------------------------------------------------

if ($GameDir -eq "") {
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

$repoRoot = $PSScriptRoot
if ([string]::IsNullOrEmpty($repoRoot)) { $repoRoot = $PWD.Path }

$ue4ssMods = Join-Path $GameDir "R5\Binaries\Win64\ue4ss\Mods"
if (-not (Test-Path -LiteralPath $ue4ssMods)) {
    Write-Fail "UE4SS not installed at $ue4ssMods — install UE4SS first."
    exit 1
}

# ===========================================================================
# STANDALONE MODE (default, v0.6+)
# ===========================================================================
if (-not $WithWindrosePlus) {

    Write-Step "1/4" "Installing standalone Lua mod..."

    $modSrc = Join-Path $repoRoot "mod"
    if (-not (Test-Path -LiteralPath $modSrc)) {
        Write-Fail "mod\ folder not found in $repoRoot"
        exit 1
    }

    $apDir     = Join-Path $ue4ssMods "AdmiralsPanel"
    $apScripts = Join-Path $apDir    "Scripts"
    New-Item -ItemType Directory -Path $apScripts -Force | Out-Null

    # Files needed in standalone mode
    Copy-Item (Join-Path $modSrc "Scripts\main.lua")     $apScripts -Force
    Copy-Item (Join-Path $modSrc "Scripts\ap_api.lua")   $apScripts -Force
    Copy-Item (Join-Path $modSrc "Scripts\ap_spool.lua") $apScripts -Force
    Copy-Item (Join-Path $modSrc "Scripts\ap_json.lua")  $apScripts -Force
    Copy-Item (Join-Path $modSrc "init.lua")             $apScripts -Force
    if (Test-Path -LiteralPath (Join-Path $modSrc "lib"))  { Copy-Item (Join-Path $modSrc "lib")  $apScripts -Recurse -Force }
    if (Test-Path -LiteralPath (Join-Path $modSrc "data")) { Copy-Item (Join-Path $modSrc "data") $apScripts -Recurse -Force }
    Set-Content -Path (Join-Path $apDir "enabled.txt") -Value "1" -Encoding ASCII -NoNewline
    Write-OK "Deployed to $apDir"

    # If an old WP sub-mod install exists, warn the user.
    $oldSub = Join-Path $ue4ssMods "WindrosePlus\Mods\admiral-admin"
    if (Test-Path -LiteralPath $oldSub) {
        Write-Warn "Old WindrosePlus sub-mod install detected at $oldSub"
        Write-Info "Both modes run in parallel — remove the sub-mod if you no longer use WindrosePlus."
    }

    Write-Step "2/4" "Installing web panel..."

    $webSrc = Join-Path $repoRoot "web"
    $webDst = Join-Path $GameDir "admiralspanel_data\web"
    New-Item -ItemType Directory -Path $webDst -Force | Out-Null
    if (Test-Path -LiteralPath $webSrc) {
        Copy-Item (Join-Path $webSrc "*") $webDst -Recurse -Force
        Write-OK "Web UI deployed to $webDst"
    } else {
        Write-Warn "web\ folder not found - skipping UI install"
    }

    Write-Step "3/4" "Installing native DLL..."

    $nativeDll = Join-Path $repoRoot "cpp\dist\main.dll"
    $nativeDir = Join-Path $ue4ssMods "AdmiralsPanelNative"
    $nativeDlls = Join-Path $nativeDir "dlls"
    New-Item -ItemType Directory -Path $nativeDlls -Force | Out-Null
    if (Test-Path -LiteralPath $nativeDll) {
        $dstDll = Join-Path $nativeDlls "main.dll"
        try {
            Copy-Item $nativeDll $dstDll -Force
            Write-OK "Deployed main.dll"
        } catch [System.IO.IOException] {
            $srcHash = (Get-FileHash $nativeDll -Algorithm SHA256).Hash
            $same = (Test-Path -LiteralPath $dstDll) -and `
                    ((Get-FileHash $dstDll -Algorithm SHA256).Hash -eq $srcHash)
            if ($same) { Write-Info "main.dll already up to date (server running)" }
            else       { Write-Warn "main.dll locked by running server and differs - stop the server and re-run" }
        }
    } else {
        Write-Warn "cpp\dist\main.dll missing - HTTP server + web panel will not start. Build via cpp\build.ps1."
    }
    Set-Content -Path (Join-Path $nativeDir "enabled.txt") -Value "1" -Encoding ASCII -NoNewline
    $modsTxt = Join-Path $ue4ssMods "mods.txt"
    $alreadyN = $false
    if (Test-Path -LiteralPath $modsTxt) {
        if ((Get-Content $modsTxt -Raw) -match "AdmiralsPanelNative") { $alreadyN = $true }
    }
    if (-not $alreadyN) { Add-Content -Path $modsTxt -Value "AdmiralsPanelNative : 1" }

    Write-Step "4/4" "Done."
    Write-Host ""
    Write-Host "  Next steps:" -ForegroundColor Cyan
    Write-Host "    1. Start (or restart) the server: $GameDir\StartWindrosePlusServer.bat"
    Write-Host "       (the script starts WindrosePlus too, but our HTTP server is independent)"
    Write-Host "    2. First run generates admiralspanel.json with a random password at the server root."
    Write-Host "    3. Open http://localhost:8790/ in a browser on the server"
    Write-Host "       (or from another machine once the firewall allows port 8790)."
    Write-Host "    4. Log in with the password from admiralspanel.json -> rcon.password."
    Write-Host ""
    exit 0
}

# ===========================================================================
# SUB-MOD MODE (legacy: under WindrosePlus)
# ===========================================================================

$wpModDir  = Join-Path $GameDir "R5\Binaries\Win64\ue4ss\Mods\WindrosePlus"
$wpSubMods = Join-Path $wpModDir "Mods"
$wpWebDir  = Join-Path $GameDir "windrose_plus\server\web"

Write-Step "1/5" "Checking prerequisites..."
if (-not (Test-Path -LiteralPath $wpModDir)) {
    Write-Fail "WindrosePlus is not installed at $wpModDir"
    Write-Host "  Install WindrosePlus first or drop -WithWindrosePlus for standalone mode." -ForegroundColor Yellow
    exit 1
}
if (-not (Test-Path -LiteralPath $wpWebDir)) {
    Write-Warn "windrose_plus\server\web missing (dashboard never started?) - creating it now."
    New-Item -ItemType Directory -Path $wpWebDir -Force | Out-Null
}
Write-OK "WindrosePlus detected."

Write-Step "2/5" "Installing Lua mod..."
$modSrc = Join-Path $repoRoot "mod"
$modDst = Join-Path $wpSubMods "admiral-admin"
if (Test-Path -LiteralPath $modDst) { Remove-Item $modDst -Recurse -Force }
Copy-Item $modSrc $modDst -Recurse -Force
Write-OK "Copied to $modDst"

$registryPath = Join-Path $wpSubMods "mods_registry.json"
$registry = @()
if (Test-Path -LiteralPath $registryPath) {
    try {
        $existing = Get-Content $registryPath -Raw | ConvertFrom-Json
        if ($existing) { $registry = @($existing) }
    } catch {
        Write-Warn "Existing mods_registry.json failed to parse - rebuilding."
    }
}
if (-not ($registry -contains "admiral-admin")) { $registry += "admiral-admin" }
$jsonOut = ConvertTo-Json $registry -Compress
$tmpReg = $registryPath + ".tmp"
Set-Content -Path $tmpReg -Value $jsonOut -Encoding ASCII -NoNewline
if (Test-Path -LiteralPath $registryPath) { Remove-Item $registryPath -Force }
Move-Item $tmpReg $registryPath
Write-OK "mods_registry.json updated"

Write-Step "3/5" "Installing web panel..."
$webSrc = Join-Path $repoRoot "web"
if (-not (Test-Path -LiteralPath $webSrc)) {
    Write-Warn "web\ folder not found - skipping UI install"
} else {
    $webDst = Join-Path $wpWebDir "admiral"
    if (Test-Path -LiteralPath $webDst) { Remove-Item $webDst -Recurse -Force }
    Copy-Item $webSrc $webDst -Recurse -Force
    Write-OK "Copied to $webDst"
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

Write-Step "4/5" "Installing native companion (optional C++ DLL)..."
$nativeDll = Join-Path $repoRoot "cpp\dist\main.dll"
if (-not (Test-Path -LiteralPath $nativeDll)) {
    Write-Info "No cpp\dist\main.dll found - skipping native install."
} else {
    $nativeDir = Join-Path $ue4ssMods "AdmiralsPanelNative"
    $nativeDlls = Join-Path $nativeDir "dlls"
    New-Item -ItemType Directory -Path $nativeDlls -Force | Out-Null
    try {
        Copy-Item $nativeDll (Join-Path $nativeDlls "main.dll") -Force
        Write-OK "Deployed main.dll"
    } catch [System.IO.IOException] { Write-Warn "main.dll locked by running server" }
    Set-Content -Path (Join-Path $nativeDir "enabled.txt") -Value "1" -Encoding ASCII -NoNewline
    $modsTxt = Join-Path $ue4ssMods "mods.txt"
    $already = $false
    if (Test-Path -LiteralPath $modsTxt) {
        if ((Get-Content $modsTxt -Raw) -match "AdmiralsPanelNative") { $already = $true }
    }
    if (-not $already) { Add-Content -Path $modsTxt -Value "AdmiralsPanelNative : 1" }
}

Write-Step "5/5" "Done."
Write-Host ""
Write-Host "  Next steps:" -ForegroundColor Cyan
Write-Host "    1. Restart the server for the mod to load."
Write-Host "    2. Start the WindrosePlus dashboard: $GameDir\windrose_plus\start_dashboard.bat"
Write-Host "    3. Log into http://localhost:8780/ with your RCON password."
Write-Host "    4. Open http://localhost:8780/admiral.html"
Write-Host ""
