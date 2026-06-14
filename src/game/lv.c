#include <ultra64.h>
#include "constants.h"
#include "bss.h"
#include "data.h"
#include "det.h"
#include "game/activemenu.h"
#include "game/atan2f.h"
#include "game/bg.h"
#include "game/body.h"
#include "game/bondgun.h"
#include "game/bondhead.h"
#include "game/bondmove.h"
#include "game/bondview.h"
#include "game/camdraw.h"
#include "game/casing.h"
#include "game/cheats.h"
#include "game/chr.h"
#include "game/chraction.h"
#include "game/credits.h"
#include "game/debug.h"
#include "game/dlights.h"
#include "game/explosions.h"
#include "game/filemgr.h"
#include "game/game_006900.h"
#include "game/game_00b820.h"
#include "game/gunfx.h"
#include "game/game_0b0fd0.h"
#include "game/modelmgr.h"
#include "game/portal.h"
#include "game/fmb.h"
#include "game/sky.h"
#include "game/artifact.h"
#include "game/game_1531a0.h"
#include "game/zbuf.h"
#include "game/challenge.h"
#include "game/chrmgr.h"
#include "game/env.h"
#include "game/gfxmemory.h"
#include "game/gunfx.h"
#include "game/hudmsg.h"
#include "game/inv.h"
#include "game/lang.h"
#include "game/lv.h"
#include "game/menu.h"
#include "game/mplayer/ingame.h"
#include "game/mplayer/mplayer.h"
#include "game/mplayer/scenarios.h"
#include "game/mplayer/setup.h"
#include "game/music.h"
#include "game/nbomb.h"
#include "game/objectives.h"
#include "game/pak.h"
#include "game/pdmode.h"
#include "game/player.h"
#include "game/playermgr.h"
#include "game/playerreset.h"
#include "game/prop.h"
#include "game/propobj.h"
#include "game/propobjstop.h"
#include "game/propsnd.h"
#include "game/room.h"
#include "game/savebuffer.h"
#include "game/setup.h"
#include "game/shards.h"
#include "game/sky.h"
#include "game/smoke.h"
#include "game/sparks.h"
#include "game/splat.h"
#include "game/stars.h"
#include "game/stubs/game_013540.h"
#include "game/stubs/game_015260.h"
#include "game/stubs/game_015270.h"
#include "game/stubs/game_0153f0.h"
#include "game/stubs/game_015400.h"
#include "game/stubs/game_015410.h"
#include "game/tex.h"
#include "game/texdecompress.h"
#include "game/tiles.h"
#include "game/title.h"
#include "game/training.h"
#include "game/utils.h"
#include "game/vtxstore.h"
#include "game/wallhit.h"
#include "game/weather.h"
#include "lib/anim.h"
#include "lib/args.h"
#include "lib/collision.h"
#include "lib/crash.h"
#include "lib/joy.h"
#include "lib/lib_06440.h"
#include "lib/lib_317f0.h"
#include "lib/main.h"
#include "lib/mtx.h"
#include "lib/music.h"
#include "lib/rng.h"
#include "lib/sched.h"
#include "lib/snd.h"
#include "lib/vars.h"
#include "lib/vi.h"
#include "types.h"
#ifndef PLATFORM_N64
#include "input.h"
#include "net/net.h"
#include "net/demo.h"
#include "net/netmsg.h"
#include "spectator.h"
#include "video.h"
#endif

struct sndstate *g_MiscSfxAudioHandles[3];
u32 var800aa5bc;
s32 g_MiscSfxActiveTypes[3];

u32 var80084010 = 0;
bool var80084014 = false;
f32 var80084018 = 1;
u32 var8008401c = 0x00000001;

s32 g_Difficulty = DIFF_A;

s32 g_StageTimeElapsed60 = 0;
s32 g_MpTimeLimit60 = SECSTOTIME60(60 * 10); // 10 minutes
s32 g_MpScoreLimit = 10;
s32 g_MpTeamScoreLimit = 20;
struct sndstate *g_MiscAudioHandle = NULL;
s32 g_NumReasonsToEndMpMatch = 0;
f32 g_StageTimeElapsed1f = 0;
bool var80084040 = true;

u32 g_MiscSfxSounds[] = {
	SFX_HEARTBEAT,
	SFX_SLAYER_WHIR,
	SFX_SLAYER_BEEP,
};

s32 var80084050 = 0;

s16 g_FadeNumFrames = 0;
f32 g_FadeFrac = -1;
u32 g_FadePrevColour = 0;
u32 g_FadeColour = 0;
s16 g_FadeDelay = 0;

u32 getVar80084040(void)
{
	return var80084040;
}

void setVar80084040(u32 value)
{
	var80084040 = value;
}

void lvInit(void)
{
	g_Vars.lockscreen = 0;
	g_Vars.joydisableframestogo = -1;
}

void lvResetMiscSfx(void)
{
	s32 i;

	for (i = 0; i != ARRAYCOUNT(g_MiscSfxAudioHandles); i++) {
		g_MiscSfxAudioHandles[i] = NULL;
		g_MiscSfxActiveTypes[i] = -1;
	}
}

s32 lvGetMiscSfxIndex(u32 type)
{
	s32 i;

	for (i = 0; i != ARRAYCOUNT(g_MiscSfxActiveTypes); i++) {
		if (g_MiscSfxActiveTypes[i] == type) {
			return i;
		}
	}

	return -1;
}

void lvSetMiscSfxState(u32 type, bool play)
{
	if (play) {
		if (lvGetMiscSfxIndex(type) == -1) {
			s32 index = lvGetMiscSfxIndex(-1);

#if VERSION >= VERSION_NTSC_1_0
			if (index != -1 && g_MiscSfxAudioHandles[index] == NULL)
#else
			if (index != -1)
#endif
			{
				sndStart(var80095200, g_MiscSfxSounds[type], &g_MiscSfxAudioHandles[index], -1, -1, -1, -1, -1);
				g_MiscSfxActiveTypes[index] = type;
			}
		}
	} else {
		u32 stack;
		s32 index = lvGetMiscSfxIndex(type);

		if (index != -1) {
			audioStop(g_MiscSfxAudioHandles[index]);
#if VERSION < VERSION_NTSC_1_0
			g_MiscSfxAudioHandles[index] = 0;
#endif
			g_MiscSfxActiveTypes[index] = -1;
		}
	}
}

void lvUpdateMiscSfx(void)
{
	s32 i;

#ifndef PLATFORM_N64
	// Slow motion can legitimately produce zero-tick frames at high fps (the
	// halved tick alternates 0/1 above 120fps). Only treat a zero frame as
	// "stopped" when the game is actually paused — these are the only paths
	// that set lvupdate240 to 0 in lvTick — so the boost heartbeat / rocket
	// hum don't stutter on/off every other frame during slow motion.
	if (g_Vars.lvupdate240 == 0 && (lvIsPaused() || mpIsPaused())) {
#else
	if (g_Vars.lvupdate240 == 0) {
#endif
		for (i = 0; i != ARRAYCOUNT(g_MiscSfxActiveTypes); i++) {
			lvSetMiscSfxState(i, false);
		}
	} else {
		bool usingboost = g_Vars.speedpillon
			&& lvGetSlowMotionType() == SLOWMOTION_OFF
			&& g_Vars.in_cutscene == false;
		bool usingrocket;

		lvSetMiscSfxState(MISCSFX_BOOSTHEARTBEAT, usingboost);

		usingrocket = false;

		for (i = 0; i < PLAYERCOUNT(); i++) {
			if (g_Vars.players[i]->visionmode == VISIONMODE_SLAYERROCKET
#ifndef PLATFORM_N64
					// Net (proto 76): a remote pawn flying its rocket runs only on
					// the server's tick, not in any local viewport — don't loop the
					// slayer hum/beep on the listen host for it.
					&& !g_Vars.players[i]->isremote
#endif
					) {
				usingrocket = true;
			}
		}

		lvSetMiscSfxState(MISCSFX_SLAYERROCKETHUM, usingrocket);
		lvSetMiscSfxState(MISCSFX_SLAYERROCKETBEEP, usingrocket);
	}

#ifndef PLATFORM_N64
	// Same zero-tick gate as above.
	if (g_Vars.lvupdate240 == 0 && (lvIsPaused() || mpIsPaused())
			&& g_MiscAudioHandle && sndGetState(g_MiscAudioHandle) != AL_STOPPED) {
#else
	if (g_Vars.lvupdate240 == 0 && g_MiscAudioHandle && sndGetState(g_MiscAudioHandle) != AL_STOPPED) {
#endif
		audioStop(g_MiscAudioHandle);
	}
}

void lvReset(s32 stagenum)
{
	lvFadeReset();

#ifndef PLATFORM_N64
	netKillcamReset(); // killcam: clear the recording ring on stage load (port-only)
	netDemoStop();     // demo: close any open recording on stage load (port-only)
#endif

	var80084014 = false;
	var80084010 = 0;

#if VERSION >= VERSION_NTSC_1_0
	joyLockCyclicPolling();

	g_Vars.joydisableframestogo = 10;
#else
	if (joyIsCyclicPollingEnabled()) {
		joyDisableCyclicPolling(760, "lv.c");

		g_Vars.joydisableframestogo = 10;
	}
#endif

	g_Vars.paksneededforgame = 0;
	g_Vars.paksneededformenu = 0;
	g_Vars.stagenum = stagenum;

	cheatsReset();

	var80084040 = true;
	g_Vars.lvframenum = 0;
	var80084050 = 0;

	g_Vars.lvframe60 = 0;
	g_Vars.lvupdate240 = 4;

#if VERSION >= VERSION_NTSC_1_0
	g_Vars.lvupdate60f = 1.0f;
	g_Vars.lvupdate60frealprev = PALUPF(1);
#else
	g_Vars.lvupdate60frealprev = PALUPF(1);
	g_Vars.lvupdate60f = 1.0f;
#endif

	g_Vars.lvupdate60freal = g_Vars.lvupdate60frealprev;

	g_StageTimeElapsed60 = 0;
	g_StageTimeElapsed1f = 0;

	g_Vars.speedpilltime = 0;
	g_Vars.speedpillchange = 0;
	g_Vars.speedpillwant = 0;
	g_Vars.speedpillon = false;

	g_Vars.restartlevel = false;
	g_Vars.aibuddiesspawned = false;
	g_Vars.totalkills = 0;
	g_Vars.antiheadnum = -1;
	g_Vars.antibodynum = -1;
	g_Vars.dontplaynrg = false;
	g_Vars.in_cutscene = false;
	g_Vars.autocutplaying = false;
	g_Vars.autocutfinished = false;
	g_Vars.autocutgroupskip = false;

	g_MiscAudioHandle = NULL;

	musicReset();
	modelmgrSetLvResetting(true);
	surfaceReset();
	texReset();
	textReset();
	hudmsgsReset();

	if (stagenum == STAGE_TEST_OLD) {
		titleReset();
	}

	if (stagenum == STAGE_TITLE) {
		titleReset();
	} else if (stagenum == STAGE_BOOTPAKMENU) {
		// empty
	} else if (stagenum == STAGE_CREDITS) {
		// empty
	} else if (stagenum == STAGE_4MBMENU) {
		// empty
	} else {
		s32 i;
		s32 j;

#ifndef PLATFORM_N64
		if (g_NetMode == NETMODE_SERVER) {
			// if we're a server, signal to clients that the level is changing
			netServerStageStart();
		} else if (g_NetMode == NETMODE_CLIENT) {
			// if we're a client, now is the time to apply the server's RNG seeds
			netClientSyncRng();
		}
#endif

		tilesReset();
		bgReset(g_Vars.stagenum);
		bgBuildTables(g_Vars.stagenum);
		skyReset(g_Vars.stagenum);

		if (g_Vars.normmplayerisrunning) {
			musicSetStageAndStartMusic(stagenum);
		} else {
			musicSetStage(stagenum);
		}

		if (g_Vars.normmplayerisrunning) {
			mpApplyLimits();
		}

		if (g_Vars.mplayerisrunning == false) {
			g_Vars.playerstats[0].mpindex = MAX_PLAYERS;
			g_PlayerConfigsArray[MAX_PLAYERS].contpad1 = 0;
			g_PlayerConfigsArray[MAX_PLAYERS].contpad2 = 1;
		}

		for (i = 0; i != ARRAYCOUNT(g_Vars.playerstats); i++) {
			g_Vars.playerstats[i].damagescale = 1;
			g_Vars.playerstats[i].drawplayercount = 0;
			g_Vars.playerstats[i].distance = 0;
			g_Vars.playerstats[i].backshotcount = 0;
			g_Vars.playerstats[i].armourcount = 0;
			g_Vars.playerstats[i].fastest2kills = S32_MAX;
			g_Vars.playerstats[i].slowest2kills = 0;
			g_Vars.playerstats[i].maxkills = 0;
			g_Vars.playerstats[i].maxsimulkills = 0;
			g_Vars.playerstats[i].longestlife = 0;
			g_Vars.playerstats[i].shortestlife = S32_MAX;
			g_Vars.playerstats[i].tokenheldtime = 0;
			g_Vars.playerstats[i].damreceived = 0;
			g_Vars.playerstats[i].damtransmitted = 0;

			for (j = 0; j != ARRAYCOUNT(g_Vars.playerstats[i].kills); j++) {
				g_Vars.playerstats[i].kills[j] = 0;
			}
		}
	}

	mpSetDefaultNamesIfEmpty();
	animsReset();
	objectivesReset();
	vtxstoreReset();
	modelmgrReset();
	psReset();
	setupLoadFiles(stagenum);
	scenarioReset();
	varsReset();
	propsReset();
	chrmgrReset();
	bodiesReset(stagenum);
	setupCreateProps(stagenum);
	tagsReset();
	explosionsReset();
	smokeReset();
	sparksReset();
	weatherReset();
	lvResetMiscSfx();

	switch (g_Vars.stagenum) {
	case STAGE_ESCAPE:
	case STAGE_EXTRACTION:
	case STAGE_INFILTRATION:
	case STAGE_DEFECTION:
	case STAGE_ATTACKSHIP:
	case STAGE_TEST_OLD:
#ifndef PLATFORM_N64
	case STAGE_TEST_ARCH: // Suburb
#endif
		starsReset();
		break;
	}

	func0f0099a4();
	boltbeamsReset();
	lasersightsReset();
	stub0f013540();
	shardsReset();
	frReset();

	if (g_Vars.stagenum == STAGE_TITLE) {
		// empty
	} else if (stagenum == STAGE_BOOTPAKMENU) {
		setCurrentPlayerNum(0);
		menuReset();
	} else if (stagenum == STAGE_4MBMENU) {
		setCurrentPlayerNum(0);
		menuReset();
	} else if (stagenum == STAGE_CREDITS) {
		creditsReset();
	} else {
		s32 i;

		utilsReset();
		casingsReset();

		for (i = 0; i < PLAYERCOUNT(); i++) {
			setCurrentPlayerNum(i);
			g_Vars.currentplayer->usedowntime = 0;
			g_Vars.currentplayer->invdowntime = g_Vars.currentplayer->usedowntime;

			menuReset();
			amReset();
			invReset();
			bgunReset();
			playerLoadDefaults();
			playerReset();
			playerSpawn();

#ifndef PLATFORM_N64
			// Campaign co-op: drop the partner a few units from the lead player at
			// stage start (instead of a separate spawn pad), like the SP co-op buddy.
			// chrAdjustPosForSpawn finds a clear adjacent spot (Defection-aware: 80u
			// to clear P1's intro animation); chrSetPos then re-grounds and updates
			// the player's vv_* fields. Net co-op host only — the remote client
			// hard-snaps to this via the UCMD_FL_FORCE* flags set just below. Only
			// the non-lead player(s) move; player 0 keeps its pad. Runs at i>=1 when
			// player 0 is already positioned, so its pos is valid here.
			if (g_NetMode == NETMODE_SERVER && g_Vars.coopplayernum >= 0
					&& g_Vars.currentplayernum != g_Vars.bondplayernum) {
				struct player *lead = g_Vars.players[g_Vars.bondplayernum];
				if (lead && lead->prop && lead->prop->chr
						&& g_Vars.currentplayer->prop && g_Vars.currentplayer->prop->chr) {
					struct coord spawnpos = lead->prop->pos;
					RoomNum spawnrooms[8];
					roomsCopy(lead->prop->rooms, spawnrooms);
					// onlysurrounding=true: ALWAYS place at an adjacent offset (80u on
					// Defection), never test the lead's exact spot first — at stage
					// load the lead's collision cylinder isn't active, so the
					// exact-spot test reads clear and the partner would land right on
					// top of them. This is the engine's own co-op P2 distance.
					chrAdjustPosForSpawn(30, &spawnpos, spawnrooms, 0.0f, true, true, true);
					chrSetPos(g_Vars.currentplayer->prop->chr, &spawnpos, spawnrooms, lead->vv_theta, true);
				}
			}

			// The initial MP spawn was RNG-desynced between server and client.
			// The pad pick is deterministic per local slot (synced RNG -> same
			// pad per slot on every machine), but each client swaps its local
			// player to slot 0 (netPlayersAllocate, net.c:1700), so a client's
			// local pawn picks the pad the server assigned to slot 0 (the host).
			// Under the trust-client model the server then ADOPTS that reported
			// position, collapsing every client's pawn onto the host's pad.
			//
			// Fix: force remote combatants to hard-snap to the SERVER's
			// authoritative spawn, and hold it server-side so the client's echo
			// can't drag it back. forcetick is latched DIRECTLY here rather than
			// relying on the pl->ucmd auto-latch in net.c (netClientRecordMove):
			// at stage load bwalkUpdateRemote runs before the first move-record,
			// so a bare FORCEMASK would be wiped by the bondwalk.c one-shot clear
			// (forcetick == 0 path) before it could latch. Mirrors the respawn
			// handling in playerStartNewLife (player.c:735-744). Clears itself on
			// client ack via netmsgClcMoveRead.
			if (g_NetMode == NETMODE_SERVER && g_Vars.currentplayer->isremote
					&& g_Vars.currentplayer->client) {
				g_Vars.currentplayer->ucmd |= UCMD_FL_FORCEPOS | UCMD_FL_FORCEANGLE | UCMD_FL_FORCEGROUND;
				g_Vars.currentplayer->client->forcetick = g_NetTick;
			}
#endif

			bheadReset();

			if (g_Vars.normmplayerisrunning && (g_MpSetup.options & MPOPTION_TEAMSENABLED)) {
				playermgrCalculateAiBuddyNums();
			}
		}

		acousticReset();
		portalsReset();
		lightsReset();
		setCurrentPlayerNum(0);
	}

	if (g_Vars.lvmpbotlevel) {
		mpCalculateTeamIsOnlyAi();
	}

	paksReset();
	sndResetCurMp3();

	if (stagenum == STAGE_BOOTPAKMENU) {
		bootmenuReset();
	}

	if (stagenum == STAGE_4MBMENU) {
		fmbReset();
	}

	if (IS8MB()) {
		pheadReset();
	}

#ifndef PLATFORM_N64
	// Stage transition: never carry a slow-motion step into the next stage /
	// the lobby (the speedpill vars are reset above; net clients get a fresh
	// SVC_TIMESCALE if the new match engages it).
	g_LvSlomoEngaged = false;

	if (g_NetMode) {
		netSyncIdsAllocate();
	}
#endif

	modelmgrSetLvResetting(false);
	var80084018 = 1;
	schedResetArtifacts();
	lvSetPaused(0);

#if PIRACYCHECKS
	{
		u32 checksum = 0;
		s32 *i = (s32 *)&lvGetSlowMotionType;
		s32 *end = (s32 *)&lvTick;

		while (i < end) {
			checksum += *i;
			i++;
		}

		if (checksum != CHECKSUM_PLACEHOLDER) {
			// This is writing a file to the start of the EEPROM data.
			// The file is PAKFILETYPE_TERMINATOR, which is used internally to
			// mark the end of the usable space. This effectively deletes all
			// save data on the game pak and makes it permanently unusable.
			u32 address = 0;
			u32 buffer[4];
			buffer[0] = 0xbb8b80bd;
			buffer[1] = 0xffffffff;
			buffer[2] = 0x020f0100;
			buffer[3] = 0xcd31100b;
			osEepromLongWrite(&g_PiMesgQueue, address, (u8 *)&buffer, 0x10);
			g_Paks[SAVEDEVICE_GAMEPAK].headercachecount = 0;
		}
	}
#endif
}

void lvConfigureFade(u32 color, s16 num_frames)
{
	g_FadeNumFrames = num_frames;
	g_FadePrevColour = g_FadeColour;

	if (g_FadeNumFrames == 0) {
		g_FadeColour = color;
		g_FadeFrac = -1;
		return;
	}

	g_FadeFrac = 0;
	g_FadeColour = color;
	g_FadeDelay = 2;
}

Gfx *lvRenderFade(Gfx *gdl)
{
	u32 colour = g_FadeColour;
	u32 inset = 0;

	if (g_Vars.stagenum == STAGE_TEST_OLD) {
		inset = 61;
	}

	if (g_FadeFrac >= 0) {
		if (g_FadeDelay > 0) {
			g_FadeDelay--;
		} else {
#if VERSION >= VERSION_PAL_BETA
			g_FadeFrac += g_Vars.diffframe60freal / g_FadeNumFrames;
#else
			g_FadeFrac += g_Vars.diffframe60f / g_FadeNumFrames;
#endif

			if (g_FadeFrac >= 1) {
				g_FadeFrac = -1;
			}
		}
	}

	if (g_FadeFrac < 0) {
		if ((g_FadeColour & 0xff) == 0) {
			return gdl;
		}
	} else {
		colour = colourBlend(g_FadeColour, g_FadePrevColour, g_FadeFrac * 255);
	}

	if ((colour & 0xff) == 0) {
		return gdl;
	}

	gDPPipeSync(gdl++);
	gDPSetRenderMode(gdl++, G_RM_CLD_SURF, G_RM_CLD_SURF2);
	gDPSetCombineMode(gdl++, G_CC_PRIMITIVE, G_CC_PRIMITIVE);
	gDPSetPrimColorViaWord(gdl++, 0, 0, colour);

	gDPFillRectangle(gdl++,
			viGetViewLeft(),
			viGetViewTop() + inset,
			viGetViewLeft() + viGetViewWidth() + 1,
			viGetViewTop() + viGetViewHeight() - inset + 2);

	return text0f153838(gdl);
}

bool lvIsFadeActive(void)
{
	return g_FadeFrac >= 0;
}

void lvFadeReset(void)
{
	g_FadeNumFrames = 0;
	g_FadeFrac = -1;
	g_FadePrevColour = 0;
	g_FadeColour = 0;
	g_FadeDelay = 0;
}

bool lvUpdateTrackedProp(struct trackedprop *trackedprop, s32 index)
{
	f32 y1;
	f32 x1;
	f32 y2;
	f32 x2;
	struct prop *prop = trackedprop->prop;
	struct chrdata *chr;

	if (trackedprop->prop && prop->chr) {
		switch (trackedprop->prop->type) {
		case PROPTYPE_PLAYER:
			if (playermgrGetPlayerNumByProp(prop) == g_Vars.currentplayernum) {
				return false;
			}
			// fall through
		case PROPTYPE_CHR:
			chr = trackedprop->prop->chr;

			if (chrIsDead(trackedprop->prop->chr)) {
				if (index >= 0) {
					// Existing trackedprop
					if (g_Vars.currentplayer->targetset[index] < TICKS(129)) {
						g_Vars.currentplayer->targetset[index] = TICKS(129);
					}

					if (g_Vars.currentplayer->targetset[index] >= (PAL ? 146 : 175)) {
						trackedprop->prop = NULL;
						return false;
					}
				} else {
					// lookingatprop
					trackedprop->prop = NULL;
					return false;
				}
			}

			if ((trackedprop->prop->flags & PROPFLAG_ONTHISSCREENTHISTICK)
					&& (chr->chrflags & CHRCFLAG_NOAUTOAIM) == 0) {
				struct model *model = chr->model;
				x1 = -1;
				y1 = -1;
				x2 = -2;
				y2 = -2;

				if (modelGetScreenCoords(model, &x2, &x1, &y2, &y1)) {
					break;
				}
				return false;
			}
			return false;
		case PROPTYPE_OBJ:
		case PROPTYPE_WEAPON:
			if (trackedprop->prop->flags & PROPFLAG_ONTHISSCREENTHISTICK) {
				struct defaultobj *obj = trackedprop->prop->obj;
				struct model *model = obj->model;
				x1 = -1;
				y1 = -1;
				x2 = -2;
				y2 = -2;

				if (modelGetScreenCoords(model, &x2, &x1, &y2, &y1)) {
					break;
				}
				return false;
			}
			return false;
		case PROPTYPE_DOOR:
		case PROPTYPE_EYESPY:
		case PROPTYPE_EXPLOSION:
		case PROPTYPE_SMOKE:
		default:
			return false;
		}

		trackedprop->x1 = x1 - 2;
		trackedprop->x2 = x2 + 2;
		trackedprop->y1 = y1 - 2;
		trackedprop->y2 = y2 + 2;
	}

	return true;
}

#ifdef DEBUG
Gfx *lvRenderManPosIfEnabled(Gfx *gdl)
{
	char bufroom[16];
	char bufx[16];
	char bufy[16];
	char bufz[16];
	char bufdir[16];
	s32 x;
	s32 y;
	s32 y2;

	if (debugIsManPosEnabled()) {
		f32 xfrac = g_Vars.currentplayer->bond2.unk00.x;
		f32 zfrac = g_Vars.currentplayer->bond2.unk00.z;

		char directions[][3] = {
			{'n', '\0', '\0'},
			{'n', 'e',  '\0'},
			{'e', '\0', '\0'},
			{'s', 'e',  '\0'},
			{'s', '\0', '\0'},
			{'s', 'w',  '\0'},
			{'w', '\0', '\0'},
			{'n', 'w',  '\0'},
			{'n', '\0', '\0'},
		};

		s32 degrees = atan2f(-xfrac, zfrac) * 180.0f / M_PI;

		sprintf(bufroom, "R=%d(%d)", g_Vars.currentplayer->prop->rooms[0], g_Vars.currentplayer->cam_room);
		sprintf(bufx, "%s%sx %4.0f", "", "", g_Vars.currentplayer->prop->pos.x);
		sprintf(bufy, "%s%sy %4.0f", "", "", g_Vars.currentplayer->prop->pos.y);
		sprintf(bufz, "%s%sz %4.0f", "", "", g_Vars.currentplayer->prop->pos.z);
		sprintf(bufdir, "%s %3d", &directions[(degrees + 22) / 45], degrees);

		x = viGetViewLeft() + 17;
		y = viGetViewTop() + 17;
		y2 = y + 10;
		gdl = text0f153628(gdl);
		gdl = text0f153a34(gdl, 0, y - 1, viGetWidth(), y2 + 1, 0x00000064);

		gdl = textRenderProjected(gdl, &x, &y, bufroom, g_CharsHandelGothicSm, g_FontHandelGothicSm, 0xffffffff, viGetWidth(), viGetHeight(), 0, 0);

		x = viGetViewLeft() + 87;
		gdl = textRenderProjected(gdl, &x, &y, bufx, g_CharsHandelGothicSm, g_FontHandelGothicSm, 0xffffffff, viGetWidth(), viGetHeight(), 0, 0);

		x = viGetViewLeft() + 141;
		gdl = textRenderProjected(gdl, &x, &y, bufy, g_CharsHandelGothicSm, g_FontHandelGothicSm, 0xffffffff, viGetWidth(), viGetHeight(), 0, 0);

		x = viGetViewLeft() + 195;
		gdl = textRenderProjected(gdl, &x, &y, bufz, g_CharsHandelGothicSm, g_FontHandelGothicSm, 0xffffffff, viGetWidth(), viGetHeight(), 0, 0);

		x = viGetViewLeft() + 249;
		gdl = textRenderProjected(gdl, &x, &y, bufdir, g_CharsHandelGothicSm, g_FontHandelGothicSm, 0xffffffff, viGetWidth(), viGetHeight(), 0, 0);

		gdl = text0f153780(gdl);
	}

	return gdl;
}
#endif

void lvFindThreatsForProp(struct prop *prop, bool inchild, struct coord *playerpos, bool *activeslots, f32 *distances)
{
	bool condition = true;
	struct defaultobj *obj;
	bool pass;
	f32 sp88;
	f32 sp84;
	f32 sp80;
	f32 sp76;
	s32 i;
	struct model *model;
	struct weaponobj *weapon;

	if (!inchild && prop->z < 0) {
		condition = false;
	}

	if (prop->obj
			&& (prop->flags & PROPFLAG_ONTHISSCREENTHISTICK)
			&& (prop->type == PROPTYPE_OBJ || prop->type == PROPTYPE_WEAPON)
			&& condition) {
		pass = false;
		obj = prop->obj;
		model = prop->obj->model;

		if (obj
				&& obj->type == OBJTYPE_AUTOGUN
				&& (obj->flags2 & (OBJFLAG2_AUTOGUN_MALFUNCTIONING1 | OBJFLAG2_AICANNOTUSE)) == 0) {
			pass = true;
		}

		if (obj && obj->modelnum == MODEL_SK_SHUTTLE) {
			pass = true;
		}

		weapon = (struct weaponobj *)prop->obj;

		if (weapon && prop->obj->type == OBJTYPE_WEAPON) {
			switch (weapon->weaponnum) {
			case WEAPON_GRENADE:
			case WEAPON_NBOMB:
			case WEAPON_TIMEDMINE:
			case WEAPON_PROXIMITYMINE:
			case WEAPON_REMOTEMINE:
				pass = true;
				break;
			case WEAPON_DRAGON:
				if (weapon->gunfunc == (u32)FUNC_SECONDARY) {
					pass = true;
				}
				break;
			}
		}

		if (obj->modelnum == MODEL_TARGET && frIsTargetOneHitExplodable(prop)) {
			pass = true;
		}

		if (pass) {
			for (i = 0; i < ARRAYCOUNT(g_Vars.currentplayer->trackedprops); i++) {
				if (g_Vars.currentplayer->trackedprops[i].prop == prop) {
					pass = false;
				}
			}
		}

		if (pass) {
			sp84 = -1;
			sp88 = -1;
			sp76 = -2;
			sp80 = -2;

			if (!modelGetScreenCoords(model, &sp76, &sp84, &sp80, &sp88)) {
				pass = false;
			}
		}

		if (pass) {
			f32 furtherestdist = 0;
			s32 index = -1;

			f32 sqdist =
				(prop->pos.f[0] - playerpos->f[0]) * (prop->pos.f[0] - playerpos->f[0]) +
				(prop->pos.f[1] - playerpos->f[1]) * (prop->pos.f[1] - playerpos->f[1]) +
				(prop->pos.f[2] - playerpos->f[2]) * (prop->pos.f[2] - playerpos->f[2]);

			for (i = 0; i < ARRAYCOUNT(g_Vars.currentplayer->trackedprops); i++) {
				if (!activeslots[i]) {
					index = i;
				}
			}

			if (index == -1) {
				// No slots available - consider replacing the furtherest
				for (i = 0; i < ARRAYCOUNT(g_Vars.currentplayer->trackedprops); i++) {
					if (distances[i] > furtherestdist) {
						furtherestdist = distances[i];
						index = i;
					}
				}

				if (sqdist >= furtherestdist) {
					index = -1;
				}
			}

			if (index >= 0) {
				g_Vars.currentplayer->trackedprops[index].prop = prop;
				g_Vars.currentplayer->trackedprops[index].x1 = sp84 - 2;
				g_Vars.currentplayer->trackedprops[index].x2 = sp76 + 2;
				g_Vars.currentplayer->trackedprops[index].y1 = sp88 - 2;
				g_Vars.currentplayer->trackedprops[index].y2 = sp80 + 2;
				g_Vars.currentplayer->targetset[index] = 0;
				activeslots[index] = true;
				distances[index] = sqdist;
			}
		}
	}

	if (prop->child) {
		lvFindThreatsForProp(prop->child, true, playerpos, activeslots, distances);
	}

	if (inchild && prop->next) {
		lvFindThreatsForProp(prop->next, inchild, playerpos, activeslots, distances);
	}
}

void func0f168f24(struct prop *prop, bool inchild, struct coord *playerpos, s32 *activeslots, f32 *distances)
{
	s32 i;
	f32 sp128;
	f32 sp124;
	f32 sp120;
	f32 sp116;
	struct model *model;

	for (i = 0; i != 4; i++) {
		if (g_Vars.currentplayer->trackedprops[i].prop == prop
				&& (prop->flags & PROPFLAG_ONTHISSCREENTHISTICK)) {
			model = NULL;

			if (prop->type == PROPTYPE_OBJ
					|| prop->type == PROPTYPE_WEAPON
					|| prop->type == PROPTYPE_DOOR) {
				model = g_Vars.currentplayer->trackedprops[i].prop->obj->model;
			} else {
				if (prop->type == PROPTYPE_CHR
						|| (prop->type == PROPTYPE_PLAYER
							&& playermgrGetPlayerNumByProp(prop) != g_Vars.currentplayernum)) {
					model = g_Vars.currentplayer->trackedprops[i].prop->chr->model;
				}
			}

			if (model) {
				sp124 = -1;
				sp128 = -1;
				sp116 = -2;
				sp120 = -2;

				if (modelGetScreenCoords(model, &sp116, &sp124, &sp120, &sp128)) {
					activeslots[i] = true;
					g_Vars.currentplayer->trackedprops[i].x1 = sp124 - 2;
					g_Vars.currentplayer->trackedprops[i].x2 = sp116 + 2;
					g_Vars.currentplayer->trackedprops[i].y1 = sp128 - 2;
					g_Vars.currentplayer->trackedprops[i].y2 = sp120 + 2;

					distances[i] =
						(prop->pos.f[0] - playerpos->f[0]) * (prop->pos.f[0] - playerpos->f[0]) +
						(prop->pos.f[1] - playerpos->f[1]) * (prop->pos.f[1] - playerpos->f[1]) +
						(prop->pos.f[2] - playerpos->f[2]) * (prop->pos.f[2] - playerpos->f[2]);
				}
			}
		}
	}

	if (prop->child) {
		func0f168f24(prop->child, true, playerpos, activeslots, distances);
	}

	if (inchild && prop->next) {
		func0f168f24(prop->next, inchild, playerpos, activeslots, distances);
	}
}

void lvFindThreats(void)
{
	s32 i;
	struct prop *prop;
	f32 distances[ARRAYCOUNT(g_Vars.currentplayer->trackedprops)] = {0};
	s32 activeslots[ARRAYCOUNT(g_Vars.currentplayer->trackedprops)] = {false};
	struct prop **propptr = g_Vars.endonscreenprops - 1;
	struct coord campos;

	campos.x = g_Vars.currentplayer->cam_pos.x;
	campos.y = g_Vars.currentplayer->cam_pos.y;
	campos.z = g_Vars.currentplayer->cam_pos.z;

	while (propptr >= g_Vars.onscreenprops) {
		prop = *propptr;

		if (prop) {
			func0f168f24(prop, false, &campos, activeslots, distances);
		}

		propptr--;
	}

	for (i = 0; i != ARRAYCOUNT(activeslots); i++) {
		if (!activeslots[i]) {
			g_Vars.currentplayer->trackedprops[i].prop = NULL;
			g_Vars.currentplayer->trackedprops[i].x1 = -1;
			g_Vars.currentplayer->trackedprops[i].x2 = -2;
		}
	}

	propptr = g_Vars.endonscreenprops - 1;

	while (propptr >= g_Vars.onscreenprops) {
		prop = *propptr;

		if (prop) {
			lvFindThreatsForProp(prop, false, &campos, activeslots, distances);
		}

		propptr--;
	}
}

#ifndef PLATFORM_N64
Gfx *lvRenderFPS(Gfx *gdl)
{
	const f32 fps = videoGetAverageFPS();
	const u8 a = 160;
	s32 x = 27, y = 13;
	u32 color;
	char buffer[16];

	if (fps <= 30.f) {
		// red -> yellow
		color = 0xff000000 | a | ((u32)((fps / 30.f) * 255.f) << 16);
	} else if (fps <= 60.f) {
		// yellow -> green
		color = 0x00ff0000 | a | ((u32)((1.f - (fps - 30.f) / 30.f) * 255.f) << 24);
	} else if (fps <= 90.f) {
		// green -> cyan
		color = 0x00ff0000 | a | ((u32)(((fps - 60.f) / 30.f) * 255.f) << 8);
	} else {
		// cyan
		color = 0x00ffff00 | a;
	}

	if (g_CharsNumeric && g_FontNumeric) {
		snprintf(buffer, sizeof buffer, "%.2f", fps);

		gSPSetExtraGeometryModeEXT(gdl++, g_HudAlignModeL);

		gdl = text0f153628(gdl);
		gdl = textRender(gdl, &x, &y, buffer, g_CharsNumeric, g_FontNumeric, color, 0x000000a0, viGetWidth(), viGetHeight(), 0, 0);
		gdl = text0f153780(gdl);

		gSPClearExtraGeometryModeEXT(gdl++, g_HudAlignModeL);
	}

	return gdl;
}
#endif

/**
 * Renders a complete frame for all players, and also does some other game logic
 * that really doesn't belong here.
 *
 * This function is pretty big, so here's an overview of its structure:
 *
 * if (stage == STAGE_TITLE) {
 *     // title screen rendering
 * } else if (stage == STAGE_BOOTPAKMENU) {
 *     // boot pak menu rendering
 * } else if (stage == STAGE_4MBMENU) {
 *     // 4MB menu rendering
 * } else if (stage == STAGE_CREDITS) {
 *     // credits rendering
 * } else {
 *     for (i = 0; i < numplayers; i++) {
 *         // rendering and logic per player
 *     }
 * }
 * // logic for auto-playing cutscene advancement
 *
 * The player loop takes up the majority of the function. In addition to
 * rendering the scene and HUD, it also handles the following logic:
 * - decreasing dizziness
 * - detecting if the prop being looked at is still valid
 * - pressing Z when using eyespy
 * - opening doors and reloading
 * - random static in the Infiltration intro cutscene
 * - combat boost activation and reverting
 */
Gfx *lvRender(Gfx *gdl)
{
	gSPSegment(gdl++, SPSEGMENT_PHYSICAL, 0x00000000);

#if VERSION >= VERSION_NTSC_1_0
	func0f0d5a7c();
#endif

	if (g_Vars.stagenum == STAGE_TITLE
			|| (g_Vars.stagenum == STAGE_TEST_OLD && titleIsKeepingMode())) {
		gSPDisplayList(gdl++, &var800613a0);

		if (debugIsZBufferDisabled()) {
			gSPDisplayList(gdl++, &var80061360);
		} else {
			gSPDisplayList(gdl++, &var80061380);
		}

		gdl = viPrepareZbuf(gdl);
		gdl = vi0000b1d0(gdl);

		gDPSetScissorFrac(gdl++, 0,
				viGetViewLeft() * 4.0f, viGetViewTop() * 4.0f,
				(viGetViewLeft() + viGetViewWidth()) * 4.0f,
				(viGetViewTop() + viGetViewHeight()) * 4.0f);

		gdl = titleRender(gdl);
		gdl = lvRenderFade(gdl);
	} else if (g_Vars.stagenum == STAGE_BOOTPAKMENU) {
		gSPClipRatio(gdl++, FRUSTRATIO_2);
		gSPDisplayList(gdl++, &var800613a0);
		gSPDisplayList(gdl++, &var80061380);

		setCurrentPlayerNum(0);
		viSetViewPosition(g_Vars.currentplayer->viewleft, g_Vars.currentplayer->viewtop);
		viSetFovAspectAndSize(g_Vars.currentplayer->fovy, g_Vars.currentplayer->aspect,
				g_Vars.currentplayer->viewwidth, g_Vars.currentplayer->viewheight);
		mtx00016748(1);

		gdl = vi0000b1d0(gdl);
		gdl = viRenderViewportEdges(gdl);
		gdl = bgScissorToViewport(gdl);
		gdl = menuRender(gdl);
	} else if (g_Vars.stagenum == STAGE_4MBMENU) {
		gSPClipRatio(gdl++, FRUSTRATIO_2);
		gSPDisplayList(gdl++, &var800613a0);
		gSPDisplayList(gdl++, &var80061380);

		setCurrentPlayerNum(0);

#if VERSION >= VERSION_PAL_BETA
		viSetMode(VIMODE_LO);
		viSetViewPosition(g_Vars.currentplayer->viewleft, g_Vars.currentplayer->viewtop);
		viSetSize(playerGetFbWidth(), playerGetFbHeight());
		viSetBufSize(playerGetFbWidth(), playerGetFbHeight());
		viSetViewSize(playerGetFbWidth(), playerGetFbHeight());
#else
		viSetViewPosition(g_Vars.currentplayer->viewleft, g_Vars.currentplayer->viewtop);
#endif

		viSetFovAspectAndSize(g_Vars.currentplayer->fovy, g_Vars.currentplayer->aspect,
				g_Vars.currentplayer->viewwidth, g_Vars.currentplayer->viewheight);

		mtx00016748(1);

		gdl = vi0000b1d0(gdl);
		gdl = bgScissorToViewport(gdl);
		gdl = menuRender(gdl);

		if (g_Vars.currentplayer->pausemode != PAUSEMODE_UNPAUSED) {
			playerTickPauseMenu();
		}
	} else if (g_Vars.stagenum == STAGE_CREDITS) {
		gSPClipRatio(gdl++, FRUSTRATIO_2);
		gSPDisplayList(gdl++, &var800613a0);
		gSPDisplayList(gdl++, &var80061380);

		setCurrentPlayerNum(0);
		viSetViewPosition(g_Vars.currentplayer->viewleft, g_Vars.currentplayer->viewtop);
		viSetFovAspectAndSize(g_Vars.currentplayer->fovy, g_Vars.currentplayer->aspect,
				g_Vars.currentplayer->viewwidth, g_Vars.currentplayer->viewheight);
		mtx00016748(1);

		gdl = vi0000b1a8(gdl);
		gdl = vi0000b1d0(gdl);
		gdl = viRenderViewportEdges(gdl);
		gdl = creditsDraw(gdl);
	} else {
		// Normal stages
		s32 i;
		s32 playercount;
		Gfx *savedgdl;
#if VERSION >= VERSION_NTSC_1_0
		bool forcesingleplayer = (g_Vars.coopplayernum >= 0 || g_Vars.antiplayernum >= 0)
			&& playerHasSharedViewport();
#else
		bool forcesingleplayer = (g_Vars.coopplayernum >= 0 || g_Vars.antiplayernum >= 0)
			&& ((g_InCutscene && !g_MainIsEndscreen) || menuGetRoot() == MENUROOT_COOPCONTINUE);
#endif
		struct player *player;
		struct chrdata *chr;

#ifndef PLATFORM_N64
		if (g_NetMode) {
			// tick all players, we'll skip the rendering
			forcesingleplayer = false;
		}
		// Demo playback (offline) of a multi-player recording spawns one local
		// player per recorded human (would splitscreen). Always render a single
		// fullscreen viewport following the chosen combatant; netDemoRenderBegin
		// drives the camera (a future UI will switch which combatant is followed).
		if (netDemoIsPlaying()) {
			forcesingleplayer = true;
		}
#endif

		playercount = forcesingleplayer ? 1 : PLAYERCOUNT();

		gSPClipRatio(gdl++, FRUSTRATIO_2);

		for (i = 0; i < playercount; i++) {
			bool islastplayer;
			u32 bluramount = 0;

			savedgdl = gdl;

			if (forcesingleplayer) {
				setCurrentPlayerNum(0);
				g_Vars.currentplayerindex = 0;
				islastplayer = true;
			} else {
				s32 nextplayernum = i + 1;
				setCurrentPlayerNum(playermgrGetPlayerAtOrder(i));
				islastplayer = playercount == nextplayernum;

#ifndef PLATFORM_N64
				// Client spectator (render redirect): when this iteration is our
				// OWN pawn's viewport and we're spectating a live player target,
				// render the TARGET's player slot here instead, so we get their
				// full first-person frame (HUD / aim / weapon) — the clean
				// JIP-reconnect-spectator behaviour made deliberate. The target is a
				// real combatant, so currentplayerindex stays a valid player slot
				// (unlike the fake-player spectator panels, which is why those
				// break). Copy our full-screen viewport onto the target first; a
				// remote player's own view* can be stale/small on the client.
				// Gated to: any net session with a local pawn (a playing CLIENT
				// or the playing HOST — a host /spec'ing a client needs this path
				// too), and our own viewport iteration. The spectator-host (panels)
				// has player==NULL so it's excluded.
				//
				// IMPORTANT: never dereference g_NetSpectateChr — it can dangle. The
				// target client may leave (chr orphaned, freed at the next stage) or
				// the chr may be torn down at round-end, and ending the match while
				// spectating then runs this render body's gameplay on freed memory
				// (the eyespy crash). So find the target by POINTER-comparing it
				// against each live player slot's chr. If it matches no live,
				// CONNECTED combatant slot (left / died / freed / orphan / a sim),
				// tnum stays -1 and we render our own viewport. The ->client test
				// drops orphan slots (disconnected client whose chr lingers until
				// the next stage) — those crash the per-player gameplay below.
				// JIP spectator variant: a mid-match joiner has NO own pawn
				// (player == NULL until the round boundary), so there's no
				// "own viewport iteration" — substitute on render order 0
				// instead: in net mode every order EXCEPT 0 has its display
				// list rolled back after the body (the `gdl = savedgdl`
				// further down), so order 0 is the only one that displays.
				const bool jipspec = g_NetMode == NETMODE_CLIENT && g_NetLocalClient
						&& !g_NetLocalClient->player && g_NetLocalClient->is_spectator;
				// Player killer: render the killer's own first-person viewport (so
				// their gun viewmodel + your body both show). The killcam overrides
				// the killer player's cam_pos/look/up to the RECORDED basis in
				// netKillcamRenderBegin, so this renders the historical view, not the
				// killer's live aim. (A SIM killer isn't a player slot, so this
				// redirect doesn't fire for it — it uses netSpectateApply's eye-cam.)
				if (g_NetMode && g_NetSpectateChr && g_NetLocalClient
						&& ((g_NetLocalClient->player
								&& g_Vars.currentplayernum == g_NetLocalClient->playernum)
							|| (jipspec && i == 0))) {
					s32 tnum = -1;
					for (s32 pn = 0; pn < MAX_PLAYERS; ++pn) {
						if (g_Vars.players[pn] && g_Vars.players[pn]->client
								&& g_Vars.players[pn]->prop
								&& g_Vars.players[pn]->prop->chr == g_NetSpectateChr) {
							tnum = pn;
							break;
						}
					}
					if (tnum >= 0 && (jipspec || tnum != g_NetLocalClient->playernum)) {
						struct player *dst = g_Vars.players[tnum];
						if (jipspec) {
							// No own pawn to copy a viewport from. A net
							// client is always single-view (playerGetLocalCount
							// returns 1), so the playerGetViewport* layout
							// helpers return the full-screen values here.
							dst->viewleft = playerGetViewportLeft();
							dst->viewtop = playerGetViewportTop();
							dst->viewwidth = playerGetViewportWidth();
							dst->viewheight = playerGetViewportHeight();
						} else {
							struct player *src = g_NetLocalClient->player;
							dst->viewleft = src->viewleft;
							dst->viewtop = src->viewtop;
							dst->viewwidth = src->viewwidth;
							dst->viewheight = src->viewheight;
							dst->fovy = src->fovy;
							dst->aspect = src->aspect;
						}
						// Substitute the target into currentplayer/num/stats so this
						// viewport renders the target's first-person frame, but KEEP
						// this iteration's render order in currentplayerindex.
						// setCurrentPlayerNum recomputes currentplayerindex to the
						// TARGET's order; if the redirected iteration was order 0 (the
						// host's own pawn normally is), that moves index 0 off this
						// frame entirely. Every "first time this frame" block keyed on
						// currentplayerindex==0 — propsTickPlayer's NOTYETTICKED re-arm
						// (prop.c), bgTickRooms ONSCREEN, player init — then never
						// fires, so NO prop/sim ever ticks and the whole world freezes
						// (the lvupdate clock still runs). This is the render-redirect
						// twin of the panel-path reorder in spectator.c. Restoring the
						// iteration's index keeps the substitution transparent to the
						// once-per-frame bookkeeping (each render order is still visited
						// exactly once); only the rendered viewpoint changes.
						const s32 savedplayerindex = g_Vars.currentplayerindex;
						setCurrentPlayerNum(tnum);
						g_Vars.currentplayerindex = savedplayerindex;
					}
				}
#endif
			}

#ifndef PLATFORM_N64
			// Spectator panel: the rest of this loop body assumes the player
			// has a chr / prop / weapons / HUD. Spectator panels have none —
			// dispatch to the minimal world-only renderer and skip ahead.
			if (g_Vars.currentplayer && g_Vars.currentplayer->is_spectator) {
				gdl = spectatorRenderPanel(gdl);
				continue;
			}
			// Spectator host: combatant slots in g_Vars.players[] exist only
			// to give remote clients a struct player for wire-update binding;
			// they're never rendered locally on the host. Without this skip
			// the loop would draw the combatant's first-person view into a
			// quadrant, painting over the spectator panel that should be
			// there.
			if (g_NetMode == NETMODE_SERVER && g_NetLocalClient && g_NetLocalClient->is_spectator
					&& g_StageNum != STAGE_CITRAINING) {
				continue;
			}

			// Pawn-less net combatant slot (crash ledger #24): a client's pawn
			// can be torn down between tick and render - a round-transition race
			// (#20/#21) or a wire message that freed the local pawn. The rest of
			// this loop body dereferences currentplayer->prop->chr unconditionally;
			// skip this slot's view for the frame rather than NULL-deref it. The
			// client-spectator redirect above already swapped a LIVE target into
			// currentplayer, so this only trips on a genuinely pawn-less slot,
			// never while spectating.
			if (g_NetMode != NETMODE_NONE
					&& (!g_Vars.currentplayer || !g_Vars.currentplayer->prop
						|| !g_Vars.currentplayer->prop->chr)) {
				continue;
			}
#endif

			// Calculate bluramount - this will be used later
			if (g_Vars.tickmode != TICKMODE_CUTSCENE) {
				player = g_Vars.currentplayer;
				chr = player->prop->chr;

				if (chr->blurdrugamount > 0
						&& !g_Vars.currentplayer->invincible
						&& !g_Vars.currentplayer->training) {
					bluramount = (chr->blurdrugamount * 130) / TICKS(5000) + 100;

					if (bluramount > 230) {
						bluramount = 230;
					}

					if (chr->blurdrugamount > TICKS(5000)) {
						chr->blurdrugamount = TICKS(5000);
					}

					chr->blurdrugamount -= g_Vars.lvupdate60 * (chr->blurnumtimesdied + 1);

					if (chr->blurdrugamount < 1) {
						chr->blurdrugamount = 0;
						chr->blurnumtimesdied = 0;
					}

#ifndef PLATFORM_N64
					// reset the drug blur to 0 if it's disabled in MP settings
					if (g_Vars.mplayerisrunning && (g_MpSetup.options & MPOPTION_NODRUGBLUR)) {
						bluramount = 0;
					}
#endif
				}
			}

			bviewSetMotionBlur(bluramount);

			gSPDisplayList(gdl++, &var800613a0);

			if (debugIsZBufferDisabled()) {
				gSPDisplayList(gdl++, &var80061360);
			} else {
				gSPDisplayList(gdl++, &var80061380);
			}

			viSetViewPosition(g_Vars.currentplayer->viewleft, g_Vars.currentplayer->viewtop);
			viSetFovAspectAndSize(g_Vars.currentplayer->fovy, g_Vars.currentplayer->aspect,
					g_Vars.currentplayer->viewwidth, g_Vars.currentplayer->viewheight);
			mtx00016748(g_Vars.currentplayerstats->scale_bg2gfx);
			envTick();
			zbufSwap();
			gdl = viPrepareZbuf(gdl);
			gdl = vi0000b1d0(gdl);
			gdl = bgScissorToViewport(gdl);
			artifactsClear();

			if ((g_Vars.stagenum != STAGE_CITRAINING || (var80087260 <= 0 && g_MenuData.root != MENUROOT_MPSETUP))
					&& g_Vars.lvframenum <= 5
					&& !g_Vars.normmplayerisrunning
					&& g_Vars.tickmode != TICKMODE_CUTSCENE) {
				if (var80084050 < 6) {
					g_Vars.lockscreen = 1;
				}

				var80084050++;
			} else if (g_Vars.currentplayer->gunctrl.loadall
					&& var80075d60 == 2
					&& g_Vars.currentplayer->cameramode != CAMERAMODE_THIRDPERSON
					&& g_Vars.currentplayer->cameramode != CAMERAMODE_EYESPY
					&& var8009dfc0 == 0) {
				g_Vars.currentplayer->gunctrl.loadall = bgunLoadAll();
			}

			if (g_Vars.lockscreen) {
				gdl = bviewDrawMotionBlur(gdl, 0xffffffff, 255);
				g_Vars.lockscreen--;
			} else if (var8009dfc0) {
				gdl = viRenderViewportEdges(gdl);
				gdl = bgScissorToViewport(gdl);
				mtx00016748(1);

				if (g_Vars.currentplayer->menuisactive) {
					gdl = menuRender(gdl);
				}
			} else {
				if (var80075d60 == 2) {
					gdl = playerUpdateShootRot(gdl);
				}

				gdl = viRenderViewportEdges(gdl);
				gdl = skyRender(gdl);
				bgTick();
				lightsTick();
				propsTickPlayer(islastplayer);
				scenarioTickChr(NULL);
				propsSort();
				autoaimTick();
				handsTickAttack();

#ifndef PLATFORM_N64
				// glares calculated earlier on PC, before prop matrices turn into garbage
				bgCalculateGlaresForVisibleRooms();
#endif

				// Calculate lookingatprop
				if (PLAYERCOUNT() == 1
						|| g_Vars.coopplayernum >= 0
						|| g_Vars.antiplayernum >= 0
						|| (weaponHasFlag(bgunGetWeaponNum(HAND_RIGHT), WEAPONFLAG_AIMTRACK) && bmoveIsInSightAimMode())) {
					g_Vars.currentplayer->lookingatprop.prop = propFindAimingAt(HAND_RIGHT, false, FINDPROPCONTEXT_QUERY);

					if (g_Vars.currentplayer->lookingatprop.prop) {
						if (g_Vars.currentplayer->lookingatprop.prop->type == PROPTYPE_CHR
								|| g_Vars.currentplayer->lookingatprop.prop->type == PROPTYPE_PLAYER) {
							chr = g_Vars.currentplayer->lookingatprop.prop->chr;

							if ((chr->hidden & CHRHFLAG_CLOAKED) && !USINGDEVICE(DEVICE_IRSCANNER)) {
								g_Vars.currentplayer->lookingatprop.prop = NULL;
							}
						} else if (g_Vars.currentplayer->lookingatprop.prop->type == PROPTYPE_OBJ
								|| g_Vars.currentplayer->lookingatprop.prop->type == PROPTYPE_WEAPON
								|| g_Vars.currentplayer->lookingatprop.prop->type == PROPTYPE_DOOR) {
							struct defaultobj *obj = g_Vars.currentplayer->lookingatprop.prop->obj;

							if ((obj->flags3 & OBJFLAG3_REACTTOSIGHT) == 0) {
								if (g_Vars.stagenum != STAGE_CITRAINING
										|| (obj->modelnum != MODEL_TARGET
											&& obj->modelnum != MODEL_CIHUB
											&& obj->modelnum != MODEL_COMHUB)) {
									g_Vars.currentplayer->lookingatprop.prop = NULL;
								}
							}
						} else {
							g_Vars.currentplayer->lookingatprop.prop = NULL;
						}
					}
				} else {
					g_Vars.currentplayer->lookingatprop.prop = NULL;
				}

				if (gsetHasFunctionFlags(&g_Vars.currentplayer->hands[0].gset, FUNCFLAG_THREATDETECTOR)) {
					lvFindThreats();
				} else if (weaponHasFlag(bgunGetWeaponNum(HAND_RIGHT), WEAPONFLAG_AIMTRACK)) {
					s32 j;

					if (frIsInTraining()
							&& g_Vars.currentplayer->lookingatprop.prop
							&& bmoveIsInSightAimMode()) {
						func0f1a0924(g_Vars.currentplayer->lookingatprop.prop);
					} else if (lvUpdateTrackedProp(&g_Vars.currentplayer->lookingatprop, -1) == 0) {
						g_Vars.currentplayer->lookingatprop.prop = NULL;
					}

					for (j = 0; j < ARRAYCOUNT(g_Vars.currentplayer->trackedprops); j++) {
						if (!lvUpdateTrackedProp(&g_Vars.currentplayer->trackedprops[j], j)) {
							g_Vars.currentplayer->trackedprops[j].x1 = -1;
							g_Vars.currentplayer->trackedprops[j].x2 = -2;
						}
					}
				}

				// Handle eyespy Z presses
				// eyespy->prop NULL-check: the block below dereferences
				// eyespy->prop (dart fire / bomb detonation) unguarded. For a
				// local player an active eyespy always has a prop, but the client
				// spectator render-redirect runs this body with currentplayer set
				// to a spectated REMOTE target, whose eyespy can be a stale pointer
				// (non-NULL + camerabuttonheld but prop freed) during round-end
				// teardown -> 0xc0000005. Always true on N64, so behaviour there is
				// unchanged.
				if (g_Vars.currentplayer->eyespy
						&& g_Vars.currentplayer->eyespy->prop
						&& (g_Vars.currentplayer->devicesactive & ~g_Vars.currentplayer->devicesinhibit & DEVICE_EYESPY)
						&& g_Vars.currentplayer->eyespy->camerabuttonheld) {
					if (g_Vars.currentplayer->eyespy->mode == EYESPYMODE_CAMSPY) {
						objectiveCheckHolograph(400);
						sndStart(var80095200, SFX_CAMSPY_SHUTTER, 0, -1, -1, -1, -1, -1);
					} else if (g_Vars.currentplayer->eyespy->mode == EYESPYMODE_DRUGSPY) {
						if (g_Vars.currentplayer->eyespydarts) {
							// Fire dart
							struct coord direction;
							sndStart(var80095200, SFX_DRUGSPY_FIREDART, 0, -1, -1, -1, -1, -1);
							g_Vars.currentplayer->eyespydarts--;

							direction.x = g_Vars.currentplayer->eyespy->look.x;
							direction.y = g_Vars.currentplayer->eyespy->look.y;
							direction.z = g_Vars.currentplayer->eyespy->look.z;

							projectileCreate(g_Vars.currentplayer->eyespy->prop, 0,
									&g_Vars.currentplayer->eyespy->prop->pos, &direction, WEAPON_TRANQUILIZER, NULL);
						} else {
							// No dart ammo
							sndStart(var80095200, SFX_FIREEMPTY, 0, -1, -1, -1, -1, -1);
						}
					} else { // EYESPYMODE_BOMBSPY
						struct coord vel = {0, 0, 0};
						struct gset gset = {WEAPON_GRENADE, 0, 0, FUNC_PRIMARY};
						explosionCreateSimple(g_Vars.currentplayer->eyespy->prop,
								&g_Vars.currentplayer->eyespy->prop->pos,
								g_Vars.currentplayer->eyespy->prop->rooms,
								EXPLOSIONTYPE_DRAGONBOMBSPY, 0);
						chrBeginDeath(g_Vars.currentplayer->eyespy->prop->chr, &vel, 0, 0, &gset, false, 0);
					}
				}

				// Handle opening doors and reloading
				if (g_Vars.currentplayer->bondactivateorreload & JO_ACTION_ACTIVATE) {
					if (!currentPlayerInteract(false)) {
#ifndef PLATFORM_N64
						// n64 behavior: interact sucessful, cancel reload
						if (!PLAYER_EXTCFG().extcontrols || PLAYER_EXTCFG().usereloads) {
							g_Vars.currentplayer->bondactivateorreload = (g_Vars.currentplayer->bondactivateorreload & ~JO_ACTION_RELOAD);
						}
#endif
					}
				} else if (g_Vars.currentplayer->eyespy
						&& g_Vars.currentplayer->eyespy->active
						&& g_Vars.currentplayer->eyespy->opendoor) {
					currentPlayerInteract(true);
					g_Vars.currentplayer->bondactivateorreload = (g_Vars.currentplayer->bondactivateorreload & ~JO_ACTION_RELOAD);
				}

				if (g_Vars.currentplayer->bondactivateorreload & JO_ACTION_RELOAD) {
					if (g_Vars.currentplayer->hands[HAND_RIGHT].state != HANDSTATE_RELOAD) {
						bgunReloadIfPossible(HAND_RIGHT);
					}
					if (g_Vars.currentplayer->hands[HAND_LEFT].state != HANDSTATE_RELOAD) {
						bgunReloadIfPossible(HAND_LEFT);
					}
					g_Vars.currentplayer->bondactivateorreload = (g_Vars.currentplayer->bondactivateorreload & ~JO_ACTION_RELOAD);
				}

				propsTestForPickup();

				gdl = bgRender(gdl);
				chr0f028498(var80075d68 == 15 || g_AnimHostEnabled);
				gdl = propsRenderBeams(gdl);
				gdl = shardsRender(gdl);
				gdl = sparksRender(gdl);
				gdl = weatherRender(gdl);

				if (g_NbombsActive) {
					gdl = nbombsRender(gdl);
				}

				if (var80075d60 == 2) {
					gdl = playerRenderHud(gdl);

#ifdef DEBUG
					gdl = lvRenderManPosIfEnabled(gdl);
#endif
				} else {
					gdl = boltbeamsRender(gdl);

					if (g_Vars.currentplayer->visionmode != VISIONMODE_XRAY) {
						gdl = bgRenderArtifacts(gdl);
					}

#ifndef PLATFORM_N64
					// Player HUD is removed this frame: animate the vanity egg
					// banners away (their fade-in/hold render from playerRenderHud).
					gdl = netCoopEggsRenderHidden(gdl);
#endif
				}

				if (g_DebugScreenshotRgb <= 0) {
					static struct sndstate *g_CutsceneStaticAudioHandle = NULL;
					static s32 g_CutsceneStaticTimer = 100;
					static u8 g_CutsceneStaticActive = false;
					bool cutscenehasstatic = false;
					u32 alpha;

					if (g_Vars.tickmode == TICKMODE_CUTSCENE) {
						// This chunk of code is unreachable
						// (STAGE_TEST_OLD is not used)
#if VERSION < VERSION_PAL_BETA
						if (g_Vars.stagenum == STAGE_TEST_OLD) {
							f32 frac = 0;
							u32 colour;
							s32 endframe = animGetNumFrames(g_CutsceneAnimNum) - 1;

							colour = 0;

							if (g_CutsceneCurAnimFrame60 < 90) {
								frac = 1.0f - (f32)g_CutsceneCurAnimFrame60 / 90.0f;
							}

							if (g_CutsceneAnimNum != ANIM_CUT_OLD_TITLE_CAM_04) {
								if (g_CutsceneCurAnimFrame60 > endframe - 90) {
									frac = (g_CutsceneCurAnimFrame60 - endframe + 90) / 90.0f;
								}
							} else {
								if (g_CutsceneCurAnimFrame60 > endframe - 30) {
									colour = 0xffffff00;
									frac = (g_CutsceneCurAnimFrame60 - endframe + 30) / 30.0f;
								}
							}

							if (frac > 0) {
								alpha = 255 * frac;

								gDPPipeSync(gdl++);
								gDPSetRenderMode(gdl++, G_RM_CLD_SURF, G_RM_CLD_SURF2);
								gDPSetCombineMode(gdl++, G_CC_PRIMITIVE, G_CC_PRIMITIVE);
								gDPSetPrimColorViaWord(gdl++, 0, 0, colour | alpha);

								gDPFillRectangle(gdl++,
									viGetViewLeft(),
									viGetViewTop(),
									viGetViewLeft() + viGetViewWidth(),
									viGetViewTop() + viGetViewHeight());

								gdl = text0f153838(gdl);
							}
						}
#endif

						// Handle visual effects in cutscenes
						switch (g_CutsceneAnimNum) {
						case ANIM_CUT_CAVE_INTRO_CAM:
							// Horizon scanner in Air Base intro
							if (g_CutsceneCurAnimFrame60 > 839 && g_CutsceneCurAnimFrame60 < 1411) {
								gdl = bviewDrawHorizonScanner(gdl);
							}
							break;
						case ANIM_CUT_LUE_INTRO_CAM_01:
						case ANIM_CUT_LUE_INTRO_CAM_02:
						case ANIM_CUT_LUE_INTRO_CAM_03:
							{
								// Show static randomly in Infiltration intro
								s32 cutscenestatic = 0;
								cutscenehasstatic = true;

								if (g_CutsceneStaticAudioHandle == NULL) {
									sndStart(var80095200, SFX_INFIL_STATIC_LONG, &g_CutsceneStaticAudioHandle, -1, -1, -1, -1, -1);
								}

								g_CutsceneStaticTimer -= g_Vars.diffframe60;

								if (g_CutsceneStaticTimer < 0) {
									g_CutsceneStaticTimer = rngRandom() % TICKS(200) + TICKS(40);
									g_CutsceneStaticActive = false;
								}

								gdl = bviewDrawFilmInterlace(gdl, 0xffffffff, 0xffffffff);

								if (g_CutsceneStaticTimer < TICKS(15)) {
									if (g_CutsceneStaticActive == false) {
										g_CutsceneStaticActive = true;
										sndStart(var80095200, SFX_INFIL_STATIC_MEDIUM, NULL, -1, -1, -1, -1, -1);
									}

									cutscenestatic = 225 - g_CutsceneStaticTimer * PALUP(10);
								}

								// Consider a single frame of static, separate
								// to the main static above
								if (rngRandom() % 60 == 1) {
									cutscenestatic = 255;
									sndStart(var80095200, SFX_INFIL_STATIC_SHORT, NULL, -1, -1, -1, -1, -1);
								}

								if (cutscenestatic) {
									gdl = bviewDrawStatic(gdl, 0xffffffff, cutscenestatic);
								}
							}
							break;
						}
					}

					if (g_CutsceneStaticAudioHandle && !cutscenehasstatic) {
						audioStop(g_CutsceneStaticAudioHandle);
					}

					// Slayer rocket shows static when flying out of bounds
					if (g_Vars.currentplayer->visionmode == VISIONMODE_SLAYERROCKET
							&& g_Vars.tickmode != TICKMODE_CUTSCENE) {
						gdl = bviewDrawSlayerRocketInterlace(gdl, 0xffffffff, 0xffffffff);

						if (g_Vars.currentplayer->badrockettime > 0) {
							u32 slayerstatic = g_Vars.currentplayer->badrockettime * 255 / TICKS(90);

							if (slayerstatic > 255) {
								slayerstatic = 255;
							}

							gdl = bviewDrawStatic(gdl, 0x4fffffff, slayerstatic);
						}
					}

#if VERSION >= VERSION_NTSC_1_0
					if (g_Vars.currentplayer->visionmode == VISIONMODE_SLAYERROCKETSTATIC) {
						gdl = bviewDrawStatic(gdl, 0x4fffffff, 255);
						g_Vars.currentplayer->visionmode = VISIONMODE_NORMAL;
					}
#endif

					if (g_Vars.currentplayer->visionmode == VISIONMODE_XRAY
							&& g_Vars.tickmode != TICKMODE_CUTSCENE) {
						s32 xraything = 99;

						if (g_Vars.currentplayer->erasertime < TICKS(200)) {
#if PAL
							xraything = 249 - ((g_Vars.currentplayer->erasertime * 180 / 50) >> 2);
#else
							xraything = 249 - (g_Vars.currentplayer->erasertime * 3 >> 2);
#endif
						}

						gdl = bviewDrawZoomBlur(gdl, 0xffffffff, xraything, 1.05f, 1.05f);
					}

					// Handle combat boosts
					if ((g_Vars.speedpillchange > 0 && g_Vars.speedpillchange < (PAL ? 26 : 30))
							|| (g_Vars.speedpillwant && !g_Vars.speedpillon)
							|| (!g_Vars.speedpillwant && g_Vars.speedpillon)) {
						if (g_Vars.speedpillchange == (PAL ? 26 : 30) && !g_Vars.speedpillwant) {
							sndStart(var80095200, lvGetSlowMotionType() ? SFX_JO_BOOST_ACTIVATE : SFX_ARGH_JO_02AD, 0, -1, -1, -1, -1, -1);
						}

						if (g_Vars.speedpillchange < (PAL ? 13 : 15)) {
							gdl = bviewDrawZoomBlur(gdl, 0xffffffff,
									g_Vars.speedpillchange * 180 / (PAL ? 13 : 15),
									(f32)g_Vars.speedpillchange * (PAL ? 0.023076923564076f : 0.02000000141561f) + 1.1f,
									(f32)g_Vars.speedpillchange * (PAL ? 0.023076923564076f : 0.02000000141561f) + 1.1f);
							gdl = playerDrawFade(gdl, 0xff, 0xff, 0xff,
									g_Vars.speedpillchange * (PAL ? 0.0076923076994717f : 0.0066666668280959f));
						} else {
							gdl = bviewDrawZoomBlur(gdl, 0xffffffff,
									((PAL ? 26 : 30) - g_Vars.speedpillchange) * 180 / (PAL ? 13 : 15),
									(f32)((PAL ? 26 : 30) - g_Vars.speedpillchange) * (PAL ? 0.023076923564076f : 0.02000000141561f) + 1.1f,
									(f32)((PAL ? 26 : 30) - g_Vars.speedpillchange) * (PAL ? 0.023076923564076f : 0.02000000141561f) + 1.1f);
							gdl = playerDrawFade(gdl, 0xff, 0xff, 0xff,
									((PAL ? 26.0f : 30.0f) - g_Vars.speedpillchange) * (PAL ? 0.0076923076994717f : 0.0066666668280959f));
						}

						if (g_Vars.currentplayernum == 0) {
							if (g_Vars.speedpillwant) {
								g_Vars.speedpillchange++;
							} else {
								g_Vars.speedpillchange--;
							}
						}

						if (g_Vars.speedpillchange > (PAL ? 26 : 30)) {
							g_Vars.speedpillchange = (PAL ? 26 : 30);
						} else if (g_Vars.speedpillchange < 0) {
							g_Vars.speedpillchange = 0;
						}
					}

					if (g_Vars.speedpillchange > (PAL ? 13 : 15)) {
						g_Vars.speedpillon = true;
					} else {
						g_Vars.speedpillon = false;
					}

					if (bluramount) {
						bviewClearMotionBlur();
						gdl = bviewDrawMotionBlur(gdl, 0xffffffff, bluramount);
					}

					// Handle blur effect in cutscenes (Extraction intro?)
					if (g_Vars.tickmode == TICKMODE_CUTSCENE) {
						f32 cutsceneblurfrac = playerGetCutsceneBlurFrac();

						if (cutsceneblurfrac > 0) {
#if VERSION < VERSION_PAL_BETA
							u32 stack;
#endif
							gdl = bviewDrawMotionBlur(gdl, 0xffffff00, cutsceneblurfrac * 255);
						}
					}

#if VERSION >= VERSION_PAL_FINAL
					if (bluramount);
					if (bluramount);
					if (bluramount);
#elif VERSION >= VERSION_NTSC_1_0
					if (bluramount);
					if (bluramount);
#else
					if (bluramount);
					if (bluramount);
					if (bluramount);
#endif

					if (debugGetMotionBlur() == 1) {
						gdl = bviewDrawMotionBlur(gdl, 0xffffff00, 128);
					} else if (debugGetMotionBlur() == 2) {
						gdl = bviewDrawMotionBlur(gdl, 0xffffff00, 192);
					} else if (debugGetMotionBlur() == 3) {
						gdl = bviewDrawMotionBlur(gdl, 0xffffff00, 230);
					}

					// Render white when teleporting
					if (g_Vars.currentplayer->teleportstate > TELEPORTSTATE_INACTIVE) {
						alpha = 0;

						if (g_Vars.currentplayer->teleportstate == TELEPORTSTATE_WHITE) {
							alpha = 255;
						}

						if (g_Vars.currentplayer->teleportstate == TELEPORTSTATE_EXITING
								&& g_Vars.currentplayer->teleporttime < 16) {
							alpha = -g_Vars.currentplayer->teleporttime * 16 + 240;
						}

						if (g_Vars.currentplayer->teleportstate == TELEPORTSTATE_ENTERING) {
							if (g_Vars.currentplayer->teleporttime > 32) {
								alpha = g_Vars.currentplayer->teleporttime * 16 - 512;
							}

							if (g_Vars.currentplayer->teleporttime == 48) {
								alpha = 255;
							}
						}

						if (alpha) {
							gdl = text0f153628(gdl);
							gdl = text0f153a34(gdl,
									viGetViewLeft(), viGetViewTop(),
									viGetViewLeft() + viGetViewWidth(),
									viGetViewTop() + viGetViewHeight(), 0xffffff00 | alpha);
							gdl = text0f153780(gdl);
						}
					}
				}

#if VERSION >= VERSION_NTSC_1_0
				gdl = scenarioRenderHud(gdl);
				gdl = lvRenderFade(gdl);
#else
				gdl = lvRenderFade(gdl);
				gdl = scenarioRenderHud(gdl);
#endif

				if (g_FrIsValidWeapon) {
					gdl = frRenderHud(gdl);
				}

				if (debugGetTilesDebugMode() != 0
						|| debugGetPadsDebugMode() != 0
						|| debug0f11eea8()
						|| debug0f11ef80()
						|| debugIsChrStatsEnabled()
						|| debug0f11ee40()) {
#if VERSION < VERSION_NTSC_1_0
					RoomNum spc8[21];
					RoomNum spb0[11];
					RoomNum sp9c[10];
					s32 j;

					sp9c[0] = g_Vars.currentplayer->memcamroom;
					sp9c[1] = -1;

					for (j = 0; sp9c[j] != -1; j++) {
						spc8[j] = sp9c[j];
					}

					spc8[j] = -1;

					for (j = 0; sp9c[j] != -1; j++) {
						bgRoomGetNeighbours(sp9c[j], spb0, 10);
						roomsAppend(spb0, spc8, 20);
					}

					if (debugIsChrStatsEnabled()) {
						gdl = chrsRenderChrStats(gdl, spc8);
					}
#endif
				}

				gdl = skyRenderOverexposure(gdl);
				gdl = amRender(gdl);
				mtx00016748(1);

				if (g_Vars.currentplayer->menuisactive) {
					gdl = menuRender(gdl);
				}

				mtx00016748(g_Vars.currentplayerstats->scale_bg2gfx);

				if (g_Vars.mplayerisrunning) {
					gdl = mpRenderModalText(gdl);
				}

				if (g_Vars.currentplayer->dostartnewlife) {
#ifndef PLATFORM_N64
					if (g_NetMode != NETMODE_CLIENT)
#endif
						playerStartNewLife();
				}
			}

			artifactsTick();

#ifndef PLATFORM_N64
			// Net mode: only the LOCAL player's render order actually
			// displays — every other slot is ticked then rolled back. This
			// was hard-coded to order 0 (the local player always sat at slot
			// 0 via the netPlayersAllocate swap), but a co-op drop-in
			// claimant binds at its wire slot N without the load-time swap,
			// so key on the local binding instead. Playerless spectators
			// keep order 0 (the jipspec redirect substitutes the spectated
			// slot there). The iteration is identified by
			// playermgrGetPlayerAtOrder(i), NOT currentplayernum — the
			// spectate redirect substitutes the latter.
			{
				bool islocaliter;
				if (g_NetMode && g_NetLocalClient && g_NetLocalClient->player) {
					islocaliter = playermgrGetPlayerAtOrder(i) == g_NetLocalClient->playernum;
				} else {
					islocaliter = (i == 0);
				}
				if ((g_NetMode && !islocaliter) || i >= MAX_LOCAL_PLAYERS) {
					gdl = savedgdl;
				}
			}
#endif

			if ((g_Vars.coopplayernum >= 0 || g_Vars.antiplayernum >= 0)
#if VERSION >= VERSION_NTSC_1_0
					&& playerHasSharedViewport()
#else
					&& ((g_InCutscene && !g_MainIsEndscreen) || menuGetRoot() == MENUROOT_COOPCONTINUE)
#endif
					&& g_Vars.currentplayernum != 0) {
				gdl = savedgdl;
			}
		} // end of player loop
	} // end of stage if-statements

	if (g_Vars.autocutplaying && g_Vars.autocutfinished) {
		g_Vars.autocutplaying = false;
		g_Vars.autocutfinished = false;

		if (g_Vars.autocutgroupskip) {
			g_Vars.autocutgroupcur = -1;
			g_Vars.autocutgroupleft = 0;
		}

		if (g_Vars.autocutgroupcur < 0 && g_Vars.autocutgroupleft <= 0) {
			mainChangeToStage(STAGE_TITLE);
		}
	}

	// Advance the cutscenes when autoplaying
	if (!g_Vars.autocutplaying && g_Vars.autocutgroupcur >= 0 && g_Vars.autocutgroupleft > 0) {
		hudmsgRemoveAll();

		g_Vars.autocutnum = g_Cutscenes[g_Vars.autocutgroupcur].scene;

#if VERSION < VERSION_NTSC_1_0
		if (mainGetStageNum() != g_Cutscenes[g_Vars.autocutgroupcur].stage)
#endif
		{
			g_MissionConfig.iscoop = false;
			g_Vars.mplayerisrunning = false;
			g_Vars.normmplayerisrunning = false;
			g_Vars.bondplayernum = 0;
			g_Vars.coopplayernum = -1;
			g_Vars.antiplayernum = -1;
			g_MissionConfig.isanti = false;
			setNumPlayers(1);
			titleSetNextMode(TITLEMODE_SKIP);
			g_MissionConfig.difficulty = DIFF_A;
			lvSetDifficulty(DIFF_A);
			g_MissionConfig.stageindex = g_Cutscenes[g_Vars.autocutgroupcur].mission;
			g_MissionConfig.stagenum = g_Cutscenes[g_Vars.autocutgroupcur].stage;
			titleSetNextStage(g_Cutscenes[g_Vars.autocutgroupcur].stage);
			mainChangeToStage(g_Cutscenes[g_Vars.autocutgroupcur].stage);
		}

		g_Vars.autocutgroupleft--;

		if (g_Vars.autocutgroupleft > 0) {
			g_Vars.autocutgroupcur++;
		} else {
			g_Vars.autocutgroupcur = -1;
		}
	}

	gDPSetScissor(gdl++, G_SC_NON_INTERLACE, 0, 0, viGetWidth(), viGetHeight());

#ifndef PLATFORM_N64
	if (videoGetDisplayFPS()) {
		gdl = lvRenderFPS(gdl);
	}
#endif

#if VERSION < VERSION_NTSC_1_0
	if ((uintptr_t)gdl < (uintptr_t)g_GfxBuffers[g_GfxActiveBufferIndex]
			|| (uintptr_t)gdl > (uintptr_t)g_GfxBuffers[g_GfxActiveBufferIndex + 1]) {
		crashSetMessage("lv.c Master DL overrun!");
		CRASH();
	}
#endif

	return gdl;
}

const char var7f1b7730[] = "fr: %d\n";

u32 g_CutsceneTime240_60 = 0;

#if VERSION >= VERSION_NTSC_1_0
u32 var800840a8 = 0;
u32 var800840ac = 0;
u32 var800840b0 = 0;
#else
u32 var80086930nb = 0;
u32 var800840a8 = 0;
u32 var800840ac = 0;
u32 var800840b0 = 0;
#endif

u32 var800840b4 = 0;
u32 var800840b8 = 0;
u32 var800840bc = 0;

void lvUpdateSoloHandicaps(void)
{
	if (g_Vars.antiplayernum >= 0) {
		if (g_Difficulty == DIFF_A) {
			g_CctvWaitScale = 2;
			g_CctvDamageRxScale = 2;
			g_AutogunAccuracyScale = 0.5f;
			g_AutogunDamageTxScale = 0.5f;
			g_AutogunDamageRxScale = 2;
			g_EnemyAccuracyScale = 0.5f;
			g_PlayerDamageRxScale = 0.35f;
			g_PlayerDamageTxScale = 4;
			g_ExplosionDamageTxScale = 0.25f;
			g_AutoAimScale = 1.5f;
			g_AmmoQuantityScale = 3;
			g_AttackWalkDurationScale = 0.2f;
		} else if (g_Difficulty == DIFF_SA) {
			g_CctvWaitScale = 2;
			g_CctvDamageRxScale = 1.5f;
			g_AutogunAccuracyScale = 0.5f;
			g_AutogunDamageTxScale = 0.5f;
			g_AutogunDamageRxScale = 1.5f;
			g_EnemyAccuracyScale = 0.6f;
			g_PlayerDamageRxScale = 0.5f;
			g_PlayerDamageTxScale = 3;
			g_ExplosionDamageTxScale = 0.25f;
			g_AutoAimScale = 1.1f;
			g_AmmoQuantityScale = 2.5f;
			g_AttackWalkDurationScale = 0.5f;
		} else {
			g_CctvWaitScale = 2;
			g_CctvDamageRxScale = 1;
			g_AutogunAccuracyScale = 0.5f;
			g_AutogunDamageTxScale = 0.5f;
			g_AutogunDamageRxScale = 1;
			g_EnemyAccuracyScale = 0.7f;
			g_PlayerDamageRxScale = 0.65f;
			g_PlayerDamageTxScale = 2;
			g_ExplosionDamageTxScale = 0.25f;
			g_AutoAimScale = 0.75f;
			g_AmmoQuantityScale = 2;
			g_AttackWalkDurationScale = 1;
		}
	} else if (g_Vars.coopplayernum >= 0) {
		if (g_Difficulty == DIFF_A) {
			g_CctvWaitScale = 2;
			g_CctvDamageRxScale = 2;
			g_AutogunAccuracyScale = 0.5f;
			g_AutogunDamageTxScale = 0.5f;
			g_AutogunDamageRxScale = 2;
			g_EnemyAccuracyScale = 0.6f;
			g_PlayerDamageRxScale = 0.5f;
			g_PlayerDamageTxScale = 2;
			g_ExplosionDamageTxScale = 0.25f;
			g_AutoAimScale = 1.5f;
			g_AmmoQuantityScale = 2;
			g_AttackWalkDurationScale = 0.2f;
		} else if (g_Difficulty == DIFF_SA) {
			g_CctvWaitScale = 1;
			g_CctvDamageRxScale = 1;
			g_AutogunAccuracyScale = 0.75f;
			g_AutogunDamageTxScale = 1;
			g_AutogunDamageRxScale = 1;
			g_EnemyAccuracyScale = 0.75f;
			g_PlayerDamageRxScale = 1;
			g_PlayerDamageTxScale = 1;
			g_ExplosionDamageTxScale = 1;
#if VERSION >= VERSION_JPN_FINAL
			g_AutoAimScale = 0.75f;
#else
			g_AutoAimScale = g_Jpn ? 1.1f : 0.75f;
#endif
			g_AmmoQuantityScale = 1.5f;
			g_AttackWalkDurationScale = 0.5f;
		} else {
			g_CctvWaitScale = 1;
			g_CctvDamageRxScale = 1;
			g_AutogunAccuracyScale = 1;
			g_AutogunDamageTxScale = 1.5f;
			g_AutogunDamageRxScale = 1;
			g_EnemyAccuracyScale = 1.5f;
			g_PlayerDamageRxScale = 1.5f;
			g_PlayerDamageTxScale = 1;
			g_ExplosionDamageTxScale = 1.5f;
#if VERSION >= VERSION_JPN_FINAL
			g_AutoAimScale = 0.2f;
#else
			g_AutoAimScale = g_Jpn ? 0.75f : 0.2f;
#endif
			g_AmmoQuantityScale = 1;
			g_AttackWalkDurationScale = 1;
		}
	} else {
		if (g_Difficulty == DIFF_A) {
			f32 totalhealth;
			f32 frac = 1;

			if (g_Vars.coopplayernum < 0 && g_Vars.antiplayernum < 0) {
				totalhealth = playerGetHealthFrac() + playerGetShieldFrac();

				if (totalhealth <= 0.125f) {
					frac = 0.5f;
				} else if (totalhealth <= 0.6f) {
					frac = (totalhealth - 0.125f) * 0.5f / 0.47500002384186f + 0.5f;
				}
			}

			g_CctvWaitScale = 2;
			g_CctvDamageRxScale = 2;
			g_AutogunAccuracyScale = 0.5f * frac;
			g_AutogunDamageTxScale = 0.5f * frac;
			g_AutogunDamageRxScale = 2;
			g_EnemyAccuracyScale = 0.6f;
			g_PlayerDamageRxScale = 0.5f * frac;
			g_PlayerDamageTxScale = 2;
			g_ExplosionDamageTxScale = 0.25f * frac;
			g_AutoAimScale = 1.5f;
			g_AmmoQuantityScale = 2;
			g_AttackWalkDurationScale = 0.2f;
		} else if (g_Difficulty == DIFF_SA) {
			g_CctvWaitScale = 1;
			g_CctvDamageRxScale = 1;
			g_AutogunAccuracyScale = 0.75f;
			g_AutogunDamageTxScale = 0.75f;
			g_AutogunDamageRxScale = 1;
			g_EnemyAccuracyScale = 0.8f;
			g_PlayerDamageRxScale = 0.6f;
			g_PlayerDamageTxScale = 1;
			g_ExplosionDamageTxScale = 0.75f;
#if VERSION >= VERSION_JPN_FINAL
			g_AutoAimScale = 0.75f;
#else
			g_AutoAimScale = g_Jpn ? 1.1f : 0.75f;
#endif
			g_AmmoQuantityScale = 1.5f;
			g_AttackWalkDurationScale = 0.5f;
		} else if (g_Difficulty == DIFF_PA) {
			g_CctvWaitScale = 1;
			g_CctvDamageRxScale = 1;
			g_AutogunAccuracyScale = 1;
			g_AutogunDamageTxScale = 1;
			g_AutogunDamageRxScale = 1;
			g_EnemyAccuracyScale = 1.175f;
			g_PlayerDamageRxScale = 1;
			g_PlayerDamageTxScale = 1;
			g_ExplosionDamageTxScale = 1;
#if VERSION >= VERSION_JPN_FINAL
			g_AutoAimScale = 0.2f;
#else
			g_AutoAimScale = g_Jpn ? 0.75f : 0.2f;
#endif
			g_AmmoQuantityScale = 1;
			g_AttackWalkDurationScale = 1;
		} else if (g_Difficulty == DIFF_PD) {
			g_CctvWaitScale = 1;
			g_CctvDamageRxScale = 1;
			g_AutogunAccuracyScale = 1;
			g_AutogunDamageTxScale = 1;
			g_AutogunDamageRxScale = 1;
			g_EnemyAccuracyScale = 1.1f;
			g_PlayerDamageRxScale = 1;
			g_PlayerDamageTxScale = 1;
			g_ExplosionDamageTxScale = 1;
			g_AutoAimScale = 1;
			g_AmmoQuantityScale = 1;
			g_AttackWalkDurationScale = 1;
		}
	}
}

#if PIRACYCHECKS

#if PAL
#define SUBAMOUNT 6661
#else
#define SUBAMOUNT 54321
#endif

s32 sub54321(s32 value)
{
	return value - SUBAMOUNT;
}
#endif

void lvUpdateCutsceneTime(void)
{
	if (g_Vars.in_cutscene) {
		g_CutsceneTime240_60 += g_Vars.lvupdate60;
		return;
	}

	g_CutsceneTime240_60 = 0;
}

#ifndef PLATFORM_N64
s32 g_LvSlomoEngaged = false;

// Halve the tick delta with a remainder carried across frames, so slow motion
// works at any framerate. Plain integer halving quantized away at high fps
// (delta is mostly 1 above ~200fps — nothing to halve); the carry makes ticks
// alternate 0/1 there, averaging exactly half speed. When the fixed-tick /
// netplay step pin is active the halving happens in detPinTimestep instead
// (a zero from here would read as "paused" to the pin), so pass through.
s32 lvSlomoScaleTick(s32 ticks)
{
	static s32 rem = 0;
	s32 total;

	if (detTickPinActive()) {
		return ticks;
	}

	total = ticks + rem;
	rem = total & 1;
	return total >> 1;
}

// Mark this frame as slow-motion for the fixed-tick / netplay step pin.
// Net clients don't decide locally — the server's decision arrives via
// SVC_TIMESCALE so both machines halve the same pinned step.
static void lvSlomoEngage(void)
{
	if (g_NetMode != NETMODE_CLIENT) {
		g_LvSlomoEngaged = true;
	}
}

// SLOWMOTION_SMART: true if a living simulant is within LV_SMART_SLOMO_RANGE
// of a living player. The vanilla proximity test only considers human
// players, so SMART could never engage in the most common Combat Sim case —
// one human plus simulants ("activates if an enemy chr is nearby" was the
// stated intent, but chrs were never checked).
static bool lvSlomoSimNearby(void)
{
	s32 i;
	s32 playernum;

	for (i = 0; i < g_BotCount; i++) {
		struct chrdata *chr = g_MpBotChrPtrs[i];

		if (!chr || !chr->prop || chrIsDead(chr)) {
			continue;
		}

		for (playernum = 0; playernum < PLAYERCOUNT(); playernum++) {
			struct player *pl = g_Vars.players[playernum];
			f32 dx;
			f32 dy;
			f32 dz;

			if (pl->is_spectator || !pl->prop || pl->isdead) {
				continue;
			}

			dx = chr->prop->pos.x - pl->prop->pos.x;
			dy = chr->prop->pos.y - pl->prop->pos.y;
			dz = chr->prop->pos.z - pl->prop->pos.z;

			if (dx * dx + dy * dy + dz * dz < LV_SMART_SLOMO_RANGE * LV_SMART_SLOMO_RANGE) {
				return true;
			}
		}
	}

	return false;
}
#endif

s32 lvGetSlowMotionType(void)
{
#if PIRACYCHECKS
#if PAL
	u32 addr = sub54321(0xb0000340 + SUBAMOUNT);
	u32 actual;
	u32 expected = sub54321(0x0330c820 + SUBAMOUNT);
#else
	u32 addr = sub54321(0xb0000a5c + SUBAMOUNT);
	u32 actual;
	u32 expected = sub54321(0x1740fff9 + SUBAMOUNT);
#endif

	osPiReadIo(addr, &actual);

	if (actual != expected) {
		u32 *ptr = (u32 *)&rspbootTextStart;
		u32 *end = (u32 *)(uintptr_t)ptr + 1024;

		while (ptr < end) {
			*ptr += 8;
			ptr++;
		}
	}
#endif

	if (g_Vars.normmplayerisrunning) {
		if (g_MpSetup.options & MPOPTION_SLOWMOTION_ON) {
			return SLOWMOTION_ON;
		}
		if (g_MpSetup.options & MPOPTION_SLOWMOTION_SMART) {
			return SLOWMOTION_SMART;
		}
	} else {
		if (cheatIsActive(CHEAT_SLOMO)) {
			return SLOWMOTION_ON;
		}
		if (debugGetSlowMotion() == SLOWMOTION_ON) {
			return SLOWMOTION_ON;
		}
		if (debugGetSlowMotion() == SLOWMOTION_SMART) {
			return SLOWMOTION_SMART;
		}
	}

	return SLOWMOTION_OFF;
}

void lvTick(void)
{
	s32 j;
	s32 i;

#ifndef PLATFORM_N64
	// Killcam (MPOPTION_KILLCAM): record the world this tick + detect the local
	// pawn's death edge to start a replay. Early in the tick so a triggered
	// killcam owns g_NetSpectateChr before lvTickPlayer's spectate-on-death check.
	netKillcamRecordTick();
	netDemoRecordTick(); // demo: append this tick's world pose if recording (port-only)
	netDemoPlayTick();   // demo: advance playback (read next frame) if playing (port-only)
#endif

#ifndef PLATFORM_N64
	// Heal a corrupted active-prop chain BEFORE any unbounded walk this frame
	// (lightingTick/roomsTickLighting below, propsTick, and lvRender's
	// propsTickPlayer): a netplay client can relink a freed prop into a ->next
	// cycle that otherwise hangs the game. No-op on a healthy list. Gated to
	// netplay (the corruption is client-side; cheap, harmless on the host).
	if (g_NetMode != NETMODE_NONE) {
		propsHealActiveList();
		// Server + client: release weapon slots orphaned by freed projectile/weapon
		// props (prop freed, slot back-pointer not cleared) so weaponCreate can reuse
		// them instead of saturating the 50-slot pool -> NULL return (the client
		// SVC_PROP_SPAWN crash; the server silently failing to spawn drops) and
		// force-recycling out live host props (sync collapse / void). Self-gates to
		// netplay. (crash ledger #16 / census projdead)
		weaponSlotsReapOrphans();
	}
#endif

	lvCheckPauseStateChanged();

#if VERSION >= VERSION_NTSC_1_0
	if (g_Vars.pakstocheck) {
		paksTick();
	}
#endif

	if (g_Vars.joydisableframestogo > 0) {
		g_Vars.joydisableframestogo--;
	} else if (g_Vars.joydisableframestogo == 0) {
#if VERSION >= VERSION_NTSC_1_0
		joyUnlockCyclicPolling();
#else
		if (!joyIsCyclicPollingEnabled()) {
			joyEnableCyclicPolling(3278, "lv.c");
		}
#endif

		if (g_Vars.stagenum == STAGE_TITLE
				|| g_Vars.stagenum == STAGE_BOOTPAKMENU
				|| g_Vars.stagenum == STAGE_CREDITS
				|| g_Vars.stagenum == STAGE_4MBMENU) {
			g_Vars.paksneededforgame = 0;
		} else {
			g_Vars.paksneededforgame = 0x1f;
			pakEnableRumbleForAllPlayers();
		}

		g_Vars.joydisableframestogo = -1;
	}

	if (IS4MB()) {
		vmPrintStatsIfEnabled();
	}

	for (j = 0; j < PLAYERCOUNT(); j++) {
		g_Vars.players[j]->hands[HAND_LEFT].hasdotinfo = false;
		g_Vars.players[j]->hands[HAND_RIGHT].hasdotinfo = false;
	}

	if (lvIsPaused()) {
		g_Vars.lvupdate240 = 0;
	} else if (mpIsPaused()) {
		g_Vars.lvupdate240 = 0;

		for (j = 0; j < PLAYERCOUNT(); j++) {
			g_Vars.players[j]->joybutinhibit = 0xffffefff;
		}
	} else {
		s32 slowmo = lvGetSlowMotionType();
		g_Vars.lvupdate240 = g_Vars.diffframe240;

#ifndef PLATFORM_N64
		// Re-decide slow-motion engagement each frame (server/local only;
		// net clients keep the wire-driven value from SVC_TIMESCALE).
		if (g_NetMode != NETMODE_CLIENT) {
			g_LvSlomoEngaged = false;
		}
#endif

		if (slowmo == SLOWMOTION_ON) {
			if (g_Vars.speedpillon == false || g_Vars.in_cutscene) {
#ifndef PLATFORM_N64
				lvSlomoEngage();
#endif
				if (g_Vars.lvupdate240 > LV_SLOMO_TICK_CAP) {
					g_Vars.lvupdate240 = LV_SLOMO_TICK_RATE;
				}
			}
		} else if (slowmo == SLOWMOTION_SMART) {
			// Smart slow motion - activates if an enemy chr is nearby
			if (g_Vars.speedpillon == false || g_Vars.in_cutscene) {
				if (g_Vars.mplayerisrunning) {
					bool foundnearbychr = false;
					s32 playernum;

#ifndef PLATFORM_N64
					// Net games: the vanilla test below asks "is player A's
					// room on player B's SCREEN" (g_MpRoomVisibility) — which
					// only exists for rendered local viewports (and is only 4
					// players wide). Remote players' visibility traversals on
					// the server run with stale viewports, so SMART almost
					// never fired in net games. Use a plain distance test
					// between living players instead; the server's decision
					// reaches clients via SVC_TIMESCALE as usual.
					if (g_NetMode) {
						for (playernum = 0; playernum < PLAYERCOUNT() && !foundnearbychr; playernum++) {
							struct player *pa = g_Vars.players[playernum];
							s32 otherplayernum;

							if (pa->is_spectator || !pa->prop || pa->isdead) {
								continue;
							}

							for (otherplayernum = playernum + 1; otherplayernum < PLAYERCOUNT(); otherplayernum++) {
								struct player *pb = g_Vars.players[otherplayernum];
								f32 dx;
								f32 dy;
								f32 dz;

								if (pb->is_spectator || !pb->prop || pb->isdead) {
									continue;
								}

								dx = pa->prop->pos.x - pb->prop->pos.x;
								dy = pa->prop->pos.y - pb->prop->pos.y;
								dz = pa->prop->pos.z - pb->prop->pos.z;

								if (dx * dx + dy * dy + dz * dz < LV_SMART_SLOMO_RANGE * LV_SMART_SLOMO_RANGE) {
									foundnearbychr = true;
									break;
								}
							}
						}
					} else
#endif
					// Check if another player is in a nearby room
					for (playernum = 0; playernum < PLAYERCOUNT() && !foundnearbychr; playernum++) {
#ifndef PLATFORM_N64
						// Spectator panel slots have no prop — skip them so
						// SLOWMOTION_SMART doesn't deref NULL on the host.
						if (g_Vars.players[playernum]->is_spectator || !g_Vars.players[playernum]->prop) {
							continue;
						}
#endif
						if (g_Vars.players[playernum]->isdead == false) {
							RoomNum *rooms = g_Vars.players[playernum]->prop->rooms;
							s32 r;

							for (r = 0; rooms[r] != -1 && !foundnearbychr; r++) {
								s32 otherplayernum;
								for (otherplayernum = 0; otherplayernum < PLAYERCOUNT(); otherplayernum++) {
									if (playernum != otherplayernum
											&& g_Vars.players[otherplayernum]->isdead == false
											&& bgRoomIsOnPlayerScreen(rooms[r], otherplayernum)) {
										foundnearbychr = true;
									}
								}
							}
						}
					}

#ifndef PLATFORM_N64
					// Simulants count as nearby chrs too (one shared site for
					// both the net distance test and the vanilla local path).
					if (!foundnearbychr) {
						foundnearbychr = lvSlomoSimNearby();
					}
#endif

					if (foundnearbychr) {
#ifndef PLATFORM_N64
						lvSlomoEngage();
#endif
						if (g_Vars.lvupdate240 > LV_SLOMO_TICK_CAP) {
							g_Vars.lvupdate240 = LV_SLOMO_TICK_RATE;
						}
					} else {
						if (g_Vars.lvupdate240 > TICKS(8)) {
							g_Vars.lvupdate240 = TICKS(8);
						}
					}
				} else {
#ifndef PLATFORM_N64
					lvSlomoEngage();
#endif
					if (g_Vars.lvupdate240 > LV_SLOMO_TICK_CAP) {
						g_Vars.lvupdate240 = LV_SLOMO_TICK_RATE;
					}
				}
			}
		} else {
			// Slow motion settings are off
			if (g_Vars.speedpillon && g_Vars.in_cutscene == false) {
#ifndef PLATFORM_N64
				lvSlomoEngage();
#endif
				if (g_Vars.lvupdate240 > LV_SLOMO_TICK_CAP) {
					g_Vars.lvupdate240 = LV_SLOMO_TICK_RATE;
				}
			}
		}
	}

	// Determinism harness: when active, pin lvupdate240 to a fixed 1/60 step
	// before the rest of the derivation runs, so the lvupdate60/60f/freal values
	// and the lvframe* counters below all advance deterministically. No-op during
	// normal play. (Must be before the lvupdate60 derivation directly below.)
	detPinTimestep();

	g_Vars.lvupdate60 = g_Vars.lvupdate240 + g_Vars.lvupdate240rem;
	g_Vars.lvupdate240rem = g_Vars.lvupdate60 & 3;
	g_Vars.lvupdate60 >>= 2;

	if (g_Vars.lvupdate240 > 0) {
		g_Vars.lvframenum++;
	}

	g_Vars.lvupdate60f = g_Vars.lvupdate240 * 0.25f;
	g_Vars.lvframe60 += g_Vars.lvupdate60;
	g_Vars.lvframe240 += g_Vars.lvupdate240;
	g_Vars.lvupdate60frealprev = g_Vars.lvupdate60freal;
	g_Vars.lvupdate60freal = PALUPF(g_Vars.lvupdate60f);

	bgunTickBoost();
	hudmsgsTick();

	if ((joyGetButtonsPressedThisFrame(0, 0xffffffff) != 0
				|| joyGetStickX(0) > 10
				|| joyGetStickX(0) < -10
				|| joyGetStickY(0) > 10
				|| joyGetStickY(0) < -10
				|| joyGetButtonsPressedThisFrame(1, 0xffffffff) != 0
				|| joyGetStickX(1) > 10
				|| joyGetStickX(1) < -10
				|| joyGetStickY(1) > 10
				|| joyGetStickY(1) < -10
				|| joyGetButtonsPressedThisFrame(2, 0xffffffff) != 0
				|| joyGetStickX(2) > 10
				|| joyGetStickX(2) < -10
				|| joyGetStickY(2) > 10
				|| joyGetStickY(2) < -10
				|| joyGetButtonsPressedThisFrame(3, 0xffffffff) != 0
				|| joyGetStickX(3) > 10
				|| joyGetStickX(3) < -10
				|| joyGetStickY(3) > 10
				|| joyGetStickY(3) < -10) && g_IsTitleDemo) {
		if (g_Vars.stagenum != STAGE_TITLE) {
			titleSetNextMode(TITLEMODE_SKIP);
			mainChangeToStage(STAGE_TITLE);
		}

		g_IsTitleDemo = false;
	}

	if (g_Vars.stagenum < STAGE_TITLE && !g_IsTitleDemo && !g_Vars.in_cutscene) {
		if (joyGetButtons(0, 0xffffffff) == 0
				&& joyGetStickX(0) < 10
				&& joyGetStickX(0) > -10
				&& joyGetStickY(0) < 10
				&& joyGetStickY(0) > -10
				&& joyGetButtons(1, 0xffffffff) == 0
				&& joyGetStickX(1) < 10
				&& joyGetStickX(1) > -10
				&& joyGetStickY(1) < 10
				&& joyGetStickY(1) > -10
				&& joyGetButtons(2, 0xffffffff) == 0
				&& joyGetStickX(2) < 10
				&& joyGetStickX(2) > -10
				&& joyGetStickY(2) < 10
				&& joyGetStickY(2) > -10
				&& joyGetButtons(3, 0xffffffff) == 0
				&& joyGetStickX(3) < 10
				&& joyGetStickX(3) > -10
				&& joyGetStickY(3) < 10
				&& joyGetStickY(3) > -10) {
			g_TitleIdleTime60 += g_Vars.diffframe60;
		} else {
			g_TitleIdleTime60 = 0;
		}
	} else {
		g_TitleIdleTime60 = 0;
	}

	g_NumReasonsToEndMpMatch = 0;

	// Handle MP match ending
	if (g_Vars.normmplayerisrunning && g_Vars.stagenum < STAGE_TITLE) {
		if (g_MpTimeLimit60 > 0) {
			s32 elapsed = g_StageTimeElapsed60;
			s32 nexttime = g_Vars.lvupdate60 + g_StageTimeElapsed60;
			s32 warntime = TICKS(g_MpTimeLimit60) - TICKS(3600);

#ifndef PLATFORM_N64
			// Combat Sim remaining-time HUD: drive the solo-mission countdown
			// timer (mm:ss:cc, bottom centre, countdownTimerRender) with the
			// match's remaining time so it counts DOWN to the time limit. The
			// value is pinned every frame (running stays false so its own
			// tick never fights this); setupLoadFiles re-hides it on the next
			// stage load by resetting g_CountdownTimerOff.
			{
				f32 remaining60 = (f32)(TICKS(g_MpTimeLimit60) - g_StageTimeElapsed60);

				if (remaining60 < 0.0f) {
					remaining60 = 0.0f;
				}

				countdownTimerSetRunning(false);
				countdownTimerSetValue60(remaining60);
				countdownTimerSetVisible(COUNTDOWNTIMERREASON_AI | COUNTDOWNTIMERREASON_NOCONTROL, true);
			}
#endif

			// Show HUD message at one minute remaining
			if (elapsed < warntime && nexttime >= warntime) {
				s32 i;

				for (i = 0; i < PLAYERCOUNT(); i++) {
					setCurrentPlayerNum(i);
					hudmsgCreate(langGet(L_MISC_068), HUDMSGTYPE_DEFAULT); // "One minute left."
				}
			}

			if (elapsed < TICKS(g_MpTimeLimit60) && nexttime >= TICKS(g_MpTimeLimit60)) {
				// Match is ending due to time limit reached
#ifndef PLATFORM_N64
				if (g_NetMode != NETMODE_CLIENT)
#endif
				mainEndStage();
			}

			// Sound alarm at 10 seconds remaining
			if (nexttime >= TICKS(g_MpTimeLimit60) - TICKS(600)
					&& g_MiscAudioHandle == NULL
					&& !lvIsPaused()
					&& nexttime < TICKS(g_MpTimeLimit60)) {
				snd00010718(&g_MiscAudioHandle, 0, AL_VOL_FULL, AL_PAN_CENTER, SFX_ALARM_DEFAULT, 1, 1, -1, true);
			}
		}

		if (g_Vars.lvupdate240 != 0) {
			s32 numdying = 0;

			for (i = 0; i < PLAYERCOUNT(); i++) {
				if (g_Vars.players[i]->isdead) {
					if (g_Vars.players[i]->redbloodfinished == false
							|| g_Vars.players[i]->deathanimfinished == false
							|| g_Vars.players[i]->colourfadetimemax60 >= 0) {
						numdying++;
					}
				}
			}

			for (i = 0; i < g_MpNumChrs; i++) {
				if (g_MpAllChrPtrs[i]->actiontype == ACT_DIE) {
					numdying++;
				}
			}

			if (g_MpScoreLimit > 0) {
				struct ranking rankings[MAX_MPCHRS];
				s32 count = mpGetPlayerRankings(rankings);

				for (i = 0; i < count; i++) {
					if (rankings[i].score >= g_MpScoreLimit) {
						g_NumReasonsToEndMpMatch++;
					}
				}
			}

			if (g_MpTeamScoreLimit > 0) {
				struct ranking rankings[MAX_MPCHRS];
				s32 count = mpGetTeamRankings(rankings);

				for (i = 0; i < count; i++) {
					if (rankings[i].score >= g_MpTeamScoreLimit) {
						g_NumReasonsToEndMpMatch++;
					}
				}
			}

#ifndef PLATFORM_N64
			// Global Lives system: last faction standing ends the match
			// (internally gated on the Lives setting). Polled here beside the
			// limit checks because g_NumReasonsToEndMpMatch is recomputed
			// from zero above — an increment made from the scenario tick
			// would be wiped before this test.
			if (elimShouldEndMatch()) {
				g_NumReasonsToEndMpMatch++;
			}

			// Race: everyone finished, or the post-winner finish timer
			// expired (internally gated on the scenario; same polling rule).
			if (raceShouldEndMatch()) {
				g_NumReasonsToEndMpMatch++;
			}

			if (g_NetMode == NETMODE_CLIENT) {
				g_NumReasonsToEndMpMatch = 0;
			}
#endif

			if (g_NumReasonsToEndMpMatch > 0 && numdying == 0) {
				mainEndStage();
			}
		}
	}

	g_StageTimeElapsed60 += g_Vars.lvupdate60;
	g_StageTimeElapsed1f = g_StageTimeElapsed60 / TICKS(60.0f);

	viSetUseZBuf(true);

	if (g_Vars.stagenum == STAGE_TEST_OLD) {
		titleTickOld();
		musicTick();
	}

	if (g_Vars.stagenum == STAGE_TITLE) {
		titleTick();
		langTick();
		musicTick();
	} else if (g_Vars.stagenum == STAGE_BOOTPAKMENU) {
		setCurrentPlayerNum(0);
#if VERSION >= VERSION_PAL_BETA
		playerConfigureVi();
#endif
		menuTick();
		musicTick();
		langTick();
		pakExecuteDebugOperations();
	} else if (g_Vars.stagenum == STAGE_4MBMENU) {
		menuTick();
		musicTick();
		langTick();
		pakExecuteDebugOperations();
	} else if (g_Vars.stagenum == STAGE_CREDITS) {
		musicTick();
		langTick();
	} else {
		lvUpdateCutsceneTime();
		vtxstoreTick();
		lvUpdateSoloHandicaps();
		roomsTick();
		skyTick();
		casingsTick();
		shardsTick();
		sparksTick();
		wallhitsTick();
		splatsTick();

		if (g_WeatherActive) {
			weatherTick();
		}

		if (g_NbombsActive) {
			nbombsTick();
		}

		lvUpdateMiscSfx();
		sndTick();
		pakExecuteDebugOperations();
		lightingTick();
		modelmgrPrintCounts();
		boltbeamsTick();
		amTick();
		menuTick();
		scenarioTick();

		if (!g_MainIsEndscreen) {
			propsTick();
		}

		musicTick();
		langTick();
		propsTickPadEffects();

		if (mainGetStageNum() == STAGE_CITRAINING) {
			struct trainingdata *trainingdata = dtGetData();

			if ((g_Vars.currentplayer->prop->rooms[0] < ROOM_DISH_HOLO1 || g_Vars.currentplayer->prop->rooms[0] > ROOM_DISH_HOLO4)
					&& g_Vars.currentplayer->prop->rooms[0] != ROOM_DISH_FIRINGRANGE
					&& (trainingdata == NULL || trainingdata->intraining == false)) {
				chrUnsetStageFlag(NULL, STAGEFLAG_CI_IN_TRAINING);
			}

			frTick();

			if (g_Vars.lvupdate240 != 0) {
				dtTick();
				htTick();
			}
		}
	}
}

const char var7f1b7738[] = "cutsceneframe: %d\n";
const char var7f1b774c[] = "pos:%s%s %.2f %.2f %.2f\n";
const char var7f1b7768[] = "";
const char var7f1b776c[] = "";

void lvTickPlayer(void)
{
	f32 xdiff;
	f32 zdiff;

#ifndef PLATFORM_N64
	// PROBE: dedicated-server stale-position investigation. Throttled to ~1Hz
	// per call. Lets us confirm whether lvTickPlayer fires at all for remote
	// players on the server, and which playerTick branch (true/false) runs.
	if ((g_NetTick % 60u) == 0u && g_Vars.currentplayer) {
		netDiagLogf("lvtp_enter",
			"pnum=%d isremote=%d has_client=%d is_spec=%d v64=%d v68=%d",
			g_Vars.currentplayernum,
			g_Vars.currentplayer->isremote ? 1 : 0,
			g_Vars.currentplayer->client ? 1 : 0,
			g_Vars.currentplayer->is_spectator ? 1 : 0,
			var80075d64, var80075d68);
	}
#endif

#ifndef PLATFORM_N64
	// Demo playback puppets every pawn from the recording (netDemoRenderBegin).
	// Running the live player tick re-drives this human pawn's movement + third-
	// person animation each frame (and the non-host pawns have no controller, so
	// they idle), overwriting the recorded pose — which is why human players looked
	// static/floating while sims (not local players, so never re-ticked) were clean.
	// Skip the live tick during playback so the recorded body pose sticks; the
	// camera is overridden in netDemoRenderBegin.
	//
	// EXCEPTION: player 0 is the single rendered viewport (forcesingleplayer) and the
	// demo "render vehicle" — keep its tick so its first-person gun pipeline runs. We
	// feed it the followed combatant's recorded view/weapon (demo.c); its own body is
	// hidden and its camera overridden. (/demoview switches the followed combatant,
	// but rendering always goes through player 0.)
	bool demoplaying = netDemoIsPlaying();
	if (demoplaying && g_Vars.currentplayernum == 0) {
		demoplaying = false;
	}
#else
	const bool demoplaying = false;
#endif
	if (!demoplaying && var80075d64 == 2) {
		if (var80075d68 == 2) {
			playerTick(true);
		} else {
			playerTick(false);
		}
	}

	xdiff = g_Vars.currentplayer->prop->pos.x - g_Vars.currentplayer->bondprevpos.x;
	zdiff = g_Vars.currentplayer->prop->pos.z - g_Vars.currentplayer->bondprevpos.z;

	g_Vars.currentplayerstats->distance += sqrtf(xdiff * xdiff + zdiff * zdiff);

#ifndef PLATFORM_N64
	// Spectate camera override. Runs once per local-player tick after physics
	// so we read the final-for-this-frame prop->pos / vv_theta from the target
	// rather than a stale value. Gated to the local netclient — remote chrs
	// hit lvTickPlayer too on the server side (per-player iteration) and we
	// don't want to stomp their state. netSpectateApply itself bails if no
	// target is set, so the cost when not spectating is one branch.
	// The JIP mid-match joiner has NO local player (player == NULL until the
	// round boundary seats it), so the first comparison never matches; run
	// the auto-update once per frame (first render order) for it — that's
	// what auto-engages the spectate redirect. netSpectateApply stays a no-op
	// there (it bails without a local player; the lvRender redirect renders).
	if (g_NetMode && g_NetLocalClient
			&& (g_Vars.currentplayer == g_NetLocalClient->player
				|| (g_NetMode == NETMODE_CLIENT && !g_NetLocalClient->player
					&& g_NetLocalClient->is_spectator && g_Vars.currentplayerindex == 0))) {
		// Auto-spectate on death / restore on respawn, then apply the camera
		// override for whatever target is active (manual or death-driven).
		netSpectateAutoUpdate();
		netSpectateApply();

		// JIP spectator pause menu: with no own player, bondmove never runs
		// for us, so the normal ESC/START -> mpPushPauseDialog path is
		// unreachable and the spectator can't leave the match. Open it here
		// off the current (order-0) slot's context: its stats->mpindex is 0,
		// so the menu binds to contpad 0 — the local keyboard/pad. The
		// rankings header shows that slot's name (cosmetic); Quit works.
		// Covers Combat Sim AND co-op (an unseated co-op spectator — mission
		// full / claim failed — was otherwise stuck force-closing the game;
		// mpPushPauseDialog's non-normmplayer branch pushes the mission
		// pause dialog).
		if (g_NetMode == NETMODE_CLIENT && !g_NetLocalClient->player
				&& g_NetLocalClient->is_spectator
				&& (g_Vars.normmplayerisrunning || g_Vars.coopplayernum >= 0)
				&& g_MenuData.root != MENUROOT_MPPAUSE
				&& g_Vars.currentplayer && g_Vars.currentplayer->prop
				&& (inputKeyJustPressed(VK_ESCAPE)
					|| (joyGetButtonsPressedThisFrame(0, START_BUTTON) != 0))) {
			mpPushPauseDialog();
		}
	}
#endif
}

void lvStop(void)
{
	paksStop(true);

#ifndef PLATFORM_N64
	// The player structs live in the stage pool that's about to be freed —
	// any netclient binding kept across this point is a dangling pointer the
	// per-tick net code dereferences on the next frame (crash observed: a
	// co-op client declining the retry-mission dialog exits to the menu
	// WHILE STILL CONNECTED; netClientRecordMove then read the freed
	// player/chr/model chain). Bindings are re-established at the next stage
	// load (netPlayersAllocate / the stage-start manifest / co-op claim), so
	// severing them here is always safe.
	if (g_NetMode) {
		for (s32 i = 0; i <= NET_MAX_CLIENTS; ++i) {
			g_NetClients[i].player = NULL;
		}
		netSpectateStop();
	}
#endif

	if (g_MiscAudioHandle && sndGetState(g_MiscAudioHandle)) {
		audioStop(g_MiscAudioHandle);
	}

	if (g_Vars.stagenum < STAGE_TITLE) {
		s32 bank = langGetLangBankIndexFromStagenum(g_Vars.stagenum);
		langClearBank(bank);
		stub0f015270();
	}

	chrmgrStop();
	explosionsStop();
	smokeStop();
	stub0f015400();
	stub0f015410();
	shardsStop();
	stub0f0153f0();
	propsStop();
	objsStop();
	weatherStop();
	objectivesStop();
	stub0f015260();
	bgunStop();
	psStop();
	musicStop();
	hudmsgsStop();

	if (g_Vars.stagenum < STAGE_TITLE) {
		bgStop();
	}

	func00033dd8();

	if (g_FileState == FILESTATE_CHANGINGAGENT) {
		menuPlaySound(MENUSOUND_EXPLOSION);
		g_FileState = FILESTATE_UNSELECTED;
	}

#if VERSION >= VERSION_NTSC_1_0
	menuStop();
#endif
}

void lvCheckPauseStateChanged(void)
{
	u32 paused = mpIsPaused();

	if (paused != var80084010) {
		if (paused) {
			pakDisableRumbleForAllPlayers();
		} else {
			pakEnableRumbleForAllPlayers();
		}
	}

	var80084010 = paused;
}

void lvSetPaused(bool paused)
{
	if (paused) {
		pakDisableRumbleForAllPlayers();
		snd0000fe20();
	} else {
		snd0000fe50();
		pakEnableRumbleForAllPlayers();
	}

	var80084014 = paused;
}

bool lvIsPaused(void)
{
	return var80084014;
}

s32 lvGetDifficulty(void)
{
	return g_Difficulty;
}

void lvSetDifficulty(s32 difficulty)
{
	if (difficulty < DIFF_A || difficulty > DIFF_PD) {
		difficulty = DIFF_A;
	}

	g_Difficulty = difficulty;
}

void lvSetMpTimeLimit60(u32 limit)
{
	g_MpTimeLimit60 = limit;
}

void lvSetMpScoreLimit(u32 limit)
{
	g_MpScoreLimit = limit;
}

void lvSetMpTeamScoreLimit(u32 limit)
{
	g_MpTeamScoreLimit = limit;
}

f32 lvGetStageTimeInSeconds(void)
{
	return g_StageTimeElapsed1f;
}

s32 lvGetStageTime60(void)
{
	return g_StageTimeElapsed60;
}

u32 func0f16ce04(u32 arg0)
{
	return arg0;
}
