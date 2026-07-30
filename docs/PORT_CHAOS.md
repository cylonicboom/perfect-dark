# Port-only: Chaos / Randomiser Mode (Lua)

Port-only chaos mode built on the Lua AI runtime: a timer fires a weighted
random effect every N seconds (Chaos Mod style), every effect is pure Lua
driven through `pd.*` native hooks, and an **external event ingress** lets any
outside process (a Twitch/YouTube chat bot, Streamer.bot, SAMMI, the
Archipelago client) inject commands. Designed as the effect backend for the
Archipelago randomiser (trap items / DeathLink → `chaos.trigger()`), with the
Twitch/YouTube window left deliberately open.

Everything is `#ifndef PLATFORM_N64` (rides the existing Lua runtime, which is
port-only). No wire-format changes, no `NET_PROTOCOL_VER` bump.

> **Chaos is SINGLE-PLAYER ONLY, by design.** New effects do **not** need to be
> netplay-safe, host-authoritative or wire-synced — gate them off when
> `g_NetMode` is set instead. This is a deliberate scope decision, and it is
> worth stating because the code can suggest otherwise: several existing effects
> carry `g_NetMode` / `NETMODE_CLIENT` guards inherited from the conventions of
> the decompiled files they are spliced into. Those guards are **incidental, not
> intent** — they are harmless and are being left in place, but do not read them
> as a promise that chaos replicates. Anything that mutates world/prop/chr state
> (bouncing drops, model shaping, physics tweaks) would otherwise have to
> reconcile with the prop-sync paths, and that cost is explicitly not being
> paid. See "Netplay caveats" for what the older effects happen to do today.

**Status: largely runtime-confirmed, actively iterated.** The retro/audio-filter
suite, teleport family (`quantum_leap`), and the K7-style weapon arming are
runtime-confirmed; the frequency/duration config and the Director submenu UI are
the newest additions (compile-verified, runtime pass in progress). Individual
effects are still being triaged — see the memory notes. Test checklist at the
bottom.

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
| `interval N` | Seconds between random effects (min 5, **default 20**, persisted) |
| `effectdur N` (alias `duration`) | Global length of every timed effect in seconds (min 1, **default 60**, persisted) |
| `votetime N` | Vote window length in seconds; 0 = vote mode off (persisted) |
| `toasts [on\|off]` | Show the per-effect "CHAOS: &lt;name&gt;" / "wore off" corner toast. **Defaults OFF** so effects fire with nothing on screen hinting Chaos did it (no arg toggles; persisted). Effects are still written to the log either way, and Chaos enabled/disabled messages always show. Effects flagged `silent` (Fake Crash, the fake-objective pair) never toast even when this is on — their whole gag is that nothing identifies them. |
| `trigger <effect> [who]` | Fire an effect immediately (channel-point style); `who` shows in the HUD announce |
| `set <effect>` | Fire an effect and **hold it on indefinitely** — it never ticks down and never wears off (no countdown bar). Instant effects (dur 0) have no state to hold and say so. Cleared by `unset`, by anything calling `stop_all` (Chaos `off`, the Supersonic flush) or by a stage change. |
| `unset [<effect>\|all]` | Release a held effect (no arg = all). |
| `vote <1\|2\|3\|name>` | Vote for a slate candidate by number or name; winner fires when the window closes (off-slate votes ignored) |
| `seed N` | `math.randomseed(N)` — deterministic effect stream (AP per-slot seeding) |
| `say <text>` | HUD message passthrough (chat shoutouts) |

Console: `/chaos on`, `/chaos trigger panic TwitchUser42`. Bare `/chaos` sends
`status`. UDP smoke test (after setting `EventPort=27110` under `[Chaos]` in
`pd.ini`):

```
echo "trigger mirror" | nc -u -w0 127.0.0.1 27110
```

External events are drained even while chaos is **disabled**, so `on` can
arrive over UDP; `trigger` also works while the random drumbeat is off (pure
"chat controls the game" mode: `interval` high or `off` + direct triggers).

## HUD + vote slate (the Twitch/YouTube on-screen foundation)

chaos.lua registers a `pd.on("draw")` overlay in the **top-left corner** (x=8,
the Lua HUD left margin, by the Combat Sim kill count):

- **Active-effect timers**: up to 5 rows, each an item-pickup-style bar —
  effect label + a dark backing box with a filled fraction that drains as
  the effect's time runs out (`st.duration` recorded at trigger). Blue
  (`C_BAR`).
- **One-off acknowledgement bars** (2026-07-28): up to 3 rows, same shape,
  **green** (`C_BARONE`) — see below.
- **Vote slate** (only while `votetime > 0`): "VOTE NEXT:" + the 3 candidate
  effects numbered 1–3 with live vote counts, and a window-countdown bar.

### One-off acknowledgement bars

An instant effect (`dur` 0/nil) never enters `st.active`, so it used to fire
with **nothing on screen at all** — you'd notice the consequence but get no
confirmation chaos caused it, especially with toasts defaulted off. Instant
effects now get a short bar of their own (`ONEOFF_BAR`, 3s — matched to the
Snap so the two read as the same kind of quick flash), in a distinct **green**
so it reads as "this just happened" rather than as a duration still running.

They live in **`st.oneoff`, deliberately NOT `st.active`**. `st.active` drives
`stop_effect`, the "wore off" announce, the sticky / Worst Day top-ups and every
is-it-running check — none of which an instant effect should touch. `st.oneoff`
is a plain display list of `{label, life, total}`, ticked next to the active
expiry (so it still drains when Chaos itself is disabled, for `/chaos trigger`),
cleared in the per-stage reset, and capped to the newest 3 on screen because
Combo Time fires three effects at once and the lo-res screen is only ~220 tall.

**Two opt-outs, and this is what the existing flags are for:**

| Flag | Meaning | Instant effects using it |
|---|---|---|
| `silent` | Nothing on screen may hint chaos is involved — no toast, and now no bar | `fake_objective`, `fake_objective_fail` |
| `nobar` | Draws its own HUD, *or* is a prank that still wants a toast but must never show a chaos bar | `game_over` |

Both are folded into one helper, `is_deniable(e)`, which gates **the bar and the
trigger sting**. **Any new prank effect whose gag depends on deniability must set
one of those two flags**, or the bar and the sound will give it away.
`game_over` is the worked example: it isn't `silent` (the toast is part of the
reveal, when toasts are on at all) but it is `nobar`, because a green
"Game over?" bar next to a real-looking mission-failed dialog kills the joke
instantly.

### Trigger sting (2026-07-28)

A universal "an effect just fired" cue, played by `play_trigger_sting()` from
`start_effect` for every effect, timed or instant, except the deniable ones.
Unlike the toasts it never says *what* fired — it's a cue, not a spoiler — so
it's on by default and independent of the toast setting.

Pick it in **Extended > Chaos > Trigger Sound** (one row per sound; selecting a
row **plays it immediately** as a preview and marks it with `>`), or with
`/chaos sound [list|next|<key>]`. Persisted as `chaos_trigsound` by key, not
index, so adding or reordering sounds later can't silently change someone's
choice. An unknown key resolves to the default rather than to `off` — silently
muting the cue would look like a bug.

Options are the `TRIGSOUNDS` table: `off`, ~19 built-in game sounds (menu blips,
error buzz, cloak on/off, pickups, glass shatter, explosion, alarms, Mauler
charge, Maian scream, throw), `random` (a different built-in every fire), and
`external`.

**Nothing here can stop the music**, which is the one hard requirement:

- Built-ins go through `pd.sound` → `chraiLuaPlaySound` → `sndStart`, an
  ordinary non-positional SFX.
- `external` plays `scripts/chaos/sounds/chaostrigger.wav` or `.mp3` through
  `pd.play_file(..., loop=false, followMusic=false)` → `audioPlayExternal`,
  which mixes into **its own voice slot** and never touches the music track.
  (`audioPlayExternal` sniffs WAV-vs-MP3 by *content*, so either extension
  works regardless of what the file is called.)
- The Silo countdown's music cut is an explicit `pd.stage_music(false)` inside
  that effect — the effect deliberately doing it, not a side effect of playing
  a file. Nothing in the sting path can inherit that.

The external result is cached in `st.trigsound_ok` (`nil` untried / `false`
missing): with no file present the audio layer logs a "can't load" warning per
attempt, which would otherwise spam the log on every single effect. Switching
sound resets the cache so a newly-added file is picked up.

**SFX ids are raw hex numbers** because `pd.sound` takes a number and Lua has no
view of the `sfx.h` enum (the pre-existing `SFX_MAIAN_ARGH` hardcodes are the
precedent). They were derived by walking the enum in `src/include/sfx.h` and
validated two ways: every self-naming constant matches its own hex
(`SFX_805E == 0x805e`), and `0x05df`–`0x05e1` came out as exactly the Maian
sounds already hardcoded in chaos.lua. **Validate the same way if you add more**
— don't eyeball a line number, most of the enum is implicit.

The `CHAOS: <name>` announce is a separate **bottom-left** weapon-pickup-style
toast (`st.toast`): white text on a dark box sized to hug the text via the
`pd.text_size` binding (the engine hudmsg box is a full line-height tall,
leaving a gap below the letters), held then faded. Chaos also stays fully
dormant in the main-menu hub / title stages (`MENU_STAGES` + `pd.stage()`),
and a return to the menu force-ends every effect (`reset_all_modes`).

**Vote mode** (`votetime N` > 0) now *replaces* the random drumbeat: each
window, 3 distinct candidates are drawn (weighted, history-avoided); chat
votes by slate number (`vote 1`) or candidate name (`vote panic`) — anything
off-slate is ignored; when the window closes the winner fires (ties and
zero-vote windows pick a random candidate — chaos must flow) and a fresh
slate is drawn. A Twitch/YouTube bot only has to forward chat "1"/"2"/"3"
messages as `vote N` datagrams to the UDP ingress; the slate panel is what
viewers read on stream. `votetime 0` returns to the solo drumbeat.

## Where settings are saved (`$S/lua_persist.txt`, 2026-07-21)

`pd.persist_set` / `pd.persist_get` (the C-owned KV in `luaai_api.c` that
outlives the per-stage `lua_State` teardown) is **backed by a text file in the
save dir**, next to `pd.ini` — so chaos settings survive quitting the game, not
just a stage load. Before this the store was RAM-only and every menu toggle was
forgotten on exit.

```
# Perfect Dark - persistent script settings (pd.persist_set).
# Rewritten by the game whenever a setting changes.
chaos_disabled=disco,jelly,superhot
chaos_effectdur=60
chaos_enabled=1
chaos_interval=20
```

- `chaos.lua` needed **no changes** — it already routed every menu-adjustable
  setting through `persist()` / `persist_disabled()`. Keys: `chaos_enabled`,
  `chaos_interval`, `chaos_effectdur`, `chaos_votetime`, `chaos_disabled`
  (comma-separated OFF list), `chaos_recent` (non-repeat queue).
- Loaded once, lazily, on the first `persist_get`/`persist_set` — which happens
  while `chaos.lua` builds its state, so scripts always see saved values.
- Rewritten in full after any *changed* value (unchanged writes are skipped).
  Delete the file to reset every script setting to defaults.
- **No escaping**: entries whose key contains `=` or whose key/value contains a
  newline are dropped on write. Values may contain `=` (the reader splits on the
  first one). Store 32 keys max (`LUA_PERSIST_MAX`); scripts currently use 6.
- **Session-only keys** (2026-07-30): a key starting with `~` is kept in the
  C table but **never written to or read from the file**, and setting one skips
  the file rewrite entirely. For state that must outlive the per-stage
  `lua_State` teardown but *not* the process — see the restart carry-over below.

## Restarting a mission does not clear effects (2026-07-30)

Restarting was the cheapest possible escape from a bad roll, so in-progress
effects now **resume with their remaining time** instead of being wiped. The
clock is never restarted: an effect with 8s left comes back with 8s left, and
restarting repeatedly keeps shrinking the remainder exactly as if you'd played
on — all a restart buys you is the loading time.

- **Why they vanished**: a restart passes through a pawn-less loading window,
  which the tick handler's hub gate (`pd.player_pos(0) == nil`) reads as "left
  gameplay" and answers with a full `reset_all_modes()`. Nothing to do with the
  `lua_State` teardown — that only fires when the stage NUMBER changes
  (`luaai.c` `luaaiExecute`), which a same-mission restart doesn't do.
- **`carry_save(stage)`** runs at that gate *before* `reset_all_modes`, and
  serialises the live timed effects as `stage;name:left:total:sticky;…` into the
  session-only key **`~chaos_carry`**. Keyed on `st.play_stage` (the stage we
  were *playing*, tracked each gameplay tick) — by the time the gate fires
  `pd.stage()` may already read as the hub.
- **`carry_restore(stage)`** runs on the first gameplay tick after any
  non-gameplay window, gated on the `st.resume_armed` latch (set by
  `reset_all_modes`, and **initially true** so a fresh state is checked too). It
  re-runs each effect's `start()` — necessary, because `lvReset` cleared the
  chaos C globals — then pins the saved `left`/`total` over what the effect
  would otherwise get. Consumed once; announced as the system message
  "Nice try" so a resumed effect doesn't read as a bug.
- **Why the persist store and not a field on `st`**: the other route to the same
  exploit — abort to the Carrington hub, re-select the mission — *does* change
  the stage number, which destroys the whole `lua_State` and `st` with it. A
  `~` key lives in C for the process, so it survives that; and because it never
  reaches disk, an effect can't be resurrected after quitting the game.
- **What is dropped**: a snapshot belonging to a different mission (play
  something else and it's discarded), remainders under 0.5s, and everything on
  **mission COMPLETE** (`carry_clear()` — beating the level is not an escape).
  Instant effects were never in `st.active`, so they never carry. Sticky
  (`/chaos set X`) pins carry too.
- Logic is unit-tested standalone against the bundled interpreter
  (serialise/parse round-trip, the shrinking-remainder property, stage
  mismatch, consume-once, a `start()` that errors). **Runtime PENDING.**

## Frequency & duration (configurable)

Two global knobs, both persisted (`pd.persist`) and adjustable from the console
(`/chaos interval N`, `/chaos effectdur N`) or the pause-menu (below):

- **Frequency** (`st.interval`, default **20s**) — seconds between random
  effects.
- **Effect duration** (`st.effectdur`, default **60s**) — the on-screen length
  of *every* timed effect. An effect's own `dur` field is now just a
  timed-vs-instant marker: any positive `dur` runs for `st.effectdur`; `dur = 0`
  stays instant. `chaos.trigger` applies this in one place.
  - **Exemption**: an effect can set `fixeddur = true` to keep its own authored
    length instead of the global. Used by `take_a_break` (freezes the *player*)
    and `freeze` (freezes NPCs) — a 60s player-freeze would be a soft-lock, so
    they keep their short randomised timers.

## Pause-menu UI (Lua Director submenus)

The Lua Director groups entries into **submenus**. `pd.menu_add(label, fn,
[group])` takes an optional third arg — a submenu title; entries sharing a title
collapse into a pushable sub-dialog, and the "> Title" opener is placed at the
**top** of the Director root (openers always sit above any flat root entries, in
first-registered order). Omit the group for a root-level entry.

Current groups:

- **Chaos** — the controls submenu. At the top, three tap-to-cycle entries that
  rewrite their own label in place via `pd.menu_set_label(index, text)` (the
  Director menuitem points at the live label buffer, so no rebuild): `Chaos:
  ON/off` (master drumbeat), `Effect duration: Ns`, `Trigger every: Ns`. Below
  them, **every effect alphabetically (by label) as an `<label>: ON/off` toggle**
  that adds/removes it from the random rotation. The disabled set is a
  comma-separated `chaos_disabled` persist key; `pick_random` (and thus the vote
  slate) skips disabled effects; disabling a live effect ends it.
- **Chaos Test** — a sibling submenu (opener next to Chaos at the Director top):
  every effect alphabetically; selecting one fires it for a fixed **30s** to try
  in isolation (`chaos.trigger(name, "test", 30)` — the `dur_override` arg forces
  the length regardless of `st.effectdur`). These timers **count down even while
  the master switch is off**: the tick handler runs the timed-effect expiry loop
  unconditionally and only gates the random drumbeat / vote on `st.enabled`.
- **Chaos Alpha** — the new-suggestion **testbed** (the `alpha_effects` block in
  chaos.lua, 2026-07-12 Discord batch: Gun Game v2, Hurricane v2, Blooper,
  Martyrdom, Terminator, Skedar King, CAPTCHA, SPEED, Russian roulette, fake
  game-over, Estus flask, phone-call Dokkaebi, classic weapons, …). Same shape
  as Chaos Test (select → fires for a fixed 30s; `fixeddur` effects keep their
  own length) but alpha effects are **never in the random rotation or vote
  slate** (`pick_random` skips `e.alpha`) and have no on/off toggles. Batch 2
  (same day) added 13 more backed by new C bindings — Secondaries only, XBLA
  mode, Weapon jam v2, Inflated bullets, Objective scramble, Nitroglycerin,
  Back to the start, Button thief, Perfect hills (fog), Max blood, Technicolor
  blood, Item swap, Brandon's mod — each `start` errors with "needs new exe"
  when run against an older binary. To promote
  a graduate, move it out of `alpha_effects` into `chaos.effects` and drop the
  flag. Alpha-only plumbing: a second `pd.on("draw")` handler (Blooper splats,
  fake game-over overlay, CAPTCHA/SPEED/countdown HUD), `weaponfound` hook
  (Mediguns), kill-hook branches that run for **any** death (Martyrdom,
  Booby-trapped drops), and `st.a_boom_off` — a main-tick countdown that shuts
  off a brief `explosions_around` burst (the C side fires exactly ONE staggered
  blast ~250-400 units out in a ~12-tick window; that's the "Live grenade!" /
  SPEED-failure boom).
- **Mission Director** (`scripts/director.lua`) and **Archipelago**
  (`scripts/ap/test.lua`) group their own entries the same way.

**Nesting**: `pd.menu_add`'s group arg may contain `/` (e.g. `"Chaos/Test"`) and
`luaDirectorRebuild` resolves the tree recursively (opener placed at the top of
the parent dialog, parent auto-created). **Caveat: a 3-deep scrollable stack
(root Director → Chaos → Test, all `SMOOTHSCROLLABLE`) crashes the menu engine
in `menuitemListTick` — two deep is fine, so keep submenus at the root level for
now** (Chaos Test is a root sibling, not nested under Chaos). `LUA_MENU_MAX` is
256 to fit chaos's Test triggers + on/off toggles alongside the other tools.

Supporting C changes: the registry entry carries a `group[LUA_MENU_LABEL]`
string (`luaai_api.c`); `luaDirectorRebuild` (mainmenu.c) builds one
`menudialogdef` + item array per distinct group (up to
`LUA_DIRECTOR_MAX_SUBMENUS = 12`), wiring openers as `SELECTABLE_OPENSDIALOG`
items whose `handler` field holds the child dialog (read by
`menuPushDialog((menudialogdef *)item->handler)` in menuitem.c). `LUA_MENU_MAX`
is 160 and the dialogs carry `MENUDIALOGFLAG_SMOOTHSCROLLABLE` so long lists
(the ~74 test triggers) scroll.

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
  `enemyshields`.
- **Arm all NPCs** (timed give + restore, the `arm_all_effect(label, w, pick)`
  factory): on start it snapshots each NPC's current weapon (`pd.chr_weapon`)
  and hands out a new one; on stop it gives the originals back. `pick` is a fixed
  weaponnum or a per-NPC function. Members: `k7_party` (K7 for all),
  `enemyrockets` ("Enemy rockets!" — **now a rocket-launcher give**, replacing
  the old `CHEAT_ENEMYROCKETS` projectile-swap so it restores cleanly),
  `weapon_roulette` (each NPC a different random gun). `pd.chr_give_weapon`
  handles both actor kinds: campaign guards fire straight from `weapons_held`,
  so it swaps the held prop; simulants (`chr->aibot`) re-pick from a bot
  inventory each tick, so it adds the weapon to the inventory with ammo and
  drives `botinvSwitchToWeapon`. Restoring an originally-unarmed guard clears the
  hands (weapon `-1` has no hand model).
- **Player state**: `godmode` (10s invincible), `cloak`/`xray`/`nightvision`
  (device on, timed), `heal` (+full shield), `blink` (white screen flash),
  `turbo` (15s Speed Pill boost, self-decays), `drunk` ("One too many": tranq
  screen sway + a 180-flipped double-vision ghost overlaid on the frame for the
  duration, `pd.double_vision`), `one_hp` (health roulette: 5–60%),
  `quantum_leap` (teleport to a random chr).
- **World**: `panic` (alert every chr), `boom` (explosion at a random chr), `airstrike` (explosions at up
  to 4 random chrs), `intruder` (20s stage alarm), `predators` (all chrs
  cloak for 20s), `buddy` (spawn ally), `reinforce` ("Supply drop" — a random
  gun dropped at a random chr).
- **`me_and_my_son`** ("Me and my son", Chaos Alpha testbed) — a friendly Jo
  clone (`pd.spawn_ally_clone`, the player's own body/head on TEAM_ALLY) with
  half HP and a 40%-height / full-width squash (`pd.chr_yscale(c, 0.4)`), so she
  fights beside you as a squat, wide runt. A death-watch in the main tick
  (`st.a_son`, modelled on the Weeping Skedar watcher — independent of any effect
  timer) polls her health; the frame she falls she takes **half of the player's
  remaining HP** with her (`pd.player_set_health(h * 0.5)`, which floors at 0.01
  so it can't be the killing blow itself). Alpha for now (needs the two new
  bindings — a fresh exe): graduate by dropping `alpha=true` / `w=0` and giving
  it a weight.
- **Ammo roulette** (`pd.ammo_swap` — every held gun fires another weapon's
  primary rounds, with periodic refills of the borrowed ammo):
  `rocket_rounds` ("Rockets for everyone"), `grenade_rounds` ("Grenade
  machine gun", the Devastator's grenades from anything), `golden_gun`
  (DY357-LX one-hit-kill rounds), `farsight_rounds` (wall-piercing),
  `sedative_rounds` (tranq darts).
- **Requests batch 3**: `nbomb_me` ("N-Bomb delivery" — storm on the player),
  `hurricane` (whole map shoved one random direction: chrs, pushable
  objects, and you), `cyclone_frenzy` ("CYCLONE FRENZY", 30s — dual Cyclones
  forced to Magazine Discharge + Unlimited Ammo No Reloads, plus `pd.gun_lock`:
  secondary forced, trigger auto-held, weapon switching disabled), `widescreen` /
  `tallscreen` (20s projection stretch, 2:1 / 1:2), `cavalry` (4 co-op
  buddies), `jukebox` (60s random unlocked Combat Sim track over the stage
  music), `skedar_ring` ("Skedar ambush" — 4 mini Skedar in a circle around
  the player, alerted, facing in), `body_snatch` ("BODY SNATCHED", weight 1 —
  a **lite** Counter-Op takeover, solo only: you teleport onto a random guard,
  take its weapon, and it vanishes, with a best-effort disguise flag. The *full*
  third-person-body takeover (`playerSpawnAnti`) is **not used** — the solo
  first-person player has no chr body model and building one mid-mission stalls
  the cutscene-only gunmem swap; see the binding note).
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
  forward/back, strafe left/right, and both look axes. Implemented at
  three input chokepoints in `bmoveProcessInput`: the c1-stick negate
  (analog gamepad + keyboard-mapped-to-stick), the `inputMouseGetScaledDelta`
  negate (whole mouse look), and — after the control-mode routing — a swap of
  the finalised `digitalstep{forward,back,left,right}` flags. That last one is
  the fix for the long-standing "movement doesn't flip, only look" bug:
  `CONTROLMODE_PC` (keyboard) derives forward/back/strafe from the
  U/D/L/R_CBUTTONS **step buttons**, never the stick, so the stick negate
  alone never reached keyboard movement. Scripted autowalk is exempt (the
  CHEAT_MIRROR `bwalkUpdateTheta` lesson — synthetic input aims at a world
  target); stacks honestly with the user's invert-pitch option and with the
  `mirror` effect (mirror + gormless horizontal = double negation = normal,
  which is its own kind of funny).
- **`australia`** — "Australia mode" (20s): the whole finished frame is
  rotated **180°** (world + viewmodel + HUD) and the controls are reversed to
  match. The rotation is a post-process — `pd.upside_down(on)` sets
  `gfx_rotate180_mode`, and the retro post filter flips the sample UV
  (`uFx` bit 64, `uv = 1 - uv`, shared shader body in `gfx_retro_common.h`, so
  GL + SDL_GPU both get it) after everything is drawn. This replaces the old
  clip-space Y-flip (`gfx_upsidedown_mode`, still present but no longer set by
  any effect), which left the HUD upright and inverted only vertical aim.
  Control reversal reuses Gormless's chokepoints via `g_ChaosControlReverse`
  (OR'd with `g_ChaosGormless`), set alongside the rotation in `chraiLuaUpsideDown`
  — but **forward/back stays normal** (you still walk into the scene); only
  look (both axes) + strafe left/right flip. The `digitalstepforward/back` swap
  is gated on `g_ChaosGormless` alone, so Australia leaves walk untouched while
  Gormless flips everything.
  Both `g_ChaosControlReverse` and `gfx_rotate180_mode` are cleared in `lvInit`
  so a mid-stage Lua death can't leave the screen rotated / controls reversed.
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
- **`pirate`** ("Pirate", Chaos Alpha testbed) — an eyepatch: blacks out
  the left or right half of the screen (random per fire) as a post-process, so
  the HUD in that half goes dark too (`pd.pirate(1|2)`; cleared with
  `pd.pirate(0)`). Timed like the other visual effects.
- **`perrep_daad` / `fecttcef_rkkr`** ("PERREP DAAD" / "FECTTCEF RKKR", Chaos
  Alpha testbed) — the effect name is the title card with the effect applied:
  mirror the left half of the finished frame onto the right (or the right onto
  the left) about the vertical centre line, kaleidoscope style, HUD included
  (`pd.half_mirror(1|2)`, retro-fx bits 0x2000/0x4000; cleared with
  `pd.half_mirror(0)`). A post-process like One Too Many — both backends share
  the shader body, no new uniforms.
- **`jelly`** ("Jelly", Chaos Alpha testbed) — true on-the-fly **vertex
  deformation**: the whole scene wobbles like jelly (`pd.vertex_wobble`, an
  eye-space per-vertex displacement in the renderer — not a post-process). The
  effect just advances the ripple phase each tick.
- **`acid_trip`** ("Acid trip", Chaos Alpha testbed) — the full trip: walls
  and characters **melt** (vertex wobble + downward `sag`), the frame smears
  (**hall of mirrors**, `pd.hall_of_mirrors`), and the **colours cycle**
  (Prismatic hue field). Vertex mods + trails + trippy colour, combined.
- **`space_program`** (Chaos Alpha testbed) — every player bullet is a
  one-hit kill that launches the victim with massive knockback (`pd.space_program`).
- **`frag_out`** (Chaos Alpha testbed) — human enemies throw a grenade
  whenever they'd fire a weapon (`pd.frag_out`).
- **`sentries_out`** (Chaos Alpha testbed) — 2-8 hostile laptop sentry guns
  spawn in a ring around the player at random offsets (`pd.spawn_sentry`); they
  stay until destroyed or the stage ends.
- **`temu_mag`** (Chaos Alpha testbed) — reloading pays the full ammo cost
  but only partly refills the magazine (`pd.temu_mag`).
- **`yassify`** — **NOT registered as a chaos effect** (pulled 2026-07-28). The
  shaping works, but the waist cinch propagates through the entire torso, so the
  proportions don't read as an hourglass yet; it needs per-joint compensation
  first. The C side (`pd.yassify`, the `chrHandleJointPositioned` hook) and the
  `/yassify waist|shoulder|neck N` tuning command are still present for
  development — re-add an `alpha_effects` entry once it looks right.
- **`rubber_objects`** ("Rubber Objects", Chaos Alpha testbed) — anything
  **dropped into the world** while the effect runs (enemy corpse drops,
  disarms/surrenders, your own dropped gun, thrown grenades) bounces like rubber
  instead of thudding down after the vanilla 6 bounces (`pd.rubber_objects`).
  Opt-in **at the drop**, deliberately: guns and crates already lying around the
  map never start twitching. Single-player only.
- **`helpful_son`** (Chaos Alpha testbed) — a toddler on the second pad:
  at random intervals grabs an input for 0.3-0.7s (look sweep / fire / walk /
  weapon fumble).
- **`beat_game`** (Chaos Alpha testbed) — a rhythm game: shoot ON the music
  beat for bonus damage, slightly off for normal, badly off and the recoil hurts
  you. Syncs to the live music tempo (`pd.music_bpm` / `pd.music_beat`) with a
  120-BPM visual-metronome fallback when no sequenced track plays. Pulsing beat
  HUD; bonus lands on `pd.aim_chr()`.
- **`silo_countdown`** ("Silo Countdown", Chaos Alpha testbed) — a
  self-destruct running `chaos.silo_seconds` (default 8:30). On start it
  kills the mission music (`pd.stage_music(false)`) and plays `Silo.mp3` once
  (`pd.play_file`, best-effort, no loop) underneath — the final-stretch music is baked
  into that single track now, so there is **no** mid-countdown swap — and draws
  its own centred MM:SS clock (red-flashing in the final 10s); at zero it
  detonates — `pd.explosions_around` the player (unlike `self_destruct`, **no**
  invincibility, so it can genuinely kill). Mission-complete or a player restart
  tears the whole thing down (music restored, track stopped, pending boom
  cancelled) via `stop()` + `reset_all_modes`. The length lives in
  `chaos.silo_seconds` so a test harness can shrink it — `scripts/silo_test.lua`
  (`/lua silo_test()` or the Chaos Alpha menu's "Silo Countdown (1-min test)")
  fires it at 60s and restores the default. Alpha for now (needs a
  fresh exe + the `Silo.mp3` asset): graduate by dropping
  `alpha=true` / `w=0`.
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
  `st.bound_pipeline` cleared + `vs_dirty`/`fs_dirty` re-set). **MSAA is
  supported on both backends**: GL's capture blit resolves implicitly; on
  SDL_GPU a multisample fb captures via `fb_readable_color()` (an empty
  `RESOLVE_AND_STORE` render pass into the fb's own single-sample
  `fb.resolve` texture — sampleable while drawing back, so no copy) and
  the draw-back pipeline is built per target sample count
  (`retro_pipe_for`, `rt_make_pipeline_ms`). `fb_readable_color` is THE
  capture pattern for any future SDL_GPU post pass that reads the frame
  back. (The RT suite still requires MSAA off on SDL_GPU — that's a
  *depth* limitation: SDL_GPU has no depth resolve and can't sample
  multisample depth; colour is solved.) Globals `gfx_retro_pixel_w/h`,
  `gfx_retro_colors` (0 keep / 2..64 grey levels / ≥256 RGB332 / 1000
  invert / 1001 Game Boy / 1002 thermal). Audio is a bitcrush at the
  `audioEndFrame` push point (the `pd.mute` mutable-copy mechanism):
  sample-and-hold every `step`th stereo frame (device rate 22 kHz ÷ step)
  masked to `bits` depth, hold phase continuous across buffer pushes; the
  external one-shot (`pd.play_file`) is mixed first so it crunches too,
  and mute wins over crush.
- **Post-filter looks** (the same retro pass, fragment body shared between
  backends in `port/fast3d/gfx_retro_common.h`; `pd.screen_fx(bits, on)`
  sets/clears composable fx bits — 1 scanlines, 2 RGB aperture grille,
  4 CRT curvature, 8 vignette, 16 VHS, 32 wobble — and `pd.lens(k)` is a
  fisheye warp, centre magnified / corners pinned): `crt` ("Tube TV",
  `pd.crt` = bits 1|2|4|8 — scanlines curve with the tube, grille rides
  physical pixels via `gl_FragCoord`), `vhs` ("Camcorder" — chroma shift +
  per-line jitter + a drifting tracking band + noise, animated by a
  backend-local frame counter passed as `uTime`), `peephole` (lens 1.4),
  `underwater` ("Submerged" — sine UV wobble + reverb 0.35), `gameboy`
  ("Handheld mode" — 160×144 + 4 DMG greens + crush), `negative` ("Film
  negative", full-res invert via `pd.pixelate(0, 0, 1000)` — w=0 means
  colour-mode-only), `thermal` ("Heat vision", luminance → heat palette).
- **Audio chain** (all at the `audioEndFrame` push point, order: reverse →
  pitch → radio → reverb → crush; each independently toggleable, states
  reset in the stage hook): `pd.audio_radio` (~400–2800 Hz bandpass + hard
  overdrive — wired into `sepia`/"1964 mode"), `pd.audio_reverb(wet)`
  (Freeverb-lite: 4 damped combs + 2 allpasses per channel, tunings halved
  for 22 kHz, R channel spread +12 — `cathedral` at 0.8), `pd.audio_reverse`
  (granular time reversal: fill one ~0.74 s chunk while playing the
  previous one backwards — `reversed` "!desreveR"), `pd.audio_pitch(rate)`
  (granular constant-tempo pitch shift: recent-input ring read at `rate`,
  grain-jump with a 64-frame crossfade on drift — `helium` 1.5 and `demon`
  0.65 + reverb).

Adding an effect = one table entry in `chaos.effects` + `/lua reload`.

## Native hooks added for chaos (proto-safe, local-only)

New `pd.*` bindings in `src/game/luaai_api.c`, backed by `chraiLua*` helpers
in `src/game/chraction.c` (declared in `src/include/game/luaai.h`, all guarded
by the `apLuaPlayerChr()` pawn-null checks):

| Binding | Backing | Notes |
|---|---|---|
| `pd.cheat(id, on)` | `cheatSetActive` (cheats.c) | id bounds 0..63. Routes normal cheats through `cheatActivate/Deactivate` (active bank) and **Experiments cheats (45-60: GoldenEye/Wireframe/Mirror/Tonal/Classic) through the ENABLED bank** — those are enabled-bank-only by design (cheatActivate refuses them so missions still save), so a plain `cheatActivate` was a no-op for them |
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
| `pd.double_vision(on)` | `gfx_doublevision_mode` (renderer) | "One too many": retro post filter blends rotated ghosts of the finished frame over the normal one — `uFx` 128/256/512 = 180/90/270, all three set, averaged then mixed at 0.6 (drunk kaleidoscope). 90/270 are backend-swapped but set together so the result matches on GL + SDL_GPU; cleared in `lvInit` |
| `pd.chr_cloak(chrnum, on)` | `CHRHFLAG_CLOAKED` bit | same flag as the cloaking device; IR scanner still reveals |
| `pd.strip_ammo()` | `bgunSetAmmoQuantity(type, 0)` loop | all ammo types 1..`AMMOTYPE_ECM_MINE` |
| `pd.set_ammo(ammotype, qty)` | `bgunSetAmmoQuantity` | set ONE pool to an exact quantity, others untouched (russian roulette's one Magnum round); type validated 1..`AMMOTYPE_ECM_MINE`, qty floored at 0. Pool only — a round already chambered in an equipped gun's clip isn't touched (same limitation as `strip_ammo`) |
| `pd.teleport_to_chr(chrnum)` | `chrSetPos` / direct prop move | Snap the local player *beside* a chr (offset by both radii along the approach direction), server-side. Validated with `chrAdjustPosForSpawn` (avoids walls + physics objects, nudges through a ring, falls back to the NPC's own pos/rooms). **Body-model player** (Combat Sim) uses `chrSetPos`; the **model-less solo player** (campaign — `chr->model == NULL`, which `chrSetPos`/`chrMoveToPos` would crash on) is moved by hand (prop pos + rooms + bondwalk `vv_*` view fields) |
| `pd.chr_weapon(chrnum)` | `aibot->weaponnum` / held prop | Current weapon of an NPC (`-1` if invalid). Snapshot before `chr_give_weapon` to restore it when a timed effect ends |
| `pd.flattex(mode)` | `gfx_flattex_mode` (gfx_pc.cpp) | 0 off / 1 white / 2 average-colour textures; applied by a texture-cache reimport at the next frame boundary; per-pixel **alpha preserved** so fonts/HUD stay readable; HD ext-tex falls back to the (flattened) N64 decode while active |
| `pd.grayscale(on)` | `gfx_force_grayscale` → `rdp.grayscale` | forces `SHADER_OPT_GRAYSCALE` with a neutral colour (both GL and SDL_GPU honour it); the game never emits `G_SETGRAYSCALE_EXT`, so no contention |
| `pd.room_tint(r,g,b)` / `()` | `g_ChaosRoomTintFrac` (dlights.c) | stage-wide room-lighting multiplier — `kohHighlightRoom`'s math applied to every room at both `scenarioHighlightRoom` sites; dirties all rooms (`ROOMFLAG_BRIGHTNESS_DIRTY_TEMP`, the paintroom pattern) |
| `pd.explosions_around(on)` | `playerSurroundWithExplosions` / `bondexploding` | the Air Force One crash loop (`playerTickExplode` spawns `EXPLOSIONTYPE_BONDEXPLODE` around the player every 15–30 ticks); damage respects `pd.invincible` (the chr damage handler early-outs, but explosions still spawn) |
| `pd.nbomb()` | `nbombCreateStorm` | the thrown N-Bomb's impact call, at the player's feet, player-owned |
| `pd.gust(force)` | `chrYeetFromPos` + `objApplyMomentum` + `bondshotspeed` | one random compass direction for the whole map: chrs flung from a virtual point behind them, objects via the explosion-knockback gate (`!MOUNTED && !GRABBED && OBJFLAG3_PUSHABLE`), local player via the shot-knockback velocity |
| `pd.dual_wield(weaponnum[, funcnum])` | `invGiveSingle/DoubleWeapon` + `bgunEquipWeapon2` both hands | the `playerSpawnAnti` dual-wield recipe + full ammo; funcnum 0/1 forces that fire function on both hand gsets (1 = Cyclone Magazine Discharge) |
| `pd.give_mags([n])` | `chraiLuaGiveMags` → `weaponGetAmmoByFunction` + `bgunSetAmmoQtyForWeapon` | Stock every ammo type with **n magazines** (default 2) instead of filling the reserve to capacity. A magazine is a property of the WEAPON FUNCTION, not the ammo type — several weapons share an ammo type with different clip sizes — so pass 1 walks every weapon × both functions recording the **largest** clip size per ammo type (largest, so n mags is sensible for whatever you're actually holding; undershooting on a Cyclone/Reaper would feel broken) and pass 2 writes n of those back, clamped to capacity. Goes through the public `bgun*ForWeapon` accessors because the ammo table is file-local to bondgun.c. Ammo types no weapon uses are left alone — zeroing them would strip gadgets and objective items sharing the array |
| `pd.gun_lock(on)` | `g_ChaosGunLock` → `bmoveProcessInput` (bondmove.c) + `amOpen` (activemenu.c) | Cyclone Frenzy: per-tick force `weaponfunc = FUNC_SECONDARY` both hands, `triggeron = true` (auto-fire), zero the weapon-cycle offsets, and block the weapon menu; local player, unpaused, alive; cleared in `lvInit` |
| `pd.backfire(on)` | `g_ChaosBackfire` → `bgunCalculatePlayerShotSpread` (bondgun.c) + `chrTick`/`chrTestHit` (chr.c) + `shotCalculateHits` (prop.c) | Backwards bullets. The ray flip itself was always fine; the effect never registered hits for **three** stacked reasons, all fixed 2026-07-28 — see "Backwards bullets: why it never worked" below |
| `pd.yassify(on)` | `g_ChaosYassify` + `chrChaosScaleXZY` → `chrHandleJointPositioned` (chr.c) | Yassify: non-uniform per-joint body shaping — waist XZ cinched, shoulders XZ flared, head scaled up (slightly wider than tall, so the face reads as cheekbones not a DK-mode balloon). Rides the existing per-joint callback that DK mode / flinch / aim-tracking already use, inside the same world-space round trip with the translation zeroed, so joint POSITIONS don't move — only the basis, and therefore the children hanging off it. Humans only (`CHRRACE == RACE_HUMAN`, like DK mode). **Scales COLUMNS, not rows**: `mtx4TransformVec` shows a row-vector convention, so columns 0/1/2 are the world-space output axes — scaling rows would scale along each joint's own axes and shear the model as it animates. World-space is safe here only because X and Z share one factor, making it invariant to the chr's Y facing. Multipliers are live-tunable (`/yassify waist\|shoulder\|neck N`) because a joint scale **propagates to that joint's children** — the waist cinch narrows everything above it, so the shoulder/neck values are compensations. Cosmetic only: no collision or hit box moves. Cleared in `lvResetChaosPerStage` |
| `pd.rubber_objects(on)` | `g_ChaosRubberObjects` + `PROJECTILEFLAG_CHAOSRUBBER` → `objSetDropped` / `projectileTick` (both propobj.c) | Rubber Objects: the mark is stamped at the **drop chokepoint** (`objSetDropped` — corpse drops, disarms, surrenders, the player's own drop, thrown grenades), so props already lying on the floor are never affected. In the bounce handler a marked projectile skips `projectileFall` while `bouncecount < 40` (vanilla settles at 6) and gets `speed.y` re-kicked to a hop that decays to 0 over that budget; restitution (`unk08c`) is raised to 0.7 at the drop, never lowered. Sticky projectiles (mines) excluded. Both the global **and** the mark are checked, so turning the effect off settles everything on its next contact — no mark sweep. Marking is gated on `g_NetMode == NETMODE_NONE` (single-player only); cleared in `lvResetChaosPerStage` |
| `pd.aspect_scale(mult)` | `g_ChaosAspectMult` (playermgr.c) | multiplier inside `playermgrSetAspectRatio` — playerTick re-derives natural aspect every tick, so the hook must live in the setter and restore is automatic; 2 = wide, 0.5 = tall, clamped 0.25..4 |
| `pd.song(slot)` / `()` | `musicStartTrackAsMenu(mpGetTrackMusicNum(slot % unlocked))` / `musicEndMenu` | the credits-roll mechanism: stage music pauses underneath, resumes on stop; only unlocked Combat Sim tracks |
| `pd.spawn_body(bodynum[, weaponnum, dx, dz])` | `chrSpawnAtCoord` | the `chraiLuaSpawnAlly` recipe with allegiance inverted: TEAM_ENEMY, GAILIST_ALERTED, `CHRCFLAG_TRIGGERSHOTLIST`, facing the player; weaponnum −1 = unarmed (melee bodies) |
| `pd.body_snatch(chrnum)` | disguise flag + guard teardown | **Lite** takeover (solo only): sets `CHRHFLAG_DISGUISED` on the player chr + `player->disguised`, then frees the guard (the `playerSpawnAnti` host-teardown: `chrRemove`/`propDeregisterRooms`/`propDelist`/`propDisable`/`propFree`). chaos.lua pairs it with `pd.teleport_to_chr` + `pd.give_weapon` to take the guard's place. **Full `playerSpawnAnti` is NOT used** — the solo first-person player has no third-person chr body model, and building one mid-mission stalls the cutscene-only gunmem swap (fights the live gun system → 4s lockscreen then revert). Disguise is best-effort: only the disguise-aware patroller ailist consults the flag; ordinary patrol/combat guards don't |
| `pd.fov_scale(mult)` | `g_ChaosFovMult` (playermgr.c) | multiplier inside `playermgrSetFovY` (the aspect-scale pattern); clamped 0.4..2.2; zoom/Gun-FOV interplay untested |
| `pd.chr_target(chrnum, victim)` | `chr->target` via `propGetIndexByChrId` | the `aiSetTargetChr` recipe + alertness 100 + `CHRCFLAG_TRIGGERSHOTLIST` |
| `pd.chr_calm(chrnum)` | `alertness = 0`, `target = -1`, trigger-shot flag cleared | doesn't rewind the AI script — stops the hunt until re-provoked |
| `pd.doors_all(open)` | `doorsRequestMode` on every `PROPTYPE_DOOR` | returns the door count; closing is transient |
| `pd.chr_summon(chrnum, dx, dz)` | `chrMoveToPos` with the player's rooms | ground-validated; fails cleanly (returns false) if the spot doesn't validate |
| `pd.gormless(on)` | `g_ChaosGormless` → `bmoveProcessInput` (bondmove.c) | negates the c1 stick (safe + raw), the mouse-look deltas, **and swaps the `digitalstep*` flags** (post-routing — catches CONTROLMODE_PC keyboard movement, which uses step buttons not the stick); local player only, autowalk exempt |
| `pd.upside_down(on)` | `gfx_rotate180_mode` (renderer) + `g_ChaosControlReverse` (bondmove.c) | "Australia mode": rotates the whole finished frame 180° via the retro post filter (`uFx` bit 64, GL + SDL_GPU) *and* reverses the controls (shares Gormless's three chokepoints via OR); both flags cleared in `lvInit`. Replaces the old clip-space Y-flip `gfx_upsidedown_mode` (now unset by any effect) |
| `pd.spawn_bike()` | runtime `hoverbikeobj` template + `objInitWithModelDef` + `setupCreateHov` | half size via `extrascale=128` + `modelSetScale` (the `setupCreateObject` semantics); the propobj.c geo-cyl radius now scales with extrascale (stage bikes at 256 are byte-identical); solo only; one static instance, revalidated via the `prop->obj` backlink across stage reloads |
| `pd.sfx_shuffle(on)` | `g_ChaosSfxShuffle` → `sndStart` (src/lib/snd.c) | remap to `LCG % g_NumSounds` after the MP3 branch, before the validity check; local LCG so game RNG is untouched |
| `pd.instrument_shuffle(on)` | `g_ChaosInstrumentShuffle` (u8!) → `AL_MIDI_ProgramChange` (n_csplayer.c) | remap to `LCG % bank->instCount`; **1-byte extern like `g_SndTonalInversion`** — never declare as game `bool`; audio thread, so local LCG only |
| `pd.one_punch(on)` | `g_ChaosOnePunch` → `chrDamage` boost (chraction.c) | player + `WEAPON_UNARMED` + NPC victim → damage = maxdamage+shield+100 and `chrYeetFromPos(victim, attacker, 250)`; boosted before the `SVC_CHR_DAMAGE` broadcast |
| `pd.backfire(on)` | `g_ChaosBackfire` (bondgun.c) | rotates the camera-space shot ray 180° about the vertical axis at the end of `bgunCalculatePlayerShotSpread` — every consumer (hitscan traces, `bgunCreateFiredProjectile` velocities, tracers, aim detection) fires behind the player, vertical aim preserved; local player only (remote pawns keep true direction) |
| `pd.ammo_swap(weaponnum)` / `()` | `g_ChaosAmmoSwapWeapon` (game_0b0fd0.c) | "Everything Rockets": every held gun fires the swap weapon's shot but keeps its **own animation, fire rate, and hand behaviour** (Paintball-style — the fire FUNCTION is *not* swapped). The swap is applied only at **shot creation**: hitscan swaps (Farsight/Tranq/LX) + firing noise via `gsetPopulateFromCurrentPlayer` presenting the swap weapon on the populated copy; projectile swaps (rocket/grenade) via a prop.c intercept in the `HANDATTACKTYPE_SHOOT` dispatch (`chaosAmmoSwapProjectile()` → `bgunCreateFiredProjectile`, one projectile per fire event at the held gun's cadence; `bgunCreateFiredProjectile` save/restore-overrides the held weapon to the swap weapon). Held weapon must be FALCON2..CROSSBOW (knife excluded); target validated SHOOT-low-byte at set time. Menus/inventory/NPC AI/remote pawns keep the real weapon |

### Batch 2 (2026-07-12, the Chaos Alpha C bindings)

| Binding | Backing | Notes |
|---|---|---|
| `pd.force_secondary(on)` | `g_ChaosForceSecondary` → `bmoveProcessInput` (bondmove.c) | "Secondaries only": per-tick pin of both hands' `gset.weaponfunc = FUNC_SECONDARY` — the gun_lock pattern minus auto-fire and the switch block |
| `pd.button_block(mask)` | `g_ChaosButtonMask` (bondmove.c) | strips N64 pad buttons from the `c1buttons` gameplay read AND from `c1allowedbuttons` (A 0x8000, B 0x4000, Z 0x2000, R 0x10, ext X/reload 0x40, ext Y/next-weapon 0x80, C-pad 8/4/2/1); the `c1allowedbuttons` strip matters because aim-mode toggle, fire-while-aiming, reload and next-weapon are per-sample `joyGetButtons*OnSample` reads that never see `c1buttons`. Menus read the joy layer directly so the pause menu always works; kb/mouse route through the same virtual pad. Bind reality check (input.c pckbbinds/pcjoybinds): kbm has NO bind on A 0x8000, pads have NO bind on B 0x4000 — steal the right INTERACT per device (Button Thief does). **Reset in lvReset — a stale mask = a permanently lost input** |
| `pd.ammo_cost(mult)` | `g_ChaosAmmoCost` (bondgun.c) | "Inflated bullets": tops up the clip decrement at the single `loadedammo -= shotstotake` site; shots unchanged, ammo drains ×mult; local player only |
| `pd.weapon_jam(2)` | `g_ChaosWeaponJam` mode 2 (bondgun.c) | "jam v2": ~35% of pulls dry-fire (rngRandom at the mode-1 reroute), and a shot that fires drains the rest of the magazine at the decrement site — reload after every bang. `true`/1 = classic full jam |
| `pd.autoaim(on)` | `g_ChaosAutoAim` → `optionsGetAutoAim` (options.c) | forces aim assist on; the saved player option is untouched |
| `pd.deadzone(frac)` | `inputSetChaosDeadzone` (port/src/input.c) | runtime deadzone floor 0..0.95 of full deflection, wins over the user's per-axis setting inside `inputAxisScale`; gamepad only (kb/mouse unaffected) |
| `pd.nitro(on)` | `g_ChaosNitro` → `objCheckDestroyed` (propobj.c) | every destroyed object's `exptype` upgraded to `EXPLOSIONTYPE_HUGE25` (the Crash Site ship) |
| `pd.objective_force(i, state)` / `pd.objective_status(i)` | `g_ChaosObjectiveForce[]` → `objectiveCheck` (objectives.c) | state 1 = force INCOMPLETE, 2 = force COMPLETE, 0 = off; no-arg call clears all. Solo-gated (`g_NetMode == NETMODE_NONE`), placed AFTER the co-op overlay blocks. Display and `objectiveIsAllComplete` both route through it, so a held objective blocks mission end. `objective_status` reads the REAL value (override bypassed) — how the Lua effect finds a completed one; returns −1 for non-live objectives (count + difficulty-bits checked). Array cleared in **lvReset** |
| `pd.mark_home()` / `pd.warp_home()` | `g_ChaosLuaHome*` + `chaosPlayerWarp` (chraction.c) | "Back to the start": chaos.lua marks once per stage on its first real gameplay tick; warp reuses the body-snatch model-less-safe player move. Valid flag cleared in **lvReset** so a stale cross-stage home can't be warped to |
| `pd.env(stagenum)` / `()` | `envChooseAndApply` (env.c) | "Brandon's mod": apply another stage's whole sky/fog/cloud/water environment; no-arg restores the current stage's own row |
| `pd.fog(fogmin, fogmax, r, g, b)` / `()` | `envChaosFog` (env.c) | custom fog overlay built from the stage's own env row (near/far inherited = draw distance untouched, distance-fade tiers off); works on no-fog stages because `envApplyFogEnvironment` enables the fog pipeline. fogmin/fogmax are per-mille of the z-range — stock rows sit ~950..1050, the Perfect Hills effect uses 500/850. No-arg = `pd.env()` restore |
| `pd.blood_colour(r,g,b)` / `()` | `g_ChaosBloodColour` → `chrGetBloodColour` (chr.c) | every body bleeds this colour — sparks, hit splats and floor drips all derive their palette from that one function (values scaled to the stock ~¼-brightness convention) |
| `pd.max_blood(on)` | `g_ChaosMaxBlood` (splat.c + chr.c) | every hit splatters (stock 1-in-3 dry roll bypassed, qty 4-7), `bulletstaken` pinned to 7 (max wounded-drip rate), hit spark spray tripled |
| `pd.items_shuffle()` | `chaosItemsShuffle` (propobj.c) | Fisher-Yates over every loose `PROPTYPE_WEAPON` pickup (≤64), swapping pos + rooms with the engine's own move idiom (write pos → `propDeregisterRooms` → `roomsCopy`). Held (parented), embedded (planted mines), airborne-projectile and deleting props skipped; **objective items deliberately NOT moved** (script-softlock risk). Returns the count |

All batch-2 C globals are cleared in **`lvReset`** (not just lvInit — the Lua
state can die on a stage change with effects live, and a lingering button mask
or objective override must never cross stages) *and* by `reset_all_modes` in
chaos.lua (guarded `if pd.X then` so the script still runs on an older exe).

### Batch 3 (2026-07-12, the deferred-list bindings)

| Binding | Backing | Notes |
|---|---|---|
| `pd.chr_wireframe(on)` | `g_ChaosWireframeChrs` (prop.c) → `G_CHRWIREFRAME_EXT 0x4c` | "Wireframe enemies": propRender's PROPTYPE_CHR case brackets hostile chrs (`chrCompareTeams COMPARE_ENEMIES`; held guns render as children inside chrRender so they wireframe too) in a new scoped-wireframe EXT opcode. The renderer (`gfx_wireframe_scope`, gfx_pc.cpp) flushes on toggle, ORs into both backends' existing wireframe reads (gfx_opengl draw_triangles + gfx_sdlgpu pipeline/wire-colour), and force-clears the scope each `gfx_start_frame` so a lost END can't leak. Friendly/non-combat chrs stay solid |
| `pd.double_shots(on)` | `g_ChaosDoubleShots` (bondgun.c) | "Quad handed": doubles `hand->shotstotake` per fire event (same site as ammo_cost; ammo drains to match). Paired with dual-wield = four barrels |
| `pd.buttons()` / `pd.buttons_pressed()` | `chraiLuaButtons` → `joyGetButtons(PressedThisFrame)` | the local player's RAW pad buttons — reads the joy layer directly, so it sees buttons `pd.button_block` is hiding from gameplay. This is the popup framework's input: block FIRE from shooting, read FIRE as the answer |
| `pd.spawn_chopper([kind[, extrascale]])` | `chraiLuaSpawnChopper` (chraction.c) | **EXPLORATORY**: a hostile chopper near the player — the spawn_bike runtime-template recipe adapted to `OBJTYPE_CHOPPER` + setup.c's chopper field block, with `GAILIST_IDLE` (choppers `chraiExecute` every tick; a NULL ailist is fatal), the player as `target`, and `CHOPPERMODE_COMBAT`. kind 0 = `MODEL_DD_HOVERCOPTER` (chaos runs it at extrascale **64 = quarter size**; `objInitWithModelDef` does NOT apply extrascale — the explicit `modelSetScale` line does); kind 1 = `MODEL_A51INTERCEPTOR` at **256** — its MODELDEF is natively ~0.1 scale (the gunfire path's `0.1/model->scale` gun-pos correction), so 256 = authored size and anything lower shrinks it toward invisible (the first-round "vanished after one frame" bug). Because the spawns run `GAILIST_IDLE`, the mission ailists' see-target→fire loop is **driven from C instead**: the chopper tick dispatch calls `chaosChopperIsChaos()` (chraction.c) and runs `chopperCheckTargetInSight` per tick (the FOV half of `aiIfLosToTarget` is skipped so it spots the player all around) + re-asserts `CHOPPERMODE_COMBAT` — without this, `targetvisible` never goes true and the chopper never fires (the first-round "doesn't shoot" bug). Two static templates (`g_ChaosChoppers[2]`), per-kind respawn summons the existing instance. **kind 1 STALKS the player** (2026-07-28): a chaos chopper has `path == NULL`, so `chopperTickCombat`'s vanilla test (`targetvisible && dist < 2000000` **or** `path == NULL`) picked the stay-put branch every tick and the interceptor just hovered where it spawned. A port-only branch keyed on `chaosChopperKind(chopper) == 1` instead aims `goalpos` at a point on a `CHOPPER_CHAOS_STANDOFF` (700u) ring around the target at `CHOPPER_CHAOS_ALTITUDE` (260u), **on the bearing the chopper already occupies** — so it closes to that radius rather than diving, and drifts round the ring as the player moves. `goalpos` is the only input the steering below it reads, so this is the entire behaviour change; flight model, banking, gunfire and LOS are untouched. kind 0 (dD hovercopter) deliberately keeps the original hold-position behaviour |

Batch-3 Lua machinery: the **popup framework** — pop_quiz / eula / lore draw a
centred card in the alpha draw hook, `pd.button_block` keeps FIRE/AIM from
shooting while `pd.buttons_pressed` reads the answer, and the effect ends
early by **returning true from its tick** (the expiry loop treats that as
expire-now; never call `stop_effect` from inside a tick — the loop would
re-add the key it just removed mid-`pairs`, which is undefined). ("Schedule 1"
rode the pre-existing `pd.possess_spawn`/`pd.unpossess` — returns nil on
failure, not -1 — but was removed 2026-07-21; the bindings stay, and
`scripts/director.lua` still drives them.) The **category folders** are sibling test submenus (Test:
Visual & Audio / Cheats / Helpful / Lethal / Weapons & World) driven by one
`CATS` name table in the menu block — root-level siblings because 3-deep
scrollable menus crash the engine. `g_ChaosWireframeChrs`/`g_ChaosDoubleShots`
cleared in lvReset like batch 2.

### Jelly / Acid Trip (2026-07-20, on-the-fly vertex deformation)

| Binding | Backing | Notes |
|---|---|---|
| `pd.vertex_wobble([amp, freq, phase, sag, desync, nearfade])` | `chraiLuaVertexWobble` → `gfx_vtx_wobble_*` → `gfx_sp_vertex` (gfx_pc.cpp) | a **true per-vertex deformation**, not a post-process. The port transforms vertices on the CPU in `gfx_sp_vertex`, so the effect splits the usual combined model→clip multiply into **model→eye (displace) →clip**: each vertex is moved in **eye space** by sines of its own position (`ex += amp·sin(ey·freq+phase)`, etc.), then projected. Eye space (world-scale, camera-relative) is the key — a fixed frequency there gives a coherent ripple across the whole scene regardless of each model's local vertex magnitude (model-space would be fine noise on big room meshes, a faint sway on small props). `amp` world units (0/absent = off, clamped ≤200), `freq` radians/world-unit, `phase` advanced by the caller each tick (the speen pattern), `sag` an extra always-**downward** eye-Y droop for the Acid Trip **melt** (walls + characters sag/drip), `desync` (0 = lockstep, clamped ≤4) a **per-vertex rate spread**: each vertex hashes its (camera-stable) model position to a stable [0,1) value that both scales and statically offsets its `phase`, so different vertices flow at different speeds and arrive out of step (an organic melt, not a coherent travelling wave). UI drawn as 3D (`G_NOMIRROR_EXT`) is exempt; the path is gated so the normal single-multiply fast path is untouched when off. **Gates the display-list cache off** (`bg.c`, alongside speen/shiny) so cached room geometry re-runs the CPU vertex path and wobbles too; cleared in `lvReset`. `nearfade` (2026-07-28, world units, 0 = off) is a **near fade**: the whole displacement — wobble AND sag — is scaled by a smoothstep of the vertex's RADIAL distance from the eye-space origin (the camera sits there), so geometry near the lens barely strays from where it belongs while the far scene still melts. Radial, not depth: something beside your head is visually close even at small `|ez|`, and depth alone would let it thrash. Measured from the UNDISTORTED eye position, for the same reason the sines are — the field must not feed back on itself. Smoothstep rather than a linear ramp so there is no visible crease at the fade limit and the derivative is zero at the camera (geometry eases into motion instead of starting to slide the instant it clears the limit). `acid_trip` uses `ACID_NEARFADE` = 900; `jelly` deliberately omits it and keeps the uniform wobble. `jelly`/`acid_trip` drive an **ease-in/out envelope** (`vwobble_prog` in chaos.lua: `sin(prog·π)` over the effect's life) and morph amp/freq/sag across `prog` between two states so the scene flows OUT to a warped state and gently back to NORMAL rather than snapping |
| `pd.hall_of_mirrors(on)` | `chraiLuaHallOfMirrors` → `gfx_hom_mode` → `gfx_pc.cpp` frame clear | skip the game framebuffer's per-frame **colour** clear so un-redrawn pixels smear — the Doom Hall-of-Mirrors / acid trails. Depth still clears (via the dlist), so new geometry renders normally over the smear. Cleared in `lvReset`. (How much shows depends on how much of the frame the scene redraws — motion edges trail heavily) |

`acid_trip` combines these with the existing **Prismatic** hue field (`pd.pixelate(0,0,1005)`, the retro post-filter's screen-space multi-rate hue rotate) for the trippy colours: melt (`vertex_wobble` + `sag`) + trails (`hall_of_mirrors`) + colour cycle.

### Beat game (2026-07-20, music-tempo bindings)

| Binding | Backing | Notes |
|---|---|---|
| `pd.music_bpm()` | `chraiLuaMusicBpm` → `sndGetMusicBeat` (lib/music.c) | tempo of the current **sequenced** music track in beats/min, or 0 if none is playing. PD's in-game music is N64 sequences (MIDI-like) and the port runs that synth, so tempo is live: the sequence player tracks `uspt` (µs/tick, updated by MIDI tempo meta events), the sequence carries `qnpt` (quarter-notes/tick = 1/division), so `BPM = 60e6 × qnpt / uspt`. `sndGetMusicBeat` scans `g_SeqInstances[]`, preferring the `TRACKTYPE_PRIMARY` track. External `pd.play_file` tracks (raw PCM) carry no tempo → 0 |
| `pd.music_beat()` | `chraiLuaMusicBeat` → `sndGetMusicBeat` | position within the current beat as `[0,1)` (0 = on the beat), or nil if no sequenced track. Phase = `(seqp->curTime mod µs-per-beat) / µs-per-beat` — `curTime` is the playback position in µs. NOTE: `curTime` is the sequencer's position, which leads the audible output by the output-buffer latency, so it's a touch ahead of what's heard; the effect's on-screen pulse uses the same phase, so players sync to the visual and it stays self-consistent |
| `pd.aim_chr()` | `chraiLuaAimChr` → `propFindAimingAt` (prop.c) | the chrnum the local player is aiming at (autoaim/crosshair `FINDPROPCONTEXT_QUERY`), or nil. Lets the beat game reward an on-beat shot with bonus damage without touching the fire→damage path |

`beat_game` is pure Lua on top of those: a `weaponfire`-hook scorer + a pulsing HUD, both reading `beat_phase()` (music beat if playing, else a free-running 120-BPM fallback accumulator in `st.a_beat`). On a shot, distance to the nearest beat picks the outcome — `< 0.10` = **on beat**, bonus `pd.chr_damage(pd.aim_chr(), 8)`; `< 0.22` = on time, normal; else = **off beat**, `pd.player_damage(1.5)`. Doing the bonus as a separate `chr_damage` on the aim target (rather than a per-bullet multiplier) keeps it entirely Lua and dodges the fire-vs-hitscan ordering question.

### Space Program / Frag Out / Sentries Out / Temu Magazine / Helpful son (2026-07-20)

| Binding | Backing | Notes |
|---|---|---|
| `pd.space_program(on)` | `g_ChaosSpaceProgram` → `chrDamage` (chraction.c) | the `one_punch` block, but for GUN shots: any player bullet on an NPC becomes `maxdamage + shield + 100` (a one-hit kill through armour) and `chrYeetFromPos(..., 900)` launches the corpse — ~3.5× one_punch's fling. Boosted before the `SVC_CHR_DAMAGE` broadcast so net clients agree; NPC victims only |
| `pd.frag_out(on)` | `g_ChaosFragOut` → `chrConsiderGrenadeThrow` (chraction.c) | human enemies lob a grenade whenever they'd fire: the chaos branch skips the `grenadeprob` roll and drops the min engagement range 200→100 (a small standoff so they don't point-blank themselves). The function's existing "no grenade in hand" path hands them one (`chrGiveWeapon(MODEL_CHRGRENADE)`, invisible), so any gun guard becomes grenade-happy |
| `pd.spawn_sentry(dx, dz)` | `chraiLuaSpawnSentry` (chraction.c) → `OBJTYPE_AUTOGUN` | deploy a hostile laptop sentry (`MODEL_CHRAUTOGUN`) at the player + horizontal offset, floor-snapped. Field values (aim range/speed, full rotation, beam) mirror `laptopDeploy`, but storage is our own `g_ChaosSentries[8]` pool (the engine's `g_ThrownLaptops` is per-player/slot-limited) so several coexist. `targetteam = player's team` — the autogun's target scan (`chr->team & targetteam`) then selects the player, i.e. unfriendly. `forcetick` so they fire off-screen. Count reset per stage in `chraiLuaResetSentries` (lvReset); solo only |
| `pd.temu_mag(on)` | `g_ChaosTemuMag` → `bgun0f098df8` reload (bondgun.c) | a knockoff magazine: a reload still deducts the FULL `amount` from the reserve, but only chambers a random ~34-100% of it (`loaded`), so reloading no longer tops you off. Guarded `amount >= 2` (single-shell/incremental reloads unaffected) and `loaded >= 1` (never wastes everything); local player only |

Helpful son is pure Lua (needs only existing `pd.player_add_yaw` / `pd.player_pitch` / `pd.forced_fire` / `pd.forced_march` / `pd.switch_weapon`): a per-tick FSM in the effect (`st.a_helpson`) that, at random 0.5-2s intervals, grabs one input for 0.3-0.7s — a look sweep, held fire, forward walk, or a fumble to a random weapon.

### Me and my son / Silo Countdown / Pirate (2026-07-20, new-effect bindings)

| Binding | Backing | Notes |
|---|---|---|
| `pd.spawn_ally_clone([healthfrac])` | `chraiLuaSpawnAllyClone` (chraction.c) | the `chraiLuaSpawnAlly` recipe, but the buddy wears the **player's own body AND head** (a Jo clone) instead of Dark Combat / VD, and her health pool (`chrSetMaxDamage` + `chrAddHealth`, both ×healthfrac) is scaled — default 0.5 = a fragile half-HP clone. Still TEAM_ALLY / SQUADRON_01 / `GAILIST_INIT_DEFAULT_BUDDY`, Falcon 2, `CHRCFLAG_NEVERSLEEP`. Server/solo-side; returns the chrnum or nil |
| `pd.chr_yscale(chrnum, mult)` | `chr->yscale` (port-only chrdata field) → `modelUpdateChrNodeMtx` (model.c) | **non-uniform** vertical squash/stretch, unlike the uniform `pd.chr_scale`. The chr root matrix `sp158` is the model→world basis, so its **row 1** (`m[1][*]`) is the world image of the model's local Y axis (the spine); scaling only that row (`mtx00015e4c`) compresses height while leaving width/depth untouched, and — being at the root — it propagates down the whole skeleton. `mult 0.4` = 40% tall, full width. Bounded `(0, 4]` on both the setter and the render read (a stray value can't invert or balloon a chr). Purely visual (hitbox/AI unchanged). `chr->yscale` is reset to 1.0 in **`chrInit`** so a recycled chrslot never inherits a stale squash; the whole path is `#ifndef PLATFORM_N64` (the N64 build is byte-identical) |
| `pd.stage_music(on)` | `chraiLuaStageMusic` (chraction.c) → `musicStop` / `musicSetStageAndStartMusic` (music.c) | stop (`on=false`) or restart (`on=true`) the **current stage's** music. Unlike `pd.song` — which layers a menu track over the *paused* stage music — this genuinely silences the level track, then re-derives primary + ambient from `g_Vars.stagenum` on restore. Backs Silo Countdown (kills the mission music, plays `Silo.mp3` via `pd.play_file` underneath). **`on=false` also sets the port-only `g_MusicSuppressed` latch** (music.c), which early-returns every music-restart path (`musicStartPrimary`/`Ambient`/`Nrg`/`TrackAsMenu`) — without this, closing the pause menu after triggering the effect calls `musicEndMenu → musicStartPrimary` and the game music creeps back. `on=true` clears the latch before restarting; `lvReset` force-clears it each stage load so it can never stick silent. The latch does **not** touch `g_MusicVolume`, so a `follow_music` `play_file` track is unaffected |
| `pd.play_file(path, [loop], [follow_music])` | `chraiLuaPlayFile` → `audioPlayExternal` (audio.c) | play an external WAV/MP3 (`SDL_LoadWAV`/minimp3, detected by content) mixed into the device stream. `loop` rewinds instead of freeing. `follow_music` scales the track by the in-game **music-volume** slider (`optionsGetMusicVolume`, 0..0x5000, applied as an 8.8 fixed-point gain in the ext-mix loop) so it ducks/mutes with the player's music setting — default **off** (full volume, e.g. the Ring Ring ringtone). A `follow_music` track also **pauses with the game**: while `lvIsPaused()` the ext-mix block is skipped so the track goes silent AND its `extSoundPos` doesn't advance, resuming cleanly when you leave the menu (non-`follow_music` tracks keep playing). Silo Countdown passes `true` so `Silo.mp3` honours the music slider, is suppressed alongside the sequenced music, and pauses in the menu |
| `pd.pirate(side)` | `chraiLuaPirate` (chraction.c) → `gfx_retro_fx` bits 0x800/0x1000 → the shared retro post-filter (`gfx_retro_common.h`) | "Pirate" eyepatch: black out the **left** (`side=1`) or **right** (`side=2`) half of the *finished frame* top-to-bottom; `0`/absent = off. Being a post-process over the composited frame, it covers the **HUD** in that half too. The shader keys on the RAW screen UV (`vUV.x`) before any warp, so the masked half is fixed in screen space; `x` is unaffected by the GL/SDL_GPU y-flip, so both backends agree. Only one side is set at a time |
| `pd.half_mirror(side)` | `chraiLuaHalfMirror` (chraction.c) → `gfx_retro_fx` bits 0x2000/0x4000 → the shared retro post-filter (`gfx_retro_common.h`) | "PERREP DAAD" / "FECTTCEF RKKR": mirror the **left** (`side=1`) or **right** (`side=2`) half of the *finished frame* onto the other half about the vertical centre line, kaleidoscope style; `0`/absent = off. Same screen-space keying and backend symmetry as `pd.pirate` (raw `uv.x`, applied before the warp/rotate stages). Only one side is set at a time — both bits together would swap the halves, which the helper never does |

`chraiLuaChrYscale`'s field is the first port-only chrdata member that the render
lib (`src/lib/model.c`) reads — `model->chr` is already the established chr
backpointer at every CHRINFO node, so no new plumbing. The "Me and my son"
death-watch (`st.a_son`) lives in chaos.lua's main tick, not a C hook, and is
cleared in `reset_all_modes` alongside `st.a_weep`.

**Silo Countdown** (`silo_countdown`) is otherwise pure Lua: a `fixeddur`
`nobar` effect whose length is `chaos.silo_seconds` (default 510s / 8:30,
via a `dur` function so a test harness can shrink it). On start it kills the
level music (`pd.stage_music(false)`, which also latches `g_MusicSuppressed` so
the music can't creep back when the pause menu closes) and plays `Silo.mp3` once
(`pd.play_file(path, false, true)` — loop off, and the `true` `follow_music` arg
makes the track honour the player's music-volume slider instead of blasting at full), and draws
its own centred MM:SS clock in the alpha HUD hook off `st.a_silo` (red-flashing
in the final 10s). The final-stretch music is baked into `Silo.mp3` now, so there
is no mid-countdown track swap; at zero the tick
fires `pd.explosions_around` with `st.a_boom_off` (the same
short-burst detonation the countdown/SPEED payoffs use — shut off by the main
tick, so the effect's own `stop()` never has to). `stop()` stops the track and
restores the music but deliberately leaves the boom alone; a mission-complete or
player restart routes through `reset_all_modes`, which runs `stop()` **and**
clears the pending boom (`explosions_around(false)` + `a_boom_off = nil`), so the
timer and detonation vanish together.

`scripts/silo_test.lua` (loaded by `init.lua` after chaos.lua) is a test harness:
`silo_test()` saves `chaos.silo_seconds`, sets it to 60, triggers the effect
(so `start()`/`dur()` capture the 60s), then restores the default — a one-minute
run that leaves the real effect untouched. Exposed as `/lua silo_test()`
and a "Silo Countdown (1-min test)" entry in the Chaos Alpha menu.

> **GOTCHA — no non-ASCII in `pd.hud_message` text.** This build's HUD font is
> ASCII-only; any byte `>= 0x80` (a UTF-8 em-dash `—`, accents, emoji) is routed
> by the text renderer into the JPN multibyte glyph path (`langGetJpnCharPixels`,
> lang.c), whose cache table `g_JpnCacheCacheItems` is **NULL** in a non-JPN ROM →
> null-deref crash in `hudmsgsRender`. An em-dash in the Silo armed-message caused
> exactly this (2026-07-20). `l_pd_hud_message` (luaai_api.c) now **scrubs high
> bytes to `?`** as a safety net — important because the `say <text>` chat
> passthrough pipes arbitrary chat text straight into `hud_message` — but effect
> strings should still use plain ASCII (`-`, not `—`) so nothing shows as `?`.

### Knockouts & Nap time (2026-07-18; effect REMOVED same day)

> The `nap_time` effect was removed from chaos.lua the same day — stage-wide
> KOs proved too troublesome to debug. The `pd.chr_ko` / `pd.chr_wake`
> bindings and all the C-side mechanics below remain live for scripting.

`pd.chr_ko(chrnum)` (Chaos Alpha, `chraiLuaChrKo`) drives the tranquiliser's
sanctioned KO path (`chrBeginDeath` knockout=true → ACT_DRUGGEDDROP → KO).
Two hard-won facts about that chain:

- **Engine knockouts are permanent.** `ACT_DRUGGEDCOMINGUP` means the drug
  *coming on*, not waking — the chain is one-way, and `chrTickDruggedKo`
  only fades/**reaps** the body (off-screen ~2s). A reaped chr reads as
  eliminated to mission scripts (`aiIfChrDead` passes on `!chr`), so KO'ing
  a protected NPC used to fail the mission. `chr_ko` now sets
  `CHRCFLAG_KEEPCORPSEKO` to park the body un-reaped (that flag gates every
  reap/cleanup site), and `pd.chr_wake(chrnum)` (`chraiLuaChrWake`) recovers
  the chr — the engine's own knockdown recovery (`func0f02ed28`, a 26-tick
  blend back to standing), AI resumes, unarmed since the drop scattered
  their guns. Nap time is now `fixeddur` 20s: start KOs + records
  `st.nap_chrs`, stop wakes them (runs on expiry, `/chaos off`, and
  re-trigger).
- **KO-counter bookkeeping**: `chr_ko` does NOT increment the knockout
  counter (nap KOs must not trip `aiIfNumKnockedOutChrs` script branches or
  exhaust `chrKnockOut`'s first-two-KOs KEEPCORPSEKO budget), and
  `chrBeginDeath` decrements it when a KO'd chr is killed outright — so
  `mpstatsDecrementTotalKnockoutCount` now clamps at zero (port-guarded;
  unreachable on N64-faithful paths) to stop the u32 underflowing.
- The Skedar crash: `chr_ko` passes `HITPART_TORSO`, not `HITPART_GENERAL` —
  GENERAL (200) isn't in `g_AnimTablesByRace`, and the Skedar fallback row
  (entry 0) has NULL deathanims → NULL deref (harmless on N64, faulted the
  port). A port-guarded fallback in `chrBeginDeath` also scans for the first
  valid row so no other caller can hit it.
- Residual limit: stages whose scripts *explicitly* branch on a chr being
  knocked out (the `aiIfChrKnockedOut`-style checks) still react during the
  nap — that's scripted behaviour, not the reap bug.

**Still deferred** (with reasons): DarkSim mission AI (bot AI is welded to
Combat Sim player slots — the co-op plan's linchpin problem; Terminator is the
approximation), player-2 pad swap (a correct swap must remap the whole VK_JOY
bind layer; kb/mouse users can't test it), drug-spy body swap (the body-snatch
gunmem tarpit — the "Schedule 1" drone-possession stand-in was removed
2026-07-21), A51 interceptor
(dropship model is cutscene-scale; the dD chopper covers the idea).

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
`chr_alert`, `chr_give_weapon`, `spawn_ally`, `spawn_at_chr`, `hud_message`,
`persist_get/set`, `menu_add`, `on`, `log`.

**`pd.all_chrs` is now polymorphic**: `pd.all_chrs(fn)` still calls `fn(chrnum)`
for every actor (the `director.lua` callback form), but `pd.all_chrs()` with no
function **returns an array table** of live chrnums — the form `chaos.lua`'s
`random_chr()` and the crowd effects rely on. Menu helpers: `pd.menu_add(label,
fn, [group])` (optional submenu title) and `pd.menu_set_label(index, text)`
(live in-place relabel — see "Pause-menu UI").

## Twitch / YouTube integration (the open window)

The contract is deliberately tiny: **one UDP datagram of plain text to
127.0.0.1:`Chaos.EventPort`, in the protocol above**. Any chat-bot stack can
speak it today:

- **Custom bot** (any language): connect to Twitch IRC / YouTube Live chat
  API, map `!chaos panic` → send `trigger panic <username>`; map channel-point
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
  `chaos.trigger("panic", "AP trap from " .. sender)`.
- **DeathLink**: on a DeathLink bounce, `chaos.trigger("boom")` or a custom
  lethal effect.
- **Per-seed randomiser**: at connect time call `chaos.set_seed(slot_seed)`
  and `chaos.handle("ap", "on")` — the weighted effect stream is then
  deterministic per AP seed (same seed = same chaos schedule).

## Backwards bullets: why it never worked (fixed 2026-07-28)

`pd.backfire` reverses the shot ray in `bgunCalculatePlayerShotSpread`
(bondgun.c) and `gundir3d` is correctly derived from the reversed `gundir2d`
(prop.c:1496/1512) — **the flip was never the problem**. Hits didn't register
because of three independent gates, each of which had to be found by testing:

1. **The candidate set.** `shotCalculateHits` walks only
   `g_Vars.onscreenprops`, which `propsSort` builds by filtering on
   `PROPFLAG_ONTHISSCREENTHISTICK`. A chr behind you has neither that flag nor
   the model-to-screen matrices the narrow phase needs — both are set in one
   `if (needsupdate)` block in `chrTick`. So the reversed ray was traced against
   an empty list. Fixed by forcing `needsupdate` for chrs in draw distance while
   the effect is on (the blind server's Tier 1 idea,
   `PORT_HEADLESS_BLIND_SERVER.md` §9), placed **before** the kill-plane and
   corpse-reap guards so those still get the last word.
2. **The per-frame update budget.** chr.c caps chr render-preps at 30 per frame
   (`var8009cdb0 + var8009cdac > 30`, reset in `propsTick`). Normally only
   on-screen chrs spend it; the force made every nearby chr spend it, so chrs
   past the cap silently lost their matrices. Symptom: **one enemy could be hit
   and the next couldn't, decided by tick order.** Forced chrs are now exempt
   from the budget and from the corpse-reap counters.
3. **A sign error, and the one that hid the longest.** `shotCalculateHits`
   clamps `shotdata.distance` to the BG hit depth via `-sp658.z` — depth along
   the CAMERA'S FORWARD axis. For the wall a reversed shot hits, that is
   **negative**, so every downstream comparison inverts: `sp68 <
   shotdata->distance` becomes `-D < -W`, i.e. a rear chr only registers if it
   is *further away than the wall behind it*, and `prop->z - radius < distance`
   only passes when `prop->z` ≈ 0. Symptom: **hits only registered when you were
   touching the guard.** Fixed by taking magnitudes at three sites (the bg-depth
   clamp in prop.c, and `prop->z` + `sp68` in `chrTestHit`) — with the ray
   flipped 180°, everything reachable is behind the camera, so the magnitude is
   the true along-ray distance. Note `sp68` also feeds `hitCreate` as the sort
   key, so a negative was mis-ordering the hit list too.

`/backfire` dumps the per-chr gates (`scr` = onscreen, `mtx` = matrices, `hid` =
hidden, `ddist` = draw distance) if this ever regresses.

### The rear-view mirror: ABANDONED after three attempts

The idea was a mirror panel showing what's behind you, so the effect is
aimable. **All three implementations were abandoned 2026-07-28. Do not
re-attempt without reading this.**

| Attempt | Approach | Result |
|---|---|---|
| 1 | Viewport hijack inside `playerRenderHud` | Total frame corruption |
| 2 | "Sibling pass" at the `lvRender` loop level, sharing the frame's viewport + z-buffer | Letterboxed the main view, stray polys everywhere |
| 3 | Render into an offscreen framebuffer (`gDPSetFramebufferTargetEXT`) + blit with `gSPImageRectangleEXT`, the menugfx.c:160-166 menu-blur recipe | Still glitchy |

Two root causes were identified and are worth keeping even though the feature
was dropped:

- **`viSetFovAspectAndSize` mutates GLOBAL VI aspect state that fast3d consumes
  at flush time, not at display-list record time.** Restore commands queued into
  the list therefore cannot unwind it. gfx_pc.cpp:2526 carries a matching
  comment: *"HACK: assume all target framebuffers have the same aspect"*.
- **`bg.c`'s draw slots and room visibility are computed once per frame for the
  forward camera.** Any second pass emits geometry against a mismatched slot
  set. Forcing `g_BgNoCull` makes it draw *something*, not the right thing.

Splitscreen survives multiple viewports only because they are **disjoint screen
regions established before anything is drawn**. A view-within-a-view is a
different problem, and attempt 3 shows that even a separate render target
doesn't fully decouple it — the global VI/aspect and per-frame bg state are
still shared. Anyone retrying this should expect to need a genuinely
frame-level second pass, not a nested one.

## Ammo policy: two magazines, not max (2026-07-28)

Every chaos effect that hands out a weapon or swaps ammo now gives
**`AMMO_MAGS` = 2 magazines** rather than topping the reserve to capacity. A
free gun should be a moment of power, not a licence to stop caring about ammo.

The rule is enforced in **two** places, and it needs both:

- **chaos.lua** routes all 23 former `pd.refill_ammo()` call sites through one
  `give_ammo_mags()` helper, so the count lives in a single constant. It falls
  back to the old max refill on an exe without `pd.give_mags`.
- **chraction.c** applies it at the C grant sites, because `chraiLuaDualWield`
  and the body-snatch weapon grab both called `bgunGiveMaxAmmo(true)` directly.
  Four `pd.dual_wield` effects don't follow up with a Lua refill, so gating it in
  Lua alone would have left those still topping up to capacity. Both now call
  `chraiLuaGiveMags(CHAOS_GUN_MAGS)`.

`pd.refill_ammo` itself is unchanged and still means "fill to capacity" — it is
simply no longer what the effects call.

**Exempt — effects whose identity IS the ammo** keep the full `pd.refill_ammo()`
resupply, because capping them would leave them with nothing to be:

- `ammo_rain` ("Ammo rain")
- the `touch_reward` "max ammo" prize (it announces itself as max ammo)

The ammo-SWAP effects (rockets / devastator / LX / farsight / tranq) are *not*
exempt: their identity is "your gun now fires rockets", not the quantity.

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
| `scripts/chaos.lua` | the chaos engine (effects/timer/votes/protocol/config/menu) |
| `scripts/director.lua`, `scripts/ap/test.lua` | group their entries under submenus (`pd.menu_add` 3rd arg) |
| `scripts/init.lua` | loads chaos.lua |
| `src/game/luaai_api.c` | the `pd.*` bindings + `luaExtEventPush` ring queue; `pd.chr_weapon`; `pd.menu_add` group arg + `pd.menu_set_label`; registry `group` field |
| `src/game/chraction.c` | `chraiLua*` helpers; `chraiLuaTeleportToChr` (offset + object-safe + model-less), `chraiLuaChrGiveWeapon` (guard + bot paths), `chraiLuaChrWeapon`; `chraiLuaSpawnAllyClone` + `chraiLuaChrYscale` (Me and my son); `chraiLuaStageMusic` (Silo Countdown) |
| `scripts/silo_test.lua` | Silo Countdown 1-minute test harness (`silo_test()` + Chaos Alpha menu entry) |
| `scripts/init.lua` | loads `silo_test.lua` after chaos.lua |
| `scripts/sounds/chaos/README.md` | documents the `Silo.mp3` external track (not shipped) |
| `port/fast3d/gfx_retro_common.h` | Pirate half-screen blackout bits (0x800/0x1000) in the shared retro post-filter body |
| `port/src/dedicated_stubs.c` | headless no-op stubs for `inputSetChaosInvertLook` / `inputSetChaosInputDelay` / `inputLastSourceWasPad` (pre-existing dedicated-server link break) |
| `src/lib/model.c` | `modelUpdateChrNodeMtx` applies the port-only `chr->yscale` vertical squash to the root matrix's model-Y row (`pd.chr_yscale`) |
| `src/game/bondgun.c` | `g_ChaosTemuMag` partial-reload hook in `bgun0f098df8` (Temu Magazine) |
| `src/game/lv.c` | `lvReset` clears the new chaos globals + `chraiLuaResetSentries` per stage |
| `src/lib/music.c`, `src/include/lib/music.h` | `sndGetMusicBeat` — reads the sequenced music tempo + beat phase for the beat game |
| `port/fast3d/gfx_pc.cpp` | `gfx_sp_vertex` eye-space per-vertex "Jelly"/"Acid" wobble + melt sag (`gfx_vtx_wobble_*`); "Hall of mirrors" no-colour-clear trails (`gfx_hom_mode`); `src/game/bg.c` gates dlcache off, `lv.c` clears them per stage |
| `src/game/chr.c` | `chrInit` resets the new port-only `chr->yscale` to 1.0 (recycled-chrslot rule) |
| `src/include/types.h` | port-only `f32 chrdata.yscale` (non-uniform vertical render scale) |
| `src/game/mainmenu.c` | `luaDirectorRebuild` builds per-group submenu dialogs (openers at root top) |
| `src/include/game/luaai.h` | declarations; `LUA_MENU_LABEL`, `LUA_DIRECTOR_MAX_SUBMENUS`, `luaMenuGroup` |
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
5. UDP: set `EventPort=27110`, `nc -u` a `trigger panic` from the same machine
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
