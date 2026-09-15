#include "dolphin.h"
#ifdef TARGET_PC
#include "port/netplay_runtime.h"
#include "port/netplay_state.h"
/* Same per-compiler intrinsic as the allocator: TARGET_PC is not
 * Windows-only, so the guard is the compiler and not the platform. */
#if defined(_MSC_VER)
#include <intrin.h>
#pragma intrinsic(_ReturnAddress)
#define PARTYBOARD_RETURN_ADDRESS() ((uintptr_t)_ReturnAddress())
#elif defined(__GNUC__) || defined(__clang__)
#define PARTYBOARD_RETURN_ADDRESS() ((uintptr_t)__builtin_return_address(0))
#else
#define PARTYBOARD_RETURN_ADDRESS() ((uintptr_t)0)
#endif
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

#ifdef TARGET_PC
/* D31. Who drew, not just how many times. A ring of the most recent draws,
 * written only under netplay and never hashed: the desync report prints it
 * when RNG is the subsystem that differs, and two peers' rings diff to the
 * call sites only one of them visited. 512 entries is several frames at the
 * 40 to 60 draws per frame measured on 2026-09-13. */
#define PARTYBOARD_RNG_RING 512u
u32 partyboardRngRingCaller[PARTYBOARD_RNG_RING];
u32 partyboardRngRingFrame[PARTYBOARD_RNG_RING];
u32 partyboardRngRingWrite;

static void PartyBoard_RngRecord(uintptr_t caller)
{
    u32 slot;
    if (!PartyBoard_NetplayEnabled()) {
        return;
    }
    slot = partyboardRngRingWrite % PARTYBOARD_RNG_RING;
    partyboardRngRingCaller[slot] = (u32)caller;
    partyboardRngRingFrame[slot] = PartyBoard_NetplayFrameForDiagnostics();
    ++partyboardRngRingWrite;
}

void PartyBoard_NetplayRngRingRead(PartyBoardRngDrawSink sink, void *context)
{
    u32 i;
    u32 total = partyboardRngRingWrite < PARTYBOARD_RNG_RING
        ? partyboardRngRingWrite : PARTYBOARD_RNG_RING;
    if (!sink) {
        return;
    }
    for (i = 0; i < total; i++) {
        /* Oldest first, so the reader sees the draws in the order made. */
        u32 slot = (partyboardRngRingWrite - total + i) % PARTYBOARD_RNG_RING;
        sink(context, partyboardRngRingFrame[slot],
            (uintptr_t)partyboardRngRingCaller[slot]);
    }
}
#endif

u32 frand(void) {
#ifdef TARGET_PC
    ++partyboardFrandCalls;
    PartyBoard_RngRecord(PARTYBOARD_RETURN_ADDRESS());
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
    PartyBoard_RngRecord(PARTYBOARD_RETURN_ADDRESS());
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
