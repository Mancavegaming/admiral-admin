# Build AdmiralsPanelNative.dll
# Requires:
#   - Visual Studio 2022 Build Tools with C++ workload + Windows SDK
#   - An Epic Games -> GitHub account link (for the UEPseudo submodule)
#   - Git on PATH
#
# Produces: build\Game__Shipping__Win64\bin\AdmiralsPanelNative.dll
# Then copies it to ..\cpp\dist\main.dll for install.ps1 to pick up.

param(
    [string]$Ue4ssDir = "",           # path to an existing RE-UE4SS clone; empty = use sibling
    [switch]$SkipClone
)

$ErrorActionPreference = "Stop"

function Fail($m) { Write-Host "  ERROR: $m" -ForegroundColor Red; exit 1 }
function Info($m) { Write-Host "  $m" -ForegroundColor DarkGray }

$repoRoot  = Split-Path -Parent $PSScriptRoot
$cppRoot   = $PSScriptRoot
$distDir   = Join-Path $cppRoot "dist"

Write-Host "" -NoNewline
Write-Host ""
Write-Host "  AdmiralsPanelNative build" -ForegroundColor Cyan
Write-Host ""

# --- 1. Locate or clone RE-UE4SS ------------------------------------------

if ($Ue4ssDir -eq "") { $Ue4ssDir = Join-Path (Split-Path -Parent $repoRoot) "RE-UE4SS" }

if (-not (Test-Path -LiteralPath $Ue4ssDir)) {
    if ($SkipClone) { Fail "RE-UE4SS not found at $Ue4ssDir and -SkipClone was given." }
    Info "Cloning RE-UE4SS into $Ue4ssDir"
    git clone --depth 1 https://github.com/UE4SS-RE/RE-UE4SS.git $Ue4ssDir
    if ($LASTEXITCODE -ne 0) { Fail "git clone failed" }
}

# Pull submodules via HTTPS so SSH isn't required (uses `gh` or git credential helper).
Info "Ensuring submodules (UEPseudo, patternsleuth) are fetched via HTTPS"
Push-Location $Ue4ssDir
try {
    git -c url."https://github.com/".insteadOf="git@github.com:" `
        submodule update --init --recursive
    if ($LASTEXITCODE -ne 0) { Fail "submodule update failed. Make sure your GitHub account is linked to an Epic Games account (needed for UEPseudo access)." }
} finally { Pop-Location }

# --- 2. Stage our mod folder alongside RE-UE4SS's bundled cppmods ---------

$modStage = Join-Path $Ue4ssDir "cppmods\AdmiralsPanelNative"
if (Test-Path -LiteralPath $modStage) { Remove-Item $modStage -Recurse -Force }
New-Item -ItemType Directory -Path $modStage -Force | Out-Null
Copy-Item "$cppRoot\CMakeLists.txt" $modStage
New-Item -ItemType Directory -Path (Join-Path $modStage "src") -Force | Out-Null
Copy-Item "$cppRoot\src\*" (Join-Path $modStage "src") -Recurse -Force

# Ensure cppmods/CMakeLists.txt references our mod
$cppmodsList = Join-Path $Ue4ssDir "cppmods\CMakeLists.txt"
$listContent = Get-Content $cppmodsList -Raw
if ($listContent -notmatch 'AdmiralsPanelNative') {
    Add-Content $cppmodsList "`nadd_subdirectory(`"AdmiralsPanelNative`")`n"
    Info "Registered AdmiralsPanelNative in cppmods\CMakeLists.txt"
}

# --- 3. Configure + build via bundled MSVC/CMake/Ninja --------------------

$vsshell = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\Launch-VsDevShell.ps1"
if (-not (Test-Path -LiteralPath $vsshell)) {
    $vsshell = "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\Launch-VsDevShell.ps1"
}
if (-not (Test-Path -LiteralPath $vsshell)) { Fail "Visual Studio 2022 Build Tools not found. Install the C++ build tools workload." }

& $vsshell -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null

$cmake = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if (-not (Test-Path -LiteralPath $cmake)) {
    $c = Get-Command cmake.exe -ErrorAction SilentlyContinue
    if ($c) { $cmake = $c.Source } else { $cmake = $null }
}
if (-not $cmake) { Fail "cmake not found" }

$buildDir = Join-Path $Ue4ssDir "build"
if (-not (Test-Path -LiteralPath $buildDir)) {
    Info "Configuring (first-time, this takes ~1-2 minutes)"
    & $cmake -B $buildDir -S $Ue4ssDir -G Ninja "-DCMAKE_BUILD_TYPE=Game__Shipping__Win64"
    if ($LASTEXITCODE -ne 0) { Fail "cmake configure failed" }
}

Info "Building AdmiralsPanelNative"
& $cmake --build $buildDir --target AdmiralsPanelNative
if ($LASTEXITCODE -ne 0) { Fail "build failed" }

# --- 4. Publish DLL to cpp\dist\ ------------------------------------------

if (-not (Test-Path -LiteralPath $distDir)) { New-Item -ItemType Directory -Path $distDir -Force | Out-Null }
$srcDll = Join-Path $buildDir "Game__Shipping__Win64\bin\AdmiralsPanelNative.dll"
if (-not (Test-Path -LiteralPath $srcDll)) { Fail "expected DLL not produced: $srcDll" }
Copy-Item $srcDll (Join-Path $distDir "main.dll") -Force

Write-Host ""
Write-Host "  DLL built: $distDir\main.dll" -ForegroundColor Green
Write-Host "  Run install.ps1 from the repo root to deploy it alongside the Lua mod." -ForegroundColor Green
Write-Host ""
