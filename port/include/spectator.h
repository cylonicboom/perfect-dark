#ifndef _IN_SPECTATOR_H
#define _IN_SPECTATOR_H

#include <PR/ultratypes.h>
#include "types.h"

// Host spectator mode (port-only, MPOPTION_HOSTSPECTATOR). When the host
// enables this in the lobby, they don't occupy a player slot in the match
// (see netPlayersAllocate's spectator skip). Instead the host's screen is
// split into 1-4 panels showing other players, sims, a free flying camera,
// or a 3D top-down view. Panel state is local to the host — no wire traffic
// beyond the lobby's is_spectator flag.

#define SPEC_MAX_PANELS 4

// Per-panel mode. Order matters — spectatorCycleMode walks this sequence.
#define SPEC_MODE_PLAYER_FP  0  // first-person from a player's eye (+ bgun overlay in Phase B)
#define SPEC_MODE_PLAYER_TP  1  // third-person follow of a player
#define SPEC_MODE_SIM_FP     2  // first-person from a sim's eye (+ faked bgun in Phase B)
#define SPEC_MODE_SIM_TP     3  // third-person follow of a sim
#define SPEC_MODE_FREECAM    4  // free flying camera, panel-local pose
#define SPEC_MODE_TOPDOWN    5  // freecam pinned overhead, looking down

// Helpers for the dispatch code — both FP/TP variants of a mode read the
// same target type, so collapse to one check.
#define SPEC_MODE_IS_PLAYER(m) ((m) == SPEC_MODE_PLAYER_FP || (m) == SPEC_MODE_PLAYER_TP)
#define SPEC_MODE_IS_SIM(m)    ((m) == SPEC_MODE_SIM_FP    || (m) == SPEC_MODE_SIM_TP)
#define SPEC_MODE_IS_FP(m)     ((m) == SPEC_MODE_PLAYER_FP || (m) == SPEC_MODE_SIM_FP)
#define SPEC_MODE_IS_TP(m)     ((m) == SPEC_MODE_PLAYER_TP || (m) == SPEC_MODE_SIM_TP)
#define SPEC_MODE_IS_TARGET(m) (SPEC_MODE_IS_PLAYER(m) || SPEC_MODE_IS_SIM(m))

#define SPEC_TARGET_NONE 0xFF

struct spectatorpanel {
	u8 mode;          // SPEC_MODE_*
	u8 target;        // netclient id for PLAYER, sim idx for SIM, unused otherwise
	// Cached pose. PLAYER/SIM modes refresh these from the target each tick;
	// FREECAM/TOPDOWN integrate input into them and keep them between frames.
	struct coord pos;
	struct coord look;
	struct coord up;
	f32 yaw;          // freecam orientation accumulators (avoid normalising
	f32 pitch;        // look/up every frame)
	// Last room the panel rendered from. Needed because BSP draw culls by
	// portal graph starting at currentplayer->cam_room; without it the world
	// renders empty when freecam strays off the player's portal cluster.
	s32 room;
	// Phase B (bgun emulation) state. bgun_inited records whether
	// bgunInitHandAnims has been called on the panel's struct player —
	// happens once on the first FP frame. bgun_weaponnum tracks the
	// last weapon we equipped so we only call bgunEquipWeapon2 on
	// weapon changes (avoids re-running the switch animation every tick).
	// -1 means "no weapon currently equipped".
	u8 bgun_inited;
	s8 bgun_weaponnum;
};

// 1-4 panels owned by the spectator host. Index N corresponds to
// g_Vars.players[N] (the local panel viewport struct allocated for it).
extern struct spectatorpanel g_SpectatorPanels[SPEC_MAX_PANELS];

// When set, chrRender skips this prop's chr — so spectatorRenderPanel can
// hide the target's own body model in first-person spectate without the
// frustum eating into the chr's head/torso geometry. Scoped to a single
// panel render: spectatorRenderPanel sets it on entry (when the mode is
// FP) and clears it on exit. The per-player render loop is sequential so
// one global is enough; other panels viewing the same chr from a different
// angle (e.g. TP) see them normally.
extern struct prop *g_SpectatorHideChrProp;

// Number of panels the host requested in the lobby. Set by the lobby UI
// before netStartServer / mpStartMatch; read by spectatorIsActive and the
// per-frame tick. Range [1, SPEC_MAX_PANELS].
extern s32 g_SpectatorPanelCount;

// Which panel currently consumes input (freecam movement, view-target cycle).
// Defaults to 0; cycled by the host via a binding.
extern s32 g_SpectatorActivePanel;

// True when the host is the local player and host-spectator mode is on for
// the current match. Drives LOCALPLAYERCOUNT, viewport setup, HUD suppression.
s32 spectatorIsActive(void);

// Allocate / free the panel struct player slots in g_Vars.players[]. Call
// allocate at lobby->game transition (after netPlayersAllocate skipped the
// host) and free at stage end.
void spectatorAllocatePanels(void);
void spectatorFreePanels(void);

// Per-frame tick for one panel. Replaces lvTickPlayer for spectator panels:
// updates cam pose from the panel mode and prepares matrices for rendering.
// panelnum is the index into g_SpectatorPanels (= g_Vars.currentplayernum).
void spectatorTickPanel(s32 panelnum);

// Per-frame render hook for one panel. Replaces the chr/HUD-laden per-player
// body of lvRender with a minimal world render. Returns the updated display
// list pointer. currentplayer must already be the panel's slot when called.
// Gfx is reachable transitively via types.h's PR includes.
Gfx *spectatorRenderPanel(Gfx *gdl);

// Read input and apply to the active panel. Called once per frame before the
// viewport loop runs.
void spectatorReadInput(void);

// Cycle through valid view targets (next/prev human or sim depending on the
// active panel's mode). Used by the in-game spectator menu and key bindings.
void spectatorCycleTarget(s32 panelnum, s32 direction);

// Cycle the active panel's mode. SPEC_MODE_PLAYER -> SIM -> FREECAM -> TOPDOWN.
void spectatorCycleMode(s32 panelnum, s32 direction);

#endif // _IN_SPECTATOR_H
