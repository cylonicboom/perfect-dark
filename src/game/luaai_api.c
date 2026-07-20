/**
 * Lua scripting API + dev overlay for the action-block runtime (port-only in
 * practice; the whole Lua layer is compiled into the port build only).
 *
 * This sits on top of luaai.c (which owns the lua_State and the ailist
 * transpile/execute loop) and adds the developer-facing surface:
 *
 *   pd.on(event, fn)            -- "weaponfire" | "alert" | "kill" | "draw"
 *   pd.draw_box(x,y,w,h,color[,secs])
 *   pd.draw_text(x,y,text,color[,secs])
 *   pd.each_chr(fn)             -- fn(chrnum, ailistid, aioffset, alertness, islua)
 *
 * Plus the C-side glue: a timed 2D overlay list rendered each frame, an event
 * registry + emitters called from game code (weapon fire / chr alert / kill),
 * and the per-frame X-ray sampling that proves every ailist is running through
 * the Lua exec loop.
 *
 * Coordinates are the lo-res virtual screen space (same as the console); colours
 * are 0xRRGGBBAA. All Lua calls go through lua_pcall so a broken script logs and
 * is skipped, never crashing the game.
 */

#include <ultra64.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "constants.h"
#include "types.h"
#include "game/luaai.h"
#include "game/game_1531a0.h" /* text0f153628 / text0f153780 / textRenderProjected */
#include "game/hudmsg.h"      /* hudmsgRenderBox */
#include "game/cheats.h"      /* cheatActivate/Deactivate/IsActive (pd.cheat, chaos mode) */
#include "game/bg.h"          /* g_BgOctreeStats (port-only octree cull counters) */
#include "game/training.h"    /* ciGet*Bio* (pd.bio_count / pd.bio_text — CI lore) */
#include "game/lang.h"        /* langGet (bio text ids -> strings) */
#include "game/menu.h"        /* func0f0f85e0 / menuPushDialog / menuPopDialog */
#include "game/mainmenu.h"    /* menuhandlerAcceptMission (Game Over restart) */
#include "game/endscreen.h"   /* endscreenMenuTitle* (real failed screen titles) */
#include "game/player.h"      /* playerSetFadeColour (game_over black screen) */
#include "game/music.h"       /* musicStartTrackAsMenu (game_over failed music) */
#include "game/tex.h"         /* texSelect (pd.draw_sprite blood-splat textures) */
#include "game/gfxmemory.h"   /* gfxAllocateVertices / gfxAllocateColours (draw_sprite) */
#ifndef PLATFORM_N64
#include "ext_tex.h"          /* extImageLoad / extImageFree (pd.load_image PNG hook) */
#endif
#include "lib/rng.h"          /* rngRandom (pd.menu_lore random bio pick) */
#include "data.h"             /* g_FontHandelGothicXs / g_CharsHandelGothicXs; g_MpPlayerNum */
#include "bss.h"              /* g_Vars / g_Menus (pd.menu_lore / pd.game_over gates) */
#include "lib/vi.h"           /* viGetWidth / viGetHeight */
#include "net/net.h"          /* g_NetMode / NETMODE_* for the AP gate server/solo guard */

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#ifndef PLATFORM_N64
#include "console.h"          /* conPrintf (port) */
#endif

/* ------------------------------------------------------------------------- *
 * State
 * ------------------------------------------------------------------------- */

#define LUA_MAX_OVERLAYS 96
#define LUA_MAX_XRAY     48
#define LUA_TEXT_MAX     56
/* LUA_MENU_MAX / LUA_MENU_LABEL are defined in game/luaai.h (shared with mainmenu.c). */

enum { OVL_BOX, OVL_TEXT, OVL_SPRITE, OVL_IMAGE };

struct luaoverlay {
	s32 kind;
	s32 x, y, w, h;
	u32 color;
	s32 texnum;     /* OVL_SPRITE: g_TexWallhitConfigs index. OVL_IMAGE: g_LuaImages index. */
	f32 angle;      /* OVL_IMAGE: rotation in radians (x,y = CENTRE when set). */
	char text[LUA_TEXT_MAX];
	s32 framesleft; /* >0 timed; one-frame entries use 1 + oneframe flag */
	s32 oneframe;
};

#ifndef PLATFORM_N64
/* External images loaded from scripts/images/ via pd.load_image. Each keeps
 * its own RGBA8888 buffer + a textureconfig pointing at it so pd.draw_image
 * can blit it through the same texrect path as the blood splats. This is a
 * loading HOOK for future effects — nothing in the shipped effects uses it. */
#define LUA_MAX_IMAGES 16
struct luaimage {
	u8 *data;   /* owned RGBA8888 (freed on stage reset) */
	u32 w, h;   /* real pixel dimensions */
	struct textureconfig config;
};
static struct luaimage g_LuaImages[LUA_MAX_IMAGES];
static s32 g_LuaImageCount = 0;
#endif

static struct luaoverlay g_LuaOverlays[LUA_MAX_OVERLAYS];
static s32 g_LuaOverlayCount = 0;

struct luaxray {
	s32 chrnum, ailistid, aioffset, alertness, islua;
};

static struct luaxray g_LuaXray[LUA_MAX_XRAY];
static s32 g_LuaXrayCount = 0;

/* Last-seen room of player 0, for synthesising the "roomenter" event in luaTick
 * (there is no single engine call site for it). -0x7fffffff = "unknown yet". */
static s32 g_LuaLastPlayerRoom = -0x7fffffff;

/* Director menu registry: scripts register pause-menu entries via pd.menu_add,
 * the Lua Director dialog (mainmenu.c) renders them and dispatches selection back
 * to the stored Lua function by index. */
struct luamenuentry {
	char label[LUA_MENU_LABEL];
	char group[LUA_MENU_LABEL]; /* "" = root; else the submenu title it lives under */
	int luaref; /* LUA_NOREF if unused */
};

static struct luamenuentry g_LuaMenu[LUA_MENU_MAX];
static s32 g_LuaMenuCount = 0;

/* registry table: event name -> array of handler functions */
static const char *const KEY_EVENTS = "luaai.events";

/* ------------------------------------------------------------------------- *
 * Logging (stderr + in-game console)
 * ------------------------------------------------------------------------- */

static void luaApiLog(const char *s)
{
	fprintf(stderr, "[luaai] %s\n", s);
#ifndef PLATFORM_N64
	conPrintf(1, "[lua] %s", s);
#endif
}

static void luaApiLog2(const char *prefix, const char *s)
{
	fprintf(stderr, "[luaai] %s%s\n", prefix, s ? s : "");
#ifndef PLATFORM_N64
	conPrintf(1, "[lua] %s%s", prefix, s ? s : "");
#endif
}

/* ------------------------------------------------------------------------- *
 * Overlay list
 * ------------------------------------------------------------------------- */

static void luaOverlayAdd(s32 kind, s32 x, s32 y, s32 w, s32 h, u32 color,
		const char *text, s32 texnum, f32 angle, f32 secs)
{
	struct luaoverlay *o;

	if (g_LuaOverlayCount >= LUA_MAX_OVERLAYS) {
		return; /* full: drop silently */
	}

	o = &g_LuaOverlays[g_LuaOverlayCount++];
	o->kind = kind;
	o->x = x;
	o->y = y;
	o->w = w;
	o->h = h;
	o->color = color;
	o->texnum = texnum;
	o->angle = angle;

	if (text) {
		strncpy(o->text, text, LUA_TEXT_MAX - 1);
		o->text[LUA_TEXT_MAX - 1] = '\0';
	} else {
		o->text[0] = '\0';
	}

	if (secs > 0.f) {
		o->oneframe = 0;
		o->framesleft = (s32)(secs * 60.f) + 1;
	} else {
		o->oneframe = 1;
		o->framesleft = 1;
	}
}

/* ------------------------------------------------------------------------- *
 * Event registry + dispatch
 * ------------------------------------------------------------------------- */

static void luaEventDispatchInts(const char *name, int argc, const lua_Integer *argv)
{
	lua_State *L = luaaiGetState();
	int i, a, n;

	if (!L) {
		return;
	}

	lua_getfield(L, LUA_REGISTRYINDEX, KEY_EVENTS); /* events */
	lua_getfield(L, -1, name);                      /* events[name] */

	if (lua_istable(L, -1)) {
		n = (int)lua_rawlen(L, -1);
		for (i = 1; i <= n; i++) {
			lua_rawgeti(L, -1, i); /* fn */
			if (lua_isfunction(L, -1)) {
				for (a = 0; a < argc; a++) {
					lua_pushinteger(L, argv[a]);
				}
				if (lua_pcall(L, argc, 0, 0) != LUA_OK) {
					luaApiLog2("event error: ", lua_tostring(L, -1));
					lua_pop(L, 1); /* error msg */
				}
			} else {
				lua_pop(L, 1); /* non-function entry */
			}
		}
	}

	lua_pop(L, 2); /* events[name] + events */
}

/* ------------------------------------------------------------------------- *
 * Lua-callable functions (registered onto the pd table)
 * ------------------------------------------------------------------------- */

/* pd.on(event, fn) */
static int l_pd_on(lua_State *L)
{
	const char *name = luaL_checkstring(L, 1);
	luaL_checktype(L, 2, LUA_TFUNCTION);

	lua_getfield(L, LUA_REGISTRYINDEX, KEY_EVENTS); /* events */
	lua_getfield(L, -1, name);                      /* events[name] */

	if (!lua_istable(L, -1)) {
		lua_pop(L, 1);             /* nil */
		lua_newtable(L);           /* new list */
		lua_pushvalue(L, -1);      /* dup list */
		lua_setfield(L, -3, name); /* events[name] = list */
	}

	/* stack: events, list */
	lua_pushvalue(L, 2);                                  /* fn */
	lua_rawseti(L, -2, (lua_Integer)lua_rawlen(L, -2) + 1); /* list[#+1] = fn */
	lua_pop(L, 2);                                        /* list + events */
	return 0;
}

/* pd.draw_box(x, y, w, h, color, [secs]) */
static int l_pd_draw_box(lua_State *L)
{
	s32 x = (s32)luaL_checkinteger(L, 1);
	s32 y = (s32)luaL_checkinteger(L, 2);
	s32 w = (s32)luaL_checkinteger(L, 3);
	s32 h = (s32)luaL_checkinteger(L, 4);
	u32 color = (u32)luaL_optinteger(L, 5, 0xffffffffu);
	f32 secs = (f32)luaL_optnumber(L, 6, 0.0);

	luaOverlayAdd(OVL_BOX, x, y, w, h, color, NULL, 0, 0.f, secs);
	return 0;
}

/* pd.draw_sprite(texnum, x, y, w, h, [color], [secs]). Paint a game wall-hit
 * texture (e.g. WALLHITTEX_BLOOD1..4 = 0x09..0x0c) as a HUD quad, tinted by
 * color (default black-opaque). Used by Blooper to splat black blood on the
 * screen. */
static int l_pd_draw_sprite(lua_State *L)
{
	s32 texnum = (s32)luaL_checkinteger(L, 1);
	s32 x = (s32)luaL_checkinteger(L, 2);
	s32 y = (s32)luaL_checkinteger(L, 3);
	s32 w = (s32)luaL_checkinteger(L, 4);
	s32 h = (s32)luaL_checkinteger(L, 5);
	u32 color = (u32)luaL_optinteger(L, 6, 0x000000ffu);
	f32 secs = (f32)luaL_optnumber(L, 7, 0.0);

	luaOverlayAdd(OVL_SPRITE, x, y, w, h, color, NULL, texnum, 0.f, secs);
	return 0;
}

/* pd.load_image(name) -> handle | nil. Load a PNG from scripts/images/<name>
 * (".png" appended if it has no extension) into an RGBA texture and return an
 * opaque handle for pd.draw_image. A LOADING HOOK for future effects — no
 * shipped effect uses it. Handles + buffers are freed on stage change.
 * SIZE LIMIT: reliable max is 64x64. Past 64 wide the single-tile load halves
 * the rows (top part shows doubled), so keep both dimensions <= 64 and
 * power-of-two. Oversize still loads but renders partial (a warning is logged).
 * To fill a bigger area, draw a 64x64 source at a larger w/h (draw_image scales). */
#ifndef PLATFORM_N64
/* pd.tex_override([name]): replace EVERY game texture's RGB with an external
 * image (scripts/images/<name>[.png]) via flat-texture mode 3 — the renderer
 * re-imports the whole texture cache through the override filter (per-pixel
 * alpha preserved, so cutouts/glyphs keep their shapes). No arg = restore
 * normal textures and free the image. */
static u8 *g_LuaTexOverride = NULL;

void luaTexOverrideReset(void)
{
	extern int gfx_flattex_mode;
	extern unsigned char *gfx_flattex_image;

	if (gfx_flattex_mode == 3) {
		gfx_flattex_mode = 0;
	}
	gfx_flattex_image = NULL;
	if (g_LuaTexOverride) {
		extImageFree(g_LuaTexOverride);
		g_LuaTexOverride = NULL;
	}
}

static int l_pd_tex_override(lua_State *L)
{
	extern int gfx_flattex_mode;
	extern unsigned char *gfx_flattex_image;
	extern int gfx_flattex_image_w;
	extern int gfx_flattex_image_h;
	const char *name = luaL_optstring(L, 1, NULL);
	char path[256];
	const char *dot;
	u8 *data;
	u32 w = 0, h = 0;

	luaTexOverrideReset();

	if (name == NULL) {
		lua_pushboolean(L, 1);
		return 1;
	}

	dot = strrchr(name, '.');
	snprintf(path, sizeof(path), "scripts/images/%s%s", name, dot ? "" : ".png");

	data = extImageLoad(path, &w, &h);
	if (!data || w == 0 || h == 0) {
		if (data) extImageFree(data);
		lua_pushboolean(L, 0);
		return 1;
	}

	g_LuaTexOverride = data;
	gfx_flattex_image = data;
	gfx_flattex_image_w = (int)w;
	gfx_flattex_image_h = (int)h;
	gfx_flattex_mode = 3; // mode change -> gfx_start_frame clears the texture cache
	lua_pushboolean(L, 1);
	return 1;
}
#else
static int l_pd_tex_override(lua_State *L)
{
	lua_pushboolean(L, 0);
	return 1;
}
#endif

static int l_pd_load_image(lua_State *L)
{
#ifndef PLATFORM_N64
	const char *name = luaL_checkstring(L, 1);
	char path[256];
	struct luaimage *im;
	u8 *data;
	u32 w = 0, h = 0;
	const char *dot;

	if (g_LuaImageCount >= LUA_MAX_IMAGES) {
		lua_pushnil(L);
		return 1;
	}

	dot = strrchr(name, '.');
	snprintf(path, sizeof(path), "scripts/images/%s%s", name, dot ? "" : ".png");

	data = extImageLoad(path, &w, &h);
	if (!data || w == 0 || h == 0) {
		if (data) extImageFree(data);
		lua_pushnil(L);
		return 1;
	}

	// Reliable max is 64x64: the single-tile load path halves the rows once
	// the width goes past 64 (the top part shows doubled). Warn but still load
	// so oversize is obvious in-game. Use power-of-two dimensions.
	if (w > 64 || h > 64) {
		char msg[192];
		snprintf(msg, sizeof(msg),
				"pd.load_image('%s'): %ux%u exceeds the reliable 64x64 max — "
				"it will render only partially (top rows doubled).",
				path, w, h);
		luaApiLog(msg);
	}

	// Convert RGBA8888 -> RGBA5551 (16-bit, big-endian) — the game's normal
	// texture format. The RGBA32 (G_IM_SIZ_32b) load path in the fast3d
	// renderer computes the wrong tile dimensions for raw configs (it derives
	// width from line_size_bytes/2, off by 2x), which stretched the image;
	// the RGBA16 path sizes correctly. Best on power-of-two dimensions.
	{
		u8 *rgba16 = (u8 *)malloc((size_t)w * h * 2);
		u32 i, n = w * h;
		if (!rgba16) {
			extImageFree(data);
			lua_pushnil(L);
			return 1;
		}
		for (i = 0; i < n; i++) {
			u8 r = data[i * 4 + 0], g = data[i * 4 + 1], b = data[i * 4 + 2], a = data[i * 4 + 3];
			u16 v = (u16)(((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | (a >> 7));
			rgba16[i * 2 + 0] = (u8)(v >> 8);
			rgba16[i * 2 + 1] = (u8)(v & 0xff);
		}
		extImageFree(data); // done with the 8888 source

		im = &g_LuaImages[g_LuaImageCount];
		im->data = rgba16;
		im->w = w;
		im->h = h;
		im->config.textureptr = rgba16;
		im->config.width  = (u8)(w > 255 ? 255 : w);
		im->config.height = (u8)(h > 255 ? 255 : h);
		im->config.level  = 0;
		im->config.format = G_IM_FMT_RGBA;
		im->config.depth  = G_IM_SIZ_16b;
		im->config.s = 0;
		im->config.t = 0;
		im->config.unk0b = 0;
	}

	lua_pushinteger(L, g_LuaImageCount);
	g_LuaImageCount++;
	return 1;
#else
	lua_pushnil(L);
	return 1;
#endif
}

/* pd.draw_image(handle, cx, cy, w, h, [angle_deg], [color], [secs]). Blit a
 * pd.load_image texture as a w*h HUD quad CENTRED at (cx,cy), rotated angle_deg
 * degrees. Default color = white opaque (untinted); pass a color to tint. */
static int l_pd_draw_image(lua_State *L)
{
#ifndef PLATFORM_N64
	s32 handle = (s32)luaL_checkinteger(L, 1);
	s32 cx = (s32)luaL_checkinteger(L, 2);
	s32 cy = (s32)luaL_checkinteger(L, 3);
	s32 w = (s32)luaL_checkinteger(L, 4);
	s32 h = (s32)luaL_checkinteger(L, 5);
	f32 angle = (f32)(luaL_optnumber(L, 6, 0.0) * (3.14159265358979 / 180.0)); /* deg -> rad */
	u32 color = (u32)luaL_optinteger(L, 7, 0xffffffffu);
	f32 secs = (f32)luaL_optnumber(L, 8, 0.0);

	luaOverlayAdd(OVL_IMAGE, cx, cy, w, h, color, NULL, handle, angle, secs);
#endif
	return 0;
}

/* pd.draw_text(x, y, text, color, [secs]) */
static int l_pd_draw_text(lua_State *L)
{
	s32 x = (s32)luaL_checkinteger(L, 1);
	s32 y = (s32)luaL_checkinteger(L, 2);
	const char *text = luaL_checkstring(L, 3);
	u32 color = (u32)luaL_optinteger(L, 4, 0xffffffffu);
	f32 secs = (f32)luaL_optnumber(L, 5, 0.0);

	luaOverlayAdd(OVL_TEXT, x, y, 0, 0, color, text, 0, 0.f, secs);
	return 0;
}

/* pd.hud_message(text, [type]): HUD message. Default type is now
 * HUDMSGTYPE_DEFAULT — the standard BOTTOM line (pickup style) — so chaos
 * chatter stays out of the middle of the screen; pass an explicit type
 * (1 = objective complete, 2 = objective failed) for the big centred banner
 * (the fake-objective effects). No-op when there's no live local player. */
static int l_pd_hud_message(lua_State *L)
{
	const char *text = luaL_checkstring(L, 1);
	s32 type = (s32)luaL_optinteger(L, 2, HUDMSGTYPE_DEFAULT);
	char buf[256];
	size_t n = strlen(text);

	// The hudmsg text MUST end with '\n': textMeasure only advances the height
	// on a newline, so a message without one measures height 0 and its box
	// collapses to a sliver under the text (and centred types mis-position).
	// The engine's own messages are all '\n'-terminated; Lua strings aren't.
	if (n > sizeof(buf) - 2) {
		n = sizeof(buf) - 2;
	}
	memcpy(buf, text, n);
	if (n == 0 || buf[n - 1] != '\n') {
		buf[n++] = '\n';
	}
	buf[n] = '\0';

	hudmsgCreateLua(buf, type);
	return 0;
}

/* ------------------------------------------------------------------------- *
 * Session-persistent key->string store (pd.persist_get / pd.persist_set).
 *
 * The whole lua_State is destroyed (lua_close in luaaiReset) on every stage
 * change -- mission load, return to the main menu -- so script globals do NOT
 * survive a reload (luaai.c luaaiExecute). This tiny C-owned table lives
 * outside the lua_State, so a script (e.g. the AP test harness check board) can
 * persist state across that teardown. Session-only; not written to disk.
 * ------------------------------------------------------------------------- */
#define LUA_PERSIST_MAX 32
static struct luapersist { char *key; char *val; } g_LuaPersist[LUA_PERSIST_MAX];

static char *luaApiStrDup(const char *s)
{
	size_t n = strlen(s) + 1;
	char *p = (char *)malloc(n);
	if (p) {
		memcpy(p, s, n);
	}
	return p;
}

/* pd.persist_set(key, value): a nil/absent value clears the key. */
static int l_pd_persist_set(lua_State *L)
{
	const char *key = luaL_checkstring(L, 1);
	const char *val = lua_isnoneornil(L, 2) ? NULL : luaL_checkstring(L, 2);
	s32 i;
	s32 slot = -1;

	for (i = 0; i < LUA_PERSIST_MAX; i++) {
		if (g_LuaPersist[i].key && strcmp(g_LuaPersist[i].key, key) == 0) {
			free(g_LuaPersist[i].val);
			g_LuaPersist[i].val = NULL;
			if (val) {
				g_LuaPersist[i].val = luaApiStrDup(val);
			} else {
				free(g_LuaPersist[i].key);
				g_LuaPersist[i].key = NULL;
			}
			return 0;
		}
		if (slot < 0 && !g_LuaPersist[i].key) {
			slot = i;
		}
	}

	if (val && slot >= 0) {
		g_LuaPersist[slot].key = luaApiStrDup(key);
		g_LuaPersist[slot].val = luaApiStrDup(val);
	}
	return 0;
}

/* pd.persist_get(key) -> string | nil */
static int l_pd_persist_get(lua_State *L)
{
	const char *key = luaL_checkstring(L, 1);
	s32 i;

	for (i = 0; i < LUA_PERSIST_MAX; i++) {
		if (g_LuaPersist[i].key && strcmp(g_LuaPersist[i].key, key) == 0) {
			if (g_LuaPersist[i].val) {
				lua_pushstring(L, g_LuaPersist[i].val);
			} else {
				lua_pushnil(L);
			}
			return 1;
		}
	}
	lua_pushnil(L);
	return 1;
}

/* ------------------------------------------------------------------------- *
 * Archipelago gating (pd.ap_mode / pd.unlock / pd.lock / pd.is_unlocked).
 *
 * One unlock set per category, 256 ids each (covers stages 0..20, weapons,
 * devices, and MP features 0..79). Lives outside the lua_State (C statics) so
 * the AP run's locks survive the per-stage lua_State teardown, exactly like the
 * persist KV. The engine gate points (mainmenu / bondgun / device) read
 * apGateActive() + apGateIsUnlocked(); everything is INERT unless an AP run has
 * called pd.ap_mode(true), so non-AP play is byte-identical.
 * ------------------------------------------------------------------------- */
#define AP_GATE_IDS 256
static bool g_ApGateMode;
static u8 g_ApUnlocks[AP_NUM_CATEGORIES][AP_GATE_IDS / 8];

/* Custom label the AP solo mission list shows in place of the "Mission 1"
 * group header — the script sets it (e.g. "Archipelago  3/20") via
 * pd.ap_list_header(). Lives outside the lua_State like the unlock set. Empty
 * string = fall back to the default header. */
#define AP_LIST_HEADER_MAX 48
static char g_ApListHeader[AP_LIST_HEADER_MAX];

/* Returns the script-set mission-list header, or NULL if unset/empty (caller
 * then uses the default). */
const char *apGetListHeader(void)
{
	return g_ApListHeader[0] ? g_ApListHeader : NULL;
}

bool apGateActive(void)
{
	// Server/solo only: a net client must not make its own access decisions
	// (AP is authoritative on the machine that owns the save). ap_mode is a
	// per-machine flag set only on that machine, so this is belt-and-suspenders.
	return g_ApGateMode && g_NetMode != NETMODE_CLIENT;
}

bool apGateIsUnlocked(s32 cat, s32 id)
{
	if (cat < 0 || cat >= AP_NUM_CATEGORIES || id < 0 || id >= AP_GATE_IDS) {
		return false;
	}
	return (g_ApUnlocks[cat][id >> 3] & (1 << (id & 7))) != 0;
}

/* Map a Lua category (string name or raw int) to an AP_CAT_* index, or -1. */
static s32 apGateCatArg(lua_State *L, s32 argn)
{
	if (lua_type(L, argn) == LUA_TNUMBER) {
		s32 c = (s32)lua_tointeger(L, argn);
		return (c >= 0 && c < AP_NUM_CATEGORIES) ? c : -1;
	}
	const char *s = luaL_optstring(L, argn, "");
	if (strcmp(s, "stage") == 0)      return AP_CAT_STAGE;
	if (strcmp(s, "difficulty") == 0) return AP_CAT_DIFFICULTY;
	if (strcmp(s, "weapon_pri") == 0) return AP_CAT_WEAPON_PRI;
	if (strcmp(s, "weapon_sec") == 0) return AP_CAT_WEAPON_SEC;
	if (strcmp(s, "device") == 0)     return AP_CAT_DEVICE;
	if (strcmp(s, "feature") == 0)    return AP_CAT_FEATURE;
	return -1;
}

static void apGateSet(s32 cat, s32 id, s32 on)
{
	if (cat < 0 || cat >= AP_NUM_CATEGORIES || id < 0 || id >= AP_GATE_IDS) {
		return;
	}
	if (on) {
		g_ApUnlocks[cat][id >> 3] |= (1 << (id & 7));
	} else {
		g_ApUnlocks[cat][id >> 3] &= ~(1 << (id & 7));
	}
}

/* pd.ap_mode([on]) -> bool : enable/disable AP gating (no arg = query). */
static int l_pd_ap_mode(lua_State *L)
{
	if (!lua_isnoneornil(L, 1)) {
		g_ApGateMode = lua_toboolean(L, 1) ? true : false;
	}
	lua_pushboolean(L, g_ApGateMode);
	return 1;
}

/* pd.unlock(category, id) : add (category,id) to the unlock set. */
static int l_pd_unlock(lua_State *L)
{
	apGateSet(apGateCatArg(L, 1), (s32)luaL_checkinteger(L, 2), 1);
	return 0;
}

/* pd.lock(category, id) : remove (category,id). */
static int l_pd_lock(lua_State *L)
{
	apGateSet(apGateCatArg(L, 1), (s32)luaL_checkinteger(L, 2), 0);
	return 0;
}

/* pd.is_unlocked(category, id) -> bool */
static int l_pd_is_unlocked(lua_State *L)
{
	lua_pushboolean(L, apGateIsUnlocked(apGateCatArg(L, 1), (s32)luaL_checkinteger(L, 2)));
	return 1;
}

/* pd.ap_reset() : clear all unlocks (does not change ap_mode). */
static int l_pd_ap_reset(lua_State *L)
{
	memset(g_ApUnlocks, 0, sizeof(g_ApUnlocks));
	(void)L;
	return 0;
}

/* pd.ap_list_header([str]) -> str : set/clear the AP mission-list group header
 * (nil or "" restores the default), returns the current value. The script
 * typically sets this to a progress string like "Archipelago  3/20". */
static int l_pd_ap_list_header(lua_State *L)
{
	if (!lua_isnoneornil(L, 1)) {
		const char *s = luaL_checkstring(L, 1);
		snprintf(g_ApListHeader, sizeof(g_ApListHeader), "%s", s);
	} else if (lua_type(L, 1) == LUA_TNIL) {
		g_ApListHeader[0] = '\0';
	}
	lua_pushstring(L, g_ApListHeader);
	return 1;
}

/* pd.each_chr(fn) -> fn(chrnum, ailistid, aioffset, alertness, islua) */
static int l_pd_each_chr(lua_State *L)
{
	s32 i;

	luaL_checktype(L, 1, LUA_TFUNCTION);

	for (i = 0; i < g_LuaXrayCount; i++) {
		struct luaxray *r = &g_LuaXray[i];
		lua_pushvalue(L, 1); /* fn */
		lua_pushinteger(L, r->chrnum);
		lua_pushinteger(L, r->ailistid);
		lua_pushinteger(L, r->aioffset);
		lua_pushinteger(L, r->alertness);
		lua_pushinteger(L, r->islua);
		if (lua_pcall(L, 5, 0, 0) != LUA_OK) {
			luaApiLog2("each_chr error: ", lua_tostring(L, -1));
			lua_pop(L, 1);
		}
	}
	return 0;
}

/* ------------------------------------------------------------------------- *
 * World / entity query API (read-only). Backed by bridge accessors in chrai.c
 * so this file stays free of engine structs.
 * ------------------------------------------------------------------------- */

/* Push a Lua table describing a chr snapshot. Shared by pd.chr_info and
 * ctx:self() so both have the same shape. Leaves the table on the stack. */
void luaApiPushChrInfo(lua_State *L, const struct luaaiselfinfo *info)
{
	lua_newtable(L);
	lua_pushinteger(L, info->chrnum);    lua_setfield(L, -2, "chrnum");
	lua_pushnumber(L, info->x);          lua_setfield(L, -2, "x");
	lua_pushnumber(L, info->y);          lua_setfield(L, -2, "y");
	lua_pushnumber(L, info->z);          lua_setfield(L, -2, "z");
	lua_pushinteger(L, info->room);      lua_setfield(L, -2, "room");
	lua_pushnumber(L, info->health);     lua_setfield(L, -2, "health");
	lua_pushnumber(L, info->maxhealth);  lua_setfield(L, -2, "maxhealth");
	lua_pushnumber(L, info->shield);     lua_setfield(L, -2, "shield");
	lua_pushinteger(L, info->alertness); lua_setfield(L, -2, "alertness");
	if (info->targetchrnum >= 0) {
		lua_pushinteger(L, info->targetchrnum);
		lua_setfield(L, -2, "target_chrnum");
	}
	if (info->targetplayernum >= 0) {
		lua_pushinteger(L, info->targetplayernum);
		lua_setfield(L, -2, "target_playernum");
	}
}

/* pd.chr_info(chrnum) -> table | nil */
static int l_pd_chr_info(lua_State *L)
{
	s32 chrnum = (s32)luaL_checkinteger(L, 1);
	struct luaaiselfinfo info;
	if (!chraiLuaGetChrInfo(chrnum, &info)) {
		lua_pushnil(L);
		return 1;
	}
	luaApiPushChrInfo(L, &info);
	return 1;
}

/* pd.chr_pos(chrnum) -> x, y, z | nil */
static int l_pd_chr_pos(lua_State *L)
{
	s32 chrnum = (s32)luaL_checkinteger(L, 1);
	struct luaaiselfinfo info;
	if (!chraiLuaGetChrInfo(chrnum, &info)) {
		lua_pushnil(L);
		return 1;
	}
	lua_pushnumber(L, info.x);
	lua_pushnumber(L, info.y);
	lua_pushnumber(L, info.z);
	return 3;
}

/* pd.chr_health(chrnum) -> health, maxhealth | nil */
static int l_pd_chr_health(lua_State *L)
{
	s32 chrnum = (s32)luaL_checkinteger(L, 1);
	struct luaaiselfinfo info;
	if (!chraiLuaGetChrInfo(chrnum, &info)) {
		lua_pushnil(L);
		return 1;
	}
	lua_pushnumber(L, info.health);
	lua_pushnumber(L, info.maxhealth);
	return 2;
}

/* pd.player_pos([n]) -> x, y, z | nil  (n defaults to 0) */
static int l_pd_player_pos(lua_State *L)
{
	s32 n = (s32)luaL_optinteger(L, 1, 0);
	struct luaaiplayerinfo info;
	if (!chraiLuaGetPlayerInfo(n, &info)) {
		lua_pushnil(L);
		return 1;
	}
	lua_pushnumber(L, info.x);
	lua_pushnumber(L, info.y);
	lua_pushnumber(L, info.z);
	return 3;
}

/* pd.player_count() -> n */
static int l_pd_player_count(lua_State *L)
{
	lua_pushinteger(L, chraiLuaGetPlayerCount());
	return 1;
}

/* pd.stage() -> current stage number (g_Vars.stagenum, e.g. STAGE_CITRAINING
 * for the Carrington Institute main-menu hub). Lets scripts tell the menu/hub
 * apart from real gameplay. */
static int l_pd_stage(lua_State *L)
{
	lua_pushinteger(L, chraiLuaGetStageNum());
	return 1;
}

/* pd.text_size(str) -> width, height in the same font pd.draw_text renders with
 * (g_FontHandelGothicXs). Lets a script size a background box to hug the text. */
static int l_pd_text_size(lua_State *L)
{
	const char *text = luaL_checkstring(L, 1);
	s32 h = 0, w = 0;
	if (g_CharsHandelGothicXs && g_FontHandelGothicXs) {
		textMeasure(&h, &w, (char *)text, g_CharsHandelGothicXs, g_FontHandelGothicXs, 0);
	}
	lua_pushinteger(L, w);
	lua_pushinteger(L, h);
	return 2;
}

/* pd.distance(x1,y1,z1, x2,y2,z2) -> number. Pure helper; convenient for
 * deciding on ranges from chr_pos/player_pos results. */
static int l_pd_distance(lua_State *L)
{
	double dx = luaL_checknumber(L, 1) - luaL_checknumber(L, 4);
	double dy = luaL_checknumber(L, 2) - luaL_checknumber(L, 5);
	double dz = luaL_checknumber(L, 3) - luaL_checknumber(L, 6);
	lua_pushnumber(L, (lua_Number)sqrt(dx * dx + dy * dy + dz * dz));
	return 1;
}

/* pd.spawn_at_chr(chrnum, weaponnum) -> true on success.
 * Spawns a weapon/item world object at that chr's location (server-side only).
 * The first mutating pd.* call; everything else above is read-only. */
static int l_pd_spawn_at_chr(lua_State *L)
{
	s32 chrnum = (s32)luaL_checkinteger(L, 1);
	s32 weaponnum = (s32)luaL_checkinteger(L, 2);
	lua_pushboolean(L, chraiLuaSpawnAtChr(chrnum, weaponnum) != 0);
	return 1;
}

/* pd.spawn(weaponnum, x, y, z, [ref_chrnum]) -> true on success.
 * Spawns a weapon/item object at an arbitrary world position; rooms are seeded
 * from ref_chrnum (or the local player's chr if omitted) and the object is
 * floor-snapped at the target. Server-side only. */
static int l_pd_spawn(lua_State *L)
{
	s32 weaponnum = (s32)luaL_checkinteger(L, 1);
	f32 x = (f32)luaL_checknumber(L, 2);
	f32 y = (f32)luaL_checknumber(L, 3);
	f32 z = (f32)luaL_checknumber(L, 4);
	s32 ref = (s32)luaL_optinteger(L, 5, -1);
	lua_pushboolean(L, chraiLuaSpawnAtPos(ref, weaponnum, x, y, z) != 0);
	return 1;
}

/* ------------------------------------------------------------------------- *
 * Toolkit framework: all-actor iteration + per-chr mutation primitives.
 * These let scripts apply mass effects (sneeze everyone, shield all, hive mind)
 * in Lua alone; adding a new effect = one wrapper here + one bridge in
 * chraction.c. All mutators are server-side (guarded in the bridge).
 * ------------------------------------------------------------------------- */

/* pd.all_chrs(fn): call fn(chrnum) for EVERY live actor (not just those whose AI
 * ran this frame, which is pd.each_chr).
 * pd.all_chrs()   : with no function arg, return an array table of the live
 * chrnums instead (chaos.lua iterates the result as a list). */
static int l_pd_all_chrs(lua_State *L)
{
	s32 i, n;
	const int hasfn = (lua_type(L, 1) == LUA_TFUNCTION);
	s32 outidx = 0;

	if (!hasfn) {
		lua_newtable(L);
	}

	n = chraiLuaGetChrSlotCount();
	for (i = 0; i < n; i++) {
		s32 chrnum = chraiLuaGetChrNumBySlot(i);
		if (chrnum < 0) {
			continue; /* empty slot */
		}
		if (hasfn) {
			lua_pushvalue(L, 1); /* fn */
			lua_pushinteger(L, chrnum);
			if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
				luaApiLog2("all_chrs error: ", lua_tostring(L, -1));
				lua_pop(L, 1);
			}
		} else {
			lua_pushinteger(L, chrnum);
			lua_rawseti(L, -2, ++outidx); /* result[outidx] = chrnum */
		}
	}
	return hasfn ? 0 : 1;
}

/* pd.chr_anim(chrnum, animnum, [speed]) -> bool. Play an animation on a chr. */
static int l_pd_chr_anim(lua_State *L)
{
	s32 chrnum = (s32)luaL_checkinteger(L, 1);
	s32 animnum = (s32)luaL_checkinteger(L, 2);
	f32 speed = (f32)luaL_optnumber(L, 3, 1.0);
	lua_pushboolean(L, chraiLuaChrAnim(chrnum, animnum, speed) != 0);
	return 1;
}

/* pd.chr_set_shield(chrnum, value) -> bool. */
static int l_pd_chr_set_shield(lua_State *L)
{
	s32 chrnum = (s32)luaL_checkinteger(L, 1);
	f32 value = (f32)luaL_checknumber(L, 2);
	lua_pushboolean(L, chraiLuaChrSetShield(chrnum, value) != 0);
	return 1;
}

/* pd.chr_alert(chrnum) -> bool. Put the chr on alert / onto its shot list. */
static int l_pd_chr_alert(lua_State *L)
{
	s32 chrnum = (s32)luaL_checkinteger(L, 1);
	lua_pushboolean(L, chraiLuaChrAlert(chrnum) != 0);
	return 1;
}

/* pd.chr_set_body(chrnum, bodynum, [headnum]) -> bool. Runtime model swap.
 * Solo/missions only (no-op in Combat Sim); player props refused. headnum
 * omitted/<0 picks a head valid for the body. */
static int l_pd_chr_set_body(lua_State *L)
{
	s32 chrnum = (s32)luaL_checkinteger(L, 1);
	s32 bodynum = (s32)luaL_checkinteger(L, 2);
	s32 headnum = (s32)luaL_optinteger(L, 3, -1);
	lua_pushboolean(L, chraiLuaChrSetBody(chrnum, bodynum, headnum) != 0);
	return 1;
}

/* pd.possess_spawn([bodynum]) -> chrnum | nil. Spawn a "cube" and fly it around
 * (free-fly). Solo/missions only; START/ESC or pd.unpossess() returns to Bond. */
static int l_pd_possess_spawn(lua_State *L)
{
	s32 bodynum = (s32)luaL_optinteger(L, 1, -1);
	s32 chrnum = chraiLuaPossessSpawn(bodynum);
	if (chrnum < 0) {
		lua_pushnil(L);
	} else {
		lua_pushinteger(L, chrnum);
	}
	return 1;
}

/* pd.unpossess(): stop possessing and return control to the player body. */
static int l_pd_unpossess(lua_State *L)
{
	chraiLuaUnpossess();
	return 0;
}

/* ------------------------------------------------------------------------- *
 * Archipelago bonus / buff API (received-side boosts to the local player).
 * Thin wrappers over the chraction.c bridges; all server/solo, no-op without a
 * live player. See docs/archipelago_blueprint.md section 4.1.
 * ------------------------------------------------------------------------- */

/* pd.player_heal() -> bool. Restore the player to full health. */
static int l_pd_player_heal(lua_State *L)
{
	lua_pushboolean(L, chraiLuaPlayerHeal() != 0);
	return 1;
}

/* pd.player_set_shield(frac) -> bool. frac 0..1 (>=1 = full). */
static int l_pd_player_set_shield(lua_State *L)
{
	f32 frac = (f32)luaL_optnumber(L, 1, 1.0);
	lua_pushboolean(L, chraiLuaPlayerSetShield(frac) != 0);
	return 1;
}

/* pd.refill_ammo() -> bool. Top all ammo to capacity (covers current weapon). */
static int l_pd_refill_ammo(lua_State *L)
{
	lua_pushboolean(L, chraiLuaRefillAmmo() != 0);
	return 1;
}

/* pd.give_ammo(ammotype, [qty]) -> bool. Grants ammo (+ the matching weapon). */
static int l_pd_give_ammo(lua_State *L)
{
	s32 ammotype = (s32)luaL_checkinteger(L, 1);
	s32 qty = (s32)luaL_optinteger(L, 2, 1);
	lua_pushboolean(L, chraiLuaGiveAmmo(ammotype, qty) != 0);
	return 1;
}

/* pd.give_weapon(weaponnum) -> bool. Add a weapon to the player's inventory. */
static int l_pd_give_weapon(lua_State *L)
{
	s32 weaponnum = (s32)luaL_checkinteger(L, 1);
	lua_pushboolean(L, chraiLuaGiveWeaponToPlayer(weaponnum) != 0);
	return 1;
}

/* pd.device_on(weaponnum) -> bool. Activate a device (e.g. WEAPON_CLOAKINGDEVICE). */
static int l_pd_device_on(lua_State *L)
{
	s32 weaponnum = (s32)luaL_checkinteger(L, 1);
	lua_pushboolean(L, chraiLuaDeviceOn(weaponnum) != 0);
	return 1;
}

/* pd.invincible(on) -> bool. Toggle invincibility (Lua manages any timer). */
static int l_pd_invincible(lua_State *L)
{
	s32 on = lua_toboolean(L, 1);
	lua_pushboolean(L, chraiLuaSetInvincible(on) != 0);
	return 1;
}

/* pd.spawn_ally() -> chrnum | nil. Spawn a friendly "Perfect Buddy". */
static int l_pd_spawn_ally(lua_State *L)
{
	s32 chrnum = chraiLuaSpawnAlly();
	if (chrnum < 0) {
		lua_pushnil(L);
	} else {
		lua_pushinteger(L, chrnum);
	}
	return 1;
}

/* pd.spawn_ally_clone([healthfrac]) -> chrnum | nil. A friendly buddy wearing
 * the player's own body/head (a Jo clone), with health scaled by healthfrac
 * (default 0.5). Backs the "Me and my son" chaos effect. */
static int l_pd_spawn_ally_clone(lua_State *L)
{
	f32 frac = (f32)luaL_optnumber(L, 1, 0.5);
	s32 chrnum = chraiLuaSpawnAllyClone(frac);
	if (chrnum < 0) {
		lua_pushnil(L);
	} else {
		lua_pushinteger(L, chrnum);
	}
	return 1;
}

/* ------------------------------------------------------------------------- *
 * Director menu registry (pd.menu_add / pd.menu_clear + C accessors)
 * ------------------------------------------------------------------------- */

static void luaMenuClearAll(lua_State *L)
{
	s32 i;
	for (i = 0; i < g_LuaMenuCount; i++) {
		if (L && g_LuaMenu[i].luaref != LUA_NOREF) {
			luaL_unref(L, LUA_REGISTRYINDEX, g_LuaMenu[i].luaref);
		}
		g_LuaMenu[i].luaref = LUA_NOREF;
		g_LuaMenu[i].label[0] = '\0';
		g_LuaMenu[i].group[0] = '\0';
	}
	g_LuaMenuCount = 0;
	luaDirectorRebuild(); /* array back to just the terminator */
}

/* pd.menu_add(label, fn, [group]) -> index (or -1 if the registry is full).
 * Adds a Lua Director pause-menu entry; selecting it later calls fn(). If group
 * is a non-empty string the entry is placed under a submenu of that title (the
 * submenu opener appears at the top of the root list); omit it for a root-level
 * entry. */
static int l_pd_menu_add(lua_State *L)
{
	const char *label = luaL_checkstring(L, 1);
	const char *group;

	luaL_checktype(L, 2, LUA_TFUNCTION);
	group = luaL_optstring(L, 3, "");

	if (g_LuaMenuCount >= LUA_MENU_MAX) {
		luaApiLog("menu_add: registry full");
		lua_pushinteger(L, -1);
		return 1;
	}

	strncpy(g_LuaMenu[g_LuaMenuCount].label, label, LUA_MENU_LABEL - 1);
	g_LuaMenu[g_LuaMenuCount].label[LUA_MENU_LABEL - 1] = '\0';
	strncpy(g_LuaMenu[g_LuaMenuCount].group, group, LUA_MENU_LABEL - 1);
	g_LuaMenu[g_LuaMenuCount].group[LUA_MENU_LABEL - 1] = '\0';

	lua_pushvalue(L, 2); /* the fn */
	g_LuaMenu[g_LuaMenuCount].luaref = luaL_ref(L, LUA_REGISTRYINDEX);

	lua_pushinteger(L, g_LuaMenuCount);
	g_LuaMenuCount++;
	luaDirectorRebuild(); /* keep the menu items array valid + current */
	return 1;
}

/* pd.menu_clear(): drop all registered Director entries (e.g. before a script
 * re-registers them on reload). */
static int l_pd_menu_clear(lua_State *L)
{
	luaMenuClearAll(L);
	return 0;
}

/* pd.menu_set_label(index, text): rewrite an existing entry's label in place.
 * The Director menuitem holds a pointer to this buffer, so the on-screen text
 * updates live with no rebuild — lets a menu entry display an adjustable value
 * (chaos frequency / effect duration) that changes when it's selected. */
static int l_pd_menu_set_label(lua_State *L)
{
	s32 i = (s32)luaL_checkinteger(L, 1);
	const char *label = luaL_checkstring(L, 2);

	if (i >= 0 && i < g_LuaMenuCount) {
		strncpy(g_LuaMenu[i].label, label, LUA_MENU_LABEL - 1);
		g_LuaMenu[i].label[LUA_MENU_LABEL - 1] = '\0';
	}

	return 0;
}

/* C accessors used by the Lua Director dialog in mainmenu.c. */
s32 luaMenuCount(void)
{
	return g_LuaMenuCount;
}

const char *luaMenuLabel(s32 i)
{
	if (i < 0 || i >= g_LuaMenuCount) {
		return "";
	}
	return g_LuaMenu[i].label;
}

const char *luaMenuGroup(s32 i)
{
	if (i < 0 || i >= g_LuaMenuCount) {
		return "";
	}
	return g_LuaMenu[i].group;
}

void luaMenuInvoke(s32 i)
{
	lua_State *L = luaaiGetState();
	if (!L || i < 0 || i >= g_LuaMenuCount || g_LuaMenu[i].luaref == LUA_NOREF) {
		return;
	}
	lua_rawgeti(L, LUA_REGISTRYINDEX, g_LuaMenu[i].luaref);
	if (lua_isfunction(L, -1)) {
		if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
			luaApiLog2("menu item error: ", lua_tostring(L, -1));
			lua_pop(L, 1);
		}
	} else {
		lua_pop(L, 1);
	}
}

/* Called by luaai.c's luaai_build_pd with the pd table on top of the stack. */
#ifndef PLATFORM_N64
/* pd.octree_stats() -> table { drawn, culled, nodes, nodesculled, passes }.
 * Live per-frame counters from the outdoor-room octree culling in bg.c (see
 * docs/PORT_OCTREE.md). All zero on frames where no octree room rendered. */
static int l_pd_octree_stats(lua_State *L)
{
	lua_createtable(L, 0, 5);
	lua_pushinteger(L, g_BgOctreeStats.batchesdrawn);  lua_setfield(L, -2, "drawn");
	lua_pushinteger(L, g_BgOctreeStats.batchesculled); lua_setfield(L, -2, "culled");
	lua_pushinteger(L, g_BgOctreeStats.nodestested);   lua_setfield(L, -2, "nodes");
	lua_pushinteger(L, g_BgOctreeStats.nodesculled);   lua_setfield(L, -2, "nodesculled");
	lua_pushinteger(L, g_BgOctreeStats.roomsculled);   lua_setfield(L, -2, "passes");
	return 1;
}

/* pd.dlcache_stats() -> table { enabled, cached, bad, batches, tris, fog,
 * lighting, cullboth, empty, texgen, tex_used, tex_max }. Live counters from the
 * display-list cache (see docs/PORT_DLCACHE.md): cached/bad leaf counts,
 * batches/tris replayed last frame, the reason flags for leaves that fell back to
 * legacy, and the texture-cache fill (tex_used/tex_max) -- when used hits max the
 * recorder is evicting on-screen textures, which renders them black (raise via
 * /texcache or Video.TextureCacheSize). Unlike `/dlcache stats` (a one-shot
 * console print) this updates every frame. */
static int l_pd_dlcache_stats(lua_State *L)
{
	extern void gfx_dlcache_get_stats(u32 *entries, u32 *bad, u32 *segments, u32 *tris, u32 *reasons);
	extern void gfx_get_texture_cache_fill(int *used, int *max);
	u32 cached = 0, bad = 0, batches = 0, tris = 0, reasons = 0;
	int texused = 0, texmax = 0;
	gfx_dlcache_get_stats(&cached, &bad, &batches, &tris, &reasons);
	gfx_get_texture_cache_fill(&texused, &texmax);

	lua_createtable(L, 0, 12);
	lua_pushboolean(L, g_DlCacheEnabled); lua_setfield(L, -2, "enabled");
	lua_pushinteger(L, cached);           lua_setfield(L, -2, "cached");
	lua_pushinteger(L, bad);              lua_setfield(L, -2, "bad");
	lua_pushinteger(L, batches);          lua_setfield(L, -2, "batches");
	lua_pushinteger(L, tris);             lua_setfield(L, -2, "tris");
	lua_pushboolean(L, reasons & 0x01);   lua_setfield(L, -2, "fog");      /* GFX_DLC_ABORT_FOG */
	lua_pushboolean(L, reasons & 0x02);   lua_setfield(L, -2, "lighting"); /* GFX_DLC_ABORT_LIGHTING */
	lua_pushboolean(L, reasons & 0x04);   lua_setfield(L, -2, "cullboth"); /* GFX_DLC_ABORT_CULLBOTH */
	lua_pushboolean(L, reasons & 0x08);   lua_setfield(L, -2, "empty");    /* GFX_DLC_ABORT_EMPTY */
	lua_pushboolean(L, reasons & 0x10);   lua_setfield(L, -2, "texgen");   /* GFX_DLC_ABORT_TEXGEN */
	lua_pushinteger(L, texused);          lua_setfield(L, -2, "tex_used");
	lua_pushinteger(L, texmax);           lua_setfield(L, -2, "tex_max");
	return 1;
}
#endif

#ifndef PLATFORM_N64
s32 g_LuaShowFps = 0; /* toggled by /fps; read by scripts/perf_overlay.lua via pd.perf() */
s32 g_LuaShowMem = 0; /* toggled by /mem */

/* pd.perf() -> table { fps, frame_ms, vtx_used, vtx_total, show_fps, show_mem }.
 * Render rate (video.c's 1s-averaged FPS) + the per-frame vtx scratch pool that
 * the No Room Culling / /octree bigroom whole-level render stresses (process RSS
 * isn't useful here: the pools are pre-allocated, so RSS doesn't move with load).
 * show_fps / show_mem are the /fps and /mem toggle states. */
static int l_pd_perf(lua_State *L)
{
	extern f32 videoGetAverageFPS(void);
	extern u32 gfxGetFreeVtx(void);
	extern u32 gfxGetVtxPoolSize(void);
	f32 fps = videoGetAverageFPS();
	u32 total = gfxGetVtxPoolSize();
	u32 freev = gfxGetFreeVtx();
	u32 used = (freev <= total) ? (total - freev) : total;

	lua_createtable(L, 0, 6);
	lua_pushnumber(L, (lua_Number)fps);                              lua_setfield(L, -2, "fps");
	lua_pushnumber(L, fps > 0.0f ? 1000.0 / (lua_Number)fps : 0.0);  lua_setfield(L, -2, "frame_ms");
	lua_pushinteger(L, (lua_Integer)used);                          lua_setfield(L, -2, "vtx_used");
	lua_pushinteger(L, (lua_Integer)total);                         lua_setfield(L, -2, "vtx_total");
	lua_pushboolean(L, g_LuaShowFps);                               lua_setfield(L, -2, "show_fps");
	lua_pushboolean(L, g_LuaShowMem);                               lua_setfield(L, -2, "show_mem");
	return 1;
}
#endif

/* ------------------------------------------------------------------------- *
 * Chaos-mode bindings (docs/PORT_CHAOS.md). Thin wrappers over the chraiLua*
 * game-side helpers, plus the cheat-bank toggles and the external event queue.
 * ------------------------------------------------------------------------- */

/* pd.cheat(cheat_id, on) -> bool. Flip a CHEAT_* active bit live (the same
 * banks the /wireframe & /mirror console commands drive). One binding unlocks
 * every port + vanilla cheat as a chaos effect: mirror, wireframe, tonal
 * inversion, hurricane fists, slo-mo, DK mode, classic options, ... */
static int l_pd_cheat(lua_State *L)
{
	s32 cheat_id = (s32)luaL_checkinteger(L, 1);
	s32 on = lua_toboolean(L, 2);
	/* two 32-bit active banks -> ids 0..63; highest defined id is 60 */
	if (cheat_id < 0 || cheat_id >= 64) {
		lua_pushboolean(L, 0);
		return 1;
	}
	// cheatSetActive (not cheatActivate) so the Experiments cheats work too —
	// GoldenEye / Wireframe / Mirror / Tonal live in the enabled bank, which
	// cheatActivate refuses; this routes them there like the menu does.
	cheatSetActive(cheat_id, on);
	lua_pushboolean(L, 1);
	return 1;
}

/* pd.cheat_active(cheat_id) -> bool */
static int l_pd_cheat_active(lua_State *L)
{
	s32 cheat_id = (s32)luaL_checkinteger(L, 1);
	lua_pushboolean(L, cheat_id >= 0 && cheat_id < 64 && cheatIsActive(cheat_id));
	return 1;
}

/* pd.sound(sfxnum) -> bool. One-shot local sound (announcer stingers). */
static int l_pd_sound(lua_State *L)
{
	lua_pushboolean(L, chraiLuaPlaySound((s32)luaL_checkinteger(L, 1)) != 0);
	return 1;
}

/* pd.take_weapon(weaponnum) -> bool */
static int l_pd_take_weapon(lua_State *L)
{
	lua_pushboolean(L, chraiLuaTakeWeapon((s32)luaL_checkinteger(L, 1)) != 0);
	return 1;
}

/* pd.weapon_held() -> weaponnum | -1 */
static int l_pd_weapon_held(lua_State *L)
{
	lua_pushinteger(L, chraiLuaWeaponHeld());
	return 1;
}

/* pd.switch_weapon(weaponnum) -> bool */
static int l_pd_switch_weapon(lua_State *L)
{
	lua_pushboolean(L, chraiLuaSwitchWeapon((s32)luaL_checkinteger(L, 1)) != 0);
	return 1;
}

/* pd.fade(r,g,b,a,time60) -> bool. Viewport fade (flashbang/blink effects). */
static int l_pd_fade(lua_State *L)
{
	s32 r = (s32)luaL_checkinteger(L, 1);
	s32 g = (s32)luaL_checkinteger(L, 2);
	s32 b = (s32)luaL_checkinteger(L, 3);
	s32 a = (s32)luaL_checkinteger(L, 4);
	f32 time60 = (f32)luaL_checknumber(L, 5);
	lua_pushboolean(L, chraiLuaScreenFade(r, g, b, a, time60) != 0);
	return 1;
}

/* pd.chr_yeet(chrnum, force) -> bool. Fling a chr away from the player. */
static int l_pd_chr_yeet(lua_State *L)
{
	s32 chrnum = (s32)luaL_checkinteger(L, 1);
	f32 force = (f32)luaL_optnumber(L, 2, 100.0);
	lua_pushboolean(L, chraiLuaYeetChr(chrnum, force) != 0);
	return 1;
}

/* pd.explosion(chrnum [, type]) -> bool. Detonate at a chr's feet. */
static int l_pd_explosion(lua_State *L)
{
	s32 chrnum = (s32)luaL_checkinteger(L, 1);
	s32 type = (s32)luaL_optinteger(L, 2, 9); /* a mid-size default type */
	lua_pushboolean(L, chraiLuaExplodeAtChr(chrnum, type) != 0);
	return 1;
}

/* pd.explosion_at(x, y, z [, type]) -> bool. Detonate at a position (rooms
 * portal-walked from the player). The Live Grenade / Martyrdom fuse boom. */
static int l_pd_explosion_at(lua_State *L)
{
	f32 x = (f32)luaL_checknumber(L, 1);
	f32 y = (f32)luaL_checknumber(L, 2);
	f32 z = (f32)luaL_checknumber(L, 3);
	s32 type = (s32)luaL_optinteger(L, 4, 9);
	lua_pushboolean(L, chraiLuaExplodeAtPos(x, y, z, type) != 0);
	return 1;
}

/* pd.grenade(x, y, z) -> bool. Drop a LIVE armed grenade at a position (real
 * engine thrown-grenade: lands, arms, plays the pin/throw SFX, detonates on
 * its own fuse). */
static int l_pd_grenade(lua_State *L)
{
	f32 x = (f32)luaL_checknumber(L, 1);
	f32 y = (f32)luaL_checknumber(L, 2);
	f32 z = (f32)luaL_checknumber(L, 3);
	s32 chrnum = (s32)luaL_optinteger(L, 4, -1); /* room-search seed (martyrdom corpses) */
	lua_pushboolean(L, chraiLuaSpawnGrenade(x, y, z, chrnum) != 0);
	return 1;
}

/* pd.input_source() -> "pad" | "kbm". The device the player most recently
 * used (Button Thief steals device-appropriate binds). */
static int l_pd_input_source(lua_State *L)
{
	extern s32 inputLastSourceWasPad(void);
	lua_pushstring(L, inputLastSourceWasPad() ? "pad" : "kbm");
	return 1;
}

/* pd.door_traps(on) -> bool. Booby-trapped doors: any door that starts
 * opening detonates. */
static int l_pd_door_traps(lua_State *L)
{
	lua_pushboolean(L, chraiLuaDoorTraps(lua_toboolean(L, 1)) != 0);
	return 1;
}

/* pd.door_opens() -> int. Doors opened this stage (task sensor). */
static int l_pd_door_opens(lua_State *L)
{
	lua_pushinteger(L, (lua_Integer)chraiLuaDoorOpens());
	return 1;
}

/* pd.env_colours(skyr,skyg,skyb, cloudr,cloudg,cloudb) -> bool. Override the
 * stage sky + cloud colours (0-255 each). pd.env() restores. */
static int l_pd_env_colours(lua_State *L)
{
	lua_pushboolean(L, chraiLuaEnvColours(
			(s32)luaL_checkinteger(L, 1), (s32)luaL_checkinteger(L, 2),
			(s32)luaL_checkinteger(L, 3), (s32)luaL_checkinteger(L, 4),
			(s32)luaL_checkinteger(L, 5), (s32)luaL_checkinteger(L, 6)) != 0);
	return 1;
}

/* pd.player_yaw() -> degrees 0..360. Look yaw (spin-around task sensor). */
static int l_pd_player_yaw(lua_State *L)
{
	lua_pushnumber(L, chraiLuaPlayerYaw());
	return 1;
}

/* pd.player_crouch() -> 0 stand / 1 duck / 2 squat. */
static int l_pd_player_crouch(lua_State *L)
{
	lua_pushinteger(L, chraiLuaPlayerCrouch());
	return 1;
}

/* pd.has_weapon(weaponnum) -> bool. Weapon is in the player's inventory. */
static int l_pd_has_weapon(lua_State *L)
{
	lua_pushboolean(L, chraiLuaHasWeapon((s32)luaL_checkinteger(L, 1)) != 0);
	return 1;
}

/* pd.bio_count() -> nchr, nmisc. How many character / misc bios the save has
 * unlocked (Carrington Institute information terminal). */
static int l_pd_bio_count(lua_State *L)
{
	lua_pushinteger(L, ciGetNumUnlockedChrBios());
	lua_pushinteger(L, ciGetNumUnlockedMiscBios());
	return 2;
}

/* pd.bio_text(kind, slot) -> name, body | nil. kind 0 = character bios
 * (body = description text), kind 1 = misc bios. slot is 0-based within the
 * unlocked set (pd.bio_count). The game's own lore, straight from the CI
 * information terminal. */
static int l_pd_bio_text(lua_State *L)
{
	s32 kind = (s32)luaL_checkinteger(L, 1);
	s32 slot = (s32)luaL_checkinteger(L, 2);

	if (kind == 0) {
		struct chrbio *bio;
		if (slot < 0 || slot >= ciGetNumUnlockedChrBios()) {
			lua_pushnil(L);
			return 1;
		}
		bio = ciGetChrBioByBodynum(ciGetChrBioBodynumBySlot(slot));
		if (bio == NULL) {
			lua_pushnil(L);
			return 1;
		}
		lua_pushstring(L, langGet(bio->name));
		lua_pushstring(L, langGet(bio->description));
		return 2;
	} else {
		struct miscbio *bio;
		if (slot < 0 || slot >= ciGetNumUnlockedMiscBios()) {
			lua_pushnil(L);
			return 1;
		}
		bio = ciGetMiscBio(ciGetMiscBioIndexBySlot(slot));
		if (bio == NULL) {
			lua_pushnil(L);
			return 1;
		}
		lua_pushstring(L, langGet(bio->name));
		lua_pushstring(L, langGet(bio->description));
		return 2;
	}
}

/* pd.device_off(weaponnum) -> bool. Deactivate a device (device_on inverse). */
static int l_pd_device_off(lua_State *L)
{
	s32 weaponnum = (s32)luaL_checkinteger(L, 1);
	lua_pushboolean(L, chraiLuaDeviceOff(weaponnum) != 0);
	return 1;
}

/* pd.lvupdate() -> int. Game ticks elapsed this frame (0 while paused). */
static int l_pd_lvupdate(lua_State *L)
{
	lua_pushinteger(L, chraiLuaLvUpdate());
	return 1;
}

/* pd.mission_complete() -> bool. True once the game has pushed a COMPLETED solo/
 * co-op mission endscreen (mission won, not failed/aborted); cleared on the next
 * stage load. Lets chaos.lua tear down effects on success, before the hub. */
static int l_pd_mission_complete(lua_State *L)
{
	extern s32 g_ChaosMissionComplete;
	lua_pushboolean(L, g_ChaosMissionComplete != 0);
	return 1;
}

/* pd.alarm(on) -> bool. Stage alarm on/off (server-side; SVC_ALARM mirrors). */
static int l_pd_alarm(lua_State *L)
{
	lua_pushboolean(L, chraiLuaSetAlarm(lua_toboolean(L, 1)) != 0);
	return 1;
}

/* pd.boost([secs]) -> bool. Speed Pill boost for N seconds (self-decaying);
 * secs <= 0 cancels an active boost. */
static int l_pd_boost(lua_State *L)
{
	f32 secs = (f32)luaL_optnumber(L, 1, 10.0);
	lua_pushboolean(L, chraiLuaBoost(secs) != 0);
	return 1;
}

/* pd.player_set_health(frac) -> bool. Set health 0.01..1 (never kills). */
static int l_pd_player_set_health(lua_State *L)
{
	f32 frac = (f32)luaL_checknumber(L, 1);
	lua_pushboolean(L, chraiLuaPlayerSetHealth(frac) != 0);
	return 1;
}

/* pd.dizzy([amount]) -> bool. Tranquiliser screen-sway (decays naturally). */
static int l_pd_dizzy(lua_State *L)
{
	s32 amount = (s32)luaL_optinteger(L, 1, 3000);
	lua_pushboolean(L, chraiLuaDizzy(amount) != 0);
	return 1;
}

/* pd.chr_cloak(chrnum, on) -> bool. Toggle a chr's cloaking device flag. */
static int l_pd_chr_cloak(lua_State *L)
{
	s32 chrnum = (s32)luaL_checkinteger(L, 1);
	lua_pushboolean(L, chraiLuaChrCloak(chrnum, lua_toboolean(L, 2)) != 0);
	return 1;
}

/* pd.strip_ammo() -> bool. Zero every ammo pool (weapons stay). */
static int l_pd_strip_ammo(lua_State *L)
{
	lua_pushboolean(L, chraiLuaStripAmmo() != 0);
	return 1;
}

/* pd.teleport_to_chr(chrnum) -> bool. Snap the player to a chr (server-side). */
static int l_pd_teleport_to_chr(lua_State *L)
{
	s32 chrnum = (s32)luaL_checkinteger(L, 1);
	lua_pushboolean(L, chraiLuaTeleportToChr(chrnum) != 0);
	return 1;
}

/* pd.flattex(mode) -> bool. 0 normal / 1 white textures (vertex shading only)
 * / 2 average-colour textures. Cosmetic only. */
static int l_pd_flattex(lua_State *L)
{
	s32 mode = (s32)luaL_optinteger(L, 1, 0);
	lua_pushboolean(L, chraiLuaFlatTex(mode) != 0);
	return 1;
}

/* pd.grayscale(on) -> bool. Force the renderer grayscale path (film noir). */
static int l_pd_grayscale(lua_State *L)
{
	lua_pushboolean(L, chraiLuaGrayscale(lua_toboolean(L, 1)) != 0);
	return 1;
}

/* pd.shiny(mode) -> bool. 0 off / 1 fake-chrome screen-space UVs on every 3D
 * surface / 2 the same plus a gold tint (Midas mode). Cosmetic only. */
static int l_pd_shiny(lua_State *L)
{
	s32 mode = (s32)luaL_optinteger(L, 1, 0);
	lua_pushboolean(L, chraiLuaShiny(mode) != 0);
	return 1;
}

/* pd.chr_give_weapon(chrnum, weaponnum) -> bool. Replace an NPC's held
 * weapons with this one (right hand). */
static int l_pd_chr_give_weapon(lua_State *L)
{
	s32 chrnum = (s32)luaL_checkinteger(L, 1);
	s32 weaponnum = (s32)luaL_checkinteger(L, 2);
	lua_pushboolean(L, chraiLuaChrGiveWeapon(chrnum, weaponnum) != 0);
	return 1;
}

/* pd.chr_weapon(chrnum) -> weaponnum. The NPC's current weapon (-1 if invalid).
 * Snapshot before chr_give_weapon so a timed effect can restore it. */
static int l_pd_chr_weapon(lua_State *L)
{
	s32 chrnum = (s32)luaL_checkinteger(L, 1);
	lua_pushinteger(L, chraiLuaChrWeapon(chrnum));
	return 1;
}

/* pd.player_health() -> number. Current health fraction (0..1), the scale
 * player_set_health writes. */
static int l_pd_player_health(lua_State *L)
{
	lua_pushnumber(L, chraiLuaPlayerHealth());
	return 1;
}

/* pd.player_damage(amount) -> bool. Hurt the local player through the real
 * damage path; ~1.0 is roughly one gunshot. */
static int l_pd_player_damage(lua_State *L)
{
	lua_pushboolean(L, chraiLuaPlayerDamage((f32)luaL_checknumber(L, 1)) != 0);
	return 1;
}

/* pd.weapon_jam(mode) -> bool. 1/true = every trigger pull dry-fires;
 * 2 = "jam v2": ~35% of pulls dry-fire, and a shot that fires drains the rest
 * of the magazine (reload to clear). false/0 = off. */
static int l_pd_weapon_jam(lua_State *L)
{
	s32 mode;
	if (lua_isnumber(L, 1)) {
		mode = (s32)lua_tointeger(L, 1);
		if (mode < 0) {
			mode = 0;
		} else if (mode > 2) {
			mode = 2;
		}
	} else {
		mode = lua_toboolean(L, 1) ? 1 : 0;
	}
	lua_pushboolean(L, chraiLuaWeaponJam(mode) != 0);
	return 1;
}

/* pd.force_secondary(on) -> bool. Pin both hands to the secondary function. */
static int l_pd_force_secondary(lua_State *L)
{
	lua_pushboolean(L, chraiLuaForceSecondary(lua_toboolean(L, 1)) != 0);
	return 1;
}

/* pd.button_block(mask) -> bool. Strip these N64 pad buttons from gameplay
 * input (A 0x8000, B 0x4000, Z 0x2000, L 0x20, R 0x10, C-up 8, C-down 4,
 * C-left 2, C-right 1). 0 = give everything back. */
static int l_pd_button_block(lua_State *L)
{
	lua_pushboolean(L, chraiLuaButtonMask((u32)luaL_optinteger(L, 1, 0)) != 0);
	return 1;
}

/* pd.ammo_cost(mult) -> bool. Each shot spends mult rounds; 1 = normal. */
static int l_pd_ammo_cost(lua_State *L)
{
	lua_pushboolean(L, chraiLuaAmmoCost((s32)luaL_optinteger(L, 1, 1)) != 0);
	return 1;
}

/* pd.autoaim(on) -> bool. Force aim assist on regardless of the option. */
static int l_pd_autoaim(lua_State *L)
{
	lua_pushboolean(L, chraiLuaAutoAim(lua_toboolean(L, 1)) != 0);
	return 1;
}

/* pd.deadzone(frac) -> bool. Analog deadzone floor, 0..1 of full deflection
 * (0.45 = the XBLA special). 0/none = off. */
static int l_pd_deadzone(lua_State *L)
{
	lua_pushboolean(L, chraiLuaDeadzone((f32)luaL_optnumber(L, 1, 0.0)) != 0);
	return 1;
}

/* pd.nitro(on) -> bool. Every destroyed object explodes like the Crash Site
 * ship. */
static int l_pd_nitro(lua_State *L)
{
	lua_pushboolean(L, chraiLuaNitro(lua_toboolean(L, 1)) != 0);
	return 1;
}

/* pd.objective_force(index, state) -> bool. state 0 = real status,
 * 1 = force INCOMPLETE, 2 = force COMPLETE. index -1 clears all. Solo only. */
static int l_pd_objective_force(lua_State *L)
{
	lua_pushboolean(L, chraiLuaObjectiveForce(
			(s32)luaL_optinteger(L, 1, -1),
			(s32)luaL_optinteger(L, 2, 0)) != 0);
	return 1;
}

/* pd.objective_status(index) -> int. The objective's REAL status (any force
 * bypassed): 0 incomplete / 1 complete / 2 failed, or -1 if the index isn't a
 * live objective on this stage + difficulty. */
static int l_pd_objective_status(lua_State *L)
{
	lua_pushinteger(L, chraiLuaObjectiveStatus((s32)luaL_checkinteger(L, 1)));
	return 1;
}

/* pd.mark_home() -> bool. Record the player's position for pd.warp_home. */
static int l_pd_mark_home(lua_State *L)
{
	lua_pushboolean(L, chraiLuaMarkHome() != 0);
	return 1;
}

/* pd.warp_home() -> bool. Teleport back to the marked position. */
static int l_pd_warp_home(lua_State *L)
{
	lua_pushboolean(L, chraiLuaWarpHome() != 0);
	return 1;
}

/* pd.env(stagenum) -> bool. Apply another stage's sky/fog/cloud environment;
 * pd.env() restores the current stage's own. */
static int l_pd_env(lua_State *L)
{
	lua_pushboolean(L, chraiLuaEnv((s32)luaL_optinteger(L, 1, -1)) != 0);
	return 1;
}

/* pd.fog(fogmin, fogmax, r, g, b) -> bool. Custom fog overlay: fogmin/fogmax
 * are per-mille of the z-range (stock stages ~950..1050, lower = closer wall);
 * r,g,b = the fog/sky colour. pd.fog() restores the stage's environment. */
static int l_pd_fog(lua_State *L)
{
	if (lua_gettop(L) == 0) {
		lua_pushboolean(L, chraiLuaEnv(-1) != 0);
		return 1;
	}
	lua_pushboolean(L, chraiLuaFog(
			(s32)luaL_checkinteger(L, 1),
			(s32)luaL_checkinteger(L, 2),
			(s32)luaL_optinteger(L, 3, 200),
			(s32)luaL_optinteger(L, 4, 200),
			(s32)luaL_optinteger(L, 5, 210)) != 0);
	return 1;
}

/* pd.blood_colour(r, g, b) -> bool. Everyone bleeds this colour; pd.blood_colour()
 * restores the per-body palettes. */
static int l_pd_blood_colour(lua_State *L)
{
	if (lua_gettop(L) == 0) {
		lua_pushboolean(L, chraiLuaBloodColour(0, 0, 0, 0) != 0);
		return 1;
	}
	lua_pushboolean(L, chraiLuaBloodColour(
			(s32)luaL_checkinteger(L, 1),
			(s32)luaL_checkinteger(L, 2),
			(s32)luaL_checkinteger(L, 3), 1) != 0);
	return 1;
}

/* pd.max_blood(on) -> bool. Every hit splatters, and hard. */
static int l_pd_max_blood(lua_State *L)
{
	lua_pushboolean(L, chraiLuaMaxBlood(lua_toboolean(L, 1)) != 0);
	return 1;
}

/* pd.items_shuffle() -> int. Shuffle every loose weapon pickup's position;
 * returns how many moved. */
static int l_pd_items_shuffle(lua_State *L)
{
	lua_pushinteger(L, chraiLuaItemsShuffle());
	return 1;
}

/* pd.chr_wireframe(on) -> bool. Hostile chrs render as polygon outlines. */
static int l_pd_chr_wireframe(lua_State *L)
{
	lua_pushboolean(L, chraiLuaChrWireframe(lua_toboolean(L, 1)) != 0);
	return 1;
}

/* pd.double_shots(on) -> bool. Every fire event takes twice the shots. */
static int l_pd_double_shots(lua_State *L)
{
	lua_pushboolean(L, chraiLuaDoubleShots(lua_toboolean(L, 1)) != 0);
	return 1;
}

/* pd.quad_top(on) -> bool. A second pair of viewmodel guns hangs upside-down
 * from the top of the screen. */
static int l_pd_quad_top(lua_State *L)
{
	lua_pushboolean(L, chraiLuaQuadTop(lua_toboolean(L, 1)) != 0);
	return 1;
}

/* pd.buttons() -> int. The local player's RAW held pad buttons (N64 mask:
 * A 0x8000, B 0x4000, Z 0x2000, R 0x10, C-up 8, C-down 4, C-left 2,
 * C-right 1). Sees buttons even while pd.button_block hides them from
 * gameplay — the popup framework blocks FIRE and still reads the answer. */
static int l_pd_buttons(lua_State *L)
{
	lua_pushinteger(L, (lua_Integer)chraiLuaButtons(0));
	return 1;
}

/* pd.buttons_pressed() -> int. Buttons newly pressed this frame (same mask). */
static int l_pd_buttons_pressed(lua_State *L)
{
	lua_pushinteger(L, (lua_Integer)chraiLuaButtons(1));
	return 1;
}

/* pd.spawn_chopper([kind[, extrascale]]) -> bool. A hostile chopper appears
 * near the player and opens fire. kind 0 (default) = the dD hovercopter,
 * 1 = the A51 manned interceptor (a native chopper type — the gunfire code
 * special-cases its model scale). extrascale: 256 = full size; omitted uses
 * the per-kind default (copter 64 = quarter, interceptor 256 — its modeldef
 * is natively ~0.1 scale, don't shrink it further). Solo only. */
static int l_pd_spawn_chopper(lua_State *L)
{
	lua_pushboolean(L, chraiLuaSpawnChopper(
			(s32)luaL_optinteger(L, 1, 0),
			(s32)luaL_optinteger(L, 2, 0)) != 0);
	return 1;
}

/* pd.player_freeze(on) -> bool. Root the local player in place. */
static int l_pd_player_freeze(lua_State *L)
{
	lua_pushboolean(L, chraiLuaPlayerFreeze(lua_toboolean(L, 1)) != 0);
	return 1;
}

/* pd.chr_freeze(on) -> bool. Pause every non-player chr's animation + firing. */
static int l_pd_chr_freeze(lua_State *L)
{
	lua_pushboolean(L, chraiLuaChrFreeze(lua_toboolean(L, 1)) != 0);
	return 1;
}

/* pd.no_drops(on) -> bool. Dead chrs keep their weapons in hand. */
static int l_pd_no_drops(lua_State *L)
{
	lua_pushboolean(L, chraiLuaNoDrops(lua_toboolean(L, 1)) != 0);
	return 1;
}

/* pd.paintball(on) -> bool. Force paintball visuals for everyone. */
static int l_pd_paintball(lua_State *L)
{
	lua_pushboolean(L, chraiLuaPaintball(lua_toboolean(L, 1)) != 0);
	return 1;
}

/* pd.damage_scale(frac) -> bool. Scale all chr/player damage (1 = normal). */
static int l_pd_damage_scale(lua_State *L)
{
	lua_pushboolean(L, chraiLuaDamageScale((f32)luaL_optnumber(L, 1, 1.0)) != 0);
	return 1;
}

/* pd.zoom_scale(mult) -> bool. Scale weapon aim-zoom FOV; >1 zooms OUT. */
static int l_pd_zoom_scale(lua_State *L)
{
	lua_pushboolean(L, chraiLuaZoomScale((f32)luaL_optnumber(L, 1, 1.0)) != 0);
	return 1;
}

/* pd.gun_sound(weaponnum) -> bool. Every gun fires with this weapon's shoot
 * sound; pd.gun_sound() restores. */
static int l_pd_gun_sound(lua_State *L)
{
	lua_pushboolean(L, chraiLuaGunSound((s32)luaL_optinteger(L, 1, 0)) != 0);
	return 1;
}

/* pd.mute(on) -> bool. Master audio mute (SFX + music). */
static int l_pd_mute(lua_State *L)
{
	lua_pushboolean(L, chraiLuaMute(lua_toboolean(L, 1)) != 0);
	return 1;
}

/* pd.play_file(path) -> bool. Play an external WAV (e.g.
 * scripts/sounds/chaos/ring.wav) through the device stream. */
static int l_pd_play_file(lua_State *L)
{
	const char *path = luaL_checkstring(L, 1);
	s32 loop = lua_toboolean(L, 2); /* pd.play_file(path, loop) */
	lua_pushboolean(L, chraiLuaPlayFile(path, loop) != 0);
	return 1;
}

/* pd.stop_file(). Stop the external sound started by pd.play_file. */
static int l_pd_stop_file(lua_State *L)
{
	chraiLuaStopFile();
	return 0;
}

/* pd.weapon_rename(weaponnum [, name]). Relabel a weapon everywhere it's
 * shown; no name / nil restores the real one. */
static int l_pd_weapon_rename(lua_State *L)
{
	s32 weaponnum = (s32)luaL_checkinteger(L, 1);
	const char *name = luaL_optstring(L, 2, NULL);
	lua_pushboolean(L, chraiLuaWeaponRename(weaponnum, name) != 0);
	return 1;
}

/* --------------------------------------------------------------------------
 * Mid-mission menu drivers (pd.menu_lore / pd.game_over). Both use the exact
 * CI-terminal recipe func0f0f85e0(dialog, root): push a root dialog + pause
 * the live stage; the player closes it and func0f0fa6ac -> playerUnpause
 * resumes. This is the same machinery the Start-button pause and the CI hub
 * information terminal use, so it's a proven mid-stage path. Solo/co-op only
 * (never Combat Sim / a net client — restart/menu semantics don't apply).
 * ------------------------------------------------------------------------ */
#ifndef PLATFORM_N64
/* true only in a real solo/co-op mission with a live local pawn */
static bool chaosMenuAllowed(void)
{
	return g_NetMode != NETMODE_CLIENT
			&& !g_Vars.normmplayerisrunning
			&& g_Vars.currentplayer != NULL
			&& g_Vars.currentplayer->prop != NULL
			&& g_Vars.stagenum != STAGE_CITRAINING;
}

/* ---- Game Over: the REAL mission-failed screen, indistinguishable from the
 * engine's, but with our two choices (Accept = restart, Decline = resume). It
 * mirrors the genuine two-screen flow:
 *   1) g_ChaosFailedStatsDialog — a clone of g_SoloMissionEndscreenFailedMenuDialog:
 *      MENUDIALOGTYPE_DANGER, the real "<Stage>: Failed" title, and the REAL
 *      stats item list (g_MissionEndscreenMenuItems). Those text functions read
 *      LIVE run stats (mission time, kills, accuracy, shots, difficulty, weapon
 *      of choice) which are valid mid-mission; the cheat-availability lines hide
 *      because we zero endscreen.cheatinfo. We deliberately do NOT call
 *      endscreenResetModels / configure a menumodel — that pool IS the live
 *      viewmodel gun-mem and reusing it mid-mission would corrupt it; a DANGER
 *      dialog draws no 3D model on its own, so skipping it is safe.
 *   2) On any input it pushes g_ChaosRetryDialog — the real retry look
 *      (objectives + Accept/Decline, "Retry: <Stage>" title, the genuine
 *      endscreenHandleRetryMission for Start=accept / Back=resume). Accept runs
 *      the real menuhandlerAcceptMission (restart); Decline resumes the LIVE,
 *      still-paused mission instead of quitting to the menu.
 * ---------------------------------------------------------------------------- */
extern struct menuitem g_MissionEndscreenMenuItems[]; /* the real failed/complete stats list (endscreen.c) */
extern s32 g_ChaosGameOverStatus; /* forces Mission=Unknown / Agent=Missing while our screen is up */

/* Close both game-over dialogs -> the menu-close path (func0f0fa6ac) unpauses
 * the still-live mission, back to exactly where we were. Two pops = the
 * stats+retry stack depth; the second is a safe no-op if only one is open. */
static MenuItemHandlerResult chaosGameOverResume(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_SET) {
		playerSetFadeColour(0, 0, 0, 0.0f); // clear the game-over black screen
		g_ChaosGameOverStatus = 0;          // stop forcing Unknown/Missing
		menuPopDialog();
		menuPopDialog();
	}
	return 0;
}

/* Retry screen — Accept / Decline. We do NOT reuse the engine's
 * g_RetryMissionMenuItems (its Objectives item) or endscreenHandleRetryMission:
 * that handler delegates to menudialog00103608, whose MENUOP_OPEN calls
 * setupLoadBriefing into the (unconfigured) menumodel buffer and DMAs the
 * briefing file into a garbage address — the mid-mission crash. Our handler
 * loads nothing; Start selects the focused item (STARTSELECTS), Back resumes. */
static struct menuitem g_ChaosRetryItems[] = {
	{ MENUITEMTYPE_SELECTABLE, 0, 0, L_OPTIONS_298 /* Accept */,  0, menuhandlerAcceptMission },
	{ MENUITEMTYPE_SELECTABLE, 0, 0, L_OPTIONS_299 /* Decline */, 0, chaosGameOverResume },
	{ MENUITEMTYPE_END },
};

static MenuDialogHandlerResult chaosRetryHandle(s32 operation, struct menudialogdef *dialogdef, union handlerdata *data)
{
	if (operation == MENUOP_TICK
			&& g_Menus[g_MpPlayerNum].curdialog
			&& g_Menus[g_MpPlayerNum].curdialog->definition == dialogdef) {
		struct menuinputs *inputs = data->dialog2.inputs;
		if (inputs->back) {
			inputs->back = false;
			chaosGameOverResume(MENUOP_SET, NULL, NULL); // clear fade + pop2 -> resume
		}
	}
	return 0;
}

static struct menudialogdef g_ChaosRetryDialog = {
	MENUDIALOGTYPE_DANGER,
	(uintptr_t)&endscreenMenuTitleRetryMission,   /* real "Retry: <Stage>" */
	g_ChaosRetryItems,
	chaosRetryHandle,
	MENUDIALOGFLAG_STARTSELECTS | MENUDIALOGFLAG_DISABLEITEMSCROLL,
	NULL,
};

/* Failed stats screen handler: on any input, advance to the retry screen —
 * the same transition the real endscreenHandle2PFailed makes. */
static MenuDialogHandlerResult chaosFailedStatsHandle(s32 operation, struct menudialogdef *dialogdef, union handlerdata *data)
{
	if (operation == MENUOP_TICK
			&& g_Menus[g_MpPlayerNum].curdialog
			&& g_Menus[g_MpPlayerNum].curdialog->definition == dialogdef) {
		struct menuinputs *inputs = data->dialog2.inputs;
		if (inputs->select || inputs->back || inputs->start) {
			inputs->select = inputs->back = inputs->start = false;
			menuPushDialog(&g_ChaosRetryDialog);
		}
	}
	return 0;
}

static struct menudialogdef g_ChaosFailedStatsDialog = {
	MENUDIALOGTYPE_DANGER,
	(uintptr_t)&endscreenMenuTitleStageFailed,    /* real "<Stage>: Failed" */
	g_MissionEndscreenMenuItems,                  /* real live-stat lines */
	chaosFailedStatsHandle,
	MENUDIALOGFLAG_DISABLEITEMSCROLL | MENUDIALOGFLAG_SMOOTHSCROLLABLE,
	NULL,
};
#endif

/* pd.menu_lore() -> bool. Open ONE random unlocked CI bio (character profile
 * or misc file) over the paused mission — the real hub-terminal reader
 * pushed directly as the menu root, so closing it resumes the mission. */
static int l_pd_menu_lore(lua_State *L)
{
#ifndef PLATFORM_N64
	extern struct menudialogdef g_BioProfileMenuDialog;
	extern struct menudialogdef g_BioTextMenuDialog;
	s32 nchr = ciGetNumUnlockedChrBios();
	s32 nmisc = ciGetNumUnlockedMiscBios();

	if (chaosMenuAllowed() && g_Menus[g_MpPlayerNum].curdialog == NULL && nchr + nmisc > 0) {
		// g_ChrBioSlot is the selector the profile/text dialogs read — the
		// same global the Information list menu sets on selection.
		g_ChrBioSlot = (u8)(rngRandom() % (u32)(nchr + nmisc));
		func0f0f85e0(g_ChrBioSlot < nchr ? &g_BioProfileMenuDialog : &g_BioTextMenuDialog,
				MENUROOT_TRAINING);
		lua_pushboolean(L, 1);
		return 1;
	}
#endif
	lua_pushboolean(L, 0);
	return 1;
}

/* pd.game_over() -> bool. Show the real mission-failed screen; Accept
 * restarts, Decline resumes where you were. */
static int l_pd_game_over(lua_State *L)
{
#ifndef PLATFORM_N64
	if (chaosMenuAllowed() && g_Menus[g_MpPlayerNum].curdialog == NULL) {
		// Minimal endscreen-state prep, matching endscreenPrepare's non-model
		// bits: zero cheatinfo (hides the "New Cheat Available" lines),
		// point stageindex at the current stage (title + difficulty lines),
		// player 0. NO endscreenResetModels / menumodel — see the dialog note.
		g_Menus[g_MpPlayerNum].endscreen.cheatinfo = 0;
		g_Menus[g_MpPlayerNum].endscreen.isfirstcompletion = false;
		g_Menus[g_MpPlayerNum].endscreen.stageindex = g_MissionConfig.stageindex;
		g_Menus[g_MpPlayerNum].playernum = 0;
		playerSetFadeColour(0, 0, 0, 1.0f);              // black out the world behind the screen
		musicStartTrackAsMenu(MUSIC_MISSION_FAILED);      // the mission-failed jingle
		g_ChaosGameOverStatus = 1;                        // Mission: Unknown / Agent: Missing
		func0f0f85e0(&g_ChaosFailedStatsDialog, MENUROOT_MAINMENU);
		lua_pushboolean(L, 1);
		return 1;
	}
#endif
	lua_pushboolean(L, 0);
	return 1;
}

/* pd.chr_speed(mult) -> bool. Scale every non-player chr's anim playback
 * (movement + attack cadence follow). 1 = normal. */
static int l_pd_chr_speed(lua_State *L)
{
	lua_pushboolean(L, chraiLuaChrSpeed((f32)luaL_optnumber(L, 1, 1.0)) != 0);
	return 1;
}

/* pd.player_speed(mult) -> bool. Scale the local player's walk/strafe speed
 * ("Gotta go fast"). 1 = normal. */
static int l_pd_player_speed(lua_State *L)
{
	lua_pushboolean(L, chraiLuaPlayerSpeed((f32)luaL_optnumber(L, 1, 1.0)) != 0);
	return 1;
}

/* pd.chr_damage(chrnum, amount) -> bool. Hurt any chr via the real damage
 * path; ~1.0 is roughly one gunshot. */
static int l_pd_chr_damage(lua_State *L)
{
	s32 chrnum = (s32)luaL_checkinteger(L, 1);
	lua_pushboolean(L, chraiLuaChrDamage(chrnum, (f32)luaL_checknumber(L, 2)) != 0);
	return 1;
}

/* pd.chr_scale(chrnum, mult) -> bool. Multiply a chr's visual scale; undo by
 * calling again with the inverse. */
static int l_pd_chr_scale(lua_State *L)
{
	s32 chrnum = (s32)luaL_checkinteger(L, 1);
	lua_pushboolean(L, chraiLuaChrScale(chrnum, (f32)luaL_checknumber(L, 2)) != 0);
	return 1;
}

/* pd.chr_yscale(chrnum, mult) -> bool. Non-uniform vertical squash: scales only
 * the chr's height, keeping width/depth (mult 0.4 = 40% tall, full width). */
static int l_pd_chr_yscale(lua_State *L)
{
	s32 chrnum = (s32)luaL_checkinteger(L, 1);
	lua_pushboolean(L, chraiLuaChrYscale(chrnum, (f32)luaL_checknumber(L, 2)) != 0);
	return 1;
}

/* pd.shake(ticks) -> bool. Explosion-style screen shake for N ticks. */
static int l_pd_shake(lua_State *L)
{
	lua_pushboolean(L, chraiLuaShake((s32)luaL_optinteger(L, 1, 24)) != 0);
	return 1;
}

/* pd.screen_tint(r, g, b) -> bool. Full-screen luminance tint (sepia,
 * terminal green, ...). pd.screen_tint() clears it. */
static int l_pd_screen_tint(lua_State *L)
{
	if (lua_gettop(L) == 0 || lua_isnil(L, 1)) {
		lua_pushboolean(L, chraiLuaScreenTint(0, 0, 0, 0) != 0);
		return 1;
	}
	lua_pushboolean(L, chraiLuaScreenTint(
			(s32)luaL_checkinteger(L, 1),
			(s32)luaL_checkinteger(L, 2),
			(s32)luaL_checkinteger(L, 3), 1) != 0);
	return 1;
}

/* pd.upside_down(on) -> bool. Australia mode: rotate the whole frame 180 and
 * reverse the controls. */
static int l_pd_upside_down(lua_State *L)
{
	lua_pushboolean(L, chraiLuaUpsideDown(lua_toboolean(L, 1)) != 0);
	return 1;
}

/* pd.screen_roll(deg) -> bool. Do a Barrel Roll: rotate the 3D view about the
 * screen centre by an absolute angle in degrees (0 = upright/off). Animate by
 * re-setting each tick. */
static int l_pd_screen_roll(lua_State *L)
{
	f32 deg = (f32)luaL_optnumber(L, 1, 0.0);
	lua_pushboolean(L, chraiLuaScreenRoll(deg) != 0);
	return 1;
}

/* pd.player_add_yaw(deg) -> bool. Speen: rotate the player's view yaw by deg
 * degrees (spins the real player — view, aim, heading). */
static int l_pd_player_add_yaw(lua_State *L)
{
	f32 deg = (f32)luaL_checknumber(L, 1);
	lua_pushboolean(L, chraiLuaPlayerAddYaw(deg) != 0);
	return 1;
}

/* pd.player_slip(push [, pitch_deg]) -> bool. Banana peel: full squat +
 * forward shove (knockback-style, collision-respecting); pitch only when
 * given (the effect glides it via pd.player_pitch instead). */
static int l_pd_player_slip(lua_State *L)
{
	f32 push = (f32)luaL_optnumber(L, 1, 25.0);
	f32 pitch = (f32)luaL_optnumber(L, 2, 999.0); /* > 180 = leave pitch alone */
	lua_pushboolean(L, chraiLuaPlayerSlip(push, pitch) != 0);
	return 1;
}

/* pd.player_push(mag) -> bool. Shove the player along their facing (positive
 * forward, negative backward) — knockback-style, collision-respecting. */
static int l_pd_player_push(lua_State *L)
{
	f32 mag = (f32)luaL_checknumber(L, 1);
	lua_pushboolean(L, chraiLuaPlayerPush(mag) != 0);
	return 1;
}

/* pd.one_bullet(on) -> bool. One Bullet Mags: clip capacity 1 (equip-baked). */
static int l_pd_one_bullet(lua_State *L)
{
	lua_pushboolean(L, chraiLuaOneBullet(lua_toboolean(L, 1)) != 0);
	return 1;
}

/* pd.invert_look(on) -> bool. Flip vertical look (mouse/gyro/right stick). */
static int l_pd_invert_look(lua_State *L)
{
	lua_pushboolean(L, chraiLuaInvertLook(lua_toboolean(L, 1)) != 0);
	return 1;
}

/* pd.input_delay(frames) -> bool. Stadia Mode: pad reads served N frames
 * late; 0 = off. Mouse look stays live. */
static int l_pd_input_delay(lua_State *L)
{
	s32 frames = (s32)luaL_optinteger(L, 1, 0);
	lua_pushboolean(L, chraiLuaInputDelay(frames) != 0);
	return 1;
}

/* pd.uwuify(on) -> bool. Evewy stwing in the game, uwuified. */
static int l_pd_uwuify(lua_State *L)
{
	lua_pushboolean(L, chraiLuaUwuify(lua_toboolean(L, 1)) != 0);
	return 1;
}

/* pd.piglatin(on) -> bool. Everyway ingstray, igpay atinlay. */
static int l_pd_piglatin(lua_State *L)
{
	lua_pushboolean(L, chraiLuaPigLatin(lua_toboolean(L, 1)) != 0);
	return 1;
}

/* pd.forced_march(on) -> bool. Movement stick pinned full forward. */
static int l_pd_forced_march(lua_State *L)
{
	lua_pushboolean(L, chraiLuaForcedMarch(lua_toboolean(L, 1)) != 0);
	return 1;
}

/* pd.forced_fire(on) -> bool. Itchy Trigger Finger: trigger held for you. */
static int l_pd_forced_fire(lua_State *L)
{
	lua_pushboolean(L, chraiLuaForcedFire(lua_toboolean(L, 1)) != 0);
	return 1;
}

/* pd.hud_off(on) -> bool. No HUD: hide every HUD element. */
static int l_pd_hud_off(lua_State *L)
{
	lua_pushboolean(L, chraiLuaHudOff(lua_toboolean(L, 1)) != 0);
	return 1;
}

/* pd.gun_fov(deg) -> bool. Viewmodel FOV override; 0 restores. */
static int l_pd_gun_fov(lua_State *L)
{
	f32 deg = (f32)luaL_optnumber(L, 1, 0.0);
	lua_pushboolean(L, chraiLuaGunFov(deg) != 0);
	return 1;
}

/* pd.chr_freeze_one(chrnum | -1) -> bool. Statue exactly one chr. */
static int l_pd_chr_freeze_one(lua_State *L)
{
	s32 chrnum = (s32)luaL_optinteger(L, 1, -1);
	lua_pushboolean(L, chraiLuaChrFreezeOne(chrnum) != 0);
	return 1;
}

/* pd.buttsbot(on) -> bool. Random-but-stable words become butt. */
static int l_pd_buttsbot(lua_State *L)
{
	lua_pushboolean(L, chraiLuaButtsbot(lua_toboolean(L, 1)) != 0);
	return 1;
}

/* pd.ipod_ad(on [, r, g, b]) -> bool. Silhouette mode: bright walls, black
 * chrs, white objects/weapons, white wireframe edges. */
static int l_pd_ipod_ad(lua_State *L)
{
	s32 on = lua_toboolean(L, 1);
	s32 r = (s32)luaL_optinteger(L, 2, 0);
	s32 g = (s32)luaL_optinteger(L, 3, 217);
	s32 b = (s32)luaL_optinteger(L, 4, 140);
	lua_pushboolean(L, chraiLuaIpodAd(on, r, g, b) != 0);
	return 1;
}

/* pd.player_name() -> string. The solo save file's agent name — whatever the
 * player typed when creating their file ("PD" by default). */
static int l_pd_player_name(lua_State *L)
{
	lua_pushstring(L, g_GameFile.name);
	return 1;
}

/* pd.list_images() -> { "gras", "red", ... }. The basenames (no extension) of
 * every .png in scripts/images/. Nepotism uses this for its random fallback. */
static int l_pd_list_images(lua_State *L)
{
#ifndef PLATFORM_N64
	static char names[64][64];
	s32 n = extImageList(names, 64);
	s32 i;

	lua_createtable(L, n, 0);
	for (i = 0; i < n; i++) {
		lua_pushstring(L, names[i]);
		lua_rawseti(L, -2, i + 1);
	}
#else
	lua_createtable(L, 0, 0);
#endif
	return 1;
}

/* pd.player_pitch([deg]) -> deg | bool. No arg: current view pitch (+up).
 * With arg: set it (clamped +/-90). */
static int l_pd_player_pitch(lua_State *L)
{
	if (lua_gettop(L) < 1 || lua_isnil(L, 1)) {
		lua_pushnumber(L, chraiLuaPlayerPitchGet());
	} else {
		lua_pushboolean(L, chraiLuaPlayerPitchSet((f32)luaL_checknumber(L, 1)) != 0);
	}
	return 1;
}

/* pd.beyblade(on) -> bool. Bayblade!: spin every NPC's model yaw at ~2 rev/s
 * (visual only — AI keeps running). */
static int l_pd_beyblade(lua_State *L)
{
	lua_pushboolean(L, chraiLuaBeyblade(lua_toboolean(L, 1)) != 0);
	return 1;
}

/* pd.double_vision(on) -> bool. One too many: blend a 180-flipped ghost of the
 * frame over the normal one (drunk double-vision). */
static int l_pd_double_vision(lua_State *L)
{
	lua_pushboolean(L, chraiLuaDoubleVision(lua_toboolean(L, 1)) != 0);
	return 1;
}

/* pd.pixelate(w, h, colours) -> bool. Pixelate the rendered frame down to a
 * w x h grid; colours 4 = 4-level greyscale, 256 = 256-colour RGB 3-3-2,
 * 1000 = invert, 1001 = Game Boy greens, 1002 = thermal palette, 0/absent =
 * keep colours. w = 0 with a colour set = colour mode at full resolution.
 * pd.pixelate() turns it off. */
static int l_pd_pixelate(lua_State *L)
{
	if (lua_gettop(L) == 0 || lua_isnil(L, 1)) {
		lua_pushboolean(L, chraiLuaPixelate(0, 0, 0) != 0);
		return 1;
	}
	lua_pushboolean(L, chraiLuaPixelate(
			(s32)luaL_checkinteger(L, 1),
			(s32)luaL_checkinteger(L, 2),
			(s32)luaL_optinteger(L, 3, 0)) != 0);
	return 1;
}

/* pd.screen_fx(bits, on) -> bool. Set/clear post-filter effect bits: 1 =
 * scanlines, 2 = RGB grille, 4 = CRT curvature, 8 = vignette, 16 = VHS,
 * 32 = underwater wobble. Bits compose across effects. */
static int l_pd_screen_fx(lua_State *L)
{
	lua_pushboolean(L, chraiLuaScreenFx(
			(s32)luaL_checkinteger(L, 1), lua_toboolean(L, 2)) != 0);
	return 1;
}

/* pd.hudvd(on) -> nil. HUDVD chaos: each HUD element group (health, crosshair,
 * ammo, radar, messages, kill-feed) bounces DVD-style in its own random
 * diagonal. Purely cosmetic — aim/hit-detection are untouched. */
static int l_pd_hudvd(lua_State *L)
{
#ifndef PLATFORM_N64
	extern void hudvdSetActive(bool on);
	hudvdSetActive((bool)lua_toboolean(L, 1));
#endif
	return 0;
}

/* pd.crt(on) -> bool. The full CRT look: curved scanlines + RGB aperture
 * grille + tube curvature + vignette (screen_fx bits 1|2|4|8). */
static int l_pd_crt(lua_State *L)
{
	lua_pushboolean(L, chraiLuaScreenFx(1 | 2 | 4 | 8, lua_toboolean(L, 1)) != 0);
	return 1;
}

/* pd.lens(k) -> bool. Fisheye lens warp (centre magnified, corners pinned);
 * k ~ 1.4 = peephole, negative = pincushion, 0/absent = off. */
static int l_pd_lens(lua_State *L)
{
	lua_pushboolean(L, chraiLuaLens((f32)luaL_optnumber(L, 1, 0.0)) != 0);
	return 1;
}

/* pd.audio_crush(step, bits) -> bool. Crunch all audio: sample-and-hold every
 * `step`th output frame (device rate 22 kHz / step) masked to `bits` bit
 * depth. pd.audio_crush() restores clean audio. */
static int l_pd_audio_crush(lua_State *L)
{
	lua_pushboolean(L, chraiLuaAudioCrush(
			(s32)luaL_optinteger(L, 1, 1),
			(s32)luaL_optinteger(L, 2, 16)) != 0);
	return 1;
}

/* pd.audio_radio(on) -> bool. AM-radio voicing: ~400..2800Hz bandpass +
 * overdrive on everything. */
static int l_pd_audio_radio(lua_State *L)
{
	lua_pushboolean(L, chraiLuaAudioRadio(lua_toboolean(L, 1)) != 0);
	return 1;
}

/* pd.audio_reverb(wet) -> bool. Cathedral reverb wash, wet 0..1;
 * pd.audio_reverb() turns it off. */
static int l_pd_audio_reverb(lua_State *L)
{
	lua_pushboolean(L, chraiLuaAudioReverb((f32)luaL_optnumber(L, 1, 0.0)) != 0);
	return 1;
}

/* pd.audio_reverse(on) -> bool. All audio plays backwards in ~0.74s
 * granules (with that much latency). */
static int l_pd_audio_reverse(lua_State *L)
{
	lua_pushboolean(L, chraiLuaAudioReverse(lua_toboolean(L, 1)) != 0);
	return 1;
}

/* pd.audio_pitch(rate) -> bool. Pitch shift at constant tempo: 1.5 =
 * helium, 0.65 = demon. pd.audio_pitch() restores normal pitch. */
static int l_pd_audio_pitch(lua_State *L)
{
	lua_pushboolean(L, chraiLuaAudioPitch((f32)luaL_optnumber(L, 1, 1.0)) != 0);
	return 1;
}

/* pd.weather(type, intensity) -> bool. 0 = off, 1 = rain, 2 = snow — on any
 * stage (unconfigured stages rain indoors too; that's the joke). */
static int l_pd_weather(lua_State *L)
{
	s32 type = (s32)luaL_optinteger(L, 1, 0);
	s32 intensity = (s32)luaL_optinteger(L, 2, 2);
	lua_pushboolean(L, chraiLuaWeather(type, intensity) != 0);
	return 1;
}

/* pd.gas(on) -> bool. The Investigation nerve gas anywhere: green env wash,
 * coughing, positional hiss, periodic damage. */
static int l_pd_gas(lua_State *L)
{
	lua_pushboolean(L, chraiLuaGas(lua_toboolean(L, 1)) != 0);
	return 1;
}

/* pd.t_pose(on) -> bool. Every skeletal model renders in its bind pose. */
static int l_pd_t_pose(lua_State *L)
{
	lua_pushboolean(L, chraiLuaTPose(lua_toboolean(L, 1)) != 0);
	return 1;
}

/* pd.chr_ko(chrnum) -> bool. Tranquiliser-style knockout: the chr collapses
 * and drops its weapon. The body is parked un-reaped (engine KOs are
 * otherwise permanent, and reaped chrs read as eliminated to mission
 * scripts) — wake it with pd.chr_wake. */
static int l_pd_chr_ko(lua_State *L)
{
	lua_pushboolean(L, chraiLuaChrKo((s32)luaL_checkinteger(L, 1)) != 0);
	return 1;
}

/* pd.chr_wake(chrnum) -> bool. Recover a KO'd chr: blends back to standing
 * over ~half a second and normal AI resumes (unarmed — the KO dropped their
 * weapons). No-op unless the chr is in one of the drugged states. */
static int l_pd_chr_wake(lua_State *L)
{
	lua_pushboolean(L, chraiLuaChrWake((s32)luaL_checkinteger(L, 1)) != 0);
	return 1;
}

/* pd.pinball(on) -> bool. Fired physics projectiles (rockets, grenade rounds)
 * become bouncing proximity pinballs. */
static int l_pd_pinball(lua_State *L)
{
	lua_pushboolean(L, chraiLuaPinball(lua_toboolean(L, 1)) != 0);
	return 1;
}

/* pd.room_tint(r, g, b) -> bool. Tint every room's lighting (0..255 per
 * channel). pd.room_tint() with no args turns the tint off. */
static int l_pd_room_tint(lua_State *L)
{
	if (lua_gettop(L) == 0 || lua_isnil(L, 1)) {
		lua_pushboolean(L, chraiLuaRoomTint(255, 255, 255, 0) != 0);
		return 1;
	}
	lua_pushboolean(L, chraiLuaRoomTint(
			(s32)luaL_checkinteger(L, 1),
			(s32)luaL_checkinteger(L, 2),
			(s32)luaL_checkinteger(L, 3), 1) != 0);
	return 1;
}

/* pd.explosions_around(on) -> bool. The Air Force One crash sequence:
 * staggered explosions surround the player until turned off. */
static int l_pd_explosions_around(lua_State *L)
{
	lua_pushboolean(L, chraiLuaPlayerExplosions(lua_toboolean(L, 1)) != 0);
	return 1;
}

/* pd.ammo_swap(weaponnum) -> bool. Held guns fire this weapon's primary
 * rounds (must be a SHOOT-type function). pd.ammo_swap() turns it off. */
static int l_pd_ammo_swap(lua_State *L)
{
	s32 weaponnum = (s32)luaL_optinteger(L, 1, -1);
	lua_pushboolean(L, chraiLuaAmmoSwap(weaponnum) != 0);
	return 1;
}

/* pd.backfire(on) -> bool. Shots leave 180 degrees behind the player. */
static int l_pd_backfire(lua_State *L)
{
	lua_pushboolean(L, chraiLuaBackfire(lua_toboolean(L, 1)) != 0);
	return 1;
}

/* pd.nbomb() -> bool. N-Bomb storm on the player. */
static int l_pd_nbomb(lua_State *L)
{
	lua_pushboolean(L, chraiLuaNbomb() != 0);
	return 1;
}

/* pd.gust([force]) -> bool. Shove everything in one random direction. */
static int l_pd_gust(lua_State *L)
{
	f32 force = (f32)luaL_optnumber(L, 1, 150.0);
	lua_pushboolean(L, chraiLuaGust(force) != 0);
	return 1;
}

/* pd.dual_wield(weaponnum [, funcnum]) -> bool. Dual-equip a weapon with
 * full ammo; funcnum 0/1 also forces that fire function on both hands. */
static int l_pd_dual_wield(lua_State *L)
{
	s32 weaponnum = (s32)luaL_checkinteger(L, 1);
	s32 funcnum = (s32)luaL_optinteger(L, 2, -1);
	lua_pushboolean(L, chraiLuaDualWield(weaponnum, funcnum) != 0);
	return 1;
}

/* pd.gun_lock(on) -> bool. Cyclone Frenzy: force secondary fire on both hands,
 * hold the trigger (auto-fire), and block weapon switching. */
static int l_pd_gun_lock(lua_State *L)
{
	lua_pushboolean(L, chraiLuaGunLock(lua_toboolean(L, 1)) != 0);
	return 1;
}

/* pd.mag_dump(on) -> bool. Mag Dump: one trigger press empties the whole clip —
 * automatic weapons hold the trigger, semi-autos rapidly pulse it. */
static int l_pd_mag_dump(lua_State *L)
{
	lua_pushboolean(L, chraiLuaMagDump(lua_toboolean(L, 1)) != 0);
	return 1;
}

/* pd.knife_lock(on) -> bool. Knife fight: block weapon switching only. */
static int l_pd_knife_lock(lua_State *L)
{
	lua_pushboolean(L, chraiLuaKnifeLock(lua_toboolean(L, 1)) != 0);
	return 1;
}

/* pd.aspect_scale([mult]) -> bool. Projection aspect multiplier: 2 = extra
 * wide, 0.5 = extra tall, 1 / no arg = normal. */
static int l_pd_aspect_scale(lua_State *L)
{
	f32 mult = (f32)luaL_optnumber(L, 1, 1.0);
	lua_pushboolean(L, chraiLuaAspectScale(mult) != 0);
	return 1;
}

/* pd.song(slot) / pd.song() -> bool. Play an unlocked Combat Sim track over
 * the stage music (slot wraps into range); no arg stops it. */
static int l_pd_song(lua_State *L)
{
	s32 slot = (s32)luaL_optinteger(L, 1, -1);
	lua_pushboolean(L, chraiLuaPlaySong(slot) != 0);
	return 1;
}

/* pd.stage_music(on) -> bool. Stop (on=false) or restart (on=true) the current
 * stage's music. Backs Silo Countdown, which kills the level track and plays its
 * own external file underneath. */
static int l_pd_stage_music(lua_State *L)
{
	lua_pushboolean(L, chraiLuaStageMusic(lua_toboolean(L, 1)) != 0);
	return 1;
}

/* pd.spawn_body(bodynum [, weaponnum, dx, dz, sunglasses]) -> chrnum | -1.
 * Spawn a hostile chr of the given body at the player plus a horizontal
 * offset. sunglasses=true forces the head's shades variant (Terminator). */
static int l_pd_spawn_body(lua_State *L)
{
	s32 bodynum = (s32)luaL_checkinteger(L, 1);
	s32 weaponnum = (s32)luaL_optinteger(L, 2, -1);
	f32 dx = (f32)luaL_optnumber(L, 3, 0.0);
	f32 dz = (f32)luaL_optnumber(L, 4, 0.0);
	s32 sunglasses = lua_toboolean(L, 5);
	lua_pushinteger(L, chraiLuaSpawnBody(bodynum, weaponnum, dx, dz, sunglasses));
	return 1;
}

/* pd.body_snatch(chrnum) -> bool. Lite Counter-Op takeover: take the guard's
 * place (its weapon + position + disguise; the guard is removed). Solo only. */
static int l_pd_body_snatch(lua_State *L)
{
	s32 chrnum = (s32)luaL_checkinteger(L, 1);
	lua_pushboolean(L, chraiLuaBodySnatch(chrnum) != 0);
	return 1;
}

/* pd.body_unsnatch(): end the snatch — drop the disguise and teleport home. */
static int l_pd_body_unsnatch(lua_State *L)
{
	lua_pushboolean(L, chraiLuaBodyUnsnatch() != 0);
	return 1;
}

/* pd.chr_target(chrnum, victimchrnum) -> bool. Point a chr's AI at a chr. */
static int l_pd_chr_target(lua_State *L)
{
	s32 chrnum = (s32)luaL_checkinteger(L, 1);
	s32 victim = (s32)luaL_checkinteger(L, 2);
	lua_pushboolean(L, chraiLuaChrTarget(chrnum, victim) != 0);
	return 1;
}

/* pd.chr_calm(chrnum) -> bool. Zero a chr's alertness and target. */
static int l_pd_chr_calm(lua_State *L)
{
	s32 chrnum = (s32)luaL_checkinteger(L, 1);
	lua_pushboolean(L, chraiLuaChrCalm(chrnum) != 0);
	return 1;
}

/* pd.doors_all(open) -> count. Open (true) / close (false) every door. */
static int l_pd_doors_all(lua_State *L)
{
	lua_pushinteger(L, chraiLuaDoorsAll(lua_toboolean(L, 1)));
	return 1;
}

/* pd.doors_lock(on) -> count. Lockdown: lock (true) / unlock (false) every door. */
static int l_pd_doors_lock(lua_State *L)
{
	lua_pushinteger(L, chraiLuaDoorsLock(lua_toboolean(L, 1)));
	return 1;
}

/* pd.civil_war(on) -> bool. Turn NPCs on each other (nearest neighbour, hostile
 * teams); call again with true to re-assert, false to restore. */
static int l_pd_civil_war(lua_State *L)
{
	lua_pushboolean(L, chraiLuaCivilWar(lua_toboolean(L, 1)) != 0);
	return 1;
}

/* pd.chr_summon(chrnum [, dx, dz]) -> bool. Teleport a chr to the player. */
static int l_pd_chr_summon(lua_State *L)
{
	s32 chrnum = (s32)luaL_checkinteger(L, 1);
	f32 dx = (f32)luaL_optnumber(L, 2, 0.0);
	f32 dz = (f32)luaL_optnumber(L, 3, 0.0);
	lua_pushboolean(L, chraiLuaChrSummon(chrnum, dx, dz) != 0);
	return 1;
}

/* pd.fov_scale([mult]) -> bool. Vertical-FOV multiplier: >1 fisheye,
 * <1 tunnel vision, 1 / no arg = normal. */
static int l_pd_fov_scale(lua_State *L)
{
	f32 mult = (f32)luaL_optnumber(L, 1, 1.0);
	lua_pushboolean(L, chraiLuaFovScale(mult) != 0);
	return 1;
}

/* pd.one_punch(on) -> bool. Unarmed strikes: lethal + mega knockback. */
static int l_pd_one_punch(lua_State *L)
{
	lua_pushboolean(L, chraiLuaOnePunch(lua_toboolean(L, 1)) != 0);
	return 1;
}

/* pd.gormless(on) -> bool. Invert movement + look axes. */
static int l_pd_gormless(lua_State *L)
{
	lua_pushboolean(L, chraiLuaGormless(lua_toboolean(L, 1)) != 0);
	return 1;
}

/* pd.spawn_bike() -> bool. Half-size hoverbike at the player (solo only). */
static int l_pd_spawn_bike(lua_State *L)
{
	lua_pushboolean(L, chraiLuaSpawnBike() != 0);
	return 1;
}

/* pd.sfx_shuffle(on) -> bool. Every SFX plays as a random other SFX. */
static int l_pd_sfx_shuffle(lua_State *L)
{
	lua_pushboolean(L, chraiLuaSfxShuffle(lua_toboolean(L, 1)) != 0);
	return 1;
}

/* pd.instrument_shuffle(on) -> bool. Random instruments on program change. */
static int l_pd_instrument_shuffle(lua_State *L)
{
	lua_pushboolean(L, chraiLuaInstrumentShuffle(lua_toboolean(L, 1)) != 0);
	return 1;
}

/* --- External event queue (the Twitch/YouTube window) ----------------------
 * A tiny {source, text} ring fed by C (console /chaos, the Chaos.EventPort
 * UDP listener in net.c, or any future embedded chat bridge) and drained by
 * Lua via pd.ext_poll(). Text is opaque to C — the protocol lives entirely in
 * scripts/chaos.lua, so a stream bot can grow new verbs without a rebuild. */
#define LUA_EXTEV_MAX  32
#define LUA_EXTEV_TEXT 128
struct luaextevent {
	char source[16];
	char text[LUA_EXTEV_TEXT];
};
static struct luaextevent g_LuaExtEvents[LUA_EXTEV_MAX];
static u32 g_LuaExtEvHead; /* next write */
static u32 g_LuaExtEvTail; /* next read */

void luaExtEventPush(const char *source, const char *text)
{
	struct luaextevent *ev;
	if (!text || !text[0]) {
		return;
	}
	if (g_LuaExtEvHead - g_LuaExtEvTail >= LUA_EXTEV_MAX) {
		g_LuaExtEvTail++; /* overflow: drop the oldest (chat spam friendly) */
	}
	ev = &g_LuaExtEvents[g_LuaExtEvHead % LUA_EXTEV_MAX];
	snprintf(ev->source, sizeof(ev->source), "%s", source ? source : "?");
	snprintf(ev->text, sizeof(ev->text), "%s", text);
	g_LuaExtEvHead++;
}

/* Optional localhost UDP ingress, implemented in net.c (socket infra lives
 * there); drained lazily whenever Lua polls so no per-frame hook is needed.
 * Weak default: absent in builds without net (never true on this branch). */
extern void netChaosEventDrain(void);

/* pd.ext_poll() -> source, text | nil. Pop one external event. */
static int l_pd_ext_poll(lua_State *L)
{
	struct luaextevent *ev;
	netChaosEventDrain();
	if (g_LuaExtEvTail == g_LuaExtEvHead) {
		lua_pushnil(L);
		return 1;
	}
	ev = &g_LuaExtEvents[g_LuaExtEvTail % LUA_EXTEV_MAX];
	g_LuaExtEvTail++;
	lua_pushstring(L, ev->source);
	lua_pushstring(L, ev->text);
	return 2;
}

void luaApiRegister(lua_State *L)
{
	/* create the events registry table (replaces any previous one) */
	lua_newtable(L);
	lua_setfield(L, LUA_REGISTRYINDEX, KEY_EVENTS);

	/* pd.* functions (pd table is at -1) */
	lua_pushcfunction(L, l_pd_on);          lua_setfield(L, -2, "on");
	lua_pushcfunction(L, l_pd_draw_box);    lua_setfield(L, -2, "draw_box");
	lua_pushcfunction(L, l_pd_draw_sprite); lua_setfield(L, -2, "draw_sprite");
	lua_pushcfunction(L, l_pd_load_image);  lua_setfield(L, -2, "load_image");
	lua_pushcfunction(L, l_pd_tex_override); lua_setfield(L, -2, "tex_override");
	lua_pushcfunction(L, l_pd_draw_image);  lua_setfield(L, -2, "draw_image");
	lua_pushcfunction(L, l_pd_draw_text);   lua_setfield(L, -2, "draw_text");
	lua_pushcfunction(L, l_pd_hud_message); lua_setfield(L, -2, "hud_message");
	lua_pushcfunction(L, l_pd_each_chr);    lua_setfield(L, -2, "each_chr");
#ifndef PLATFORM_N64
	lua_pushcfunction(L, l_pd_octree_stats);lua_setfield(L, -2, "octree_stats");
	lua_pushcfunction(L, l_pd_dlcache_stats);lua_setfield(L, -2, "dlcache_stats");
	lua_pushcfunction(L, l_pd_perf);        lua_setfield(L, -2, "perf");
#endif
	/* world / entity queries */
	lua_pushcfunction(L, l_pd_chr_info);    lua_setfield(L, -2, "chr_info");
	lua_pushcfunction(L, l_pd_chr_pos);     lua_setfield(L, -2, "chr_pos");
	lua_pushcfunction(L, l_pd_chr_health);  lua_setfield(L, -2, "chr_health");
	lua_pushcfunction(L, l_pd_player_pos);  lua_setfield(L, -2, "player_pos");
	lua_pushcfunction(L, l_pd_player_count);lua_setfield(L, -2, "player_count");
	lua_pushcfunction(L, l_pd_stage);       lua_setfield(L, -2, "stage");
	lua_pushcfunction(L, l_pd_text_size);   lua_setfield(L, -2, "text_size");
	lua_pushcfunction(L, l_pd_distance);    lua_setfield(L, -2, "distance");
	/* world mutation (server-side) */
	lua_pushcfunction(L, l_pd_spawn_at_chr);lua_setfield(L, -2, "spawn_at_chr");
	lua_pushcfunction(L, l_pd_spawn);       lua_setfield(L, -2, "spawn");
	/* toolkit: all-actor iteration + per-chr mutators (server-side) */
	lua_pushcfunction(L, l_pd_all_chrs);    lua_setfield(L, -2, "all_chrs");
	lua_pushcfunction(L, l_pd_chr_anim);    lua_setfield(L, -2, "chr_anim");
	lua_pushcfunction(L, l_pd_chr_set_shield); lua_setfield(L, -2, "chr_set_shield");
	lua_pushcfunction(L, l_pd_chr_alert);   lua_setfield(L, -2, "chr_alert");
	lua_pushcfunction(L, l_pd_chr_set_body); lua_setfield(L, -2, "chr_set_body");
	lua_pushcfunction(L, l_pd_possess_spawn); lua_setfield(L, -2, "possess_spawn");
	lua_pushcfunction(L, l_pd_unpossess);   lua_setfield(L, -2, "unpossess");
	/* archipelago bonus / buff API (server-side) */
	lua_pushcfunction(L, l_pd_player_heal);      lua_setfield(L, -2, "player_heal");
	lua_pushcfunction(L, l_pd_player_set_shield);lua_setfield(L, -2, "player_set_shield");
	lua_pushcfunction(L, l_pd_refill_ammo);      lua_setfield(L, -2, "refill_ammo");
	lua_pushcfunction(L, l_pd_give_ammo);        lua_setfield(L, -2, "give_ammo");
	lua_pushcfunction(L, l_pd_give_weapon);      lua_setfield(L, -2, "give_weapon");
	lua_pushcfunction(L, l_pd_device_on);        lua_setfield(L, -2, "device_on");
	lua_pushcfunction(L, l_pd_device_off);       lua_setfield(L, -2, "device_off");
	lua_pushcfunction(L, l_pd_lvupdate);         lua_setfield(L, -2, "lvupdate");
	lua_pushcfunction(L, l_pd_mission_complete); lua_setfield(L, -2, "mission_complete");
	lua_pushcfunction(L, l_pd_invincible);       lua_setfield(L, -2, "invincible");
	lua_pushcfunction(L, l_pd_spawn_ally);       lua_setfield(L, -2, "spawn_ally");
	lua_pushcfunction(L, l_pd_spawn_ally_clone); lua_setfield(L, -2, "spawn_ally_clone");
	/* director pause-menu registry */
	lua_pushcfunction(L, l_pd_menu_add);    lua_setfield(L, -2, "menu_add");
	lua_pushcfunction(L, l_pd_menu_clear);  lua_setfield(L, -2, "menu_clear");
	lua_pushcfunction(L, l_pd_menu_set_label); lua_setfield(L, -2, "menu_set_label");
	/* session-persistent KV (survives the per-stage lua_State teardown) */
	lua_pushcfunction(L, l_pd_persist_get); lua_setfield(L, -2, "persist_get");
	lua_pushcfunction(L, l_pd_persist_set); lua_setfield(L, -2, "persist_set");
	/* archipelago gating */
	lua_pushcfunction(L, l_pd_ap_mode);     lua_setfield(L, -2, "ap_mode");
	lua_pushcfunction(L, l_pd_unlock);      lua_setfield(L, -2, "unlock");
	lua_pushcfunction(L, l_pd_lock);        lua_setfield(L, -2, "lock");
	lua_pushcfunction(L, l_pd_is_unlocked); lua_setfield(L, -2, "is_unlocked");
	lua_pushcfunction(L, l_pd_ap_reset);    lua_setfield(L, -2, "ap_reset");
	lua_pushcfunction(L, l_pd_ap_list_header); lua_setfield(L, -2, "ap_list_header");

	/* chaos-mode primitives + external event ingress (docs/PORT_CHAOS.md) */
	lua_pushcfunction(L, l_pd_cheat);         lua_setfield(L, -2, "cheat");
	lua_pushcfunction(L, l_pd_cheat_active);  lua_setfield(L, -2, "cheat_active");
	lua_pushcfunction(L, l_pd_sound);         lua_setfield(L, -2, "sound");
	lua_pushcfunction(L, l_pd_take_weapon);   lua_setfield(L, -2, "take_weapon");
	lua_pushcfunction(L, l_pd_weapon_held);   lua_setfield(L, -2, "weapon_held");
	lua_pushcfunction(L, l_pd_switch_weapon); lua_setfield(L, -2, "switch_weapon");
	lua_pushcfunction(L, l_pd_fade);          lua_setfield(L, -2, "fade");
	lua_pushcfunction(L, l_pd_chr_yeet);      lua_setfield(L, -2, "chr_yeet");
	lua_pushcfunction(L, l_pd_explosion);     lua_setfield(L, -2, "explosion");
	lua_pushcfunction(L, l_pd_explosion_at);  lua_setfield(L, -2, "explosion_at");
	lua_pushcfunction(L, l_pd_grenade);       lua_setfield(L, -2, "grenade");
	lua_pushcfunction(L, l_pd_input_source);  lua_setfield(L, -2, "input_source");
	lua_pushcfunction(L, l_pd_door_traps);    lua_setfield(L, -2, "door_traps");
	lua_pushcfunction(L, l_pd_door_opens);    lua_setfield(L, -2, "door_opens");
	lua_pushcfunction(L, l_pd_env_colours);   lua_setfield(L, -2, "env_colours");
	lua_pushcfunction(L, l_pd_player_yaw);    lua_setfield(L, -2, "player_yaw");
	lua_pushcfunction(L, l_pd_player_crouch); lua_setfield(L, -2, "player_crouch");
	lua_pushcfunction(L, l_pd_has_weapon);    lua_setfield(L, -2, "has_weapon");
	lua_pushcfunction(L, l_pd_bio_count);     lua_setfield(L, -2, "bio_count");
	lua_pushcfunction(L, l_pd_bio_text);      lua_setfield(L, -2, "bio_text");
	lua_pushcfunction(L, l_pd_ext_poll);      lua_setfield(L, -2, "ext_poll");
	lua_pushcfunction(L, l_pd_alarm);         lua_setfield(L, -2, "alarm");
	lua_pushcfunction(L, l_pd_boost);         lua_setfield(L, -2, "boost");
	lua_pushcfunction(L, l_pd_player_set_health); lua_setfield(L, -2, "player_set_health");
	lua_pushcfunction(L, l_pd_dizzy);         lua_setfield(L, -2, "dizzy");
	lua_pushcfunction(L, l_pd_chr_cloak);     lua_setfield(L, -2, "chr_cloak");
	lua_pushcfunction(L, l_pd_strip_ammo);    lua_setfield(L, -2, "strip_ammo");
	lua_pushcfunction(L, l_pd_teleport_to_chr); lua_setfield(L, -2, "teleport_to_chr");
	lua_pushcfunction(L, l_pd_flattex);       lua_setfield(L, -2, "flattex");
	lua_pushcfunction(L, l_pd_shiny);         lua_setfield(L, -2, "shiny");
	lua_pushcfunction(L, l_pd_chr_give_weapon); lua_setfield(L, -2, "chr_give_weapon");
	lua_pushcfunction(L, l_pd_chr_weapon);    lua_setfield(L, -2, "chr_weapon");
	lua_pushcfunction(L, l_pd_player_health); lua_setfield(L, -2, "player_health");
	lua_pushcfunction(L, l_pd_player_damage); lua_setfield(L, -2, "player_damage");
	lua_pushcfunction(L, l_pd_weapon_jam);    lua_setfield(L, -2, "weapon_jam");
	lua_pushcfunction(L, l_pd_force_secondary); lua_setfield(L, -2, "force_secondary");
	lua_pushcfunction(L, l_pd_button_block);  lua_setfield(L, -2, "button_block");
	lua_pushcfunction(L, l_pd_ammo_cost);     lua_setfield(L, -2, "ammo_cost");
	lua_pushcfunction(L, l_pd_autoaim);       lua_setfield(L, -2, "autoaim");
	lua_pushcfunction(L, l_pd_deadzone);      lua_setfield(L, -2, "deadzone");
	lua_pushcfunction(L, l_pd_nitro);         lua_setfield(L, -2, "nitro");
	lua_pushcfunction(L, l_pd_objective_force); lua_setfield(L, -2, "objective_force");
	lua_pushcfunction(L, l_pd_objective_status); lua_setfield(L, -2, "objective_status");
	lua_pushcfunction(L, l_pd_mark_home);     lua_setfield(L, -2, "mark_home");
	lua_pushcfunction(L, l_pd_warp_home);     lua_setfield(L, -2, "warp_home");
	lua_pushcfunction(L, l_pd_env);           lua_setfield(L, -2, "env");
	lua_pushcfunction(L, l_pd_fog);           lua_setfield(L, -2, "fog");
	lua_pushcfunction(L, l_pd_blood_colour);  lua_setfield(L, -2, "blood_colour");
	lua_pushcfunction(L, l_pd_max_blood);     lua_setfield(L, -2, "max_blood");
	lua_pushcfunction(L, l_pd_items_shuffle); lua_setfield(L, -2, "items_shuffle");
	lua_pushcfunction(L, l_pd_chr_wireframe); lua_setfield(L, -2, "chr_wireframe");
	lua_pushcfunction(L, l_pd_double_shots);  lua_setfield(L, -2, "double_shots");
	lua_pushcfunction(L, l_pd_quad_top);      lua_setfield(L, -2, "quad_top");
	lua_pushcfunction(L, l_pd_buttons);       lua_setfield(L, -2, "buttons");
	lua_pushcfunction(L, l_pd_buttons_pressed); lua_setfield(L, -2, "buttons_pressed");
	lua_pushcfunction(L, l_pd_spawn_chopper); lua_setfield(L, -2, "spawn_chopper");
	lua_pushcfunction(L, l_pd_player_freeze); lua_setfield(L, -2, "player_freeze");
	lua_pushcfunction(L, l_pd_chr_freeze);    lua_setfield(L, -2, "chr_freeze");
	lua_pushcfunction(L, l_pd_no_drops);      lua_setfield(L, -2, "no_drops");
	lua_pushcfunction(L, l_pd_paintball);     lua_setfield(L, -2, "paintball");
	lua_pushcfunction(L, l_pd_damage_scale);  lua_setfield(L, -2, "damage_scale");
	lua_pushcfunction(L, l_pd_zoom_scale);    lua_setfield(L, -2, "zoom_scale");
	lua_pushcfunction(L, l_pd_gun_sound);     lua_setfield(L, -2, "gun_sound");
	lua_pushcfunction(L, l_pd_mute);          lua_setfield(L, -2, "mute");
	lua_pushcfunction(L, l_pd_play_file);     lua_setfield(L, -2, "play_file");
	lua_pushcfunction(L, l_pd_stop_file);     lua_setfield(L, -2, "stop_file");
	lua_pushcfunction(L, l_pd_weapon_rename); lua_setfield(L, -2, "weapon_rename");
	lua_pushcfunction(L, l_pd_menu_lore);     lua_setfield(L, -2, "menu_lore");
	lua_pushcfunction(L, l_pd_game_over);     lua_setfield(L, -2, "game_over");
	lua_pushcfunction(L, l_pd_chr_speed);     lua_setfield(L, -2, "chr_speed");
	lua_pushcfunction(L, l_pd_player_speed);  lua_setfield(L, -2, "player_speed");
	lua_pushcfunction(L, l_pd_chr_damage);    lua_setfield(L, -2, "chr_damage");
	lua_pushcfunction(L, l_pd_chr_scale);     lua_setfield(L, -2, "chr_scale");
	lua_pushcfunction(L, l_pd_chr_yscale);    lua_setfield(L, -2, "chr_yscale");
	lua_pushcfunction(L, l_pd_shake);         lua_setfield(L, -2, "shake");
	lua_pushcfunction(L, l_pd_screen_tint);   lua_setfield(L, -2, "screen_tint");
	lua_pushcfunction(L, l_pd_upside_down);   lua_setfield(L, -2, "upside_down");
	lua_pushcfunction(L, l_pd_screen_roll);   lua_setfield(L, -2, "screen_roll");
	lua_pushcfunction(L, l_pd_player_add_yaw); lua_setfield(L, -2, "player_add_yaw");
	lua_pushcfunction(L, l_pd_player_slip);   lua_setfield(L, -2, "player_slip");
	lua_pushcfunction(L, l_pd_player_pitch);  lua_setfield(L, -2, "player_pitch");
	lua_pushcfunction(L, l_pd_player_push);   lua_setfield(L, -2, "player_push");
	lua_pushcfunction(L, l_pd_one_bullet);    lua_setfield(L, -2, "one_bullet");
	lua_pushcfunction(L, l_pd_invert_look);   lua_setfield(L, -2, "invert_look");
	lua_pushcfunction(L, l_pd_input_delay);   lua_setfield(L, -2, "input_delay");
	lua_pushcfunction(L, l_pd_uwuify);        lua_setfield(L, -2, "uwuify");
	lua_pushcfunction(L, l_pd_piglatin);      lua_setfield(L, -2, "piglatin");
	lua_pushcfunction(L, l_pd_forced_march);  lua_setfield(L, -2, "forced_march");
	lua_pushcfunction(L, l_pd_forced_fire);   lua_setfield(L, -2, "forced_fire");
	lua_pushcfunction(L, l_pd_hud_off);       lua_setfield(L, -2, "hud_off");
	lua_pushcfunction(L, l_pd_gun_fov);       lua_setfield(L, -2, "gun_fov");
	lua_pushcfunction(L, l_pd_chr_freeze_one); lua_setfield(L, -2, "chr_freeze_one");
	lua_pushcfunction(L, l_pd_buttsbot);      lua_setfield(L, -2, "buttsbot");
	lua_pushcfunction(L, l_pd_ipod_ad);       lua_setfield(L, -2, "ipod_ad");
	lua_pushcfunction(L, l_pd_player_name);   lua_setfield(L, -2, "player_name");
	lua_pushcfunction(L, l_pd_list_images);   lua_setfield(L, -2, "list_images");
	lua_pushcfunction(L, l_pd_beyblade);      lua_setfield(L, -2, "beyblade");
	lua_pushcfunction(L, l_pd_double_vision); lua_setfield(L, -2, "double_vision");
	lua_pushcfunction(L, l_pd_weather);       lua_setfield(L, -2, "weather");
	lua_pushcfunction(L, l_pd_gas);           lua_setfield(L, -2, "gas");
	lua_pushcfunction(L, l_pd_t_pose);        lua_setfield(L, -2, "t_pose");
	lua_pushcfunction(L, l_pd_chr_ko);        lua_setfield(L, -2, "chr_ko");
	lua_pushcfunction(L, l_pd_chr_wake);      lua_setfield(L, -2, "chr_wake");
	lua_pushcfunction(L, l_pd_pinball);       lua_setfield(L, -2, "pinball");
	lua_pushcfunction(L, l_pd_grayscale);     lua_setfield(L, -2, "grayscale");
	lua_pushcfunction(L, l_pd_room_tint);     lua_setfield(L, -2, "room_tint");
	lua_pushcfunction(L, l_pd_explosions_around); lua_setfield(L, -2, "explosions_around");
	lua_pushcfunction(L, l_pd_ammo_swap);     lua_setfield(L, -2, "ammo_swap");
	lua_pushcfunction(L, l_pd_backfire);      lua_setfield(L, -2, "backfire");
	lua_pushcfunction(L, l_pd_nbomb);         lua_setfield(L, -2, "nbomb");
	lua_pushcfunction(L, l_pd_gust);          lua_setfield(L, -2, "gust");
	lua_pushcfunction(L, l_pd_dual_wield);    lua_setfield(L, -2, "dual_wield");
	lua_pushcfunction(L, l_pd_gun_lock);      lua_setfield(L, -2, "gun_lock");
	lua_pushcfunction(L, l_pd_mag_dump);      lua_setfield(L, -2, "mag_dump");
	lua_pushcfunction(L, l_pd_knife_lock);    lua_setfield(L, -2, "knife_lock");
	lua_pushcfunction(L, l_pd_aspect_scale);  lua_setfield(L, -2, "aspect_scale");
	lua_pushcfunction(L, l_pd_song);          lua_setfield(L, -2, "song");
	lua_pushcfunction(L, l_pd_stage_music);   lua_setfield(L, -2, "stage_music");
	lua_pushcfunction(L, l_pd_spawn_body);    lua_setfield(L, -2, "spawn_body");
	lua_pushcfunction(L, l_pd_body_snatch);   lua_setfield(L, -2, "body_snatch");
	lua_pushcfunction(L, l_pd_body_unsnatch); lua_setfield(L, -2, "body_unsnatch");
	lua_pushcfunction(L, l_pd_chr_target);    lua_setfield(L, -2, "chr_target");
	lua_pushcfunction(L, l_pd_chr_calm);      lua_setfield(L, -2, "chr_calm");
	lua_pushcfunction(L, l_pd_doors_all);     lua_setfield(L, -2, "doors_all");
	lua_pushcfunction(L, l_pd_doors_lock);    lua_setfield(L, -2, "doors_lock");
	lua_pushcfunction(L, l_pd_civil_war);     lua_setfield(L, -2, "civil_war");
	lua_pushcfunction(L, l_pd_chr_summon);    lua_setfield(L, -2, "chr_summon");
	lua_pushcfunction(L, l_pd_fov_scale);     lua_setfield(L, -2, "fov_scale");
	lua_pushcfunction(L, l_pd_one_punch);     lua_setfield(L, -2, "one_punch");
	lua_pushcfunction(L, l_pd_gormless);      lua_setfield(L, -2, "gormless");
	lua_pushcfunction(L, l_pd_spawn_bike);    lua_setfield(L, -2, "spawn_bike");
	lua_pushcfunction(L, l_pd_sfx_shuffle);   lua_setfield(L, -2, "sfx_shuffle");
	lua_pushcfunction(L, l_pd_instrument_shuffle); lua_setfield(L, -2, "instrument_shuffle");
	lua_pushcfunction(L, l_pd_pixelate);      lua_setfield(L, -2, "pixelate");
	lua_pushcfunction(L, l_pd_screen_fx);     lua_setfield(L, -2, "screen_fx");
	lua_pushcfunction(L, l_pd_hudvd);         lua_setfield(L, -2, "hudvd");
	lua_pushcfunction(L, l_pd_crt);           lua_setfield(L, -2, "crt");
	lua_pushcfunction(L, l_pd_lens);          lua_setfield(L, -2, "lens");
	lua_pushcfunction(L, l_pd_audio_crush);   lua_setfield(L, -2, "audio_crush");
	lua_pushcfunction(L, l_pd_audio_radio);   lua_setfield(L, -2, "audio_radio");
	lua_pushcfunction(L, l_pd_audio_reverb);  lua_setfield(L, -2, "audio_reverb");
	lua_pushcfunction(L, l_pd_audio_reverse); lua_setfield(L, -2, "audio_reverse");
	lua_pushcfunction(L, l_pd_audio_pitch);   lua_setfield(L, -2, "audio_pitch");

	/* archipelago transport (pd.ap_connect/status/send/poll/disconnect) */
	luaApiRegisterAp(L);
}

/* Clear C-side per-state data. Called from luaaiReset (the Lua registry events
 * table is dropped automatically when the state is closed). */
void luaApiResetFrame(void)
{
	g_LuaOverlayCount = 0;
	g_LuaXrayCount = 0;
	g_LuaLastPlayerRoom = -0x7fffffff; /* re-baseline room tracking on reset */
#ifndef PLATFORM_N64
	/* Free pd.load_image buffers (our RGBA5551 conversions — plain malloc, so
	 * plain free). The Lua-side handles die with the state. */
	{
		s32 i;
		for (i = 0; i < g_LuaImageCount; i++) {
			if (g_LuaImages[i].data) {
				free(g_LuaImages[i].data);
				g_LuaImages[i].data = NULL;
			}
		}
		g_LuaImageCount = 0;
	}
#endif
	/* The Lua state is closing on reset, so the refs go with it; just drop the
	 * count + clear labels (don't luaL_unref against a dead state). */
	{
		s32 i;
		for (i = 0; i < g_LuaMenuCount; i++) {
			g_LuaMenu[i].luaref = LUA_NOREF;
			g_LuaMenu[i].label[0] = '\0';
			g_LuaMenu[i].group[0] = '\0';
		}
		g_LuaMenuCount = 0;
	}
	luaDirectorRebuild(); /* drop stale entries from the menu items array */
}

/* ------------------------------------------------------------------------- *
 * X-ray sampling (called from luaaiExecute once per chr per frame)
 * ------------------------------------------------------------------------- */

void luaApiRecordChr(s32 chrnum, s32 ailistid, s32 aioffset, s32 alertness, s32 islua)
{
	struct luaxray *r;

	if (chrnum < 0 || g_LuaXrayCount >= LUA_MAX_XRAY) {
		return;
	}

	r = &g_LuaXray[g_LuaXrayCount++];
	r->chrnum = chrnum;
	r->ailistid = ailistid;
	r->aioffset = aioffset;
	r->alertness = alertness;
	r->islua = islua;
}

/* ------------------------------------------------------------------------- *
 * Event emitters (called from game code)
 * ------------------------------------------------------------------------- */

void luaEmitWeaponFire(s32 weaponnum, s32 playernum)
{
	lua_Integer a[2];
	a[0] = weaponnum;
	a[1] = playernum;
	luaEventDispatchInts("weaponfire", 2, a);
}

void luaEmitAlert(s32 chrnum, s32 playernum)
{
	lua_Integer a[2];
	a[0] = chrnum;
	a[1] = playernum;
	luaEventDispatchInts("alert", 2, a);
}

void luaEmitKill(s32 chrnum, s32 killerplayernum)
{
	lua_Integer a[2];
	a[0] = chrnum;
	a[1] = killerplayernum;
	luaEventDispatchInts("kill", 2, a);
}

void luaEmitDamage(s32 chrnum, s32 attackerplayernum, s32 amount)
{
	lua_Integer a[3];
	a[0] = chrnum;
	a[1] = attackerplayernum;
	a[2] = amount;
	luaEventDispatchInts("damage", 3, a);
}

void luaEmitSpawn(s32 chrnum)
{
	lua_Integer a[1];
	a[0] = chrnum;
	luaEventDispatchInts("spawn", 1, a);
}

void luaEmitRoomEnter(s32 room, s32 fromroom)
{
	lua_Integer a[2];
	a[0] = room;
	a[1] = fromroom;
	luaEventDispatchInts("roomenter", 2, a);
}

void luaEmitMissionComplete(s32 stageindex, s32 difficulty, s32 secs, s32 cheated)
{
	lua_Integer a[4];
	a[0] = stageindex;
	a[1] = difficulty;
	a[2] = secs;
	a[3] = cheated;
	luaEventDispatchInts("missioncomplete", 4, a);
}

void luaEmitFiringRange(s32 weaponindex, s32 medal)
{
	lua_Integer a[2];
	a[0] = weaponindex;
	a[1] = medal;
	luaEventDispatchInts("firingrange", 2, a);
}

void luaEmitWeaponFound(s32 weaponnum)
{
	lua_Integer a[1];
	a[0] = weaponnum;
	luaEventDispatchInts("weaponfound", 1, a);
}

void luaEmitObjective(s32 stageindex, s32 difficulty, s32 objindex, s32 status)
{
	lua_Integer a[4];
	a[0] = stageindex;
	a[1] = difficulty;
	a[2] = objindex;
	a[3] = status;
	luaEventDispatchInts("objective", 4, a);
}

void luaEmitCheatUnlock(s32 cheatid)
{
	lua_Integer a[1];
	a[0] = cheatid;
	luaEventDispatchInts("cheatunlock", 1, a);
}

void luaEmitChallengeComplete(s32 challengeindex, s32 numplayers)
{
	lua_Integer a[2];
	a[0] = challengeindex;
	a[1] = numplayers;
	luaEventDispatchInts("challengecomplete", 2, a);
}

/* ------------------------------------------------------------------------- *
 * Per-frame tick + render (called from the port frame loop)
 * ------------------------------------------------------------------------- */

void luaTick(void)
{
	s32 i, w;

	/* Make sure scripts are loaded even when no AI is running (title/CI), so
	 * the console and event handlers work everywhere. */
	luaaiEnsureState();

	/* Service the AP transport socket BEFORE the tick event, so any inbound
	 * messages are queued and a Lua "tick" handler drains them the same frame.
	 * Lives in C statics, so it keeps running across the per-stage state reset. */
	apTransportTick();

	/* Per-frame "tick" event -- fires everywhere (menus/loading too), unlike
	 * "draw" which only fires while the HUD renders. AP polling lives here. */
	luaEventDispatchInts("tick", 0, NULL);

	/* Synthesise the "roomenter" event by watching player 0's room each frame
	 * (there is no single engine call site that means "player changed room").
	 * Only emits on an actual change; the first observed room is recorded
	 * silently so we don't fire a spurious enter at stage start. */
	{
		struct luaaiplayerinfo pi;
		if (chraiLuaGetPlayerInfo(0, &pi) && pi.valid) {
			if (g_LuaLastPlayerRoom == -0x7fffffff) {
				g_LuaLastPlayerRoom = pi.room;
			} else if (pi.room != g_LuaLastPlayerRoom) {
				s32 from = g_LuaLastPlayerRoom;
				g_LuaLastPlayerRoom = pi.room;
				luaEmitRoomEnter(pi.room, from);
			}
		}
	}

	/* Age timed overlays. One-frame overlays are removed by luaHudRender after
	 * they're drawn, so they shouldn't normally be present here. */
	w = 0;
	for (i = 0; i < g_LuaOverlayCount; i++) {
		struct luaoverlay *o = &g_LuaOverlays[i];
		if (o->oneframe) {
			continue; /* drop stragglers */
		}
		if (--o->framesleft > 0) {
			if (w != i) {
				g_LuaOverlays[w] = *o;
			}
			w++;
		}
	}
	g_LuaOverlayCount = w;
}

#ifndef PLATFORM_N64
// Draw a wall-hit texture (blood splat) as a tinted HUD quad. Mirrors the
// menugfx sprite recipe (texSelect + textured tri), with per-vertex colour =
// the requested tint so the splat shape comes from the texture and the RGB
// from `color`. Screen coords are the on-screen pixel rect; texcoords span the
// whole texture.
extern struct textureconfig *g_TexWallhitConfigs;

// Blit a textureconfig as a screen-space texrect (no projection matrix needed
// — same path the font glyphs use inside the text0f153628 bracket). Combine:
// colour = PRIMITIVE (the requested tint), alpha = TEXEL0 * PRIMITIVE.alpha, so
// the shape comes from the texture and the RGB from `color`. Pass a full-white
// opaque colour to draw an image untinted.
static Gfx *luaDrawTexConfig(Gfx *gdl, struct textureconfig *tc, s32 texw, s32 texh,
		s32 x, s32 y, s32 w, s32 h, u32 color)
{
	if (texw < 1) texw = 1;
	if (texh < 1) texh = 1;
	if (w < 1) w = 1;
	if (h < 1) h = 1;

	texSelect(&gdl, tc, 2, 0, 2, 1, NULL);

	gDPSetCombineLERP(gdl++,
			0, 0, 0, PRIMITIVE, TEXEL0, 0, PRIMITIVE, 0,
			0, 0, 0, PRIMITIVE, TEXEL0, 0, PRIMITIVE, 0);
	gDPSetPrimColorViaWord(gdl++, 0, 0, color);

	gSPTextureRectangle(gdl++,
			x * 4, y * 4, (x + w) * 4, (y + h) * 4,
			G_TX_RENDERTILE,
			0, 0,
			(texw << 10) / w, (texh << 10) / h);

	return gdl;
}

static Gfx *luaDrawSprite(Gfx *gdl, s32 texnum, s32 x, s32 y, s32 w, s32 h, u32 color)
{
	if (!g_TexWallhitConfigs) {
		return gdl;
	}
	return luaDrawTexConfig(gdl, &g_TexWallhitConfigs[texnum],
			g_TexWallhitConfigs[texnum].width, g_TexWallhitConfigs[texnum].height,
			x, y, w, h, color);
}

// Draw a loaded image as a w*h quad centred at (cx,cy), rotated by `angle`
// radians. Uses textured triangles (rotation needs real geometry, unlike the
// axis-aligned texrect), a MODULATE combine (colour = TEXEL0 * PRIMITIVE) so
// the image shows its OWN colours — pass white for untinted, a colour to tint.
static Gfx *luaDrawImage(Gfx *gdl, s32 handle, s32 cx, s32 cy, s32 w, s32 h, f32 angle, u32 color)
{
	struct luaimage *im;
	s32 iw, ih;

	if (handle < 0 || handle >= g_LuaImageCount || g_LuaImages[handle].data == NULL) {
		return gdl;
	}
	im = &g_LuaImages[handle];
	iw = (s32)im->w;
	ih = (s32)im->h;
	if (w < 1) w = 1;
	if (h < 1) h = 1;

	// Load the RGBA5551 buffer with the standard gDPLoadTextureBlock macro
	// (the same path menugfx uses for the raw RGBA16 blur buffer — it sets up
	// the load/render tiles + sizes correctly, unlike a hand-built texSelect
	// config which mis-sized the tile). Combine: colour = TEXEL0 * PRIMITIVE,
	// alpha = TEXEL0 * PRIMITIVE, so white PRIM = the image untouched and a
	// coloured PRIM tints it.
	gDPPipeSync(gdl++);
	gSPTexture(gdl++, 0xffff, 0xffff, 0, G_TX_RENDERTILE, G_ON);
	gDPLoadTextureBlock(gdl++, im->data, G_IM_FMT_RGBA, G_IM_SIZ_16b, iw, ih, 0,
			G_TX_NOMIRROR | G_TX_CLAMP, G_TX_NOMIRROR | G_TX_CLAMP,
			texGetMask(iw), texGetMask(ih), G_TX_NOLOD, G_TX_NOLOD);
	gDPSetCycleType(gdl++, G_CYC_1CYCLE);
	gDPSetAlphaCompare(gdl++, G_AC_NONE);
	gDPSetCombineLERP(gdl++,
			TEXEL0, 0, PRIMITIVE, 0, TEXEL0, 0, PRIMITIVE, 0,
			TEXEL0, 0, PRIMITIVE, 0, TEXEL0, 0, PRIMITIVE, 0);
	gDPSetPrimColorViaWord(gdl++, 0, 0, color);
	gSPClearGeometryMode(gdl++, G_CULL_BOTH);
	gDPSetTextureFilter(gdl++, G_TF_BILERP);
	gDPSetRenderMode(gdl++, G_RM_XLU_SURF, G_RM_XLU_SURF2);

	if (angle == 0.0f) {
		// Axis-aligned: a screen-space texrect (fast, no vertices).
		gSPTextureRectangle(gdl++,
				(cx - w / 2) * 4, (cy - h / 2) * 4, (cx + w / 2) * 4, (cy + h / 2) * 4,
				G_TX_RENDERTILE, 0, 0, (iw << 10) / w, (ih << 10) / h);
	} else {
		// Rotated: textured quad (texrects can't rotate). Two traps the
		// axis-aligned texrect path doesn't hit:
		// 1. The overlay bracket loads NO matrices, so tris go through
		//    whatever lvRender left behind — load an explicit pixel-space
		//    ortho projection + identity modelview (x10 for subpixel).
		// 2. text0f153628 sets G_TP_NONE, and triangle texcoords are HALVED
		//    when texture persp is off (texrects are exempt) — bake a 2x
		//    into the S10.5 coords (<<6 instead of <<5).
		Vtx *vertices = gfxAllocateVertices(4);
		Mtx *ortho = gfxAllocateMatrix();
		Mtx *ident = gfxAllocateMatrix();
		f32 co = cosf(angle), si = sinf(angle), hw = w * 0.5f, hh = h * 0.5f;
		s16 smax = (s16)(iw << 6), tmax = (s16)(ih << 6);
		const f32 dx[4] = { -1.f, 1.f, 1.f, -1.f };
		const f32 dy[4] = { -1.f, -1.f, 1.f, 1.f };
		s32 i;

		guOrtho(ortho, 0, viGetWidth() * 10.0f, viGetHeight() * 10.0f, 0, -10, 10, 1);
		guMtxIdent(ident);
		gSPMatrix(gdl++, osVirtualToPhysical(ortho), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_PROJECTION);
		gSPMatrix(gdl++, osVirtualToPhysical(ident), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);

		for (i = 0; i < 4; i++) {
			f32 lx = dx[i] * hw, ly = dy[i] * hh;
			vertices[i].x = (s16)((cx + (lx * co - ly * si)) * 10);
			vertices[i].y = (s16)((cy + (lx * si + ly * co)) * 10);
			vertices[i].z = 0;
			vertices[i].s = (i == 1 || i == 2) ? smax : 0;
			vertices[i].t = (i >= 2) ? tmax : 0;
			vertices[i].colour = 0;
		}
		gSPVertex(gdl++, osVirtualToPhysical(vertices), 4, 0);
		gSPTri2(gdl++, 0, 1, 2, 2, 3, 0);
	}

	return gdl;
}
#endif

Gfx *luaHudRender(Gfx *gdl)
{
#ifndef PLATFORM_N64
	s32 i, w;

	if (!g_FontHandelGothicXs || !g_CharsHandelGothicXs) {
		g_LuaXrayCount = 0;
		return gdl;
	}

	/* Fire the per-frame draw event so scripts enqueue this frame's overlays
	 * (e.g. the X-ray, which reads the freshly-sampled g_LuaXray rows). */
	luaEventDispatchInts("draw", 0, NULL);

	if (g_LuaOverlayCount > 0) {
		gdl = text0f153628(gdl);

		for (i = 0; i < g_LuaOverlayCount; i++) {
			struct luaoverlay *o = &g_LuaOverlays[i];
			if (o->kind == OVL_BOX) {
				gdl = hudmsgRenderBox(gdl, o->x, o->y, o->x + o->w, o->y + o->h,
						1.f, o->color, 0.85f);
			} else if (o->kind == OVL_SPRITE) {
				gdl = luaDrawSprite(gdl, o->texnum, o->x, o->y, o->w, o->h, o->color);
			} else if (o->kind == OVL_IMAGE) {
				gdl = luaDrawImage(gdl, o->texnum, o->x, o->y, o->w, o->h, o->angle, o->color);
			} else {
				s32 tx = o->x, ty = o->y;
				// langChaosTransform: chaos UwUify covers the Lua overlay
				// text too (no-op when the mode is off)
				gdl = textRenderProjected(gdl, &tx, &ty, langChaosTransform(o->text),
						g_CharsHandelGothicXs, g_FontHandelGothicXs, (s32)o->color,
						viGetWidth(), viGetHeight(), 0, 0);
			}
		}

		gdl = text0f153780(gdl);

		/* Remove one-frame overlays now that they've been drawn; timed ones
		 * persist and are aged in luaTick. */
		w = 0;
		for (i = 0; i < g_LuaOverlayCount; i++) {
			if (!g_LuaOverlays[i].oneframe) {
				if (w != i) {
					g_LuaOverlays[w] = g_LuaOverlays[i];
				}
				w++;
			}
		}
		g_LuaOverlayCount = w;
	}

	/* X-ray rows are consumed each frame; AI re-fills them next tick. */
	g_LuaXrayCount = 0;
#endif
	return gdl;
}

/* ------------------------------------------------------------------------- *
 * Console commands (/lua reload | /lua <expr>)
 * ------------------------------------------------------------------------- */

void luaaiReload(void)
{
	luaaiReset();
	luaaiEnsureState();
	luaApiLog("reloaded scripts/init.lua");
}

void luaaiDoString(const char *expr)
{
	lua_State *L;

	if (!expr || !*expr) {
		luaApiLog("usage: /lua reload  |  /lua <expr>");
		return;
	}

	if (!luaaiEnsureState()) {
		luaApiLog("no lua state");
		return;
	}

	L = luaaiGetState();
	if (!L) {
		luaApiLog("no lua state");
		return;
	}

	if (luaL_dostring(L, expr) != LUA_OK) {
		luaApiLog2("error: ", lua_tostring(L, -1));
		lua_pop(L, 1);
	} else {
		luaApiLog("ok");
	}
}

void luaaiConsoleCommand(const char *args)
{
	if (args && strncmp(args, "reload", 6) == 0) {
		luaaiReload();
	} else {
		luaaiDoString(args);
	}
}
