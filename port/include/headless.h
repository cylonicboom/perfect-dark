#ifndef _IN_HEADLESS_H
#define _IN_HEADLESS_H

#include <PR/ultratypes.h>

// Wall-clock tick pacer for headless dedicated server. Normally videoEndFrame's
// vsync sleep paces the main loop at the display refresh rate; in headless mode
// there is no swap, so without a pacer mainTick burns one CPU core at full
// speed. headlessPace() sleeps until target_hz dictates the next tick, using
// SDL_Delay for the bulk wait and a brief spin for sub-ms precision. Call once
// per mainTick when g_NetDedicatedMode == 1.
//
// target_hz of 60 matches the game's nominal tick rate (g_NetTick advances at
// 60Hz). A lower rate makes the server lag behind clients; a higher rate burns
// CPU for no benefit since game logic is gated by diffframe60.
void headlessPace(s32 target_hz);

#endif
