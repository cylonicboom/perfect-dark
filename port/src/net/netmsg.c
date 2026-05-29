#include <string.h>
#include <stdio.h>
#include "net/netenet.h"
#include "types.h"
#include "data.h"
#include "bss.h"
#include "lib/main.h"
#include "lib/mtx.h"
#include "lib/model.h"
#include "lib/anim.h"
#include "game/mplayer/mplayer.h"
#include "game/chr.h"
#include "game/chraction.h"
#include "game/prop.h"
#include "game/propobj.h"
#include "game/explosions.h"
#include "game/dlights.h"
#include "game/lang.h"
#include "game/player.h"
#include "game/playermgr.h"
#include "game/bondgun.h"
#include "game/game_0b0fd0.h"
#include "game/inv.h"
#include "game/menu.h"
#include "game/setup.h"
#include "game/setuputils.h"
#include "game/modelmgr.h"
#include "game/propsnd.h"
#include "system.h"
#include "romdata.h"
#include "fs.h"
#include "console.h"
#include "net/net.h"
#include "net/netbuf.h"
#include "net/netmsg.h"

/* utils */

static inline u32 netbufReadHidden(struct netbuf *buf)
{
	u32 hidden = netbufReadU32(buf);

	// swap owner player numbers to match server
	const u8 ownerclid = (hidden & 0xf0000000) >> 28;
	if (ownerclid < NET_MAX_CLIENTS) {
		const u8 ownerplayernum = g_NetClients[ownerclid].playernum;
		hidden = (hidden & 0x0fffffff) | (ownerplayernum << 28);
	}

	return hidden;
}

static inline u32 netbufWriteRooms(struct netbuf *buf, const s16 *rooms, const s32 num)
{
	for (s32 i = 0; i < num; ++i) {
		netbufWriteS16(buf, rooms[i]);
		if (rooms[i] < 0) {
			break;
		}
	}
	return buf->error;
}

static inline u32 netbufReadRooms(struct netbuf *buf, s16 *rooms, const s32 num)
{
	for (s32 i = 0; i < num; ++i) {
		rooms[i] = netbufReadS16(buf);
		if (rooms[i] < 0) {
			break;
		}
	}
	return buf->error;
}

static inline u32 netbufWriteGset(struct netbuf *buf, const struct gset *gset)
{
	netbufWriteData(buf, gset, sizeof(*gset));
	return buf->error;
}

static inline u32 netbufReadGset(struct netbuf *buf, struct gset *gset)
{
	netbufReadData(buf, gset, sizeof(*gset));
	return buf->error;
}

static inline u32 netbufWritePlayerMove(struct netbuf *buf, const struct netplayermove *in)
{
	netbufWriteU32(buf, in->tick);
	netbufWriteU32(buf, in->ucmd);
	netbufWriteF32(buf, in->leanofs);
	netbufWriteF32(buf, in->crouchofs);
	netbufWriteF32(buf, in->movespeed[0]);
	netbufWriteF32(buf, in->movespeed[1]);
	netbufWriteF32(buf, in->angles[0]);
	netbufWriteF32(buf, in->angles[1]);
	netbufWriteF32(buf, in->crosspos[0]);
	netbufWriteF32(buf, in->crosspos[1]);
	netbufWriteS8(buf, in->weaponnum);
	netbufWriteCoord(buf, &in->pos);
	netbufWriteS16(buf, in->animnum);
	netbufWriteS16(buf, in->animframe);
	if (in->ucmd & UCMD_AIMMODE) {
		netbufWriteF32(buf, in->zoomfov);
	}
	return buf->error;
}

static inline u32 netbufReadPlayerMove(struct netbuf *buf, struct netplayermove *in)
{
	in->tick = netbufReadU32(buf);
	in->ucmd = netbufReadU32(buf);
	in->leanofs = netbufReadF32(buf);
	in->crouchofs = netbufReadF32(buf);
	in->movespeed[0] = netbufReadF32(buf);
	in->movespeed[1] = netbufReadF32(buf);
	in->angles[0] = netbufReadF32(buf);
	in->angles[1] = netbufReadF32(buf);
	in->crosspos[0] = netbufReadF32(buf);
	in->crosspos[1] = netbufReadF32(buf);
	in->weaponnum = netbufReadS8(buf);
	netbufReadCoord(buf, &in->pos);
	in->animnum = netbufReadS16(buf);
	in->animframe = netbufReadS16(buf);
	if (in->ucmd & UCMD_AIMMODE) {
		in->zoomfov = netbufReadF32(buf);
	} else {
		in->zoomfov = 0.f;
	}
	return buf->error;
}

static inline u32 netbufWritePropPtr(struct netbuf *buf, const struct prop *prop)
{
	netbufWriteU32(buf, prop ? prop->syncid : 0);
	return buf->error;
}

static inline struct prop *netbufReadPropPtr(struct netbuf *buf)
{
	const u32 syncid = netbufReadU32(buf);
	if (syncid == 0) {
		return NULL;
	}

	// TODO: make a map or something
	for (s32 i = 0; i < g_Vars.maxprops; ++i) {
		if (g_Vars.props[i].syncid == syncid) {
			return &g_Vars.props[i];
		}
	}

	sysLogPrintf(LOG_WARNING, "NET: prop with syncid %u does not exist", syncid);
	return NULL;
}

static inline s32 propRoomsEqual(const RoomNum *ra, const RoomNum *rb)
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

/* client -> server */

u32 netmsgClcAuthWrite(struct netbuf *dst)
{
	const char *modDir = fsGetModDir();
	if (!modDir) {
		modDir = "";
	}

	netbufWriteU8(dst, CLC_AUTH);
	netbufWriteStr(dst, g_NetLocalClient->settings.name);
	netbufWriteStr(dst, g_RomName); // TODO: use a CRC or something
	netbufWriteStr(dst, modDir);
	netbufWriteU8(dst, 1); // TODO: number of local players

	return dst->error;
}

u32 netmsgClcAuthRead(struct netbuf *src, struct netclient *srccl)
{
	if (srccl->state != CLSTATE_AUTH) {
		sysLogPrintf(LOG_WARNING, "NET: CLC_AUTH from client %u, who is not in CLSTATE_AUTH", srccl->id);
		return 1;
	}

	char *name = netbufReadStr(src);
	const char *romName = netbufReadStr(src);
	const char *modDir = netbufReadStr(src);
	const u8 players = netbufReadU8(src);

	if (src->error) {
		sysLogPrintf(LOG_WARNING, "NET: malformed CLC_AUTH from client %u", srccl->id);
		netServerKick(srccl, DISCONNECT_KICKED);
		return 1;
	}

	if (strcasecmp(romName, g_RomName) != 0) {
		sysLogPrintf(LOG_WARNING, "NET: CLC_AUTH: client %u has the wrong ROM, disconnecting", srccl->id);
		netServerKick(srccl, DISCONNECT_FILES);
		return src->error;
	}

	if (modDir[0] == '\0') {
		modDir = NULL;
	}

	const char *myModDir = fsGetModDir();
	if ((!myModDir != !modDir) || (myModDir && modDir && strcasecmp(modDir, myModDir) != 0)) {
		sysLogPrintf(LOG_WARNING, "NET: CLC_AUTH: client %u has the wrong mod, disconnecting", srccl->id);
		netServerKick(srccl, DISCONNECT_FILES);
		return src->error;
	}

	// for now use settings from our own client, remote is supposed to send CLC_SETTINGS after CLC_AUTH
	srccl->settings = g_NetLocalClient->settings;
	strncpy(srccl->settings.name, name, sizeof(srccl->settings.name) - 1);
	srccl->state = CLSTATE_LOBBY;

	sysLogPrintf(LOG_NOTE, "NET: CLC_AUTH from client %u (%s), responding", srccl->id, srccl->settings.name);

	netbufStartWrite(&srccl->out);
	netmsgSvcAuthWrite(&srccl->out, srccl);
	netSend(srccl, NULL, true, NETCHAN_CONTROL);

	sysLogPrintf(LOG_NOTE, "NET: %s (%u) joined", srccl->settings.name, srccl->id);

	netChatPrintf(NULL, "%s joined", name);

	// Join-in-progress: if this client connected while the host was already
	// in CLSTATE_GAME, netServerEvConnect tagged them with
	// jip_pending_unspectate. Ship the current SVC_STAGE_START to them now
	// so they enter the running match as a spectator (is_spectator = 1).
	// mpStartMatch at the next round boundary clears their JIP flags so they
	// spawn cleanly. No prop-spawn snapshot is sent — they'll see whatever
	// is on the wire from this point forward; dropped weapons / tokens
	// already on the ground won't be reconstructed (deferred to v2).
	if (srccl->jip_pending_unspectate) {
		netbufStartWrite(&srccl->out);
		netmsgSvcStageStartWrite(&srccl->out);
		netSend(srccl, NULL, true, NETCHAN_DEFAULT);
		sysLogPrintf(LOG_NOTE, "NET: shipped JIP SVC_STAGE_START to client %u", srccl->id);
	}

	return 0;
}

u32 netmsgClcChatWrite(struct netbuf *dst, const char *str)
{
	netbufWriteU8(dst, CLC_CHAT);
	netbufWriteStr(dst, str);
	return dst->error;
}

u32 netmsgClcChatRead(struct netbuf *src, struct netclient *srccl)
{
	char tmp[1024];
	const char *msg = netbufReadStr(src);
	if (msg && !src->error) {
		sysLogPrintf(LOG_CHAT, "%s", msg);
		netbufStartWrite(&g_NetMsgRel);
		netmsgSvcChatWrite(&g_NetMsgRel, msg);
		netSend(NULL, &g_NetMsgRel, true, NETCHAN_DEFAULT);
	}
	return src->error;
}

u32 netmsgClcMoveWrite(struct netbuf *dst)
{
	netbufWriteU8(dst, CLC_MOVE);
	netbufWriteU32(dst, g_NetLocalClient->inmove[g_NetLocalClient->inmove_head].tick);
	netbufWritePlayerMove(dst, &g_NetLocalClient->outmove[0]);
	return dst->error;
}

u32 netmsgClcMoveRead(struct netbuf *src, struct netclient *srccl)
{
	struct netplayermove newmove;
	const u32 outmoveack = netbufReadU32(src);
	netbufReadPlayerMove(src, &newmove);

	if (srccl->state != CLSTATE_GAME) {
		// silently ignore
		return src->error;
	}

	srccl->outmoveack = outmoveack;

	if (!src->error) {
		// enforce teleports and such
		if (srccl->forcetick && srccl->player) {
			if (srccl->outmoveack >= srccl->forcetick) {
				// client has acknowledged our last sent move, clear the force flags
				srccl->forcetick = 0;
				srccl->player->ucmd &= ~UCMD_FL_FORCEMASK;
				sysLogPrintf(LOG_NOTE, "NET: client %u successfully forcemoved", srccl->id);
			} else {
				// client hasn't teleported yet, discard the new position from the input command
				if (srccl->player->ucmd & UCMD_FL_FORCEPOS) {
					newmove.pos = srccl->player->prop->pos;
				}
				if (srccl->player->ucmd & UCMD_FL_FORCEANGLE) {
					newmove.angles[0] = srccl->player->vv_theta;
					newmove.angles[1] = srccl->player->vv_verta;
				}
			}
		}
		// RING BUFFER PUSH: advance head to the next slot and store the newest
		// move there; head wraps modulo NET_SNAPSHOT_COUNT (8). The buffer
		// powers entity interpolation: bwalkUpdateRemote and bmoveProcessRemoteInput
		// find two snapshots bracketing (g_NetTick - g_NetInterpTicks) and lerp
		// position / speeds between them, smoothing out the discrete arrival
		// cadence of SVC_PLAYER_MOVE packets. When the buffer is full the oldest
		// snapshot is overwritten — depth limits how far back we can lerp, which
		// also caps the maximum useful g_NetInterpTicks.
		srccl->inmove_head = (srccl->inmove_head + 1) % NET_SNAPSHOT_COUNT;
		srccl->inmove[srccl->inmove_head] = newmove;
		srccl->lerpticks = 0;
	}

	return src->error;
}

u32 netmsgClcSettingsWrite(struct netbuf *dst)
{
	netbufWriteU8(dst, CLC_SETTINGS);
	netbufWriteU16(dst, g_NetLocalClient->settings.options);
	netbufWriteU8(dst, g_NetLocalClient->settings.bodynum);
	netbufWriteU8(dst, g_NetLocalClient->settings.headnum);
	netbufWriteF32(dst, g_NetLocalClient->settings.fovy);
	netbufWriteF32(dst, g_NetLocalClient->settings.fovzoommult);
	netbufWriteStr(dst, g_NetLocalClient->settings.name);
	return dst->error;
}

u32 netmsgClcSettingsRead(struct netbuf *src, struct netclient *srccl)
{
	const u16 options = netbufReadU16(src);
	const u8 bodynum = netbufReadU8(src);
	const u8 headnum = netbufReadU8(src);
	const f32 fovy = netbufReadF32(src);
	const f32 fovzoommult = netbufReadF32(src);
	char *name = netbufReadStr(src);

	if (src->error) {
		sysLogPrintf(LOG_WARNING, "NET: malformed CLC_SETTINGS from client %u", srccl->id);
		netServerKick(srccl, DISCONNECT_KICKED);
		return 1;
	}

	if (srccl->settings.name[0] && strncmp(srccl->settings.name, name, MAX_PLAYERNAME) != 0) {
		netChatPrintf(NULL, "%s is now known as %s", srccl->settings.name, name);
	}

	strncpy(srccl->settings.name, name, sizeof(srccl->settings.name) - 1);
	srccl->settings.options = options;
	srccl->settings.bodynum = bodynum;
	srccl->settings.headnum = headnum;
	srccl->settings.fovy = fovy;
	srccl->settings.fovzoommult = fovzoommult;

	return src->error;
}

u32 netmsgClcHitWrite(struct netbuf *dst, struct chrdata *chr, f32 damage, struct coord *vector, struct gset *gset, s16 hitpart, s16 side, s16 *arg10)
{
	netbufWriteU8(dst, CLC_HIT);
	netbufWriteU32(dst, g_NetTick);
	netbufWriteU32(dst, chr->prop->syncid);
	netbufWriteF32(dst, damage);
	netbufWriteCoord(dst, vector);
	netbufWriteGset(dst, gset);
	netbufWriteS16(dst, hitpart);
	netbufWriteS16(dst, side);
	netbufWriteS16(dst, arg10 ? arg10[0] : 0);
	netbufWriteS16(dst, arg10 ? arg10[1] : 0);
	netbufWriteS16(dst, arg10 ? arg10[2] : 0);
	return dst->error;
}

u32 netmsgClcHitRead(struct netbuf *src, struct netclient *srccl)
{
	const u32 tick          = netbufReadU32(src);
	const u32 target_syncid = netbufReadU32(src);
	const f32 damage        = netbufReadF32(src);
	struct coord vector; netbufReadCoord(src, &vector);
	struct gset gset; netbufReadGset(src, &gset);
	const s16 hitpart = netbufReadS16(src);
	const s16 side    = netbufReadS16(src);
	s16 arg10[3];
	arg10[0] = netbufReadS16(src);
	arg10[1] = netbufReadS16(src);
	arg10[2] = netbufReadS16(src);

	if (src->error || srccl->state < CLSTATE_GAME || g_NetMode != NETMODE_SERVER) {
		return src->error;
	}

	// Reject hits whose tick falls outside the lag-compensation buffer.
	if (g_NetTick > tick && g_NetTick - tick > NET_LAGCOMP_SIZE) {
		return src->error;
	}

	// Find the target chr prop by syncid.
	struct prop *target = NULL;
	for (s32 i = 0; i < g_Vars.maxprops && !target; ++i) {
		if (g_Vars.props[i].syncid == target_syncid &&
				(g_Vars.props[i].type == PROPTYPE_CHR || g_Vars.props[i].type == PROPTYPE_PLAYER)) {
			target = &g_Vars.props[i];
		}
	}

	if (!target || !target->chr) {
		return src->error;
	}

	// Enqueue for chrDamage in netEndFrame (after buffer reset, before flush)
	// so SVC_CHR_DAMAGE is actually broadcast to clients. Calling chrDamage
	// here during event processing would have it write to g_NetMsgRel just
	// before netStartFrame resets the buffer, discarding the broadcast.
	netServerEnqueueHit(target, damage, &vector, &gset, hitpart, side, arg10,
			(srccl->playernum < MAX_PLAYERS) ? (s32)srccl->playernum : -1,
			srccl->player ? srccl->player->prop : NULL);

	return src->error;
}

/* server -> client */

u32 netmsgSvcAuthWrite(struct netbuf *dst, struct netclient *authcl)
{
	netbufWriteU8(dst, SVC_AUTH);
	netbufWriteU8(dst, authcl - g_NetClients);
	netbufWriteU8(dst, g_NetMaxClients);
	netbufWriteU32(dst, g_NetTick);
	return dst->error;
}

u32 netmsgSvcAuthRead(struct netbuf *src, struct netclient *srccl)
{
	if (g_NetLocalClient->state != CLSTATE_AUTH) {
		sysLogPrintf(LOG_WARNING, "NET: SVC_AUTH from server but we're not in AUTH state");
		return 1;
	}

	const u8 id = netbufReadU8(src);
	const u8 maxclients = netbufReadU8(src);
	g_NetTick = netbufReadU32(src);
	if (g_NetLocalClient->in.error || id == NET_NULL_CLIENT || id == 0 || maxclients == 0) {
		sysLogPrintf(LOG_WARNING, "NET: malformed SVC_AUTH from server");
		return 1;
	}

	sysLogPrintf(LOG_NOTE, "NET: SVC_AUTH from server, our ID is %u", id);

	// there's at least one client, which is us, and we know maxclients as well
	g_NetMaxClients = maxclients;
	g_NetNumClients = 1;

	// we now know our proper ID, so move to the appropriate client slot and reset the old one
	g_NetLocalClient = &g_NetClients[id];
	g_NetClients[id] = g_NetClients[NET_MAX_CLIENTS];
	g_NetLocalClient->out.data = g_NetLocalClient->out_data;
	g_NetLocalClient->id = id;

	// clear out the old slot
	g_NetClients[NET_MAX_CLIENTS].id = NET_MAX_CLIENTS;
	g_NetClients[NET_MAX_CLIENTS].state = 0;
	g_NetClients[NET_MAX_CLIENTS].peer = NULL;

	// the server's client probably is in the lobby state by now
	g_NetClients[0].state = CLSTATE_LOBBY;

	g_NetLocalClient->state = CLSTATE_LOBBY;

	return src->error;
}

u32 netmsgSvcChatWrite(struct netbuf *dst, const char *str)
{
	netbufWriteU8(dst, SVC_CHAT);
	netbufWriteStr(dst, str);
	return dst->error;
}

u32 netmsgSvcChatRead(struct netbuf *src, struct netclient *srccl)
{
	char tmp[1024];
	const char *msg = netbufReadStr(src);
	if (msg && !src->error) {
		sysLogPrintf(LOG_CHAT, "%s", msg);
	}
	return src->error;
}

u32 netmsgSvcStageStartWrite(struct netbuf *dst)
{
	netbufWriteU8(dst, SVC_STAGE_START);

	netbufWriteU32(dst, g_NetTick);

	netbufWriteU64(dst, g_RngSeed);
	netbufWriteU64(dst, g_Rng2Seed);

	// Snapshot the music RNG seed so both sides can advance it independently
	// from now on without drifting. mpChooseTrack consumes from this seed
	// instead of g_RngSeed so non-music RNG consumers don't desync the music.
	g_NetMusicRngSeed = g_RngSeed;

	netbufWriteU8(dst, g_StageNum);

	if (g_StageNum == STAGE_TITLE || g_StageNum == STAGE_CITRAINING) {
		// going back to lobby, don't need anything else
		return dst->error;
	}

	// game settings
	netbufWriteU8(dst, 0); // 0 for combat sim TODO: coop/anti
	netbufWriteU8(dst, g_MpSetup.scenario);
	netbufWriteU8(dst, g_MpSetup.scorelimit);
	netbufWriteU8(dst, g_MpSetup.timelimit);
	netbufWriteU16(dst, g_MpSetup.teamscorelimit);
	netbufWriteU16(dst, g_MpSetup.chrslots);
	netbufWriteU32(dst, g_MpSetup.options);
	netbufWriteData(dst, g_MpSetup.weapons, sizeof(g_MpSetup.weapons));
	// KotH static-hill index (NET_PROTOCOL_VER >= 28). 0 = Random; 1..N = hillpads[index-1].
	// Both sides need the same value before kohInitProps runs to keep g_RngSeed in sync
	// (the static-pick path skips rngRandom).
	netbufWriteU8(dst, g_MpSetup.kohstatichill);
	// CTC per-team base pins (NET_PROTOCOL_VER >= 29). 0 = Random; 1..4 = spawnpadsperteam[N-1].
	// Server + client must agree before ctcInitProps team-assignment loop runs.
	for (s32 i = 0; i < 4; ++i) {
		netbufWriteU8(dst, g_MpSetup.ctcteambase[i]);
	}
	// HTB / HTM static spawn pins (NET_PROTOCOL_VER >= 29). 0 = Random; 1..N = padnums[N-1].
	// Server + client must agree before htbCreateToken / htbCreateUplink runs.
	netbufWriteU8(dst, g_MpSetup.htbstaticpad);
	netbufWriteU8(dst, g_MpSetup.htmstaticpad);

	// who the fuck is in the game
	netbufWriteU8(dst, g_NetNumClients);
	for (s32 i = 0; i < g_NetMaxClients; ++i) {
		struct netclient *ncl = &g_NetClients[i];
		if (ncl->state) {
			// Spectator clients have no config (skipped by netPlayersAllocate),
			// so don't dereference cfg->base.team here. The settings.team value
			// the spectator brought into the lobby stays as-is.
			if (ncl->config) {
				ncl->settings.team = ncl->config->base.team;
			}
			netbufWriteU8(dst, ncl->id);
			netbufWriteU8(dst, ncl->playernum);
			netbufWriteU8(dst, ncl->settings.team);
			netbufWriteU16(dst, ncl->settings.options);
			netbufWriteU8(dst, ncl->settings.bodynum);
			netbufWriteU8(dst, ncl->settings.headnum);
			netbufWriteF32(dst, ncl->settings.fovy);
			netbufWriteF32(dst, ncl->settings.fovzoommult);
			netbufWriteStr(dst, ncl->settings.name);
			// Spectator flag (NET_PROTOCOL_VER >= 27). Bumped at the tail of
			// the per-client block so older fields stay byte-compatible if we
			// ever need a downgrade path.
			netbufWriteU8(dst, ncl->is_spectator);
			memset(ncl->inmove, 0, sizeof(ncl->inmove));
			memset(ncl->outmove, 0, sizeof(ncl->outmove));
			ncl->inmove_head = 0;
			ncl->lerpticks = 0;
			ncl->outmoveack = 0;
			ncl->state = CLSTATE_GAME;
		}
	}

	// Sim bot configs — without these the client's g_BotConfigsArray stays
	// default and botmgrAllocateBot picks default heads/bodies on the
	// client side, so sims show up wearing the wrong models/colors. Send
	// all MAX_BOTS slots so the array is fully reconstructable; difficulty
	// is what gates which slots actually spawn so it has to come along too.
	netbufWriteU8(dst, MAX_BOTS);
	for (s32 i = 0; i < MAX_BOTS; ++i) {
		const struct mpbotconfig *bot = &g_BotConfigsArray[i];
		netbufWriteU8(dst, bot->base.mpheadnum);
		netbufWriteU8(dst, bot->base.mpbodynum);
		netbufWriteU8(dst, bot->base.team);
		netbufWriteU8(dst, bot->type);
		netbufWriteU8(dst, bot->difficulty);
		netbufWriteStr(dst, bot->base.name);
	}

	return dst->error;
}

u32 netmsgSvcStageStartRead(struct netbuf *src, struct netclient *srccl)
{
	if (srccl->state != CLSTATE_LOBBY) {
		sysLogPrintf(LOG_WARNING, "NET: SVC_STAGE from server but we're not in LOBBY state");
		return 1;
	}

	g_NetTick = netbufReadU32(src);

	g_NetRngSeeds[0] = netbufReadU64(src);
	g_NetRngSeeds[1] = netbufReadU64(src);
	g_NetRngLatch = true;

	// Mirror the server's snapshot for music selection so mpChooseTrack stays
	// in sync; this seed is advanced only by track picks, not by other RNG.
	g_NetMusicRngSeed = g_NetRngSeeds[0];

	const u8 stagenum = netbufReadU8(src);

	if (stagenum == STAGE_TITLE || stagenum == STAGE_CITRAINING) {
		// server went back to lobby, we don't really care
		return src->error;
	}

	const u8 mode = netbufReadU8(src); // TODO: coop, anti
	g_MpSetup.stagenum = stagenum;
	g_MpSetup.scenario = netbufReadU8(src);
	g_MpSetup.scorelimit = netbufReadU8(src);
	g_MpSetup.timelimit = netbufReadU8(src);
	g_MpSetup.teamscorelimit = netbufReadU16(src);
	g_MpSetup.chrslots = netbufReadU16(src);
	g_MpSetup.options = netbufReadU32(src);
	netbufReadData(src, g_MpSetup.weapons, sizeof(g_MpSetup.weapons));
	g_MpSetup.kohstatichill = netbufReadU8(src);
	for (s32 i = 0; i < 4; ++i) {
		g_MpSetup.ctcteambase[i] = netbufReadU8(src);
	}
	g_MpSetup.htbstaticpad = netbufReadU8(src);
	g_MpSetup.htmstaticpad = netbufReadU8(src);
	strcpy(g_MpSetup.name, "server");

	if (src->error) {
		sysLogPrintf(LOG_WARNING, "NET: malformed SVC_STAGE from server");
		return 1;
	}

	// read players
	const u8 numplayers = netbufReadU8(src);
	if (src->error || !numplayers || numplayers > g_NetMaxClients + 1) {
		sysLogPrintf(LOG_WARNING, "NET: malformed SVC_STAGE from server");
		return 2;
	}

	for (u8 i = 0; i < numplayers; ++i) {
		const u8 id = netbufReadU8(src);
		struct netclient *ncl = &g_NetClients[id];
		ncl->playernum = netbufReadU8(src);
		ncl->settings.team = netbufReadU8(src);
		if (ncl != g_NetLocalClient) {
			ncl->id = id;
			ncl->settings.options = netbufReadU16(src);
			ncl->settings.bodynum = netbufReadU8(src);
			ncl->settings.headnum = netbufReadU8(src);
			ncl->settings.fovy = netbufReadF32(src);
			ncl->settings.fovzoommult = netbufReadF32(src);
			char *name = netbufReadStr(src);
			if (name) {
				strncpy(ncl->settings.name, name, sizeof(ncl->settings.name) - 1);
			} else {
				sysLogPrintf(LOG_WARNING, "NET: malformed SVC_STAGE from server");
				return 3;
			}
		} else {
			// skip our own settings except for the team and player number
			netbufReadU16(src);
			netbufReadU8(src);
			netbufReadU8(src);
			netbufReadF32(src);
			netbufReadF32(src);
			netbufReadStr(src);
		}
		// Spectator flag (NET_PROTOCOL_VER >= 27). Read for every client
		// including the local one so the client knows whether the host is
		// observing. Force playernum to the sentinel for spectators — the
		// wire value may be a stale combatant slot from before the toggle.
		ncl->is_spectator = netbufReadU8(src);
		if (ncl->is_spectator) {
			ncl->playernum = NET_PLAYERNUM_SPECTATOR;
		}
		ncl->state = CLSTATE_GAME;
		ncl->player = NULL;
	}

	if (src->error) {
		return src->error;
	}

	// set teams on the player configs, but swap teams with the server player
	// because we haven't swapped player numbers yet (see netPlayersAlloc)
	for (u32 i = 0; i < NET_MAX_CLIENTS; ++i) {
		struct netclient *ncl = &g_NetClients[i];
		if (ncl->state) {
			u32 playernum = 0;
			if (ncl->id == 0) {
				playernum = g_NetLocalClient->playernum;
			} else if (ncl == g_NetLocalClient) {
				playernum = g_NetClients[0].playernum;
			} else {
				playernum = ncl->playernum;
			}
			g_PlayerConfigsArray[playernum].base.team = ncl->settings.team;
		}
	}

	// Sim bot configs. Must be read here, before mpStartMatch, because that's
	// what spawns the bots — and botmgrAllocateBot copies from
	// g_BotConfigsArray to pick each bot's head, body, team, type, difficulty,
	// and name. Without this read the client uses its default array (whatever
	// the local "Combat Sim" menu was last set to) so sims show up with the
	// wrong models and names. Must match the write in netmsgSvcStageStartWrite.
	const u8 numbots = netbufReadU8(src);
	if (src->error || numbots != MAX_BOTS) {
		sysLogPrintf(LOG_WARNING, "NET: malformed SVC_STAGE bot config block from server (got %u, want %d)", numbots, MAX_BOTS);
		return 4;
	}
	for (s32 i = 0; i < numbots; ++i) {
		struct mpbotconfig *bot = &g_BotConfigsArray[i];
		bot->base.mpheadnum = netbufReadU8(src);
		bot->base.mpbodynum = netbufReadU8(src);
		bot->base.team = netbufReadU8(src);
		bot->type = netbufReadU8(src);
		bot->difficulty = netbufReadU8(src);
		char *name = netbufReadStr(src);
		if (name) {
			strncpy(bot->base.name, name, sizeof(bot->base.name) - 1);
			bot->base.name[sizeof(bot->base.name) - 1] = '\0';
		}
	}

	if (src->error) {
		sysLogPrintf(LOG_WARNING, "NET: malformed SVC_STAGE bot configs from server");
		return 5;
	}

	g_NetNumClients = numplayers;

	sysLogPrintf(LOG_NOTE, "NET: SVC_STAGE from server: going to stage 0x%02x with %u players", g_MpSetup.stagenum, numplayers);

	// Diagnostic: log right before AND after mpStartMatch so a crash inside
	// stage init shows up as the "_pre" line being the last entry in the
	// diag log. Originally only the server logged stage_start, which made
	// "client crashes on Skedar" invisible — nothing between client_start
	// and the actual segfault.
	netDiagLogf("stage_start_pre", "stage=%u numplayers=%u numbots=%u",
		(u32)g_MpSetup.stagenum, (u32)numplayers, (u32)numbots);

	mpStartMatch();
	menuStop();

	netDiagLogf("stage_start_post", "stage=%u", (u32)g_MpSetup.stagenum);

	return 0;
}

u32 netmsgSvcStageEndWrite(struct netbuf *dst)
{
	netbufWriteU8(dst, SVC_STAGE_END);

	for (s32 i = 0; i < g_NetMaxClients; ++i) {
		struct netclient *ncl = &g_NetClients[i];
		if (ncl->state) {
			ncl->state = CLSTATE_LOBBY;
			ncl->playernum = 0;
			if (ncl->player) {
				ncl->player->client = NULL;
				ncl->player->isremote = false;
				ncl->player = NULL;
			}
			if (ncl->config) {
				ncl->config->client = NULL;
				ncl->config = NULL;
			}
		}
	}

	return dst->error;
}

u32 netmsgSvcStageEndRead(struct netbuf *src, struct netclient *srccl)
{
	for (s32 i = 0; i < g_NetMaxClients; ++i) {
		struct netclient *ncl = &g_NetClients[i];
		if (ncl->state) {
			ncl->state = CLSTATE_DISCONNECTED;
			ncl->player = NULL;
			ncl->config = NULL;
		}
	}

	g_NetLocalClient->state = CLSTATE_LOBBY;

	g_NumReasonsToEndMpMatch = 1;
	mainEndStage();

	return src->error;
}

u32 netmsgSvcPlayerMoveWrite(struct netbuf *dst, struct netclient *movecl)
{
	if (movecl->state < CLSTATE_GAME || !movecl->player || !movecl->player->prop) {
		return dst->error;
	}

	const struct netplayermove *inmove = &movecl->inmove[movecl->inmove_head];
	const bool has_force = (movecl->outmove[0].ucmd & UCMD_FL_FORCEMASK) != 0;

	netbufWriteU8(dst, SVC_PLAYER_MOVE);
	netbufWriteU8(dst, movecl->id);
	netbufWriteU32(dst, inmove->tick);

	if (!has_force && inmove->tick) {
		// Echo the client's own last CLC_MOVE back. The client's netCspReconcile
		// compares this against its self-recorded CSP history at inmove->tick —
		// the values are identical so error = 0 and no correction fires.
		// Sending outmove[0] (server extrapolation) instead causes CSP to see
		// large positional drift at high latency (21 ticks at 350ms), firing a
		// hard snap every frame and producing the slide/snap-back behaviour.
		netbufWritePlayerMove(dst, inmove);
	} else {
		// Force correction (respawn, kill plane, initial state) or no CLC_MOVE
		// received yet (server's own player). Send the authoritative server state.
		netbufWritePlayerMove(dst, &movecl->outmove[0]);
		if (has_force) {
			netbufWriteRooms(dst, movecl->player->prop->rooms, ARRAYCOUNT(movecl->player->prop->rooms));
		}
	}

	return dst->error;
}

u32 netmsgSvcPlayerMoveRead(struct netbuf *src, struct netclient *srccl)
{
	u8 id = 0;
	u32 outmoveack = 0;
	struct netplayermove newmove;
	RoomNum newrooms[8] = { -1 };

	id = netbufReadU8(src);
	outmoveack = netbufReadU32(src);
	netbufReadPlayerMove(src, &newmove);
	if (newmove.ucmd & UCMD_FL_FORCEMASK) {
		netbufReadRooms(src, newrooms, ARRAYCOUNT(newrooms));
	}

	if (src->error || srccl->state < CLSTATE_GAME) {
		return src->error;
	}

	struct netclient *movecl = &g_NetClients[id];

	// Push into the snapshot ring buffer
	movecl->inmove_head = (movecl->inmove_head + 1) % NET_SNAPSHOT_COUNT;
	movecl->inmove[movecl->inmove_head] = newmove;
	movecl->outmoveack = outmoveack;
	movecl->lerpticks = 0;

	// Teleport/respawn: flood all ring buffer slots with the new position so
	// entity interpolation can't reach any pre-teleport entry during the interp
	// window. Without this, desired_tick still points into the death-position era
	// for g_NetInterpTicks frames and the player appears to warp back to the void.
	//
	// Two triggers:
	//   Force flags   — set for remote-client respawns via chraction.c; also
	//                   clears lag comp so shots can't rewind to death position.
	//   Big position jump (>512 units in any axis) — catches the host player's
	//                   own respawn, which never gets force flags because
	//                   player->isremote is false on the server.
	{
		const struct netplayermove *prev =
			&movecl->inmove[(movecl->inmove_head + NET_SNAPSHOT_COUNT - 1) % NET_SNAPSHOT_COUNT];
		const bool big_jump = prev->tick && (
			fabsf(newmove.pos.x - prev->pos.x) > 512.f ||
			fabsf(newmove.pos.y - prev->pos.y) > 512.f ||
			fabsf(newmove.pos.z - prev->pos.z) > 512.f
		);
		if ((newmove.ucmd & UCMD_FL_FORCEMASK) || big_jump) {
			for (s32 i = 0; i < NET_SNAPSHOT_COUNT; ++i) {
				movecl->inmove[i] = newmove;
			}
		}
		if (newmove.ucmd & UCMD_FL_FORCEMASK) {
			memset(movecl->lagcomp, 0, sizeof(movecl->lagcomp));
			movecl->lagcomp_head = 0;
		}
	}

	if (movecl == g_NetLocalClient) {
		if (newmove.ucmd & UCMD_FL_FORCEMASK) {
			// Server wants to teleport us; cancel any pending CSP correction
			g_NetCspCorrFrames = 0;
			if (movecl->player && movecl->player->prop) {
				chrSetPos(movecl->player->prop->chr, &newmove.pos, newrooms, newmove.angles[0], (newmove.ucmd & UCMD_FL_FORCEGROUND) != 0);
			}
		} else {
			// Normal authoritative position: check CSP prediction error and
			// schedule a smooth correction if needed. Theta is passed so the
			// snap branch can call chrSetPos and re-derive ground/rooms (a bare
			// prop->pos write gets clamped back by the next local physics tick).
			netCspReconcile(outmoveack, &newmove.pos, newmove.angles[0]);
		}
	}

	return src->error;
}

u32 netmsgSvcPlayerStatsWrite(struct netbuf *dst, struct netclient *actcl)
{
	if (actcl->state < CLSTATE_GAME || !actcl->player || !actcl->player->prop) {
		return dst->error;
	}
	const struct player *pl = actcl->player;
	const u8 flags = (pl->isdead != 0) | (pl->gunctrl.dualwielding << 1) |
		(pl->hands[0].inuse << 2) | (pl->hands[1].inuse << 3);
	netbufWriteU8(dst, SVC_PLAYER_STATS);
	netbufWriteU8(dst, actcl->id);
	netbufWriteU8(dst, flags);
	netbufWriteS8(dst, pl->gunctrl.weaponnum);
	netbufWriteF32(dst, pl->prop->chr->damage);
	netbufWriteF32(dst, pl->bondhealth);
	netbufWriteF32(dst, pl->prop->chr->cshield);
	netbufWriteCoord(dst, &pl->bondshotspeed);

	for (s32 i = 0; i < 2; ++i) {
		if (pl->hands[i].inuse) {
			netbufWriteS16(dst, pl->hands[i].loadedammo[0]);
			netbufWriteS16(dst, pl->hands[i].loadedammo[1]);
		}
	}

	// assemble a bitmask of all non-zero ammo entries below 32
	u32 mask = 0;
	for (u32 i = 0; i < 32; ++i) {
		if (pl->ammoheldarr[i]) {
			mask |= (1 << i);
		}
	}

	netbufWriteU32(dst, mask);

	// write all entries that are non-zero or have index above 32
	for (s32 i = 0; i < ARRAYCOUNT(pl->ammoheldarr); ++i) {
		if (pl->ammoheldarr[i] || i >= 32) {
			netbufWriteS16(dst, pl->ammoheldarr[i]);
		}
	}

	return dst->error;
}

u32 netmsgSvcPlayerStatsRead(struct netbuf *src, struct netclient *srccl)
{
	const u8 clid = netbufReadU8(src);
	const u8 flags = netbufReadU8(src);
	const s8 newweaponnum = netbufReadS8(src);
	const f32 newdamage = netbufReadF32(src);
	const f32 newhealth = netbufReadF32(src);
	const f32 newshield = netbufReadF32(src);
	struct coord newshotspeed; netbufReadCoord(src, &newshotspeed);
	const bool handused[2] = { (flags & (1 << 2)) != 0, (flags & (1 << 3)) != 0 };

	if (src->error) {
		return src->error;
	}

	struct netclient *actcl = g_NetClients + clid;
	if (actcl->state < CLSTATE_GAME) {
		return 1;
	}

	struct player *pl = actcl->player;
	if (!pl || !pl->prop) {
		return src->error;
	}

	pl->prop->chr->damage = newdamage;
	pl->prop->chr->cshield = newshield;
	pl->bondhealth = newhealth;
	pl->bondshotspeed = newshotspeed;

	// Skip applying ammo to the local player. The client already decrements
	// ammo when it fires locally, so the server's reply — delayed by RTT —
	// arrives carrying the pre-shot value and bounces the counter back up.
	// Health/damage/shield above are still applied because those are fully
	// server-authoritative (the client doesn't predict them). Ammo from
	// pickups and reloads stays correct because the local game code runs
	// those same paths on the client.
	const bool islocal = (actcl == g_NetLocalClient);

	for (s32 i = 0; i < 2; ++i) {
		if (handused[i]) {
			const s16 ammo0 = netbufReadS16(src);
			const s16 ammo1 = netbufReadS16(src);
			if (!islocal) {
				pl->hands[i].loadedammo[0] = ammo0;
				pl->hands[i].loadedammo[1] = ammo1;
			}
		}
	}

	const u32 ammomask = netbufReadU32(src);
	for (s32 i = 0; i < ARRAYCOUNT(pl->ammoheldarr); ++i) {
		if (i >= 32 || (ammomask & (1 << i))) {
			const s16 ammo = netbufReadS16(src);
			if (!islocal) {
				pl->ammoheldarr[i] = ammo;
			}
		} else if (!islocal) {
			pl->ammoheldarr[i] = 0;
		}
	}

	const s32 prevplayernum = g_Vars.currentplayernum;
	setCurrentPlayerNum(actcl->playernum);

	const bool newisdead = (flags & (1 << 0)) != 0;
	if (!pl->isdead && newisdead) {
		// SVC_PLAYER_STATS marks the player dead but no prior SVC_CHR_DAMAGE
		// killed them locally — fall damage / drown / off-map, or chrDamage
		// applied but didn't drop bondhealth to zero on the client (lag /
		// state desync). Look at chr->lastattacker (set inside chrDamage on
		// both sides) to find the killer. If there's no attacker, this is
		// genuinely a self/env death so suicide attribution is correct.
		// lastshooter is dead code — never written anywhere — so it was
		// always falling through to currentplayernum and the victim was
		// being credited as their own killer (kill went to wrong player,
		// "Suicide count: N" hudmsg on respawn).
		s32 shooter = -1;
		struct chrdata *attacker = pl->prop->chr->lastattacker;
		if (attacker && attacker->prop) {
			if (attacker->prop->type == PROPTYPE_PLAYER) {
				shooter = playermgrGetPlayerNumByProp(attacker->prop);
			} else if (attacker->prop->type == PROPTYPE_CHR && attacker->aibot) {
				// Bot killer — mpPlayerGetIndex returns the mpchr index;
				// playerDieByShooter forwards it as the "shooter" playernum
				// and mpstatsRecordDeath uses func0f18d074 to bring it back
				// to the same mpchr index for ampchr lookup.
				shooter = mpPlayerGetIndex(attacker);
			}
		}
		if (shooter < 0) {
			// Genuine env/self death — fall back to victim → suicide branch.
			shooter = g_Vars.currentplayernum;
		}
		playerDieByShooter((u32)shooter, true);
	} else if (pl->isdead && !newisdead) {
		playerStartNewLife();
	}

	const bool dualwielding = (flags & (1 << 1)) != 0;
	if (!pl->isdead && (newweaponnum != pl->gunctrl.weaponnum || dualwielding != pl->gunctrl.dualwielding)) {
		pl->gunctrl.dualwielding = dualwielding;
		bgunEquipWeapon(newweaponnum);
	}

	setCurrentPlayerNum(prevplayernum);

	return src->error;
}

u32 netmsgSvcPropMoveWrite(struct netbuf *dst, struct prop *prop, struct coord *initrot)
{
	// prop->obj, prop->chr, prop->door etc. all alias the same union slot, so
	// `prop->obj != NULL` is meaninglessly true for any prop that has chr/door
	// data instead of an obj. This caused a past crash: netEndFrame broadcasts
	// SVC_PROP_MOVE for sim props (PROPTYPE_CHR), and the old code didn't check
	// prop->type, so it read random chr-struct bytes as OBJHFLAG_PROJECTILE and
	// dereferenced a junk pointer inside mtx4GetRotation. Now we gate the
	// obj/projectile path strictly on prop->type, avoiding the misread.
	const bool has_obj = (prop->type == PROPTYPE_OBJ
			|| prop->type == PROPTYPE_WEAPON
			|| prop->type == PROPTYPE_DOOR)
		&& prop->obj != NULL;
	u8 flags = has_obj ? 1 : 0;

	struct projectile *projectile = NULL;
	if (has_obj) {
		if (prop->obj->hidden & OBJHFLAG_EMBEDDED) {
			projectile = prop->obj->embedment->projectile;
		} else if (prop->obj->hidden & OBJHFLAG_PROJECTILE) {
			projectile = prop->obj->projectile;
		}
		if (projectile) {
			flags |= (1 << 1);
			// AUTO-DERIVE PROJECTILE ROTATION when the caller didn't supply one.
			// Most callers pass initrot=NULL because they don't track rotation
			// directly. Without bit 2 set, the read side leaves projectile->mtx
			// at whatever it was after allocation, so rockets that should be
			// pointing along their flight path render as if they'd been spawned
			// at their default pose. Extract the rotation from the projectile's
			// current matrix and ship it on every move so visual orientation
			// matches what the server is rendering.
			struct coord derived_rot = {0, 0, 0};
			if (!initrot) {
				mtx4GetRotation(projectile->mtx.m, &derived_rot);
				initrot = &derived_rot;
			}
			if (initrot) {
				flags |= (1 << 2);
			}
			if (prop->obj->type == OBJTYPE_HOVERPROP || prop->obj->type == OBJTYPE_HOVERBIKE) {
				flags |= (1 << 3);
			}
		}
	}

	// BIT 4: CHR-STATE EXTENSION. For PROPTYPE_CHR we append yrot, animation
	// state, anim speed, held weapons, and aim properties. Clients don't run
	// botTick (sim AI is server-only), so none of these update on their own:
	// without the block, sims end up stuck in their spawn anim (often a T-pose
	// because the bot AI normally drives the first modelSetAnimation), facing
	// their spawn direction, with empty hands and rigid posture. actiontype is
	// included in the wire format for compatibility but is discarded on read
	// (see read side for the union-data crash rationale).
	const bool wantChrState = (prop->type == PROPTYPE_CHR) && prop->chr;
	if (wantChrState) {
		flags |= (1 << 4);
	}

	netbufWriteU8(dst, SVC_PROP_MOVE);
	netbufWriteU8(dst, flags);
	netbufWritePropPtr(dst, prop);
	netbufWriteCoord(dst, &prop->pos);
	netbufWriteRooms(dst, prop->rooms, ARRAYCOUNT(prop->rooms));
	if (projectile) {
		netbufWriteCoord(dst, &projectile->speed);
		netbufWriteF32(dst, projectile->unk0dc);
		netbufWriteU32(dst, projectile->flags);
		netbufWriteS8(dst, projectile->bouncecount);
		netbufWritePropPtr(dst, projectile->ownerprop);
		netbufWritePropPtr(dst, projectile->targetprop);
		if (initrot) {
			netbufWriteCoord(dst, initrot);
		}
		if (prop->obj->type == OBJTYPE_HOVERPROP || prop->obj->type == OBJTYPE_HOVERBIKE) {
			netbufWriteF32(dst, projectile->unk08c);
			netbufWriteF32(dst, projectile->unk098);
			netbufWriteF32(dst, projectile->unk0e0);
			netbufWriteF32(dst, projectile->unk0e4);
			netbufWriteF32(dst, projectile->unk0ec);
			netbufWriteF32(dst, projectile->unk0f0);
		}
	}

	if (wantChrState) {
		struct chrdata *chr = prop->chr;
		// ACTIONTYPE: intentionally NOT USED on the client. See netmsgSvcPropMoveRead
		// for rationale (it would crash due to uninitialized action-state union data).
		netbufWriteS8(dst, chr->actiontype);
		// BODY ROTATION: yrot for view direction. The server derives this from AI
		// decisions; clients apply it directly in modelSetChrRotY so the chr's body
		// faces the right direction.
		netbufWriteF32(dst, chrGetRotY(chr));
		// ANIMATION: animnum, current frame index, and playback speed. anim->speed
		// is set on the server by playerChooseThirdPersonAnimation (called via
		// botApplyMovement) and scales the cycle to match the chr's actual
		// movement rate — fast strafe-run, slow walk, etc. Without syncing it
		// the client's sim would keep whatever speed was last assigned (typically
		// the value from the last modelSetAnimation call) and the running cycle
		// would no longer line up with how fast the body is actually traversing
		// world units. animnum=0 is the sentinel for "anim not yet allocated";
		// the read side leaves the chr's current anim untouched in that case.
		if (chr->model && chr->model->anim) {
			netbufWriteS16(dst, chr->model->anim->animnum);
			netbufWriteS16(dst, chr->model->anim->framea);
			netbufWriteF32(dst, chr->model->anim->speed);
			// FLIP was tried here and reverted: the goal was to fix sims
			// appearing left-handed when the server played a flipped variant
			// (e.g. left-strafe), but syncing the bit broke Skedar and other
			// maps. Two failure modes were hit when the client called
			// modelSetAnimation with the wire flip:
			//   1. Skedar bot models don't have the same flipped-bone remap
			//      that humans do, so the skeleton ended up referencing parts
			//      that don't exist for that race.
			//   2. anim->flip toggles frequently for strafing bots; every flip
			//      transition tripped the "anim differs → modelSetAnimation"
			//      path on the read side, which resets frame counters, so the
			//      anim cycle constantly restarted and locked / jittered.
			// Better to ship without flip sync (sim's left/right hand may be
			// cosmetically wrong during strafes) than break entire stages.
			// If revisited, gate by chr race / model skel and don't trigger
			// modelSetAnimation purely on a flip change.
		} else {
			netbufWriteS16(dst, 0);
			netbufWriteS16(dst, 0);
			netbufWriteF32(dst, 1.0f);
		}
		// HELD WEAPONS: per-hand weaponnum (or -1 if unarmed). The client doesn't
		// run bot AI (which controls weapon swaps via chrGiveWeapon when
		// changeguntimer60 elapses), so sims have no weapon props on the client
		// side. Send the server's current choices so the client can spawn/sync
		// matching weapon props to render in the chr's hands. These are local
		// client-side props (syncid=0, no network references) that attach to hand
		// bones and get cleaned up when the sync message changes the weaponnum.
		for (s32 h = 0; h < 2; ++h) {
			s8 wn = -1;
			if (chr->weapons_held[h] && chr->weapons_held[h]->obj
					&& chr->weapons_held[h]->obj->type == OBJTYPE_WEAPON
					&& chr->weapons_held[h]->weapon) {
				wn = (s8)chr->weapons_held[h]->weapon->weaponnum;
			}
			netbufWriteS8(dst, wn);
		}
		// AIM PROPERTIES: drive the chr's upper-body pose (shoulders, waist rotation).
		// chrHandleJointPositioned uses these in rendering: shoulders pivot on
		// aimuplshoulder/aimuprshoulder, waist xrot on aimupback, waist yrot on
		// aimsideback+angleoffset. Without these, sims on the client keep their
		// arms pointed straight forward regardless of target/aim direction, making
		// the held weapon not align with the chr's actual aim. angleoffset is
		// aibot-specific (AI angle offset from target); non-aibots send 0.
		netbufWriteF32(dst, chr->aimupback);
		netbufWriteF32(dst, chr->aimsideback);
		netbufWriteF32(dst, chr->aimuplshoulder);
		netbufWriteF32(dst, chr->aimuprshoulder);
		netbufWriteF32(dst, chr->aibot ? chr->aibot->angleoffset : 0.f);
	}

	return dst->error;
}

u32 netmsgSvcPropMoveRead(struct netbuf *src, struct netclient *srccl)
{
	const u8 flags = netbufReadU8(src);
	struct prop *prop = netbufReadPropPtr(src);
	struct coord pos; netbufReadCoord(src, &pos);
	RoomNum rooms[8] = { -1 }; netbufReadRooms(src, rooms, ARRAYCOUNT(rooms));

	if (src->error || !prop) {
		return src->error;
	}

	if (srccl->state < CLSTATE_GAME) {
		return 1;
	}

	// Save the pre-update position so the chr-state block can blend toward
	// the wire pos instead of snapping. Projectiles still hard-snap below.
	const struct coord oldpos = prop->pos;

	prop->pos = pos;

	if (!propRoomsEqual(rooms, prop->rooms)) {
		if (prop->active) {
			propDeregisterRooms(prop);
		}
		roomsCopy(rooms, prop->rooms);
		if (prop->active) {
			propRegisterRooms(prop);
		}
	}

	// obj / projectile section — present only when bit 0 is set
	if (flags & (1 << 0)) {
		if (!prop->obj) {
			sysLogPrintf(LOG_WARNING, "NET: SVC_PROP_MOVE: prop %u should have an obj, but doesn't", prop->syncid);
			return 1;
		}

		if (flags & (1 << 1)) {
			// create a projectile for this prop if it isn't already there
			func0f0685e4(prop);

			struct projectile *projectile = NULL;
			if (prop->obj->hidden & OBJHFLAG_EMBEDDED) {
				projectile = prop->obj->embedment->projectile;
			} else if (prop->obj->hidden & OBJHFLAG_PROJECTILE) {
				projectile = prop->obj->projectile;
			}

			if (!projectile) {
				sysLogPrintf(LOG_WARNING, "NET: SVC_PROP_MOVE: prop %u should have a projectile, but doesn't", prop->syncid);
				return 1;
			}

			netbufReadCoord(src, &projectile->speed);
			projectile->unk0dc = netbufReadF32(src);
			projectile->flags = netbufReadU32(src);
			projectile->bouncecount = netbufReadS8(src);
			projectile->ownerprop = netbufReadPropPtr(src);
			projectile->targetprop = netbufReadPropPtr(src);

			if (flags & (1 << 2)) {
				struct coord initrot; netbufReadCoord(src, &initrot);
				mtx4LoadRotation(&initrot, &projectile->mtx);
			}

			if (flags & (1 << 3)) {
				projectile->unk08c = netbufReadF32(src);
				projectile->unk098 = netbufReadF32(src);
				projectile->unk0e0 = netbufReadF32(src);
				projectile->unk0e4 = netbufReadF32(src);
				projectile->unk0ec = netbufReadF32(src);
				projectile->unk0f0 = netbufReadF32(src);
			}

			prop->pos = pos;
		}
	}

	// CHR-STATE BLOCK: present when bit 4 is set, used for PROPTYPE_CHR props
	// (sims/NPCs). Contains orientation, animation, aim, and weapons. We READ
	// actiontype from the wire but intentionally DISCARD it — see the comment
	// below on why actiontype can't be safely applied on the client.
	if (flags & (1 << 4)) {
		// Crash-hunt diagnostic: log first N chr-state arrivals after match
		// start. Lets us tell whether the client got as far as processing a
		// sim's chr-state block before crashing. Static counter resets each
		// run (it's a TU-local, only meaningful for the current process).
		// Capped low (20) so a healthy match doesn't pollute the diag log
		// past the initial-frame visibility we actually want.
		static u32 chrstate_logged = 0;
		if (chrstate_logged < 20u && prop && prop->chr) {
			netDiagLogf("chrstate_enter", "sid=%u race=%d body=%d",
				(u32)prop->syncid,
				(s32)(prop->chr ? (s32)prop->chr->race : -1),
				(s32)(prop->chr ? (s32)prop->chr->bodynum : -1));
			chrstate_logged++;
		}
		// ACTIONTYPE: discarded, not applied. Reason: most action states
		// (ACT_GOPOS, ACT_ATTACK, ACT_PATROL, ACT_THROWGRENADE, etc.) store
		// per-state data in the chr->act_* union. The matching chrTick* functions
		// blindly dereference this union without null-checking, assuming the AI
		// properly initialized it. The server populates the union as the AI
		// transitions states. The client doesn't receive union data, so applying
		// the server's actiontype would leave the union zero-init'd: calling
		// chrTickGoPos with uninitialized waypoint data, or chrTickAttack with
		// uninitialized target data, crashes inside e.g. chrGoPosGetCurWaypointInfoWithFlags
		// (chraction.c:5448, seen in the wild). Safer to force ACT_STAND, which
		// only calls chrTickStand — a no-op with zero-init data. The visible
		// animation is still correct because animnum below drives the skeletal
		// anim, so attack/run anims play correctly, just without the matching
		// ai-tick logic. Better than T-pose and safe from crashes.
		(void)netbufReadS8(src); // received actiontype, intentionally discarded
		const f32 yrot = netbufReadF32(src);
		const s16 animnum = netbufReadS16(src);
		const s16 animframe = netbufReadS16(src);
		const f32 animspeed = netbufReadF32(src);
		// flip byte was removed — see Write side. The client keeps whatever
		// flip the local chrTick happened to set; better than the maps
		// breaking when we tried to sync it.
		const s8 weapon_r = netbufReadS8(src);
		const s8 weapon_l = netbufReadS8(src);
		const f32 aimupback = netbufReadF32(src);
		const f32 aimsideback = netbufReadF32(src);
		const f32 aimuplshoulder = netbufReadF32(src);
		const f32 aimuprshoulder = netbufReadF32(src);
		const f32 angleoffset = netbufReadF32(src);
		if (prop->chr) {
			struct chrdata *chr = prop->chr;
			chr->actiontype = ACT_STAND;

			// POSITION SMOOTHING: under packet loss or low update rate, the chr
			// would snap between server positions on each catch-up packet (visible
			// jitter / teleport). Blend by moving 50% of the way from the last
			// received pos toward the new one, so the chr glides over a couple of
			// receives instead of stepping. Skip the blend if the per-receive
			// delta exceeds 80 units in any axis combined (sqrt(6400)) — that's
			// above what AI movement can produce in one server update, so it's
			// almost certainly a respawn / kill-plane drop and blending would
			// stretch the chr across the map for a frame.
			struct coord smoothpos = pos;
			const f32 dx = pos.x - oldpos.x;
			const f32 dy = pos.y - oldpos.y;
			const f32 dz = pos.z - oldpos.z;
			const f32 dist_sq = dx*dx + dy*dy + dz*dz;
			if (dist_sq < 80.f * 80.f) {
				const f32 alpha = 0.5f;
				smoothpos.x = oldpos.x + dx * alpha;
				smoothpos.y = oldpos.y + dy * alpha;
				smoothpos.z = oldpos.z + dz * alpha;
				prop->pos = smoothpos;
			}

			// Crash-hunt: one shared counter gates all per-step diagnostic
			// logs in this block. Limits total volume so the diag log stays
			// readable while still bracketing the crash to a specific step
			// (rootpos / rotY / setAnim / give-weapon).
			static u32 chrstate_step_logged = 0;
			const bool log_steps = (chrstate_step_logged < 60u);
			if (log_steps) chrstate_step_logged++;

			// POSITION TO MODEL: setting prop->pos alone isn't enough. Rendering
			// reads rwdata->chrinfo.pos (the model's internal root), not prop->pos.
			// modelSetRootPosition writes it. Without this call, the sim's running
			// animation plays in place — the model root never moves to the new
			// world position. This mirrors what botApplyMovement does server-side.
			if (chr->model) {
				if (log_steps) netDiagLogf("step_rootpos", "sid=%u", (u32)prop->syncid);
				modelSetRootPosition(chr->model, &smoothpos);
			}

			// ROTATION: apply yrot to BOTH chr->aibot->roty (via chrSetRotY) AND
			// the model's chrinfo yrot (via modelSetChrRotY). These must stay
			// synced: rendering uses chrinfo.yrot, and AI-facing accessors like
			// chrGetRotY return aibot->roty. chrSetRotY alone doesn't propagate
			// to chrinfo for aibots, so without the explicit modelSetChrRotY call,
			// sims stay facing their spawn direction. The server keeps them in
			// sync because botApplyMovement calls modelSetChrRotY directly after
			// moving them — we replicate that here on the client.
			if (log_steps) netDiagLogf("step_rotY", "sid=%u", (u32)prop->syncid);
			chrSetRotY(chr, yrot);
			if (chr->model) {
				modelSetChrRotY(chr->model, yrot);
			}

			// ANIMATION: snap when animnum diverges from current. Guard against
			// invalid ids first: out-of-range animnums index past g_Anims and
			// crash inside animLoadHeader. animHasFrames filters sentinel 0,
			// negatives, and anything beyond g_NumAnims.
			//
			// Two paths once validated:
			//   - Different anim: full modelSetAnimation, which resets frame
			//     counters to animframe and applies the server's speed. Pass a
			//     small merge time (0.0625) so the changeover blends out the
			//     previous anim's pose over the next tick instead of popping.
			//   - Same anim: just poke anim->speed so modelTickAnim picks up
			//     the new playback rate. Re-calling modelSetAnimation here would
			//     reset framea/frameb to animframe and visibly snap the cycle
			//     backward whenever the server's frame index trailed ours.
			if (animnum > 0 && animHasFrames(animnum) && chr->model && chr->model->anim) {
				// Force flip=0 on the client (right-handed). The server
				// may pick flip=1 for some animations (left-strafe etc.),
				// but we deliberately don't sync that bit (it broke Skedar
				// maps when we tried — see netmsgSvcPropMoveWrite's FLIP
				// comment). Instead we lock the client to right-handed
				// always, matching the user-visible expectation that every
				// chr is right-handed. Without this, anim slots recycled
				// from a previously left-handed chr leak flip=1 and the
				// sim's weapon appears in the wrong hand.
				chr->model->anim->flip = 0;
				if (chr->model->anim->animnum != animnum) {
					if (log_steps) netDiagLogf("step_setanim", "sid=%u animnum=%d body=%d",
							(u32)prop->syncid, (s32)animnum, (s32)chr->bodynum);
					modelSetAnimation(chr->model, animnum, 0, (f32)animframe, animspeed, 0.0625f);
				} else {
					chr->model->anim->speed = animspeed;
				}
			}

			// AIM PROPERTIES: drive upper-body joint rotations in chrHandleJointPositioned:
			// shoulders (aimuplshoulder/aimuprshoulder), waist rotation (aimupback +
			// aimsideback + angleoffset). Without these, the chr's arms point
			// straight forward regardless of aim direction, so the held weapon
			// doesn't align with actual firing direction. We also set aimend*
			// fields to the current values and zero aimendcount so that
			// chrUpdateAimProperties (called on each fulltick) snaps aim* to
			// aimend* immediately instead of slowly interpolating back to some
			// previous AI state (which we don't receive on the client). This
			// ensures the sim's posture matches the server immediately.
			chr->aimupback = aimupback;
			chr->aimsideback = aimsideback;
			chr->aimuplshoulder = aimuplshoulder;
			chr->aimuprshoulder = aimuprshoulder;
			chr->aimendback = aimupback;
			chr->aimendsideback = aimsideback;
			chr->aimendlshoulder = aimuplshoulder;
			chr->aimendrshoulder = aimuprshoulder;
			chr->aimendcount = 0;
			if (chr->aibot) {
				// angleoffset: added to waist yrot in chrHandleJointPositioned,
				// used by botApplyMovement to decouple body-facing from animation
				// direction during strafe runs (so the chr can fire left while
				// running forward). Syncing it is essential for the weapon to
				// align with the server's upper-body orientation.
				chr->aibot->angleoffset = angleoffset;
			}

			// HELD WEAPONS: sync per-hand weapon choices. The server's bot AI
			// spawns weapon props via chrGiveWeapon when changeguntimer60 elapses
			// (bot.c). That loop is gated to server-only on the client, so sims
			// have no weapon props locally — the chr animation plays the armed
			// pose, but with nothing in hand. This sync fixes that: for each hand,
			// compare the wire weaponnum (-1 if empty) to the current held weapon.
			// If different, delete the old one and spawn a new one via
			// chrGiveWeapon. New weapon props get syncid=0 (client-allocated,
			// propAllocate) so they're not referenced by any network messages —
			// the chr->weapons_held[] link is enough to keep them positioned and
			// rendered attached to hand bones. This gives the sim visible guns
			// matching the server without needing per-weapon network sync.
			static const u32 hand_flags[2] = { 0, OBJFLAG_WEAPON_LEFTHANDED };
			const s8 want_weapon[2] = { weapon_r, weapon_l };
			for (s32 h = 0; h < 2; ++h) {
				const s32 want = want_weapon[h];
				const s32 cur = (chr->weapons_held[h] && chr->weapons_held[h]->obj
						&& chr->weapons_held[h]->obj->type == OBJTYPE_WEAPON
						&& chr->weapons_held[h]->weapon)
					? (s32)chr->weapons_held[h]->weapon->weaponnum : -1;
				if (want == cur) {
					continue;
				}
				// Mismatch: delete old, spawn new
				if (chr->weapons_held[h]) {
					if (chr->weapons_held[h]->obj) {
						chr->weapons_held[h]->obj->hidden |= OBJHFLAG_DELETING;
					}
					chr->weapons_held[h] = NULL;
				}
				if (want >= 0) {
					const s32 modelnum = playermgrGetModelOfWeapon(want);
					if (modelnum >= 0) {
						// Crash-hunt diagnostic: log first N weapon-give
						// attempts so we can tell if the chrGiveWeapon
						// path is the one that crashes. _pre fires
						// before the call; _post fires after. If we see
						// _pre with no matching _post, the crash is
						// inside chrGiveWeapon (Skedar models on
						// human-weapon bones are a known suspect).
						static u32 giveweap_logged = 0;
						const bool log_this = (giveweap_logged < 30u);
						if (log_this) {
							netDiagLogf("giveweap_pre", "sid=%u h=%d wpn=%d model=%d body=%d",
								(u32)prop->syncid, h, want, modelnum,
								(s32)(chr ? chr->bodynum : -1));
						}
						chrGiveWeapon(chr, modelnum, want, hand_flags[h]);
						if (log_this) {
							netDiagLogf("giveweap_post", "sid=%u h=%d", (u32)prop->syncid, h);
							giveweap_logged++;
						}
					}
				}
			}
		}
	}

	return src->error;
}

u32 netmsgSvcPropSpawnWrite(struct netbuf *dst, struct prop *prop)
{
	const u8 msgflags = (prop->active != 0) | ((prop->obj != NULL) << 1) | ((prop->forcetick != 0) << 2);
	const u8 objtype = prop->obj ? prop->obj->type : 0;

	netbufWriteU8(dst, SVC_PROP_SPAWN);
	netbufWriteU8(dst, msgflags);
	netbufWriteU32(dst, prop->syncid);
	netbufWriteCoord(dst, &prop->pos);
	netbufWriteRooms(dst, prop->rooms, ARRAYCOUNT(prop->rooms));
	netbufWritePropPtr(dst, prop->parent);
	netbufWriteU8(dst, prop->type);
	netbufWriteU8(dst, objtype);
	netbufWriteU8(dst, prop->flags);

	switch (prop->type) {
		case PROPTYPE_WEAPON:
			// dropped gun or projectile
			netbufWriteS16(dst, prop->weapon->base.modelnum);
			netbufWriteU8(dst, prop->weapon->weaponnum);
			netbufWriteS8(dst, prop->weapon->dualweaponnum);
			netbufWriteS8(dst, prop->weapon->unk5d);
			netbufWriteS8(dst, prop->weapon->unk5e);
			netbufWriteU8(dst, prop->weapon->gunfunc);
			netbufWriteS16(dst, prop->weapon->timer240);
			break;
		case PROPTYPE_OBJ:
			// we already send most of the important obj stuff below, so
			netbufWriteS16(dst, prop->obj->modelnum);
			if (objtype == OBJTYPE_AUTOGUN) {
				// thrown laptop probably
				struct autogunobj *autogun = (struct autogunobj *)prop->obj;
				const u8 ownerplayernum = (prop->obj->hidden & 0xf0000000) >> 28;
				netbufWriteU8(dst, autogun->ammoquantity);
				netbufWriteU8(dst, autogun->firecount);
				netbufWriteU8(dst, autogun->targetteam);
				netbufWriteU8(dst, g_Vars.players[ownerplayernum]->client->id);
			}
			break;
		default:
			break;
	}

	if (prop->obj) {
		netbufWriteU32(dst, prop->obj->flags);
		netbufWriteU32(dst, prop->obj->flags2);
		netbufWriteU32(dst, prop->obj->flags3);
		netbufWriteU32(dst, prop->obj->hidden);
		netbufWriteU8(dst, prop->obj->hidden2);
		netbufWriteU16(dst, prop->obj->extrascale);
		netbufWriteS16(dst, prop->obj->pad);
		for (s32 i = 0; i < 3; ++i) {
			for (s32 j = 0; j < 3; ++j) {
				netbufWriteF32(dst, prop->obj->realrot[i][j]);
			}
		}
		if ((prop->obj->hidden & OBJHFLAG_PROJECTILE) && prop->obj->projectile) {
			netbufWriteCoord(dst, &prop->obj->projectile->nextsteppos);
			netbufWritePropPtr(dst, prop->obj->projectile->ownerprop);
			netbufWritePropPtr(dst, prop->obj->projectile->targetprop);
			netbufWriteU32(dst, prop->obj->projectile->flags);
			netbufWriteF32(dst, prop->obj->projectile->unk08c);
			netbufWriteS16(dst, prop->obj->projectile->pickuptimer240);
			netbufWriteS16(dst, prop->obj->projectile->droptype);
			netbufWriteMtxf(dst, &prop->obj->projectile->mtx);
			if (prop->obj->projectile->flags & PROJECTILEFLAG_POWERED) {
				netbufWriteF32(dst, prop->obj->projectile->unk010);
				netbufWriteF32(dst, prop->obj->projectile->unk014);
				netbufWriteF32(dst, prop->obj->projectile->unk018);
			}
		}
	}

	return dst->error;
}

u32 netmsgSvcPropSpawnRead(struct netbuf *src, struct netclient *srccl)
{
	const u8 msgflags = netbufReadU8(src);
	const u32 syncid = netbufReadU32(src);
	struct coord pos; netbufReadCoord(src, &pos);
	RoomNum rooms[8] = { -1 }; netbufReadRooms(src, rooms, ARRAYCOUNT(rooms));
	struct prop *parent = netbufReadPropPtr(src);
	const u8 type = netbufReadU8(src);
	const u8 objtype = netbufReadU8(src);
	const u8 propflags = netbufReadU8(src);

	if (src->error) {
		return src->error;
	}

	if (srccl->state < CLSTATE_GAME) {
		return 1;
	}

	struct prop *prop = (type == PROPTYPE_OBJ && objtype == OBJTYPE_AUTOGUN) ? NULL : propAllocate();

	if (type == PROPTYPE_WEAPON) {
		const s16 modelnum = netbufReadS16(src);
		const u8 weaponnum = netbufReadU8(src);
		const s8 dualweaponnum = netbufReadS8(src);
		const s8 unk5d = netbufReadS8(src);
		const s8 unk5e = netbufReadS8(src);
		const u8 gunfunc = netbufReadU8(src);
		const s16 timer240 = netbufReadS16(src);
		setupLoadModeldef(modelnum);
		struct modeldef *modeldef = g_ModelStates[modelnum].modeldef;
		struct model *model = modelmgrInstantiateModelWithoutAnim(modeldef);
		struct weaponobj *weapon = weaponCreate(prop == NULL, model == NULL, modeldef);
		struct weaponobj tmp = {
			256,                    // extrascale
			0,                      // hidden2
			OBJTYPE_WEAPON,         // type
			0,                      // modelnum
			-1,                     // pad
			OBJFLAG_FALL,           // flags
			0,                      // flags2
			0,                      // flags3
			NULL,                   // prop
			NULL,                   // model
			1, 0, 0,                // realrot
			0, 1, 0,
			0, 0, 1,
			0,                      // hidden
			NULL,                   // geo
			NULL,                   // projectile
			0,                      // damage
			1000,                   // maxdamage
			0xff, 0xff, 0xff, 0x00, // shadecol
			0xff, 0xff, 0xff, 0x00, // nextcol
			0x0fff,                 // floorcol
			0,                      // tiles
			0,                      // weaponnum
			0,                      // unk5d
			0,                      // unk5e
			0,                      // gunfunc
			0,                      // fadeouttimer60
			-1,                     // dualweaponnum
			-1,                     // timer240
			NULL,                   // dualweapon
		};
		*weapon = tmp;
		weapon->base.modelnum = modelnum;
		weapon->weaponnum = weaponnum;
		weapon->unk5d = unk5d;
		weapon->unk5e = unk5e;
		weapon->gunfunc = gunfunc;
		weapon->timer240 = timer240;
		prop = func0f08adc8(weapon, modeldef, prop, model);
	} else if (type == PROPTYPE_OBJ) {
		const s16 modelnum = netbufReadS16(src);
		if (objtype == OBJTYPE_AUTOGUN) {
			// thrown laptop?
			const u8 ammocount = netbufReadU8(src);
			const u8 firecount = netbufReadU8(src);
			const u8 targetteam = netbufReadU8(src);
			const u8 clid = netbufReadU8(src);
			struct chrdata *ownerchr = g_NetClients[clid].player->prop->chr;
			struct autogunobj *obj = laptopDeploy(modelnum, NULL, ownerchr);
			obj->ammoquantity = ammocount;
			obj->firecount = firecount;
			obj->targetteam = targetteam;
			prop = obj->base.prop;
		}
	}

	if (prop) {
		prop->type = type;
		prop->syncid = syncid;
		prop->pos = pos;
		prop->forcetick = (msgflags & (1 << 2)) != 0;
		// prop->flags = propflags;
		roomsCopy(rooms, prop->rooms);
		if (msgflags & (1 << 0)) {
			propActivate(prop);
			propRegisterRooms(prop);
		} else {
			propPause(prop);
		}
		if (propflags & PROPFLAG_ENABLED) {
			propEnable(prop);
		} else {
			propDisable(prop);
		}
		if (parent) {
			propReparent(prop, parent);
		}
	} else {
		sysLogPrintf(LOG_WARNING, "NET: no prop allocated when spawning prop %u (%u)", syncid, type);
		return src->error;
	}

	if (msgflags & (1 << 1)) {
		const u32 flags = netbufReadU32(src);
		const u32 flags2 = netbufReadU32(src);
		const u32 flags3 = netbufReadU32(src);
		const u32 hidden = netbufReadHidden(src);
		const u8 hidden2 = netbufReadU8(src);
		const u16 extrascale = netbufReadU16(src);
		const s16 pad = netbufReadS16(src);
		for (s32 i = 0; i < 3; ++i) {
			for (s32 j = 0; j < 3; ++j) {
				prop->obj->realrot[i][j] = netbufReadF32(src);
			}
		}
		if (prop->obj) {
			if (hidden & OBJHFLAG_PROJECTILE) {
				func0f0685e4(prop);
				netbufReadCoord(src, &prop->obj->projectile->nextsteppos);
				prop->obj->projectile->ownerprop = netbufReadPropPtr(src);
				prop->obj->projectile->targetprop = netbufReadPropPtr(src);
				prop->obj->projectile->flags = netbufReadU32(src);
				prop->obj->projectile->unk08c = netbufReadF32(src);
				prop->obj->projectile->pickuptimer240 = netbufReadS16(src);
				prop->obj->projectile->droptype = netbufReadS16(src);
				prop->obj->projectile->flighttime240 = 0;
				netbufReadMtxf(src, &prop->obj->projectile->mtx);
				if (prop->obj->projectile->flags & PROJECTILEFLAG_POWERED) {
					// rocket; get acceleration and realrot
					prop->obj->projectile->unk010 = netbufReadF32(src);
					prop->obj->projectile->unk014 = netbufReadF32(src);
					prop->obj->projectile->unk018 = netbufReadF32(src);
					prop->obj->projectile->powerlimit240 = TICKS(1200);
					prop->obj->projectile->smoketimer240 = TICKS(24);
				}
				if ((type == PROPTYPE_WEAPON && (prop->obj->projectile->flags & PROJECTILEFLAG_00000002)) || objtype == OBJTYPE_AUTOGUN) {
					// this is a thrown projectile, play throw sound
					psCreate(NULL, prop, SFX_THROW, -1, -1, 0, 0, PSTYPE_NONE, NULL, -1, NULL, -1, -1, -1, -1);
				}
			}
			prop->obj->flags = flags;
			prop->obj->flags2 = flags2;
			prop->obj->flags3 = flags3;
			prop->obj->hidden = hidden;
			prop->obj->hidden2 = hidden2;
			prop->obj->extrascale = extrascale;
			prop->obj->pad = pad;
			if (prop->obj->model) {
				modelSetScale(prop->obj->model, prop->obj->model->scale * ((f32)extrascale / 256.f));
			}
		}
	}

	// just in case
	prop->pos = pos;

	return src->error;
}

u32 netmsgSvcPropDamageWrite(struct netbuf *dst, struct prop *prop, f32 damage, struct coord *pos, s32 weaponnum, s32 playernum)
{
	if (!prop || !prop->obj) {
		return dst->error;
	}
	netbufWriteU8(dst, SVC_PROP_DAMAGE);
	netbufWritePropPtr(dst, prop);
	netbufWriteCoord(dst, pos);
	netbufWriteF32(dst, prop->obj->damage);
	netbufWriteF32(dst, damage);
	netbufWriteS8(dst, weaponnum);
	netbufWriteS8(dst, playernum);
	netbufWriteU32(dst, prop->obj->hidden & ~(OBJHFLAG_PROJECTILE | OBJHFLAG_EMBEDDED));
	return dst->error;
}

u32 netmsgSvcPropDamageRead(struct netbuf *src, struct netclient *srccl)
{
	struct prop *prop = netbufReadPropPtr(src);
	struct coord pos; netbufReadCoord(src, &pos);
	const f32 damagepre = netbufReadF32(src);
	const f32 damage = netbufReadF32(src);
	const s8 weaponnum = netbufReadS8(src);
	const s8 playernum = netbufReadS8(src);
	const u32 hidden = netbufReadHidden(src);
	if (srccl->state < CLSTATE_GAME) {
		return src->error;
	}
	if (prop && prop->obj && prop->type != PROPTYPE_PLAYER && prop->type != PROPTYPE_CHR && !src->error) {
		prop->obj->damage = damagepre;
		prop->obj->hidden = hidden | (prop->obj->hidden & (OBJHFLAG_PROJECTILE | OBJHFLAG_EMBEDDED));
		objDamage(prop->obj, -damage, &pos, weaponnum, playernum);
	}
	return src->error;
}

u32 netmsgSvcPropPickupWrite(struct netbuf *dst, struct netclient *actcl, struct prop *prop, const s32 tickop)
{
	netbufWriteU8(dst, SVC_PROP_PICKUP);
	// 0xff = sim/AI pickup with no human attribution. The reader skips the
	// player-side bookkeeping (propPickupByPlayer / setCurrentPlayerNum) and
	// just runs the tickop so the prop disappears locally — matches the
	// server, which freed it via botPickupProp's objFree call.
	netbufWriteU8(dst, actcl ? actcl->id : 0xff);
	netbufWriteS8(dst, tickop);
	netbufWritePropPtr(dst, prop);
	return dst->error;
}

u32 netmsgSvcPropPickupRead(struct netbuf *src, struct netclient *srccl)
{
	const u8 clid = netbufReadU8(src);
	const s8 tickop = netbufReadS8(src);
	struct prop *prop = netbufReadPropPtr(src);
	if (src->error || !prop || srccl->state < CLSTATE_GAME) {
		return src->error;
	}

	if (clid == 0xff) {
		// Sim pickup: no human player to attribute. Just execute the tickop
		// (typically TICKOP_FREE) so the weapon / ammo crate disappears from
		// the client's world to mirror the server. Skipping propPickupByPlayer
		// avoids dereferencing a NULL currentplayer for inventory updates the
		// client doesn't care about anyway — sims are server-authoritative.
		if (tickop != TICKOP_NONE) {
			propExecuteTickOperation(prop, tickop);
		}
		return src->error;
	}

	if (clid >= NET_MAX_CLIENTS) {
		return src->error;
	}
	struct netclient *actcl = g_NetClients + clid;
	if (actcl->is_spectator) {
		// Spectator clients have no mpchr and no playernum — a SVC_PROP_PICKUP
		// referencing one is either a stale message or a peer bug. Run the
		// tickop so the prop still vanishes locally but skip the per-player
		// inventory bookkeeping that would deref a NULL currentplayer.
		if (tickop != TICKOP_NONE) {
			propExecuteTickOperation(prop, tickop);
		}
		return src->error;
	}

	const s32 prevplayernum = g_Vars.currentplayernum;
	setCurrentPlayerNum(actcl->playernum);

	propPickupByPlayer(prop, true);
	if (tickop != TICKOP_NONE) {
		propExecuteTickOperation(prop, tickop);
	}

	setCurrentPlayerNum(prevplayernum);

	return src->error;
}

u32 netmsgSvcPropUseWrite(struct netbuf *dst, struct prop *prop, struct netclient *usercl, const s32 tickop)
{
	netbufWriteU8(dst, SVC_PROP_USE);
	netbufWritePropPtr(dst, prop);
	netbufWriteU8(dst, usercl->id);
	netbufWriteS8(dst, tickop);
	return dst->error;
}

u32 netmsgSvcPropUseRead(struct netbuf *src, struct netclient *srccl)
{
	struct prop *prop = netbufReadPropPtr(src);
	const u8 clid = netbufReadU8(src);
	const s8 tickop = netbufReadS8(src);

	if (!prop || srccl->state < CLSTATE_GAME) {
		return src->error;
	}

	struct netclient *actcl = &g_NetClients[clid];
	if (actcl->is_spectator) {
		// Same rationale as in netmsgSvcPropPickupRead — don't index player
		// arrays by the spectator sentinel.
		return src->error;
	}

	const s32 prevplayernum = g_Vars.currentplayernum;
	setCurrentPlayerNum(actcl->playernum);

	s32 ownop;
	switch (prop->type) {
		case PROPTYPE_OBJ:
		case PROPTYPE_WEAPON:
			if (!prop->obj || prop->obj->type != OBJTYPE_LIFT) {
				ownop = propobjInteract(prop);
			} else {
				ownop = TICKOP_NONE;
			}
			break;
		default:
			// NOTE: doors and lifts are handled with SVC_PROP_DOOR/_LIFT
			// TODO: eventually remove this message completely
			ownop = TICKOP_NONE;
			break;
	}

	propExecuteTickOperation(prop, tickop);

	setCurrentPlayerNum(prevplayernum);

	return src->error;
}

u32 netmsgSvcPropDoorWrite(struct netbuf *dst, struct prop *prop, struct netclient *usercl)
{
	if (prop->type != PROPTYPE_DOOR || !prop->door) {
		return dst->error;
	}

	struct doorobj *door = prop->door;

	netbufWriteU8(dst, SVC_PROP_DOOR);
	netbufWritePropPtr(dst, prop);
	netbufWriteU8(dst, usercl ? usercl->id : NET_NULL_CLIENT);
	netbufWriteS8(dst, door->mode);
	netbufWriteU32(dst, door->base.flags);
	netbufWriteU32(dst, door->base.hidden);

	return dst->error;
}

u32 netmsgSvcPropDoorRead(struct netbuf *src, struct netclient *srccl)
{
	struct prop *prop = netbufReadPropPtr(src);
	const u8 clid = netbufReadU8(src);
	const s8 doormode = netbufReadS8(src);
	const u32 flags = netbufReadU32(src);
	const u32 hidden = netbufReadHidden(src);

	struct netclient *actcl = (clid == NET_NULL_CLIENT) ? NULL : &g_NetClients[clid];
	if (actcl && actcl->is_spectator) {
		// A spectator can't have triggered a door. Treat it like an attribution-
		// less event (NET_NULL_CLIENT) so we still process the door state but
		// skip the playernum swap.
		actcl = NULL;
	}

	if (!prop || srccl->state < CLSTATE_GAME) {
		return src->error;
	}

	if (!prop->door || prop->type != PROPTYPE_DOOR) {
		sysLogPrintf(LOG_WARNING, "NET: SVC_PROP_DOOR: prop %u should be a door, but isn't", prop->syncid);
		return src->error;
	}

	const s32 prevplayernum = g_Vars.currentplayernum;
	if (actcl) {
		setCurrentPlayerNum(actcl->playernum);
	}

	doorSetMode(prop->door, doormode);
	prop->door->base.hidden = hidden;
	prop->door->base.flags = flags;

	if (actcl) {
		setCurrentPlayerNum(prevplayernum);
	}

	return src->error;
}

u32 netmsgSvcPropLiftWrite(struct netbuf *dst, struct prop *prop)
{
	if (prop->type != PROPTYPE_OBJ || !prop->obj || prop->obj->type != OBJTYPE_LIFT) {
		return dst->error;
	}

	struct liftobj *lift = (struct liftobj *)prop->obj;

	netbufWriteU8(dst, SVC_PROP_LIFT);
	netbufWritePropPtr(dst, prop);
	netbufWriteS8(dst, lift->levelcur);
	netbufWriteS8(dst, lift->levelaim);
	netbufWriteF32(dst, lift->accel);
	netbufWriteF32(dst, lift->speed);
	netbufWriteF32(dst, lift->dist);
	netbufWriteU32(dst, lift->base.flags);
	netbufWriteCoord(dst, &prop->pos);
	netbufWriteRooms(dst, prop->rooms, ARRAYCOUNT(prop->rooms));

	return dst->error;
}

u32 netmsgSvcPropLiftRead(struct netbuf *src, struct netclient *srccl)
{
	struct prop *prop = netbufReadPropPtr(src);
	const s8 levelcur = netbufReadS8(src);
	const s8 levelaim = netbufReadS8(src);
	const f32 accel = netbufReadF32(src);
	const f32 speed = netbufReadF32(src);
	const f32 dist = netbufReadF32(src);
	const u32 flags = netbufReadU32(src);
	struct coord pos; netbufReadCoord(src, &pos);
	RoomNum rooms[8]; netbufReadRooms(src, rooms, ARRAYCOUNT(rooms));

	if (!prop || srccl->state < CLSTATE_GAME) {
		return src->error;
	}

	if (!prop->obj || prop->type != PROPTYPE_OBJ || prop->obj->type != OBJTYPE_LIFT) {
		sysLogPrintf(LOG_WARNING, "NET: SVC_PROP_LIFT: prop %u should be a lift, but isn't", prop->syncid);
		return src->error;
	}

	struct liftobj *lift = (struct liftobj *)prop->obj;
	lift->levelcur = levelcur;
	lift->levelaim = levelaim;
	lift->speed = speed;
	lift->dist = dist;
	lift->accel = accel;
	lift->base.flags = flags;

	prop->pos = pos;

	if (!propRoomsEqual(rooms, prop->rooms)) {
		if (prop->active) {
			propDeregisterRooms(prop);
		}
		roomsCopy(rooms, prop->rooms);
		if (prop->active) {
			propRegisterRooms(prop);
		}
	}

	return src->error;
}

u32 netmsgSvcChrDamageWrite(struct netbuf *dst, struct chrdata *chr, f32 damage, struct coord *vector, struct gset *gset,
		struct prop *aprop, s32 hitpart, bool damageshield, struct prop *prop2, s32 side, s16 *arg11, bool explosion, struct coord *explosionpos)
{
	const u8 flags = damageshield | (explosion << 1) | ((gset != NULL) << 2) |
		((aprop != NULL) << 3) | ((prop2 != NULL) << 4) | ((arg11 != NULL) << 5) | ((explosionpos != NULL) << 6);

	netbufWriteU8(dst, SVC_CHR_DAMAGE);
	netbufWriteU8(dst, flags);
	netbufWritePropPtr(dst, chr->prop);
	netbufWriteF32(dst, damage);
	netbufWriteCoord(dst, vector);
	netbufWriteS16(dst, hitpart);
	netbufWriteS16(dst, side);
	if (gset) {
		netbufWriteGset(dst, gset);
	}
	if (aprop) {
		netbufWritePropPtr(dst, aprop);
	}
	if (prop2) {
		netbufWritePropPtr(dst, prop2);
	}
	if (arg11) {
		netbufWriteS16(dst, arg11[0]);
		netbufWriteS16(dst, arg11[1]);
		netbufWriteS16(dst, arg11[2]);
	}
	if (explosionpos) {
		netbufWriteCoord(dst, explosionpos);
	}

	return dst->error;
}

u32 netmsgSvcChrDamageRead(struct netbuf *src, struct netclient *srccl)
{
	const u8 flags = netbufReadU8(src);
	struct prop *chrprop = netbufReadPropPtr(src);
	const f32 damage = netbufReadF32(src);
	struct coord vector; netbufReadCoord(src, &vector);
	const s16 hitpart = netbufReadS16(src);
	const s16 side = netbufReadS16(src);

	struct gset gsetvalue;
	struct gset *gset = NULL;
	if (flags & (1 << 2)) {
		netbufReadGset(src, &gsetvalue);
		gset = &gsetvalue;
	}

	struct prop *aprop = (flags & (1 << 3)) ? netbufReadPropPtr(src) : NULL;
	struct prop *prop2 = (flags & (1 << 4)) ? netbufReadPropPtr(src) : NULL;

	s16 arg11[3], *arg11ptr = NULL;
	if (flags & (1 << 5)) {
		arg11[0] = netbufReadS16(src);
		arg11[1] = netbufReadS16(src);
		arg11[2] = netbufReadS16(src);
		arg11ptr = arg11;
	}

	struct coord explosionpos, *explosionposptr = NULL;
	if (flags & (1 << 6)) {
		netbufReadCoord(src, &explosionpos);
		explosionposptr = &explosionpos;
	}

	if (src->error || srccl->state < CLSTATE_GAME) {
		return src->error;
	}

	if (!chrprop || !chrprop->chr) {
		return src->error;
	}

	const bool damageshield = (flags & (1 << 0)) != 0;
	const bool explosion = (flags & (1 << 1)) != 0;

	const s32 prevplayernum = g_Vars.currentplayernum;
	if (chrprop->type == PROPTYPE_PLAYER) {
		setCurrentPlayerNum(playermgrGetPlayerNumByProp(chrprop));
	}

	chrDamage(chrprop->chr, damage, &vector, gset, aprop, hitpart, damageshield, prop2, NULL, NULL, side, arg11ptr, explosion, explosionposptr);

	if (chrprop->type == PROPTYPE_PLAYER) {
		setCurrentPlayerNum(prevplayernum);
	}

	return src->error;
}

u32 netmsgSvcChrDisarmWrite(struct netbuf *dst, struct chrdata *chr, struct prop *aprop, u8 weaponnum, f32 wpndamage, struct coord *wpnpos)
{
	netbufWriteU8(dst, SVC_CHR_DISARM);
	netbufWritePropPtr(dst, chr->prop);
	netbufWritePropPtr(dst, aprop);
	netbufWriteU8(dst, weaponnum);
	netbufWriteF32(dst, wpndamage);
	if (wpndamage > 0.f && wpnpos) {
		netbufWriteCoord(dst, wpnpos);
	}
	return dst->error;
}

u32 netmsgSvcChrDisarmRead(struct netbuf *src, struct netclient *srccl)
{
	struct prop *chrprop = netbufReadPropPtr(src);
	struct prop *aprop = netbufReadPropPtr(src);
	const u8 weaponnum = netbufReadU8(src);
	const f32 weapondmg = netbufReadF32(src);
	struct coord pos = { 0.f, 0.f, 0.f };

	if (src->error || srccl->state < CLSTATE_GAME) {
		return src->error;
	}

	if (!chrprop || !chrprop->chr) {
		return 1;
	}

	struct chrdata *chr = chrprop->chr;

	if (chrprop->type == PROPTYPE_CHR) {
		return src->error;
	}

	if (weapondmg > 0.f) {
		// someone shot a grenade the chr is holding, explode that shit
		netbufReadCoord(src, &pos);
		struct weaponobj *weapon = NULL;
		if (chr->weapons_held[0] && chr->weapons_held[0]->weapon) {
			weapon = chr->weapons_held[0]->weapon;
		} else if (chr->weapons_held[1] && chr->weapons_held[1]->weapon) {
			weapon = chr->weapons_held[1]->weapon;
		} else {
			sysLogPrintf(LOG_WARNING, "NET: trying to explode chr %u's gun, but there's no gun", chrprop->syncid);
			return src->error;
		}
		objSetDropped(chrprop, DROPTYPE_DEFAULT);
		chr->hidden |= CHRHFLAG_DROPPINGITEM;
		objDamage(&weapon->base, -weapondmg, &pos, weaponnum, g_Vars.currentplayernum);
		return src->error;
	}

	const s32 prevplayernum = g_Vars.currentplayernum;
	setCurrentPlayerNum(playermgrGetPlayerNumByProp(chrprop));

	struct player *player = g_Vars.currentplayer;

	if (weaponHasFlag(weaponnum, WEAPONFLAG_UNDROPPABLE) || weaponnum > WEAPON_RCP45 || weaponnum <= WEAPON_UNARMED) {
		setCurrentPlayerNum(prevplayernum);
		return src->error;
	}

	if (weaponnum == WEAPON_RCP120) {
		player->devicesactive &= ~DEVICE_CLOAKRCP120;
	}

	if (weaponnum == WEAPON_CLOAKINGDEVICE) {
		player->devicesactive &= ~DEVICE_CLOAKDEVICE;
	}

	weaponDeleteFromChr(chr, HAND_RIGHT);
	weaponDeleteFromChr(chr, HAND_LEFT);

	invRemoveItemByNum(weaponnum);

	player->hands[1].state = HANDSTATE_IDLE;
	player->hands[1].ejectstate = EJECTSTATE_INIT;
	player->hands[1].ejecttype = EJECTTYPE_GUN;
	player->hands[0].ejectstate = EJECTSTATE_INIT;
	player->hands[0].ejecttype = EJECTTYPE_GUN;
	player->hands[0].state = HANDSTATE_IDLE;

	if (player->visionmode == VISIONMODE_SLAYERROCKET) {
		struct weaponobj *rocket = g_Vars.currentplayer->slayerrocket;
		if (rocket && rocket->base.prop) {
			rocket->timer240 = 0;
		}
		player->visionmode = VISIONMODE_NORMAL;
	}

	bgunEquipWeapon2(HAND_RIGHT, WEAPON_UNARMED);
	bgunEquipWeapon2(HAND_LEFT, WEAPON_NONE);

	setCurrentPlayerNum(prevplayernum);

	return src->error;
}

// Sim chr fired its weapon. The client doesn't run sim AI (chrTick is gated in
// prop.c) so it never plays the local shoot sound or muzzle-flash. The server
// emits one of these on every sim shot so clients can play a positional sound
// and flag the chr as gunfire-visible for the matching held weapon prop.
u32 netmsgSvcChrFireWrite(struct netbuf *dst, struct chrdata *chr, u8 handnum, u16 soundnum)
{
	if (!chr || !chr->prop || !chr->prop->syncid) {
		return dst->error;
	}
	netbufWriteU8(dst, SVC_CHR_FIRE);
	netbufWritePropPtr(dst, chr->prop);
	netbufWriteU8(dst, handnum);
	netbufWriteU16(dst, soundnum);
	return dst->error;
}

u32 netmsgSvcChrFireRead(struct netbuf *src, struct netclient *srccl)
{
	struct prop *chrprop = netbufReadPropPtr(src);
	const u8 handnum = netbufReadU8(src);
	const u16 soundnum = netbufReadU16(src);

	if (src->error || srccl->state < CLSTATE_GAME) {
		return src->error;
	}

	if (!chrprop || !chrprop->chr) {
		return src->error;
	}

	// Positional shoot sound at the chr's current world position. Using
	// PSTYPE_CHRSHOOT so the channel manager evicts the chr's prior shoot
	// channel when they fire again, matching server-side behavior.
	if (soundnum) {
		psCreate(NULL, chrprop, soundnum, -1, -1, PSFLAG_0400, PSFLAG2_PRINTABLE, PSTYPE_CHRSHOOT,
			NULL, -1.f, NULL, -1, -1.f, -1.f, -1.f);
	}

	// Brief muzzle-flash hint: enable gunfire-visible on the chr's currently
	// held weapon for this hand. The state will be reset on the next fire
	// event or when the chr stops firing on the server (sent as soundnum=0).
	// Guard handnum (untrusted u8 from wire) against the chr->weapons_held[3]
	// bound before calling chrGetHeldProp, and check weaponprop->obj since
	// weaponSetGunfireVisible derefs it without its own null guard.
	if (handnum < 2) {
		struct prop *weaponprop = chrGetHeldProp(chrprop->chr, handnum);
		if (weaponprop && weaponprop->obj) {
			weaponSetGunfireVisible(weaponprop, soundnum != 0, chrprop->rooms[0]);
		}
	}

	return src->error;
}

// Kill feed entry. The server ships shooter + victim names from its
// g_MpAllChrConfigPtrs lookups directly, avoiding any client-vs-server
// mp-chr index mapping drift (netPlayersAllocate swaps the local client to
// slot 0 on the client, while the server keeps the original slot numbering).
// Two strings on the wire so the client renderer can colour each side
// independently (shooter green, victim red). An empty shooter string means
// the victim died alone (suicide / environment). Reliable channel so kills
// can't get dropped under packet loss.
u32 netmsgSvcKillWrite(struct netbuf *dst, const char *shooter, const char *victim, u8 shooter_team, u8 victim_team)
{
	netbufWriteU8(dst, SVC_KILL);
	netbufWriteStr(dst, shooter ? shooter : "");
	netbufWriteStr(dst, victim ? victim : "");
	// Team bytes let the receiver render names in team colours via
	// g_TeamColours. 0xff = "unknown team" (env death, no shooter, etc.) —
	// receiver falls back to the generic green/red palette in that case.
	netbufWriteU8(dst, shooter_team);
	netbufWriteU8(dst, victim_team);
	return dst->error;
}

u32 netmsgSvcKillRead(struct netbuf *src, struct netclient *srccl)
{
	const char *shooter = netbufReadStr(src);
	const char *victim = netbufReadStr(src);
	const u8 shooter_team = netbufReadU8(src);
	const u8 victim_team = netbufReadU8(src);
	if (src->error || srccl->state < CLSTATE_GAME) {
		return src->error;
	}
	if (victim && victim[0]) {
		netKillFeedAdd(shooter, victim, shooter_team, victim_team);
	}
	return src->error;
}

// Server-authoritative scoreboard update. The host owns the kill/death
// counters in g_MpAllChrConfigPtrs (g_PlayerConfigsArray for human slots,
// g_BotConfigsArray for sims). Clients used to compute their own from the
// chrDamage relay, which drifted under packet loss / timing skew because
// each mpstats increment ran independently on each side. Now the server
// gates the writes (see mpstatsRecordDeath ifdef) and ships authoritative
// deltas via this message — typically just the attacker and victim entries
// per kill, but the writer accepts a list so we can do bulk syncs on
// stage start / late-join in the future.
// Helper: find the netclient ID whose server-side playernum equals `slot`.
// Returns -1 if no netclient claims that slot. On the server side cl->playernum
// is sequentially assigned == cl->id, but we look it up properly so this works
// even if the assignment changes in the future.
static s32 netScoreNetIdForSlot(s32 slot)
{
	for (s32 i = 0; i < g_NetMaxClients; ++i) {
		const struct netclient *cl = &g_NetClients[i];
		if (cl->state >= CLSTATE_GAME && cl->playernum == slot) {
			return (s32)cl->id;
		}
	}
	return -1;
}

u32 netmsgSvcScoreWrite(struct netbuf *dst, const s32 *mpchrindexes, s32 count)
{
	// Wire schema: each entry's idx and the per-entry killcounts[0..MAX_PLAYERS-1]
	// are addressed by NETCLIENT ID for human slots, not by server-side mpchr
	// index. That decouples the wire format from netPlayersAllocate's local-only
	// slot swap (see comment near the swap in net.c): on a client the same
	// netclient may live at a different g_Vars.players[] slot than on the
	// server, so a raw mpchr index would land on the wrong row of the local
	// scoreboard. Bot slots (mpchr index >= MAX_PLAYERS) are deterministic on
	// both sides via shared RNG + setup, so those keep their direct mpchr index.
	if (count <= 0 || !mpchrindexes) {
		return dst->error;
	}
	if (count > MAX_MPCHRS) {
		count = MAX_MPCHRS;
	}
	netbufWriteU8(dst, SVC_SCORE);
	netbufWriteU8(dst, (u8)count);
	for (s32 i = 0; i < count; ++i) {
		const s32 idx = mpchrindexes[i];
		if (idx < 0 || idx >= MAX_MPCHRS || !g_MpAllChrConfigPtrs[idx]) {
			// write a sentinel so the reader can skip — keep the message
			// alignment regardless of which slots are populated.
			netbufWriteU8(dst, 0xff);
			netbufWriteS16(dst, 0);
			netbufWriteS16(dst, 0);
			netbufWriteS8(dst, 0);
			netbufWriteS32(dst, 0);
			for (s32 k = 0; k < MAX_MPCHRS; ++k) {
				netbufWriteS16(dst, 0);
			}
			continue;
		}
		const struct mpchrconfig *mpchr = g_MpAllChrConfigPtrs[idx];
		// Translate entry idx: humans → netclient ID (so the receiver can map
		// to its local slot); bots → mpchr index unchanged.
		u8 wireidx;
		if (idx < MAX_PLAYERS) {
			s32 cl_id = netScoreNetIdForSlot(idx);
			wireidx = (cl_id >= 0 && cl_id < MAX_PLAYERS) ? (u8)cl_id : 0xff;
		} else {
			wireidx = (u8)idx;
		}
		netbufWriteU8(dst, wireidx);
		netbufWriteS16(dst, mpchr->numdeaths);
		netbufWriteS16(dst, mpchr->numpoints);
		netbufWriteS8(dst, mpchr->placement);
		netbufWriteS32(dst, mpchr->rankablescore);
		// Killcounts: write [0..MAX_PLAYERS-1] in netclient ID order — entry k
		// is "kills against netclient k". The receiver translates back through
		// its own netclient → slot map. Bot entries follow at the same mpchr
		// index on both sides.
		for (s32 k = 0; k < MAX_PLAYERS; ++k) {
			const struct netclient *cl = &g_NetClients[k];
			if (cl->state >= CLSTATE_GAME && cl->playernum >= 0 && cl->playernum < MAX_MPCHRS) {
				netbufWriteS16(dst, mpchr->killcounts[cl->playernum]);
			} else {
				netbufWriteS16(dst, 0);
			}
		}
		for (s32 k = MAX_PLAYERS; k < MAX_MPCHRS; ++k) {
			netbufWriteS16(dst, mpchr->killcounts[k]);
		}
	}
	return dst->error;
}

u32 netmsgSvcScoreRead(struct netbuf *src, struct netclient *srccl)
{
	const u8 count = netbufReadU8(src);
	if (src->error || srccl->state < CLSTATE_GAME) {
		return src->error;
	}
	if (count > MAX_MPCHRS) {
		// Malformed — bail rather than overrun the wire.
		return 1;
	}
	for (s32 i = 0; i < count; ++i) {
		const u8 wireidx = netbufReadU8(src);
		const s16 numdeaths = netbufReadS16(src);
		const s16 numpoints = netbufReadS16(src);
		const s8 placement = netbufReadS8(src);
		const s32 rankablescore = netbufReadS32(src);
		// Killcounts arrive with positions 0..MAX_PLAYERS-1 indexed by
		// netclient ID, MAX_PLAYERS..MAX_MPCHRS-1 indexed by mpchr index
		// (bots). Translate the human slice to LOCAL mpchr slots before
		// applying — see SvcScoreWrite for the rationale.
		s16 wire_killcounts[MAX_MPCHRS];
		for (s32 k = 0; k < MAX_MPCHRS; ++k) {
			wire_killcounts[k] = netbufReadS16(src);
		}
		if (src->error) {
			return src->error;
		}
		// 0xff sentinel = server skipped this entry.
		if (wireidx == 0xff) {
			continue;
		}
		// Translate wireidx: humans → look up the netclient and use its
		// LOCAL playernum; bots → mpchr index directly.
		s32 local_idx;
		if (wireidx < MAX_PLAYERS) {
			const struct netclient *cl = &g_NetClients[wireidx];
			if (cl->state < CLSTATE_GAME || cl->playernum < 0 || cl->playernum >= MAX_MPCHRS) {
				continue;
			}
			local_idx = cl->playernum;
		} else {
			local_idx = wireidx;
		}
		if (local_idx >= MAX_MPCHRS || !g_MpAllChrConfigPtrs[local_idx]) {
			continue;
		}
		struct mpchrconfig *mpchr = g_MpAllChrConfigPtrs[local_idx];
		mpchr->numdeaths = numdeaths;
		mpchr->numpoints = numpoints;
		mpchr->placement = placement;
		mpchr->rankablescore = rankablescore;
		// Map wire killcounts back to local mpchr index. Human positions
		// (0..MAX_PLAYERS-1) are keyed by netclient ID; each maps to the
		// LOCAL slot via g_NetClients[k].playernum. Bot positions index
		// directly.
		for (s32 k = 0; k < MAX_PLAYERS; ++k) {
			const struct netclient *cl = &g_NetClients[k];
			if (cl->state >= CLSTATE_GAME && cl->playernum >= 0 && cl->playernum < MAX_MPCHRS) {
				mpchr->killcounts[cl->playernum] = wire_killcounts[k];
			}
		}
		for (s32 k = MAX_PLAYERS; k < MAX_MPCHRS; ++k) {
			mpchr->killcounts[k] = wire_killcounts[k];
		}
	}
	return src->error;
}

// SVC_KOH_STATE: server broadcasts the authoritative King of the Hill state so
// clients track the same hill position, occupying team, and timer. Sent on
// every hill change and periodically as a keep-alive. The color-tween fracs
// are NOT sent — clients derive them locally from occupiedteam, which is
// enough for them to converge on the right room tint.
u32 netmsgSvcKohStateWrite(struct netbuf *dst)
{
	const struct scenariodata_koh *koh = &g_ScenarioData.koh;
	netbufWriteU8(dst, SVC_KOH_STATE);
	netbufWriteS16(dst, koh->hillindex);
	netbufWriteS16(dst, koh->hillrooms[0]);
	netbufWriteS16(dst, koh->hillrooms[1]);
	netbufWriteCoord(dst, &koh->hillpos);
	netbufWriteU8(dst, koh->movehill ? 1 : 0);
	netbufWriteS16(dst, koh->occupiedteam);
	netbufWriteS16(dst, koh->elapsed240);
	return dst->error;
}

u32 netmsgSvcKohStateRead(struct netbuf *src, struct netclient *srccl)
{
	const s16 hillindex  = netbufReadS16(src);
	const s16 hillroom0  = netbufReadS16(src);
	const s16 hillroom1  = netbufReadS16(src);
	struct coord hillpos;
	netbufReadCoord(src, &hillpos);
	const u8 movehill      = netbufReadU8(src);
	const s16 occupiedteam = netbufReadS16(src);
	const s16 elapsed240   = netbufReadS16(src);

	if (src->error || srccl->state < CLSTATE_GAME) {
		return src->error;
	}
	if (g_MpSetup.scenario != MPSCENARIO_KINGOFTHEHILL) {
		return 0;
	}

	struct scenariodata_koh *koh = &g_ScenarioData.koh;

	// Only touch room lighting when the hill position actually changes.
	// During a movehill transition the light ops are handled by kohTick's
	// color-fade path; we only apply HIGHLIGHT when the new hill lands.
	const s16 old_room = koh->hillrooms[0];
	const bool room_changed = (hillroom0 != old_room);

	koh->hillindex    = hillindex;
	koh->hillrooms[0] = hillroom0;
	koh->hillrooms[1] = hillroom1;
	koh->hillpos      = hillpos;
	koh->movehill     = movehill;
	koh->occupiedteam = occupiedteam;
	koh->elapsed240   = elapsed240;

	if (!movehill && room_changed) {
		// Old room reverts to natural; new room highlighted.
		if (old_room >= 0) {
			roomSetLightOp(old_room, LIGHTOP_NONE, 0, 0, 0);
		}
		if (hillroom0 >= 0) {
			roomSetLightOp(hillroom0, LIGHTOP_HIGHLIGHT, 0, 0, 0);
		}
	}

	return src->error;
}

// SVC_EXPLOSION: server notifies clients of an explosion visual at a world
// position. Used when a timer-detonated networked prop (phoenix secondary,
// grenade, etc.) explodes — the weapon's propExplode runs server-side only,
// so clients need an explicit event to spawn the local particle effect.
u32 netmsgSvcExplosionWrite(struct netbuf *dst, s32 exptype, const struct coord *pos, const RoomNum *rooms)
{
	netbufWriteU8(dst, SVC_EXPLOSION);
	netbufWriteS16(dst, (s16)exptype);
	netbufWriteCoord(dst, pos);
	netbufWriteS16(dst, rooms[0]);
	return dst->error;
}

u32 netmsgSvcExplosionRead(struct netbuf *src, struct netclient *srccl)
{
	const s16 exptype = netbufReadS16(src);
	struct coord pos;
	netbufReadCoord(src, &pos);
	const s16 room = netbufReadS16(src);

	if (src->error || srccl->state < CLSTATE_GAME) {
		return src->error;
	}

	RoomNum rooms[2] = { room, -1 };
	explosionCreateComplex(NULL, &pos, rooms, exptype, 0);
	return src->error;
}

// SVC_LOBBY_STATE: server periodically broadcasts current game setup to
// clients in CLSTATE_LOBBY so they can display live info while waiting.
// All display strings are pre-resolved here so the client render path is
// simple. Sent every ~60 ticks and when a new client enters the lobby.
u32 netmsgSvcLobbyStateWrite(struct netbuf *dst)
{
	static const char *const scenarioNames[] = {
		"Combat", "Hold the Briefcase", "Hacker Central",
		"Pop-A-Cap", "King of the Hill", "Capture the Case",
	};

	// Helper: copy src into buf (max len), strip embedded '\n' width markers.
	#define LOBBY_COPY(buf, src, len) \
		do { \
			strncpy((buf), (src) ? (src) : "", (len) - 1); \
			(buf)[(len) - 1] = '\0'; \
			for (s32 _i = 0; (buf)[_i]; _i++) { \
				if ((buf)[_i] == '\n') { (buf)[_i] = '\0'; break; } \
			} \
		} while (0)

	char sbuf[NET_LOBBY_ARENANAME_LEN];

	netbufWriteU8(dst, SVC_LOBBY_STATE);

	netbufWriteU8(dst, g_MpSetup.scenario);
	netbufWriteU8(dst, g_MpSetup.stagenum);
	netbufWriteU32(dst, g_MpSetup.options);
	netbufWriteU8(dst, g_MpSetup.scorelimit);
	netbufWriteU8(dst, g_MpSetup.timelimit);
	netbufWriteU16(dst, g_MpSetup.teamscorelimit);

	// Arena name: search g_MpArenas for the current stagenum
	const char *arena_raw = "?";
	for (s32 i = 0; i < 17; i++) {
		if (g_MpArenas[i].stagenum == g_MpSetup.stagenum) {
			arena_raw = langGet(g_MpArenas[i].name);
			break;
		}
	}
	LOBBY_COPY(sbuf, arena_raw, sizeof(sbuf));
	netbufWriteStr(dst, sbuf);

	// Scenario name
	const char *scen_raw = (g_MpSetup.scenario < (u8)ARRAYCOUNT(scenarioNames))
		? scenarioNames[g_MpSetup.scenario] : "?";
	netbufWriteStr(dst, scen_raw);

	// Weapon set name
	LOBBY_COPY(sbuf, mpGetWeaponSetName(mpGetWeaponSet()), sizeof(sbuf));
	netbufWriteStr(dst, sbuf);

	// Per-slot weapon names (6 slots)
	for (s32 i = 0; i < NUM_MPWEAPONSLOTS; i++) {
		char wbuf[NET_LOBBY_WPNNAME_LEN];
		LOBBY_COPY(wbuf, mpGetWeaponLabel(g_MpSetup.weapons[i]), sizeof(wbuf));
		netbufWriteStr(dst, wbuf);
	}

	// Connected clients: name, ping, team
	s32 numclients = 0;
	for (s32 i = 0; i < g_NetMaxClients; i++) {
		if (g_NetClients[i].state >= CLSTATE_LOBBY) { numclients++; }
	}
	netbufWriteU8(dst, (u8)numclients);
	for (s32 i = 0; i < g_NetMaxClients; i++) {
		const struct netclient *cl = &g_NetClients[i];
		if (cl->state < CLSTATE_LOBBY) { continue; }
		char nbuf[NET_MAX_NAME];
		LOBBY_COPY(nbuf, cl->settings.name, sizeof(nbuf));
		netbufWriteStr(dst, nbuf);
		netbufWriteU16(dst, (u16)(cl->peer ? enet_peer_get_rtt(cl->peer) : 0u));
		netbufWriteU8(dst, cl->settings.team);
		// Spectator flag (NET_PROTOCOL_VER >= 27). Lets the lobby UI on
		// remote clients tag the host as "(spectator)" before stage start.
		netbufWriteU8(dst, cl->is_spectator);
	}

	// Active bots: name, team, difficulty
	s32 numbots = 0;
	for (s32 i = 0; i < MAX_BOTS; i++) {
		if (g_BotConfigsArray[i].difficulty != BOTDIFF_DISABLED) { numbots++; }
	}
	netbufWriteU8(dst, (u8)numbots);
	for (s32 i = 0; i < MAX_BOTS; i++) {
		const struct mpbotconfig *bot = &g_BotConfigsArray[i];
		if (bot->difficulty == BOTDIFF_DISABLED) { continue; }
		char nbuf[NET_MAX_NAME];
		LOBBY_COPY(nbuf, bot->base.name, sizeof(nbuf));
		netbufWriteStr(dst, nbuf);
		netbufWriteU8(dst, bot->base.team);
		netbufWriteU8(dst, bot->difficulty);
	}

	// Team names (always sent; client renders them if MPOPTION_TEAMSENABLED)
	for (s32 i = 0; i < MAX_TEAMS; i++) {
		char tbuf[NET_LOBBY_TEAMNAME_LEN];
		LOBBY_COPY(tbuf, g_BossFile.teamnames[i], sizeof(tbuf));
		netbufWriteStr(dst, tbuf);
	}

	#undef LOBBY_COPY
	return dst->error;
}

u32 netmsgSvcLobbyStateRead(struct netbuf *src, struct netclient *srccl)
{
	const u8 scenario        = netbufReadU8(src);
	const u8 stagenum        = netbufReadU8(src);
	const u32 options        = netbufReadU32(src);
	const u8 scorelimit      = netbufReadU8(src);
	const u8 timelimit       = netbufReadU8(src);
	const u16 teamscorelimit = netbufReadU16(src);

	// Pre-resolved display strings
	const char *arena_name   = netbufReadStr(src);
	const char *scen_name    = netbufReadStr(src);
	const char *wpnset_name  = netbufReadStr(src);
	const char *wpn_names[NUM_MPWEAPONSLOTS];
	for (s32 i = 0; i < NUM_MPWEAPONSLOTS; i++) {
		wpn_names[i] = netbufReadStr(src);
	}

	// Clients
	const u8 num_clients = netbufReadU8(src);
	struct netlobbyclient clients[NET_MAX_CLIENTS];
	const u8 ncl = (num_clients < NET_MAX_CLIENTS) ? num_clients : NET_MAX_CLIENTS;
	for (s32 i = 0; i < ncl; i++) {
		const char *name = netbufReadStr(src);
		const u16 ping   = netbufReadU16(src);
		const u8 team    = netbufReadU8(src);
		const u8 spec    = netbufReadU8(src);
		strncpy(clients[i].name, name ? name : "", NET_MAX_NAME - 1);
		clients[i].name[NET_MAX_NAME - 1] = '\0';
		clients[i].ping = ping;
		clients[i].team = team;
		clients[i].is_spectator = spec;
	}

	// Bots
	const u8 num_bots = netbufReadU8(src);
	struct netlobbybot bots[MAX_BOTS];
	const u8 nbt = (num_bots < MAX_BOTS) ? num_bots : MAX_BOTS;
	for (s32 i = 0; i < nbt; i++) {
		const char *name = netbufReadStr(src);
		const u8 team    = netbufReadU8(src);
		const u8 diff    = netbufReadU8(src);
		strncpy(bots[i].name, name ? name : "", NET_MAX_NAME - 1);
		bots[i].name[NET_MAX_NAME - 1] = '\0';
		bots[i].team       = team;
		bots[i].difficulty = diff;
	}

	// Team names
	char teamnames[MAX_TEAMS][NET_LOBBY_TEAMNAME_LEN];
	for (s32 i = 0; i < MAX_TEAMS; i++) {
		const char *name = netbufReadStr(src);
		strncpy(teamnames[i], name ? name : "", NET_LOBBY_TEAMNAME_LEN - 1);
		teamnames[i][NET_LOBBY_TEAMNAME_LEN - 1] = '\0';
	}

	if (src->error) {
		return src->error;
	}
	// Discard stale packets that arrive after the game has already started.
	if (srccl->state != CLSTATE_LOBBY) {
		return 0;
	}

	g_NetLobbyState.valid          = 1;
	g_NetLobbyState.scenario       = scenario;
	g_NetLobbyState.stagenum       = stagenum;
	g_NetLobbyState.options        = options;
	g_NetLobbyState.scorelimit     = scorelimit;
	g_NetLobbyState.timelimit      = timelimit;
	g_NetLobbyState.teamscorelimit = teamscorelimit;

	strncpy(g_NetLobbyState.arena_name, arena_name ? arena_name : "?",
		NET_LOBBY_ARENANAME_LEN - 1);
	g_NetLobbyState.arena_name[NET_LOBBY_ARENANAME_LEN - 1] = '\0';

	strncpy(g_NetLobbyState.scenario_name, scen_name ? scen_name : "?",
		NET_LOBBY_SCENNAME_LEN - 1);
	g_NetLobbyState.scenario_name[NET_LOBBY_SCENNAME_LEN - 1] = '\0';

	strncpy(g_NetLobbyState.weaponset_name, wpnset_name ? wpnset_name : "?",
		NET_LOBBY_WPNSETNAME_LEN - 1);
	g_NetLobbyState.weaponset_name[NET_LOBBY_WPNSETNAME_LEN - 1] = '\0';

	for (s32 i = 0; i < NUM_MPWEAPONSLOTS; i++) {
		strncpy(g_NetLobbyState.weapon_names[i], wpn_names[i] ? wpn_names[i] : "?",
			NET_LOBBY_WPNNAME_LEN - 1);
		g_NetLobbyState.weapon_names[i][NET_LOBBY_WPNNAME_LEN - 1] = '\0';
	}

	g_NetLobbyState.num_clients = ncl;
	for (s32 i = 0; i < ncl; i++) {
		g_NetLobbyState.clients[i] = clients[i];
	}

	g_NetLobbyState.num_bots = nbt;
	for (s32 i = 0; i < nbt; i++) {
		g_NetLobbyState.bots[i] = bots[i];
	}

	for (s32 i = 0; i < MAX_TEAMS; i++) {
		memcpy(g_NetLobbyState.teamnames[i], teamnames[i], NET_LOBBY_TEAMNAME_LEN);
	}

	return 0;
}

// ---------- Vote messages (port-only) ----------

u32 netmsgSvcVoteOpenWrite(struct netbuf *dst)
{
	netbufWriteU8(dst, SVC_VOTE_OPEN);
	netbufWriteU8(dst, g_NetVote.num_candidates);
	netbufWriteU8(dst, g_NetVote.vote_seconds);
	for (s32 i = 0; i < g_NetVote.num_candidates; ++i) {
		const struct netvotecandidate *c = &g_NetVote.candidates[i];
		// playlist_index: -1 sentinel marshals to 0xFF on the wire
		netbufWriteU8(dst, (u8)(c->playlist_index < 0 ? 0xFFu : (u8)c->playlist_index));
		netbufWriteU8(dst, c->stagenum);
		netbufWriteU8(dst, c->scenario);
		netbufWriteU8(dst, c->preset_index);
		netbufWriteU8(dst, c->bot_count);
		netbufWriteU8(dst, c->timelimit);
		netbufWriteU8(dst, c->scorelimit);
		netbufWriteStr(dst, c->name);
	}
	return dst->error;
}

u32 netmsgSvcVoteOpenRead(struct netbuf *src, struct netclient *srccl)
{
	(void)srccl;
	const u8 num = netbufReadU8(src);
	const u8 secs = netbufReadU8(src);
	if (src->error || num > NET_VOTE_MAX_CANDIDATES) {
		return src->error;
	}
	g_NetVote.state = NETVOTE_OPEN;
	g_NetVote.num_candidates = num;
	g_NetVote.vote_seconds = secs;
	g_NetVote.winning_index = 0;
	g_NetVote.winner_was_random = 0;
	for (s32 i = 0; i < num; ++i) {
		struct netvotecandidate *c = &g_NetVote.candidates[i];
		const u8 pl_idx = netbufReadU8(src);
		c->playlist_index = (pl_idx == 0xFF) ? -1 : (s8)pl_idx;
		c->stagenum = netbufReadU8(src);
		c->scenario = netbufReadU8(src);
		c->preset_index = netbufReadU8(src);
		c->bot_count = netbufReadU8(src);
		c->timelimit = netbufReadU8(src);
		c->scorelimit = netbufReadU8(src);
		const char *nm = netbufReadStr(src);
		strncpy(c->name, nm ? nm : "?", sizeof(c->name) - 1);
		c->name[sizeof(c->name) - 1] = '\0';
		g_NetVote.tally[i] = 0;
	}

	sysLogPrintf(LOG_CHAT, "VOTE: %d candidates, %d seconds. Vote with /vote N:", (s32)num, (s32)secs);
	for (s32 i = 0; i < num; ++i) {
		sysLogPrintf(LOG_CHAT, "  [%d] %s", i, g_NetVote.candidates[i].name);
	}

	return src->error;
}

u32 netmsgSvcVoteResultsWrite(struct netbuf *dst)
{
	netbufWriteU8(dst, SVC_VOTE_RESULTS);
	netbufWriteU8(dst, g_NetVote.winning_index);
	netbufWriteU8(dst, g_NetVote.winner_was_random);
	netbufWriteU8(dst, g_NetVote.num_candidates);
	for (s32 i = 0; i < g_NetVote.num_candidates; ++i) {
		netbufWriteU8(dst, g_NetVote.tally[i]);
	}
	return dst->error;
}

u32 netmsgSvcVoteResultsRead(struct netbuf *src, struct netclient *srccl)
{
	(void)srccl;
	const u8 winning = netbufReadU8(src);
	const u8 was_random = netbufReadU8(src);
	const u8 count = netbufReadU8(src);
	if (src->error || count > NET_VOTE_MAX_CANDIDATES) {
		return src->error;
	}
	g_NetVote.state = NETVOTE_RESULTS;
	g_NetVote.winning_index = winning;
	g_NetVote.winner_was_random = was_random;
	for (s32 i = 0; i < count; ++i) {
		g_NetVote.tally[i] = netbufReadU8(src);
	}

	if (winning < count) {
		const char *wname = g_NetVote.candidates[winning].name;
		sysLogPrintf(LOG_CHAT, "VOTE: winner = [%d] %s%s (%d votes)",
				(s32)winning, wname,
				was_random ? " (RANDOM)" : "",
				(s32)g_NetVote.tally[winning]);
	}

	return src->error;
}

u32 netmsgClcVoteWrite(struct netbuf *dst, u8 candidate_index)
{
	netbufWriteU8(dst, CLC_VOTE);
	netbufWriteU8(dst, candidate_index);
	return dst->error;
}

u32 netmsgClcVoteRead(struct netbuf *src, struct netclient *srccl)
{
	const u8 idx = netbufReadU8(src);
	if (src->error) return src->error;
	netServerVoteRecord(srccl, idx);
	return 0;
}
