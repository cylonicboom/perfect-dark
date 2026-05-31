#ifndef _IN_NET_H
#define _IN_NET_H

#include "types.h"
#include "constants.h"
#include "net/netbuf.h"

#define NET_PROTOCOL_VER 35 // 35: CLC_PROP_HIT (client-reported destructible-prop / glass damage)

#define NET_QUERY_MAGIC "PDQM\x01"

#define NET_MAX_CLIENTS MAX_PLAYERS
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
	u8 scenario;
	u8 stagenum;
	u32 options;
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

// Admin remote control. g_NetAdminPassword (Server.AdminPassword / --admin-password;
// empty = admin disabled) gates the CLC_ADMIN `login` command. An authenticated
// admin may "take control" of the server, which suspends the dedicated playlist
// auto-start and the end-of-round vote so the admin can drive the match (end it,
// reconfigure, start) manually. g_NetAdminController holds the client id that
// currently holds control, or NET_NULL_CLIENT when nobody does.
extern char g_NetAdminPassword[NET_MAX_PASSWORD];
extern u32 g_NetAdminController;

// net frame, ticks at 60 fps, starts at 0 when the server is started
extern u32 g_NetTick;
extern u32 g_NetNextSyncId;

extern u64 g_NetRngSeeds[2];
extern u32 g_NetRngLatch;

// Dedicated seed used only by mpChooseTrack so that music selection stays in
// sync between host and clients. Seeded at stage start from g_RngSeed; only
// advanced when a new track is picked, so it never drifts due to server-side
// RNG consumers (AI, sims, particle effects) that the clients don't run.
extern u64 g_NetMusicRngSeed;

extern u32 g_NetInterpTicks;
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

// Client-side: load the Combat Sim setup file + prime the Combat Sim menu so the
// admin can configure the match via the normal built-in menu before pushing it.
// Defined in netmenu.c where the menu handlers live.
void netAdminConfigure(void);

void netServerStageStart(void);
void netServerStageEnd(void);
void netServerKick(struct netclient *cl, const u32 reason);

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

#endif // _IN_NET_H
