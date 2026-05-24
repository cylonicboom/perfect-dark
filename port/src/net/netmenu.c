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
#include "config.h"
#include "mpsetups.h"
#include "net/net.h"

extern MenuItemHandlerResult menuhandlerMainMenuCombatSimulator(s32 operation, struct menuitem *item, union handlerdata *data);
extern MenuItemHandlerResult menuhandlerMpAdvancedSetup(s32 operation, struct menuitem *item, union handlerdata *data);
extern struct menuitem g_MpPlayerSetup234MenuItems[];
extern struct menudialogdef g_NetJoinPlayerSetupMenuDialog;

static s32 g_NetMenuMaxPlayers = NET_MAX_CLIENTS;
static s32 g_NetMenuPort = NET_DEFAULT_PORT;
static char g_NetJoinAddr[NET_MAX_ADDR + 1];
static s32 g_NetJoinAddrPtr = 0;

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

static MenuItemHandlerResult menuhandlerHostPort(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_SET) {

	}

	return 0;
}

static char *menuhandlerHostPortValue(struct menuitem *item)
{
	static char tmp[16];
	snprintf(tmp, sizeof(tmp), "%u\n", g_NetMenuPort);
	return tmp;
}

MenuItemHandlerResult menuhandlerHostStart(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_SET) {
		if (netStartServer(g_NetMenuPort, g_NetMenuMaxPlayers) == 0) {
			// load the setup file when entering the Combat Simulator
			mpsetupCopyAllFromPak();
			mpsetupLoadCurrentFile();
			menuhandlerMainMenuCombatSimulator(MENUOP_SET, NULL, NULL);
			menuhandlerMpAdvancedSetup(MENUOP_SET, NULL, NULL);
		}
	}

	return 0;
}

struct menuitem g_NetHostMenuItems[] = {
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
		(uintptr_t)"Port\n",
		(uintptr_t)&menuhandlerHostPortValue,
		menuhandlerHostPort,
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
		static const struct { u32 flag; const char *name; } s_opts[] = {
			{ MPOPTION_ONEHITKILLS,       "One Hit Kills"    },
			{ MPOPTION_FASTMOVEMENT,      "Fast Movement"    },
			{ MPOPTION_SLOWMOTION_ON,     "Slow Motion"      },
			{ MPOPTION_NORADAR,           "No Radar"         },
			{ MPOPTION_NOAUTOAIM,         "No Auto-Aim"      },
			{ MPOPTION_NOPLAYERHIGHLIGHT, "No Highlight"     },
			{ MPOPTION_FRIENDLYFIRE,      "Friendly Fire"    },
			{ MPOPTION_SPAWNWITHWEAPON,   "Start w/Weapon"   },
			{ MPOPTION_KILLSSCORE,        "Kills Score"      },
			{ MPOPTION_NODRUGBLUR,        "No Drug Blur"     },
			{ MPOPTION_CONTROLLERS_ONLY,  "Controllers Only" },
			{ MPOPTION_NOCULL,            "No Room Culling"  },
			{ MPOPTION_NOOMLIMIT,         "No Draw Limit"    },
			{ MPOPTION_TEAMSENABLED,      "Teams"            },
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
		if (teams_on && cl->team < MAX_TEAMS) {
			snprintf(tmp, sizeof(tmp), "%s%s  %s  %s\n",
					is_me ? "* " : "  ",
					cl->name, ping, ls->teamnames[cl->team]);
		} else {
			snprintf(tmp, sizeof(tmp), "%s%s  %s\n",
					is_me ? "* " : "  ",
					cl->name, ping);
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

static MenuItemHandlerResult menuhandlerJoining(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (inputKeyPressed(VK_ESCAPE)) {
		netDisconnect();
		menuPopDialog();
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
	NULL,
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
		(uintptr_t)"Join Game\n",
		0,
		menuhandlerJoinGame,
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
