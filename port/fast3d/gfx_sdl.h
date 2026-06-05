#ifndef GFX_SDL_H
#define GFX_SDL_H

#include "gfx_window_manager_api.h"

#ifdef __cplusplus
extern "C" {
#endif

extern struct GfxWindowManagerAPI gfx_sdl;

// Select the rendering backend the window is created for BEFORE gfx_init:
// 0 = OpenGL (default; window gets a GL context), 1 = SDL_GPU (plain window,
// no GL context — the renderer creates the device and owns presentation).
void gfx_sdl_set_backend(int gpu);

#ifdef __cplusplus
}
#endif

#endif
