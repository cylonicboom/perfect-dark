-- Archipelago client (online). Drives the C transport bridge (pd.ap_connect /
-- ap_status / ap_send / ap_poll) and speaks the AP protocol: handshake, receive
-- items -> pd.unlock (the gate layer), report checks -> LocationChecks.
--
-- P1 (MVP): validated against tools/ap/mock_ws.py over ws://. The AP id <-> gate
-- id maps below are STATIC stubs for the mock; the real values come from the
-- Perfect Dark apworld / the DataPackage (P2). See docs/archipelago_blueprint.md
-- §6.5.
--
-- Console:  /lua ap.connect("ws://127.0.0.1:38281")  |  ap.disconnect()  |
--           ap.netstatus()

local json = dofile("scripts/ap/json.lua")

ap = ap or {}

ap.slot_name = ap.slot_name or "Player1"
ap.game_name = ap.game_name or "Perfect Dark"
ap.password  = ap.password  or ""

-- net.item_id_to_name / loc_name_to_id come from the DataPackage (resolved by
-- NAME, so engine-side ids are never hard-coded). pending_items buffers item ids
-- that arrive before the DataPackage does.
local net = {
  status = "disconnected", connected = false,
  have_dp = false, item_id_to_name = {}, loc_name_to_id = {}, pending = {},
}
ap.net = net

-- ---- name -> engine gate (the stable contract with the apworld) ------------
-- Display names here MUST match tools/ap/apworld/perfect_dark/data.py.
local STAGE_NAME_TO_INDEX = {
  ["Defection"]=0, ["Investigation"]=1, ["Extraction"]=2, ["Villa"]=3,
  ["Chicago"]=4, ["G5 Building"]=5, ["Infiltration"]=6, ["Rescue"]=7,
  ["Escape"]=8, ["Air Base"]=9,
}
local DIFF_NAME_TO_INDEX = { ["Special Agent"]=1, ["Perfect Agent"]=2 }
local DEVICE_NAME_TO_WEAPON = {
  ["Night Vision"]=45, ["IR Scanner"]=48, ["X-Ray Scanner"]=47,
  ["Cloaking Device"]=49,
}
-- "Weapon: X" -> engine weaponnum (fire gate). MUST match WEAPON_NAME_TO_NUM in
-- tools/ap/apworld/perfect_dark/data.py. Gadgets (Eye Spy / Door Decoder / Data
-- Uplink / AutoSurgeon / Suitcase) deploy through this same fire gate.
local WEAPON_NAME_TO_NUM = {
  ["Falcon 2"]=2, ["MagSec 4"]=5, ["AR34"]=17, ["Shotgun"]=19,
  ["Rocket Launcher"]=24, ["Grenade"]=30, ["Sniper Rifle"]=21,
  ["ECM Mine"]=53, ["Data Uplink"]=54, ["Eye Spy"]=46, ["Door Decoder"]=57,
  ["Remote Mine"]=34, ["Tracer Bug"]=62, ["Comms Rider"]=61,
  ["AutoSurgeon"]=58, ["Suitcase"]=77,
}

-- Engine stage index / difficulty -> apworld display name (for location lookup).
local STAGE_DISP = {}
for k, v in pairs(STAGE_NAME_TO_INDEX) do STAGE_DISP[v] = k end
local DIFF_DISP = { [0]="Agent", [1]="Special Agent", [2]="Perfect Agent" }

-- weaponnum -> display name, for reporting "Firing Range: <name>" checks. Only
-- weapons with a range location resolve to a check (others no-op in lookup).
local NUM_TO_WEAPON_NAME = {}
for name, num in pairs(WEAPON_NAME_TO_NUM) do NUM_TO_WEAPON_NAME[num] = name end

-- Map an AP item NAME to an engine gate {category, id}, or nil for filler.
local function name_to_gate(name)
  local s = name:match("^Stage: (.+)$")
  if s and STAGE_NAME_TO_INDEX[s] then return { "stage", STAGE_NAME_TO_INDEX[s] } end
  local d = name:match("^Difficulty: (.+)$")
  if d and DIFF_NAME_TO_INDEX[d] then return { "difficulty", DIFF_NAME_TO_INDEX[d] } end
  local dev = name:match("^Device: (.+)$")
  if dev and DEVICE_NAME_TO_WEAPON[dev] then return { "device", DEVICE_NAME_TO_WEAPON[dev] } end
  local w = name:match("^Weapon: (.+)$")
  if w and WEAPON_NAME_TO_NUM[w] then return { "weapon", WEAPON_NAME_TO_NUM[w] } end
  return nil
end

-- ---- low-level send --------------------------------------------------------
-- An AP message is a JSON ARRAY of command objects.
local function send_cmd(cmd)
  if not pd.ap_send then return false end
  return pd.ap_send(json.encode({ cmd }))
end

-- ---- inbound handlers ------------------------------------------------------
local handlers = {}

-- Apply one received item id (resolve id -> name -> gate). Returns true if it
-- unlocked a gate. Items that arrive before the DataPackage are buffered.
local function apply_item(item_id)
  local name = net.item_id_to_name[item_id]
  if not name then
    net.pending[#net.pending + 1] = item_id
    return false
  end
  local g = name_to_gate(name)
  if g and pd.unlock then
    if g[1] == "weapon" then
      -- One weapon item unlocks BOTH fire functions (primary + secondary).
      pd.unlock("weapon_pri", g[2])
      pd.unlock("weapon_sec", g[2])
      pd.log(string.format("ap: item '%s' -> unlock weapon %d (pri+sec)", name, g[2]))
    else
      pd.unlock(g[1], g[2])
      pd.log(string.format("ap: item '%s' -> unlock %s %d", name, g[1], g[2]))
    end
    return true
  end
  pd.log("ap: item '" .. name .. "' (filler / no gate)")
  return false
end

local function drain_pending()
  local ids = net.pending
  net.pending = {}
  local n = 0
  for _, id in ipairs(ids) do
    if apply_item(id) then n = n + 1 end
  end
  if n > 0 and ap.refresh_header then ap.refresh_header() end
end

handlers.RoomInfo = function(_msg)
  -- Fetch the id<->name tables first so items resolve, then connect.
  send_cmd({ cmd = "GetDataPackage", games = { ap.game_name } })
  send_cmd({
    cmd = "Connect",
    game = ap.game_name,
    name = ap.slot_name,
    password = ap.password,
    uuid = "pd-" .. ap.slot_name,
    version = { major = 0, minor = 5, build = 0, class = "Version" },
    items_handling = 7,   -- receive others' + own + starting-inventory items
    tags = {},
    slot_data = false,
  })
  pd.log("ap: RoomInfo -> GetDataPackage + Connect as '" .. ap.slot_name .. "'")
end

handlers.DataPackage = function(msg)
  local games = msg.data and msg.data.games
  local g = games and games[ap.game_name]
  if not g then return end
  net.item_id_to_name = {}
  for name, id in pairs(g.item_name_to_id or {}) do
    net.item_id_to_name[id] = name
  end
  net.loc_name_to_id = g.location_name_to_id or {}
  net.have_dp = true
  pd.log("ap: DataPackage loaded")
  drain_pending()
end

handlers.Connected = function(msg)
  net.connected = true
  pd.log("ap: Connected (slot " .. tostring(msg.slot) .. ")")
  -- Make sure gating is on so received items actually unlock content.
  if pd.ap_mode and not pd.ap_mode() then
    pd.ap_mode(true)
  end
end

handlers.ConnectionRefused = function(msg)
  pd.log("ap: ConnectionRefused: " .. table.concat(msg.errors or { "?" }, ", "))
end

handlers.ReceivedItems = function(msg)
  local n = 0
  for _, it in ipairs(msg.items or {}) do
    if apply_item(it.item) then n = n + 1 end
  end
  if n > 0 and ap.refresh_header then ap.refresh_header() end
end

handlers.PrintJSON = function(msg)
  -- Surface plain text lines (joins the "data" segments).
  if msg.data then
    local parts = {}
    for _, seg in ipairs(msg.data) do parts[#parts + 1] = seg.text or "" end
    pd.log("ap> " .. table.concat(parts))
  end
end

handlers.RoomUpdate    = function(_m) end
handlers.Retrieved     = function(_m) end
handlers.SetReply      = function(_m) end
handlers.Bounced       = function(_m) end
handlers.InvalidPacket = function(msg)
  pd.log("ap: InvalidPacket: " .. tostring(msg.text))
end

local function on_message(text)
  local ok, arr = pcall(json.decode, text)
  if not ok then
    pd.log("ap: bad json (" .. tostring(arr) .. ")")
    return
  end
  if type(arr) ~= "table" then return end
  for _, m in ipairs(arr) do
    local h = m.cmd and handlers[m.cmd]
    if h then
      local hok, herr = pcall(h, m)
      if not hok then pd.log("ap: handler " .. tostring(m.cmd) .. " error: " .. tostring(herr)) end
    else
      pd.log("ap: unhandled cmd " .. tostring(m.cmd))
    end
  end
end

-- ---- check reporting -------------------------------------------------------
-- Report an apworld location by its display name (resolved to an id via the
-- DataPackage). No-op until connected + DataPackage loaded.
local function report_location_name(locname)
  local id = net.loc_name_to_id[locname]
  if id and net.connected then
    send_cmd({ cmd = "LocationChecks", locations = { id } })
    pd.log("ap: check '" .. locname .. "' -> LocationChecks " .. id)
  end
end

pd.on("missioncomplete", function(stageindex, difficulty)
  local s = STAGE_DISP[stageindex]
  local d = DIFF_DISP[difficulty]
  if s and d then report_location_name(s .. " (" .. d .. ")") end
end)

-- Firing-range weapon found/fired -> "Firing Range: <name>" check (no-op unless
-- that weapon has a range location in the DataPackage).
pd.on("weaponfound", function(weaponnum)
  local name = NUM_TO_WEAPON_NAME[weaponnum]
  if name then report_location_name("Firing Range: " .. name) end
end)

-- Combat Simulator challenge complete -> "Challenge N" check. Engine index is
-- 0-based; the apworld names them 1-based. No-op past the modelled count.
pd.on("challengecomplete", function(index)
  report_location_name("Challenge " .. (index + 1))
end)

-- ---- per-frame drain + status edge log ------------------------------------
pd.on("tick", function()
  if not pd.ap_status then return end
  local st = pd.ap_status()
  if st ~= net.status then
    net.status = st
    pd.log("ap: transport " .. st)
    if st ~= "connected" then net.connected = false end
  end
  if not pd.ap_poll then return end
  local m = pd.ap_poll()
  while m do
    on_message(m)
    m = pd.ap_poll()
  end
end)

-- ---- console / public API --------------------------------------------------
function ap.connect(url)
  if not pd.ap_connect then pd.log("ap: transport not in this build"); return false end
  url = url or "ws://127.0.0.1:38281"
  pd.log("ap: connecting to " .. url)
  return pd.ap_connect(url)
end

function ap.disconnect()
  if pd.ap_disconnect then pd.ap_disconnect() end
  net.connected = false
  pd.log("ap: disconnected")
end

function ap.netstatus()
  pd.log("ap: transport=" .. (pd.ap_status and pd.ap_status() or "?") ..
         " connected=" .. tostring(net.connected))
end

-- ---- resume across a Lua-state teardown ------------------------------------
-- The engine lua_close()s and re-dofile()s this script on every stage load
-- (luaai.c), so the `net` table above is fresh each time -- but the C transport
-- (pd.ap_*) survives. If we come back up already connected, the AP server won't
-- re-send RoomInfo, so re-fetch the DataPackage ourselves and restore the
-- connected flag; otherwise loc/item id maps stay empty and checks never report.
-- (Unlocks already survive teardown -- they live C-side in g_ApUnlocks.)
if pd.ap_status and pd.ap_status() == "connected" and pd.ap_send then
  net.connected = true
  net.status = "connected"
  send_cmd({ cmd = "GetDataPackage", games = { ap.game_name } })
  pd.log("ap: resumed live connection -> re-fetching DataPackage")
end

pd.log("ap: client.lua loaded (ap.connect / ap.disconnect / ap.netstatus)")
