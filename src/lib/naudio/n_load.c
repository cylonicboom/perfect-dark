#include "n_synthInternals.h"
#include <os.h>
#include <R4300.h>

#define ADPCMFBYTES      9
#define LFSAMPLES        4

Acmd *_decodeChunk(Acmd *ptr, N_PVoice *f, s32 tsam, s32 nbytes, s16 outp, s16 inp, u32 flags);

#ifndef PLATFORM_N64
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

/**
 * n_alRaw16Pull — the raw-PCM sample path Rare stripped out of naudio,
 * restored for the ADPCM-predecode feature (snd.c retypes wavetables to
 * AL_RAW16_WAVE; without this every voice fed raw PCM through the ADPCM
 * frame parser = loud corruption). Mirrors n_alAdpcmPull's contract: emit
 * alist commands leaving outCount s16 samples readable at *outp, advance
 * dc_sample/dc_memin, count down finite loops (-1 = forever, same
 * small-loop imprecision as the ADPCM path) and zero-fill past the end.
 * Raw data needs no book, no decoder-state block and no frame alignment,
 * so *outp stays where the caller pointed it and loop wraps are plain
 * sequential loads. dc_lastsam stays 0 by construction.
 */
// Diagnostics for the sustained-note hunt (/sndpool raw16 line, net.c):
// wraps = loop restarts executed, ends = one-shot silence fills emitted.
// A sustained note cutting short with wraps NOT advancing means the loop
// never engages (dc_loop bad); wraps advancing but audio still cutting
// means the post-wrap data is wrong (memin/base bad).
s32 g_SndRaw16Wraps = 0;
s32 g_SndRaw16Ends = 0;

static Acmd *n_alRaw16Pull(N_PVoice *f, s16 *outp, s32 outCount, Acmd *p)
{
	Acmd *ptr = p;
	s32 pos;
	s32 remaining = outCount;

	// Start the data one ADPCM-frame (16 samples) into the caller's DMEM
	// window, exactly where the ADPCM path's decoded stream starts after its
	// state block, and advance *outp to match (the lastsam==0 adjustment).
	// This is NOT cosmetic: the resampler's history splice writes 8 samples
	// BEFORE its input pointer (aResampleImpl's `in - 8`), and with data at
	// the window base that write lands OUTSIDE the window — clobbering
	// whatever DMEM precedes it (adjacent voice/bus data; heard as sustained
	// looped notes cutting short when this path first shipped without the
	// offset).
	*outp += ADPCMFSIZE << 1;
	pos = *outp;

	while (remaining > 0) {
		s32 avail;
		s32 n;
		s32 looping;

		// Wrap FIRST, at the top of every segment — not only mid-pull. The
		// original tail-only wrap (gated on remaining > 0) missed the case
		// where a pull ends with dc_sample landing EXACTLY on loop.end: the
		// next pull then saw sample < end as false, played the few samples
		// of post-loop tail and went silent — a sustained note dying at the
		// loop seam whenever the pull boundary aligned with it (the
		// intermittent theremin/strings cutoff, diagnosed via the bank loop
		// log: every loop was well-formed, so the fault had to be here).
		if (f->dc_loop.count != 0 && f->dc_sample >= (s32)f->dc_loop.end) {
			g_SndRaw16Wraps++;
			if (f->dc_loop.count != -1) {
				f->dc_loop.count--;
			}
			f->dc_sample = f->dc_loop.start;
			f->dc_memin = (intptr_t)f->dc_table->base + ((s32)f->dc_loop.start << 1);
		}

		looping = f->dc_loop.count != 0 && f->dc_sample < (s32)f->dc_loop.end;

		if (looping) {
			avail = f->dc_loop.end - f->dc_sample;
		} else {
			avail = (s32)(f->dc_table->len >> 1) - f->dc_sample;
		}

		if (avail <= 0) {
			// ran off the end (one-shot finished): the consumer still reads
			// outCount samples, so the tail must be silence, not garbage
			g_SndRaw16Ends++;
#ifndef PLATFORM_N64
			// TRIPWIRE: with the top-of-segment wrap, a voice with a live
			// loop (count != 0) can never reach this fill — looping is
			// always re-established before avail is computed. If this line
			// ever prints, that proof is wrong and the residual sustained-
			// note cutoff is still a decoder escape; the state dump says how.
			// Silence here otherwise means a residual cut is EXTERNAL to the
			// decoder (voice steal / envelope) — check /sndpool steals.
			if (f->dc_loop.count != 0) {
				extern void sysLogPrintf(s32 level, const char *fmt, ...);
				sysLogPrintf(1, "raw16: LOOP ESCAPE sample=%d start=%u end=%u count=%d len2=%d",
						f->dc_sample, f->dc_loop.start, f->dc_loop.end,
						(s32)f->dc_loop.count, (s32)(f->dc_table->len >> 1));
			}
#endif
			aClearBuffer(ptr++, pos, remaining << 1);
			return ptr;
		}

		n = MIN(remaining, avail);
		// same round-up-to-8 the ADPCM loads use; the predecode buffers are
		// over-allocated (snd.c) so the tail overread stays in bounds
		n_aLoadBuffer(ptr++, (n << 1) + 8 - ((n << 1) & 0x7), pos, K0_TO_PHYS(f->dc_memin));
		pos += n << 1;
		remaining -= n;
		f->dc_sample += n;
		f->dc_memin += n << 1;
	}

	return ptr;
}
#endif

Acmd *n_alAdpcmPull(N_PVoice *filter, s16 *outp, s32 outCount, Acmd *p)
{
	Acmd *ptr = p;
	s16 inp;
	s32 tsam;
	s32 nframes;
	s32 nbytes;
	s32 overFlow;
	s32 startZero;
	s32 nOver;
	s32 nSam;
	s32 op;
	s32 nLeft;
	s32 bEnd;
	s32 decoded = 0;
	s32 looped = 0;

	N_PVoice *f = filter;

	if (outCount == 0) {
		return ptr;
	}

#ifndef PLATFORM_N64
	// Predecoded (raw PCM16) tables take the restored raw path; the ADPCM
	// book/frame machinery below would parse their PCM as ADPCM frames.
	if (f->dc_table != NULL && f->dc_table->type == AL_RAW16_WAVE) {
		return n_alRaw16Pull(f, outp, outCount, p);
	}
#endif

	inp = N_AL_DECODER_IN;

	aLoadADPCM(ptr++, f->dc_bookSize, K0_TO_PHYS(f->dc_table->waveInfo.adpcmWave.book->book));

	looped = (outCount + f->dc_sample > f->dc_loop.end) && (f->dc_loop.count != 0);

	if (looped) {
		nSam = f->dc_loop.end - f->dc_sample;
	} else {
		nSam = outCount;
	}

	if (f->dc_lastsam) {
		nLeft = ADPCMFSIZE - f->dc_lastsam;
	} else {
		nLeft = 0;
	}

	tsam = nSam - nLeft;

	if (tsam<0) {
		tsam = 0;
	}

	nframes = (tsam + ADPCMFSIZE - 1) >> LFSAMPLES;
	nbytes =  nframes*ADPCMFBYTES;

	if (looped) {
		ptr = _decodeChunk(ptr, f, tsam, nbytes, *outp, inp, f->dc_first);

		/*
		 * Fix up output pointer, which will be used as the input pointer
		 * by the following module.
		 */
		if (f->dc_lastsam) {
			*outp += (f->dc_lastsam<<1);
		} else {
			*outp += (ADPCMFSIZE<<1);
		}

		/*
		 * Now fix up state info to reflect the loop start point
		 */
		f->dc_lastsam = f->dc_loop.start &0xf;
		f->dc_memin = (intptr_t) f->dc_table->base + ADPCMFBYTES * ((s32) (f->dc_loop.start>>LFSAMPLES) + 1);
		f->dc_sample = f->dc_loop.start;

		bEnd = *outp;

		while (outCount > nSam) {
			outCount -= nSam;

			/*
			 * Put next one after the end of the last lot - on the
			 * frame boundary (32 byte) after the end.
			 */
			op = (bEnd + ((nframes+1)<<(LFSAMPLES+1)) + 16) & ~0x1f;

			/*
			 * The actual end of data
			 */
			bEnd += (nSam<<1);

			/*
			 * -1 is loop forever - the loop count is not exact now
			 * for small loops!
			 */
			if (f->dc_loop.count != -1 && f->dc_loop.count != 0) {
				f->dc_loop.count--;
			}

			/*
			 * What's left to compute.
			 */
			nSam = MIN(outCount, f->dc_loop.end - f->dc_loop.start);
			tsam = nSam - ADPCMFSIZE + f->dc_lastsam;

			if (tsam < 0) {
				tsam = 0;
			}

			nframes = (tsam+ADPCMFSIZE - 1) >> LFSAMPLES;
			nbytes =  nframes*ADPCMFBYTES;
			ptr = _decodeChunk(ptr, f, tsam, nbytes, op, inp, f->dc_first | A_LOOP);

			/*
			 * Merge the two sections in DMEM.
			 */
			aDMEMMove(ptr++, op + (f->dc_lastsam << 1), bEnd, nSam << 1);
		}

		f->dc_lastsam = (outCount + f->dc_lastsam) & 0xf;
		f->dc_sample += outCount;
		f->dc_memin += ADPCMFBYTES*nframes;

		return ptr;
	}

	/*
	 * The unlooped case, which is executed most of the time
	 */

	nSam = nframes << LFSAMPLES;

	/*
	 * overFlow is the number of bytes past the end
	 * of the bitstream I try to generate
	 */
	overFlow = f->dc_memin + nbytes - ((intptr_t) f->dc_table->base + f->dc_table->len);

	if (overFlow < 0) {
		overFlow = 0;
	}

	nOver = (overFlow / ADPCMFBYTES) << LFSAMPLES;

	if (nOver > nSam + nLeft) {
		nOver = nSam + nLeft;
	}

	nbytes -= overFlow;

	if (nOver - (nOver & 0xf) < outCount) {
		decoded = 1;
		ptr = _decodeChunk(ptr, f, nSam - nOver, nbytes, *outp, inp, f->dc_first);

		if (f->dc_lastsam) {
			*outp += f->dc_lastsam << 1;
		} else {
			*outp += ADPCMFSIZE << 1;
		}

		f->dc_lastsam = (outCount + f->dc_lastsam) & 0xf;
		f->dc_sample += outCount;
		f->dc_memin += ADPCMFBYTES * nframes;
	} else {
		f->dc_lastsam = 0;
		f->dc_memin += ADPCMFBYTES * nframes;
	}

	/*
	 * Put zeros in if necessary
	 */
	if (nOver) {
		f->dc_lastsam = 0;

		if (decoded) {
			startZero = (nLeft + nSam - nOver) << 1;
		} else {
			startZero = 0;
		}

		aClearBuffer(ptr++, startZero + *outp, nOver << 1);
	}

	return ptr;
}

s32 n_alLoadParam(N_PVoice *filter, s32 paramID, void *param)
{
	N_PVoice *a = filter;

	switch (paramID) {
	case (AL_FILTER_SET_WAVETABLE):
		a->dc_table = (ALWaveTable *) param;
		a->dc_memin = (intptr_t) a->dc_table->base;
		a->dc_sample = 0;

		switch (a->dc_table->type) {
		case (AL_ADPCM_WAVE):
			a->dc_table->len = ADPCMFBYTES * ((s32) (a->dc_table->len/ADPCMFBYTES));

			a->dc_bookSize = 2*a->dc_table->waveInfo.adpcmWave.book->order*
				a->dc_table->waveInfo.adpcmWave.book->npredictors*ADPCMVSIZE;

			if (a->dc_table->waveInfo.adpcmWave.loop) {
				a->dc_loop.start = a->dc_table->waveInfo.adpcmWave.loop->start;
				a->dc_loop.end = a->dc_table->waveInfo.adpcmWave.loop->end;
				a->dc_loop.count = a->dc_table->waveInfo.adpcmWave.loop->count;

				bcopy(a->dc_table->waveInfo.adpcmWave.loop->state, a->dc_lstate, sizeof(ADPCM_STATE));
			} else {
				a->dc_loop.start = a->dc_loop.end = a->dc_loop.count = 0;
			}
			break;
		case (AL_RAW16_WAVE):
			if (a->dc_table->waveInfo.rawWave.loop) {
				a->dc_loop.start = a->dc_table->waveInfo.rawWave.loop->start;
				a->dc_loop.end = a->dc_table->waveInfo.rawWave.loop->end;
				a->dc_loop.count = a->dc_table->waveInfo.rawWave.loop->count;
			} else {
				a->dc_loop.start = a->dc_loop.end = a->dc_loop.count = 0;
			}
			break;
		default:
			break;
		}
		break;
	case (AL_FILTER_RESET):
		a->dc_lastsam = 0;
		a->dc_first   = 1;
		a->dc_sample = 0;

		/* sct 2/14/96 - Check table since it is initialized to null and */
		/* Get loop info according to table type. */
		if (a->dc_table) {
			a->dc_memin  = (intptr_t) a->dc_table->base;

			if (a->dc_table->type == AL_ADPCM_WAVE) {
				if (a->dc_table->waveInfo.adpcmWave.loop) {
					a->dc_loop.count = a->dc_table->waveInfo.adpcmWave.loop->count;
				}
			} else if (a->dc_table->type == AL_RAW16_WAVE) {
				if (a->dc_table->waveInfo.rawWave.loop) {
					a->dc_loop.count = a->dc_table->waveInfo.rawWave.loop->count;
				}
			}
		}
		break;
	default:
		break;
	}

	return 0;
}

Acmd *_decodeChunk(Acmd *ptr, N_PVoice *f, s32 tsam, s32 nbytes, s16 outp, s16 inp, u32 flags)
{
	intptr_t dramAlign, dramLoc;

	if (nbytes > 0) {
		dramLoc = (f->dc_dma)(f->dc_memin, nbytes, f->dc_dmaState);
		/*
		 * Make sure enough is loaded into DMEM to take care
		 * of 8 byte alignment
		 */
		dramAlign = dramLoc & 0x7;
		nbytes += dramAlign;

		n_aLoadBuffer(ptr++, nbytes + 8 - (nbytes & 0x7), inp, dramLoc - dramAlign);
	} else {
		dramAlign = 0;
	}

	if (flags & A_LOOP) {
		aSetLoop(ptr++, K0_TO_PHYS(f->dc_lstate));
	}

	n_aADPCMdec(ptr++, K0_TO_PHYS(f->dc_state), flags, tsam << 1, dramAlign, outp);

	f->dc_first = 0;

	return ptr;
}
