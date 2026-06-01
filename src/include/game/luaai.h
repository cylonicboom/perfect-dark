#ifndef _IN_GAME_LUAAI_H
#define _IN_GAME_LUAAI_H

#include <ultra64.h>
#include "types.h"

/**
 * Lua scripting layer for the action block (ailist) system.
 *
 * At runtime, each action block (ailist) is transpiled from its original
 * bytecode form into a self contained Lua chunk. The Lua chunk drives
 * execution by dispatching on the current command offset and invoking the
 * original C command handlers through a small bridge (ctx:exec). This keeps
 * 100% fidelity with the original behaviour while routing all action block
 * execution through Lua, which in turn enables external Lua scripting and
 * modding (custom missions, multiplayer maps, networking helpers, etc).
 *
 * The original bytecode interpreter (chraiExecuteBytecode) is still present
 * and is used as a fallback if the Lua layer is disabled or fails to build a
 * chunk for a given list.
 */

/* Non-zero when action blocks should be executed via Lua. Defaults to 1. */
extern s32 g_LuaAiEnabled;

/**
 * Execute the current action block for the given entity via the Lua layer.
 * Mirrors chraiExecute(). Falls back to the bytecode interpreter on error.
 */
void luaaiExecute(void *entity, s32 proptype);

/**
 * Reset all cached Lua state. Should be called when a new stage is loaded so
 * that transpiled chunks and registered overrides from the previous stage do
 * not leak into the next one.
 */
void luaaiReset(void);

/**
 * Transpile a single action block (ailist) bytecode buffer into Lua source.
 *
 * The returned string is heap allocated and must be freed by the caller.
 * Returns NULL on allocation failure.
 *
 * This function is intentionally free of any game engine dependencies so that
 * it can be reused by offline tooling and unit tests. The caller supplies a
 * callback that returns the byte length of the command at a given offset and
 * the opcode value that marks the end of a list.
 *
 * @param list       pointer to the bytecode buffer
 * @param maxlen     maximum number of bytes that may be read from list (safety)
 * @param cmdlen     callback returning the length in bytes of the command at off
 * @param endopcode  opcode value (e.g. 0x0004) that terminates the list
 */
char *luaaiTranspile(const unsigned char *list, unsigned int maxlen,
		unsigned int (*cmdlen)(const unsigned char *list, unsigned int off),
		unsigned int endopcode);

/* ------------------------------------------------------------------------- *
 * Bridge functions implemented in chrai.c.
 *
 * These keep all access to g_Vars / engine structs inside chrai.c, so that
 * luaai.c only needs the Lua headers and these small accessors.
 * ------------------------------------------------------------------------- */

/**
 * Resolve the entity, load its current ailist into g_Vars and apply the
 * shot/dodge/darkroom list-switch logic. After this call g_Vars.ailist and
 * g_Vars.aioffset describe the list that should run (g_Vars.ailist may be NULL
 * if there is nothing to execute).
 */
void chraiPrepare(void *entity, s32 proptype);

/**
 * Run the bytecode dispatch loop from the current g_Vars state. Assumes
 * chraiPrepare() has already been called. Used as the Lua fallback path.
 */
void chraiRunLoop(void);

/**
 * Execute a single command at the given offset and update g_Vars.aioffset /
 * g_Vars.ailist exactly like the bytecode interpreter would. Returns 1 if the
 * command handler requested a break (e.g. yield), 0 otherwise.
 */
s32 chraiLuaStep(u32 off);

/** Current program counter (g_Vars.aioffset). */
u32 chraiLuaGetOffset(void);

/** Currently executing ailist pointer (g_Vars.ailist). */
void *chraiLuaGetList(void);

/** Look up the id of an ailist pointer, or -1 if unknown. */
s32 chraiLuaGetListId(void *list);

/** Current stage number (used to detect stage changes and reset Lua state). */
s32 chraiLuaGetStageNum(void);

/**
 * Non-zero when Lua ailist overrides may be applied. Returns false on a net
 * client so that AI stays server-authoritative: clients always run the
 * deterministic transpiled chunk regardless of any locally-registered
 * overrides, which prevents host/client script divergence.
 */
s32 chraiLuaOverridesAllowed(void);

/**
 * Build a synthetic single command (opcode + operand bytes) and run its
 * handler. Intended for hand-written Lua scripts that want to invoke engine
 * commands directly. Returns the handler's break flag (0/1). Note: control
 * flow commands (labels, gotos) are not meaningful in synthetic mode.
 */
s32 chraiLuaRunSynthetic(u32 opcode, const u8 *operands, u32 n);

/** The bytecode interpreter (original behaviour); used when Lua is disabled. */
void chraiExecuteBytecode(void *entity, s32 proptype);

/* ------------------------------------------------------------------------- *
 * Scripting API + dev overlay (luaai_api.c). Port-only in practice.
 * ------------------------------------------------------------------------- */

struct lua_State; /* avoid pulling lua.h into game headers */

/** Live Lua state, or NULL if not yet built. */
struct lua_State *luaaiGetState(void);

/** Ensure the Lua state is built and scripts/init.lua loaded. Returns 1 on success. */
s32 luaaiEnsureState(void);

/** Reload scripts now (reset + rebuild + re-run scripts/init.lua). */
void luaaiReload(void);

/** Run a Lua string now; logs result/error to the console. */
void luaaiDoString(const char *expr);

/** Handle a "/lua ..." console command (args may be NULL/empty). */
void luaaiConsoleCommand(const char *args);

/** Per-frame tick (ensure state, age overlays). Call once per frame. */
void luaTick(void);

/** Render the 2D scripting overlays. Call once per frame in the 2D pass. */
Gfx *luaHudRender(Gfx *gdl);

/** Register the pd.on/draw_box/draw_text/each_chr functions (pd table on stack top). */
void luaApiRegister(struct lua_State *L);

/** Clear C-side overlay + X-ray state (called on luaaiReset). */
void luaApiResetFrame(void);

/** Record one chr's live AI state for the X-ray (called from luaaiExecute). */
void luaApiRecordChr(s32 chrnum, s32 ailistid, s32 aioffset, s32 alertness, s32 islua);

/** Event emitters, called from game code (no-op if no script is listening). */
void luaEmitWeaponFire(s32 weaponnum, s32 playernum);
void luaEmitAlert(s32 chrnum, s32 playernum);
void luaEmitKill(s32 chrnum, s32 killerplayernum);

/** chr-state bridges for the X-ray (defined in chrai.c). */
s32 chraiLuaGetChrNum(void);
s32 chraiLuaGetAlertness(void);

#endif
