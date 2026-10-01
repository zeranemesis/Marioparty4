#pragma once

// The game shelf: every GameCube disc image found in the launcher's game
// folders, inspected and classified against the game catalogue.

#include <string>
#include <vector>

#include "catalog.hpp"
#include "disc.hpp"
#include "i18n.hpp"

namespace partyboard::launcher {

// Declared in shelf order: playable games first.
enum class Compatibility : unsigned char {
    Supported,           // in the catalogue, with a Switch runtime
    NoSwitchRuntime,     // in the catalogue, runtime not ported to Switch yet
    UnsupportedRevision, // right game, a revision its runtime does not accept
    UnsupportedRegion,   // recognised release that is not playable yet (GMPJ01)
    OtherGame,           // a valid GameCube disc the catalogue does not know
    Unreadable,          // container or header could not be read
};

struct GameEntry {
    std::string path;
    std::string fileName;
    std::string coverPath; // optional PNG box art, empty when none was found
    DiscInfo disc;
    DiscError error = DiscError::None;
    Compatibility compatibility = Compatibility::Unreadable;
    const CatalogEntry* catalog = nullptr; // points into the Catalog passed to scanLibrary

    bool launchable() const { return compatibility == Compatibility::Supported; }

    // Title for the shelf: catalogue title (per region), banner long title,
    // header title, then the file name.
    std::string title(Language language) const;
    // "Maker" line from the banner, or empty.
    std::string maker(Language language) const;
    std::string description(Language language) const;
};

Compatibility classifyDisc(const DiscInfo& disc, const Catalog& catalog, const CatalogEntry** entry);

// Scans each directory (and one level of sub-folders, Dolphin style). Cover
// art is looked up as <coversDir>/<GAMEID>.png, then next to the image as
// <image name>.png. Playable games sort first, then by title. `catalog` must
// outlive the returned entries.
std::vector<GameEntry> scanLibrary(const std::vector<std::string>& directories,
                                   const std::string& coversDir,
                                   Language language,
                                   const Catalog& catalog);

BannerLanguage bannerLanguageFor(Language language);

} // namespace partyboard::launcher
