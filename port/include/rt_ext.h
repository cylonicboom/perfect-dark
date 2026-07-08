#ifndef PORT_RT_EXT_H
#define PORT_RT_EXT_H

/**
 * Screen-space raytracing suite (port-only; see docs/PORT_RAYTRACING.md).
 *
 * This header is shared between decompiled game code (player.c fills the
 * camera snapshot + emits G_RTRESOLVE_EXT; net.c drives the /rt console
 * command; video.c registers the Video.RT.* config keys) and the fast3d
 * renderer (gfx_pc.cpp defines the globals, gfx_rt.cpp implements the
 * OpenGL pipeline). Keep it plain C.
 */

#ifdef __cplusplus
extern "C" {
#endif

// Dynamic light source for the /rt dark relight mode: harvested from the
// map's room lights (the same data the glare/lens-flare artifacts draw from)
// by rtCollectLights below. World-space; the renderer transforms to view
// space per player. NOTE: the shader-side array sizes are LITERALS in both
// backend preludes (gfx_rt.cpp kFSCommon, gfx_sdlgpu.cpp lights_ubo) — keep
// them in sync when changing this.
#define RT_MAX_LIGHTS 32
typedef struct rtlight {
	float pos[3];    // world position (light bbox average + room pos)
	float radius;    // falloff radius, world units
	float color[3];  // 0..1 rgb (the light's 4/4/4/4 colour nibbles)
	float intensity; // per-light gain (brightnessmult/32 = nominal 1.0)
} rtlight;

// Camera snapshot handed from game code to the renderer through the
// G_RTRESOLVE_EXT display-list command (w1 = pointer to one of these).
// One static instance per local player slot, refilled every frame in
// playerRenderHud, so the pointer is stable until gfx_run consumes it.
typedef struct rtcamera {
	float viewmtx[16]; // world->view (worldtoscreenmtx), column-major — PD's
	                   // Mtxf m[i][j] flattens to GL column-major as-is
	float fovy;        // vertical FOV in degrees (viGetFovY)
	float aspect;      // projection aspect (viGetAspect)
	float znear;       // viGetZRange
	float zfar;
	int playernum;     // local player index 0-3 (temporal history slot)
	int valid;         // 0 = worldtoscreenmtx was NULL; renderer skips
	int lightcount;    // entries filled in lights[] (0 = no map lights)
	rtlight lights[RT_MAX_LIGHTS];
	// Skylight: global light derived from the stage's live sky colour
	// (rtComputeSkyLight): warm skies keep their own hue (sunset), bright
	// cool skies map to warm-white sunlight (day), dark cool skies to dim
	// moon-blue (night). Tints the dark-mode ambient + the GI sky term.
	float skylight[3]; // hue x intensity, 0..~1 per channel
	int skylight_ok;   // 0 = black sky (indoor stage) — renderer stays neutral
} rtcamera;

// Game-side collector (artifact.c, port-only): fills out[] with the nearest
// lit ("on" + healthy — shot-out lights don't illuminate) room lights around
// campos (float[3] world), sorted nearest-first. Returns the count (<= max,
// max clamped to RT_MAX_LIGHTS). Only loaded rooms carry light data.
int rtCollectLights(const float* campos, rtlight* out, int max);

// Game-side skylight derivation (artifact.c, port-only): maps the stage's
// live sky colour (envGetCurrent) to a light colour per the rules on the
// rtcamera.skylight field. Writes hue*intensity into out[3]; *ok = 0 for a
// black sky (indoor stage).
void rtComputeSkyLight(float out[3], int* ok);

// Debug view modes (gfx_rt_debug)
enum {
	RT_DEBUG_OFF = 0,
	RT_DEBUG_DEPTH,   // 1: linearized depth
	RT_DEBUG_NORMALS, // 2: reconstructed view-space normals
	RT_DEBUG_AO,      // 3: raw ambient occlusion term
	RT_DEBUG_SHADOW,  // 4: raw sun-shadow term
	RT_DEBUG_GI,      // 5: accumulated GI radiance
	RT_DEBUG_SSR,     // 6: reflection colour * confidence
	RT_DEBUG_LIGHT,   // 7: dynamic-light radiance (map lights + torch)
	RT_DEBUG_MAX
};

// GI modes (gfx_rt_gi)
enum {
	RT_GI_OFF = 0,
	RT_GI_SSGI,      // 1: single-bounce screen-space GI
	RT_GI_PATHTRACE, // 2: stochastic multi-bounce path trace + temporal accum
};

// Master + per-effect toggles. int, not bool: game-side bool is 4 bytes vs
// the renderer's 1 — see the gfx_wireframe_mode note in docs/PORT_WIREFRAME.md.
extern int gfx_rt_enabled;   // master switch (nothing runs when 0)
extern int gfx_rt_ao;        // raymarched ambient occlusion
extern int gfx_rt_shadows;   // screen-space directional (sun) shadows
extern int gfx_rt_ssr;       // screen-space raytraced reflections
extern int gfx_rt_gi;        // RT_GI_* mode
extern int gfx_rt_debug;     // RT_DEBUG_* view
extern int gfx_rt_quality;   // 0 low / 1 medium / 2 high (sample+step budgets)

extern float gfx_rt_ao_intensity;     // 0..1 darkening amount
extern float gfx_rt_ao_radius;        // world units (PD units, ~100/m)
extern float gfx_rt_shadow_intensity; // 0..1
extern float gfx_rt_shadow_length;    // world units of shadow ray march
extern float gfx_rt_ssr_intensity;    // 0..1 reflection blend
extern float gfx_rt_gi_intensity;     // 0..4 bounce-light gain
extern float gfx_rt_gi_scale;         // internal res of the GI/PT pass (0.25..1)
extern float gfx_rt_sun_dir[3];       // world-space direction TOWARD the light
extern float gfx_rt_sky[3];           // sky/miss radiance for GI/PT rays

// Dark / relight mode ("blacken the world, illuminate from map lights"):
// the multiplicative composite crushes the scene to dark_ambient, and a
// per-light raytraced lighting pass (point lights from rtCollectLights, each
// with its own screen-space shadow march, plus an optional camera torch)
// re-illuminates additively using the captured scene colour as albedo.
extern int gfx_rt_dark;               // crush the scene to the ambient floor
extern float gfx_rt_dark_ambient;     // 0..1 remaining base brightness
extern int gfx_rt_lights;             // harvest + render map (glare) lights
extern int gfx_rt_light_shadows;      // per-light screen-space shadow rays
extern float gfx_rt_light_intensity;  // global gain on map lights
extern float gfx_rt_light_radius;     // falloff radius per light, world units
extern float gfx_rt_light_cull;       // harvest range beyond the radius: a
                                      // light is collected within radius +
                                      // cull of the CAMERA, so fixtures far
                                      // from you still light surfaces you see
extern float gfx_rt_light_max;        // per-light brightness cap (hue-
                                      // preserving) so close-range lights in
                                      // small rooms don't blow out to white
extern int gfx_rt_skylight;           // derive ambient tint + GI sky from the
                                      // stage's sky colour (day/sunset/night)
extern float gfx_rt_skylight_gain;    // skylight -> GI miss-radiance scale
extern int gfx_rt_torch;              // camera-mounted test spotlight
extern float gfx_rt_torch_intensity;
extern float gfx_rt_torch_range;      // world units

#ifdef __cplusplus
}
#endif

#endif // PORT_RT_EXT_H
