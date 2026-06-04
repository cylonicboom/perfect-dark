#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include "platform.h"
#include "types.h"
#include "constants.h"
#include "data.h"
#include "bss.h"
#include "config.h"
#include "video.h"
#include "input.h"
#include "console.h"
#include "system.h"
#include "utils.h"
#include "net/net.h"
#include "game/hudmsg.h"
#include "game/game_1531a0.h"
#include "lib/vi.h"

#define CON_ROWS 80
#define CON_COLS 56
#define CON_VISROWS 10
#define CON_MSGROWS 4
#define CON_MSGTIMER 3.f

static char conBuf[CON_ROWS][CON_COLS + 1];
static char conInput[CON_COLS + 1];
static char *conVisRows[CON_VISROWS];
static s32 conPrintRow = 0;
static s32 conPrintCol = 0;
static s32 conMsgRows = 0;
static f32 conMsgTimer = 0.f;
static s32 conInputCol = 0;
static u32 conTextColour = 0x00ff00ff;
static s32 conOpen = 0;
static s32 conButton = 0;
// Console.ShowMessages: when 0 (default), log/chat lines do NOT flash on screen
// while the console is closed — boot and play stay clean (good for streaming).
// The dev console (~) still opens and shows full scrollback either way; only the
// transient closed-console overlay (conRenderMsgs) is gated. Set to 1 to restore
// the on-screen message popups.
static s32 conShowMsgs = 0;

PD_CONSTRUCTOR static void consoleConfigInit(void)
{
	configRegisterInt("Console.ShowMessages", &conShowMsgs, 0, 1);
}
// Scrollback offset in rows. 0 = pinned to the live tail (newest line at the
// bottom). Positive values pan back through the ring buffer. PageUp/PageDown
// move it by CON_SCROLLSTEP. Capped so we never wrap past the oldest valid
// row of the ring.
#define CON_SCROLLSTEP (CON_VISROWS / 2)
#define CON_SCROLLMAX  (CON_ROWS - CON_VISROWS)
static s32 conScrollOfs = 0;

// Recompute conVisRows from the current conScrollOfs. vis[0] is the newest
// visible completed line; vis[CON_VISROWS-1] is the oldest. The row currently
// being written into (conPrintRow itself) is excluded since the prompt line
// already shows in-progress input.
static void conRebuildVisRows(void)
{
	for (s32 i = 0; i < CON_VISROWS; ++i) {
		s32 row = conPrintRow - i - 1 - conScrollOfs;
		while (row < 0) {
			row += CON_ROWS;
		}
		conVisRows[i] = &conBuf[row][0];
	}
}

void conInit(void)
{
	memset(conBuf, 0, sizeof(conBuf));
	memset(conInput, 0, sizeof(conInput));
	memset(conVisRows, 0, sizeof(conVisRows));
	conPrintRow = conPrintCol = 0;
	conOpen = 0;
	conMsgRows = 0;
	conMsgTimer = 0.0f;
	conInputCol = 0;
	conScrollOfs = 0;
}

void conPrint(s32 showmsg, const char *str)
{
	if (!str || !*str) {
		return;
	}

	const s32 oldRow = conPrintRow;

	for (const char *s = str; *s; ++s) {
		char ch = *s;
		switch (ch) {
			case '\n':
				if (conPrintCol) {
					conPrintCol = 0;
					conPrintRow = (conPrintRow + 1) % CON_ROWS;
				}
				break;
			case '\r':
				break;
			case '\t':
				ch = ' ';
				/* fallthrough */
			default:
				// Non-ASCII bytes route to the JPN multibyte font path in
				// textRenderProjected (fontjpn segment is unloaded on non-JPN
				// ROMs), which crashes. The console is ASCII-only; strip them
				// to '?'. Mirrors the F9 net-overlay strip in net.c.
				if ((u8)ch >= 0x80) {
					ch = '?';
				}
				conBuf[conPrintRow][conPrintCol++] = ch;
				if (conPrintCol == CON_COLS) {
					conPrintCol = 0;
					conPrintRow = (conPrintRow + 1) % CON_ROWS;
				}
				break;
		}
		conBuf[conPrintRow][conPrintCol] = '\0';
	}

	if (conPrintRow != oldRow) {
		const s32 advanced = (conPrintRow > oldRow)
			? (conPrintRow - oldRow)
			: (conPrintRow + (CON_ROWS - oldRow));

		// Keep the user's scrolled view anchored to the same absolute rows
		// while new lines pile on at the live tail. When pinned to the tail
		// (conScrollOfs == 0) we naturally show the new lines instead.
		if (conScrollOfs > 0) {
			conScrollOfs += advanced;
			if (conScrollOfs > CON_SCROLLMAX) {
				conScrollOfs = CON_SCROLLMAX;
			}
		}

		conRebuildVisRows();

		if (showmsg) {
			if (conMsgRows < CON_MSGROWS) {
				conMsgRows += advanced;
			}
			conMsgTimer = sysGetSeconds() + CON_MSGTIMER;
		}
	}
}

void conPrintLn(s32 showmsg, const char *s)
{
	char tmp[4096];
	snprintf(tmp, sizeof(tmp), "%s\n", s);
	conPrint(showmsg, tmp);
}

void conPrintf(s32 showmsg, const char *fmt, ...)
{
	char tmp[4096];
	tmp[sizeof(tmp) - 1] = '\0';
	tmp[0] = '\0';

	va_list args;
	va_start(args, fmt);
	vsnprintf(tmp, sizeof(tmp), fmt, args);
	va_end(args);

	conPrint(showmsg, tmp);
}

static inline Gfx *conRenderMsgs(Gfx *gdl)
{
	if (!conShowMsgs) {
		// Overlay disabled: don't draw the transient popups, and drop any queued
		// rows so they can't appear if it's re-enabled mid-session.
		conMsgRows = 0;
		conMsgTimer = 0;
		return gdl;
	}

	if (conMsgRows) {
		s32 x, y;
		const u32 c = (conTextColour & 0xffffff00) | 0xa0;
		for (s32 i = 0; i < conMsgRows; ++i) {
			char *s = conVisRows[i];
			if (s) {
				x = 4;
				y = 4 + 8 * (conMsgRows - i - 1);
				gdl = textRenderProjected(gdl, &x, &y, s, g_CharsHandelGothicXs, g_FontHandelGothicXs, c, viGetWidth(), viGetHeight(), 0, 0);
			}
		}
		const f32 t = sysGetSeconds();
		if (conMsgTimer <= t) {
			conMsgTimer = t + CON_MSGTIMER;
			--conMsgRows;
		}
	} else {
		conMsgTimer = 0;
	}

	return gdl;
}

Gfx *conRender(Gfx *gdl)
{
	if (!g_CharsHandelGothicXs || !g_FontHandelGothicXs) {
		return gdl;
	}

	gdl = text0f153628(gdl);

	if (!conOpen) {
		gSPExtraGeometryModeEXT(gdl++, G_ASPECT_MODE_EXT, G_ASPECT_LEFT_EXT);
		gdl = conRenderMsgs(gdl);
	} else {
		s32 x, y;
		gSPExtraGeometryModeEXT(gdl++, G_ASPECT_MODE_EXT, G_ASPECT_CENTER_EXT);
		gdl = hudmsgRenderBox(gdl, 16, 0, SCREEN_WIDTH_LO - 16, 4 + 8 * (CON_VISROWS + 1), 1.f, conTextColour, 0.9f);
		for (s32 i = 0; i < CON_VISROWS; ++i) {
			char *s = conVisRows[i];
			if (s) {
				x = 18;
				y = 4 + 8 * (CON_VISROWS - i - 1);
				gdl = textRenderProjected(gdl, &x, &y, s, g_CharsHandelGothicXs, g_FontHandelGothicXs, conTextColour, viGetWidth(), viGetHeight(), 0, 0);
			}
		}
		char tmp[CON_COLS + 24];
		if (conScrollOfs > 0) {
			// Indicate scrollback position so the user remembers PgDn/End
			// puts them back at live output.
			snprintf(tmp, sizeof(tmp), "[-%d] > %s", conScrollOfs, conInput);
		} else {
			snprintf(tmp, sizeof(tmp), "> %s", conInput);
		}
		// conInput may hold non-ASCII bytes (SDL text input / paste); strip
		// them so the prompt line doesn't hit the JPN font path either.
		for (char *t = tmp; *t; ++t) {
			if ((u8)*t >= 0x80) {
				*t = '?';
			}
		}
		x = 18;
		y = 4 + 8 * CON_VISROWS;
		gdl = textRenderProjected(gdl, &x, &y, tmp, g_CharsHandelGothicXs, g_FontHandelGothicXs, conTextColour, viGetWidth(), viGetHeight(), 0, 0);
	}

	gSPClearExtraGeometryModeEXT(gdl++, G_ASPECT_MODE_EXT);

	gdl = text0f153780(gdl);

	return gdl;
}

void conTick(void)
{
	const s32 button = inputKeyPressed(VK_GRAVE);
	if (button && !conButton) {
		conOpen = !conOpen;
		g_MenuKeyboardPlayer = -1;
		if (conOpen) {
			inputClearLastTextChar();
			inputStartTextInput();
		} else {
			inputStopTextInput();
			// Snap back to the live tail when closing so the next open
			// shows the latest output instead of resuming a stale view.
			if (conScrollOfs != 0) {
				conScrollOfs = 0;
				conRebuildVisRows();
			}
		}
	}

	conButton = button;

	if (conOpen) {
		// PageUp/PageDown scroll the buffer. Edge-triggered so a held key
		// doesn't fly through the ring; the user can tap to step. Home/End
		// jump to the top of the scrollback / live tail respectively.
		s32 newOfs = conScrollOfs;
		if (inputKeyJustPressed(VK_PAGEUP)) {
			newOfs += CON_SCROLLSTEP;
		}
		if (inputKeyJustPressed(VK_PAGEDOWN)) {
			newOfs -= CON_SCROLLSTEP;
		}
		if (inputKeyJustPressed(VK_HOME)) {
			newOfs = CON_SCROLLMAX;
		}
		if (inputKeyJustPressed(VK_END)) {
			newOfs = 0;
		}
		if (newOfs < 0) newOfs = 0;
		if (newOfs > CON_SCROLLMAX) newOfs = CON_SCROLLMAX;
		if (newOfs != conScrollOfs) {
			conScrollOfs = newOfs;
			conRebuildVisRows();
		}

		if (inputTextHandler(conInput, CON_COLS, &conInputCol, false)) {
			// Lines starting with '/' are local netplay/debug commands,
			// not chat. Handled even outside a net session so the user can
			// pre-configure things like /lag before connecting.
			if (conInput[0] == '/') {
				netConsoleCommand(conInput);
			} else if (g_NetMode) {
				netChat(NULL, conInput);
			}
			conInput[0] = '\0';
			conInputCol = 0;
			// Submitting a command implies the user wants to see its output;
			// jump back to the live tail.
			if (conScrollOfs != 0) {
				conScrollOfs = 0;
				conRebuildVisRows();
			}
		}
	}
}

s32 conIsOpen(void)
{
	return conOpen;
}
