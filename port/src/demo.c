// Demo recording (Phase 2 — brute-force full-match capture to disk).
//
// Reuses the killcam's per-tick capture primitive (netKillcamCaptureLiveFrame):
// instead of a 240-tick ring, every logical 60Hz tick's world pose is appended
// to a file, preceded by a header (mp setup + bot configs + RNG seeds) that lets
// playback reconstruct the stage. Players + sims only in this MVP; props are a
// follow-up (the file is versioned).
//
// Client-side + local, no wire change. Triggered by /demorec start|stop.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "types.h"
#include "bss.h"
#include "data.h"
#include "constants.h"
#include "game/player.h"
#include "game/playermgr.h"
#include "game/chr.h"
#include "game/chraction.h"
#include "game/prop.h"
#include "game/propobj.h"
#include "game/mplayer/mplayer.h"
#include "game/bondgun.h"
#include "game/inv.h"
#include "game/propsnd.h"
#include "game/game_0b0fd0.h"
#include "lib/model.h"
#include "net/net.h"
#include "net/netprop.h"
#include "net/demo.h"
#include "fs.h"
#include "system.h"

#define DEMO_DIR        "$S/demos/"
#define DEMO_DEFAULT    "demo"

// Live-tunable yaw correction (degrees) for REMOTE-player bodies during playback.
// The host stores a remote player's body yaw with a convention offset (it renders
// correctly there, but modelGetChrRotY captures the un-compensated value), so the
// recorded slave body faces wrong by a constant angle. Dial in via /demoyaw, then
// bake the discovered constant as the default. Sims + the local recorder are
// unaffected (offset applies only to haspcam && !islocalplayer pawns).
static f32 g_DemoBodyYawDeg = 0.0f;

static FILE *g_DemoFile = NULL;
static u32 g_DemoFrameCount = 0;
static u32 g_DemoLastFrame = 0xffffffffu;

s32 netDemoIsRecording(void)
{
	return g_DemoFile != NULL;
}

void netDemoStop(void)
{
	if (g_DemoFile == NULL) {
		return;
	}
	fsFileFree(g_DemoFile);
	g_DemoFile = NULL;
	sysLogPrintf(LOG_CHAT, "DEMO: stopped — %u frames recorded", g_DemoFrameCount);
	g_DemoFrameCount = 0;
	g_DemoLastFrame = 0xffffffffu;
}

// Begin recording to DEMO_DIR/<name>.pddm. Overwrites an existing file of the
// same name. Returns 1 on success.
static s32 netDemoStart(const char *name)
{
	if (g_DemoFile != NULL) {
		sysLogPrintf(LOG_CHAT, "DEMO: already recording (use /demorec stop)");
		return 0;
	}
	if (!g_Vars.mplayerisrunning) {
		sysLogPrintf(LOG_CHAT, "DEMO: no match running");
		return 0;
	}

	// Ensure the demos directory exists.
	if (fsFileSize(DEMO_DIR) < 0) {
		fsCreateDir(DEMO_DIR);
	}

	char rel[128];
	snprintf(rel, sizeof(rel), DEMO_DIR "%s.pddm", (name && name[0]) ? name : DEMO_DEFAULT);

	g_DemoFile = fsFileOpenWrite(rel);
	if (g_DemoFile == NULL) {
		sysLogPrintf(LOG_ERROR, "DEMO: cannot open '%s' for writing", rel);
		return 0;
	}

	struct demoheader hdr;
	memset(&hdr, 0, sizeof(hdr));
	hdr.magic = DEMO_MAGIC;
	hdr.version = DEMO_VERSION;
	hdr.protover = NET_PROTOCOL_VER;
	hdr.framewidth = MAX_MPCHRS;
	hdr.botcount = (u32)g_BotCount;
	hdr.rngseeds[0] = g_NetRngSeeds[0];
	hdr.rngseeds[1] = g_NetRngSeeds[1];
	hdr.setup = g_MpSetup;
	for (s32 i = 0; i < NET_MAX_BOTS; i++) {
		hdr.bots[i] = g_BotConfigsArray[i];
	}

	if (fwrite(&hdr, sizeof(hdr), 1, g_DemoFile) != 1) {
		sysLogPrintf(LOG_ERROR, "DEMO: header write failed");
		netDemoStop();
		return 0;
	}

	g_DemoFrameCount = 0;
	g_DemoLastFrame = 0xffffffffu;
	sysLogPrintf(LOG_CHAT, "DEMO: recording to %s (stage %d)", rel, (s32)g_MpSetup.stagenum);
	return 1;
}

// Called once per tick from lvTick (after the killcam recorder). Appends one
// frame at the logical 60Hz rate — lvTick runs per render frame, so gate on
// lvframe60 advancing (the killcam pattern) for framerate-independent demos.
void netDemoRecordTick(void)
{
	if (g_DemoFile == NULL) {
		return;
	}
	if (!g_Vars.mplayerisrunning) {
		// Match ended out from under us — close the file cleanly.
		netDemoStop();
		return;
	}

	const u32 nowframe = (u32)g_Vars.lvframe60;
	if (nowframe == g_DemoLastFrame) {
		return; // already sampled this logical tick
	}
	g_DemoLastFrame = nowframe;

	struct netkillcamframe frame;
	memset(&frame, 0, sizeof(frame));
	netKillcamCaptureLiveFrame(&frame);

	if (fwrite(&frame, sizeof(frame), 1, g_DemoFile) != 1) {
		sysLogPrintf(LOG_ERROR, "DEMO: frame write failed — stopping");
		netDemoStop();
		return;
	}
	g_DemoFrameCount++;
}

// ---------------------------------------------------------------------------
// Playback (/demoplay <file>)
//
// Loads the stage from the header (mp setup + seeds + bot configs → mpStartMatch,
// the same path the netplay client uses on SVC_STAGE_START), then each tick reads
// the next frame and, in a render bracket mirroring the killcam, puppets every
// combatant to its recorded pose and drives the local viewport camera from the
// followed combatant's recorded eye. Live poses are saved and restored around the
// render so the underlying sim isn't disturbed (the displayed frame is always the
// recording, read absolutely from disk).
//
// MVP limits (compile-verified, runtime-unproven): the live sim still ticks
// underneath (bots wander, the local pawn responds to input, match logic can end
// the round); freecam / target cycling / transport (pause/seek/speed) are TODO —
// playback currently follows one combatant from its recorded eye.
// ---------------------------------------------------------------------------

enum { DEMO_PLAY_IDLE = 0, DEMO_PLAY_LOADING, DEMO_PLAY_PLAYING };

static FILE *g_DemoPlayFile = NULL;
static s32 g_DemoPlayState = DEMO_PLAY_IDLE;
static struct demoheader g_DemoPlayHdr;
static struct netkillcamframe g_DemoPlayFrame;  // current recorded frame (applied)
static struct netkillcamframe g_DemoSavedFrame; // live poses saved across the render
static u32 g_DemoSavedFlags[MAX_MPCHRS];
static u8 g_DemoPrevGunfire[MAX_MPCHRS];         // last frame's gunfire bits (shoot-sound edge detect)
static s32 g_DemoLastClip[2] = { -1, -1 };       // followed player's last clip per hand (reload edge detect)
static bool g_DemoPlayHasFrame = false;
static u32 g_DemoPlayFrameIdx = 0;
static u32 g_DemoPlayLastFrame = 0xffffffffu;
static s32 g_DemoFollowIdx = -1;                // g_MpAllChrPtrs slot to view from (future: switch UI)

s32 netDemoIsPlaying(void)
{
	return g_DemoPlayState != DEMO_PLAY_IDLE;
}

// The combatant whose eye the single view renders from. prop.c keeps this pawn on
// playerTickThirdPerson (first-person body hidden) while routing the OTHER player
// pawns to chrTick, so the camera doesn't end up inside the followed player's body.
struct chrdata *netDemoFollowedChr(void)
{
	if (g_DemoPlayState == DEMO_PLAY_PLAYING && g_DemoFollowIdx >= 0 && g_DemoFollowIdx < MAX_MPCHRS) {
		return g_MpAllChrPtrs[g_DemoFollowIdx];
	}
	return NULL;
}

void netDemoPlayStop(void)
{
	if (g_DemoPlayFile) {
		fsFileFree(g_DemoPlayFile);
		g_DemoPlayFile = NULL;
	}
	if (g_DemoPlayState != DEMO_PLAY_IDLE) {
		sysLogPrintf(LOG_CHAT, "DEMO: playback stopped (%u frames shown)", g_DemoPlayFrameIdx);
	}
	g_DemoPlayState = DEMO_PLAY_IDLE;
	g_DemoPlayHasFrame = false;
	g_DemoPlayFrameIdx = 0;
	g_DemoPlayLastFrame = 0xffffffffu;
	g_DemoFollowIdx = -1;
	memset(g_DemoPrevGunfire, 0, sizeof(g_DemoPrevGunfire));
	g_DemoLastClip[0] = g_DemoLastClip[1] = -1;
}

static s32 netDemoPlayStart(const char *name)
{
	if (g_DemoFile != NULL) {
		sysLogPrintf(LOG_CHAT, "DEMO: stop recording before playback");
		return 0;
	}
	if (g_Vars.mplayerisrunning) {
		sysLogPrintf(LOG_CHAT, "DEMO: return to the menu before /demoplay");
		return 0;
	}
	if (!name || !name[0]) {
		sysLogPrintf(LOG_CHAT, "DEMO: usage: /demoplay <file>");
		return 0;
	}
	netDemoPlayStop();

	char rel[128];
	snprintf(rel, sizeof(rel), DEMO_DIR "%s.pddm", name);
	g_DemoPlayFile = fsFileOpenRead(rel);
	if (g_DemoPlayFile == NULL) {
		sysLogPrintf(LOG_CHAT, "DEMO: cannot open '%s'", rel);
		return 0;
	}

	if (fread(&g_DemoPlayHdr, sizeof(g_DemoPlayHdr), 1, g_DemoPlayFile) != 1
			|| g_DemoPlayHdr.magic != DEMO_MAGIC) {
		sysLogPrintf(LOG_CHAT, "DEMO: '%s' is not a demo file", rel);
		netDemoPlayStop();
		return 0;
	}
	if (g_DemoPlayHdr.version != DEMO_VERSION || g_DemoPlayHdr.framewidth != MAX_MPCHRS) {
		sysLogPrintf(LOG_CHAT, "DEMO: '%s' version/format mismatch (v%u w%u, expect v%u w%d)",
				rel, g_DemoPlayHdr.version, g_DemoPlayHdr.framewidth, DEMO_VERSION, MAX_MPCHRS);
		netDemoPlayStop();
		return 0;
	}

	// Reconstruct the stage from the header, then kick the match load. Same path
	// the netplay client uses on SVC_STAGE_START / CLC_ADMIN_SETUP.
	g_MpSetup = g_DemoPlayHdr.setup;
	g_NetRngSeeds[0] = g_DemoPlayHdr.rngseeds[0];
	g_NetRngSeeds[1] = g_DemoPlayHdr.rngseeds[1];
	for (s32 i = 0; i < NET_MAX_BOTS; i++) {
		g_BotConfigsArray[i] = g_DemoPlayHdr.bots[i];
	}
	g_BotCount = (s32)g_DemoPlayHdr.botcount;

	g_DemoPlayState = DEMO_PLAY_LOADING;
	g_DemoPlayHasFrame = false;
	g_DemoPlayFrameIdx = 0;
	g_DemoPlayLastFrame = 0xffffffffu;
	g_DemoFollowIdx = -1;

	sysLogPrintf(LOG_CHAT, "DEMO: loading '%s' (stage %d, %u sims)", rel,
			(s32)g_MpSetup.stagenum, g_DemoPlayHdr.botcount);
	mpStartMatch();
	return 1;
}

// Read the next frame from disk into g_DemoPlayFrame. Returns false at EOF.
static bool netDemoReadFrame(void)
{
	if (fread(&g_DemoPlayFrame, sizeof(g_DemoPlayFrame), 1, g_DemoPlayFile) != 1) {
		return false;
	}
	g_DemoPlayFrameIdx++;
	return true;
}

void netDemoPlayTick(void)
{
	if (g_DemoPlayState == DEMO_PLAY_IDLE) {
		return;
	}

	if (g_DemoPlayState == DEMO_PLAY_LOADING) {
		if (!g_Vars.mplayerisrunning) {
			return; // stage still loading
		}
		g_DemoPlayState = DEMO_PLAY_PLAYING;
		// A multi-human recording loads as a multi-player split (viewports sized for
		// 2+ players). We render ONE fullscreen view; the live playerTick that would
		// normally size the viewport is suppressed during playback, so reconfigure
		// player 0's viewport here. playerGetViewport* now report a single fullscreen
		// view (playerGetLocalCount()==1 during playback).
		if (g_Vars.players[0]) {
			setCurrentPlayerNum(0);
			playerConfigureVi();
		}
		sysLogPrintf(LOG_CHAT, "DEMO: playing");
	}

	if (!g_Vars.mplayerisrunning) {
		// Match ended underneath us (round end / menu) — finish playback.
		netDemoPlayStop();
		return;
	}

	// Skip the spawn intro's TICK gating: the intro drops these tick-mode globals
	// off 2, so lvTickPlayer doesn't run the followed player's tick (no gun setup /
	// processing) for the first few seconds. Force them to 2 every tick (this runs
	// before lvTickPlayer in lvTick) so the followed player ticks from frame one.
	// Pairs with the render-mode force (var80075d60) in netDemoRenderBegin.
	var80075d64 = 2;
	var80075d68 = 2;

	// Advance at the logical 60Hz rate (one recorded frame per logical tick).
	const u32 nowframe = (u32)g_Vars.lvframe60;
	if (nowframe == g_DemoPlayLastFrame) {
		return;
	}
	g_DemoPlayLastFrame = nowframe;

	if (!netDemoReadFrame()) {
		sysLogPrintf(LOG_CHAT, "DEMO: end of demo (%u frames)", g_DemoPlayFrameIdx);
		netDemoPlayStop();
		return;
	}
	g_DemoPlayHasFrame = true;

	// Pick a default view target the first time we have a frame: the first valid
	// combatant with a real first-person camera (a human), else the first valid.
	if (g_DemoFollowIdx < 0) {
		s32 firstvalid = -1;
		for (s32 i = 0; i < MAX_MPCHRS; i++) {
			if (g_DemoPlayFrame.ents[i].valid) {
				if (firstvalid < 0) {
					firstvalid = i;
				}
				if (g_DemoPlayFrame.ents[i].haspcam) {
					g_DemoFollowIdx = i;
					break;
				}
			}
		}
		if (g_DemoFollowIdx < 0) {
			g_DemoFollowIdx = firstvalid;
		}
	}
}

// Drive one local player's viewport camera from a recorded entry. A real
// (non-remote) local player replays its exact basis; sims and remote players —
// whose camera wasn't computed on the recording machine — get a stable yaw-only
// eye-cam from the recorded pose (avoids the "floating" stale-cam view).
static void netDemoApplyCam(struct player *pl, const struct netkillcamentry *e)
{
	if (!pl || !e->valid) {
		return;
	}
	// A genuine local player has the exact recorded first-person basis — replay it.
	if (e->islocalplayer && e->haspcam) {
		pl->cam_pos = e->campos;
		pl->cam_look = e->camlook;
		pl->cam_up = e->camup;
		pl->cam_room = e->camroom;
		return;
	}
	// Otherwise build a yaw-only eye-cam (the killcam sim-eye formula) from the
	// recorded BODY yaw. The recording host maintains body yaw (modelSetChrRotY)
	// for remote players AND sims, but NOT their view angle (vv_theta) — so
	// pose.yrot is the reliable source for both. Pitch is flat for now.
	const f32 TWO_PI = 6.2831853071795865f;
	const f32 yaw = e->pose.yrot;
	const f32 a = TWO_PI - yaw;
	const f32 s = sinf(a);
	const f32 c = cosf(a);
	struct coord eye = e->pose.pos;
	// A player's recorded pos IS its eye/camera height (campos.y == pos.y), so no
	// offset; a sim's pos is at the model root, so raise to approx eye height.
	if (!e->haspcam) {
		eye.y += 50.0f;
	}
	pl->cam_pos = eye;
	pl->cam_look.x = eye.x - s;
	pl->cam_look.y = eye.y;
	pl->cam_look.z = eye.z + c;
	pl->cam_up.x = 0.0f;
	pl->cam_up.y = 1.0f;
	pl->cam_up.z = 0.0f;
	pl->cam_room = (e->pose.rooms[0] != 0) ? e->pose.rooms[0] : pl->cam_room;
}

// Give a puppeted body its recorded held weapon(s) so the right gun model shows
// in its hands. Reuses the sim chr-state weapon-swap path: only acts on a change
// (cheap most frames), frees the old prop cleanly, spawns the new with syncid 0.
// Used for OTHER players (sims manage their own weapons; the followed player is
// first-person). The recorded gunfire bits drive muzzle flash separately.
static void netDemoApplyWeapons(struct chrdata *chr, const struct netkillcamentry *e)
{
	static const u32 hand_flags[2] = { 0, OBJFLAG_WEAPON_LEFTHANDED };
	for (s32 h = 0; h < 2; h++) {
		const s32 want = e->heldweapon[h];
		const s32 cur = (chr->weapons_held[h] && chr->weapons_held[h]->obj
				&& chr->weapons_held[h]->obj->type == OBJTYPE_WEAPON
				&& chr->weapons_held[h]->weapon)
			? (s32)chr->weapons_held[h]->weapon->weaponnum : -1;
		if (want == cur) {
			continue;
		}
		if (chr->weapons_held[h]) {
			struct prop *oldwp = chr->weapons_held[h];
			chr->weapons_held[h] = NULL;
			if (oldwp->obj) {
				weaponSetGunfireVisible(oldwp, false, chr->prop ? chr->prop->rooms[0] : 0);
				netPropFreeSynced(oldwp, NETPROP_FREE_WEAPONSWAP);
			}
		}
		if (want >= 0) {
			const s32 modelnum = playermgrGetModelOfWeapon(want);
			if (modelnum >= 0) {
				chrGiveWeapon(chr, modelnum, want, hand_flags[h]);
			}
		}
	}
}

// First-person viewmodel. players[0] is ALWAYS the single rendered viewport (lv.c
// forcesingleplayer), so it's the "render vehicle": we feed it the FOLLOWED
// combatant's recorded view (camera elsewhere) + weapon here, regardless of which
// combatant is followed. This is what makes /demoview switching work — following
// another player still renders through players[0], just with that player's data.
// The demo-loaded pawn is unarmed (equip happens in a skipped tick), so equip the
// recorded weapon; players[0]'s real playerTick (lv.c) drives the gun pipeline.
static void netDemoTickFollowedGun(void)
{
	if (g_DemoFollowIdx < 0 || !g_Vars.players[0]) {
		return;
	}
	const struct netkillcamentry *e = &g_DemoPlayFrame.ents[g_DemoFollowIdx];
	if (!e->valid) {
		return;
	}

	const s32 saved = g_Vars.currentplayernum;
	setCurrentPlayerNum(0);

	// Skip the Combat Sim spawn intro (third-person camera orbiting the body before
	// it drops to first person): force first-person camera mode so the body isn't
	// drawn and the gun/HUD show immediately. var80075d60 (the render-mode global) is
	// forced to 2 in netDemoRenderBegin so the HUD draws + the first-person path runs.
	g_Vars.currentplayer->cameramode = CAMERAMODE_DEFAULT;

	// Give the recorded weapon to inventory and REQUEST the switch. The followed
	// player keeps its real playerTick during demo (lv.c), so its gun pipeline is
	// initialized and processes this deferred switch over the next frame(s) — which
	// is why a standalone equip didn't take. bgunEquipWeapon is idempotent once the
	// weapon is current.
	const s32 want_r = e->heldweapon[HAND_RIGHT];
	if (want_r >= 0 && g_Vars.currentplayer->gunctrl.weaponnum != want_r) {
		invGiveSingleWeapon(want_r);
		bgunEquipWeapon(want_r);
	}

	// Dual-wield: a second recorded weapon (left hand) means dual-wielding (PD holds
	// the same weapon in both hands). Toggle the gunctrl flag to match; bgunEquipWeapon2
	// sets/clears dualwielding and the followed player's tick brings up/down the off
	// hand. -1 in the left slot => single weapon.
	const s32 want_l = e->heldweapon[HAND_LEFT];
	const bool wantdual = (want_l >= 0);
	if (wantdual != (g_Vars.currentplayer->gunctrl.dualwielding != 0)) {
		if (wantdual) {
			invGiveSingleWeapon(want_l);
		}
		bgunEquipWeapon2(HAND_LEFT, wantdual ? want_l : WEAPON_NONE);
	}

	// Reserve ammo: keep the equipped weapon's reserve topped up so the switch's
	// reload fills the clip and a replayed reload (below) has ammo to draw from.
	const s32 cw = g_Vars.currentplayer->gunctrl.weaponnum;
	if (cw > WEAPON_UNARMED) {
		const u32 atype = bgunGetAmmoTypeForWeapon((u32)cw, FUNC_PRIMARY);
		if (atype != 0) {
			bgunSetAmmoQuantity((s32)atype, 255);
		}
	}

	// Recorded clip ammo (v3): show the recorder's real HUD count, and play the
	// reload animation when the recorded clip jumps up (a reload happened). clipammo
	// is -1 when following a remote player/sim (ammo unknown) — leave the clip alone.
	for (s32 h = 0; h < 2; h++) {
		const s32 clip = e->clipammo[h];
		if (clip < 0) {
			g_DemoLastClip[h] = -1;
			continue;
		}
		if (g_DemoLastClip[h] >= 0 && clip > g_DemoLastClip[h]) {
			bgunReloadIfPossible(h); // recorded reload — drive the reload animation
		}
		g_Vars.currentplayer->hands[h].loadedammo[0] = clip;
		g_DemoLastClip[h] = clip;
	}

	// Force the recorded muzzle flash AND firing state from the gunfire bits (no live
	// trigger, so bgunTick won't drive them). flashon = muzzle flash; firing drives
	// the recoil/fire visual. Set after the gun tick; the fire LOGIC (ammo/sound/
	// bullets) is gated on the live trigger (false here), so this is visual-only.
	const bool fr = (e->gunfire & (1 << HAND_RIGHT)) != 0;
	const bool fl = (e->gunfire & (1 << HAND_LEFT)) != 0;
	g_Vars.currentplayer->hands[HAND_RIGHT].flashon = fr;
	g_Vars.currentplayer->hands[HAND_LEFT].flashon = fl;
	g_Vars.currentplayer->hands[HAND_RIGHT].firing = fr;
	g_Vars.currentplayer->hands[HAND_LEFT].firing = fl;

	setCurrentPlayerNum(saved);
}

s32 netDemoRenderBegin(void)
{
	if (g_DemoPlayState != DEMO_PLAY_PLAYING || !g_DemoPlayHasFrame) {
		return 0;
	}

	// Force first-person render mode (skip the spawn intro): var80075d60 == 2 is the
	// normal HUD/first-person path; the intro drops it to 0/1 (third-person body, no
	// HUD). Set it before lvRender so the followed view renders first-person + HUD
	// from the start. Paired with cameramode = DEFAULT in netDemoTickFollowedGun.
	var80075d60 = 2;

	// Override the camera FIRST: the positional-audio listener is players[0]->cam_pos
	// (propsnd.c psCalculatePan2), and the followed player's live playerTick reset it
	// to its live position this frame. The combatant loop below plays shot sounds via
	// psCreate, so the listener must be the recorded camera by then or every shot
	// attenuates to silence. (Re-asserted nowhere else this frame, so it persists.)
	if (g_DemoFollowIdx >= 0) {
		netDemoApplyCam(g_Vars.players[0], &g_DemoPlayFrame.ents[g_DemoFollowIdx]);
	}

	// Save live poses, then puppet every recorded combatant to its frame pose.
	netKillcamCaptureLiveFrame(&g_DemoSavedFrame);
	for (s32 i = 0; i < MAX_MPCHRS; i++) {
		struct chrdata *chr = g_MpAllChrPtrs[i];
		if (!chr || !chr->prop || !chr->model || !g_DemoPlayFrame.ents[i].valid) {
			continue;
		}
		netKillcamApplyEntry(chr, &g_DemoPlayFrame.ents[i]);
		// Un-hide combatants that are corpsed/hidden live but were alive in the
		// recording, and drive third-person muzzle flash from the recorded bits.
		g_DemoSavedFlags[i] = chr->chrflags;
		chr->chrflags &= ~CHRCFLAG_HIDDEN;
		// Re-ground player pawns: with the live player tick suppressed during
		// playback, a human pawn's vertical ground state (manground/fallspeed) goes
		// stale and the body renders floating. Force it back onto the ground found
		// at the recorded position. Sims keep their own (working) vertical, so this
		// is player-only. Flag is transient (chrflags restored in RenderEnd) and
		// consumed by the render-time ground-find.
		if (chr->prop->type == PROPTYPE_PLAYER) {
			chr->chrflags |= CHRCFLAG_FORCETOGROUND;
			chr->manground = 0.0f;
			chr->sumground = 0.0f;
			chr->fallspeed.x = 0.0f;
			chr->fallspeed.y = 0.0f;
			chr->fallspeed.z = 0.0f;
			// chrTick interpolates the CURRENT aim joints (aimupback/etc., which
			// netKillcamApplyEntry set from the recording) toward the aimend*
			// TARGETS. For a puppeted player those targets are never set, so the
			// body's vertical aim (look up/down) drifts to zero. Pin the targets to
			// the recorded aim so the pitch holds. (Sims aim correctly without this.)
			chr->aimendback = g_DemoPlayFrame.ents[i].pose.aimupback;
			chr->aimendsideback = g_DemoPlayFrame.ents[i].pose.aimsideback;
			chr->aimendlshoulder = g_DemoPlayFrame.ents[i].pose.aimuplshoulder;
			chr->aimendrshoulder = g_DemoPlayFrame.ents[i].pose.aimuprshoulder;
			chr->aimendcount = 10;
		}
		// Remote players (had a player slot but weren't the local recorder) carry a
		// constant body-yaw convention offset from the host; correct it.
		if (g_DemoBodyYawDeg != 0.0f
				&& g_DemoPlayFrame.ents[i].haspcam && !g_DemoPlayFrame.ents[i].islocalplayer) {
			const f32 y = g_DemoPlayFrame.ents[i].pose.yrot + g_DemoBodyYawDeg * (3.14159265f / 180.0f);
			modelSetChrRotY(chr->model, y);
			chrSetRotY(chr, y);
		}
		// Give other players' bodies the recorded weapon model so the right gun
		// shows in-hand (the followed player is first-person; sims manage their own).
		if (chr->prop->type == PROPTYPE_PLAYER && chr != netDemoFollowedChr()) {
			netDemoApplyWeapons(chr, &g_DemoPlayFrame.ents[i]);
		}
		const u8 gf = g_DemoPlayFrame.ents[i].gunfire;
		// Hand-robust firing: a combatant holding exactly ONE weapon flashes/sounds
		// on ANY gunfire bit, not just the matching hand. This works around the
		// netplay bug where a remote player's single weapon lands in the LEFT hand
		// while the gunfire bit is reported on the right (or vice-versa), which
		// otherwise produces a single-weapon shot with no sound/flash. See
		// [[netplay-single-weapon-wrong-hand]]. Dual-wield keeps strict per-hand bits.
		struct prop *wp0 = chrGetHeldProp(chr, 0);
		struct prop *wp1 = chrGetHeldProp(chr, 1);
		const bool has0 = wp0 && wp0->obj;
		const bool has1 = wp1 && wp1->obj;
		const bool single = (has0 != has1); // exactly one weapon held
		for (s32 h = 0; h < 2; h++) {
			struct prop *wp = (h == 0) ? wp0 : wp1;
			if (!wp || !wp->obj) {
				continue;
			}
			const bool on = single ? (gf != 0) : ((gf & (1 << h)) != 0);
			weaponSetGunfireVisible(wp, on, chr->prop->rooms[0]);
			// Gunshot SOUND on the off->on edge (the fire logic that normally plays
			// it is suppressed for puppeted pawns). Positional, like a sim shot.
			const bool was = single ? (g_DemoPrevGunfire[i] != 0) : ((g_DemoPrevGunfire[i] & (1 << h)) != 0);
			if (on && !was && wp->weapon) {
				const u16 soundnum = gsetGetSingleShootSound(&wp->weapon->gset);
				if (soundnum) {
					psCreate(NULL, chr->prop, soundnum, -1, -1, PSFLAG_0400, PSFLAG2_PRINTABLE,
							PSTYPE_CHRSHOOT, NULL, -1.f, NULL, -1, -1.f, -1.f, -1.f);
				}
			}
		}
		g_DemoPrevGunfire[i] = gf;
	}

	// Hide the FOLLOWED combatant's own body so we don't see ourselves from its eye
	// when it isn't player 0 (player 0's body is already hidden by first-person). Its
	// saved flags are restored in RenderEnd.
	if (g_DemoFollowIdx >= 0 && g_DemoPlayFrame.ents[g_DemoFollowIdx].valid) {
		struct chrdata *fchr = g_MpAllChrPtrs[g_DemoFollowIdx];
		struct chrdata *p0chr = (g_Vars.players[0] && g_Vars.players[0]->prop)
				? g_Vars.players[0]->prop->chr : NULL;
		if (fchr && fchr != p0chr) {
			fchr->chrflags |= CHRCFLAG_HIDDEN;
		}
	}

	// First-person viewmodel: give/equip the followed player's recorded weapon + force
	// the muzzle flash (its real playerTick ticks the gun; camera overridden above).
	netDemoTickFollowedGun();
	return 1;
}

void netDemoRenderEnd(void)
{
	if (g_DemoPlayState != DEMO_PLAY_PLAYING) {
		return;
	}
	for (s32 i = 0; i < MAX_MPCHRS; i++) {
		struct chrdata *chr = g_MpAllChrPtrs[i];
		if (chr && chr->prop && chr->model && g_DemoSavedFrame.ents[i].valid) {
			netKillcamApplyEntry(chr, &g_DemoSavedFrame.ents[i]);
			chr->chrflags = g_DemoSavedFlags[i];
		}
	}
}

// /demorec start [name] | stop | status
s32 netDemoConsoleCommand(const char *cmd, const char *arg)
{
	if (strcmp(cmd, "demoplay") == 0) {
		if (strcmp(arg, "stop") == 0) {
			netDemoPlayStop();
		} else {
			netDemoPlayStart(arg);
		}
		return 1;
	}

	// Switch which combatant the single view follows (the split-feature replacement).
	if (strcmp(cmd, "demoview") == 0) {
		if (g_DemoPlayState != DEMO_PLAY_PLAYING || !g_DemoPlayHasFrame) {
			sysLogPrintf(LOG_CHAT, "DEMO: not playing");
			return 1;
		}
		const s32 dir = (strcmp(arg, "prev") == 0) ? -1 : 1;
		s32 idx = (g_DemoFollowIdx >= 0) ? g_DemoFollowIdx : 0;
		for (s32 step = 0; step < MAX_MPCHRS; step++) {
			idx = (idx + dir + MAX_MPCHRS) % MAX_MPCHRS;
			if (g_DemoPlayFrame.ents[idx].valid && g_MpAllChrPtrs[idx]) {
				g_DemoFollowIdx = idx;
				g_DemoLastClip[0] = g_DemoLastClip[1] = -1; // re-detect reload for the new view
				sysLogPrintf(LOG_CHAT, "DEMO: viewing combatant %d%s", idx,
						g_DemoPlayFrame.ents[idx].haspcam ? " (player)" : " (sim)");
				break;
			}
		}
		return 1;
	}

	// Live yaw correction for remote-player bodies (find the convention offset).
	if (strcmp(cmd, "demoyaw") == 0) {
		if (arg[0]) {
			g_DemoBodyYawDeg = (f32)atof(arg);
		}
		sysLogPrintf(LOG_CHAT, "DEMO: remote-body yaw offset = %.1f deg", g_DemoBodyYawDeg);
		return 1;
	}

	if (strcmp(cmd, "demorec") != 0) {
		return 0;
	}

	if (strncmp(arg, "start", 5) == 0) {
		const char *name = arg + 5;
		while (*name == ' ' || *name == '\t') {
			++name;
		}
		netDemoStart(name);
	} else if (strcmp(arg, "stop") == 0) {
		if (g_DemoFile == NULL) {
			sysLogPrintf(LOG_CHAT, "DEMO: not recording");
		} else {
			netDemoStop();
		}
	} else if (arg[0] == '\0' || strcmp(arg, "status") == 0) {
		if (g_DemoFile != NULL) {
			sysLogPrintf(LOG_CHAT, "DEMO: recording, %u frames (~%.1fs)",
					g_DemoFrameCount, (f32)g_DemoFrameCount / 60.0f);
		} else {
			sysLogPrintf(LOG_CHAT, "DEMO: idle (usage: /demorec start [name] | stop | status)");
		}
	} else {
		sysLogPrintf(LOG_CHAT, "DEMO: usage: /demorec start [name] | stop | status");
	}
	return 1;
}
