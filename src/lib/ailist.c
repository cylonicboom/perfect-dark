#include <ultra64.h>
#include "constants.h"
#include "bss.h"
#include "lib/ailist.h"
#include "data.h"
#include "types.h"

s32 g_NumGlobalAilists = 0;
s32 g_NumLvAilists = 0;

u8 *ailistFindById(s32 ailistid)
{
	s32 lower;
	s32 upper;
	s32 index;

	if (ailistid >= 0x401) {
		if (g_StageSetup.ailists) {
			lower = 0;
#ifndef PLATFORM_N64
			// B13: with the inclusive `upper >= lower` loop the last valid index
			// is count - 1; seeding upper with the count lets a miss probe
			// ailists[count] (one-element OOB read that can garbage-match).
			// In-bounds ids are found identically - only the OOB probe goes away.
			upper = g_NumLvAilists - 1;
#else
			upper = g_NumLvAilists;
#endif
			index;

			while (upper >= lower) {
				index = (lower + upper) / 2;

				if (g_StageSetup.ailists[index].id == ailistid) {
					return g_StageSetup.ailists[index].list;
				}

				if (ailistid < g_StageSetup.ailists[index].id) {
					upper = index - 1;
				} else {
					lower = index + 1;
				}
			}
		}
	} else {
		lower = 0;
#ifndef PLATFORM_N64
		// B13: same count-1 upper-bound fix as the stage-list search above
		upper = g_NumGlobalAilists - 1;
#else
		upper = g_NumGlobalAilists;
#endif
		index;

		while (upper >= lower) {
			index = (lower + upper) / 2;

			if (g_GlobalAilists[index].id == ailistid) {
				return g_GlobalAilists[index].list;
			}

			if (ailistid < g_GlobalAilists[index].id) {
				upper = index - 1;
			} else {
				lower = index + 1;
			}
		}
	}

	return NULL;
}
