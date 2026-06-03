/**
 * Determinism harness — see port/include/det.h.
 *
 * Contains: the canonical decomposable state hash (the hard part), the
 * fixed-step pin, and input record/replay (capture/inject via the joy raw-ring
 * seam + per-tick hash compare).
 *
 * Field-selection rule for the hash: fold a field iff it is read-modified by the
 * per-tick sim AND is not a pointer, render scratch, audio handle, syncid, or
 * wall-clock value. Walk entities in a deterministic order (by index / list
 * order), never by memory address. Start with a minimal high-value field set
 * (positions / angles / health / RNG / counts) that catches gross desync; expand
 * later, re-checking a record→replay self-match after each expansion.
 */
#include <ultra64.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "constants.h"
#include "types.h"
#include "bss.h"   // g_Vars, g_NumChrs
#include "data.h"
#include "system.h"
#include "platform.h"
#include "config.h"
#include "lib/joy.h"
#include "video.h"
#include "det.h"

s32 g_DetMode = DET_OFF;

// Fixed-step gameplay tick (port-only; ON by default — Game.FixedTick /
// /fixedtick / /forcetick). When set, mainTick runs the gameplay sim a whole
// number of fixed steps per render frame (catch-up at low fps, render-only
// frames at high fps) instead of one variable-dt step, and detPinTimestep pins
// each lvTick to the chosen step. This LOCKS gameplay speed to the tick rate
// (default 60Hz = real-time) independent of the render frame rate, so unlocking
// fps gives more frames without speeding the sim up. Set to 0 (Game.FixedTick 0
// / /forcetick off) to fall back to the legacy variable-dt path, which couples
// the sim to fps via the mininc60 clamp (runs faster than 1x at high fps).
s32 g_FixedTickEnabled = 1;

// Gameplay tick RATE in ticks/sec (Game.FixedTickRate / `/forcetick <n>`). The
// sim runs in REAL TIME at this rate: each tick advances game-time by 1/rate of
// a second (detPinTimestep pins the per-tick dt to 240/rate in 1/240ths), so the
// game runs at normal speed but in coarser/finer chunks. 60 = the normal 1/60
// step (default, smooth real-time). 20 = 1/20-sec steps. 1 = one tick per second,
// each a 1-second MEGA-STEP — an intentionally extreme test mode where the sim
// sees a huge dt (entities lurch a full second per tick); useful for stressing
// interpolation / observing coarse server-tick behaviour. Capped at 240 (the
// finest step, lvupdate240 >= 1).
s32 g_FixedTickRate = 60;

PD_CONSTRUCTOR static void detConfigInit(void)
{
	configRegisterInt("Game.FixedTick", &g_FixedTickEnabled, 0, 1);
	configRegisterInt("Game.FixedTickRate", &g_FixedTickRate, 1, 1000);
}

// Gameplay RNG streams (extern'd here to avoid pulling the rng headers).
extern u64 g_RngSeed;   // src/lib/rng_c.c
extern u64 g_Rng2Seed;  // src/game/rng2_c.c

/* ---- FNV-1a 64-bit fold helpers ---- */

#define DET_FNV_OFFSET 0xcbf29ce484222325ULL
#define DET_FNV_PRIME  0x00000100000001b3ULL

static void detFoldBytes(u64 *h, const void *p, u32 n)
{
	const u8 *b = (const u8 *)p;
	u64 x = *h;
	u32 i;
	for (i = 0; i < n; i++) {
		x ^= (u64)b[i];
		x *= DET_FNV_PRIME;
	}
	*h = x;
}

static void detFoldU8(u64 *h, u8 v)   { detFoldBytes(h, &v, sizeof(v)); }
static void detFoldU16(u64 *h, u16 v) { detFoldBytes(h, &v, sizeof(v)); }
static void detFoldU32(u64 *h, u32 v) { detFoldBytes(h, &v, sizeof(v)); }
static void detFoldU64(u64 *h, u64 v) { detFoldBytes(h, &v, sizeof(v)); }
static void detFoldS32(u64 *h, s32 v) { detFoldU32(h, (u32)v); }

static void detFoldF32(u64 *h, f32 v)
{
	union { f32 f; u32 i; } u;
	if (v == 0.0f) {
		u.f = 0.0f;          // normalize -0.0 -> +0.0
	} else if (v != v) {
		u.i = 0x7FC00000;    // canonical NaN sentinel
	} else {
		u.f = v;
	}
	detFoldU32(h, u.i);
}

static void detFoldCoord(u64 *h, const struct coord *c)
{
	detFoldF32(h, c->x);
	detFoldF32(h, c->y);
	detFoldF32(h, c->z);
}

/* ---- per-entity hashing (minimal high-value field sets) ---- */

static void detHashPlayer(u64 *h, const struct player *pl)
{
	// View angles + movement speeds + ground/fall + crouch + health. The
	// player's world position is folded via the prop pass (pl->prop).
	detFoldF32(h, pl->vv_theta);
	detFoldF32(h, pl->vv_verta);
	detFoldF32(h, pl->speedsideways);
	detFoldF32(h, pl->speedstrafe);
	detFoldF32(h, pl->speedforwards);
	detFoldF32(h, pl->speedboost);
	detFoldS32(h, pl->speedmaxtime60);
	detFoldF32(h, pl->sumground);
	detFoldF32(h, pl->vv_manground);
	detFoldF32(h, pl->vv_ground);
	detFoldCoord(h, &pl->bdeltapos);
	detFoldS32(h, pl->isfalling);
	detFoldS32(h, pl->fallstart);
	detFoldS32(h, pl->crouchpos);
	detFoldS32(h, pl->autocrouchpos);
	detFoldF32(h, pl->crouchoffset);
	detFoldS32(h, pl->isdead);
	detFoldF32(h, pl->bondhealth);
	detFoldF32(h, pl->oldhealth);
	detFoldF32(h, pl->oldarmour);
	detFoldF32(h, pl->apparenthealth);
	detFoldF32(h, pl->apparentarmour);
}

static void detHashProp(u64 *h, const struct prop *p)
{
	detFoldU8(h, p->type);
	detFoldU8(h, p->flags);
	detFoldU16(h, (u16)p->timetoregen);
	detFoldCoord(h, &p->pos);
	detFoldF32(h, p->z);
	s32 i;
	for (i = 0; i < 8; i++) {
		detFoldS32(h, (s32)p->rooms[i]);
	}
	detFoldU8(h, (u8)p->active);
	// SKIP: union ptr, parent/child/next/prev, wallhits, syncid, and the
	// backgrounding/update-slot scratch (render/onscreen-dependent).
}

static void detHashChr(u64 *h, const struct chrdata *chr)
{
	detFoldU16(h, (u16)chr->chrnum);
	detFoldU8(h, (u8)chr->actiontype);
	detFoldU32(h, chr->flags);
	detFoldU32(h, chr->flags2);
	detFoldU32(h, chr->chrflags);
	detFoldU32(h, chr->hidden);
	detFoldF32(h, chr->damage);
	detFoldF32(h, chr->maxdamage);
	detFoldF32(h, chr->cshield);
	detFoldS32(h, chr->timer60);
	detFoldCoord(h, &chr->prevpos);
	detFoldCoord(h, &chr->fallspeed);
	detFoldF32(h, chr->ground);
	detFoldF32(h, chr->manground);
	detFoldF32(h, chr->sumground);
	detFoldS32(h, (s32)chr->target);
	detFoldU8(h, chr->morale);
	detFoldU8(h, chr->alertness);
	detFoldU8(h, chr->random);
	detFoldF32(h, chr->oldframe);
	detFoldF32(h, chr->magicframe);
	detFoldF32(h, chr->magicspeed);
	detFoldS32(h, (s32)chr->magicanim);
	detFoldU16(h, chr->aioffset);
	detFoldS32(h, (s32)chr->aireturnlist);
	detFoldS32(h, (s32)chr->aishotlist);
	// SKIP: the act_* union (discriminator `actiontype` above catches state-
	// machine divergence; fold the union only after a per-variant pointer
	// audit), all pointers/model/weapons, visual cm*/shadecol fields.
}

void detComputeHash(struct dethash *out)
{
	out->rng = DET_FNV_OFFSET;
	out->players = DET_FNV_OFFSET;
	out->props = DET_FNV_OFFSET;
	out->chrs = DET_FNV_OFFSET;
	out->all = DET_FNV_OFFSET;

	// rng: both gameplay streams + the step-sensitive frame counters (these
	// advance deterministically once the step is pinned, so a mismatch flags a
	// step desync immediately). The cosmetic RNG stream is intentionally NOT
	// folded — it is allowed to diverge.
	detFoldU64(&out->rng, g_RngSeed);
	detFoldU64(&out->rng, g_Rng2Seed);
	detFoldS32(&out->rng, g_Vars.lvframenum);
	detFoldS32(&out->rng, g_Vars.lvframe60);
	detFoldS32(&out->rng, g_Vars.lvframe240);
	detFoldS32(&out->rng, g_Vars.lvupdate240rem);

	// players: by index, skipping empty / spectator slots.
	{
		s32 i;
		for (i = 0; i < MAX_PLAYERS; i++) {
			const struct player *pl = g_Vars.players[i];
			if (pl == NULL || pl->is_spectator) {
				continue;
			}
			detFoldS32(&out->players, i);
			detHashPlayer(&out->players, pl);
		}
	}

	// props: walk the active list head->tail in list order; fold the count
	// first so a count mismatch is caught before any field divergence.
	{
		struct prop *p = g_Vars.activeprops;
		s32 count = 0;
		const s32 maxprops = g_Vars.maxprops;
		while (p != NULL) {
			detHashProp(&out->props, p);
			count++;
			if (p == g_Vars.activepropstail) {
				break;
			}
			p = p->next;
			if (count > maxprops) {
				break; // safety against a corrupt/cyclic list
			}
		}
		detFoldS32(&out->props, count);
	}

	// chrs: by array index.
	{
		s32 i;
		const s32 n = g_NumChrs;
		detFoldS32(&out->chrs, n);
		for (i = 0; i < n; i++) {
			detHashChr(&out->chrs, &g_Vars.chrdata[i]);
		}
	}

	// combined
	detFoldU64(&out->all, out->rng);
	detFoldU64(&out->all, out->players);
	detFoldU64(&out->all, out->props);
	detFoldU64(&out->all, out->chrs);
}

/* ---- fixed-step pin ---- */

void detPinTimestep(void)
{
	if (g_DetMode == DET_OFF && !g_FixedTickEnabled) {
		return;
	}
	// Respect pause: when the engine chose a zero step (paused / cutscene gate),
	// leave it zero so a paused frame stays a no-op in both record and replay.
	if (g_Vars.lvupdate240 <= 0) {
		return;
	}
	// Pick the per-tick step (in 1/240ths). This is called BEFORE lv.c derives
	// lvupdate60 / lvupdate60f / lvupdate60freal and advances the lvframe*
	// counters, so the existing derivation block produces fully deterministic
	// values from these two assignments — and the 200+ downstream `* lvupdate60f`
	// sites inherit the step untouched. (lvupdate240=4 -> lvupdate60=1,
	// lvupdate60f=1.0.)
	s32 step = 4; // det record/replay: always a fixed 1/60 step for reproducibility
	if (g_DetMode != DET_RECORD && g_DetMode != DET_REPLAY && g_FixedTickEnabled) {
		// /forcetick active (and not pinned for a deterministic record/replay):
		// pin to the chosen tick rate so each tick advances 1/rate of a second
		// (real-time-coarse). At rate=1 this is a 240/240 = 1-second MEGA-STEP.
		// The condition matches the mainTick accumulator (which runs for every
		// mode except RECORD/REPLAY), keeping the per-tick size and the step count
		// in sync — otherwise game speed would scale by rate/60.
		s32 rate = g_FixedTickRate;
		if (rate < 1) {
			rate = 1;
		} else if (rate > 240) {
			rate = 240;
		}
		step = 240 / rate;
		if (step < 1) {
			step = 1;
		}
	}
	g_Vars.lvupdate240 = step;
	g_Vars.lvupdate240rem = 0;
}

/* ---- record / replay ---- */

// Bump DET_FIELDSET_VER whenever the set of fields folded into the hash changes,
// so a replay against a recording made with a different field set is rejected
// (its hashes would otherwise spuriously "diverge").
#define DET_MAGIC        0x31544544u // "DET1"
#define DET_VERSION      1u
#define DET_FIELDSET_VER 1u

#define DET_MAX_RING 20
#define DET_MAX_PADS 4

struct detfileheader {
	u32 magic;
	u32 version;
	u32 ringsize;
	u32 padcount;
	u32 oscontpadsize;
	u32 fieldsetver;
};

// One frame's reproducible input slice: the whole ring + its geometry. Snapshot
// the entire ring (not just the cur window) so replay is bulletproof against the
// intra-frame cur-window mapping — record and replay are the same build, so this
// is exact.
struct detframeinputs {
	s32 lvframenum;
	s32 curstart;
	s32 curlast;
	s32 ringsize;
	s32 padcount;
	OSContPad pads[DET_MAX_RING * DET_MAX_PADS];
};

static FILE *g_DetFile = NULL;
static struct detframeinputs g_DetIn;
static struct dethash g_DetExpected;
static s32 g_DetReplayDone = 0;
static s32 g_DetFrameCount = 0;
static s32 g_DetMismatchCount = 0;
static s32 g_DetFirstDivergeFrame = -1;
static const char *g_DetFirstDivergeSub = "";

static void detClose(void)
{
	if (g_DetFile) {
		fclose(g_DetFile);
		g_DetFile = NULL;
	}
}

static void detResetReplayState(void)
{
	g_DetReplayDone = 0;
	g_DetFrameCount = 0;
	g_DetMismatchCount = 0;
	g_DetFirstDivergeFrame = -1;
	g_DetFirstDivergeSub = "";
}

static void detCaptureInputs(struct detframeinputs *in)
{
	in->lvframenum = g_Vars.lvframenum;
	in->curstart = joyGetCurStart();
	in->curlast = joyGetCurLast();
	in->ringsize = joyGetRingSize();
	in->padcount = joyGetPadCount();
	if (in->ringsize > DET_MAX_RING) in->ringsize = DET_MAX_RING;
	if (in->padcount > DET_MAX_PADS) in->padcount = DET_MAX_PADS;
	s32 r, p;
	for (r = 0; r < in->ringsize; r++) {
		for (p = 0; p < in->padcount; p++) {
			joyGetRawSample(r, p, &in->pads[r * in->padcount + p]);
		}
	}
}

static void detInjectInputs(const struct detframeinputs *in)
{
	s32 r, p;
	for (r = 0; r < in->ringsize; r++) {
		for (p = 0; p < in->padcount; p++) {
			joySetRawSample(r, p, &in->pads[r * in->padcount + p]);
		}
	}
	joySetCurWindow(in->curstart, in->curlast);
}

static s32 detWriteHeader(void)
{
	struct detfileheader hdr;
	hdr.magic = DET_MAGIC;
	hdr.version = DET_VERSION;
	hdr.ringsize = (u32)joyGetRingSize();
	hdr.padcount = (u32)joyGetPadCount();
	hdr.oscontpadsize = (u32)sizeof(OSContPad);
	hdr.fieldsetver = DET_FIELDSET_VER;
	return fwrite(&hdr, sizeof(hdr), 1, g_DetFile) == 1;
}

static s32 detCheckHeader(void)
{
	struct detfileheader hdr;
	if (fread(&hdr, sizeof(hdr), 1, g_DetFile) != 1) {
		return 0;
	}
	if (hdr.magic != DET_MAGIC || hdr.version != DET_VERSION
			|| hdr.oscontpadsize != (u32)sizeof(OSContPad)
			|| hdr.fieldsetver != DET_FIELDSET_VER) {
		sysLogPrintf(LOG_CHAT, "DET: recording incompatible (magic/version/fieldset mismatch)");
		return 0;
	}
	return 1;
}

static void detWriteFrame(const struct detframeinputs *in, const struct dethash *h)
{
	const s32 n = in->ringsize * in->padcount;
	fwrite(&in->lvframenum, sizeof(s32), 1, g_DetFile);
	fwrite(&in->curstart, sizeof(s32), 1, g_DetFile);
	fwrite(&in->curlast, sizeof(s32), 1, g_DetFile);
	fwrite(&in->ringsize, sizeof(s32), 1, g_DetFile);
	fwrite(&in->padcount, sizeof(s32), 1, g_DetFile);
	fwrite(in->pads, sizeof(OSContPad), n, g_DetFile);
	fwrite(h, sizeof(struct dethash), 1, g_DetFile);
	fflush(g_DetFile);
}

// Returns 1 on a full frame read, 0 on EOF / short read.
static s32 detReadFrame(struct detframeinputs *in, struct dethash *expected)
{
	if (fread(&in->lvframenum, sizeof(s32), 1, g_DetFile) != 1) return 0;
	if (fread(&in->curstart, sizeof(s32), 1, g_DetFile) != 1) return 0;
	if (fread(&in->curlast, sizeof(s32), 1, g_DetFile) != 1) return 0;
	if (fread(&in->ringsize, sizeof(s32), 1, g_DetFile) != 1) return 0;
	if (fread(&in->padcount, sizeof(s32), 1, g_DetFile) != 1) return 0;
	if (in->ringsize < 0 || in->ringsize > DET_MAX_RING
			|| in->padcount < 0 || in->padcount > DET_MAX_PADS) {
		return 0;
	}
	const s32 n = in->ringsize * in->padcount;
	if ((s32)fread(in->pads, sizeof(OSContPad), n, g_DetFile) != n) return 0;
	if (fread(expected, sizeof(struct dethash), 1, g_DetFile) != 1) return 0;
	return 1;
}

static void detCompare(const struct dethash *got, const struct dethash *exp, s32 frame)
{
	if (got->all == exp->all) {
		return;
	}
	g_DetMismatchCount++;
	if (g_DetFirstDivergeFrame >= 0) {
		return; // already reported the first divergence; state has forked
	}
	const char *sub =
			got->rng != exp->rng ? "RNG" :
			got->players != exp->players ? "PLAYERS" :
			got->props != exp->props ? "PROPS" :
			got->chrs != exp->chrs ? "CHRS" : "ALL";
	g_DetFirstDivergeFrame = frame;
	g_DetFirstDivergeSub = sub;
	sysLogPrintf(LOG_CHAT, "DET: DIVERGE frame=%d sub=%s exp=%016llx got=%016llx",
			frame, sub, (unsigned long long)exp->all, (unsigned long long)got->all);
}

void detFrameBegin(void)
{
	if (g_DetMode == DET_RECORD) {
		detCaptureInputs(&g_DetIn);
	} else if (g_DetMode == DET_REPLAY && !g_DetReplayDone) {
		if (!detReadFrame(&g_DetIn, &g_DetExpected)) {
			g_DetReplayDone = 1;
			sysLogPrintf(LOG_CHAT, "DET: replay end, %d frames, %d mismatch%s%s",
					g_DetFrameCount, g_DetMismatchCount,
					g_DetMismatchCount == 1 ? "" : "es",
					g_DetMismatchCount == 0 ? " (DETERMINISTIC)" : "");
			detClose();
			g_DetMode = DET_OFF;
			return;
		}
		detInjectInputs(&g_DetIn);
	}
}

void detEndTick(void)
{
	if (g_DetMode == DET_RECORD) {
		struct dethash h;
		detComputeHash(&h);
		detWriteFrame(&g_DetIn, &h);
		g_DetFrameCount++;
	} else if (g_DetMode == DET_REPLAY && !g_DetReplayDone) {
		struct dethash h;
		detComputeHash(&h);
		detCompare(&h, &g_DetExpected, g_DetIn.lvframenum);
		g_DetFrameCount++;
	}
}

/* ---- console ---- */

s32 detConsoleCommand(const char *cmd, const char *arg)
{
	if (strcmp(cmd, "dethash") == 0) {
		struct dethash h;
		detComputeHash(&h);
		sysLogPrintf(LOG_CHAT, "DET: all=%016llx rng=%016llx pl=%016llx prop=%016llx chr=%016llx",
				(unsigned long long)h.all, (unsigned long long)h.rng,
				(unsigned long long)h.players, (unsigned long long)h.props,
				(unsigned long long)h.chrs);
		return 1;
	}

	if (strcmp(cmd, "detpin") == 0) {
		if (strcmp(arg, "on") == 0) {
			if (g_DetMode == DET_OFF) {
				g_DetMode = DET_PIN;
			}
			sysLogPrintf(LOG_CHAT, "DET: fixed-step pin ON (mode=%d)", g_DetMode);
		} else if (strcmp(arg, "off") == 0) {
			if (g_DetMode == DET_PIN) {
				g_DetMode = DET_OFF;
			}
			sysLogPrintf(LOG_CHAT, "DET: fixed-step pin OFF (mode=%d)", g_DetMode);
		} else {
			sysLogPrintf(LOG_CHAT, "DET: pin is %s (usage: /detpin on|off)",
					g_DetMode != DET_OFF ? "active" : "off");
		}
		return 1;
	}

	if (strcmp(cmd, "detrec") == 0) {
		detClose();
		if (g_DetMode == DET_RECORD || g_DetMode == DET_REPLAY) {
			g_DetMode = DET_OFF;
		}
		if (*arg) {
			g_DetFile = fopen(arg, "wb");
			if (!g_DetFile || !detWriteHeader()) {
				detClose();
				sysLogPrintf(LOG_CHAT, "DET: could not open '%s' for record", arg);
				return 1;
			}
			detResetReplayState();
			g_DetMode = DET_RECORD;
			sysLogPrintf(LOG_CHAT, "DET: recording to '%s' (fixed step pinned)", arg);
		} else {
			sysLogPrintf(LOG_CHAT, "DET: recording stopped (%d frames written)", g_DetFrameCount);
		}
		return 1;
	}

	if (strcmp(cmd, "detplay") == 0) {
		detClose();
		if (g_DetMode == DET_RECORD || g_DetMode == DET_REPLAY) {
			g_DetMode = DET_OFF;
		}
		if (*arg) {
			g_DetFile = fopen(arg, "rb");
			if (!g_DetFile || !detCheckHeader()) {
				detClose();
				sysLogPrintf(LOG_CHAT, "DET: could not open/validate '%s' for replay", arg);
				return 1;
			}
			detResetReplayState();
			g_DetMode = DET_REPLAY;
			sysLogPrintf(LOG_CHAT, "DET: replaying '%s' (fixed step pinned)", arg);
		} else {
			sysLogPrintf(LOG_CHAT, "DET: replay stopped");
		}
		return 1;
	}

	// /fixedtick and /forcetick are aliases: control the fixed-step gameplay tick.
	// Accepts an integer tick RATE (ticks/sec), real-time-coarse: /forcetick 60 =
	// normal 1/60 step, /forcetick 20 = 1/20-sec steps, /forcetick 1 = one tick/sec
	// (a 1-second mega-step per tick). on = enable at the current rate, off / 0 =
	// disable, bare = toggle.
	if (strcmp(cmd, "fixedtick") == 0 || strcmp(cmd, "forcetick") == 0) {
		if (*arg == '\0') {
			g_FixedTickEnabled = !g_FixedTickEnabled; // bare command toggles
		} else if (strcmp(arg, "on") == 0) {
			g_FixedTickEnabled = 1;
		} else if (strcmp(arg, "off") == 0) {
			g_FixedTickEnabled = 0;
		} else {
			s32 n = atoi(arg);
			if (n > 0) {
				if (n > 1000) {
					n = 1000;
				}
				g_FixedTickRate = n;
				g_FixedTickEnabled = 1;
			} else {
				g_FixedTickEnabled = 0; // "0" or non-numeric => disable
			}
		}
		sysLogPrintf(LOG_CHAT,
				"DET: fixed tick = %s @ %d/sec (1/%d-sec step; 60 = normal; usage: /forcetick <n>|on|off)",
				g_FixedTickEnabled ? "ON" : "OFF", g_FixedTickRate, g_FixedTickRate);
		return 1;
	}

	// /framelimit <n> — set the render framerate cap. 0 = truly unlimited (no
	// pacing) in single-player. During netplay the netplay ceiling (/netframelimit)
	// still applies on top of this. With vsync on, presentation is paced by the
	// vblank regardless. (Deliberately doesn't call videoGetFramerateLimit, which
	// mutates the stored limit from the live target — during netplay that target
	// is the netplay cap and would clobber the single-player value.)
	if (strcmp(cmd, "framelimit") == 0) {
		if (*arg) {
			const s32 n = atoi(arg);
			videoSetFramerateLimit(n);
			sysLogPrintf(LOG_CHAT, "VIDEO: framerate limit = %d%s",
					n, n == 0 ? " (unlimited)" : "");
		} else {
			sysLogPrintf(LOG_CHAT, "VIDEO: usage /framelimit <n> (0 = unlimited)");
		}
		return 1;
	}

	// /netframelimit <n> — set the NETPLAY-only render ceiling. g_NetTick advances
	// per render frame, so this bounds the fps that interpolation/lag-comp timing
	// is measured against. 0 disables the netplay cap (then /framelimit applies;
	// 0 there = unlimited even in netplay — may distort net timing).
	if (strcmp(cmd, "netframelimit") == 0) {
		if (*arg) {
			videoSetNetplayFramerateLimit(atoi(arg));
		}
		const s32 nfl = videoGetNetplayFramerateLimit();
		sysLogPrintf(LOG_CHAT, "VIDEO: netplay framerate cap = %d%s (usage: /netframelimit <n>, 0 = no cap)",
				nfl, nfl == 0 ? " (no cap)" : "");
		return 1;
	}

	if (strcmp(cmd, "detinfo") == 0) {
		struct dethash h;
		detComputeHash(&h);
		const char *modestr = g_DetMode == DET_OFF ? "OFF" :
				g_DetMode == DET_PIN ? "PIN" :
				g_DetMode == DET_RECORD ? "RECORD" : "REPLAY";
		sysLogPrintf(LOG_CHAT, "DET: mode=%s frames=%d lvframenum=%d all=%016llx",
				modestr, g_DetFrameCount, g_Vars.lvframenum, (unsigned long long)h.all);
		if (g_DetMismatchCount > 0) {
			sysLogPrintf(LOG_CHAT, "DET: %d mismatches; first diverge frame=%d sub=%s",
					g_DetMismatchCount, g_DetFirstDivergeFrame, g_DetFirstDivergeSub);
		}
		return 1;
	}

	return 0;
}
