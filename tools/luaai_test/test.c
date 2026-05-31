/*
 * Standalone unit test for the action block -> Lua transpiler and dispatch
 * model. This does NOT require the Perfect Dark ROM or the full game build; it
 * validates the core algorithm (luaaiTranspile + the ctx:exec/ctx:cur contract)
 * against the vendored Lua interpreter using a small synthetic action block.
 *
 * Build (see build.sh):
 *   cc -I../../port/lua test.c ../../src/game/luaai_transpile.c \
 *      ../../port/lua/*.c -lm -o luaai_test
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

char *luaaiTranspile(const unsigned char *list, unsigned int maxlen,
		unsigned int (*cmdlen)(const unsigned char *list, unsigned int off),
		unsigned int endopcode);

/* ---- Synthetic opcode set (mirrors the real engine's structure) ---- */
#define OP_GOTO_NEXT 0x0000 /* goto_next(label): 3 bytes */
#define OP_LABEL     0x0002 /* label(id):        3 bytes */
#define OP_YIELD     0x0003 /* yield:            2 bytes */
#define OP_END       0x0004 /* endlist:          2 bytes */
#define OP_ACTION    0x0010 /* action:           2 bytes */
#define OP_ACTION2   0x0011 /* action+operand:   4 bytes */

static unsigned int test_cmdlen(const unsigned char *list, unsigned int off)
{
	unsigned int op = ((unsigned int)list[off] << 8) | (unsigned int)list[off + 1];
	switch (op) {
	case OP_GOTO_NEXT: return 3;
	case OP_LABEL:     return 3;
	case OP_YIELD:     return 2;
	case OP_END:       return 2;
	case OP_ACTION:    return 2;
	case OP_ACTION2:   return 4;
	default:           return 2;
	}
}

/*
 * The test "program":
 *   0:  label(1)            ; loop top
 *   3:  action
 *   5:  action2(0x42)
 *   9:  yield
 *   11: goto_next(1)        ; jump back to label 1
 *   14: endlist
 *
 * Each "frame" should execute the label + the two actions, then yield. The goto
 * after the yield loops back to the label. So across multiple frames we run two
 * actions per frame indefinitely.
 */
static unsigned char g_prog[] = {
	0x00, 0x02, 0x01,       /* label 1            @0  */
	0x00, 0x10,             /* action             @3  */
	0x00, 0x11, 0x00, 0x42, /* action2 0x42       @5  */
	0x00, 0x03,             /* yield              @9  */
	0x00, 0x00, 0x01,       /* goto_next 1        @11 */
	0x00, 0x04,             /* endlist            @14 */
};

/* ---- Simulated execution context (the C side of ctx) ---- */
struct ctx {
	unsigned int pc;
	int actions_run;
};

static struct ctx g_ctx;

static unsigned int find_label_from(unsigned int from, unsigned char label)
{
	unsigned int off = from;
	while (off + 1 < sizeof(g_prog)) {
		unsigned int op = ((unsigned int)g_prog[off] << 8) | g_prog[off + 1];
		if (op == OP_LABEL && g_prog[off + 2] == label) {
			return off;
		}
		if (op == OP_END) {
			return 0;
		}
		off += test_cmdlen(g_prog, off);
	}
	return 0;
}

/* ctx:cur() -> current pc */
static int l_cur(lua_State *L)
{
	lua_pushinteger(L, (lua_Integer)g_ctx.pc);
	return 1;
}

/* ctx:exec(off) -> 0 continue, 1 yield, 2 switch/terminal */
static int l_exec(lua_State *L)
{
	unsigned int off = (unsigned int)luaL_checkinteger(L, 2);
	unsigned int op = ((unsigned int)g_prog[off] << 8) | g_prog[off + 1];

	switch (op) {
	case OP_LABEL:
		g_ctx.pc = off + 3;
		lua_pushinteger(L, 0);
		return 1;
	case OP_ACTION:
		g_ctx.actions_run++;
		g_ctx.pc = off + 2;
		lua_pushinteger(L, 0);
		return 1;
	case OP_ACTION2:
		g_ctx.actions_run++;
		g_ctx.pc = off + 4;
		lua_pushinteger(L, 0);
		return 1;
	case OP_GOTO_NEXT:
		g_ctx.pc = find_label_from(off, g_prog[off + 2]);
		lua_pushinteger(L, 0);
		return 1;
	case OP_YIELD:
		g_ctx.pc = off + 2;
		lua_pushinteger(L, 1);
		return 1;
	default:
		lua_pushinteger(L, 2);
		return 1;
	}
}

int main(void)
{
	lua_State *L;
	char *src;
	int frame;
	int rc = 0;

	src = luaaiTranspile(g_prog, (unsigned int)sizeof(g_prog), test_cmdlen, OP_END);
	if (!src) {
		fprintf(stderr, "FAIL: luaaiTranspile returned NULL\n");
		return 1;
	}

	printf("=== Generated Lua ===\n%s\n=====================\n", src);

	L = luaL_newstate();
	luaL_openlibs(L);

	lua_newtable(L); /* ctx */
	lua_pushcfunction(L, l_cur);
	lua_setfield(L, -2, "cur");
	lua_pushcfunction(L, l_exec);
	lua_setfield(L, -2, "exec");
	lua_setglobal(L, "CTX");

	if (luaL_loadstring(L, src) != LUA_OK) {
		fprintf(stderr, "FAIL: loadstring: %s\n", lua_tostring(L, -1));
		free(src);
		return 1;
	}
	if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
		fprintf(stderr, "FAIL: chunk: %s\n", lua_tostring(L, -1));
		free(src);
		return 1;
	}
	lua_setglobal(L, "RUN"); /* RUN = function(ctx) ... end */

	g_ctx.pc = 0;

	for (frame = 0; frame < 3; frame++) {
		int status;
		lua_getglobal(L, "RUN");
		lua_getglobal(L, "CTX");
		if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
			fprintf(stderr, "FAIL: RUN frame %d: %s\n", frame, lua_tostring(L, -1));
			free(src);
			return 1;
		}
		status = (int)lua_tointeger(L, -1);
		lua_pop(L, 1);
		printf("frame %d -> status %d (pc now %u)\n", frame, status, g_ctx.pc);
		if (status != 1) {
			fprintf(stderr, "FAIL: expected yield (1) on frame %d, got %d\n", frame, status);
			rc = 1;
		}
	}

	printf("total actions run across 3 frames: %d (expected 6)\n", g_ctx.actions_run);
	if (g_ctx.actions_run != 6) {
		fprintf(stderr, "FAIL: expected 6 actions, got %d\n", g_ctx.actions_run);
		rc = 1;
	}

	lua_close(L);
	free(src);

	if (rc == 0) {
		printf("\nPASS: transpiler + dispatch model behaves correctly.\n");
	}
	return rc;
}
