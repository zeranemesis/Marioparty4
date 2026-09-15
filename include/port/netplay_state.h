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
/* One live allocation: heap id, payload bytes, allocation group, and the
 * return address of the code that asked for it. Diagnostics only - this one
 * DOES carry an address, and it is never hashed. The reader is responsible
 * for turning it into a module-relative offset before two machines compare
 * anything, because a raw address is an ASLR address. */
typedef void (*PartyBoardHeapBlockSink)(void *, int, uint32_t, uint32_t, uintptr_t);
/* One recorded random draw: the simulation frame it was made on, and the
 * return address of the code that asked for it. Diagnostics only, and like
 * the heap census the reader must reduce the address to a module offset
 * before two machines can be compared. */
typedef void (*PartyBoardRngDrawSink)(void *, uint32_t, uintptr_t);
void PartyBoard_NetplayRngRingRead(PartyBoardRngDrawSink sink, void *context);
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
/* Every live block of every ready heap. Used by the desync report when the
 * HEAPS subsystem is the one that differs - see D30. */
void PartyBoard_NetplayHeapCensus(PartyBoardHeapBlockSink sink, void *context);
/* Test-only: allocate and deliberately keep blocks, to make the HEAPS
 * subsystem diverge on purpose and prove the census works. */
void PartyBoard_NetplayHeapInjectLeak(int blocks, uint32_t bytes);
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
