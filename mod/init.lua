-- AdmiralsPanel — server-side Lua backend.
-- Adds ap.* RCON commands used by the /admiral/ web panel.
--
-- Loads in two modes:
--   1. Sub-mod of WindrosePlus: WindrosePlus.API is set by WP before we run.
--      Entry: Mods/admiral-admin/mod.json (main=init.lua).
--   2. Standalone UE4SS mod (v0.6+): Scripts/main.lua sets AdmiralsPanel.API
--      then dofile()s this init.lua. No WindrosePlus dependency.
-- Either way, the `API` local below is the same interface.

local API
if AdmiralsPanel and AdmiralsPanel.API then
    API = AdmiralsPanel.API   -- standalone mode (v0.6+)
elseif WindrosePlus and WindrosePlus.API then
    API = WindrosePlus.API    -- sub-mod mode (legacy)
else
    error("AdmiralsPanel: no API available — neither WindrosePlus.API nor AdmiralsPanel.API set")
end

-- JSON + Config modules. Under WindrosePlus these live at
-- WindrosePlus/Scripts/modules. Under standalone we ship a local json
-- (ap_json) and skip Config (multiplier persistence handled natively).
local json, Config
do
    local ok_j, j = pcall(require, "modules.json")
    if ok_j then json = j
    else
        local ok2, j2 = pcall(require, "ap_json")
        json = ok2 and j2 or nil
    end
    local ok_c, c = pcall(require, "modules.config")
    Config = ok_c and c or nil
end

local MOD_NAME = "AdmiralsPanel"
local VERSION  = "0.8.0"

-- ---------------------------------------------------------------------------
-- Paths
-- ---------------------------------------------------------------------------

-- Cache the resolved game dir — io.popen("cd") spawns a visible cmd window
-- on Windows, so we must NOT call it on every hot path.
local _cached_game_dir = nil
local _cached_game_dir_resolved = false

local function gameDir()
    if _cached_game_dir_resolved then return _cached_game_dir end
    _cached_game_dir_resolved = true

    if Config then
        local ok, p = pcall(function() return Config._path end)
        if ok and p then
            _cached_game_dir = p:match("^(.*)[\\/][^\\/]+$")
            return _cached_game_dir
        end
    end
    -- Standalone: UE4SS cwd is <game>\R5\Binaries\Win64. Walk up 3 levels
    -- to reach the game root. io.popen("cd") is called exactly ONCE
    -- (startup) thanks to the module-level cache above.
    local ok, c = pcall(function()
        local f = io.popen("cd")
        if not f then return nil end
        local s = f:read("*l")
        f:close()
        return s
    end)
    if not (ok and c and c ~= "") then return nil end
    for _ = 1, 3 do
        local parent = c:match("^(.+)[\\/][^\\/]+$")
        if parent and parent ~= "" then c = parent end
    end
    _cached_game_dir = c
    return c
end

local function modDir()
    local src = debug.getinfo(1, "S").source
    local path = src:sub(1, 1) == "@" and src:sub(2) or src
    return path:match("^(.*)[\\/][^\\/]+$") or ""
end

local function dataDir()
    local gd = gameDir()
    if not gd then return nil end
    if Config then
        return gd .. "\\windrose_plus_data"   -- sub-mod: share WP's data dir
    end
    return gd .. "\\admiralspanel_data"       -- standalone: our own
end

local ADMIN_LOG_NAME = "admiral_admin_log.json"

-- ---------------------------------------------------------------------------
-- Small helpers
-- ---------------------------------------------------------------------------

local function log(level, msg) API.log(level, "AdmiralsPanel", msg) end

local function readJson(path)
    local f = io.open(path, "r")
    if not f then return nil end
    local content = f:read("*a")
    f:close()
    local ok, data = pcall(json.decode, content)
    if not ok then return nil end
    return data
end

local function writeJson(path, data)
    local ok, encoded = pcall(json.encode, data)
    if not ok then return false, "encode failed" end
    local tmp = path .. ".tmp"
    local f = io.open(tmp, "w")
    if not f then return false, "cannot open tmp file: " .. tmp end
    f:write(encoded)
    f:close()
    os.remove(path)
    local okRename = os.rename(tmp, path)
    if not okRename then return false, "rename failed" end
    return true
end

local function trim(s) return (s:gsub("^%s+", ""):gsub("%s+$", "")) end

-- ---------------------------------------------------------------------------
-- Player lookup
-- ---------------------------------------------------------------------------

local function pcDisplayName(pc)
    local out
    pcall(function()
        local ps = pc.PlayerState
        if ps and ps:IsValid() then
            local val = ps.PlayerNamePrivate
            if val then
                local ok, str = pcall(function() return val:ToString() end)
                if ok and str then out = str end
            end
        end
    end)
    return out
end

local function listPlayers()
    local out = {}
    local pcs = FindAllOf("PlayerController")
    if not pcs then return out end
    for _, pc in ipairs(pcs) do
        if pc:IsValid() then
            local name = pcDisplayName(pc)
            if name then
                local pawn
                pcall(function() pawn = pc.Pawn end)
                if pawn and pawn:IsValid() then
                    table.insert(out, { name = name, pc = pc, pawn = pawn })
                end
            end
        end
    end
    return out
end

local function findPlayerByName(name)
    if not name or name == "" then return nil end
    local target = name:lower()
    local players = listPlayers()
    for _, p in ipairs(players) do
        if p.name:lower() == target then return p end
    end
    for _, p in ipairs(players) do
        if p.name:lower():sub(1, #target) == target then return p end
    end
    return nil
end

-- ---------------------------------------------------------------------------
-- Multipliers (pure Lua)
-- ---------------------------------------------------------------------------

-- Each multiplier ships with an `impl` field tracking what's wired:
--   "unwired"  - persisted to JSON, no live game effect.
--   "native"   - applied via AdmiralsPanelNative.dll's applyworldmult dispatch.
-- The previous code wrote to gm[XPMultiplier] etc. on R5GameMode which has no
-- such property. UE4SS's pcall accepts the write silently, so the result
-- always SAID "(live: XPMultiplier)" but nothing actually happened. We're
-- replacing that lie with honest reporting and per-key native impls.
local MULTIPLIERS = {
    loot             = { desc = "Loot drop multiplier",         impl = "native"  },
    xp               = { desc = "XP / experience multiplier",   impl = "native"  },
    weight           = { desc = "Carry weight multiplier",      impl = "native"  },
    craft_cost       = { desc = "Crafting cost multiplier",     impl = "native"  },
    stack_size       = { desc = "Inventory stack size mult.",   impl = "native"  },
    crop_speed       = { desc = "Crop growth speed multiplier", impl = "native"  },
    points_per_level = { desc = "Skill points per level mult.", impl = "native"  },
    harvest_yield    = { desc = "Harvest yield multiplier",     impl = "native"  },
    coop_scale       = { desc = "Coop drop-scale override",     impl = "native"  },
}

local function validMultiplierValue(v)
    return type(v) == "number" and v >= 0 and v < 1000
end

-- Returns a status string describing whether the value took effect live or is
-- just persisted. Native dispatch lands per-multiplier as RE work catches up.
-- We look up the native function directly (not via nativeAvailable, which is
-- declared later in the file) to avoid a forward-reference issue.
local function applyMultiplierToGame(key, value)
    local spec = MULTIPLIERS[key]
    if not spec then return "saved" end
    if spec.impl == "native" then
        local fn = _G["admiralspanel_native_applyworldmult"]
        if type(fn) ~= "function" then
            return "saved; native DLL missing (install AdmiralsPanelNative.dll)"
        end
        local ok, result = pcall(fn, key, value)
        if not ok then return "saved; native call failed: " .. tostring(result) end
        return tostring(result)
    end
    return "saved; not yet wired (no in-game effect)"
end

local function persistMultiplier(key, value)
    local path
    if Config and Config._path then
        path = Config._path
    else
        local gd = gameDir()
        if not gd then return false, "game dir unknown" end
        path = gd .. "\\admiralspanel.json"
    end
    local cfg = readJson(path)
    if not cfg then return false, "Cannot read " .. path end
    cfg.multipliers = cfg.multipliers or {}
    cfg.multipliers[key] = value
    local ok, err = writeJson(path, cfg)
    if not ok then return false, err end
    if Config and Config.reload then pcall(Config.reload) end
    return true
end

-- ---------------------------------------------------------------------------
-- Presets
-- ---------------------------------------------------------------------------

local function loadPresets()
    local path = modDir() .. "\\data\\presets.json"
    local presets = readJson(path)
    if type(presets) ~= "table" then
        log("error", "Failed to load presets from " .. path)
        return {}
    end
    return presets
end

local function listPresetNames()
    local names = {}
    for name in pairs(loadPresets()) do table.insert(names, name) end
    table.sort(names)
    return names
end

local function applyPreset(name)
    local presets = loadPresets()
    local p = presets[name]
    if not p then return nil, "Preset '" .. name .. "' not found" end
    if type(p.multipliers) ~= "table" then return nil, "Preset has no multipliers" end

    local lines = { "Applied preset '" .. name .. "':" }
    local anyApplied = false
    for key, value in pairs(p.multipliers) do
        if MULTIPLIERS[key] and validMultiplierValue(value) then
            local okPersist, persistErr = persistMultiplier(key, value)
            if okPersist then
                anyApplied = true
                local status = applyMultiplierToGame(key, value)
                table.insert(lines, string.format("  %s = %s  (%s)", key, tostring(value), status))
            else
                table.insert(lines, string.format("  %s = %s -- FAILED: %s", key, tostring(value), persistErr or "?"))
            end
        else
            table.insert(lines, "  skipped '" .. key .. "' (unknown key or bad value)")
        end
    end
    if anyApplied then return table.concat(lines, "\n"), nil end
    return nil, "No multipliers were applied (check preset file)"
end

-- ---------------------------------------------------------------------------
-- Admin action log
-- ---------------------------------------------------------------------------

local ADMIN_LOG_MAX = 200
local function adminLogPath()
    local dd = dataDir()
    if not dd then return nil end
    return dd .. "\\" .. ADMIN_LOG_NAME
end

local function appendAdminLog(cmd, args, result)
    local path = adminLogPath()
    if not path then return end
    local logtab = readJson(path) or {}
    if type(logtab) ~= "table" then logtab = {} end
    table.insert(logtab, {
        ts = os.time(), cmd = cmd, args = args or {}, result = result or "",
    })
    while #logtab > ADMIN_LOG_MAX do table.remove(logtab, 1) end
    pcall(writeJson, path, logtab)
end

local function recentAdminLog(limit)
    local path = adminLogPath()
    if not path then return {} end
    local logtab = readJson(path) or {}
    if type(logtab) ~= "table" then return {} end
    limit = math.min(limit or 25, #logtab)
    local out = {}
    for i = math.max(1, #logtab - limit + 1), #logtab do
        table.insert(out, logtab[i])
    end
    return out
end

-- ---------------------------------------------------------------------------
-- Teleport helpers (pure Lua via K2_TeleportTo UFunction)
-- ---------------------------------------------------------------------------

local function readPos(char)
    local ok, loc = pcall(function() return char:K2_GetActorLocation() end)
    if ok and loc then return loc.X, loc.Y, loc.Z end
    return nil
end

local function teleportTo(pawn, x, y, z)
    return pcall(function()
        return pawn:K2_TeleportTo({X = x, Y = y, Z = z}, {Pitch = 0, Yaw = 0, Roll = 0})
    end)
end

-- ---------------------------------------------------------------------------
-- Native-DLL wrapper helper (graceful fallback)
-- ---------------------------------------------------------------------------

local function nativeAvailable(fn_name)
    local fn = _G[fn_name]
    return type(fn) == "function", fn
end

-- ---------------------------------------------------------------------------
-- Death watch — poll HP each tick, snapshot last alive pos, persist last
-- death location per player to admiralspanel_data\death_locations.json so
-- the panel can offer a "TP to last death" button.
-- ---------------------------------------------------------------------------

local function deathFile()
    local dd = dataDir()
    if not dd then return nil end
    return dd .. "\\death_locations.json"
end

local function readDeaths()
    local p = deathFile()
    if not p then return {} end
    local t = readJson(p)
    return type(t) == "table" and t or {}
end

local function recordDeath(name, x, y, z)
    local p = deathFile()
    if not p then return end
    local t = readDeaths()
    t[name] = { x = x, y = y, z = z, t = os.time() }
    pcall(writeJson, p, t)
end

-- Per-player {hp, x, y, z} from the last tick. Lost on restart, which is
-- fine — death_locations.json is the persistent record.
local _deathwatch = { hp = {}, pos = {} }

local function readAllStatsJson()
    local has, fn = nativeAvailable("admiralspanel_native_allstats")
    if not (has and json) then return nil end
    local ok, msg = pcall(fn)
    if not ok or type(msg) ~= "string" then return nil end
    local ok2, data = pcall(json.decode, msg)
    if not ok2 then return nil end
    return data
end

local function deathwatchTick()
    local stats = readAllStatsJson()
    if not stats then return end
    local players = listPlayers()
    for _, p in ipairs(players) do
        local s = stats[p.name]
        if type(s) == "table" then
            local hp = tonumber(s.Health) or 0
            local x, y, z = readPos(p.pawn)
            if hp > 0 and x then
                _deathwatch.pos[p.name] = { x = x, y = y, z = z, t = os.time() }
            end
            local prev = _deathwatch.hp[p.name]
            if prev and prev > 0 and hp <= 0 then
                local last = _deathwatch.pos[p.name]
                if last then
                    recordDeath(p.name, last.x, last.y, last.z)
                    log("info", string.format("Death: %s @ (%.0f, %.0f, %.0f)",
                        p.name, last.x, last.y, last.z))
                end
            end
            _deathwatch.hp[p.name] = hp
        end
    end
end

if not _G._AP_DEATHWATCH_STARTED and type(LoopAsync) == "function" then
    _G._AP_DEATHWATCH_STARTED = true
    LoopAsync(1000, function()
        local ok, err = pcall(deathwatchTick)
        if not ok then log("error", "deathwatch tick failed: " .. tostring(err)) end
        return false
    end)
    log("info", "Death watch started (1s poll)")
end

-- Loot-multiplier tick. Applies the current loot factor to any newly-spawned
-- R5LootActor every 2s. Native side stores the factor + a processed-set so
-- this is a cheap walk that no-ops when factor=1.0 or no new actors appeared.
if not _G._AP_LOOTMULT_STARTED and type(LoopAsync) == "function" then
    _G._AP_LOOTMULT_STARTED = true
    LoopAsync(2000, function()
        local fn = _G["admiralspanel_native_loot_tick"]
        if type(fn) == "function" then pcall(fn) end
        return false
    end)
    log("info", "Loot multiplier tick started (2s poll)")
end

-- Harvest-yield tick. Re-applies the configured multiplier to all loot
-- tables every 2s, catching tables that load lazily as regions stream in.
if not _G._AP_HARVEST_TICK_STARTED and type(LoopAsync) == "function" then
    _G._AP_HARVEST_TICK_STARTED = true
    LoopAsync(2000, function()
        local fn = _G["admiralspanel_native_harvest_tick"]
        if type(fn) == "function" then pcall(fn) end
        return false
    end)
    log("info", "Harvest yield tick started (2s poll)")
end

-- Startup re-apply: after a server restart, the DLL's in-memory factor
-- defaults to 1.0 even though the JSON still has the user's chosen value.
-- Push each native-impl multiplier through once so the DLL state matches
-- what the user persisted. Defers a few seconds to let the native module
-- finish registering its functions.
if not _G._AP_MULT_REAPPLY_QUEUED and type(LoopAsync) == "function" then
    _G._AP_MULT_REAPPLY_QUEUED = true
    local _reapply_done = false
    LoopAsync(3000, function()
        if _reapply_done then return false end
        local cfg = nil
        local path
        if Config and Config._path then path = Config._path
        else
            local gd = gameDir()
            if gd then path = gd .. "\\admiralspanel.json" end
        end
        if path then cfg = readJson(path) end
        if not cfg or type(cfg.multipliers) ~= "table" then
            _reapply_done = true
            return false
        end
        for key, value in pairs(cfg.multipliers) do
            local spec = MULTIPLIERS[key]
            if spec and spec.impl == "native" and validMultiplierValue(value) then
                local fn = _G["admiralspanel_native_applyworldmult"]
                if type(fn) == "function" then
                    pcall(fn, key, value)
                    log("info", string.format("Re-applied %s = %s on startup", key, tostring(value)))
                end
            end
        end
        _reapply_done = true
        return false
    end)
end

-- ---------------------------------------------------------------------------
-- Commands
-- ---------------------------------------------------------------------------

-- MULTIPLIERS
API.registerCommand("ap.setmult", function(args)
    if #args < 2 then
        local keys = {}
        for k in pairs(MULTIPLIERS) do table.insert(keys, k) end
        table.sort(keys)
        return "Usage: ap.setmult <key> <value>\n  keys: " .. table.concat(keys, ", ")
    end
    local key, value = args[1], tonumber(args[2])
    if not MULTIPLIERS[key] then return "Unknown multiplier key: " .. key end
    if not validMultiplierValue(value) then return "Invalid value (0..999): " .. tostring(args[2]) end

    local okPersist, persistErr = persistMultiplier(key, value)
    local result
    if not okPersist then
        result = "Persist failed: " .. (persistErr or "?")
    else
        local status = applyMultiplierToGame(key, value)
        result = string.format("%s = %s  (%s)", key, tostring(value), status)
    end
    appendAdminLog("ap.setmult", args, result)
    return result
end, "Set a world multiplier", "ap.setmult <key> <value>")

API.registerCommand("ap.preset", function(args)
    if #args < 1 then
        return "Usage: ap.preset <name>\n  presets: " .. table.concat(listPresetNames(), ", ")
    end
    local out, err = applyPreset(args[1])
    local result = out or ("Failed: " .. (err or "?"))
    appendAdminLog("ap.preset", args, err and ("err: " .. err) or "ok")
    return result
end, "Apply a named multiplier preset", "ap.preset <name>")

-- ANNOUNCE
API.registerCommand("ap.say", function(args)
    if #args < 1 then return "Usage: ap.say <message>" end
    local msg = trim(table.concat(args, " "))
    if msg == "" then return "(empty message ignored)" end
    log("info", "BROADCAST: " .. msg)
    appendAdminLog("ap.say", args, "logged")
    return "Announced (log-only): " .. msg
end, "Broadcast to server log", "ap.say <message>")

API.registerCommand("ap.bringall", function(args)
    local players = listPlayers()
    if #players == 0 then return "No players online" end
    local n = 0
    for _, p in ipairs(players) do
        pcall(function()
            local mc = p.pawn.CharacterMovement or p.pawn.MovementComponent
            if mc and mc:IsValid() then
                mc.CheatMovementSpeedModifer = 1
                n = n + 1
            end
        end)
    end
    local result = "Reset speed modifier on " .. n .. " player(s). Use `wp.speed <player> 1` for a full baseline restore."
    appendAdminLog("ap.bringall", args, result)
    return result
end, "Reset all players' speed modifier to 1.0", "ap.bringall")

API.registerCommand("ap.adminlog", function(args)
    local limit = tonumber(args[1]) or 25
    local entries = recentAdminLog(limit)
    if #entries == 0 then return "(admin log is empty)" end
    local lines = { "Recent admin actions:" }
    for _, e in ipairs(entries) do
        table.insert(lines, string.format("  [%s] %s %s -> %s",
            os.date("%Y-%m-%d %H:%M:%S", e.ts), e.cmd,
            table.concat(e.args or {}, " "), e.result))
    end
    return table.concat(lines, "\n")
end, "Show recent AdmiralsPanel admin actions", "ap.adminlog [limit]")

-- TELEPORT (pure Lua, server-authoritative via K2_TeleportTo UFunction)
API.registerCommand("ap.tp", function(args)
    if #args < 2 then return "Usage: ap.tp <player> <targetPlayer>" end
    local src = findPlayerByName(args[1])
    local dst = findPlayerByName(args[2])
    if not src then return "Source '" .. args[1] .. "' not found" end
    if not dst then return "Target '" .. args[2] .. "' not found" end
    local x, y, z = readPos(dst.pawn)
    if not x then return "Could not read target position" end
    local ok, err = teleportTo(src.pawn, x, y, z)
    if ok then
        local result = string.format("Teleported %s -> %s (%.0f, %.0f, %.0f)", src.name, dst.name, x, y, z)
        appendAdminLog("ap.tp", args, result)
        return result
    end
    return "Teleport failed: " .. tostring(err)
end, "Teleport a player to another player", "ap.tp <player> <target>")

API.registerCommand("ap.tpxyz", function(args)
    if #args < 4 then return "Usage: ap.tpxyz <player> <x> <y> <z>" end
    local src = findPlayerByName(args[1])
    if not src then return "Player '" .. args[1] .. "' not found" end
    local x = tonumber(args[2]); local y = tonumber(args[3]); local z = tonumber(args[4])
    if not (x and y and z) then return "x/y/z must be numbers" end
    local ok, err = teleportTo(src.pawn, x, y, z)
    if ok then
        local result = string.format("Teleported %s -> (%.0f, %.0f, %.0f)", src.name, x, y, z)
        appendAdminLog("ap.tpxyz", args, result)
        return result
    end
    return "Teleport failed: " .. tostring(err)
end, "Teleport player to coordinates", "ap.tpxyz <player> <x> <y> <z>")

API.registerCommand("ap.tplastdeath", function(args)
    if #args < 1 then return "Usage: ap.tplastdeath <player>" end
    local p = findPlayerByName(args[1])
    if not p then return "Player '" .. args[1] .. "' not found" end
    local entry = readDeaths()[p.name]
    if not entry then return "No recorded death for " .. p.name end
    local ok, err = teleportTo(p.pawn, entry.x, entry.y, entry.z)
    if not ok then return "Teleport failed: " .. tostring(err) end
    local result = string.format("Teleported %s -> last death (%.0f, %.0f, %.0f)",
        p.name, entry.x, entry.y, entry.z)
    appendAdminLog("ap.tplastdeath", args, result)
    return result
end, "Teleport a player to their last recorded death location",
   "ap.tplastdeath <player>")

-- Players list with positions + last_death. Returns JSON string for the web
-- UI to populate state.status.players.
API.registerCommand("ap.players", function()
    if not json then return "[]" end
    local deaths = readDeaths()
    local out = {}
    for _, p in ipairs(listPlayers()) do
        local x, y, z = readPos(p.pawn)
        local row = { name = p.name }
        if x then row.x = x; row.y = y; row.z = z end
        local d = deaths[p.name]
        if d then row.last_death = d end
        out[#out+1] = row
    end
    local ok, encoded = pcall(json.encode, out)
    return ok and encoded or "[]"
end, "JSON list of online players with positions and last death", "ap.players")

-- NATIVE-BACKED commands (require AdmiralsPanelNative.dll)
-- Commands we exclude from the admin log because they are fired repeatedly
-- by the web panel's auto-polling (every few seconds) and swamp the log.
local NOLOG_COMMANDS = {
    ["ap.allstatsn"] = true,
    ["ap.adminlog"]  = true,
    ["ap.lootitems"] = true,
    ["ap.lootlistn"] = true,
    ["ap.itemlist"]  = true,
}

local function nativeWrapper(native_fn_name, usage_fmt, logcmd)
    return function(args)
        local has, fn = nativeAvailable(native_fn_name)
        if not has then
            return "Native feature unavailable (install AdmiralsPanelNative.dll — see cpp/README.md)"
        end
        local ok, result = pcall(fn, table.unpack(args))
        if not ok then return "Call failed: " .. tostring(result) end
        if not NOLOG_COMMANDS[logcmd] then
            appendAdminLog(logcmd, args, tostring(result))
        end
        return tostring(result)
    end
end

API.registerCommand("ap.healn",   nativeWrapper("admiralspanel_native_heal",   nil, "ap.healn"),   "Heal a player (native)",      "ap.healn <player> <amount>")
API.registerCommand("ap.damagen", nativeWrapper("admiralspanel_native_damage", nil, "ap.damagen"), "Damage a player (native)",    "ap.damagen <player> <amount>")
API.registerCommand("ap.killn",   nativeWrapper("admiralspanel_native_kill",   nil, "ap.killn"),   "Kill a player (native)",      "ap.killn <player>")
API.registerCommand("ap.feedn",   nativeWrapper("admiralspanel_native_feed",   nil, "ap.feedn"),   "Refill Health + Stamina",     "ap.feedn <player>")
API.registerCommand("ap.reviven", nativeWrapper("admiralspanel_native_revive", nil, "ap.reviven"), "Set Health to MaxHealth",     "ap.reviven <player>")
API.registerCommand("ap.setattrn",nativeWrapper("admiralspanel_native_setattr",nil, "ap.setattrn"),"Set any R5AttributeSet field","ap.setattrn <player> <attr> <value>")
API.registerCommand("ap.readattrn",nativeWrapper("admiralspanel_native_readattr",nil,"ap.readattrn"),"Read any R5AttributeSet field","ap.readattrn <player> <attr>")
API.registerCommand("ap.allstatsn",nativeWrapper("admiralspanel_native_allstats",nil,"ap.allstatsn"),"JSON of all players' Health/Stamina/Comfort/Posture","ap.allstatsn")
API.registerCommand("ap.classprobe",nativeWrapper("admiralspanel_native_classprobe",nil,"ap.classprobe"),"Dump a UClass's properties (reverse-eng tool)","ap.classprobe <ClassName>")
API.registerCommand("ap.scan",nativeWrapper("admiralspanel_native_scan",nil,"ap.scan"),"Scan loaded UObjects for classes matching substring","ap.scan <substring>")
API.registerCommand("ap.scanpath",nativeWrapper("admiralspanel_native_scanpath",nil,"ap.scanpath"),"Scan UObject full-paths for substring (e.g. /Game/Items/)","ap.scanpath <substring>")
API.registerCommand("ap.rawdumpn",nativeWrapper("admiralspanel_native_rawdump",nil,"ap.rawdumpn"),"Hex-dump a UObject (class name or full /path)","ap.rawdumpn <target> [bytes]")
API.registerCommand("ap.dumpclassn",nativeWrapper("admiralspanel_native_dumpclass",nil,"ap.dumpclassn"),"List first N live instances of a class with addr+path","ap.dumpclassn <classname> [N]")
API.registerCommand("ap.spawnn",nativeWrapper("admiralspanel_native_spawn",nil,"ap.spawnn"),"Spawn an Actor class at player's feet","ap.spawnn <player> <class_path> [dx dy dz]")
API.registerCommand("ap.locn",nativeWrapper("admiralspanel_native_loc",nil,"ap.locn"),"Print player's location","ap.locn <player>")
API.registerCommand("ap.findclassn",nativeWrapper("admiralspanel_native_findclass",nil,"ap.findclassn"),"Resolve a full object path","ap.findclassn <path>")
API.registerCommand("ap.funcparamsn",nativeWrapper("admiralspanel_native_funcparams",nil,"ap.funcparamsn"),"Dump a UFunction's param layout","ap.funcparamsn <path>")
API.registerCommand("ap.yankactorn",nativeWrapper("admiralspanel_native_yankactor",nil,"ap.yankactorn"),"Teleport any actor (by full path) to player","ap.yankactorn <player> <path>")
API.registerCommand("ap.giveloot",nativeWrapper("admiralspanel_native_giveloot",nil,"ap.giveloot"),"Yank N populated loot actors to player (auto-pickup)","ap.giveloot <player> [count]")
API.registerCommand("ap.lootlistn",nativeWrapper("admiralspanel_native_lootlist",nil,"ap.lootlistn"),"List populated R5LootActors in world","ap.lootlistn [N]")
API.registerCommand("ap.lootinspectn",nativeWrapper("admiralspanel_native_lootinspect",nil,"ap.lootinspectn"),"Hex-dump first populated R5LootActor's LootView","ap.lootinspectn [bytes]")
API.registerCommand("ap.lootitems",nativeWrapper("admiralspanel_native_lootitems",nil,"ap.lootitems"),"List populated R5LootActors with their item contents","ap.lootitems [N]")
API.registerCommand("ap.giveitem",nativeWrapper("admiralspanel_native_giveitem",nil,"ap.giveitem"),"Teleport a loot actor containing a matching item to the player","ap.giveitem <player> <search>")
API.registerCommand("ap.itemlist",nativeWrapper("admiralspanel_native_itemlist",nil,"ap.itemlist"),"List known UR5BLInventoryItem data assets (optionally filtered)","ap.itemlist [search]")
API.registerCommand("ap.itemcatalog",nativeWrapper("admiralspanel_native_itemcatalog",nil,"ap.itemcatalog"),"JSON array of all 1268 items with name+path (used by Spawn UI)","ap.itemcatalog")
API.registerCommand("ap.itemscan",nativeWrapper("admiralspanel_native_itemscan",nil,"ap.itemscan"),"Scan an object's memory for UR5BLInventoryItem references","ap.itemscan <target> [bytes]")
API.registerCommand("ap.lootslots",nativeWrapper("admiralspanel_native_lootslots",nil,"ap.lootslots"),"Hex-dump slot structs of first populated R5LootActor (RE diagnostic)","ap.lootslots [N]")
API.registerCommand("ap.dumpinv",  nativeWrapper("admiralspanel_native_dumpinv",  nil, "ap.dumpinv"),  "Dump server-side inventory hierarchy state for a player (recon)","ap.dumpinv <player>")
API.registerCommand("ap.overwriteinv",nativeWrapper("admiralspanel_native_overwriteinv",nil,"ap.overwriteinv"),"Test: overwrite an asset path substring in player inventory memory","ap.overwriteinv <player> [from] [to]")
API.registerCommand("ap.findn",   nativeWrapper("admiralspanel_native_find",   nil, "ap.findn"),   "Debug: find pawn by name",    "ap.findn <player>")
API.registerCommand("ap.inspectn",nativeWrapper("admiralspanel_native_inspect",nil, "ap.inspectn"),"Debug: dump object properties","ap.inspectn <player>")

-- ---------------------------------------------------------------------------
-- Boot
-- ---------------------------------------------------------------------------

log("info", string.format("%s v%s loaded — pure-Lua: setmult preset say bringall adminlog tp tpxyz; native: heal damage kill feed revive setattr readattr find inspect",
    MOD_NAME, VERSION))
log("info", "Web panel: http://localhost:<dashboard_port>/admiral.html")
