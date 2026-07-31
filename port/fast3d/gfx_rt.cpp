/**
 * Screen-space raytracing suite (port-only; docs/PORT_RAYTRACING.md).
 *
 * Runs as a post pass at the G_RTRESOLVE_EXT display-list marker (emitted at
 * the top of playerRenderHud: world + props are rendered and the depth buffer
 * is still intact — bgunRender clears depth right after, so the viewmodel is
 * deliberately excluded from the capture and composited over untouched).
 *
 * Pipeline per resolve (per local-player viewport):
 *   1. blit colour+depth from the game framebuffer into RT-owned textures
 *      (GL_RGB8 / GL_DEPTH24_STENCIL8 to keep MSAA resolve blits legal)
 *   2. prepass: reconstruct view-space normals + linear depth (RGBA16F)
 *   3. AO + sun-shadow trace (hemisphere occlusion rays + a directional
 *      screen-space shadow march), bilateral-blurred
 *   4. GI: single-bounce SSGI or stochastic multi-bounce path trace,
 *      temporally accumulated with depth-validated reprojection
 *      (per-player history so split-screen doesn't cross-feed)
 *   5. SSR: raymarched reflections with binary refinement + fresnel weight
 *   6. composite back over the game framebuffer as a multiplicative
 *      (AO * shadow) quad + an additive (GI + SSR) quad — blending, so
 *      per-sample MSAA edge colour survives
 *
 * Everything is reconstructed from depth: no G-buffer, no material data.
 * All GL state touched here is saved with glGet* on entry and restored on
 * exit, so the immediate-mode renderer's cached state stays truthful.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "glad/glad.h"
#include "gfx_opengl.h"
#include "gfx_rt.h"
#include "gfx_rt_common.h" // shared pass bodies, quality table, matrix helpers

// logging (values from port/include/system.h; keep the dependency thin —
// pulling system.h in would drag PR/ultratypes into the renderer)
extern "C" void sysLogPrintf(int level, const char* fmt, ...);
#define RT_LOG_NOTE 0                 // LOG_NOTE
#define RT_LOG_ERROR (2 | (1 << 7))   // LOG_ERROR (2 | LOGFLAG_SHOWMSG)


// ---------------------------------------------------------------------------
// state

static bool s_broken = false;      // a shader failed to build; suite disabled
static bool s_inited = false;
static int s_fbw = 0, s_fbh = 0;   // size the resources were built for
static int s_giw = 0, s_gih = 0;
static float s_giscale = 0.0f;
static unsigned int s_frame = 0;

// fullscreen triangle
static GLuint s_vao = 0, s_vbo = 0;

// full-res targets
static GLuint s_scene_fbo = 0, s_scene_col = 0, s_scene_dep = 0;
// depth capture for the default-framebuffer case: depth blits demand exactly
// matching formats, and the window backbuffer's depth format is driver-chosen,
// so fbo==0 captures via glCopyTexSubImage2D into a plain DEPTH_COMPONENT24
// texture instead (format conversion is legal for copies, not blits).
static GLuint s_scene_dep_copy = 0;
static GLuint s_norm_fbo = 0, s_norm_tex = 0;
static GLuint s_ao_fbo = 0, s_ao_tex = 0;
static GLuint s_aotmp_fbo = 0, s_aotmp_tex = 0;
static GLuint s_ssr_fbo = 0, s_ssr_tex = 0;
static GLuint s_light_fbo = 0, s_light_tex = 0; // dynamic-light radiance (dark mode)

// GI-res targets (gfx_rt_gi_scale)
static GLuint s_gitrace_fbo = 0, s_gitrace_tex = 0;
static GLuint s_gitmp_fbo = 0, s_gitmp_tex = 0;
static GLuint s_gifinal_fbo = 0, s_gifinal_tex = 0;
// per-player temporal history ping-pong (lazily created)
static GLuint s_hist_fbo[RT_MAX_PLAYERS][2];
static GLuint s_hist_tex[RT_MAX_PLAYERS][2];
static int s_hist_idx[RT_MAX_PLAYERS];

// per-player previous-frame camera (for reprojection)
static bool s_prev_valid[RT_MAX_PLAYERS];
static float s_prev_view[RT_MAX_PLAYERS][16];
static float s_prev_fovy[RT_MAX_PLAYERS], s_prev_aspect[RT_MAX_PLAYERS];
static float s_prev_znear[RT_MAX_PLAYERS], s_prev_zfar[RT_MAX_PLAYERS];

// programs
enum {
    PROG_PREPASS,
    PROG_AOSHADOW,
    PROG_TRACE,     // SSGI / path trace (uBounces selects)
    PROG_TEMPORAL,
    PROG_BLUR,
    PROG_SSR,
    PROG_LIGHT,     // dynamic lights + torch (dark/relight mode)
    PROG_COMP_MUL,
    PROG_COMP_ADD,
    PROG_DEBUG,
    PROG_COUNT
};
static GLuint s_prog[PROG_COUNT];

// Every uniform name rtU is queried with, resolved once per program at init
// (the sampler-loop pattern in rtLink) instead of glGetUniformLocation by
// string at draw time (A20). GLSL-side names are unchanged; the X-macro keeps
// the enum and the name table in sync.
#define RT_UNIFORM_LIST(X) \
    X(uRect) X(uProj) X(uYSign) X(uTexel) X(uFrame) \
    X(uAOOn) X(uShadowOn) X(uAOSamples) X(uShadowSteps) \
    X(uAORadius) X(uShadowLen) X(uSun) X(uDir) \
    X(uRays) X(uSteps) X(uBounces) X(uGIRadius) X(uSky) \
    X(uCurToPrev) X(uBlend) X(uSSRSteps) X(uMaxDist) \
    X(uLightCount) X(uLightPosRad) X(uLightCol) X(uLightShadows) \
    X(uLightSteps) X(uTorch) X(uTorchInt) X(uTorchRange) X(uLightMax) \
    X(uMode) X(uAOInt) X(uShInt) X(uDark) X(uDarkAmbient) X(uAmbientCol) \
    X(uGIOn) X(uSSROn) X(uLightsOn) X(uGIInt) X(uSSRInt) X(uRelight)

enum {
#define RT_U_ENUM(n) RTU_##n,
    RT_UNIFORM_LIST(RT_U_ENUM)
#undef RT_U_ENUM
    RTU_COUNT
};

static const char* kRtUniformNames[RTU_COUNT] = {
#define RT_U_NAME(n) #n,
    RT_UNIFORM_LIST(RT_U_NAME)
#undef RT_U_NAME
};

// per-program cached locations, filled in rtInit once all programs are built
static GLint s_uloc[PROG_COUNT][RTU_COUNT];

// (quality presets + matrix helpers come from gfx_rt_common.h)

// ---------------------------------------------------------------------------
// GL state save/restore — everything the passes touch

struct RtGLState {
    GLint draw_fbo, read_fbo;
    GLint viewport[4];
    GLint scissor_box[4];
    GLboolean scissor_test, depth_test, blend, cull;
    GLboolean depth_mask;
    GLint depth_func;
    GLint blend_src_rgb, blend_dst_rgb, blend_src_a, blend_dst_a;
    GLint program;
    GLint active_texture;
    GLint tex_binding[8]; // units 0..7
    GLint vao, array_buffer;
};

static void rtSaveState(RtGLState* s) {
    // A20: filled from gfx_opengl's CPU-side value shadows — zero glGet*
    // round-trips (this used to issue ~24 queries per resolve per player,
    // including 8 glActiveTexture+glGet pairs). The restore below is
    // unchanged, so the state put BACK is exactly what the shadows say the
    // game path had — the same values the queries would have returned at
    // this flush boundary. See gfx_opengl_get_rt_state for the validity
    // argument per field.
    GfxGlRtState g;
    gfx_opengl_get_rt_state(&g);

    s->draw_fbo = g.draw_fbo;
    s->read_fbo = g.read_fbo;
    for (int i = 0; i < 4; i++) {
        s->viewport[i] = g.viewport[i];
        s->scissor_box[i] = g.scissor_box[i];
    }
    s->scissor_test = g.scissor_test;
    s->depth_test = g.depth_test;
    s->blend = g.blend;
    s->cull = g.cull;
    s->depth_mask = g.depth_mask;
    s->depth_func = g.depth_func;
    s->blend_src_rgb = g.blend_src_rgb;
    s->blend_dst_rgb = g.blend_dst_rgb;
    s->blend_src_a = g.blend_src_a;
    s->blend_dst_a = g.blend_dst_a;
    s->program = g.program;
    s->active_texture = g.active_texture;
    for (int i = 0; i < 8; i++) {
        s->tex_binding[i] = g.tex_binding[i];
    }
    s->vao = g.vao;
    s->array_buffer = g.array_buffer;
}

static void rtRestoreState(const RtGLState* s) {
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s->draw_fbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, s->read_fbo);
    glViewport(s->viewport[0], s->viewport[1], s->viewport[2], s->viewport[3]);
    glScissor(s->scissor_box[0], s->scissor_box[1], s->scissor_box[2], s->scissor_box[3]);
    if (s->scissor_test) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    if (s->depth_test) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (s->blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (s->cull) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    glDepthMask(s->depth_mask);
    glDepthFunc(s->depth_func);
    glBlendFuncSeparate(s->blend_src_rgb, s->blend_dst_rgb, s->blend_src_a, s->blend_dst_a);
    for (int i = 7; i >= 0; i--) {
        glActiveTexture(GL_TEXTURE0 + i);
        glBindTexture(GL_TEXTURE_2D, s->tex_binding[i]);
    }
    glActiveTexture(s->active_texture);
    glUseProgram(s->program);
    if (glad_glBindVertexArray) {
        glBindVertexArray(s->vao);
    }
    glBindBuffer(GL_ARRAY_BUFFER, s->array_buffer);
}

// ---------------------------------------------------------------------------
// shaders

// Shared vertex shader: fullscreen triangle; vUV spans the viewport rect in
// full-texture UV space (all RT textures are framebuffer-normalized).
static const char* kVS =
    "IN vec2 aPos;\n"
    "OUT vec2 vUV;\n"
    "uniform vec4 uRect;\n"
    "void main() {\n"
    "    vec2 t = aPos * 0.5 + 0.5;\n"
    "    vUV = mix(uRect.xy, uRect.zw, t);\n"
    "    gl_Position = vec4(aPos, 0.0, 1.0);\n"
    "}\n";

// Shared fragment prelude: every sampler + uniform any pass body references,
// declared loose (GLSL 130). Unused declarations are optimized out (location
// -1, skipped by rtU); same-type samplers sharing a texture unit is legal GL.
// The reconstruction helpers + the pass bodies themselves live in
// gfx_rt_common.h, shared with the SDL_GPU backend.
static const char* kFSCommon =
    "IN vec2 vUV;\n"
    "OUT vec4 oCol;\n"
    "uniform sampler2D uDepth;\n"
    "uniform sampler2D uNorm;\n"  // xyz = view normal, w = linear depth (0 = sky)
    "uniform sampler2D uColor;\n" // scene colour
    "uniform sampler2D uCur;\n"
    "uniform sampler2D uHist;\n"
    "uniform sampler2D uSrc;\n"
    "uniform sampler2D uAO;\n"
    "uniform sampler2D uGI;\n"
    "uniform sampler2D uSSR;\n"
    "uniform vec4 uProj;\n"
    "uniform vec4 uRect;\n"
    "uniform float uYSign;\n"
    "uniform vec2 uTexel;\n"
    "uniform vec2 uDir;\n"
    "uniform int uFrame;\n"
    "uniform mat4 uCurToPrev;\n"
    "uniform vec3 uSun;\n" // view-space, toward the light, normalized
    "uniform vec3 uSky;\n"
    "uniform float uBlend;\n"
    "uniform float uAORadius;\n"
    "uniform float uShadowLen;\n"
    "uniform float uGIRadius;\n"
    "uniform float uMaxDist;\n"
    "uniform float uAOInt;\n"
    "uniform float uShInt;\n"
    "uniform float uGIInt;\n"
    "uniform float uSSRInt;\n"
    "uniform int uAOOn;\n"
    "uniform int uShadowOn;\n"
    "uniform int uGIOn;\n"
    "uniform int uSSROn;\n"
    "uniform int uMode;\n"
    "uniform int uAOSamples;\n"
    "uniform int uShadowSteps;\n"
    "uniform int uRays;\n"
    "uniform int uSteps;\n"
    "uniform int uBounces;\n"
    "uniform int uSSRSteps;\n"
    // dark/relight mode: dynamic lights (view-space, premultiplied colours),
    // the camera torch, and the composite darkening controls
    "uniform sampler2D uLight;\n"
    "uniform vec4 uLightPosRad[64];\n" // 64 == RT_MAX_LIGHTS
    "uniform vec4 uLightCol[64];\n"
    "uniform int uLightCount;\n"
    "uniform int uLightShadows;\n"
    "uniform int uLightSteps;\n"
    "uniform int uTorch;\n"
    "uniform float uTorchInt;\n"
    "uniform float uTorchRange;\n"
    "uniform int uDark;\n"
    "uniform float uDarkAmbient;\n"
    "uniform int uLightsOn;\n"
    "uniform float uLightMax;\n"   // per-light brightness ceiling (hue-preserving)
    "uniform vec3 uAmbientCol;\n"  // skylight tint for the dark ambient
    "uniform float uRelight;\n";   // 0..1 wall relight (fades GI/SSR as it rises)

// ---------------------------------------------------------------------------
// shader building

static GLuint rtCompile(GLenum type, const char* version, const char* body, bool is_fs) {
    char* src = (char*)malloc(strlen(kFSCommon) + strlen(RT_GLSL_HELPERS) + strlen(body) + 1024);
    char* p = src;
    p += sprintf(p, "#version %s\n", version);
    // GLSL 130+: in/out everywhere. Aliased so the sources stay single strings.
    p += sprintf(p, "#define IN in\n#define OUT out\n");
    if (is_fs) {
        strcpy(p, kFSCommon);
        p += strlen(kFSCommon);
        strcpy(p, RT_GLSL_HELPERS);
        p += strlen(RT_GLSL_HELPERS);
    }
    strcpy(p, body);

    GLuint sh = glCreateShader(type);
    const char* s = src;
    glShaderSource(sh, 1, &s, NULL);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(sh, sizeof(log), NULL, log);
        sysLogPrintf(RT_LOG_ERROR, "RT: shader compile failed: %s", log);
        glDeleteShader(sh);
        sh = 0;
    }
    free(src);
    return sh;
}

static GLuint rtLink(const char* version, const char* fs_body) {
    GLuint vs = rtCompile(GL_VERTEX_SHADER, version, kVS, false);
    GLuint fs = rtCompile(GL_FRAGMENT_SHADER, version, fs_body, true);
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return 0;
    }
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glBindAttribLocation(prog, 0, "aPos");
    if (glad_glBindFragDataLocation) {
        glBindFragDataLocation(prog, 0, "oCol");
    }
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(prog, sizeof(log), NULL, log);
        sysLogPrintf(RT_LOG_ERROR, "RT: program link failed: %s", log);
        glDeleteProgram(prog);
        return 0;
    }
    // fixed sampler unit assignments (set once)
    glUseProgram(prog);
    const struct { const char* name; int unit; } samplers[] = {
        { "uDepth", 5 }, { "uNorm", 3 }, { "uColor", 4 },
        { "uCur", 0 },   { "uHist", 6 }, { "uSrc", 0 },
        { "uAO", 0 },    { "uGI", 1 },   { "uSSR", 2 },
        { "uLight", 7 },
    };
    for (size_t i = 0; i < sizeof(samplers) / sizeof(samplers[0]); i++) {
        GLint loc = glGetUniformLocation(prog, samplers[i].name);
        if (loc >= 0) glUniform1i(loc, samplers[i].unit);
    }
    return prog;
}

// ---------------------------------------------------------------------------
// resources

static GLuint rtMakeTex(GLenum internal, GLenum format, GLenum type, int w, int h, GLenum filter) {
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, internal, w, h, 0, format, type, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return tex;
}

static GLuint rtMakeFbo(GLuint colortex) {
    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colortex, 0);
    return fbo;
}

static void rtDeleteTexFbo(GLuint* fbo, GLuint* tex) {
    if (*fbo) { glDeleteFramebuffers(1, fbo); *fbo = 0; }
    if (*tex) { glDeleteTextures(1, tex); *tex = 0; }
}

static void rtFreeTargets(void) {
    rtDeleteTexFbo(&s_norm_fbo, &s_norm_tex);
    rtDeleteTexFbo(&s_ao_fbo, &s_ao_tex);
    rtDeleteTexFbo(&s_aotmp_fbo, &s_aotmp_tex);
    rtDeleteTexFbo(&s_ssr_fbo, &s_ssr_tex);
    rtDeleteTexFbo(&s_light_fbo, &s_light_tex);
    rtDeleteTexFbo(&s_gitrace_fbo, &s_gitrace_tex);
    rtDeleteTexFbo(&s_gitmp_fbo, &s_gitmp_tex);
    rtDeleteTexFbo(&s_gifinal_fbo, &s_gifinal_tex);
    for (int p = 0; p < RT_MAX_PLAYERS; p++) {
        for (int i = 0; i < 2; i++) {
            rtDeleteTexFbo(&s_hist_fbo[p][i], &s_hist_tex[p][i]);
        }
        s_prev_valid[p] = false;
    }
    if (s_scene_fbo) { glDeleteFramebuffers(1, &s_scene_fbo); s_scene_fbo = 0; }
    if (s_scene_col) { glDeleteTextures(1, &s_scene_col); s_scene_col = 0; }
    if (s_scene_dep) { glDeleteTextures(1, &s_scene_dep); s_scene_dep = 0; }
    if (s_scene_dep_copy) { glDeleteTextures(1, &s_scene_dep_copy); s_scene_dep_copy = 0; }
}

static bool rtBuildTargets(int fbw, int fbh, float giscale) {
    rtFreeTargets();

    s_giw = (int)(fbw * giscale); if (s_giw < 1) s_giw = 1;
    s_gih = (int)(fbh * giscale); if (s_gih < 1) s_gih = 1;

    // scene copy: colour must be GL_RGB8 and depth GL_DEPTH24_STENCIL8 to
    // exactly match the game framebuffer formats — an MSAA resolve blit
    // requires identical internal formats.
    s_scene_col = rtMakeTex(GL_RGB8, GL_RGB, GL_UNSIGNED_BYTE, fbw, fbh, GL_LINEAR);
    glGenTextures(1, &s_scene_dep);
    glBindTexture(GL_TEXTURE_2D, s_scene_dep);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, fbw, fbh, 0, GL_DEPTH_STENCIL,
                 GL_UNSIGNED_INT_24_8, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &s_scene_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, s_scene_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_scene_col, 0);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, s_scene_dep, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        sysLogPrintf(RT_LOG_ERROR, "RT: scene framebuffer incomplete");
        return false;
    }

    glGenTextures(1, &s_scene_dep_copy);
    glBindTexture(GL_TEXTURE_2D, s_scene_dep_copy);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, fbw, fbh, 0, GL_DEPTH_COMPONENT,
                 GL_UNSIGNED_INT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    s_norm_tex = rtMakeTex(GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, fbw, fbh, GL_NEAREST);
    s_norm_fbo = rtMakeFbo(s_norm_tex);
    s_ao_tex = rtMakeTex(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, fbw, fbh, GL_LINEAR);
    s_ao_fbo = rtMakeFbo(s_ao_tex);
    s_aotmp_tex = rtMakeTex(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, fbw, fbh, GL_LINEAR);
    s_aotmp_fbo = rtMakeFbo(s_aotmp_tex);
    s_ssr_tex = rtMakeTex(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, fbw, fbh, GL_LINEAR);
    s_ssr_fbo = rtMakeFbo(s_ssr_tex);
    s_light_tex = rtMakeTex(GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, fbw, fbh, GL_LINEAR);
    s_light_fbo = rtMakeFbo(s_light_tex);

    s_gitrace_tex = rtMakeTex(GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, s_giw, s_gih, GL_LINEAR);
    s_gitrace_fbo = rtMakeFbo(s_gitrace_tex);
    s_gitmp_tex = rtMakeTex(GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, s_giw, s_gih, GL_LINEAR);
    s_gitmp_fbo = rtMakeFbo(s_gitmp_tex);
    s_gifinal_tex = rtMakeTex(GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, s_giw, s_gih, GL_LINEAR);
    s_gifinal_fbo = rtMakeFbo(s_gifinal_tex);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        sysLogPrintf(RT_LOG_ERROR, "RT: render target incomplete (RGBA16F unsupported?)");
        return false;
    }

    s_fbw = fbw;
    s_fbh = fbh;
    s_giscale = giscale;
    return true;
}

// per-player history is created lazily (single player never pays for 4 sets)
static bool rtEnsureHistory(int player) {
    if (s_hist_tex[player][0]) {
        return true;
    }
    for (int i = 0; i < 2; i++) {
        s_hist_tex[player][i] = rtMakeTex(GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, s_giw, s_gih, GL_LINEAR);
        s_hist_fbo[player][i] = rtMakeFbo(s_hist_tex[player][i]);
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f); // a=0 -> "no history" to the validator
        glClear(GL_COLOR_BUFFER_BIT);
    }
    s_hist_idx[player] = 0;
    return true;
}

static bool rtInit(const char* glsl_version) {
    static const char* names[PROG_COUNT] = {
        "prepass", "aoshadow", "trace", "temporal", "blur", "ssr", "light", "comp_mul", "comp_add", "debug"
    };
    const char* bodies[PROG_COUNT] = {
        RT_FS_PREPASS_BODY, RT_FS_AOSHADOW_BODY, RT_FS_TRACE_BODY, RT_FS_TEMPORAL_BODY,
        RT_FS_BLUR_BODY, RT_FS_SSR_BODY, RT_FS_LIGHT_BODY, RT_FS_COMP_MUL_BODY, RT_FS_COMP_ADD_BODY,
        RT_FS_DEBUG_BODY,
    };
    for (int i = 0; i < PROG_COUNT; i++) {
        s_prog[i] = rtLink(glsl_version, bodies[i]);
        if (!s_prog[i]) {
            sysLogPrintf(RT_LOG_ERROR, "RT: failed to build '%s' — raytracing disabled", names[i]);
            return false;
        }
    }

    // resolve every uniform location once; rtU serves these from here on
    for (int i = 0; i < PROG_COUNT; i++) {
        for (int u = 0; u < RTU_COUNT; u++) {
            s_uloc[i][u] = glGetUniformLocation(s_prog[i], kRtUniformNames[u]);
        }
    }

    // fullscreen triangle
    static const float verts[6] = { -1.0f, -1.0f, 3.0f, -1.0f, -1.0f, 3.0f };
    if (glad_glGenVertexArrays) {
        glGenVertexArrays(1, &s_vao);
        glBindVertexArray(s_vao);
    }
    glGenBuffers(1, &s_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, (void*)0);

    sysLogPrintf(RT_LOG_NOTE, "RT: raytracing suite initialized (GLSL %s)", glsl_version);
    return true;
}

// ---------------------------------------------------------------------------
// per-pass helpers

static GLint rtU(GLuint prog, int u) {
    // cached lookup: locations were resolved once per program in rtInit
    for (int i = 0; i < PROG_COUNT; i++) {
        if (s_prog[i] == prog) {
            return s_uloc[i][u];
        }
    }
    return -1;
}

// per-frame constants shared by every pass
struct RtFrame {
    float rect[4];      // viewport in full-texture UV: minx,miny,maxx,maxy
    float proj[4];      // tanHalfFovX, tanHalfFovY, znear, zfar
    float ysign;
    float texel[2];
    int frame;
};

static void rtSetCommon(GLuint prog, const RtFrame* f) {
    glUseProgram(prog);
    GLint loc;
    if ((loc = rtU(prog, RTU_uRect)) >= 0) glUniform4fv(loc, 1, f->rect);
    if ((loc = rtU(prog, RTU_uProj)) >= 0) glUniform4fv(loc, 1, f->proj);
    if ((loc = rtU(prog, RTU_uYSign)) >= 0) glUniform1f(loc, f->ysign);
    if ((loc = rtU(prog, RTU_uTexel)) >= 0) glUniform2fv(loc, 1, f->texel);
    if ((loc = rtU(prog, RTU_uFrame)) >= 0) glUniform1i(loc, f->frame);
}

static void rtDraw(void) {
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

static void rtBindTex(int unit, GLuint tex) {
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D, tex);
}

// ---------------------------------------------------------------------------
// the resolve

void gfx_rt_resolve(const rtcamera* cam, int vx, int vy, int vw, int vh,
                    unsigned int fbo, int fbw, int fbh, int msaa,
                    bool invert_y, const char* glsl_version) {
    (void)msaa; // formats are chosen so the capture blit resolves MSAA implicitly
    if (s_broken || !cam || !cam->valid || fbw <= 0 || fbh <= 0) {
        return;
    }
    if (cam->playernum < 0 || cam->playernum >= RT_MAX_PLAYERS) {
        return;
    }
    if (vw <= 0 || vh <= 0) {
        vx = 0; vy = 0; vw = fbw; vh = fbh;
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

    RtGLState saved;
    rtSaveState(&saved);

    if (!s_inited) {
        s_inited = true;
        if (!rtInit(glsl_version)) {
            s_broken = true;
            gfx_rt_enabled = 0;
            rtRestoreState(&saved);
            return;
        }
    }

    float giscale = gfx_rt_gi_scale;
    if (giscale < 0.25f) giscale = 0.25f;
    if (giscale > 1.0f) giscale = 1.0f;
    if (s_fbw != fbw || s_fbh != fbh || s_giscale != giscale) {
        if (!rtBuildTargets(fbw, fbh, giscale)) {
            s_broken = true;
            gfx_rt_enabled = 0;
            rtRestoreState(&saved);
            return;
        }
    }

    const int pl = cam->playernum;
    s_frame++;

    // neutral raster state for the passes
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    if (s_vao) {
        glBindVertexArray(s_vao);
    } else {
        glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, (void*)0);
    }

    // 1. capture colour + depth. Two paths:
    //    - game FBO source: blit (also resolves MSAA; RGB8/D24S8 formats match
    //      the game framebuffer by construction, as depth blits require)
    //    - default framebuffer source (fbo == 0, the no-MSAA/no-scale case):
    //      the backbuffer's depth format is driver-chosen, so blitting depth is
    //      illegal in general — copy through glCopyTexSubImage2D instead
    GLuint depth_tex = s_scene_dep;
    if (fbo != 0) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_scene_fbo);
        glBlitFramebuffer(0, 0, fbw, fbh, 0, 0, fbw, fbh,
                          GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    } else {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, s_scene_col);
        glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, fbw, fbh);
        glBindTexture(GL_TEXTURE_2D, s_scene_dep_copy);
        glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, fbw, fbh);
        depth_tex = s_scene_dep_copy;
    }

    // clamp rect
    if (vx < 0) { vw += vx; vx = 0; }
    if (vy < 0) { vh += vy; vy = 0; }
    if (vx + vw > fbw) vw = fbw - vx;
    if (vy + vh > fbh) vh = fbh - vy;
    if (vw <= 0 || vh <= 0) {
        rtRestoreState(&saved);
        return;
    }

    RtFrame fr;
    fr.rect[0] = (float)vx / fbw;
    fr.rect[1] = (float)vy / fbh;
    fr.rect[2] = (float)(vx + vw) / fbw;
    fr.rect[3] = (float)(vy + vh) / fbh;
    const float fovy_rad = cam->fovy * (float)(3.14159265358979 / 180.0);
    const float thfy = tanf(fovy_rad * 0.5f);
    fr.proj[0] = thfy * cam->aspect;
    fr.proj[1] = thfy;
    fr.proj[2] = cam->znear > 0.1f ? cam->znear : 0.1f;
    fr.proj[3] = cam->zfar > fr.proj[2] + 1.0f ? cam->zfar : fr.proj[2] + 1.0f;
    fr.ysign = invert_y ? -1.0f : 1.0f;
    fr.texel[0] = 1.0f / fbw;
    fr.texel[1] = 1.0f / fbh;
    fr.frame = (int)(s_frame & 0xffff);

    const int q = (gfx_rt_quality < 0) ? 0 : (gfx_rt_quality > 2 ? 2 : gfx_rt_quality);

    // shared inputs
    rtBindTex(3, s_norm_tex);
    rtBindTex(4, s_scene_col);
    rtBindTex(5, depth_tex);

    // 2. prepass: normals + linear depth
    glBindFramebuffer(GL_FRAMEBUFFER, s_norm_fbo);
    glViewport(vx, vy, vw, vh);
    rtSetCommon(s_prog[PROG_PREPASS], &fr);
    rtDraw();

    // 3. AO + shadow trace, then separable bilateral blur
    const bool aosh = ao_on || sh_on || dbg == RT_DEBUG_AO || dbg == RT_DEBUG_SHADOW;
    if (aosh) {
        // sun direction: world -> view (rotation only), normalized. Auto-sun
        // (the stage's lens-flare sun, per-camera) wins over the manual dir.
        float sw[3];
        if (gfx_rt_autosun && cam->sun_ok) {
            sw[0] = cam->sundir[0]; // already normalized game-side
            sw[1] = cam->sundir[1];
            sw[2] = cam->sundir[2];
        } else {
            sw[0] = gfx_rt_sun_dir[0];
            sw[1] = gfx_rt_sun_dir[1];
            sw[2] = gfx_rt_sun_dir[2];
            float sl = sqrtf(sw[0] * sw[0] + sw[1] * sw[1] + sw[2] * sw[2]);
            if (sl < 0.0001f) { sw[0] = 0.0f; sw[1] = 1.0f; sw[2] = 0.0f; sl = 1.0f; }
            sw[0] /= sl; sw[1] /= sl; sw[2] /= sl;
        }
        const float* m = cam->viewmtx;
        float sv[3] = {
            m[0] * sw[0] + m[4] * sw[1] + m[8] * sw[2],
            m[1] * sw[0] + m[5] * sw[1] + m[9] * sw[2],
            m[2] * sw[0] + m[6] * sw[1] + m[10] * sw[2],
        };

        glBindFramebuffer(GL_FRAMEBUFFER, s_ao_fbo);
        glViewport(vx, vy, vw, vh);
        GLuint pr = s_prog[PROG_AOSHADOW];
        rtSetCommon(pr, &fr);
        glUniform1i(rtU(pr, RTU_uAOOn), (ao_on || dbg == RT_DEBUG_AO) ? 1 : 0);
        glUniform1i(rtU(pr, RTU_uShadowOn), (sh_on || dbg == RT_DEBUG_SHADOW) ? 1 : 0);
        glUniform1i(rtU(pr, RTU_uAOSamples), kQuality[q].ao_samples);
        glUniform1i(rtU(pr, RTU_uShadowSteps), kQuality[q].shadow_steps);
        glUniform1f(rtU(pr, RTU_uAORadius), gfx_rt_ao_radius);
        glUniform1f(rtU(pr, RTU_uShadowLen), gfx_rt_shadow_length);
        glUniform3fv(rtU(pr, RTU_uSun), 1, sv);
        rtDraw();

        // blur H: ao -> aotmp, blur V: aotmp -> ao
        pr = s_prog[PROG_BLUR];
        rtSetCommon(pr, &fr);
        glBindFramebuffer(GL_FRAMEBUFFER, s_aotmp_fbo);
        rtBindTex(0, s_ao_tex);
        glUniform2f(rtU(pr, RTU_uDir), fr.texel[0], 0.0f);
        rtDraw();
        glBindFramebuffer(GL_FRAMEBUFFER, s_ao_fbo);
        rtBindTex(0, s_aotmp_tex);
        glUniform2f(rtU(pr, RTU_uDir), 0.0f, fr.texel[1]);
        rtDraw();
    }

    // 4. GI / path trace at reduced res + temporal accumulation + blur
    const bool gi_run = gi_mode != RT_GI_OFF || dbg == RT_DEBUG_GI;
    if (gi_run) {
        rtEnsureHistory(pl);

        const int givx = (int)(vx * s_giscale), givy = (int)(vy * s_giscale);
        int givw = (int)(vw * s_giscale), givh = (int)(vh * s_giscale);
        if (givw < 1) givw = 1;
        if (givh < 1) givh = 1;

        const int mode = (gi_mode == RT_GI_OFF) ? RT_GI_SSGI : gi_mode;
        const int rays = (mode == RT_GI_PATHTRACE) ? kQuality[q].pt_rays : kQuality[q].gi_rays;
        int bounces = (mode == RT_GI_PATHTRACE) ? kQuality[q].pt_bounces : 1;
        if (gfx_rt_bounces > 0) { // user override (works in ssgi mode too)
            bounces = gfx_rt_bounces > 8 ? 8 : gfx_rt_bounces;
        }

        glBindFramebuffer(GL_FRAMEBUFFER, s_gitrace_fbo);
        glViewport(givx, givy, givw, givh);
        GLuint pr = s_prog[PROG_TRACE];
        rtSetCommon(pr, &fr);
        glUniform1i(rtU(pr, RTU_uRays), rays);
        glUniform1i(rtU(pr, RTU_uSteps), kQuality[q].gi_steps);
        glUniform1i(rtU(pr, RTU_uBounces), bounces);
        glUniform1f(rtU(pr, RTU_uGIRadius), gfx_rt_ao_radius * 20.0f);
        if (gfx_rt_skylight && cam->skylight_ok) {
            // sky-derived GI miss radiance (day/sunset/night)
            const float sk[3] = { cam->skylight[0] * gfx_rt_skylight_gain,
                                  cam->skylight[1] * gfx_rt_skylight_gain,
                                  cam->skylight[2] * gfx_rt_skylight_gain };
            glUniform3fv(rtU(pr, RTU_uSky), 1, sk);
        } else {
            glUniform3fv(rtU(pr, RTU_uSky), 1, gfx_rt_sky);
        }
        rtDraw();

        // temporal: cur + history[read] -> history[write]
        float cur_to_prev[16];
        if (s_prev_valid[pl]) {
            float inv_cur[16], vprev_invcur[16], pprev[16];
            mtxRigidInverse(inv_cur, cam->viewmtx);
            mtxMul(vprev_invcur, s_prev_view[pl], inv_cur);
            mtxPerspective(pprev, s_prev_fovy[pl], s_prev_aspect[pl], s_prev_znear[pl], s_prev_zfar[pl]);
            mtxMul(cur_to_prev, pprev, vprev_invcur);
        } else {
            memset(cur_to_prev, 0, sizeof(cur_to_prev)); // w always 0 -> history rejected
        }

        const int hread = s_hist_idx[pl];
        const int hwrite = 1 - hread;
        glBindFramebuffer(GL_FRAMEBUFFER, s_hist_fbo[pl][hwrite]);
        glViewport(givx, givy, givw, givh);
        pr = s_prog[PROG_TEMPORAL];
        rtSetCommon(pr, &fr);
        rtBindTex(0, s_gitrace_tex);
        rtBindTex(6, s_hist_tex[pl][hread]);
        glUniformMatrix4fv(rtU(pr, RTU_uCurToPrev), 1, GL_FALSE, cur_to_prev);
        glUniform1f(rtU(pr, RTU_uBlend), (mode == RT_GI_PATHTRACE) ? 0.93f : 0.85f);
        rtDraw();
        s_hist_idx[pl] = hwrite;

        // blur H/V into gifinal (history itself stays sharp for reprojection)
        pr = s_prog[PROG_BLUR];
        rtSetCommon(pr, &fr);
        glBindFramebuffer(GL_FRAMEBUFFER, s_gitmp_fbo);
        rtBindTex(0, s_hist_tex[pl][hwrite]);
        glUniform2f(rtU(pr, RTU_uDir), 1.0f / s_giw, 0.0f);
        rtDraw();
        glBindFramebuffer(GL_FRAMEBUFFER, s_gifinal_fbo);
        rtBindTex(0, s_gitmp_tex);
        glUniform2f(rtU(pr, RTU_uDir), 0.0f, 1.0f / s_gih);
        rtDraw();
    }

    // save this frame's camera for next frame's reprojection
    memcpy(s_prev_view[pl], cam->viewmtx, sizeof(s_prev_view[pl]));
    s_prev_fovy[pl] = cam->fovy;
    s_prev_aspect[pl] = cam->aspect;
    s_prev_znear[pl] = fr.proj[2];
    s_prev_zfar[pl] = fr.proj[3];
    s_prev_valid[pl] = true;

    // 5. SSR
    const bool ssr_run = ssr_on || dbg == RT_DEBUG_SSR;
    if (ssr_run) {
        glBindFramebuffer(GL_FRAMEBUFFER, s_ssr_fbo);
        glViewport(vx, vy, vw, vh);
        GLuint pr = s_prog[PROG_SSR];
        rtSetCommon(pr, &fr);
        glUniform1i(rtU(pr, RTU_uSSRSteps), kQuality[q].ssr_steps);
        glUniform1f(rtU(pr, RTU_uMaxDist), fr.proj[3] * 0.35f);
        rtDraw();
    }

    // 5b. dynamic lights + torch (dark/relight mode): map lights transformed
    // world -> view on the CPU, colours premultiplied by intensity
    if (lights_run) {
        float posrad[RT_MAX_LIGHTS * 4];
        float lcol[RT_MAX_LIGHTS * 4];
        const float* m = cam->viewmtx;
        for (int i = 0; i < nlights; i++) {
            const rtlight* l = &cam->lights[i];
            posrad[i * 4 + 0] = m[0] * l->pos[0] + m[4] * l->pos[1] + m[8] * l->pos[2] + m[12];
            posrad[i * 4 + 1] = m[1] * l->pos[0] + m[5] * l->pos[1] + m[9] * l->pos[2] + m[13];
            posrad[i * 4 + 2] = m[2] * l->pos[0] + m[6] * l->pos[1] + m[10] * l->pos[2] + m[14];
            posrad[i * 4 + 3] = l->radius > 1.0f ? l->radius : 1.0f;
            const float gain = l->intensity * gfx_rt_light_intensity;
            lcol[i * 4 + 0] = l->color[0] * gain;
            lcol[i * 4 + 1] = l->color[1] * gain;
            lcol[i * 4 + 2] = l->color[2] * gain;
            lcol[i * 4 + 3] = 0.0f;
        }

        glBindFramebuffer(GL_FRAMEBUFFER, s_light_fbo);
        glViewport(vx, vy, vw, vh);
        GLuint pr = s_prog[PROG_LIGHT];
        rtSetCommon(pr, &fr);
        glUniform1i(rtU(pr, RTU_uLightCount), nlights);
        if (nlights > 0) {
            glUniform4fv(rtU(pr, RTU_uLightPosRad), nlights, posrad);
            glUniform4fv(rtU(pr, RTU_uLightCol), nlights, lcol);
        }
        glUniform1i(rtU(pr, RTU_uLightShadows), gfx_rt_light_shadows);
        glUniform1i(rtU(pr, RTU_uLightSteps), kQuality[q].light_steps);
        glUniform1i(rtU(pr, RTU_uTorch), gfx_rt_torch);
        glUniform1f(rtU(pr, RTU_uTorchInt), gfx_rt_torch_intensity);
        glUniform1f(rtU(pr, RTU_uTorchRange), gfx_rt_torch_range);
        glUniform1f(rtU(pr, RTU_uLightMax), gfx_rt_light_max > 0.005f ? gfx_rt_light_max : 0.005f);
        rtDraw();
    }

    // 6. composite back over the game framebuffer
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(vx, vy, vw, vh);

    if (dbg != 0) {
        GLuint pr = s_prog[PROG_DEBUG];
        rtSetCommon(pr, &fr);
        rtBindTex(0, s_ao_tex);
        rtBindTex(1, s_gifinal_tex);
        rtBindTex(2, s_ssr_tex);
        rtBindTex(7, s_light_tex);
        glUniform1i(rtU(pr, RTU_uMode), dbg);
        rtDraw();
    } else {
        if (ao_on || sh_on || dark_on) {
            glEnable(GL_BLEND);
            glBlendFunc(GL_ZERO, GL_SRC_COLOR); // dst *= src
            GLuint pr = s_prog[PROG_COMP_MUL];
            rtSetCommon(pr, &fr);
            rtBindTex(0, s_ao_tex);
            glUniform1i(rtU(pr, RTU_uAOOn), ao_on ? 1 : 0);
            glUniform1i(rtU(pr, RTU_uShadowOn), sh_on ? 1 : 0);
            glUniform1f(rtU(pr, RTU_uAOInt), gfx_rt_ao_intensity);
            glUniform1f(rtU(pr, RTU_uShInt), gfx_rt_shadow_intensity);
            glUniform1i(rtU(pr, RTU_uDark), dark_on ? 1 : 0);
            glUniform1f(rtU(pr, RTU_uDarkAmbient), gfx_rt_dark_ambient);
            if (gfx_rt_skylight && cam->skylight_ok) {
                glUniform3fv(rtU(pr, RTU_uAmbientCol), 1, cam->skylight);
            } else {
                const float white[3] = { 1.0f, 1.0f, 1.0f };
                glUniform3fv(rtU(pr, RTU_uAmbientCol), 1, white);
            }
            rtDraw();
            glDisable(GL_BLEND);
        }
        if (gi_mode != RT_GI_OFF || ssr_on || lights_run) {
            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ONE); // dst += src
            GLuint pr = s_prog[PROG_COMP_ADD];
            rtSetCommon(pr, &fr);
            rtBindTex(1, s_gifinal_tex);
            rtBindTex(2, s_ssr_tex);
            rtBindTex(7, s_light_tex);
            glUniform1i(rtU(pr, RTU_uGIOn), gi_mode != RT_GI_OFF ? 1 : 0);
            glUniform1i(rtU(pr, RTU_uSSROn), ssr_on ? 1 : 0);
            glUniform1i(rtU(pr, RTU_uLightsOn), lights_run ? 1 : 0);
            glUniform1f(rtU(pr, RTU_uGIInt), gfx_rt_gi_intensity);
            glUniform1f(rtU(pr, RTU_uSSRInt), gfx_rt_ssr_intensity);
            glUniform1i(rtU(pr, RTU_uDark), dark_on ? 1 : 0);
            glUniform1f(rtU(pr, RTU_uDarkAmbient), gfx_rt_dark_ambient);
            glUniform1f(rtU(pr, RTU_uRelight), gfx_rt_relight_amount);
            rtDraw();
            glDisable(GL_BLEND);
        }
    }

    rtRestoreState(&saved);
}
