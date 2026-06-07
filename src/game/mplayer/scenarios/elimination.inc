/**
 * Global Lives system (port-only; formerly the "Elimination" scenario,
 * MPSCENARIO_ELIMINATION 8 — retired before release, see
 * docs/PORT_ELIMINATION.md for the history).
 *
 * A scenario-independent "limit": when the Lives setting (Limits menu) is
 * non-zero, every combatant starts with N lives (1-9, the co-op F3 range)
 * and every death spends one:
 *  - Solo Lives: each combatant has their own pool of N.
 *  - Team Lives: the team shares ONE pool of exactly N — the setting IS the
 *    team's total, regardless of member count. Falls back to Solo behaviour
 *    if teams are disabled.
 * Out of lives = no respawn (player and bot respawn paths are gated via
 * elimChrCanRespawn — player.c MP dead branch, chraction.c chrTickDead).
 * A dead combatant with no lives left is ELIMINATED (latched). When at most
 * one faction remains the match ends via elimShouldEndMatch(), polled from
 * lv.c's match-end block (g_NumReasonsToEndMpMatch is recomputed from ZERO
 * there each frame, so a tick-side increment would be wiped — never do
 * that). Scoring stays whatever the active scenario says: lives award no
 * points of their own.
 *
 * Although no longer a scenario, the system still lives in this .inc (the
 * scenarios.c TU) and hooks the scenario dispatchers: elimReset from
 * scenarioInitProps, elimTick from scenarioTick, elimRenderHud from
 * scenarioRenderHud — each internally gated on the Lives setting.
 *
 * Lives / eliminations are host-authoritative and broadcast via
 * SVC_ELIM_STATE (on change + 1s heartbeat); clients receive state for the
 * HUD — all gating runs on the server (net respawn is server-driven).
 */

// Host signal: lives / eliminations changed this frame, so netEndFrame
// should broadcast SVC_ELIM_STATE. Cleared by the net layer after sending.
u8 g_MpElimDirty = 0;

// The per-match lives state. NOT in the g_ScenarioData union — lives are
// scenario-independent, and the union slot belongs to the active scenario.
struct elimdata g_ElimData;

// Sanitized Lives setting (wad tails and admin pushes can carry anything).
// 0 = the system is OFF.
static s32 elimSaneLives(void)
{
	s32 n = g_MpSetup.elimlives;

	if (n < 0) {
		n = 0;
	}
	if (n > 9) {
		n = 9;
	}

	return n;
}

static bool elimLivesEnabled(void)
{
	return g_Vars.normmplayerisrunning && elimSaneLives() > 0;
}

// Team (shared-pool) mode requires teams; without them it degrades to Solo.
static bool elimTeamMode(void)
{
	return g_MpSetup.elimlivesmode != 0 && (g_MpSetup.options & MPOPTION_TEAMSENABLED) != 0;
}

// "Lives Mode" dropdown (Limits menu): Solo = per-combatant pools; Team =
// one shared pool per team (exactly the Lives value). Greyed out while
// Lives is Off.
MenuItemHandlerResult menuhandlerMpElimLivesMode(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_CHECKDISABLED:
		return elimSaneLives() == 0 ? 1 : 0;
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = 2;
		break;
	case MENUOP_GETOPTIONTEXT:
		return data->dropdown.value == 0 ? (uintptr_t)"Solo" : (uintptr_t)"Team";
	case MENUOP_SET:
		g_MpSetup.elimlivesmode = (u8)data->dropdown.value;
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = g_MpSetup.elimlivesmode ? 1 : 0;
		break;
	}

	return 0;
}

// "Lives" slider (Limits menu): 0 = Off (default), 1-9 = lives.
MenuItemHandlerResult menuhandlerMpElimLives(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GETSLIDER:
		data->slider.value = elimSaneLives();
		break;
	case MENUOP_SET:
		g_MpSetup.elimlives = (u8)data->slider.value;
		break;
	case MENUOP_GETSLIDERLABEL:
		if (data->slider.value == 0) {
			sprintf(data->slider.label, "Off\n");
		} else {
			sprintf(data->slider.label, "%d\n", (s32)data->slider.value);
		}
		break;
	}

	return 0;
}

/**
 * Per-match reset, called unconditionally from scenarioInitProps (every
 * stage load, host and client alike). Lives are seeded lazily on the first
 * tick — g_MpAllChrPtrs isn't populated until mpStartMatch finishes.
 */
void elimReset(void)
{
	struct elimdata *elim = &g_ElimData;
	s32 i;

	for (i = 0; i < (s32)ARRAYCOUNT(elim->lives); i++) {
		elim->lives[i] = 0;
		elim->eliminated[i] = 0;
	}

	for (i = 0; i < MAX_TEAMS; i++) {
		elim->teamlives[i] = 0;
	}

	elim->initdone = 0;
	elim->startfactions = 0;

	g_MpElimDirty = 0;
}

// A combatant's faction for the last-standing test: the team with teams
// enabled, else themselves (free-for-all — every combatant is a faction).
static s32 elimFactionIndex(s32 chrnum)
{
	if (g_MpSetup.options & MPOPTION_TEAMSENABLED) {
		return radarGetTeamIndex(g_MpAllChrPtrs[chrnum]->team);
	}

	return chrnum;
}

static void elimLazyInit(void)
{
	struct elimdata *elim = &g_ElimData;
	u32 seen = 0;
	s32 i;

	for (i = 0; i < MAX_TEAMS; i++) {
		elim->teamlives[i] = 0;
	}

	for (i = 0; i < g_MpNumChrs && i < (s32)ARRAYCOUNT(elim->lives); i++) {
		s32 team = radarGetTeamIndex(g_MpAllChrPtrs[i]->team);
		s32 faction = elimFactionIndex(i);

		elim->lives[i] = elimSaneLives();
		elim->eliminated[i] = 0;

		// team pool = the Lives setting exactly — the value is the team's
		// TOTAL, regardless of member count
		if (team >= 0 && team < MAX_TEAMS) {
			elim->teamlives[team] = elimSaneLives();
		}

		if (faction >= 0 && faction < 32 && (seen & (1u << faction)) == 0) {
			seen |= 1u << faction;
			elim->startfactions++;
		}
	}

	elim->initdone = 1;

#ifndef PLATFORM_N64
	if (g_NetMode != NETMODE_CLIENT) {
		g_MpElimDirty = 1;
	}
#endif
}

// Remaining respawn budget for a combatant (by g_MpAllChrPtrs index).
static s32 elimChrRemaining(s32 chrnum)
{
	struct elimdata *elim = &g_ElimData;

	if (elimTeamMode()) {
		s32 team = radarGetTeamIndex(g_MpAllChrPtrs[chrnum]->team);

		if (team >= 0 && team < MAX_TEAMS) {
			return elim->teamlives[team];
		}

		return 1;
	}

	return elim->lives[chrnum];
}

/**
 * Respawn gate, called from the MP dead-player branch (player.c) and the bot
 * corpse-fade respawn (chraction.c chrTickDead). Returns true whenever the
 * Lives system is off, so call sites stay one-liners. Runs on the authority
 * only — net respawn is server-driven (clients just send UCMD_RESPAWN).
 */
bool elimChrCanRespawn(struct chrdata *chr)
{
	struct elimdata *elim = &g_ElimData;
	s32 chrnum;

	if (!elimLivesEnabled() || !elim->initdone) {
		return true;
	}

	chrnum = mpPlayerGetIndex(chr);

	if (chrnum < 0 || chrnum >= (s32)ARRAYCOUNT(elim->lives)) {
		return true;
	}

	if (elim->eliminated[chrnum]) {
		return false;
	}

	return elimChrRemaining(chrnum) > 0;
}

/**
 * Death hook (mpstatsRecordDeath, the pacHandleDeath pattern): every death —
 * kills, suicides, unattributed — spends one life from the victim's pool.
 * Host-only: clients receive lives via SVC_ELIM_STATE.
 */
void elimHandleDeath(s32 aplayernum, s32 vplayernum)
{
	struct elimdata *elim = &g_ElimData;

#ifndef PLATFORM_N64
	if (g_NetMode == NETMODE_CLIENT) {
		return;
	}
#endif

	if (!elim->initdone || vplayernum < 0 || vplayernum >= g_MpNumChrs
			|| vplayernum >= (s32)ARRAYCOUNT(elim->lives)) {
		return;
	}

	if (elimTeamMode()) {
		s32 team = radarGetTeamIndex(g_MpAllChrPtrs[vplayernum]->team);

		if (team >= 0 && team < MAX_TEAMS && elim->teamlives[team] > 0) {
			elim->teamlives[team]--;
		}
	} else if (elim->lives[vplayernum] > 0) {
		elim->lives[vplayernum]--;
	}

	g_MpElimDirty = 1;
}

/**
 * Per-frame upkeep, called from scenarioTick beside the active scenario's
 * tickfunc: lazy seed, then (server-side) latch eliminations — dead with no
 * budget left, kept down by the respawn gates.
 */
void elimTick(void)
{
	struct elimdata *elim = &g_ElimData;
	s32 i;

	if (!elimLivesEnabled()) {
		return;
	}

	if (!elim->initdone) {
		if (g_MpNumChrs > 0) {
			elimLazyInit();
		}

		return;
	}

#ifndef PLATFORM_N64
	if (g_NetMode == NETMODE_CLIENT) {
		return;
	}
#endif

	for (i = 0; i < g_MpNumChrs && i < (s32)ARRAYCOUNT(elim->lives); i++) {
		struct chrdata *chr = g_MpAllChrPtrs[i];

		if (chr == NULL || chr->prop == NULL || elim->eliminated[i]) {
			continue;
		}

		if (chrIsDead(chr) && elimChrRemaining(i) == 0) {
			elim->eliminated[i] = 1;
			g_MpElimDirty = 1;
		}
	}
}

/**
 * True when the match has been decided: lives are on, the match started with
 * a contest (>= 2 factions) and at most one faction still has non-eliminated
 * members.
 *
 * Called from lv.c's match-end block, beside the score-limit checks — NOT
 * from elimTick: lv.c recomputes g_NumReasonsToEndMpMatch from ZERO every
 * frame right before those checks, so an increment made during the scenario
 * tick is wiped before the end test reads it.
 */
bool elimShouldEndMatch(void)
{
	struct elimdata *elim = &g_ElimData;
	u32 seen = 0;
	s32 remaining = 0;
	s32 i;

	if (!elimLivesEnabled() || !elim->initdone || elim->startfactions < 2) {
		return false;
	}

	for (i = 0; i < g_MpNumChrs && i < (s32)ARRAYCOUNT(elim->lives); i++) {
		s32 faction;

		if (elim->eliminated[i]) {
			continue;
		}

		faction = elimFactionIndex(i);

		if (faction >= 0 && faction < 32 && (seen & (1u << faction)) == 0) {
			seen |= 1u << faction;
			remaining++;
		}
	}

	return remaining <= 1;
}

/**
 * Lives HUD, called from scenarioRenderHud beside the active scenario's
 * hudfunc (which owns the top centre) — so it draws top LEFT: remaining
 * lives (the shared pool in Team mode) in the team colour, "OUT" once
 * eliminated.
 */
Gfx *elimRenderHud(Gfx *gdl)
{
	struct elimdata *elim = &g_ElimData;
	struct chrdata *chr = g_Vars.currentplayer->prop->chr;
	s32 textwidth;
	s32 textheight;
	s32 x;
	s32 y;
	s32 chrnum;
	s32 myteam;
	char text[20];

	if (!elimLivesEnabled() || !elim->initdone || chr == NULL) {
		return gdl;
	}

	chrnum = mpPlayerGetIndex(chr);

	if (chrnum < 0 || chrnum >= (s32)ARRAYCOUNT(elim->lives)) {
		return gdl;
	}

	myteam = radarGetTeamIndex(chr->team);

	if (myteam < 0 || myteam >= MAX_TEAMS) {
		return gdl;
	}

	if (elim->eliminated[chrnum]) {
		sprintf(text, "OUT");
	} else {
		sprintf(text, "%d", elimChrRemaining(chrnum));
	}

	x = viGetViewLeft() + 8;
	y = viGetViewTop() + 10;

	gdl = text0f153628(gdl);
	textMeasure(&textheight, &textwidth, text, g_CharsHandelGothicXs, g_FontHandelGothicXs, 0);

	textwidth += x;
	textheight += y;

	gdl = text0f153990(gdl, x, y, textwidth, textheight);
	gdl = textRender(gdl, &x, &y, text, g_CharsHandelGothicXs, g_FontHandelGothicXs,
			(g_TeamColours[myteam] & 0xffffff00) | 0xa0, 0xa0, viGetWidth(), viGetHeight(), 0, 0);
	gdl = text0f153780(gdl);

	return gdl;
}

struct elimdata *elimGetData(void)
{
	return &g_ElimData;
}

/**
 * Client-side SVC_ELIM_STATE apply: authoritative lives, team pools and the
 * eliminated set. initdone is forced so the HUD renders from wire state even
 * before the local lazy seed ran.
 */
void elimApplyWireState(const u8 *lives, const u8 *teamlives, u16 elimmask)
{
	struct elimdata *elim = &g_ElimData;
	s32 i;

	for (i = 0; i < (s32)ARRAYCOUNT(elim->lives); i++) {
		elim->lives[i] = lives[i];
		elim->eliminated[i] = (elimmask >> i) & 1;
	}

	for (i = 0; i < MAX_TEAMS; i++) {
		elim->teamlives[i] = teamlives[i];
	}

	elim->initdone = 1;
}
