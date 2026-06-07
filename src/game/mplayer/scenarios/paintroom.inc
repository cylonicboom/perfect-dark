/**
 * Graffiti (port-only, MPSCENARIO_PAINTROOM; formerly "Paint the Map")
 *
 * A team game inspired by Splatoon / Tony Hawk graffiti: whenever a living
 * combatant stands in a room, that room's floor is tinted their team's colour.
 * Rooms are re-paintable — the last team to cross a room owns it. At the time
 * limit the team owning the most rooms wins.
 *
 * Room ownership is host-authoritative (computed in paintTick, gated to the
 * server) and broadcast to clients via SVC_PAINT_STATE. The tint reuses the
 * existing scenario room-highlight pipeline (scenarioIsRoomHighlighted /
 * scenarioHighlightRoom -> dlights.c), exactly like Capture the Case's static
 * base rooms — the only difference is the owner set is dynamic.
 *
 * Per-room ownership lives in g_ScenarioData.paint.roomowner (a per-stage array
 * sized g_Vars.roomcount, MEMPOOL_STAGE). roomowner[room] holds the owning team
 * +1, with 0 meaning unpainted. Once painted a room never reverts to unpainted
 * (only its owning team changes), so the wire format is a simple full list of
 * owned rooms applied idempotently.
 */

// Host signal: the painted set changed this frame, so netEndFrame should
// broadcast SVC_PAINT_STATE. Cleared by the net layer after sending.
u8 g_MpPaintDirty = 0;

// Cap on the candidate pad subset handed to playerChooseSpawnLocation — its
// internal verybadpads/badpads/padsqdists arrays are [24], so numpads must not
// exceed that.
#define PAINT_SPAWN_MAXPADS 24

struct menuitem g_MpPaintOptionsMenuItems[] = {
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
		MENUITEMTYPE_CHECKBOX,
		0,
		MENUITEMFLAG_LOCKABLEMINOR,
		L_OPTIONS_257, // "Friendly Fire"
		MPOPTION_FRIENDLYFIRE,
		menuhandlerMpDisplayTeam,
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
		// Port-only: "Owned Room Spawn" lives in the high 32 bits of
		// g_MpSetup.options, so it uses the high-word checkbox handler.
		// param3 carries the high-word bit (MPOPTION_OWNEDROOMSPAWN >> 32),
		// which the handler shifts back up by 32. When on, dying players
		// respawn in (or adjacent to) a room their team already owns.
		MENUITEMTYPE_CHECKBOX,
		0,
		MENUITEMFLAG_LOCKABLEMINOR | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Owned Room Spawn",
		MPOPTION_OWNEDROOMSPAWN >> 32,
		menuhandlerMpCheckboxPortOption,
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

struct menudialogdef g_MpPaintOptionsMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Graffiti Options\n",
	g_MpPaintOptionsMenuItems,
	mpOptionsMenuDialog,
	MENUDIALOGFLAG_MPLOCKABLE | MENUDIALOGFLAG_LITERAL_TEXT,
	&g_ExtGameOptionsMenuDialog,
};

void paintInit(void)
{
	s32 i;

	g_MpSetup.options |= MPOPTION_TEAMSENABLED;

	g_ScenarioData.paint.roomowner = NULL;
	g_ScenarioData.paint.roomcount = 0;

	for (i = 0; i < MAX_TEAMS; i++) {
		g_ScenarioData.paint.teamcounts[i] = 0;
	}

	g_MpPaintDirty = 0;
}

void paintInitProps(void)
{
	struct scenariodata_paint *paint = &g_ScenarioData.paint;
	s32 i;

	paint->roomcount = g_Vars.roomcount;
	paint->roomowner = mempAlloc(ALIGN16(g_Vars.roomcount), MEMPOOL_STAGE);

	for (i = 0; i < g_Vars.roomcount; i++) {
		paint->roomowner[i] = 0;
	}

	for (i = 0; i < MAX_TEAMS; i++) {
		paint->teamcounts[i] = 0;
	}

	for (i = 0; i < (s32)ARRAYCOUNT(paint->lastroom); i++) {
		paint->lastroom[i] = -1;
	}

	g_MpPaintDirty = 0;
}

/**
 * Apply one room's ownership and update its lighting. Shared by the host tick
 * and the client-side SVC_PAINT_STATE apply (declared in net/net.h). Marking
 * the room brightness-dirty forces dlights.c to re-shade it so a team-flip
 * re-tints (the flash-lighting pattern). On the host a real change raises the
 * broadcast flag; clients never set it (they don't broadcast).
 */
void paintSetRoomOwner(s32 roomnum, u8 owner)
{
	struct scenariodata_paint *paint = &g_ScenarioData.paint;

	if (paint->roomowner == NULL || roomnum < 1 || roomnum >= paint->roomcount) {
		return;
	}

	if (paint->roomowner[roomnum] == owner) {
		return;
	}

	paint->roomowner[roomnum] = owner;

	if (owner) {
		roomSetLightOp(roomnum, LIGHTOP_HIGHLIGHT, 0, 0, 0);
	} else {
		roomSetLightOp(roomnum, LIGHTOP_NONE, 0, 0, 0);
	}

	g_Rooms[roomnum].flags |= ROOMFLAG_BRIGHTNESS_DIRTY_TEMP;

#ifndef PLATFORM_N64
	if (g_NetMode != NETMODE_CLIENT) {
		g_MpPaintDirty = 1;
	}
#endif
}

u8 *paintGetRoomOwner(s32 *roomcount_out)
{
	if (roomcount_out) {
		*roomcount_out = g_ScenarioData.paint.roomcount;
	}

	return g_ScenarioData.paint.roomowner;
}

void paintTick(void)
{
	struct scenariodata_paint *paint = &g_ScenarioData.paint;
	s32 i;

	if (paint->roomowner == NULL) {
		return;
	}

	// Ownership is host-authoritative; clients receive it via SVC_PAINT_STATE.
	// Rooms are claimed on ENTRY only (rooms[0] changed since last tick) — NOT
	// continuously by standing in them. Continuous claiming made contested
	// rooms flip every frame to whoever iterated last in g_MpAllChrPtrs (the
	// sims, which sit after the humans), so a human could never hold a room a
	// sim was standing in. Kills also claim — see paintHandleDeath.
#ifndef PLATFORM_N64
	if (g_NetMode != NETMODE_CLIENT)
#endif
	{
		for (i = 0; i < g_MpNumChrs && i < (s32)ARRAYCOUNT(paint->lastroom); i++) {
			struct chrdata *chr = g_MpAllChrPtrs[i];

			if (chr && chr->prop && !chrIsDead(chr)) {
				RoomNum room = chr->prop->rooms[0];
				s32 team = radarGetTeamIndex(chr->team);

				if (room != paint->lastroom[i]) {
					paint->lastroom[i] = room;

					if (room >= 1 && room < paint->roomcount
							&& team >= 0 && team < MAX_TEAMS) {
						paintSetRoomOwner(room, (u8)(team + 1));
					}
				}
			} else {
				// dead/missing: forget the room so the respawn counts as an
				// entry even if it lands in the same room they died in
				paint->lastroom[i] = -1;
			}
		}
	}

	// Recompute live per-team room tallies (everywhere — clients drive their
	// scoreboard from the wire-synced ownership) and mirror them onto each
	// player's numpoints so ranking reflects floor coverage.
	for (i = 0; i < MAX_TEAMS; i++) {
		paint->teamcounts[i] = 0;
	}

	for (i = 1; i < paint->roomcount; i++) {
		u8 owner = paint->roomowner[i];

		if (owner >= 1 && owner <= MAX_TEAMS) {
			paint->teamcounts[owner - 1]++;
		}
	}

	for (i = 0; i < g_MpNumChrs; i++) {
		s32 team = radarGetTeamIndex(g_MpAllChrPtrs[i]->team);

		if (team >= 0 && team < MAX_TEAMS) {
			g_MpAllChrConfigPtrs[i]->numpoints = paint->teamcounts[team];
		}
	}
}

/**
 * Kill claim: a kill claims the killer's current room for the killer's team
 * (the natural "win the contested room" rule — if several people are fighting
 * over a room, the one who scores a kill takes it). Called from
 * mpstatsRecordDeath (the pacHandleDeath pattern); aplayernum/vplayernum are
 * g_MpAllChrPtrs indices. Suicides and unattributed deaths claim nothing.
 * Host-only: clients receive ownership via SVC_PAINT_STATE.
 */
void paintHandleDeath(s32 aplayernum, s32 vplayernum)
{
	struct scenariodata_paint *paint = &g_ScenarioData.paint;
	struct chrdata *killer;
	RoomNum room;
	s32 team;

#ifndef PLATFORM_N64
	if (g_NetMode == NETMODE_CLIENT) {
		return;
	}
#endif

	if (paint->roomowner == NULL || aplayernum < 0 || aplayernum == vplayernum
			|| aplayernum >= g_MpNumChrs) {
		return;
	}

	killer = g_MpAllChrPtrs[aplayernum];

	if (killer == NULL || killer->prop == NULL || chrIsDead(killer)) {
		return;
	}

	room = killer->prop->rooms[0];
	team = radarGetTeamIndex(killer->team);

	if (room >= 1 && room < paint->roomcount && team >= 0 && team < MAX_TEAMS) {
		paintSetRoomOwner(room, (u8)(team + 1));
	}
}

Gfx *paintRenderHud(Gfx *gdl)
{
	struct scenariodata_paint *paint = &g_ScenarioData.paint;
	struct chrdata *chr = g_Vars.currentplayer->prop->chr;
	s32 myteam;
	s32 textwidth;
	s32 textheight;
	s32 x;
	s32 y;
	char text[16];

	if (paint->roomowner == NULL || chr == NULL) {
		return gdl;
	}

	// Show the local player's own team's room tally at the top of the screen.
	myteam = radarGetTeamIndex(chr->team);

	if (myteam < 0 || myteam >= MAX_TEAMS) {
		return gdl;
	}

	sprintf(text, "%d", paint->teamcounts[myteam]);

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
			(g_TeamColours[myteam] & 0xffffff00) | 0xa0, 0xa0, viGetWidth(), viGetHeight(), 0, 0);

	// Team floor-coverage bars (bottom centre, just above the countdown
	// timer): one vertical bar per active team in its team colour. The
	// leading team's bar is full height (15px); the others scale as a ratio
	// of the leader. Each bar sits in a dim full-height slot for readability.
	// Drawing pattern (textSetPrimColour + gDPFillRectangle with
	// text0f153838 flushes between colour changes) follows the HTM
	// download bar in hackthatmac.inc.
	{
		bool present[MAX_TEAMS];
		s32 t;
		s32 i;
		s32 nteams = 0;
		s32 leader = 0;
		const s32 barw = 4;
		const s32 gap = 2;
		const s32 maxh = 15;

		for (t = 0; t < MAX_TEAMS; t++) {
			present[t] = false;
		}

		for (i = 0; i < g_MpNumChrs; i++) {
			t = radarGetTeamIndex(g_MpAllChrPtrs[i]->team);

			if (t >= 0 && t < MAX_TEAMS) {
				present[t] = true;
			}
		}

		for (t = 0; t < MAX_TEAMS; t++) {
			if (present[t]) {
				nteams++;

				if (paint->teamcounts[t] > leader) {
					leader = paint->teamcounts[t];
				}
			}
		}

		if (nteams > 0) {
			s32 barx = viGetViewLeft() + (viGetViewWidth() - (nteams * (barw + gap) - gap)) / 2;
			// bottom centre, in the spot the bottom-centre countdown digits
			// used to occupy (the Combat Sim timer now sits bottom-left in
			// the campaign mission-timer position)
			s32 bary2 = viGetViewTop() + viGetViewHeight() - 14;

			for (t = 0; t < MAX_TEAMS; t++) {
				s32 h;

				if (!present[t]) {
					continue;
				}

				h = 1;

				if (leader > 0 && paint->teamcounts[t] > 0) {
					h = (paint->teamcounts[t] * maxh + leader / 2) / leader;

					if (h < 1) {
						h = 1;
					}
					if (h > maxh) {
						h = maxh;
					}
				}

				// dim full-height slot
				gdl = text0f153838(gdl);
				gdl = textSetPrimColour(gdl, 0x18181870);
				gDPFillRectangle(gdl++, barx, bary2 - maxh + 1, barx + barw, bary2 + 1);

				// filled portion from the bottom up, in team colour
				gdl = text0f153838(gdl);
				gdl = textSetPrimColour(gdl, (g_TeamColours[t] & 0xffffff00) | 0xc0);
				gDPFillRectangle(gdl++, barx, bary2 - h + 1, barx + barw, bary2 + 1);

				barx += barw + gap;
			}

			gdl = text0f153838(gdl);
		}
	}

	gdl = text0f153780(gdl);

#ifndef PLATFORM_N64
	gSPClearExtraGeometryModeEXT(gdl++, G_ASPECT_MODE_EXT);
#endif

	return gdl;
}

void paintCalculatePlayerScore(struct mpchrconfig *mpchr, s32 mpchrnum, s32 *score, s32 *deaths)
{
	// Score is purely floor coverage: the player's team's owned-room count
	// (mirrored onto numpoints each tick in paintTick). The whole team shares
	// the count, so the winning team ranks above the others.
	*score = mpchr->numpoints;
	*deaths = mpchr->numdeaths;
}

/**
 * "Owned Room Spawn" option: prefer to respawn in a room the dying player's
 * team owns; failing that, a room adjacent to owned territory; failing that,
 * fall through (return false) to the stock random spawn. Both host and client
 * pick from the same wire-synced ownership, and the host's authoritative
 * position correction reconciles any RNG divergence in the exact pad chosen.
 */
bool paintChooseSpawnLocation(f32 arg0, struct coord *pos, RoomNum *rooms, struct prop *prop, f32 *arg4)
{
	struct scenariodata_paint *paint = &g_ScenarioData.paint;
	struct chrdata *chr = prop->chr;
	s32 myteam;
	u8 wantowner;
	s16 candidates[PAINT_SPAWN_MAXPADS];
	s32 numcandidates = 0;
	s32 start;
	s32 p;
	s32 j;
	s32 k;
	struct pad pad;
	RoomNum neighbours[24];

	if (!(g_MpSetup.options & MPOPTION_OWNEDROOMSPAWN) || paint->roomowner == NULL
			|| g_NumSpawnPoints <= 0) {
		return false;
	}

	myteam = radarGetTeamIndex(chr->team);

	if (myteam < 0 || myteam >= MAX_TEAMS) {
		return false;
	}

	wantowner = (u8)(myteam + 1);

	// Tier 1: pads sitting in a room this team already owns. Start the scan at
	// a random offset so a >24-pad map still draws from the whole owned set.
	start = rngRandom() % g_NumSpawnPoints;

	for (j = 0; j < g_NumSpawnPoints && numcandidates < PAINT_SPAWN_MAXPADS; j++) {
		p = (start + j) % g_NumSpawnPoints;
		padUnpack(g_SpawnPoints[p], PADFIELD_ROOM, &pad);

		if (pad.room >= 1 && pad.room < paint->roomcount
				&& paint->roomowner[pad.room] == wantowner) {
			candidates[numcandidates++] = g_SpawnPoints[p];
		}
	}

	// Tier 2 ("as close as possible"): pads whose room neighbours an owned room.
	if (numcandidates == 0) {
		for (j = 0; j < g_NumSpawnPoints && numcandidates < PAINT_SPAWN_MAXPADS; j++) {
			p = (start + j) % g_NumSpawnPoints;
			padUnpack(g_SpawnPoints[p], PADFIELD_ROOM, &pad);

			if (pad.room < 1 || pad.room >= paint->roomcount) {
				continue;
			}

			bgRoomGetNeighbours(pad.room, neighbours, 20);

			for (k = 0; k < 20 && neighbours[k] != -1; k++) {
				RoomNum nb = neighbours[k];

				if (nb >= 1 && nb < paint->roomcount && paint->roomowner[nb] == wantowner) {
					candidates[numcandidates++] = g_SpawnPoints[p];
					break;
				}
			}
		}
	}

	// Tier 3: nothing near owned territory — let the caller pick at random.
	if (numcandidates == 0) {
		return false;
	}

	*arg4 = playerChooseSpawnLocation(arg0, pos, rooms, prop, candidates, numcandidates);
	return true;
}

bool paintIsRoomHighlighted(RoomNum room)
{
	struct scenariodata_paint *paint = &g_ScenarioData.paint;

	return paint->roomowner != NULL && room >= 1 && room < paint->roomcount
			&& paint->roomowner[room] != 0;
}

void paintHighlightRoom(RoomNum roomnum, s32 *arg1, s32 *arg2, s32 *arg3)
{
	struct scenariodata_paint *paint = &g_ScenarioData.paint;
	u8 owner;
	u32 colour;
	f32 a;
	f32 b;
	f32 c;

	if (paint->roomowner == NULL || roomnum < 1 || roomnum >= paint->roomcount) {
		return;
	}

	owner = paint->roomowner[roomnum];

	if (owner < 1 || owner > MAX_TEAMS) {
		return;
	}

	colour = g_TeamColours[owner - 1];
	a = *arg1;
	b = *arg2;
	c = *arg3;

	a *= (s32)((colour >> 24 & 0xff) + 0xff) * (1.0f / 512.0f);
	b *= (s32)((colour >> 16 & 0xff) + 0xff) * (1.0f / 512.0f);
	c *= (s32)((colour >> 8 & 0xff) + 0xff) * (1.0f / 512.0f);

	*arg1 = a;
	*arg2 = b;
	*arg3 = c;
}
