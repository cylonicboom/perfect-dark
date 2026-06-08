#ifdef PD_ENABLE_CPAK

#include <stdio.h>
#include <string.h>
#include <PR/os_internal.h>
#include <PR/rcp.h>
#include "controller.h"
#include "types.h"
#include "constants.h"
#include "fs.h"
#include "system.h"
#include "mempak.h"

/* from src/game/pak.c */
void pakCalculateChecksum(u8 *start, u8 *end, u16 *checksum);

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
 * Validate the pack-ID checksum at block 1, interpreting the 14 leading u16s as
 * either little-endian (our native virtual-pak format) or big-endian (a real N64
 * pak, e.g. a DexDrive dump). Used to detect which byte order an image is in.
 */
static int mempakIdChecksumOK(const u8 *id, int bigendian)
{
	u32 sum = 0;
	for (s32 i = 0; i < 28; i += 2) {
		sum += bigendian ? ((id[i] << 8) | id[i + 1]) : (id[i] | (id[i + 1] << 8));
	}
	u16 stored = bigendian ? ((id[28] << 8) | id[29]) : (id[28] | (id[29] << 8));
	return (u16)sum == stored;
}

/* Defined below: rebuild a big-endian real-hardware pak as a native LE image. */
static int mempakImportBigEndian(s32 channel);

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

			if (mempakIdChecksumOK(g_MempakBuf[channel] + BLOCKSIZE, 0)) {
				// native little-endian virtual pak: use directly
				return 0;
			}
			if (mempakIdChecksumOK(g_MempakBuf[channel] + BLOCKSIZE, 1)
					&& mempakImportBigEndian(channel)) {
				// real-hardware (big-endian) pak: converted to native format
				sysLogPrintf(LOG_NOTE, "mempak: imported big-endian pak `%s` to native format", path);
				mempakFlush(channel);
				return 0;
			}

			sysLogPrintf(LOG_WARNING, "mempak: `%s` is not a valid pak image, reformatting", path);
		} else {
			sysLogPrintf(LOG_WARNING, "mempak: `%s` (%u bytes) is not a recognised pak image, reformatting",
					path, (u32)n);
		}
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

/* ---- big-endian (real N64 / DexDrive) pak import ----
 *
 * Real Controller Paks store all multi-byte values big-endian, whereas this
 * native (PC) build is little-endian. Perfect Dark's note *bodies* are written
 * with a byte-oriented, MSB-first bit packer (savebuffer), so they are
 * endian-independent; only the PFS metadata and the fixed 16-byte file headers
 * differ. We therefore rebuild the pak natively: parse the big-endian
 * directory/inode chains to recover each note's bytes, convert Perfect Dark's
 * file headers, then re-create every note through the (little-endian) engine so
 * all metadata and checksums are regenerated correctly. Other games' notes are
 * preserved byte-for-byte (their bodies are copied verbatim).
 *
 * EXPERIMENTAL: validated by reasoning but not yet against real hardware.
 */

static u16 mempakRdBe16(const u8 *p) { return (p[0] << 8) | p[1]; }
static u32 mempakRdBe32(const u8 *p) { return ((u32)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }

/* Convert Perfect Dark's note body file headers from big-endian to native, in place. */
static void mempakConvertPdNoteHeaders(u8 *body, int len)
{
	int off = 0;

	while (off + (int)sizeof(struct pakfileheader) <= len) {
		u8 *h = body + off;
		u32 w1 = mempakRdBe32(h + 8);   // filetype:9, bodylen:11, filelen:12
		u32 w2 = mempakRdBe32(h + 12);  // deviceserial:13, fileid:7, generation:9, occupied:1, writecompleted:1, version:1
		u32 bodylen = (w1 >> 12) & 0x7ff;
		u32 filelen = w1 & 0xfff;
		u32 filetype = (w1 >> 23) & 0x1ff;
		struct pakfileheader *hdr = (struct pakfileheader *)h;

		if (filelen == 0) {
			break;
		}

		// repack the two bitfield words in native (little-endian) order
		hdr->filetype = filetype;
		hdr->bodylen = bodylen;
		hdr->filelen = filelen;
		hdr->deviceserial = (w2 >> 19) & 0x1fff;
		hdr->fileid = (w2 >> 12) & 0x7f;
		hdr->generation = (w2 >> 3) & 0x1ff;
		hdr->occupied = (w2 >> 2) & 1;
		hdr->writecompleted = (w2 >> 1) & 1;
		hdr->version = w2 & 1;

		// recompute checksums over the now-native bytes (body bytes are unchanged)
		if (off + 16 + (int)bodylen <= len) {
			pakCalculateChecksum(h + 16, h + 16 + bodylen, hdr->bodysum);
		}
		pakCalculateChecksum(h + 8, h + 16, hdr->headersum);

		if (filetype & PAKFILETYPE_TERMINATOR) {
			break;
		}

		off += filelen;
	}
}

static int mempakImportBigEndian(s32 channel)
{
	static u8 src[MEMPAK_SIZE];
	static u8 notebody[MEMPAK_SIZE];
	const u8 *dir;
	const u8 *inode;
	OSPfs pfs;
	s32 e;

	memcpy(src, g_MempakBuf[channel], MEMPAK_SIZE);

	// only standard single-bank 32KB paks are supported (banks at id+0x1a)
	if (src[BLOCKSIZE + 0x1a] != 1) {
		return 0;
	}

	dir = src + 24 * BLOCKSIZE;   // dir_table  = page 3
	inode = src + 8 * BLOCKSIZE;  // inode_table = page 1

	// start from a fresh native pak; notes are re-created through the engine
	mempakFormatBlank(channel);
	if (mempakInitPak(NULL, &pfs, channel, NULL) != 0) {
		return 0;
	}

	for (e = 0; e < 16; e++) {
		const u8 *d = dir + e * 32;
		u32 game = mempakRdBe32(d + 0);
		u16 company = mempakRdBe16(d + 4);
		s32 page = mempakRdBe16(d + 6) & 0xff;
		s32 notelen = 0;
		s32 guard = 0;
		s32 fileno = -1;
		u8 name[PFS_FILE_NAME_LEN];
		u8 ext[PFS_FILE_EXT_LEN];

		if (game == 0 || company == 0) {
			continue;
		}

		memcpy(ext, d + 0xc, PFS_FILE_EXT_LEN);
		memcpy(name, d + 0x10, PFS_FILE_NAME_LEN);

		// follow the inode page chain to gather the note's pages in order
		while (page >= 5 && page < MEMPAK_NUM_PAGES && guard < MEMPAK_NUM_PAGES
				&& notelen + 256 <= (s32)sizeof(notebody)) {
			u16 next = mempakRdBe16(inode + page * 2);
			memcpy(notebody + notelen, src + page * 256, 256);
			notelen += 256;
			if (next == 1) {            // PFS_PAGE_LAST
				break;
			}
			if (next == 3) {            // free page - chain is broken
				break;
			}
			page = next & 0xff;
			guard++;
		}

		if (notelen == 0) {
			continue;
		}

		// Perfect Dark's own note needs its file headers converted to native order
		if (game == (u32)ROM_GAMECODE && company == (u16)ROM_COMPANYCODE) {
			mempakConvertPdNoteHeaders(notebody, notelen);
		}

		if (osPfsAllocateFile(&pfs, company, game, name, ext, notelen, &fileno) == 0 && fileno >= 0) {
			osPfsReadWriteFile(&pfs, fileno, PFS_WRITE, 0, notelen, notebody);
		} else {
			sysLogPrintf(LOG_WARNING, "mempak: could not import note %d during big-endian conversion", e);
		}
	}

	g_MempakDirty[channel] = 1;
	return 1;
}

#endif /* PD_ENABLE_CPAK */
