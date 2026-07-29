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
void luaEmitWeaponPickup(s32 weaponnum);      /* every local pickup, not just first discovery */
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
s32 chraiLuaPlayerHeal(f32 amount);           /* amount<=0 => full HP; else add fraction (capped) */
s32 chraiLuaPlayerSetShield(f32 frac);        /* shield 0..1 (>=1 = full) */
s32 chraiLuaRefillAmmo(void);                 /* top all ammo to capacity */
s32 chraiLuaGiveMags(s32 mags);               /* stock every ammo type with N magazines, not max */
s32 chraiLuaGiveAmmo(s32 ammotype, s32 qty);  /* grant ammo (+ matching weapon) */
s32 chraiLuaGiveWeaponToPlayer(s32 weaponnum);/* add a weapon to inventory */
s32 chraiLuaDeviceOn(s32 weaponnum);          /* activate a device (e.g. cloak) */
s32 chraiLuaSetInvincible(s32 on);            /* toggle invincibility */
s32 chraiLuaSpawnAlly(void);                  /* spawn a friendly "Perfect Buddy"; chrnum or -1 */
s32 chraiLuaSpawnAllyClone(f32 healthfrac, f32 yscale);   /* friendly Jo clone (player body/head), scaled HP + vertical squash; chrnum or -1 */
s32 chraiLuaChrYscale(s32 chrnum, f32 mult);  /* non-uniform vertical squash (width kept) */
s32 chraiLuaStageMusic(s32 on);               /* stop / restart the current stage's music */
s32 chraiLuaPirate(s32 side);                 /* black out one screen half (post-process; 1=L, 2=R, 0=off) */
s32 chraiLuaSpaceProgram(s32 on);             /* player bullets one-hit-kill + massive knockback */
s32 chraiLuaFragOut(s32 on);                  /* enemies throw grenades instead of firing */
s32 chraiLuaSpawnSentry(f32 dx, f32 dz);      /* deploy a hostile laptop sentry at an offset */
void chraiLuaResetSentries(void);             /* drop the sentry pool count (per stage) */
s32 chraiLuaTemuMag(s32 on);                  /* reload pays full cost but only partly refills */
f32 chraiLuaMusicBpm(void);                   /* current sequenced-music tempo (BPM), 0 if none */
f32 chraiLuaMusicBeat(void);                  /* beat phase [0,1), -1 if no sequenced track */
s32 chraiLuaAimChr(void);                     /* chrnum the player is aiming at, or -1 */
s32 chraiLuaVertexWobble(f32 amp, f32 freq, f32 phase, f32 sag, f32 desync, f32 nearfade); /* "Jelly"/"Acid" vertex deformation (+melt sag, +per-vertex rate spread) */
s32 chraiLuaHallOfMirrors(s32 on);            /* skip the frame colour clear (HOM trails) */

/* Chaos-mode primitives (docs/PORT_CHAOS.md; backs scripts/chaos.lua). Same
 * apLuaPlayerChr() contract as the AP helpers above. */
s32 chraiLuaDeviceOff(s32 weaponnum);         /* deactivate a device (device_on inverse) */
s32 chraiLuaDeviceActive(s32 weaponnum);      /* device currently switched on? */
s32 chraiLuaLvUpdate(void);                   /* game ticks this frame (0 while paused) */
s32 chraiLuaTakeWeapon(s32 weaponnum);        /* remove weapon (+ cycle off it) */
s32 chraiLuaWeaponHeld(void);                 /* right-hand weaponnum, -1 = no pawn */
s32 chraiLuaSwitchWeapon(s32 weaponnum);      /* force-equip an owned weapon */
s32 chraiLuaScreenFade(s32 r, s32 g, s32 b, s32 a, f32 time60); /* viewport fade */
s32 chraiLuaYeetChr(s32 chrnum, f32 force);   /* knockback-fling a chr away from the player */
s32 chraiLuaExplodeAtChr(s32 chrnum, s32 type); /* explosion at a chr's feet */
s32 chraiLuaExplodeAtPos(f32 x, f32 y, f32 z, s32 type); /* explosion at a position */
s32 chraiLuaSpawnGrenade(f32 x, f32 y, f32 z, s32 chrnum); /* live armed grenade; chrnum >= 0 seeds the room search */
s32 chraiLuaDoorTraps(s32 on);                /* booby-trapped doors: opening detonates */
u32 chraiLuaDoorOpens(void);                  /* doors-opened counter (task sensor) */
s32 chraiLuaEnvColours(s32 sr, s32 sg, s32 sb, s32 cr, s32 cg, s32 cb); /* sky+cloud override */
f32 chraiLuaPlayerYaw(void);                  /* look yaw degrees (spin task sensor) */
s32 chraiLuaPlayerCrouch(void);               /* 0 stand / 1 duck / 2 squat */
s32 chraiLuaHasWeapon(s32 weaponnum);         /* weapon in player inventory */
s32 chraiLuaPlaySound(s32 sfxnum);            /* one-shot local sound */
s32 chraiLuaMetronomeClick(void);            /* Beat game click at half music volume */
s32 chraiLuaSetAlarm(s32 on);                 /* stage alarm on/off (server-side) */
s32 chraiLuaBoost(f32 secs);                  /* Speed Pill boost for N secs (<=0 cancels) */
s32 chraiLuaPlayerSetHealth(f32 frac);        /* health 0.01..1 (never kills) */
s32 chraiLuaShowHealth(void);                 /* pop the health bar, no change */
s32 chraiLuaDizzy(s32 amount);                /* tranq screen-sway, 0..4000 blur units */
s32 chraiLuaChrCloak(s32 chrnum, s32 on);     /* toggle CHRHFLAG_CLOAKED on a chr */
s32 chraiLuaStripAmmo(void);                  /* zero every ammo pool */
s32 chraiLuaSetAmmo(s32 ammotype, s32 qty);   /* set one ammo pool to an exact qty */
s32 chraiLuaTeleportToChr(s32 chrnum);        /* snap player to a chr (server-side) */
s32 chraiLuaFlatTex(s32 mode);                /* 0 normal, 1 white/vertex-only, 2 avg-colour textures */
s32 chraiLuaGrayscale(s32 on);                /* force the renderer grayscale path */
s32 chraiLuaShiny(s32 mode);                  /* 0 off, 1 fake-chrome UVs everywhere, 2 + gold tint */
s32 chraiLuaChrGiveWeapon(s32 chrnum, s32 weaponnum); /* replace an NPC's held weapons with this one */
s32 chraiLuaChrWeapon(s32 chrnum); /* the NPC's current weaponnum (-1 if invalid) */
f32 chraiLuaPlayerHealth(void);               /* current health fraction 0..1 */
f32 chraiLuaPlayerShield(void);               /* current shield fraction 0..1 */
s32 chraiLuaPlayerReloading(void);            /* either hand mid-reload */
s32 chraiLuaPlayerActivate(void);             /* use/activate button held this frame */
s32 chraiLuaPlayerDamage(f32 amount);         /* hurt the local player via the real damage path */
s32 chraiLuaWeaponJam(s32 on);                /* trigger pulls dry-fire, no shot, no ammo */
s32 chraiLuaPlayerFreeze(s32 on);             /* root the local player (look/fire still live) */
s32 chraiLuaChrFreeze(s32 on);                /* statue every non-player chr */
s32 chraiLuaNoDrops(s32 on);                  /* dead chrs keep their weapons */
s32 chraiLuaPaintball(s32 on);                /* force paintball visuals */
s32 chraiLuaDamageScale(f32 frac);            /* scale all chr/player damage; 1 = off */
s32 chraiLuaZoomScale(f32 mult);              /* scale weapon aim-zoom FOV; >1 zooms OUT */
s32 chraiLuaGunSound(s32 weaponnum);          /* all guns fire with this weapon's shoot sound; 0 = off */
s32 chraiLuaMute(s32 on);                     /* master audio mute */
s32 chraiLuaPlayFile(const char *path, s32 loop, s32 followMusic); /* play an external WAV/MP3 through the device stream (followMusic: scale by the music volume) */
void chraiLuaStopFile(void);                  /* stop the pd.play_file sound */
s32 chraiLuaWeaponRename(s32 weaponnum, const char *name); /* relabel a weapon (nil restores) */
s32 chraiLuaChrSpeed(f32 mult);               /* scale all non-player chr anim/movement speed; 1 = off */
s32 chraiLuaPlayerSpeed(f32 mult);            /* scale the local player's walk/strafe speed; 1 = normal */
s32 chraiLuaChrDamage(s32 chrnum, f32 amount); /* hurt any chr via the real damage path */
s32 chraiLuaChrScale(s32 chrnum, f32 mult);   /* multiply a chr's visual scale */
s32 chraiLuaShake(s32 ticks);                 /* explosion screen-shake for N ticks */
s32 chraiLuaScreenTint(s32 r, s32 g, s32 b, s32 on); /* full-screen luminance tint; on=0 clears */
s32 chraiLuaUpsideDown(s32 on);               /* Australia: rotate frame 180 + reverse controls */
s32 chraiLuaScreenRoll(f32 deg);              /* Barrel Roll: roll the 3D view (absolute degrees, 0 = off) */
s32 chraiLuaPlayerAddYaw(f32 deg);            /* Speen: rotate the player's view yaw by deg degrees */
s32 chraiLuaPlayerSlip(f32 push, f32 pitchdeg); /* Banana peel: squat + forward shove (+ optional pitch snap) */
f32 chraiLuaPlayerPitchGet(void);             /* view pitch in degrees (+up) */
s32 chraiLuaPlayerPitchSet(f32 deg);          /* set view pitch (clamped +/-90) */
s32 chraiLuaPlayerPush(f32 mag);              /* shove along facing (+fwd/-back, knockback-style) */
s32 chraiLuaOneBullet(s32 on);                /* One Bullet Mags (equip-baked clip=1) */
s32 chraiLuaInvertLook(s32 on);               /* toggle the player's pitch-inversion setting */
s32 chraiLuaInputDelay(s32 frames);           /* Stadia Mode: pad reads N frames late (0=off) */
s32 chraiLuaUwuify(s32 on);                   /* uwuify every langGet string */
s32 chraiLuaPigLatin(s32 on);                 /* pig-latin every langGet string (shared mode) */
s32 chraiLuaForcedMarch(s32 on);              /* movement stick pinned full forward */
s32 chraiLuaForcedFire(s32 on);               /* trigger held down for you */
s32 chraiLuaRapidFire(s32 on);                /* semi-autos fire as fast as automatics while held */
s32 chraiLuaForcedCrouch(s32 on);             /* stance pinned to a crouch */
s32 chraiLuaNoReload(s32 on);                 /* all reload transitions refused */
s32 chraiLuaSpread(f32 mult);                 /* scale weapon shot spread */
s32 chraiLuaChrArmor(s32 chrnum, f32 amount); /* give an NPC body armor (negative-damage) */
s32 chraiLuaHeadshotsOnly(s32 on);            /* zero non-head damage to the local player */
s32 chraiLuaDropWeapon(s32 weaponnum);        /* drop a player weapon as a collectable pickup */
s32 chraiLuaHaunt(f32 force);                 /* hurl LOS-visible props at the player */
s32 chraiLuaTrapdoor(void);                   /* drop the local player through the floor */
s32 chraiLuaIceFloor(f32 mult);               /* scale walk accel/decel (slippery floors) */
f32 chraiLuaPlayerMoveSpeed(void);            /* local player normalised move speed 0..~1 */
s32 chraiLuaHudOff(s32 on);                   /* hide every HUD element */
s32 chraiLuaGunFov(f32 deg);                  /* viewmodel FOV override (0 restores) */
s32 chraiLuaChrFreezeOne(s32 chrnum);         /* statue exactly one chr (-1 = none) */
s32 chraiLuaButtsbot(s32 on);                 /* text mode 3: words become butt */
s32 chraiLuaIpodAd(s32 on, s32 r, s32 g, s32 b); /* iPod Ad silhouette mode */
#ifndef PLATFORM_N64
void luaTexOverrideReset(void);               /* free the pd.tex_override image + restore textures */
#endif
s32 chraiLuaBeyblade(s32 on);                 /* Bayblade!: spin every NPC's model yaw */
s32 chraiLuaDoubleVision(s32 on);             /* One too many: 180-flipped ghost blended over the frame */
s32 chraiLuaGunLock(s32 on);                  /* Cyclone Frenzy: force secondary + hold fire + no weapon switch */
s32 chraiLuaRubberObjects(s32 on);            /* Rubber Objects: newly-dropped items bounce instead of settling */
s32 chraiLuaYassify(s32 on);                  /* Yassify: cinched waist + broader shoulders + bigger head */
s32 chraiLuaMagDump(s32 on);                  /* Mag Dump: one trigger press empties the clip (hold auto / pulse semi) */
s32 chraiLuaKnifeLock(s32 on);                /* Knife fight: block weapon switching only (knife used normally) */
s32 chraiLuaCloakLock(s32 on);                /* unbreakable no-ammo player cloak (Now you see me) */
s32 chraiLuaWeather(s32 type, s32 intensity); /* 0 off / 1 rain / 2 snow, any stage */
s32 chraiLuaGas(s32 on);                      /* nerve gas on any stage (wash + cough + damage) */
s32 chraiLuaTPose(s32 on);                    /* all skeletal models render in bind pose */
s32 chraiLuaChrKo(s32 chrnum);                /* tranq-style knockout: collapse, drop gun, parked un-reaped */
s32 chraiLuaChrWake(s32 chrnum);              /* recover a KO'd chr: blend back to standing, AI resumes */
s32 chraiLuaPinball(s32 on);                  /* fired projectiles become proximity pinballs */
s32 chraiLuaRoomTint(s32 r, s32 g, s32 b, s32 on); /* stage-wide room lighting tint (KotH hill math) */
s32 chraiLuaPlayerExplosions(s32 on);         /* AFO crash explosions around the player */
s32 chraiLuaAmmoSwap(s32 weaponnum);          /* held guns fire this weapon's primary; -1 off */
s32 chraiLuaBackfire(s32 on);                 /* shots leave 180 degrees behind the player */
s32 chraiLuaNbomb(void);                      /* N-Bomb storm on the player */
s32 chraiLuaGust(f32 force);                  /* shove chrs/objects/player in one random direction */
s32 chraiLuaDualWield(s32 weaponnum, s32 funcnum); /* dual-equip a weapon; funcnum 0/1 forces fire func */
s32 chraiLuaAspectScale(f32 mult);            /* projection aspect multiplier (1.0 = normal) */
s32 chraiLuaPlaySong(s32 slot, f32 frac);     /* play an unlocked MP track over the stage music; -1 stops; frac>0 = start that far in */
s32 chraiLuaSpawnBody(s32 bodynum, s32 weaponnum, f32 dx, f32 dz, s32 sunglasses); /* hostile chr at player + offset */
s32 chraiLuaBodySnatch(s32 chrnum);           /* lite Counter-Op takeover of a chr (solo) */
s32 chraiLuaBodyUnsnatch(void);               /* end body_snatch: un-disguise + teleport home */
s32 chraiLuaChrTarget(s32 chrnum, s32 victimchrnum); /* point a chr's combat AI at another chr */
s32 chraiLuaChrCalm(s32 chrnum);              /* zero alertness, clear target (neuralyzer) */
s32 chraiLuaDoorsAll(s32 open);               /* open (1) / close (0) every door; returns count */
s32 chraiLuaDoorsLock(s32 on);                /* Lockdown: lock (1) / unlock (0) every door shut */
s32 chraiLuaCivilWar(s32 on);                 /* NPCs fight each other (hostile teams + nearest target); on=false restores */
s32 chraiLuaChrSummon(s32 chrnum, f32 dx, f32 dz); /* teleport a chr next to the player */
s32 chraiLuaFovScale(f32 mult);               /* vertical-FOV multiplier (1.0 = normal) */
s32 chraiLuaOnePunch(s32 on);                 /* unarmed strikes: lethal + mega knockback */
s32 chraiLuaGormless(s32 on);                 /* invert movement + look, swap fire/aim */
s32 chraiLuaSpawnBike(void);                  /* half-size hoverbike at the player (solo) */
s32 chraiLuaSfxShuffle(s32 on);               /* every SFX plays as a random other SFX */
s32 chraiLuaSfxReplace(s32 from, s32 to);     /* one sound id plays as another; -1,-1 = off */
s32 chraiLuaInstrumentShuffle(s32 on);        /* MIDI program changes pick random instruments */
s32 chraiLuaPixelate(s32 w, s32 h, s32 colors); /* pixelate to w x h; colours 4=grey4, 256=RGB332, 1000=invert, 1001=gameboy, 1002=thermal */
s32 chraiLuaScreenFx(s32 bits, s32 on);       /* post-fx bits: 1 scanlines, 2 grille, 4 curve, 8 vignette, 16 VHS, 32 wobble */
s32 chraiLuaLens(f32 k);                      /* fisheye lens warp; 0 = off */
s32 chraiLuaAudioCrush(s32 step, s32 bits);   /* sample-hold every Nth frame at `bits` depth; 1,16 = off */
s32 chraiLuaAudioRadio(s32 on);               /* AM-radio bandpass + overdrive */
s32 chraiLuaAudioReverb(f32 wet);             /* cathedral reverb, wet 0..1; 0 = off */
s32 chraiLuaAudioReverse(s32 on);             /* audio plays backwards in ~0.74s granules */
s32 chraiLuaAudioPitch(f32 rate);             /* constant-tempo pitch shift; 1 = off */
s32 chraiLuaExtVolume(s32 pct);               /* external-sound volume, % of music slider; <0 = get */
s32 chraiLuaForceSecondary(s32 on);           /* pin both hands to the secondary weapon function */
s32 chraiLuaButtonMask(u32 mask);             /* strip pad buttons from gameplay input; 0 = off */
s32 chraiLuaAmmoCost(s32 mult);               /* each shot spends mult clip rounds; 1 = normal */
s32 chraiLuaAutoAim(s32 on);                  /* force aim assist on regardless of the option */
s32 chraiLuaDeadzone(f32 frac);               /* analog deadzone floor 0..1 of full deflection; 0 = off */
s32 chraiLuaNitro(s32 on);                    /* destroyed objects explode like the Crash Site ship */
s32 chraiLuaObjectiveForce(s32 index, s32 state); /* 0 off / 1 force incomplete / 2 force complete; index -1 clears all */
s32 chraiLuaObjectiveStatus(s32 index);       /* real objective status (override bypassed); -1 if not live */
s32 chraiLuaMarkHome(void);                   /* record the player's position for warp_home */
s32 chraiLuaWarpHome(void);                   /* teleport back to the marked home position */
s32 chraiLuaEnv(s32 stagenum);                /* apply another stage's sky/fog environment; -1 restores */
s32 chraiLuaFog(s32 fogmin, s32 fogmax, s32 r, s32 g, s32 b); /* custom fog overlay (per-mille of z-range) */
s32 chraiLuaBloodColour(s32 r, s32 g, s32 b, s32 on); /* every body bleeds this colour; on=0 restores */
s32 chraiLuaMaxBlood(s32 on);                 /* every hit splatters big + max drip rate */
s32 chraiLuaItemsShuffle(void);               /* shuffle all loose weapon pickups' positions; returns count */
s32 chraiLuaChrWireframe(s32 on);             /* hostile chrs render as wireframe (G_CHRWIREFRAME_EXT) */
s32 chraiLuaDoubleShots(s32 on);              /* every fire event takes twice the shots (Quad handed) */
s32 chraiLuaQuadTop(s32 on);                  /* second pair of viewmodel guns at the top of the screen */
u32 chraiLuaButtons(s32 pressed);             /* raw local-player pad buttons (held / pressed this frame) */
s32 chraiLuaSpawnChopper(s32 kind, s32 extrascale); /* hostile chopper near the player: kind 0 = dD copter, 1 = A51 interceptor; extrascale 256 = full (<8 = per-kind default) */

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
#define LUA_MENU_MAX 640           /* max Director entries (shared with mainmenu.c); sized for
                                      chaos's per-effect Test triggers (~220) AND on/off toggles
                                      (~220) plus the director/AP toolkits — the dialog smooth-scrolls.
                                      Raised 256->640: the on/off list is registered LAST, so once the
                                      registry filled it silently truncated to a handful of toggles. */
#define LUA_MENU_LABEL 40          /* per-entry label / group buffer size (shared) */
#define LUA_MENU_DESC 224          /* per-entry scroll-panel description buffer size */
s32 luaMenuCount(void);            /* number of registered Director entries */
const char *luaMenuLabel(s32 i);   /* label of entry i ("" if out of range) */
const char *luaMenuGroup(s32 i);   /* submenu title of entry i ("" = root) */
void luaMenuInvoke(s32 i);         /* call entry i's Lua action fn (guarded, logged) */
/* Typed Director rows (cheats-style Chaos menu): kind 0 = action/selectable,
 * 1 = checkbox, 2 = slider. Checkbox/slider round-trip through Lua get/set. */
s32 luaMenuKind(s32 i);            /* row kind of entry i (0/1/2) */
const char *luaMenuDesc(s32 i);    /* scroll-panel description of entry i */
s32 luaMenuSliderMin(s32 i);       /* slider lower bound (kind 2) */
s32 luaMenuSliderMax(s32 i);       /* slider upper bound (kind 2) */
s32 luaMenuGetBool(s32 i);         /* call the checkbox getter -> 0/1 */
void luaMenuSetBool(s32 i, s32 v); /* call the checkbox setter with a bool */
s32 luaMenuGetInt(s32 i);          /* call the slider getter -> int */
void luaMenuSetInt(s32 i, s32 v);  /* call the slider setter with an int */

#define LUA_DIRECTOR_MAX_SUBMENUS 16 /* distinct submenu groups in the Director (raised
                                        12->16 for the cheats-style Chaos menu: a Chaos
                                        root + per-category Enable + Test folders + Alpha) */

/* Rebuild the Director menu items array from the registry (defined in
 * mainmenu.c). Called by pd.menu_add/menu_clear so the array is always valid +
 * current before any dialog open. No-op stub when the menu isn't compiled. */
void luaDirectorRebuild(void);

#endif
