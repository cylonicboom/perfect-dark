#ifndef _IN_AUDIO_H
#define _IN_AUDIO_H

#include <PR/ultratypes.h>

s32 audioInit(void);
s32 audioGetBytesBuffered(void);
s32 audioGetSamplesBuffered(void);
void audioSetNextBuffer(const s16 *buf, u32 len);
void audioEndFrame(void);
// Chaos audio extras (docs/PORT_CHAOS.md): master mute (pd.mute), one-shot
// external WAV playback mixed into the device stream (pd.play_file), and the
// bitcrush (pd.audio_crush): sample-and-hold every `step`th output frame at
// `bits` bit depth. step 1 / bits 16 = off.
void audioSetMuted(s32 on);
s32 audioPlayExternal(const char *path);
void audioSetCrush(s32 step, s32 bits);

#endif
