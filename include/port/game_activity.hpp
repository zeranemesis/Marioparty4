#pragma once

// Where the game is, in terms a friend would care about: on which board and which turn, in a
// minigame, or in the menus. Read from the game's own variables in src/port/game_activity.cpp,
// which is the one place that includes the game's headers for it, so UI code stays clear of them.
namespace partyboard {

struct GameActivity {
    enum class Scene { Menus, Board, Minigame };
    Scene scene = Scene::Menus;
    // 0 to 5, the board's place on the disc (Toad, Goomba, Boo, Koopa, Shy Guy, Bowser), when on one.
    int board = -1;
    int turn = 0;
    int maxTurn = 0;
};

GameActivity current_game_activity() noexcept;

} // namespace partyboard
