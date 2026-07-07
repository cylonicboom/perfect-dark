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

// Chaos audio extras (docs/PORT_CHAOS.md):
// - audioMuted (pd.mute): the outgoing device buffer is replaced with silence
//   at the single push point in audioEndFrame — a true master mute (SFX +
//   music) with no interaction with the persisted volume settings.
// - extSound (pd.play_file): a one-shot external WAV, pre-converted to the
//   device spec at load, additively mixed into the outgoing buffer until it
//   runs out. Used for the Ring Ring effect's Discord ringtone.
static s32 audioMuted = 0;
static u8 *extSound = NULL;
static u32 extSoundLen = 0;
static u32 extSoundPos = 0;
static s16 *mixBuf = NULL;
static u32 mixBufCap = 0;
#endif

static s32 bufferSize = 512;
static s32 queueLimit = 8192;

void audioSetMuted(s32 on)
{
#ifndef DEDICATED_SERVER
	audioMuted = on;
#endif
}

s32 audioPlayExternal(const char *path)
{
#ifdef DEDICATED_SERVER
	return 0;
#else
	SDL_AudioSpec wavspec;
	SDL_AudioSpec dstspec;
	Uint8 *wavdata = NULL;
	Uint32 wavlen = 0;
	Uint8 *conv = NULL;
	int convlen = 0;

	if (!stream || !path || !path[0]) {
		return 0;
	}

	if (!SDL_LoadWAV(path, &wavspec, &wavdata, &wavlen)) {
		sysLogPrintf(LOG_WARNING, "audio: can't load '%s': %s", path, SDL_GetError());
		return 0;
	}

	SDL_zero(dstspec);
	dstspec.format = SDL_AUDIO_S16;
	dstspec.channels = 2;
	dstspec.freq = 22020; // must match the device stream opened in audioInit

	if (!SDL_ConvertAudioSamples(&wavspec, wavdata, (int)wavlen, &dstspec, &conv, &convlen)) {
		sysLogPrintf(LOG_WARNING, "audio: can't convert '%s': %s", path, SDL_GetError());
		SDL_free(wavdata);
		return 0;
	}

	SDL_free(wavdata);

	if (extSound) {
		SDL_free(extSound);
	}

	extSound = conv;
	extSoundLen = (u32)convlen;
	extSoundPos = 0;
	return 1;
#endif
}

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
			const void *out = nextBuf;

			// Chaos mute / external one-shot: both need a mutable copy of the
			// outgoing buffer (the mixer owns nextBuf). Push cadence and sizes
			// are unchanged so the frame pacing that reads the queued-bytes
			// count stays identical.
			if (audioMuted || (extSound && extSoundPos < extSoundLen)) {
				if (mixBufCap < nextSize) {
					mixBuf = (s16 *)SDL_realloc(mixBuf, nextSize);
					mixBufCap = mixBuf ? nextSize : 0;
				}

				if (mixBuf) {
					if (audioMuted) {
						SDL_memset(mixBuf, 0, nextSize);
					} else {
						SDL_memcpy(mixBuf, nextBuf, nextSize);
					}

					// mix the external sound (already device-spec s16 stereo);
					// muted mutes it too
					if (!audioMuted && extSound && extSoundPos < extSoundLen) {
						const s16 *ext = (const s16 *)(extSound + extSoundPos);
						u32 bytes = extSoundLen - extSoundPos;
						u32 i, n;

						if (bytes > nextSize) {
							bytes = nextSize;
						}
						n = bytes / sizeof(s16);

						for (i = 0; i < n; i++) {
							s32 s = (s32)mixBuf[i] + (s32)ext[i];
							if (s > 32767) s = 32767;
							if (s < -32768) s = -32768;
							mixBuf[i] = (s16)s;
						}

						extSoundPos += bytes;
					}

					out = mixBuf;
				}
			}

			if (extSound && extSoundPos >= extSoundLen) {
				SDL_free(extSound);
				extSound = NULL;
				extSoundLen = extSoundPos = 0;
			}

			SDL_PutAudioStreamData(stream, out, nextSize);
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
