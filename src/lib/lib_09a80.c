#include <ultra64.h>
#include "constants.h"
#include "game/game_0b3350.h"
#include "game/game_0b4950.h"
#include "game/game_0b69d0.h"
#include "game/file.h"
#include "game/game_176080.h"
#include "game/gfxmemory.h"
#include "game/options.h"
#include "bss.h"
#include "lib/lib_09660.h"
#include "lib/lib_09a80.h"
#include "lib/memory.h"
#include "lib/lib_159b0.h"
#include "lib/lib_30ce0.h"
#include "data.h"
#include "types.h"

Mtxf var80092830;
Mtx *var80092870;
u16 var80092874;
u8 var80092876;
u8 var80092877;

struct rend_vidat var8005d530[] = {
	{
		0, 0, 0, 0,
		320, 220,         // x and y
		60,               // fovy
		1.4545454978943f, // aspect
		30,               // znear
		10000,            // zfar
		320, 220,         // bufx and bufy
		320, 220,         // viewx and viewy
		0, 0,             // viewleft and viewtop
		true,             // usezbuf
		0,
	}, {
		0, 0, 0, 0,
		320, 220,         // x and y
		60,               // fovy
		1.4545454978943f, // aspect
		30,               // znear
		10000,            // zfar
		320, 220,         // bufx and bufy
		320, 220,         // viewx and viewy
		0, 0,             // viewleft and viewtop
		true,             // usezbuf
		0,
	},
};

u32 var8005d588 = 0;
u32 var8005d58c = 0;
struct rend_vidat *var8005d590 = &var8005d530[0];
struct rend_vidat *g_ViData = &var8005d530[0];
bool g_ViIs16Bit = true;
s32 var8005d59c = 0;
u32 var8005d5a0 = 0x00000000;

void vi00009a80(void)
{
	// empty
}

void vi00009a88(void)
{
	// empty
}

void vi00009a90(void)
{
	// empty
}

void vi00009a98(void)
{
	// empty
}

void vi00009aa0(u32 value)
{
	// empty
}

Gfx *viRenderDebug(Gfx *gdl)
{
	return gdl;
}

void vi00009ab0(void)
{
	var80092876 = 0;
	var80092877 = 1;

	var8005d590 = var8005d530 + var80092876;
	g_ViData = var8005d530 + var80092877;

#if VERSION >= VERSION_PAL_FINAL
	if (IS4MB()) {
		var8005d530[0].y = 220;
		var8005d530[0].bufy = 220;
		var8005d530[0].viewy = 220;

		var8005d530[1].y = 220;
		var8005d530[1].bufy = 220;
		var8005d530[1].viewy = 220;

		var8005d588 = 0;
		var8005d58c = 0;
	} else {
		var8005d588 = 0;
		var8005d58c = 12;
	}
#else
	var8005d588 = 0;
	var8005d58c = 0;

	if (IS4MB()) {
		var8005d530[0].y = 220;
		var8005d530[0].bufy = 220;
		var8005d530[0].viewy = 220;

		var8005d530[1].y = 220;
		var8005d530[1].bufy = 220;
		var8005d530[1].viewy = 220;
	}
#endif
}

void vi00009b50(u8 *fb)
{
	s32 i;

	for (i = 0; i < 2; i++) {
		var8009cac0[i] = fb;

		var8005d530[i].x = 576;
		var8005d530[i].bufx = 576;
		var8005d530[i].viewx = (VERSION >= VERSION_NTSC_1_0 ? 576 : 480);

		var8005d530[i].y = 48;
		var8005d530[i].bufy = 48;
		var8005d530[i].viewy = 48;
	}

	var8005d590->fb = var8009cac0[var80092876];
	g_ViData->fb = var8009cac0[var80092877];

	var8005d59c = 1;
	g_Vars.fourmeg2player = false;
}

void vi00009bf8(void)
{
	s32 i;

	for (i = 0; i < 2; i++) {
		var8005d530[i].x = 320;
		var8005d530[i].bufx = 320;
		var8005d530[i].viewx = 320;

		var8005d530[i].y = 220;
		var8005d530[i].bufy = 220;
		var8005d530[i].viewy = 220;
	}

	g_Vars.fourmeg2player = false;

#if VERSION >= VERSION_PAL_FINAL
	viResetDefaultModeIf4Mb();
#endif
}

const s16 var700526d0[] = {320, 320, 640};

#if PAL
const s16 var700526d8[] = {220, 220, 504};
#else
const s16 var700526d8[] = {220, 220, 440};
#endif

/**
 * Allocate the colour framebuffers for the given stage.
 *
 * Regardless of whether hi-res is being used or not, the buffers are allocated
 * for hi-res. This is because hi-res can be changed mid-stage, and the engine
 * cannot reallocate the frame buffer without clearing the stage's entire memory
 * pool.
 *
 * The same is probably true for wide and cinema modes.
 */
void viAllocateFbs(s32 stagenum)
{
	s32 i;
	s32 fbsize;
	u8 *ptr;
	u8 *fb0;
	u8 *fb1;

	g_Vars.fourmeg2player = false;

	if (stagenum == STAGE_TITLE || stagenum == STAGE_TEST_OLD) {
		if (IS4MB()) {
			vi0000aab0(2);
			fbsize = 640 * 440 * 2;
		} else {
			vi0000aab0(2);
			fbsize = var700526d0[2] * var700526d8[2] * 2;
		}
	} else {
		vi0000aab0(1);

		if (1);

		fbsize = IS4MB() ? 320 * 220 * 2 : 320 * 220 * 4;

		if (IS4MB() && PLAYERCOUNT() == 2) {
			fbsize = 320 * 220;
			g_Vars.fourmeg2player = true;
		} else if ((g_Vars.coopplayernum >= 0 || g_Vars.antiplayernum >= 0) && PLAYERCOUNT() == 2) {
#if PAL
			fbsize = 320 * 266 * 2;
#else
			fbsize = 320 * 220 * 2;
#endif
		}
	}

	ptr = malloc(fbsize * 2 + 0x40, MEMPOOL_STAGE);

	ptr = (u8 *)(((u32)ptr + 0x3f) & 0xffffffc0);

	var8009cac0[0] = &ptr[0];
	var8009cac0[1] = fbsize + ptr;

	var8005d590->fb = var8009cac0[var80092876];
	g_ViData->fb = var8009cac0[var80092877];

	fb0 = var8009cac0[0];
	fb1 = var8009cac0[1];

	for (i = 0; i < fbsize; i++) {
		fb0[i] = 0;
		fb1[i] = 0;
	}

	var8005d59c = 1;
}

/**
 * If black is true, set the video output to black indefinitely.
 * g_ViUnblackTimer is set to 3 which causes the timer to be paused.
 *
 * If black is false, set the timer to 2. This causes it to tick down once per
 * frame and unblack once it reaches 0.
 */
void viBlack(bool black)
{
	black += 2;
	g_ViUnblackTimer = black;
}

#if VERSION >= VERSION_NTSC_1_0
GLOBAL_ASM(
glabel vi00009ed4
/*     9ed4:	3c038006 */ 	lui	$v1,%hi(var8005ce9c)
/*     9ed8:	2463ce9c */ 	addiu	$v1,$v1,%lo(var8005ce9c)
/*     9edc:	8c620000 */ 	lw	$v0,0x0($v1)
/*     9ee0:	27bdffe0 */ 	addiu	$sp,$sp,-32
/*     9ee4:	afbf0014 */ 	sw	$ra,0x14($sp)
/*     9ee8:	10400006 */ 	beqz	$v0,.L00009f04
/*     9eec:	3c188006 */ 	lui	$t8,%hi(var8005ce98)
/*     9ef0:	244effff */ 	addiu	$t6,$v0,-1
/*     9ef4:	15c00003 */ 	bnez	$t6,.L00009f04
/*     9ef8:	ac6e0000 */ 	sw	$t6,0x0($v1)
/*     9efc:	3c018006 */ 	lui	$at,%hi(var8005ce98)
/*     9f00:	ac20ce98 */ 	sw	$zero,%lo(var8005ce98)($at)
.L00009f04:
/*     9f04:	3c038006 */ 	lui	$v1,%hi(var8005ce94)
/*     9f08:	2463ce94 */ 	addiu	$v1,$v1,%lo(var8005ce94)
/*     9f0c:	8c620000 */ 	lw	$v0,0x0($v1)
/*     9f10:	8f18ce98 */ 	lw	$t8,%lo(var8005ce98)($t8)
/*     9f14:	24040001 */ 	addiu	$a0,$zero,0x1
/*     9f18:	0002c823 */ 	negu	$t9,$v0
/*     9f1c:	00580019 */ 	multu	$v0,$t8
/*     9f20:	ac790000 */ 	sw	$t9,0x0($v1)
/*     9f24:	00002812 */ 	mflo	$a1
/*     9f28:	afa50018 */ 	sw	$a1,0x18($sp)
/*     9f2c:	0c012194 */ 	jal	osSetIntMask
/*     9f30:	00000000 */ 	nop
/*     9f34:	3c078006 */ 	lui	$a3,%hi(var8005ce74)
/*     9f38:	3c038009 */ 	lui	$v1,%hi(var8008de0c)
/*     9f3c:	8c6dde0c */ 	lw	$t5,%lo(var8008de0c)($v1)
/*     9f40:	24e7ce74 */ 	addiu	$a3,$a3,%lo(var8005ce74)
/*     9f44:	8fa50018 */ 	lw	$a1,0x18($sp)
/*     9f48:	8cf90000 */ 	lw	$t9,0x0($a3)
/*     9f4c:	000d4c03 */ 	sra	$t1,$t5,0x10
/*     9f50:	3c068009 */ 	lui	$a2,%hi(var8008dd60+0x4)
/*     9f54:	01255821 */ 	addu	$t3,$t1,$a1
/*     9f58:	00194080 */ 	sll	$t0,$t9,0x2
/*     9f5c:	24c6dd64 */ 	addiu	$a2,$a2,%lo(var8008dd60+0x4)
/*     9f60:	00084823 */ 	negu	$t1,$t0
/*     9f64:	00c95021 */ 	addu	$t2,$a2,$t1
/*     9f68:	000b6400 */ 	sll	$t4,$t3,0x10
/*     9f6c:	8d4b0000 */ 	lw	$t3,0x0($t2)
/*     9f70:	01a57021 */ 	addu	$t6,$t5,$a1
/*     9f74:	31cfffff */ 	andi	$t7,$t6,0xffff
/*     9f78:	018fc025 */ 	or	$t8,$t4,$t7
/*     9f7c:	3c038009 */ 	lui	$v1,%hi(var8008de10)
/*     9f80:	ad780030 */ 	sw	$t8,0x30($t3)
/*     9f84:	8c68de10 */ 	lw	$t0,%lo(var8008de10)($v1)
/*     9f88:	8ceb0000 */ 	lw	$t3,0x0($a3)
/*     9f8c:	00402025 */ 	or	$a0,$v0,$zero
/*     9f90:	00087403 */ 	sra	$t6,$t0,0x10
/*     9f94:	01c57821 */ 	addu	$t7,$t6,$a1
/*     9f98:	000b6880 */ 	sll	$t5,$t3,0x2
/*     9f9c:	000d7023 */ 	negu	$t6,$t5
/*     9fa0:	00ce6021 */ 	addu	$t4,$a2,$t6
/*     9fa4:	000fcc00 */ 	sll	$t9,$t7,0x10
/*     9fa8:	01054821 */ 	addu	$t1,$t0,$a1
/*     9fac:	8d8f0000 */ 	lw	$t7,0x0($t4)
/*     9fb0:	312affff */ 	andi	$t2,$t1,0xffff
/*     9fb4:	032ac025 */ 	or	$t8,$t9,$t2
/*     9fb8:	0c012194 */ 	jal	osSetIntMask
/*     9fbc:	adf80044 */ 	sw	$t8,0x44($t7)
/*     9fc0:	3c088006 */ 	lui	$t0,%hi(var8005ce74)
/*     9fc4:	8d08ce74 */ 	lw	$t0,%lo(var8005ce74)($t0)
/*     9fc8:	3c048009 */ 	lui	$a0,%hi(var8008dd60+0x4)
/*     9fcc:	00084880 */ 	sll	$t1,$t0,0x2
/*     9fd0:	0009c823 */ 	negu	$t9,$t1
/*     9fd4:	00992021 */ 	addu	$a0,$a0,$t9
/*     9fd8:	0c012354 */ 	jal	osViSetMode
/*     9fdc:	8c84dd64 */ 	lw	$a0,%lo(var8008dd60+0x4)($a0)
/*     9fe0:	3c048006 */ 	lui	$a0,%hi(g_ViUnblackTimer+0x3)
/*     9fe4:	0c012338 */ 	jal	osViBlack
/*     9fe8:	9084ce93 */ 	lbu	$a0,%lo(g_ViUnblackTimer+0x3)($a0)
/*     9fec:	3c0a8006 */ 	lui	$t2,%hi(var8005ce74)
/*     9ff0:	8d4ace74 */ 	lw	$t2,%lo(var8005ce74)($t2)
/*     9ff4:	3c018006 */ 	lui	$at,%hi(var8005ce78+0x4)
/*     9ff8:	000a5880 */ 	sll	$t3,$t2,0x2
/*     9ffc:	000b6823 */ 	negu	$t5,$t3
/*     a000:	002d0821 */ 	addu	$at,$at,$t5
/*     a004:	0c012370 */ 	jal	osViSetXScale
/*     a008:	c42cce7c */ 	lwc1	$f12,%lo(var8005ce78+0x4)($at)
/*     a00c:	3c0e8006 */ 	lui	$t6,%hi(var8005ce74)
/*     a010:	8dcece74 */ 	lw	$t6,%lo(var8005ce74)($t6)
/*     a014:	3c018006 */ 	lui	$at,%hi(var8005ce80+0x4)
/*     a018:	000e6080 */ 	sll	$t4,$t6,0x2
/*     a01c:	000cc023 */ 	negu	$t8,$t4
/*     a020:	00380821 */ 	addu	$at,$at,$t8
/*     a024:	0c0123bc */ 	jal	osViSetYScale
/*     a028:	c42cce84 */ 	lwc1	$f12,%lo(var8005ce80+0x4)($at)
/*     a02c:	0c0123d4 */ 	jal	osViSetSpecialFeatures
/*     a030:	24040042 */ 	addiu	$a0,$zero,0x42
/*     a034:	8fbf0014 */ 	lw	$ra,0x14($sp)
/*     a038:	27bd0020 */ 	addiu	$sp,$sp,0x20
/*     a03c:	03e00008 */ 	jr	$ra
/*     a040:	00000000 */ 	nop
);
#else
GLOBAL_ASM(
glabel vi00009ed4
/*     a078:	3c038006 */ 	lui	$v1,0x8006
/*     a07c:	2463e61c */ 	addiu	$v1,$v1,-6628
/*     a080:	8c620000 */ 	lw	$v0,0x0($v1)
/*     a084:	27bdffe8 */ 	addiu	$sp,$sp,-24
/*     a088:	afbf0014 */ 	sw	$ra,0x14($sp)
/*     a08c:	10400006 */ 	beqz	$v0,.NB0000a0a8
/*     a090:	3c088006 */ 	lui	$t0,0x8006
/*     a094:	244effff */ 	addiu	$t6,$v0,-1
/*     a098:	15c00003 */ 	bnez	$t6,.NB0000a0a8
/*     a09c:	ac6e0000 */ 	sw	$t6,0x0($v1)
/*     a0a0:	3c018006 */ 	lui	$at,0x8006
/*     a0a4:	ac20e618 */ 	sw	$zero,-0x19e8($at)
.NB0000a0a8:
/*     a0a8:	2508e614 */ 	addiu	$t0,$t0,-6636
/*     a0ac:	3c188006 */ 	lui	$t8,0x8006
/*     a0b0:	8f18e618 */ 	lw	$t8,-0x19e8($t8)
/*     a0b4:	8d050000 */ 	lw	$a1,0x0($t0)
/*     a0b8:	3c028009 */ 	lui	$v0,0x8009
/*     a0bc:	3c078006 */ 	lui	$a3,0x8006
/*     a0c0:	00b80019 */ 	multu	$a1,$t8
/*     a0c4:	0005c823 */ 	negu	$t9,$a1
/*     a0c8:	ad190000 */ 	sw	$t9,0x0($t0)
/*     a0cc:	8c4e043c */ 	lw	$t6,0x43c($v0)
/*     a0d0:	24e7e5f4 */ 	addiu	$a3,$a3,-6668
/*     a0d4:	8ce90000 */ 	lw	$t1,0x0($a3)
/*     a0d8:	000e5403 */ 	sra	$t2,$t6,0x10
/*     a0dc:	3c068009 */ 	lui	$a2,0x8009
/*     a0e0:	24c60394 */ 	addiu	$a2,$a2,0x394
/*     a0e4:	3c028009 */ 	lui	$v0,0x8009
/*     a0e8:	00001812 */ 	mflo	$v1
/*     a0ec:	01436021 */ 	addu	$t4,$t2,$v1
/*     a0f0:	00095080 */ 	sll	$t2,$t1,0x2
/*     a0f4:	000a5823 */ 	negu	$t3,$t2
/*     a0f8:	000c6c00 */ 	sll	$t5,$t4,0x10
/*     a0fc:	00cb6021 */ 	addu	$t4,$a2,$t3
/*     a100:	01c37821 */ 	addu	$t7,$t6,$v1
/*     a104:	8d8e0000 */ 	lw	$t6,0x0($t4)
/*     a108:	31f8ffff */ 	andi	$t8,$t7,0xffff
/*     a10c:	01b8c825 */ 	or	$t9,$t5,$t8
/*     a110:	add90030 */ 	sw	$t9,0x30($t6)
/*     a114:	8c4b0440 */ 	lw	$t3,0x440($v0)
/*     a118:	8cef0000 */ 	lw	$t7,0x0($a3)
/*     a11c:	000b6c03 */ 	sra	$t5,$t3,0x10
/*     a120:	01a34821 */ 	addu	$t1,$t5,$v1
/*     a124:	000f6880 */ 	sll	$t5,$t7,0x2
/*     a128:	000dc023 */ 	negu	$t8,$t5
/*     a12c:	00095400 */ 	sll	$t2,$t1,0x10
/*     a130:	00d84821 */ 	addu	$t1,$a2,$t8
/*     a134:	01636021 */ 	addu	$t4,$t3,$v1
/*     a138:	8d2b0000 */ 	lw	$t3,0x0($t1)
/*     a13c:	3199ffff */ 	andi	$t9,$t4,0xffff
/*     a140:	01597025 */ 	or	$t6,$t2,$t9
/*     a144:	ad6e0044 */ 	sw	$t6,0x44($t3)
/*     a148:	8cec0000 */ 	lw	$t4,0x0($a3)
/*     a14c:	000c5080 */ 	sll	$t2,$t4,0x2
/*     a150:	000ac823 */ 	negu	$t9,$t2
/*     a154:	00d97821 */ 	addu	$t7,$a2,$t9
/*     a158:	0c012848 */ 	jal	osViSetMode
/*     a15c:	8de40000 */ 	lw	$a0,0x0($t7)
/*     a160:	3c048006 */ 	lui	$a0,0x8006
/*     a164:	0c01282c */ 	jal	osViBlack
/*     a168:	9084e613 */ 	lbu	$a0,-0x19ed($a0)
/*     a16c:	3c0d8006 */ 	lui	$t5,0x8006
/*     a170:	8dade5f4 */ 	lw	$t5,-0x1a0c($t5)
/*     a174:	3c018006 */ 	lui	$at,0x8006
/*     a178:	000dc080 */ 	sll	$t8,$t5,0x2
/*     a17c:	00184823 */ 	negu	$t1,$t8
/*     a180:	00290821 */ 	addu	$at,$at,$t1
/*     a184:	0c012864 */ 	jal	osViSetXScale
/*     a188:	c42ce5fc */ 	lwc1	$f12,-0x1a04($at)
/*     a18c:	3c0e8006 */ 	lui	$t6,0x8006
/*     a190:	8dcee5f4 */ 	lw	$t6,-0x1a0c($t6)
/*     a194:	3c018006 */ 	lui	$at,0x8006
/*     a198:	000e5880 */ 	sll	$t3,$t6,0x2
/*     a19c:	000b6023 */ 	negu	$t4,$t3
/*     a1a0:	002c0821 */ 	addu	$at,$at,$t4
/*     a1a4:	0c0128b0 */ 	jal	osViSetYScale
/*     a1a8:	c42ce604 */ 	lwc1	$f12,-0x19fc($at)
/*     a1ac:	0c0128c8 */ 	jal	osViSetSpecialFeatures
/*     a1b0:	24040042 */ 	addiu	$a0,$zero,0x42
/*     a1b4:	8fbf0014 */ 	lw	$ra,0x14($sp)
/*     a1b8:	27bd0018 */ 	addiu	$sp,$sp,0x18
/*     a1bc:	03e00008 */ 	jr	$ra
/*     a1c0:	00000000 */ 	sll	$zero,$zero,0x0
);
#endif

// Mismatch: regalloc
//void vi00009ed4(void)
//{
//	s32 prevmask;
//	s32 value;
//
//	if (var8005ce9c != 0) {
//		var8005ce9c--;
//
//		if (var8005ce9c == 0) {
//			var8005ce98 = 0;
//		}
//	}
//
//	value = var8005ce94 * var8005ce98;
//	var8005ce94 = -var8005ce94;
//
//#if VERSION >= VERSION_NTSC_1_0
//	prevmask = osSetIntMask(1);
//#endif
//
//	var8008dd60[1 - var8005ce74]->fldRegs[0].vStart =
//		((var8008de0c >> 16) + value) << 16 | var8008de0c + value & 0xffff;
//	var8008dd60[1 - var8005ce74]->fldRegs[1].vStart =
//		((var8008de10 >> 16) + value) << 16 | var8008de10 + value & 0xffff;
//
//#if VERSION >= VERSION_NTSC_1_0
//	osSetIntMask(prevmask);
//#endif
//
//	osViSetMode(var8008dd60[1 - var8005ce74]);
//	osViBlack(g_ViUnblackTimer);
//	osViSetXScale(var8005ce78[1 - var8005ce74]);
//	osViSetYScale(var8005ce80[1 - var8005ce74]);
//	osViSetSpecialFeatures(OS_VI_GAMMA_OFF | OS_VI_DITHER_FILTER_ON);
//}

#if VERSION >= VERSION_PAL_FINAL
GLOBAL_ASM(
glabel vi0000a044
/*     9f64:	3c098006 */ 	lui	$t1,0x8006
/*     9f68:	3c0e8006 */ 	lui	$t6,0x8006
/*     9f6c:	8dced230 */ 	lw	$t6,-0x2dd0($t6)
/*     9f70:	8d29d234 */ 	lw	$t1,-0x2dcc($t1)
/*     9f74:	27bdffe8 */ 	addiu	$sp,$sp,-24
/*     9f78:	afbf0014 */ 	sw	$ra,0x14($sp)
/*     9f7c:	91cf0000 */ 	lbu	$t7,0x0($t6)
/*     9f80:	91220000 */ 	lbu	$v0,0x0($t1)
/*     9f84:	504f0014 */ 	beql	$v0,$t7,.PF00009fd8
/*     9f88:	85380004 */ 	lh	$t8,0x4($t1)
/*     9f8c:	10400008 */ 	beqz	$v0,.PF00009fb0
/*     9f90:	00401825 */ 	move	$v1,$v0
/*     9f94:	241f0001 */ 	li	$ra,0x1
/*     9f98:	105f000e */ 	beq	$v0,$ra,.PF00009fd4
/*     9f9c:	24010002 */ 	li	$at,0x2
/*     9fa0:	5061000d */ 	beql	$v1,$at,.PF00009fd8
/*     9fa4:	85380004 */ 	lh	$t8,0x4($t1)
/*     9fa8:	1000000b */ 	b	.PF00009fd8
/*     9fac:	85380004 */ 	lh	$t8,0x4($t1)
.PF00009fb0:
/*     9fb0:	3c013f80 */ 	lui	$at,0x3f80
/*     9fb4:	44816000 */ 	mtc1	$at,$f12
/*     9fb8:	0c0121fc */ 	jal	osViSetYScale
/*     9fbc:	00000000 */ 	nop
/*     9fc0:	0c012178 */ 	jal	osViBlack
/*     9fc4:	24040001 */ 	li	$a0,0x1
/*     9fc8:	3c098006 */ 	lui	$t1,0x8006
/*     9fcc:	8d29d234 */ 	lw	$t1,-0x2dcc($t1)
/*     9fd0:	91220000 */ 	lbu	$v0,0x0($t1)
.PF00009fd4:
/*     9fd4:	85380004 */ 	lh	$t8,0x4($t1)
.PF00009fd8:
/*     9fd8:	85390018 */ 	lh	$t9,0x18($t1)
/*     9fdc:	852e0006 */ 	lh	$t6,0x6($t1)
/*     9fe0:	44982000 */ 	mtc1	$t8,$f4
/*     9fe4:	852f001a */ 	lh	$t7,0x1a($t1)
/*     9fe8:	44994000 */ 	mtc1	$t9,$f8
/*     9fec:	468021a0 */ 	cvt.s.w	$f6,$f4
/*     9ff0:	448e8000 */ 	mtc1	$t6,$f16
/*     9ff4:	448f2000 */ 	mtc1	$t7,$f4
/*     9ff8:	3c0d8009 */ 	lui	$t5,0x8009
/*     9ffc:	241f0001 */ 	li	$ra,0x1
/*     a000:	468042a0 */ 	cvt.s.w	$f10,$f8
/*     a004:	3c0c8006 */ 	lui	$t4,0x8006
/*     a008:	3c013f80 */ 	lui	$at,0x3f80
/*     a00c:	25ade35c */ 	addiu	$t5,$t5,-7332
/*     a010:	468084a0 */ 	cvt.s.w	$f18,$f16
/*     a014:	46802220 */ 	cvt.s.w	$f8,$f4
/*     a018:	460a3083 */ 	div.s	$f2,$f6,$f10
/*     a01c:	14400003 */ 	bnez	$v0,.PF0000a02c
/*     a020:	46089003 */ 	div.s	$f0,$f18,$f8
/*     a024:	44810000 */ 	mtc1	$at,$f0
/*     a028:	00000000 */ 	nop
.PF0000a02c:
/*     a02c:	8d8cd240 */ 	lw	$t4,-0x2dc0($t4)
/*     a030:	3c018006 */ 	lui	$at,0x8006
/*     a034:	000c5880 */ 	sll	$t3,$t4,0x2
/*     a038:	002b0821 */ 	addu	$at,$at,$t3
/*     a03c:	e422cb28 */ 	swc1	$f2,-0x34d8($at)
/*     a040:	3c018006 */ 	lui	$at,0x8006
/*     a044:	002b0821 */ 	addu	$at,$at,$t3
/*     a048:	e420cb30 */ 	swc1	$f0,-0x34d0($at)
/*     a04c:	91220000 */ 	lbu	$v0,0x0($t1)
/*     a050:	57e200a3 */ 	bnel	$ra,$v0,.PF0000a2e0
/*     a054:	24010002 */ 	li	$at,0x2
/*     a058:	3c0a8006 */ 	lui	$t2,0x8006
/*     a05c:	8d4ad238 */ 	lw	$t2,-0x2dc8($t2)
/*     a060:	3c1f8009 */ 	lui	$ra,0x8009
/*     a064:	000c7880 */ 	sll	$t7,$t4,0x2
/*     a068:	11400019 */ 	beqz	$t2,.PF0000a0d0
/*     a06c:	27ffe360 */ 	addiu	$ra,$ra,-7328
/*     a070:	000cc080 */ 	sll	$t8,$t4,0x2
/*     a074:	030cc021 */ 	addu	$t8,$t8,$t4
/*     a078:	3c198009 */ 	lui	$t9,0x8009
/*     a07c:	2739e210 */ 	addiu	$t9,$t9,-7664
/*     a080:	0018c100 */ 	sll	$t8,$t8,0x4
/*     a084:	03191021 */ 	addu	$v0,$t8,$t9
/*     a088:	3c0e8006 */ 	lui	$t6,0x8006
/*     a08c:	25cef8c0 */ 	addiu	$t6,$t6,-1856
/*     a090:	25d80048 */ 	addiu	$t8,$t6,0x48
/*     a094:	0040c825 */ 	move	$t9,$v0
.PF0000a098:
/*     a098:	8dc10000 */ 	lw	$at,0x0($t6)
/*     a09c:	25ce000c */ 	addiu	$t6,$t6,0xc
/*     a0a0:	2739000c */ 	addiu	$t9,$t9,0xc
/*     a0a4:	af21fff4 */ 	sw	$at,-0xc($t9)
/*     a0a8:	8dc1fff8 */ 	lw	$at,-0x8($t6)
/*     a0ac:	af21fff8 */ 	sw	$at,-0x8($t9)
/*     a0b0:	8dc1fffc */ 	lw	$at,-0x4($t6)
/*     a0b4:	15d8fff8 */ 	bne	$t6,$t8,.PF0000a098
/*     a0b8:	af21fffc */ 	sw	$at,-0x4($t9)
/*     a0bc:	8dc10000 */ 	lw	$at,0x0($t6)
/*     a0c0:	af210000 */ 	sw	$at,0x0($t9)
/*     a0c4:	8dd80004 */ 	lw	$t8,0x4($t6)
/*     a0c8:	10000017 */ 	b	.PF0000a128
/*     a0cc:	af380004 */ 	sw	$t8,0x4($t9)
.PF0000a0d0:
/*     a0d0:	01ec7821 */ 	addu	$t7,$t7,$t4
/*     a0d4:	3c188009 */ 	lui	$t8,0x8009
/*     a0d8:	2718e210 */ 	addiu	$t8,$t8,-7664
/*     a0dc:	000f7900 */ 	sll	$t7,$t7,0x4
/*     a0e0:	01f81021 */ 	addu	$v0,$t7,$t8
/*     a0e4:	3c0e8006 */ 	lui	$t6,0x8006
/*     a0e8:	25cefa00 */ 	addiu	$t6,$t6,-1536
/*     a0ec:	25cf0048 */ 	addiu	$t7,$t6,0x48
/*     a0f0:	0040c025 */ 	move	$t8,$v0
.PF0000a0f4:
/*     a0f4:	8dc10000 */ 	lw	$at,0x0($t6)
/*     a0f8:	25ce000c */ 	addiu	$t6,$t6,0xc
/*     a0fc:	2718000c */ 	addiu	$t8,$t8,0xc
/*     a100:	af01fff4 */ 	sw	$at,-0xc($t8)
/*     a104:	8dc1fff8 */ 	lw	$at,-0x8($t6)
/*     a108:	af01fff8 */ 	sw	$at,-0x8($t8)
/*     a10c:	8dc1fffc */ 	lw	$at,-0x4($t6)
/*     a110:	15cffff8 */ 	bne	$t6,$t7,.PF0000a0f4
/*     a114:	af01fffc */ 	sw	$at,-0x4($t8)
/*     a118:	8dc10000 */ 	lw	$at,0x0($t6)
/*     a11c:	af010000 */ 	sw	$at,0x0($t8)
/*     a120:	8dcf0004 */ 	lw	$t7,0x4($t6)
/*     a124:	af0f0004 */ 	sw	$t7,0x4($t8)
.PF0000a128:
/*     a128:	85390018 */ 	lh	$t9,0x18($t1)
/*     a12c:	24010280 */ 	li	$at,0x280
/*     a130:	24080400 */ 	li	$t0,0x400
/*     a134:	ac590008 */ 	sw	$t9,0x8($v0)
/*     a138:	852f0018 */ 	lh	$t7,0x18($t1)
/*     a13c:	8c44001c */ 	lw	$a0,0x1c($v0)
/*     a140:	3c078006 */ 	lui	$a3,0x8006
/*     a144:	000f7280 */ 	sll	$t6,$t7,0xa
/*     a148:	01c1001a */ 	div	$zero,$t6,$at
/*     a14c:	0000c012 */ 	mflo	$t8
/*     a150:	ac580020 */ 	sw	$t8,0x20($v0)
/*     a154:	85390018 */ 	lh	$t9,0x18($t1)
/*     a158:	3406ffff */ 	li	$a2,0xffff
/*     a15c:	00197840 */ 	sll	$t7,$t9,0x1
/*     a160:	ac4f0028 */ 	sw	$t7,0x28($v0)
/*     a164:	852e0018 */ 	lh	$t6,0x18($t1)
/*     a168:	ac48002c */ 	sw	$t0,0x2c($v0)
/*     a16c:	ac480040 */ 	sw	$t0,0x40($v0)
/*     a170:	000ec040 */ 	sll	$t8,$t6,0x1
/*     a174:	ac58003c */ 	sw	$t8,0x3c($v0)
/*     a178:	8ce7d228 */ 	lw	$a3,-0x2dd8($a3)
/*     a17c:	0004cc03 */ 	sra	$t9,$a0,0x10
/*     a180:	332fffff */ 	andi	$t7,$t9,0xffff
/*     a184:	01e77021 */ 	addu	$t6,$t7,$a3
/*     a188:	01c6001a */ 	div	$zero,$t6,$a2
/*     a18c:	14c00002 */ 	bnez	$a2,.PF0000a198
/*     a190:	00000000 */ 	nop
/*     a194:	0007000d */ 	break	0x7
.PF0000a198:
/*     a198:	2401ffff */ 	li	$at,-1
/*     a19c:	14c10004 */ 	bne	$a2,$at,.PF0000a1b0
/*     a1a0:	3c018000 */ 	lui	$at,0x8000
/*     a1a4:	15c10002 */ 	bne	$t6,$at,.PF0000a1b0
/*     a1a8:	00000000 */ 	nop
/*     a1ac:	0006000d */ 	break	0x6
.PF0000a1b0:
/*     a1b0:	308fffff */ 	andi	$t7,$a0,0xffff
/*     a1b4:	0000c010 */ 	mfhi	$t8
/*     a1b8:	01e77021 */ 	addu	$t6,$t7,$a3
/*     a1bc:	0018cc00 */ 	sll	$t9,$t8,0x10
/*     a1c0:	01c6001a */ 	div	$zero,$t6,$a2
/*     a1c4:	14c00002 */ 	bnez	$a2,.PF0000a1d0
/*     a1c8:	00000000 */ 	nop
/*     a1cc:	0007000d */ 	break	0x7
.PF0000a1d0:
/*     a1d0:	2401ffff */ 	li	$at,-1
/*     a1d4:	14c10004 */ 	bne	$a2,$at,.PF0000a1e8
/*     a1d8:	3c018000 */ 	lui	$at,0x8000
/*     a1dc:	15c10002 */ 	bne	$t6,$at,.PF0000a1e8
/*     a1e0:	00000000 */ 	nop
/*     a1e4:	0006000d */ 	break	0x6
.PF0000a1e8:
/*     a1e8:	0000c010 */ 	mfhi	$t8
/*     a1ec:	03381825 */ 	or	$v1,$t9,$t8
/*     a1f0:	ac43001c */ 	sw	$v1,0x1c($v0)
/*     a1f4:	3c018009 */ 	lui	$at,0x8009
/*     a1f8:	ac23e358 */ 	sw	$v1,-0x1ca8($at)
/*     a1fc:	8525001a */ 	lh	$a1,0x1a($t1)
/*     a200:	24180140 */ 	li	$t8,0x140
/*     a204:	3c048006 */ 	lui	$a0,0x8006
/*     a208:	00057a80 */ 	sll	$t7,$a1,0xa
/*     a20c:	000f7282 */ 	srl	$t6,$t7,0xa
/*     a210:	29c1012d */ 	slti	$at,$t6,0x12d
/*     a214:	14200002 */ 	bnez	$at,.PF0000a220
/*     a218:	01c02825 */ 	move	$a1,$t6
/*     a21c:	000e2843 */ 	sra	$a1,$t6,0x1
.PF0000a220:
/*     a220:	03053823 */ 	subu	$a3,$t8,$a1
/*     a224:	24ef0002 */ 	addiu	$t7,$a3,0x2
/*     a228:	24b9fffe */ 	addiu	$t9,$a1,-2
/*     a22c:	0019c040 */ 	sll	$t8,$t9,0x1
/*     a230:	000f7400 */ 	sll	$t6,$t7,0x10
/*     a234:	00f87821 */ 	addu	$t7,$a3,$t8
/*     a238:	25f90002 */ 	addiu	$t9,$t7,0x2
/*     a23c:	8c84d22c */ 	lw	$a0,-0x2dd4($a0)
/*     a240:	01d94025 */ 	or	$t0,$t6,$t9
/*     a244:	0008c403 */ 	sra	$t8,$t0,0x10
/*     a248:	330fffff */ 	andi	$t7,$t8,0xffff
/*     a24c:	01e47021 */ 	addu	$t6,$t7,$a0
/*     a250:	01c6001a */ 	div	$zero,$t6,$a2
/*     a254:	14c00002 */ 	bnez	$a2,.PF0000a260
/*     a258:	00000000 */ 	nop
/*     a25c:	0007000d */ 	break	0x7
.PF0000a260:
/*     a260:	2401ffff */ 	li	$at,-1
/*     a264:	14c10004 */ 	bne	$a2,$at,.PF0000a278
/*     a268:	3c018000 */ 	lui	$at,0x8000
/*     a26c:	15c10002 */ 	bne	$t6,$at,.PF0000a278
/*     a270:	00000000 */ 	nop
/*     a274:	0006000d */ 	break	0x6
.PF0000a278:
/*     a278:	310fffff */ 	andi	$t7,$t0,0xffff
/*     a27c:	01e47021 */ 	addu	$t6,$t7,$a0
/*     a280:	0000c810 */ 	mfhi	$t9
/*     a284:	0019c400 */ 	sll	$t8,$t9,0x10
/*     a288:	240f0001 */ 	li	$t7,0x1
/*     a28c:	01c6001a */ 	div	$zero,$t6,$a2
/*     a290:	14c00002 */ 	bnez	$a2,.PF0000a29c
/*     a294:	00000000 */ 	nop
/*     a298:	0007000d */ 	break	0x7
.PF0000a29c:
/*     a29c:	2401ffff */ 	li	$at,-1
/*     a2a0:	14c10004 */ 	bne	$a2,$at,.PF0000a2b4
/*     a2a4:	3c018000 */ 	lui	$at,0x8000
/*     a2a8:	15c10002 */ 	bne	$t6,$at,.PF0000a2b4
/*     a2ac:	00000000 */ 	nop
/*     a2b0:	0006000d */ 	break	0x6
.PF0000a2b4:
/*     a2b4:	0000c810 */ 	mfhi	$t9
/*     a2b8:	03191825 */ 	or	$v1,$t8,$t9
/*     a2bc:	3c018006 */ 	lui	$at,0x8006
/*     a2c0:	ac430030 */ 	sw	$v1,0x30($v0)
/*     a2c4:	ada30000 */ 	sw	$v1,0x0($t5)
/*     a2c8:	ac430044 */ 	sw	$v1,0x44($v0)
/*     a2cc:	afe30000 */ 	sw	$v1,0x0($ra)
/*     a2d0:	002b0821 */ 	addu	$at,$at,$t3
/*     a2d4:	100000dd */ 	b	.PF0000a64c
/*     a2d8:	ac2fcb38 */ 	sw	$t7,-0x34c8($at)
/*     a2dc:	24010002 */ 	li	$at,0x2
.PF0000a2e0:
/*     a2e0:	144100d5 */ 	bne	$v0,$at,.PF0000a638
/*     a2e4:	000c7080 */ 	sll	$t6,$t4,0x2
/*     a2e8:	01cc7021 */ 	addu	$t6,$t6,$t4
/*     a2ec:	3c188009 */ 	lui	$t8,0x8009
/*     a2f0:	2718e210 */ 	addiu	$t8,$t8,-7664
/*     a2f4:	000e7100 */ 	sll	$t6,$t6,0x4
/*     a2f8:	01d81021 */ 	addu	$v0,$t6,$t8
/*     a2fc:	3c198006 */ 	lui	$t9,0x8006
/*     a300:	3c0d8009 */ 	lui	$t5,0x8009
/*     a304:	2739fb90 */ 	addiu	$t9,$t9,-1136
/*     a308:	25ade35c */ 	addiu	$t5,$t5,-7332
/*     a30c:	3406ffff */ 	li	$a2,0xffff
/*     a310:	24080800 */ 	li	$t0,0x800
/*     a314:	272e0048 */ 	addiu	$t6,$t9,0x48
/*     a318:	0040c025 */ 	move	$t8,$v0
.PF0000a31c:
/*     a31c:	8f210000 */ 	lw	$at,0x0($t9)
/*     a320:	2739000c */ 	addiu	$t9,$t9,0xc
/*     a324:	2718000c */ 	addiu	$t8,$t8,0xc
/*     a328:	af01fff4 */ 	sw	$at,-0xc($t8)
/*     a32c:	8f21fff8 */ 	lw	$at,-0x8($t9)
/*     a330:	af01fff8 */ 	sw	$at,-0x8($t8)
/*     a334:	8f21fffc */ 	lw	$at,-0x4($t9)
/*     a338:	172efff8 */ 	bne	$t9,$t6,.PF0000a31c
/*     a33c:	af01fffc */ 	sw	$at,-0x4($t8)
/*     a340:	8f210000 */ 	lw	$at,0x0($t9)
/*     a344:	3c078006 */ 	lui	$a3,0x8006
/*     a348:	3c048006 */ 	lui	$a0,0x8006
/*     a34c:	af010000 */ 	sw	$at,0x0($t8)
/*     a350:	8f2e0004 */ 	lw	$t6,0x4($t9)
/*     a354:	24010280 */ 	li	$at,0x280
/*     a358:	3c1f8009 */ 	lui	$ra,0x8009
/*     a35c:	af0e0004 */ 	sw	$t6,0x4($t8)
/*     a360:	852f0018 */ 	lh	$t7,0x18($t1)
/*     a364:	8c45001c */ 	lw	$a1,0x1c($v0)
/*     a368:	27ffe360 */ 	addiu	$ra,$ra,-7328
/*     a36c:	ac4f0008 */ 	sw	$t7,0x8($v0)
/*     a370:	852e0018 */ 	lh	$t6,0x18($t1)
/*     a374:	ac48002c */ 	sw	$t0,0x2c($v0)
/*     a378:	ac480040 */ 	sw	$t0,0x40($v0)
/*     a37c:	000eca80 */ 	sll	$t9,$t6,0xa
/*     a380:	0321001a */ 	div	$zero,$t9,$at
/*     a384:	0000c012 */ 	mflo	$t8
/*     a388:	ac580020 */ 	sw	$t8,0x20($v0)
/*     a38c:	852f0018 */ 	lh	$t7,0x18($t1)
/*     a390:	3c0a8006 */ 	lui	$t2,0x8006
/*     a394:	000f7040 */ 	sll	$t6,$t7,0x1
/*     a398:	ac4e0028 */ 	sw	$t6,0x28($v0)
/*     a39c:	85390018 */ 	lh	$t9,0x18($t1)
/*     a3a0:	00057c03 */ 	sra	$t7,$a1,0x10
/*     a3a4:	31eeffff */ 	andi	$t6,$t7,0xffff
/*     a3a8:	0019c080 */ 	sll	$t8,$t9,0x2
/*     a3ac:	ac58003c */ 	sw	$t8,0x3c($v0)
/*     a3b0:	8ce7d228 */ 	lw	$a3,-0x2dd8($a3)
/*     a3b4:	01c7c821 */ 	addu	$t9,$t6,$a3
/*     a3b8:	0326001a */ 	div	$zero,$t9,$a2
/*     a3bc:	14c00002 */ 	bnez	$a2,.PF0000a3c8
/*     a3c0:	00000000 */ 	nop
/*     a3c4:	0007000d */ 	break	0x7
.PF0000a3c8:
/*     a3c8:	2401ffff */ 	li	$at,-1
/*     a3cc:	14c10004 */ 	bne	$a2,$at,.PF0000a3e0
/*     a3d0:	3c018000 */ 	lui	$at,0x8000
/*     a3d4:	17210002 */ 	bne	$t9,$at,.PF0000a3e0
/*     a3d8:	00000000 */ 	nop
/*     a3dc:	0006000d */ 	break	0x6
.PF0000a3e0:
/*     a3e0:	30aeffff */ 	andi	$t6,$a1,0xffff
/*     a3e4:	0000c010 */ 	mfhi	$t8
/*     a3e8:	01c7c821 */ 	addu	$t9,$t6,$a3
/*     a3ec:	00187c00 */ 	sll	$t7,$t8,0x10
/*     a3f0:	0326001a */ 	div	$zero,$t9,$a2
/*     a3f4:	14c00002 */ 	bnez	$a2,.PF0000a400
/*     a3f8:	00000000 */ 	nop
/*     a3fc:	0007000d */ 	break	0x7
.PF0000a400:
/*     a400:	2401ffff */ 	li	$at,-1
/*     a404:	14c10004 */ 	bne	$a2,$at,.PF0000a418
/*     a408:	3c018000 */ 	lui	$at,0x8000
/*     a40c:	17210002 */ 	bne	$t9,$at,.PF0000a418
/*     a410:	00000000 */ 	nop
/*     a414:	0006000d */ 	break	0x6
.PF0000a418:
/*     a418:	0000c010 */ 	mfhi	$t8
/*     a41c:	01f81825 */ 	or	$v1,$t7,$t8
/*     a420:	ac43001c */ 	sw	$v1,0x1c($v0)
/*     a424:	3c018009 */ 	lui	$at,0x8009
/*     a428:	ac23e358 */ 	sw	$v1,-0x1ca8($at)
/*     a42c:	8c450030 */ 	lw	$a1,0x30($v0)
/*     a430:	8c84d22c */ 	lw	$a0,-0x2dd4($a0)
/*     a434:	00057403 */ 	sra	$t6,$a1,0x10
/*     a438:	31d9ffff */ 	andi	$t9,$t6,0xffff
/*     a43c:	03247821 */ 	addu	$t7,$t9,$a0
/*     a440:	01e6001a */ 	div	$zero,$t7,$a2
/*     a444:	14c00002 */ 	bnez	$a2,.PF0000a450
/*     a448:	00000000 */ 	nop
/*     a44c:	0007000d */ 	break	0x7
.PF0000a450:
/*     a450:	2401ffff */ 	li	$at,-1
/*     a454:	14c10004 */ 	bne	$a2,$at,.PF0000a468
/*     a458:	3c018000 */ 	lui	$at,0x8000
/*     a45c:	15e10002 */ 	bne	$t7,$at,.PF0000a468
/*     a460:	00000000 */ 	nop
/*     a464:	0006000d */ 	break	0x6
.PF0000a468:
/*     a468:	30b9ffff */ 	andi	$t9,$a1,0xffff
/*     a46c:	03247821 */ 	addu	$t7,$t9,$a0
/*     a470:	0000c010 */ 	mfhi	$t8
/*     a474:	8c450044 */ 	lw	$a1,0x44($v0)
/*     a478:	00187400 */ 	sll	$t6,$t8,0x10
/*     a47c:	01e6001a */ 	div	$zero,$t7,$a2
/*     a480:	14c00002 */ 	bnez	$a2,.PF0000a48c
/*     a484:	00000000 */ 	nop
/*     a488:	0007000d */ 	break	0x7
.PF0000a48c:
/*     a48c:	2401ffff */ 	li	$at,-1
/*     a490:	14c10004 */ 	bne	$a2,$at,.PF0000a4a4
/*     a494:	3c018000 */ 	lui	$at,0x8000
/*     a498:	15e10002 */ 	bne	$t7,$at,.PF0000a4a4
/*     a49c:	00000000 */ 	nop
/*     a4a0:	0006000d */ 	break	0x6
.PF0000a4a4:
/*     a4a4:	0000c010 */ 	mfhi	$t8
/*     a4a8:	0005cc03 */ 	sra	$t9,$a1,0x10
/*     a4ac:	01d81825 */ 	or	$v1,$t6,$t8
/*     a4b0:	332fffff */ 	andi	$t7,$t9,0xffff
/*     a4b4:	01e47021 */ 	addu	$t6,$t7,$a0
/*     a4b8:	01c6001a */ 	div	$zero,$t6,$a2
/*     a4bc:	30afffff */ 	andi	$t7,$a1,0xffff
/*     a4c0:	0000c010 */ 	mfhi	$t8
/*     a4c4:	0018cc00 */ 	sll	$t9,$t8,0x10
/*     a4c8:	ac430030 */ 	sw	$v1,0x30($v0)
/*     a4cc:	ada30000 */ 	sw	$v1,0x0($t5)
/*     a4d0:	14c00002 */ 	bnez	$a2,.PF0000a4dc
/*     a4d4:	00000000 */ 	nop
/*     a4d8:	0007000d */ 	break	0x7
.PF0000a4dc:
/*     a4dc:	2401ffff */ 	li	$at,-1
/*     a4e0:	14c10004 */ 	bne	$a2,$at,.PF0000a4f4
/*     a4e4:	3c018000 */ 	lui	$at,0x8000
/*     a4e8:	15c10002 */ 	bne	$t6,$at,.PF0000a4f4
/*     a4ec:	00000000 */ 	nop
/*     a4f0:	0006000d */ 	break	0x6
.PF0000a4f4:
/*     a4f4:	01e47021 */ 	addu	$t6,$t7,$a0
/*     a4f8:	01c6001a */ 	div	$zero,$t6,$a2
/*     a4fc:	0000c010 */ 	mfhi	$t8
/*     a500:	03381825 */ 	or	$v1,$t9,$t8
/*     a504:	ac430044 */ 	sw	$v1,0x44($v0)
/*     a508:	afe30000 */ 	sw	$v1,0x0($ra)
/*     a50c:	3c0f8006 */ 	lui	$t7,0x8006
/*     a510:	8defd9b8 */ 	lw	$t7,-0x2648($t7)
/*     a514:	14c00002 */ 	bnez	$a2,.PF0000a520
/*     a518:	00000000 */ 	nop
/*     a51c:	0007000d */ 	break	0x7
.PF0000a520:
/*     a520:	2401ffff */ 	li	$at,-1
/*     a524:	14c10004 */ 	bne	$a2,$at,.PF0000a538
/*     a528:	3c018000 */ 	lui	$at,0x8000
/*     a52c:	15c10002 */ 	bne	$t6,$at,.PF0000a538
/*     a530:	00000000 */ 	nop
/*     a534:	0006000d */ 	break	0x6
.PF0000a538:
/*     a538:	248e01fa */ 	addiu	$t6,$a0,0x1fa
/*     a53c:	11e00038 */ 	beqz	$t7,.PF0000a620
/*     a540:	00000000 */ 	nop
/*     a544:	01c6001a */ 	div	$zero,$t6,$a2
/*     a548:	0000c810 */ 	mfhi	$t9
/*     a54c:	248f0086 */ 	addiu	$t7,$a0,0x86
/*     a550:	14c00002 */ 	bnez	$a2,.PF0000a55c
/*     a554:	00000000 */ 	nop
/*     a558:	0007000d */ 	break	0x7
.PF0000a55c:
/*     a55c:	2401ffff */ 	li	$at,-1
/*     a560:	14c10004 */ 	bne	$a2,$at,.PF0000a574
/*     a564:	3c018000 */ 	lui	$at,0x8000
/*     a568:	15c10002 */ 	bne	$t6,$at,.PF0000a574
/*     a56c:	00000000 */ 	nop
/*     a570:	0006000d */ 	break	0x6
.PF0000a574:
/*     a574:	01e6001a */ 	div	$zero,$t7,$a2
/*     a578:	00007010 */ 	mfhi	$t6
/*     a57c:	0019c400 */ 	sll	$t8,$t9,0x10
/*     a580:	249901fc */ 	addiu	$t9,$a0,0x1fc
/*     a584:	0326001a */ 	div	$zero,$t9,$a2
/*     a588:	030e1825 */ 	or	$v1,$t8,$t6
/*     a58c:	248e0084 */ 	addiu	$t6,$a0,0x84
/*     a590:	14c00002 */ 	bnez	$a2,.PF0000a59c
/*     a594:	00000000 */ 	nop
/*     a598:	0007000d */ 	break	0x7
.PF0000a59c:
/*     a59c:	2401ffff */ 	li	$at,-1
/*     a5a0:	14c10004 */ 	bne	$a2,$at,.PF0000a5b4
/*     a5a4:	3c018000 */ 	lui	$at,0x8000
/*     a5a8:	15e10002 */ 	bne	$t7,$at,.PF0000a5b4
/*     a5ac:	00000000 */ 	nop
/*     a5b0:	0006000d */ 	break	0x6
.PF0000a5b4:
/*     a5b4:	00007810 */ 	mfhi	$t7
/*     a5b8:	ac430030 */ 	sw	$v1,0x30($v0)
/*     a5bc:	ada30000 */ 	sw	$v1,0x0($t5)
/*     a5c0:	01c6001a */ 	div	$zero,$t6,$a2
/*     a5c4:	14c00002 */ 	bnez	$a2,.PF0000a5d0
/*     a5c8:	00000000 */ 	nop
/*     a5cc:	0007000d */ 	break	0x7
.PF0000a5d0:
/*     a5d0:	2401ffff */ 	li	$at,-1
/*     a5d4:	14c10004 */ 	bne	$a2,$at,.PF0000a5e8
/*     a5d8:	3c018000 */ 	lui	$at,0x8000
/*     a5dc:	17210002 */ 	bne	$t9,$at,.PF0000a5e8
/*     a5e0:	00000000 */ 	nop
/*     a5e4:	0006000d */ 	break	0x6
.PF0000a5e8:
/*     a5e8:	0000c810 */ 	mfhi	$t9
/*     a5ec:	000fc400 */ 	sll	$t8,$t7,0x10
/*     a5f0:	03191825 */ 	or	$v1,$t8,$t9
/*     a5f4:	ac430044 */ 	sw	$v1,0x44($v0)
/*     a5f8:	afe30000 */ 	sw	$v1,0x0($ra)
/*     a5fc:	14c00002 */ 	bnez	$a2,.PF0000a608
/*     a600:	00000000 */ 	nop
/*     a604:	0007000d */ 	break	0x7
.PF0000a608:
/*     a608:	2401ffff */ 	li	$at,-1
/*     a60c:	14c10004 */ 	bne	$a2,$at,.PF0000a620
/*     a610:	3c018000 */ 	lui	$at,0x8000
/*     a614:	15c10002 */ 	bne	$t6,$at,.PF0000a620
/*     a618:	00000000 */ 	nop
/*     a61c:	0006000d */ 	break	0x6
.PF0000a620:
/*     a620:	3c018006 */ 	lui	$at,0x8006
/*     a624:	002b0821 */ 	addu	$at,$at,$t3
/*     a628:	240f0001 */ 	li	$t7,0x1
/*     a62c:	ac2fcb38 */ 	sw	$t7,-0x34c8($at)
/*     a630:	10000006 */ 	b	.PF0000a64c
/*     a634:	8d4ad238 */ 	lw	$t2,-0x2dc8($t2)
.PF0000a638:
/*     a638:	3c018006 */ 	lui	$at,0x8006
/*     a63c:	002b0821 */ 	addu	$at,$at,$t3
/*     a640:	3c0a8006 */ 	lui	$t2,0x8006
/*     a644:	8d4ad238 */ 	lw	$t2,-0x2dc8($t2)
/*     a648:	ac20cb38 */ 	sw	$zero,-0x34c8($at)
.PF0000a64c:
/*     a64c:	258c0001 */ 	addiu	$t4,$t4,0x1
/*     a650:	05810004 */ 	bgez	$t4,.PF0000a664
/*     a654:	318e0001 */ 	andi	$t6,$t4,0x1
/*     a658:	11c00002 */ 	beqz	$t6,.PF0000a664
/*     a65c:	00000000 */ 	nop
/*     a660:	25cefffe */ 	addiu	$t6,$t6,-2
.PF0000a664:
/*     a664:	3c018006 */ 	lui	$at,0x8006
/*     a668:	11400006 */ 	beqz	$t2,.PF0000a684
/*     a66c:	ac2ed240 */ 	sw	$t6,-0x2dc0($at)
/*     a670:	3c198006 */ 	lui	$t9,0x8006
/*     a674:	8f39edb8 */ 	lw	$t9,-0x1248($t9)
/*     a678:	8d380028 */ 	lw	$t8,0x28($t1)
/*     a67c:	10000006 */ 	b	.PF0000a698
/*     a680:	af380058 */ 	sw	$t8,0x58($t9)
.PF0000a684:
/*     a684:	3c0f800a */ 	lui	$t7,0x800a
/*     a688:	3c0e8006 */ 	lui	$t6,0x8006
/*     a68c:	8dceedb8 */ 	lw	$t6,-0x1248($t6)
/*     a690:	8defd020 */ 	lw	$t7,-0x2fe0($t7)
/*     a694:	adcf0058 */ 	sw	$t7,0x58($t6)
.PF0000a698:
/*     a698:	3c028009 */ 	lui	$v0,0x8009
/*     a69c:	24422dc6 */ 	addiu	$v0,$v0,0x2dc6
/*     a6a0:	90580000 */ 	lbu	$t8,0x0($v0)
/*     a6a4:	3c048006 */ 	lui	$a0,0x8006
/*     a6a8:	8c84d234 */ 	lw	$a0,-0x2dcc($a0)
/*     a6ac:	27190001 */ 	addiu	$t9,$t8,0x1
/*     a6b0:	3c038009 */ 	lui	$v1,0x8009
/*     a6b4:	2408002c */ 	li	$t0,0x2c
/*     a6b8:	24632dc7 */ 	addiu	$v1,$v1,0x2dc7
/*     a6bc:	906e0000 */ 	lbu	$t6,0x0($v1)
/*     a6c0:	3c078006 */ 	lui	$a3,0x8006
/*     a6c4:	24e7d1d0 */ 	addiu	$a3,$a3,-11824
/*     a6c8:	07210004 */ 	bgez	$t9,.PF0000a6dc
/*     a6cc:	332f0001 */ 	andi	$t7,$t9,0x1
/*     a6d0:	11e00002 */ 	beqz	$t7,.PF0000a6dc
/*     a6d4:	00000000 */ 	nop
/*     a6d8:	25effffe */ 	addiu	$t7,$t7,-2
.PF0000a6dc:
/*     a6dc:	a04f0000 */ 	sb	$t7,0x0($v0)
/*     a6e0:	904f0000 */ 	lbu	$t7,0x0($v0)
/*     a6e4:	25d80001 */ 	addiu	$t8,$t6,0x1
/*     a6e8:	07010004 */ 	bgez	$t8,.PF0000a6fc
/*     a6ec:	33190001 */ 	andi	$t9,$t8,0x1
/*     a6f0:	13200002 */ 	beqz	$t9,.PF0000a6fc
/*     a6f4:	00000000 */ 	nop
/*     a6f8:	2739fffe */ 	addiu	$t9,$t9,-2
.PF0000a6fc:
/*     a6fc:	01e80019 */ 	multu	$t7,$t0
/*     a700:	a0790000 */ 	sb	$t9,0x0($v1)
/*     a704:	3c018006 */ 	lui	$at,0x8006
/*     a708:	3c058006 */ 	lui	$a1,0x8006
/*     a70c:	2406002c */ 	li	$a2,0x2c
/*     a710:	00007012 */ 	mflo	$t6
/*     a714:	00eec021 */ 	addu	$t8,$a3,$t6
/*     a718:	ac38d230 */ 	sw	$t8,-0x2dd0($at)
/*     a71c:	90790000 */ 	lbu	$t9,0x0($v1)
/*     a720:	3c018006 */ 	lui	$at,0x8006
/*     a724:	03280019 */ 	multu	$t9,$t0
/*     a728:	00007812 */ 	mflo	$t7
/*     a72c:	00ef7021 */ 	addu	$t6,$a3,$t7
/*     a730:	ac2ed234 */ 	sw	$t6,-0x2dcc($at)
/*     a734:	0c0129ec */ 	jal	bcopy
/*     a738:	8ca5d234 */ 	lw	$a1,-0x2dcc($a1)
/*     a73c:	3c038009 */ 	lui	$v1,0x8009
/*     a740:	24632dc7 */ 	addiu	$v1,$v1,0x2dc7
/*     a744:	90780000 */ 	lbu	$t8,0x0($v1)
/*     a748:	3c0f800a */ 	lui	$t7,0x800a
/*     a74c:	3c0e8006 */ 	lui	$t6,0x8006
/*     a750:	0018c880 */ 	sll	$t9,$t8,0x2
/*     a754:	01f97821 */ 	addu	$t7,$t7,$t9
/*     a758:	8defd020 */ 	lw	$t7,-0x2fe0($t7)
/*     a75c:	8dced234 */ 	lw	$t6,-0x2dcc($t6)
/*     a760:	3c028006 */ 	lui	$v0,0x8006
/*     a764:	2442d23c */ 	addiu	$v0,$v0,-11716
/*     a768:	adcf0028 */ 	sw	$t7,0x28($t6)
/*     a76c:	8c580000 */ 	lw	$t8,0x0($v0)
/*     a770:	53000005 */ 	beqzl	$t8,.PF0000a788
/*     a774:	8fbf0014 */ 	lw	$ra,0x14($sp)
/*     a778:	ac400000 */ 	sw	$zero,0x0($v0)
/*     a77c:	0c002779 */ 	jal	viBlack
/*     a780:	00002025 */ 	move	$a0,$zero
/*     a784:	8fbf0014 */ 	lw	$ra,0x14($sp)
.PF0000a788:
/*     a788:	27bd0018 */ 	addiu	$sp,$sp,0x18
/*     a78c:	03e00008 */ 	jr	$ra
/*     a790:	00000000 */ 	nop
);
#else
GLOBAL_ASM(
glabel vi0000a044
/*     a044:	3c098006 */ 	lui	$t1,%hi(g_ViData)
/*     a048:	3c0e8006 */ 	lui	$t6,%hi(var8005d590)
/*     a04c:	8dced590 */ 	lw	$t6,%lo(var8005d590)($t6)
/*     a050:	8d29d594 */ 	lw	$t1,%lo(g_ViData)($t1)
/*     a054:	27bdffe8 */ 	addiu	$sp,$sp,-24
/*     a058:	afbf0014 */ 	sw	$ra,0x14($sp)
/*     a05c:	91cf0000 */ 	lbu	$t7,0x0($t6)
/*     a060:	91230000 */ 	lbu	$v1,0x0($t1)
/*     a064:	506f0014 */ 	beql	$v1,$t7,.L0000a0b8
/*     a068:	85380004 */ 	lh	$t8,0x4($t1)
/*     a06c:	10600008 */ 	beqz	$v1,.L0000a090
/*     a070:	00601025 */ 	or	$v0,$v1,$zero
/*     a074:	241f0001 */ 	addiu	$ra,$zero,0x1
/*     a078:	107f000e */ 	beq	$v1,$ra,.L0000a0b4
/*     a07c:	24040002 */ 	addiu	$a0,$zero,0x2
/*     a080:	5044000d */ 	beql	$v0,$a0,.L0000a0b8
/*     a084:	85380004 */ 	lh	$t8,0x4($t1)
/*     a088:	1000000b */ 	b	.L0000a0b8
/*     a08c:	85380004 */ 	lh	$t8,0x4($t1)
.L0000a090:
/*     a090:	3c013f80 */ 	lui	$at,0x3f80
/*     a094:	44816000 */ 	mtc1	$at,$f12
/*     a098:	0c0123bc */ 	jal	osViSetYScale
/*     a09c:	00000000 */ 	nop
/*     a0a0:	0c012338 */ 	jal	osViBlack
/*     a0a4:	24040001 */ 	addiu	$a0,$zero,0x1
/*     a0a8:	3c098006 */ 	lui	$t1,%hi(g_ViData)
/*     a0ac:	8d29d594 */ 	lw	$t1,%lo(g_ViData)($t1)
/*     a0b0:	91230000 */ 	lbu	$v1,0x0($t1)
.L0000a0b4:
/*     a0b4:	85380004 */ 	lh	$t8,0x4($t1)
.L0000a0b8:
/*     a0b8:	85390018 */ 	lh	$t9,0x18($t1)
/*     a0bc:	852e0006 */ 	lh	$t6,0x6($t1)
/*     a0c0:	44982000 */ 	mtc1	$t8,$f4
/*     a0c4:	852f001a */ 	lh	$t7,0x1a($t1)
/*     a0c8:	44994000 */ 	mtc1	$t9,$f8
/*     a0cc:	468021a0 */ 	cvt.s.w	$f6,$f4
/*     a0d0:	448e8000 */ 	mtc1	$t6,$f16
/*     a0d4:	448f2000 */ 	mtc1	$t7,$f4
/*     a0d8:	24040002 */ 	addiu	$a0,$zero,0x2
/*     a0dc:	241f0001 */ 	addiu	$ra,$zero,0x1
/*     a0e0:	468042a0 */ 	cvt.s.w	$f10,$f8
/*     a0e4:	3c0b8006 */ 	lui	$t3,%hi(var8005d5a0)
/*     a0e8:	3c013f80 */ 	lui	$at,0x3f80
/*     a0ec:	468084a0 */ 	cvt.s.w	$f18,$f16
/*     a0f0:	46802220 */ 	cvt.s.w	$f8,$f4
/*     a0f4:	460a3083 */ 	div.s	$f2,$f6,$f10
/*     a0f8:	14600003 */ 	bnez	$v1,.L0000a108
/*     a0fc:	46089003 */ 	div.s	$f0,$f18,$f8
/*     a100:	44810000 */ 	mtc1	$at,$f0
/*     a104:	00000000 */ 	nop
.L0000a108:
/*     a108:	8d6bd5a0 */ 	lw	$t3,%lo(var8005d5a0)($t3)
/*     a10c:	3c018006 */ 	lui	$at,%hi(var8005ce78)
/*     a110:	000b6080 */ 	sll	$t4,$t3,0x2
/*     a114:	002c0821 */ 	addu	$at,$at,$t4
/*     a118:	e422ce78 */ 	swc1	$f2,%lo(var8005ce78)($at)
/*     a11c:	3c018006 */ 	lui	$at,%hi(var8005ce80)
/*     a120:	002c0821 */ 	addu	$at,$at,$t4
/*     a124:	e420ce80 */ 	swc1	$f0,%lo(var8005ce80)($at)
/*     a128:	91230000 */ 	lbu	$v1,0x0($t1)
/*     a12c:	17e30101 */ 	bne	$ra,$v1,.L0000a534
/*     a130:	00000000 */ 	nop
/*     a134:	3c0a8006 */ 	lui	$t2,%hi(g_ViIs16Bit)
/*     a138:	8d4ad598 */ 	lw	$t2,%lo(g_ViIs16Bit)($t2)
/*     a13c:	3c188000 */ 	lui	$t8,0x8000
/*     a140:	11400033 */ 	beqz	$t2,.L0000a210
/*     a144:	00000000 */ 	nop
/*     a148:	8f180300 */ 	lw	$t8,0x300($t8)
/*     a14c:	3c198009 */ 	lui	$t9,%hi(var8008dcc0)
/*     a150:	2739dcc0 */ 	addiu	$t9,$t9,%lo(var8008dcc0)
/*     a154:	14980019 */ 	bne	$a0,$t8,.L0000a1bc
/*     a158:	3c0f8006 */ 	lui	$t7,%hi(osViModeTable+0xa0)
/*     a15c:	000bc880 */ 	sll	$t9,$t3,0x2
/*     a160:	032bc821 */ 	addu	$t9,$t9,$t3
/*     a164:	3c0e8009 */ 	lui	$t6,%hi(var8008dcc0)
/*     a168:	25cedcc0 */ 	addiu	$t6,$t6,%lo(var8008dcc0)
/*     a16c:	0019c900 */ 	sll	$t9,$t9,0x4
/*     a170:	032e2021 */ 	addu	$a0,$t9,$t6
/*     a174:	3c0f8006 */ 	lui	$t7,%hi(osViModeTable+0x960)
/*     a178:	25ef0070 */ 	addiu	$t7,$t7,%lo(osViModeTable+0x960)
/*     a17c:	25f90048 */ 	addiu	$t9,$t7,0x48
/*     a180:	00807025 */ 	or	$t6,$a0,$zero
.L0000a184:
/*     a184:	8de10000 */ 	lw	$at,0x0($t7)
/*     a188:	25ef000c */ 	addiu	$t7,$t7,12
/*     a18c:	25ce000c */ 	addiu	$t6,$t6,12
/*     a190:	adc1fff4 */ 	sw	$at,-0xc($t6)
/*     a194:	8de1fff8 */ 	lw	$at,-0x8($t7)
/*     a198:	adc1fff8 */ 	sw	$at,-0x8($t6)
/*     a19c:	8de1fffc */ 	lw	$at,-0x4($t7)
/*     a1a0:	15f9fff8 */ 	bne	$t7,$t9,.L0000a184
/*     a1a4:	adc1fffc */ 	sw	$at,-0x4($t6)
/*     a1a8:	8de10000 */ 	lw	$at,0x0($t7)
/*     a1ac:	adc10000 */ 	sw	$at,0x0($t6)
/*     a1b0:	8df90004 */ 	lw	$t9,0x4($t7)
/*     a1b4:	10000048 */ 	b	.L0000a2d8
/*     a1b8:	add90004 */ 	sw	$t9,0x4($t6)
.L0000a1bc:
/*     a1bc:	000bc080 */ 	sll	$t8,$t3,0x2
/*     a1c0:	030bc021 */ 	addu	$t8,$t8,$t3
/*     a1c4:	0018c100 */ 	sll	$t8,$t8,0x4
/*     a1c8:	03192021 */ 	addu	$a0,$t8,$t9
/*     a1cc:	25eff7b0 */ 	addiu	$t7,$t7,%lo(osViModeTable+0xa0)
/*     a1d0:	25f80048 */ 	addiu	$t8,$t7,0x48
/*     a1d4:	0080c825 */ 	or	$t9,$a0,$zero
.L0000a1d8:
/*     a1d8:	8de10000 */ 	lw	$at,0x0($t7)
/*     a1dc:	25ef000c */ 	addiu	$t7,$t7,0xc
/*     a1e0:	2739000c */ 	addiu	$t9,$t9,0xc
/*     a1e4:	af21fff4 */ 	sw	$at,-0xc($t9)
/*     a1e8:	8de1fff8 */ 	lw	$at,-0x8($t7)
/*     a1ec:	af21fff8 */ 	sw	$at,-0x8($t9)
/*     a1f0:	8de1fffc */ 	lw	$at,-0x4($t7)
/*     a1f4:	15f8fff8 */ 	bne	$t7,$t8,.L0000a1d8
/*     a1f8:	af21fffc */ 	sw	$at,-0x4($t9)
/*     a1fc:	8de10000 */ 	lw	$at,0x0($t7)
/*     a200:	af210000 */ 	sw	$at,0x0($t9)
/*     a204:	8df80004 */ 	lw	$t8,0x4($t7)
/*     a208:	10000033 */ 	b	.L0000a2d8
/*     a20c:	af380004 */ 	sw	$t8,0x4($t9)
.L0000a210:
/*     a210:	3c0e8000 */ 	lui	$t6,0x8000
/*     a214:	8dce0300 */ 	lw	$t6,0x300($t6)
/*     a218:	148e0018 */ 	bne	$a0,$t6,.L0000a27c
/*     a21c:	000bc080 */ 	sll	$t8,$t3,0x2
/*     a220:	030bc021 */ 	addu	$t8,$t8,$t3
/*     a224:	3c0f8009 */ 	lui	$t7,%hi(var8008dcc0)
/*     a228:	25efdcc0 */ 	addiu	$t7,$t7,%lo(var8008dcc0)
/*     a22c:	0018c100 */ 	sll	$t8,$t8,0x4
/*     a230:	030f2021 */ 	addu	$a0,$t8,$t7
/*     a234:	3c198006 */ 	lui	$t9,%hi(osViModeTable+0xaa0)
/*     a238:	273901b0 */ 	addiu	$t9,$t9,%lo(osViModeTable+0xaa0)
/*     a23c:	27380048 */ 	addiu	$t8,$t9,0x48
/*     a240:	00807825 */ 	or	$t7,$a0,$zero
.L0000a244:
/*     a244:	8f210000 */ 	lw	$at,0x0($t9)
/*     a248:	2739000c */ 	addiu	$t9,$t9,0xc
/*     a24c:	25ef000c */ 	addiu	$t7,$t7,0xc
/*     a250:	ade1fff4 */ 	sw	$at,-0xc($t7)
/*     a254:	8f21fff8 */ 	lw	$at,-0x8($t9)
/*     a258:	ade1fff8 */ 	sw	$at,-0x8($t7)
/*     a25c:	8f21fffc */ 	lw	$at,-0x4($t9)
/*     a260:	1738fff8 */ 	bne	$t9,$t8,.L0000a244
/*     a264:	ade1fffc */ 	sw	$at,-0x4($t7)
/*     a268:	8f210000 */ 	lw	$at,0x0($t9)
/*     a26c:	ade10000 */ 	sw	$at,0x0($t7)
/*     a270:	8f380004 */ 	lw	$t8,0x4($t9)
/*     a274:	10000018 */ 	b	.L0000a2d8
/*     a278:	adf80004 */ 	sw	$t8,0x4($t7)
.L0000a27c:
/*     a27c:	000b7080 */ 	sll	$t6,$t3,0x2
/*     a280:	01cb7021 */ 	addu	$t6,$t6,$t3
/*     a284:	3c188009 */ 	lui	$t8,%hi(var8008dcc0)
/*     a288:	2718dcc0 */ 	addiu	$t8,$t8,%lo(var8008dcc0)
/*     a28c:	000e7100 */ 	sll	$t6,$t6,0x4
/*     a290:	01d82021 */ 	addu	$a0,$t6,$t8
/*     a294:	3c198006 */ 	lui	$t9,%hi(osViModeTable+0x1e0)
/*     a298:	2739f8f0 */ 	addiu	$t9,$t9,%lo(osViModeTable+0x1e0)
/*     a29c:	272e0048 */ 	addiu	$t6,$t9,0x48
/*     a2a0:	0080c025 */ 	or	$t8,$a0,$zero
.L0000a2a4:
/*     a2a4:	8f210000 */ 	lw	$at,0x0($t9)
/*     a2a8:	2739000c */ 	addiu	$t9,$t9,0xc
/*     a2ac:	2718000c */ 	addiu	$t8,$t8,0xc
/*     a2b0:	af01fff4 */ 	sw	$at,-0xc($t8)
/*     a2b4:	8f21fff8 */ 	lw	$at,-0x8($t9)
/*     a2b8:	af01fff8 */ 	sw	$at,-0x8($t8)
/*     a2bc:	8f21fffc */ 	lw	$at,-0x4($t9)
/*     a2c0:	172efff8 */ 	bne	$t9,$t6,.L0000a2a4
/*     a2c4:	af01fffc */ 	sw	$at,-0x4($t8)
/*     a2c8:	8f210000 */ 	lw	$at,0x0($t9)
/*     a2cc:	af010000 */ 	sw	$at,0x0($t8)
/*     a2d0:	8f2e0004 */ 	lw	$t6,0x4($t9)
/*     a2d4:	af0e0004 */ 	sw	$t6,0x4($t8)
.L0000a2d8:
/*     a2d8:	852f0018 */ 	lh	$t7,0x18($t1)
/*     a2dc:	24010280 */ 	addiu	$at,$zero,0x280
/*     a2e0:	3c0d8009 */ 	lui	$t5,%hi(var8008de0c)
/*     a2e4:	ac8f0008 */ 	sw	$t7,0x8($a0)
/*     a2e8:	852e0018 */ 	lh	$t6,0x18($t1)
/*     a2ec:	3406ffff */ 	dli	$a2,0xffff
/*     a2f0:	25adde0c */ 	addiu	$t5,$t5,%lo(var8008de0c)
/*     a2f4:	000eca80 */ 	sll	$t9,$t6,0xa
/*     a2f8:	0321001a */ 	div	$zero,$t9,$at
/*     a2fc:	0000c012 */ 	mflo	$t8
/*     a300:	ac980020 */ 	sw	$t8,0x20($a0)
/*     a304:	852f0018 */ 	lh	$t7,0x18($t1)
/*     a308:	000f7040 */ 	sll	$t6,$t7,0x1
/*     a30c:	ac8e0028 */ 	sw	$t6,0x28($a0)
/*     a310:	85390018 */ 	lh	$t9,0x18($t1)
/*     a314:	3c0f8009 */ 	lui	$t7,%hi(g_Is4Mb)
/*     a318:	0019c040 */ 	sll	$t8,$t9,0x1
/*     a31c:	ac98003c */ 	sw	$t8,0x3c($a0)
/*     a320:	91ef0af0 */ 	lbu	$t7,%lo(g_Is4Mb)($t7)
/*     a324:	17ef0004 */ 	bne	$ra,$t7,.L0000a338
/*     a328:	24020400 */ 	addiu	$v0,$zero,0x400
/*     a32c:	ac82002c */ 	sw	$v0,0x2c($a0)
/*     a330:	1000001e */ 	b	.L0000a3ac
/*     a334:	ac820040 */ 	sw	$v0,0x40($a0)
.L0000a338:
/*     a338:	852e001a */ 	lh	$t6,0x1a($t1)
/*     a33c:	240201b8 */ 	addiu	$v0,$zero,0x1b8
/*     a340:	000ecac0 */ 	sll	$t9,$t6,0xb
/*     a344:	0322001a */ 	div	$zero,$t9,$v0
/*     a348:	0000c012 */ 	mflo	$t8
/*     a34c:	ac98002c */ 	sw	$t8,0x2c($a0)
/*     a350:	852f001a */ 	lh	$t7,0x1a($t1)
/*     a354:	14400002 */ 	bnez	$v0,.L0000a360
/*     a358:	00000000 */ 	nop
/*     a35c:	0007000d */ 	break	0x7
.L0000a360:
/*     a360:	2401ffff */ 	addiu	$at,$zero,-1
/*     a364:	14410004 */ 	bne	$v0,$at,.L0000a378
/*     a368:	3c018000 */ 	lui	$at,0x8000
/*     a36c:	17210002 */ 	bne	$t9,$at,.L0000a378
/*     a370:	00000000 */ 	nop
/*     a374:	0006000d */ 	break	0x6
.L0000a378:
/*     a378:	000f72c0 */ 	sll	$t6,$t7,0xb
/*     a37c:	01c2001a */ 	div	$zero,$t6,$v0
/*     a380:	0000c812 */ 	mflo	$t9
/*     a384:	ac990040 */ 	sw	$t9,0x40($a0)
/*     a388:	14400002 */ 	bnez	$v0,.L0000a394
/*     a38c:	00000000 */ 	nop
/*     a390:	0007000d */ 	break	0x7
.L0000a394:
/*     a394:	2401ffff */ 	addiu	$at,$zero,-1
/*     a398:	14410004 */ 	bne	$v0,$at,.L0000a3ac
/*     a39c:	3c018000 */ 	lui	$at,0x8000
/*     a3a0:	15c10002 */ 	bne	$t6,$at,.L0000a3ac
/*     a3a4:	00000000 */ 	nop
/*     a3a8:	0006000d */ 	break	0x6
.L0000a3ac:
/*     a3ac:	8c85001c */ 	lw	$a1,0x1c($a0)
/*     a3b0:	3c078006 */ 	lui	$a3,%hi(var8005d588)
/*     a3b4:	8ce7d588 */ 	lw	$a3,%lo(var8005d588)($a3)
/*     a3b8:	0005c403 */ 	sra	$t8,$a1,0x10
/*     a3bc:	330fffff */ 	andi	$t7,$t8,0xffff
/*     a3c0:	01e77021 */ 	addu	$t6,$t7,$a3
/*     a3c4:	01c6001a */ 	div	$zero,$t6,$a2
/*     a3c8:	14c00002 */ 	bnez	$a2,.L0000a3d4
/*     a3cc:	00000000 */ 	nop
/*     a3d0:	0007000d */ 	break	0x7
.L0000a3d4:
/*     a3d4:	2401ffff */ 	addiu	$at,$zero,-1
/*     a3d8:	14c10004 */ 	bne	$a2,$at,.L0000a3ec
/*     a3dc:	3c018000 */ 	lui	$at,0x8000
/*     a3e0:	15c10002 */ 	bne	$t6,$at,.L0000a3ec
/*     a3e4:	00000000 */ 	nop
/*     a3e8:	0006000d */ 	break	0x6
.L0000a3ec:
/*     a3ec:	30afffff */ 	andi	$t7,$a1,0xffff
/*     a3f0:	01e77021 */ 	addu	$t6,$t7,$a3
/*     a3f4:	0000c810 */ 	mfhi	$t9
/*     a3f8:	0019c400 */ 	sll	$t8,$t9,0x10
/*     a3fc:	3c1f8009 */ 	lui	$ra,%hi(var8008de10)
/*     a400:	01c6001a */ 	div	$zero,$t6,$a2
/*     a404:	14c00002 */ 	bnez	$a2,.L0000a410
/*     a408:	00000000 */ 	nop
/*     a40c:	0007000d */ 	break	0x7
.L0000a410:
/*     a410:	2401ffff */ 	addiu	$at,$zero,-1
/*     a414:	14c10004 */ 	bne	$a2,$at,.L0000a428
/*     a418:	3c018000 */ 	lui	$at,0x8000
/*     a41c:	15c10002 */ 	bne	$t6,$at,.L0000a428
/*     a420:	00000000 */ 	nop
/*     a424:	0006000d */ 	break	0x6
.L0000a428:
/*     a428:	0000c810 */ 	mfhi	$t9
/*     a42c:	03191025 */ 	or	$v0,$t8,$t9
/*     a430:	ac82001c */ 	sw	$v0,0x1c($a0)
/*     a434:	3c018009 */ 	lui	$at,%hi(var8008de08)
/*     a438:	ac22de08 */ 	sw	$v0,%lo(var8008de08)($at)
/*     a43c:	8523001a */ 	lh	$v1,0x1a($t1)
/*     a440:	8c8e002c */ 	lw	$t6,0x2c($a0)
/*     a444:	27ffde10 */ 	addiu	$ra,$ra,%lo(var8008de10)
/*     a448:	00037a80 */ 	sll	$t7,$v1,0xa
/*     a44c:	01ee001b */ 	divu	$zero,$t7,$t6
/*     a450:	00001812 */ 	mflo	$v1
/*     a454:	2861012d */ 	slti	$at,$v1,0x12d
/*     a458:	15c00002 */ 	bnez	$t6,.L0000a464
/*     a45c:	00000000 */ 	nop
/*     a460:	0007000d */ 	break	0x7
.L0000a464:
/*     a464:	24190115 */ 	addiu	$t9,$zero,0x115
/*     a468:	14200003 */ 	bnez	$at,.L0000a478
/*     a46c:	3c058006 */ 	lui	$a1,%hi(var8005d58c)
/*     a470:	0003c043 */ 	sra	$t8,$v1,0x1
/*     a474:	03001825 */ 	or	$v1,$t8,$zero
.L0000a478:
/*     a478:	03233823 */ 	subu	$a3,$t9,$v1
/*     a47c:	24ef0002 */ 	addiu	$t7,$a3,0x2
/*     a480:	2478fffe */ 	addiu	$t8,$v1,-2
/*     a484:	0018c840 */ 	sll	$t9,$t8,0x1
/*     a488:	000f7400 */ 	sll	$t6,$t7,0x10
/*     a48c:	00f97821 */ 	addu	$t7,$a3,$t9
/*     a490:	25f80002 */ 	addiu	$t8,$t7,0x2
/*     a494:	8ca5d58c */ 	lw	$a1,%lo(var8005d58c)($a1)
/*     a498:	01d84025 */ 	or	$t0,$t6,$t8
/*     a49c:	0008cc03 */ 	sra	$t9,$t0,0x10
/*     a4a0:	332fffff */ 	andi	$t7,$t9,0xffff
/*     a4a4:	01e57021 */ 	addu	$t6,$t7,$a1
/*     a4a8:	01c6001a */ 	div	$zero,$t6,$a2
/*     a4ac:	14c00002 */ 	bnez	$a2,.L0000a4b8
/*     a4b0:	00000000 */ 	nop
/*     a4b4:	0007000d */ 	break	0x7
.L0000a4b8:
/*     a4b8:	2401ffff */ 	addiu	$at,$zero,-1
/*     a4bc:	14c10004 */ 	bne	$a2,$at,.L0000a4d0
/*     a4c0:	3c018000 */ 	lui	$at,0x8000
/*     a4c4:	15c10002 */ 	bne	$t6,$at,.L0000a4d0
/*     a4c8:	00000000 */ 	nop
/*     a4cc:	0006000d */ 	break	0x6
.L0000a4d0:
/*     a4d0:	310fffff */ 	andi	$t7,$t0,0xffff
/*     a4d4:	01e57021 */ 	addu	$t6,$t7,$a1
/*     a4d8:	0000c010 */ 	mfhi	$t8
/*     a4dc:	0018cc00 */ 	sll	$t9,$t8,0x10
/*     a4e0:	240f0001 */ 	addiu	$t7,$zero,0x1
/*     a4e4:	01c6001a */ 	div	$zero,$t6,$a2
/*     a4e8:	14c00002 */ 	bnez	$a2,.L0000a4f4
/*     a4ec:	00000000 */ 	nop
/*     a4f0:	0007000d */ 	break	0x7
.L0000a4f4:
/*     a4f4:	2401ffff */ 	addiu	$at,$zero,-1
/*     a4f8:	14c10004 */ 	bne	$a2,$at,.L0000a50c
/*     a4fc:	3c018000 */ 	lui	$at,0x8000
/*     a500:	15c10002 */ 	bne	$t6,$at,.L0000a50c
/*     a504:	00000000 */ 	nop
/*     a508:	0006000d */ 	break	0x6
.L0000a50c:
/*     a50c:	0000c010 */ 	mfhi	$t8
/*     a510:	03381025 */ 	or	$v0,$t9,$t8
/*     a514:	3c018006 */ 	lui	$at,%hi(var8005ce88)
/*     a518:	ac820030 */ 	sw	$v0,0x30($a0)
/*     a51c:	ada20000 */ 	sw	$v0,0x0($t5)
/*     a520:	ac820044 */ 	sw	$v0,0x44($a0)
/*     a524:	afe20000 */ 	sw	$v0,0x0($ra)
/*     a528:	002c0821 */ 	addu	$at,$at,$t4
/*     a52c:	100000f6 */ 	b	.L0000a908
/*     a530:	ac2fce88 */ 	sw	$t7,%lo(var8005ce88)($at)
.L0000a534:
/*     a534:	148300ef */ 	bne	$a0,$v1,.L0000a8f4
/*     a538:	3c0e8000 */ 	lui	$t6,0x8000
/*     a53c:	8dce0300 */ 	lw	$t6,0x300($t6)
/*     a540:	3406ffff */ 	dli	$a2,0xffff
/*     a544:	24080800 */ 	addiu	$t0,$zero,0x800
/*     a548:	148e0019 */ 	bne	$a0,$t6,.L0000a5b0
/*     a54c:	3c0d8009 */ 	lui	$t5,%hi(var8008de0c)
/*     a550:	000bc880 */ 	sll	$t9,$t3,0x2
/*     a554:	032bc821 */ 	addu	$t9,$t9,$t3
/*     a558:	3c188009 */ 	lui	$t8,%hi(var8008dcc0)
/*     a55c:	2718dcc0 */ 	addiu	$t8,$t8,%lo(var8008dcc0)
/*     a560:	0019c900 */ 	sll	$t9,$t9,0x4
/*     a564:	03382021 */ 	addu	$a0,$t9,$t8
/*     a568:	3c0f8006 */ 	lui	$t7,%hi(osViModeTable+0xc30)
/*     a56c:	25ef0340 */ 	addiu	$t7,$t7,%lo(osViModeTable+0xc30)
/*     a570:	25f90048 */ 	addiu	$t9,$t7,0x48
/*     a574:	0080c025 */ 	or	$t8,$a0,$zero
.L0000a578:
/*     a578:	8de10000 */ 	lw	$at,0x0($t7)
/*     a57c:	25ef000c */ 	addiu	$t7,$t7,12
/*     a580:	2718000c */ 	addiu	$t8,$t8,0xc
/*     a584:	af01fff4 */ 	sw	$at,-0xc($t8)
/*     a588:	8de1fff8 */ 	lw	$at,-0x8($t7)
/*     a58c:	af01fff8 */ 	sw	$at,-0x8($t8)
/*     a590:	8de1fffc */ 	lw	$at,-0x4($t7)
/*     a594:	15f9fff8 */ 	bne	$t7,$t9,.L0000a578
/*     a598:	af01fffc */ 	sw	$at,-0x4($t8)
/*     a59c:	8de10000 */ 	lw	$at,0x0($t7)
/*     a5a0:	af010000 */ 	sw	$at,0x0($t8)
/*     a5a4:	8df90004 */ 	lw	$t9,0x4($t7)
/*     a5a8:	10000018 */ 	b	.L0000a60c
/*     a5ac:	af190004 */ 	sw	$t9,0x4($t8)
.L0000a5b0:
/*     a5b0:	000b7080 */ 	sll	$t6,$t3,0x2
/*     a5b4:	01cb7021 */ 	addu	$t6,$t6,$t3
/*     a5b8:	3c198009 */ 	lui	$t9,%hi(var8008dcc0)
/*     a5bc:	2739dcc0 */ 	addiu	$t9,$t9,%lo(var8008dcc0)
/*     a5c0:	000e7100 */ 	sll	$t6,$t6,0x4
/*     a5c4:	01d92021 */ 	addu	$a0,$t6,$t9
/*     a5c8:	3c0f8006 */ 	lui	$t7,%hi(osViModeTable+0x370)
/*     a5cc:	25effa80 */ 	addiu	$t7,$t7,%lo(osViModeTable+0x370)
/*     a5d0:	25ee0048 */ 	addiu	$t6,$t7,0x48
/*     a5d4:	0080c825 */ 	or	$t9,$a0,$zero
.L0000a5d8:
/*     a5d8:	8de10000 */ 	lw	$at,0x0($t7)
/*     a5dc:	25ef000c */ 	addiu	$t7,$t7,0xc
/*     a5e0:	2739000c */ 	addiu	$t9,$t9,0xc
/*     a5e4:	af21fff4 */ 	sw	$at,-0xc($t9)
/*     a5e8:	8de1fff8 */ 	lw	$at,-0x8($t7)
/*     a5ec:	af21fff8 */ 	sw	$at,-0x8($t9)
/*     a5f0:	8de1fffc */ 	lw	$at,-0x4($t7)
/*     a5f4:	15eefff8 */ 	bne	$t7,$t6,.L0000a5d8
/*     a5f8:	af21fffc */ 	sw	$at,-0x4($t9)
/*     a5fc:	8de10000 */ 	lw	$at,0x0($t7)
/*     a600:	af210000 */ 	sw	$at,0x0($t9)
/*     a604:	8dee0004 */ 	lw	$t6,0x4($t7)
/*     a608:	af2e0004 */ 	sw	$t6,0x4($t9)
.L0000a60c:
/*     a60c:	85380018 */ 	lh	$t8,0x18($t1)
/*     a610:	24010280 */ 	addiu	$at,$zero,0x280
/*     a614:	8c83001c */ 	lw	$v1,0x1c($a0)
/*     a618:	ac980008 */ 	sw	$t8,0x8($a0)
/*     a61c:	852e0018 */ 	lh	$t6,0x18($t1)
/*     a620:	ac88002c */ 	sw	$t0,0x2c($a0)
/*     a624:	ac880040 */ 	sw	$t0,0x40($a0)
/*     a628:	000e7a80 */ 	sll	$t7,$t6,0xa
/*     a62c:	01e1001a */ 	div	$zero,$t7,$at
/*     a630:	0000c812 */ 	mflo	$t9
/*     a634:	ac990020 */ 	sw	$t9,0x20($a0)
/*     a638:	85380018 */ 	lh	$t8,0x18($t1)
/*     a63c:	3c078006 */ 	lui	$a3,%hi(var8005d588)
/*     a640:	3c058006 */ 	lui	$a1,%hi(var8005d58c)
/*     a644:	00187040 */ 	sll	$t6,$t8,0x1
/*     a648:	ac8e0028 */ 	sw	$t6,0x28($a0)
/*     a64c:	852f0018 */ 	lh	$t7,0x18($t1)
/*     a650:	0003c403 */ 	sra	$t8,$v1,0x10
/*     a654:	330effff */ 	andi	$t6,$t8,0xffff
/*     a658:	000fc880 */ 	sll	$t9,$t7,0x2
/*     a65c:	ac99003c */ 	sw	$t9,0x3c($a0)
/*     a660:	8ce7d588 */ 	lw	$a3,%lo(var8005d588)($a3)
/*     a664:	25adde0c */ 	addiu	$t5,$t5,%lo(var8008de0c)
/*     a668:	3c1f8009 */ 	lui	$ra,%hi(var8008de10)
/*     a66c:	01c77821 */ 	addu	$t7,$t6,$a3
/*     a670:	01e6001a */ 	div	$zero,$t7,$a2
/*     a674:	14c00002 */ 	bnez	$a2,.L0000a680
/*     a678:	00000000 */ 	nop
/*     a67c:	0007000d */ 	break	0x7
.L0000a680:
/*     a680:	2401ffff */ 	addiu	$at,$zero,-1
/*     a684:	14c10004 */ 	bne	$a2,$at,.L0000a698
/*     a688:	3c018000 */ 	lui	$at,0x8000
/*     a68c:	15e10002 */ 	bne	$t7,$at,.L0000a698
/*     a690:	00000000 */ 	nop
/*     a694:	0006000d */ 	break	0x6
.L0000a698:
/*     a698:	306effff */ 	andi	$t6,$v1,0xffff
/*     a69c:	0000c810 */ 	mfhi	$t9
/*     a6a0:	01c77821 */ 	addu	$t7,$t6,$a3
/*     a6a4:	0019c400 */ 	sll	$t8,$t9,0x10
/*     a6a8:	01e6001a */ 	div	$zero,$t7,$a2
/*     a6ac:	14c00002 */ 	bnez	$a2,.L0000a6b8
/*     a6b0:	00000000 */ 	nop
/*     a6b4:	0007000d */ 	break	0x7
.L0000a6b8:
/*     a6b8:	2401ffff */ 	addiu	$at,$zero,-1
/*     a6bc:	14c10004 */ 	bne	$a2,$at,.L0000a6d0
/*     a6c0:	3c018000 */ 	lui	$at,0x8000
/*     a6c4:	15e10002 */ 	bne	$t7,$at,.L0000a6d0
/*     a6c8:	00000000 */ 	nop
/*     a6cc:	0006000d */ 	break	0x6
.L0000a6d0:
/*     a6d0:	0000c810 */ 	mfhi	$t9
/*     a6d4:	03191025 */ 	or	$v0,$t8,$t9
/*     a6d8:	ac82001c */ 	sw	$v0,0x1c($a0)
/*     a6dc:	3c018009 */ 	lui	$at,%hi(var8008de08)
/*     a6e0:	ac22de08 */ 	sw	$v0,%lo(var8008de08)($at)
/*     a6e4:	8c830030 */ 	lw	$v1,0x30($a0)
/*     a6e8:	8ca5d58c */ 	lw	$a1,%lo(var8005d58c)($a1)
/*     a6ec:	27ffde10 */ 	addiu	$ra,$ra,%lo(var8008de10)
/*     a6f0:	00037403 */ 	sra	$t6,$v1,0x10
/*     a6f4:	31cfffff */ 	andi	$t7,$t6,0xffff
/*     a6f8:	01e5c021 */ 	addu	$t8,$t7,$a1
/*     a6fc:	0306001a */ 	div	$zero,$t8,$a2
/*     a700:	14c00002 */ 	bnez	$a2,.L0000a70c
/*     a704:	00000000 */ 	nop
/*     a708:	0007000d */ 	break	0x7
.L0000a70c:
/*     a70c:	2401ffff */ 	addiu	$at,$zero,-1
/*     a710:	14c10004 */ 	bne	$a2,$at,.L0000a724
/*     a714:	3c018000 */ 	lui	$at,0x8000
/*     a718:	17010002 */ 	bne	$t8,$at,.L0000a724
/*     a71c:	00000000 */ 	nop
/*     a720:	0006000d */ 	break	0x6
.L0000a724:
/*     a724:	306fffff */ 	andi	$t7,$v1,0xffff
/*     a728:	01e5c021 */ 	addu	$t8,$t7,$a1
/*     a72c:	0000c810 */ 	mfhi	$t9
/*     a730:	8c830044 */ 	lw	$v1,0x44($a0)
/*     a734:	00197400 */ 	sll	$t6,$t9,0x10
/*     a738:	0306001a */ 	div	$zero,$t8,$a2
/*     a73c:	14c00002 */ 	bnez	$a2,.L0000a748
/*     a740:	00000000 */ 	nop
/*     a744:	0007000d */ 	break	0x7
.L0000a748:
/*     a748:	2401ffff */ 	addiu	$at,$zero,-1
/*     a74c:	14c10004 */ 	bne	$a2,$at,.L0000a760
/*     a750:	3c018000 */ 	lui	$at,0x8000
/*     a754:	17010002 */ 	bne	$t8,$at,.L0000a760
/*     a758:	00000000 */ 	nop
/*     a75c:	0006000d */ 	break	0x6
.L0000a760:
/*     a760:	0000c810 */ 	mfhi	$t9
/*     a764:	00037c03 */ 	sra	$t7,$v1,0x10
/*     a768:	01d91025 */ 	or	$v0,$t6,$t9
/*     a76c:	31f8ffff */ 	andi	$t8,$t7,0xffff
/*     a770:	03057021 */ 	addu	$t6,$t8,$a1
/*     a774:	01c6001a */ 	div	$zero,$t6,$a2
/*     a778:	3078ffff */ 	andi	$t8,$v1,0xffff
/*     a77c:	0000c810 */ 	mfhi	$t9
/*     a780:	00197c00 */ 	sll	$t7,$t9,0x10
/*     a784:	ac820030 */ 	sw	$v0,0x30($a0)
/*     a788:	ada20000 */ 	sw	$v0,0x0($t5)
/*     a78c:	14c00002 */ 	bnez	$a2,.L0000a798
/*     a790:	00000000 */ 	nop
/*     a794:	0007000d */ 	break	0x7
.L0000a798:
/*     a798:	2401ffff */ 	addiu	$at,$zero,-1
/*     a79c:	14c10004 */ 	bne	$a2,$at,.L0000a7b0
/*     a7a0:	3c018000 */ 	lui	$at,0x8000
/*     a7a4:	15c10002 */ 	bne	$t6,$at,.L0000a7b0
/*     a7a8:	00000000 */ 	nop
/*     a7ac:	0006000d */ 	break	0x6
.L0000a7b0:
/*     a7b0:	03057021 */ 	addu	$t6,$t8,$a1
/*     a7b4:	01c6001a */ 	div	$zero,$t6,$a2
/*     a7b8:	0000c810 */ 	mfhi	$t9
/*     a7bc:	01f91025 */ 	or	$v0,$t7,$t9
/*     a7c0:	ac820044 */ 	sw	$v0,0x44($a0)
/*     a7c4:	afe20000 */ 	sw	$v0,0x0($ra)
/*     a7c8:	3c188006 */ 	lui	$t8,%hi(var8005dd18)
/*     a7cc:	8f18dd18 */ 	lw	$t8,%lo(var8005dd18)($t8)
/*     a7d0:	14c00002 */ 	bnez	$a2,.L0000a7dc
/*     a7d4:	00000000 */ 	nop
/*     a7d8:	0007000d */ 	break	0x7
.L0000a7dc:
/*     a7dc:	2401ffff */ 	addiu	$at,$zero,-1
/*     a7e0:	14c10004 */ 	bne	$a2,$at,.L0000a7f4
/*     a7e4:	3c018000 */ 	lui	$at,0x8000
/*     a7e8:	15c10002 */ 	bne	$t6,$at,.L0000a7f4
/*     a7ec:	00000000 */ 	nop
/*     a7f0:	0006000d */ 	break	0x6
.L0000a7f4:
/*     a7f4:	3c0a8006 */ 	lui	$t2,%hi(g_ViIs16Bit)
/*     a7f8:	13000038 */ 	beqz	$t8,.L0000a8dc
/*     a7fc:	24ae01af */ 	addiu	$t6,$a1,0x1af
/*     a800:	01c6001a */ 	div	$zero,$t6,$a2
/*     a804:	00007810 */ 	mfhi	$t7
/*     a808:	24b8007b */ 	addiu	$t8,$a1,0x7b
/*     a80c:	14c00002 */ 	bnez	$a2,.L0000a818
/*     a810:	00000000 */ 	nop
/*     a814:	0007000d */ 	break	0x7
.L0000a818:
/*     a818:	2401ffff */ 	addiu	$at,$zero,-1
/*     a81c:	14c10004 */ 	bne	$a2,$at,.L0000a830
/*     a820:	3c018000 */ 	lui	$at,0x8000
/*     a824:	15c10002 */ 	bne	$t6,$at,.L0000a830
/*     a828:	00000000 */ 	nop
/*     a82c:	0006000d */ 	break	0x6
.L0000a830:
/*     a830:	0306001a */ 	div	$zero,$t8,$a2
/*     a834:	00007010 */ 	mfhi	$t6
/*     a838:	000fcc00 */ 	sll	$t9,$t7,0x10
/*     a83c:	24af01b1 */ 	addiu	$t7,$a1,0x1b1
/*     a840:	01e6001a */ 	div	$zero,$t7,$a2
/*     a844:	032e1025 */ 	or	$v0,$t9,$t6
/*     a848:	24ae0079 */ 	addiu	$t6,$a1,0x79
/*     a84c:	14c00002 */ 	bnez	$a2,.L0000a858
/*     a850:	00000000 */ 	nop
/*     a854:	0007000d */ 	break	0x7
.L0000a858:
/*     a858:	2401ffff */ 	addiu	$at,$zero,-1
/*     a85c:	14c10004 */ 	bne	$a2,$at,.L0000a870
/*     a860:	3c018000 */ 	lui	$at,0x8000
/*     a864:	17010002 */ 	bne	$t8,$at,.L0000a870
/*     a868:	00000000 */ 	nop
/*     a86c:	0006000d */ 	break	0x6
.L0000a870:
/*     a870:	0000c010 */ 	mfhi	$t8
/*     a874:	ac820030 */ 	sw	$v0,0x30($a0)
/*     a878:	ada20000 */ 	sw	$v0,0x0($t5)
/*     a87c:	01c6001a */ 	div	$zero,$t6,$a2
/*     a880:	14c00002 */ 	bnez	$a2,.L0000a88c
/*     a884:	00000000 */ 	nop
/*     a888:	0007000d */ 	break	0x7
.L0000a88c:
/*     a88c:	2401ffff */ 	addiu	$at,$zero,-1
/*     a890:	14c10004 */ 	bne	$a2,$at,.L0000a8a4
/*     a894:	3c018000 */ 	lui	$at,0x8000
/*     a898:	15e10002 */ 	bne	$t7,$at,.L0000a8a4
/*     a89c:	00000000 */ 	nop
/*     a8a0:	0006000d */ 	break	0x6
.L0000a8a4:
/*     a8a4:	00007810 */ 	mfhi	$t7
/*     a8a8:	0018cc00 */ 	sll	$t9,$t8,0x10
/*     a8ac:	032f1025 */ 	or	$v0,$t9,$t7
/*     a8b0:	ac820044 */ 	sw	$v0,0x44($a0)
/*     a8b4:	afe20000 */ 	sw	$v0,0x0($ra)
/*     a8b8:	14c00002 */ 	bnez	$a2,.L0000a8c4
/*     a8bc:	00000000 */ 	nop
/*     a8c0:	0007000d */ 	break	0x7
.L0000a8c4:
/*     a8c4:	2401ffff */ 	addiu	$at,$zero,-1
/*     a8c8:	14c10004 */ 	bne	$a2,$at,.L0000a8dc
/*     a8cc:	3c018000 */ 	lui	$at,0x8000
/*     a8d0:	15c10002 */ 	bne	$t6,$at,.L0000a8dc
/*     a8d4:	00000000 */ 	nop
/*     a8d8:	0006000d */ 	break	0x6
.L0000a8dc:
/*     a8dc:	3c018006 */ 	lui	$at,%hi(var8005ce88)
/*     a8e0:	002c0821 */ 	addu	$at,$at,$t4
/*     a8e4:	24180001 */ 	addiu	$t8,$zero,0x1
/*     a8e8:	ac38ce88 */ 	sw	$t8,%lo(var8005ce88)($at)
/*     a8ec:	10000006 */ 	b	.L0000a908
/*     a8f0:	8d4ad598 */ 	lw	$t2,%lo(g_ViIs16Bit)($t2)
.L0000a8f4:
/*     a8f4:	3c018006 */ 	lui	$at,%hi(var8005ce88)
/*     a8f8:	002c0821 */ 	addu	$at,$at,$t4
/*     a8fc:	3c0a8006 */ 	lui	$t2,%hi(g_ViIs16Bit)
/*     a900:	8d4ad598 */ 	lw	$t2,%lo(g_ViIs16Bit)($t2)
/*     a904:	ac20ce88 */ 	sw	$zero,%lo(var8005ce88)($at)
.L0000a908:
/*     a908:	256b0001 */ 	addiu	$t3,$t3,0x1
/*     a90c:	05610004 */ 	bgez	$t3,.L0000a920
/*     a910:	316e0001 */ 	andi	$t6,$t3,0x1
/*     a914:	11c00002 */ 	beqz	$t6,.L0000a920
/*     a918:	00000000 */ 	nop
/*     a91c:	25cefffe */ 	addiu	$t6,$t6,-2
.L0000a920:
/*     a920:	3c018006 */ 	lui	$at,%hi(var8005d5a0)
/*     a924:	11400006 */ 	beqz	$t2,.L0000a940
/*     a928:	ac2ed5a0 */ 	sw	$t6,%lo(var8005d5a0)($at)
/*     a92c:	3c0f8006 */ 	lui	$t7,%hi(g_RdpCurTask)
/*     a930:	8deff108 */ 	lw	$t7,%lo(g_RdpCurTask)($t7)
/*     a934:	8d390028 */ 	lw	$t9,0x28($t1)
/*     a938:	10000006 */ 	b	.L0000a954
/*     a93c:	adf90058 */ 	sw	$t9,0x58($t7)
.L0000a940:
/*     a940:	3c18800a */ 	lui	$t8,%hi(var8009cac0)
/*     a944:	3c0e8006 */ 	lui	$t6,%hi(g_RdpCurTask)
/*     a948:	8dcef108 */ 	lw	$t6,%lo(g_RdpCurTask)($t6)
/*     a94c:	8f18cac0 */ 	lw	$t8,%lo(var8009cac0)($t8)
/*     a950:	add80058 */ 	sw	$t8,0x58($t6)
.L0000a954:
/*     a954:	3c028009 */ 	lui	$v0,%hi(var80092874+0x2)
/*     a958:	24422876 */ 	addiu	$v0,$v0,%lo(var80092874+0x2)
/*     a95c:	90590000 */ 	lbu	$t9,0x0($v0)
/*     a960:	3c048006 */ 	lui	$a0,%hi(g_ViData)
/*     a964:	8c84d594 */ 	lw	$a0,%lo(g_ViData)($a0)
/*     a968:	272f0001 */ 	addiu	$t7,$t9,0x1
/*     a96c:	3c038009 */ 	lui	$v1,%hi(var80092874+0x3)
/*     a970:	2408002c */ 	addiu	$t0,$zero,0x2c
/*     a974:	24632877 */ 	addiu	$v1,$v1,%lo(var80092874+0x3)
/*     a978:	906e0000 */ 	lbu	$t6,0x0($v1)
/*     a97c:	3c078006 */ 	lui	$a3,%hi(var8005d530)
/*     a980:	24e7d530 */ 	addiu	$a3,$a3,%lo(var8005d530)
/*     a984:	05e10004 */ 	bgez	$t7,.L0000a998
/*     a988:	31f80001 */ 	andi	$t8,$t7,0x1
/*     a98c:	13000002 */ 	beqz	$t8,.L0000a998
/*     a990:	00000000 */ 	nop
/*     a994:	2718fffe */ 	addiu	$t8,$t8,-2
.L0000a998:
/*     a998:	a0580000 */ 	sb	$t8,0x0($v0)
/*     a99c:	90580000 */ 	lbu	$t8,0x0($v0)
/*     a9a0:	25d90001 */ 	addiu	$t9,$t6,0x1
/*     a9a4:	07210004 */ 	bgez	$t9,.L0000a9b8
/*     a9a8:	332f0001 */ 	andi	$t7,$t9,0x1
/*     a9ac:	11e00002 */ 	beqz	$t7,.L0000a9b8
/*     a9b0:	00000000 */ 	nop
/*     a9b4:	25effffe */ 	addiu	$t7,$t7,-2
.L0000a9b8:
/*     a9b8:	03080019 */ 	multu	$t8,$t0
/*     a9bc:	a06f0000 */ 	sb	$t7,0x0($v1)
/*     a9c0:	3c018006 */ 	lui	$at,%hi(var8005d590)
/*     a9c4:	3c058006 */ 	lui	$a1,%hi(g_ViData)
/*     a9c8:	2406002c */ 	addiu	$a2,$zero,0x2c
/*     a9cc:	00007012 */ 	mflo	$t6
/*     a9d0:	00eec821 */ 	addu	$t9,$a3,$t6
/*     a9d4:	ac39d590 */ 	sw	$t9,%lo(var8005d590)($at)
/*     a9d8:	906f0000 */ 	lbu	$t7,0x0($v1)
/*     a9dc:	3c018006 */ 	lui	$at,%hi(g_ViData)
/*     a9e0:	01e80019 */ 	multu	$t7,$t0
/*     a9e4:	0000c012 */ 	mflo	$t8
/*     a9e8:	00f87021 */ 	addu	$t6,$a3,$t8
/*     a9ec:	ac2ed594 */ 	sw	$t6,%lo(g_ViData)($at)
/*     a9f0:	0c012c5c */ 	jal	bcopy
/*     a9f4:	8ca5d594 */ 	lw	$a1,%lo(g_ViData)($a1)
/*     a9f8:	3c038009 */ 	lui	$v1,%hi(var80092874+0x3)
/*     a9fc:	24632877 */ 	addiu	$v1,$v1,%lo(var80092874+0x3)
/*     aa00:	90790000 */ 	lbu	$t9,0x0($v1)
/*     aa04:	3c18800a */ 	lui	$t8,%hi(var8009cac0)
/*     aa08:	3c0e8006 */ 	lui	$t6,%hi(g_ViData)
/*     aa0c:	00197880 */ 	sll	$t7,$t9,0x2
/*     aa10:	030fc021 */ 	addu	$t8,$t8,$t7
/*     aa14:	8f18cac0 */ 	lw	$t8,%lo(var8009cac0)($t8)
/*     aa18:	8dced594 */ 	lw	$t6,%lo(g_ViData)($t6)
/*     aa1c:	3c028006 */ 	lui	$v0,%hi(var8005d59c)
/*     aa20:	2442d59c */ 	addiu	$v0,$v0,%lo(var8005d59c)
/*     aa24:	add80028 */ 	sw	$t8,0x28($t6)
/*     aa28:	8c590000 */ 	lw	$t9,0x0($v0)
/*     aa2c:	53200005 */ 	beqzl	$t9,.L0000aa44
/*     aa30:	8fbf0014 */ 	lw	$ra,0x14($sp)
/*     aa34:	ac400000 */ 	sw	$zero,0x0($v0)
/*     aa38:	0c0027b1 */ 	jal	viBlack
/*     aa3c:	00002025 */ 	or	$a0,$zero,$zero
/*     aa40:	8fbf0014 */ 	lw	$ra,0x14($sp)
.L0000aa44:
/*     aa44:	27bd0018 */ 	addiu	$sp,$sp,0x18
/*     aa48:	03e00008 */ 	jr	$ra
/*     aa4c:	00000000 */ 	nop
);
#endif

void vi0000aa50(f32 arg0)
{
	if (arg0 > 14) {
		arg0 = 14;
	}

	if (arg0 < 0) {
		arg0 = 0;
	}

	var8005ce98 = arg0;
	var8005ce9c = 10;
}

void vi0000aab0(s32 mode)
{
	g_ViData->mode = mode;

	g_ViData->x = g_ViData->bufx = var700526d0[mode];
	g_ViData->y = g_ViData->bufy = var700526d8[mode];
}

void viSet16Bit(void)
{
	g_ViIs16Bit = true;
}

void viSet32Bit(void)
{
	g_ViIs16Bit = false;
}

u8 *viGetUnk28(void)
{
	return g_ViData->fb;
}

u8 *vi2GetUnk28(void)
{
	return var8005d590->fb;
}

void viSetUnk28(u8 *fb)
{
	g_ViData->fb = fb;
}

Vp *viGetCurrentPlayerViewport(void)
{
	return &g_Vars.currentplayer->viewport[var80092877];
}

u16 vi0000ab6c(void)
{
	return var80092874;
}

Gfx *vi0000ab78(Gfx *gdl)
{
	Mtxf sp110;
	Mtxf spd0;
	Mtxf sp90;
	Mtxf sp50;
	Mtx *sp4c;
	Mtx *sp48;
	u16 sp46;

	guPerspectiveF(sp110.m, &sp46, g_ViData->fovy, g_ViData->aspect, g_ViData->znear, g_ViData->zfar + g_ViData->zfar, 1);
	func00015d18(currentPlayerGetMatrix1740(), &sp90);

	sp90.m[3][0] = 0;
	sp90.m[3][1] = 0;
	sp90.m[3][2] = 0;

	func00015a00(&sp110, &sp90, &spd0);
	sp4c = gfxAllocateMatrix();
	guMtxF2L(spd0.m, sp4c);

	func000159b0(&sp50);
	sp48 = gfxAllocateMatrix();
	guMtxF2L(sp50.m, sp48);

	gSPMatrix(gdl++, OS_K0_TO_PHYSICAL(sp4c), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_PROJECTION);
	gSPMatrix(gdl++, OS_K0_TO_PHYSICAL(sp48), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
	gSPPerspNormalize(gdl++, sp46);

	return gdl;
}

Gfx *vi0000aca4(Gfx *gdl, f32 znear, f32 zfar)
{
	u16 scale;
	Mtxf tmp;
	Mtx *mtx = gfxAllocateMatrix();

	guPerspectiveF(tmp.m, &scale, g_ViData->fovy, g_ViData->aspect, znear, zfar, 1);
	guMtxF2L(tmp.m, mtx);

	gSPMatrix(gdl++, OS_K0_TO_PHYSICAL(mtx), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_PROJECTION);
	gSPPerspNormalize(gdl++, scale);

	return gdl;
}

Gfx *vi0000ad5c(Gfx *gdl, Vp *vp)
{
	vp[var80092877].vp.vscale[0] = g_ViData->viewx * 2;
	vp[var80092877].vp.vtrans[0] = g_ViData->viewx * 2 + g_ViData->viewleft * 4;

	vp[var80092877].vp.vscale[1] = g_ViData->viewy * 2;
	vp[var80092877].vp.vtrans[1] = g_ViData->viewy * 2 + g_ViData->viewtop * 4;

	gSPViewport(gdl++, OS_K0_TO_PHYSICAL(&vp[var80092877]));

	var80092870 = gfxAllocateMatrix();
	guPerspectiveF(var80092830.m, &var80092874, g_ViData->fovy, g_ViData->aspect, g_ViData->znear, g_ViData->zfar, 1);
	guMtxF2L(var80092830.m, var80092870);

	gSPMatrix(gdl++, OS_K0_TO_PHYSICAL(var80092870), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_PROJECTION);
	gSPPerspNormalize(gdl++, var80092874);

	currentPlayerSetUnk1750(var80092870);
	currentPlayerSetUnk1754(&var80092830);

	return gdl;
}

Gfx *vi0000af00(Gfx *gdl, Vp *vp)
{
	vp[var80092877].vp.vscale[0] = g_ViData->viewx * 2;
	vp[var80092877].vp.vtrans[0] = g_ViData->viewx * 2 + g_ViData->viewleft * 4;

	vp[var80092877].vp.vscale[1] = g_ViData->viewy * 2;
	vp[var80092877].vp.vtrans[1] = g_ViData->viewy * 2 + g_ViData->viewtop * 4;

	vp[var80092877].vp.vscale[2] = 511;
	vp[var80092877].vp.vtrans[2] = 511;

	vp[var80092877].vp.vscale[3] = 0;
	vp[var80092877].vp.vtrans[3] = 0;

	gSPViewport(gdl++, OS_K0_TO_PHYSICAL(&vp[var80092877]));

	var80092870 = gfxAllocateMatrix();
	guPerspectiveF(var80092830.m, &var80092874, g_ViData->fovy, g_ViData->aspect, g_ViData->znear, g_ViData->zfar, 1);
	guMtxF2L(var80092830.m, var80092870);

	gSPMatrix(gdl++, OS_K0_TO_PHYSICAL(var80092870), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_PROJECTION);
	gSPPerspNormalize(gdl++, var80092874);

	currentPlayerSetUnk1750(var80092870);
	currentPlayerSetUnk1754(&var80092830);

	return gdl;
}

Gfx *vi0000b0e8(Gfx *gdl, f32 fovy, f32 aspect)
{
	Mtxf tmp;
	Mtx *mtx = gfxAllocateMatrix();

	guPerspectiveF(tmp.m, &var80092874, fovy, aspect, g_ViData->znear, g_ViData->zfar, 1);
	guMtxF2L(tmp.m, mtx);

	gSPMatrix(gdl++, OS_K0_TO_PHYSICAL(mtx), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_PROJECTION);
	gSPPerspNormalize(gdl++, var80092874);

	return gdl;
}

Gfx *vi0000b1a8(Gfx *gdl)
{
	return vi0000ad5c(gdl, &g_Vars.currentplayer->viewport[0]);
}

Gfx *vi0000b1d0(Gfx *gdl)
{
	gdl = vi0000b1a8(gdl);

	if (g_ViIs16Bit) {
		gDPSetColorImage(gdl++, G_IM_FMT_RGBA, G_IM_SIZ_16b, g_ViData->bufx, OS_K0_TO_PHYSICAL(g_ViData->fb));
	} else {
		gDPSetColorImage(gdl++, G_IM_FMT_RGBA, G_IM_SIZ_32b, g_ViData->bufx, OS_K0_TO_PHYSICAL(var8009cac0[0]));
	}

	return gdl;
}

Gfx *vi0000b280(Gfx *gdl)
{
	if (g_ViData->usezbuf) {
		gdl = func0f1762ac(gdl);
		gdl = func0f1763f4(gdl);
	}

	return gdl;
}

Gfx *func0000b2c4(Gfx *gdl)
{
	gDPSetCycleType(gdl++, G_CYC_FILL);
	gDPFillRectangle(gdl++, 0, 0, g_ViData->bufx - 1, g_ViData->bufy - 1);
	gDPPipeSync(gdl++);

	return gdl;
}

Gfx *viRenderViewportEdges(Gfx *gdl)
{
	gDPSetCycleType(gdl++, G_CYC_FILL);
	gDPSetScissor(gdl++, G_SC_NON_INTERLACE, 0, 0, viGetWidth(), viGetHeight());
	gDPSetFillColor(gdl++, GPACK_RGBA5551(0, 0, 0, 1) << 16 | GPACK_RGBA5551(0, 0, 0, 1));

#if VERSION >= VERSION_NTSC_1_0
	if (PLAYERCOUNT() == 1
			|| ((g_Vars.coopplayernum >= 0 || g_Vars.antiplayernum >= 0)
				&& is2PSharedViewport() && g_Vars.currentplayernum == 0))
#else
	if (PLAYERCOUNT() == 1
			|| ((g_Vars.coopplayernum >= 0 || g_Vars.antiplayernum >= 0)
				&& (
					(g_InCutscene && !g_MainIsEndscreen) || menuGetRoot() == MENUROOT_COOPCONTINUE
					) && g_Vars.currentplayernum == 0))
#endif
	{
		// Single viewport
		if (viGetViewTop() > 0) {
			// Fill above
			gDPFillRectangle(gdl++, 0, 0, viGetWidth() - 1, viGetViewTop() - 1);
			gDPPipeSync(gdl++);
		}

		if (viGetViewTop() + viGetViewHeight() < viGetHeight()) {
			// Fill below
			gDPFillRectangle(gdl++,
					0, viGetViewTop() + viGetViewHeight(),
					viGetWidth() - 1, viGetHeight() - 1);
			gDPPipeSync(gdl++);
		}
	} else {
		if (g_Vars.currentplayerindex == 0) {
			s32 topplayernum = 0;
			s32 bottomplayernum = 0;
			s32 tmpplayernum = 0;

			if (PLAYERCOUNT() == 2) {
				bottomplayernum = 1;
				tmpplayernum = 1;
			} else if (PLAYERCOUNT() >= 3) {
				bottomplayernum = 2;
				tmpplayernum = 2;
			}

			if (g_Vars.players[topplayernum]->viewtop > 0) {
				// Fill above all viewports - full width
				gDPFillRectangle(gdl++, 0, 0, viGetWidth() - 1, g_Vars.players[topplayernum]->viewtop - 1);
				gDPPipeSync(gdl++);
			}

			if (g_Vars.players[bottomplayernum]->viewtop + g_Vars.players[bottomplayernum]->viewheight < viGetHeight()) {
				// Fill below all viewports - full width
				gDPFillRectangle(gdl++,
						0, g_Vars.players[bottomplayernum]->viewtop + g_Vars.players[bottomplayernum]->viewheight,
						viGetWidth() - 1, viGetHeight() - 1);
				gDPPipeSync(gdl++);
			}

			// Horizontal middle line
			gDPFillRectangle(gdl++,
					0, g_Vars.players[tmpplayernum]->viewtop - 1,
					viGetWidth() - 1, g_Vars.players[tmpplayernum]->viewtop - 1);
			gDPPipeSync(gdl++);

			if (PLAYERCOUNT() >= 3 ||
					(PLAYERCOUNT() == 2 && (optionsGetScreenSplit() == SCREENSPLIT_VERTICAL || g_Vars.fourmeg2player))) {
				if (PLAYERCOUNT() == 2) {
					tmpplayernum = 0;
				}

				// Vertical middle line
				gDPFillRectangle(gdl++,
						g_Vars.players[tmpplayernum]->viewleft + g_Vars.players[tmpplayernum]->viewwidth, 0,
						g_Vars.players[tmpplayernum]->viewleft + g_Vars.players[tmpplayernum]->viewwidth, viGetHeight() - 1);
				gDPPipeSync(gdl++);
			}

			if (PLAYERCOUNT() == 3) {
				// Blank square in P4 spot
				gDPFillRectangle(gdl++,
						g_Vars.players[tmpplayernum]->viewleft + g_Vars.players[tmpplayernum]->viewwidth + 1, g_Vars.players[tmpplayernum]->viewtop,
						viGetWidth() - 1, viGetHeight() - 1);
				gDPPipeSync(gdl++);
			}
		}
	}

	return gdl;
}

#if VERSION < VERSION_NTSC_1_0
GLOBAL_ASM(
glabel vi0000bd44nb
/*     bd44:	3c018006 */ 	lui	$at,0x8006
/*     bd48:	03e00008 */ 	jr	$ra
/*     bd4c:	ac24ed2c */ 	sw	$a0,-0x12d4($at)
);

GLOBAL_ASM(
glabel vi0000bd50nb
/*     bd50:	3c028006 */ 	lui	$v0,0x8006
/*     bd54:	03e00008 */ 	jr	$ra
/*     bd58:	8c42ed2c */ 	lw	$v0,-0x12d4($v0)
);

GLOBAL_ASM(
glabel vi0000bd5cnb
/*     bd5c:	3c018006 */ 	lui	$at,0x8006
/*     bd60:	03e00008 */ 	jr	$ra
/*     bd64:	ac24ed28 */ 	sw	$a0,-0x12d8($at)
);

GLOBAL_ASM(
glabel vi0000bd68nb
/*     bd68:	3c028006 */ 	lui	$v0,0x8006
/*     bd6c:	03e00008 */ 	jr	$ra
/*     bd70:	8c42ed28 */ 	lw	$v0,-0x12d8($v0)
);
#endif

void viSetBuf(s16 x, s16 y)
{
	g_ViData->bufx = x;
	g_ViData->bufy = y;
}

s16 viGetBufX(void)
{
	return g_ViData->bufx;
}

s16 viGetBufY(void)
{
	return g_ViData->bufy;
}

void viSetXY(s16 x, s16 y)
{
	g_ViData->x = x;
	g_ViData->y = y;
}

s16 viGetWidth(void)
{
	return g_ViData->x;
}

s16 viGetHeight(void)
{
	return g_ViData->y;
}

void viSetViewSize(s16 x, s16 y)
{
	g_ViData->viewx = x;
	g_ViData->viewy = y;

	currentPlayerSetScreenSize(g_ViData->viewx, g_ViData->viewy);
	currentPlayerSetCameraScale();
}

s16 viGetViewWidth(void)
{
	return g_ViData->viewx;
}

s16 viGetViewHeight(void)
{
	return g_ViData->viewy;
}

void viSetViewPosition(s16 left, s16 top)
{
	g_ViData->viewleft = left;
	g_ViData->viewtop = top;

	currentPlayerSetScreenPosition(g_ViData->viewleft, g_ViData->viewtop);
}

s16 viGetViewLeft(void)
{
	return g_ViData->viewleft;
}

s16 viGetViewTop(void)
{
	return g_ViData->viewtop;
}

void viSetUseZBuf(bool use)
{
	g_ViData->usezbuf = use;
}

void viSetFovY(f32 fovy)
{
	g_ViData->fovy = fovy;

	currentPlayerSetPerspective(g_ViData->znear, g_ViData->fovy, g_ViData->aspect);
	currentPlayerSetCameraScale();
}

void viSetAspect(f32 aspect)
{
	g_ViData->aspect = aspect;

	currentPlayerSetPerspective(g_ViData->znear, g_ViData->fovy, g_ViData->aspect);
	currentPlayerSetCameraScale();
}

f32 viGetAspect(void)
{
	return g_ViData->aspect;
}

void viSetFovAspectAndSize(f32 fovy, f32 aspect, s16 width, s16 height)
{
	g_ViData->fovy = fovy;
	g_ViData->aspect = aspect;
	g_ViData->viewx = width;
	g_ViData->viewy = height;

	currentPlayerSetScreenSize(g_ViData->viewx, g_ViData->viewy);
	currentPlayerSetPerspective(g_ViData->znear, g_ViData->fovy, g_ViData->aspect);
	currentPlayerSetCameraScale();
}

f32 viGetFovY(void)
{
	return g_ViData->fovy;
}

void viSetZRange(f32 near, f32 far)
{
	g_ViData->znear = near;
	g_ViData->zfar = far;

	currentPlayerSetPerspective(g_ViData->znear, g_ViData->fovy, g_ViData->aspect);
	currentPlayerSetCameraScale();
}

void viGetZRange(struct zrange *zrange)
{
	zrange->near = g_ViData->znear;
	zrange->far = g_ViData->zfar;
}

Gfx *viSetFillColour(Gfx *gdl, s32 r, s32 g, s32 b)
{
	if (g_ViIs16Bit) {
		gDPSetFillColor(gdl++, (GPACK_RGBA5551(r, g, b, 1) << 16) | GPACK_RGBA5551(r, g, b, 1));
	} else {
		gDPSetFillColor(gdl++, r << 24 | g << 16 | b << 8 | 0xff);
	}

	return gdl;
}

void vi0000bf8c(void)
{
	// empty
}

void vi0000bf94(void)
{
	// empty
}

void vi0000bf9c(void)
{
	// empty
}

void vi0000bfa4(void)
{
	// empty
}
