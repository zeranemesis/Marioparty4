#ifndef PARTYBOARD_PORT_DUAL_SCREEN_H
#define PARTYBOARD_PORT_DUAL_SCREEN_H

#ifdef __cplusplus
extern "C" {
#endif

// Publishes the board for the bottom-screen map and applies settings changed there
// (src/port/dual_screen.cpp). Called once per frame on the game thread.
void PartyBoard_DualScreenFrame(void);

#ifdef __cplusplus
}
#endif

#endif
