#include "platform.h"
#include <PR/ultratypes.h>
#include <stdlib.h>
#include "system.h"
#include "headless.h"

#ifdef _WIN32
#include <windows.h>

// Windows console control handler. Fires for Ctrl-C, the window's X button
// (CTRL_CLOSE_EVENT), logoff, and shutdown. Without this, clicking X on the
// cmd window kills the console but leaves the headless server orphaned in
// the background — the user has to Task-Manager it. ExitProcess triggers
// the registered atexit cleanup() which calls netDisconnect to flush ENet
// and close the diag log gracefully.
//
// CTRL_CLOSE_EVENT only gives the process ~5 seconds before Windows force-
// terminates, so cleanup must stay quick. netDisconnect + close-log fits
// well under that budget.
static BOOL WINAPI headlessConsoleHandler(DWORD ctrl)
{
	switch (ctrl) {
	case CTRL_C_EVENT:
	case CTRL_BREAK_EVENT:
	case CTRL_CLOSE_EVENT:
	case CTRL_LOGOFF_EVENT:
	case CTRL_SHUTDOWN_EVENT:
		sysLogPrintf(LOG_NOTE, "headless: console signal %lu, force-shutting down", (unsigned long)ctrl);
		// _exit bypasses atexit cleanup (which calls netDisconnect — that
		// can block on ENet peer teardown if a client is unresponsive).
		// The OS reclaims the UDP socket immediately. Diag log is the only
		// thing that loses its tail; flush before exiting.
		_exit(0);
		return TRUE;
	}
	return FALSE;
}

void headlessInstallSignalHandlers(void)
{
	SetConsoleCtrlHandler(headlessConsoleHandler, TRUE);
}

#else // POSIX

#include <signal.h>
#include <unistd.h>

static void headlessPosixSignal(int sig)
{
	sysLogPrintf(LOG_NOTE, "headless: signal %d, force-shutting down", sig);
	_exit(0);
}

void headlessInstallSignalHandlers(void)
{
	signal(SIGINT,  headlessPosixSignal);
	signal(SIGTERM, headlessPosixSignal);
	signal(SIGHUP,  headlessPosixSignal);
}

#endif

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
		// sysSleep takes 100-nanosecond units (Windows FILETIME convention);
		// 1us = 10 of those. Uses nanosleep on POSIX / waitable timer on
		// Windows, so no SDL dependency for the headless pacer.
		sysSleep((s64)sleep_us * 10);
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
