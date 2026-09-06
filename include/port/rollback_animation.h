#ifndef PARTYBOARD_ROLLBACK_ANIMATION_H
#define PARTYBOARD_ROLLBACK_ANIMATION_H

#ifndef __cplusplus
#include <stdbool.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Advances model, sprite and texture animation clocks using minimumVcount.
 * No drawing is performed. Call once at the original end-of-render boundary,
 * or with a one-tick clock while replaying a fully restored scene. This does
 * not evaluate geometry/cameras, invoke drawing hooks, or advance wipes. */
void PartyBoard_AnimationAdvance(void);
#ifdef TARGET_PC
bool PartyBoard_AnimationRollbackSelfTest(void);
#endif

#ifdef __cplusplus
}
#endif
#endif
