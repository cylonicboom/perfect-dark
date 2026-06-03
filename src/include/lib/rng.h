#ifndef _IN_LIB_RNG_H
#define _IN_LIB_RNG_H
#include <ultra64.h>
#include "data.h"
#include "types.h"

u32 rngRandom(void);
u32 rngRotateSeed(u64 *value);

// Cosmetic RNG stream (port-only) — see src/game/rngcosmetic_c.c. Declared here
// (alongside rngRandom) so the visual-effect modules that already include rng.h
// pick it up without new includes. NOT network-synced; allowed to diverge.
extern u64 g_RngCosmeticSeed;
u32 rngCosmeticRandom(void);
void rngCosmeticSetSeed(u64 seed);

#endif
