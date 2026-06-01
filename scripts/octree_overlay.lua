-- Octree culling — live on-screen stats for testing the outdoor-room octree.
--
-- Reads pd.octree_stats() every frame (the counters bg.c's bgRenderScene fills:
-- see docs/PORT_OCTREE.md) and draws them top-right. Only shows while the octree
-- is actually active this frame -- after `/octree mark` / `/octree markall`, or
-- in a room flagged ROOMFLAG_EX_OCTREE -- so it stays out of the way otherwise.
--
-- Unlike `/octree stats` (a one-shot console snapshot) this updates live, so you
-- can watch `drawn` rise and fall as geometry enters/leaves the view frustum.
--
-- Loaded by scripts/init.lua. Colours are 0xRRGGBBAA; coords are the lo-res
-- virtual screen (~320x240, like the console).

local C_WHITE = 0xffffffff
local C_GREEN = 0x40ff40ff
local C_RED   = 0xff5050ff
local C_CYAN  = 0x40e0ffff

pd.log("octree_overlay.lua loaded (/octree markall then /octree forcecull to test)")

pd.on("draw", function()
  local s = pd.octree_stats()
  if not s or s.passes == 0 then
    return -- no octree room rendered this frame; nothing to show
  end

  local total = s.drawn + s.culled
  local pct = (total > 0) and math.floor(100 * s.culled / total + 0.5) or 0
  local x = 230

  pd.draw_text(x, 28, "-- OCTREE --", C_GREEN)
  pd.draw_text(x, 38, string.format("drawn  %d", s.drawn), C_WHITE)
  pd.draw_text(x, 46, string.format("culled %d", s.culled), C_CYAN)
  pd.draw_text(x, 54, string.format("cull   %d%%", pct), (pct > 0) and C_GREEN or C_RED)
  pd.draw_text(x, 62, string.format("nodes  %d/%d cut", s.nodesculled, s.nodes), C_WHITE)
  pd.draw_text(x, 70, string.format("passes %d", s.passes), C_WHITE)
end)
