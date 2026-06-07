#include <ultra64.h>
#include "constants.h"
#include "game/cheats.h"
#include "game/inv.h"
#include "game/bondgun.h"
#include "game/game_0b0fd0.h"
#include "game/player.h"
#include "game/hudmsg.h"
#include "game/playermgr.h"
#include "game/mplayer/setup.h"
#include "game/botcmd.h"
#include "game/botinv.h"
#include "game/lang.h"
#include "game/mplayer/mplayer.h"
#include "game/options.h"
#include "bss.h"
#include "data.h"
#include "types.h"
#ifndef PLATFORM_N64
#include <stdio.h>
#include "net/net.h"
#include "net/netmsg.h"
#endif

u32 var80070590 = 0x00000000;

void mpstatsIncrementPlayerShotCount(struct gset *gset, s32 region)
{
	if (!weaponHasFlag(gset->weaponnum, WEAPONFLAG_DONTCOUNTSHOTS)) {
		g_Vars.currentplayerstats->shotcount[region]++;
	}
}

void mpstatsIncrementPlayerShotCount2(struct gset *gset, s32 region)
{
	if (region == 0) {
		if (!weaponHasFlag(gset->weaponnum, WEAPONFLAG_DONTCOUNTSHOTS)) {
			var80070590 = 1;
			g_Vars.currentplayerstats->shotcount[region]++;
		}
	} else {
		if (var80070590) {
			if (!weaponHasFlag(gset->weaponnum, WEAPONFLAG_DONTCOUNTSHOTS)) {
				g_Vars.currentplayerstats->shotcount[region]++;
			}

			var80070590 = 0;
		}
	}
}

void mpstats0f0b0520(void)
{
	var80070590 = 0;
}

s32 mpstatsGetPlayerShotCountByRegion(u32 type)
{
	return g_Vars.currentplayerstats->shotcount[type];
}

void mpstatsIncrementTotalKillCount(void)
{
	g_Vars.killcount++;
}

void mpstatsIncrementTotalKnockoutCount(void)
{
	g_Vars.knockoutcount++;
}

void mpstatsDecrementTotalKnockoutCount(void)
{
	g_Vars.knockoutcount--;
}

u8 mpstatsGetTotalKnockoutCount(void)
{
	return g_Vars.knockoutcount;
}

u32 mpstatsGetTotalKillCount(void)
{
	return g_Vars.killcount;
}

void mpstatsRecordPlayerKill(void)
{
	char text[256];
	s32 simulkills;
	s32 duration;
	s32 time;

	g_Vars.currentplayerstats->killcount++;
	g_Vars.currentplayer->killsthislife++;

	if (g_Vars.normmplayerisrunning) {
		time = playerGetMissionTime();

		// Show HUD message
		// "Kill count: %d"
		sprintf(text, "%s: %d\n", langGet(L_GUN_001), g_Vars.currentplayerstats->killcount);
		hudmsgCreate(text, HUDMSGTYPE_DEFAULT);

		// Update slowest/fastest two kills
		if (g_Vars.currentplayerstats->killcount > 1) {
			duration = time - g_Vars.currentplayer->lastkilltime60;

			if (duration > g_Vars.currentplayerstats->slowest2kills) {
				g_Vars.currentplayerstats->slowest2kills = duration;
			}

			if (duration < g_Vars.currentplayerstats->fastest2kills) {
				g_Vars.currentplayerstats->fastest2kills = duration;
			}
		}

		// Update max simultaneous kills
		simulkills = 1;

		g_Vars.currentplayer->lastkilltime60_4 = g_Vars.currentplayer->lastkilltime60_3;
		g_Vars.currentplayer->lastkilltime60_3 = g_Vars.currentplayer->lastkilltime60_2;
		g_Vars.currentplayer->lastkilltime60_2 = g_Vars.currentplayer->lastkilltime60;
		g_Vars.currentplayer->lastkilltime60 = time;

		if (g_Vars.currentplayer->lastkilltime60_2 != -1 && g_Vars.currentplayer->lastkilltime60 - g_Vars.currentplayer->lastkilltime60_2 < 120) {
			simulkills++;

			if (g_Vars.currentplayer->lastkilltime60_3 != -1 && g_Vars.currentplayer->lastkilltime60 - g_Vars.currentplayer->lastkilltime60_3 < 120) {
				simulkills++;

				if (g_Vars.currentplayer->lastkilltime60_4 != -1 && g_Vars.currentplayer->lastkilltime60 - g_Vars.currentplayer->lastkilltime60_4 < 120) {
					simulkills++;
				}
			}
		}

		if (simulkills > g_Vars.currentplayerstats->maxsimulkills) {
			g_Vars.currentplayerstats->maxsimulkills = simulkills;
		}
	}
}

s32 mpstatsGetPlayerKillCount(void)
{
	return g_Vars.currentplayerstats->killcount;
}

void mpstatsIncrementPlayerGgKillCount(void)
{
	g_Vars.currentplayerstats->ggkillcount++;
}

void mpstatsRecordPlayerDeath(void)
{
	char buffer[256];

	g_Vars.currentplayer->deathcount++;

	if (g_Vars.normmplayerisrunning) {
		if (g_Vars.currentplayer->deathcount == 1) {
			sprintf(buffer, langGet(L_GUN_002)); // "Died once"
		} else {
			sprintf(buffer, "%s %d %s\n",
					langGet(L_GUN_003), // "Died"
					g_Vars.currentplayer->deathcount,
					langGet(L_GUN_004)); // "times"
		}

		hudmsgCreate(buffer, HUDMSGTYPE_DEFAULT);
	}
}

void mpstatsRecordPlayerSuicide(void)
{
	char text[256];
	s32 simulkills;
	s32 duration;
	s32 time;
	s32 mpindex;
	struct mpchrconfig *mpchr;

	if (g_Vars.normmplayerisrunning) {
		time = playerGetMissionTime();
		mpindex = g_Vars.currentplayerstats->mpindex;

		mpchr = MPCHR(mpindex);

		// Show HUD message
		// "Suicide count: %d"
		sprintf(text, "%s: %d\n", langGet(L_GUN_005), mpchr->killcounts[mpindex]);
		hudmsgCreate(text, HUDMSGTYPE_DEFAULT);

		// Update slowest/fastest two kills
		if (g_Vars.currentplayerstats->killcount > 1) {
			duration = time - g_Vars.currentplayer->lastkilltime60;

			if (duration > g_Vars.currentplayerstats->slowest2kills) {
				g_Vars.currentplayerstats->slowest2kills = duration;
			}

			if (duration < g_Vars.currentplayerstats->fastest2kills) {
				g_Vars.currentplayerstats->fastest2kills = duration;
			}
		}

		// Update max simultaneous kills
		simulkills = 1;

		g_Vars.currentplayer->lastkilltime60_4 = g_Vars.currentplayer->lastkilltime60_3;
		g_Vars.currentplayer->lastkilltime60_3 = g_Vars.currentplayer->lastkilltime60_2;
		g_Vars.currentplayer->lastkilltime60_2 = g_Vars.currentplayer->lastkilltime60;
		g_Vars.currentplayer->lastkilltime60 = time;

		if (g_Vars.currentplayer->lastkilltime60_2 != -1 && g_Vars.currentplayer->lastkilltime60 - g_Vars.currentplayer->lastkilltime60_2 < 120) {
			simulkills++;

			if (g_Vars.currentplayer->lastkilltime60_3 != -1 && g_Vars.currentplayer->lastkilltime60 - g_Vars.currentplayer->lastkilltime60_3 < 120) {
				simulkills++;

				if (g_Vars.currentplayer->lastkilltime60_4 != -1 && g_Vars.currentplayer->lastkilltime60 - g_Vars.currentplayer->lastkilltime60_4 < 120) {
					simulkills++;
				}
			}
		}

		if (simulkills > g_Vars.currentplayerstats->maxsimulkills) {
			g_Vars.currentplayerstats->maxsimulkills = simulkills;
		}
	}
}

void mpstatsRecordDeath(s32 aplayernum, s32 vplayernum)
{
	s32 vmpindex = -1;
	struct mpchrconfig *vmpchr = NULL;
	s32 ampindex;
	struct mpchrconfig *ampchr = NULL;
	s32 prevplayernum;
	char text[256];

	// On a network client the server owns kill/death counters and broadcasts
	// them via SVC_SCORE — see netmsgSvcScoreRead. Skipping the local writes
	// here keeps the client from double-counting (chrDamage runs on both
	// sides via SVC_CHR_DAMAGE relay so this function fires on both) and
	// from drifting on dropped packets. The hudmsg "Killed by X" /
	// "Killed X" lines are still emitted because they're per-local-player
	// UI feedback, not match state.
#ifndef PLATFORM_N64
	const bool ownsStats = (g_NetMode != NETMODE_CLIENT);
#else
	const bool ownsStats = true;
#endif

	if (g_Vars.normmplayerisrunning && g_MpSetup.scenario == MPSCENARIO_POPACAP) {
		pacHandleDeath(aplayernum, vplayernum);
	}

#ifndef PLATFORM_N64
	// Paint the Map: a kill claims the killer's current room for their team
	// (paintHandleDeath no-ops on clients; ownership rides SVC_PAINT_STATE)
	if (g_Vars.normmplayerisrunning && g_MpSetup.scenario == MPSCENARIO_PAINTROOM) {
		paintHandleDeath(aplayernum, vplayernum);
	}
#endif

	// Find attacker and victim mpchrs
	if (aplayernum >= 0) {
		ampindex = func0f18d074(aplayernum);

		if (ampindex >= 0) {
			ampchr = MPCHR(ampindex);
		}
	}

	if (vplayernum >= 0) {
		vmpindex = func0f18d074(vplayernum);

		if (vmpindex >= 0) {
			vmpchr = MPCHR(vmpindex);
		}
	}

	if (vplayernum >= 0 && aplayernum == vplayernum) {
		// Player suicide
		if (vmpchr && vmpindex >= 0 && ownsStats) {
			vmpchr->numdeaths++;
			vmpchr->killcounts[vmpindex]++;
		}

		if (vplayernum < PLAYERCOUNT()) {
			prevplayernum = g_Vars.currentplayernum;
			setCurrentPlayerNum(vplayernum);
			mpstatsRecordPlayerSuicide();
			setCurrentPlayerNum(prevplayernum);
		}
	} else {
		// Normal kill
		if (vplayernum >= 0) {
			if (vmpchr && ownsStats) {
				vmpchr->numdeaths++;
			}

			if (vplayernum < PLAYERCOUNT()) {
				// Victim was a player
				prevplayernum = g_Vars.currentplayernum;
				setCurrentPlayerNum(vplayernum);

				if (g_Vars.normmplayerisrunning && aplayernum >= 0) {
					// "Killed by %s"
					sprintf(text, "%s %s", langGet(L_MISC_183), g_MpAllChrConfigPtrs[aplayernum]->name);
					hudmsgCreate(text, HUDMSGTYPE_DEFAULT);
				}

				mpstatsRecordPlayerDeath();
				setCurrentPlayerNum(prevplayernum);
			}
		}

		if (ampchr && vmpindex >= 0 && ownsStats) {
			ampchr->killcounts[vmpindex]++;
		}

		if (aplayernum >= 0 && aplayernum < PLAYERCOUNT()) {
			// Attacker was a player
			prevplayernum = g_Vars.currentplayernum;
			setCurrentPlayerNum(aplayernum);

			if (g_Vars.normmplayerisrunning && vplayernum >= 0) {
				// "Killed %s"
				sprintf(text, "%s %s", langGet(L_MISC_184), g_MpAllChrConfigPtrs[vplayernum]->name);
				hudmsgCreate(text, HUDMSGTYPE_DEFAULT);
			}

			mpstatsRecordPlayerKill();
			setCurrentPlayerNum(prevplayernum);
		}

		// If someone killed an aibot
		if (g_Vars.normmplayerisrunning
				&& aplayernum >= 0
				&& vplayernum >= PLAYERCOUNT()
				&& aplayernum != vplayernum
				&& ownsStats) {
			g_MpAllChrPtrs[vplayernum]->aibot->lastkilledbyplayernum = aplayernum;
		}
	}

	if (g_Vars.normmplayerisrunning && aplayernum >= 0 && g_MpAllChrPtrs[aplayernum]->aibot && ownsStats) {
		s32 index = mpGetWeaponSlotByWeaponNum(g_MpAllChrPtrs[aplayernum]->aibot->weaponnum);

		if (index >= 0) {
			if (aplayernum == vplayernum) {
				g_MpAllChrPtrs[aplayernum]->aibot->suicidesbygunfunc[index][g_MpAllChrPtrs[aplayernum]->aibot->gunfunc]++;
			} else {
				g_MpAllChrPtrs[aplayernum]->aibot->killsbygunfunc[index][g_MpAllChrPtrs[aplayernum]->aibot->gunfunc]++;
			}
		}
	}

	if (ownsStats) {
		g_Vars.totalkills++;
	}

#ifndef PLATFORM_N64
	// Kill-feed broadcast + authoritative score sync. Server is the authority
	// here — it sees the definitive kill once via mpstatsRecordDeath and ships
	// a pre-formatted line to all clients (including its own host display via
	// the local netKillFeedAdd call). Clients receive SVC_KILL and only
	// render; they never broadcast back. Names come from g_MpAllChrConfigPtrs
	// which is indexed by the same scheme on both sides (players
	// 0..MAX_PLAYERS-1, then bots), so the server's lookup matches what the
	// receiver expects.
	//
	// Score broadcast: only the attacker and victim mpchrs have changed
	// stats, so we ship a 1- or 2-entry SVC_SCORE delta immediately. The
	// client overwrites local mpchrconfig fields with the server's values,
	// which keeps scoreboards in lockstep even if SVC_CHR_DAMAGE relays
	// drop or arrive out of order.
	if (g_NetMode == NETMODE_SERVER) {
		const char *aName = (aplayernum >= 0 && aplayernum < MAX_MPCHRS
				&& g_MpAllChrConfigPtrs[aplayernum])
			? g_MpAllChrConfigPtrs[aplayernum]->name : NULL;
		const char *vName = (vplayernum >= 0 && vplayernum < MAX_MPCHRS
				&& g_MpAllChrConfigPtrs[vplayernum])
			? g_MpAllChrConfigPtrs[vplayernum]->name : NULL;

		if (vName) {
			// Pass NULL shooter for suicides / environment kills so the
			// receiver renders "victim [died]" instead of "shooter > victim".
			const char *shooterPass =
				(aplayernum < 0 || aplayernum == vplayernum || !aName)
				? NULL : aName;
			// Team bytes for kill-feed colouring. 0xff = unknown — render falls
			// back to the green/red palette. Mpchrconfig stores team as 0..7
			// for MPTEAM_1..MPTEAM_8, which maps directly to g_TeamColours[].
			const u8 vTeam = g_MpAllChrConfigPtrs[vplayernum]->team;
			const u8 aTeam = (shooterPass && aplayernum >= 0 && aplayernum < MAX_MPCHRS
					&& g_MpAllChrConfigPtrs[aplayernum])
				? g_MpAllChrConfigPtrs[aplayernum]->team : 0xff;
			netKillFeedAdd(shooterPass, vName, aTeam, vTeam);
			netbufStartWrite(&g_NetMsgRel);
			netmsgSvcKillWrite(&g_NetMsgRel, shooterPass, vName, aTeam, vTeam);
			netSend(NULL, &g_NetMsgRel, true, NETCHAN_CONTROL);
		}

		// Build the score-delta entry list. Skip duplicates so suicides
		// only ship a single entry instead of the same index twice.
		s32 score_indexes[2];
		s32 score_count = 0;
		if (aplayernum >= 0 && aplayernum < MAX_MPCHRS) {
			score_indexes[score_count++] = aplayernum;
		}
		if (vplayernum >= 0 && vplayernum < MAX_MPCHRS && vplayernum != aplayernum) {
			score_indexes[score_count++] = vplayernum;
		}
		if (score_count > 0) {
			netbufStartWrite(&g_NetMsgRel);
			netmsgSvcScoreWrite(&g_NetMsgRel, score_indexes, score_count);
			netSend(NULL, &g_NetMsgRel, true, NETCHAN_CONTROL);
		}
	}
#endif
}
