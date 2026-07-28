/**
 * Race (port-only, MPSCENARIO_RACE) — checkpoint racing over the King of the
 * Hill setup.
 *
 * The stage's KoH hillpads become an ordered checkpoint course: every racer
 * must touch hill 1, 2, 3, ... N in order; touching the last wraps back to
 * hill 1 — that's one lap. First to the configured lap count ("Laps", 1-10)
 * takes finishing position 1, but the match keeps running until every human
 * has finished OR the "Finish Timer" (0-120s, started by the first finisher;
 * 0 = end with the winner) expires. The end condition is polled from lv.c's
 * match-end block via raceShouldEndMatch (g_NumReasonsToEndMpMatch resets
 * each frame there — never increment it from a scenario tick).
 *
 * All checkpoint rooms tint the KoH hill green; the radar always shows a dot
 * on YOUR next checkpoint (per-player — room tints are global so they can't
 * point at it). Progress, finish order and the finish timer are
 * host-authoritative and broadcast via SVC_RACE_STATE (on change + 1s
 * heartbeat). Bots do not race — they fight as usual and sit at the bottom
 * of the scoreboard.
 *
 * Scoring is a synthetic ranking value (finishers above racers, racers by
 * course progress), so mpApplyLimits forces the score/team-score limits
 * unlimited for this scenario — only the race end condition and the time
 * limit end the match.
 *
 * This is a HI-BIT scenario (id 8): the wad's 3-bit scenario field stores
 * id&7 (= Combat) and the 4th bit rides the v10 tail. At parse time the
 * block dispatches as Combat, so the scenario save slot (laps + finish
 * timer) arrives via the g_ScenarioSaveSlotRaw capture + raceApplySaveSlot.
 * See the constants.h MPSCENARIO note and docs/PORT_ELIMINATION.md appendix.
 */

// Host signal: race progress / finish state changed this frame, so
// netEndFrame should broadcast SVC_RACE_STATE. Cleared by the net layer.
u8 g_MpRaceDirty = 0;

// Sanitized settings (wad tails and admin pushes can carry anything).
static s32 raceSaneLaps(void)
{
	s32 n = g_MpSetup.racelaps;

	if (n < 1 || n > 10) {
		n = 3;
	}

	return n;
}

static s32 raceSanePity(void)
{
	s32 n = g_MpSetup.racepitytime;

	if (n > 120) {
		n = 30;
	}

	return n;
}

MenuItemHandlerResult menuhandlerMpRaceLaps(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GETSLIDER:
		data->slider.value = raceSaneLaps() - 1;
		break;
	case MENUOP_SET:
		g_MpSetup.racelaps = (u8)(data->slider.value + 1);
		break;
	case MENUOP_GETSLIDERLABEL:
		sprintf(data->slider.label, "%d\n", (s32)data->slider.value + 1);
		break;
	}

	return 0;
}

MenuItemHandlerResult menuhandlerMpRacePityTime(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GETSLIDER:
		data->slider.value = raceSanePity();
		break;
	case MENUOP_SET:
		g_MpSetup.racepitytime = (u8)data->slider.value;
		break;
	case MENUOP_GETSLIDERLABEL:
		if (data->slider.value == 0) {
			sprintf(data->slider.label, "Instant\n");
		} else {
			sprintf(data->slider.label, "%ds\n", (s32)data->slider.value);
		}
		break;
	}

	return 0;
}

struct menuitem g_MpRaceOptionsMenuItems[] = {
	{
		MENUITEMTYPE_CHECKBOX,
		0,
		MENUITEMFLAG_LOCKABLEMINOR,
		L_MPMENU_222, // "One-Hit Kills"
		MPOPTION_ONEHITKILLS,
		menuhandlerMpOneHitKills,
	},
	{
		MENUITEMTYPE_DROPDOWN,
		0,
		MENUITEMFLAG_LOCKABLEMINOR,
		L_MPMENU_223, // "Slow Motion"
		0,
		menuhandlerMpSlowMotion,
	},
	{
		MENUITEMTYPE_CHECKBOX,
		0,
		MENUITEMFLAG_LOCKABLEMINOR,
		L_MPMENU_224, // "Fast Movement"
		MPOPTION_FASTMOVEMENT,
		menuhandlerMpCheckboxOption,
	},
	{
		MENUITEMTYPE_CHECKBOX,
		0,
		MENUITEMFLAG_LOCKABLEMINOR,
		L_MPMENU_225, // "Display Team"
		MPOPTION_DISPLAYTEAM,
		menuhandlerMpDisplayTeam,
	},
	{
		MENUITEMTYPE_CHECKBOX,
		0,
		MENUITEMFLAG_LOCKABLEMINOR,
		L_MPMENU_226, // "No Radar"
		MPOPTION_NORADAR,
		menuhandlerMpCheckboxOption,
	},
	{
		MENUITEMTYPE_CHECKBOX,
		0,
		MENUITEMFLAG_LOCKABLEMINOR,
		L_MPMENU_227, // "No Auto-Aim"
		MPOPTION_NOAUTOAIM,
		menuhandlerMpCheckboxOption,
	},
	{
		MENUITEMTYPE_SEPARATOR,
		0,
		0,
		0,
		0,
		NULL,
	},
	{
		// Laps to finish (touch every checkpoint in order = one lap).
		// param3 = slider max (value + 1 = laps).
		MENUITEMTYPE_SLIDER,
		0,
		MENUITEMFLAG_LOCKABLEMINOR | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Laps",
		9,
		menuhandlerMpRaceLaps,
	},
	{
		// Seconds the rest of the field gets after the first finisher
		// before the match force-ends; 0 = end with the winner.
		MENUITEMTYPE_SLIDER,
		0,
		MENUITEMFLAG_LOCKABLEMINOR | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Finish Timer",
		120,
		menuhandlerMpRacePityTime,
	},
	{
		MENUITEMTYPE_SEPARATOR,
		0,
		0,
		0,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_CLOSESDIALOG,
		L_MPMENU_239, // "Back"
		0,
		NULL,
	},
	{ MENUITEMTYPE_END },
};

struct menudialogdef g_MpRaceOptionsMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Race Options\n",
	g_MpRaceOptionsMenuItems,
	mpOptionsMenuDialog,
	MENUDIALOGFLAG_MPLOCKABLE | MENUDIALOGFLAG_LITERAL_TEXT,
	&g_ExtGameOptionsMenuDialog,
};

void raceInit(void)
{
	struct scenariodata_race *race = &g_ScenarioData.race;
	s32 i;

	race->hillcount = 0;
	race->finishcount = 0;
	race->humancount = 0;
	race->initdone = 0;
	race->pitystarted = 0;
	race->pity240 = 0;

	for (i = 0; i < (s32)ARRAYCOUNT(race->hillpads); i++) {
		race->hillpads[i] = -1;
		race->hillrooms[i] = -1;
	}

	for (i = 0; i < (s32)ARRAYCOUNT(race->nextcp); i++) {
		race->nextcp[i] = 0;
		race->lapsdone[i] = 0;
		race->finishpos[i] = 0;
	}

	g_MpRaceDirty = 0;
}

// INTROCMD_HILL collector — the kohAddHill/zonesAddHill pattern, into the
// race union member. Checkpoint order is stage-data order.
void raceAddHill(s32 *cmd)
{
	struct scenariodata_race *race = &g_ScenarioData.race;

	if (race->hillcount < (s16)ARRAYCOUNT(race->hillpads)) {
		race->hillpads[race->hillcount] = (s16)cmd[1];
		race->hillcount++;
	}
}

void raceInitProps(void)
{
	struct scenariodata_race *race = &g_ScenarioData.race;
	struct pad pad;
	s32 i;

	// Resolve every checkpoint's room + position and highlight all of them
	// (the KoH hill green, via raceHighlightRoom) so the course is visible.
	for (i = 0; i < race->hillcount; i++) {
		padUnpack(race->hillpads[i], PADFIELD_POS | PADFIELD_ROOM, &pad);
		race->hillrooms[i] = pad.room;
		race->hillpos[i] = pad.pos;
		roomSetLightOp(race->hillrooms[i], LIGHTOP_HIGHLIGHT, 0, 0, 0);
	}

	for (i = 0; i < (s32)ARRAYCOUNT(race->nextcp); i++) {
		race->nextcp[i] = 0;
		race->lapsdone[i] = 0;
		race->finishpos[i] = 0;
	}

	race->finishcount = 0;
	race->humancount = 0;
	race->initdone = 0;
	race->pitystarted = 0;
	race->pity240 = 0;

	g_MpRaceDirty = 0;
}

static void raceLazyInit(void)
{
	struct scenariodata_race *race = &g_ScenarioData.race;
	s32 i;

	race->humancount = 0;

	for (i = 0; i < g_MpNumChrs && i < (s32)ARRAYCOUNT(race->nextcp); i++) {
		if (g_MpAllChrPtrs[i] && g_MpAllChrPtrs[i]->aibot == NULL) {
			race->humancount++;
		}
	}

	race->initdone = 1;

#ifndef PLATFORM_N64
	if (g_NetMode != NETMODE_CLIENT) {
		g_MpRaceDirty = 1;
	}
#endif
}

struct scenariodata_race *raceGetData(void)
{
	return &g_ScenarioData.race;
}

void raceTick(void)
{
	struct scenariodata_race *race = &g_ScenarioData.race;
	s32 i;

	if (race->hillcount <= 0) {
		return;
	}

	if (!race->initdone) {
		if (g_MpNumChrs > 0) {
			raceLazyInit();
		}

		return;
	}

	// Checkpoint detection + finish order are host-authoritative; clients
	// receive progress via SVC_RACE_STATE.
#ifndef PLATFORM_N64
	if (g_NetMode != NETMODE_CLIENT)
#endif
	{
		for (i = 0; i < g_MpNumChrs && i < (s32)ARRAYCOUNT(race->nextcp); i++) {
			struct chrdata *chr = g_MpAllChrPtrs[i];

			// bots don't race; the dead can't pass checkpoints; finishers
			// are done
			if (chr == NULL || chr->prop == NULL || chr->aibot != NULL
					|| race->finishpos[i] != 0 || chrIsDead(chr)) {
				continue;
			}

			if (chr->prop->rooms[0] == race->hillrooms[race->nextcp[i]]) {
				race->nextcp[i]++;

				// touched the last checkpoint: lap complete, wrap to hill 1
				if (race->nextcp[i] >= race->hillcount) {
					race->nextcp[i] = 0;
					race->lapsdone[i]++;

					if (race->lapsdone[i] >= raceSaneLaps()) {
						race->finishcount++;
						race->finishpos[i] = race->finishcount;

						// the first finisher starts the finish timer
						if (race->finishcount == 1) {
							race->pitystarted = 1;
							race->pity240 = raceSanePity() * TICKS(240);
						}
					}
				}

				g_MpRaceDirty = 1;
			}
		}
	}

	// Finish timer ticks on every machine (clients display it locally; the
	// heartbeat corrects drift; the END decision is host-side in
	// raceShouldEndMatch).
	if (race->pitystarted && race->pity240 > 0) {
		race->pity240 -= g_Vars.lvupdate240;

		if (race->pity240 < 0) {
			race->pity240 = 0;
		}
	}
}

/**
 * True when the race has been decided: someone finished AND (every human has
 * finished OR the finish timer expired).
 *
 * Polled from lv.c's match-end block beside the score-limit checks — NOT
 * from raceTick: g_NumReasonsToEndMpMatch is recomputed from ZERO there each
 * frame, so a tick-side increment is wiped before the end test reads it.
 */
bool raceShouldEndMatch(void)
{
	struct scenariodata_race *race = &g_ScenarioData.race;

	if (g_MpSetup.scenario != MPSCENARIO_RACE || !g_Vars.normmplayerisrunning
			|| !race->initdone || race->finishcount == 0) {
		return false;
	}

	if (race->humancount > 0 && race->finishcount >= race->humancount) {
		return true;
	}

	return race->pitystarted && race->pity240 <= 0;
}

Gfx *raceRenderHud(Gfx *gdl)
{
	struct scenariodata_race *race = &g_ScenarioData.race;
	struct chrdata *chr = g_Vars.currentplayer->prop->chr;
	s32 textwidth;
	s32 textheight;
	s32 x;
	s32 y;
	s32 chrnum;
	char text[48];

	if (race->hillcount <= 0 || !race->initdone || chr == NULL) {
		return gdl;
	}

	chrnum = mpPlayerGetIndex(chr);

	if (chrnum < 0 || chrnum >= (s32)ARRAYCOUNT(race->nextcp)) {
		return gdl;
	}

	// Top centre: lap + next-checkpoint progress while racing, finishing
	// position once done; the finish-timer countdown (ceil seconds) appends
	// once the first finisher crosses.
	{
		s32 pitysecs = -1;

		if (race->pitystarted && race->finishcount < race->humancount) {
			pitysecs = (race->pity240 + TICKS(240) - 1) / TICKS(240);

			if (pitysecs < 0) {
				pitysecs = 0;
			}
		}

		if (race->finishpos[chrnum] != 0) {
			if (pitysecs >= 0) {
				sprintf(text, "Finished #%d | %d", race->finishpos[chrnum], pitysecs);
			} else {
				sprintf(text, "Finished #%d", race->finishpos[chrnum]);
			}
		} else if (pitysecs >= 0) {
			sprintf(text, "Lap %d/%d - CP %d/%d | %d",
					race->lapsdone[chrnum] + 1, raceSaneLaps(),
					race->nextcp[chrnum] + 1, race->hillcount, pitysecs);
		} else {
			sprintf(text, "Lap %d/%d - CP %d/%d",
					race->lapsdone[chrnum] + 1, raceSaneLaps(),
					race->nextcp[chrnum] + 1, race->hillcount);
		}
	}

	x = viGetViewLeft() + viGetViewWidth() / 2;
	y = viGetViewTop() + 10;

#ifndef PLATFORM_N64
	{
		const s32 playercount = PLAYERCOUNT();
		if (playercount < 2 || (playercount == 2 && optionsGetScreenSplit() == SCREENSPLIT_HORIZONTAL)) {
			gSPSetExtraGeometryModeEXT(gdl++, G_ASPECT_CENTER_EXT);
		}
	}
#endif

	gdl = text0f153628(gdl);
	textMeasure(&textheight, &textwidth, text, g_CharsHandelGothicXs, g_FontHandelGothicXs, 0);

	x -= textwidth / 2;
	textwidth += x;
	textheight += y;

	gdl = text0f153990(gdl, x, y, textwidth, textheight);
	gdl = textRender(gdl, &x, &y, text, g_CharsHandelGothicXs, g_FontHandelGothicXs,
			0x00ff00a0, 0xa0, viGetWidth(), viGetHeight(), 0, 0);
	gdl = text0f153780(gdl);

#ifndef PLATFORM_N64
	gSPClearExtraGeometryModeEXT(gdl++, G_ASPECT_MODE_EXT);
#endif

	return gdl;
}

/**
 * Synthetic ranking score: finishers rank by finishing position (above every
 * racer), racers by course progress. mpApplyLimits forces the score limits
 * unlimited for Race so these values can't trip an early end.
 */
void raceCalculatePlayerScore(struct mpchrconfig *mpchr, s32 mpchrnum, s32 *score, s32 *deaths)
{
	struct scenariodata_race *race = &g_ScenarioData.race;
	// mpchrnum is a chrslots SLOT index (bots sit at 16+), but raceTick writes
	// nextcp/lapsdone/finishpos indexed by the PACKED g_MpAllChrPtrs position.
	// Reading them with the raw slot returned another combatant's progress
	// whenever the two differ. htmCalculatePlayerScore does this conversion;
	// race didn't. (Same class as the packed-vs-slot bug noted for proto 82.)
	s32 index = func0f18d0e8(mpchrnum);

	if (index >= 0 && index < (s32)ARRAYCOUNT(race->nextcp)) {
		if (race->finishpos[index] != 0) {
			*score = 2000 - race->finishpos[index];
		} else {
			*score = race->lapsdone[index] * 20 + race->nextcp[index];
		}
	} else {
		*score = 0;
	}

	*deaths = mpchr->numdeaths;
}

Gfx *raceRadarExtra(Gfx *gdl)
{
	struct scenariodata_race *race = &g_ScenarioData.race;
	struct chrdata *chr = g_Vars.currentplayer->prop->chr;
	s32 chrnum;

	if (race->hillcount <= 0 || !race->initdone || chr == NULL) {
		return gdl;
	}

	chrnum = mpPlayerGetIndex(chr);

	// Always-on dot at the local player's NEXT checkpoint (per-player — the
	// global room tints can't point at it). Finishers get no dot.
	if (chrnum >= 0 && chrnum < (s32)ARRAYCOUNT(race->nextcp)
			&& race->finishpos[chrnum] == 0) {
		struct coord dist;
		s32 cp = race->nextcp[chrnum];

		dist.x = race->hillpos[cp].x - g_Vars.currentplayer->prop->pos.x;
		dist.y = race->hillpos[cp].y - g_Vars.currentplayer->prop->pos.y;
		dist.z = race->hillpos[cp].z - g_Vars.currentplayer->prop->pos.z;

		gdl = radarDrawDot(gdl, NULL, &dist, 0x00ff0000, 0, 1);
	}

	return gdl;
}

// Cap on the candidate pad subset handed to playerChooseSpawnLocation — its
// internal verybadpads/badpads/padsqdists arrays are [24] (the Graffiti
// spawnfunc constraint).
#define RACE_SPAWN_MAXPADS 24

/**
 * Race spawns: every PLAYER spawns at checkpoint 1 — spawn pads in its room
 * first, then pads in a neighbouring room, then the stock random spawn as a
 * last resort (a stage may have no pads near hill 1). Applies to the match
 * start (the grid) and to respawns (death sends you back to the start area;
 * checkpoint progress is kept). Bots fall through to random spawns — they
 * don't race, so they shouldn't pile up on the start line.
 *
 * Both host and client run this from the same locally-derived course data;
 * the host's authoritative position correction reconciles any RNG divergence
 * in the exact pad chosen (the established net-spawn behaviour).
 */
bool raceChooseSpawnLocation(f32 arg0, struct coord *pos, RoomNum *rooms, struct prop *prop, f32 *arg4)
{
	struct scenariodata_race *race = &g_ScenarioData.race;
	s16 candidates[RACE_SPAWN_MAXPADS];
	s32 numcandidates = 0;
	RoomNum neighbours[24];
	RoomNum cproom;
	struct pad pad;
	s32 start;
	s32 p;
	s32 j;
	s32 k;

	if (race->hillcount <= 0 || g_NumSpawnPoints <= 0) {
		return false;
	}

	// bots don't race — stock random spawns keep them off the start line
	if (prop == NULL || prop->chr == NULL || prop->chr->aibot != NULL) {
		return false;
	}

	cproom = race->hillrooms[0];

	// Tier 1: pads in checkpoint 1's room. Random scan offset so repeated
	// spawns draw from the whole set.
	start = rngRandom() % g_NumSpawnPoints;

	for (j = 0; j < g_NumSpawnPoints && numcandidates < RACE_SPAWN_MAXPADS; j++) {
		p = (start + j) % g_NumSpawnPoints;
		padUnpack(g_SpawnPoints[p], PADFIELD_ROOM, &pad);

		if (pad.room == cproom) {
			candidates[numcandidates++] = g_SpawnPoints[p];
		}
	}

	// Tier 2: pads in a room adjacent to checkpoint 1.
	if (numcandidates == 0) {
		bgRoomGetNeighbours(cproom, neighbours, 20);

		for (j = 0; j < g_NumSpawnPoints && numcandidates < RACE_SPAWN_MAXPADS; j++) {
			p = (start + j) % g_NumSpawnPoints;
			padUnpack(g_SpawnPoints[p], PADFIELD_ROOM, &pad);

			for (k = 0; k < 20 && neighbours[k] != -1; k++) {
				if (pad.room == neighbours[k]) {
					candidates[numcandidates++] = g_SpawnPoints[p];
					break;
				}
			}
		}
	}

	// Tier 3: nothing near the start — let the caller pick at random.
	if (numcandidates == 0) {
		return false;
	}

	*arg4 = playerChooseSpawnLocation(arg0, pos, rooms, prop, candidates, numcandidates);
	return true;
}

bool raceIsRoomHighlighted(RoomNum room)
{
	struct scenariodata_race *race = &g_ScenarioData.race;
	s32 i;

	for (i = 0; i < race->hillcount; i++) {
		if (race->hillrooms[i] == room) {
			return true;
		}
	}

	return false;
}

void raceHighlightRoom(RoomNum roomnum, s32 *arg1, s32 *arg2, s32 *arg3)
{
	if (raceIsRoomHighlighted(roomnum)) {
		// the KoH unoccupied-hill green for every checkpoint
		f32 a = *arg1;
		f32 c = *arg3;

		a *= 0.25f;
		c *= 0.25f;

		*arg1 = a;
		*arg3 = c;
	}
}

/**
 * Client-side SVC_RACE_STATE apply: authoritative per-racer progress, finish
 * order and the finish timer. initdone is forced so the HUD renders from
 * wire state even before the local lazy seed ran.
 */
void raceApplyWireState(const u8 *nextcp, const u8 *lapsdone, const u8 *finishpos,
		u8 finishcount, u8 humancount, u8 pitystarted, s32 pity240)
{
	struct scenariodata_race *race = &g_ScenarioData.race;
	s32 i;

	for (i = 0; i < (s32)ARRAYCOUNT(race->nextcp); i++) {
		race->nextcp[i] = (race->hillcount > 0) ? (u8)(nextcp[i] % race->hillcount) : 0;
		race->lapsdone[i] = lapsdone[i];
		race->finishpos[i] = finishpos[i];
	}

	race->finishcount = finishcount;
	race->humancount = humancount;
	race->pitystarted = pitystarted;
	race->pity240 = pity240;
	race->initdone = 1;
}

/**
 * Per-setup save: laps + finish timer pack into the 32-bit scenario save
 * slot. Bits 0-3 = racelaps, bits 4-10 = racepitytime. The READ side can't
 * go through a readsavefunc: at parse time the wad's 3-bit scenario field
 * reads as Combat (Race's id 8 rides the v10 tail high-bit), so the slot is
 * consumed by scenarioReadSave's default branch — which captures it raw in
 * g_ScenarioSaveSlotRaw — and mpsetupfileLoadWad re-applies it here once the
 * high-bit is known. raceWriteSave dispatches normally (write-time scenario
 * is the real 8).
 */
void raceApplySaveSlot(u32 val)
{
	g_MpSetup.racelaps = val & 0xf;
	g_MpSetup.racepitytime = (val >> 4) & 0x7f;

	if (g_MpSetup.racelaps < 1 || g_MpSetup.racelaps > 10) {
		g_MpSetup.racelaps = 3;
	}

	if (g_MpSetup.racepitytime > 120) {
		g_MpSetup.racepitytime = 30;
	}
}

void raceWriteSave(struct savebuffer *buffer)
{
	savebufferOr(buffer, ((u32)(g_MpSetup.racepitytime & 0x7f) << 4)
			| (g_MpSetup.racelaps & 0xf), 32);
}
