#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <PR/ultratypes.h>
#include <PR/gbi.h>
#include "platform.h"
#include "config.h"
#include "system.h"
#include "video.h"

// Forward-decl only: pulling net/net.h would drag in types.h which redefines
// `bool` and collides with <stdbool.h> already included above.
extern s32 g_NetDedicatedMode;

#include "../fast3d/gfx_api.h"
#include "../fast3d/gfx_sdl.h"
#include "../fast3d/gfx_opengl.h"
#include "../fast3d/gfx_sdlgpu.h"
#include "rt_ext.h"

#ifdef PLATFORM_NSWITCH
#define DEFAULT_VID_WIDTH 1280
#define DEFAULT_VID_HEIGHT 720
#define DEFAULT_VID_FULLSCREEN true
#define DEFAULT_VID_FULLSCREEN_EXCLUSIVE true
#else
#define DEFAULT_VID_WIDTH 640
#define DEFAULT_VID_HEIGHT 480
#define DEFAULT_VID_FULLSCREEN false
#define DEFAULT_VID_FULLSCREEN_EXCLUSIVE false
#endif

static struct GfxWindowManagerAPI *wmAPI;
static struct GfxRenderingAPI *renderingAPI;

static bool initDone = false;

// rendering backend: "opengl" (default) or "sdlgpu" (SDL_GPU/Vulkan, only
// when built with USE_SDLGPU); --renderer overrides the config value
static char vidRenderer[16] = "opengl";
// SDL_GPU driver: "" = platform default (vulkan on Windows), or "vulkan" /
// "direct3d12" / "metal"; --gpu-driver overrides
static char vidGpuDriver[16] = "";
// HDR output (SDL_GPU only): scRGB/HDR10 swapchain + FP16 targets +
// paper-white scaling; applied at startup when the display supports it
static s32 vidHDR = 0;
static f32 vidHDRPaperWhite = 200.0f; // nits; scRGB 1.0 = 80
static f32 vidHDRPeak = 600.0f;       // highlight-expansion target, nits

static s32 vidWidth = DEFAULT_VID_WIDTH;
static s32 vidHeight = DEFAULT_VID_HEIGHT;
static s32 vidFramebuffers = true;
static s32 vidFullscreen = DEFAULT_VID_FULLSCREEN;
static s32 vidFullscreenExclusive = DEFAULT_VID_FULLSCREEN_EXCLUSIVE;
static f32 vidRefreshRate = 0.f; // exclusive-fullscreen refresh rate; 0 = auto
static s32 vidMaximize = false;
static s32 vidCenter = false;
// default ON since SDL3: renders at native pixel density on macOS/Wayland
// (SDL_WINDOW_HIGH_PIXEL_DENSITY); no effect on Windows, where windows are
// always pixel-sized and SDL3 is DPI-aware out of the box
static s32 vidAllowHiDpi = true;
static s32 vidVsync = 1;
static s32 vidMSAA = 1;
static s32 vidFramerateLimit = 0;
// Netplay-only framerate ceiling. g_NetTick advances per render frame on the
// client, so interpolation intervals, snapshot spacing and the lag-comp RTT->tick
// math are all measured in render frames — letting fps run unbounded in netplay
// would distort that timing. This caps render fps during netplay only; 0 = no
// netplay cap (use at your own risk). Single-player honours vidFramerateLimit
// directly (0 there = truly unlimited).
static s32 vidNetplayFramerateLimit = 120;

static s32 vidDisplayFPS = 0;
static f32 vidDisplayFPSInterval = 1.f;
static f32 vidAvgFPS = 0;

static s32 vidNumModes = 1;
static displaymode vidModeDefault;
static displaymode *vidModes = &vidModeDefault;

static f32 vidGlareBrightness = 1.f;
static f32 vidOverexposureScale = 1.f;

static s32 texFilter = FILTER_LINEAR;
static s32 texFilter2D = true;
static s32 texDetail = false;
static s32 texMipmapFilter = MIPMAP_LINEAR;
static u32 texAnisotropicFilter = 4;
static s32 texExternal = false;
// Texture-cache COUNT cap (see gfx_pc.cpp). Default 4096; raise for HD packs that
// go black with /dlcache (the recorder imports off-screen textures, overflowing the
// cap -> on-screen textures get LRU-evicted). Live override: /texcache N.
static s32 texCacheSize = 4096;
// Cached display-list backface-cull mode (see /dlcache cull). "auto" = per-segment
// recorded G_CULL_* + winding (perf default). Some GPU drivers cull cached geometry
// the immediate (CPU-culled) path doesn't, dropping whole walls (black on GL /
// see-through on Vulkan); "off" draws both faces (opaque is z-buffer-identical) and
// is the persistent form of /dlcache cull off. Values: auto|off|back|front.
static char vidDlCacheCull[16] = "auto";
// Display-list cache master enable (Video.DlCache; mirrors /dlcache on|off and the
// Extended>Video "Display List Cache" checkbox). Default on. Turn off to skip the
// GPU-resident cache entirely on hardware where it mis-renders (black/stretched/
// mis-coloured cached geometry) -- see docs/PORT_DLCACHE_BLACK_TEXTURES.md. Off is
// byte-identical to a non-cached build.
static s32 vidDlCache = 1;
// Cached display-list front-face winding (see /dlcache ff). The cache GPU-culls
// with this winding; the immediate path CPU-culls and is unaffected. The CORRECT
// winding is PER-RENDERER: Vulkan/SDL_GPU flips Y in NDC, which reverses triangle
// winding vs OpenGL, so a machine can want ccw on GL and cw on Vulkan at once. A
// single global breaks the other renderer (forces every back face), so it's split
// per backend. off/ccw = default; cw = the persistent /dlcache ff. The active
// renderer's value is applied at videoInit + by the Extended>Video toggle.
static char vidDlCacheFrontGL[8] = "ccw";
static char vidDlCacheFrontGpu[8] = "ccw";

static u32 dlcount = 0;
static u32 frames = 0;
static f64 startTime, endTime;
static f64 accumDelta = 0.0;
static f64 fpsTime = 0.0;
static s32 fpsNumFrames = 0;

static s32 videoInitDisplayModes(void);
static s32 videoVRRCap(void);
static s32 videoEffectiveLimit(s32 userlimit);
void optionsMenuInit();

// The cached-cull winding string for the renderer that is currently live. Vulkan
// reverses winding vs OpenGL, so each backend keeps its own value.
static char *videoDlCacheFrontActive(void)
{
#ifdef USE_SDLGPU
	if (renderingAPI == &gfx_sdlgpu_api) {
		return vidDlCacheFrontGpu;
	}
#endif
	return vidDlCacheFrontGL;
}

s32 videoInit(void)
{
	// Dedicated headless server: no SDL window, no GL context. Leave wmAPI =
	// NULL and initDone = false; the per-frame entry points (videoStartFrame /
	// videoEndFrame / videoSubmitCommands) short-circuit on initDone, and the
	// other wmAPI-touching entry points below now guard on wmAPI != NULL.
	if (g_NetDedicatedMode == 1) {
		sysLogPrintf(LOG_NOTE, "video: headless dedicated server, skipping init");
		return 0;
	}

#ifdef DEDICATED_SERVER
	// Server-only build: the SDL window manager and OpenGL renderer are not
	// compiled in, so videoInit is always a no-op (g_NetDedicatedMode is forced
	// to 1 in main(), so we never reach here anyway).
	return 0;
#else
	wmAPI = &gfx_sdl;
	renderingAPI = &gfx_opengl_api;

	// Push the configured texture-cache cap into the renderer (Video.TextureCacheSize
	// from pd.ini). Sets a renderer global read live at texture import; safe pre-context.
	{
		extern void gfx_set_texture_cache_size(int n);
		gfx_set_texture_cache_size(texCacheSize);
	}

	// Persisted display-list-cache master enable (Video.DlCache). Default on; off
	// skips the cache (correct on hardware where it mis-renders).
	{
		extern bool g_DlCacheEnabled;
		extern void gfx_dlcache_clear(void);
		g_DlCacheEnabled = (bool)vidDlCache;
		if (!vidDlCache) {
			gfx_dlcache_clear();
		}
	}

	// Persisted cached-cull mode (Video.DlCacheCull). Lets a driver that mis-culls
	// cached geometry pin "off" permanently instead of re-typing /dlcache cull off.
	{
		extern void gfx_dlcache_set_cullmode(int mode);
		int cm = 0; // auto
		if (strcmp(vidDlCacheCull, "off") == 0) {
			cm = 1;
		} else if (strcmp(vidDlCacheCull, "back") == 0) {
			cm = 2;
		} else if (strcmp(vidDlCacheCull, "front") == 0) {
			cm = 3;
		}
		gfx_dlcache_set_cullmode(cm);
	}

#ifdef USE_SDLGPU
	// Optional SDL_GPU (Vulkan) renderer. Probed before the window exists so
	// a missing/broken Vulkan driver falls back to OpenGL cleanly.
	{
		const char *rend = sysArgGetString("--renderer");
		if (!rend || !*rend) {
			rend = vidRenderer;
		}
		if (strcmp(rend, "sdlgpu") == 0) {
			gfx_sdlgpu_set_driver_default(vidGpuDriver);
			gfx_sdlgpu_request_hdr(vidHDR, vidHDRPaperWhite, vidHDRPeak);
			if (gfx_sdlgpu_probe()) {
				renderingAPI = &gfx_sdlgpu_api;
				gfx_sdl_set_backend(1);
				sysLogPrintf(LOG_NOTE, "video: using SDL_GPU renderer");
			} else {
				sysLogPrintf(LOG_WARNING, "video: SDL_GPU renderer unavailable, falling back to OpenGL");
			}
		} else if (strcmp(rend, "opengl") != 0) {
			sysLogPrintf(LOG_WARNING, "video: unknown renderer '%s', using OpenGL", rend);
		}
	}
#endif

	// Persisted cached front-face winding for the NOW-FINALISED renderer
	// (Video.DlCacheFrontFaceGL / ...GPU). Applied here, after the SDL_GPU probe
	// settles renderingAPI, so the right per-backend value is used.
	{
		extern void gfx_dlcache_set_frontface(int ccw);
		gfx_dlcache_set_frontface(strcmp(videoDlCacheFrontActive(), "cw") == 0 ? 0 : 1);
	}

	gfx_current_native_viewport.width = 320;
	gfx_current_native_viewport.height = 220;
	gfx_current_native_aspect = 320.f / 220.f;
	gfx_framebuffers_enabled = (bool)vidFramebuffers;
	gfx_detail_textures_enabled = (bool)texDetail;
	gfx_external_textures_enabled = (bool)texExternal;
	gfx_msaa_level = vidMSAA;

	struct GfxInitSettings set = {
		.wapi = wmAPI,
		.rapi = renderingAPI,
		.window_settings = {
			.title = "Perfect Dark",
			.width = vidWidth,
			.height = vidHeight,
			.x = 100,
			.y = 100,
			.fullscreen = vidFullscreen,
			.fullscreen_is_exclusive = vidFullscreenExclusive,
			.maximized = vidMaximize,
			.centered = vidCenter,
			.allow_hidpi = vidAllowHiDpi
		}
	};

	gfx_init(&set);

	// apply the configured exclusive-fullscreen refresh rate (0 = auto);
	// re-picks the mode immediately if we booted into exclusive fullscreen
	if (wmAPI->set_refresh_rate) {
		wmAPI->set_refresh_rate(vidRefreshRate);
	}

	videoInitDisplayModes();
	videoSetVsync(vidVsync);
	videoSetFramerateLimit(vidFramerateLimit);

	gfx_set_texture_filter((enum FilteringMode)texFilter);
	gfx_set_mipmap_filter((enum MipmapFilteringMode)texMipmapFilter);
	videoSetAnisotropicFilter(texAnisotropicFilter);
	optionsMenuInit();

	initDone = true;
	return 0;
#endif
}

void videoStartFrame(void)
{
	if (initDone) {
		startTime = wmAPI->get_time();
		gfx_start_frame();
		// Synchronize with their backend counterparts. Moved inside the
		// initDone gate so headless dedicated (wmAPI == NULL) doesn't deref.
		vidFullscreen = videoGetFullscreen();
		vidMaximize = videoGetMaximizeWindow();

		// VRR cap tracks the display refresh, which changes on mode switches /
		// monitor moves; re-derive it every couple of seconds while VRR is on
		if (vidVsync == -2) {
			static u32 vrrRecheck = 0;
			if (++vrrRecheck >= 120) {
				vrrRecheck = 0;
				wmAPI->set_target_fps(videoEffectiveLimit(vidFramerateLimit));
			}
		}
	}
}

void videoSubmitCommands(Gfx *cmds)
{
	if (initDone) {
		gfx_run(cmds);
		++dlcount;
	}
}

void videoEndFrame(void)
{
	if (!initDone) {
		return;
	}

	gfx_end_frame();

	++frames;
	++fpsNumFrames;

	const f64 flipTime = wmAPI->get_time();
	accumDelta += flipTime - endTime;
	endTime = flipTime;

	if (endTime >= fpsTime) {
		char tmp[128];
		vidAvgFPS = fpsNumFrames ? ((f64)fpsNumFrames / accumDelta) : 0.f;
		fpsNumFrames = 0;
		accumDelta = 0.0;
		fpsTime = endTime + vidDisplayFPSInterval;
	}
}



f32 videoGetAverageFPS(void)
{
	return vidAvgFPS;
}

void videoClearScreen(void)
{
	videoStartFrame();
	// TODO: clear
	videoEndFrame();
}

void *videoGetWindowHandle(void)
{
	if (initDone) {
		return wmAPI->get_window_handle();
	}
	return NULL;
}

void videoSetTaskbarProgress(s32 state, f32 value)
{
	if (initDone && wmAPI && wmAPI->set_taskbar_progress) {
		wmAPI->set_taskbar_progress(state, value);
	}
}

s32 videoGetAllowHiDpi(void)
{
	return vidAllowHiDpi;
}

void videoSetAllowHiDpi(s32 allow)
{
	// takes effect at the next window creation (restart)
	vidAllowHiDpi = !!allow;
}

s32 videoGetRefreshRates(f32 *out, s32 max)
{
	if (initDone && wmAPI && wmAPI->get_refresh_rates) {
		// rates for the currently selected resolution
		return wmAPI->get_refresh_rates(vidWidth, vidHeight, out, max);
	}
	return 0;
}

f32 videoGetRefreshRate(void)
{
	return vidRefreshRate;
}

void videoSetRefreshRate(f32 hz)
{
	vidRefreshRate = hz;
	if (initDone && wmAPI && wmAPI->set_refresh_rate) {
		wmAPI->set_refresh_rate(hz);
	}
}

void videoUpdateNativeResolution(s32 w, s32 h)
{
	if (!wmAPI) return;
	gfx_current_native_viewport.width = w;
	gfx_current_native_viewport.height = h;
	gfx_current_native_aspect = (float)w / (float)h;
}

s32 videoGetNativeWidth(void)
{
	return gfx_current_native_viewport.width;
}

s32 videoGetNativeHeight(void)
{
	return gfx_current_native_viewport.height;
}

s32 videoGetWidth(void)
{
	return gfx_current_dimensions.width;
}

s32 videoGetHeight(void)
{
	return gfx_current_dimensions.height;
}

s32 videoGetFullscreen(void)
{
	if (!wmAPI) return vidFullscreen;
	vidFullscreen = wmAPI->get_fullscreen_state();
	return vidFullscreen;
}

s32 videoGetFullscreenMode(void)
{
	if (!wmAPI) return vidFullscreenExclusive;
	vidFullscreenExclusive = wmAPI->get_fullscreen_flag_mode();
	return vidFullscreenExclusive;
}

s32 videoGetMaximizeWindow(void)
{
	if (!wmAPI) return vidMaximize;
	vidMaximize = wmAPI->get_maximized_state();
	return vidMaximize;
}

s32 videoGetCenterWindow(void)
{
	return vidCenter;
}

f32 videoGetAspect(void)
{
	return gfx_current_dimensions.aspect_ratio;
}

s32 videoGetDisplayModeIndex(void)
{
	for (s32 i = 1; i < vidNumModes; ++i) {
		if (vidModes[i].width == gfx_current_dimensions.width &&
		    vidModes[i].height == gfx_current_dimensions.height) {
			return i;
		}
	}
	// Current dimensions don't match any known mode, so return index 0, "Custom".
	return 0;
}

s32 videoGetMSAA(void)
{
	vidMSAA = (s32)gfx_msaa_level;
	return vidMSAA;
}

// VRR (G-Sync/FreeSync) support, Video.VSync = -2: tearing-allowed
// presentation (backend swap interval 0) plus an automatic framerate cap
// just below the display refresh, so frame delivery stays inside the VRR
// window and never bounces off the vsync ceiling. The cap follows the
// Blur Busters rule (refresh - refresh^2/3600: 60->59, 120->116, 144->138).
// Returns 0 when VRR is off or the refresh is unknown.
static s32 videoVRRCap(void)
{
	u32 hz = 0;

	if (vidVsync != -2 || !wmAPI) {
		return 0;
	}

	wmAPI->get_active_window_refresh_rate(&hz);

	if (hz < 30) {
		return 0;
	}

	const s32 cap = (s32)hz - (s32)((hz * hz) / 3600);
	return cap > 10 ? cap : 10;
}

// the frame cap actually applied to the window manager: the user's limit,
// additionally bounded by the VRR cap when VRR mode is on (0 = unlimited)
static s32 videoEffectiveLimit(s32 userlimit)
{
	const s32 vrr = videoVRRCap();

	if (vrr > 0 && (userlimit == 0 || userlimit > vrr)) {
		return vrr;
	}

	return userlimit;
}

s32 videoGetVsync(void)
{
	if (!wmAPI) return vidVsync;
	// in VRR mode the backend runs swap interval 0; reading that back would
	// turn the stored -2 into a plain "off"
	if (vidVsync != -2) {
		vidVsync = wmAPI->get_swap_interval();
	}
	return vidVsync;
}

s32 videoGetFramerateLimit(void)
{
	if (!wmAPI) return vidFramerateLimit;
	// in VRR mode the window manager holds the effective (VRR-capped) value,
	// not the user's configured limit — don't clobber the setting with it
	if (vidVsync != -2) {
		vidFramerateLimit = wmAPI->get_target_fps();
	}
	return vidFramerateLimit;
}

s32 videoGetNetplayFramerateLimit(void)
{
	return vidNetplayFramerateLimit;
}

void videoSetNetplayFramerateLimit(const s32 limit)
{
	vidNetplayFramerateLimit = limit;
}

s32 videoGetDisplayFPS(void)
{
	return vidDisplayFPS;
}

static s32 videoInitDisplayModes(void)
{
	if (!wmAPI->get_current_display_mode(&vidModeDefault.width, &vidModeDefault.height)) {
		vidModeDefault.width = 640;
		vidModeDefault.height = 480;
		return false;
	}

	const s32 numBaseModes = wmAPI->get_num_display_modes();
	if (!numBaseModes) {
		return false;
	}

	const s32 numCustomModes = 1;
	displaymode *modeList = sysMemZeroAlloc((numBaseModes + numCustomModes) * sizeof(displaymode));
	if (!modeList) {
		return false;
	}

	modeList[0].width = 0;
	modeList[0].height = 0;

	s32 numModes = 1;
	s32 w = -1, h = w, neww = w, newh = w;

	// SDL modes are guaranteed to be sorted high to low
	for (s32 i = 0; i < numBaseModes; ++i) {
		wmAPI->get_display_mode(i, &neww, &newh);

		if (neww != w || newh != h) {
			w = neww;
			h = newh;
			modeList[numModes].width = w;
			modeList[numModes].height = h;
			++numModes;
		}
	}

	modeList = sysMemRealloc(modeList, numModes * sizeof(displaymode));
	if (!modeList) {
		return false;
	}

	vidModes = modeList;
	vidNumModes = numModes;

	return true;
}

s32 videoGetDisplayMode(displaymode *out, const s32 index)
{
	if (index >= 0 && index < vidNumModes) {
		*out = vidModes[index];
		return true;
	}
	return false;
}

s32 videoGetNumDisplayModes(void)
{
	return vidNumModes;
}

void videoSetDisplayMode(const s32 index)
{
	const displaymode dm = vidModes[index];

	if (index == 0) {
		// "Custom" video mode.
		return;
	}

	vidWidth = dm.width;
	vidHeight = dm.height;

	s32 posX = 100;
	s32 posY = 100;
	if (vidCenter) {
		wmAPI->get_centered_positions(vidWidth, vidHeight, &posX, &posY);
	}

	if (vidFullscreen) {
		wmAPI->set_closest_resolution(vidWidth, vidHeight, vidCenter);
	} else {
		if (vidMaximize) {
			videoSetMaximizeWindow(false);
		} else {
			wmAPI->set_dimensions(vidWidth, vidHeight, posX, posY);
		}
	}
}

s32 videoGetTextureFilter2D(void)
{
	return texFilter2D;
}

u32 videoGetTextureFilter(void)
{
	return texFilter;
}

u32 videoGetAnisotropicFilter()
{
	return texAnisotropicFilter;
}

u32 videoGetMaxAnisotropyLevel()
{
	return renderingAPI->get_max_anisotropy_level();
}

s32 videoGetDetailTextures(void)
{
	return texDetail;
}

f32 videoGetGlareBrightness(void)
{
	return vidGlareBrightness;
}

f32 videoGetOverexposureScale(void)
{
	return vidOverexposureScale;
}

s32 videoGetExternalTextures(void)
{
	return texExternal;
}

void videoSetWindowOffset(s32 x, s32 y)
{
	gfx_current_game_window_viewport.x = x;
	gfx_current_game_window_viewport.y = y;
}

void videoSetFullscreen(s32 fs)
{
	if (fs != vidFullscreen) {
		vidFullscreen = !!fs;
		wmAPI->set_closest_resolution(vidWidth, vidHeight, vidCenter);
		wmAPI->set_fullscreen(vidFullscreen);
		if (!vidFullscreen && vidMaximize) {
			wmAPI->set_maximize(false);
			wmAPI->set_maximize(true);
		}
	}
}

void videoSetFullscreenMode(s32 mode)
{
	vidFullscreenExclusive = mode;
	wmAPI->set_fullscreen_flag(mode);
	if (vidFullscreen) {
		wmAPI->set_fullscreen(false);
		wmAPI->set_fullscreen(true);
	}
}

void videoSetMaximizeWindow(s32 fs)
{
	if (fs != vidMaximize) {
		vidMaximize = !!fs;
		wmAPI->set_maximize(vidMaximize);
		if (vidCenter && !vidMaximize) {
			s32 posX = 0;
			s32 posY = 0;
			wmAPI->get_centered_positions(vidWidth, vidHeight, &posX, &posY);
			wmAPI->set_dimensions(vidWidth, vidHeight, posX, posY);
		}
	}
}

void videoSetCenterWindow(s32 center)
{
	vidCenter = center;
	if (vidCenter && !vidMaximize) {
		s32 posX = 0;
		s32 posY = 0;
		wmAPI->get_centered_positions(vidWidth, vidHeight, &posX, &posY);
		wmAPI->set_dimensions(vidWidth, vidHeight, posX, posY);
	}
}

void videoSetTextureFilter(u32 filter)
{
	if (filter > FILTER_THREE_POINT) filter = FILTER_THREE_POINT;
	if (texFilter == filter) return;
	texFilter = filter;
	gfx_set_texture_filter((enum FilteringMode)filter);
}

void videoSetTextureFilter2D(s32 filter)
{
	texFilter2D = !!filter;
}

void videoSetAnisotropicFilter(u32 level)
{
	texAnisotropicFilter = level;
	renderingAPI->set_anisotropy_level(level);
}

void videoSetDetailTextures(s32 detail)
{
	texDetail = !!detail;
	gfx_detail_textures_enabled = (bool)texDetail;
}

void videoCapFramerate(s32 limit)
{
	if (!wmAPI) {
		return;
	}
	if (vidFramerateLimit > 0 && vidFramerateLimit < limit) {
		limit = vidFramerateLimit;
	}
	wmAPI->set_target_fps(videoEffectiveLimit(limit ? limit : vidFramerateLimit));
}

void videoSetGlareBrightness(f32 bright)
{
	vidGlareBrightness = (bright < 0.f ? 0.f : (bright > 1.f ? 1.f : bright));
}

void videoSetOverexposureScale(f32 scale)
{
	vidOverexposureScale = (scale < 0.f ? 0.f : (scale > 1.f ? 1.f : scale));
}

void videoSetExternalTextures(s32 external)
{
	texExternal = !!external;
	gfx_external_textures_enabled = (bool)texExternal;
	// drop cached imports so the toggle takes effect on already-seen textures
	videoResetTextureCache();
}

// Display-list cache master enable (Extended > Video "Display List Cache" +
// Video.DlCache). Off skips the GPU-resident cache (byte-identical to non-cached).
s32 videoGetDlCacheEnabled(void)
{
	extern bool g_DlCacheEnabled;
	return g_DlCacheEnabled;
}

void videoSetDlCacheEnabled(s32 on)
{
	extern bool g_DlCacheEnabled;
	extern void gfx_dlcache_clear(void);
	vidDlCache = !!on;
	g_DlCacheEnabled = (bool)vidDlCache;
	if (!on) {
		gfx_dlcache_clear(); // drop any recorded buffers when disabling
	}
}

// Cached display-list cull winding for the LIVE renderer (Extended > Video "DL
// Cache Flip Winding" + Video.DlCacheFrontFaceGL/GPU). off = default ccw, on = cw
// (the persistent /dlcache ff). Per-renderer so flipping it for Vulkan can't break
// OpenGL (and vice versa).
s32 videoGetDlCacheFlipWinding(void)
{
	return strcmp(videoDlCacheFrontActive(), "cw") == 0;
}

void videoSetDlCacheFlipWinding(s32 flip)
{
	extern void gfx_dlcache_set_frontface(int ccw);
	strcpy(videoDlCacheFrontActive(), flip ? "cw" : "ccw");
	gfx_dlcache_set_frontface(flip ? 0 : 1); // cw = front-face NOT ccw
}

s32 videoCreateFramebuffer(u32 w, u32 h, s32 upscale, s32 autoresize)
{
	if (!wmAPI) return -1;
	return gfx_create_framebuffer(w, h, upscale, autoresize);
}

void videoSetMSAA(const s32 msaa)
{
	vidMSAA = msaa;
	gfx_msaa_level = (u32)vidMSAA;
}

void videoSetVsync(const s32 vsync)
{
	if (!wmAPI) return;
	// -2 = VRR mode: vsync off at the backend (tearing-allowed presentation,
	// which VRR displays sync to) + the automatic below-refresh cap applied
	// via videoEffectiveLimit in the framerate-limit re-apply below
	const s32 interval = (vsync == -2) ? 0 : vsync;
	vidVsync = wmAPI->set_swap_interval(interval) ? vsync : 0;

	// No auto-cap on vsync-off: a framerate limit of 0 now means TRULY unlimited
	// (the SDL layer skips frame pacing when target_fps == 0). Re-apply the current
	// limit so the swap-interval change takes effect. With vsync on, presentation
	// is still paced by the vblank regardless of the 0 limit; with vsync off and
	// limit 0 the renderer runs unbounded (single-player). Netplay applies its own
	// ceiling via videoCapFramerate (vidNetplayFramerateLimit).
	videoSetFramerateLimit(vidFramerateLimit);
}

void videoSetFramerateLimit(const s32 limit)
{
	if (!wmAPI) return;
	// 0 == truly unlimited (no frame pacing). Previously 0 with vsync off was
	// force-bumped to VIDEO_MAX_FPS as a safety; that prevented an intentional
	// unlimited cap, so it's removed. set_target_fps(0) disables pacing entirely.
	// In VRR mode the applied value is additionally bounded just below the
	// display refresh (videoEffectiveLimit); vidFramerateLimit keeps the
	// user's configured value.
	vidFramerateLimit = limit;
	wmAPI->set_target_fps(videoEffectiveLimit(vidFramerateLimit));
}

void videoSetDisplayFPS(const s32 displayfps)
{
	vidDisplayFPS = displayfps;
}

void videoSetFramebuffer(s32 target)
{
	if (!wmAPI) return;
	return gfx_set_framebuffer(target, 1.f);
}

void videoResetFramebuffer(void)
{
	if (!wmAPI) return;
	return gfx_reset_framebuffer();
}

s32 videoFramebuffersSupported(void)
{
	return gfx_framebuffers_enabled;
}

void videoResizeFramebuffer(s32 target, u32 w, u32 h, s32 upscale, s32 autoresize)
{
	if (!wmAPI) return;
	gfx_resize_framebuffer(target, w, h, upscale, autoresize);
}

void videoCopyFramebuffer(s32 dst, s32 src, s32 left, s32 top)
{
	if (!wmAPI) return;
	// assume immediate copies always read the front buffer
	gfx_copy_framebuffer(dst, src, left, top, false);
}

void videoResetTextureCache(void)
{
	if (!wmAPI) return;
	gfx_texture_cache_clear();
}

void videoFreeCachedTexture(const void *texptr)
{
	if (!wmAPI) return;
	gfx_texture_cache_delete(texptr);
}

void videoFreeCachedTextures(const void *start, const void *end)
{
	gfx_texture_cache_delete_range(start, end);
}

// Renderer selection for the Extended > Video menu, flattened to one index:
// 0 = OpenGL, 1 = SDL_GPU/Vulkan, 2 = SDL_GPU/Direct3D 12 (Windows) or
// SDL_GPU/Metal (macOS). Pure config writes — applied on next startup.
s32 videoGetRendererSetting(void)
{
#ifdef USE_SDLGPU
	if (strcmp(vidRenderer, "sdlgpu") == 0) {
		if (strcmp(vidGpuDriver, "direct3d12") == 0 || strcmp(vidGpuDriver, "metal") == 0) {
			return 2;
		}
		return 1;
	}
#endif
	return 0;
}

void videoSetRendererSetting(s32 idx)
{
	if (idx <= 0) {
		strcpy(vidRenderer, "opengl");
		vidGpuDriver[0] = '\0';
		return;
	}
	strcpy(vidRenderer, "sdlgpu");
	if (idx >= 2) {
#if defined(__APPLE__)
		strcpy(vidGpuDriver, "metal");
#else
		strcpy(vidGpuDriver, "direct3d12");
#endif
	} else {
		strcpy(vidGpuDriver, "vulkan");
	}
}

s32 videoGetHDR(void)
{
	return vidHDR;
}

void videoSetHDR(s32 on)
{
	vidHDR = on; // applied on next startup (swapchain + target formats)
}

f32 videoGetHDRPaperWhite(void)
{
	return vidHDRPaperWhite;
}

void videoSetHDRPaperWhite(f32 nits)
{
	vidHDRPaperWhite = nits;
#ifdef USE_SDLGPU
	gfx_sdlgpu_set_hdr_paperwhite(nits); // live when HDR is active
#endif
}

f32 videoGetHDRPeak(void)
{
	return vidHDRPeak;
}

void videoSetHDRPeak(f32 nits)
{
	vidHDRPeak = nits;
#ifdef USE_SDLGPU
	gfx_sdlgpu_set_hdr_peak(nits); // live when HDR is active
#endif
}

void videoGetRendererInfo(char *buf, u32 len)
{
	if (!wmAPI || !renderingAPI) {
		snprintf(buf, len, "none (headless)");
		return;
	}
#ifdef USE_SDLGPU
	if (renderingAPI == &gfx_sdlgpu_api) {
		char tmp[256];
		gfx_sdlgpu_get_info(tmp, sizeof(tmp));
		snprintf(buf, len, "SDL_GPU: %s", tmp);
		return;
	}
#endif
	snprintf(buf, len, "%s", renderingAPI->get_name());
}

void videoShutdown(void)
{
	// In headless dedicated, vidModes was never reassigned away from the
	// static &vidModeDefault default — calling free() on it is undefined.
	if (vidModes != &vidModeDefault) {
		free(vidModes);
	}
}

PD_CONSTRUCTOR static void videoConfigInit(void)
{
	configRegisterString("Video.Renderer", vidRenderer, sizeof(vidRenderer));
	configRegisterString("Video.GpuDriver", vidGpuDriver, sizeof(vidGpuDriver));
	configRegisterInt("Video.HDR", &vidHDR, 0, 1);
	configRegisterFloat("Video.HDRPaperWhite", &vidHDRPaperWhite, 80.f, 1000.f);
	configRegisterFloat("Video.HDRPeak", &vidHDRPeak, 80.f, 4000.f);
	configRegisterInt("Video.DefaultFullscreen", &vidFullscreen, 0, 1);
	configRegisterInt("Video.DefaultMaximize", &vidMaximize, 0, 1);
	configRegisterInt("Video.DefaultWidth", &vidWidth, 0, 32767);
	configRegisterInt("Video.DefaultHeight", &vidHeight, 0, 32767);
	configRegisterInt("Video.ExclusiveFullscreen", &vidFullscreenExclusive, 0, 1);
	configRegisterFloat("Video.RefreshRate", &vidRefreshRate, 0.f, 1000.f);
	configRegisterInt("Video.CenterWindow", &vidCenter, 0, 1);
	configRegisterInt("Video.AllowHiDpi", &vidAllowHiDpi, 0, 1);
	configRegisterInt("Video.VSync", &vidVsync, -2, 10); // -2 = VRR mode
	configRegisterInt("Video.FramebufferEffects", &vidFramebuffers, 0, 1);
	configRegisterInt("Video.FramerateLimit", &vidFramerateLimit, 0, 10000);
	configRegisterInt("Video.NetplayFramerateLimit", &vidNetplayFramerateLimit, 0, 10000);
	configRegisterInt("Video.DisplayFPS", &vidDisplayFPS, 0, 1);
	configRegisterFloat("Video.DisplayFPSInterval", &vidDisplayFPSInterval, 0.01f, 32.f);
	configRegisterInt("Video.MSAA", &vidMSAA, 1, 16);
	configRegisterInt("Video.TextureFilter", &texFilter, 0, 2);
	configRegisterInt("Video.TextureFilter2D", &texFilter2D, 0, 1);
	configRegisterInt("Video.DetailTextures", &texDetail, 0, 1);
	configRegisterInt("Video.MipmapFilter", &texMipmapFilter, 0, 2);
	configRegisterInt("Video.AnisotropicFilter", &texAnisotropicFilter, 0, 16);
	configRegisterFloat("Video.GlareBrightness", &vidGlareBrightness, 0.f, 1.f);
	configRegisterFloat("Video.OverexposureScale", &vidOverexposureScale, 0.f, 1.f);
	configRegisterInt("Video.ExternalTextures", &texExternal, 0, 1);
	configRegisterInt("Video.TextureCacheSize", &texCacheSize, 256, 262144);
	configRegisterInt("Video.DlCache", &vidDlCache, 0, 1);
	configRegisterString("Video.DlCacheCull", vidDlCacheCull, sizeof(vidDlCacheCull));
	configRegisterString("Video.DlCacheFrontFaceGL", vidDlCacheFrontGL, sizeof(vidDlCacheFrontGL));
	configRegisterString("Video.DlCacheFrontFaceGPU", vidDlCacheFrontGpu, sizeof(vidDlCacheFrontGpu));

	// Screen-space raytracing suite (docs/PORT_RAYTRACING.md; live control via
	// the /rt console command). GL backend only; default off.
	configRegisterInt("Video.RT.Enabled", &gfx_rt_enabled, 0, 1);
	configRegisterInt("Video.RT.AO", &gfx_rt_ao, 0, 1);
	configRegisterInt("Video.RT.Shadows", &gfx_rt_shadows, 0, 1);
	configRegisterInt("Video.RT.SSR", &gfx_rt_ssr, 0, 1);
	configRegisterInt("Video.RT.GI", &gfx_rt_gi, 0, 2);
	configRegisterInt("Video.RT.Quality", &gfx_rt_quality, 0, 2);
	configRegisterFloat("Video.RT.AOIntensity", &gfx_rt_ao_intensity, 0.f, 1.f);
	configRegisterFloat("Video.RT.AORadius", &gfx_rt_ao_radius, 1.f, 1000.f);
	configRegisterFloat("Video.RT.ShadowIntensity", &gfx_rt_shadow_intensity, 0.f, 1.f);
	configRegisterFloat("Video.RT.ShadowLength", &gfx_rt_shadow_length, 1.f, 2000.f);
	configRegisterFloat("Video.RT.SSRIntensity", &gfx_rt_ssr_intensity, 0.f, 1.f);
	configRegisterFloat("Video.RT.GIIntensity", &gfx_rt_gi_intensity, 0.f, 4.f);
	configRegisterFloat("Video.RT.GIScale", &gfx_rt_gi_scale, 0.25f, 1.f);
	configRegisterFloat("Video.RT.SunX", &gfx_rt_sun_dir[0], -1.f, 1.f);
	configRegisterFloat("Video.RT.SunY", &gfx_rt_sun_dir[1], -1.f, 1.f);
	configRegisterFloat("Video.RT.SunZ", &gfx_rt_sun_dir[2], -1.f, 1.f);
	configRegisterFloat("Video.RT.SkyR", &gfx_rt_sky[0], 0.f, 4.f);
	configRegisterFloat("Video.RT.SkyG", &gfx_rt_sky[1], 0.f, 4.f);
	configRegisterFloat("Video.RT.SkyB", &gfx_rt_sky[2], 0.f, 4.f);
	configRegisterInt("Video.RT.Dark", &gfx_rt_dark, 0, 1);
	configRegisterFloat("Video.RT.DarkAmbient", &gfx_rt_dark_ambient, 0.f, 1.f);
	configRegisterInt("Video.RT.Lights", &gfx_rt_lights, 0, 1);
	configRegisterInt("Video.RT.LightShadows", &gfx_rt_light_shadows, 0, 1);
	configRegisterFloat("Video.RT.LightIntensity", &gfx_rt_light_intensity, 0.f, 8.f);
	configRegisterFloat("Video.RT.LightRadius", &gfx_rt_light_radius, 50.f, 5000.f);
	configRegisterFloat("Video.RT.LightCull", &gfx_rt_light_cull, 0.f, 20000.f);
	configRegisterFloat("Video.RT.LightMax", &gfx_rt_light_max, 0.05f, 8.f);
	configRegisterInt("Video.RT.Skylight", &gfx_rt_skylight, 0, 1);
	configRegisterFloat("Video.RT.SkylightGain", &gfx_rt_skylight_gain, 0.f, 4.f);
	configRegisterInt("Video.RT.Bounces", &gfx_rt_bounces, 0, 8);
	configRegisterInt("Video.RT.AutoSun", &gfx_rt_autosun, 0, 1);
	configRegisterFloat("Video.RT.Relight", &gfx_rt_relight, 0.f, 1.f);
	configRegisterInt("Video.RT.Torch", &gfx_rt_torch, 0, 1);
	configRegisterFloat("Video.RT.TorchIntensity", &gfx_rt_torch_intensity, 0.f, 8.f);
	configRegisterFloat("Video.RT.TorchRange", &gfx_rt_torch_range, 100.f, 10000.f);
}
