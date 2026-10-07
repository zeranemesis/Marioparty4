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

// The set's rich presence ("Toad's Midway Madness - Turn 12/20 ..."), as last computed, or empty
// while no set is being played. Main thread.
std::string richPresence();

// The password is passed straight to rcheevos and never stored; only the
// session token the server returns is kept, as every RetroAchievements client
// does.
void loginWithPassword(const std::string& user, const std::string& password);
void logout();

// The loaded set: its RetroAchievements game id and title, 0 and empty while
// none is loaded.
uint32_t gameId();
std::string gameTitle();

// CubeShelf keeps one RetroAchievements session for every game it launches.
// When it starts this one, it hands the session over in CUBESHELF_RA_USER and
// CUBESHELF_RA_TOKEN: that session is used for the run and never written to
// the settings, so the settings keep whatever the player had here. Whatever
// happens to a session -- a login or logout made here, the server refusing the
// session CubeShelf gave -- is reported back to it (src/port/ui/cubeshelf.cpp).
bool fromLauncher();
// The account's user name, as the server knows it -- what CubeShelf matches its
// session against, where username() is the name shown. Empty while logged out.
std::string accountName();

enum class SessionEvent {
    None,
    Login,    // logged in here: CubeShelf takes the session for every game
    Logout,   // logged out here: CubeShelf forgets it too
    Rejected, // the server refused the session CubeShelf handed over
};

struct SessionReport {
    SessionEvent event = SessionEvent::None;
    std::string user;
    std::string token; // only for Login
};

// The latest thing to tell CubeShelf about the session, once. Main thread.
SessionReport takeSessionReport();

} // namespace partyboard::ra
#endif
