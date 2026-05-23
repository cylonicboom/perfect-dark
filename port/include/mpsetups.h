#ifndef _IN_MPSETUPS_H
#define _IN_MPSETUPS_H

#include "types.h"

s32 mpsetupLoadCurrentFile(void);
s32 mpsetupSaveCurrentFile(void);
void mpsetupLoadSetup(s32 slotindex);
s32 mpsetupSaveSetup(s32 slotindex, u8 savefile);
void mpsetupCopyAllFromPak(void);
void mpProfileLoadFromPak(void);
void mpProfileSave(void);

extern s32 g_MpProfileHead;
extern s32 g_MpProfileBody;

#endif
