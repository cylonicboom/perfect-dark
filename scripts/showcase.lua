-- Perfect Dark - Lua scripting showcase
--
-- Proves the action-block -> Lua -> execute pipeline plus the event + draw API.
-- Loaded by scripts/init.lua on startup, on each stage load, and on /lua reload.
--
-- Coordinates are the lo-res virtual screen (~320x240, like the console).
-- Colours are 0xRRGGBBAA.

local KILLS = 0

local C_WHITE  = 0xffffffff
local C_GREEN  = 0x40ff40ff
local C_RED    = 0xff4040ff
local C_YELLOW = 0xffe040ff
local C_CYAN   = 0x40e0ffff

pd.log("showcase.lua loaded")

-- 1) Weapon fire -> a labelled box flashes for ~1.5s.
--    Change the `weaponnum ==` test to react to one specific weapon only.
pd.on("weaponfire", function(weaponnum, playernum)
  pd.draw_text(30, 28, string.format("FIRE  weapon=%d  player=%d", weaponnum, playernum), C_YELLOW, 1.5)
  pd.draw_box(30, 38, 92, 6, C_YELLOW, 1.5)
end)

-- 2) Enemy reacts to the player through its action block.
pd.on("alert", function(chrnum)
  pd.draw_text(108, 50, string.format("! ENEMY %d ALERTED", chrnum), C_RED, 2.5)
end)

-- 3) Damage -> brief flash showing who was hurt and by how much.
pd.on("damage", function(chrnum, attacker, amount)
  pd.draw_text(108, 60, string.format("HIT chr %d  -%d  (by p%d)", chrnum, amount, attacker), C_YELLOW, 1.0)
end)

-- 4) Spawn -> note when a chr is created (reinforcements / mid-level spawns).
pd.on("spawn", function(chrnum)
  pd.draw_text(108, 40, string.format("SPAWN chr %d", chrnum), C_GREEN, 1.5)
end)

-- 5) Kill -> running counter + a marker flash.
local WEAPON_PROXIMITYMINE = 0x21  -- a small object that sits on the ground

pd.on("kill", function(chrnum, killer)
  KILLS = KILLS + 1
  pd.draw_text(118, 70, string.format("KILL #%d  (chr %d)", KILLS, chrnum), C_CYAN, 2.0)
  pd.draw_box(150, 80, 18, 18, C_CYAN, 1.0)
  -- Spawn a real 3D object in the world where the enemy died (server-side).
  -- chrnum is still valid here -- chrDie drops the chr's own loot at this point.
  pd.spawn_at_chr(chrnum, WEAPON_PROXIMITYMINE)
end)

-- 4) AI X-RAY: each frame, label every live enemy with the exact ailist id and
--    command offset it is running THROUGH the Lua exec loop. This is the proof
--    that action blocks are transpiled to Lua and executed every tick.
pd.on("draw", function()
  pd.draw_text(8, 10, "-- AI X-RAY (ailist @ offset) --", C_GREEN)
  local y = 20
  pd.each_chr(function(chrnum, ailistid, off, alertness, islua)
    if y > 222 then return end
    local tag = (islua ~= 0) and "lua" or "bc"
    local col = (alertness > 0) and C_RED or C_GREEN
    pd.draw_text(8, y, string.format("chr %-3d ailist 0x%04x @%-3d %s", chrnum, ailistid, off, tag), col)
    y = y + 8
  end)
end)

-- 5) (Optional) Author an enemy's behaviour entirely in Lua. Set an ailist id
--    that actually runs in your test level, then uncomment. When that list runs,
--    this Lua function drives it instead of the original bytecode.
--
-- pd.register_ailist(0x0001, function(ctx)
--   pd.log("custom Lua ailist running")
--   return 1  -- 1 = yield this frame, 0 = list finished, 2 = list switched
-- end)
