/**
 * Zones (port-only, MPSCENARIO_ZONES) — TimeSplitters 2 style territory
 * control built on the King of the Hill framework.
 *
 * Every KoH hillpad on the stage is an active zone simultaneously. Walking
 * into a zone flips it to your team's colour (optionally after a configurable
 * hold time — the "Capture Time" slider; the hold timer pauses KoH-style
 * while another team is also in the zone). A repeating score cycle (the
 * "Score Interval" slider) counts every zone each time it expires: each team
 * scores +1 per zone it owns, then the countdown resets. The match ends on
 * the team score limit or the time limit.
 *
 * Zone ownership, team scores and the cycle countdown are host-authoritative
 * and broadcast via SVC_ZONES_STATE (on change + 1s heartbeat). Zones reuse
 * the scenario room-highlight pipeline: neutral zones tint KoH-green, owned
 * zones tint the owning team's colour.
 */

// Host signal: zone owners / team scores / cycle reset changed this frame, so
// netEndFrame should broadcast SVC_ZONES_STATE. Cleared by the net layer.
u8 g_MpZonesDirty = 0;

// "Score Interval" slider: seconds between score cycles. Stored in
// g_MpSetup.zonescoretime (5..60, default 10).
#define ZONES_SCORETIME_MIN 5
#define ZONES_SCORETIME_MAX 60

// Sanitized score-cycle length in lvupdate240 units. Clamps stale / pushed
// out-of-range values (an old wad or admin push could carry anything).
static s32 zonesGetInterval240(void)
{
	s32 secs = g_MpSetup.zonescoretime;

	if (secs < ZONES_SCORETIME_MIN) {
		secs = ZONES_SCORETIME_MIN;
	}
	if (secs > ZONES_SCORETIME_MAX) {
		secs = ZONES_SCORETIME_MAX;
	}

	return secs * TICKS(240);
}

MenuItemHandlerResult menuhandlerMpZonesScoreTime(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GETSLIDER:
		data->slider.value = zonesGetInterval240() / TICKS(240) - ZONES_SCORETIME_MIN;
		break;
	case MENUOP_SET:
		g_MpSetup.zonescoretime = (u8)(data->slider.value + ZONES_SCORETIME_MIN);
		break;
	case MENUOP_GETSLIDERLABEL:
		sprintf(data->slider.label, "%ds\n", (s32)data->slider.value + ZONES_SCORETIME_MIN);
		break;
	}

	return 0;
}

// "Capture Time" slider: seconds a combatant must hold a zone before it
// flips (0 = instant on entry). Stored in g_MpSetup.zonecapturetime (0..10).
MenuItemHandlerResult menuhandlerMpZonesCaptureTime(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GETSLIDER:
		data->slider.value = g_MpSetup.zonecapturetime;
		break;
	case MENUOP_SET:
		g_MpSetup.zonecapturetime = (u8)data->slider.value;
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

struct menuitem g_MpZonesOptionsMenuItems[] = {
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
		// Reuses the KoH hill-on-radar bit (the scenarios are mutually
		// exclusive); default on via mpInit.
		MENUITEMTYPE_CHECKBOX,
		0,
		MENUITEMFLAG_LOCKABLEMINOR | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Zones on Radar",
		MPOPTION_KOH_HILLONRADAR,
		menuhandlerMpCheckboxOption,
	},
	{
		// Seconds between score cycles (each cycle every team scores +1 per
		// zone owned). param3 is the slider max (value + MIN = seconds).
		MENUITEMTYPE_SLIDER,
		0,
		MENUITEMFLAG_LOCKABLEMINOR | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Score Interval",
		ZONES_SCORETIME_MAX - ZONES_SCORETIME_MIN,
		menuhandlerMpZonesScoreTime,
	},
	{
		// Seconds a combatant must hold a zone before it flips (0 = instant).
		MENUITEMTYPE_SLIDER,
		0,
		MENUITEMFLAG_LOCKABLEMINOR | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Capture Time",
		10,
		menuhandlerMpZonesCaptureTime,
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

struct menudialogdef g_MpZonesOptionsMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Zones Options\n",
	g_MpZonesOptionsMenuItems,
	mpOptionsMenuDialog,
	MENUDIALOGFLAG_MPLOCKABLE | MENUDIALOGFLAG_LITERAL_TEXT,
	&g_ExtGameOptionsMenuDialog,
};

void zonesInit(void)
{
	struct scenariodata_zones *zones = &g_ScenarioData.zones;
	s32 i;

	g_MpSetup.options |= MPOPTION_TEAMSENABLED;

	zones->hillcount = 0;
	zones->cycle240 = 0;

	for (i = 0; i < (s32)ARRAYCOUNT(zones->hillpads); i++) {
		zones->hillpads[i] = -1;
		zones->hillrooms[i] = -1;
		zones->owners[i] = 0;
	}

	for (i = 0; i < MAX_TEAMS; i++) {
		zones->teamscores[i] = 0;
	}

	for (i = 0; i < (s32)ARRAYCOUNT(zones->lastzone); i++) {
		zones->lastzone[i] = -1;
		zones->capturetime240[i] = 0;
	}

	g_MpZonesDirty = 0;
}

// INTROCMD_HILL collector — the same stage-data source kohAddHill reads, but
// into the zones union member (zones and koh can't be active simultaneously).
void zonesAddHill(s32 *cmd)
{
	struct scenariodata_zones *zones = &g_ScenarioData.zones;

	if (zones->hillcount < (s16)ARRAYCOUNT(zones->hillpads)) {
		zones->hillpads[zones->hillcount] = (s16)cmd[1];
		zones->hillcount++;
	}
}

void zonesInitProps(void)
{
	struct scenariodata_zones *zones = &g_ScenarioData.zones;
	struct pad pad;
	s32 i;

	// Resolve every hillpad's room + position and highlight all of them from
	// the start (neutral zones tint KoH-green via zonesHighlightRoom until
	// captured). The position feeds the radar dots.
	for (i = 0; i < zones->hillcount; i++) {
		padUnpack(zones->hillpads[i], PADFIELD_POS | PADFIELD_ROOM, &pad);
		zones->hillrooms[i] = pad.room;
		zones->zonepos[i] = pad.pos;
		zones->owners[i] = 0;
		roomSetLightOp(zones->hillrooms[i], LIGHTOP_HIGHLIGHT, 0, 0, 0);
	}

	for (i = 0; i < MAX_TEAMS; i++) {
		zones->teamscores[i] = 0;
	}

	for (i = 0; i < (s32)ARRAYCOUNT(zones->lastzone); i++) {
		zones->lastzone[i] = -1;
		zones->capturetime240[i] = 0;
	}

	zones->cycle240 = zonesGetInterval240();
	g_MpZonesDirty = 0;
}

static s32 zonesFindZoneByRoom(RoomNum room)
{
	struct scenariodata_zones *zones = &g_ScenarioData.zones;
	s32 i;

	for (i = 0; i < zones->hillcount; i++) {
		if (zones->hillrooms[i] == room) {
			return i;
		}
	}

	return -1;
}

/**
 * A zone is contested while a living combatant from another team is inside
 * it (KoH dual-occupancy rule) — the timed capture pauses for everyone in the
 * zone until one team holds it exclusively.
 */
static bool zonesZoneContested(s32 zoneindex, s32 team)
{
	struct scenariodata_zones *zones = &g_ScenarioData.zones;
	s32 j;

	for (j = 0; j < g_MpNumChrs; j++) {
		struct chrdata *other = g_MpAllChrPtrs[j];

		if (other && other->prop && !chrIsDead(other)
				&& other->prop->rooms[0] == zones->hillrooms[zoneindex]
				&& radarGetTeamIndex(other->team) != team) {
			return true;
		}
	}

	return false;
}

/**
 * Apply one zone's ownership and re-tint its room. Shared by the host tick
 * and the client-side SVC_ZONES_STATE apply. The room's highlight op was set
 * at init (zones are always highlighted); flipping the owner just needs the
 * brightness-dirty reshade. On the host a real change raises the broadcast
 * flag; clients never set it.
 */
void zonesSetZoneOwner(s32 zoneindex, u8 owner)
{
	struct scenariodata_zones *zones = &g_ScenarioData.zones;

	if (zoneindex < 0 || zoneindex >= zones->hillcount || owner > MAX_TEAMS) {
		return;
	}

	if (zones->owners[zoneindex] == owner) {
		return;
	}

	zones->owners[zoneindex] = owner;

	if (zones->hillrooms[zoneindex] >= 1 && zones->hillrooms[zoneindex] < g_Vars.roomcount) {
		g_Rooms[zones->hillrooms[zoneindex]].flags |= ROOMFLAG_BRIGHTNESS_DIRTY_TEMP;
	}

	// no flip cue by design — with all zones active at once and combatants
	// constantly crossing them, a per-flip sound is just noise. The room
	// re-tint (and the HUD squares / radar dots) carry the information; the
	// score cue still marks each cycle award.

#ifndef PLATFORM_N64
	if (g_NetMode != NETMODE_CLIENT) {
		g_MpZonesDirty = 1;
	}
#endif
}

struct scenariodata_zones *zonesGetData(void)
{
	return &g_ScenarioData.zones;
}

/**
 * Client-side SVC_ZONES_STATE apply: zone owners (idempotent, via the shared
 * setter so rooms re-tint), authoritative team scores and the cycle countdown.
 * Plays the score cue when any team's score increased (the heartbeat resends
 * unchanged scores, so edge-detect here rather than on receipt).
 */
void zonesApplyWireState(const u8 *owners, s32 count, const s32 *teamscores, s32 cycle240)
{
	struct scenariodata_zones *zones = &g_ScenarioData.zones;
	bool scored = false;
	s32 i;

	for (i = 0; i < count && i < zones->hillcount; i++) {
		zonesSetZoneOwner(i, owners[i]);
	}

	for (i = 0; i < MAX_TEAMS; i++) {
		if (teamscores[i] > zones->teamscores[i]) {
			scored = true;
		}

		zones->teamscores[i] = teamscores[i];
	}

	zones->cycle240 = cycle240;

	if (scored) {
		sndStart(var80095200, SFX_MP_SCOREPOINT, 0, -1, -1, -1, -1, -1);
	}
}

void zonesTick(void)
{
	struct scenariodata_zones *zones = &g_ScenarioData.zones;
	s32 i;

	if (zones->hillcount <= 0) {
		return;
	}

	// Capture tracking (the Graffiti pattern): runs on every machine so
	// clients can display capture state, but the ownership WRITES are gated
	// to the server. With Capture Time at 0 a zone flips on ENTRY; with a
	// non-zero time the combatant must hold the zone, paused while another
	// team is also inside it.
	{
		s32 captureticks = g_MpSetup.zonecapturetime * TICKS(240);
#ifndef PLATFORM_N64
		bool isclient = g_NetMode == NETMODE_CLIENT;
#else
		bool isclient = false;
#endif

		for (i = 0; i < g_MpNumChrs && i < (s32)ARRAYCOUNT(zones->lastzone); i++) {
			struct chrdata *chr = g_MpAllChrPtrs[i];

			if (chr && chr->prop && !chrIsDead(chr)) {
				s32 zone = zonesFindZoneByRoom(chr->prop->rooms[0]);
				s32 team = radarGetTeamIndex(chr->team);

				if (zone != zones->lastzone[i]) {
					zones->lastzone[i] = zone;
					zones->capturetime240[i] = 0;

					if (captureticks == 0 && !isclient && zone >= 0
							&& team >= 0 && team < MAX_TEAMS) {
						zonesSetZoneOwner(zone, (u8)(team + 1));
					}
				} else if (captureticks > 0 && zone >= 0
						&& team >= 0 && team < MAX_TEAMS
						&& zones->owners[zone] != (u8)(team + 1)
						&& !zonesZoneContested(zone, team)) {
					zones->capturetime240[i] += g_Vars.lvupdate240;

					if (zones->capturetime240[i] >= captureticks) {
						if (isclient) {
							// hold at the threshold until the host's flip
							// arrives via SVC_ZONES_STATE
							zones->capturetime240[i] = captureticks;
						} else {
							zones->capturetime240[i] = 0;
							zonesSetZoneOwner(zone, (u8)(team + 1));
						}
					}
				}
			} else {
				zones->lastzone[i] = -1;
				zones->capturetime240[i] = 0;
			}
		}
	}

	// Score cycle: when the countdown expires, every team scores +1 per zone
	// it owns and the countdown resets. Server-authoritative; clients tick
	// the countdown locally for display and clamp at 0 until the broadcast
	// (raised dirty below) resyncs them with the reset value.
	zones->cycle240 -= g_Vars.lvupdate240;

#ifndef PLATFORM_N64
	if (g_NetMode == NETMODE_CLIENT) {
		if (zones->cycle240 < 0) {
			zones->cycle240 = 0;
		}
	} else
#endif
	{
		if (zones->cycle240 <= 0) {
			bool scored = false;

			zones->cycle240 += zonesGetInterval240();

			if (zones->cycle240 <= 0) {
				// huge hitch — don't burst multiple awards
				zones->cycle240 = zonesGetInterval240();
			}

			for (i = 0; i < zones->hillcount; i++) {
				u8 owner = zones->owners[i];

				if (owner >= 1 && owner <= MAX_TEAMS) {
					zones->teamscores[owner - 1]++;
					scored = true;
				}
			}

#ifndef PLATFORM_N64
			// broadcast the new scores + reset countdown promptly even when
			// no zone changed hands this cycle
			if (g_NetMode != NETMODE_CLIENT) {
				g_MpZonesDirty = 1;
			}
#endif

			if (scored) {
				sndStart(var80095200, SFX_MP_SCOREPOINT, 0, -1, -1, -1, -1, -1);
			}
		}
	}

	// Mirror each team's accumulated score onto its members' numpoints (the
	// Graffiti pattern) so ranking and the team score limit see it.
	// mpCalculateTeamScore has a Zones override so the team total isn't
	// multiplied by the member count.
	for (i = 0; i < g_MpNumChrs; i++) {
		s32 team = radarGetTeamIndex(g_MpAllChrPtrs[i]->team);

		if (team >= 0 && team < MAX_TEAMS) {
			g_MpAllChrConfigPtrs[i]->numpoints = zones->teamscores[team];
		}
	}
}

Gfx *zonesRenderHud(Gfx *gdl)
{
	struct scenariodata_zones *zones = &g_ScenarioData.zones;
	s32 textwidth;
	s32 textheight;
	s32 x;
	s32 y;
	s32 secs;
	s32 i;
	char text[16];

	if (zones->hillcount <= 0) {
		return gdl;
	}

	// Score-cycle countdown, top centre (KoH numeric-font style). Rounds up
	// so the display reads the full interval right after a reset and "1"
	// just before the award.
	secs = (zones->cycle240 + TICKS(240) - 1) / TICKS(240);

	if (secs < 0) {
		secs = 0;
	}

	sprintf(text, "%02d", secs);

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
	gdl = textRender(gdl, &x, &y, text, g_CharsNumeric, g_FontNumeric,
			0x00ff00a0, 0xa0, viGetWidth(), viGetHeight(), 0, 0);

	// Per-zone ownership squares (bottom centre, the spot Graffiti's coverage
	// bars occupy): one 3x3 square per zone, in zone-index order, coloured by
	// owner — neutral zones use the hill green. Drawing pattern
	// (textSetPrimColour + gDPFillRectangle with text0f153838 flushes between
	// colour changes) follows the Graffiti bars.
	{
		const s32 sqw = 3;
		const s32 gap = 2;
		s32 sqx = viGetViewLeft() + (viGetViewWidth() - (zones->hillcount * (sqw + gap) - gap)) / 2;
		s32 sqy2 = viGetViewTop() + viGetViewHeight() - 14;

		for (i = 0; i < zones->hillcount; i++) {
			u32 colour;

			if (zones->owners[i] >= 1 && zones->owners[i] <= MAX_TEAMS) {
				colour = (g_TeamColours[zones->owners[i] - 1] & 0xffffff00) | 0xc0;
			} else {
				colour = 0x00ff00c0;
			}

			gdl = text0f153838(gdl);
			gdl = textSetPrimColour(gdl, colour);
			gDPFillRectangle(gdl++, sqx, sqy2 - 2, sqx + sqw, sqy2 + 1);

			sqx += sqw + gap;
		}

		gdl = text0f153838(gdl);
	}

	gdl = text0f153780(gdl);

#ifndef PLATFORM_N64
	gSPClearExtraGeometryModeEXT(gdl++, G_ASPECT_MODE_EXT);
#endif

	return gdl;
}

Gfx *zonesRadarExtra(Gfx *gdl)
{
	struct scenariodata_zones *zones = &g_ScenarioData.zones;
	s32 i;

	// One dot per zone in its owner's colour (the kohRadarExtra pattern;
	// neutral zones use the same green as the unoccupied hill).
	if (g_MpSetup.options & MPOPTION_KOH_HILLONRADAR) {
		for (i = 0; i < zones->hillcount; i++) {
			struct coord dist;
			u32 colour;

			dist.x = zones->zonepos[i].x - g_Vars.currentplayer->prop->pos.x;
			dist.y = zones->zonepos[i].y - g_Vars.currentplayer->prop->pos.y;
			dist.z = zones->zonepos[i].z - g_Vars.currentplayer->prop->pos.z;

			if (zones->owners[i] >= 1 && zones->owners[i] <= MAX_TEAMS) {
				colour = g_TeamColours[zones->owners[i] - 1];
			} else {
				colour = 0x00ff0000;
			}

			gdl = radarDrawDot(gdl, NULL, &dist, colour, 0, 1);
		}
	}

	return gdl;
}

void zonesCalculatePlayerScore(struct mpchrconfig *mpchr, s32 mpchrnum, s32 *score, s32 *deaths)
{
	// Score is the team's accumulated cycle awards (mirrored onto numpoints
	// each tick in zonesTick); the whole team shares the total.
	*score = mpchr->numpoints;
	*deaths = mpchr->numdeaths;
}

bool zonesIsRoomHighlighted(RoomNum room)
{
	return g_ScenarioData.zones.hillcount > 0 && zonesFindZoneByRoom(room) >= 0;
}

void zonesHighlightRoom(RoomNum roomnum, s32 *arg1, s32 *arg2, s32 *arg3)
{
	s32 zone = zonesFindZoneByRoom(roomnum);
	u8 owner;
	f32 a;
	f32 b;
	f32 c;

	if (zone < 0) {
		return;
	}

	owner = g_ScenarioData.zones.owners[zone];
	a = *arg1;
	b = *arg2;
	c = *arg3;

	if (owner >= 1 && owner <= MAX_TEAMS) {
		// owned: the owning team's colour (the Graffiti tint math)
		u32 colour = g_TeamColours[owner - 1];

		a *= (s32)((colour >> 24 & 0xff) + 0xff) * (1.0f / 512.0f);
		b *= (s32)((colour >> 16 & 0xff) + 0xff) * (1.0f / 512.0f);
		c *= (s32)((colour >> 8 & 0xff) + 0xff) * (1.0f / 512.0f);
	} else {
		// neutral: the KoH unoccupied-hill green
		a *= 0.25f;
		c *= 0.25f;
	}

	*arg1 = a;
	*arg2 = b;
	*arg3 = c;
}

/**
 * Per-setup save: both sliders pack into the 32-bit scenario save slot
 * (the slot KoH uses for mphilltime), so no mpsetups wad version bump is
 * needed. Bits 0-3 = zonecapturetime, bits 4-9 = zonescoretime.
 */
void zonesReadSave(struct savebuffer *buffer, u8 version)
{
	u8 sz = version > 0 ? 32 : 8;
	u32 val = savebufferReadBits(buffer, sz);

	if (version > 0) {
		g_MpSetup.zonecapturetime = val & 0xf;
		g_MpSetup.zonescoretime = (val >> 4) & 0x3f;

		if (g_MpSetup.zonecapturetime > 10) {
			g_MpSetup.zonecapturetime = 10;
		}

		if (g_MpSetup.zonescoretime < ZONES_SCORETIME_MIN
				|| g_MpSetup.zonescoretime > ZONES_SCORETIME_MAX) {
			g_MpSetup.zonescoretime = 10;
		}
	}
}

void zonesWriteSave(struct savebuffer *buffer)
{
	savebufferOr(buffer, ((u32)g_MpSetup.zonescoretime << 4)
			| (g_MpSetup.zonecapturetime & 0xf), 32);
}
