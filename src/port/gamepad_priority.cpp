#include "port/gamepad_priority.hpp"

#include <filesystem>

#include "port/main.h"

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_joystick.h>
#include <SDL3/SDL_log.h>
#include <SDL3/SDL_stdinc.h>

#include <filesystem>
#include <system_error>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace partyboard::input {

void apply_gamepad_priority() noexcept
{
#if defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IOS)
    // The controller settings write controller_ports.dat when the player picks a port by hand;
    // that choice wins over any automatic order.
    std::error_code error;
    if (std::filesystem::exists(PartyBoard_ConfigPath / "controller_ports.dat", error)) {
        return;
    }
    int count = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&count);
    if (ids == nullptr) {
        return;
    }
    int nextPort = 0;
    for (int i = 0; i < count; ++i) {
        SDL_Gamepad *gamepad = SDL_GetGamepadFromID(ids[i]); // only those the game has opened
        if (gamepad == nullptr) {
            continue;
        }
        // A controller has sticks. Android also reports keyboards and remotes with a D-pad as
        // gamepads; one of those took player 1 and hid the screen controls with nothing to play on.
        SDL_Joystick *joystick = SDL_GetGamepadJoystick(gamepad);
        const bool real = joystick != nullptr && SDL_GetNumJoystickAxes(joystick) >= 2;
        const int port = real && nextPort < 4 ? nextPort++ : -1;
        if (SDL_GetGamepadPlayerIndex(gamepad) != port) {
            SDL_SetGamepadPlayerIndex(gamepad, port);
            SDL_Log("Gamepad '%s' -> player %d%s", SDL_GetGamepadName(gamepad) ? SDL_GetGamepadName(gamepad) : "?",
                port + 1, port >= 0 ? "" : " (none: no sticks, not a controller)");
        }
    }
    SDL_free(ids);
#endif
}

} // namespace partyboard::input
