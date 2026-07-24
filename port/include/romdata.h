#ifndef _IN_ROMDATA_H
#define _IN_ROMDATA_H

#include <PR/ultratypes.h>

extern u8 *g_RomFile;
extern u32 g_RomFileSize;
extern const char *g_RomName;
extern s32 g_ChainRomActive;

// Chaos live model swap (see romdata.c). g_ModelRomActive: an overlay ROM is
// loaded. g_ModelSwapActive: character models are currently sourced from it.
// g_ModelSwapFiles[fileNum] != 0 flags a file for redirection. The game side
// (body.c modelSwapSetActive) owns these.
extern s32 g_ModelRomActive;
extern s32 g_ModelSwapActive;
extern u8 g_ModelSwapFiles[];
extern s32 g_ModelSwapRedirects; // # of times the redirect served overlay bytes
extern s32 g_ModelSwapMisses;    // # of times it was armed but couldn't serve
// Overlay TEXTURE table/data (built when the overlay ROM loads) + the flag that
// gates per-number texture redirection while a swapped model's textures load.
struct texture;
extern struct texture *g_ModelSwapTexList; // overlay texture table (dataoffsets)
extern s32 g_ModelSwapTexCount;            // entries in g_ModelSwapTexList
extern u8 *g_ModelSwapTexData;             // overlay texturesdata base
extern s32 g_ModelSwapTexActive;           // set only while a swapped model loads
s32 romdataChainFileGetNumForName(const char *name);
s32 romdataLoadModelRom(const char *path); // path = ROM file or dir to scan; 1 = loaded

s32 romdataInit(void);

u8 *romdataFileLoad(s32 fileNum, u32 *outSize);
void romdataFilePreprocess(s32 fileNum, s32 loadType, u8 *data, u32 size, u32 *outSize);
void romdataFileFree(s32 fileNum);
void romdataFileFreeForSolo(void); // All Solos in Multi Mod
const char *romdataFileGetName(s32 fileNum);

u8 *romdataFileGetData(s32 fileNum);
s32 romdataFileGetSize(s32 fileNum);

s32 romdataFileGetNumForName(const char *name);

u8 *romdataSegGetData(const char *segName);
u8 *romdataSegGetDataEnd(const char *segName);
u32 romdataSegGetSize(const char *segName);
u32 romdataFileGetEstimatedSize(const u32 size, const u32 loadtype);

s32 romdataCheckGbcRom(void);

#endif
