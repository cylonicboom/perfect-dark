#include <ultra64.h>
#ifndef PLATFORM_N64
#include <string.h> // memset for the prop-pool zeroing below
#endif
#include "constants.h"
#include "game/prop.h"
#include "bss.h"
#include "lib/memp.h"
#include "lib/vars.h"
#include "data.h"
#include "types.h"

void varsResetRoomProps(void);

void varsReset(void)
{
	s32 i;

	g_Vars.props = mempAlloc(ALIGN64(g_Vars.maxprops * sizeof(struct prop)), MEMPOOL_STAGE);
	g_Vars.onscreenprops = mempAlloc(ALIGN64(MAX_ONSCREEN_PROPS * sizeof(void *)), MEMPOOL_STAGE);

#ifndef PLATFORM_N64
	// The stage pool reuses the same arena every load, so virgin (never
	// propAllocate'd) slots contain the PREVIOUS stage's prop bytes — stale
	// syncid + type + obj pointers into the old heap layout. The netplay
	// walkers that scan the pool BY INDEX (the co-op NPC/obj round-robins and
	// prop-reconcile in netEndFrame, the JIP catch-up snapshot) trusted
	// `syncid != 0` as "live prop" and dereferenced a stale prop->obj from a
	// virgin slot — an intermittent 0xc0000005 that only appears after a
	// previous stage seeded the arena (first boot reads zeros and is safe).
	// Zeroing the pool makes `syncid != 0` a sound liveness test (and
	// incidentally fixes the vanilla uninitialised freelist-tail @bug below).
	memset(g_Vars.props, 0, g_Vars.maxprops * sizeof(struct prop));
#endif

	g_AutoAimScale = 1;

	g_Vars.activeprops = g_Vars.activepropstail = NULL;
	g_Vars.pausedprops = NULL;

	g_Vars.numonscreenprops = 0;
	g_Vars.onscreenprops[0] = NULL;
	g_Vars.endonscreenprops = g_Vars.onscreenprops;

	g_Vars.freeprops = g_Vars.props;

	// @bug: The tail of the freeprops list will have an uninitialised next pointer.
	// This will likely crash the game if too many props get allocated,
	// but there is no known way to exhaust the free props list.
	for (i = 0; i < g_Vars.maxprops - 1; i++) {
		g_Vars.props[i].next = &g_Vars.props[i + 1];
	}

	varsResetRoomProps();

	if (g_Vars.normmplayerisrunning) {
		g_Vars.numpropstates = 4;
	} else {
		g_Vars.numpropstates = 7;
	}

	g_Vars.allocstateindex = 0;
	g_Vars.runstateindex = 0;
	g_Vars.alwaystick = 0;
	g_Vars.updateframe = 0xfffe;
	g_Vars.prevupdateframe = 0xffff;

	for (i = 0; i < ARRAYCOUNT(g_Vars.propstates); i++) {
		g_Vars.propstates[i].propcount = 0;
		g_Vars.propstates[i].chrpropcount = 0;
		g_Vars.propstates[i].updatetime = 0;
		g_Vars.propstates[i].chrupdatetime = 0;
		g_Vars.propstates[i].slotupdate240 = 0;
		g_Vars.propstates[i].slotupdate60error = 2;
	}
}

void varsResetRoomProps(void)
{
	s32 i;
	s32 j;

	g_RoomPropListChunkIndexes = mempAlloc(ALIGN16(g_Vars.roomcount * sizeof(s16)), MEMPOOL_STAGE);
	g_RoomPropListChunks = mempAlloc(MAX_ROOMPROPLISTCHUNKS * sizeof(struct roomproplistchunk), MEMPOOL_STAGE);

	for (i = 0; i < g_Vars.roomcount; i++) {
		g_RoomPropListChunkIndexes[i] = -1;
	}

	for (i = 0; i < MAX_ROOMPROPLISTCHUNKS; i++) {
		g_RoomPropListChunks[i].propnums[0] = -2;

		for (j = 1; j < ARRAYCOUNT(g_RoomPropListChunks[i].propnums); j++) {
			g_RoomPropListChunks[i].propnums[j] = -1;
		}
	}
}
