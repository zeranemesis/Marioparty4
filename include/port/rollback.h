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

/* Apply one recorded raw PAD tick through the game's edge/repeat/clamp logic.
 * connectedMask uses bits 0..3. No hardware polling, audio service or network
 * traffic is performed. The owner must restore the PAD snapshot before replay.
 * This is a replay building block, not a complete game simulation tick. */
bool PartyBoard_RollbackApplyPads(const PartyBoardRollbackInput inputs[4], u8 connectedMask);

/* Runs the same process/sequence body as one normal main-loop tick after
 * applying recorded PAD state. Presentation, audio finalization and the
 * global frame counter remain the caller's explicit responsibility. */
bool PartyBoard_RollbackRunGameLogicTick(const PartyBoardRollbackInput inputs[4], u8 connectedMask);

#ifdef __cplusplus
}
#endif

#endif
