// Host spectator mode (port-only). See port/include/spectator.h for the
// architecture summary. Activated by MPOPTION_HOSTSPECTATOR. Owns 1-4 panel
// viewports rendered as a faux split-screen on the host; each panel reads
// from another player/sim, a free-flying camera, or a 3D overhead view.

#include <math.h>
#include <string.h>
#include <PR/ultratypes.h>

#include "spectator.h"

#include "data.h"
#include "types.h"
#include "constants.h"
#include "bss.h"
#include "lib/joy.h"
#include "lib/main.h"
#include "lib/mtx.h"
#include "game/playermgr.h"
#include "game/player.h"
#include "game/chraction.h"
#include "game/bondgun.h"
#include "input.h"
#include "game/bg.h"
#include "game/sky.h"
#include "game/zbuf.h"
#include "game/env.h"
#include "game/dlights.h"
#include "game/prop.h"
#include "game/propobj.h"
#include "game/shards.h"
#include "game/sparks.h"
#include "game/weather.h"
#include "game/artifact.h"
#include "lib/vi.h"
#include "net/net.h"

// Decompiled symbols whose declarations live in headers we already include.
// Listed here as a comment so the dependency is obvious from this file:
//   playerAllocateMatrices, playerSetCamPropertiesWithoutRoom        (player.h)
//   playerGetViewport{Width,Height,Left,Top}, player0f0bd358          (player.h)
//   bgGetScaleBg2Gfx                                                  (bg.h)
//   mtx00016748                                                       (lib/mtx.h)
//   propsSort, propsTickPlayer                                        (prop.h)
//   propsRenderBeams                                                  (propobj.h)
//   shardsRender, sparksRender, weatherRender                         (shards.h / sparks.h / weather.h)
//   artifactsClear, artifactsTick                                     (artifact.h)
//   var800613a0[], var80061380[]                                      (data.h)

struct spectatorpanel g_SpectatorPanels[SPEC_MAX_PANELS];
s32 g_SpectatorPanelCount = 1;
s32 g_SpectatorActivePanel = 0;
struct prop *g_SpectatorHideChrProp = NULL;

// Freecam tuning knobs. World units per tick at the analog stick's extreme;
// scaled by stick magnitude. Conservative defaults — keeps the cam controllable
// even on small maps where 1000 u/s would overshoot in one frame.
#define SPEC_FREECAM_MOVE_SPEED   30.f
#define SPEC_FREECAM_BOOST_MULT   4.f
#define SPEC_FREECAM_TURN_SPEED   2.5f  // radians per tick at full stick
#define SPEC_FREECAM_VERT_SPEED   25.f
// Mouse look multiplier on top of inputMouseGetScaledDelta's already-scaled
// output. Tuned so 1px of mouse movement at default sensitivity rotates the
// cam ~0.01 rad (~0.57°), matching the feel of bondmove's freelook.
#define SPEC_MOUSELOOK_SCALE      0.01f
// Top-down altitude offset above the centre of the level. Picked empirically
// so most Combat Sim arenas fit on screen without clipping into geometry.
#define SPEC_TOPDOWN_ALTITUDE     5000.f
// Third-person follow offsets. Cam sits SPEC_FOLLOW_BACK units behind the
// target along their facing and SPEC_FOLLOW_UP above eye, looking at the
// target's head. 120 ≈ 1.2× chr body height; 40 up gives ~18° downward look.
#define SPEC_FOLLOW_BACK          120.f
#define SPEC_FOLLOW_UP             40.f
// Pitch clamp for freecam (radians). Stops short of straight up/down to avoid
// gimbal lock — top-down mode uses an explicit straight-down vector instead of
// approaching it from freecam pitch.
#define SPEC_PITCH_LIMIT          (1.55f)

// SDL scancodes for the FPS-style WASD/Q/E/F/TAB bindings. input.h only names
// VK_A and VK_Z explicitly; the rest of the alphabet is at A + (letter - 'A')
// in SDL scancode order, so we name the ones we need locally.
#define SPEC_VK_W    26
#define SPEC_VK_A    4
#define SPEC_VK_S    22
#define SPEC_VK_D    7
#define SPEC_VK_Q    20
#define SPEC_VK_E    8
#define SPEC_VK_F    9
#define SPEC_VK_TAB  43
#define SPEC_VK_LBR  47   // [
#define SPEC_VK_RBR  48   // ]

s32 spectatorIsActive(void)
{
	// Host only (NETMODE_SERVER, never NETMODE_CLIENT). g_NetLocalClient's
	// is_spectator flag is the authoritative source: g_MpSetup.options gets
	// overwritten by mpsetupLoadCurrentFile during the lobby->Combat-Sim
	// transition (mplayer.c:4100 does `g_MpSetup = config->config.setup;`),
	// so any MPOPTION bit set in the lobby UI doesn't survive into the match.
	// The netclient field is set once in menuhandlerHostStart and not touched
	// until the next netStartServer / netDisconnect.
	if (g_NetMode != NETMODE_SERVER) {
		return 0;
	}
	if (!g_NetLocalClient) {
		return 0;
	}
	return g_NetLocalClient->is_spectator ? 1 : 0;
}

static void spectatorDefaultPanel(struct spectatorpanel *p, s32 idx)
{
	(void)idx;
	memset(p, 0, sizeof(*p));
	p->bgun_weaponnum = -1;  // memset 0 would mean "weapon 0", which is real (WEAPON_NONE)
	// All panels default to FREECAM so they share the same spawn-seed path —
	// spectatorTickPanel's `room < 0` check triggers spectatorPickSpawnPos
	// which finds a valid prop position (sim or client) on the first tick.
	// Defaulting panels 1..3 to PLAYER mode against netclient indices that
	// may not exist (sims-only Combat Sim has no remote clients) left them
	// rendering from (0,0,0)+room=1 — pose outside the world, BSP walk into
	// garbage rooms, crash deep in bgRenderRoomOpaque. The host can switch
	// any panel to PLAYER/SIM/TOPDOWN at runtime via C-buttons.
	p->mode = SPEC_MODE_FREECAM;
	p->target = SPEC_TARGET_NONE;
	p->up.x = 0.f;
	p->up.y = 1.f;
	p->up.z = 0.f;
	p->look.x = 0.f;
	p->look.y = 0.f;
	p->look.z = 1.f;
	p->yaw = 0.f;
	p->pitch = 0.f;
	p->room = -1;
}

void spectatorAllocatePanels(void)
{
	if (g_NetDedicatedMode) {
		// Dedicated server: no local viewports at all. Clamping to 1 panel
		// would tag a g_Vars.players[] slot as spectator AFTER its chr/prop
		// already spawned in setup.c, leaving a ghost player visible to
		// remote clients. Skip allocation entirely.
		g_SpectatorPanelCount = 0;
		g_SpectatorActivePanel = 0;
		return;
	}
	if (g_SpectatorPanelCount < 1) {
		g_SpectatorPanelCount = 1;
	}
	if (g_SpectatorPanelCount > SPEC_MAX_PANELS) {
		g_SpectatorPanelCount = SPEC_MAX_PANELS;
	}
	for (s32 i = 0; i < SPEC_MAX_PANELS; i++) {
		spectatorDefaultPanel(&g_SpectatorPanels[i], i);
	}
	// Combatants live at g_Vars.players[0..combatants-1] (placed there by
	// playermgrAllocatePlayer + netPlayersAllocate, matching cl->playernum 1:1)
	// and panels go at the high slots [combatants..combatants+panels-1].
	// PLAYERCOUNT() = combatants + panels per pdmain's spectator adjustment
	// to numplayers (playermgrAllocatePlayer fills slots [0..numplayers-1]
	// sequentially), so combatants is just the leftover after subtracting
	// the panel count.
	const s32 total = PLAYERCOUNT();
	s32 combatants = total - g_SpectatorPanelCount;
	if (combatants < 0) combatants = 0;
	// Tag the high-index slots as panels. The is_spectator flag gates HUD
	// suppression, routes lvTickPlayer through spectatorTickPanel, and lets
	// lvRender skip the non-panel (combatant) slots in its per-player loop.
	// We don't reorder g_Vars.playerorder — playermgrShuffle resets it to
	// [0..MAX_PLAYERS-1] every frame anyway, and lvRender just iterates all
	// PLAYERCOUNT() slots and skips combatants instead.
	for (s32 i = 0; i < g_SpectatorPanelCount; i++) {
		const s32 slot = combatants + i;
		if (slot >= MAX_PLAYERS) break;
		struct player *pl = g_Vars.players[slot];
		if (pl) {
			pl->is_spectator = 1;
			pl->spectator_panel = (u8)i;
		}
	}
	g_SpectatorActivePanel = 0;
}

void spectatorFreePanels(void)
{
	for (s32 i = 0; i < SPEC_MAX_PANELS; i++) {
		memset(&g_SpectatorPanels[i], 0, sizeof(g_SpectatorPanels[i]));
		struct player *pl = g_Vars.players[i];
		if (pl) {
			pl->is_spectator = 0;
			pl->spectator_panel = 0;
		}
	}
	g_SpectatorPanelCount = 1;
	g_SpectatorActivePanel = 0;
}

// Translate stick reading [-128, 127] to normalized [-1, 1] with dead-zone.
static f32 spectatorStick(s8 raw)
{
	const s32 dead = 16;
	if (raw > dead) {
		return (f32)(raw - dead) / (f32)(127 - dead);
	}
	if (raw < -dead) {
		return (f32)(raw + dead) / (f32)(128 - dead);
	}
	return 0.f;
}

// Resolve a panel's target to a chr we can read pose from. Returns NULL if the
// target is gone (disconnected client, despawned sim) so the caller can fall
// back to freecam.
//
// Reads chr via g_MpAllChrPtrs[cl->playernum] — pdmain inflates numplayers to
// (combatants + panels) for spectator hosts and spectatorAllocatePanels parks
// panels at the high g_Vars.players[] indices, so combatant slots [0..N-1]
// still get a real player+chr allocation through lvReset's playerSpawn loop
// (which populates g_MpAllChrPtrs). cl->player is also valid for combatants;
// either path works.
static struct chrdata *spectatorResolveTargetChr(struct spectatorpanel *p)
{
	if (SPEC_MODE_IS_PLAYER(p->mode)) {
		if (p->target >= NET_MAX_CLIENTS) {
			return NULL;
		}
		const struct netclient *cl = &g_NetClients[p->target];
		if (cl->state < CLSTATE_GAME || cl->is_spectator || cl->playernum >= MAX_MPCHRS) {
			return NULL;
		}
		struct chrdata *chr = g_MpAllChrPtrs[cl->playernum];
		if (!chr || !chr->prop) {
			return NULL;
		}
		return chr;
	}
	if (SPEC_MODE_IS_SIM(p->mode)) {
		if (p->target >= NET_MAX_BOTS) {
			return NULL;
		}
		struct chrdata *chr = g_MpBotChrPtrs[p->target];
		if (!chr || !chr->prop || chr->actiontype == ACT_DEAD || chr->actiontype == ACT_DIE) {
			return NULL;
		}
		return chr;
	}
	return NULL;
}

// Pick the next valid target index for a SPEC_MODE_PLAYER / SPEC_MODE_SIM
// panel. dir is +1 / -1; wraps. Returns SPEC_TARGET_NONE if there are no
// candidates (which forces a fallback to freecam at tick time).
static u8 spectatorNextTarget(u8 mode, u8 cur, s32 dir)
{
	const s32 limit = SPEC_MODE_IS_PLAYER(mode) ? NET_MAX_CLIENTS : NET_MAX_BOTS;
	if (limit <= 0) {
		return SPEC_TARGET_NONE;
	}
	s32 step = (dir >= 0) ? 1 : -1;
	s32 idx = (cur == SPEC_TARGET_NONE) ? 0 : (s32)cur;
	for (s32 tries = 0; tries < limit; tries++) {
		idx = (idx + step + limit) % limit;
		if (SPEC_MODE_IS_PLAYER(mode)) {
			const struct netclient *cl = &g_NetClients[idx];
			if (cl->state >= CLSTATE_GAME && !cl->is_spectator && cl->playernum < MAX_MPCHRS) {
				struct chrdata *chr = g_MpAllChrPtrs[cl->playernum];
				if (chr && chr->prop) {
					return (u8)idx;
				}
			}
		} else {
			struct chrdata *chr = g_MpBotChrPtrs[idx];
			if (chr && chr->prop && chr->actiontype != ACT_DEAD && chr->actiontype != ACT_DIE) {
				return (u8)idx;
			}
		}
	}
	return SPEC_TARGET_NONE;
}

void spectatorCycleTarget(s32 panelnum, s32 direction)
{
	if (panelnum < 0 || panelnum >= SPEC_MAX_PANELS) {
		return;
	}
	struct spectatorpanel *p = &g_SpectatorPanels[panelnum];
	if (SPEC_MODE_IS_TARGET(p->mode)) {
		p->target = spectatorNextTarget(p->mode, p->target, direction);
	}
}

void spectatorCycleMode(s32 panelnum, s32 direction)
{
	if (panelnum < 0 || panelnum >= SPEC_MAX_PANELS) {
		return;
	}
	struct spectatorpanel *p = &g_SpectatorPanels[panelnum];
	s32 m = (s32)p->mode + ((direction >= 0) ? 1 : -1);
	if (m < SPEC_MODE_PLAYER_FP) m = SPEC_MODE_TOPDOWN;
	if (m > SPEC_MODE_TOPDOWN)   m = SPEC_MODE_PLAYER_FP;
	p->mode = (u8)m;
	// When switching into a target-driven mode, snap to a valid target so the
	// panel doesn't render an empty world for a frame. When swapping between
	// FP/TP variants of the same target type the existing target is fine —
	// only re-snap if it's invalid.
	if (SPEC_MODE_IS_TARGET(p->mode)) {
		if (spectatorResolveTargetChr(p) == NULL) {
			p->target = spectatorNextTarget(p->mode, SPEC_TARGET_NONE, +1);
		}
	}
}

// Drive the active panel's freecam pose from input. Called once per frame
// regardless of which panels are visible — the inputs only modify the active
// panel's state, so non-active freecam panels stay frozen at their last pose.
void spectatorReadInput(void)
{
	if (!spectatorIsActive()) {
		return;
	}
	if (g_SpectatorActivePanel < 0 || g_SpectatorActivePanel >= g_SpectatorPanelCount) {
		g_SpectatorActivePanel = 0;
	}

	// Re-apply panel-first ordering to g_Vars.playerorder so playermgrGetPlayerAtOrder
	// hands out panel slots before combatant slots. playermgrShuffle (called every
	// frame from pdmain.c::mainTick right before this) resets playerorder to the
	// default [0..MAX_PLAYERS-1], which puts the combatant slot at order 0 — that
	// claims g_Vars.currentplayerindex==0, and lvRender then skips combatants on a
	// spectator host. The first panel to actually render therefore inherits index
	// 1+, which silently disables every "first time this frame" block keyed on
	// currentplayerindex==0 (bgTick→bgTickRooms for ROOMFLAG_ONSCREEN,
	// propsTickPlayer's NOTYETTICKED setup, vi/player init paths). Net effect: sim
	// AI never ticks on the host, sims appear frozen to clients, and weapon pickup
	// state machines on the host don't advance. Reorder so panels come first;
	// combatants land at high orders and still get lvTickPlayer (which uses
	// currentplayernum for wire input application, not currentplayerindex).
	s32 panels = g_SpectatorPanelCount;
	if (panels < 1) panels = 1;
	if (panels > SPEC_MAX_PANELS) panels = SPEC_MAX_PANELS;
	s32 combatants = 0;
	for (s32 i = 0; i < MAX_PLAYERS; i++) {
		if (g_Vars.players[i] && !g_Vars.players[i]->is_spectator) {
			combatants++;
		}
	}
	s32 ord = 0;
	for (s32 i = 0; i < panels && ord < MAX_PLAYERS; i++) {
		g_Vars.playerorder[ord++] = (u32)(combatants + i);
	}
	for (s32 i = 0; i < combatants && ord < MAX_PLAYERS; i++) {
		g_Vars.playerorder[ord++] = (u32)i;
	}
	for (s32 i = combatants + panels; i < MAX_PLAYERS && ord < MAX_PLAYERS; i++) {
		g_Vars.playerorder[ord++] = (u32)i;
	}

	// Dedicated server has no local panels (g_SpectatorPanelCount == 0): there's
	// nothing to drive, and the panel-cycle '% g_SpectatorPanelCount' below would
	// be a divide-by-zero (0xc0000094) the moment Z-trigger / TAB is pressed. The
	// playerorder reorder above still runs (sims depend on it).
	if (g_SpectatorPanelCount < 1) {
		return;
	}

	// Mode/target/active-panel cycle bindings. C-buttons aren't bound to
	// anything useful for a spectator (no weapon, no aiming), so we reuse
	// them: C-Left / C-Right cycle the target, C-Up / C-Down cycle the mode,
	// Z cycles the active panel. Keyboard alternatives mirror the gamepad
	// (TAB / F / [ / ]) for users on KBM.
	const u32 pressed = joyGetButtonsPressedThisFrame(0, 0xffffffff);
	const bool target_prev = (pressed & L_CBUTTONS) != 0 || inputKeyJustPressed(SPEC_VK_LBR);
	const bool target_next = (pressed & R_CBUTTONS) != 0 || inputKeyJustPressed(SPEC_VK_RBR);
	const bool mode_prev   = (pressed & U_CBUTTONS) != 0;
	const bool mode_next   = (pressed & D_CBUTTONS) != 0 || inputKeyJustPressed(SPEC_VK_F);
	const bool panel_next  = (pressed & Z_TRIG)     != 0 || inputKeyJustPressed(SPEC_VK_TAB);
	if (target_prev) { spectatorCycleTarget(g_SpectatorActivePanel, -1); }
	if (target_next) { spectatorCycleTarget(g_SpectatorActivePanel, +1); }
	if (mode_prev)   { spectatorCycleMode(g_SpectatorActivePanel, -1); }
	if (mode_next)   { spectatorCycleMode(g_SpectatorActivePanel, +1); }
	if (panel_next)  { g_SpectatorActivePanel = (g_SpectatorActivePanel + 1) % g_SpectatorPanelCount; }

	struct spectatorpanel *p = &g_SpectatorPanels[g_SpectatorActivePanel];
	if (p->mode != SPEC_MODE_FREECAM && p->mode != SPEC_MODE_TOPDOWN) {
		return;
	}

	// Gamepad sticks. Held input each frame; deadzone applied by spectatorStick.
	const f32 lx_pad = spectatorStick(joyGetStickX(0));
	const f32 ly_pad = spectatorStick(joyGetStickY(0));
	const f32 rx_pad = spectatorStick(joyGetRStickX(0));
	const f32 ry_pad = spectatorStick(joyGetRStickY(0));
	const u32 held = joyGetButtons(0, 0xffffffff);

	// Keyboard WASD pan. Sum with stick so both work simultaneously without
	// one overriding the other. Q/E and Space/LCTRL drive altitude. LSHIFT
	// is the keyboard boost modifier (mirrors gamepad R-trigger).
	f32 lx_kbm = 0.f, ly_kbm = 0.f;
	if (inputKeyPressed(SPEC_VK_D)) { lx_kbm += 1.f; }
	if (inputKeyPressed(SPEC_VK_A)) { lx_kbm -= 1.f; }
	if (inputKeyPressed(SPEC_VK_W)) { ly_kbm += 1.f; }
	if (inputKeyPressed(SPEC_VK_S)) { ly_kbm -= 1.f; }

	const bool boost_held = (held & R_TRIG) || inputKeyPressed(VK_LSHIFT);
	const f32 boost = boost_held ? SPEC_FREECAM_BOOST_MULT : 1.f;

	const f32 lx = lx_pad + lx_kbm;
	const f32 ly = ly_pad + ly_kbm;

	// Mouse look — only when the mouse is locked into the window (otherwise
	// the user is interacting with menus / OS chrome and we'd swing the cam
	// every time the cursor crosses the window). inputMouseGetScaledDelta
	// returns (0,0) when unlocked, but we gate explicitly to make the
	// intent obvious.
	f32 mdx = 0.f, mdy = 0.f;
	if (inputMouseIsLocked()) {
		inputMouseGetScaledDelta(&mdx, &mdy);
	}

	// Altitude inputs: gamepad D-pad up/down, keyboard Space (up) / LCTRL (down),
	// plus Q (down) / E (up) for users who prefer them on the home row.
	f32 alt = 0.f;
	if (held & U_JPAD)            { alt += 1.f; }
	if (held & D_JPAD)            { alt -= 1.f; }
	if (inputKeyPressed(VK_SPACE)) { alt += 1.f; }
	if (inputKeyPressed(VK_LCTRL)) { alt -= 1.f; }
	if (inputKeyPressed(SPEC_VK_E)) { alt += 1.f; }
	if (inputKeyPressed(SPEC_VK_Q)) { alt -= 1.f; }

	// Top-down forces the look vector straight down before each tick. Pan
	// inputs translate the world position in the XZ plane; altitude inputs
	// move Y. Mouse delta is ignored in top-down so the view stays
	// orthographically aligned.
	if (p->mode == SPEC_MODE_TOPDOWN) {
		p->look.x = 0.f;
		p->look.y = -1.f;
		p->look.z = 0.f;
		p->up.x = 0.f;
		p->up.y = 0.f;
		p->up.z = 1.f;
		p->yaw = 0.f;
		p->pitch = -SPEC_PITCH_LIMIT;
		p->pos.x += lx * SPEC_FREECAM_MOVE_SPEED * boost;
		p->pos.z += ly * SPEC_FREECAM_MOVE_SPEED * boost;
		p->pos.y += alt * SPEC_FREECAM_VERT_SPEED * boost;
		return;
	}

	// Freecam: integrate yaw/pitch from right stick and mouse, compute
	// forward/right from those, then translate by lx/ly along the
	// cam-relative plane.
	p->yaw -= rx_pad * SPEC_FREECAM_TURN_SPEED * 0.05f;
	p->pitch += ry_pad * SPEC_FREECAM_TURN_SPEED * 0.05f;
	p->yaw -= mdx * SPEC_MOUSELOOK_SCALE;
	p->pitch -= mdy * SPEC_MOUSELOOK_SCALE;
	if (p->pitch >  SPEC_PITCH_LIMIT) p->pitch =  SPEC_PITCH_LIMIT;
	if (p->pitch < -SPEC_PITCH_LIMIT) p->pitch = -SPEC_PITCH_LIMIT;

	const f32 cy = cosf(p->yaw);
	const f32 sy = sinf(p->yaw);
	const f32 cp = cosf(p->pitch);
	const f32 sp = sinf(p->pitch);
	// Look direction (yaw rotates around world up, then pitch around the
	// horizontal right vector).
	p->look.x = sy * cp;
	p->look.y = sp;
	p->look.z = cy * cp;
	p->up.x = 0.f;
	p->up.y = 1.f;
	p->up.z = 0.f;

	// Pan in the cam's local horizontal plane. Use yaw-only forward so
	// holding "up" on the stick doesn't push the cam underground when
	// pitched down.
	const f32 fwdx = sy;
	const f32 fwdz = cy;
	const f32 rgtx = cy;
	const f32 rgtz = -sy;
	p->pos.x += (fwdx * ly + rgtx * lx) * SPEC_FREECAM_MOVE_SPEED * boost;
	p->pos.z += (fwdz * ly + rgtz * lx) * SPEC_FREECAM_MOVE_SPEED * boost;
	p->pos.y += alt * SPEC_FREECAM_VERT_SPEED * boost;
}

// Pick a reasonable initial position for freecam/topdown panels that haven't
// been positioned yet. Uses the host's first remote client or sim, falling
// back to world origin if the level isn't populated yet.
static void spectatorPickSpawnPos(struct coord *out, s32 *room_out)
{
	for (s32 i = 0; i < NET_MAX_CLIENTS; i++) {
		const struct netclient *cl = &g_NetClients[i];
		if (cl->state >= CLSTATE_GAME && !cl->is_spectator && cl->playernum < MAX_MPCHRS) {
			struct chrdata *chr = g_MpAllChrPtrs[cl->playernum];
			if (chr && chr->prop) {
				*out = chr->prop->pos;
				out->y += 200.f;
				*room_out = chr->prop->rooms[0];
				return;
			}
		}
	}
	for (s32 i = 0; i < NET_MAX_BOTS; i++) {
		struct chrdata *chr = g_MpBotChrPtrs[i];
		if (chr && chr->prop) {
			*out = chr->prop->pos;
			out->y += 200.f;
			*room_out = chr->prop->rooms[0];
			return;
		}
	}
	out->x = 0.f;
	out->y = 200.f;
	out->z = 0.f;
	*room_out = 1;
}

// Read a target chr's eye pose into out. Approximates "what the player sees"
// for the spectator panel — uses the chr's prop position + a small head-height
// offset and the chr's facing (chr->aimtheta when available, else prop->rot
// derived from theta).
// Common: derive eye position + forward vector for the target chr.
// PROPTYPE_PLAYER's prop->pos is already at eye height (bondmovePlayer sets
// prop->pos.y = groundy + vv_eyeheight); PROPTYPE_CHR's prop->pos is at the
// chr's centre so we nudge up. chrGetInverseTheta gives yaw in the game's
// CCW convention; TWO_PI - theta converts to the forward we use elsewhere.
static void spectatorTargetEyeAndForward(struct chrdata *chr, struct coord *eye, struct coord *forward)
{
	*eye = chr->prop->pos;
	if (chr->prop->type == PROPTYPE_CHR) {
		eye->y += 50.f;
	}
	const f32 TWO_PI = 6.2831853071795865f;
	const f32 theta = TWO_PI - chrGetInverseTheta(chr);
	const f32 ct = cosf(theta);
	const f32 st = sinf(theta);
	forward->x = -st;
	forward->y = 0.f;
	forward->z = ct;
}

// First-person pose: cam sits at the target's eye, looking the way they look.
// Same Y / facing logic as the existing /spec console command's
// netSpectateApply.
static void spectatorReadTargetPoseFP(struct chrdata *chr, struct coord *pos, struct coord *look, struct coord *up, s32 *room)
{
	struct coord forward;
	spectatorTargetEyeAndForward(chr, pos, &forward);
	*look = forward;
	up->x = 0.f;
	up->y = 1.f;
	up->z = 0.f;
	*room = chr->prop->rooms[0];
}

// Third-person pose: cam sits SPEC_FOLLOW_BACK behind the target along their
// facing and SPEC_FOLLOW_UP above eye, looking back at the target's head.
// Gives a slight downward angle so the target sits in the lower-middle of
// frame with their world-prop weapon visible.
static void spectatorReadTargetPoseTP(struct chrdata *chr, struct coord *pos, struct coord *look, struct coord *up, s32 *room)
{
	struct coord eye, forward;
	spectatorTargetEyeAndForward(chr, &eye, &forward);
	pos->x = eye.x - forward.x * SPEC_FOLLOW_BACK;
	pos->y = eye.y + SPEC_FOLLOW_UP;
	pos->z = eye.z - forward.z * SPEC_FOLLOW_BACK;
	// Look from cam_pos back at the target's head (eye). Normalise so the
	// view matrix builds cleanly.
	f32 lx = eye.x - pos->x;
	f32 ly = eye.y - pos->y;
	f32 lz = eye.z - pos->z;
	const f32 len = sqrtf(lx * lx + ly * ly + lz * lz);
	if (len > 0.0001f) {
		const f32 inv = 1.0f / len;
		look->x = lx * inv;
		look->y = ly * inv;
		look->z = lz * inv;
	} else {
		// Degenerate (cam coincident with eye) — fall back to target's facing
		// so we don't hand a zero vector to playerAllocateMatrices.
		*look = forward;
	}
	up->x = 0.f;
	up->y = 1.f;
	up->z = 0.f;
	// Use the target's room. The cam_pos may technically be in an adjacent
	// room when the target is near a portal, but portal culling tolerates
	// short displacements (eyespy and cutscene cams rely on this too).
	*room = chr->prop->rooms[0];
}

// Dispatcher: picks FP vs TP based on the panel's mode.
static void spectatorReadTargetPose(struct spectatorpanel *p, struct chrdata *chr, struct coord *pos, struct coord *look, struct coord *up, s32 *room)
{
	if (SPEC_MODE_IS_TP(p->mode)) {
		spectatorReadTargetPoseTP(chr, pos, look, up, room);
	} else {
		spectatorReadTargetPoseFP(chr, pos, look, up, room);
	}
}

void spectatorTickPanel(s32 panelnum)
{
	if (panelnum < 0 || panelnum >= g_SpectatorPanelCount) {
		return;
	}
	struct spectatorpanel *p = &g_SpectatorPanels[panelnum];
	struct player *pl = g_Vars.currentplayer;
	if (!pl || !pl->is_spectator) {
		return;
	}

	// Refresh the viewport rect on every tick — split-screen math depends on
	// LOCALPLAYERCOUNT() and currentplayernum and we may have switched panel
	// count between frames. Aspect / fov stay at default; the freecam doesn't
	// have a "zoom" notion so PLAYER_DEFAULT_FOV is fine for all modes.
	pl->fovy = PLAYER_DEFAULT_FOV;
	pl->aspect = player0f0bd358();
	// playerGetViewport* read g_Vars.currentplayernum to pick the quadrant
	// (0=TL, 1=TR, 2=BL, 3=BR). Panels live at high slot indices on a
	// spectator host with combatants, so currentplayernum (= slot index) no
	// longer matches the wanted quadrant — temp-swap to the panel index
	// across the calls. These functions are pure reads on currentplayernum,
	// so the swap is safe; nothing else runs between the calls.
	const s32 saved_pnum = g_Vars.currentplayernum;
	g_Vars.currentplayernum = panelnum;
	pl->viewwidth = playerGetViewportWidth();
	pl->viewheight = playerGetViewportHeight();
	pl->viewleft = playerGetViewportLeft();
	pl->viewtop = playerGetViewportTop();
	g_Vars.currentplayernum = saved_pnum;

	// Resolve the panel's pose. PLAYER/SIM read from the target; FREECAM/
	// TOPDOWN use the panel-local pose updated by spectatorReadInput. If the
	// target can't be resolved (no clients joined, sim despawned, etc.), fall
	// back to a sensible freecam spawn so the BSP renderer doesn't walk from
	// (0,0,0) into garbage rooms.
	struct coord pos = p->pos;
	struct coord look = p->look;
	struct coord up = p->up;
	s32 room = (p->room < 0) ? 1 : p->room;
	bool need_spawn_seed = false;
	struct chrdata *target_chr = NULL;
	if (SPEC_MODE_IS_TARGET(p->mode)) {
		target_chr = spectatorResolveTargetChr(p);
		if (target_chr) {
			spectatorReadTargetPose(p, target_chr, &pos, &look, &up, &room);
		} else {
			// Target gone (or never existed — e.g. panel 1 defaults to
			// netclient #1 but only the host is connected). Seed pose if we
			// haven't already, otherwise hold the last-known pose so the
			// view doesn't twitch between modes.
			need_spawn_seed = (p->pos.x == 0.f && p->pos.y == 0.f && p->pos.z == 0.f);
		}
	} else if (p->mode == SPEC_MODE_TOPDOWN && p->room < 0) {
		// First tick of topdown: jump high above whatever spawn we can find.
		spectatorPickSpawnPos(&pos, &room);
		pos.y += SPEC_TOPDOWN_ALTITUDE;
		p->pos = pos;
		p->room = room;
	} else if ((p->mode == SPEC_MODE_FREECAM) && p->room < 0) {
		need_spawn_seed = true;
	}

	if (need_spawn_seed) {
		spectatorPickSpawnPos(&pos, &room);
		p->pos = pos;
		p->room = room;
	}

	// Cache room for the next tick so the BSP renderer can start its portal
	// walk from a populated room. For free/topdown cams we keep whichever
	// room playerSetCamPropertiesWithoutRoom picks (it walks the portal
	// graph from `room`) — see also /octree bigroom (g_BgNoCull).
	p->pos = pos;

	pl->cam_pos = pos;
	pl->cam_look = look;
	pl->cam_up = up;
	pl->cam_room = room;
	playerSetCamPropertiesWithoutRoom(&pos, &up, &look, room);
	// Don't call playerAllocateMatrices here — it reads camGetMtxF1754()
	// (the perspective matrix) which is only valid AFTER vi0000b1d0 has run,
	// and that happens inside lvRender. Allocation is deferred to
	// spectatorRenderPanel; this tick just records the desired pose.

	p->room = pl->cam_room;

	// Phase B (experimental): mirror the target's held weapon onto the
	// panel's struct player so bgunRender (called in spectatorRenderPanel)
	// has something to draw. bgun normally runs on a real player with
	// playerTick + bondmove ticking each frame; we short-circuit by
	// brute-forcing the load state and directly setting the hand fields
	// that bgunTickSwitch2 would set if its bgunCanFreeWeapon gate let it
	// through (the gate requires a mid-CHANGEGUN animation our idle
	// spectator panel never produces).
	//
	// Known limitation: sims (PROPTYPE_CHR) don't have a bgun on their
	// own machine, so this only meaningfully works for spectated remote
	// PLAYERS. Sim FP currently renders without a weapon — see the bgun
	// notes in PORT_NET_KNOWN_ISSUES.md.
	if (SPEC_MODE_IS_FP(p->mode) && target_chr) {
		struct prop *wprop = target_chr->weapons_held[HAND_RIGHT];
		s32 target_weapon = (wprop && wprop->weapon) ? wprop->weapon->weaponnum : WEAPON_UNARMED;

		if (!p->bgun_inited) {
			bgunInitHandAnims();
			p->bgun_inited = 1;
			p->bgun_weaponnum = -1;
		}
		if ((s32)p->bgun_weaponnum != target_weapon) {
			// GUNLOADSTATE_FLUX / MASTERLOADSTATE_FLUX are 0 (private
			// #defines inside bondgun.c — not in any header).
			pl->gunctrl.gunmemowner = GUNMEMOWNER_BONDGUN;
			pl->gunctrl.gunmemtype = -1;
			pl->gunctrl.gunmemnew = target_weapon;
			pl->gunctrl.gunlocktimer = 0;
			pl->gunctrl.masterloadstate = 0;
			pl->gunctrl.gunloadstate = 0;

			// Drain the masterload loop synchronously so bgunIsLoaded()
			// returns true on this frame's render. Capped to avoid infinite
			// loop if the loader can't converge on missing state.
			for (s32 i = 0; i < 64 && !bgunIsLoaded(); i++) {
				bgunTickMasterLoad();
			}

			// Directly set the hand / gunctrl fields that bgunTickSwitch2
			// would set, bypassing its bgunCanFreeWeapon gate (which
			// requires a mid-CHANGEGUN animation — our idle panel never
			// satisfies it, so the gated path leaves hand->inuse = false
			// and bgunRender sets hand->visible = false).
			pl->gunctrl.weaponnum = target_weapon;
			pl->gunctrl.switchtoweaponnum = -1;
			pl->hands[HAND_RIGHT].inuse = true;
			pl->hands[HAND_LEFT].inuse = (target_weapon == WEAPON_REMOTEMINE || pl->gunctrl.dualwielding);
			for (s32 h = 0; h < 2; h++) {
				pl->hands[h].gset.weaponnum = target_weapon;
				pl->hands[h].gset.weaponfunc = FUNC_PRIMARY;
				pl->hands[h].state = HANDSTATE_IDLE;
				pl->hands[h].mode = 0;
			}

			p->bgun_weaponnum = (s8)target_weapon;

			netDiagLogf("spec_bgun_equip",
				"panel=%d weapon=%d owner=%d type=%d new=%d masterload=%d loaded=%d inuse_r=%d",
				(s32)pl->spectator_panel, target_weapon,
				(s32)pl->gunctrl.gunmemowner, (s32)pl->gunctrl.gunmemtype,
				(s32)pl->gunctrl.gunmemnew, (s32)pl->gunctrl.masterloadstate,
				(s32)bgunIsLoaded(),
				(s32)pl->hands[HAND_RIGHT].inuse);
		}
	}
}

// Minimum-viable per-panel render. Mirrors the structure of the normal
// per-player block in lvRender (framebuffer/scissor setup -> sky -> bg ->
// props) but skips everything that depends on the player having an mpchr:
// HUD, gun draw, eyespy, lookingatprop, drug-blur, interact/reload, etc.
// The matrices were already allocated in spectatorTickPanel via
// playerAllocateMatrices, so bgRender / propsTickPlayer use the panel's cam.
Gfx *spectatorRenderPanel(Gfx *gdl)
{
	struct player *pl = g_Vars.currentplayer;
	if (!pl || !pl->is_spectator) {
		return gdl;
	}

	// First-person spectate: hide the target chr's own body model so we
	// don't see triangles of their head/torso clipping into the cam frustum.
	// Set the global hide-prop before any prop/chr rendering kicks in, clear
	// it before returning. Only one panel renders at a time so the global
	// scopes correctly to this panel's pass.
	struct spectatorpanel *panel = (pl->spectator_panel < SPEC_MAX_PANELS)
			? &g_SpectatorPanels[pl->spectator_panel] : NULL;
	g_SpectatorHideChrProp = NULL;
	if (panel && SPEC_MODE_IS_FP(panel->mode)) {
		struct chrdata *tgt = spectatorResolveTargetChr(panel);
		if (tgt && tgt->prop) {
			g_SpectatorHideChrProp = tgt->prop;
		}
	}

	gSPDisplayList(gdl++, var800613a0);
	gSPDisplayList(gdl++, var80061380);

	viSetViewPosition(pl->viewleft, pl->viewtop);
	viSetFovAspectAndSize(pl->fovy, pl->aspect, pl->viewwidth, pl->viewheight);
	mtx00016748(bgGetScaleBg2Gfx());
	zbufSwap();
	gdl = viPrepareZbuf(gdl);
	// vi0000b1d0 builds the perspective matrix and stashes it via
	// camSetMtxF1754 — playerAllocateMatrices reads that for the orthogonal
	// matrix product, so it MUST run before the matrices below. Skipping
	// this is how an earlier iteration crashed in skyRender (NULL projection).
	gdl = vi0000b1d0(gdl);
	gdl = viRenderViewportEdges(gdl);
	gdl = bgScissorToViewport(gdl);

	// Allocate the per-frame matrices for this panel's camera. Deferred from
	// spectatorTickPanel because vi0000b1d0 above must run first.
	playerAllocateMatrices(&pl->cam_pos, &pl->cam_look, &pl->cam_up);

	// Once-per-frame global state advancement. lvRender's normal per-player
	// loop fires these every iteration (4× for 4-player split), which is fine
	// for local-only play but breaks netplay timing on the spectator host:
	// botTick (called transitively from propsTickPlayer) runs sim AI at the
	// panel-count rate, so a 4-panel host advances sims 4× per frame while
	// clients only tick once — sim positions broadcast via SVC_PROP_MOVE then
	// arrive faster than clients can apply them, producing visible desync.
	//
	// Gate to panel 0. spectatorReadInput reorders g_Vars.playerorder every
	// frame so panel 0 is the FIRST slot iterated by lvRender — that gives
	// it g_Vars.currentplayerindex == 0, which is also the gate several
	// of these calls (bgTick→bgTickRooms, propsTickPlayer's first-frame
	// setup) key their once-per-frame work off. Without the reorder, the
	// combatant claims index 0 and gets skipped, leaving no panel with
	// index 0 — sims would freeze on the host.
	const bool isFirstPanel = (pl->spectator_panel == 0);
	if (isFirstPanel) {
		envTick();
		artifactsClear();
	}

	gdl = skyRender(gdl);
	if (isFirstPanel) {
		bgTick();
		lightsTick();
		// islastplayer=true so the once-per-frame updateframe /
		// runstateindex bookkeeping still completes.
		propsTickPlayer(true);
		propsSort();
	}

	gdl = bgRender(gdl);
	gdl = propsRenderBeams(gdl);
	gdl = shardsRender(gdl);
	gdl = sparksRender(gdl);
	gdl = weatherRender(gdl);

	// Phase B: in first-person spectate, render the panel's bgun overlay so
	// you see the target's weapon model. spectatorTickPanel already mirrored
	// their held weapon onto the panel's struct player via bgunEquipWeapon2,
	// so currentplayer->hands has a valid gunmodel. bgunRender uses
	// currentplayer-relative state for everything (viewport, matrices, hand
	// pose), so we don't need to touch anything else — just call it.
	if (panel && SPEC_MODE_IS_FP(panel->mode) && panel->bgun_inited && g_SpectatorHideChrProp) {
		bgunRender(&gdl);
	}

	if (isFirstPanel) {
		artifactsTick();
	}
	g_SpectatorHideChrProp = NULL;
	return gdl;
}
