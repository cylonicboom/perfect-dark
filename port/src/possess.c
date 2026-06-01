/**
 * Lua "possession" — let the player take control of a spawned entity (a cube)
 * and fly it around with its own camera, then return to their body. v1 scope:
 *
 *   - Solo / missions only (refused in Combat Sim and on net clients).
 *   - Free-fly (no gravity), 6-DOF noclip movement driven by the gamepad/KBM,
 *     modelled directly on the spectator freecam (port/src/spectator.c).
 *   - A visible "cube" prop is spawned at the fly pose so you can see your body;
 *     the camera follows it. The real player body is frozen (control suppressed)
 *     while possessing and restored on exit.
 *
 * Backs pd.possess_spawn() / pd.unpossess() (see luaai_api.c). The per-frame
 * driver luaPossessReadInput() + camera apply luaPossessApplyCamera() are called
 * from the port frame loop (pdmain.c) next to the spectator hooks.
 *
 * This is the most experimental toolkit feature; everything here is port-only.
 */

#include <math.h>
#include "platform.h"
#include "types.h"
#include "constants.h"
#include "data.h"
#include "bss.h"
#include "input.h"
#include "lib/joy.h"        /* joyGetStickX/Buttons + R_TRIG/U_JPAD/START_BUTTON (via ultra64) */
#include "game/player.h"
#include "game/luaai.h"

#ifndef PLATFORM_N64

// Tunables (mirrors the spectator freecam feel).
#define POSSESS_MOVE_SPEED 30.f
#define POSSESS_BOOST_MULT 4.f
#define POSSESS_TURN_SPEED 2.5f
#define POSSESS_VERT_SPEED 25.f
#define POSSESS_PITCH_LIMIT 1.45f // just under pi/2

// Raw SDL scancodes for WASD (the VK_ enum in input.h only names a few keys;
// these match spectator.c's SPEC_VK_* values).
#define POSSESS_VK_W 26
#define POSSESS_VK_A 4
#define POSSESS_VK_S 22
#define POSSESS_VK_D 7

struct possessstate {
	s32 active;
	s32 chrnum;        // the spawned cube's chrnum (-1 if a bare freecam)
	f32 yaw;
	f32 pitch;
	struct coord pos;
	struct coord look;
	struct coord up;
	s32 savedctrl;     // g_PlayersWithControl[0] saved across possession
};

static struct possessstate g_Possess;

s32 luaPossessIsActive(void)
{
	return g_Possess.active;
}

// Begin possessing: seed the fly pose from the player's current camera, freeze
// the player's control, and (caller) optionally spawn a cube at the pose.
// Returns 1 on success. Solo-only + server-side gating is enforced by the
// bridge in chraction.c before this is reached, but we re-check defensively.
s32 luaPossessBegin(s32 cube_chrnum)
{
	struct player *pl = g_Vars.players ? g_Vars.players[0] : NULL;

	if (g_NetMode == NETMODE_CLIENT || g_Vars.normmplayerisrunning) {
		return 0;
	}
	if (pl == NULL || pl->prop == NULL) {
		return 0;
	}

	g_Possess.active = 1;
	g_Possess.chrnum = cube_chrnum;

	// Seed pose at the player's eye, looking along their current view.
	g_Possess.pos = pl->cam_pos;
	g_Possess.yaw = 0.f;
	g_Possess.pitch = 0.f;
	g_Possess.look.x = 0.f; g_Possess.look.y = 0.f; g_Possess.look.z = 1.f;
	g_Possess.up.x = 0.f; g_Possess.up.y = 1.f; g_Possess.up.z = 0.f;

	// Freeze the player body's control while we fly the cube.
	g_Possess.savedctrl = g_PlayersWithControl[0];
	g_PlayersWithControl[0] = false;
	return 1;
}

void luaPossessEnd(void)
{
	if (!g_Possess.active) {
		return;
	}
	g_PlayersWithControl[0] = g_Possess.savedctrl;
	g_Possess.active = 0;
	g_Possess.chrnum = -1;
	// Restore the player's own camera.
	playerSetCameraMode(CAMERAMODE_DEFAULT);
}

s32 luaPossessGetChrNum(void)
{
	return g_Possess.active ? g_Possess.chrnum : -1;
}

static f32 possessStick(s32 raw)
{
	// Normalise a joystick axis [-80,80]-ish to [-1,1] with a small deadzone.
	f32 v = (f32)raw / 80.f;
	if (v > 1.f) v = 1.f;
	if (v < -1.f) v = -1.f;
	if (v > -0.1f && v < 0.1f) v = 0.f;
	return v;
}

// Per-frame: read input and integrate the fly pose. Called once per frame from
// pdmain.c (next to spectatorReadInput). No-op unless possession is active.
void luaPossessReadInput(void)
{
	f32 lx, ly, rx, ry, alt, boost;
	f32 cy, sy, cp, sp;
	f32 fwdx, fwdz, rgtx, rgtz;
	u32 held;

	if (!g_Possess.active) {
		return;
	}

	// Exit on START / ESC (the body's pause is suppressed while possessing).
	held = joyGetButtons(0, 0xffffffff);
	if ((held & START_BUTTON) || inputKeyJustPressed(VK_ESCAPE)) {
		luaPossessEnd();
		return;
	}

	lx = possessStick(joyGetStickX(0));
	ly = possessStick(joyGetStickY(0));
	rx = possessStick(joyGetRStickX(0));
	ry = possessStick(joyGetRStickY(0));

	// Keyboard WASD + altitude, summed with stick so both work.
	if (inputKeyPressed(POSSESS_VK_D)) lx += 1.f;
	if (inputKeyPressed(POSSESS_VK_A)) lx -= 1.f;
	if (inputKeyPressed(POSSESS_VK_W)) ly += 1.f;
	if (inputKeyPressed(POSSESS_VK_S)) ly -= 1.f;

	boost = ((held & R_TRIG) || inputKeyPressed(VK_LSHIFT)) ? POSSESS_BOOST_MULT : 1.f;

	alt = 0.f;
	if (held & U_JPAD) alt += 1.f;
	if (held & D_JPAD) alt -= 1.f;
	if (inputKeyPressed(VK_SPACE)) alt += 1.f;
	if (inputKeyPressed(VK_LCTRL)) alt -= 1.f;

	// Mouse look when locked.
	{
		f32 mdx = 0.f, mdy = 0.f;
		if (inputMouseIsLocked()) {
			inputMouseGetScaledDelta(&mdx, &mdy);
		}
		g_Possess.yaw -= mdx * 0.0025f;
		g_Possess.pitch -= mdy * 0.0025f;
	}

	g_Possess.yaw -= rx * POSSESS_TURN_SPEED * 0.05f;
	g_Possess.pitch += ry * POSSESS_TURN_SPEED * 0.05f;
	if (g_Possess.pitch > POSSESS_PITCH_LIMIT) g_Possess.pitch = POSSESS_PITCH_LIMIT;
	if (g_Possess.pitch < -POSSESS_PITCH_LIMIT) g_Possess.pitch = -POSSESS_PITCH_LIMIT;

	cy = cosf(g_Possess.yaw); sy = sinf(g_Possess.yaw);
	cp = cosf(g_Possess.pitch); sp = sinf(g_Possess.pitch);

	g_Possess.look.x = sy * cp;
	g_Possess.look.y = sp;
	g_Possess.look.z = cy * cp;
	g_Possess.up.x = 0.f; g_Possess.up.y = 1.f; g_Possess.up.z = 0.f;

	// Pan in the cam's horizontal plane (yaw-only forward). Right vector is
	// negated vs. a naive perpendicular to match PD's coordinate handedness --
	// without this, left/right strafe is inverted.
	fwdx = sy; fwdz = cy;
	rgtx = -cy; rgtz = sy;
	g_Possess.pos.x += (fwdx * ly + rgtx * lx) * POSSESS_MOVE_SPEED * boost;
	g_Possess.pos.z += (fwdz * ly + rgtz * lx) * POSSESS_MOVE_SPEED * boost;
	g_Possess.pos.y += alt * POSSESS_VERT_SPEED * boost;

	// Keep the visible cube prop sitting at the fly pose, if one was spawned.
	if (g_Possess.chrnum >= 0) {
		chraiLuaSetChrPos(g_Possess.chrnum, g_Possess.pos.x, g_Possess.pos.y, g_Possess.pos.z);
	}
}

// Third-person camera offset behind + above the cube, so you SEE the cube you're
// driving. The cube sits at g_Possess.pos; the camera is pulled back along the
// (negated) look vector and lifted a little.
#define POSSESS_CAM_BACK 140.f
#define POSSESS_CAM_UP   60.f

// Point the active player's render camera at the fly pose. Called from the
// per-player render/tick path when possession is active.
void luaPossessApplyCamera(void)
{
	struct player *pl;
	struct coord campos;

	if (!g_Possess.active) {
		return;
	}
	pl = g_Vars.currentplayer;
	if (pl == NULL) {
		return;
	}

	// Camera = cube position - look*back + up*lift. Look stays aimed forward, so
	// the cube is framed ahead-and-below centre.
	campos.x = g_Possess.pos.x - g_Possess.look.x * POSSESS_CAM_BACK;
	campos.y = g_Possess.pos.y - g_Possess.look.y * POSSESS_CAM_BACK + POSSESS_CAM_UP;
	campos.z = g_Possess.pos.z - g_Possess.look.z * POSSESS_CAM_BACK;

	pl->cam_pos = campos;
	pl->cam_look = g_Possess.look;
	pl->cam_up = g_Possess.up;
	playerSetCamPropertiesWithoutRoom(&campos, &g_Possess.up, &g_Possess.look, pl->cam_room);
	playerAllocateMatrices(&pl->cam_pos, &pl->cam_look, &pl->cam_up);
}

#endif
