#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <PR/ultratypes.h>
#include "platform.h"
#include "data.h"
#include "types.h"
#include "game/mainmenu.h"
#include "game/menu.h"
#include "game/gamefile.h"
#include "game/filelist.h"
#include "video.h"
#include "input.h"
#include "lib/vi.h"
#include "game/game_1531a0.h"
#include "config.h"
#include "system.h"
#include "mpsetups.h"
#include "bss.h"
#include "game/lang.h"
#include "game/title.h"   // titleSetNextMode / setNumPlayers (Host Online lobby reload)
#include "game/pdmode.h"  // titleSetNextStage
#include "game/mplayer/mplayer.h" // mpSetPaused
#include "lib/main.h"     // mainChangeToStage
#include "net/net.h"
#include "net/netmaster.h"
#include "net/playlist.h"
#include "spectator.h"

extern MenuItemHandlerResult menuhandlerMainMenuCombatSimulator(s32 operation, struct menuitem *item, union handlerdata *data);
extern MenuItemHandlerResult menuhandlerMpAdvancedSetup(s32 operation, struct menuitem *item, union handlerdata *data);
extern struct menuitem g_MpPlayerSetup234MenuItems[];
extern struct menudialogdef g_NetJoinPlayerSetupMenuDialog;

// Combat Sim menu handlers + data reused by the admin match-setup menu below.
// Declared here (rather than via setup.h) to match this file's existing pattern
// of externing the specific Combat Sim entry points it drives. The reused
// sub-dialogs g_MpArenaMenuDialog / g_MpWeaponsMenuDialog / g_MpLimitsMenuDialog
// are already declared in data.h.
extern MenuItemHandlerResult menuhandlerMpCheckboxOption(s32 operation, struct menuitem *item, union handlerdata *data);
extern char *mpMenuTextArenaName(struct menuitem *item);
extern void mpCreateBotFromProfile(s32 botnum, u8 difficulty);

static s32 g_NetMenuMaxPlayers = NET_MAX_CLIENTS;
static s32 g_NetMenuPort = NET_DEFAULT_PORT;
// Host spectator-mode toggles. The host applies these when starting the
// server so the first SVC_LOBBY_STATE broadcast already carries the flag.
// Panel count is host-local — it never goes over the wire.
static s32 g_NetMenuHostSpectator = 0;
static s32 g_NetMenuHostPanels = 1;
static char g_NetJoinAddr[NET_MAX_ADDR + 1];
static s32 g_NetJoinAddrPtr = 0;
static s32 g_NetJoinPasswordPtr = 0; // shared by the manual-join field + browser prompt

#define NET_FAV_MAX      8
#define NET_FAV_NAME_LEN 32

static char g_NetFavName[NET_FAV_MAX][NET_FAV_NAME_LEN];
static char g_NetFavAddr[NET_FAV_MAX][NET_MAX_ADDR + 1];
static char g_NetFavNewName[NET_FAV_NAME_LEN];
static s32  g_NetFavNewNamePtr = 0;
static s32  g_NetJoinHidden = 0;

PD_CONSTRUCTOR static void netmenuConfigInit(void)
{
	char key[64];
	for (s32 i = 0; i < NET_FAV_MAX; i++) {
		snprintf(key, sizeof(key), "Net.Favourites.%d.Name", i);
		configRegisterString(key, g_NetFavName[i], NET_FAV_NAME_LEN - 1);
		snprintf(key, sizeof(key), "Net.Favourites.%d.Addr", i);
		configRegisterString(key, g_NetFavAddr[i], NET_MAX_ADDR);
	}
	configRegisterInt("Net.Client.HideAddress", &g_NetJoinHidden, 0, 1);
}

/* host */

static MenuItemHandlerResult menuhandlerHostMaxPlayers(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GETSLIDER:
		data->slider.value = g_NetMenuMaxPlayers;
		break;
	case MENUOP_SET:
		if (data->slider.value) {
			g_NetMenuMaxPlayers = data->slider.value;
		}
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerHostSpectator(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_SET) {
		g_NetMenuHostSpectator = !g_NetMenuHostSpectator;
	}
	return 0;
}

static const char *menutextHostSpectator(struct menuitem *item)
{
	return g_NetMenuHostSpectator ? "On\n" : "Off\n";
}

// Spectator Panels dropdown (1..SPEC_MAX_PANELS). Hidden unless Spectator Mode
// is on, since the panels only exist for a spectating host.
static MenuItemHandlerResult menuhandlerHostPanels(s32 operation, struct menuitem *item, union handlerdata *data)
{
	static const char *const opts[] = { "1", "2", "3", "4" };
	switch (operation) {
	case MENUOP_CHECKHIDDEN:
		return !g_NetMenuHostSpectator;
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = sizeof(opts) / sizeof(opts[0]);
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t)opts[data->dropdown.value];
	case MENUOP_SET:
		g_NetMenuHostPanels = (s32)data->checkbox.value + 1;
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = (g_NetMenuHostPanels >= 1) ? g_NetMenuHostPanels - 1 : 0;
		break;
	}
	return 0;
}

MenuItemHandlerResult menuhandlerHostStart(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_SET) {
		if (netStartServer(g_NetMenuPort, g_NetMenuMaxPlayers) == 0) {
			// Dedicated server (CLI --dedicated / --dedicated-windowed):
			// netStartServer already forced is_spectator=1 and panel count=0.
			// Don't let the menu-driven "Host Spectator" toggle clobber that
			// — there's no UI in dedicated mode, g_NetMenuHostSpectator
			// stays at its default 0, and applying it would re-spawn a
			// ghost host player. Same reason we don't touch g_MpSetup.options
			// for HOSTSPECTATOR below in dedicated.
			if (!g_NetDedicatedMode) {
				// Stamp spectator state onto the local client. This survives
				// mpsetupLoadCurrentFile (which clobbers g_MpSetup) and is the
				// authoritative source the spectator code reads at runtime.
				if (g_NetLocalClient) {
					g_NetLocalClient->is_spectator = (u8)(g_NetMenuHostSpectator ? 1 : 0);
				}
				g_SpectatorPanelCount = g_NetMenuHostSpectator ? g_NetMenuHostPanels : 1;
			}

			// load the setup file when entering the Combat Simulator
			mpsetupCopyAllFromPak();
			mpsetupLoadCurrentFile();

			// mpsetupLoadCurrentFile just overwrote g_MpSetup.options from
			// disk, so re-apply MPOPTION_HOSTSPECTATOR here. The bit doesn't
			// gate server-side behaviour (that uses g_NetLocalClient->is_spectator),
			// but SVC_LOBBY_STATE/SVC_STAGE_START ship g_MpSetup.options as-is
			// so remote clients can show "Host Spectator" in their options
			// list.
			if (g_NetDedicatedMode || g_NetMenuHostSpectator) {
				g_MpSetup.options |= MPOPTION_HOSTSPECTATOR;
			} else {
				g_MpSetup.options &= ~MPOPTION_HOSTSPECTATOR;
			}

			menuhandlerMainMenuCombatSimulator(MENUOP_SET, NULL, NULL);
			menuhandlerMpAdvancedSetup(MENUOP_SET, NULL, NULL);
		}
	}

	return 0;
}

/* admin: lightweight match-setup menu (see docs/PORT_ADMIN_GUI_CONFIGURE.md) */

// Difficulty applied uniformly to the sims configured in this menu. Menu-local;
// the resulting g_MpSetup / g_BotConfigsArray are what the push (CLC_ADMIN_SETUP)
// actually serialises. Weapons are edited via the reused Combat Sim Weapons
// sub-dialog (g_MpWeaponsMenuDialog), which writes g_MpSetup.weapons directly.
static s32 g_NetAdminMenuSimDiff = BOTDIFF_NORMAL;

// Rebuild g_BotConfigsArray for `count` sims at difficulty `diff`, exactly like
// playlistApply's bot block: clear every bot slot (chrslots bit + name), then
// create N from the difficulty profile. g_BotCount is the count the push
// serialises. Pure config — no world props are touched (mpCreateBotFromProfile
// only writes g_BotConfigsArray / chrslots and draws a head/body from the RNG),
// so this is safe to run while the client's lobby world is ticking.
static void netAdminMenuSetSims(s32 count, u8 diff)
{
	if (count < 0) {
		count = 0;
	}
	if (count > MAX_BOTS) {
		count = MAX_BOTS;
	}
	for (s32 i = 0; i < MAX_BOTS; ++i) {
		g_MpSetup.chrslots &= ~(1u << (i + MAX_PLAYERS));
		g_BotConfigsArray[i].base.name[0] = '\0';
	}
	for (s32 i = 0; i < count; ++i) {
		mpCreateBotFromProfile(i, diff);
	}
	g_BotCount = count;
}

static MenuItemHandlerResult menuhandlerNetAdminScenario(s32 operation, struct menuitem *item, union handlerdata *data)
{
	static const char *const opts[] = {
		"Combat", "Hold the Briefcase", "Hacker Central",
		"Pop a Cap", "King of the Hill", "Capture the Case",
		"Graffiti", "Zones", "Race",
	};
	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = sizeof(opts) / sizeof(opts[0]);
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t)opts[data->dropdown.value];
	case MENUOP_SET:
		g_MpSetup.scenario = (u8)data->checkbox.value;
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = (g_MpSetup.scenario < sizeof(opts) / sizeof(opts[0])) ? g_MpSetup.scenario : 0;
		break;
	}
	return 0;
}

static MenuItemHandlerResult menuhandlerNetAdminSims(s32 operation, struct menuitem *item, union handlerdata *data)
{
	static const char *const opts[] = { "0", "1", "2", "3", "4", "5", "6", "7", "8" }; // 0..MAX_BOTS
	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = MAX_BOTS + 1;
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t)opts[data->dropdown.value];
	case MENUOP_SET:
		netAdminMenuSetSims((s32)data->checkbox.value, (u8)g_NetAdminMenuSimDiff);
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = (g_BotCount <= MAX_BOTS) ? (uintptr_t)g_BotCount : MAX_BOTS;
		break;
	}
	return 0;
}

static MenuItemHandlerResult menuhandlerNetAdminSimDiff(s32 operation, struct menuitem *item, union handlerdata *data)
{
	static const char *const opts[] = { "Meat", "Easy", "Normal", "Hard", "Perfect", "Dark" };
	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = sizeof(opts) / sizeof(opts[0]);
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t)opts[data->dropdown.value];
	case MENUOP_SET:
		g_NetAdminMenuSimDiff = (s32)data->checkbox.value;
		// Re-apply the new difficulty to the currently-configured sim count.
		netAdminMenuSetSims(g_BotCount, (u8)g_NetAdminMenuSimDiff);
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = (g_NetAdminMenuSimDiff >= 0 && g_NetAdminMenuSimDiff <= BOTDIFF_DARK)
				? (uintptr_t)g_NetAdminMenuSimDiff : BOTDIFF_NORMAL;
		break;
	}
	return 0;
}

static MenuItemHandlerResult menuhandlerNetAdminPushStart(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_SET) {
		// Serialise g_MpSetup + bot configs to the server (CLC_ADMIN_SETUP). The
		// server validates admin/control, commits, and runs mpStartMatch;
		// SVC_STAGE_START then transitions every client into the match. (On a
		// listen host this starts the match locally instead.)
		netAdminPushStart();
		menuPopDialog();
	}
	return 0;
}

// Seed the editable setup from the server's broadcast lobby state when the menu
// opens, so the admin starts from the running config rather than stale local
// menu values. Sims are rebuilt to the lobby's bot count (at the menu's current
// difficulty); weapons aren't reconstructable from the lobby display block, so
// they keep their current loadout until a preset is picked.
static s32 netAdminSetupDialogHandler(s32 operation, struct menudialogdef *dialogdef, union handlerdata *data)
{
	if (operation == MENUOP_OPEN && g_NetLobbyState.valid) {
		g_MpSetup.stagenum       = g_NetLobbyState.stagenum;
		g_MpSetup.scenario       = g_NetLobbyState.scenario;
		g_MpSetup.options        = g_NetLobbyState.options;
		g_MpSetup.scorelimit     = g_NetLobbyState.scorelimit;
		g_MpSetup.timelimit      = g_NetLobbyState.timelimit;
		g_MpSetup.teamscorelimit = g_NetLobbyState.teamscorelimit;
		// Rebuild the sim configs to match the server's current bot count so the
		// pushed g_BotConfigsArray / chrslots / g_BotCount are always consistent,
		// even if the admin pushes without touching the Simulants control.
		netAdminMenuSetSims(g_NetLobbyState.num_bots, (u8)g_NetAdminMenuSimDiff);
	}
	return 0;
}

// Game options live in their own sub-dialog so the main menu stays compact (a
// long flat menu overflowed the screen). Each checkbox reuses
// menuhandlerMpCheckboxOption, which toggles its g_MpSetup.options bit
// (item->param3) — same handler the Combat Sim options menu uses.
static struct menuitem g_NetAdminOptionsMenuItems[] = {
	{ MENUITEMTYPE_CHECKBOX, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"One Hit Kills", MPOPTION_ONEHITKILLS,  menuhandlerMpCheckboxOption },
	{ MENUITEMTYPE_CHECKBOX, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Slow Motion",   MPOPTION_SLOWMOTION_ON, menuhandlerMpCheckboxOption },
	{ MENUITEMTYPE_CHECKBOX, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Fast Movement", MPOPTION_FASTMOVEMENT, menuhandlerMpCheckboxOption },
	{ MENUITEMTYPE_CHECKBOX, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Teams",         MPOPTION_TEAMSENABLED, menuhandlerMpCheckboxOption },
	{ MENUITEMTYPE_CHECKBOX, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"No Radar",      MPOPTION_NORADAR,      menuhandlerMpCheckboxOption },
	{ MENUITEMTYPE_CHECKBOX, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"No Auto-Aim",   MPOPTION_NOAUTOAIM,    menuhandlerMpCheckboxOption },
	{ MENUITEMTYPE_CHECKBOX, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Friendly Fire", MPOPTION_FRIENDLYFIRE, menuhandlerMpCheckboxOption },
	{ MENUITEMTYPE_CHECKBOX, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Kills = Score", MPOPTION_KILLSSCORE,   menuhandlerMpCheckboxOption },
	{ MENUITEMTYPE_SEPARATOR, 0, 0, 0, 0, NULL },
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_SELECTABLE_CLOSESDIALOG | MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Back\n", 0, NULL },
	{ MENUITEMTYPE_END },
};

static struct menudialogdef g_NetAdminOptionsMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Match Options",
	g_NetAdminOptionsMenuItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT | MENUDIALOGFLAG_STARTSELECTS,
	NULL,
};

static struct menuitem g_NetAdminSetupMenuItems[] = {
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_OPENSDIALOG | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Arena\n",
		(uintptr_t)&mpMenuTextArenaName,
		(void *)&g_MpArenaMenuDialog,
	},
	{
		MENUITEMTYPE_DROPDOWN,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Scenario",
		0,
		menuhandlerNetAdminScenario,
	},
	{
		MENUITEMTYPE_DROPDOWN,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Simulants",
		0,
		menuhandlerNetAdminSims,
	},
	{
		MENUITEMTYPE_DROPDOWN,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Sim Difficulty",
		0,
		menuhandlerNetAdminSimDiff,
	},
	{ MENUITEMTYPE_SEPARATOR, 0, 0, 0, 0, NULL },
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_OPENSDIALOG | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Weapons\n",
		0,
		(void *)&g_MpWeaponsMenuDialog,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_OPENSDIALOG | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Limits\n",
		0,
		(void *)&g_MpLimitsMenuDialog,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_OPENSDIALOG | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Options\n",
		0,
		(void *)&g_NetAdminOptionsMenuDialog,
	},
	{ MENUITEMTYPE_SEPARATOR, 0, 0, 0, 0, NULL },
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Push & Start Match\n",
		0,
		menuhandlerNetAdminPushStart,
	},
	{ MENUITEMTYPE_SEPARATOR, 0, 0, 0, 0, NULL },
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_CLOSESDIALOG | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Back\n",
		0,
		NULL,
	},
	{ MENUITEMTYPE_END },
};

static struct menudialogdef g_NetAdminSetupMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Admin: Match Setup",
	g_NetAdminSetupMenuItems,
	netAdminSetupDialogHandler,
	MENUDIALOGFLAG_LITERAL_TEXT | MENUDIALOGFLAG_STARTSELECTS,
	NULL,
};

// Admin remote control: open the lightweight "Admin: Match Setup" menu so a
// connected admin can configure the next match (arena, scenario, sims, limits,
// options, weapons) and push it with the in-menu "Push & Start" button
// (or /admin pushstart). Unlike the old menu-configure flow this edits the
// already-synced g_MpSetup / g_BotConfigsArray in place — no mpInit / pak reload
// / world teardown — so it is safe to open while the client's lobby world is
// live. The server stays the sole authority: the push (CLC_ADMIN_SETUP) is
// rejected unless the sender is the logged-in admin holding control, so opening
// and editing the local copy is harmless. Reached from /admin configure.
void netAdminConfigure(void)
{
	if (g_NetMode != NETMODE_CLIENT) {
		sysLogPrintf(LOG_CHAT, "admin: configure only works as a connected client");
		return;
	}
	if (!g_NetLocalClient || g_NetLocalClient->state < CLSTATE_LOBBY) {
		sysLogPrintf(LOG_CHAT, "admin: connect to a server first");
		return;
	}
	// The dialog is pushed onto the (lobby) menu underneath the console; tell the
	// admin to close the console so it's visible. The push it produces is still
	// gated server-side, so remind them to take control first.
	menuPushDialog(&g_NetAdminSetupMenuDialog);
	sysLogPrintf(LOG_CHAT, "admin: opened Match Setup menu - press ~ to close the console (login + take control to push)");
}

/* host: password + public listing */

static s32 g_NetHostPasswordPtr = 0;
static struct menudialogdef g_NetHostPasswordDialog;

// Value shown next to "Password:" in the host menu — masked, or "(none)".
static const char *menutextHostPassword(struct menuitem *item)
{
	static char tmp[NET_MAX_PASSWORD + 2];
	if (!g_NetServerPassword[0]) {
		return "(none)\n";
	}
	s32 n = (s32)strlen(g_NetServerPassword);
	if (n > NET_MAX_PASSWORD) { n = NET_MAX_PASSWORD; }
	s32 i;
	for (i = 0; i < n; i++) { tmp[i] = '*'; }
	tmp[i] = '\n';
	tmp[i + 1] = '\0';
	return tmp;
}

// Editable masked password in the entry dialog.
static const char *menutextHostPasswordEntry(struct menuitem *item)
{
	static char tmp[NET_MAX_PASSWORD + 3];
	s32 n = (s32)strlen(g_NetServerPassword);
	if (n > NET_MAX_PASSWORD) { n = NET_MAX_PASSWORD; }
	s32 i;
	for (i = 0; i < n; i++) { tmp[i] = '*'; }
	tmp[i] = '_';
	tmp[i + 1] = '\n';
	tmp[i + 2] = '\0';
	return tmp;
}

static MenuItemHandlerResult menuhandlerEnterHostPassword(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (!menuIsDialogOpen(&g_NetHostPasswordDialog)) {
		return 0;
	}
	if (inputTextHandler(g_NetServerPassword, NET_MAX_PASSWORD, &g_NetHostPasswordPtr, false) < 0) {
		// ESC / Enter — keep whatever was typed and close
		inputStopTextInput();
		menuPopDialog();
	}
	return 0;
}

static struct menuitem g_NetHostPasswordMenuItems[] = {
	{
		MENUITEMTYPE_LABEL,
		0,
		MENUITEMFLAG_SELECTABLE_CENTRE,
		(uintptr_t)&menutextHostPasswordEntry,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SEPARATOR,
		0, 0, 0, 0, NULL,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_CENTRE | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"ESC to save\n",
		0,
		menuhandlerEnterHostPassword,
	},
	{ MENUITEMTYPE_END },
};

static struct menudialogdef g_NetHostPasswordDialog = {
	MENUDIALOGTYPE_SUCCESS,
	(uintptr_t)"Set Password (blank = open)",
	g_NetHostPasswordMenuItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT | MENUDIALOGFLAG_IGNOREBACK | MENUDIALOGFLAG_STARTSELECTS,
	NULL,
};

static MenuItemHandlerResult menuhandlerHostPassword(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_SET) {
		inputClearLastKey();
		inputClearLastTextChar();
		inputStartTextInput();
		g_NetHostPasswordPtr = (s32)strlen(g_NetServerPassword);
		menuPushDialog(&g_NetHostPasswordDialog);
	}
	return 0;
}

/* host: server name */

static s32 g_NetServerNamePtr = 0;
static struct menudialogdef g_NetServerNameDialog;

// Value shown next to "Server Name:" in the host menu.
static const char *menutextHostServerName(struct menuitem *item)
{
	static char tmp[sizeof(g_NetServerName) + 1];
	snprintf(tmp, sizeof(tmp), "%s\n", g_NetServerName[0] ? g_NetServerName : "(unnamed)");
	return tmp;
}

// Editable name (with a trailing cursor) in the entry dialog.
static const char *menutextHostServerNameEntry(struct menuitem *item)
{
	static char tmp[sizeof(g_NetServerName) + 2];
	snprintf(tmp, sizeof(tmp), "%s_\n", g_NetServerName);
	return tmp;
}

static MenuItemHandlerResult menuhandlerEnterHostServerName(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (!menuIsDialogOpen(&g_NetServerNameDialog)) {
		return 0;
	}
	if (inputTextHandler(g_NetServerName, sizeof(g_NetServerName), &g_NetServerNamePtr, false) < 0) {
		// ESC / Enter — keep whatever was typed and close
		inputStopTextInput();
		menuPopDialog();
	}
	return 0;
}

static struct menuitem g_NetServerNameMenuItems[] = {
	{
		MENUITEMTYPE_LABEL,
		0,
		MENUITEMFLAG_SELECTABLE_CENTRE,
		(uintptr_t)&menutextHostServerNameEntry,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SEPARATOR,
		0, 0, 0, 0, NULL,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_CENTRE | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"ESC to save\n",
		0,
		menuhandlerEnterHostServerName,
	},
	{ MENUITEMTYPE_END },
};

static struct menudialogdef g_NetServerNameDialog = {
	MENUDIALOGTYPE_SUCCESS,
	(uintptr_t)"Server Name",
	g_NetServerNameMenuItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT | MENUDIALOGFLAG_IGNOREBACK | MENUDIALOGFLAG_STARTSELECTS,
	NULL,
};

static MenuItemHandlerResult menuhandlerHostServerName(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_SET) {
		inputClearLastKey();
		inputClearLastTextChar();
		inputStartTextInput();
		g_NetServerNamePtr = (s32)strlen(g_NetServerName);
		menuPushDialog(&g_NetServerNameDialog);
	}
	return 0;
}

struct menuitem g_NetHostMenuItems[] = {
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Server Name:\n",
		(uintptr_t)&menutextHostServerName,
		menuhandlerHostServerName,
	},
	{
		MENUITEMTYPE_SLIDER,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Max Players",
		NET_MAX_CLIENTS,
		menuhandlerHostMaxPlayers,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Password:\n",
		(uintptr_t)&menutextHostPassword,
		menuhandlerHostPassword,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Spectator Mode:\n",
		(uintptr_t)&menutextHostSpectator,
		menuhandlerHostSpectator,
	},
	{
		MENUITEMTYPE_DROPDOWN,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Spectator Panels",
		0,
		menuhandlerHostPanels,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		0,
		L_MPMENU_036, // "Start Game"
		0,
		menuhandlerHostStart,
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
		L_OPTIONS_213, // "Back"
		0,
		NULL,
	},
	{ MENUITEMTYPE_END },
};

struct menudialogdef g_NetHostMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Host Network Game",
	g_NetHostMenuItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT | MENUDIALOGFLAG_STARTSELECTS,
	NULL,
};

/* Host Online Game: the master spawns a dedicated instance for us, we connect
 * as its auto-admin and drive the full Combat Sim hosting UI. See
 * docs/PORT_HOSTED_SERVER.md. */

extern struct menudialogdef g_NetJoiningDialog; // defined below in the join section

// Auto-admin handshake progress for this session: 0 = waiting for CLSTATE_LOBBY,
// 1 = login+take sent, 2 = hosting UI entered. Reset when a grant is accepted.
static s32 g_NetHostOnlineStep = 0;
static u32 g_NetHostOnlineStepTick = 0;

// Reload a fresh CITRAINING world and re-enter the Combat Sim setup through the
// existing "returning from a multiplayer match" latch (menutick.c, var80087260)
// — the same proven path the post-match return uses, mirroring the netDisconnect
// wasingame return (net.c). The setup menus then open over a virgin 1-player CI
// world (frame >= 4), which is the context they were designed for. Running the
// title-screen setup-load directly over the live connected lobby world is the
// documented shieldhits crash class (docs/PORT_ADMIN_GUI_CONFIGURE.md) — never
// do that.
void netHostOnlineEnterSetup(void)
{
	mpSetPaused(MPPAUSEMODE_UNPAUSED);
	g_MpSetup.chrslots = 1;
	g_Vars.mplayerisrunning = false;
	g_Vars.normmplayerisrunning = false;
	g_Vars.lvmpbotlevel = 0;
	titleSetNextStage(STAGE_CITRAINING);
	setNumPlayers(1);
	titleSetNextMode(TITLEMODE_SKIP);
	mainChangeToStage(STAGE_CITRAINING);
	var80087260 = 3; // arm the post-match menu-reopen latch (menutick.c)
}

// Status line in the wait dialog.
static const char *menutextHostOnlineStatus(struct menuitem *item)
{
	static char tmp[NET_HOSTREQ_REASON_LEN + 2];
	switch (g_NetHostRequestState) {
	case NETHOSTREQ_REQUESTING:
		return "Requesting a server...\n";
	case NETHOSTREQ_GRANTED:
		return "Server granted - connecting...\n";
	case NETHOSTREQ_DENIED:
	case NETHOSTREQ_ERROR:
		snprintf(tmp, sizeof(tmp), "%s\n", g_NetHostDenyReason[0] ? g_NetHostDenyReason : "Request failed");
		return tmp;
	}
	return "\n";
}

static MenuItemHandlerResult menuhandlerHostOnlineWait(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (inputKeyPressed(VK_ESCAPE)
			|| (operation == MENUOP_SET && g_NetHostRequestState != NETHOSTREQ_REQUESTING)) {
		netHostRequestClose();
		menuPopDialog();
	}
	return 0;
}

static s32 netHostOnlineWaitDialogHandler(s32 operation, struct menudialogdef *dialogdef, union handlerdata *data)
{
	if (operation == MENUOP_TICK) {
		netHostRequestTick();

		if (g_NetHostRequestState == NETHOSTREQ_GRANTED) {
			netHostRequestClose();

			// The instance was spawned with --password <our password>; our own
			// join must supply it in CLC_AUTH like any other client.
			strncpy(g_NetJoinPassword, g_NetServerPassword, NET_MAX_PASSWORD - 1);
			g_NetJoinPassword[NET_MAX_PASSWORD - 1] = '\0';
			strncpy(g_NetJoinAddr, g_NetHostGrantAddr, NET_MAX_ADDR);
			g_NetJoinAddr[NET_MAX_ADDR] = '\0';
			strncpy(g_NetAutoAdminToken, g_NetHostGrantToken, NET_MAX_PASSWORD - 1);
			g_NetAutoAdminToken[NET_MAX_PASSWORD - 1] = '\0';

			if (netStartClient(g_NetJoinAddr) == 0) {
				g_NetHostOnlineMode = 1;
				g_NetHostOnlineStep = 0;
				menuPopDialog(); // this wait dialog
				menuPushDialog(&g_NetJoiningDialog);
			} else {
				strcpy(g_NetHostDenyReason, "Could not connect to the server");
				g_NetHostRequestState = NETHOSTREQ_ERROR;
			}
		}
	} else if (operation == MENUOP_CLOSE) {
		netHostRequestClose(); // no-op if already closed
	}
	return 0;
}

static struct menuitem g_NetHostOnlineWaitMenuItems[] = {
	{
		MENUITEMTYPE_LABEL,
		0,
		MENUITEMFLAG_SELECTABLE_CENTRE,
		(uintptr_t)&menutextHostOnlineStatus,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SEPARATOR,
		0, 0, 0, 0, NULL,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_CENTRE | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"ESC to cancel\n",
		0,
		menuhandlerHostOnlineWait,
	},
	{ MENUITEMTYPE_END },
};

static struct menudialogdef g_NetHostOnlineWaitDialog = {
	MENUDIALOGTYPE_SUCCESS,
	(uintptr_t)"Host Online Game",
	g_NetHostOnlineWaitMenuItems,
	netHostOnlineWaitDialogHandler,
	MENUDIALOGFLAG_LITERAL_TEXT | MENUDIALOGFLAG_IGNOREBACK | MENUDIALOGFLAG_STARTSELECTS,
	NULL,
};

static MenuItemHandlerResult menuhandlerHostOnlineStart(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_SET) {
		netHostRequestOpen(g_NetServerName, g_NetMenuMaxPlayers, g_NetServerPassword);
		menuPushDialog(&g_NetHostOnlineWaitDialog);
	}
	return 0;
}

// Same name/players/password controls as the local host menu (shared handlers
// and backing globals), minus the port and spectator items — the master picks
// the port, and the requester is a remote admin client, not a local server.
static struct menuitem g_NetHostOnlineMenuItems[] = {
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Server Name:\n",
		(uintptr_t)&menutextHostServerName,
		menuhandlerHostServerName,
	},
	{
		MENUITEMTYPE_SLIDER,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Max Players",
		NET_MAX_CLIENTS,
		menuhandlerHostMaxPlayers,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Password:\n",
		(uintptr_t)&menutextHostPassword,
		menuhandlerHostPassword,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Request Server\n",
		0,
		menuhandlerHostOnlineStart,
	},
	{
		MENUITEMTYPE_SEPARATOR,
		0, 0, 0, 0, NULL,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_CLOSESDIALOG,
		L_OPTIONS_213, // "Back"
		0,
		NULL,
	},
	{ MENUITEMTYPE_END },
};

static struct menudialogdef g_NetHostOnlineMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Host Online Game",
	g_NetHostOnlineMenuItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT | MENUDIALOGFLAG_STARTSELECTS,
	NULL,
};

static MenuItemHandlerResult menuhandlerHostOnlineGame(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_SET) {
		g_NetMenuMaxPlayers = g_NetMaxClients;
		menuPushDialog(&g_NetHostOnlineMenuDialog);
	}
	return 0;
}

/* join */

static const char *menutextJoinAddress(struct menuitem *item)
{
	static char tmp[256 + 1];
	if (item && item->flags & MENUITEMFLAG_SELECTABLE_CENTRE) {
		if (g_NetMode == NETMODE_NONE) {
			// Address entry dialog — hide IP from stream if requested
			if (g_NetJoinHidden) {
				return "_\n";
			}
			snprintf(tmp, sizeof(tmp), "%s_\n", g_NetJoinAddr);
		} else if (g_NetLocalClient->state == CLSTATE_CONNECTING) {
			snprintf(tmp, sizeof(tmp), g_NetJoinHidden ? "Connecting...\n" : "Connecting to %s...\n", g_NetJoinAddr);
		} else if (g_NetLocalClient->state == CLSTATE_AUTH) {
			snprintf(tmp, sizeof(tmp), g_NetJoinHidden ? "Authenticating...\n" : "Authenticating with %s...\n", g_NetJoinAddr);
		} else if (g_NetLocalClient->state == CLSTATE_LOBBY && !g_NetLobbyState.valid) {
			snprintf(tmp, sizeof(tmp), "Waiting for host...\n");
		} else {
			return "";
		}
	} else {
		// "Address:" value in the join menu
		if (g_NetJoinHidden) {
			// Show the loaded favourite's name when the address matches one
			for (s32 i = 0; i < NET_FAV_MAX; i++) {
				if (g_NetFavAddr[i][0] && strcmp(g_NetFavAddr[i], g_NetJoinAddr) == 0 && g_NetFavName[i][0]) {
					snprintf(tmp, sizeof(tmp), "%s\n", g_NetFavName[i]);
					return tmp;
				}
			}
			return "\n";
		}
		snprintf(tmp, sizeof(tmp), "%s\n", g_NetJoinAddr);
	}
	return tmp;
}

// Dispatcher: each lobby info line is a MENUITEMTYPE_LABEL whose item->param
// carries a line index. Returns "" (zero height — invisible) when lobby state
// is not yet valid or the slot is empty.
static char *menutextLobbyLine(struct menuitem *item)
{
	static char tmp[256];
	static const char *const s_diff[] = {
		"Meat", "Easy", "Normal", "Hard", "Perfect", "Dark", "?"
	};

	if (!g_NetLobbyState.valid) {
		return "";
	}

	const struct netlobbystate *ls = &g_NetLobbyState;
	const u8 idx = item->param;

	// 0: scenario + arena
	if (idx == 0) {
		snprintf(tmp, sizeof(tmp), "%s - %s\n",
				ls->scenario_name[0] ? ls->scenario_name : "?",
				ls->arena_name[0]    ? ls->arena_name    : "?");
		return tmp;
	}

	// 1: score / time limits (hidden if all zero)
	if (idx == 1) {
		if (!ls->scorelimit && !ls->timelimit && !ls->teamscorelimit) {
			return "";
		}
		tmp[0] = '\0';
		char seg[48];
		if (ls->scorelimit) {
			snprintf(seg, sizeof(seg), "Pts: %d", (s32)ls->scorelimit);
			strncat(tmp, seg, sizeof(tmp) - strlen(tmp) - 1);
		}
		if (ls->timelimit) {
			if (tmp[0]) { strncat(tmp, "  ", sizeof(tmp) - strlen(tmp) - 1); }
			snprintf(seg, sizeof(seg), "Time: %d:00", (s32)ls->timelimit);
			strncat(tmp, seg, sizeof(tmp) - strlen(tmp) - 1);
		}
		if (ls->teamscorelimit) {
			if (tmp[0]) { strncat(tmp, "  ", sizeof(tmp) - strlen(tmp) - 1); }
			snprintf(seg, sizeof(seg), "Team Pts: %d", (s32)ls->teamscorelimit);
			strncat(tmp, seg, sizeof(tmp) - strlen(tmp) - 1);
		}
		strncat(tmp, "\n", sizeof(tmp) - strlen(tmp) - 1);
		return tmp;
	}

	// 2: weapon set name
	if (idx == 2) {
		if (!ls->weaponset_name[0]) { return ""; }
		snprintf(tmp, sizeof(tmp), "Weapons: %s\n", ls->weaponset_name);
		return tmp;
	}

	// 3-4: weapon slot rows (3 slots each)
	if (idx >= 3 && idx <= 4) {
		const s32 row = (s32)(idx - 3);
		tmp[0] = '\0';
		for (s32 col = 0; col < 3; col++) {
			const s32 slot = row * 3 + col;
			if (slot < NUM_MPWEAPONSLOTS && ls->weapon_names[slot][0]) {
				if (tmp[0]) { strncat(tmp, " / ", sizeof(tmp) - strlen(tmp) - 1); }
				strncat(tmp, ls->weapon_names[slot], sizeof(tmp) - strlen(tmp) - 1);
			}
		}
		if (!tmp[0]) { return ""; }
		strncat(tmp, "\n", sizeof(tmp) - strlen(tmp) - 1);
		return tmp;
	}

	// 5: enabled options comma-separated (hidden if none)
	if (idx == 5) {
		// flag is u64: the Classic options live in the high 32 bits of
		// ls->options (a u32 flag would mask them to 0).
		static const struct { u64 flag; const char *name; } s_opts[] = {
			{ MPOPTION_ONEHITKILLS,         "One Hit Kills"    },
			{ MPOPTION_FASTMOVEMENT,        "Fast Movement"    },
			{ MPOPTION_SLOWMOTION_ON,       "Slow Motion"      },
			{ MPOPTION_NORADAR,             "No Radar"         },
			{ MPOPTION_NOAUTOAIM,           "No Auto-Aim"      },
			{ MPOPTION_NOPLAYERHIGHLIGHT,   "No Highlight"     },
			{ MPOPTION_FRIENDLYFIRE,        "Friendly Fire"    },
			{ MPOPTION_SPAWNWITHWEAPON,     "Start w/Weapon"   },
			{ MPOPTION_KILLSSCORE,          "Kills Score"      },
			{ MPOPTION_NODRUGBLUR,          "No Drug Blur"     },
			{ MPOPTION_CONTROLLERS_ONLY,    "Controllers Only" },
			{ MPOPTION_HOSTSPECTATOR,       "Host Spectator"   },
			{ MPOPTION_GOLDENEYE,           "GoldenEye Style"  },
			{ MPOPTION_NODOORS,             "No Doors"         },
			{ MPOPTION_CLASSIC_SNAPLEAN,    "Snap Lean"        },
			{ MPOPTION_CLASSIC_NOCROUCHACC, "No Crouch Acc"    },
			{ MPOPTION_CLASSIC_RELOAD,      "Classic Reloads"  },
			{ MPOPTION_CLASSIC_LEDGEWALL,   "Ledge Walls"      },
			{ MPOPTION_CLASSIC_SIGHT,       "Classic Xhair"    },
			{ MPOPTION_CLASSIC_HIDESIGHT,   "Hide Xhair"       },
			{ MPOPTION_CLASSIC_GEHUD,       "GE HUD"           },
			{ MPOPTION_CLASSIC_NOSECONDARY, "No Secondary"     },
			{ MPOPTION_CLASSIC_NOMIDCROUCH, "No Mid-Crouch"    },
			{ MPOPTION_CLASSIC_NODUALWIELD, "No Dual Wield"    },
			{ MPOPTION_CLASSIC_IFRAMES,     "I-Frames"         },
			{ MPOPTION_CLASSIC_NOBLUR,      "No Blur"          },
			{ MPOPTION_TEAMSENABLED,        "Teams"            },
		};
		char opts[220];
		opts[0] = '\0';
		for (s32 i = 0; i < (s32)(sizeof(s_opts) / sizeof(s_opts[0])); i++) {
			if (ls->options & s_opts[i].flag) {
				if (opts[0]) { strncat(opts, ", ", sizeof(opts) - strlen(opts) - 1); }
				strncat(opts, s_opts[i].name, sizeof(opts) - strlen(opts) - 1);
			}
		}
		if (!opts[0]) { return ""; }
		snprintf(tmp, sizeof(tmp), "Opts: %s\n", opts);
		return tmp;
	}

	// 6: players header (hidden if no clients)
	if (idx == 6) {
		if (!ls->num_clients) { return ""; }
		snprintf(tmp, sizeof(tmp), "Players (%d):\n", (s32)ls->num_clients);
		return tmp;
	}

	// 7-14: player slots 0-7
	if (idx >= 7 && idx <= 14) {
		const s32 slot = (s32)(idx - 7);
		if (slot >= (s32)ls->num_clients) { return ""; }
		const struct netlobbyclient *cl = &ls->clients[slot];
		const bool is_me = g_NetLocalClient
				&& strcmp(cl->name, g_NetLocalClient->settings.name) == 0;
		const bool teams_on = (ls->options & MPOPTION_TEAMSENABLED) != 0;
		char ping[16];
		if (cl->ping == 0) {
			snprintf(ping, sizeof(ping), "--");
		} else {
			snprintf(ping, sizeof(ping), "%dms", (s32)cl->ping);
		}
		const char *spec_tag = cl->is_spectator ? " (spec)" : "";
		if (teams_on && cl->team < MAX_TEAMS) {
			snprintf(tmp, sizeof(tmp), "%s%s%s  %s  %s\n",
					is_me ? "* " : "  ",
					cl->name, spec_tag, ping, ls->teamnames[cl->team]);
		} else {
			snprintf(tmp, sizeof(tmp), "%s%s%s  %s\n",
					is_me ? "* " : "  ",
					cl->name, spec_tag, ping);
		}
		return tmp;
	}

	// 15: sims header (hidden if no bots)
	if (idx == 15) {
		if (!ls->num_bots) { return ""; }
		snprintf(tmp, sizeof(tmp), "Sims (%d):\n", (s32)ls->num_bots);
		return tmp;
	}

	// 16-23: sim slots 0-7
	if (idx >= 16 && idx <= 23) {
		const s32 slot = (s32)(idx - 16);
		if (slot >= (s32)ls->num_bots) { return ""; }
		const struct netlobbybot *bot = &ls->bots[slot];
		const u8 diff = (bot->difficulty < 6u) ? bot->difficulty : 6u;
		const bool teams_on = (ls->options & MPOPTION_TEAMSENABLED) != 0;
		if (teams_on && bot->team < MAX_TEAMS) {
			snprintf(tmp, sizeof(tmp), "  %s  %s  %s\n",
					bot->name, s_diff[diff], ls->teamnames[bot->team]);
		} else {
			snprintf(tmp, sizeof(tmp), "  %s  %s\n", bot->name, s_diff[diff]);
		}
		return tmp;
	}

	return "";
}

// Defined later in this file; referenced here for the co-op join transition.
static struct menudialogdef g_NetCoopHostMenuDialog;

static MenuItemHandlerResult menuhandlerJoining(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (inputKeyPressed(VK_ESCAPE)) {
		netDisconnect();
		menuPopDialog();
		return 0;
	}

	// #4: if the host's lobby state says this is a co-op game, swap the generic
	// Combat-Sim "Joining Game..." window for the co-op Match Setup window (the
	// player's own Body Type editable, host-only settings greyed). Happens once —
	// once we pop this dialog, this handler stops running.
	if (g_NetLobbyState.valid && g_NetLobbyState.iscoop
			&& g_NetLocalClient && g_NetLocalClient->state >= CLSTATE_LOBBY) {
		menuPopDialog();
		menuPushDialog(&g_NetCoopHostMenuDialog);
	}

	return 0;
}

// Host Online Game auto-admin handshake, pumped once per frame from the Joining
// dialog's MENUOP_TICK while connected to the granted instance. Both admin
// lines ride the reliable ordered control channel, so `login` is always
// processed before `take` and no SVC_ADMIN reply parsing is needed (the
// replies are plain console text; watch the console for confirmation).
static void netHostOnlineJoiningTick(void)
{
	if (!g_NetHostOnlineMode || g_NetHostOnlineStep >= 2
			|| !g_NetLocalClient || g_NetLocalClient->state < CLSTATE_LOBBY) {
		return;
	}

	if (g_NetHostOnlineStep == 0) {
		char line[NET_MAX_PASSWORD + 8];
		snprintf(line, sizeof(line), "login %s", g_NetAutoAdminToken);
		netClientSendAdminLine(line);
		netClientSendAdminLine("take");
		g_NetHostOnlineStep = 1;
		g_NetHostOnlineStepTick = g_NetTick;
		return;
	}

	// Give the server a moment to process login+take, then enter the hosting
	// UI through the fresh-lobby reload. The one-shot setup-load latch makes
	// menutick.c's re-entry block run the Combat Sim setup-load (the same load
	// menuhandlerHostStart does) on the fresh world.
	if ((g_NetTick - g_NetHostOnlineStepTick) >= 30u) {
		g_NetHostOnlineStep = 2;
		menuPopDialog(); // the Joining dialog
		g_NetHostOnlineSetupLoad = 1;
		netHostOnlineEnterSetup();
	}
}

static s32 netJoiningDialogHandler(s32 operation, struct menudialogdef *dialogdef, union handlerdata *data)
{
	if (operation == MENUOP_TICK) {
		// The session can die underneath this dialog (server rejected the
		// auth: files/version/password mismatch — netClientEvDisconnect
		// already logged the reason and tore the session down). Without this
		// the dialog sat on "Joining Game..." forever.
		if (g_NetMode == NETMODE_NONE) {
			menuPopDialog();
			return 0;
		}
		netHostOnlineJoiningTick();
	}
	return 0;
}

#define LOBBYLINE(n) \
	{ MENUITEMTYPE_LABEL, (n), MENUITEMFLAG_SMALLFONT, (uintptr_t)&menutextLobbyLine, 0, NULL }

struct menuitem g_NetJoiningMenuItems[] = {
	// Status row: "Connecting..." / "Waiting for host..." / "" when lobby valid
	{
		MENUITEMTYPE_LABEL,
		0,
		MENUITEMFLAG_SELECTABLE_CENTRE,
		(uintptr_t)&menutextJoinAddress,
		0,
		NULL,
	},
	// Lobby info rows — each collapses to zero height when not applicable
	LOBBYLINE(0),   // scenario - arena
	LOBBYLINE(1),   // score/time limits
	LOBBYLINE(2),   // weapon set name
	LOBBYLINE(3),   // weapon slots 0-2
	LOBBYLINE(4),   // weapon slots 3-5
	LOBBYLINE(5),   // enabled options
	LOBBYLINE(6),   // "Players (N):"
	LOBBYLINE(7),   // player 0
	LOBBYLINE(8),   // player 1
	LOBBYLINE(9),   // player 2
	LOBBYLINE(10),  // player 3
	LOBBYLINE(11),  // player 4
	LOBBYLINE(12),  // player 5
	LOBBYLINE(13),  // player 6
	LOBBYLINE(14),  // player 7
	LOBBYLINE(15),  // "Sims (N):"
	LOBBYLINE(16),  // sim 0
	LOBBYLINE(17),  // sim 1
	LOBBYLINE(18),  // sim 2
	LOBBYLINE(19),  // sim 3
	LOBBYLINE(20),  // sim 4
	LOBBYLINE(21),  // sim 5
	LOBBYLINE(22),  // sim 6
	LOBBYLINE(23),  // sim 7
	{
		MENUITEMTYPE_SEPARATOR,
		0, 0, 0, 0, NULL,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_CENTRE | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"ESC to abort\n",
		0,
		menuhandlerJoining,
	},
	{ MENUITEMTYPE_END },
};

#undef LOBBYLINE

struct menudialogdef g_NetJoiningDialog = {
	MENUDIALOGTYPE_SUCCESS,
	(uintptr_t)"Joining Game...",
	g_NetJoiningMenuItems,
	netJoiningDialogHandler, // Host Online auto-admin handshake (no-op otherwise)
	MENUDIALOGFLAG_LITERAL_TEXT | MENUDIALOGFLAG_IGNOREBACK | MENUDIALOGFLAG_STARTSELECTS,
	NULL,
};

static MenuItemHandlerResult menuhandlerEnterJoinAddress(s32 operation, struct menuitem *item, union handlerdata *data);

struct menuitem g_NetJoinAddressMenuItems[] = {
	{
		MENUITEMTYPE_LABEL,
		0,
		MENUITEMFLAG_SELECTABLE_CENTRE,
		(uintptr_t)&menutextJoinAddress,
		0,
		NULL,
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
		MENUITEMFLAG_SELECTABLE_CENTRE | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"ESC to return\n",
		0,
		menuhandlerEnterJoinAddress,
	},
	{ MENUITEMTYPE_END },
};

struct menudialogdef g_NetJoinAddressDialog = {
	MENUDIALOGTYPE_SUCCESS,
	(uintptr_t)"Enter Address",
	g_NetJoinAddressMenuItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT | MENUDIALOGFLAG_IGNOREBACK | MENUDIALOGFLAG_STARTSELECTS,
	NULL,
};

static MenuItemHandlerResult menuhandlerEnterJoinAddress(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (!menuIsDialogOpen(&g_NetJoinAddressDialog)) {
		return 0;
	}

	if (inputTextHandler(g_NetJoinAddr, NET_MAX_ADDR, &g_NetJoinAddrPtr, false) < 0) {
		// escape has been pressed, stop editing
		inputStopTextInput();
		menuPopDialog();
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerJoinAddress(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_SET) {
		inputClearLastKey();
		inputClearLastTextChar();
		inputStartTextInput();
		g_NetJoinAddrPtr = strlen(g_NetJoinAddr);
		menuPushDialog(&g_NetJoinAddressDialog);
	}

	return 0;
}

MenuItemHandlerResult menuhandlerJoinStart(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_SET) {
		if (netStartClient(g_NetJoinAddr) == 0) {
			menuPushDialog(&g_NetJoiningDialog);
		}
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerJoinPlayerSetup(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_SET) {
		// load MP player file
		filelistCreate(0, FILETYPE_MPPLAYER);
		filelistsTick();
		menuPushDialog(&g_NetJoinPlayerSetupMenuDialog);
	}

	return 0;
}

/* favourites */

static s32 g_NetFavSelected = 0;

static MenuItemHandlerResult menuhandlerEnterFavName(s32 operation, struct menuitem *item, union handlerdata *data);
static struct menudialogdef g_NetFavNameDialog;
static struct menudialogdef g_NetFavActionDialog;
static struct menudialogdef g_NetFavMenuDialog;

static const char *menutextFavEntry(struct menuitem *item)
{
	static char tmp[NET_FAV_NAME_LEN + NET_MAX_ADDR + 8];
	const s32 i = item->param;
	if (!g_NetFavAddr[i][0]) {
		return "";
	}
	if (g_NetFavName[i][0]) {
		snprintf(tmp, sizeof(tmp), "%s\n", g_NetFavName[i]);
	} else {
		snprintf(tmp, sizeof(tmp), "%s\n", g_NetFavAddr[i]);
	}
	return tmp;
}

static const char *menutextFavAdd(struct menuitem *item)
{
	if (!g_NetJoinAddr[0]) {
		return "";
	}
	for (s32 i = 0; i < NET_FAV_MAX; i++) {
		if (g_NetFavAddr[i][0] && strcmp(g_NetFavAddr[i], g_NetJoinAddr) == 0) {
			return "";
		}
	}
	for (s32 i = 0; i < NET_FAV_MAX; i++) {
		if (!g_NetFavAddr[i][0]) {
			return "Add Current Address\n";
		}
	}
	return "";
}

static MenuItemHandlerResult menuhandlerFavAdd(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_SET) {
		for (s32 i = 0; i < NET_FAV_MAX; i++) {
			if (!g_NetFavAddr[i][0]) {
				strncpy(g_NetFavAddr[i], g_NetJoinAddr, NET_MAX_ADDR);
				g_NetFavAddr[i][NET_MAX_ADDR] = '\0';
				g_NetFavName[i][0] = '\0';
				break;
			}
		}
	}
	return 0;
}

static MenuItemHandlerResult menuhandlerFavSelect(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_SET) {
		const s32 i = item->param;
		if (g_NetFavAddr[i][0]) {
			g_NetFavSelected = i;
			menuPushDialog(&g_NetFavActionDialog);
		}
	}
	return 0;
}

static const char *menutextFavActionLabel(struct menuitem *item)
{
	static char tmp[NET_FAV_NAME_LEN + NET_MAX_ADDR + 4];
	const s32 i = g_NetFavSelected;
	if (g_NetFavName[i][0]) {
		snprintf(tmp, sizeof(tmp), "%s\n", g_NetFavName[i]);
	} else {
		snprintf(tmp, sizeof(tmp), "%s\n", g_NetFavAddr[i]);
	}
	return tmp;
}

static MenuItemHandlerResult menuhandlerFavLoad(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_SET) {
		const s32 i = g_NetFavSelected;
		strncpy(g_NetJoinAddr, g_NetFavAddr[i], NET_MAX_ADDR);
		g_NetJoinAddr[NET_MAX_ADDR] = '\0';
		g_NetJoinAddrPtr = strlen(g_NetJoinAddr);
		menuPopDialog();
		menuPopDialog();
	}
	return 0;
}

static MenuItemHandlerResult menuhandlerFavRename(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_SET) {
		strncpy(g_NetFavNewName, g_NetFavName[g_NetFavSelected], NET_FAV_NAME_LEN - 1);
		g_NetFavNewName[NET_FAV_NAME_LEN - 1] = '\0';
		g_NetFavNewNamePtr = strlen(g_NetFavNewName);
		inputClearLastKey();
		inputClearLastTextChar();
		inputStartTextInput();
		menuPushDialog(&g_NetFavNameDialog);
	}
	return 0;
}

static MenuItemHandlerResult menuhandlerFavDelete(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_SET) {
		g_NetFavAddr[g_NetFavSelected][0] = '\0';
		g_NetFavName[g_NetFavSelected][0] = '\0';
		menuPopDialog();
	}
	return 0;
}

static const char *menutextFavNewName(struct menuitem *item)
{
	static char tmp[NET_FAV_NAME_LEN + 4];
	snprintf(tmp, sizeof(tmp), "%s_\n", g_NetFavNewName);
	return tmp;
}

static struct menuitem g_NetFavNameMenuItems[] = {
	{
		MENUITEMTYPE_LABEL,
		0,
		MENUITEMFLAG_SELECTABLE_CENTRE,
		(uintptr_t)&menutextFavNewName,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SEPARATOR,
		0, 0, 0, 0, NULL,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_CENTRE | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"ESC to save\n",
		0,
		menuhandlerEnterFavName,
	},
	{ MENUITEMTYPE_END },
};

static struct menudialogdef g_NetFavNameDialog = {
	MENUDIALOGTYPE_SUCCESS,
	(uintptr_t)"Favourite Name",
	g_NetFavNameMenuItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT | MENUDIALOGFLAG_IGNOREBACK | MENUDIALOGFLAG_STARTSELECTS,
	NULL,
};

static MenuItemHandlerResult menuhandlerEnterFavName(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (!menuIsDialogOpen(&g_NetFavNameDialog)) {
		return 0;
	}

	if (inputTextHandler(g_NetFavNewName, NET_FAV_NAME_LEN - 1, &g_NetFavNewNamePtr, false) < 0) {
		// ESC — save typed name into the selected slot and close
		strncpy(g_NetFavName[g_NetFavSelected], g_NetFavNewName, NET_FAV_NAME_LEN - 1);
		g_NetFavName[g_NetFavSelected][NET_FAV_NAME_LEN - 1] = '\0';
		inputStopTextInput();
		menuPopDialog();
	}

	return 0;
}

static struct menuitem g_NetFavActionMenuItems[] = {
	{
		MENUITEMTYPE_LABEL,
		0,
		MENUITEMFLAG_SMALLFONT,
		(uintptr_t)&menutextFavActionLabel,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SEPARATOR,
		0, 0, 0, 0, NULL,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Load\n",
		0,
		menuhandlerFavLoad,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Rename\n",
		0,
		menuhandlerFavRename,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Delete\n",
		0,
		menuhandlerFavDelete,
	},
	{
		MENUITEMTYPE_SEPARATOR,
		0, 0, 0, 0, NULL,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_CLOSESDIALOG,
		L_OPTIONS_213, // "Back"
		0,
		NULL,
	},
	{ MENUITEMTYPE_END },
};

static struct menudialogdef g_NetFavActionDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Favourite",
	g_NetFavActionMenuItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT | MENUDIALOGFLAG_STARTSELECTS,
	NULL,
};

static MenuItemHandlerResult menuhandlerFavMenu(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_SET) {
		menuPushDialog(&g_NetFavMenuDialog);
	}
	return 0;
}

static const char *menutextHideAddr(struct menuitem *item)
{
	return g_NetJoinHidden ? "On\n" : "Off\n";
}

static MenuItemHandlerResult menuhandlerHideAddr(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_SET) {
		g_NetJoinHidden = !g_NetJoinHidden;
	}
	return 0;
}

#define FAVLINE(n) \
	{ MENUITEMTYPE_SELECTABLE, (n), MENUITEMFLAG_SMALLFONT, \
	  (uintptr_t)&menutextFavEntry, 0, menuhandlerFavSelect }

static struct menuitem g_NetFavMenuItems[] = {
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		0,
		(uintptr_t)&menutextFavAdd,
		0,
		menuhandlerFavAdd,
	},
	{
		MENUITEMTYPE_SEPARATOR,
		0, 0, 0, 0, NULL,
	},
	FAVLINE(0),
	FAVLINE(1),
	FAVLINE(2),
	FAVLINE(3),
	FAVLINE(4),
	FAVLINE(5),
	FAVLINE(6),
	FAVLINE(7),
	{
		MENUITEMTYPE_SEPARATOR,
		0, 0, 0, 0, NULL,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_CLOSESDIALOG,
		L_OPTIONS_213, // "Back"
		0,
		NULL,
	},
	{ MENUITEMTYPE_END },
};

static struct menudialogdef g_NetFavMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Favourites",
	g_NetFavMenuItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT | MENUDIALOGFLAG_STARTSELECTS,
	NULL,
};

/* join: optional password (direct joins to a passworded server) */

static struct menudialogdef g_NetJoinPwEditDialog;

// Value shown next to "Password:" in the Join menu — masked, or "(none)".
static const char *menutextJoinPwValue(struct menuitem *item)
{
	static char tmp[NET_MAX_PASSWORD + 2];
	if (!g_NetJoinPassword[0]) {
		return "(none)\n";
	}
	s32 n = (s32)strlen(g_NetJoinPassword);
	if (n > NET_MAX_PASSWORD) { n = NET_MAX_PASSWORD; }
	s32 i;
	for (i = 0; i < n; i++) { tmp[i] = '*'; }
	tmp[i] = '\n';
	tmp[i + 1] = '\0';
	return tmp;
}

// Editable masked password in the entry dialog.
static const char *menutextJoinPwEntry(struct menuitem *item)
{
	static char tmp[NET_MAX_PASSWORD + 3];
	s32 n = (s32)strlen(g_NetJoinPassword);
	if (n > NET_MAX_PASSWORD) { n = NET_MAX_PASSWORD; }
	s32 i;
	for (i = 0; i < n; i++) { tmp[i] = '*'; }
	tmp[i] = '_';
	tmp[i + 1] = '\n';
	tmp[i + 2] = '\0';
	return tmp;
}

static MenuItemHandlerResult menuhandlerEnterJoinPw(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (!menuIsDialogOpen(&g_NetJoinPwEditDialog)) {
		return 0;
	}
	if (inputTextHandler(g_NetJoinPassword, NET_MAX_PASSWORD, &g_NetJoinPasswordPtr, false) < 0) {
		// ESC / Enter — keep whatever was typed and close
		inputStopTextInput();
		menuPopDialog();
	}
	return 0;
}

static struct menuitem g_NetJoinPwEditMenuItems[] = {
	{
		MENUITEMTYPE_LABEL,
		0,
		MENUITEMFLAG_SELECTABLE_CENTRE,
		(uintptr_t)&menutextJoinPwEntry,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SEPARATOR,
		0, 0, 0, 0, NULL,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_CENTRE | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"ESC to save\n",
		0,
		menuhandlerEnterJoinPw,
	},
	{ MENUITEMTYPE_END },
};

static struct menudialogdef g_NetJoinPwEditDialog = {
	MENUDIALOGTYPE_SUCCESS,
	(uintptr_t)"Join Password (blank = none)",
	g_NetJoinPwEditMenuItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT | MENUDIALOGFLAG_IGNOREBACK | MENUDIALOGFLAG_STARTSELECTS,
	NULL,
};

static MenuItemHandlerResult menuhandlerJoinPassword(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_SET) {
		inputClearLastKey();
		inputClearLastTextChar();
		inputStartTextInput();
		g_NetJoinPasswordPtr = (s32)strlen(g_NetJoinPassword);
		menuPushDialog(&g_NetJoinPwEditDialog);
	}
	return 0;
}

struct menuitem g_NetJoinMenuItems[] = {
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Address:\n",
		(uintptr_t)&menutextJoinAddress,
		menuhandlerJoinAddress,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Favourites\n",
		0,
		menuhandlerFavMenu,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Hide Address:\n",
		(uintptr_t)&menutextHideAddr,
		menuhandlerHideAddr,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Password:\n",
		(uintptr_t)&menutextJoinPwValue,
		menuhandlerJoinPassword,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		0,
		L_MPMENU_028, // "Player Setup"
		0,
		menuhandlerJoinPlayerSetup,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		0,
		L_MPMENU_036, // "Start Game"
		0,
		menuhandlerJoinStart,
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
		L_OPTIONS_213, // "Back"
		0,
		NULL,
	},
	{ MENUITEMTYPE_END },
};

struct menudialogdef g_NetJoinMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Join Network Game",
	g_NetJoinMenuItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT | MENUDIALOGFLAG_STARTSELECTS,
	NULL,
};

/* server browser (master-server directory + direct per-server queries) */

static s32 g_NetBrowserSelected = 0;
static s32 g_NetDetailsReqTick = 0;
static u32 g_NetBrowserMarquee = 0; // frame counter; scrolls the focused row's long name

static struct menudialogdef g_NetBrowserDialog;
static struct menudialogdef g_NetBrowserActionDialog;
static struct menudialogdef g_NetBrowserDetailsDialog;
static struct menudialogdef g_NetJoinPasswordDialog;
static MenuItemHandlerResult menuhandlerBrowserConnect(s32 operation, struct menuitem *item, union handlerdata *data);
static void netBrowserBeginConnect(void);

// stagenum / scenario are sent as indices; resolve to display names locally
// (the same tables netmsgSvcLobbyStateWrite uses, available while disconnected).
static const char *netBrowserScenarioName(u8 scenario)
{
	static const char *const names[] = {
		"Combat", "Hold the Briefcase", "Hacker Central",
		"Pop-A-Cap", "King of the Hill", "Capture the Case",
		"Graffiti", "Zones", "Race",
	};
	return (scenario < (u8)(sizeof(names) / sizeof(names[0]))) ? names[scenario] : "?";
}

static const char *netBrowserMapName(u8 stagenum)
{
	for (s32 i = 0; i < 17; i++) {
		if (g_MpArenas[i].stagenum == stagenum) {
			return langGet(g_MpArenas[i].name);
		}
	}
	return "?";
}

static const char *menutextBrowserHeader(struct menuitem *item)
{
	static char tmp[96];
	if (g_NetBrowserState == NETBROWSER_ERROR) {
		return "Master server unreachable\n";
	}
	if (g_NetServerCount == 0) {
		return (g_NetBrowserState == NETBROWSER_REQUESTING)
				? "Searching for servers...\n" : "No servers found\n";
	}
	snprintf(tmp, sizeof(tmp), "%d server%s found:\n",
			g_NetServerCount, g_NetServerCount == 1 ? "" : "s");
	return tmp;
}

// Short game-type label for the narrow Type column.
static const char *netBrowserScenarioShort(u8 scenario)
{
	static const char *const names[] = { "DM", "HtB", "HC", "PaC", "KotH", "CtC" };
	return (scenario < (u8)(sizeof(names) / sizeof(names[0]))) ? names[scenario] : "?";
}

// Render one piece of text at (x, y) in the extra-small font. Copies to a local
// buffer so callers can pass string literals (textRenderProjected wants char*).
// Wrap a batch of these between text0f153628 / text0f153780.
static Gfx *netBrowserText(Gfx *gdl, s32 x, s32 y, const char *s, u32 colour)
{
	char buf[64];
	s32 tx = x;
	s32 ty = y;
	snprintf(buf, sizeof(buf), "%s", s);
	return textRenderProjected(gdl, &tx, &ty, buf, g_CharsHandelGothicXs, g_FontHandelGothicXs,
			(s32)colour, viGetWidth(), viGetHeight(), 0, 0);
}

// Build the name-column text. For the focused row, a name longer than the column
// budget scrolls (marquee) via a character ticker driven by g_NetBrowserMarquee;
// other rows are truncated. NET_QF_PASSWORD shows a leading lock.
static void netBrowserRowName(char *out, s32 outsz, const struct netserverentry *e, s32 focused)
{
	char full[80];
	snprintf(full, sizeof(full), "%s%s", (e->flags & NET_QF_PASSWORD) ? "#" : "", e->name);
	const s32 budget = 15;
	const s32 len = (s32)strlen(full);

	if (len <= budget || !focused) {
		snprintf(out, outsz, "%.*s", budget, full);
		return;
	}

	char loop[100];
	snprintf(loop, sizeof(loop), "%s    ", full); // gap before it wraps
	const s32 looplen = (s32)strlen(loop);
	const s32 off = (s32)((g_NetBrowserMarquee / 8u) % (u32)looplen); // ~8 frames/char
	s32 i;
	for (i = 0; i < budget && i < outsz - 1; i++) {
		out[i] = loop[(off + i) % looplen];
	}
	out[i] = '\0';
}

// Shared column geometry so the header titles and the row cells line up. Each
// column has a divider X (where the '|' bar goes) and a text X a few pixels to
// its right, giving breathing room around the bars.
struct netbrowsercols {
	s32 name;
	s32 dplay, play;
	s32 dsim, sim;
	s32 dtype, type;
	s32 dmap, map;
	s32 dping, ping;
};

static void netBrowserCols(struct netbrowsercols *c, s32 x0, s32 w)
{
	const s32 pad = 4; // gap between a divider bar and the text after it
	c->name  = x0 + 2;
	c->dplay = x0 + w * 40 / 100; c->play = c->dplay + pad;
	c->dsim  = x0 + w * 50 / 100; c->sim  = c->dsim + pad;
	c->dtype = x0 + w * 56 / 100; c->type = c->dtype + pad;
	c->dmap  = x0 + w * 72 / 100; c->map  = c->dmap + pad;
	c->dping = x0 + w * 88 / 100; c->ping = c->dping + pad;
}

// Custom render for one server row (MENUOP_RENDER on the LIST item). Columns are
// drawn at fixed pixel X (so they truly align), with faint '|' dividers. The menu
// passes the row index in data->list.unk04 and per-row state in renderdata
// (x/y/width/colour, unk10 = focused). Tracks the focused row for activation.
static Gfx *netBrowserRenderRow(union handlerdata *data)
{
	Gfx *gdl = data->type19.gdl;
	struct menuitemrenderdata *rd = data->type19.renderdata2;
	const s32 row = (s32)data->list.unk04;
	if (!rd || row < 0 || row >= g_NetServerCount) {
		return gdl;
	}
	const struct netserverentry *e = &g_NetServerList[row];
	if (rd->unk10) {
		g_NetBrowserSelected = row; // focused row → action dialog target
	}

	const s32 x0 = rd->x;
	const s32 w = rd->width;
	const s32 y = rd->y;
	const u32 col = rd->colour;
	const u32 divc = 0xa0a0a050; // faint grey dividers

	struct netbrowsercols c;
	netBrowserCols(&c, x0, w);
	char buf[80];

	gdl = text0f153628(gdl);

	gdl = netBrowserText(gdl, c.dplay, y, "|", divc);
	gdl = netBrowserText(gdl, c.dsim,  y, "|", divc);
	gdl = netBrowserText(gdl, c.dtype, y, "|", divc);
	gdl = netBrowserText(gdl, c.dmap,  y, "|", divc);
	gdl = netBrowserText(gdl, c.dping, y, "|", divc);

	netBrowserRowName(buf, sizeof(buf), e, rd->unk10);
	gdl = netBrowserText(gdl, c.name, y, buf, col);

	snprintf(buf, sizeof(buf), "%d/%d", (s32)e->num_clients, (s32)e->max_clients);
	gdl = netBrowserText(gdl, c.play, y, buf, col);

	snprintf(buf, sizeof(buf), "%d", (s32)e->num_sims);
	gdl = netBrowserText(gdl, c.sim, y, buf, col);

	gdl = netBrowserText(gdl, c.type, y, netBrowserScenarioShort(e->scenario), col);

	snprintf(buf, sizeof(buf), "%.7s", netBrowserMapName(e->stagenum));
	gdl = netBrowserText(gdl, c.map, y, buf, col);

	if (e->ping == NET_PING_PENDING) {
		gdl = netBrowserText(gdl, c.ping, y, "--", col);
	} else {
		snprintf(buf, sizeof(buf), "%dms", (s32)e->ping);
		gdl = netBrowserText(gdl, c.ping, y, buf, col);
	}

	gdl = text0f153780(gdl);
	return gdl;
}

// Custom-render label drawing the column titles, aligned to the row columns.
static MenuItemHandlerResult menuhandlerBrowserHeader(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation != MENUOP_RENDER) {
		return 0;
	}
	Gfx *gdl = data->type19.gdl;
	struct menuitemrenderdata *rd = data->type19.renderdata2;
	if (!rd) {
		return (intptr_t)gdl;
	}
	const s32 x0 = rd->x;
	const s32 w = rd->width;
	const s32 y = rd->y;
	const u32 col = 0xc8c8c8c0;
	const u32 divc = 0xa0a0a050;
	struct netbrowsercols c;
	netBrowserCols(&c, x0, w);

	gdl = text0f153628(gdl);
	gdl = netBrowserText(gdl, c.dplay, y, "|", divc);
	gdl = netBrowserText(gdl, c.dsim,  y, "|", divc);
	gdl = netBrowserText(gdl, c.dtype, y, "|", divc);
	gdl = netBrowserText(gdl, c.dmap,  y, "|", divc);
	gdl = netBrowserText(gdl, c.dping, y, "|", divc);
	gdl = netBrowserText(gdl, c.name, y, "Name", col);
	gdl = netBrowserText(gdl, c.play, y, "Plr",  col);
	gdl = netBrowserText(gdl, c.sim,  y, "Sim",  col);
	gdl = netBrowserText(gdl, c.type, y, "Type", col);
	gdl = netBrowserText(gdl, c.map,  y, "Map",  col);
	gdl = netBrowserText(gdl, c.ping, y, "Png",  col);
	gdl = text0f153780(gdl);

	return (intptr_t)gdl;
}

// The server table itself (MENUITEMTYPE_LIST). The menu handles scrolling +
// focus; we provide the row count, height, current selection, per-row render,
// and open the action dialog when a row is activated.
static MenuItemHandlerResult menuhandlerBrowserList(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->list.value = g_NetServerCount;
		break;
	case MENUOP_GETOPTIONHEIGHT:
		data->list.value = 9;
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->list.value = (g_NetBrowserSelected >= 0 && g_NetBrowserSelected < g_NetServerCount)
				? g_NetBrowserSelected : 0;
		break;
	case MENUOP_RENDER:
		return (intptr_t)netBrowserRenderRow(data);
	case MENUOP_SET:
		// g_NetBrowserSelected is kept current by the focused row's render.
		if (g_NetBrowserSelected >= 0 && g_NetBrowserSelected < g_NetServerCount) {
			menuPushDialog(&g_NetBrowserActionDialog);
		}
		break;
	}
	return 0;
}

static MenuItemHandlerResult menuhandlerBrowserRefresh(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_SET) {
		netBrowserRefresh();
	}
	return 0;
}

// Drives the query socket: open/close the standalone UDP socket with the dialog
// and poll it every frame (MENUOP_TICK fires once per frame for the active dialog).
static s32 netBrowserDialogHandler(s32 operation, struct menudialogdef *dialogdef, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_OPEN:
		netBrowserOpen();
		break;
	case MENUOP_TICK:
		netBrowserTick();
		g_NetBrowserMarquee++; // advance the focused-row name scroll
		break;
	case MENUOP_CLOSE:
		netBrowserClose();
		break;
	}
	return 0;
}

static struct menuitem g_NetBrowserMenuItems[] = {
	// status line ("N servers found" / "Searching..." / "Master unreachable")
	{ MENUITEMTYPE_LABEL, 0, MENUITEMFLAG_SMALLFONT, (uintptr_t)&menutextBrowserHeader, 0, NULL },
	{ MENUITEMTYPE_SEPARATOR, 0, 0, 0, 0, NULL },
	// custom-rendered column header (under the line), aligned to the list columns.
	// Needs a (blank) literal text or menuitemLabelRender bails before the custom
	// MENUOP_RENDER call (it early-returns on a null param2/text).
	{ MENUITEMTYPE_LABEL, 0, MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SMALLFONT | MENUITEMFLAG_LIST_CUSTOMRENDER, (uintptr_t)" ", 0, menuhandlerBrowserHeader },
	// the scrollable server table (one custom-rendered row per server). The list
	// keeps the focused row vertically centred, so a shorter area keeps the rows
	// up near the header instead of halfway down the window.
	{
		MENUITEMTYPE_LIST,
		0,
		MENUITEMFLAG_LIST_CUSTOMRENDER,
		0x00000118,
		0x00000030,
		menuhandlerBrowserList,
	},
	{ MENUITEMTYPE_SEPARATOR, 0, 0, 0, 0, NULL },
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Refresh\n",
		0,
		menuhandlerBrowserRefresh,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_CLOSESDIALOG,
		L_OPTIONS_213, // "Back"
		0,
		NULL,
	},
	{ MENUITEMTYPE_END },
};

static struct menudialogdef g_NetBrowserDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Server Browser",
	g_NetBrowserMenuItems,
	netBrowserDialogHandler,
	MENUDIALOGFLAG_LITERAL_TEXT | MENUDIALOGFLAG_STARTSELECTS,
	NULL,
};

/* browser: per-server action menu (Connect / Details / Add Favourite) */

static const char *menutextBrowserActionTitle(struct menuitem *item)
{
	static char tmp[NET_BROWSER_NAME_LEN + 2];
	if (g_NetBrowserSelected < g_NetServerCount) {
		snprintf(tmp, sizeof(tmp), "%s\n", g_NetServerList[g_NetBrowserSelected].name);
		return tmp;
	}
	return "";
}

static MenuItemHandlerResult menuhandlerBrowserDetails(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_SET) {
		menuPushDialog(&g_NetBrowserDetailsDialog);
	}
	return 0;
}

static MenuItemHandlerResult menuhandlerBrowserAddFav(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_SET && g_NetBrowserSelected < g_NetServerCount) {
		const struct netserverentry *e = &g_NetServerList[g_NetBrowserSelected];
		for (s32 i = 0; i < NET_FAV_MAX; i++) {
			if (g_NetFavAddr[i][0] && strcmp(g_NetFavAddr[i], e->addr) == 0) {
				return 0; // already saved
			}
		}
		for (s32 i = 0; i < NET_FAV_MAX; i++) {
			if (!g_NetFavAddr[i][0]) {
				strncpy(g_NetFavAddr[i], e->addr, NET_MAX_ADDR);
				g_NetFavAddr[i][NET_MAX_ADDR] = '\0';
				strncpy(g_NetFavName[i], e->name, NET_FAV_NAME_LEN - 1);
				g_NetFavName[i][NET_FAV_NAME_LEN - 1] = '\0';
				break;
			}
		}
	}
	return 0;
}

static struct menuitem g_NetBrowserActionMenuItems[] = {
	{
		MENUITEMTYPE_LABEL,
		0,
		MENUITEMFLAG_SMALLFONT,
		(uintptr_t)&menutextBrowserActionTitle,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SEPARATOR,
		0, 0, 0, 0, NULL,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Connect\n",
		0,
		menuhandlerBrowserConnect,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Details\n",
		0,
		menuhandlerBrowserDetails,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Add to Favourites\n",
		0,
		menuhandlerBrowserAddFav,
	},
	{
		MENUITEMTYPE_SEPARATOR,
		0, 0, 0, 0, NULL,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_CLOSESDIALOG,
		L_OPTIONS_213, // "Back"
		0,
		NULL,
	},
	{ MENUITEMTYPE_END },
};

static struct menudialogdef g_NetBrowserActionDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Server",
	g_NetBrowserActionMenuItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT | MENUDIALOGFLAG_STARTSELECTS,
	NULL,
};

/* browser: live details (re-queried ~1Hz from the selected server) */

static char *menutextDetailsLine(struct menuitem *item)
{
	static char tmp[160];
	static const char *const s_diff[] = {
		"Meat", "Easy", "Normal", "Hard", "Perfect", "Dark", "?"
	};
	const struct netserverdetails *d = &g_NetServerDetails;
	const u8 idx = item->param;

	if (!d->valid) {
		return (idx == 0) ? "Querying server...\n" : "";
	}

	// 0: server name
	if (idx == 0) {
		snprintf(tmp, sizeof(tmp), "%s\n", d->name[0] ? d->name : "?");
		return tmp;
	}

	// 1: type - map
	if (idx == 1) {
		snprintf(tmp, sizeof(tmp), "%s - %s\n",
				netBrowserScenarioName(d->scenario), netBrowserMapName(d->stagenum));
		return tmp;
	}

	// 2: score / time limits (hidden if all zero)
	if (idx == 2) {
		if (!d->scorelimit && !d->timelimit && !d->teamscorelimit) {
			return "";
		}
		tmp[0] = '\0';
		char seg[48];
		if (d->scorelimit) {
			snprintf(seg, sizeof(seg), "Pts: %d", (s32)d->scorelimit);
			strncat(tmp, seg, sizeof(tmp) - strlen(tmp) - 1);
		}
		if (d->timelimit) {
			if (tmp[0]) { strncat(tmp, "  ", sizeof(tmp) - strlen(tmp) - 1); }
			snprintf(seg, sizeof(seg), "Time: %d:00", (s32)d->timelimit);
			strncat(tmp, seg, sizeof(tmp) - strlen(tmp) - 1);
		}
		if (d->teamscorelimit) {
			if (tmp[0]) { strncat(tmp, "  ", sizeof(tmp) - strlen(tmp) - 1); }
			snprintf(seg, sizeof(seg), "Team Pts: %d", (s32)d->teamscorelimit);
			strncat(tmp, seg, sizeof(tmp) - strlen(tmp) - 1);
		}
		strncat(tmp, "\n", sizeof(tmp) - strlen(tmp) - 1);
		return tmp;
	}

	// 21: server mod + local-compatibility marker (rendered between the
	// limits row and the player list — see the items array order). The join
	// auth rejects a mod-dir mismatch, so warn before a doomed connect;
	// netmaster.c computes modmatch at parse time (netModDirName compare).
	if (idx == 21) {
		if (d->modmatch) {
			if (!d->mod[0]) {
				return ""; // both vanilla — nothing to report
			}
			snprintf(tmp, sizeof(tmp), "Mod: %s\n", d->mod);
		} else {
			snprintf(tmp, sizeof(tmp), "Mod: %s - mismatch, can't join\n",
					d->mod[0] ? d->mod : "none");
		}
		return tmp;
	}

	// 3: "Players (N):"
	if (idx == 3) {
		if (!d->num_players) { return ""; }
		snprintf(tmp, sizeof(tmp), "Players (%d):\n", (s32)d->num_players);
		return tmp;
	}

	// 4-11: player rows
	if (idx >= 4 && idx <= 11) {
		const s32 p = (s32)(idx - 4);
		if (p >= (s32)d->num_players) { return ""; }
		const struct netserverdetailplayer *pl = &d->players[p];
		char ping[12];
		if (pl->ping == 0) {
			snprintf(ping, sizeof(ping), "--");
		} else {
			snprintf(ping, sizeof(ping), "%dms", (s32)pl->ping);
		}
		snprintf(tmp, sizeof(tmp), "  %-12.12s  %d pts  %d d  %s\n",
				pl->name, (s32)pl->score, (s32)pl->deaths, ping);
		return tmp;
	}

	// 12: "Sims (N):"
	if (idx == 12) {
		if (!d->num_sims) { return ""; }
		snprintf(tmp, sizeof(tmp), "Sims (%d):\n", (s32)d->num_sims);
		return tmp;
	}

	// 13-20: sim rows
	if (idx >= 13 && idx <= 20) {
		const s32 s = (s32)(idx - 13);
		if (s >= (s32)d->num_sims) { return ""; }
		const struct netserverdetailsim *si = &d->sims[s];
		const u8 df = (si->difficulty < 6u) ? si->difficulty : 6u;
		snprintf(tmp, sizeof(tmp), "  %-12.12s  %s  %d pts\n",
				si->name, s_diff[df], (s32)si->score);
		return tmp;
	}

	return "";
}

// Keeps the socket pumped while the details dialog is on top of the browser,
// and re-queries the selected server ~1Hz for a live scoreboard.
static s32 netBrowserDetailsDialogHandler(s32 operation, struct menudialogdef *dialogdef, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_OPEN:
		g_NetServerDetails.valid = 0;
		g_NetDetailsReqTick = 0;
		netBrowserQueryDetails(g_NetBrowserSelected);
		break;
	case MENUOP_TICK:
		netBrowserTick();
		if (++g_NetDetailsReqTick >= 60) { // ~1s at 60fps
			g_NetDetailsReqTick = 0;
			netBrowserQueryDetails(g_NetBrowserSelected);
		}
		break;
	}
	return 0;
}

#define DETAILLINE(n) \
	{ MENUITEMTYPE_LABEL, (n), MENUITEMFLAG_SMALLFONT, (uintptr_t)&menutextDetailsLine, 0, NULL }

static struct menuitem g_NetBrowserDetailsMenuItems[] = {
	// Display order is array order; the param (passed to menutextDetailsLine)
	// picks the text. 21 = the mod line, rendered after the limits row.
	DETAILLINE(0),  DETAILLINE(1),  DETAILLINE(2),  DETAILLINE(21),
	DETAILLINE(3),
	DETAILLINE(4),  DETAILLINE(5),  DETAILLINE(6),  DETAILLINE(7),
	DETAILLINE(8),  DETAILLINE(9),  DETAILLINE(10), DETAILLINE(11),
	DETAILLINE(12), DETAILLINE(13), DETAILLINE(14), DETAILLINE(15),
	DETAILLINE(16), DETAILLINE(17), DETAILLINE(18), DETAILLINE(19),
	DETAILLINE(20),
	{ MENUITEMTYPE_SEPARATOR, 0, 0, 0, 0, NULL },
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Connect\n",
		0,
		menuhandlerBrowserConnect,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_CLOSESDIALOG,
		L_OPTIONS_213, // "Back"
		0,
		NULL,
	},
	{ MENUITEMTYPE_END },
};

#undef DETAILLINE

static struct menudialogdef g_NetBrowserDetailsDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Server Details",
	g_NetBrowserDetailsMenuItems,
	netBrowserDetailsDialogHandler,
	MENUDIALOGFLAG_LITERAL_TEXT | MENUDIALOGFLAG_STARTSELECTS,
	NULL,
};

/* browser: join-password prompt (shown before connecting to a passworded server) */

static char *menutextJoinPassword(struct menuitem *item)
{
	static char tmp[NET_MAX_PASSWORD + 3];
	s32 n = (s32)strlen(g_NetJoinPassword);
	if (n > NET_MAX_PASSWORD) { n = NET_MAX_PASSWORD; }
	s32 i;
	for (i = 0; i < n; i++) { tmp[i] = '*'; }
	tmp[i] = '_';
	tmp[i + 1] = '\n';
	tmp[i + 2] = '\0';
	return tmp;
}

static MenuItemHandlerResult menuhandlerEnterJoinPassword(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (!menuIsDialogOpen(&g_NetJoinPasswordDialog)) {
		return 0;
	}
	const s32 r = inputTextHandler(g_NetJoinPassword, NET_MAX_PASSWORD, &g_NetJoinPasswordPtr, false);
	if (r > 0) {
		// Enter — connect with the entered password
		inputStopTextInput();
		netBrowserBeginConnect();
	} else if (r < 0) {
		// ESC — cancel, return to the action menu
		inputStopTextInput();
		menuPopDialog();
	}
	return 0;
}

static struct menuitem g_NetJoinPasswordMenuItems[] = {
	{
		MENUITEMTYPE_LABEL,
		0,
		MENUITEMFLAG_SELECTABLE_CENTRE,
		(uintptr_t)&menutextJoinPassword,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SEPARATOR,
		0, 0, 0, 0, NULL,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_CENTRE | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"ENTER to connect, ESC to cancel\n",
		0,
		menuhandlerEnterJoinPassword,
	},
	{ MENUITEMTYPE_END },
};

static struct menudialogdef g_NetJoinPasswordDialog = {
	MENUDIALOGTYPE_SUCCESS,
	(uintptr_t)"Server Password",
	g_NetJoinPasswordMenuItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT | MENUDIALOGFLAG_IGNOREBACK | MENUDIALOGFLAG_STARTSELECTS,
	NULL,
};

// Connect to the selected server. Closes every browser dialog first (which
// fires g_NetBrowserDialog's CLOSE handler → destroys the query socket), then
// starts the client and shows the Joining dialog.
static void netBrowserBeginConnect(void)
{
	while (menuIsDialogOpen(&g_NetJoinPasswordDialog)
			|| menuIsDialogOpen(&g_NetBrowserDetailsDialog)
			|| menuIsDialogOpen(&g_NetBrowserActionDialog)
			|| menuIsDialogOpen(&g_NetBrowserDialog)) {
		menuPopDialog();
	}
	if (netStartClient(g_NetJoinAddr) == 0) {
		menuPushDialog(&g_NetJoiningDialog);
	}
}

static MenuItemHandlerResult menuhandlerBrowserConnect(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation != MENUOP_SET || g_NetBrowserSelected >= g_NetServerCount) {
		return 0;
	}
	const struct netserverentry *e = &g_NetServerList[g_NetBrowserSelected];
	strncpy(g_NetJoinAddr, e->addr, NET_MAX_ADDR);
	g_NetJoinAddr[NET_MAX_ADDR] = '\0';
	g_NetJoinAddrPtr = (s32)strlen(g_NetJoinAddr);

	if (e->flags & NET_QF_PASSWORD) {
		// prompt for the password, then connect from the prompt handler
		g_NetJoinPassword[0] = '\0';
		g_NetJoinPasswordPtr = 0;
		inputClearLastKey();
		inputClearLastTextChar();
		inputStartTextInput();
		menuPushDialog(&g_NetJoinPasswordDialog);
	} else {
		g_NetJoinPassword[0] = '\0';
		netBrowserBeginConnect();
	}
	return 0;
}

static MenuItemHandlerResult menuhandlerServerBrowser(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_SET) {
		menuPushDialog(&g_NetBrowserDialog);
	}
	return 0;
}

/* main */

MenuItemHandlerResult menuhandlerHostGame(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_SET) {
		g_NetMenuPort = g_NetServerPort;
		g_NetMenuMaxPlayers = g_NetMaxClients;
		menuPushDialog(&g_NetHostMenuDialog);
	}

	return 0;
}

MenuItemHandlerResult menuhandlerJoinGame(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_SET) {
		if (g_NetJoinAddr[0] == '\0') {
			strncpy(g_NetJoinAddr, g_NetLastJoinAddr, NET_MAX_ADDR);
			g_NetJoinAddrPtr = strlen(g_NetJoinAddr);
		}
		menuPushDialog(&g_NetJoinMenuDialog);
	}

	return 0;
}

// ---- Cooperative (campaign co-op) online lobby — F0 (docs/PORT_COOP_ONLINE.md) ----
// Host-side front-end that replaces the /coop console command: pick mission +
// difficulty + mutators, Start Hosting (server/lobby), then Launch Mission for
// everyone via netCoopEnterStage. Clients join through the normal Join Game /
// Server Browser; the host's SVC_STAGE_START (NETSTAGEMODE_COOP) pulls them into
// the same stage. The mutator selections (Lives, Body Type) and the profile import
// are stored/stubbed here now; their gameplay/render wiring lands in F1/F2/F3.
static s32 g_NetCoopMenuStageIdx = SOLOSTAGEINDEX_DEFECTION;
static s32 g_NetCoopMenuDiff = DIFF_A;
// Lives mode + count live in net globals g_NetCoopLivesMode / g_NetCoopLivesCount
// (net.h) so they sync to clients in SVC_STAGE_START (F3).
// Body type lives in the net global g_NetCoopBodyMode (net.h) so it can be
// resolved + synced at stage start (F2). COOPBODY_FEMININE/MASCULINE/RANDOM.

static MenuItemHandlerResult menuhandlerNetCoopStage(s32 operation, struct menuitem *item, union handlerdata *data)
{
	// Host-only setting: greyed out (disabled) for clients; the host owns these.
	if (operation == MENUOP_CHECKDISABLED) {
		return (g_NetMode == NETMODE_CLIENT) ? 1 : 0;
	}

	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = NUM_SOLOSTAGES;
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t)langGet(g_SoloStages[data->dropdown.value].name3);
	case MENUOP_SET:
		g_NetCoopMenuStageIdx = (s32)data->checkbox.value;
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = (g_NetCoopMenuStageIdx >= 0 && g_NetCoopMenuStageIdx < NUM_SOLOSTAGES) ? g_NetCoopMenuStageIdx : 0;
		break;
	}
	return 0;
}

static MenuItemHandlerResult menuhandlerNetCoopDifficulty(s32 operation, struct menuitem *item, union handlerdata *data)
{
	// Host-only setting: greyed out (disabled) for clients; the host owns these.
	if (operation == MENUOP_CHECKDISABLED) {
		return (g_NetMode == NETMODE_CLIENT) ? 1 : 0;
	}

	static const char *const opts[] = { "Agent", "Special Agent", "Perfect Agent", "Perfect Dark" };
	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = sizeof(opts) / sizeof(opts[0]);
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t)opts[data->dropdown.value];
	case MENUOP_SET:
		g_NetCoopMenuDiff = (s32)data->checkbox.value;
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = (g_NetCoopMenuDiff >= DIFF_A && g_NetCoopMenuDiff <= DIFF_PD) ? g_NetCoopMenuDiff : DIFF_A;
		break;
	}
	return 0;
}

static MenuItemHandlerResult menuhandlerNetCoopLives(s32 operation, struct menuitem *item, union handlerdata *data)
{
	// Host-only setting: greyed out (disabled) for clients; the host owns these.
	if (operation == MENUOP_CHECKDISABLED) {
		return (g_NetMode == NETMODE_CLIENT) ? 1 : 0;
	}

	// F3 host mutator. Off = stock steal-half-a-buddy's-health revive; Per Player /
	// Shared Pool replace it with a respawn budget. Synced in SVC_STAGE_START.
	static const char *const opts[] = { "Off (Steal Health)", "Per Player", "Shared Pool" };
	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = sizeof(opts) / sizeof(opts[0]);
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t)opts[data->dropdown.value];
	case MENUOP_SET:
		g_NetCoopLivesMode = (s32)data->checkbox.value;
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = (g_NetCoopLivesMode >= COOP_LIVES_OFF && g_NetCoopLivesMode <= COOP_LIVES_SHARED) ? g_NetCoopLivesMode : COOP_LIVES_OFF;
		break;
	}
	return 0;
}

static MenuItemHandlerResult menuhandlerNetCoopLivesCount(s32 operation, struct menuitem *item, union handlerdata *data)
{
	// Host-only setting: greyed out (disabled) for clients; the host owns these.
	if (operation == MENUOP_CHECKDISABLED) {
		return (g_NetMode == NETMODE_CLIENT) ? 1 : 0;
	}

	// F3: lives granted per player (Per Player) or, scaled by player count, the
	// shared pool (Shared Pool). Options 1..9. Only meaningful when Lives != Off.
	static const char *const opts[] = { "1", "2", "3", "4", "5", "6", "7", "8", "9" };
	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = sizeof(opts) / sizeof(opts[0]);
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t)opts[data->dropdown.value];
	case MENUOP_SET:
		g_NetCoopLivesCount = (s32)data->checkbox.value + 1;
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = (g_NetCoopLivesCount >= 1 && g_NetCoopLivesCount <= 9) ? g_NetCoopLivesCount - 1 : 2;
		break;
	}
	return 0;
}

static MenuItemHandlerResult menuhandlerNetCoopBody(s32 operation, struct menuitem *item, union handlerdata *data)
{
	// F2: PER-PLAYER body-type choice. Stored in g_NetCoopBodyMode and pushed to
	// the host via CLC_SETTINGS (netClientSettingsChanged), where it is resolved
	// into the synced per-player bitmask at stage start. "Masculine" falls back to
	// the feminine model until per-outfit masculine art exists. The head is always
	// the player's Combat Sim profile head regardless of this choice.
	static const char *const opts[] = { "Feminine", "Masculine", "Random" };
	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = sizeof(opts) / sizeof(opts[0]);
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t)opts[data->dropdown.value];
	case MENUOP_SET:
		g_NetCoopBodyMode = (s32)data->checkbox.value;
		netClientSettingsChanged(); // push the choice to the host (no-op when hosting)
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = (g_NetCoopBodyMode >= COOPBODY_FEMININE && g_NetCoopBodyMode <= COOPBODY_RANDOM) ? g_NetCoopBodyMode : COOPBODY_FEMININE;
		break;
	}
	return 0;
}

static MenuItemHandlerResult menuhandlerNetCoopImportProfile(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_SET) {
		// F1 (todo): copy the active Combat Sim profile's head/body/name into the
		// local co-op identity and the SVC_STAGE_START co-op manifest.
		sysLogPrintf(LOG_CHAT, "NET: co-op profile import not yet implemented");
	}
	return 0;
}

static MenuItemHandlerResult menuhandlerNetCoopStartHosting(s32 operation, struct menuitem *item, union handlerdata *data)
{
	// Host-only setting: greyed out (disabled) for clients; the host owns these.
	if (operation == MENUOP_CHECKDISABLED) {
		return (g_NetMode == NETMODE_CLIENT) ? 1 : 0;
	}

	if (operation == MENUOP_SET) {
		if (g_NetMode == NETMODE_SERVER) {
			g_NetCoopHosting = 1; // mark co-op so joining clients show the co-op window
			sysLogPrintf(LOG_CHAT, "NET: already hosting — configure, then Launch Mission");
		} else if (netStartServer(g_NetServerPort, g_NetMaxClients) == 0) {
			g_NetCoopHosting = 1; // advertised as netlobbystate.iscoop in SVC_LOBBY_STATE
			sysLogPrintf(LOG_CHAT, "NET: co-op server started — waiting for players, then Launch Mission");
		} else {
			sysLogPrintf(LOG_CHAT, "NET: failed to start co-op server");
		}
	}
	return 0;
}

static MenuItemHandlerResult menuhandlerNetCoopLaunch(s32 operation, struct menuitem *item, union handlerdata *data)
{
	// Host-only setting: greyed out (disabled) for clients; the host owns these.
	if (operation == MENUOP_CHECKDISABLED) {
		return (g_NetMode == NETMODE_CLIENT) ? 1 : 0;
	}

	if (operation == MENUOP_SET) {
		if (g_NetMode != NETMODE_SERVER) {
			sysLogPrintf(LOG_CHAT, "NET: Start Hosting first");
			return 0;
		}
		s32 idx = (g_NetCoopMenuStageIdx >= 0 && g_NetCoopMenuStageIdx < NUM_SOLOSTAGES) ? g_NetCoopMenuStageIdx : SOLOSTAGEINDEX_DEFECTION;
		g_MissionConfig.stageindex = idx;
		// Same entry point as the /coop command; N = g_NetNumClients (host + all
		// connected partners). See netCoopEnterStage / PORT_COOP_8P.md Bucket C.
		sysLogPrintf(LOG_CHAT, "NET: launching co-op (stage %d, difficulty %d, %d players)", idx, g_NetCoopMenuDiff, g_NetNumClients);
		netCoopEnterStage((s32)g_SoloStages[idx].stagenum, g_NetCoopMenuDiff, g_NetNumClients);
	}
	return 0;
}

// Host setup: mission + difficulty + mutators + Start Hosting / Launch Mission.
static struct menuitem g_NetCoopHostMenuItems[] = {
	// Per-player customisation — enabled for everyone (host AND each client).
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Import Combat Sim Profile\n", 0, menuhandlerNetCoopImportProfile },
	{ MENUITEMTYPE_DROPDOWN, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"My Body Type", 0, menuhandlerNetCoopBody },
	{ MENUITEMTYPE_SEPARATOR, 0, 0, 0, 0, NULL },
	// Host-only match settings — greyed out for clients (the handlers return
	// MENUOP_CHECKDISABLED when g_NetMode == NETMODE_CLIENT).
	{ MENUITEMTYPE_DROPDOWN, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Mission", 0, menuhandlerNetCoopStage },
	{ MENUITEMTYPE_DROPDOWN, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Difficulty", 0, menuhandlerNetCoopDifficulty },
	{ MENUITEMTYPE_SEPARATOR, 0, 0, 0, 0, NULL },
	{ MENUITEMTYPE_DROPDOWN, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Lives", 0, menuhandlerNetCoopLives },
	{ MENUITEMTYPE_DROPDOWN, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Lives Count", 0, menuhandlerNetCoopLivesCount },
	{ MENUITEMTYPE_SEPARATOR, 0, 0, 0, 0, NULL },
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Start Hosting\n", 0, menuhandlerNetCoopStartHosting },
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Launch Mission\n", 0, menuhandlerNetCoopLaunch },
	{ MENUITEMTYPE_SEPARATOR, 0, 0, 0, 0, NULL },
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_SELECTABLE_CLOSESDIALOG | MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Back\n", 0, NULL },
	{ MENUITEMTYPE_END },
};

static struct menudialogdef g_NetCoopHostMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Co-op Match Setup",
	g_NetCoopHostMenuItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT | MENUDIALOGFLAG_STARTSELECTS,
	NULL,
};

// Co-Operative -> Online hub: Match Setup / Join / Server Browser, mirroring the
// Combat Sim network menu (g_NetMenuItems) but kept SEPARATE from it. Join /
// Browser reuse the mode-agnostic handlers (joining is identical; the host's
// SVC_STAGE_START NETSTAGEMODE_COOP is what selects co-op). Non-static dialog:
// referenced from the main-menu "Co-Operative -> Online" entry
// (src/game/mainmenu.c g_CoopModeMenuItems).
static struct menuitem g_NetCoopMenuItems[] = {
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_SELECTABLE_OPENSDIALOG | MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Match Setup\n", 0, (void *)&g_NetCoopHostMenuDialog },
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Join Game\n", 0, menuhandlerJoinGame },
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Server Browser\n", 0, menuhandlerServerBrowser },
	{ MENUITEMTYPE_SEPARATOR, 0, 0, 0, 0, NULL },
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_SELECTABLE_CLOSESDIALOG | MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Back\n", 0, NULL },
	{ MENUITEMTYPE_END },
};

struct menudialogdef g_NetCoopMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Cooperative",
	g_NetCoopMenuItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT | MENUDIALOGFLAG_STARTSELECTS,
	NULL,
};

struct menuitem g_NetMenuItems[] = {
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Host Game\n",
		0,
		menuhandlerHostGame,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Host Online Game\n",
		0,
		menuhandlerHostOnlineGame,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Join Game\n",
		0,
		menuhandlerJoinGame,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Server Browser\n",
		0,
		menuhandlerServerBrowser,
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
		L_OPTIONS_213, // "Back"
		0,
		NULL,
	},
	{ MENUITEMTYPE_END },
};

struct menudialogdef g_NetMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Network Game",
	g_NetMenuItems,
	NULL,
	MENUDIALOGFLAG_MPLOCKABLE | MENUDIALOGFLAG_LITERAL_TEXT | MENUDIALOGFLAG_STARTSELECTS,
	NULL,
};

struct menudialogdef g_NetJoinPlayerSetupMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	L_MPMENU_028, // "Player Setup"
	g_MpPlayerSetup234MenuItems,
	NULL,
	MENUDIALOGFLAG_DROPOUTONCLOSE | MENUDIALOGFLAG_STARTSELECTS,
	NULL,
};
