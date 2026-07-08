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
  candidates = {},       -- the 3 effects chat can vote on this window
  cvotes   = {0, 0, 0},  -- votes per candidate slot
  active   = {},         -- name -> ticks remaining (timed effects)
  duration = {},         -- name -> total ticks (for the HUD bars)
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
local BODY = { MINISKEDAR=0x7b }
local CHEAT = { FISTS=0, AMMO=4, NORELOAD=5, SLOMO=6, DK=7, SMALLJO=10, SMALLCHARS=11,
  ENEMYSHIELDS=12, JOSHIELD=13, SUPERSHIELD=14, TEAMHEADS=16, ELVIS=17,
  ENEMYROCKETS=18, MARQUIS=20, GOLDENEYE=45, WIREFRAME=46, MIRROR=47, TONAL=48 }

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
  -- CHEAT_GOLDENEYE = the "GoldenEye Style" master: all 12 classic behaviours
  -- at once (snap lean, lower-and-raise reloads, GE arc HUD + damage flash,
  -- classic crosshair, no dual-wield, i-frames, ...). docs/PORT_GOLDENEYE.md.
  goldeneye    = setmetatable({ label="GoldenEye mode", w=5 }, {__index=cheat_effect(CHEAT.GOLDENEYE, 45)}),
  -- player state
  godmode      = { label="Invincible!",       w=4, dur=10,
                   start=function() pd.invincible(true) end,
                   stop=function() pd.invincible(false) end },
  cloak        = { label="Now you see me...", w=5, dur=20,
                   start=function() pd.device_on(W.CLOAK) end,
                   stop=function() pd.device_off(W.CLOAK) end },
  xray         = { label="X-ray specs",       w=4, dur=20,
                   start=function() pd.device_on(W.XRAY) end,
                   stop=function() pd.device_off(W.XRAY) end },
  nightvision  = { label="Night vision",      w=4, dur=20,
                   start=function() pd.device_on(W.NIGHTVISION) end,
                   stop=function() pd.device_off(W.NIGHTVISION) end },
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
  nbomb_me       = { label="N-Bomb delivery",     w=4, dur=0,
                     start=function() pd.nbomb() end },
  hurricane      = { label="Hurricane",           w=4, dur=0,
                     start=function() pd.gust(150) end },
  cyclone_frenzy = { label="CYCLONE FRENZY",      w=3, dur=30,
                     start=function()
                       pd.dual_wield(W.CYCLONE, 1)    -- both hands, Magazine Discharge
                       pd.cheat(CHEAT.NORELOAD, true) -- unlimited ammo, no reloads
                       pd.refill_ammo()
                     end,
                     stop=function() pd.cheat(CHEAT.NORELOAD, false) end },
  widescreen     = { label="CinemaScope",         w=3, dur=20,
                     start=function() pd.aspect_scale(2) end,
                     stop=function() pd.aspect_scale(1) end },
  tallscreen     = { label="Tall boy",            w=3, dur=20,
                     start=function() pd.aspect_scale(0.5) end,
                     stop=function() pd.aspect_scale(1) end },
  cavalry        = { label="Send in the cavalry", w=3, dur=0,
                     start=function() for i = 1, 4 do pd.spawn_ally() end end },
  jukebox        = { label="Jukebox",             w=5, dur=60,
                     start=function() pd.song(math.random(0, 255)) end,
                     stop=function() pd.song() end },
  skedar_ring    = { label="Skedar ambush",       w=3, dur=0,
                     start=function()
                       for i = 0, 3 do
                         local a = i * math.pi / 2
                         pd.spawn_body(BODY.MINISKEDAR, -1, math.sin(a) * 150, math.cos(a) * 150)
                       end
                     end },
  -- FOV warps (self-restoring setter hook, like aspect_scale)
  fisheye        = { label="Quake Pro",           w=4, dur=20,
                     start=function() pd.fov_scale(1.6) end,
                     stop=function() pd.fov_scale(1) end },
  tunnel_vision  = { label="Tunnel vision",       w=4, dur=20,
                     start=function() pd.fov_scale(0.55) end,
                     stop=function() pd.fov_scale(1) end },
  vertigo        = { label="Vertigo",             w=3, dur=15,
                     start=function() pd.fov_scale(1.2) end,
                     tick=function(left)
                       pd.fov_scale(1 + 0.35 * math.sin(left / 12))
                     end,
                     stop=function() pd.fov_scale(1) end },
  -- crowd control
  infighting     = { label="Civil war",           w=4, dur=0,
                     start=function()
                       local list = pd.all_chrs() or {}
                       if #list < 2 then error("not enough chrs") end
                       for i = 1, #list do
                         pd.chr_target(list[i], list[(i % #list) + 1])
                       end
                     end },
  neuralyzer     = { label="Neuralyzed",          w=4, dur=0,
                     start=function()
                       for _, c in ipairs(pd.all_chrs() or {}) do pd.chr_calm(c) end
                     end },
  house_party    = { label="House party",         w=3, dur=0,
                     start=function()
                       local list = pd.all_chrs() or {}
                       if #list == 0 then error("no chrs") end
                       for i, c in ipairs(list) do
                         local a = (i / #list) * 2 * math.pi
                         pd.chr_summon(c, math.sin(a) * 220, math.cos(a) * 220)
                       end
                     end },
  evil_twin      = { label="Evil twin",           w=2, dur=0,
                     start=function()
                       local held = pd.weapon_held()
                       local a = math.random() * 2 * math.pi
                       pd.spawn_body(-1, (held and held > 1) and held or W.FALCON2,
                                     math.sin(a) * 180, math.cos(a) * 180)
                     end },
  -- doors
  open_sesame    = { label="Open sesame",         w=4, dur=0,
                     start=function() pd.doors_all(true) end },
  lockdown       = { label="Lockdown",            w=3, dur=0,
                     start=function() pd.doors_all(false) end },
  body_snatch    = { label="BODY SNATCHED",       w=1, dur=0,
                     start=function()
                       local list = pd.all_chrs() or {}
                       if #list == 0 then error("no chrs") end
                       for _ = 1, 8 do
                         local c = list[math.random(#list)]
                         if c and pd.body_snatch(c) then return end
                       end
                       error("no snatchable chr")
                     end },
  joyride        = { label="Joyride",             w=3, dur=0,
                     start=function()
                       if not pd.spawn_bike() then error("no bike here") end
                     end },
  soundboard     = { label="Soundboard",          w=4, dur=20,
                     start=function() pd.sfx_shuffle(true) end,
                     stop=function() pd.sfx_shuffle(false) end },
  kazoo          = { label="Discount orchestra",  w=4, dur=60,
                     start=function()
                       pd.instrument_shuffle(true)
                       pd.song(math.random(0, 255)) -- program changes fire at track start
                     end,
                     stop=function()
                       pd.instrument_shuffle(false)
                       pd.song()
                     end },
  gormless       = { label="Gormless",            w=4, dur=20,
                     start=function() pd.gormless(true) end,
                     stop=function() pd.gormless(false) end },
  one_punch      = { label="ONE PUNCH",           w=3, dur=25,
                     start=function()
                       pd.cheat(CHEAT.FISTS, true) -- Hurricane Fists punch speed
                       pd.one_punch(true)
                       pd.switch_weapon(W.UNARMED)
                     end,
                     tick=function()
                       -- fists ONLY: snap back if the player switches away
                       local h = pd.weapon_held()
                       if h and h ~= W.UNARMED then pd.switch_weapon(W.UNARMED) end
                     end,
                     stop=function()
                       pd.one_punch(false)
                       pd.cheat(CHEAT.FISTS, false)
                     end },
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
  shiny        = { label="So shiny!",          w=5, dur=25,
                   start=function() pd.shiny(1) end,
                   stop=function() pd.shiny(0) end },
  midas        = { label="The Midas touch",    w=4, dur=25,
                   start=function() pd.shiny(2) end,
                   stop=function() pd.shiny(0) end },
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
  reinforce    = { label="Supply drop",       w=4, dur=0, start=function()
                     local c = random_chr(); if c then pd.spawn_at_chr(c, GUNS[math.random(#GUNS)]) end end },
  -- request batch 4
  k7_party     = { label="K7 Avengers for all", w=4, dur=0, start=function()
                     local list = pd.all_chrs() or {}
                     if #list == 0 then error("no chrs") end
                     for _, c in ipairs(list) do pd.chr_give_weapon(c, W.K7) end end },
  paintball    = { label="Paintball!",        w=5, dur=30,
                   start=function()
                     pd.paintball(true)
                     pd.gun_sound(W.TRANQ)   -- every gun fires with the tranq's pfft
                     pd.damage_scale(0.1)    -- stings, doesn't kill (much)
                   end,
                   stop=function()
                     pd.paintball(false)
                     pd.gun_sound()
                     pd.damage_scale(1)
                   end },
  misfire      = { label="Misfire",           w=5, dur=0,
                   start=function() st.misfire_armed = true end },
  weapon_jam   = { label="Weapon jam",        w=5, dur=12,
                   start=function() pd.weapon_jam(true) end,
                   stop=function() pd.weapon_jam(false) end },
  take_a_break = { label="Take a break",      w=4, dur=function() return math.random(10, 30) end,
                   start=function() pd.player_freeze(true) end,
                   stop=function() pd.player_freeze(false) end },
  vampire      = { label="Vampire",           w=4, dur=30,
                   -- drain ~2%/s; damaging enemies feeds you (see the
                   -- pd.on("damage") handler below)
                   tick=function(left)
                     if left % 60 == 0 then
                       local h = pd.player_health()
                       if h <= 0.03 then pd.player_damage(20) -- drained dry
                       else pd.player_set_health(h - 0.02) end
                     end
                   end,
                   start=function() end,
                   stop=function() end },
  freeze       = { label="FREEZE!",           w=4, dur=function() return math.random(10, 20) end,
                   start=function() pd.chr_freeze(true) end,
                   stop=function() pd.chr_freeze(false) end },
  no_drops     = { label="No drops",          w=4, dur=45,
                   start=function() pd.no_drops(true) end,
                   stop=function() pd.no_drops(false) end },
  random_loadout = { label="Random loadout",  w=4, dur=0, start=function()
                     for _, g in ipairs(GUNS) do pd.take_weapon(g) end
                     pd.take_weapon(W.KNIFE)
                     local given, n, first = {}, 0, nil
                     while n < 6 do
                       local g = GUNS[math.random(#GUNS)]
                       if not given[g] then
                         given[g] = true
                         pd.give_weapon(g)
                         first = first or g
                         n = n + 1
                       end
                     end
                     pd.refill_ammo()
                     if first then pd.switch_weapon(first) end end },
  muted        = { label="Muted",             w=4, dur=20,
                   start=function() pd.mute(true) end,
                   stop=function() pd.mute(false) end },
  ring_ring    = { label="Ring ring!",        w=4, dur=0, start=function()
                     -- ships without the sound; drop a WAV or MP3 at this path
                     -- (e.g. the Discord call ringtone) to complete the bit
                     if not (pd.play_file("scripts/sounds/chaos/ring.wav")
                         or pd.play_file("scripts/sounds/chaos/ring.mp3")) then
                       error("scripts/sounds/chaos/ring.wav|mp3 missing")
                     end
                     for _, c in ipairs(pd.all_chrs() or {}) do pd.chr_alert(c) end end },
  negative_zoom = { label="Negative zoom",    w=4, dur=25,
                   start=function() pd.zoom_scale(4) end,
                   stop=function() pd.zoom_scale(1) end },
  -- freeform batch: hooks nobody asked for but everybody needs
  benny_hill   = { label="Benny Hill mode",   w=4, dur=20,
                   start=function() pd.chr_speed(2.5) end,
                   stop=function() pd.chr_speed(1) end },
  zombies      = { label="Zombie shuffle",    w=4, dur=20,
                   start=function() pd.chr_speed(0.4) end,
                   stop=function() pd.chr_speed(1) end },
  earthquake   = { label="EARTHQUAKE",        w=4, dur=15,
                   start=function() pd.shake(40); pd.dizzy(1500) end,
                   tick=function(left)
                     if left % 75 == 0 then pd.shake(30) end
                   end },
  thanos_snap  = { label="The snap",          w=2, dur=0, start=function()
                     local list = pd.all_chrs() or {}
                     if #list == 0 then error("no chrs") end
                     pd.fade(255, 255, 255, 200, 90)
                     for i, c in ipairs(list) do
                       if i % 2 == 0 then pd.chr_damage(c, 100) end
                     end end },
  plague       = { label="The plague",        w=3, dur=20,
                   tick=function(left)
                     if left % 120 == 0 then
                       for _, c in ipairs(pd.all_chrs() or {}) do
                         pd.chr_damage(c, 0.35)
                       end
                     end
                   end,
                   start=function() end },
  sepia        = { label="1964 mode",         w=4, dur=30,
                   start=function() pd.screen_tint(230, 190, 130) end,
                   stop=function() pd.screen_tint() end },
  terminal     = { label="Terminal green",    w=4, dur=30,
                   start=function() pd.screen_tint(110, 255, 130) end,
                   stop=function() pd.screen_tint() end },
  -- retro era pair: pixelate the frame + bitcrush the audio (device rate is
  -- 22kHz, so step 4 ~= 5.5kHz @ 8-bit and step 2 ~= 11kHz @ 10-bit)
  bit8         = { label="8-bit era",         w=3, dur=30,
                   start=function() pd.pixelate(160, 120, 4); pd.audio_crush(4, 8) end,
                   stop=function() pd.pixelate(); pd.audio_crush() end },
  bit16        = { label="16-bit era",        w=3, dur=30,
                   start=function() pd.pixelate(256, 192, 256); pd.audio_crush(2, 10) end,
                   stop=function() pd.pixelate(); pd.audio_crush() end },
  australia    = { label="Australia mode",    w=3, dur=20,
                   start=function() pd.upside_down(true) end,
                   stop=function() pd.upside_down(false) end },
  giants       = { label="Attack of the giants", w=3, dur=25,
                   start=function()
                     st.scaled_g = {}
                     for _, c in ipairs(pd.all_chrs() or {}) do
                       if pd.chr_scale(c, 1.6) then st.scaled_g[#st.scaled_g + 1] = c end
                     end
                     if #st.scaled_g == 0 then error("no chrs") end
                   end,
                   stop=function()
                     for _, c in ipairs(st.scaled_g or {}) do pd.chr_scale(c, 1 / 1.6) end
                     st.scaled_g = nil
                   end },
  ant_farm     = { label="Ant farm",          w=3, dur=25,
                   start=function()
                     st.scaled_a = {}
                     for _, c in ipairs(pd.all_chrs() or {}) do
                       if pd.chr_scale(c, 0.45) then st.scaled_a[#st.scaled_a + 1] = c end
                     end
                     if #st.scaled_a == 0 then error("no chrs") end
                   end,
                   stop=function()
                     for _, c in ipairs(st.scaled_a or {}) do pd.chr_scale(c, 1 / 0.45) end
                     st.scaled_a = nil
                   end },
  monsoon      = { label="Monsoon",           w=4, dur=30,
                   start=function() pd.weather(1, 2); st.weather_set = true end,
                   stop=function() pd.weather(0); st.weather_set = false end },
  pinball_wizard = { label="Pinball wizard",  w=4, dur=25,
                   -- rockets/grenade rounds launch as grenade-secondary
                   -- Proximity Pinballs: ballistic, bouncy, and they detonate
                   -- when ANYONE gets close — the shooter very much included
                   start=function() pd.pinball(true) end,
                   stop=function() pd.pinball(false) end },
  -- composite batch: pure-Lua combos over the existing hook surface
  nap_time     = { label="Nap time",          w=4, dur=0, start=function()
                     local n = 0
                     for _, c in ipairs(pd.all_chrs() or {}) do
                       if pd.chr_ko(c) then n = n + 1 end
                     end
                     if n == 0 then error("nobody to KO") end end },
  gun_game     = { label="Gun Game",          w=3, dur=60,
                   start=function()
                     st.gungame_idx = 1
                     pd.give_weapon(GUNS[1]); pd.switch_weapon(GUNS[1]); pd.refill_ammo()
                   end,
                   stop=function() st.gungame_idx = nil end },
  glass_cannon = { label="Glass cannons",     w=4, dur=15,
                   start=function() pd.damage_scale(8) end,
                   stop=function() pd.damage_scale(1) end },
  karma        = { label="Empath",            w=4, dur=20,
                   start=function() end }, -- reflect handled in the damage hook
  pinata       = { label="Pinata party",      w=4, dur=30,
                   start=function() end }, -- kill rewards handled in the kill hook
  clone_army   = { label="Clone army",        w=2, dur=0, start=function()
                     local held = pd.weapon_held()
                     local wpn = (held and held > 1) and held or W.FALCON2
                     for i = 0, 4 do
                       local a = i * 2 * math.pi / 5
                       pd.spawn_body(-1, wpn, math.sin(a) * 200, math.cos(a) * 200)
                     end end },
  musical_statues = { label="Musical statues", w=3, dur=21,
                   start=function()
                     pd.song(math.random(0, 255))
                     pd.chr_freeze(false)
                   end,
                   tick=function(left)
                     if left % 180 == 0 then pd.chr_freeze(false)
                     elseif left % 90 == 0 then pd.chr_freeze(true) end
                   end,
                   stop=function()
                     pd.chr_freeze(false)
                     pd.song()
                   end },
  full_flip    = { label="Full rotation",     w=2, dur=20,
                   start=function() pd.cheat(CHEAT.MIRROR, true); pd.upside_down(true) end,
                   stop=function() pd.cheat(CHEAT.MIRROR, false); pd.upside_down(false) end },
  personal_space = { label="Personal space",  w=3, dur=16,
                   start=function() end,
                   tick=function(left)
                     if left % 240 == 0 then
                       local list = pd.all_chrs() or {}
                       for i, c in ipairs(list) do
                         local a = (i / #list) * 2 * math.pi
                         pd.chr_summon(c, math.sin(a) * 250, math.cos(a) * 250)
                       end
                     end
                   end },
  shields_up   = { label="Shields up",        w=4, dur=0, start=function()
                     local list = pd.all_chrs() or {}
                     if #list == 0 then error("no chrs") end
                     for _, c in ipairs(list) do pd.chr_set_shield(c, 8) end end },
  fire_sale    = { label="Fire sale",         w=4, dur=0, start=function()
                     local list = pd.all_chrs() or {}
                     if #list == 0 then error("no chrs") end
                     for _, c in ipairs(list) do
                       pd.spawn_at_chr(c, GUNS[math.random(#GUNS)])
                     end end },
  quantum_instability = { label="Quantum instability", w=3, dur=20,
                   start=function() end,
                   tick=function(left)
                     if left % 300 == 0 then
                       local c = random_chr()
                       if c then pd.teleport_to_chr(c) end
                     end
                   end },
  motivator    = { label="Motivational speaker", w=3, dur=20,
                   start=function() end,
                   tick=function(left)
                     if left % 240 == 0 then
                       local lines = {
                         "YOU'RE DOING GREAT",
                         "believe in yourself",
                         "have you tried shooting them?",
                         "perfect agents hydrate",
                         "your K/D is a social construct",
                         "remember to stretch",
                       }
                       pd.hud_message(lines[math.random(#lines)])
                     end
                   end },
  assert_authority = { label="Assert Authority", w=3, dur=20,
                   -- every skeletal model drops into its bind pose; root
                   -- motion still applies, so T-posers glide around dominantly
                   start=function() pd.t_pose(true) end,
                   stop=function() pd.t_pose(false) end },
  woof_gas     = { label="WOOF GAS",          w=4, dur=30,
                   -- the Investigation nerve gas, anywhere: green env wash on
                   -- fog stages, coughing + hiss + damage every ~4s. The green
                   -- screen tint guarantees the look on stages with no fog env.
                   start=function()
                     pd.gas(true)
                     pd.screen_tint(120, 220, 110)
                   end,
                   stop=function()
                     pd.gas(false)
                     pd.screen_tint()
                   end },
  blizzard     = { label="Blizzard",          w=4, dur=30,
                   start=function() pd.weather(2, 2); st.weather_set = true end,
                   stop=function() pd.weather(0); st.weather_set = false end },
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
  st.duration[name] = nil
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
  -- dur may be a function for randomised durations (e.g. Take a break 10-30s)
  local dur = (type(e.dur) == "function") and e.dur() or e.dur
  if dur and dur > 0 then
    st.active[name] = dur * TICKS
    st.duration[name] = dur * TICKS
  end
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

-- Pick the 3 distinct effects chat votes on this window (weighted, history-
-- avoided, like the drumbeat). The Twitch/YouTube voting foundation: a bot
-- forwards chat "1"/"2"/"3" as `vote N` datagrams; the HUD shows the slate.
local function pick_candidates()
  st.candidates = {}
  st.cvotes = {0, 0, 0}
  local tries = 0
  while #st.candidates < 3 and tries < 60 do
    tries = tries + 1
    local name = pick_random()
    if name then
      local dup = false
      for _, c in ipairs(st.candidates) do if c == name then dup = true end end
      if not dup then st.candidates[#st.candidates + 1] = name end
    end
  end
end

-- ---------------------------------------------------- external protocol ----
function chaos.handle(source, text)
  local cmd, arg = text:match("^(%S+)%s*(.*)$")
  if not cmd then return end
  cmd = cmd:lower()
  if cmd == "on" then
    st.enabled = true; st.timer = st.interval * TICKS; persist(); announce("enabled")
    if st.votetime > 0 then st.votetimer = st.votetime * TICKS; pick_candidates() end
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
    persist()
    if st.votetime > 0 then pick_candidates() else st.candidates = {} end
    pd.log("[chaos] votetime = " .. st.votetime .. "s" .. (st.votetime == 0 and " (off)" or ""))
  elseif cmd == "trigger" then
    local name, who = arg:match("^(%S+)%s*(.*)$")
    chaos.trigger(name or "", who ~= "" and who or source)
  elseif cmd == "vote" then
    -- chat votes by slate number ("vote 1") or by candidate name; anything
    -- not on the current slate is ignored
    if st.votetime > 0 and #st.candidates > 0 then
      local slot = tonumber(arg:match("^(%d)"))
      if not slot then
        local name = arg:match("^(%S+)")
        for i, c in ipairs(st.candidates) do
          if c == name then slot = i end
        end
      end
      if slot and st.candidates[slot] then
        st.cvotes[slot] = (st.cvotes[slot] or 0) + 1
      end
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

  -- Advance on GAME time, not frames: lvupdate() is the ticks the sim
  -- actually ran this frame — 0 while paused (no pausing out a bad effect),
  -- scaled during slo-mo/boost. Everything below (effect timers, the vote
  -- window, the drumbeat) freezes with the game.
  local dt = pd.lvupdate and pd.lvupdate() or 1
  if not st.enabled or dt <= 0 then return end

  -- timed effect expiry (+ optional per-tick driver, e.g. disco's hue cycle)
  for name, left in pairs(st.active) do
    local e = chaos.effects[name]
    if e and e.tick then pcall(e.tick, left) end
    left = left - dt
    if left <= 0 then
      stop_effect(name)
      announce((e and e.label or name) .. " wore off")
    else
      st.active[name] = left
    end
  end

  -- vote mode: chat picks from the 3-candidate slate; the winner fires when
  -- the window closes (ties / no votes -> random candidate, chaos must flow).
  -- While voting is on it REPLACES the random drumbeat below.
  if st.votetime > 0 then
    if #st.candidates == 0 then pick_candidates() end
    st.votetimer = st.votetimer - dt
    if st.votetimer <= 0 then
      st.votetimer = st.votetime * TICKS
      local best, bestn = {}, -1
      for i = 1, #st.candidates do
        local n = st.cvotes[i] or 0
        if n > bestn then best, bestn = { i }, n
        elseif n == bestn then best[#best + 1] = i end
      end
      local slot = best[math.random(#best)]
      if slot then
        chaos.trigger(st.candidates[slot],
            bestn > 0 and ("chat vote x" .. bestn) or "no votes, dealer's choice")
      end
      pick_candidates()
    end
    return
  end

  -- the random drumbeat
  st.timer = st.timer - dt
  if st.timer <= 0 then
    st.timer = st.interval * TICKS
    local name = pick_random()
    if name then chaos.trigger(name) end
  end
end)

-- Misfire: the next shot fired after arming blows up in the player's face.
pd.on("weaponfire", function(weaponnum, playernum)
  if st.misfire_armed then
    st.misfire_armed = false
    pd.player_damage(1.5)
    pd.hud_message("CHAOS: BANG! It misfired!")
  end
end)

-- Vampire: damaging any chr while the effect is active feeds you.
-- Empath (karma): damaging any chr hurts you a little too.
pd.on("damage", function(chrnum, attackerplayernum, amount)
  if st.active.vampire and attackerplayernum == 0 then
    local h = pd.player_health()
    pd.player_set_health(math.min(1, h + 0.04))
  end
  if st.active.karma and attackerplayernum == 0 then
    pd.player_damage(0.3)
  end
end)

-- Gun Game: each kill advances to the next weapon in the list.
-- Pinata party: each kill bursts ammo + health.
pd.on("kill", function(chrnum, killerplayernum)
  if killerplayernum ~= 0 then return end
  if st.active.gun_game and st.gungame_idx then
    local cur = GUNS[st.gungame_idx]
    st.gungame_idx = st.gungame_idx + 1
    if st.gungame_idx > #GUNS then
      st.gungame_idx = 1
      pd.hud_message("CHAOS: GUN GAME COMPLETE!")
    else
      pd.hud_message(string.format("CHAOS: gun %d/%d", st.gungame_idx, #GUNS))
    end
    local nxt = GUNS[st.gungame_idx]
    if cur then pd.take_weapon(cur) end
    pd.give_weapon(nxt); pd.switch_weapon(nxt); pd.refill_ammo()
  end
  if st.active.pinata then
    pd.refill_ammo()
    local h = pd.player_health()
    if h then pd.player_set_health(math.min(1, h + 0.15)) end
  end
end)

pd.on("stage", function()
  -- fresh world: drop timed-effect bookkeeping (cheat banks reset with the
  -- stage; re-arm the timer so the first effect isn't instant)
  st.active = {}
  st.duration = {}
  st.cvotes = {0, 0, 0}
  st.timer = st.interval * TICKS
  st.votetimer = st.votetime * TICKS
  st.misfire_armed = false
  st.gungame_idx = nil
  -- the visual modes + ammo swap live in globals that SURVIVE the stage
  -- reload (unlike the cheat bank) — reset them explicitly
  if pd.flattex then pd.flattex(0) end
  if pd.grayscale then pd.grayscale(false) end
  if pd.shiny then pd.shiny(0) end
  if pd.room_tint then pd.room_tint() end
  if pd.ammo_swap then pd.ammo_swap() end
  if pd.backfire then pd.backfire(false) end
  if pd.aspect_scale then pd.aspect_scale(1) end
  if pd.fov_scale then pd.fov_scale(1) end
  if pd.song then pd.song() end
  if pd.one_punch then pd.one_punch(false) end
  if pd.gormless then pd.gormless(false) end
  if pd.sfx_shuffle then pd.sfx_shuffle(false) end
  if pd.instrument_shuffle then pd.instrument_shuffle(false) end
  -- request batch 4 globals
  if pd.gun_sound then pd.gun_sound() end
  if pd.damage_scale then pd.damage_scale(1) end
  if pd.paintball then pd.paintball(false) end
  if pd.weapon_jam then pd.weapon_jam(false) end
  if pd.player_freeze then pd.player_freeze(false) end
  if pd.chr_freeze then pd.chr_freeze(false) end
  if pd.no_drops then pd.no_drops(false) end
  if pd.mute then pd.mute(false) end
  if pd.zoom_scale then pd.zoom_scale(1) end
  -- freeform batch globals (weather only if WE turned it on — never kill a
  -- stage's own configured rain)
  if pd.chr_speed then pd.chr_speed(1) end
  if pd.screen_tint then pd.screen_tint() end
  if pd.pixelate then pd.pixelate() end
  if pd.audio_crush then pd.audio_crush() end
  if pd.upside_down then pd.upside_down(false) end
  if pd.gas then pd.gas(false) end
  if pd.t_pose then pd.t_pose(false) end
  if pd.pinball then pd.pinball(false) end
  if st.weather_set and pd.weather then pd.weather(0); st.weather_set = false end
  st.scaled_g = nil
  st.scaled_a = nil
end)

-- ---- HUD: active-effect timer bars + the chat-vote slate (top right) -------
-- Item-pickup-style bars: label, then a dark backing box with a filled
-- fraction that drains as the effect runs out. Below the bars, the 3-effect
-- vote slate + live counts + a window-countdown bar — the on-screen half of
-- the Twitch/YouTube voting foundation (chat sends `vote 1|2|3` via the UDP
-- ingress; this panel is what the streamer's viewers read).
local HUD_X, HUD_W = 232, 74
local C_TEXT, C_BAR, C_BARBG, C_VOTE = 0xffffffff, 0x40c0ffff, 0x00000090, 0xffe040ff

pd.on("draw", function()
  local y = 4

  -- active timed effects, stable order
  if next(st.active) ~= nil then
    local names = {}
    for name in pairs(st.active) do names[#names + 1] = name end
    table.sort(names)
    for i = 1, math.min(#names, 5) do
      local name = names[i]
      local e = chaos.effects[name]
      local left = st.active[name]
      local total = st.duration[name] or left
      local frac = (total > 0) and (left / total) or 0
      pd.draw_text(HUD_X, y, e and e.label or name, C_TEXT)
      pd.draw_box(HUD_X, y + 8, HUD_W, 4, C_BARBG)
      pd.draw_box(HUD_X, y + 8, math.max(1, math.floor(HUD_W * frac)), 4, C_BAR)
      y = y + 16
    end
  end

  -- vote slate
  if st.enabled and st.votetime > 0 and #st.candidates > 0 then
    y = y + 2
    pd.draw_text(HUD_X, y, "VOTE NEXT:", C_VOTE)
    y = y + 9
    for i = 1, #st.candidates do
      local e = chaos.effects[st.candidates[i]]
      pd.draw_text(HUD_X, y, string.format("%d %s (%d)", i,
          e and e.label or st.candidates[i], st.cvotes[i] or 0), C_TEXT)
      y = y + 9
    end
    local frac = st.votetimer / (st.votetime * TICKS)
    if frac < 0 then frac = 0 elseif frac > 1 then frac = 1 end
    pd.draw_box(HUD_X, y + 1, HUD_W, 3, C_BARBG)
    pd.draw_box(HUD_X, y + 1, math.max(1, math.floor(HUD_W * frac)), 3, C_VOTE)
  end
end)

if pd.menu_add then
  pd.menu_add("Chaos: toggle",      function() chaos.handle("menu", "toggle") end)
  pd.menu_add("Chaos: random now",  function() local n = pick_random(); if n then chaos.trigger(n, "menu") end end)
  pd.menu_add("Chaos: vote 30s on/off", function()
    chaos.handle("menu", st.votetime > 0 and "votetime 0" or "votetime 30")
  end)

  -- Test menu: every effect as its own pause-menu entry (the Lua Director
  -- dialog smooth-scrolls past one screen). Sorted by internal name so the
  -- list order is stable between sessions.
  local names = {}
  for name in pairs(chaos.effects) do names[#names + 1] = name end
  table.sort(names)
  for _, name in ipairs(names) do
    local n = name
    local e = chaos.effects[n]
    pd.menu_add("Test: " .. (e.label or n), function() chaos.trigger(n, "test") end)
  end
end

pd.log("chaos.lua loaded (" .. (st.enabled and "ENABLED" or "off") .. ") — /chaos on | /chaos list")
