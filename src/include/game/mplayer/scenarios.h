#ifndef _IN_GAME_MPLAYER_SCENARIOS_H
#define _IN_GAME_MPLAYER_SCENARIOS_H
#include <ultra64.h>
#include "data.h"
#include "types.h"

extern struct menudialogdef g_MpScenarioMenuDialog;
extern struct menudialogdef g_MpQuickTeamScenarioMenuDialog;

struct mpscenariooverview {
	u16 name;
	u16 shortname;
	u8 requirefeature;
	u8 teamonly;
};

#ifndef PLATFORM_N64
extern struct mpscenariooverview g_MpScenarioOverviews[9]; // +3 port-only: Graffiti, Zones, Race
#else
extern struct mpscenariooverview g_MpScenarioOverviews[6];
#endif

#ifndef PLATFORM_N64
// Last raw 32-bit scenario save slot consumed by scenarioReadSave's default
// (no-readsavefunc) branch. mpsetupfileLoadWad re-dispatches it when the v10
// scenario high-bit reveals the real scenario (8+) after the slot was already
// consumed under the masked low-3-bits id (see constants.h MPSCENARIO note).
extern u32 g_ScenarioSaveSlotRaw;
void raceApplySaveSlot(u32 val);
#endif

#ifndef PLATFORM_N64
// Global Lives system (elimination.inc — scenario-independent; see
// docs/PORT_ELIMINATION.md). Menu handlers for the Limits dialog (setup.c)
// and the per-match reset hooked from scenarioInitProps.
MenuItemHandlerResult menuhandlerMpElimLivesMode(s32 operation, struct menuitem *item, union handlerdata *data);
MenuItemHandlerResult menuhandlerMpElimLives(s32 operation, struct menuitem *item, union handlerdata *data);
void elimReset(void);
void elimTick(void);
Gfx *elimRenderHud(Gfx *gdl);
#endif

MenuItemHandlerResult menuhandlerMpOpenOptions(s32 operation, struct menuitem *item, union handlerdata *data);
void scenarioReadSave(struct savebuffer *buffer, u8 version);
void scenarioWriteSave(struct savebuffer *buffer);
void scenarioInit(void);
s32 scenarioNumProps(void);
void scenarioInitProps(void);
void scenarioTick(void);
void scenarioTickChr(struct chrdata *chr);
Gfx *scenarioRadarExtra(Gfx *gdl);
bool scenarioRadarChr(Gfx **gdl, struct prop *prop);
f32 scenarioChooseSpawnLocation(f32 chrradius, struct coord *pos, RoomNum *rooms, struct prop *prop);
s32 scenarioGetMaxTeams(void);
void scenarioHighlightRoom(RoomNum room, s32 *arg1, s32 *arg2, s32 *arg3);

#endif
