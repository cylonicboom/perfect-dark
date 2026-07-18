#ifndef _IN_AUDIO_H
#define _IN_AUDIO_H

#include <PR/ultratypes.h>

s32 audioInit(void);
s32 audioGetBytesBuffered(void);
s32 audioGetSamplesBuffered(void);
void audioSetNextBuffer(const s16 *buf, u32 len);
void audioEndFrame(void);
// Chaos audio extras (docs/PORT_CHAOS.md), all applied at the single push
// point in audioEndFrame (chain: reverse -> pitch -> radio -> reverb ->
// crush; mute wins over everything):
// - audioSetMuted: master mute (pd.mute)
// - audioPlayExternal: one-shot WAV/MP3 mixed into the stream (pd.play_file)
// - audioSetCrush: sample-and-hold every `step`th frame at `bits` depth;
//   1, 16 = off (pd.audio_crush)
// - audioSetRadio: AM-radio bandpass + overdrive (pd.audio_radio)
// - audioSetReverb: Freeverb-lite cathedral wash, wet 0..1, 0 = off
//   (pd.audio_reverb)
// - audioSetReverse: granular time reversal, ~0.74s chunks (pd.audio_reverse)
// - audioSetPitch: granular pitch shift at constant tempo, rate 0.25..4,
//   1 = off (pd.audio_pitch)
void audioSetMuted(s32 on);
s32 audioPlayExternal(const char *path, s32 loop);
void audioStopExternal(void);
void audioSetCrush(s32 step, s32 bits);
void audioSetRadio(s32 on);
void audioSetReverb(f32 wet);
void audioSetReverse(s32 on);
void audioSetPitch(f32 rate);

#endif
