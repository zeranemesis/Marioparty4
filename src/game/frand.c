#include "dolphin.h"
#ifdef TARGET_PC
#include "port/netplay_runtime.h"
#endif

static u32 frand_seed;

#ifdef TARGET_PC
/* Consumption counters. A differing seed proves an extra draw happened; these
 * say which generator drew it and at which call index, which is what a desync
 * report needs. Rebased when a network timeline starts. */
u32 partyboardFrandCalls;
#endif

extern s32 rand8(void);

static inline u32 frandom(u32 param)
{
    s32 rand2, rand3;

    if (param == 0) {
        param = rand8();
#ifdef TARGET_PC
        /* The zero-seed path is also reachable after the online RNG handshake. */
        param ^= PartyBoard_NetplayEnabled() ? 0x4d503452u : (u32)OSGetTime();
#else
        param = param ^ (s64)OSGetTime();
#endif
        param ^= 0xD826BC89;
    }

    rand2 = param / (u32)0x1F31D;
    rand3 = param - (rand2 * 0x1F31D);
    param = rand2 * 0xB14;
    param =  param - rand3 * 0x41A7;
    return param;
}

u32 frand(void) {
#ifdef TARGET_PC
    ++partyboardFrandCalls;
#endif
    return frand_seed = frandom(frand_seed);
}

f32 frandf(void) {
    u32 value = frand();
    f32 ret;
    value &= 0x7FFFFFFF;
    ret = (f32)value/2147483648;
    return ret;
}

u32 frandmod(u32 arg0) {
    u32 ret;
#ifdef TARGET_PC
    ++partyboardFrandCalls;
#endif
    frand_seed = frandom(frand_seed);
#ifdef TARGET_PC
    if (arg0 == 0) {
        ret = (frand_seed & 0x7FFFFFFF);
        return ret;
    }
#endif
    ret = (frand_seed & 0x7FFFFFFF)%arg0;
    return ret;
}

#ifdef TARGET_PC
u32 frand_state_get(void) {
    return frand_seed;
}

void frand_state_set(u32 state) {
    frand_seed = state;
}

#include "port/netplay_state.h"

extern u32 partyboardRand8Calls;
extern u32 partyboardBoardRandCalls;

void PartyBoard_NetplayRandomCountersReset(void) {
    partyboardFrandCalls = 0;
    partyboardRand8Calls = 0;
    partyboardBoardRandCalls = 0;
}

uint32_t PartyBoard_NetplayFrandCalls(void) { return partyboardFrandCalls; }
uint32_t PartyBoard_NetplayRand8Calls(void) { return partyboardRand8Calls; }
uint32_t PartyBoard_NetplayBoardRandCalls(void) { return partyboardBoardRandCalls; }
#endif

#ifdef TARGET_PC
#include "port/rollback_scene.h"
bool PartyBoard_RollbackRandomRegions(PartyBoardRollbackRegionSink sink, void *context)
{
    return sink && sink(context, &frand_seed, sizeof(frand_seed))
        && sink(context, &partyboardFrandCalls, sizeof(partyboardFrandCalls))
        && PartyBoard_RollbackBoardRandomRegions(sink, context);
}
#endif
