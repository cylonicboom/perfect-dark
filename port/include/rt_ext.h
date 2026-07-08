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
} rtcamera;

// Debug view modes (gfx_rt_debug)
enum {
	RT_DEBUG_OFF = 0,
	RT_DEBUG_DEPTH,   // 1: linearized depth
	RT_DEBUG_NORMALS, // 2: reconstructed view-space normals
	RT_DEBUG_AO,      // 3: raw ambient occlusion term
	RT_DEBUG_SHADOW,  // 4: raw sun-shadow term
	RT_DEBUG_GI,      // 5: accumulated GI radiance
	RT_DEBUG_SSR,     // 6: reflection colour * confidence
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

#ifdef __cplusplus
}
#endif

#endif // PORT_RT_EXT_H
