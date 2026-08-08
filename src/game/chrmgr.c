#include <ultra64.h>
#include "constants.h"
#include "game/game_00b820.h"
#include "game/title.h"
#include "bss.h"
#include "lib/memp.h"
#include "data.h"
#include "types.h"
#ifndef PLATFORM_N64
#include "video.h"
#include "net/net.h"
#endif

void chrmgrReset(void)
{
	s32 i;

	var80062968 = 1;
	var8006296c = 0;
	g_SelectedAnimNum = 0;
	var80062974 = 0;
	var80062978 = 0;
	var8006297c = 0;
	g_NextChrnum = 5000;
	g_ChrSlots = NULL;
	g_NumChrSlots = 0;

	g_ShieldHits = mempAlloc(sizeof(struct shieldhit) * 20, MEMPOOL_STAGE);

	for (i = 0; i < 20; i++) {
		g_ShieldHits[i].prop = NULL;
	}

	g_ShieldHitActive = 0;
	g_NumChrs = 0;
	g_Chrnums = NULL;
	g_ChrIndexes = NULL;
	var80062960 = mempAlloc(ALIGN16(15 * sizeof(struct var80062960)), MEMPOOL_STAGE);

	for (i = 0; i < ARRAYCOUNT(var8009ccc0); i++) {
#ifdef PLATFORM_N64
		var8009ccc0[i] = (void *)ALIGN64(mempAlloc(16 * 16 * sizeof(u16) + 0x40, MEMPOOL_STAGE));
#else
		if (!var8009ccc0[i]) {
			var8009ccc0[i] = videoCreateFramebuffer(16, 16, false, false);
		}
#endif
	}

	resetSomeStageThings();
}

#ifndef PLATFORM_N64
// Experiments > Unlimited Corpses (from Ben Colclough's branch): corpses are
// exempt from the count-based fade passes in chraTickBg, and the chr slot
// pool grows so spawner stages don't run out of slots while corpses linger.
// Single-player only (slot count and fade RNG affect netplay determinism);
// the slot growth applies at stage load. Persisted as Game.UnlimitedCorpses.
s32 g_UnlimitedCorpses = 0;
#endif

void chrmgrConfigure(s32 numchrs)
{
	s32 i;

#ifndef PLATFORM_N64
	g_NumChrSlots = PLAYERCOUNT() + numchrs + MAX_BOTS;

	if (g_UnlimitedCorpses && g_NetMode == NETMODE_NONE) {
		// Ben's branch uses +400 spare slots outright; keep his headroom.
		g_NumChrSlots += 400;
	}
#else
	g_NumChrSlots = PLAYERCOUNT() + numchrs + 10;
#endif
	g_ChrSlots = mempAlloc(ALIGN16(g_NumChrSlots * sizeof(struct chrdata)), MEMPOOL_STAGE);

#ifndef PLATFORM_N64
	// Zero the whole slot array. MEMPOOL_STAGE is rewound, never cleared, so
	// slots otherwise start as whatever the PREVIOUS stage left at this pool
	// offset — and chrInit is field-by-field, leaving ~15 fields plus most of
	// the act_* union running on those stale bytes (the exact mechanism behind
	// the uninitialised-netsnap crash family; g_BgChrs already block-zeroes via
	// blankchr in game_00b820.c). Also the precondition for the chrdata cache
	// repack: with a zeroed start, field ORDER can no longer change which
	// garbage lands in an uninitialised field.
	bzero(g_ChrSlots, g_NumChrSlots * sizeof(struct chrdata));
#endif

	for (i = 0; i < g_NumChrSlots; i++) {
		g_ChrSlots[i].chrnum = -1;
		g_ChrSlots[i].model = NULL;
		g_ChrSlots[i].prop = NULL;
	}

	g_NumChrs = 0;
	g_Chrnums = mempAlloc(ALIGN16(g_NumChrSlots * sizeof(g_Chrnums[0])), MEMPOOL_STAGE);
	g_ChrIndexes = mempAlloc(ALIGN16(g_NumChrSlots * sizeof(g_ChrIndexes[0])), MEMPOOL_STAGE);

	for (i = 0; i < g_NumChrSlots; i++) {
		g_Chrnums[i] = -1;
		g_ChrIndexes[i] = -1;
	}
}
