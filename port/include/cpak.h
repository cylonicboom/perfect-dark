#ifndef _IN_PORT_CPAK_H
#define _IN_PORT_CPAK_H

#include <PR/ultratypes.h>

/*
 * Controller Pak feature: runtime toggles and orchestration.
 *
 * All toggles default OFF (fully opt-in) and are only meaningful when the
 * matching build option is enabled:
 *   - g_VirtualPakEnabled  : PD_ENABLE_CPAK    (live, file-backed virtual paks)
 *   - g_RaphnetEnabled     : PD_ENABLE_RAPHNET (physical pak backup via adapter)
 *   - g_RaphnetAutoBackup  : PD_ENABLE_RAPHNET (extra defensive backups)
 */

#ifdef PD_ENABLE_CPAK

extern s32 g_VirtualPakEnabled;

#ifdef PD_ENABLE_RAPHNET
extern s32 g_RaphnetEnabled;
extern s32 g_RaphnetAutoBackup;
extern s32 g_RaphnetBootBackup;

/* Status codes returned by the physical-pak operations below. */
typedef enum {
	CPAK_OK = 0,
	CPAK_ERR_DISABLED,   /* the Raphnet toggle is off */
	CPAK_ERR_NODEVICE,   /* no adapter / pak found */
	CPAK_ERR_IO,         /* transfer or file error */
} CpakResult;

/* Read the physical Controller Pak and dump it to $S/mempak_<timestamp>.mpk. */
CpakResult cpakPhysicalBackup(void);

/*
 * Read the physical Controller Pak via the adapter and install it as the live
 * virtual pak for `channel` ($S/cpak<N>.mpk), converting from the pak's native
 * big-endian format. Read-only with respect to the physical cartridge.
 */
CpakResult cpakImportPhysical(s32 channel);

/* Back up the current physical pak, then write `path`'s 32KB image to it. */
CpakResult cpakPhysicalRestore(const char *path);

/* Human-readable message for a CpakResult (for menu feedback). */
const char *cpakResultText(CpakResult r);
#endif /* PD_ENABLE_RAPHNET */

#endif /* PD_ENABLE_CPAK */

#endif
