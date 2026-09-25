#pragma once

// Maps a Mario Party 4 disc header (game ID + revision byte) to the game version the port runs as.
// Pure logic with no Aurora or SDL dependency, so the disc validator, the boot code and a
// standalone unit test all share it.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "version.h"

namespace partyboard::version {

enum class GameVersion : uint8_t {
    UsaRev0 = VERSION_NO_ENG0,
    UsaRev1 = VERSION_NO_ENG1,
    PalRev0 = VERSION_NO_PAL0,
    PalRev1 = VERSION_NO_PAL1,
    PalRev2 = VERSION_NO_PAL2,
    Jpn = VERSION_NO_JP,
};

enum class DiscRegion : uint8_t {
    Usa,
    Europe,
    Japan,
};

struct DiscIdentity {
    DiscRegion region;
    // Revision byte as read from the disc header.
    uint8_t revision;
    GameVersion version;
    // False when the disc reports a revision newer than any known one; `version` is then the
    // newest known revision of that region.
    bool exactRevision;
    // Japan needs its own fonts and text encoding, which the port does not handle yet.
    bool supported;
};

// `gameId` is the 6-character ID from the disc header (e.g. "GMPE01"). Returns nothing for
// anything that is not Mario Party 4.
constexpr std::optional<DiscIdentity> identify_disc(std::string_view gameId, uint8_t revision)
{
    struct Known {
        std::string_view id;
        DiscRegion region;
        bool supported;
        // Indexed by revision; the last entry is the newest known revision.
        GameVersion revisions[3];
        uint8_t revisionCount;
    };
    constexpr Known known[] = {
        {"GMPE01", DiscRegion::Usa, true, {GameVersion::UsaRev0, GameVersion::UsaRev1}, 2},
        {"GMPP01", DiscRegion::Europe, true, {GameVersion::PalRev0, GameVersion::PalRev1, GameVersion::PalRev2}, 3},
        {"GMPJ01", DiscRegion::Japan, false, {GameVersion::Jpn}, 1},
    };
    for (const auto &disc : known) {
        if (disc.id != gameId) {
            continue;
        }
        const bool exact = revision < disc.revisionCount;
        return DiscIdentity {
            .region = disc.region,
            .revision = revision,
            .version = disc.revisions[exact ? revision : disc.revisionCount - 1],
            .exactRevision = exact,
            .supported = disc.supported,
        };
    }
    return std::nullopt;
}

constexpr std::string_view region_name(DiscRegion region)
{
    switch (region) {
        case DiscRegion::Usa:
            return "USA";
        case DiscRegion::Europe:
            return "EUR";
        case DiscRegion::Japan:
            return "JPN";
    }
    return "?";
}

// Short label such as "USA Rev 1" or "EUR Rev 2". Japan only ever had one release, so its
// revision is left out unless the disc claims another one. `revisionWord` lets the UI translate "Rev".
inline std::string describe(DiscRegion region, uint8_t revision, std::string_view revisionWord = "Rev")
{
    std::string text(region_name(region));
    if (region == DiscRegion::Japan && revision == 0) {
        return text;
    }
    return text + " " + std::string(revisionWord) + " " + std::to_string(revision);
}

}  // namespace partyboard::version
