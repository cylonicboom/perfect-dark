#ifndef GFX_RETRO_COMMON_H
#define GFX_RETRO_COMMON_H

/**
 * Chaos retro/post filter — the fragment BODY shared by both backends
 * (docs/PORT_CHAOS.md). Declaration-free GLSL: each backend prepends its own
 * prelude declaring vUV / oCol / uColor and the uniforms below (loose
 * uniforms on GL 130+, one std140 UBO on SDL_GPU 450 — member names must
 * match exactly):
 *
 *   vec2  uGrid    pixelation grid (e.g. 160x120); x <= 0 = no snap
 *   float uLevels  greyscale level count for uMode 1
 *   int   uMode    0 keep colours, 1 grey-N, 2 RGB332, 3 invert,
 *                  4 Game Boy DMG greens, 5 thermal palette
 *   int   uFx      bitmask: 1 scanlines, 2 RGB aperture grille, 4 CRT
 *                  curvature, 8 vignette, 16 VHS, 32 underwater wobble
 *   float uWarp    fisheye lens strength (0 = off; CRT adds its own +0.12)
 *   float uAspect  framebuffer w/h (for circular radial warp)
 *   float uTime    seconds, for the animated effects (VHS jitter, wobble)
 *
 * Order: lens warp -> wobble -> VHS line jitter -> out-of-bounds black ->
 * pixel snap -> sample (VHS adds chroma shift + noise) -> colour mode ->
 * scanlines / grille / vignette. Raster-space looks (scanlines, vignette)
 * use the pre-snap uv so they curve with the tube and stay per-line under
 * pixelation. Everything is orientation-symmetric, so GL's bottom-up and
 * SDL_GPU's top-down storage need no special-casing.
 */
#define RETRO_GLSL_BODY \
    "void main() {\n" \
    "    vec2 uv = vUV;\n" \
    "    float k = uWarp + (((uFx & 4) != 0) ? 0.12 : 0.0);\n" \
    "    if (k != 0.0) {\n" \
    "        vec2 d = uv - 0.5;\n" \
    "        d.x *= uAspect;\n" \
    "        float rmax2 = 0.25 * (uAspect * uAspect + 1.0);\n" \
    "        float f = (1.0 + k * dot(d, d)) / (1.0 + k * rmax2);\n" \
    "        d *= f;\n" \
    "        d.x /= uAspect;\n" \
    "        uv = d + 0.5;\n" \
    "    }\n" \
    "    if ((uFx & 32) != 0) {\n" \
    "        uv.x += sin(uv.y * 24.0 + uTime * 2.3) * 0.006;\n" \
    "        uv.y += cos(uv.x * 21.0 + uTime * 1.7) * 0.006;\n" \
    "    }\n" \
    "    vec2 suv = uv;\n" \
    "    if ((uFx & 16) != 0) {\n" \
    "        float ln = floor(suv.y * 240.0);\n" \
    "        float h = fract(sin(ln * 12.9898 + floor(uTime * 30.0) * 78.233) * 43758.5453);\n" \
    "        float jitter = (h - 0.5) * 0.003;\n" \
    "        if (fract(suv.y * 0.7 + uTime * 0.11) > 0.965) {\n" \
    "            jitter += (h - 0.5) * 0.06;\n" \
    "        }\n" \
    "        uv.x += jitter;\n" \
    "    }\n" \
    "    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {\n" \
    "        oCol = vec4(0.0, 0.0, 0.0, 1.0);\n" \
    "        return;\n" \
    "    }\n" \
    "    if (uGrid.x > 0.5) {\n" \
    "        uv = (floor(uv * uGrid) + 0.5) / uGrid;\n" \
    "    }\n" \
    "    vec3 c;\n" \
    "    if ((uFx & 16) != 0) {\n" \
    "        c.r = texture(uColor, vec2(min(uv.x + 0.0022, 1.0), uv.y)).r;\n" \
    "        c.g = texture(uColor, uv).g;\n" \
    "        c.b = texture(uColor, vec2(max(uv.x - 0.0022, 0.0), uv.y)).b;\n" \
    "        float n = fract(sin(dot(suv * 917.0, vec2(12.9898, 78.233)) + uTime * 61.0) * 43758.5453);\n" \
    "        c += vec3((n - 0.5) * 0.07);\n" \
    "    } else {\n" \
    "        c = texture(uColor, uv).rgb;\n" \
    "    }\n" \
    "    if (uMode == 1) {\n" \
    "        float l = dot(c, vec3(0.299, 0.587, 0.114));\n" \
    "        l = floor(min(l, 0.9999) * uLevels) / (uLevels - 1.0);\n" \
    "        c = vec3(l);\n" \
    "    } else if (uMode == 2) {\n" \
    "        vec3 q = vec3(8.0, 8.0, 4.0);\n" \
    "        c = floor(min(c, vec3(0.9999)) * q) / (q - vec3(1.0));\n" \
    "    } else if (uMode == 3) {\n" \
    "        c = vec3(1.0) - c;\n" \
    "    } else if (uMode == 4) {\n" \
    "        float l = dot(c, vec3(0.299, 0.587, 0.114));\n" \
    "        float q4 = floor(min(l, 0.9999) * 4.0);\n" \
    "        c = q4 < 1.0 ? vec3(0.06, 0.22, 0.06)\n" \
    "          : (q4 < 2.0 ? vec3(0.19, 0.38, 0.19)\n" \
    "          : (q4 < 3.0 ? vec3(0.55, 0.67, 0.06) : vec3(0.61, 0.74, 0.06)));\n" \
    "    } else if (uMode == 5) {\n" \
    "        float l = dot(c, vec3(0.299, 0.587, 0.114));\n" \
    "        if (l < 0.25) c = mix(vec3(0.0, 0.0, 0.25), vec3(0.3, 0.0, 0.65), l * 4.0);\n" \
    "        else if (l < 0.5) c = mix(vec3(0.3, 0.0, 0.65), vec3(0.9, 0.25, 0.0), (l - 0.25) * 4.0);\n" \
    "        else if (l < 0.75) c = mix(vec3(0.9, 0.25, 0.0), vec3(1.0, 0.85, 0.0), (l - 0.5) * 4.0);\n" \
    "        else c = mix(vec3(1.0, 0.85, 0.0), vec3(1.0, 1.0, 1.0), (l - 0.75) * 4.0);\n" \
    "    }\n" \
    "    if ((uFx & 1) != 0) {\n" \
    "        float lines = uGrid.y > 0.5 ? uGrid.y : 480.0;\n" \
    "        c *= 0.78 + 0.22 * cos(suv.y * lines * 6.2831853);\n" \
    "    }\n" \
    "    if ((uFx & 2) != 0) {\n" \
    "        float px = mod(gl_FragCoord.x, 3.0);\n" \
    "        vec3 m = px < 1.0 ? vec3(1.0, 0.62, 0.62)\n" \
    "          : (px < 2.0 ? vec3(0.62, 1.0, 0.62) : vec3(0.62, 0.62, 1.0));\n" \
    "        c *= m * 1.25;\n" \
    "    }\n" \
    "    if ((uFx & 8) != 0) {\n" \
    "        vec2 v = suv - 0.5;\n" \
    "        c *= 1.0 - 0.45 * smoothstep(0.35, 0.72, length(v));\n" \
    "    }\n" \
    "    oCol = vec4(c, 1.0);\n" \
    "}\n"

#endif
