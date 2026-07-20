-- Silo Countdown TEST HARNESS
--
-- The real "Silo Countdown" chaos effect runs for chaos.silo_seconds (default
-- 8 minutes). This harness fires it with a 1-MINUTE timer so you can test the
-- whole sequence — the MM:SS clock, the 30-second Silox.mp3 swap, and the
-- detonation — without waiting eight minutes. It restores the 8-minute default
-- immediately after firing, so the normal effect is left unchanged (the running
-- timer already captured the 60s at trigger time).
--
-- Use it via:
--   ~ console:   /lua silo_test()
--   pause menu:  Lua Director -> Chaos Alpha -> "Silo Countdown (1-min test)"
--
-- Loaded from scripts/init.lua AFTER chaos.lua (it needs chaos.trigger, and it
-- registers its menu entry after chaos.lua has built the Chaos Alpha submenu).

function silo_test()
  if not chaos or not chaos.trigger then
    pd.log("silo_test: chaos.lua not loaded")
    return false
  end
  local saved = chaos.silo_seconds
  chaos.silo_seconds = 60                      -- 1-minute countdown for testing
  chaos.trigger("silo_countdown", "1-min test") -- start()/dur() capture the 60s now
  chaos.silo_seconds = saved                   -- leave the real 8-min default intact
  return true
end

-- Pause-menu shortcut, dropped into the same Chaos Alpha submenu as the full
-- Silo Countdown testbed entry.
if pd.menu_add then
  pd.menu_add("Silo Countdown (1-min test)", silo_test, "Chaos Alpha")
end

pd.log("silo_test.lua loaded — /lua silo_test() for a 1-minute Silo Countdown")
