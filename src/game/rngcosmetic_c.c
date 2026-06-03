#include <ultra64.h>
#include "constants.h"
#include "bss.h"
#include "lib/rng.h"
#include "data.h"
#include "types.h"

// Cosmetic RNG stream (port-only). Separate from the gameplay stream g_RngSeed
// (rng_c.c) so that purely-visual consumers — blood splats, sparks, smoke,
// muzzle flash, glass shards, bullet-hole decals, dynamic-light flicker, weather
// — no longer advance the gameplay stream. Those effects run (or don't) on each
// machine depending on what is on-screen, so drawing them from the shared
// gameplay stream drifts g_RngSeed differently per client and desyncs subsequent
// gameplay rolls. This stream is intentionally NOT synced over the network and
// is excluded from the determinism hash: it is allowed to diverge.
//
// Same 64-bit xorshift as rngRandom(); see docs/netplay-code-review-2026.md and
// the determinism plan (item 4).
u64 g_RngCosmeticSeed = 0xab8d9f7781280783;

u32 rngCosmeticRandom(void)
{
	g_RngCosmeticSeed = ((g_RngCosmeticSeed << 63) >> 31 | (g_RngCosmeticSeed << 31) >> 32) ^ (g_RngCosmeticSeed << 44) >> 32;
	g_RngCosmeticSeed = ((g_RngCosmeticSeed >> 20) & 0xfff) ^ g_RngCosmeticSeed;

	return g_RngCosmeticSeed;
}

void rngCosmeticSetSeed(u64 seed)
{
	g_RngCosmeticSeed = seed + 1; // +1 so it is never 0
}
