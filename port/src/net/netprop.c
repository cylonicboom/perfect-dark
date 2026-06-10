// Networked-prop lifecycle seam — see port/include/net/netprop.h for the
// design summary and docs/PORT_NET_PROP_LIFECYCLE.md for the full rationale
// (it consolidates the spawn-broadcast sites, owns the free invariant, and
// records the per-prop audit trail behind /proplog).

#include <string.h>
#include <stdio.h>
#include "types.h"
#include "constants.h"
#include "data.h"
#include "bss.h"
#include "game/prop.h"
#include "game/propobj.h"
#include "system.h"
#include "net/net.h"
#include "net/netbuf.h"
#include "net/netmsg.h"
#include "net/netprop.h"

// ---------------------------------------------------------------------------
// Lifecycle event ring
// ---------------------------------------------------------------------------

struct netproplogentry {
	u32 frame;    // g_Vars.lvframe60 at the event
	u32 syncid;   // prop->syncid at the event (0 = client-local / unassigned)
	s16 propidx;  // index into g_Vars.props, -1 if outside the pool
	u8 ev;        // enum netpropev
	u8 ptype;     // prop->type at the event
	u16 extra;    // event-specific (free reason, drop reason, id low bits)
};

#define NETPROP_LOG_SIZE 4096 // power of two; ~48KB static

static struct netproplogentry g_NetPropLog[NETPROP_LOG_SIZE];
static u32 g_NetPropLogHead = 0; // total events appended (wraps the ring by mask)

static const char *netPropEvName(u8 ev)
{
	switch (ev) {
		case NETPROP_EV_SYNCID:          return "syncid";
		case NETPROP_EV_ACTIVATE:        return "activate";
		case NETPROP_EV_ACTIVATE_TF:     return "activate_tf";
		case NETPROP_EV_PAUSE:           return "pause";
		case NETPROP_EV_DELIST:          return "delist";
		case NETPROP_EV_FREE:            return "free";
		case NETPROP_EV_WIRE_SPAWN_TX:   return "spawn_tx";
		case NETPROP_EV_WIRE_SPAWN_RX:   return "spawn_rx";
		case NETPROP_EV_WIRE_SPAWN_DROP: return "spawn_drop";
		case NETPROP_EV_NETFREE:         return "netfree";
		case NETPROP_EV_EXPLODE_GATED:   return "explode_gated";
		case NETPROP_EV_HARDFREE_GATED:  return "hardfree_gated";
		case NETPROP_EV_PICKUP_REQ:      return "pickup_req";
		default:                         return "?";
	}
}

void netPropLogReset(void)
{
	g_NetPropLogHead = 0;
	memset(g_NetPropLog, 0, sizeof(g_NetPropLog));
}

void netPropLogEvent(struct prop *prop, u8 ev, u16 extra)
{
	// Outside a net session the ring stays idle so SP perf/behaviour is
	// untouched (the hooks sit on prop-list hot paths).
	if (g_NetMode == NETMODE_NONE || !prop) {
		return;
	}

	struct netproplogentry *e = &g_NetPropLog[g_NetPropLogHead & (NETPROP_LOG_SIZE - 1)];
	g_NetPropLogHead++;

	e->frame = g_Vars.lvframe60;
	e->syncid = prop->syncid;
	e->ev = ev;
	e->ptype = prop->type;
	e->extra = extra;

	if (g_Vars.props && prop >= g_Vars.props && prop < g_Vars.props + g_Vars.maxprops) {
		e->propidx = (s16)(prop - g_Vars.props);
	} else {
		e->propidx = -1;
	}
}

void netPropLogDump(u32 syncid, s32 maxlines)
{
	const u32 head = g_NetPropLogHead;
	const u32 avail = (head < NETPROP_LOG_SIZE) ? head : NETPROP_LOG_SIZE;
	s32 printed = 0;

	if (maxlines <= 0) {
		maxlines = 40;
	}

	if (avail == 0) {
		sysLogPrintf(LOG_CHAT, "proplog: empty");
		return;
	}

	if (syncid) {
		// All buffered events for one prop, oldest first.
		for (u32 i = head - avail; i != head; ++i) {
			const struct netproplogentry *e = &g_NetPropLog[i & (NETPROP_LOG_SIZE - 1)];
			if (e->syncid == syncid && printed < maxlines) {
				sysLogPrintf(LOG_CHAT, "proplog: f=%u sid=%u idx=%d t=%u %s x=%u",
						e->frame, e->syncid, e->propidx, e->ptype, netPropEvName(e->ev), e->extra);
				printed++;
			}
		}
	} else {
		// Newest maxlines events of any prop, oldest first.
		u32 start = head - ((avail < (u32)maxlines) ? avail : (u32)maxlines);
		for (u32 i = start; i != head; ++i) {
			const struct netproplogentry *e = &g_NetPropLog[i & (NETPROP_LOG_SIZE - 1)];
			sysLogPrintf(LOG_CHAT, "proplog: f=%u sid=%u idx=%d t=%u %s x=%u",
					e->frame, e->syncid, e->propidx, e->ptype, netPropEvName(e->ev), e->extra);
			printed++;
		}
	}

	sysLogPrintf(LOG_CHAT, "proplog: %d line(s), %u event(s) buffered, head=%u (usage: /proplog [syncid])",
			printed, avail, head);
}

// ---------------------------------------------------------------------------
// Syncid diet
// ---------------------------------------------------------------------------

void netPropAssignSyncId(struct prop *prop)
{
	if (g_NetMode != NETMODE_SERVER || !prop || prop->syncid) {
		return;
	}

	// Explosions/smoke are never wire-referenced by syncid (SVC_EXPLOSION
	// carries pos/rooms, smoke is purely local), but as the highest-churn
	// allocations they were the id-space burn that pushed real weapon/obj ids
	// past the reconcile coverage cap (NET_RECONCILE_MAXSYNCID) within minutes
	// on a busy server. Skip them; everything else keeps an id so all existing
	// wire references (players, chrs, weapons, objs, doors) are unaffected.
	if (prop->type == PROPTYPE_EXPLOSION || prop->type == PROPTYPE_SMOKE) {
		return;
	}

	prop->syncid = g_NetNextSyncId++;
	netPropLogEvent(prop, NETPROP_EV_SYNCID, (u16)prop->syncid);
}

// ---------------------------------------------------------------------------
// Teardown choke point
// ---------------------------------------------------------------------------

void netPropFreeSynced(struct prop *prop, u8 reason)
{
	if (!prop) {
		return;
	}

	netPropLogEvent(prop, NETPROP_EV_NETFREE, reason);

	// Only weapon/obj props route here; a chr or player prop reaching this is
	// a caller bug (the union at prop+0x48 would be torn down as the wrong
	// type — the exact class of corruption this choke point exists to stop).
	if (!prop->obj || (prop->type != PROPTYPE_WEAPON && prop->type != PROPTYPE_OBJ)) {
		sysLogPrintf(LOG_WARNING, "netprop: free reason=%u on prop %d type %d without obj — skipped",
				reason, (g_Vars.props && prop >= g_Vars.props) ? (s32)(prop - g_Vars.props) : -1,
				prop->type);
		return;
	}

	// Broken backlink = the obj has already been freed/recycled by another
	// owner. Tearing it down again is the double-free that self-loops the
	// freelist (crash ledger #12). Leave it for the heal/reap layers, loudly.
	if (prop->obj->prop != prop) {
		sysLogPrintf(LOG_WARNING, "netprop: free reason=%u prop %d syncid %u — obj backlink broken, skipped",
				reason, (g_Vars.props && prop >= g_Vars.props) ? (s32)(prop - g_Vars.props) : -1,
				prop->syncid);
		return;
	}

	// The engine's FULL teardown: objDetach from any parent chr, embedment/
	// projectile free, wallhit/inventory/chr-ref/shieldhit clearing, model
	// free, room dereg, delist, propFree. Anything less leaves a dangling
	// reference some later walk trips over (ledger #16/#17).
	objFreePermanently(prop->obj, true);

	// Post-free tripwire: no weapon slot may still back-reference the prop.
	// objFree clears the slot's base.prop, so a hit here means a NEW
	// free-without-clear generator exists — log it with the reason so the
	// caller is named.
	if (g_WeaponSlots) {
		for (s32 i = 0; i < g_MaxWeaponSlots; i++) {
			if (g_WeaponSlots[i].base.prop == prop) {
				sysLogPrintf(LOG_WARNING, "netprop: POST-FREE slot %d still references prop %d (reason=%u wpn=%d)",
						i, (s32)(prop - g_Vars.props), reason, (s32)g_WeaponSlots[i].weaponnum);
			}
		}
	}
}

// ---------------------------------------------------------------------------
// Spawn broadcast
// ---------------------------------------------------------------------------

void netSyncPropSpawn(struct prop *prop)
{
	if (g_NetMode != NETMODE_SERVER || !prop || !prop->syncid
			|| !g_NetLocalClient || g_NetLocalClient->state != CLSTATE_GAME) {
		return;
	}

	// Deployed-autogun attribution guard: the spawn writer derefs
	// g_Vars.players[owner]->client->id, which is NULL between a mid-match
	// disconnect and the next stage (netClientReset cleared the backlink).
	// The JIP snapshot path has carried this guard since proto 59; the live
	// broadcast sites never did.
	if (prop->type == PROPTYPE_OBJ && prop->obj && prop->obj->type == OBJTYPE_AUTOGUN) {
		const u8 ownerplayernum = (prop->obj->hidden & 0xf0000000) >> 28;
		if (ownerplayernum >= PLAYERCOUNT()
				|| !g_Vars.players[ownerplayernum]
				|| !g_Vars.players[ownerplayernum]->client) {
			sysLogPrintf(LOG_WARNING, "NET: autogun spawn %u skipped — owner playernum %u unresolved",
					prop->syncid, ownerplayernum);
			return;
		}
	}

	// Dedupe. objDrop broadcasts from its own tail AND several of its callers
	// broadcast again right after calling it (a pre-existing double-spawn:
	// the client allocated TWO props with the same syncid; the duplicate was
	// referenced by no move/free — and the reconcile couldn't reap it because
	// its syncid IS in the host's set — a permanent ghost gun). A same-syncid
	// re-broadcast within a few frames can only be that pattern (a legitimate
	// re-spawn after pickup/free is seconds away), so swallow it centrally
	// instead of auditing every caller forever. The read side independently
	// enforces one-prop-per-syncid (latest spawn wins) as the backstop.
	{
		static struct { u32 syncid; u32 frame; } s_recent[8];
		static u32 s_recenthead = 0;

		for (u32 i = 0; i < 8; i++) {
			if (s_recent[i].syncid == prop->syncid
					&& (u32)(g_Vars.lvframe60 - s_recent[i].frame) < 4u) {
				return;
			}
		}
		s_recent[s_recenthead & 7].syncid = prop->syncid;
		s_recent[s_recenthead & 7].frame = g_Vars.lvframe60;
		s_recenthead++;
	}

	netmsgSvcPropSpawnWrite(&g_NetMsgRel, prop);
	netmsgSvcPropMoveWrite(&g_NetMsgRel, prop, NULL);
	netPropLogEvent(prop, NETPROP_EV_WIRE_SPAWN_TX, 0);
}
