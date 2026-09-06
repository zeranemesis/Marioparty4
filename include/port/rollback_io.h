#ifndef PARTYBOARD_ROLLBACK_IO_H
#define PARTYBOARD_ROLLBACK_IO_H

#ifdef TARGET_PC
#include <stdint.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif
#ifdef __cplusplus
extern "C" {
#endif
/* Tracks HuData directory DVD reads only, not every engine I/O source.
 * Begin before any affected writes; End after the last affected write.
 * The initiating call and asynchronous callback each own one activity. */
void PartyBoard_RollbackIOBegin(void);
void PartyBoard_RollbackIOEnd(void);
/* Successful acquisition excludes new tracked writes until Release.
 * Capture and release must run on the same thread, without nesting.
 * Epochs also invalidate snapshots across completed tracked reads. */
bool PartyBoard_RollbackIOTryCapture(uint64_t *epoch);
void PartyBoard_RollbackIORelease(void);
bool PartyBoard_RollbackIOSelfTest(void);
#ifdef __cplusplus
}
#endif
#else
#define PartyBoard_RollbackIOBegin() ((void)0)
#define PartyBoard_RollbackIOEnd() ((void)0)
#endif
#endif
