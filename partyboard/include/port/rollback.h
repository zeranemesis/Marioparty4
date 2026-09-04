#ifndef PARTYBOARD_PORT_ROLLBACK_H
#define PARTYBOARD_PORT_ROLLBACK_H

#include "dolphin/types.h"

#ifndef __cplusplus
#include <stdbool.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Complete controller state exchanged for one player on one 60 Hz tick. */
typedef struct PartyBoardRollbackInput {
    u16 buttons;
    s8 stickX;
    s8 stickY;
    s8 substickX;
    s8 substickY;
    u8 triggerLeft;
    u8 triggerRight;
} PartyBoardRollbackInput;

typedef struct PartyBoardRollbackStats {
    u32 currentFrame;
    u32 rollbackCount;
    u32 resimulatedFrames;
    u32 maximumRollback;
    u32 predictedFrames;
    u32 lateInputs;
} PartyBoardRollbackStats;

/* Runs a deterministic two-player simulation with delayed remote inputs. */
bool PartyBoard_RollbackRunSelfTest(void);

/* True while old ticks are replayed. Irreversible side effects must not be
 * emitted in this phase (audio, rumble, achievements and saves). */
bool PartyBoard_RollbackIsResimulating(void);

#ifdef __cplusplus
}
#endif

#endif
