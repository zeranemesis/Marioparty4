// Disc header to game version mapping for every Mario Party 4 GameCube release.
// Standalone:
//   c++ -std=c++20 -Iinclude tools/tests/disc_version_test.cpp
#include "port/disc_version.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

using namespace partyboard::version;

namespace {

int failures = 0;

void check(bool condition, const char *what)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

void check_disc(const char *id, uint8_t revision, DiscRegion region, GameVersion version, bool exact, bool supported,
    const char *label)
{
    const auto identity = identify_disc(id, revision);
    if (!identity) {
        std::fprintf(stderr, "FAIL: %s rev %u not recognised\n", id, revision);
        ++failures;
        return;
    }
    if (identity->region != region || identity->version != version || identity->exactRevision != exact
        || identity->supported != supported || identity->revision != revision) {
        std::fprintf(stderr, "FAIL: %s rev %u mapped wrongly\n", id, revision);
        ++failures;
    }
    if (describe(identity->region, identity->revision) != label) {
        std::fprintf(stderr, "FAIL: %s rev %u described as '%s', expected '%s'\n", id, revision,
            describe(identity->region, identity->revision).c_str(), label);
        ++failures;
    }
}

} // namespace

int main()
{
    // The six retail releases.
    check_disc("GMPE01", 0, DiscRegion::Usa, GameVersion::UsaRev0, true, true, "USA Rev 0");
    check_disc("GMPE01", 1, DiscRegion::Usa, GameVersion::UsaRev1, true, true, "USA Rev 1");
    check_disc("GMPP01", 0, DiscRegion::Europe, GameVersion::PalRev0, true, true, "EUR Rev 0");
    check_disc("GMPP01", 1, DiscRegion::Europe, GameVersion::PalRev1, true, true, "EUR Rev 1");
    check_disc("GMPP01", 2, DiscRegion::Europe, GameVersion::PalRev2, true, true, "EUR Rev 2");
    check_disc("GMPJ01", 0, DiscRegion::Japan, GameVersion::Jpn, true, false, "JPN");

    // A revision newer than any known one runs as the newest known revision of its region.
    check_disc("GMPE01", 2, DiscRegion::Usa, GameVersion::UsaRev1, false, true, "USA Rev 2");
    check_disc("GMPP01", 7, DiscRegion::Europe, GameVersion::PalRev2, false, true, "EUR Rev 7");
    check_disc("GMPJ01", 1, DiscRegion::Japan, GameVersion::Jpn, false, false, "JPN Rev 1");

    // The UI passes a translated "Rev".
    check(describe(DiscRegion::Europe, 2, "Rév.") == "EUR Rév. 2", "translated revision word");
    check(describe(DiscRegion::Japan, 0, "Rév.") == "JPN", "Japan has no revision word");

    // The values the game code and the address tables index with.
    check(static_cast<int>(GameVersion::UsaRev0) == VERSION_NO_ENG0, "UsaRev0 value");
    check(static_cast<int>(GameVersion::UsaRev1) == VERSION_NO_ENG1, "UsaRev1 value");
    check(static_cast<int>(GameVersion::PalRev0) == VERSION_NO_PAL0, "PalRev0 value");
    check(static_cast<int>(GameVersion::PalRev1) == VERSION_NO_PAL1, "PalRev1 value");
    check(static_cast<int>(GameVersion::PalRev2) == VERSION_NO_PAL2, "PalRev2 value");
    check(static_cast<int>(GameVersion::Jpn) == VERSION_NO_JP, "Jpn value");

    // Anything else is not Mario Party 4.
    check(!identify_disc("GALE01", 2), "another game is refused");
    check(!identify_disc("GMPE02", 0), "another publisher code is refused");
    check(!identify_disc("GMPK01", 0), "an unknown region is refused");
    check(!identify_disc("GMPE", 0), "a truncated ID is refused");
    check(!identify_disc("", 0), "an empty ID is refused");

    // Usable at compile time, so a mistake in the table fails the build of every user.
    static_assert(identify_disc("GMPP01", 1)->version == GameVersion::PalRev1);

    if (failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return EXIT_FAILURE;
    }
    std::puts("disc_version_test: all checks passed");
    return EXIT_SUCCESS;
}
