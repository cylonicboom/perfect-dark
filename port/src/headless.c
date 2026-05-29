#include <SDL.h>
#include <PR/ultratypes.h>
#include "platform.h"
#include "system.h"
#include "headless.h"

// SDL_Delay granularity is ~1ms on most platforms; we sleep for (target_us -
// now - 1500us) and busy-wait the last bit so the cadence stays tight.
#define HEADLESS_SPIN_THRESHOLD_US 1500ULL

static u64 s_nextTickUs = 0;

void headlessPace(s32 target_hz)
{
	if (target_hz <= 0) {
		return;
	}

	const u64 period_us = 1000000ULL / (u64)target_hz;
	const u64 now_us = sysGetMicroseconds();

	if (s_nextTickUs == 0) {
		s_nextTickUs = now_us + period_us;
		return;
	}

	if (s_nextTickUs > now_us + HEADLESS_SPIN_THRESHOLD_US) {
		const u64 sleep_us = (s_nextTickUs - now_us) - HEADLESS_SPIN_THRESHOLD_US;
		SDL_Delay((u32)(sleep_us / 1000ULL));
	}

	while (sysGetMicroseconds() < s_nextTickUs) {
		// brief spin for sub-ms precision
	}

	s_nextTickUs += period_us;

	// If we've fallen more than 4 ticks behind (heavy stall on a worker
	// thread, debugger break, etc.) snap forward instead of trying to catch
	// up — repeated catch-up bursts would starve clients waiting on the wire.
	const u64 now2 = sysGetMicroseconds();
	if (s_nextTickUs + (period_us * 4) < now2) {
		s_nextTickUs = now2 + period_us;
	}
}
