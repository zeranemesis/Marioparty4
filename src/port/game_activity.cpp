#include "port/game_activity.hpp"

extern "C" {
#include "game/gamework_data.h"
#include "game/object.h"
}

namespace partyboard {

GameActivity current_game_activity() noexcept
{
    GameActivity activity;
    const int overlay = static_cast<int>(omcurovl);
    if (overlay >= DLL_w01Dll && overlay <= DLL_w06Dll) {
        activity.scene = GameActivity::Scene::Board;
        activity.board = overlay - DLL_w01Dll;
        // Only a turn the game is actually counting: both set, and in range.
        if (GWSystem.max_turn > 0 && GWSystem.turn > 0 && GWSystem.turn <= GWSystem.max_turn) {
            activity.turn = GWSystem.turn;
            activity.maxTurn = GWSystem.max_turn;
        }
    }
    else if (overlay >= DLL_m300Dll && overlay <= DLL_m463Dll) {
        activity.scene = GameActivity::Scene::Minigame;
    }
    return activity;
}

} // namespace partyboard
