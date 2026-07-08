#define NOMINMAX

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cassert>
#include <cstdio>

#include <map>
#include <set>
#include <unordered_map>
#include <vector>
#include <list>
#include <stack>
#include <string>
#include <iostream>
#include <memory>
#include <limits>

#ifndef _LANGUAGE_C
#define _LANGUAGE_C
#endif
#include <PR/gbi.h>

#include "platform.h"

#include "gfx_pc.h"
#include "gfx_cc.h"
#include "gfx_window_manager_api.h"
#include "gfx_rendering_api.h"
#include "gfx_screen_config.h"
#include "rt_ext.h"

extern "C" {
#include "ext_tex.h"
}

uintptr_t gfxFramebuffer;

#define ALIGN(x, a) (((x) + (a - 1)) & ~(a - 1))

#define SUPPORT_CHECK(x) assert(x)

// SCALE_M_N: upscale/downscale M-bit integer to N-bit
#define SCALE_5_8(VAL_) (((VAL_)*0xFF) / 0x1F)
#define SCALE_8_5(VAL_) ((((VAL_) + 4) * 0x1F) / 0xFF)
#define SCALE_4_8(VAL_) ((VAL_)*0x11)
#define SCALE_8_4(VAL_) ((VAL_) / 0x11)
#define SCALE_3_8(VAL_) ((VAL_)*0x24)
#define SCALE_8_3(VAL_) ((VAL_) / 0x24)

// SCREEN_WIDTH and SCREEN_HEIGHT are defined in the headerfile
#define HALF_SCREEN_WIDTH (SCREEN_WIDTH / 2.f)
#define HALF_SCREEN_HEIGHT (SCREEN_HEIGHT / 2.f)

#define RATIO_X (gfx_current_dimensions.width / (float)SCREEN_WIDTH)
#define RATIO_Y (gfx_current_dimensions.height / (float)SCREEN_HEIGHT)

#define MAX_BUFFERED 256
#define MAX_LIGHTS 4
#define MAX_VERTICES 128
#define MAX_VERTEX_COLORS 64

// Max distinct textures held in the GPU texture cache. This is a COUNT cap, not a
// memory cap (the "I set 256MB" knob doesn't change it). 1024 is an N64-era value;
// AIO HD-texture sets blow past it, and with /dlcache on every eviction clears the
// whole display-list cache (dlcacheInvalidateAll) -> re-record churn -> within-frame
// texture thrash -> textures bind to evicted/reused ids and go black, worsening as
// the visible working set grows. Raised so a level's HD working set fits without
// eviction; memory is still bounded by the textures actually loaded. (fast3d is
// desktop-only - there is no N64 build of this file.)
#define TEXTURE_CACHE_DEFAULT_SIZE 4096

// Runtime-adjustable (live: /texcache N). The dlcache RECORDER additionally
// imports a leaf's OFF-screen textures (clip-reject is disabled while recording,
// gfx_sp_tri1), so with HD packs the working set can exceed the fixed cap and
// LRU-evict on-screen textures -> they sample black, and the eviction churns the
// dlcache (PORT_DLCACHE.md). Raise this until the level's working set fits without
// eviction; watch the fill via /texcache or /dlcache stats (tex=used/max).
static int g_TextureCacheMaxSize = TEXTURE_CACHE_DEFAULT_SIZE;

#define C0(pos, width) ((cmd->words.w0 >> (pos)) & ((1U << width) - 1))
#define C1(pos, width) ((cmd->words.w1 >> (pos)) & ((1U << width) - 1))

struct RGBA {
    uint8_t r, g, b, a;
};

struct NormalColor {
    union {
        struct { uint8_t r, g, b, a; };
        struct { int8_t x, y, z, w; };
    };
};

struct LoadedVertex {
    float x, y, z, w;
    float u, v;
    struct RGBA color;
    uint8_t fog;
    uint8_t clip_rej;
    float ox, oy, oz; // object-space position, for the display-list cache recorder
    float colour_index; // palette index (v->colour>>2), for the shader-side GPU palette
};

static struct {
    TextureCacheMap map;
    std::list<TextureCacheMapIter> lru;
    std::vector<uint32_t> free_texture_ids;
} gfx_texture_cache;

struct ColorCombiner {
    uint64_t shader_id0;
    uint32_t shader_id1;
    bool used_textures[2];
    struct ShaderProgram* prg[16];
    uint8_t shader_input_mapping[2][7];
};

static std::map<ColorCombinerKey, struct ColorCombiner> color_combiner_pool;
static std::map<ColorCombinerKey, struct ColorCombiner>::iterator prev_combiner = color_combiner_pool.end();

static uint8_t* tex_upload_buffer = nullptr;

static struct RSP {
    float modelview_matrix_stack[11][4][4];
    uint8_t modelview_matrix_stack_size;

    float MP_matrix[4][4];
    float P_matrix[4][4];

    Light_t lookat[2];
    bool lookat_enabled;

    Light_t current_lights[MAX_LIGHTS + 1];
    float current_lights_coeffs[MAX_LIGHTS][3];
    float current_lookat_coeffs[2][3]; // lookat_x, lookat_y
    uint8_t current_num_lights;        // includes ambient light
    bool lights_changed;

    uint32_t geometry_mode;
    int16_t fog_mul, fog_offset;

    uint32_t extra_geometry_mode;

    uint32_t aspect_mode;
    float aspect_ofs;
    float aspect_scale;

    struct {
        // U0.16
        uint16_t s, t;
    } texture_scaling_factor;

    struct LoadedVertex loaded_vertices[MAX_VERTICES + 4];

    const struct NormalColor *vertex_colors; //[MAX_VERTEX_COLORS];
} rsp;

struct RawTexMetadata {
    uint16_t width, height;
    float h_byte_scale = 1, v_pixel_scale = 1;
};

struct LoadedTexture {
    const uint8_t* addr;
    uint32_t orig_size_bytes;
    uint32_t full_size_bytes; // full_image_line_size_bytes * height
    uint32_t size_bytes; // line_size_bytes * height
    uint32_t full_image_line_size_bytes;
    uint32_t line_size_bytes;
    uint32_t tex_flags;
    // external-texture tag (rafccq/port-ext-textures): set by G_SETTEXINFO_EXT
    // via gfx_dp_load_block when a data/ext_tex replacement exists; ext_key's
    // top byte flags "external" and keys the texture cache instead of addr
    uint64_t ext_key;
    uint8_t type;
    uint16_t id;
    uint16_t id_mask;
    uint32_t texnum;
    struct RawTexMetadata raw_tex_metadata;
};

static struct RDP {
    uint16_t palette[256];
    const uint8_t* palette_addrs[2];
    uint32_t palette_fmt;
    struct {
        const uint8_t* addr;
        uint8_t siz;
        uint32_t width;
        uint32_t tex_flags;
        // pending G_SETTEXINFO_EXT tag for the next load (ext_tex)
        uint8_t type;
        uint16_t id;
        uint16_t id_mask;
        uint32_t texnum;
        struct RawTexMetadata raw_tex_metadata;
    } texture_to_load;
    struct {
        uint8_t fmt;
        uint8_t siz;
        uint8_t cms, cmt;
        uint8_t shifts, shiftt;
        uint16_t uls, ult, lrs, lrt; // U10.2
        uint16_t width, height;      // in texels
        uint16_t tmem;               // 0-511, in 64-bit word units
        uint32_t line_size_bytes;
        uint8_t palette;
    } texture_tile[8];
    LoadedTexture loaded_texture[512]; // for each tmem location
    bool textures_changed[2];

    uint8_t first_tile_index;
    uint8_t tex_min_lod;
    uint8_t tex_max_lod;

    uint32_t other_mode_l, other_mode_h;
    uint64_t combine_mode;
    bool grayscale;
    bool tex_lod;
    bool tex_detail;

    uint8_t prim_lod_fraction;
    struct RGBA env_color, prim_color, fog_color, fill_color, grayscale_color;
    struct XYWidthHeight viewport, scissor;
    bool viewport_or_scissor_changed;
    void* z_buf_address;
    void* color_image_address;

    int16_t subpixel_ofs_x;
    int16_t subpixel_ofs_y;
} rdp;

static struct RenderingState {
    uint8_t depth_mode;
    bool alpha_blend;
    bool modulate;
    struct XYWidthHeight viewport, scissor;
    struct ShaderProgram* shader_program;
    TextureCacheNode* textures[SHADER_MAX_TEXTURES];
} rendering_state;

struct GfxDimensions gfx_current_window_dimensions;
int32_t gfx_current_window_position_x;
int32_t gfx_current_window_position_y;
struct GfxDimensions gfx_current_dimensions;
static struct GfxDimensions gfx_prev_dimensions;
struct XYWidthHeight gfx_current_game_window_viewport;
struct XYWidthHeight gfx_current_native_viewport;
float gfx_current_native_aspect = 4.f / 3.f;
bool gfx_framebuffers_enabled = true;
bool gfx_detail_textures_enabled = true;
bool gfx_wireframe_mode = false;

// Screen-space raytracing suite controls (docs/PORT_RAYTRACING.md). Defined
// here — not in gfx_rt.cpp — so they exist on every build, including the
// dedicated server (which drops gfx_opengl.cpp/gfx_rt.cpp but still links
// net.c's /rt console command). Declared in port/include/rt_ext.h.
int gfx_rt_enabled = 0;
int gfx_rt_ao = 1;
int gfx_rt_shadows = 0; // screen-space sun shadows: stylized, off by default
int gfx_rt_ssr = 1;
int gfx_rt_gi = RT_GI_SSGI;
int gfx_rt_debug = RT_DEBUG_OFF;
int gfx_rt_quality = 1;
float gfx_rt_ao_intensity = 0.7f;
float gfx_rt_ao_radius = 45.0f;       // PD world units (~100/metre)
float gfx_rt_shadow_intensity = 0.5f;
float gfx_rt_shadow_length = 120.0f;
float gfx_rt_ssr_intensity = 0.6f;
float gfx_rt_gi_intensity = 1.0f;
float gfx_rt_gi_scale = 0.5f;
float gfx_rt_sun_dir[3] = { 0.35f, 0.85f, 0.40f }; // world-space, toward light
float gfx_rt_sky[3] = { 0.18f, 0.20f, 0.26f };     // miss radiance for GI rays
// dark / relight mode (docs/PORT_RAYTRACING.md "Dark mode")
int gfx_rt_dark = 0;
float gfx_rt_dark_ambient = 0.08f;
int gfx_rt_lights = 1;              // harvested only while RT runs; costs nothing otherwise
int gfx_rt_light_shadows = 1;
float gfx_rt_light_intensity = 1.0f;
float gfx_rt_light_radius = 700.0f; // PD world units (~7 m)
int gfx_rt_torch = 0;
float gfx_rt_torch_intensity = 1.4f;
float gfx_rt_torch_range = 1400.0f;
bool gfx_mirror_mode = false;
// Chaos flat-texture mode (docs/PORT_CHAOS.md): 0 = off; 1 = white out texel
// RGB (the combiner multiplies TEXEL*SHADE, so this leaves pure vertex
// shading); 2 = flood texel RGB with the texture's average colour. Per-pixel
// alpha is preserved either way, so font glyphs / HUD icons / cutout textures
// keep their shapes. Toggling is applied in gfx_start_frame via a texture
// cache clear, which re-imports everything through gfx_upload_tex_filtered.
int gfx_flattex_mode = 0;
// Chaos forced grayscale: drives rdp.grayscale with a neutral colour from
// gfx_start_frame (the game itself never emits G_SETGRAYSCALE_EXT, so there
// is no mid-frame contention).
int gfx_force_grayscale = 0;
// Chaos forced-shiny mode (docs/PORT_CHAOS.md): 0 = off; 1 = every 3D vertex's
// texture coords are replaced with a screen-space projection (PS1-style fake
// chrome — the texture is glued to the screen so it slides over surfaces as
// the view moves, which reads as "everything is reflective"); 2 = the same
// plus a gold tint via the grayscale shader path (luminance * gold). 2D
// texrects (HUD/text) don't pass through gfx_sp_vertex and are unaffected;
// menu geometry marked G_NOMIRROR_EXT is exempted like the mirror flip.
// bg.c gates dlcache off while this is active (cached rooms replay recorded
// UVs, so they would stay matte while everything else shines).
int gfx_shiny_mode = 0;
// Chaos upside-down mode (pd.upside_down): negate clip-space Y — the exact
// sibling of gfx_mirror_mode's X flip (winding compensated in gfx_sp_tri1,
// scissor reflected about the viewport's vertical centre, G_NOMIRROR_EXT
// geometry exempt). bg.c gates dlcache off while active (cached replay's
// uMVP is not Y-flipped), the gfx_shiny_mode pattern.
bool gfx_upsidedown_mode = false;
// Chaos screen tint (pd.screen_tint): 0x00RRGGBB, 0 = off. Rides the
// grayscale shader path (luminance * tint) like the Midas gold mode.
int gfx_screen_tint = 0;
float gfx_hdr_dazzle = 0.0f; // G_SETDAZZLE_EXT weight; see gfx_api.h
int gfx_wireframe_wire_color_enabled = 0;
float gfx_wireframe_wire_color[3] = {1.0f, 1.0f, 1.0f};
float gfx_wireframe_line_width = 1.0f;
bool gfx_external_textures_enabled = false; // data/ext_tex PNG substitution

static bool game_renders_to_framebuffer;
static int game_framebuffer;
static int game_framebuffer_msaa_resolved;

uint32_t gfx_msaa_level = 1;

static bool dropped_frame;

static float buf_vbo[MAX_BUFFERED * (32 * 3)]; // 3 vertices in a triangle and 32 floats per vtx
static size_t buf_vbo_len;
static size_t buf_vbo_num_tris;

static struct GfxWindowManagerAPI* gfx_wapi;
static struct GfxRenderingAPI* gfx_rapi;

static uintptr_t segmentPointers[16];

struct FBInfo {
    uint32_t orig_width, orig_height;
    uint32_t applied_width, applied_height;
    bool upscale, autoresize;
};

static bool fbActive = 0;
static std::map<int, FBInfo>::iterator active_fb;
static std::map<int, FBInfo> framebuffers;

static constexpr float clampf(const float x, const float min, const float max) {
    return (x < min) ? min : (x > max) ? max : x;
}

// ======================== Display-list cache ========================
// Port-only: cache static room leaf geometry in persistent GPU buffers and
// replay it with a GPU-side uMVP instead of CPU-transforming every vertex each
// frame. Keyed by the leaf gdl pointer. See docs/PORT_DLCACHE.md.

struct DlCacheSegment {
    struct ShaderProgram* prg;
    uint32_t tex_id[2];
    // The owning texture-cache node, for refreshing the LRU at replay. A live
    // segment's texture is guaranteed still in the cache (any eviction/delete calls
    // dlcacheInvalidateAll, which drops every segment), so this never dangles.
    // Without the refresh, textures shown ONLY via cached replay never touch the LRU,
    // drift to the front, and get evicted while on screen -> invalidate churn -> black.
    TextureCacheNode* tex_node[2];
    bool tex_used[2];
    bool tex_linear[2];
    uint8_t tex_cms[2], tex_cmt[2];
    bool tex_lod;
    bool depth_test, depth_update, depth_compare, depth_source_prim;
    uint16_t zmode;
    struct XYWidthHeight viewport, scissor;
    bool alpha_blend, modulate;
    int cull; // 0 none, 1 cull-back, 2 cull-front
    bool fog_compute; // G_FOG set -> shader recomputes distance fog; else baked aFog.a
    float fog_mul, fog_off;
    int shade_route; // shader-side palette: 3 bits/input (rgb type + alpha shade)
    uint32_t state_group; // segments sharing this id share all GL state (for replay dedup/merge)
    int32_t batch_index;  // octree vtxbatch index within the leaf (k-th G_VTX); -1 = none
    size_t base_float; // float offset of this segment's first vertex
    size_t num_tris;
};

struct DlCacheEntry {
    bool bad;   // failed restrictions -> always render legacy
    bool ready; // uploaded and replayable
    uint32_t buffer_id;
    std::vector<DlCacheSegment> segments;
    // Shader-side GPU palette (live dynamic vertex lighting). palette_ok means the
    // leaf had a single G_COL we can resolve live; the per-vertex colour index is in
    // the cached buffer and the palette texture is re-uploaded when colours change.
    bool palette_ok;
    uint32_t palette_tex;
    uintptr_t palette_w1; // raw G_COL w1, re-resolved against the live BG_COL segment
    int palette_count;
};

static std::unordered_map<const void*, DlCacheEntry> g_DlCache;
static bool g_DlCacheRecording;
static bool g_DlCacheAbort;
static DlCacheEntry* g_DlCacheCur;
static std::vector<float> g_DlCacheStaging;
static size_t g_DlCacheSegStartFloat;
static size_t g_DlCacheSegTris;
static int g_DlCacheSegCull;
static bool g_DlCacheSegFogCompute;    // open segment's fog mode (G_FOG distance vs baked)
static int16_t g_DlCacheSegFogMul;
static int16_t g_DlCacheSegFogOff;
static int g_DlCacheSegShadeRoute;     // open segment's shade routing (from the combiner)
// Transient palette capture for the entry being recorded (one G_COL per leaf).
static bool g_DlCacheGColSeen;
static bool g_DlCacheGColMulti;
static uintptr_t g_DlCacheGColW1;
static int g_DlCacheGColCount;
static uint32_t g_DlCacheStateGroup;   // current state-group id (bumped on state/cull change)
static int32_t g_DlCacheBatchIndex;    // current octree vtxbatch index (G_VTX count in the leaf)
static int32_t g_DlCacheSegBatch;      // batch index of the open segment
static bool g_DlCacheFrontCcw = true;   // GL front-face winding for cached cull
// 0 auto (per-segment), 1 off, 2 force-back, 3 force-front. Defaults to AUTO now
// that the "turn around -> rooms missing" bug is fixed (it was a baked SCISSOR, not
// culling). AUTO uses each segment's recorded G_CULL_* mode with the `/dlcache ff`
// winding (default CCW, which testing found correct). If geometry still drops with
// a clean test, `/dlcache cull off` is an instant live escape (draws both faces;
// opaque is z-buffer-identical).
static int g_DlCacheCullMode = 0;
// Diagnostic: when false, cached replay skips the shader-side GPU palette (live
// vertex-shade substitution) and draws with the BAKED record-time shade instead.
// Geometry + textures are still cached, so this isolates "is the bad surface a
// vertex-shading/palette bug or a geometry/texture bake bug?" Lighting goes static
// (no muzzle-flash brightening) while off — that's expected. `/dlcache palette
// on|off`; read live at replay, no re-record. Default on (palette active).
static bool g_DlCachePaletteEnabled = true;
static uint32_t g_DlCacheFrameSegments; // segments replayed last frame
static uint32_t g_DlCacheFrameTris;     // tris replayed last frame
static uint32_t g_DlCacheFrameDraws;    // cache_draw calls issued last frame
// octree-visibility accounting for replayed leaves (vis != NULL only), so the
// octree overlay/stats aren't blind when dlcache handles the culling:
static uint32_t g_DlCacheFrameVisDrawn;    // batches drawn because visible
static uint32_t g_DlCacheFrameVisCulled;   // batches skipped (culled, not drawn)
static uint32_t g_DlCacheFrameVisAbsorbed; // culled batches drawn anyway (gap-merge)
static uint32_t g_DlCacheAbortReasons;  // OR of GFX_DLC_ABORT_* across bad leaves (diagnostic)

// Octree + dlcache interop: gap-tolerant draw merging. An octree-culled batch
// used to split the contiguous-batch merge unconditionally, fragmenting a
// leaf's few large cache_draw calls into many small ones — making octree+
// dlcache SLOWER than dlcache alone (draw-call overhead outweighs the saved
// triangles; culled batches are outside the frustum so drawing them costs
// only vertex shading, no raster). Culled holes up to this many tris are now
// absorbed into the surrounding draw; only bigger culled spans split it.
// 0 = legacy split-on-every-hole. Tune live with /dlcache gap <tris>.
static int g_DlCacheGapTris = 256;

static const float g_DlCacheIdentity[16] = {
    1.f, 0.f, 0.f, 0.f,
    0.f, 1.f, 0.f, 0.f,
    0.f, 0.f, 1.f, 0.f,
    0.f, 0.f, 0.f, 1.f,
};

// Close the open segment, snapshotting the GL state the just-recorded tris used.
static void dlcacheCloseSegment(void) {
    if (g_DlCacheCur == NULL || g_DlCacheSegTris == 0) {
        return;
    }
    DlCacheSegment seg;
    seg.prg = rendering_state.shader_program;
    for (int i = 0; i < 2; i++) {
        TextureCacheNode* n = rendering_state.textures[i];
        seg.tex_used[i] = (n != NULL);
        seg.tex_node[i] = n;
        seg.tex_id[i] = n ? n->second.texture_id : 0;
        seg.tex_linear[i] = n ? n->second.linear_filter : false;
        seg.tex_cms[i] = n ? n->second.cms : 0;
        seg.tex_cmt[i] = n ? n->second.cmt : 0;
    }
    seg.tex_lod = rdp.tex_lod;
    const uint8_t dm = rendering_state.depth_mode; // packed in gfx_sp_tri1
    seg.depth_test = (dm & 1) != 0;
    seg.depth_update = (dm & 2) != 0;
    seg.depth_compare = (dm & 4) != 0;
    seg.depth_source_prim = (dm & 8) != 0;
    seg.zmode = (uint16_t)((dm & 0x30) << 6);
    seg.viewport = rendering_state.viewport;
    seg.scissor = rendering_state.scissor;
    seg.alpha_blend = rendering_state.alpha_blend;
    seg.modulate = rendering_state.modulate;
    seg.cull = g_DlCacheSegCull;
    seg.fog_compute = g_DlCacheSegFogCompute;
    seg.fog_mul = (float)g_DlCacheSegFogMul;
    seg.fog_off = (float)g_DlCacheSegFogOff;
    seg.shade_route = g_DlCacheSegShadeRoute;
    seg.state_group = g_DlCacheStateGroup;
    seg.batch_index = g_DlCacheSegBatch;
    seg.base_float = g_DlCacheSegStartFloat;
    seg.num_tris = g_DlCacheSegTris;
    g_DlCacheCur->segments.push_back(seg);
    g_DlCacheSegStartFloat = g_DlCacheStaging.size();
    g_DlCacheSegTris = 0;
}

// Drop cached entries whose KEY (the leaf gdl pointer) lies inside a freed
// memory range. Called from bgUnloadRoom: room gfxdata is heap-allocated, so a
// NEW room can load at the SAME address as a freed one — its leaf gdl pointers
// then collide with the dead room's cache keys and replay serves the OLD
// room's geometry in the new room's draw slot ("rooms drawn in the incorrect
// slots"). The original "orphaned entries get dropped on the next cache
// clear" assumption is wrong under allocator address reuse.
extern "C" void gfx_dlcache_invalidate_range(const void* start, const void* end) {
    const uintptr_t s = (uintptr_t)start;
    const uintptr_t t = (uintptr_t)end;

    for (auto it = g_DlCache.begin(); it != g_DlCache.end();) {
        const uintptr_t key = (uintptr_t)it->first;

        if (key >= s && key < t) {
            if (it->second.buffer_id != 0) {
                gfx_rapi->cache_delete_buffer(it->second.buffer_id);
            }
            if (it->second.palette_tex != 0) {
                gfx_rapi->cache_delete_palette(it->second.palette_tex);
            }
            it = g_DlCache.erase(it);
        } else {
            ++it;
        }
    }
}

// Drop every cached entry + its GPU buffer (and abort any in-progress record).
// Called on any texture-cache change so stored texture ids can never dangle.
static void dlcacheInvalidateAll(void) {
    if (g_DlCacheRecording) {
        g_DlCacheRecording = false;
        g_DlCacheCur = NULL;
        g_DlCacheAbort = true;
    }
    for (auto& kv : g_DlCache) {
        if (kv.second.buffer_id != 0) {
            gfx_rapi->cache_delete_buffer(kv.second.buffer_id);
        }
        if (kv.second.palette_tex != 0) {
            gfx_rapi->cache_delete_palette(kv.second.palette_tex);
        }
    }
    g_DlCache.clear();
    g_DlCacheAbortReasons = 0;
}

static void gfx_flush(void) {
    if (buf_vbo_len > 0) {
        gfx_rapi->draw_triangles(buf_vbo, buf_vbo_len, buf_vbo_num_tris);
        buf_vbo_len = 0;
        buf_vbo_num_tris = 0;
    }
    // A flush is a render-state boundary: close the open cache segment and start
    // a new state group so the next run is recorded (and replayed) with its own
    // GL state.
    if (g_DlCacheRecording && g_DlCacheSegTris > 0) {
        dlcacheCloseSegment();
        g_DlCacheStateGroup++;
    }
}

static struct ShaderProgram* gfx_lookup_or_create_shader_program(uint64_t shader_id0, uint32_t shader_id1) {
    struct ShaderProgram* prg = gfx_rapi->lookup_shader(shader_id0, shader_id1);
    if (prg == NULL) {
        gfx_rapi->unload_shader(rendering_state.shader_program);
        prg = gfx_rapi->create_and_load_new_shader(shader_id0, shader_id1);
        rendering_state.shader_program = prg;
    }
    return prg;
}

static const char* ccmux_to_string(uint32_t ccmux) {
    static const char* const tbl[] = {
        "G_CCMUX_COMBINED",
        "G_CCMUX_TEXEL0",
        "G_CCMUX_TEXEL1",
        "G_CCMUX_PRIMITIVE",
        "G_CCMUX_SHADE",
        "G_CCMUX_ENVIRONMENT",
        "G_CCMUX_1",
        "G_CCMUX_COMBINED_ALPHA",
        "G_CCMUX_TEXEL0_ALPHA",
        "G_CCMUX_TEXEL1_ALPHA",
        "G_CCMUX_PRIMITIVE_ALPHA",
        "G_CCMUX_SHADE_ALPHA",
        "G_CCMUX_ENV_ALPHA",
        "G_CCMUX_LOD_FRACTION",
        "G_CCMUX_PRIM_LOD_FRAC",
        "G_CCMUX_K5",
    };
    if (ccmux > 15) {
        return "G_CCMUX_0";

    } else {
        return tbl[ccmux];
    }
}

static const char* acmux_to_string(uint32_t acmux) {
    static const char* const tbl[] = {
        "G_ACMUX_COMBINED or G_ACMUX_LOD_FRACTION",
        "G_ACMUX_TEXEL0",
        "G_ACMUX_TEXEL1",
        "G_ACMUX_PRIMITIVE",
        "G_ACMUX_SHADE",
        "G_ACMUX_ENVIRONMENT",
        "G_ACMUX_1 or G_ACMUX_PRIM_LOD_FRAC",
        "G_ACMUX_0",
    };
    return tbl[acmux];
}

static void gfx_generate_cc(struct ColorCombiner* comb, const ColorCombinerKey& key) {
    bool is_2cyc = (key.options & (uint64_t)SHADER_OPT_2CYC) != 0;

    uint8_t c[2][2][4] = { { { 0 } } };
    uint64_t shader_id0 = 0;
    uint32_t shader_id1 = key.options;
    uint8_t shader_input_mapping[2][7] = { { 0 } };
    bool used_textures[2] = { false, false };
    for (int i = 0; i < 2 && (i == 0 || is_2cyc); i++) {
        uint32_t rgb_a = (key.combine_mode >> (i * 28)) & 0xf;
        uint32_t rgb_b = (key.combine_mode >> (i * 28 + 4)) & 0xf;
        uint32_t rgb_c = (key.combine_mode >> (i * 28 + 8)) & 0x1f;
        uint32_t rgb_d = (key.combine_mode >> (i * 28 + 13)) & 7;
        uint32_t alpha_a = (key.combine_mode >> (i * 28 + 16)) & 7;
        uint32_t alpha_b = (key.combine_mode >> (i * 28 + 16 + 3)) & 7;
        uint32_t alpha_c = (key.combine_mode >> (i * 28 + 16 + 6)) & 7;
        uint32_t alpha_d = (key.combine_mode >> (i * 28 + 16 + 9)) & 7;

        if (rgb_a >= 8) {
            rgb_a = G_CCMUX_0;
        }
        if (rgb_b >= 8) {
            rgb_b = G_CCMUX_0;
        }
        if (rgb_c >= 16) {
            rgb_c = G_CCMUX_0;
        }
        if (rgb_d == 7) {
            rgb_d = G_CCMUX_0;
        }

        if (rgb_a == rgb_b || rgb_c == G_CCMUX_0) {
            // Normalize
            rgb_a = G_CCMUX_0;
            rgb_b = G_CCMUX_0;
            rgb_c = G_CCMUX_0;
        }
        if (alpha_a == alpha_b || alpha_c == G_ACMUX_0) {
            // Normalize
            alpha_a = G_ACMUX_0;
            alpha_b = G_ACMUX_0;
            alpha_c = G_ACMUX_0;
        }
        if (i == 1) {
            if (rgb_a != G_CCMUX_COMBINED && rgb_b != G_CCMUX_COMBINED && rgb_c != G_CCMUX_COMBINED &&
                rgb_d != G_CCMUX_COMBINED) {
                // First cycle RGB not used, so clear it away
                c[0][0][0] = c[0][0][1] = c[0][0][2] = c[0][0][3] = G_CCMUX_0;
            }
            if (rgb_c != G_CCMUX_COMBINED_ALPHA && alpha_a != G_ACMUX_COMBINED && alpha_b != G_ACMUX_COMBINED &&
                alpha_d != G_ACMUX_COMBINED) {
                // First cycle ALPHA not used, so clear it away
                c[0][1][0] = c[0][1][1] = c[0][1][2] = c[0][1][3] = G_ACMUX_0;
            }
        }

        c[i][0][0] = rgb_a;
        c[i][0][1] = rgb_b;
        c[i][0][2] = rgb_c;
        c[i][0][3] = rgb_d;
        c[i][1][0] = alpha_a;
        c[i][1][1] = alpha_b;
        c[i][1][2] = alpha_c;
        c[i][1][3] = alpha_d;
    }
    if (!is_2cyc) {
        for (int i = 0; i < 2; i++) {
            for (int k = 0; k < 4; k++) {
                c[1][i][k] = i == 0 ? G_CCMUX_0 : G_ACMUX_0;
            }
        }
    }
    {
        uint8_t input_number[32] = { 0 };
        int next_input_number = SHADER_INPUT_1;
        for (int i = 0; i < 2 && (i == 0 || is_2cyc); i++) {
            for (int j = 0; j < 4; j++) {
                uint32_t val = 0;
                switch (c[i][0][j]) {
                    case G_CCMUX_0:
                        val = SHADER_0;
                        break;
                    case G_CCMUX_1:
                        val = SHADER_1;
                        break;
                    case G_CCMUX_TEXEL0:
                        val = SHADER_TEXEL0;
                        used_textures[0] = true;
                        break;
                    case G_CCMUX_TEXEL1:
                        val = SHADER_TEXEL1;
                        used_textures[1] = true;
                        break;
                    case G_CCMUX_TEXEL0_ALPHA:
                        val = SHADER_TEXEL0A;
                        used_textures[0] = true;
                        break;
                    case G_CCMUX_TEXEL1_ALPHA:
                        val = SHADER_TEXEL1A;
                        used_textures[1] = true;
                        break;
                    case G_CCMUX_NOISE:
                        val = SHADER_NOISE;
                        break;
                    case G_CCMUX_PRIMITIVE:
                    case G_CCMUX_PRIMITIVE_ALPHA:
                    case G_CCMUX_PRIM_LOD_FRAC:
                    case G_CCMUX_SHADE:
                    case G_CCMUX_SHADE_ALPHA:
                    case G_CCMUX_ENVIRONMENT:
                    case G_CCMUX_ENV_ALPHA:
                    case G_CCMUX_LOD_FRACTION:
                        if (input_number[c[i][0][j]] == 0) {
                            shader_input_mapping[0][next_input_number - 1] = c[i][0][j];
                            input_number[c[i][0][j]] = next_input_number++;
                        }
                        val = input_number[c[i][0][j]];
                        break;
                    case G_CCMUX_COMBINED:
                        val = SHADER_COMBINED;
                        break;
                    default:
                        sysLogPrintf(LOG_WARNING, "Unsupported ccmux: %d", c[i][0][j]);
                        break;
                }
                shader_id0 |= (uint64_t)val << (i * 32 + j * 4);
            }
        }
    }
    {
        uint8_t input_number[16] = { 0 };
        int next_input_number = SHADER_INPUT_1;
        for (int i = 0; i < 2; i++) {
            for (int j = 0; j < 4; j++) {
                uint32_t val = 0;
                switch (c[i][1][j]) {
                    case G_ACMUX_0:
                        val = SHADER_0;
                        break;
                    case G_ACMUX_TEXEL0:
                        val = SHADER_TEXEL0;
                        used_textures[0] = true;
                        break;
                    case G_ACMUX_TEXEL1:
                        val = SHADER_TEXEL1;
                        used_textures[1] = true;
                        break;
                    case G_ACMUX_LOD_FRACTION:
                        // case G_ACMUX_COMBINED: same numerical value
                        if (j != 2) {
                            val = SHADER_COMBINED;
                            break;
                        }
                        c[i][1][j] = G_CCMUX_LOD_FRACTION;
                        [[fallthrough]]; // for G_ACMUX_LOD_FRACTION
                    case G_ACMUX_1:
                        // case G_ACMUX_PRIM_LOD_FRAC: same numerical value
                        if (j != 2) {
                            val = SHADER_1;
                            break;
                        }
                        [[fallthrough]]; // for G_ACMUX_PRIM_LOD_FRAC
                    case G_ACMUX_PRIMITIVE:
                    case G_ACMUX_SHADE:
                    case G_ACMUX_ENVIRONMENT:
                        if (input_number[c[i][1][j]] == 0) {
                            shader_input_mapping[1][next_input_number - 1] = c[i][1][j];
                            input_number[c[i][1][j]] = next_input_number++;
                        }
                        val = input_number[c[i][1][j]];
                        break;
                }
                shader_id0 |= (uint64_t)val << (i * 32 + 16 + j * 4);
            }
        }
    }
    comb->shader_id0 = shader_id0;
    comb->shader_id1 = shader_id1;
    comb->used_textures[0] = used_textures[0];
    comb->used_textures[1] = used_textures[1];
    // comb->prg = gfx_lookup_or_create_shader_program(shader_id0, shader_id1);
    memcpy(comb->shader_input_mapping, shader_input_mapping, sizeof(shader_input_mapping));
}

static struct ColorCombiner* gfx_lookup_or_create_color_combiner(const ColorCombinerKey& key) {
    if (prev_combiner != color_combiner_pool.end() && prev_combiner->first == key) {
        return &prev_combiner->second;
    }

    prev_combiner = color_combiner_pool.find(key);
    if (prev_combiner != color_combiner_pool.end()) {
        return &prev_combiner->second;
    }
    gfx_flush();
    prev_combiner = color_combiner_pool.insert(std::make_pair(key, ColorCombiner())).first;
    gfx_generate_cc(&prev_combiner->second, key);
    return &prev_combiner->second;
}

void gfx_texture_cache_clear() {
    gfx_flush();
    dlcacheInvalidateAll(); // cached segments hold texture ids that are about to be freed
    for (const auto& entry : gfx_texture_cache.map) {
        gfx_texture_cache.free_texture_ids.push_back(entry.second.texture_id);
    }
    gfx_texture_cache.map.clear();
    gfx_texture_cache.lru.clear();
    rdp.textures_changed[0] = rdp.textures_changed[1] = true;
    memset(rendering_state.textures, 0, sizeof(rendering_state.textures));
    extTexFree(); // drop decoded ext_tex PNG data along with the GPU cache
}

static bool gfx_texture_cache_lookup(int i, const TextureCacheKey& key) {
    TextureCacheMap::iterator it = gfx_texture_cache.map.find(key);
    TextureCacheNode** n = &rendering_state.textures[i];

    if (it != gfx_texture_cache.map.end()) {
        gfx_rapi->select_texture(i, it->second.texture_id, it->second.linear_filter);
        *n = &*it;
        gfx_texture_cache.lru.splice(gfx_texture_cache.lru.end(), gfx_texture_cache.lru,
                                     it->second.lru_location); // move to back
        return true;
    }

    if (gfx_texture_cache.map.size() >= (size_t)g_TextureCacheMaxSize) {
        // Remove the texture that was least recently used
        dlcacheInvalidateAll(); // the evicted texture id may be referenced by a cached segment
        it = gfx_texture_cache.lru.front().it;
        gfx_texture_cache.free_texture_ids.push_back(it->second.texture_id);
        gfx_texture_cache.map.erase(it);
        gfx_texture_cache.lru.pop_front();
    }

    uint32_t texture_id;
    if (!gfx_texture_cache.free_texture_ids.empty()) {
        texture_id = gfx_texture_cache.free_texture_ids.back();
        gfx_texture_cache.free_texture_ids.pop_back();
    } else {
        texture_id = gfx_rapi->new_texture();
    }

    it = gfx_texture_cache.map.insert(std::make_pair(key, TextureCacheValue())).first;
    TextureCacheNode* node = &*it;
    node->second.texture_id = texture_id;
    node->second.lru_location = gfx_texture_cache.lru.insert(gfx_texture_cache.lru.end(), { it });

    gfx_rapi->select_texture(i, texture_id, false);
    gfx_rapi->set_sampler_parameters(i, false, 0, 0, rdp.tex_lod);
    *n = node;
    return false;
}

void gfx_texture_cache_delete(const uint8_t* orig_addr) {
    gfx_flush();
    dlcacheInvalidateAll();

    for (int i = 0; i < 2; ++i) {
        if (rendering_state.textures[i] && rendering_state.textures[i]->first.texture_addr == orig_addr) {
            rdp.textures_changed[i] = true;
            rendering_state.textures[i] = nullptr;
        }
    }

    while (gfx_texture_cache.map.bucket_count() > 0) {
        TextureCacheKey key = { orig_addr, { 0 }, 0, 0 }; // bucket index only depends on the address
        size_t bucket = gfx_texture_cache.map.bucket(key);
        bool again = false;
        for (auto it = gfx_texture_cache.map.begin(bucket); it != gfx_texture_cache.map.end(bucket); ++it) {
            if (it->first.texture_addr == orig_addr) {
                gfx_texture_cache.lru.erase(it->second.lru_location);
                gfx_texture_cache.free_texture_ids.push_back(it->second.texture_id);
                gfx_texture_cache.map.erase(it->first);
                again = true;
                break;
            }
        }
        if (!again) {
            break;
        }
    }
}

void gfx_texture_cache_delete_range(const uint8_t* start, const uint8_t* end) {
    gfx_flush();
    dlcacheInvalidateAll();

    for (int i = 0; i < 2; ++i) {
        if (rendering_state.textures[i]
                && rendering_state.textures[i]->first.texture_addr >= start
                && rendering_state.textures[i]->first.texture_addr < end) {
            rdp.textures_changed[i] = true;
            rendering_state.textures[i] = nullptr;
        }
    }

    for (auto it = gfx_texture_cache.map.begin(); it != gfx_texture_cache.map.end(); ) {
        if (it->first.texture_addr >= start && it->first.texture_addr < end) {
            gfx_texture_cache.lru.erase(it->second.lru_location);
            gfx_texture_cache.free_texture_ids.push_back(it->second.texture_id);
            it = gfx_texture_cache.map.erase(it);
        } else {
            ++it;
        }
    }
}

// Chokepoint for every N64-format texture import: all import_texture_* decode
// into tex_upload_buffer as RGBA32 and upload from there. When a chaos flat-
// texture mode is active, munge the buffer in place first (see the
// gfx_flattex_mode comment for the mode semantics).
static void gfx_upload_tex_filtered(uint32_t width, uint32_t height, bool gen_mipmaps) {
    if (gfx_flattex_mode == 1 || gfx_flattex_mode == 2) {
        const uint32_t count = width * height;
        uint8_t* px = tex_upload_buffer;
        uint8_t fr = 255, fg = 255, fb = 255;
        if (gfx_flattex_mode == 2) {
            uint64_t r = 0, g = 0, b = 0, n = 0;
            for (uint32_t i = 0; i < count; i++, px += 4) {
                if (px[3]) { // average visible texels only, or glyph RGB drowns in cutout black
                    r += px[0];
                    g += px[1];
                    b += px[2];
                    n++;
                }
            }
            if (n) {
                fr = (uint8_t)(r / n);
                fg = (uint8_t)(g / n);
                fb = (uint8_t)(b / n);
            }
            px = tex_upload_buffer;
        }
        for (uint32_t i = 0; i < count; i++, px += 4) {
            px[0] = fr;
            px[1] = fg;
            px[2] = fb;
        }
    }
    gfx_rapi->upload_texture(tex_upload_buffer, width, height, gen_mipmaps);
}

static void import_texture_rgba16(int tile, const LoadedTexture& loaded_texture, bool gen_mipmaps) {
    const uint8_t* addr = loaded_texture.addr;
    const uint32_t size_bytes = loaded_texture.size_bytes;
    const uint32_t full_image_line_size_bytes =
        loaded_texture.full_image_line_size_bytes;
    const uint32_t line_size_bytes = loaded_texture.line_size_bytes;
    // SUPPORT_CHECK(full_image_line_size_bytes == line_size_bytes);
    // TODO: this trips in some places with a garbage size in full_image_line_size_bytes
    // probably wherever framebuffer effects are used

    uint8_t *dest = tex_upload_buffer;
    for (uint32_t i = 0; i < size_bytes / 2; i++, dest += 4) {
        const uint16_t col16 = (addr[2 * i] << 8) | addr[2 * i + 1];
        const uint8_t a = col16 & 1;
        const uint8_t r = col16 >> 11;
        const uint8_t g = (col16 >> 6) & 0x1f;
        const uint8_t b = (col16 >> 1) & 0x1f;
        dest[0] = SCALE_5_8(r);
        dest[1] = SCALE_5_8(g);
        dest[2] = SCALE_5_8(b);
        dest[3] = a ? 255 : 0;
    }

    const uint32_t width = rdp.texture_tile[tile].line_size_bytes / 2;
    const uint32_t height = size_bytes / rdp.texture_tile[tile].line_size_bytes;

	gfx_upload_tex_filtered(width, height, gen_mipmaps);
    // DumpTexture(loaded_texture.otr_path, rgba32_buf, width, height);
}

static void import_texture_rgba32(int tile, const LoadedTexture& loaded_texture, bool gen_mipmaps) {
    const RawTexMetadata* metadata = &loaded_texture.raw_tex_metadata;
    const uint8_t* addr = loaded_texture.addr;
    const uint32_t size_bytes = loaded_texture.size_bytes;
    const uint32_t full_image_line_size_bytes =
        loaded_texture.full_image_line_size_bytes;
    const uint32_t line_size_bytes = loaded_texture.line_size_bytes;
    SUPPORT_CHECK(full_image_line_size_bytes == line_size_bytes);

    uint32_t *dest = (uint32_t *)tex_upload_buffer;
    const uint32_t *src = (const uint32_t *)addr;
    for (uint32_t i = 0; i < size_bytes; i += 4, ++dest, ++src) {
        *dest = PD_BE32(*src);
    }

    const uint32_t width = rdp.texture_tile[tile].line_size_bytes / 2;
    const uint32_t height = (size_bytes / 2) / rdp.texture_tile[tile].line_size_bytes;
	gfx_upload_tex_filtered(width, height, gen_mipmaps);
    // DumpTexture(loaded_texture.otr_path, addr, width, height);
}

static void import_texture_ia4(int tile, const LoadedTexture& loaded_texture, bool gen_mipmaps) {
    const RawTexMetadata* metadata = &loaded_texture.raw_tex_metadata;
    const uint8_t* addr = loaded_texture.addr;
    const uint32_t size_bytes = loaded_texture.size_bytes;
    const uint32_t full_image_line_size_bytes =
        loaded_texture.full_image_line_size_bytes;
    const uint32_t line_size_bytes = loaded_texture.line_size_bytes;
    SUPPORT_CHECK(full_image_line_size_bytes == line_size_bytes);

    uint8_t *dest = tex_upload_buffer;
    for (uint32_t i = 0; i < size_bytes * 2; i++, dest += 4) {
        const uint8_t byte = addr[i / 2];
        const uint8_t part = (byte >> (4 - (i % 2) * 4)) & 0xf;
        const uint8_t intensity = part >> 1;
        const uint8_t alpha = part & 1;
        const uint8_t c = SCALE_3_8(intensity);
        dest[0] = c;
        dest[1] = c;
        dest[2] = c;
        dest[3] = alpha ? 255 : 0;
    }

    const uint32_t width = rdp.texture_tile[tile].line_size_bytes * 2;
    const uint32_t height = size_bytes / rdp.texture_tile[tile].line_size_bytes;

	gfx_upload_tex_filtered(width, height, gen_mipmaps);
    // DumpTexture(loaded_texture.otr_path, rgba32_buf, width, height);
}

static void import_texture_ia8(int tile, const LoadedTexture& loaded_texture, bool gen_mipmaps) {
    const RawTexMetadata* metadata = &loaded_texture.raw_tex_metadata;
    const uint8_t* addr = loaded_texture.addr;
    const uint32_t size_bytes = loaded_texture.size_bytes;
    const uint32_t full_image_line_size_bytes =
        loaded_texture.full_image_line_size_bytes;
    const uint32_t line_size_bytes = loaded_texture.line_size_bytes;
    SUPPORT_CHECK(full_image_line_size_bytes == line_size_bytes);

    uint8_t *dest = tex_upload_buffer;
    for (uint32_t i = 0; i < size_bytes; i++, dest += 4) {
        const uint8_t intensity = SCALE_4_8(addr[i] >> 4);
        const uint8_t alpha = SCALE_4_8(addr[i] & 0xf);
        dest[0] = intensity;
        dest[1] = intensity;
        dest[2] = intensity;
        dest[3] = alpha;
    }

    const uint32_t width = rdp.texture_tile[tile].line_size_bytes;
    const uint32_t height = size_bytes / rdp.texture_tile[tile].line_size_bytes;

	gfx_upload_tex_filtered(width, height, gen_mipmaps);
    // DumpTexture(loaded_texture.otr_path, rgba32_buf, width, height);
}

static void import_texture_ia16(int tile, const LoadedTexture& loaded_texture, bool gen_mipmaps) {
    const RawTexMetadata* metadata = &loaded_texture.raw_tex_metadata;
    const uint8_t* addr = loaded_texture.addr;
    const uint32_t size_bytes = loaded_texture.size_bytes;
    const uint32_t full_image_line_size_bytes =
        loaded_texture.full_image_line_size_bytes;
    const uint32_t line_size_bytes = loaded_texture.line_size_bytes;
    SUPPORT_CHECK(full_image_line_size_bytes == line_size_bytes);

    uint8_t *dest = tex_upload_buffer;
    for (uint32_t i = 0; i < size_bytes / 2; i++, dest += 4) {
        const uint8_t intensity = addr[2 * i];
        const uint8_t alpha = addr[2 * i + 1];
        dest[0] = intensity;
        dest[1] = intensity;
        dest[2] = intensity;
        dest[3] = alpha;
    }

    const uint32_t width = rdp.texture_tile[tile].line_size_bytes / 2;
    const uint32_t height = size_bytes / rdp.texture_tile[tile].line_size_bytes;

	gfx_upload_tex_filtered(width, height, gen_mipmaps);
    // DumpTexture(loaded_texture.otr_path, rgba32_buf, width, height);
}

static void import_texture_i4(int tile, const LoadedTexture& loaded_texture, bool gen_mipmaps) {
    const RawTexMetadata* metadata = &loaded_texture.raw_tex_metadata;
    const uint8_t* addr = loaded_texture.addr;
    const uint32_t size_bytes = loaded_texture.size_bytes;
    const uint32_t full_image_line_size_bytes =
        loaded_texture.full_image_line_size_bytes;
    const uint32_t line_size_bytes = loaded_texture.line_size_bytes;
    SUPPORT_CHECK(full_image_line_size_bytes == line_size_bytes);

    uint8_t *dest = tex_upload_buffer;
    for (uint32_t i = 0; i < size_bytes * 2; i++, dest += 4) {
        const uint8_t byte = addr[i / 2];
        const uint8_t part = (byte >> (4 - (i % 2) * 4)) & 0xf;
        const uint8_t intensity = SCALE_4_8(part);
        dest[0] = intensity;
        dest[1] = intensity;
        dest[2] = intensity;
        dest[3] = intensity;
    }

    const uint32_t width = rdp.texture_tile[tile].line_size_bytes * 2;
    const uint32_t height = size_bytes / rdp.texture_tile[tile].line_size_bytes;

	gfx_upload_tex_filtered(width, height, gen_mipmaps);
    // DumpTexture(loaded_texture.otr_path, rgba32_buf, width, height);
}

static void import_texture_i8(int tile, const LoadedTexture& loaded_texture, bool gen_mipmaps) {
    const RawTexMetadata* metadata = &loaded_texture.raw_tex_metadata;
    const uint8_t* addr = loaded_texture.addr;
    const uint32_t size_bytes = loaded_texture.size_bytes;
    uint32_t full_image_line_size_bytes =
        loaded_texture.full_image_line_size_bytes;
    const uint32_t line_size_bytes = loaded_texture.line_size_bytes;
    SUPPORT_CHECK(full_image_line_size_bytes == line_size_bytes);

    uint8_t *dest = tex_upload_buffer;
    for (uint32_t i = 0; i < size_bytes; i++, dest += 4) {
        const uint8_t intensity = addr[i];
        dest[0] = intensity;
        dest[1] = intensity;
        dest[2] = intensity;
        dest[3] = intensity;
    }

    const uint32_t width = rdp.texture_tile[tile].line_size_bytes;
    const uint32_t height = size_bytes / rdp.texture_tile[tile].line_size_bytes;

	gfx_upload_tex_filtered(width, height, gen_mipmaps);
    // DumpTexture(loaded_texture.otr_path, rgba32_buf, width, height);
}

static inline void palette_to_rgba32(const uint16_t palentry, uint8_t *rgba32_buf) {
    if (rdp.palette_fmt == G_TT_IA16) {
        const uint8_t intensity = (palentry & 0xff);
        const uint8_t alpha = palentry >> 8;
        rgba32_buf[0] = intensity;
        rgba32_buf[1] = intensity;
        rgba32_buf[2] = intensity;
        rgba32_buf[3] = alpha;
    } else {
        // assume G_TT_RGBA16
        const uint8_t a = palentry & 1;
        const uint8_t r = palentry >> 11;
        const uint8_t g = (palentry >> 6) & 0x1f;
        const uint8_t b = (palentry >> 1) & 0x1f;
        rgba32_buf[0] = SCALE_5_8(r);
        rgba32_buf[1] = SCALE_5_8(g);
        rgba32_buf[2] = SCALE_5_8(b);
        rgba32_buf[3] = a ? 255 : 0;
    }
}

static void import_texture_ci4(int tile, const LoadedTexture& loaded_texture, bool gen_mipmaps) {
	const RawTexMetadata* metadata = &loaded_texture.raw_tex_metadata;
    const uint8_t* addr = loaded_texture.addr;
    const uint32_t size_bytes = loaded_texture.size_bytes;
    const uint32_t full_image_line_size_bytes =
        loaded_texture.full_image_line_size_bytes;
    const uint32_t line_size_bytes = loaded_texture.line_size_bytes;
    const uint32_t pal_idx = rdp.texture_tile[tile].palette; // 0-15
    const uint16_t* palette = (const uint16_t *)(rdp.palette + pal_idx * 16); // 16 pixel entries, 16 bits each
    SUPPORT_CHECK(full_image_line_size_bytes == line_size_bytes);

    for (uint32_t i = 0; i < size_bytes * 2; i++) {
        const uint8_t byte = addr[i / 2];
        const uint8_t idx = (byte >> (4 - (i % 2) * 4)) & 0xf;
        palette_to_rgba32(palette[idx], tex_upload_buffer +4 * i);
    }

    uint32_t result_line_size = rdp.texture_tile[tile].line_size_bytes;
    if (metadata->h_byte_scale != 1) {
        result_line_size *= metadata->h_byte_scale;
    }

    const uint32_t width = result_line_size * 2;
    const uint32_t height = size_bytes / result_line_size;

	gfx_upload_tex_filtered(width, height, gen_mipmaps);
}

static void import_texture_ci8(int tile, const LoadedTexture& loaded_texture, bool gen_mipmaps) {
	const RawTexMetadata* metadata = &loaded_texture.raw_tex_metadata;
    const uint8_t* addr = loaded_texture.addr;
    const uint32_t size_bytes = loaded_texture.size_bytes;
    const uint32_t full_image_line_size_bytes =
        loaded_texture.full_image_line_size_bytes;
    const uint32_t line_size_bytes = loaded_texture.line_size_bytes;

    for (uint32_t i = 0, j = 0; i < size_bytes; j += full_image_line_size_bytes - line_size_bytes) {
        for (uint32_t k = 0; k < line_size_bytes; i++, k++, j++) {
            const uint8_t idx = addr[j];
            palette_to_rgba32(rdp.palette[idx], tex_upload_buffer + 4 * i);
        }
    }

    uint32_t result_line_size = rdp.texture_tile[tile].line_size_bytes;
    if (metadata->h_byte_scale != 1) {
        result_line_size *= metadata->h_byte_scale;
    }

    const uint32_t width = result_line_size;
    const uint32_t height = size_bytes / result_line_size;

	gfx_upload_tex_filtered(width, height, gen_mipmaps);
}

static void import_texture(int i, int tile, bool importReplacement) {
    LoadedTexture& loaded_texture = rdp.loaded_texture[rdp.texture_tile[tile].tmem];
    const uint8_t fmt = rdp.texture_tile[tile].fmt;
    const uint8_t siz = rdp.texture_tile[tile].siz;
    const uint32_t tex_flags = loaded_texture.tex_flags;
    const uint8_t palette_index = rdp.texture_tile[tile].palette;

    if ((rdp.tex_lod && tile >= rdp.first_tile_index + rdp.tex_detail) || !loaded_texture.addr) {
        // set up miplevel 0; also acts as a catch-all for when .addr is NULL because my texture loader sucks
        loaded_texture.addr = rdp.texture_to_load.addr;
        loaded_texture.line_size_bytes = rdp.texture_tile[tile].line_size_bytes;
        loaded_texture.full_image_line_size_bytes = rdp.texture_tile[tile].line_size_bytes;
        loaded_texture.full_size_bytes = loaded_texture.full_image_line_size_bytes * rdp.texture_tile[tile].height;
        loaded_texture.size_bytes = loaded_texture.line_size_bytes * rdp.texture_tile[tile].height;
        if (siz == G_IM_SIZ_32b) {
            // HACK: fixup 32-bit LODed texture height
            loaded_texture.size_bytes <<= 1;
            loaded_texture.full_size_bytes <<= 1;
        }
        loaded_texture.orig_size_bytes = loaded_texture.size_bytes;
    }

    const RawTexMetadata* metadata = &loaded_texture.raw_tex_metadata;
    const uint8_t* orig_addr = loaded_texture.addr;
    SUPPORT_CHECK(orig_addr);

    // ext_tex: a texture tagged external (gfx_dp_load_block found a PNG for
    // its G_SETTEXINFO_EXT id) is cached by ext_key instead of address and
    // uploaded straight from the decoded PNG, skipping the N64 format decode.
    // Chaos flat-texture modes force the N64 decode path instead (the PNG
    // buffer is persistent and can't be munged in place); the cache clear on
    // toggle re-imports as external once the mode ends.
    const uint8_t external = gfx_flattex_mode ? 0 : (uint8_t)(loaded_texture.ext_key >> (7 * 8));

    TextureCacheKey key;
    if (external) {
        key = { 0, {}, 0, 0, 0, loaded_texture.ext_key, loaded_texture.id_mask };
    } else if (fmt == G_IM_FMT_CI) {
        key = { orig_addr, { rdp.palette_addrs[0], rdp.palette_addrs[1] }, fmt, siz, palette_index };
    } else {
        key = { orig_addr, {}, fmt, siz, palette_index };
    }

    if (gfx_texture_cache_lookup(i, key)) {
        loaded_texture.id_mask = 0;
        return;
    }

    if (external) {
        uint8_t type = loaded_texture.type;
        uint16_t id = loaded_texture.id | loaded_texture.id_mask;
        uint32_t texnum = loaded_texture.texnum;

        uint32_t width, height;

        uint8_t* ext_addr = extTexLoad(type, id, texnum, &width, &height);
        if (!ext_addr) {
            // upload a single pink pixel as visual feedback
            tex_upload_buffer[0] = 255;
            tex_upload_buffer[1] = 0;
            tex_upload_buffer[2] = 255;
            tex_upload_buffer[3] = 255;
            width = height = 1;
            ext_addr = tex_upload_buffer;
        }

        loaded_texture.id_mask = 0;

        gfx_rapi->upload_texture(ext_addr, width, height, rdp.tex_lod);
        return;
    }

    if (fmt == G_IM_FMT_RGBA) {
        if (siz == G_IM_SIZ_16b) {
            import_texture_rgba16(tile, loaded_texture, rdp.tex_lod);
        } else if (siz == G_IM_SIZ_32b) {
            import_texture_rgba32(tile, loaded_texture, rdp.tex_lod);
        } else {
            sysFatalError("Bad size for RGBA texture in tile %d: %02x", tile, siz);
        }
    } else if (fmt == G_IM_FMT_IA) {
        if (siz == G_IM_SIZ_4b) {
            import_texture_ia4(tile, loaded_texture, rdp.tex_lod);
        } else if (siz == G_IM_SIZ_8b) {
            import_texture_ia8(tile, loaded_texture, rdp.tex_lod);
        } else if (siz == G_IM_SIZ_16b) {
            import_texture_ia16(tile, loaded_texture, rdp.tex_lod);
        } else {
            sysFatalError("Bad size for IA texture in tile %d: %02x", tile, siz);
        }
    } else if (fmt == G_IM_FMT_CI) {
        if (siz == G_IM_SIZ_4b) {
            import_texture_ci4(tile, loaded_texture, rdp.tex_lod);
        } else if (siz == G_IM_SIZ_8b) {
            import_texture_ci8(tile, loaded_texture, rdp.tex_lod);
        } else {
            sysFatalError("Bad size for CI texture in tile %d: %02x", tile, siz);
        }
    } else if (fmt == G_IM_FMT_I) {
        if (siz == G_IM_SIZ_4b) {
            import_texture_i4(tile, loaded_texture, rdp.tex_lod);
        } else if (siz == G_IM_SIZ_8b) {
            import_texture_i8(tile, loaded_texture, rdp.tex_lod);
        } else {
            sysFatalError("Bad size for I texture in tile %d: %02x", tile, siz);
        }
    } else {
        sysFatalError("Bad texture format in tile %d: %02x %02x", tile, fmt, siz);
    }
}

static void gfx_normalize_vector(float v[3]) {
    float s = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    v[0] /= s;
    v[1] /= s;
    v[2] /= s;
}

static void gfx_transposed_matrix_mul(float res[3], const float a[3], const float b[4][4]) {
    res[0] = a[0] * b[0][0] + a[1] * b[0][1] + a[2] * b[0][2];
    res[1] = a[0] * b[1][0] + a[1] * b[1][1] + a[2] * b[1][2];
    res[2] = a[0] * b[2][0] + a[1] * b[2][1] + a[2] * b[2][2];
}

static void calculate_normal_dir(const Light_t* light, float coeffs[3]) {
    const float light_dir[3] = { light->dir[0] / 127.f, light->dir[1] / 127.f, light->dir[2] / 127.f };

    gfx_transposed_matrix_mul(coeffs, light_dir, rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1]);
    gfx_normalize_vector(coeffs);
}

static void calculate_normal_dir(const struct NormalColor *vcn, float coeffs[3]) {
    const float light_dir[3] = { vcn->x / 127.f, vcn->y / 127.f, vcn->z / 127.f };

    gfx_transposed_matrix_mul(coeffs, light_dir, rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1]);
    gfx_normalize_vector(coeffs);
}

static void gfx_matrix_mul(float res[4][4], const float a[4][4], const float b[4][4]) {
    float tmp[4][4];
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            tmp[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j] + a[i][3] * b[3][j];
        }
    }
    memcpy(res, tmp, sizeof(tmp));
}

static void gfx_sp_matrix(uint8_t parameters, const int32_t* addr) {
    float matrix[4][4];

    if (addr == NULL) {
        // A null matrix pointer reaches here when the game emits gSPMatrix with
        // an unresolved/NULL address (seg_addr returns NULL only for w1 == 0).
        // Observed under 32-simulant memory pressure, where a model whose
        // matrix allocation failed still queues its matrix command. Dereferencing
        // addr would fault (read at 0x0); fall back to identity so the rest of
        // the function still runs the PUSH/LOAD/POP bookkeeping and the matrix
        // stack stays balanced for any paired G_POPMTX.
        memset(matrix, 0, sizeof(matrix));
        matrix[0][0] = matrix[1][1] = matrix[2][2] = matrix[3][3] = 1.0f;
    } else {
#ifndef GBI_FLOATS
        // Original GBI where fixed point matrices are used
        for (int i = 0; i < 4; i++) {
            for (int j = 0; j < 4; j += 2) {
                int32_t int_part = addr[i * 2 + j / 2];
                uint32_t frac_part = addr[8 + i * 2 + j / 2];
                matrix[i][j] = (int32_t)((int_part & 0xffff0000) | (frac_part >> 16)) / 65536.0f;
                matrix[i][j + 1] = (int32_t)((int_part << 16) | (frac_part & 0xffff)) / 65536.0f;
            }
        }
#else
        // For a modified GBI where fixed point values are replaced with floats
        memcpy(matrix, addr, sizeof(matrix));
#endif
    }

    if (parameters & G_MTX_PROJECTION) {
        if (parameters & G_MTX_LOAD) {
            memcpy(rsp.P_matrix, matrix, sizeof(matrix));
        } else {
            gfx_matrix_mul(rsp.P_matrix, matrix, rsp.P_matrix);
        }
    } else { // G_MTX_MODELVIEW
        if ((parameters & G_MTX_PUSH) && rsp.modelview_matrix_stack_size < 11) {
            ++rsp.modelview_matrix_stack_size;
            memcpy(rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1],
                   rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 2], sizeof(matrix));
        }
        if (parameters & G_MTX_LOAD) {
            memcpy(rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1], matrix, sizeof(matrix));
        } else {
            gfx_matrix_mul(rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1], matrix,
                           rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1]);
        }
        rsp.lights_changed = 1;
    }
    gfx_matrix_mul(rsp.MP_matrix, rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1], rsp.P_matrix);
}

static void gfx_sp_pop_matrix(uint32_t count) {
    while (count--) {
        if (rsp.modelview_matrix_stack_size > 0) {
            --rsp.modelview_matrix_stack_size;
            if (rsp.modelview_matrix_stack_size > 0) {
                gfx_matrix_mul(rsp.MP_matrix, rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1],
                               rsp.P_matrix);
            }
        }
    }
}

static float gfx_adjust_x_for_aspect_ratio(float x, float w = 1.f) {
    if (fbActive) {
        return x;
    } else {
        return (rsp.aspect_ofs * w + x) * rsp.aspect_scale / gfx_current_dimensions.aspect_ratio;
    }
}

static void gfx_adjust_width_height_for_scale(uint32_t& width, uint32_t& height) {
    width = std::round(width * RATIO_Y);
    height = std::round(height * RATIO_Y);
    if (width == 0) {
        width = 1;
    }
    if (height == 0) {
        height = 1;
    }
}

static void gfx_sp_vertex(size_t n_vertices, size_t dest_index, const Vtx* vertices) {
    SUPPORT_CHECK(n_vertices <= MAX_VERTICES);

    // One vtxbatch per G_VTX command (matches bg.c bgPopulateVtxBatchType), so the
    // octree's per-batch visibility lines up with this index at replay.
    if (g_DlCacheRecording) {
        g_DlCacheBatchIndex++;
    }

    for (size_t i = 0; i < n_vertices; i++, dest_index++) {
        const Vtx* v = &vertices[i];
        struct LoadedVertex* d = &rsp.loaded_vertices[dest_index];

        float x = v->v[0] * rsp.MP_matrix[0][0] + v->v[1] * rsp.MP_matrix[1][0] + v->v[2] * rsp.MP_matrix[2][0] + rsp.MP_matrix[3][0];
        float y = v->v[0] * rsp.MP_matrix[0][1] + v->v[1] * rsp.MP_matrix[1][1] + v->v[2] * rsp.MP_matrix[2][1] + rsp.MP_matrix[3][1];
        float z = v->v[0] * rsp.MP_matrix[0][2] + v->v[1] * rsp.MP_matrix[1][2] + v->v[2] * rsp.MP_matrix[2][2] + rsp.MP_matrix[3][2];
        float w = v->v[0] * rsp.MP_matrix[0][3] + v->v[1] * rsp.MP_matrix[1][3] + v->v[2] * rsp.MP_matrix[2][3] + rsp.MP_matrix[3][3];

        x = gfx_adjust_x_for_aspect_ratio(x, w);

        // CHEAT_MIRROR: flip clip-space X to reflect the scene left-right. Done
        // after the aspect fixup so the aspect ratio is preserved. Winding is
        // compensated in gfx_sp_tri1 (this reverses triangle winding, which would
        // otherwise make back-face culling render the world inside-out).
        // G_NOMIRROR_EXT exempts 2D UI drawn as 3D geometry (menu/HUD borders via
        // menugfxDrawTri2) so they stay put while the world flips.
        if (gfx_mirror_mode && !(rsp.extra_geometry_mode & G_NOMIRROR_EXT)) {
            x = -x;
        }

        // Chaos upside-down: the same trick about the horizontal axis. Winding
        // is compensated in gfx_sp_tri1 alongside the mirror compensation (two
        // active flips cancel there, correctly).
        if (gfx_upsidedown_mode && !(rsp.extra_geometry_mode & G_NOMIRROR_EXT)) {
            y = -y;
        }

        short U = v->s * rsp.texture_scaling_factor.s >> 16;
        short V = v->t * rsp.texture_scaling_factor.t >> 16;

        const struct NormalColor *vcn = &rsp.vertex_colors[v->colour >> 2];

        if (rsp.geometry_mode & G_LIGHTING) {
            if (rsp.lights_changed) {
                for (int i = 0; i < rsp.current_num_lights - 1; i++) {
                    calculate_normal_dir(&rsp.current_lights[i], rsp.current_lights_coeffs[i]);
                }
                if (rsp.lookat_enabled) {
                    calculate_normal_dir(&rsp.lookat[0], rsp.current_lookat_coeffs[0]);
                    calculate_normal_dir(&rsp.lookat[1], rsp.current_lookat_coeffs[1]);
                }
                rsp.lights_changed = false;
            }

            int r = rsp.current_lights[rsp.current_num_lights - 1].col[0];
            int g = rsp.current_lights[rsp.current_num_lights - 1].col[1];
            int b = rsp.current_lights[rsp.current_num_lights - 1].col[2];

            for (int i = 0; i < rsp.current_num_lights - 1; i++) {
                float intensity = 0;
                intensity += vcn->x * rsp.current_lights_coeffs[i][0];
                intensity += vcn->y * rsp.current_lights_coeffs[i][1];
                intensity += vcn->z * rsp.current_lights_coeffs[i][2];
                intensity /= 127.0f;
                if (intensity > 0.0f) {
                    r += intensity * rsp.current_lights[i].col[0];
                    g += intensity * rsp.current_lights[i].col[1];
                    b += intensity * rsp.current_lights[i].col[2];
                }
            }

            d->color.r = r > 255 ? 255 : r;
            d->color.g = g > 255 ? 255 : g;
            d->color.b = b > 255 ? 255 : b;

            if (rsp.geometry_mode & G_TEXTURE_GEN) {
                float dotx = 0, doty = 0;
                if (rsp.lookat_enabled) {
                    dotx += vcn->x * rsp.current_lookat_coeffs[0][0];
                    dotx += vcn->y * rsp.current_lookat_coeffs[0][1];
                    dotx += vcn->z * rsp.current_lookat_coeffs[0][2];
                    doty += vcn->x * rsp.current_lookat_coeffs[1][0];
                    doty += vcn->y * rsp.current_lookat_coeffs[1][1];
                    doty += vcn->z * rsp.current_lookat_coeffs[1][2];
                    dotx /= 127.0f;
                    doty /= 127.0f;
                } else {
                    float tvcn[3];
                    calculate_normal_dir(vcn, tvcn);
                    dotx = tvcn[0];
                    doty = tvcn[1];
                }

                dotx = clampf(dotx, -1.0f, 1.0f);
                doty = clampf(doty, -1.0f, 1.0f);

                if (rsp.geometry_mode & G_TEXTURE_GEN_LINEAR) {
                    // Not sure exactly what formula we should use to get accurate values
                    /*dotx = (2.906921f * dotx * dotx + 1.36114f) * dotx;
                    doty = (2.906921f * doty * doty + 1.36114f) * doty;
                    dotx = (dotx + 1.0f) / 4.0f;
                    doty = (doty + 1.0f) / 4.0f;*/
                    dotx = acosf(-dotx) /* M_PI */ / 4.0f;
                    doty = acosf(-doty) /* M_PI */ / 4.0f;
                } else {
                    dotx = (dotx + 1.0f) / 4.0f;
                    doty = (doty + 1.0f) / 4.0f;
                }

                U = (int32_t)(dotx * rsp.texture_scaling_factor.s);
                V = (int32_t)(doty * rsp.texture_scaling_factor.t);
            }
        } else {
            d->color.r = vcn->r;
            d->color.g = vcn->g;
            d->color.b = vcn->b;
        }

        // Chaos forced-shiny: override the texture coords with a screen-space
        // projection of the clip-space position (see gfx_shiny_mode). The
        // (n+1)/4 mapping matches the G_TEXTURE_GEN sphere-map convention above
        // so the sampled window is the same half-texture range env maps use.
        // Skip G_NOMIRROR_EXT geometry (menu/HUD borders drawn as 3D tris).
        if (gfx_shiny_mode && !(rsp.extra_geometry_mode & G_NOMIRROR_EXT) && w != 0.0f) {
            const float sx = clampf(x / w, -1.0f, 1.0f);
            const float sy = clampf(y / w, -1.0f, 1.0f);
            U = (int32_t)((sx + 1.0f) / 4.0f * rsp.texture_scaling_factor.s);
            V = (int32_t)((sy + 1.0f) / 4.0f * rsp.texture_scaling_factor.t);
        }

        d->u = U;
        d->v = V;

        // trivial clip rejection
        d->clip_rej = 0;
        if (x < -w) {
            d->clip_rej |= 1; // CLIP_LEFT
        }
        if (x > w) {
            d->clip_rej |= 2; // CLIP_RIGHT
        }
        if (y < -w) {
            d->clip_rej |= 4; // CLIP_BOTTOM
        }
        if (y > w) {
            d->clip_rej |= 8; // CLIP_TOP
        }
        // if (z < -w) d->clip_rej |= 16; // CLIP_NEAR
        if (z > w) {
            d->clip_rej |= 32; // CLIP_FAR
        }

        d->x = x;
        d->y = y;
        d->z = z;
        d->w = w;

        // Object-space position for the display-list cache (GPU-side uMVP).
        d->ox = v->v[0];
        d->oy = v->v[1];
        d->oz = v->v[2];
        // Palette index for the shader-side GPU palette (live dynamic lighting).
        d->colour_index = (float)(v->colour >> 2);

        if (rsp.geometry_mode & G_FOG) {
            if (fabsf(w) < 0.001f) {
                // To avoid division by zero
                w = 0.001f;
            }

            float winv = 1.0f / w;
            if (winv < 0.0f) {
                winv = std::numeric_limits<int16_t>::max();
            }

            float fog_z = z * winv * rsp.fog_mul + rsp.fog_offset;
            d->fog = clampf(fog_z, 0.f, 255.f);
        } else {
            d->fog = rdp.fog_color.a;
        }

        d->color.a = vcn->a; // can be required for SHADE_ALPHA even if fog is enabled
    }
}

static void gfx_sp_modify_vertex(uint16_t vtx_idx, uint8_t where, uint32_t val) {
    SUPPORT_CHECK(where == G_MWO_POINT_ST);

    int16_t s = (int16_t)(val >> 16);
    int16_t t = (int16_t)val;

    struct LoadedVertex* v = &rsp.loaded_vertices[vtx_idx];
    v->u = s;
    v->v = t;
}

static inline int gfx_lod_tile_offset(const int i) {
    if (gfx_detail_textures_enabled)
        return ((rdp.tex_lod && !rdp.tex_detail) ? 0 : i);
    return (rdp.tex_lod ? rdp.tex_detail : i);
}

// CHEAT_MIRROR: the geometry is reflected about NDC x=0 (see gfx_sp_vertex),
// which glViewport maps to the viewport's horizontal centre in window pixels.
// Reflect a scissor box about that same window-pixel axis so per-room portal
// draw-slot scissors clip the left-right-flipped geometry on the correct side.
// Full-viewport scissors (HUD, the player's own room) are symmetric about that
// axis, so this is a no-op for them. rdp.viewport is live at both call sites.
static inline float gfx_mirror_scissor_x(float scissor_x, float scissor_w) {
    const float vp_centre = rdp.viewport.x + rdp.viewport.width * 0.5f;
    return 2.0f * vp_centre - (scissor_x + scissor_w);
}

// Chaos upside-down: the vertical analogue of gfx_mirror_scissor_x.
static inline float gfx_upsidedown_scissor_y(float scissor_y, float scissor_h) {
    const float vp_centre = rdp.viewport.y + rdp.viewport.height * 0.5f;
    return 2.0f * vp_centre - (scissor_y + scissor_h);
}

static void gfx_sp_tri1(uint8_t vtx1_idx, uint8_t vtx2_idx, uint8_t vtx3_idx, bool is_rect) {
    struct LoadedVertex* v1 = &rsp.loaded_vertices[vtx1_idx];
    struct LoadedVertex* v2 = &rsp.loaded_vertices[vtx2_idx];
    struct LoadedVertex* v3 = &rsp.loaded_vertices[vtx3_idx];
    struct LoadedVertex* v_arr[3] = { v1, v2, v3 };

    // While recording the display-list cache we must capture the WHOLE leaf
    // (the GPU clips + culls per-frame on replay), so skip the camera-dependent
    // clip-reject and backface-cull early-outs that would otherwise bake the
    // record-frame camera into the cache. Costs one frame of record-frame
    // overdraw; the geometry is still drawn correctly via the z-buffer.
    const bool dlcache_capturing = g_DlCacheRecording && !g_DlCacheAbort;

    if ((rsp.extra_geometry_mode & G_NO_CLIPPING_EXT) == 0) {
        if (v1->clip_rej & v2->clip_rej & v3->clip_rej) {
            // The whole triangle lies outside the visible area
            if (!dlcache_capturing) {
                return;
            }
        }
    }

    if ((rsp.geometry_mode & G_CULL_BOTH) != 0 && !dlcache_capturing) {
        float dx1 = v1->x / (v1->w) - v2->x / (v2->w);
        float dy1 = v1->y / (v1->w) - v2->y / (v2->w);
        float dx2 = v3->x / (v3->w) - v2->x / (v2->w);
        float dy2 = v3->y / (v3->w) - v2->y / (v2->w);
        float cross = dx1 * dy2 - dy1 * dx2;

        if ((v1->w < 0) ^ (v2->w < 0) ^ (v3->w < 0)) {
            // If one vertex lies behind the eye, negating cross will give the correct result.
            // If all vertices lie behind the eye, the triangle will be rejected anyway.
            cross = -cross;
        }

        // If inverted culling is requested, negate the cross
        // if ((rsp.extra_geometry_mode & G_EX_INVERT_CULLING) == 1) {
        //     cross = -cross;
        // }

        // CHEAT_MIRROR negates clip-space X in gfx_sp_vertex, which reverses
        // triangle winding. Negate the cross back so back-face culling keeps the
        // same faces it would un-mirrored (otherwise the world renders inside-out).
        // Skipped for G_NOMIRROR_EXT geometry (it wasn't X-negated either).
        if (gfx_mirror_mode && !(rsp.extra_geometry_mode & G_NOMIRROR_EXT)) {
            cross = -cross;
        }

        // Chaos upside-down reverses winding the same way; with mirror also
        // active the two X/Y flips restore the original winding, and the two
        // negates here cancel to match.
        if (gfx_upsidedown_mode && !(rsp.extra_geometry_mode & G_NOMIRROR_EXT)) {
            cross = -cross;
        }

        switch (rsp.geometry_mode & G_CULL_BOTH) {
            case G_CULL_FRONT:
                if (cross <= 0) {
                    return;
                }
                break;
            case G_CULL_BACK:
                if (cross >= 0) {
                    return;
                }
                break;
            case G_CULL_BOTH:
                // Why is this even an option?
                return;
        }
    }

    bool depth_test = ((rsp.geometry_mode & G_ZBUFFER) == G_ZBUFFER || (rdp.other_mode_l & G_ZS_PRIM) == G_ZS_PRIM) &&
                      ((rdp.other_mode_h & G_CYC_1CYCLE) == G_CYC_1CYCLE || (rdp.other_mode_h & G_CYC_2CYCLE) == G_CYC_2CYCLE);
    bool depth_update = (rdp.other_mode_l & Z_UPD) == Z_UPD;
    bool depth_compare = (rdp.other_mode_l & Z_CMP) == Z_CMP;
    bool depth_source_prim = (rdp.other_mode_l & G_ZS_PRIM) == G_ZS_PRIM /* && gDP.primDepth.z == 1.0f */;
    uint16_t zmode = rdp.other_mode_l & ZMODE_DEC;
    uint8_t depth_mode = (depth_test ? 1 : 0) | (depth_update ? 2 : 0) | (depth_compare ? 4 : 0) | (depth_source_prim ? 8 : 0) | (zmode >> 6);

    if (depth_mode != rendering_state.depth_mode) {
        gfx_flush();
        gfx_rapi->set_depth_mode(depth_test, depth_update, depth_compare, depth_source_prim, zmode);
        rendering_state.depth_mode = depth_mode;
    }

    if (rdp.viewport_or_scissor_changed) {
        if (memcmp(&rdp.viewport, &rendering_state.viewport, sizeof(rdp.viewport)) != 0) {
            gfx_flush();
            gfx_rapi->set_viewport(rdp.viewport.x, rdp.viewport.y, rdp.viewport.width, rdp.viewport.height);
            rendering_state.viewport = rdp.viewport;
        }
        if (memcmp(&rdp.scissor, &rendering_state.scissor, sizeof(rdp.scissor)) != 0) {
            gfx_flush();
            // Mirror only 3D-geometry scissors, not 2D rects: gfx_draw_rectangle
            // writes un-mirrored vertices directly and temporarily forces a
            // full-screen viewport, so reflecting its scissor would misplace the
            // HUD (notably in split-screen). is_rect distinguishes the two.
            const float scx = (gfx_mirror_mode && !is_rect) ? gfx_mirror_scissor_x(rdp.scissor.x, rdp.scissor.width) : rdp.scissor.x;
            const float scy = (gfx_upsidedown_mode && !is_rect) ? gfx_upsidedown_scissor_y(rdp.scissor.y, rdp.scissor.height) : rdp.scissor.y;
            gfx_rapi->set_scissor(scx, scy, rdp.scissor.width, rdp.scissor.height);
            rendering_state.scissor = rdp.scissor;
        }
        rdp.viewport_or_scissor_changed = false;
    }

    uint64_t cc_options = 0;
    bool use_alpha =
        (rdp.other_mode_l & (3 << 20)) == (G_BL_CLR_MEM << 20) && (rdp.other_mode_l & (3 << 16)) == (G_BL_1MA << 16);
    const bool use_fog = ((rdp.other_mode_l >> 30) == G_BL_CLR_FOG) || ((rdp.other_mode_l >> 26) == G_BL_A_FOG);
    const bool texture_edge = (rdp.other_mode_l & CVG_X_ALPHA) == CVG_X_ALPHA;
    const bool use_noise = (rdp.other_mode_l & (3U << G_MDSFT_ALPHACOMPARE)) == G_AC_DITHER;
    const bool use_2cyc = (rdp.other_mode_h & (3U << G_MDSFT_CYCLETYPE)) == G_CYC_2CYCLE;
    const bool alpha_threshold = (rdp.other_mode_l & (3U << G_MDSFT_ALPHACOMPARE)) == G_AC_THRESHOLD;
    const bool invisible = (rdp.other_mode_l & (3 << 24)) == (G_BL_0 << 24) && (rdp.other_mode_l & (3 << 20)) == (G_BL_CLR_MEM << 20);
    const bool use_grayscale = rdp.grayscale;
    const bool use_modulate = use_alpha && (rsp.extra_geometry_mode & G_MODULATE_EXT) != 0;
    const bool use_blur = (rdp.other_mode_h & (3U << G_MDSFT_TEXTFILT)) == G_TF_BLUR_EXT;

    if (texture_edge) {
        use_alpha = true;
    }

    if (use_alpha) {
        cc_options |= (uint64_t)SHADER_OPT_ALPHA;
    }
    if (use_fog) {
        cc_options |= (uint64_t)SHADER_OPT_FOG;
    }
    if (texture_edge) {
        cc_options |= (uint64_t)SHADER_OPT_TEXTURE_EDGE;
    }
    if (use_noise) {
        cc_options |= (uint64_t)SHADER_OPT_NOISE;
    }
    if (use_2cyc) {
        cc_options |= (uint64_t)SHADER_OPT_2CYC;
    }
    if (alpha_threshold) {
        cc_options |= (uint64_t)SHADER_OPT_ALPHA_THRESHOLD;
    }
    if (invisible) {
        cc_options |= (uint64_t)SHADER_OPT_INVISIBLE;
    }
    if (use_grayscale) {
        cc_options |= (uint64_t)SHADER_OPT_GRAYSCALE;
    }
    if (use_blur) {
        cc_options |= (uint64_t)SHADER_OPT_BLUR;
    }

    // If we are not using alpha, clear the alpha components of the combiner as they have no effect
    if (!use_alpha) {
        cc_options &= ~((0xfff << 16) | ((uint64_t)0xfff << 44));
    }

    ColorCombinerKey key;
    key.combine_mode = rdp.combine_mode;
    key.options = cc_options;

    ColorCombiner* comb = gfx_lookup_or_create_color_combiner(key);

    // Display-list cache restrictions: bail out (this leaf stays legacy) only on
    // genuinely un-bakeable state. G_LIGHTING and G_TEXTURE_GEN are both view-space
    // (camera-dependent colour / texgen UVs computed from the view direction), so
    // baking them freezes to the record-frame camera. Fog is supported: distance
    // fog (G_FOG) is recomputed in the shader, constant fog bakes into aFog.a;
    // 2-cycle / multitexture / grayscale bake fine too.
    if (g_DlCacheRecording && !g_DlCacheAbort) {
        int reason = 0;
        if (rsp.geometry_mode & G_LIGHTING) {
            reason |= GFX_DLC_ABORT_LIGHTING;
        }
        if (rsp.geometry_mode & G_TEXTURE_GEN) {
            reason |= GFX_DLC_ABORT_TEXGEN;
        }
        if ((rsp.geometry_mode & G_CULL_BOTH) == G_CULL_BOTH) {
            reason |= GFX_DLC_ABORT_CULLBOTH;
        }
        if (reason) {
            g_DlCacheAbort = true;
            g_DlCacheAbortReasons |= reason;
        }
    }

    uint32_t tm = 0;
    uint32_t tex_width[2], tex_height[2], tex_width2[2], tex_height2[2];

    for (int i = 0; i < 2; i++) {
        // TODO: fix this; for now just ignore smaller mips
        const uint32_t tile = rdp.first_tile_index + gfx_lod_tile_offset(i);
        if (comb->used_textures[i]) {
            if (rdp.textures_changed[i]) {
                gfx_flush();
                import_texture(i, tile, false);
                rdp.textures_changed[i] = false;
            }

            uint8_t cms = rdp.texture_tile[tile].cms;
            uint8_t cmt = rdp.texture_tile[tile].cmt;

            uint32_t tex_size_bytes = rdp.loaded_texture[rdp.texture_tile[tile].tmem].orig_size_bytes;
            uint32_t line_size = rdp.texture_tile[tile].line_size_bytes;

            if (line_size == 0) {
                line_size = 1;
            }

            tex_height[i] = tex_size_bytes / line_size;
            switch (rdp.texture_tile[tile].siz) {
                case G_IM_SIZ_4b:
                    line_size <<= 1;
                    break;
                case G_IM_SIZ_8b:
                    break;
                case G_IM_SIZ_16b:
                    line_size /= G_IM_SIZ_16b_LINE_BYTES;
                    break;
                case G_IM_SIZ_32b:
                    line_size /= G_IM_SIZ_32b_LINE_BYTES; // this is 2!
                    tex_height[i] /= 2;
                    break;
            }
            tex_width[i] = line_size;

            tex_width2[i] = (rdp.texture_tile[tile].lrs - rdp.texture_tile[tile].uls + 4) / 4;
            tex_height2[i] = (rdp.texture_tile[tile].lrt - rdp.texture_tile[tile].ult + 4) / 4;

            uint32_t tex_width1 = tex_width[i] << (cms & G_TX_MIRROR);
            uint32_t tex_height1 = tex_height[i] << (cmt & G_TX_MIRROR);

            if ((cms & G_TX_CLAMP) && ((cms & G_TX_MIRROR) || tex_width1 != tex_width2[i])) {
                tm |= 1 << 2 * i;
                cms &= ~G_TX_CLAMP;
            }
            if ((cmt & G_TX_CLAMP) && ((cmt & G_TX_MIRROR) || tex_height1 != tex_height2[i])) {
                tm |= 1 << (2 * i + 1);
                cmt &= ~G_TX_CLAMP;
            }

            if (rendering_state.textures[i]) {
                bool linear_filter = (rdp.other_mode_h & (3U << G_MDSFT_TEXTFILT)) != G_TF_POINT;
                if (linear_filter != rendering_state.textures[i]->second.linear_filter ||
                    cms != rendering_state.textures[i]->second.cms || cmt != rendering_state.textures[i]->second.cmt) {
                    gfx_flush();
                    gfx_rapi->set_sampler_parameters(i, linear_filter, cms, cmt, rdp.tex_lod);
                    rendering_state.textures[i]->second.linear_filter = linear_filter;
                    rendering_state.textures[i]->second.cms = cms;
                    rendering_state.textures[i]->second.cmt = cmt;
                }
            }
        }
    }

    struct ShaderProgram* prg = comb->prg[tm];
    if (prg == NULL) {
        comb->prg[tm] = prg =
            gfx_lookup_or_create_shader_program(comb->shader_id0, comb->shader_id1 | (tm * SHADER_OPT_TEXEL0_CLAMP_S));
    }
    if (prg != rendering_state.shader_program) {
        gfx_flush();
        gfx_rapi->unload_shader(rendering_state.shader_program);
        gfx_rapi->load_shader(prg);
        rendering_state.shader_program = prg;
    }
    if (use_alpha != rendering_state.alpha_blend || use_modulate != rendering_state.modulate) {
        gfx_flush();
        gfx_rapi->set_use_alpha(use_alpha, use_modulate);
        rendering_state.alpha_blend = use_alpha;
        rendering_state.modulate = use_modulate;
    }
    uint8_t num_inputs;
    bool used_textures[2];

    gfx_rapi->shader_get_info(prg, &num_inputs, used_textures);

    struct GfxClipParameters clip_parameters = gfx_rapi->get_clip_parameters();

    const size_t rec_tri_start = buf_vbo_len; // for the display-list cache tee

    for (int i = 0; i < 3; i++) {
        float z = v_arr[i]->z, w = v_arr[i]->w;
        if (clip_parameters.z_is_from_0_to_1) {
            z = (z + w) / 2.0f;
        }

        buf_vbo[buf_vbo_len++] = v_arr[i]->x;
        buf_vbo[buf_vbo_len++] = clip_parameters.invert_y ? -v_arr[i]->y : v_arr[i]->y;
        buf_vbo[buf_vbo_len++] = z;
        buf_vbo[buf_vbo_len++] = w;

        for (int t = 0; t < 2; t++) {
            if (!used_textures[t]) {
                continue;
            }

            // TODO: fix this; for now just ignore smaller mips
            const uint32_t tile = gfx_lod_tile_offset(t);

            float u = v_arr[i]->u / 32.0f;
            float v = v_arr[i]->v / 32.0f;

            int shifts = rdp.texture_tile[rdp.first_tile_index + tile].shifts;
            int shiftt = rdp.texture_tile[rdp.first_tile_index + tile].shiftt;
            if (shifts != 0) {
                if (shifts <= 10) {
                    u /= 1 << shifts;
                } else {
                    u *= 1 << (16 - shifts);
                }
            }
            if (shiftt != 0) {
                if (shiftt <= 10) {
                    v /= 1 << shiftt;
                } else {
                    v *= 1 << (16 - shiftt);
                }
            }

            u -= rdp.texture_tile[rdp.first_tile_index + tile].uls / 4.0f;
            v -= rdp.texture_tile[rdp.first_tile_index + tile].ult / 4.0f;

            if (!is_rect) {
                if (!(rdp.other_mode_h & G_TP_PERSP)) {
                    u *= 0.5f;
                    v *= 0.5f;
                }

                if ((rdp.other_mode_h & (3U << G_MDSFT_TEXTFILT)) != G_TF_POINT) {
                    // Linear filter adds 0.5f to the coordinates
                    u += 0.5f;
                    v += 0.5f;
                }
            }

            buf_vbo[buf_vbo_len++] = u / tex_width[t];
            buf_vbo[buf_vbo_len++] = v / tex_height[t];

            bool clampS = tm & (1 << 2 * t);
            bool clampT = tm & (1 << (2 * t + 1));

            if (clampS) {
                buf_vbo[buf_vbo_len++] = (tex_width2[t] - 0.5f) / tex_width[t];
            }
            if (clampT) {
                buf_vbo[buf_vbo_len++] = (tex_height2[t] - 0.5f) / tex_height[t];
            }
        }

        if (use_fog) {
            buf_vbo[buf_vbo_len++] = rdp.fog_color.r / 255.0f;
            buf_vbo[buf_vbo_len++] = rdp.fog_color.g / 255.0f;
            buf_vbo[buf_vbo_len++] = rdp.fog_color.b / 255.0f;
            buf_vbo[buf_vbo_len++] = v_arr[i]->fog / 255.0f; // fog factor
        }

        if (use_grayscale) {
            buf_vbo[buf_vbo_len++] = rdp.grayscale_color.r / 255.0f;
            buf_vbo[buf_vbo_len++] = rdp.grayscale_color.g / 255.0f;
            buf_vbo[buf_vbo_len++] = rdp.grayscale_color.b / 255.0f;
            buf_vbo[buf_vbo_len++] = rdp.grayscale_color.a / 255.0f; // lerp interpolation factor (not alpha)
        }

        for (int j = 0; j < num_inputs; j++) {
            struct RGBA* color = 0;
            struct RGBA tmp = { 0 };
            for (int k = 0; k < 1 + (use_alpha ? 1 : 0); k++) {
                switch (comb->shader_input_mapping[k][j]) {
                        // Note: CCMUX constants and ACMUX constants used here have same value, which is why this works
                        // (except LOD fraction).
                    case G_CCMUX_PRIMITIVE:
                        color = &rdp.prim_color;
                        break;
                    case G_CCMUX_SHADE:
                        color = &v_arr[i]->color;
                        break;
                    case G_CCMUX_SHADE_ALPHA:
                        tmp.r = tmp.g = tmp.b = v_arr[i]->color.a;
                        color = &tmp;
                        break;
                    case G_CCMUX_ENVIRONMENT:
                        color = &rdp.env_color;
                        break;
                    case G_CCMUX_PRIMITIVE_ALPHA: {
                        tmp.r = tmp.g = tmp.b = rdp.prim_color.a;
                        color = &tmp;
                        break;
                    }
                    case G_CCMUX_ENV_ALPHA: {
                        tmp.r = tmp.g = tmp.b = rdp.env_color.a;
                        color = &tmp;
                        break;
                    }
                    case G_CCMUX_PRIM_LOD_FRAC: {
                        tmp.r = tmp.g = tmp.b = rdp.prim_lod_fraction;
                        color = &tmp;
                        break;
                    }
                    case G_CCMUX_LOD_FRACTION: {
                        if (rdp.other_mode_h & G_TL_LOD) {
                            // HACK: very roughly eyeballed based on the carpets in Defection
                            // this is actually supposed to be calculated per pixel
                            const float distance_frac = std::max(0.f, std::min(w / 1024.f, 1.f));
                            tmp.r = tmp.g = tmp.b = tmp.a = (0.7f + distance_frac * 0.3f) * 255.f;
                        } else {
                            tmp.r = tmp.g = tmp.b = tmp.a = 255;
                        }
                        color = &tmp;
                        break;
                    }
                    case G_ACMUX_PRIM_LOD_FRAC:
                        tmp.a = rdp.prim_lod_fraction;
                        color = &tmp;
                        break;
                    default:
                        memset(&tmp, 0, sizeof(tmp));
                        color = &tmp;
                        break;
                }
                if (k == 0) {
                    buf_vbo[buf_vbo_len++] = color->r / 255.0f;
                    buf_vbo[buf_vbo_len++] = color->g / 255.0f;
                    buf_vbo[buf_vbo_len++] = color->b / 255.0f;
                } else {
                    buf_vbo[buf_vbo_len++] = color->a / 255.0f;
                }
            }
        }
    }

    // Display-list cache tee: record this (surviving, un-culled) triangle in
    // object space. Same attribute layout as buf_vbo, but the 4 position floats
    // are replaced by object-space (ox,oy,oz,1); the shader applies uMVP.
    if (g_DlCacheRecording && !g_DlCacheAbort) {
        int cull = 0;
        switch (rsp.geometry_mode & G_CULL_BOTH) {
        case G_CULL_FRONT: cull = 2; break;
        case G_CULL_BACK:  cull = 1; break;
        default:           cull = 0; break;
        }
        // Fog mode/params are also CPU-side state not captured by gfx_flush
        // boundaries (G_FOG geometry mode + the G_MW_FOG mul/offset).
        const bool fogc = (rsp.geometry_mode & G_FOG) != 0;

        // Close the open segment when its octree batch, cull mode, or fog state
        // changes (none captured by gfx_flush boundaries). A cull/fog change is a
        // GL state change (new state group); a batch change keeps the same state.
        if (g_DlCacheSegTris > 0) {
            const bool stateChanged = cull != g_DlCacheSegCull ||
                                      fogc != g_DlCacheSegFogCompute ||
                                      rsp.fog_mul != g_DlCacheSegFogMul ||
                                      rsp.fog_offset != g_DlCacheSegFogOff;
            if (stateChanged) {
                dlcacheCloseSegment();
                g_DlCacheStateGroup++;
            } else if (g_DlCacheBatchIndex != g_DlCacheSegBatch) {
                dlcacheCloseSegment();
            }
        }
        g_DlCacheSegCull = cull;
        g_DlCacheSegFogCompute = fogc;
        g_DlCacheSegFogMul = rsp.fog_mul;
        g_DlCacheSegFogOff = rsp.fog_offset;
        g_DlCacheSegBatch = g_DlCacheBatchIndex;

        // Shader-side palette: which input slots are shade (from the combiner), so
        // replay can route the live palette colour into them. Constant within a
        // segment (a combiner change is a shader change = flush = new segment):
        // bits per input slot j at j*3 -> rgb type (0/1/2) + alpha-shade bit.
        {
            int route = 0;
            for (int j = 0; j < num_inputs && j < 7; j++) {
                int r = 0;
                if (comb->shader_input_mapping[0][j] == G_CCMUX_SHADE) {
                    r |= 1;
                } else if (comb->shader_input_mapping[0][j] == G_CCMUX_SHADE_ALPHA) {
                    r |= 2;
                }
                if (use_alpha && comb->shader_input_mapping[1][j] == G_CCMUX_SHADE) {
                    r |= 4;
                }
                route |= (r & 7) << (j * 3);
            }
            g_DlCacheSegShadeRoute = route;
        }

        const size_t per_vtx = (buf_vbo_len - rec_tri_start) / 3;
        for (int i = 0; i < 3; i++) {
            const float* src = &buf_vbo[rec_tri_start + i * per_vtx];
            g_DlCacheStaging.push_back(v_arr[i]->ox);
            g_DlCacheStaging.push_back(v_arr[i]->oy);
            g_DlCacheStaging.push_back(v_arr[i]->oz);
            g_DlCacheStaging.push_back(1.0f);
            for (size_t f = 4; f < per_vtx; f++) {
                g_DlCacheStaging.push_back(src[f]);
            }
            // Trailing palette colour index (cached stride = buf num_floats + 1).
            g_DlCacheStaging.push_back(v_arr[i]->colour_index);
        }
        g_DlCacheSegTris++;
    }

    if (++buf_vbo_num_tris == MAX_BUFFERED) {
        gfx_flush();
    }
}

static inline void gfx_sp_tri4(Gfx *cmd) {
    // the game issues gSPTri2 for quads, which uses G_TRI4 with 2 empty triangles
    uint8_t x = C1(0, 4);
    uint8_t y = C1(4, 4);
    uint8_t z = C0(0, 4);

    if(x || y || z) {
        gfx_sp_tri1(x, y, z, false);
    }

    x = C1(8, 4);
    y = C1(12, 4);
    z = C0(4, 4);

    if (x || y || z) {
        gfx_sp_tri1(x, y, z, false);
    }

    x = C1(16, 4);
    y = C1(20, 4);
    z = C0(8, 4);

    if (x || y || z) {
        gfx_sp_tri1(x, y, z, false);
    }

    x = C1(24, 4);
    y = C1(28, 4);
    z = C0(12, 4);

    if (x || y || z) {
        gfx_sp_tri1(x, y, z, false);
    }
}

static void gfx_sp_geometry_mode(uint32_t clear, uint32_t set) {
    rsp.geometry_mode &= ~clear;
    rsp.geometry_mode |= set;
}

static inline void gfx_update_aspect_mode(void) {
    const uint32_t side = rsp.aspect_mode & G_ASPECT_CENTER_EXT;

    rsp.aspect_scale = rsp.aspect_mode ? gfx_current_native_aspect : gfx_current_window_dimensions.aspect_ratio;

    if (side == G_ASPECT_LEFT_EXT) {
        rsp.aspect_ofs = 1.f - gfx_current_dimensions.aspect_ratio / gfx_current_native_aspect;
    } else if (side == G_ASPECT_RIGHT_EXT) {
        rsp.aspect_ofs = gfx_current_dimensions.aspect_ratio / gfx_current_native_aspect - 1.f;
    } else {
        rsp.aspect_ofs = 0.f;
    }

    if (side && (rsp.aspect_mode & G_ASPECT_WIDE_EXT)) {
        constexpr float c = 16.f / 9.f;
        if (gfx_current_dimensions.aspect_ratio > c) {
            rsp.aspect_ofs *= c / gfx_current_dimensions.aspect_ratio;
        }
    }
}

static void gfx_sp_extra_geometry_mode(uint32_t clear, uint32_t set) {
    rsp.extra_geometry_mode &= ~clear;
    rsp.extra_geometry_mode |= set;
    rsp.aspect_mode = (rsp.extra_geometry_mode & G_ASPECT_MODE_EXT);
    gfx_update_aspect_mode();
}

static void gfx_adjust_viewport_or_scissor(XYWidthHeight* area, bool preserve_aspect = false) {
    // HACK: assume all target framebuffers have the same aspect
    // Use floor/ceil to ensure scissor fully contains the logical region
    // and prevents sub-pixel gaps at viewport edges
    float x1 = area->x * RATIO_X;
    float y1 = (SCREEN_HEIGHT - area->y) * RATIO_Y;
    float x2 = (area->x + area->width) * RATIO_X;
    float y2 = (SCREEN_HEIGHT - area->y + area->height) * RATIO_Y;
    
    area->x = std::floor(x1);
    area->y = std::floor(y1);
    area->width = std::ceil(x2) - area->x;
    area->height = std::ceil(y2) - area->y;
    
    if (preserve_aspect) {
        // preserve native aspect ratio
        const float ratio = gfx_current_native_aspect / gfx_current_dimensions.aspect_ratio;
        const float midx = gfx_current_dimensions.width * 0.5f;
        area->x = midx + (area->x - midx) * ratio;
        area->x += rsp.aspect_ofs * gfx_current_dimensions.width * 0.5f;
        area->width *= ratio;
    }

    if (!game_renders_to_framebuffer ||
        (gfx_msaa_level > 1 && gfx_current_dimensions.width == gfx_current_game_window_viewport.width &&
            gfx_current_dimensions.height == gfx_current_game_window_viewport.height)) {
        area->x += gfx_current_game_window_viewport.x;
        area->y += gfx_current_window_dimensions.height -
                    (gfx_current_game_window_viewport.y + gfx_current_game_window_viewport.height);
    }
}

static void gfx_calc_and_set_viewport(const Vp_t* viewport) {
    // 2 bits fraction
    float width = 2.0f * viewport->vscale[0] / 4.0f;
    float height = 2.0f * viewport->vscale[1] / 4.0f;
    float x = (viewport->vtrans[0] / 4.0f) - width / 2.0f;
    float y = ((viewport->vtrans[1] / 4.0f) + height / 2.0f);

    rdp.viewport.x = x;
    rdp.viewport.y = y;
    rdp.viewport.width = width;
    rdp.viewport.height = height;

    gfx_adjust_viewport_or_scissor(&rdp.viewport);

    rdp.viewport_or_scissor_changed = true;
}

static void gfx_sp_movemem(uint8_t index, uint8_t offset, const void* data) {
    switch (index) {
        case G_MV_VIEWPORT:
            gfx_calc_and_set_viewport((const Vp_t*)data);
            break;
        case G_MV_LOOKATY:
        case G_MV_LOOKATX:
            // I think this is only really used for guLookAtReflect
            index = !((index - G_MV_LOOKATY) / 2);
            rsp.lookat[index] = ((const Light *)data)->l;
            rsp.lookat_enabled = (index == 0) || (rsp.lookat[1].dir[0] || rsp.lookat[1].dir[1]);
            rsp.lights_changed = true;
            break;
        case G_MV_L0:
        case G_MV_L1:
        case G_MV_L2:
            // NOTE: reads out of bounds if it is an ambient light
            memcpy(rsp.current_lights + (index - G_MV_L0) / 2, data, sizeof(Light_t));
            break;
    }
}

static void gfx_sp_moveword(uint8_t index, uint16_t offset, uintptr_t data) {
    switch (index) {
        case G_MW_NUMLIGHT:
            // Ambient light is included
            // The 31th bit is a flag that lights should be recalculated
            rsp.current_num_lights = (data - 0x80000000U) / 32;
            rsp.lights_changed = 1;
            break;
        case G_MW_FOG:
            rsp.fog_mul = (int16_t)(data >> 16);
            rsp.fog_offset = (int16_t)data;
            break;
        case G_MW_SEGMENT:
            segmentPointers[(offset >> 2) & 0xff] = data;
            break;
    }
}

static void gfx_sp_texture(uint16_t sc, uint16_t tc, uint8_t level, uint8_t tile, uint8_t on) {
    rsp.texture_scaling_factor.s = sc;
    rsp.texture_scaling_factor.t = tc;
    rdp.tex_max_lod = level;
    if (rdp.first_tile_index != tile) {
        rdp.textures_changed[0] = true;
        rdp.textures_changed[1] = true;
        rdp.first_tile_index = tile;
    }
}

static void gfx_dp_set_scissor(uint32_t mode, uint32_t ulx, uint32_t uly, uint32_t lrx, uint32_t lry) {
    float x = ulx / 4.0f;
    float y = lry / 4.0f;
    float width = (lrx - ulx) / 4.0f;
    float height = (lry - uly) / 4.0f;

    rdp.scissor.x = x;
    rdp.scissor.y = y;
    rdp.scissor.width = width;
    rdp.scissor.height = height;

    gfx_adjust_viewport_or_scissor(&rdp.scissor, rsp.aspect_mode != 0);

    rdp.viewport_or_scissor_changed = true;
}

static void gfx_dp_set_texture_image(uint32_t format, uint32_t size, uint32_t width, uint32_t tex_flags, const void* addr) {
    rdp.texture_to_load.addr = (const uint8_t*)addr;
    rdp.texture_to_load.siz = size;
    rdp.texture_to_load.width = width;
    rdp.texture_to_load.tex_flags = tex_flags;
}

// ext_tex (rafccq/port-ext-textures): G_SETTEXINFO_EXT handler — remembers the
// game texture identity for the next load so gfx_dp_load_block can check for a
// data/ext_tex replacement
static void gfx_dp_set_texture_info(uint8_t type, uint8_t id_mask, uint16_t id, uint32_t texnum) {
    rdp.texture_to_load.type = type;
    rdp.texture_to_load.texnum = texnum;
    rdp.texture_to_load.id = id;
    rdp.texture_to_load.id_mask = id_mask << 8;
}

static inline uint64_t make_key(bool external, uint64_t type, uint64_t id, uint32_t texnum) {
    return (uint64_t)external << (7 * 8) | type << (6 * 8) | id << (4 * 8) | texnum;
}

static void gfx_dp_set_tile(uint8_t fmt, uint32_t siz, uint32_t line, uint32_t tmem, uint8_t tile, uint32_t palette,
                            uint32_t cmt, uint32_t maskt, uint32_t shiftt, uint32_t cms, uint32_t masks,
                            uint32_t shifts) {
    // OTRTODO:
    // SUPPORT_CHECK(tmem == 0 || tmem == 256);
    static uint32_t max_tmem = 0;
    if (cms == G_TX_WRAP && masks == G_TX_NOMASK) {
        cms = G_TX_CLAMP;
    }
    if (cmt == G_TX_WRAP && maskt == G_TX_NOMASK) {
        cmt = G_TX_CLAMP;
    }

    if (fmt == G_IM_FMT_RGBA && siz < G_IM_SIZ_16b) {
        // HACK: sometimes the game will submit G_IM_FMT_RGBA, G_IM_SIZ_8b/4b, intending it to read as CI8/CI4 with RGBA16 palette
        fmt = G_IM_FMT_CI;
    } else if (fmt == G_IM_FMT_IA && siz == G_IM_SIZ_32b) {
        // HACK: ... and sometimes it submits this, apparently intending it to be I8
        fmt = G_IM_FMT_I;
        siz = G_IM_SIZ_8b;
    }

    rdp.texture_tile[tile].palette = palette; // palette should set upper 4 bits of color index in 4b mode
    rdp.texture_tile[tile].fmt = fmt;
    rdp.texture_tile[tile].siz = siz;
    rdp.texture_tile[tile].cms = cms;
    rdp.texture_tile[tile].cmt = cmt;
    rdp.texture_tile[tile].shifts = shifts;
    rdp.texture_tile[tile].shiftt = shiftt;
    rdp.texture_tile[tile].line_size_bytes = line * 8;
    rdp.texture_tile[tile].tmem = tmem;

    rdp.textures_changed[0] = true;
    rdp.textures_changed[1] = true;
}

static void gfx_dp_set_tile_size(uint8_t tile, uint16_t uls, uint16_t ult, uint16_t lrs, uint16_t lrt) {
    rdp.texture_tile[tile].uls = uls;
    rdp.texture_tile[tile].ult = ult;
    rdp.texture_tile[tile].lrs = lrs;
    rdp.texture_tile[tile].lrt = lrt;
    rdp.texture_tile[tile].width = (lrs - uls + 4) / 4;
    rdp.texture_tile[tile].height = (lrt - ult + 4) / 4;
    rdp.textures_changed[0] = true;
    rdp.textures_changed[1] = true;
}

static void gfx_dp_load_tlut(uint8_t tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t lrt) {
    // SUPPORT_CHECK(tile == G_TX_LOADTILE);
    SUPPORT_CHECK(rdp.texture_to_load.siz == G_IM_SIZ_16b);
    SUPPORT_CHECK(rdp.texture_tile[tile].tmem >= 256);

    rdp.texture_tile[tile].uls = uls;
    rdp.texture_tile[tile].ult = ult;
    rdp.texture_tile[tile].lrs = lrs;
    rdp.texture_tile[tile].lrt = lrt;

    const uint32_t width = (lrs - uls + 1);
    const uint32_t height = (lrt - ult + 1);
    const uint32_t pitch = rdp.texture_to_load.width + 1;
    const uint32_t count =  width * height;
    const uint16_t *base = (const uint16_t *)rdp.texture_to_load.addr + pitch * ult + uls;

    if (rdp.texture_tile[tile].tmem == 256) {
        rdp.palette_addrs[0] = (const uint8_t *)base;
        if (count >= 256) {
            rdp.palette_addrs[1] = (const uint8_t *)(base + 128);
        }
    } else {
        rdp.palette_addrs[1] = (const uint8_t *)base;
    }

    const uint32_t palofs = rdp.texture_tile[tile].tmem - 256;
    SUPPORT_CHECK(palofs + count <= 256);

    const uint16_t *src = base;
    uint16_t *dst = rdp.palette + palofs;
    for (uint32_t i = 0; i < count; ++i) {
        *dst++ = PD_BE16(*src++);
    }

    rdp.textures_changed[0] = rdp.textures_changed[1] = true;
}

static void gfx_dp_load_block(uint8_t tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t dxt) {
    // SUPPORT_CHECK(tile == G_TX_LOADTILE);
    SUPPORT_CHECK(uls == 0);
    SUPPORT_CHECK(ult == 0);

    // The lrs field rather seems to be number of pixels to load
    uint32_t orig_size_bytes = (lrs + 1) << rdp.texture_to_load.siz >> 1;
    uint32_t size_bytes = orig_size_bytes;
    if (rdp.texture_to_load.raw_tex_metadata.h_byte_scale != 1 ||
        rdp.texture_to_load.raw_tex_metadata.v_pixel_scale != 1) {
        size_bytes *= rdp.texture_to_load.raw_tex_metadata.h_byte_scale;
        size_bytes *= rdp.texture_to_load.raw_tex_metadata.v_pixel_scale;
    }

    LoadedTexture& loaded_texture = rdp.loaded_texture[rdp.texture_tile[tile].tmem];
    loaded_texture.orig_size_bytes = orig_size_bytes;
    loaded_texture.size_bytes = size_bytes;
    loaded_texture.full_size_bytes = size_bytes;
    loaded_texture.line_size_bytes = size_bytes;
    loaded_texture.full_image_line_size_bytes = size_bytes;
    loaded_texture.tex_flags = rdp.texture_to_load.tex_flags;
    loaded_texture.raw_tex_metadata = rdp.texture_to_load.raw_tex_metadata;
    loaded_texture.addr = rdp.texture_to_load.addr;

    // ext_tex: if this load was tagged by G_SETTEXINFO_EXT and a replacement
    // PNG exists, flag the slot external (import_texture short-circuits).
    // .addr stays set (unlike upstream) so the original data still imports
    // when the feature is toggled off mid-session.
    {
        uint8_t type = rdp.texture_to_load.type;
        uint16_t id = rdp.texture_to_load.id;
        uint16_t id_mask = rdp.texture_to_load.id_mask;
        uint32_t texnum = rdp.texture_to_load.texnum;

        if (gfx_external_textures_enabled && extTexExists(type, id, texnum)) {
            loaded_texture.type = type;
            loaded_texture.id = id;
            loaded_texture.texnum = texnum;
            loaded_texture.id_mask = id_mask;
            loaded_texture.ext_key = make_key(1, type, id, texnum);

            // clear the masked id if its related texture doesn't exist
            if (id_mask != 0 && !extTexExists(type, id | id_mask, texnum)) {
                loaded_texture.id_mask = 0;
            }
        } else {
            loaded_texture.ext_key = 0;
        }

        // one-shot: unlike upstream (which tags every SETTIMG site), not all
        // our load sites emit G_SETTEXINFO_EXT — consume the tag here so a
        // stale one can't mis-substitute the next untagged load
        rdp.texture_to_load.type = G_TEXTYPE_NONE;
        rdp.texture_to_load.id = 0;
        rdp.texture_to_load.id_mask = 0;
        rdp.texture_to_load.texnum = 0;
    }

    rdp.textures_changed[0] = rdp.textures_changed[1] = true;
}

static void gfx_dp_load_tile(uint8_t tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t lrt) {
    SUPPORT_CHECK(tile == G_TX_LOADTILE);

    uint32_t offset_x = uls >> G_TEXTURE_IMAGE_FRAC;
    uint32_t offset_y = ult >> G_TEXTURE_IMAGE_FRAC;
    uint32_t tile_width = ((lrs - uls) >> G_TEXTURE_IMAGE_FRAC) + 1;
    uint32_t tile_height = ((lrt - ult) >> G_TEXTURE_IMAGE_FRAC) + 1;
    uint32_t full_image_width = rdp.texture_to_load.width + 1;

    uint32_t offset_x_in_bytes = offset_x << rdp.texture_to_load.siz >> 1;
    uint32_t tile_line_size_bytes = tile_width << rdp.texture_to_load.siz >> 1;
    uint32_t full_image_line_size_bytes = full_image_width << rdp.texture_to_load.siz >> 1;

    uint32_t orig_size_bytes = tile_line_size_bytes * tile_height;
    uint32_t size_bytes = orig_size_bytes;
    uint32_t start_offset_bytes = full_image_line_size_bytes * offset_y + offset_x_in_bytes;

    float h_byte_scale = rdp.texture_to_load.raw_tex_metadata.h_byte_scale;
    float v_pixel_scale = rdp.texture_to_load.raw_tex_metadata.v_pixel_scale;

    if (h_byte_scale != 1 || v_pixel_scale != 1) {
        start_offset_bytes = h_byte_scale * (v_pixel_scale * offset_y * full_image_line_size_bytes + offset_x_in_bytes);
        size_bytes *= h_byte_scale * v_pixel_scale;
        full_image_line_size_bytes *= h_byte_scale;
        tile_line_size_bytes *= h_byte_scale;
    }

    LoadedTexture& loaded_texture = rdp.loaded_texture[rdp.texture_tile[tile].tmem];
    loaded_texture.orig_size_bytes = orig_size_bytes;
    loaded_texture.size_bytes = size_bytes;
    loaded_texture.full_size_bytes = full_image_line_size_bytes * tile_height;
    loaded_texture.full_image_line_size_bytes = full_image_line_size_bytes;
    loaded_texture.line_size_bytes = tile_line_size_bytes;
    loaded_texture.tex_flags = rdp.texture_to_load.tex_flags;
    loaded_texture.raw_tex_metadata = rdp.texture_to_load.raw_tex_metadata;
    loaded_texture.addr = rdp.texture_to_load.addr + start_offset_bytes;
    loaded_texture.ext_key = 0; // LOADTILE loads are never ext-tagged; clear any stale tag

    rdp.texture_tile[tile].uls = uls;
    rdp.texture_tile[tile].ult = ult;
    rdp.texture_tile[tile].lrs = lrs;
    rdp.texture_tile[tile].lrt = lrt;
    rdp.texture_tile[tile].width = ((lrs - uls) >> G_TEXTURE_IMAGE_FRAC) + 1;
    rdp.texture_tile[tile].height = ((lrt - ult) >> G_TEXTURE_IMAGE_FRAC) + 1;

    rdp.textures_changed[0] = rdp.textures_changed[1] = true;
}

static void gfx_dp_set_combine_mode(uint32_t rgb, uint32_t alpha, uint32_t rgb_cyc2, uint32_t alpha_cyc2) {
    rdp.combine_mode = rgb | (alpha << 16) | ((uint64_t)rgb_cyc2 << 28) | ((uint64_t)alpha_cyc2 << 44);
}

static inline uint32_t color_comb(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    return (a & 0xf) | ((b & 0xf) << 4) | ((c & 0x1f) << 8) | ((d & 7) << 13);
}

static inline uint32_t alpha_comb(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    return (a & 7) | ((b & 7) << 3) | ((c & 7) << 6) | ((d & 7) << 9);
}

static void gfx_dp_set_grayscale_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    rdp.grayscale_color.r = r;
    rdp.grayscale_color.g = g;
    rdp.grayscale_color.b = b;
    rdp.grayscale_color.a = a;
}

static void gfx_dp_set_env_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    rdp.env_color.r = r;
    rdp.env_color.g = g;
    rdp.env_color.b = b;
    rdp.env_color.a = a;
}

static void gfx_dp_set_prim_color(uint8_t m, uint8_t l, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    rdp.prim_lod_fraction = l;
    rdp.prim_color.r = r;
    rdp.prim_color.g = g;
    rdp.prim_color.b = b;
    rdp.prim_color.a = a;
    rdp.fill_color.r = r;
    rdp.fill_color.g = g;
    rdp.fill_color.b = b;
    rdp.fill_color.a = a;
    rdp.tex_min_lod = m;

}

static void gfx_dp_set_fog_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    rdp.fog_color.r = r;
    rdp.fog_color.g = g;
    rdp.fog_color.b = b;
    rdp.fog_color.a = a;
}

static void gfx_dp_set_fill_color(uint32_t packed_color) {
    uint16_t col16 = (uint16_t)packed_color;
    uint32_t r = col16 >> 11;
    uint32_t g = (col16 >> 6) & 0x1f;
    uint32_t b = (col16 >> 1) & 0x1f;
    uint32_t a = col16 & 1;
    rdp.fill_color.r = SCALE_5_8(r);
    rdp.fill_color.g = SCALE_5_8(g);
    rdp.fill_color.b = SCALE_5_8(b);
    rdp.fill_color.a = a * 255;
}

static void gfx_dp_set_subpixel_offset(int16_t x, int16_t y) {
    rdp.subpixel_ofs_x = x;
    rdp.subpixel_ofs_y = y;
}

static void gfx_draw_rectangle(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry) {
    uint32_t saved_other_mode_h = rdp.other_mode_h;
    uint32_t cycle_type = (rdp.other_mode_h & (3U << G_MDSFT_CYCLETYPE));

    if (cycle_type == G_CYC_COPY) {
        rdp.other_mode_h = (rdp.other_mode_h & ~(3U << G_MDSFT_TEXTFILT)) | G_TF_POINT;
    }

    ulx += rdp.subpixel_ofs_x;
    lrx += rdp.subpixel_ofs_x;
    uly += rdp.subpixel_ofs_y;
    lry += rdp.subpixel_ofs_y;

    // U10.2 coordinates
    float ulxf = ulx;
    float ulyf = uly;
    float lrxf = lrx;
    float lryf = lry;

    ulxf = ulxf / (4.0f * HALF_SCREEN_WIDTH) - 1.0f;
    ulyf = -(ulyf / (4.0f * HALF_SCREEN_HEIGHT)) + 1.0f;
    lrxf = lrxf / (4.0f * HALF_SCREEN_WIDTH) - 1.0f;
    lryf = -(lryf / (4.0f * HALF_SCREEN_HEIGHT)) + 1.0f;

    ulxf = gfx_adjust_x_for_aspect_ratio(ulxf);
    lrxf = gfx_adjust_x_for_aspect_ratio(lrxf);

    struct LoadedVertex* ul = &rsp.loaded_vertices[MAX_VERTICES + 0];
    struct LoadedVertex* ll = &rsp.loaded_vertices[MAX_VERTICES + 1];
    struct LoadedVertex* lr = &rsp.loaded_vertices[MAX_VERTICES + 2];
    struct LoadedVertex* ur = &rsp.loaded_vertices[MAX_VERTICES + 3];

    ul->x = ulxf;
    ul->y = ulyf;
    ul->z = -1.0f;
    ul->w = 1.0f;

    ll->x = ulxf;
    ll->y = lryf;
    ll->z = -1.0f;
    ll->w = 1.0f;

    lr->x = lrxf;
    lr->y = lryf;
    lr->z = -1.0f;
    lr->w = 1.0f;

    ur->x = lrxf;
    ur->y = ulyf;
    ur->z = -1.0f;
    ur->w = 1.0f;

    // The coordinates for texture rectangle shall bypass the viewport setting
    struct XYWidthHeight default_viewport = { 0, (int16_t)SCREEN_HEIGHT, (uint32_t)SCREEN_WIDTH, (uint32_t)SCREEN_HEIGHT };
    struct XYWidthHeight viewport_saved = rdp.viewport;
    uint32_t geometry_mode_saved = rsp.geometry_mode;

    gfx_adjust_viewport_or_scissor(&default_viewport);

    rdp.viewport = default_viewport;
    rdp.viewport_or_scissor_changed = true;
    rsp.geometry_mode = 0;

    gfx_sp_tri1(MAX_VERTICES + 0, MAX_VERTICES + 1, MAX_VERTICES + 3, true);
    gfx_sp_tri1(MAX_VERTICES + 1, MAX_VERTICES + 2, MAX_VERTICES + 3, true);

    rsp.geometry_mode = geometry_mode_saved;
    rdp.viewport = viewport_saved;
    rdp.viewport_or_scissor_changed = true;

    if (cycle_type == G_CYC_COPY) {
        rdp.other_mode_h = saved_other_mode_h;
    }
}

static void gfx_dp_texture_rectangle(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry, uint8_t tile, int16_t uls,
                                     int16_t ult, int16_t dsdx, int16_t dtdy, bool flip) {
    uint64_t saved_combine_mode = rdp.combine_mode;
    if ((rdp.other_mode_h & (3U << G_MDSFT_CYCLETYPE)) == G_CYC_COPY) {
        // Per RDP Command Summary Set Tile's shift s and this dsdx should be set to 4 texels
        // Divide by 4 to get 1 instead
        dsdx >>= 2;

        // Color combiner is turned off in copy mode
        gfx_dp_set_combine_mode(color_comb(0, 0, 0, G_CCMUX_TEXEL0), alpha_comb(0, 0, 0, G_ACMUX_TEXEL0), 0, 0);

        // Per documentation one extra pixel is added in this modes to each edge
        lrx += 1 << 2;
        lry += 1 << 2;
    }

    // uls and ult are S10.5
    // dsdx and dtdy are S5.10
    // lrx, lry, ulx, uly are U10.2
    // lrs, lrt are S10.5

    const int16_t width = flip ? lry - uly : lrx - ulx;
    const int16_t height = flip ? lrx - ulx : lry - uly;
    const float lrs = ((uls << 7) + dsdx * width) >> 7;
    const float lrt = ((ult << 7) + dtdy * height) >> 7;

    struct LoadedVertex* ul = &rsp.loaded_vertices[MAX_VERTICES + 0];
    struct LoadedVertex* ll = &rsp.loaded_vertices[MAX_VERTICES + 1];
    struct LoadedVertex* lr = &rsp.loaded_vertices[MAX_VERTICES + 2];
    struct LoadedVertex* ur = &rsp.loaded_vertices[MAX_VERTICES + 3];
    ul->u = uls;
    ul->v = ult;
    lr->u = lrs;
    lr->v = lrt;
    if (!flip) {
        ll->u = uls;
        ll->v = lrt;
        ur->u = lrs;
        ur->v = ult;
    } else {
        ll->u = lrs;
        ll->v = ult;
        ur->u = uls;
        ur->v = lrt;
    }

    uint8_t saved_tile = rdp.first_tile_index;
    if (saved_tile != tile) {
        rdp.textures_changed[0] = true;
        rdp.textures_changed[1] = true;
    }
    rdp.first_tile_index = tile;

    gfx_draw_rectangle(ulx, uly, lrx, lry);
    if (saved_tile != tile) {
        rdp.textures_changed[0] = true;
        rdp.textures_changed[1] = true;
    }
    rdp.first_tile_index = saved_tile;
    rdp.combine_mode = saved_combine_mode;
}

static void gfx_dp_image_rectangle(int32_t tile, int32_t w, int32_t h,
                                   int32_t ulx, int32_t uly, int16_t uls, int16_t ult,
                                   int32_t lrx, int32_t lry, int16_t lrs, int16_t lrt) {
    uint64_t saved_combine_mode = rdp.combine_mode;

    struct LoadedVertex* ul = &rsp.loaded_vertices[MAX_VERTICES + 0];
    struct LoadedVertex* ll = &rsp.loaded_vertices[MAX_VERTICES + 1];
    struct LoadedVertex* lr = &rsp.loaded_vertices[MAX_VERTICES + 2];
    struct LoadedVertex* ur = &rsp.loaded_vertices[MAX_VERTICES + 3];
    ul->u = uls * 32;
    ul->v = ult * 32;
    lr->u = lrs * 32;
    lr->v = lrt * 32;
    ll->u = uls * 32;
    ll->v = lrt * 32;
    ur->u = lrs * 32;
    ur->v = ult * 32;

    // ensure we have the correct texture size
    rdp.texture_tile[tile].line_size_bytes = w << rdp.texture_tile[tile].siz >> 1;
    rdp.texture_tile[tile].width = w;
    rdp.texture_tile[tile].height = h;
    rdp.texture_tile[tile].cms = 0;
    rdp.texture_tile[tile].cmt = 0;
    rdp.texture_tile[tile].shifts = 0;
    rdp.texture_tile[tile].shiftt = 0;
    auto& loadtex = rdp.loaded_texture[rdp.texture_tile[tile].tmem];
    loadtex.full_image_line_size_bytes = loadtex.line_size_bytes = rdp.texture_tile[tile].line_size_bytes;
    loadtex.size_bytes = loadtex.orig_size_bytes = loadtex.full_size_bytes = loadtex.line_size_bytes * h;

    uint8_t saved_tile = rdp.first_tile_index;
    if (saved_tile != tile) {
        rdp.textures_changed[0] = true;
        rdp.textures_changed[1] = true;
    }
    rdp.first_tile_index = tile;

    gfx_draw_rectangle(ulx, uly, lrx, lry);
    if (saved_tile != tile) {
        rdp.textures_changed[0] = true;
        rdp.textures_changed[1] = true;
    }
    rdp.first_tile_index = saved_tile;

    rdp.combine_mode = saved_combine_mode;
}

static void gfx_dp_fill_rectangle(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry) {
    if (rdp.color_image_address == rdp.z_buf_address) {
        // Don't clear Z buffer here since we already did it with glClear
        return;
    }
    uint32_t mode = (rdp.other_mode_h & (3U << G_MDSFT_CYCLETYPE));

    // OTRTODO: This is a bit of a hack for widescreen screen fades, but it'll work for now...
    if (ulx == 0 && uly == 0 && lrx == 319 * 4 && lry == 239 * 4) {
        ulx = -1024;
        uly = -1024;
        lrx = 2048;
        lry = 2048;
    }

    if (mode == G_CYC_COPY || mode == G_CYC_FILL) {
        // Per documentation one extra pixel is added in this modes to each edge
        lrx += 1 << 2;
        lry += 1 << 2;
    }

    for (int i = MAX_VERTICES; i < MAX_VERTICES + 4; i++) {
        struct LoadedVertex* v = &rsp.loaded_vertices[i];
        v->color = rdp.fill_color;
    }

    uint64_t saved_combine_mode = rdp.combine_mode;

    if (mode == G_CYC_FILL) {
        gfx_dp_set_combine_mode(color_comb(0, 0, 0, G_CCMUX_SHADE), alpha_comb(0, 0, 0, G_ACMUX_SHADE), 0, 0);
    }

    gfx_draw_rectangle(ulx, uly, lrx, lry);
    rdp.combine_mode = saved_combine_mode;
}

static void gfx_dp_set_z_image(void* z_buf_address) {
    rdp.z_buf_address = z_buf_address;
}

static void gfx_dp_set_color_image(uint32_t format, uint32_t size, uint32_t width, void* address) {
    rdp.color_image_address = address;
}

static void gfx_sp_set_other_mode(uint32_t shift, uint32_t num_bits, uint64_t mode) {
    uint64_t mask = (((uint64_t)1 << num_bits) - 1) << shift;
    uint64_t om = rdp.other_mode_l | ((uint64_t)rdp.other_mode_h << 32);
    om = (om & ~mask) | mode;
    rdp.other_mode_l = (uint32_t)om;
    rdp.other_mode_h = (uint32_t)(om >> 32);
    rdp.palette_fmt = rdp.other_mode_h & (3U << G_MDSFT_TEXTLUT);
    rdp.tex_lod = (rdp.other_mode_h & G_TL_LOD) != 0;
    rdp.tex_detail = (rdp.other_mode_h & (2U << G_MDSFT_TEXTDETAIL)) == G_TD_DETAIL;
}

static void gfx_sp_set_vertex_colors(uint32_t count, const struct NormalColor *vcn) {
    // common sense dictates that we should copy the colors as the command is supposed to do,
    // but it actually doesn't seem to matter
    // SUPPORT_CHECK(count <= sizeof(rsp.vertex_colors) / sizeof(rsp.vertex_colors[0]));
    // for (uint32_t i = 0; i < count; ++i) {
    //     rsp.vertex_colors[i] = vcn[i];
    // }
    rsp.vertex_colors = vcn;
}

static void gfx_dp_set_other_mode(uint32_t h, uint32_t l) {
    rdp.other_mode_h = h;
    rdp.other_mode_l = l;
}

static inline void *seg_addr(uintptr_t w1) {
    // all segmented addresses have the least significant bit set
    if (w1 & 1) {
        // seg 0 is reserved and doesn't count here
        const uintptr_t seg = (w1 & 0x0f000000) >> 24;
        if (seg && segmentPointers[seg]) {
            const uintptr_t addr = (w1 & 0x00fffffe);
            return (void *)(segmentPointers[seg] + addr);
        }
    }
    return (void *)w1;
}

uintptr_t clearMtx;

static void dlcacheBeginRecord(const void* key) {
    gfx_flush(); // close the pending immediate batch before recording starts
    DlCacheEntry& e = g_DlCache[key];
    e.bad = false;
    e.ready = false;
    e.buffer_id = 0;
    e.segments.clear();
    e.palette_ok = false;
    e.palette_tex = 0;
    e.palette_w1 = 0;
    e.palette_count = 0;
    g_DlCacheCur = &e;
    g_DlCacheRecording = true;
    g_DlCacheAbort = false;
    g_DlCacheStaging.clear();
    g_DlCacheSegStartFloat = 0;
    g_DlCacheSegTris = 0;
    g_DlCacheSegCull = 0;
    g_DlCacheStateGroup = 0;
    g_DlCacheBatchIndex = -1; // first G_VTX -> batch 0
    g_DlCacheSegBatch = -1;
    g_DlCacheSegFogCompute = false;
    g_DlCacheSegFogMul = 0;
    g_DlCacheSegFogOff = 0;
    g_DlCacheSegShadeRoute = 0;
    g_DlCacheGColSeen = false;
    g_DlCacheGColMulti = false;
    g_DlCacheGColW1 = 0;
    g_DlCacheGColCount = 0;
}

static void dlcacheEndRecord(void) {
    if (!g_DlCacheRecording) {
        return;
    }
    gfx_flush(); // draw + close the final segment
    g_DlCacheRecording = false;
    DlCacheEntry* e = g_DlCacheCur;
    g_DlCacheCur = NULL;
    if (e == NULL) {
        return;
    }
    if (g_DlCacheAbort || e->segments.empty() || g_DlCacheStaging.empty()) {
        e->bad = true;
        e->segments.clear();
        e->segments.shrink_to_fit();
        if (!g_DlCacheAbort) {
            g_DlCacheAbortReasons |= GFX_DLC_ABORT_EMPTY;
        }
        return;
    }
    e->buffer_id = gfx_rapi->cache_create_buffer(g_DlCacheStaging.data(), g_DlCacheStaging.size());
    if (e->buffer_id == 0) {
        e->bad = true;
        e->segments.clear();
        return;
    }
    e->ready = true;

    // Shader-side GPU palette: if the leaf had exactly one resolvable G_COL, build a
    // palette texture from the live colours. Dynamic lighting then updates at cache
    // speed (the dirty bit re-uploads this texture instead of re-recording the VBO).
    if (g_DlCacheGColSeen && !g_DlCacheGColMulti && g_DlCacheGColCount > 0) {
        void* pal = seg_addr(g_DlCacheGColW1);
        if (pal != NULL) {
            e->palette_tex = gfx_rapi->cache_create_palette();
            if (e->palette_tex != 0) {
                gfx_rapi->cache_upload_palette(e->palette_tex, pal, g_DlCacheGColCount);
                e->palette_w1 = g_DlCacheGColW1;
                e->palette_count = g_DlCacheGColCount;
                e->palette_ok = true;
            }
        }
    }
}

// vis: per-batch visibility (vis[seg.batch_index]) for octree culling this frame,
// or NULL to draw every batch (octree inactive for this room).
static void dlcacheReplay(DlCacheEntry* e, const uint8_t* vis) {
    gfx_flush();

    // uMVP = (aspect-X scale/offset, invert-Y) folded into the live room
    // MP_matrix. GL desktop keeps z as-is (clip z_is_from_0_to_1 == false);
    // 0..1-clip backends (SDL_GPU) additionally fold the z = (z + w) / 2
    // remap the immediate path applies CPU-side — without it cached depth
    // lands in [-w, w], clamps to 0 for the near half, and rooms composite
    // in draw order instead of by depth.
    float (*M)[4] = rsp.MP_matrix;
    const struct GfxClipParameters clip = gfx_rapi->get_clip_parameters();
    const float sy = clip.invert_y ? -1.0f : 1.0f;
    float sx, ox;
    if (fbActive) {
        sx = 1.0f;
        ox = 0.0f;
    } else {
        sx = rsp.aspect_scale / gfx_current_dimensions.aspect_ratio;
        ox = rsp.aspect_ofs;
    }
    // CHEAT_MIRROR: reflect cached geometry left-right by negating the X scale
    // folded into uMVP (the cached-replay equivalent of the immediate path's
    // clip-space X negate). Winding is compensated below via cache_set_cull.
    const float mx = gfx_mirror_mode ? -1.0f : 1.0f;
    float mvp[16]; // column-major
    for (int k = 0; k < 4; k++) {
        mvp[k * 4 + 0] = mx * sx * (M[k][0] + ox * M[k][3]);
        mvp[k * 4 + 1] = sy * M[k][1];
        mvp[k * 4 + 2] = clip.z_is_from_0_to_1 ? 0.5f * (M[k][2] + M[k][3]) : M[k][2];
        mvp[k * 4 + 3] = M[k][3];
    }
    gfx_rapi->set_mvp(mvp);

    // We drive GL directly during replay; disable the pre-replay shader's attribs.
    gfx_rapi->unload_shader(rendering_state.shader_program);
    gfx_rapi->cache_replay_begin(e->buffer_id);

    // Viewport + scissor are VIEW-dependent (the scissor is the room's portal-
    // clipped draw-slot box, which moves/shrinks as the camera turns), so use the
    // LIVE values set for this room this frame -- NOT the ones baked at record
    // time, which would clip the room to where it used to be on screen (rooms
    // vanish when you turn). They're constant across a leaf's segments, so set once.
    gfx_rapi->set_viewport(rdp.viewport.x, rdp.viewport.y, rdp.viewport.width, rdp.viewport.height);
    const float dlc_scx = gfx_mirror_mode ? gfx_mirror_scissor_x(rdp.scissor.x, rdp.scissor.width) : rdp.scissor.x;
    gfx_rapi->set_scissor(dlc_scx, rdp.scissor.y, rdp.scissor.width, rdp.scissor.height);

    // Shader-side GPU palette: bind the live palette texture + enable the lookup so
    // shade is resolved on the GPU (dynamic lighting at cache speed). Per-segment
    // shade routing is set in the state-group block below.
    const bool use_palette = e->palette_ok && g_DlCachePaletteEnabled;
    if (use_palette) {
        gfx_rapi->cache_bind_palette(e->palette_tex, e->palette_count);
        gfx_rapi->set_palette_enable(1);
    }

    // Walk segments, applying GL state once per state group and merging
    // contiguous visible batches into one draw. Segments are buffer-contiguous by
    // construction, so a merged draw just sums num_tris; a state-group change
    // flushes the pending draw. Octree-culled batches are GAP-TOLERANT: a culled
    // hole of <= g_DlCacheGapTris tris is absorbed into the surrounding draw
    // (its tris are outside the frustum — the GPU clips them after trivial
    // vertex shading), keeping the draw count low; only a bigger culled span
    // (or a hole whose program/stride differs, which can't share a draw range)
    // splits the merge.
    struct ShaderProgram* curprg = NULL;
    uint32_t applied_group = (uint32_t)-1;
    struct ShaderProgram* pend_prg = NULL;
    size_t pend_base = 0;
    size_t pend_tris = 0;
    size_t gap_tris = 0;    // culled tris accumulated since the last visible segment
    uint32_t gap_batches = 0; // culled batches in that gap (stats only)
    uint32_t drawn_batches = 0;

    for (const DlCacheSegment& seg : e->segments) {
        const bool visible = (vis == NULL) || (seg.batch_index < 0) || vis[seg.batch_index];
        if (!visible) {
            if (pend_tris == 0) {
                g_DlCacheFrameVisCulled++;
                continue; // leading hole: nothing pending to extend, just skip
            }
            if (seg.prg == pend_prg) {
                // defer the split decision to the next visible seg
                gap_tris += seg.num_tris;
                gap_batches++;
            } else {
                // different program = different vertex stride: the pending draw
                // range can't span this segment, so the split is forced
                gfx_rapi->cache_draw(pend_prg, pend_base, pend_tris);
                g_DlCacheFrameTris += (uint32_t)pend_tris;
                g_DlCacheFrameDraws++;
                pend_tris = 0;
                g_DlCacheFrameVisCulled += gap_batches + 1;
                gap_tris = 0;
                gap_batches = 0;
            }
            continue;
        }
        if (seg.state_group != applied_group) {
            if (pend_tris > 0) {
                gfx_rapi->cache_draw(pend_prg, pend_base, pend_tris);
                g_DlCacheFrameTris += (uint32_t)pend_tris;
                g_DlCacheFrameDraws++;
                pend_tris = 0;
            }
            // a trailing hole before a state change is never drawn
            g_DlCacheFrameVisCulled += gap_batches;
            gap_tris = 0;
            gap_batches = 0;
            gfx_rapi->set_depth_mode(seg.depth_test, seg.depth_update, seg.depth_compare,
                                     seg.depth_source_prim, seg.zmode);
            // viewport/scissor are set once above from live state (view-dependent).
            gfx_rapi->set_use_alpha(seg.alpha_blend, seg.modulate);
            for (int i = 0; i < 2; i++) {
                if (seg.tex_used[i]) {
                    gfx_rapi->select_texture(i, seg.tex_id[i], seg.tex_linear[i]);
                    gfx_rapi->set_sampler_parameters(i, seg.tex_linear[i], seg.tex_cms[i], seg.tex_cmt[i], seg.tex_lod);
                    // Mark this texture most-recently-used so the LRU eviction below
                    // doesn't drop a texture that's actively on screen via cached
                    // replay (which otherwise never refreshes the cache LRU).
                    if (seg.tex_node[i]) {
                        gfx_texture_cache.lru.splice(gfx_texture_cache.lru.end(), gfx_texture_cache.lru,
                                                     seg.tex_node[i]->second.lru_location);
                    }
                }
            }
            if (seg.prg != curprg) {
                if (curprg != NULL) {
                    gfx_rapi->unload_shader(curprg);
                }
                gfx_rapi->load_shader(seg.prg);
                curprg = seg.prg;
            }
            // Cull mode: auto = the recorded per-segment mode; the others are
            // diagnostics (off / force a single face) for /dlcache cull.
            int cm = seg.cull;
            if (g_DlCacheCullMode == 1) {
                cm = 0;
            } else if (g_DlCacheCullMode == 2) {
                cm = 1;
            } else if (g_DlCacheCullMode == 3) {
                cm = 2;
            }
            // CHEAT_MIRROR reflects the cached geometry (negated X in uMVP above),
            // reversing winding — flip the front-face sense so GPU back-face
            // culling keeps the same faces it would un-mirrored.
            const bool front_ccw = gfx_mirror_mode ? !g_DlCacheFrontCcw : g_DlCacheFrontCcw;
            gfx_rapi->cache_set_cull(cm, front_ccw);
            // Distance fog (G_FOG) must be recomputed per-frame from gl_Position;
            // constant fog is baked in aFog.a (use_vertex_fog = 1).
            gfx_rapi->set_fog_params(seg.fog_compute ? 0 : 1, seg.fog_mul, seg.fog_off);
            // Shader-side palette: route the live shade colour into this combiner's
            // shade input slots (after load_shader so it targets this program).
            if (use_palette) {
                gfx_rapi->set_shade_routing(seg.shade_route);
            }
            applied_group = seg.state_group;
        }
        if (pend_tris == 0) {
            pend_prg = seg.prg;
            pend_base = seg.base_float;
            gap_tris = 0;
            gap_batches = 0;
        } else if (gap_tris > 0) {
            // a culled hole sits between the pending draw and this segment:
            // absorb it if small (offscreen tris just get clipped), else split
            if (gap_tris <= (size_t)g_DlCacheGapTris) {
                pend_tris += gap_tris;
                g_DlCacheFrameVisAbsorbed += gap_batches;
            } else {
                gfx_rapi->cache_draw(pend_prg, pend_base, pend_tris);
                g_DlCacheFrameTris += (uint32_t)pend_tris;
                g_DlCacheFrameDraws++;
                pend_tris = 0;
                pend_prg = seg.prg;
                pend_base = seg.base_float;
                g_DlCacheFrameVisCulled += gap_batches;
            }
            gap_tris = 0;
            gap_batches = 0;
        }
        if (vis != NULL && seg.batch_index >= 0) {
            g_DlCacheFrameVisDrawn++;
        }
        pend_tris += seg.num_tris;
        drawn_batches++;
    }
    g_DlCacheFrameVisCulled += gap_batches; // trailing hole at the end of the leaf
    if (pend_tris > 0) {
        gfx_rapi->cache_draw(pend_prg, pend_base, pend_tris);
        g_DlCacheFrameTris += (uint32_t)pend_tris;
        g_DlCacheFrameDraws++;
    }
    if (curprg != NULL) {
        gfx_rapi->unload_shader(curprg);
    }
    g_DlCacheFrameSegments += drawn_batches;

    if (use_palette) {
        gfx_rapi->set_palette_enable(0); // back to baked combiner inputs for the immediate path
    }
    gfx_rapi->set_mvp(g_DlCacheIdentity);
    gfx_rapi->set_fog_params(1, 0.f, 0.f); // back to baked per-vertex fog for the immediate path
    gfx_rapi->cache_replay_end();

    // Force the immediate path to re-establish shader (attrib pointers back into
    // opengl_vbo) + textures on its next draw. depth/viewport/scissor are forced
    // too; alpha/modulate mirror the last segment (GL is in that state).
    rendering_state.shader_program = NULL;
    rendering_state.textures[0] = NULL;
    rendering_state.textures[1] = NULL;
    rdp.textures_changed[0] = true;
    rdp.textures_changed[1] = true;
    rendering_state.depth_mode = 0xff;
    rendering_state.viewport = {};
    rendering_state.scissor = {};
    rdp.viewport_or_scissor_changed = true;
    if (!e->segments.empty()) {
        rendering_state.alpha_blend = e->segments.back().alpha_blend;
        rendering_state.modulate = e->segments.back().modulate;
    }
}

extern "C" void gfx_dlcache_clear(void) {
    dlcacheInvalidateAll();
}

extern "C" void gfx_dlcache_set_frontface(int ccw) {
    // Read live at replay time, so no re-record needed.
    g_DlCacheFrontCcw = (ccw != 0);
}

extern "C" int gfx_dlcache_get_frontface(void) {
    return g_DlCacheFrontCcw ? 1 : 0;
}

extern "C" void gfx_dlcache_set_cullmode(int mode) {
    g_DlCacheCullMode = mode; // read live at replay; no re-record
}

extern "C" int gfx_dlcache_get_cullmode(void) {
    return g_DlCacheCullMode;
}

extern "C" void gfx_dlcache_set_gap_tris(int tris) {
    g_DlCacheGapTris = tris < 0 ? 0 : tris; // read live at replay; no re-record
}

extern "C" void gfx_dlcache_set_palette(int on) {
    g_DlCachePaletteEnabled = (on != 0); // read live at replay; no re-record
}

// Texture-cache COUNT cap + current fill. The cap is the usual cause of dlcache
// "black textures": the recorder imports off-screen textures too, overflowing the
// cap -> LRU evicts on-screen textures (PORT_DLCACHE.md). Raising the cap live lets
// the working set fit; nothing is freed, so it takes effect on the next imports.
extern "C" void gfx_set_texture_cache_size(int n) {
    if (n < 256) n = 256;
    g_TextureCacheMaxSize = n;
}

extern "C" void gfx_get_texture_cache_fill(int* used, int* max) {
    if (used) *used = (int)gfx_texture_cache.map.size();
    if (max) *max = g_TextureCacheMaxSize;
}

extern "C" int gfx_dlcache_get_palette(void) {
    return g_DlCachePaletteEnabled ? 1 : 0;
}

extern "C" int gfx_dlcache_get_gap_tris(void) {
    return g_DlCacheGapTris;
}

extern "C" int gfx_dlcache_get_frame_draws(void) {
    return (int)g_DlCacheFrameDraws;
}

// Octree-visibility accounting for cached replays (last completed frame).
// drawn excludes absorbed; absorbed = octree-culled but drawn anyway by the
// gap-tolerant merge (they're frustum-clipped, so this costs ~nothing).
extern "C" void gfx_dlcache_get_vis_stats(uint32_t* drawn, uint32_t* culled, uint32_t* absorbed) {
    if (drawn) *drawn = g_DlCacheFrameVisDrawn;
    if (culled) *culled = g_DlCacheFrameVisCulled;
    if (absorbed) *absorbed = g_DlCacheFrameVisAbsorbed;
}

extern "C" void gfx_dlcache_get_stats(uint32_t* entries, uint32_t* bad, uint32_t* segments, uint32_t* tris,
                                      uint32_t* reasons) {
    uint32_t ne = 0, nb = 0;
    for (const auto& kv : g_DlCache) {
        if (kv.second.bad) {
            nb++;
        } else if (kv.second.ready) {
            ne++;
        }
    }
    if (entries) *entries = ne;
    if (bad) *bad = nb;
    if (segments) *segments = g_DlCacheFrameSegments;
    if (tris) *tris = g_DlCacheFrameTris;
    if (reasons) *reasons = g_DlCacheAbortReasons;
}

static void gfx_run_dl(Gfx* cmd) {
    // puts("dl");
    int dummy = 0;
    char dlName[128];
    const char* fileName;

    Gfx* dListStart = cmd;
    uint64_t ourHash = -1;

    for (;;) {
        uint32_t opcode = cmd->words.w0 >> 24;
        // gfx_print_cmd(cmd);
        switch (opcode) {
                // RSP commands:
            case G_NOOP:
                break;
            case G_MTX: {
                gfx_sp_matrix(C0(16, 8), (const int32_t*)seg_addr(cmd->words.w1));
                break;
            }
            case (uint8_t)G_POPMTX:
                gfx_sp_pop_matrix(1);
                break;
            case G_MOVEMEM:
                gfx_sp_movemem(C0(16, 8), 0, seg_addr(cmd->words.w1));
                break;
            case (uint8_t)G_MOVEWORD:
                gfx_sp_moveword(C0(0, 8), C0(8, 16), cmd->words.w1);
                break;
            case (uint8_t)G_TEXTURE:
                gfx_sp_texture(C1(16, 16), C1(0, 16), C0(11, 3), C0(8, 3), C0(0, 8));
                break;
            case G_VTX:
                gfx_sp_vertex(C0(0, 16) / sizeof(Vtx), C0(16, 4), (const Vtx*)seg_addr(cmd->words.w1));
                break;
            case G_DL:
                if (C0(16, 1) == 0) {
                    // Push return address
                    Gfx* subGFX = (Gfx*)seg_addr(cmd->words.w1);

                    if (subGFX != nullptr) {
                        gfx_run_dl(subGFX);
                    }
                } else {
                    cmd = (Gfx*)seg_addr(cmd->words.w1);
                    --cmd; // increase after break
                }
                break;
            case (uint8_t)G_ENDDL:
                return;
            case (uint8_t)G_SETGEOMETRYMODE:
                gfx_sp_geometry_mode(0, cmd->words.w1);
                break;
            case (uint8_t)G_CLEARGEOMETRYMODE:
                gfx_sp_geometry_mode(cmd->words.w1, 0);
                break;
            case G_EXTRAGEOMETRYMODE_EXT:
                gfx_sp_extra_geometry_mode(~C0(0, 24), cmd->words.w1);
                break;
            case G_DLCACHE_BEGIN_EXT: {
                // Cache key = the leaf gdl pointer in the gSPDisplayList that
                // immediately follows this command. w1 carries the per-frame
                // octree batch-visibility array (&g_BgCullVisible[startidx]) or 0.
                // w0 low bit = the room's vertex colours changed this frame
                // (dynamic lighting) -> drop + re-record so the bake isn't stale.
                const Gfx* dlcmd = cmd + 1;
                const void* key = (const void*)seg_addr(dlcmd->words.w1);
                const uint8_t* vis = (const uint8_t*)cmd->words.w1;
                const bool dirty = (cmd->words.w0 & 1u) != 0;
                auto it = g_DlCache.find(key);
                if (dirty && it != g_DlCache.end() && it->second.ready) {
                    if (it->second.palette_ok) {
                        // Dynamic lighting: refresh the live palette texture and keep
                        // the cached geometry (cache-speed; no VBO rebuild).
                        void* pal = seg_addr(it->second.palette_w1);
                        if (pal != NULL) {
                            gfx_rapi->cache_upload_palette(it->second.palette_tex, pal, it->second.palette_count);
                        }
                    } else {
                        // No palette path for this leaf: fall back to re-record.
                        if (it->second.buffer_id != 0) {
                            gfx_rapi->cache_delete_buffer(it->second.buffer_id);
                        }
                        g_DlCache.erase(it);
                        it = g_DlCache.end();
                    }
                }
                if (it != g_DlCache.end() && it->second.ready) {
                    dlcacheReplay(&it->second, vis);
                    ++cmd; // skip the following gSPDisplayList (already replayed)
                } else if (it != g_DlCache.end() && it->second.bad) {
                    // known-uncacheable: let the following gSPDisplayList run legacy
                } else {
                    dlcacheBeginRecord(key);
                }
                break;
            }
            case G_DLCACHE_END_EXT:
                dlcacheEndRecord();
                break;
            case (uint8_t)G_TRI1:
                gfx_sp_tri1(C1(16, 8) / 10, C1(8, 8) / 10, C1(0, 8) / 10, false);
                break;
            case (uint8_t)G_TRI4:
                gfx_sp_tri4(cmd);
                break;
            case (uint8_t)G_SETOTHERMODE_L:
                gfx_sp_set_other_mode(C0(8, 8), C0(0, 8), cmd->words.w1);
                break;
            case (uint8_t)G_SETOTHERMODE_H:
                gfx_sp_set_other_mode(C0(8, 8) + 32, C0(0, 8), (uint64_t)cmd->words.w1 << 32);
                break;
            case G_COL:
                // Capture the leaf's colour palette for the shader-side GPU palette.
                // We expect exactly one G_COL per cached leaf; a second disables the
                // palette path for it (falls back to re-record-on-dirty).
                if (g_DlCacheRecording) {
                    if (g_DlCacheGColSeen) {
                        g_DlCacheGColMulti = true;
                    } else {
                        g_DlCacheGColSeen = true;
                        g_DlCacheGColW1 = cmd->words.w1;
                        g_DlCacheGColCount = C0(0, 16) / 4;
                    }
                }
                gfx_sp_set_vertex_colors(C0(0, 16) / 4, (NormalColor *)seg_addr(cmd->words.w1));
                break;

            // RDP Commands:
            case G_SETTIMG: {
                gfx_dp_set_texture_image(C0(21, 3), C0(19, 2), C0(0, 10), 0, seg_addr(cmd->words.w1));
                break;
            }
            case G_SETTEXINFO_EXT: {
                gfx_dp_set_texture_info(C0(0, 8), C0(8, 8), C1(20, 12), C1(0, 20));
                break;
            }
            case G_SETTIMG_FB_EXT: {
                gfx_flush();
                gfx_rapi->select_texture_fb(cmd->words.w1);
                rdp.textures_changed[0] = false;
                rdp.textures_changed[1] = false;

                // clear the external tex key
                {
                    const uint32_t tile = gfx_lod_tile_offset(0);
                    LoadedTexture& tex = rdp.loaded_texture[rdp.texture_tile[tile].tmem];
                    tex.ext_key = 0;
                }
                break;
            }
            case G_SETGRAYSCALE_EXT:
                rdp.grayscale = cmd->words.w1;
                break;
            case G_SETDAZZLE_EXT:
                // flush so the boost applies exactly to the draws issued
                // while the weight is set (glares / overexposure flash)
                gfx_flush();
                gfx_hdr_dazzle = (float)(cmd->words.w1 & 0xff) / 255.0f;
                break;
            case G_RTRESOLVE_EXT:
                // Screen-space raytracing resolve (docs/PORT_RAYTRACING.md):
                // world + props are drawn, depth still intact. w1 = the
                // emitting player's rtcamera*. The backend saves/restores raw
                // GL state itself; force the cached viewport/scissor to
                // reapply on the next draw regardless.
                if (gfx_rt_enabled && gfx_rapi->rt_resolve && cmd->words.w1 != 0) {
                    gfx_flush();
                    gfx_rapi->rt_resolve((const void*)cmd->words.w1, rdp.viewport.x,
                                         rdp.viewport.y, rdp.viewport.width, rdp.viewport.height);
                    rdp.viewport_or_scissor_changed = true;
                }
                break;
            case G_LOADBLOCK:
                gfx_dp_load_block(C1(24, 3), C0(12, 12), C0(0, 12), C1(12, 12), C1(0, 12));
                break;
            case G_LOADTILE:
                gfx_dp_load_tile(C1(24, 3), C0(12, 12), C0(0, 12), C1(12, 12), C1(0, 12));
                break;
            case G_SETTILE:
                gfx_dp_set_tile(C0(21, 3), C0(19, 2), C0(9, 9), C0(0, 9), C1(24, 3), C1(20, 4), C1(18, 2), C1(14, 4),
                                C1(10, 4), C1(8, 2), C1(4, 4), C1(0, 4));
                break;
            case G_SETTILESIZE:
                gfx_dp_set_tile_size(C1(24, 3), C0(12, 12), C0(0, 12), C1(12, 12), C1(0, 12));
                break;
            case G_LOADTLUT:
                gfx_dp_load_tlut(C1(24, 3), C0(14, 10), C0(2, 10), C1(14, 10), C1(2, 10));
                break;
            case G_SETENVCOLOR:
                gfx_dp_set_env_color(C1(24, 8), C1(16, 8), C1(8, 8), C1(0, 8));
                break;
            case G_SETPRIMCOLOR:
                gfx_dp_set_prim_color(C0(8, 8), C0(0, 8), C1(24, 8), C1(16, 8), C1(8, 8), C1(0, 8));
                break;
            case G_SETFOGCOLOR:
                gfx_dp_set_fog_color(C1(24, 8), C1(16, 8), C1(8, 8), C1(0, 8));
                break;
            case G_SETFILLCOLOR:
                gfx_dp_set_fill_color(cmd->words.w1);
                break;
            case G_SETINTENSITY_EXT:
                gfx_dp_set_grayscale_color(C1(24, 8), C1(16, 8), C1(8, 8), C1(0, 8));
                break;
            case G_SETCOMBINE:
                gfx_dp_set_combine_mode(color_comb(C0(20, 4), C1(28, 4), C0(15, 5), C1(15, 3)),
                                        alpha_comb(C0(12, 3), C1(12, 3), C0(9, 3), C1(9, 3)),
                                        color_comb(C0(5, 4), C1(24, 4), C0(0, 5), C1(6, 3)),
                                        alpha_comb(C1(21, 3), C1(3, 3), C1(18, 3), C1(0, 3)));
                break;
            // G_SETPRIMCOLOR, G_CCMUX_PRIMITIVE, G_ACMUX_PRIMITIVE, is used by Goddard
            // G_CCMUX_TEXEL1, LOD_FRACTION is used in Bowser room 1
            case G_SETSUBPIXELOFFSET_EXT: {
                gfx_dp_set_subpixel_offset(C0(0, 16), C1(0, 16));
                break;
            }
            case G_TEXRECT:
            case G_TEXRECTFLIP: {
                int32_t lrx, lry, tile, ulx, uly;
                uint32_t uls, ult, dsdx, dtdy;
                lrx = C0(12, 12);
                lry = C0(0, 12);
                tile = C1(24, 3);
                ulx = C1(12, 12);
                uly = C1(0, 12);
                ++cmd;
                uls = C1(16, 16);
                ult = C1(0, 16);
                ++cmd;
                dsdx = C1(16, 16);
                dtdy = C1(0, 16);
                gfx_dp_texture_rectangle(ulx, uly, lrx, lry, tile, uls, ult, dsdx, dtdy, opcode == G_TEXRECTFLIP);
                break;
            }
            case G_FILLRECT:
                gfx_dp_fill_rectangle(C1(12, 12), C1(0, 12), C0(12, 12), C0(0, 12));
                break;
            case G_FILLRECT_WIDE_EXT: {
                int32_t lrx, lry, ulx, uly;
                lrx = (int32_t)(C0(0, 24) << 8) >> 8;
                lry = (int32_t)(C1(0, 24) << 8) >> 8;
                ++cmd;
                ulx = (int32_t)(C0(0, 24) << 8) >> 8;
                uly = (int32_t)(C1(0, 24) << 8) >> 8;
                gfx_dp_fill_rectangle(ulx, uly, lrx, lry);
                break;
            }
            case G_TEXRECT_WIDE_EXT: {
                int32_t lrx, lry, tile, ulx, uly;
                uint32_t uls, ult, dsdx, dtdy;
                bool flip;
                lrx = (int32_t)((C0(0, 24) << 8)) >> 8;
                lry = (int32_t)((C1(0, 24) << 8)) >> 8;
                tile = C1(24, 3);
                flip = C1(27, 1);
                ++cmd;
                ulx = (int32_t)((C0(0, 24) << 8)) >> 8;
                uly = (int32_t)((C1(0, 24) << 8)) >> 8;
                ++cmd;
                uls = C0(16, 16);
                ult = C0(0, 16);
                dsdx = C1(16, 16);
                dtdy = C1(0, 16);
                gfx_dp_texture_rectangle(ulx, uly, lrx, lry, tile, uls, ult, dsdx, dtdy, flip);
                break;
            }
            case G_IMAGERECT_EXT: {
                int16_t tile, iw, ih;
                int16_t x0, y0, s0, t0;
                int16_t x1, y1, s1, t1;
                tile = C0(0, 3);
                iw = C1(16, 16);
                ih = C1(0, 16);
                ++cmd;
                x0 = C0(16, 16);
                y0 = C0(0, 16);
                s0 = C1(16, 16);
                t0 = C1(0, 16);
                ++cmd;
                x1 = C0(16, 16);
                y1 = C0(0, 16);
                s1 = C1(16, 16);
                t1 = C1(0, 16);
                gfx_dp_image_rectangle(tile, iw, ih, x0, y0, s0, t0, x1, y1, s1, t1);
                break;
            }
            case G_SETSCISSOR:
                gfx_dp_set_scissor(C1(24, 2), C0(12, 12), C0(0, 12), C1(12, 12), C1(0, 12));
                break;
            case G_SETZIMG:
                gfx_dp_set_z_image(seg_addr(cmd->words.w1));
                break;
            case G_SETCIMG:
                gfx_dp_set_color_image(C0(21, 3), C0(19, 2), C0(0, 11), seg_addr(cmd->words.w1));
                break;
            case G_SETFB_EXT:
                gfx_flush();
                if (cmd->words.w1) {
                    // don't care about noise here
                    gfx_set_framebuffer(cmd->words.w1, 1.f);
                    fbActive = true;
                } else {
                    gfx_reset_framebuffer();
                    fbActive = false;
                }
                break;
            case G_COPYFB_EXT:
                gfx_copy_framebuffer(C0(11, 11), C0(0, 11), (int16_t)C1(16, 16), (int16_t)C1(0, 16), C0(22, 1));
                break;
            case G_RDPSETOTHERMODE:
                gfx_dp_set_other_mode(C0(0, 24), cmd->words.w1);
                break;
            case G_INVALTEXCACHE_EXT:
                if (cmd->words.w1) {
                    gfx_texture_cache_delete((const uint8_t *)seg_addr(cmd->words.w1));
                } else {
                    gfx_texture_cache_clear();
                }
                break;
            case (uint8_t)G_RDPHALF_1:
            case (uint8_t)G_RDPHALF_2:
            case (uint8_t)G_RDPHALF_CONT:
                // on N64 skyRender uses these to render some types of skies and skybox water
                // by issuing low-level ucode commands G_TRI_FILL and G_TRI_SHADE_TXTR
                // the port renders the sky in a different manner
                break;
            case G_RDPFLUSH_EXT:
                gfx_flush();
                break;
            case G_CLEAR_DEPTH_EXT:
                gfx_flush();
                gfx_rapi->clear_framebuffer(false, true);
                break;
            case G_RDPPIPESYNC:
            case G_RDPFULLSYNC:
            case G_RDPLOADSYNC:
            case G_RDPTILESYNC:
                break;
            default:
                sysFatalError("Unknown GBI opcode 0x%02x at %p.\nw0 %08x\nw1 %08x", opcode, cmd, cmd->words.w0, cmd->words.w1);
                break;
        }
        ++cmd;
    }
}

static void gfx_sp_reset() {
    rsp.modelview_matrix_stack_size = 1;
    rsp.current_num_lights = 2;
    rsp.lights_changed = true;
}

extern "C" void gfx_get_dimensions(uint32_t* width, uint32_t* height, int32_t* posX, int32_t* posY) {
    gfx_wapi->get_dimensions(width, height, posX, posY);
}

extern "C" void gfx_init(const GfxInitSettings *settings) {
    gfx_wapi = settings->wapi;
    gfx_rapi = settings->rapi;
    gfx_wapi->init(&settings->window_settings);
    gfx_rapi->init();
    gfx_rapi->update_framebuffer_parameters(0, settings->window_settings.width, settings->window_settings.height, 1, false, true, true, true);
    gfx_current_dimensions.internal_mul = 1;
    gfx_current_game_window_viewport.width = gfx_current_dimensions.width = settings->window_settings.width;
    gfx_current_game_window_viewport.height = gfx_current_dimensions.height = settings->window_settings.height;
    game_framebuffer = gfx_rapi->create_framebuffer();
    game_framebuffer_msaa_resolved = gfx_rapi->create_framebuffer();

    if (gfx_msaa_level > 1 && !gfx_framebuffers_enabled) {
        sysLogPrintf(LOG_WARNING, "F3D: MSAA set to %d, but framebuffers are not available; disabling", gfx_msaa_level);
        gfx_msaa_level = 1;
    }

    for (int i = 0; i < 16; i++) {
        segmentPointers[i] = 0;
    }

    if (tex_upload_buffer == nullptr) {
        // We cap texture max to 8k, because why would you need more?
        int max_tex_size = std::min(8192, gfx_rapi->get_max_texture_size());
        tex_upload_buffer = (uint8_t*)malloc(max_tex_size * max_tex_size * 4);
    }

    rsp.lookat[0].dir[0] = rsp.lookat[1].dir[1] = 0x7F;
    rsp.current_lookat_coeffs[0][0] = rsp.current_lookat_coeffs[1][1] = 1.f;
    rsp.lookat_enabled = true;
}

extern "C" void gfx_destroy(void) {
    // TODO: should also destroy rapi and wapi, and any other resources acquired in fast3d

    // Texture cache and loaded textures store references to Resources which need to be unreferenced.
    gfx_texture_cache_clear();
}

extern "C" struct GfxRenderingAPI* gfx_get_current_rendering_api(void) {
    return gfx_rapi;
}

extern "C" void gfx_start_frame(void) {
    // Chaos visual modes (docs/PORT_CHAOS.md), applied at the frame boundary:
    // a flat-texture toggle clears the texture cache so everything re-imports
    // through gfx_upload_tex_filtered (the clear also invalidates the dlcache,
    // whose segments hold the old texture ids); the grayscale toggle drives
    // the ordinary rdp.grayscale path with a neutral colour and drops the
    // dlcache (cached leaves baked the old shader choice).
    static int flattex_applied = 0;
    static int grayscale_applied = 0;
    if (gfx_flattex_mode != flattex_applied) {
        flattex_applied = gfx_flattex_mode;
        gfx_texture_cache_clear();
    }
    // The grayscale shader path serves three chaos modes: gold-shiny
    // (gfx_shiny_mode == 2 — luminance * gold reads as metal once the shiny
    // UV warp is on), the generic screen tint (pd.screen_tint, 0x00RRGGBB),
    // and plain forced grayscale (film noir, neutral colour) — in that
    // priority order; turning one off re-applies the next.
    {
        int want_on = 0;
        uint32_t want_col = 0x00ffffff;

        if (gfx_shiny_mode == 2) {
            want_on = 1;
            want_col = 0x00ffc440; // gold: bright yellow-orange metal tint
        } else if (gfx_screen_tint != 0) {
            want_on = 1;
            want_col = (uint32_t)gfx_screen_tint & 0x00ffffff;
        } else if (gfx_force_grayscale) {
            want_on = 1;
        }

        // grayscale_applied packs (on << 24 | colour) so a colour change while
        // already on still re-applies.
        const int want_key = want_on ? (int)(0x01000000u | want_col) : 0;
        if (want_key != grayscale_applied) {
            grayscale_applied = want_key;
            rdp.grayscale = want_on != 0;
            rdp.grayscale_color.r = (want_col >> 16) & 0xff;
            rdp.grayscale_color.g = (want_col >> 8) & 0xff;
            rdp.grayscale_color.b = want_col & 0xff;
            rdp.grayscale_color.a = 255; // full lerp to luminance
            dlcacheInvalidateAll();
        }
    }

    gfx_wapi->handle_events();
    gfx_wapi->get_dimensions(&gfx_current_window_dimensions.width, &gfx_current_window_dimensions.height,
                             &gfx_current_window_position_x, &gfx_current_window_position_y);

    if (gfx_current_window_dimensions.height == 0) {
        // Avoid division by zero
        gfx_current_window_dimensions.height = 1;
    }

    gfx_current_window_dimensions.aspect_ratio = (float)gfx_current_window_dimensions.width / gfx_current_window_dimensions.height;

    gfx_current_dimensions = gfx_current_window_dimensions;

    gfx_current_game_window_viewport.width = gfx_current_dimensions.width;
    gfx_current_game_window_viewport.height = gfx_current_dimensions.height;

    if (gfx_current_dimensions.height != gfx_prev_dimensions.height) {
        for (auto& fb : framebuffers) {
            uint32_t width, height, msaa;
            if (fb.second.autoresize) {
                if (fb.second.upscale) {
                    width = fb.second.orig_width;
                    height = fb.second.orig_height;
                    gfx_adjust_width_height_for_scale(width, height);
                } else {
                    // assume this is a fullscreen fb
                    width = gfx_current_dimensions.width;
                    height = gfx_current_dimensions.height;
                }
                if (width != fb.second.applied_width || height != fb.second.applied_height) {
                    gfx_rapi->update_framebuffer_parameters(fb.first, width, height, 1, true, true, true, true);
                    fb.second.applied_width = width;
                    fb.second.applied_height = height;
                }
            }
        }
    }
    gfx_prev_dimensions = gfx_current_dimensions;

    bool different_size = gfx_current_dimensions.width != gfx_current_game_window_viewport.width ||
                          gfx_current_dimensions.height != gfx_current_game_window_viewport.height;
    if (gfx_framebuffers_enabled && (different_size || gfx_msaa_level > 1)) {
        game_renders_to_framebuffer = true;
        if (different_size) {
            gfx_rapi->update_framebuffer_parameters(game_framebuffer, gfx_current_dimensions.width,
                                                    gfx_current_dimensions.height, gfx_msaa_level, true, true, true,
                                                    true);
        } else {
            // MSAA framebuffer needs to be resolved to an equally sized target when complete, which must therefore
            // match the window size
            gfx_rapi->update_framebuffer_parameters(game_framebuffer, gfx_current_window_dimensions.width,
                                                    gfx_current_window_dimensions.height, gfx_msaa_level, false, true,
                                                    true, true);
        }
        if (gfx_msaa_level > 1 && different_size) {
            gfx_rapi->update_framebuffer_parameters(game_framebuffer_msaa_resolved, gfx_current_dimensions.width,
                                                    gfx_current_dimensions.height, 1, false, false, false, false);
        }
    } else {
        game_renders_to_framebuffer = false;
    }

    fbActive = 0;

    // update aspect scale and offset
    gfx_update_aspect_mode();
}

uint32_t num_dls = 0;

extern "C" void gfx_run(Gfx* commands) {
    ++num_dls;
    gfx_sp_reset();

    gfx_hdr_dazzle = 0.0f; // defensive: never let a dazzle bracket leak across frames

    g_DlCacheFrameSegments = 0;
    g_DlCacheFrameTris = 0;
    g_DlCacheFrameDraws = 0;
    g_DlCacheFrameVisDrawn = 0;
    g_DlCacheFrameVisCulled = 0;
    g_DlCacheFrameVisAbsorbed = 0;

    // puts("New frame");

    if (!gfx_wapi->start_frame()) {
        dropped_frame = true;
        return;
    }
    dropped_frame = false;

    gfx_rapi->update_framebuffer_parameters(0, gfx_current_window_dimensions.width,
                                            gfx_current_window_dimensions.height, 1, false, true, true,
                                            !game_renders_to_framebuffer);
    gfx_rapi->start_frame();
    gfx_rapi->start_draw_to_framebuffer(game_renders_to_framebuffer ? game_framebuffer : 0,
                                        (float)gfx_current_dimensions.height / SCREEN_HEIGHT);
    gfx_rapi->clear_framebuffer(true, false);
    rdp.viewport_or_scissor_changed = true;
    rendering_state.viewport = {};
    rendering_state.scissor = {};
    gfx_run_dl(commands);
    gfx_flush();
    gfxFramebuffer = 0;

    if (game_renders_to_framebuffer) {
        gfx_rapi->start_draw_to_framebuffer(0, 1);
        gfx_rapi->clear_framebuffer(true, true);

        if (gfx_msaa_level > 1) {
            bool different_size = gfx_current_dimensions.width != gfx_current_game_window_viewport.width ||
                                  gfx_current_dimensions.height != gfx_current_game_window_viewport.height;

            if (different_size) {
                gfx_rapi->resolve_msaa_color_buffer(game_framebuffer_msaa_resolved, game_framebuffer);
                gfxFramebuffer = (uintptr_t)gfx_rapi->get_framebuffer_texture_id(game_framebuffer_msaa_resolved);
            } else {
                gfx_rapi->resolve_msaa_color_buffer(0, game_framebuffer);
            }
        } else {
            gfxFramebuffer = (uintptr_t)gfx_rapi->get_framebuffer_texture_id(game_framebuffer);
        }
    }

    gfx_rapi->end_frame();
    gfx_wapi->swap_buffers_begin();
}

extern "C" void gfx_end_frame(void) {
    if (!dropped_frame) {
        gfx_rapi->finish_render();
        gfx_wapi->swap_buffers_end();
    }
}

extern "C" void gfx_set_target_fps(int fps) {
    gfx_wapi->set_target_fps(fps);
}

extern "C" void reset_texture_state() {
    gfx_texture_cache_clear();
    if (rendering_state.shader_program) {
        gfx_rapi->unload_shader(rendering_state.shader_program);
        rendering_state.shader_program = nullptr;
    }
    gfx_rapi->clear_shaders();
    color_combiner_pool.clear();
    prev_combiner = color_combiner_pool.end();
}

extern "C" void gfx_set_texture_filter(enum FilteringMode mode) {
    reset_texture_state();
    gfx_rapi->set_texture_filter(mode);
}

extern "C" void gfx_set_mipmap_filter(enum MipmapFilteringMode mode) {
    reset_texture_state();
    gfx_rapi->set_mipmap_filter(mode);
}

extern "C" int gfx_create_framebuffer(uint32_t width, uint32_t height, int upscale, int autoresize) {
    int fb = gfx_rapi->create_framebuffer();
    gfx_resize_framebuffer(fb, width, height, upscale, autoresize);
    return fb;
}

extern "C" void gfx_resize_framebuffer(int fb, uint32_t width, uint32_t height, int upscale, int autoresize) {
    uint32_t orig_width, orig_height;

    if (width && height) {
        // user-specified size
        orig_width = width;
        orig_height = height;
        if (upscale) {
            gfx_adjust_width_height_for_scale(width, height);
        }
        gfx_rapi->update_framebuffer_parameters(fb, width, height, 1, true, true, true, true);
    } else {
        // same size as main fb
        orig_width = width = gfx_current_dimensions.width;
        orig_height = height = gfx_current_dimensions.height;
        upscale = false;
        autoresize = true;
        gfx_rapi->update_framebuffer_parameters(fb, width, height, 1, true, true, true, true);
    }

    framebuffers[fb] = { orig_width, orig_height, width, height, (bool)upscale, (bool)autoresize };
}

extern "C" void gfx_set_framebuffer(int fb, float noise_scale) {
    gfx_rapi->start_draw_to_framebuffer(fb, noise_scale);
    gfx_rapi->clear_framebuffer(true, true);
    active_fb = framebuffers.find(fb);
}

extern "C" void gfx_copy_framebuffer(int fb_dst, int fb_src, int left, int top, int use_back) {
    const bool is_main_fb = (fb_src == 0);

    if (is_main_fb) {
        if (left > 0 && top > 0) {
            // upscale the position
            left = left * gfx_current_dimensions.width / gfx_current_native_viewport.width;
            top = top * gfx_current_dimensions.height / gfx_current_native_viewport.height;
            // flip Y
            top = gfx_current_dimensions.height - top - 1;
        }
        if (use_back && gfx_msaa_level > 1) {
            // read from the framebuffer we've been rendering to
            fb_src = game_framebuffer;
        }
    }

    gfx_rapi->copy_framebuffer(fb_dst, fb_src, left, top, is_main_fb, (bool)use_back);
}

extern "C" void gfx_reset_framebuffer(void) {
    gfx_rapi->start_draw_to_framebuffer(0, (float)gfx_current_dimensions.height / SCREEN_HEIGHT);
    active_fb = framebuffers.end();
}
