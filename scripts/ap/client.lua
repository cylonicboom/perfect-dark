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

local net = { status = "disconnected", connected = false }
ap.net = net

-- Stage index -> name (mirror of the harness map; kept local so client.lua is
-- self-contained).
local STAGES = {
  [0]="Defection",[1]="Investigation",[2]="Extraction",[3]="Villa",[4]="Chicago",
  [5]="G5Building",[6]="Infiltration",[7]="Rescue",[8]="Escape",[9]="AirBase",
}
local DIFFS = { [0]="Agent",[1]="SpecialAgent",[2]="PerfectAgent" }

-- ---- STATIC id maps (mock / MVP) -------------------------------------------
-- AP item id -> {gate category, gate id}.  pd.unlock accepts the category as a
-- string ("stage"/"difficulty"/"weapon_pri"/"weapon_sec"/"device"/"feature").
local AP_ITEM_TO_GATE = {
  [1000] = {"stage", 0},      [1001] = {"stage", 1},
  [1003] = {"stage", 3},      [1007] = {"stage", 7},
  [2001] = {"difficulty", 1}, [2002] = {"difficulty", 2},
  [3045] = {"device", 45},    -- Night Vision
}

-- Our check name -> AP location id.
local AP_LOCATION = {
  ["mission:Defection/Agent"]  = 5000,
  ["mission:Villa/Agent"]      = 5001,
}

-- ---- low-level send --------------------------------------------------------
-- An AP message is a JSON ARRAY of command objects.
local function send_cmd(cmd)
  if not pd.ap_send then return false end
  return pd.ap_send(json.encode({ cmd }))
end

-- ---- inbound handlers ------------------------------------------------------
local handlers = {}

handlers.RoomInfo = function(_msg)
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
  pd.log("ap: RoomInfo -> sent Connect as '" .. ap.slot_name .. "'")
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
    local g = AP_ITEM_TO_GATE[it.item]
    if g and pd.unlock then
      pd.unlock(g[1], g[2])
      n = n + 1
      pd.log(string.format("ap: item %d -> unlock %s %d", it.item, g[1], g[2]))
    else
      pd.log("ap: item " .. tostring(it.item) .. " (no gate mapping)")
    end
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
handlers.DataPackage   = function(_m) end
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
local function report_location(name)
  local id = AP_LOCATION[name]
  if id and net.connected then
    send_cmd({ cmd = "LocationChecks", locations = { id } })
    pd.log("ap: check '" .. name .. "' -> LocationChecks " .. id)
  end
end

pd.on("missioncomplete", function(stageindex, difficulty)
  local s = STAGES[stageindex] or ("stage" .. stageindex)
  local d = DIFFS[difficulty] or ("diff" .. difficulty)
  report_location("mission:" .. s .. "/" .. d)
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

pd.log("ap: client.lua loaded (ap.connect / ap.disconnect / ap.netstatus)")
