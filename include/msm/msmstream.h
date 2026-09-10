#ifndef MSMSTREAM_H
#define MSMSTREAM_H

#include "game/msm.h"

s32 msmStreamGetStatus(int streamNo);
#ifdef TARGET_PC
void msmStreamLogicalTick(void);
s32 msmStreamLogicalTicksFor(s32 samples, s32 frequency);
#endif
void msmStreamSetMasterVolume(s32 arg0);
void msmStreamStopAll(s32 speed);
s32 msmStreamStop(int streamNo, s32 speed);
int msmStreamPlay(int streamId, MSM_STREAMPARAM* streamParam);
void msmStreamPeriodicProc(void);
void msmStreamSetOutputMode(s32 arg0);
void msmStreamAmemFree(void);
s32 msmStreamAmemAlloc(void);
s32 msmStreamInit(char* arg0);


#endif
