#ifndef GFX_RT_COMMON_H
#define GFX_RT_COMMON_H

/**
 * Screen-space raytracing suite — shared pass algorithms + CPU helpers
 * (docs/PORT_RAYTRACING.md). Included by BOTH backend implementations:
 * gfx_rt.cpp (OpenGL, GLSL 130 loose uniforms) and gfx_sdlgpu.cpp (GLSL 450
 * Vulkan dialect, set/binding layouts + one std140 UBO whose MEMBERS carry
 * the same bare names) — which is exactly why the strings below contain no
 * declarations: each backend prepends its own prelude declaring vUV/oCol,
 * the samplers and the uniform names these bodies reference.
 *
 * Uniform names referenced by the bodies (a backend prelude must declare all
 * the ones its pass uses): uProj (vec4: tanHalfFovX, tanHalfFovY, znear,
 * zfar), uRect (vec4 viewport uv rect: min.xy, max.zw), uYSign, uTexel
 * (vec2), uFrame (int), uCurToPrev (mat4), uSun/uSky (vec3), uBlend, uDir
 * (vec2), uAORadius, uShadowLen, uGIRadius, uMaxDist, uAOInt, uShInt,
 * uGIInt, uSSRInt, the int toggles/budgets uAOOn uShadowOn uGIOn uSSROn
 * uMode uAOSamples uShadowSteps uRays uSteps uBounces uSSRSteps, and the
 * dark/relight set: uLightPosRad[RT_MAX_LIGHTS] (vec4, view-space xyz +
 * radius), uLightCol[RT_MAX_LIGHTS] (vec4, premultiplied rgb), uLightCount,
 * uLightShadows, uLightSteps, uTorch (int), uTorchInt, uTorchRange, uDark
 * (int), uDarkAmbient, uLightsOn (int).
 * Samplers per pass: prepass uDepth; aoshadow uNorm; trace uNorm+uColor;
 * temporal uNorm+uCur+uHist; blur uNorm+uSrc; ssr uNorm+uColor; light uNorm;
 * comp_mul uAO; comp_add uColor+uGI+uSSR+uLight; debug uNorm+uAO+uGI+uSSR+
 * uLight.
 */

#include <string.h>
#include <math.h>

#ifndef RT_MAX_PLAYERS
#define RT_MAX_PLAYERS 4
#endif

// quality presets: sample/step budgets per gfx_rt_quality
static const struct {
    int ao_samples, shadow_steps, ssr_steps;
    int gi_rays, gi_steps, pt_rays, pt_bounces;
    int light_steps; // per-light shadow-ray march (dark/relight mode)
} kQuality[3] = {
    {  6, 12, 20, 4,  8, 1, 2,  8 }, // low
    { 10, 20, 32, 6, 12, 2, 3, 12 }, // medium
    { 16, 28, 48, 8, 16, 3, 3, 16 }, // high
};

// ---------------------------------------------------------------------------
// small matrix helpers (column-major 4x4, GL convention)

static inline void mtxMul(float* out, const float* a, const float* b) { // out = a*b
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
static inline void mtxRigidInverse(float* out, const float* m) {
    float r[16];
    for (int c = 0; c < 3; c++) {
        for (int rr = 0; rr < 3; rr++) {
            r[c * 4 + rr] = m[rr * 4 + c];
        }
    }
    r[3] = r[7] = r[11] = 0.0f;
    const float tx = m[12], ty = m[13], tz = m[14];
    r[12] = -(r[0] * tx + r[4] * ty + r[8] * tz);
    r[13] = -(r[1] * tx + r[5] * ty + r[9] * tz);
    r[14] = -(r[2] * tx + r[6] * ty + r[10] * tz);
    r[15] = 1.0f;
    memcpy(out, r, sizeof(r));
}

// GL-convention perspective (ndc z in [-1,1]). Only used to reproject xy/w —
// prevClip.w = -z_view — so it serves both clip conventions.
static inline void mtxPerspective(float* out, float fovy_deg, float aspect, float zn, float zf) {
    memset(out, 0, sizeof(float) * 16);
    const float t = tanf(fovy_deg * (float)(3.14159265358979 / 180.0) * 0.5f);
    out[0] = 1.0f / (t * aspect);
    out[5] = 1.0f / t;
    out[10] = -(zf + zn) / (zf - zn);
    out[11] = -1.0f;
    out[14] = -2.0f * zf * zn / (zf - zn);
}

// ---------------------------------------------------------------------------
// shared GLSL: reconstruction + sampling helpers (no declarations — see top)

static const char* const RT_GLSL_HELPERS =
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

// prepass: depth -> view-space normal + linear depth (samples uDepth)
static const char* const RT_FS_PREPASS_BODY =
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
// out: r = ambient visibility, g = sun visibility. uSun is view-space here.
static const char* const RT_FS_AOSHADOW_BODY =
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
static const char* const RT_FS_TRACE_BODY =
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

// temporal accumulation with depth-validated reprojection (uCur, uHist)
static const char* const RT_FS_TEMPORAL_BODY =
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

// separable depth-aware blur (uSrc; uDir = one-texel step along the blur axis)
static const char* const RT_FS_BLUR_BODY =
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
static const char* const RT_FS_SSR_BODY =
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

// Dynamic-light radiance (dark/relight mode): point lights harvested from the
// map's glare lights, each with its own screen-space shadow march, plus the
// camera-mounted test torch. Lights arrive in VIEW space (transformed CPU-
// side), colours premultiplied by intensity. The torch casts no shadow ray on
// purpose: along the eye ray the depth buffer IS the first hit, so a light at
// the camera can never be occluded — physically free of shadow acne.
static const char* const RT_FS_LIGHT_BODY =
    "void main() {\n"
    "    vec4 nz = texture(uNorm, vUV);\n"
    "    if (nz.w <= 0.0) { oCol = vec4(0.0); return; }\n"
    "    vec3 n = nz.xyz;\n"
    "    vec3 P = vpos(vUV, nz.w);\n"
    "    vec2 seed = gl_FragCoord.xy + vec2(float(uFrame) * 13.7, float(uFrame) * 5.3);\n"
    "    vec3 acc = vec3(0.0);\n"
    "    for (int i = 0; i < uLightCount; i++) {\n"
    "        vec3 toL = uLightPosRad[i].xyz - P;\n"
    "        float rad = uLightPosRad[i].w;\n"
    "        float d2 = dot(toL, toL);\n"
    "        if (d2 > rad * rad) continue;\n"
    "        float d = sqrt(d2);\n"
    "        vec3 L = toL / max(d, 0.001);\n"
    "        float ndl = dot(n, L);\n"
    "        if (ndl <= 0.0) continue;\n"
    "        float att = 1.0 - d / rad;\n"
    "        att *= att;\n"
    "        float contrib = ndl * att;\n"
    "        if (contrib < 0.004) continue;\n"
    "        float vis = 1.0;\n"
    "        if (uLightShadows != 0) {\n"
    "            float dt = d / float(uLightSteps);\n"
    "            float t = dt * (0.4 + 0.6 * h12(seed + vec2(float(i) * 7.3, 3.1)));\n"
    "            for (int s = 0; s < uLightSteps; s++) {\n"
    "                if (t >= d - 2.0) break;\n" // reached the light
    "                vec3 S = P + L * t;\n"
    "                t += dt;\n"
    "                if (S.z > -uProj.z) break;\n"
    "                vec2 uv2 = puv(S);\n"
    "                if (!inrect(uv2)) break;\n"
    "                float sz = texture(uNorm, uv2).w;\n"
    "                float rz = -S.z;\n"
    "                if (sz > 0.0 && rz - sz > 1.5 && rz - sz < 4.0 + t * 0.12) { vis = 0.0; break; }\n"
    "            }\n"
    "        }\n"
    "        acc += uLightCol[i].rgb * (contrib * vis);\n"
    "    }\n"
    "    if (uTorch != 0) {\n"
    "        float d = length(P);\n"
    "        vec3 L = -P / max(d, 0.001);\n"     // surface -> camera
    "        float ndl = max(dot(n, L), 0.0);\n"
    "        float ca = -P.z / max(d, 0.001);\n" // cos(angle to the view axis)
    "        float cone = smoothstep(0.80, 0.93, ca);\n"
    "        float att = clamp(1.0 - d / uTorchRange, 0.0, 1.0);\n"
    "        att *= att;\n"
    "        acc += vec3(1.0, 0.97, 0.9) * (ndl * att * cone * uTorchInt);\n"
    "    }\n"
    "    oCol = vec4(acc, 1.0);\n"
    "}\n";

// multiplicative composite: dst *= AO * shadow * dark-ambient
// (blend dst_new = src * dst)
static const char* const RT_FS_COMP_MUL_BODY =
    "void main() {\n"
    "    vec2 aosh = texture(uAO, vUV).rg;\n"
    "    float m = 1.0;\n"
    "    if (uAOOn != 0) m *= mix(1.0, aosh.r * aosh.r, uAOInt);\n"
    "    if (uShadowOn != 0) m *= 1.0 - uShInt * (1.0 - aosh.g);\n"
    "    if (uDark != 0) m *= uDarkAmbient;\n"
    "    oCol = vec4(vec3(m), 1.0);\n"
    "}\n";

// additive composite: dst += GI * albedo + SSR + lights * albedo
// (blend ONE, ONE). In dark mode the GI/SSR terms are scaled down: they
// sample the scene captured BEFORE the darkening, so unscaled they'd glow
// with the pre-dark world. The dynamic-light term uses the same bright
// capture as albedo deliberately — that's what the lights re-illuminate.
static const char* const RT_FS_COMP_ADD_BODY =
    "void main() {\n"
    "    vec3 add = vec3(0.0);\n"
    "    float darkscale = (uDark != 0) ? min(uDarkAmbient * 2.0, 1.0) : 1.0;\n"
    "    if (uGIOn != 0) {\n"
    "        vec3 albedo = texture(uColor, vUV).rgb;\n"
    "        add += texture(uGI, vUV).rgb * albedo * uGIInt * darkscale;\n"
    "    }\n"
    "    if (uSSROn != 0) {\n"
    "        vec4 ssr = texture(uSSR, vUV);\n"
    "        add += ssr.rgb * ssr.a * uSSRInt * darkscale;\n"
    "    }\n"
    "    if (uLightsOn != 0) {\n"
    "        add += texture(uLight, vUV).rgb * texture(uColor, vUV).rgb;\n"
    "    }\n"
    "    // tiny dither to keep the additive gradient from banding on RGB8\n"
    "    add += (h12(gl_FragCoord.xy) - 0.5) / 255.0;\n"
    "    oCol = vec4(max(add, 0.0), 0.0);\n"
    "}\n";

// debug views (replaces the scene in the viewport)
static const char* const RT_FS_DEBUG_BODY =
    "void main() {\n"
    "    vec4 nz = texture(uNorm, vUV);\n"
    "    vec3 c = vec3(1.0, 0.0, 1.0);\n"
    "    if (uMode == 1) c = vec3(clamp(nz.w / uProj.w, 0.0, 1.0));\n"
    "    else if (uMode == 2) c = nz.xyz * 0.5 + 0.5;\n"
    "    else if (uMode == 3) c = vec3(texture(uAO, vUV).r);\n"
    "    else if (uMode == 4) c = vec3(texture(uAO, vUV).g);\n"
    "    else if (uMode == 5) c = texture(uGI, vUV).rgb;\n"
    "    else if (uMode == 6) { vec4 s = texture(uSSR, vUV); c = s.rgb * s.a; }\n"
    "    else if (uMode == 7) c = texture(uLight, vUV).rgb;\n"
    "    oCol = vec4(c, 1.0);\n"
    "}\n";

#endif // GFX_RT_COMMON_H
