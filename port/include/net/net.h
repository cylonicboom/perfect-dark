#ifndef _IN_NET_H
#define _IN_NET_H

#include "types.h"
#include "constants.h"
#include "net/netbuf.h"

#define NET_PROTOCOL_VER 74 // 74: NET_MAX_CLIENTS = MAX_PLAYERS + 1 (9). A spectator host (dedicated / Host-Online, listen Host-Spectator) no longer burns a combatant slot — it sits on the extra +1 client slot so all MAX_PLAYERS (8) wire slots stay free for remote combatants (was 7 on dedicated). The lobby / SVC_STAGE_START manifests are count-prefixed and id-keyed, so the byte layout is unchanged for <=8 clients — but a 9-client server now emits client id 8, which only a proto-74 peer's netResolveWireClient accepts, so mixed versions must not join. "wire id 0 = host" is preserved.
// 73: SVC_RACE_STATE / SVC_ELIM_STATE per-combatant slices are now WIRE-KEYED (humans by netclient id, bots by mpchr index — the SVC_SCORE convention) instead of raw local slots, which differ per machine (netPlayersAllocate's local slot-0 swap) and made every client read the HOST's race progress / lives as its own. Same byte layout, different keying — mixed versions must not join.
// 72: "Race" scenario (MPSCENARIO_RACE 8, checkpoint racing over the KoH hillpads) — new SVC_RACE_STATE (0x58: per-racer progress + finish order + finish timer), and g_MpSetup.racelaps/racepitytime u8s appended after elimlives in SVC_STAGE_START and CLC_ADMIN_SETUP. See docs/PORT_RACE.md
// 71: Lives went GLOBAL (any scenario; Limits menu; elimlives 0 = off) and the short-lived Elimination scenario (id 8) was retired — same wire fields as 70 but gate semantics differ and id 8 no longer exists, so mixed versions must not join. See docs/PORT_ELIMINATION.md
// 70: "Elimination" scenario (MPSCENARIO_ELIMINATION, lives-based last-standing) — new SVC_ELIM_STATE (0x57: per-combatant lives + team pools + eliminated set), and g_MpSetup.elimlivesmode/elimlives u8s appended after zonecapturetime in SVC_STAGE_START and CLC_ADMIN_SETUP. See docs/PORT_ELIMINATION.md
// 69: "Zones" scenario (MPSCENARIO_ZONES, TS2-style territory control) — new SVC_ZONES_STATE (0x56: zone owners + team scores + score-cycle countdown), and g_MpSetup.zonescoretime/zonecapturetime u8s appended after paintclaimtime in SVC_STAGE_START and CLC_ADMIN_SETUP. See docs/PORT_ZONES.md
// 68: Graffiti "Claim Time" — g_MpSetup.paintclaimtime u8 appended after htmstaticpad in SVC_STAGE_START and CLC_ADMIN_SETUP (seconds in a room before it can be claimed; timer pauses while contested by another team). See docs/PORT_GRAFFITI.md
// 67: Classic Options — MPOPTION_CLASSIC_* high-word bits 34-45 (GE Style broken into per-behaviour options) + MPOPTION_NOCULL/NOOMLIMIT retired. Wire format unchanged (options already ride as u64) but gameplay-gate semantics differ across builds, so mixed versions must not join. See docs/PORT_GOLDENEYE.md
// 66: SVC_PAINT_STATE — "Graffiti" scenario broadcasts per-room team ownership (full owned-room list, on-change + 1s heartbeat); rooms tint to the last team to cross them. See docs/PORT_GRAFFITI.md
// 65: SVC_PROP_MOVE position quantization — when Net.Server.PosQuant is on, the per-chr coord rides as 3x s16 (6B) instead of 3x f32 (flags bit 5; out-of-range positions stay full coord). See docs/netplay-perf-review-2026.md P2
// 64: CLC_DOOR_ACTIVATE — client predicts a door and sends the host the exact door syncid, so high-ping door activation no longer depends on the host re-deriving the door from a lagged position + a momentary UCMD_ACTIVATE. See docs/netplay-perf-review-2026.md
// 63: netplayermove carries renderbehind (u8) — the client's g_NetInterpTicks render offset, so server lag-comp rewinds targets to the EXACT server-tick the shooter was displaying (inmovetick - renderbehind) instead of an RTT/2 + interp_lag symmetric-latency estimate. See docs/netplay-perf-review-2026.md lag-comp item
// 62: SVC_PROP_MOVE chr-state pose bandwidth cut — body yaw, the four aim joints, angleoffset and anim speed now ride as s16 (quantized) instead of f32 (-14 bytes/chr/tick; pos + chr->damage stay full-precision). See docs/netplay-perf-review-2026.md P1
// 61: co-op drop-in — SVC_COOP_CLAIM (seat/release dormant slots mid-mission) + SVC_PROP_RECONCILE also lists chr syncids in co-op (heals the joiner's ghost NPCs)
// 60: co-op SVC_STAGE_START manifest carries a spectator byte — mid-mission JIP joiners ride flagged spectator (sentinel playernum) instead of colliding with the host's slot 0
// 59: CLC_STAGE_READY (client world built) + JIP catch-up snapshot (replayed dynamic prop spawns, door/lift state to mid-match joiners)
// 58: SVC_STAGE_START carries g_MpSlotFnFlags[6] after the weapons block — playlist weapon/function bans + preset fn restrictions now enforced on clients
// 57: SVC_TIMESCALE — slow motion / combat boost: server mirrors the sim-step halving flag (+ boost timer) so clients tick in lockstep
// 56: CLC_BOT_CMD — clients can order own-team simulants (server validates team ownership + applies)
// 55: CLC_PICKUP_REQUEST — co-op clients can collect OBJ/weapon props (host re-validates + grants)
// 54: SVC_PROP_PICKUP carries the host's show-toast decision so co-op clients mirror it
// 53: CLC_OBJECTIVE_DONE — co-op client reports objectives it completed that the host can't witness
// 52: SVC_LOBBY_STATE carries an iscoop flag so clients show the co-op lobby window
// 47: SVC_STAGE_FLAGS — mirror host-authoritative g_StageFlags to co-op clients (scripted objective/gate completion)
// 46: SVC_CHR_TALK — replicate NPC voice lines (quips/conversation) to co-op clients
// 45: SVC_CHR_SPAWN — replicate host runtime chr spawns (reinforcements/clones) to co-op clients
// 44: SVC_OBJECTIVE — host-authoritative co-op objective status mirror
// 43: CLC_STAGE_COMPLETE — co-op client tells the host its local sim finished the mission so the host ends the stage for all
// 42: SVC_STAGE_START carries a co-op mode byte + difficulty (campaign co-op: clients load the solo stage via the co-op path instead of mpStartMatch)

#define NET_QUERY_MAGIC "PDQM\x01"

// MAX_PLAYERS combatant slots PLUS one extra client slot for a non-combatant
// host (dedicated / Host-Online / listen Host-Spectator). g_NetClients[] is
// sized [NET_MAX_CLIENTS + 1] — the trailing index is the client-side temp slot
// used before SVC_AUTH assigns a real id. Wire id 0 is always the host; remote
// combatants take ids in [1, NET_MAX_CLIENTS). A combatant host counts as one
// of MAX_PLAYERS, so a listen server effectively caps at MAX_PLAYERS clients
// (host + MAX_PLAYERS-1 remotes); only a spectator host uses all NET_MAX_CLIENTS
// (host + MAX_PLAYERS remotes). netStartServer applies that cap on g_NetMaxClients.
#define NET_MAX_CLIENTS (MAX_PLAYERS + 1)
#define NET_MAX_NAME MAX_PLAYERNAME
#define NET_MAX_ADDR 256

#define NET_BUFSIZE 1440

#define NET_DEFAULT_PORT 27100

// Snapshot ring buffer: stores the last N received moves per remote player for
// interpolation. Increasing this allows interpolation over a longer history at
// the cost of more memory per client.
#define NET_SNAPSHOT_COUNT    8

// Client-side prediction: how many past local positions to remember so we can
// measure prediction error when the server's authoritative state arrives.
#define NET_CSP_HISTORY_SIZE  64

// Lag compensation: server-side position history depth per remote client.
// 120 ticks ≈ 2 seconds at 60 Hz. Shots rewinding further than this will use
// the oldest available snapshot instead.
#define NET_LAGCOMP_SIZE      120

// Depth of the per-client recent server-detected-hit ring (see netclient.srvhits
// and g_NetHitValidate). A few ticks of history covers the small timing skew
// between when the server replays a remote's shot and when its CLC_HIT lands.
#define NET_SRVHIT_COUNT      12

// Server-side keep-alive cadence (ticks) for KoH-state and lobby-state
// broadcasts. ~1 second at 60 Hz. The KoH and lobby broadcasts use the same
// interval but are phase-offset by half (NET_HEARTBEAT_INTERVAL / 2) so they
// don't both land on the same tick.
#define NET_HEARTBEAT_INTERVAL 60u

// Live-tunable CSP / interp knobs. Were #define constants; converted to
// globals so the in-game /csp* and /stale console commands can adjust them
// at runtime without a rebuild. The CORR / TELEPORT thresholds are stored
// squared so the hot path comparison in netCspReconcile stays a single
// multiply + compare. Externs (rather than the original #defines) so call
// sites read the current values each frame.
//
// NET_CSP_CORR_FRAMES — ticks the CSP smooth correction spreads an error
// over. Smaller = snappier; larger = smoother but slower convergence.
// (g_NetCspCorrFrames itself is the *remaining* countdown — declared further
// down — so the tunable max lives in its own global.)
extern u32 g_NetCspCorrFramesMax;
#define NET_CSP_CORR_FRAMES ((s32)g_NetCspCorrFramesMax)

// NET_CSP_CORR_THRESH_SQ — minimum squared world-unit error before a CSP
// correction kicks in. Below it tiny server/client divergences are ignored
// to avoid continuous micro-corrections.
extern f32 g_NetCspCorrThreshSq;
#define NET_CSP_CORR_THRESH_SQ (g_NetCspCorrThreshSq)

// NET_CSP_TELEPORT_THRESH_SQ — squared distance above which the CSP path
// hard-snaps to the server position rather than smooth-correcting. Default
// 14400 (120 world units) covers max strafe-run + fastmovement + ramp +
// fall combined; anything beyond that is treated as a teleport / network
// glitch (smoothing it would chase a moving target and pinball).
extern f32 g_NetCspTeleportThreshSq;
#define NET_CSP_TELEPORT_THRESH_SQ (g_NetCspTeleportThreshSq)

// How many ticks the newest snapshot can lag behind g_NetTick before
// bwalkUpdateRemote hard-snaps to it instead of lerping between stale
// entries. Default 30 (~500ms) tolerates UpdateFrames=2..3 + jitter.
extern u32 g_NetStaleSnapshotTicks;

// Remote-player extrapolation window (ticks). When the newest snapshot is older
// than the interpolation target, bwalkUpdateRemote dead-reckons from last
// velocity for up to this many ticks instead of freezing. 0 = converge to newest
// (no extrapolation). Tunable via /extrap. See bondwalk.c. Also used by the
// network-chr (sim/co-op-NPC) interpolation below.
extern u32 g_NetExtrapMaxTicks;

// Network-replicated chr POSE interpolation (Combat Sim bots now; campaign NPCs
// under online co-op — same model: server runs the AI, replicates state, client
// interpolates). One snapshot is the chr's full facing pose at a wire instant.
struct netchrpose {
	struct coord pos;
	f32 yrot;            // body yaw (radians)
	f32 angleoffset;     // waist twist (aibot; decouples facing from move dir)
	f32 aimupback;       // upper-body aim joints (gun direction)
	f32 aimsideback;
	f32 aimuplshoulder;
	f32 aimuprshoulder;
	s16 animnum;         // server's leg/body animation at this instant (0 = none)
	s16 framea;          // server's anim frame index at this instant
	f32 speed;           // server's anim playback speed at this instant
	RoomNum rooms[8];    // wire room membership (applied time-aligned with pos in netChrInterpolate)
};

// netChrRecordSnapshot stamps the wire pose with the local receive tick into the
// chr's ring (called from the SVC_PROP_MOVE apply); netChrInterpolate runs every
// frame on the client and reconstructs the WHOLE pose (position + facing + aim)
// for one consistent past instant — the same scheme bwalkUpdateRemote uses for
// remote players, extended to the full facing pose so body/facing/gun agree.
// Bounded extrapolation when packets are late. Both no-op when there are no
// snapshots (or when g_NetChrInterp is 0), so they're safe to call on any chr.
void netChrRecordSnapshot(struct chrdata *chr, const struct netchrpose *pose);
void netChrInterpolate(struct chrdata *chr);

// Live toggle for the chr pose interpolation (console /chrinterp, default 1).
// 0 reverts to the receive-time per-packet apply (for A/B comparison).
extern s32 g_NetChrInterp;
extern s32 g_NetCoopChrLifecycle;
extern s32 g_NetCoopObjWireDriven;

// Campaign co-op body type (F2, docs/PORT_COOP_ONLINE.md). PER-PLAYER choice:
// g_NetCoopBodyMode is THIS machine's local player's selection; it rides
// CLC_SETTINGS to the host (settings.coopbodytype). At SVC_STAGE_START the host
// resolves every player's choice into per-player bits in g_NetCoopBodyBits (bit i
// = player i uses the masculine body) — COOPBODY_RANDOM is rolled host-side so all
// machines agree — and ships the bitmask. Jo's body+head are outfit-driven per
// level (playerChooseBodyAndHead); the "masculine" model is a per-outfit
// counterpart, falling back to the feminine model until that art exists (so this
// is currently a no-op visually). The HEAD is always the player's Combat Sim
// profile head, independent of body type.
#define COOPBODY_FEMININE  0
#define COOPBODY_MASCULINE 1
#define COOPBODY_RANDOM    2
extern s32 g_NetCoopHosting;  // host: this server was started for campaign co-op (advertised as netlobbystate.iscoop)
extern s32 g_NetCoopBodyMode; // COOPBODY_* — local player's choice (synced via CLC_SETTINGS)
extern u8 g_NetCoopBodyBits;  // resolved per-player masculine bitmask (host-assembled, synced)

// Campaign co-op LIVES mutator (F3, docs/PORT_COOP_ONLINE.md). Host setting,
// synced in SVC_STAGE_START. COOP_LIVES_OFF keeps the stock steal-half-a-buddy's-
// health revive; PER_PLAYER / SHARED replace it with a respawn budget — each death
// spends a life (own counter, or a shared pool of count*N), and at zero the player
// stays down. Host-authoritative: the host owns the counters and the all-out
// mission-end. (Per-player HUD readout + full counter sync is F3b.)
#define COOP_LIVES_OFF       0
#define COOP_LIVES_PERPLAYER 1
#define COOP_LIVES_SHARED    2
extern s32 g_NetCoopLivesMode;          // COOP_LIVES_* — host setting, synced
extern s32 g_NetCoopLivesCount;         // lives granted per player (host setting, synced)
extern s32 g_NetCoopLives[MAX_PLAYERS]; // per-player remaining (host-authoritative)
extern s32 g_NetCoopSharedLives;        // shared pool remaining (host-authoritative)

// Server-side CLC_HIT validation against the server's own lag-comp'd hit
// detection. 0 = off (trust the client, current behaviour); 1 = log-only
// (validate and log would-be rejections via netDiagLogf but still apply the hit
// — use this to measure agreement before enforcing); 2 = enforce (drop a claimed
// hit the server's authoritative trace never detected). Config
// Net.Server.HitValidate / console /hitvalidate. Default 0.
extern s32 g_NetHitValidate;

// Server-side: record that this client's shot was detected hitting prop `syncid`
// by the authoritative lag-comp'd shotCalculateHits pass (called from prop.c).
void netServerRecordDetectedHit(struct netclient *cl, u16 syncid);

// Server-side: did the server recently detect `shooter` hitting `syncid`? Used to
// validate a CLC_HIT claim. Returns 1 if found within the recent tick window.
s32 netServerHitWasDetected(const struct netclient *shooter, u16 syncid);

// Hidden test feature (toggle /hitmarker): brief centred hitmarker on a confirmed
// local hit. g_NetHitmarkerExpireTick is set when the local player's shot
// registers a chr/player hit (see chraction.c). Off by default.
extern s32 g_NetHitmarkerEnabled;
extern u32 g_NetHitmarkerExpireTick;
#define NET_HITMARKER_TICKS 12u // hitmarker visible window (~200ms at 60Hz)

// Kill feed: rolling list of recent eliminations shown top-left. New entries
// land at index 0 and older ones shift down. Tuned so a 4-way deathmatch keeps
// most of the action visible without flooding.
//
// Entries store shooter and victim names separately rather than a pre-formatted
// string so the renderer can colour each name independently (shooter green,
// victim red, separator white). An empty shooter name means "self-kill" — the
// victim died from suicide, environment damage, or unknown.
#define NET_KILLFEED_MAX            5
#define NET_KILLFEED_DURATION_TICKS (60 * 6)  // 6 seconds at 60Hz
#define NET_KILLFEED_NAME           24

struct netkillfeedentry {
	u32 expire_tick;
	char shooter[NET_KILLFEED_NAME];
	char victim[NET_KILLFEED_NAME];
	// MPTEAM index for each side, or 0xff for "no team / unknown". Render uses
	// these to colour the names from g_TeamColours[] so the feed visually
	// matches radar / on-chr highlights instead of always going green/red.
	u8 shooter_team;
	u8 victim_team;
};

// Lobby state: cached locally by the client when in CLSTATE_LOBBY.
// Populated from SVC_LOBBY_STATE broadcasts sent by the server every ~1s.
// Cleared on disconnect. Display strings are pre-resolved server-side so
// the render path can display them directly.
#define NET_LOBBY_TEAMNAME_LEN   12
#define NET_LOBBY_ARENANAME_LEN  32
#define NET_LOBBY_SCENNAME_LEN   32
#define NET_LOBBY_WPNSETNAME_LEN 32
#define NET_LOBBY_WPNNAME_LEN    24

struct netlobbyclient {
	char name[NET_MAX_NAME];
	u16 ping;
	u8 team;
	// Non-zero when the client is in host-spectator mode (no mpchr, no scoring).
	// Mirrored from netclient.is_spectator in SVC_LOBBY_STATE so the lobby UI
	// on remote clients can mark the host as "(spectator)" before stage start.
	u8 is_spectator;
};

struct netlobbybot {
	char name[NET_MAX_NAME];
	u8 team;
	u8 difficulty;
};

struct netlobbystate {
	u8 valid;
	u8 iscoop; // host is running a campaign co-op lobby (clients show the co-op window, not the Combat Sim one)
	u8 scenario;
	u8 stagenum;
	u64 options; // mirrors g_MpSetup.options (64-bit)
	u8 scorelimit;
	u8 timelimit;
	u16 teamscorelimit;
	u8 num_clients;
	struct netlobbyclient clients[NET_MAX_CLIENTS];
	u8 num_bots;
	struct netlobbybot bots[MAX_BOTS];
	char teamnames[MAX_TEAMS][NET_LOBBY_TEAMNAME_LEN];
	char arena_name[NET_LOBBY_ARENANAME_LEN];
	char scenario_name[NET_LOBBY_SCENNAME_LEN];
	char weaponset_name[NET_LOBBY_WPNSETNAME_LEN];
	char weapon_names[NUM_MPWEAPONSLOTS][NET_LOBBY_WPNNAME_LEN];
};

#define NET_NULL_CLIENT 0xFF
#define NET_NULL_PROP 0

// Sanity ceiling for client-reported hit damage (CLC_HIT). No legitimate weapon
// hit in this engine approaches this; it's a finite-magnitude backstop so a
// hostile client can't push an extreme value into the damage/health math. It is
// NOT a substitute for server-side damage authority (hit detection here is still
// client-reported) — see netmsgClcHitRead and docs/netplay-code-review-2026.md.
#define NET_MAX_HIT_DAMAGE 1000000.0f

// Sentinel playernum for a spectator netclient. Host spectator clients keep a
// netclient entry (so they receive broadcasts and can chat) but do not occupy
// a slot in g_PlayerConfigsArray / g_Vars.players / g_MpAllChrPtrs. Code that
// dereferences cl->playernum against those arrays must guard against this
// value. netPlayersAllocate skips spectator clients when assigning sequential
// playernums; remaining 0..MAX_PLAYERS-1 slots are free for combatants.
#define NET_PLAYERNUM_SPECTATOR 0xFE

#define NETCHAN_DEFAULT 0
#define NETCHAN_CONTROL 1
#define NETCHAN_COUNT 2

#define DISCONNECT_UNKNOWN  0
#define DISCONNECT_SHUTDOWN 1
#define DISCONNECT_VERSION 2
#define DISCONNECT_KICKED 3
#define DISCONNECT_BANNED 4
#define DISCONNECT_TIMEOUT 5
#define DISCONNECT_FULL 6
#define DISCONNECT_LATE 7
#define DISCONNECT_FILES 8
#define DISCONNECT_PASSWORD 9

#define CLSTATE_DISCONNECTED 0
#define CLSTATE_CONNECTING 1
#define CLSTATE_AUTH 2
#define CLSTATE_LOBBY 3
#define CLSTATE_GAME 4

#define UCMD_FIRE (1 << 0)
#define UCMD_ACTIVATE (1 << 1)
#define UCMD_RELOAD (1 << 2)
#define UCMD_AIMMODE (1 << 3)
#define UCMD_DUCK (1 << 4)
#define UCMD_SQUAT (1 << 5)
#define UCMD_ZOOMIN (1 << 6)
#define UCMD_SELECT (1 << 7)
#define UCMD_SELECT_DUAL (1 << 8)
#define UCMD_EYESSHUT (1 << 9)
#define UCMD_SECONDARY (1 << 10)
// STATE bit, not an input: the owner's chr is currently cloaked (cloaking
// device or RCP-120 cloak). The cloak DECISION (devicesactive / ammo drain /
// cloakpause) runs only on the owning machine; remote machines mirror this bit
// onto the chr's CHRHFLAG_CLOAKED (edge-detected in bmoveProcessRemoteInput so
// the on/off sound plays once per transition). Carried in every move, so a
// dropped packet self-heals on the next one. Old peers ignore the bit — no
// protocol bump.
#define UCMD_CLOAKED (1 << 11)
#define UCMD_RESPAWN (1 << 27)
#define UCMD_CHAT (1 << 28)
#define UCMD_IMPORTANT_MASK (UCMD_FIRE | UCMD_ACTIVATE | UCMD_RELOAD | UCMD_AIMMODE | UCMD_SELECT | UCMD_SELECT_DUAL)
#define UCMD_FL_FORCEPOS (1 << 29)
#define UCMD_FL_FORCEANGLE (1 << 30)
#define UCMD_FL_FORCEGROUND (1 << 31)
#define UCMD_FL_FORCEMASK (UCMD_FL_FORCEPOS | UCMD_FL_FORCEANGLE | UCMD_FL_FORCEGROUND)

// One saved local-player position per tick, used by CSP reconciliation.
struct csp_snapshot {
	u32 tick;
	struct coord pos;
};

// One saved world-space position per tick per remote client, used by the
// server for lag compensation hit rewinds.
struct lagcomp_snapshot {
	u32 tick;
	struct coord pos;
};

struct netplayermove {
	u32 tick; // g_NetTIck value when this struct was written; if 0, this struct is invalid
	u32 ucmd; // player commands (UCMD_)
	f32 leanofs; // analog lean value (-1 .. 1; equal to player->swaytarget / 75.f)
	f32 crouchofs; // analog crouch value (-90 for SQUAT, 0 for STAND; player->crouchofs)
	f32 zoomfov; // manual zoom fov for the current gun; synced only if UCMD_AIMING is set
	f32 movespeed[2]; // move inputs, [0] is forward, [1] is sideways; used mostly for animation
	f32 angles[2]; // view angles, [0] is theta, [1] is verta
	f32 crosspos[2]; // crosshair position in aiming mode; normalized to default aspect ratio
	s8 weaponnum; // switch to this weapon if UCMD_SELECT is set
	struct coord pos; // player position at g_NetTick == tick
	s16 animnum; // chr->model->anim->animnum at write time, 0 if unknown
	s16 animframe; // integer frame index of the active animation (chr->model->anim->framea)
	u8 renderbehind; // client's g_NetInterpTicks at write time (server lag-comp render offset, proto 63). Appended after the anim tail so it's outside netClientNeedMove's memcmp (it's ~constant, must not force sends)
};

struct netclient {
	struct _ENetPeer *peer;
	u32 id; // remote client number, server is always 0, even on clients
	u32 state; // CLSTATE_

	struct {
		char name[NET_MAX_NAME];
		u16 options;
		u8 headnum;
		u8 bodynum;
		u8 team;
		u8 coopbodytype; // F2: COOPBODY_* — this player's co-op body-type choice
		f32 fovy;
		f32 fovzoommult;
	} settings;

	struct mpplayerconfig *config;
	struct player *player;
	u8 playernum;
	// Host spectator flag — when non-zero this client is a non-combatant
	// observer. config/player are NULL on a spectator and playernum equals
	// NET_PLAYERNUM_SPECTATOR. Wired in CLC_SETTINGS (lobby) and re-broadcast
	// in SVC_STAGE_START's per-client manifest so all peers agree.
	u8 is_spectator;
	// Join-in-progress transient flag. Set to 1 when netServerEvConnect
	// accepts a late client (host is already in CLSTATE_GAME) and forces
	// is_spectator=1 so they observe the current round without trying to
	// allocate a player slot mid-match. mpStartMatch (server-side, at the
	// next round's start) walks g_NetClients[] and clears both this flag
	// and is_spectator for any client with this set, so they spawn cleanly
	// into the new round. Not sent over the wire — local server state only.
	u8 jip_pending_unspectate;
	// Server-side one-shot: the JIP catch-up snapshot was sent to this client
	// (in response to its CLC_STAGE_READY). Guards a misbehaving client from
	// requesting repeated snapshots.
	u8 jip_snapshot_sent;

	// Server-side only: set when this client has authenticated as an admin via
	// the CLC_ADMIN `login` command (password matches g_NetAdminPassword).
	// Grants access to admin commands; an admin may additionally "take control"
	// (recorded in g_NetAdminController) to suspend the playlist/vote auto-
	// advance and drive the match manually. Never sent over the wire.
	u8 is_admin;

	struct netplayermove outmove[2]; // last 2 outgoing player inputs, newest one first
	// Ring buffer of incoming player moves. inmove_head is the index of the
	// newest entry; older entries go backwards modulo NET_SNAPSHOT_COUNT.
	struct netplayermove inmove[NET_SNAPSHOT_COUNT];
	u32 inmove_head; // index of newest entry in inmove[]
	u32 inmovetick; // last inmove tick which was applied to the player
	u32 oneshot_fwd_tick; // server: inmove tick whose one-shot ucmd bits (RELOAD/SELECT) were last forwarded into the rebroadcast — forwarding them every frame replayed a stale reload tap forever on observers
	u8 renderbehind; // server-side: the firing client's render offset (g_NetInterpTicks) from its last applied inmove; lag-comp rewinds to inmovetick - renderbehind (proto 63)
	u32 outmoveack; // last acked outmove tick
	u32 forcetick; // tick on which the client's position was forced, or 0 if not forcing
	u32 lerpticks; // how many ticks we've been lerping the position
	// Adaptive interpolation: smoothed peak-hold estimate of how many ticks the
	// freshest received snapshot lags our local g_NetTick (≈ the full network
	// path for this client, in ticks). Re-estimated on every snapshot arrival in
	// netUpdateInterpLag. The interpolators subtract it so remote entities render
	// behind the *snapshot stream* rather than behind the local free-running
	// clock — which makes interpolation immune to the stage-start clock offset
	// and to 60Hz drift, and lets it work at any ping. 0 until the first snapshot.
	f32 interp_lag;

	// Server-side only: ring buffer of recent world positions for lag
	// compensation. Written each tick; indexed by lagcomp_head (newest).
	struct lagcomp_snapshot lagcomp[NET_LAGCOMP_SIZE];
	u32 lagcomp_head;

	// Server-side only: recent chr/player prop syncids this client's shots were
	// detected hitting by the server's own lag-comp'd shotCalculateHits pass.
	// Used to validate the client's CLC_HIT claims (see g_NetHitValidate): the
	// client shouldn't be able to claim a hit the server's authoritative,
	// lag-compensated trace never registered. Ring of {syncid, tick}.
	struct { u16 syncid; u32 tick; } srvhits[NET_SRVHIT_COUNT];
	u32 srvhits_head;

	struct netbuf out; // outbound messages are written here, except broadcasts
	struct netbuf in; // incoming packets are fed here

	u8 out_data[NET_BUFSIZE]; // buffer for out
};

extern s32 g_NetMode;

extern s32 g_NetJoinLatch;
extern s32 g_NetHostLatch;

// Dedicated-server mode. 0 = listen server (host plays), 1 = headless dedicated
// (no SDL window, no audio device, server-only tick), 2 = windowed dedicated
// (window open showing a status overlay, no local combatant). Set from the
// --dedicated / --dedicated-windowed CLI flags before videoInit/audioInit, or
// at runtime by the "Dedicated Server" menu handler (mode 2 only). When
// non-zero, g_NetLocalClient->is_spectator is forced to 1 in netStartServer
// and no local player slot is allocated.
extern s32 g_NetDedicatedMode;
extern s32 g_NetDedicatedLatch;
extern char g_NetServerName[64];
extern char g_NetPlaylistPath[260];

// Join password. g_NetServerPassword is the password this host requires (empty
// = open server); it is never sent over the wire — only a "passworded" flag is
// advertised, and the server string-compares the client's CLC_AUTH password
// against it. g_NetJoinPassword is the password the local client will send in
// its next CLC_AUTH (set by the browser / join menu before netStartClient).
#define NET_MAX_PASSWORD 64
extern char g_NetServerPassword[NET_MAX_PASSWORD];
extern char g_NetJoinPassword[NET_MAX_PASSWORD];

// Length-independent password compare. Unlike strcmp it does not early-exit on
// the first mismatching character, so its timing doesn't leak how many leading
// characters of `secret` the candidate matched. `cand` is only read within its
// own length, so a short attacker-supplied candidate is never over-read.
// Returns 1 if equal, 0 otherwise. (ENet itself is unencrypted, so this is
// defence-in-depth for the admin password, not strong transport security.)
s32 netSecureStrEqual(const char *secret, const char *cand);

// Admin remote control. g_NetAdminPassword (Server.AdminPassword / --admin-password;
// empty = admin disabled) gates the CLC_ADMIN `login` command. An authenticated
// admin may "take control" of the server, which suspends the dedicated playlist
// auto-start and the end-of-round vote so the admin can drive the match (end it,
// reconfigure, start) manually. g_NetAdminController holds the client id that
// currently holds control, or NET_NULL_CLIENT when nobody does.
extern char g_NetAdminPassword[NET_MAX_PASSWORD];
extern u32 g_NetAdminController;

// Host Online Game (master-spawned dedicated instance; docs/PORT_HOSTED_SERVER.md).
// g_NetHostOnlineMode is set from grant-accept until netDisconnect: this client
// is the instance's auto-admin "host" and drives the full Combat Sim hosting UI.
// The token is the per-instance admin password granted by the master; the
// one-shot setup-load latch and the push watchdog stamp are shared with
// menutick.c's post-match menu re-entry and the "Begin Match" interception.
extern s32 g_NetHostOnlineMode;
extern char g_NetAutoAdminToken[NET_MAX_PASSWORD];
extern s32 g_NetHostOnlineSetupLoad;
extern u32 g_NetHostOnlinePushTick;

// Send one CLC_ADMIN command line to the server (the console's /admin path).
// No-op unless connected as a client at CLSTATE_AUTH+.
void netClientSendAdminLine(const char *line);

// Re-apply the LOCAL pads to our own slot after mpReset's slot-indexed contpad
// assignment (slot i = pad i), which is wrong for a net local player seated at
// slot N >= 1 (spectator-host servers don't slot-0-swap). Called from mpReset
// per combatant slot; no-op for remote slots / non-net.
void netMpConfigFixLocalPads(s32 slot);

// True when g_Vars.currentplayer should receive mouse input: under netplay
// the single local (non-remote) pawn — which can sit at any slot — otherwise
// local player 0 (splitscreen: only player 1 owns the mouse). Replaces the
// raw currentplayernum == 0 gates at the mouse-input sites.
s32 netPlayerOwnsMouse(void);

// Host Online: reload a fresh CITRAINING world and re-enter the Combat Sim
// hosting UI through the post-match latch (menutick.c). Defined in netmenu.c.
void netHostOnlineEnterSetup(void);

// net frame, ticks at 60 fps, starts at 0 when the server is started
extern u32 g_NetTick;
extern u32 g_NetNextSyncId;
extern u32 g_NetFirstDynamicSyncId;

extern u64 g_NetRngSeeds[2];
extern u32 g_NetRngLatch;

// Dedicated seed used only by mpChooseTrack so that music selection stays in
// sync between host and clients. Seeded at stage start from g_RngSeed; only
// advanced when a new track is picked, so it never drifts due to server-side
// RNG consumers (AI, sims, particle effects) that the clients don't run.
extern u64 g_NetMusicRngSeed;

extern u32 g_NetInterpTicks;
extern s32 g_NetLagCompExact; // 1 = exact rewind (inmovetick - renderbehind, proto 63); 0 = legacy RTT/2 + interp_lag estimate. Live A/B via /lagcomp
extern s32 g_NetRelevancy; // P2: 1 = per-client relevancy-culled sim/NPC chr-state (default); 0 = identical broadcast to all. /relevancy
extern f32 g_NetRelevancyDist; // cull distance for a sim not sharing the client pawn's room (world units). /relevancy dist N
extern s32 g_NetPosQuant; // P2: 1 = quantize SVC_PROP_MOVE positions to s16 (proto 65, ~6B vs 12B, default); 0 = full coord. /posquant
extern f32 g_NetPosQuantScale; // world units per s16 step (default 1.0 = ~1-unit precision, +/-32767 range). /posquant scale N
extern u32 g_NetServerPort;
// Actual bound listen port of the running server (set in netStartServer). The
// master heartbeat advertises this so the tracker pairs it with the source IP.
extern u16 g_NetServerActualPort;
extern char g_NetLastJoinAddr[NET_MAX_ADDR + 1];

extern s32 g_NetDebugDraw;

// Client-side prediction: ring buffer of local-player positions, one per tick.
// Written by the client each frame; read when the server's ack arrives.
extern struct csp_snapshot g_NetCspHistory[NET_CSP_HISTORY_SIZE];
extern u32 g_NetCspHead;
// Pending smooth correction: g_NetCspCorrFrames counts down each tick from
// g_NetCspCorrFramesMax (NET_CSP_CORR_FRAMES); the per-tick step is delta/frames.
extern struct coord g_NetCspCorrDelta;
extern s32 g_NetCspCorrFrames;

extern s32 g_NetMaxClients;
extern s32 g_NetNumClients;
extern struct netclient g_NetClients[NET_MAX_CLIENTS + 1]; // last is an extra temporary client
extern struct netclient *g_NetLocalClient;

extern struct netbuf g_NetMsg;
extern struct netbuf g_NetMsgRel;

extern struct netlobbystate g_NetLobbyState;

// Vote-for-next-map state machine (port-only, dedicated/server side and
// client-side cache). Server opens a ballot at end-of-round (g_MpPaused ==
// MPPAUSEMODE_GAMEOVER on the host), broadcasts candidates, tallies after
// vote_seconds, and advances to the winner. Client mirrors the cache so
// /vote N and a future overlay menu can read the candidates list.
#define NET_VOTE_MAX_CANDIDATES 6

#define NETVOTE_IDLE     0
#define NETVOTE_OPEN     1
#define NETVOTE_RESULTS  2

struct netvotecandidate {
	s8  playlist_index;     // -1 (0xFF on wire) for the RANDOM slot
	u8  stagenum;
	u8  scenario;
	u8  preset_index;       // 0xFF = default
	u8  bot_count;
	u8  timelimit;
	u8  scorelimit;
	char name[32];
};

struct netvotestate {
	u8  state;              // NETVOTE_*
	u8  num_candidates;
	u8  vote_seconds;
	u8  winning_index;
	u8  winner_was_random;
	u32 deadline_tick;      // server-side tick at which the vote closes
	struct netvotecandidate candidates[NET_VOTE_MAX_CANDIDATES];
	u8  tally[NET_VOTE_MAX_CANDIDATES];
	s8  client_vote[NET_MAX_CLIENTS + 1]; // server-side: each client's choice, -1 = none
};

extern struct netvotestate g_NetVote;

// Server-side: open a vote ballot from the current playlist. Builds the
// candidate list (with optional RANDOM slot), broadcasts SVC_VOTE_OPEN,
// transitions state to NETVOTE_OPEN. No-op if vote already open or playlist
// empty.
void netServerVoteOpen(void);

// Server-side: tally + broadcast SVC_VOTE_RESULTS + apply winning entry +
// mpStartMatch. Called from netEndFrame when g_NetTick >= deadline_tick.
void netServerVoteClose(void);

// Server-side: record a CLC_VOTE from a client into the tally.
void netServerVoteRecord(struct netclient *cl, u8 candidate_index);

// Client-side: send a CLC_VOTE for the local choice. Returns -1 if no vote
// is currently open or the candidate index is out of range. /vote N hook.
s32 netClientVoteCast(s32 candidate_index);

const char *netFormatClientAddr(const struct netclient *cl);

void netInit(void);
s32 netDisconnect(void);
void netStartFrame(void);
void netEndFrame(void);

// Campaign co-op: enter a solo stage in N-player co-op (host trigger + client
// SVC_STAGE_START handler both call this). numplayers = total co-op players N
// (host + remote partners), up to MAX_PLAYERS.
void netCoopEnterStage(s32 stagenum, s32 difficulty, s32 numplayers);

// Co-op drop-in: net co-op stages always allocate this many player slots
// regardless of the connected count, so the prop/syncid layout is identical
// on every machine (the deterministic-allocation invariant) and a mid-mission
// joiner can claim a pre-allocated dormant slot. Matches the locked co-op
// plan cap of 4 players (PORT_COOP_PLAN.md); extra connectees stay spectators.
#define NET_COOP_MAX_SLOTS MAX_LOCAL_PLAYERS

// Seat a netclient on co-op player slot `playernum` (drop-in claim): binds
// netclient<->player, wakes the dormant pawn (unhide, clear isdormant/isdead),
// sets the config (local profile restore for the claimant; remote-config
// otherwise) and the F2 body bit. Shared by the server's claim decision and
// the client-side SVC_COOP_CLAIM apply.
void netCoopSeatClient(struct netclient *ncl, s32 playernum, const char *name, u8 bodybit);

// Server: pick a slot for a mid-mission co-op joiner (reserved-by-name first
// — reclaim after a disconnect — else first dormant), seat it, respawn the
// pawn through the checkpoint path and broadcast SVC_COOP_CLAIM. No-op if no
// slot is free (the joiner stays a spectator).
void netServerCoopClaim(struct netclient *cl);

// Mark co-op slot `playernum` dormant again (release): park the pawn dead +
// hidden and unbind any netclient holding it. Used at release-on-disconnect
// (server + SVC_COOP_CLAIM release broadcast on clients).
void netCoopDormantSlot(s32 playernum);

s32 netStartServer(u16 port, s32 maxclients);
s32 netStartClient(const char *addr);

u32 netSend(struct netclient *dstcl, struct netbuf *buf, const s32 reliable, const s32 chan);

// Console command handler — returns 1 if the line was consumed as a netplay
// command (and shouldn't be relayed as chat), 0 otherwise. Lines beginning
// with '/' are commands; everything else is regular chat. Implemented in
// net.c so the command set lives alongside the state it manipulates.
s32 netConsoleCommand(const char *line);

// Simulated outgoing latency in ms. When > 0 every outbound packet (chat
// included) is queued and released after the requested delay. Set via
// '/lag <ms>' in the console.
extern s32 g_NetSimLagMs;

void netChat(struct netclient *dst, const char *text);
void netChatPrintf(struct netclient *dst, const char *fmt, ...);

// Server-side: execute an admin command line received via CLC_ADMIN from cl
// (or from the local host console). All output is sent back to cl via SVC_ADMIN
// using netAdminReply. Authentication and "in control" checks are enforced
// inside. Defined in net.c.
void netServerAdminCommand(struct netclient *cl, const char *line);

// Send one formatted SVC_ADMIN reply line to a single client (admin console
// output). On the local host (cl == g_NetLocalClient) it logs locally instead.
void netAdminReply(struct netclient *cl, const char *fmt, ...);

// Client-side admin "start": serialize the locally-configured g_MpSetup + bot
// configs and push them to the server (CLC_ADMIN_SETUP), which then starts the
// match and broadcasts it to all clients. On a listen host it just starts.
void netAdminPushStart(void);
bool netAdminAutoEndForRestart(void); // auto-end in-progress match on a setup push (one-click restart)

// Client-side: load the Combat Sim setup file + prime the Combat Sim menu so the
// admin can configure the match via the normal built-in menu before pushing it.
// Defined in netmenu.c where the menu handlers live.
void netAdminConfigure(void);

void netServerStageStart(void);
void netServerStageEnd(void);

// JIP catch-up snapshot: replay to ONE freshly-authed mid-match joiner the
// world state its fresh stage load can't reproduce — runtime-spawned
// weapon/obj props (dropped guns, projectiles, mines; syncid >=
// g_NetFirstDynamicSyncId) plus current door modes and lift state. Called
// right after the JIP SVC_STAGE_START is shipped (netmsgClcAuthRead).
// Everything else heals via the existing heartbeats (score, stats,
// prop-reconcile, KoH, timescale) within ~1s. Server-only no-op otherwise.
void netServerSendJipSnapshot(struct netclient *cl);
void netClientStageComplete(void);
void netServerBroadcastObjectives(void);
void netClientSendObjectiveDone(s32 objindex);
// Combat Sim: forward a simulant order from the client's active menu to the
// server (CLC_BOT_CMD). botindex/targetindex are g_MpAllChrPtrs indices (wire-
// stable: players sit at their playernum, bots follow via the deterministic
// allocation invariant). targetindex is only meaningful for AIBOTCMD_ATTACK;
// pass -1 otherwise. The server validates team ownership before applying.
void netClientSendBotCmd(s32 botindex, u32 command, s32 targetindex);
void netClientRequestPickup(struct prop *prop);
void netServerBroadcastChrSpawn(struct prop *prop, f32 angle, u32 spawnflags);
void netServerBroadcastChrTalk(struct prop *prop, s32 audioid);
// F3 lives: "N lives remaining" respawn notification. The host calls
// netServerNotifyLives on a respawn (shared -> all players; individual -> just the
// victim); it shows the message to host-local players and sends SVC_COOP_LIVES to
// the relevant client(s), whose netCoopShowLivesMsg renders it for their local player.
void netServerNotifyLives(s32 victimplayernum, s32 count, bool shared);
void netCoopShowLivesMsg(s32 count);
void netServerKick(struct netclient *cl, const u32 reason);

// Co-op host-authoritative objective status, set by SVC_OBJECTIVE on the client
// and overlaid onto the client's local objectiveCheck() (union). Sized
// MAX_OBJECTIVES in net.c; declared incomplete here so net.h needn't pull in
// constants.h. Indexed by objective index.
extern u32 g_NetCoopObjStatuses[];
extern u8 g_NetCoopClientObjDone[]; // host: objectives a client reported done via CLC_OBJECTIVE_DONE (latched into objectiveCheck)
extern u8 g_NetCoopObjToastShown[]; // co-op: per-objective completion-toast-shown latch (shows the toast once the HUD is up even if it completed during a cutscene)
extern s8 g_NetPickupWireShowMsg;   // client: -1 = normal local gate; 0/1 = host's toast decision for a wire-driven SVC_PROP_PICKUP

struct netclient *netClientForPlayerNum(s32 playernum);

void netClientSyncRng(void);
void netClientSettingsChanged(void);

void netPlayersAllocate(void);
void netSyncIdsAllocate(void);

// Entity interpolation: update a client's interp_lag estimate from a freshly
// received snapshot's tick. Peak-holds the worst recent (g_NetTick - snaptick)
// staleness and decays it slowly, giving a self-sizing jitter buffer. Called
// from both move-read paths (server: netmsgClcMoveRead; client:
// netmsgSvcPlayerMoveRead) after the inmove ring push. No-op when snaptick == 0.
void netUpdateInterpLag(struct netclient *cl, u32 snaptick);

// Client-side prediction: compare server's authoritative position at ack_tick
// to what the client predicted, and schedule a smooth correction if the error
// exceeds NET_CSP_CORR_THRESH_SQ.
void netCspReconcile(u32 ack_tick, const struct coord *server_pos, f32 server_theta);

// Client-side prediction: apply one tick's worth of the pending smooth
// correction. Call once per game tick after physics have run.
void netCspTick(void);

// Lag compensation: snapshot a remote client's current world position.
// Called by the server each tick for every connected client.
void netLagCompSave(struct netclient *cl);

// Lag compensation: rewind all remote client positions to what the shooter saw.
// Must be followed by netLagCompEnd() after hit detection.
void netLagCompBegin(const struct netclient *shooter);

// Lag compensation: restore all positions modified by netLagCompBegin().
void netLagCompEnd(void);

Gfx *netDebugRender(Gfx *gdl);

// Kill feed: append an entry to the on-screen elimination list. The server
// calls this directly on the host (where it also generates the broadcast),
// and the client calls it from SVC_KILL receive. Pass an empty or NULL
// shooter to mark a self-kill (suicide / environmental death).
void netKillFeedAdd(const char *shooter, const char *victim, u8 shooter_team, u8 victim_team);

// Kill feed: render the rolling list of recent eliminations in the top-right.
// Returns the updated display list pointer.
Gfx *netKillFeedRender(Gfx *gdl);

// Vanity easter egg: render the boxed lower-left "Graslu" banner (pickup-message
// style) when the hidden /graslu command has toggled it on and a level is
// running. Called on the HUD layer from playerRenderHud (just after
// hudmsgsRender) — not as a top-level overlay, so don't also call it from pdmain.
Gfx *netGrasluRender(Gfx *gdl);

// Companion red "Redvox57" vanity banner; same HUD slot/renderer as Graslu.
Gfx *netRedvox57Render(Gfx *gdl);

// Renders the egg banners' FADE-OUT only, for the frames where the player HUD is
// removed (lv.c's `var80075d60 != 2` path). netGrasluRender/netRedvox57Render
// drive the fade-in/hold while the HUD is drawn; this drives the animate-away when
// it is removed, so the banner slides out instead of vanishing. No-op once gone.
Gfx *netCoopEggsRenderHidden(Gfx *gdl);

// Hidden test hitmarker: centred marker shown briefly after a confirmed local
// hit (toggle /hitmarker). No-op unless enabled and within the flash window.
Gfx *netHitmarkerRender(Gfx *gdl);

// Spectate mode. When non-NULL, the local player's first-person camera is
// overridden to ride along with the target chr (player or sim). Cleared by
// /spec off or when the target disappears. Drive it from the /spec console
// command, or set directly from a HUD binding. Render hook lives in
// playerTick (see netSpectateApply).
extern struct chrdata *g_NetSpectateChr;

// Cycle the spectate target. direction: +1 = next, -1 = prev. Selects from
// live players and sims (skips the local player and dead targets). Sets
// g_NetSpectateChr to the chosen chr, or NULL if no valid target.
void netSpectateCycle(s32 direction);

// Clear the spectate override and restore normal first-person view.
void netSpectateStop(void);

// Per-frame hook: when g_NetSpectateChr is set, override the local player's
// camera pose to follow the target. Called from playerTick after physics so
// it has the final-for-this-frame pos to read.
void netSpectateApply(void);

// Manual spectate toggle (client): enter spectate of the first live target if
// not spectating, else return to first-person. For a key bind / console command.
void netSpectateToggle(void);

// Per-frame client hook: auto-spectate on death, return to own view on respawn.
// Call once per frame for the local client (alongside netSpectateApply).
void netSpectateAutoUpdate(void);

// Append one line to the active diagnostic log if /diag has opened one.
// Format is "<tick>,<realtime_s>,<event>,<formatted args>". No-op when the
// log isn't open, so call sites can sprinkle these without guarding.
void netDiagLogf(const char *event, const char *fmt, ...);

// Server-side deferred hit processing: called from netmsgClcHitRead instead
// of chrDamage directly. Queues the hit for processing in netEndFrame, after
// the send buffers have been reset but before the first flush. This ensures
// SVC_CHR_DAMAGE (and SVC_KILL / SVC_SCORE on kill) lands in a fresh buffer
// and is actually broadcast to clients.
void netServerEnqueueHit(struct prop *target, f32 damage, const struct coord *vector,
        const struct gset *gset, s16 hitpart, s16 side, const s16 *arg10,
        s32 playernum, struct prop *shooter_prop);

// Lag-style defer for client-reported destructible-prop hits (CLC_PROP_HIT).
// Same rationale as netServerEnqueueHit: objDamage broadcasts SVC_PROP_DAMAGE,
// so it must run in netEndFrame (after the buffer reset, before flush).
void netServerEnqueuePropHit(struct prop *prop, f32 damage, const struct coord *pos,
        s32 weaponnum, s32 playernum);

// Client -> server: report our local player's gunfire hit on a destructible prop.
// Called from objTakeGunfire; no-op unless we're a connected client in-game.
void netClientReportPropHit(struct prop *prop, f32 damage, const struct coord *pos, s32 weaponnum);

// "Graffiti" scenario (MPSCENARIO_PAINTROOM) shared state. The owner array
// + room count live in g_ScenarioData.paint (scenarios.c); these accessors let
// the net layer (netmsg.c/net.c) read/write ownership and the host signal that
// the painted set changed this frame so netEndFrame broadcasts SVC_PAINT_STATE.
// paintSetRoomOwner applies one room's owner + the LIGHTOP_HIGHLIGHT/reshade so
// both the host tick and the client wire-apply share one code path.
u8 *paintGetRoomOwner(s32 *roomcount_out);
void paintSetRoomOwner(s32 roomnum, u8 owner);

// "Zones" scenario (MPSCENARIO_ZONES) shared state — the same arrangement as
// the Graffiti accessors above. zonesGetData exposes the zone/score state for
// the SVC_ZONES_STATE writer (struct scenariodata_zones is in types.h);
// zonesApplyWireState is the client-side apply (owners via the shared setter
// so rooms re-tint, authoritative team scores, cycle countdown resync);
// g_MpZonesDirty is the host's on-change broadcast signal.
struct scenariodata_zones *zonesGetData(void);
void zonesApplyWireState(const u8 *owners, s32 count, const s32 *teamscores, s32 cycle240);
extern u8 g_MpZonesDirty;
void paintHandleDeath(s32 aplayernum, s32 vplayernum); // kill claims the killer's room (mpstatsRecordDeath hook)
extern u8 g_MpPaintDirty; // host: painted set changed this frame -> broadcast in netEndFrame

// Global Lives system shared state (elimination.inc; scenario-independent,
// active when g_MpSetup.elimlives > 0) — the zones/paint accessor pattern.
// elimGetData feeds the SVC_ELIM_STATE writer; elimApplyWireState is the
// client apply; elimHandleDeath is the mpstatsRecordDeath hook (spends a
// life); elimChrCanRespawn gates the MP player respawn (player.c) and the
// bot corpse-fade respawn (chraction.c).
struct elimdata *elimGetData(void);
void elimApplyWireState(const u8 *lives, const u8 *teamlives, u16 elimmask);
void elimHandleDeath(s32 aplayernum, s32 vplayernum);
bool elimChrCanRespawn(struct chrdata *chr);
bool elimShouldEndMatch(void); // polled from lv.c's match-end block (NOT elimTick — the reasons counter resets each frame)
extern u8 g_MpElimDirty;

// "Race" scenario (MPSCENARIO_RACE) shared state — the same accessor
// pattern. raceGetData feeds the SVC_RACE_STATE writer; raceApplyWireState
// is the client apply; raceShouldEndMatch is polled from lv.c's match-end
// block (same rule as elimShouldEndMatch above).
struct scenariodata_race *raceGetData(void);
void raceApplyWireState(const u8 *nextcp, const u8 *lapsdone, const u8 *finishpos,
		u8 finishcount, u8 humancount, u8 pitystarted, s32 pity240);
bool raceShouldEndMatch(void);
extern u8 g_MpRaceDirty;

#endif // _IN_NET_H
