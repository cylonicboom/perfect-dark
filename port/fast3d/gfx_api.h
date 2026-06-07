#ifndef GFX_API_H
#define GFX_API_H

#ifndef __cplusplus
#include <stdint.h>
#include <stdbool.h>
#endif

#include "gfx_rendering_api.h"
#include "gfx_window_manager_api.h"

struct XYWidthHeight {
    int16_t x, y;
    uint32_t width, height;
};

struct GfxDimensions {
    float internal_mul;
    uint32_t width, height;
    float aspect_ratio;
};

struct GfxInitSettings {
    struct GfxWindowManagerAPI *wapi;
    struct GfxRenderingAPI *rapi;
    struct GfxWindowInitSettings window_settings;
};

extern struct GfxDimensions gfx_current_window_dimensions; // The dimensions of the window
extern struct GfxDimensions
    gfx_current_dimensions; // The dimensions of the draw area the game draws to, before scaling (if applicable)
extern struct XYWidthHeight
    gfx_current_game_window_viewport; // The area of the window the game is drawn to, (0, 0) is top-left corner
extern uint32_t gfx_msaa_level;
extern struct XYWidthHeight gfx_current_native_viewport; // The internal/native video mode of the game
extern float gfx_current_native_aspect; // The aspect ratio of the above mode
extern bool gfx_framebuffers_enabled;
extern bool gfx_detail_textures_enabled;
extern bool gfx_wireframe_mode;
extern bool gfx_mirror_mode;                 // flip the 3D scene left-right (CHEAT_MIRROR)
// HDR dazzle weight 0..1 set by the G_SETDAZZLE_EXT display-list command
// (flush-aligned): draws issued while non-zero are emissive-boosted toward
// the HDR peak by the SDL_GPU backend. GL and SDR ignore it.
extern float gfx_hdr_dazzle;
extern int gfx_wireframe_wire_color_enabled; // 0 = natural/textured wires
extern float gfx_wireframe_wire_color[3];    // flat wire colour, 0..1 RGB
extern float gfx_wireframe_line_width;        // wire thickness in pixels
extern bool gfx_external_textures_enabled;   // data/ext_tex PNG substitution (rafccq/port-ext-textures)

void gfx_init(const struct GfxInitSettings *settings);
void gfx_destroy(void);
struct GfxRenderingAPI* gfx_get_current_rendering_api(void);
void gfx_start_frame(void);
void gfx_run(Gfx* commands);
void gfx_end_frame(void);
void gfx_set_target_fps(int);
void gfx_set_texture_filter(enum FilteringMode mode);
void gfx_set_mipmap_filter(enum MipmapFilteringMode mode);
void gfx_texture_cache_clear(void);
void gfx_texture_cache_delete(const uint8_t *orig_addr);
void gfx_texture_cache_delete_range(const uint8_t *start, const uint8_t *end);
int gfx_create_framebuffer(uint32_t width, uint32_t height, int upscale, int autoresize);
void gfx_resize_framebuffer(int fb, uint32_t width, uint32_t height, int upscale, int autoresize);
void gfx_set_framebuffer(int fb, float noise_scale) ;
void gfx_reset_framebuffer(void);
void gfx_copy_framebuffer(int fb_dst, int fb_src, int left, int top, int use_back);

// Display-list cache (port-only; see docs/PORT_DLCACHE.md). Driven by /dlcache.
// Abort-reason bits returned in gfx_dlcache_get_stats(reasons) — why leaves fell
// back to legacy (the un-bakeable / unsupported state encountered while recording).
#define GFX_DLC_ABORT_FOG      0x01
#define GFX_DLC_ABORT_LIGHTING 0x02
#define GFX_DLC_ABORT_CULLBOTH 0x04
#define GFX_DLC_ABORT_EMPTY    0x08
#define GFX_DLC_ABORT_TEXGEN   0x10
void gfx_dlcache_clear(void);
void gfx_dlcache_set_frontface(int ccw);
int gfx_dlcache_get_frontface(void);
void gfx_dlcache_set_cullmode(int mode); // 0 auto, 1 off, 2 force-back, 3 force-front
int gfx_dlcache_get_cullmode(void);
void gfx_dlcache_get_stats(uint32_t *entries, uint32_t *bad, uint32_t *segments, uint32_t *tris, uint32_t *reasons);

#endif
