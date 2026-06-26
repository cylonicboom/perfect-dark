-- Minimal JSON encode/decode for the Archipelago bridge. Pure Lua, no deps.
-- Handles the AP message subset: objects, arrays, strings, numbers, booleans,
-- null. Returns the json table (use: local json = dofile("scripts/ap/json.lua")).
--
-- Limitations (acceptable for the AP wire format): JSON null decodes to Lua nil
-- (so a null inside an array creates a hole -- AP arrays don't carry nulls);
-- \u escapes handle the BMP only (no surrogate-pair joining).

local json = {}

-- ----------------------------------------------------------------- encode ---
local escape_map = {
  ['"'] = '\\"', ['\\'] = '\\\\', ['\b'] = '\\b', ['\f'] = '\\f',
  ['\n'] = '\\n', ['\r'] = '\\r', ['\t'] = '\\t',
}

local function escape_char(c)
  return escape_map[c] or string.format("\\u%04x", string.byte(c))
end

local function encode_string(s)
  return '"' .. s:gsub('[%z\1-\31\\"]', escape_char) .. '"'
end

local encode_value

local function is_array(t)
  local n = 0
  for k in pairs(t) do
    if type(k) ~= "number" then return false end
    n = n + 1
  end
  for i = 1, n do
    if t[i] == nil then return false end
  end
  return true, n
end

local function encode_table(t)
  local arr, n = is_array(t)
  local parts = {}
  if arr then
    for i = 1, n do parts[i] = encode_value(t[i]) end
    return "[" .. table.concat(parts, ",") .. "]"
  end
  local i = 0
  for k, v in pairs(t) do
    i = i + 1
    parts[i] = encode_string(tostring(k)) .. ":" .. encode_value(v)
  end
  return "{" .. table.concat(parts, ",") .. "}"
end

encode_value = function(v)
  local tv = type(v)
  if v == nil then return "null"
  elseif tv == "boolean" then return v and "true" or "false"
  elseif tv == "number" then
    if v % 1 == 0 and v == v and v ~= math.huge and v ~= -math.huge then
      return string.format("%d", v)
    end
    return string.format("%.14g", v)
  elseif tv == "string" then return encode_string(v)
  elseif tv == "table" then return encode_table(v)
  else error("json: cannot encode " .. tv) end
end

function json.encode(v)
  return encode_value(v)
end

-- ----------------------------------------------------------------- decode ---
local decode_value, decode_object, decode_array, decode_string

local function decode_error(str, i, msg)
  error(string.format("json decode error at %d: %s", i, msg))
end

local function skip_ws(str, i)
  return (str:find("[^ \t\r\n]", i)) or (#str + 1)
end

local function utf8_char(cp)
  if cp < 0x80 then
    return string.char(cp)
  elseif cp < 0x800 then
    return string.char(0xC0 + math.floor(cp / 0x40), 0x80 + cp % 0x40)
  else
    return string.char(0xE0 + math.floor(cp / 0x1000),
                       0x80 + math.floor(cp / 0x40) % 0x40,
                       0x80 + cp % 0x40)
  end
end

local esc_decode = {
  ['"'] = '"', ['\\'] = '\\', ['/'] = '/',
  b = '\b', f = '\f', n = '\n', r = '\r', t = '\t',
}

decode_string = function(str, i)
  local res = {}
  local j = i + 1
  while j <= #str do
    local c = str:sub(j, j)
    if c == '"' then
      return table.concat(res), j + 1
    elseif c == '\\' then
      local nx = str:sub(j + 1, j + 1)
      if nx == 'u' then
        local cp = tonumber(str:sub(j + 2, j + 5), 16)
        if not cp then decode_error(str, j, "bad \\u escape") end
        res[#res + 1] = utf8_char(cp)
        j = j + 6
      else
        res[#res + 1] = esc_decode[nx] or nx
        j = j + 2
      end
    else
      res[#res + 1] = c
      j = j + 1
    end
  end
  decode_error(str, i, "unterminated string")
end

decode_object = function(str, i)
  local obj = {}
  i = skip_ws(str, i + 1)
  if str:sub(i, i) == '}' then return obj, i + 1 end
  while true do
    i = skip_ws(str, i)
    if str:sub(i, i) ~= '"' then decode_error(str, i, "expected key string") end
    local key
    key, i = decode_string(str, i)
    i = skip_ws(str, i)
    if str:sub(i, i) ~= ':' then decode_error(str, i, "expected ':'") end
    local val
    val, i = decode_value(str, i + 1)
    obj[key] = val
    i = skip_ws(str, i)
    local c = str:sub(i, i)
    if c == ',' then
      i = i + 1
    elseif c == '}' then
      return obj, i + 1
    else
      decode_error(str, i, "expected ',' or '}'")
    end
  end
end

decode_array = function(str, i)
  local arr = {}
  i = skip_ws(str, i + 1)
  if str:sub(i, i) == ']' then return arr, i + 1 end
  while true do
    local val
    val, i = decode_value(str, i)
    arr[#arr + 1] = val
    i = skip_ws(str, i)
    local c = str:sub(i, i)
    if c == ',' then
      i = i + 1
    elseif c == ']' then
      return arr, i + 1
    else
      decode_error(str, i, "expected ',' or ']'")
    end
  end
end

decode_value = function(str, i)
  i = skip_ws(str, i)
  local c = str:sub(i, i)
  if c == '{' then return decode_object(str, i)
  elseif c == '[' then return decode_array(str, i)
  elseif c == '"' then return decode_string(str, i)
  elseif c == 't' then
    if str:sub(i, i + 3) == "true" then return true, i + 4 end
    decode_error(str, i, "invalid literal")
  elseif c == 'f' then
    if str:sub(i, i + 4) == "false" then return false, i + 5 end
    decode_error(str, i, "invalid literal")
  elseif c == 'n' then
    if str:sub(i, i + 3) == "null" then return nil, i + 4 end
    decode_error(str, i, "invalid literal")
  else
    local s, e = str:find("^%-?%d+%.?%d*[eE]?[%+%-]?%d*", i)
    if not s then decode_error(str, i, "unexpected char '" .. c .. "'") end
    return tonumber(str:sub(s, e)), e + 1
  end
end

function json.decode(str)
  local v = decode_value(str, 1)
  return v
end

return json
