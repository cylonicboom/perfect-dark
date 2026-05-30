#include <PR/ultratypes.h>
#include <stdio.h>
#ifndef DEDICATED_SERVER
#include <SDL.h>
#endif
#include "platform.h"
#include "config.h"
#include "audio.h"
#include "system.h"

// Forward-decl only: avoid pulling net/net.h -> types.h, which redefines
// `bool` and would clash with SDL's <stdbool.h>.
extern s32 g_NetDedicatedMode;

#ifndef DEDICATED_SERVER
static SDL_AudioDeviceID dev;
static const s16 *nextBuf;
static u32 nextSize = 0;
#endif

static s32 bufferSize = 512;
static s32 queueLimit = 8192;

s32 audioInit(void)
{
#ifdef DEDICATED_SERVER
	// Server-only build: no audio device, no SDL.
	return 0;
#else
	if (g_NetDedicatedMode == 1) {
		// Headless dedicated: no audio device, no mixer output. dev stays 0;
		// SDL_QueueAudio(0, ...) is a no-op so audioEndFrame won't crash if
		// it somehow gets called past the g_SndDisabled gate.
		sysLogPrintf(LOG_NOTE, "audio: headless dedicated server, skipping init");
		return 0;
	}

	if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
		sysLogPrintf(LOG_ERROR, "SDL audio init error: %s", SDL_GetError());
		return -1;
	}

	SDL_AudioSpec want, have;
	SDL_zero(want);
	want.freq = 22020; // TODO: this might cause trouble for some platforms
	want.format = AUDIO_S16SYS;
	want.channels = 2;
	want.samples = bufferSize;
	want.callback = NULL;

	nextBuf = NULL;

	dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
	if (dev == 0) {
		sysLogPrintf(LOG_ERROR, "SDL_OpenAudio error: %s", SDL_GetError());
		return -1;
	}

	SDL_PauseAudioDevice(dev, 0);

	return 0;
#endif
}

s32 audioGetBytesBuffered(void)
{
#ifdef DEDICATED_SERVER
	return 0;
#else
	return SDL_GetQueuedAudioSize(dev);
#endif
}

s32 audioGetSamplesBuffered(void)
{
	return audioGetBytesBuffered() / 4;
}

void audioSetNextBuffer(const s16 *buf, u32 len)
{
#ifdef DEDICATED_SERVER
	(void)buf;
	(void)len;
#else
	nextBuf = buf;
	nextSize = len;
#endif
}

void audioEndFrame(void)
{
#ifndef DEDICATED_SERVER
	if (nextBuf && nextSize) {
		if (audioGetSamplesBuffered() < queueLimit) {
			SDL_QueueAudio(dev, nextBuf, nextSize);
		}
		nextBuf = NULL;
		nextSize = 0;
	}
#endif
}

PD_CONSTRUCTOR static void audioConfigInit(void)
{
	configRegisterInt("Audio.BufferSize", &bufferSize, 0, 1 * 1024 * 1024);
	configRegisterInt("Audio.QueueLimit", &queueLimit, 0, 1 * 1024 * 1024);
}
