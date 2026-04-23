# AdmiralsPanelNative (C++)

The optional native companion DLL. Unlocks heal / kill / feed / revive / direct-attribute-write features by writing straight to Windrose's GAS (Gameplay Ability System) `UR5AttributeSet` structure. Without this DLL, AdmiralsPanel still works — you just lose the `ap.*n` native commands.

## Why C++?

Windrose stores Health, Stamina, Damage, Armor, and about 100 other attributes as `FGameplayAttributeData` structs on `PlayerState.R5AttributeSet`. The struct has a virtual destructor (so a vtable pointer at offset 0, floats at offsets 8 and 12). UE4SS's Lua can see the property but serves up a proxy UObject — the underlying float layout isn't reachable from Lua reliably. A C++ mod can reinterpret the memory directly and write the CurrentValue float.

See `docs/native-mod.md` in the repo root for the full technical writeup.

## Building

### Prerequisites

1. Windows 10/11 or Windows Server
2. Visual Studio 2022 Build Tools with the **C++ build tools** workload (includes MSVC, CMake, Ninja, Windows SDK). Free.
3. Git on PATH
4. **Epic Games ↔ GitHub account link.** UE4SS's source pulls in `Re-UE4SS/UEPseudo` which is private and gated behind Epic's GitHub org.
   - Create/sign in at https://www.epicgames.com/account/personal → Apps and Accounts → connect GitHub
   - Accept the email invitation from @EpicGames
   - Verify at https://github.com/EpicGames/UnrealEngine (should load, not 404)

### One-command build

From this `cpp/` folder:

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1
```

That will:
- Clone `UE4SS-RE/RE-UE4SS` as a sibling of the repo (unless `-Ue4ssDir` is passed)
- Init submodules (uses HTTPS — no SSH setup needed, relies on existing `gh` / git credential helper auth)
- Stage our mod folder into `RE-UE4SS/cppmods/AdmiralsPanelNative/`
- Configure via CMake (Ninja, `Game__Shipping__Win64`)
- Build only the `AdmiralsPanelNative` target (~15s after first-time UE4SS build, ~15min first time)
- Publish `cpp/dist/main.dll` ready for `install.ps1` to pick up

### Subsequent rebuilds

Fast — CMake + Ninja recompile only changed files. Typically < 30 seconds.

```powershell
.\build.ps1
```

### Manual build (if the script breaks)

```powershell
# From a VS Developer PowerShell prompt:
cd ..\RE-UE4SS
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Game__Shipping__Win64
cmake --build build --target AdmiralsPanelNative
# DLL is at: build\Game__Shipping__Win64\bin\AdmiralsPanelNative.dll
```

## Deployment

Don't copy the DLL by hand. After a successful build, run `install.ps1` from the repo root — it detects `cpp/dist/main.dll` and deploys to the correct game path:
`<gamedir>\R5\Binaries\Win64\ue4ss\Mods\AdmiralsPanelNative\dlls\main.dll`
plus an `enabled.txt` marker, plus a line in `ue4ss\Mods\mods.txt`.

## Developing

- `src/dllmain.cpp` is the entire mod. Single translation unit, ~350 LOC.
- Rebuild + deploy + restart game server = one `build.ps1` + one `install.ps1` + one server restart. ~90 seconds total.
- Look at `docs/native-mod.md` for the GAS reverse-engineering details and the Lua binding contract.

## Skipping the build

If you don't want to build: the binary is attached to each GitHub release. Download `AdmiralsPanelNative-<version>.dll` from the release page, drop it in `cpp/dist/main.dll`, run `install.ps1`. Same outcome.
