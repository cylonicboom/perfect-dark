// Killcam (MPOPTION_KILLCAM) — port-only, client-side, cosmetic.
//
// While the option is on, every tick records all MP combatants' visual poses
// into a 4-second ring (NET_KILLCAM_TICKS). When the local pawn dies, it replays
// that window from the killer's point of view (chr->lastattacker), reusing the
// existing spectate machinery (g_NetSpectateChr drives the lvRender player
// redirect for a human killer and netSpectateApply's eye-camera for a sim).
//
// Playback is non-destructive: each replay-render the live combatant poses are
// saved, the recorded frame is applied, the view is rendered, then the live
// poses are restored — so the authoritative sim/tick is never disturbed (the
// netLagCompBegin/End save-restore idiom, generalised to full pose).
//
// No wire format change: this records data the client already receives. Old
// peers are unaffected; the toggle just rides g_MpSetup.options.

#include <string.h>
#include <math.h>
#include "types.h"
#include "constants.h"
#include "bss.h"
#include "data.h"
#include "game/player.h"
#include "game/playermgr.h"
#include "game/chr.h"
#include "game/chraction.h"
#include "game/prop.h"
#include "game/propobj.h"
#include "lib/model.h"
#include "net/net.h"
#include "system.h"

struct netkillcamstate g_NetKillcam;
static struct netkillcamframe g_NetKillcamRing[NET_KILLCAM_TICKS];
// chrflags saved across a replay-render bracket (so we can un-hide dead/corpsed
// combatants — including the victim — for the replay, then restore live state).
static u32 g_NetKillcamSavedFlags[MAX_MPCHRS];
// One replay per death: set when a replay starts, cleared on respawn (the
// dead->alive edge). Without it the self/lastattacker fallback would re-trigger
// the moment the previous replay ends while the pawn is still dead.
static bool g_NetKillcamPlayedThisDeath = false;

// The local player whose death triggers the killcam: the net local client's
// pawn, else the solo / listen-host primary (slot 0).
static struct player *netKillcamLocalPlayer(void)
{
	// Prefer the net local client's own pawn in EVERY netmode. On a listen host
	// with spectator mode enabled, spectatorAllocatePanels seats combatants at
	// the low g_Vars.players[] slots and the host's panels at the high ones, so
	// players[0] is a REMOTE client's pawn — the host would latch that client's
	// death as its own, freeze recording, and run the replay rewind (below) on
	// the authoritative world. A dedicated/spectator host has no pawn at all,
	// which is a NULL return and no killcam, as intended.
	if (g_NetMode != NETMODE_NONE) {
		return g_NetLocalClient ? g_NetLocalClient->player : NULL;
	}

	// Offline: slot 0 is the primary local player.
	return g_Vars.players[0];
}

// Capture a chr's current visual pose into an entry (mirrors the chr-state
// fields netmsgSvcPropMoveWrite ships). Used both for recording and for saving
// the live pose before a replay-render.
static void netKillcamCapture(struct chrdata *chr, struct netkillcamentry *e)
{
	struct prop *prop = chr->prop;
	e->pose.pos = prop->pos;
	e->pose.yrot = chr->model ? modelGetChrRotY(chr->model) : chrGetRotY(chr);
	e->pose.angleoffset = chr->aibot ? chr->aibot->angleoffset : 0.f;
	e->pose.aimupback = chr->aimupback;
	e->pose.aimsideback = chr->aimsideback;
	e->pose.aimuplshoulder = chr->aimuplshoulder;
	e->pose.aimuprshoulder = chr->aimuprshoulder;
	if (chr->model && chr->model->anim) {
		e->pose.animnum = chr->model->anim->animnum;
		e->pose.framea = chr->model->anim->framea;
		e->pose.speed = chr->model->anim->speed;
	} else {
		e->pose.animnum = 0;
		e->pose.framea = 0;
		e->pose.speed = 1.f;
	}
	for (s32 ri = 0; ri < 8; ri++) {
		e->pose.rooms[ri] = prop->rooms[ri];
	}
	e->gunfire = (u8)((chrIsGunfireVisible(chr, HAND_RIGHT) ? 1 : 0)
			| (chrIsGunfireVisible(chr, HAND_LEFT) ? 2 : 0));
	for (s32 h = 0; h < 2; h++) {
		e->heldweapon[h] = (chr->weapons_held[h] && chr->weapons_held[h]->weapon)
				? (s16)chr->weapons_held[h]->weapon->weaponnum : -1;
		e->clipammo[h] = -1; // unknown by default (remote players / sims)
	}

	const s32 pnum = playermgrGetPlayerNumByProp(prop);
	if (pnum >= 0 && g_Vars.players[pnum]) {
		struct player *pl = g_Vars.players[pnum];
		e->campos = pl->cam_pos;
		e->theta = pl->vv_theta;
		e->verta = pl->vv_verta;
		// Final camera basis — replayed verbatim so the view matches the killer's
		// exact historical aim (no angle re-derivation / convention mismatch).
		e->camlook = pl->cam_look;
		e->camup = pl->cam_up;
		e->camroom = pl->cam_room;
		e->haspcam = 1;
		// Only a NON-remote local player has a freshly-computed camera; a remote
		// player's cam_pos is stale on this machine (see the demo player).
		e->islocalplayer = pl->isremote ? 0 : 1;
		// Clip ammo is only knowable for a NON-remote local player (the recorder's
		// own gunctrl); record it so the demo shows the real HUD count + reloads on
		// the followed view.
		if (!pl->isremote) {
			e->clipammo[HAND_RIGHT] = (s16)pl->hands[HAND_RIGHT].loadedammo[0];
			e->clipammo[HAND_LEFT] = (s16)pl->hands[HAND_LEFT].loadedammo[0];
		}
	} else {
		e->campos = prop->pos;
		e->theta = 0.f;
		e->verta = 0.f;
		e->camlook = prop->pos;
		e->camup = prop->pos;
		e->camroom = prop->rooms[0];
		e->haspcam = 0;
		e->islocalplayer = 0;
	}
	e->valid = 1;
}

// Apply a recorded entry back onto a live chr (the interp-apply ops). Also used
// to restore the saved live pose after a replay-render.
static void netKillcamApply(struct chrdata *chr, const struct netkillcamentry *e)
{
	struct prop *prop = chr->prop;
	prop->pos = e->pose.pos;
	if (chr->model) {
		modelSetRootPosition(chr->model, (struct coord *)&e->pose.pos);
		modelSetChrRotY(chr->model, e->pose.yrot);
		if (chr->model->anim) {
			if (chr->model->anim->animnum != e->pose.animnum && e->pose.animnum) {
				modelSetAnimation(chr->model, e->pose.animnum,
						chr->model->anim->flip, e->pose.framea, e->pose.speed, 16.0f);
			} else {
				chr->model->anim->speed = e->pose.speed;
			}
		}
	}
	chrSetRotY(chr, e->pose.yrot);
	chr->aimupback = e->pose.aimupback;
	chr->aimsideback = e->pose.aimsideback;
	chr->aimuplshoulder = e->pose.aimuplshoulder;
	chr->aimuprshoulder = e->pose.aimuprshoulder;
	if (chr->aibot) {
		chr->aibot->angleoffset = e->pose.angleoffset;
	}

	if (prop->active) {
		propDeregisterRooms(prop);
	}
	roomsCopy((RoomNum *)e->pose.rooms, prop->rooms);
	if (prop->active) {
		propRegisterRooms(prop);
	}
	// NOTE: the replay CAMERA is NOT applied here. It's driven on the local
	// player's own viewport by netSpectateApply -> netKillcamGetCamera (which
	// reads the killer's recorded eye/look/up straight from the ring). Writing a
	// player's cam_pos here would clobber that — and we only stored cam_pos, not
	// the full basis, so it'd produce a mixed/broken view. Bodies only.
}

// Public wrapper over the (static) interp-apply primitive — used by the demo
// player (port/src/demo.c) to puppet combatants from a recorded frame, the same
// way the killcam replay-render does. Bodies only (no camera); see netKillcamApply.
void netKillcamApplyEntry(struct chrdata *chr, const struct netkillcamentry *e)
{
	netKillcamApply(chr, e);
}

void netKillcamReset(void)
{
	memset(&g_NetKillcam, 0, sizeof(g_NetKillcam));
	memset(g_NetKillcamRing, 0, sizeof(g_NetKillcamRing));
	g_NetKillcam.killerchrindex = -1;
	g_NetKillcam.pendingkiller = -1;
	g_NetKillcamPlayedThisDeath = false;
}

s32 netKillcamActive(void)
{
	return g_NetKillcam.active;
}

void netKillcamStop(void)
{
	if (g_NetKillcam.active) {
		g_NetSpectateChr = NULL;
	}
	g_NetKillcam.active = 0;
	g_NetKillcam.killerchrindex = -1;
}

// Called once per tick (lvTick). Detects the local pawn's death edge to trigger
// the replay (works in net + solo via netKillcamLocalPlayer), then records the
// current world while alive. Runs before netSpectateAutoUpdate so a triggered
// killcam owns g_NetSpectateChr and the spectate-on-death check (which only
// engages when g_NetSpectateChr is NULL) stands down.
void netKillcamRecordTick(void)
{
	static bool s_wasdead = false;

	if (!g_Vars.mplayerisrunning || !(g_MpSetup.options & MPOPTION_KILLCAM)
			|| g_NetDedicatedMode || netKillcamLocalPlayer() == NULL) {
		s_wasdead = false;
		netKillcamStop();
		return;
	}

	// Ticks spent dead-but-still-recording (the post-death "story" tail). Reset on
	// respawn so each death captures its own tail.
	static s32 s_postdeath = 0;

	struct player *lp = netKillcamLocalPlayer();
	const bool dead = (lp->isdead != 0);
	if (!dead && s_wasdead) {
		netKillcamStop();              // respawned: end any replay
		g_NetKillcam.pendingkiller = -1;
		g_NetKillcamPlayedThisDeath = false; // re-arm for the next death
		s_postdeath = 0;
	}
	s_wasdead = dead;

	if (g_NetKillcam.active) {
		return; // frozen while replaying
	}

	// Record at the LOGICAL 60Hz tick rate, not the render rate. lvTick (our
	// caller) runs once per RENDER frame, so at high/VRR fps this function fires
	// 2x+ per logical tick — which doubled the per-frame capture cost (the
	// "stutter over time" report), filled the 5s ring in 2.5s, and halved the
	// post-death window. Gate the post-death advance + capture on lvframe60
	// actually moving so behaviour is framerate-independent. (Death-edge detection
	// and respawn handling above still run every call — they're cheap + idempotent.)
	static u32 s_lastrecframe = 0xffffffffu;
	const u32 nowframe = (u32)g_Vars.lvframe60;
	if (nowframe == s_lastrecframe) {
		return; // already sampled this logical tick
	}
	s_lastrecframe = nowframe;

	if (dead && !g_NetKillcamPlayedThisDeath) {
		// Keep recording for a short window AFTER death so the replay can run a
		// couple of seconds past the kill (the killer's reaction, the body
		// dropping). Once the tail is captured, begin the replay; until then fall
		// through and record this (corpse + killer) frame into the ring.
		if (s_postdeath >= NET_KILLCAM_POSTDEATH) {
			netKillcamOnLocalDeath(); // retries each tick (late SVC_KILL latch)
			return;
		}
		s_postdeath++;
	} else if (dead) {
		return; // already played this death — idle until respawn
	}

	g_NetKillcam.head = (g_NetKillcam.head + 1) % NET_KILLCAM_TICKS;
	struct netkillcamframe *f = &g_NetKillcamRing[g_NetKillcam.head];
	netKillcamCaptureLiveFrame(f);

	if (g_NetKillcam.count < NET_KILLCAM_TICKS) {
		g_NetKillcam.count++;
	}
}

// Capture the current live world (all MP combatants' poses + cameras) into a
// frame. Shared by the killcam ring recorder above and the demo recorder
// (port/src/demo.c) — the single Phase-1/Phase-2 capture primitive.
void netKillcamCaptureLiveFrame(struct netkillcamframe *f)
{
	f->frame = g_Vars.lvframe60 ? (u32)g_Vars.lvframe60 : 1u;

	for (s32 i = 0; i < MAX_MPCHRS; i++) {
		struct chrdata *chr = g_MpAllChrPtrs[i];
		if (chr && chr->prop && chr->model) {
			netKillcamCapture(chr, &f->ents[i]);
		} else {
			f->ents[i].valid = 0;
		}
	}
}

// Resolve a combatant chr from its config name (g_MpAllChrConfigPtrs[]->name is
// identical on every machine — unlike slot indices — so this is a reliable
// cross-machine identity for the SVC_KILL names).
struct chrdata *netKillcamFindChrByName(const char *name)
{
	if (!name || !name[0]) {
		return NULL;
	}
	for (s32 i = 0; i < MAX_MPCHRS; i++) {
		if (g_MpAllChrPtrs[i] && g_MpAllChrConfigPtrs[i]
				&& strcmp(g_MpAllChrConfigPtrs[i]->name, name) == 0) {
			return g_MpAllChrPtrs[i];
		}
	}
	return NULL;
}

// Latch the killer of the LOCAL pawn (the reliable trigger source: SVC_KILL on a
// client, mpstatsRecordDeath on solo/host). chr->lastattacker is NOT set on the
// victim's own client, so the killcam couldn't trigger before. The dead-edge
// (netKillcamRecordTick) consumes this latch.
void netKillcamNoteKill(struct chrdata *killer, struct chrdata *victim)
{
	if (!(g_MpSetup.options & MPOPTION_KILLCAM)) {
		return; // option off — silent (record path logs the gate)
	}
	if (!killer || !victim) {
		sysLogPrintf(LOG_NOTE, "killcam: notekill unresolved (killer=%p victim=%p)", killer, victim);
		return;
	}
	if (killer == victim) {
		return; // suicide / environment — no killer POV
	}
	struct player *lp = netKillcamLocalPlayer();
	if (!lp || !lp->prop || lp->prop->chr != victim) {
		return; // not the local pawn's death (kills of others fire on their machines)
	}
	for (s32 i = 0; i < MAX_MPCHRS; i++) {
		if (g_MpAllChrPtrs[i] == killer) {
			g_NetKillcam.pendingkiller = i;
			sysLogPrintf(LOG_NOTE, "killcam: kill latched (killer idx=%d, recorded=%u)",
					i, g_NetKillcam.count);
			return;
		}
	}
	sysLogPrintf(LOG_NOTE, "killcam: kill of local pawn but killer chr not in g_MpAllChrPtrs");
}

void netKillcamOnLocalDeath(void)
{
	if (g_NetKillcam.active || g_NetKillcamPlayedThisDeath
			|| !(g_MpSetup.options & MPOPTION_KILLCAM)) {
		return;
	}
	if (g_NetKillcam.count < 30) {
		// Not enough recorded history yet (died early in the round). Bail quietly;
		// retried each dead tick by the recorder so a late-enough death still fires.
		return;
	}

	struct player *lp = netKillcamLocalPlayer();
	struct chrdata *localchr = (lp && lp->prop) ? lp->prop->chr : NULL;

	// Resolve the POV: latched killer (SVC_KILL / mpstats) → local pawn's
	// lastattacker → the victim's own eyes. The recorder only calls us after the
	// post-death tail is captured (NET_KILLCAM_POSTDEATH ticks), so any SVC_KILL
	// has long since arrived; the fallbacks just guarantee a replay always fires.
	s32 killeridx = -1;
	const char *src = "pending";
	if (g_NetKillcam.pendingkiller >= 0 && g_NetKillcam.pendingkiller < MAX_MPCHRS
			&& g_MpAllChrPtrs[g_NetKillcam.pendingkiller]
			&& g_MpAllChrPtrs[g_NetKillcam.pendingkiller]->prop
			&& g_MpAllChrPtrs[g_NetKillcam.pendingkiller] != localchr) {
		killeridx = g_NetKillcam.pendingkiller;
	} else if (localchr && localchr->lastattacker && localchr->lastattacker != localchr) {
		for (s32 i = 0; i < MAX_MPCHRS; i++) {
			if (g_MpAllChrPtrs[i] == localchr->lastattacker && g_MpAllChrPtrs[i]->prop) {
				killeridx = i;
				src = "lastattacker";
				break;
			}
		}
	}
	// Last resort: replay from the victim's own POV (still a useful "last 4s").
	if (killeridx < 0) {
		for (s32 i = 0; i < MAX_MPCHRS; i++) {
			if (g_MpAllChrPtrs[i] == localchr) {
				killeridx = i;
				src = "self";
				break;
			}
		}
	}
	struct chrdata *killer = (killeridx >= 0 && killeridx < MAX_MPCHRS) ? g_MpAllChrPtrs[killeridx] : NULL;
	if (!killer || !killer->prop) {
		return; // nothing renderable — try again next dead tick
	}
	g_NetKillcamPlayedThisDeath = true; // one replay per death

	g_NetKillcam.killerchrindex = killeridx;
	g_NetKillcam.pendingkiller = -1;
	g_NetKillcam.windowlen = (s32)g_NetKillcam.count;
	if (g_NetKillcam.windowlen > NET_KILLCAM_TICKS) {
		g_NetKillcam.windowlen = NET_KILLCAM_TICKS;
	}
	g_NetKillcam.playoffset = 0;
	g_NetKillcam.deathframe = (u32)g_Vars.lvframe60;
	g_NetKillcam.active = 1;
	g_NetSpectateChr = killer; // engage the existing spectate POV (player / sim)
	sysLogPrintf(LOG_NOTE, "killcam: START (killer idx=%d via %s, window=%d frames)",
			g_NetKillcam.killerchrindex, src, g_NetKillcam.windowlen);
}

// Ring index of the frame currently being replayed (window ends at head, walk
// forward by playoffset). playoffset only advances in RenderEnd, so the tick-time
// camera (netKillcamGetCamera via netSpectateApply) and the render-time body apply
// (RenderBegin) resolve the SAME frame within a given game frame.
static s32 netKillcamReplayIdx(void)
{
	return ((s32)g_NetKillcam.head - g_NetKillcam.windowlen + 1
			+ g_NetKillcam.playoffset + 2 * NET_KILLCAM_TICKS) % NET_KILLCAM_TICKS;
}

// Camera override consumed by netSpectateApply. Fills the killer's recorded eye /
// look / up / room for the current replay frame so the view follows the killer's
// HISTORICAL aim, not their live one (the camera basis is otherwise rebuilt from
// the live vv_theta during the tick, which is what kept the look stuck on "now").
s32 netKillcamGetCamera(struct coord *eye, struct coord *look, struct coord *up, s32 *room)
{
	if (!g_NetKillcam.active || g_NetKillcam.killerchrindex < 0) {
		return 0;
	}
	const struct netkillcamframe *f = &g_NetKillcamRing[netKillcamReplayIdx()];
	const struct netkillcamentry *e = &f->ents[g_NetKillcam.killerchrindex];
	if (!e->valid) {
		return 0;
	}
	if (e->haspcam) {
		// Player killer: replay the exact recorded first-person camera basis.
		*eye = e->campos;
		*look = e->camlook;
		*up = e->camup;
		*room = e->camroom;
		return 1;
	}
	// Sim killer: derive an eye-camera from the recorded body pose (no stored
	// first-person camera), mirroring netSpectateApply's sim path (yaw-only).
	const f32 TWO_PI = 6.2831853071795865f;
	const f32 thetaRad = TWO_PI - e->pose.yrot;
	const f32 sinT = sinf(thetaRad);
	const f32 cosT = cosf(thetaRad);
	struct coord camlook = { -sinT, 0.f, cosT };
	struct coord eyepos = e->pose.pos;
	eyepos.y += 50.f;
	eyepos.x += camlook.x * 28.f;
	eyepos.z += camlook.z * 28.f;
	*eye = eyepos;
	*look = camlook;
	up->x = 0.f; up->y = 1.f; up->z = 0.f;
	*room = e->camroom;
	return 1;
}

// Returns 1 when a replay frame was applied (caller must call RenderEnd after the
// world render to restore live state); 0 when not replaying.
s32 netKillcamRenderBegin(void)
{
	if (!g_NetKillcam.active) {
		return 0;
	}
	struct chrdata *killer = (g_NetKillcam.killerchrindex >= 0)
			? g_MpAllChrPtrs[g_NetKillcam.killerchrindex] : NULL;
	if (!killer || !killer->prop) {
		netKillcamStop(); // killer freed mid-replay
		return 0;
	}
	g_NetSpectateChr = killer; // keep the spectate target pinned

	const s32 idx = netKillcamReplayIdx();
	const struct netkillcamframe *f = &g_NetKillcamRing[idx];

	for (s32 i = 0; i < MAX_MPCHRS; i++) {
		struct chrdata *chr = g_MpAllChrPtrs[i];
		if (!chr || !chr->prop || !chr->model) {
			g_NetKillcam.saved[i].valid = 0;
			continue;
		}
		netKillcamCapture(chr, &g_NetKillcam.saved[i]); // save live
		if (f->ents[i].valid) {
			netKillcamApply(chr, &f->ents[i]); // apply recorded
			// Un-hide for the replay: the victim (you) and anyone who died in the
			// window is hidden/corpsed live, but was alive in the recording — show
			// them so the replay shows the actual fight. Restored in RenderEnd.
			g_NetKillcamSavedFlags[i] = chr->chrflags;
			chr->chrflags &= ~CHRCFLAG_HIDDEN;
			// THIRD-PERSON muzzle flash: drive each combatant's held-weapon
			// gunfire-visible from the recorded per-hand bit so shooters visibly
			// fire in the replay (the killer's FIRST-PERSON flash is forced below).
			// Self-healing — the live chr-state sync overwrites it after the replay.
			for (s32 h = 0; h < 2; h++) {
				struct prop *wp = chrGetHeldProp(chr, h);
				if (wp && wp->obj) {
					weaponSetGunfireVisible(wp, (f->ents[i].gunfire & (1 << h)) != 0,
							chr->prop->rooms[0]);
				}
			}
		} else {
			g_NetKillcam.saved[i].valid = 0; // not in this recorded frame — don't restore-touch
		}
	}

	// Player killer: override the killer player's camera with the RECORDED basis
	// so the lv.c redirect (currentplayer = killer) renders the HISTORICAL view —
	// and with it the killer's gun viewmodel + your (the victim's) body, which the
	// local-player eye-cam path can't show (you never see yourself in first-person,
	// and the killer's gun isn't your gun). Set after all ticks, before lvRender;
	// next tick rebuilds the killer's live cam, so no restore is needed. Sim killers
	// aren't player slots — the redirect doesn't fire, netSpectateApply drives them.
	{
		const struct netkillcamentry *ke = &f->ents[g_NetKillcam.killerchrindex];
		if (ke->valid && ke->haspcam) {
			const s32 kpnum = playermgrGetPlayerNumByProp(killer->prop);
			if (kpnum >= 0 && g_Vars.players[kpnum]) {
				struct player *kpl = g_Vars.players[kpnum];
				kpl->cam_pos = ke->campos;
				kpl->cam_look = ke->camlook;
				kpl->cam_up = ke->camup;
				kpl->cam_room = ke->camroom;
				// FIRST-PERSON muzzle flash: the redirect renders the killer's gun
				// viewmodel, and bgunRender draws the flash element when hand->flashon
				// is set. Force it from the recorded per-hand gunfire bit so the gun
				// visibly fires at the moments it actually did. Set after the killer's
				// gun tick (which clears flashon when not live-firing) and before
				// lvRender; the next tick re-derives it, so no restore is needed.
				kpl->hands[HAND_RIGHT].flashon = (ke->gunfire & (1 << HAND_RIGHT)) ? true : false;
				kpl->hands[HAND_LEFT].flashon = (ke->gunfire & (1 << HAND_LEFT)) ? true : false;
			}
		}
	}

	static u32 s_lastlog = 0xffffffffu;
	if (g_Vars.lvframe60 != 0 && (u32)g_Vars.lvframe60 / 60u != s_lastlog) {
		s_lastlog = (u32)g_Vars.lvframe60 / 60u;
		sysLogPrintf(LOG_NOTE, "killcam: replay frame %d/%d (killer idx=%d)",
				g_NetKillcam.playoffset, g_NetKillcam.windowlen, g_NetKillcam.killerchrindex);
	}
	return 1;
}

void netKillcamRenderEnd(void)
{
	if (!g_NetKillcam.active) {
		return;
	}
	for (s32 i = 0; i < MAX_MPCHRS; i++) {
		struct chrdata *chr = g_MpAllChrPtrs[i];
		if (chr && chr->prop && chr->model && g_NetKillcam.saved[i].valid) {
			netKillcamApply(chr, &g_NetKillcam.saved[i]); // restore live pose
			chr->chrflags = g_NetKillcamSavedFlags[i];    // restore live hide/flags
		}
	}

	// Advance one recorded frame per LOGICAL tick, not per render frame. The
	// recording side is already gated on g_Vars.lvframe60 (s_lastrecframe
	// above); playback was not, so the 300-tick window was consumed at the
	// render rate — 2x speed at the 120fps netplay cap, half speed at 30fps.
	{
		static u32 lastplayframe = 0xffffffffu;

		if ((u32)g_Vars.lvframe60 != lastplayframe) {
			lastplayframe = (u32)g_Vars.lvframe60;
			g_NetKillcam.playoffset++;
		}
	}

	if (g_NetKillcam.playoffset >= g_NetKillcam.windowlen) {
		netKillcamStop(); // reached the death moment — hand back to the dead-cam
	}
}
