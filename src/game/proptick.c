#include <ultra64.h>
#include "lib/sched.h"
#include "constants.h"
#include "game/bondmove.h"
#include "game/cheats.h"
#include "game/chraction.h"
#include "game/debug.h"
#include "game/chr.h"
#include "game/prop.h"
#include "game/propsnd.h"
#include "game/objectives.h"
#include "game/game_096360.h"
#include "game/bondgun.h"
#include "game/gunfx.h"
#include "game/game_0b0fd0.h"
#include "game/modelmgr.h"
#include "game/tex.h"
#include "game/camera.h"
#include "game/player.h"
#include "game/playermgr.h"
#include "game/vtxstore.h"
#include "game/gfxmemory.h"
#include "game/explosions.h"
#include "game/smoke.h"
#include "game/sparks.h"
#include "game/bg.h"
#include "game/file.h"
#include "game/mplayer/setup.h"
#include "game/bot.h"
#include "game/botact.h"
#include "game/mplayer/mplayer.h"
#include "game/pad.h"
#include "game/propobj.h"
#include "game/splat.h"
#include "game/wallhit.h"
#include "bss.h"
#include "lib/vi.h"
#include "lib/main.h"
#include "lib/model.h"
#include "lib/rng.h"
#include "lib/mtx.h"
#include "lib/anim.h"
#include "lib/collision.h"
#include "data.h"
#include "types.h"

#ifndef PLATFORM_N64
#include "system.h"
#include "net/net.h"
#endif

void propsTick(void)
{
	s32 i;
	struct prop *prop;
	struct prop *next;
	struct prop *next2;
	s32 done;
	s32 tickop;

	for (i = 0; i < PLAYERCOUNT(); i++) {
		g_Vars.players[i]->bondextrapos.x = 0;
		g_Vars.players[i]->bondextrapos.y = 0;
		g_Vars.players[i]->bondextrapos.z = 0;
	}

	shieldhitsTick();
	chraTickBg();

	prop = g_Vars.activeprops;

#ifndef PLATFORM_N64
	// Walk guard (crash diagnostic): the loop below is only safe against frees
	// that go through the TICKOP protocol. If a tick (or propExecuteTickOperation,
	// which runs AFTER the next2 re-read) delists/frees the prop the cursor is
	// about to step onto, propDelist has NULLed its ->next (and propFree relinks
	// it into the freelist), so the walk runs off the end into a NULL deref —
	// observed as an 0xc0000005 at the next = prop->next read. Detect a NULL or
	// delisted cursor, log who we last ticked (the culprit's tick is what broke
	// the list), and abort this frame's walk instead of crashing. One frame of
	// missed prop ticks is invisible; the log line is the evidence we need.
	struct prop *guardprev = NULL;
	s32 guardprevop = TICKOP_NONE;
#endif

	do {
#ifndef PLATFORM_N64
		if (!prop || (!prop->active && prop != g_Vars.pausedprops)) {
			const s32 previdx = (guardprev && guardprev >= g_Vars.props && guardprev < g_Vars.props + g_Vars.maxprops)
					? (s32)(guardprev - g_Vars.props) : -1;
			const s32 curidx = (prop && prop >= g_Vars.props && prop < g_Vars.props + g_Vars.maxprops)
					? (s32)(prop - g_Vars.props) : -1;
			sysLogPrintf(LOG_WARNING,
					"propsTick: %s cursor mid-walk (cur=%p idx=%d type=%d active=%d) after prop idx=%d type=%d tickop=%d - aborting walk",
					prop ? "delisted" : "NULL", prop, curidx,
					prop ? prop->type : -1, prop ? prop->active : -1,
					previdx, guardprev ? guardprev->type : -1, guardprevop);
			netDiagLogf("proptick_guard", "cur=%d type=%d prev=%d prevtype=%d prevop=%d",
					curidx, prop ? prop->type : -1, previdx,
					guardprev ? guardprev->type : -1, guardprevop);
			break;
		}
		guardprev = prop;
#endif
		next = prop->next;
		done = next == g_Vars.pausedprops;
		tickop = TICKOP_NONE;

		if (prop->type == PROPTYPE_CHR) {
			tickop = chrTickBeams(prop);
		} else if (prop->type == PROPTYPE_OBJ || prop->type == PROPTYPE_WEAPON || prop->type == PROPTYPE_DOOR) {
			tickop = objTick(prop);
		} else if (prop->type == PROPTYPE_EXPLOSION) {
			tickop = explosionTick(prop);
		} else if (prop->type == PROPTYPE_SMOKE) {
			tickop = smokeTick(prop);
		} else if (prop->type == PROPTYPE_PLAYER) {
			tickop = playerTickBeams(prop);
		}

		if (tickop == TICKOP_CHANGEDLIST) {
			next2 = next;
		} else {
			next2 = prop->next;
			done = next2 == g_Vars.pausedprops;

			if (tickop == TICKOP_RETICK) {
				propDelist(prop);
				propActivateThisFrame(prop);

				if (done) {
					next2 = prop;
					done = false;
				}
			} else {
				propExecuteTickOperation(prop, tickop);
			}
		}

#ifndef PLATFORM_N64
		guardprevop = tickop;
#endif
		prop = next2;
	} while (!done);
}
