#ifndef PORT_WIDESCREEN_H
#define PORT_WIDESCREEN_H

#include "dolphin/mtx.h"

#ifndef __cplusplus
#include <stdbool.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

bool PartyBoard_WidescreenEnabled(void);
float PartyBoard_WidescreenAspectScale(void);
float PartyBoard_WidescreenPerspectiveAspect(float originalAspect);
void PartyBoard_WidescreenOrthoBounds(float originalLeft, float originalRight,
                                      float *renderLeft, float *renderRight);
void PartyBoard_WidescreenAdjustHudMatrix(Mtx matrix, float originalCenterX);

#ifdef __cplusplus
}
#endif

#ifdef TARGET_PC
#define PARTYBOARD_WIDESCREEN_ASPECT(value) PartyBoard_WidescreenPerspectiveAspect(value)
#else
#define PARTYBOARD_WIDESCREEN_ASPECT(value) (value)
#endif

#endif
