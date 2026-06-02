# Ruthless Code Review — `port-net-predict` netplay + Lua scripting

**Date:** 2026-06-02
**Scope reviewed:** the delta of `port-net-predict` over upstream `port`
(`git diff origin/port...origin/port-net-predict` — 264 files, ~75k insertions,
of which ~30k are vendored Lua 5.4.7 + ENet and are *not* reviewed for internal
correctness, only for how they are embedded/configured).

The review concentrated on the parts that are (a) attacker-reachable and (b)
newly written for this fork:

- `port/src/net/*` — the client/server protocol, prediction, lag-comp, browser,
  admin control (`net.c`, `netmsg.c`, `netbuf.c`, `netmaster.c`, `playlist.c`,
  `netmenu.c`), and `port/src/spectator.c`.
- `src/game/luaai*.c`, `src/include/game/luaai.h`, `scripts/*.lua` and the
  `chrai.c`/`chraction.c` Lua bridge — the embedded Lua AI/modding layer.
- The netplay-driven modifications to core engine files (`mplayer.c`, `setup.c`,
  `player.c`, `bondgun.c`, `bg.c`, `chraction.c`, `mainmenu.c`,
  `modeldata/general.c`) and the `CMakeLists.txt` integration.

## Threat model (why severities are what they are)

The in-game server **browser hands the player to arbitrary internet servers**,
and the ENet transport is **unauthenticated and unencrypted**. Therefore
"a malicious server corrupts a connecting client" and "a malicious client
attacks the server it joined" are both *realistic* attacks, not theoretical.
Every `SVC_*` handler is attacker-controlled input on the client; every `CLC_*`
handler is attacker-controlled input on the server. That is the lens used below.

---

## Executive verdict

The netplay design is genuinely good — authoritative server, client-side
prediction with reconciliation, entity interpolation with an adaptive jitter
buffer, server-side lag compensation, an admin/vote/playlist control plane, a
master-server browser, and a cleanly carved-out headless dedicated server. The
*architecture* and much of the *control-plane* code (admin auth, lag-comp
save/restore, vote/score/lobby handlers, the browser, the playlist parser, the
spectator) are careful and correctly bounds-checked.

**But the packet-deserialization layer is systematically unsafe**, and the
**Lua layer ships an arbitrary-code-execution surface enabled by default**.
These are not edge cases — they are reachable on the very first packets of a
join and at process startup respectively. **This branch is not safe to expose to
untrusted peers or untrusted script bundles in its current state.**

The single most valuable fact: the memory-safety bugs are nearly all the *same
bug* — **a wire-supplied client/player index used to address `g_NetClients[]` /
`g_PlayerConfigsArray[]` without a bounds check**. One shared resolver function
closes the entire class.

---

## CRITICAL

### CR-1 — `SVC_AUTH`: unbounded `id` → remote out-of-bounds struct write on join
`port/src/net/netmsg.c:670-693`

```c
const u8 id = netbufReadU8(src);
const u8 maxclients = netbufReadU8(src);
...
if (... || id == NET_NULL_CLIENT || id == 0 || maxclients == 0) { return 1; } // only rejects 0 and 0xFF
g_NetMaxClients = maxclients;                         // unclamped (can be 255)
g_NetLocalClient = &g_NetClients[id];                 // OOB for id in [9..254]
g_NetClients[id] = g_NetClients[NET_MAX_CLIENTS];     // multi-KB struct copy to OOB dest
```

`g_NetClients[]` has only `NET_MAX_CLIENTS + 1 == 9` entries. The guard rejects
`0` and `0xFF` but **not `9..254`**, and `maxclients` itself is never clamped. A
server you joined from the browser sends `id = 200` and gets an out-of-bounds
copy of a whole `netclient` (which embeds `out_data[1440]` + ring buffers — a
multi-KB write) into BSS, and `g_NetLocalClient` now points outside the array so
every later local-client dereference is wild. **Remote memory corruption / RCE
primitive, triggered on join.**

**Fix:** `if (id == 0 || id >= NET_MAX_CLIENTS || maxclients > NET_MAX_CLIENTS) { reject; }`.

### CR-2 — Systemic unbounded wire client-index in the `SVC_*` handler family → remote OOB read/write
Multiple sites in `port/src/net/netmsg.c`:

| Handler | Site | Effect |
|---|---|---|
| `SVC_STAGE_START` | `:886-921` (`id`), `:943` (`playernum`→`g_PlayerConfigsArray[]`) | OOB struct writes (incl. `strncpy` of attacker name) every match start |
| `SVC_PLAYER_MOVE` | `:1086,1097`, `memset(movecl->lagcomp,…)` `:~1131` | OOB `netplayermove` ring write **and a large OOB `memset`** |
| `SVC_PLAYER_STATS` | `:1208,1221` | OOB `->state` read, writes through OOB `->player` |
| `SVC_PROP_USE` | `:2214-2229` | OOB index, then `setCurrentPlayerNum(actcl->playernum)` |
| `SVC_PROP_DOOR` | `:2281` | OOB `->is_spectator`/`->playernum` read |
| `SVC_PROP_SPAWN` | `:2011-2012` | OOB index **+ NULL `->player->prop->chr` deref** (CR-3) |

In every case a `u8` (or `s16` for `modelnum`, see CR-4) is read from the wire
and used to index a 9-slot array with no `>= NET_MAX_CLIENTS` / `< MAX_PLAYERS`
check. `SVC_PLAYER_MOVE`'s OOB `memset` is effectively an arbitrary-write-zero
primitive. Contrast with the *correctly* written `SVC_SCORE` / `SVC_LOBBY_STATE`
/ `SVC_VOTE_*` handlers which all clamp — those show the intended pattern.

**Fix (the structural one):** add
```c
static struct netclient *netResolveWireClient(u8 id) {
    return (id < (u32)g_NetMaxClients && id < NET_MAX_CLIENTS) ? &g_NetClients[id] : NULL;
}
```
and route **every** `netbufReadU8`-derived `g_NetClients[...]` access through it;
validate `playernum < MAX_PLAYERS` (or `== NET_PLAYERNUM_SPECTATOR`) before any
`g_PlayerConfigsArray[]` / `setCurrentPlayerNum`. This single change closes
CR-2, CR-3, and the H-class index bugs.

### CR-3 — `SVC_PROP_SPAWN` autogun path: OOB index + NULL dereference
`port/src/net/netmsg.c:2011-2012`

```c
const u8 clid = netbufReadU8(src);
struct chrdata *ownerchr = g_NetClients[clid].player->prop->chr; // OOB + .player may be NULL
```

Unbounded `clid` **and** `.player` is NULL for spectators / not-yet-spawned
clients even in range → server-triggered crash, or a type-confused pointer fed
into `laptopDeploy`. Subsumed by the CR-2 fix plus a `!cl->player || !cl->player->prop`
guard.

### CR-4 — `SVC_PROP_SPAWN`: unchecked wire `modelnum` indexes `g_ModelStates[]`
`port/src/net/netmsg.c:1960-1961, 2005/2013`

```c
const s16 modelnum = netbufReadS16(src);          // signed, -32768..32767
setupLoadModeldef(modelnum);
struct modeldef *modeldef = g_ModelStates[modelnum].modeldef;  // OOB read
```

A signed wire value indexes the model-state table and drives a ROM model load.
Negative / large values are OOB → crash or an arbitrary `modeldef` pointer fed
into `modelmgrInstantiateModelWithoutAnim`. Same in the `OBJTYPE_AUTOGUN`
branch. **Fix:** validate `modelnum` against the valid model count before
loading/indexing; drop the spawn otherwise.

### CR-5 — `CLC_HIT`: server applies fully client-controlled damage with no authorization or plausibility
`port/src/net/netmsg.c:569-614`, applied verbatim in `netEndFrame` (`net.c:~1506`)

The server reads attacker-controlled `damage` (f32), `target_syncid`, full
`gset`, `hitpart`, `side` from a **client** and calls `chrDamage(target, damage, …)`
unmodified. There is **no** clamp on `damage` (send `1e30` or `NaN`), **no**
finite check, **no** verification the shooter owns the weapon in `gset`, **no**
LoS/range check, and **no** check the shooter is an alive combatant
(`srccl->player` may be NULL / the client may be `is_spectator`). A malicious
client deals arbitrary damage to **any** chr/player by syncid — a one-packet
instakill/aimbot — and attributes the kill to itself in the feed/score.

The lag-comp rewind correctly derives its rewind from the *server-measured* RTT
and ignores the client-claimed tick (good), but that does nothing to constrain
the damage value or target. **Fix:** reject if `is_spectator` / `!player` / dead;
require `isfinite(damage) && damage > 0` and clamp to the resolved weapon's max;
ideally recompute damage server-side from the weapon rather than trusting the
wire; validate the target is within the rewound LoS/range.

### CR-6 — `netbufReadStr` returns a non-NUL-terminated pointer into the packet → heap over-read everywhere
`port/src/net/netbuf.c:113-122`

```c
char *netbufReadStr(struct netbuf *buf) {
    const u16 len = netbufReadU16(buf);
    if (netbufCanRead(buf, len)) {
        char *ret = (char *)&buf->data[buf->rp];  // points into ev.packet->data
        buf->rp += len;
        return ret;                                // NOT guaranteed NUL-terminated
    }
    return NULL;
}
```

ENet does not append a trailing NUL, and a hostile peer can send a `len` whose
last byte is non-zero / that runs to the end of the packet. Every consumer
treats the result as a C string: `strcasecmp(romName, g_RomName)` and the mod
compare (`netmsg.c:232,243`, **server-side, pre-auth**), the join-password
`strcmp` (`netmsg.c:253`), the admin-password `strcmp` (`net.c:~2890`), name
`strncpy`s, `netChatPrintf("%s joined", name)`, the `%s` chat/kill-feed logging,
etc. Result: read past the end of the packet allocation → info leak (adjacent
heap echoed into chat/logs) and potential crash. The writer always emits a
trailing NUL (`netbuf.c:271`), so requiring it on read is free.

**Fix:** in `netbufReadStr`, reject unless `len > 0 && ret[len-1] == '\0'`
(else set `buf->error` / return `NULL`); return `""` for `len == 0`. One change
neutralizes the whole string-handling class.

### CR-7 — Lua: full standard library opened + `scripts/init.lua` auto-run, on by default → arbitrary code execution
`src/game/luaai.c:35` (`g_LuaAiEnabled = 1`), `:233` (`luaL_openlibs(L)`), `:166-175` (auto-`dofile`)

`luaL_openlibs` opens the **entire** stdlib — `os` (`os.execute`, `os.remove`),
`io` (`io.open/popen`), `package`/`require` (with `LUA_USE_DLOPEN` live →
`package.loadlib` of native `.so`/`.dll`), `debug`, and `load`/`loadfile`/`dofile`.
The layer is **enabled by default** and runs `scripts/init.lua` **from the
working directory at startup**, which itself `dofile`s further scripts. Any
`scripts/*.lua` on disk — dropped by a downloaded "mod pack"/"custom map" bundle,
or anything that can write the cwd — executes with the **full privileges of the
game process**, at launch, with no prompt. The pcall "a broken mod can't crash
the game" wrapping does nothing against *intentional* code; it only swallows
errors. The `/lua <expr>` console (`luaai_api.c:~902`) is then a turnkey
`os.execute` from the in-game console.

**Fix:** do **not** call `luaL_openlibs`. Open only `base` (minus
`load*`/`dofile`/`require`), `table`, `string`, `math`, `coroutine`, `utf8`;
explicitly nil `os`/`io`/`package`/`debug`. Gate any file loading behind an
engine-controlled, path-whitelisted reader. **Never** execute network-delivered
Lua in this state. Until done, default `g_LuaAiEnabled = 0` and put the external
loader + `/lua` behind a developer flag.

### CR-8 — Lua: `ctx:exec(off)` OOB dispatch and `ctx:run(opcode,…)` full engine-command surface
`src/game/luaai.c:75-92` / `chrai.c:676-691`; `luaai.c:112-126` / `chrai.c:854-885`

`ctx:exec(off)` takes a script-supplied integer and does
`cmd = g_Vars.ailist + off; type = (cmd[0]<<8)+cmd[1]; g_CommandPointers[type]()`
with **no bound on `off`** → OOB read of the opcode and a controlled jump into
the command table at an arbitrary list position; the handler then parses its
operands from the same OOB location. `ctx:run(opcode, …)` lets a script invoke
**any** AI command (spawn/teleport/give-weapon/objective triggers) with arbitrary
operands, bypassing the `NetMode`/`chrFindByLiteralId` guards that wrap the
`pd.*` mutators. (The operand *buffer* is correctly capped at 60 bytes — no
overflow there — but the unrestricted command surface and unchecked `off` are
the problem.) **Fix:** bound `off` against the current ailist length in
`chraiLuaStep`; whitelist/validate opcodes for `ctx:run`, or don't expose it to
untrusted scripts.

---

## HIGH

### H-1 — `netbufCanRead` calls `__builtin_trap()` on any malformed/truncated packet → trivial remote DoS
`port/src/net/netbuf.c:28-37`

```c
if (buf->error || buf->rp + num > buf->wp) {
    sysLogPrintf(LOG_ERROR, "NET: could not read %u bytes", num);
    __builtin_trap();   // aborts the process
    buf->error = 1;     // dead code — the graceful path never runs
    return false;
}
```

Any peer (or a spoofed datagram on the connectionless paths) sending a short
packet **crashes the process**. The intended `error` flag that every handler
checks (`if (src->error) …`) is unreachable. **Fix:** delete `__builtin_trap()`;
set `error` and return false.

### H-2 — No finite/NaN validation on any wire-supplied position
`port/src/net/netmsg.c:~1148` (`chrSetPos` force-move), `:~1492-1499` (`netSimBlendLinear`)

CSP reconciliation itself is NaN-safe (a NaN error fails both threshold
comparisons → no correction — good). But the **force-move** branch writes the
wire `pos`/`theta` straight into `chrSetPos`, and `netSimBlendLinear` returns the
wire `target` directly when the current value is out of range, so a `NaN`/`inf`
target propagates into sim/player positions and poisons collision and rendering.
**Fix:** reject/clamp non-finite coords/angles at read time (a
`netbufReadCoordChecked` that sets `error` on non-finite is cleanest).

### H-3 — `chrDamage` broadcasts `SVC_CHR_DAMAGE` *before* the authoritative early-return filters, with raw pre-scaling damage
`src/game/chraction.c:4387-4391` (and the GE i-frame `return` at `:4349-4356` that precedes it)

The server emits the damage broadcast at the **top** of `chrDamage`, before the
CI-training/non-interactable/coop-FF/anti-kill/team-FF/`invincible` early-returns
and before difficulty/handicap/shield scaling. So clients play hit reactions and
decrement health for hits the server actually **rejected**, using an **un-scaled**
magnitude — a host/client divergence. Worse, the GoldenEye i-frame gate
`return`s *above* the broadcast using **unsynced local** `lastdamagetick60`, so
i-frame suppression is nondeterministic across host/clients. **Fix:** move the
broadcast to the damage-commit site and send the *applied* amount; make the
i-frame decision server-authoritative (or carry the outcome in the SVC message).

### H-4 — Lua: the "server-authoritative" claim is honor-system only; ESP/wallhack + desync are trivial
`chrai.c:719-729`, `luaai_api.c` query/draw API

Override application and the heavy mutators are gated on `NETMODE_CLIENT`, but
`ctx:exec`/`ctx:run`/`chraiLuaStep`/`chraiLuaRunSynthetic` have **no** NetMode
gate, and the read/draw API (`pd.chr_info`, `pd.player_pos`, `pd.each_chr`,
`pd.draw_box`) runs on clients — trivially enabling enemy-position/health ESP and
HUD overlays. "Authority" is client-side code the cheater controls; a modified
client deletes the check. **Fix:** be honest in the docs that this is not
server-authoritative; route all world mutation through host-originated
replication; accept that the read APIs leak and gate them in competitive play.

### H-5 — Lua: `CMD_PRINT` unbounded NUL scan defeats the transpiler's `maxlen` guard → OOB read
`chrai.c:1041-1049` (reached from `luaai_transpile.c:184` and `chraiLuaStep`)

```c
if (type == CMD_PRINT) {
    u32 prop = aioffset + 2;
    while (ailist[prop] != 0) ++prop;   // no maxlen bound — over-reads on truncated/corrupt lists
    ...
}
```

The transpiler bounds its outer walk to `maxlen`, but the length callback
ignores it, so a `CMD_PRINT` near the end of a truncated list scans off the
buffer. **Fix:** thread `maxlen` into the length computation and clamp the scan.

### H-6 — Cleartext join + admin passwords over an unauthenticated/unencrypted transport
`port/src/net/netmsg.c:253` (join), `net.c:~2890` (admin)

Both passwords are sent in plaintext over UDP and compared with
length-/early-exit-dependent `strcmp` (timing side channel; marginal over a
network but free to fix for the admin password, which grants server control).
There is no DTLS/handshake encryption. At minimum, use a constant-time compare
for the admin password and document that passwords are not confidential on the
wire.

---

## MEDIUM

- **M-1 — `chrSetPos` lacks NULL guards on `chr->prop` / `chr->model`.**
  `chraction.c:15976-16037`. This is the net-snap entry point
  (`net.c:2107`, `netmsg.c:1148`); its sibling `chrMoveToPos` is only reached
  through guarded paths. Current callers pass real models so it doesn't crash
  today, but a spectator/remote slot with a momentarily-NULL model would.
  Add an early `if (!chr || !chr->prop || !chr->model) return false;`.

- **M-2 — Inserting bodies mid-`g_MpBodies` shifts persisted/wire `mpbodynum`.**
  `mplayer.c:2443-2446`. Two entries inserted mid-array shift
  `BODY_CONNERY/MOORE/DALTON` (and all later) up by 2. `mpbodynum` is a
  saved/wire value indexed directly into `g_MpBodies[]`, so old setup wads and
  peers with a different table resolve the wrong body. Bounds are checked (no
  crash) but it's a silent save/replication mismatch. **Append** new bodies at
  the end; the stale `/*0x39*/ /*0x3a*/` comments are now wrong.

- **M-3 — SVC handlers apply wire `weaponnum`/`hitpart`/`damage` without range/finite checks.**
  `SVC_PLAYER_STATS` `bgunEquipWeapon(newweaponnum)` (`netmsg.c:1315`, unchecked
  s8 — note `SVC_CHR_DISARM` at `:2530` *does* bound it), `SVC_CHR_DAMAGE`
  (`:2414-2469`) and `SVC_PROP_DAMAGE` (`:2132-2135`) trust attacker `damage`
  with no `isfinite`. A malicious server NaN/instakills the local player.

- **M-4 — `g_NetNumClients` leaks on rejected connections.** `net.c:1056`
  increments before the protocol/full checks; the reject `return`s don't
  decrement. Normally rebalanced by the disconnect event, but a lost event
  drifts the counter permanently. Increment only after a slot is assigned.

- **M-5 — `CLC_CHAT` / `CLC_SETTINGS` have no state gate, length cap, or rate
  limit.** `netmsg.c:299,524`. A client in `CONNECTING`/`AUTH` (or a spectator)
  can broadcast chat (relayed verbatim, combined with CR-6's non-terminated
  string). Gate on `state >= CLSTATE_LOBBY` and clamp length.

- **M-6 — Lua fallback can leave state half-applied + is a global one-way kill
  switch.** `luaai.c:416-438`. On a Lua error mid-list the bytecode interpreter
  resumes from the current offset with no rollback of partial `pd.*`/`ctx:run`
  side effects (a netplay divergence source), and a single transient error sets
  the **global** `g_LuaAiEnabled = 0`, silently changing every actor's behavior
  for the rest of the session. Snapshot/restore the offset on error; make the
  disable per-list and log loudly.

- **M-7 — Lua chunk cache keyed by raw ailist pointer, only stage-invalidated.**
  `luaai.c:278-393`. If an ailist buffer is rebuilt/reallocated within a stage,
  the cache returns a stale transpiled chunk whose offsets no longer match → CR-8
  style OOB. Key on (stage, list id, content hash/length).

- **M-8 — Lua event emitters run handlers synchronously inside engine mutation
  paths → use-after-free.** `luaai_api.c:724-770` called from `chrDamage`/`chrDie`/
  spawn. A `kill` handler can `pd.chr_set_body` (frees+reallocs the model) the
  very chr being killed, leaving the calling C frame with a dangling pointer.
  Defer mutating effects to a safe end-of-frame drain, or enforce read-only
  handlers.

- **M-9 — Per-chr, per-frame `lua_pcall` on the AI hot path.** `luaai.c:382-439`.
  Even cached, a full Lua round-trip per actor per frame is a real cost and a GC
  pressure source on this engine's budget. Benchmark against the bytecode path
  before shipping enabled.

- **M-10 — Master browser parses datagrams from any source address.**
  `netmaster.c:292-431` (`netBrowserParseList`/`Query`) don't verify the reply
  came from the resolved master / the queried entry. Spoofed `LIST_RESPONSE`
  can inject arbitrary server-list entries. Bounded (no memory corruption — all
  copies are `strncpy` with caps and `np`/`nb` are clamped to
  `NET_MAX_CLIENTS`/`MAX_BOTS`), so impact is list pollution only.

- **M-11 — Unaligned multi-byte reads / strict-aliasing in `netbuf`.**
  `*(u16 *)&buf->data[rp]` etc. (`netbuf.c:60,68,78,200,…`). Fine on x86/x64,
  UB on strict-alignment targets and under `-fstrict-aliasing` — and the PR's own
  optimization notes flag ~900 misaligned-access sites elsewhere. Use `memcpy`
  into the typed temporary.

---

## LOW / nits

- **L-1** `mainmenu.c:1701-1744` — `labels[labelidx]` (4-entry array) can overrun
  if `joyGetConnectedControllers()` ever reports >4 set bits. Clamp `labelidx <= 3`.
- **L-2** `bondgun.c` `bgunUpdateReaper` stores `audiohandle = (void*)1` as a
  sentinel that's safe only because every consumer is `isremote`-gated; a future
  ungated `audioStop(handle)` derefs `0x1`. Prefer a dedicated bool flag.
- **L-3** Dead `char tmp[1024]` buffers in `netmsgClcChatRead` (`:301`) and
  `netmsgSvcChatRead` (`:712`).
- **L-4** `CLC_AUTH` reads a `players` byte (`netmsg.c:223`) that is then unused.
- **L-5** CSP reconcile silently no-ops for acks older than `NET_CSP_HISTORY_SIZE`
  (64 ticks) — correct, but worth a diag note at very high ping.
- **L-6** Lag-comp lookup underflow returns the oldest (zeroed) entry → a target
  shot in its first ~2s can rewind to world origin. Minor hit-reg glitch.

---

## What is actually done well (don't re-chase these)

These were checked and found correct — credit where due, and so reviewers don't
spend time re-verifying:

- **Admin auth gating** (`net.c:2862-2904`): `login` is the only pre-auth verb;
  everything else is behind `is_host || is_admin`; match-control behind
  `in_control`; `CLC_ADMIN_SETUP` independently gated
  (`!is_admin || g_NetAdminController != id`). No bypass found.
- **Lag compensation save/restore** (`net.c:2282-2303`): symmetric and
  exception-safe — no early-return path skips a restore; `rootmtx` balanced.
- **CSP reconcile** is NaN-safe by construction; the ring search is correct.
- **`SVC_SCORE` / `SVC_LOBBY_STATE` / `SVC_VOTE_*`** handlers clamp every count
  and index — the pattern the CR-2 sites should copy.
- **Client-side firing/damage authority is consistent**: the
  `chrDamageBy*`/projectile/disarm paths all early-return under `NETMODE_CLIENT`;
  no double-damage path on the client.
- **`modeldata/general.c`** is purely two `static modelstate[NUM_MODELS]` tables
  with JPN-guarded tail entries — no runtime allocation, no OOB; the "+1369
  lines" are data, not logic.
- **64-bit `g_MpSetup.options` serialization** (`mplayer.c:4602,4626`,
  `setup.c`) is correct, including the v5→v6 migration and the
  `(u64)param3 << 32` reconstruction.
- **`bg.c` draw-slot extension**, **`chrslots` 8-player bit math**, the
  **spectator** target cycling, the **playlist** parser, the **netmenu**
  password masking, and the **CMake** integration (Lua isolated as its own
  static lib with game flags excluded; dedicated server cleanly carved out) are
  all in-bounds / sound.

---

## Recommended remediation order

1. **Close the wire-index class (CR-1/2/3/4).** Add `netResolveWireClient(id)`
   and route every `g_NetClients[]`/`g_PlayerConfigsArray[]`/`g_ModelStates[]`
   access from wire data through a bound check. Highest impact, mostly mechanical.
2. **Fix `netbufReadStr` to require a trailing NUL (CR-6)** and **delete the
   `__builtin_trap` (H-1)**. Two tiny changes, broad coverage.
3. **Authorize/clamp `CLC_HIT` damage (CR-5)** and finite-check all wire
   positions/damage (H-2, M-3). This is what stops trivial cheating.
4. **Sandbox the Lua state (CR-7)**, bound `ctx:exec`/restrict `ctx:run` (CR-8),
   and default the layer **off** until done.
5. **Fix the `chrDamage` broadcast ordering (H-3)** for host/client determinism.
6. Work through the MEDIUM list; the LOW items are cleanup.

Until items 1–4 are addressed, treat `port-net-predict` as a LAN / trusted-peer
prototype, not something to expose on the public browser or to run untrusted
script bundles against.
