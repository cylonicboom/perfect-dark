// SDL_GPU backend shader pipeline (port-only; see docs/PORT_SDLGPU.md).
//
// Runtime-generates Vulkan-dialect GLSL 450 per combiner permutation and
// compiles to SPIR-V via glslang. The generator mirrors the GL backend's
// gfx_opengl_create_and_load_new_shader feature-for-feature (combiner
// formulas, fog recompute with near/behind-eye guards, three-point filter,
// blur, noise, grayscale, texel clamp, texture edge, wireframe colour) so
// the two backends produce the same image. Differences are dialect-only:
// explicit in/out locations, std140 push-uniform blocks instead of loose
// uniforms, and SDL_GPU's SPIR-V descriptor-set convention (VS samplers
// set=0, VS UBO set=1, FS samplers set=2, FS UBO set=3).

#ifdef USE_SDLGPU

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <map>
#include <vector>

#include <glslang/Include/glslang_c_interface.h>
#include <glslang/Public/resource_limits_c.h>

#ifdef GFX_SDLGPU_HAS_SPIRV_CROSS
#include <spirv_cross/spirv_cross_c.h>
#if defined(_WIN32)
#include <d3dcompiler.h> // pD3DCompile + ID3DBlob; the DLL is loaded dynamically
#endif
#endif

#include "platform.h"
#include "system.h"

#include "gfx_cc.h"
#include "gfx_sdlgpu_shader.h"

static bool s_glslang_initialized;

// The single shader format the device accepts, picked at init. glslang's
// SPIR-V is the universal intermediate; SPIRV-Cross translates it for DXBC
// (-> HLSL -> D3DCompile) and MSL devices.
static SDL_GPUShaderFormat s_shader_format = SDL_GPU_SHADERFORMAT_SPIRV;
static uint8_t s_shader_format_tag; // cache-key tag: 0 spirv, 1 dxbc, 2 msl

// ---------------------------------------------------------------------------
// Disk SPIR-V cache: one packed append-only file in the home dir. Skips the
// glslang compile (the cold-start hitch) for permutations seen on previous
// runs; SDL_CreateGPUShader still runs from the cached words. Disable with
// --no-shader-cache. BUMP THE VERSION whenever shader codegen changes — stale
// SPIR-V would otherwise be loaded for the new generator.

#define SHADER_CACHE_MAGIC 0x43534450u // 'PDSC'
#define SHADER_CACHE_VERSION 3u        // v3: uEmissive in the FS uniform block
#define SHADER_CACHE_MAX_BYTES (1u << 24)

struct ShaderCacheKey {
    uint64_t id0;
    uint32_t id1;
    uint8_t filter;  // FS codegen depends on the filter mode; 0xff for VS-only records
    uint8_t variant; // 0 = immediate vs+fs pair, 1 = cached VS only
    uint8_t fmt;     // s_shader_format_tag — blobs are device-format-specific
    bool operator<(const ShaderCacheKey &o) const {
        if (id0 != o.id0) return id0 < o.id0;
        if (id1 != o.id1) return id1 < o.id1;
        if (filter != o.filter) return filter < o.filter;
        if (variant != o.variant) return variant < o.variant;
        return fmt < o.fmt;
    }
};

struct ShaderCacheRecHdr {
    uint64_t id0;
    uint32_t id1;
    uint8_t filter;
    uint8_t variant;
    uint8_t fmt;
    uint8_t pad;
    uint32_t vs_bytes;
    uint32_t fs_bytes;
};
SDL_COMPILE_TIME_ASSERT(shader_cache_rec_hdr, sizeof(struct ShaderCacheRecHdr) == 24);

struct ShaderCacheBlobs {
    std::vector<uint8_t> vs, fs;
};

static std::map<ShaderCacheKey, ShaderCacheBlobs> s_shader_cache;
static char s_cache_path[1024];
static bool s_cache_enabled;

// rewrite the file from the in-memory map (fresh header + all records)
static void shader_cache_rewrite(void) {
    FILE *f = fopen(s_cache_path, "wb");
    if (!f) {
        sysLogPrintf(LOG_WARNING, "SDL_GPU: cannot write shader cache %s", s_cache_path);
        s_cache_enabled = false;
        return;
    }
    const uint32_t magic = SHADER_CACHE_MAGIC, ver = SHADER_CACHE_VERSION;
    fwrite(&magic, sizeof(magic), 1, f);
    fwrite(&ver, sizeof(ver), 1, f);
    for (const auto &it : s_shader_cache) {
        ShaderCacheRecHdr h;
        memset(&h, 0, sizeof(h));
        h.id0 = it.first.id0;
        h.id1 = it.first.id1;
        h.filter = it.first.filter;
        h.variant = it.first.variant;
        h.fmt = it.first.fmt;
        h.vs_bytes = (uint32_t)it.second.vs.size();
        h.fs_bytes = (uint32_t)it.second.fs.size();
        fwrite(&h, sizeof(h), 1, f);
        if (!it.second.vs.empty()) {
            fwrite(it.second.vs.data(), 1, it.second.vs.size(), f);
        }
        if (!it.second.fs.empty()) {
            fwrite(it.second.fs.data(), 1, it.second.fs.size(), f);
        }
    }
    fclose(f);
}

static void shader_cache_load(void) {
    bool valid = false;
    bool tail_corrupt = false;

    FILE *f = fopen(s_cache_path, "rb");
    if (f) {
        uint32_t magic = 0, ver = 0;
        if (fread(&magic, sizeof(magic), 1, f) == 1 && fread(&ver, sizeof(ver), 1, f) == 1 &&
            magic == SHADER_CACHE_MAGIC && ver == SHADER_CACHE_VERSION) {
            valid = true;
            for (;;) {
                ShaderCacheRecHdr h;
                if (fread(&h, sizeof(h), 1, f) != 1) {
                    break; // clean EOF (or partial header: nothing usable follows)
                }
                if (h.vs_bytes > SHADER_CACHE_MAX_BYTES || h.fs_bytes > SHADER_CACHE_MAX_BYTES) {
                    tail_corrupt = true;
                    break;
                }
                ShaderCacheBlobs b;
                b.vs.resize(h.vs_bytes);
                b.fs.resize(h.fs_bytes);
                if ((h.vs_bytes > 0 && fread(b.vs.data(), 1, h.vs_bytes, f) != h.vs_bytes) ||
                    (h.fs_bytes > 0 && fread(b.fs.data(), 1, h.fs_bytes, f) != h.fs_bytes)) {
                    tail_corrupt = true; // truncated write (crash mid-append)
                    break;
                }
                s_shader_cache[{ h.id0, h.id1, h.filter, h.variant, h.fmt }] = std::move(b);
            }
        }
        fclose(f);
    }

    if (!valid || tail_corrupt) {
        // missing/old-version/corrupt: start (or compact to) a clean file
        shader_cache_rewrite();
    }

    if (!s_shader_cache.empty()) {
        sysLogPrintf(LOG_NOTE, "SDL_GPU: shader cache: %d entries", (int)s_shader_cache.size());
    }
}

static const ShaderCacheBlobs *shader_cache_find(const ShaderCacheKey &k) {
    if (!s_cache_enabled) {
        return NULL;
    }
    auto it = s_shader_cache.find(k);
    return it == s_shader_cache.end() ? NULL : &it->second;
}

static void shader_cache_append(const ShaderCacheKey &k, const std::vector<uint8_t> &vs,
                                const std::vector<uint8_t> &fs) {
    if (!s_cache_enabled) {
        return;
    }
    s_shader_cache[k] = { vs, fs };

    FILE *f = fopen(s_cache_path, "ab");
    if (!f) {
        return;
    }
    ShaderCacheRecHdr h;
    memset(&h, 0, sizeof(h));
    h.id0 = k.id0;
    h.id1 = k.id1;
    h.filter = k.filter;
    h.variant = k.variant;
    h.fmt = k.fmt;
    h.vs_bytes = (uint32_t)vs.size();
    h.fs_bytes = (uint32_t)fs.size();
    fwrite(&h, sizeof(h), 1, f);
    if (!vs.empty()) {
        fwrite(vs.data(), 1, vs.size(), f);
    }
    if (!fs.empty()) {
        fwrite(fs.data(), 1, fs.size(), f);
    }
    fclose(f);
}

void gfx_sdlgpu_shader_init(SDL_GPUDevice *device) {
    if (!s_glslang_initialized) {
        glslang_initialize_process();
        s_glslang_initialized = true;

        const SDL_GPUShaderFormat fmts = SDL_GetGPUShaderFormats(device);
        if (fmts & SDL_GPU_SHADERFORMAT_SPIRV) {
            s_shader_format = SDL_GPU_SHADERFORMAT_SPIRV;
            s_shader_format_tag = 0;
        } else if (fmts & SDL_GPU_SHADERFORMAT_DXBC) {
            s_shader_format = SDL_GPU_SHADERFORMAT_DXBC;
            s_shader_format_tag = 1;
        } else if (fmts & SDL_GPU_SHADERFORMAT_MSL) {
            s_shader_format = SDL_GPU_SHADERFORMAT_MSL;
            s_shader_format_tag = 2;
        } else {
            sysFatalError("SDL_GPU: device accepts none of the supported shader formats (%x)", fmts);
        }
        sysLogPrintf(LOG_NOTE, "SDL_GPU: shader format: %s",
                     s_shader_format_tag == 0 ? "SPIR-V" : s_shader_format_tag == 1 ? "DXBC" : "MSL");

        s_cache_enabled = !sysArgCheck("--no-shader-cache");
        if (s_cache_enabled) {
            char home[960];
            home[0] = '\0';
            sysGetHomePath(home, sizeof(home) - 1);
            snprintf(s_cache_path, sizeof(s_cache_path), "%s/shadercache_sdlgpu.bin", home);
            shader_cache_load();
        }
    }
}

const char *gfx_sdlgpu_shader_format_name(void) {
    return s_shader_format_tag == 0 ? "SPIR-V" : s_shader_format_tag == 1 ? "DXBC" : "MSL";
}

int gfx_sdlgpu_shader_cache_count(void) {
    return s_cache_enabled ? (int)s_shader_cache.size() : -1;
}

// ---------------------------------------------------------------------------
// SPIR-V -> device-format translation

#ifdef GFX_SDLGPU_HAS_SPIRV_CROSS

#if defined(_WIN32)
// HLSL -> DXBC via the system d3dcompiler (ships with Windows 10+); loaded
// dynamically so there is no link-time dependency.
static pD3DCompile d3dcompile_load(void) {
    static pD3DCompile fn;
    static bool tried;
    if (!tried) {
        tried = true;
        SDL_SharedObject *dll = SDL_LoadObject("d3dcompiler_47.dll");
        if (dll) {
            fn = (pD3DCompile)SDL_LoadFunction(dll, "D3DCompile");
        }
        if (!fn) {
            sysLogPrintf(LOG_ERROR, "SDL_GPU: could not load d3dcompiler_47.dll: %s", SDL_GetError());
        }
    }
    return fn;
}
#endif

// Translate SPIR-V to the device format via SPIRV-Cross. Our GLSL already
// uses SDL_GPU's descriptor-set convention (VS: samplers set 0 / UBO set 1,
// FS: samplers set 2 / UBO set 3), which SPIRV-Cross's SM 5.1 HLSL backend
// maps 1:1 to register(xN, spaceM) — exactly SDL_GPU's D3D12 register order.
static bool spirv_translate(glslang_stage_t stage, const std::vector<unsigned int> &spirv,
                            std::vector<uint8_t> &out, const char *what, uint64_t id0, uint32_t id1) {
    spvc_context ctx = NULL;
    if (spvc_context_create(&ctx) != SPVC_SUCCESS) {
        return false;
    }

    spvc_parsed_ir ir = NULL;
    spvc_compiler comp = NULL;
    spvc_compiler_options opts = NULL;
    const spvc_backend backend = s_shader_format == SDL_GPU_SHADERFORMAT_DXBC ? SPVC_BACKEND_HLSL : SPVC_BACKEND_MSL;
    const char *source = NULL;
    bool ok = false;

    if (spvc_context_parse_spirv(ctx, spirv.data(), spirv.size(), &ir) == SPVC_SUCCESS &&
        spvc_context_create_compiler(ctx, backend, ir, SPVC_CAPTURE_MODE_TAKE_OWNERSHIP, &comp) == SPVC_SUCCESS &&
        spvc_compiler_create_compiler_options(comp, &opts) == SPVC_SUCCESS) {
        if (backend == SPVC_BACKEND_HLSL) {
            spvc_compiler_options_set_uint(opts, SPVC_COMPILER_OPTION_HLSL_SHADER_MODEL, 51);
        } else {
            spvc_compiler_options_set_uint(opts, SPVC_COMPILER_OPTION_MSL_VERSION, SPVC_MAKE_MSL_VERSION(2, 1, 0));
            // SDL_GPU Metal binding model (best-effort; Metal is untestable
            // here): uniform buffers from [[buffer(0)]], textures/samplers by
            // binding index. SDL binds vertex data buffers at high indices,
            // so the low buffer slots are free for uniforms.
            const SpvExecutionModel model =
                stage == GLSLANG_STAGE_VERTEX ? SpvExecutionModelVertex : SpvExecutionModelFragment;
            for (unsigned set = 0; set < 4; set++) {
                for (unsigned binding = 0; binding < 2; binding++) {
                    spvc_msl_resource_binding rb;
                    spvc_msl_resource_binding_init(&rb);
                    rb.stage = model;
                    rb.desc_set = set;
                    rb.binding = binding;
                    rb.msl_buffer = binding;
                    rb.msl_texture = binding;
                    rb.msl_sampler = binding;
                    spvc_compiler_msl_add_resource_binding(comp, &rb);
                }
            }
        }
        if (spvc_compiler_install_compiler_options(comp, opts) == SPVC_SUCCESS &&
            spvc_compiler_compile(comp, &source) == SPVC_SUCCESS && source) {
            ok = true;
        }
    }

    if (!ok) {
        sysLogPrintf(LOG_ERROR, "SDL_GPU: SPIRV-Cross failed (%s, ID %llx, %x): %s", what,
                     (unsigned long long)id0, id1, spvc_context_get_last_error_string(ctx));
        spvc_context_destroy(ctx);
        return false;
    }

    if (s_shader_format == SDL_GPU_SHADERFORMAT_MSL) {
        // MSL is consumed as source text
        out.assign((const uint8_t *)source, (const uint8_t *)source + strlen(source));
        spvc_context_destroy(ctx);
        return true;
    }

#if defined(_WIN32)
    // HLSL -> DXBC
    pD3DCompile compile = d3dcompile_load();
    if (!compile) {
        spvc_context_destroy(ctx);
        return false;
    }
    const char *target = stage == GLSLANG_STAGE_VERTEX ? "vs_5_1" : "ps_5_1";
    ID3DBlob *code = NULL, *errors = NULL;
    const HRESULT hr = compile(source, strlen(source), NULL, NULL, NULL, "main", target, 0, 0, &code, &errors);
    if (FAILED(hr) || !code) {
        sysLogPrintf(LOG_ERROR, "SDL_GPU: D3DCompile failed (%s, ID %llx, %x, hr %lx):\n%s\n%s", what,
                     (unsigned long long)id0, id1, (unsigned long)hr,
                     errors ? (const char *)errors->GetBufferPointer() : "(no error blob)", source);
        if (errors) {
            errors->Release();
        }
        if (code) {
            code->Release();
        }
        spvc_context_destroy(ctx);
        return false;
    }
    out.assign((const uint8_t *)code->GetBufferPointer(),
               (const uint8_t *)code->GetBufferPointer() + code->GetBufferSize());
    code->Release();
    if (errors) {
        errors->Release();
    }
    spvc_context_destroy(ctx);
    return true;
#else
    spvc_context_destroy(ctx);
    return false; // DXBC translation is Windows-only
#endif
}

#endif // GFX_SDLGPU_HAS_SPIRV_CROSS

// SPIR-V words -> final device-format blob
static bool spirv_to_blob(glslang_stage_t stage, const std::vector<unsigned int> &spirv, std::vector<uint8_t> &out,
                          const char *what, uint64_t id0, uint32_t id1) {
    if (s_shader_format == SDL_GPU_SHADERFORMAT_SPIRV) {
        out.assign((const uint8_t *)spirv.data(), (const uint8_t *)spirv.data() + spirv.size() * sizeof(unsigned int));
        return true;
    }
#ifdef GFX_SDLGPU_HAS_SPIRV_CROSS
    return spirv_translate(stage, spirv, out, what, id0, id1);
#else
    (void)stage;
    (void)what;
    (void)id0;
    (void)id1;
    sysLogPrintf(LOG_ERROR, "SDL_GPU: built without SPIRV-Cross, cannot produce non-SPIR-V shaders");
    return false;
#endif
}

// final blob -> SDL_GPUShader
static SDL_GPUShader *blob_to_shader(SDL_GPUDevice *device, SDL_GPUShaderStage stage,
                                     const std::vector<uint8_t> &blob, Uint32 num_samplers, Uint32 num_ubos) {
    SDL_GPUShaderCreateInfo ci;
    SDL_zero(ci);
    ci.code_size = blob.size();
    ci.code = blob.data();
    // SPIRV-Cross's MSL backend renames the entry point to main0
    ci.entrypoint = s_shader_format == SDL_GPU_SHADERFORMAT_MSL ? "main0" : "main";
    ci.format = s_shader_format;
    ci.stage = stage;
    ci.num_samplers = num_samplers;
    ci.num_uniform_buffers = num_ubos;
    return SDL_CreateGPUShader(device, &ci);
}

// defined below, after the generator helpers
static bool compile_stage(const char *src, glslang_stage_t stage, std::vector<unsigned int> &spirv,
                          const char *what, uint64_t shader_id0, uint32_t shader_id1);

bool gfx_sdlgpu_shader_compile_fixed(SDL_GPUDevice *device, const char *vs_src, const char *fs_src,
                                     unsigned int vs_ubos, unsigned int fs_samplers, unsigned int fs_ubos,
                                     SDL_GPUShader **out_vs, SDL_GPUShader **out_fs) {
    std::vector<unsigned int> vs_spirv, fs_spirv;
    if (!compile_stage(vs_src, GLSLANG_STAGE_VERTEX, vs_spirv, "fixed vertex", 0, 0) ||
        !compile_stage(fs_src, GLSLANG_STAGE_FRAGMENT, fs_spirv, "fixed fragment", 0, 0)) {
        return false;
    }
    std::vector<uint8_t> vs_blob, fs_blob;
    if (!spirv_to_blob(GLSLANG_STAGE_VERTEX, vs_spirv, vs_blob, "fixed vertex", 0, 0) ||
        !spirv_to_blob(GLSLANG_STAGE_FRAGMENT, fs_spirv, fs_blob, "fixed fragment", 0, 0)) {
        return false;
    }
    *out_vs = blob_to_shader(device, SDL_GPU_SHADERSTAGE_VERTEX, vs_blob, 0, vs_ubos);
    *out_fs = blob_to_shader(device, SDL_GPU_SHADERSTAGE_FRAGMENT, fs_blob, fs_samplers, fs_ubos);
    if (!*out_vs || !*out_fs) {
        sysLogPrintf(LOG_ERROR, "SDL_GPU: fixed shader creation failed: %s", SDL_GetError());
        if (*out_vs) {
            SDL_ReleaseGPUShader(device, *out_vs);
            *out_vs = NULL;
        }
        if (*out_fs) {
            SDL_ReleaseGPUShader(device, *out_fs);
            *out_fs = NULL;
        }
        return false;
    }
    return true;
}

static void append_str(char *buf, size_t *len, const char *str) {
    while (*str != '\0') {
        buf[(*len)++] = *str++;
    }
}

static void append_line(char *buf, size_t *len, const char *str) {
    while (*str != '\0') {
        buf[(*len)++] = *str++;
    }
    buf[(*len)++] = '\n';
}

// Identical to the GL backend's noise expression; gl_FragCoord has a top-left
// origin on SDL_GPU (vs bottom-left on GL), so the dither pattern is mirrored
// vertically relative to GL — a documented cosmetic difference.
#define RAND_NOISE "((random(vec3(floor(gl_FragCoord.xy * noise_scale), float(frame_count))) + 1.0) / 2.0)"

static const char *shader_item_to_str(uint32_t item, bool with_alpha, bool only_alpha, bool inputs_have_alpha,
                                      bool hint_single_element) {
    if (!only_alpha) {
        switch (item) {
            case SHADER_0:
                return with_alpha ? "vec4(0.0, 0.0, 0.0, 0.0)" : "vec3(0.0, 0.0, 0.0)";
            case SHADER_1:
                return with_alpha ? "vec4(1.0, 1.0, 1.0, 1.0)" : "vec3(1.0, 1.0, 1.0)";
            case SHADER_INPUT_1:
                return with_alpha || !inputs_have_alpha ? "vInput1" : "vInput1.rgb";
            case SHADER_INPUT_2:
                return with_alpha || !inputs_have_alpha ? "vInput2" : "vInput2.rgb";
            case SHADER_INPUT_3:
                return with_alpha || !inputs_have_alpha ? "vInput3" : "vInput3.rgb";
            case SHADER_INPUT_4:
                return with_alpha || !inputs_have_alpha ? "vInput4" : "vInput4.rgb";
            case SHADER_TEXEL0:
                return with_alpha ? "texVal0" : "texVal0.rgb";
            case SHADER_TEXEL0A:
                return hint_single_element ? "texVal0.a"
                                           : (with_alpha ? "vec4(texVal0.a, texVal0.a, texVal0.a, texVal0.a)"
                                                         : "vec3(texVal0.a, texVal0.a, texVal0.a)");
            case SHADER_TEXEL1A:
                return hint_single_element ? "texVal1.a"
                                           : (with_alpha ? "vec4(texVal1.a, texVal1.a, texVal1.a, texVal1.a)"
                                                         : "vec3(texVal1.a, texVal1.a, texVal1.a)");
            case SHADER_TEXEL1:
                return with_alpha ? "texVal1" : "texVal1.rgb";
            case SHADER_COMBINED:
                return with_alpha ? "texel" : "texel.rgb";
            case SHADER_NOISE:
                return with_alpha ? "vec4(" RAND_NOISE ", " RAND_NOISE ", " RAND_NOISE ", " RAND_NOISE ")"
                                  : "vec3(" RAND_NOISE ", " RAND_NOISE ", " RAND_NOISE ")";
        }
    } else {
        switch (item) {
            case SHADER_0:
                return "0.0";
            case SHADER_1:
                return "1.0";
            case SHADER_INPUT_1:
                return "vInput1.a";
            case SHADER_INPUT_2:
                return "vInput2.a";
            case SHADER_INPUT_3:
                return "vInput3.a";
            case SHADER_INPUT_4:
                return "vInput4.a";
            case SHADER_TEXEL0:
                return "texVal0.a";
            case SHADER_TEXEL0A:
                return "texVal0.a";
            case SHADER_TEXEL1A:
                return "texVal1.a";
            case SHADER_TEXEL1:
                return "texVal1.a";
            case SHADER_COMBINED:
                return "texel.a";
            case SHADER_NOISE:
                return RAND_NOISE;
        }
    }
    return "";
}

#undef RAND_NOISE

static void append_formula(char *buf, size_t *len, uint8_t c[2][4], bool do_single, bool do_multiply, bool do_mix,
                           bool with_alpha, bool only_alpha, bool opt_alpha) {
    if (do_single) {
        append_str(buf, len, shader_item_to_str(c[only_alpha][3], with_alpha, only_alpha, opt_alpha, false));
    } else if (do_multiply) {
        append_str(buf, len, shader_item_to_str(c[only_alpha][0], with_alpha, only_alpha, opt_alpha, false));
        append_str(buf, len, " * ");
        append_str(buf, len, shader_item_to_str(c[only_alpha][2], with_alpha, only_alpha, opt_alpha, true));
    } else if (do_mix) {
        append_str(buf, len, "mix(");
        append_str(buf, len, shader_item_to_str(c[only_alpha][1], with_alpha, only_alpha, opt_alpha, false));
        append_str(buf, len, ", ");
        append_str(buf, len, shader_item_to_str(c[only_alpha][0], with_alpha, only_alpha, opt_alpha, false));
        append_str(buf, len, ", ");
        append_str(buf, len, shader_item_to_str(c[only_alpha][2], with_alpha, only_alpha, opt_alpha, true));
        append_str(buf, len, ")");
    } else {
        append_str(buf, len, "(");
        append_str(buf, len, shader_item_to_str(c[only_alpha][0], with_alpha, only_alpha, opt_alpha, false));
        append_str(buf, len, " - ");
        append_str(buf, len, shader_item_to_str(c[only_alpha][1], with_alpha, only_alpha, opt_alpha, false));
        append_str(buf, len, ") * ");
        append_str(buf, len, shader_item_to_str(c[only_alpha][2], with_alpha, only_alpha, opt_alpha, true));
        append_str(buf, len, " + ");
        append_str(buf, len, shader_item_to_str(c[only_alpha][3], with_alpha, only_alpha, opt_alpha, false));
    }
}

// Compile one GLSL 450 stage to SPIR-V. Returns empty vector on failure.
static bool compile_stage(const char *src, glslang_stage_t stage, std::vector<unsigned int> &spirv,
                          const char *what, uint64_t shader_id0, uint32_t shader_id1) {
    glslang_input_t input;
    memset(&input, 0, sizeof(input));
    input.language = GLSLANG_SOURCE_GLSL;
    input.stage = stage;
    input.client = GLSLANG_CLIENT_VULKAN;
    input.client_version = GLSLANG_TARGET_VULKAN_1_0;
    input.target_language = GLSLANG_TARGET_SPV;
    input.target_language_version = GLSLANG_TARGET_SPV_1_0;
    input.code = src;
    input.default_version = 450;
    input.default_profile = GLSLANG_NO_PROFILE;
    input.force_default_version_and_profile = false;
    input.forward_compatible = false;
    input.messages = GLSLANG_MSG_DEFAULT_BIT;
    input.resource = glslang_default_resource();

    glslang_shader_t *sh = glslang_shader_create(&input);
    if (!sh) {
        sysLogPrintf(LOG_ERROR, "SDL_GPU: glslang_shader_create failed (%s, ID %llx, %x)", what,
                     (unsigned long long)shader_id0, shader_id1);
        return false;
    }

    if (!glslang_shader_preprocess(sh, &input) || !glslang_shader_parse(sh, &input)) {
        sysLogPrintf(LOG_ERROR, "SDL_GPU: failed to parse this %s shader (ID %llx, %x):\n%s", what,
                     (unsigned long long)shader_id0, shader_id1, src);
        sysLogPrintf(LOG_ERROR, "SDL_GPU: glslang: %s\n%s", glslang_shader_get_info_log(sh),
                     glslang_shader_get_info_debug_log(sh));
        glslang_shader_delete(sh);
        return false;
    }

    glslang_program_t *prog = glslang_program_create();
    glslang_program_add_shader(prog, sh);

    if (!glslang_program_link(prog, GLSLANG_MSG_SPV_RULES_BIT | GLSLANG_MSG_VULKAN_RULES_BIT)) {
        sysLogPrintf(LOG_ERROR, "SDL_GPU: failed to link this %s shader (ID %llx, %x):\n%s", what,
                     (unsigned long long)shader_id0, shader_id1, src);
        sysLogPrintf(LOG_ERROR, "SDL_GPU: glslang: %s\n%s", glslang_program_get_info_log(prog),
                     glslang_program_get_info_debug_log(prog));
        glslang_program_delete(prog);
        glslang_shader_delete(sh);
        return false;
    }

    glslang_program_SPIRV_generate(prog, stage);

    const size_t size = glslang_program_SPIRV_get_size(prog);
    spirv.resize(size);
    if (size > 0) {
        glslang_program_SPIRV_get(prog, spirv.data());
    }

    const char *spv_msg = glslang_program_SPIRV_get_messages(prog);
    if (spv_msg && *spv_msg) {
        sysLogPrintf(LOG_WARNING, "SDL_GPU: glslang SPIR-V messages (%s, ID %llx, %x): %s", what,
                     (unsigned long long)shader_id0, shader_id1, spv_msg);
    }

    glslang_program_delete(prog);
    glslang_shader_delete(sh);
    return size > 0;
}

// Build the GLSL450 vertex-shader source for a combiner permutation into
// vs_buf, returning its length and the per-vertex float count. `cached` emits
// the display-list-cache replay variant: a trailing aShadeIdx attribute, the
// palette texture as a VERTEX-stage sampler (set=0, binding=0 per SDL_GPU's
// SPIR-V convention) and the per-input uShadeRoute routing — the GL backend's
// palette_supported path, minus the ES/GLSL120 fallbacks (always available
// here). Only meaningful when the permutation has combiner inputs.
static size_t build_vs_source(char *vs_buf, const struct CCFeatures &cc, bool cached, size_t *out_num_floats) {
    size_t vs_len = 0;
    size_t num_floats = 4;
    int in_loc = 0;   // vertex attribute locations
    int var_loc = 0;  // varying (VS out / FS in) locations

    const bool palette = cached && cc.num_inputs > 0;

    append_line(vs_buf, &vs_len, "#version 450");
    vs_len += sprintf(vs_buf + vs_len, "layout(location = %d) in vec4 aVtxPos;\n", in_loc++);

    for (int i = 0; i < 2; i++) {
        if (cc.used_textures[i]) {
            vs_len += sprintf(vs_buf + vs_len, "layout(location = %d) in vec2 aTexCoord%d;\n", in_loc++, i);
            vs_len += sprintf(vs_buf + vs_len, "layout(location = %d) out vec2 vTexCoord%d;\n", var_loc++, i);
            num_floats += 2;
            for (int j = 0; j < 2; j++) {
                if (cc.clamp[i][j]) {
                    vs_len += sprintf(vs_buf + vs_len, "layout(location = %d) in float aTexClamp%s%d;\n", in_loc++,
                                      j == 0 ? "S" : "T", i);
                    vs_len += sprintf(vs_buf + vs_len, "layout(location = %d) out float vTexClamp%s%d;\n", var_loc++,
                                      j == 0 ? "S" : "T", i);
                    num_floats += 1;
                }
            }
        }
    }
    if (cc.opt_fog) {
        vs_len += sprintf(vs_buf + vs_len, "layout(location = %d) in vec4 aFog;\n", in_loc++);
        vs_len += sprintf(vs_buf + vs_len, "layout(location = %d) out vec4 vFog;\n", var_loc++);
        num_floats += 4;
    }

    if (cc.opt_grayscale) {
        vs_len += sprintf(vs_buf + vs_len, "layout(location = %d) in vec4 aGrayscaleColor;\n", in_loc++);
        vs_len += sprintf(vs_buf + vs_len, "layout(location = %d) out vec4 vGrayscaleColor;\n", var_loc++);
        num_floats += 4;
    }

    for (int i = 0; i < cc.num_inputs; i++) {
        vs_len += sprintf(vs_buf + vs_len, "layout(location = %d) in vec%d aInput%d;\n", in_loc++,
                          cc.opt_alpha ? 4 : 3, i + 1);
        vs_len += sprintf(vs_buf + vs_len, "layout(location = %d) out vec%d vInput%d;\n", var_loc++,
                          cc.opt_alpha ? 4 : 3, i + 1);
        num_floats += cc.opt_alpha ? 4 : 3;
    }

    if (palette) {
        // trailing float of the cached vertex layout (stride num_floats + 1)
        vs_len += sprintf(vs_buf + vs_len, "layout(location = %d) in float aShadeIdx;\n", in_loc++);
        append_line(vs_buf, &vs_len, "layout(set = 0, binding = 0) uniform sampler2D uPalette;");
    }

    // Push-uniform block (SDL_GPU vertex uniform slot 0). Must mirror
    // struct GfxSdlGpuVSUni exactly; declared whole even when fog/palette
    // members go unused by this permutation.
    append_line(vs_buf, &vs_len, "layout(std140, set = 1, binding = 0) uniform VSUni {");
    append_line(vs_buf, &vs_len, "    mat4 uMVP;");
    append_line(vs_buf, &vs_len, "    int uUseVertexFog;");
    append_line(vs_buf, &vs_len, "    float uFogMul;");
    append_line(vs_buf, &vs_len, "    float uFogOff;");
    append_line(vs_buf, &vs_len, "    int uPaletteEnable;");
    append_line(vs_buf, &vs_len, "    float uPaletteW;");
    append_line(vs_buf, &vs_len, "    int uShadeRoute;");
    append_line(vs_buf, &vs_len, "    int uVSPad0;");
    append_line(vs_buf, &vs_len, "    int uVSPad1;");
    append_line(vs_buf, &vs_len, "};");

    append_line(vs_buf, &vs_len, "void main() {");
    for (int i = 0; i < 2; i++) {
        if (cc.used_textures[i]) {
            vs_len += sprintf(vs_buf + vs_len, "    vTexCoord%d = aTexCoord%d;\n", i, i);
            for (int j = 0; j < 2; j++) {
                if (cc.clamp[i][j]) {
                    vs_len += sprintf(vs_buf + vs_len, "    vTexClamp%s%d = aTexClamp%s%d;\n", j == 0 ? "S" : "T", i,
                                      j == 0 ? "S" : "T", i);
                }
            }
        }
    }
    if (cc.opt_grayscale) {
        append_line(vs_buf, &vs_len, "    vGrayscaleColor = aGrayscaleColor;");
    }

    if (palette) {
        // uPaletteEnable==0: pass baked combiner inputs straight through
        // (identical to the immediate shader). Enabled: fetch the live shade
        // colour once and substitute it into shade input slots per
        // uShadeRoute (bits 0-1 rgb type, bit 2 alpha) — same routing as the
        // GL backend's palette path.
        append_line(vs_buf, &vs_len, "    vec4 shadeCol = vec4(0.0);");
        append_line(vs_buf, &vs_len, "    if (uPaletteEnable != 0) {");
        append_line(vs_buf, &vs_len, "        shadeCol = texelFetch(uPalette, ivec2(int(aShadeIdx + 0.5), 0), 0);");
        append_line(vs_buf, &vs_len, "    }");
        for (int i = 0; i < cc.num_inputs; i++) {
            const int sh = i * 3;
            vs_len += sprintf(vs_buf + vs_len, "    if (uPaletteEnable != 0 && ((uShadeRoute >> %d) & 7) != 0) {\n", sh);
            vs_len += sprintf(vs_buf + vs_len, "        int r = (uShadeRoute >> %d) & 3;\n", sh);
            vs_len += sprintf(vs_buf + vs_len,
                "        vec3 rgb = (r == 1) ? shadeCol.rgb : (r == 2) ? vec3(shadeCol.a) : aInput%d.rgb;\n", i + 1);
            if (cc.opt_alpha) {
                vs_len += sprintf(vs_buf + vs_len,
                    "        float al = (((uShadeRoute >> %d) & 4) != 0) ? shadeCol.a : aInput%d.a;\n", sh, i + 1);
                vs_len += sprintf(vs_buf + vs_len, "        vInput%d = vec4(rgb, al);\n", i + 1);
            } else {
                vs_len += sprintf(vs_buf + vs_len, "        vInput%d = rgb;\n", i + 1);
            }
            vs_len += sprintf(vs_buf + vs_len, "    } else {\n");
            vs_len += sprintf(vs_buf + vs_len, "        vInput%d = aInput%d;\n", i + 1, i + 1);
            vs_len += sprintf(vs_buf + vs_len, "    }\n");
        }
    } else {
        // baked combiner inputs pass straight through
        for (int i = 0; i < cc.num_inputs; i++) {
            vs_len += sprintf(vs_buf + vs_len, "    vInput%d = aInput%d;\n", i + 1, i + 1);
        }
    }

    append_line(vs_buf, &vs_len, "    gl_Position = uMVP * aVtxPos;");

    if (cc.opt_fog) {
        // Same as GL: baked per-vertex factor for the immediate path, or
        // recomputed distance fog for cached geometry, incl. the
        // near/behind-eye guards (see gfx_opengl.cpp for the rationale).
        // One backend difference: gl_Position.z here is the 0..1-remapped
        // clip z ((z + w) / 2 — CPU-side on the immediate path, folded into
        // uMVP for cached replay), and the N64 fog formula wants the
        // original z, so undo the remap first.
        append_line(vs_buf, &vs_len, "    vFog.rgb = aFog.rgb;");
        append_line(vs_buf, &vs_len, "    if (uUseVertexFog != 0) {");
        append_line(vs_buf, &vs_len, "        vFog.a = aFog.a;");
        append_line(vs_buf, &vs_len, "    } else {");
        append_line(vs_buf, &vs_len, "        float fw = gl_Position.w;");
        append_line(vs_buf, &vs_len, "        if (abs(fw) < 0.001) fw = 0.001;");
        append_line(vs_buf, &vs_len, "        float winv = 1.0 / fw;");
        append_line(vs_buf, &vs_len, "        if (winv < 0.0) winv = 32767.0;");
        append_line(vs_buf, &vs_len, "        float fzclip = 2.0 * gl_Position.z - gl_Position.w;");
        append_line(vs_buf, &vs_len, "        float fz = fzclip * winv * uFogMul + uFogOff;");
        append_line(vs_buf, &vs_len, "        vFog.a = clamp(fz, 0.0, 255.0) / 255.0;");
        append_line(vs_buf, &vs_len, "    }");
    }

    // No z *= 0.3 depth hack here: the pipeline uses rasterizer depth clamp
    // (the GL hack only exists for drivers without GL_DEPTH_CLAMP), and z is
    // already remapped to [0, w] by gfx_pc (z_is_from_0_to_1).
    append_line(vs_buf, &vs_len, "}");

    *out_num_floats = num_floats;
    return vs_len;
}

bool gfx_sdlgpu_shader_compile(SDL_GPUDevice *device, uint64_t shader_id0, uint32_t shader_id1,
                               enum FilteringMode filter_mode, struct ShaderProgram *prg) {
    struct CCFeatures cc_features;
    memset(&cc_features, 0, sizeof(cc_features));
    gfx_cc_get_features(shader_id0, shader_id1, &cc_features);

    char vs_buf[8192];
    char fs_buf[8192];
    size_t fs_len = 0;
    size_t num_floats = 4;
    int var_loc = 0;  // varying (VS out / FS in) locations

    // Vertex shader (immediate-path variant)

    size_t vs_len = build_vs_source(vs_buf, cc_features, false, &num_floats);

    // Fragment shader

    append_line(fs_buf, &fs_len, "#version 450");

    append_line(fs_buf, &fs_len, "#define SAMPLE_TEX(tex, uv) texture(tex, uv)");
    append_line(fs_buf, &fs_len, "#define OUTPUT_COLOR outColor");

    // Reference approach to color wrapping as per GLideN64 (same as GL backend)
    append_line(fs_buf, &fs_len, "#define WRAP(x, low, high) mod((x)-(low), (high)-(low)) + (low)");
    append_line(fs_buf, &fs_len, "#define TEX_OFFSET(tex, uv, texSize, off) SAMPLE_TEX(tex, uv - (off)/texSize)");

    var_loc = 0;
    for (int i = 0; i < 2; i++) {
        if (cc_features.used_textures[i]) {
            fs_len += sprintf(fs_buf + fs_len, "layout(location = %d) in vec2 vTexCoord%d;\n", var_loc++, i);
            for (int j = 0; j < 2; j++) {
                if (cc_features.clamp[i][j]) {
                    fs_len += sprintf(fs_buf + fs_len, "layout(location = %d) in float vTexClamp%s%d;\n", var_loc++,
                                      j == 0 ? "S" : "T", i);
                }
            }
        }
    }
    if (cc_features.opt_fog) {
        fs_len += sprintf(fs_buf + fs_len, "layout(location = %d) in vec4 vFog;\n", var_loc++);
    }
    if (cc_features.opt_grayscale) {
        fs_len += sprintf(fs_buf + fs_len, "layout(location = %d) in vec4 vGrayscaleColor;\n", var_loc++);
    }
    for (int i = 0; i < cc_features.num_inputs; i++) {
        fs_len += sprintf(fs_buf + fs_len, "layout(location = %d) in vec%d vInput%d;\n", var_loc++,
                          cc_features.opt_alpha ? 4 : 3, i + 1);
    }

    // Compact fragment sampler bindings: SDL_GPU requires bindings in
    // [0, num_samplers), so uTex1 takes binding 0 when uTex0 is unused.
    int fs_samplers = 0;
    int tex_binding[2] = { -1, -1 };
    for (int i = 0; i < 2; i++) {
        if (cc_features.used_textures[i]) {
            tex_binding[i] = fs_samplers++;
            fs_len += sprintf(fs_buf + fs_len, "layout(set = 2, binding = %d) uniform sampler2D uTex%d;\n",
                              tex_binding[i], i);
        }
    }

    // Push-uniform block (SDL_GPU fragment uniform slot 0); mirrors
    // struct GfxSdlGpuFSUni. Member names match the GL backend's loose
    // uniforms so the body below stays textually identical to GL's.
    append_line(fs_buf, &fs_len, "layout(std140, set = 3, binding = 0) uniform FSUni {");
    append_line(fs_buf, &fs_len, "    int frame_count;");
    append_line(fs_buf, &fs_len, "    float noise_scale;");
    append_line(fs_buf, &fs_len, "    int three_point_filter0;");
    append_line(fs_buf, &fs_len, "    int three_point_filter1;");
    append_line(fs_buf, &fs_len, "    vec4 wireframe_color;");
    append_line(fs_buf, &fs_len, "    float uEmissive;");
    append_line(fs_buf, &fs_len, "    float uFsPad0;");
    append_line(fs_buf, &fs_len, "    float uFsPad1;");
    append_line(fs_buf, &fs_len, "    float uFsPad2;");
    append_line(fs_buf, &fs_len, "};");

    append_line(fs_buf, &fs_len, "layout(location = 0) out vec4 outColor;");

    append_line(fs_buf, &fs_len, "float random(in vec3 value) {");
    append_line(fs_buf, &fs_len, "    float random = dot(sin(value), vec3(12.9898, 78.233, 37.719));");
    append_line(fs_buf, &fs_len, "    return fract(sin(random) * 143758.5453);");
    append_line(fs_buf, &fs_len, "}");

    if (filter_mode == FILTER_THREE_POINT) {
        append_line(fs_buf, &fs_len, "vec4 filter3point(in sampler2D tex, in vec2 texCoord, in vec2 texSize) {");
        append_line(fs_buf, &fs_len, "    vec2 offset = fract(texCoord*texSize - vec2(0.5));");
        append_line(fs_buf, &fs_len, "    offset -= step(1.0, offset.x + offset.y);");
        append_line(fs_buf, &fs_len, "    vec4 c0 = TEX_OFFSET(tex, texCoord, texSize, offset);");
        append_line(fs_buf, &fs_len, "    vec4 c1 = TEX_OFFSET(tex, texCoord, texSize, vec2(offset.x - sign(offset.x), offset.y));");
        append_line(fs_buf, &fs_len, "    vec4 c2 = TEX_OFFSET(tex, texCoord, texSize, vec2(offset.x, offset.y - sign(offset.y)));");
        append_line(fs_buf, &fs_len, "    return c0 + abs(offset.x)*(c1-c0) + abs(offset.y)*(c2-c0);");
        append_line(fs_buf, &fs_len, "}");
    }

    if (cc_features.opt_blur) {
        // blur filter, used for menu backgrounds (same construction as GL)
        if (filter_mode == FILTER_THREE_POINT)
            append_line(fs_buf, &fs_len, "vec4 hookTexture2D(in sampler2D t, in vec2 uv, in vec2 texSize, in int three_point_filter) {");
        else
            append_line(fs_buf, &fs_len, "vec4 hookTexture2D(in sampler2D t, in vec2 uv, in vec2 texSize) {");

        append_line(fs_buf, &fs_len, "    vec4 cw = vec4(0.0);");
        append_line(fs_buf, &fs_len, "    for (int i = 0; i < 16; ++i) {");
        append_line(fs_buf, &fs_len, "        vec2 xy = vec2(float(i & 3), float(i >> 2));");
        append_line(fs_buf, &fs_len, "        float w = 0.009947 - length(xy) * 0.001;");
        append_line(fs_buf, &fs_len, "        vec2 scaled_uv = uv + (vec2(-1.5) + xy) / texSize;");

        if (filter_mode == FILTER_THREE_POINT)
            append_line(fs_buf, &fs_len, "        vec4 tex = mix(SAMPLE_TEX(t, scaled_uv), filter3point(t, scaled_uv, texSize), float(three_point_filter));");
        else
            append_line(fs_buf, &fs_len, "        vec4 tex = SAMPLE_TEX(t, scaled_uv);");

        append_line(fs_buf, &fs_len, "        cw += vec4(tex.rgb * w, w);");
        append_line(fs_buf, &fs_len, "    }");
        append_line(fs_buf, &fs_len, "    return vec4(cw.rgb / cw.a, 1.0);");
        append_line(fs_buf, &fs_len, "}");
    } else {
        if (filter_mode == FILTER_THREE_POINT) {
            append_line(fs_buf, &fs_len, "vec4 hookTexture2D(in sampler2D tex, in vec2 uv, in vec2 texSize, in int three_point_filter) {");
            append_line(fs_buf, &fs_len, "    return mix(SAMPLE_TEX(tex, uv), filter3point(tex, uv, texSize), float(three_point_filter));");
            append_line(fs_buf, &fs_len, "}");
        } else {
            append_line(fs_buf, &fs_len, "vec4 hookTexture2D(in sampler2D tex, in vec2 uv, in vec2 texSize) {");
            append_line(fs_buf, &fs_len, "    return SAMPLE_TEX(tex, uv);");
            append_line(fs_buf, &fs_len, "}");
        }
    }

    append_line(fs_buf, &fs_len, "void main() {");

    for (int i = 0; i < 2; i++) {
        if (cc_features.used_textures[i]) {
            bool s = cc_features.clamp[i][0], t = cc_features.clamp[i][1];

            fs_len += sprintf(fs_buf + fs_len, "    vec2 texSize%d = vec2(textureSize(uTex%d, 0));\n", i, i);

            if (!s && !t) {
                fs_len += sprintf(fs_buf + fs_len, "    vec2 vTexCoordAdj%d = vTexCoord%d;\n", i, i);
            } else {
                if (s && t) {
                    fs_len += sprintf(fs_buf + fs_len,
                                      "    vec2 vTexCoordAdj%d = clamp(vTexCoord%d, 0.5 / texSize%d, "
                                      "vec2(vTexClampS%d, vTexClampT%d));\n",
                                      i, i, i, i, i);
                } else if (s) {
                    fs_len += sprintf(fs_buf + fs_len,
                                      "    vec2 vTexCoordAdj%d = vec2(clamp(vTexCoord%d.s, 0.5 / "
                                      "texSize%d.s, vTexClampS%d), vTexCoord%d.t);\n",
                                      i, i, i, i, i);
                } else {
                    fs_len += sprintf(fs_buf + fs_len,
                                      "    vec2 vTexCoordAdj%d = vec2(vTexCoord%d.s, clamp(vTexCoord%d.t, "
                                      "0.5 / texSize%d.t, vTexClampT%d));\n",
                                      i, i, i, i, i);
                }
            }

            if (filter_mode == FILTER_THREE_POINT)
                fs_len += sprintf(fs_buf + fs_len, "    vec4 texVal%d = hookTexture2D(uTex%d, vTexCoordAdj%d, texSize%d, three_point_filter%d);\n", i, i, i, i, i);
            else
                fs_len += sprintf(fs_buf + fs_len, "    vec4 texVal%d = hookTexture2D(uTex%d, vTexCoordAdj%d, texSize%d);\n", i, i, i, i);
        }
    }

    append_line(fs_buf, &fs_len, cc_features.opt_alpha ? "    vec4 texel;" : "    vec3 texel;");
    for (int c = 0; c < (cc_features.opt_2cyc ? 2 : 1); c++) {
        append_str(fs_buf, &fs_len, "    texel = ");
        if (!cc_features.color_alpha_same[c] && cc_features.opt_alpha) {
            append_str(fs_buf, &fs_len, "vec4(");
            append_formula(fs_buf, &fs_len, cc_features.c[c], cc_features.do_single[c][0],
                           cc_features.do_multiply[c][0], cc_features.do_mix[c][0], false, false, true);
            append_str(fs_buf, &fs_len, ", ");
            append_formula(fs_buf, &fs_len, cc_features.c[c], cc_features.do_single[c][1],
                           cc_features.do_multiply[c][1], cc_features.do_mix[c][1], true, true, true);
            append_str(fs_buf, &fs_len, ")");
        } else {
            append_formula(fs_buf, &fs_len, cc_features.c[c], cc_features.do_single[c][0],
                           cc_features.do_multiply[c][0], cc_features.do_mix[c][0], cc_features.opt_alpha, false,
                           cc_features.opt_alpha);
        }
        append_line(fs_buf, &fs_len, ";");

        if (c == 0) {
            append_line(fs_buf, &fs_len, "    texel = WRAP(texel, -1.01, 1.01);");
        }
    }

    append_line(fs_buf, &fs_len, "    texel = WRAP(texel, -0.51, 1.51);");
    append_line(fs_buf, &fs_len, "    texel = clamp(texel, 0.0, 1.0);");
    if (cc_features.opt_fog) {
        if (cc_features.opt_alpha) {
            append_line(fs_buf, &fs_len, "    texel = vec4(mix(texel.rgb, vFog.rgb, vFog.a), texel.a);");
        } else {
            append_line(fs_buf, &fs_len, "    texel = mix(texel, vFog.rgb, vFog.a);");
        }
    }

    if (cc_features.opt_texture_edge && cc_features.opt_alpha) {
        append_line(fs_buf, &fs_len, "    if (texel.a > 0.19) texel.a = 1.0; else discard;");
    }

    if (cc_features.opt_alpha && cc_features.opt_noise) {
        append_line(fs_buf, &fs_len,
                    "    texel.a *= floor(clamp(random(vec3(floor(gl_FragCoord.xy * noise_scale), float(frame_count))) + "
                    "texel.a, 0.0, 1.0));");
    }

    if (cc_features.opt_grayscale) {
        append_line(fs_buf, &fs_len, "    float intensity = (texel.r + texel.g + texel.b) / 3.0;");
        append_line(fs_buf, &fs_len, "    vec3 new_texel = vGrayscaleColor.rgb * intensity;");
        append_line(fs_buf, &fs_len, "    texel.rgb = mix(texel.rgb, new_texel, vGrayscaleColor.a);");
    }

    // Wireframe cheat: replace the surface colour with a flat wire colour when enabled.
    append_line(fs_buf, &fs_len, "    if (wireframe_color.a > 0.5) texel.rgb = wireframe_color.rgb;");

    if (cc_features.opt_alpha) {
        if (cc_features.opt_alpha_threshold) {
            append_line(fs_buf, &fs_len, "    if (texel.a < 8.0 / 256.0) discard;");
        }
        if (cc_features.opt_invisible) {
            append_line(fs_buf, &fs_len, "    texel.a = 0.0;");
        }

        // HDR dazzle: emissive-boost the colour into the FP16 range (> 1.0);
        // the present pass maps it beyond paper white toward the peak. 1.0
        // (the default, and always in SDR) is a no-op.
        append_line(fs_buf, &fs_len, "    texel.rgb *= uEmissive;");
        append_line(fs_buf, &fs_len, "    OUTPUT_COLOR = texel;");
    } else {
        append_line(fs_buf, &fs_len, "    texel *= uEmissive;");
        append_line(fs_buf, &fs_len, "    OUTPUT_COLOR = vec4(texel, 1.0);");
    }

    append_line(fs_buf, &fs_len, "}");

    vs_buf[vs_len] = '\0';
    fs_buf[fs_len] = '\0';

    // GLSL -> SPIR-V -> device format (disk-cached: the glslang compile and
    // any SPIRV-Cross/D3DCompile translation are the cold-start hitch; the
    // text generation above is microseconds and also computes the program
    // metadata, so it always runs)

    std::vector<uint8_t> vs_blob, fs_blob;
    const ShaderCacheKey ck = { shader_id0, shader_id1, (uint8_t)filter_mode, 0, s_shader_format_tag };
    const ShaderCacheBlobs *hit = shader_cache_find(ck);
    if (hit && !hit->vs.empty() && !hit->fs.empty()) {
        vs_blob = hit->vs;
        fs_blob = hit->fs;
    } else {
        std::vector<unsigned int> vs_spirv, fs_spirv;
        if (!compile_stage(vs_buf, GLSLANG_STAGE_VERTEX, vs_spirv, "vertex", shader_id0, shader_id1) ||
            !compile_stage(fs_buf, GLSLANG_STAGE_FRAGMENT, fs_spirv, "fragment", shader_id0, shader_id1)) {
            return false;
        }
        if (!spirv_to_blob(GLSLANG_STAGE_VERTEX, vs_spirv, vs_blob, "vertex", shader_id0, shader_id1) ||
            !spirv_to_blob(GLSLANG_STAGE_FRAGMENT, fs_spirv, fs_blob, "fragment", shader_id0, shader_id1)) {
            return false;
        }
        shader_cache_append(ck, vs_blob, fs_blob);
    }

    // blob -> SDL_GPUShader (Phase 2's cached VS variant adds the palette sampler)
    SDL_GPUShader *vs = blob_to_shader(device, SDL_GPU_SHADERSTAGE_VERTEX, vs_blob, 0, 1);
    SDL_GPUShader *fs = blob_to_shader(device, SDL_GPU_SHADERSTAGE_FRAGMENT, fs_blob, (Uint32)fs_samplers, 1);
    if (!vs || !fs) {
        sysLogPrintf(LOG_ERROR, "SDL_GPU: SDL_CreateGPUShader failed (ID %llx, %x): %s",
                     (unsigned long long)shader_id0, shader_id1, SDL_GetError());
        if (vs) {
            SDL_ReleaseGPUShader(device, vs);
        }
        if (fs) {
            SDL_ReleaseGPUShader(device, fs);
        }
        return false;
    }

    // Fill the program descriptor: attribute layout mirrors the GL backend's
    // (position, per-texture coords + clamps, fog, grayscale, inputs).
    memset(prg, 0, sizeof(*prg));
    prg->shader_id0 = shader_id0;
    prg->shader_id1 = shader_id1;
    prg->cc = cc_features;
    prg->vs = vs;
    prg->fs = fs;
    prg->num_inputs = cc_features.num_inputs;
    prg->used_textures[0] = cc_features.used_textures[0];
    prg->used_textures[1] = cc_features.used_textures[1];
    prg->num_floats = (uint8_t)num_floats;
    prg->fs_sampler_count = (uint8_t)fs_samplers;
    prg->tex_binding[0] = (int8_t)tex_binding[0];
    prg->tex_binding[1] = (int8_t)tex_binding[1];

    size_t cnt = 0;
    prg->attrib_sizes[cnt++] = 4; // aVtxPos
    for (int i = 0; i < 2; i++) {
        if (cc_features.used_textures[i]) {
            prg->attrib_sizes[cnt++] = 2;
            for (int j = 0; j < 2; j++) {
                if (cc_features.clamp[i][j]) {
                    prg->attrib_sizes[cnt++] = 1;
                }
            }
        }
    }
    if (cc_features.opt_fog) {
        prg->attrib_sizes[cnt++] = 4;
    }
    if (cc_features.opt_grayscale) {
        prg->attrib_sizes[cnt++] = 4;
    }
    for (int i = 0; i < cc_features.num_inputs; i++) {
        prg->attrib_sizes[cnt++] = cc_features.opt_alpha ? 4 : 3;
    }
    prg->num_attribs = (uint8_t)cnt;

    return true;
}

bool gfx_sdlgpu_shader_compile_cached_vs(SDL_GPUDevice *device, struct ShaderProgram *prg) {
    if (prg->vs_cached) {
        return true;
    }
    if (prg->num_inputs == 0) {
        // no combiner inputs -> nothing to route from the palette; the
        // immediate VS works as-is (the cached pipeline only changes the
        // vertex stride, and the trailing aShadeIdx float goes unread)
        prg->vs_cached = prg->vs;
        return true;
    }

    char vs_buf[8192];
    size_t num_floats = 0;
    const size_t vs_len = build_vs_source(vs_buf, prg->cc, true, &num_floats);
    vs_buf[vs_len] = '\0';

    // the VS depends only on the combiner (not the filter mode) -> filter 0xff
    std::vector<uint8_t> blob;
    const ShaderCacheKey ck = { prg->shader_id0, prg->shader_id1, 0xff, 1, s_shader_format_tag };
    const ShaderCacheBlobs *hit = shader_cache_find(ck);
    if (hit && !hit->vs.empty()) {
        blob = hit->vs;
    } else {
        std::vector<unsigned int> spirv;
        if (!compile_stage(vs_buf, GLSLANG_STAGE_VERTEX, spirv, "cached vertex", prg->shader_id0, prg->shader_id1)) {
            return false;
        }
        if (!spirv_to_blob(GLSLANG_STAGE_VERTEX, spirv, blob, "cached vertex", prg->shader_id0, prg->shader_id1)) {
            return false;
        }
        shader_cache_append(ck, blob, std::vector<uint8_t>());
    }

    prg->vs_cached = blob_to_shader(device, SDL_GPU_SHADERSTAGE_VERTEX, blob, 1 /* uPalette, vertex set=0 */, 1);
    if (!prg->vs_cached) {
        sysLogPrintf(LOG_ERROR, "SDL_GPU: cached VS creation failed (ID %llx, %x): %s",
                     (unsigned long long)prg->shader_id0, prg->shader_id1, SDL_GetError());
        return false;
    }
    return true;
}

#endif // USE_SDLGPU
