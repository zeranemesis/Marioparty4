#pragma once

// RetroAchievements, through the official rcheevos client (rc_client).
//
// The set for Mario Party 4 is written against GameCube RAM addresses. The port
// does not run in that memory; src/port/retroachievements_memory.cpp translates
// each address the set reads to the port's copy of the same variable. An
// address it cannot translate is reported as unreadable, and rcheevos then
// disables every achievement that depends on it -- a missing translation can
// cost an achievement, never award one by accident.
//
// Hardcore is always off: the server downgrades unlocks from a client it has
// not validated, and the RetroAchievements team has to validate one first.

#ifdef __cplusplus
extern "C" {
#endif

// Once, after the settings are loaded and the disc is known.
void PartyBoard_RAInit(void);
void PartyBoard_RAShutdown(void);
// Once per 60 Hz game logic tick, after the tick ran. This is the rate the
// console evaluated achievements at, whatever the display refresh rate.
void PartyBoard_RAGameTick(void);
// Once per rendered frame on the main thread: delivers finished server
// requests and keeps the session alive while no tick runs.
void PartyBoard_RAFramePump(void);

#ifdef __cplusplus
}

#include <cstdint>
#include <string>
#include <vector>

namespace partyboard::ra {

// One achievement of the loaded set, for the achievements window.
struct AchievementInfo {
    uint32_t id;
    std::string title;
    std::string description;
    uint32_t points;
    bool unlocked;
    // "12/20" for achievements that measure progress, empty otherwise.
    std::string progress;
    float progressPercent;
    // The badge as an RmlUi image source, greyed while locked; empty until it
    // has been downloaded (src/port/retroachievements_badges.cpp).
    std::string image;
};

// The set's achievements, empty unless a set is loaded. The server's warning
// entries (such as "Unknown Emulator") are left out.
std::vector<AchievementInfo> achievements();
// Changes whenever an achievement unlocks or the badges finish downloading, so
// a window knows to refresh.
uint32_t revision();

enum class State {
    Unavailable,  // no HTTPS backend in this build
    Disabled,     // switched off in the settings
    LoggedOut,
    LoggingIn,
    LoggedIn,     // logged in, game not (yet) identified
    LoadingGame,
    Playing,      // the set is loaded and being evaluated
    NoSet,        // logged in, but the server knows no set for this disc
};

State state();
// A one-line human-readable status for the settings screen.
std::string statusText();
std::string username();

// The password is passed straight to rcheevos and never stored; only the
// session token the server returns is kept, as every RetroAchievements client
// does.
void loginWithPassword(const std::string& user, const std::string& password);
void logout();

} // namespace partyboard::ra
#endif
