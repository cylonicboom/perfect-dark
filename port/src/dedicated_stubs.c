// Dedicated-server build stubs.
//
// The headless dedicated server (-DDEDICATED_SERVER=ON) excludes the SDL input
// layer (port/src/input.c) from the build entirely — it never reads a keyboard,
// mouse or game controller. The rest of the engine still references the input
// API, so this file provides no-op implementations that satisfy the linker and
// return benign defaults. At runtime the dedicated host is a non-combatant
// spectator with no local player, so none of these meaningfully affect play;
// inputReadController in particular reports "no controller" and zeroes the pad.
//
// Compiled only when DEDICATED_SERVER is defined.

#include <string.h>
#include <PR/ultratypes.h>
#include <PR/os_thread.h>
#include <PR/os_cont.h>
#include "platform.h"
#include "input.h"

s32 inputInit(void) { return 0; }

s32 inputReadController(s32 idx, OSContPad *npad)
{
	(void)idx;
	if (npad) {
		memset(npad, 0, sizeof(*npad));
	}
	return 1; // non-zero: no controller read
}

s32 inputRumbleSupported(s32 idx) { (void)idx; return 0; }
s32 inputControllerConnected(s32 idx) { (void)idx; return 0; }
s32 inputControllerMask(void) { return 0; }

s32 inputControllerGetSticksSwapped(s32 cidx) { (void)cidx; return 0; }
void inputControllerSetSticksSwapped(s32 cidx, s32 swapped) { (void)cidx; (void)swapped; }

s32 inputControllerGetDualAnalog(s32 cidx) { (void)cidx; return 0; }
void inputControllerSetDualAnalog(s32 cidx, s32 enable) { (void)cidx; (void)enable; }

s32 inputControllerGetCancelCButtons(s32 cidx) { (void)cidx; return 0; }
void inputControllerSetCancelCButtons(s32 cidx, s32 cancel) { (void)cidx; (void)cancel; }

f32 inputControllerGetAxisScale(s32 cidx, s32 stick, s32 axis) { (void)cidx; (void)stick; (void)axis; return 1.0f; }
void inputControllerSetAxisScale(s32 cidx, s32 stick, s32 axis, f32 value) { (void)cidx; (void)stick; (void)axis; (void)value; }

f32 inputControllerGetAxisDeadzone(s32 cidx, s32 stick, s32 axis) { (void)cidx; (void)stick; (void)axis; return 0.0f; }
void inputControllerSetAxisDeadzone(s32 cidx, s32 stick, s32 axis, f32 value) { (void)cidx; (void)stick; (void)axis; (void)value; }

s32 inputGetConnectedControllers(s32 *out) { (void)out; return 0; }
const char *inputGetConnectedControllerName(s32 id) { (void)id; return "Invalid"; }
s32 inputGetAssignedControllerId(s32 cidx) { (void)cidx; return -1; }
s32 inputAssignController(s32 cidx, s32 id) { (void)cidx; (void)id; return 0; }

s32 inputKeyPressed(u32 vk) { (void)vk; return 0; }
s32 inputKeyJustPressed(u32 vk) { (void)vk; return 0; }
s32 inputButtonPressed(s32 idx, u32 contbtn) { (void)idx; (void)contbtn; return 0; }

void inputKeyBind(s32 idx, u32 ck, s32 bind, u32 vk) { (void)idx; (void)ck; (void)bind; (void)vk; }
const u32 *inputKeyGetBinds(s32 idx, u32 ck) { (void)idx; (void)ck; return NULL; }

s32 inputGetKeyByName(const char *name) { (void)name; return 0; }
const char *inputGetKeyName(s32 vk) { (void)vk; return ""; }
s32 inputGetContKeyByName(const char *name) { (void)name; return 0; }
const char *inputGetContKeyName(u32 ck) { (void)ck; return ""; }

void inputRumble(s32 idx, f32 strength, f32 time) { (void)idx; (void)strength; (void)time; }
f32 inputRumbleGetStrength(s32 cidx) { (void)cidx; return 0.0f; }
void inputRumbleSetStrength(s32 cidx, f32 val) { (void)cidx; (void)val; }

void inputLockMouse(s32 lock) { (void)lock; }
s32 inputMouseIsLocked(void) { return 0; }

s32 inputMouseGetPosition(s32 *x, s32 *y)
{
	if (x) { *x = 0; }
	if (y) { *y = 0; }
	return 0;
}

void inputMouseGetRawDelta(s32 *dx, s32 *dy)
{
	if (dx) { *dx = 0; }
	if (dy) { *dy = 0; }
}

void inputMouseGetScaledDelta(f32 *dx, f32 *dy)
{
	if (dx) { *dx = 0.0f; }
	if (dy) { *dy = 0.0f; }
}

void inputMouseGetAbsScaledDelta(f32 *dx, f32 *dy)
{
	if (dx) { *dx = 0.0f; }
	if (dy) { *dy = 0.0f; }
}

void inputMouseGetSpeed(f32 *x, f32 *y)
{
	if (x) { *x = 0.0f; }
	if (y) { *y = 0.0f; }
}

void inputMouseSetSpeed(f32 x, f32 y) { (void)x; (void)y; }

s32 inputMouseIsEnabled(void) { return 0; }
void inputMouseEnable(s32 enabled) { (void)enabled; }

void inputUpdate(void) { }
void inputSaveBinds(void) { }
void inputSetDefaultKeyBinds(s32 cidx, s32 n64mode) { (void)cidx; (void)n64mode; }

void inputClearLastKey(void) { }
s32 inputGetLastKey(void) { return 0; }

s32 inputGetMouseLockMode(void) { return 0; }
void inputSetMouseLockMode(s32 lockmode) { (void)lockmode; }
s32 inputAutoLockMouse(s32 wantlock) { (void)wantlock; return 0; }
void inputMouseShowCursor(s32 show) { (void)show; }

void inputStartTextInput(void) { }
void inputStopTextInput(void) { }
s32 inputIsTextInputActive(void) { return 0; }

void inputClearLastTextChar(void) { }
char inputGetLastTextChar(void) { return 0; }

s32 inputTextHandler(char *out, const u32 outSize, s32 *curCol, s32 oskCharsOnly)
{
	(void)out;
	(void)outSize;
	(void)curCol;
	(void)oskCharsOnly;
	return 0;
}

void inputClearClipboard(void) { }
const char *inputGetClipboard(void) { return ""; }

u32 inputGetKeyModState(void) { return 0; }
