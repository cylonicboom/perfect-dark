-- Display-list cache — live on-screen stats for testing the GPU-resident cache.
--
-- Reads pd.dlcache_stats() every frame and draws it top-right, below the perf
-- overlay. Only shows while /dlcache is ON. Unlike `/dlcache stats` (a one-shot
-- console print) this updates live, so you can watch `cached` climb as rooms
-- record, `draw` (batches/tris replayed) rise and fall as you move/cull, the
-- `tex` line go RED/FULL when the texture cache overflows (the recorder evicting
-- on-screen textures = black surfaces; raise with /texcache or Video.TextureCacheSize),
-- and the `bad:` reason flags tell you why some leaves stayed on the legacy path
-- (fog / lit = G_LIGHTING / cull2 = G_CULL_BOTH / empty). See docs/PORT_DLCACHE.md.
--
-- Loaded by scripts/init.lua. Colours are 0xRRGGBBAA; coords are the lo-res
-- virtual screen (~320x240, like the console).

local C_WHITE = 0xffffffff
local C_GREEN = 0x40ff40ff
local C_RED   = 0xff5050ff
local C_CYAN  = 0x40e0ffff
local C_YELL  = 0xffe040ff

local X = 230 -- right column, under the octree (28-70) + perf (80-88) overlays
local Y = 98

pd.log("dlcache_overlay.lua loaded (toggle with /dlcache on)")

pd.on("draw", function()
  local s = pd.dlcache_stats()
  if not s or not s.enabled then
    return -- cache off; nothing to show
  end

  pd.draw_text(X, Y,      "-- DLCACHE --", C_GREEN)
  pd.draw_text(X, Y + 10, string.format("cached %d  bad %d", s.cached, s.bad),
      (s.cached > 0) and C_WHITE or C_RED)
  pd.draw_text(X, Y + 18, string.format("draw %db %dt", s.batches, s.tris), C_CYAN)

  -- Texture-cache fill. FULL (used >= max) = the recorder is evicting on-screen
  -- textures -> they render black. Red when full so it's obvious at a glance.
  local tu, tm = s.tex_used or 0, s.tex_max or 0
  local full = (tm > 0 and tu >= tm)
  pd.draw_text(X, Y + 26,
      string.format("tex %d/%d%s", tu, tm, full and " FULL" or ""),
      full and C_RED or C_WHITE)

  if s.bad > 0 then
    local r = ""
    if s.fog      then r = r .. " fog"    end
    if s.lighting then r = r .. " lit"    end
    if s.cullboth then r = r .. " cull2"  end
    if s.empty    then r = r .. " empty"  end
    if s.texgen   then r = r .. " texgen" end
    if r == "" then r = " ?" end
    pd.draw_text(X, Y + 34, "bad:" .. r, C_YELL)
  end
end)
