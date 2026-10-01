#pragma once

// CubeShelf's mod folders, read and edited on the SD card.
//
// CubeShelf keeps each game's mods in <data>/Mods/<game id>/: one folder per
// GameBanana mod id, installed.json describing them, and the active-mods.txt
// PartyBoard reads (PARTYBOARD_MOD_LIST on desktop). Copying that Mods folder
// to sdmc:/cubeshelf/Mods/ brings the same mods to the Switch: this module
// re-roots the Windows paths installed.json stores, lets the player switch
// mods on and off and reorder them, and writes the list the engine receives
// as --mod-list. installed.json keeps its original paths, so the folder can
// go back to the PC unchanged.

#include <string>
#include <vector>

namespace partyboard::launcher {

struct InstalledMod {
    int id = 0;
    std::string name;
    long long updated = 0;
    bool enabled = true;
    int priority = 0;
    std::string contentRoot; // as stored by CubeShelf (usually a Windows path)
    std::string sha256;

    std::string resolvedRoot; // the same folder on this SD card
    bool present = false;     // resolvedRoot exists
    bool playerDisabled = false; // switched off from inside the game

    bool active() const { return enabled && !playerDisabled && present; }
};

class ModSet {
public:
    // The mod folder for a disc: <modsRoot>/<GAMEID>_<REV>, then the catalogue
    // id, then <GAMEID>. Empty when none exists.
    static std::string findDirectory(const std::string& modsRoot, const std::vector<std::string>& candidates);

    bool load(const std::string& directory);
    bool save() const;

    // Rewrites active-mods.txt (highest priority first, one SD path per line)
    // and active-mods.json; returns the .txt path, or empty with no active mod.
    std::string writeActiveList() const;

    void toggle(size_t index);
    // Moves a mod up (-1) or down (+1) in load order by swapping priorities.
    bool move(size_t index, int direction);

    const std::vector<InstalledMod>& mods() const { return m_mods; }
    const std::string& directory() const { return m_directory; }
    size_t activeCount() const;

private:
    void sort();

    std::string m_directory;
    std::vector<InstalledMod> m_mods; // highest priority first
};

} // namespace partyboard::launcher
