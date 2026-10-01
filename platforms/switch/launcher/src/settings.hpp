#pragma once

// Launcher options, persisted as a small INI file, and the command line
// handed to the engine NRO.
//
// Engine contract (argv after the NRO path, all optional except the disc),
// parsed by src/port/launch_args.cpp:
//   --disc-image=<path>        same meaning as PARTYBOARD_DISC_IMAGE on desktop
//   --aspect=4:3|stretch|wide  locked 4:3, stretched 16:9, adaptive widescreen
//   --lang=fr|en               game language on PAL discs
//   --mod-list=<active-mods.txt>  CubeShelf mod roots, same as PARTYBOARD_MOD_LIST
//   --launcher=<launcher NRO>  where to return to when the game exits

#include <string>
#include <vector>

#include "i18n.hpp"

namespace partyboard::launcher {

enum class AspectMode : unsigned char { Original43, Stretch169, Wide169 };
enum class LanguagePref : unsigned char { Auto, French, English };

struct Settings {
    bool bootAnimation = true;
    AspectMode aspect = AspectMode::Original43;
    LanguagePref language = LanguagePref::Auto;
    std::string lastGame;   // path of the last launched image, to reselect it
    std::string enginePath; // optional override of the engine NRO location

    Language resolveLanguage(Language system) const;
};

bool loadSettings(const std::string& path, Settings& out);
bool saveSettings(const std::string& path, const Settings& settings);

std::vector<std::string> buildLaunchArgs(const std::string& enginePath,
                                         const std::string& discPath,
                                         const std::string& launcherPath,
                                         const Settings& settings,
                                         Language language,
                                         const std::string& modList = {});

// Joins argv for libnx envSetNextLoad, quoting arguments with spaces.
std::string joinArgv(const std::vector<std::string>& args);

// mkdir -p that understands the "sdmc:/" device prefix.
bool makeDirectories(const std::string& path);

} // namespace partyboard::launcher
