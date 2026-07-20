#include <PR/ultratypes.h>
#include <stdio.h>
#ifndef DEDICATED_SERVER
#include <SDL3/SDL.h>
#endif
#include "platform.h"
#include "config.h"
#include "audio.h"
#include "system.h"
#ifndef DEDICATED_SERVER
// decoder implementation lives in port/external/minimp3.c
#include "external/minimp3.h"
#endif

// Forward-decl only: avoid pulling net/net.h -> types.h, which redefines
// `bool` and would clash with SDL's <stdbool.h>.
extern s32 g_NetDedicatedMode;
// Game-side music volume (src/game/options.c). Forward-declared to avoid pulling
// the game headers here. Range is 0..0x5000 (the NTSC music-slider max); used to
// scale a pd.play_file track that opts into following the in-game music volume.
extern u16 optionsGetMusicVolume(void);
#define AUDIO_MUSICVOL_MAX 0x5000
// Game pause state (src/game/lv.c). Declared s32 to match the decompiled build's
// 4-byte `bool` ABI. A pd.play_file music track (followMusic) holds its position
// and goes silent while the game is paused, so it resumes cleanly from the menu.
extern s32 lvIsPaused(void);

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
static s32 extLoop = 0; // pd.play_file(path, loop): rewind instead of freeing
static s32 extFollowMusic = 0; // pd.play_file(...,follow): scale by music volume
static s16 *mixBuf = NULL;
static u32 mixBufCap = 0;
// - bitcrush (pd.audio_crush): sample-and-hold every crushStep'th stereo
//   frame, masked to crushBits of depth — the classic low-sample-rate +
//   low-bit-depth crunch. The hold value/phase persist across buffer pushes
//   so the effective sample rate is continuous. step 1 / bits 16 = off.
static s32 audioCrushStep = 1;
static s32 audioCrushBits = 16;
static s16 audioCrushL = 0;
static s16 audioCrushR = 0;
static u32 audioCrushPhase = 0;
// - radio (pd.audio_radio): AM-radio voicing — a ~400..2800Hz bandpass (two
//   one-pole filters per channel) plus overdrive into a hard clip. Filter
//   state persists across pushes. Used by the "1964 mode" sepia effect.
static s32 audioRadioOn = 0;
static f32 audioRadioLp[2];
static f32 audioRadioHp[2];
// - reverb (pd.audio_reverb): Freeverb-style network — 4 damped feedback
//   combs + 2 series allpasses per channel, tunings halved for the 22020Hz
//   device rate, right channel spread by +12 samples. wet 0 = off.
#define REVERB_NCOMB 4
#define REVERB_NAP 2
#define REVERB_MAXCOMB 704
#define REVERB_MAXAP 304
static const u16 audioReverbCombLen[2][REVERB_NCOMB] = { { 558, 594, 638, 678 }, { 570, 606, 650, 690 } };
static const u16 audioReverbApLen[2][REVERB_NAP] = { { 278, 112 }, { 290, 124 } };
static f32 audioReverbWet = 0.0f;
static f32 audioReverbComb[2][REVERB_NCOMB][REVERB_MAXCOMB];
static u16 audioReverbCombIdx[2][REVERB_NCOMB];
static f32 audioReverbCombFilt[2][REVERB_NCOMB];
static f32 audioReverbAp[2][REVERB_NAP][REVERB_MAXAP];
static u16 audioReverbApIdx[2][REVERB_NAP];
// - reverse (pd.audio_reverse): granular time reversal — fill one chunk while
//   playing the previous chunk backwards (double buffer, ~0.74s each), so
//   everything comes out backwards in ~0.74s granules with that much latency.
//   Until the first chunk completes the stream passes through untouched.
#define REV_CHUNK 16384 // frames
static s16 audioRevBuf[2][REV_CHUNK * 2];
static s32 audioRevOn = 0;
static u32 audioRevPos = 0;    // frame index into the fill chunk
static s32 audioRevFill = 0;   // which of the two buffers is being filled
static s32 audioRevValid = 0;  // a completed chunk exists to play back
// - pitch (pd.audio_pitch): granular pitch shift at constant tempo — a ring
//   of recent input read back at `rate` with linear interpolation; when the
//   read head drifts too close to (or too far from) the write head it jumps
//   by one grain with a short crossfade to hide the seam. rate 1 = off.
#define PITCH_RING 8192 // frames, power of two
#define PITCH_GRAIN 1764 // ~80ms
#define PITCH_XFADE 64
static f32 audioPitchRate = 1.0f;
static s16 audioPitchRing[PITCH_RING * 2];
static u32 audioPitchWrite = 0;  // absolute frame counter (write head)
static f64 audioPitchRead = 0.0; // absolute frame position (read head)
static f64 audioPitchOld = 0.0;  // pre-jump read position while crossfading
static s32 audioPitchFade = 0;   // crossfade frames remaining
#endif

static s32 bufferSize = 512;
static s32 queueLimit = 8192;

void audioSetMuted(s32 on)
{
#ifndef DEDICATED_SERVER
	audioMuted = on;
#endif
}

void audioSetCrush(s32 step, s32 bits)
{
#ifndef DEDICATED_SERVER
	audioCrushStep = step < 1 ? 1 : step > 64 ? 64 : step;
	audioCrushBits = bits < 1 ? 1 : bits > 16 ? 16 : bits;
	audioCrushPhase = 0;
#else
	(void)step;
	(void)bits;
#endif
}

void audioSetRadio(s32 on)
{
#ifndef DEDICATED_SERVER
	if (on && !audioRadioOn) {
		audioRadioLp[0] = audioRadioLp[1] = 0.0f;
		audioRadioHp[0] = audioRadioHp[1] = 0.0f;
	}
	audioRadioOn = on ? 1 : 0;
#else
	(void)on;
#endif
}

void audioSetReverb(f32 wet)
{
#ifndef DEDICATED_SERVER
	wet = wet < 0.0f ? 0.0f : wet > 1.0f ? 1.0f : wet;
	if (wet > 0.0f && audioReverbWet <= 0.0f) {
		// coming from off: flush stale tails
		SDL_memset(audioReverbComb, 0, sizeof(audioReverbComb));
		SDL_memset(audioReverbAp, 0, sizeof(audioReverbAp));
		SDL_memset(audioReverbCombFilt, 0, sizeof(audioReverbCombFilt));
	}
	audioReverbWet = wet;
#else
	(void)wet;
#endif
}

void audioSetReverse(s32 on)
{
#ifndef DEDICATED_SERVER
	audioRevOn = on ? 1 : 0;
	audioRevPos = 0;
	audioRevFill = 0;
	audioRevValid = 0;
#else
	(void)on;
#endif
}

void audioSetPitch(f32 rate)
{
#ifndef DEDICATED_SERVER
	rate = rate < 0.25f ? 0.25f : rate > 4.0f ? 4.0f : rate;
	if (rate != 1.0f && audioPitchRate == 1.0f) {
		SDL_memset(audioPitchRing, 0, sizeof(audioPitchRing));
		// start the read head one grain behind the write head, both offset
		// into the (zeroed) ring so positions never go negative on a jump
		audioPitchWrite = PITCH_RING;
		audioPitchRead = (f64)(PITCH_RING - PITCH_GRAIN);
		audioPitchFade = 0;
	}
	audioPitchRate = rate;
#else
	(void)rate;
#endif
}

#ifndef DEDICATED_SERVER
// --- chaos audio processing helpers (called from audioEndFrame's chain) ---

// granular reversal: store each incoming frame in the fill chunk, output the
// previous chunk backwards at the mirrored index
static void audioProcessReverse(s16 *buf, u32 frames)
{
	u32 i;

	for (i = 0; i < frames; i++) {
		s16 *fill = &audioRevBuf[audioRevFill][audioRevPos * 2];
		const s16 *play = &audioRevBuf[1 - audioRevFill][(REV_CHUNK - 1 - audioRevPos) * 2];

		fill[0] = buf[i * 2 + 0];
		fill[1] = buf[i * 2 + 1];

		if (audioRevValid) {
			buf[i * 2 + 0] = play[0];
			buf[i * 2 + 1] = play[1];
		}

		if (++audioRevPos >= REV_CHUNK) {
			audioRevPos = 0;
			audioRevFill = 1 - audioRevFill;
			audioRevValid = 1;
		}
	}
}

// read one interpolated frame from the pitch ring at absolute position pos
static void audioPitchTap(f64 pos, f32 *l, f32 *r)
{
	const u32 i0 = (u32)pos & (PITCH_RING - 1);
	const u32 i1 = (i0 + 1) & (PITCH_RING - 1);
	const f32 fr = (f32)(pos - (f64)(u64)pos);

	*l = (f32)audioPitchRing[i0 * 2 + 0] + ((f32)audioPitchRing[i1 * 2 + 0] - (f32)audioPitchRing[i0 * 2 + 0]) * fr;
	*r = (f32)audioPitchRing[i0 * 2 + 1] + ((f32)audioPitchRing[i1 * 2 + 1] - (f32)audioPitchRing[i0 * 2 + 1]) * fr;
}

// granular pitch shift: constant tempo, grain-jump with crossfade on drift
static void audioProcessPitch(s16 *buf, u32 frames)
{
	u32 i;

	for (i = 0; i < frames; i++) {
		u32 wr = audioPitchWrite & (PITCH_RING - 1);
		f32 l, r, l2, r2;
		f64 lag;

		audioPitchRing[wr * 2 + 0] = buf[i * 2 + 0];
		audioPitchRing[wr * 2 + 1] = buf[i * 2 + 1];
		audioPitchWrite++;

		// keep the read head inside [GRAIN/4, GRAIN*2] behind the write head;
		// jump one grain (with a crossfade) when it drifts out
		lag = (f64)audioPitchWrite - audioPitchRead;
		if (lag < PITCH_GRAIN / 4) {
			audioPitchOld = audioPitchRead;
			audioPitchRead -= PITCH_GRAIN;
			audioPitchFade = PITCH_XFADE;
		} else if (lag > PITCH_GRAIN * 2) {
			audioPitchOld = audioPitchRead;
			audioPitchRead += PITCH_GRAIN;
			audioPitchFade = PITCH_XFADE;
		}

		audioPitchTap(audioPitchRead, &l, &r);

		if (audioPitchFade > 0) {
			const f32 mix = (f32)audioPitchFade / PITCH_XFADE;
			audioPitchTap(audioPitchOld, &l2, &r2);
			l = l * (1.0f - mix) + l2 * mix;
			r = r * (1.0f - mix) + r2 * mix;
			audioPitchOld += audioPitchRate;
			audioPitchFade--;
		}

		audioPitchRead += audioPitchRate;

		buf[i * 2 + 0] = (s16)(l < -32768.0f ? -32768.0f : l > 32767.0f ? 32767.0f : l);
		buf[i * 2 + 1] = (s16)(r < -32768.0f ? -32768.0f : r > 32767.0f ? 32767.0f : r);
	}
}

// AM radio: ~400..2800Hz bandpass + overdrive clip
static void audioProcessRadio(s16 *buf, u32 frames)
{
	u32 i;
	s32 ch;

	for (i = 0; i < frames; i++) {
		for (ch = 0; ch < 2; ch++) {
			const f32 x = (f32)buf[i * 2 + ch];
			f32 y;

			audioRadioLp[ch] += 0.55f * (x - audioRadioLp[ch]);            // ~2.8kHz lowpass
			audioRadioHp[ch] += 0.11f * (audioRadioLp[ch] - audioRadioHp[ch]); // tracks <~430Hz
			y = (audioRadioLp[ch] - audioRadioHp[ch]) * 2.4f;              // band + drive

			buf[i * 2 + ch] = (s16)(y < -32768.0f ? -32768.0f : y > 32767.0f ? 32767.0f : y);
		}
	}
}

// Freeverb-lite: 4 damped combs + 2 series allpasses per channel
static void audioProcessReverb(s16 *buf, u32 frames)
{
	const f32 wet = audioReverbWet;
	const f32 feedback = 0.88f;
	const f32 damp = 0.4f;
	u32 i;
	s32 ch, c;

	for (i = 0; i < frames; i++) {
		for (ch = 0; ch < 2; ch++) {
			const f32 x = (f32)buf[i * 2 + ch];
			const f32 in = x * 0.25f;
			f32 acc = 0.0f;
			f32 y;

			for (c = 0; c < REVERB_NCOMB; c++) {
				f32 *slot = &audioReverbComb[ch][c][audioReverbCombIdx[ch][c]];
				const f32 out = *slot;

				audioReverbCombFilt[ch][c] = out * (1.0f - damp) + audioReverbCombFilt[ch][c] * damp;
				*slot = in + audioReverbCombFilt[ch][c] * feedback;
				if (++audioReverbCombIdx[ch][c] >= audioReverbCombLen[ch][c]) {
					audioReverbCombIdx[ch][c] = 0;
				}
				acc += out;
			}

			for (c = 0; c < REVERB_NAP; c++) {
				f32 *slot = &audioReverbAp[ch][c][audioReverbApIdx[ch][c]];
				const f32 bufout = *slot;

				*slot = acc + bufout * 0.5f;
				acc = bufout - acc;
				if (++audioReverbApIdx[ch][c] >= audioReverbApLen[ch][c]) {
					audioReverbApIdx[ch][c] = 0;
				}
			}

			y = x * (1.0f - wet * 0.4f) + acc * wet * 1.5f;
			buf[i * 2 + ch] = (s16)(y < -32768.0f ? -32768.0f : y > 32767.0f ? 32767.0f : y);
		}
	}
}
#endif

#ifndef DEDICATED_SERVER
// Whole-file MP3 decode via the bundled minimp3 (same decoder mixer.c uses
// for the game's own MP3 assets). Returns malloc'd (SDL_malloc) s16 PCM +
// its source spec. Capped so a runaway file can't eat unbounded memory
// (~64MB PCM; external one-shots are ring-tone sized).
static s32 audioLoadMp3(const char *path, Uint8 **outdata, int *outlen, SDL_AudioSpec *outspec)
{
	size_t fsize = 0;
	Uint8 *fdata = (Uint8 *)SDL_LoadFile(path, &fsize);
	mp3dec_t dec;
	mp3dec_frame_info_t info;
	mp3d_sample_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
	s16 *buf = NULL;
	size_t cap = 0, len = 0, pos = 0; // len in s16 samples
	const size_t maxsamples = 32u * 1024u * 1024u;
	int hz = 0, channels = 0;

	if (!fdata || fsize == 0) {
		SDL_free(fdata);
		return 0;
	}

	mp3dec_init(&dec);

	while (pos < fsize && len < maxsamples) {
		const int samples = mp3dec_decode_frame(&dec, fdata + pos, (int)(fsize - pos), pcm, &info);

		if (info.frame_bytes <= 0) {
			break; // no more recognisable frames
		}
		pos += (size_t)info.frame_bytes;

		if (samples > 0) {
			const size_t add = (size_t)samples * (size_t)info.channels;

			if (hz == 0) {
				hz = info.hz;
				channels = info.channels;
			}

			if (len + add > cap) {
				size_t newcap = cap ? cap * 2 : 65536;
				s16 *newbuf;
				while (newcap < len + add) {
					newcap *= 2;
				}
				newbuf = (s16 *)SDL_realloc(buf, newcap * sizeof(s16));
				if (!newbuf) {
					break;
				}
				buf = newbuf;
				cap = newcap;
			}

			SDL_memcpy(buf + len, pcm, add * sizeof(s16));
			len += add;
		}
	}

	SDL_free(fdata);

	if (!buf || len == 0 || hz == 0) {
		SDL_free(buf);
		return 0;
	}

	SDL_zero(*outspec);
	outspec->format = SDL_AUDIO_S16;
	outspec->channels = channels;
	outspec->freq = hz;
	*outdata = (Uint8 *)buf;
	*outlen = (int)(len * sizeof(s16));
	return 1;
}
#endif

s32 audioPlayExternal(const char *path, s32 loop, s32 followMusic)
{
#ifdef DEDICATED_SERVER
	(void)loop;
	(void)followMusic;
	return 0;
#else
	SDL_AudioSpec srcspec;
	SDL_AudioSpec dstspec;
	Uint8 *srcdata = NULL;
	Uint32 wavlen = 0;
	int srclen = 0;
	Uint8 *conv = NULL;
	int convlen = 0;

	if (!stream || !path || !path[0]) {
		return 0;
	}

	// Try WAV first, then MP3 — by content, not extension, so either format
	// works whatever the file is called.
	if (SDL_LoadWAV(path, &srcspec, &srcdata, &wavlen)) {
		srclen = (int)wavlen;
	} else if (!audioLoadMp3(path, &srcdata, &srclen, &srcspec)) {
		sysLogPrintf(LOG_WARNING, "audio: can't load '%s' as WAV or MP3: %s", path, SDL_GetError());
		return 0;
	}

	SDL_zero(dstspec);
	dstspec.format = SDL_AUDIO_S16;
	dstspec.channels = 2;
	dstspec.freq = 22020; // must match the device stream opened in audioInit

	if (!SDL_ConvertAudioSamples(&srcspec, srcdata, srclen, &dstspec, &conv, &convlen)) {
		sysLogPrintf(LOG_WARNING, "audio: can't convert '%s': %s", path, SDL_GetError());
		SDL_free(srcdata);
		return 0;
	}

	SDL_free(srcdata);

	if (extSound) {
		SDL_free(extSound);
	}

	extSound = conv;
	extSoundLen = (u32)convlen;
	extSoundPos = 0;
	extLoop = loop ? 1 : 0;
	extFollowMusic = followMusic ? 1 : 0;
	return 1;
#endif
}

// Stop the external sound immediately (pd.stop_file / call answered).
void audioStopExternal(void)
{
#ifndef DEDICATED_SERVER
	extLoop = 0;
	extFollowMusic = 0;
	if (extSound) {
		SDL_free(extSound);
		extSound = NULL;
		extSoundLen = extSoundPos = 0;
	}
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

			// Chaos mute / external one-shot / effect chain: all need a
			// mutable copy of the outgoing buffer (the mixer owns nextBuf).
			// Push cadence and sizes are unchanged so the frame pacing that
			// reads the queued-bytes count stays identical.
			const s32 crushing = audioCrushStep > 1 || audioCrushBits < 16;
			const s32 pitching = audioPitchRate != 1.0f;
			const s32 fxactive = crushing || pitching || audioRadioOn || audioRevOn || audioReverbWet > 0.0f;

			if (audioMuted || fxactive || (extSound && extSoundPos < extSoundLen)) {
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
					// muted mutes it too. A followMusic track (Silo.mp3) also
					// pauses with the game: while paused we skip the mix AND the
					// position advance below, so it holds and resumes from the menu.
					if (!audioMuted && extSound && extSoundPos < extSoundLen
							&& !(extFollowMusic && lvIsPaused())) {
						const s16 *ext = (const s16 *)(extSound + extSoundPos);
						u32 bytes = extSoundLen - extSoundPos;
						u32 i, n;
						// 8.8 fixed-point gain: 256 = unity. When the track opts
						// into following the in-game music volume, scale by the
						// current music slider (0..0x5000) so it ducks/mutes with
						// the player's music setting instead of blasting at full.
						s32 gain256 = 256;

						if (extFollowMusic) {
							s32 mv = (s32)optionsGetMusicVolume();
							gain256 = (mv * 256) / AUDIO_MUSICVOL_MAX;
							if (gain256 > 256) gain256 = 256;
							if (gain256 < 0) gain256 = 0;
						}

						if (bytes > nextSize) {
							bytes = nextSize;
						}
						n = bytes / sizeof(s16);

						for (i = 0; i < n; i++) {
							s32 s = (s32)mixBuf[i] + (((s32)ext[i] * gain256) >> 8);
							if (s > 32767) s = 32767;
							if (s < -32768) s = -32768;
							mixBuf[i] = (s16)s;
						}

						extSoundPos += bytes;
					}

					// effect chain (after the ext mix so one-shots are
					// processed too): reverse -> pitch -> radio -> reverb ->
					// crush. Order puts the tonal effects on the already
					// time-warped signal and the crunch last.
					if (!audioMuted && fxactive) {
						const u32 frames = nextSize / (2 * sizeof(s16));

						if (audioRevOn) {
							audioProcessReverse(mixBuf, frames);
						}
						if (pitching) {
							audioProcessPitch(mixBuf, frames);
						}
						if (audioRadioOn) {
							audioProcessRadio(mixBuf, frames);
						}
						if (audioReverbWet > 0.0f) {
							audioProcessReverb(mixBuf, frames);
						}

						// bitcrush: the mask sign-extends through the int
						// promotion, so negative samples quantize the same
						// as positive.
						if (crushing) {
							const s16 mask = (s16)(0xffffu << (16 - audioCrushBits));
							u32 i;

							for (i = 0; i < frames; i++) {
								if (audioCrushPhase == 0) {
									audioCrushL = mixBuf[i * 2 + 0] & mask;
									audioCrushR = mixBuf[i * 2 + 1] & mask;
								}
								if (++audioCrushPhase >= (u32)audioCrushStep) {
									audioCrushPhase = 0;
								}
								mixBuf[i * 2 + 0] = audioCrushL;
								mixBuf[i * 2 + 1] = audioCrushR;
							}
						}
					}

					out = mixBuf;
				}
			}

			if (extSound && extSoundPos >= extSoundLen) {
				if (extLoop) {
					extSoundPos = 0; // seamless-ish loop: rewind, keep the buffer
				} else {
					SDL_free(extSound);
					extSound = NULL;
					extSoundLen = extSoundPos = 0;
				}
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
