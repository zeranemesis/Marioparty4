// The online game's view of the board: whose turn the board is waiting on, who
// plays whom, where a player's head is on screen, and turning an idle player
// into a CPU. Apart from pad.c on purpose: the PAD regression test builds pad.c
// on its own, without the board, the camera or the game's work areas.
#include "dolphin.h"
#include "game/board/model.h"
#include "game/board/player.h"
#include "game/disp.h"
#include "game/gamework_data.h"
#include "game/hsfex.h"
#include "game/hu3d.h"
#include "game/object.h"
#include "game/objsub.h"
#include "port/netplay_runtime.h"
#include "port/widescreen.h"

#ifdef TARGET_PC

// Only the party boards (w01..w06): the tutorial and the result screens are not
// a turn anyone can hold up.
s32 PartyBoard_NetplayTurnPad(void)
{
    s32 player;
    if (omcurovl < DLL_w01dll || omcurovl > DLL_w06dll) return -1;
    player = GWSystem.player_curr;
    if (player < 0 || player > 3 || GWPlayer[player].com) return -1;
    return GWPlayer[player].port;
}

s32 PartyBoard_NetplayPadCharacter(s32 pad)
{
    s32 player;
    for (player = 0; player < 4; player++) {
        if (GWPlayer[player].port == pad) return GWPlayer[player].character;
    }
    return -1;
}

// Display only: reads the board and the camera, changes nothing.
bool PartyBoard_NetplayPadHeadOnScreen(s32 pad, float *x, float *y)
{
    s32 player;
    s16 model;
    Vec head, view, screen;
    Mtx look;
    HU3DCAMERA *camera = &Hu3DCamera[0];
    if (omcurovl < DLL_w01dll || omcurovl > DLL_w06dll) return false;
    for (player = 0; player < 4; player++) {
        if (GWPlayer[player].port == pad) break;
    }
    if (player == 4) return false;
    model = BoardPlayerModelGet(player);
    if (BoardModelPosGet(model, &head) != 0 || !BoardModelVisibilityGet(model)) return false;
    // Just over the head, where the board itself puts what a player picks up.
    head.y += 280.0f;
    // Behind the camera would project mirrored onto the screen.
    C_MTXLookAt(look, &camera->pos, &camera->up, &camera->target);
    MTXMultVec(look, &head, &view);
    if (view.z > -1.0f) return false;
    Hu3D3Dto2D(&head, 1, &screen);
    // Where it is on the picture, not where a HUD sprite would go.
    *x = PartyBoard_WidescreenPictureX(screen.x) / HU_DISP_WIDTHF;
    *y = screen.y / HU_DISP_HEIGHTF;
    return *x >= 0.0f && *x <= 1.0f && *y >= 0.0f && *y <= 1.0f;
}

bool PartyBoard_NetplayPadCharacters(int characters[4])
{
    s32 pad, player;
    // The menus have not chosen yet; boards and minigames have.
    if ((omcurovl < DLL_w01dll || omcurovl > DLL_w06dll) && omMgIndexGet(omcurovl) == -1) return false;
    for (pad = 0; pad < 4; pad++) {
        characters[pad] = -1;
        for (player = 0; player < 4; player++) {
            if (GWPlayerCfg[player].pad_idx == pad) characters[pad] = GWPlayerCfg[player].character;
        }
    }
    return true;
}

// Both copies: GWPlayer drives the board, and GWPlayerCfg is what the next
// minigame and the next board load read the CPU flag back from. Normal is 1
// (easy, normal, hard, very hard).
void PartyBoard_NetplayPadBecomesCom(s32 pad)
{
    s32 player;
    for (player = 0; player < 4; player++) {
        if (GWPlayer[player].port != pad) continue;
        GWPlayer[player].com = 1;
        GWPlayer[player].diff = 1;
        GWPlayerCfg[player].iscom = 1;
        GWPlayerCfg[player].diff = 1;
    }
}

#endif
