#ifndef _IN_NETPROP_H
#define _IN_NETPROP_H

// Networked-prop lifecycle seam (port-only, ENet-free so game code can include it).
//
// Three jobs, all aimed at the prop-list / weapon-slot corruption families in
// docs/PORT_NET_CRASH_LEDGER.md:
//
//  1. netPropFreeSynced() — the single teardown choke point every PORT-ADDED
//     free of a networked weapon/obj prop routes through (wire free, reconcile
//     reap, chr-state weapon swap, the propExecuteTickOperation netplay branch,
//     the client force-recycle). One place owns the "freed => fully
//     dereferenced" invariant and tripwires any violation with the culprit.
//  2. netSyncPropSpawn() — the single spawn broadcast helper every server-side
//     dynamic-prop spawn site calls (replaces five hand-rolled inline blocks
//     with inconsistent gating).
//  3. The lifecycle event ring + /proplog — a cheap per-event audit trail
//     (activate/pause/free/wire events, with reasons) so the next corruption
//     generator is identified from one log instead of a gdb session.
//
// Also hosts netPropAssignSyncId(): dynamic syncids are now assigned at first
// propActivate/propPause instead of propAllocate (the "syncid diet") so
// explosions/smoke stop consuming ids — see docs/PORT_NET_PROP_LIFECYCLE.md.

#include "types.h"

struct prop;

// Lifecycle ring event codes (netproplogentry.ev).
enum netpropev {
	NETPROP_EV_NONE = 0,
	NETPROP_EV_SYNCID,           // dynamic syncid assigned (extra = low 16 bits of id)
	NETPROP_EV_ACTIVATE,         // propActivate
	NETPROP_EV_ACTIVATE_TF,      // propActivateThisFrame
	NETPROP_EV_PAUSE,            // propPause (actually paused, not DONTPAUSE no-op)
	NETPROP_EV_DELIST,           // propDelist
	NETPROP_EV_FREE,             // propFree reached (any path)
	NETPROP_EV_WIRE_SPAWN_TX,    // server broadcast SVC_PROP_SPAWN
	NETPROP_EV_WIRE_SPAWN_RX,    // client created a prop from SVC_PROP_SPAWN
	NETPROP_EV_WIRE_SPAWN_DROP,  // client dropped a SVC_PROP_SPAWN (extra = reason)
	NETPROP_EV_NETFREE,          // netPropFreeSynced entry (extra = reason)
	NETPROP_EV_EXPLODE_GATED,    // client suppressed local propExplode for a synced prop
	NETPROP_EV_HARDFREE_GATED,   // client skipped hard-free fade of a synced prop
	NETPROP_EV_PICKUP_REQ,       // client sent CLC_PICKUP_REQUEST for this prop
};

// netPropFreeSynced reasons (NETPROP_EV_NETFREE extra).
enum netpropfreereason {
	NETPROP_FREE_WIRE = 1,       // SVC_PROP_FREE apply
	NETPROP_FREE_RECONCILE,      // SVC_PROP_RECONCILE ghost reap
	NETPROP_FREE_WEAPONSWAP,     // chr-state held-weapon swap (client-local, syncid 0)
	NETPROP_FREE_TICKOP,         // propExecuteTickOperation intact-backlink route
	NETPROP_FREE_RECYCLE,        // weaponCreate client force-recycle victim
	NETPROP_FREE_RESPAWN,        // client replacing its copy on a re-received SVC_PROP_SPAWN
};

// NETPROP_EV_WIRE_SPAWN_DROP reasons.
enum netpropspawndropreason {
	NETPROP_DROP_BADMODEL = 1,   // modelnum out of range
	NETPROP_DROP_SLOTSFULL,      // weaponCreate returned NULL (50-slot pool full)
	NETPROP_DROP_NOOWNER,        // autogun owner client unresolved
	NETPROP_DROP_NOOBJ,          // no obj bound after construction
	NETPROP_DROP_NOPROP,         // no prop allocated
};

// Reset the ring (stage start, both roles).
void netPropLogReset(void);

// Append one event. No-ops outside a net session (g_NetMode == NETMODE_NONE),
// so single-player perf is untouched. Cheap enough for the prop list hot paths.
void netPropLogEvent(struct prop *prop, u8 ev, u16 extra);

// Dump ring entries to the console/log. syncid 0 = the newest `maxlines` events
// of any prop; nonzero = all buffered events for that syncid (newest last).
void netPropLogDump(u32 syncid, s32 maxlines);

// Syncid diet: assign a dynamic syncid at first activation/pause (server only,
// skips EXPLOSION/SMOKE — they were the id-space burn that pushed weapon ids
// past the reconcile coverage cap). Called from propActivate/propActivateThisFrame/
// propPause; safe to call repeatedly (no-op once assigned).
void netPropAssignSyncId(struct prop *prop);

// THE teardown choke point for port-added frees of networked WEAPON/OBJ props
// (syncid may legitimately be 0 for the client-local held-weapon case). Routes
// through the engine's full objFreePermanently teardown, then tripwires any
// weapon slot still back-referencing the prop. Never touches a prop whose obj
// backlink is already broken (someone else owns it — the heal layers reap it).
void netPropFreeSynced(struct prop *prop, u8 reason);

// THE spawn broadcast: SVC_PROP_SPAWN + an initial SVC_PROP_MOVE on the
// reliable channel, with the full gate (server role, syncid assigned, local
// client in CLSTATE_GAME) and the deployed-autogun owner-validity guard that
// the old inline sites lacked. Safe to call from any server-side spawn site.
void netSyncPropSpawn(struct prop *prop);

#endif
