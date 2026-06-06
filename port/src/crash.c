#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#ifndef DEDICATED_SERVER
#include <SDL3/SDL.h>
#endif
#include <PR/ultratypes.h>
#include "system.h"
#include "platform.h"

#define CRASH_LOG_FNAME "pd.crash.log"
#define CRASH_MAX_MSG 8192
#define CRASH_MAX_SYM 256
#define CRASH_MAX_FRAMES 32
#define CRASH_MSG(...) \
	if (msglen < CRASH_MAX_MSG) msglen += snprintf(msg + msglen, CRASH_MAX_MSG - msglen, __VA_ARGS__)

#if defined(PLATFORM_WIN32) || defined(PLATFORM_LINUX)
// Write the crash dump to pd.crash.log. Tried in order: next to the exe
// (where users look for it), then the working directory (the old behaviour —
// a double-clicked exe has CWD == exe dir, but shortcuts/launchers often
// don't), then the home dir. Always written, even when a regular log is open
// — the dump also reaches that log via sysFatalError, but the standalone
// artifact is what bug reports ask for (the old handler skipped it whenever
// --log was active: "I got the crash screen but there's no pd.crash.log").
// Returns the path written (static storage) or NULL if every location failed.
static const char *crashWriteLogFile(const char *msg)
{
	static char path[2048];
	char dir[2048] = "";
	FILE *f = NULL;

	sysGetExecutablePath(dir, sizeof(dir) - 1);
	if (dir[0]) {
		snprintf(path, sizeof(path), "%s/%s", dir, CRASH_LOG_FNAME);
		f = fopen(path, "wb");
	}

	if (!f) {
		snprintf(path, sizeof(path), "./%s", CRASH_LOG_FNAME);
		f = fopen(path, "wb");
	}

	if (!f) {
		dir[0] = '\0';
		sysGetHomePath(dir, sizeof(dir) - 1);
		if (dir[0]) {
			snprintf(path, sizeof(path), "%s/%s", dir, CRASH_LOG_FNAME);
			f = fopen(path, "wb");
		}
	}

	if (!f) {
		return NULL;
	}

	fprintf(f, "Crash!\n\n%s", msg);
	fclose(f);
	return path;
}
#endif

#if defined(PLATFORM_WIN32)

#include <windows.h>
#include <dbghelp.h>
#include <inttypes.h>
#include <excpt.h>

// Game builds with gcc, which emits DWARF debug info. Windows DbgHelp only
// understands PDB, so SymFromAddr returns nothing useful for our exe. As a
// fallback we shell out to addr2line (ships with MinGW binutils, available
// in any MSYS2 environment that built the game) to resolve module-relative
// offsets into function names + file:line via the DWARF data already
// embedded in the debug exe. Result is appended below the raw offset line
// so the existing dump format is preserved.

static LPTOP_LEVEL_EXCEPTION_FILTER prevExFilter;

static void *crashGetModuleBase(const void *addr)
{
	HMODULE h = NULL;
	GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, addr, &h);
	return (void *)h;
}

static const char *crashGetMainExePath(void)
{
	static char path[MAX_PATH] = "";
	static int tried = 0;
	if (!tried) {
		tried = 1;
		const DWORD n = GetModuleFileNameA(NULL, path, sizeof(path));
		if (n == 0 || n >= sizeof(path)) {
			path[0] = '\0';
		}
	}
	return path[0] ? path : NULL;
}

// On Windows, addr2line wants addresses keyed to the binary's *preferred*
// image base (from the PE header), not the runtime load address. With ASLR
// the runtime base differs every launch. Read the linker-recorded image
// base from the in-memory PE header so we can convert a module offset into
// the address addr2line actually expects.
static ULONGLONG crashGetPreferredImageBase(void)
{
	static ULONGLONG cached = 0;
	static int tried = 0;
	if (tried) {
		return cached;
	}
	tried = 1;
	HMODULE h = GetModuleHandleA(NULL);
	if (!h) {
		return 0;
	}
	PIMAGE_DOS_HEADER dos = (PIMAGE_DOS_HEADER)h;
	if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
		return 0;
	}
	PIMAGE_NT_HEADERS nt = (PIMAGE_NT_HEADERS)((BYTE *)h + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE) {
		return 0;
	}
	cached = nt->OptionalHeader.ImageBase;
	return cached;
}

// Find the addr2line binary. Looked up in this order:
//   1. Next to the exe (so tester builds can ship addr2line.exe alongside pd
//      and get symbolised crashes without an MSYS2 install).
//   2. PATH (devs running from an MSYS2 MinGW64 shell).
//   3. Common MSYS2 MinGW64 install locations (devs whose PATH doesn't carry
//      MSYS2 — _popen launches cmd.exe which doesn't inherit the shell PATH).
static const char *crashFindAddr2Line(void)
{
	static char path[MAX_PATH] = "";
	static int tried = 0;
	if (tried) {
		return path[0] ? path : NULL;
	}
	tried = 1;

	// 1. Next to the exe.
	const char *exe = crashGetMainExePath();
	if (exe) {
		char local[MAX_PATH];
		strncpy(local, exe, sizeof(local) - 1);
		local[sizeof(local) - 1] = '\0';
		char *slash = strrchr(local, '\\');
		if (!slash) slash = strrchr(local, '/');
		if (slash) {
			// Truncate to directory + append "addr2line.exe".
			const size_t dirlen = (size_t)(slash - local) + 1;
			if (dirlen + sizeof("addr2line.exe") <= sizeof(local)) {
				strcpy(local + dirlen, "addr2line.exe");
				if (GetFileAttributesA(local) != INVALID_FILE_ATTRIBUTES) {
					strncpy(path, local, sizeof(path) - 1);
					path[sizeof(path) - 1] = '\0';
					return path;
				}
			}
		}
	}

	// 2. PATH.
	if (SearchPathA(NULL, "addr2line.exe", NULL, sizeof(path), path, NULL)) {
		return path;
	}

	// 3. Common MSYS2 MinGW64 install locations.
	static const char *candidates[] = {
		"C:\\msys64\\mingw64\\bin\\addr2line.exe",
		"C:\\msys2\\mingw64\\bin\\addr2line.exe",
		"C:\\tools\\msys64\\mingw64\\bin\\addr2line.exe",
	};
	for (int i = 0; i < (int)(sizeof(candidates) / sizeof(candidates[0])); ++i) {
		if (GetFileAttributesA(candidates[i]) != INVALID_FILE_ATTRIBUTES) {
			strncpy(path, candidates[i], sizeof(path) - 1);
			return path;
		}
	}

	path[0] = '\0';
	return NULL;
}

// Try to resolve a main-exe module offset using addr2line. Appends one or
// more "      function at file:line" lines (one per inline expansion level)
// into the crash message buffer. Silently no-ops if addr2line isn't on PATH
// or if the offset can't be resolved (no debug info compiled in).
static void crashAppendDwarf(char *msg, DWORD *msglenp, uintptr_t modofs)
{
	const char *exe = crashGetMainExePath();
	const char *a2l = crashFindAddr2Line();
	if (!exe || !a2l) {
		return;
	}

	// addr2line wants linker-base + offset, not the bare module offset.
	const ULONGLONG imageBase = crashGetPreferredImageBase();
	const unsigned long long dwarfAddr = (unsigned long long)imageBase + (unsigned long long)modofs;

	// -f function names, -p one-line pretty print, -i include inline chain.
	// Run addr2line directly via CreateProcess + an anonymous pipe. _popen
	// CANNOT be used here: it spawns cmd.exe, which needs a console — and the
	// client has been a GUI-subsystem app since the SDL3 migration
	// (WIN32_EXECUTABLE), so _popen fails silently and every crash dump from
	// the windowed build regressed to raw offsets while the exact same
	// command line worked from any shell. Going direct also removes the
	// cmd.exe quote-stripping rules entirely — CreateProcess parses the two
	// quoted paths (which may contain spaces) sanely.
	char cmd[1024];
	snprintf(cmd, sizeof(cmd), "\"%s\" -e \"%s\" -f -p -i 0x%llx", a2l, exe, dwarfAddr);

	SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
	HANDLE rd = NULL;
	HANDLE wr = NULL;
	if (!CreatePipe(&rd, &wr, &sa, 0)) {
		return;
	}
	SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

	STARTUPINFOA si;
	memset(&si, 0, sizeof(si));
	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESTDHANDLES;
	si.hStdOutput = wr;
	si.hStdError = wr; // tool errors get the "??"/name filter below

	PROCESS_INFORMATION pi;
	memset(&pi, 0, sizeof(pi));

	const BOOL ok = CreateProcessA(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
	// Close OUR write end now: the child holds its own copy, and keeping
	// ours open would stop ReadFile from ever seeing EOF.
	CloseHandle(wr);
	if (!ok) {
		CloseHandle(rd);
		return;
	}

	char out[2048];
	DWORD total = 0;
	for (;;) {
		DWORD got = 0;
		if (!ReadFile(rd, out + total, (DWORD)(sizeof(out) - 1 - total), &got, NULL) || got == 0) {
			break;
		}
		total += got;
		if (total >= sizeof(out) - 1) {
			break;
		}
	}
	out[total] = '\0';
	CloseHandle(rd);
	WaitForSingleObject(pi.hProcess, 5000);
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);

	char *line = out;
	while (line && *line) {
		char *nl = strchr(line, '\n');
		if (nl) {
			*nl = '\0';
		}
		size_t len = strlen(line);
		while (len > 0 && line[len - 1] == '\r') {
			line[--len] = '\0';
		}
		// addr2line emits "?? at ??:0" for unresolvable addresses, and its
		// own error messages start with the tool name — skip both.
		if (len > 0 && strncmp(line, "??", 2) != 0 && strncmp(line, "addr2line", 9) != 0) {
			DWORD msglen = *msglenp;
			if (msglen < CRASH_MAX_MSG) {
				msglen += snprintf(msg + msglen, CRASH_MAX_MSG - msglen, "      %s\n", line);
				*msglenp = msglen;
			}
		}
		line = nl ? nl + 1 : NULL;
	}
}

static void crashStackTrace(char *msg, PEXCEPTION_POINTERS exinfo)
{
	CONTEXT context = *exinfo->ContextRecord;
	HANDLE process = GetCurrentProcess();
	HANDLE thread = GetCurrentThread();

	SymSetOptions(SymGetOptions() | SYMOPT_DEBUG | SYMOPT_LOAD_LINES);
	SymInitialize(process, NULL, TRUE);

	STACKFRAME64 stackframe;
	memset(&stackframe, 0, sizeof(stackframe));

	DWORD image;
#ifdef PLATFORM_X86
	image = IMAGE_FILE_MACHINE_I386;
	stackframe.AddrPC.Offset = context.Eip;
	stackframe.AddrPC.Mode = AddrModeFlat;
	stackframe.AddrFrame.Offset = context.Ebp;
	stackframe.AddrFrame.Mode = AddrModeFlat;
	stackframe.AddrStack.Offset = context.Esp;
	stackframe.AddrStack.Mode = AddrModeFlat;
#elif defined(PLATFORM_X86_64)
	image = IMAGE_FILE_MACHINE_AMD64;
	stackframe.AddrPC.Offset = context.Rip;
	stackframe.AddrPC.Mode = AddrModeFlat;
	stackframe.AddrFrame.Offset = context.Rsp;
	stackframe.AddrFrame.Mode = AddrModeFlat;
	stackframe.AddrStack.Offset = context.Rsp;
	stackframe.AddrStack.Mode = AddrModeFlat;
#else
	snprintf(msg, CRASH_MAX_MSG, "no stack trace available on this arch\n");
	return;
#endif

	DWORD disp = 0;
	DWORD64 disp64 = 0;
	IMAGEHLP_LINE64 line;
	DWORD msglen = 0;

	CRASH_MSG("EXCEPTION: 0x%08lx\n", exinfo->ExceptionRecord->ExceptionCode);
	CRASH_MSG("PC: %p", exinfo->ExceptionRecord->ExceptionAddress);
	const BOOL pcHasLine = SymGetLineFromAddr64(process, (uintptr_t)exinfo->ExceptionRecord->ExceptionAddress, &disp, &line);
	if (pcHasLine) {
		CRASH_MSG(": %s:%lu+%lu", line.FileName, line.LineNumber, disp);
	}
	const void *pcModBase = crashGetModuleBase(exinfo->ExceptionRecord->ExceptionAddress);
	const void *mainModBase = crashGetModuleBase(crashInit);
	CRASH_MSG("\nMODULE: [%p]\n", pcModBase);
	CRASH_MSG("MAIN MODULE: [%p]\n", mainModBase);
	// If DbgHelp didn't give us a file:line and the PC is in our own exe, try
	// addr2line for the DWARF symbols MinGW emits.
	if (!pcHasLine && pcModBase == mainModBase && pcModBase) {
		const uintptr_t pcOfs = (uintptr_t)exinfo->ExceptionRecord->ExceptionAddress - (uintptr_t)pcModBase;
		crashAppendDwarf(msg, &msglen, pcOfs);
	}

	// Say WHY a dump has raw offsets only, so a pasted crash screen is
	// diagnosable: without this, "no addr2line found" and "addr2line found
	// but resolution failed" look identical (both just print no symbol lines).
	if (!crashFindAddr2Line()) {
		CRASH_MSG("\n(addr2line.exe not found - raw offsets only; ship addr2line.exe next to the exe to symbolise)\n");
	}

	CRASH_MSG("\nBACKTRACE:\n");

	char symbuf[sizeof(SYMBOL_INFO) + CRASH_MAX_SYM * sizeof(TCHAR)];
	PSYMBOL_INFO sym = (PSYMBOL_INFO)symbuf;
	sym->SizeOfStruct = sizeof(*sym);
	sym->MaxNameLen = CRASH_MAX_SYM;

	s32 i;
	for (i = 0; i < CRASH_MAX_FRAMES; ++i) {
		const BOOL res = StackWalk64(image, process, thread, &stackframe, &context, NULL, SymFunctionTableAccess64, SymGetModuleBase64, NULL);
		if (!res) {
			break;
		}

		CRASH_MSG("#%02d: %p", i, (void *)(uintptr_t)stackframe.AddrPC.Offset);

		const BOOL hasName = SymFromAddr(process, stackframe.AddrPC.Offset, &disp64, sym);
		const uintptr_t frameModBase = (uintptr_t)crashGetModuleBase((void *)(uintptr_t)stackframe.AddrPC.Offset);
		const uintptr_t frameModOfs = (uintptr_t)stackframe.AddrPC.Offset - frameModBase;

		if (hasName) {
			CRASH_MSG(": %s+%llu", sym->Name, disp64);
		} else if (stackframe.AddrPC.Offset) {
			CRASH_MSG(": [%p]+%p", (void *)frameModBase, (void *)frameModOfs);
		}

		if (SymGetLineFromAddr64(process, stackframe.AddrPC.Offset, &disp, &line)) {
			CRASH_MSG(" (%s:%lu+%lu)", line.FileName, line.LineNumber, disp);
		}

		CRASH_MSG("\n");

		// DWARF fallback for frames inside our own exe — DbgHelp can't read
		// MinGW's debug info but addr2line can.
		if (!hasName && (void *)frameModBase == mainModBase && frameModBase) {
			crashAppendDwarf(msg, &msglen, frameModOfs);
		}
	}

	if (i <= 1) {
		CRASH_MSG("no information\n");
	} else if (i == CRASH_MAX_FRAMES) {
		CRASH_MSG("...\n");
	}

	SymCleanup(process);
}

static long __stdcall crashHandler(PEXCEPTION_POINTERS exinfo)
{
	char msg[CRASH_MAX_MSG + 1] = { 0 };

	if (IsDebuggerPresent()) {
		if (prevExFilter) {
			return prevExFilter(exinfo);
		}
		return EXCEPTION_CONTINUE_EXECUTION;
	}

	sysLogPrintf(LOG_ERROR, "FATAL: Crashed: PC=%p CODE=0x%08lx", exinfo->ExceptionRecord->ExceptionAddress, exinfo->ExceptionRecord->ExceptionCode);

	fflush(stderr);
	fflush(stdout);

	crashStackTrace(msg, exinfo);

	// Write the standalone crash artifact (exe dir -> cwd -> home) and note
	// in the open log, if any, where it went.
	{
		const char *logged = crashWriteLogFile(msg);
		if (logged) {
			sysLogPrintf(LOG_ERROR, "FATAL: crash dump written to %s", logged);
		}
	}

	sysFatalError("Crash!\n\n%s", msg);

	return EXCEPTION_CONTINUE_EXECUTION;
}

#elif defined(PLATFORM_LINUX)

#include <ucontext.h>
#include <signal.h>
#include <execinfo.h>
#include <unistd.h>
#include <ctype.h>
#include <dlfcn.h>
#include <sys/fcntl.h>

static struct sigaction prevSigAction;

static s32 crashIsDebuggerPresent(void)
{
	static s32 result = -1;

	if (result >= 0) {
		return result;
	}

	char buf[4096] = { 0 };

	int fd = open("/proc/self/status", O_RDONLY);
	if (fd < 0) {
		result = 0;
		return 0;
	}

	int rx = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (rx <= 0) {
		result = 0;
		return 0;
	}

	buf[rx] = 0;

	char *str = strstr(buf, "TracerPid:");
	if (!str) {
		result = 0;
		return 0;
	}

	str += 10;

	while (*str && !isdigit(*str)) {
		if (*str == '\n') {
			result = 0;
			return 0;
		}
		++str;
	}

	result = (atoi(str) != 0);
	return result;
}

static void *crashGetModuleBase(const void *addr)
{
	Dl_info info;
	if (dladdr(addr, &info)) {
		return info.dli_fbase;
	}
	return NULL;
}

static void crashStackTrace(char *msg, s32 sig, void *pc)
{
	u32 msglen = 0;
	void *frames[CRASH_MAX_FRAMES] = { NULL };

	const s32 nframes = backtrace(frames, CRASH_MAX_FRAMES);
	if (nframes <= 0) {
		CRASH_MSG("no information\n");
		return;
	}

	char **strings = backtrace_symbols(frames, nframes);

	CRASH_MSG("SIGNAL: %d\n", sig);
	CRASH_MSG("PC: ");
	if (pc) {
		CRASH_MSG("%p\n", pc);
	} else if (strings) {
		CRASH_MSG("%s\n", strings[0]);
	} else {
		CRASH_MSG("%p\n", frames[0]);
	}

	CRASH_MSG("MODULE: %p\n", crashGetModuleBase(frames[0]));
	CRASH_MSG("MAIN MODULE: %p\n", crashGetModuleBase(crashInit));
	CRASH_MSG("\nBACKTRACE:\n");

	s32 i;
	for (i = 0; i < nframes; ++i) {
		CRASH_MSG("#%02d: ", i);
		if (strings && strings[i]) {
			CRASH_MSG("%s\n", strings[i]);
		} else {
			CRASH_MSG("%p\n", frames[i]);
		}
	}

	if (i == CRASH_MAX_FRAMES) {
		CRASH_MSG("...\n");
	} else if (i <= 1) {
		CRASH_MSG("no information\n");
	}

	free(strings);
}

static void crashHandler(s32 sig, siginfo_t *siginfo, void *ctx)
{
	char msg[CRASH_MAX_MSG + 1] = { 0 };

	if (crashIsDebuggerPresent()) {
		return;
	}

	void *pc = NULL;
	if (ctx) {
		ucontext_t *ucontext = (ucontext_t *)ctx;
#ifdef PLATFORM_X86
		pc = (void *)ucontext->uc_mcontext.gregs[REG_EIP];
#elif defined(PLATFORM_X86_64)
		pc = (void *)ucontext->uc_mcontext.gregs[REG_RIP];
#elif defined(PLATFORM_ARM) && defined(PLATFORM_64BIT)
		pc = (void *)ucontext->uc_mcontext.pc;
#elif defined(PLATFORM_ARM)
		pc = (void *)ucontext->uc_mcontext.arm_pc;
#endif
	}

	sysLogPrintf(LOG_ERROR, "FATAL: Crashed: PC=%p SIGNAL=%d", pc, sig);

	fflush(stderr);
	fflush(stdout);

	crashStackTrace(msg, sig, pc);

	// Linux/dedicated previously wrote no crash file at all — only the log
	// line + fatal-error path. Same artifact cascade as Windows now.
	{
		const char *logged = crashWriteLogFile(msg);
		if (logged) {
			sysLogPrintf(LOG_ERROR, "FATAL: crash dump written to %s", logged);
		}
	}

	sysFatalError("Crash!\n\n%s", msg);
}

#endif

s32 g_CrashEnabled = 0;

static char crashMsg[1024];

void crashInit(void)
{
#ifdef PLATFORM_WIN32
	SetErrorMode(SEM_FAILCRITICALERRORS);
	prevExFilter = SetUnhandledExceptionFilter(crashHandler);
	g_CrashEnabled = 1;
#elif defined(PLATFORM_LINUX)
	struct sigaction sigact = { 0 };
	sigact.sa_flags = SA_SIGINFO | SA_ONSTACK;
	sigact.sa_sigaction = crashHandler;
	sigaction(SIGSEGV, &sigact, &prevSigAction);
	sigaction(SIGABRT, &sigact, &prevSigAction);
	sigaction(SIGBUS,  &sigact, &prevSigAction);
	sigaction(SIGILL,  &sigact, &prevSigAction);
	g_CrashEnabled = 1;
#endif
}

void crashShutdown(void)
{
	if (!g_CrashEnabled) {
		return;
	}
#ifdef PLATFORM_WIN32
	if (prevExFilter) {
		SetUnhandledExceptionFilter(prevExFilter);
	}
#elif defined(PLATFORM_LINUX)
	sigaction(SIGSEGV, &prevSigAction, NULL);
	sigaction(SIGABRT, &prevSigAction, NULL);
	sigaction(SIGBUS,  &prevSigAction, NULL);
	sigaction(SIGILL,  &prevSigAction, NULL);
#endif
	g_CrashEnabled = 0;
}

void crashCreateThread(void)
{

}

void crashSetMessage(char *string)
{

}

void crashReset(void)
{

}

void crashAppendChar(char c)
{

}
