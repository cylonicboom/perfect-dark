#ifndef GFX_OPENGL_H
#define GFX_OPENGL_H

#include "gfx_rendering_api.h"

extern struct GfxRenderingAPI gfx_opengl_api;

// A20: the GL state snapshot the RT suite needs before its passes, filled
// from gfx_opengl's CPU-side value shadows instead of ~24 glGet* round-trips
// per resolve per player (zero queries — even the fbo binding comes from the
// tracked current framebuffer). Field meanings mirror gfx_rt.cpp's
// RtGLState; rtSaveState copies this into it and rtRestoreState is
// unchanged, so the restore semantics the RT suite was verified with are
// untouched.
struct GfxGlRtState {
	int draw_fbo, read_fbo;
	int viewport[4];
	int scissor_box[4];
	unsigned char scissor_test, depth_test, blend, cull;
	unsigned char depth_mask;
	int depth_func;
	int blend_src_rgb, blend_dst_rgb, blend_src_a, blend_dst_a;
	int program;
	int active_texture;
	int tex_binding[8];
	int vao, array_buffer;
};

void gfx_opengl_get_rt_state(struct GfxGlRtState* s);

#endif
