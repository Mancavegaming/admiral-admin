-- Minimal JSON encode/decode for the AdmiralsPanel spool bridge.
-- Handles: strings, numbers, booleans, null, arrays (sequential tables),
--          objects (non-sequential tables). No comments, no trailing commas.
-- Sized for cmd/res payloads we exchange with the native side, not a general
-- JSON parser — it's fine for flat {id,command,args,ts} records.

local Json = {}

----------------- decode --------------------------------------------------

local function skip_ws(s, i)
    while i <= #s do
        local c = s:sub(i, i)
        if c == " " or c == "\t" or c == "\n" or c == "\r" then
            i = i + 1
        else
            return i
        end
    end
    return i
end

local parse_value

local function parse_string(s, i)
    -- assume s[i] == '"'
    i = i + 1
    local out = {}
    while i <= #s do
        local c = s:sub(i, i)
        if c == '"' then
            return table.concat(out), i + 1
        elseif c == "\\" then
            local nx = s:sub(i + 1, i + 1)
            if     nx == '"'  then out[#out+1] = '"'
            elseif nx == "\\" then out[#out+1] = "\\"
            elseif nx == "/"  then out[#out+1] = "/"
            elseif nx == "n"  then out[#out+1] = "\n"
            elseif nx == "r"  then out[#out+1] = "\r"
            elseif nx == "t"  then out[#out+1] = "\t"
            elseif nx == "u"  then
                local hex = s:sub(i + 2, i + 5)
                local n = tonumber(hex, 16) or 0
                if n < 128 then
                    out[#out+1] = string.char(n)
                else
                    -- lossy ASCII fallback — we don't use unicode in our payloads
                    out[#out+1] = "?"
                end
                i = i + 4
            else
                out[#out+1] = nx
            end
            i = i + 2
        else
            out[#out+1] = c
            i = i + 1
        end
    end
    error("unterminated string at " .. i)
end

local function parse_number(s, i)
    local start = i
    if s:sub(i, i) == "-" then i = i + 1 end
    while i <= #s do
        local c = s:sub(i, i)
        if c:match("[0-9eE%.%+%-]") then
            i = i + 1
        else
            break
        end
    end
    return tonumber(s:sub(start, i - 1)), i
end

local function parse_array(s, i)
    i = i + 1 -- skip [
    local arr = {}
    i = skip_ws(s, i)
    if s:sub(i, i) == "]" then return arr, i + 1 end
    while i <= #s do
        local v
        v, i = parse_value(s, i)
        arr[#arr + 1] = v
        i = skip_ws(s, i)
        local c = s:sub(i, i)
        if c == "," then i = i + 1; i = skip_ws(s, i)
        elseif c == "]" then return arr, i + 1
        else error("expected , or ] at " .. i) end
    end
    error("unterminated array")
end

local function parse_object(s, i)
    i = i + 1 -- skip {
    local obj = {}
    i = skip_ws(s, i)
    if s:sub(i, i) == "}" then return obj, i + 1 end
    while i <= #s do
        i = skip_ws(s, i)
        if s:sub(i, i) ~= '"' then error("expected string key at " .. i) end
        local k
        k, i = parse_string(s, i)
        i = skip_ws(s, i)
        if s:sub(i, i) ~= ":" then error("expected : at " .. i) end
        i = i + 1
        i = skip_ws(s, i)
        local v
        v, i = parse_value(s, i)
        obj[k] = v
        i = skip_ws(s, i)
        local c = s:sub(i, i)
        if c == "," then i = i + 1
        elseif c == "}" then return obj, i + 1
        else error("expected , or } at " .. i) end
    end
    error("unterminated object")
end

parse_value = function(s, i)
    i = skip_ws(s, i)
    local c = s:sub(i, i)
    if c == '"' then return parse_string(s, i)
    elseif c == "{" then return parse_object(s, i)
    elseif c == "[" then return parse_array(s, i)
    elseif c == "t" and s:sub(i, i + 3) == "true"  then return true,  i + 4
    elseif c == "f" and s:sub(i, i + 4) == "false" then return false, i + 5
    elseif c == "n" and s:sub(i, i + 3) == "null"  then return nil,   i + 4
    elseif c == "-" or c:match("[0-9]") then return parse_number(s, i)
    else error("unexpected char " .. c .. " at " .. i) end
end

function Json.decode(s)
    if not s or s == "" then return nil end
    local ok, v = pcall(parse_value, s, 1)
    if ok then return v end
    return nil, v
end

----------------- encode --------------------------------------------------

local encode_value

local function encode_string(s)
    local out = {'"'}
    for i = 1, #s do
        local c = s:sub(i, i)
        local b = s:byte(i)
        if     c == '"'  then out[#out+1] = '\\"'
        elseif c == "\\" then out[#out+1] = "\\\\"
        elseif b == 0x0A then out[#out+1] = "\\n"
        elseif b == 0x0D then out[#out+1] = "\\r"
        elseif b == 0x09 then out[#out+1] = "\\t"
        elseif b < 0x20  then out[#out+1] = string.format("\\u%04x", b)
        else                  out[#out+1] = c end
    end
    out[#out+1] = '"'
    return table.concat(out)
end

local function is_array(t)
    local n = 0
    for k, _ in pairs(t) do
        if type(k) ~= "number" then return false end
        if k ~= math.floor(k) or k < 1 then return false end
        if k > n then n = k end
    end
    return true, n
end

encode_value = function(v)
    local tp = type(v)
    if v == nil or tp == "nil"  then return "null"
    elseif tp == "boolean"      then return v and "true" or "false"
    elseif tp == "number"       then
        if v ~= v or v == math.huge or v == -math.huge then return "null" end
        return tostring(v)
    elseif tp == "string"       then return encode_string(v)
    elseif tp == "table"        then
        local arr, n = is_array(v)
        if arr then
            local parts = {}
            for i = 1, n do parts[i] = encode_value(v[i]) end
            return "[" .. table.concat(parts, ",") .. "]"
        else
            local parts = {}
            for k, val in pairs(v) do
                parts[#parts+1] = encode_string(tostring(k)) .. ":" .. encode_value(val)
            end
            return "{" .. table.concat(parts, ",") .. "}"
        end
    end
    return "null"
end

function Json.encode(v)
    return encode_value(v)
end

return Json
