#ifndef _IN_GAME_LV_H
#define _IN_GAME_LV_H
#include <ultra64.h>
#include "data.h"
#include "types.h"

#ifdef PLATFORM_N64
// game runs at ~30, so slomo = 1/2 of 30fps
#define LV_SLOMO_TICK_CAP 4
#define LV_SLOMO_TICK_RATE LV_SLOMO_TICK_CAP
#else
// game runs at 60+, so slomo = 1/2 of whatever framerate we're running at.
// The old CAP=1 + plain integer halving quantized away at high framerates:
// above ~200fps the per-frame delta is mostly 1, the > test never fired, and
// slow motion / combat boost did nothing (while still working in heavier,
// lower-fps scenes — hence "works in campaign, not in Combat Sim").
// lvSlomoScaleTick carries a remainder across frames so the AVERAGE rate is
// exactly half at any fps; the occasional zero-tick frame this produces is
// handled like a paused frame (and lvUpdateMiscSfx is gated so looping sfx
// don't stutter through them).
#define LV_SLOMO_TICK_CAP 0 // engage on any nonzero tick
#define LV_SLOMO_TICK_RATE lvSlomoScaleTick(g_Vars.lvupdate240)
s32 lvSlomoScaleTick(s32 ticks);
// SLOWMOTION_SMART proximity radius for NET games (world units). The vanilla
// test ("is player A's room on player B's screen") needs per-player render
// traversals that don't exist for remote players on the server, so net games
// use a plain player-to-player distance test instead (lvTick).
#define LV_SMART_SLOMO_RANGE 1500.0f
// True when lvTick decided this frame runs at half speed (slow-motion option
// or combat boost). detPinTimestep halves the pinned fixed-tick / netplay
// step from it; on net clients it's wire-driven via SVC_TIMESCALE.
extern s32 g_LvSlomoEngaged;
#endif

u32 getVar80084040(void);
void lvInit(void);
void lvResetMiscSfx(void);
s32 lvGetMiscSfxIndex(u32 arg0);
void lvSetMiscSfxState(u32 type, bool play);
void lvUpdateMiscSfx(void);
void lvReset(s32 stagenum);
Gfx *lvRenderFade(Gfx *gdl);
void lvFadeReset(void);
bool lvUpdateTrackedProp(struct trackedprop *trackedprop, s32 index);
void lvFindThreatsForProp(struct prop *prop, bool inchild, struct coord *playerpos, s32 *activeslots, f32 *param_5);
void func0f168f24(struct prop *prop, bool inchild, struct coord *playerpos, s32 *activeslots, f32 *distances);
void lvFindThreats(void);
Gfx *lvRender(Gfx *gdl);
void lvUpdateSoloHandicaps(void);
s32 sub54321(s32 value);
void lvUpdateCutsceneTime(void);
s32 lvGetSlowMotionType(void);
void lvTick(void);
void lvTickPlayer(void);
void lvCheckPauseStateChanged(void);
void lvSetPaused(bool paused);
void lvConfigureFade(u32 color, s16 num_frames);
bool lvIsFadeActive(void);
void lvStop(void);
bool lvIsPaused(void);
s32 lvGetDifficulty(void);
void lvSetDifficulty(s32 difficulty);
void lvSetMpTimeLimit60(u32 limit);
void lvSetMpScoreLimit(u32 limit);
void lvSetMpTeamScoreLimit(u32 limit);
f32 lvGetStageTimeInSeconds(void);
s32 lvGetStageTime60(void);

#endif
