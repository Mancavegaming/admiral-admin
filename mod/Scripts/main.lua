-- AdmiralsPanel standalone UE4SS Lua mod entry point.
--
-- UE4SS runs this automatically for any directory under
--   <game>/R5/Binaries/Win64/ue4ss/Mods/<ModName>/
-- listed in enabled.txt. Here we bootstrap AdmiralsPanel.API (our own
-- command registry + logger), start the spool poller that bridges HTTP
-- requests from the native DLL's server, then delegate to init.lua for
-- the actual command implementations.

local API   = require("ap_api")
local Spool = require("ap_spool")

-- Publish the API as a global so init.lua can detect standalone mode.
AdmiralsPanel     = AdmiralsPanel or {}
AdmiralsPanel.API = API

API.log("info", "AP", "Standalone mode: AdmiralsPanel v" .. API.VERSION)

-- Start the spool poller before running user code so the first commands
-- find it ready.
Spool.start(API, 150)

-- Load init.lua from this mod's Scripts dir.
local src = debug.getinfo(1, "S").source:sub(2) -- strip '@'
local dir = src:match("^(.*)[/\\]")
if dir and dir ~= "" then
    local init_path = dir .. "\\init.lua"
    local chunk, err = loadfile(init_path)
    if chunk then
        local ok, exec_err = pcall(chunk)
        if not ok then API.log("error", "AP", "init.lua failed: " .. tostring(exec_err)) end
    else
        API.log("error", "AP", "load init.lua failed: " .. tostring(err))
    end
else
    API.log("error", "AP", "could not resolve mod dir from " .. src)
end
