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
#include "gfx_rt.h"

// logging (values from port/include/system.h; keep the dependency thin —
// pulling system.h in would drag PR/ultratypes into the renderer)
extern "C" void sysLogPrintf(int level, const char* fmt, ...);
#define RT_LOG_NOTE 0                 // LOG_NOTE
#define RT_LOG_ERROR (2 | (1 << 7))   // LOG_ERROR (2 | LOGFLAG_SHOWMSG)

#define RT_MAX_PLAYERS 4

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
    PROG_COMP_MUL,
    PROG_COMP_ADD,
    PROG_DEBUG,
    PROG_COUNT
};
static GLuint s_prog[PROG_COUNT];

// quality presets: sample/step budgets per gfx_rt_quality
static const struct {
    int ao_samples, shadow_steps, ssr_steps;
    int gi_rays, gi_steps, pt_rays, pt_bounces;
} kQuality[3] = {
    {  6, 12, 20, 4,  8, 1, 2 }, // low
    { 10, 20, 32, 6, 12, 2, 3 }, // medium
    { 16, 28, 48, 8, 16, 3, 3 }, // high
};

// ---------------------------------------------------------------------------
// small matrix helpers (column-major 4x4, GL convention)

static void mtxMul(float* out, const float* a, const float* b) { // out = a*b
    float r[16];
    for (int c = 0; c < 4; c++) {
        for (int rr = 0; rr < 4; rr++) {
            r[c * 4 + rr] = a[0 * 4 + rr] * b[c * 4 + 0] + a[1 * 4 + rr] * b[c * 4 + 1] +
                            a[2 * 4 + rr] * b[c * 4 + 2] + a[3 * 4 + rr] * b[c * 4 + 3];
        }
    }
    memcpy(out, r, sizeof(r));
}

// rigid-body inverse (rotation + translation only)
static void mtxRigidInverse(float* out, const float* m) {
    float r[16];
    // transpose rotation
    for (int c = 0; c < 3; c++) {
        for (int rr = 0; rr < 3; rr++) {
            r[c * 4 + rr] = m[rr * 4 + c];
        }
    }
    r[3] = r[7] = r[11] = 0.0f;
    // t' = -R^T * t
    const float tx = m[12], ty = m[13], tz = m[14];
    r[12] = -(r[0] * tx + r[4] * ty + r[8] * tz);
    r[13] = -(r[1] * tx + r[5] * ty + r[9] * tz);
    r[14] = -(r[2] * tx + r[6] * ty + r[10] * tz);
    r[15] = 1.0f;
    memcpy(out, r, sizeof(r));
}

static void mtxPerspective(float* out, float fovy_deg, float aspect, float zn, float zf) {
    memset(out, 0, sizeof(float) * 16);
    const float t = tanf(fovy_deg * (float)(3.14159265358979 / 180.0) * 0.5f);
    out[0] = 1.0f / (t * aspect);
    out[5] = 1.0f / t;
    out[10] = -(zf + zn) / (zf - zn);
    out[11] = -1.0f;
    out[14] = -2.0f * zf * zn / (zf - zn);
}

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
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &s->draw_fbo);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &s->read_fbo);
    glGetIntegerv(GL_VIEWPORT, s->viewport);
    glGetIntegerv(GL_SCISSOR_BOX, s->scissor_box);
    s->scissor_test = glIsEnabled(GL_SCISSOR_TEST);
    s->depth_test = glIsEnabled(GL_DEPTH_TEST);
    s->blend = glIsEnabled(GL_BLEND);
    s->cull = glIsEnabled(GL_CULL_FACE);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &s->depth_mask);
    glGetIntegerv(GL_DEPTH_FUNC, &s->depth_func);
    glGetIntegerv(GL_BLEND_SRC_RGB, &s->blend_src_rgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &s->blend_dst_rgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &s->blend_src_a);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &s->blend_dst_a);
    glGetIntegerv(GL_CURRENT_PROGRAM, &s->program);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &s->active_texture);
    for (int i = 0; i < 8; i++) {
        glActiveTexture(GL_TEXTURE0 + i);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &s->tex_binding[i]);
    }
    s->vao = 0;
    if (glad_glGetIntegerv && glad_glBindVertexArray) {
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &s->vao);
    }
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &s->array_buffer);
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

// Shared fragment prelude: reconstruction + hashing helpers.
// uProj = (tanHalfFovX, tanHalfFovY, znear, zfar); uYSign flips the vertical
// NDC<->UV mapping for framebuffers rendered upside-down (fb invert_y).
static const char* kFSCommon =
    "IN vec2 vUV;\n"
    "OUT vec4 oCol;\n"
    "uniform sampler2D uNorm;\n"  // xyz = view normal, w = linear depth (0 = sky)
    "uniform sampler2D uColor;\n" // scene colour
    "uniform vec4 uProj;\n"
    "uniform vec4 uRect;\n"
    "uniform float uYSign;\n"
    "uniform vec2 uTexel;\n"
    "uniform int uFrame;\n"
    "vec3 vpos(vec2 uv, float lz) {\n"
    "    vec2 nd = ((uv - uRect.xy) / (uRect.zw - uRect.xy)) * 2.0 - 1.0;\n"
    "    return vec3(nd.x * uProj.x * lz, nd.y * uYSign * uProj.y * lz, -lz);\n"
    "}\n"
    "vec2 puv(vec3 p) {\n"
    "    float lz = -p.z;\n"
    "    vec2 nd = vec2(p.x / (uProj.x * lz), p.y * uYSign / (uProj.y * lz));\n"
    "    return uRect.xy + (nd * 0.5 + 0.5) * (uRect.zw - uRect.xy);\n"
    "}\n"
    "bool inrect(vec2 uv) {\n"
    "    return uv.x > uRect.x && uv.y > uRect.y && uv.x < uRect.z && uv.y < uRect.w;\n"
    "}\n"
    "float h12(vec2 p) {\n"
    "    vec3 p3 = fract(vec3(p.xyx) * 0.1031);\n"
    "    p3 += dot(p3, p3.yzx + 33.33);\n"
    "    return fract((p3.x + p3.y) * p3.z);\n"
    "}\n"
    "vec2 h22(vec2 p) {\n"
    "    vec3 p3 = fract(vec3(p.xyx) * vec3(0.1031, 0.1030, 0.0973));\n"
    "    p3 += dot(p3, p3.yzx + 33.33);\n"
    "    return fract((p3.xx + p3.yz) * p3.zy);\n"
    "}\n"
    "vec3 coshemi(vec3 n, vec2 r) {\n"
    "    float a = 6.2831853 * r.x;\n"
    "    vec3 t = normalize(abs(n.z) < 0.999 ? cross(n, vec3(0.0, 0.0, 1.0))\n"
    "                                        : cross(n, vec3(1.0, 0.0, 0.0)));\n"
    "    vec3 b = cross(n, t);\n"
    "    float s = sqrt(max(1.0 - r.y, 0.0));\n"
    "    return normalize(t * (cos(a) * s) + b * (sin(a) * s) + n * sqrt(r.y));\n"
    "}\n";

// prepass: depth -> view-space normal + linear depth
static const char* kFSPrepass =
    "uniform sampler2D uDepth;\n"
    "float lin(float d) {\n"
    "    float n = uProj.z, f = uProj.w;\n"
    "    float nd = d * 2.0 - 1.0;\n"
    "    return 2.0 * n * f / (f + n - nd * (f - n));\n"
    "}\n"
    "void main() {\n"
    "    float d = texture(uDepth, vUV).r;\n"
    "    if (d >= 0.99999) { oCol = vec4(0.0); return; }\n"
    "    vec2 dx = vec2(uTexel.x, 0.0), dy = vec2(0.0, uTexel.y);\n"
    "    float lz = lin(d);\n"
    "    vec3 P = vpos(vUV, lz);\n"
    "    vec3 pR = vpos(vUV + dx, lin(texture(uDepth, vUV + dx).r));\n"
    "    vec3 pL = vpos(vUV - dx, lin(texture(uDepth, vUV - dx).r));\n"
    "    vec3 pU = vpos(vUV + dy, lin(texture(uDepth, vUV + dy).r));\n"
    "    vec3 pD = vpos(vUV - dy, lin(texture(uDepth, vUV - dy).r));\n"
    "    vec3 ddx = (abs(pR.z - P.z) < abs(P.z - pL.z)) ? (pR - P) : (P - pL);\n"
    "    vec3 ddy = (abs(pU.z - P.z) < abs(P.z - pD.z)) ? (pU - P) : (P - pD);\n"
    "    vec3 n = normalize(cross(ddx, ddy));\n"
    "    if (dot(n, -P) < 0.0) n = -n;\n"
    "    oCol = vec4(n, lz);\n"
    "}\n";

// AO (hemisphere occlusion rays) + directional screen-space shadow march.
// out: r = ambient visibility, g = sun visibility
static const char* kFSAoShadow =
    "uniform int uAOOn, uShadowOn;\n"
    "uniform int uAOSamples, uShadowSteps;\n"
    "uniform float uAORadius, uShadowLen;\n"
    "uniform vec3 uSun;\n" // view-space, toward the light, normalized
    "void main() {\n"
    "    vec4 nz = texture(uNorm, vUV);\n"
    "    if (nz.w <= 0.0) { oCol = vec4(1.0); return; }\n"
    "    vec3 n = nz.xyz;\n"
    "    vec3 P = vpos(vUV, nz.w);\n"
    "    vec2 seed = gl_FragCoord.xy + vec2(float(uFrame) * 17.13, float(uFrame) * 7.77);\n"
    "    float ao = 1.0;\n"
    "    if (uAOOn != 0) {\n"
    "        float occ = 0.0;\n"
    "        for (int i = 0; i < uAOSamples; i++) {\n"
    "            vec2 r = h22(seed + vec2(float(i) * 3.7, float(i) * 9.1));\n"
    "            vec3 dir = coshemi(n, r);\n"
    "            float t = uAORadius * (0.15 + 0.85 * h12(seed + vec2(float(i) * 5.3, 2.1)));\n"
    "            vec3 S = P + dir * t;\n"
    "            vec2 uv2 = puv(S);\n"
    "            if (!inrect(uv2)) continue;\n"
    "            float sz = texture(uNorm, uv2).w;\n"
    "            float rz = -S.z;\n"
    "            if (sz > 0.0 && sz < rz - 0.5 && (rz - sz) < uAORadius * 1.5) {\n"
    "                occ += 1.0 - clamp((rz - sz) / (uAORadius * 1.5), 0.0, 1.0) * 0.5;\n"
    "            }\n"
    "        }\n"
    "        ao = clamp(1.0 - occ / float(uAOSamples), 0.0, 1.0);\n"
    "    }\n"
    "    float sh = 1.0;\n"
    "    if (uShadowOn != 0) {\n"
    "        float ndl = dot(n, uSun);\n"
    "        if (ndl <= 0.0) {\n"
    "            sh = 0.0;\n"
    "        } else {\n"
    "            float dt = uShadowLen / float(uShadowSteps);\n"
    "            float t = dt * (0.3 + 0.7 * h12(seed + vec2(31.7, 13.3)));\n"
    "            vec3 O = P + n * (nz.w * 0.01 + 0.5);\n"
    "            for (int i = 0; i < uShadowSteps; i++) {\n"
    "                vec3 S = O + uSun * t;\n"
    "                t += dt;\n"
    "                if (S.z > -uProj.z) break;\n" // crossed the near plane
    "                vec2 uv2 = puv(S);\n"
    "                if (!inrect(uv2)) break;\n"
    "                float sz = texture(uNorm, uv2).w;\n"
    "                float rz = -S.z;\n"
    "                float thick = 4.0 + t * 0.15;\n"
    "                if (sz > 0.0 && rz - sz > 0.8 && rz - sz < thick) { sh = 0.0; break; }\n"
    "            }\n"
    "            sh *= smoothstep(0.0, 0.25, ndl);\n"
    "        }\n"
    "    }\n"
    "    oCol = vec4(ao, sh, 0.0, 1.0);\n"
    "}\n";

// GI trace: cosine-hemisphere rays marched against the depth buffer.
// uBounces == 1 -> SSGI (single bounce); > 1 -> stochastic path trace with
// throughput. Scene colour doubles as both radiance and albedo (the accepted
// screen-space hack — there is no material data).
static const char* kFSTrace =
    "uniform int uRays, uSteps, uBounces;\n"
    "uniform float uGIRadius;\n"
    "uniform vec3 uSky;\n"
    "void main() {\n"
    "    vec4 nz = texture(uNorm, vUV);\n"
    "    if (nz.w <= 0.0) { oCol = vec4(0.0, 0.0, 0.0, 1.0); return; }\n"
    "    vec2 seed = gl_FragCoord.xy * 1.37 + vec2(float(uFrame) * 23.71, float(uFrame) * 11.29);\n"
    "    vec3 total = vec3(0.0);\n"
    "    for (int r = 0; r < uRays; r++) {\n"
    "        vec3 P = vpos(vUV, nz.w);\n"
    "        vec3 n = nz.xyz;\n"
    "        vec3 through = vec3(1.0);\n"
    "        for (int b = 0; b < uBounces; b++) {\n"
    "            vec2 rnd = h22(seed + vec2(float(r) * 13.1 + float(b) * 41.3, float(r) * 7.9 + float(b) * 3.1));\n"
    "            vec3 dir = coshemi(n, rnd);\n"
    "            float t = 2.0 + 4.0 * h12(seed + vec2(float(b) * 9.7, float(r) * 5.1));\n"
    "            float dt = uGIRadius / float(uSteps) * 0.35;\n"
    "            bool hit = false;\n"
    "            vec2 hituv = vec2(0.0);\n"
    "            for (int i = 0; i < uSteps; i++) {\n"
    "                vec3 S = P + dir * t;\n"
    "                if (S.z > -uProj.z) break;\n"
    "                vec2 uv2 = puv(S);\n"
    "                if (!inrect(uv2)) break;\n"
    "                float sz = texture(uNorm, uv2).w;\n"
    "                float rz = -S.z;\n"
    "                if (sz > 0.0 && rz - sz > 0.5 && rz - sz < 6.0 + t * 0.35) {\n"
    "                    hit = true; hituv = uv2; break;\n"
    "                }\n"
    "                t += dt;\n"
    "                dt *= 1.22;\n"
    "            }\n"
    "            if (hit) {\n"
    "                vec3 col = texture(uColor, hituv).rgb;\n"
    "                total += through * col;\n"
    "                if (b + 1 < uBounces) {\n"
    "                    through *= col;\n"
    "                    vec4 hnz = texture(uNorm, hituv);\n"
    "                    if (hnz.w <= 0.0) break;\n"
    "                    vec3 hn = hnz.xyz;\n"
    "                    if (dot(hn, dir) > 0.0) hn = -hn;\n"
    "                    P = vpos(hituv, hnz.w);\n"
    "                    n = hn;\n"
    "                }\n"
    "            } else {\n"
    "                total += through * uSky;\n"
    "                break;\n"
    "            }\n"
    "        }\n"
    "    }\n"
    "    oCol = vec4(total / float(uRays), 1.0);\n"
    "}\n";

// temporal accumulation with depth-validated reprojection
static const char* kFSTemporal =
    "uniform sampler2D uCur;\n"
    "uniform sampler2D uHist;\n"
    "uniform mat4 uCurToPrev;\n" // P_prev * V_prev * inv(V_cur)
    "uniform float uBlend;\n"
    "void main() {\n"
    "    vec4 nz = texture(uNorm, vUV);\n"
    "    vec3 cur = texture(uCur, vUV).rgb;\n"
    "    if (nz.w <= 0.0) { oCol = vec4(cur, 0.0); return; }\n"
    "    vec3 P = vpos(vUV, nz.w);\n"
    "    vec4 pc = uCurToPrev * vec4(P, 1.0);\n"
    "    vec3 res = cur;\n"
    "    if (pc.w > 0.02) {\n"
    "        vec2 nd = pc.xy / pc.w;\n"
    "        vec2 uv2 = uRect.xy + (vec2(nd.x, nd.y * uYSign) * 0.5 + 0.5) * (uRect.zw - uRect.xy);\n"
    "        if (inrect(uv2)) {\n"
    "            vec4 h = texture(uHist, uv2);\n"
    "            if (h.a > 0.0 && abs(h.a - pc.w) < 0.08 * pc.w) {\n"
    "                res = mix(cur, h.rgb, uBlend);\n"
    "            }\n"
    "        }\n"
    "    }\n"
    "    oCol = vec4(res, nz.w);\n"
    "}\n";

// separable depth-aware blur (uDir = one-texel step along the blur axis)
static const char* kFSBlur =
    "uniform sampler2D uSrc;\n"
    "uniform vec2 uDir;\n"
    "void main() {\n"
    "    float zc = texture(uNorm, vUV).w;\n"
    "    vec4 acc = texture(uSrc, vUV);\n"
    "    float wsum = 1.0;\n"
    "    if (zc > 0.0) {\n"
    "        acc *= 0.4026; wsum = 0.4026;\n"
    "        for (int i = 1; i <= 2; i++) {\n"
    "            vec2 o = uDir * float(i);\n"
    "            float gw = (i == 1) ? 0.2442 : 0.0545;\n"
    "            float z1 = texture(uNorm, vUV + o).w;\n"
    "            float z2 = texture(uNorm, vUV - o).w;\n"
    "            float w1 = gw * exp(-abs(zc - z1) / max(zc * 0.05, 1.0));\n"
    "            float w2 = gw * exp(-abs(zc - z2) / max(zc * 0.05, 1.0));\n"
    "            acc += texture(uSrc, vUV + o) * w1 + texture(uSrc, vUV - o) * w2;\n"
    "            wsum += w1 + w2;\n"
    "        }\n"
    "        acc /= wsum;\n"
    "    }\n"
    "    oCol = acc;\n"
    "}\n";

// SSR: reflected-ray march + binary refinement. out rgb = colour, a = weight
static const char* kFSSsr =
    "uniform int uSSRSteps;\n"
    "uniform float uMaxDist;\n"
    "void main() {\n"
    "    vec4 nz = texture(uNorm, vUV);\n"
    "    if (nz.w <= 0.0) { oCol = vec4(0.0); return; }\n"
    "    vec3 n = nz.xyz;\n"
    "    vec3 P = vpos(vUV, nz.w);\n"
    "    vec3 V = normalize(-P);\n"
    "    vec3 R = reflect(-V, n);\n"
    "    float jitter = h12(gl_FragCoord.xy + vec2(float(uFrame) * 3.3, 0.0));\n"
    "    float t = max(nz.w * 0.02, 1.0) * (0.5 + jitter);\n"
    "    float dt = uMaxDist / float(uSSRSteps) * 0.12;\n"
    "    float tprev = 0.0;\n"
    "    vec3 col = vec3(0.0);\n"
    "    float conf = 0.0;\n"
    "    for (int i = 0; i < uSSRSteps; i++) {\n"
    "        vec3 S = P + R * t;\n"
    "        if (S.z > -uProj.z) break;\n"
    "        vec2 uv2 = puv(S);\n"
    "        if (!inrect(uv2)) break;\n"
    "        float sz = texture(uNorm, uv2).w;\n"
    "        float rz = -S.z;\n"
    "        if (sz > 0.0 && rz - sz > 0.1 && rz - sz < 4.0 + t * 0.25) {\n"
    "            float lo = tprev, hi = t;\n"
    "            for (int j = 0; j < 4; j++) {\n"
    "                float mid = (lo + hi) * 0.5;\n"
    "                vec3 M = P + R * mid;\n"
    "                float mz = texture(uNorm, puv(M)).w;\n"
    "                if (mz > 0.0 && -M.z > mz) hi = mid; else lo = mid;\n"
    "            }\n"
    "            vec2 uvh = puv(P + R * hi);\n"
    "            if (inrect(uvh)) {\n"
    "                col = texture(uColor, uvh).rgb;\n"
    "                vec2 rc = (uvh - uRect.xy) / (uRect.zw - uRect.xy);\n"
    "                vec2 ef = min(rc, 1.0 - rc) * 8.0;\n"
    "                float edge = clamp(min(ef.x, ef.y), 0.0, 1.0);\n"
    "                float dist = 1.0 - clamp(t / uMaxDist, 0.0, 1.0);\n"
    "                conf = edge * dist;\n"
    "            }\n"
    "            break;\n"
    "        }\n"
    "        tprev = t;\n"
    "        t += dt;\n"
    "        dt *= 1.12;\n"
    "        if (t > uMaxDist) break;\n"
    "    }\n"
    "    float fres = pow(1.0 - clamp(dot(n, V), 0.0, 1.0), 5.0);\n"
    "    oCol = vec4(col, conf * mix(0.08, 1.0, fres));\n"
    "}\n";

// multiplicative composite: dst *= AO * shadow (blend GL_ZERO, GL_SRC_COLOR)
static const char* kFSCompMul =
    "uniform sampler2D uAO;\n"
    "uniform int uAOOn, uShadowOn;\n"
    "uniform float uAOInt, uShInt;\n"
    "void main() {\n"
    "    vec2 aosh = texture(uAO, vUV).rg;\n"
    "    float m = 1.0;\n"
    "    if (uAOOn != 0) m *= mix(1.0, aosh.r * aosh.r, uAOInt);\n"
    "    if (uShadowOn != 0) m *= 1.0 - uShInt * (1.0 - aosh.g);\n"
    "    oCol = vec4(vec3(m), 1.0);\n"
    "}\n";

// additive composite: dst += GI * albedo + SSR (blend GL_ONE, GL_ONE)
static const char* kFSCompAdd =
    "uniform sampler2D uGI;\n"
    "uniform sampler2D uSSR;\n"
    "uniform int uGIOn, uSSROn;\n"
    "uniform float uGIInt, uSSRInt;\n"
    "void main() {\n"
    "    vec3 add = vec3(0.0);\n"
    "    if (uGIOn != 0) {\n"
    "        vec3 albedo = texture(uColor, vUV).rgb;\n"
    "        add += texture(uGI, vUV).rgb * albedo * uGIInt;\n"
    "    }\n"
    "    if (uSSROn != 0) {\n"
    "        vec4 ssr = texture(uSSR, vUV);\n"
    "        add += ssr.rgb * ssr.a * uSSRInt;\n"
    "    }\n"
    "    // tiny dither to keep the additive gradient from banding on RGB8\n"
    "    add += (h12(gl_FragCoord.xy) - 0.5) / 255.0;\n"
    "    oCol = vec4(max(add, 0.0), 0.0);\n"
    "}\n";

// debug views (replaces the scene in the viewport)
static const char* kFSDebug =
    "uniform sampler2D uAO;\n"
    "uniform sampler2D uGI;\n"
    "uniform sampler2D uSSR;\n"
    "uniform int uMode;\n"
    "void main() {\n"
    "    vec4 nz = texture(uNorm, vUV);\n"
    "    vec3 c = vec3(1.0, 0.0, 1.0);\n"
    "    if (uMode == 1) c = vec3(clamp(nz.w / uProj.w, 0.0, 1.0));\n"
    "    else if (uMode == 2) c = nz.xyz * 0.5 + 0.5;\n"
    "    else if (uMode == 3) c = vec3(texture(uAO, vUV).r);\n"
    "    else if (uMode == 4) c = vec3(texture(uAO, vUV).g);\n"
    "    else if (uMode == 5) c = texture(uGI, vUV).rgb;\n"
    "    else if (uMode == 6) { vec4 s = texture(uSSR, vUV); c = s.rgb * s.a; }\n"
    "    oCol = vec4(c, 1.0);\n"
    "}\n";

// ---------------------------------------------------------------------------
// shader building

static GLuint rtCompile(GLenum type, const char* version, const char* body, bool is_fs) {
    char* src = (char*)malloc(strlen(kFSCommon) + strlen(body) + 512);
    char* p = src;
    p += sprintf(p, "#version %s\n", version);
    // GLSL 130+: in/out everywhere. Aliased so the sources stay single strings.
    p += sprintf(p, "#define IN in\n#define OUT out\n");
    if (is_fs) {
        strcpy(p, kFSCommon);
        p += strlen(kFSCommon);
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
        "prepass", "aoshadow", "trace", "temporal", "blur", "ssr", "comp_mul", "comp_add", "debug"
    };
    const char* bodies[PROG_COUNT] = {
        kFSPrepass, kFSAoShadow, kFSTrace, kFSTemporal, kFSBlur, kFSSsr, kFSCompMul, kFSCompAdd, kFSDebug
    };
    for (int i = 0; i < PROG_COUNT; i++) {
        s_prog[i] = rtLink(glsl_version, bodies[i]);
        if (!s_prog[i]) {
            sysLogPrintf(RT_LOG_ERROR, "RT: failed to build '%s' — raytracing disabled", names[i]);
            return false;
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

static GLint rtU(GLuint prog, const char* name) {
    return glGetUniformLocation(prog, name);
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
    if ((loc = rtU(prog, "uRect")) >= 0) glUniform4fv(loc, 1, f->rect);
    if ((loc = rtU(prog, "uProj")) >= 0) glUniform4fv(loc, 1, f->proj);
    if ((loc = rtU(prog, "uYSign")) >= 0) glUniform1f(loc, f->ysign);
    if ((loc = rtU(prog, "uTexel")) >= 0) glUniform2fv(loc, 1, f->texel);
    if ((loc = rtU(prog, "uFrame")) >= 0) glUniform1i(loc, f->frame);
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
    const int gi_mode = (gfx_rt_gi < 0) ? 0 : (gfx_rt_gi > 2 ? 2 : gfx_rt_gi);
    const int dbg = (gfx_rt_debug > 0 && gfx_rt_debug < RT_DEBUG_MAX) ? gfx_rt_debug : 0;
    if (!ao_on && !sh_on && !ssr_on && gi_mode == RT_GI_OFF && dbg == 0) {
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
        // sun direction: world -> view (rotation only), normalized
        float sw[3] = { gfx_rt_sun_dir[0], gfx_rt_sun_dir[1], gfx_rt_sun_dir[2] };
        float sl = sqrtf(sw[0] * sw[0] + sw[1] * sw[1] + sw[2] * sw[2]);
        if (sl < 0.0001f) { sw[0] = 0.0f; sw[1] = 1.0f; sw[2] = 0.0f; sl = 1.0f; }
        sw[0] /= sl; sw[1] /= sl; sw[2] /= sl;
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
        glUniform1i(rtU(pr, "uAOOn"), (ao_on || dbg == RT_DEBUG_AO) ? 1 : 0);
        glUniform1i(rtU(pr, "uShadowOn"), (sh_on || dbg == RT_DEBUG_SHADOW) ? 1 : 0);
        glUniform1i(rtU(pr, "uAOSamples"), kQuality[q].ao_samples);
        glUniform1i(rtU(pr, "uShadowSteps"), kQuality[q].shadow_steps);
        glUniform1f(rtU(pr, "uAORadius"), gfx_rt_ao_radius);
        glUniform1f(rtU(pr, "uShadowLen"), gfx_rt_shadow_length);
        glUniform3fv(rtU(pr, "uSun"), 1, sv);
        rtDraw();

        // blur H: ao -> aotmp, blur V: aotmp -> ao
        pr = s_prog[PROG_BLUR];
        rtSetCommon(pr, &fr);
        glBindFramebuffer(GL_FRAMEBUFFER, s_aotmp_fbo);
        rtBindTex(0, s_ao_tex);
        glUniform2f(rtU(pr, "uDir"), fr.texel[0], 0.0f);
        rtDraw();
        glBindFramebuffer(GL_FRAMEBUFFER, s_ao_fbo);
        rtBindTex(0, s_aotmp_tex);
        glUniform2f(rtU(pr, "uDir"), 0.0f, fr.texel[1]);
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
        const int bounces = (mode == RT_GI_PATHTRACE) ? kQuality[q].pt_bounces : 1;

        glBindFramebuffer(GL_FRAMEBUFFER, s_gitrace_fbo);
        glViewport(givx, givy, givw, givh);
        GLuint pr = s_prog[PROG_TRACE];
        rtSetCommon(pr, &fr);
        glUniform1i(rtU(pr, "uRays"), rays);
        glUniform1i(rtU(pr, "uSteps"), kQuality[q].gi_steps);
        glUniform1i(rtU(pr, "uBounces"), bounces);
        glUniform1f(rtU(pr, "uGIRadius"), gfx_rt_ao_radius * 20.0f);
        glUniform3fv(rtU(pr, "uSky"), 1, gfx_rt_sky);
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
        glUniformMatrix4fv(rtU(pr, "uCurToPrev"), 1, GL_FALSE, cur_to_prev);
        glUniform1f(rtU(pr, "uBlend"), (mode == RT_GI_PATHTRACE) ? 0.93f : 0.85f);
        rtDraw();
        s_hist_idx[pl] = hwrite;

        // blur H/V into gifinal (history itself stays sharp for reprojection)
        pr = s_prog[PROG_BLUR];
        rtSetCommon(pr, &fr);
        glBindFramebuffer(GL_FRAMEBUFFER, s_gitmp_fbo);
        rtBindTex(0, s_hist_tex[pl][hwrite]);
        glUniform2f(rtU(pr, "uDir"), 1.0f / s_giw, 0.0f);
        rtDraw();
        glBindFramebuffer(GL_FRAMEBUFFER, s_gifinal_fbo);
        rtBindTex(0, s_gitmp_tex);
        glUniform2f(rtU(pr, "uDir"), 0.0f, 1.0f / s_gih);
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
        glUniform1i(rtU(pr, "uSSRSteps"), kQuality[q].ssr_steps);
        glUniform1f(rtU(pr, "uMaxDist"), fr.proj[3] * 0.35f);
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
        glUniform1i(rtU(pr, "uMode"), dbg);
        rtDraw();
    } else {
        if (ao_on || sh_on) {
            glEnable(GL_BLEND);
            glBlendFunc(GL_ZERO, GL_SRC_COLOR); // dst *= src
            GLuint pr = s_prog[PROG_COMP_MUL];
            rtSetCommon(pr, &fr);
            rtBindTex(0, s_ao_tex);
            glUniform1i(rtU(pr, "uAOOn"), ao_on ? 1 : 0);
            glUniform1i(rtU(pr, "uShadowOn"), sh_on ? 1 : 0);
            glUniform1f(rtU(pr, "uAOInt"), gfx_rt_ao_intensity);
            glUniform1f(rtU(pr, "uShInt"), gfx_rt_shadow_intensity);
            rtDraw();
            glDisable(GL_BLEND);
        }
        if (gi_mode != RT_GI_OFF || ssr_on) {
            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ONE); // dst += src
            GLuint pr = s_prog[PROG_COMP_ADD];
            rtSetCommon(pr, &fr);
            rtBindTex(1, s_gifinal_tex);
            rtBindTex(2, s_ssr_tex);
            glUniform1i(rtU(pr, "uGIOn"), gi_mode != RT_GI_OFF ? 1 : 0);
            glUniform1i(rtU(pr, "uSSROn"), ssr_on ? 1 : 0);
            glUniform1f(rtU(pr, "uGIInt"), gfx_rt_gi_intensity);
            glUniform1f(rtU(pr, "uSSRInt"), gfx_rt_ssr_intensity);
            rtDraw();
            glDisable(GL_BLEND);
        }
    }

    rtRestoreState(&saved);
}
