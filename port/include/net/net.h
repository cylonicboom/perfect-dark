#ifndef _IN_NET_H
#define _IN_NET_H

#include "types.h"
#include "constants.h"
#include "net/netbuf.h"

#define NET_PROTOCOL_VER 21

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

// Smooth correction: number of game ticks over which a CSP position error is
// blended away. Smaller = snappier corrections; larger = smoother but slower.
#define NET_CSP_CORR_FRAMES   10

// Minimum squared error (in world units) needed to trigger a CSP correction.
// Below this threshold, tiny server/client divergences are ignored to avoid
// continuous micro-corrections.
#define NET_CSP_CORR_THRESH_SQ 625.f  // 25 units

// Maximum plausible per-tick movement for a player, used as the "this is a
// teleport, not a smooth-correctible drift" threshold. Beyond this the CSP
// path hard-snaps to the server position and cancels any in-flight smooth
// correction, instead of trying to interpolate over many frames (which is
// what produces visible pinballing when corrections keep retargeting).
//
// Derivation (see player movement notes in bondwalk.c):
//   diagonal strafe-run max normalized speed = sqrt(1.08^2 + 1.0^2) ≈ 1.47
//   with MPOPTION_FASTMOVEMENT eye-height multiplier (1.25x): ≈ 1.84
//   estimated world units per tick at peak: ~25 horizontal, ~50 vertical
//   plus ~50% running-down-ramp gravity boost and a safety buffer
//
// 120 world-units total magnitude covers strafe-run + fastmovement + ramp +
// fall combined; anything past that is not physically reachable in a single
// tick and is treated as a teleport / network glitch. Squared so the CSP
// reconcile can compare against err_sq without a sqrt.
#define NET_CSP_TELEPORT_THRESH_SQ 14400.f  // 120 units

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
};

#define NET_NULL_CLIENT 0xFF
#define NET_NULL_PROP 0

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

	struct netplayermove outmove[2]; // last 2 outgoing player inputs, newest one first
	// Ring buffer of incoming player moves. inmove_head is the index of the
	// newest entry; older entries go backwards modulo NET_SNAPSHOT_COUNT.
	struct netplayermove inmove[NET_SNAPSHOT_COUNT];
	u32 inmove_head; // index of newest entry in inmove[]
	u32 inmovetick; // last inmove tick which was applied to the player
	u32 outmoveack; // last acked outmove tick
	u32 forcetick; // tick on which the client's position was forced, or 0 if not forcing
	u32 lerpticks; // how many ticks we've been lerping the position

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
extern char g_NetLastJoinAddr[NET_MAX_ADDR + 1];

extern s32 g_NetDebugDraw;

// Client-side prediction: ring buffer of local-player positions, one per tick.
// Written by the client each frame; read when the server's ack arrives.
extern struct csp_snapshot g_NetCspHistory[NET_CSP_HISTORY_SIZE];
extern u32 g_NetCspHead;
// Pending smooth correction: delta remaining to be applied over g_NetCspCorrFrames ticks.
extern struct coord g_NetCspCorrDelta;
extern s32 g_NetCspCorrFrames;

extern s32 g_NetMaxClients;
extern s32 g_NetNumClients;
extern struct netclient g_NetClients[NET_MAX_CLIENTS + 1]; // last is an extra temporary client
extern struct netclient *g_NetLocalClient;

extern struct netbuf g_NetMsg;
extern struct netbuf g_NetMsgRel;

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

void netServerStageStart(void);
void netServerStageEnd(void);
void netServerKick(struct netclient *cl, const u32 reason);

struct netclient *netClientForPlayerNum(s32 playernum);

void netClientSyncRng(void);
void netClientSettingsChanged(void);

void netPlayersAllocate(void);
void netSyncIdsAllocate(void);

// Client-side prediction: compare server's authoritative position at ack_tick
// to what the client predicted, and schedule a smooth correction if the error
// exceeds NET_CSP_CORR_THRESH_SQ.
void netCspReconcile(u32 ack_tick, const struct coord *server_pos);

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
void netKillFeedAdd(const char *shooter, const char *victim);

// Kill feed: render the rolling list of recent eliminations in the top-right.
// Returns the updated display list pointer.
Gfx *netKillFeedRender(Gfx *gdl);

#endif // _IN_NET_H
