#ifndef PORT_FRAME_INTERPOLATION_H
#define PORT_FRAME_INTERPOLATION_H

#include "game/hu3d.h"
#include "game/sprite.h"

#ifndef __cplusplus
#include <stdbool.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

void PartyBoard_FrameInterpolationBeginSimulation(void);
void PartyBoard_FrameInterpolationEndSimulation(void);
void PartyBoard_FrameInterpolationReset(void);
void PartyBoard_FrameInterpolationSetEnabled(bool enabled);
void PartyBoard_FrameInterpolationSetStep(float step);
void PartyBoard_FrameInterpolationInvalidateModel(s16 modelId);
void PartyBoard_FrameInterpolationInvalidateCamera(s16 cameraId);
void PartyBoard_FrameInterpolationInvalidateSprite(s16 spriteId);
void PartyBoard_FrameInterpolationInvalidateSpriteGroup(s16 groupId);
bool PartyBoard_FrameInterpolationModel(s16 modelId, HuVecF *pos, HuVecF *rot, HuVecF *scale, Mtx mtx);
bool PartyBoard_FrameInterpolationCamera(s16 cameraId, HU3DCAMERA *camera);
bool PartyBoard_FrameInterpolationSprite(s16 spriteId, HUSPRITE *sprite);
bool PartyBoard_FrameInterpolationSpriteGroup(s16 groupId, HUSPRGRP *group);
bool PartyBoard_FrameInterpolationEnabled(void);
float PartyBoard_FrameInterpolationStep(void);

#ifdef __cplusplus
}
#endif

#endif
