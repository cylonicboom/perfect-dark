#ifndef IN_GAME_CHEATS_H
#define IN_GAME_CHEATS_H
#include <ultra64.h>
#include "data.h"
#include "types.h"

extern struct menudialogdef g_CheatsMenuDialog;
extern struct menudialogdef g_CheatsFunMenuDialog;
extern struct menudialogdef g_CheatsGameplayMenuDialog;
extern struct menudialogdef g_CheatsSoloWeaponsMenuDialog;
extern struct menudialogdef g_CheatsClassicWeaponsMenuDialog;
extern struct menudialogdef g_CheatsWeaponsMenuDialog;
extern struct menudialogdef g_CheatsBuddiesMenuDialog;

u32 cheatIsUnlocked(s32 cheat_id);
bool cheatIsActive(s32 cheat_id);
void cheatActivate(s32 cheat_id);
void cheatDeactivate(s32 cheat_id);
void cheatsInit(void);
void cheatsReset(void);
char *cheatGetNameIfUnlocked(struct menuitem *item);
char *cheatGetMarquee(struct menuitem *item);
s32 cheatGetByTimedStageIndex(s32 stage_index, s32 difficulty);
s32 cheatGetByCompletedStageIndex(s32 stage_index);
s32 cheatGetTime(s32 cheat_id);
char *cheatGetName(s32 cheat_id);
MenuDialogHandlerResult cheatMenuHandleDialog(s32 operation, struct menudialogdef *dialogdef, union handlerdata *data);
MenuItemHandlerResult cheatCheckboxMenuHandler(s32 operation, struct menuitem *item, union handlerdata *data);
MenuItemHandlerResult cheatMenuHandleBuddyCheckbox(s32 operation, struct menuitem *item, union handlerdata *data);
MenuItemHandlerResult cheatMenuHandleTurnOffAllCheats(s32 operation, struct menuitem *item, union handlerdata *data);

#ifndef PLATFORM_N64
// Confirm dialog whose "Unlock" option calls gamefileUnlockEverything().
// Referenced by the Extended Options > Experiments menu (optionsmenu.c).
extern struct menudialogdef g_CheatsConfirmUnlockMenuDialog;

// Returns true if GoldenEye Style behaviour should be active right now:
// either the Combat Sim MP option (`MPOPTION_GOLDENEYE`) is set in an
// active match, or the gameplay cheat `CHEAT_GOLDENEYE` is enabled. The
// cheat path works in any mode (single-player, training, etc.).
bool goldeneyeStyleActive(void);

// Per-behaviour Classic Options gate: true when the GoldenEye Style master
// (cheat or MP option) OR the behaviour's own cheat / MP option bit is
// active. Every former goldeneyeStyleActive() gate site routes through this
// with its CHEAT_CLASSIC_* / MPOPTION_CLASSIC_* pair.
bool classicOptionActive(s32 cheat_id, u64 mpoption);

// Wireframe cheat (CHEAT_WIREFRAME) backdrop colour, RGB. Set via the
// `/wireframe RRGGBB` console command; defaults to black. Read by sky.c to
// recolour the sky / clouds / water backdrop while the cheat is active.
extern u8 g_WireframeBgColour[3];
#endif

#endif
