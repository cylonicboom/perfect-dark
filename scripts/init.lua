-- Entry point. The engine runs scripts/init.lua automatically on startup, on
-- each stage load, and whenever you type `/lua reload` in the ~ console.
--
-- Keep your own scripting here, or require/dofile other files. Each load is
-- wrapped in pcall so a broken script logs an error but doesn't stop the others.

local function load(path)
  local ok, err = pcall(dofile, path)
  if not ok then
    pd.log("init.lua: failed to load " .. path .. ": " .. tostring(err))
  end
end

-- The showcase: weapon-fire / alert / kill events + the live AI X-ray overlay.
load("scripts/showcase.lua")

-- The worked example: replaces one enemy's action block with hand-written Lua.
-- Open the ~ console, read an ailist id off the X-ray overlay, set TARGET_AILIST
-- at the top of this file, then `/lua reload`.
load("scripts/examples/lua_authored_enemy.lua")

-- The mission-director toolkit: registers a "Lua Director" pause-menu panel of
-- live actions + scenarios. Hack on scripts/director.lua to add your own.
load("scripts/director.lua")

-- Live octree-culling stats overlay (testing the outdoor-room octree). Shows
-- top-right only while an octree room is rendering. Comment out to hide.
load("scripts/octree_overlay.lua")
