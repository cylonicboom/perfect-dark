-- Archipelago testing harness (no server required).
--
-- This is a *local simulator* for the Archipelago integration: it listens to the
-- new check-detection events the engine now emits, keeps a check registry, and
-- exposes tools to "complete" checks and apply "bonus" items by hand -- so the
-- whole flow can be exercised in-game before the real AP socket exists.
--
-- Two interfaces:
--   * Console (open with ~):  /lua ap.list()   /lua ap.complete("Defection/Agent")
--                             /lua ap.heal()    /lua ap.buddy()   etc.
--   * Pause menu -> "Lua Director": ready-made buttons for the bonuses + checks.
--
-- Loaded from scripts/init.lua. Safe to leave in; it only logs + offers tools.

local ap = {}
_G.ap = ap  -- expose to the /lua console

-- ---------------------------------------------------------------------------
-- Check registry
-- ---------------------------------------------------------------------------

-- SOLOSTAGEINDEX order (constants.h). 0..20.
local STAGES = {
  [0]="Defection", [1]="Investigation", [2]="Extraction", [3]="Villa",
  [4]="Chicago", [5]="G5Building", [6]="Infiltration", [7]="Rescue",
  [8]="Escape", [9]="AirBase", [10]="AirForceOne", [11]="CrashSite",
  [12]="Pelagic", [13]="DeepSea", [14]="Defense", [15]="AttackShip",
  [16]="SkedarRuins", [17]="MBR", [18]="MaianSOS", [19]="WAR", [20]="Duel",
}
local DIFFS = { [0]="Agent", [1]="SpecialAgent", [2]="PerfectAgent" }

-- checks[name] = false (pending) / true (done). Built up as events fire or as
-- ap.complete() is called.
ap.checks = {}
local order = {}  -- stable iteration order for ap.list()

local function add_check(name)
  if ap.checks[name] == nil then
    ap.checks[name] = false
    order[#order + 1] = name
  end
end

-- Session persistence. The engine destroys the whole Lua state on every stage
-- load (mission start / return to menu), so completed checks would reset. We
-- mirror the completed set into the engine's C-side persist store
-- (pd.persist_set/get, which outlives the state teardown) and restore on reload.
local PERSIST_KEY = "ap.checks"

local function save_state()
  if type(pd.persist_set) ~= "function" then return end
  local done = {}
  for _, n in ipairs(order) do
    if ap.checks[n] then done[#done + 1] = n end
  end
  pd.persist_set(PERSIST_KEY, table.concat(done, "\n"))
end

-- Seed the mission-completion checks (stage x difficulty) so ap.list() shows the
-- full board even before anything is completed.
for s = 0, 20 do
  for d = 0, 2 do
    add_check("mission:" .. STAGES[s] .. "/" .. DIFFS[d])
  end
end

-- Restore previously-completed checks across the stage-load Lua-state reset.
-- (Also re-creates runtime checks like firingrange/weaponfound that were earned.)
if type(pd.persist_get) == "function" then
  local saved = pd.persist_get(PERSIST_KEY)
  if saved and saved ~= "" then
    local n = 0
    for name in saved:gmatch("[^\n]+") do
      add_check(name)
      ap.checks[name] = true
      n = n + 1
    end
    pd.log(string.format("AP: restored %d completed check(s)", n))
  end
end

local function mark(name, source)
  add_check(name)
  if not ap.checks[name] then
    ap.checks[name] = true
    pd.log(string.format("AP CHECK: %s  (%s)", name, source or "manual"))
    -- Big centred banner, the same path the engine uses for "Objective
    -- Complete" (no-op off-mission; silently skipped on a build without it).
    if type(pd.hud_message) == "function" then
      pd.hud_message("AP Check: " .. name)
    end
    save_state()
  else
    pd.log("AP CHECK (already done): " .. name)
  end
end

-- ---------------------------------------------------------------------------
-- Engine check-detection events -> registry
-- ---------------------------------------------------------------------------

pd.on("missioncomplete", function(stageindex, difficulty, secs, cheated)
  local s = STAGES[stageindex] or ("stage" .. stageindex)
  local d = DIFFS[difficulty] or ("diff" .. difficulty)
  mark("mission:" .. s .. "/" .. d,
       string.format("%ds%s", secs, cheated ~= 0 and " CHEATED" or ""))
end)

pd.on("firingrange", function(weaponindex, medal)
  local m = ({[1]="Bronze",[2]="Silver",[3]="Gold"})[medal] or ("medal" .. medal)
  mark(string.format("firingrange:w%d/%s", weaponindex, m), "firing range")
end)

pd.on("weaponfound", function(weaponnum)
  mark(string.format("weaponfound:w%d", weaponnum), "first found")
end)

-- Per-objective completion. Keyed by stage + difficulty + objective index so it
-- is unique across stages (each stage indexes its objectives from 0). These are
-- added to the board on the fly as they fire (the full per-stage objective
-- catalog isn't seeded -- the real AP data tables will carry that).
pd.on("objective", function(stageindex, difficulty, objindex, status)
  local s = STAGES[stageindex] or ("stage" .. stageindex)
  local d = DIFFS[difficulty] or ("diff" .. difficulty)
  mark(string.format("objective:%s/%s/%d", s, d, objindex), "objective complete")
end)

-- Cheat unlock (a timed/completion cheat's condition was newly met, cheats-off).
pd.on("cheatunlock", function(cheatid)
  mark(string.format("cheat:%d", cheatid), "cheat unlocked")
end)

-- Combat-Sim challenge completed (at the player count it was beaten with).
pd.on("challengecomplete", function(index, numplayers)
  mark(string.format("challenge:%d/%dp", index, numplayers), "challenge complete")
end)

-- ---------------------------------------------------------------------------
-- Check tools (console)
-- ---------------------------------------------------------------------------

-- ap.list(): print every check + its state, and a done/total tally.
function ap.list()
  local done = 0
  for _, name in ipairs(order) do
    local d = ap.checks[name]
    if d then done = done + 1 end
    pd.log(string.format("[%s] %s", d and "x" or " ", name))
  end
  pd.log(string.format("AP: %d / %d checks complete", done, #order))
  return done, #order
end

-- ap.complete(name): manually flag a check (substring match for convenience).
function ap.complete(name)
  if name == nil then
    pd.log("usage: ap.complete(\"mission:Defection/Agent\")")
    return
  end
  if ap.checks[name] ~= nil then
    mark(name, "forced")
    return
  end
  -- substring fallback: complete the first pending match
  for _, n in ipairs(order) do
    if not ap.checks[n] and n:find(name, 1, true) then
      mark(n, "forced (matched)")
      return
    end
  end
  pd.log("AP: no pending check matching '" .. tostring(name) .. "'")
end

-- ap.next(): complete the next pending check (handy for rapid testing).
function ap.next()
  for _, n in ipairs(order) do
    if not ap.checks[n] then mark(n, "forced (next)"); return n end
  end
  pd.log("AP: all checks already complete")
end

function ap.reset()
  for n in pairs(ap.checks) do ap.checks[n] = false end
  save_state()
  pd.log("AP: all checks reset to pending")
end

function ap.status()
  local done = 0
  for _, n in ipairs(order) do if ap.checks[n] then done = done + 1 end end
  pd.log(string.format("AP: %d / %d checks complete", done, #order))
  return done, #order
end

-- ---------------------------------------------------------------------------
-- Bonus / item tools (received-side boosts). Each guards on the pd.* existing so
-- the script still loads on an engine build without the new bridges.
-- ---------------------------------------------------------------------------

local function need(fnname)
  if type(pd[fnname]) ~= "function" then
    pd.log("AP: pd." .. fnname .. " not available in this build")
    return false
  end
  return true
end

-- Announce a received item/bonus: console log + the same big centred banner the
-- checks use (no-op off-mission). This is where we want incoming-item toasts.
local function notify(label)
  pd.log("AP item: " .. label)
  if type(pd.hud_message) == "function" then
    pd.hud_message("AP Item: " .. label)
  end
end

function ap.heal()       if need("player_heal")       then pd.player_heal();       notify("Full HP") end end
function ap.shield()     if need("player_set_shield") then pd.player_set_shield(1); notify("Full Shield") end end
function ap.ammo()       if need("refill_ammo")       then pd.refill_ammo();        notify("Refill Ammo") end end
function ap.invincible() if need("invincible")        then pd.invincible(true);     notify("Invincibility") end end
function ap.mortal()     if need("invincible")        then pd.invincible(false);    pd.log("AP: invincibility off") end end

-- WEAPON_GRENADE 0x1e, AMMOTYPE_GRENADE 0x07, WEAPON_CLOAKINGDEVICE 0x31.
function ap.grenade()    if need("give_ammo")  then pd.give_ammo(0x07, 1); notify("Grenade") end end
function ap.cloak()
  if need("give_ammo") and need("device_on") then
    pd.give_ammo(0x14, 60 * 30)           -- AMMOTYPE_CLOAK, ~30s
    pd.device_on(0x31)                    -- WEAPON_CLOAKINGDEVICE
    notify("Cloak")
  end
end
function ap.weapon(n)    if need("give_weapon") then pd.give_weapon(n); notify("Weapon " .. tostring(n)) end end

-- ap.buddy(): spawn a friendly Perfect Buddy that fights for you.
function ap.buddy()
  if not need("spawn_ally") then return end
  local chrnum = pd.spawn_ally()
  if chrnum then
    notify("Perfect Buddy")
  else
    pd.log("AP: buddy spawn failed (no live player? net client?)")
  end
end

-- ---------------------------------------------------------------------------
-- Gating tools (simulate "content locked until the item is received").
-- The engine gate points (stage/difficulty access, weapon primary/secondary
-- functions, gadgets/devices) consult the unlock set when pd.ap_mode is on.
-- Categories: "stage" (0-20) | "difficulty" (0=Agent,1=SA,2=PA) |
-- "weapon_pri"/"weapon_sec"/"device" (keyed by weaponnum). All persist across
-- stage loads; cleared by ap.gate(true) or a game restart.
-- ---------------------------------------------------------------------------

-- ap.gate([on]): enable/disable AP gating. On enable, lock everything, then
-- grant Agent difficulty + Defection as the "starting items" so the run is
-- playable. Receive more with ap.give(); revoke with ap.take().
function ap.gate(on)
  if type(pd.ap_mode) ~= "function" then
    pd.log("AP: gating not in this build (rebuild needed)")
    return
  end
  if on == nil then on = not pd.ap_mode() end
  pd.ap_mode(on)
  if on then
    pd.ap_reset()
    pd.unlock("difficulty", 0) -- Agent, the always-available starting difficulty
    pd.unlock("stage", 0)      -- Defection, the canonical first mission
    pd.log("AP gating ON -- all stages/difficulties/weapon-functions/devices locked.")
    pd.log("  starting items granted: Agent + Defection.")
    pd.log("  receive: /lua ap.give('stage',N) | ('difficulty',1|2) | ('weapon_pri'|'weapon_sec'|'device',weaponnum)")
  else
    pd.log("AP gating OFF -- vanilla unlock rules.")
  end
  return on
end

function ap.give(cat, id)
  if type(pd.unlock) == "function" then
    pd.unlock(cat, id)
    notify(string.format("Unlock %s %d", tostring(cat), id))
  end
end

function ap.take(cat, id)
  if type(pd.lock) == "function" then
    pd.lock(cat, id)
    pd.log(string.format("AP: locked %s %d", tostring(cat), id))
  end
end

-- ap.gstatus(): print the current gate mode + which stages/difficulties are open.
function ap.gstatus()
  if type(pd.is_unlocked) ~= "function" then pd.log("AP: gating not in this build"); return end
  pd.log("AP gating: " .. (pd.ap_mode() and "ON" or "off"))
  local diffs = {}
  for d = 0, 2 do if pd.is_unlocked("difficulty", d) then diffs[#diffs + 1] = DIFFS[d] end end
  pd.log("  difficulties: " .. (#diffs > 0 and table.concat(diffs, ", ") or "(none)"))
  local st = {}
  for s = 0, 20 do if pd.is_unlocked("stage", s) then st[#st + 1] = STAGES[s] end end
  pd.log("  stages: " .. (#st > 0 and table.concat(st, ", ") or "(none)"))
end

-- ---------------------------------------------------------------------------
-- Pause-menu buttons (Lua Director)
-- ---------------------------------------------------------------------------

-- ---------------------------------------------------------------------------
-- HUD overlay: live "done / total" counter. Drawn via the "draw" event (fires
-- once per frame while the HUD renders, so it shows in-mission). Solo-campaign
-- only in practice, so top-left is clear of the Combat-Sim kill feed. Colours
-- are 0xRRGGBBAA. Toggle with /lua ap.hud(). The per-completion notification is
-- the big centred banner popped from mark() via pd.hud_message (like the
-- engine's "Objective Complete").
-- ---------------------------------------------------------------------------

ap.hud_enabled = true   -- master toggle
ap.hud_x       = 8      -- counter top-left anchor (move to taste)
ap.hud_y       = 8

local C_COUNT  = 0x40ff40ff  -- green "AP d/t"
local C_DONE   = 0xffd040ff  -- gold once everything is complete
local C_SHADOW = 0x000000c0  -- 1px drop shadow for readability over bright scenes

-- ap.hud([on]): toggle (no arg) or set the overlay on/off.
function ap.hud(on)
  if on == nil then on = not ap.hud_enabled end
  ap.hud_enabled = on and true or false
  pd.log("AP HUD: " .. (ap.hud_enabled and "on" or "off"))
  return ap.hud_enabled
end

pd.on("draw", function()
  if not ap.hud_enabled then return end

  -- Live counter (recomputed each frame so it tracks resets/forces too).
  local done = 0
  for _, n in ipairs(order) do if ap.checks[n] then done = done + 1 end end
  local total = #order
  local col = (total > 0 and done >= total) and C_DONE or C_COUNT
  local txt = string.format("AP %d/%d", done, total)
  pd.draw_text(ap.hud_x + 1, ap.hud_y + 1, txt, C_SHADOW)
  pd.draw_text(ap.hud_x,     ap.hud_y,     txt, col)
end)

if type(pd.menu_add) == "function" then
  pd.menu_add("AP: Complete Next Check", ap.next)
  pd.menu_add("AP: List Checks (console)", ap.list)
  pd.menu_add("AP: Toggle HUD Counter", ap.hud)
  pd.menu_add("AP Gate: Toggle Locking", ap.gate)
  pd.menu_add("AP Gate: Status (console)", ap.gstatus)
  pd.menu_add("AP Bonus: Full HP", ap.heal)
  pd.menu_add("AP Bonus: Full Shield", ap.shield)
  pd.menu_add("AP Bonus: Refill Ammo", ap.ammo)
  pd.menu_add("AP Bonus: Grenade", ap.grenade)
  pd.menu_add("AP Bonus: Cloak", ap.cloak)
  pd.menu_add("AP Bonus: Spawn Perfect Buddy", ap.buddy)
end

pd.log("AP test harness loaded: /lua ap.list()  |  HUD counter on (/lua ap.hud())  |  pause menu -> Lua Director")
