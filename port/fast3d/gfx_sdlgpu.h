#ifndef GFX_SDLGPU_H
#define GFX_SDLGPU_H

#include "gfx_rendering_api.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef USE_SDLGPU

extern struct GfxRenderingAPI gfx_sdlgpu_api;

// Probe-create the SDL_GPU device (kept around and reused by the subsequent
// rapi init). Returns 0 if SDL_GPU/Vulkan is unavailable on this system so
// the caller can fall back to the OpenGL backend before any window exists.
int gfx_sdlgpu_probe(void);

// Vsync forward from the SDL window manager: with SDL_GPU the renderer owns
// the swapchain, so SDL_GL_SetSwapInterval does not apply. 0 = immediate,
// >0 = vsync (intervals > 1 are paced by the frame limiter), <0 = mailbox
// where supported.
void gfx_sdlgpu_set_vsync(int interval);

// One-line diagnostics (driver, shader format, msaa, vsync, shader cache)
// for the /gpu console command.
void gfx_sdlgpu_get_info(char *buf, unsigned int len);

// Default GPU driver when --gpu-driver isn't passed (the Video.GpuDriver
// config); empty = platform default. Call before gfx_sdlgpu_probe.
void gfx_sdlgpu_set_driver_default(const char *drv);

// Request HDR output (Video.HDR + Video.HDRPaperWhite/HDRPeak) — applied at
// init: scRGB or HDR10 swapchain + FP16 internal targets + a present pass
// mapping diffuse content to paperwhite nits and expanding near-white
// highlights toward peak nits. Call before gfx_init.
void gfx_sdlgpu_request_hdr(int enable, float paperwhite_nits, float peak_nits);

// Live adjustments (nits; scRGB 1.0 = 80). No-ops unless HDR is active this
// session. Peak <= paperwhite disables highlight expansion.
void gfx_sdlgpu_set_hdr_paperwhite(float nits);
void gfx_sdlgpu_set_hdr_peak(float nits);

#endif // USE_SDLGPU

#ifdef __cplusplus
}
#endif

#endif
