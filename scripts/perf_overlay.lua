-- Render-time + memory on-screen overlays. Two independent toggles:
--   /fps   render time  (fps + frame ms)
--   /mem   memory       (per-frame vtx scratch pool used/total)
--
-- Drawn on the RIGHT, stacked just below the octree overlay (octree_overlay.lua
-- is x=230 y=28-70), so the two read as one group and we stay clear of the AI
-- X-ray (showcase.lua, left column). Kept near the top because the lo-res screen
-- is only ~220 tall -- the bottom few rows are off-screen. Move X/Y_* to taste.
--
-- Reads pd.perf(); show_fps/show_mem come from the /fps and /mem console toggles
-- (g_LuaShowFps / g_LuaShowMem). Loaded by scripts/init.lua. Colours 0xRRGGBBAA.

local C_WHITE = 0xffffffff
local C_GREEN = 0x40ff40ff
local C_YELL  = 0xffe040ff
local C_RED   = 0xff5050ff

local X     = 230  -- right column, under the octree overlay
local Y_FPS = 80
local Y_MEM = 88

pd.log("perf_overlay.lua loaded (toggle with /fps and /mem)")

local function kb(bytes) return bytes / 1024 end

pd.on("draw", function()
  local p = pd.perf()
  if not p then return end

  if p.show_fps then
    -- colour by frame time: green < 17ms (60fps), yellow < 33ms, red otherwise
    local c = (p.frame_ms < 17) and C_GREEN or (p.frame_ms < 33) and C_YELL or C_RED
    pd.draw_text(X, Y_FPS, string.format("%.0ffps %.1fms", p.fps, p.frame_ms), c)
  end

  if p.show_mem then
    local pct = (p.vtx_total > 0) and (100 * p.vtx_used / p.vtx_total) or 0
    local c = (pct < 75) and C_WHITE or (pct < 90) and C_YELL or C_RED
    pd.draw_text(X, Y_MEM, string.format("vtx %.0f/%.0fK", kb(p.vtx_used), kb(p.vtx_total)), c)
  end
end)
