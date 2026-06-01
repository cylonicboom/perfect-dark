-- ============================================================================
-- Mission Director -- a play-around toolkit, driven from the in-game pause menu.
--
-- Open the pause menu and choose "Lua Director" to get a panel of live actions
-- and scenario toggles. Every entry here is registered from Lua via
-- pd.menu_add(label, fn); the C side renders whatever we register and calls the
-- function back when selected. This file is meant to be HACKED ON -- add your
-- own effects and they show up in the menu automatically.
--
-- ---------------------------------------------------------------------------
-- HOW TO ADD YOUR OWN EFFECT
--   1. Write a function that does the thing (use the pd.* API below).
--   2. pd.menu_add("My Cool Effect", my_function)
--   3. /lua reload in the ~ console -> it appears in the pause menu.
--
-- Building blocks (all server-side; see docs/luascripting.md + aicommands.md):
--   pd.all_chrs(fn)            -- fn(chrnum) for EVERY actor
--   pd.chr_anim(chrnum, anim, [speed])  -- play an animation on a chr
--   pd.chr_set_shield(chrnum, v)
--   pd.chr_alert(chrnum)       -- put a chr on alert (hive-mind lever)
--   pd.spawn(weaponnum, x,y,z, [ref])   -- drop an object in the world
--   pd.spawn_at_chr(chrnum, weaponnum)
--   pd.player_pos([n]) / pd.chr_pos(n) / pd.chr_info(n) / pd.distance(...)
--   pd.on(event, fn) for weaponfire/alert/damage/kill/spawn/roomenter/draw
--   pd.draw_text / pd.draw_box  -- HUD overlays
-- ============================================================================

local C_WHITE = 0xffffffff
local C_GREEN = 0x40ff40ff
local C_CYAN  = 0x40e0ffff

-- Animation id used for the "sneeze" gag. Animation ids are generated per-ROM
-- (see src/assets/*/animations.json -> build/.../animations.h), so this is a
-- plain number you can tweak; pick whatever looks funny in your build.
local ANIM_GAG = 0x67

-- Some weapon ids for spawning (engine WEAPON_* enum, constants.h):
local WEAPON_FALCON2       = 0x02
local WEAPON_PROXIMITYMINE = 0x21

-- Body ids for the "Turn Everyone Into..." gag (BODY_* enum, constants.h).
-- pd.chr_set_body is SOLO/MISSIONS ONLY (no-op in Combat Sim) and skips the
-- player; it transforms NPC/guard actors. Try any BODY_* number here.
local BODY_SKEDAR   = 0x5c
local BODY_DRCAROLL = 0x6b
local BODY_MRBLONDE = 0x5b

-- Director state the event handlers below react to.
local D = { scenario = nil, waves = 0 }

-- ---------------------------------------------------------------------------
-- Panel actions
-- ---------------------------------------------------------------------------

local function spawn_wave()
  local x, y, z = pd.player_pos(0)
  if not x then return end
  -- ring of pickups around the player; swap WEAPON_FALCON2 for variety
  for i = 0, 5 do
    local a = (i / 6) * 6.28318
    pd.spawn(WEAPON_FALCON2, x + math.cos(a) * 200, y, z + math.sin(a) * 200)
  end
  D.waves = D.waves + 1
  pd.log("director: spawned wave " .. D.waves)
end

local function hive_mind()
  local n = 0
  pd.all_chrs(function(chrnum)
    pd.chr_alert(chrnum)
    n = n + 1
  end)
  pd.log("director: alerted " .. n .. " actors")
end

local function make_everyone_sneeze()
  -- The showcase of the toolkit framework: one animation on every actor.
  pd.all_chrs(function(chrnum) pd.chr_anim(chrnum, ANIM_GAG, 1.0) end)
end

local function shield_all()
  pd.all_chrs(function(chrnum) pd.chr_set_shield(chrnum, 8.0) end)
end

-- "Turn Everyone Into X" -- runtime body swap on every actor. Solo/missions
-- only; the player and (in Combat Sim) all actors are refused by the bridge, so
-- this transforms NPC/guard actors in single-player.
local function turn_everyone_into(body)
  local n = 0
  pd.all_chrs(function(chrnum)
    if pd.chr_set_body(chrnum, body) then n = n + 1 end
  end)
  pd.log("director: transformed " .. n .. " actors")
end

-- ---------------------------------------------------------------------------
-- Scenarios (picker) -- set D.scenario; the event handlers below act on it.
-- ---------------------------------------------------------------------------

local function scenario_last_stand()
  D.scenario, D.waves = "laststand", 0
  pd.log("director: scenario = Last Stand (kill enemies -> reinforcements)")
end

local function scenario_escort()
  D.scenario = "escort"
  pd.log("director: scenario = Escort")
end

local function scenario_off()
  D.scenario = nil
  pd.log("director: scenario off")
end

-- Last Stand: every kill drops a mine and (every 3rd) spawns a fresh wave.
pd.on("kill", function(chrnum)
  if D.scenario == "laststand" then
    pd.spawn_at_chr(chrnum, WEAPON_PROXIMITYMINE)
    D.waves = (D.waves or 0) + 1
    if D.waves % 3 == 0 then spawn_wave() end
  end
end)

-- Director HUD: show active scenario + wave count.
pd.on("draw", function()
  if D.scenario then
    pd.draw_text(8, 220, string.format("DIRECTOR: %s  waves=%d", D.scenario, D.waves or 0), C_CYAN)
  end
end)

-- ---------------------------------------------------------------------------
-- Register the menu. Clear first so /lua reload rebuilds cleanly.
-- ---------------------------------------------------------------------------

pd.menu_clear()
-- panel
pd.menu_add("Spawn Wave",           spawn_wave)
pd.menu_add("Hive Mind (alert all)", hive_mind)
pd.menu_add("Make Everyone Sneeze", make_everyone_sneeze)
pd.menu_add("Shield All",           shield_all)
-- "Turn Everyone Into..." (solo/missions only; no-op in Combat Sim)
pd.menu_add("Everyone -> Skedar",   function() turn_everyone_into(BODY_SKEDAR) end)
pd.menu_add("Everyone -> Dr Caroll", function() turn_everyone_into(BODY_DRCAROLL) end)
pd.menu_add("Everyone -> Mr Blonde", function() turn_everyone_into(BODY_MRBLONDE) end)
-- Controllable entity (solo/missions only): fly a cube around, START/ESC or
-- "Stop Possessing" to return to your body.
pd.menu_add("Become A Cube",        function() pd.possess_spawn() end)
pd.menu_add("Stop Possessing",      function() pd.unpossess() end)
-- scenario picker
pd.menu_add("Scenario: Last Stand", scenario_last_stand)
pd.menu_add("Scenario: Escort",     scenario_escort)
pd.menu_add("Scenario: Off",        scenario_off)

pd.log("director.lua loaded (" .. "pause menu -> Lua Director)")
