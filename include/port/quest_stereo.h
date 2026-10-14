#ifndef PORT_QUEST_STEREO_H
#define PORT_QUEST_STEREO_H

// Meta Quest: the game's 3D world drawn a second time for the headset's eyes,
// as a model standing on the player's table (src/port/quest_stereo.cpp). Does
// nothing elsewhere, or while the headset shows only the flat screen.

#include "game/hu3d.h"
#include "game/object.h"

#ifdef __cplusplus
extern "C" {
#endif

// Around one camera's 3D layers (Hu3DExec): only camera 0 goes to the eyes.
void PartyBoard_StereoBeginCamera(s16 cameraNo);
void PartyBoard_StereoEndCamera(void);
// The camera's view matrix, each time Hu3DCameraSet computes it.
void PartyBoard_StereoCameraView(s32 cameraNo, Mtx view);
// A split screen whose cameras change hands (Mario Speedwagons sorts them by
// rank): the minigame tells, each frame, which camera shows `player` (-1:
// none). The eyes and the interface's part take the local player's.
void PartyBoard_StereoPlayerCamera(s32 player, s32 camera);
// A piece of a scene's set the headset leaves out, by its HSF object name:
// the minigame menu's walls and ceiling (only its curtains and Goomba stay).
BOOL PartyBoard_StereoSetPieceHidden(const char *name);
// Whether the current camera draws into the headset; visibility uses both eyes.
BOOL PartyBoard_StereoActive(void);
// MR board presentation also remains active after the camera's draw scope.
BOOL PartyBoard_StereoBoardPresentation(void);
// Each object drawn for the eyes: its local bounds, while the scene's floor is measured.
void PartyBoard_StereoObserveBounds(Mtx modelView, const HuVecF *min, const HuVecF *max);
// An object the headset should not draw: a backdrop around the player (sky, skybox).
BOOL PartyBoard_StereoBackdrop(float x, float y, float z, float radius);
BOOL PartyBoard_StereoSphereVisible(float x, float y, float z, float radius);
// A piece of a model the game never culls, its bounding sphere in the game
// camera's view space: FALSE when neither eye can see it (hsfdraw.c skips
// it). TRUE while the scene's floor is still measured, or with
// debug.partyboard.cull_objects 0.
BOOL PartyBoard_StereoObjectVisible(float x, float y, float z, float radius);
// Around each object hsfdraw.c draws (ObjDraw): its draws skip the eyes' cut
// when its bounds cannot reach the other eye.
void PartyBoard_StereoObjectBegin(Mtx modelView, const HuVecF *min, const HuVecF *max);
void PartyBoard_StereoObjectEnd(void);
// Before `next` (a board or a minigame) starts: true while the headset asks for
// the table's calibration. Never in an online game.
BOOL PartyBoard_QuestHoldOverlay(OMOVL next);

#ifdef __cplusplus
}
#endif

#endif
