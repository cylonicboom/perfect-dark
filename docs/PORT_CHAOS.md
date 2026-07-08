# Port-only: Chaos / Randomiser Mode (Lua)

Port-only chaos mode built on the Lua AI runtime: a timer fires a weighted
random effect every N seconds (Chaos Mod style), every effect is pure Lua
driven through `pd.*` native hooks, and an **external event ingress** lets any
outside process (a Twitch/YouTube chat bot, Streamer.bot, SAMMI, the
Archipelago client) inject commands. Designed as the effect backend for the
Archipelago randomiser (trap items / DeathLink → `chaos.trigger()`), with the
Twitch/YouTube window left deliberately open.

Everything is `#ifndef PLATFORM_N64` (rides the existing Lua runtime, which is
port-only). No wire-format changes, no `NET_PROTOCOL_VER` bump — chaos is a
local-machine feature; in netplay each effect acts on the local player/cheat
banks only (see "Netplay caveats").

**Status: compile-verified only.** The C hooks and the ingress build clean
(dedicated target); the Lua layer and every effect need a ROM-in-hand runtime
pass. Test checklist at the bottom.

---

## Architecture

```
Twitch/YT chat bot ──UDP──▶ 127.0.0.1:<Chaos.EventPort> ─┐
~ console  /chaos <text> ────────────────────────────────┤
                                                         ▼
                              luaExtEventPush(source, text)   [C ring queue]
                                                         │
scripts/ap/client.lua ── chaos.trigger()/handle() ──┐    ▼
scripts/chaos.lua  pd.on("tick") ── pd.ext_poll() ──┴─▶ chaos.handle(source, text)
                                                         │
                                          weighted timer / votes / triggers
                                                         ▼
                                     effects via pd.* native hooks (below)
```

Three layers:

1. **C ingress** — a small ring queue in `luaai_api.c` (`luaExtEventPush()`,
   32 entries, 16-byte source + 128-byte text, overflow drops oldest), drained
   from Lua via `pd.ext_poll()`. Fed by:
   - the `~` console command `/chaos <text>` (`port/src/net/net.c`), and
   - a **localhost-only UDP listener** (`netChaosEventDrain()`, `net.c`):
     config `Chaos.EventPort` in `pd.ini` (default **0 = off**). The socket is
     bound to `127.0.0.1` via `enet_address_set_ip` — it can never be a remote
     ingress; anything on the wider network must go through a local bot
     process. Lazily opened on first drain after the port is set; drains up to
     16 datagrams per tick; trailing newlines stripped.
2. **Lua engine** — `scripts/chaos.lua`: effect table, weighted random picker
   (4-deep history to avoid instant repeats), timed-effect expiry, vote
   windows, persistence (`pd.persist_get/set` — enabled/interval/votetime
   survive stage reload), Lua Director pause-menu entries, `pd.on("stage")`
   cleanup.
3. **Native hooks** — the `pd.*` API surface (below). New effects are pure
   Lua; no rebuild needed.

## Control protocol

All three ingress paths (console, UDP, Lua) land in `chaos.handle(source,
text)`. One command per line/datagram:

| Command | Effect |
|---|---|
| `on` / `off` / `toggle` | Enable/disable the random drumbeat (persisted) |
| `status` | Log enabled/interval/votetime/active-count |
| `list` | Log all effect names |
| `interval N` | Seconds between random effects (min 5, default 30, persisted) |
| `votetime N` | Vote window length in seconds; 0 = vote mode off (persisted) |
| `trigger <effect> [who]` | Fire an effect immediately (channel-point style); `who` shows in the HUD announce |
| `vote <1\|2\|3\|name>` | Vote for a slate candidate by number or name; winner fires when the window closes (off-slate votes ignored) |
| `seed N` | `math.randomseed(N)` — deterministic effect stream (AP per-slot seeding) |
| `say <text>` | HUD message passthrough (chat shoutouts) |

Console: `/chaos on`, `/chaos trigger yeet TwitchUser42`. Bare `/chaos` sends
`status`. UDP smoke test (after setting `EventPort=27110` under `[Chaos]` in
`pd.ini`):

```
echo "trigger mirror" | nc -u -w0 127.0.0.1 27110
```

External events are drained even while chaos is **disabled**, so `on` can
arrive over UDP; `trigger` also works while the random drumbeat is off (pure
"chat controls the game" mode: `interval` high or `off` + direct triggers).

## HUD + vote slate (the Twitch/YouTube on-screen foundation)

chaos.lua registers a `pd.on("draw")` overlay in the **top-right corner**
(x=232, the overlay right column — above the octree/perf overlays):

- **Active-effect timers**: up to 5 rows, each an item-pickup-style bar —
  effect label + a dark backing box with a filled fraction that drains as
  the effect's time runs out (`st.duration` recorded at trigger).
- **Vote slate** (only while `votetime > 0`): "VOTE NEXT:" + the 3 candidate
  effects numbered 1–3 with live vote counts, and a window-countdown bar.

**Vote mode** (`votetime N` > 0) now *replaces* the random drumbeat: each
window, 3 distinct candidates are drawn (weighted, history-avoided); chat
votes by slate number (`vote 1`) or candidate name (`vote yeet`) — anything
off-slate is ignored; when the window closes the winner fires (ties and
zero-vote windows pick a random candidate — chaos must flow) and a fresh
slate is drawn. A Twitch/YouTube bot only has to forward chat "1"/"2"/"3"
messages as `vote N` datagrams to the UDP ingress; the slate panel is what
viewers read on stream. `votetime 0` returns to the solo drumbeat.

## Test menu

Every effect is registered as a `Test: <label>` entry in the **Lua
Director** pause-menu panel (sorted by internal name), alongside
"Chaos: toggle", "Chaos: random now", and "Chaos: vote 30s on/off".
Supporting C changes: `LUA_MENU_MAX` raised 24 → 160 (luaai.h) and the
Director dialog got `MENUDIALOGFLAG_SMOOTHSCROLLABLE` (mainmenu.c, the
endscreen long-content mechanism) so the ~100-entry list scrolls.

## Effect table (scripts/chaos.lua)

~72 effects, all self-cleaning. Weights (`w`) bias the random pick; `dur` in
seconds (0 = instant). Cheat-bank effects use the `cheat_effect(id, secs)`
factory (activate → timed deactivate). Timed effects may also carry a `tick`
function, called every frame while active (disco's hue cycle).

- **Arsenal**: `arsenal` (random gun + switch + ammo), `disarm` (take held
  weapon), `knife_fight`, `ammo_rain`, `lock_n_load` (every gun + full ammo),
  `amnesia` (take every gun), `dry_spell` (zero all ammo, weapons kept).
- **Cheat bank** (timed): `mirror`, `wireframe`, `tonal` (tonal inversion),
  `fists`, `slomo`, `dkmode`, `smalljo`, `smallchars`, `elvis`, `marquis`,
  `enemyrockets`, `enemyshields`.
- **Player state**: `godmode` (10s invincible), `cloak`/`xray`/`nightvision`
  (device on, timed), `heal` (+full shield), `blink` (white screen flash),
  `turbo` (15s Speed Pill boost, self-decays), `drunk` (tranq screen sway,
  wears off), `one_hp` (health roulette: 5–60%), `quantum_leap` (teleport to
  a random chr).
- **World**: `panic` (alert every chr), `yeet` (fling every chr away from the
  player), `boom` (explosion at a random chr), `airstrike` (explosions at up
  to 4 random chrs), `intruder` (20s stage alarm), `predators` (all chrs
  cloak for 20s), `buddy` (spawn ally), `reinforce` ("Supply drop" — a random
  gun dropped at a random chr).
- **Ammo roulette** (`pd.ammo_swap` — every held gun fires another weapon's
  primary rounds, with periodic refills of the borrowed ammo):
  `rocket_rounds` ("Rockets for everyone"), `grenade_rounds` ("Grenade
  machine gun", the Devastator's grenades from anything), `golden_gun`
  (DY357-LX one-hit-kill rounds), `farsight_rounds` (wall-piercing),
  `sedative_rounds` (tranq darts).
- **Requests batch 3**: `nbomb_me` ("N-Bomb delivery" — storm on the player),
  `hurricane` (whole map shoved one random direction: chrs, pushable
  objects, and you), `cyclone_frenzy` ("CYCLONE FRENZY", 30s — dual Cyclones
  forced to Magazine Discharge + Unlimited Ammo No Reloads), `widescreen` /
  `tallscreen` (20s projection stretch, 2:1 / 1:2), `cavalry` (4 co-op
  buddies), `jukebox` (60s random unlocked Combat Sim track over the stage
  music), `skedar_ring` ("Skedar ambush" — 4 mini Skedar in a circle around
  the player, alerted, facing in), `body_snatch` ("BODY SNATCHED", weight 1 —
  the Counter-Op takeover: you become a random guard, disguised; permanent
  for the rest of the level, solo only).
- **FOV warps** (`pd.fov_scale`, the aspect-scale sibling): `fisheye`
  ("Quake Pro", ×1.6), `tunnel_vision` (×0.55), `vertigo` (15s sine pulse
  via the `tick` driver).
- **Crowd control**: `infighting` ("Civil war" — every chr's combat AI
  targeted at the next chr in the list), `neuralyzer` ("Neuralyzed" — zero
  alertness + cleared targets, everyone forgets you), `house_party` (every
  living chr teleported into a ring around the player, ground-validated via
  `chrMoveToPos`), `evil_twin` (weight 2 — a hostile copy of the PLAYER's
  body carrying your held weapon spawns nearby; `pd.spawn_body(-1, …)`).
- **Doors**: `open_sesame` (every door on the stage opens at once),
  `lockdown` (every door closes — transient, they re-trigger).
- **`joyride`** — a personal HALF-SIZE hoverbike spawns in front of the
  player (`pd.spawn_bike`: the setup.c OBJTYPE_HOVERBIKE recipe built at
  runtime, extrascale 128, floor-snapped; one static instance — retriggering
  summons the existing bike back; solo only, no syncid). The hardcoded
  90-unit collision-cylinder radius in propobj.c now scales with extrascale,
  so the half bike genuinely fits where a full one wouldn't.
- **`soundboard`** — "Soundboard" (20s, `pd.sfx_shuffle`): every one-shot
  SFX plays as a random other sound (remapped inside `sndStart` just before
  the `g_NumSounds` validity check — always a real sound-table entry).
- **`kazoo`** — "Discount orchestra" (60s, `pd.instrument_shuffle`): every
  MIDI program change picks a random instrument from the loaded bank
  (remapped before the `instCount` bounds check in the sequence player's
  `AL_MIDI_ProgramChange` handler). Program changes fire at track start, so
  the effect pairs the toggle with a random `pd.song`; NRG/death stingers
  mid-stage shuffle too.
- **`gormless`** — "Gormless" (20s): movement AND look fully inverted —
  forward/back, strafe left/right, and both look axes. Implemented at the
  two input chokepoints in `bmoveProcessInput`: the c1-stick negate (covers
  gamepad + the port's keyboard-to-stick mapping, movement and stick-look
  alike) and the `inputMouseGetScaledDelta` negate (whole mouse look).
  Scripted autowalk is exempt (the CHEAT_MIRROR `bwalkUpdateTheta` lesson —
  synthetic input aims at a world target); stacks honestly with the user's
  invert-pitch option and with the `mirror` effect (mirror + gormless
  horizontal = double negation = normal, which is its own kind of funny).
- **`one_punch`** — "ONE PUNCH" (25s): Hurricane Fists + fists-only (the
  per-effect `tick` snaps the held weapon back to unarmed if the player
  switches) + `pd.one_punch` — every unarmed strike is lethal through any
  armour and launches the guard flying (`chrDamage` boost + `chrYeetFromPos`
  at force 250, applied before the SVC_CHR_DAMAGE broadcast so net clients
  replay the same hit; NPC victims only, co-op partners take normal fists).
- **`backfire`** — "Backwards bullets" (15s): every shot (bullets, rockets,
  tracers) leaves 180° behind the player; the crosshair stays put. Turn
  around to hit what's in front of you.
- **`self_destruct`** — "SELF-DESTRUCT SEQUENCE" (8s): invincibility on, then
  the Air Force One crash block (`playerSurroundWithExplosions` — staggered
  explosions around the player), then both off. Looks lethal, isn't — to
  you; nearby NPCs genuinely catch the blasts.
- **Visual** (renderer + room lighting; all timed, all local-cosmetic):
  `untextured` ("1996 mode" — every texture white, pure vertex shading),
  `watercolour` (every texture flooded with its own average colour),
  `noir` (forced grayscale), `paint_red` ("Paint the town red" — the KotH
  hill highlight applied to every room), `toxic` (green tint), `blackout`
  (near-dark blue tint), `disco` (hue-cycling room lighting via the
  per-effect `tick` driver).
- **Retro era pair** (`pd.pixelate` + `pd.audio_crush`): `bit8` ("8-bit
  era" — frame pixelated to a 160×120 grid + 4-level greyscale, audio
  sample-and-held to ~5.5 kHz @ 8-bit) and `bit16` ("16-bit era" — 256×192
  grid + 256 displayable colours (RGB 3-3-2), audio ~11 kHz @ 10-bit).
  Video is a post pass over the finished frame (world + viewmodel + HUD) in
  `port/fast3d/gfx_retro.cpp`, dispatched at `gfx_run`'s tail through the
  nullable `retro_filter` rapi entry (the `rt_resolve` pattern): capture the
  framebuffer colour (MSAA-resolving blit, or `glCopyTexSubImage2D` for the
  default framebuffer), then one fullscreen shader that snaps UVs to the
  grid (`GL_NEAREST` — each block is one point-sampled source pixel) and
  quantizes colours. Implemented on **both backends**: GL in
  `gfx_retro.cpp`; SDL_GPU (Vulkan/D3D12) in `gfx_sdlgpu.cpp`'s retro
  section — one same-format copy of `fb.color` + one fullscreen pipeline
  (the RT fullscreen VS with rect (0,0,1,1) is identity in image space, so
  the single round-trip doesn't flip), GLSL450 through the glslang/
  SPIRV-Cross pipeline, and the `rt_resolve` hand-back idiom (`st.pass`/
  `st.bound_pipeline` cleared + `vs_dirty`/`fs_dirty` re-set). Like the RT
  resolve, the SDL_GPU path **requires MSAA off** (logs once and no-ops on
  a multisample fb). Globals `gfx_retro_pixel_w/h`,
  `gfx_retro_colors` (0 keep / 2..64 grey levels / ≥256 RGB332). Audio is a
  bitcrush at the `audioEndFrame` push point (the `pd.mute` mutable-copy
  mechanism): sample-and-hold every `step`th stereo frame (device rate
  22 kHz ÷ step) masked to `bits` depth, hold phase continuous across
  buffer pushes; the external one-shot (`pd.play_file`) is mixed first so
  it crunches too, and mute wins over crush.

Adding an effect = one table entry in `chaos.effects` + `/lua reload`.

## Native hooks added for chaos (proto-safe, local-only)

New `pd.*` bindings in `src/game/luaai_api.c`, backed by `chraiLua*` helpers
in `src/game/chraction.c` (declared in `src/include/game/luaai.h`, all guarded
by the `apLuaPlayerChr()` pawn-null checks):

| Binding | Backing | Notes |
|---|---|---|
| `pd.cheat(id, on)` | `cheatActivate`/`cheatDeactivate` | id bounds 0..63 (two 32-bit banks) |
| `pd.cheat_active(id)` | `cheatIsActive` | |
| `pd.sound(sfxnum)` | `sndStart(var80095200, ...)` | non-positional UI sting |
| `pd.take_weapon(num)` | `chraiLuaTakeWeapon` | `invRemoveItemByNum` + `bgunCycleBack` if held |
| `pd.weapon_held()` | `chraiLuaWeaponHeld` | `bgunGetWeaponNum(HAND_RIGHT)`, −1 = no pawn |
| `pd.switch_weapon(num)` | `chraiLuaSwitchWeapon` | `bgunEquipWeapon2(HAND_RIGHT, ...)` |
| `pd.fade(r,g,b,a,time60)` | `chraiLuaScreenFade` | `playerSetFadeColour` + `playerSetFadeFrac` |
| `pd.chr_yeet(chrnum, force)` | `chraiLuaYeetChr` | `chrYeetFromPos` away from the player (default force 100) |
| `pd.explosion(chrnum, type)` | `chraiLuaExplodeAtChr` | `explosionCreateSimple` at the chr (default type 9) |
| `pd.ext_poll()` | ring queue pop | returns `source, text` or `nil`; also drains the UDP socket |
| `pd.device_off(num)` | `currentPlayerSetDeviceActive(num, false)` | the `device_on` inverse — clears the `devicesactive` bit; the timed gadget effects (cloak/xray/nightvision) call it in `stop` |
| `pd.lvupdate()` | `g_Vars.lvupdate60` | game ticks elapsed this frame — 0 while paused; all chaos timers (effects, vote window, drumbeat) advance by it, so pausing can't run out a bad effect |
| `pd.alarm(on)` | `alarmActivate`/`alarmDeactivate` | server-side; SVC_ALARM (proto 85) mirrors to clients |
| `pd.boost(secs)` | `bgunAddBoost` | Speed Pill boost; self-decays via `bgunTickBoost`; ≤0 cancels |
| `pd.player_set_health(frac)` | `bondhealth` write | clamped 0.01..1 — never kills |
| `pd.dizzy(amount)` | `blurdrugamount` write | tranq screen-sway, 0..4000 (below the TICKS(5000) KO band), decays naturally |
| `pd.chr_cloak(chrnum, on)` | `CHRHFLAG_CLOAKED` bit | same flag as the cloaking device; IR scanner still reveals |
| `pd.strip_ammo()` | `bgunSetAmmoQuantity(type, 0)` loop | all ammo types 1..`AMMOTYPE_ECM_MINE` |
| `pd.teleport_to_chr(chrnum)` | `chrSetPos` | the netcode's player force-position primitive; server-side |
| `pd.flattex(mode)` | `gfx_flattex_mode` (gfx_pc.cpp) | 0 off / 1 white / 2 average-colour textures; applied by a texture-cache reimport at the next frame boundary; per-pixel **alpha preserved** so fonts/HUD stay readable; HD ext-tex falls back to the (flattened) N64 decode while active |
| `pd.grayscale(on)` | `gfx_force_grayscale` → `rdp.grayscale` | forces `SHADER_OPT_GRAYSCALE` with a neutral colour (both GL and SDL_GPU honour it); the game never emits `G_SETGRAYSCALE_EXT`, so no contention |
| `pd.room_tint(r,g,b)` / `()` | `g_ChaosRoomTintFrac` (dlights.c) | stage-wide room-lighting multiplier — `kohHighlightRoom`'s math applied to every room at both `scenarioHighlightRoom` sites; dirties all rooms (`ROOMFLAG_BRIGHTNESS_DIRTY_TEMP`, the paintroom pattern) |
| `pd.explosions_around(on)` | `playerSurroundWithExplosions` / `bondexploding` | the Air Force One crash loop (`playerTickExplode` spawns `EXPLOSIONTYPE_BONDEXPLODE` around the player every 15–30 ticks); damage respects `pd.invincible` (the chr damage handler early-outs, but explosions still spawn) |
| `pd.nbomb()` | `nbombCreateStorm` | the thrown N-Bomb's impact call, at the player's feet, player-owned |
| `pd.gust(force)` | `chrYeetFromPos` + `objApplyMomentum` + `bondshotspeed` | one random compass direction for the whole map: chrs flung from a virtual point behind them, objects via the explosion-knockback gate (`!MOUNTED && !GRABBED && OBJFLAG3_PUSHABLE`), local player via the shot-knockback velocity |
| `pd.dual_wield(weaponnum[, funcnum])` | `invGiveSingle/DoubleWeapon` + `bgunEquipWeapon2` both hands | the `playerSpawnAnti` dual-wield recipe + full ammo; funcnum 0/1 forces that fire function on both hand gsets (1 = Cyclone Magazine Discharge) |
| `pd.aspect_scale(mult)` | `g_ChaosAspectMult` (playermgr.c) | multiplier inside `playermgrSetAspectRatio` — playerTick re-derives natural aspect every tick, so the hook must live in the setter and restore is automatic; 2 = wide, 0.5 = tall, clamped 0.25..4 |
| `pd.song(slot)` / `()` | `musicStartTrackAsMenu(mpGetTrackMusicNum(slot % unlocked))` / `musicEndMenu` | the credits-roll mechanism: stage music pauses underneath, resumes on stop; only unlocked Combat Sim tracks |
| `pd.spawn_body(bodynum[, weaponnum, dx, dz])` | `chrSpawnAtCoord` | the `chraiLuaSpawnAlly` recipe with allegiance inverted: TEAM_ENEMY, GAILIST_ALERTED, `CHRCFLAG_TRIGGERSHOTLIST`, facing the player; weaponnum −1 = unarmed (melee bodies) |
| `pd.body_snatch(chrnum)` | `playerSpawnAnti` + `player->disguised` | the real Counter-Op takeover: player teleports into the chr's body (weapons/health/shield/third-person model copied, host chr freed) + the disguise flag so guard AI ignores you until blown (gailists.c patroller logic). **Solo only, one-way for the rest of the level** — the engine has no return-to-Jo path |
| `pd.fov_scale(mult)` | `g_ChaosFovMult` (playermgr.c) | multiplier inside `playermgrSetFovY` (the aspect-scale pattern); clamped 0.4..2.2; zoom/Gun-FOV interplay untested |
| `pd.chr_target(chrnum, victim)` | `chr->target` via `propGetIndexByChrId` | the `aiSetTargetChr` recipe + alertness 100 + `CHRCFLAG_TRIGGERSHOTLIST` |
| `pd.chr_calm(chrnum)` | `alertness = 0`, `target = -1`, trigger-shot flag cleared | doesn't rewind the AI script — stops the hunt until re-provoked |
| `pd.doors_all(open)` | `doorsRequestMode` on every `PROPTYPE_DOOR` | returns the door count; closing is transient |
| `pd.chr_summon(chrnum, dx, dz)` | `chrMoveToPos` with the player's rooms | ground-validated; fails cleanly (returns false) if the spot doesn't validate |
| `pd.gormless(on)` | `g_ChaosGormless` → `bmoveProcessInput` (bondmove.c) | negates the c1 stick (safe + raw) and the mouse-look deltas; local player only, autowalk exempt |
| `pd.spawn_bike()` | runtime `hoverbikeobj` template + `objInitWithModelDef` + `setupCreateHov` | half size via `extrascale=128` + `modelSetScale` (the `setupCreateObject` semantics); the propobj.c geo-cyl radius now scales with extrascale (stage bikes at 256 are byte-identical); solo only; one static instance, revalidated via the `prop->obj` backlink across stage reloads |
| `pd.sfx_shuffle(on)` | `g_ChaosSfxShuffle` → `sndStart` (src/lib/snd.c) | remap to `LCG % g_NumSounds` after the MP3 branch, before the validity check; local LCG so game RNG is untouched |
| `pd.instrument_shuffle(on)` | `g_ChaosInstrumentShuffle` (u8!) → `AL_MIDI_ProgramChange` (n_csplayer.c) | remap to `LCG % bank->instCount`; **1-byte extern like `g_SndTonalInversion`** — never declare as game `bool`; audio thread, so local LCG only |
| `pd.one_punch(on)` | `g_ChaosOnePunch` → `chrDamage` boost (chraction.c) | player + `WEAPON_UNARMED` + NPC victim → damage = maxdamage+shield+100 and `chrYeetFromPos(victim, attacker, 250)`; boosted before the `SVC_CHR_DAMAGE` broadcast |
| `pd.backfire(on)` | `g_ChaosBackfire` (bondgun.c) | rotates the camera-space shot ray 180° about the vertical axis at the end of `bgunCalculatePlayerShotSpread` — every consumer (hitscan traces, `bgunCreateFiredProjectile` velocities, tracers, aim detection) fires behind the player, vertical aim preserved; local player only (remote pawns keep true direction) |
| `pd.ammo_swap(weaponnum)` / `()` | `g_ChaosAmmoSwapWeapon` (game_0b0fd0.c) | the gset function getters return the swap weapon's PRIMARY for the local player's **hand gsets only** (pointer-compared against `hands[].gset`), so menus/inventory/NPC AI/remote pawns keep the real function; held weapon must be in the FALCON2..CROSSBOW gun range (knife excluded); target validated SHOOT-type at set time |

Renderer notes: the flat-texture filter lives at the single
`gfx_upload_tex_filtered` chokepoint in `gfx_pc.cpp` (all nine N64-format
import paths decode to RGBA32 in `tex_upload_buffer` and upload through it),
so both backends get it for free. Mode/grayscale toggles are applied in
`gfx_start_frame`: a flattex change calls `gfx_texture_cache_clear()` (which
already `dlcacheInvalidateAll()`s — cached segments hold the old texture
ids), a grayscale change drops just the dlcache (cached leaves baked the old
shader choice). These globals **survive stage reloads** (unlike the cheat
bank), so chaos.lua's `stage` handler resets `flattex`/`grayscale`/`room_tint`
explicitly. All three hooks are pure-cosmetic: no game state, net-safe,
save-safe.

Pre-existing bindings chaos reuses: `give_weapon`, `refill_ammo`,
`invincible`, `device_on`, `player_heal`, `player_set_shield`, `all_chrs`,
`chr_alert`, `spawn_ally`, `spawn_at_chr`, `hud_message`, `persist_get/set`,
`menu_add`, `on`, `log`.

## Twitch / YouTube integration (the open window)

The contract is deliberately tiny: **one UDP datagram of plain text to
127.0.0.1:`Chaos.EventPort`, in the protocol above**. Any chat-bot stack can
speak it today:

- **Custom bot** (any language): connect to Twitch IRC / YouTube Live chat
  API, map `!chaos yeet` → send `trigger yeet <username>`; map channel-point
  redemptions → `trigger <effect> <redeemer>`; map bits/superchats → `vote`.
- **Streamer.bot / SAMMI / Mix It Up**: use their UDP-send action with the
  command string; sub-alert → `trigger boom <subname>`, etc.
- **Vote overlays**: set `votetime 30`, forward each chat message matching
  `!vote <effect>` as a `vote <effect>` datagram; the winner fires
  automatically each window.

Future (not built): a *direct* `wss://` Twitch-IRC client in Lua is possible
by generalising the AP transport — `pd.ap_connect/send/poll` already speaks
generic text frames over ws/wss, but it is **single-socket** (one session
global in `luaai_api.c`), so using it for Twitch would steal the Archipelago
connection. The clean path is multi-session handles (`pd.ws_open() → id`).
Until then, the UDP bridge is the supported route.

## Archipelago integration

`scripts/ap/client.lua` can drive chaos directly (same Lua state):

- **Trap items**: in the AP item-received handler, map trap ids →
  `chaos.trigger("yeet", "AP trap from " .. sender)`.
- **DeathLink**: on a DeathLink bounce, `chaos.trigger("boom")` or a custom
  lethal effect.
- **Per-seed randomiser**: at connect time call `chaos.set_seed(slot_seed)`
  and `chaos.handle("ap", "on")` — the weighted effect stream is then
  deterministic per AP seed (same seed = same chaos schedule).

## Netplay caveats

- Cheat-bank effects (`mirror`, `slomo`, `dkmode`, …) follow the existing
  cheat rules — several are blocked or desync-prone in netplay; the cheat
  layer's own gates apply. Chaos does not sync anything: two netplay players
  each need their own chaos instance for a shared experience, or accept
  host-only visual chaos.
- `spawn_ally` / `spawn_at_chr` are server-authoritative in net games (the
  underlying spawn helpers are gated); on a client they no-op or act locally
  per the existing binding semantics.
- The UDP ingress is loopback-bound. Do not "fix" this by binding `0.0.0.0` —
  that turns a chat toy into an unauthenticated remote-control port.

## Files touched

| File | Change |
|---|---|
| `scripts/chaos.lua` | NEW — the chaos engine (effects/timer/votes/protocol) |
| `scripts/init.lua` | loads chaos.lua |
| `src/game/luaai_api.c` | 10 new bindings + `luaExtEventPush` ring queue |
| `src/game/chraction.c` | 7 `chraiLua*` helpers (take/held/switch weapon, fade, yeet, explosion, sound) |
| `src/include/game/luaai.h` | declarations for the above |
| `port/src/net/net.c` | `/chaos` console command; `netChaosEventDrain()` UDP listener; `Chaos.EventPort` config |
| `port/include/net/net.h` | `g_ChaosEventPort` / `netChaosEventDrain` declarations |

## Runtime test checklist (needs ROM)

1. Boot, `~`, `/chaos list` — all effect names print; `/chaos trigger heal`
   announces on the HUD.
2. `/chaos on`, play a mission ~2 minutes — an effect every 30s, timed ones
   announce "wore off" and actually revert (watch `mirror`/`slomo`).
3. Timed-effect restart: `/chaos trigger mirror` twice quickly — timer resets,
   no stuck state.
4. Stage transition mid-effect — cheat resets with the stage, no "wore off"
   ghost announces, first post-load effect waits a full interval.
5. UDP: set `EventPort=27110`, `nc -u` a `trigger yeet` from the same machine
   (works) and from another machine (must NOT work).
6. Vote mode: `votetime 15` — the 3-candidate slate + countdown bar appear
   top-right; send `vote 1`/`vote 2` datagrams and watch the counts tick up;
   winner fires at window close and a fresh slate draws; a zero-vote window
   fires a random candidate ("dealer's choice"). `votetime 0` clears the
   panel and the drumbeat resumes.
6b. HUD: trigger two timed effects (`mirror` + `turbo`) — two labelled bars
   appear top-right and drain in sync with the effect timers, disappearing
   as each wears off. Check they don't collide with the octree/perf overlays
   if those are on.
6c. Test menu: pause → Lua Director — "Test: <label>" entries for every
   effect, list scrolls smoothly past one screen, selecting one fires it.
7. `seed 12345`, note the first 5 effects; `/lua reload`, `seed 12345` again —
   same 5 effects.
8. AP smoke: `ap.connect` to the mock server, call
   `chaos.trigger("boom", "ap")` from the console — HUD announce shows.
9. Visuals: `/chaos trigger untextured` (world goes flat-shaded, HUD text
   still readable), `watercolour` (flat but coloured), `noir`, `paint_red`
   (all rooms red like a stage-wide hill), `disco` (colour cycles), each
   reverting after its timer; trigger `untextured` on BOTH renderers (GL and
   `--renderer sdlgpu`) and once with an HD texture pack loaded (pack should
   flatten too, then come back). Change stage mid-`noir` — new stage must
   load un-grayscaled.
10. Self-destruct: `/chaos trigger self_destruct` — explosions ring the
   player for 8s, zero damage taken (watch health bar), nearby NPCs do get
   hurt, then it stops cleanly and damage resumes (get shot to confirm
   invincibility actually lifted).
11. Ammo swap: `/chaos trigger rocket_rounds` — Falcon/CMP/etc fire real
   rockets; check reload still cycles, ammo refills keep coming, and the
   effect expiring restores normal rounds. Repeat `golden_gun` (one-hit
   kills, hitscan) and `grenade_rounds`. While active, open the pause-menu
   weapon inventory — descriptions may show the swap weapon (cosmetic,
   expected). Confirm NPC fire is unaffected.
12. Batch 3: `nbomb_me` (storm envelops you, disorientation, wears off);
   `hurricane` (NPCs + crates + you all lurch the same way — repeat a few
   times for different directions); `cyclone_frenzy` (both hands Cyclones,
   trigger dumps the whole clip, never reloads for 30s, reverts);
   `widescreen`/`tallscreen` (world stretches, HUD should stay usable, snaps
   back after 20s AND on stage change); `cavalry` (4 buddies, all fight
   enemies); `jukebox` (song plays, stage music resumes after 60s);
   `skedar_ring` (4 mini Skedar spawn around you already aggro — watch for
   wall clipping at spawn); `body_snatch` (you become a guard: his gun,
   his body in cutscenes/third-person, guards ignore you until you fire
   near them; confirm mission objectives still completable or accept the
   level is a wash — it's weight 1 for a reason).
