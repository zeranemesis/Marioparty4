#pragma once
#include <stdint.h>
#include <string.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Read-only canonical numeric export. Names are static diagnostic labels.
 * No pointers/resource IDs or native structure bytes cross this interface.
 * Every exporter must be callable from the game thread at a tick boundary and
 * must never mutate game state, allocate, or block. */
typedef void (*PartyBoardNetplayStateSink)(void *, const char *, uint32_t);
static inline uint32_t PartyBoard_NetplayFloatWord(float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits)); /* IEEE-754 numeric value only. */
    if ((bits & 0x7fffffffU) == 0) return 0;
    if ((bits & 0x7f800000U) == 0x7f800000U && (bits & 0x007fffffU)) return 0x7fc00000U;
    return bits;
}
void PartyBoard_NetplaySequenceState(PartyBoardNetplayStateSink sink, void *context);
void PartyBoard_NetplayDiceState(PartyBoardNetplayStateSink sink, void *context);
void PartyBoard_NetplayProcessState(PartyBoardNetplayStateSink sink, void *context);
void PartyBoard_NetplayOverlayState(PartyBoardNetplayStateSink sink, void *context);
void PartyBoard_NetplayObjectState(PartyBoardNetplayStateSink sink, void *context);
void PartyBoard_NetplaySceneState(PartyBoardNetplayStateSink sink, void *context);
void PartyBoard_NetplayTimerState(PartyBoardNetplayStateSink sink, void *context);
void PartyBoard_NetplayHeapState(PartyBoardNetplayStateSink sink, void *context);
void PartyBoard_NetplayAnimationState(PartyBoardNetplayStateSink sink, void *context);
/* Logical audio state only: what the game can observe and block on. Physical
 * mixing, device buffers and voice DSP state are deliberately excluded. */
void PartyBoard_NetplayAudioState(PartyBoardNetplayStateSink sink, void *context);
/* RNG consumption counters, rebased when a network timeline starts. */
void PartyBoard_NetplayRandomCountersReset(void);
uint32_t PartyBoard_NetplayFrandCalls(void);
uint32_t PartyBoard_NetplayRand8Calls(void);
uint32_t PartyBoard_NetplayBoardRandCalls(void);
#ifdef __cplusplus
}
#endif
