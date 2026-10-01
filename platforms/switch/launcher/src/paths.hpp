#pragma once

// SD card layout shared by the console build ("sdmc:") and the host preview
// (a scratch directory standing in for the SD card root).
//
//   <root>/partyboard/games/            disc images (one sub-folder level allowed)
//   <root>/switch/partyboard/games/     alternative games folder
//   <root>/partyboard/covers/<ID>.png   optional box art
//   <root>/config/partyboard/launcher.ini
//   <root>/switch/partyboard/partyboard.nro   the engine
//   <root>/switch/partyboard-launcher.nro     this launcher

#include <string>
#include <vector>

namespace partyboard::launcher {

struct SdLayout {
    std::vector<std::string> gameDirectories;
    std::string coversDirectory;
    std::string settingsPath;
    std::vector<std::string> engineCandidates;
    std::string launcherPath;
};

inline SdLayout sdLayout(const std::string& root) {
    SdLayout layout;
    layout.gameDirectories = {root + "/partyboard/games", root + "/switch/partyboard/games"};
    layout.coversDirectory = root + "/partyboard/covers";
    layout.settingsPath = root + "/config/partyboard/launcher.ini";
    layout.engineCandidates = {root + "/switch/partyboard/partyboard.nro", root + "/switch/partyboard.nro"};
    layout.launcherPath = root + "/switch/partyboard-launcher.nro";
    return layout;
}

} // namespace partyboard::launcher
