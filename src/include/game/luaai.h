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
void luaEmitDamage(s32 chrnum, s32 attackerplayernum, s32 amount);
void luaEmitSpawn(s32 chrnum);
void luaEmitRoomEnter(s32 room, s32 fromroom);
/* Archipelago check-detection emitters (port-only; no-op if nothing listens). */
void luaEmitMissionComplete(s32 stageindex, s32 difficulty, s32 secs, s32 cheated);
void luaEmitFiringRange(s32 weaponindex, s32 medal);
void luaEmitWeaponFound(s32 weaponnum);
void luaEmitObjective(s32 stageindex, s32 difficulty, s32 objindex, s32 status);
void luaEmitCheatUnlock(s32 cheatid);
void luaEmitChallengeComplete(s32 challengeindex, s32 numplayers);

/* Archipelago gating (port-only). An AP run locks content until the matching
 * item arrives; the engine gate points consult these. Categories index the
 * unlock set written by pd.unlock / pd.lock. All inert unless pd.ap_mode(true).
 * weapon_pri/sec are keyed by weaponnum, device by the device weaponnum. */
#define AP_CAT_STAGE       0
#define AP_CAT_DIFFICULTY  1
#define AP_CAT_WEAPON_PRI  2
#define AP_CAT_WEAPON_SEC  3
#define AP_CAT_DEVICE      4
#define AP_CAT_FEATURE     5
#define AP_NUM_CATEGORIES  6
bool apGateActive(void);
bool apGateIsUnlocked(s32 cat, s32 id);
const char *apGetListHeader(void);

/* Archipelago transport bridge (luaai_ap.c). The socket lives in C statics so
 * it survives the per-stage lua_State teardown. apTransportTick() is the
 * per-frame pump (call from luaTick); luaApiRegisterAp registers the pd.ap_*
 * transport functions (pd table on stack top). */
void apTransportTick(void);
void luaApiRegisterAp(struct lua_State *L);

/** chr-state bridges for the X-ray (defined in chrai.c). */
s32 chraiLuaGetChrNum(void);
s32 chraiLuaGetAlertness(void);

/**
 * Read-only snapshot of the chr currently running its ailist, for ctx:self().
 * Populated by chraiLuaGetSelf() from g_Vars.chrdata. All fields are 0 / -1 when
 * there is no current chr (e.g. an object-driven list).
 */
struct luaaiselfinfo {
	s32 chrnum;      // -1 if none
	s32 valid;       // 1 if a chr is currently executing, else 0
	f32 x, y, z;     // world position
	s32 room;        // first room number, -1 if unknown
	f32 health;      // maxdamage - damage (clamped >= 0)
	f32 maxhealth;   // maxdamage
	f32 shield;
	s32 alertness;   // 0..255
	s32 targetchrnum;   // chrnum of the chr's current target, or -1
	s32 targetplayernum; // playernum if the target is a player, else -1
};

/** Read-only world position of a player, for pd.player_pos(). */
struct luaaiplayerinfo {
	s32 valid;
	f32 x, y, z;
	s32 room;
};

/** Fill `out` from the currently-executing chr. Returns out->valid. Defined in chrai.c. */
s32 chraiLuaGetSelf(struct luaaiselfinfo *out);

/** Fill `out` from any chr by literal chrnum (<0/unknown -> valid=0). Defined in chrai.c. */
s32 chraiLuaGetChrInfo(s32 chrnum, struct luaaiselfinfo *out);

/** Fill `out` from a player by index (0..MAX_PLAYERS-1). Returns out->valid. */
s32 chraiLuaGetPlayerInfo(s32 playernum, struct luaaiplayerinfo *out);

/** Number of active local players. */
s32 chraiLuaGetPlayerCount(void);

/**
 * Spawn a weapon/item world object at chr `chrnum`'s location (model derived
 * from `weaponnum`). Server-side only; no-op if chrnum unknown or weapon has no
 * world model. Returns 1 on success. Backs pd.spawn_at_chr(). Defined in
 * chraction.c (next to chrDropItem, which it reuses).
 */
s32 chraiLuaSpawnAtChr(s32 chrnum, s32 weaponnum);

/**
 * Spawn a weapon/item object at an arbitrary world position (x,y,z). Rooms are
 * seeded from `refchrnum` (or the local player's chr if refchrnum < 0) and the
 * object is floor-snapped at the target. Server-side only; returns 1 on success.
 * Backs pd.spawn(). Defined in chraction.c.
 */
s32 chraiLuaSpawnAtPos(s32 refchrnum, s32 weaponnum, f32 x, f32 y, f32 z);

/** Push a Lua table describing a chr snapshot (shared by pd.chr_info + ctx:self). */
void luaApiPushChrInfo(struct lua_State *L, const struct luaaiselfinfo *info);

/* ------------------------------------------------------------------------- *
 * Toolkit framework bridges (defined in chraction.c). All-actor iteration +
 * per-chr mutators backing pd.all_chrs / pd.chr_anim / chr_set_shield / chr_alert.
 * Mutators are server-side (no-op on a net client) and return 1 on success.
 * ------------------------------------------------------------------------- */
s32 chraiLuaGetChrSlotCount(void);            /* total chr slots to iterate */
s32 chraiLuaGetChrNumBySlot(s32 slot);        /* chrnum at slot, or -1 if empty */
s32 chraiLuaChrAnim(s32 chrnum, s32 animnum, f32 speed);
s32 chraiLuaChrSetShield(s32 chrnum, f32 value);
s32 chraiLuaChrAlert(s32 chrnum);
s32 chraiLuaChrSetBody(s32 chrnum, s32 bodynum, s32 headnum); /* runtime model swap, solo only */
s32 chraiLuaSetChrPos(s32 chrnum, f32 x, f32 y, f32 z); /* move a chr prop (no physics) */

/* Archipelago bonus/buff bridges (chraction.c). Apply to the local player on
 * receipt of an AP "bonus" item; server/solo only, no-op without a live player. */
s32 chraiLuaPlayerHeal(void);                 /* full HP */
s32 chraiLuaPlayerSetShield(f32 frac);        /* shield 0..1 (>=1 = full) */
s32 chraiLuaRefillAmmo(void);                 /* top all ammo to capacity */
s32 chraiLuaGiveAmmo(s32 ammotype, s32 qty);  /* grant ammo (+ matching weapon) */
s32 chraiLuaGiveWeaponToPlayer(s32 weaponnum);/* add a weapon to inventory */
s32 chraiLuaDeviceOn(s32 weaponnum);          /* activate a device (e.g. cloak) */
s32 chraiLuaSetInvincible(s32 on);            /* toggle invincibility */
s32 chraiLuaSpawnAlly(void);                  /* spawn a friendly "Perfect Buddy"; chrnum or -1 */

/* Chaos-mode primitives (docs/PORT_CHAOS.md; backs scripts/chaos.lua). Same
 * apLuaPlayerChr() contract as the AP helpers above. */
s32 chraiLuaTakeWeapon(s32 weaponnum);        /* remove weapon (+ cycle off it) */
s32 chraiLuaWeaponHeld(void);                 /* right-hand weaponnum, -1 = no pawn */
s32 chraiLuaSwitchWeapon(s32 weaponnum);      /* force-equip an owned weapon */
s32 chraiLuaScreenFade(s32 r, s32 g, s32 b, s32 a, f32 time60); /* viewport fade */
s32 chraiLuaYeetChr(s32 chrnum, f32 force);   /* knockback-fling a chr away from the player */
s32 chraiLuaExplodeAtChr(s32 chrnum, s32 type); /* explosion at a chr's feet */
s32 chraiLuaPlaySound(s32 sfxnum);            /* one-shot local sound */
s32 chraiLuaSetAlarm(s32 on);                 /* stage alarm on/off (server-side) */
s32 chraiLuaBoost(f32 secs);                  /* Speed Pill boost for N secs (<=0 cancels) */
s32 chraiLuaPlayerSetHealth(f32 frac);        /* health 0.01..1 (never kills) */
s32 chraiLuaDizzy(s32 amount);                /* tranq screen-sway, 0..4000 blur units */
s32 chraiLuaChrCloak(s32 chrnum, s32 on);     /* toggle CHRHFLAG_CLOAKED on a chr */
s32 chraiLuaStripAmmo(void);                  /* zero every ammo pool */
s32 chraiLuaTeleportToChr(s32 chrnum);        /* snap player to a chr (server-side) */
s32 chraiLuaFlatTex(s32 mode);                /* 0 normal, 1 white/vertex-only, 2 avg-colour textures */
s32 chraiLuaGrayscale(s32 on);                /* force the renderer grayscale path */
s32 chraiLuaRoomTint(s32 r, s32 g, s32 b, s32 on); /* stage-wide room lighting tint (KotH hill math) */
s32 chraiLuaPlayerExplosions(s32 on);         /* AFO crash explosions around the player */
s32 chraiLuaAmmoSwap(s32 weaponnum);          /* held guns fire this weapon's primary; -1 off */

/* External event ingress (luaai_api.c): generic {source, text} queue backing
 * pd.ext_poll(). Fed by the /chaos console command, the optional localhost
 * UDP listener (Chaos.EventPort, net.c), and any future chat bridge
 * (Twitch/YouTube) — see docs/PORT_CHAOS.md. Safe to call from game code. */
void luaExtEventPush(const char *source, const char *text);

/* ------------------------------------------------------------------------- *
 * Controllable entity / possession (port/src/possess.c + chraction.c bridges).
 * Solo/missions only; lets the player fly a spawned "cube" (free-fly) and return
 * to their body. Backs pd.possess_spawn / pd.unpossess.
 * ------------------------------------------------------------------------- */
s32 chraiLuaPossessSpawn(s32 bodynum); /* spawn + possess a cube; returns chrnum or -1 */
void chraiLuaUnpossess(void);          /* stop + free the cube */
/* possess.c internals (called by the bridges + the pdmain frame loop): */
s32 luaPossessBegin(s32 cube_chrnum);
void luaPossessEnd(void);
s32 luaPossessIsActive(void);
s32 luaPossessGetChrNum(void);
void luaPossessReadInput(void);   /* per-frame input + pose integration */
void luaPossessApplyCamera(void); /* point the render camera at the fly pose */

/* ------------------------------------------------------------------------- *
 * Director pause-menu registry. Scripts register entries via pd.menu_add; the
 * Lua Director dialog (mainmenu.c) reads these accessors to render + dispatch.
 * Defined in luaai_api.c.
 * ------------------------------------------------------------------------- */
#define LUA_MENU_MAX 24            /* max Director entries (shared with mainmenu.c) */
s32 luaMenuCount(void);            /* number of registered Director entries */
const char *luaMenuLabel(s32 i);   /* label of entry i ("" if out of range) */
void luaMenuInvoke(s32 i);         /* call entry i's Lua fn (guarded, logged) */

/* Rebuild the Director menu items array from the registry (defined in
 * mainmenu.c). Called by pd.menu_add/menu_clear so the array is always valid +
 * current before any dialog open. No-op stub when the menu isn't compiled. */
void luaDirectorRebuild(void);

#endif
