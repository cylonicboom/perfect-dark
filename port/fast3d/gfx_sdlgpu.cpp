// SDL_GPU rendering backend for fast3d (port-only; see docs/PORT_SDLGPU.md).
//
// Phase 1: full immediate-path game rendering on Vulkan.
//  - Shaders: runtime GLSL450 -> glslang -> SPIR-V (gfx_sdlgpu_shader.cpp).
//  - Pipelines: SDL_GPU bakes GL's dynamic state (blend/depth/cull/fill) into
//    immutable pipeline objects; a cache keyed on shader + state resolves them
//    lazily at draw time. Viewport/scissor stay dynamic.
//  - Framebuffers: rapi fb 0 is an internal offscreen texture, never the
//    swapchain (swapchain textures are color-target-only); end_frame blits
//    fb0 -> swapchain. This uniformly handles copy_framebuffer's use_back,
//    screenshots, and the GL<->Vulkan y-flip question.
//  - Frame: two command buffers. Texture/vertex uploads record copy passes on
//    an upload CB as they occur; draws/blits record on a render CB. Submitting
//    upload-then-render at end_frame guarantees uploads execute first, which
//    sidesteps "no copy passes inside render passes" entirely. Re-uploads to
//    an existing texture id allocate a fresh GPU texture (draws recorded
//    earlier keep referencing the old one), so GL's mid-frame upload ordering
//    is preserved.
//  - Coordinates: gfx_pc hands viewport/scissor in GL bottom-left window
//    coords; converted to SDL_GPU's top-left here. Clip parameters are
//    { z 0..1, invert_y = false }: SDL_GPU's NDC is +y up with top-left
//    texture origin, which makes both window and sampled-offscreen rendering
//    come out upright without any inversion. --gpu-invert-y flips this at
//    runtime in case a driver disagrees (saves a rebuild while verifying).
//
// Display-list cache (cache_*) entries still return 0 -> per-leaf legacy
// fallback; implemented in Phase 2. The OpenGL backend is untouched and
// remains the default; select with Video.Renderer=sdlgpu / --renderer sdlgpu.

#ifdef USE_SDLGPU

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <map>
#include <unordered_map>
#include <vector>

#include <SDL3/SDL.h>

#ifndef _LANGUAGE_C
#define _LANGUAGE_C
#endif
#include <PR/gbi.h>

#include "platform.h"
#include "system.h"

#include "gfx_cc.h"
#include "gfx_rendering_api.h"
#include "gfx_window_manager_api.h"
#include "gfx_api.h"
#include "gfx_sdl.h"
#include "gfx_sdlgpu.h"
#include "gfx_sdlgpu_shader.h"
#include "rt_ext.h"        // raytracing suite controls + rtcamera (docs/PORT_RAYTRACING.md)
#include "gfx_rt_common.h" // shared RT pass bodies, quality table, matrix helpers

using namespace std;

// 16 MB of streamed immediate-path vertex data per frame (worst observed PD
// frames are a few MB). Overflow logs once and drops the remaining draws.
#define GFX_SDLGPU_VTX_BUFFER_CAP (16u * 1024u * 1024u)

// Vertex streaming is triple-buffered across frames. Cycling the GPU vertex
// buffer is NOT an option here: vertex-buffer binds are recorded throughout
// the frame, but the upload copy records at end_frame — cycling there would
// point the already-recorded binds at the old backing while the copy fills
// the new one (draws then render last frame's data; manifested as corrupt
// geometry with every few frames correct). Distinct per-frame buffers with
// cycle=false keep binds and copy on the same allocation, and the 3-deep
// rotation means the copy never targets a buffer a frame in flight still
// reads (SDL's automatic sync would make that correct anyway, just slow).
#define GFX_SDLGPU_VTX_RING 3

// depth comparison selected by set_depth_mode
enum { DF_LEQUAL, DF_LESS, DF_ALWAYS };

enum { TILE_NONE, TILE_TEX, TILE_FB };

struct TexEntry {
    SDL_GPUTexture *tex;
    uint32_t w, h;
    bool in_use;       // id allocated (new_texture) and not deleted
    uint32_t skey;     // packed sampler key (set_sampler_parameters)
};

struct GpuFb {
    SDL_GPUTexture *color;   // sample count = msaa (COLOR_TARGET only when msaa > 1)
    SDL_GPUTexture *depth;
    SDL_GPUTexture *resolve; // msaa > 1 only: single-sample COLOR_TARGET|SAMPLER resolve target
    uint32_t w, h;
    uint32_t msaa;           // applied sample count (1/2/4/8, clamped to device support)
    bool has_depth;
    bool color_virgin; // never rendered to since (re)creation: first pass clears
    bool depth_virgin;
};

struct PipelineKey {
    struct ShaderProgram *prg;
    uint32_t flags; // blend(2) | test(1)<<2 | write(1)<<3 | func(2)<<4 | bias(1)<<6 | fill_line(1)<<7 | has_depth(1)<<8
    uint32_t color_fmt;

    bool operator==(const PipelineKey &o) const {
        return prg == o.prg && flags == o.flags && color_fmt == o.color_fmt;
    }
};

struct PipelineKeyHash {
    size_t operator()(const PipelineKey &k) const {
        size_t h = std::hash<void *>()(k.prg);
        h ^= (size_t)k.flags * 0x9E3779B9u;
        h ^= (size_t)k.color_fmt * 0x85EBCA6Bu;
        return h;
    }
};

static struct {
    SDL_GPUDevice *device;
    SDL_Window *window;
    bool claimed;
    bool invert_y; // --gpu-invert-y escape hatch
    uint32_t frame_count;
    int swap_interval;
    FilteringMode filter_mode;
    MipmapFilteringMode mipmap_mode;
    int anisotropy;
    SDL_GPUTextureFormat depth_format;
    bool depth_samplable; // depth format supports DEPTH_STENCIL_TARGET|SAMPLER (RT suite)

    // per-frame command buffers (null outside start_frame..end_frame)
    SDL_GPUCommandBuffer *upload_cb;
    SDL_GPUCommandBuffer *render_cb;

    // immediate-path vertex streaming (triple-buffered, see GFX_SDLGPU_VTX_RING)
    SDL_GPUBuffer *vtx_buf[GFX_SDLGPU_VTX_RING];
    SDL_GPUTransferBuffer *vtx_tbuf[GFX_SDLGPU_VTX_RING];
    uint32_t vtx_cur;
    uint8_t *vtx_map;
    uint32_t vtx_used;
    bool vtx_overflow_logged;

    SDL_GPUTexture *dummy_tex; // 1x1 white, bound where a real texture is missing

    // MSAA diagnostics for /gpu: per-frame draw counts by target sample count
    // (last completed frame) + one-shot logs proving the msaa pipeline /
    // resolve paths actually engage
    uint32_t dbg_draws_msaa, dbg_draws_1x;
    uint32_t dbg_last_msaa, dbg_last_1x;
    bool dbg_msaa_pipeline_logged;
    bool dbg_resolve_logged;

    // GL front-buffer emulation: snapshot of fb0 taken at the end of every
    // frame. copy_framebuffer(use_back=false) means glReadBuffer(GL_FRONT) in
    // the GL backend — the previously PRESENTED frame — which the pause-menu
    // blur and menu backgrounds rely on (at copy time the current fb0 is
    // freshly cleared, so reading it directly yields black).
    SDL_GPUTexture *front_tex;
    uint32_t front_w, front_h;

    // Video.GpuDriver default for when --gpu-driver isn't passed
    char driver_default[16];

    // HDR output (scRGB extended linear): FP16 swapchain + FP16 internal
    // render targets + a paper-white scaling present pass. SDR-authored
    // content maps shader 1.0 -> paperwhite nits (scRGB 1.0 = 80 nits).
    bool hdr_requested;
    bool hdr_active;
    float hdr_paperwhite;
    float hdr_peak; // highlight-expansion target; <= paperwhite disables it
    SDL_GPUSwapchainComposition composition; // SDR (0) unless HDR active
    SDL_GPUTextureFormat fb_format;          // RGBA8, or FP16 when HDR active
    SDL_GPUShader *present_vs, *present_fs;
    SDL_GPUGraphicsPipeline *present_pipeline;
} gpu = {};

// Display-list cache storage (port-only; see docs/PORT_DLCACHE.md): persistent
// per-leaf vertex buffers, uploaded once, replayed each frame; and per-leaf
// shade-palette textures (count x 1 RGBA8) sampled in the VERTEX stage so
// dynamic lighting updates at cache speed.
struct CacheBuf {
    SDL_GPUBuffer *buf;
    bool in_use;
};

struct PalEntry {
    SDL_GPUTexture *tex;
    int count;
    bool in_use;
};

static std::map<pair<uint64_t, uint32_t>, struct ShaderProgram> shader_pool;
static std::unordered_map<PipelineKey, SDL_GPUGraphicsPipeline *, PipelineKeyHash> pipeline_cache;
static std::unordered_map<uint32_t, SDL_GPUSampler *> sampler_cache;
static std::vector<TexEntry> textures;
static std::vector<GpuFb> fbs;
static std::vector<CacheBuf> cache_bufs;
static std::vector<PalEntry> palettes;

// deferred releases, flushed after the frame's submits
static std::vector<SDL_GPUTexture *> dead_textures;
static std::vector<SDL_GPUTransferBuffer *> dead_transfers;
static std::vector<SDL_GPUBuffer *> dead_buffers;

static struct {
    int cur_fb;
    SDL_GPURenderPass *pass;
    SDL_GPUGraphicsPipeline *bound_pipeline;

    struct ShaderProgram *prg;
    uint8_t blend; // 0 off, 1 src-alpha, 2 modulate (dst_color * src)
    bool depth_test, depth_write, depth_bias;
    uint8_t depth_func;

    // viewport/scissor in GL bottom-left coords as handed over by gfx_pc
    int vp_x, vp_y, vp_w, vp_h;
    int sc_x, sc_y, sc_w, sc_h;
    bool sc_valid;
    float depth_min, depth_max;

    struct {
        uint8_t kind;
        uint32_t idx;
    } tile[2];
    int last_tile; // GL active-texture-unit analogue for upload_texture

    // display-list cache replay state
    uint32_t cache_buf;          // bound cached buffer id (cache_replay_begin), 0 = none
    uint8_t cull_mode;           // cache_set_cull: 0 none, 1 back, 2 front (cached draws only)
    bool front_ccw;
    SDL_GPUTexture *palette_tex; // bound shade palette (cache_bind_palette), NULL = none

    struct GfxSdlGpuVSUni vs_uni;
    struct GfxSdlGpuFSUni fs_uni;
    bool vs_dirty, fs_dirty;
} st;

// ---------------------------------------------------------------------------
// probe / vsync (Phase 0)

int gfx_sdlgpu_probe(void) {
    if (gpu.device) {
        return 1;
    }

    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        sysLogPrintf(LOG_WARNING, "SDL_GPU: could not init SDL video: %s", SDL_GetError());
        return 0;
    }

    // Offer every shader format we can produce; SDL picks a driver that
    // accepts one of them. SPIR-V is the universal intermediate (glslang);
    // DXBC/MSL are SPIRV-Cross translations (Phase 4).
    SDL_GPUShaderFormat formats = SDL_GPU_SHADERFORMAT_SPIRV;
#ifdef GFX_SDLGPU_HAS_SPIRV_CROSS
#if defined(_WIN32)
    formats |= SDL_GPU_SHADERFORMAT_DXBC;
#elif defined(__APPLE__)
    formats |= SDL_GPU_SHADERFORMAT_MSL;
#endif
#endif

    // Driver selection: --gpu-driver <vulkan|direct3d12|metal> overrides; on
    // Windows the default stays Vulkan (the battle-tested path) even though
    // DXBC is offered — D3D12 is opt-in. Elsewhere SDL picks its platform
    // default (Metal on macOS). The SDL_GPU_DRIVER env var also works (it
    // outranks a normal-priority hint).
    const char *drv = sysArgGetString("--gpu-driver");
    if (!drv || !*drv) {
        drv = gpu.driver_default[0] ? gpu.driver_default : NULL; // Video.GpuDriver
    }
    // accept common shorthands; SDL's driver names are exact strings
    if (drv && (strcmp(drv, "d3d12") == 0 || strcmp(drv, "dx12") == 0)) {
        drv = "direct3d12";
    } else if (drv && strcmp(drv, "vk") == 0) {
        drv = "vulkan";
    }
#ifndef GFX_SDLGPU_HAS_SPIRV_CROSS
    if (drv && *drv && strcmp(drv, "vulkan") != 0) {
        // without SPIRV-Cross we can only feed SPIR-V, i.e. Vulkan; honouring
        // the request would just fail device creation
        sysLogPrintf(LOG_WARNING, "SDL_GPU: built without SPIRV-Cross, only vulkan is available; ignoring --gpu-driver %s", drv);
        drv = NULL;
    }
#endif
#if defined(_WIN32)
    if (!drv || !*drv) {
        drv = "vulkan";
    }
#endif
    if (drv && *drv) {
        SDL_SetHint(SDL_HINT_GPU_DRIVER, drv);
    }

    const bool debug = sysArgCheck("--debug-gpu");
    gpu.device = SDL_CreateGPUDevice(formats, debug, NULL);
    if (!gpu.device && debug) {
        // e.g. the D3D12 debug layer needs the Windows "Graphics Tools"
        // optional feature; don't let a missing validation layer kill the run
        sysLogPrintf(LOG_WARNING, "SDL_GPU: debug device creation failed (%s), retrying without --debug-gpu", SDL_GetError());
        gpu.device = SDL_CreateGPUDevice(formats, false, NULL);
    }
    if (!gpu.device && drv && *drv && strcmp(drv, "vulkan") != 0) {
        // requested driver unavailable/failed: fall back to vulkan before
        // giving up on SDL_GPU entirely
        sysLogPrintf(LOG_WARNING, "SDL_GPU: could not create %s device (%s), retrying with vulkan", drv, SDL_GetError());
        SDL_SetHint(SDL_HINT_GPU_DRIVER, "vulkan");
        gpu.device = SDL_CreateGPUDevice(formats, debug, NULL);
        if (!gpu.device && debug) {
            gpu.device = SDL_CreateGPUDevice(formats, false, NULL);
        }
    }
    if (!gpu.device) {
        sysLogPrintf(LOG_WARNING, "SDL_GPU: could not create device: %s", SDL_GetError());
        return 0;
    }

    sysLogPrintf(LOG_NOTE, "SDL_GPU: created device, driver: %s%s",
        SDL_GetGPUDeviceDriver(gpu.device), debug ? " (debug)" : "");
    return 1;
}

void gfx_sdlgpu_set_vsync(int interval) {
    gpu.swap_interval = interval;

    if (!gpu.device || !gpu.window || !gpu.claimed) {
        return; // applied after the window is claimed in init
    }

    // intervals > 1 have no SDL_GPU equivalent; they present as VSYNC and the
    // existing frame limiter handles the actual pacing
    SDL_GPUPresentMode mode = SDL_GPU_PRESENTMODE_VSYNC;
    if (interval == 0 && SDL_WindowSupportsGPUPresentMode(gpu.device, gpu.window, SDL_GPU_PRESENTMODE_IMMEDIATE)) {
        mode = SDL_GPU_PRESENTMODE_IMMEDIATE;
    } else if (interval < 0 && SDL_WindowSupportsGPUPresentMode(gpu.device, gpu.window, SDL_GPU_PRESENTMODE_MAILBOX)) {
        mode = SDL_GPU_PRESENTMODE_MAILBOX;
    }

    if (!SDL_SetGPUSwapchainParameters(gpu.device, gpu.window, gpu.composition, mode)) {
        sysLogPrintf(LOG_WARNING, "SDL_GPU: failed to set present mode %d: %s", (int)mode, SDL_GetError());
    }
}

void gfx_sdlgpu_set_driver_default(const char *drv) {
    gpu.driver_default[0] = '\0';
    if (drv && *drv) {
        strncpy(gpu.driver_default, drv, sizeof(gpu.driver_default) - 1);
        gpu.driver_default[sizeof(gpu.driver_default) - 1] = '\0';
    }
}

void gfx_sdlgpu_request_hdr(int enable, float paperwhite_nits, float peak_nits) {
    gpu.hdr_requested = enable != 0;
    gpu.hdr_paperwhite = paperwhite_nits > 0.0f ? paperwhite_nits : 200.0f;
    gpu.hdr_peak = peak_nits > 0.0f ? peak_nits : 600.0f;
}

void gfx_sdlgpu_set_hdr_paperwhite(float nits) {
    if (nits > 0.0f) {
        gpu.hdr_paperwhite = nits;
    }
}

void gfx_sdlgpu_set_hdr_peak(float nits) {
    if (nits > 0.0f) {
        gpu.hdr_peak = nits;
    }
}

// ---------------------------------------------------------------------------
// upload helpers

// Copy passes record on the upload CB, which executes before the render CB.
// Outside a frame (boot-time uploads), an ad-hoc CB is acquired and submitted
// immediately by the caller via upload_end.
static SDL_GPUCommandBuffer *upload_cb_get(bool *adhoc) {
    if (gpu.upload_cb) {
        *adhoc = false;
        return gpu.upload_cb;
    }
    *adhoc = true;
    return SDL_AcquireGPUCommandBuffer(gpu.device);
}

static void upload_cb_end(SDL_GPUCommandBuffer *cb, bool adhoc) {
    if (adhoc && cb) {
        SDL_SubmitGPUCommandBuffer(cb);
    }
}

// ---------------------------------------------------------------------------
// samplers

// packed sampler key bits: 0 min_linear, 1 mag_linear, 2 mips, 3 mip_linear,
// 4-5 wrap_s, 6-7 wrap_t, 8 aniso
#define SK_MIN_LINEAR 0x001
#define SK_MAG_LINEAR 0x002
#define SK_MIPS       0x004
#define SK_MIP_LINEAR 0x008
#define SK_WRAP_S_SHIFT 4
#define SK_WRAP_T_SHIFT 6
#define SK_ANISO      0x100

enum { WRAP_CLAMP, WRAP_REPEAT, WRAP_MIRROR };

static uint32_t gfx_cm_to_wrap(uint32_t val) {
    switch (val) {
        case G_TX_NOMIRROR | G_TX_CLAMP:
            return WRAP_CLAMP;
        case G_TX_MIRROR | G_TX_WRAP:
            return WRAP_MIRROR;
        case G_TX_MIRROR | G_TX_CLAMP:
            return WRAP_MIRROR; // no mirror-clamp in SDL_GPU; same fallback GL uses pre-4.4
        case G_TX_NOMIRROR | G_TX_WRAP:
            return WRAP_REPEAT;
    }
    return WRAP_CLAMP;
}

static SDL_GPUSamplerAddressMode wrap_to_sdl(uint32_t w) {
    switch (w) {
        case WRAP_REPEAT:
            return SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
        case WRAP_MIRROR:
            return SDL_GPU_SAMPLERADDRESSMODE_MIRRORED_REPEAT;
        default:
            return SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    }
}

static SDL_GPUSampler *sampler_get(uint32_t key) {
    auto it = sampler_cache.find(key);
    if (it != sampler_cache.end()) {
        return it->second;
    }

    SDL_GPUSamplerCreateInfo ci;
    SDL_zero(ci);
    ci.min_filter = (key & SK_MIN_LINEAR) ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
    ci.mag_filter = (key & SK_MAG_LINEAR) ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
    ci.mipmap_mode = (key & SK_MIP_LINEAR) ? SDL_GPU_SAMPLERMIPMAPMODE_LINEAR : SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    ci.address_mode_u = wrap_to_sdl((key >> SK_WRAP_S_SHIFT) & 3);
    ci.address_mode_v = wrap_to_sdl((key >> SK_WRAP_T_SHIFT) & 3);
    ci.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    ci.min_lod = 0.0f;
    ci.max_lod = (key & SK_MIPS) ? 1000.0f : 0.0f;
    if (key & SK_ANISO) {
        ci.enable_anisotropy = true;
        ci.max_anisotropy = (float)gpu.anisotropy;
    }

    SDL_GPUSampler *s = SDL_CreateGPUSampler(gpu.device, &ci);
    if (!s) {
        sysLogPrintf(LOG_WARNING, "SDL_GPU: could not create sampler %x: %s", key, SDL_GetError());
    }
    sampler_cache[key] = s;
    return s;
}

// fb colour textures and the dummy are sampled linear/clamp without mips
#define SK_FB_LINEAR (SK_MIN_LINEAR | SK_MAG_LINEAR | (WRAP_CLAMP << SK_WRAP_S_SHIFT) | (WRAP_CLAMP << SK_WRAP_T_SHIFT))

// ---------------------------------------------------------------------------
// MSAA helpers

static SDL_GPUSampleCount msaa_to_enum(uint32_t n) {
    switch (n) {
        case 8: return SDL_GPU_SAMPLECOUNT_8;
        case 4: return SDL_GPU_SAMPLECOUNT_4;
        case 2: return SDL_GPU_SAMPLECOUNT_2;
        default: return SDL_GPU_SAMPLECOUNT_1;
    }
}

// clamp the requested vidMSAA level (1-16) to a sample count the device
// supports for both the colour (RGBA8, or FP16 in HDR) and depth formats
static uint32_t msaa_clamp(uint32_t requested) {
    uint32_t n = requested >= 8 ? 8 : requested >= 4 ? 4 : requested >= 2 ? 2 : 1;
    while (n > 1) {
        if (SDL_GPUTextureSupportsSampleCount(gpu.device, gpu.fb_format, msaa_to_enum(n)) &&
            SDL_GPUTextureSupportsSampleCount(gpu.device, gpu.depth_format, msaa_to_enum(n))) {
            break;
        }
        n >>= 1;
    }
    return n;
}

// Make a framebuffer's colour readable (sample/blit source). Non-msaa fbs
// return their colour texture directly; msaa fbs get a resolve pass (load the
// msaa contents, resolve into fb.resolve, keep the msaa data) recorded on cb
// first — SDL_GPU cannot sample or blit from multisample textures.
static SDL_GPUTexture *fb_readable_color(GpuFb &fb, SDL_GPUCommandBuffer *cb) {
    if (fb.msaa <= 1 || !fb.resolve) {
        return fb.color;
    }
    if (!fb.color) {
        return NULL;
    }
    SDL_GPUColorTargetInfo ct;
    SDL_zero(ct);
    ct.texture = fb.color;
    ct.load_op = SDL_GPU_LOADOP_LOAD;
    ct.store_op = SDL_GPU_STOREOP_RESOLVE_AND_STORE;
    ct.resolve_texture = fb.resolve;
    SDL_GPURenderPass *p = SDL_BeginGPURenderPass(cb, &ct, 1, NULL);
    SDL_EndGPURenderPass(p);
    return fb.resolve;
}

// ---------------------------------------------------------------------------
// render pass management

static void end_pass(void) {
    if (st.pass) {
        SDL_EndGPURenderPass(st.pass);
        st.pass = NULL;
        st.bound_pipeline = NULL;
    }
}

static void apply_viewport(void) {
    if (!st.pass || st.vp_w <= 0 || st.vp_h <= 0) {
        return;
    }
    const GpuFb &fb = fbs[st.cur_fb];
    SDL_GPUViewport v;
    v.x = (float)st.vp_x;
    v.y = (float)((int)fb.h - st.vp_y - st.vp_h); // GL bottom-left -> top-left
    v.w = (float)st.vp_w;
    v.h = (float)st.vp_h;
    v.min_depth = st.depth_min;
    v.max_depth = st.depth_max;
    SDL_SetGPUViewport(st.pass, &v);
}

static void apply_scissor(void) {
    if (!st.pass) {
        return;
    }
    const GpuFb &fb = fbs[st.cur_fb];
    SDL_Rect r;
    if (st.sc_valid) {
        // NOTE: a degenerate scissor (w or h <= 0) must stay EMPTY — it means
        // "clip everything", like glScissor with zero extents. The menus rely
        // on this: list options scrolled out of the dialog get a zero-height
        // scissor (menuitem.c clamps Y2 = Y1) and must not draw at all.
        // Treating it as "no scissor" made scrolled-out items fully visible.
        r.x = st.sc_x;
        r.y = (int)fb.h - st.sc_y - st.sc_h; // GL bottom-left -> top-left
        r.w = st.sc_w > 0 ? st.sc_w : 0;
        r.h = st.sc_h > 0 ? st.sc_h : 0;
        // clamp into the target (Vulkan requires non-negative offsets)
        if (r.x < 0) { r.w += r.x; r.x = 0; }
        if (r.y < 0) { r.h += r.y; r.y = 0; }
        if (r.x > (int)fb.w) { r.x = (int)fb.w; }
        if (r.y > (int)fb.h) { r.y = (int)fb.h; }
        if (r.x + r.w > (int)fb.w) { r.w = (int)fb.w - r.x; }
        if (r.y + r.h > (int)fb.h) { r.h = (int)fb.h - r.y; }
        if (r.w < 0) { r.w = 0; }
        if (r.h < 0) { r.h = 0; }
    } else {
        r.x = r.y = 0;
        r.w = (int)fb.w;
        r.h = (int)fb.h;
    }
    SDL_SetGPUScissor(st.pass, &r);
}

static void ensure_pass(void) {
    if (st.pass || !gpu.render_cb) {
        return;
    }
    GpuFb &fb = fbs[st.cur_fb];
    if (!fb.color) {
        return;
    }

    SDL_GPUColorTargetInfo ct;
    SDL_zero(ct);
    ct.texture = fb.color;
    ct.load_op = fb.color_virgin ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
    ct.store_op = SDL_GPU_STOREOP_STORE;
    ct.clear_color.a = 1.0f;

    SDL_GPUDepthStencilTargetInfo ds;
    SDL_zero(ds);
    const bool has_depth = fb.depth != NULL;
    if (has_depth) {
        ds.texture = fb.depth;
        ds.load_op = fb.depth_virgin ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
        ds.store_op = SDL_GPU_STOREOP_STORE;
        ds.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
        ds.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
        ds.clear_depth = 1.0f;
    }

    st.pass = SDL_BeginGPURenderPass(gpu.render_cb, &ct, 1, has_depth ? &ds : NULL);
    fb.color_virgin = false;
    fb.depth_virgin = false;
    st.bound_pipeline = NULL;
    apply_viewport();
    apply_scissor();
}

// ---------------------------------------------------------------------------
// pipelines

static SDL_GPUGraphicsPipeline *pipeline_resolve(bool cached) {
    const GpuFb &fb = fbs[st.cur_fb];
    const bool has_depth = fb.depth != NULL;
    // Cached draws stay solid under the wireframe cheat (parity with GL,
    // where glPolygonMode wireframe only wraps draw_triangles).
    const bool fill_line = !cached && gfx_wireframe_mode && st.depth_test;
    // Backface culling exists only on the cached path (the immediate path is
    // CPU-culled by gfx_pc); set by cache_set_cull per replay segment.
    const uint32_t cull = cached ? st.cull_mode : 0;
    const bool front_ccw = cached ? st.front_ccw : true;

    const uint32_t sample_bits = fb.msaa >= 8 ? 3 : fb.msaa >= 4 ? 2 : fb.msaa >= 2 ? 1 : 0;

    PipelineKey k;
    k.prg = st.prg;
    k.flags = (uint32_t)st.blend |
              ((uint32_t)st.depth_test << 2) |
              ((uint32_t)st.depth_write << 3) |
              ((uint32_t)st.depth_func << 4) |
              ((uint32_t)st.depth_bias << 6) |
              ((uint32_t)fill_line << 7) |
              ((uint32_t)has_depth << 8) |
              ((uint32_t)cached << 9) |
              (cull << 10) |
              ((uint32_t)front_ccw << 12) |
              (sample_bits << 13);
    k.color_fmt = (uint32_t)gpu.fb_format;

    auto it = pipeline_cache.find(k);
    if (it != pipeline_cache.end()) {
        return it->second;
    }

    struct ShaderProgram *prg = st.prg;

    // cached vertex layout has a trailing aShadeIdx float (palette index)
    SDL_GPUVertexBufferDescription vbd;
    SDL_zero(vbd);
    vbd.slot = 0;
    vbd.pitch = ((Uint32)prg->num_floats + (cached ? 1 : 0)) * sizeof(float);
    vbd.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

    SDL_GPUVertexAttribute attrs[17];
    uint32_t off = 0;
    int num_attrs = prg->num_attribs;
    for (int i = 0; i < prg->num_attribs; i++) {
        SDL_zero(attrs[i]);
        attrs[i].location = (Uint32)i;
        attrs[i].buffer_slot = 0;
        switch (prg->attrib_sizes[i]) {
            case 1: attrs[i].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT; break;
            case 2: attrs[i].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2; break;
            case 3: attrs[i].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3; break;
            default: attrs[i].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4; break;
        }
        attrs[i].offset = off;
        off += (uint32_t)prg->attrib_sizes[i] * sizeof(float);
    }
    if (cached && prg->vs_cached != prg->vs) {
        // aShadeIdx, consumed by the palette VS variant
        SDL_zero(attrs[num_attrs]);
        attrs[num_attrs].location = (Uint32)num_attrs;
        attrs[num_attrs].buffer_slot = 0;
        attrs[num_attrs].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT;
        attrs[num_attrs].offset = off;
        num_attrs++;
    }

    SDL_GPUGraphicsPipelineCreateInfo ci;
    SDL_zero(ci);
    ci.vertex_shader = cached ? prg->vs_cached : prg->vs;
    ci.fragment_shader = prg->fs;
    ci.vertex_input_state.vertex_buffer_descriptions = &vbd;
    ci.vertex_input_state.num_vertex_buffers = 1;
    ci.vertex_input_state.vertex_attributes = attrs;
    ci.vertex_input_state.num_vertex_attributes = (Uint32)num_attrs;
    ci.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;

    ci.rasterizer_state.fill_mode = fill_line ? SDL_GPU_FILLMODE_LINE : SDL_GPU_FILLMODE_FILL;
    ci.rasterizer_state.cull_mode = cull == 1 ? SDL_GPU_CULLMODE_BACK
                                  : cull == 2 ? SDL_GPU_CULLMODE_FRONT
                                              : SDL_GPU_CULLMODE_NONE;
    ci.rasterizer_state.front_face = front_ccw ? SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE : SDL_GPU_FRONTFACE_CLOCKWISE;
    if (st.depth_bias) {
        // ZMODE_DEC decals; replaces glPolygonOffset(-2, -2)
        ci.rasterizer_state.enable_depth_bias = true;
        ci.rasterizer_state.depth_bias_constant_factor = -2.0f;
        ci.rasterizer_state.depth_bias_slope_factor = -2.0f;
    }
    // depth clamp (GL_DEPTH_CLAMP equivalent) so near gun geometry isn't clipped
    ci.rasterizer_state.enable_depth_clip = false;

    ci.multisample_state.sample_count = msaa_to_enum(fb.msaa);

    const bool depth_on = st.depth_test && has_depth;
    ci.depth_stencil_state.enable_depth_test = depth_on;
    ci.depth_stencil_state.enable_depth_write = depth_on && st.depth_write;
    switch (depth_on ? st.depth_func : DF_ALWAYS) {
        case DF_LEQUAL: ci.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL; break;
        case DF_LESS:   ci.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS; break;
        default:        ci.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_ALWAYS; break;
    }

    SDL_GPUColorTargetDescription ctd;
    SDL_zero(ctd);
    ctd.format = (SDL_GPUTextureFormat)k.color_fmt;
    if (st.blend != 0) {
        ctd.blend_state.enable_blend = true;
        ctd.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
        ctd.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
        if (st.blend == 2) {
            // GL modulate: glBlendFunc(GL_DST_COLOR, GL_ZERO)
            ctd.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_DST_COLOR;
            ctd.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
            ctd.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_DST_ALPHA;
            ctd.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
        } else {
            ctd.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
            ctd.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
            ctd.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
            ctd.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        }
    }
    ci.target_info.color_target_descriptions = &ctd;
    ci.target_info.num_color_targets = 1;
    ci.target_info.has_depth_stencil_target = has_depth;
    if (has_depth) {
        ci.target_info.depth_stencil_format = gpu.depth_format;
    }

    SDL_GPUGraphicsPipeline *p = SDL_CreateGPUGraphicsPipeline(gpu.device, &ci);
    if (!p) {
        sysLogPrintf(LOG_ERROR, "SDL_GPU: pipeline creation failed (ID %llx, %x, flags %x): %s",
                     (unsigned long long)prg->shader_id0, prg->shader_id1, k.flags, SDL_GetError());
    } else if (fb.msaa > 1 && !gpu.dbg_msaa_pipeline_logged) {
        gpu.dbg_msaa_pipeline_logged = true;
        sysLogPrintf(LOG_NOTE, "SDL_GPU: first %ux-msaa pipeline created", fb.msaa);
    }
    pipeline_cache[k] = p;
    return p;
}

// ---------------------------------------------------------------------------
// rapi: shaders

static const char *gfx_sdlgpu_get_name(void) {
    return "SDL_GPU";
}

static int gfx_sdlgpu_get_max_texture_size(void) {
    // SDL_GPU has no query; 8192 is the guaranteed floor for 2D textures on
    // every backend it supports
    return 8192;
}

static struct GfxClipParameters gfx_sdlgpu_get_clip_parameters(void) {
    return { true, gpu.invert_y };
}

static void gfx_sdlgpu_unload_shader(struct ShaderProgram *old_prg) {
}

static void gfx_sdlgpu_load_shader(struct ShaderProgram *new_prg) {
    st.prg = new_prg;
}

static struct ShaderProgram *gfx_sdlgpu_create_and_load_new_shader(uint64_t shader_id0, uint32_t shader_id1) {
    struct ShaderProgram *prg = &shader_pool[make_pair(shader_id0, shader_id1)];
    if (!gfx_sdlgpu_shader_compile(gpu.device, shader_id0, shader_id1, gpu.filter_mode, prg)) {
        sysFatalError("SDL_GPU: shader compilation failed (ID %llx, %x), see log",
                      (unsigned long long)shader_id0, shader_id1);
    }
    st.prg = prg;
    return prg;
}

static struct ShaderProgram *gfx_sdlgpu_lookup_shader(uint64_t shader_id0, uint32_t shader_id1) {
    auto it = shader_pool.find(make_pair(shader_id0, shader_id1));
    return it == shader_pool.end() ? nullptr : &it->second;
}

static void gfx_sdlgpu_shader_get_info(struct ShaderProgram *prg, uint8_t *num_inputs, bool used_textures[2]) {
    *num_inputs = prg->num_inputs;
    used_textures[0] = prg->used_textures[0];
    used_textures[1] = prg->used_textures[1];
}

static void gfx_sdlgpu_clear_shaders(void) {
    for (auto &pc : pipeline_cache) {
        if (pc.second) {
            SDL_ReleaseGPUGraphicsPipeline(gpu.device, pc.second);
        }
    }
    pipeline_cache.clear();
    for (auto &sp : shader_pool) {
        if (sp.second.vs_cached && sp.second.vs_cached != sp.second.vs) {
            SDL_ReleaseGPUShader(gpu.device, sp.second.vs_cached);
        }
        if (sp.second.vs) {
            SDL_ReleaseGPUShader(gpu.device, sp.second.vs);
        }
        if (sp.second.fs) {
            SDL_ReleaseGPUShader(gpu.device, sp.second.fs);
        }
    }
    shader_pool.clear();
    st.prg = NULL;
    st.bound_pipeline = NULL;
}

// ---------------------------------------------------------------------------
// rapi: textures

static uint32_t gfx_sdlgpu_new_texture(void) {
    for (size_t i = 0; i < textures.size(); i++) {
        if (!textures[i].in_use) {
            textures[i] = {};
            textures[i].in_use = true;
            return (uint32_t)i + 1;
        }
    }
    textures.push_back({});
    textures.back().in_use = true;
    return (uint32_t)textures.size();
}

static void gfx_sdlgpu_delete_texture(uint32_t texID) {
    if (texID == 0 || texID > textures.size()) {
        return;
    }
    TexEntry &e = textures[texID - 1];
    if (e.tex) {
        dead_textures.push_back(e.tex);
    }
    e = {};
}

static void gfx_sdlgpu_select_texture(int tile, uint32_t texture_id, bool linear_filter) {
    if (tile < 0 || tile > 1) {
        return;
    }
    st.tile[tile].kind = TILE_TEX;
    st.tile[tile].idx = texture_id;
    st.last_tile = tile;

    // GL's current_textures_linear_filter analogue (drives three-point mix)
    int32_t *tp = tile == 0 ? &st.fs_uni.three_point_filter0 : &st.fs_uni.three_point_filter1;
    if (*tp != (int32_t)linear_filter) {
        *tp = (int32_t)linear_filter;
        st.fs_dirty = true;
    }
}

static void gfx_sdlgpu_upload_texture(const uint8_t *rgba32_buf, uint32_t width, uint32_t height, bool gen_mipmaps) {
    if (st.tile[st.last_tile].kind != TILE_TEX) {
        return;
    }
    const uint32_t id = st.tile[st.last_tile].idx;
    if (id == 0 || id > textures.size()) {
        return;
    }
    TexEntry &e = textures[id - 1];

    // a fresh GPU texture per upload keeps draws recorded earlier this frame
    // pointing at the old contents (GL mid-frame re-upload ordering)
    if (e.tex) {
        dead_textures.push_back(e.tex);
        e.tex = NULL;
    }

    const bool mips = gen_mipmaps || gpu.filter_mode == FILTER_THREE_POINT;
    uint32_t levels = 1;
    if (mips) {
        uint32_t m = width > height ? width : height;
        while (m >>= 1) {
            levels++;
        }
    }

    SDL_GPUTextureCreateInfo tci;
    SDL_zero(tci);
    tci.type = SDL_GPU_TEXTURETYPE_2D;
    tci.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    tci.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    if (mips) {
        tci.usage |= SDL_GPU_TEXTUREUSAGE_COLOR_TARGET; // mipmap gen blits internally
    }
    tci.width = width;
    tci.height = height;
    tci.layer_count_or_depth = 1;
    tci.num_levels = levels;
    tci.sample_count = SDL_GPU_SAMPLECOUNT_1;

    e.tex = SDL_CreateGPUTexture(gpu.device, &tci);
    if (!e.tex) {
        sysLogPrintf(LOG_WARNING, "SDL_GPU: could not create %ux%u texture: %s", width, height, SDL_GetError());
        return;
    }
    e.w = width;
    e.h = height;

    SDL_GPUTransferBufferCreateInfo tbci;
    SDL_zero(tbci);
    tbci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tbci.size = width * height * 4;
    SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(gpu.device, &tbci);
    if (!tb) {
        sysLogPrintf(LOG_WARNING, "SDL_GPU: could not create texture transfer buffer: %s", SDL_GetError());
        return;
    }
    void *map = SDL_MapGPUTransferBuffer(gpu.device, tb, false);
    if (map) {
        memcpy(map, rgba32_buf, width * height * 4);
        SDL_UnmapGPUTransferBuffer(gpu.device, tb);
    }

    bool adhoc = false;
    SDL_GPUCommandBuffer *cb = upload_cb_get(&adhoc);
    if (cb) {
        SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cb);
        SDL_GPUTextureTransferInfo src;
        SDL_zero(src);
        src.transfer_buffer = tb;
        SDL_GPUTextureRegion dst;
        SDL_zero(dst);
        dst.texture = e.tex;
        dst.w = width;
        dst.h = height;
        dst.d = 1;
        SDL_UploadToGPUTexture(cp, &src, &dst, false);
        SDL_EndGPUCopyPass(cp);
        if (mips && levels > 1) {
            SDL_GenerateMipmapsForGPUTexture(cb, e.tex);
        }
        upload_cb_end(cb, adhoc);
    }
    dead_transfers.push_back(tb);
}

static void gfx_sdlgpu_set_sampler_parameters(int tile, bool linear_filter, uint32_t cms, uint32_t cmt, bool mipmaps) {
    if (tile < 0 || tile > 1) {
        return;
    }
    st.last_tile = tile;
    if (st.tile[tile].kind != TILE_TEX) {
        return;
    }
    const uint32_t id = st.tile[tile].idx;
    if (id == 0 || id > textures.size()) {
        return;
    }

    // mirror the GL backend's min/mag filter table
    mipmaps = mipmaps && (gpu.mipmap_mode != MIPMAP_DISABLED);
    const bool three_point = gpu.filter_mode == FILTER_THREE_POINT;
    const bool min_linear = linear_filter && gpu.filter_mode == FILTER_LINEAR;
    const bool mag_linear = linear_filter && gpu.filter_mode == FILTER_LINEAR;
    const bool use_mips = mipmaps && linear_filter && !three_point;
    const bool mip_linear = use_mips && gpu.mipmap_mode == MIPMAP_LINEAR;

    uint32_t key = 0;
    if (min_linear) key |= SK_MIN_LINEAR;
    if (mag_linear) key |= SK_MAG_LINEAR;
    if (use_mips) key |= SK_MIPS;
    if (mip_linear) key |= SK_MIP_LINEAR;
    key |= gfx_cm_to_wrap(cms) << SK_WRAP_S_SHIFT;
    key |= gfx_cm_to_wrap(cmt) << SK_WRAP_T_SHIFT;
    if (use_mips && gpu.anisotropy > 1) key |= SK_ANISO;

    textures[id - 1].skey = key;
}

// ---------------------------------------------------------------------------
// rapi: render state

static void gfx_sdlgpu_set_depth_mode(bool depth_test, bool depth_update, bool depth_compare, bool depth_source_prim,
                                      uint16_t zmode) {
    st.depth_test = depth_test;
    if (depth_test) {
        st.depth_write = depth_update;
        st.depth_bias = false;
        if (depth_compare) {
            switch (zmode) {
                case ZMODE_INTER:
                    st.depth_func = DF_LEQUAL;
                    break;
                case ZMODE_OPA:
                case ZMODE_XLU:
                    st.depth_func = depth_source_prim ? DF_LEQUAL : DF_LESS;
                    break;
                case ZMODE_DEC:
                    st.depth_func = DF_LEQUAL;
                    st.depth_bias = true;
                    break;
            }
        } else {
            st.depth_func = DF_ALWAYS;
        }
    }
}

static void gfx_sdlgpu_set_depth_range(float znear, float zfar) {
    st.depth_min = znear;
    st.depth_max = zfar;
    apply_viewport();
}

static void gfx_sdlgpu_set_viewport(int x, int y, int width, int height) {
    st.vp_x = x;
    st.vp_y = y;
    st.vp_w = width;
    st.vp_h = height;
    apply_viewport();
}

static void gfx_sdlgpu_set_scissor(int x, int y, int width, int height) {
    st.sc_x = x;
    st.sc_y = y;
    st.sc_w = width;
    st.sc_h = height;
    st.sc_valid = true;
    apply_scissor();
}

static void gfx_sdlgpu_set_use_alpha(bool use_alpha, bool modulate) {
    st.blend = use_alpha ? (modulate ? 2 : 1) : 0;
}

// ---------------------------------------------------------------------------
// rapi: draw

static void bind_tile_samplers(void) {
    if (st.prg->fs_sampler_count == 0) {
        return;
    }
    SDL_GPUTextureSamplerBinding binds[2];
    for (int i = 0; i < 2; i++) {
        if (!st.prg->used_textures[i]) {
            continue;
        }
        SDL_GPUTexture *tex = NULL;
        uint32_t skey = SK_FB_LINEAR;
        if (st.tile[i].kind == TILE_TEX && st.tile[i].idx > 0 && st.tile[i].idx <= textures.size()) {
            tex = textures[st.tile[i].idx - 1].tex;
            skey = textures[st.tile[i].idx - 1].skey;
        } else if (st.tile[i].kind == TILE_FB && st.tile[i].idx < fbs.size()) {
            // msaa colour can't be sampled; the resolve target holds the last
            // resolved contents (PD only samples non-msaa fbs in practice)
            const GpuFb &sfb = fbs[st.tile[i].idx];
            tex = sfb.msaa > 1 ? sfb.resolve : sfb.color;
        }
        if (!tex) {
            tex = gpu.dummy_tex;
            skey = SK_FB_LINEAR;
        }
        binds[st.prg->tex_binding[i]].texture = tex;
        binds[st.prg->tex_binding[i]].sampler = sampler_get(skey);
    }
    SDL_BindGPUFragmentSamplers(st.pass, 0, binds, st.prg->fs_sampler_count);
}

// HDR dazzle: map the flush-aligned G_SETDAZZLE_EXT weight (gfx_hdr_dazzle)
// to an emissive multiplier on marked draws (glares, the overexposure
// flash). DISABLED (strength 0) after playtesting: even small boosts on
// large screen-space glare billboards read as overblown; marked content now
// brightens exactly like any equally-bright pixel under the present pass's
// mild highlight lift. The bracket plumbing (G_SETDAZZLE_EXT -> this
// multiplier -> uEmissive) is kept inert — raise the strength to re-enable.
#define GFX_SDLGPU_DAZZLE_STRENGTH 0.0f
#define GFX_SDLGPU_DAZZLE_MAX 1.125f

static void update_emissive(void) {
    float emissive = 1.0f;
    if (GFX_SDLGPU_DAZZLE_STRENGTH > 0.0f && gpu.hdr_active && gfx_hdr_dazzle > 0.0f &&
        gpu.hdr_peak > gpu.hdr_paperwhite) {
        const float w = gfx_hdr_dazzle > 1.0f ? 1.0f : gfx_hdr_dazzle;
        emissive = 1.0f + (gpu.hdr_peak / gpu.hdr_paperwhite - 1.0f) * GFX_SDLGPU_DAZZLE_STRENGTH * w;
        if (emissive > GFX_SDLGPU_DAZZLE_MAX) {
            emissive = GFX_SDLGPU_DAZZLE_MAX;
        }
    }
    if (st.fs_uni.emissive != emissive) {
        st.fs_uni.emissive = emissive;
        st.fs_dirty = true;
    }
}

static void push_uniforms(void) {
    if (st.vs_dirty) {
        SDL_PushGPUVertexUniformData(gpu.render_cb, 0, &st.vs_uni, sizeof(st.vs_uni));
        st.vs_dirty = false;
    }
    if (st.fs_dirty) {
        SDL_PushGPUFragmentUniformData(gpu.render_cb, 0, &st.fs_uni, sizeof(st.fs_uni));
        st.fs_dirty = false;
    }
}

static void gfx_sdlgpu_draw_triangles(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris) {
    if (!st.prg || !gpu.render_cb || !gpu.vtx_map) {
        return;
    }

    const uint32_t bytes = (uint32_t)(buf_vbo_len * sizeof(float));
    if (gpu.vtx_used + bytes > GFX_SDLGPU_VTX_BUFFER_CAP) {
        if (!gpu.vtx_overflow_logged) {
            sysLogPrintf(LOG_ERROR, "SDL_GPU: per-frame vertex buffer overflow (%u bytes), dropping draws",
                         GFX_SDLGPU_VTX_BUFFER_CAP);
            gpu.vtx_overflow_logged = true;
        }
        return;
    }

    ensure_pass();
    if (!st.pass) {
        return;
    }

    SDL_GPUGraphicsPipeline *pipe = pipeline_resolve(false);
    if (!pipe) {
        return;
    }
    if (pipe != st.bound_pipeline) {
        SDL_BindGPUGraphicsPipeline(st.pass, pipe);
        st.bound_pipeline = pipe;
    }

    memcpy(gpu.vtx_map + gpu.vtx_used, buf_vbo, bytes);
    SDL_GPUBufferBinding bb;
    bb.buffer = gpu.vtx_buf[gpu.vtx_cur];
    bb.offset = gpu.vtx_used;
    SDL_BindGPUVertexBuffers(st.pass, 0, &bb, 1);
    gpu.vtx_used += bytes;

    bind_tile_samplers();

    update_emissive();

    // Wireframe cheat flat wire colour (mirrors the GL backend's per-draw
    // uniform juggle; only the fill mode itself lives in the pipeline).
    const bool wire_colour = gfx_wireframe_mode && st.depth_test && gfx_wireframe_wire_color_enabled;
    if (wire_colour) {
        st.fs_uni.wireframe_color[0] = gfx_wireframe_wire_color[0];
        st.fs_uni.wireframe_color[1] = gfx_wireframe_wire_color[1];
        st.fs_uni.wireframe_color[2] = gfx_wireframe_wire_color[2];
        st.fs_uni.wireframe_color[3] = 1.0f;
        st.fs_dirty = true;
    }

    push_uniforms();

    if (fbs[st.cur_fb].msaa > 1) {
        gpu.dbg_draws_msaa++;
    } else {
        gpu.dbg_draws_1x++;
    }

    SDL_DrawGPUPrimitives(st.pass, (Uint32)(3 * buf_vbo_num_tris), 1, 0, 0);

    if (wire_colour) {
        st.fs_uni.wireframe_color[0] = 0.0f;
        st.fs_uni.wireframe_color[1] = 0.0f;
        st.fs_uni.wireframe_color[2] = 0.0f;
        st.fs_uni.wireframe_color[3] = 0.0f;
        st.fs_dirty = true;
    }
}

// ---------------------------------------------------------------------------
// rapi: init / frame lifecycle

// Pick the best HDR composition available on the current device/window:
// 0 = none, 1 = scRGB extended-linear (preferred), 2 = HDR10 ST2084 (PQ).
// The composition checks are the real gate: they need OS HDR on for THIS
// display in its CURRENT mode (exclusive-fullscreen mode switches can drop
// HDR) AND driver support (some stacks only expose PQ output).
static int hdr_pick_composition(void) {
    const bool sdr_lin = SDL_WindowSupportsGPUSwapchainComposition(gpu.device, gpu.window,
                                                                   SDL_GPU_SWAPCHAINCOMPOSITION_SDR_LINEAR);
    const bool scrgb = SDL_WindowSupportsGPUSwapchainComposition(gpu.device, gpu.window,
                                                                 SDL_GPU_SWAPCHAINCOMPOSITION_HDR_EXTENDED_LINEAR);
    const bool hdr10 = SDL_WindowSupportsGPUSwapchainComposition(gpu.device, gpu.window,
                                                                 SDL_GPU_SWAPCHAINCOMPOSITION_HDR10_ST2084);
    const bool fp16 = SDL_GPUTextureSupportsFormat(gpu.device, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT,
                                                   SDL_GPU_TEXTURETYPE_2D,
                                                   SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
    // the support matrix tells exactly what the driver/display rejected
    // (scrgb=0 hdr10=0 usually means OS HDR is off for this display/mode)
    sysLogPrintf(LOG_NOTE, "SDL_GPU: %s composition support: sdr_linear=%d scrgb=%d hdr10=%d fp16_target=%d",
                 SDL_GetGPUDeviceDriver(gpu.device), sdr_lin, scrgb, hdr10, fp16);
    if (!fp16) {
        return 0;
    }
    return scrgb ? 1 : hdr10 ? 2 : 0;
}

void gfx_sdlgpu_get_info(char *buf, unsigned int len) {
    if (!gpu.device) {
        snprintf(buf, len, "no device");
        return;
    }
    const int cache_count = gfx_sdlgpu_shader_cache_count();
    char cachebuf[32];
    if (cache_count >= 0) {
        snprintf(cachebuf, sizeof(cachebuf), "%d entries", cache_count);
    } else {
        snprintf(cachebuf, sizeof(cachebuf), "off");
    }
#ifdef GFX_SDLGPU_HAS_SPIRV_CROSS
    const char *xlate = "spirv-cross yes";
#else
    const char *xlate = "spirv-cross NO (vulkan only)";
#endif
    char hdrbuf[48];
    if (gpu.hdr_active) {
        snprintf(hdrbuf, sizeof(hdrbuf), "hdr %s (paper white %g nits)",
                 gpu.composition == SDL_GPU_SWAPCHAINCOMPOSITION_HDR10_ST2084 ? "HDR10" : "scRGB",
                 gpu.hdr_paperwhite);
    } else {
        snprintf(hdrbuf, sizeof(hdrbuf), "hdr %s", gpu.hdr_requested ? "unavailable" : "off");
    }
    snprintf(buf, len, "driver %s, shaders %s, msaa %ux (req %ux), vsync %d, %s, shader cache %s, %s, draws msaa/1x %u/%u",
             SDL_GetGPUDeviceDriver(gpu.device), gfx_sdlgpu_shader_format_name(),
             msaa_clamp(gfx_msaa_level), gfx_msaa_level, gpu.swap_interval, hdrbuf, cachebuf, xlate,
             gpu.dbg_last_msaa, gpu.dbg_last_1x);
}

static void gfx_sdlgpu_init(void) {
    if (!gpu.device && !gfx_sdlgpu_probe()) {
        // video.c probes before committing to this backend, so normally we
        // never get here without a device
        sysFatalError("SDL_GPU: device creation failed:\n%s", SDL_GetError());
    }

    gpu.window = (SDL_Window *)gfx_sdl.get_window_handle();
    if (!gpu.window || !SDL_ClaimWindowForGPUDevice(gpu.device, gpu.window)) {
        sysFatalError("SDL_GPU: could not claim window:\n%s", SDL_GetError());
    }
    gpu.claimed = true;

    // HDR output: scRGB extended-linear swapchain (FP16) + FP16 internal
    // render targets, when the display/driver supports it. SDR-authored
    // content is brightness-mapped by the present pass below.
    gpu.composition = SDL_GPU_SWAPCHAINCOMPOSITION_SDR;
    gpu.fb_format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    if (gpu.hdr_requested) {
        int hdr_mode = hdr_pick_composition();

#if defined(_WIN32) && defined(GFX_SDLGPU_HAS_SPIRV_CROSS)
        // HDR over Vulkan is vendor-dependent on Windows (NVIDIA exposes
        // it, AMD/Intel often only via DXGI); D3D12 supports it whenever OS
        // HDR is enabled. HDR was explicitly requested, so recreate the
        // device on direct3d12 — nothing else has been created yet.
        if (hdr_mode == 0 && strcmp(SDL_GetGPUDeviceDriver(gpu.device), "direct3d12") != 0) {
            char prevdrv[16];
            strncpy(prevdrv, SDL_GetGPUDeviceDriver(gpu.device), sizeof(prevdrv) - 1);
            prevdrv[sizeof(prevdrv) - 1] = '\0';
            sysLogPrintf(LOG_NOTE, "SDL_GPU: %s has no HDR output on this system, retrying with direct3d12", prevdrv);

            const bool dbg = sysArgCheck("--debug-gpu") != 0;
            SDL_ReleaseWindowFromGPUDevice(gpu.device, gpu.window);
            SDL_DestroyGPUDevice(gpu.device);
            SDL_SetHint(SDL_HINT_GPU_DRIVER, "direct3d12");
            gpu.device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXBC, dbg, NULL);
            if (gpu.device && SDL_ClaimWindowForGPUDevice(gpu.device, gpu.window)) {
                hdr_mode = hdr_pick_composition();
                sysLogPrintf(LOG_NOTE, "SDL_GPU: recreated device, driver: %s", SDL_GetGPUDeviceDriver(gpu.device));
            } else {
                // d3d12 unavailable: go back to the original driver, no HDR
                if (gpu.device) {
                    SDL_DestroyGPUDevice(gpu.device);
                }
                SDL_SetHint(SDL_HINT_GPU_DRIVER, prevdrv);
                gpu.device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXBC, dbg, NULL);
                if (!gpu.device || !SDL_ClaimWindowForGPUDevice(gpu.device, gpu.window)) {
                    sysFatalError("SDL_GPU: could not recreate %s device:\n%s", prevdrv, SDL_GetError());
                }
                hdr_mode = 0;
            }
        }
#endif

        if (hdr_mode != 0) {
            gpu.composition = hdr_mode == 1 ? SDL_GPU_SWAPCHAINCOMPOSITION_HDR_EXTENDED_LINEAR
                                            : SDL_GPU_SWAPCHAINCOMPOSITION_HDR10_ST2084;
            gpu.fb_format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
            gpu.hdr_active = true;
        } else {
            sysLogPrintf(LOG_WARNING,
                         "SDL_GPU: HDR requested but unsupported by the display/driver (is OS HDR enabled?), staying SDR");
        }
    }

    // apply any vsync mode forwarded before the window existed (also applies
    // the swapchain composition chosen above)
    gfx_sdlgpu_set_vsync(gpu.swap_interval);

    gfx_sdlgpu_shader_init(gpu.device);

    // HDR present pass: fullscreen triangle scaling SDR-authored content to
    // the configured paper white (scRGB 1.0 = 80 nits). Replaces the SDR
    // path's plain fb0 -> swapchain blit.
    if (gpu.hdr_active) {
        static const char *present_vs_src =
            "#version 450\n"
            "layout(location = 0) out vec2 vUV;\n"
            "void main() {\n"
            "    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);\n"
            "    vUV = vec2(p.x, 1.0 - p.y);\n"
            "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
            "}\n";
        // scRGB extended-linear: sRGB EOTF -> linear BT.709, 1.0 = 80 nits,
        // scaled to the paper white (uScale = paperwhite / 80). The EOTF
        // matters: scRGB expects LINEAR values, and treating the game's
        // sRGB-encoded output as linear washes out mid-tones.
        // Highlight expansion (both variants): scene diffuse stays at paper
        // white, near-white content gains toward uBoost = peak/paperwhite
        // (Y^4 ramp; darks/mids untouched), and dazzled draws (uEmissive > 1
        // in the FP16 buffer) land EXACTLY at the peak: the gain is capped at
        // uBoost / Y so peak is a hard luminance ceiling, not a launchpad —
        // emissive, EOTF and curve can't compound past it. The sRGB EOTF is
        // extended linearly above 1.0 (pow() on extended values would blow
        // up super-linearly). uBoost 1 = expansion disabled.
        static const char *present_fs_scrgb_src =
            "#version 450\n"
            "layout(location = 0) in vec2 vUV;\n"
            "layout(set = 2, binding = 0) uniform sampler2D uTex0;\n"
            "layout(std140, set = 3, binding = 0) uniform PresentUni {\n"
            "    float uScale;\n"
            "    float uBoost;\n"
            "    float uPad1;\n"
            "    float uPad2;\n"
            "};\n"
            "layout(location = 0) out vec4 outColor;\n"
            "void main() {\n"
            "    vec3 c = texture(uTex0, vUV).rgb;\n"
            "    vec3 cl = min(c, vec3(1.0));\n"
            "    vec3 lo = cl / 12.92;\n"
            "    vec3 hi = pow((cl + vec3(0.055)) / 1.055, vec3(2.4));\n"
            "    vec3 lin = mix(lo, hi, step(vec3(0.04045), cl)) + max(c - vec3(1.0), vec3(0.0));\n"
            "    float yl = max(dot(lin, vec3(0.2126, 0.7152, 0.0722)), 0.0001);\n"
            "    float yc = min(yl, 1.0);\n"
            "    float gain = 1.0 + (uBoost - 1.0) * 0.175 * yc * yc * yc * yc;\n"
            "    gain = min(gain, uBoost / yl);\n"
            "    lin *= gain;\n"
            "    outColor = vec4(lin * uScale, 1.0);\n"
            "}\n";

        // HDR10 ST2084: sRGB EOTF -> linear BT.709 -> BT.2020 primaries ->
        // absolute luminance (uScale = paperwhite / 10000; PQ is absolute)
        // -> ST2084 PQ encode.
        static const char *present_fs_hdr10_src =
            "#version 450\n"
            "layout(location = 0) in vec2 vUV;\n"
            "layout(set = 2, binding = 0) uniform sampler2D uTex0;\n"
            "layout(std140, set = 3, binding = 0) uniform PresentUni {\n"
            "    float uScale;\n"
            "    float uBoost;\n"
            "    float uPad1;\n"
            "    float uPad2;\n"
            "};\n"
            "layout(location = 0) out vec4 outColor;\n"
            "void main() {\n"
            "    vec3 c = texture(uTex0, vUV).rgb;\n"
            "    vec3 cl = min(c, vec3(1.0));\n"
            "    vec3 lo = cl / 12.92;\n"
            "    vec3 hi = pow((cl + vec3(0.055)) / 1.055, vec3(2.4));\n"
            "    vec3 lin = mix(lo, hi, step(vec3(0.04045), cl)) + max(c - vec3(1.0), vec3(0.0));\n"
            "    float yl = max(dot(lin, vec3(0.2126, 0.7152, 0.0722)), 0.0001);\n"
            "    float yc = min(yl, 1.0);\n"
            "    float gain = 1.0 + (uBoost - 1.0) * 0.175 * yc * yc * yc * yc;\n"
            "    gain = min(gain, uBoost / yl);\n"
            "    lin *= gain;\n"
            "    mat3 to2020 = mat3(\n"
            "        0.6274, 0.0691, 0.0164,\n"
            "        0.3293, 0.9195, 0.0880,\n"
            "        0.0433, 0.0114, 0.8956);\n"
            "    vec3 y = clamp(to2020 * lin * uScale, 0.0, 1.0);\n"
            "    vec3 yp = pow(y, vec3(0.1593017578125));\n"
            "    vec3 pq = pow((vec3(0.8359375) + 18.8515625 * yp) / (vec3(1.0) + 18.6875 * yp), vec3(78.84375));\n"
            "    outColor = vec4(pq, 1.0);\n"
            "}\n";

        const bool hdr10 = gpu.composition == SDL_GPU_SWAPCHAINCOMPOSITION_HDR10_ST2084;
        bool ok = gfx_sdlgpu_shader_compile_fixed(gpu.device, present_vs_src,
                                                  hdr10 ? present_fs_hdr10_src : present_fs_scrgb_src,
                                                  0 /* vs ubos */, 1 /* fs samplers */, 1 /* fs ubos */,
                                                  &gpu.present_vs, &gpu.present_fs);
        if (ok) {
            SDL_GPUGraphicsPipelineCreateInfo ci;
            SDL_zero(ci);
            ci.vertex_shader = gpu.present_vs;
            ci.fragment_shader = gpu.present_fs;
            ci.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
            ci.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
            ci.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
            ci.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
            ci.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
            SDL_GPUColorTargetDescription ctd;
            SDL_zero(ctd);
            ctd.format = SDL_GetGPUSwapchainTextureFormat(gpu.device, gpu.window);
            ci.target_info.color_target_descriptions = &ctd;
            ci.target_info.num_color_targets = 1;
            gpu.present_pipeline = SDL_CreateGPUGraphicsPipeline(gpu.device, &ci);
            ok = gpu.present_pipeline != NULL;
        }
        if (!ok) {
            // back out to SDR cleanly: no fbs/pipelines exist yet at this point
            sysLogPrintf(LOG_WARNING, "SDL_GPU: HDR present pipeline failed (%s), staying SDR", SDL_GetError());
            gpu.hdr_active = false;
            gpu.composition = SDL_GPU_SWAPCHAINCOMPOSITION_SDR;
            gpu.fb_format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
            gfx_sdlgpu_set_vsync(gpu.swap_interval);
        } else {
            sysLogPrintf(LOG_NOTE, "SDL_GPU: HDR output active (%s, FP16 targets, paper white %g nits)",
                         hdr10 ? "HDR10 ST2084" : "scRGB extended linear", gpu.hdr_paperwhite);
        }
    }

    gpu.invert_y = sysArgCheck("--gpu-invert-y") != 0;
    if (gpu.invert_y) {
        sysLogPrintf(LOG_NOTE, "SDL_GPU: --gpu-invert-y active");
    }

    // depth format: prefer D32, fall back as needed
    static const SDL_GPUTextureFormat depth_candidates[] = {
        SDL_GPU_TEXTUREFORMAT_D32_FLOAT,
        SDL_GPU_TEXTUREFORMAT_D24_UNORM,
        SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT,
        SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT,
    };
    gpu.depth_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    for (size_t i = 0; i < SDL_arraysize(depth_candidates); i++) {
        if (SDL_GPUTextureSupportsFormat(gpu.device, depth_candidates[i], SDL_GPU_TEXTURETYPE_2D,
                                         SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET)) {
            gpu.depth_format = depth_candidates[i];
            break;
        }
    }

    // Raytracing suite: the RT passes sample the framebuffer depth directly,
    // so non-msaa fb depth textures gain SAMPLER usage when the format
    // supports the combination (docs/PORT_RAYTRACING.md).
    gpu.depth_samplable = SDL_GPUTextureSupportsFormat(
        gpu.device, gpu.depth_format, SDL_GPU_TEXTURETYPE_2D,
        SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);

    // per-frame streamed vertex storage, triple-buffered
    for (int i = 0; i < GFX_SDLGPU_VTX_RING; i++) {
        SDL_GPUBufferCreateInfo bci;
        SDL_zero(bci);
        bci.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
        bci.size = GFX_SDLGPU_VTX_BUFFER_CAP;
        gpu.vtx_buf[i] = SDL_CreateGPUBuffer(gpu.device, &bci);

        SDL_GPUTransferBufferCreateInfo tbci;
        SDL_zero(tbci);
        tbci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        tbci.size = GFX_SDLGPU_VTX_BUFFER_CAP;
        gpu.vtx_tbuf[i] = SDL_CreateGPUTransferBuffer(gpu.device, &tbci);

        if (!gpu.vtx_buf[i] || !gpu.vtx_tbuf[i]) {
            sysFatalError("SDL_GPU: could not allocate vertex streaming buffers:\n%s", SDL_GetError());
        }
    }

    // 1x1 white dummy bound wherever a real texture is missing
    {
        SDL_GPUTextureCreateInfo tci;
        SDL_zero(tci);
        tci.type = SDL_GPU_TEXTURETYPE_2D;
        tci.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        tci.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        tci.width = 1;
        tci.height = 1;
        tci.layer_count_or_depth = 1;
        tci.num_levels = 1;
        tci.sample_count = SDL_GPU_SAMPLECOUNT_1;
        gpu.dummy_tex = SDL_CreateGPUTexture(gpu.device, &tci);

        SDL_GPUTransferBufferCreateInfo dtb;
        SDL_zero(dtb);
        dtb.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        dtb.size = 4;
        SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(gpu.device, &dtb);
        if (gpu.dummy_tex && tb) {
            uint8_t *map = (uint8_t *)SDL_MapGPUTransferBuffer(gpu.device, tb, false);
            if (map) {
                map[0] = map[1] = map[2] = map[3] = 255;
                SDL_UnmapGPUTransferBuffer(gpu.device, tb);
            }
            bool adhoc = false;
            SDL_GPUCommandBuffer *cb = upload_cb_get(&adhoc);
            if (cb) {
                SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cb);
                SDL_GPUTextureTransferInfo src;
                SDL_zero(src);
                src.transfer_buffer = tb;
                SDL_GPUTextureRegion dst;
                SDL_zero(dst);
                dst.texture = gpu.dummy_tex;
                dst.w = 1;
                dst.h = 1;
                dst.d = 1;
                SDL_UploadToGPUTexture(cp, &src, &dst, false);
                SDL_EndGPUCopyPass(cp);
                upload_cb_end(cb, adhoc);
            }
        }
        if (tb) {
            SDL_ReleaseGPUTransferBuffer(gpu.device, tb);
        }
    }

    fbs.resize(1); // fb 0 = main offscreen target, sized by update_framebuffer_parameters

    // uniform defaults (match the GL backend's startup state)
    memset(&st.vs_uni, 0, sizeof(st.vs_uni));
    st.vs_uni.mvp[0] = st.vs_uni.mvp[5] = st.vs_uni.mvp[10] = st.vs_uni.mvp[15] = 1.0f;
    st.vs_uni.use_vertex_fog = 1;
    st.vs_uni.palette_w = 1.0f;
    memset(&st.fs_uni, 0, sizeof(st.fs_uni));
    st.fs_uni.emissive = 1.0f;
    st.depth_min = 0.0f;
    st.depth_max = 1.0f;
    st.front_ccw = true;
}

static void gfx_sdlgpu_on_resize(void) {
    // fb0 is resized by update_framebuffer_parameters; the swapchain resizes
    // itself on acquire
}

static void gfx_sdlgpu_start_frame(void) {
    gpu.frame_count++;
    st.fs_uni.frame_count = (int32_t)gpu.frame_count;

    gpu.upload_cb = SDL_AcquireGPUCommandBuffer(gpu.device);
    gpu.render_cb = SDL_AcquireGPUCommandBuffer(gpu.device);
    if (!gpu.upload_cb || !gpu.render_cb) {
        sysLogPrintf(LOG_WARNING, "SDL_GPU: could not acquire command buffers: %s", SDL_GetError());
    }

    gpu.dbg_last_msaa = gpu.dbg_draws_msaa;
    gpu.dbg_last_1x = gpu.dbg_draws_1x;
    gpu.dbg_draws_msaa = 0;
    gpu.dbg_draws_1x = 0;

    // rotate to this frame's streaming pair; the map cycle is safe (its only
    // consumer, the end_frame copy, records after this call) and covers the
    // rare case of the ring wrapping onto a still-pending upload
    gpu.vtx_cur = gpu.frame_count % GFX_SDLGPU_VTX_RING;
    gpu.vtx_map = (uint8_t *)SDL_MapGPUTransferBuffer(gpu.device, gpu.vtx_tbuf[gpu.vtx_cur], true);
    gpu.vtx_used = 0;
    gpu.vtx_overflow_logged = false;

    // pushed uniform state is per command buffer
    st.vs_dirty = true;
    st.fs_dirty = true;
    st.pass = NULL;
    st.bound_pipeline = NULL;
}

static void gfx_sdlgpu_end_frame(void) {
    end_pass();

    if (gpu.vtx_map) {
        SDL_UnmapGPUTransferBuffer(gpu.device, gpu.vtx_tbuf[gpu.vtx_cur]);
        gpu.vtx_map = NULL;
    }

    if (gpu.upload_cb) {
        if (gpu.vtx_used > 0) {
            // one bulk copy of everything draw_triangles wrote this frame; the
            // upload CB is submitted before the render CB, so the data is in
            // place before any draw executes. cycle MUST be false: the frame's
            // vertex-buffer binds are already recorded and have to reference
            // the same backing this copy fills (see GFX_SDLGPU_VTX_RING).
            SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(gpu.upload_cb);
            SDL_GPUTransferBufferLocation src;
            src.transfer_buffer = gpu.vtx_tbuf[gpu.vtx_cur];
            src.offset = 0;
            SDL_GPUBufferRegion dst;
            dst.buffer = gpu.vtx_buf[gpu.vtx_cur];
            dst.offset = 0;
            dst.size = gpu.vtx_used;
            SDL_UploadToGPUBuffer(cp, &src, &dst, false);
            SDL_EndGPUCopyPass(cp);
        }
        SDL_SubmitGPUCommandBuffer(gpu.upload_cb);
        gpu.upload_cb = NULL;
    }

    if (gpu.render_cb && fbs[0].color) {
        // refresh the front-buffer snapshot (recorded after all of the
        // frame's passes, so it holds the completed frame — GL_FRONT
        // semantics for next frame's copy_framebuffer(use_back=false))
        if (!gpu.front_tex || gpu.front_w != fbs[0].w || gpu.front_h != fbs[0].h) {
            if (gpu.front_tex) {
                dead_textures.push_back(gpu.front_tex);
            }
            SDL_GPUTextureCreateInfo tci;
            SDL_zero(tci);
            tci.type = SDL_GPU_TEXTURETYPE_2D;
            tci.format = gpu.fb_format;
            tci.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
            tci.width = fbs[0].w;
            tci.height = fbs[0].h;
            tci.layer_count_or_depth = 1;
            tci.num_levels = 1;
            tci.sample_count = SDL_GPU_SAMPLECOUNT_1;
            gpu.front_tex = SDL_CreateGPUTexture(gpu.device, &tci);
            gpu.front_w = fbs[0].w;
            gpu.front_h = fbs[0].h;
        }
        if (gpu.front_tex) {
            SDL_GPUBlitInfo b;
            SDL_zero(b);
            b.source.texture = fbs[0].color;
            b.source.w = fbs[0].w;
            b.source.h = fbs[0].h;
            b.destination.texture = gpu.front_tex;
            b.destination.w = gpu.front_w;
            b.destination.h = gpu.front_h;
            b.load_op = SDL_GPU_LOADOP_DONT_CARE;
            b.filter = SDL_GPU_FILTER_NEAREST;
            SDL_BlitGPUTexture(gpu.render_cb, &b);
        }
    }

    if (gpu.render_cb) {
        SDL_GPUTexture *swap = NULL;
        Uint32 sw = 0, sh = 0;
        if (SDL_WaitAndAcquireGPUSwapchainTexture(gpu.render_cb, gpu.window, &swap, &sw, &sh) && swap &&
            fbs[0].color) {
            if (gpu.hdr_active && gpu.present_pipeline) {
                // HDR present: fullscreen triangle sampling fb0 and scaling
                // SDR-authored content to the configured paper white
                // (scRGB 1.0 = 80 nits)
                SDL_GPUColorTargetInfo ct;
                SDL_zero(ct);
                ct.texture = swap;
                ct.load_op = SDL_GPU_LOADOP_DONT_CARE; // fully covered
                ct.store_op = SDL_GPU_STOREOP_STORE;
                SDL_GPURenderPass *p = SDL_BeginGPURenderPass(gpu.render_cb, &ct, 1, NULL);
                SDL_BindGPUGraphicsPipeline(p, gpu.present_pipeline);
                SDL_GPUTextureSamplerBinding tb;
                tb.texture = fbs[0].color;
                tb.sampler = sampler_get(SK_FB_LINEAR);
                SDL_BindGPUFragmentSamplers(p, 0, &tb, 1);
                // scRGB: 1.0 = 80 nits; HDR10 PQ: absolute, normalized to 10000
                const float scale = gpu.composition == SDL_GPU_SWAPCHAINCOMPOSITION_HDR10_ST2084
                    ? gpu.hdr_paperwhite / 10000.0f
                    : gpu.hdr_paperwhite / 80.0f;
                // highlight expansion: white reaches peak nits (boost >= 1)
                const float boost = gpu.hdr_peak > gpu.hdr_paperwhite ? gpu.hdr_peak / gpu.hdr_paperwhite : 1.0f;
                const float blk[4] = { scale, boost, 0.0f, 0.0f };
                SDL_PushGPUFragmentUniformData(gpu.render_cb, 0, blk, sizeof(blk));
                SDL_DrawGPUPrimitives(p, 3, 1, 0, 0);
                SDL_EndGPURenderPass(p);
            } else {
                SDL_GPUBlitInfo b;
                SDL_zero(b);
                b.source.texture = fbs[0].color;
                b.source.w = fbs[0].w;
                b.source.h = fbs[0].h;
                b.destination.texture = swap;
                b.destination.w = sw;
                b.destination.h = sh;
                b.load_op = SDL_GPU_LOADOP_DONT_CARE;
                b.filter = (fbs[0].w == sw && fbs[0].h == sh) ? SDL_GPU_FILTER_NEAREST : SDL_GPU_FILTER_LINEAR;
                SDL_BlitGPUTexture(gpu.render_cb, &b);
            }
        }
        SDL_SubmitGPUCommandBuffer(gpu.render_cb);
        gpu.render_cb = NULL;
    }

    // deferred releases: safe now that the command buffers are submitted
    // (SDL_GPU defers actual destruction until the GPU is done with them)
    for (SDL_GPUTexture *t : dead_textures) {
        SDL_ReleaseGPUTexture(gpu.device, t);
    }
    dead_textures.clear();
    for (SDL_GPUTransferBuffer *t : dead_transfers) {
        SDL_ReleaseGPUTransferBuffer(gpu.device, t);
    }
    dead_transfers.clear();
    for (SDL_GPUBuffer *b : dead_buffers) {
        SDL_ReleaseGPUBuffer(gpu.device, b);
    }
    dead_buffers.clear();
}

static void gfx_sdlgpu_finish_render(void) {
}

// ---------------------------------------------------------------------------
// rapi: framebuffers

static int gfx_sdlgpu_create_framebuffer(void) {
    fbs.push_back({});
    return (int)fbs.size() - 1;
}

static void gfx_sdlgpu_update_framebuffer_parameters(int fb_id, uint32_t width, uint32_t height, uint32_t msaa_level,
                                                     bool opengl_invert_y, bool render_target, bool has_depth_buffer,
                                                     bool can_extract_depth) {
    if (fb_id < 0 || fb_id >= (int)fbs.size()) {
        return;
    }
    GpuFb &fb = fbs[fb_id];

    width = width > 1 ? width : 1;
    height = height > 1 ? height : 1;

    const uint32_t msaa = msaa_clamp(msaa_level);

    // opengl_invert_y is a GL-ism (FBO rendering is flipped there); SDL_GPU
    // renders top-down uniformly, so it's ignored on purpose.

    if (fb.color && fb.w == width && fb.h == height && fb.has_depth == has_depth_buffer && fb.msaa == msaa) {
        return;
    }

    // recreating the textures of the framebuffer an open pass is targeting
    // would pull them out from under it
    if (fb_id == st.cur_fb) {
        end_pass();
    }

    if (fb.color) {
        dead_textures.push_back(fb.color);
        fb.color = NULL;
    }
    if (fb.depth) {
        dead_textures.push_back(fb.depth);
        fb.depth = NULL;
    }
    if (fb.resolve) {
        dead_textures.push_back(fb.resolve);
        fb.resolve = NULL;
    }

    SDL_GPUTextureCreateInfo tci;
    SDL_zero(tci);
    tci.type = SDL_GPU_TEXTURETYPE_2D;
    tci.format = gpu.fb_format; // RGBA8, or FP16 when HDR is active
    // multisample textures cannot be sampled in SDL_GPU; the resolve target
    // below takes over the readable role for msaa fbs
    tci.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | (msaa > 1 ? 0 : SDL_GPU_TEXTUREUSAGE_SAMPLER);
    tci.width = width;
    tci.height = height;
    tci.layer_count_or_depth = 1;
    tci.num_levels = 1;
    tci.sample_count = msaa_to_enum(msaa);
    fb.color = SDL_CreateGPUTexture(gpu.device, &tci);
    if (!fb.color) {
        sysLogPrintf(LOG_ERROR, "SDL_GPU: could not create %ux%u framebuffer %d: %s", width, height, fb_id,
                     SDL_GetError());
    }

    if (msaa > 1) {
        tci.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        tci.sample_count = SDL_GPU_SAMPLECOUNT_1;
        fb.resolve = SDL_CreateGPUTexture(gpu.device, &tci);
        if (!fb.resolve) {
            sysLogPrintf(LOG_ERROR, "SDL_GPU: could not create msaa resolve target for framebuffer %d: %s", fb_id,
                         SDL_GetError());
        }
    }

    if (has_depth_buffer) {
        tci.format = gpu.depth_format;
        // non-msaa depth doubles as the RT suite's depth input (msaa depth
        // can be neither sampled nor resolved in SDL_GPU)
        tci.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET |
                    ((msaa <= 1 && gpu.depth_samplable) ? SDL_GPU_TEXTUREUSAGE_SAMPLER : 0);
        tci.sample_count = msaa_to_enum(msaa);
        fb.depth = SDL_CreateGPUTexture(gpu.device, &tci);
        if (!fb.depth) {
            sysLogPrintf(LOG_ERROR, "SDL_GPU: could not create depth buffer for framebuffer %d: %s", fb_id,
                         SDL_GetError());
        }
    }

    fb.w = width;
    fb.h = height;
    fb.msaa = (fb.color != NULL) ? msaa : 1;
    fb.has_depth = has_depth_buffer && fb.depth != NULL;
    fb.color_virgin = true;
    fb.depth_virgin = true;

    // recreation-gated, so this only logs on boot / resize / msaa change —
    // ground truth for whether MSAA is actually applied to the game fb
    sysLogPrintf(LOG_NOTE, "SDL_GPU: framebuffer %d: %ux%u msaa %ux (req %ux)%s%s", fb_id, width, height, fb.msaa,
                 msaa_level, fb.has_depth ? " +depth" : "", fb.resolve ? " +resolve" : "");
}

static bool gfx_sdlgpu_start_draw_to_framebuffer(int fb_id, float noise_scale) {
    if (fb_id < 0 || fb_id >= (int)fbs.size()) {
        return false;
    }
    if (noise_scale != 0.0f) {
        const float ns = 1.0f / noise_scale;
        if (st.fs_uni.noise_scale != ns) {
            st.fs_uni.noise_scale = ns;
            st.fs_dirty = true;
        }
    }
    if (fb_id != st.cur_fb) {
        end_pass();
        st.cur_fb = fb_id;
    }
    return true;
}

static void gfx_sdlgpu_clear_framebuffer(bool clear_color, bool clear_depth) {
    if (!gpu.render_cb) {
        return;
    }
    GpuFb &fb = fbs[st.cur_fb];
    if (!fb.color) {
        return;
    }

    end_pass();

    // an immediate empty pass whose load ops do the clearing; full-target,
    // like GL's scissor-disabled glClear. Virgin attachments clear too so
    // their first use never loads garbage.
    SDL_GPUColorTargetInfo ct;
    SDL_zero(ct);
    ct.texture = fb.color;
    ct.load_op = (clear_color || fb.color_virgin) ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
    ct.store_op = SDL_GPU_STOREOP_STORE;
    ct.clear_color.a = 1.0f;

    SDL_GPUDepthStencilTargetInfo ds;
    SDL_zero(ds);
    const bool has_depth = fb.depth != NULL;
    if (has_depth) {
        ds.texture = fb.depth;
        ds.load_op = (clear_depth || fb.depth_virgin) ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
        ds.store_op = SDL_GPU_STOREOP_STORE;
        ds.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
        ds.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
        ds.clear_depth = 1.0f;
    }

    SDL_GPURenderPass *p = SDL_BeginGPURenderPass(gpu.render_cb, &ct, 1, has_depth ? &ds : NULL);
    SDL_EndGPURenderPass(p);
    fb.color_virgin = false;
    fb.depth_virgin = false;
}

static void gfx_sdlgpu_copy_framebuffer(int fb_dst, int fb_src, int left, int top, bool flip_y, bool use_back) {
    if (fb_dst >= (int)fbs.size() || fb_src >= (int)fbs.size() || fb_dst < 0 || fb_src < 0) {
        return;
    }
    GpuFb &src = fbs[fb_src];
    GpuFb &dst = fbs[fb_dst];

    end_pass();

    // The pause-blur/menu-background copies come from game-TICK code
    // (videoCopyFramebuffer in menugfx.c / pdsched.c), which runs BETWEEN
    // frames — before gfx_run acquires the frame's command buffers. GL is an
    // immediate API and doesn't care; here an ad-hoc CB is recorded and
    // submitted on the spot. Ordering holds: last frame's CBs (including the
    // front_tex snapshot) are already submitted, this frame's come later.
    bool adhoc = false;
    SDL_GPUCommandBuffer *cb = gpu.render_cb;
    if (!cb) {
        cb = SDL_AcquireGPUCommandBuffer(gpu.device);
        adhoc = true;
        if (!cb) {
            return;
        }
    }

    // GL front-buffer semantics: use_back=false on the main fb reads the
    // previously presented frame (glReadBuffer(GL_FRONT)), not the freshly
    // cleared current one — that's what the pause-blur/menu backgrounds blur.
    // msaa sources (the use_back && msaa>1 substitution to game_framebuffer)
    // are resolved into their readable resolve target first.
    SDL_GPUTexture *src_tex;
    uint32_t src_w = src.w, src_h = src.h;
    if (fb_src == 0 && !use_back && gpu.front_tex) {
        src_tex = gpu.front_tex;
        src_w = gpu.front_w;
        src_h = gpu.front_h;
    } else {
        src_tex = fb_readable_color(src, cb);
    }
    if (!src_tex || !dst.color) {
        if (adhoc) {
            SDL_SubmitGPUCommandBuffer(cb);
        }
        return;
    }

    SDL_GPUBlitInfo b;
    SDL_zero(b);
    b.source.texture = src_tex;
    b.destination.texture = dst.color;
    b.destination.w = dst.w;
    b.destination.h = dst.h;
    b.load_op = SDL_GPU_LOADOP_DONT_CARE;
    b.filter = SDL_GPU_FILTER_NEAREST;

    if (left >= 0 && top >= 0) {
        // unscaled rect copy. gfx_pc pre-flips `top` into GL bottom-left
        // window coords for main-fb sources (flip_y == is_main_fb); convert
        // back to our uniform top-left orientation.
        int y = flip_y ? (int)src_h - top - (int)dst.h : top;
        int x = left;
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        if (x + (int)dst.w > (int)src_w) x = (int)src_w - (int)dst.w < 0 ? 0 : (int)src_w - (int)dst.w;
        if (y + (int)dst.h > (int)src_h) y = (int)src_h - (int)dst.h < 0 ? 0 : (int)src_h - (int)dst.h;
        b.source.x = (Uint32)x;
        b.source.y = (Uint32)y;
        b.source.w = dst.w <= src_w ? dst.w : src_w;
        b.source.h = dst.h <= src_h ? dst.h : src_h;
    } else {
        // scaled full copy; both contents are top-down here, no flip needed
        b.source.w = src_w;
        b.source.h = src_h;
    }

    SDL_BlitGPUTexture(cb, &b);
    fbs[fb_dst].color_virgin = false; // blit filled it; don't clear on next pass

    if (adhoc) {
        SDL_SubmitGPUCommandBuffer(cb);
    }
}

static void gfx_sdlgpu_resolve_msaa_color_buffer(int fb_id_target, int fb_id_source) {
    if (!gpu.render_cb || fb_id_target >= (int)fbs.size() || fb_id_source >= (int)fbs.size()) {
        return;
    }
    GpuFb &src = fbs[fb_id_source];
    GpuFb &dst = fbs[fb_id_target];
    if (!src.color || !dst.color) {
        return;
    }

    end_pass();

    if (src.msaa > 1 && src.w == dst.w && src.h == dst.h && dst.msaa <= 1) {
        if (!gpu.dbg_resolve_logged) {
            gpu.dbg_resolve_logged = true;
            sysLogPrintf(LOG_NOTE, "SDL_GPU: msaa resolve active (fb%d %ux -> fb%d)", fb_id_source, src.msaa,
                         fb_id_target);
        }
        // same-size resolve (the only case PD hits: game fb -> fb 0): an
        // empty render pass on the msaa colour that resolves straight into
        // the destination. RESOLVE_AND_STORE keeps the msaa contents valid
        // for a later copy_framebuffer(use_back) read.
        SDL_GPUColorTargetInfo ct;
        SDL_zero(ct);
        ct.texture = src.color;
        ct.load_op = SDL_GPU_LOADOP_LOAD;
        ct.store_op = SDL_GPU_STOREOP_RESOLVE_AND_STORE;
        ct.resolve_texture = dst.color;
        SDL_GPURenderPass *p = SDL_BeginGPURenderPass(gpu.render_cb, &ct, 1, NULL);
        SDL_EndGPURenderPass(p);
        fbs[fb_id_target].color_virgin = false;
        return;
    }

    // fallback: resolve (if needed) into a readable texture, then blit scaled
    SDL_GPUTexture *src_tex = fb_readable_color(src, gpu.render_cb);
    if (!src_tex) {
        return;
    }

    SDL_GPUBlitInfo b;
    SDL_zero(b);
    b.source.texture = src_tex;
    b.source.w = src.w;
    b.source.h = src.h;
    b.destination.texture = dst.msaa > 1 ? dst.resolve : dst.color;
    b.destination.w = dst.w;
    b.destination.h = dst.h;
    b.load_op = SDL_GPU_LOADOP_DONT_CARE;
    b.filter = SDL_GPU_FILTER_NEAREST;
    if (b.destination.texture) {
        SDL_BlitGPUTexture(gpu.render_cb, &b);
        fbs[fb_id_target].color_virgin = false;
    }
}

static void *gfx_sdlgpu_get_framebuffer_texture_id(int fb_id) {
    // opaque non-zero cookie; nothing in this port dereferences it (the
    // gfxFramebuffer texture-draw path is unused: the game always renders at
    // window size)
    return (void *)(uintptr_t)(0x40000000u | (uint32_t)fb_id);
}

static void gfx_sdlgpu_select_texture_fb(int fb_id) {
    st.tile[0].kind = TILE_FB;
    st.tile[0].idx = (uint32_t)fb_id;
    if (st.fs_uni.three_point_filter0 != 1) {
        st.fs_uni.three_point_filter0 = 1; // GL marks fb textures linear
        st.fs_dirty = true;
    }
}

// ---------------------------------------------------------------------------
// rapi: filter modes / misc

static void gfx_sdlgpu_set_texture_filter(FilteringMode mode) {
    gpu.filter_mode = mode; // gfx_pc clears shaders + textures around this
}

static FilteringMode gfx_sdlgpu_get_texture_filter(void) {
    return gpu.filter_mode;
}

static void gfx_sdlgpu_set_mipmap_filter(MipmapFilteringMode mode) {
    gpu.mipmap_mode = mode;
}

static void gfx_sdlgpu_set_anisotropy_level(int level) {
    gpu.anisotropy = level;
}

static int gfx_sdlgpu_get_max_anisotropy_level(void) {
    return 16;
}

// ---------------------------------------------------------------------------
// rapi: uniform-driven entries shared with the display-list cache

static void gfx_sdlgpu_set_mvp(const float m[16]) {
    memcpy(st.vs_uni.mvp, m, sizeof(st.vs_uni.mvp));
    st.vs_dirty = true;
}

static void gfx_sdlgpu_set_fog_params(int use_vertex_fog, float fog_mul, float fog_off) {
    st.vs_uni.use_vertex_fog = use_vertex_fog;
    st.vs_uni.fog_mul = fog_mul;
    st.vs_uni.fog_off = fog_off;
    st.vs_dirty = true;
}

static void gfx_sdlgpu_set_palette_enable(int enable) {
    st.vs_uni.palette_enable = enable;
    st.vs_dirty = true;
}

static void gfx_sdlgpu_set_shade_routing(int packed) {
    st.vs_uni.shade_route = packed;
    st.vs_dirty = true;
}

// ---------------------------------------------------------------------------
// rapi: display-list cache (port-only; see docs/PORT_DLCACHE.md)
//
// Persistent per-leaf vertex buffers of object-space geometry, uploaded once
// and replayed each frame with the folded uMVP. Vertex stride is
// (num_floats + 1) floats — the trailing float is aShadeIdx, the palette
// index consumed by the cached VS variant. Returning 0 from create marks the
// leaf bad in gfx_pc -> per-leaf legacy fallback (also our error path).

static uint32_t gfx_sdlgpu_cache_create_buffer(const float *data, size_t num_floats) {
    if (!gpu.device || num_floats == 0) {
        return 0;
    }

    const uint32_t size = (uint32_t)(num_floats * sizeof(float));

    SDL_GPUBufferCreateInfo bci;
    SDL_zero(bci);
    bci.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    bci.size = size;
    SDL_GPUBuffer *buf = SDL_CreateGPUBuffer(gpu.device, &bci);
    if (!buf) {
        sysLogPrintf(LOG_WARNING, "SDL_GPU: dlcache buffer alloc (%u bytes) failed: %s", size, SDL_GetError());
        return 0;
    }

    SDL_GPUTransferBufferCreateInfo tbci;
    SDL_zero(tbci);
    tbci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tbci.size = size;
    SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(gpu.device, &tbci);
    if (!tb) {
        SDL_ReleaseGPUBuffer(gpu.device, buf);
        return 0;
    }
    void *map = SDL_MapGPUTransferBuffer(gpu.device, tb, false);
    if (!map) {
        SDL_ReleaseGPUTransferBuffer(gpu.device, tb);
        SDL_ReleaseGPUBuffer(gpu.device, buf);
        return 0;
    }
    memcpy(map, data, size);
    SDL_UnmapGPUTransferBuffer(gpu.device, tb);

    // one-shot upload; fresh buffer so no cycling concerns. Recording happens
    // mid-frame on the upload CB (executes before the render CB's draws);
    // the ad-hoc path covers out-of-frame creation.
    bool adhoc = false;
    SDL_GPUCommandBuffer *cb = upload_cb_get(&adhoc);
    if (!cb) {
        SDL_ReleaseGPUTransferBuffer(gpu.device, tb);
        SDL_ReleaseGPUBuffer(gpu.device, buf);
        return 0;
    }
    SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cb);
    SDL_GPUTransferBufferLocation src;
    src.transfer_buffer = tb;
    src.offset = 0;
    SDL_GPUBufferRegion dst;
    dst.buffer = buf;
    dst.offset = 0;
    dst.size = size;
    SDL_UploadToGPUBuffer(cp, &src, &dst, false);
    SDL_EndGPUCopyPass(cp);
    upload_cb_end(cb, adhoc);
    dead_transfers.push_back(tb);

    for (size_t i = 0; i < cache_bufs.size(); i++) {
        if (!cache_bufs[i].in_use) {
            cache_bufs[i] = { buf, true };
            return (uint32_t)i + 1;
        }
    }
    cache_bufs.push_back({ buf, true });
    return (uint32_t)cache_bufs.size();
}

static void gfx_sdlgpu_cache_delete_buffer(uint32_t id) {
    if (id == 0 || id > cache_bufs.size()) {
        return;
    }
    CacheBuf &e = cache_bufs[id - 1];
    if (e.buf) {
        dead_buffers.push_back(e.buf); // deferred: this frame's recorded draws may reference it
    }
    e = {};
    if (st.cache_buf == id) {
        st.cache_buf = 0;
    }
}

static void gfx_sdlgpu_cache_replay_begin(uint32_t id) {
    st.cache_buf = (id > 0 && id <= cache_bufs.size() && cache_bufs[id - 1].buf) ? id : 0;
}

static void gfx_sdlgpu_cache_set_cull(int mode, bool front_ccw) {
    st.cull_mode = (uint8_t)mode;
    st.front_ccw = front_ccw;
}

static void gfx_sdlgpu_cache_draw(struct ShaderProgram *prg, size_t base_float, size_t num_tris) {
    if (!prg || !gpu.render_cb || st.cache_buf == 0 || num_tris == 0) {
        return;
    }
    SDL_GPUBuffer *vb = cache_bufs[st.cache_buf - 1].buf;
    if (!vb) {
        return;
    }

    // replay loads the segment's shader via load_shader, but be defensive
    st.prg = prg;

    if (!prg->vs_cached && !gfx_sdlgpu_shader_compile_cached_vs(gpu.device, prg)) {
        return;
    }

    ensure_pass();
    if (!st.pass) {
        return;
    }

    SDL_GPUGraphicsPipeline *pipe = pipeline_resolve(true);
    if (!pipe) {
        return;
    }
    if (pipe != st.bound_pipeline) {
        SDL_BindGPUGraphicsPipeline(st.pass, pipe);
        st.bound_pipeline = pipe;
    }

    SDL_GPUBufferBinding bb;
    bb.buffer = vb;
    bb.offset = (Uint32)(base_float * sizeof(float));
    SDL_BindGPUVertexBuffers(st.pass, 0, &bb, 1);

    // palette VS variant samples uPalette in the vertex stage (set=0)
    if (prg->vs_cached != prg->vs) {
        SDL_GPUTextureSamplerBinding pb;
        pb.texture = st.palette_tex ? st.palette_tex : gpu.dummy_tex;
        pb.sampler = sampler_get(0); // nearest/clamp; texelFetch ignores filtering
        SDL_BindGPUVertexSamplers(st.pass, 0, &pb, 1);
    }

    bind_tile_samplers();
    update_emissive();
    push_uniforms();

    if (fbs[st.cur_fb].msaa > 1) {
        gpu.dbg_draws_msaa++;
    } else {
        gpu.dbg_draws_1x++;
    }

    SDL_DrawGPUPrimitives(st.pass, (Uint32)(3 * num_tris), 1, 0, 0);
}

static void gfx_sdlgpu_cache_replay_end(void) {
    // mirror GL: subsequent immediate draws are uncull-ed and re-bind their
    // own vertex buffer per draw
    st.cache_buf = 0;
    st.cull_mode = 0;
    st.front_ccw = true;
}

static uint32_t gfx_sdlgpu_cache_create_palette(void) {
    // the texture itself is (re)created at upload time, when the count is
    // known; a non-zero handle is the palette_ok signal for gfx_pc
    for (size_t i = 0; i < palettes.size(); i++) {
        if (!palettes[i].in_use) {
            palettes[i] = { NULL, 0, true };
            return (uint32_t)i + 1;
        }
    }
    palettes.push_back({ NULL, 0, true });
    return (uint32_t)palettes.size();
}

static void gfx_sdlgpu_cache_delete_palette(uint32_t id) {
    if (id == 0 || id > palettes.size()) {
        return;
    }
    PalEntry &e = palettes[id - 1];
    if (e.tex) {
        dead_textures.push_back(e.tex);
    }
    if (st.palette_tex == e.tex) {
        st.palette_tex = NULL;
    }
    e = {};
}

static void gfx_sdlgpu_cache_upload_palette(uint32_t id, const void *rgba, int count) {
    if (id == 0 || id > palettes.size() || count <= 0) {
        return;
    }
    PalEntry &e = palettes[id - 1];

    // fresh texture per upload (same rule as regular textures): draws already
    // recorded this frame keep sampling the old contents
    if (e.tex) {
        if (st.palette_tex == e.tex) {
            st.palette_tex = NULL;
        }
        dead_textures.push_back(e.tex);
        e.tex = NULL;
    }

    SDL_GPUTextureCreateInfo tci;
    SDL_zero(tci);
    tci.type = SDL_GPU_TEXTURETYPE_2D;
    tci.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    tci.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    tci.width = (Uint32)count;
    tci.height = 1;
    tci.layer_count_or_depth = 1;
    tci.num_levels = 1;
    tci.sample_count = SDL_GPU_SAMPLECOUNT_1;
    e.tex = SDL_CreateGPUTexture(gpu.device, &tci);
    if (!e.tex) {
        sysLogPrintf(LOG_WARNING, "SDL_GPU: dlcache palette (%d) creation failed: %s", count, SDL_GetError());
        return;
    }
    e.count = count;

    SDL_GPUTransferBufferCreateInfo tbci;
    SDL_zero(tbci);
    tbci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tbci.size = (Uint32)count * 4;
    SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(gpu.device, &tbci);
    if (!tb) {
        return;
    }
    void *map = SDL_MapGPUTransferBuffer(gpu.device, tb, false);
    if (map) {
        memcpy(map, rgba, (size_t)count * 4);
        SDL_UnmapGPUTransferBuffer(gpu.device, tb);
    }

    bool adhoc = false;
    SDL_GPUCommandBuffer *cb = upload_cb_get(&adhoc);
    if (cb) {
        SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cb);
        SDL_GPUTextureTransferInfo src;
        SDL_zero(src);
        src.transfer_buffer = tb;
        SDL_GPUTextureRegion dst;
        SDL_zero(dst);
        dst.texture = e.tex;
        dst.w = (Uint32)count;
        dst.h = 1;
        dst.d = 1;
        SDL_UploadToGPUTexture(cp, &src, &dst, false);
        SDL_EndGPUCopyPass(cp);
        upload_cb_end(cb, adhoc);
    }
    dead_transfers.push_back(tb);
}

static void gfx_sdlgpu_cache_bind_palette(uint32_t id, int count) {
    st.palette_tex = (id > 0 && id <= palettes.size()) ? palettes[id - 1].tex : NULL;
    if (st.vs_uni.palette_w != (float)count) {
        st.vs_uni.palette_w = (float)count;
        st.vs_dirty = true;
    }
}

// ---------------------------------------------------------------------------
// Screen-space raytracing suite (port-only; docs/PORT_RAYTRACING.md)
//
// SDL_GPU implementation of the rt_resolve rapi hook — the same pass
// algorithms as the GL backend (shared bodies in gfx_rt_common.h), compiled
// as GLSL450 through the same glslang/SPIRV-Cross pipeline as the combiner
// and HDR present shaders, so it works on Vulkan and D3D12 (and best-effort
// Metal) alike. Differences from the GL implementation:
//  - depth is sampled DIRECTLY from the framebuffer depth texture (created
//    with SAMPLER usage when gpu.depth_samplable) — no depth copy exists;
//  - scene colour is captured with one same-format texture-to-texture copy;
//  - all pass parameters ride one std140 UBO (set=3) whose MEMBERS carry the
//    exact bare names the shared bodies reference;
//  - the fullscreen triangle is gl_VertexIndex-generated (no vertex buffer),
//    with the viewport uv rect in a tiny VS UBO (set=1);
//  - MSAA framebuffers are unsupported: SDL_GPU can neither sample nor
//    resolve multisample depth — the resolve logs once and no-ops;
//  - uYSign is -1: fb0 is stored top-down (top-left texture origin), the
//    vertical inverse of the GL default. --gpu-invert-y flips it back.
// Depth linearization is unchanged: gfx_pc's 0..1-clip remap is
// z01 = (z+w)/2, so the shared lin() (d*2-1 -> GL ndc) still holds.

// std140 mirror of the RtUni block emitted by rt_build_fs_source below.
// vec3+float pairs share one 16-byte slot; the two vec2s share another.
struct RtGpuUni {
    float cur_to_prev[16];
    float proj[4];
    float rect[4];
    float sun[3]; float ysign;
    float sky[3]; float blend;
    float texel[2]; float dir[2];
    float ao_radius, shadow_len, gi_radius, max_dist;
    float ao_int, sh_int, gi_int, ssr_int;
    int32_t frame, ao_on, shadow_on, gi_on;
    int32_t ssr_on, mode, ao_samples, shadow_steps;
    int32_t rays, steps, bounces, ssr_steps;
    // dark/relight mode (order matches the GLSL block extension)
    int32_t light_count, light_shadows, light_steps, torch;
    float torch_int, torch_range, dark_ambient; int32_t dark;
    int32_t lights_on; int32_t pad[3];
};

// std140 mirror of the light pass's second block (set=3, binding=1):
// view-space light positions + falloff radii, premultiplied colours
struct RtGpuLights {
    float posrad[RT_MAX_LIGHTS][4];
    float col[RT_MAX_LIGHTS][4];
};

enum {
    RTP_PREPASS, RTP_AOSHADOW, RTP_TRACE, RTP_TEMPORAL,
    RTP_BLUR, RTP_SSR, RTP_LIGHT, RTP_COMP_MUL, RTP_COMP_ADD, RTP_DEBUG,
    RTP_COUNT
};

static struct {
    bool broken, inited;
    bool msaa_warned, depth_warned;
    SDL_GPUShader *vs[RTP_COUNT]; // compile_fixed creates one VS per pair
    SDL_GPUShader *fs[RTP_COUNT];
    SDL_GPUGraphicsPipeline *pipe[RTP_COUNT];
    SDL_GPUGraphicsPipeline *pipe_blur16; // blur variant targeting the FP16 GI textures
    int fbw, fbh, giw, gih;
    float giscale;
    uint32_t frame;
    SDL_GPUTexture *scene_col;                             // gpu.fb_format copy of the fb colour
    SDL_GPUTexture *norm_tex;                              // RGBA16F: view normal + linear depth
    SDL_GPUTexture *ao_tex, *aotmp_tex;                    // RGBA8: r = AO, g = shadow
    SDL_GPUTexture *ssr_tex;                               // RGBA8: reflection colour + confidence
    SDL_GPUTexture *light_tex;                             // RGBA16F: dynamic-light radiance (dark mode)
    SDL_GPUTexture *gitrace_tex, *gitmp_tex, *gifinal_tex; // RGBA16F at giscale
    SDL_GPUTexture *hist_tex[RT_MAX_PLAYERS][2];           // per-player temporal ping-pong
    int hist_idx[RT_MAX_PLAYERS];
    bool prev_valid[RT_MAX_PLAYERS];
    float prev_view[RT_MAX_PLAYERS][16];
    float prev_fovy[RT_MAX_PLAYERS], prev_aspect[RT_MAX_PLAYERS];
    float prev_znear[RT_MAX_PLAYERS], prev_zfar[RT_MAX_PLAYERS];
} rt = {};

// per-pass fragment sampler lists; array order = contiguous set=2 bindings =
// the order rt_run_pass binds textures in
static const struct {
    const char *names[5];
    int count;
} rt_pass_samplers[RTP_COUNT] = {
    { { "uDepth" }, 1 },                                // prepass
    { { "uNorm" }, 1 },                                 // aoshadow
    { { "uNorm", "uColor" }, 2 },                       // trace
    { { "uNorm", "uCur", "uHist" }, 3 },                // temporal
    { { "uNorm", "uSrc" }, 2 },                         // blur
    { { "uNorm", "uColor" }, 2 },                       // ssr
    { { "uNorm" }, 1 },                                 // light
    { { "uAO" }, 1 },                                   // comp_mul
    { { "uColor", "uGI", "uSSR", "uLight" }, 4 },       // comp_add
    { { "uNorm", "uAO", "uGI", "uSSR", "uLight" }, 5 }, // debug
};

static const char *const rt_pass_bodies[RTP_COUNT] = {
    RT_FS_PREPASS_BODY, RT_FS_AOSHADOW_BODY, RT_FS_TRACE_BODY, RT_FS_TEMPORAL_BODY,
    RT_FS_BLUR_BODY, RT_FS_SSR_BODY, RT_FS_LIGHT_BODY, RT_FS_COMP_MUL_BODY, RT_FS_COMP_ADD_BODY,
    RT_FS_DEBUG_BODY,
};

// Fullscreen triangle from gl_VertexIndex (the present-VS pattern). The uv
// mapping accounts for the top-left texture origin: ndc.y = +1 (top of the
// viewport) must sample the rect's v-MIN row.
static const char *const rt_vs_src =
    "#version 450\n"
    "layout(location = 0) out vec2 vUV;\n"
    "layout(std140, set = 1, binding = 0) uniform RtVs { vec4 uVsRect; };\n"
    "void main() {\n"
    "    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);\n"
    "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "    vUV = vec2(mix(uVsRect.x, uVsRect.z, p.x), mix(uVsRect.w, uVsRect.y, p.y));\n"
    "}\n";

// assemble a pass's GLSL450 fragment source: layouts + samplers + the shared
// UBO (member names = the bare names the shared bodies reference) + helpers
// + body. Caller frees.
static char *rt_build_fs_source(int pass) {
    static const char *const ubo =
        "layout(std140, set = 3, binding = 0) uniform RtUni {\n"
        "    mat4 uCurToPrev;\n"
        "    vec4 uProj;\n"
        "    vec4 uRect;\n"
        "    vec3 uSun; float uYSign;\n"
        "    vec3 uSky; float uBlend;\n"
        "    vec2 uTexel; vec2 uDir;\n"
        "    float uAORadius; float uShadowLen; float uGIRadius; float uMaxDist;\n"
        "    float uAOInt; float uShInt; float uGIInt; float uSSRInt;\n"
        "    int uFrame; int uAOOn; int uShadowOn; int uGIOn;\n"
        "    int uSSROn; int uMode; int uAOSamples; int uShadowSteps;\n"
        "    int uRays; int uSteps; int uBounces; int uSSRSteps;\n"
        "    int uLightCount; int uLightShadows; int uLightSteps; int uTorch;\n"
        "    float uTorchInt; float uTorchRange; float uDarkAmbient; int uDark;\n"
        "    int uLightsOn; int uPad0; int uPad1; int uPad2;\n"
        "};\n";
    // the light pass's second block (RtGpuLights, pushed on fragment slot 1)
    static const char *const lights_ubo =
        "layout(std140, set = 3, binding = 1) uniform RtLightsUni {\n"
        "    vec4 uLightPosRad[32];\n" // 32 == RT_MAX_LIGHTS
        "    vec4 uLightCol[32];\n"
        "};\n";
    const size_t cap = strlen(ubo) + strlen(lights_ubo) + strlen(RT_GLSL_HELPERS) +
                       strlen(rt_pass_bodies[pass]) + 1024;
    char *src = (char *)malloc(cap);
    if (!src) {
        return NULL;
    }
    char *p = src;
    p += sprintf(p, "#version 450\n"
                    "layout(location = 0) in vec2 vUV;\n"
                    "layout(location = 0) out vec4 oCol;\n");
    for (int i = 0; i < rt_pass_samplers[pass].count; i++) {
        p += sprintf(p, "layout(set = 2, binding = %d) uniform sampler2D %s;\n", i,
                     rt_pass_samplers[pass].names[i]);
    }
    strcpy(p, ubo);
    p += strlen(ubo);
    if (pass == RTP_LIGHT) {
        strcpy(p, lights_ubo);
        p += strlen(lights_ubo);
    }
    strcpy(p, RT_GLSL_HELPERS);
    p += strlen(RT_GLSL_HELPERS);
    strcpy(p, rt_pass_bodies[pass]);
    return src;
}

// blend: 0 = none, 1 = multiplicative (dst_new = src * dst), 2 = additive
static SDL_GPUGraphicsPipeline *rt_make_pipeline(SDL_GPUShader *vs, SDL_GPUShader *fs,
                                                 SDL_GPUTextureFormat fmt, int blend) {
    SDL_GPUGraphicsPipelineCreateInfo ci;
    SDL_zero(ci);
    ci.vertex_shader = vs;
    ci.fragment_shader = fs;
    ci.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    ci.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    ci.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    ci.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    ci.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;

    SDL_GPUColorTargetDescription ctd;
    SDL_zero(ctd);
    ctd.format = fmt;
    if (blend != 0) {
        ctd.blend_state.enable_blend = true;
        ctd.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
        ctd.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
        if (blend == 1) {
            ctd.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_DST_COLOR;
            ctd.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
            ctd.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_DST_ALPHA;
            ctd.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
        } else {
            ctd.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
            ctd.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
            ctd.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
            ctd.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        }
    }
    ci.target_info.color_target_descriptions = &ctd;
    ci.target_info.num_color_targets = 1;
    return SDL_CreateGPUGraphicsPipeline(gpu.device, &ci);
}

static bool rt_init(void) {
    const SDL_GPUTextureFormat fmt16 = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
    const SDL_GPUTextureFormat fmt8 = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;

    if (!SDL_GPUTextureSupportsFormat(gpu.device, fmt16, SDL_GPU_TEXTURETYPE_2D,
                                      SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER)) {
        sysLogPrintf(LOG_ERROR, "SDL_GPU RT: FP16 render targets unsupported — raytracing disabled");
        return false;
    }

    for (int i = 0; i < RTP_COUNT; i++) {
        char *fs_src = rt_build_fs_source(i);
        if (!fs_src) {
            return false;
        }
        // the light pass declares the second (lights) uniform block
        const unsigned int fs_ubos = (i == RTP_LIGHT) ? 2 : 1;
        const bool ok = gfx_sdlgpu_shader_compile_fixed(gpu.device, rt_vs_src, fs_src,
                                                        1 /* vs ubos */, rt_pass_samplers[i].count,
                                                        fs_ubos, &rt.vs[i], &rt.fs[i]);
        free(fs_src);
        if (!ok) {
            sysLogPrintf(LOG_ERROR, "SDL_GPU RT: pass %d shader failed — raytracing disabled", i);
            return false;
        }
    }

    rt.pipe[RTP_PREPASS] = rt_make_pipeline(rt.vs[RTP_PREPASS], rt.fs[RTP_PREPASS], fmt16, 0);
    rt.pipe[RTP_AOSHADOW] = rt_make_pipeline(rt.vs[RTP_AOSHADOW], rt.fs[RTP_AOSHADOW], fmt8, 0);
    rt.pipe[RTP_TRACE] = rt_make_pipeline(rt.vs[RTP_TRACE], rt.fs[RTP_TRACE], fmt16, 0);
    rt.pipe[RTP_TEMPORAL] = rt_make_pipeline(rt.vs[RTP_TEMPORAL], rt.fs[RTP_TEMPORAL], fmt16, 0);
    rt.pipe[RTP_BLUR] = rt_make_pipeline(rt.vs[RTP_BLUR], rt.fs[RTP_BLUR], fmt8, 0);
    rt.pipe_blur16 = rt_make_pipeline(rt.vs[RTP_BLUR], rt.fs[RTP_BLUR], fmt16, 0);
    rt.pipe[RTP_SSR] = rt_make_pipeline(rt.vs[RTP_SSR], rt.fs[RTP_SSR], fmt8, 0);
    rt.pipe[RTP_LIGHT] = rt_make_pipeline(rt.vs[RTP_LIGHT], rt.fs[RTP_LIGHT], fmt16, 0);
    rt.pipe[RTP_COMP_MUL] = rt_make_pipeline(rt.vs[RTP_COMP_MUL], rt.fs[RTP_COMP_MUL], gpu.fb_format, 1);
    rt.pipe[RTP_COMP_ADD] = rt_make_pipeline(rt.vs[RTP_COMP_ADD], rt.fs[RTP_COMP_ADD], gpu.fb_format, 2);
    rt.pipe[RTP_DEBUG] = rt_make_pipeline(rt.vs[RTP_DEBUG], rt.fs[RTP_DEBUG], gpu.fb_format, 0);

    for (int i = 0; i < RTP_COUNT; i++) {
        if (!rt.pipe[i]) {
            sysLogPrintf(LOG_ERROR, "SDL_GPU RT: pipeline %d failed (%s) — raytracing disabled", i, SDL_GetError());
            return false;
        }
    }
    if (!rt.pipe_blur16) {
        sysLogPrintf(LOG_ERROR, "SDL_GPU RT: fp16 blur pipeline failed — raytracing disabled");
        return false;
    }

    sysLogPrintf(LOG_NOTE, "SDL_GPU RT: raytracing suite initialized (%s)", gfx_sdlgpu_shader_format_name());
    return true;
}

// COLOR_TARGET|SAMPLER texture, cleared once at creation so blur edge-taps
// and history reads outside the viewport rect never see garbage
static SDL_GPUTexture *rt_make_tex(SDL_GPUTextureFormat fmt, int w, int h) {
    SDL_GPUTextureCreateInfo tci;
    SDL_zero(tci);
    tci.type = SDL_GPU_TEXTURETYPE_2D;
    tci.format = fmt;
    tci.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    tci.width = (Uint32)w;
    tci.height = (Uint32)h;
    tci.layer_count_or_depth = 1;
    tci.num_levels = 1;
    tci.sample_count = SDL_GPU_SAMPLECOUNT_1;
    SDL_GPUTexture *t = SDL_CreateGPUTexture(gpu.device, &tci);
    if (t && gpu.render_cb) {
        SDL_GPUColorTargetInfo ct;
        SDL_zero(ct);
        ct.texture = t;
        ct.load_op = SDL_GPU_LOADOP_CLEAR;
        ct.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass *p = SDL_BeginGPURenderPass(gpu.render_cb, &ct, 1, NULL);
        SDL_EndGPURenderPass(p);
    }
    return t;
}

static void rt_release_tex(SDL_GPUTexture **t) {
    if (*t) {
        dead_textures.push_back(*t);
        *t = NULL;
    }
}

static bool rt_build_targets(int fbw, int fbh, float giscale) {
    rt_release_tex(&rt.scene_col);
    rt_release_tex(&rt.norm_tex);
    rt_release_tex(&rt.ao_tex);
    rt_release_tex(&rt.aotmp_tex);
    rt_release_tex(&rt.ssr_tex);
    rt_release_tex(&rt.light_tex);
    rt_release_tex(&rt.gitrace_tex);
    rt_release_tex(&rt.gitmp_tex);
    rt_release_tex(&rt.gifinal_tex);
    for (int p = 0; p < RT_MAX_PLAYERS; p++) {
        rt_release_tex(&rt.hist_tex[p][0]);
        rt_release_tex(&rt.hist_tex[p][1]);
        rt.prev_valid[p] = false;
    }

    const SDL_GPUTextureFormat fmt16 = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
    const SDL_GPUTextureFormat fmt8 = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;

    rt.giw = (int)(fbw * giscale);
    rt.gih = (int)(fbh * giscale);
    if (rt.giw < 1) rt.giw = 1;
    if (rt.gih < 1) rt.gih = 1;

    rt.scene_col = rt_make_tex(gpu.fb_format, fbw, fbh);
    rt.norm_tex = rt_make_tex(fmt16, fbw, fbh);
    rt.ao_tex = rt_make_tex(fmt8, fbw, fbh);
    rt.aotmp_tex = rt_make_tex(fmt8, fbw, fbh);
    rt.ssr_tex = rt_make_tex(fmt8, fbw, fbh);
    rt.light_tex = rt_make_tex(fmt16, fbw, fbh);
    rt.gitrace_tex = rt_make_tex(fmt16, rt.giw, rt.gih);
    rt.gitmp_tex = rt_make_tex(fmt16, rt.giw, rt.gih);
    rt.gifinal_tex = rt_make_tex(fmt16, rt.giw, rt.gih);

    if (!rt.scene_col || !rt.norm_tex || !rt.ao_tex || !rt.aotmp_tex || !rt.ssr_tex || !rt.light_tex ||
        !rt.gitrace_tex || !rt.gitmp_tex || !rt.gifinal_tex) {
        sysLogPrintf(LOG_ERROR, "SDL_GPU RT: render target creation failed: %s", SDL_GetError());
        return false;
    }

    rt.fbw = fbw;
    rt.fbh = fbh;
    rt.giscale = giscale;
    return true;
}

static bool rt_ensure_history(int player) {
    if (rt.hist_tex[player][0]) {
        return true;
    }
    const SDL_GPUTextureFormat fmt16 = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
    rt.hist_tex[player][0] = rt_make_tex(fmt16, rt.giw, rt.gih); // cleared: a=0 -> "no history"
    rt.hist_tex[player][1] = rt_make_tex(fmt16, rt.giw, rt.gih);
    rt.hist_idx[player] = 0;
    return rt.hist_tex[player][0] && rt.hist_tex[player][1];
}

// one fullscreen pass: LOAD/STORE render pass on target, viewport = the
// (top-left) rect, per-pass UBO push, bind inputs in rt_pass_samplers order
static void rt_run_pass(SDL_GPUGraphicsPipeline *pipe, SDL_GPUTexture *target, int vx_tl, int vy_tl, int vw, int vh,
                        SDL_GPUTexture *const texs[], const uint32_t skeys[], int ntex, const RtGpuUni *uni) {
    SDL_GPUColorTargetInfo ct;
    SDL_zero(ct);
    ct.texture = target;
    ct.load_op = SDL_GPU_LOADOP_LOAD;
    ct.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass *p = SDL_BeginGPURenderPass(gpu.render_cb, &ct, 1, NULL);
    SDL_BindGPUGraphicsPipeline(p, pipe);

    SDL_GPUViewport vp;
    vp.x = (float)vx_tl;
    vp.y = (float)vy_tl;
    vp.w = (float)vw;
    vp.h = (float)vh;
    vp.min_depth = 0.0f;
    vp.max_depth = 1.0f;
    SDL_SetGPUViewport(p, &vp);

    SDL_GPUTextureSamplerBinding binds[8];
    for (int i = 0; i < ntex; i++) {
        binds[i].texture = texs[i];
        binds[i].sampler = sampler_get(skeys[i]);
    }
    SDL_BindGPUFragmentSamplers(p, 0, binds, (Uint32)ntex);

    SDL_PushGPUFragmentUniformData(gpu.render_cb, 0, uni, sizeof(*uni));
    SDL_DrawGPUPrimitives(p, 3, 1, 0, 0);
    SDL_EndGPURenderPass(p);
}

static void gfx_sdlgpu_rt_resolve(const void *camv, int vx, int vy, int vw, int vh) {
    const rtcamera *cam = (const rtcamera *)camv;
    if (rt.broken || !cam || !cam->valid || !gpu.render_cb) {
        return;
    }
    if (cam->playernum < 0 || cam->playernum >= RT_MAX_PLAYERS) {
        return;
    }
    GpuFb &fb = fbs[st.cur_fb];
    if (!fb.color || !fb.depth) {
        return;
    }
    if (fb.msaa > 1) {
        if (!rt.msaa_warned) {
            rt.msaa_warned = true;
            sysLogPrintf(LOG_WARNING,
                         "SDL_GPU RT: raytracing needs MSAA off on this backend (multisample depth "
                         "can't be sampled or resolved) — set Video.MSAA=1");
        }
        return;
    }
    if (!gpu.depth_samplable) {
        if (!rt.depth_warned) {
            rt.depth_warned = true;
            sysLogPrintf(LOG_WARNING, "SDL_GPU RT: depth format not samplable on this driver — raytracing disabled");
        }
        return;
    }

    const bool ao_on = gfx_rt_ao != 0;
    const bool sh_on = gfx_rt_shadows != 0;
    const bool ssr_on = gfx_rt_ssr != 0;
    const bool dark_on = gfx_rt_dark != 0;
    const int gi_mode = (gfx_rt_gi < 0) ? 0 : (gfx_rt_gi > 2 ? 2 : gfx_rt_gi);
    const int dbg = (gfx_rt_debug > 0 && gfx_rt_debug < RT_DEBUG_MAX) ? gfx_rt_debug : 0;
    int nlights = (gfx_rt_lights && cam->lightcount > 0) ? cam->lightcount : 0;
    if (nlights > RT_MAX_LIGHTS) {
        nlights = RT_MAX_LIGHTS;
    }
    const bool lights_run = nlights > 0 || gfx_rt_torch != 0 || dbg == RT_DEBUG_LIGHT;
    if (!ao_on && !sh_on && !ssr_on && gi_mode == RT_GI_OFF && dbg == 0 && !dark_on && !lights_run) {
        return;
    }

    end_pass();

    if (!rt.inited) {
        rt.inited = true;
        if (!rt_init()) {
            rt.broken = true;
            gfx_rt_enabled = 0;
            return;
        }
    }

    float giscale = gfx_rt_gi_scale;
    if (giscale < 0.25f) giscale = 0.25f;
    if (giscale > 1.0f) giscale = 1.0f;
    if (rt.fbw != (int)fb.w || rt.fbh != (int)fb.h || rt.giscale != giscale) {
        if (!rt_build_targets((int)fb.w, (int)fb.h, giscale)) {
            rt.broken = true;
            gfx_rt_enabled = 0;
            return;
        }
    }

    const int fbw = (int)fb.w, fbh = (int)fb.h;
    if (vw <= 0 || vh <= 0) {
        vx = 0; vy = 0; vw = fbw; vh = fbh;
    }
    // clamp the (GL bottom-left) rect into the framebuffer
    if (vx < 0) { vw += vx; vx = 0; }
    if (vy < 0) { vh += vy; vy = 0; }
    if (vx + vw > fbw) vw = fbw - vx;
    if (vy + vh > fbh) vh = fbh - vy;
    if (vw <= 0 || vh <= 0) {
        return;
    }
    const int vy_tl = fbh - vy - vh; // -> top-left, the backend's uniform orientation

    // capture scene colour (same size + format, exact copy)
    {
        SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(gpu.render_cb);
        SDL_GPUTextureLocation csrc, cdst;
        SDL_zero(csrc);
        SDL_zero(cdst);
        csrc.texture = fb.color;
        cdst.texture = rt.scene_col;
        SDL_CopyGPUTextureToTexture(cp, &csrc, &cdst, (Uint32)fbw, (Uint32)fbh, 1, false);
        SDL_EndGPUCopyPass(cp);
    }

    rt.frame++;
    const int pl = cam->playernum;
    const int q = (gfx_rt_quality < 0) ? 0 : (gfx_rt_quality > 2 ? 2 : gfx_rt_quality);

    RtGpuUni uni;
    memset(&uni, 0, sizeof(uni));
    uni.rect[0] = (float)vx / fbw;
    uni.rect[1] = (float)vy_tl / fbh;
    uni.rect[2] = (float)(vx + vw) / fbw;
    uni.rect[3] = (float)(vy_tl + vh) / fbh;
    const float thfy = tanf(cam->fovy * (float)(3.14159265358979 / 180.0) * 0.5f);
    uni.proj[0] = thfy * cam->aspect;
    uni.proj[1] = thfy;
    uni.proj[2] = cam->znear > 0.1f ? cam->znear : 0.1f;
    uni.proj[3] = cam->zfar > uni.proj[2] + 1.0f ? cam->zfar : uni.proj[2] + 1.0f;
    uni.ysign = gpu.invert_y ? 1.0f : -1.0f; // top-down storage; --gpu-invert-y flips
    uni.texel[0] = 1.0f / fbw;
    uni.texel[1] = 1.0f / fbh;
    uni.frame = (int32_t)(rt.frame & 0xffff);
    uni.ao_radius = gfx_rt_ao_radius;
    uni.shadow_len = gfx_rt_shadow_length;
    uni.gi_radius = gfx_rt_ao_radius * 20.0f;
    uni.max_dist = uni.proj[3] * 0.35f;
    uni.ao_int = gfx_rt_ao_intensity;
    uni.sh_int = gfx_rt_shadow_intensity;
    uni.gi_int = gfx_rt_gi_intensity;
    uni.ssr_int = gfx_rt_ssr_intensity;
    uni.ao_on = (ao_on || dbg == RT_DEBUG_AO) ? 1 : 0;
    uni.shadow_on = (sh_on || dbg == RT_DEBUG_SHADOW) ? 1 : 0;
    uni.gi_on = gi_mode != RT_GI_OFF ? 1 : 0;
    uni.ssr_on = ssr_on ? 1 : 0;
    uni.mode = dbg;
    uni.ao_samples = kQuality[q].ao_samples;
    uni.shadow_steps = kQuality[q].shadow_steps;
    uni.ssr_steps = kQuality[q].ssr_steps;
    uni.sky[0] = gfx_rt_sky[0];
    uni.sky[1] = gfx_rt_sky[1];
    uni.sky[2] = gfx_rt_sky[2];
    uni.light_count = nlights;
    uni.light_shadows = gfx_rt_light_shadows;
    uni.light_steps = kQuality[q].light_steps;
    uni.torch = gfx_rt_torch;
    uni.torch_int = gfx_rt_torch_intensity;
    uni.torch_range = gfx_rt_torch_range;
    uni.dark = dark_on ? 1 : 0;
    uni.dark_ambient = gfx_rt_dark_ambient;
    uni.lights_on = lights_run ? 1 : 0;

    // sun: world -> view (rotation only), normalized
    {
        float sw[3] = { gfx_rt_sun_dir[0], gfx_rt_sun_dir[1], gfx_rt_sun_dir[2] };
        float sl = sqrtf(sw[0] * sw[0] + sw[1] * sw[1] + sw[2] * sw[2]);
        if (sl < 0.0001f) { sw[0] = 0.0f; sw[1] = 1.0f; sw[2] = 0.0f; sl = 1.0f; }
        sw[0] /= sl; sw[1] /= sl; sw[2] /= sl;
        const float *m = cam->viewmtx;
        uni.sun[0] = m[0] * sw[0] + m[4] * sw[1] + m[8] * sw[2];
        uni.sun[1] = m[1] * sw[0] + m[5] * sw[1] + m[9] * sw[2];
        uni.sun[2] = m[2] * sw[0] + m[6] * sw[1] + m[10] * sw[2];
    }

    // the VS rect push persists on the command buffer for all RT passes
    SDL_PushGPUVertexUniformData(gpu.render_cb, 0, uni.rect, sizeof(uni.rect));

    const uint32_t NEAREST = 0;           // all-nearest, clamp
    const uint32_t LINEAR = SK_FB_LINEAR; // linear, clamp

    // prepass: depth -> normals + linear depth
    {
        SDL_GPUTexture *t[1] = { fb.depth };
        const uint32_t k[1] = { NEAREST };
        rt_run_pass(rt.pipe[RTP_PREPASS], rt.norm_tex, vx, vy_tl, vw, vh, t, k, 1, &uni);
    }

    // AO + shadow trace, then separable bilateral blur
    if (uni.ao_on || uni.shadow_on) {
        {
            SDL_GPUTexture *t[1] = { rt.norm_tex };
            const uint32_t k[1] = { NEAREST };
            rt_run_pass(rt.pipe[RTP_AOSHADOW], rt.ao_tex, vx, vy_tl, vw, vh, t, k, 1, &uni);
        }
        uni.dir[0] = 1.0f / fbw;
        uni.dir[1] = 0.0f;
        {
            SDL_GPUTexture *t[2] = { rt.norm_tex, rt.ao_tex };
            const uint32_t k[2] = { NEAREST, LINEAR };
            rt_run_pass(rt.pipe[RTP_BLUR], rt.aotmp_tex, vx, vy_tl, vw, vh, t, k, 2, &uni);
        }
        uni.dir[0] = 0.0f;
        uni.dir[1] = 1.0f / fbh;
        {
            SDL_GPUTexture *t[2] = { rt.norm_tex, rt.aotmp_tex };
            const uint32_t k[2] = { NEAREST, LINEAR };
            rt_run_pass(rt.pipe[RTP_BLUR], rt.ao_tex, vx, vy_tl, vw, vh, t, k, 2, &uni);
        }
    }

    // GI / path trace at reduced res + temporal accumulation + blur
    const bool gi_run = gi_mode != RT_GI_OFF || dbg == RT_DEBUG_GI;
    if (gi_run && rt_ensure_history(pl)) {
        const int gvx = (int)(vx * rt.giscale);
        const int gvy_tl = (int)(vy_tl * rt.giscale);
        int gvw = (int)(vw * rt.giscale);
        int gvh = (int)(vh * rt.giscale);
        if (gvw < 1) gvw = 1;
        if (gvh < 1) gvh = 1;

        const int mode = (gi_mode == RT_GI_OFF) ? RT_GI_SSGI : gi_mode;
        uni.rays = (mode == RT_GI_PATHTRACE) ? kQuality[q].pt_rays : kQuality[q].gi_rays;
        uni.bounces = (mode == RT_GI_PATHTRACE) ? kQuality[q].pt_bounces : 1;
        uni.steps = kQuality[q].gi_steps;
        uni.blend = (mode == RT_GI_PATHTRACE) ? 0.93f : 0.85f;

        {
            SDL_GPUTexture *t[2] = { rt.norm_tex, rt.scene_col };
            const uint32_t k[2] = { NEAREST, LINEAR };
            rt_run_pass(rt.pipe[RTP_TRACE], rt.gitrace_tex, gvx, gvy_tl, gvw, gvh, t, k, 2, &uni);
        }

        // temporal: cur + history[read] -> history[write]
        if (rt.prev_valid[pl]) {
            float inv_cur[16], vprev_invcur[16], pprev[16];
            mtxRigidInverse(inv_cur, cam->viewmtx);
            mtxMul(vprev_invcur, rt.prev_view[pl], inv_cur);
            mtxPerspective(pprev, rt.prev_fovy[pl], rt.prev_aspect[pl], rt.prev_znear[pl], rt.prev_zfar[pl]);
            mtxMul(uni.cur_to_prev, pprev, vprev_invcur);
        } else {
            memset(uni.cur_to_prev, 0, sizeof(uni.cur_to_prev)); // w always 0 -> history rejected
        }

        const int hread = rt.hist_idx[pl];
        const int hwrite = 1 - hread;
        {
            SDL_GPUTexture *t[3] = { rt.norm_tex, rt.gitrace_tex, rt.hist_tex[pl][hread] };
            const uint32_t k[3] = { NEAREST, LINEAR, LINEAR };
            rt_run_pass(rt.pipe[RTP_TEMPORAL], rt.hist_tex[pl][hwrite], gvx, gvy_tl, gvw, gvh, t, k, 3, &uni);
        }
        rt.hist_idx[pl] = hwrite;

        // blur H/V into gifinal (history itself stays sharp for reprojection)
        uni.dir[0] = 1.0f / rt.giw;
        uni.dir[1] = 0.0f;
        {
            SDL_GPUTexture *t[2] = { rt.norm_tex, rt.hist_tex[pl][hwrite] };
            const uint32_t k[2] = { NEAREST, LINEAR };
            rt_run_pass(rt.pipe_blur16, rt.gitmp_tex, gvx, gvy_tl, gvw, gvh, t, k, 2, &uni);
        }
        uni.dir[0] = 0.0f;
        uni.dir[1] = 1.0f / rt.gih;
        {
            SDL_GPUTexture *t[2] = { rt.norm_tex, rt.gitmp_tex };
            const uint32_t k[2] = { NEAREST, LINEAR };
            rt_run_pass(rt.pipe_blur16, rt.gifinal_tex, gvx, gvy_tl, gvw, gvh, t, k, 2, &uni);
        }
    }

    // save this frame's camera for next frame's reprojection
    memcpy(rt.prev_view[pl], cam->viewmtx, sizeof(rt.prev_view[pl]));
    rt.prev_fovy[pl] = cam->fovy;
    rt.prev_aspect[pl] = cam->aspect;
    rt.prev_znear[pl] = uni.proj[2];
    rt.prev_zfar[pl] = uni.proj[3];
    rt.prev_valid[pl] = true;

    // SSR
    if (ssr_on || dbg == RT_DEBUG_SSR) {
        SDL_GPUTexture *t[2] = { rt.norm_tex, rt.scene_col };
        const uint32_t k[2] = { NEAREST, LINEAR };
        rt_run_pass(rt.pipe[RTP_SSR], rt.ssr_tex, vx, vy_tl, vw, vh, t, k, 2, &uni);
    }

    // dynamic lights + torch (dark/relight mode): map lights transformed
    // world -> view on the CPU, colours premultiplied; pushed as the light
    // pass's second uniform block (fragment slot 1)
    if (lights_run) {
        static RtGpuLights lbuf; // 1.5 KB; static keeps it off the stack
        const float *m = cam->viewmtx;
        for (int i = 0; i < nlights; i++) {
            const rtlight *l = &cam->lights[i];
            lbuf.posrad[i][0] = m[0] * l->pos[0] + m[4] * l->pos[1] + m[8] * l->pos[2] + m[12];
            lbuf.posrad[i][1] = m[1] * l->pos[0] + m[5] * l->pos[1] + m[9] * l->pos[2] + m[13];
            lbuf.posrad[i][2] = m[2] * l->pos[0] + m[6] * l->pos[1] + m[10] * l->pos[2] + m[14];
            lbuf.posrad[i][3] = l->radius > 1.0f ? l->radius : 1.0f;
            const float gain = l->intensity * gfx_rt_light_intensity;
            lbuf.col[i][0] = l->color[0] * gain;
            lbuf.col[i][1] = l->color[1] * gain;
            lbuf.col[i][2] = l->color[2] * gain;
            lbuf.col[i][3] = 0.0f;
        }
        SDL_PushGPUFragmentUniformData(gpu.render_cb, 1, &lbuf, sizeof(lbuf));

        SDL_GPUTexture *t[1] = { rt.norm_tex };
        const uint32_t k[1] = { NEAREST };
        rt_run_pass(rt.pipe[RTP_LIGHT], rt.light_tex, vx, vy_tl, vw, vh, t, k, 1, &uni);
    }

    // composite back over the framebuffer colour (no depth attached)
    if (dbg != 0) {
        SDL_GPUTexture *t[5] = { rt.norm_tex, rt.ao_tex, rt.gifinal_tex, rt.ssr_tex, rt.light_tex };
        const uint32_t k[5] = { NEAREST, LINEAR, LINEAR, LINEAR, LINEAR };
        rt_run_pass(rt.pipe[RTP_DEBUG], fb.color, vx, vy_tl, vw, vh, t, k, 5, &uni);
    } else {
        if (ao_on || sh_on || dark_on) {
            SDL_GPUTexture *t[1] = { rt.ao_tex };
            const uint32_t k[1] = { LINEAR };
            // uAOOn/uShadowOn carry the real toggles here (no dbg override)
            uni.ao_on = ao_on ? 1 : 0;
            uni.shadow_on = sh_on ? 1 : 0;
            rt_run_pass(rt.pipe[RTP_COMP_MUL], fb.color, vx, vy_tl, vw, vh, t, k, 1, &uni);
        }
        if (gi_mode != RT_GI_OFF || ssr_on || lights_run) {
            SDL_GPUTexture *t[4] = { rt.scene_col, rt.gifinal_tex, rt.ssr_tex, rt.light_tex };
            const uint32_t k[4] = { LINEAR, LINEAR, LINEAR, LINEAR };
            rt_run_pass(rt.pipe[RTP_COMP_ADD], fb.color, vx, vy_tl, vw, vh, t, k, 4, &uni);
        }
    }

    // hand the frame back to the immediate path: our passes are closed and our
    // uniform pushes clobbered the command buffer's vertex/fragment slots
    st.pass = NULL;
    st.bound_pipeline = NULL;
    st.vs_dirty = true;
    st.fs_dirty = true;
}

struct GfxRenderingAPI gfx_sdlgpu_api = {
    gfx_sdlgpu_get_name,
    gfx_sdlgpu_get_max_texture_size,
    gfx_sdlgpu_get_clip_parameters,
    gfx_sdlgpu_unload_shader,
    gfx_sdlgpu_load_shader,
    gfx_sdlgpu_create_and_load_new_shader,
    gfx_sdlgpu_lookup_shader,
    gfx_sdlgpu_shader_get_info,
    gfx_sdlgpu_clear_shaders,
    gfx_sdlgpu_new_texture,
    gfx_sdlgpu_select_texture,
    gfx_sdlgpu_upload_texture,
    gfx_sdlgpu_set_sampler_parameters,
    gfx_sdlgpu_set_depth_mode,
    gfx_sdlgpu_set_depth_range,
    gfx_sdlgpu_set_viewport,
    gfx_sdlgpu_set_scissor,
    gfx_sdlgpu_set_use_alpha,
    gfx_sdlgpu_draw_triangles,
    gfx_sdlgpu_init,
    gfx_sdlgpu_on_resize,
    gfx_sdlgpu_start_frame,
    gfx_sdlgpu_end_frame,
    gfx_sdlgpu_finish_render,
    gfx_sdlgpu_create_framebuffer,
    gfx_sdlgpu_update_framebuffer_parameters,
    gfx_sdlgpu_start_draw_to_framebuffer,
    gfx_sdlgpu_copy_framebuffer,
    gfx_sdlgpu_clear_framebuffer,
    gfx_sdlgpu_resolve_msaa_color_buffer,
    gfx_sdlgpu_get_framebuffer_texture_id,
    gfx_sdlgpu_select_texture_fb,
    gfx_sdlgpu_delete_texture,
    gfx_sdlgpu_set_texture_filter,
    gfx_sdlgpu_get_texture_filter,
    gfx_sdlgpu_set_mipmap_filter,
    gfx_sdlgpu_set_anisotropy_level,
    gfx_sdlgpu_get_max_anisotropy_level,
    gfx_sdlgpu_set_mvp,
    gfx_sdlgpu_cache_create_buffer,
    gfx_sdlgpu_cache_delete_buffer,
    gfx_sdlgpu_cache_replay_begin,
    gfx_sdlgpu_cache_draw,
    gfx_sdlgpu_cache_set_cull,
    gfx_sdlgpu_cache_replay_end,
    gfx_sdlgpu_set_fog_params,
    gfx_sdlgpu_cache_create_palette,
    gfx_sdlgpu_cache_delete_palette,
    gfx_sdlgpu_cache_upload_palette,
    gfx_sdlgpu_cache_bind_palette,
    gfx_sdlgpu_set_palette_enable,
    gfx_sdlgpu_set_shade_routing,
    gfx_sdlgpu_rt_resolve, // screen-space raytracing suite (docs/PORT_RAYTRACING.md)
};

#endif // USE_SDLGPU
