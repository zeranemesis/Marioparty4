#pragma once
#include "dolphin/types.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Local same-build state. Not a complete game snapshot or a wire format. */
typedef struct PartyBoardRollbackClock {
    u32 version;
    u32 frandSeed;
    s32 rand8Seed;
    u32 globalCounter;
} PartyBoardRollbackClock;
void PartyBoard_RollbackClockSave(PartyBoardRollbackClock *state);
BOOL PartyBoard_RollbackClockLoad(const PartyBoardRollbackClock *state);
BOOL PartyBoard_RollbackClockSelfTest(void);
#ifdef __cplusplus
}
#endif
