#ifndef PARTYBOARD_PORT_PERF_HINT_H
#define PARTYBOARD_PORT_PERF_HINT_H

#ifdef __cplusplus
extern "C" {
#endif

// Android's performance hints (ADPF): the game says how long a frame may take and how long it
// did, so the CPU speeds up before a frame is missed and slows down when there is slack, instead
// of the governor guessing afterwards. Does nothing elsewhere or before Android 13.
void PartyBoard_PerfFrameBegin(void);
void PartyBoard_PerfFrameEnd(void);

#ifdef __cplusplus
}
#endif

#endif
