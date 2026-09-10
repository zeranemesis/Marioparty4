#pragma once
#include <stdint.h>
#include <string.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Read-only canonical numeric export. Names are static diagnostic labels.
 * No pointers/resource IDs or native structure bytes cross this interface. */
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
#ifdef __cplusplus
}
#endif
