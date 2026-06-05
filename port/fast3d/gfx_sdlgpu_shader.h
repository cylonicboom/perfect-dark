// SDL_GPU backend shader pipeline (port-only; see docs/PORT_SDLGPU.md).
// Runtime-generates Vulkan-dialect GLSL 450 per combiner permutation
// (paralleling gfx_opengl_create_and_load_new_shader) and compiles it to
// SPIR-V with glslang. Private to gfx_sdlgpu.cpp / gfx_sdlgpu_shader.cpp.

#ifndef GFX_SDLGPU_SHADER_H
#define GFX_SDLGPU_SHADER_H

#include <stdint.h>
#include <stdbool.h>
#include <SDL3/SDL.h>

#include "gfx_cc.h"
#include "gfx_rendering_api.h"

// Backend-private shader handle behind the opaque ShaderProgram forward
// declaration (each fast3d backend defines its own; never crosses backends).
struct ShaderProgram {
    uint64_t shader_id0;
    uint32_t shader_id1;
    struct CCFeatures cc;
    SDL_GPUShader *vs; // immediate-path vertex shader
    // Display-list-cache replay variant: consumes the trailing aShadeIdx
    // attribute and resolves live shade colours from the palette texture
    // (vertex sampler set=0). Compiled lazily on first cache_draw; aliases
    // `vs` when the permutation has no combiner inputs (no palette to route —
    // only the pipeline's vertex stride differs then).
    SDL_GPUShader *vs_cached;
    SDL_GPUShader *fs;
    uint8_t num_inputs;
    bool used_textures[2];
    uint8_t num_floats;       // floats per vertex (immediate vertex layout)
    uint8_t num_attribs;
    uint8_t attrib_sizes[16]; // per-attribute float counts (1/2/3/4), in buffer order
    uint8_t fs_sampler_count; // 0..2 compact fragment sampler bindings
    int8_t tex_binding[2];    // FS sampler binding for uTex0/uTex1, -1 if unused
};

// VS push-uniform block, std140, set=1 binding=0 (SDL_GPU vertex uniform
// slot 0). Must match the block emitted by the generator field-for-field.
struct GfxSdlGpuVSUni {
    float mvp[16];        // uMVP, column-major
    int32_t use_vertex_fog;
    float fog_mul;
    float fog_off;
    int32_t palette_enable; // display-list cache palette (Phase 2); 0 = baked
    float palette_w;
    int32_t shade_route;
    int32_t pad[2];       // std140 block size rounds to 16
};

// FS push-uniform block, std140, set=3 binding=0 (fragment uniform slot 0).
struct GfxSdlGpuFSUni {
    int32_t frame_count;
    float noise_scale;
    int32_t three_point_filter0;
    int32_t three_point_filter1;
    float wireframe_color[4];
    float emissive; // HDR dazzle multiplier on the final colour (1 = none)
    float pad[3];
};

// Call once after the SDL_GPU device exists: initializes glslang, picks the
// device's shader format (SPIR-V on Vulkan; DXBC on D3D12 / MSL on Metal via
// SPIRV-Cross when built with GFX_SDLGPU_HAS_SPIRV_CROSS), and loads the disk
// shader cache.
void gfx_sdlgpu_shader_init(SDL_GPUDevice *device);

// Generate + compile the shader pair for a combiner permutation. filter_mode
// affects fragment-shader codegen (three-point/blur), mirroring the GL
// backend; gfx_pc clears all shaders whenever the filter mode changes.
// Returns false (with logging) on failure.
bool gfx_sdlgpu_shader_compile(SDL_GPUDevice *device, uint64_t shader_id0, uint32_t shader_id1,
                               enum FilteringMode filter_mode, struct ShaderProgram *prg);

// Lazily compile the display-list-cache replay vertex-shader variant into
// prg->vs_cached (idempotent). Returns false (with logging) on failure.
bool gfx_sdlgpu_shader_compile_cached_vs(SDL_GPUDevice *device, struct ShaderProgram *prg);

// Compile a fixed (non-generated) GLSL450 shader pair through the same
// glslang -> device-format pipeline as the combiner shaders. Used for the
// HDR present pass. Resource counts must match the sources' declarations.
bool gfx_sdlgpu_shader_compile_fixed(SDL_GPUDevice *device, const char *vs_src, const char *fs_src,
                                     unsigned int vs_ubos, unsigned int fs_samplers, unsigned int fs_ubos,
                                     SDL_GPUShader **out_vs, SDL_GPUShader **out_fs);

// "SPIR-V" / "DXBC" / "MSL" (the format picked at init), for diagnostics.
const char *gfx_sdlgpu_shader_format_name(void);

// number of entries loaded/compiled into the disk shader cache, for diagnostics
int gfx_sdlgpu_shader_cache_count(void);

#endif
