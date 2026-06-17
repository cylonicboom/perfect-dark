#ifndef _IN_VIDEO_H
#define _IN_VIDEO_H

#include <PR/ultratypes.h>
#include <PR/gbi.h>

// maximum framerate; if the game runs faster than this, things will break
#if PAL
#define VIDEO_MAX_FPS 200
#else
#define VIDEO_MAX_FPS 240
#endif

typedef struct {
	s32 width;
	s32 height;
} displaymode;

// taskbar/dock progress indicator states (videoSetTaskbarProgress)
enum taskbarprogress {
	VIDEO_TASKBAR_NONE = 0,
	VIDEO_TASKBAR_INDETERMINATE = 1,
	VIDEO_TASKBAR_NORMAL = 2,
};

s32 videoInit(void);
void videoStartFrame(void);
void videoSubmitCommands(Gfx *cmds);
void videoClearScreen(void);
void videoEndFrame(void);

void *videoGetWindowHandle(void);

// taskbar/dock progress: state is a VIDEO_TASKBAR_* value, value is 0..1
// (only used for VIDEO_TASKBAR_NORMAL); no-op when unsupported or headless
void videoSetTaskbarProgress(s32 state, f32 value);

// HiDPI rendering (SDL_WINDOW_HIGH_PIXEL_DENSITY); set takes effect on restart
s32 videoGetAllowHiDpi(void);
void videoSetAllowHiDpi(s32 allow);

// exclusive-fullscreen refresh rate; 0 = auto. videoGetRefreshRates fills out
// with the distinct rates available for the currently selected resolution.
s32 videoGetRefreshRates(f32 *out, s32 max);
f32 videoGetRefreshRate(void);
void videoSetRefreshRate(f32 hz);

void videoUpdateNativeResolution(s32 w, s32 h);
s32 videoGetNativeWidth(void);
s32 videoGetNativeHeight(void);

s32 videoGetWidth(void);
s32 videoGetHeight(void);
f32 videoGetAspect(void);
s32 videoGetFullscreen(void);
s32 videoGetFullscreenMode(void);
s32 videoGetMaximizeWindow(void);
void videoSetMaximizeWindow(s32 fs);
s32 videoGetCenterWindow(void);
void videoSetCenterWindow(s32 center);
u32 videoGetTextureFilter(void);
s32 videoGetTextureFilter2D(void);
u32 videoGetAnisotropicFilter(void);
u32 videoGetMaxAnisotropyLevel(void);
s32 videoGetDetailTextures(void);
s32 videoGetExternalTextures(void);
s32 videoGetDisplayModeIndex(void);
s32 videoGetDisplayMode(displaymode *out, const s32 index);
s32 videoGetNumDisplayModes(void);
s32 videoGetVsync(void);
s32 videoGetFramerateLimit(void);
s32 videoGetNetplayFramerateLimit(void);
s32 videoGetDisplayFPS(void);
s32 videoGetMSAA(void);
f32 videoGetGlareBrightness(void);
f32 videoGetOverexposureScale(void);

f32 videoGetAverageFPS(void);

void videoCapFramerate(s32 limit);

void videoSetWindowOffset(s32 x, s32 y);
void videoSetFullscreen(s32 fs);
void videoSetFullscreenMode(s32 mode);
void videoSetTextureFilter(u32 filter);
void videoSetTextureFilter2D(s32 filter);
void videoSetAnisotropicFilter(u32 filter);
void videoSetDetailTextures(s32 detail);
void videoSetExternalTextures(s32 external);
s32 videoGetDlCacheFlipWinding(void);
void videoSetDlCacheFlipWinding(s32 flip);
void videoSetDisplayMode(const s32 index);
void videoSetVsync(const s32 vsync);
void videoSetFramerateLimit(const s32 limit);
void videoSetNetplayFramerateLimit(const s32 limit);
void videoSetDisplayFPS(const s32 displayfps);
void videoSetMSAA(const s32 msaa);
void videoSetGlareBrightness(f32 bright);
void videoSetOverexposureScale(f32 scale);

s32 videoCreateFramebuffer(u32 w, u32 h, s32 upscale, s32 autoresize);
void videoSetFramebuffer(s32 target);
void videoResetFramebuffer(void);
void videoCopyFramebuffer(s32 dst, s32 src, s32 left, s32 top);
void videoResizeFramebuffer(s32 target, u32 w, u32 h, s32 upscale, s32 autoresize);
s32 videoFramebuffersSupported(void);

void videoResetTextureCache(void);
void videoFreeCachedTexture(const void *texptr);
void videoFreeCachedTextures(const void *start, const void *end);

// one-line active-renderer summary for the /gpu console command: backend name
// plus (for SDL_GPU) driver / shader format / msaa / vsync / hdr / shader cache
void videoGetRendererInfo(char *buf, u32 len);

// renderer picker for the Extended > Video menu: 0 = OpenGL,
// 1 = SDL GPU (Vulkan), 2 = SDL GPU (Direct3D 12 / Metal). Config-only;
// applies on next startup.
s32 videoGetRendererSetting(void);
void videoSetRendererSetting(s32 idx);

// HDR output (SDL_GPU only). The toggle applies on next startup; paper white
// and peak (nits) apply live while HDR is active. Peak <= paper white
// disables highlight expansion.
s32 videoGetHDR(void);
void videoSetHDR(s32 on);
f32 videoGetHDRPaperWhite(void);
void videoSetHDRPaperWhite(f32 nits);
f32 videoGetHDRPeak(void);
void videoSetHDRPeak(f32 nits);

void videoShutdown(void);

#endif
