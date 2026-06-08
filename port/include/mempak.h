#ifndef _IN_PORT_MEMPAK_H
#define _IN_PORT_MEMPAK_H

#include <PR/ultratypes.h>
#include <PR/os_internal.h>

/**
 * Host-side, RAM-backed N64 Controller Pak (mempak) support.
 *
 * The decomp Controller Pak filesystem engine (src/lib/ultra/io/pfs*.c,
 * contpfs.c) reaches the physical pak through exactly four primitives:
 * __osContRamRead/__osContRamWrite/__osPfsGetStatus/__osPfsSelectBank. This
 * module reimplements those four primitives over an in-memory 32KB image per
 * controller channel, so the unmodified engine can read/write a "Virtual Pak"
 * that is persisted to a .mpk file on disk.
 *
 * Everything here is only compiled when PD_ENABLE_CPAK is defined.
 */

#define MEMPAK_SIZE      32768          /* one standard Controller Pak: 128 pages * 256 bytes */
#define MEMPAK_NUM_PAGES 128

/**
 * Bind channel `channel` to the .mpk file at `path` (a "$S/..."-style path is
 * accepted and expanded by the fs layer). If the file does not exist or is the
 * wrong size, a freshly formatted blank pak is created and written to disk.
 * After this call the channel is "present" and the four primitives operate on
 * its buffer. Returns 0 on success.
 */
s32 mempakLoadFile(s32 channel, const char *path);

/**
 * Initialise an OSPfs for `channel` against its in-memory buffer. Mirrors
 * osPfsInitPak() but reads from RAM and deliberately skips osPfsChecker (the
 * checker is a physical-corruption repair pass whose heuristics are not needed
 * for our self-consistent in-memory image). The channel must already have been
 * bound via mempakLoadFile(). Returns 0 on success, or a PFS_ERR_* code.
 */
s32 mempakInitPak(OSMesgQueue *queue, OSPfs *pfs, s32 channel, s32 *arg3);

/** True if `channel` currently has a virtual pak bound. */
s32 mempakIsPresent(s32 channel);

/** Flush any dirty buffers to their .mpk files (e.g. on shutdown). */
void mempakFlushAll(void);

#endif
