#pragma once

// The game shelf: every GameCube disc image found in the launcher's game
// folders, inspected and classified against what the PartyBoard engine runs.

#include <string>
#include <vector>

#include "disc.hpp"
#include "i18n.hpp"

namespace partyboard::launcher {

enum class Compatibility : unsigned char {
    Supported,         // GMPE01 / GMPP01: what PartyBoard boots
    UnsupportedRegion, // GMPJ01: recognised, not playable yet
    OtherGame,         // a valid GameCube disc, not Mario Party 4
    Unreadable,        // container or header could not be read
};

struct GameEntry {
    std::string path;
    std::string fileName;
    std::string coverPath; // optional PNG box art, empty when none was found
    DiscInfo disc;
    DiscError error = DiscError::None;
    Compatibility compatibility = Compatibility::Unreadable;

    bool launchable() const { return compatibility == Compatibility::Supported; }

    // Title for the shelf: banner long title, built-in title, header title,
    // then the file name.
    std::string title(Language language) const;
    // "Maker" line from the banner, or empty.
    std::string maker(Language language) const;
    std::string description(Language language) const;
};

Compatibility classifyDisc(const DiscInfo& disc);

// Scans each directory (and one level of sub-folders, Dolphin style). Cover
// art is looked up as <coversDir>/<GAMEID>.png, then next to the image as
// <image name>.png. Supported games sort first, then by title.
std::vector<GameEntry> scanLibrary(const std::vector<std::string>& directories,
                                   const std::string& coversDir,
                                   Language language);

BannerLanguage bannerLanguageFor(Language language);

} // namespace partyboard::launcher
