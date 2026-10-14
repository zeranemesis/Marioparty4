#pragma once

// The game catalogue, after CubeShelf's games.json: which discs each runtime
// accepts, how to present the game, and which NRO runs it on Switch. Adding a
// game is a catalogue edit, not code.
//
// Loaded from romfs:/catalog.json, then merged with an optional
// sdmc:/config/partyboard/catalog.json (same id replaces, new ids append).

#include <string>
#include <utility>
#include <vector>

#include "disc.hpp"
#include "i18n.hpp"
#include "json.hpp"

namespace partyboard::launcher {

struct Localized {
    std::string fr;
    std::string en;

    const std::string& get(Language language) const {
        return language == Language::French && !fr.empty() ? fr : en.empty() ? fr : en;
    }
};

struct CoverArt {
    std::string front; // resource paths, relative to the launcher's resources
    std::string spine;
};

struct CatalogEntry {
    std::string id; // CubeShelf id, e.g. GMPE01_00
    std::string title;
    std::vector<std::pair<char, std::string>> regionTitles; // e.g. 'P' -> Mario Smash Football
    int year = 0;
    Localized genre;
    Localized players;
    Localized description;
    std::string runtime; // PartyBoard, Ring Out, Strikers...
    // Accepted discs: "GMPE01_00" is that revision only, "G4QE01" every revision.
    std::vector<std::string> discs;
    // Recognised as this game but not playable (e.g. the Japanese release).
    std::vector<std::string> recognised;
    std::vector<std::string> engines; // Switch NROs, relative to the SD root; empty = no Switch port
    bool mods = false;                // reads a CubeShelf active-mods.txt
    std::vector<std::pair<std::string, CoverArt>> covers; // key: region letter, or "*"

    std::string titleFor(char region) const;
    const CoverArt* coverFor(char region) const;
};

enum class DiscMatch : unsigned char {
    None,
    Accepted,
    OtherRevision, // right game, a revision the runtime does not list
    Recognised,
};

// "GMPE01_01": the id CubeShelf uses for one revision of a disc.
std::string revisionId(const DiscInfo& disc);

class Catalog {
public:
    // Merges a catalogue document into this one. False if it cannot be read.
    bool load(const json::Value& document);
    bool loadFile(const std::string& path, std::string* error = nullptr);

    // Index of the entry matching `disc`, or -1, with how it matched.
    int find(const DiscInfo& disc, DiscMatch& match) const;

    const std::vector<CatalogEntry>& games() const { return m_games; }
    bool empty() const { return m_games.empty(); }

    // Mario Party 4 only: what the launcher knows without any catalogue file.
    static Catalog builtin();

private:
    std::vector<CatalogEntry> m_games;
};

} // namespace partyboard::launcher
