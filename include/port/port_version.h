#pragma once

#ifdef __cplusplus

#include <cstdlib>
#include <dvd.h>
#include <initializer_list>
#include <string>
#include <types.h>
#include "port/disc_version.hpp"

/**
 * Functionality for switching game behavior based on the loaded game version (e.g. PAL/JPN, GC/Wii)
 */
namespace partyboard::version {
bool isRegionPal();
bool isRegionJpn();
bool isRegionUsa();

GameVersion getGameVersion();

const DVDDiskID& getDiskID();

// The loaded disc as shown to the player, e.g. "USA Rev 1" or "EUR Rev 2".
std::string describe();

void init();

template<typename T>
struct VersionOption {
    GameVersion mVersion;
    T mValue;

    constexpr VersionOption(GameVersion version, T value) : mVersion(version), mValue(value) {}
};

template<typename T>
const T& versionSelect(const std::initializer_list<VersionOption<T>> options) {
    const auto version = getGameVersion();
    for (const auto& opt : options) {
        if (opt.mVersion == version) {
            return opt.mValue;
        }
    }

    // Unable to find value.
    abort();
}

template<typename T>
const T& versionSelect(const std::initializer_list<VersionOption<T>> options, const T& defaultValue) {
    const auto version = getGameVersion();
    for (const auto& opt : options) {
        if (opt.mVersion == version) {
            return opt.mValue;
        }
    }

    return defaultValue;
}
}  // namespace partyboard::version

#else

u8 partyboard_version_get_version(void);

BOOL partyboard_version_is_pal(void);
BOOL partyboard_version_is_usa(void);
BOOL partyboard_version_is_ntsc(void);

#endif