-- AdmiralsPanel command-spool poller. Reads cmd_*.json files the native
-- HTTP server drops into <game_dir>/admiralspanel_data/rcon and dispatches
-- them through the registered command handlers, writing res_*.json back.
--
-- LoopAsync fires the poll at a short interval; each tick processes all
-- pending entries and clears pending_commands.txt.

local Json = require("ap_json")

local Spool = {}

-- Discover the game dir. The dedicated server exe lives at
--   <gamedir>\R5\Binaries\Win64\WindroseServer-Win64-Shipping.exe
-- and LUA_PATH is rooted at the Scripts/ folder of our mod, so we walk
-- relative from the exe-derived cwd.
local function game_dir()
    -- UE4SS's working directory is usually <game>\R5\Binaries\Win64, so we
    -- walk up three levels to reach the game root.
    local cwd
    do
        local h = io.popen("cd")
        if h then cwd = h:read("*l"); h:close() end
    end
    if not cwd or cwd == "" then return "." end
    local up = cwd
    for _ = 1, 3 do
        local parent = up:match("^(.+)[\\/][^\\/]+$")
        if parent and parent ~= "" then up = parent end
    end
    return up
end

local SPOOL_DIR   = nil
local PENDING_TXT = nil

local function ensure_paths()
    if SPOOL_DIR then return true end
    local gd = game_dir()
    SPOOL_DIR   = gd .. "\\admiralspanel_data\\rcon"
    PENDING_TXT = SPOOL_DIR .. "\\pending_commands.txt"
    return true
end

local function file_exists(p)
    local f = io.open(p, "rb")
    if not f then return false end
    f:close()
    return true
end

local function read_all(p)
    local f = io.open(p, "rb")
    if not f then return nil end
    local s = f:read("*a")
    f:close()
    return s
end

local function write_all(p, content)
    local f = io.open(p, "wb")
    if not f then return false end
    f:write(content)
    f:close()
    return true
end

local function delete(p)
    os.remove(p)
end

-- Process one command file. Returns true on success.
local function process(filename, api)
    local cmd_path = SPOOL_DIR .. "\\" .. filename
    local body = read_all(cmd_path)
    delete(cmd_path)
    if not body or body == "" then return false end

    local data = Json.decode(body)
    if not data or not data.id or not data.command then return false end

    local id = tostring(data.id):gsub("[^%w_%-]", "")
    if id == "" then id = "0" end
    local command   = data.command
    local args      = data.args or {}
    local originalCommand = command

    -- Allow "ap.foo arg1 arg2" syntax when args[] wasn't provided.
    if #args == 0 then
        local parts = {}
        for w in command:gmatch("%S+") do parts[#parts+1] = w end
        if #parts > 1 then
            command = parts[1]
            for i = 2, #parts do args[#args+1] = parts[i] end
        end
    end

    local status, message = api.execute(command, args)

    local res_path = SPOOL_DIR .. "\\res_" .. id .. ".json"
    local payload = Json.encode({
        id        = id,
        status    = status,
        message   = message,
        command   = originalCommand,
        timestamp = os.time(),
    })
    write_all(res_path, payload)
    return true
end

-- Read + clear pending_commands.txt, then process everything listed.
function Spool.tick(api)
    ensure_paths()
    if not file_exists(PENDING_TXT) then return 0 end

    local content = read_all(PENDING_TXT)
    -- Clear atomically by writing empty content; if we fail later we'll
    -- just miss those commands (they time out on the HTTP side).
    write_all(PENDING_TXT, "")
    if not content or content == "" then return 0 end

    local count = 0
    for line in content:gmatch("[^\r\n]+") do
        if line ~= "" then
            local ok, err = pcall(process, line, api)
            if ok then count = count + 1
            else api.log("error", "AP.Spool", "process failed: " .. tostring(err)) end
        end
    end
    return count
end

function Spool.start(api, interval_ms)
    ensure_paths()
    local iv = interval_ms or 200
    api.log("info", "AP.Spool", "polling " .. SPOOL_DIR .. " every " .. iv .. "ms")
    LoopAsync(iv, function()
        local ok, err = pcall(Spool.tick, api)
        if not ok then api.log("error", "AP.Spool", "tick failed: " .. tostring(err)) end
        return false
    end)
end

return Spool
