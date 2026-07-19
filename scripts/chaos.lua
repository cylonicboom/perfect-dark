-- Chaos / randomiser mode (docs/PORT_CHAOS.md).
--
-- A timer picks a weighted random effect every `interval` seconds; timed
-- effects clean themselves up. Everything is driven through the pd.* native
-- hooks, so new effects are pure Lua — no rebuild.
--
-- Control surface (all reach chaos.handle(), in priority order):
--   ~ console:  /chaos on|off|toggle|status|list|interval N|effectdur N|
--               votetime N|trigger <effect>|vote <effect>|say <text>|seed N
--   UDP ingress: same verbs, one datagram each, to 127.0.0.1:<Chaos.EventPort>
--               (pd.ini [Chaos] EventPort=27110) — the Twitch/YouTube window:
--               point any chat bot / Streamer.bot / SAMMI action at it.
--   Lua:        chaos.trigger("mirror"), chaos.handle("udp", "vote panic") — so
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

-- DVD screensaver palette: bright RGBA words cycled on every wall bounce.
local DVD_COLS = { 0xff4040ff, 0x40ff40ff, 0x4080ffff, 0xffe040ff,
                   0xff40c0ff, 0x40ffe0ff, 0xff8020ff, 0xc060ffff }

-- CHAOS: <name> toast (weapon-pickup style, bottom-left): seconds fully shown,
-- then fade out. Rendered by the draw hook with a box sized to hug the text.
local TOAST_HOLD, TOAST_FADE = 3, 1
local TOAST_TICKS = (TOAST_HOLD + TOAST_FADE) * TICKS

-- ---------------------------------------------------------------- state ----
local st = {
  enabled  = (pd.persist_get and pd.persist_get("chaos_enabled") == "1") or false,
  -- frequency: seconds between random effects (default one every 20s)
  interval = tonumber(pd.persist_get and pd.persist_get("chaos_interval") or "") or 20,
  -- global effect duration: how long every timed effect runs (default 60s).
  -- Applied in chaos.trigger so all timed effects share one adjustable length;
  -- instant effects (dur 0) stay instant.
  effectdur = tonumber(pd.persist_get and pd.persist_get("chaos_effectdur") or "") or 60,
  votetime = tonumber(pd.persist_get and pd.persist_get("chaos_votetime") or "") or 0,
  timer    = 0,          -- ticks until the next random effect
  votetimer = 0,         -- ticks left in the current vote window
  candidates = {},       -- the 3 effects chat can vote on this window
  cvotes   = {0, 0, 0},  -- votes per candidate slot
  active   = {},         -- name -> ticks remaining (timed effects)
  duration = {},         -- name -> total ticks (for the HUD bars)
  cooldown = {},         -- name -> fires-until-recovery: a shown effect's pick
                         -- weight is suppressed, easing back to full only once
                         -- the whole enabled list has had a turn (anti-repeat deck)
  disabled = {},         -- name -> true for effects switched OFF in the menu
}

-- Load the disabled-effect set (comma-separated names) from persistence.
do
  local s = pd.persist_get and pd.persist_get("chaos_disabled") or ""
  for name in tostring(s):gmatch("[^,]+") do st.disabled[name] = true end
end

local function effect_enabled(name)
  return not st.disabled[name]
end

local function persist_disabled()
  if not pd.persist_set then return end
  local t = {}
  for name in pairs(st.disabled) do t[#t + 1] = name end
  table.sort(t)
  pd.persist_set("chaos_disabled", table.concat(t, ","))
end

local function persist()
  if pd.persist_set then
    pd.persist_set("chaos_enabled", st.enabled and "1" or "0")
    pd.persist_set("chaos_interval", tostring(st.interval))
    pd.persist_set("chaos_effectdur", tostring(st.effectdur))
    pd.persist_set("chaos_votetime", tostring(st.votetime))
  end
end

local function announce(text)
  -- Weapon-pickup-style toast in the bottom-left. Rendered by the draw hook
  -- with a box sized to HUG the text (the engine hudmsg box is a full
  -- line-height tall, leaving a gap below the letters). Held then faded.
  st.toast = { text = "CHAOS: " .. text, life = TOAST_TICKS }
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
local BODY = { MINISKEDAR=0x7b, SKEDAR=0x5c, THEKING=0x67, SKEDARKING=0x93,
               MRBLONDE=0x5b, DARKCOMBAT=0x56, DJBOND=0x00 }
local CHEAT = { FISTS=0, AMMO=4, NORELOAD=5, SLOMO=6, DK=7, SMALLJO=10, SMALLCHARS=11,
  ENEMYSHIELDS=12, JOSHIELD=13, SUPERSHIELD=14, TEAMHEADS=16, ELVIS=17,
  ENEMYROCKETS=18, MARQUIS=20, GOLDENEYE=45, WIREFRAME=46, MIRROR=47, TONAL=48 }

-- Non-gameplay stages where Chaos must stay dormant: the Carrington Institute
-- main-menu hub plus the title / boot / credits menus (src/include/constants.h).
-- Chaos only fires in real missions / Combat Sim. (STAGE_CITRAINING is the hub
-- the main menu is drawn over — it has a live player pawn, so the pawn check
-- alone won't catch it.)
local MENU_STAGES = {
  [0x26] = true, -- STAGE_CITRAINING (hub)
  [0x4e] = true, -- STAGE_TEST_OLD
  [0x5c] = true, -- STAGE_TITLE
  [0x5d] = true, -- STAGE_BOOTPAKMENU / STAGE_4MBMENU
  [0x5e] = true, -- STAGE_CREDITS
}

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

-- Equip a weapon and KEEP equipping it until it sticks. A switch_weapon in the
-- same tick as a take_weapon loses to take's cycle-back (bgunCycleBack beats
-- bgunEquipWeapon2) and the player is left holding nothing — the main tick
-- retries st.switch_want every frame for up to ~2s (stops on first success).
local function force_switch(w)
  pd.switch_weapon(w)
  st.switch_want = { weapon = w, ticks = 120 }
end

-- Ringtones: prefer a random ring1..ring6 (wav or mp3) from
-- scripts/sounds/chaos/, falling back to the original ring.wav/mp3. Drop
-- however many ringN files you like in the folder — missing slots just skip.
local function play_ring(loop)
  local first = math.random(6)
  for k = 0, 5 do
    local i = (first + k - 1) % 6 + 1
    if pd.play_file("scripts/sounds/chaos/ring" .. i .. ".wav", loop)
        or pd.play_file("scripts/sounds/chaos/ring" .. i .. ".mp3", loop) then
      return true
    end
  end
  return (pd.play_file("scripts/sounds/chaos/ring.wav", loop)
      or pd.play_file("scripts/sounds/chaos/ring.mp3", loop)) and true or false
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

-- Factory for the timed "arm every NPC" effects (K7 for all, enemy rockets,
-- weapon roulette). On start it snapshots each NPC's current weapon and hands
-- out a new one; on stop (timer end / clean restart) it gives the originals
-- back. `pick` is either a fixed weaponnum or a function()->weaponnum evaluated
-- per NPC (for the randomiser). Each returned effect owns its own `saved` table
-- so concurrent arm effects don't clobber each other. Needs pd.chr_weapon to
-- restore (degrades to give-only without it).
local function arm_all_effect(label, weight, pick)
  local saved = {}
  return {
    label = label, w = weight, dur = 1, -- dur>0 = timed; length is st.effectdur
    start = function()
      local list = pd.all_chrs() or {}
      if #list == 0 then error("no chrs") end
      saved = {}
      for _, c in ipairs(list) do
        saved[c] = pd.chr_weapon and pd.chr_weapon(c) or nil
        local w = (type(pick) == "function") and pick(c) or pick
        pd.chr_give_weapon(c, w)
      end
    end,
    stop = function()
      for c, w in pairs(saved) do
        if w and w >= 0 then pd.chr_give_weapon(c, w) end
      end
      saved = {}
    end,
  }
end

chaos.effects = {
  -- arsenal roulette
  arsenal      = { label="Free gun!",         w=10, dur=0, start=function()
                     local g = GUNS[math.random(#GUNS)]
                     pd.give_weapon(g); pd.switch_weapon(g); pd.refill_ammo() end },
  disarm       = { label="Butterfingers",     w=8,  dur=0, start=function()
                     local h = pd.weapon_held()
                     if h and h > W.UNARMED then pd.take_weapon(h) end end },
  knife_fight  = { label="Knife fight!",      w=5,  dur=20,
                   -- force the knife and lock out all other weapons (Cyclone-style)
                   start=function()
                     pd.give_weapon(W.KNIFE); pd.switch_weapon(W.KNIFE)
                     pd.give_ammo(0x09, 1) -- AMMOTYPE_KNIFE: one throwable knife
                     if pd.knife_lock then pd.knife_lock(true) end
                   end,
                   stop=function()
                     if pd.knife_lock then pd.knife_lock(false) end
                   end },
  ammo_rain    = { label="Ammo rain",         w=8,  dur=0, start=function() pd.refill_ammo() end },
  -- cheat-bank chaos (visual + gameplay)
  mirror       = setmetatable({ label="Mirror world",  w=8 }, {__index=cheat_effect(CHEAT.MIRROR, 30)}),
  tonal        = setmetatable({ label="Evil music",    w=6 }, {__index=cheat_effect(CHEAT.TONAL, 60)}),
  fists        = setmetatable({ label="Hurricane fists", w=6 }, {__index=cheat_effect(CHEAT.FISTS, 30)}),
  slomo        = setmetatable({ label="Slow motion",   w=6 }, {__index=cheat_effect(CHEAT.SLOMO, 12)}),
  dkmode       = setmetatable({ label="DK mode",       w=5 }, {__index=cheat_effect(CHEAT.DK, 45)}),
  smalljo      = setmetatable({ label="Tiny Jo",       w=4 }, {__index=cheat_effect(CHEAT.SMALLJO, 30)}),
  smallchars   = setmetatable({ label="Tiny everyone", w=4 }, {__index=cheat_effect(CHEAT.SMALLCHARS, 30)}),
  -- Queensberry rules: everyone melee only. Disarm every enemy (remembering
  -- their weapons), force the player to fists + lock weapon switching, and keep
  -- CHEAT_MARQUIS on so freshly-spawned guards are unarmed too. All restored on
  -- the timer: enemies get their guns back, the player can swap off unarmed.
  marquis      = { label="Marquis mode",  w=3, dur=20,
                   start=function()
                     st.marquis_saved = {}
                     local list = pd.all_chrs() or {}
                     for _, c in ipairs(list) do
                       if pd.chr_weapon then st.marquis_saved[c] = pd.chr_weapon(c) end
                       pd.chr_give_weapon(c, W.UNARMED) -- player pawn is skipped (NPC-only)
                     end
                     pd.cheat(CHEAT.MARQUIS, true)
                     pd.switch_weapon(W.UNARMED)
                     if pd.knife_lock then pd.knife_lock(true) end
                   end,
                   stop=function()
                     for c, w in pairs(st.marquis_saved or {}) do
                       if w and w > W.UNARMED then pd.chr_give_weapon(c, w) end
                     end
                     st.marquis_saved = {}
                     pd.cheat(CHEAT.MARQUIS, false)
                     if pd.knife_lock then pd.knife_lock(false) end
                   end },
  -- Enemies actually wield rocket launchers (and hand their guns back after),
  -- the K7-party mechanism rather than the projectile-swap cheat.
  enemyrockets = arm_all_effect("Enemy rockets!", 3, W.ROCKET),
  -- Give every current enemy a full shield, once. (CHEAT_ENEMYSHIELDS only
  -- shields chrs at spawn, so already-spawned guards got nothing.) Single
  -- activation — the shields stay; they're not taken back.
  enemyshields = { label="Shielded enemies", w=4, dur=0, start=function()
                     local list = pd.all_chrs() or {}
                     local n = 0
                     for _, c in ipairs(list) do
                       if pd.chr_set_shield(c, 8) then n = n + 1 end
                     end
                     if n == 0 then error("no chrs") end
                   end },
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
  blink        = { label="Blink",             w=5, fixeddur=true, dur=3,
                   -- flash white, hold 1s, then fade back to gameplay over 2s
                   start=function()
                     pd.fade(255, 255, 255, 255, 0)   -- flash to white + hold
                     st.blink_fade = false
                     st.blink_held = 0
                   end,
                   tick=function()
                     if not st.blink_fade then
                       st.blink_held = st.blink_held + (pd.lvupdate and pd.lvupdate() or 1)
                       if st.blink_held >= 60 then     -- 1s of game time held
                         st.blink_fade = true
                         pd.fade(255, 255, 255, 255, 120)   -- fade out over 2s
                       end
                     end
                   end,
                   stop=function() pd.fade(0, 0, 0, 0, 0) end },  -- ensure cleared
  -- ammo roulette (pd.ammo_swap: every held gun fires another weapon's
  -- primary rounds; refills keep the borrowed ammo topped up while active)
  rocket_rounds  = { label="Everything Rockets",   w=4, dur=20,
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
  -- (hurricane removed 2026-07-19 — superseded by hurricane2's repeated
  -- smaller gusts + storm weather.)
  cyclone_frenzy = { label="CYCLONE FRENZY",      w=3, dur=30,
                     start=function()
                       pd.dual_wield(W.CYCLONE, 1)    -- both hands, Magazine Discharge
                       pd.cheat(CHEAT.NORELOAD, true) -- unlimited ammo, no reloads
                       pd.refill_ammo()
                       -- force secondary + hold fire + no weapon switching
                       if pd.gun_lock then pd.gun_lock(true) end
                     end,
                     stop=function()
                       pd.cheat(CHEAT.NORELOAD, false)
                       if pd.gun_lock then pd.gun_lock(false) end
                       pd.take_weapon(W.CYCLONE) -- take the cyclones back
                     end },
  mag_dump       = { label="Mag Dump",            w=4, dur=20,
                     -- a single trigger tap empties the clip: autos hold fire,
                     -- semi-autos get a rapidly pulsed trigger (all in C).
                     start=function() if pd.mag_dump then pd.mag_dump(true) end end,
                     stop=function()  if pd.mag_dump then pd.mag_dump(false) end end },
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
  -- (Civil war removed — PD's player-centric guard AI made it unreliable and a
  -- source of mission softlocks. The pd.civil_war binding remains in C, unused.)
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
                       -- spawn ~1000 units away and let her hunt the player down
                       pd.spawn_body(-1, (held and held > 1) and held or W.FALCON2,
                                     math.sin(a) * 1000, math.cos(a) * 1000)
                     end },
  -- doors
  open_sesame    = { label="Open sesame",         w=4, dur=0,
                     start=function() pd.doors_all(true) end },
  lockdown       = { label="Lockdown",            w=3, fixeddur=true, dur=15,
                     -- actually LOCK every door shut for the duration (fake key
                     -- flag), not just the transient close of doors_all
                     start=function() pd.doors_lock(true) end,
                     stop=function() pd.doors_lock(false) end },
  body_snatch    = { label="BODY SNATCHED",       w=1, dur=1,
                     -- "Lite" takeover: take a guard's place (its weapon +
                     -- position), disguised so nobody aggros, for the effect
                     -- duration; then teleport back to Jo and it's business as
                     -- usual. (Full third-person Counter-Op body isn't buildable
                     -- mid-mission in solo.)
                     start=function()
                       local list = pd.all_chrs() or {}
                       if #list == 0 then error("no chrs") end
                       for _ = 1, 8 do
                         local c = list[math.random(#list)]
                         if c and pd.body_snatch(c) then    -- weapon+warp+disguise+remove guard
                           -- calm everyone so nobody aggros against the disguise
                           for _, o in ipairs(pd.all_chrs() or {}) do pd.chr_calm(o) end
                           return
                         end
                       end
                       error("no snatchable chr")
                     end,
                     tick=function(left)
                       -- keep guards passive while disguised (mop up any that
                       -- slipped through the target-search skip)
                       if left % 30 == 0 then
                         for _, o in ipairs(pd.all_chrs() or {}) do pd.chr_calm(o) end
                       end
                     end,
                     stop=function() if pd.body_unsnatch then pd.body_unsnatch() end end },
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
                       -- keep invincibility 3s longer (see st.sd_invuln in tick)
                       -- so lingering blasts can't kill as the effect wears off
                       st.sd_invuln = TICKS * 3
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
  turbo        = { label="GOTTA GO FAST",     w=6, fixeddur=true, dur=20,
                   -- just fast movement, no Combat Boost / bullet-time stim
                   start=function() pd.player_speed(2.5) end,
                   stop=function() pd.player_speed(1) end },
  drunk        = { label="One too many",      w=6, dur=20,
                   start=function()
                     pd.dizzy(3500)
                     if pd.double_vision then pd.double_vision(true) end
                   end,
                   tick=function(left)
                     -- keep the sway topped up so it lasts the whole effect
                     if left % 60 == 0 then pd.dizzy(3500) end
                   end,
                   stop=function()
                     if pd.double_vision then pd.double_vision(false) end
                   end },
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
  boom         = { label="Incoming!",         w=5, dur=0, start=function()
                     local c = random_chr(); if c then pd.explosion(c) end end },
  buddy        = { label="Backup arrives",    w=5, dur=0, start=function() pd.spawn_ally() end },
  reinforce    = { label="Supply drop",       w=4, dur=0, start=function()
                     local c = random_chr(); if c then pd.spawn_at_chr(c, GUNS[math.random(#GUNS)]) end end },
  -- request batch 4
  k7_party     = arm_all_effect("K7 Avengers for all", 4, W.K7),
  -- Every NPC gets a different random gun for the duration, then their own back.
  weapon_roulette = arm_all_effect("NPC weapon roulette", 3,
                     function() return GUNS[math.random(#GUNS)] end),
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
  take_a_break = { label="Take a break",      w=4, fixeddur=true, dur=function() return math.random(10, 30) end,
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
  freeze       = { label="FREEZE!",           w=4, fixeddur=true, dur=function() return math.random(10, 20) end,
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
                     if not play_ring(false) then
                       error("scripts/sounds/chaos/ring*.wav|mp3 missing")
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
  thanos_snap  = { label="The snap",          w=2, fixeddur=true, dur=3,
                   -- flash white + hold 1s + fade over 2s; each NPC has a 50/50
                   -- chance to be dusted (real coin flip, not every-other).
                   start=function()
                     local list = pd.all_chrs() or {}
                     if #list == 0 then error("no chrs") end
                     pd.fade(255, 255, 255, 255, 0)   -- flash to white + hold
                     st.snap_fade = false
                     st.snap_held = 0
                     for _, c in ipairs(list) do
                       if math.random() < 0.5 then pd.chr_damage(c, 100) end
                     end
                   end,
                   tick=function()
                     if not st.snap_fade then
                       st.snap_held = st.snap_held + (pd.lvupdate and pd.lvupdate() or 1)
                       if st.snap_held >= 60 then      -- 1s of game time held
                         st.snap_fade = true
                         pd.fade(255, 255, 255, 255, 120)   -- fade out over 2s
                       end
                     end
                   end,
                   stop=function() pd.fade(0, 0, 0, 0, 0) end },
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
                   start=function() pd.screen_tint(230, 190, 130); pd.audio_radio(true) end,
                   stop=function() pd.screen_tint(); pd.audio_radio(false) end },
  terminal     = { label="Terminal green",    w=4, dur=30,
                   start=function() pd.screen_tint(110, 255, 130) end,
                   stop=function() pd.screen_tint() end },
  -- Rainbow World: every texture on screen has its hue rotated in lockstep,
  -- cycling continuously (renderer colour mode 1004 — a luminance-preserving
  -- hue rotation animated by the shader's clock, so it costs nothing to run
  -- and needs no per-frame Lua). Whole frame incl. HUD.
  rainbow_world = { label="Rainbow World",     w=4, dur=25,
                   start=function() pd.pixelate(0, 0, 1004) end,
                   stop=function() pd.pixelate() end },
  -- Prismatic: like Rainbow World, but the hue-cycle RATE and DIRECTION vary
  -- across the screen (renderer mode 1005) — parts of the view spin their
  -- colours faster, slower, or backwards, so the whole scene shimmers out of
  -- sync. (Screen-space field, not literally per-texture — a post-filter
  -- can't tell one surface from another; colours flow over surfaces as you
  -- move, which suits the psychedelic look.)
  prismatic  = { label="Prismatic",           w=4, dur=25,
                   start=function() pd.pixelate(0, 0, 1005) end,
                   stop=function() pd.pixelate() end },
  -- retro era pair: pixelate the frame + bitcrush the audio (device rate is
  -- 22kHz, so step 4 ~= 5.5kHz @ 8-bit and step 2 ~= 11kHz @ 10-bit)
  bit8         = { label="8-bit era",         w=3, dur=30,
                   start=function() pd.pixelate(160, 120, 4); pd.audio_crush(4, 8) end,
                   stop=function() pd.pixelate(); pd.audio_crush() end },
  bit16        = { label="16-bit era",        w=3, dur=30,
                   start=function() pd.pixelate(256, 192, 256); pd.audio_crush(2, 10) end,
                   stop=function() pd.pixelate(); pd.audio_crush() end },
  gameboy      = { label="Handheld mode",     w=3, dur=30,
                   start=function() pd.pixelate(160, 144, 1001); pd.audio_crush(4, 8) end,
                   stop=function() pd.pixelate(); pd.audio_crush() end },
  -- post-filter looks (pd.crt/lens/screen_fx bits compose if two land at once)
  crt          = { label="Tube TV",           w=4, dur=30,
                   start=function() pd.crt(true) end,
                   stop=function() pd.crt(false) end },
  vhs          = { label="Camcorder",         w=4, dur=25,
                   start=function() pd.screen_fx(16, true) end,
                   stop=function() pd.screen_fx(16, false) end },
  peephole     = { label="Peephole",          w=3, dur=20,
                   start=function() pd.lens(1.4) end,
                   stop=function() pd.lens() end },
  underwater   = { label="Submerged",         w=3, dur=25,
                   start=function() pd.screen_fx(32, true); pd.audio_reverb(0.35) end,
                   stop=function() pd.screen_fx(32, false); pd.audio_reverb() end },
  negative     = { label="Film negative",     w=3, dur=20,
                   start=function() pd.pixelate(0, 0, 1000) end,
                   stop=function() pd.pixelate() end },
  thermal      = { label="Heat vision",       w=3, dur=20,
                   start=function() pd.pixelate(0, 0, 1002) end,
                   stop=function() pd.pixelate() end },
  -- audio-chain toys
  cathedral    = { label="Cathedral acoustics", w=4, dur=30,
                   start=function() pd.audio_reverb(0.8) end,
                   stop=function() pd.audio_reverb() end },
  reversed     = { label="!desreveR",         w=3, dur=20,
                   start=function() pd.audio_reverse(true) end,
                   stop=function() pd.audio_reverse(false) end },
  helium       = { label="Helium leak",       w=3, dur=20,
                   start=function() pd.audio_pitch(1.5) end,
                   stop=function() pd.audio_pitch() end },
  demon        = { label="Demonic presence",  w=2, dur=20,
                   start=function() pd.audio_pitch(0.65); pd.audio_reverb(0.5) end,
                   stop=function() pd.audio_pitch(); pd.audio_reverb() end },
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
  -- nap_time removed 2026-07-18: KO'ing whole stages proved too troublesome
  -- to debug (see PORT_CHAOS.md "Knockouts & Nap time" for the KO/wake
  -- mechanics). pd.chr_ko / pd.chr_wake remain available for scripting.
  -- (gun_game + gun_game2 removed 2026-07-19 — never worked reliably,
  -- retired rather than debugged further.)
  glass_cannon = { label="Glass cannons",     w=4, dur=15,
                   -- every gun fires a Gold Magnum (DY357-LX) one-shot-kill round,
                   -- then SHATTERS (removed from inventory — see the weaponfire
                   -- handler). A glass cannon: devastating once, then gone.
                   start=function() pd.ammo_swap(W.LX); pd.refill_ammo() end,
                   stop=function() pd.ammo_swap(); st.glass_pending = nil end },
  karma        = { label="Empath",            w=4, dur=20,
                   start=function() end }, -- reflect handled in the damage hook
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
                     for _, c in ipairs(list) do pd.chr_set_shield(c, 8) end
                     pd.player_set_shield(1) -- everyone, incl. the player
                   end },
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

-- ===================================================== CHAOS ALPHA ==========
-- Former new-suggestion testbed (2026-07-12 Discord batch). GRADUATED
-- 2026-07-19: everything here is now in the main rotation/vote slate (see the
-- registration loop after the table) — only image_test remains alpha-only.
-- New experimental effects can still land here first: mark them by name in
-- the registration loop to keep them out of the rotation while testing.
W.PSYCHOSIS = 0x2c
local CLASSICS = { 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x2b } -- PP9i..RCP45
local GOGGLES  = { W.NIGHTVISION, W.XRAY, W.IR, W.CLOAK }
local AMMO = { PSYCHOSIS=0x16, REMOTEMINE=0x0c, PROXYMINE=0x0d, TIMEDMINE=0x0e,
               MAGNUM=0x0a }

-- ---- interactive task library (EULA / CAPTCHA accept requirements) --------
-- A task is a little sensor the player must satisfy: press FIRE, fire real
-- shots, open a door, hold crouch, or spin a full circle. task_tick returns
-- true when complete; task_label renders the live instruction. The door /
-- crouch / spin kinds need the 2026-07-18 exe (pd.door_opens etc.) — the
-- pool builder only offers what the exe supports.
local function task_new(kind)
  local t = { kind = kind, prog = 0 }
  if kind == "door" then t.base = pd.door_opens() end
  if kind == "spin" then t.last = pd.player_yaw(); t.turned = 0 end
  if kind == "fire" then t.shots = 0 end
  if kind == "crouch" then t.held = 0 end
  return t
end

local function task_label(t)
  if t.kind == "press" then return "press FIRE to accept" end
  if t.kind == "fire" then return string.format("fire your weapon (%d/5)", t.shots or 0) end
  if t.kind == "door" then return "open a door to accept" end
  if t.kind == "crouch" then return string.format("hold crouch to accept (%d%%)", math.floor(math.min(1, t.prog) * 100)) end
  if t.kind == "spin" then return string.format("spin around to accept (%d%%)", math.floor(math.min(1, t.prog) * 100)) end
  return "?"
end

-- Advance the task one tick. Returns true when satisfied. "press" reads the
-- raw pad (works while pd.button_block keeps FIRE from shooting); "fire"
-- counts real shots via the weaponfire hook bumping t.shots.
local function task_tick(t)
  if t.kind == "press" then
    local pressed = pd.buttons_pressed()
    return pressed and (pressed & 0x2000) ~= 0
  elseif t.kind == "fire" then
    t.prog = (t.shots or 0) / 5
    return (t.shots or 0) >= 5
  elseif t.kind == "door" then
    return pd.door_opens() > t.base
  elseif t.kind == "crouch" then
    if pd.player_crouch() > 0 then t.held = (t.held or 0) + 1 end
    t.prog = (t.held or 0) / (3 * TICKS)
    return t.prog >= 1
  elseif t.kind == "spin" then
    local y = pd.player_yaw()
    local d = y - (t.last or y)
    if d > 180 then d = d - 360 elseif d < -180 then d = d + 360 end
    t.turned = (t.turned or 0) + math.abs(d)
    t.last = y
    t.prog = (t.turned or 0) / 360
    return t.prog >= 1
  end
  return false
end

-- Word-wrap text to a pixel width for the popup cards (fixes lines drawn
-- past the card edge). Falls back to ~40-char wraps when pd.text_size is
-- missing. Respects embedded newlines (CI bio text is paragraph-formatted).
local function wrap_lines(text, maxw)
  local out = {}
  for para in tostring(text):gmatch("[^\n]+") do
    local line = ""
    for word in para:gmatch("%S+") do
      local try = (line == "") and word or (line .. " " .. word)
      local w = pd.text_size and pd.text_size(try) or (#try * 5.4)
      if w > maxw and line ~= "" then
        out[#out + 1] = line
        line = word
      else
        line = try
      end
    end
    if line ~= "" then out[#out + 1] = line end
  end
  return out
end

-- Random task pool.
--   nofire  : exclude the live-shots task (EULA blocks the trigger, so
--             demanding real shots would soft-lock the page).
--   strict  : DELIBERATE actions only (fire + crouch). CAPTCHA uses this so
--             the check can't be satisfied incidentally — normal mouse-look
--             was completing the "spin" task and any nearby door the "door"
--             task, making it feel like looking/doors passed a "shoot" check.
local function task_random(nofire, strict)
  local pool = {}
  if not nofire then pool[#pool + 1] = "fire" end
  if pd.player_crouch then pool[#pool + 1] = "crouch" end
  if not strict then
    pool[#pool + 1] = "press"
    if pd.door_opens then pool[#pool + 1] = "door" end
    if pd.player_yaw then pool[#pool + 1] = "spin" end
  end
  if #pool == 0 then pool[1] = "press" end -- safety (nofire+strict+no crouch)
  return task_new(pool[math.random(#pool)])
end

local alpha_effects = {
  -- Hurricane v2: much smaller gust force, repeated through the effect, plus
  -- storm weather for the duration. (Faster weather animation needs C.)
  hurricane2 = { label="Hurricane v2", dur=1,
                 start=function() pd.weather(1, 2); st.weather_set = true; pd.gust(45) end,
                 tick=function(left) if left % 90 == 0 then pd.gust(45) end end,
                 stop=function() pd.weather(0); st.weather_set = false end },
  -- Blooper: Mario-Kart ink splats obstruct the view, fading out at the end.
  -- Blooper: black blood splats smeared across the view (the real WALLHITTEX
  -- blood-splat textures painted on the HUD, tinted black), fading out.
  blooper    = { label="Blooper", fixeddur=true, dur=8,
                 start=function()
                   st.a_bloop = {}
                   for i = 1, 9 do
                     -- ~4x bigger than the first pass (2x each dimension), so a
                     -- single splat can swallow a big chunk of the view
                     local w, h = math.random(120, 240), math.random(110, 220)
                     st.a_bloop[i] = { x = math.random(-60, 320) - w // 2,
                                       y = math.random(-40, 240) - h // 2,
                                       w = w, h = h,
                                       tex = 0x09 + math.random(0, 3) } -- WALLHITTEX_BLOOD1..4
                   end
                 end,
                 stop=function() st.a_bloop = nil end },
  -- Image loader test: loads scripts/images/test.png and cycles it through
  -- center / scroll / resize / spin (10s each) with a phase label. Proves the
  -- pd.load_image + pd.draw_image hook end to end. (Not a "real" effect.)
  image_test = { label="Image test", fixeddur=true, dur=40,
                 start=function()
                   if not pd.load_image then error("needs new exe") end
                   local h = pd.load_image("test.png")          -- 64x64 (reliable max)
                   if not h then error("scripts/images/test.png missing") end
                   st.a_imgtest = { handle = h, t = 0 }
                 end,
                 tick=function()
                   if st.a_imgtest then st.a_imgtest.t = st.a_imgtest.t + (pd.lvupdate and pd.lvupdate() or 1) end
                 end,
                 stop=function() st.a_imgtest = nil end },
  -- DVD screensaver meme: the logo bounces around the screen, changing colour
  -- on every wall hit. A perfect corner hit (both walls the same frame) is
  -- celebrated on-screen — and, like the meme, almost never happens.
  dvd = { label="DVD Screensaver", fixeddur=true, dur=45,
                 start=function()
                   if not pd.load_image then error("needs new exe") end
                   local h = pd.load_image("dvd.png")
                   if not h then error("scripts/images/dvd.png missing") end
                   -- 4x bigger (2x each dim): 32x14 source drawn as 64x28, so the
                   -- hitbox is hw=32, hh=14 to keep the bounce edge-accurate.
                   local hw, hh = 32, 14
                   local sx = (math.random(0, 1) == 0) and -1 or 1
                   local sy = (math.random(0, 1) == 0) and -1 or 1
                   st.a_dvd = { handle=h,
                                x=math.random(hw, 320 - hw), y=math.random(hh, 220 - hh),
                                vx=1.6 * sx, vy=1.15 * sy,
                                ci=1, hw=hw, hh=hh, corners=0, flash=0 }
                 end,
                 tick=function()
                   local d = st.a_dvd; if not d then return end
                   local dt = (pd.lvupdate and pd.lvupdate() or 1)
                   if d.flash > 0 then d.flash = d.flash - dt end
                   d.x = d.x + d.vx * dt
                   d.y = d.y + d.vy * dt
                   -- HUD 2D space is 320x220 (the native viewport), NOT 240.
                   local hitx, hity = false, false
                   if d.x < d.hw then d.x = d.hw; d.vx = -d.vx; hitx = true
                   elseif d.x > 320 - d.hw then d.x = 320 - d.hw; d.vx = -d.vx; hitx = true end
                   if d.y < d.hh then d.y = d.hh; d.vy = -d.vy; hity = true
                   elseif d.y > 220 - d.hh then d.y = 220 - d.hh; d.vy = -d.vy; hity = true end
                   if hitx or hity then d.ci = d.ci % #DVD_COLS + 1 end
                   if hitx and hity then d.corners = d.corners + 1; d.flash = 4 * TICKS end
                 end,
                 stop=function() st.a_dvd = nil end },
  -- HUDVD: the whole HUD comes apart — each element (health, crosshair, ammo,
  -- radar, messages, kill-feed) bounces DVD-style in its own random diagonal.
  -- Engine-side (G_HUDOFFSET_EXT); aim + hit-detection are untouched.
  hudvd = { label="HUDVD", fixeddur=true, dur=25,
                 start=function()
                   if not pd.hudvd then error("needs new exe") end
                   pd.hudvd(true)
                 end,
                 stop=function() if pd.hudvd then pd.hudvd(false) end end },
  -- Fake objective-complete toast (real green complete style, no prefix;
  -- doesn't actually complete anything).
  fake_objective = { label="Objective complete?", dur=0, start=function()
                   pd.hud_message(string.format("Objective %d complete", math.random(1, 5)), 1)
                 end },
  -- The pessimist spinoff: real RED "objective failed" style (hud type 2),
  -- no prefix. Also changes nothing.
  fake_objective_fail = { label="Objective failed?", dur=0, start=function()
                   pd.hud_message(string.format("Objective %d failed", math.random(1, 5)), 2)
                 end },
  -- (minefield_drops removed 2026-07-18 round 2 — superseded by booby_doors.)
  -- Booby-trapped DOORS: any door that starts opening detonates (C hook in
  -- doorSetMode). NPCs opening doors count too — chaos is an equal-
  -- opportunity employer.
  booby_doors = { label="Booby-trapped doors", dur=1,
                 start=function()
                   if not pd.door_traps then error("needs new exe") end
                   pd.door_traps(true)
                   pd.hud_message("CHAOS: do NOT touch the doors")
                 end,
                 stop=function() pd.door_traps(false) end },
  -- Martyrdom: anyone who dies drops a REAL live grenade at their feet
  -- (pd.grenade — engine handles the pin sound, fuse, and blast). See the
  -- kill hook.
  martyrdom  = { label="Martyrdom", dur=1, start=function() end },
  -- Psychosis Gun with a single dart.
  psychosis  = { label="Psychosis dart", dur=0, start=function()
                   pd.give_weapon(W.PSYCHOSIS); pd.give_ammo(AMMO.PSYCHOSIS, 1)
                   pd.switch_weapon(W.PSYCHOSIS)
                 end },
  -- Vertigo v2: same FOV sway at half the cycle rate.
  vertigo2   = { label="Vertigo v2", dur=1,
                 start=function() pd.fov_scale(1.2) end,
                 tick=function(left) pd.fov_scale(1 + 0.35 * math.sin(left / 24)) end,
                 stop=function() pd.fov_scale(1) end },
  -- Bayblade: every NPC spins like a top (~2 rev/s, engine-side yaw stomp —
  -- AI keeps fighting) while the beyblade clip plays. Drop
  -- scripts/sounds/chaos/beyblade.wav|mp3 in for the full "LET IT RIP";
  -- the spin works without it.
  bayblade   = { label="Bayblade!", dur=1,
                 start=function()
                   if not pd.beyblade then error("needs new exe") end
                   pd.beyblade(true)
                   local _ = pd.play_file("scripts/sounds/chaos/beyblade.wav")
                         or pd.play_file("scripts/sounds/chaos/beyblade.mp3")
                 end,
                 stop=function()
                   pd.beyblade(false)
                   if pd.stop_file then pd.stop_file() end
                 end },
  -- Speen: the PLAYER spins — view yaw whipped around at one revolution per
  -- second (aim and heading go with it; look input still adds on top). Runs
  -- 1s plus 1s per full 10s of the configured chaos effect time:
  -- effectdur 45 -> 1 + floor(45/10) = 5 seconds of speen.
  speen      = { label="Speen", fixeddur=true,
                 dur=function() return 1 + math.floor(st.effectdur / 10) end,
                 start=function()
                   if not pd.player_add_yaw then error("needs new exe") end
                   local _ = pd.play_file("scripts/sounds/chaos/speen.wav")
                         or pd.play_file("scripts/sounds/chaos/speen.mp3")
                 end,
                 tick=function(left)
                   local dt = pd.lvupdate and pd.lvupdate() or 1
                   pd.player_add_yaw(dt * 6) -- 6 deg/60Hz tick = 360 deg/s
                 end },
  -- Banana peel: you slip — dropped to a full squat, shoved forward a few
  -- map units (knockback physics, so walls stop the slide) and left staring
  -- at the ceiling. scripts/sounds/chaos/banana.wav|mp3 sells it.
  banana_peel = { label="Banana peel", dur=0,
                 start=function()
                   if not pd.player_slip or not pd.player_pitch then
                     error("needs new exe")
                   end
                   pd.player_slip(25) -- squat + shove; pitch glides below
                   -- glide the look-up over 0.3s (18 ticks) instead of a
                   -- snap — the main tick runs st.pitch_anim to completion
                   st.pitch_anim = { from = pd.player_pitch(), to = 65,
                                     t = 0, len = 18 }
                   local _ = pd.play_file("scripts/sounds/chaos/banana.wav")
                         or pd.play_file("scripts/sounds/chaos/banana.mp3")
                 end },
  -- Do a Barrel Roll: the whole view rolls through exactly ONE 360 (about a
  -- second), then rights itself. The Speen spiritual sibling, renderer-side
  -- (pd.screen_roll — HUD stays upright, dlcache gated off while rolling).
  barrel_roll = { label="Do a Barrel Roll", fixeddur=true, dur=1,
                 start=function()
                   if not pd.screen_roll then error("needs new exe") end
                   st.a_roll = 0
                   -- the clip likely outlives the 1s roll — let it play out
                   -- (no stop_file in stop), it's the whole joke
                   local _ = pd.play_file("scripts/sounds/chaos/barrelroll.wav")
                         or pd.play_file("scripts/sounds/chaos/barrelroll.mp3")
                 end,
                 tick=function(left)
                   local dt = pd.lvupdate and pd.lvupdate() or 1
                   st.a_roll = math.min(360, (st.a_roll or 0) + dt * 6)
                   pd.screen_roll(st.a_roll) -- 360 == upright, lands clean
                 end,
                 stop=function()
                   st.a_roll = nil
                   pd.screen_roll(0)
                 end },
  -- A live grenade lands at your feet: a REAL armed grenade (pd.grenade —
  -- engine plays the pin/throw sound and detonates it on its own fuse).
  hot_potato = { label="Live grenade!", dur=0,
                 start=function()
                   if not pd.grenade then error("needs new exe") end
                   local x, y, z = pd.player_pos(0)
                   if not x then error("no player") end
                   if not pd.grenade(x, y, z) then error("couldn't drop it") end
                   pd.hud_message("CHAOS: grenade out! (it's at your feet)")
                 end },
  -- Terminator: the DJ Bond tuxedo body in SUNGLASSES with a shotgun and an
  -- absurd shield, spawned far away (1200u). No timer — it comes for you and
  -- keeps coming.
  terminator = { label="Terminator", dur=0,
                 start=function()
                   local a = math.random() * 2 * math.pi
                   local d = 1200 -- far spawn (matches Skedar+Reaper / Terminator)
                   local c = pd.spawn_body(BODY.DJBOND, W.SHOTGUN,
                                           math.sin(a) * d, math.cos(a) * d,
                                           true) -- sunglasses
                   if not c or c < 0 then error("no room for him here") end
                   pd.chr_set_shield(c, 30)
                   pd.chr_alert(c)
                 end },
  -- Two-handed: EVERY weapon is forced dual-wield. Whatever you switch to
  -- gets re-dual-wielded on the next tick, so there's no single-handed
  -- option left for anything.
  two_handed = { label="Two-handed", dur=1,
                 start=function()
                   local h = pd.weapon_held()
                   if not h or h <= W.UNARMED then h = W.FALCON2 end
                   st.a_twoh = { cur = h }
                   pd.dual_wield(h)
                   pd.refill_ammo()
                 end,
                 tick=function()
                   local t = st.a_twoh
                   if not t then return end
                   local h = pd.weapon_held()
                   -- re-dual-wield only real guns, and only when the held gun
                   -- actually changes (dual_wield each frame would fight input)
                   if h and h > W.UNARMED and h ~= W.KNIFE and h ~= t.cur then
                     t.cur = h
                     pd.dual_wield(h)
                     pd.refill_ammo()
                   end
                 end,
                 stop=function()
                   local h = st.a_twoh and st.a_twoh.cur
                   if h then pd.take_weapon(h); pd.give_weapon(h); pd.switch_weapon(h) end
                   st.a_twoh = nil
                 end },
  double_lx  = { label="Double Magnum LX", dur=1,
                 start=function() pd.dual_wield(W.LX); pd.refill_ammo() end,
                 stop=function() pd.take_weapon(W.LX) end },
  -- Tank: dual rocket launchers, barely able to walk.
  tank       = { label="Tank mode", dur=1,
                 start=function()
                   pd.dual_wield(W.ROCKET); pd.refill_ammo()
                   pd.player_speed(0.25)
                 end,
                 stop=function()
                   pd.player_speed(1)
                   pd.take_weapon(W.ROCKET)
                 end },
  -- Mediguns: picking up any weapon heals a chunk (see weaponfound hook).
  mediguns   = { label="Mediguns", dur=1, start=function()
                   pd.hud_message("CHAOS: weapon pickups heal you")
                 end },
  -- Enemy LTK: any hit that costs you health finishes the job. Your own guns
  -- behave normally. (Don't run with Vampire — the drain counts as a hit.)
  enemy_ltk  = { label="Enemy LTK", dur=1,
                 start=function() st.a_ltk = pd.player_health() end,
                 tick=function()
                   local h = pd.player_health()
                   if h and h > 0 and st.a_ltk and h < st.a_ltk - 0.005 then
                     pd.player_damage(100)
                   end
                   st.a_ltk = h
                 end,
                 stop=function() st.a_ltk = nil end },
  -- SPEED: a pedometer appears. Cover the distance before the timer or boom.
  speed      = { label="SPEED", fixeddur=true, dur=30,
                 start=function()
                   st.a_run = { need = 3000, done = 0 }
                   pd.hud_message("CHAOS: RUN OR EXPLODE")
                 end,
                 tick=function(left)
                   local r = st.a_run
                   if not r then return end
                   local x, y, z = pd.player_pos(0)
                   if x and r.x then
                     local dx, dz = x - r.x, z - r.z
                     r.done = r.done + math.sqrt(dx * dx + dz * dz)
                   end
                   r.x, r.z = x, z
                   if left <= 10 and not r.fired then
                     r.fired = true
                     if r.done < r.need then
                       pd.explosions_around(true); st.a_boom_off = 12
                       pd.hud_message("CHAOS: TOO SLOW")
                     else
                       pd.hud_message("CHAOS: fast enough. this time.")
                     end
                   end
                 end,
                 stop=function() st.a_run = nil end },
  -- Ominous countdown: the klaxon wails for the duration... and this time
  -- something actually happens at zero — one random calamity from the payoff
  -- table (including, occasionally, nothing at all: the dread must stay
  -- honest). (nobar: no HUD timer bar — you don't get to know when.)
  countdown  = { label="Ominous countdown", fixeddur=true, dur=20, nobar=true,
                 start=function()
                   st.a_cd = { fired = false }
                   pd.alarm(true)
                 end,
                 tick=function(left)
                   local cd = st.a_cd
                   if not cd or cd.fired then return end
                   if left <= 8 then -- last few ticks = zero (dt can be >1)
                     cd.fired = true
                     -- weighted payoffs: the explosion barrage is a VERY small
                     -- chance (1/20) — the rest of the deck carries the dread
                     local payoffs = {
                       { w = 1, fn = function() -- rolling barrage (RARE)
                         pd.hud_message("CHAOS: INCOMING!")
                         if pd.explosions_around then
                           pd.explosions_around(true)
                           st.a_boom_off = 2 * TICKS -- main tick shuts it off
                         else
                           pd.explosion()
                         end
                       end },
                       { w = 3, fn = function() -- N-bomb on your position
                         pd.hud_message("CHAOS: package delivered")
                         pd.nbomb()
                       end },
                       { w = 4, fn = function() -- gale-force blast
                         pd.hud_message("CHAOS: storm front")
                         pd.gust(150)
                       end },
                       { w = 5, fn = function() -- every guard on the map hears it
                         pd.hud_message("CHAOS: they all heard that")
                         for _, c in ipairs(pd.all_chrs() or {}) do pd.chr_alert(c) end
                       end },
                       { w = 3, fn = function() -- something big lands nearby
                         pd.hud_message("CHAOS: something landed")
                         for i = 1, 2 do
                           local a = math.random() * 2 * math.pi
                           pd.spawn_body(BODY.MINISKEDAR, -1,
                                         math.sin(a) * 250, math.cos(a) * 250)
                         end
                       end },
                       { w = 4, fn = function() -- ...anticlimax
                         pd.hud_message("CHAOS: ...false alarm. this time.")
                       end },
                     }
                     local total = 0
                     for _, p in ipairs(payoffs) do total = total + p.w end
                     local roll = math.random(total)
                     for _, p in ipairs(payoffs) do
                       roll = roll - p.w
                       if roll <= 0 then p.fn(); break end
                     end
                   end
                 end,
                 stop=function() pd.alarm(false); st.a_cd = nil end },
  -- CAPTCHA: prove you're human — a random verification task (shots, door,
  -- crouch, spin, or just pressing FIRE). Complete it and the window closes;
  -- run out of time and the failed check hurts.
  captcha    = { label="CAPTCHA", fixeddur=true, dur=15,
                 start=function()
                   if not pd.buttons_pressed then error("needs new exe") end
                   st.a_cap = { task = task_random(false, true) } -- deliberate-only
                 end,
                 tick=function(left)
                   local c = st.a_cap
                   if not c or c.done then return end
                   if task_tick(c.task) then
                     c.done = true
                     pd.hud_message("CHAOS: verified human")
                     return true -- close the window early
                   elseif left <= 10 then
                     c.done = true
                     pd.player_damage(4)
                     pd.hud_message("CHAOS: CAPTCHA failed. beep boop.")
                     return true
                   end
                 end,
                 stop=function() st.a_cap = nil end },
  -- (skedar_king removed 2026-07-18 round 2 — "does not work for now".)
  -- (sea_groans removed 2026-07-18: needed an external sound file that never
  -- shipped; "does not work, just remove it".)
  -- Feeling lucky? All weapons gone, have a Magnum. 20% it's the LX.
  feeling_lucky = { label="Feeling lucky?", dur=0, start=function()
                   for _, g in ipairs(GUNS) do pd.take_weapon(g) end
                   pd.take_weapon(W.KNIFE)
                   local w = (math.random() < 0.2) and W.LX or W.MAGNUM
                   pd.give_weapon(w); force_switch(w); pd.refill_ammo()
                 end },
  -- Russian roulette: you're handed a Magnum with exactly ONE round, weapon
  -- switching locks, and the effect waits until you pull the trigger — the
  -- weaponfire hook resolves the spin (1-in-6 it's yours; otherwise someone
  -- else eats it and you get a little health back for your nerve). Stall too
  -- long and the gun gets impatient.
  russian_roulette = { label="Russian roulette", fixeddur=true, dur=45,
                 start=function()
                   st.a_rr = { fired = false }
                   pd.strip_ammo()
                   pd.give_weapon(W.MAGNUM)
                   pd.give_ammo(AMMO.MAGNUM, 1)
                   force_switch(W.MAGNUM)
                   if pd.knife_lock then pd.knife_lock(true) end
                   pd.hud_message("CHAOS: six chambers. one round. FIRE.")
                 end,
                 tick=function(left)
                   local rr = st.a_rr
                   if rr and not rr.fired and left <= 10 then
                     rr.fired = true -- time's up: the gun goes off by itself
                     pd.hud_message("CHAOS: too slow. it fired itself")
                     if math.random(6) == 1 then pd.player_damage(100) end
                     return true
                   end
                 end,
                 stop=function()
                   if pd.knife_lock then pd.knife_lock(false) end
                   st.a_rr = nil
                 end },
  -- Fake game over screen (draw hook paints it; audio ducks for the bit).
  -- Game over: the REAL mission-failed screen (a red DANGER dialog over the
  -- paused mission). Accept restarts the mission; Decline resumes right where
  -- you were. Solo/co-op only.
  game_over  = { label="Game over?", dur=0,
                 start=function()
                   if not pd.game_over then error("needs new exe") end
                   if not pd.game_over() then error("can't open it here") end
                 end },
  -- The real full-size Skedar (BODY_SKEDAR — its own model, skeleton and
  -- collision, NOT an upscaled mini, so it doesn't float) with a Reaper,
  -- spawned far across the map (1200u). No timer — it hunts you until it
  -- (or you) is dead. The C spawn now force-loads the skedar model file so
  -- it works on non-Skedar stages too.
  skedar_reaper = { label="Skedar with a Reaper", dur=0, start=function()
                   local a = math.random() * 2 * math.pi
                   local d = 1200 -- far spawn (matches Terminator)
                   local c = pd.spawn_body(BODY.SKEDAR, W.REAPER,
                                           math.sin(a) * d, math.cos(a) * d)
                   if not c or c < 0 then error("no room / skedar model unavailable") end
                   pd.chr_alert(c)
                 end },
  -- Classic chaos: EVERYONE's weapon is swapped for its GE-era classic
  -- EQUIVALENT (pistol->pistol, SMG->SMG, rifle->rifle, heavy->RC-P45),
  -- and everything — NPC guns AND your full arsenal — comes back when the
  -- timer ends.
  classic_weapons = { label="Classic weapons", dur=1,
                 start=function()
                   -- modern -> classic equivalent (0x24 PP9i pistol, 0x25 CC13
                   -- pistol, 0x26 KL01313 SMG, 0x27 KF7 rifle, 0x28 ZZT SMG,
                   -- 0x29 DMC SMG, 0x2a AR53 rifle, 0x2b RC-P45)
                   local MAP = {
                     [W.FALCON2] = 0x24, [W.MAGSEC] = 0x26, [W.MAULER] = 0x24,
                     [W.PHOENIX] = 0x25, [W.MAGNUM] = 0x25, [W.LX] = 0x25,
                     [W.CMP150] = 0x27, [W.CYCLONE] = 0x28, [W.LAPTOP] = 0x29,
                     [W.DRAGON] = 0x29, [W.K7] = 0x2a, [W.AR34] = 0x2a,
                     [W.SUPERDRAGON] = 0x2a, [W.SHOTGUN] = 0x2b,
                     [W.REAPER] = 0x2b, [W.SNIPER] = 0x27, [W.FARSIGHT] = 0x2b,
                     [W.DEVASTATOR] = 0x2b, [W.ROCKET] = 0x2b, [W.SLAYER] = 0x2b,
                     [W.CROSSBOW] = 0x26, [W.TRANQ] = 0x24,
                     [0x03] = 0x24, [0x04] = 0x24, -- Falcon 2 Silencer / Scope
                     [W.LASER] = 0x25, [W.PSYCHOSIS] = 0x24,
                   }
                   -- The player sweep needs its own COMPLETE list: GUNS is the
                   -- arsenal-roulette pool and misses holdable guns — the
                   -- Falcon 2 Silencer/Scope variants (the usual mission
                   -- loadout!), the LX, the Laser and the Psychosis Gun — so
                   -- those survived the swap. Grenades are deliberately NOT
                   -- swapped (they're classic-era-authentic; trading them for
                   -- a random gun was a bug, not a feature).
                   local SWAP = { 0x03, 0x04, W.LX, W.LASER, W.PSYCHOSIS }
                   for _, g in ipairs(GUNS) do
                     if g ~= W.GRENADE then SWAP[#SWAP + 1] = g end
                   end
                   -- NPCs: swap each chr's gun for its equivalent, remember
                   -- the original for the restore pass.
                   st.a_classic = { chrs = {}, mine = {} }
                   for _, c in ipairs(pd.all_chrs() or {}) do
                     local w = pd.chr_weapon and pd.chr_weapon(c) or nil
                     st.a_classic.chrs[c] = w
                     pd.chr_give_weapon(c, (w and MAP[w]) or CLASSICS[math.random(#CLASSICS)])
                   end
                   -- Player: snapshot the whole arsenal, trade every gun for
                   -- its classic equivalent (deduped).
                   local given, first = {}, nil
                   for _, g in ipairs(SWAP) do
                     if (pd.has_weapon and pd.has_weapon(g)) then
                       st.a_classic.mine[#st.a_classic.mine + 1] = g
                       pd.take_weapon(g)
                       local eq = MAP[g] or CLASSICS[math.random(#CLASSICS)]
                       if not given[eq] then
                         given[eq] = true
                         pd.give_weapon(eq)
                         first = first or eq
                       end
                     end
                   end
                   if not first then -- unarmed players still get a classic
                     first = CLASSICS[math.random(#CLASSICS)]
                     pd.give_weapon(first)
                     given[first] = true
                   end
                   st.a_classic.given = given
                   -- force_switch: the take_weapon cycle-back above would eat
                   -- a same-tick equip, leaving the player empty-handed
                   force_switch(first); pd.refill_ammo()
                 end,
                 stop=function()
                   local cl = st.a_classic
                   if cl then
                     for c, w in pairs(cl.chrs or {}) do
                       if w and w >= 0 then pd.chr_give_weapon(c, w) end
                     end
                     for eq in pairs(cl.given or {}) do pd.take_weapon(eq) end
                     local back = nil
                     for _, g in ipairs(cl.mine or {}) do
                       pd.give_weapon(g); back = back or g
                     end
                     if back then force_switch(back); pd.refill_ammo() end
                   end
                   st.a_classic = nil
                 end },
  -- One of each mine.
  mine_trio  = { label="Mine, mine, mine", dur=0, start=function()
                   pd.give_weapon(W.TIMEDMINE);  pd.give_ammo(AMMO.TIMEDMINE, 1)
                   pd.give_weapon(W.PROXYMINE);  pd.give_ammo(AMMO.PROXYMINE, 1)
                   pd.give_weapon(W.REMOTEMINE); pd.give_ammo(AMMO.REMOTEMINE, 1)
                 end },
  -- Quad(-ish) laser: dual lasers is as quad as two hands get.
  quad_laser = { label="Quad(-ish) laser", dur=1,
                 start=function() pd.dual_wield(W.LASER) end,
                 stop=function() pd.take_weapon(W.LASER) end },
  -- Estus flask: rooted to the spot while ~60% health sips back in.
  estus      = { label="Estus flask", fixeddur=true, dur=8,
                 start=function()
                   pd.player_speed(0.05)
                   local _ = pd.play_file("scripts/sounds/chaos/estus.wav")
                         or pd.play_file("scripts/sounds/chaos/estus.mp3")
                 end,
                 tick=function(left)
                   if left % 30 == 0 then
                     pd.player_set_health(math.min(1, pd.player_health() + 0.045))
                   end
                 end,
                 stop=function() pd.player_speed(1) end },
  -- A random pair of goggles appears in your inventory.
  new_glasses = { label="New glasses", dur=0, start=function()
                   pd.give_weapon(GOGGLES[math.random(#GOGGLES)])
                 end },
  -- Teen angst: mid-2000s emo energy, one line at a time. (Original pastiche
  -- lines in the style — not actual song lyrics.)
  teen_angst = { label="Teen angst", dur=1,
                 start=function() st.a_angst = 0 end,
                 tick=function(left)
                   if left % 300 == 0 then
                     local lines = {
                       "nobody understands this loadout",
                       "my heart is a locked door and no key spawns",
                       "the darkness in me is darker than this stage",
                       "they told me to smile more. i equipped the Reaper",
                       "this isn't a phase. it's a mission objective",
                       "rain on the window. respawn screen of the soul",
                     }
                     st.a_angst = (st.a_angst or 0) % #lines + 1
                     pd.hud_message(lines[st.a_angst])
                   end
                 end,
                 stop=function() st.a_angst = nil end },
  -- ===== batch 2: effects backed by the new C bindings (2026-07-12) =====
  -- (secondaries_only removed 2026-07-19 per user.)
  -- XBLA mode: 45% stick deadzone, autoaim on, massive reverb. Xbox Live
  -- Arcade nostalgia at its most authentic.
  xbla_mode  = { label="XBLA mode", dur=1,
                 start=function()
                   if not pd.autoaim then error("needs new exe") end
                   pd.autoaim(true)
                   pd.deadzone(0.45)
                   pd.audio_reverb(0.6)
                 end,
                 stop=function()
                   pd.autoaim(false)
                   pd.deadzone(0)
                   pd.audio_reverb()
                 end },
  -- Gun jam v2: pulls sometimes dry-fire, and a shot that DOES fire jams the
  -- rest of the mag — reload after every bang.
  gun_jam2   = { label="Weapon jam v2", dur=1,
                 start=function()
                   if not pd.weapon_jam then error("needs new exe") end
                   pd.weapon_jam(2)
                 end,
                 stop=function() pd.weapon_jam(false) end },
  -- Inflated bullets: ammo costs twice as much per shot.
  inflated_bullets = { label="Inflated bullets", dur=1,
                 start=function()
                   if not pd.ammo_cost then error("needs new exe") end
                   pd.ammo_cost(2)
                 end,
                 stop=function() pd.ammo_cost(1) end },
  -- Objective scramble: EVERY live objective's status gets randomised for
  -- the duration — done ones read incomplete, undone ones read complete —
  -- then everything snaps back to the truth. (Old version only flipped one
  -- already-completed objective, so early in a mission it just errored.)
  objective_scramble = { label="Objective scramble", dur=1,
                 start=function()
                   if not pd.objective_status then error("needs new exe") end
                   local live = {}
                   for i = 0, 9 do
                     if pd.objective_status(i) ~= -1 then live[#live + 1] = i end
                   end
                   if #live == 0 then error("no objectives on this stage") end
                   st.a_objf = true
                   for _, i in ipairs(live) do
                     pd.objective_force(i, math.random(2)) -- 1 = force incomplete, 2 = force complete
                   end
                   pd.hud_message("CHAOS: objectives scrambled. probably fine")
                 end,
                 stop=function()
                   if st.a_objf then
                     pd.objective_force(-1, 0) -- clear every override
                     pd.hud_message("CHAOS: objectives restored")
                     st.a_objf = nil
                   end
                 end },
  -- Nitroglycerin: any object destroyed goes up like the Crash Site ship.
  nitroglycerin = { label="Nitroglycerin", dur=1,
                 start=function()
                   if not pd.nitro then error("needs new exe") end
                   pd.nitro(true)
                 end,
                 stop=function() pd.nitro(false) end },
  -- TP to mission start (the start point is auto-marked each stage; see tick).
  tp_start   = { label="Back to the start", dur=0, start=function()
                   if not pd.warp_home then error("needs new exe") end
                   if not pd.warp_home() then error("no start recorded yet") end
                 end },
  -- Button thief: inputs vanish ONE BY ONE — every quarter of the countdown
  -- another button joins the stolen pile (up to four gone by the end).
  button_thief = { label="Button thief", dur=1,
                 start=function()
                   if not pd.button_block then error("needs new exe") end
                   -- Steal only buttons that are actually BOUND on the device
                   -- in hand (pd.input_source), so every theft is felt:
                   --   kbm: FIRE=LMB(Z), AIM=RMB(R), INTERACT=E(B button),
                   --        RELOAD=R-key(ext X 0x0040), wheel=Y 0x0080,
                   --        WASD = the C-buttons.
                   --   pad: FIRE=RT(Z), AIM=LT(R), INTERACT=south(A button),
                   --        RELOAD=west(X), NEXT WEAPON=north(Y); movement is
                   --        the analog stick (not stealable via buttons), and
                   --        B 0x4000 has no pad bind — skip both.
                   -- (The old deck stole A on kbm and B on pad — neither is
                   -- bound there, so those thefts changed nothing in-game.)
                   local kbm = (not pd.input_source) or pd.input_source() == "kbm"
                   local BTNS = {
                     { 0x2000, "FIRE" },
                     { 0x0010, "AIM" },
                     { 0x0040, "RELOAD" },
                     { 0x0080, "NEXT WEAPON" },
                     kbm and { 0x4000, "INTERACT" } or { 0x8000, "INTERACT" },
                   }
                   if kbm then
                     local MOVE = {
                       { 0x0008, "FORWARD" },     -- C-up  = W
                       { 0x0004, "BACKWARD" },    -- C-down = S
                       { 0x0002, "STRAFE LEFT" }, -- C-left = A
                       { 0x0001, "STRAFE RIGHT" },-- C-right = D
                     }
                     for _, b in ipairs(MOVE) do BTNS[#BTNS + 1] = b end
                   end
                   -- shuffle a private deck, steal the first one now
                   for i = #BTNS, 2, -1 do
                     local j = math.random(i)
                     BTNS[i], BTNS[j] = BTNS[j], BTNS[i]
                   end
                   st.a_thief = { deck = BTNS, stolen = 1, mask = BTNS[1][1] }
                   pd.button_block(st.a_thief.mask)
                   pd.hud_message("CHAOS: stole your " .. BTNS[1][2] .. " button")
                 end,
                 tick=function(left)
                   local th = st.a_thief
                   if not th then return end
                   th.total = th.total or left -- first tick = full duration
                   -- one more theft per elapsed quarter, capped at 4 buttons
                   local want = math.min(4, 1 + math.floor((1 - left / th.total) * 4))
                   while th.stolen < want do
                     th.stolen = th.stolen + 1
                     local b = th.deck[th.stolen]
                     th.mask = th.mask | b[1]
                     pd.button_block(th.mask)
                     pd.hud_message("CHAOS: also stole " .. b[2])
                   end
                 end,
                 stop=function() st.a_thief = nil; pd.button_block(0) end },
  -- Perfect hills: extreme fog rolls in (values are per-mille of the z-range;
  -- stock stages sit ~950..1050 — tune here).
  perfect_hills = { label="Perfect hills", dur=1,
                 start=function()
                   if not pd.fog then error("needs new exe") end
                   pd.fog(500, 850, 190, 195, 205)
                 end,
                 stop=function() pd.fog() end },
  -- Max blood: every hit erupts, wounded guards drip at the maximum rate.
  max_blood  = { label="Max blood", dur=1,
                 start=function()
                   if not pd.max_blood then error("needs new exe") end
                   pd.max_blood(true)
                 end,
                 stop=function() pd.max_blood(false) end },
  -- Technicolor blood: everyone bleeds a cycling rainbow.
  blood_rainbow = { label="Technicolor blood", dur=1,
                 start=function()
                   if not pd.blood_colour then error("needs new exe") end
                   pd.blood_colour(hsv(math.random(0, 359)))
                 end,
                 tick=function(left)
                   if left % 30 == 0 then pd.blood_colour(hsv((left * 3) % 360)) end
                 end,
                 stop=function() pd.blood_colour() end },
  -- Item swap: every weapon lying on the floor trades places with another.
  item_swap  = { label="Item swap", dur=0, start=function()
                   if not pd.items_shuffle then error("needs new exe") end
                   local n = pd.items_shuffle()
                   if n < 2 then error("not enough loose weapons") end
                   pd.hud_message(string.format("CHAOS: %d pickups shuffled", n))
                 end },
  -- Brandon's mod: the sky goes full random — bright random sky, cloud and
  -- fog colours instead of any stage's prebaked look. Bright hues only
  -- (hsv picks fully-saturated colours).
  brandons_mod = { label="Brandon's mod", dur=1,
                 start=function()
                   if not pd.env_colours then error("needs new exe") end
                   local sr, sg, sb = hsv(math.random(0, 359))
                   local cr, cg, cb = hsv(math.random(0, 359))
                   local fr, fg, fb = hsv(math.random(0, 359))
                   pd.env_colours(sr, sg, sb, cr, cg, cb)
                   if pd.fog then pd.fog(900, 1000, fr, fg, fb) end
                 end,
                 stop=function()
                   pd.env()          -- re-apply the stage's authored environment
                   if pd.fog then pd.fog() end
                 end },

  -- ===== batch 3: popup framework + renderer/AI bindings (2026-07-12) =====
  -- Popups draw in the alpha draw hook and read RAW buttons (pd.buttons_pressed
  -- sees them even though pd.button_block keeps FIRE/AIM from shooting).
  -- Pop quiz: answer with FIRE (1) or AIM (2) before the timer — wrong or
  -- ignored costs half your health.
  pop_quiz   = { label="Pop quiz", fixeddur=true, dur=30,
                 start=function()
                   if not pd.buttons_pressed then error("needs new exe") end
                   local QS = {
                     { q="The Skedar homeworld's star system?", a="unknown, it's classified", b="Alpha Centauri", correct=1 },
                     { q="Elvis is a...", a="Maian", b="Skedar", correct=1 },
                     { q="The CI hub pistol range is on which floor?", a="the basement", b="the roof", correct=1 },
                     { q="dataDyne's CEO is...", a="Cassandra De Vries", b="Daniel Carrington", correct=1 },
                     { q="The FarSight sees through...", a="everything, it's broken", b="nothing", correct=1 },
                     { q="Proximity mines are best deployed...", a="carefully", b="at your own feet", correct=1 },
                   }
                   -- 1-3 questions, drawn without repeats, for unpredictability
                   local want = math.random(1, 3)
                   local deck, used = {}, {}
                   while #deck < want do
                     local i = math.random(#QS)
                     if not used[i] then used[i] = true; deck[#deck + 1] = QS[i] end
                   end
                   st.a_quiz = { deck = deck, idx = 1, total = want }
                   pd.button_block(0x2010) -- FIRE + AIM answer, don't shoot
                 end,
                 tick=function(left)
                   local qz = st.a_quiz
                   if not qz or qz.done then return end
                   local cur = qz.deck[qz.idx]
                   local pressed = pd.buttons_pressed()
                   local pick = nil
                   if pressed and (pressed & 0x2000) ~= 0 then pick = 1
                   elseif pressed and (pressed & 0x0010) ~= 0 then pick = 2 end
                   if pick then
                     if pick == cur.correct then
                       pd.hud_message(string.format("CHAOS: correct (%d/%d)", qz.idx, qz.total))
                     else
                       pd.player_damage(4)
                       pd.hud_message("CHAOS: WRONG.")
                     end
                     qz.idx = qz.idx + 1
                     if qz.idx > qz.total then
                       qz.done = true
                       pd.button_block(0)
                       return true -- quiz over (expiry loop handles it)
                     end
                   elseif left <= 10 then
                     qz.done = true
                     pd.player_damage(4)
                     pd.hud_message("CHAOS: time's up. that also counts as wrong")
                   end
                 end,
                 stop=function() st.a_quiz = nil; pd.button_block(0) end },
  -- Agree to the EULA: three pages of terms, each with its OWN acceptance
  -- ritual — press FIRE, open a door, hold crouch, or spin in a circle
  -- (task library above). Your guns don't work until you're done reading.
  eula       = { label="Agree to the EULA", fixeddur=true, dur=60,
                 start=function()
                   if not pd.buttons_pressed then error("needs new exe") end
                   -- page 1 is always the classic FIRE-to-accept; later pages
                   -- draw random rituals (never live-fire — guns are blocked)
                   st.a_eula = { page = 1, task = task_new("press") }
                   pd.button_block(0x2000)
                 end,
                 tick=function()
                   local eu = st.a_eula
                   if not eu then return end
                   if task_tick(eu.task) then
                     eu.page = eu.page + 1
                     if eu.page > 3 then
                       pd.button_block(0)
                       pd.hud_message("CHAOS: agreement accepted. weapons restored")
                       return true
                     end
                     eu.task = task_random(true)
                   end
                 end,
                 stop=function() st.a_eula = nil; pd.button_block(0) end },
  -- Catch up on the lore: opens the REAL Carrington Institute "Information"
  -- menu (character & misc bios) over the paused mission, exactly like a hub
  -- terminal. The player reads and closes it themselves; closing resumes the
  -- mission. Solo/co-op only.
  lore       = { label="Catch up on the lore", dur=0,
                 start=function()
                   if not pd.menu_lore then error("needs new exe") end
                   if not pd.menu_lore() then error("can't open it here") end
                 end },
  -- Quad handed: dual-wield whatever you hold AND every shot fires twice —
  -- four barrels' worth of output (ammo drains to match).
  -- Quad handed: dual-wield whatever you hold AND a second pair of the same
  -- guns hangs upside-down from the top of the screen (pd.quad_top). Every
  -- shot also fires twice — four barrels' worth of output (ammo to match).
  quad_handed = { label="Quad handed", dur=1,
                 start=function()
                   if not pd.double_shots then error("needs new exe") end
                   local h = pd.weapon_held()
                   if not h or h <= W.UNARMED then h = W.CMP150; pd.give_weapon(h) end
                   st.a_quad = h
                   -- set the quad flag FIRST: it also doubles the magazine, and
                   -- clip sizes are baked when the weapon is (re-)equipped below
                   if pd.quad_top then pd.quad_top(true) end -- top guns + double mag
                   pd.double_shots(true)                     -- double ammo per shot
                   pd.dual_wield(h)                          -- equips -> bakes 2x clip
                   pd.refill_ammo()
                 end,
                 stop=function()
                   pd.double_shots(false)
                   if pd.quad_top then pd.quad_top(false) end
                   local h = st.a_quad
                   if h then pd.take_weapon(h); pd.give_weapon(h); pd.switch_weapon(h) end
                   st.a_quad = nil
                 end },
  -- Wireframe enemies: hostile chrs (and their guns) render as outlines.
  wireframe_enemies = { label="Wireframe enemies", dur=1,
                 start=function()
                   if not pd.chr_wireframe then error("needs new exe") end
                   pd.chr_wireframe(true)
                 end,
                 stop=function() pd.chr_wireframe(false) end },
  -- Helicopter helicopter: the dD hovercopter drops by at QUARTER scale.
  -- Exploratory — the mission choppers are script-driven; this one holds
  -- position and (best-effort) opens fire on you.
  helicopter = { label="Helicopter helicopter", dur=0, start=function()
                   if not pd.spawn_chopper then error("needs new exe") end
                   if not pd.spawn_chopper(0, 64) then error("no room for a chopper") end
                 end },
  -- A51 interceptor: the manned interceptor scrambles to your position. A
  -- native chopper type, so it flies and fires with the real machinery.
  -- (256 = the authored size — the modeldef is natively ~0.1 scale, so lower
  -- shrinks it toward invisible; 1024 = the 4x menace requested.)
  interceptor = { label="A51 interceptor", dur=0, start=function()
                   if not pd.spawn_chopper then error("needs new exe") end
                   if not pd.spawn_chopper(1, 1024) then error("no room for an interceptor") end
                 end },
  -- Schedule 1: you turn into the drug spy for a while (free-fly drone; your
  -- body waits where you left it).
  schedule_1 = { label="Schedule 1", fixeddur=true, dur=20,
                 start=function()
                   if not pd.possess_spawn then error("needs new exe") end
                   if not pd.possess_spawn(0x6c) then error("possession failed") end -- BODY_EYESPY
                 end,
                 stop=function() if pd.unpossess then pd.unpossess() end end },

  -- ===== SA-inspired batch (2026-07-19, zolika1351 GTA:SA chaos list) =====
  -- All held in the ALPHA_ONLY test area until runtime-proven.
  -- Wrong Way: about face. That's it. That's the effect.
  wrong_way  = { label="Wrong Way", dur=0, start=function()
                   if not pd.player_add_yaw then error("needs new exe") end
                   pd.player_add_yaw(180)
                 end },
  -- Combo Time: three random main-pool effects fire AT ONCE.
  combo_time = { label="Combo Time", dur=0, start=function()
                   local pool = {}
                   for n, e in pairs(chaos.effects) do
                     if not e.alpha and (e.w or 0) > 0 and not st.active[n]
                         and effect_enabled(n) then
                       pool[#pool + 1] = n
                     end
                   end
                   for i = 1, math.min(3, #pool) do
                     chaos.trigger(table.remove(pool, math.random(#pool)), "combo")
                   end
                 end },
  -- No Pausing: the START button is confiscated. (Stomps any concurrent
  -- button_block mask — the thief and the popups already share that quirk.)
  no_pausing = { label="No Pausing", dur=1,
                 start=function()
                   if not pd.button_block then error("needs new exe") end
                   pd.button_block(0x1000)
                   pd.hud_message("CHAOS: pausing is for cowards")
                 end,
                 stop=function() pd.button_block(0) end },
  -- No Shooting Allowed: you shoot, you die (weaponfire hook).
  no_shooting = { label="No Shooting Allowed", dur=1,
                 start=function()
                   pd.hud_message("CHAOS: you shoot, you DIE")
                 end },
  -- Pacifist: violence has a price — every shot costs health (weaponfire hook).
  pacifist   = { label="Pacifist", dur=1,
                 start=function()
                   pd.hud_message("CHAOS: violence has a price")
                 end },
  -- Slow Bleeding: lose 90% of REMAINING health across the duration —
  -- proportional decay, floors around 10% of what you started with. Never
  -- lethal by itself; everything else suddenly is.
  slow_bleed = { label="Slow Bleeding", dur=1,
                 start=function() st.a_bleed = {} end,
                 tick=function(left)
                   local b = st.a_bleed
                   if not b then return end
                   b.total = b.total or left
                   b.next = b.next or left
                   if left <= b.next then
                     b.next = left - 30 -- one step per half second
                     local steps = b.total / 30
                     pd.player_set_health(pd.player_health() * (0.1 ^ (1 / steps)))
                   end
                 end,
                 stop=function() st.a_bleed = nil end },
  -- 1% chance of death: the roll is real.
  death_chance = { label="1% chance of death", dur=0, start=function()
                   if math.random(100) == 1 then
                     pd.hud_message("CHAOS: unlucky.")
                     pd.player_damage(100)
                   else
                     pd.hud_message("CHAOS: ...you live. this time")
                   end
                 end },
  -- SUPERHOT: time moves when you move (binary — standing still toggles the
  -- slo-mo cheat on, moving releases it).
  superhot   = { label="SUPERHOT", dur=1,
                 start=function() st.a_shot = { on = false } end,
                 tick=function(left)
                   local s = st.a_shot
                   if not s then return end
                   local x, y, z = pd.player_pos(0)
                   if not x then return end
                   local moved = s.x and ((x - s.x) ^ 2 + (z - s.z) ^ 2) or 0
                   s.x, s.z = x, z
                   local still = moved < 4
                   if still ~= s.on then
                     s.on = still
                     pd.cheat(CHEAT.SLOMO, still)
                   end
                 end,
                 stop=function()
                   st.a_shot = nil
                   pd.cheat(CHEAT.SLOMO, false)
                 end },
  -- (mitosis removed 2026-07-19 — spawn-at-corpse never worked, retired
  -- rather than debugged.)
  -- Fake Lag: random rubber-banding freezes, 0.1-0.3s at a time.
  fake_lag   = { label="Fake Lag", dur=1,
                 start=function() st.a_lagt = 0; st.a_lagon = nil end,
                 tick=function(left)
                   local dt = pd.lvupdate and pd.lvupdate() or 1
                   st.a_lagt = (st.a_lagt or 0) - dt
                   if st.a_lagt <= 0 then
                     if st.a_lagon then
                       pd.player_freeze(false); st.a_lagon = nil
                       st.a_lagt = math.random(30, 120)
                     else
                       pd.player_freeze(true); st.a_lagon = true
                       st.a_lagt = math.random(6, 18)
                     end
                   end
                 end,
                 stop=function()
                   st.a_lagt, st.a_lagon = nil
                   pd.player_freeze(false)
                 end },
  -- Note 7: phone call, but the phone is a Note 7. Answering it detonates.
  note_7     = { label="Note 7", dur=1,
                 start=function()
                   st.a_note7 = { had = pd.has_weapon and pd.has_weapon(W.PSYCHOSIS) }
                   pd.give_weapon(W.PSYCHOSIS); pd.give_ammo(AMMO.PSYCHOSIS, 1)
                   if pd.weapon_rename then pd.weapon_rename(W.PSYCHOSIS, "Note 7") end
                   local _ = play_ring(true)
                   pd.hud_message("CHAOS: incoming call! equip the phone to answer")
                 end,
                 tick=function(left)
                   local p = st.a_note7
                   if p and pd.weapon_held() == W.PSYCHOSIS then
                     if pd.stop_file then pd.stop_file() end
                     pd.hud_message("CHAOS: hello? ...oh no. it's a Note 7")
                     local x, y, z = pd.player_pos(0)
                     if x and pd.explosion_at then pd.explosion_at(x, y, z) end
                     return true
                   end
                   if left % 90 == 0 then
                     for _, c in ipairs(pd.all_chrs() or {}) do pd.chr_alert(c) end
                   end
                 end,
                 stop=function()
                   if pd.stop_file then pd.stop_file() end
                   if pd.weapon_rename then pd.weapon_rename(W.PSYCHOSIS) end
                   if st.a_note7 and not st.a_note7.had then pd.take_weapon(W.PSYCHOSIS) end
                   st.a_note7 = nil
                 end },
  -- One Bullet Mags: reload after every shot. (The C binding re-bakes the
  -- held gun's clip immediately — excess loaded rounds go back to reserve.)
  one_bullet_mags = { label="One Bullet Mags", dur=1,
                 start=function()
                   if not pd.one_bullet then error("needs new exe") end
                   pd.one_bullet(true)
                 end,
                 stop=function() pd.one_bullet(false) end },
  -- Heavy Recoil: every shot launches you backward (weaponfire hook).
  heavy_recoil = { label="Heavy Recoil", dur=1,
                 start=function()
                   if not pd.player_push then error("needs new exe") end
                   pd.hud_message("CHAOS: mind the kick")
                 end },
  -- Inverted Look: up is down (mouse, gyro and right stick).
  invert_look = { label="Inverted Look", dur=1,
                 start=function()
                   if not pd.invert_look then error("needs new exe") end
                   pd.invert_look(true)
                 end,
                 stop=function() pd.invert_look(false) end },
  -- Stadia Mode: your inputs arrive half a second late. Mouse look stays
  -- live (honest limit) — buttons, keys and sticks all lag.
  stadia_mode = { label="Stadia Mode", dur=1,
                 start=function()
                   if not pd.input_delay then error("needs new exe") end
                   pd.input_delay(30)
                   pd.hud_message("CHAOS: streaming from a data centre near you")
                 end,
                 stop=function() pd.input_delay(0) end },
  -- UwUify: evewy stwing in the game. Evewy singwe one.
  uwuify     = { label="UwUify", dur=1,
                 start=function()
                   if not pd.uwuify then error("needs new exe") end
                   pd.uwuify(true)
                   pd.hud_message("CHAOS: what have you bwought upon this cuwsed wand")
                 end,
                 stop=function() pd.uwuify(false) end },
  -- Pig Latin: everyway ingstray, igpay atinlay. Same pipeline as UwUify
  -- (menus, briefings, HUD messages, chaos popups).
  piglatin   = { label="Pig Latin", dur=1,
                 start=function()
                   if not pd.piglatin then error("needs new exe") end
                   pd.piglatin(true)
                   pd.hud_message("CHAOS: eway eakspay igpay atinlay ownay")
                 end,
                 stop=function() pd.piglatin(false) end },
  -- Forced March: ONWARD. You may not stop walking.
  forced_march = { label="Forced March", dur=1,
                 start=function()
                   if not pd.forced_march then error("needs new exe") end
                   pd.forced_march(true)
                   pd.hud_message("CHAOS: ONWARD")
                 end,
                 stop=function() pd.forced_march(false) end },

  -- Texture override test: EVERY texture in the game becomes test.png for
  -- 20 seconds, then everything restores (the renderer re-imports the whole
  -- texture cache through the override filter both ways). The validator for
  -- pd.tex_override — alpha-only, like image_test.
  texture_test = { label="Texture test", fixeddur=true, dur=20,
                 start=function()
                   if not pd.tex_override then error("needs new exe") end
                   if not pd.tex_override("test.png") then
                     error("scripts/images/test.png missing")
                   end
                 end,
                 stop=function() pd.tex_override() end },
  -- Nepotism: the whole world gets skinned with an image related to you.
  -- First tries to match your agent name — an image whose name is a PREFIX
  -- of your (lowercased, alphanumeric-only) name, longest match first, down
  -- to 1 char (Gras/Graslu/Graslu00 -> gras.png; Red/Redvox/Redvox57 ->
  -- red.png). If nothing resembles you, the family picks a favourite anyway:
  -- a RANDOM image from scripts/images/. Works for any name.
  nepotism   = { label="Nepotism", dur=1,
                 start=function()
                   if not pd.tex_override or not pd.player_name then
                     error("needs new exe")
                   end
                   local name = (pd.player_name() or ""):lower():gsub("[^%w]", "")
                   -- longest-prefix name match
                   for len = #name, 1, -1 do
                     if pd.tex_override(name:sub(1, len)) then return end
                   end
                   -- no relation: pick a random image from the folder
                   local imgs = pd.list_images and pd.list_images() or {}
                   if #imgs == 0 then error("scripts/images/ has no images") end
                   -- shuffle-try so a broken/oversized PNG doesn't kill it
                   for i = #imgs, 2, -1 do
                     local j = math.random(i)
                     imgs[i], imgs[j] = imgs[j], imgs[i]
                   end
                   for _, n in ipairs(imgs) do
                     if pd.tex_override(n) then return end
                   end
                   error("no usable image in scripts/images/")
                 end,
                 stop=function() pd.tex_override() end },
  -- iPod Ad: the silhouette dance. Walls turn a solid vivid colour, everyone
  -- becomes a black silhouette, weapons and objects go pure white, and the
  -- whole scene wears white wireframe edges. A random iPod-ad colour each time.
  ipod_ad    = { label="iPod Ad", fixeddur=true, dur=25,
                 start=function()
                   if not pd.ipod_ad then error("needs new exe") end
                   local COLS = {
                     {  0, 217, 140 }, -- vivid green
                     {255,  40, 130 }, -- hot pink
                     { 40, 180, 255 }, -- cyan blue
                     {255, 140,   0 }, -- orange
                     {170,  60, 255 }, -- purple
                     {255, 210,   0 }, -- yellow
                   }
                   local c = COLS[math.random(#COLS)]
                   pd.ipod_ad(true, c[1], c[2], c[3])
                 end,
                 stop=function() pd.ipod_ad(false) end },
  -- ===== SA/HL2 wave 2 (2026-07-19 "go nuts" batch) =====
  -- Dutch Angle: the camera tilts and STAYS tilted. 1-in-20 it goes full 90.
  dutch_angle = { label="Dutch Angle", dur=1,
                 start=function()
                   if not pd.screen_roll then error("needs new exe") end
                   local a = (math.random(2) == 1 and 1 or -1)
                       * (math.random(20) == 1 and 90 or math.random(8, 15))
                   pd.screen_roll(a)
                 end,
                 stop=function() pd.screen_roll(0) end },
  -- Blind: instant black, sight bleeds back in over ~8 seconds.
  blind      = { label="Blind", fixeddur=true, dur=9,
                 start=function() pd.fade(0, 0, 0, 255, 480) end,
                 stop=function() pd.fade(0, 0, 0, 0, 0) end },
  -- Fading Out: the lights go down slowly across the whole duration, then
  -- snap back. (Never fully black — 240/255 at the end.)
  fading_out = { label="Fading Out", dur=1,
                 start=function() st.a_fadeout = {} end,
                 tick=function(left)
                   local f = st.a_fadeout
                   if not f then return end
                   f.total = f.total or left
                   if not f.next or left <= f.next then
                     f.next = left - 10
                     pd.fade(0, 0, 0, math.floor(240 * (1 - left / f.total)), 0)
                   end
                 end,
                 stop=function() st.a_fadeout = nil; pd.fade(0, 0, 0, 0, 0) end },
  -- Sleepy Mode: your eyelids keep drooping — slow fade to nearly-black,
  -- brief hold, snap awake, repeat at random intervals.
  sleepy     = { label="Sleepy Mode", dur=1,
                 start=function() st.a_sleep = { t = 0, period = 420 } end,
                 tick=function(left)
                   local s = st.a_sleep
                   if not s then return end
                   local dt = pd.lvupdate and pd.lvupdate() or 1
                   s.t = s.t + dt
                   local droopstart = s.period - 120 -- 1.5s droop + 0.5s hold
                   if s.t >= s.period then
                     s.t = 0
                     s.period = math.random(300, 540)
                     pd.fade(0, 0, 0, 0, 0) -- snap awake
                   elseif s.t >= droopstart then
                     local p = (s.t - droopstart) / 90
                     pd.fade(0, 0, 0, math.floor(235 * math.min(1, p)), 0)
                   end
                 end,
                 stop=function() st.a_sleep = nil; pd.fade(0, 0, 0, 0, 0) end },
  -- Virtual Boy: four shades of red working towards black (renderer palette
  -- mode 1003, the Handheld recipe in red) AND the screen duplicated into
  -- two side-by-side eye panels at the ORIGINAL aspect ratio (fx bit 1024 —
  -- each eye is the whole scene at 50% scale, letterboxed top and bottom).
  -- Nintendo 1995.
  virtualboy = { label="Virtual Boy", dur=1,
                 start=function()
                   pd.pixelate(192, 112, 1003)
                   pd.screen_fx(1024, true)
                   pd.audio_crush(4, 8)
                 end,
                 stop=function()
                   pd.pixelate()
                   pd.screen_fx(1024, false)
                   pd.audio_crush()
                 end },
  -- (mosh_pit + world_peace removed 2026-07-19 per user — world_peace's
  -- chr_calm spam had no visible effect; retired, not debugged.)
  -- (no_chaos_ui removed 2026-07-19 per user.)
  -- Mercy: every active effect ends right now.
  clear_effects = { label="Mercy", dur=0, start=function()
                   local names = {}
                   for n in pairs(st.active) do names[#names + 1] = n end
                   for _, n in ipairs(names) do
                     local e = chaos.effects[n]
                     if e and e.stop then pcall(e.stop) end
                     st.active[n] = nil
                     st.duration[n] = nil
                   end
                 end },
  -- Overtime: every active effect's timer refills to full. You're welcome.
  refill_effects = { label="Overtime", dur=0, start=function()
                   for n in pairs(st.active) do
                     st.active[n] = st.duration[n] or st.active[n]
                   end
                 end },
  -- Buttsbot: random (but stable) words across the whole game become "butt".
  buttsbot   = { label="Buttsbot", dur=1,
                 start=function()
                   if not pd.buttsbot then error("needs new exe") end
                   pd.buttsbot(true)
                 end,
                 stop=function() pd.buttsbot(false) end },
  -- Itchy Trigger Finger: at random moments the gun fires ONE shot by
  -- itself (a ~2-tick trigger pulse every 1-4 seconds).
  itchy_trigger = { label="Itchy Trigger Finger", dur=1,
                 start=function()
                   if not pd.forced_fire then error("needs new exe") end
                   st.a_itchy = { t = math.random(60, 240) }
                 end,
                 tick=function(left)
                   local it = st.a_itchy
                   if not it then return end
                   local dt = pd.lvupdate and pd.lvupdate() or 1
                   if it.firing then
                     it.firing = it.firing - dt
                     if it.firing <= 0 then
                       pd.forced_fire(false)
                       it.firing = nil
                     end
                   else
                     it.t = it.t - dt
                     if it.t <= 0 then
                       it.t = math.random(60, 240)
                       it.firing = 2
                       pd.forced_fire(true)
                     end
                   end
                 end,
                 stop=function()
                   st.a_itchy = nil
                   pd.forced_fire(false)
                 end },
  -- No HUD: health, crosshair, ammo, radar, messages, kill feed, weapon
  -- select — gone. Aim from the heart.
  no_hud     = { label="No HUD", dur=1,
                 start=function()
                   if not pd.hud_off then error("needs new exe") end
                   pd.hud_off(true)
                 end,
                 stop=function() pd.hud_off(false) end },
  -- WAYTOODANK: gun FOV 140. The weapon becomes an experience.
  waytoodank = { label="WAYTOODANK", dur=1,
                 start=function()
                   if not pd.gun_fov then error("needs new exe") end
                   pd.gun_fov(140)
                 end,
                 stop=function() pd.gun_fov(0) end },
  -- Weeping Skedar: a ONE-OFF spawn, no timer — it hunts until dead, but
  -- only moves when you're not looking at it. Don't blink. (The view-cone
  -- statue logic runs in the MAIN tick off st.a_weep, outside any effect
  -- duration, like the martyrdom queue.)
  weeping    = { label="Weeping Skedar", dur=0,
                 start=function()
                   if not pd.chr_freeze_one then error("needs new exe") end
                   local a = math.random() * 2 * math.pi
                   local c = pd.spawn_body(BODY.SKEDAR, -1,
                                           math.sin(a) * 900, math.cos(a) * 900)
                   if not c or c < 0 then error("no room / skedar unavailable") end
                   pd.chr_alert(c)
                   st.a_weep = { c = c }
                 end },

  -- Dokkaebi: your phone rings LOUDLY, alerting every guard on repeat. A
  -- "phone" (a unique item) appears in your inventory — EQUIP it to answer
  -- and end the call. (Uses the Psychosis Gun slot as the stand-in handset.)
  phone_call = { label="Phone call for you", dur=1,
                 start=function()
                   st.a_phone = { had = pd.has_weapon and pd.has_weapon(W.PSYCHOSIS) }
                   pd.give_weapon(W.PSYCHOSIS); pd.give_ammo(AMMO.PSYCHOSIS, 1)
                   -- rename the "phone" everywhere it shows for the bit (the
                   -- new exe also relabels the weapon WHEEL via the shortname
                   -- override and hides the Psychosis model in the pause menu)
                   if pd.weapon_rename then pd.weapon_rename(W.PSYCHOSIS, "Nokia 3315") end
                   -- loop a ringtone start-to-finish until answered: random
                   -- pick from ring1..ring5(.wav|.mp3), fallback ring.*
                   local _ = play_ring(true)
                   pd.hud_message("CHAOS: incoming call! equip the phone to answer")
                 end,
                 tick=function(left)
                   local p = st.a_phone
                   if p and pd.weapon_held() == W.PSYCHOSIS then
                     if pd.stop_file then pd.stop_file() end
                     pd.hud_message("CHAOS: ...hello? uh huh. ok. wrong number.")
                     return true -- answered
                   end
                   -- keep the guards agitated; the ringtone loops in C now, so
                   -- don't re-trigger it (that restarted it every 1.5s)
                   if left % 90 == 0 then
                     for _, c in ipairs(pd.all_chrs() or {}) do pd.chr_alert(c) end
                   end
                 end,
                 stop=function()
                   if pd.stop_file then pd.stop_file() end -- silence on any exit
                   if pd.weapon_rename then pd.weapon_rename(W.PSYCHOSIS) end -- restore name
                   if st.a_phone and not st.a_phone.had then pd.take_weapon(W.PSYCHOSIS) end
                   st.a_phone = nil
                 end },
}

-- 2026-07-19: the original alpha batch GRADUATED — effects here join the main
-- rotation / vote slate / ON-off list at a standard draw weight (each keeps
-- its tuned duration) UNLESS held back below. ALPHA_ONLY = the TEST AREA:
-- listed effects live only in the Chaos Alpha folder + /chaos trigger, never
-- the random rotation. Delete a name to graduate it.
local ALPHA_ONLY = {
  image_test=1, -- the image-hook validator, not a real effect
  -- (SA-inspired batch graduated to the main pool 2026-07-19 after testing;
  -- mitosis removed outright — spawn-at-corpse never worked.)
  -- (SA/HL2 wave 2 graduated to the main pool 2026-07-19 after testing.)
  texture_test=1, -- the pd.tex_override validator, not a real effect
}
for name, e in pairs(alpha_effects) do
  if ALPHA_ONLY[name] then
    e.alpha = true
    e.w = 0 -- never randomly drawn (pick_random skips alpha anyway)
  else
    e.w = e.w or 3 -- standard weight (alpha entries carried none)
  end
  chaos.effects[name] = e
end

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

-- Full teardown: stop every active effect and reset every C-side global an
-- effect can leave set (visual filters, input locks, speed/damage/zoom scales,
-- audio modes) — these live in C and SURVIVE both a stage reload and the Lua
-- state teardown, so nothing else clears them. Reused by the return-to-menu
-- path (tick handler) and the stage event.
local function reset_all_modes()
  -- Experiment cheats (GoldenEye / Wireframe / Mirror / Evil music) ride the
  -- ENABLED cheat bank, which SURVIVES a reload (unlike the active bank). Clear
  -- the ones CHAOS turned on while st.active still records them — never touch a
  -- user's menu-set experiment.
  if pd.cheat then
    if st.active.mirror then pd.cheat(CHEAT.MIRROR, false) end
    if st.active.tonal     then pd.cheat(CHEAT.TONAL, false) end
    if st.active.goldeneye then pd.cheat(CHEAT.GOLDENEYE, false) end
  end
  -- Run each active effect's own stop() cleanup, then drop bookkeeping and
  -- re-arm the timer so the first effect isn't instant on the next stage.
  stop_all()
  st.active = {}
  st.duration = {}
  st.cvotes = {0, 0, 0}
  st.timer = st.interval * TICKS
  st.votetimer = st.votetime * TICKS
  st.misfire_armed = false
  st.switch_want = nil
  st.martyr_queue = nil
  st.pitch_anim = nil
  st.recoil_kick = nil
  st.a_bleed, st.a_shot, st.a_note7 = nil
  st.a_fadeout, st.a_sleep, st.a_weep, st.a_itchy = nil
  if pd.fade then pd.fade(0, 0, 0, 0, 0) end
  if pd.forced_fire then pd.forced_fire(false) end
  if pd.hud_off then pd.hud_off(false) end
  if pd.gun_fov then pd.gun_fov(0) end
  if pd.chr_freeze_one then pd.chr_freeze_one(-1) end
  if pd.buttsbot then pd.buttsbot(false) end
  if pd.tex_override then pd.tex_override() end
  if pd.ipod_ad then pd.ipod_ad(false) end
  st.a_lagt, st.a_lagon = nil
  -- Visual modes + ammo swap + input locks etc. — explicit reset (C globals).
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
  if pd.gun_sound then pd.gun_sound() end
  if pd.damage_scale then pd.damage_scale(1) end
  if pd.paintball then pd.paintball(false) end
  if pd.weapon_jam then pd.weapon_jam(false) end
  if pd.player_freeze then pd.player_freeze(false) end
  if pd.chr_freeze then pd.chr_freeze(false) end
  if pd.no_drops then pd.no_drops(false) end
  if pd.mute then pd.mute(false) end
  if pd.zoom_scale then pd.zoom_scale(1) end
  if pd.chr_speed then pd.chr_speed(1) end
  if pd.player_speed then pd.player_speed(1) end
  if pd.screen_tint then pd.screen_tint() end
  if pd.pixelate then pd.pixelate() end -- also clears the hue-rotate / virtualboy colour modes
  if pd.screen_fx then pd.screen_fx(0x43f, false) end -- incl. the 1024 side-by-side bit
  if pd.lens then pd.lens() end
  if pd.audio_crush then pd.audio_crush() end
  if pd.audio_radio then pd.audio_radio(false) end
  if pd.audio_reverb then pd.audio_reverb() end
  if pd.audio_reverse then pd.audio_reverse(false) end
  if pd.audio_pitch then pd.audio_pitch() end
  if pd.upside_down then pd.upside_down(false) end
  if pd.double_vision then pd.double_vision(false) end
  if pd.gun_lock then pd.gun_lock(false) end
  if pd.knife_lock then pd.knife_lock(false) end
  if pd.mag_dump then pd.mag_dump(false) end
  if pd.gas then pd.gas(false) end
  if pd.t_pose then pd.t_pose(false) end
  if pd.pinball then pd.pinball(false) end
  if st.weather_set and pd.weather then pd.weather(0); st.weather_set = false end
  -- Drop any pending self-destruct invuln grace (the tick that would clear it
  -- won't run once we're in the hub / end screen).
  if st.sd_invuln then st.sd_invuln = nil; if pd.invincible then pd.invincible(false) end end
  st.scaled_g = nil
  st.scaled_a = nil
  -- Chaos Alpha state (belt and braces — each effect's stop() already ran).
  if pd.explosions_around then pd.explosions_around(false) end
  st.a_boom_off = nil
  st.a_bloop, st.a_twoh, st.a_imgtest = nil
  st.a_ltk, st.a_run, st.a_cap, st.a_rr = nil
  st.a_classic, st.a_angst, st.a_phone, st.a_count = nil
  st.a_objf, st.a_thief, st.a_cd, st.a_roll = nil
  st.a_quiz, st.a_eula, st.a_quad = nil
  if pd.beyblade then pd.beyblade(false) end
  if pd.screen_roll then pd.screen_roll(0) end
  -- SA batch C globals
  if pd.one_bullet then pd.one_bullet(false) end
  if pd.invert_look then pd.invert_look(false) end
  if pd.input_delay then pd.input_delay(0) end
  if pd.uwuify then pd.uwuify(false) end -- zeroes the shared text mode (covers piglatin)
  if pd.forced_march then pd.forced_march(false) end
  st.home_marked = false -- re-mark the start point on the next stage entered
  -- Batch-2 C globals (new-exe bindings; guarded so old exes still run).
  if pd.force_secondary then pd.force_secondary(false) end
  if pd.button_block then pd.button_block(0) end
  if pd.ammo_cost then pd.ammo_cost(1) end
  if pd.autoaim then pd.autoaim(false) end
  if pd.deadzone then pd.deadzone(0) end
  if pd.nitro then pd.nitro(false) end
  if pd.objective_force then pd.objective_force() end -- clears all overrides
  if pd.max_blood then pd.max_blood(false) end
  if pd.blood_colour then pd.blood_colour() end
  if pd.env then pd.env() end -- restores the stage's own sky/fog (also clears pd.fog)
  if pd.chr_wireframe then pd.chr_wireframe(false) end
  if pd.double_shots then pd.double_shots(false) end
  if pd.unpossess then pd.unpossess() end
end

-- chaos.trigger(name, who, dur_override): fire an effect. dur_override (seconds)
-- forces a specific length for timed effects (the Test menu passes 30) instead
-- of the global st.effectdur; instant effects ignore it.
function chaos.trigger(name, who, dur_override)
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
  -- An effect's own dur is now mostly just a timed-vs-instant marker (a positive
  -- value, possibly a function). The actual on-screen length comes from the
  -- single adjustable st.effectdur so every timed effect shares one knob;
  -- instant effects (dur 0/nil) stay instant. Effects flagged fixeddur keep
  -- their own authored length (e.g. player-freeze effects that a 60s global
  -- would turn into a soft-lock).
  local base = (type(e.dur) == "function") and e.dur() or e.dur
  if base and base > 0 then
    -- fixeddur effects keep their own length even from the Test menu's 30s
    -- override (e.g. Snap/Blink must stay 3s, not hold white for 30s).
    local secs = e.fixeddur and base or (dur_override or st.effectdur)
    local ticks = secs * TICKS
    st.active[name] = ticks
    st.duration[name] = ticks
  end
  announce(e.label .. (who and ("  [" .. who .. "]") or ""))
  -- Anti-repeat deck: age every effect's cooldown by one fire, then put the one
  -- that just played on a fresh cooldown as long as the enabled list. Its pick
  -- weight stays suppressed (pick_random) until the whole list has cycled, so
  -- effects spread out instead of clumping — but it's a chance, never a hard ban.
  for n, cd in pairs(st.cooldown) do
    if cd <= 1 then st.cooldown[n] = nil else st.cooldown[n] = cd - 1 end
  end
  local enabled_count = 0
  for n, en in pairs(chaos.effects) do
    if effect_enabled(n) and not en.alpha then enabled_count = enabled_count + 1 end
  end
  st.cooldown[name] = math.max(1, enabled_count - 1)
  return true
end

local function pick_random()
  local pool, total = {}, 0
  for name, e in pairs(chaos.effects) do
    if effect_enabled(name) and not e.alpha then
      -- Full base weight when rested; heavily reduced right after firing, easing
      -- back as the cooldown ages down over subsequent effects (never zero, so a
      -- repeat is merely unlikely and unfired effects dominate the draw).
      local cd = st.cooldown[name] or 0
      local w = (e.w or 1) / (1 + cd)
      total = total + w
      pool[#pool + 1] = { name = name, acc = total }
    end
  end
  if total <= 0 then return nil end
  local r = math.random() * total
  for _, p in ipairs(pool) do
    if r <= p.acc then return p.name end
  end
  return pool[#pool] and pool[#pool].name or nil
end

function chaos.set_seed(n)
  math.randomseed(tonumber(n) or 0)
  pd.log("[chaos] seeded with " .. tostring(n) .. " (deterministic effect stream)")
end

-- Pick the 3 distinct effects chat votes on this window (weighted, cooldown-
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
    st.enabled = true; st.timer = st.interval * TICKS
    st.cooldown = {} -- fresh anti-repeat deck: every effect at full chance again
    persist(); announce("enabled")
    if st.votetime > 0 then st.votetimer = st.votetime * TICKS; pick_candidates() end
  elseif cmd == "off" then
    st.enabled = false; stop_all(); persist(); announce("disabled")
  elseif cmd == "toggle" then
    chaos.handle(source, st.enabled and "off" or "on")
  elseif cmd == "status" then
    pd.log(string.format("[chaos] %s  interval=%ds effectdur=%ds votetime=%ds active=%d port-fed-by=%s",
        st.enabled and "ON" or "off", st.interval, st.effectdur, st.votetime,
        (function() local n=0 for _ in pairs(st.active) do n=n+1 end return n end)(), source))
  elseif cmd == "list" then
    local names = {}
    for name in pairs(chaos.effects) do names[#names + 1] = name end
    table.sort(names)
    pd.log("[chaos] effects: " .. table.concat(names, " "))
  elseif cmd == "interval" then
    st.interval = math.max(5, tonumber(arg) or 20); persist()
    pd.log("[chaos] interval = " .. st.interval .. "s")
  elseif cmd == "effectdur" or cmd == "duration" then
    st.effectdur = math.max(1, tonumber(arg) or 60); persist()
    pd.log("[chaos] effect duration = " .. st.effectdur .. "s")
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

  -- Main-menu / hub detection: force-end all chaos and never fire a new effect.
  -- The "tick" event runs everywhere (menus/title/loading too). We're OUT of
  -- real gameplay when either: the local player pawn is gone (pd.player_pos nil
  -- — true in the pawn-less title menu, but STILL VALID during a mid-game pause,
  -- so a pause is unaffected); OR the current stage is the Carrington Institute
  -- main-menu hub / a title-menu stage (which DO have a pawn, so the pawn check
  -- alone misses them). Tear everything down once (latched) so nothing leaks and
  -- the random drumbeat stays silent in the hub.
  local have_player = pd.player_pos and (pd.player_pos(0) ~= nil)
  local in_hub = pd.stage and MENU_STAGES[pd.stage()]
  if (not have_player) or in_hub then
    if not st.in_menu then
      st.in_menu = true
      reset_all_modes()
    end
    return
  end
  st.in_menu = false

  -- Mission SUCCESS safeguard: the instant the game shows a COMPLETED mission
  -- endscreen (won, not failed/aborted), tear everything down — same as reaching
  -- the hub, but earlier. The endscreen freezes the sim (dt would be 0 below), so
  -- this must run BEFORE the paused early-return, or the visual filters/effects
  -- would linger over the end-of-mission screen. Latched (fires once); the C flag
  -- clears on the next stage load, re-arming it. Only fires on success — a failed
  -- or aborted mission never sets the flag, so effects run right up to the hub.
  if pd.mission_complete and pd.mission_complete() then
    if not st.mission_done then
      st.mission_done = true
      reset_all_modes()
    end
    return
  end
  st.mission_done = false

  -- Record the mission start point once per stage for "Back to the start"
  -- (pd.warp_home). We're past the menu/hub/endscreen gates here, so the
  -- first tick that reaches this line is the first real gameplay tick.
  if not st.home_marked and pd.mark_home then
    st.home_marked = pd.mark_home() or nil
  end

  -- Advance on GAME time, not frames: lvupdate() is the ticks the sim
  -- actually ran this frame — 0 while paused (no pausing out a bad effect),
  -- scaled during slo-mo/boost. Everything below (effect timers, the vote
  -- window, the drumbeat) freezes with the game.
  local dt = pd.lvupdate and pd.lvupdate() or 1
  if dt <= 0 then return end -- paused: freeze everything, timers included

  -- Age the bottom-left CHAOS toast on game time.
  if st.toast then
    st.toast.life = st.toast.life - dt
    if st.toast.life <= 0 then st.toast = nil end
  end

  -- Glass cannons: a fired gun shatters — remove it one tick after the shot
  -- (deferred so we don't change weapons re-entrantly inside the fire event).
  if st.glass_pending then
    for wn in pairs(st.glass_pending) do pd.take_weapon(wn) end
    st.glass_pending = nil
  end

  -- Martyrdom: spawn the corpse grenades queued by the kill hook, OUTSIDE the
  -- kill callback (see there). Every attempt logs to the console (~) so a
  -- failure names its link: no line at all = the kill event never fired;
  -- "pos unavailable" = the dying chr couldn't be looked up; "FAIL" = the
  -- engine refused the projectile spawn.
  if st.martyr_queue then
    for _, m in ipairs(st.martyr_queue) do
      if m.has then
        local ok = pd.grenade(m.x, m.y, m.z, m.chrnum)
        pd.log(string.format("[chaos] martyrdom: chr=%d grenade=%s",
                             m.chrnum, ok and "ok" or "FAIL"))
      else
        pd.log(string.format("[chaos] martyrdom: chr=%d pos unavailable", m.chrnum))
      end
    end
    st.martyr_queue = nil
  end

  -- Weeping Skedar: the view-cone statue logic — runs while the stalker
  -- lives, independent of any effect timer (the spawn is a one-off).
  -- Frozen while inside a ~40-degree half-cone of the player's facing.
  if st.a_weep and pd.chr_freeze_one then
    local w = st.a_weep
    local hp = pd.chr_health(w.c)
    if not hp or hp <= 0 then
      pd.chr_freeze_one(-1)
      st.a_weep = nil
    else
      local px, py, pz = pd.player_pos(0)
      local cx, cy, cz = pd.chr_pos(w.c)
      if px and cx then
        local yaw = math.rad(pd.player_yaw and pd.player_yaw() or 0)
        local fx, fz = -math.sin(yaw), math.cos(yaw)
        local dx, dz = cx - px, cz - pz
        local d = math.sqrt(dx * dx + dz * dz)
        if d < 1 then d = 1 end
        if (fx * dx + fz * dz) / d > 0.766 then
          pd.chr_freeze_one(w.c)
        else
          pd.chr_freeze_one(-1)
        end
      end
    end
  end

  -- Heavy Recoil: apply the deferred kick queued by the weaponfire hook —
  -- one frame after the shot, so the projectile is created and gone before
  -- the shooter gets launched. One kick per frame regardless of barrels.
  if st.recoil_kick then
    st.recoil_kick = nil
    if st.active.heavy_recoil and pd.player_push then
      pd.player_push(-22)
      if pd.player_pitch then
        pd.player_pitch(math.min(90, pd.player_pitch() + 10))
      end
    end
  end

  -- Self-destruct grace: hold invincibility ~1s past the last explosion so a
  -- blast still expanding on the exact frame the effect wears off can't kill you.
  if st.sd_invuln then
    st.sd_invuln = st.sd_invuln - dt
    if st.sd_invuln <= 0 then
      st.sd_invuln = nil
      if pd.invincible then pd.invincible(false) end
    end
  end

  -- Chaos Alpha: delayed explosions_around cutoff (Live grenade! fuse blast /
  -- the SPEED failure boom) — a short burst scheduled from an effect tick or
  -- stop, shut off here so nothing has to keep running to end it.
  if st.a_boom_off then
    st.a_boom_off = st.a_boom_off - dt
    if st.a_boom_off <= 0 then
      st.a_boom_off = nil
      if pd.explosions_around then pd.explosions_around(false) end
    end
  end

  -- Short view-pitch glide (banana peel's 0.3s look-up): runs OUTSIDE any
  -- effect lifetime so instant effects can animate the view without a timed
  -- wrapper (and its "wore off" toast).
  if st.pitch_anim and pd.player_pitch then
    local pa = st.pitch_anim
    pa.t = pa.t + dt
    local f = math.min(1, pa.t / pa.len)
    pd.player_pitch(pa.from + (pa.to - pa.from) * f)
    if f >= 1 then st.pitch_anim = nil end
  end

  -- Deferred weapon switch: a give_weapon+switch_weapon in the same tick as a
  -- take_weapon loses the race — take's bgunCycleBack overrides the equip and
  -- the player is left holding nothing. force_switch() records the wanted gun
  -- here; keep re-equipping each tick until it sticks (or ~2s passes).
  if st.switch_want then
    local sw = st.switch_want
    sw.ticks = sw.ticks - dt
    if pd.weapon_held() == sw.weapon or sw.ticks <= 0 then
      st.switch_want = nil
    else
      pd.switch_weapon(sw.weapon)
    end
  end

  -- Timed-effect expiry runs REGARDLESS of the master switch, so effects fired
  -- from the Test menu still count down and wear off while Chaos is turned off.
  -- A tick that returns true ends its effect NOW (the popup effects finish on
  -- input) — never call stop_effect from inside a tick: this loop would re-add
  -- the key it just removed, which corrupts the pairs iteration.
  for name, left in pairs(st.active) do
    local e = chaos.effects[name]
    local endnow = false
    if e and e.tick then
      local ok, r = pcall(e.tick, left)
      endnow = ok and r == true
    end
    left = left - dt
    if endnow or left <= 0 then
      stop_effect(name)
      announce((e and e.label or name) .. (endnow and " cleared" or " wore off"))
    else
      st.active[name] = left
    end
  end

  -- The random drumbeat and chat-vote only run while Chaos is enabled; the
  -- expiry above already ran so manual test effects stay on their own timers.
  if not st.enabled then return end

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
  -- Glass cannons: every shot is a one-shot-kill Gold Magnum round, but the gun
  -- shatters afterwards — queue it for removal on the next tick (weaponnum > 1
  -- skips fists/knife). You burn through your whole arsenal one shot at a time.
  if st.active.glass_cannon and playernum == 0 and weaponnum and weaponnum > 1 then
    st.glass_pending = st.glass_pending or {}
    st.glass_pending[weaponnum] = true
  end
  -- Alpha CAPTCHA: real shots feed the "fire your weapon" task.
  if st.active.captcha and st.a_cap and st.a_cap.task
      and st.a_cap.task.kind == "fire" and playernum == 0 then
    st.a_cap.task.shots = (st.a_cap.task.shots or 0) + 1
  end
  -- SA batch: No Shooting Allowed (instant death), Pacifist (a health tax),
  -- Heavy Recoil (every shot launches you backward). weaponnum > 1 skips
  -- fists/knife, the glass-cannon convention.
  if playernum == 0 and weaponnum and weaponnum > 1 then
    if st.active.no_shooting then
      pd.hud_message("CHAOS: told you.")
      pd.player_damage(100)
    end
    if st.active.pacifist then
      pd.player_damage(0.4)
    end
    if st.active.heavy_recoil then
      -- FLAG only — the kick applies from the MAIN tick, one frame later,
      -- so the bullet/projectile is fully created and on its way before the
      -- push/pitch move the shooter (kicking inside the fire event could
      -- deflect the very shot being fired)
      st.recoil_kick = true
    end
  end
  -- Russian roulette: the trigger pull IS the spin. Resolve immediately.
  if st.active.russian_roulette and st.a_rr and not st.a_rr.fired
      and playernum == 0 and weaponnum == W.MAGNUM then
    st.a_rr.fired = true
    if math.random(6) == 1 then
      pd.player_damage(100)
    else
      local c = random_chr()
      if c then pd.chr_damage(c, 100) end
      pd.player_set_health(math.min(1, pd.player_health() + 0.1))
      pd.hud_message("CHAOS: click... someone else was less lucky")
    end
    stop_effect("russian_roulette")
  end
end)

-- Mediguns (alpha): any weapon pickup heals a chunk.
pd.on("weaponfound", function(weaponnum)
  if st.active.mediguns then
    pd.player_set_health(math.min(1, pd.player_health() + 0.1))
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

-- Kill hook: death-reactive effects (martyrdom grenades, etc).
pd.on("kill", function(chrnum, killerplayernum)
  -- Alpha effects that react to ANY death, whoever caused it:
  if st.active.martyrdom and pd.grenade then
    -- QUEUE the drop for the main tick instead of spawning here: this
    -- callback runs synchronously inside the engine's death processing, and
    -- creating a projectile prop mid-death-tick is the same re-entrancy that
    -- broke gun_game2's weapon churn (the glass_pending lesson). Position is
    -- captured now, spawn happens one frame later from the safe tick context.
    local x, y, z = pd.chr_pos(chrnum)
    st.martyr_queue = st.martyr_queue or {}
    st.martyr_queue[#st.martyr_queue + 1] =
        { x = x, y = y, z = z, chrnum = chrnum, has = (x ~= nil) }
  end
  if killerplayernum ~= 0 then return end
  -- (gun_game / gun_game2 kill-advance blocks removed 2026-07-19 with the
  -- effects.)
end)

-- Stage transition: full teardown so no C-side effect leaks into the next
-- stage. (The engine currently doesn't dispatch a "stage" event, so the real
-- trigger is the return-to-menu detection in the tick handler above; this stays
-- wired for the day a stage event is added.)
pd.on("stage", reset_all_modes)

-- ---- HUD: active-effect timer bars + the chat-vote slate (top left) --------
-- Item-pickup-style bars: label, then a dark backing box with a filled
-- fraction that drains as the effect runs out. Below the bars, the 3-effect
-- vote slate + live counts + a window-countdown bar — the on-screen half of
-- the Twitch/YouTube voting foundation (chat sends `vote 1|2|3` via the UDP
-- ingress; this panel is what the streamer's viewers read).
-- Anchored top-left (x=8, the Lua HUD left margin), by the Combat Sim kill count.
local HUD_X, HUD_W = 8, 74
local C_TEXT, C_BAR, C_BARBG, C_VOTE = 0xffffffff, 0x40c0ffff, 0x00000090, 0xffe040ff

pd.on("draw", function()
  local y = 4

  -- active timed effects, stable order
  if next(st.active) ~= nil then
    local names = {}
    for name in pairs(st.active) do names[#names + 1] = name end
    table.sort(names)
    local shown = 0
    for i = 1, #names do
      if shown >= 5 then break end
      local name = names[i]
      local e = chaos.effects[name]
      if not (e and e.nobar) then -- nobar: one-offs that draw their own HUD
        local left = st.active[name]
        local total = st.duration[name] or left
        local frac = (total > 0) and (left / total) or 0
        pd.draw_text(HUD_X, y, e and e.label or name, C_TEXT)
        pd.draw_box(HUD_X, y + 8, HUD_W, 4, C_BARBG)
        pd.draw_box(HUD_X, y + 8, math.max(1, math.floor(HUD_W * frac)), 4, C_BAR)
        y = y + 16
        shown = shown + 1
      end
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

  -- CHAOS: <name> toast, bottom-left, weapon-pickup style. Box HUGS the letters:
  -- text_size gives the true width; the box height is a tight cap-height (the
  -- engine hudmsg box is a full line-height tall, which is the gap being fixed).
  if st.toast then
    local text = st.toast.text
    local tw = 60
    if pd.text_size then tw = (pd.text_size(text)) end
    -- fade out over the last TOAST_FADE seconds
    local frac = st.toast.life / (TOAST_FADE * TICKS)
    if frac > 1 then frac = 1 elseif frac < 0 then frac = 0 end
    local a = math.floor(255 * frac)
    local TX, TY, TH = 8, 202, 10  -- bottom-left anchor; TH hugs XS caps
    pd.draw_box(TX - 2, TY - 1, tw + 4, TH, math.floor(a * 0.75)) -- black box, faded
    pd.draw_text(TX, TY, text, 0xffffff00 + a)                    -- white text, faded
  end
end)

-- ---- HUD: Chaos Alpha overlays (second draw handler; pd.on supports many) --
local function centered_text(y, text, color)
  local tw = pd.text_size and pd.text_size(text) or 60
  pd.draw_text(math.floor((320 - tw) / 2), y, text, color)
end

pd.on("draw", function()
  -- Blooper: black blood-splat textures smeared over the view, fading out over
  -- the last 3 seconds.
  if st.active.blooper and st.a_bloop and pd.draw_sprite then
    local frac = math.min(1, (st.active.blooper or 0) / (3 * TICKS))
    local a = math.floor(255 * frac)
    if a > 0 then
      for _, s in ipairs(st.a_bloop) do
        pd.draw_sprite(s.tex, s.x, s.y, s.w, s.h, 0x000000 * 256 + a) -- black + fade alpha
      end
    end
  end

  -- Image loader test: center -> scroll -> resize -> spin, 10s per phase.
  if st.active.image_test and st.a_imgtest and pd.draw_image then
    local it = st.a_imgtest
    local t = it.t
    local cx, cy, w, h, angle = 160, 120, 96, 96, 0
    local phase = math.floor(t / (10 * TICKS)) % 4
    local label
    if phase == 0 then
      label = "CENTER"
    elseif phase == 1 then
      cx = 160 + math.floor(90 * math.cos(t / 30))
      cy = 120 + math.floor(60 * math.sin(t / 30))
      label = "SCROLL"
    elseif phase == 2 then
      local s = 48 + math.floor(60 * (1 + math.sin(t / 24)))
      w, h = s, s
      label = "RESIZE"
    else
      angle = (t * 3) % 360
      label = "SPIN"
    end
    pd.draw_image(it.handle, cx, cy, w, h, angle)
    centered_text(26, "IMAGE TEST 64x64: " .. label, 0xffe040ff)
  end

  -- DVD screensaver: bouncing tinted logo + the rare perfect-corner payoff.
  if st.active.dvd and st.a_dvd and pd.draw_image then
    local d = st.a_dvd
    pd.draw_image(d.handle, math.floor(d.x), math.floor(d.y), d.hw * 2, d.hh * 2, 0, DVD_COLS[d.ci])
    if d.flash > 0 then
      centered_text(30, "PERFECT CORNER!", 0xffff40ff)
      centered_text(42, "corners hit: " .. d.corners, 0xffffffff)
    end
  end

  -- (game_over now opens the real engine mission-failed dialog — no painted
  -- overlay needed.)

  -- CAPTCHA: the verification demand + the live task instruction.
  if st.active.captcha and st.a_cap and st.a_cap.task and not st.a_cap.done then
    pd.draw_box(96, 46, 128, 26, 0x000000a0)
    centered_text(50, "PROVE YOU ARE HUMAN", 0xffe040ff)
    centered_text(61, task_label(st.a_cap.task), 0xffffffff)
  end

  -- SPEED: pedometer bar.
  if st.active.speed and st.a_run then
    local r = st.a_run
    local frac = math.min(1, r.done / r.need)
    centered_text(50, string.format("RUN: %d / %d", math.floor(r.done), r.need),
        frac >= 1 and 0x40ff40ff or 0xffe040ff)
    pd.draw_box(110, 60, 100, 5, 0x00000090)
    pd.draw_box(110, 60, math.max(1, math.floor(100 * frac)), 5,
        frac >= 1 and 0x40ff40ff or 0xff8020ff)
  end

  -- Popup framework panels (pop quiz / EULA / lore). One shared look: a
  -- centred dark card with a title bar and body lines. Lines are word-
  -- wrapped to the card width (long EULA clauses used to draw past the
  -- card edge).
  local function popup_card(title, lines, footer)
    local X, Y, W2 = 48, 58, 224
    local wrapped = {}
    for _, ln in ipairs(lines) do
      if ln == "" then
        wrapped[#wrapped + 1] = ""
      else
        for _, wln in ipairs(wrap_lines(ln, W2 - 12)) do
          wrapped[#wrapped + 1] = wln
        end
      end
    end
    local H = 24 + #wrapped * 9 + (footer and 12 or 4)
    pd.draw_box(X, Y, W2, H, 0x000000d8)
    pd.draw_box(X, Y, W2, 11, 0x202848f0)
    centered_text(Y + 2, title, 0xffe040ff)
    for i, ln in ipairs(wrapped) do
      pd.draw_text(X + 6, Y + 14 + (i - 1) * 9, ln, 0xffffffff)
    end
    if footer then
      centered_text(Y + H - 10, footer, 0x80ff80ff)
    end
  end

  if st.active.pop_quiz and st.a_quiz and not st.a_quiz.done then
    local cur = st.a_quiz.deck and st.a_quiz.deck[st.a_quiz.idx]
    if cur then
      popup_card(string.format("POP QUIZ  (%d/%d)", st.a_quiz.idx, st.a_quiz.total), {
        cur.q,
        "",
        "1) " .. cur.a,
        "2) " .. cur.b,
      }, "FIRE = 1    AIM = 2    (wrong answer hurts)")
    end
  end

  if st.active.eula and st.a_eula then
    local pages = {
      { "1. By continuing to exist in this simulation you",
        "   accept all effects, past, present and future.",
        "2. Chaos is provided AS IS with no warranty of",
        "   fitness for any purpose, including fun." },
      { "3. The licensor is not liable for damage caused",
        "   by falling pianos, live grenades, or Elvis.",
        "4. You waive the right to complain in chat.",
        "5. Sections 1-4 apply even if unread." },
      { "6. This agreement renews every time you blink.",
        "7. Void where prohibited. Prohibited where void.",
        "8. Thank you for choosing Chaos(tm).",
        "" },
    }
    local pg = math.min(st.a_eula.page, 3)
    popup_card(string.format("END USER LICENSE AGREEMENT  (%d/3)", pg),
        pages[pg],
        st.a_eula.task and task_label(st.a_eula.task) or "FIRE to accept this page")
  end

  -- (lore now opens the real CI Information menu — no popup.)
end)

if pd.menu_add then
  -- One "Chaos" submenu in the Lua Director. At the TOP: the master switch and
  -- the two global knobs (how long each effect lasts, how often one fires) as
  -- tap-to-cycle entries whose labels rewrite in place (pd.menu_set_label).
  -- Below them: every effect, alphabetical by label, as an ON/off toggle that
  -- adds/removes it from the random rotation. All state persists via pd.persist.
  local GROUP = "Chaos"
  local INTERVALS = { 5, 10, 15, 20, 30, 45, 60, 90, 120 }
  local DURATIONS = { 5, 10, 15, 20, 30, 45, 60, 90, 120, 180 }

  -- Next value strictly greater than cur, wrapping to the smallest. Works even
  -- if the persisted value isn't itself a list entry.
  local function next_in(list, cur)
    for _, v in ipairs(list) do if v > cur then return v end end
    return list[1]
  end

  local i_toggle, i_dur, i_freq

  local function lbl_toggle() return "Chaos: " .. (st.enabled and "ON" or "off") end
  local function lbl_dur()    return "Effect duration: " .. st.effectdur .. "s" end
  local function lbl_freq()   return "Trigger every: " .. st.interval .. "s" end

  local function relabel()
    if not pd.menu_set_label then return end
    pd.menu_set_label(i_toggle, lbl_toggle())
    pd.menu_set_label(i_dur, lbl_dur())
    pd.menu_set_label(i_freq, lbl_freq())
  end

  i_toggle = pd.menu_add(lbl_toggle(), function()
    chaos.handle("menu", "toggle"); relabel()
  end, GROUP)
  i_dur = pd.menu_add(lbl_dur(), function()
    st.effectdur = next_in(DURATIONS, st.effectdur); persist(); relabel()
  end, GROUP)
  i_freq = pd.menu_add(lbl_freq(), function()
    st.interval = next_in(INTERVALS, st.interval); persist(); relabel()
  end, GROUP)

  -- Effect list, sorted by display label (ties broken by internal name).
  -- Alpha (testbed) effects live in their own submenu below, not here.
  local function label_sort(a, b)
    local la = (chaos.effects[a].label or a):lower()
    local lb = (chaos.effects[b].label or b):lower()
    if la == lb then return a < b end
    return la < lb
  end
  local names, anames = {}, {}
  for name, e in pairs(chaos.effects) do
    if e.alpha then anames[#anames + 1] = name
    else names[#names + 1] = name end
  end
  table.sort(names, label_sort)
  table.sort(anames, label_sort)

  -- Test menus: category folders (a suggestion from the alpha batch). The old
  -- single "Chaos Test" list is split into sibling submenus by effect type —
  -- all root-level (a submenu inside a submenu crashes the menu engine at
  -- 3-deep scroll stacks, so folders are siblings, not nested). Selecting an
  -- effect fires it for a fixed 30s; timers run even with the master off.
  -- Categorisation is this one table — names not listed fall into the
  -- "Weapons & World" catch-all. Edit freely.
  local CATS = {
    { title = "Test: Visual & Audio", set = {
      mirror=1, untextured=1, watercolour=1, noir=1, shiny=1, midas=1,
      paint_red=1, toxic=1, blackout=1, disco=1, sepia=1, terminal=1,
      bit8=1, bit16=1, gameboy=1, crt=1, vhs=1, peephole=1, underwater=1,
      negative=1, thermal=1, cathedral=1, reversed=1, helium=1, demon=1,
      australia=1, tonal=1, muted=1, soundboard=1, kazoo=1, jukebox=1,
      widescreen=1, tallscreen=1, fisheye=1, tunnel_vision=1, vertigo=1,
      drunk=1, blink=1, assert_authority=1, giants=1, ant_farm=1,
      monsoon=1, blizzard=1, ring_ring=1, negative_zoom=1,
      -- graduated alpha batch
      vertigo2=1, blooper=1, dvd=1, hudvd=1, perfect_hills=1, max_blood=1,
      blood_rainbow=1, brandons_mod=1, teen_angst=1, wireframe_enemies=1,
      fake_objective=1, fake_objective_fail=1, hurricane2=1,
      bayblade=1, speen=1, barrel_roll=1, banana_peel=1,
      uwuify=1, piglatin=1,
      -- SA/HL2 wave 2 graduates
      dutch_angle=1, blind=1, fading_out=1, sleepy=1, virtualboy=1,
      buttsbot=1, no_hud=1, waytoodank=1, rainbow_world=1, prismatic=1,
      ipod_ad=1, nepotism=1,
    } },
    { title = "Test: Cheats", set = {
      fists=1, slomo=1, dkmode=1, smalljo=1, smallchars=1, goldeneye=1,
      cloak=1, xray=1, nightvision=1, marquis=1, godmode=1, one_punch=1,
      superhot=1,
    } },
    { title = "Test: Helpful", set = {
      arsenal=1, ammo_rain=1, heal=1, shields_up=1, cavalry=1, buddy=1,
      reinforce=1, lock_n_load=1, random_loadout=1, turbo=1, enemyshields=1,
      golden_gun=1, no_drops=1, freeze=1, nap_time=1, benny_hill=1, zombies=1,
      -- graduated alpha batch
      estus=1, new_glasses=1, psychosis=1, mine_trio=1, quad_laser=1,
      mediguns=1, tank=1, double_lx=1, two_handed=1, quad_handed=1,
    } },
    { title = "Test: Lethal", set = {
      self_destruct=1, misfire=1, weapon_jam=1, vampire=1, plague=1,
      thanos_snap=1, airstrike=1, boom=1, panic=1, intruder=1, predators=1,
      take_a_break=1, one_hp=1, dry_spell=1, amnesia=1, disarm=1,
      evil_twin=1, clone_army=1, skedar_ring=1, enemyrockets=1, karma=1,
      glass_cannon=1, backfire=1, nbomb_me=1, earthquake=1,
      quantum_leap=1, quantum_instability=1, gormless=1, woof_gas=1,
      -- graduated alpha batch
      hot_potato=1, martyrdom=1, booby_doors=1, russian_roulette=1,
      countdown=1, skedar_reaper=1, terminator=1, gun_jam2=1, enemy_ltk=1,
      speed=1, nitroglycerin=1, inflated_bullets=1, button_thief=1,
      helicopter=1, interceptor=1,
      no_shooting=1, pacifist=1, slow_bleed=1, death_chance=1, note_7=1,
      heavy_recoil=1,
      itchy_trigger=1, weeping=1,
    } },
  }
  local CATCHALL = "Test: Weapons & World"
  local function cat_of(n)
    for _, c in ipairs(CATS) do
      if c.set[n] then return c.title end
    end
    return CATCHALL
  end
  for _, name in ipairs(names) do
    local n = name
    local e = chaos.effects[n]
    pd.menu_add(e.label or n, function() chaos.trigger(n, "test", 30) end, cat_of(n))
  end

  -- Chaos Alpha: the new-suggestion testbed (see the CHAOS ALPHA block above).
  -- Same shape as Chaos Test — select to fire for a fixed 30s (fixeddur effects
  -- keep their own length) — but these are never in the random rotation.
  for _, name in ipairs(anames) do
    local n = name
    local e = chaos.effects[n]
    pd.menu_add(e.label or n, function() chaos.trigger(n, "alpha", 30) end, "Chaos Alpha")
  end

  -- Effect on/off list (adds/removes each from the random rotation), alphabetical.
  for _, name in ipairs(names) do
    local n = name
    local e = chaos.effects[n]
    local mi
    local function lbl() return (e.label or n) .. ": " .. (effect_enabled(n) and "ON" or "off") end
    mi = pd.menu_add(lbl(), function()
      if st.disabled[n] then
        st.disabled[n] = nil                 -- re-enable
      else
        st.disabled[n] = true                -- disable + end it if it's live
        if st.active[n] then stop_effect(n) end
      end
      persist_disabled()
      if pd.menu_set_label and mi then pd.menu_set_label(mi, lbl()) end
    end, GROUP)
  end
end

pd.log("chaos.lua loaded (" .. (st.enabled and "ENABLED" or "off") .. ") — /chaos on | /chaos list")
