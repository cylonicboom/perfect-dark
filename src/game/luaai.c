/**
 * Lua scripting layer for the action block (ailist) system.
 *
 * See game/luaai.h for the high level description. In short: each ailist is
 * transpiled (luaai_transpile.c) into a Lua chunk that drives execution by
 * calling back into the original C command handlers via the ctx:exec bridge.
 * This routes all action block execution through Lua, enabling external Lua
 * scripts to override lists (pd.register_ailist) and to invoke engine commands
 * directly (ctx:run), which is the foundation for scripted missions, custom
 * multiplayer maps and networking helpers.
 *
 * Safety: if Lua fails to initialise, or a chunk raises an error, execution
 * transparently falls back to the original bytecode interpreter so the game
 * keeps running.
 */

#include <ultra64.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "constants.h"
#include "game/luaai.h"
#include "types.h"

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#ifndef PLATFORM_N64
#include "console.h" /* conPrintf — route pd.log to the in-game console */
#endif

/* On by default, per design. Can be toggled at runtime. */
s32 g_LuaAiEnabled = 1;

/* Status codes returned by luaai_run_list(). */
#define LUAAI_TERMINAL 0
#define LUAAI_YIELD    1
#define LUAAI_SWITCH   2
#define LUAAI_ERR      (-1)

/* The opcode that ends an ailist (see commands.h: endlist). */
#ifndef CMD_END
#define CMD_END 0x0004
#endif

static lua_State *g_LuaState = NULL;
static s32 g_LuaCurStage = -0x7fffffff;
static s32 g_LuaInitFailed = 0;
/* Number of registered ailist overrides. When zero, the per-list override
 * lookup (and its ailist id scan) is skipped entirely on the hot path. */
static s32 g_LuaOverrideCount = 0;

/* Registry keys for our internal tables. */
static const char *const KEY_CHUNKS = "luaai.chunks";       /* lightuserdata(list) -> function */
static const char *const KEY_OVERRIDES = "luaai.overrides"; /* id (int) -> function */
static const char *const KEY_CTX = "luaai.ctx";             /* the shared ctx table */

extern u32 chraiGetCommandLength(u8 *ailist, u32 aioffset);
extern u32 chraiGetAilistLength(u8 *list);

/* ------------------------------------------------------------------------- *
 * ctx bridge (the C side of the Lua "ctx" object)
 * ------------------------------------------------------------------------- */

/* ctx:cur() -> current program counter */
static int l_ctx_cur(lua_State *L)
{
	lua_pushinteger(L, (lua_Integer)chraiLuaGetOffset());
	return 1;
}

/* ctx:exec(off) -> 0 continue, 1 yield, 2 list changed/terminated */
static int l_ctx_exec(lua_State *L)
{
	u32 off = (u32)luaL_checkinteger(L, 2);
	void *before = chraiLuaGetList();
	s32 brk;
	void *after;

	brk = chraiLuaStep(off);
	after = chraiLuaGetList();

	if (after != before || after == NULL) {
		lua_pushinteger(L, LUAAI_SWITCH);
		return 1;
	}

	lua_pushinteger(L, brk ? LUAAI_YIELD : LUAAI_TERMINAL);
	return 1;
}

/* ctx:self() -> table describing the chr currently running this ailist, or nil
 * if there is no current chr (e.g. an object-driven list). The table is a
 * read-only snapshot for this call; re-call each frame for fresh values. */
static int l_ctx_self(lua_State *L)
{
	struct luaaiselfinfo info;

	if (!chraiLuaGetSelf(&info) || !info.valid) {
		lua_pushnil(L);
		return 1;
	}

	lua_newtable(L);
	lua_pushinteger(L, info.chrnum);        lua_setfield(L, -2, "chrnum");
	lua_pushnumber(L, info.x);              lua_setfield(L, -2, "x");
	lua_pushnumber(L, info.y);              lua_setfield(L, -2, "y");
	lua_pushnumber(L, info.z);              lua_setfield(L, -2, "z");
	lua_pushinteger(L, info.room);          lua_setfield(L, -2, "room");
	lua_pushnumber(L, info.health);         lua_setfield(L, -2, "health");
	lua_pushnumber(L, info.maxhealth);      lua_setfield(L, -2, "maxhealth");
	lua_pushnumber(L, info.shield);         lua_setfield(L, -2, "shield");
	lua_pushinteger(L, info.alertness);     lua_setfield(L, -2, "alertness");
	if (info.targetchrnum >= 0) {
		lua_pushinteger(L, info.targetchrnum);
		lua_setfield(L, -2, "target_chrnum");
	}
	if (info.targetplayernum >= 0) {
		lua_pushinteger(L, info.targetplayernum);
		lua_setfield(L, -2, "target_playernum");
	}
	return 1;
}

/* ctx:run(opcode, b0, b1, ...) -> break flag
 * Invoke an arbitrary engine command from Lua with explicit operand bytes. */
static int l_ctx_run(lua_State *L)
{
	u32 opcode = (u32)luaL_checkinteger(L, 2);
	u8 operands[60];
	int top = lua_gettop(L);
	int i;
	u32 n = 0;

	for (i = 3; i <= top && n < (u32)sizeof(operands); i++) {
		operands[n++] = (u8)(luaL_checkinteger(L, i) & 0xff);
	}

	lua_pushinteger(L, chraiLuaRunSynthetic(opcode, operands, n));
	return 1;
}

/* pd.register_ailist(id, fn): register a Lua override for an ailist id. */
static int l_pd_register_ailist(lua_State *L)
{
	lua_Integer id = luaL_checkinteger(L, 1);
	luaL_checktype(L, 2, LUA_TFUNCTION);

	lua_getfield(L, LUA_REGISTRYINDEX, KEY_OVERRIDES);
	lua_pushinteger(L, id);
	lua_pushvalue(L, 2);
	lua_settable(L, -3);
	lua_pop(L, 1);

	g_LuaOverrideCount++;
	return 0;
}

/* pd.log(msg) */
static int l_pd_log(lua_State *L)
{
	const char *s = luaL_optstring(L, 1, "");
	fprintf(stderr, "[luaai] %s\n", s);
#ifndef PLATFORM_N64
	conPrintf(1, "[lua] %s", s);
#endif
	return 0;
}

/* ------------------------------------------------------------------------- *
 * State setup
 * ------------------------------------------------------------------------- */

static unsigned int luaai_cmdlen(const unsigned char *list, unsigned int off)
{
	return (unsigned int)chraiGetCommandLength((u8 *)list, off);
}

static void luaai_load_external_scripts(lua_State *L)
{
	/* Best-effort: if a scripts/init.lua exists in the working dir, run it. It
	 * may require/dofile additional files and call pd.register_ailist. Errors
	 * are logged and ignored so a broken mod cannot crash the game. */
	FILE *f = fopen("scripts/init.lua", "rb");
	if (!f) {
		return;
	}
	fclose(f);

	if (luaL_dofile(L, "scripts/init.lua") != LUA_OK) {
		fprintf(stderr, "[luaai] error loading scripts/init.lua: %s\n",
				lua_tostring(L, -1));
		lua_pop(L, 1);
	} else {
		fprintf(stderr, "[luaai] loaded scripts/init.lua\n");
	}
}

static void luaai_build_ctx(lua_State *L)
{
	/* ctx = { cur=..., exec=..., run=... } stored in registry. */
	lua_newtable(L);

	lua_pushcfunction(L, l_ctx_cur);
	lua_setfield(L, -2, "cur");
	lua_pushcfunction(L, l_ctx_exec);
	lua_setfield(L, -2, "exec");
	lua_pushcfunction(L, l_ctx_run);
	lua_setfield(L, -2, "run");
	lua_pushcfunction(L, l_ctx_self);
	lua_setfield(L, -2, "self");

	lua_setfield(L, LUA_REGISTRYINDEX, KEY_CTX);
}

static void luaai_build_pd(lua_State *L)
{
	lua_newtable(L); /* pd */

	lua_pushcfunction(L, l_pd_register_ailist);
	lua_setfield(L, -2, "register_ailist");
	lua_pushcfunction(L, l_pd_log);
	lua_setfield(L, -2, "log");

	luaApiRegister(L); /* adds pd.on / draw_box / draw_text / each_chr */

	lua_setglobal(L, "pd");
}

static int luaai_ensure_state(void)
{
	lua_State *L;

	if (g_LuaState) {
		return 1;
	}
	if (g_LuaInitFailed) {
		return 0;
	}

	L = luaL_newstate();
	if (!L) {
		g_LuaInitFailed = 1;
		fprintf(stderr, "[luaai] failed to create Lua state; using bytecode\n");
		return 0;
	}

	luaL_openlibs(L);

	/* registry tables */
	lua_newtable(L);
	lua_setfield(L, LUA_REGISTRYINDEX, KEY_CHUNKS);
	lua_newtable(L);
	lua_setfield(L, LUA_REGISTRYINDEX, KEY_OVERRIDES);

	luaai_build_ctx(L);
	luaai_build_pd(L);

	g_LuaState = L;

	luaai_load_external_scripts(L);
	return 1;
}

void luaaiReset(void)
{
	if (g_LuaState) {
		lua_close(g_LuaState);
		g_LuaState = NULL;
	}
	g_LuaInitFailed = 0;
	g_LuaOverrideCount = 0;
	luaApiResetFrame();
}

struct lua_State *luaaiGetState(void)
{
	return g_LuaState;
}

s32 luaaiEnsureState(void)
{
	return luaai_ensure_state();
}

/* ------------------------------------------------------------------------- *
 * Chunk cache / lookup
 *
 * On return, the chunk function for `list` is left on top of the Lua stack.
 * Returns 1 on success, 0 on failure (nothing pushed).
 * ------------------------------------------------------------------------- */

static int luaai_get_chunk(lua_State *L, void *list)
{
	char *src;
	u32 listlen;

	/* 1) Lua override by ailist id. Consulted only when overrides are actually
	 * registered (g_LuaOverrideCount), and never on a net client: AI is
	 * server-authoritative, so clients always run the deterministic transpiled
	 * chunk regardless of any locally-registered overrides. Skipping this when
	 * there are no overrides also avoids a per-entity, per-frame id scan. */
	if (g_LuaOverrideCount > 0 && chraiLuaOverridesAllowed()) {
		s32 id = chraiLuaGetListId(list);
		if (id >= 0) {
			lua_getfield(L, LUA_REGISTRYINDEX, KEY_OVERRIDES);
			lua_pushinteger(L, id);
			lua_gettable(L, -2);
			if (lua_isfunction(L, -1)) {
				lua_remove(L, -2); /* remove overrides table, keep function */
				return 1;
			}
			lua_pop(L, 2); /* nil + overrides table */
		}
	}

	/* 2) cached transpiled chunk */
	lua_getfield(L, LUA_REGISTRYINDEX, KEY_CHUNKS);
	lua_pushlightuserdata(L, list);
	lua_gettable(L, -2);
	if (lua_isfunction(L, -1)) {
		lua_remove(L, -2); /* remove chunks table */
		return 1;
	}
	lua_pop(L, 1); /* nil */
	/* chunks table still on stack at -1 */

	/* 3) transpile now. Bound the walk to the real list length so a missing end
	 * marker cannot read past the buffer; the 0xffff cap is only a fallback if
	 * the length is somehow unknown. */
	listlen = chraiGetAilistLength((u8 *)list);
	src = luaaiTranspile((const unsigned char *)list, listlen ? listlen : 0xffffu, luaai_cmdlen, CMD_END);
	if (!src) {
		lua_pop(L, 1); /* chunks table */
		return 0;
	}

	if (luaL_loadstring(L, src) != LUA_OK) {
		fprintf(stderr, "[luaai] transpile load error (id %d): %s\n",
				chraiLuaGetListId(list), lua_tostring(L, -1));
		free(src);
		lua_pop(L, 2); /* error + chunks table */
		return 0;
	}
	free(src);

	/* run the chunk to obtain the function it returns */
	if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
		fprintf(stderr, "[luaai] transpile run error (id %d): %s\n",
				chraiLuaGetListId(list), lua_tostring(L, -1));
		lua_pop(L, 2); /* error + chunks table */
		return 0;
	}

	if (!lua_isfunction(L, -1)) {
		lua_pop(L, 2); /* result + chunks table */
		return 0;
	}

	/* cache: chunks[lightuserdata(list)] = function */
	lua_pushlightuserdata(L, list);
	lua_pushvalue(L, -2); /* the function */
	lua_settable(L, -4);  /* chunks table */

	lua_remove(L, -2); /* remove chunks table, keep function */
	return 1;
}

/* Run the chunk for `list`. Returns LUAAI_* status. */
static int luaai_run_list(lua_State *L, void *list)
{
	int status;

	if (!luaai_get_chunk(L, list)) {
		return LUAAI_ERR;
	}

	/* push ctx argument */
	lua_getfield(L, LUA_REGISTRYINDEX, KEY_CTX);

	if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
		fprintf(stderr, "[luaai] run error (id %d): %s\n",
				chraiLuaGetListId(list), lua_tostring(L, -1));
		lua_pop(L, 1);
		return LUAAI_ERR;
	}

	status = (int)lua_tointeger(L, -1);
	lua_pop(L, 1);
	return status;
}

/* ------------------------------------------------------------------------- *
 * Public entry point
 * ------------------------------------------------------------------------- */

void luaaiExecute(void *entity, s32 proptype)
{
	lua_State *L;
	int guard = 0;
	s32 stage;

	/* Reset cached state when the stage changes (ailist pointers are reused
	 * across stages, so stale transpiled chunks must be discarded). */
	stage = chraiLuaGetStageNum();
	if (stage != g_LuaCurStage) {
		luaaiReset();
		g_LuaCurStage = stage;
	}

	/* Resolve entity + apply list-switch logic. */
	chraiPrepare(entity, proptype);
	if (chraiLuaGetList() == NULL) {
		return;
	}

	/* Sample this chr's live AI state for the pd.each_chr X-ray overlay. */
	luaApiRecordChr(chraiLuaGetChrNum(),
			chraiLuaGetListId(chraiLuaGetList()),
			chraiLuaGetOffset(),
			chraiLuaGetAlertness(), 1);

	if (!luaai_ensure_state()) {
		/* No Lua available: run the bytecode loop from the prepared state. */
		chraiRunLoop();
		return;
	}

	L = g_LuaState;

	while (chraiLuaGetList() != NULL) {
		int r = luaai_run_list(L, chraiLuaGetList());

		if (r == LUAAI_ERR) {
			/* Fall back to the bytecode interpreter for the remainder and
			 * for all future frames. */
			fprintf(stderr, "[luaai] disabling Lua AI after error\n");
			g_LuaAiEnabled = 0;
			chraiRunLoop();
			return;
		}

		if (r == LUAAI_SWITCH) {
			if (++guard > 1024) {
				/* Runaway list switching; bail to avoid hanging. */
				break;
			}
			continue;
		}

		/* LUAAI_YIELD or LUAAI_TERMINAL: done for this frame. */
		break;
	}
}
