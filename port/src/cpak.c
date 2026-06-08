#ifdef PD_ENABLE_CPAK

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <PR/ultratypes.h>
#include "platform.h"
#include "config.h"
#include "fs.h"
#include "system.h"
#include "mempak.h"
#include "cpak.h"
#ifdef PD_ENABLE_RAPHNET
#include "raphnet.h"
#endif

/* Runtime toggles; all default OFF (fully opt-in). */
s32 g_VirtualPakEnabled = 0;
#ifdef PD_ENABLE_RAPHNET
s32 g_RaphnetEnabled = 0;
s32 g_RaphnetAutoBackup = 0;
#endif

PD_CONSTRUCTOR static void cpakConfigInit(void)
{
	configRegisterInt("ControllerPak.VirtualEnabled", &g_VirtualPakEnabled, 0, 1);
#ifdef PD_ENABLE_RAPHNET
	configRegisterInt("ControllerPak.RaphnetEnabled", &g_RaphnetEnabled, 0, 1);
	configRegisterInt("ControllerPak.RaphnetAutoBackup", &g_RaphnetAutoBackup, 0, 1);
#endif
}

#ifdef PD_ENABLE_RAPHNET

const char *cpakResultText(CpakResult r)
{
	switch (r) {
	case CPAK_OK:          return "Done";
	case CPAK_ERR_DISABLED: return "Enable Raphnet first";
	case CPAK_ERR_NODEVICE: return "No adapter or pak found";
	case CPAK_ERR_IO:       return "Transfer failed";
	}
	return "Error";
}

/* Build "$S/mempak_YYYYMMDD-HHMMSS.mpk" into `out`. */
static void cpakMakeBackupPath(char *out, size_t outlen)
{
	time_t now = time(NULL);
	struct tm *tm = localtime(&now);
	char stamp[32];
	if (tm) {
		strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", tm);
	} else {
		snprintf(stamp, sizeof(stamp), "unknown");
	}
	snprintf(out, outlen, "$S/mempak_%s.mpk", stamp);
}

static CpakResult cpakWriteImage(const char *path, const u8 *buf)
{
	FILE *fp = fsFileOpenWrite(path);
	if (!fp) {
		sysLogPrintf(LOG_ERROR, "cpak: could not open `%s` for writing", fsFullPath(path));
		return CPAK_ERR_IO;
	}
	size_t n = fwrite(buf, 1, MEMPAK_SIZE, fp);
	fsFileFree(fp);
	if (n != MEMPAK_SIZE) {
		return CPAK_ERR_IO;
	}
	sysLogPrintf(LOG_NOTE, "cpak: wrote `%s`", fsFullPath(path));
	return CPAK_OK;
}

CpakResult cpakPhysicalBackup(void)
{
	if (!g_RaphnetEnabled) {
		return CPAK_ERR_DISABLED;
	}

	raphnet_dev *dev = raphnetOpen();
	if (!dev) {
		return CPAK_ERR_NODEVICE;
	}

	u8 buf[MEMPAK_SIZE];
	CpakResult res = CPAK_OK;

	if (raphnetReadPak(dev, buf) != 0) {
		res = CPAK_ERR_IO;
	} else {
		char path[FS_MAXPATH + 1];
		cpakMakeBackupPath(path, sizeof(path));
		res = cpakWriteImage(path, buf);
	}

	raphnetClose(dev);
	return res;
}

CpakResult cpakPhysicalRestore(const char *path)
{
	if (!g_RaphnetEnabled) {
		return CPAK_ERR_DISABLED;
	}

	raphnet_dev *dev = raphnetOpen();
	if (!dev) {
		return CPAK_ERR_NODEVICE;
	}

	u8 cur[MEMPAK_SIZE];
	u8 img[MEMPAK_SIZE];
	CpakResult res = CPAK_OK;

	/* Mandatory safety backup of the pak's current contents before any write. */
	if (raphnetReadPak(dev, cur) != 0) {
		res = CPAK_ERR_IO;
	} else {
		char backup[FS_MAXPATH + 1];
		cpakMakeBackupPath(backup, sizeof(backup));
		cpakWriteImage(backup, cur);

		FILE *fp = fsFileOpenRead(path);
		if (!fp) {
			res = CPAK_ERR_IO;
		} else {
			size_t n = fread(img, 1, MEMPAK_SIZE, fp);
			fsFileFree(fp);
			if (n != MEMPAK_SIZE) {
				res = CPAK_ERR_IO;
			} else if (raphnetWritePak(dev, img) != 0) {
				res = CPAK_ERR_IO;
			}
		}
	}

	raphnetClose(dev);
	return res;
}

#endif /* PD_ENABLE_RAPHNET */

#endif /* PD_ENABLE_CPAK */
