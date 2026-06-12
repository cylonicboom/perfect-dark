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

// Spawn-broadcast dedupe window (netSyncPropSpawn). File-scope so the
// per-stage reset below can clear it: both inputs it keys on restart at a
// stage boundary (lvReset zeroes lvframe60, netSyncIdsAllocate restarts the
// syncid counter), so a stale entry from the previous match could otherwise
// false-match a legitimate spawn in the new one and silently swallow its
// broadcast (client permanently misses the prop).
static struct { u32 syncid; u32 frame; } g_NetSpawnRecent[8];
static u32 g_NetSpawnRecentHead = 0;

void netPropLogReset(void)
{
	g_NetPropLogHead = 0;
	memset(g_NetPropLog, 0, sizeof(g_NetPropLog));
	memset(g_NetSpawnRecent, 0, sizeof(g_NetSpawnRecent));
	g_NetSpawnRecentHead = 0;
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

	// Fly-by-wire (proto 76): if any player's rocket-cam is locked onto the obj
	// we're about to free (wire FREE / reconcile reap / spawn-collision replace /
	// hard-free), drop the dangling pointer before objFreePermanently recycles
	// the slot, or the camera follows garbage. The propobj.c explode loops only
	// run on the machine doing the explode, so the client teardown choke point is
	// the only place this fires for a wire-owned free. STATIC lets the local
	// lvRender play the vanilla cut back to NORMAL.
	for (s32 pi = 0; pi < PLAYERCOUNT(); pi++) {
		struct player *fp = g_Vars.players[pi];
		if (fp && fp->slayerrocket == (struct weaponobj *)prop->obj) {
			fp->slayerrocket = NULL;
			fp->visionmode = VISIONMODE_SLAYERROCKETSTATIC;
		}
		if (fp && fp->fbw_spawnsyncid && fp->fbw_spawnsyncid == (u16)prop->syncid) {
			fp->fbw_spawnsyncid = 0;
		}
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
	for (u32 i = 0; i < 8; i++) {
		if (g_NetSpawnRecent[i].syncid == prop->syncid
				&& (u32)(g_Vars.lvframe60 - g_NetSpawnRecent[i].frame) < 4u) {
			return;
		}
	}
	g_NetSpawnRecent[g_NetSpawnRecentHead & 7].syncid = prop->syncid;
	g_NetSpawnRecent[g_NetSpawnRecentHead & 7].frame = g_Vars.lvframe60;
	g_NetSpawnRecentHead++;

	netmsgSvcPropSpawnWrite(&g_NetMsgRel, prop);
	netmsgSvcPropMoveWrite(&g_NetMsgRel, prop, NULL);
	netPropLogEvent(prop, NETPROP_EV_WIRE_SPAWN_TX, 0);
}

// ---------------------------------------------------------------------------
// Invariant auditor
// ---------------------------------------------------------------------------

u32 g_NetAuditHealFires = 0;
u32 g_NetAuditReapFires = 0;
u32 g_NetAuditOrphanFires = 0;
s32 g_NetAuditEnabled = 1;
u32 g_NetAuditRate = 60;

static u32 g_NetAuditFailCycles = 0; // cumulative FAIL cycles this stage
static u32 g_NetAuditCycles = 0;     // total cycles this stage

// Coverage cap mirrors NET_RECONCILE_MAXSYNCID (netmsg.c). Kept local to size
// the dedupe bitmap; a syncid at or past it is uncovered by both systems.
#define NETAUDIT_MAXSYNCID 65536

// Order-independent per-syncid hash for the manifest digest (Knuth
// multiplicative finalizer). XOR-combined across the set, so the two machines'
// differing pool order doesn't matter — only the SET membership does.
static u32 netAuditSyncHash(u32 syncid)
{
	u32 h = syncid * 2654435761u;
	h ^= h >> 15;
	h *= 2246822519u;
	h ^= h >> 13;
	return h;
}

void netPropAuditReset(void)
{
	g_NetAuditHealFires = 0;
	g_NetAuditReapFires = 0;
	g_NetAuditOrphanFires = 0;
	g_NetAuditFailCycles = 0;
	g_NetAuditCycles = 0;
}

bool netPropAudit(void)
{
	// Dedupe bitmap over the syncid space — static so we don't put 8KB on the
	// game-thread stack. Single-threaded, so reuse across calls is fine.
	static u8 seen[NETAUDIT_MAXSYNCID / 8];
	memset(seen, 0, sizeof(seen));

	if (g_NetMode == NETMODE_NONE || !g_Vars.props) {
		return true;
	}

	s32 netprops = 0;   // networked weapon/obj props
	s32 dupes = 0;      // two props sharing a syncid
	s32 corpses = 0;    // listed null-union weapon/obj/door/explosion/smoke
	s32 overcap = 0;    // networked syncid >= coverage cap
	u32 manifest = 0;   // xor-hash of the networked weapon/obj syncid set

	// Pure pool iteration — never follows ->next, so a corrupt/cyclic active
	// list can't hang the audit (that's propsHealActiveList's job; we just
	// count its fires below).
	for (s32 i = 0; i < g_Vars.maxprops; i++) {
		struct prop *prop = &g_Vars.props[i];

		// Standing corpse: in the active list (active flag set) but the union
		// pointer is NULL — the freed-but-still-listed class every tick walk
		// AVs on. (propAllocate-fresh free props have active == false.)
		if (prop->active && prop->obj == NULL
				&& (prop->type == PROPTYPE_OBJ || prop->type == PROPTYPE_WEAPON
					|| prop->type == PROPTYPE_DOOR || prop->type == PROPTYPE_EXPLOSION
					|| prop->type == PROPTYPE_SMOKE)) {
			corpses++;
		}

		if (prop->syncid
				&& (prop->type == PROPTYPE_WEAPON || prop->type == PROPTYPE_OBJ)
				&& prop->obj) {
			netprops++;
			manifest ^= netAuditSyncHash(prop->syncid);
			if (prop->syncid < NETAUDIT_MAXSYNCID) {
				const u32 bit = (u32)prop->syncid;
				if (seen[bit >> 3] & (1 << (bit & 7))) {
					dupes++;
				} else {
					seen[bit >> 3] |= (u8)(1 << (bit & 7));
				}
			} else {
				overcap++;
			}
		}
	}

	// Weapon-slot census (formalizes the ad-hoc `weaponslots` diag line).
	s32 occ = 0, synced = 0, proj = 0, projdead = 0, orphan = 0;
	if (g_WeaponSlots) {
		for (s32 i = 0; i < g_MaxWeaponSlots; i++) {
			struct prop *wp = g_WeaponSlots[i].base.prop;
			if (!wp) {
				continue;
			}
			occ++;
			if (wp->syncid) {
				synced++;
			}
			if (g_WeaponSlots[i].base.hidden & OBJHFLAG_PROJECTILE) {
				proj++;
				if (!wp->active) {
					projdead++;
				}
			}
			// Orphan: the slot references a prop whose union no longer points
			// back (freed/recycled without releasing the slot) — the ledger #16
			// generator. weaponSlotsReapOrphans clears these; a nonzero count
			// here means one slipped past it this frame.
			if (wp->obj != &g_WeaponSlots[i].base) {
				orphan++;
			}
		}
	}

	// Heal-layer activity since the last cycle (read + reset).
	const u32 heal = g_NetAuditHealFires;
	const u32 reap = g_NetAuditReapFires;
	const u32 orphreap = g_NetAuditOrphanFires;
	g_NetAuditHealFires = 0;
	g_NetAuditReapFires = 0;
	g_NetAuditOrphanFires = 0;

	// Standing corruption = hard FAIL; heal activity = the corruption was
	// present but masked (still a finding for the soak verdict).
	const bool standing = (dupes > 0 || corpses > 0 || orphan > 0 || overcap > 0);
	const bool fired = (heal > 0 || reap > 0 || orphreap > 0);
	const bool pass = !standing && !fired;

	g_NetAuditCycles++;
	if (!pass) {
		g_NetAuditFailCycles++;
	}

	const char role = (g_NetMode == NETMODE_CLIENT) ? 'C' : 'S';

	// Always to the diag log (machine-parseable, offline-comparable manifest).
	netDiagLogf("audit",
			"role=%c result=%s netprops=%d manifest=0x%08x dupes=%d corpses=%d overcap=%d "
			"slots_occ=%d synced=%d local=%d proj=%d projdead=%d orphan=%d heal=%u reap=%u orphreap=%u",
			role, pass ? "PASS" : "FAIL", netprops, manifest, dupes, corpses, overcap,
			occ, g_MaxWeaponSlots, occ - synced, proj, projdead, orphan, heal, reap, orphreap);

	// To the console only when there's something to see, so a healthy soak is
	// quiet but a regression is loud even without a diag file open.
	if (!pass) {
		sysLogPrintf(LOG_WARNING,
				"AUDIT %s [%c]: dupes=%d corpses=%d orphan=%d overcap=%d | heal=%u reap=%u orphreap=%u (fail %u/%u)",
				standing ? "FAIL" : "WARN", role, dupes, corpses, orphan, overcap,
				heal, reap, orphreap, g_NetAuditFailCycles, g_NetAuditCycles);
	}

	return pass;
}

void netPropAuditTick(void)
{
	if (!g_NetAuditEnabled || g_NetMode == NETMODE_NONE) {
		return;
	}
	if (g_NetAuditRate == 0) {
		return;
	}
	if ((g_NetTick % g_NetAuditRate) != 0) {
		return;
	}
	netPropAudit();
}
