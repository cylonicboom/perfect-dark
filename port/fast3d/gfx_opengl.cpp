#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <map>
#include <unordered_map>
#include <vector>

#include <SDL3/SDL.h>

#ifndef _LANGUAGE_C
#define _LANGUAGE_C
#endif
#include <PR/gbi.h>

#include "glad/glad.h"

#include "gfx_cc.h"
#include "gfx_rendering_api.h"
#include "gfx_pc.h"
#include "gfx_rt.h"
#include "gfx_retro.h"

using namespace std;

struct ShaderProgram {
    GLuint opengl_program_id;
    uint8_t num_inputs;
    bool used_textures[SHADER_MAX_TEXTURES];
    uint8_t num_floats;
    GLint attrib_locations[16];
    uint8_t attrib_sizes[16];
    uint8_t num_attribs;
    GLint frame_count_location;
    GLint noise_scale_location;
    GLint three_point_filter_locations[2];
    GLint wireframe_color_location;
    GLint wireframe_thickness_location;
    GLint mvp_location;
    GLint use_vertex_fog_location;
    GLint fog_mul_location;
    GLint fog_off_location;
    GLint shade_idx_location;       // aShadeIdx vertex attribute (cached palette lookup)
    GLint palette_enable_location;  // uPaletteEnable
    GLint palette_w_location;       // uPaletteW (palette texture width)
    GLint shade_route_location;     // uShadeRoute (3 bits/input)

    // Per-program uniform cache (round-2 opt): last-uploaded values plus the
    // global generation this program last synced against. 0 = never synced;
    // the value fields are only meaningful once uniform_generation != 0 has
    // been through gfx_opengl_set_uniforms (zero-init matches GL's post-link
    // uniform default of 0, so the first sync is still correct).
    uint64_t uniform_generation;
    uint32_t cached_frame_count;
    float cached_noise_scale;
    int cached_three_point_filter[2];
    float cached_mvp[16];
    int cached_use_vertex_fog;
    float cached_fog_mul;
    float cached_fog_off;
    int cached_palette_enable;
    float cached_palette_w;
    int cached_shade_route;
};

#define GFX_PALETTE_TEX_UNIT 2 // uTex0=0, uTex1=1, palette=2

struct Framebuffer {
    uint32_t width, height;
    bool has_depth_buffer;
    uint32_t msaa_level;
    bool invert_y;

    GLuint fbo, clrbuf, clrbuf_msaa, rbo;
};

static std::map<pair<uint64_t, uint32_t>, struct ShaderProgram> shader_program_pool;
static GLuint opengl_vbo;
static GLuint opengl_vao;
static bool current_depth_mask;

static uint32_t frame_count;

static std::vector<Framebuffer> framebuffers;
static size_t current_framebuffer;
static float current_noise_scale;
static int current_anisotropy_level;
static FilteringMode current_filter_mode = FILTER_LINEAR;
static MipmapFilteringMode current_mipmap_filter_mode = MIPMAP_LINEAR;
static bool current_textures_linear_filter[2] = {false, false};

static int gl_glsl_version = 130;
// Defined near the API table; used by the draw path above it.
static bool gfx_opengl_shader_wireframe_supported(void);
static char gl_glsl_version_str[16] = "130";
static GLenum gl_mirror_clamp = GL_MIRROR_CLAMP_TO_EDGE;
static bool gl_es = false;
static bool gl_core_profile = false;

// Tracks whether the most recently set depth mode has depth testing enabled.
// Used to restrict wireframe (CHEAT_WIREFRAME) to 3D geometry: depth-tested
// draws (world, props, viewmodel) become outlines while 2D HUD/menus (no
// depth test) stay solid.
static bool s_wireframe_depth_test = false;

// Currently-bound shader program, tracked so draw_triangles can set the
// per-draw wireframe wire-colour uniform on it.
static struct ShaderProgram *gfx_current_shader_program = NULL;

// Model-view-projection matrix uploaded to every shader's uMVP uniform
// (column-major). Defaults to identity so the normal immediate-mode path —
// which feeds pre-transformed clip-space positions into aVtxPos — is
// unaffected (identity * clip == clip). The display-list cache sets this to a
// folded room matrix while replaying object-space geometry, then restores it.
static float gfx_current_mvp[16] = {
    1.f, 0.f, 0.f, 0.f,
    0.f, 1.f, 0.f, 0.f,
    0.f, 0.f, 1.f, 0.f,
    0.f, 0.f, 0.f, 1.f,
};

// Fog source for the uFogMul/uFogOff/uUseVertexFog shader uniforms. Default = use
// the per-vertex baked factor (the immediate-mode path). The display-list cache
// flips uUseVertexFog to 0 + supplies fog_mul/off for cached distance fog, then
// restores these defaults. Uploaded by gfx_opengl_set_uniforms on every shader
// load (GL uniforms default to 0, which would otherwise disable immediate fog).
static int gfx_current_use_vertex_fog = 1;
static float gfx_current_fog_mul = 0.f;
static float gfx_current_fog_off = 0.f;

// Display-list cache shader-side palette state (per-draw uniforms, mvp/fog pattern).
// Default: disabled, so the immediate path resolves shade from the baked combiner
// inputs exactly as before. Cached replay flips uPaletteEnable on + supplies the
// palette width and per-combiner shade routing.
static int gfx_current_palette_enable = 0;
static float gfx_current_palette_w = 1.f;
static int gfx_current_shade_routing = 0;

// --- Round-2 GL-call-reduction state (docs/PORT_OPT_ROUND2_CANDIDATES.md A3/A6 + Tier-A3) ---

// Global uniform generation: bumped by every site that changes any global read
// by gfx_opengl_set_uniforms (mvp, fog, palette, shade routing, frame count,
// noise scale, per-tile linear-filter flags). Invariant: a program whose
// uniform_generation equals this has GL uniform values equal to the current
// globals, so its sync can be skipped entirely.
static uint64_t gfx_uniform_generation = 1;

// Redundant-state shadows. Sentinel (-1 / UINT32_MAX) = unknown, must
// re-issue. Reset in start_frame (external GL state can change between
// frames), around the rt/retro passes, and at every site in this file that
// touches the corresponding GL state outside the shadowed setters.
#define GFX_NUM_TEX_UNITS 3 // uTex0=0, uTex1=1, palette=2 (GFX_PALETTE_TEX_UNIT)
static int s_active_texture_unit = -1;              // GL_TEXTURE0-relative
static uint32_t s_bound_texture[GFX_NUM_TEX_UNITS] = { 0xffffffffu, 0xffffffffu, 0xffffffffu };
static int s_blend_enabled = -1;                    // glEnable(GL_BLEND)
static int s_blend_modulate = -1;                   // 1 = DST_COLOR/ZERO, 0 = SRC_ALPHA/ONE_MINUS_SRC_ALPHA
static int s_polygon_offset_fill = -1;              // 1 = enabled at (-2,-2), 0 = disabled at (0,0)

static void gfx_opengl_invalidate_state_shadows(void) {
    s_active_texture_unit = -1;
    for (int i = 0; i < GFX_NUM_TEX_UNITS; i++) {
        s_bound_texture[i] = 0xffffffffu;
    }
    s_blend_enabled = -1;
    s_blend_modulate = -1;
    s_polygon_offset_fill = -1;
}

// --- Persistent-mapped vertex ring (round-2 A4, docs/PORT_OPT_ROUND2_CANDIDATES.md) ---
// With GL_ARB_buffer_storage (or GL 4.4+), opengl_vbo is created as ONE
// immutable buffer of GFX_VTX_RING_REGIONS fenced regions, mapped
// persistent+coherent once at init, replacing the per-flush variable-size
// glBufferData(GL_STREAM_DRAW) orphan. Each flush memcpys into the write
// cursor and draws with a non-zero `first` vertex; attrib pointers stay baked
// at the current REGION base, so they are re-specified only on shader change
// (exactly as today, via vertex_array_set_attribs) plus once per region
// rotation — no per-flush attrib respec, no per-flush buffer rebind, and the
// single-VAO/attrib state from A3 is untouched. The cursor is aligned to a
// whole triangle (3 * stride) before each flush so `first` stays a multiple
// of 3 and the barycentric-wireframe gl_VertexID % 3 corner derivation keeps
// working. One GLsync per region: created when rotating OFF a region (i.e.
// after every draw that sourced from it), waited on before that region is
// written again; a wait timeout/failure degrades to glFinish and logs once.
// Fallback: if buffer_storage is unavailable or any init step fails,
// opengl_vbo stays a plain mutable buffer and draw_triangles keeps the
// original glBufferData path unchanged.
#define GFX_VTX_RING_REGIONS 3
// Must cover gfx_pc's worst-case flush: MAX_BUFFERED (4096) tris * 3 verts *
// 32 floats * 4 bytes = 1.5 MiB; 2 MiB gives headroom. Re-checked per flush —
// an oversize flush (i.e. gfx_pc's buffer grew) disables the ring for good
// instead of corrupting.
#define GFX_VTX_RING_REGION_SIZE (2u * 1024u * 1024u)
#define GFX_VTX_RING_FENCE_TIMEOUT_NS 1000000000ull // 1 s, generous

// The bundled glad is not generated with ARB_buffer_storage, so the enums and
// entry point are declared here and the function is probed via SDL.
#ifndef GL_MAP_PERSISTENT_BIT
#define GL_MAP_PERSISTENT_BIT 0x0040
#endif
#ifndef GL_MAP_COHERENT_BIT
#define GL_MAP_COHERENT_BIT 0x0080
#endif
typedef void (APIENTRY *PFNGLBUFFERSTORAGEPDPROC)(GLenum target, GLsizeiptr size, const void* data,
                                                  GLbitfield flags);

static bool s_vtx_ring_active = false;
static uint8_t* s_vtx_ring_map = NULL;    // persistent mapping base
static int s_vtx_ring_region = 0;         // region currently being written
static size_t s_vtx_ring_cursor = 0;      // write offset, bytes, buffer-relative
static size_t s_vtx_ring_attrib_base = 0; // region base baked into the attrib pointers (0 = fallback)
static GLsync s_vtx_ring_fence[GFX_VTX_RING_REGIONS];
static bool s_vtx_ring_wait_warned = false;

static int gfx_opengl_get_max_texture_size() {
    GLint max_texture_size;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_texture_size);
    return max_texture_size;
}

static const char* gfx_opengl_get_name() {
    return "OpenGL";
}

static struct GfxClipParameters gfx_opengl_get_clip_parameters(void) {
    return { false, framebuffers[current_framebuffer].invert_y };
}

static void gfx_opengl_vertex_array_set_attribs(struct ShaderProgram* prg) {
    size_t num_floats = prg->num_floats;
    size_t pos = 0;

    for (int i = 0; i < prg->num_attribs; i++) {
        if (prg->attrib_locations[i] >= 0) {
            glEnableVertexAttribArray(prg->attrib_locations[i]);
            // s_vtx_ring_attrib_base rebases the stream at the vertex ring's
            // current region; 0 on the glBufferData fallback path.
            glVertexAttribPointer(prg->attrib_locations[i], prg->attrib_sizes[i], GL_FLOAT, GL_FALSE,
                                num_floats * sizeof(float),
                                (void*)(s_vtx_ring_attrib_base + pos * sizeof(float)));
        }
        pos += prg->attrib_sizes[i];
    }
}

// Advance the vertex ring to the next region: fence the region being left
// (created after all draws that sourced from it), then wait out the fence
// guarding the region being entered before it is overwritten.
static void gfx_opengl_vtx_ring_rotate(void) {
    if (s_vtx_ring_fence[s_vtx_ring_region] != NULL) {
        glDeleteSync(s_vtx_ring_fence[s_vtx_ring_region]); // defensive; cleared on entry below
    }
    s_vtx_ring_fence[s_vtx_ring_region] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);

    s_vtx_ring_region = (s_vtx_ring_region + 1) % GFX_VTX_RING_REGIONS;

    if (s_vtx_ring_fence[s_vtx_ring_region] != NULL) {
        const GLenum r = glClientWaitSync(s_vtx_ring_fence[s_vtx_ring_region],
                                          GL_SYNC_FLUSH_COMMANDS_BIT, GFX_VTX_RING_FENCE_TIMEOUT_NS);
        if (r != GL_ALREADY_SIGNALED && r != GL_CONDITION_SATISFIED) {
            if (!s_vtx_ring_wait_warned) {
                sysLogPrintf(LOG_WARNING, "GL: vtx ring fence wait failed (0x%x), using glFinish", r);
                s_vtx_ring_wait_warned = true;
            }
            glFinish();
        }
        glDeleteSync(s_vtx_ring_fence[s_vtx_ring_region]);
        s_vtx_ring_fence[s_vtx_ring_region] = NULL;
    }

    s_vtx_ring_cursor = (size_t)s_vtx_ring_region * GFX_VTX_RING_REGION_SIZE;
    // Attrib pointers bake byte offsets: rebase the currently-loaded program at
    // the new region (later programs rebase on their load_shader as usual).
    s_vtx_ring_attrib_base = s_vtx_ring_cursor;
    if (gfx_current_shader_program != NULL) {
        gfx_opengl_vertex_array_set_attribs(gfx_current_shader_program);
    }
}

// Permanently abandon the ring (should-not-happen guard: a flush bigger than a
// region). The ring buffer's storage is immutable, so glBufferData can't reuse
// it — swap in a fresh mutable buffer and return to the original stream path.
static void gfx_opengl_vtx_ring_disable(const char* why) {
    sysLogPrintf(LOG_WARNING, "GL: disabling vtx ring: %s", why);
    for (int i = 0; i < GFX_VTX_RING_REGIONS; i++) {
        if (s_vtx_ring_fence[i] != NULL) {
            glDeleteSync(s_vtx_ring_fence[i]);
            s_vtx_ring_fence[i] = NULL;
        }
    }
    GLuint newbuf = 0;
    glGenBuffers(1, &newbuf);
    glBindBuffer(GL_ARRAY_BUFFER, newbuf);
    glDeleteBuffers(1, &opengl_vbo); // also releases the persistent mapping
    opengl_vbo = newbuf;
    s_vtx_ring_active = false;
    s_vtx_ring_map = NULL;
    s_vtx_ring_region = 0;
    s_vtx_ring_cursor = 0;
    s_vtx_ring_attrib_base = 0;
    if (gfx_current_shader_program != NULL) {
        gfx_opengl_vertex_array_set_attribs(gfx_current_shader_program);
    }
}

// Try to promote the freshly-created (and bound) opengl_vbo into the
// persistent-mapped ring. On any failure the buffer is left (or restored to)
// plain mutable storage and the glBufferData path runs unchanged.
static void gfx_opengl_vtx_ring_init(void) {
    if (gl_es) {
        return; // buffer_storage is an EXT on ES; keep this desktop-only
    }
    const bool have_bs = (GLVersion.major > 4 || (GLVersion.major == 4 && GLVersion.minor >= 4))
            || SDL_GL_ExtensionSupported("GL_ARB_buffer_storage");
    // glad is generated without ARB_buffer_storage, so probe the entry point
    // directly; the sync/map functions are GL 3.2/3.0 core but may be NULL on
    // ancient contexts, so check them too.
    PFNGLBUFFERSTORAGEPDPROC pglBufferStorage =
            (PFNGLBUFFERSTORAGEPDPROC)SDL_GL_GetProcAddress("glBufferStorage");
    if (!have_bs || pglBufferStorage == NULL || glad_glFenceSync == NULL || glad_glClientWaitSync == NULL
            || glad_glDeleteSync == NULL || glad_glMapBufferRange == NULL) {
        sysLogPrintf(LOG_NOTE, "GL: ARB_buffer_storage: no (using glBufferData vertex streaming)");
        return;
    }

    while (glGetError() != GL_NO_ERROR) { } // clear stale errors so the checks below are ours

    const GLsizeiptr total = (GLsizeiptr)GFX_VTX_RING_REGIONS * GFX_VTX_RING_REGION_SIZE;
    const GLbitfield flags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
    pglBufferStorage(GL_ARRAY_BUFFER, total, NULL, flags);
    if (glGetError() != GL_NO_ERROR) {
        // Storage was refused, so the buffer is still storage-less and mutable;
        // the glBufferData path will define it as before.
        sysLogPrintf(LOG_WARNING, "GL: glBufferStorage failed, using glBufferData vertex streaming");
        return;
    }

    void* map = glMapBufferRange(GL_ARRAY_BUFFER, 0, total, flags);
    if (map == NULL || glGetError() != GL_NO_ERROR) {
        // The buffer now has immutable storage but no mapping — it can never
        // take glBufferData, so it must be replaced with a mutable one.
        sysLogPrintf(LOG_WARNING, "GL: persistent map failed, using glBufferData vertex streaming");
        GLuint newbuf = 0;
        glGenBuffers(1, &newbuf);
        glBindBuffer(GL_ARRAY_BUFFER, newbuf);
        glDeleteBuffers(1, &opengl_vbo);
        opengl_vbo = newbuf;
        return;
    }

    s_vtx_ring_map = (uint8_t*)map;
    s_vtx_ring_region = 0;
    s_vtx_ring_cursor = 0;
    s_vtx_ring_attrib_base = 0;
    s_vtx_ring_active = true;
    sysLogPrintf(LOG_NOTE, "GL: persistent-mapped vertex ring: %d x %u KiB",
                 GFX_VTX_RING_REGIONS, (unsigned)(GFX_VTX_RING_REGION_SIZE / 1024u));
}

// Syncs prg's uniforms to the current globals. prg MUST be the currently bound
// program (glUseProgram). Skips entirely when the program already synced at the
// current generation; otherwise uploads only values that actually changed.
static void gfx_opengl_set_uniforms(struct ShaderProgram* prg) {
    if (prg->uniform_generation == gfx_uniform_generation) {
        return;
    }
    if (prg->frame_count_location >= 0 && prg->cached_frame_count != frame_count) {
        glUniform1i(prg->frame_count_location, frame_count);
        prg->cached_frame_count = frame_count;
    }
    if (prg->noise_scale_location >= 0 && prg->cached_noise_scale != current_noise_scale) {
        glUniform1f(prg->noise_scale_location, current_noise_scale);
        prg->cached_noise_scale = current_noise_scale;
    }
    for (int i = 0; i < 2; i++) {
        const int tpf = current_textures_linear_filter[i] ? 1 : 0;
        if (prg->three_point_filter_locations[i] >= 0 && prg->cached_three_point_filter[i] != tpf) {
            glUniform1i(prg->three_point_filter_locations[i], tpf);
            prg->cached_three_point_filter[i] = tpf;
        }
    }
    if (prg->mvp_location >= 0 && memcmp(prg->cached_mvp, gfx_current_mvp, sizeof(gfx_current_mvp)) != 0) {
        glUniformMatrix4fv(prg->mvp_location, 1, GL_FALSE, gfx_current_mvp);
        memcpy(prg->cached_mvp, gfx_current_mvp, sizeof(gfx_current_mvp));
    }
    if (prg->use_vertex_fog_location >= 0 && prg->cached_use_vertex_fog != gfx_current_use_vertex_fog) {
        glUniform1i(prg->use_vertex_fog_location, gfx_current_use_vertex_fog);
        prg->cached_use_vertex_fog = gfx_current_use_vertex_fog;
    }
    if (prg->fog_mul_location >= 0 && prg->cached_fog_mul != gfx_current_fog_mul) {
        glUniform1f(prg->fog_mul_location, gfx_current_fog_mul);
        prg->cached_fog_mul = gfx_current_fog_mul;
    }
    if (prg->fog_off_location >= 0 && prg->cached_fog_off != gfx_current_fog_off) {
        glUniform1f(prg->fog_off_location, gfx_current_fog_off);
        prg->cached_fog_off = gfx_current_fog_off;
    }
    if (prg->palette_enable_location >= 0 && prg->cached_palette_enable != gfx_current_palette_enable) {
        glUniform1i(prg->palette_enable_location, gfx_current_palette_enable);
        prg->cached_palette_enable = gfx_current_palette_enable;
    }
    if (prg->palette_w_location >= 0 && prg->cached_palette_w != gfx_current_palette_w) {
        glUniform1f(prg->palette_w_location, gfx_current_palette_w);
        prg->cached_palette_w = gfx_current_palette_w;
    }
    if (prg->shade_route_location >= 0 && prg->cached_shade_route != gfx_current_shade_routing) {
        glUniform1i(prg->shade_route_location, gfx_current_shade_routing);
        prg->cached_shade_route = gfx_current_shade_routing;
    }
    prg->uniform_generation = gfx_uniform_generation;
}

static void gfx_opengl_set_mvp(const float m[16]) {
    for (int i = 0; i < 16; i++) {
        gfx_current_mvp[i] = m[i];
    }
    gfx_uniform_generation++;
    // Apply immediately to the bound program so a set_mvp between draws (without
    // a reload) takes effect; other programs sync on their next bind.
    if (gfx_current_shader_program != NULL) {
        gfx_opengl_set_uniforms(gfx_current_shader_program);
    }
}

static void gfx_opengl_set_fog_params(int use_vertex_fog, float fog_mul, float fog_off) {
    gfx_current_use_vertex_fog = use_vertex_fog;
    gfx_current_fog_mul = fog_mul;
    gfx_current_fog_off = fog_off;
    gfx_uniform_generation++;
    if (gfx_current_shader_program != NULL) {
        gfx_opengl_set_uniforms(gfx_current_shader_program);
    }
}

static void gfx_opengl_set_palette_enable(int enable) {
    gfx_current_palette_enable = enable;
    gfx_uniform_generation++;
    if (gfx_current_shader_program != NULL) {
        gfx_opengl_set_uniforms(gfx_current_shader_program);
    }
}

static void gfx_opengl_set_shade_routing(int packed) {
    gfx_current_shade_routing = packed;
    gfx_uniform_generation++;
    if (gfx_current_shader_program != NULL) {
        gfx_opengl_set_uniforms(gfx_current_shader_program);
    }
}

static void gfx_opengl_unload_shader(struct ShaderProgram* old_prg) {
    if (old_prg != NULL) {
        for (int i = 0; i < old_prg->num_attribs; i++) {
            if (old_prg->attrib_locations[i] >= 0) {
                glDisableVertexAttribArray(old_prg->attrib_locations[i]);
            }
        }
        // aShadeIdx is enabled only by cache_draw; disabling here (a no-op if it was
        // never enabled) keeps it from lingering into immediate-mode draws.
        if (old_prg->shade_idx_location >= 0) {
            glDisableVertexAttribArray(old_prg->shade_idx_location);
        }
    }
}

static void gfx_opengl_load_shader(struct ShaderProgram* new_prg) {
    // if (!new_prg) return;
    gfx_current_shader_program = new_prg;
    glUseProgram(new_prg->opengl_program_id);
    gfx_opengl_vertex_array_set_attribs(new_prg);
    gfx_opengl_set_uniforms(new_prg);
}

static void append_str(char* buf, size_t* len, const char* str) {
    while (*str != '\0') {
        buf[(*len)++] = *str++;
    }
}

static void append_line(char* buf, size_t* len, const char* str) {
    while (*str != '\0') {
        buf[(*len)++] = *str++;
    }
    buf[(*len)++] = '\n';
}

#define RAND_NOISE "((random(vec3(floor(gl_FragCoord.xy * noise_scale), float(frame_count))) + 1.0) / 2.0)"

static const char* shader_item_to_str(uint32_t item, bool with_alpha, bool only_alpha, bool inputs_have_alpha,
                                      bool hint_single_element) {
    if (!only_alpha) {
        switch (item) {
            case SHADER_0:
                return with_alpha ? "vec4(0.0, 0.0, 0.0, 0.0)" : "vec3(0.0, 0.0, 0.0)";
            case SHADER_1:
                return with_alpha ? "vec4(1.0, 1.0, 1.0, 1.0)" : "vec3(1.0, 1.0, 1.0)";
            case SHADER_INPUT_1:
                return with_alpha || !inputs_have_alpha ? "vInput1" : "vInput1.rgb";
            case SHADER_INPUT_2:
                return with_alpha || !inputs_have_alpha ? "vInput2" : "vInput2.rgb";
            case SHADER_INPUT_3:
                return with_alpha || !inputs_have_alpha ? "vInput3" : "vInput3.rgb";
            case SHADER_INPUT_4:
                return with_alpha || !inputs_have_alpha ? "vInput4" : "vInput4.rgb";
            case SHADER_TEXEL0:
                return with_alpha ? "texVal0" : "texVal0.rgb";
            case SHADER_TEXEL0A:
                return hint_single_element ? "texVal0.a"
                                           : (with_alpha ? "vec4(texVal0.a, texVal0.a, texVal0.a, texVal0.a)"
                                                         : "vec3(texVal0.a, texVal0.a, texVal0.a)");
            case SHADER_TEXEL1A:
                return hint_single_element ? "texVal1.a"
                                           : (with_alpha ? "vec4(texVal1.a, texVal1.a, texVal1.a, texVal1.a)"
                                                         : "vec3(texVal1.a, texVal1.a, texVal1.a)");
            case SHADER_TEXEL1:
                return with_alpha ? "texVal1" : "texVal1.rgb";
            case SHADER_COMBINED:
                return with_alpha ? "texel" : "texel.rgb";
            case SHADER_NOISE:
                return with_alpha ? "vec4(" RAND_NOISE ", " RAND_NOISE ", " RAND_NOISE ", " RAND_NOISE ")"
                                  : "vec3(" RAND_NOISE ", " RAND_NOISE ", " RAND_NOISE ")";
        }
    } else {
        switch (item) {
            case SHADER_0:
                return "0.0";
            case SHADER_1:
                return "1.0";
            case SHADER_INPUT_1:
                return "vInput1.a";
            case SHADER_INPUT_2:
                return "vInput2.a";
            case SHADER_INPUT_3:
                return "vInput3.a";
            case SHADER_INPUT_4:
                return "vInput4.a";
            case SHADER_TEXEL0:
                return "texVal0.a";
            case SHADER_TEXEL0A:
                return "texVal0.a";
            case SHADER_TEXEL1A:
                return "texVal1.a";
            case SHADER_TEXEL1:
                return "texVal1.a";
            case SHADER_COMBINED:
                return "texel.a";
            case SHADER_NOISE:
                return RAND_NOISE;
        }
    }
    return "";
}

#undef RAND_NOISE

static void append_formula(char* buf, size_t* len, uint8_t c[2][4], bool do_single, bool do_multiply, bool do_mix,
                           bool with_alpha, bool only_alpha, bool opt_alpha) {
    if (do_single) {
        append_str(buf, len, shader_item_to_str(c[only_alpha][3], with_alpha, only_alpha, opt_alpha, false));
    } else if (do_multiply) {
        append_str(buf, len, shader_item_to_str(c[only_alpha][0], with_alpha, only_alpha, opt_alpha, false));
        append_str(buf, len, " * ");
        append_str(buf, len, shader_item_to_str(c[only_alpha][2], with_alpha, only_alpha, opt_alpha, true));
    } else if (do_mix) {
        append_str(buf, len, "mix(");
        append_str(buf, len, shader_item_to_str(c[only_alpha][1], with_alpha, only_alpha, opt_alpha, false));
        append_str(buf, len, ", ");
        append_str(buf, len, shader_item_to_str(c[only_alpha][0], with_alpha, only_alpha, opt_alpha, false));
        append_str(buf, len, ", ");
        append_str(buf, len, shader_item_to_str(c[only_alpha][2], with_alpha, only_alpha, opt_alpha, true));
        append_str(buf, len, ")");
    } else {
        append_str(buf, len, "(");
        append_str(buf, len, shader_item_to_str(c[only_alpha][0], with_alpha, only_alpha, opt_alpha, false));
        append_str(buf, len, " - ");
        append_str(buf, len, shader_item_to_str(c[only_alpha][1], with_alpha, only_alpha, opt_alpha, false));
        append_str(buf, len, ") * ");
        append_str(buf, len, shader_item_to_str(c[only_alpha][2], with_alpha, only_alpha, opt_alpha, true));
        append_str(buf, len, " + ");
        append_str(buf, len, shader_item_to_str(c[only_alpha][3], with_alpha, only_alpha, opt_alpha, false));
    }
}

static struct ShaderProgram* gfx_opengl_create_and_load_new_shader(uint64_t shader_id0, uint32_t shader_id1) {
    struct CCFeatures cc_features = { 0 };
    gfx_cc_get_features(shader_id0, shader_id1, &cc_features);

    char vs_buf[8192]; // was 2048; the GPU-palette per-input shade routing needs more
    char fs_buf[8192];
    size_t vs_len = 0;
    size_t fs_len = 0;
    size_t num_floats = 4;

    // Vertex shader

    vs_len += sprintf(vs_buf + vs_len, "#version %s\n", gl_glsl_version_str);

    if (gl_es) {
        append_line(vs_buf, &vs_len, "precision mediump float;");
    }

    if (gl_glsl_version >= 130) {
        append_line(vs_buf, &vs_len, "#define INPUT in");
        append_line(vs_buf, &vs_len, "#define OUTPUT out");
    } else {
        append_line(vs_buf, &vs_len, "#define INPUT attribute");
        append_line(vs_buf, &vs_len, "#define OUTPUT varying");
    }

    append_line(vs_buf, &vs_len, "INPUT vec4 aVtxPos;");
    append_line(vs_buf, &vs_len, "uniform mat4 uMVP;");

    for (int i = 0; i < 2; i++) {
        if (cc_features.used_textures[i]) {
            vs_len += sprintf(vs_buf + vs_len, "INPUT vec2 aTexCoord%d;\n", i);
            vs_len += sprintf(vs_buf + vs_len, "OUTPUT vec2 vTexCoord%d;\n", i);
            num_floats += 2;
            for (int j = 0; j < 2; j++) {
                if (cc_features.clamp[i][j]) {
                    vs_len += sprintf(vs_buf + vs_len, "INPUT float aTexClamp%s%d;\n", j == 0 ? "S" : "T", i);
                    vs_len += sprintf(vs_buf + vs_len, "OUTPUT float vTexClamp%s%d;\n", j == 0 ? "S" : "T", i);
                    num_floats += 1;
                }
            }
        }
    }
    if (cc_features.opt_fog) {
        append_line(vs_buf, &vs_len, "INPUT vec4 aFog;");
        append_line(vs_buf, &vs_len, "OUTPUT vec4 vFog;");
        // Display-list cache fog: uUseVertexFog selects the per-vertex baked fog
        // factor (aFog.a, the immediate path) vs a shader-computed distance fog
        // (cached G_FOG geometry, where the baked factor would be camera-stale).
        append_line(vs_buf, &vs_len, "uniform int uUseVertexFog;");
        append_line(vs_buf, &vs_len, "uniform float uFogMul;");
        append_line(vs_buf, &vs_len, "uniform float uFogOff;");
        num_floats += 4;
    }

    if (cc_features.opt_wireframe) {
        append_line(vs_buf, &vs_len, "OUTPUT vec3 vBary;");
    }
    if (cc_features.opt_grayscale) {
        append_line(vs_buf, &vs_len, "INPUT vec4 aGrayscaleColor;");
        append_line(vs_buf, &vs_len, "OUTPUT vec4 vGrayscaleColor;");
        num_floats += 4;
    }

    for (int i = 0; i < cc_features.num_inputs; i++) {
        vs_len += sprintf(vs_buf + vs_len, "INPUT vec%d aInput%d;\n", cc_features.opt_alpha ? 4 : 3, i + 1);
        vs_len += sprintf(vs_buf + vs_len, "OUTPUT vec%d vInput%d;\n", cc_features.opt_alpha ? 4 : 3, i + 1);
        num_floats += cc_features.opt_alpha ? 4 : 3;
    }

    // Display-list cache: live shade colour from a palette texture (PORT_DLCACHE.md).
    // Default-off via uPaletteEnable, so the immediate path is unchanged. Desktop GL
    // only -- ES may have zero vertex-shader texture units, so declaring a VS sampler
    // there could fail to link even with the cache off; ES keeps the re-record path
    // (cache_create_palette returns 0). aShadeIdx is supplied only by cache_draw.
    // Needs GLSL >= 130: the shade routing uses integer bitwise ops + texelFetch,
    // neither of which exists in GLSL 120. Older desktops / ES fall back to re-record.
    const bool palette_supported = cc_features.num_inputs > 0 && !gl_es && gl_glsl_version >= 130;
    if (palette_supported) {
        append_line(vs_buf, &vs_len, "INPUT float aShadeIdx;");
        append_line(vs_buf, &vs_len, "uniform sampler2D uPalette;");
        append_line(vs_buf, &vs_len, "uniform int uPaletteEnable;");
        append_line(vs_buf, &vs_len, "uniform float uPaletteW;");
        append_line(vs_buf, &vs_len, "uniform int uShadeRoute;");
    }

    append_line(vs_buf, &vs_len, "void main() {");
    for (int i = 0; i < 2; i++) {
        if (cc_features.used_textures[i]) {
            vs_len += sprintf(vs_buf + vs_len, "    vTexCoord%d = aTexCoord%d;\n", i, i);
            for (int j = 0; j < 2; j++) {
                if (cc_features.clamp[i][j]) {
                    vs_len += sprintf(vs_buf + vs_len, "    vTexClamp%s%d = aTexClamp%s%d;\n", j == 0 ? "S" : "T", i,
                                      j == 0 ? "S" : "T", i);
                }
            }
        }
    }
    if (cc_features.opt_grayscale) {
        append_line(vs_buf, &vs_len, "    vGrayscaleColor = aGrayscaleColor;");
    }
    if (palette_supported) {
        // uPaletteEnable==0 (immediate): pass baked combiner inputs straight through
        // (byte-identical). Cached: fetch the live shade colour once and substitute
        // it into shade input slots per uShadeRoute (bits0-1 rgb type, bit2 alpha).
        append_line(vs_buf, &vs_len, "    vec4 shadeCol = vec4(0.0);");
        append_line(vs_buf, &vs_len, "    if (uPaletteEnable != 0) {");
        if (gl_glsl_version >= 130) {
            append_line(vs_buf, &vs_len, "        shadeCol = texelFetch(uPalette, ivec2(int(aShadeIdx + 0.5), 0), 0);");
        } else {
            append_line(vs_buf, &vs_len, "        shadeCol = texture2DLod(uPalette, vec2((aShadeIdx + 0.5) / uPaletteW, 0.5), 0.0);");
        }
        append_line(vs_buf, &vs_len, "    }");
        for (int i = 0; i < cc_features.num_inputs; i++) {
            const int sh = i * 3;
            vs_len += sprintf(vs_buf + vs_len, "    if (uPaletteEnable != 0 && ((uShadeRoute >> %d) & 7) != 0) {\n", sh);
            vs_len += sprintf(vs_buf + vs_len, "        int r = (uShadeRoute >> %d) & 3;\n", sh);
            vs_len += sprintf(vs_buf + vs_len,
                "        vec3 rgb = (r == 1) ? shadeCol.rgb : (r == 2) ? vec3(shadeCol.a) : aInput%d.rgb;\n", i + 1);
            if (cc_features.opt_alpha) {
                vs_len += sprintf(vs_buf + vs_len,
                    "        float al = (((uShadeRoute >> %d) & 4) != 0) ? shadeCol.a : aInput%d.a;\n", sh, i + 1);
                vs_len += sprintf(vs_buf + vs_len, "        vInput%d = vec4(rgb, al);\n", i + 1);
            } else {
                vs_len += sprintf(vs_buf + vs_len, "        vInput%d = rgb;\n", i + 1);
            }
            vs_len += sprintf(vs_buf + vs_len, "    } else {\n");
            vs_len += sprintf(vs_buf + vs_len, "        vInput%d = aInput%d;\n", i + 1, i + 1);
            vs_len += sprintf(vs_buf + vs_len, "    }\n");
        }
    } else {
        // No palette (ES, or no combiner inputs): original passthrough, byte-identical.
        for (int i = 0; i < cc_features.num_inputs; i++) {
            vs_len += sprintf(vs_buf + vs_len, "    vInput%d = aInput%d;\n", i + 1, i + 1);
        }
    }

    if (cc_features.opt_wireframe) {
        // Every draw in this renderer is a NON-INDEXED triangle list whose first
        // vertex is a multiple of 3 (glDrawArrays / SDL_DrawGPUPrimitives; 0 on
        // the cached path, and the vertex ring aligns its cursor to whole
        // triangles), so a vertex's corner within its triangle is simply
        // gl_VertexID % 3 — no extra vertex attribute needed.
        append_line(vs_buf, &vs_len, "    int bcorner = gl_VertexID % 3;");
        append_line(vs_buf, &vs_len, "    vBary = vec3(bcorner == 0 ? 1.0 : 0.0, bcorner == 1 ? 1.0 : 0.0, bcorner == 2 ? 1.0 : 0.0);");
    }
    append_line(vs_buf, &vs_len, "    gl_Position = uMVP * aVtxPos;");

    if (cc_features.opt_fog) {
        // Fog colour is always the baked per-vertex value; the factor is either
        // baked (aFog.a, immediate path) or recomputed from the un-hacked clip
        // z/w (cached distance fog) — must run before the depth-clamp z hack.
        append_line(vs_buf, &vs_len, "    vFog.rgb = aFog.rgb;");
        append_line(vs_buf, &vs_len, "    if (uUseVertexFog != 0) {");
        append_line(vs_buf, &vs_len, "        vFog.a = aFog.a;");
        append_line(vs_buf, &vs_len, "    } else {");
        // Mirror gfx_sp_vertex's fog exactly, incl. the near/behind-eye guards:
        // clamp |w|<0.001 and force max fog for w<0 (winv<0). Without this, the
        // close geometry of the room you're standing in (tiny/negative w) gets
        // garbage fog and is painted with the fog/sky colour. For w>0.001 this is
        // identical to z/w, so far geometry is unchanged.
        append_line(vs_buf, &vs_len, "        float fw = gl_Position.w;");
        append_line(vs_buf, &vs_len, "        if (abs(fw) < 0.001) fw = 0.001;");
        append_line(vs_buf, &vs_len, "        float winv = 1.0 / fw;");
        append_line(vs_buf, &vs_len, "        if (winv < 0.0) winv = 32767.0;");
        append_line(vs_buf, &vs_len, "        float fz = gl_Position.z * winv * uFogMul + uFogOff;");
        append_line(vs_buf, &vs_len, "        vFog.a = clamp(fz, 0.0, 255.0) / 255.0;");
        append_line(vs_buf, &vs_len, "    }");
    }

    if (!GLAD_GL_ARB_depth_clamp) {
        // HACK: workaround for no GL_DEPTH_CLAMP
        append_line(vs_buf, &vs_len, "    gl_Position.z *= 0.3f;");
    }
    append_line(vs_buf, &vs_len, "}");

    // Fragment shader

    fs_len += sprintf(fs_buf + fs_len, "#version %s\n", gl_glsl_version_str);

    if (gl_es) {
        append_line(fs_buf, &fs_len, "precision mediump float;");
    }

    if (gl_glsl_version >= 130) {
        append_line(fs_buf, &fs_len, "#define INPUT in");
        append_line(fs_buf, &fs_len, "#define OUTPUT_COLOR outColor");
        append_line(fs_buf, &fs_len, "#define SAMPLE_TEX(tex, uv) texture(tex, uv)");
    } else {
        append_line(fs_buf, &fs_len, "#define INPUT varying");
        append_line(fs_buf, &fs_len, "#define OUTPUT_COLOR gl_FragColor");
        append_line(fs_buf, &fs_len, "#define SAMPLE_TEX(tex, uv) texture2D(tex, uv)");
    }

    // Reference approach to color wrapping as per GLideN64
    // Return wrapped value of x in interval [low, high)
    append_line(fs_buf, &fs_len, "#define WRAP(x, low, high) mod((x)-(low), (high)-(low)) + (low)");

    append_line(fs_buf, &fs_len, "#define TEX_OFFSET(tex, uv, texSize, off) SAMPLE_TEX(tex, uv - (off)/texSize)");

    // append_line(fs_buf, &fs_len, "precision mediump float;");
    for (int i = 0; i < 2; i++) {
        if (cc_features.used_textures[i]) {
            fs_len += sprintf(fs_buf + fs_len, "INPUT vec2 vTexCoord%d;\n", i);
            for (int j = 0; j < 2; j++) {
                if (cc_features.clamp[i][j]) {
                    fs_len += sprintf(fs_buf + fs_len, "INPUT float vTexClamp%s%d;\n", j == 0 ? "S" : "T", i);
                }
            }
        }
    }
    if (cc_features.opt_fog) {
        append_line(fs_buf, &fs_len, "INPUT vec4 vFog;");
    }
    if (cc_features.opt_grayscale) {
        append_line(fs_buf, &fs_len, "INPUT vec4 vGrayscaleColor;");
    }
    for (int i = 0; i < cc_features.num_inputs; i++) {
        fs_len += sprintf(fs_buf + fs_len, "INPUT vec%d vInput%d;\n", cc_features.opt_alpha ? 4 : 3, i + 1);
    }

    if (cc_features.used_textures[0]) {
        append_line(fs_buf, &fs_len, "uniform sampler2D uTex0;");
        if (current_filter_mode == FILTER_THREE_POINT)
            append_line(fs_buf, &fs_len, "uniform int three_point_filter0;");
    }
    if (cc_features.used_textures[1]) {
        append_line(fs_buf, &fs_len, "uniform sampler2D uTex1;");
        if (current_filter_mode == FILTER_THREE_POINT)
            append_line(fs_buf, &fs_len, "uniform int three_point_filter1;");
    }

    append_line(fs_buf, &fs_len, "uniform int frame_count;");
    append_line(fs_buf, &fs_len, "uniform float noise_scale;");
    // Wireframe cheat flat wire colour: rgb = colour, a > 0.5 enables the override.
    append_line(fs_buf, &fs_len, "uniform vec4 wireframe_color;");
    if (cc_features.opt_wireframe) {
        append_line(fs_buf, &fs_len, "INPUT vec3 vBary;");
        append_line(fs_buf, &fs_len, "uniform float wireframe_thickness;");
    }

    append_line(fs_buf, &fs_len, "float random(in vec3 value) {");
    append_line(fs_buf, &fs_len, "    float random = dot(sin(value), vec3(12.9898, 78.233, 37.719));");
    append_line(fs_buf, &fs_len, "    return fract(sin(random) * 143758.5453);");
    append_line(fs_buf, &fs_len, "}");

    if (current_filter_mode == FILTER_THREE_POINT) {
        append_line(fs_buf, &fs_len, "vec4 filter3point(in sampler2D tex, in vec2 texCoord, in vec2 texSize) {");
        append_line(fs_buf, &fs_len, "    vec2 offset = fract(texCoord*texSize - vec2(0.5));");
        append_line(fs_buf, &fs_len, "    offset -= step(1.0, offset.x + offset.y);");
        append_line(fs_buf, &fs_len, "    vec4 c0 = TEX_OFFSET(tex, texCoord, texSize, offset);");
        append_line(fs_buf, &fs_len, "    vec4 c1 = TEX_OFFSET(tex, texCoord, texSize, vec2(offset.x - sign(offset.x), offset.y));");
        append_line(fs_buf, &fs_len, "    vec4 c2 = TEX_OFFSET(tex, texCoord, texSize, vec2(offset.x, offset.y - sign(offset.y)));");
        append_line(fs_buf, &fs_len, "    return c0 + abs(offset.x)*(c1-c0) + abs(offset.y)*(c2-c0);");
        append_line(fs_buf, &fs_len, "}");
    }

    if (cc_features.opt_blur) {
        // blur filter, used for menu backgrounds
        // used to be two for loops from 0 to 4, but apparently intel drivers crashed trying to unroll it
        // used to have a const weight array, but apparently drivers for the GT620 don't like const array initializers

        if (current_filter_mode == FILTER_THREE_POINT)
            append_line(fs_buf, &fs_len, "lowp vec4 hookTexture2D(in sampler2D t, in vec2 uv, in vec2 texSize, in int three_point_filter) {");
        else
            append_line(fs_buf, &fs_len, "lowp vec4 hookTexture2D(in sampler2D t, in vec2 uv, in vec2 texSize) {");

        append_line(fs_buf, &fs_len, "    lowp vec4 cw = vec4(0.0);");
        append_line(fs_buf, &fs_len, "    for (int i = 0; i < 16; ++i) {");
        append_line(fs_buf, &fs_len, "        vec2 xy = vec2(float(i & 3), float(i >> 2));");
        append_line(fs_buf, &fs_len, "        lowp float w = 0.009947 - length(xy) * 0.001;");
        append_line(fs_buf, &fs_len, "        vec2 scaled_uv = uv + (vec2(-1.5) + xy) / texSize;");

        if (current_filter_mode == FILTER_THREE_POINT)
            append_line(fs_buf, &fs_len, "        lowp vec4 tex = mix(SAMPLE_TEX(t, scaled_uv), filter3point(t, scaled_uv, texSize), three_point_filter);");
        else
            append_line(fs_buf, &fs_len, "        lowp vec4 tex = SAMPLE_TEX(t, scaled_uv);");

        append_line(fs_buf, &fs_len, "        cw += vec4(tex.rgb * w, w);");
        append_line(fs_buf, &fs_len, "    }");
        append_line(fs_buf, &fs_len, "    return vec4(cw.rgb / cw.a, 1.0);");
        append_line(fs_buf, &fs_len, "}");
    } else {
        if (current_filter_mode == FILTER_THREE_POINT) {
            append_line(fs_buf, &fs_len, "vec4 hookTexture2D(in sampler2D tex, in vec2 uv, in vec2 texSize, in int three_point_filter) {");
            append_line(fs_buf, &fs_len, "    return mix(SAMPLE_TEX(tex, uv), filter3point(tex, uv, texSize), three_point_filter);");
            append_line(fs_buf, &fs_len, "}");
        } else {
            append_line(fs_buf, &fs_len, "vec4 hookTexture2D(in sampler2D tex, in vec2 uv, in vec2 texSize) {");
            append_line(fs_buf, &fs_len, "    return SAMPLE_TEX(tex, uv);");
            append_line(fs_buf, &fs_len, "}");
        }
    }

    if (gl_glsl_version >= 130) {
        append_line(fs_buf, &fs_len, "out vec4 outColor;");
    }

    append_line(fs_buf, &fs_len, "void main() {");

    for (int i = 0; i < 2; i++) {
        if (cc_features.used_textures[i]) {
            bool s = cc_features.clamp[i][0], t = cc_features.clamp[i][1];

            fs_len += sprintf(fs_buf + fs_len, "    vec2 texSize%d = vec2(textureSize(uTex%d, 0));\n", i, i);

            if (!s && !t) {
                fs_len += sprintf(fs_buf + fs_len, "    vec2 vTexCoordAdj%d = vTexCoord%d;\n", i, i);
            } else {
                if (s && t) {
                    fs_len += sprintf(fs_buf + fs_len,
                                      "    vec2 vTexCoordAdj%d = clamp(vTexCoord%d, 0.5 / texSize%d, "
                                      "vec2(vTexClampS%d, vTexClampT%d));\n",
                                      i, i, i, i, i);
                } else if (s) {
                    fs_len += sprintf(fs_buf + fs_len,
                                      "    vec2 vTexCoordAdj%d = vec2(clamp(vTexCoord%d.s, 0.5 / "
                                      "texSize%d.s, vTexClampS%d), vTexCoord%d.t);\n",
                                      i, i, i, i, i);
                } else {
                    fs_len += sprintf(fs_buf + fs_len,
                                      "    vec2 vTexCoordAdj%d = vec2(vTexCoord%d.s, clamp(vTexCoord%d.t, "
                                      "0.5 / texSize%d.t, vTexClampT%d));\n",
                                      i, i, i, i, i);
                }
            }

            if (current_filter_mode == FILTER_THREE_POINT)
                fs_len += sprintf(fs_buf + fs_len, "    vec4 texVal%d = hookTexture2D(uTex%d, vTexCoordAdj%d, texSize%d, three_point_filter%d);\n", i, i, i, i, i);
            else
                fs_len += sprintf(fs_buf + fs_len, "    vec4 texVal%d = hookTexture2D(uTex%d, vTexCoordAdj%d, texSize%d);\n", i, i, i, i);
        }
    }

    append_line(fs_buf, &fs_len, cc_features.opt_alpha ? "    vec4 texel;" : "    vec3 texel;");
    for (int c = 0; c < (cc_features.opt_2cyc ? 2 : 1); c++) {
        append_str(fs_buf, &fs_len, "    texel = ");
        if (!cc_features.color_alpha_same[c] && cc_features.opt_alpha) {
            append_str(fs_buf, &fs_len, "vec4(");
            append_formula(fs_buf, &fs_len, cc_features.c[c], cc_features.do_single[c][0],
                           cc_features.do_multiply[c][0], cc_features.do_mix[c][0], false, false, true);
            append_str(fs_buf, &fs_len, ", ");
            append_formula(fs_buf, &fs_len, cc_features.c[c], cc_features.do_single[c][1],
                           cc_features.do_multiply[c][1], cc_features.do_mix[c][1], true, true, true);
            append_str(fs_buf, &fs_len, ")");
        } else {
            append_formula(fs_buf, &fs_len, cc_features.c[c], cc_features.do_single[c][0],
                           cc_features.do_multiply[c][0], cc_features.do_mix[c][0], cc_features.opt_alpha, false,
                           cc_features.opt_alpha);
        }
        append_line(fs_buf, &fs_len, ";");

        if (c == 0) {
            append_line(fs_buf, &fs_len, "    texel = WRAP(texel, -1.01, 1.01);");
        }
    }

    append_line(fs_buf, &fs_len, "    texel = WRAP(texel, -0.51, 1.51);");
    append_line(fs_buf, &fs_len, "    texel = clamp(texel, 0.0, 1.0);");
    // TODO discard if alpha is 0?
    if (cc_features.opt_fog) {
        if (cc_features.opt_alpha) {
            append_line(fs_buf, &fs_len, "    texel = vec4(mix(texel.rgb, vFog.rgb, vFog.a), texel.a);");
        } else {
            append_line(fs_buf, &fs_len, "    texel = mix(texel, vFog.rgb, vFog.a);");
        }
    }

    if (cc_features.opt_texture_edge && cc_features.opt_alpha) {
        append_line(fs_buf, &fs_len, "    if (texel.a > 0.19) texel.a = 1.0; else discard;");
    }

    if (cc_features.opt_alpha && cc_features.opt_noise) {
        append_line(fs_buf, &fs_len,
                    "    texel.a *= floor(clamp(random(vec3(floor(gl_FragCoord.xy * noise_scale), float(frame_count))) + "
                    "texel.a, 0.0, 1.0));");
    }

    if (cc_features.opt_grayscale) {
        append_line(fs_buf, &fs_len, "    float intensity = (texel.r + texel.g + texel.b) / 3.0;");
        append_line(fs_buf, &fs_len, "    vec3 new_texel = vGrayscaleColor.rgb * intensity;");
        append_line(fs_buf, &fs_len, "    texel.rgb = mix(texel.rgb, new_texel, vGrayscaleColor.a);");
    }

    if (cc_features.opt_wireframe) {
        // Single-pass barycentric wireframe: keep fragments within
        // wireframe_thickness pixels of a triangle edge, discard the interior.
        // fwidth() converts the barycentric gradient into screen space, so the
        // wire is a constant pixel width at any depth or angle — and unlike
        // glLineWidth it isn't clamped by the driver, and unlike hardware line
        // mode it works on backends with no line-width concept at all.
        append_line(fs_buf, &fs_len, "    vec3 bw = fwidth(vBary);");
        append_line(fs_buf, &fs_len, "    vec3 bedge = smoothstep(vec3(0.0), bw * wireframe_thickness, vBary);");
        append_line(fs_buf, &fs_len, "    float bmin = min(min(bedge.x, bedge.y), bedge.z);");
        append_line(fs_buf, &fs_len, "    if (bmin >= 1.0) discard;");
    }

    // Wireframe cheat: replace the surface colour with a flat wire colour when enabled.
    append_line(fs_buf, &fs_len, "    if (wireframe_color.a > 0.5) texel.rgb = wireframe_color.rgb;");

    if (cc_features.opt_alpha) {
        if (cc_features.opt_alpha_threshold) {
            append_line(fs_buf, &fs_len, "    if (texel.a < 8.0 / 256.0) discard;");
        }
        if (cc_features.opt_invisible) {
            append_line(fs_buf, &fs_len, "    texel.a = 0.0;");
        }

        append_line(fs_buf, &fs_len, "    OUTPUT_COLOR = texel;");
    } else {
        append_line(fs_buf, &fs_len, "    OUTPUT_COLOR = vec4(texel, 1.0);");
    }

    append_line(fs_buf, &fs_len, "}");

    vs_buf[vs_len] = '\0';
    fs_buf[fs_len] = '\0';

    const GLchar* sources[2] = { vs_buf, fs_buf };
    const GLint lengths[2] = { (GLint)vs_len, (GLint)fs_len };
    GLint success;

    GLuint vertex_shader = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vertex_shader, 1, &sources[0], &lengths[0]);
    glCompileShader(vertex_shader);
    glGetShaderiv(vertex_shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        GLint max_length = 1024;
        glGetShaderiv(vertex_shader, GL_INFO_LOG_LENGTH, &max_length);
        char error_log[1024];
        glGetShaderInfoLog(vertex_shader, max_length, &max_length, &error_log[0]);
        sysLogPrintf(LOG_ERROR, "Failed to compile this vertex shader (ID %llx, %x):\n%s", shader_id0, shader_id1, vs_buf);
        sysFatalError("Vertex shader compilation failed:\n%s", error_log);
    }

    GLuint fragment_shader = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fragment_shader, 1, &sources[1], &lengths[1]);
    glCompileShader(fragment_shader);
    glGetShaderiv(fragment_shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        GLint max_length = 1024;
        glGetShaderiv(fragment_shader, GL_INFO_LOG_LENGTH, &max_length);
        char error_log[1024];
        glGetShaderInfoLog(fragment_shader, max_length, &max_length, &error_log[0]);
        sysLogPrintf(LOG_ERROR, "Failed to compile this fragment shader (ID %llx, %x):\n%s", shader_id0, shader_id1, fs_buf);
        sysFatalError("Fragment shader compilation failed:\n%s", error_log);
    }

    GLuint shader_program = glCreateProgram();
    glAttachShader(shader_program, vertex_shader);
    glAttachShader(shader_program, fragment_shader);
    glLinkProgram(shader_program);

    glDetachShader(shader_program, vertex_shader);
    glDetachShader(shader_program, fragment_shader);
    glDeleteShader(vertex_shader);
    glDeleteShader(fragment_shader);

    size_t cnt = 0;

    struct ShaderProgram* prg = &shader_program_pool[make_pair(shader_id0, shader_id1)];
    prg->attrib_locations[cnt] = glGetAttribLocation(shader_program, "aVtxPos");
    prg->attrib_sizes[cnt] = 4;
    ++cnt;

    for (int i = 0; i < 2; i++) {
        if (cc_features.used_textures[i]) {
            char name[32];
            sprintf(name, "aTexCoord%d", i);
            prg->attrib_locations[cnt] = glGetAttribLocation(shader_program, name);
            prg->attrib_sizes[cnt] = 2;
            ++cnt;

            for (int j = 0; j < 2; j++) {
                if (cc_features.clamp[i][j]) {
                    sprintf(name, "aTexClamp%s%d", j == 0 ? "S" : "T", i);
                    prg->attrib_locations[cnt] = glGetAttribLocation(shader_program, name);
                    prg->attrib_sizes[cnt] = 1;
                    ++cnt;
                }
            }
        }
    }

    if (cc_features.opt_fog) {
        prg->attrib_locations[cnt] = glGetAttribLocation(shader_program, "aFog");
        prg->attrib_sizes[cnt] = 4;
        ++cnt;
    }

    if (cc_features.opt_grayscale) {
        prg->attrib_locations[cnt] = glGetAttribLocation(shader_program, "aGrayscaleColor");
        prg->attrib_sizes[cnt] = 4;
        ++cnt;
    }

    for (int i = 0; i < cc_features.num_inputs; i++) {
        char name[16];
        sprintf(name, "aInput%d", i + 1);
        prg->attrib_locations[cnt] = glGetAttribLocation(shader_program, name);
        prg->attrib_sizes[cnt] = cc_features.opt_alpha ? 4 : 3;
        ++cnt;
    }

    prg->opengl_program_id = shader_program;
    prg->num_inputs = cc_features.num_inputs;
    prg->used_textures[0] = cc_features.used_textures[0];
    prg->used_textures[1] = cc_features.used_textures[1];
    prg->num_floats = num_floats;
    prg->num_attribs = cnt;

    glUseProgram(shader_program);

    if (cc_features.used_textures[0]) {
        GLint sampler_location = glGetUniformLocation(shader_program, "uTex0");
        glUniform1i(sampler_location, 0);
    }
    if (cc_features.used_textures[1]) {
        GLint sampler_location = glGetUniformLocation(shader_program, "uTex1");
        glUniform1i(sampler_location, 1);
    }

    prg->frame_count_location = glGetUniformLocation(shader_program, "frame_count");
    prg->noise_scale_location = glGetUniformLocation(shader_program, "noise_scale");
    prg->three_point_filter_locations[0] = glGetUniformLocation(shader_program, "three_point_filter0");
    prg->three_point_filter_locations[1] = glGetUniformLocation(shader_program, "three_point_filter1");
    prg->wireframe_color_location = glGetUniformLocation(shader_program, "wireframe_color");
    prg->wireframe_thickness_location = glGetUniformLocation(shader_program, "wireframe_thickness");
    prg->mvp_location = glGetUniformLocation(shader_program, "uMVP");
    if (prg->mvp_location >= 0) {
        // Program is already bound (glUseProgram above); seed with the current
        // matrix (identity unless mid-replay).
        glUniformMatrix4fv(prg->mvp_location, 1, GL_FALSE, gfx_current_mvp);
    }
    prg->use_vertex_fog_location = glGetUniformLocation(shader_program, "uUseVertexFog");
    prg->fog_mul_location = glGetUniformLocation(shader_program, "uFogMul");
    prg->fog_off_location = glGetUniformLocation(shader_program, "uFogOff");
    if (prg->use_vertex_fog_location >= 0) {
        glUniform1i(prg->use_vertex_fog_location, gfx_current_use_vertex_fog);
    }
    if (prg->fog_mul_location >= 0) {
        glUniform1f(prg->fog_mul_location, gfx_current_fog_mul);
    }
    if (prg->fog_off_location >= 0) {
        glUniform1f(prg->fog_off_location, gfx_current_fog_off);
    }
    prg->shade_idx_location = glGetAttribLocation(shader_program, "aShadeIdx");
    prg->palette_enable_location = glGetUniformLocation(shader_program, "uPaletteEnable");
    prg->palette_w_location = glGetUniformLocation(shader_program, "uPaletteW");
    prg->shade_route_location = glGetUniformLocation(shader_program, "uShadeRoute");
    {
        GLint pal = glGetUniformLocation(shader_program, "uPalette");
        if (pal >= 0) {
            glUniform1i(pal, GFX_PALETTE_TEX_UNIT); // sampler -> palette texture unit
        }
    }
    if (prg->palette_enable_location >= 0) {
        glUniform1i(prg->palette_enable_location, gfx_current_palette_enable);
    }
    if (prg->palette_w_location >= 0) {
        glUniform1f(prg->palette_w_location, gfx_current_palette_w);
    }
    if (prg->shade_route_location >= 0) {
        glUniform1i(prg->shade_route_location, gfx_current_shade_routing);
    }

    gfx_opengl_load_shader(prg);

    return prg;
}

static struct ShaderProgram* gfx_opengl_lookup_shader(uint64_t shader_id0, uint32_t shader_id1) {
    auto it = shader_program_pool.find(make_pair(shader_id0, shader_id1));
    return it == shader_program_pool.end() ? nullptr : &it->second;
}

static void gfx_opengl_shader_get_info(struct ShaderProgram* prg, uint8_t* num_inputs, bool used_textures[2]) {
    *num_inputs = prg->num_inputs;
    used_textures[0] = prg->used_textures[0];
    used_textures[1] = prg->used_textures[1];
}

static void gfx_opengl_clear_shaders(void) {
    glUseProgram(0);
    for (auto& pair : shader_program_pool) {
        glDeleteProgram(pair.second.opengl_program_id);
    }
    shader_program_pool.clear();
    // The pool owned this pointer; the uniform-sync path both reads and writes
    // through it, so it must not dangle past the pool's destruction.
    gfx_current_shader_program = NULL;
}

static GLuint gfx_opengl_new_texture(void) {
    GLuint ret;
    glGenTextures(1, &ret);
    return ret;
}

static void gfx_opengl_delete_texture(uint32_t texID) {
    glDeleteTextures(1, &texID);
    // GL reverts the binding of a deleted texture to 0 on every unit it was
    // bound to (current context) — mirror that in the shadow.
    for (int i = 0; i < GFX_NUM_TEX_UNITS; i++) {
        if (s_bound_texture[i] == texID) {
            s_bound_texture[i] = 0;
        }
    }
}

static void gfx_opengl_select_texture(int tile, GLuint texture_id, bool linear_filter) {
    // Shadowed: skip the activate/bind when unchanged. Post-condition preserved
    // either way: active unit == tile with texture_id bound (upload_texture and
    // set_sampler_parameters rely on the active unit). Reused ids from gfx_pc's
    // free_texture_ids pool are the same still-live GL texture object, so a
    // matching shadow entry is a genuine bind.
    if (s_active_texture_unit != tile) {
        glActiveTexture(GL_TEXTURE0 + tile);
        s_active_texture_unit = tile;
    }
    if (tile >= GFX_NUM_TEX_UNITS || s_bound_texture[tile] != texture_id) {
        glBindTexture(GL_TEXTURE_2D, texture_id);
        if (tile < GFX_NUM_TEX_UNITS) {
            s_bound_texture[tile] = texture_id;
        }
    }

    if (current_textures_linear_filter[tile] != linear_filter) {
        current_textures_linear_filter[tile] = linear_filter;
        gfx_uniform_generation++; // feeds the three_point_filter uniforms
    }
}

static void gfx_opengl_upload_texture(const uint8_t* rgba32_buf, uint32_t width, uint32_t height, bool gen_mipmaps) {
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba32_buf);
	// Note: FILTER_THREE_POINT no longer forces mipmap generation — its
	// min_filters row is all GL_NEAREST, so the chain was never sampled (A17).
	if (gen_mipmaps) {
		glGenerateMipmap(GL_TEXTURE_2D);
	}
}

static uint32_t gfx_cm_to_opengl(uint32_t val) {
    switch (val) {
        case G_TX_NOMIRROR | G_TX_CLAMP:
            return GL_CLAMP_TO_EDGE;
        case G_TX_MIRROR | G_TX_WRAP:
            return GL_MIRRORED_REPEAT;
        case G_TX_MIRROR | G_TX_CLAMP:
            return gl_mirror_clamp;
        case G_TX_NOMIRROR | G_TX_WRAP:
            return GL_REPEAT;
    }
    return 0;
}

static void gfx_opengl_set_sampler_parameters(int tile, bool linear_filter, uint32_t cms, uint32_t cmt, bool mipmaps) {
    const GLint min_filters[3][3] = {
        // MIPMAP_DISABLED   MIPMAP_NEAREST               MIPMAP_LINEAR
        {  GL_NEAREST,       GL_NEAREST_MIPMAP_NEAREST,   GL_NEAREST_MIPMAP_LINEAR  }, // FILTER_NONE
        {  GL_LINEAR,        GL_LINEAR_MIPMAP_NEAREST,    GL_LINEAR_MIPMAP_LINEAR   }, // FILTER_BILINEAR
        {  GL_NEAREST,       GL_NEAREST,                  GL_NEAREST                }, // FILTER_THREE_POINT
    };

    mipmaps = mipmaps && (current_mipmap_filter_mode != MIPMAP_DISABLED);
    const int mip_idx = mipmaps ? current_mipmap_filter_mode : 0;
    const GLint min_filter = linear_filter ? min_filters[current_filter_mode][mip_idx] : GL_NEAREST;
    const GLint max_filter = linear_filter && (current_filter_mode == FILTER_LINEAR) ? GL_LINEAR : GL_NEAREST;

    if (s_active_texture_unit != tile) {
        glActiveTexture(GL_TEXTURE0 + tile);
        s_active_texture_unit = tile;
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, min_filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, max_filter);

    if (mipmaps) {
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY, current_anisotropy_level);
    }

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, gfx_cm_to_opengl(cms));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, gfx_cm_to_opengl(cmt));
}

// Only two polygon-offset states ever exist (disabled at 0,0 / decal at -2,-2);
// shadowed so the common disabled->disabled case costs no GL calls.
static void gfx_opengl_set_polygon_offset_state(int enabled) {
    if (s_polygon_offset_fill == enabled) {
        return;
    }
    if (enabled) {
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(-2, -2);
    } else {
        glDisable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(0, 0);
    }
    s_polygon_offset_fill = enabled;
}

static void gfx_opengl_set_depth_mode(bool depth_test, bool depth_update, bool depth_compare, bool depth_source_prim, uint16_t zmode) {
    s_wireframe_depth_test = depth_test;
    if (depth_test) {
        glEnable(GL_DEPTH_TEST);
        glDepthMask(depth_update ? GL_TRUE : GL_FALSE);
        current_depth_mask = depth_update;

        if (depth_compare) {
            switch (zmode) {
                case ZMODE_INTER:
                    glDepthFunc(GL_LEQUAL);
                    gfx_opengl_set_polygon_offset_state(0);
                    break;

                case ZMODE_OPA:
                case ZMODE_XLU:
                    if (depth_source_prim) {
                        glDepthFunc(GL_LEQUAL);
                    } else {
                        glDepthFunc(GL_LESS);
                    }
                    gfx_opengl_set_polygon_offset_state(0);
                    break;

                case ZMODE_DEC:
                    glDepthFunc(GL_LEQUAL);
                    gfx_opengl_set_polygon_offset_state(1);
                    break;
            }
        } else {
            glDepthFunc(GL_ALWAYS);
            gfx_opengl_set_polygon_offset_state(0);
        }
    } else {
        glDisable(GL_DEPTH_TEST);
    }
}

static void gfx_opengl_set_depth_range(float znear, float zfar) {
    if (glDepthRangef) {
        glDepthRangef(znear, zfar);
    } else {
        glDepthRange(znear, zfar);
    }
}

static void gfx_opengl_set_viewport(int x, int y, int width, int height) {
    glViewport(x, y, width, height);
}

static void gfx_opengl_set_scissor(int x, int y, int width, int height) {
    glScissor(x, y, width, height);
}

static void gfx_opengl_set_use_alpha(bool use_alpha, bool modulate) {
    // Shadowed independently: enable state and blend func are separate GL
    // state, each skipped when unchanged (-1 shadow = unknown, always issue).
    const int ena = use_alpha ? 1 : 0;
    const int mod = modulate ? 1 : 0;
    if (s_blend_enabled != ena) {
        if (use_alpha) {
            glEnable(GL_BLEND);
        } else {
            glDisable(GL_BLEND);
        }
        s_blend_enabled = ena;
    }
    if (s_blend_modulate != mod) {
        if (modulate) {
            glBlendFunc(GL_DST_COLOR, GL_ZERO);
        } else {
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        }
        s_blend_modulate = mod;
    }
}

static void gfx_opengl_draw_triangles(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris) {
    // printf("flushing %d tris\n", buf_vbo_num_tris);
    if (buf_vbo_num_tris == 0) {
        return;
    }

    GLint first_vtx = 0;
    const size_t bytes = sizeof(float) * buf_vbo_len;
    if (s_vtx_ring_active && bytes > GFX_VTX_RING_REGION_SIZE) {
        gfx_opengl_vtx_ring_disable("flush larger than a ring region");
    }
    if (s_vtx_ring_active) {
        const size_t tri_bytes = bytes / buf_vbo_num_tris; // 3 verts * stride, exact
        const size_t region_base = (size_t)s_vtx_ring_region * GFX_VTX_RING_REGION_SIZE;
        // Align the cursor to a whole triangle: keeps the attrib offsets
        // stride-aligned AND `first` a multiple of 3 for gl_VertexID % 3.
        size_t rel = s_vtx_ring_cursor - region_base;
        rel += (tri_bytes - rel % tri_bytes) % tri_bytes;
        if (rel + bytes > GFX_VTX_RING_REGION_SIZE) {
            gfx_opengl_vtx_ring_rotate(); // waits the incoming region's fence
            rel = 0;
        }
        const size_t off = (size_t)s_vtx_ring_region * GFX_VTX_RING_REGION_SIZE + rel;
        memcpy(s_vtx_ring_map + off, buf_vbo, bytes); // coherent mapping: no explicit flush
        s_vtx_ring_cursor = off + bytes;
        first_vtx = (GLint)((off - s_vtx_ring_attrib_base) / (tri_bytes / 3));
    } else {
        glBufferData(GL_ARRAY_BUFFER, sizeof(float) * buf_vbo_len, buf_vbo, GL_STREAM_DRAW);
    }

    // Wireframe cheat: draw depth-tested 3D geometry as polygon outlines. Skipped
    // for 2D HUD/menus (no depth test) and on GL ES (glPolygonMode is desktop-GL only).
    const bool wireframe = (gfx_wireframe_mode || gfx_wireframe_scope) && s_wireframe_depth_test && !gl_es;
    const bool wire_colour = wireframe && gfx_wireframe_wire_color_enabled
            && gfx_current_shader_program && gfx_current_shader_program->wireframe_color_location >= 0;
    // iPod Ad silhouette: flat-fill depth-tested 3D geometry with the current
    // scope colour (walls bright, chrs black, objects/weapons white). Reuses
    // the wireframe_color shader stage. Not while wireframe outlines are on.
    const bool sil_fill = gfx_silhouette && !wireframe && s_wireframe_depth_test
            && gfx_current_shader_program && gfx_current_shader_program->wireframe_color_location >= 0;
    // Shader wireframe (SHADER_OPT_WIREFRAME) draws the outline itself via the
    // barycentric edge test, so the geometry stays FILLED and no hardware line
    // state is touched — that is the only path that honours thickness reliably.
    // The glPolygonMode path remains for GL ES / GLSL < 130, where gl_VertexID
    // isn't available (there thickness is still driver-limited, as before).
    const bool shader_wire = wireframe && gfx_opengl_shader_wireframe_supported()
            && gfx_current_shader_program && gfx_current_shader_program->wireframe_thickness_location >= 0;
    if (shader_wire) {
        glUniform1f(gfx_current_shader_program->wireframe_thickness_location, gfx_wireframe_line_width);
    } else if (wireframe) {
        glLineWidth(gfx_wireframe_line_width);
        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    }
    if (wire_colour) {
        // Force a flat wire colour instead of the surface's textured/shaded colour.
        glUniform4f(gfx_current_shader_program->wireframe_color_location,
                gfx_wireframe_wire_color[0], gfx_wireframe_wire_color[1], gfx_wireframe_wire_color[2], 1.0f);
    }
    if (sil_fill) {
        glUniform4f(gfx_current_shader_program->wireframe_color_location,
                gfx_silhouette_color[0], gfx_silhouette_color[1], gfx_silhouette_color[2], 1.0f);
    }

    glDrawArrays(GL_TRIANGLES, first_vtx, 3 * buf_vbo_num_tris);

    if (sil_fill && gfx_silhouette_edges) {
        // White wireframe edges over the flat fill (the iPod-ad geometry
        // outline) — walls only; chrs/objects/weapons stay clean silhouettes.
        // Second pass in line mode with a white wire colour.
        glLineWidth(gfx_wireframe_line_width);
        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
        glUniform4f(gfx_current_shader_program->wireframe_color_location, 1.0f, 1.0f, 1.0f, 1.0f);
        glDrawArrays(GL_TRIANGLES, first_vtx, 3 * buf_vbo_num_tris);
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        glLineWidth(1.0f);
    }

    if (wire_colour || sil_fill) {
        // Reset so subsequent draws sharing this program (e.g. the HUD) are unaffected.
        glUniform4f(gfx_current_shader_program->wireframe_color_location, 0.0f, 0.0f, 0.0f, 0.0f);
    }
    if (wireframe && !shader_wire) {
        // Only the hardware-line fallback touched this state.
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        glLineWidth(1.0f);
    }
}

// --- Display-list cache (port-only; see docs/PORT_DLCACHE.md) ---
// Persistent VBOs of object-space room geometry, replayed each frame with a
// GPU-side uMVP instead of CPU-transforming every vertex. All cached draws use
// the same single VAO as the immediate path; the only state that leaks is the
// GL_ARRAY_BUFFER binding and the attrib pointers, which cache_replay_end and a
// forced shader reload (in gfx_pc) restore for immediate-mode drawing.

static uint32_t gfx_opengl_cache_create_buffer(const float* data, size_t num_floats) {
    GLuint buf = 0;
    glGenBuffers(1, &buf);
    if (buf == 0) {
        return 0;
    }
    glBindBuffer(GL_ARRAY_BUFFER, buf);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float) * num_floats, data, GL_STATIC_DRAW);
    // Restore the immediate-mode buffer binding (draw_triangles assumes it).
    glBindBuffer(GL_ARRAY_BUFFER, opengl_vbo);
    return buf;
}

static void gfx_opengl_cache_delete_buffer(uint32_t id) {
    if (id != 0) {
        GLuint b = id;
        glDeleteBuffers(1, &b);
    }
}

static void gfx_opengl_cache_replay_begin(uint32_t id) {
    glBindBuffer(GL_ARRAY_BUFFER, id);
}

static uint32_t gfx_opengl_cache_create_palette(void) {
    if (gl_es || gl_glsl_version < 130) {
        // No VS palette path here (see shader codegen / palette_supported); 0 keeps
        // palette_ok false so gfx_pc falls back to re-record-on-dirty for lighting.
        return 0;
    }
    GLuint t = 0;
    glGenTextures(1, &t);
    return t;
}

// Allocated width per palette texture id, so refresh uploads of an unchanged
// size can use glTexSubImage2D instead of a full respecify + param set (A6).
// Entries are erased in cache_delete_palette — the only teardown path for ids
// minted by cache_create_palette (gfx_pc owns the pairing).
static std::unordered_map<uint32_t, int> s_palette_alloc;

static void gfx_opengl_cache_delete_palette(uint32_t id) {
    if (id != 0) {
        GLuint t = id;
        glDeleteTextures(1, &t);
        s_palette_alloc.erase(id);
        if (s_bound_texture[GFX_PALETTE_TEX_UNIT] == id) {
            s_bound_texture[GFX_PALETTE_TEX_UNIT] = 0; // GL reverts deleted bindings to 0
        }
    }
}

static void gfx_opengl_cache_upload_palette(uint32_t id, const void* rgba, int count) {
    if (id == 0 || count <= 0) {
        return;
    }
    glActiveTexture(GL_TEXTURE0 + GFX_PALETTE_TEX_UNIT);
    glBindTexture(GL_TEXTURE_2D, id);
    s_bound_texture[GFX_PALETTE_TEX_UNIT] = id;
    auto it = s_palette_alloc.find(id);
    if (it == s_palette_alloc.end() || it->second != count) {
        // first upload (or size change): allocate + set the immutable params
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, count, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        s_palette_alloc[id] = count;
    } else {
        // same size: refresh contents only, no respecify
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, count, 1, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    }
    glActiveTexture(GL_TEXTURE0);
    s_active_texture_unit = 0;
}

static void gfx_opengl_cache_bind_palette(uint32_t id, int count) {
    if (s_bound_texture[GFX_PALETTE_TEX_UNIT] != id) {
        glActiveTexture(GL_TEXTURE0 + GFX_PALETTE_TEX_UNIT);
        glBindTexture(GL_TEXTURE_2D, id);
        glActiveTexture(GL_TEXTURE0);
        s_bound_texture[GFX_PALETTE_TEX_UNIT] = id;
        s_active_texture_unit = 0;
    }
    const float w = (float)(count > 0 ? count : 1);
    if (w != gfx_current_palette_w) {
        gfx_current_palette_w = w;
        gfx_uniform_generation++;
        if (gfx_current_shader_program != NULL) {
            gfx_opengl_set_uniforms(gfx_current_shader_program);
        }
    }
}

static void gfx_opengl_cache_draw(struct ShaderProgram* prg, size_t base_float, size_t num_tris) {
    // Same attribute packing as buf_vbo (aVtxPos first, then tex/inputs), but the
    // position is object-space and the run starts at base_float in the bound cached
    // buffer. Cached vertices carry one extra float (the palette colour index) after
    // the normal layout, so the stride is num_floats + 1. The vertex shader applies
    // uMVP and (when enabled) the live palette lookup.
    const size_t stride = (prg->num_floats + 1) * sizeof(float);
    size_t pos = base_float;
    for (int i = 0; i < prg->num_attribs; i++) {
        if (prg->attrib_locations[i] >= 0) {
            glEnableVertexAttribArray(prg->attrib_locations[i]);
            glVertexAttribPointer(prg->attrib_locations[i], prg->attrib_sizes[i], GL_FLOAT, GL_FALSE,
                                  stride, (void*)(pos * sizeof(float)));
        }
        pos += prg->attrib_sizes[i];
    }
    if (prg->shade_idx_location >= 0) {
        glEnableVertexAttribArray(prg->shade_idx_location);
        glVertexAttribPointer(prg->shade_idx_location, 1, GL_FLOAT, GL_FALSE, stride,
                              (void*)((base_float + prg->num_floats) * sizeof(float)));
    }
    glDrawArrays(GL_TRIANGLES, 0, 3 * num_tris);
}

static void gfx_opengl_cache_set_cull(int mode, bool front_ccw) {
    if (mode == 0) {
        glDisable(GL_CULL_FACE);
        return;
    }
    glEnable(GL_CULL_FACE);
    glFrontFace(front_ccw ? GL_CCW : GL_CW);
    glCullFace(mode == 2 ? GL_FRONT : GL_BACK);
}

static void gfx_opengl_cache_replay_end(void) {
    // The immediate path culls on the CPU and expects opengl_vbo bound.
    glDisable(GL_CULL_FACE);
    glBindBuffer(GL_ARRAY_BUFFER, opengl_vbo);
}

typedef void (APIENTRY *DEBUGPROC)(GLenum source,
    GLenum type,
    GLuint id,
    GLenum severity,
    GLsizei length,
    const GLchar *message,
    const void *userParam);

static void APIENTRY gl_debug(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length, const GLchar *msg, const void *p) {
    sysLogPrintf(LOG_WARNING, "GL: (%05x) %s", id, msg);
}

static void gfx_opengl_enable_debug(void) {
    if (GLAD_GL_KHR_debug) {
        glEnable(GL_DEBUG_OUTPUT);
    }
    if (glDebugMessageControl != NULL) {
        // enable everything except some specific spam messages
        const GLuint disable[] = {
            0x20061, /* "Framebuffer detailed info" */
            0x20071  /* "Buffer detailed info" */
        };
        glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DONT_CARE, 0, NULL, GL_TRUE);
        glDebugMessageControl(GL_DEBUG_SOURCE_API, GL_DEBUG_TYPE_OTHER, GL_DONT_CARE, 2, disable, GL_FALSE);
    }
    if (glDebugMessageCallback != NULL) {
        glDebugMessageCallback(gl_debug, NULL);
    }
}

static bool gfx_opengl_supports_framebuffers(void) {
    if (GLVersion.major > 2) {
        // GL3.0+ supports everything we need, but we'll still check it for sanity
        return (glad_glFramebufferRenderbuffer && glad_glBlitFramebuffer && glad_glRenderbufferStorageMultisample);
    }
    if (GLAD_GL_ARB_framebuffer_object) {
        // some implementations might be missing these
        return (glad_glBlitFramebuffer && glad_glRenderbufferStorageMultisample);
    }
    if (GLAD_GL_EXT_framebuffer_object && GLAD_GL_EXT_framebuffer_blit && GLAD_GL_EXT_framebuffer_multisample) {
        // sanity check
        return (glad_glFramebufferRenderbuffer && glad_glBlitFramebuffer && glad_glRenderbufferStorageMultisample);
    }
    // nothing
    return false;
}

static bool gfx_opengl_supports_shaders(void) {
    if (GLVersion.major > 2) {
        // should support GLSL130+
        return true;
    }

    // check supported GLSL version
    const char *ver = (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION);
    if (ver) {
        int maj = 0, min = 0;
        sscanf(ver, "%d.%d", &maj, &min);
        if (maj > 1 || (maj == 1 && min > 20)) {
            // above 120, should be fine
            return true;
        }
    }

    // check for extension that adds textureSize
    return GLAD_GL_EXT_gpu_shader4;
}

static void gfx_opengl_log_info(void) {
    const char *version = (const char *)glGetString(GL_VERSION);
    const char *vendor = (const char *)glGetString(GL_VENDOR);
    const char *renderer = (const char *)glGetString(GL_RENDERER);
    const char *glsl_version = (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION);
    sysLogPrintf(LOG_NOTE, "GL: version: %s", version ? version : "unknown");
    sysLogPrintf(LOG_NOTE, "GL: vendor: %s", vendor ? vendor : "unknown");
    sysLogPrintf(LOG_NOTE, "GL: renderer: %s", renderer ? renderer : "unknown");
    sysLogPrintf(LOG_NOTE, "GL: GLSL version: %s", glsl_version ? glsl_version : "unknown");
    sysLogPrintf(LOG_NOTE, "GL: ARB_framebuffer_object: %s", gfx_opengl_supports_framebuffers() ? "yes" : "no");
    sysLogPrintf(LOG_NOTE, "GL: ARB_depth_clamp: %s", GLAD_GL_ARB_depth_clamp ? "yes" : "no");
    sysLogPrintf(LOG_NOTE, "GL: ARB_texture_mirror_clamp_to_edge: %s", GLAD_GL_ARB_texture_mirror_clamp_to_edge ? "yes" : "no");
}

static void *gl_load_proc(const char *name) {
    // SDL3 returns SDL_FunctionPointer, glad wants void*
    void *ret = reinterpret_cast<void *>(SDL_GL_GetProcAddress(name));
    if (ret) {
        return ret;
    }

    // try with postfixes
    static const char *post[] = { "ARB", "EXT" };
    char tmp[256] = { 0 };
    for (size_t i = 0; i < sizeof(post) / sizeof(*post); ++i) {
        snprintf(tmp, sizeof(tmp), "%s%s", name, post[i]);
        ret = reinterpret_cast<void *>(SDL_GL_GetProcAddress(tmp));
        if (ret) {
            return ret;
        }
    }

    sysLogPrintf(LOG_ERROR, "GL: could not find function: %s", name);

    return NULL;
}

static void gfx_opengl_init_extensions(void) {
    // patch up some extension values and pointers
    if (!GLAD_GL_ARB_depth_clamp) {
        if (GLAD_GL_EXT_depth_clamp || GLAD_GL_NV_depth_clamp) {
            GLAD_GL_ARB_depth_clamp = 1;
        } else if (!gl_es && GLVersion.major >= 3) {
            // GL3.2+ should have depth_clamp as part of the spec, but some devices don't report it for some reason
            GLAD_GL_ARB_depth_clamp = (GLVersion.major > 3 || (GLVersion.major == 3 && GLVersion.minor >= 2));
        }
    }

    if (!GLAD_GL_ARB_texture_mirror_clamp_to_edge) {
        GLAD_GL_ARB_texture_mirror_clamp_to_edge = GLAD_GL_EXT_texture_mirror_clamp_to_edge;
    }

    if (GLVersion.major < 3 && !GLAD_GL_ARB_framebuffer_object) {
        if (GLAD_GL_EXT_framebuffer_object && GLAD_GL_EXT_framebuffer_blit && GLAD_GL_EXT_framebuffer_multisample) {
            // because of the way glad works we'll have to copy the pointers over
            glad_glGenFramebuffers = glad_glGenFramebuffersEXT;
            glad_glGenRenderbuffers = glad_glGenRenderbuffersEXT;
            glad_glDeleteFramebuffers = glad_glDeleteFramebuffersEXT;
            glad_glDeleteRenderbuffers = glad_glDeleteRenderbuffersEXT;
            glad_glBindFramebuffer = glad_glBindFramebufferEXT;
            glad_glBindRenderbuffer = glad_glBindRenderbufferEXT;
            glad_glFramebufferRenderbuffer = glad_glFramebufferRenderbufferEXT;
            glad_glFramebufferTexture2D = glad_glFramebufferTexture2DEXT;
            glad_glRenderbufferStorage = glad_glRenderbufferStorageEXT;
            glad_glRenderbufferStorageMultisample = glad_glRenderbufferStorageMultisampleEXT;
            glad_glBlitFramebuffer = glad_glBlitFramebufferEXT;
        }
    }
}

static void gfx_opengl_init(void) {
    if (!gladLoadGLLoader(gl_load_proc) || glGetString == NULL || glEnable == NULL) {
        sysFatalError("Could not load OpenGL.\nReported SDL error: %s", SDL_GetError());
    }

    // check if we're using ES or core, which have more limited feature sets
    int val = 0;
    SDL_GL_GetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, &val);
    gl_core_profile = (val == SDL_GL_CONTEXT_PROFILE_CORE);
    gl_es = (val == SDL_GL_CONTEXT_PROFILE_ES);

    gfx_opengl_init_extensions();

    if (sysArgCheck("--debug-gl")) {
        gfx_opengl_enable_debug();
        // dump version info as early as possible
        gfx_opengl_log_info();
    }

    if (GLVersion.major < 2 || (GLVersion.major == 2 && GLVersion.minor < 1)) {
        const char *ver = (const char *)glGetString(GL_VERSION);
        sysFatalError("Could not load OpenGL 2.1.\nReported version: %d.%d (%s)",
            GLVersion.major, GLVersion.minor, ver ? ver : "unknown");
    }

    if (!gfx_opengl_supports_shaders()) {
        sysLogPrintf(LOG_WARNING, "GL: GLSL 1.30 may be unsupported");
        // maybe replace this with sysFatalError, though the GLSL compiler will cause that later anyway
    }

    if (!gfx_framebuffers_enabled) {
        sysLogPrintf(LOG_WARNING, "GL: framebuffer effects disabled by user");
    } else if (!gfx_opengl_supports_framebuffers()) {
        sysLogPrintf(LOG_WARNING, "GL: GL_ARB_framebuffer_object unsupported, framebuffer effects disabled");
        gfx_framebuffers_enabled = false;
    }

    if ((GLVersion.major < 4 || GLVersion.minor < 4) && !GLAD_GL_ARB_texture_mirror_clamp_to_edge) {
        // GL_MIRROR_CLAMP_TO_EDGE unsupported
        gl_mirror_clamp = GL_MIRRORED_REPEAT;
    }

    // determine GLSL version
    if (gl_es) {
        // ES has its own numbering scheme, but it should support 300 even on 3.1 and 3.2
        gl_glsl_version = 300;
        snprintf(gl_glsl_version_str, sizeof(gl_glsl_version_str), "%d es", gl_glsl_version);
    } else if (!gl_core_profile) {
        // in compat profile we can just request the lowest possible
        gl_glsl_version = 130;
        snprintf(gl_glsl_version_str, sizeof(gl_glsl_version_str), "%d", gl_glsl_version);
    } else {
        // otherwise we have to pick a specific version
        if (GLVersion.major == 3 && GLVersion.minor == 2) {
            // 3.2core is the earliest core version and it follows the old numbering scheme
            gl_glsl_version = 150;
        } else {
            // 3.3+ follow the new numbering scheme
            gl_glsl_version = GLVersion.major * 100 + GLVersion.minor * 10;
        }
        snprintf(gl_glsl_version_str, sizeof(gl_glsl_version_str), "%d core", gl_glsl_version);
    }
    sysLogPrintf(LOG_NOTE, "GL: using GLSL version %s", gl_glsl_version_str);

    glGenBuffers(1, &opengl_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, opengl_vbo);
    gfx_opengl_vtx_ring_init(); // may promote opengl_vbo into the persistent-mapped ring (A4)

    if (gl_core_profile || gl_es) {
        // warn user that funny things can happen
        sysLogPrintf(LOG_WARNING, "GL: using core profile or ES, watch out for errors");
        // core/es will explode if we don't use a VAO for our VBO
        glGenVertexArrays(1, &opengl_vao);
        glBindVertexArray(opengl_vao);
    } else if (glad_glGenVertexArrays && glad_glBindVertexArray) {
        // compat profile 3.0+ (the shipping Windows path): not required, but
        // keep attrib state in one VAO anyway — groundwork for VAO-based state
        // reuse, and drivers fast-path VAO-owned attribs (A3).
        glGenVertexArrays(1, &opengl_vao);
        glBindVertexArray(opengl_vao);
    }

    if (GLAD_GL_ARB_depth_clamp) {
        glEnable(GL_DEPTH_CLAMP);
    }
    glDepthFunc(GL_LEQUAL);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    if (!gl_es) {
        // Desktop shaders declare the uPalette vertex sampler even for immediate-mode
        // draws (the display-list cache palette), so keep a complete 1x1 texture bound
        // to the palette unit at all times — some drivers reject draws otherwise.
        // cache_bind_palette swaps in the real palette during replay.
        GLuint dummy = 0;
        glGenTextures(1, &dummy);
        glActiveTexture(GL_TEXTURE0 + GFX_PALETTE_TEX_UNIT);
        glBindTexture(GL_TEXTURE_2D, dummy);
        static const uint8_t white[4] = { 255, 255, 255, 255 };
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glActiveTexture(GL_TEXTURE0);
    }

    framebuffers.resize(1); // for the default screen buffer
}

static void gfx_opengl_on_resize(void) {
}

static void gfx_opengl_start_frame(void) {
    frame_count++;
    gfx_uniform_generation++; // frame_count feeds the noise shaders
    // External GL state may have changed between frames (SDL, overlays, other
    // passes) — force the redundant-state shadows to re-issue on first use.
    gfx_opengl_invalidate_state_shadows();
}

static void gfx_opengl_end_frame(void) {
    // No explicit glFlush: SDL_GL_SwapWindow performs an implicit flush, and
    // the original call carried no rationale (present since the initial import).
}

static void gfx_opengl_finish_render(void) {
}

static int gfx_opengl_create_framebuffer() {
    size_t i = framebuffers.size();
    framebuffers.resize(i + 1);

    GLuint clrbuf;
    glGenTextures(1, &clrbuf);
    glBindTexture(GL_TEXTURE_2D, clrbuf);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, 1, 1, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D, 0);
    gfx_opengl_invalidate_state_shadows(); // bound textures on whatever unit was active
    framebuffers[i].clrbuf = clrbuf;

    if (!gfx_framebuffers_enabled) {
        return i;
    }

    GLuint clrbuf_msaa;
    glGenRenderbuffers(1, &clrbuf_msaa);
    framebuffers[i].clrbuf_msaa = clrbuf_msaa;

    GLuint rbo;
    glGenRenderbuffers(1, &rbo);
    glBindRenderbuffer(GL_RENDERBUFFER, rbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, 1, 1);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);
    framebuffers[i].rbo = rbo;

    GLuint fbo;
    glGenFramebuffers(1, &fbo);
    framebuffers[i].fbo = fbo;

    return i;
}

static void gfx_opengl_update_framebuffer_parameters(int fb_id, uint32_t width, uint32_t height, uint32_t msaa_level,
                                                     bool opengl_invert_y, bool render_target, bool has_depth_buffer,
                                                     bool can_extract_depth) {
    Framebuffer& fb = framebuffers[fb_id];

    width = max(width, 1U);
    height = max(height, 1U);

    if (gfx_framebuffers_enabled) {
        glBindFramebuffer(GL_FRAMEBUFFER, fb.fbo);

        if (fb_id != 0) {
            if (fb.width != width || fb.height != height || fb.msaa_level != msaa_level) {
                if (msaa_level <= 1) {
                    glBindTexture(GL_TEXTURE_2D, fb.clrbuf);
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
                    glBindTexture(GL_TEXTURE_2D, 0);
                    gfx_opengl_invalidate_state_shadows(); // bound textures on whatever unit was active
                    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fb.clrbuf, 0);
                } else {
                    glBindRenderbuffer(GL_RENDERBUFFER, fb.clrbuf_msaa);
                    glRenderbufferStorageMultisample(GL_RENDERBUFFER, msaa_level, GL_RGB8, width, height);
                    glBindRenderbuffer(GL_RENDERBUFFER, 0);
                    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, fb.clrbuf_msaa);
                }
            }

            if (has_depth_buffer &&
                (fb.width != width || fb.height != height || fb.msaa_level != msaa_level || !fb.has_depth_buffer)) {
                glBindRenderbuffer(GL_RENDERBUFFER, fb.rbo);
                if (msaa_level <= 1) {
                    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
                } else {
                    glRenderbufferStorageMultisample(GL_RENDERBUFFER, msaa_level, GL_DEPTH24_STENCIL8, width, height);
                }
                glBindRenderbuffer(GL_RENDERBUFFER, 0);
            }

            if (!fb.has_depth_buffer && has_depth_buffer) {
                glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, fb.rbo);
            } else if (fb.has_depth_buffer && !has_depth_buffer) {
                glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, 0);
            }
        }
    } else {
        has_depth_buffer = false;
    }

    fb.width = width;
    fb.height = height;
    fb.has_depth_buffer = has_depth_buffer;
    fb.msaa_level = msaa_level;
    fb.invert_y = opengl_invert_y;
}

bool gfx_opengl_start_draw_to_framebuffer(int fb_id, float noise_scale) {
    if (gfx_framebuffers_enabled && fb_id < (int)framebuffers.size()) {
        Framebuffer& fb = framebuffers[fb_id];
        if (noise_scale != 0.0f) {
            const float ns = 1.0f / noise_scale;
            if (ns != current_noise_scale) {
                current_noise_scale = ns;
                gfx_uniform_generation++; // feeds the noise_scale uniform
            }
        }
        glBindFramebuffer(GL_FRAMEBUFFER, fb.fbo);
        current_framebuffer = fb_id;
        return true;
    } else {
        return false;
    }
}

void gfx_opengl_clear_framebuffer(bool clear_color, bool clear_depth) {
    glDisable(GL_SCISSOR_TEST);
    
    GLbitfield mask = 0;
    if (clear_color) {
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        mask |= GL_COLOR_BUFFER_BIT;
    }
    if (clear_depth) {
        glDepthMask(GL_TRUE);
        mask |= GL_DEPTH_BUFFER_BIT;
    }
    glClear(mask);
    if (clear_depth) {
        glDepthMask(current_depth_mask ? GL_TRUE : GL_FALSE);
    }

    glEnable(GL_SCISSOR_TEST);
}

void gfx_opengl_resolve_msaa_color_buffer(int fb_id_target, int fb_id_source) {
    if (!gfx_framebuffers_enabled) {
        return;
    }

    Framebuffer& fb_dst = framebuffers[fb_id_target];
    Framebuffer& fb_src = framebuffers[fb_id_source];
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fb_dst.fbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fb_src.fbo);
    glBlitFramebuffer(0, 0, fb_src.width, fb_src.height, 0, 0, fb_dst.width, fb_dst.height, GL_COLOR_BUFFER_BIT,
                      GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, current_framebuffer);
    glEnable(GL_SCISSOR_TEST);
}

void* gfx_opengl_get_framebuffer_texture_id(int fb_id) {
    return (void*)(uintptr_t)framebuffers[fb_id].clrbuf;
}

void gfx_opengl_select_texture_fb(int fb_id) {
    // glDisable(GL_DEPTH_TEST);
    const GLuint tex = framebuffers[fb_id].clrbuf;
    if (s_active_texture_unit != 0) {
        glActiveTexture(GL_TEXTURE0 + 0);
        s_active_texture_unit = 0;
    }
    if (s_bound_texture[0] != tex) {
        glBindTexture(GL_TEXTURE_2D, tex);
        s_bound_texture[0] = tex;
    }

    if (!current_textures_linear_filter[0]) {
        current_textures_linear_filter[0] = true;
        gfx_uniform_generation++; // feeds the three_point_filter uniforms
    }
}

void gfx_opengl_copy_framebuffer(int fb_dst, int fb_src, int left, int top, bool flip_y, bool use_back) {
    if (!gfx_framebuffers_enabled || fb_dst >= (int)framebuffers.size() || fb_src >= (int)framebuffers.size()) {
        return;
    }

    const Framebuffer& src = framebuffers[fb_src];
    const Framebuffer& dst = framebuffers[fb_dst];

    int srcX0, srcY0, srcX1, srcY1;
    int dstX0, dstY0, dstX1, dstY1;

    dstX0 = dstY0 = 0;
    dstX1 = dst.width;
    dstY1 = dst.height;

    if (left >= 0 && top >= 0) {
        // unscaled rect copy
        srcX0 = left;
        srcY0 = top;
        srcX1 = left + dst.width;
        srcY1 = top + dst.height;
    } else {
        // scaled full copy
        srcX0 = 0;
        srcY0 = 0;
        srcX1 = src.width;
        srcY1 = src.height;
    }

    glDisable(GL_SCISSOR_TEST);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, src.fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dst.fbo);

    if (flip_y) {
        // flip the dst rect to mirror the image vertically
        std::swap(dstY0, dstY1);
    }

    if (fb_src == 0) {
        // GLES does not support GL_FRONT here
        glReadBuffer((use_back || gl_es) ? GL_BACK : GL_FRONT);
    } else {
        glReadBuffer(GL_COLOR_ATTACHMENT0);
    }

    glBlitFramebuffer(srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1, GL_COLOR_BUFFER_BIT, GL_NEAREST);

    glBindFramebuffer(GL_FRAMEBUFFER, framebuffers[current_framebuffer].fbo);

    glReadBuffer(GL_BACK);

    glEnable(GL_SCISSOR_TEST);
}

void gfx_opengl_set_texture_filter(FilteringMode mode) {
    current_filter_mode = mode;
}

FilteringMode gfx_opengl_get_texture_filter(void) {
    return current_filter_mode;
}

void gfx_opengl_set_mipmap_filter(MipmapFilteringMode mode) {
    current_mipmap_filter_mode = mode;
}

static int gfx_opengl_get_max_anisotropy_level() {
	GLfloat max_aniso_level;
	glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &max_aniso_level);
	return (int)max_aniso_level;
}

static void gfx_opengl_set_anisotropy_level(int level) {
	current_anisotropy_level = level;
}

// Screen-space raytracing resolve (docs/PORT_RAYTRACING.md). gfx_rt.cpp does
// the work and saves/restores every piece of GL state it touches, so nothing
// the immediate-mode path has cached goes stale. Desktop GL 3.0+ only: the
// pipeline needs FBOs, depth captures and RGBA16F render targets.
static void gfx_opengl_rt_resolve(const void* cam, int vx, int vy, int vw, int vh) {
    if (gl_es || gl_glsl_version < 130 || !gfx_framebuffers_enabled) {
        return;
    }
    const Framebuffer& fb = framebuffers[current_framebuffer];
    // fb 0 is the default backbuffer: its fbo field stays 0 (= the default
    // framebuffer binding) and its dims are refreshed every frame by gfx_run's
    // update_framebuffer_parameters(0, ...) call.
    gfx_rt_resolve((const rtcamera*)cam, vx, vy, vw, vh, fb.fbo, (int)fb.width, (int)fb.height,
                   (int)(fb.msaa_level > 1 ? fb.msaa_level : 1), fb.invert_y, gl_glsl_version_str);
    // The RT passes bind textures/programs/blend behind the backend's back.
    // Their glGet save/restore keeps real GL state truthful, but drop the
    // redundant-state shadows anyway so the next draw re-issues rather than
    // trusting them (per-program uniforms are untouched by rt, so the uniform
    // caches stay valid).
    gfx_opengl_invalidate_state_shadows();
}

// Chaos retro/post filter (docs/PORT_CHAOS.md). gfx_retro.cpp does the work
// and saves/restores every piece of GL state it touches. Same desktop-GL
// gate as the RT resolve: the capture path needs FBOs + blits.
static void gfx_opengl_retro_filter(int pixw, int pixh, int cmode, int clevels, int fx, float warp) {
    if (gl_es || gl_glsl_version < 130 || !gfx_framebuffers_enabled) {
        return;
    }
    const Framebuffer& fb = framebuffers[current_framebuffer];
    gfx_retro_filter(pixw, pixh, cmode, clevels, fx, warp, fb.fbo, (int)fb.width, (int)fb.height,
                     gl_glsl_version_str);
    // Same reasoning as the rt_resolve wrapper: the pass touches texture/blend
    // state directly, so the shadows can't be trusted afterwards.
    gfx_opengl_invalidate_state_shadows();
}

// Barycentric wireframe needs gl_VertexID (GLSL 130+) and is pointless on GL ES,
// where glPolygonMode doesn't exist either. Below that we keep the old hardware
// line path, so nothing regresses on the Switch / ancient-driver targets.
static bool gfx_opengl_shader_wireframe_supported(void) {
    return !gl_es && gl_glsl_version >= 130;
}

struct GfxRenderingAPI gfx_opengl_api = {
    gfx_opengl_get_name,
    gfx_opengl_get_max_texture_size,
    gfx_opengl_get_clip_parameters,
    gfx_opengl_unload_shader,
    gfx_opengl_load_shader,
    gfx_opengl_create_and_load_new_shader,
    gfx_opengl_lookup_shader,
    gfx_opengl_shader_get_info,
    gfx_opengl_clear_shaders,
    gfx_opengl_new_texture,
    gfx_opengl_select_texture,
    gfx_opengl_upload_texture,
    gfx_opengl_set_sampler_parameters,
    gfx_opengl_set_depth_mode,
    gfx_opengl_set_depth_range,
    gfx_opengl_set_viewport,
    gfx_opengl_set_scissor,
    gfx_opengl_set_use_alpha,
    gfx_opengl_draw_triangles,
    gfx_opengl_init,
    gfx_opengl_on_resize,
    gfx_opengl_start_frame,
    gfx_opengl_end_frame,
    gfx_opengl_finish_render,
    gfx_opengl_create_framebuffer,
    gfx_opengl_update_framebuffer_parameters,
    gfx_opengl_start_draw_to_framebuffer,
    gfx_opengl_copy_framebuffer,
    gfx_opengl_clear_framebuffer,
    gfx_opengl_resolve_msaa_color_buffer,
    gfx_opengl_get_framebuffer_texture_id,
    gfx_opengl_select_texture_fb,
    gfx_opengl_delete_texture,
    gfx_opengl_set_texture_filter,
    gfx_opengl_get_texture_filter,
    gfx_opengl_set_mipmap_filter,
    gfx_opengl_set_anisotropy_level,
    gfx_opengl_get_max_anisotropy_level,
    gfx_opengl_set_mvp,
    gfx_opengl_cache_create_buffer,
    gfx_opengl_cache_delete_buffer,
    gfx_opengl_cache_replay_begin,
    gfx_opengl_cache_draw,
    gfx_opengl_cache_set_cull,
    gfx_opengl_cache_replay_end,
    gfx_opengl_set_fog_params,
    gfx_opengl_cache_create_palette,
    gfx_opengl_cache_delete_palette,
    gfx_opengl_cache_upload_palette,
    gfx_opengl_cache_bind_palette,
    gfx_opengl_set_palette_enable,
    gfx_opengl_set_shade_routing,
    gfx_opengl_rt_resolve,
    gfx_opengl_retro_filter,
    gfx_opengl_shader_wireframe_supported
};
