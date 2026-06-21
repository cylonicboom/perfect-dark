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

#ifndef PLATFORM_N64
	// CENTRAL attacker recovery (server authority). Any death path that couldn't
	// resolve the shooter (aplayernum < 0) or that fell back to the victim itself
	// (aplayernum == vplayernum, the dead-lastshooter suicide bug) is recovered here
	// from the victim's most-recent damager (chr->lastattacker), the same derivation
	// the victim's own client uses in netmsgSvcPlayerStatsRead. This is the single
	// choke point for the kill feed (SVC_KILL) + score (SVC_SCORE), so it fixes
	// gunfire, explosion, env/fall/knockback, AND chr-death paths at once — without
	// it a player shot then finished by a non-gunfire trigger shows as "X died" /
	// suicide with no credit. g_MpAllChrPtrs[vplayernum] is the victim chr for both
	// player deaths (vplayernum == player slot == its g_MpAllChrPtrs index) and chr
	// deaths (vplayernum == mpPlayerGetIndex). Only fires when we'd otherwise record
	// no killer, so a genuine self/environment death (no recent attacker) still reads
	// as a suicide.
	if (ownsStats && g_Vars.normmplayerisrunning
			&& (aplayernum < 0 || aplayernum == vplayernum)
			&& vplayernum >= 0 && vplayernum < MAX_MPCHRS && g_MpAllChrPtrs[vplayernum]) {
		struct chrdata *vchr = g_MpAllChrPtrs[vplayernum];
		struct chrdata *atk = vchr->lastattacker;
		s32 recovered = -1;
		if (atk && atk->prop && atk != vchr) {
			if (atk->prop->type == PROPTYPE_PLAYER) {
				recovered = playermgrGetPlayerNumByProp(atk->prop);
			} else if (atk->prop->type == PROPTYPE_CHR && atk->aibot) {
				recovered = mpPlayerGetIndex(atk);
			}
		}
		netDiagLogf("killattrib", "a_in=%d v=%d cur=%d latk=%d recovered=%d opt=%d",
				aplayernum, vplayernum, (s32)g_Vars.currentplayernum,
				(atk && atk->prop) ? mpPlayerGetIndex(atk) : -1, recovered,
				(g_MpSetup.options & MPOPTION_LASTATTACKERKILL) ? 1 : 0);
		// "Last Attacker Attribution" (Combat Sim More Options) — only credit the
		// recent attacker when the host enabled it, so pushes / knockback / suicide
		// plays reward the attacker. Off = vanilla (these read as suicides).
		if ((g_MpSetup.options & MPOPTION_LASTATTACKERKILL)
				&& recovered >= 0 && recovered != vplayernum) {
			aplayernum = recovered;
		}
	}
#endif

	if (g_Vars.normmplayerisrunning && g_MpSetup.scenario == MPSCENARIO_POPACAP) {
		pacHandleDeath(aplayernum, vplayernum);
	}

#ifndef PLATFORM_N64
	// Graffiti: a kill claims the killer's current room for their team
	// (paintHandleDeath no-ops on clients; ownership rides SVC_PAINT_STATE)
	if (g_Vars.normmplayerisrunning && g_MpSetup.scenario == MPSCENARIO_PAINTROOM) {
		paintHandleDeath(aplayernum, vplayernum);
	}

	// Global Lives system: every death spends one of the victim's lives
	// (elimHandleDeath no-ops on clients; lives ride SVC_ELIM_STATE)
	if (g_Vars.normmplayerisrunning && g_MpSetup.elimlives > 0) {
		elimHandleDeath(aplayernum, vplayernum);
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

#ifndef PLATFORM_N64
	// Diag: sim-victim kill attribution (the "wrong human credited for a sim
	// kill" bug). Fires on the server for any aibot-victim kill with a resolved
	// attacker. `cur` is the shooter playernum the damage path ran under (set by
	// the CLC_HIT drain / shot sim); `a` is the attacker mpPlayerGetIndex resolved
	// from aprop->chr. If a != cur, aprop->chr resolved to a DIFFERENT index than
	// the actual shooter (g_MpAllChrPtrs aliasing). `aisbot` flags whether the
	// resolved attacker chr is itself a bot. numchrs/pc expose the player/bot
	// packing boundary.
	if (g_NetMode == NETMODE_SERVER && g_Vars.normmplayerisrunning
			&& vplayernum >= PLAYERCOUNT() && aplayernum >= 0 && aplayernum < MAX_MPCHRS) {
		struct chrdata *achr = g_MpAllChrPtrs[aplayernum];
		netDiagLogf("simkill", "a=%d cur=%d v=%d aname=%s aisbot=%d numchrs=%d pc=%d",
				aplayernum, (s32)g_Vars.currentplayernum, vplayernum,
				g_MpAllChrConfigPtrs[aplayernum] ? g_MpAllChrConfigPtrs[aplayernum]->name : "?",
				(achr && achr->aibot) ? 1 : 0, g_MpNumChrs, PLAYERCOUNT());
	}
#endif

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
			// back to the green/red palette (local player red, everyone else
			// green). Mpchrconfig stores team as 0..7 for MPTEAM_1..MPTEAM_8,
			// which maps directly to g_TeamColours[]. Only pass real teams in a
			// TEAM game — in a free-for-all every player has a distinct team
			// value, which would otherwise colour the feed by those per-player
			// teams instead of the intended green/red.
			const bool teamgame = (g_MpSetup.options & MPOPTION_TEAMSENABLED) != 0;
			const u8 vTeam = teamgame ? g_MpAllChrConfigPtrs[vplayernum]->team : 0xff;
			const u8 aTeam = (teamgame && shooterPass && aplayernum >= 0 && aplayernum < MAX_MPCHRS
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

	// Offline Combat Sim kill feed (single machine — no SVC_KILL broadcast).
	// Colour locally: in a free-for-all, local human players are red
	// (NET_KILLFEED_TEAM_LOCAL) and simulants green (NET_KILLFEED_TEAM_NONE);
	// a team game uses the real team colours.
	if (g_NetMode == NETMODE_NONE && g_Vars.normmplayerisrunning) {
		const char *aName = (aplayernum >= 0 && aplayernum < MAX_MPCHRS
				&& g_MpAllChrConfigPtrs[aplayernum])
			? g_MpAllChrConfigPtrs[aplayernum]->name : NULL;
		const char *vName = (vplayernum >= 0 && vplayernum < MAX_MPCHRS
				&& g_MpAllChrConfigPtrs[vplayernum])
			? g_MpAllChrConfigPtrs[vplayernum]->name : NULL;

		if (vName) {
			// Suicide / environment kill: NULL shooter renders "victim [died]".
			const char *shooterPass =
				(aplayernum < 0 || aplayernum == vplayernum || !aName) ? NULL : aName;
			const bool teamgame = (g_MpSetup.options & MPOPTION_TEAMSENABLED) != 0;
			u8 vTeam, aTeam;

			if (teamgame) {
				vTeam = g_MpAllChrConfigPtrs[vplayernum]->team;
				aTeam = shooterPass ? g_MpAllChrConfigPtrs[aplayernum]->team : NET_KILLFEED_TEAM_NONE;
			} else {
				// Players 0..PLAYERCOUNT()-1 are local humans; bots are above.
				vTeam = (vplayernum < PLAYERCOUNT()) ? NET_KILLFEED_TEAM_LOCAL : NET_KILLFEED_TEAM_NONE;
				aTeam = (shooterPass && aplayernum < PLAYERCOUNT()) ? NET_KILLFEED_TEAM_LOCAL : NET_KILLFEED_TEAM_NONE;
			}

			netKillFeedAdd(shooterPass, vName, aTeam, vTeam);
		}
	}

	// Killcam: latch the killer of the LOCAL pawn for SOLO / listen-host (the
	// client path is SVC_KILL). Indices are local-consistent here (no wire), so
	// resolve the chrs directly; netKillcamNoteKill self-filters to the local pawn.
	if (g_NetMode != NETMODE_CLIENT && aplayernum >= 0 && aplayernum < MAX_MPCHRS
			&& vplayernum >= 0 && vplayernum < MAX_MPCHRS) {
		netKillcamNoteKill(g_MpAllChrPtrs[aplayernum], g_MpAllChrPtrs[vplayernum]);
	}
#endif
}
