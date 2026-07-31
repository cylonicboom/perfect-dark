#ifndef _IN_LIB_MAIN_H
#define _IN_LIB_MAIN_H
#include <ultra64.h>
#include "data.h"
#include "types.h"

extern s32 g_MainIsBooting;

void mainInit(void);
void mainProc(void);
#ifndef PLATFORM_N64
// Port: rmon variable overriding doesn't exist; the function body is empty.
// Define it as a static inline no-op so the ~147 call sites compile to nothing
// without needing LTO. (The out-of-line definition in port/src/pdmain.c is
// guarded out; src/lib/main.c is not compiled into the port build.)
static inline void mainOverrideVariable(char *name, void *value)
{
	(void)name;
	(void)value;
}
#else
void mainOverrideVariable(char *name, void *value);
#endif
void mainLoop(void);
void mainTick(void);
void mainEndStage(void);
void mainChangeToStage(s32 stagenum);
void func0000e990(void);
void func0000e9c0(void);
s32 mainGetStageNum(void);

#endif
