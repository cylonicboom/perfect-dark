/**
 * Chaos "retro" post filter (pd.pixelate / pd.crt / pd.lens / pd.screen_fx;
 * docs/PORT_CHAOS.md). One fullscreen pass hosting the whole video-fx family:
 * pixelation + colour crush (grey-N / RGB332 / invert / Game Boy / thermal),
 * CRT (scanlines, aperture grille, curvature, vignette), fisheye lens warp,
 * VHS (chroma shift + line jitter + noise), underwater wobble. The fragment
 * BODY is shared with the SDL_GPU backend via gfx_retro_common.h.
 *
 * Runs once per frame from gfx_run's tail (after the final gfx_flush, so the
 * whole frame — world, viewmodel, HUD — is in the framebuffer) via the
 * nullable retro_filter rapi entry, the rt_resolve pattern. In-place:
 *
 *   1. capture the current framebuffer colour into a retro-owned GL_RGB8
 *      texture. Two paths, same reasoning as gfx_rt.cpp: a game-FBO source is
 *      blitted (which also resolves MSAA — RGB8 matches the game framebuffer
 *      format, as resolve blits require); the default framebuffer (fbo == 0)
 *      captures via glCopyTexSubImage2D.
 *   2. draw a fullscreen triangle back over the framebuffer, sampling the
 *      capture with UVs snapped to a pixw x pixh grid (GL_NEAREST, so each
 *      block is one point-sampled source pixel — authentic chunky downscale)
 *      and crushing colours: 2..64 = N-level greyscale, >= 256 = RGB 3-3-2
 *      (256 displayable colours), 0 = pixelate only.
 *
 * All GL state touched here is saved with glGet* on entry and restored on
 * exit, so the immediate-mode renderer's cached state stays truthful.
 * This file is the GL implementation; the SDL_GPU backend (Vulkan/D3D12) has
 * its own twin in gfx_sdlgpu.cpp's retro section.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "glad/glad.h"
#include "gfx_retro.h"
#include "gfx_retro_common.h" // shared fragment body (both backends)

extern "C" void sysLogPrintf(int level, const char* fmt, ...);
#define RETRO_LOG_ERROR (2 | (1 << 7)) // LOG_ERROR | LOGFLAG_SHOWMSG

// ---------------------------------------------------------------------------
// state

static bool s_broken = false; // shader failed to build; filter disabled
static bool s_inited = false;
static GLuint s_prog = 0;
static GLuint s_vao = 0, s_vbo = 0;
static GLuint s_cap_tex = 0, s_cap_fbo = 0;
static int s_capw = 0, s_caph = 0;
static GLint s_loc_grid = -1, s_loc_mode = -1, s_loc_levels = -1;
static GLint s_loc_fx = -1, s_loc_warp = -1, s_loc_aspect = -1, s_loc_time = -1;
static unsigned int s_frames = 0; // drives uTime (the animated fx)

// GL state save/restore — everything the pass touches
struct RetroGLState {
    GLint draw_fbo, read_fbo;
    GLint viewport[4];
    GLint scissor_box[4];
    GLboolean scissor_test, depth_test, blend, cull;
    GLboolean depth_mask;
    GLint program;
    GLint active_texture;
    GLint tex_binding0; // unit 0 only — the pass binds nothing else
    GLint vao, array_buffer;
};

static void retroSaveState(RetroGLState* s) {
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &s->draw_fbo);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &s->read_fbo);
    glGetIntegerv(GL_VIEWPORT, s->viewport);
    glGetIntegerv(GL_SCISSOR_BOX, s->scissor_box);
    s->scissor_test = glIsEnabled(GL_SCISSOR_TEST);
    s->depth_test = glIsEnabled(GL_DEPTH_TEST);
    s->blend = glIsEnabled(GL_BLEND);
    s->cull = glIsEnabled(GL_CULL_FACE);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &s->depth_mask);
    glGetIntegerv(GL_CURRENT_PROGRAM, &s->program);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &s->active_texture);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &s->tex_binding0);
    s->vao = 0;
    if (glad_glGetIntegerv && glad_glBindVertexArray) {
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &s->vao);
    }
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &s->array_buffer);
}

static void retroRestoreState(const RetroGLState* s) {
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s->draw_fbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, s->read_fbo);
    glViewport(s->viewport[0], s->viewport[1], s->viewport[2], s->viewport[3]);
    glScissor(s->scissor_box[0], s->scissor_box[1], s->scissor_box[2], s->scissor_box[3]);
    if (s->scissor_test) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    if (s->depth_test) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (s->blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (s->cull) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    glDepthMask(s->depth_mask);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s->tex_binding0);
    glActiveTexture(s->active_texture);
    glUseProgram(s->program);
    if (glad_glBindVertexArray) {
        glBindVertexArray(s->vao);
    }
    glBindBuffer(GL_ARRAY_BUFFER, s->array_buffer);
}

// ---------------------------------------------------------------------------
// shader

static const char* kVS =
    "IN vec2 aPos;\n"
    "OUT vec2 vUV;\n"
    "void main() {\n"
    "    vUV = aPos * 0.5 + 0.5;\n"
    "    gl_Position = vec4(aPos, 0.0, 1.0);\n"
    "}\n";

// prelude (uniform declarations) + the shared body from gfx_retro_common.h
static const char* kFS =
    "IN vec2 vUV;\n"
    "OUT vec4 oCol;\n"
    "uniform sampler2D uColor;\n"
    "uniform vec2 uGrid;\n"
    "uniform float uLevels;\n"
    "uniform int uMode;\n"
    "uniform int uFx;\n"
    "uniform float uWarp;\n"
    "uniform float uAspect;\n"
    "uniform float uTime;\n"
    RETRO_GLSL_BODY;

static GLuint retroCompile(GLenum type, const char* version, const char* body) {
    char* src = (char*)malloc(strlen(body) + 256);
    sprintf(src, "#version %s\n#define IN %s\n#define OUT %s\n%s", version,
            "in", "out", body);
    GLuint sh = glCreateShader(type);
    const char* s = src;
    glShaderSource(sh, 1, &s, NULL);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(sh, sizeof(log), NULL, log);
        sysLogPrintf(RETRO_LOG_ERROR, "retro: shader compile failed: %s", log);
        glDeleteShader(sh);
        sh = 0;
    }
    free(src);
    return sh;
}

static bool retroInit(const char* glsl_version) {
    GLuint vs = retroCompile(GL_VERTEX_SHADER, glsl_version, kVS);
    GLuint fs = retroCompile(GL_FRAGMENT_SHADER, glsl_version, kFS);
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return false;
    }
    s_prog = glCreateProgram();
    glAttachShader(s_prog, vs);
    glAttachShader(s_prog, fs);
    glBindAttribLocation(s_prog, 0, "aPos");
    if (glad_glBindFragDataLocation) {
        glBindFragDataLocation(s_prog, 0, "oCol");
    }
    glLinkProgram(s_prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(s_prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(s_prog, sizeof(log), NULL, log);
        sysLogPrintf(RETRO_LOG_ERROR, "retro: program link failed: %s", log);
        glDeleteProgram(s_prog);
        s_prog = 0;
        return false;
    }
    glUseProgram(s_prog);
    GLint loc = glGetUniformLocation(s_prog, "uColor");
    if (loc >= 0) glUniform1i(loc, 0);
    s_loc_grid = glGetUniformLocation(s_prog, "uGrid");
    s_loc_mode = glGetUniformLocation(s_prog, "uMode");
    s_loc_levels = glGetUniformLocation(s_prog, "uLevels");
    s_loc_fx = glGetUniformLocation(s_prog, "uFx");
    s_loc_warp = glGetUniformLocation(s_prog, "uWarp");
    s_loc_aspect = glGetUniformLocation(s_prog, "uAspect");
    s_loc_time = glGetUniformLocation(s_prog, "uTime");

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
    return true;
}

static bool retroEnsureCapture(int fbw, int fbh) {
    if (s_capw == fbw && s_caph == fbh && s_cap_tex) {
        return true;
    }
    if (s_cap_fbo) { glDeleteFramebuffers(1, &s_cap_fbo); s_cap_fbo = 0; }
    if (s_cap_tex) { glDeleteTextures(1, &s_cap_tex); s_cap_tex = 0; }

    // GL_RGB8 exactly matches the game framebuffer colour format — an MSAA
    // resolve blit requires identical internal formats (see gfx_rt.cpp).
    glGenTextures(1, &s_cap_tex);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_cap_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, fbw, fbh, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &s_cap_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, s_cap_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_cap_tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        sysLogPrintf(RETRO_LOG_ERROR, "retro: capture framebuffer incomplete");
        return false;
    }
    s_capw = fbw;
    s_caph = fbh;
    return true;
}

// ---------------------------------------------------------------------------
// the filter

void gfx_retro_filter(int pixw, int pixh, int cmode, int clevels, int fx, float warp,
                      unsigned int fbo, int fbw, int fbh, const char* glsl_version) {
    if (s_broken || fbw <= 0 || fbh <= 0) {
        return;
    }

    RetroGLState saved;
    retroSaveState(&saved);

    if (!s_inited) {
        s_inited = true;
        if (!retroInit(glsl_version)) {
            s_broken = true;
            retroRestoreState(&saved);
            return;
        }
    }
    if (!retroEnsureCapture(fbw, fbh)) {
        s_broken = true;
        retroRestoreState(&saved);
        return;
    }

    // capture the finished frame's colour
    if (fbo != 0) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_cap_fbo);
        glBlitFramebuffer(0, 0, fbw, fbh, 0, 0, fbw, fbh, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    } else {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, s_cap_tex);
        glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, fbw, fbh);
    }

    // draw it back filtered, over the full framebuffer
    s_frames++;

    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, fbw, fbh);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glUseProgram(s_prog);
    if (s_loc_grid >= 0) glUniform2f(s_loc_grid, (float)pixw, (float)pixh);
    if (s_loc_mode >= 0) glUniform1i(s_loc_mode, cmode);
    if (s_loc_levels >= 0) glUniform1f(s_loc_levels, (float)clevels);
    if (s_loc_fx >= 0) glUniform1i(s_loc_fx, fx);
    if (s_loc_warp >= 0) glUniform1f(s_loc_warp, warp);
    if (s_loc_aspect >= 0) glUniform1f(s_loc_aspect, (float)fbw / (float)fbh);
    if (s_loc_time >= 0) glUniform1f(s_loc_time, (float)(s_frames % 216000u) / 60.0f);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_cap_tex);
    if (s_vao) {
        glBindVertexArray(s_vao);
    } else {
        glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, (void*)0);
    }
    glDrawArrays(GL_TRIANGLES, 0, 3);

    retroRestoreState(&saved);
}
