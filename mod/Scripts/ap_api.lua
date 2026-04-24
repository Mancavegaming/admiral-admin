-- AdmiralsPanel standalone API surface. Loaded by Scripts/main.lua when
-- running as a standalone UE4SS mod (no WindrosePlus). Mirrors the subset
-- of WindrosePlus.API that mod/init.lua uses so the same code works under
-- either bootstrap.

local API = {}

API.VERSION = "0.6.0"

-- log(level, source, msg) — write to stdout / UE4SS log.
function API.log(level, source, msg)
    print(string.format("[%s] [%s] %s",
        tostring(level):upper(), tostring(source), tostring(msg)))
end

-- Command registry. { name -> { handler, description, usage } }
local Commands = {}

function API.registerCommand(name, handler, description, usage)
    Commands[name] = {
        handler     = handler,
        description = description or "",
        usage       = usage or name,
    }
    API.log("info", "AP.API", "registered: " .. name)
end

function API.getCommand(name)
    return Commands[name]
end

function API.listCommands()
    local out = {}
    for k, v in pairs(Commands) do
        out[#out+1] = { name = k, description = v.description, usage = v.usage }
    end
    table.sort(out, function(a, b) return a.name < b.name end)
    return out
end

-- Execute a registered command synchronously and return
--   (status, message). Used by the spool poller.
function API.execute(name, args)
    local entry = Commands[name]
    if not entry then return "error", "unknown command: " .. tostring(name) end
    local ok, result = pcall(entry.handler, args or {})
    if not ok then return "error", tostring(result) end
    -- registerCommand handlers return either a string (message) or a
    -- (status, message) pair. Normalize.
    if type(result) == "table" and result.status then
        return result.status, result.message or ""
    end
    return "ok", result == nil and "" or tostring(result)
end

return API
