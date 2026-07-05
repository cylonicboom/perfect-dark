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
| `vote <effect>` | Tally a vote; winner fires when the window closes |
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

## Effect table (scripts/chaos.lua)

~25 effects, all self-cleaning. Weights (`w`) bias the random pick; `dur` in
seconds (0 = instant). Cheat-bank effects use the `cheat_effect(id, secs)`
factory (activate → timed deactivate).

- **Arsenal**: `arsenal` (random gun + switch + ammo), `disarm` (take held
  weapon), `knife_fight`, `ammo_rain`.
- **Cheat bank** (timed): `mirror`, `wireframe`, `tonal` (tonal inversion),
  `fists`, `slomo`, `dkmode`, `smalljo`, `smallchars`, `elvis`, `marquis`,
  `enemyrockets`, `enemyshields`.
- **Player state**: `godmode` (10s invincible), `cloak`/`xray`/`nightvision`
  (device on, timed), `heal` (+full shield), `blink` (white screen flash).
- **World**: `panic` (alert every chr), `yeet` (fling every chr away from the
  player), `boom` (explosion at a random chr), `buddy` (spawn ally),
  `reinforce` (spawn armed enemy at a random chr).

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
6. Vote mode: `votetime 15`, send several `vote` datagrams, winner fires at
   window close.
7. `seed 12345`, note the first 5 effects; `/lua reload`, `seed 12345` again —
   same 5 effects.
8. AP smoke: `ap.connect` to the mock server, call
   `chaos.trigger("boom", "ap")` from the console — HUD announce shows.
