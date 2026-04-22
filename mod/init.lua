-- AdmiralsPanel — server-side Lua backend
-- Adds ap.* RCON commands used by the /admiral/ web panel.
-- Loaded by WindrosePlus via Mods/admiral-admin/mod.json (main=init.lua).
--
-- Safe to hot-reload: every handler is pcall-wrapped; persistent state
-- lives on disk, not in module globals.

local API    = WindrosePlus.API
local json   = require("modules.json")
local Config = require("modules.config")

local MOD_NAME = "AdmiralsPanel"
local VERSION  = "0.1.0"

-- ---------------------------------------------------------------------------
-- Paths
-- ---------------------------------------------------------------------------

-- Our mod file lives at:
--   <gamedir>\R5\Binaries\Win64\ue4ss\Mods\WindrosePlus\Mods\admiral-admin\init.lua
-- Config._path holds the absolute path to windrose_plus.json; strip the
-- filename to get the game directory.
local function gameDir()
    local ok, p = pcall(function() return Config._path end)
    if not ok or not p then return nil end
    return p:match("^(.*)[\\/][^\\/]+$")
end

local function modDir()
    local src = debug.getinfo(1, "S").source
    local path = src:sub(1, 1) == "@" and src:sub(2) or src
    return path:match("^(.*)[\\/][^\\/]+$") or ""
end

local function dataDir()
    local gd = gameDir()
    if not gd then return nil end
    return gd .. "\\windrose_plus_data"
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

-- Atomic: write to .tmp, os.rename onto final path.
local function writeJson(path, data)
    local ok, encoded = pcall(json.encode, data)
    if not ok then return false, "encode failed" end
    local tmp = path .. ".tmp"
    local f = io.open(tmp, "w")
    if not f then return false, "cannot open tmp file: " .. tmp end
    f:write(encoded)
    f:close()
    os.remove(path) -- os.rename fails on Windows if target exists
    local okRename = os.rename(tmp, path)
    if not okRename then return false, "rename failed" end
    return true
end

local function trim(s)
    return (s:gsub("^%s+", ""):gsub("%s+$", ""))
end

-- ---------------------------------------------------------------------------
-- Player lookup — PlayerController -> PlayerState -> PlayerNamePrivate
-- Mirrors the pattern from the shipped admin.lua (wp.speed) so matching
-- is consistent with the rest of WindrosePlus.
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

-- Returns array of {name, pc, pawn} for all valid PlayerControllers.
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
-- Multipliers
-- ---------------------------------------------------------------------------

-- JSON key -> { description, ue_property_candidates }
-- UE property names come from Admin._UE4_PROPS in the shipped admin.lua
-- (line 1028-1075). Multiple candidates are tried in order.
local MULTIPLIERS = {
    loot            = { desc = "Loot drop multiplier",         ue = {"LootMultiplier"} },
    xp              = { desc = "XP / experience multiplier",   ue = {"XPMultiplier", "ExperienceMultiplier"} },
    weight          = { desc = "Carry weight multiplier",      ue = {"WeightMultiplier"} },
    craft_cost      = { desc = "Crafting cost multiplier",     ue = {"CraftCostMultiplier"} },
    stack_size      = { desc = "Inventory stack size mult.",   ue = {"StackSizeMultiplier"} },
    crop_speed      = { desc = "Crop growth speed multiplier", ue = {"CropGrowthMultiplier", "CropSpeedMultiplier"} },
    cooking_speed   = { desc = "Cooking speed multiplier",     ue = {"CookingSpeedMultiplier", "CookingSpeed"} },
    inventory_size  = { desc = "Inventory size multiplier",    ue = {"InventorySizeMultiplier"} },
    points_per_level= { desc = "Skill points per level mult.", ue = {"PointsPerLevelMultiplier"} },
    harvest_yield   = { desc = "Harvest yield multiplier",     ue = {"HarvestMultiplier", "HarvestYieldMultiplier"} },
}

-- Range: we don't strictly clamp (user might want extreme event values) but
-- do sanity-check to reject garbage like negatives.
local function validMultiplierValue(v)
    return type(v) == "number" and v >= 0 and v < 1000
end

-- Try writing value to each candidate UE property on all R5GameMode instances.
-- Returns a list of property names that accepted the write.
local function applyMultiplierToGame(key, value)
    local spec = MULTIPLIERS[key]
    if not spec then return {} end
    local applied = {}
    local gms = FindAllOf("R5GameMode")
    if not gms then return applied end
    for _, gm in ipairs(gms) do
        if gm:IsValid() then
            for _, prop in ipairs(spec.ue) do
                pcall(function()
                    gm[prop] = value
                    table.insert(applied, prop)
                end)
            end
        end
    end
    return applied
end

-- Persist multiplier change to windrose_plus.json + Config.reload()
local function persistMultiplier(key, value)
    local path = Config._path
    if not path then return false, "Config path unknown" end

    local cfg = readJson(path)
    if not cfg then return false, "Cannot read " .. path end

    cfg.multipliers = cfg.multipliers or {}
    cfg.multipliers[key] = value

    local ok, err = writeJson(path, cfg)
    if not ok then return false, err end

    pcall(Config.reload)
    return true
end

-- ---------------------------------------------------------------------------
-- Presets
-- ---------------------------------------------------------------------------

-- Load preset bundles from mod's data/presets.json at every invocation so
-- edits to that file are picked up without a hot-reload.
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

-- Apply a preset: iterate its multipliers, persist each, apply to game.
-- Returns a list of human-readable lines to show the admin.
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
            local applied = applyMultiplierToGame(key, value)
            if okPersist then
                anyApplied = true
                local liveMarker = #applied > 0 and " (live: " .. table.concat(applied, ",") .. ")" or " (saved; applies on restart)"
                table.insert(lines, string.format("  %s = %s%s", key, tostring(value), liveMarker))
            else
                table.insert(lines, string.format("  %s = %s — FAILED: %s", key, tostring(value), persistErr or "?"))
            end
        else
            table.insert(lines, "  skipped '" .. key .. "' (unknown key or bad value)")
        end
    end

    if anyApplied then
        return table.concat(lines, "\n"), nil
    end
    return nil, "No multipliers were applied (check preset file)"
end

-- ---------------------------------------------------------------------------
-- Admin action log (persisted to windrose_plus_data/admiral_admin_log.json)
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
    local log = readJson(path) or {}
    if type(log) ~= "table" then log = {} end
    table.insert(log, {
        ts = os.time(),
        cmd = cmd,
        args = args or {},
        result = result or "",
    })
    while #log > ADMIN_LOG_MAX do table.remove(log, 1) end
    pcall(writeJson, path, log)
end

local function recentAdminLog(limit)
    local path = adminLogPath()
    if not path then return {} end
    local log = readJson(path) or {}
    if type(log) ~= "table" then return {} end
    limit = math.min(limit or 25, #log)
    local out = {}
    for i = math.max(1, #log - limit + 1), #log do
        table.insert(out, log[i])
    end
    return out
end

-- ---------------------------------------------------------------------------
-- RCON commands
-- ---------------------------------------------------------------------------

API.registerCommand("ap.setmult", function(args)
    if #args < 2 then
        local keys = {}
        for k in pairs(MULTIPLIERS) do table.insert(keys, k) end
        table.sort(keys)
        return "Usage: ap.setmult <key> <value>\n  keys: " .. table.concat(keys, ", ")
    end
    local key = args[1]
    local value = tonumber(args[2])
    if not MULTIPLIERS[key] then
        return "Unknown multiplier key: " .. key
    end
    if not validMultiplierValue(value) then
        return "Invalid value (expected number 0..999): " .. tostring(args[2])
    end

    local okPersist, persistErr = persistMultiplier(key, value)
    local applied = applyMultiplierToGame(key, value)

    local result
    if not okPersist then
        result = "Persist failed: " .. (persistErr or "?")
    elseif #applied == 0 then
        result = string.format("%s = %s  (saved; applies on next restart — no live property found)", key, tostring(value))
    else
        result = string.format("%s = %s  (live: %s)", key, tostring(value), table.concat(applied, ", "))
    end
    appendAdminLog("ap.setmult", args, result)
    return result
end, "Set a world multiplier (live where possible, saved to windrose_plus.json)", "ap.setmult <key> <value>")

API.registerCommand("ap.preset", function(args)
    if #args < 1 then
        return "Usage: ap.preset <name>\n  presets: " .. table.concat(listPresetNames(), ", ")
    end
    local name = args[1]
    local out, err = applyPreset(name)
    local result = out or ("Failed: " .. (err or "?"))
    appendAdminLog("ap.preset", args, err and ("err: " .. err) or "ok")
    return result
end, "Apply a named multiplier preset", "ap.preset <name>")

API.registerCommand("ap.say", function(args)
    if #args < 1 then
        return "Usage: ap.say <message>"
    end
    local msg = trim(table.concat(args, " "))
    if msg == "" then return "(empty message ignored)" end
    log("info", "BROADCAST: " .. msg)
    appendAdminLog("ap.say", args, "logged")
    -- Note: in-game chat delivery requires calling a UFunction Windrose
    -- does not yet expose to Lua. This command currently logs only; the
    -- web panel shows these to operators. See docs/roadmap.md.
    return "Announced (log-only for now): " .. msg
end, "Broadcast an announcement to server log / events log", "ap.say <message>")

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
        local argstr = table.concat(e.args or {}, " ")
        table.insert(lines, string.format("  [%s] %s %s -> %s",
            os.date("%Y-%m-%d %H:%M:%S", e.ts), e.cmd, argstr, e.result))
    end
    return table.concat(lines, "\n")
end, "Show recent AdmiralsPanel admin actions", "ap.adminlog [limit]")

-- ---------------------------------------------------------------------------
-- Boot message
-- ---------------------------------------------------------------------------

log("info", string.format("%s v%s loaded — commands: ap.setmult, ap.preset, ap.say, ap.bringall, ap.adminlog",
    MOD_NAME, VERSION))
log("info", "Web panel: http://localhost:<dashboard_port>/admiral.html (once web/ is deployed)")
