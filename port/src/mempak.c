#ifdef PD_ENABLE_CPAK

#include <stdio.h>
#include <string.h>
#include <PR/os_internal.h>
#include <PR/rcp.h>
#include "controller.h"
#include "fs.h"
#include "system.h"
#include "mempak.h"

/*
 * RAM-backed implementation of the four Controller Pak hardware primitives the
 * decomp PFS engine depends on. See mempak.h for the overall design.
 *
 * Only standard single-bank 32KB Controller Paks are supported (which is every
 * real N64 Controller Pak); multi-bank third-party paks are not.
 */

#define MEMPAK_PAGE_FREE 3 /* inode_page[].ipage marker for a free page */

static u8   g_MempakBuf[MAXCONTROLLERS][MEMPAK_SIZE];
static u8   g_MempakPresent[MAXCONTROLLERS];
static u8   g_MempakDirty[MAXCONTROLLERS];
static char g_MempakPath[MAXCONTROLLERS][FS_MAXPATH + 1];

static int mempakValidChannel(int channel)
{
	return channel >= 0 && channel < MAXCONTROLLERS && g_MempakPresent[channel];
}

static void mempakFlush(s32 channel)
{
	g_MempakDirty[channel] = 0;

	if (!g_MempakPath[channel][0]) {
		return;
	}

	FILE *fp = fsFileOpenWrite(g_MempakPath[channel]);
	if (fp) {
		fwrite(g_MempakBuf[channel], 1, MEMPAK_SIZE, fp);
		fsFileFree(fp);
	} else {
		sysLogPrintf(LOG_ERROR, "mempak: could not write `%s`", fsFullPath(g_MempakPath[channel]));
	}
}

void mempakFlushAll(void)
{
	for (s32 i = 0; i < MAXCONTROLLERS; ++i) {
		if (g_MempakPresent[i] && g_MempakDirty[i]) {
			mempakFlush(i);
		}
	}
}

s32 mempakIsPresent(s32 channel)
{
	return mempakValidChannel(channel);
}

/* ---- the four primitives the PFS engine calls ---- */

s32 __osContRamRead(OSMesgQueue *mq, int channel, u16 address, u8 *buffer)
{
	if (!mempakValidChannel(channel)) {
		return PFS_ERR_NOPACK;
	}
	if ((u32)address * BLOCKSIZE + BLOCKSIZE > MEMPAK_SIZE) {
		return PFS_ERR_INVALID;
	}
	memcpy(buffer, g_MempakBuf[channel] + (u32)address * BLOCKSIZE, BLOCKSIZE);
	return 0;
}

s32 __osContRamWrite(OSMesgQueue *mq, int channel, u16 address, u8 *buffer, int force)
{
	if (!mempakValidChannel(channel)) {
		return PFS_ERR_NOPACK;
	}
	if ((u32)address * BLOCKSIZE + BLOCKSIZE > MEMPAK_SIZE) {
		return PFS_ERR_INVALID;
	}
	memcpy(g_MempakBuf[channel] + (u32)address * BLOCKSIZE, buffer, BLOCKSIZE);
	g_MempakDirty[channel] = 1;
	return 0;
}

s32 __osPfsGetStatus(OSMesgQueue *queue, int channel)
{
	if (!mempakValidChannel(channel)) {
		return PFS_ERR_NOPACK;
	}
	/*
	 * The engine calls this at the end of every write operation. Coalesce the
	 * many 32-byte block writes of a single save into one whole-image flush by
	 * persisting here only when something actually changed. Reads never set the
	 * dirty flag, so they never trigger disk writes.
	 */
	if (g_MempakDirty[channel]) {
		mempakFlush(channel);
	}
	return 0;
}

s32 __osPfsSelectBank(OSPfs *pfs, u8 bank)
{
	/* Standard paks are a single 32KB bank; just track the requested bank. */
	if (pfs) {
		pfs->activebank = bank;
	}
	return 0;
}

/* ---- formatting / loading ---- */

/*
 * Build a valid empty Controller Pak image in g_MempakBuf[channel]:
 *  - page 0  : the pack ID (replicated at blocks 1,3,4,6) + label (block 7)
 *  - page 1/2: the inode table and its mirror, with every data page free
 *  - page 3/4: the directory (16 entries), all zero == empty
 */
static void mempakFormatBlank(s32 channel)
{
	OSPfs pfs;
	__OSPackId id;
	__OSInode inode;
	u8 *buf = g_MempakBuf[channel];
	s32 i;

	memset(buf, 0, MEMPAK_SIZE);
	g_MempakPresent[channel] = 1;

	/* A minimal but valid pack ID for a single-bank 32KB pak. */
	memset(&id, 0, sizeof(id));
	id.random = (u32)osGetCount();
	id.serial_mid = ((u64)osGetCount() << 16) ^ 0x5044000000ULL;
	id.serial_low = ((u64)osGetCount() << 1) ^ 0x4452ULL;
	id.deviceid = 1;                /* bit 0 set == valid device */
	id.banks = 1;
	id.version = OS_PFS_VERSION_LO;
	__osIdCheckSum((u16 *)&id, &id.checksum, &id.inverted_checksum);

	memcpy(buf + 1 * BLOCKSIZE, &id, sizeof(id));
	memcpy(buf + 3 * BLOCKSIZE, &id, sizeof(id));
	memcpy(buf + 4 * BLOCKSIZE, &id, sizeof(id));
	memcpy(buf + 6 * BLOCKSIZE, &id, sizeof(id));
	/* block 7 (the label) is left zeroed */

	/* Lay out an empty inode table and let the engine write it (and the mirror)
	 * with the correct page checksum. */
	memset(&pfs, 0, sizeof(pfs));
	pfs.channel = channel;
	pfs.activebank = 0;
	pfs.banks = 1;
	pfs.inode_start_page = 1 * 2 + 3;                /* 5 */
	pfs.inode_table = 8;
	pfs.minode_table = 1 * PFS_ONE_PAGE + 8;         /* 16 */
	pfs.dir_table = pfs.minode_table + 1 * PFS_ONE_PAGE; /* 24 */
	pfs.dir_size = 16;

	memset(&inode, 0, sizeof(inode));
	for (i = pfs.inode_start_page; i < MEMPAK_NUM_PAGES; ++i) {
		inode.inode_page[i].ipage = MEMPAK_PAGE_FREE;
	}
	__osPfsRWInode(&pfs, &inode, PFS_WRITE, 0);

	/* directory pages are already zero == 16 empty entries */
}

/*
 * DexDrive ".n64" container: a 0x1040-byte header (starting with the ASCII
 * magic "123-456-STD") followed by the raw 32KB pak image. This is what tools
 * like pj64raphnetraw export.
 */
#define MEMPAK_DEXDRIVE_SIZE   (0x1040 + MEMPAK_SIZE)
#define MEMPAK_DEXDRIVE_HDRLEN 0x1040
static const char MEMPAK_DEXDRIVE_MAGIC[] = "123-456-STD";

s32 mempakLoadFile(s32 channel, const char *path)
{
	if (channel < 0 || channel >= MAXCONTROLLERS) {
		return PFS_ERR_NOPACK;
	}

	strncpy(g_MempakPath[channel], path, FS_MAXPATH);
	g_MempakPath[channel][FS_MAXPATH] = '\0';
	g_MempakDirty[channel] = 0;

	FILE *fp = fsFileOpenRead(path);
	if (fp) {
		// Read the whole file up front so the container format can be detected.
		static u8 filebuf[MEMPAK_DEXDRIVE_SIZE];
		size_t n = fread(filebuf, 1, sizeof(filebuf), fp);
		fsFileFree(fp);

		const u8 *image = NULL;
		if (n == MEMPAK_SIZE) {
			// raw 32KB pak image (.mpk)
			image = filebuf;
		} else if (n == MEMPAK_DEXDRIVE_SIZE
				&& memcmp(filebuf, MEMPAK_DEXDRIVE_MAGIC, sizeof(MEMPAK_DEXDRIVE_MAGIC) - 1) == 0) {
			// DexDrive .n64 container: skip the header to reach the pak image
			image = filebuf + MEMPAK_DEXDRIVE_HDRLEN;
		}

		if (image) {
			memcpy(g_MempakBuf[channel], image, MEMPAK_SIZE);
			g_MempakPresent[channel] = 1;
			return 0;
		}

		sysLogPrintf(LOG_WARNING, "mempak: `%s` (%u bytes) is not a recognised pak image, reformatting",
				path, (u32)n);
	}

	/* No file (or an unrecognised one): create and persist a fresh blank pak. */
	mempakFormatBlank(channel);
	mempakFlush(channel);
	return 0;
}

s32 mempakInitPak(OSMesgQueue *queue, OSPfs *pfs, s32 channel, s32 *arg3)
{
	s32 ret;

	if (!mempakValidChannel(channel)) {
		return PFS_ERR_NOPACK;
	}

	pfs->queue = queue;
	pfs->channel = channel;
	pfs->status = 0;
	pfs->activebank = 0;

	/* __osGetId reads/validates the pack ID and fills in all of the pfs
	 * geometry fields (banks, inode_table, minode_table, dir_table, ...). */
	ret = __osGetId(pfs);
	if (ret != 0) {
		return ret;
	}

	pfs->status |= PFS_INITIALIZED;

	if (arg3 != NULL) {
		*arg3 = 0;
	}

	return 0;
}

#endif /* PD_ENABLE_CPAK */
