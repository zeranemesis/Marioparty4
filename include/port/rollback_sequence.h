#ifndef PARTYBOARD_ROLLBACK_SEQUENCE_H
#define PARTYBOARD_ROLLBACK_SEQUENCE_H
#include "dolphin/types.h"
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
#ifdef TARGET_PC
/* Local, same-build state. Caller owns lifetime validation and must also
 * restore the referenced sprite/font heaps and processes. No resources are
 * allocated, freed or re-created by Load. Not a portable or complete scene. */
size_t PartyBoard_RollbackSequenceSize(void);
BOOL PartyBoard_RollbackSequenceSave(void *destination, size_t capacity);
BOOL PartyBoard_RollbackSequenceLoad(const void *source, size_t size);
BOOL PartyBoard_RollbackSequenceSelfTest(void);
#endif
#ifdef __cplusplus
}
#endif
#endif
