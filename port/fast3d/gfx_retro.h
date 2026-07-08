#ifndef GFX_RETRO_H
#define GFX_RETRO_H

/**
 * Chaos retro/post filter (pd.pixelate / pd.crt / pd.lens / pd.screen_fx;
 * docs/PORT_CHAOS.md). Called by gfx_opengl.cpp's retro_filter rapi entry
 * once per frame, after the final flush, to filter the finished frame in
 * place. Saves and restores all GL state it touches. This is the GL
 * implementation; the SDL_GPU twin lives in gfx_sdlgpu.cpp's retro section,
 * sharing the fragment body via gfx_retro_common.h.
 *
 * cmode/clevels are the pre-mapped colour mode (see gfx_retro_common.h);
 * fx is the effect bitmask; warp the fisheye strength. gfx_pc.cpp's
 * dispatcher does the gfx_retro_colors -> (cmode, clevels) mapping.
 */
void gfx_retro_filter(int pixw, int pixh, int cmode, int clevels, int fx, float warp,
                      unsigned int fbo, int fbw, int fbh, const char* glsl_version);

#endif
