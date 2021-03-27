#include <ultra64.h>
#include "constants.h"
#include "bss.h"
#include "lib/lib_38d30.h"
#include "lib/lib_3c4d0.h"
#include "data.h"
#include "types.h"

u8 __getTrackByte(ALCSeq *seq, u32 track);
u32 __readVarLen(ALCSeq *seq,u32 track);

GLOBAL_ASM(
glabel alCSeqNew
/*    38d30:	27bdffd8 */ 	addiu	$sp,$sp,-40
/*    38d34:	afbf0014 */ 	sw	$ra,0x14($sp)
/*    38d38:	afa40028 */ 	sw	$a0,0x28($sp)
/*    38d3c:	afa5002c */ 	sw	$a1,0x2c($sp)
/*    38d40:	8fae002c */ 	lw	$t6,0x2c($sp)
/*    38d44:	8faf0028 */ 	lw	$t7,0x28($sp)
/*    38d48:	adee0000 */ 	sw	$t6,0x0($t7)
/*    38d4c:	8fb80028 */ 	lw	$t8,0x28($sp)
/*    38d50:	af000004 */ 	sw	$zero,0x4($t8)
/*    38d54:	8fb90028 */ 	lw	$t9,0x28($sp)
/*    38d58:	af200010 */ 	sw	$zero,0x10($t9)
/*    38d5c:	8fa80028 */ 	lw	$t0,0x28($sp)
/*    38d60:	ad00000c */ 	sw	$zero,0xc($t0)
/*    38d64:	8faa0028 */ 	lw	$t2,0x28($sp)
/*    38d68:	24090001 */ 	addiu	$t1,$zero,0x1
/*    38d6c:	ad490014 */ 	sw	$t1,0x14($t2)
/*    38d70:	afa00024 */ 	sw	$zero,0x24($sp)
.L00038d74:
/*    38d74:	8fab0028 */ 	lw	$t3,0x28($sp)
/*    38d78:	8fac0024 */ 	lw	$t4,0x24($sp)
/*    38d7c:	016c6821 */ 	addu	$t5,$t3,$t4
/*    38d80:	a1a000a8 */ 	sb	$zero,0xa8($t5)
/*    38d84:	8faf0024 */ 	lw	$t7,0x24($sp)
/*    38d88:	8fae0028 */ 	lw	$t6,0x28($sp)
/*    38d8c:	000fc080 */ 	sll	$t8,$t7,0x2
/*    38d90:	01d8c821 */ 	addu	$t9,$t6,$t8
/*    38d94:	af200058 */ 	sw	$zero,0x58($t9)
/*    38d98:	8fa80028 */ 	lw	$t0,0x28($sp)
/*    38d9c:	8fa90024 */ 	lw	$t1,0x24($sp)
/*    38da0:	01095021 */ 	addu	$t2,$t0,$t1
/*    38da4:	a1400098 */ 	sb	$zero,0x98($t2)
/*    38da8:	8fab0028 */ 	lw	$t3,0x28($sp)
/*    38dac:	8fad0024 */ 	lw	$t5,0x24($sp)
/*    38db0:	8d6c0000 */ 	lw	$t4,0x0($t3)
/*    38db4:	000d7880 */ 	sll	$t7,$t5,0x2
/*    38db8:	018f7021 */ 	addu	$t6,$t4,$t7
/*    38dbc:	8dd80000 */ 	lw	$t8,0x0($t6)
/*    38dc0:	afb80020 */ 	sw	$t8,0x20($sp)
/*    38dc4:	8fb90020 */ 	lw	$t9,0x20($sp)
/*    38dc8:	1320001b */ 	beqz	$t9,.L00038e38
/*    38dcc:	00000000 */ 	nop
/*    38dd0:	8fa80024 */ 	lw	$t0,0x24($sp)
/*    38dd4:	24090001 */ 	addiu	$t1,$zero,0x1
/*    38dd8:	01095004 */ 	sllv	$t2,$t1,$t0
/*    38ddc:	afaa001c */ 	sw	$t2,0x1c($sp)
/*    38de0:	8fab0028 */ 	lw	$t3,0x28($sp)
/*    38de4:	8fac001c */ 	lw	$t4,0x1c($sp)
/*    38de8:	8d6d0004 */ 	lw	$t5,0x4($t3)
/*    38dec:	01ac7825 */ 	or	$t7,$t5,$t4
/*    38df0:	ad6f0004 */ 	sw	$t7,0x4($t3)
/*    38df4:	8fa80024 */ 	lw	$t0,0x24($sp)
/*    38df8:	8fae002c */ 	lw	$t6,0x2c($sp)
/*    38dfc:	8fb80020 */ 	lw	$t8,0x20($sp)
/*    38e00:	8fa90028 */ 	lw	$t1,0x28($sp)
/*    38e04:	00085080 */ 	sll	$t2,$t0,0x2
/*    38e08:	01d8c821 */ 	addu	$t9,$t6,$t8
/*    38e0c:	012a6821 */ 	addu	$t5,$t1,$t2
/*    38e10:	adb90018 */ 	sw	$t9,0x18($t5)
/*    38e14:	8fa40028 */ 	lw	$a0,0x28($sp)
/*    38e18:	0c00e6d3 */ 	jal	__readVarLen
/*    38e1c:	8fa50024 */ 	lw	$a1,0x24($sp)
/*    38e20:	8faf0024 */ 	lw	$t7,0x24($sp)
/*    38e24:	8fac0028 */ 	lw	$t4,0x28($sp)
/*    38e28:	000f5880 */ 	sll	$t3,$t7,0x2
/*    38e2c:	018b7021 */ 	addu	$t6,$t4,$t3
/*    38e30:	10000006 */ 	b	.L00038e4c
/*    38e34:	adc200b8 */ 	sw	$v0,0xb8($t6)
.L00038e38:
/*    38e38:	8fa80024 */ 	lw	$t0,0x24($sp)
/*    38e3c:	8fb80028 */ 	lw	$t8,0x28($sp)
/*    38e40:	00084880 */ 	sll	$t1,$t0,0x2
/*    38e44:	03095021 */ 	addu	$t2,$t8,$t1
/*    38e48:	ad400018 */ 	sw	$zero,0x18($t2)
.L00038e4c:
/*    38e4c:	8fb90024 */ 	lw	$t9,0x24($sp)
/*    38e50:	272d0001 */ 	addiu	$t5,$t9,0x1
/*    38e54:	2da10010 */ 	sltiu	$at,$t5,0x10
/*    38e58:	1420ffc6 */ 	bnez	$at,.L00038d74
/*    38e5c:	afad0024 */ 	sw	$t5,0x24($sp)
/*    38e60:	8faf0028 */ 	lw	$t7,0x28($sp)
/*    38e64:	3c013f80 */ 	lui	$at,0x3f80
/*    38e68:	44812000 */ 	mtc1	$at,$f4
/*    38e6c:	8dec0000 */ 	lw	$t4,0x0($t7)
/*    38e70:	8d8b0040 */ 	lw	$t3,0x40($t4)
/*    38e74:	448b3000 */ 	mtc1	$t3,$f6
/*    38e78:	05610005 */ 	bgez	$t3,.L00038e90
/*    38e7c:	46803220 */ 	cvt.s.w	$f8,$f6
/*    38e80:	3c014f80 */ 	lui	$at,0x4f80
/*    38e84:	44815000 */ 	mtc1	$at,$f10
/*    38e88:	00000000 */ 	nop
/*    38e8c:	460a4200 */ 	add.s	$f8,$f8,$f10
.L00038e90:
/*    38e90:	46082403 */ 	div.s	$f16,$f4,$f8
/*    38e94:	e5f00008 */ 	swc1	$f16,0x8($t7)
/*    38e98:	10000001 */ 	b	.L00038ea0
/*    38e9c:	00000000 */ 	nop
.L00038ea0:
/*    38ea0:	8fbf0014 */ 	lw	$ra,0x14($sp)
/*    38ea4:	27bd0028 */ 	addiu	$sp,$sp,0x28
/*    38ea8:	03e00008 */ 	jr	$ra
/*    38eac:	00000000 */ 	nop
);

GLOBAL_ASM(
glabel alCSeqNextEvent
/*    38eb0:	27bdffc8 */ 	addiu	$sp,$sp,-56
/*    38eb4:	afbf001c */ 	sw	$ra,0x1c($sp)
/*    38eb8:	afa40038 */ 	sw	$a0,0x38($sp)
/*    38ebc:	afa5003c */ 	sw	$a1,0x3c($sp)
/*    38ec0:	afa60040 */ 	sw	$a2,0x40($sp)
/*    38ec4:	afb00018 */ 	sw	$s0,0x18($sp)
/*    38ec8:	240effff */ 	addiu	$t6,$zero,-1
/*    38ecc:	afae0030 */ 	sw	$t6,0x30($sp)
/*    38ed0:	8faf0038 */ 	lw	$t7,0x38($sp)
/*    38ed4:	8df80010 */ 	lw	$t8,0x10($t7)
/*    38ed8:	afb80028 */ 	sw	$t8,0x28($sp)
/*    38edc:	afa00034 */ 	sw	$zero,0x34($sp)
.L00038ee0:
/*    38ee0:	8fb90038 */ 	lw	$t9,0x38($sp)
/*    38ee4:	8fa90034 */ 	lw	$t1,0x34($sp)
/*    38ee8:	8f280004 */ 	lw	$t0,0x4($t9)
/*    38eec:	01285006 */ 	srlv	$t2,$t0,$t1
/*    38ef0:	314b0001 */ 	andi	$t3,$t2,0x1
/*    38ef4:	1160001e */ 	beqz	$t3,.L00038f70
/*    38ef8:	00000000 */ 	nop
/*    38efc:	8fac0038 */ 	lw	$t4,0x38($sp)
/*    38f00:	8d8d0014 */ 	lw	$t5,0x14($t4)
/*    38f04:	11a00009 */ 	beqz	$t5,.L00038f2c
/*    38f08:	00000000 */ 	nop
/*    38f0c:	8faf0034 */ 	lw	$t7,0x34($sp)
/*    38f10:	8fae0038 */ 	lw	$t6,0x38($sp)
/*    38f14:	8fa90028 */ 	lw	$t1,0x28($sp)
/*    38f18:	000fc080 */ 	sll	$t8,$t7,0x2
/*    38f1c:	01d8c821 */ 	addu	$t9,$t6,$t8
/*    38f20:	8f2800b8 */ 	lw	$t0,0xb8($t9)
/*    38f24:	01095023 */ 	subu	$t2,$t0,$t1
/*    38f28:	af2a00b8 */ 	sw	$t2,0xb8($t9)
.L00038f2c:
/*    38f2c:	8fac0034 */ 	lw	$t4,0x34($sp)
/*    38f30:	8fab0038 */ 	lw	$t3,0x38($sp)
/*    38f34:	8fb80030 */ 	lw	$t8,0x30($sp)
/*    38f38:	000c6880 */ 	sll	$t5,$t4,0x2
/*    38f3c:	016d7821 */ 	addu	$t7,$t3,$t5
/*    38f40:	8dee00b8 */ 	lw	$t6,0xb8($t7)
/*    38f44:	01d8082b */ 	sltu	$at,$t6,$t8
/*    38f48:	10200009 */ 	beqz	$at,.L00038f70
/*    38f4c:	00000000 */ 	nop
/*    38f50:	8fa90034 */ 	lw	$t1,0x34($sp)
/*    38f54:	8fa80038 */ 	lw	$t0,0x38($sp)
/*    38f58:	00095080 */ 	sll	$t2,$t1,0x2
/*    38f5c:	010ac821 */ 	addu	$t9,$t0,$t2
/*    38f60:	8f2c00b8 */ 	lw	$t4,0xb8($t9)
/*    38f64:	afac0030 */ 	sw	$t4,0x30($sp)
/*    38f68:	8fab0034 */ 	lw	$t3,0x34($sp)
/*    38f6c:	afab002c */ 	sw	$t3,0x2c($sp)
.L00038f70:
/*    38f70:	8fad0034 */ 	lw	$t5,0x34($sp)
/*    38f74:	25af0001 */ 	addiu	$t7,$t5,0x1
/*    38f78:	2de10010 */ 	sltiu	$at,$t7,0x10
/*    38f7c:	1420ffd8 */ 	bnez	$at,.L00038ee0
/*    38f80:	afaf0034 */ 	sw	$t7,0x34($sp)
/*    38f84:	8fa40038 */ 	lw	$a0,0x38($sp)
/*    38f88:	8fa5002c */ 	lw	$a1,0x2c($sp)
/*    38f8c:	8fa6003c */ 	lw	$a2,0x3c($sp)
/*    38f90:	0c00e40b */ 	jal	__alCSeqGetTrackEvent
/*    38f94:	8fa70040 */ 	lw	$a3,0x40($sp)
/*    38f98:	8fae0030 */ 	lw	$t6,0x30($sp)
/*    38f9c:	8fb8003c */ 	lw	$t8,0x3c($sp)
/*    38fa0:	af0e0004 */ 	sw	$t6,0x4($t8)
/*    38fa4:	8fa90038 */ 	lw	$t1,0x38($sp)
/*    38fa8:	8faa0030 */ 	lw	$t2,0x30($sp)
/*    38fac:	8d28000c */ 	lw	$t0,0xc($t1)
/*    38fb0:	010ac821 */ 	addu	$t9,$t0,$t2
/*    38fb4:	ad39000c */ 	sw	$t9,0xc($t1)
/*    38fb8:	8fac0030 */ 	lw	$t4,0x30($sp)
/*    38fbc:	8fab0038 */ 	lw	$t3,0x38($sp)
/*    38fc0:	ad6c0010 */ 	sw	$t4,0x10($t3)
/*    38fc4:	8fad003c */ 	lw	$t5,0x3c($sp)
/*    38fc8:	24010012 */ 	addiu	$at,$zero,0x12
/*    38fcc:	85af0000 */ 	lh	$t7,0x0($t5)
/*    38fd0:	11e1000c */ 	beq	$t7,$at,.L00039004
/*    38fd4:	00000000 */ 	nop
/*    38fd8:	8fa40038 */ 	lw	$a0,0x38($sp)
/*    38fdc:	0c00e6d3 */ 	jal	__readVarLen
/*    38fe0:	8fa5002c */ 	lw	$a1,0x2c($sp)
/*    38fe4:	8fb8002c */ 	lw	$t8,0x2c($sp)
/*    38fe8:	8fae0038 */ 	lw	$t6,0x38($sp)
/*    38fec:	00408025 */ 	or	$s0,$v0,$zero
/*    38ff0:	00184080 */ 	sll	$t0,$t8,0x2
/*    38ff4:	01c85021 */ 	addu	$t2,$t6,$t0
/*    38ff8:	8d5900b8 */ 	lw	$t9,0xb8($t2)
/*    38ffc:	03304821 */ 	addu	$t1,$t9,$s0
/*    39000:	ad4900b8 */ 	sw	$t1,0xb8($t2)
.L00039004:
/*    39004:	8fab0038 */ 	lw	$t3,0x38($sp)
/*    39008:	240c0001 */ 	addiu	$t4,$zero,0x1
/*    3900c:	ad6c0014 */ 	sw	$t4,0x14($t3)
/*    39010:	10000001 */ 	b	.L00039018
/*    39014:	00000000 */ 	nop
.L00039018:
/*    39018:	8fbf001c */ 	lw	$ra,0x1c($sp)
/*    3901c:	8fb00018 */ 	lw	$s0,0x18($sp)
/*    39020:	27bd0038 */ 	addiu	$sp,$sp,0x38
/*    39024:	03e00008 */ 	jr	$ra
/*    39028:	00000000 */ 	nop
);

u32 __alCSeqGetTrackEvent(ALCSeq *seq, u32 track, ALEvent *event, s32 arg3)
{
	u32 offset;
	u8 status, loopCt, curLpCt, *tmpPtr;

	status = __getTrackByte(seq, track);

	if (status == AL_MIDI_Meta) {
		u8 type = __getTrackByte(seq, track);

		if (type == AL_MIDI_META_TEMPO) {
			event->type = AL_TEMPO_EVT;
			event->msg.tempo.status = status;
			event->msg.tempo.type = type;
			event->msg.tempo.byte1 = __getTrackByte(seq, track);
			event->msg.tempo.byte2 = __getTrackByte(seq, track);
			event->msg.tempo.byte3 = __getTrackByte(seq, track);
			seq->lastStatus[track] = 0;  /* lastStatus not supported after meta */
		} else if (type == AL_MIDI_META_EOT) {
			u32 flagMask;

			flagMask = 1 << track;
			seq->validTracks = seq->validTracks ^ flagMask;

			if (seq->validTracks) { /* there is music left don't end */
				event->type = AL_TRACK_END;
			} else {       /* no more music send AL_SEQ_END_EVT msg */
				event->type = AL_SEQ_END_EVT;
			}
		} else if (type == AL_CMIDI_LOOPSTART_CODE) {
			status = __getTrackByte(seq, track);
			event->msg.loop.count = status << 8;

			status = __getTrackByte(seq, track);
			event->msg.loop.count += status;

			seq->lastStatus[track] = 0;
			event->type = AL_CSP_LOOPSTART;
		} else if (type == AL_CMIDI_LOOPEND_CODE) {
			tmpPtr = seq->curLoc[track];
			loopCt = *tmpPtr++;
			curLpCt = *tmpPtr;

			if (curLpCt == 0 || !arg3) {
				*tmpPtr = loopCt; /* reset current loop count */
				seq->curLoc[track] = tmpPtr + 5; /* move pointer to end of event */
			} else {
				if (curLpCt != 0xff) { /* not a loop forever */
					*tmpPtr = curLpCt - 1;   /* decrement current loop count */
				}

				tmpPtr++;                    /* get offset from end of event */
				offset = (*tmpPtr++) << 24;
				offset += (*tmpPtr++) << 16;
				offset += (*tmpPtr++) << 8;
				offset += *tmpPtr++;
				seq->curLoc[track] = tmpPtr - offset;
			}

			seq->lastStatus[track] = 0;
			event->type = AL_CSP_LOOPEND;
		}
	} else {
		event->type = AL_SEQ_MIDI_EVT;

		if (status & 0x80) {
			event->msg.midi.status = (status & 0xf0) | track;
			event->msg.midi.byte1 = __getTrackByte(seq,track);
			seq->lastStatus[track] = event->msg.midi.status;
		} else {    /* running status */
			event->msg.midi.status = seq->lastStatus[track];
			event->msg.midi.byte1 = status;
		}

		if ((event->msg.midi.status & 0xf0) != AL_MIDI_ProgramChange
				&& (event->msg.midi.status & 0xf0) != AL_MIDI_ChannelPressure) {
			event->msg.midi.byte2 = __getTrackByte(seq,track);

			if ((event->msg.midi.status & 0xf0) == AL_MIDI_NoteOn) {
				event->msg.midi.duration = __readVarLen(seq,track);
			}
		} else {
			event->msg.midi.byte2 = 0;
		}
	}

	return TRUE;
}

s32 alCSeqGetTicks(ALCSeq *seq)
{
	return seq->lastTicks;
}

void alCSeqSetLoc(ALCSeq *seq, ALCSeqMarker *m)
{
	s32 i;

	seq->validTracks    = m->validTracks;
	seq->lastTicks      = m->lastTicks;
	seq->lastDeltaTicks = m->lastDeltaTicks;

	for (i = 0; i < 16; i++) {
		seq->curLoc[i]        = m->curLoc[i];
		seq->curBUPtr[i]      = m->curBUPtr[i];
		seq->curBULen[i]      = m->curBULen[i];
		seq->lastStatus[i]    = m->lastStatus[i];
		seq->evtDeltaTicks[i] = m->evtDeltaTicks[i];
	}
}

void alCSeqGetLoc(ALCSeq *seq, ALCSeqMarker *m)
{
	s32 i;

	m->validTracks    = seq->validTracks;
	m->lastTicks      = seq->lastTicks;
	m->lastDeltaTicks = seq->lastDeltaTicks;

	for (i = 0; i < 16; i++) {
		m->curLoc[i]        = seq->curLoc[i];
		m->curBUPtr[i]      = seq->curBUPtr[i];
		m->curBULen[i]      = seq->curBULen[i];
		m->lastStatus[i]    = seq->lastStatus[i];
		m->evtDeltaTicks[i] = seq->evtDeltaTicks[i];
	}
}

void alCSeqNewMarker(ALCSeq *seq, ALCSeqMarker *m, u32 ticks)
{
	ALEvent evt;
	ALCSeq tempSeq;
	s32 i;

	alCSeqNew(&tempSeq, (u8*)seq->base);

	do {
		m->validTracks    = tempSeq.validTracks;
		m->lastTicks      = tempSeq.lastTicks;
		m->lastDeltaTicks = tempSeq.lastDeltaTicks;

		for (i = 0; i < 16; i++) {
			m->curLoc[i]        = tempSeq.curLoc[i];
			m->curBUPtr[i]      = tempSeq.curBUPtr[i];
			m->curBULen[i]      = tempSeq.curBULen[i];
			m->lastStatus[i]    = tempSeq.lastStatus[i];
			m->evtDeltaTicks[i] = tempSeq.evtDeltaTicks[i];
		}

		alCSeqNextEvent(&tempSeq, &evt, 0);

		if (evt.type == AL_SEQ_END_EVT) {
			break;
		}
	} while (tempSeq.lastTicks < ticks);
}

GLOBAL_ASM(
glabel func00039718
/*    39718:	27bdfde8 */ 	addiu	$sp,$sp,-536
/*    3971c:	afbf0014 */ 	sw	$ra,0x14($sp)
/*    39720:	afa40218 */ 	sw	$a0,0x218($sp)
/*    39724:	afa5021c */ 	sw	$a1,0x21c($sp)
/*    39728:	afa60220 */ 	sw	$a2,0x220($sp)
/*    3972c:	afa70224 */ 	sw	$a3,0x224($sp)
/*    39730:	8fae0218 */ 	lw	$t6,0x218($sp)
/*    39734:	27a40110 */ 	addiu	$a0,$sp,0x110
/*    39738:	0c00e34c */ 	jal	alCSeqNew
/*    3973c:	8dc50000 */ 	lw	$a1,0x0($t6)
/*    39740:	8faf0220 */ 	lw	$t7,0x220($sp)
/*    39744:	afa00108 */ 	sw	$zero,0x108($sp)
/*    39748:	11e00010 */ 	beqz	$t7,.L0003978c
/*    3974c:	00000000 */ 	nop
.L00039750:
/*    39750:	8fb90108 */ 	lw	$t9,0x108($sp)
/*    39754:	8fb8021c */ 	lw	$t8,0x21c($sp)
/*    39758:	00194100 */ 	sll	$t0,$t9,0x4
/*    3975c:	01194023 */ 	subu	$t0,$t0,$t9
/*    39760:	00084080 */ 	sll	$t0,$t0,0x2
/*    39764:	01194023 */ 	subu	$t0,$t0,$t9
/*    39768:	00084080 */ 	sll	$t0,$t0,0x2
/*    3976c:	03084821 */ 	addu	$t1,$t8,$t0
/*    39770:	ad200004 */ 	sw	$zero,0x4($t1)
/*    39774:	8faa0108 */ 	lw	$t2,0x108($sp)
/*    39778:	8fac0220 */ 	lw	$t4,0x220($sp)
/*    3977c:	254b0001 */ 	addiu	$t3,$t2,0x1
/*    39780:	016c082b */ 	sltu	$at,$t3,$t4
/*    39784:	1420fff2 */ 	bnez	$at,.L00039750
/*    39788:	afab0108 */ 	sw	$t3,0x108($sp)
.L0003978c:
/*    3978c:	8fad0114 */ 	lw	$t5,0x114($sp)
/*    39790:	afad001c */ 	sw	$t5,0x1c($sp)
/*    39794:	8fae011c */ 	lw	$t6,0x11c($sp)
/*    39798:	afae0020 */ 	sw	$t6,0x20($sp)
/*    3979c:	8faf0120 */ 	lw	$t7,0x120($sp)
/*    397a0:	afaf0024 */ 	sw	$t7,0x24($sp)
/*    397a4:	afa0010c */ 	sw	$zero,0x10c($sp)
.L000397a8:
/*    397a8:	8fb9010c */ 	lw	$t9,0x10c($sp)
/*    397ac:	0019c080 */ 	sll	$t8,$t9,0x2
/*    397b0:	03b84021 */ 	addu	$t0,$sp,$t8
/*    397b4:	8d080128 */ 	lw	$t0,0x128($t0)
/*    397b8:	03b84821 */ 	addu	$t1,$sp,$t8
/*    397bc:	ad280028 */ 	sw	$t0,0x28($t1)
/*    397c0:	8faa010c */ 	lw	$t2,0x10c($sp)
/*    397c4:	000a5880 */ 	sll	$t3,$t2,0x2
/*    397c8:	03ab6021 */ 	addu	$t4,$sp,$t3
/*    397cc:	8d8c0168 */ 	lw	$t4,0x168($t4)
/*    397d0:	03ab6821 */ 	addu	$t5,$sp,$t3
/*    397d4:	adac0068 */ 	sw	$t4,0x68($t5)
/*    397d8:	8fae010c */ 	lw	$t6,0x10c($sp)
/*    397dc:	03ae7821 */ 	addu	$t7,$sp,$t6
/*    397e0:	91ef01a8 */ 	lbu	$t7,0x1a8($t7)
/*    397e4:	03aec821 */ 	addu	$t9,$sp,$t6
/*    397e8:	a32f00a8 */ 	sb	$t7,0xa8($t9)
/*    397ec:	8fa8010c */ 	lw	$t0,0x10c($sp)
/*    397f0:	03a8c021 */ 	addu	$t8,$sp,$t0
/*    397f4:	931801b8 */ 	lbu	$t8,0x1b8($t8)
/*    397f8:	03a84821 */ 	addu	$t1,$sp,$t0
/*    397fc:	a13800b8 */ 	sb	$t8,0xb8($t1)
/*    39800:	8faa010c */ 	lw	$t2,0x10c($sp)
/*    39804:	000a6080 */ 	sll	$t4,$t2,0x2
/*    39808:	03ac5821 */ 	addu	$t3,$sp,$t4
/*    3980c:	8d6b01c8 */ 	lw	$t3,0x1c8($t3)
/*    39810:	03ac6821 */ 	addu	$t5,$sp,$t4
/*    39814:	adab00c8 */ 	sw	$t3,0xc8($t5)
/*    39818:	8faf010c */ 	lw	$t7,0x10c($sp)
/*    3981c:	25ee0001 */ 	addiu	$t6,$t7,0x1
/*    39820:	29c10010 */ 	slti	$at,$t6,0x10
/*    39824:	1420ffe0 */ 	bnez	$at,.L000397a8
/*    39828:	afae010c */ 	sw	$t6,0x10c($sp)
/*    3982c:	27a40110 */ 	addiu	$a0,$sp,0x110
/*    39830:	27a50208 */ 	addiu	$a1,$sp,0x208
/*    39834:	0c00e3ac */ 	jal	alCSeqNextEvent
/*    39838:	00003025 */ 	or	$a2,$zero,$zero
/*    3983c:	87b90208 */ 	lh	$t9,0x208($sp)
/*    39840:	24010013 */ 	addiu	$at,$zero,0x13
/*    39844:	1721003a */ 	bne	$t9,$at,.L00039930
/*    39848:	00000000 */ 	nop
/*    3984c:	8fb80214 */ 	lw	$t8,0x214($sp)
/*    39850:	8fa90224 */ 	lw	$t1,0x224($sp)
/*    39854:	00184203 */ 	sra	$t0,$t8,0x8
/*    39858:	0109082b */ 	sltu	$at,$t0,$t1
/*    3985c:	14200034 */ 	bnez	$at,.L00039930
/*    39860:	00000000 */ 	nop
/*    39864:	8faa0220 */ 	lw	$t2,0x220($sp)
/*    39868:	012a5821 */ 	addu	$t3,$t1,$t2
/*    3986c:	010b082b */ 	sltu	$at,$t0,$t3
/*    39870:	1020002f */ 	beqz	$at,.L00039930
/*    39874:	00000000 */ 	nop
/*    39878:	8fac0214 */ 	lw	$t4,0x214($sp)
/*    3987c:	8faf0224 */ 	lw	$t7,0x224($sp)
/*    39880:	8fb8021c */ 	lw	$t8,0x21c($sp)
/*    39884:	000c6a03 */ 	sra	$t5,$t4,0x8
/*    39888:	01af7023 */ 	subu	$t6,$t5,$t7
/*    3988c:	000ec900 */ 	sll	$t9,$t6,0x4
/*    39890:	032ec823 */ 	subu	$t9,$t9,$t6
/*    39894:	0019c880 */ 	sll	$t9,$t9,0x2
/*    39898:	032ec823 */ 	subu	$t9,$t9,$t6
/*    3989c:	0019c880 */ 	sll	$t9,$t9,0x2
/*    398a0:	03194821 */ 	addu	$t1,$t8,$t9
/*    398a4:	8d2a0004 */ 	lw	$t2,0x4($t1)
/*    398a8:	15400021 */ 	bnez	$t2,.L00039930
/*    398ac:	00000000 */ 	nop
/*    398b0:	8fa80214 */ 	lw	$t0,0x214($sp)
/*    398b4:	8fac0224 */ 	lw	$t4,0x224($sp)
/*    398b8:	8fae021c */ 	lw	$t6,0x21c($sp)
/*    398bc:	00085a03 */ 	sra	$t3,$t0,0x8
/*    398c0:	016c6823 */ 	subu	$t5,$t3,$t4
/*    398c4:	000d7900 */ 	sll	$t7,$t5,0x4
/*    398c8:	01ed7823 */ 	subu	$t7,$t7,$t5
/*    398cc:	000f7880 */ 	sll	$t7,$t7,0x2
/*    398d0:	01ed7823 */ 	subu	$t7,$t7,$t5
/*    398d4:	000f7880 */ 	sll	$t7,$t7,0x2
/*    398d8:	27b9001c */ 	addiu	$t9,$sp,0x1c
/*    398dc:	272800e4 */ 	addiu	$t0,$t9,0xe4
/*    398e0:	01cfc021 */ 	addu	$t8,$t6,$t7
.L000398e4:
/*    398e4:	8f2a0000 */ 	lw	$t2,0x0($t9)
/*    398e8:	2739000c */ 	addiu	$t9,$t9,0xc
/*    398ec:	2718000c */ 	addiu	$t8,$t8,0xc
/*    398f0:	af0afff4 */ 	sw	$t2,-0xc($t8)
/*    398f4:	8f29fff8 */ 	lw	$t1,-0x8($t9)
/*    398f8:	af09fff8 */ 	sw	$t1,-0x8($t8)
/*    398fc:	8f2afffc */ 	lw	$t2,-0x4($t9)
/*    39900:	1728fff8 */ 	bne	$t9,$t0,.L000398e4
/*    39904:	af0afffc */ 	sw	$t2,-0x4($t8)
/*    39908:	8f2a0000 */ 	lw	$t2,0x0($t9)
/*    3990c:	af0a0000 */ 	sw	$t2,0x0($t8)
/*    39910:	8f290004 */ 	lw	$t1,0x4($t9)
/*    39914:	af090004 */ 	sw	$t1,0x4($t8)
/*    39918:	8fab0108 */ 	lw	$t3,0x108($sp)
/*    3991c:	256cffff */ 	addiu	$t4,$t3,-1
/*    39920:	1d800003 */ 	bgtz	$t4,.L00039930
/*    39924:	afac0108 */ 	sw	$t4,0x108($sp)
/*    39928:	10000007 */ 	b	.L00039948
/*    3992c:	00000000 */ 	nop
.L00039930:
/*    39930:	87ad0208 */ 	lh	$t5,0x208($sp)
/*    39934:	24010004 */ 	addiu	$at,$zero,0x4
/*    39938:	15a1ff94 */ 	bne	$t5,$at,.L0003978c
/*    3993c:	00000000 */ 	nop
/*    39940:	10000001 */ 	b	.L00039948
/*    39944:	00000000 */ 	nop
.L00039948:
/*    39948:	8fbf0014 */ 	lw	$ra,0x14($sp)
/*    3994c:	27bd0218 */ 	addiu	$sp,$sp,0x218
/*    39950:	03e00008 */ 	jr	$ra
/*    39954:	00000000 */ 	nop
);

u8 __getTrackByte(ALCSeq *seq, u32 track)
{
    u8 theByte;

    if (seq->curBULen[track]) {
        theByte = *seq->curBUPtr[track];
        seq->curBUPtr[track]++;
        seq->curBULen[track]--;
    } else /* need to handle backup mode */ {
        theByte = *seq->curLoc[track];
        seq->curLoc[track]++;

        if (theByte == AL_CMIDI_BLOCK_CODE) {
            u8 loBackUp, hiBackUp, theLen, nextByte;
            u32 backup;

            nextByte = *seq->curLoc[track];
            seq->curLoc[track]++;

            if (nextByte != AL_CMIDI_BLOCK_CODE) {
                /* if here, then got a backup section. get the amount of
                   backup, and the len of the section. Subtract the amount of
                   backup from the curLoc ptr, and subtract four more, since
                   curLoc has been advanced by four while reading the codes. */
                hiBackUp = nextByte;
                loBackUp = *seq->curLoc[track];
                seq->curLoc[track]++;
                theLen = *seq->curLoc[track];
                seq->curLoc[track]++;
                backup = (u32)hiBackUp;
                backup = backup << 8;
                backup += loBackUp;
                seq->curBUPtr[track] = seq->curLoc[track] - (backup + 4);
                seq->curBULen[track] = (u32)theLen;

                /* now get the byte */
                theByte = *seq->curBUPtr[track];
                seq->curBUPtr[track]++;
                seq->curBULen[track]--;
            }
        }
    }

    return theByte;
}

u32 __readVarLen(ALCSeq *seq, u32 track)
{
    u32 value;
    u32 c;

    value = (u32)__getTrackByte(seq, track);

    if (value & 0x80) {
        value &= 0x7f;

        do {
            c = (u32)__getTrackByte(seq, track);
            value = (value << 7) + (c & 0x7f);
        } while (c & 0x80);
    }

    return value;
}

GLOBAL_ASM(
glabel func00039be0
/*    39be0:	27bdffd8 */ 	addiu	$sp,$sp,-40
/*    39be4:	afbf0014 */ 	sw	$ra,0x14($sp)
/*    39be8:	afa40028 */ 	sw	$a0,0x28($sp)
/*    39bec:	afa5002c */ 	sw	$a1,0x2c($sp)
/*    39bf0:	240e000d */ 	addiu	$t6,$zero,0xd
/*    39bf4:	a7ae0018 */ 	sh	$t6,0x18($sp)
/*    39bf8:	8faf002c */ 	lw	$t7,0x2c($sp)
/*    39bfc:	afaf001c */ 	sw	$t7,0x1c($sp)
/*    39c00:	8fa40028 */ 	lw	$a0,0x28($sp)
/*    39c04:	27a50018 */ 	addiu	$a1,$sp,0x18
/*    39c08:	00003025 */ 	or	$a2,$zero,$zero
/*    39c0c:	00003825 */ 	or	$a3,$zero,$zero
/*    39c10:	0c00f184 */ 	jal	alEvtqPostEvent
/*    39c14:	24840048 */ 	addiu	$a0,$a0,0x48
/*    39c18:	10000001 */ 	b	.L00039c20
/*    39c1c:	00000000 */ 	nop
.L00039c20:
/*    39c20:	8fbf0014 */ 	lw	$ra,0x14($sp)
/*    39c24:	27bd0028 */ 	addiu	$sp,$sp,0x28
/*    39c28:	03e00008 */ 	jr	$ra
/*    39c2c:	00000000 */ 	nop
);

GLOBAL_ASM(
glabel func00039c30
/*    39c30:	27bdffd8 */ 	addiu	$sp,$sp,-40
/*    39c34:	afbf0014 */ 	sw	$ra,0x14($sp)
/*    39c38:	afa40028 */ 	sw	$a0,0x28($sp)
/*    39c3c:	240e000f */ 	addiu	$t6,$zero,0xf
/*    39c40:	a7ae0018 */ 	sh	$t6,0x18($sp)
/*    39c44:	8fa40028 */ 	lw	$a0,0x28($sp)
/*    39c48:	27a50018 */ 	addiu	$a1,$sp,0x18
/*    39c4c:	00003025 */ 	or	$a2,$zero,$zero
/*    39c50:	00003825 */ 	or	$a3,$zero,$zero
/*    39c54:	0c00f184 */ 	jal	alEvtqPostEvent
/*    39c58:	24840048 */ 	addiu	$a0,$a0,0x48
/*    39c5c:	10000001 */ 	b	.L00039c64
/*    39c60:	00000000 */ 	nop
.L00039c64:
/*    39c64:	8fbf0014 */ 	lw	$ra,0x14($sp)
/*    39c68:	27bd0028 */ 	addiu	$sp,$sp,0x28
/*    39c6c:	03e00008 */ 	jr	$ra
/*    39c70:	00000000 */ 	nop
);
