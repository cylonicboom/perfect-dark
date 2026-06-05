#ifndef IN_GAME_ACTIVEMENU_H
#define IN_GAME_ACTIVEMENU_H
#include <ultra64.h>
#include "data.h"
#include "types.h"

#ifndef PLATFORM_N64
// Port: "Command All Simulants" gets a dedicated active-menu screen at index
// 2 — the FIRST sim screen, before the per-simulant screens, which shift up
// to start at AM_SCREEN_BUDDY0 (3). Standing on it asserts the same allbots
// flag the hold-R modal uses (reasserted per tick in amTick), so every
// apply/render path — including the netplay CLC_BOT_CMD forwarding — works
// unchanged, and amRenderAibotInfo's existing allbots branch supplies the
// "All Simulants" title. On N64 the per-simulant screens start at 2 and "all"
// exists only as the hold-R modal; the macro expands to the original constant
// so the N64 build is byte-identical.
#define AM_SCREEN_ALL    2
#define AM_SCREEN_BUDDY0 3
#else
#define AM_SCREEN_BUDDY0 2
#endif

void amTick(void);

void amOpenPickTarget(void);
MenuDialogHandlerResult menudialog000fcd48(s32 operation, struct menudialogdef *dialogdef, union handlerdata *data);
MenuDialogHandlerResult amPickTargetMenuDialog(s32 operation, struct menudialogdef *dialogdef, union handlerdata *data);
void amSetAiBuddyTemperament(bool aggressive);
void amSetAiBuddyStealth(void);
s32 amGetFirstBuddyIndex(void);
void amApply(s32 slot);
void amGetSlotDetails(s32 slot, u32 *flags, char *label);
void amReset(void);
s16 amCalculateSlotWidth(void);
void amChangeScreen(s32 step);
void amAssignWeaponSlots(void);
void amOpen(void);
void amClose(void);
bool amIsCramped(void);
void amCalculateSlotPosition(s16 column, s16 row, s16 *x, s16 *y);
Gfx *amRenderText(Gfx *gdl, char *text, u32 colour, s16 left, s16 top);
Gfx *amRenderAibotInfo(Gfx *gdl, s32 buddynum);
Gfx *amRenderSlot(Gfx *gdl, char *text, s16 x, s16 y, s32 mode, s32 flags);
Gfx *amRender(Gfx *gdl);

#endif
