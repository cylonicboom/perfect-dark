/**
 * Combat
 *
 * This is a general combat scenario.
 *
 * There are no callback functions or special logic.
 */

extern struct menudialogdef g_ExtGameOptionsMenuDialog;

struct menuitem g_MpCombatOptionsMenuItems[] = {
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
#ifndef PLATFORM_N64
	{
		MENUITEMTYPE_CHECKBOX,
		0,
		MENUITEMFLAG_LOCKABLEMINOR | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Start Armed",
		MPOPTION_SPAWNWITHWEAPON,
		menuhandlerMpCheckboxOption,
	},
	{
		MENUITEMTYPE_CHECKBOX,
		0,
		MENUITEMFLAG_LOCKABLEMINOR | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"No Drug Blur",
		MPOPTION_NODRUGBLUR,
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
		MENUITEMTYPE_CHECKBOX,
		0,
		MENUITEMFLAG_LOCKABLEMINOR | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"No Player on Radar",
		MPOPTION_NOPLAYERONRADAR,
		menuhandlerMpCheckboxOption,
	},
	{
		// Forces controller-only input for every connected machine. The
		// option lives in g_MpSetup.options so it ships in SVC_STAGE_START
		// — every client honours the host's setting via the gate in the
		// input layer. Useful for "fair" lobbies that want to rule out
		// mouse-aim and instant keyboard strafes.
		MENUITEMTYPE_CHECKBOX,
		0,
		MENUITEMFLAG_LOCKABLEMINOR | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Controllers Only",
		MPOPTION_CONTROLLERS_ONLY,
		menuhandlerMpCheckboxOption,
	},
	{
		// Bypasses portal-based room culling: every room is marked onscreen
		// every frame so nothing is hidden by the visibility graph. Fixes
		// cases where a gap or window lets you see into a room the portal
		// graph would normally cull. Costs extra draw calls on large maps.
		MENUITEMTYPE_CHECKBOX,
		0,
		MENUITEMFLAG_LOCKABLEMINOR | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"No Room Culling",
		MPOPTION_NOCULL,
		menuhandlerMpCheckboxOption,
	},
	{
		// Removes the 60-room draw-slot cap so every room above slot 59 gets its
		// own dedicated draw call instead of being merged into slot 59's scissor
		// region. Only meaningful with No Room Culling enabled (the portal graph
		// naturally keeps visible room counts well under 60 otherwise).
		MENUITEMTYPE_CHECKBOX,
		0,
		MENUITEMFLAG_LOCKABLEMINOR | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"No Draw Slot Limit",
		MPOPTION_NOOMLIMIT,
		menuhandlerMpCheckboxOption,
	},
	{
		// GoldenEye-style combat: snap lean (no interpolation), no
		// crouch accuracy bonus, lower-and-raise reload for every
		// weapon (skip first-person reload anim entirely), and
		// invisible-wall ledges. Applies to human players only;
		// sims/bots are unaffected.
		MENUITEMTYPE_CHECKBOX,
		0,
		MENUITEMFLAG_LOCKABLEMINOR | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"GoldenEye Style",
		MPOPTION_GOLDENEYE,
		menuhandlerMpCheckboxOption,
	},
#endif
	{
		MENUITEMTYPE_SEPARATOR,
		0,
		0,
		0,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_CHECKBOX,
		0,
		MENUITEMFLAG_LOCKABLEMINOR,
		L_MPMENU_228, // "No Player Highlight"
		MPOPTION_NOPLAYERHIGHLIGHT,
		menuhandlerMpCheckboxOption,
	},
	{
		MENUITEMTYPE_CHECKBOX,
		0,
		MENUITEMFLAG_LOCKABLEMINOR,
		L_MPMENU_229, // "No Pickup Highlight"
		MPOPTION_NOPICKUPHIGHLIGHT,
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
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_CLOSESDIALOG,
		L_MPMENU_239, // "Back"
		0,
		NULL,
	},
	{ MENUITEMTYPE_END },
};

struct menudialogdef g_MpCombatOptionsMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	L_MPMENU_215, // "Combat Options"
	g_MpCombatOptionsMenuItems,
	mpOptionsMenuDialog,
	MENUDIALOGFLAG_MPLOCKABLE,
	&g_ExtGameOptionsMenuDialog,
};
