#ifndef GFX_RETRO_H
#define GFX_RETRO_H

/**
 * Chaos "retro" post filter (pd.pixelate; docs/PORT_CHAOS.md). Called by
 * gfx_opengl.cpp's retro_filter rapi entry once per frame, after the final
 * flush, to pixelate + colour-crush the finished frame in place. Saves and
 * restores all GL state it touches. GL backend only.
 *
 * colors: 0 = keep colours (pixelate only), 2..64 = N-level greyscale,
 * >= 256 = RGB 3-3-2 (256 displayable colours).
 */
void gfx_retro_filter(int pixw, int pixh, int colors, unsigned int fbo, int fbw, int fbh,
                      const char* glsl_version);

#endif
