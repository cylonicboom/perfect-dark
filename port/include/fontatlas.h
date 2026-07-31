#ifndef _IN_FONTATLAS_H
#define _IN_FONTATLAS_H

#include <PR/ultratypes.h>

/*
 * Font glyph atlas (opt round-2 item A12).
 *
 * At font load (textLoadFont) each font's glyph bitmaps are copied into one
 * CI4 atlas image so text rendering can emit plain texrects with atlas UV
 * offsets instead of a SetTextureImage + LoadBlock per glyph. The atlas is a
 * byte-for-byte repack of the original 16-texel-wide CI4 glyph rows, so it
 * feeds the fast3d texture cache through the ordinary SetTextureImage /
 * LoadTile / SetTile commands (no renderer changes): the cache keys on the
 * stable atlas pointer, so the whole font imports once per texture-cache
 * generation.
 *
 * Memory comes from MEMPOOL_STAGE, matching the font's own lifetime; the
 * per-stage texReset() -> videoResetTextureCache() clear (lv.c stage load)
 * runs before any text renders, so a stale atlas pointer can never reach the
 * renderer. textReset() calls fontAtlasResetAll() before reloading fonts.
 */

struct font;
struct fontchar;

#define FONTATLAS_MAX_GLYPHS 135 /* PAL fonts; 94 elsewhere */
#define FONTATLAS_MAX_FONTS  8

struct fontatlas {
	struct font *font;
	struct fontchar *chars;
	s32 numchars;
	u8 *pixels;  /* CI4 texel data, (width / 2) bytes per row; NULL = build failed */
	s32 width;   /* texels; multiple of 16 so the CI4 tile "line" field is exact */
	s32 height;  /* texel rows */
	s32 hasext;  /* font has ext_tex per-glyph replacements; skip atlas while enabled */
	u16 glyphs_s[FONTATLAS_MAX_GLYPHS]; /* cell content origin, texels; 0xffff = not atlased */
	u16 glyphs_t[FONTATLAS_MAX_GLYPHS];
};

/* Master toggle (parent session wires the /fontatlas console command) */
extern s32 g_FontAtlasEnabled;

/* Forget all atlases (stage memory is about to be reused). */
void fontAtlasResetAll(void);

/* Build an atlas for a freshly loaded font. Failure is silent: the font
 * simply renders through the per-glyph path. */
void fontAtlasRegister(struct font *font, struct fontchar *chars, s32 numchars);

/* Atlas to use for a string in the given font, or NULL to use the per-glyph
 * path (disabled, unknown font, build failed, or ext_tex replacements live). */
struct fontatlas *fontAtlasForString(struct font *font);

/* Look up a glyph's atlas cell. Returns 1 and writes the cell content origin
 * (texels) on success; 0 if the glyph is not in this atlas (e.g. the JPN
 * dynamic glyphs, which are stack/lazily-generated fontchars). */
s32 fontAtlasLookup(struct fontatlas *atl, struct fontchar *chr, s32 *s, s32 *t);

#endif
