/**
 * Determinism harness — see port/include/det.h.
 *
 * Phase 1+2: the canonical decomposable state hash (the hard part) and the
 * fixed-step pin. Record/replay (input capture/inject + per-tick compare) land
 * in a follow-up commit once the joy input seam is exposed.
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
#include "det.h"

s32 g_DetMode = DET_OFF;

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
	if (g_DetMode == DET_OFF) {
		return;
	}
	// Respect pause: when the engine chose a zero step (paused / cutscene gate),
	// leave it zero so a paused frame stays a no-op in both record and replay.
	if (g_Vars.lvupdate240 <= 0) {
		return;
	}
	// Pin to a fixed 1/60 step. This is called BEFORE lv.c derives lvupdate60 /
	// lvupdate60f / lvupdate60freal and advances the lvframe* counters, so the
	// existing derivation block produces fully deterministic values from these
	// two assignments — and the 200+ downstream `* lvupdate60f` sites inherit
	// the fixed step untouched. (lvupdate240=4 -> lvupdate60=1, lvupdate60f=1.0)
	g_Vars.lvupdate240 = 4;
	g_Vars.lvupdate240rem = 0;
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

	if (strcmp(cmd, "detinfo") == 0) {
		struct dethash h;
		detComputeHash(&h);
		const char *modestr = g_DetMode == DET_OFF ? "OFF" :
				g_DetMode == DET_PIN ? "PIN" :
				g_DetMode == DET_RECORD ? "RECORD" : "REPLAY";
		sysLogPrintf(LOG_CHAT, "DET: mode=%s lvframenum=%d all=%016llx",
				modestr, g_Vars.lvframenum, (unsigned long long)h.all);
		return 1;
	}

	return 0;
}
