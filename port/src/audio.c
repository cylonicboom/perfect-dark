#include <PR/ultratypes.h>
#include <stdio.h>
#ifndef DEDICATED_SERVER
#include <SDL3/SDL.h>
#endif
#include "platform.h"
#include "config.h"
#include "audio.h"
#include "system.h"

// Forward-decl only: avoid pulling net/net.h -> types.h, which redefines
// `bool` and would clash with SDL's <stdbool.h>.
extern s32 g_NetDedicatedMode;

#ifndef DEDICATED_SERVER
static SDL_AudioStream *stream;
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
		// Headless dedicated: no audio device, no mixer output. stream stays
		// NULL; audioEndFrame / audioGetBytesBuffered guard on it so nothing
		// crashes if they somehow get called past the g_SndDisabled gate.
		sysLogPrintf(LOG_NOTE, "audio: headless dedicated server, skipping init");
		return 0;
	}

	if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
		sysLogPrintf(LOG_ERROR, "SDL audio init error: %s", SDL_GetError());
		return -1;
	}

	SDL_AudioSpec spec;
	SDL_zero(spec);
	spec.freq = 22020; // TODO: this might cause trouble for some platforms
	spec.format = SDL_AUDIO_S16; // native byte order, like SDL2's AUDIO_S16SYS
	spec.channels = 2;

	// SDL3 has no SDL_AudioSpec.samples; the device buffer size is a hint
	char sampleStr[16];
	snprintf(sampleStr, sizeof(sampleStr), "%d", bufferSize);
	SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, sampleStr);

	nextBuf = NULL;

	stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
	if (!stream) {
		sysLogPrintf(LOG_ERROR, "SDL_OpenAudioDeviceStream error: %s", SDL_GetError());
		return -1;
	}

	// the device starts paused (replaces SDL2's SDL_PauseAudioDevice(dev, 0))
	SDL_ResumeAudioStreamDevice(stream);

	return 0;
#endif
}

s32 audioGetBytesBuffered(void)
{
#ifdef DEDICATED_SERVER
	return 0;
#else
	return stream ? SDL_GetAudioStreamQueued(stream) : 0;
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
		if (stream && audioGetSamplesBuffered() < queueLimit) {
			SDL_PutAudioStreamData(stream, nextBuf, nextSize);
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
