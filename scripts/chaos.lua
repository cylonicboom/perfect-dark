-- Chaos / randomiser mode (docs/PORT_CHAOS.md).
--
-- A timer picks a weighted random effect every `interval` seconds; timed
-- effects clean themselves up. Everything is driven through the pd.* native
-- hooks, so new effects are pure Lua — no rebuild.
--
-- Control surface (all reach chaos.handle(), in priority order):
--   ~ console:  /chaos on|off|toggle|status|list|interval N|votetime N|
--               trigger <effect>|vote <effect>|say <text>|seed N
--   UDP ingress: same verbs, one datagram each, to 127.0.0.1:<Chaos.EventPort>
--               (pd.ini [Chaos] EventPort=27110) — the Twitch/YouTube window:
--               point any chat bot / Streamer.bot / SAMMI action at it.
--   Lua:        chaos.trigger("mirror"), chaos.handle("udp", "vote yeet") — so
--               the Archipelago client can map AP traps/deathlink to effects.
--   Pause menu: Lua Director entries (toggle / trigger now).
--
-- Vote mode: while votetime > 0, incoming `vote <effect>` lines are tallied
-- and the winner fires when the window closes (repeats forever). `trigger`
-- lines fire immediately regardless (channel-point style).
--
-- AP integration: scripts/ap/client.lua can call chaos.trigger() on trap
-- items / DeathLink bounces; chaos.set_seed(slotseed) makes the effect stream
-- deterministic per AP slot (a true per-seed randomiser).

chaos = chaos or {}

local TICKS = 60 -- pd "tick" event runs at the sim rate

-- ---------------------------------------------------------------- state ----
local st = {
  enabled  = (pd.persist_get and pd.persist_get("chaos_enabled") == "1") or false,
  interval = tonumber(pd.persist_get and pd.persist_get("chaos_interval") or "") or 30,
  votetime = tonumber(pd.persist_get and pd.persist_get("chaos_votetime") or "") or 0,
  timer    = 0,          -- ticks until the next random effect
  votetimer = 0,         -- ticks left in the current vote window
  votes    = {},         -- effect -> count
  active   = {},         -- name -> ticks remaining (timed effects)
  history  = {},         -- last few names, to avoid instant repeats
}

local function persist()
  if pd.persist_set then
    pd.persist_set("chaos_enabled", st.enabled and "1" or "0")
    pd.persist_set("chaos_interval", tostring(st.interval))
    pd.persist_set("chaos_votetime", tostring(st.votetime))
  end
end

local function announce(text)
  pd.hud_message("CHAOS: " .. text)
  pd.log("[chaos] " .. text)
end

-- ------------------------------------------------------------- effects -----
-- duration in seconds (0 = instant). start/stop run under pcall.
-- Weapon/cheat ids from src/include/constants.h.
local W = { FALCON2=0x02, MAGSEC=0x05, MAULER=0x06, PHOENIX=0x07, MAGNUM=0x08, LX=0x09,
  CMP150=0x0a, CYCLONE=0x0b, LAPTOP=0x0e, DRAGON=0x0f, K7=0x10, AR34=0x11,
  SUPERDRAGON=0x12, SHOTGUN=0x13, REAPER=0x14, SNIPER=0x15, FARSIGHT=0x16,
  DEVASTATOR=0x17, ROCKET=0x18, SLAYER=0x19, KNIFE=0x1a, CROSSBOW=0x1b,
  TRANQ=0x1c, LASER=0x1d, GRENADE=0x1e, NBOMB=0x1f, TIMEDMINE=0x20,
  PROXYMINE=0x21, REMOTEMINE=0x22, UNARMED=0x01,
  NIGHTVISION=0x2d, XRAY=0x2f, IR=0x30, CLOAK=0x31 }
local GUNS = { W.FALCON2, W.MAGSEC, W.MAULER, W.PHOENIX, W.MAGNUM, W.CMP150,
  W.CYCLONE, W.LAPTOP, W.DRAGON, W.K7, W.AR34, W.SUPERDRAGON, W.SHOTGUN,
  W.REAPER, W.SNIPER, W.FARSIGHT, W.DEVASTATOR, W.ROCKET, W.SLAYER,
  W.CROSSBOW, W.TRANQ, W.GRENADE }
local CHEAT = { FISTS=0, AMMO=4, SLOMO=6, DK=7, SMALLJO=10, SMALLCHARS=11,
  ENEMYSHIELDS=12, JOSHIELD=13, SUPERSHIELD=14, TEAMHEADS=16, ELVIS=17,
  ENEMYROCKETS=18, MARQUIS=20, WIREFRAME=46, MIRROR=47, TONAL=48 }

local function cheat_effect(id, secs)
  return {
    dur = secs,
    start = function() pd.cheat(id, true) end,
    stop  = function() pd.cheat(id, false) end,
  }
end

local function random_chr()
  local list = pd.all_chrs and pd.all_chrs() or nil
  if not list or #list == 0 then return nil end
  return list[math.random(#list)]
end

-- hue (0..359) -> r, g, b in 0..255, full saturation/value (disco lights)
local function hsv(h)
  local x = math.floor((1 - math.abs((h / 60) % 2 - 1)) * 255)
  if h < 60 then return 255, x, 0
  elseif h < 120 then return x, 255, 0
  elseif h < 180 then return 0, 255, x
  elseif h < 240 then return 0, x, 255
  elseif h < 300 then return x, 0, 255
  else return 255, 0, x end
end

chaos.effects = {
  -- arsenal roulette
  arsenal      = { label="Free gun!",         w=10, dur=0, start=function()
                     local g = GUNS[math.random(#GUNS)]
                     pd.give_weapon(g); pd.switch_weapon(g); pd.refill_ammo() end },
  disarm       = { label="Butterfingers",     w=8,  dur=0, start=function()
                     local h = pd.weapon_held()
                     if h and h > W.UNARMED then pd.take_weapon(h) end end },
  knife_fight  = { label="Knife fight!",      w=5,  dur=0, start=function()
                     pd.give_weapon(W.KNIFE); pd.switch_weapon(W.KNIFE) end },
  ammo_rain    = { label="Ammo rain",         w=8,  dur=0, start=function() pd.refill_ammo() end },
  -- cheat-bank chaos (visual + gameplay)
  mirror       = setmetatable({ label="Mirror world",  w=8 }, {__index=cheat_effect(CHEAT.MIRROR, 30)}),
  wireframe    = setmetatable({ label="The Matrix",    w=6 }, {__index=cheat_effect(CHEAT.WIREFRAME, 20)}),
  tonal        = setmetatable({ label="Evil music",    w=6 }, {__index=cheat_effect(CHEAT.TONAL, 60)}),
  fists        = setmetatable({ label="Hurricane fists", w=6 }, {__index=cheat_effect(CHEAT.FISTS, 30)}),
  slomo        = setmetatable({ label="Slow motion",   w=6 }, {__index=cheat_effect(CHEAT.SLOMO, 12)}),
  dkmode       = setmetatable({ label="DK mode",       w=5 }, {__index=cheat_effect(CHEAT.DK, 45)}),
  smalljo      = setmetatable({ label="Tiny Jo",       w=4 }, {__index=cheat_effect(CHEAT.SMALLJO, 30)}),
  smallchars   = setmetatable({ label="Tiny everyone", w=4 }, {__index=cheat_effect(CHEAT.SMALLCHARS, 30)}),
  elvis        = setmetatable({ label="Play as Elvis", w=3 }, {__index=cheat_effect(CHEAT.ELVIS, 45)}),
  marquis      = setmetatable({ label="Marquis mode",  w=3 }, {__index=cheat_effect(CHEAT.MARQUIS, 30)}),
  enemyrockets = setmetatable({ label="Enemy rockets!", w=3 }, {__index=cheat_effect(CHEAT.ENEMYROCKETS, 30)}),
  enemyshields = setmetatable({ label="Shielded enemies", w=4 }, {__index=cheat_effect(CHEAT.ENEMYSHIELDS, 30)}),
  -- player state
  godmode      = { label="Invincible!",       w=4, dur=10,
                   start=function() pd.invincible(true) end,
                   stop=function() pd.invincible(false) end },
  cloak        = { label="Now you see me...", w=5, dur=20, start=function() pd.device_on(W.CLOAK) end },
  xray         = { label="X-ray specs",       w=4, dur=20, start=function() pd.device_on(W.XRAY) end },
  nightvision  = { label="Night vision",      w=4, dur=20, start=function() pd.device_on(W.NIGHTVISION) end },
  heal         = { label="Medic!",            w=5, dur=0, start=function() pd.player_heal(); pd.player_set_shield(1) end },
  blink        = { label="Blink",             w=5, dur=0, start=function() pd.fade(255,255,255,255, 45) end },
  -- ammo roulette (pd.ammo_swap: every held gun fires another weapon's
  -- primary rounds; refills keep the borrowed ammo topped up while active)
  rocket_rounds  = { label="Rockets for everyone", w=4, dur=20,
                     start=function() pd.ammo_swap(W.ROCKET); pd.refill_ammo() end,
                     tick=function(left) if left % 120 == 0 then pd.refill_ammo() end end,
                     stop=function() pd.ammo_swap() end },
  grenade_rounds = { label="Grenade machine gun",  w=4, dur=20,
                     start=function() pd.ammo_swap(W.DEVASTATOR); pd.refill_ammo() end,
                     tick=function(left) if left % 120 == 0 then pd.refill_ammo() end end,
                     stop=function() pd.ammo_swap() end },
  golden_gun     = { label="The golden gun",       w=3, dur=15,
                     start=function() pd.ammo_swap(W.LX); pd.refill_ammo() end,
                     stop=function() pd.ammo_swap() end },
  farsight_rounds= { label="FarSight rounds",      w=3, dur=15,
                     start=function() pd.ammo_swap(W.FARSIGHT); pd.refill_ammo() end,
                     tick=function(left) if left % 120 == 0 then pd.refill_ammo() end end,
                     stop=function() pd.ammo_swap() end },
  sedative_rounds= { label="Sedative rounds",      w=3, dur=20,
                     start=function() pd.ammo_swap(W.TRANQ); pd.refill_ammo() end,
                     stop=function() pd.ammo_swap() end },
  backfire       = { label="Backwards bullets",   w=4, dur=15,
                     start=function() pd.backfire(true) end,
                     stop=function() pd.backfire(false) end },
  -- the Air Force One crash block: explosions everywhere, but you're covered
  self_destruct  = { label="SELF-DESTRUCT SEQUENCE", w=3, dur=8,
                     start=function()
                       pd.invincible(true)
                       pd.explosions_around(true)
                     end,
                     stop=function()
                       pd.explosions_around(false)
                       pd.invincible(false)
                     end },
  -- visual chaos (renderer + room lighting hooks; timed, all self-revert)
  untextured   = { label="1996 mode",          w=5, dur=30,
                   start=function() pd.flattex(1) end,
                   stop=function() pd.flattex(0) end },
  watercolour  = { label="Watercolour world",  w=5, dur=30,
                   start=function() pd.flattex(2) end,
                   stop=function() pd.flattex(0) end },
  noir         = { label="Film noir",          w=5, dur=30,
                   start=function() pd.grayscale(true) end,
                   stop=function() pd.grayscale(false) end },
  paint_red    = { label="Paint the town red", w=6, dur=30,
                   start=function() pd.room_tint(255, 48, 48) end,
                   stop=function() pd.room_tint() end },
  toxic        = { label="Toxic spill",        w=4, dur=25,
                   start=function() pd.room_tint(80, 255, 80) end,
                   stop=function() pd.room_tint() end },
  blackout     = { label="Lights out",         w=4, dur=15,
                   start=function() pd.room_tint(30, 30, 60) end,
                   stop=function() pd.room_tint() end },
  disco        = { label="Disco inferno",      w=5, dur=20,
                   start=function() pd.room_tint(255, 64, 64) end,
                   tick=function(left)
                     if left % 12 == 0 then pd.room_tint(hsv((left * 5) % 360)) end
                   end,
                   stop=function() pd.room_tint() end },
  -- player state, SA-chaos style
  turbo        = { label="GOTTA GO FAST",     w=6, dur=0, start=function() pd.boost(15) end },
  drunk        = { label="One too many",      w=6, dur=0, start=function() pd.dizzy(3500) end },
  one_hp       = { label="Health roulette",   w=4, dur=0, start=function()
                     pd.player_set_health(math.random(5, 60) / 100) end },
  dry_spell    = { label="Dry spell",         w=5, dur=0, start=function() pd.strip_ammo() end },
  quantum_leap = { label="Quantum leap",      w=5, dur=0, start=function()
                     local c = random_chr(); if c then pd.teleport_to_chr(c) end end },
  lock_n_load  = { label="Lock and load",     w=3, dur=0, start=function()
                     for _, g in ipairs(GUNS) do pd.give_weapon(g) end
                     pd.refill_ammo() end },
  amnesia      = { label="Amnesia",           w=2, dur=0, start=function()
                     for _, g in ipairs(GUNS) do pd.take_weapon(g) end
                     pd.take_weapon(W.KNIFE) end },
  -- world chaos
  intruder     = { label="INTRUDER ALERT",    w=5, dur=20,
                   start=function() pd.alarm(true) end,
                   stop=function() pd.alarm(false) end },
  predators    = { label="Predators",         w=4, dur=20,
                   start=function()
                     for _, c in ipairs(pd.all_chrs() or {}) do pd.chr_cloak(c, true) end end,
                   stop=function()
                     for _, c in ipairs(pd.all_chrs() or {}) do pd.chr_cloak(c, false) end end },
  airstrike    = { label="Airstrike",         w=4, dur=0, start=function()
                     local list = pd.all_chrs() or {}
                     for i = 1, math.min(4, #list) do
                       pd.explosion(list[math.random(#list)])
                     end end },
  panic        = { label="PANIC!",            w=6, dur=0, start=function()
                     for _, c in ipairs(pd.all_chrs() or {}) do pd.chr_alert(c) end end },
  yeet         = { label="YEET",              w=6, dur=0, start=function()
                     for _, c in ipairs(pd.all_chrs() or {}) do pd.chr_yeet(c, 120) end end },
  boom         = { label="Incoming!",         w=5, dur=0, start=function()
                     local c = random_chr(); if c then pd.explosion(c) end end },
  buddy        = { label="Backup arrives",    w=5, dur=0, start=function() pd.spawn_ally() end },
  reinforce    = { label="Reinforcements",    w=4, dur=0, start=function()
                     local c = random_chr(); if c then pd.spawn_at_chr(c, GUNS[math.random(#GUNS)]) end end },
}

-- fix the setmetatable shorthand: pull dur/start/stop through the metatable
for name, e in pairs(chaos.effects) do
  local mt = getmetatable(e)
  if mt and mt.__index then
    e.dur = e.dur or mt.__index.dur
    e.start = e.start or mt.__index.start
    e.stop = e.stop or mt.__index.stop
    setmetatable(e, nil)
  end
end

-- ------------------------------------------------------------- engine ------
local function stop_effect(name)
  local e = chaos.effects[name]
  if e and e.stop then pcall(e.stop) end
  st.active[name] = nil
end

local function stop_all()
  for name in pairs(st.active) do stop_effect(name) end
end

function chaos.trigger(name, who)
  local e = chaos.effects[name]
  if not e then
    pd.log("[chaos] unknown effect: " .. tostring(name))
    return false
  end
  if st.active[name] then stop_effect(name) end -- restart timed effects cleanly
  local ok, err = pcall(e.start)
  if not ok then
    pd.log("[chaos] effect '" .. name .. "' failed: " .. tostring(err))
    return false
  end
  if e.dur and e.dur > 0 then st.active[name] = e.dur * TICKS end
  announce(e.label .. (who and ("  [" .. who .. "]") or ""))
  table.insert(st.history, 1, name)
  if #st.history > 4 then table.remove(st.history) end
  return true
end

local function pick_random()
  local pool, total = {}, 0
  for name, e in pairs(chaos.effects) do
    local recent = false
    for _, h in ipairs(st.history) do if h == name then recent = true end end
    if not recent then
      total = total + (e.w or 1)
      pool[#pool + 1] = { name = name, acc = total }
    end
  end
  if total == 0 then return nil end
  local r = math.random(total)
  for _, p in ipairs(pool) do
    if r <= p.acc then return p.name end
  end
end

function chaos.set_seed(n)
  math.randomseed(tonumber(n) or 0)
  pd.log("[chaos] seeded with " .. tostring(n) .. " (deterministic effect stream)")
end

-- ---------------------------------------------------- external protocol ----
function chaos.handle(source, text)
  local cmd, arg = text:match("^(%S+)%s*(.*)$")
  if not cmd then return end
  cmd = cmd:lower()
  if cmd == "on" then
    st.enabled = true; st.timer = st.interval * TICKS; persist(); announce("enabled")
  elseif cmd == "off" then
    st.enabled = false; stop_all(); persist(); announce("disabled")
  elseif cmd == "toggle" then
    chaos.handle(source, st.enabled and "off" or "on")
  elseif cmd == "status" then
    pd.log(string.format("[chaos] %s  interval=%ds votetime=%ds active=%d port-fed-by=%s",
        st.enabled and "ON" or "off", st.interval, st.votetime,
        (function() local n=0 for _ in pairs(st.active) do n=n+1 end return n end)(), source))
  elseif cmd == "list" then
    local names = {}
    for name in pairs(chaos.effects) do names[#names + 1] = name end
    table.sort(names)
    pd.log("[chaos] effects: " .. table.concat(names, " "))
  elseif cmd == "interval" then
    st.interval = math.max(5, tonumber(arg) or 30); persist()
    pd.log("[chaos] interval = " .. st.interval .. "s")
  elseif cmd == "votetime" then
    st.votetime = math.max(0, tonumber(arg) or 0); st.votetimer = st.votetime * TICKS
    st.votes = {}; persist()
    pd.log("[chaos] votetime = " .. st.votetime .. "s" .. (st.votetime == 0 and " (off)" or ""))
  elseif cmd == "trigger" then
    local name, who = arg:match("^(%S+)%s*(.*)$")
    chaos.trigger(name or "", who ~= "" and who or source)
  elseif cmd == "vote" then
    local name = arg:match("^(%S+)")
    if name and chaos.effects[name] and st.votetime > 0 then
      st.votes[name] = (st.votes[name] or 0) + 1
    end
  elseif cmd == "seed" then
    chaos.set_seed(arg)
  elseif cmd == "say" then
    if arg ~= "" then pd.hud_message(arg:sub(1, 60)) end
  else
    pd.log("[chaos] unknown command from " .. source .. ": " .. text)
  end
end

-- ------------------------------------------------------------- wiring ------
pd.on("tick", function()
  -- drain external events even while disabled, so `on` can arrive over UDP
  while true do
    local source, text = pd.ext_poll()
    if not source then break end
    local ok, err = pcall(chaos.handle, source, text)
    if not ok then pd.log("[chaos] handler error: " .. tostring(err)) end
  end

  if not st.enabled then return end

  -- timed effect expiry (+ optional per-tick driver, e.g. disco's hue cycle)
  for name, left in pairs(st.active) do
    local e = chaos.effects[name]
    if e and e.tick then pcall(e.tick, left) end
    left = left - 1
    if left <= 0 then
      stop_effect(name)
      announce((e and e.label or name) .. " wore off")
    else
      st.active[name] = left
    end
  end

  -- vote window
  if st.votetime > 0 then
    st.votetimer = st.votetimer - 1
    if st.votetimer <= 0 then
      st.votetimer = st.votetime * TICKS
      local best, bestn = nil, 0
      for name, n in pairs(st.votes) do
        if n > bestn then best, bestn = name, n end
      end
      st.votes = {}
      if best then chaos.trigger(best, "chat vote x" .. bestn) end
    end
  end

  -- the random drumbeat
  st.timer = st.timer - 1
  if st.timer <= 0 then
    st.timer = st.interval * TICKS
    local name = pick_random()
    if name then chaos.trigger(name) end
  end
end)

pd.on("stage", function()
  -- fresh world: drop timed-effect bookkeeping (cheat banks reset with the
  -- stage; re-arm the timer so the first effect isn't instant)
  st.active = {}
  st.votes = {}
  st.timer = st.interval * TICKS
  -- the visual modes + ammo swap live in globals that SURVIVE the stage
  -- reload (unlike the cheat bank) — reset them explicitly
  if pd.flattex then pd.flattex(0) end
  if pd.grayscale then pd.grayscale(false) end
  if pd.room_tint then pd.room_tint() end
  if pd.ammo_swap then pd.ammo_swap() end
  if pd.backfire then pd.backfire(false) end
end)

if pd.menu_add then
  pd.menu_add("Chaos: toggle",      function() chaos.handle("menu", "toggle") end)
  pd.menu_add("Chaos: random now",  function() local n = pick_random(); if n then chaos.trigger(n, "menu") end end)
end

pd.log("chaos.lua loaded (" .. (st.enabled and "ENABLED" or "off") .. ") — /chaos on | /chaos list")
