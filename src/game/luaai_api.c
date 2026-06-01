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
#include "data.h"             /* g_FontHandelGothicXs / g_CharsHandelGothicXs */
#include "lib/vi.h"           /* viGetWidth / viGetHeight */

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

enum { OVL_BOX, OVL_TEXT };

struct luaoverlay {
	s32 kind;
	s32 x, y, w, h;
	u32 color;
	char text[LUA_TEXT_MAX];
	s32 framesleft; /* >0 timed; one-frame entries use 1 + oneframe flag */
	s32 oneframe;
};

static struct luaoverlay g_LuaOverlays[LUA_MAX_OVERLAYS];
static s32 g_LuaOverlayCount = 0;

struct luaxray {
	s32 chrnum, ailistid, aioffset, alertness, islua;
};

static struct luaxray g_LuaXray[LUA_MAX_XRAY];
static s32 g_LuaXrayCount = 0;

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
		const char *text, f32 secs)
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

	luaOverlayAdd(OVL_BOX, x, y, w, h, color, NULL, secs);
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

	luaOverlayAdd(OVL_TEXT, x, y, 0, 0, color, text, secs);
	return 0;
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

/* Called by luaai.c's luaai_build_pd with the pd table on top of the stack. */
void luaApiRegister(lua_State *L)
{
	/* create the events registry table (replaces any previous one) */
	lua_newtable(L);
	lua_setfield(L, LUA_REGISTRYINDEX, KEY_EVENTS);

	/* pd.* functions (pd table is at -1) */
	lua_pushcfunction(L, l_pd_on);          lua_setfield(L, -2, "on");
	lua_pushcfunction(L, l_pd_draw_box);    lua_setfield(L, -2, "draw_box");
	lua_pushcfunction(L, l_pd_draw_text);   lua_setfield(L, -2, "draw_text");
	lua_pushcfunction(L, l_pd_each_chr);    lua_setfield(L, -2, "each_chr");
	/* world / entity queries */
	lua_pushcfunction(L, l_pd_chr_info);    lua_setfield(L, -2, "chr_info");
	lua_pushcfunction(L, l_pd_chr_pos);     lua_setfield(L, -2, "chr_pos");
	lua_pushcfunction(L, l_pd_chr_health);  lua_setfield(L, -2, "chr_health");
	lua_pushcfunction(L, l_pd_player_pos);  lua_setfield(L, -2, "player_pos");
	lua_pushcfunction(L, l_pd_player_count);lua_setfield(L, -2, "player_count");
	lua_pushcfunction(L, l_pd_distance);    lua_setfield(L, -2, "distance");
	/* world mutation (server-side) */
	lua_pushcfunction(L, l_pd_spawn_at_chr);lua_setfield(L, -2, "spawn_at_chr");
}

/* Clear C-side per-state data. Called from luaaiReset (the Lua registry events
 * table is dropped automatically when the state is closed). */
void luaApiResetFrame(void)
{
	g_LuaOverlayCount = 0;
	g_LuaXrayCount = 0;
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

/* ------------------------------------------------------------------------- *
 * Per-frame tick + render (called from the port frame loop)
 * ------------------------------------------------------------------------- */

void luaTick(void)
{
	s32 i, w;

	/* Make sure scripts are loaded even when no AI is running (title/CI), so
	 * the console and event handlers work everywhere. */
	luaaiEnsureState();

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
			} else {
				s32 tx = o->x, ty = o->y;
				gdl = textRenderProjected(gdl, &tx, &ty, o->text,
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
