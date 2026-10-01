#pragma once

// SD card layout shared by the console build ("sdmc:") and the host preview
// (a scratch directory standing in for the SD card root).
//
//   <root>/partyboard/games/            disc images (one sub-folder level allowed)
//   <root>/switch/partyboard/games/     alternative games folder
//   <root>/partyboard/covers/<ID>.png   optional box art
//   <root>/config/partyboard/launcher.ini
//   <root>/config/partyboard/catalog.json     optional catalogue additions
//   <root>/cubeshelf/Mods/<game id>/          CubeShelf's Mods folder, copied from the PC
//   <root>/switch/partyboard/partyboard.nro   the engine (engines come from the catalogue)
//   <root>/switch/partyboard-launcher.nro     this launcher

#include <string>
#include <vector>

namespace partyboard::launcher {

struct SdLayout {
    std::string root;
    std::vector<std::string> gameDirectories;
    std::string coversDirectory;
    std::string settingsPath;
    std::string userCatalogPath;
    std::string modsDirectory;
    std::string launcherPath;
};

inline SdLayout sdLayout(const std::string& root) {
    SdLayout layout;
    layout.root = root;
    layout.gameDirectories = {root + "/partyboard/games", root + "/switch/partyboard/games"};
    layout.coversDirectory = root + "/partyboard/covers";
    layout.settingsPath = root + "/config/partyboard/launcher.ini";
    layout.userCatalogPath = root + "/config/partyboard/catalog.json";
    layout.modsDirectory = root + "/cubeshelf/Mods";
    layout.launcherPath = root + "/switch/partyboard-launcher.nro";
    return layout;
}

} // namespace partyboard::launcher
