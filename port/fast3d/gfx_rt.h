#ifndef GFX_RT_H
#define GFX_RT_H

/**
 * Screen-space raytracing suite — OpenGL implementation entry point.
 * See docs/PORT_RAYTRACING.md. Called by gfx_opengl.cpp's rt_resolve
 * rapi hook; all control globals live in rt_ext.h / gfx_pc.cpp.
 */

#include "rt_ext.h"

// Run the full post pipeline over the framebuffer identified by fbo.
// vx/vy/vw/vh   = the emitting player's viewport in fb coords (GL origin)
// fbw/fbh       = full framebuffer dimensions
// msaa          = the fb's sample count (1 = single-sampled)
// invert_y      = the fb is rendered vertically flipped (gfx_pc invert_y)
// glsl_version  = version string for shader "#version %s" (e.g. "130",
//                 "410 core") — the same one gfx_opengl compiled with
// Saves and restores every piece of GL state it touches.
void gfx_rt_resolve(const rtcamera* cam, int vx, int vy, int vw, int vh,
                    unsigned int fbo, int fbw, int fbh, int msaa,
                    bool invert_y, const char* glsl_version);

#endif // GFX_RT_H
