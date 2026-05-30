#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <ctype.h>
#include <string.h>
#include <math.h>
#include "platform.h"
#include "net/netenet.h"
#include "net/net.h"
#include "net/netbuf.h"
#include "net/netmsg.h"
#include "net/netmaster.h"
#include "net/playlist.h"
#include "types.h"
#include "constants.h"
#include "data.h"
#include "bss.h"
#include "game/hudmsg.h"
#include "game/playermgr.h"
#include "game/player.h"
#include "game/bondgun.h"
#include "game/cheats.h"
#include "game/game_1531a0.h"
#include "game/game_0b0fd0.h"
#include "game/title.h"
#include "game/menu.h"
#include "game/pdmode.h"
#include "game/mplayer/mplayer.h"
#include "spectator.h"
#include "game/chraction.h"
#include "game/chr.h"
#include "lib/main.h"
#include "lib/vi.h"
#include "lib/model.h"
#include "config.h"
#include "system.h"
#include "console.h"
#include "fs.h"
#include "romdata.h"
#include "utils.h"

s32 g_NetMode = NETMODE_NONE;

s32 g_NetHostLatch = false;
s32 g_NetJoinLatch = false;

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
// $S = save dir (same place pd.ini lives). Override via --playlist <path>
// or Server.PlaylistPath in pd.ini; absolute / cwd-relative paths are
// honored as-is by fsFullPath.
char g_NetPlaylistPath[260] = "$S/server_playlist.ini";

struct netvotestate g_NetVote;

u32 g_NetServerUpdateRate = 1;
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

char g_NetLastJoinAddr[NET_MAX_ADDR + 1] = "127.0.0.1:27100";

u32 g_NetTick = 0;
u32 g_NetNextSyncId = 1;

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
	// Flush every line so a crash doesn't lose the last few events that
	// would otherwise sit in the stdio buffer.
	fflush(g_NetDiagFile);
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
	const u8 *cmpa = (const u8 *)move + sizeof(move->tick);
	const u8 *cmpb = (const u8 *)moveprev + sizeof(move->tick);
	const size_t tail = sizeof(move->animnum) + sizeof(move->animframe);
	return (memcmp(cmpa, cmpb, sizeof(*move) - sizeof(move->tick) - tail) != 0);
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

void netInit(void)
{
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

	// Clear the kill feed and lobby state so a fresh session starts clean.
	netKillFeedClear();
	g_NetLobbyState.valid = 0;

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

	++g_NetNumClients;

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
		sysLogPrintf(LOG_NOTE, "NET: %s joining in progress as spectator (will spawn next round)", addrstr);
	}
	enet_peer_set_data(peer, cl);
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

	netClientReset(cl);

	--g_NetNumClients;
}

static void netServerEvReceive(struct netclient *cl)
{
	u32 rc = 0;
	u8 msgid = 0;

	while (!rc && netbufReadLeft(&cl->in) > 0) {
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

	while (!rc && netbufReadLeft(&cl->in) > 0) {
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

void netStartFrame(void)
{
	if (!g_NetMode) {
		return;
	}

	++g_NetTick;

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
	s32 polled = false;
	ENetEvent ev = { .type = ENET_EVENT_TYPE_NONE };
	while (!polled) {
		if (enet_host_check_events(g_NetHost, &ev) <= 0) {
			if (enet_host_service(g_NetHost, &ev, 1) <= 0) {
				break;
			}
			polled = true;
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
						sysLogPrintf(LOG_WARNING | LOGFLAG_NOCON, "NET: disconnect from %s without attached client", netFormatPeerAddr(ev.peer));
						--g_NetNumClients;
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
						netmsgSvcPlayerMoveWrite(needrel ? &g_NetMsgRel : &g_NetMsg, cl);
					}
				}
			}
#ifndef PLATFORM_N64
			// broadcast sim (bot) chr positions so clients can position-drive them
			if (g_Vars.lvmpbotlevel) {
				for (s32 i = 0; i < g_BotCount; i++) {
					struct chrdata *chr = g_MpBotChrPtrs[i];
					if (chr && chr->prop && chr->prop->syncid) {
						netmsgSvcPropMoveWrite(&g_NetMsg, chr->prop, NULL);
					}
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
						netmsgSvcPlayerStatsWrite(&g_NetMsgRel, cl);
					}
				}
			}
#endif
			if (g_NetNextUpdate <= g_NetTick) {
				g_NetNextUpdate = g_NetTick + g_NetServerUpdateRate;
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
			// TODO: backup the player configs or something
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
}

void netSyncIdsAllocate(void)
{
	// allocate sync ids sequentially for all active or paused props
	g_NetNextSyncId = 1;

	// don't allocate anything else if we're in lobby
	if (g_StageNum == STAGE_TITLE || g_StageNum == STAGE_CITRAINING) {
		return;
	}

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
	// current server-authoritative pose. The rewind amount is RTT/2 (network)
	// plus the shooter's interpolation delay, computed in two steps below.
	const u32 rtt_ms      = enet_peer_get_rtt(shooter->peer);
	// Stack the shooter's INTERPOLATION delay on top of the network RTT/2: they
	// render remote targets behind by g_NetInterpTicks + their measured interp_lag
	// (Fix #1's snapshot-domain clock), so the pose they actually shot at was
	// RTT/2 + interp_delay in the past. Omitting it under-rewound, so close-range
	// and fast-strafe hits at high ping missed. interp_lag ~= one-way latency in
	// ticks (the server's peak-hold of this client's snapshot staleness); under
	// roughly symmetric latency it stands in for the shooter's own render-behind
	// with no wire change (an exact shooter-sent render-tick would need a protocol
	// bump). NET_LAGCOMP_SIZE (120 ticks / 2 s) covers the combined rewind at 350ms.
	const u32 interp_ticks = g_NetInterpTicks + (u32)(shooter->interp_lag + 0.5f);
	const u32 rewind_ticks = (rtt_ms / 2 + 8) / 16 + interp_ticks;
	const u32 target_tick  = (g_NetTick > rewind_ticks) ? (g_NetTick - rewind_ticks) : 0;

	g_LagCompLastRewindTicks = rewind_ticks;

	netDiagLogf("lagcomp", "shooter=%u rtt=%u interp=%u rewind_ticks=%u", shooter->id, rtt_ms, interp_ticks, rewind_ticks);

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
	// Humans first (mpchr 0..MAX_PLAYERS-1), then sims (>=MAX_PLAYERS). Skip
	// the local player and anything that's hidden / dead / unspawned.
	for (s32 i = 0; i < MAX_MPCHRS && n < cap; ++i) {
		struct chrdata *chr = g_MpAllChrPtrs[i];
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

	if (g_NetMode != NETMODE_CLIENT || !g_NetLocalClient || !g_NetLocalClient->player) {
		s_wasdead = false;
		return;
	}

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
		} else if (strcmp(arg, g_NetAdminPassword) == 0) {
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
			g_NetAdminController = cl->id;
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
		// /admin <subcommand...> — remote server administration. On a client
		// the line is sent to the server (CLC_ADMIN) and executed there, gated
		// by the admin password; responses arrive as SVC_ADMIN and print here.
		// On the local host it runs directly. Try `/admin help`.
		if (g_NetMode == NETMODE_CLIENT && g_NetLocalClient
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
		sysLogPrintf(LOG_CHAT, "STATUS: stage=0x%02x scenario=%d options=0x%08x score=%d time=%d",
				g_MpSetup.stagenum, g_MpSetup.scenario, g_MpSetup.options,
				(s32)g_MpSetup.scorelimit, (s32)g_MpSetup.timelimit);
		sysLogPrintf(LOG_CHAT, "STATUS: playlist=%s (%d entries, vote=%ds/%dcand)",
				g_NetPlaylistPath, (s32)g_NetPlaylist.count,
				(s32)g_NetPlaylist.vote_seconds, (s32)g_NetPlaylist.vote_candidates);
	} else if (strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) {
		sysLogPrintf(LOG_CHAT, "NET commands:");
		sysLogPrintf(LOG_CHAT, "  /lag <ms>        artificial outgoing latency (0 = off)");
		sysLogPrintf(LOG_CHAT, "  /loss <N>        drop ~1 in N unreliable packets (0 = off)");
		sysLogPrintf(LOG_CHAT, "  /diag <path>     start diag log to file (no arg = stop)");
		sysLogPrintf(LOG_CHAT, "  /diagrate <n>    ticks between pos dumps (0 = disable dumps)");
		sysLogPrintf(LOG_CHAT, "  /netinfo         print current net state + tuning knobs");
		sysLogPrintf(LOG_CHAT, "  /igtick          print local in-game tick rate + GE iframe state");
		sysLogPrintf(LOG_CHAT, "  /spec [name|next|prev|off]  follow another player/sim");
		sysLogPrintf(LOG_CHAT, "  /interp <n>      entity interpolation ticks (default 3)");
		sysLogPrintf(LOG_CHAT, "  /stale <n>       snap-on-stale threshold ticks (default 30)");
		sysLogPrintf(LOG_CHAT, "  /svcrate <n>     server update interval, ticks (default 1)");
		sysLogPrintf(LOG_CHAT, "  /clcrate <n>     client update interval, ticks (default 1)");
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

	configRegisterString("Net.Debug.LogPath", g_NetDiagPath, sizeof(g_NetDiagPath) - 1);
	configRegisterUInt("Net.Debug.LogRate", &g_NetDiagDumpRate, 0, 600);

	configRegisterString("Server.Name", g_NetServerName, sizeof(g_NetServerName) - 1);
	configRegisterString("Server.PlaylistPath", g_NetPlaylistPath, sizeof(g_NetPlaylistPath) - 1);
	configRegisterString("Server.AdminPassword", g_NetAdminPassword, sizeof(g_NetAdminPassword) - 1);
}
