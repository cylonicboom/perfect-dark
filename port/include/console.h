#ifndef _IN_CONSOLE_H
#define _IN_CONSOLE_H

#include <PR/ultratypes.h>
#include "types.h"

void conInit(void);
Gfx *conRender(Gfx *gdl);
void conTick(void);
void conPrint(s32 showmsg, const char *s);
void conPrintf(s32 showmsg, const char *fmt, ...);
void conPrintLn(s32 showmsg, const char *s);
s32 conIsOpen(void);
// Console-only view controls (see /con in netConsoleCommand). The filter
// affects the on-screen ring ONLY - pd.log always keeps everything.
void conSetFilter(const char *f);
const char *conGetFilter(void);
void conSetOpaqueBg(s32 on);
s32 conGetOpaqueBg(void);

#endif
