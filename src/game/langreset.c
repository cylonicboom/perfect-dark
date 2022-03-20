#include <ultra64.h>
#include "constants.h"
#include "game/file.h"
#include "game/lang.h"
#include "bss.h"
#include "lib/memp.h"
#include "data.h"
#include "types.h"

#if VERSION >= VERSION_JPN_FINAL
GLOBAL_ASM(
glabel langReset
/*  f00b3b0:	27bdffe0 */ 	addiu	$sp,$sp,-32
/*  f00b3b4:	3c02800b */ 	lui	$v0,0x800b
/*  f00b3b8:	3c03800b */ 	lui	$v1,0x800b
/*  f00b3bc:	afbf0014 */ 	sw	$ra,0x14($sp)
/*  f00b3c0:	2463b5a4 */ 	addiu	$v1,$v1,-19036
/*  f00b3c4:	2442b490 */ 	addiu	$v0,$v0,-19312
.JF0f00b3c8:
/*  f00b3c8:	24420004 */ 	addiu	$v0,$v0,0x4
/*  f00b3cc:	1443fffe */ 	bne	$v0,$v1,.JF0f00b3c8
/*  f00b3d0:	ac40fffc */ 	sw	$zero,-0x4($v0)
/*  f00b3d4:	3c02800b */ 	lui	$v0,0x800b
/*  f00b3d8:	2442b490 */ 	addiu	$v0,$v0,-19312
/*  f00b3dc:	24030001 */ 	li	$v1,0x1
/*  f00b3e0:	2401005c */ 	li	$at,0x5c
/*  f00b3e4:	ac430098 */ 	sw	$v1,0x98($v0)
/*  f00b3e8:	ac4300a0 */ 	sw	$v1,0xa0($v0)
/*  f00b3ec:	ac4300a4 */ 	sw	$v1,0xa4($v0)
/*  f00b3f0:	ac4300a8 */ 	sw	$v1,0xa8($v0)
/*  f00b3f4:	ac4300ac */ 	sw	$v1,0xac($v0)
/*  f00b3f8:	14810002 */ 	bne	$a0,$at,.JF0f00b404
/*  f00b3fc:	ac4300b0 */ 	sw	$v1,0xb0($v0)
/*  f00b400:	ac43009c */ 	sw	$v1,0x9c($v0)
.JF0f00b404:
/*  f00b404:	24010026 */ 	li	$at,0x26
/*  f00b408:	14810004 */ 	bne	$a0,$at,.JF0f00b41c
/*  f00b40c:	24050004 */ 	li	$a1,0x4
/*  f00b410:	3c030001 */ 	lui	$v1,0x1
/*  f00b414:	10000009 */ 	b	.JF0f00b43c
/*  f00b418:	3463a5e0 */ 	ori	$v1,$v1,0xa5e0
.JF0f00b41c:
/*  f00b41c:	3c0e8009 */ 	lui	$t6,0x8009
/*  f00b420:	91ce1160 */ 	lbu	$t6,0x1160($t6)
/*  f00b424:	24010001 */ 	li	$at,0x1
/*  f00b428:	3403dac0 */ 	li	$v1,0xdac0
/*  f00b42c:	51c10004 */ 	beql	$t6,$at,.JF0f00b440
/*  f00b430:	2464000f */ 	addiu	$a0,$v1,0xf
/*  f00b434:	3c030001 */ 	lui	$v1,0x1
/*  f00b438:	346309a0 */ 	ori	$v1,$v1,0x9a0
.JF0f00b43c:
/*  f00b43c:	2464000f */ 	addiu	$a0,$v1,0xf
.JF0f00b440:
/*  f00b440:	348f000f */ 	ori	$t7,$a0,0xf
/*  f00b444:	39e4000f */ 	xori	$a0,$t7,0xf
/*  f00b448:	0c0048e2 */ 	jal	mempAlloc
/*  f00b44c:	afa30018 */ 	sw	$v1,0x18($sp)
/*  f00b450:	3c018008 */ 	lui	$at,0x8008
/*  f00b454:	8fa30018 */ 	lw	$v1,0x18($sp)
/*  f00b458:	ac224774 */ 	sw	$v0,0x4774($at)
/*  f00b45c:	3c018008 */ 	lui	$at,0x8008
/*  f00b460:	0fc5bab6 */ 	jal	langReload
/*  f00b464:	ac23477c */ 	sw	$v1,0x477c($at)
/*  f00b468:	8fbf0014 */ 	lw	$ra,0x14($sp)
/*  f00b46c:	27bd0020 */ 	addiu	$sp,$sp,0x20
/*  f00b470:	03e00008 */ 	jr	$ra
/*  f00b474:	00000000 */ 	nop
);
#elif VERSION >= VERSION_PAL_FINAL
GLOBAL_ASM(
glabel langReset
/*  f00b330:	27bdffe0 */ 	addiu	$sp,$sp,-32
/*  f00b334:	3c02800b */ 	lui	$v0,0x800b
/*  f00b338:	3c03800b */ 	lui	$v1,0x800b
/*  f00b33c:	afbf0014 */ 	sw	$ra,0x14($sp)
/*  f00b340:	2463b154 */ 	addiu	$v1,$v1,-20140
/*  f00b344:	2442b040 */ 	addiu	$v0,$v0,-20416
.PF0f00b348:
/*  f00b348:	24420004 */ 	addiu	$v0,$v0,0x4
/*  f00b34c:	1443fffe */ 	bne	$v0,$v1,.PF0f00b348
/*  f00b350:	ac40fffc */ 	sw	$zero,-0x4($v0)
/*  f00b354:	3c02800b */ 	lui	$v0,0x800b
/*  f00b358:	2442b040 */ 	addiu	$v0,$v0,-20416
/*  f00b35c:	24030001 */ 	li	$v1,0x1
/*  f00b360:	2401005c */ 	li	$at,0x5c
/*  f00b364:	ac430098 */ 	sw	$v1,0x98($v0)
/*  f00b368:	ac4300a0 */ 	sw	$v1,0xa0($v0)
/*  f00b36c:	ac4300a4 */ 	sw	$v1,0xa4($v0)
/*  f00b370:	ac4300a8 */ 	sw	$v1,0xa8($v0)
/*  f00b374:	ac4300ac */ 	sw	$v1,0xac($v0)
/*  f00b378:	14810002 */ 	bne	$a0,$at,.PF0f00b384
/*  f00b37c:	ac4300b0 */ 	sw	$v1,0xb0($v0)
/*  f00b380:	ac43009c */ 	sw	$v1,0x9c($v0)
.PF0f00b384:
/*  f00b384:	24010026 */ 	li	$at,0x26
/*  f00b388:	14810004 */ 	bne	$a0,$at,.PF0f00b39c
/*  f00b38c:	24050004 */ 	li	$a1,0x4
/*  f00b390:	3c030001 */ 	lui	$v1,0x1
/*  f00b394:	10000009 */ 	b	.PF0f00b3bc
/*  f00b398:	3463a5e0 */ 	ori	$v1,$v1,0xa5e0
.PF0f00b39c:
/*  f00b39c:	3c0e8009 */ 	lui	$t6,0x8009
/*  f00b3a0:	91ce1040 */ 	lbu	$t6,0x1040($t6)
/*  f00b3a4:	24010001 */ 	li	$at,0x1
/*  f00b3a8:	3403dac0 */ 	li	$v1,0xdac0
/*  f00b3ac:	51c10004 */ 	beql	$t6,$at,.PF0f00b3c0
/*  f00b3b0:	2464000f */ 	addiu	$a0,$v1,0xf
/*  f00b3b4:	3c030001 */ 	lui	$v1,0x1
/*  f00b3b8:	346309a0 */ 	ori	$v1,$v1,0x9a0
.PF0f00b3bc:
/*  f00b3bc:	2464000f */ 	addiu	$a0,$v1,0xf
.PF0f00b3c0:
/*  f00b3c0:	348f000f */ 	ori	$t7,$a0,0xf
/*  f00b3c4:	39e4000f */ 	xori	$a0,$t7,0xf
/*  f00b3c8:	0c004856 */ 	jal	mempAlloc
/*  f00b3cc:	afa30018 */ 	sw	$v1,0x18($sp)
/*  f00b3d0:	3c018008 */ 	lui	$at,0x8008
/*  f00b3d4:	8fa30018 */ 	lw	$v1,0x18($sp)
/*  f00b3d8:	ac224664 */ 	sw	$v0,0x4664($at)
/*  f00b3dc:	3c018008 */ 	lui	$at,0x8008
/*  f00b3e0:	0fc5bdbb */ 	jal	langReload
/*  f00b3e4:	ac23466c */ 	sw	$v1,0x466c($at)
/*  f00b3e8:	8fbf0014 */ 	lw	$ra,0x14($sp)
/*  f00b3ec:	27bd0020 */ 	addiu	$sp,$sp,0x20
/*  f00b3f0:	03e00008 */ 	jr	$ra
/*  f00b3f4:	00000000 */ 	nop
);
#elif VERSION >= VERSION_PAL_BETA
GLOBAL_ASM(
glabel langReset
/*  f00b330:	27bdffe0 */ 	addiu	$sp,$sp,-32
/*  f00b334:	3c02800b */ 	lui	$v0,0x800b
/*  f00b338:	3c03800b */ 	lui	$v1,0x800b
/*  f00b33c:	afbf0014 */ 	sw	$ra,0x14($sp)
/*  f00b340:	2463f1d4 */ 	addiu	$v1,$v1,-3628
/*  f00b344:	2442f0c0 */ 	addiu	$v0,$v0,-3904
.PB0f00b348:
/*  f00b348:	24420004 */ 	addiu	$v0,$v0,0x4
/*  f00b34c:	1443fffe */ 	bne	$v0,$v1,.PB0f00b348
/*  f00b350:	ac40fffc */ 	sw	$zero,-0x4($v0)
/*  f00b354:	3c02800b */ 	lui	$v0,0x800b
/*  f00b358:	2442f0c0 */ 	addiu	$v0,$v0,-3904
/*  f00b35c:	24030001 */ 	li	$v1,0x1
/*  f00b360:	2401005c */ 	li	$at,0x5c
/*  f00b364:	ac430098 */ 	sw	$v1,0x98($v0)
/*  f00b368:	ac4300a0 */ 	sw	$v1,0xa0($v0)
/*  f00b36c:	ac4300a4 */ 	sw	$v1,0xa4($v0)
/*  f00b370:	ac4300a8 */ 	sw	$v1,0xa8($v0)
/*  f00b374:	ac4300ac */ 	sw	$v1,0xac($v0)
/*  f00b378:	14810002 */ 	bne	$a0,$at,.PB0f00b384
/*  f00b37c:	ac4300b0 */ 	sw	$v1,0xb0($v0)
/*  f00b380:	ac43009c */ 	sw	$v1,0x9c($v0)
.PB0f00b384:
/*  f00b384:	24010026 */ 	li	$at,0x26
/*  f00b388:	14810004 */ 	bne	$a0,$at,.PB0f00b39c
/*  f00b38c:	24050004 */ 	li	$a1,0x4
/*  f00b390:	3c030001 */ 	lui	$v1,0x1
/*  f00b394:	10000009 */ 	b	.PB0f00b3bc
/*  f00b398:	3463a5e0 */ 	ori	$v1,$v1,0xa5e0
.PB0f00b39c:
/*  f00b39c:	3c0e8009 */ 	lui	$t6,0x8009
/*  f00b3a0:	91ce2fd0 */ 	lbu	$t6,0x2fd0($t6)
/*  f00b3a4:	24010001 */ 	li	$at,0x1
/*  f00b3a8:	3403dac0 */ 	li	$v1,0xdac0
/*  f00b3ac:	51c10004 */ 	beql	$t6,$at,.PB0f00b3c0
/*  f00b3b0:	2464000f */ 	addiu	$a0,$v1,0xf
/*  f00b3b4:	3c030001 */ 	lui	$v1,0x1
/*  f00b3b8:	346309a0 */ 	ori	$v1,$v1,0x9a0
.PB0f00b3bc:
/*  f00b3bc:	2464000f */ 	addiu	$a0,$v1,0xf
.PB0f00b3c0:
/*  f00b3c0:	348f000f */ 	ori	$t7,$a0,0xf
/*  f00b3c4:	39e4000f */ 	xori	$a0,$t7,0xf
/*  f00b3c8:	0c00490a */ 	jal	mempAlloc
/*  f00b3cc:	afa30018 */ 	sw	$v1,0x18($sp)
/*  f00b3d0:	3c018008 */ 	lui	$at,0x8008
/*  f00b3d4:	8fa30018 */ 	lw	$v1,0x18($sp)
/*  f00b3d8:	ac2265f4 */ 	sw	$v0,0x65f4($at)
/*  f00b3dc:	3c018008 */ 	lui	$at,0x8008
/*  f00b3e0:	0fc5c07b */ 	jal	langReload
/*  f00b3e4:	ac2365fc */ 	sw	$v1,0x65fc($at)
/*  f00b3e8:	8fbf0014 */ 	lw	$ra,0x14($sp)
/*  f00b3ec:	27bd0020 */ 	addiu	$sp,$sp,0x20
/*  f00b3f0:	03e00008 */ 	jr	$ra
/*  f00b3f4:	00000000 */ 	nop
);
#else
void langReset(s32 stagenum)
{
	s32 i;

	for (i = 0; i < ARRAYCOUNT(g_LangBanks); i++) {
		g_LangBanks[i] = NULL;
	}

	g_LangBanks[LANGBANK_GUN] = fileLoadToNew(langGetFileId(LANGBANK_GUN), FILELOADMETHOD_DEFAULT);
	g_LangBanks[LANGBANK_MPMENU] = fileLoadToNew(langGetFileId(LANGBANK_MPMENU), FILELOADMETHOD_DEFAULT);
	g_LangBanks[LANGBANK_PROPOBJ] = fileLoadToNew(langGetFileId(LANGBANK_PROPOBJ), FILELOADMETHOD_DEFAULT);
	g_LangBanks[LANGBANK_MPWEAPONS] = fileLoadToNew(langGetFileId(LANGBANK_MPWEAPONS), FILELOADMETHOD_DEFAULT);
	g_LangBanks[LANGBANK_OPTIONS] = fileLoadToNew(langGetFileId(LANGBANK_OPTIONS), FILELOADMETHOD_DEFAULT);
	g_LangBanks[LANGBANK_MISC] = fileLoadToNew(langGetFileId(LANGBANK_MISC), FILELOADMETHOD_DEFAULT);

	if (stagenum == STAGE_CREDITS) {
		g_LangBanks[LANGBANK_TITLE] = fileLoadToNew(langGetFileId(LANGBANK_TITLE), FILELOADMETHOD_DEFAULT);
	}
}
#endif