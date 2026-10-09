#pragma once

// Command-line arguments a launcher passes to the game.
//
// On desktop CubeShelf talks to the game through environment variables
// (PARTYBOARD_DISC_IMAGE, PARTYBOARD_MOD_LIST). A Nintendo Switch homebrew
// loader can only pass argv, so the Switch launcher
// (platforms/switch/launcher) sends the same information as arguments:
//
//   --disc-image=<path>        the disc to boot, like PARTYBOARD_DISC_IMAGE
//   --mod-list=<path>          CubeShelf's active-mods.txt, like PARTYBOARD_MOD_LIST
//   --lang=fr|en               game language for PAL discs
//   --aspect=4:3|stretch|wide  locked 4:3, stretched, adaptive widescreen
//   --launcher=<path>          the launcher to return to when the game exits
//
// Anything else is left alone: other parts of the game (netplay, the online
// companion) parse their own arguments from the same argv.
// Settings taken from here are applied at the config Override layer: they
// last for the session and are never written to the player's config.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace partyboard::launch {

enum class AspectArg : unsigned char {
    Locked43,
    Stretch,
    Wide,
};

enum class LanguageArg : unsigned char {
    English,
    French,
};

struct LaunchArgs {
    std::string discImage;
    std::string modList;
    std::string launcherPath;
    std::optional<LanguageArg> language;
    std::optional<AspectArg> aspect;
    std::vector<std::string> ignored; // known option with a value it does not accept, for the log
};

LaunchArgs parse(int argc, const char* const* argv);
LaunchArgs parse(const std::vector<std::string_view>& args);

// The arguments this process was started with, kept by port_main so the
// platform exit path can return to `launcherPath`.
const LaunchArgs& current();
void setCurrent(LaunchArgs args);

} // namespace partyboard::launch
