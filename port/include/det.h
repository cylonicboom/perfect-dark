#ifndef _IN_PORT_DET_H
#define _IN_PORT_DET_H

#include <ultra64.h>
#include "types.h"

// Determinism harness (port-only).
//
// Goal: measure sim determinism so the peer-determinism refactors (explicit
// player threading, global fixed timestep, sim/render decouple, entity
// flattening) can each be validated. It records per-tick inputs + a decomposable
// whole-state hash and replays them, asserting the per-tick hashes match.
//
// This is the measurement instrument named by the codebase review as the
// prerequisite for any peer netcode ("a desync with no repro harness is
// unfixable"). It is a no-op during normal play via a runtime g_DetMode check —
// there is NO compile-time guard, and normal play is unaffected when DET_OFF.
//
// Phase 1 (this header's initial surface): the canonical state hash + /dethash.
// Phase 2: the fixed-step pin (/detpin). Record/replay land in later phases.

#define DET_OFF    0
#define DET_PIN    1 // fixed-step pin only (for /detpin testing)
#define DET_RECORD 2
#define DET_REPLAY 3

// Decomposable per-tick state hash. Each accumulator is an independent FNV-1a
// rolling hash so a divergence report can name which subsystem diverged; `all`
// folds the four together.
struct dethash {
	u64 rng;
	u64 players;
	u64 props;
	u64 chrs;
	u64 all;
};

extern s32 g_DetMode;

// Fixed 60 Hz gameplay tick toggle (config Game.FixedTick / console /fixedtick).
// When non-zero, mainTick runs the gameplay sim in whole 1/60 steps decoupled
// from the render frame rate (see det.c). Default 0 (original variable-dt path).
extern s32 g_FixedTickEnabled;

// Number of fixed 1/60 steps to run per real second when g_FixedTickEnabled is
// set. 60 = real-time; lower = slow-motion; higher = fast-forward. See det.c.
extern s32 g_FixedTickRate;

// Fixed-tick camera interpolation (tickrate phase 2, det.c)
extern f32 g_TickInterpAlpha;
extern s32 g_TickThisFrameAdvanced;

// Compute the four sub-hashes (+ combined) over the current live sim state.
// Walks entities in a deterministic order (by index / list order, never by
// address) and folds only sim-authoritative value fields — never pointers,
// render scratch, audio handles, syncids, or wall-clock timing fields.
void detComputeHash(struct dethash *out);

// Fixed-step pin. Called from the lvupdate derivation in lv.c. No-op unless the
// harness is active; when active (and not paused) it forces the gameplay step to
// a fixed 1/60 so record and replay advance identically regardless of wall clock.
void detPinTimestep(void);

// True when detPinTimestep will override lvupdate240 (det harness, fixed tick,
// or netplay). lvTick's slow-motion halver stands down when active — the
// halving happens on the pinned step inside detPinTimestep instead.
s32 detTickPinActive(void);

// Record/replay per-frame hooks. Called from the sim loop in main.c around the
// per-player tick: detFrameBegin() just before the loop (capture inputs in
// record mode / inject recorded inputs in replay mode), detEndTick() just after
// it (write the {inputs, state-hash} record, or recompute the hash and compare
// against the recording). Both are no-ops unless g_DetMode is RECORD/REPLAY.
void detFrameBegin(void);
void detEndTick(void);

// Console command hook, chained from netConsoleCommand. Returns 1 if the command
// word was consumed, 0 otherwise. Handles: dethash, detpin, detinfo, detrec,
// detplay.
s32 detConsoleCommand(const char *cmd, const char *arg);

#endif
