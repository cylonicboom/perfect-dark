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

-- Archipelago testing harness: check registry + bonus/item tools (no server
-- needed). Console: /lua ap.list()  |  pause menu -> Lua Director. See
-- docs/archipelago_blueprint.md.
load("scripts/ap/test.lua")

-- Archipelago ONLINE client: the WebSocket transport bridge (pd.ap_*) + AP
-- protocol. Console: /lua ap.connect("ws://127.0.0.1:38281"). Validate against
-- tools/ap/mock_ws.py. Inert until you call ap.connect.
load("scripts/ap/client.lua")

-- Chaos / randomiser mode: timed random effects, external event ingress
-- (/chaos console command + localhost UDP via Chaos.EventPort — the Twitch /
-- YouTube window), vote mode, and chaos.trigger()/chaos.set_seed() for the AP
-- client (trap items / DeathLink). Inert until "/chaos on". See
-- docs/PORT_CHAOS.md.
load("scripts/chaos.lua")

-- Silo Countdown test harness: /lua silo_test() fires the Silo Countdown chaos
-- effect with a 1-minute timer (instead of 8) so it can be tested quickly. Must
-- load AFTER chaos.lua. See scripts/silo_test.lua.
load("scripts/silo_test.lua")

-- Live octree-culling stats overlay (testing the outdoor-room octree). Shows
-- top-right only while an octree room is rendering. Comment out to hide.
load("scripts/octree_overlay.lua")

-- Render-time (/fps) + memory (/mem) overlays, bottom-left. Off until toggled.
load("scripts/perf_overlay.lua")

-- Live display-list cache stats overlay (testing the GPU-resident cache). Shows
-- top-right only while /dlcache is on. Comment out to hide.
load("scripts/dlcache_overlay.lua")
