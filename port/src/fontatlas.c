/*
 * Font glyph atlas builder (opt round-2 item A12). See fontatlas.h for the
 * overview and game_1531a0.c for the render-side integration.
 *
 * Glyph bitmap format (from the original per-glyph loads in game_1531a0.c):
 * each fontchar's pixeldata is CI4, 16 texels (8 bytes) per row, height + 2
 * rows (the original LoadBlock loads ((height * 8 + 17) >> 1) 16-bit units =
 * (height + 2) * 8 bytes). Row/column 0 are transparent border (palette
 * index 0 is transparent in every font TLUT), and the texrects sample at
 * most width + 2 <= 16 columns.
 *
 * Atlas layout: fixed 18-texel cell pitch = 2 zero gutter columns + the
 * 16-texel glyph row; shelves of 16 cells with 1 zero gutter row between
 * them. The gutters keep bilinear taps (up to 1 texel outside the sampled
 * rect, both from the +0.5 tap and the port's hi-res subpixel sampling) on
 * transparent texels, matching what the per-glyph path samples at glyph
 * edges. Cells start on even texels so the CI4 copy stays byte-aligned.
 */

#include <string.h>
#include <stdint.h>

#include "gbiex.h"
#include "types.h"

#include "constants.h"
#include "lib/memp.h"
#include "ext_tex.h"
#include "fontatlas.h"

/* fast3d global (gfx_pc.cpp, compiled in every build incl. dedicated);
 * C-side declaration follows the pdmain.c precedent for the bool ABI seam */
extern unsigned char gfx_external_textures_enabled;

s32 g_FontAtlasEnabled = 1;

#define ATLAS_COLS   16
#define ATLAS_CELLW  18                        /* 2 gutter + 16 content texels */
#define ATLAS_W      (ATLAS_COLS * ATLAS_CELLW) /* 288 texels; multiple of 16 */
#define ATLAS_WBYTES (ATLAS_W / 2)              /* CI4: 2 texels per byte */
#define ATLAS_MAXH   1023                       /* LoadTile lrt is 10.2 in 12 bits */

#define GLYPH_WTEXELS 16
#define GLYPH_WBYTES  8

static struct fontatlas g_FontAtlases[FONTATLAS_MAX_FONTS];
static s32 g_FontAtlasCount = 0;

void fontAtlasResetAll(void)
{
	/* pixel buffers live in MEMPOOL_STAGE and die with the stage */
	memset(g_FontAtlases, 0, sizeof(g_FontAtlases));
	g_FontAtlasCount = 0;
}

static s32 fontAtlasFontHasExtGlyphs(struct font *font, s32 numchars)
{
	u8 fontid = extTexFontID(font);
	s32 i;

	if (fontid == 0xff) {
		return 0;
	}

	for (i = 0; i < numchars; i++) {
		if (extTexExists(G_TEXTYPE_FONT, fontid, i)
				|| extTexExists(G_TEXTYPE_FONT, (u16)(fontid | (MASK_FONT_OUTLINE << 8)), i)) {
			return 1;
		}
	}

	return 0;
}

void fontAtlasRegister(struct font *font, struct fontchar *chars, s32 numchars)
{
	struct fontatlas *atl = NULL;
	s32 i;
	s32 row;
	s32 ty;
	u32 bytes;

	if (font == NULL || chars == NULL || numchars <= 0 || numchars > FONTATLAS_MAX_GLYPHS) {
		return;
	}

	/* reuse an entry if this font pointer was registered before, else claim a new one */
	for (i = 0; i < g_FontAtlasCount; i++) {
		if (g_FontAtlases[i].font == font) {
			atl = &g_FontAtlases[i];
			break;
		}
	}

	if (atl == NULL) {
		if (g_FontAtlasCount >= FONTATLAS_MAX_FONTS) {
			return;
		}

		atl = &g_FontAtlases[g_FontAtlasCount++];
	}

	memset(atl, 0, sizeof(*atl));
	atl->font = font;
	atl->chars = chars;
	atl->numchars = numchars;
	atl->hasext = fontAtlasFontHasExtGlyphs(font, numchars);

	/* shelf layout: rows of ATLAS_COLS cells, shelf height = tallest glyph
	 * in the row + 2 border rows, 1 zero gutter row above each shelf */
	ty = 0;

	for (row = 0; row * ATLAS_COLS < numchars; row++) {
		s32 base = row * ATLAS_COLS;
		s32 maxh = 0;
		s32 col;

		for (col = 0; col < ATLAS_COLS && base + col < numchars; col++) {
			struct fontchar *chr = &chars[base + col];

			if (chr->pixeldata != NULL && chr->height + 2 > maxh) {
				maxh = chr->height + 2;
			}
		}

		ty++; /* gutter row above the shelf */

		for (col = 0; col < ATLAS_COLS && base + col < numchars; col++) {
			if (chars[base + col].pixeldata != NULL) {
				atl->glyphs_s[base + col] = col * ATLAS_CELLW + 2;
				atl->glyphs_t[base + col] = ty;
			} else {
				/* JPN dynamic glyph slot; renders through the per-glyph path */
				atl->glyphs_s[base + col] = 0xffff;
				atl->glyphs_t[base + col] = 0xffff;
			}
		}

		ty += maxh;
	}

	ty++; /* trailing gutter row */

	if (ty > ATLAS_MAXH) {
		atl->pixels = NULL; /* overflow: whole font falls back to per-glyph */
		return;
	}

	atl->width = ATLAS_W;
	atl->height = ty;

	bytes = (u32)ATLAS_WBYTES * (u32)ty;
	atl->pixels = mempAlloc(ALIGN16(bytes), MEMPOOL_STAGE);

	if (atl->pixels == NULL) {
		return;
	}

	memset(atl->pixels, 0, bytes); /* CI4 index 0 = transparent in the font TLUTs */

	/* byte-copy each glyph's CI4 rows into its cell (no format conversion:
	 * the atlas is the same CI4 data the per-glyph LoadBlock consumed) */
	for (i = 0; i < numchars; i++) {
		struct fontchar *chr = &chars[i];
		s32 rows;
		s32 r;
		u8 *dst;

		if (chr->pixeldata == NULL || atl->glyphs_s[i] == 0xffff) {
			continue;
		}

		rows = chr->height + 2;
		dst = atl->pixels + (u32)atl->glyphs_t[i] * ATLAS_WBYTES + (atl->glyphs_s[i] >> 1);

		for (r = 0; r < rows; r++) {
			memcpy(dst + (u32)r * ATLAS_WBYTES, chr->pixeldata + r * GLYPH_WBYTES, GLYPH_WBYTES);
		}
	}
}

struct fontatlas *fontAtlasForString(struct font *font)
{
	s32 i;

	if (!g_FontAtlasEnabled || font == NULL) {
		return NULL;
	}

	for (i = 0; i < g_FontAtlasCount; i++) {
		struct fontatlas *atl = &g_FontAtlases[i];

		if (atl->font == font) {
			if (atl->pixels == NULL) {
				return NULL;
			}

			/* live per-glyph PNG replacements can't be pre-atlased; simplest
			 * correct behaviour is whole-font fallback while they're active */
			if (atl->hasext && gfx_external_textures_enabled) {
				return NULL;
			}

			return atl;
		}
	}

	return NULL;
}

s32 fontAtlasLookup(struct fontatlas *atl, struct fontchar *chr, s32 *s, s32 *t)
{
	uintptr_t off;
	u32 idx;

	if (atl == NULL || chr == NULL) {
		return 0;
	}

	/* membership by address: JPN dynamic glyphs are stack fontchars and land
	 * outside the font's chars table, falling through to the per-glyph path */
	off = (uintptr_t)chr - (uintptr_t)atl->chars;

	if (off % sizeof(struct fontchar) != 0) {
		return 0;
	}

	idx = off / sizeof(struct fontchar);

	if (idx >= (u32)atl->numchars) {
		return 0;
	}

	if (atl->glyphs_s[idx] == 0xffff) {
		return 0;
	}

	*s = atl->glyphs_s[idx];
	*t = atl->glyphs_t[idx];
	return 1;
}
