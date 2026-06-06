#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <ctype.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include "platform.h"
#include "net/netenet.h"
#include "net/net.h"
#include "net/netbuf.h"
#include "net/netmsg.h"
#include "net/netmaster.h"
#include "net/playlist.h"
#include "det.h"
#include "mpsetups.h"
#include "types.h"
#include "constants.h"
#include "data.h"
#include "bss.h"
#include "lib/rng.h" // rngCosmeticRandom — F2 co-op body randomisation (unsynced, host-side)
#include "game/hudmsg.h"
#include "game/menugfx.h"
#include "game/playermgr.h"
#include "game/player.h"
#include "game/bondgun.h"
#include "game/cheats.h"
#include "game/challenge.h"
#include "game/bg.h"
#include "game/game_1531a0.h"
#include "game/luaai.h"
#include "game/game_0b0fd0.h"
#include "game/title.h"
#include "game/lv.h"
#include "game/menu.h"
#include "game/pdmode.h"
#include "game/mplayer/mplayer.h"
#include "spectator.h"
#include "game/chraction.h"
#include "game/chr.h"
#include "game/prop.h"
#include "game/propobj.h"
#include "lib/main.h"
#include "lib/vi.h"
#include "lib/model.h"
#include "lib/anim.h"
#include "config.h"
#include "system.h"
#include "console.h"
#include "fs.h"
#include "romdata.h"
#include "utils.h"

// Forward-decl only: input.h can't be included here — windows.h (pulled in via
// enet.h -> winsock2.h) #defines VK_RETURN/VK_ESCAPE/... which collide with
// input.h's virtkey enum. Same pattern as audio.c's g_NetDedicatedMode.
extern void inputPadTest(const char *arg);     // /padtest debug command (input.c)
extern void inputGyroCommand(const char *arg); // /gyro aim control (input.c)

s32 g_NetMode = NETMODE_NONE;

// Last g_StageFlags value broadcast to co-op clients, so netEndFrame only sends
// SVC_STAGE_FLAGS on change (plus a periodic heal). Reset at co-op stage entry.
// Defined here (above netCoopEnterStage's reset) since it's file-static — unlike
// g_NetCoopObjStatuses, which has an extern in net.h covering its forward use.
static u32 g_NetLastStageFlags;

// Last co-op cutscene state broadcast (active + anim), so netEndFrame only sends
// SVC_CUTSCENE on a transition. Reset at co-op stage entry.
static s32 g_NetLastCutsceneActive;
static s16 g_NetLastCutsceneAnim;

// Last slow-motion engaged flag broadcast (SVC_TIMESCALE), so netEndFrame only
// sends on a transition (plus a periodic heal). Self-corrects across stages:
// lvReset zeroes g_LvSlomoEngaged, so a stale 1 here just triggers one
// harmless "off" broadcast at the next in-game frame.
static u8 g_NetLastTimescale;

s32 g_NetHostLatch = false;
s32 g_NetJoinLatch = false;

// Snapshot of the LOCAL machine's player-1 profile (g_PlayerConfigsArray[0]),
// taken at net session start while it's still pristine. netPlayersAllocate
// repurposes the shared g_PlayerConfigsArray slots for REMOTE players
// (CONTROLMODE_NA, remote-adjusted options, their name/body) — while the
// local client is a JIP spectator, that stomps EVERY slot including [0], and
// when the next round seats us on one of those slots we'd inherit the
// poison: CONTROLMODE_NA kills all input including the pause menu (bondmove
// early-returns before the ESC/START handling). The local-bind path in
// netPlayersAllocate and the co-op drop-in seat (netCoopSeatClient) restore
// the input/identity fields from this snapshot.
static struct mpplayerconfig g_NetLocalProfileBackup;

// Restore the input/identity fields of the local profile into a config slot
// the local client is binding to. base.team is deliberately untouched (the
// stage-start manifest / claim flow owns it). Contpads restored too — a co-op
// drop-in claimant binds at its wire slot N, whose mpReset default contpad is
// pad N, not the local pad 0.
static void netRestoreLocalProfile(struct mpplayerconfig *cfg)
{
	// Guard a snapshot that was itself taken from a poisoned profile (e.g. a
	// force-closed session left NA in the array): never seat the local player
	// with dead controls.
	cfg->controlmode = (g_NetLocalProfileBackup.controlmode == CONTROLMODE_NA)
			? CONTROLMODE_11 : g_NetLocalProfileBackup.controlmode;
	cfg->options = g_NetLocalProfileBackup.options;
	cfg->contpad1 = g_NetLocalProfileBackup.contpad1;
	cfg->contpad2 = g_NetLocalProfileBackup.contpad2;
	cfg->base.mpbodynum = g_NetLocalProfileBackup.base.mpbodynum;
	cfg->base.mpheadnum = g_NetLocalProfileBackup.base.mpheadnum;
	memcpy(cfg->base.name, g_NetLocalProfileBackup.base.name, sizeof(cfg->base.name));
}

// Dedicated-server mode latches. g_NetDedicatedLatch is set by --dedicated
// (mode 1) or --dedicated-windowed (mode 2) at CLI parse time, before any
// subsystem init. main.c copies it to g_NetDedicatedMode before videoInit /
// audioInit so those subsystems can no-op cleanly. g_NetDedicatedMode is also
// set directly by the "Dedicated Server" menu handler (always to 2 — windowed
// is the only mode available post-init since videoInit has already run).
s32 g_NetDedicatedMode = 0;
s32 g_NetDedicatedLatch = 0;
char g_NetServerName[64] = "Perfect Dark Dedicated";
// Join password (see net.h). Empty server password = open server.
char g_NetServerPassword[NET_MAX_PASSWORD] = "";
char g_NetJoinPassword[NET_MAX_PASSWORD] = "";
// Admin remote control (see net.h). Empty password = admin disabled.
char g_NetAdminPassword[NET_MAX_PASSWORD] = "";
u32 g_NetAdminController = NET_NULL_CLIENT;

// Admin scratch match config. The admin `set` commands edit this while holding
// control; `apply` runs it through playlistApply + mpStartMatch, and
// `saverotation` appends a copy to the live playlist. Captured from the current
// g_MpSetup when an admin takes control so tweaks build on what's running.
static struct playlistentry g_NetAdminSetup;
// $S = save dir (same place pd.ini lives). Override via --playlist <path>
// or Server.PlaylistPath in pd.ini; absolute / cwd-relative paths are
// honored as-is by fsFullPath.
char g_NetPlaylistPath[260] = "$S/server_playlist.ini";

struct netvotestate g_NetVote;

u32 g_NetServerUpdateRate = 1;
s32 g_NetLagCompExact = 1; // 1 = exact rewind (inmovetick - renderbehind, proto 63); 0 = legacy RTT/2 + interp_lag estimate. /lagcomp toggles for live A/B
u32 g_NetServerInRate = 128 * 1024;
u32 g_NetServerOutRate = 128 * 1024;
u32 g_NetServerPort = NET_DEFAULT_PORT;
u16 g_NetServerActualPort = 0; // bound listen port; advertised to the master
s32 g_NetServerInfoQuery = true;

u32 g_NetClientUpdateRate = 1;
u32 g_NetClientInRate = 128 * 1024;
u32 g_NetClientOutRate = 128 * 1024;

u32 g_NetInterpTicks = 3;

// Live-tunable CSP / interp / snapshot knobs. Were #defines in net.h;
// promoted to globals so console commands (/cspframes, /cspcorr, /cspteleport,
// /stale) and config keys can adjust them at runtime. Defaults match the
// pre-promotion #define values so the hot-path behaviour is unchanged out
// of the box.
u32 g_NetCspCorrFramesMax     = 10;
f32 g_NetCspCorrThreshSq      = 625.f;    //  25 units squared
f32 g_NetCspTeleportThreshSq  = 14400.f;  // 120 units squared
u32 g_NetStaleSnapshotTicks   = 30;       // ~500ms at 60Hz
// Remote-player dead-reckoning: when the freshest snapshot is older than the
// interpolation target (late packet / jitter spike), extrapolate the remote's
// position from its last inter-snapshot velocity for up to this many ticks
// instead of freezing. 0 = no extrapolation (converge to the newest snapshot).
// Small by design: a missed direction-change overshoots, so keep it short.
u32 g_NetExtrapMaxTicks       = 3;
// Server-side CLC_HIT validation mode (0 off / 1 log-only / 2 enforce). Default
// off so behaviour is unchanged; flip to 1 to measure agreement between the
// server's authoritative lag-comp'd trace and clients' claimed hits before
// enabling enforcement. See netServerHitWasDetected / netServerRecordDetectedHit.
s32 g_NetHitValidate          = 0;
// Hidden test feature: centred hitmarker flash on a confirmed local hit. Off by
// default; toggled via /hitmarker. g_NetHitmarkerExpireTick is set to g_NetTick +
// NET_HITMARKER_TICKS when the local player's shot registers a chr/player hit.
s32 g_NetHitmarkerEnabled     = 0;
u32 g_NetHitmarkerExpireTick  = 0;

char g_NetLastJoinAddr[NET_MAX_ADDR + 1] = "127.0.0.1:27100";

u32 g_NetTick = 0;
u32 g_NetNextSyncId = 1;
// First syncid that belongs to a RUNTIME-spawned prop (everything below it is
// a static stage prop both sides allocate identically at load). Recorded by
// netSyncIdsAllocate; used by the JIP catch-up snapshot to replay only the
// dynamic props a mid-match joiner's fresh stage load cannot have.
u32 g_NetFirstDynamicSyncId = 1;

s32 g_NetSimPacketLoss = 0;
s32 g_NetSimLagMs = 0;
s32 g_NetDebugDraw = 0;

// Forward declarations for the lag-sim helpers below, which reference state
// (g_NetHost) declared further down the file.
static ENetHost *g_NetHost;

// --- Outgoing latency simulator ---
// When g_NetSimLagMs > 0, netSend creates the ENet packet immediately but
// holds it in this queue and delays the actual peer_send / host_broadcast
// until the requested delay has elapsed. Drained from netStartFrame each
// tick. Lets you reproduce high-ping CSP / interp / lag-comp behavior on a
// LAN test rig without needing an external network shaper.
#define NET_LAG_QUEUE_SIZE 512
struct net_lag_entry {
	u64 send_at_us;     // 0 means slot is free
	struct _ENetPeer *peer;       // NULL for broadcast
	s32 chan;
	struct _ENetPacket *packet;   // owns a reference until sent
};
static struct net_lag_entry g_NetLagQueue[NET_LAG_QUEUE_SIZE];
static s32 g_NetLagQueueDropped = 0;

static void netLagQueuePush(ENetPeer *peer, s32 chan, ENetPacket *p, u32 delay_ms)
{
	const u64 now_us = sysGetMicroseconds();
	for (s32 i = 0; i < NET_LAG_QUEUE_SIZE; ++i) {
		if (g_NetLagQueue[i].send_at_us == 0) {
			g_NetLagQueue[i].send_at_us = now_us + (u64)delay_ms * 1000ULL;
			g_NetLagQueue[i].peer = peer;
			g_NetLagQueue[i].chan = chan;
			g_NetLagQueue[i].packet = p;
			return;
		}
	}
	// Queue full — drop the packet rather than block or allocate more memory.
	// Caller doesn't retry; the ENet packet is owned by us at this point, so
	// destroying it here returns the buffer to ENet's internal pool. The
	// dropped count is exposed via g_NetLagQueueDropped for the F9 overlay.
	enet_packet_destroy(p);
	++g_NetLagQueueDropped;
}

static void netLagQueueDrain(void)
{
	if (!g_NetHost) {
		return;
	}
	const u64 now_us = sysGetMicroseconds();
	for (s32 i = 0; i < NET_LAG_QUEUE_SIZE; ++i) {
		struct net_lag_entry *e = &g_NetLagQueue[i];
		if (e->send_at_us == 0 || e->send_at_us > now_us) {
			continue;
		}
		if (e->peer) {
			enet_peer_send(e->peer, e->chan, e->packet);
		} else {
			enet_host_broadcast(g_NetHost, e->chan, e->packet);
		}
		e->send_at_us = 0;
		e->peer = NULL;
		e->packet = NULL;
	}
}

static void netLagQueueClear(void)
{
	for (s32 i = 0; i < NET_LAG_QUEUE_SIZE; ++i) {
		if (g_NetLagQueue[i].packet) {
			enet_packet_destroy(g_NetLagQueue[i].packet);
		}
	}
	memset(g_NetLagQueue, 0, sizeof(g_NetLagQueue));
}

// Drop queued packets targeting a specific peer that's about to be torn down.
// Called from the disconnect path so we don't try to send into a dead peer.
// Broadcast entries (peer==NULL) are left alone — they're safe regardless.
static void netLagQueueDropPeer(ENetPeer *peer)
{
	if (!peer) {
		return;
	}
	for (s32 i = 0; i < NET_LAG_QUEUE_SIZE; ++i) {
		if (g_NetLagQueue[i].packet && g_NetLagQueue[i].peer == peer) {
			enet_packet_destroy(g_NetLagQueue[i].packet);
			g_NetLagQueue[i].packet = NULL;
			g_NetLagQueue[i].peer = NULL;
			g_NetLagQueue[i].send_at_us = 0;
		}
	}
}

u64 g_NetRngSeeds[2];
u32 g_NetRngLatch = 0;
u64 g_NetMusicRngSeed = 0;

s32 g_NetMaxClients = NET_MAX_CLIENTS;
s32 g_NetNumClients = 0;
struct netclient g_NetClients[NET_MAX_CLIENTS + 1]; // last is an extra temporary client
struct netclient *g_NetLocalClient = &g_NetClients[NET_MAX_CLIENTS];

static u8 g_NetMsgBuf[NET_BUFSIZE];
struct netbuf g_NetMsg = { .data = g_NetMsgBuf, .size = sizeof(g_NetMsgBuf) };

static u8 g_NetMsgRelBuf[NET_BUFSIZE * 4]; // reliable buffer can be reliably fragmented
struct netbuf g_NetMsgRel = { .data = g_NetMsgRelBuf, .size = sizeof(g_NetMsgRelBuf) };

// Spectate target: when non-NULL, netSpectateApply rides the local camera on
// this chr each tick. /spec console commands set/clear it; netSpectateCycle
// walks the live players-then-sims list. Cleared automatically by
// netSpectateApply if the chr disappears (round ends, sim removed) so callers
// don't have to bookkeep it.
struct chrdata *g_NetSpectateChr = NULL;

static s32 g_NetInit = false;
// g_NetHost is forward-declared near the top of the file because the lag-sim
// helpers reference it.
static ENetAddress g_NetLocalAddr;
static ENetAddress g_NetRemoteAddr;

static u32 g_NetNextUpdate = 0;

static u32 g_NetReliableFrameLen = 0;
static u32 g_NetUnreliableFrameLen = 0;

// /netstats — server-side per-message-type tx accounting. Bytes are counted as
// they are written into the broadcast buffers (so the value is bytes GENERATED;
// multiply by client count for actual wire bytes). Snapshotted once per ~second.
enum { NETSTAT_PLAYERMOVE, NETSTAT_PROPMOVE, NETSTAT_PLAYERSTATS, NETSTAT_COUNT };
static u32 g_NetStatAccum[NETSTAT_COUNT];
static u32 g_NetStatPerSec[NETSTAT_COUNT];
static u32 g_NetStatSecBase = 0;
static inline void netStatAdd(s32 type, u32 bytes) { g_NetStatAccum[type] += bytes; }

// Client-side prediction globals
struct csp_snapshot g_NetCspHistory[NET_CSP_HISTORY_SIZE];
u32 g_NetCspHead = 0;
struct coord g_NetCspCorrDelta;
s32 g_NetCspCorrFrames = 0;

// --- Diagnostic logging ---
// Writes per-event CSV lines to a file when Net.Debug.LogPath is set in
// pd.ini. Designed to be greppable: `tick,realtime_s,event,key=val,...`.
// Open the resulting file in a text editor or `tail -f` it during play to
// see what's happening in real time. Per-tick position dumps are gated by
// Net.Debug.LogRate (default 6 ticks ≈ 10 Hz) to keep file size sane.
static FILE *g_NetDiagFile = NULL;
static u64 g_NetDiagStartUs = 0;
char g_NetDiagPath[256] = "";
u32 g_NetDiagDumpRate = 6;

static void netDiagClose(void)
{
	if (g_NetDiagFile) {
		fclose(g_NetDiagFile);
		g_NetDiagFile = NULL;
	}
}

static void netDiagOpen(void)
{
	netDiagClose();
	if (!g_NetDiagPath[0]) {
		return;
	}
	g_NetDiagFile = fopen(g_NetDiagPath, "w");
	if (!g_NetDiagFile) {
		sysLogPrintf(LOG_WARNING, "NET: could not open diag log '%s'", g_NetDiagPath);
		return;
	}
	g_NetDiagStartUs = sysGetMicroseconds();
	fprintf(g_NetDiagFile, "# tick,realtime_s,event,fields...\n");
	fflush(g_NetDiagFile);
	sysLogPrintf(LOG_NOTE, "NET: diag log -> %s", g_NetDiagPath);
}

// Non-static so netmsg.c (and other TUs that need diag tracing) can call it
// without each file open-coding the same fprintf / fflush boilerplate. The
// declaration lives in net.h.
void netDiagLogf(const char *event, const char *fmt, ...)
{
	if (!g_NetDiagFile) {
		return;
	}
	const f32 rt = (f32)(sysGetMicroseconds() - g_NetDiagStartUs) / 1000000.f;
	fprintf(g_NetDiagFile, "%u,%.3f,%s,", g_NetTick, rt, event);
	va_list ap;
	va_start(ap, fmt);
	vfprintf(g_NetDiagFile, fmt, ap);
	va_end(ap);
	fputc('\n', g_NetDiagFile);
	// Flush every line EXCEPT the high-rate per-tick position dumps (pos_cl /
	// pos_sim, ~10Hz per client/sim at the default LogRate). Event/bracket lines
	// (server_start, stage_start, csp_recon, lagcomp, tick, disconnect,
	// proptick_guard, ...) still flush immediately so a crash never loses the
	// lines that bracket it; the redundant position sampling rides the stdio
	// buffer and reaches disk on the next event line's flush. The position trail
	// is therefore only ever incomplete for the sub-100ms window since the last
	// event — where the event lines already pinpoint the crash — in exchange for
	// far fewer synchronous fflushes on the game thread under busy dumps.
	const bool isposdump = (strcmp(event, "pos_cl") == 0 || strcmp(event, "pos_sim") == 0);
	if (!isposdump) {
		fflush(g_NetDiagFile);
	}
}

// Lag compensation: saved client state for restore after hit rewind.
// We translate prop->pos and the root bone matrix only. A broader translation
// across the whole chr->model->matrices array was attempted but reverted: the
// matrices pointer is allocated each frame from the per-frame graphics heap
// (gfxAllocate) and may point to stale or already-reused memory by the time
// shotCalculateHits runs, so writing past matrix[0] risks corrupting whatever
// the heap has handed out since. Narrow-phase hits therefore still test
// against bones at the current world pose, just like before.
static struct {
	struct netclient *cl;
	struct coord pos;
	f32 rootmtx_xyz[3];
	s32 has_rootmtx;
} g_LagCompSaved[NET_MAX_CLIENTS];
static s32 g_LagCompCount = 0;
// Debug-overlay diagnostics: snapshots of the most-recent lag-comp event so
// the F9 panel can report what was rewound on the last shot. Not used by the
// hit-test code itself.
static s32 g_LagCompLastCount = 0;
static u32 g_LagCompLastRewindTicks = 0;

// Forward declaration: the kill-feed buffer and its clear helper live below,
// near the render code, but netDisconnect needs to wipe the feed on session
// teardown — declare it here so the dispatch order doesn't break.
static void netKillFeedClear(void);

s32 netParseAddr(ENetAddress *out, const char *str)
{
	char tmp[256] = { 0 };

	if (!str || !str[0]) {
		return false;
	}

	strncpy(tmp, str, sizeof(tmp) - 1);

	char *host = tmp;
	char *port = NULL;
	char *colon = strrchr(tmp, ':');

	// if there is a : in the address string, there could be a port value
	// otherwise it's an ip or hostname with default port
	if (colon > tmp) {
		if (tmp[0] == '[' && colon[-1] == ']' && isdigit(colon[1])) {
			// ipv6 with port: [ADDR]:PORT
			colon[-1] = '\0'; // terminate ip
			host = tmp + 1; // skip [
			port = colon + 1; // skip :
		} else if (isdigit(colon[1]) && strchr(host, ':') == colon) {
			// ipv4 or hostname with port
			colon[0] = '\0'; // terminate ip
			port = colon + 1; // skip :
		}
	}

	if (!host[0]) {
		return false;
	}

	const s32 portval = port ? atoi(port) : NET_DEFAULT_PORT;
	if (portval < 0 || portval > 0xFFFF) {
		return false;
	}

	memset(out, 0, sizeof(*out));
	out->port = portval;

	if (isdigit(host[0]) || strchr(host, ':')) {
		// we stripped off the :PORT at this point, now check if this is an IP address
		if (enet_address_set_ip(out, host) == 0) {
			return true;
		}
	}

	// must be a domain name; do a lookup
	return (enet_address_set_hostname(out, host) == 0);
}

static const char *netFormatAddr(const ENetAddress *addr)
{
	static char str[256];
	char tmp[256];
	if (addr && enet_address_get_ip(addr, tmp, sizeof(tmp) - 1) == 0) {
		if (tmp[0]) {
			if (strchr(tmp, ':')) {
				// ipv6
				snprintf(str, sizeof(str) - 1, "[%s]:%u", tmp, addr->port);
			} else {
				// ipv4
				snprintf(str, sizeof(str) - 1, "%s:%u", tmp, addr->port);
			}
			return str;
		}
	}
	return NULL;
}

static inline const char *netFormatPeerAddr(const ENetPeer *peer)
{
	return netFormatAddr(&peer->address);
}

const char *netFormatClientAddr(const struct netclient *cl)
{
	return cl->peer ? netFormatPeerAddr(cl->peer) : "<local>";
}

static inline void netClientReset(struct netclient *cl)
{
	if (cl->state >= CLSTATE_GAME && cl->player) {
		cl->player->client = NULL;
		cl->player->isremote = false;
	}
	memset(cl, 0, sizeof(*cl));
	cl->out.data = cl->out_data;
	cl->out.size = sizeof(cl->out_data);
	cl->id = cl - g_NetClients;
	cl->settings.team = 0xff;
}

static inline void netClientResetAll(void)
{
	g_NetMaxClients = NET_MAX_CLIENTS;
	g_NetNumClients = 1; // always at least one client, which is us
	for (u32 i = 0; i < NET_MAX_CLIENTS + 1; ++i) {
		netClientReset(&g_NetClients[i]);
	}
}

static inline void netClientRecordMove(struct netclient *cl, const struct player *pl)
{
	// make space in the move stack
	memmove(cl->outmove + 1, cl->outmove, sizeof(cl->outmove) - sizeof(*cl->outmove));

	struct netplayermove *move = &cl->outmove[0];

	// Zero the slot before populating fields individually. netClientNeedMove
	// change-detects with a memcmp over the whole struct (minus tick + the anim
	// tail); netplayermove has padding (e.g. after the s8 weaponnum, before the
	// coord pos) that the field-by-field writes below never touch. Leaving it as
	// recycled stack bytes makes that memcmp depend on stale padding — a latent
	// spurious-send / missed-send the moment a field is reordered. memset makes
	// the padding deterministic so the comparison only reflects real fields.
	memset(move, 0, sizeof(*move));

	move->tick = g_NetTick;
	move->crouchofs = pl->crouchoffset;
	move->leanofs = pl->swaytarget / 75.f;
	move->movespeed[0] = pl->speedforwards;
	move->movespeed[1] = pl->speedsideways;
	move->angles[0] = pl->vv_theta;
	move->angles[1] = pl->vv_verta;
  move->pos = (pl->prop) ? pl->prop->pos : pl->cam_pos;

	move->crosspos[0] = pl->crosspos[0];
	move->crosspos[1] = pl->crosspos[1];

	if (!pl->isremote) {
		// normalize crosspos x
		move->crosspos[0] -= (f32)(SCREEN_WIDTH_LO / 2);
		move->crosspos[0] = (f32)(SCREEN_WIDTH_LO / 2) + move->crosspos[0] * pl->aspect / SCREEN_ASPECT;
	}

	move->ucmd = pl->ucmd;

	// Capture chr model animation state so remote viewers can keep
	// non-input-driven anims (hit reactions, pickups, special transitions)
	// in sync. Pure walk/run anims would converge from synced inputs alone,
	// but anything event-triggered by the server can otherwise diverge.
	move->animnum = 0;
	move->animframe = 0;
	if (pl->prop && pl->prop->chr && pl->prop->chr->model && pl->prop->chr->model->anim) {
		move->animnum = pl->prop->chr->model->anim->animnum;
		move->animframe = pl->prop->chr->model->anim->framea;
	}

	// Render offset (proto 63): how many ticks behind its own net clock this
	// machine renders other entities (= g_NetInterpTicks, the interpolators'
	// render-behind). Travels with the move so the server's lag-comp can rewind
	// targets to the exact server-tick the shooter was displaying when it fired
	// (inmovetick - renderbehind), instead of an RTT/2 + interp_lag estimate.
	move->renderbehind = (u8)((g_NetInterpTicks > 255u) ? 255u : g_NetInterpTicks);

	const struct netplayermove *inmove_newest = &cl->inmove[cl->inmove_head];
	if (g_NetMode == NETMODE_SERVER && pl->isremote && inmove_newest->tick) {
		// carry some of the client inputs over to the outmove
		move->ucmd |= (inmove_newest->ucmd & (UCMD_FIRE | UCMD_RELOAD | UCMD_AIMMODE | UCMD_EYESSHUT | UCMD_SELECT | UCMD_SELECT_DUAL));
		move->crosspos[0] = inmove_newest->crosspos[0];
		move->crosspos[1] = inmove_newest->crosspos[1];
	}

	if (pl->crouchpos == CROUCHPOS_DUCK) {
		move->ucmd |= UCMD_DUCK;
	} else if (pl->crouchpos == CROUCHPOS_SQUAT) {
		move->ucmd |= UCMD_SQUAT;
	}

	if (pl->gunctrl.switchtoweaponnum >= 0 && !pl->gunctrl.throwing) {
		move->ucmd |= UCMD_SELECT;
		move->weaponnum = pl->gunctrl.switchtoweaponnum;
	} else {
		move->weaponnum = -1;
	}

	if (pl->gunctrl.dualwielding && !pl->gunctrl.throwing) {
		move->ucmd |= UCMD_SELECT_DUAL;
		if ((move->ucmd ^ cl->outmove[1].ucmd) & UCMD_SELECT_DUAL) {
			move->ucmd |= UCMD_SELECT;
		}
	}

	const s32 oldnum = g_Vars.currentplayernum;
	setCurrentPlayerNum(cl->playernum);

	if (bgunIsUsingSecondaryFunction()) {
		move->ucmd |= UCMD_SECONDARY;
	}

	if (pl->insightaimmode) {
		move->ucmd |= UCMD_AIMMODE;
		move->zoomfov = currentPlayerGetGunZoomFov();
	} else {
		move->zoomfov = 0.f;
	}

	setCurrentPlayerNum(oldnum);

	if (cl != g_NetLocalClient && !cl->forcetick && (move->ucmd & UCMD_FL_FORCEMASK)) {
		cl->forcetick = move->tick;
		sysLogPrintf(LOG_NOTE, "NET: forcing client %u to move at tick %u", cl->id, cl->forcetick);
	}

	// CSP: save the local player's predicted position each tick so we can
	// measure prediction error when the server's authoritative state arrives.
	if (cl == g_NetLocalClient && g_NetMode == NETMODE_CLIENT) {
		g_NetCspHead = (g_NetCspHead + 1) % NET_CSP_HISTORY_SIZE;
		g_NetCspHistory[g_NetCspHead].tick = move->tick;
		g_NetCspHistory[g_NetCspHead].pos = move->pos;
	}

	// Lag comp: snapshot every player's world position on the server so shots
	// can be rewound to what the shooter saw. This includes the host (local
	// client) — without it the host's lagcomp buffer stays zeroed and remote
	// bullets move the host's hitbox to world origin, making them unkillable.
	if (g_NetMode == NETMODE_SERVER && cl->player && cl->player->prop) {
		netLagCompSave(cl);
	}
}

static inline s32 netClientNeedReliableMove(const struct netclient *cl)
{
	const struct netplayermove *move = &cl->outmove[0];
	const struct netplayermove *moveprev = &cl->outmove[1];
	return !moveprev->tick || (g_NetMode == NETMODE_SERVER && cl->forcetick) ||
		(moveprev->ucmd & UCMD_IMPORTANT_MASK) != (move->ucmd & UCMD_IMPORTANT_MASK) ||
		(move->ucmd & UCMD_ACTIVATE);
}

static inline s32 netClientNeedMove(const struct netclient *cl)
{
	if (g_NetTick < g_NetNextUpdate) {
		return false;
	}
	const struct netplayermove *move = &cl->outmove[0];
	const struct netplayermove *moveprev = &cl->outmove[1];
	if (move->tick && cl->outmoveack >= move->tick) {
		return false;
	}
	// Exclude the trailing anim fields from the change detection: animframe
	// usually ticks every frame, which would otherwise force a send on every
	// tick and undo the update-rate gating above. The anim fields piggyback
	// on whatever sends we do make for genuine input/position changes, which
	// is sufficient for keeping remote chr animations in rough sync.
	// Compare only the fields between tick and the anim tail (animnum, animframe,
	// and the proto-63 renderbehind that follows them). animframe ticks every
	// frame and renderbehind is ~constant, so including either would force a send
	// every tick and undo the update-rate gating. Bound the compare at the address
	// of animnum so all trailing fields + padding are excluded regardless of
	// layout (don't subtract fixed field sizes — that breaks when a field is
	// appended after animframe, as renderbehind was).
	const u8 *base = (const u8 *)move;
	const u8 *cmpa = base + sizeof(move->tick);
	const u8 *cmpb = (const u8 *)moveprev + sizeof(move->tick);
	const size_t cmplen = (size_t)((const u8 *)&move->animnum - base) - sizeof(move->tick);
	return (memcmp(cmpa, cmpb, cmplen) != 0);
}

static inline void netClientReadConfig(struct netclient *cl, const s32 playernum)
{
	cl->settings.options = g_PlayerConfigsArray[playernum].options;
	cl->settings.bodynum = g_PlayerConfigsArray[playernum].base.mpbodynum;
	cl->settings.headnum = g_PlayerConfigsArray[playernum].base.mpheadnum;
	cl->settings.fovy = g_PlayerExtCfg[playernum].fovy;
	cl->settings.fovzoommult = g_PlayerExtCfg[playernum].fovzoommult;
	memcpy(cl->settings.name, g_PlayerConfigsArray[playernum].base.name, sizeof(cl->settings.name));
	// the \n will be readded in the playerconfig
	char *newline = strrchr(g_NetLocalClient->settings.name, '\n');
	if (newline) {
		*newline = '\0';
	}
}

static inline void netFlushSendBuffers(void)
{
	if (g_NetMsgRel.wp) {
		if (g_NetMsgRel.error) {
			sysLogPrintf(LOG_WARNING, "NET: reliable out buffer overflow");
		}
		g_NetReliableFrameLen += g_NetMsgRel.wp;
		netSend(NULL, &g_NetMsgRel, true, NETCHAN_DEFAULT);
	}

	if (g_NetMsg.wp) {
		if (g_NetMsg.error) {
			sysLogPrintf(LOG_WARNING, "NET: unreliable out buffer overflow");
		}
		g_NetUnreliableFrameLen += g_NetMsg.wp;
		netSend(NULL, &g_NetMsg, false, NETCHAN_DEFAULT);
	}
}

static inline const char *netGetDisconnectReason(const u32 reason)
{
	static const char *msgs[] = {
		"Unknown",
		"Server is shutting down",
		"Protocol or version mismatch",
		"Kicked by console",
		"You are banned on this server",
		"Connection timed out",
		"Server is full",
		"The game is already in progress",
		"Your files differ from the server's",
		"Incorrect password"
	};
	if (reason < (u32)ARRAYCOUNT(msgs)) {
		return msgs[reason];
	}
	return msgs[0];
}

// Extended server query response. querytype selects NET_QUERYTYPE_SUMMARY (the
// browser-list row) or NET_QUERYTYPE_DETAILS (summary + live scoreboard). The
// summary block is the shared netmsgQuerySummaryWrite payload so the in-game
// browser and the master server decode identical bytes. Larger static buffer
// than the legacy response since details can carry up to 8 players + 8 sims.
// Connectionless server queries (PDQM) are answered to an UNVERIFIED, spoofable
// UDP source with a response many times larger than the 5-6 byte request — a
// classic reflection/amplification primitive, and these servers are publicly
// advertised to the master browser. Throttle responses per source IP (and a
// global per-tick backstop) so a spoofed-victim flood can't be amplified through
// us. A legitimate browser queries each server only a couple of times (summary +
// details), so a few per second per source is ample headroom.
#define NET_QUERY_LRU             16
#define NET_QUERY_MIN_TICKS       15u // summary: ~4 responses/sec/source
#define NET_QUERY_DETAILS_TICKS   60u // details (largest payload): ~1/sec/source
#define NET_QUERY_GLOBAL_PER_TICK 8u  // hard cap on responses emitted per tick

static s32 netQueryRateAllowed(const ENetAddress *address, u8 querytype)
{
	static struct { struct in6_addr host; u32 tick; u8 used; } lru[NET_QUERY_LRU];
	static u32 next = 0;
	static u32 gtick = 0, gcount = 0;
	const u32 now = g_NetTick;
	const u32 mininterval = (querytype == NET_QUERYTYPE_DETAILS)
			? NET_QUERY_DETAILS_TICKS : NET_QUERY_MIN_TICKS;
	s32 known = 0;

	for (s32 i = 0; i < NET_QUERY_LRU; i++) {
		if (lru[i].used && !memcmp(&lru[i].host, &address->ipv6, sizeof(lru[i].host))) {
			// (now - tick) via u32 wraparound is safe on the free-running tick clock.
			if ((u32)(now - lru[i].tick) < mininterval) {
				return 0; // per-source throttle
			}
			lru[i].tick = now;
			known = 1;
			break;
		}
	}
	if (!known) {
		lru[next].host = address->ipv6;
		lru[next].tick = now;
		lru[next].used = 1;
		next = (next + 1) % NET_QUERY_LRU;
	}

	// Global backstop: bound total responses per tick so a flood spread across
	// many spoofed source IPs — which each slip the per-source LRU once — still
	// can't turn us into a high-rate reflector.
	if (gtick != now) { gtick = now; gcount = 0; }
	if (gcount >= NET_QUERY_GLOBAL_PER_TICK) {
		return 0;
	}
	gcount++;
	return 1;
}

static void netServerQueryResponse(ENetAddress *address, u8 querytype)
{
	static u8 data[1024];
	static ENetBuffer ebuf;
	struct netbuf buf = { .data = data, .size = sizeof(data) };

	netbufStartWrite(&buf);
	netbufWriteData(&buf, NET_QUERY_MAGIC, sizeof(NET_QUERY_MAGIC) - 1);
	netbufWriteU16(&buf, 0); // space for size

	netmsgQuerySummaryWrite(&buf);
	if (querytype == NET_QUERYTYPE_DETAILS) {
		netmsgQueryDetailsWrite(&buf);
	}

	netbufWriteU16(&buf, 0); // space for checksum

	ebuf.data = buf.data;
	ebuf.dataLength = buf.wp;

	// rewrite size
	buf.wp = sizeof(NET_QUERY_MAGIC) - 1;
	netbufWriteU16(&buf, ebuf.dataLength);

	// calculate and rewrite checksum
	buf.wp = ebuf.dataLength - sizeof(u16);
	u16 crc = 0xFFFF;
	u16 x;
	for (u32 i = 0; i < buf.wp; ++i) {
		x = crc >> 8 ^ buf.data[i];
		x ^= x >> 4;
		crc += (crc << 8) ^ (x << 12) ^ (x << 5) ^ x;
	}
	netbufWriteU16(&buf, crc);

	enet_socket_send(g_NetHost->socket, address, &ebuf, 1);
}

// Send a raw connectionless datagram out of the server's ENet socket. Used by
// the master-server heartbeat (netmaster.c) so the packet's source ip:port is
// the same address clients connect to (NAT-friendly). No-op if the host is down.
void netSendConnectionless(const ENetAddress *addr, const void *data, u32 len)
{
	if (!g_NetHost || !addr || !data || !len) {
		return;
	}
	ENetBuffer ebuf;
	ebuf.data = (void *)data;
	ebuf.dataLength = len;
	enet_socket_send(g_NetHost->socket, addr, &ebuf, 1);
}

static s32 netServerConnectionlessPacket(ENetEvent *event, ENetAddress *address, u8 *rxdata, s32 rxlen)
{
	if (rxdata && rxlen >= 5) {
		if (!memcmp(rxdata, NET_QUERY_MAGIC, sizeof(NET_QUERY_MAGIC) - 1)) {
			// direct server query; optional trailing byte selects summary/details
			const u8 querytype = (rxlen >= 6) ? rxdata[5] : NET_QUERYTYPE_SUMMARY;
			// Drop (silently consume) if this source / the server as a whole is over
			// the reflection-amplification rate limit. Still return 1 so the packet
			// isn't passed to ENet as a connection attempt.
			if (!netQueryRateAllowed(address, querytype)) {
				return 1;
			}
			sysLogPrintf(LOG_NOTE | LOGFLAG_NOCON, "NET: query request from %s, responding", netFormatAddr(address));
			netServerQueryResponse(address, querytype);
			return 1;
		}
#ifndef PLATFORM_N64
		if (rxlen >= (s32)(sizeof(NET_MASTER_MAGIC) - 1) &&
				!memcmp(rxdata, NET_MASTER_MAGIC, sizeof(NET_MASTER_MAGIC) - 1)) {
			// reply from the master server (e.g. REGISTER_ACK)
			netMasterHandlePacket(rxdata, rxlen);
			return 1;
		}
#endif
	}
	// probably a normal packet, pass through to enet
	return 0;
}

struct netclient *netClientForPlayerNum(s32 playernum)
{
	s32 slot = 0;
	for (s32 i = 0; i < g_NetMaxClients; ++i) {
		struct netclient *cl = &g_NetClients[i];
		if (cl->state >= CLSTATE_LOBBY) {
			if (slot == playernum) {
				return cl;
			}
			++slot;
		}
	}
	return NULL;
}

static void netApplyEggConfig(void);

void netInit(void)
{
	// Auto-enable a vanity egg banner from the "Egg" ini key (config is already
	// loaded by now). Done before the ENet check so it works even if net init fails.
	netApplyEggConfig();

	if (enet_initialize() < 0) {
		sysLogPrintf(LOG_ERROR, "NET: could not init ENet, disabling networking");
		return;
	}

	const s32 argmaxclients = sysArgGetInt("--maxclients", -1);
	if (argmaxclients > 0 && argmaxclients <= NET_MAX_CLIENTS) {
		g_NetMaxClients = argmaxclients;
	}

	const s32 argport = sysArgGetInt("--port", -1);
	if (argport > 0 && argport < 0x10000) {
		g_NetServerPort = argport;
	}

	const char *argjoin = sysArgGetString("--connect");
	if (argjoin) {
		strncpy(g_NetLastJoinAddr, argjoin, sizeof(g_NetLastJoinAddr) - 1);
		g_NetJoinLatch = true;
	}

	if (sysArgCheck("--host")) {
		g_NetHostLatch = true;
	}

	// --dedicated: true headless. videoInit/audioInit will no-op, mainTick
	// skips the render path. Implies --host (auto-starts server on boot).
	// --dedicated-windowed: same server-mode, but keeps the SDL window for a
	// status overlay — useful for beginners who want to see what's going on.
	if (sysArgCheck("--dedicated")) {
		g_NetDedicatedLatch = 1;
		g_NetHostLatch = true;
	} else if (sysArgCheck("--dedicated-windowed")) {
		g_NetDedicatedLatch = 2;
		g_NetHostLatch = true;
	}

	const char *argplaylist = sysArgGetString("--playlist");
	if (argplaylist && argplaylist[0]) {
		strncpy(g_NetPlaylistPath, argplaylist, sizeof(g_NetPlaylistPath) - 1);
		g_NetPlaylistPath[sizeof(g_NetPlaylistPath) - 1] = '\0';
	}

	const char *argname = sysArgGetString("--server-name");
	if (argname && argname[0]) {
		strncpy(g_NetServerName, argname, sizeof(g_NetServerName) - 1);
		g_NetServerName[sizeof(g_NetServerName) - 1] = '\0';
	}

	const char *argmaster = sysArgGetString("--master");
	if (argmaster && argmaster[0]) {
		strncpy(g_NetMasterAddr, argmaster, sizeof(g_NetMasterAddr) - 1);
		g_NetMasterAddr[sizeof(g_NetMasterAddr) - 1] = '\0';
	}

	if (sysArgCheck("--no-advertise")) {
		g_NetMasterAdvertise = 0;
	}

	const char *argpassword = sysArgGetString("--password");
	if (argpassword) {
		strncpy(g_NetServerPassword, argpassword, sizeof(g_NetServerPassword) - 1);
		g_NetServerPassword[sizeof(g_NetServerPassword) - 1] = '\0';
	}

	const char *argadminpassword = sysArgGetString("--admin-password");
	if (argadminpassword) {
		strncpy(g_NetAdminPassword, argadminpassword, sizeof(g_NetAdminPassword) - 1);
		g_NetAdminPassword[sizeof(g_NetAdminPassword) - 1] = '\0';
	}

	// --netdiag <path> (alias --diag): diagnostic CSV log path. Same target as
	// the Net.Debug.LogPath config key, but as a CLI flag for dedicated servers
	// (which have no console for /diag). Overrides the config value if both are
	// set; the file is opened by netDiagOpen at netStartServer / netStartClient.
	// NOTE: this is the NETPLAY diagnostic log — the system/boot log is a
	// separate file (pd.log) enabled with --log.
	const char *argnetdiag = sysArgGetString("--netdiag");
	if (!argnetdiag || !argnetdiag[0]) {
		argnetdiag = sysArgGetString("--diag");
	}
	if (argnetdiag && argnetdiag[0]) {
		strncpy(g_NetDiagPath, argnetdiag, sizeof(g_NetDiagPath) - 1);
		g_NetDiagPath[sizeof(g_NetDiagPath) - 1] = '\0';
	}

	// Initialise playlist to empty defaults; an actual load (which logs if
	// the file is missing) only runs when we're going to be a server.
	playlistFree(&g_NetPlaylist);
	if (g_NetDedicatedLatch || g_NetHostLatch) {
		playlistLoad(&g_NetPlaylist, g_NetPlaylistPath);
		// If the playlist named a server, prefer it over the --server-name
		// default; the CLI flag still wins because g_NetServerName was
		// updated above with strncpy if --server-name was provided.
		if (g_NetPlaylist.server_name[0] && strcmp(g_NetServerName, "Perfect Dark Dedicated") == 0) {
			strncpy(g_NetServerName, g_NetPlaylist.server_name, sizeof(g_NetServerName) - 1);
		}
	}

	g_NetInit = true;
}

s32 netStartServer(u16 port, s32 maxclients)
{
	if (g_NetMode || !g_NetInit) {
		return -1;
	}

	memset(&g_NetLocalAddr, 0, sizeof(g_NetLocalAddr));
	g_NetLocalAddr.port = port;
	g_NetHost = enet_host_create(&g_NetLocalAddr, maxclients, NETCHAN_COUNT, g_NetServerInRate, g_NetServerOutRate, 0);
	if (!g_NetHost) {
		sysLogPrintf(LOG_ERROR, "NET: could not create ENet host");
		return -2;
	}

	g_NetServerActualPort = port;

	if (g_NetServerInfoQuery) {
		enet_host_set_intercept_callback(g_NetHost, netServerConnectionlessPacket);
	}

	netClientResetAll();
	g_NetMaxClients = maxclients;

	// the server's local client is client 0
	g_NetLocalClient = &g_NetClients[0];
	g_NetLocalClient->state = CLSTATE_LOBBY; // local client doesn't need auth
	netClientReadConfig(g_NetLocalClient, 0);
	g_NetLocalProfileBackup = g_PlayerConfigsArray[0];

	// Dedicated server: the host doesn't participate as a combatant. Force
	// is_spectator=1 so netPlayersAllocate skips slot 0 for the host
	// (combatants take 0..N-1), and force panel count to 0 — dedicated has
	// no local viewports. spectatorAllocatePanels respects the 0 (won't
	// clamp to 1) so no phantom player chr/prop spawns in the world.
	if (g_NetDedicatedMode) {
		g_NetLocalClient->is_spectator = 1;
		g_SpectatorPanelCount = 0;
	}

	g_NetMode = NETMODE_SERVER;

	g_NetTick = 0;
	g_NetNextUpdate = 0;
	g_NetNextSyncId = 1;

	sysLogPrintf(LOG_NOTE, "NET: using protocol version %d", NET_PROTOCOL_VER);
	sysLogPrintf(LOG_NOTE, "NET: created server on port %u", port);

	netDiagOpen();
	netDiagLogf("server_start", "port=%u maxclients=%d protocol=%d", port, maxclients, NET_PROTOCOL_VER);

	return 0;
}

// Campaign co-op (Phase 0): enter a solo stage in 2-player co-op. Mirrors the
// solo/co-op start in mainmenu.c (titleSetNextStage -> coop player slots ->
// setNumPlayers -> difficulty -> mainChangeToStage). Called on the HOST from the
// /coop command and on the CLIENT from netmsgSvcStageStartRead's co-op branch, so
// both ends reach the same stage with the same co-op player model. The generic
// stage-load hooks then fire: lv.c's netServerStageStart broadcasts the host's
// SVC_STAGE_START, and playermgr's netPlayersAllocate seats remote clients.
//
// `numplayers` is the total co-op player count N (host + remote partners), up to
// MAX_PLAYERS. The host derives it from g_NetNumClients (which already counts the
// host as g_NetClients[0]); the client mirrors it from the SVC_STAGE_START co-op
// manifest count. coopplayernum stays 1 (the single splitscreen-buddy pointer /
// co-op gate); the N players live in g_Vars.players[0..N-1].
void netCoopEnterStage(s32 stagenum, s32 difficulty, s32 numplayers)
{
	if (numplayers < 1) {
		numplayers = 1;
	}
	if (numplayers > MAX_PLAYERS) {
		numplayers = MAX_PLAYERS;
	}

	// F2 body type is per-player: each player's choice (g_NetCoopBodyMode) rides
	// CLC_SETTINGS to the host, which assembles the resolved per-player bitmask
	// (g_NetCoopBodyBits) in the SVC_STAGE_START write. The client applied that
	// wire value in netmsgSvcStageStartRead before calling this, so nothing to do
	// here.

	// F3 lives: the HOST seeds the respawn budget. PER_PLAYER gives each player
	// `count` lives; SHARED gives one pool of `count * N`. The client gets the
	// mode + count from SVC_STAGE_START (read before this call) and follows the
	// host's authoritative respawn / all-out decisions, so it doesn't seed here.
	if (g_NetMode != NETMODE_CLIENT) {
		for (s32 i = 0; i < MAX_PLAYERS; i++) {
			g_NetCoopLives[i] = g_NetCoopLivesCount;
		}
		g_NetCoopSharedLives = g_NetCoopLivesCount * numplayers;
	}

	// Clear the host-authoritative objective mirror so a previous mission's
	// completions can't leak into this one (the client overlays these onto its
	// local objective evaluation; a stale COMPLETE would falsely mark an objective
	// done before the host's first SVC_OBJECTIVE for the new stage arrives).
	// Explicit size: only the incomplete `extern u32[]` from net.h is in scope here
	// (the sized definition is later in this file), so sizeof(array) won't compile.
	memset(g_NetCoopObjStatuses, 0, sizeof(u32) * MAX_OBJECTIVES);
	memset(g_NetCoopClientObjDone, 0, sizeof(u8) * MAX_OBJECTIVES); // host: clear client-reported completions
	memset(g_NetCoopObjToastShown, 0, sizeof(u8) * MAX_OBJECTIVES); // clear per-objective completion-toast latches
	g_NetLastStageFlags = 0; // re-broadcast flags from scratch for the new stage
	g_NetCoopLocalStageFlags = 0; // client: clear locally-set stage flags for the new stage
	g_NetLastCutsceneActive = 0;
	g_NetLastCutsceneAnim = 0;

	g_MissionConfig.iscoop = 1;
	g_MissionConfig.isanti = 0;
	g_MissionConfig.pdmode = 0;
	g_MissionConfig.difficulty = difficulty;
	g_MissionConfig.stagenum = stagenum;
	g_Vars.numaibuddies = 0;

	titleSetNextStage(stagenum);
	g_Vars.bondplayernum = 0;
	g_Vars.coopplayernum = 1;
	g_Vars.antiplayernum = -1;
	// Drop-in: net co-op always allocates the full slot budget so every
	// machine (including a future mid-mission joiner's fresh load) produces
	// the identical player/prop/syncid layout. Slots beyond the connected
	// players start dormant (netCoopDormantInit) and are claimed via
	// SVC_COOP_CLAIM. numplayers (the connected count) still sized the
	// shared lives pool above. Non-net (splitscreen) co-op is unchanged.
	setNumPlayers(g_NetMode ? NET_COOP_MAX_SLOTS : numplayers);
	lvSetDifficulty(difficulty);
	titleSetNextMode(TITLEMODE_SKIP);
	mainChangeToStage(stagenum);
}

void netServerStageStart(void)
{
	if (g_NetMode != NETMODE_SERVER) {
		return;
	}

	if (g_StageNum == STAGE_TITLE || g_StageNum == STAGE_CITRAINING) {
		g_NetLocalClient->state = CLSTATE_LOBBY;
		return;
	}

	// re-read the player config in case it changed
	netClientReadConfig(g_NetLocalClient, 0);

	g_NetLocalClient->state = CLSTATE_GAME;

	netbufStartWrite(&g_NetMsgRel);
	netmsgSvcStageStartWrite(&g_NetMsgRel);
	netSend(NULL, &g_NetMsgRel, true, NETCHAN_DEFAULT);

	netDiagLogf("stage_start", "stage=%u clients=%d sims=%d", g_StageNum, g_NetNumClients, g_BotCount);
}

void netServerSendJipSnapshot(struct netclient *cl)
{
	// One packet's worth of buffer; flushed whenever the next message might
	// not fit (largest spawn message — powered projectile — is ~250 bytes).
	static u8 snapdata[NET_BUFSIZE];
	struct netbuf buf = { 0 };
	s32 nprops = 0;
	s32 ndoors = 0;
	s32 nlifts = 0;

	if (g_NetMode != NETMODE_SERVER || !cl) {
		return;
	}

	if (g_StageNum == STAGE_TITLE || g_StageNum == STAGE_CITRAINING) {
		return;
	}

	buf.data = snapdata;
	buf.size = sizeof(snapdata);
	netbufStartWrite(&buf);

	for (s32 i = 0; i < g_Vars.maxprops; ++i) {
		struct prop *prop = &g_Vars.props[i];

		if (!prop->syncid) {
			continue;
		}

		if (buf.wp >= NET_BUFSIZE - 320) {
			netSend(cl, &buf, true, NETCHAN_DEFAULT); // resets buf
		}

		if (prop->syncid >= g_NetFirstDynamicSyncId && prop->obj
				&& (prop->type == PROPTYPE_WEAPON || prop->type == PROPTYPE_OBJ)) {
			// Runtime-spawned weapon/obj the joiner's fresh stage load lacks.
			// Skip a deployed autogun whose owner is gone — the spawn write
			// attributes it via g_Vars.players[owner]->client->id, which is
			// NULL once the owner disconnected (orphaned pawn).
			if (prop->type == PROPTYPE_OBJ && prop->obj->type == OBJTYPE_AUTOGUN) {
				const u8 ownerplayernum = (prop->obj->hidden & 0xf0000000) >> 28;
				if (ownerplayernum >= PLAYERCOUNT()
						|| !g_Vars.players[ownerplayernum]
						|| !g_Vars.players[ownerplayernum]->client) {
					continue;
				}
			}
			netmsgSvcPropSpawnWrite(&buf, prop);
			++nprops;
		} else if (prop->type == PROPTYPE_DOOR && prop->door) {
			// Only doors that have moved off their stage-default closed/idle
			// state; doorSetMode on the read side replays the motion and the
			// frac converges as the door finishes it.
			if (prop->door->mode != DOORMODE_IDLE || prop->door->frac > 0.0f) {
				netmsgSvcPropDoorWrite(&buf, prop, NULL);
				++ndoors;
			}
		} else if (prop->type == PROPTYPE_OBJ && prop->obj
				&& prop->obj->type == OBJTYPE_LIFT) {
			// Full lift state (level, motion, position) — idempotent.
			netmsgSvcPropLiftWrite(&buf, prop);
			++nlifts;
		}
	}

	if (buf.wp) {
		netSend(cl, &buf, true, NETCHAN_DEFAULT);
	}

	sysLogPrintf(LOG_NOTE, "NET: JIP snapshot to client %u: %d dynamic props, %d doors, %d lifts",
			cl->id, nprops, ndoors, nlifts);
}

// ---------- Co-op drop-in (dormant slots / claim / release / reclaim) ----------

// Reservation of a co-op slot for a disconnected client (reclaim-on-rejoin):
// the leaver's name, matched against CLC_AUTH names of later joiners. Empty
// string = no reservation. Server-side only; cleared on stage change.
static char g_NetCoopReservedNames[MAX_PLAYERS][32];

// Park co-op slot `playernum` dormant: dead + hidden + unbound. Runs on every
// machine (stage-load init for unclaimed slots; SVC_COOP_CLAIM release).
void netCoopDormantSlot(s32 playernum)
{
	struct player *pl = (playernum >= 0 && playernum < PLAYERCOUNT())
			? g_Vars.players[playernum] : NULL;

	if (!pl) {
		return;
	}

	pl->isdormant = true;
	// "Dead awaiting respawn" is the engine state with all the machinery we
	// want for free: enemies ignore dead players, the buddy-revive search
	// skips them, and the claim revives through the normal respawn path.
	// Anim/blood flags pre-finished so the death handling treats it as
	// settled rather than mid-death.
	pl->isdead = true;
	pl->redbloodfinished = true;
	pl->deathanimfinished = true;
	pl->dostartnewlife = false;

	// A parked slot must never sample local input: its config may hold a
	// leftover live controlmode (mpReset doesn't NA it), and bondmove would
	// happily read pad N for it. The claim restores/assigns the real value.
	g_PlayerConfigsArray[playernum].controlmode = CONTROLMODE_NA;

	if (pl->prop && pl->prop->chr) {
		pl->prop->chr->chrflags |= CHRCFLAG_HIDDEN;
	}

	if (pl->client) {
		pl->client->player = NULL;
		pl->client = NULL;
	}
	pl->isremote = false;
}

// Stage-load init: park every pre-allocated co-op slot that has no netclient
// seated (netPlayersAllocate bound the connected ones just before). Runs on
// every machine from netSyncIdsAllocate — the bindings are identical
// everywhere, so the dormant set is too.
static void netCoopDormantInit(void)
{
	s32 ndormant = 0;

	if (!g_NetMode || g_Vars.coopplayernum < 0) {
		return;
	}

	for (s32 n = 0; n < PLAYERCOUNT(); ++n) {
		struct player *pl = g_Vars.players[n];
		if (!pl || pl->client || pl->is_spectator) {
			continue;
		}
		netCoopDormantSlot(n);
		++ndormant;
	}

	if (g_NetMode == NETMODE_SERVER) {
		memset(g_NetCoopReservedNames, 0, sizeof(g_NetCoopReservedNames));
	}

	if (ndormant) {
		sysLogPrintf(LOG_NOTE, "NET: co-op drop-in: %d dormant slot(s) ready", ndormant);
	}
}

void netCoopSeatClient(struct netclient *ncl, s32 playernum, const char *name, u8 bodybit)
{
	struct player *pl = (playernum >= 0 && playernum < PLAYERCOUNT())
			? g_Vars.players[playernum] : NULL;

	if (!ncl || !pl) {
		return;
	}

	// netclient seat
	ncl->playernum = (u8)playernum;
	ncl->is_spectator = 0;
	ncl->jip_pending_unspectate = 0;
	ncl->state = CLSTATE_GAME;
	if (name && name != ncl->settings.name) {
		strncpy(ncl->settings.name, name, sizeof(ncl->settings.name) - 1);
		ncl->settings.name[sizeof(ncl->settings.name) - 1] = '\0';
	}

	// config (the netPlayersAllocate per-client binding, claim-time edition)
	struct mpplayerconfig *cfg = &g_PlayerConfigsArray[playernum];
	if (ncl == g_NetLocalClient) {
		// We're the claimant: local profile drives controls + identity
		// (contpads included — this slot's mpReset default is pad N).
		netRestoreLocalProfile(cfg);
		netSpectateStop();
	} else {
		cfg->controlmode = CONTROLMODE_NA;
		cfg->base.mpbodynum = ncl->settings.bodynum;
		cfg->base.mpheadnum = ncl->settings.headnum;
		snprintf(cfg->base.name, sizeof(cfg->base.name), "%s\n", ncl->settings.name);
		cfg->options = g_PlayerConfigsArray[0].options & OPTION_PAINTBALL;
		cfg->options |= ncl->settings.options & ~OPTION_PAINTBALL;
		cfg->options &= ~(OPTION_AIMCONTROL | OPTION_LOOKAHEAD);
		cfg->options |= OPTION_FORWARDPITCH | OPTION_ASKEDSAVEPLAYER;
	}
	cfg->client = ncl;
	cfg->handicap = 0x80;
	ncl->config = cfg;

	// player bind + wake the dormant pawn. The HOST revives it through the
	// normal respawn path (dostartnewlife -> playerStartNewLife, set by the
	// caller); clients just unpark it — position/health arrive via the
	// force-snap + stats heartbeat.
	ncl->player = pl;
	pl->client = ncl;
	pl->isremote = (ncl != g_NetLocalClient);
	pl->isdormant = false;
	pl->isdead = false;
	if (pl->prop && pl->prop->chr) {
		pl->prop->chr->chrflags &= ~CHRCFLAG_HIDDEN;
	}

	// F2 body bit for this slot (cosmetic; the pawn's chrbody was built at
	// stage start, so a mismatched body may not apply until a model rebuild)
	if (bodybit) {
		g_NetCoopBodyBits |= (u8)(1 << playernum);
	} else {
		g_NetCoopBodyBits &= (u8)~(1 << playernum);
	}

	// fresh wire state for the new binding (the stage-start manifest pattern)
	memset(ncl->inmove, 0, sizeof(ncl->inmove));
	memset(ncl->outmove, 0, sizeof(ncl->outmove));
	ncl->inmove_head = 0;
	ncl->lerpticks = 0;
	ncl->outmoveack = 0;

	sysLogPrintf(LOG_CHAT, "%s joined the mission (slot %d)", ncl->settings.name, playernum);
}

void netServerCoopClaim(struct netclient *cl)
{
	s32 slot = -1;

	if (g_NetMode != NETMODE_SERVER || !cl || g_Vars.coopplayernum < 0) {
		return;
	}

	// Reclaim first: a slot reserved under this client's name (it
	// disconnected mid-mission and came back) takes priority.
	for (s32 n = 0; n < PLAYERCOUNT(); ++n) {
		if (g_NetCoopReservedNames[n][0]
				&& strncmp(g_NetCoopReservedNames[n], cl->settings.name,
						sizeof(g_NetCoopReservedNames[n]) - 1) == 0) {
			slot = n;
			break;
		}
	}

	// Otherwise the first dormant, unreserved slot.
	if (slot < 0) {
		for (s32 n = 0; n < PLAYERCOUNT(); ++n) {
			struct player *pl = g_Vars.players[n];
			if (pl && pl->isdormant && !g_NetCoopReservedNames[n][0]) {
				slot = n;
				break;
			}
		}
	}

	if (slot < 0) {
		// Mission full (all slots seated or reserved for others): the joiner
		// stays a spectator, exactly like the pre-drop-in behaviour.
		sysLogPrintf(LOG_CHAT, "%s joined as spectator (no free co-op slot)", cl->settings.name);
		return;
	}

	g_NetCoopReservedNames[slot][0] = '\0';

	const u8 bodybit = (cl->settings.coopbodytype == COOPBODY_MASCULINE
			|| (cl->settings.coopbodytype == COOPBODY_RANDOM && (rngCosmeticRandom() & 1))) ? 1 : 0;

	netCoopSeatClient(cl, slot, NULL, bodybit);

	// Shared lives pool: the new player brings its contribution (per-player
	// budgets were seeded for every slot at stage start already).
	if (g_NetCoopLivesMode == COOP_LIVES_SHARED) {
		g_NetCoopSharedLives += g_NetCoopLivesCount;
	}

	// Revive through the normal respawn path: playerStartNewLife runs from
	// the per-player loop (server-gated) and the established force-snap
	// ships the spawn position to every client.
	if (cl->player) {
		cl->player->dostartnewlife = true;
	}

	// Tell everyone (including the claimant) about the seat.
	netbufStartWrite(&g_NetMsgRel);
	netmsgSvcCoopClaimWrite(&g_NetMsgRel, cl->id, (u8)slot, cl->settings.name, bodybit);
	netSend(NULL, &g_NetMsgRel, true, NETCHAN_DEFAULT);

	netDiagLogf("coop_claim", "cl=%u slot=%d body=%u", cl->id, slot, bodybit);
}

void netServerStageEnd(void)
{
	if (g_NetMode != NETMODE_SERVER) {
		return;
	}

	g_NetLocalClient->state = CLSTATE_LOBBY;

	netbufStartWrite(&g_NetMsgRel);
	netmsgSvcStageEndWrite(&g_NetMsgRel);
	netSend(NULL, &g_NetMsgRel, true, NETCHAN_DEFAULT);

	netDiagLogf("stage_end", "");
}

// Co-op: the local (client) simulation reached the exit / a scripted
// mission-complete fired. Tell the host so it ends the stage for ALL players
// (mainEndStage on the host broadcasts SVC_STAGE_END). Reliable control channel.
// No-op on the server — the host reaches stage end through mainEndStage directly.
void netClientStageComplete(void)
{
	if (g_NetMode != NETMODE_CLIENT || !g_NetLocalClient) {
		return;
	}

	netbufStartWrite(&g_NetMsgRel);
	netbufWriteU8(&g_NetMsgRel, CLC_STAGE_COMPLETE);
	netSend(g_NetLocalClient, &g_NetMsgRel, true, NETCHAN_CONTROL);
}

// Combat Sim: forward a simulant order from the client's active menu to the
// server (clients don't run bot AI, so a local botcmdApply would evaporate).
// botindex/targetindex are g_MpAllChrPtrs indices; targetindex only matters for
// AIBOTCMD_ATTACK (pass -1 otherwise). Server validates team ownership and
// applies (netmsgClcBotCmdRead). Reliable control channel — orders are rare
// and must not drop.
void netClientSendBotCmd(s32 botindex, u32 command, s32 targetindex)
{
	if (g_NetMode != NETMODE_CLIENT || !g_NetLocalClient
			|| g_NetLocalClient->state != CLSTATE_GAME) {
		return;
	}

	netbufStartWrite(&g_NetMsgRel);
	netbufWriteU8(&g_NetMsgRel, CLC_BOT_CMD);
	netbufWriteU8(&g_NetMsgRel, (u8)botindex);
	netbufWriteU8(&g_NetMsgRel, (u8)command);
	netbufWriteU8(&g_NetMsgRel, targetindex < 0 ? 0xffu : (u8)targetindex);
	netSend(g_NetLocalClient, &g_NetMsgRel, true, NETCHAN_CONTROL);
}

// Co-op client: report an objective WE completed that the host can't witness (a
// scripted trigger room we entered, a mine we threw onto an object, a holograph our
// camera saw). The host latches it (g_NetCoopClientObjDone) into objectiveCheck and
// rebroadcasts the authoritative status. Reliable, so a single send is enough.
void netClientSendObjectiveDone(s32 objindex)
{
	if (g_NetMode != NETMODE_CLIENT || !g_NetLocalClient
			|| objindex < 0 || objindex >= MAX_OBJECTIVES) {
		return;
	}

	netbufStartWrite(&g_NetMsgRel);
	netbufWriteU8(&g_NetMsgRel, CLC_OBJECTIVE_DONE);
	netbufWriteU8(&g_NetMsgRel, (u8)objindex);
	netSend(g_NetLocalClient, &g_NetMsgRel, true, NETCHAN_CONTROL);
}

// Co-op client: ask the host to let us pick up an OBJ/weapon prop. Clients run the
// same (read-only) pickup tests the host does (objTestForPickup) but can't take the
// prop themselves — they send this so the host re-validates against our synced
// position and grants it via SVC_PROP_PICKUP (which gives us the item + toast).
// Debounced per-prop so the request RTT doesn't flood the reliable channel.
void netClientRequestPickup(struct prop *prop)
{
	static u16 lastsid = 0;
	static u32 lasttick = 0;

	if (g_NetMode != NETMODE_CLIENT || !g_NetLocalClient || !prop || !prop->syncid) {
		return;
	}

	// Skip a re-request for the same prop within ~1/3s; if the host hasn't granted
	// it by then (LOS/position still settling) we ask again.
	if (prop->syncid == lastsid && (u32)(g_NetTick - lasttick) < 20u) {
		return;
	}
	lastsid = prop->syncid;
	lasttick = g_NetTick;

	netbufStartWrite(&g_NetMsgRel);
	netbufWriteU8(&g_NetMsgRel, CLC_PICKUP_REQUEST);
	netbufWriteU16(&g_NetMsgRel, (u16)prop->syncid);
	netSend(g_NetLocalClient, &g_NetMsgRel, true, NETCHAN_CONTROL);
}

// Co-op host-authoritative objective status. Set by SVC_OBJECTIVE on clients and
// overlaid onto objectiveCheck() (see objectives.c). Zeroed (= OBJECTIVE_INCOMPLETE)
// at boot and reset at stage start so a previous mission's completions can't leak.
u32 g_NetCoopObjStatuses[MAX_OBJECTIVES];

// Co-op host: objectives a client reported done (CLC_OBJECTIVE_DONE) that the host
// couldn't witness itself. Latched into objectiveCheck() so the host's authoritative
// status includes them. Reset at stage start with g_NetCoopObjStatuses.
u8 g_NetCoopClientObjDone[MAX_OBJECTIVES];

// Co-op: per-objective "completion toast already shown" latch. An objective can
// complete while the full HUD isn't rendering (a scripted beat), so objectivesCheckAll
// misses the transition toast; this lets it show once the HUD returns. Reset at stage
// start with the status arrays.
u8 g_NetCoopObjToastShown[MAX_OBJECTIVES];

// Client: while processing a wire-driven SVC_PROP_PICKUP, holds the host's show-toast
// decision (0/1); -1 otherwise. propPickupByPlayer mirrors it instead of re-running
// its local in_cutscene gate, so co-op pickup toasts match the host (see netmsg.c).
s8 g_NetPickupWireShowMsg = -1;

// Broadcast the host's objective status array to all clients (reliable). Called
// from objectivesCheckAll when any objective status changes, in a co-op game.
void netServerBroadcastObjectives(void)
{
	if (g_NetMode != NETMODE_SERVER) {
		return;
	}

	netbufStartWrite(&g_NetMsgRel);
	netmsgSvcObjectiveWrite(&g_NetMsgRel);
	netSend(NULL, &g_NetMsgRel, true, NETCHAN_DEFAULT);
}

// F3 lives: render "N lives remaining" to the CURRENT player as a bottom-left
// notification, mirroring Combat Sim's "Killed by X" (hudmsgCreate / DEFAULT).
static void netCoopShowLivesText(s32 count)
{
	char text[48];

	// The trailing '\n' matters: the hud-message box height comes from textMeasure,
	// which only accrues height on a newline (the pickup/kill-feed lang strings all
	// end in '\n'). Without it the box collapses to a 5px sliver in the wrong spot.
	if (count == 1) {
		sprintf(text, "1 life remaining\n");
	} else {
		sprintf(text, "%d lives remaining\n", count);
	}

	// Bottom-left notification like the kill feed, but held ~1s longer than the
	// default 80-tick duration so the player has time to read the new life count.
	hudmsgCreateWithDuration(text, HUDMSGTYPE_DEFAULT, &g_HudmsgTypes[HUDMSGTYPE_DEFAULT], 140);
}

// Show the lives notification to THIS machine's local player. The local player is
// always slot g_NetLocalClient->playernum (0 on host and, after the
// netPlayersAllocate swap, on clients too).
void netCoopShowLivesMsg(s32 count)
{
	s32 prev = g_Vars.currentplayernum;
	setCurrentPlayerNum((s32)g_NetLocalClient->playernum);
	netCoopShowLivesText(count);
	setCurrentPlayerNum(prev);
}

// Host-side: notify about a respawn. SHARED -> everyone (host-local + all clients);
// per-player -> only the victim (host-local if it's the host, else unicast to that
// client). Non-net (splitscreen) co-op shows directly to the victim's viewport.
void netServerNotifyLives(s32 victimplayernum, s32 count, bool shared)
{
	if (g_NetMode == NETMODE_CLIENT) {
		return;
	}

	if (g_NetMode == NETMODE_NONE) {
		// splitscreen co-op (non-net): show to the victim's own viewport.
		s32 prev = g_Vars.currentplayernum;
		setCurrentPlayerNum(victimplayernum);
		netCoopShowLivesText(count);
		setCurrentPlayerNum(prev);
		return;
	}

	if (shared) {
		netCoopShowLivesMsg(count); // host's local player
		netbufStartWrite(&g_NetMsgRel);
		netmsgSvcCoopLivesWrite(&g_NetMsgRel, count);
		netSend(NULL, &g_NetMsgRel, true, NETCHAN_DEFAULT); // every client
	} else if (victimplayernum == (s32)g_NetLocalClient->playernum) {
		netCoopShowLivesMsg(count); // the host is the victim
	} else {
		// unicast to the victim's client
		for (s32 i = 0; i < g_NetMaxClients; i++) {
			struct netclient *cl = &g_NetClients[i];
			if (cl != g_NetLocalClient && !cl->is_spectator
					&& cl->state >= CLSTATE_GAME && (s32)cl->playernum == victimplayernum) {
				netbufStartWrite(&g_NetMsgRel);
				netmsgSvcCoopLivesWrite(&g_NetMsgRel, count);
				netSend(cl, &g_NetMsgRel, true, NETCHAN_DEFAULT);
				break;
			}
		}
	}
}

// Replicate a host runtime chr spawn (reinforcement/clone) to clients so they
// create a matching chr shell with the host's syncid. Reliable — sent once.
void netServerBroadcastChrSpawn(struct prop *prop, f32 angle, u32 spawnflags)
{
	if (g_NetMode != NETMODE_SERVER || !prop || !prop->chr || !prop->syncid) {
		return;
	}

	netbufStartWrite(&g_NetMsgRel);
	netmsgSvcChrSpawnWrite(&g_NetMsgRel, prop, angle, spawnflags);
	netSend(NULL, &g_NetMsgRel, true, NETCHAN_DEFAULT);
}

// Replicate an NPC voice line (quip/conversation) to clients so they hear it —
// NPC AI runs server-only. Reliable control channel (a one-shot event).
void netServerBroadcastChrTalk(struct prop *prop, s32 audioid)
{
	// Co-op only: in Combat Sim, sim speech isn't replicated (and would be a
	// behaviour change). The call sites just hand us the speaking chr's prop.
	if (g_NetMode != NETMODE_SERVER || g_Vars.coopplayernum < 0 || !prop || !prop->syncid) {
		return;
	}

	netbufStartWrite(&g_NetMsgRel);
	netmsgSvcChrTalkWrite(&g_NetMsgRel, prop, audioid);
	netSend(NULL, &g_NetMsgRel, true, NETCHAN_CONTROL);
}

void netServerKick(struct netclient *cl, const u32 reason)
{
	if (g_NetMode != NETMODE_SERVER) {
		return;
	}

	if (!cl || !cl->state || !cl->peer) {
		return;
	}

	enet_peer_disconnect(cl->peer, reason);
}

s32 netStartClient(const char *addr)
{
	if (g_NetMode || !g_NetInit) {
		return -1;
	}

	if (!netParseAddr(&g_NetRemoteAddr, addr)) {
		sysLogPrintf(LOG_ERROR, "NET: `%s` is not a valid address", addr);
		return -2;
	}

	memset(&g_NetLocalAddr, 0, sizeof(g_NetLocalAddr));
	g_NetHost = enet_host_create(&g_NetLocalAddr, 1, NETCHAN_COUNT, g_NetClientInRate, g_NetClientOutRate, 0);
	if (!g_NetHost) {
		sysLogPrintf(LOG_ERROR, "NET: could not create ENet host");
		return -3;
	}

	enet_host_set_intercept_callback(g_NetHost, NULL);

	// save the address since it appears to be valid
	strncpy(g_NetLastJoinAddr, addr, NET_MAX_ADDR);

	// we'll use the whole array to store what we know of other clients
	netClientResetAll();

	// for now use last client struct
	g_NetLocalClient = &g_NetClients[NET_MAX_CLIENTS];

	sysLogPrintf(LOG_NOTE, "NET: using protocol version %d", NET_PROTOCOL_VER);
	sysLogPrintf(LOG_NOTE, "NET: connecting to %s...", addr);

	g_NetLocalClient->peer = enet_host_connect(g_NetHost, &g_NetRemoteAddr, NETCHAN_COUNT, NET_PROTOCOL_VER);
	if (!g_NetLocalClient->peer) {
		sysLogPrintf(LOG_WARNING, "NET: could not connect to %s", addr);
		enet_host_destroy(g_NetHost);
		g_NetHost = NULL;
		return -4;
	}

	g_NetLocalClient->state = CLSTATE_CONNECTING;
	netClientReadConfig(g_NetLocalClient, 0);
	g_NetLocalProfileBackup = g_PlayerConfigsArray[0];

	g_NetMode = NETMODE_CLIENT;

	g_NetTick = 0;
	g_NetNextUpdate = 0;
	g_NetNextSyncId = 1;

	sysLogPrintf(LOG_NOTE, "NET: waiting for response from %s...", addr);

	netDiagOpen();
	netDiagLogf("client_start", "addr=%s protocol=%d", addr, NET_PROTOCOL_VER);

	return 0;
}

s32 netDisconnect(void)
{
	if (!g_NetMode) {
		return -1;
	}

	// Tell the master we're going away (best-effort) while the socket is still
	// up and we're still in NETMODE_SERVER. No-op on clients.
	netMasterUnregister();

	// stop responding to connectionless packets
	enet_host_set_intercept_callback(g_NetHost, NULL);

	const bool wasingame = (g_NetLocalClient->state >= CLSTATE_GAME);

	for (s32 i = 0; i < NET_MAX_CLIENTS + 1; ++i) {
		if (g_NetClients[i].peer) {
			enet_peer_disconnect_now(g_NetClients[i].peer, DISCONNECT_SHUTDOWN);
		}
		netClientReset(&g_NetClients[i]);
	}

	g_NetLocalClient = &g_NetClients[NET_MAX_CLIENTS];

	// flush pending packets
	enet_host_flush(g_NetHost);

	// service for a bit just to ensure disconnect gets to peer(s)
	enet_host_service(g_NetHost, NULL, 10);

	enet_host_destroy(g_NetHost);

	g_NetHost = NULL;
	g_NetMode = NETMODE_NONE;

	// Reset CSP correction state so it doesn't bleed into the next session
	g_NetCspCorrFrames = 0;
	g_NetCspHead = 0;
	memset(g_NetCspHistory, 0, sizeof(g_NetCspHistory));

	// Slow-motion timescale: a client leaving mid-slow-mo must not stay at
	// half tick rate offline (the wire flag would never be cleared).
	g_LvSlomoEngaged = false;
	g_NetLastTimescale = 0;

	// Clear the kill feed and lobby state so a fresh session starts clean.
	netKillFeedClear();
	g_NetLobbyState.valid = 0;
	g_NetCoopHosting = 0; // co-op hosting intent is per-session

	// Free any packets still sitting in the lag-sim queue (they'll never be
	// sent since the peers are gone). Keep g_NetSimLagMs / g_NetSimPacketLoss
	// configured across sessions so the user can host → /lag 100 → disconnect
	// → host again without re-issuing the command.
	netLagQueueClear();
	g_NetLagQueueDropped = 0;

	netDiagLogf("disconnect", "wasingame=%d", (int)wasingame);
	netDiagClose();

	sysLogPrintf(LOG_CHAT, "NET: disconnected");

	if (wasingame) {
		// skip the "want to save" dialog for all players
		for (s32 i = 0; i < MAX_PLAYERS; ++i) {
			if (g_Vars.players[i]) {
				g_PlayerConfigsArray[i].options |= OPTION_ASKEDSAVEPLAYER;
			}
		}
		// end the stage immediately
		mainEndStage();
		// try to drop back to main menu with 1 player
		mpSetPaused(MPPAUSEMODE_UNPAUSED);
		g_MpSetup.chrslots = 1;
		g_Vars.mplayerisrunning = false;
		g_Vars.normmplayerisrunning = false;
		g_Vars.lvmpbotlevel = 0;
		titleSetNextStage(STAGE_CITRAINING);
		setNumPlayers(1);
		titleSetNextMode(TITLEMODE_SKIP);
		mainChangeToStage(STAGE_CITRAINING);
	}

	return 0;
}

static void netServerEvConnect(ENetPeer *peer, const u32 data)
{
	const char *addrstr = netFormatPeerAddr(peer);

	sysLogPrintf(LOG_NOTE | LOGFLAG_NOCON, "NET: connection attempt from %s", addrstr);

	// g_NetNumClients is bumped only once a slot is actually assigned (below),
	// not here — otherwise the reject paths (protocol mismatch / server full)
	// return having counted a peer that never became a client, and a rejected
	// peer whose disconnect event is lost drifts the counter up permanently.

	if (data != NET_PROTOCOL_VER) {
		sysLogPrintf(LOG_NOTE | LOGFLAG_NOCON, "NET: %s rejected: protocol mismatch", addrstr);
		enet_peer_disconnect(peer, DISCONNECT_VERSION);
		return;
	}

	// Late-join handling: previously hard-rejected (DISCONNECT_LATE). Now
	// accepted as a spectator — they observe the running match without
	// allocating a chr/prop/syncid, and mpStartMatch unspectates them at
	// the next round boundary so they spawn cleanly. The is_spectator byte
	// is broadcast in SVC_STAGE_START / SVC_LOBBY_STATE so other clients
	// see them tagged as (spec) in the lobby UI.
	const bool jip = (g_NetLocalClient && g_NetLocalClient->state > CLSTATE_LOBBY);

	struct netclient *cl = NULL;

	// id 0 is the local client
	for (s32 i = 1; i < g_NetMaxClients; ++i) {
		if (!g_NetClients[i].state) {
			cl = &g_NetClients[i];
			break;
		}
	}

	if (!cl) {
		sysLogPrintf(LOG_NOTE | LOGFLAG_NOCON, "NET: %s rejected: server is full", addrstr);
		enet_peer_disconnect(peer, DISCONNECT_FULL);
		return;
	}

	netClientReset(cl);
	cl->state = CLSTATE_AUTH; // skip CLSTATE_CONNECTING, since we already know it connected
	cl->peer = peer;
	if (jip) {
		cl->is_spectator = 1;
		cl->jip_pending_unspectate = 1;
		// Never let a JIP joiner carry a combatant playernum before it's
		// seated — netClientReset's memset default of 0 collided with the
		// host's slot 0 in the co-op manifest, making the joiner drive a
		// local copy of the host's pawn that nothing updated (the
		// "intangible ghost walker"). The sentinel blows up loudly instead.
		cl->playernum = NET_PLAYERNUM_SPECTATOR;
		sysLogPrintf(LOG_NOTE, "NET: %s joining in progress as spectator (will spawn next round)", addrstr);
	}
	enet_peer_set_data(peer, cl);
	// Count only now that the peer owns a real client slot; netServerEvDisconnect
	// (the only path that runs for an attached client) decrements the match.
	++g_NetNumClients;
}

static void netServerEvDisconnect(struct netclient *cl)
{
	sysLogPrintf(LOG_NOTE | LOGFLAG_NOCON, "NET: disconnect event from %s", netFormatClientAddr(cl));

	if (cl->peer) {
		// Discard any packets the lag-sim is holding for this peer before
		// the peer object is freed by enet_peer_reset.
		netLagQueueDropPeer(cl->peer);
		enet_peer_reset(cl->peer);
	}

	if (cl->settings.name[0]) {
		sysLogPrintf(LOG_NOTE, "NET: client %u (%s) disconnected", cl->id, cl->settings.name);
		netChatPrintf(NULL, "%s disconnected", cl->settings.name);
	} else {
		sysLogPrintf(LOG_CHAT, "NET: client %u disconnected", cl->id);
	}

	// If the disconnecting client held admin control, release it so the
	// dedicated auto-start / vote machine resumes instead of staying frozen.
	if (g_NetAdminController == cl->id) {
		g_NetAdminController = NET_NULL_CLIENT;
		sysLogPrintf(LOG_NOTE, "NET: admin controller (client %u) disconnected, releasing control", cl->id);
	}

	// Vote tally upkeep: if this client had a vote outstanding, drop it.
	// The deadline isn't extended — the vote closes on schedule, just with
	// one fewer ballot in the pool.
	if (g_NetVote.state == NETVOTE_OPEN
			&& cl->id < (sizeof(g_NetVote.client_vote) / sizeof(g_NetVote.client_vote[0]))) {
		const s8 prev = g_NetVote.client_vote[cl->id];
		if (prev >= 0 && prev < g_NetVote.num_candidates && g_NetVote.tally[prev] > 0) {
			g_NetVote.tally[prev]--;
		}
		g_NetVote.client_vote[cl->id] = -1;
	}

	// If we were spectating this client's pawn, stop now — its chr is about to
	// be orphaned (client->player->client cleared by netClientReset) and freed
	// at the next stage. Leaving g_NetSpectateChr pointing at it makes the
	// spectate redirect / camera chase a dangling pointer when the round ends.
	if (g_NetSpectateChr && cl->player && cl->player->prop
			&& cl->player->prop->chr == g_NetSpectateChr) {
		netSpectateStop();
	}

	// Combat Sim: kill the leaver's pawn through the normal death path
	// (playerDie = the kill-plane path: drops weapons, records the death,
	// sets isdead; the server broadcast inside it ships SVC_PLAYER_STATS) so
	// it corpses instead of standing in the arena as an untargetable statue
	// for the rest of the round. The slot itself is still recycled only at
	// the next round boundary. Co-op is excluded — there a leaver's pawn is
	// kept for reclaim-on-rejoin (see the co-op drop-in work).
	if (cl->state == CLSTATE_GAME && g_Vars.normmplayerisrunning
			&& cl->playernum < MAX_PLAYERS
			&& cl->player && cl->player->prop && !cl->player->isdead) {
		const s32 prevplayernum = g_Vars.currentplayernum;
		setCurrentPlayerNum(cl->playernum);
		playerDie(true);
		setCurrentPlayerNum(prevplayernum);
	}

	// Co-op drop-in: reserve the leaver's slot under its name (reclaim on
	// rejoin — netServerCoopClaim matches CLC_AUTH names against this) and
	// park the pawn dormant; broadcast the release so every client parks it
	// too. The reservation holds until mission end or the owner returns.
	if (cl->state == CLSTATE_GAME && g_Vars.coopplayernum >= 0
			&& cl->playernum < MAX_PLAYERS && cl->player) {
		const s32 slot = cl->playernum;
		strncpy(g_NetCoopReservedNames[slot], cl->settings.name,
				sizeof(g_NetCoopReservedNames[slot]) - 1);
		g_NetCoopReservedNames[slot][sizeof(g_NetCoopReservedNames[slot]) - 1] = '\0';
		netCoopDormantSlot(slot); // also unbinds cl->player
		netbufStartWrite(&g_NetMsgRel);
		netmsgSvcCoopClaimWrite(&g_NetMsgRel, NET_NULL_CLIENT, (u8)slot,
				g_NetCoopReservedNames[slot], 0);
		netSend(NULL, &g_NetMsgRel, true, NETCHAN_DEFAULT);
		sysLogPrintf(LOG_CHAT, "%s left the mission (slot %d held for rejoin)",
				g_NetCoopReservedNames[slot], slot);
	}

	netClientReset(cl);

	--g_NetNumClients;
}

static void netServerEvReceive(struct netclient *cl)
{
	u32 rc = 0;
	u8 msgid = 0;

	// Stop on cl->in.error as well as rc: a truncated final message leaves the
	// read pointer short of wp with the error flag set; netbufReadU8 then keeps
	// returning 0 (= *_NOP, rc stays 0) without advancing rp, so without this
	// guard the loop would spin forever on a malformed packet (remote hang).
	while (!rc && !cl->in.error && netbufReadLeft(&cl->in) > 0) {
		msgid = netbufReadU8(&cl->in);
		switch (msgid) {
			case CLC_NOP: rc = 0; break;
			case CLC_AUTH: rc = netmsgClcAuthRead(&cl->in, cl); break;
			case CLC_CHAT: rc = netmsgClcChatRead(&cl->in, cl); break;
			case CLC_MOVE: rc = netmsgClcMoveRead(&cl->in, cl); break;
			case CLC_SETTINGS: rc = netmsgClcSettingsRead(&cl->in, cl); break;
			case CLC_HIT: rc = netmsgClcHitRead(&cl->in, cl); break;
			case CLC_VOTE: rc = netmsgClcVoteRead(&cl->in, cl); break;
			case CLC_ADMIN: rc = netmsgClcAdminRead(&cl->in, cl); break;
			case CLC_ADMIN_SETUP: rc = netmsgClcAdminSetupRead(&cl->in, cl); break;
			case CLC_PROP_HIT: rc = netmsgClcPropHitRead(&cl->in, cl); break;
			case CLC_STAGE_COMPLETE: rc = netmsgClcStageCompleteRead(&cl->in, cl); break;
			case CLC_OBJECTIVE_DONE: rc = netmsgClcObjectiveDoneRead(&cl->in, cl); break;
			case CLC_PICKUP_REQUEST: rc = netmsgClcPickupRequestRead(&cl->in, cl); break;
			case CLC_BOT_CMD: rc = netmsgClcBotCmdRead(&cl->in, cl); break;
			case CLC_STAGE_READY: rc = netmsgClcStageReadyRead(&cl->in, cl); break;
			default:
				rc = 1;
				break;
		}
	}

	if (rc) {
		sysLogPrintf(LOG_WARNING , "NET: malformed or unknown message 0x%02x from client %u", msgid, cl->id);
	}
}

static void netClientEvConnect(const u32 data)
{
	sysLogPrintf(LOG_NOTE, "NET: connected to server, sending CLC_AUTH");

	g_NetLocalClient->state = CLSTATE_AUTH;

	// send auth request
	netbufStartWrite(&g_NetMsgRel);
	netmsgClcAuthWrite(&g_NetMsgRel);
	netmsgClcSettingsWrite(&g_NetMsgRel);
	netSend(g_NetLocalClient, &g_NetMsgRel, true, NETCHAN_CONTROL);
}

static void netClientEvDisconnect(const u32 reason)
{
	sysLogPrintf(LOG_CHAT, "NET: disconnected from server: %s (%u)", netGetDisconnectReason(reason), reason);
	netDisconnect();
}

static void netClientEvReceive(struct netclient *cl)
{
	u32 rc = 0;
	u8 msgid = 0;

	// See netServerEvReceive: stop on cl->in.error too so a truncated trailing
	// message can't spin this loop forever (SVC_NOP on a non-advancing read).
	while (!rc && !cl->in.error && netbufReadLeft(&cl->in) > 0) {
		msgid = netbufReadU8(&cl->in);
		switch (msgid) {
			case SVC_NOP: rc = 0; break;
			case SVC_AUTH: rc = netmsgSvcAuthRead(&cl->in, cl); break;
			case SVC_CHAT: rc = netmsgSvcChatRead(&cl->in, cl); break;
			case SVC_STAGE_START: rc = netmsgSvcStageStartRead(&cl->in, cl); break;
			case SVC_STAGE_END: rc = netmsgSvcStageEndRead(&cl->in, cl); break;
			case SVC_PLAYER_MOVE: rc = netmsgSvcPlayerMoveRead(&cl->in, cl); break;
			case SVC_PLAYER_STATS: rc = netmsgSvcPlayerStatsRead(&cl->in, cl); break;
			case SVC_PROP_MOVE: rc = netmsgSvcPropMoveRead(&cl->in, cl); break;
			case SVC_PROP_SPAWN: rc = netmsgSvcPropSpawnRead(&cl->in, cl); break;
			case SVC_PROP_DAMAGE: rc = netmsgSvcPropDamageRead(&cl->in, cl); break;
			case SVC_PROP_PICKUP: rc = netmsgSvcPropPickupRead(&cl->in, cl); break;
			case SVC_PROP_USE: rc = netmsgSvcPropUseRead(&cl->in, cl); break;
			case SVC_PROP_DOOR: rc = netmsgSvcPropDoorRead(&cl->in, cl); break;
			case SVC_PROP_LIFT: rc = netmsgSvcPropLiftRead(&cl->in, cl); break;
			case SVC_PROP_FREE: rc = netmsgSvcPropFreeRead(&cl->in, cl); break;
			case SVC_PROP_RECONCILE: rc = netmsgSvcPropReconcileRead(&cl->in, cl); break;
			case SVC_CHR_DAMAGE: rc = netmsgSvcChrDamageRead(&cl->in, cl); break;
			case SVC_CHR_DISARM: rc = netmsgSvcChrDisarmRead(&cl->in, cl); break;
			case SVC_CHR_FIRE: rc = netmsgSvcChrFireRead(&cl->in, cl); break;
			case SVC_KILL: rc = netmsgSvcKillRead(&cl->in, cl); break;
			case SVC_SCORE: rc = netmsgSvcScoreRead(&cl->in, cl); break;
			case SVC_KOH_STATE: rc = netmsgSvcKohStateRead(&cl->in, cl); break;
			case SVC_EXPLOSION: rc = netmsgSvcExplosionRead(&cl->in, cl); break;
			case SVC_LOBBY_STATE: rc = netmsgSvcLobbyStateRead(&cl->in, cl); break;
			case SVC_VOTE_OPEN: rc = netmsgSvcVoteOpenRead(&cl->in, cl); break;
			case SVC_VOTE_RESULTS: rc = netmsgSvcVoteResultsRead(&cl->in, cl); break;
			case SVC_ADMIN: rc = netmsgSvcAdminRead(&cl->in, cl); break;
			case SVC_OBJECTIVE: rc = netmsgSvcObjectiveRead(&cl->in, cl); break;
			case SVC_CHR_SPAWN: rc = netmsgSvcChrSpawnRead(&cl->in, cl); break;
			case SVC_CHR_TALK: rc = netmsgSvcChrTalkRead(&cl->in, cl); break;
			case SVC_STAGE_FLAGS: rc = netmsgSvcStageFlagsRead(&cl->in, cl); break;
			case SVC_CUTSCENE: rc = netmsgSvcCutsceneRead(&cl->in, cl); break;
			case SVC_COOP_LIVES: rc = netmsgSvcCoopLivesRead(&cl->in, cl); break;
			case SVC_TIMESCALE: rc = netmsgSvcTimescaleRead(&cl->in, cl); break;
			case SVC_COOP_CLAIM: rc = netmsgSvcCoopClaimRead(&cl->in, cl); break;
			default:
				rc = 1;
				break;
		}
	}

	if (rc) {
		sysLogPrintf(LOG_WARNING, "NET: malformed or unknown message 0x%02x from server", msgid);
	}
}

void netClientSyncRng(void)
{
	if (g_NetMode == NETMODE_CLIENT && g_NetRngLatch) {
		g_NetRngLatch = 0;
		g_RngSeed = g_NetRngSeeds[0];
		g_Rng2Seed = g_NetRngSeeds[1];
	}
}

void netClientSettingsChanged(void)
{
	if (g_NetMode != NETMODE_CLIENT || !g_NetLocalClient) {
		return;
	}

	netClientReadConfig(g_NetLocalClient, 0);

	netbufStartWrite(&g_NetMsgRel);
	netmsgClcSettingsWrite(&g_NetMsgRel);
	netSend(NULL, &g_NetMsgRel, true, NETCHAN_CONTROL);
}

// Deferred CLC_HIT queue. CLC_HIT arrives during netStartFrame event processing,
// but netStartFrame resets g_NetMsgRel immediately after. If chrDamage were called
// then, the SVC_CHR_DAMAGE it writes would be discarded before netFlushSendBuffers
// ever runs. Instead, netmsgClcHitRead calls netServerEnqueueHit to stage the hit,
// and netEndFrame drains the queue before its first flush so broadcasts go through.
#define NET_PENDING_HITS_MAX 16

struct net_pending_hit {
	struct prop *target;
	struct prop *shooter_prop;
	struct coord vector;
	struct gset gset;
	f32 damage;
	s32 playernum;
	s16 hitpart;
	s16 side;
	s16 arg10[3];
};

static struct net_pending_hit g_NetPendingHits[NET_PENDING_HITS_MAX];
static s32 g_NetPendingHitCount = 0;

void netServerEnqueueHit(struct prop *target, f32 damage, const struct coord *vector,
		const struct gset *gset, s16 hitpart, s16 side, const s16 *arg10,
		s32 playernum, struct prop *shooter_prop)
{
	if (g_NetPendingHitCount >= NET_PENDING_HITS_MAX) {
		return;
	}
	struct net_pending_hit *ph = &g_NetPendingHits[g_NetPendingHitCount++];
	ph->target = target;
	ph->shooter_prop = shooter_prop;
	ph->vector = *vector;
	ph->gset = *gset;
	ph->damage = damage;
	ph->playernum = playernum;
	ph->hitpart = hitpart;
	ph->side = side;
	ph->arg10[0] = arg10 ? arg10[0] : 0;
	ph->arg10[1] = arg10 ? arg10[1] : 0;
	ph->arg10[2] = arg10 ? arg10[2] : 0;
}

// Same deferred-application pattern as the chr-hit queue above, for
// client-reported destructible-prop hits (CLC_PROP_HIT). objDamage broadcasts
// SVC_PROP_DAMAGE, so it must run in netEndFrame after the buffer reset.
#define NET_PENDING_PROP_HITS_MAX 16

struct net_pending_prop_hit {
	struct prop *prop;
	struct coord pos;
	f32 damage;
	s32 weaponnum;
	s32 playernum;
};

static struct net_pending_prop_hit g_NetPendingPropHits[NET_PENDING_PROP_HITS_MAX];
static s32 g_NetPendingPropHitCount = 0;

void netServerEnqueuePropHit(struct prop *prop, f32 damage, const struct coord *pos,
		s32 weaponnum, s32 playernum)
{
	if (g_NetPendingPropHitCount >= NET_PENDING_PROP_HITS_MAX) {
		return;
	}
	struct net_pending_prop_hit *ph = &g_NetPendingPropHits[g_NetPendingPropHitCount++];
	ph->prop = prop;
	ph->pos = *pos;
	ph->damage = damage;
	ph->weaponnum = weaponnum;
	ph->playernum = playernum;
}

// Client -> server: report our local player's gunfire hit on a destructible prop
// (glass / object). Called from objTakeGunfire. No-op unless we're a connected
// client in-game. The server validates + applies + broadcasts SVC_PROP_DAMAGE.
void netClientReportPropHit(struct prop *prop, f32 damage, const struct coord *pos, s32 weaponnum)
{
	if (g_NetMode != NETMODE_CLIENT || !g_NetLocalClient
			|| g_NetLocalClient->state < CLSTATE_GAME || !prop || !prop->syncid) {
		return;
	}
	netbufStartWrite(&g_NetMsgRel);
	netmsgClcPropHitWrite(&g_NetMsgRel, prop, damage, pos, weaponnum);
	netSend(g_NetLocalClient, &g_NetMsgRel, true, NETCHAN_CONTROL);
}

void netStartFrame(void)
{
	if (!g_NetMode) {
		return;
	}

	++g_NetTick;

	// R4 (docs/netplay-perf-review-2026.md): g_NetTick advances once per
	// netStartFrame call (once per diffframe60>0 frame), NOT once per logical 1/60
	// sim step. On a machine that sustains <60fps the net clock drifts slower than
	// wall-clock while the sim runs multiple steps per frame, skewing tick-stamped
	// moves and CSP/interp timing. DIAGNOSTIC ONLY (no behaviour change): compare
	// the net tick against the microsecond wall clock and warn (throttled) on
	// sustained divergence so the condition is visible instead of silent. Re-bases
	// on session start, a backward jump, or an implausibly large step (a client
	// adopting the server's tick value), none of which are frame-rate drift.
	{
		static u64 s_base_us = 0;
		static u32 s_base_tick = 0;
		static u32 s_last_warn_tick = 0;
		const u64 now_us = sysGetMicroseconds();
		const s64 dtick = (s64)g_NetTick - (s64)s_base_tick;
		if (s_base_us == 0u || dtick < 0) {
			s_base_us = now_us;
			s_base_tick = g_NetTick;
		} else {
			const s64 elapsed_ticks = (s64)((now_us - s_base_us) * 60ULL / 1000000ULL);
			const s64 drift = dtick - elapsed_ticks; // < 0 => net clock behind wall clock
			if (drift > 600 || drift < -600) {
				// >10s implied drift can't accrue from frame pacing this fast — it's a
				// clock discontinuity (e.g. server-tick adoption). Re-base silently.
				s_base_us = now_us;
				s_base_tick = g_NetTick;
			} else if ((drift > 30 || drift < -30) && (g_NetTick - s_last_warn_tick) >= 60u) {
				s_last_warn_tick = g_NetTick;
				netDiagLogf("tickdrift", "net=%lld wall=%lld drift=%lld",
						(long long)dtick, (long long)elapsed_ticks, (long long)drift);
				sysLogPrintf(LOG_WARNING | LOGFLAG_NOCON,
						"NET: tick clock drift %lld ticks vs wall clock (sustained <60fps?)",
						(long long)drift);
			}
		}
	}

	// Heartbeat for crash hunts. Logs every 6 ticks (~100ms at 60Hz) so the
	// diag file shows progress through gameplay with fine enough granularity
	// to bracket a crash to ≤6 frames. Pairs with the existing pos_cl /
	// pos_sim dumps (same rate) so a missing tick line implies the crash
	// landed inside that 100ms window. Lifetime cost is minor — one fprintf
	// per 6 frames is well under the diag log's existing event rate.
	if ((g_NetTick % 6u) == 0u) {
		netDiagLogf("tick", "stage=%u clstate=%u",
			(u32)g_StageNum,
			(u32)(g_NetLocalClient ? g_NetLocalClient->state : 0));
	}

	// Release any artificially-delayed packets whose hold time has elapsed.
	// Has to happen before ENet services its socket so newly-due packets are
	// actually transmitted this frame instead of waiting another tick.
	if (g_NetSimLagMs > 0 || g_NetLagQueueDropped > 0) {
		netLagQueueDrain();
	}

	const bool isClient = (g_NetMode == NETMODE_CLIENT);

	// Admin "configure" opens the Combat Sim menu (MENUROOT_MPSETUP) over the
	// live CITRAINING lobby straight from the console, bypassing the CI data-
	// terminal interaction that normally strips player control. Without that,
	// the admin's local pawn keeps reading input and the camera drifts around
	// behind the menu. While that menu is open on a client, clear the local
	// player's control gate (the bmoveTick(0,0,0,1) branch at player.c:4074);
	// restore it once on close. A client's local player is always slot 0
	// (netPlayersAllocate swaps it there). Runs pre-tick so the player read
	// later this frame sees the cleared flag.
	{
		static bool s_adminMenuLockHeld = false;
		const bool adminMenuOpen = (isClient && g_MenuData.root == MENUROOT_MPSETUP);
		if (adminMenuOpen) {
			g_PlayersWithControl[0] = false;
			s_adminMenuLockHeld = true;
		} else if (s_adminMenuLockHeld) {
			g_PlayersWithControl[0] = true;
			s_adminMenuLockHeld = false;
		}
	}
	// Drain every event ready this frame, not just the first. enet_host_service()
	// reads the whole socket and queues the inbound burst but hands back only the
	// first event; the old loop processed that one and exited, leaving the rest of
	// the burst to wait for the next frame (up to ~16ms added latency under load —
	// netplay-perf-review-2026 "smaller"). Re-drain the queue with
	// enet_host_check_events after each service, and cap the number of socket
	// services so a sustained packet flood can't stall the frame.
	const s32 maxservices = 8;
	s32 numservices = 0;
	ENetEvent ev = { .type = ENET_EVENT_TYPE_NONE };
	for (;;) {
		if (enet_host_check_events(g_NetHost, &ev) <= 0) {
			if (numservices >= maxservices || enet_host_service(g_NetHost, &ev, 1) <= 0) {
				break;
			}
			numservices++;
		}

		switch (ev.type) {
			case ENET_EVENT_TYPE_CONNECT:
				if (isClient) {
					netClientEvConnect(ev.data);
				} else if (ev.peer) {
					netServerEvConnect(ev.peer, ev.data);
				}
				break;
			case ENET_EVENT_TYPE_DISCONNECT:
			case ENET_EVENT_TYPE_DISCONNECT_TIMEOUT:
				if (isClient) {
					netClientEvDisconnect(ev.type == ENET_EVENT_TYPE_DISCONNECT_TIMEOUT ? DISCONNECT_TIMEOUT : ev.data);
				} else if (ev.peer) {
					struct netclient *cl = enet_peer_get_data(ev.peer);
					if (cl) {
						netServerEvDisconnect(cl);
					} else {
						// No attached client => this peer was rejected before a slot
						// was assigned, so it was never counted; just log it.
						sysLogPrintf(LOG_WARNING | LOGFLAG_NOCON, "NET: disconnect from %s without attached client", netFormatPeerAddr(ev.peer));
					}
				}
				break;
			case ENET_EVENT_TYPE_RECEIVE:
				if (ev.peer) {
					struct netclient *cl = (g_NetMode == NETMODE_CLIENT) ? g_NetLocalClient : enet_peer_get_data(ev.peer);
					if (cl && cl->state) {
						if (ev.packet->data && ev.packet->dataLength) {
							netbufStartReadData(&cl->in, ev.packet->data, ev.packet->dataLength);
							if (isClient) {
								netClientEvReceive(cl);
							} else {
								netServerEvReceive(cl);
							}
							netbufReset(&cl->in);
						}
					} else if (!isClient) {
						sysLogPrintf(LOG_WARNING | LOGFLAG_NOCON, "NET: receive from %s without attached client", netFormatPeerAddr(ev.peer));
					}
				}
				enet_packet_dispose(ev.packet);
				break;
			default:
				break;
		}
	}

	netbufStartWrite(&g_NetMsg);
	netbufStartWrite(&g_NetMsgRel);

	// Crash-hunt diagnostic: ns_exit / ne_enter / ne_exit bracket the main
	// game-tick gap. With per-tick TU-static counters capped at 30 we get
	// visibility on the first 30 ticks of each PROCESS run without flooding
	// the log forever (g_NetTick on the client doesn't reset on stage
	// change — it carries the server's tick number — so a tick-value cap
	// like "tick < 30" would never fire mid-match).
	//
	// Trail: ns_exit → ne_enter → ne_exit each frame. Last line before
	// the crash tells you which phase died.
	static u32 ns_exit_count = 0;
	if (ns_exit_count < 30u) {
		netDiagLogf("ns_exit", "tick=%u", g_NetTick);
		ns_exit_count++;
	}
}

void netEndFrame(void)
{
	if (!g_NetMode) {
		return;
	}

	// Companion to ns_exit. Missing ne_enter ⇒ crash in mainTick (game
	// render / physics / sim chrTick path); missing ne_exit / pos_cl ⇒
	// crash in netEndFrame's send / CSP / diag block.
	static u32 ne_enter_count = 0;
	if (ne_enter_count < 30u) {
		netDiagLogf("ne_enter", "tick=%u", g_NetTick);
		ne_enter_count++;
	}

	g_NetReliableFrameLen = 0;
	g_NetUnreliableFrameLen = 0;

	// /netstats: snapshot the per-message-type byte accumulators once per second.
	if (g_NetTick - g_NetStatSecBase >= 60u) {
		for (s32 i = 0; i < NETSTAT_COUNT; ++i) {
			g_NetStatPerSec[i] = g_NetStatAccum[i];
			g_NetStatAccum[i] = 0;
		}
		g_NetStatSecBase = g_NetTick;
	}

	// Drain deferred CLC_HIT entries. chrDamage here writes SVC_CHR_DAMAGE
	// (and SVC_KILL / SVC_SCORE on a kill) into g_NetMsgRel, which was reset
	// by netStartFrame. The flush below picks them all up.
	if (g_NetPendingHitCount > 0 && g_NetMode == NETMODE_SERVER) {
		const s32 prevplayernum = g_Vars.currentplayernum;
		for (s32 i = 0; i < g_NetPendingHitCount; ++i) {
			const struct net_pending_hit *ph = &g_NetPendingHits[i];
			if (!ph->target || !ph->target->chr) {
				continue;
			}
			// Server-side hit validation: confirm the server's own authoritative,
			// lag-comp'd shotCalculateHits trace actually detected this shooter
			// hitting this target. The client's CLC_HIT is otherwise trusted; this
			// rejects (or logs) claims the server never saw. Off by default; log
			// mode applies the hit anyway so agreement can be measured first.
			if (g_NetHitValidate && ph->playernum >= 0 && ph->target->syncid) {
				struct netclient *shooter = netClientForPlayerNum(ph->playernum);
				if (shooter && !netServerHitWasDetected(shooter, (u16)ph->target->syncid)) {
					netDiagLogf("hit_reject", "shooter=%u target_sid=%u dmg=%.1f mode=%d",
							shooter->id, (unsigned)ph->target->syncid, ph->damage, g_NetHitValidate);
					if (g_NetHitValidate >= 2) {
						continue; // enforce: drop the unvalidated claim
					}
				}
			}
			if (ph->playernum >= 0) {
				setCurrentPlayerNum(ph->playernum);
			}
			chrDamage(ph->target->chr, ph->damage, (struct coord *)&ph->vector,
					(struct gset *)&ph->gset, ph->shooter_prop,
					ph->hitpart, true, ph->target, NULL, NULL,
					ph->side, (s16 *)ph->arg10, false, NULL);
		}
		setCurrentPlayerNum(prevplayernum);
		g_NetPendingHitCount = 0;
	}

	// Drain deferred CLC_PROP_HIT entries (client-reported glass / object damage).
	// On the server objDamage applies the damage (shattering glass etc.) and
	// broadcasts SVC_PROP_DAMAGE into the freshly-reset g_NetMsgRel; the flush
	// below sends it, and clients apply it via objDamage's damage<0 path.
	if (g_NetPendingPropHitCount > 0 && g_NetMode == NETMODE_SERVER) {
		const s32 prevplayernum = g_Vars.currentplayernum;
		for (s32 i = 0; i < g_NetPendingPropHitCount; ++i) {
			const struct net_pending_prop_hit *ph = &g_NetPendingPropHits[i];
			if (!ph->prop || !ph->prop->obj) {
				continue;
			}
			if (ph->playernum >= 0) {
				setCurrentPlayerNum(ph->playernum);
			}
			objDamage(ph->prop->obj, ph->damage, (struct coord *)&ph->pos,
					ph->weaponnum, ph->playernum);
		}
		setCurrentPlayerNum(prevplayernum);
		g_NetPendingPropHitCount = 0;
	}

	// send whatever messages have accumulated so far
	netFlushSendBuffers();

	// The player+prop precondition is only meaningful for the CLIENT branch
	// (which records its OWN player's move). The SERVER branch iterates remote
	// clients and sims independently and doesn't read the local client's
	// player. In dedicated mode g_NetLocalClient is a spectator with
	// player==NULL, so gating the whole block on it silently drops every
	// per-tick server broadcast — SVC_PLAYER_MOVE for each remote client,
	// SVC_PROP_MOVE for each sim, plus KoH / score / stats heartbeats —
	// and clients receive no state updates from the host.
	if ((g_NetMode == NETMODE_CLIENT
			&& g_NetLocalClient->state == CLSTATE_GAME
			&& g_NetLocalClient->player && g_NetLocalClient->player->prop)
			|| (g_NetMode == NETMODE_SERVER
			&& g_NetLocalClient->state == CLSTATE_GAME)) {
		if (g_NetMode == NETMODE_CLIENT) {
			if (g_NetTick > 100) {
				netClientRecordMove(g_NetLocalClient, g_NetLocalClient->player);
				const bool needrel = netClientNeedReliableMove(g_NetLocalClient);
				if (needrel || netClientNeedMove(g_NetLocalClient)) {
					netmsgClcMoveWrite(needrel ? &g_NetMsgRel : &g_NetMsg);
				}
			}
			if (g_NetNextUpdate <= g_NetTick) {
				g_NetNextUpdate = g_NetTick + g_NetClientUpdateRate;
			}
		} else {
			for (s32 i = 0; i < g_NetMaxClients; ++i) {
				struct netclient *cl = &g_NetClients[i];
				if (cl->state >= CLSTATE_GAME && cl->player) {
					netClientRecordMove(cl, cl->player);
					// Respawn/teleport: clear lag comp history so shots fired by
					// other players immediately after can't rewind this player back
					// to their pre-death position.
					if (cl->outmove[0].ucmd & UCMD_FL_FORCEMASK) {
						memset(cl->lagcomp, 0, sizeof(cl->lagcomp));
						cl->lagcomp_head = 0;
					}
					const bool needrel = netClientNeedReliableMove(cl);
					if (needrel || netClientNeedMove(cl)) {
						struct netbuf *mb = needrel ? &g_NetMsgRel : &g_NetMsg;
						const u32 b0 = mb->wp;
						netmsgSvcPlayerMoveWrite(mb, cl);
						netStatAdd(NETSTAT_PLAYERMOVE, mb->wp - b0);
					}
				}
			}
#ifndef PLATFORM_N64
			// broadcast sim (bot) chr positions so clients can position-drive them.
			// BYTE-BUDGETED ROUND-ROBIN (same scheme as the co-op NPC loop below):
			// a chr-state block is ~120 bytes, so a high-sim-count Combat Sim match
			// can exceed the unreliable buffer (g_NetMsg, NET_BUFSIZE). A plain
			// 0..g_BotCount scan with no space guard let it overflow — the packet was
			// then truncated on send and the client dropped the tail message, so the
			// SAME high-index sims lost their update every tick (fixed scan order),
			// reading as permanently laggy while sim 0 stayed smooth. Resume from a
			// rotating cursor and stop before the buffer fills, so the loss (when it
			// happens at all) is shared and interpolation hides it. Player moves were
			// written above, so the threshold accounts for them.
			if (g_Vars.lvmpbotlevel && g_BotCount > 0) {
				static s32 simcursor = 0;
				if (simcursor >= g_BotCount) {
					simcursor = 0;
				}
				s32 scanned = 0;
				s32 i = simcursor;
				while (scanned < g_BotCount && g_NetMsg.wp < NET_BUFSIZE - 340) {
					struct chrdata *chr = g_MpBotChrPtrs[i];
					if (chr && chr->prop && chr->prop->syncid) {
						const u32 b0 = g_NetMsg.wp;
						netmsgSvcPropMoveWrite(&g_NetMsg, chr->prop, NULL);
						netStatAdd(NETSTAT_PROPMOVE, g_NetMsg.wp - b0);
					}
					i = (i + 1) % g_BotCount;
					scanned++;
				}
				simcursor = i; // resume here next tick
			}

			// Campaign co-op (Phase 1): broadcast every active campaign NPC chr's
			// state via the same chr-state block sims use, so clients position/anim-
			// drive them. The client renders + force-ACT_STANDs them and skips their
			// AI (host-authoritative; gated in chraiExecute). Combat Sim uses the
			// g_MpBotChrPtrs loop above; the two paths are mutually exclusive.
			// BYTE-BUDGETED ROUND-ROBIN: a campaign level has more chrs than fit in one
			// unreliable packet (g_NetMsg, NET_BUFSIZE), so broadcasting all of them every
			// tick overflowed it. Resume from a rotating slot index each tick and stop once
			// the buffer nears full, so every NPC updates over a few ticks (interpolation
			// hides the gap) and the packet never overflows. Player moves were already
			// written above, so the threshold accounts for them.
			if (g_Vars.coopplayernum >= 0 && g_ChrSlots) {
				const s32 numslots = chrsGetNumSlots();
				static s32 coopnpcstart = 0;
				if (coopnpcstart >= numslots) {
					coopnpcstart = 0;
				}
				s32 scanned = 0;
				s32 i = coopnpcstart;
				while (scanned < numslots && g_NetMsg.wp < NET_BUFSIZE - 340) {
					struct chrdata *chr = &g_ChrSlots[i];
					if (chr->chrnum >= 0 && chr->prop && chr->prop->syncid
							&& chr->prop->type == PROPTYPE_CHR) {
						const u32 b0 = g_NetMsg.wp;
						netmsgSvcPropMoveWrite(&g_NetMsg, chr->prop, NULL);
						netStatAdd(NETSTAT_PROPMOVE, g_NetMsg.wp - b0);
					}
					i = (i + 1) % numslots;
					scanned++;
				}
				coopnpcstart = i; // resume here next tick
			}

			// Co-op movable OBJ position sync. An OBJ's position is otherwise only
			// broadcast on impulse events (push / throw / drop) in propobj.c — never as
			// it settles, nor while it quietly drifts. Clients run the obj's local
			// physics and (with /coopobj, default on) wire-snap to the LAST received
			// pos, so a pushed object freezes mid-arc and "floats", and a settled
			// object that drifted from its host counterpart never re-syncs. Fix: the
			// host re-broadcasts networked OBJ positions itself, in two parts:
			//  - Pass 1, every tick: any OBJ in motion (OBJHFLAG_PROJECTILE = airborne
			//    / sliding / falling) so the client follows the full arc and lands
			//    exactly where the host does (the projectile block carries speed +
			//    rotation so the motion matches). Usually 0-2 objs, so the full scan is
			//    cheap.
			//  - Pass 2, round-robin: refresh a few SETTLED objs each tick from a
			//    rotating cursor, so every networked obj re-syncs within
			//    ~maxprops/budget ticks — healing a settled-but-drifted obj, a missed
			//    impulse packet, or a JIP client, without a once-a-second burst.
			// Unreliable (g_NetMsg) like the sim/NPC moves above: latest-wins, and a
			// dropped frame self-heals on the next tick / cursor sweep.
			if (g_Vars.coopplayernum >= 0) {
				const s32 maxprops = g_Vars.maxprops;
				// Pass 1: moving objs, every tick.
				for (s32 i = 0; i < maxprops && g_NetMsg.wp < NET_BUFSIZE - 64; i++) {
					struct prop *prop = &g_Vars.props[i];
					if (prop->syncid && prop->obj && prop->type == PROPTYPE_OBJ
							&& (prop->obj->hidden & OBJHFLAG_PROJECTILE)) {
						const u32 b0 = g_NetMsg.wp;
						netmsgSvcPropMoveWrite(&g_NetMsg, prop, NULL);
						netStatAdd(NETSTAT_PROPMOVE, g_NetMsg.wp - b0);
					}
				}
				// Pass 2: settled objs, round-robin (a few per tick from a cursor).
				static s32 coopobjcursor = 0;
				if (coopobjcursor >= maxprops) {
					coopobjcursor = 0;
				}
				s32 scanned = 0;
				s32 sent = 0;
				s32 i = coopobjcursor;
				while (scanned < maxprops && sent < 4 && g_NetMsg.wp < NET_BUFSIZE - 64) {
					struct prop *prop = &g_Vars.props[i];
					if (prop->syncid && prop->obj && prop->type == PROPTYPE_OBJ
							&& (prop->obj->hidden & OBJHFLAG_PROJECTILE) == 0) {
						const u32 b0 = g_NetMsg.wp;
						netmsgSvcPropMoveWrite(&g_NetMsg, prop, NULL);
						netStatAdd(NETSTAT_PROPMOVE, g_NetMsg.wp - b0);
						sent++;
					}
					i = (i + 1) % maxprops;
					scanned++;
				}
				coopobjcursor = i; // resume here next tick
			}

			// Co-op stage flags: scripts, objectives and triggered events gate on
			// g_StageFlags, set host-side by action blocks / scripts the client
			// doesn't run. Mirror it (reliable) so the client's flag-gated logic
			// agrees — OBJECTIVETYPE_COMPFLAGS objective completion (the "objective
			// complete" pop), door/event gates, cutscene progression. On change for
			// immediacy, plus a heartbeat heal at a free phase offset (KoH 0, score
			// 15, lobby 30, stats 45) in case a change landed before the client was
			// in CLSTATE_GAME.
			if (g_Vars.coopplayernum >= 0
					&& (g_StageFlags != g_NetLastStageFlags
						|| (g_NetTick % NET_HEARTBEAT_INTERVAL) == 20u)) {
				g_NetLastStageFlags = g_StageFlags;
				netmsgSvcStageFlagsWrite(&g_NetMsgRel);
			}

			// Co-op cutscene state: in-engine cutscenes (intro, mid-mission, outro)
			// all run through playerStartCutscene/EndCutscene via AI commands the
			// client doesn't run, so mirror the tickmode==CUTSCENE state + anim. The
			// client starts/ends in lockstep with the host (fixes the client stranded
			// mid-scene until the host moves). Broadcast on transition (reliable, so a
			// single send is enough). A heartbeat re-send while active heals a join
			// that missed the start edge.
			if (g_Vars.coopplayernum >= 0) {
				const s32 active = (g_Vars.tickmode == TICKMODE_CUTSCENE) ? 1 : 0;
				const s16 anim = g_CutsceneAnimNum;
				if (active != g_NetLastCutsceneActive
						|| (active && anim != g_NetLastCutsceneAnim)
						|| (active && (g_NetTick % NET_HEARTBEAT_INTERVAL) == 40u)) {
					g_NetLastCutsceneActive = active;
					g_NetLastCutsceneAnim = anim;
					netmsgSvcCutsceneWrite(&g_NetMsgRel, active, anim);
				}
			}

			// Slow motion / combat boost timescale: lvTick decides the halved
			// sim step on the server (g_LvSlomoEngaged); mirror it so clients
			// halve the same pinned step in detPinTimestep and both machines
			// advance identical sim time per tick (the g_NetTick cadence is
			// real-time 60Hz either way, so interp / lag-comp / CSP timing is
			// unaffected). On change for immediacy, plus a heartbeat heal at a
			// free phase offset (50) for drops. Not co-op-gated — Combat Sim's
			// Slow Motion option and the Combat Boost pickup both drive it.
			{
				const u8 ts = g_LvSlomoEngaged ? 1 : 0;
				if (ts != g_NetLastTimescale
						|| (g_NetTick % NET_HEARTBEAT_INTERVAL) == 50u) {
					g_NetLastTimescale = ts;
					netmsgSvcTimescaleWrite(&g_NetMsgRel);
				}
			}

			// King of the Hill: keep clients' hill state in sync. Broadcast
			// every NET_HEARTBEAT_INTERVAL ticks (~1 second) as a keep-alive;
			// on-change broadcasts come from kohTick (kingofthehill.inc)
			// immediately after hill selection.
			if (g_MpSetup.scenario == MPSCENARIO_KINGOFTHEHILL
					&& (g_NetTick % NET_HEARTBEAT_INTERVAL) == 0u) {
				netmsgSvcKohStateWrite(&g_NetMsgRel);
			}

			// Scoreboard heartbeat: SVC_SCORE only fires on kill events
			// (SVC_KILL path) — if a single packet is dropped or a client
			// joined mid-round via JIP, the local scoreboard can silently
			// disagree with the server. Rebroadcast the full table every
			// second to heal it. Phase-offset within NET_HEARTBEAT_INTERVAL
			// so it doesn't land on the same tick as KoH (0) or lobby (30).
			if ((g_NetTick % NET_HEARTBEAT_INTERVAL) == (NET_HEARTBEAT_INTERVAL / 4u)) {
				s32 indexes[MAX_MPCHRS];
				s32 count = 0;
				for (s32 i = 0; i < MAX_MPCHRS; ++i) {
					if (g_MpAllChrConfigPtrs[i]) {
						indexes[count++] = i;
					}
				}
				if (count > 0) {
					netmsgSvcScoreWrite(&g_NetMsgRel, indexes, count);
				}
			}

			// Player-stats heartbeat: SVC_PLAYER_STATS is on-change too
			// (health, armor, weapon, ammo). Same drift risk as scores,
			// same cheap fix. Phase-offset to 3/4 within the interval so
			// the four periodic broadcasts (KoH at 0, score at 15, lobby
			// at 30, stats at 45) spread their bandwidth instead of all
			// landing on the same frame.
			if ((g_NetTick % NET_HEARTBEAT_INTERVAL) == (3u * NET_HEARTBEAT_INTERVAL / 4u)) {
				for (s32 i = 0; i < g_NetMaxClients; ++i) {
					struct netclient *cl = &g_NetClients[i];
					if (cl->state >= CLSTATE_GAME && cl->player && cl->player->prop) {
						const u32 b0 = g_NetMsgRel.wp;
							netmsgSvcPlayerStatsWrite(&g_NetMsgRel, cl);
							netStatAdd(NETSTAT_PLAYERSTATS, g_NetMsgRel.wp - b0);
					}
				}
			}

			// Prop reconciliation backstop: twice a second (phase 10) broadcast the
			// active weapon/obj syncid set so clients drop ghosts the host already
			// freed but whose SVC_PROP_FREE they missed (the screen-gated embedded-
			// mine free path). Backstop only — SVC_PROP_FREE is the primary path.
			if ((g_NetTick % (NET_HEARTBEAT_INTERVAL / 2u)) == 10u) {
				netmsgSvcPropReconcileWrite(&g_NetMsgRel);
			}
#endif
			if (g_NetNextUpdate <= g_NetTick) {
				// P3 (docs/netplay-perf-review-2026.md): adaptive player-move send
				// cadence. The gate that throttles SVC_PLAYER_MOVE sends is global,
				// so its interval scales the per-tick player-move bandwidth directly.
				// Keep every-tick (rate 1) for the common 2-4 combatant case — no
				// feel change — and only stretch to every-other-tick once the match
				// is large enough that 60Hz of full moves for everyone is wasteful;
				// interpolation (g_NetInterpTicks, default 3 ticks) easily hides the
				// 30Hz cadence. An operator override (/svcrate or Net.Server.Update-
				// Frames > 1) still wins via max().
				s32 combatants = 0;
				for (s32 ci = 0; ci < g_NetMaxClients; ++ci) {
					if (g_NetClients[ci].state >= CLSTATE_GAME && g_NetClients[ci].player) {
						++combatants;
					}
				}
				const u32 adaptive = (combatants > 4) ? 2u : 1u;
				const u32 rate = (g_NetServerUpdateRate > adaptive) ? g_NetServerUpdateRate : adaptive;
				g_NetNextUpdate = g_NetTick + rate;
			}
		}
	}

	// send position updates
	netFlushSendBuffers();

	// Advertise to the master server (server-only; self-gated + rate-limited).
	netMasterTick();

#ifndef PLATFORM_N64
	// Dedicated server: auto-start the first match from the playlist once the
	// server is up and in CITRAINING (combat-sim lobby). Gives a 1-second
	// grace window so config / playlist / menu state settles, then applies
	// the first playlist entry and calls mpStartMatch (which transitions to
	// the actual stage). After this first match, the vote machine below
	// handles round-to-round advancement.
	// Helper: count connected non-spectator clients. Used by the dedicated
	// auto-start gate and the post-vote advance to decide whether to spin a
	// match or sit idle. Skips slot 0 (the local server client — always
	// is_spectator in dedicated mode).
	s32 humans_connected = 0;
	if (g_NetMode == NETMODE_SERVER) {
		for (s32 _ci = 1; _ci < g_NetMaxClients; _ci++) {
			if (g_NetClients[_ci].state >= CLSTATE_LOBBY
					&& !g_NetClients[_ci].is_spectator) {
				humans_connected++;
			}
		}
	}

	if (g_NetMode == NETMODE_SERVER && g_NetDedicatedMode
			&& g_StageNum == STAGE_CITRAINING && g_NetPlaylist.count > 0
			&& g_NetAdminController == NET_NULL_CLIENT) {
		// Gate the first-match start on min_humans_to_start. As soon as
		// enough clients are in the lobby, arm a 1-second grace so any
		// stragglers connecting in the same window land before the round
		// begins. If clients disconnect during grace, the grace resets.
		// Re-arms automatically whenever we land in CITRAINING (post-vote
		// fallback path also returns here when humans drop below threshold).
		static u32 s_ded_armed_at = 0;
		const s32 needed = (s32)g_NetPlaylist.min_humans_to_start;
		if (humans_connected >= needed) {
			if (s_ded_armed_at == 0) {
				s_ded_armed_at = g_NetTick + 60u; // ~1 second grace
				sysLogPrintf(LOG_NOTE,
						"dedicated: %d/%d humans connected, starting match in 1s",
						humans_connected, needed);
			} else if (g_NetTick >= s_ded_armed_at) {
				struct playlistentry resolved;
				playlistResolveRandoms(&g_NetPlaylist.entries[0], &resolved);
				playlistApply(&resolved);
				sysLogPrintf(LOG_NOTE,
						"dedicated: auto-starting match `%s` stage=0x%02x scenario=%d bots=%d",
						resolved.name, (s32)resolved.stagenum, (s32)resolved.scenario,
						(s32)resolved.bot_count);
				mpStartMatch();
				s_ded_armed_at = 0; // re-arms on next CITRAINING entry
			}
		} else if (s_ded_armed_at != 0) {
			sysLogPrintf(LOG_NOTE, "dedicated: humans dropped below threshold, cancelling start");
			s_ded_armed_at = 0;
		}
	}

	// Vote machine: server side. When a match ends (g_MpSetup.paused becomes
	// MPPAUSEMODE_GAMEOVER while we're still in CLSTATE_GAME on the host),
	// open the vote. When the deadline elapses, close + apply + advance.
	// Skipped if no playlist configured (then operator drives /nextmap).
	{
		static u8  s_vote_seen_gameover = 0;
		static u32 s_vote_apply_at = 0;
		if (g_NetMode == NETMODE_SERVER && g_NetPlaylist.count > 0
				&& g_StageNum != STAGE_CITRAINING
				&& g_NetAdminController == NET_NULL_CLIENT) {
			if (g_MpSetup.paused == MPPAUSEMODE_GAMEOVER) {
				if (!s_vote_seen_gameover) {
					s_vote_seen_gameover = 1;
					s_vote_apply_at = 0;
					netServerVoteOpen();
				}
				if (g_NetVote.state == NETVOTE_OPEN && g_NetTick >= g_NetVote.deadline_tick) {
					netServerVoteClose();
				}
				if (g_NetVote.state == NETVOTE_RESULTS) {
					if (s_vote_apply_at == 0) {
						s_vote_apply_at = g_NetTick + 60u;
					}
					if (g_NetTick >= s_vote_apply_at) {
						const s32 needed = (s32)g_NetPlaylist.min_humans_to_start;
						if (g_NetDedicatedMode && humans_connected < needed) {
							// No humans left to play for — drop back to the
							// Combat Sim lobby. The dedicated auto-start gate
							// above will re-arm and wait for clients again.
							sysLogPrintf(LOG_NOTE,
									"dedicated: %d/%d humans after vote, returning to lobby",
									humans_connected, needed);
							mpSetPaused(MPPAUSEMODE_UNPAUSED);
							titleSetNextStage(STAGE_CITRAINING);
							titleSetNextMode(TITLEMODE_SKIP);
							mainChangeToStage(STAGE_CITRAINING);
						} else {
							mpStartMatch();
						}
						g_NetVote.state = NETVOTE_IDLE;
						s_vote_seen_gameover = 0;
						s_vote_apply_at = 0;
					}
				}
			} else {
				s_vote_seen_gameover = 0;
				s_vote_apply_at = 0;
			}
		}
	}

	// Lobby state: broadcast to waiting clients after the main send flush so
	// g_NetMsgRel is empty. Runs during lobby phase (g_NetLocalClient is
	// CLSTATE_LOBBY on the server) so there are no player-move messages to
	// clobber. Only sent every NET_HEARTBEAT_INTERVAL ticks when at least one
	// remote client is still in CLSTATE_LOBBY (skipped once all clients have
	// started the game). Phase-offset by half the interval so this doesn't
	// land on the same tick as the KoH keep-alive above.
	if (g_NetMode == NETMODE_SERVER
			&& (g_NetTick % NET_HEARTBEAT_INTERVAL) == (NET_HEARTBEAT_INTERVAL / 2u)) {
		for (s32 _li = 0; _li < g_NetMaxClients; _li++) {
			if (g_NetClients[_li].state == CLSTATE_LOBBY
					&& g_NetClients[_li].peer != NULL) {
				netmsgSvcLobbyStateWrite(&g_NetMsgRel);
				netSend(NULL, &g_NetMsgRel, true, NETCHAN_CONTROL);
				break;
			}
		}
	}
#endif

	// CSP: blend the local player toward the server-corrected position one
	// tick at a time, after all physics have run for this frame.
	if (g_NetMode == NETMODE_CLIENT) {
		netCspTick();
	}

	// Diagnostic dump of every player + sim position so the log can be
	// post-processed to find teleports, desync drift, or stuck sims.
	// Rate-limited so the file stays small. One dump per N ticks; default 6
	// ticks ≈ 10 Hz which is dense enough to spot teleports but not so chatty
	// that a 5-minute match produces gigabytes.
	if (g_NetDiagFile && g_NetDiagDumpRate > 0 && (g_NetTick % g_NetDiagDumpRate) == 0) {
		for (s32 i = 0; i < g_NetMaxClients; ++i) {
			const struct netclient *cl = &g_NetClients[i];
			if (cl->state < CLSTATE_GAME || !cl->player || !cl->player->prop) {
				continue;
			}
			const struct coord *p = &cl->player->prop->pos;
			const u32 ping = cl->peer ? enet_peer_get_rtt(cl->peer) : 0;
			netDiagLogf("pos_cl", "id=%u name=%s x=%.1f y=%.1f z=%.1f ping=%u theta=%.2f verta=%.2f",
				cl->id, cl->settings.name, p->x, p->y, p->z, ping,
				cl->player->vv_theta, cl->player->vv_verta);
		}
#ifndef PLATFORM_N64
		if (g_Vars.lvmpbotlevel) {
			for (s32 i = 0; i < g_BotCount; ++i) {
				const struct chrdata *chr = g_MpBotChrPtrs[i];
				if (!chr || !chr->prop) {
					continue;
				}
				const struct coord *p = &chr->prop->pos;
				// yrot = body facing direction (radians). For sim debugging
				// it lets you cross-reference the orientation broadcast in
				// SVC_PROP_MOVE's chr-state block against what the server
				// thought the bot was doing — useful when chasing "sim
				// facing the wrong way after respawn" or strafe-related
				// glitches. Speed prints the anim cycle playback rate set
				// by playerChooseThirdPersonAnimation so you can correlate
				// stuck/slow anims with the bot's actual movement.
				const f32 yrot = chrGetRotY((struct chrdata *)chr);
				const s16 animnum = (chr->model && chr->model->anim) ? chr->model->anim->animnum : 0;
				const f32 animspeed = (chr->model && chr->model->anim) ? chr->model->anim->speed : 0.f;
				netDiagLogf("pos_sim", "id=%d sid=%u x=%.1f y=%.1f z=%.1f yrot=%.3f anim=%d aspd=%.2f act=%d hp=%.0f",
					i, chr->prop->syncid, p->x, p->y, p->z,
					yrot, (s32)animnum, animspeed,
					chr->actiontype, chr->maxdamage - chr->damage);
			}
		}
#endif
	}

	enet_host_flush(g_NetHost);

	// netEndFrame complete. If ne_enter fired but ne_exit didn't, the
	// crash is in the move/send/CSP/diag block between them. Capped at
	// 30 to match the other bracket logs.
	static u32 ne_exit_count = 0;
	if (ne_exit_count < 30u) {
		netDiagLogf("ne_exit", "tick=%u", g_NetTick);
		ne_exit_count++;
	}
}

u32 netSend(struct netclient *dstcl, struct netbuf *buf, const s32 reliable, const s32 chan)
{
	if (g_NetMode == NETMODE_CLIENT) {
		dstcl = g_NetLocalClient;
	}

	if (buf == NULL) {
		if (dstcl) {
			buf = &dstcl->out;
		} else {
			buf = reliable ? &g_NetMsgRel : &g_NetMsg;
		}
	}

	if (reliable || !g_NetSimPacketLoss || (rand() % g_NetSimPacketLoss) == 0) {
		const u32 flags = (reliable ? ENET_PACKET_FLAG_RELIABLE : 0);
		ENetPacket *p = enet_packet_create(buf->data, buf->wp, flags);
		if (!p) {
			sysLogPrintf(LOG_ERROR, "NET: could not alloc %u bytes for packet", buf->wp);
			return 0;
		}

		if (g_NetSimLagMs > 0) {
			// Hold the packet for the requested delay before letting ENet see
			// it. We still create it now so the source buffer can be reused
			// immediately (ENet copies the data on create).
			netLagQueuePush(dstcl ? dstcl->peer : NULL, chan, p, (u32)g_NetSimLagMs);
		} else if (dstcl == NULL) {
			enet_host_broadcast(g_NetHost, chan, p);
		} else {
			enet_peer_send(dstcl->peer, chan, p);
		}
	}

	const u32 ret = buf->wp;

	netbufStartWrite(buf);

	return ret;
}

void netPlayersAllocate(void)
{
	s32 playernum = 0;

	if (g_NetMode == NETMODE_CLIENT) {
		// we always put the local player at index 0, even client-side
		// which means that clientside we have to put the server's player into our slot.
		// Skip the swap if either the local client or the host (g_NetClients[0])
		// is a spectator. The local-spectator case has no slot to swap into. The
		// host-spectator case is different: the host has no playernum (sentinel
		// 0xFE) and the local client is already at slot 0 on the wire because
		// netPlayersAllocate-on-server skipped the spectator host when assigning
		// sequential combatant playernums. Swapping would clobber g_NetClients[0]'s
		// sentinel with a valid slot index that doesn't match its (NULL) player.
		if (!g_NetLocalClient->is_spectator && !g_NetClients[0].is_spectator) {
			const s32 svplayernum = g_NetLocalClient->playernum;
			g_NetLocalClient->playernum = 0;
			g_NetClients[0].playernum = svplayernum;

			// F2 body bits arrive wire-indexed (by the host's dense playernums). The
			// swap above moves the local client to slot 0 and the host to svplayernum,
			// so mirror that swap in g_NetCoopBodyBits — playerChooseBodyAndHead indexes
			// it by the LOCAL g_Vars.players[] slot, so without this the client reads the
			// wrong player's masculine choice (its own body ends up keyed to the host's).
			if (svplayernum > 0 && svplayernum < MAX_PLAYERS) {
				const u8 bit0 = (u8)((g_NetCoopBodyBits >> 0) & 1);
				const u8 bitsv = (u8)((g_NetCoopBodyBits >> svplayernum) & 1);
				g_NetCoopBodyBits &= (u8)~((1 << 0) | (1 << svplayernum));
				g_NetCoopBodyBits |= (u8)(bit0 << svplayernum);
				g_NetCoopBodyBits |= (u8)(bitsv << 0);
			}
		}
	}

	for (s32 i = 0; i < g_NetMaxClients; ++i) {
		struct netclient *cl = &g_NetClients[i];
		if (cl->state < CLSTATE_LOBBY) {
			continue;
		}

		// Spectator clients have no mpchr / no player config — skip slot
		// assignment entirely. They keep the sentinel playernum so any code
		// that indexes g_PlayerConfigsArray / g_Vars.players by playernum
		// blows up loudly instead of silently corrupting slot 0xFE.
		if (cl->is_spectator) {
			if (g_NetMode == NETMODE_SERVER) {
				cl->playernum = NET_PLAYERNUM_SPECTATOR;
			}
			cl->config = NULL;
			cl->player = NULL;
			continue;
		}

		if (g_NetMode == NETMODE_SERVER) {
			// on the server allocate players sequentially (spectators were
			// skipped above so playernum stays a dense [0..g_NetNumClients) range
			// of combatants only)
			cl->playernum = playernum++;
		}

		if (cl != g_NetLocalClient) {
			// disable controls for the remote pawns and set their settings
			// (the local profile this stomps is snapshotted in
			// g_NetLocalProfileBackup at session start and restored by the
			// local-bind branch below whenever we get seated on a slot)
			struct mpplayerconfig *cfg = &g_PlayerConfigsArray[cl->playernum];
			cfg->controlmode = CONTROLMODE_NA;
			cfg->base.mpbodynum = cl->settings.bodynum;
			cfg->base.mpheadnum = cl->settings.headnum;
			snprintf(cfg->base.name, sizeof(cfg->base.name), "%s\n", cl->settings.name);
			// take some of the options from our local player and others from the client
			// Source from the first combatant config (host slot may be unused
			// in spectator mode, but config index 0 is still safe to read).
			cfg->options = g_PlayerConfigsArray[0].options & OPTION_PAINTBALL;
			cfg->options |= cl->settings.options & ~OPTION_PAINTBALL;
			// don't enable toggle aim, invert pitch or lookahead for remote players
			cfg->options &= ~(OPTION_AIMCONTROL | OPTION_LOOKAHEAD);
			cfg->options |= OPTION_FORWARDPITCH | OPTION_ASKEDSAVEPLAYER;
		} else {
			// Restore the LOCAL profile into whatever slot we bind. The slot
			// may have belonged to a REMOTE player last round (we were a JIP
			// spectator, so the branch above stomped every combatant config,
			// ours included) — inheriting it left CONTROLMODE_NA on our own
			// pawn, which kills all input INCLUDING the pause menu (the
			// bondmove gate early-returns before ESC/START handling), and
			// wore the remote player's name/body/options. base.team is left
			// alone — the stage-start manifest just assigned it.
			netRestoreLocalProfile(&g_PlayerConfigsArray[cl->playernum]);
		}

		cl->config = &g_PlayerConfigsArray[cl->playernum];
		cl->config->client = cl;
		cl->config->handicap = 0x80;
		// Combatants and panels live in disjoint g_Vars.players[] ranges on
		// the spectator host (combatants at [0..N-1] = cl->playernum, panels
		// at [N..N+P-1]) so cl->player binds straight to its combatant slot
		// without colliding with a panel. spectatorAllocatePanels runs after
		// this and tags the high slots.
		cl->player = g_Vars.players[cl->playernum];
		if (cl->player) {
			cl->player->client = cl;
			cl->player->isremote = (cl != g_NetLocalClient);
		}
	}

	// F2 body type: resolve the per-player masculine bitmask now — after playernums
	// are assigned (loop above) but BEFORE the chrbody models are built in the player
	// tick. The host reads each client's synced choice (settings.coopbodytype) keyed
	// by its playernum; COOPBODY_RANDOM is rolled here (cosmetic RNG, so the result
	// ships without touching the gameplay seed). The client keeps the value it read
	// from SVC_STAGE_START. Resolving here (not in the SVC_STAGE_START write) is what
	// makes the third-person chrbody pick up the right body, not just the first-person
	// hands (which re-derive every frame and so updated even when the bits landed late).
	if (g_NetMode == NETMODE_SERVER && g_Vars.coopplayernum >= 0) {
		if (g_NetLocalClient) {
			g_NetLocalClient->settings.coopbodytype = (u8)g_NetCoopBodyMode;
		}
		g_NetCoopBodyBits = 0;
		for (s32 bi = 0; bi < g_NetMaxClients; bi++) {
			struct netclient *bcl = &g_NetClients[bi];
			if (bcl->state < CLSTATE_LOBBY || bcl->is_spectator || bcl->playernum >= MAX_PLAYERS) {
				continue;
			}
			const u8 mode = bcl->settings.coopbodytype;
			if (mode == COOPBODY_MASCULINE
					|| (mode == COOPBODY_RANDOM && (rngCosmeticRandom() & 1))) {
				g_NetCoopBodyBits |= (u8)(1 << bcl->playernum);
			}
		}
	}
}

void netSyncIdsAllocate(void)
{
	// allocate sync ids sequentially for all active or paused props
	g_NetNextSyncId = 1;

	// don't allocate anything else if we're in lobby
	if (g_StageNum == STAGE_TITLE || g_StageNum == STAGE_CITRAINING) {
		g_NetFirstDynamicSyncId = g_NetNextSyncId;
		return;
	}

	// Co-op drop-in: park the pre-allocated slots that have no client seated
	// (netPlayersAllocate just ran, so the bindings — identical on every
	// machine — are in place, and the player props exist).
	netCoopDormantInit();

	// iterate active props first
	struct prop *prop = g_Vars.activeprops;
	while (prop && prop != g_Vars.pausedprops) {
		prop->syncid = prop - g_Vars.props + 1;
		if (prop->syncid > g_NetNextSyncId) {
			g_NetNextSyncId = prop->syncid;
		}
		prop = prop->next;
	}

	// then the paused props
	prop = g_Vars.pausedprops;
	while (prop) {
		prop->syncid = prop - g_Vars.props + 1;
		if (prop->syncid > g_NetNextSyncId) {
			g_NetNextSyncId = prop->syncid;
		}
		prop = prop->next;
	}

	// HACK: when we're a client, we'll need to swap our player and server player's props
	// because of what we do in netPlayersAllocate
	if (g_NetMode == NETMODE_CLIENT) {
		// The stage world is built and syncids are assigned — tell the server
		// targeted state can be applied now. For a JIP joiner this triggers
		// the catch-up snapshot (netmsgClcStageReadyRead); harmless no-op for
		// everyone else. Rides g_NetMsgRel, flushed by this frame's
		// netEndFrame. Must run BEFORE the spectator early-return below —
		// JIP joiners ARE spectators and are the whole point.
		netbufWriteU8(&g_NetMsgRel, CLC_STAGE_READY);

		// JIP-as-spectator: the local client connected mid-match and was
		// flagged is_spectator on the server. They have no player / no prop
		// of their own (won't until mpStartMatch unspectates them next
		// round). Skip the prop-existence check and the swap — both are
		// no-ops for a spectator.
		if (g_NetLocalClient->is_spectator) {
			return;
		}
		if (!g_NetLocalClient->player || !g_NetLocalClient->player->prop) {
			sysLogPrintf(LOG_ERROR, "NET: no props allocated for players?");
			netDisconnect();
			return;
		}
		// Skip the swap when the host is a spectator — they have no prop on
		// the wire, so g_NetClients[0].player is NULL and there's nothing to
		// swap with. The local client is already at slot 0 in this case
		// (netPlayersAllocate doesn't remap it).
		if (g_NetClients[0].player && g_NetClients[0].player->prop) {
			const u16 sid = g_NetClients[0].player->prop->syncid;
			g_NetClients[0].player->prop->syncid = g_NetLocalClient->player->prop->syncid;
			g_NetLocalClient->player->prop->syncid = sid;
		}
	}

	// g_NetNextSyncId now holds the highest syncid assigned above. propAllocate
	// does syncid = g_NetNextSyncId++ (post-increment), so without this bump the
	// first dynamic allocation would get the same syncid as the highest static
	// prop — a collision that sends two props with the same syncid to clients.
	g_NetNextSyncId++;

	// Remember the static/dynamic boundary for the JIP catch-up snapshot:
	// syncid >= this means the prop was spawned at runtime, so a mid-match
	// joiner's fresh stage load won't have it and needs a replayed spawn.
	g_NetFirstDynamicSyncId = g_NetNextSyncId;

	sysLogPrintf(LOG_NOTE, "NET: last initial syncid: %u, next dynamic: %u",
			g_NetNextSyncId - 1, g_NetNextSyncId);
}

// --- Entity interpolation clock ---

void netUpdateInterpLag(struct netclient *cl, u32 snaptick)
{
	if (!cl || !snaptick) {
		return;
	}

	// How stale this snapshot is in our local clock domain. For a client
	// viewing a remote player this is roughly the full path (~2x one-way
	// latency) because the snapshot carries the sender's clock and our
	// g_NetTick was only ever baselined to the server's clock at stage start.
	// Clamp at 0 in case clock drift briefly makes a snapshot look "future".
	const f32 raw = (g_NetTick > snaptick) ? (f32)(g_NetTick - snaptick) : 0.f;

	// Peak-hold with slow decay = self-sizing jitter buffer. Rise instantly to
	// the worst recent staleness so a late packet is already absorbed; fall back
	// slowly (~2% per snapshot) when the link improves so we don't over-tighten
	// and start starving. The interpolators add g_NetInterpTicks on top of this
	// as the steady-state margin behind the freshest snapshot.
	if (raw > cl->interp_lag) {
		cl->interp_lag = raw;
	} else {
		cl->interp_lag += (raw - cl->interp_lag) * 0.02f;
	}
}

// --- Client-side prediction ---

void netCspReconcile(u32 ack_tick, const struct coord *server_pos, f32 server_theta)
{
	if (!ack_tick) {
		return;
	}

	// Walk backwards through history looking for the snapshot we recorded at
	// ack_tick. ack_tick is the most recent client tick the server has received
	// and processed (read from SVC_PLAYER_MOVE's outmoveack field). It's at most
	// ~RTT-worth of ticks behind the newest CSP snapshot we've recorded, so
	// starting at head and walking back is the fastest lookup.
	for (s32 i = 0; i < NET_CSP_HISTORY_SIZE; ++i) {
		const s32 idx = (g_NetCspHead + NET_CSP_HISTORY_SIZE - i) % NET_CSP_HISTORY_SIZE;
		const struct csp_snapshot *snap = &g_NetCspHistory[idx];
		if (snap->tick != ack_tick) {
			continue;
		}

		const f32 ex = server_pos->x - snap->pos.x;
		const f32 ey = server_pos->y - snap->pos.y;
		const f32 ez = server_pos->z - snap->pos.z;
		const f32 err_sq = ex*ex + ey*ey + ez*ez;

		// Teleport threshold: divergence above this magnitude (~120 units) can't
		// result from player input alone — it indicates respawn, kill plane, or
		// network desync. Smooth-correcting a large error causes visible pinballing:
		// each fresh ack retargets the correction mid-smooth, so local pos zig-zags
		// between old and new targets. Instead, hard-snap and discard any pending
		// smooth correction (the pinballing at high ping is worse than one frame snap).
		//
		// chrSetPos (not a bare prop->pos write) is required: it re-derives ground
		// height and floor room from the new position and — critically for PLAYER
		// props — overwrites player->vv_manground / vv_ground / vv_theta. Without
		// that, the next bondmovePlayer tick sees the new pos but the OLD ground
		// reference and clamps the player back to the old floor, producing an
		// "I keep snapping but never sticking" loop visible in the diag log as
		// many csp_snap entries with growing err.
		//
		// We reuse the chr's current rooms array as the input to chrSetPos: it's
		// slightly stale (the chr hasn't physically moved yet) but cdFindGroundInfoAtCyl
		// walks the portal graph from there to find the right floor, which handles
		// snap distances up to a few hundred units. Theta from the wire is the
		// server's view of our look angle and is what chrSetPos expects (degrees).
		// findground=true so the chr's ground is actually re-derived (the whole
		// point of switching off the bare-write).
		if (err_sq > NET_CSP_TELEPORT_THRESH_SQ) {
			g_NetCspCorrFrames = 0;
			g_NetCspCorrDelta.x = 0.f;
			g_NetCspCorrDelta.y = 0.f;
			g_NetCspCorrDelta.z = 0.f;
			if (g_NetLocalClient && g_NetLocalClient->player && g_NetLocalClient->player->prop
					&& g_NetLocalClient->player->prop->chr) {
				struct chrdata *chr = g_NetLocalClient->player->prop->chr;
				struct coord snap_pos = *server_pos;
				chrSetPos(chr, &snap_pos, chr->prop->rooms, server_theta, true);
			}
			netDiagLogf("csp_snap", "ack=%u err=%.1f dx=%.1f dy=%.1f dz=%.1f",
				ack_tick, sqrtf(err_sq), ex, ey, ez);
			return;
		}

		if (err_sq > NET_CSP_CORR_THRESH_SQ) {
			// Smooth correction: retarget to the freshest server error and
			// restart the smoothing window. Earlier approaches tried history-shift
			// ("input replay") and error-magnitude-scaled windows — both reverted
			// for causing exponential teleporting at high ping:
			// - History shift: modifying entries from ack_tick forward desync the
			//   next ack comparison, triggering another shift, another snap, etc.
			// - Variable window: large errors snap aggressively in 2–5 frames,
			//   creating fast-motion "teleport" feel instead of smooth correction.
			// Simple retarget+fixed-window is stable: each ack moves us toward
			// the server position over NET_CSP_CORR_FRAMES (~10 ticks), converging
			// smoothly even at high latency.
			g_NetCspCorrDelta.x = ex;
			g_NetCspCorrDelta.y = ey;
			g_NetCspCorrDelta.z = ez;
			g_NetCspCorrFrames = NET_CSP_CORR_FRAMES;
			netDiagLogf("csp_recon", "ack=%u err=%.1f dx=%.1f dy=%.1f dz=%.1f",
				ack_tick, sqrtf(err_sq), ex, ey, ez);
		}
		return;
	}
}

void netCspTick(void)
{
	if (g_NetCspCorrFrames <= 0) {
		return;
	}
	if (!g_NetLocalClient || !g_NetLocalClient->player || !g_NetLocalClient->player->prop) {
		g_NetCspCorrFrames = 0;
		return;
	}

	// Apply 1/N of the remaining delta and then scale the delta down by
	// (N-1)/N. Because frames_remaining is also decremented each tick, the
	// recomputed step (1 / new frames_remaining) cancels out and the actual
	// amount applied per tick is constant — delta_orig / initial_frames each
	// time. e.g. delta_orig=100 over 10 frames adds 10/frame for 10 frames.
	//
	// We store it as a shrinking delta rather than a fixed per-frame amount
	// because netCspReconcile may retarget mid-correction: a fresh server ack
	// just replaces delta and resets frames_remaining, and the math keeps
	// converging on the new target without bookkeeping the leftover from the
	// previous correction.
	const f32 step = 1.f / (f32)g_NetCspCorrFrames;
	struct coord *pos = &g_NetLocalClient->player->prop->pos;
	pos->x += g_NetCspCorrDelta.x * step;
	pos->y += g_NetCspCorrDelta.y * step;
	pos->z += g_NetCspCorrDelta.z * step;

	g_NetCspCorrDelta.x *= (1.f - step);
	g_NetCspCorrDelta.y *= (1.f - step);
	g_NetCspCorrDelta.z *= (1.f - step);
	--g_NetCspCorrFrames;
}

// --- Lag compensation ---

void netLagCompSave(struct netclient *cl)
{
	if (!cl || !cl->player || !cl->player->prop) {
		return;
	}
	// Record the client's position each frame into a ring buffer. This gives us
	// a history of positions to rewind to when running hit tests. Called once per
	// client per frame in netEndFrame so we have snapshots of every client's
	// authoritative position throughout the game.
	cl->lagcomp_head = (cl->lagcomp_head + 1) % NET_LAGCOMP_SIZE;
	cl->lagcomp[cl->lagcomp_head].tick = g_NetTick;
	cl->lagcomp[cl->lagcomp_head].pos  = cl->player->prop->pos;
}

void netServerRecordDetectedHit(struct netclient *cl, u16 syncid)
{
	if (!cl || !syncid) {
		return;
	}
	cl->srvhits_head = (cl->srvhits_head + 1) % NET_SRVHIT_COUNT;
	cl->srvhits[cl->srvhits_head].syncid = syncid;
	cl->srvhits[cl->srvhits_head].tick = g_NetTick;
}

s32 netServerHitWasDetected(const struct netclient *shooter, u16 syncid)
{
	if (!shooter || !syncid) {
		return 0;
	}
	// Accept a small backward window: the server's shot replay (which records the
	// detected hit) and the client's CLC_HIT can land a few ticks apart depending
	// on update rate and jitter. 12 ticks is generous to avoid false rejections.
	for (s32 i = 0; i < NET_SRVHIT_COUNT; ++i) {
		if (shooter->srvhits[i].syncid == syncid
				&& shooter->srvhits[i].tick != 0
				&& (g_NetTick - shooter->srvhits[i].tick) <= 12u) {
			return 1;
		}
	}
	return 0;
}

/* ---- network-chr position interpolation (sims now; co-op NPCs later) ---- */

// Smoothed average gap (in local ticks) between consecutive replicated-chr
// snapshots. Snapshots are stamped with the local receive tick, so their spacing
// = client_fps / server_update_hz (e.g. ~4 at 240fps vs a 60Hz server). The
// interp delay adapts to this so we usually have two snapshots bracketing the
// render target rather than constantly extrapolating. All sims share the server's
// update cadence, so a single global estimate suffices.
static f32 g_NetChrSnapInterval = 1.0f;

s32 g_NetChrInterp = 1; // /chrinterp toggle; 0 = old receive-time per-packet apply
s32 g_NetCoopChrLifecycle = 1; // /coopchr toggle; gates runtime co-op chr SPAWN + FREE replication (diagnostic isolation)
s32 g_NetCoopObjWireDriven = 1; // /coopobj toggle; ON (default) makes networked OBJ props wire-driven on clients so they stick to the host pos instead of drifting/floating (/coopobj off reverts)
s32 g_NetCoopHosting = 0;                  // host: server started for co-op (set by the co-op menu's Start Hosting)
s32 g_NetCoopBodyMode = COOPBODY_FEMININE; // F2 local player's choice; synced via CLC_SETTINGS
u8 g_NetCoopBodyBits = 0;                  // F2 resolved per-player masculine bitmask (host-assembled in SVC_STAGE_START write)
s32 g_NetCoopLivesMode = COOP_LIVES_OFF;    // F3 host setting, synced
s32 g_NetCoopLivesCount = 3;                // F3 lives per player (host setting, synced)
s32 g_NetCoopLives[MAX_PLAYERS] = {0};      // F3 per-player remaining (host-authoritative)
s32 g_NetCoopSharedLives = 0;               // F3 shared pool remaining (host-authoritative)

static f32 netLerpf(f32 a, f32 b, f32 t)
{
	if (!(a > -1.0e4f && a < 1.0e4f)) { // self-heal NaN/inf: snap to target
		return b;
	}
	return a + (b - a) * t;
}

static f32 netAngleLerp(f32 a, f32 b, f32 t)
{
	// Shortest-arc radian interpolation. Bounded-input guard keeps the wrap loops
	// finite and snaps on a garbage value rather than spinning.
	if (!(a > -100.f && a < 100.f && b > -100.f && b < 100.f)) {
		return b;
	}
	const f32 TWO_PI = 6.2831853071795865f;
	f32 d = b - a;
	while (d >  TWO_PI * 0.5f) d -= TWO_PI;
	while (d < -TWO_PI * 0.5f) d += TWO_PI;
	return a + d * t;
}

void netChrRecordSnapshot(struct chrdata *chr, const struct netchrpose *pose)
{
	if (!chr || !pose) {
		return;
	}
	// Corrupt-head recovery (see the invariant check in netChrInterpolate):
	// re-seat an out-of-range head so the `prev` read below can't index outside
	// the struct and the ring rebuilds with fresh snapshots.
	if (chr->netsnaphead >= NET_SNAPSHOT_COUNT) {
		chr->netsnaphead = 0;
	}
	const u32 prev = chr->netsnap[chr->netsnaphead].tick;
	if (prev && g_NetTick > prev) {
		const f32 gap = (f32)(g_NetTick - prev);
		if (gap < 60.f) { // ignore spawn / stall outliers
			g_NetChrSnapInterval += (gap - g_NetChrSnapInterval) * 0.1f;
		}
	}
	const u32 h = (chr->netsnaphead + 1) % NET_SNAPSHOT_COUNT;
	chr->netsnaphead = h;
	chr->netsnap[h].tick           = g_NetTick ? g_NetTick : 1u; // 0 == empty
	chr->netsnap[h].pos            = pose->pos;
	chr->netsnap[h].yrot           = pose->yrot;
	chr->netsnap[h].angleoffset    = pose->angleoffset;
	chr->netsnap[h].aimupback      = pose->aimupback;
	chr->netsnap[h].aimsideback    = pose->aimsideback;
	chr->netsnap[h].aimuplshoulder = pose->aimuplshoulder;
	chr->netsnap[h].aimuprshoulder = pose->aimuprshoulder;
	chr->netsnap[h].animnum        = pose->animnum;
	chr->netsnap[h].framea         = pose->framea;
	chr->netsnap[h].speed          = pose->speed;
	for (s32 ri = 0; ri < 8; ++ri) {
		chr->netsnap[h].rooms[ri] = pose->rooms[ri];
	}
}

// Same -1-terminated room compare as netmsg.c's propRoomsEqual (that one is static
// to netmsg.c). Used to skip the deregister/register churn when the time-aligned
// rooms haven't changed since last frame.
static s32 netChrRoomsEqual(const RoomNum *ra, const RoomNum *rb)
{
	for (s32 i = 0; i < 8; ++i) {
		if (ra[i] != rb[i]) {
			return 0;
		}
		if (ra[i] == -1) {
			break;
		}
	}
	return 1;
}

void netChrInterpolate(struct chrdata *chr)
{
	if (!g_NetChrInterp || g_NetMode != NETMODE_CLIENT || !chr || !chr->prop) {
		return;
	}

	const u32 head = chr->netsnaphead;
	// INVARIANT CHECK: netsnaphead is only ever advanced `% NET_SNAPSHOT_COUNT`
	// (netChrRecordSnapshot), so out-of-range means this chr's ring was never
	// initialised or its memory was trashed. The known source — the
	// port-appended netsnap/netsnaphead fields never being cleared by chrInit
	// over recycled stage-pool memory — is now fixed in chrInit (chr.c); this
	// stays as a cheap guard so any future corruption logs and skips instead of
	// indexing netsnap[garbage] (the original 0xc0000005 here) or silently
	// freezing.
	if (head >= NET_SNAPSHOT_COUNT) {
		static u32 s_corrupt_tick = 0xffffffffu;
		if (g_NetTick != s_corrupt_tick) {
			s_corrupt_tick = g_NetTick;
			sysLogPrintf(LOG_WARNING, "NET: netChrInterpolate: corrupt netsnaphead %u (chr syncid %u)",
					head, chr->prop->syncid);
		}
		return;
	}
	if (!chr->netsnap[head].tick) {
		return; // no snapshots yet — leave the receive-time pose in place
	}

	// Render in the past at the interp delay. Snapshots are stamped with the same
	// local g_NetTick clock we read here (arrival time), so no interp_lag
	// rebaseline is needed. delay = one measured snapshot interval (so two
	// snapshots normally bracket the target) + g_NetInterpTicks jitter margin
	// (/interp). Extrapolation below covers a late packet beyond that.
	u32 interval = (u32)(g_NetChrSnapInterval + 0.5f);
	if (interval < 1u) { interval = 1u; }
	const u32 delay = g_NetInterpTicks + interval;
	const u32 desired = (g_NetTick > delay) ? (g_NetTick - delay) : 0u;

	// Find the two snapshots bracketing `desired` (newest-first walk).
	s32 inewer = -1, iolder = -1;
	for (s32 i = 0; i < NET_SNAPSHOT_COUNT; ++i) {
		const s32 idx = (s32)((head + NET_SNAPSHOT_COUNT - (u32)i) % NET_SNAPSHOT_COUNT);
		if (!chr->netsnap[idx].tick) {
			break; // empty slot
		}
		if (chr->netsnap[idx].tick >= desired) {
			inewer = idx;
		} else {
			iolder = idx;
			break;
		}
	}

	struct netchrpose out;
	// Anim-switch hysteresis: true when we're confident enough in out.animnum to
	// switch the model to it. False when the two bracketing snapshots disagree on
	// the animnum — that happens when a firing bot hovers at the stand<->soft-turn
	// movement threshold and the server toggles its anim every snapshot, which
	// would otherwise flip-flop the client between a walk and a standstill. We then
	// hold the current anim (still updating its speed) until a change persists
	// across two snapshots, so a genuine transition is adopted within ~1 snapshot
	// but a 1-snapshot blip is ignored.
	bool animstable = true;

	if (inewer >= 0 && iolder >= 0) {
		// Normal case: interpolate the WHOLE pose between the bracketing snapshots,
		// so body, facing and aim all reconstruct for the same past instant.
		const u32 span = chr->netsnap[inewer].tick - chr->netsnap[iolder].tick;
		const f32 t = (span > 0) ? (f32)(desired - chr->netsnap[iolder].tick) / (f32)span : 1.f;
		out.pos.x          = netLerpf(chr->netsnap[iolder].pos.x, chr->netsnap[inewer].pos.x, t);
		out.pos.y          = netLerpf(chr->netsnap[iolder].pos.y, chr->netsnap[inewer].pos.y, t);
		out.pos.z          = netLerpf(chr->netsnap[iolder].pos.z, chr->netsnap[inewer].pos.z, t);
		out.yrot           = netAngleLerp(chr->netsnap[iolder].yrot, chr->netsnap[inewer].yrot, t);
		out.angleoffset    = netAngleLerp(chr->netsnap[iolder].angleoffset, chr->netsnap[inewer].angleoffset, t);
		out.aimupback      = netLerpf(chr->netsnap[iolder].aimupback, chr->netsnap[inewer].aimupback, t);
		out.aimsideback    = netLerpf(chr->netsnap[iolder].aimsideback, chr->netsnap[inewer].aimsideback, t);
		out.aimuplshoulder = netLerpf(chr->netsnap[iolder].aimuplshoulder, chr->netsnap[inewer].aimuplshoulder, t);
		out.aimuprshoulder = netLerpf(chr->netsnap[iolder].aimuprshoulder, chr->netsnap[inewer].aimuprshoulder, t);
		// Anim: take the OLDER snapshot's discrete animnum/frame (the value in
		// effect at the instant we're rendering, [iolder, inewer)), and blend the
		// continuous playback speed. This time-aligns the legs with the body
		// position above — the whole point of the fix.
		out.animnum        = chr->netsnap[iolder].animnum;
		out.framea         = chr->netsnap[iolder].framea;
		out.speed          = netLerpf(chr->netsnap[iolder].speed, chr->netsnap[inewer].speed, t);
		// Only switch the discrete anim when both bracket snapshots agree (see
		// animstable above); otherwise hold the current anim through the toggle.
		animstable = (chr->netsnap[inewer].animnum == chr->netsnap[iolder].animnum);
	} else {
		// Single-snapshot / extrapolation: facing + aim hold the newest values;
		// position dead-reckons (bounded) when desired is ahead of all snapshots.
		const s32 src = (inewer >= 0) ? inewer : (s32)head;
		out.pos            = chr->netsnap[src].pos;
		out.yrot           = chr->netsnap[head].yrot;
		out.angleoffset    = chr->netsnap[head].angleoffset;
		out.aimupback      = chr->netsnap[head].aimupback;
		out.aimsideback    = chr->netsnap[head].aimsideback;
		out.aimuplshoulder = chr->netsnap[head].aimuplshoulder;
		out.aimuprshoulder = chr->netsnap[head].aimuprshoulder;
		out.animnum        = chr->netsnap[head].animnum;
		out.framea         = chr->netsnap[head].framea;
		out.speed          = chr->netsnap[head].speed;
		if (inewer < 0) {
			const u32 prevh = (head + NET_SNAPSHOT_COUNT - 1u) % NET_SNAPSHOT_COUNT;
			if (chr->netsnap[prevh].tick && chr->netsnap[head].tick > chr->netsnap[prevh].tick
					&& desired > chr->netsnap[head].tick) {
				u32 ahead = desired - chr->netsnap[head].tick;
				if (ahead > g_NetExtrapMaxTicks) {
					ahead = g_NetExtrapMaxTicks;
				}
				const f32 vscale = (f32)ahead / (f32)(chr->netsnap[head].tick - chr->netsnap[prevh].tick);
				out.pos.x = chr->netsnap[head].pos.x + (chr->netsnap[head].pos.x - chr->netsnap[prevh].pos.x) * vscale;
				out.pos.y = chr->netsnap[head].pos.y + (chr->netsnap[head].pos.y - chr->netsnap[prevh].pos.y) * vscale;
				out.pos.z = chr->netsnap[head].pos.z + (chr->netsnap[head].pos.z - chr->netsnap[prevh].pos.z) * vscale;
			}
		}
	}

	// Apply the reconstructed pose (overrides the receive-time per-packet apply).
	chr->prop->pos = out.pos;

	// TIME-ALIGNED ROOMS: re-register prop->rooms from the SAME past instant the body
	// is rendered at — the older bracket snapshot (matching the discrete animnum
	// chosen above), or the source snapshot in the single/extrapolation case. The
	// receive-time path (netmsgSvcPropMoveRead) deliberately skips registering rooms
	// for interpolated chrs precisely so this owns it: applying the CURRENT wire rooms
	// to a pos rendered ~interp-delay ticks in the past makes prop->rooms and prop->pos
	// disagree at room boundaries (ledge/doorway), misfiring func0f08e8ac's visibility
	// gate and room culling. The chosen snapshot always has tick != 0 (the function
	// early-returns when head is empty, and iolder/inewer only index recorded slots).
	{
		const s32 roomidx = (iolder >= 0) ? iolder : (inewer >= 0 ? inewer : (s32)head);
		RoomNum *wantrooms = chr->netsnap[roomidx].rooms;
		if (!netChrRoomsEqual(wantrooms, chr->prop->rooms)) {
			if (chr->prop->active) {
				propDeregisterRooms(chr->prop);
			}
			roomsCopy(wantrooms, chr->prop->rooms);
			if (chr->prop->active) {
				propRegisterRooms(chr->prop);
			}
		}
	}

	if (chr->model) {
		modelSetRootPosition(chr->model, &out.pos);
		modelSetChrRotY(chr->model, out.yrot);
	}
	chrSetRotY(chr, out.yrot);
	if (chr->aibot) {
		chr->aibot->angleoffset = out.angleoffset;
	}
	chr->aimupback      = out.aimupback;
	chr->aimsideback    = out.aimsideback;
	chr->aimuplshoulder = out.aimuplshoulder;
	chr->aimuprshoulder = out.aimuprshoulder;
	chr->aimendback     = out.aimupback;
	chr->aimendsideback = out.aimsideback;
	chr->aimendlshoulder = out.aimuplshoulder;
	chr->aimendrshoulder = out.aimuprshoulder;
	chr->aimendcount = 0;

	// ANIMATION (time-aligned with the interpolated body above). The leg/body anim
	// is reconstructed for the SAME past instant as the position, instead of being
	// applied at receive time (current) in netmsgSvcPropMoveRead. That removes the
	// time-domain mismatch that froze the legs mid-stride under a still-gliding body
	// when a bot decelerated to fire: the server transmits a near-zero anim speed
	// during deceleration (playerChooseThirdPersonAnimation's soft-turn band), and
	// the old receive-time apply pinned the legs to that CURRENT ~0 speed while the
	// body rendered a DELAYED, still-moving position. Now both come from the same
	// snapshot instant, so they always agree. The client free-runs the frame via its
	// own chrTick at this speed. The receive-time apply in netmsgSvcPropMoveRead is
	// the fallback (when /chrinterp is off, or before any snapshot exists — this
	// whole function early-returns in those cases).
	if (out.animnum > 0 && animHasFrames(out.animnum) && chr->model && chr->model->anim) {
		// Lock right-handed: the flip bit is deliberately not synced (it broke
		// Skedar maps — see netmsgSvcPropMoveWrite's FLIP comment).
		chr->model->anim->flip = 0;
		// Adopt the new anim when hysteresis confirms it, OR unconditionally for a
		// freshly-spawned sim that has no anim yet (animnum 0) so it can't get stuck
		// pose-less waiting for two agreeing snapshots.
		if (chr->model->anim->animnum != out.animnum
				&& (animstable || chr->model->anim->animnum == 0)) {
			// animnum change (confirmed by hysteresis): seed the new anim near the
			// server's frame at this instant and cross-fade the changeover. Merge time
			// 16 matches the host's chr transitions, including bot locomotion
			// (playerChooseThirdPersonAnimation, player.c:6186), so sim anim switches
			// fade in/out like the base game instead of popping (0.0625 was ~256x too
			// short — effectively instant).
			modelSetAnimation(chr->model, out.animnum, 0, (f32)out.framea, out.speed, 16.0f);
		} else {
			// Same anim, OR a not-yet-confirmed switch we're holding through a
			// stand<->walk toggle: just track the playback speed and let the frame
			// free-run. Re-seeding framea here would snap the cycle backward
			// whenever the server's frame index trailed ours.
			chr->model->anim->speed = out.speed;
		}
	}
}

static struct coord netLagCompLookup(const struct netclient *cl, u32 target_tick)
{
	// Retrieve the position snapshot at or just before the requested tick.
	// Walk backwards from the most-recent snapshot (head) since target_tick is
	// typically close to the current tick.
	for (s32 i = 0; i < NET_LAGCOMP_SIZE; ++i) {
		const s32 idx = (cl->lagcomp_head + NET_LAGCOMP_SIZE - i) % NET_LAGCOMP_SIZE;
		if (cl->lagcomp[idx].tick && cl->lagcomp[idx].tick <= target_tick) {
			return cl->lagcomp[idx].pos;
		}
	}
	// Fallback on buffer underflow (early frames before history fills up):
	// use the oldest position we have. Not ideal, but better than uninitialized.
	return cl->lagcomp[(cl->lagcomp_head + 1) % NET_LAGCOMP_SIZE].pos;
}

void netLagCompBegin(const struct netclient *shooter)
{
	g_LagCompCount = 0;

	if (!shooter || !shooter->peer) {
		return;
	}

	// Rewind remote players to where they were when the shooter fired, so
	// hit-tests reflect what the shooter saw on their screen rather than the
	// current server-authoritative pose. Two ways to pick the rewind target tick:
	// the exact path (default, proto 63) uses the shooter's own clock + render
	// offset off the wire; the legacy path estimates it from RTT/2 + interp delay.
	const u32 rtt_ms = enet_peer_get_rtt(shooter->peer);
	u32 rewind_ticks;
	u32 target_tick;

	if (g_NetLagCompExact && shooter->inmovetick) {
		// EXACT rewind (proto 63). The shooter stamped its firing move with its own
		// net clock (inmovetick) and told us how far behind that clock it renders
		// other entities (renderbehind = its g_NetInterpTicks). The server-tick it
		// was actually displaying targets at when it fired is therefore
		// inmovetick - renderbehind, which indexes our lagcomp ring directly (same
		// server-tick epoch — a client's g_NetTick is baselined to the server's).
		// This needs no latency estimate: g_NetTick - inmovetick already IS the true
		// upstream staleness, and renderbehind is the client's real render-behind, so
		// there's no symmetric-RTT or interp_lag-proxy assumption. Falls back to the
		// legacy estimate below only when the shooter has no applied move yet.
		target_tick = (shooter->inmovetick > shooter->renderbehind)
			? (shooter->inmovetick - shooter->renderbehind) : 0;
		rewind_ticks = (g_NetTick > target_tick) ? (g_NetTick - target_tick) : 0;
		netDiagLogf("lagcomp", "shooter=%u mode=exact rtt=%u rb=%u rewind_ticks=%u target=%u",
			shooter->id, rtt_ms, (u32)shooter->renderbehind, rewind_ticks, target_tick);
	} else {
		// LEGACY estimate (/lagcomp legacy, or no inmove yet): RTT/2 (network) plus
		// the shooter's interpolation delay (g_NetInterpTicks + measured interp_lag).
		// Correct only under roughly symmetric latency — the exact path above removes
		// that assumption. NET_LAGCOMP_SIZE (120 ticks / 2 s) covers ~350ms either way.
		const u32 interp_ticks = g_NetInterpTicks + (u32)(shooter->interp_lag + 0.5f);
		rewind_ticks = (rtt_ms / 2 + 8) / 16 + interp_ticks;
		target_tick = (g_NetTick > rewind_ticks) ? (g_NetTick - rewind_ticks) : 0;
		netDiagLogf("lagcomp", "shooter=%u mode=legacy rtt=%u interp=%u rewind_ticks=%u",
			shooter->id, rtt_ms, interp_ticks, rewind_ticks);
	}

	g_LagCompLastRewindTicks = rewind_ticks;

	for (s32 i = 0; i < g_NetMaxClients; ++i) {
		struct netclient *cl = &g_NetClients[i];
		// Skip the shooter (their position is already correct) and the host
		// (g_NetLocalClient): the host runs at zero lag on the server, so their
		// current position IS the authoritative position — no rewind needed.
		// Rewinding them with a one-tick-stale lagcomp entry (or the zeroed
		// fallback on early frames) moves their hitbox to the wrong place.
		if (cl == shooter || cl == g_NetLocalClient || cl->state < CLSTATE_GAME || !cl->player || !cl->player->prop) {
			continue;
		}
		if (g_LagCompCount >= NET_MAX_CLIENTS) {
			break;
		}

		struct prop *prop = cl->player->prop;
		const struct coord lagged_pos = netLagCompLookup(cl, target_tick);

		// Save current state so netLagCompEnd can restore after the hit-test
		g_LagCompSaved[g_LagCompCount].cl  = cl;
		g_LagCompSaved[g_LagCompCount].pos = prop->pos;
		g_LagCompSaved[g_LagCompCount].has_rootmtx = 0;

		// Move the prop to the lagged position
		prop->pos = lagged_pos;

		// Patch only the root model matrix translation so the sphere broad-phase
		// check in chrTestHit uses the rewound position. Writing the whole
		// matrices array is unsafe — chr->model->matrices is allocated each frame
		// from the per-frame graphics heap (gfxAllocate) and may point to stale or
		// already-reused memory by the time shotCalculateHits runs, so writing
		// past matrix[0] risks corrupting vertex buffers or other heap allocations.
		// Narrow-phase hits (bone raycast) still test against the current frame's
		// matrices, so those remain server-authoritative only — a safer tradeoff.
		if (cl->player->prop->chr && cl->player->prop->chr->model) {
			Mtxf *rootmtx = modelGetRootMtx(cl->player->prop->chr->model);
			if (rootmtx) {
				g_LagCompSaved[g_LagCompCount].rootmtx_xyz[0] = rootmtx->m[3][0];
				g_LagCompSaved[g_LagCompCount].rootmtx_xyz[1] = rootmtx->m[3][1];
				g_LagCompSaved[g_LagCompCount].rootmtx_xyz[2] = rootmtx->m[3][2];
				rootmtx->m[3][0] = lagged_pos.x;
				rootmtx->m[3][1] = lagged_pos.y;
				rootmtx->m[3][2] = lagged_pos.z;
				g_LagCompSaved[g_LagCompCount].has_rootmtx = 1;
			}
		}

		++g_LagCompCount;
	}
}

void netLagCompEnd(void)
{
	// Capture for the F9 debug overlay before we zero the count.
	g_LagCompLastCount = g_LagCompCount;

	for (s32 i = 0; i < g_LagCompCount; ++i) {
		struct netclient *cl = g_LagCompSaved[i].cl;
		if (!cl || !cl->player || !cl->player->prop) {
			continue;
		}
		cl->player->prop->pos = g_LagCompSaved[i].pos;
		if (g_LagCompSaved[i].has_rootmtx && cl->player->prop->chr && cl->player->prop->chr->model) {
			Mtxf *rootmtx = modelGetRootMtx(cl->player->prop->chr->model);
			if (rootmtx) {
				rootmtx->m[3][0] = g_LagCompSaved[i].rootmtx_xyz[0];
				rootmtx->m[3][1] = g_LagCompSaved[i].rootmtx_xyz[1];
				rootmtx->m[3][2] = g_LagCompSaved[i].rootmtx_xyz[2];
			}
		}
	}
	g_LagCompCount = 0;
}

void netChatPrintf(struct netclient *dst, const char *fmt, ...)
{
	char tmp[512];
	u8 bufdata[600];
	struct netbuf buf = { NULL };

	if (!g_NetMode || !g_NetLocalClient || g_NetLocalClient->state < CLSTATE_LOBBY) {
		return;
	}

	va_list args;
	va_start(args, fmt);
	vsnprintf(tmp, sizeof(tmp) - 1, fmt, args);
	va_end(args);

	buf.data = bufdata;
	buf.size = sizeof(bufdata);

	if (g_NetMode == NETMODE_SERVER) {
		sysLogPrintf(LOG_CHAT, "%s", tmp);
		netmsgSvcChatWrite(&buf, tmp);
	} else {
		netmsgClcChatWrite(&buf, tmp);
	}

	netSend(dst, &buf, true, NETCHAN_CONTROL);
}

void netChat(struct netclient *dst, const char *text)
{
	if (g_NetMode && g_NetLocalClient) {
		netChatPrintf(dst, "%s: %s", g_NetLocalClient->settings.name, text);
	}
}

// Build the ordered list of valid spectate targets: live remote players
// followed by live sims, in mpchr index order. Used by both the cycle and
// the safety check on resume. Returns the number filled in `out`; caller
// passes an array sized at least MAX_MPCHRS.
static s32 netSpectateGatherTargets(struct chrdata **out, s32 cap)
{
	s32 n = 0;
	if (!g_NetMode) {
		return 0;
	}
	const struct chrdata *localchr = (g_NetLocalClient && g_NetLocalClient->player && g_NetLocalClient->player->prop)
		? g_NetLocalClient->player->prop->chr : NULL;
	// JIP spectator (client with no own pawn): only PLAYER targets are
	// viewable — the lvRender redirect renders a player slot's viewport, but
	// the sim camera path overrides the LOCAL player's camera, which doesn't
	// exist. Players occupy mpchr 0..PLAYERCOUNT()-1.
	const bool playersonly = g_NetMode == NETMODE_CLIENT && g_NetLocalClient
		&& !g_NetLocalClient->player && g_NetLocalClient->is_spectator;
	// Humans first (mpchr 0..MAX_PLAYERS-1), then sims (>=MAX_PLAYERS). Skip
	// the local player and anything that's hidden / dead / unspawned.
	for (s32 i = 0; i < MAX_MPCHRS && n < cap; ++i) {
		struct chrdata *chr = g_MpAllChrPtrs[i];
		if (playersonly && i >= PLAYERCOUNT()) {
			break;
		}
		if (!chr || !chr->prop || chr == localchr) {
			continue;
		}
		if (chr->chrflags & CHRCFLAG_HIDDEN) {
			continue;
		}
		if (chrIsDead(chr)) {
			continue;
		}
		out[n++] = chr;
	}
	return n;
}

// Show or hide the local player's chr body for spectate mode. While
// spectating, the body is hidden so the player can spectate from any point
// (not just during the death animation). Cleared on spectate stop so the
// chr reappears when the player resumes normal play.
static void netSpectateHideLocal(bool hide)
{
	struct player *pl = g_NetLocalClient ? g_NetLocalClient->player : NULL;
	if (!pl || !pl->prop || !pl->prop->chr) {
		return;
	}
	struct chrdata *chr = pl->prop->chr;
	if (hide) {
		chr->chrflags |= CHRCFLAG_HIDDEN;
	} else {
		chr->chrflags &= ~CHRCFLAG_HIDDEN;
	}
}

void netSpectateStop(void)
{
	if (g_NetSpectateChr) {
		sysLogPrintf(LOG_CHAT, "NET: spectate off");
	}
	netSpectateHideLocal(false);
	g_NetSpectateChr = NULL;
}

// ---------- Vote helpers (port-only, dedicated/server side) ----------

// Server-side: build the ballot from the current playlist and broadcast
// SVC_VOTE_OPEN. The ballot includes vote_candidates entries; if the playlist
// has random_in_pool set and there's more than one candidate, the last slot
// becomes a RANDOM sentinel (playlist_index = -1) that resolves at apply
// time. Stages and scenarios on every other candidate are pre-resolved here
// so clients display concrete names; RANDOM picks resolve only at apply.
void netServerVoteOpen(void)
{
	if (g_NetMode != NETMODE_SERVER) return;
	if (g_NetVote.state == NETVOTE_OPEN) return;
	if (g_NetPlaylist.count == 0) return;

	const s32 n_req = g_NetPlaylist.vote_candidates
			? g_NetPlaylist.vote_candidates : 3;
	s32 n = n_req > NET_VOTE_MAX_CANDIDATES ? NET_VOTE_MAX_CANDIDATES : n_req;
	if (n > g_NetPlaylist.count + (g_NetPlaylist.random_in_pool ? 1 : 0)) {
		n = g_NetPlaylist.count + (g_NetPlaylist.random_in_pool ? 1 : 0);
	}
	if (n < 1) n = 1;

	// Seed from current tick so successive ballots aren't identical.
	u64 rng = ((u64)g_NetTick * 0x9E3779B97F4A7C15ULL) ^ sysGetMicroseconds();

	s8 picks[NET_VOTE_MAX_CANDIDATES];
	const s32 chosen = playlistPickBallot(&g_NetPlaylist, &rng, n, picks);
	if (chosen <= 0) return;

	g_NetVote.num_candidates = (u8)chosen;
	g_NetVote.vote_seconds = g_NetPlaylist.vote_seconds ? g_NetPlaylist.vote_seconds : 20;
	g_NetVote.deadline_tick = g_NetTick + (u32)g_NetVote.vote_seconds * 60u;
	g_NetVote.winning_index = 0;
	g_NetVote.winner_was_random = 0;
	for (s32 i = 0; i < NET_VOTE_MAX_CANDIDATES; ++i) {
		g_NetVote.tally[i] = 0;
	}
	for (s32 i = 0; i < (s32)(sizeof(g_NetVote.client_vote) / sizeof(g_NetVote.client_vote[0])); ++i) {
		g_NetVote.client_vote[i] = -1;
	}

	for (s32 i = 0; i < chosen; ++i) {
		struct netvotecandidate *c = &g_NetVote.candidates[i];
		c->playlist_index = picks[i];
		if (picks[i] < 0) {
			// RANDOM slot — fill with sentinel display info; the actual
			// pick happens at apply time so all clients see the same name
			// pre-resolve.
			c->stagenum = 0;
			c->scenario = 0;
			c->preset_index = 0xFF;
			c->bot_count = 0;
			c->timelimit = 0;
			c->scorelimit = 0;
			strncpy(c->name, "Random", sizeof(c->name) - 1);
			c->name[sizeof(c->name) - 1] = '\0';
		} else {
			struct playlistentry resolved;
			playlistResolveRandoms(&g_NetPlaylist.entries[picks[i]], &resolved);
			c->stagenum = (u8)resolved.stagenum;
			c->scenario = (u8)resolved.scenario;
			c->preset_index = (u8)(resolved.weaponpreset < 0 ? 0xFF : resolved.weaponpreset);
			c->bot_count = resolved.bot_count;
			c->timelimit = resolved.timelimit;
			c->scorelimit = resolved.scorelimit;
			strncpy(c->name, resolved.name, sizeof(c->name) - 1);
			c->name[sizeof(c->name) - 1] = '\0';
		}
	}

	g_NetVote.state = NETVOTE_OPEN;

	netbufStartWrite(&g_NetMsgRel);
	netmsgSvcVoteOpenWrite(&g_NetMsgRel);
	netSend(NULL, &g_NetMsgRel, true, NETCHAN_CONTROL);

	sysLogPrintf(LOG_CHAT, "VOTE: opened %d candidates, %ds deadline", chosen, (s32)g_NetVote.vote_seconds);
	netDiagLogf("vote_open", "candidates=%d secs=%d", chosen, (s32)g_NetVote.vote_seconds);
}

// Server-side: tally the votes, broadcast SVC_VOTE_RESULTS, apply the winner,
// and call mpStartMatch to begin the next round.
void netServerVoteClose(void)
{
	if (g_NetMode != NETMODE_SERVER) return;
	if (g_NetVote.state != NETVOTE_OPEN) return;

	// Tie-break: lowest index wins, except RANDOM (index N-1 with playlist
	// _index < 0) wins ties against itself so the outcome stays randomized.
	u8 best = 0;
	for (s32 i = 1; i < g_NetVote.num_candidates; ++i) {
		if (g_NetVote.tally[i] > g_NetVote.tally[best]) {
			best = (u8)i;
		}
	}
	g_NetVote.winning_index = best;
	g_NetVote.winner_was_random =
		(g_NetVote.candidates[best].playlist_index < 0) ? 1u : 0u;

	netbufStartWrite(&g_NetMsgRel);
	netmsgSvcVoteResultsWrite(&g_NetMsgRel);
	netSend(NULL, &g_NetMsgRel, true, NETCHAN_CONTROL);

	sysLogPrintf(LOG_CHAT, "VOTE: closed, winner [%d] %s (%d votes%s)",
			(s32)best, g_NetVote.candidates[best].name,
			(s32)g_NetVote.tally[best],
			g_NetVote.winner_was_random ? ", random" : "");
	netDiagLogf("vote_close", "winner=%d votes=%d random=%d",
			(s32)best, (s32)g_NetVote.tally[best],
			(s32)g_NetVote.winner_was_random);

	// Resolve and apply the winning entry. RANDOM slot: pick a fresh
	// playlist entry now (weighted) and resolve its random sub-fields.
	struct playlistentry resolved;
	const s8 pl_idx = g_NetVote.candidates[best].playlist_index;
	if (pl_idx < 0) {
		u64 rng = ((u64)g_NetTick * 0xBF58476D1CE4E5B9ULL) ^ sysGetMicroseconds();
		const s32 picked = playlistPick(&g_NetPlaylist, &rng);
		if (picked < 0) {
			sysLogPrintf(LOG_WARNING, "VOTE: RANDOM winner but playlist empty?");
			g_NetVote.state = NETVOTE_IDLE;
			return;
		}
		playlistResolveRandoms(&g_NetPlaylist.entries[picked], &resolved);
	} else {
		playlistResolveRandoms(&g_NetPlaylist.entries[pl_idx], &resolved);
	}
	playlistApply(&resolved);

	g_NetVote.state = NETVOTE_RESULTS;

	// Defer the actual mpStartMatch by a frame so SVC_VOTE_RESULTS lands
	// before the stage transition kicks in. Stash the trigger; the netEndFrame
	// poll will fire mpStartMatch when the grace tick elapses.
}

void netServerVoteRecord(struct netclient *cl, u8 candidate_index)
{
	if (g_NetMode != NETMODE_SERVER) return;
	if (g_NetVote.state != NETVOTE_OPEN) return;
	if (!cl || cl->id >= (sizeof(g_NetVote.client_vote) / sizeof(g_NetVote.client_vote[0]))) return;
	if (candidate_index >= g_NetVote.num_candidates && candidate_index != 0xFFu) return;

	// Undo previous vote if any.
	const s8 prev = g_NetVote.client_vote[cl->id];
	if (prev >= 0 && prev < g_NetVote.num_candidates && g_NetVote.tally[prev] > 0) {
		g_NetVote.tally[prev]--;
	}

	if (candidate_index == 0xFFu) {
		g_NetVote.client_vote[cl->id] = -1;
		return;
	}

	g_NetVote.client_vote[cl->id] = (s8)candidate_index;
	g_NetVote.tally[candidate_index]++;

	sysLogPrintf(LOG_CHAT, "VOTE: %s voted [%d] %s",
			cl->settings.name, (s32)candidate_index,
			g_NetVote.candidates[candidate_index].name);
}

s32 netClientVoteCast(s32 candidate_index)
{
	if (g_NetMode != NETMODE_CLIENT) return -1;
	if (g_NetVote.state != NETVOTE_OPEN) return -1;
	if (candidate_index < 0 || candidate_index >= g_NetVote.num_candidates) return -1;

	netbufStartWrite(&g_NetMsgRel);
	netmsgClcVoteWrite(&g_NetMsgRel, (u8)candidate_index);
	netSend(NULL, &g_NetMsgRel, true, NETCHAN_CONTROL);
	return 0;
}

void netSpectateCycle(s32 direction)
{
	struct chrdata *targets[MAX_MPCHRS];
	const s32 n = netSpectateGatherTargets(targets, ARRAYCOUNT(targets));
	if (n <= 0) {
		netSpectateStop();
		sysLogPrintf(LOG_CHAT, "NET: no valid spectate targets");
		return;
	}
	s32 current = -1;
	for (s32 i = 0; i < n; ++i) {
		if (targets[i] == g_NetSpectateChr) {
			current = i;
			break;
		}
	}
	s32 next;
	if (current < 0) {
		// Not currently spectating: start at first (next direction) or last (prev).
		next = (direction >= 0) ? 0 : (n - 1);
	} else {
		// Modular cycle; +n keeps it positive after subtracting 1.
		next = ((current + (direction >= 0 ? 1 : -1)) + n) % n;
	}
	g_NetSpectateChr = targets[next];
	const char *name = "?";
	if (g_NetSpectateChr) {
		const s32 mpidx = mpPlayerGetIndex(g_NetSpectateChr);
		if (mpidx >= 0 && mpidx < MAX_MPCHRS && g_MpAllChrConfigPtrs[mpidx]) {
			name = g_MpAllChrConfigPtrs[mpidx]->name;
		}
	}
	netSpectateHideLocal(true);
	sysLogPrintf(LOG_CHAT, "NET: spectating %s", name);
}

void netSpectateApply(void)
{
	if (!g_NetMode || !g_NetSpectateChr) {
		return;
	}
	// Validate the target by POINTER against the live mpchr list before we touch
	// it — g_NetSpectateChr can dangle (the target left, its chr was freed at
	// round-end, or a sim was removed). A chr still in the list is safe to read
	// (even if dead); a freed one is gone, so stop cleanly. This comparison
	// never dereferences the possibly-freed pointer.
	{
		bool present = false;
		for (s32 i = 0; i < MAX_MPCHRS; ++i) {
			if (g_MpAllChrPtrs[i] == g_NetSpectateChr) {
				present = true;
				break;
			}
		}
		if (!present) {
			netSpectateHideLocal(false);
			g_NetSpectateChr = NULL;
			return;
		}
	}
	// Target validation: hidden / despawned. Clear silently in those cases —
	// user can re-/spec to pick someone else. Dead targets are still valid
	// spectate subjects. Safe to dereference now: present in the mpchr list.
	struct chrdata *t = g_NetSpectateChr;
	if (!t->prop || (t->chrflags & CHRCFLAG_HIDDEN)) {
		netSpectateHideLocal(false);
		g_NetSpectateChr = NULL;
		return;
	}
	struct player *pl = g_NetLocalClient ? g_NetLocalClient->player : NULL;
	if (!pl || !pl->prop) {
		return;
	}

	// Override the CAMERA only — leave prop->pos alone so the corpse stays
	// where it died. The renderer's view matrix reads cam_pos / cam_look /
	// cam_up (set up at the top of playerAllocateMatrices), so populating
	// those is enough to move the viewpoint without dragging the body.
	// cam_room drives room visibility, which has to match the spectated
	// chr's location or rendering culls everything beyond the corpse's
	// rooms and you see geometry pop in.
	//
	// chrGetInverseTheta returns radians; convert to degrees-from-CCW for
	// vv_theta downstream. TWO_PI literal so this TU doesn't need to pull
	// in the game's math.h on top of <math.h>.
	const f32 TWO_PI = 6.2831853071795865f;
	// Sims: take yaw from the MODEL's rotation (modelGetChrRotY / chrinfo.yrot),
	// which Fix #2 syncs every SVC_PROP_MOVE via modelSetChrRotY and is exactly
	// what visibly turns. We can't use chrGetInverseTheta (returns aibot->lookangle,
	// never synced) NOR chrGetRotY (returns aibot->roty — the client's chrTick
	// recomputes it from movement, so it diverges from the synced model yaw; that's
	// why the camera stayed locked on clients while the model turned). On the host
	// the AI keeps both fields in step, which is why it looked fine there. lookangle
	// and roty are both assigned modelGetChrRotY server-side, so the TWO_PI - facing
	// convention is unchanged. Players keep chrGetInverseTheta — vv_theta is synced.
	const f32 facing = (t->prop->type == PROPTYPE_CHR && t->model)
		? modelGetChrRotY(t->model) : chrGetInverseTheta(t);
	const f32 thetaRad = TWO_PI - facing;

	// Pitch: ride the target's vertical look so spectating is true first-person
	// (up/down), not just yaw. Player targets sync vv_verta from SVC_PLAYER_MOVE
	// (bmoveProcessRemoteInput applies it on the client too); sims don't expose a
	// clean pitch, so they stay level — yaw-only reads fine for AI targets. If
	// pitch comes out inverted in testing, negate pitchDeg (vv_verta convention).
	f32 pitchDeg = 0.f;
	if (t->prop->type == PROPTYPE_PLAYER) {
		for (s32 pi = 0; pi < MAX_PLAYERS; ++pi) {
			if (g_Vars.players[pi] && g_Vars.players[pi]->prop == t->prop) {
				pitchDeg = g_Vars.players[pi]->vv_verta;
				break;
			}
		}
	}
	const f32 pitchRad = pitchDeg * TWO_PI / 360.0f;

	// For PROPTYPE_PLAYER, prop->pos.y is set by bondmovePlayer to
	// groundy + vv_eyeheight — already at eye level. A sim (PROPTYPE_CHR) has
	// prop->pos at the chr's CENTRE, so spectating it from there puts the camera
	// inside the body (the "torso" view). Nudge up to head height — same +50 the
	// host-spectator's spectatorTargetEyeAndForward uses, so /spec on a sim and
	// the host panel's sim view line up.
	struct coord eyepos = t->prop->pos;
	if (t->prop->type == PROPTYPE_CHR) {
		eyepos.y += 50.f;
	}

	// Look direction: forward vector from yaw + pitch. cam_up is world up.
	const f32 sinT = sinf(thetaRad);
	const f32 cosT = cosf(thetaRad);
	const f32 sinP = sinf(pitchRad);
	const f32 cosP = cosf(pitchRad);
	struct coord camlook = { -sinT * cosP, sinP, cosT * cosP };
	struct coord camup = { 0.f, 1.f, 0.f };

	// A sim's eye nudge above sits at the head's CENTRE, so the camera looks out
	// from inside the model — you see the inside of the face and the body clips in.
	// Push forward along the view to the eye/face surface so the head and body sit
	// behind the camera: first-person without clipping, and no model-hide needed
	// (CHRCFLAG_HIDDEN only freezes a chr's animation, it does NOT stop the draw).
	// ~28 units ~= head half-depth; small enough that the orbit as the bot turns is
	// unnoticeable. Players are already at true eye level, so they're left alone.
	if (t->prop->type == PROPTYPE_CHR) {
		eyepos.x += camlook.x * 28.f;
		eyepos.y += camlook.y * 28.f;
		eyepos.z += camlook.z * 28.f;
	}

	// Push into the camera. We have to call setCurrentPlayer because
	// playerSetCamProperties* writes to g_Vars.currentplayer, not the pl
	// pointer directly. Save/restore so we don't trample the caller's
	// notion of which player slot is "current" — lvTickPlayer is the one
	// who set currentplayer to the local pawn before invoking us, but the
	// general contract for this helper is "leave globals as you found them".
	const s32 prev = g_Vars.currentplayernum;
	setCurrentPlayerNum(g_NetLocalClient->playernum);
	const RoomNum camroom = t->prop->rooms[0];
	playerSetCamPropertiesWithRoom(&eyepos, &camup, &camlook, camroom);
	setCurrentPlayerNum(prev);

	// vv_theta / vv_verta drive any code that still reads "where is the
	// player facing" (HUD compass, third-person model orientation, etc.)
	// — sync them so those overlays match the spectated view direction.
	// vv_theta is degrees in the game's convention.
	pl->vv_theta = thetaRad * 360.0f / TWO_PI;
	pl->vv_verta = pitchDeg;
}

// Manual spectate toggle: enter spectate (first live target) if not currently
// spectating, otherwise return to first-person. For a key bind / console
// command so clients can watch others mid-match. No-op outside a net session.
void netSpectateToggle(void)
{
	if (!g_NetMode) {
		return;
	}
	if (g_NetSpectateChr) {
		netSpectateStop();
	} else {
		netSpectateCycle(+1);
	}
}

// Per-frame client hook: drive spectate automatically off the local player's
// death state. On the death transition (alive -> dead) we start spectating a
// live target; on the respawn transition (dead -> alive) we return to our own
// view. Manual /spec / netSpectateToggle still works between transitions —
// dying while manually spectating keeps the chosen target, and respawning
// always hands control back. Client-only; the host runs its own view.
void netSpectateAutoUpdate(void)
{
	static bool s_wasdead = false;

	static bool s_jipnotified = false;

	if (g_NetMode != NETMODE_CLIENT || !g_NetLocalClient) {
		s_wasdead = false;
		s_jipnotified = false;
		return;
	}

	if (!g_NetLocalClient->player) {
		// JIP mid-match joiner: no own pawn until the round boundary seats
		// us. Auto-engage the spectate redirect on the first live player
		// target so the wait is a proper first-person spectate instead of an
		// undefined view. Pre-check targets so an empty match (bots only /
		// everyone dead) doesn't log "no valid targets" every frame.
		// Combat Sim only (normmplayerisrunning): in co-op the g_MpAllChrPtrs
		// list the target gather walks is stale (mpStartMatch never ran), so
		// a co-op JIP spectator must not dereference it.
		if (g_NetLocalClient->is_spectator && !g_NetSpectateChr
				&& g_Vars.normmplayerisrunning) {
			struct chrdata *targets[MAX_MPCHRS];
			if (netSpectateGatherTargets(targets, ARRAYCOUNT(targets)) > 0) {
				netSpectateCycle(+1);
				if (!s_jipnotified) {
					s_jipnotified = true;
					sysLogPrintf(LOG_CHAT, "Joined mid-round - spectating until the next round starts");
				}
			}
		}
		s_wasdead = false;
		return;
	}

	s_jipnotified = false;

	const bool dead = (g_NetLocalClient->player->isdead != 0);

	if (dead && !s_wasdead) {
		// Just died — auto-spectate if we aren't already (manual target wins).
		if (!g_NetSpectateChr) {
			netSpectateCycle(+1); // picks first live target; no-op if none exist
		}
	} else if (!dead && s_wasdead) {
		// Just respawned — always hand the camera back to our own pawn.
		netSpectateStop();
	}

	s_wasdead = dead;
}

// Local-only console commands. Available any time the in-game chat console
// is open (~). Lines starting with '/' are routed here instead of being
// broadcast as chat. Each command prints feedback via sysLogPrintf so the
// result is visible in the console output area.
void netAdminReply(struct netclient *cl, const char *fmt, ...)
{
	char tmp[512];
	va_list args;
	va_start(args, fmt);
	vsnprintf(tmp, sizeof(tmp) - 1, fmt, args);
	va_end(args);
	tmp[sizeof(tmp) - 1] = '\0';

	// Local host admin, or a client with no live peer: log locally instead of
	// trying to send a packet to nobody.
	if (!cl || cl == g_NetLocalClient || !cl->peer) {
		sysLogPrintf(LOG_CHAT, "%s", tmp);
		return;
	}

	u8 bufdata[600];
	struct netbuf buf = { NULL };
	buf.data = bufdata;
	buf.size = sizeof(bufdata);
	netmsgSvcAdminWrite(&buf, tmp);
	netSend(cl, &buf, true, NETCHAN_CONTROL);
}

void netAdminPushStart(void)
{
	if (g_NetMode == NETMODE_CLIENT && g_NetLocalClient
			&& g_NetLocalClient->state >= CLSTATE_LOBBY) {
		netbufStartWrite(&g_NetMsgRel);
		netmsgClcAdminSetupWrite(&g_NetMsgRel);
		netSend(g_NetLocalClient, &g_NetMsgRel, true, NETCHAN_CONTROL);
		sysLogPrintf(LOG_CHAT, "admin: pushed match setup to server");
	} else if (g_NetMode == NETMODE_SERVER) {
		// Listen host: the config is already local, just start.
		mpStartMatch();
		g_NetVote.state = NETVOTE_IDLE;
	} else {
		sysLogPrintf(LOG_CHAT, "admin: not connected to a server");
	}
}

// Copy the next whitespace-delimited token of `p` into out[], returning a
// pointer just past it (ready for the following nextTok). out is always
// NUL-terminated. Used to parse admin `set` arguments.
static const char *nextTok(const char *p, char *out, s32 outsz)
{
	while (*p == ' ' || *p == '\t') { ++p; }
	s32 i = 0;
	while (*p && *p != ' ' && *p != '\t' && i < outsz - 1) { out[i++] = *p++; }
	out[i] = '\0';
	return p;
}

// Seed the admin scratch setup from the currently-running match config so
// `set` tweaks build on what's live. Called when an admin takes control.
static void netAdminCaptureSetup(void)
{
	struct playlistentry *e = &g_NetAdminSetup;
	memset(e, 0, sizeof(*e));
	e->stagenum = (s16)g_MpSetup.stagenum;
	e->scenario = (s8)g_MpSetup.scenario;
	e->weaponpreset = -1; // no preset -> keep current weapons unless `set preset`
	e->preset_name[0] = '\0';
	const u64 mask = playlistAllOptionBits();
	e->mp_options = g_MpSetup.options & mask;
	e->mp_options_mask = mask;
	e->scorelimit = g_MpSetup.scorelimit;
	e->timelimit = g_MpSetup.timelimit;
	e->teamscorelimit = g_MpSetup.teamscorelimit;
	e->bot_count = (u8)(g_BotCount > MAX_BOTS ? MAX_BOTS : g_BotCount);
	e->bot_difficulty = BOTDIFF_NORMAL;
	e->weight = 1;
	strcpy(e->name, "admin");
}

s32 netSecureStrEqual(const char *secret, const char *cand)
{
	if (!secret) secret = "";
	if (!cand) cand = "";
	const size_t slen = strlen(secret);
	const size_t clen = strlen(cand);
	u32 diff = (u32)(slen ^ clen);
	// Iterate over the secret's length (constant for a given server config),
	// not the candidate's, so a partial-prefix match doesn't shorten the loop.
	for (size_t i = 0; i < slen; ++i) {
		const u8 cc = (i < clen) ? (u8)cand[i] : 0;
		diff |= (u32)((u8)secret[i] ^ cc);
	}
	return diff == 0;
}

void netServerAdminCommand(struct netclient *cl, const char *line)
{
	if (g_NetMode != NETMODE_SERVER || !cl || !line) {
		return;
	}

	// Split into a lowercased command word + remainder, mirroring
	// netConsoleCommand's tokeniser.
	char cmd[24] = { 0 };
	const char *p = line;
	while (*p == ' ' || *p == '\t') { ++p; }
	s32 ci = 0;
	while (*p && *p != ' ' && *p != '\t' && ci < (s32)sizeof(cmd) - 1) {
		cmd[ci++] = (char)tolower((unsigned char)*p);
		++p;
	}
	cmd[ci] = '\0';
	while (*p == ' ' || *p == '\t') { ++p; }
	const char *arg = p; // remainder, may be ""

	// The local host client (id 0) is always an implicit admin.
	const s32 is_host = (cl->id == 0);
	const s32 authed = is_host || cl->is_admin;

	// `login` is the only command available before authentication.
	if (strcmp(cmd, "login") == 0) {
		if (g_NetAdminPassword[0] == '\0') {
			netAdminReply(cl, "admin: disabled (no Server.AdminPassword / --admin-password set)");
		} else if (netSecureStrEqual(g_NetAdminPassword, arg)) {
			cl->is_admin = 1;
			netAdminReply(cl, "admin: authenticated. type /admin help for commands.");
			sysLogPrintf(LOG_NOTE, "NET: client %u (%s) authenticated as admin", cl->id, cl->settings.name);
		} else {
			netAdminReply(cl, "admin: wrong password");
			sysLogPrintf(LOG_WARNING, "NET: client %u (%s) failed admin login", cl->id, cl->settings.name);
		}
		return;
	}

	if (!authed) {
		netAdminReply(cl, "admin: not authenticated (use: login <password>)");
		return;
	}

	if (cmd[0] == '\0' || strcmp(cmd, "help") == 0) {
		netAdminReply(cl, "admin commands:");
		netAdminReply(cl, "  login <pw>        authenticate as admin");
		netAdminReply(cl, "  take | release    take/release exclusive control");
		netAdminReply(cl, "  status            control + match state");
		netAdminReply(cl, "  endmatch          end current match, return to lobby");
		netAdminReply(cl, "  start [index]     start a playlist entry (random if omitted)");
		netAdminReply(cl, "  set <field> <val> edit scratch config (see: set help)");
		netAdminReply(cl, "  show              print the scratch config");
		netAdminReply(cl, "  apply             start a match from the scratch config");
		netAdminReply(cl, "  savepreset <name> save current weapons as a named preset");
		netAdminReply(cl, "  saverotation <nm> add scratch config to the live rotation");
		netAdminReply(cl, "  players           list connected clients");
		netAdminReply(cl, "  kick <name|id>    disconnect a client");
		netAdminReply(cl, "  say <message>     broadcast a server message");
		return;
	}

	if (strcmp(cmd, "status") == 0) {
		const u32 c = g_NetAdminController;
		if (c == NET_NULL_CLIENT) {
			netAdminReply(cl, "control: free");
		} else {
			const char *nm = (c < (u32)(NET_MAX_CLIENTS + 1)) ? g_NetClients[c].settings.name : "?";
			netAdminReply(cl, "control: held by client %u (%s)%s", c, nm, c == cl->id ? " (you)" : "");
		}
		s32 humans = 0;
		for (s32 i = 1; i < g_NetMaxClients; ++i) {
			if (g_NetClients[i].state >= CLSTATE_LOBBY) { ++humans; }
		}
		netAdminReply(cl, "%s stage=0x%02x clients=%d bots=%d",
				g_StageNum == STAGE_CITRAINING ? "lobby" : "match",
				(u32)g_StageNum, humans, (s32)g_BotCount);
		return;
	}

	if (strcmp(cmd, "take") == 0) {
		if (g_NetAdminController != NET_NULL_CLIENT && g_NetAdminController != cl->id) {
			netAdminReply(cl, "take: control already held by client %u", g_NetAdminController);
		} else {
			const s32 was_held = (g_NetAdminController == cl->id);
			g_NetAdminController = cl->id;
			if (!was_held) {
				netAdminCaptureSetup(); // seed scratch from the running match
			}
			netAdminReply(cl, "take: you control the server now (auto-rotation suspended)");
			sysLogPrintf(LOG_NOTE, "NET: client %u took admin control", cl->id);
		}
		return;
	}

	if (strcmp(cmd, "release") == 0) {
		if (g_NetAdminController != cl->id) {
			netAdminReply(cl, "release: you don't hold control");
		} else {
			g_NetAdminController = NET_NULL_CLIENT;
			netAdminReply(cl, "release: control released (auto-rotation resumed)");
			sysLogPrintf(LOG_NOTE, "NET: client %u released admin control", cl->id);
		}
		return;
	}

	// Match-control verbs require holding control so two admins can't fight.
	const s32 in_control = (g_NetAdminController == cl->id);

	if (strcmp(cmd, "endmatch") == 0) {
		if (!in_control) { netAdminReply(cl, "endmatch: take control first (take)"); return; }
		if (g_StageNum == STAGE_CITRAINING) {
			netAdminReply(cl, "endmatch: no match in progress");
		} else {
			netAdminReply(cl, "endmatch: ending current match");
			mainEndStage();
			// The vote/advance machine that normally returns the dedicated
			// server to the Combat Sim lobby after a match is suppressed while
			// an admin holds control (the g_NetAdminController gate ~line 1630),
			// so nothing would bring g_StageNum back to CITRAINING — and the
			// follow-up pushstart/go (gated on CITRAINING) would refuse with
			// "end the current match first". Drive the clean lobby return here,
			// mirroring the post-vote dedicated path.
			mpSetPaused(MPPAUSEMODE_UNPAUSED);
			titleSetNextStage(STAGE_CITRAINING);
			titleSetNextMode(TITLEMODE_SKIP);
			mainChangeToStage(STAGE_CITRAINING);
		}
		return;
	}

	if (strcmp(cmd, "start") == 0) {
		if (!in_control) { netAdminReply(cl, "start: take control first (take)"); return; }
		if (g_NetPlaylist.count == 0) { netAdminReply(cl, "start: playlist empty"); return; }
		s32 idx = -1;
		if (*arg) {
			idx = (s32)strtol(arg, NULL, 0);
			if (idx < 0 || idx >= g_NetPlaylist.count) {
				netAdminReply(cl, "start: index %d out of range (0..%d)", idx, (s32)g_NetPlaylist.count - 1);
				return;
			}
		} else {
			u64 rng = ((u64)g_NetTick * 0x9E3779B97F4A7C15ULL) ^ sysGetMicroseconds();
			idx = playlistPick(&g_NetPlaylist, &rng);
		}
		if (idx < 0) { netAdminReply(cl, "start: could not pick an entry"); return; }
		struct playlistentry resolved;
		playlistResolveRandoms(&g_NetPlaylist.entries[idx], &resolved);
		playlistApply(&resolved);
		netAdminReply(cl, "start: applying [%d] %s", idx, resolved.name);
		mpStartMatch();
		g_NetVote.state = NETVOTE_IDLE;
		return;
	}

	if (strcmp(cmd, "set") == 0) {
		if (!in_control) { netAdminReply(cl, "set: take control first (take)"); return; }
		char field[20] = { 0 }, v1[40] = { 0 }, v2[16] = { 0 };
		const char *q = nextTok(arg, field, sizeof field);
		q = nextTok(q, v1, sizeof v1);
		nextTok(q, v2, sizeof v2);
		for (char *fp = field; *fp; ++fp) { *fp = (char)tolower((unsigned char)*fp); }
		struct playlistentry *e = &g_NetAdminSetup;

		if (field[0] == '\0' || strcmp(field, "help") == 0) {
			netAdminReply(cl, "set fields: stage <name>, scenario <name>, timelimit <n>,");
			netAdminReply(cl, "  scorelimit <n>, teamscorelimit <n>, bots <n> [diff],");
			netAdminReply(cl, "  option <name> [on|off], preset <name>");
		} else if (strcmp(field, "stage") == 0) {
			const s32 id = playlistLookupStage(v1);
			if (id < 0) { netAdminReply(cl, "set stage: unknown stage `%s`", v1); }
			else { e->stagenum = (s16)id; netAdminReply(cl, "stage = %s", v1); }
		} else if (strcmp(field, "scenario") == 0) {
			const s32 id = playlistLookupScenario(v1);
			if (id < 0) { netAdminReply(cl, "set scenario: unknown scenario `%s`", v1); }
			else { e->scenario = (s8)id; netAdminReply(cl, "scenario = %s", v1); }
		} else if (strcmp(field, "timelimit") == 0) {
			s32 n = (s32)strtol(v1, NULL, 0); if (n < 0) n = 0; if (n > 255) n = 255;
			e->timelimit = (u8)n; netAdminReply(cl, "timelimit = %d", n);
		} else if (strcmp(field, "scorelimit") == 0) {
			s32 n = (s32)strtol(v1, NULL, 0); if (n < 0) n = 0; if (n > 255) n = 255;
			e->scorelimit = (u8)n; netAdminReply(cl, "scorelimit = %d", n);
		} else if (strcmp(field, "teamscorelimit") == 0) {
			s32 n = (s32)strtol(v1, NULL, 0); if (n < 0) n = 0; if (n > 65535) n = 65535;
			e->teamscorelimit = (u16)n; netAdminReply(cl, "teamscorelimit = %d", n);
		} else if (strcmp(field, "bots") == 0) {
			s32 n = (s32)strtol(v1, NULL, 0); if (n < 0) n = 0; if (n > MAX_BOTS) n = MAX_BOTS;
			e->bot_count = (u8)n;
			if (v2[0]) {
				const s32 d = playlistLookupBotDiff(v2);
				if (d < 0) { netAdminReply(cl, "set bots: unknown difficulty `%s`", v2); return; }
				e->bot_difficulty = (u8)d;
			}
			netAdminReply(cl, "bots = %d diff = %d", n, (s32)e->bot_difficulty);
		} else if (strcmp(field, "option") == 0) {
			const u64 bit = playlistLookupOption(v1);
			if (!bit) { netAdminReply(cl, "set option: unknown option `%s`", v1); return; }
			const s32 off = (strcasecmp(v2, "off") == 0 || strcmp(v2, "0") == 0
					|| strcasecmp(v2, "false") == 0 || strcasecmp(v2, "no") == 0);
			if (off) { e->mp_options &= ~bit; } else { e->mp_options |= bit; }
			e->mp_options_mask |= bit;
			netAdminReply(cl, "option %s = %s", v1, off ? "off" : "on");
		} else if (strcmp(field, "preset") == 0) {
			const s32 idx = mpWeaponPresetFind(v1);
			if (idx < 0) { netAdminReply(cl, "set preset: no preset named `%s`", v1); return; }
			e->weaponpreset = (s8)idx;
			strncpy(e->preset_name, v1, sizeof(e->preset_name) - 1);
			e->preset_name[sizeof(e->preset_name) - 1] = '\0';
			netAdminReply(cl, "preset = %s (#%d)", v1, idx);
		} else {
			netAdminReply(cl, "set: unknown field `%s` (try: set help)", field);
		}
		return;
	}

	if (strcmp(cmd, "show") == 0) {
		if (!in_control) { netAdminReply(cl, "show: take control first (take)"); return; }
		const struct playlistentry *e = &g_NetAdminSetup;
		netAdminReply(cl, "scratch: stage=0x%02x scenario=%d time=%d score=%d teamscore=%d",
				(u32)(u16)e->stagenum, (s32)e->scenario, (s32)e->timelimit,
				(s32)e->scorelimit, (s32)e->teamscorelimit);
		netAdminReply(cl, "  bots=%d diff=%d options=0x%016llx preset=%s",
				(s32)e->bot_count, (s32)e->bot_difficulty, (unsigned long long)e->mp_options,
				e->preset_name[0] ? e->preset_name : "(default weapons)");
		return;
	}

	if (strcmp(cmd, "apply") == 0) {
		if (!in_control) { netAdminReply(cl, "apply: take control first (take)"); return; }
		playlistApply(&g_NetAdminSetup);
		netAdminReply(cl, "apply: starting match from scratch config");
		mpStartMatch();
		g_NetVote.state = NETVOTE_IDLE;
		return;
	}

	if (strcmp(cmd, "savepreset") == 0) {
		if (!in_control) { netAdminReply(cl, "savepreset: take control first (take)"); return; }
		if (!*arg) { netAdminReply(cl, "usage: savepreset <name>"); return; }
		// Captures the *active* match weapons (g_MpSetup.weapons), so this is
		// most useful after the GUI menu phase configures them; from the text
		// interface it snapshots whatever the running match currently uses.
		s32 idx = mpWeaponPresetFind(arg);
		if (idx >= 0) {
			mpWeaponPresetReplace(idx, g_MpSetup.weapons, g_MpSlotFnFlags);
		} else {
			idx = mpWeaponPresetAdd(arg, g_MpSetup.weapons, g_MpSlotFnFlags);
		}
		if (idx < 0) { netAdminReply(cl, "savepreset: failed (preset table full?)"); return; }
		mpsetupSaveCurrentFile();
		g_NetAdminSetup.weaponpreset = (s8)idx;
		strncpy(g_NetAdminSetup.preset_name, arg, sizeof(g_NetAdminSetup.preset_name) - 1);
		g_NetAdminSetup.preset_name[sizeof(g_NetAdminSetup.preset_name) - 1] = '\0';
		netAdminReply(cl, "savepreset: saved `%s` (#%d) and persisted to mpsetups.bin", arg, idx);
		return;
	}

	if (strcmp(cmd, "saverotation") == 0) {
		if (!in_control) { netAdminReply(cl, "saverotation: take control first (take)"); return; }
		if (!*arg) { netAdminReply(cl, "usage: saverotation <name>"); return; }
		if (g_NetPlaylist.count >= PLAYLIST_MAX_ENTRIES) {
			netAdminReply(cl, "saverotation: playlist full (%d entries)", PLAYLIST_MAX_ENTRIES);
			return;
		}
		struct playlistentry *dst = &g_NetPlaylist.entries[g_NetPlaylist.count];
		*dst = g_NetAdminSetup;
		strncpy(dst->name, arg, sizeof(dst->name) - 1);
		dst->name[sizeof(dst->name) - 1] = '\0';
		if (dst->weight == 0) { dst->weight = 1; }
		g_NetPlaylist.count++;
		const s32 persisted = playlistAppendEntryToFile(dst);
		netAdminReply(cl, "saverotation: added `%s` as entry [%d]%s",
				dst->name, (s32)g_NetPlaylist.count - 1,
				persisted == 0 ? " (persisted to playlist file)" : " (live only; file not writable)");
		return;
	}

	if (strcmp(cmd, "players") == 0) {
		s32 shown = 0;
		for (s32 i = 0; i < g_NetMaxClients; ++i) {
			const struct netclient *c = &g_NetClients[i];
			if (c->state < CLSTATE_LOBBY) { continue; }
			netAdminReply(cl, "  [%d] %s%s%s state=%d team=%d", i, c->settings.name,
					c->is_spectator ? " (spec)" : "", c->is_admin ? " (admin)" : "",
					(s32)c->state, (s32)c->settings.team);
			++shown;
		}
		netAdminReply(cl, "players: %d connected, %d bots", shown, (s32)g_BotCount);
		return;
	}

	if (strcmp(cmd, "kick") == 0) {
		if (!*arg) { netAdminReply(cl, "usage: kick <name|id>"); return; }
		char who[NET_MAX_NAME + 1] = { 0 };
		s32 wi = 0;
		const char *q = arg;
		while (*q && *q != ' ' && *q != '\t' && wi < (s32)sizeof(who) - 1) { who[wi++] = *q++; }
		who[wi] = '\0';
		struct netclient *target = NULL;
		char *endp = NULL;
		const s32 id_try = (s32)strtol(who, &endp, 10);
		if (endp && *endp == '\0' && id_try > 0 && id_try < g_NetMaxClients
				&& g_NetClients[id_try].state >= CLSTATE_LOBBY) {
			target = &g_NetClients[id_try];
		}
		if (!target) {
			for (s32 i = 1; i < g_NetMaxClients; ++i) {
				if (g_NetClients[i].state >= CLSTATE_LOBBY
						&& strcasecmp(g_NetClients[i].settings.name, who) == 0) {
					target = &g_NetClients[i];
					break;
				}
			}
		}
		if (!target) {
			netAdminReply(cl, "kick: no such client `%s`", who);
		} else if (target == cl) {
			netAdminReply(cl, "kick: refusing to kick yourself");
		} else {
			netAdminReply(cl, "kick: disconnecting %s", target->settings.name);
			netChatPrintf(NULL, "%s was kicked by admin", target->settings.name);
			netServerKick(target, DISCONNECT_KICKED);
		}
		return;
	}

	if (strcmp(cmd, "say") == 0) {
		if (!*arg) { netAdminReply(cl, "usage: say <message>"); return; }
		netChatPrintf(NULL, "[ADMIN] %s", arg);
		netAdminReply(cl, "say: sent");
		return;
	}

	netAdminReply(cl, "admin: unknown command `%s` (try: help)", cmd);
}

// Parse a 6-digit hex colour "RRGGBB" (optional leading '#') into out[3].
// Returns true only on an exact 6-hex-digit string.
static bool netParseHexColour(const char *s, u8 out[3])
{
	if (s[0] == '#') {
		s++;
	}
	for (s32 i = 0; i < 6; i++) {
		const char c = s[i];
		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
			return false;
		}
	}
	if (s[6] != '\0') {
		return false;
	}
	const u32 rgb = (u32)strtoul(s, NULL, 16);
	out[0] = (rgb >> 16) & 0xff;
	out[1] = (rgb >> 8) & 0xff;
	out[2] = rgb & 0xff;
	return true;
}

// --- /wireframe save|load: persist the wireframe appearance (sky + wire colour
// + thickness) to pd.ini. config.c only saves/loads *registered* variables, so
// these shadow vars are registered lazily (idempotent) and synced to the live
// globals; the u8 sky colour packs into a u32.
static u32 g_WfCfgBg = 0; // packed 0x00RRGGBB
static f32 g_WfCfgWireR = 0.0f, g_WfCfgWireG = 0.0f, g_WfCfgWireB = 0.0f;
static s32 g_WfCfgWireEnabled = 0;
static f32 g_WfCfgThick = 1.0f;

static void netWireframeCfgRegister(void)
{
	configRegisterUInt("Wireframe.BgColour", &g_WfCfgBg, 0, 0xffffff);
	configRegisterFloat("Wireframe.WireColourR", &g_WfCfgWireR, 0.0f, 1.0f);
	configRegisterFloat("Wireframe.WireColourG", &g_WfCfgWireG, 0.0f, 1.0f);
	configRegisterFloat("Wireframe.WireColourB", &g_WfCfgWireB, 0.0f, 1.0f);
	configRegisterInt("Wireframe.WireColourEnabled", &g_WfCfgWireEnabled, 0, 1);
	configRegisterFloat("Wireframe.Thickness", &g_WfCfgThick, 0.5f, 16.0f); // 0.5 floor: glLineWidth(0) is invalid
}

// Copy the live wireframe appearance into the shadows (before save, and before
// load so file-absent keys leave the current look unchanged).
static void netWireframeCfgSnapshot(void)
{
	extern u8 g_WireframeBgColour[3];
	extern int gfx_wireframe_wire_color_enabled;
	extern f32 gfx_wireframe_wire_color[3];
	extern f32 gfx_wireframe_line_width;
	g_WfCfgBg = ((u32)g_WireframeBgColour[0] << 16)
	          | ((u32)g_WireframeBgColour[1] << 8)
	          |  (u32)g_WireframeBgColour[2];
	g_WfCfgWireR = gfx_wireframe_wire_color[0];
	g_WfCfgWireG = gfx_wireframe_wire_color[1];
	g_WfCfgWireB = gfx_wireframe_wire_color[2];
	g_WfCfgWireEnabled = gfx_wireframe_wire_color_enabled;
	g_WfCfgThick = gfx_wireframe_line_width;
}

// Apply the shadows to the live wireframe appearance (after load).
static void netWireframeCfgApply(void)
{
	extern u8 g_WireframeBgColour[3];
	extern int gfx_wireframe_wire_color_enabled;
	extern f32 gfx_wireframe_wire_color[3];
	extern f32 gfx_wireframe_line_width;
	g_WireframeBgColour[0] = (g_WfCfgBg >> 16) & 0xff;
	g_WireframeBgColour[1] = (g_WfCfgBg >> 8) & 0xff;
	g_WireframeBgColour[2] = g_WfCfgBg & 0xff;
	gfx_wireframe_wire_color[0] = g_WfCfgWireR;
	gfx_wireframe_wire_color[1] = g_WfCfgWireG;
	gfx_wireframe_wire_color[2] = g_WfCfgWireB;
	gfx_wireframe_wire_color_enabled = g_WfCfgWireEnabled;
	gfx_wireframe_line_width = g_WfCfgThick;
}

// Local vanity easter egg: when set, netGrasluRender draws a "Graslu" banner in
// the lower-left HUD corner during gameplay. Toggled by the hidden /graslu
// console command; purely local (nothing about it goes on the wire).
static s32 g_GrasluEgg = 0;

// Companion vanity egg: when set, netRedvox57Render draws a red "Redvox57" banner
// in the same lower-left HUD slot. Toggled by the hidden /redvox57 command; never
// enabled alongside Graslu. Purely local (nothing goes on the wire).
static s32 g_Redvox57Egg = 0;

// Config "Game.Egg" (pd.ini, under [Game] as `Egg=`): leave "0" (default) for no
// banner, or set to a vanity egg's command name ("graslu" / "redvox57") to
// auto-enable it on boot. Applied once in netInit, after the config is loaded.
// Case-insensitive.
static char g_EggConfig[16] = "0";

static void netApplyEggConfig(void)
{
	if (strcasecmp(g_EggConfig, "graslu") == 0) {
		g_GrasluEgg = 1;
	} else if (strcasecmp(g_EggConfig, "redvox57") == 0) {
		g_Redvox57Egg = 1;
	}
}

s32 netConsoleCommand(const char *line)
{
	if (!line || line[0] != '/') {
		return 0;
	}

	// Split into command word + remainder. Cmd word is the first whitespace-
	// delimited token after the leading '/'.
	char cmd[32] = { 0 };
	const char *p = line + 1;
	s32 ci = 0;
	while (*p && *p != ' ' && *p != '\t' && ci < (s32)sizeof(cmd) - 1) {
		cmd[ci++] = (char)tolower((unsigned char)*p);
		++p;
	}
	cmd[ci] = '\0';
	while (*p == ' ' || *p == '\t') {
		++p;
	}
	const char *arg = p; // may be ""

	// Determinism harness commands (/dethash, /detpin, /detinfo) — checked first.
	if (detConsoleCommand(cmd, arg)) {
		return 1;
	}

	if (strcmp(cmd, "lua") == 0) {
		luaaiConsoleCommand(*arg ? arg : NULL);
		return 1;
	}

	if (strcmp(cmd, "lag") == 0) {
		if (*arg) {
			const s32 ms = atoi(arg);
			g_NetSimLagMs = (ms < 0) ? 0 : (ms > 5000 ? 5000 : ms);
			if (g_NetSimLagMs == 0) {
				netLagQueueClear();
				sysLogPrintf(LOG_CHAT, "NET: fake lag disabled");
			} else {
				sysLogPrintf(LOG_CHAT, "NET: fake outgoing lag = %d ms", g_NetSimLagMs);
			}
		} else {
			sysLogPrintf(LOG_CHAT, "NET: fake lag is %d ms (usage: /lag <ms>)", g_NetSimLagMs);
		}
	} else if (strcmp(cmd, "loss") == 0) {
		if (*arg) {
			const s32 n = atoi(arg);
			g_NetSimPacketLoss = (n < 0) ? 0 : n;
			if (g_NetSimPacketLoss == 0) {
				sysLogPrintf(LOG_CHAT, "NET: packet loss sim disabled");
			} else {
				sysLogPrintf(LOG_CHAT, "NET: dropping ~1 in %d unreliable packets", g_NetSimPacketLoss);
			}
		} else {
			sysLogPrintf(LOG_CHAT, "NET: packet loss = 1/%d (usage: /loss <N>, 0=off)", g_NetSimPacketLoss);
		}
	} else if (strcmp(cmd, "diag") == 0) {
		if (*arg) {
			strncpy(g_NetDiagPath, arg, sizeof(g_NetDiagPath) - 1);
			g_NetDiagPath[sizeof(g_NetDiagPath) - 1] = '\0';
			netDiagOpen();
		} else {
			netDiagClose();
			g_NetDiagPath[0] = '\0';
			sysLogPrintf(LOG_CHAT, "NET: diag log closed");
		}
	} else if (strcmp(cmd, "diagrate") == 0) {
		if (*arg) {
			const s32 r = atoi(arg);
			g_NetDiagDumpRate = (r < 0) ? 0 : (r > 600 ? 600 : r);
			sysLogPrintf(LOG_CHAT, "NET: diag log pos-dump rate = every %u ticks", g_NetDiagDumpRate);
		} else {
			sysLogPrintf(LOG_CHAT, "NET: diag log rate = %u (usage: /diagrate <ticks>)", g_NetDiagDumpRate);
		}
	} else if (strcmp(cmd, "netinfo") == 0) {
		sysLogPrintf(LOG_CHAT, "NET: tick=%u mode=%s clients=%d sims=%d lag=%dms loss=1/%d diag='%s'",
			g_NetTick,
			g_NetMode == NETMODE_SERVER ? "SERVER" : g_NetMode == NETMODE_CLIENT ? "CLIENT" : "NONE",
			g_NetNumClients, g_BotCount, g_NetSimLagMs, g_NetSimPacketLoss,
			g_NetDiagPath[0] ? g_NetDiagPath : "(off)");
		sysLogPrintf(LOG_CHAT, "NET: interp=%u stale=%u svc-update=%u clc-update=%u",
			g_NetInterpTicks, g_NetStaleSnapshotTicks,
			g_NetServerUpdateRate, g_NetClientUpdateRate);
		sysLogPrintf(LOG_CHAT, "NET: csp frames=%u corr_thresh=%.1fu teleport_thresh=%.1fu",
			g_NetCspCorrFramesMax,
			sqrtf(g_NetCspCorrThreshSq),
			sqrtf(g_NetCspTeleportThreshSq));
	} else if (strcmp(cmd, "netstats") == 0) {
		// Per-message-type tx bytes GENERATED in the last second (multiply by the
		// number of clients for actual wire bytes — these go into the broadcast
		// buffer once). Tells us whether players, sims or stats dominate so we
		// optimise the right thing.
		const u32 pm = g_NetStatPerSec[NETSTAT_PLAYERMOVE];
		const u32 prm = g_NetStatPerSec[NETSTAT_PROPMOVE];
		const u32 ps = g_NetStatPerSec[NETSTAT_PLAYERSTATS];
		sysLogPrintf(LOG_CHAT, "NET stats (B/s generated, x%d clients on wire): player_move=%u sim_move=%u player_stats=%u",
				g_NetNumClients > 1 ? g_NetNumClients - 1 : 0, pm, prm, ps);
		sysLogPrintf(LOG_CHAT, "NET: sims=%d -> sim_move is the big lever; F9 shows total tx",
				(s32)g_BotCount);
	} else if (strcmp(cmd, "interp") == 0) {
		// /interp <ticks> — entity interpolation lag. Higher = smoother
		// remote players under jitter but more visible latency; lower =
		// snappier but more jittery if packets arrive unevenly. Default 3.
		// Clamped to [0, 60] — beyond a second of lag the ring buffer
		// can't hold enough snapshots anyway.
		if (*arg) {
			const s32 n = atoi(arg);
			g_NetInterpTicks = (u32)((n < 0) ? 0 : (n > 60 ? 60 : n));
			sysLogPrintf(LOG_CHAT, "NET: interp ticks = %u", g_NetInterpTicks);
		} else {
			sysLogPrintf(LOG_CHAT, "NET: interp ticks = %u (usage: /interp <ticks>)", g_NetInterpTicks);
		}
	} else if (strcmp(cmd, "stale") == 0) {
		// /stale <ticks> — how old the newest snapshot can be before
		// bwalkUpdateRemote hard-snaps instead of lerping between stale
		// entries. Default 30 (~500ms). Bump for sparse update rates,
		// lower for tighter desync recovery.
		if (*arg) {
			const s32 n = atoi(arg);
			g_NetStaleSnapshotTicks = (u32)((n < 0) ? 0 : (n > 600 ? 600 : n));
			sysLogPrintf(LOG_CHAT, "NET: stale-snapshot threshold = %u ticks", g_NetStaleSnapshotTicks);
		} else {
			sysLogPrintf(LOG_CHAT, "NET: stale-snapshot threshold = %u ticks (usage: /stale <ticks>)", g_NetStaleSnapshotTicks);
		}
	} else if (strcmp(cmd, "extrap") == 0) {
		// /extrap <ticks> — remote-player dead-reckoning window. When the newest
		// snapshot is older than the interp target (late packet / jitter), the
		// remote is extrapolated from last velocity for up to this many ticks
		// instead of freezing. 0 = converge to newest (no extrapolation). Keep
		// small (default 3) — large values overshoot on direction changes.
		if (*arg) {
			const s32 n = atoi(arg);
			g_NetExtrapMaxTicks = (u32)((n < 0) ? 0 : (n > 12 ? 12 : n));
			sysLogPrintf(LOG_CHAT, "NET: remote extrapolation = %u ticks", g_NetExtrapMaxTicks);
		} else {
			sysLogPrintf(LOG_CHAT, "NET: remote extrapolation = %u ticks (usage: /extrap <ticks>, 0=off)", g_NetExtrapMaxTicks);
		}
	} else if (strcmp(cmd, "chrinterp") == 0) {
		// /chrinterp on|off — full pose interpolation for replicated chrs (sims /
		// co-op NPCs): position + body facing + aim reconstructed for one
		// consistent past instant. off reverts to the receive-time per-packet
		// apply for A/B comparison.
		if (strcmp(arg, "on") == 0) {
			g_NetChrInterp = 1;
		} else if (strcmp(arg, "off") == 0) {
			g_NetChrInterp = 0;
		}
		sysLogPrintf(LOG_CHAT, "NET: chr pose interpolation = %s%s", g_NetChrInterp ? "ON" : "OFF",
				(*arg && strcmp(arg, "on") && strcmp(arg, "off")) ? " (usage: /chrinterp on|off)" : "");
	} else if (strcmp(cmd, "coopchr") == 0) {
		// /coopchr on|off — runtime co-op chr lifecycle replication (SVC_CHR_SPAWN
		// for reinforcement/clone spawns + SVC_PROP_FREE for reaped corpses). Off
		// disables both so a crash during e.g. an alarm reinforcement wave can be
		// isolated to this path. Host-side gate; flip it on the host.
		if (strcmp(arg, "on") == 0) {
			g_NetCoopChrLifecycle = 1;
		} else if (strcmp(arg, "off") == 0) {
			g_NetCoopChrLifecycle = 0;
		}
		sysLogPrintf(LOG_CHAT, "NET: co-op runtime chr lifecycle = %s%s", g_NetCoopChrLifecycle ? "ON" : "OFF",
				(*arg && strcmp(arg, "on") && strcmp(arg, "off")) ? " (usage: /coopchr on|off)" : "");
	} else if (strcmp(cmd, "coopobj") == 0) {
		// /coopobj on|off (default off) — EXPERIMENTAL. On a CLIENT, make networked
		// OBJ props fully wire-driven: after objTickPlayer runs, snap prop->pos back to
		// the host's authoritative position, discarding the client's local physics
		// integration (which otherwise drifts the model away from the wire-corrected
		// hitbox at high ping, and lags items parented to a moving object). Pickups /
		// interactions still run. Flip on each client to A/B the physics-object desync.
		if (strcmp(arg, "on") == 0) {
			g_NetCoopObjWireDriven = 1;
		} else if (strcmp(arg, "off") == 0) {
			g_NetCoopObjWireDriven = 0;
		}
		sysLogPrintf(LOG_CHAT, "NET: co-op OBJ wire-driven = %s%s", g_NetCoopObjWireDriven ? "ON" : "OFF",
				(*arg && strcmp(arg, "on") && strcmp(arg, "off")) ? " (usage: /coopobj on|off)" : "");
	} else if (strcmp(cmd, "coop") == 0) {
		// /coop [solostageindex] [difficulty] — HOST only. Start a campaign co-op
		// session on a solo stage (default Defection, index 0). Difficulty is
		// 0=Agent (default), 1=Special Agent, 2=Perfect Agent, 3=Perfect Dark.
		// Clients already in the lobby load the same stage + difficulty via
		// SVC_STAGE_START's co-op branch.
		if (g_NetMode != NETMODE_SERVER) {
			sysLogPrintf(LOG_CHAT, "NET: /coop is host only");
		} else {
			s32 idx = SOLOSTAGEINDEX_DEFECTION;
			s32 diff = DIFF_A;
			if (*arg) {
				char *end = NULL;
				idx = (s32)strtol(arg, &end, 0);
				if (idx < 0 || idx >= NUM_SOLOSTAGES) {
					idx = SOLOSTAGEINDEX_DEFECTION;
				}
				while (*end == ' ' || *end == '\t') { end++; }
				if (*end) {
					diff = (s32)strtol(end, NULL, 0);
					if (diff < DIFF_A) { diff = DIFF_A; }
					if (diff > DIFF_PD) { diff = DIFF_PD; }
				}
			}
			g_MissionConfig.stageindex = idx;
			// N = all players currently in the session: g_NetNumClients already
			// counts the host (g_NetClients[0]) plus every connected remote client.
			// The SVC_STAGE_START co-op manifest sends this same count so clients
			// derive the identical N. (Late joins are rejected, so the set is fixed.)
			sysLogPrintf(LOG_CHAT, "NET: starting co-op (solo stage %d, difficulty %d, %d players)", idx, diff, g_NetNumClients);
			netCoopEnterStage((s32)g_SoloStages[idx].stagenum, diff, g_NetNumClients);
		}
	} else if (strcmp(cmd, "hitvalidate") == 0) {
		// /hitvalidate <0|1|2> — server-side validation of client CLC_HIT claims
		// against the server's own lag-comp'd hit detection. 0=off (trust client),
		// 1=log-only (apply but log mismatches to /diag), 2=enforce (drop claims
		// the server never detected). Start at 1 and watch the diag log for
		// 'hit_reject' lines before enabling 2.
		if (*arg) {
			const s32 n = atoi(arg);
			g_NetHitValidate = (n < 0) ? 0 : (n > 2 ? 2 : n);
		}
		sysLogPrintf(LOG_CHAT, "NET: hit validation = %d (%s)%s", g_NetHitValidate,
				g_NetHitValidate == 0 ? "off" : g_NetHitValidate == 1 ? "log-only" : "enforce",
				*arg ? "" : " (usage: /hitvalidate 0|1|2)");
	} else if (strcmp(cmd, "hitmarker") == 0) {
		// Hidden test feature: centred hitmarker flash on a confirmed local hit,
		// giving immediate feedback at high ping instead of waiting for the
		// server's damage round-trip. Off by default.
		if (strcmp(arg, "on") == 0) {
			g_NetHitmarkerEnabled = 1;
		} else if (strcmp(arg, "off") == 0) {
			g_NetHitmarkerEnabled = 0;
		}
		sysLogPrintf(LOG_CHAT, "NET: hitmarker = %s%s", g_NetHitmarkerEnabled ? "ON" : "OFF",
				(*arg && strcmp(arg, "on") && strcmp(arg, "off")) ? " (usage: /hitmarker on|off)" : "");
	} else if (strcmp(cmd, "svcrate") == 0) {
		// /svcrate <N> — server-side update interval. 1 = send every tick
		// (max bandwidth, smoothest). Larger = bandwidth saving but
		// snapshot ring fills slower, more lerp jitter.
		if (*arg) {
			const s32 n = atoi(arg);
			g_NetServerUpdateRate = (u32)((n < 1) ? 1 : (n > 60 ? 60 : n));
			sysLogPrintf(LOG_CHAT, "NET: server update interval = every %u ticks", g_NetServerUpdateRate);
		} else {
			sysLogPrintf(LOG_CHAT, "NET: server update interval = %u (usage: /svcrate <ticks>)", g_NetServerUpdateRate);
		}
	} else if (strcmp(cmd, "clcrate") == 0) {
		// /clcrate <N> — client-side input send interval. Same trade-off:
		// 1 = every tick, larger = less bandwidth but worse server-side
		// hit reg and latency.
		if (*arg) {
			const s32 n = atoi(arg);
			g_NetClientUpdateRate = (u32)((n < 1) ? 1 : (n > 60 ? 60 : n));
			sysLogPrintf(LOG_CHAT, "NET: client update interval = every %u ticks", g_NetClientUpdateRate);
		} else {
			sysLogPrintf(LOG_CHAT, "NET: client update interval = %u (usage: /clcrate <ticks>)", g_NetClientUpdateRate);
		}
	} else if (strcmp(cmd, "lagcomp") == 0) {
		// /lagcomp [exact|legacy] — server-side hit-rewind mode. exact (default,
		// proto 63) rewinds targets to inmovetick - renderbehind, the exact
		// server-tick the shooter was displaying; legacy uses the old RTT/2 +
		// interp_lag symmetric-latency estimate. Live A/B for hit registration feel.
		if (strcmp(arg, "exact") == 0) {
			g_NetLagCompExact = 1;
			sysLogPrintf(LOG_CHAT, "NET: lag-comp = exact (inmovetick - renderbehind)");
		} else if (strcmp(arg, "legacy") == 0) {
			g_NetLagCompExact = 0;
			sysLogPrintf(LOG_CHAT, "NET: lag-comp = legacy (RTT/2 + interp_lag)");
		} else {
			sysLogPrintf(LOG_CHAT, "NET: lag-comp = %s (usage: /lagcomp exact|legacy)",
				g_NetLagCompExact ? "exact" : "legacy");
		}
	} else if (strcmp(cmd, "cspframes") == 0) {
		// /cspframes <N> — ticks the smooth CSP correction spreads error
		// over. Smaller = snappier; larger = smoother but slower. Default 10.
		// Sets the *initial* window (g_NetCspCorrFramesMax); the in-flight
		// countdown (g_NetCspCorrFrames) reloads from this on the next ack.
		if (*arg) {
			const s32 n = atoi(arg);
			g_NetCspCorrFramesMax = (u32)((n < 1) ? 1 : (n > 120 ? 120 : n));
			sysLogPrintf(LOG_CHAT, "NET: CSP correction window = %u ticks", g_NetCspCorrFramesMax);
		} else {
			sysLogPrintf(LOG_CHAT, "NET: CSP correction window = %u ticks (usage: /cspframes <ticks>)", g_NetCspCorrFramesMax);
		}
	} else if (strcmp(cmd, "cspcorr") == 0) {
		// /cspcorr <units> — minimum prediction error (in world units)
		// before smooth correction kicks in. Below this, divergences are
		// ignored to avoid jitter from sub-noise drift. Stored squared
		// internally; the user enters / sees plain units. Default 25.
		if (*arg) {
			const f32 u = (f32)atof(arg);
			const f32 clamped = (u < 0.f) ? 0.f : (u > 1000.f ? 1000.f : u);
			g_NetCspCorrThreshSq = clamped * clamped;
			sysLogPrintf(LOG_CHAT, "NET: CSP correction threshold = %.1fu (sq=%.1f)", clamped, g_NetCspCorrThreshSq);
		} else {
			sysLogPrintf(LOG_CHAT, "NET: CSP correction threshold = %.1fu (usage: /cspcorr <units>)",
				sqrtf(g_NetCspCorrThreshSq));
		}
	} else if (strcmp(cmd, "cspteleport") == 0 || strcmp(cmd, "cspport") == 0) {
		// /cspteleport <units> — above this prediction error the CSP path
		// hard-snaps instead of smooth-correcting. Default 120u covers max
		// strafe-run + fastmovement + ramp + fall combined; anything past
		// that is treated as a teleport / network glitch. Should always be
		// > /cspcorr (otherwise no smooth correction window exists).
		if (*arg) {
			const f32 u = (f32)atof(arg);
			const f32 clamped = (u < 0.f) ? 0.f : (u > 10000.f ? 10000.f : u);
			g_NetCspTeleportThreshSq = clamped * clamped;
			sysLogPrintf(LOG_CHAT, "NET: CSP teleport threshold = %.1fu (sq=%.1f)", clamped, g_NetCspTeleportThreshSq);
		} else {
			sysLogPrintf(LOG_CHAT, "NET: CSP teleport threshold = %.1fu (usage: /cspteleport <units>)",
				sqrtf(g_NetCspTeleportThreshSq));
		}
	} else if (strcmp(cmd, "spec") == 0 || strcmp(cmd, "spectate") == 0) {
		// /spec — cycle to next live target
		// /spec next | /spec prev — cycle direction
		// /spec off | /spec stop — clear and return to first-person
		if (!*arg || strcmp(arg, "next") == 0) {
			netSpectateCycle(+1);
		} else if (strcmp(arg, "prev") == 0 || strcmp(arg, "previous") == 0) {
			netSpectateCycle(-1);
		} else if (strcmp(arg, "off") == 0 || strcmp(arg, "stop") == 0 || strcmp(arg, "none") == 0) {
			netSpectateStop();
		} else if (strcmp(arg, "toggle") == 0) {
			netSpectateToggle();
		} else {
			// Treat anything else as a name lookup against g_MpAllChrConfigPtrs.
			// Case-sensitive prefix match keeps things predictable when the host
			// has named bots with the dictionary scheme ("BobSim", "AliceSim").
			struct chrdata *match = NULL;
			const size_t arglen = strlen(arg);
			for (s32 i = 0; i < MAX_MPCHRS; ++i) {
				if (!g_MpAllChrPtrs[i] || !g_MpAllChrConfigPtrs[i]) continue;
				if (strncmp(g_MpAllChrConfigPtrs[i]->name, arg, arglen) == 0) {
					match = g_MpAllChrPtrs[i];
					break;
				}
			}
			if (match) {
				g_NetSpectateChr = match;
				sysLogPrintf(LOG_CHAT, "NET: spectating %s", g_MpAllChrConfigPtrs[mpPlayerGetIndex(match)]->name);
			} else {
				sysLogPrintf(LOG_CHAT, "NET: no chr matching '%s'", arg);
			}
		}
	} else if (strcmp(cmd, "igtick") == 0) {
		// /igtick — diagnostic for the LOCAL in-game tick rate (not the
		// server / wire tick). Prints the current `lvframe60` and, on
		// the second+ call, the wall-clock rate of lvframe60 advance
		// between calls. Also dumps the local player chr's GE i-frame
		// stamp + age so you can see whether the gate is firing.
		static u64 prev_us = 0;
		static s32 prev_lvframe60 = 0;
		const u64 now_us = sysGetMicroseconds();
		sysLogPrintf(LOG_CHAT, "IGTICK: lvframe60=%d lvframenum=%d lvupdate60=%d",
				g_Vars.lvframe60, g_Vars.lvframenum, g_Vars.lvupdate60);
		if (prev_us > 0) {
			const u64 elapsed_us = now_us - prev_us;
			const s32 frame_delta = g_Vars.lvframe60 - prev_lvframe60;
			const f64 secs = elapsed_us / 1000000.0;
			const f64 tps = (elapsed_us > 0) ? (frame_delta * 1000000.0 / (f64)elapsed_us) : 0.0;
			sysLogPrintf(LOG_CHAT, "IGTICK: +%d ticks over %.2fs = %.1f tps (target 60)",
					frame_delta, secs, tps);
		} else {
			sysLogPrintf(LOG_CHAT, "IGTICK: call /igtick again to see tick rate");
		}
		prev_us = now_us;
		prev_lvframe60 = g_Vars.lvframe60;
		// GE i-frame state for the local player chr.
		sysLogPrintf(LOG_CHAT, "IGTICK: gemode=%d normmpr=%d gecheat=%d active=%d ticks(18)=%d",
				(g_MpSetup.options & MPOPTION_GOLDENEYE) ? 1 : 0,
				g_Vars.normmplayerisrunning,
				cheatIsActive(CHEAT_GOLDENEYE) ? 1 : 0,
				goldeneyeStyleActive() ? 1 : 0,
				(s32)TICKS(18));
		if (!g_Vars.currentplayer) {
			sysLogPrintf(LOG_CHAT, "IGTICK: no currentplayer");
		} else if (!g_Vars.currentplayer->prop) {
			sysLogPrintf(LOG_CHAT, "IGTICK: currentplayer has no prop (in menu?)");
		} else if (!g_Vars.currentplayer->prop->chr) {
			sysLogPrintf(LOG_CHAT, "IGTICK: currentplayer->prop has no chr");
		} else {
			struct chrdata *mychr = g_Vars.currentplayer->prop->chr;
			const u32 age = (u32)g_Vars.lvframe60 - (u32)mychr->lastdamagetick60;
			const u32 window = (u32)TICKS(18);
			const bool stamped = (mychr->lastdamagetick60 != 0);
			const bool in_iframe = stamped && (age < window);
			sysLogPrintf(LOG_CHAT, "IGTICK: my chr lastdamage=%d age=%u window=%u %s",
					mychr->lastdamagetick60, stamped ? age : 0u, window,
					in_iframe ? "(IFRAME ACTIVE)" : stamped ? "(iframe expired)" : "(never damaged)");
		}
	} else if (strcmp(cmd, "slomo") == 0) {
		// /slomo — diagnostic for the slow-motion / combat-boost chain. Dumps
		// every input of the decision (option bits, challenge unlock, type),
		// the per-frame engage flag, the tick-pin state and the live tick
		// values, so a single call on each machine pinpoints where the chain
		// breaks (options missing vs flag not set vs step not halved vs
		// client not applying the wire flag).
		sysLogPrintf(LOG_CHAT, "SLOMO: type=%d (0=off 1=on 2=smart) opt_on=%d opt_smart=%d unlocked=%d normmpr=%d",
				lvGetSlowMotionType(),
				(g_MpSetup.options & MPOPTION_SLOWMOTION_ON) ? 1 : 0,
				(g_MpSetup.options & MPOPTION_SLOWMOTION_SMART) ? 1 : 0,
				challengeIsFeatureUnlocked(MPFEATURE_SLOWMOTION) ? 1 : 0,
				g_Vars.normmplayerisrunning);
		sysLogPrintf(LOG_CHAT, "SLOMO: engaged=%d pin=%d netmode=%d up240=%d up60=%d rem=%d diff240=%d",
				g_LvSlomoEngaged, detTickPinActive(), g_NetMode,
				g_Vars.lvupdate240, g_Vars.lvupdate60, g_Vars.lvupdate240rem,
				g_Vars.diffframe240);
		sysLogPrintf(LOG_CHAT, "SLOMO: speedpill on=%d want=%d time=%d incutscene=%d",
				g_Vars.speedpillon, g_Vars.speedpillwant, g_Vars.speedpilltime,
				g_Vars.in_cutscene);
	} else if (strcmp(cmd, "playlist") == 0) {
		// /playlist [list|reload]   server-only
		if (g_NetMode != NETMODE_SERVER && g_NetMode != NETMODE_NONE) {
			sysLogPrintf(LOG_CHAT, "/playlist is server-only");
		} else if (!*arg || strncmp(arg, "list", 4) == 0) {
			playlistDumpToChat();
		} else if (strncmp(arg, "reload", 6) == 0) {
			const s32 ok = playlistLoad(&g_NetPlaylist, g_NetPlaylistPath);
			sysLogPrintf(LOG_CHAT, "playlist: reload %s (%d entries)",
					ok ? "ok" : "failed", (s32)g_NetPlaylist.count);
		} else {
			sysLogPrintf(LOG_CHAT, "usage: /playlist [list|reload]");
		}
	} else if (strcmp(cmd, "nextmap") == 0) {
		// /nextmap [index]   server-only: force the next playlist entry now,
		// skipping the vote. With no index, picks a random weighted entry.
		if (g_NetMode != NETMODE_SERVER) {
			sysLogPrintf(LOG_CHAT, "/nextmap is server-only");
		} else if (g_NetPlaylist.count == 0) {
			sysLogPrintf(LOG_CHAT, "playlist empty");
		} else {
			s32 idx = -1;
			if (*arg) {
				idx = (s32)strtol(arg, NULL, 0);
				if (idx < 0 || idx >= g_NetPlaylist.count) {
					sysLogPrintf(LOG_CHAT, "nextmap: index %d out of range (0..%d)",
							idx, (s32)g_NetPlaylist.count - 1);
					return 1;
				}
			} else {
				u64 rng = ((u64)g_NetTick * 0x9E3779B97F4A7C15ULL) ^ sysGetMicroseconds();
				idx = playlistPick(&g_NetPlaylist, &rng);
			}
			struct playlistentry resolved;
			playlistResolveRandoms(&g_NetPlaylist.entries[idx], &resolved);
			playlistApply(&resolved);
			sysLogPrintf(LOG_CHAT, "nextmap: applying [%d] %s", idx, resolved.name);
			mpStartMatch();
			g_NetVote.state = NETVOTE_IDLE; // cancel any vote in flight
		}
	} else if (strcmp(cmd, "kick") == 0) {
		// /kick <name|id> [reason]   server-only
		if (g_NetMode != NETMODE_SERVER) {
			sysLogPrintf(LOG_CHAT, "/kick is server-only");
		} else if (!*arg) {
			sysLogPrintf(LOG_CHAT, "usage: /kick <name|id>");
		} else {
			// Parse first whitespace-delimited token as id-or-name.
			char who[NET_MAX_NAME + 1] = { 0 };
			s32 wi = 0;
			const char *q = arg;
			while (*q && *q != ' ' && *q != '\t' && wi < (s32)sizeof(who) - 1) {
				who[wi++] = *q++;
			}
			who[wi] = '\0';
			struct netclient *target = NULL;
			// Try numeric id first.
			char *endp = NULL;
			const s32 id_try = (s32)strtol(who, &endp, 10);
			if (endp && *endp == '\0' && id_try > 0 && id_try < g_NetMaxClients) {
				if (g_NetClients[id_try].state >= CLSTATE_LOBBY) {
					target = &g_NetClients[id_try];
				}
			}
			// Fall back to name match.
			if (!target) {
				for (s32 i = 1; i < g_NetMaxClients; ++i) {
					if (g_NetClients[i].state >= CLSTATE_LOBBY
							&& strcasecmp(g_NetClients[i].settings.name, who) == 0) {
						target = &g_NetClients[i];
						break;
					}
				}
			}
			if (!target) {
				sysLogPrintf(LOG_CHAT, "kick: no such client `%s`", who);
			} else {
				sysLogPrintf(LOG_CHAT, "kick: disconnecting %s", target->settings.name);
				netChatPrintf(NULL, "%s was kicked", target->settings.name);
				netServerKick(target, DISCONNECT_KICKED);
			}
		}
	} else if (strcmp(cmd, "say") == 0) {
		// /say <msg>   server-only chat broadcast as the server
		if (g_NetMode != NETMODE_SERVER) {
			sysLogPrintf(LOG_CHAT, "/say is server-only");
		} else if (!*arg) {
			sysLogPrintf(LOG_CHAT, "usage: /say <message>");
		} else {
			netChatPrintf(NULL, "[SERVER] %s", arg);
		}
	} else if (strcmp(cmd, "admin") == 0) {
		// /admin <subcommand...> — remote server administration. Most subcommands
		// are sent to the server (CLC_ADMIN) and executed there, gated by the
		// admin password; responses arrive as SVC_ADMIN and print here. Two are
		// handled locally on the client: `configure` (load the Combat Sim setup
		// to edit via the normal menu) and `pushstart`/`go` (send the configured
		// setup to the server, which starts the match for everyone).
		char sub[16] = { 0 };
		s32 si = 0;
		const char *ap = arg;
		while (*ap && *ap != ' ' && *ap != '\t' && si < (s32)sizeof(sub) - 1) {
			sub[si++] = (char)tolower((unsigned char)*ap);
			++ap;
		}
		sub[si] = '\0';

		if (strcmp(sub, "pushstart") == 0 || strcmp(sub, "go") == 0) {
			netAdminPushStart();
		} else if (strcmp(sub, "configure") == 0 || strcmp(sub, "config") == 0) {
			netAdminConfigure();
		} else if (g_NetMode == NETMODE_CLIENT && g_NetLocalClient
				&& g_NetLocalClient->state >= CLSTATE_AUTH) {
			netbufStartWrite(&g_NetMsgRel);
			netmsgClcAdminWrite(&g_NetMsgRel, arg);
			netSend(g_NetLocalClient, &g_NetMsgRel, true, NETCHAN_CONTROL);
		} else if (g_NetMode == NETMODE_SERVER) {
			netServerAdminCommand(g_NetLocalClient, arg);
		} else {
			sysLogPrintf(LOG_CHAT, "/admin requires being connected to a server");
		}
	} else if (strcmp(cmd, "endmatch") == 0) {
		// /endmatch   server-only: triggers mainEndStage flow (score screen +
		// vote/nextmap). Useful for skipping a stuck round.
		if (g_NetMode != NETMODE_SERVER) {
			sysLogPrintf(LOG_CHAT, "/endmatch is server-only");
		} else if (g_StageNum == STAGE_CITRAINING || g_StageNum >= STAGE_TITLE) {
			sysLogPrintf(LOG_CHAT, "no match in progress");
		} else {
			sysLogPrintf(LOG_CHAT, "endmatch: triggering mainEndStage");
			mainEndStage();
		}
	} else if (strcmp(cmd, "players") == 0) {
		// /players   dump connected client roster
		s32 shown = 0;
		for (s32 i = 0; i < g_NetMaxClients; ++i) {
			const struct netclient *cl = &g_NetClients[i];
			if (cl->state < CLSTATE_LOBBY) continue;
			sysLogPrintf(LOG_CHAT, "  [%d] %s%s state=%d team=%d ping=%u",
					(s32)i, cl->settings.name,
					cl->is_spectator ? " (spec)" : "",
					(s32)cl->state,
					(s32)cl->settings.team,
					(unsigned)(cl->peer ? cl->peer->roundTripTime : 0u));
			++shown;
		}
		sysLogPrintf(LOG_CHAT, "players: %d connected, %d bots", shown, (s32)g_BotCount);
	} else if (strcmp(cmd, "vote") == 0) {
		// /vote N   client-side: cast a ballot for candidate N
		if (g_NetMode != NETMODE_CLIENT) {
			sysLogPrintf(LOG_CHAT, "/vote is client-only");
		} else if (g_NetVote.state != NETVOTE_OPEN) {
			sysLogPrintf(LOG_CHAT, "no vote currently open");
		} else if (!*arg) {
			sysLogPrintf(LOG_CHAT, "usage: /vote <0..%d>", (s32)g_NetVote.num_candidates - 1);
			for (s32 i = 0; i < g_NetVote.num_candidates; ++i) {
				sysLogPrintf(LOG_CHAT, "  [%d] %s", i, g_NetVote.candidates[i].name);
			}
		} else {
			const s32 idx = (s32)strtol(arg, NULL, 0);
			if (netClientVoteCast(idx) == 0) {
				sysLogPrintf(LOG_CHAT, "voted for [%d] %s", idx,
						g_NetVote.candidates[idx].name);
			} else {
				sysLogPrintf(LOG_CHAT, "vote failed: index out of range or vote closed");
			}
		}
	} else if (strcmp(cmd, "status") == 0) {
		// /status   dump server / match state
		sysLogPrintf(LOG_CHAT, "STATUS: name=\"%s\" mode=%d port=%u clients=%d/%d sims=%d tick=%u",
				g_NetServerName, g_NetMode, g_NetServerPort,
				g_NetNumClients, g_NetMaxClients, (s32)g_BotCount, g_NetTick);
		sysLogPrintf(LOG_CHAT, "STATUS: stage=0x%02x scenario=%d options=0x%016llx score=%d time=%d",
				g_MpSetup.stagenum, g_MpSetup.scenario, (unsigned long long)g_MpSetup.options,
				(s32)g_MpSetup.scorelimit, (s32)g_MpSetup.timelimit);
		sysLogPrintf(LOG_CHAT, "STATUS: playlist=%s (%d entries, vote=%ds/%dcand)",
				g_NetPlaylistPath, (s32)g_NetPlaylist.count,
				(s32)g_NetPlaylist.vote_seconds, (s32)g_NetPlaylist.vote_candidates);
	} else if (strcmp(cmd, "fps") == 0) {
		// Toggle the render-time Lua overlay (scripts/perf_overlay.lua reads
		// g_LuaShowFps via pd.perf()).
		extern s32 g_LuaShowFps;
		g_LuaShowFps = (*arg) ? !(strcmp(arg, "0") == 0 || strcmp(arg, "off") == 0) : !g_LuaShowFps;
		sysLogPrintf(LOG_CHAT, "OVERLAY: render-time %s", g_LuaShowFps ? "ON" : "OFF");
	} else if (strcmp(cmd, "mem") == 0) {
		// Toggle the memory Lua overlay (per-frame vtx pool usage).
		extern s32 g_LuaShowMem;
		g_LuaShowMem = (*arg) ? !(strcmp(arg, "0") == 0 || strcmp(arg, "off") == 0) : !g_LuaShowMem;
		sysLogPrintf(LOG_CHAT, "OVERLAY: memory (vtx pool) %s", g_LuaShowMem ? "ON" : "OFF");
	} else if (strcmp(cmd, "graslu") == 0) {
		// Hidden vanity easter egg — toggle the lower-left "Graslu" HUD banner
		// (netGrasluRender). Local-only; deliberately omitted from /help.
		g_GrasluEgg = (*arg) ? !(strcmp(arg, "0") == 0 || strcmp(arg, "off") == 0) : !g_GrasluEgg;
		sysLogPrintf(LOG_CHAT, "Graslu: %s", g_GrasluEgg ? "ON" : "OFF");
	} else if (strcmp(cmd, "redvox57") == 0) {
		// Hidden vanity easter egg — toggle the lower-left red "Redvox57" HUD banner
		// (netRedvox57Render). Local-only; deliberately omitted from /help.
		g_Redvox57Egg = (*arg) ? !(strcmp(arg, "0") == 0 || strcmp(arg, "off") == 0) : !g_Redvox57Egg;
		sysLogPrintf(LOG_CHAT, "Redvox57: %s", g_Redvox57Egg ? "ON" : "OFF");
	} else if (strcmp(cmd, "padtest") == 0) {
		// /padtest caps|led R G B|rumble S MS|trig S MS|hp — debug aid for the
		// SDL3 gamepad extras (LED colours / trigger rumble, PORT_SDL3_EXTRAS.md).
		// Body lives in input.c (needs the SDL_Gamepad handles). Local-only.
		inputPadTest(arg);
	} else if (strcmp(cmd, "gyro") == 0) {
		// /gyro [on|off|sens X [Y]|status] — gyro aim (pad 1, SDL3 gamepad
		// sensors; PORT_SDL3_EXTRAS.md). Body lives in input.c. Local-only.
		inputGyroCommand(arg);
	} else if (strcmp(cmd, "tonal") == 0) {
		// /tonal [on|off]   toggle the Tonal Inversion cheat
		// (CHEAT_TONALINVERSION) live: reflects every music note around middle
		// C in the sequence player (strict melodic inversion; SFX unaffected).
		// Cosmetic-only like Mirror: lives in the ENABLED bank only, never
		// counts as an active cheat. See docs/PORT_TONAL_INVERSION.md.
		extern u32 g_CheatsEnabledBank1;
		const u32 bit = 1 << (CHEAT_TONALINVERSION - 32);
		s32 on = !(g_CheatsEnabledBank1 & bit);
		if (*arg) {
			on = !(strcmp(arg, "0") == 0 || strcmp(arg, "off") == 0);
		}
		if (on) {
			g_CheatsEnabledBank1 |= bit;
		} else {
			g_CheatsEnabledBank1 &= ~bit;
		}
		sysLogPrintf(LOG_CHAT, "tonal inversion %s", on ? "ON" : "OFF");
	} else if (strcmp(cmd, "wireframe") == 0 || strcmp(cmd, "wf") == 0) {
		// /wireframe [on|off]        toggle the Wireframe cheat (CHEAT_WIREFRAME)
		//                            live, no stage reload.
		// /wireframe bg RRGGBB       sky backdrop colour (default black)
		// /wireframe wire RRGGBB|off flat wire colour, or off = natural/textured
		// /wireframe thick N         wire thickness in pixels (1..16)
		// /wireframe vomit           animate bg/wire hue + thickness (seizure mode)
		// /wireframe trip            same animation, 4x slower
		// /wireframe save / load     persist sky/wire colour + thickness to pd.ini
		// Bare RRGGBB is also accepted as a bg shortcut. Setting bg/wire/thick/
		// vomit/trip turns wireframe on. Works outside a net session.
		extern u32 g_CheatsActiveBank1;
		extern u32 g_CheatsEnabledBank1;
		extern u8 g_WireframeBgColour[3];
		extern int gfx_wireframe_wire_color_enabled;
		extern f32 gfx_wireframe_wire_color[3];
		extern f32 gfx_wireframe_line_width;
		extern s32 g_WireframeAnimSpeed;
		const u32 bit = 1u << (CHEAT_WIREFRAME - 32);

		// Split arg into <sub> (first token) and <val> (the remainder).
		char sub[16];
		const char *val = arg;
		s32 si = 0;
		while (*val == ' ') {
			val++;
		}
		while (val[si] && val[si] != ' ' && si < (s32)sizeof(sub) - 1) {
			sub[si] = val[si];
			si++;
		}
		sub[si] = '\0';
		val += si;
		while (*val == ' ') {
			val++;
		}

		u8 rgb[3];

		if (strcmp(sub, "bg") == 0) {
			if (netParseHexColour(val, rgb)) {
				g_WireframeBgColour[0] = rgb[0];
				g_WireframeBgColour[1] = rgb[1];
				g_WireframeBgColour[2] = rgb[2];
				g_CheatsActiveBank1 |= bit;
				g_CheatsEnabledBank1 |= bit;
				sysLogPrintf(LOG_CHAT, "wireframe bg=%02x%02x%02x", rgb[0], rgb[1], rgb[2]);
			} else {
				sysLogPrintf(LOG_CHAT, "usage: /wireframe bg RRGGBB");
			}
		} else if (strcmp(sub, "wire") == 0) {
			if (strcmp(val, "off") == 0 || strcmp(val, "natural") == 0) {
				gfx_wireframe_wire_color_enabled = 0;
				sysLogPrintf(LOG_CHAT, "wireframe wire=natural");
			} else if (netParseHexColour(val, rgb)) {
				gfx_wireframe_wire_color[0] = rgb[0] / 255.0f;
				gfx_wireframe_wire_color[1] = rgb[1] / 255.0f;
				gfx_wireframe_wire_color[2] = rgb[2] / 255.0f;
				gfx_wireframe_wire_color_enabled = 1;
				g_CheatsActiveBank1 |= bit;
				g_CheatsEnabledBank1 |= bit;
				sysLogPrintf(LOG_CHAT, "wireframe wire=%02x%02x%02x", rgb[0], rgb[1], rgb[2]);
			} else {
				sysLogPrintf(LOG_CHAT, "usage: /wireframe wire RRGGBB|off");
			}
		} else if (strcmp(sub, "thick") == 0) {
			if (*val) {
				const f32 w = (f32)atof(val);
				gfx_wireframe_line_width = (w < 1.0f) ? 1.0f : (w > 16.0f ? 16.0f : w);
				g_CheatsActiveBank1 |= bit;
				g_CheatsEnabledBank1 |= bit;
				sysLogPrintf(LOG_CHAT, "wireframe thick=%.1f", gfx_wireframe_line_width);
			} else {
				sysLogPrintf(LOG_CHAT, "wireframe thick=%.1f (usage: /wireframe thick N)", gfx_wireframe_line_width);
			}
		} else if (strcmp(sub, "vomit") == 0 || strcmp(sub, "trip") == 0) {
			// Same animation; vomit = fast (4 deg/frame), trip = 4x slower.
			const s32 myspeed = (sub[0] == 'v') ? 4 : 1;
			if (strcmp(val, "off") == 0 || strcmp(val, "0") == 0) {
				g_WireframeAnimSpeed = 0;
			} else if (strcmp(val, "on") == 0 || strcmp(val, "1") == 0) {
				g_WireframeAnimSpeed = myspeed;
			} else {
				g_WireframeAnimSpeed = (g_WireframeAnimSpeed == myspeed) ? 0 : myspeed;
			}
			if (g_WireframeAnimSpeed != 0) {
				// Animation drives the flat wire colour; turn wireframe on too so
				// there's something to look at.
				g_CheatsActiveBank1 |= bit;
				g_CheatsEnabledBank1 |= bit;
			}
			sysLogPrintf(LOG_CHAT, "wireframe %s %s", sub,
					g_WireframeAnimSpeed != 0 ? "ON" : "OFF");
		} else if (strcmp(sub, "save") == 0) {
			netWireframeCfgRegister();
			netWireframeCfgSnapshot();
			if (configSave(CONFIG_PATH)) {
				sysLogPrintf(LOG_CHAT, "wireframe: saved sky/wire colour + thickness to " CONFIG_FNAME);
			} else {
				sysLogPrintf(LOG_CHAT, "wireframe: save failed");
			}
		} else if (strcmp(sub, "load") == 0) {
			netWireframeCfgRegister();
			netWireframeCfgSnapshot(); // current look = fallback for keys absent from the file
			if (configLoad(CONFIG_PATH)) {
				netWireframeCfgApply();
				g_WireframeAnimSpeed = 0; // stop vomit/trip so the loaded static look shows
				g_CheatsActiveBank1 |= bit;
				g_CheatsEnabledBank1 |= bit;
				sysLogPrintf(LOG_CHAT, "wireframe: loaded from " CONFIG_FNAME);
			} else {
				sysLogPrintf(LOG_CHAT, "wireframe: load failed (no " CONFIG_FNAME "?)");
			}
		} else if (netParseHexColour(sub, rgb)) {
			// Bare RRGGBB shortcut == /wireframe bg RRGGBB.
			g_WireframeBgColour[0] = rgb[0];
			g_WireframeBgColour[1] = rgb[1];
			g_WireframeBgColour[2] = rgb[2];
			g_CheatsActiveBank1 |= bit;
			g_CheatsEnabledBank1 |= bit;
			sysLogPrintf(LOG_CHAT, "wireframe bg=%02x%02x%02x", rgb[0], rgb[1], rgb[2]);
		} else {
			bool on;
			if (!sub[0]) {
				on = !(g_CheatsActiveBank1 & bit);
			} else {
				on = !(strcmp(sub, "0") == 0 || strcmp(sub, "off") == 0);
			}
			if (on) {
				g_CheatsActiveBank1 |= bit;
				g_CheatsEnabledBank1 |= bit;
			} else {
				g_CheatsActiveBank1 &= ~bit;
				g_CheatsEnabledBank1 &= ~bit;
			}
			sysLogPrintf(LOG_CHAT, "wireframe %s", on ? "ON" : "OFF");
		}
	} else if (strcmp(cmd, "mirror") == 0) {
		// /mirror [on|off]   toggle the Mirror cheat (CHEAT_MIRROR) live, no stage
		// reload. Flips the whole rendered 3D scene left-right (works everywhere,
		// including the Carrington Institute hub); 2D HUD/text stay readable. The
		// on/off bit flows to gfx_mirror_mode via bgTickPortals next frame.
		// Mirror is cosmetic-only: it lives in the ENABLED bank only and never
		// enters the active bank (cheatIsActive special-cases it), so it doesn't
		// flag the game as cheated or block mission completion / saving.
		extern u32 g_CheatsEnabledBank1;
		const u32 bit = 1u << (CHEAT_MIRROR - 32);
		bool on;
		if (!arg[0]) {
			on = !(g_CheatsEnabledBank1 & bit);
		} else {
			on = !(strcmp(arg, "0") == 0 || strcmp(arg, "off") == 0);
		}
		if (on) {
			g_CheatsEnabledBank1 |= bit;
		} else {
			g_CheatsEnabledBank1 &= ~bit;
		}
		sysLogPrintf(LOG_CHAT, "mirror %s", on ? "ON" : "OFF");
	} else if (strcmp(cmd, "octree") == 0) {
		// /octree [on|off]    toggle outdoor-room octree frustum culling
		// /octree forcecull   debug: cull every batch (flagged rooms go black)
		// /octree stats       print last-frame culling counters
		// Drives g_BgOctree* in bg.c (ROOMFLAG_EX_OCTREE rooms; see PORT_OCTREE.md).
		if (strcmp(arg, "stats") == 0) {
			sysLogPrintf(LOG_CHAT, "OCTREE: culling=%s forcecull=%s",
					g_BgOctreeEnabled ? "ON" : "OFF",
					g_BgOctreeForceCullAll ? "ON" : "OFF");
			sysLogPrintf(LOG_CHAT, "OCTREE: passes=%d nodes tested=%d culled=%d",
					g_BgOctreeStats.roomsculled, g_BgOctreeStats.nodestested,
					g_BgOctreeStats.nodesculled);
			sysLogPrintf(LOG_CHAT, "OCTREE: batches drawn=%d culled=%d",
					g_BgOctreeStats.batchesdrawn, g_BgOctreeStats.batchesculled);
		} else if (strcmp(arg, "forcecull") == 0 || strcmp(arg, "cull") == 0) {
			g_BgOctreeForceCullAll = !g_BgOctreeForceCullAll;
			sysLogPrintf(LOG_CHAT, "OCTREE: force-cull-all %s",
					g_BgOctreeForceCullAll ? "ON (flagged rooms go black)" : "OFF");
		} else if (strcmp(arg, "mark") == 0) {
			s32 r = bgOctreeMarkCurrentRoom();
			if (r > 0) {
				sysLogPrintf(LOG_CHAT, "OCTREE: marked current room %d (try /octree forcecull)", r);
			} else {
				sysLogPrintf(LOG_CHAT, "OCTREE: couldn't mark current room (in a loaded room?)");
			}
		} else if (strcmp(arg, "markall") == 0 || strcmp(arg, "mark all") == 0) {
			g_BgOctreeMarkAll = !g_BgOctreeMarkAll;
			sysLogPrintf(LOG_CHAT, "OCTREE: mark-all %s (every loaded room octree-culled, lazy-built)",
					g_BgOctreeMarkAll ? "ON" : "OFF");
		} else if (strcmp(arg, "bigroom") == 0) {
			// Treat the whole level as one open space: disable portal room-culling
			// (+ raise the draw-slot cap) and octree-cull every room. Best on open
			// levels -- there's no occlusion culling, so indoor levels render a lot.
			g_BgOctreeBigRoom = !g_BgOctreeBigRoom;
			if (g_BgOctreeBigRoom) {
				g_BgOctreeEnabled = true; // make sure the octree is actually doing the culling
			}
			sysLogPrintf(LOG_CHAT, "OCTREE: big-room %s (portal culling off, whole level octree-culled)",
					g_BgOctreeBigRoom ? "ON" : "OFF");
		} else if (strcmp(arg, "portal") == 0 || strcmp(arg, "portalcull") == 0) {
			// Cull octree geometry to each room's portal-clipped screen box rather
			// than the full viewport, so a room seen through a doorway only submits
			// what's visible through it.
			g_BgOctreePortalCull = !g_BgOctreePortalCull;
			sysLogPrintf(LOG_CHAT, "OCTREE: portal-box culling %s (%s)",
					g_BgOctreePortalCull ? "ON" : "OFF",
					g_BgOctreePortalCull ? "to each room's doorway footprint" : "to full viewport");
		} else if (strcmp(arg, "auto") == 0 || strcmp(arg, "outdoor") == 0) {
			// Auto octree-cull every outdoor room (ROOMFLAG_OUTDOORS, from level
			// data) with no manual /octree mark. Lazy-built per room on first sight.
			g_BgOctreeAutoOutdoor = !g_BgOctreeAutoOutdoor;
			if (g_BgOctreeAutoOutdoor) {
				g_BgOctreeEnabled = true; // make sure the master switch is on
			}
			sysLogPrintf(LOG_CHAT, "OCTREE: auto-outdoor %s (every ROOMFLAG_OUTDOORS room octree-culled)",
					g_BgOctreeAutoOutdoor ? "ON" : "OFF");
		} else if (strcmp(arg, "unmark") == 0) {
			bgOctreeUnmarkAll();
			sysLogPrintf(LOG_CHAT, "OCTREE: cleared all runtime marks (octree culling off everywhere)");
		} else {
			bool on;
			if (!arg[0]) {
				on = !g_BgOctreeEnabled;
			} else {
				on = !(strcmp(arg, "0") == 0 || strcmp(arg, "off") == 0);
			}
			g_BgOctreeEnabled = on;
			sysLogPrintf(LOG_CHAT, "OCTREE: culling %s", on ? "ON" : "OFF");
		}
	} else if (strcmp(cmd, "dlcache") == 0) {
		// /dlcache [on|off]   GPU-resident caching of static room display lists
		// /dlcache stats      print cache counters
		// /dlcache clear      drop all cached buffers (re-record next frame)
		// /dlcache ff         flip cached-geometry backface winding (calibration)
		// Drives g_DlCacheEnabled (bg.c) + the renderer cache; see PORT_DLCACHE.md.
		extern void gfx_dlcache_clear(void);
		extern void gfx_dlcache_set_frontface(int ccw);
		extern int gfx_dlcache_get_frontface(void);
		extern void gfx_dlcache_set_cullmode(int mode);
		extern int gfx_dlcache_get_cullmode(void);
		extern void gfx_dlcache_get_stats(u32 *entries, u32 *bad, u32 *segments, u32 *tris, u32 *reasons);
		if (strcmp(arg, "stats") == 0) {
			u32 entries = 0, bad = 0, segments = 0, tris = 0, reasons = 0;
			gfx_dlcache_get_stats(&entries, &bad, &segments, &tris, &reasons);
			sysLogPrintf(LOG_CHAT, "DLCACHE: %s  cached=%u bad=%u  front=%s",
					g_DlCacheEnabled ? "ON" : "OFF", entries, bad,
					gfx_dlcache_get_frontface() ? "CCW" : "CW");
			sysLogPrintf(LOG_CHAT, "DLCACHE: replayed last frame: batches=%u tris=%u", segments, tris);
			if (reasons) {
				// GFX_DLC_ABORT_* bits (gfx_api.h): why leaves fell back to legacy.
				sysLogPrintf(LOG_CHAT, "DLCACHE: bad reasons:%s%s%s%s%s",
						(reasons & 0x01) ? " fog" : "",
						(reasons & 0x02) ? " lighting" : "",
						(reasons & 0x04) ? " cullboth" : "",
						(reasons & 0x08) ? " empty" : "",
						(reasons & 0x10) ? " texgen" : "");
			}
		} else if (strcmp(arg, "clear") == 0) {
			gfx_dlcache_clear();
			sysLogPrintf(LOG_CHAT, "DLCACHE: cleared all cached buffers");
		} else if (strcmp(arg, "ff") == 0 || strcmp(arg, "frontface") == 0) {
			int ccw = !gfx_dlcache_get_frontface();
			gfx_dlcache_set_frontface(ccw);
			sysLogPrintf(LOG_CHAT, "DLCACHE: cached front-face = %s (if culling looks wrong, flip this)",
					ccw ? "CCW" : "CW");
		} else if (strcmp(arg, "cull") == 0 || strncmp(arg, "cull ", 5) == 0) {
			// /dlcache cull [auto|off|back|front] — diagnose missing geometry.
			const char *m = (arg[4] == ' ') ? arg + 5 : "";
			int mode;
			if (strcmp(m, "off") == 0) {
				mode = 1;
			} else if (strcmp(m, "back") == 0) {
				mode = 2;
			} else if (strcmp(m, "front") == 0) {
				mode = 3;
			} else {
				mode = 0; // auto (per-segment recorded mode)
			}
			gfx_dlcache_set_cullmode(mode);
			sysLogPrintf(LOG_CHAT, "DLCACHE: cached cull = %s",
					mode == 1 ? "OFF (draw both faces)" :
					mode == 2 ? "force BACK" :
					mode == 3 ? "force FRONT" : "auto (per-segment)");
		} else {
			bool on;
			if (!arg[0]) {
				on = !g_DlCacheEnabled;
			} else {
				on = !(strcmp(arg, "0") == 0 || strcmp(arg, "off") == 0);
			}
			g_DlCacheEnabled = on;
			if (!on) {
				gfx_dlcache_clear();
			}
			sysLogPrintf(LOG_CHAT, "DLCACHE: %s", on ? "ON" : "OFF");
		}
	} else if (strcmp(cmd, "gpu") == 0 || strcmp(cmd, "renderer") == 0) {
		// /gpu — show the active rendering backend; for SDL_GPU also the
		// driver (vulkan/direct3d12/metal), shader format, msaa, vsync and
		// shader-cache state. See docs/PORT_SDLGPU.md. Routed here like the
		// other non-net debug commands (/wireframe, /padtest).
		extern void videoGetRendererInfo(char *buf, u32 len);
		char info[256];
		videoGetRendererInfo(info, sizeof(info));
		sysLogPrintf(LOG_CHAT, "GPU: %s", info);
	} else if (strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) {
		sysLogPrintf(LOG_CHAT, "NET commands:");
		sysLogPrintf(LOG_CHAT, "  /lag <ms>        artificial outgoing latency (0 = off)");
		sysLogPrintf(LOG_CHAT, "  /loss <N>        drop ~1 in N unreliable packets (0 = off)");
		sysLogPrintf(LOG_CHAT, "  /diag <path>     start diag log to file (no arg = stop)");
		sysLogPrintf(LOG_CHAT, "  /diagrate <n>    ticks between pos dumps (0 = disable dumps)");
		sysLogPrintf(LOG_CHAT, "  /netinfo         print current net state + tuning knobs");
		sysLogPrintf(LOG_CHAT, "  /igtick          print local in-game tick rate + GE iframe state");
		sysLogPrintf(LOG_CHAT, "  /slomo           print slow-motion / combat-boost decision state");
		sysLogPrintf(LOG_CHAT, "  /wireframe [on|off]              toggle wireframe (CHEAT_WIREFRAME)");
		sysLogPrintf(LOG_CHAT, "  /wireframe bg|wire RRGGBB        sky / wire colour (wire off = natural)");
		sysLogPrintf(LOG_CHAT, "  /wireframe thick N               wire thickness in pixels (1..16)");
		sysLogPrintf(LOG_CHAT, "  /wireframe vomit|trip            animate bg/wire hue + thickness (trip = 4x slower)");
		sysLogPrintf(LOG_CHAT, "  /wireframe save|load             persist sky/wire colour + thickness to pd.ini");
		sysLogPrintf(LOG_CHAT, "  /mirror [on|off]                 flip the world left-right (CHEAT_MIRROR)");
		sysLogPrintf(LOG_CHAT, "  /octree [on|off|forcecull|stats] outdoor-room octree culling");
		sysLogPrintf(LOG_CHAT, "  /octree mark|markall|unmark      flag current room / every room (test anywhere)");
		sysLogPrintf(LOG_CHAT, "  /octree auto                     auto-cull every outdoor room (no manual mark)");
		sysLogPrintf(LOG_CHAT, "  /octree bigroom                  portal culling off + octree-cull whole level");
		sysLogPrintf(LOG_CHAT, "  /octree portal                   cull to room's doorway footprint vs viewport (default on)");
		sysLogPrintf(LOG_CHAT, "  /dlcache [on|off|stats|clear|ff]  cache static room geometry on the GPU");
		sysLogPrintf(LOG_CHAT, "  /dlcache cull [auto|off|back|front] cached backface-cull mode (debug missing rooms)");
		sysLogPrintf(LOG_CHAT, "  /gpu                             show active renderer (+SDL_GPU driver/format/msaa)");
		sysLogPrintf(LOG_CHAT, "  /fps   [on|off]                  render-time overlay (fps + frame ms)");
		sysLogPrintf(LOG_CHAT, "  /mem   [on|off]                  memory overlay (per-frame vtx pool)");
		sysLogPrintf(LOG_CHAT, "  /spec [name|next|prev|off]  follow another player/sim");
		sysLogPrintf(LOG_CHAT, "  /interp <n>      entity interpolation ticks (default 3)");
		sysLogPrintf(LOG_CHAT, "  /stale <n>       snap-on-stale threshold ticks (default 30)");
		sysLogPrintf(LOG_CHAT, "  /svcrate <n>     server update interval, ticks (default 1)");
		sysLogPrintf(LOG_CHAT, "  /clcrate <n>     client update interval, ticks (default 1)");
		sysLogPrintf(LOG_CHAT, "  /lagcomp x        hit-rewind mode: exact|legacy (default exact)");
		sysLogPrintf(LOG_CHAT, "  /cspframes <n>   CSP smooth-correction window (default 10)");
		sysLogPrintf(LOG_CHAT, "  /cspcorr <u>     CSP min correction error, units (default 25)");
		sysLogPrintf(LOG_CHAT, "  /cspteleport <u> CSP hard-snap threshold, units (default 120)");
		sysLogPrintf(LOG_CHAT, "Server commands (host only):");
		sysLogPrintf(LOG_CHAT, "  /playlist [list|reload]  show or reload server_playlist.ini");
		sysLogPrintf(LOG_CHAT, "  /nextmap [index]         force next playlist entry");
		sysLogPrintf(LOG_CHAT, "  /kick <name|id>          disconnect a client");
		sysLogPrintf(LOG_CHAT, "  /say <msg>               broadcast a server chat line");
		sysLogPrintf(LOG_CHAT, "  /endmatch                force the current round to end");
		sysLogPrintf(LOG_CHAT, "  /players                 list connected clients");
		sysLogPrintf(LOG_CHAT, "  /status                  dump server / match state");
		sysLogPrintf(LOG_CHAT, "Client commands (during a vote):");
		sysLogPrintf(LOG_CHAT, "  /vote <N>                vote for candidate N");
	} else {
		sysLogPrintf(LOG_CHAT, "NET: unknown command /%s (try /help)", cmd);
	}

	return 1;
}

// --- Kill feed ---
// Rolling list of recent eliminations shown top-left. New entries go to slot 0
// and older ones shift down toward NET_KILLFEED_MAX-1; entries past their
// expire_tick are skipped at render time and overwritten by the next addition.
//
// Each entry stores shooter and victim names separately so the renderer can
// colour each side independently. An empty shooter[0] means the victim died
// alone (suicide / environment); in that case the line renders as
// "victim [died]" with the victim in red and "[died]" in white.
static struct netkillfeedentry g_NetKillFeed[NET_KILLFEED_MAX];
struct netlobbystate g_NetLobbyState;

// Colour palette — keep saturated so each name reads at a glance even at
// extra-small console-font size. Alpha 0xff: the feed is short-lived so
// fading is unnecessary. SHOOTER/VICTIM colours are FALLBACKS used when a
// team is unknown (0xff) — when a team is present, netKillFeedTeamColor
// substitutes the matching g_TeamColours entry so the feed visually agrees
// with radar / on-chr highlights.
#define NET_KILLFEED_COL_SHOOTER 0x33ff33ff  // bright green
#define NET_KILLFEED_COL_VICTIM  0xff4444ff  // bright red
#define NET_KILLFEED_COL_PLAIN   0xffffffff  // white separator / "[died]"
// Outline colour passed as textRender's second colour arg — sets PRIMITIVE in
// the dual-cycle combiner so character edges (TEXEL1_ALPHA areas) draw black
// while the body (TEXEL0_ALPHA fill) takes the per-segment colour. Matches the
// FPS counter's 0x000000a0 so both overlays read with the same outline weight.
#define NET_KILLFEED_COL_OUTLINE 0x000000a0  // black, alpha 0xa0

// Map a team index to a renderable RGBA colour. g_TeamColours is RGB0-format
// (alpha byte = 0 because it's authored for the radar's RGB combiner that
// supplies alpha elsewhere), so OR in 0xff for text rendering. team==0xff
// is the "unknown / no team" sentinel — return the supplied fallback.
static inline u32 netKillFeedTeamColor(u8 team, u32 fallback)
{
	if (team == 0xff) {
		return fallback;
	}
	// g_TeamColours has 8 entries (one per MPTEAM). Out-of-range teams
	// (corrupt wire data, future expansion) fall back rather than
	// indexing past the array.
	if (team >= 8) {
		return fallback;
	}
	return g_TeamColours[team] | 0xffu;
}

static void netKillFeedClear(void)
{
	memset(g_NetKillFeed, 0, sizeof(g_NetKillFeed));
}

// Copy at most NET_KILLFEED_NAME-1 chars into dst, stopping at the first '\n'
// because chr-config names embed it as a width marker for the in-game HUD
// font and that leaks ugly box-drawing characters into the feed otherwise.
static void killFeedCopyName(char *dst, const char *src)
{
	dst[0] = '\0';
	if (!src) {
		return;
	}
	s32 i;
	for (i = 0; i < NET_KILLFEED_NAME - 1; ++i) {
		const char c = src[i];
		if (c == '\0' || c == '\n') {
			break;
		}
		dst[i] = c;
	}
	dst[i] = '\0';
}

void netKillFeedAdd(const char *shooter, const char *victim, u8 shooter_team, u8 victim_team)
{
	if (!victim || !victim[0]) {
		return;
	}

	// Shift older entries down, freshest goes at index 0.
	for (s32 i = NET_KILLFEED_MAX - 1; i > 0; --i) {
		g_NetKillFeed[i] = g_NetKillFeed[i - 1];
	}

	g_NetKillFeed[0].expire_tick = g_NetTick + NET_KILLFEED_DURATION_TICKS;
	killFeedCopyName(g_NetKillFeed[0].shooter, shooter);
	killFeedCopyName(g_NetKillFeed[0].victim, victim);
	g_NetKillFeed[0].shooter_team = shooter_team;
	g_NetKillFeed[0].victim_team = victim_team;
}

Gfx *netKillFeedRender(Gfx *gdl)
{
	if (!g_NetMode) {
		return gdl;
	}

	// Match the console (~) font so the two HUD overlays read as part of the
	// same surface. Falls back silently if the font assets haven't loaded.
	if (!g_CharsHandelGothicXs || !g_FontHandelGothicXs) {
		return gdl;
	}

	gdl = text0f153628(gdl);
	// Anchor to the left edge — matters in widescreen so the feed hugs the
	// HUD edge instead of floating in from the letterbox. Same flag the
	// console uses for its top-left message strip.
	gSPSetExtraGeometryModeEXT(gdl++, G_ASPECT_LEFT_EXT);

	const s32 screenw = viGetWidth();
	const s32 screenh = viGetHeight();
	const s32 lineHeight = 9;
	const s32 leftMargin = 4;
	const s32 topMargin = 4;

	s32 visible = 0;
	for (s32 i = 0; i < NET_KILLFEED_MAX; ++i) {
		struct netkillfeedentry *e = &g_NetKillFeed[i];
		if (!e->victim[0]) {
			continue;
		}
		if (g_NetTick >= e->expire_tick) {
			// Expired — clear so it doesn't get re-rendered after a wraparound.
			e->victim[0] = '\0';
			e->shooter[0] = '\0';
			continue;
		}

		s32 x = leftMargin;
		s32 y = topMargin + visible * lineHeight;

		// Local player appears red; everyone else appears green. Team games
		// override with team colours regardless of local/remote.
		const char *myname = g_NetLocalClient ? g_NetLocalClient->settings.name : NULL;
		const u32 shooter_fallback = (myname && strcmp(e->shooter, myname) == 0)
				? NET_KILLFEED_COL_VICTIM : NET_KILLFEED_COL_SHOOTER;
		const u32 victim_fallback = (myname && strcmp(e->victim, myname) == 0)
				? NET_KILLFEED_COL_VICTIM : NET_KILLFEED_COL_SHOOTER;
		const u32 shooter_col = netKillFeedTeamColor(e->shooter_team, shooter_fallback);
		const u32 victim_col = netKillFeedTeamColor(e->victim_team, victim_fallback);

		if (e->shooter[0]) {
			// "Shooter > Victim" — three segments, each with its own colour.
			// textRender (vs. textRenderProjected) takes a second colour for the
			// dual-cycle combiner so we get a black outline around each glyph,
			// matching the FPS counter. It mutates x to the end of the rendered
			// text so consecutive calls line up without manual width math.
			gdl = textRender(gdl, &x, &y, e->shooter,
					g_CharsHandelGothicXs, g_FontHandelGothicXs,
					shooter_col, NET_KILLFEED_COL_OUTLINE,
					screenw, screenh, 0, 0);

			gdl = textRender(gdl, &x, &y, " > ",
					g_CharsHandelGothicXs, g_FontHandelGothicXs,
					NET_KILLFEED_COL_PLAIN, NET_KILLFEED_COL_OUTLINE,
					screenw, screenh, 0, 0);

			gdl = textRender(gdl, &x, &y, e->victim,
					g_CharsHandelGothicXs, g_FontHandelGothicXs,
					victim_col, NET_KILLFEED_COL_OUTLINE,
					screenw, screenh, 0, 0);
		} else {
			// "Victim [died]" — suicide / environment kill.
			gdl = textRender(gdl, &x, &y, e->victim,
					g_CharsHandelGothicXs, g_FontHandelGothicXs,
					victim_col, NET_KILLFEED_COL_OUTLINE,
					screenw, screenh, 0, 0);

			gdl = textRender(gdl, &x, &y, " [died]",
					g_CharsHandelGothicXs, g_FontHandelGothicXs,
					NET_KILLFEED_COL_PLAIN, NET_KILLFEED_COL_OUTLINE,
					screenw, screenh, 0, 0);
		}

		++visible;
	}

	gSPClearExtraGeometryModeEXT(gdl++, G_ASPECT_CENTER_EXT);
	gdl = text0f153780(gdl);
	return gdl;
}

// Hidden test feature (toggle /hitmarker): a brief centred marker shown the
// instant the local player's shot registers a chr/player hit, so we can evaluate
// immediate hit feedback at high ping without waiting for the server's
// SVC_CHR_DAMAGE round-trip and without touching the crosshair render. Reuses the
// kill-feed text path (no new gfx primitives). Local-only; not networked.
Gfx *netHitmarkerRender(Gfx *gdl)
{
	if (!g_NetMode || !g_NetHitmarkerEnabled || g_NetTick >= g_NetHitmarkerExpireTick) {
		return gdl;
	}
	if (!g_CharsHandelGothicXs || !g_FontHandelGothicXs) {
		return gdl;
	}

	gdl = text0f153628(gdl);

	const s32 screenw = viGetWidth();
	const s32 screenh = viGetHeight();

	// Fade alpha out over the remaining lifetime.
	const u32 remain = g_NetHitmarkerExpireTick - g_NetTick;
	u32 a = (remain * 255u) / NET_HITMARKER_TICKS;
	if (a > 255u) { a = 255u; }
	const u32 col = 0xffffff00u | a;

	// Centre an "X" mark on the reticle (screen centre). textRender mutates x.
	char mark[] = "X";
	s32 x = screenw / 2 - 3;
	s32 y = screenh / 2 - 4;
	gdl = textRender(gdl, &x, &y, mark,
			g_CharsHandelGothicXs, g_FontHandelGothicXs,
			col, 0x00000080u, screenw, screenh, 0, 0);

	return gdl;
}

// Hidden vanity easter egg honouring the Perfect Dark content creator Graslu.
// Deliberately mirrors the weapon/ammo pickup message style (HUDMSGTYPE_DEFAULT
// in hudmsg.c is a *boxed* message): Handel Gothic Sm font, green text inside a
// green-bordered translucent box. Drawn on the HUD layer — called from
// playerRenderHud right after hudmsgsRender so it shares the pickups' layer —
// and stacked one row above them. Toggled by /graslu. Local-only: not networked.
// ---- Vanity easter-egg banners (/graslu, /redvox57) ----
// A boxed lower-left HUD banner mimicking a weapon/ammo pickup. It now has the
// full notification lifecycle (like the kill feed / lives toast): animate IN when
// the player HUD is drawn, HOLD (a bright highlight marching around the box), then
// animate OUT when the HUD is removed. Fade-in/hold render from playerRenderHud
// (HUD drawn, var80075d60==2); the fade-out renders from lv.c's HUD-removed path
// via netCoopEggsRenderHidden, so the banner slides away instead of popping off.
enum { EGG_HIDDEN, EGG_FADEIN, EGG_HOLD, EGG_FADEOUT };
// Ticks (lvframe60) the mission timer must keep counting before the banner begins
// its fade-in, so it lands just behind the mission actually starting.
#define EGG_STAGE_INTRO_DELAY 30
// How long (lvframe60) the timer may sit flat before we call it paused. The mission
// timer (bondviewlevtime60 += lvupdate60) only advances on whole 1/60s steps, so
// above 60fps it's flat for a few frames at a time even during normal play; tolerate
// that so the banner doesn't flicker, while a real cutscene pause (seconds) trips it.
#define EGG_TIMER_PAUSE_SLACK 20
struct netegg {
	s32 phase;        // EGG_* state
	s32 phasestart;   // g_Vars.lvframe60 at phase entry
	s32 readyframe;   // lvframe60 when the mission timer was first seen counting; -1 while paused
	s32 lasttime;     // last sampled bondviewlevtime60
	s32 lastframe;    // lvframe60 of the last sample (we sample once per game frame)
	s32 lastadvframe; // lvframe60 when the timer last INCREASED; -1 = never
};
static struct netegg g_GrasluAnim = { EGG_HIDDEN, 0, -1, 0, -1, -1 };
static struct netegg g_Redvox57Anim = { EGG_HIDDEN, 0, -1, 0, -1, -1 };

static Gfx *netEggRender(Gfx *gdl, const char *text, u32 bordercol, u32 textcol, u32 hicol, bool want, struct netegg *anim)
{
	// Gate: only on the HUD layer, in a running level, with the pickup font loaded.
	if (g_Vars.stagenum == STAGE_TITLE || !g_CharsHandelGothicSm || !g_FontHandelGothicSm) {
		want = false;
	}

	s32 lvf = g_Vars.lvframe60;

	if (lvf < anim->phasestart) {
		// Stage (re)load — lvframe60 was reset to 0. HARD-reset so nothing carries
		// across the reload; this is what keeps a mission RESTART clean (no stick, no
		// stale fade state). Also covers a fresh stage load.
		anim->phasestart = lvf;
		anim->phase = EGG_HIDDEN;
		anim->readyframe = -1;
		anim->lasttime = g_Vars.currentplayer->bondviewlevtime60;
		anim->lastframe = lvf;
		anim->lastadvframe = -1;
	} else if (g_InCutscene || g_MainIsEndscreen) {
		// MID-mission cutscene / end screen (stage not reloaded): stop counting toward
		// the fade-in and don't re-arm, but DON'T snap the banner away — fall through
		// so a fully-shown banner fades OUT gracefully over the cutscene. It keeps
		// rendering via the HUD path while the HUD is up, and via lv.c's HUD-removed
		// path (netCoopEggsRenderHidden) when it isn't — so the fade-out is visible
		// either way. A fade-in caught in progress snaps to hidden in the edge
		// transitions below. Skip the timer sample (lastframe=lvf) so the paused timer
		// can't re-arm here, and leave phase/phasestart alone so the fade-out animates
		// from where it started.
		anim->readyframe = -1;
		anim->lasttime = g_Vars.currentplayer->bondviewlevtime60;
		anim->lastframe = lvf;
		anim->lastadvframe = -1;
	}

	// Track the MISSION OVERALL timer (bondviewlevtime60 / playerGetMissionTime).
	// Unlike the raw stage timer (g_StageTimeElapsed60, which counts from stage load),
	// this one only counts up once the mission has started and the HUD is live — it
	// stays 0 through the intro cutscene and pauses during mid-mission cutscenes
	// (player.c playerTick gates it on (tickmode GE_FADEIN|NORMAL) && !g_InCutscene &&
	// !g_MainIsEndscreen). Sample once per game frame and watch it INCREASE (it's
	// reset to 0 at stage start — a decrease that must NOT count as advancing, hence
	// `>` not `!=`); treat the timer as running while the last increase was within
	// EGG_TIMER_PAUSE_SLACK frames (rides over its 1/60s granularity at high fps). The
	// banner shows only while the timer counts: fades in just after it starts, out
	// when it pauses for a cutscene, back in on resume. A short settle keeps the
	// fade-in just behind the timer.
	if (lvf != anim->lastframe) {
		s32 t = g_Vars.currentplayer->bondviewlevtime60;
		if (t > anim->lasttime) {
			anim->lastadvframe = lvf; // timer counted up this frame
		}
		anim->lasttime = t;
		anim->lastframe = lvf;

		bool running = anim->lastadvframe >= 0 && (lvf - anim->lastadvframe) <= EGG_TIMER_PAUSE_SLACK;
		if (!running) {
			anim->readyframe = -1; // paused: intro / cutscene / end screen
		} else if (anim->readyframe < 0) {
			// Timer resumed after a pause: arm a fresh fade-in and snap away any
			// leftover fade. Without this, a fade-out that STARTED during a short
			// cutscene (one shorter than the fade animation) would run to completion
			// after the cutscene ends and then fade back in — the "fades out then back
			// in at the cutscene end" glitch. Snapping to HIDDEN here means a single
			// clean fade-in instead.
			anim->readyframe = lvf;
			anim->phase = EGG_HIDDEN;
			anim->phasestart = lvf;
		}
	}
	const bool show = want && anim->readyframe >= 0 && (lvf - anim->readyframe) >= EGG_STAGE_INTRO_DELAY;

	// Edge transitions: fade in once shown; when it should hide, fade OUT only if it
	// was fully shown (HOLD). If it's interrupted while still fading IN — a cutscene
	// triggering or the mission restarting before the fade-in finishes — snap it
	// straight to HIDDEN instead of reversing into a fade-out, so we don't get the
	// "fade out then fade back in" glitch; it just fades in once, cleanly, afterward.
	if (show && (anim->phase == EGG_HIDDEN || anim->phase == EGG_FADEOUT)) {
		anim->phase = EGG_FADEIN;
		anim->phasestart = lvf;
	} else if (!show) {
		if (anim->phase == EGG_HOLD) {
			anim->phase = EGG_FADEOUT;
			anim->phasestart = lvf;
		} else if (anim->phase == EGG_FADEIN) {
			anim->phase = EGG_HIDDEN; // interrupted mid-fade-in: snap, don't reverse-fade
		}
	}

	if (anim->phase == EGG_HIDDEN) {
		return gdl;
	}

	// Measure: height from "<text>\n" (one pickup line), width from "<text>".
	char nl[24];
	s32 lineh = 0;
	s32 tw = 0;
	s32 discard = 0;
	snprintf(nl, sizeof(nl), "%s\n", text);
	textMeasure(&lineh, &discard, nl, g_CharsHandelGothicSm, g_FontHandelGothicSm, 0);
	textMeasure(&discard, &tw, (char *)text, g_CharsHandelGothicSm, g_FontHandelGothicSm, 0);

	const s32 screenw = viGetWidth();
	const s32 screenh = viGetHeight();
	s32 x = 27;
	s32 y = screenh - 2 * lineh - 24;
	if (cheatIsActive(CHEAT_MIRROR)) {
		// One pixel left of the exact reflection so the mirrored banner lines up.
		x = screenw - x - tw - 1;
	}
	const s32 bx1 = x - 3;
	const s32 by1 = y - 3;
	const s32 bx2 = x + tw + 3;
	const s32 by2 = y + lineh + 2;

	// The pickup-style wipe duration (also the fade-in / fade-out length).
	const f32 animtime = (sqrtf((f32)(tw * tw + lineh * lineh)) + 132.0f) / PALUPF(7.0f);
	const f32 dur = animtime > 30.0f ? 30.0f : animtime;
	const s32 elapsed = lvf - anim->phasestart;

	gdl = text0f153628(gdl);
	gSPSetExtraGeometryModeEXT(gdl++, cheatIsActive(CHEAT_MIRROR) ? G_ASPECT_RIGHT_EXT : G_ASPECT_LEFT_EXT);

	if (anim->phase == EGG_FADEIN) {
		f32 a = (f32)elapsed / dur;
		if (a > 1.0f) a = 1.0f;
		if (a < 0.0f) a = 0.0f;

		textSetDiagonalBlend(x, y, (f32)elapsed * PALUPF(7.0f), DIAGMODE_FADEIN);
		gdl = hudmsgRenderBox(gdl, bx1, by1, bx2, by2, 1.0f, bordercol, a);
		if (a > 0.0f) {
			gdl = textRenderProjected(gdl, &x, &y, (char *)text, g_CharsHandelGothicSm, g_FontHandelGothicSm, textcol, screenw, screenh, 0, 0);
		}
		textResetBlends();

		if ((f32)elapsed >= animtime) {
			anim->phase = EGG_HOLD;
			anim->phasestart = lvf;
		}
	} else if (anim->phase == EGG_FADEOUT) {
		// Mirror of the hud-message fade-out (hudmsg.c): a diagonal wipe from the far
		// corner with a counting-down timer, box alpha ramping to 0.
		f32 a = (f32)elapsed / dur;
		if (a > 1.0f) a = 1.0f;
		if (a < 0.0f) a = 0.0f;

		textSetDiagonalBlend(x + tw, y + lineh, (animtime - (f32)elapsed) * PALUPF(7.0f), DIAGMODE_FADEOUT);
		gdl = hudmsgRenderBox(gdl, bx1, by1, bx2, by2, 1.0f, bordercol, 1.0f - a);
		if (a < 1.0f) {
			gdl = textRenderProjected(gdl, &x, &y, (char *)text, g_CharsHandelGothicSm, g_FontHandelGothicSm, textcol, screenw, screenh, 0, 0);
		}
		textResetBlends();

		if ((f32)elapsed >= animtime) {
			anim->phase = EGG_HIDDEN;
		}
	} else {
		// EGG_HOLD: solid box + text, with a bright highlight marching around the
		// outline (top -> right -> bottom -> left, looping ~2px/tick).
		const s32 bw = bx2 - bx1;
		const s32 bh = by2 - by1;
		const s32 perim = 2 * (bw + bh);
		const s32 seg = 10;
		const s32 p = perim > 0 ? (lvf * 2) % perim : 0;
		s32 sx1, sy1, sx2, sy2;

		gdl = hudmsgRenderBox(gdl, bx1, by1, bx2, by2, 1.0f, bordercol, 1.0f);
		gdl = textRenderProjected(gdl, &x, &y, (char *)text, g_CharsHandelGothicSm, g_FontHandelGothicSm, textcol, screenw, screenh, 0, 0);

		if (p < bw) {
			sx1 = bx1 + p;       sy1 = by1;
			sx2 = sx1 + seg;     sy2 = by1 + 2;
			if (sx2 > bx2) sx2 = bx2;
		} else if (p < bw + bh) {
			sx1 = bx2 - 1;       sy1 = by1 + (p - bw);
			sx2 = bx2 + 1;       sy2 = sy1 + seg;
			if (sy2 > by2) sy2 = by2;
		} else if (p < 2 * bw + bh) {
			sx2 = bx2 - (p - bw - bh); sy1 = by2 - 1;
			sx1 = sx2 - seg;     sy2 = by2 + 1;
			if (sx1 < bx1) sx1 = bx1;
		} else {
			sx1 = bx1;           sy2 = by2 - (p - 2 * bw - bh);
			sx2 = bx1 + 2;       sy1 = sy2 - seg;
			if (sy1 < by1) sy1 = by1;
		}
		gdl = menugfxDrawFilledRect(gdl, sx1, sy1, sx2, sy2, hicol, hicol);
	}

	gSPClearExtraGeometryModeEXT(gdl++, G_ASPECT_CENTER_EXT);
	gdl = text0f153780(gdl);
	return gdl;
}

Gfx *netGrasluRender(Gfx *gdl)
{
	return netEggRender(gdl, "Graslu", 0x00ff0040, 0x00ff00a0, 0x80ff80ff, g_GrasluEgg != 0, &g_GrasluAnim);
}

Gfx *netRedvox57Render(Gfx *gdl)
{
	return netEggRender(gdl, "Redvox57", 0xff000040, 0xff0000a0, 0xff8080ff, g_Redvox57Egg != 0, &g_Redvox57Anim);
}

// HUD-removed frames (lv.c var80075d60 != 2): drive both banners' fade-out
// (want=false) so they animate away. No-op once each has finished fading.
Gfx *netCoopEggsRenderHidden(Gfx *gdl)
{
	gdl = netEggRender(gdl, "Graslu", 0x00ff0040, 0x00ff00a0, 0x80ff80ff, false, &g_GrasluAnim);
	gdl = netEggRender(gdl, "Redvox57", 0xff000040, 0xff0000a0, 0xff8080ff, false, &g_Redvox57Anim);
	return gdl;
}

Gfx *netDebugRender(Gfx *gdl)
{
	char tmp[2048];

	if (!g_NetMode || !g_NetDebugDraw) {
		return gdl;
	}

	if (!g_CharsHandelGothicXs || !g_FontHandelGothicXs) {
		return gdl;
	}

	// Compute kB/s bandwidth on a rolling 1-second window so the panel shows
	// a useful rate instead of an ever-growing total.
	static u32 lastSampleTick = 0;
	static u32 lastSentBytes = 0;
	static u32 lastRecvBytes = 0;
	static f32 sentKBps = 0.f;
	static f32 recvKBps = 0.f;
	const u32 curSent = enet_host_get_bytes_sent(g_NetHost);
	const u32 curRecv = enet_host_get_bytes_received(g_NetHost);
	const u32 dt_ticks = g_NetTick - lastSampleTick;
	if (dt_ticks >= 60 || !lastSampleTick) {
		const f32 secs = dt_ticks > 0 ? (f32)dt_ticks / 60.f : 1.f;
		sentKBps = (f32)(curSent - lastSentBytes) / secs / 1024.f;
		recvKBps = (f32)(curRecv - lastRecvBytes) / secs / 1024.f;
		lastSentBytes = curSent;
		lastRecvBytes = curRecv;
		lastSampleTick = g_NetTick;
	}

	gdl = text0f153628(gdl);
	gSPSetExtraGeometryModeEXT(gdl++, G_ASPECT_LEFT_EXT);

	const char *modeStr = (g_NetMode == NETMODE_SERVER) ? "SERVER" : "CLIENT";
	const u32 ownPing = g_NetLocalClient->peer ? enet_peer_get_rtt(g_NetLocalClient->peer) : 0;

	// ENet's RTT measurement is at the protocol layer and doesn't see our
	// app-level lag queue, so /lag never shows up in the raw ping field.
	// Compose an "eff=Nms" suffix to make the user-perceived RTT visible.
	char effSuffix[32] = "";
	if (g_NetSimLagMs > 0) {
		snprintf(effSuffix, sizeof(effSuffix), " eff=%ums", ownPing + (u32)g_NetSimLagMs);
	}

	// Header: connection summary + bandwidth + sim/client counts. Includes a
	// "** SIM ACTIVE **" line whenever fake lag/loss is set so the user
	// doesn't forget the slowdown is artificial.
	s32 off = snprintf(tmp, sizeof(tmp),
		"%s id=%u/%u  tick=%u  ping=%ums%s\n"
		"tx=%.1f kB/s  rx=%.1f kB/s\n"
		"frame: %uR %uU bytes  total=%u/%u\n"
		"clients=%d/%d  sims=%d  interp=%ut\n",
		modeStr, g_NetLocalClient->id, g_NetLocalClient->playernum,
		g_NetTick, ownPing, effSuffix,
		sentKBps, recvKBps,
		g_NetReliableFrameLen, g_NetUnreliableFrameLen,
		curSent, curRecv,
		g_NetNumClients, g_NetMaxClients, g_BotCount, g_NetInterpTicks);

	if (g_NetSimLagMs > 0 || g_NetSimPacketLoss > 0 || g_NetLagQueueDropped > 0) {
		off += snprintf(tmp + off, sizeof(tmp) - off,
			"** SIM ACTIVE ** lag=%dms loss=1/%d qdrop=%d\n"
			"   ENet ping does not include /lag - check /netinfo on each side\n",
			g_NetSimLagMs, g_NetSimPacketLoss, g_NetLagQueueDropped);
	}

	// CSP correction state — only meaningful on the client where reconcile runs
	if (g_NetMode == NETMODE_CLIENT) {
		off += snprintf(tmp + off, sizeof(tmp) - off,
			"CSP: %df  delta=(%.1f,%.1f,%.1f)\n",
			g_NetCspCorrFrames,
			g_NetCspCorrDelta.x, g_NetCspCorrDelta.y, g_NetCspCorrDelta.z);
	}

	// Lag-comp activity — only the server runs it
	if (g_NetMode == NETMODE_SERVER) {
		off += snprintf(tmp + off, sizeof(tmp) - off,
			"lagcomp: last=%d rewinds  ticks=%u\n",
			g_LagCompLastCount, g_LagCompLastRewindTicks);
	}

	// Per-client list: pos, ping, last-tick lag, animnum, key ucmd bits.
	// Position is read from the player prop (the authoritative live world
	// position) — falling back to the inmove snapshot only for clients we
	// haven't fully resolved yet. The local client never receives its own
	// moves so its inmove ring stays empty, which was showing as 0,0,0.
	for (s32 i = 0; i < g_NetMaxClients; ++i) {
		const struct netclient *cl = &g_NetClients[i];
		if (cl->state < CLSTATE_LOBBY) {
			continue;
		}
		if (off >= (s32)sizeof(tmp) - 128) {
			break; // out of buffer
		}

		const struct netplayermove *m = &cl->inmove[cl->inmove_head];
		const u32 ping = cl->peer ? enet_peer_get_rtt(cl->peer) : 0;
		const u32 inLag = (m->tick && g_NetTick > m->tick) ? (g_NetTick - m->tick) : 0;
		const u32 outAckLag = (cl->outmove[0].tick && cl->outmove[0].tick > cl->outmoveack)
			? (cl->outmove[0].tick - cl->outmoveack) : 0;

		// Prefer the live prop position over the snapshot — works for the
		// local client and stays current on remotes once their first move
		// has been applied.
		struct coord livepos = { 0.f, 0.f, 0.f };
		if (cl->player && cl->player->prop) {
			livepos = cl->player->prop->pos;
		} else {
			livepos = m->pos;
		}

		char flags[8] = "....";
		flags[0] = (m->ucmd & UCMD_FIRE)    ? 'F' : '.';
		flags[1] = (m->ucmd & UCMD_AIMMODE) ? 'A' : '.';
		flags[2] = (m->ucmd & UCMD_RELOAD)  ? 'R' : '.';
		flags[3] = (m->ucmd & (UCMD_DUCK|UCMD_SQUAT)) ? 'D' : '.';
		flags[4] = '\0';

		const char *stateStr = "??";
		switch (cl->state) {
			case CLSTATE_CONNECTING: stateStr = "CON"; break;
			case CLSTATE_AUTH:       stateStr = "AUTH"; break;
			case CLSTATE_LOBBY:      stateStr = "LOBBY"; break;
			case CLSTATE_GAME:       stateStr = "GAME"; break;
		}

		const char *youTag = (cl == g_NetLocalClient) ? "*" : " ";
		const char *name = cl->settings.name[0] ? cl->settings.name : "<?>";

		off += snprintf(tmp + off, sizeof(tmp) - off,
			"%s[%u] %-8.8s %s p=%ums in-%u out-%u lerp=%u il=%u\n"
			"   pos=(%.0f,%.0f,%.0f) a=%d/%d [%s]\n",
			youTag, cl->id, name, stateStr,
			ping, inLag, outAckLag, cl->lerpticks, (u32)(cl->interp_lag + 0.5f),
			livepos.x, livepos.y, livepos.z,
			m->animnum, m->animframe, flags);
	}

	// Sim (AI bot) list: not in g_NetClients but they're the other half of
	// what needs syncing. Show each bot's syncid, world pos, current weapon,
	// damage taken, and which player they're attacking.
	s32 numSims = 0;
	if (g_Vars.lvmpbotlevel) {
		for (s32 i = 0; i < g_BotCount; ++i) {
			const struct chrdata *chr = g_MpBotChrPtrs[i];
			if (!chr || !chr->prop) {
				continue;
			}
			if (off >= (s32)sizeof(tmp) - 96) {
				break;
			}
			const struct coord *p = &chr->prop->pos;
			const s32 weapon = chr->aibot ? chr->aibot->weaponnum : -1;
			const s32 target = chr->aibot ? chr->aibot->attackingplayernum : -1;
			off += snprintf(tmp + off, sizeof(tmp) - off,
				" <b%d> sid=%u pos=(%.0f,%.0f,%.0f) w=%d tgt=%d hp=%.0f\n",
				i, chr->prop->syncid, p->x, p->y, p->z,
				weapon, target, chr->maxdamage - chr->damage);
			++numSims;
		}
	}

	// Strip non-ASCII bytes — textRenderProjected on NTSC treats them as JPN
	// multibyte codepoints, which crashes on non-JPN builds.
	for (s32 i = 0; i < off; ++i) {
		if ((u8)tmp[i] >= 0x80) {
			tmp[i] = '?';
		}
	}

	// Position the panel from the bottom — leave enough room for the maximum
	// possible content (header + CSP/lagcomp + up to 8 clients × 2 lines +
	// up to MAX_BOTS sim lines).
	const s32 lineCount = 4 + 1 + (g_NetMaxClients * 2) + numSims + 1;
	s32 x = 2;
	s32 y = viGetHeight() - 1 - (lineCount * 8);
	if (y < 8) y = 8;
	gdl = textRenderProjected(gdl, &x, &y, tmp, g_CharsHandelGothicXs, g_FontHandelGothicXs, 0x00ff00ff, viGetWidth(), viGetHeight(), 0, 0);

	gSPClearExtraGeometryModeEXT(gdl++, G_ASPECT_CENTER_EXT);
	gdl = text0f153780(gdl);

	return gdl;
}

PD_CONSTRUCTOR static void netConfigInit(void)
{
	configRegisterUInt("Net.LerpTicks", &g_NetInterpTicks, 0, 600);

	configRegisterString("Net.Client.LastJoinAddr", g_NetLastJoinAddr, NET_MAX_ADDR);
	configRegisterUInt("Net.Client.InRate", &g_NetClientInRate, 0, 10 * 1024 * 1024);
	configRegisterUInt("Net.Client.OutRate", &g_NetClientOutRate, 0, 10 * 1024 * 1024);
	configRegisterUInt("Net.Client.UpdateFrames", &g_NetClientUpdateRate, 0, 60);

	configRegisterUInt("Net.Server.Port", &g_NetServerPort, 0, 0xFFFF);
	configRegisterUInt("Net.Server.InRate", &g_NetServerInRate, 0, 10 * 1024 * 1024);
	configRegisterUInt("Net.Server.OutRate", &g_NetServerOutRate, 0, 10 * 1024 * 1024);
	configRegisterUInt("Net.Server.UpdateFrames", &g_NetServerUpdateRate, 0, 60);
	configRegisterInt("Net.Server.AllowInfoQuery", &g_NetServerInfoQuery, 0, 1);
	configRegisterInt("Net.Server.HitValidate", &g_NetHitValidate, 0, 2);

	configRegisterString("Net.Debug.LogPath", g_NetDiagPath, sizeof(g_NetDiagPath) - 1);
	configRegisterUInt("Net.Debug.LogRate", &g_NetDiagDumpRate, 0, 600);

	configRegisterString("Server.Name", g_NetServerName, sizeof(g_NetServerName) - 1);
	configRegisterString("Server.PlaylistPath", g_NetPlaylistPath, sizeof(g_NetPlaylistPath) - 1);
	configRegisterString("Server.AdminPassword", g_NetAdminPassword, sizeof(g_NetAdminPassword) - 1);

	// Vanity egg auto-enable: "0" (default) = off; "graslu" / "redvox57" turns that
	// banner on at boot (same as typing the /graslu or /redvox57 console command).
	// Must be a sectioned key ("Game.Egg" -> "[Game]" / "Egg=..."): the config
	// system mangles section-less keys (seclen 0 drops the first char on save).
	configRegisterString("Game.Egg", g_EggConfig, sizeof(g_EggConfig) - 1);
}
