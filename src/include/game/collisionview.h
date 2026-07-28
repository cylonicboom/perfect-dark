#ifndef _IN_GAME_COLLISIONVIEW_H
#define _IN_GAME_COLLISIONVIEW_H

#include <ultra64.h>
#include "types.h"

#ifndef PLATFORM_N64

/**
 * Collision View — port-only debug visualiser. See docs/PORT_COLLISION_VIEW.md.
 *
 * Every toggle is a plain s32 (never `bool`): optionsmenu.c and net.c are PORT
 * translation units while this file is a GAME one, and `bool` is a different
 * width on the two sides of that seam (the VR bring-up bug — buttons read as
 * permanently held). s32 is unambiguous in both.
 */

extern s32 g_ColViewEnabled;    // master toggle
extern s32 g_ColViewGeo;        // layer: world collision volumes
extern s32 g_ColViewHitboxes;   // layer: chr hit bboxes
extern s32 g_ColViewOutlines;   // black edge bars on the chr hit bboxes
extern s32 g_ColViewProps;      // layer: object/door collision (blocks + cyls)
extern s32 g_ColViewXray;       // draw through walls (no depth test)
extern s32 g_ColViewAlpha;      // fill alpha, 0..255
extern s32 g_ColViewRange;      // draw radius in world units

Gfx *colviewRender(Gfx *gdl);

#endif

#endif
