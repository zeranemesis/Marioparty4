#ifndef _GAME_INIT_H
#define _GAME_INIT_H

#include "dolphin.h"
#include "version.h"

SHARED_SYM extern GXRenderModeObj *RenderMode;
extern OSHeapHandle currentHeapHandle;

extern void *DemoFrameBuffer1;
extern void *DemoFrameBuffer2;
extern void *DemoCurrentBuffer;
extern u32 minimumVcount;
extern float minimumVcountf;
SHARED_SYM extern u32 worstVcount;

#ifdef TARGET_PC
s32 rand8_state_get(void);
void rand8_state_set(s32 state);
#endif

void HuSysInit(GXRenderModeObj *mode);
void HuSysBeforeRender();
void HuSysDoneRender(s32 retrace_count);

#endif
