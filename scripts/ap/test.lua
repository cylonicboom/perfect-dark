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

-- Seed the mission-completion checks (stage x difficulty) so ap.list() shows the
-- full board even before anything is completed.
for s = 0, 20 do
  for d = 0, 2 do
    add_check("mission:" .. STAGES[s] .. "/" .. DIFFS[d])
  end
end

local function mark(name, source)
  add_check(name)
  if not ap.checks[name] then
    ap.checks[name] = true
    pd.log(string.format("AP CHECK: %s  (%s)", name, source or "manual"))
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

function ap.heal()       if need("player_heal")       then pd.player_heal();        pd.log("AP bonus: full HP") end end
function ap.shield()     if need("player_set_shield") then pd.player_set_shield(1);  pd.log("AP bonus: full shield") end end
function ap.ammo()       if need("refill_ammo")       then pd.refill_ammo();         pd.log("AP bonus: ammo refilled") end end
function ap.invincible() if need("invincible")        then pd.invincible(true);      pd.log("AP bonus: invincible (call ap.mortal() to clear)") end end
function ap.mortal()     if need("invincible")        then pd.invincible(false);     pd.log("AP: invincibility off") end end

-- WEAPON_GRENADE 0x1e, AMMOTYPE_GRENADE 0x07, WEAPON_CLOAKINGDEVICE 0x31.
function ap.grenade()    if need("give_ammo")  then pd.give_ammo(0x07, 1); pd.log("AP bonus: grenade") end end
function ap.cloak()
  if need("give_ammo") and need("device_on") then
    pd.give_ammo(0x14, 60 * 30)           -- AMMOTYPE_CLOAK, ~30s
    pd.device_on(0x31)                    -- WEAPON_CLOAKINGDEVICE
    pd.log("AP bonus: cloak engaged")
  end
end
function ap.weapon(n)    if need("give_weapon") then pd.give_weapon(n); pd.log("AP bonus: weapon " .. tostring(n)) end end

-- ap.buddy(): spawn a friendly Perfect Buddy that fights for you.
function ap.buddy()
  if not need("spawn_ally") then return end
  local chrnum = pd.spawn_ally()
  if chrnum then
    pd.log("AP bonus: Perfect Buddy spawned (chr " .. chrnum .. ")")
  else
    pd.log("AP: buddy spawn failed (no live player? net client?)")
  end
end

-- ---------------------------------------------------------------------------
-- Pause-menu buttons (Lua Director)
-- ---------------------------------------------------------------------------

if type(pd.menu_add) == "function" then
  pd.menu_add("AP: Complete Next Check", ap.next)
  pd.menu_add("AP: List Checks (console)", ap.list)
  pd.menu_add("AP Bonus: Full HP", ap.heal)
  pd.menu_add("AP Bonus: Full Shield", ap.shield)
  pd.menu_add("AP Bonus: Refill Ammo", ap.ammo)
  pd.menu_add("AP Bonus: Grenade", ap.grenade)
  pd.menu_add("AP Bonus: Cloak", ap.cloak)
  pd.menu_add("AP Bonus: Spawn Perfect Buddy", ap.buddy)
end

pd.log("AP test harness loaded: /lua ap.list()  |  pause menu -> Lua Director")
