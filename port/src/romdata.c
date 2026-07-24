#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h> // model-swap overlay ROM auto-load (scan scripts/chaos/rom)
#include <PR/ultratypes.h>
#include "lib/rzip.h"
#include "romdata.h"
#include "fs.h"
#include "system.h"
#include "preprocess.h"
#include "platform.h"
#include "video.h" // taskbar progress during boot preprocessing
#include "data.h" // g_Stages, for the chain ROM stage table import

/**
 * asset files and ROM segments can be replaced by optional external files,
 * but asset filenames still have to be either pulled from the ROM or from an
 * external file, so stuff can't be completely custom
 * 
 * all data is assumed to be big endian, so it has to be byteswapped
 * at load time, which is fucking terrible
 */

#define ROMDATA_FILEDIR "files"
#define ROMDATA_SEGDIR "segs"

#define ROMDATA_ROM_NAME "pd." VERSION_ROMID ".z64"
#define ROMDATA_ROM_SIZE 33554432

#if VERSION == VERSION_NTSC_FINAL
#define ROMDATA_ROM_TITLE "Perfect Dark"
#define ROMDATA_ROM_ID "NPDE"
#define ROMDATA_ROM_DESC "NTSC v1.1"
#define ROMDATA_FILES_OFS 0x28080
#define ROMDATA_DATA_OFS 0x39850
#elif VERSION == VERSION_PAL_FINAL
#define ROMDATA_ROM_TITLE "Perfect Dark"
#define ROMDATA_ROM_ID "NPDP"
#define ROMDATA_ROM_DESC "PAL"
#define ROMDATA_FILES_OFS 0x28910
#define ROMDATA_DATA_OFS 0x39850
#elif VERSION == VERSION_JPN_FINAL
#define ROMDATA_ROM_TITLE "PERFECT DARK"
#define ROMDATA_ROM_ID "NPDJ"
#define ROMDATA_ROM_DESC "JPN"
#define ROMDATA_FILES_OFS 0x28800
#define ROMDATA_DATA_OFS 0x39850
#else
#error "This ROM version is unsupported."
#endif

#define ROMDATA_MAX_FILES 2048

#define GBC_ROM_NAME "pd.gbc"
#define GBC_ROM_SIZE 4194304

u8 *g_RomFile;
u32 g_RomFileSize;
const char *g_RomName = ROMDATA_ROM_NAME;

// chain-loaded second ROM (--mod-rom): a whole pre-modded PD ROM whose file
// table and segments back the MOD_CHAINROM slot, while the engine code stays
// this build. g_ChainRomActive locks g_ModNum to MOD_CHAINROM for the session.
s32 g_ChainRomActive = 0;
static u8 *chainRomFile;
static u32 chainRomFileSize;
static u8 *chainDataSeg;
static u32 chainDataSegSize;

// Chaos live model swap: an OVERLAY ROM is a second full PD ROM whose file table
// is loaded into the MOD_CHAINROM slot WITHOUT switching the active slot (unlike
// --mod-rom, which locks g_ModNum to MOD_CHAINROM). While g_ModelSwapActive is
// set, character-model files flagged in g_ModelSwapFiles are sourced from that
// overlay by NAME (so a mod that reordered its file table still resolves). Model
// files embed their own textures (CT_TEXDATA), so geometry + textures swap
// together. See docs / port_net_predict changes.
s32 g_ModelRomActive = 0;                     // overlay ROM loaded, base slot still active
s32 g_ModelSwapActive = 0;                    // sourcing flagged models from the overlay now
u8 g_ModelSwapFiles[ROMDATA_MAX_FILES] = { 0 }; // per-file: 1 = redirect to the overlay when active

static u8 *romDataSeg;
static u32 romDataSegSize;

// base pointer/size the segment loader resolves segment offsets against; set in
// romdataInit to the base ROM, or the chain ROM when a --mod-rom is active
static u8 *segRomBase;
static u32 segRomBaseSize;

// file num currently being preprocessed; read by ext_tex.c to key embedded
// model textures by their owning file (rafccq/port-ext-textures)
s32 loadingFileNum;

enum loadsource {
	SRC_UNLOADED = 0,
	SRC_ROM,
	SRC_EXTERNAL
};

struct romfilepatch {
	u32 ofs;
	u32 len;
	const char *src;
	const char *dst;
};

struct romfile {
	u8 **segstart;
	u8 **segend;
	const char *name;
	u8 *data;
	u32 size;
	preprocessfunc preprocess;
	s32 source; // enum loadsource
	s32 preprocessed;
	const struct romfilepatch *patches;
	u32 numpatches;
};

/* patches for individual files; applied on file load, before preprocFuncs, but */
/* after unzip; only applied when loading from a ROM file                       */
static const struct romfilepatch filePatches[] = {
	/* FILE_USETUPLUE: fixes Jon's double "if what" in Infiltration outro */
	{ 0x92a2, 1, "\x6c", "\x99" },
	{ 0x92b0, 1, "\x6c", "\x99" },
};

static struct romfile fileSlots[MOD_COUNT][ROMDATA_MAX_FILES] = {
	{ [FILE_USETUPLUE] = { .patches = &filePatches[0], .numpatches = 2 } },
	{ [FILE_USETUPLUE] = { .patches = &filePatches[0], .numpatches = 2 } }, // GoldenEye X Mod
	{ [FILE_USETUPLUE] = { .patches = &filePatches[0], .numpatches = 2 } }, // Kakariko Village Mod
	{ [FILE_USETUPLUE] = { .patches = &filePatches[0], .numpatches = 2 } }, // Dark Moon Mod
	{ [FILE_USETUPLUE] = { .patches = &filePatches[0], .numpatches = 2 } }, // Goldfinger 64 Mod
};

#define ROMSEG_START(n) _ ## n ## SegmentRomStart
#define ROMSEG_END(n) _ ## n ## SegmentRomEnd

/* segment table for ntsc-final                                                     */
/* size will get calculated automatically if it is 0                                */
/* if there are replacement files in the data dir, they will be loaded instead      */
/* offsets are specified for ntsc-final, pal-final and jpn-final in that order      */
#define ROMSEG_LIST() \
	ROMSEG_DECL_SEG(fontjpnsingle,      0x194b20,  0x180330,  0x0,       0x0,      preprocessJpnFont       ) \
	ROMSEG_DECL_SEG(fontjpnmulti,       0x19fb40,  0x18b340,  0x0,       0x0,      preprocessJpnFont       ) \
	ROMSEG_DECL_SEG(animations,         0x1a15c0,  0x18cdc0,  0x190c50,  0x0,      preprocessAnimations    ) \
	ROMSEG_DECL_SEG(mpconfigs,          0x7d0a40,  0x7bc240,  0x7c00d0,  0x11e0,   preprocessMpConfigs     ) \
	ROMSEG_DECL_SEG(mpstringsE,         0x7d1c20,  0x7bd420,  0x7c12b0,  0x3700,   NULL                    ) \
	ROMSEG_DECL_SEG(mpstringsJ,         0x7d5320,  0x7c0b20,  0x7c49b0,  0x3700,   NULL                    ) \
	ROMSEG_DECL_SEG(mpstringsP,         0x7d8a20,  0x7c4220,  0x7c80b0,  0x3700,   NULL                    ) \
	ROMSEG_DECL_SEG(mpstringsG,         0x7dc120,  0x7c7920,  0x7cb7b0,  0x3700,   NULL                    ) \
	ROMSEG_DECL_SEG(mpstringsF,         0x7df820,  0x7cb020,  0x7ceeb0,  0x3700,   NULL                    ) \
	ROMSEG_DECL_SEG(mpstringsS,         0x7e2f20,  0x7ce720,  0x7d25b0,  0x3700,   NULL                    ) \
	ROMSEG_DECL_SEG(mpstringsI,         0x7e6620,  0x7d1e20,  0x7d5cb0,  0x3700,   NULL                    ) \
	ROMSEG_DECL_SEG(firingrange,        0x7e9d20,  0x7d5520,  0x7d93b0,  0x1550,   NULL                    ) \
	ROMSEG_DECL_SEG(fonttahoma,         0x7f7860,  0x7e3060,  0x7e6ef0,  0x0,      preprocessFont          ) \
	ROMSEG_DECL_SEG(fontnumeric,        0x7f8b20,  0x7e4320,  0x7e81b0,  0x0,      preprocessFont          ) \
	ROMSEG_DECL_SEG(fonthandelgothicsm, 0x7f9d30,  0x7e5530,  0x7e93c0,  0x0,      preprocessFont          ) \
	ROMSEG_DECL_SEG(fonthandelgothicxs, 0x7fbfb0,  0x7e87b0,  0x7ec640,  0x0,      preprocessFont          ) \
	ROMSEG_DECL_SEG(fonthandelgothicmd, 0x7fdd80,  0x7eae20,  0x7eecb0,  0x0,      preprocessFont          ) \
	ROMSEG_DECL_SEG(fonthandelgothiclg, 0x8008e0,  0x7eee70,  0x7f2d00,  0x0,      preprocessFont          ) \
	ROMSEG_DECL_SEG(sfxctl,             0x80a250,  0x7f87e0,  0x7fc670,  0x2fb80,  preprocessALBankFile    ) \
	ROMSEG_DECL_SEG(sfxtbl,             0x839dd0,  0x828360,  0x82c1f0,  0x4c2160, NULL                    ) \
	ROMSEG_DECL_SEG(seqctl,             0xcfbf30,  0xcea4c0,  0xcee350,  0xa060,   preprocessALBankFile    ) \
	ROMSEG_DECL_SEG(seqtbl,             0xd05f90,  0xcf4520,  0xcf83b0,  0x17c070, NULL                    ) \
	ROMSEG_DECL_SEG(sequences,          0xe82000,  0xe70590,  0xe74420,  0x563a0,  preprocessSequences     ) \
	ROMSEG_DECL_SEG(texturesdata,       0x1d65f40, 0x1d5ca20, 0x1d61f90, 0x0,      NULL                    ) \
	ROMSEG_DECL_SEG(textureslist,       0x1ff7ca0, 0x1fee780, 0x1ff68f0, 0x0,      preprocessTexturesList  ) \
	ROMSEG_DECL_SEG(copyright,          0x1ffea20, 0x1ff5500, 0x1ffd6b0, 0xb30,    NULL                    ) \
	ROMSEG_DECL_SEG(fontjpn,            0x0,       0x0,       0x178c40,  0x17920,  preprocessJpnFont       )

// declare the vars first

#undef ROMSEG_DECL_SEG
#define ROMSEG_DECL_SEG(name, ofs_ntsc, ofs_pal, ofs_jpn, size, preproc) u8 *ROMSEG_START(name), *ROMSEG_END(name);
ROMSEG_LIST()

// this is part of the animations seg and as such does not follow the naming convention
// these are set in preprocessAnimations
u8 *_animationsTableRomStart;
u8 *_animationsTableRomEnd;

// then build the table

#undef ROMSEG_DECL_SEG

#if VERSION == VERSION_NTSC_FINAL
#define ROMSEG_DECL_SEG(name, ofs_ntsc, ofs_pal, ofs_jpn, size, preproc) { &ROMSEG_START(name), &ROMSEG_END(name), #name, (u8 *)ofs_ntsc, size, preproc },
#elif VERSION == VERSION_PAL_FINAL
#define ROMSEG_DECL_SEG(name, ofs_ntsc, ofs_pal, ofs_jpn, size, preproc) { &ROMSEG_START(name), &ROMSEG_END(name), #name, (u8 *)ofs_pal, size, preproc },
#elif VERSION == VERSION_JPN_FINAL
#define ROMSEG_DECL_SEG(name, ofs_ntsc, ofs_pal, ofs_jpn, size, preproc) { &ROMSEG_START(name), &ROMSEG_END(name), #name, (u8 *)ofs_jpn, size, preproc },
#endif

static struct romfile romSegs[] = {
	ROMSEG_LIST()
	{ NULL, NULL, NULL, NULL, 0, NULL },
};

/* the game sets g_LoadType to the type of file it expects,              */
/* so we can hijack that in fileLoad and automatically byteswap the file */
static preprocessfunc filePreprocFuncs[] = {
	/* LOADTYPE_NONE  */ NULL,
	/* LOADTYPE_BG    */ NULL, // loaded in parts
	/* LOADTYPE_TILES */ preprocessTilesFile,
	/* LOADTYPE_LANG  */ preprocessLangFile,
	/* LOADTYPE_SETUP */ preprocessSetupFile,
	/* LOADTYPE_PADS  */ preprocessPadsFile,
	/* LOADTYPE_MODEL */ preprocessModelFile,
	/* LOADTYPE_GUN   */ preprocessGunFile,
};

static inline void romdataWrongRomError(const char *fmt, ...)
{
	char reason[1024];
	reason[0] = '\0';

	va_list args;
	va_start(args, fmt);
	vsnprintf(reason, sizeof(reason), fmt, args);
	va_end(args);

	sysFatalError("Wrong ROM file.\n%s\nEnsure that you have the correct " ROMDATA_ROM_DESC " ROM in z64 format.", reason);
}

// load and validate a PD ROM, inflating its compressed data segment. shared by
// the base ROM and the chain-loaded --mod-rom. requireHeader is true for the
// base ROM (the stock NPDE/"Perfect Dark" header must match); it is relaxed to
// a warning for the chain ROM, since a modded ROM / total conversion may carry
// a changed title or cart id while keeping the 32MB stock layout. the size and
// 1173-compression checks still apply to both (the loader assumes both).
// Returns true on success. When `fatal` is false (the boot-time model-swap
// overlay auto-load), any problem logs a warning, frees what it allocated, and
// returns false instead of aborting — a bad/incompatible overlay ROM must never
// brick boot. `fatal` true (base ROM, --mod-rom) keeps the hard-error behaviour.
static bool romdataLoadRomFile(const char *name, u8 **outRom, u32 *outSize, u8 **outSeg, u32 *outSegSize, bool requireHeader, bool fatal)
{
	sysLogPrintf(LOG_NOTE, "ROM file: %s", name);

	u32 romSize = 0;
	u8 *rom = fsFileLoad(name, &romSize);

	if (!rom) {
		if (fatal) {
			sysFatalError("Could not open ROM file %s.\nEnsure that it is in the %s directory.", name, fsFullPath(""));
		}
		sysLogPrintf(LOG_WARNING, "romdataLoadRomFile: could not open %s", name);
		return false;
	}

	// zips are not guaranteed to start with PK, but might as well at least try
	if (romSize > 2 && (!memcmp(rom, "PK", 2) || !memcmp(rom, "Rar", 3) || !memcmp(rom, "7z", 2))) {
		if (fatal) {
			romdataWrongRomError("Your ROM is in an archive file. Please extract it.");
		}
		sysLogPrintf(LOG_WARNING, "romdataLoadRomFile: %s is an archive, skipping", name);
		sysMemFree(rom);
		return false;
	}

	if (requireHeader) {
		// base ROM: must be exactly the stock size
		if (romSize != ROMDATA_ROM_SIZE) {
			romdataWrongRomError("ROM size does not match: expected: %u, got: %u.", ROMDATA_ROM_SIZE, romSize);
		}
	} else {
		// chain / model-swap overlay ROMs: total conversions commonly EXPAND the
		// ROM past the stock 32MB (appended model/texture data). Accept anything
		// at least stock-sized; reject only a truncated ROM. The data segment and
		// file table are still read at their stock offsets below, so a mod that
		// RELOCATED those will fail the 1173 check (skipped gracefully here).
		if (romSize < ROMDATA_ROM_SIZE) {
			if (fatal) {
				romdataWrongRomError("ROM too small: expected at least %u, got: %u.", ROMDATA_ROM_SIZE, romSize);
			}
			sysLogPrintf(LOG_WARNING, "romdataLoadRomFile: %s too small (%u), skipping", name, romSize);
			sysMemFree(rom);
			return false;
		} else if (romSize != ROMDATA_ROM_SIZE) {
			sysLogPrintf(LOG_WARNING, "chain/overlay ROM is %u bytes (stock %u) — expanded mod, loading anyway", romSize, ROMDATA_ROM_SIZE);
		}
	}

	if (memcmp(rom + 0x3b, ROMDATA_ROM_ID, 4) || memcmp(rom + 0x20, ROMDATA_ROM_TITLE, sizeof(ROMDATA_ROM_TITLE) - 1)) {
		if (requireHeader) {
			romdataWrongRomError("ROM header does not match.");
		} else {
			sysLogPrintf(LOG_WARNING, "chain ROM header does not match stock %s; loading anyway (mod/total conversion)", ROMDATA_ROM_DESC);
		}
	}

	// inflate the compressed data segment since that's where some useful stuff is

	u8 *zipped = rom + ROMDATA_DATA_OFS;
	if (!rzipIs1173(zipped)) {
		if (fatal) {
			romdataWrongRomError("Data segment is not 1173-compressed.");
		}
		sysLogPrintf(LOG_WARNING, "romdataLoadRomFile: %s data segment not 1173-compressed at 0x%x (relocated/incompatible layout) — skipping overlay", name, ROMDATA_DATA_OFS);
		sysMemFree(rom);
		return false;
	}

	const u32 dataSegLen = ((u32)zipped[2] << 16) | ((u32)zipped[3] << 8) | (u32)zipped[4];
	if (dataSegLen < ROMDATA_FILES_OFS) {
		if (fatal) {
			romdataWrongRomError("Data segment too small (%u), need at least %u.", dataSegLen, ROMDATA_FILES_OFS);
		}
		sysLogPrintf(LOG_WARNING, "romdataLoadRomFile: %s data segment too small (%u), skipping", name, dataSegLen);
		sysMemFree(rom);
		return false;
	}

	u8 *dataSeg = sysMemAlloc(dataSegLen);
	if (!dataSeg) {
		if (fatal) {
			sysFatalError("Could not allocate %u bytes for data segment.", dataSegLen);
		}
		sysLogPrintf(LOG_WARNING, "romdataLoadRomFile: could not alloc %u for data seg, skipping", dataSegLen);
		sysMemFree(rom);
		return false;
	}

	u8 scratch[5 * 1024];
	if (rzipInflate(zipped, dataSeg, scratch) < 0) {
		sysMemFree(dataSeg);
		if (fatal) {
			sysFatalError("Could not inflate data segment.");
		}
		sysLogPrintf(LOG_WARNING, "romdataLoadRomFile: could not inflate %s data seg, skipping", name);
		sysMemFree(rom);
		return false;
	}

	*outRom = rom;
	*outSize = romSize;
	*outSeg = dataSeg;
	*outSegSize = dataSegLen;
	return true;
}

static inline void romdataLoadRom(void)
{
	romdataLoadRomFile(g_RomName, &g_RomFile, &g_RomFileSize, &romDataSeg, &romDataSegSize, true, true);
}

static inline void romdataUpdateSegStartEnd(struct romfile* seg)
{
	if (seg->segstart) {
		*seg->segstart = seg->data;
	}

	if (seg->segend) {
		*seg->segend = seg->data + seg->size;
	}
}

static inline void romdataInitSegment(struct romfile *seg)
{
	if (!seg->data) {
		// unused in this ROM, skip it
		sysLogPrintf(LOG_NOTE, "skipping segment %s", seg->name);
		return;
	}

	if (!seg->size) {
		// size unknown
		if (seg[1].name) {
			// use next segment's base to calculate
			seg->size = seg[1].data - seg->data;
		} else {
			// this is the last segment, calculate based on rom size
			seg->size = (uintptr_t)segRomBaseSize - (uintptr_t)seg->data;
		}
	}

	// check if we have an external replacement and load it if so
	char tmp[FS_MAXPATH];
	snprintf(tmp, sizeof(tmp), ROMDATA_SEGDIR "/%s", seg->name);
	u8 *newData = NULL;
	const s32 extFileSize = fsFileSize(tmp);
	if (extFileSize > 0) {
		newData = fsFileLoad(tmp, &seg->size);
	}

	if (!newData) {
		// no external data, just make it point to the rom
		if (segRomBase) {
			newData = segRomBase + (uintptr_t)seg->data;
			seg->source = SRC_ROM;
			sysLogPrintf(LOG_NOTE, "loading segment %s from ROM (offset %08x pointer %p)", seg->name, (uintptr_t)seg->data, newData);
		} else {
			sysFatalError("No ROM or external file for segment:\n%s", seg->name);
		}
	} else {
		// loaded external data
		seg->source = SRC_EXTERNAL;
		sysLogPrintf(LOG_NOTE, "loading segment %s from file (pointer %p)", seg->name, newData);
	}

	seg->data = newData;

	romdataUpdateSegStartEnd(seg);

	// call the post load function if any
	if (seg->preprocess && !seg->preprocessed) {
		newData = seg->preprocess(seg->data, seg->size, &seg->size);

		if (newData) {
			if (seg->source == SRC_EXTERNAL)
				sysMemFree(seg->data);
			seg->data = newData;
			romdataUpdateSegStartEnd(seg);
		}
		
		seg->preprocessed = 1;
	}
}

static inline s32 romdataLoadExternalFileList(void)
{
	romDataSeg = fsFileLoad("filenames.lst", &romDataSegSize); // this null terminates the file by itself
	if (!romDataSeg || !romDataSegSize) {
		return 0;
	}

	s32 n = 1;
	char *p = (char *)romDataSeg;
	while (*p && n < ROMDATA_MAX_FILES) {
		// skip whitespace
		while (*p && isspace(*p)) ++p;
		if (*p) {
			const char *start = p;
			// skip to next whitespace or end of file
			while (*p && !isspace(*p)) ++p;
			// null terminate the name if needed
			if (*p) {
				*p++ = '\0';
			}
			fileSlots[g_ModNum][n++].name = start;
		}
	}

	return n - 1;
}

static inline void romdataInitFiles(void)
{
	if (!g_RomFile) {
		// no ROM; try to load the file name list from disk
		if (!romdataLoadExternalFileList()) {
			sysFatalError("No ROM file or external filename table found.");
		}
		return;
	}

	// the file offset table is in the data seg
	const u32 *offsets = (u32 *)(romDataSeg + ROMDATA_FILES_OFS);
	u32 i;
	for (i = 1; offsets[i]; ++i) {
		if (offsets + i + 1 < (u32 *)(romDataSeg + romDataSegSize)) {
			const u32 nextofs = PD_BE32(offsets[i + 1]);
			const u32 ofs = PD_BE32(offsets[i]);
			fileSlots[MOD_NORMAL][i].data = g_RomFile + ofs;
			fileSlots[MOD_NORMAL][i].size = nextofs - ofs;
			fileSlots[MOD_NORMAL][i].source = SRC_UNLOADED;
			fileSlots[MOD_NORMAL][i].preprocessed = 0;
			fileSlots[MOD_GEX][i].data = g_RomFile + ofs;
			fileSlots[MOD_GEX][i].size = nextofs - ofs;
			fileSlots[MOD_GEX][i].source = SRC_UNLOADED;
			fileSlots[MOD_GEX][i].preprocessed = 0;
			fileSlots[MOD_KAKARIKO][i].data = g_RomFile + ofs;
			fileSlots[MOD_KAKARIKO][i].size = nextofs - ofs;
			fileSlots[MOD_KAKARIKO][i].source = SRC_UNLOADED;
			fileSlots[MOD_KAKARIKO][i].preprocessed = 0;
			fileSlots[MOD_DARKNOON][i].data = g_RomFile + ofs;
			fileSlots[MOD_DARKNOON][i].size = nextofs - ofs;
			fileSlots[MOD_DARKNOON][i].source = SRC_UNLOADED;
			fileSlots[MOD_DARKNOON][i].preprocessed = 0;
			fileSlots[MOD_GOLDFINGER_64][i].data = g_RomFile + ofs;
			fileSlots[MOD_GOLDFINGER_64][i].size = nextofs - ofs;
			fileSlots[MOD_GOLDFINGER_64][i].source = SRC_UNLOADED;
			fileSlots[MOD_GOLDFINGER_64][i].preprocessed = 0;
		}
	}

	// last offset is to the name table
	const u32 *nameOffsets = (u32 *)(g_RomFile + PD_BE32(offsets[i - 1]));
	for (i = 1; nameOffsets[i]; ++i) {
		const u32 ofs = PD_BE32(nameOffsets[i]);
		fileSlots[MOD_NORMAL][i].name = (const char *)nameOffsets + ofs; // ofs is relative to the start of the name table
		fileSlots[MOD_GEX][i].name = (const char *)nameOffsets + ofs; // ofs is relative to the start of the name table
		fileSlots[MOD_KAKARIKO][i].name = (const char *)nameOffsets + ofs; // ofs is relative to the start of the name table
		fileSlots[MOD_DARKNOON][i].name = (const char *)nameOffsets + ofs; // ofs is relative to the start of the name table
		fileSlots[MOD_GOLDFINGER_64][i].name = (const char *)nameOffsets + ofs; // ofs is relative to the start of the name table
	}

	// Model Slot Expansion
	// Dr. Caroll Body (PD Plus Mod)
	fileSlots[MOD_NORMAL][FILE_CDRCARROLL2].data = 0;
	fileSlots[MOD_NORMAL][FILE_CDRCARROLL2].size = 0;
	fileSlots[MOD_NORMAL][FILE_CDRCARROLL2].source = SRC_UNLOADED;
	fileSlots[MOD_NORMAL][FILE_CDRCARROLL2].preprocessed = 0;
	fileSlots[MOD_NORMAL][FILE_CDRCARROLL2].name = "Ccarroll2Z";
	fileSlots[MOD_GEX][FILE_CDRCARROLL2] = fileSlots[MOD_NORMAL][FILE_CDRCARROLL2];
	fileSlots[MOD_KAKARIKO][FILE_CDRCARROLL2] = fileSlots[MOD_NORMAL][FILE_CDRCARROLL2];
	fileSlots[MOD_DARKNOON][FILE_CDRCARROLL2] = fileSlots[MOD_NORMAL][FILE_CDRCARROLL2];
	fileSlots[MOD_GOLDFINGER_64][FILE_CDRCARROLL2] = fileSlots[MOD_NORMAL][FILE_CDRCARROLL2];
	// Skedar Body (PD Plus Mod)
	fileSlots[MOD_NORMAL][FILE_CSKEDAR2].data = 0;
	fileSlots[MOD_NORMAL][FILE_CSKEDAR2].size = 0;
	fileSlots[MOD_NORMAL][FILE_CSKEDAR2].source = SRC_UNLOADED;
	fileSlots[MOD_NORMAL][FILE_CSKEDAR2].preprocessed = 0;
	fileSlots[MOD_NORMAL][FILE_CSKEDAR2].name = "Cskedar2Z";
	fileSlots[MOD_GEX][FILE_CSKEDAR2] = fileSlots[MOD_NORMAL][FILE_CSKEDAR2];
	fileSlots[MOD_KAKARIKO][FILE_CSKEDAR2] = fileSlots[MOD_NORMAL][FILE_CSKEDAR2];
	fileSlots[MOD_DARKNOON][FILE_CSKEDAR2] = fileSlots[MOD_NORMAL][FILE_CSKEDAR2];
	fileSlots[MOD_GOLDFINGER_64][FILE_CSKEDAR2] = fileSlots[MOD_NORMAL][FILE_CSKEDAR2];
	// Dr. Caroll Hand (PD Plus Mod)
	fileSlots[MOD_NORMAL][FILE_GHAND_DRCARROLL].data = 0;
	fileSlots[MOD_NORMAL][FILE_GHAND_DRCARROLL].size = 0;
	fileSlots[MOD_NORMAL][FILE_GHAND_DRCARROLL].source = SRC_UNLOADED;
	fileSlots[MOD_NORMAL][FILE_GHAND_DRCARROLL].preprocessed = 0;
	fileSlots[MOD_NORMAL][FILE_GHAND_DRCARROLL].name = "Ghand_carollZ";
	fileSlots[MOD_GEX][FILE_GHAND_DRCARROLL] = fileSlots[MOD_NORMAL][FILE_GHAND_DRCARROLL];
	fileSlots[MOD_KAKARIKO][FILE_GHAND_DRCARROLL] = fileSlots[MOD_NORMAL][FILE_GHAND_DRCARROLL];
	fileSlots[MOD_DARKNOON][FILE_GHAND_DRCARROLL] = fileSlots[MOD_NORMAL][FILE_GHAND_DRCARROLL];
	fileSlots[MOD_GOLDFINGER_64][FILE_GHAND_DRCARROLL] = fileSlots[MOD_NORMAL][FILE_GHAND_DRCARROLL];
	// Skedar Hand (PD Plus Mod)
	fileSlots[MOD_NORMAL][FILE_GHAND_SKEDAR].data = 0;
	fileSlots[MOD_NORMAL][FILE_GHAND_SKEDAR].size = 0;
	fileSlots[MOD_NORMAL][FILE_GHAND_SKEDAR].source = SRC_UNLOADED;
	fileSlots[MOD_NORMAL][FILE_GHAND_SKEDAR].preprocessed = 0;
	fileSlots[MOD_NORMAL][FILE_GHAND_SKEDAR].name = "Ghand_skedarZ";
	fileSlots[MOD_GEX][FILE_GHAND_SKEDAR] = fileSlots[MOD_NORMAL][FILE_GHAND_SKEDAR];
	fileSlots[MOD_KAKARIKO][FILE_GHAND_SKEDAR] = fileSlots[MOD_NORMAL][FILE_GHAND_SKEDAR];
	fileSlots[MOD_DARKNOON][FILE_GHAND_SKEDAR] = fileSlots[MOD_NORMAL][FILE_GHAND_SKEDAR];
	fileSlots[MOD_GOLDFINGER_64][FILE_GHAND_SKEDAR] = fileSlots[MOD_NORMAL][FILE_GHAND_SKEDAR];
}

// build the MOD_CHAINROM file table from the chain ROM's own offset and name
// tables, so its assets and setup files (which carry the stage-specific action
// blocks) resolve from the chain ROM rather than the base ROM
static inline void romdataInitChainFiles(void)
{
	// the file offset table is in the chain ROM's data seg
	const u32 *offsets = (u32 *)(chainDataSeg + ROMDATA_FILES_OFS);
	u32 i;
	for (i = 1; offsets[i]; ++i) {
		if (offsets + i + 1 < (u32 *)(chainDataSeg + chainDataSegSize)) {
			const u32 nextofs = PD_BE32(offsets[i + 1]);
			const u32 ofs = PD_BE32(offsets[i]);
			fileSlots[MOD_CHAINROM][i].data = chainRomFile + ofs;
			fileSlots[MOD_CHAINROM][i].size = nextofs - ofs;
			fileSlots[MOD_CHAINROM][i].source = SRC_UNLOADED;
			fileSlots[MOD_CHAINROM][i].preprocessed = 0;
		}
	}

	// last offset is to the name table
	const u32 *nameOffsets = (u32 *)(chainRomFile + PD_BE32(offsets[i - 1]));
	for (i = 1; nameOffsets[i]; ++i) {
		const u32 ofs = PD_BE32(nameOffsets[i]);
		fileSlots[MOD_CHAINROM][i].name = (const char *)nameOffsets + ofs; // ofs is relative to the start of the name table
	}

	// mirror the manually-added model-slot-expansion entries from the base table
	// (loaded from loose files if present; absent from the stock ROM file table)
	fileSlots[MOD_CHAINROM][FILE_CDRCARROLL2] = fileSlots[MOD_NORMAL][FILE_CDRCARROLL2];
	fileSlots[MOD_CHAINROM][FILE_CSKEDAR2] = fileSlots[MOD_NORMAL][FILE_CSKEDAR2];
	fileSlots[MOD_CHAINROM][FILE_GHAND_DRCARROLL] = fileSlots[MOD_NORMAL][FILE_GHAND_DRCARROLL];
	fileSlots[MOD_CHAINROM][FILE_GHAND_SKEDAR] = fileSlots[MOD_NORMAL][FILE_GHAND_SKEDAR];
}

static inline void romdataResetFile(s32 fileNum)
{
	// the file offset table is in the data seg
	const u32 *offsets = (u32 *)(romDataSeg + ROMDATA_FILES_OFS);
	if (offsets + fileNum + 1 < (u32 *)(romDataSeg + romDataSegSize)) {
		const u32 nextofs = PD_BE32(offsets[fileNum + 1]);
		const u32 ofs = PD_BE32(offsets[fileNum]);
		fileSlots[g_ModNum][fileNum].data = g_RomFile + ofs;
		fileSlots[g_ModNum][fileNum].size = nextofs - ofs;
		fileSlots[g_ModNum][fileNum].source = SRC_UNLOADED;
		fileSlots[g_ModNum][fileNum].preprocessed = 0;
	}
}

static inline struct romfile *romdataGetSeg(const char *name)
{
	struct romfile *seg = romSegs;
	while (seg->name && strcmp(name, seg->name)) {
		++seg;
	}
	return seg;
}

// read the 24-bit big-endian dataoffset of textureslist entry n
static inline u32 romdataTexListDofs(const u8 *rom, u32 listOfs, u32 n)
{
	const u8 *e = rom + listOfs + n * 8;
	return ((u32)e[1] << 16) | ((u32)e[2] << 8) | e[3];
}

// total conversions commonly grow the texture data, which shifts the trailing
// texturesdata/textureslist/copyright segments away from their stock offsets
// while everything before them (fonts, animations, audio banks) stays in place.
// locate the chain ROM's real textureslist by signature: 8-byte entries with a
// non-decreasing 24-bit big-endian dataoffset in bytes 1-3 and zeroes in bytes
// 4-7, first entry at dataoffset 0; the final (terminator) entry holds the
// total texturesdata size. texturesdata itself is NOT reliably adjacent to the
// list (GoldenEye X has an extra build-tool structure between them), so its
// base is found by correlation against the base ROM: PD-derived mods keep many
// stock textures byte-identical, so sample entries across the list, take each
// texture's first bytes from the base ROM and search for them in the chain
// ROM; every hit votes for an implied base offset, majority wins. on an
// unmodified ROM all of this reproduces the stock offsets exactly.
static void romdataChainRelocateTexSegments(void)
{
	const u8 *rom = chainRomFile;
	u32 bestOfs = 0, bestCount = 0, bestTerm = 0;
	u32 runOfs = 0, runCount = 0, runRises = 0, prevDofs = 0;

	for (u32 o = 0; o + 8 <= chainRomFileSize; o += 8) {
		const u32 dofs = ((u32)rom[o + 1] << 16) | ((u32)rom[o + 2] << 8) | rom[o + 3];
		const s32 entryok = rom[o + 4] == 0 && rom[o + 5] == 0 && rom[o + 6] == 0 && rom[o + 7] == 0
			&& (runCount == 0 ? dofs == 0 : dofs >= prevDofs);

		if (entryok) {
			if (runCount == 0) {
				runOfs = o;
				runRises = 0;
			} else if (dofs > prevDofs) {
				++runRises;
			}
			prevDofs = dofs;
			++runCount;
			continue;
		}

		if (runCount) {
			// trim fake leading entries: zero padding right before the real
			// list can parse as extra zero-dataoffset entries. the real first
			// entry is the only zero-dataoffset entry whose successor has a
			// nonzero dataoffset (texture 0 is never empty in a PD-derived ROM)
			while (runCount > 1) {
				const u32 second = ((u32)rom[runOfs + 9] << 16) | ((u32)rom[runOfs + 10] << 8) | rom[runOfs + 11];
				if (second != 0) {
					break;
				}
				runOfs += 8;
				--runCount;
			}

			// run ended; viable candidates are 16-aligned (segments are) with
			// plenty of entries, mostly increasing offsets (rejects zero-filled
			// regions), a plausible texturesdata size, and that data must fit
			// right before the list
			const u32 dataSize = (prevDofs + 15) & ~15u;
			if ((runOfs & 15) == 0 && runCount >= 1024 && runRises >= runCount / 2
					&& prevDofs >= 0x10000 && dataSize < runOfs && runCount > bestCount) {
				bestOfs = runOfs;
				bestCount = runCount;
				bestTerm = prevDofs;
			}
			runCount = 0;
			o -= 8; // the entry that broke the run may start a new one
		}
	}

	if (!bestCount) {
		sysLogPrintf(LOG_WARNING, "chain ROM: could not locate a textureslist; chain ROM textures may be broken");
		return;
	}

	struct romfile *segData = romdataGetSeg("texturesdata");
	struct romfile *segList = romdataGetSeg("textureslist");
	struct romfile *segCopy = romdataGetSeg("copyright");
	const u32 stockListOfs = (u32)(uintptr_t)segList->data;
	const u32 stockDataOfs = (u32)(uintptr_t)segData->data;
	const u32 stockCount = ((u32)(uintptr_t)segCopy->data - stockListOfs) / 8;

	// vote for the texturesdata base by correlating texture bytes with the base ROM
	enum { CORR_SAMPLES = 24, CORR_PATLEN = 16, CORR_MAXCAND = 32, CORR_MAXHITS = 16, CORR_MINVOTES = 3 };
	struct { u32 base; u32 votes; } cand[CORR_MAXCAND];
	u32 numCand = 0;

	const u32 maxn = (bestCount < stockCount ? bestCount : stockCount) - 1;
	for (u32 s = 0; s < CORR_SAMPLES; ++s) {
		const u32 n = 8 + (u32)((u64)(maxn - 8) * s / CORR_SAMPLES);
		const u32 sThis = romdataTexListDofs(g_RomFile, stockListOfs, n);
		const u32 sNext = romdataTexListDofs(g_RomFile, stockListOfs, n + 1);
		const u32 cThis = romdataTexListDofs(rom, bestOfs, n);
		const u32 cNext = romdataTexListDofs(rom, bestOfs, n + 1);
		if (sThis >= sNext || cThis >= cNext) {
			continue; // no data for this texture in one of the ROMs
		}

		const u8 *pat = g_RomFile + stockDataOfs + sThis;
		const u8 *p = rom;
		const u8 *end = rom + chainRomFileSize - CORR_PATLEN;
		u32 hits = 0;

		while (p <= end && hits < CORR_MAXHITS) {
			p = memchr(p, pat[0], end - p + 1);
			if (!p) {
				break;
			}
			if (memcmp(p, pat, CORR_PATLEN) == 0) {
				++hits;
				const u32 pos = (u32)(p - rom);
				if (pos >= cThis) {
					const u32 base = pos - cThis;
					u32 c;
					for (c = 0; c < numCand && cand[c].base != base; ++c);
					if (c < numCand) {
						++cand[c].votes;
					} else if (numCand < CORR_MAXCAND) {
						cand[numCand].base = base;
						cand[numCand].votes = 1;
						++numCand;
					}
				}
			}
			++p;
		}
	}

	u32 dataOfs = 0, dataVotes = 0;
	for (u32 c = 0; c < numCand; ++c) {
		if (cand[c].votes > dataVotes) {
			dataOfs = cand[c].base;
			dataVotes = cand[c].votes;
		}
	}

	if (dataVotes < CORR_MINVOTES || (u64)dataOfs + bestTerm > chainRomFileSize) {
		sysLogPrintf(LOG_WARNING, "chain ROM: could not locate texturesdata (list at 0x%x, best base 0x%x with %u votes); chain ROM textures may be broken",
			bestOfs, dataOfs, dataVotes);
		return;
	}

	sysLogPrintf(LOG_NOTE, "chain ROM: textureslist at 0x%x (%u entries), texturesdata at 0x%x size 0x%x (%u votes; stock 0x%x/0x%x)",
		bestOfs, bestCount, dataOfs, bestTerm, dataVotes, stockListOfs, stockDataOfs);

	segData->data = (u8 *)(uintptr_t)dataOfs;
	segData->size = bestTerm;
	segList->data = (u8 *)(uintptr_t)bestOfs;
	segList->size = bestCount * 8;
	segCopy->data = (u8 *)(uintptr_t)(bestOfs + bestCount * 8); // keeps its stock size; only the start moves
}

// import the chain ROM's own stage table from its inflated data segment.
// mods edit per-stage parameters there: GoldenEye X remaps several stages'
// bg/tiles/pads/setup file assignments and tunes per-stage lighting, which
// the compiled-in g_Stages of this build would otherwise override with stock
// values. the table is located by matching this build's stage-id sequence at
// a self-calibrated stride, since the N64 entry (0x38 bytes) is smaller than
// the port's struct stagetableentry, which has fields appended at the end.
// the id is the match key; all N64 fields are imported, while port-appended
// fields (alarm, extragunmem) and port-added stage entries past the N64
// table keep their compiled-in values. file ids are only taken when they
// resolve to a file actually present in the chain ROM's file table.
static void romdataChainImportStageTable(void)
{
	const u32 numStages = sizeof(g_Stages) / sizeof(g_Stages[0]);
	const u8 *seg = chainDataSeg;
	u32 tabOfs = 0, tabStride = 0, tabCount = 0;

	for (u32 stride = 0x30; stride <= 0x48 && !tabCount; stride += 2) {
		for (u32 o = 0; o + stride * 32 <= chainDataSegSize; o += 2) {
			u32 k = 0;
			while (k < numStages && o + (k + 1) * stride <= chainDataSegSize
					&& PD_BE16(*(u16 *)(seg + o + k * stride)) == (u16)g_Stages[k].id) {
				++k;
			}
			if (k >= 32) {
				tabOfs = o;
				tabStride = stride;
				tabCount = k;
				break;
			}
		}
	}

	if (!tabCount) {
		sysLogPrintf(LOG_WARNING, "chain ROM: stage table not found in the data segment; keeping stock stage parameters");
		return;
	}

	u32 filesKept = 0;

	for (u32 k = 0; k < tabCount; ++k) {
		const u8 *e = seg + tabOfs + k * tabStride;
		struct stagetableentry *dst = &g_Stages[k];
		u32 u;

		dst->light_type = e[0x02];
		dst->light_alpha = e[0x03];
		dst->light_width = e[0x04];
		dst->light_height = e[0x05];
		dst->unk06 = PD_BE16(*(u16 *)(e + 0x06));

		// file assignments
		const u32 fileFieldOfs[5] = { 0x08, 0x0a, 0x0c, 0x0e, 0x10 };
		u16 *const dstFiles[5] = { &dst->bgfileid, &dst->tilefileid, &dst->padsfileid, &dst->setupfileid, &dst->mpsetupfileid };
		for (u32 f = 0; f < 5; ++f) {
			const u16 v = PD_BE16(*(u16 *)(e + fileFieldOfs[f]));
			if (v > 0 && v < ROMDATA_MAX_FILES && fileSlots[MOD_CHAINROM][v].data) {
				*dstFiles[f] = v;
			} else if (v != *dstFiles[f]) {
				++filesKept;
			}
		}

		u = PD_BE32(*(u32 *)(e + 0x14)); memcpy(&dst->unk14, &u, 4);
		u = PD_BE32(*(u32 *)(e + 0x18)); memcpy(&dst->unk18, &u, 4);
		u = PD_BE32(*(u32 *)(e + 0x1c)); memcpy(&dst->unk1c, &u, 4);
		dst->unk20 = PD_BE16(*(u16 *)(e + 0x20));
		dst->unk22 = e[0x22];
		dst->unk23 = (s8)e[0x23];
		dst->unk24 = PD_BE32(*(u32 *)(e + 0x24));
		dst->unk28 = PD_BE32(*(u32 *)(e + 0x28));
		dst->unk2c = (s16)PD_BE16(*(u16 *)(e + 0x2c));
		dst->eraserpropdist = (s16)PD_BE16(*(u16 *)(e + 0x2e));
		dst->unk30 = (s16)PD_BE16(*(u16 *)(e + 0x30));

		if (tabStride >= 0x38) {
			u = PD_BE32(*(u32 *)(e + 0x34)); memcpy(&dst->unk34, &u, 4);
		}
	}

	sysLogPrintf(LOG_NOTE, "chain ROM: imported stage table from data segment (offset 0x%x, stride 0x%x, %u entries, %u file refs kept stock)",
		tabOfs, tabStride, tabCount, filesKept);
}

// import the chain ROM's character model table (g_HeadsAndBodies) from its
// inflated data segment. mods repoint bodies/heads to their own model files
// and retune per-body scale/animscale/height there (GoldenEye X changes the
// file of 67 of the 152 entries), which the compiled-in table would otherwise
// override with stock values. the N64 entry is 0x14 bytes: u16 packed
// bitfield (ismale/unk/canvaryheight/type/height), u16 filenum, f32 scale,
// f32 animscale, a runtime modeldef cache pointer that is always zero in the
// ROM (which doubles as the locator signature, since the filenum column is
// exactly what mods edit), and u16 handfilenum. the bitfield is decoded by
// big-endian bit position and assigned by name, since host bitfield layout
// differs from MIPS. file ids are only taken when they resolve in the chain
// ROM's file table; the modeldef cache stays NULL.
static void romdataChainImportHeadsAndBodies(void)
{
	const u32 numBodies = sizeof(g_HeadsAndBodies) / sizeof(g_HeadsAndBodies[0]);
	const u32 entSize = 0x14;
	const u8 *seg = chainDataSeg;
	u32 tabOfs = 0, tabMatches = 0;

	for (u32 o = 0; o + numBodies * entSize <= chainDataSegSize; o += 4) {
		// the modeldef cache column must be zero in every entry
		u32 k = 0;
		while (k < numBodies && !*(u32 *)(seg + o + k * entSize + 0x0c)) {
			++k;
		}
		if (k < numBodies) {
			continue;
		}

		// count entries whose filenum matches ours; mods edit some, not most
		u32 matches = 0;
		for (k = 0; k < numBodies; ++k) {
			if (PD_BE16(*(u16 *)(seg + o + k * entSize + 0x02)) == g_HeadsAndBodies[k].filenum) {
				++matches;
			}
		}
		if (matches > tabMatches) {
			tabOfs = o;
			tabMatches = matches;
		}
	}

	if (tabMatches < numBodies / 3) {
		sysLogPrintf(LOG_WARNING, "chain ROM: character model table not found in the data segment (best %u/%u matches); keeping stock models",
			tabMatches, numBodies);
		return;
	}

	u32 filesKept = 0;

	for (u32 k = 0; k < numBodies; ++k) {
		const u8 *e = seg + tabOfs + k * entSize;
		struct headorbody *dst = &g_HeadsAndBodies[k];
		const u16 bits = PD_BE16(*(u16 *)(e + 0x00));
		u32 u;

		dst->ismale = (bits >> 15) & 1;
		dst->unk00_01 = (bits >> 14) & 1;
		dst->canvaryheight = (bits >> 13) & 1;
		dst->type = (bits >> 10) & 7;
		dst->height = (bits >> 2) & 0xff;

		const u16 fn = PD_BE16(*(u16 *)(e + 0x02));
		if (fn > 0 && fn < ROMDATA_MAX_FILES && fileSlots[MOD_CHAINROM][fn].data) {
			dst->filenum = fn;
		} else if (fn != dst->filenum) {
			++filesKept;
		}

		u = PD_BE32(*(u32 *)(e + 0x04)); memcpy(&dst->scale, &u, 4);
		u = PD_BE32(*(u32 *)(e + 0x08)); memcpy(&dst->animscale, &u, 4);

		const u16 hfn = PD_BE16(*(u16 *)(e + 0x10));
		if (hfn == 0 || (hfn < ROMDATA_MAX_FILES && fileSlots[MOD_CHAINROM][hfn].data)) {
			dst->handfilenum = hfn;
		} else if (hfn != dst->handfilenum) {
			++filesKept;
		}
	}

	sysLogPrintf(LOG_NOTE, "chain ROM: imported character model table from data segment (offset 0x%x, %u entries, %u stock filenum matches, %u file refs kept stock)",
		tabOfs, numBodies, tabMatches, filesKept);
}

s32 romdataInit(void)
{
	const char *altRomName = sysArgGetString("--rom-file");
	if (altRomName) {
		g_RomName = altRomName;
	}

	romdataLoadRom();

	// optional chain-loaded second ROM: a whole pre-modded PD ROM whose data
	// (assets + setup files carrying action blocks) backs the MOD_CHAINROM slot.
	// must be the same region as the base ROM (file table offsets and FILE_* ids
	// are version-specific). the engine code stays this build.
	const char *chainRomName = sysArgGetString("--mod-rom");
	if (chainRomName) {
		romdataLoadRomFile(chainRomName, &chainRomFile, &chainRomFileSize, &chainDataSeg, &chainDataSegSize, false, true);
		g_ChainRomActive = 1;
		g_ModNum = MOD_CHAINROM;
		sysLogPrintf(LOG_NOTE, "romdataInit: chain ROM active (MOD_CHAINROM): %s", chainRomName);
	}

	// resolve segments (and the slow boot preprocessing) against the chain ROM
	// when active, otherwise the base ROM
	if (g_ChainRomActive) {
		segRomBase = chainRomFile;
		segRomBaseSize = chainRomFileSize;
		romdataChainRelocateTexSegments();
	} else {
		segRomBase = g_RomFile;
		segRomBaseSize = g_RomFileSize;
	}

	// boot preprocessing (animations, textures, audio banks) is the slow part
	// of startup; mirror its progress onto the taskbar/dock icon
	s32 totalSegs = 0;
	for (struct romfile *seg = romSegs; seg->name; ++seg) {
		++totalSegs;
	}

	// set segments to point to the rom or load them externally
	s32 doneSegs = 0;
	for (struct romfile *seg = romSegs; seg->name; ++seg) {
		videoSetTaskbarProgress(VIDEO_TASKBAR_NORMAL, (f32)doneSegs / (f32)totalSegs);
		romdataInitSegment(seg);
		++doneSegs;
	}

	// load the base ROM file table into all the loose-file mod slots
	romdataInitFiles();

	// then build the MOD_CHAINROM table from the chain ROM's own file table,
	// and import its stage table (file assignments, lighting, scales) and
	// character model table (body/head model files, scales, heights)
	if (g_ChainRomActive) {
		romdataInitChainFiles();
		romdataChainImportStageTable();
		romdataChainImportHeadsAndBodies();
	}

	// Chaos model-swap OVERLAY ROM via the --model-rom launch arg (optional; the
	// Lua side normally auto-loads from scripts/chaos/rom instead). Deferred to
	// romdataLoadModelRom so both entry points share one path.
	{
		const char *modelRomName = sysArgGetString("--model-rom");
		if (modelRomName) {
			romdataLoadModelRom(modelRomName);
		}
	}

	videoSetTaskbarProgress(VIDEO_TASKBAR_NONE, 0.f);

	sysLogPrintf(LOG_NOTE, "romdataInit: loaded rom, size = %u", g_RomFileSize);

	return 0;
}

// Load a model-swap OVERLAY ROM at runtime. `path` may be a ROM FILE or a
// DIRECTORY (scanned for the first ROM-sized file — so the Lua side can just
// point at scripts/chaos/rom and the user drops any-named z64 in it). Populates
// the MOD_CHAINROM file table without switching the active slot; nothing is
// redirected until g_ModelSwapActive is set. Returns 1 on success, 0 if no
// usable ROM was found (graceful — a missing folder/file never fatals, so the
// boot auto-load is safe when no ROM is present). A ROM that IS present but the
// wrong size/compression still fatals in romdataLoadRomFile, same as --rom-file.
s32 romdataLoadModelRom(const char *path)
{
	char filepath[FS_MAXPATH + 1] = { 0 };

	if (g_ChainRomActive) {
		sysLogPrintf(LOG_WARNING, "romdataLoadModelRom: --mod-rom active; model overlay skipped");
		return 0;
	}
	if (g_ModelRomActive) {
		return 1; // already loaded (idempotent)
	}
	if (!path || !path[0]) {
		return 0;
	}

	// A ROM is at least the stock 32MB; expanded total conversions (e.g. a Mario
	// model pack) are larger, so accept anything >= stock size.
	if (fsFileSize(path) >= (s32)ROMDATA_ROM_SIZE) {
		// path is the ROM file itself
		snprintf(filepath, sizeof(filepath), "%s", path);
	} else {
		// Treat path as a directory. Resolve it through the fs search path (mod +
		// base dirs) ONCE, then opendir + stat that SAME absolute location — a raw
		// relative opendir uses the CWD while fsFileSize resolves via fsFullPath,
		// so in an AIO/mod layout they disagree and the folder looks empty even
		// with the ROM present.
		char dirfull[FS_MAXPATH + 1];
		DIR *dr;
		struct dirent *de;

		snprintf(dirfull, sizeof(dirfull), "%s", fsFullPath(path));

		dr = opendir(dirfull);
		if (!dr) {
			sysLogPrintf(LOG_NOTE, "romdataLoadModelRom: no folder %s (resolved: %s)", path, dirfull);
			return 0;
		}

		while ((de = readdir(dr)) != NULL) {
			// cand is built from the SAME resolved dir opendir walked; fsFileSize
			// treats this ./- or absolute path consistently (no CWD-vs-search
			// mismatch), so listing and sizing agree.
			char cand[FS_MAXPATH + 1];
			snprintf(cand, sizeof(cand), "%s/%s", dirfull, de->d_name);
			if (fsFileSize(cand) >= (s32)ROMDATA_ROM_SIZE) {
				snprintf(filepath, sizeof(filepath), "%s", cand);
				break;
			}
		}

		closedir(dr);

		if (!filepath[0]) {
			sysLogPrintf(LOG_NOTE, "romdataLoadModelRom: no ROM-sized file in %s (resolved: %s)", path, dirfull);
			return 0;
		}
	}

	sysLogPrintf(LOG_NOTE, "romdataLoadModelRom: loading model overlay ROM: %s", filepath);
	// graceful (fatal=false): an incompatible/relocated mod ROM logs + skips
	// rather than bricking boot.
	if (!romdataLoadRomFile(filepath, &chainRomFile, &chainRomFileSize, &chainDataSeg, &chainDataSegSize, false, false)) {
		sysLogPrintf(LOG_WARNING, "romdataLoadModelRom: %s could not be loaded as an overlay (see above); model swap disabled", filepath);
		return 0;
	}
	romdataInitChainFiles();
	g_ModelRomActive = 1;
	return 1;
}

static inline bool romdataCheckGbcRomContents(const u8 *gbcRomFile, const u32 gbcRomSize)
{
	if (gbcRomSize != GBC_ROM_SIZE) {
		return false;
	}

	// ROM title
	if (memcmp(gbcRomFile + 0x134, "PerfDark   VPDE", 15) != 0) {
		return false;
	}

	// Licensee code
	if (memcmp(gbcRomFile + 0x144, "4Y", 2) != 0) {
		return false;
	}

	// Header and global checksums
	if (gbcRomFile[0x14D] != 0xA1 || gbcRomFile[0x14E] != 0xAD || gbcRomFile[0x14F] != 0x0F) {
		return false;
	}

	return true;
}

s32 romdataCheckGbcRom(void)
{
	if (fsFileSize(GBC_ROM_NAME) < 0) {
		// bail early if it doesn't exist to avoid generating error messages
		return false;
	}

	u32 gbcRomSize = 0;
	u8 *gbcRomFile = fsFileLoad(GBC_ROM_NAME, &gbcRomSize);
	if (!gbcRomFile) {
		return false;
	}

	const bool ret = romdataCheckGbcRomContents(gbcRomFile, gbcRomSize);
	sysMemFree(gbcRomFile);

	if (ret) {
		sysLogPrintf(LOG_NOTE, "romdataCheckGbcRom: valid GBC rom found");
	}

	return ret;
}

s32 romdataFileGetSize(s32 fileNum)
{
	if (fileNum < 1 || fileNum >= ROMDATA_MAX_FILES) {
		sysLogPrintf(LOG_ERROR, "romdataFileGetSize: invalid file num %d", fileNum);
		return -1;
	}

	// ensure any external files are loaded and we use their size
	if (romdataFileLoad(fileNum, NULL)) {
		return fileSlots[g_ModNum][fileNum].size;
	}

	sysLogPrintf(LOG_ERROR, "romdataFileGetSize: could not load file num %d", fileNum);
	return -1;
}

u8 *romdataFileGetData(s32 fileNum)
{
	return romdataFileLoad(fileNum, NULL);
}

u8 *romdataFileLoad(s32 fileNum, u32 *outSize)
{
	if (fileNum < 1 || fileNum >= ROMDATA_MAX_FILES) {
		sysLogPrintf(LOG_ERROR, "romdataFileLoad: invalid file num %d", fileNum);
		return NULL;
	}

	u8 *out = NULL;

	// try to load external file
	if (fileSlots[g_ModNum][fileNum].source == SRC_UNLOADED) {
		char tmp[FS_MAXPATH] = { 0 };
		snprintf(tmp, sizeof(tmp), ROMDATA_FILEDIR "/%s", fileSlots[g_ModNum][fileNum].name);

		// All Solos in Multi Mod: do not load in solo, coop, counter-op (excluding playable skedar model)
		if (fsFileSize(tmp) > 0 && (!g_NotLoadMod || fileNum == FILE_CSKEDAR2 || fileNum == FILE_GHAND_SKEDAR)) {
			u32 size = 0;

			out = fsFileLoad(tmp, &size);

			if (out && size) {
				sysLogPrintf(LOG_NOTE, "file %d (%s) loaded externally (g_ModNum: %d)", fileNum, fileSlots[g_ModNum][fileNum].name, g_ModNum);
				fileSlots[g_ModNum][fileNum].data = out;
				fileSlots[g_ModNum][fileNum].size = size;
				fileSlots[g_ModNum][fileNum].source = SRC_EXTERNAL;
				// external file; do not apply patches to this
				fileSlots[g_ModNum][fileNum].numpatches = 0;
			}
		}

		if (fileSlots[g_ModNum][fileNum].source == SRC_UNLOADED) {
			// tried and failed, fall back to ROM
			fileSlots[g_ModNum][fileNum].source = SRC_ROM;
		}
	}

	// Chaos live model swap: source flagged character-model files from the
	// overlay ROM, matched by NAME (a reordered mod file table still resolves).
	// Returns the overlay's compressed bytes; the caller inflates + preprocesses
	// them under the BASE file id, so texture/ext_tex linkage stays correct.
	if (!out && g_ModelSwapActive && g_ModelRomActive && g_ModelSwapFiles[fileNum]) {
		const s32 cn = romdataChainFileGetNumForName(fileSlots[g_ModNum][fileNum].name);
		if (cn > 0 && fileSlots[MOD_CHAINROM][cn].data) {
			if (outSize) {
				*outSize = fileSlots[MOD_CHAINROM][cn].size;
			}
			return fileSlots[MOD_CHAINROM][cn].data;
		}
	}

	if (!out) {
		out = fileSlots[g_ModNum][fileNum].data;
	}

	if (out && outSize) {
		*outSize = fileSlots[g_ModNum][fileNum].size;
	}

	return out;
}

void romdataFilePreprocess(s32 fileNum, s32 loadType, u8 *data, u32 size, u32 *outSize)
{
	loadingFileNum = fileNum;
	if (fileNum < 1 || fileNum >= ROMDATA_MAX_FILES) {
		sysLogPrintf(LOG_ERROR, "romdataFilePreprocess: invalid file num %d", fileNum);
		return;
	}

	if (data && size /* && !fileSlots[g_ModNum][fileNum].preprocessed*/) {
		if (loadType && loadType < (u32)ARRAYCOUNT(filePreprocFuncs) && filePreprocFuncs[loadType]) {
			// apply patches
			for (u32 i = 0; i < fileSlots[g_ModNum][fileNum].numpatches; ++i) {
				const struct romfilepatch *p = &fileSlots[g_ModNum][fileNum].patches[i];
				if (!memcmp(data + p->ofs, p->src, p->len)) {
					memcpy(data + p->ofs, p->dst, p->len);
					sysLogPrintf(LOG_NOTE, "file %d (%s) patched at offset 0x%x", fileNum, fileSlots[g_ModNum][fileNum].name, p->ofs);
				}
			}
			// then preprocess
			filePreprocFuncs[loadType](data, size, outSize);
			// fileSlots[g_ModNum][fileNum].preprocessed = 1;
		}
	}
}

void romdataFileFree(s32 fileNum)
{
	if (fileNum < 1 || fileNum >= ROMDATA_MAX_FILES) {
		sysLogPrintf(LOG_ERROR, "fsFileFree: invalid file num %d", fileNum);
		return;
	}

	if (fileSlots[g_ModNum][fileNum].source == SRC_EXTERNAL) {
		sysMemFree(fileSlots[g_ModNum][fileNum].data);
		fileSlots[g_ModNum][fileNum].data = NULL;
	}

	fileSlots[g_ModNum][fileNum].source = SRC_UNLOADED;
}

void romdataFileFreeForSolo(void)
{
	// the chain ROM is a full data set bound to MOD_CHAINROM for the whole
	// session; resetting its slots back to the base ROM would defeat the point
	if (g_ChainRomActive) {
		return;
	}

	// All Solos in Multi Mod: reset mod files for solo (bg, clipping, pads)
	romdataResetFile(0x009); // bgdata/bg_azt.seg
	romdataResetFile(0x00a); // bgdata/bg_pete.seg
	romdataResetFile(0x00b); // bgdata/bg_depo.seg
	romdataResetFile(0x00e); // bgdata/bg_dam.seg
	romdataResetFile(0x014); // bgdata/bg_cave.seg
	romdataResetFile(0x017); // bgdata/bg_sho.seg
	romdataResetFile(0x018); // bgdata/bg_eld.seg
	romdataResetFile(0x019); // bgdata/bg_imp.seg
	romdataResetFile(0x01b); // bgdata/bg_lue.seg
	romdataResetFile(0x01c); // bgdata/bg_ame.seg
	romdataResetFile(0x01d); // bgdata/bg_rit.seg
	romdataResetFile(0x01f); // bgdata/bg_ear.seg
	romdataResetFile(0x020); // bgdata/bg_lee.seg
	romdataResetFile(0x024); // bgdata/bg_pam.seg
	romdataResetFile(0x14b); // bgdata/bg_ame_padsZ
	romdataResetFile(0x14c); // bgdata/bg_ame_tilesZ
	romdataResetFile(0x155); // bgdata/bg_azt_padsZ
	romdataResetFile(0x156); // bgdata/bg_azt_tilesZ
	romdataResetFile(0x159); // bgdata/bg_cave_padsZ
	romdataResetFile(0x15a); // bgdata/bg_cave_tilesZ
	romdataResetFile(0x15f); // bgdata/bg_dam_padsZ
	romdataResetFile(0x160); // bgdata/bg_dam_tilesZ
	romdataResetFile(0x161); // bgdata/bg_depo_padsZ
	romdataResetFile(0x162); // bgdata/bg_depo_tilesZ
	romdataResetFile(0x167); // bgdata/bg_ear_padsZ
	romdataResetFile(0x168); // bgdata/bg_ear_tilesZ
	romdataResetFile(0x169); // bgdata/bg_eld_padsZ
	romdataResetFile(0x16a); // bgdata/bg_eld_tilesZ
	romdataResetFile(0x16b); // bgdata/bg_imp_padsZ
	romdataResetFile(0x16c); // bgdata/bg_imp_tilesZ
	romdataResetFile(0x16f); // bgdata/bg_lee_padsZ
	romdataResetFile(0x170); // bgdata/bg_lee_tilesZ
	romdataResetFile(0x175); // bgdata/bg_lue_padsZ
	romdataResetFile(0x176); // bgdata/bg_lue_tilesZ
	romdataResetFile(0x179); // bgdata/bg_pam_padsZ
	romdataResetFile(0x17a); // bgdata/bg_pam_tilesZ
	romdataResetFile(0x17b); // bgdata/bg_pete_padsZ
	romdataResetFile(0x17c); // bgdata/bg_pete_tilesZ
	romdataResetFile(0x17f); // bgdata/bg_rit_padsZ
	romdataResetFile(0x180); // bgdata/bg_rit_tilesZ
	romdataResetFile(0x189); // bgdata/bg_sho_padsZ
	romdataResetFile(0x18a); // bgdata/bg_sho_tilesZ
}

const char *romdataFileGetName(s32 fileNum)
{
	if (fileNum < 1 || fileNum >= ROMDATA_MAX_FILES) {
		return NULL;
	}
	return fileSlots[g_ModNum][fileNum].name;
}

s32 romdataFileGetNumForName(const char *name)
{
	if (!name || !name[0]) {
		return -1;
	}

	for (s32 i = 0; i < ROMDATA_MAX_FILES; ++i) {
		if (fileSlots[g_ModNum][i].name && !strcmp(fileSlots[g_ModNum][i].name, name)) {
			return i;
		}
	}

	return -1;
}

// Same as romdataFileGetNumForName but searches the overlay (MOD_CHAINROM) table
// — used to resolve a base model file to the overlay ROM's same-named file.
s32 romdataChainFileGetNumForName(const char *name)
{
	if (!name || !name[0]) {
		return -1;
	}

	for (s32 i = 0; i < ROMDATA_MAX_FILES; ++i) {
		if (fileSlots[MOD_CHAINROM][i].name && !strcmp(fileSlots[MOD_CHAINROM][i].name, name)) {
			return i;
		}
	}

	return -1;
}

u8 *romdataSegGetData(const char *segName)
{
	return romdataGetSeg(segName)->data;
}

u8 *romdataSegGetDataEnd(const char *segName)
{
	struct romfile *seg = romdataGetSeg(segName);
	return seg->data + seg->size;
}

u32 romdataSegGetSize(const char *segName)
{
	return romdataGetSeg(segName)->size;
}

u32 romdataFileGetEstimatedSize(const u32 size, const u32 loadtype)
{
#ifdef PLATFORM_64BIT
	switch (loadtype) {
	case LOADTYPE_BG:	 return (u32)(size * 1.1f);
	case LOADTYPE_TILES: return (u32)(size * 1.1f);
	case LOADTYPE_LANG:  return (u32)(size * 1.3f);
	case LOADTYPE_SETUP: return (u32)(size * 1.5f);
	case LOADTYPE_PADS:  return (u32)(size * 1.7f);
	case LOADTYPE_MODEL: return (u32)(size * 1.7f);
	case LOADTYPE_GUN: return (u32)(size * 1.7f);
	default:
		sysLogPrintf(LOG_WARNING, "romdataFileGetEstimatedSize: wrong loadtype %d", loadtype);
	}
#else
	if (loadtype == LOADTYPE_MODEL) {
		return (u32)(size * 1.1f);
	}
#endif
	return size;
}
