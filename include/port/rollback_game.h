#ifndef PARTYBOARD_PORT_ROLLBACK_GAME_H
#define PARTYBOARD_PORT_ROLLBACK_GAME_H

#include "dolphin/types.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef TARGET_PC
/* Same-build, local-process state only. Heaps, rendering, audio and external
 * resources are intentionally outside this snapshot. */
size_t PartyBoard_RollbackGameSize(void);
BOOL PartyBoard_RollbackGameSave(void *destination, size_t capacity);
BOOL PartyBoard_RollbackGameLoad(const void *source, size_t size);
BOOL PartyBoard_RollbackGameSelfTest(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
