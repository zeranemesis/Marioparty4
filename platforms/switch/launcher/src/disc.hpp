#pragma once

// GameCube disc image inspection for the launcher's game shelf.
//
// Reads only what a GameCube menu shows: the boot header (game ID, revision,
// internal title) and opening.bnr (the 96x32 banner and its title/maker/
// description blocks). Nothing here validates hashes; the engine still runs
// its own disc verification before booting.
//
// Supported containers:
//   .iso/.gcm   raw image                     header + banner
//   .ciso       Wii Backup Manager sparse     header + banner
//   .gcz        Dolphin zlib blocks           header + banner
//   .wia/.rvz   Dolphin                       header only (banner is packed)

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace partyboard::launcher {

enum class DiscFormat : uint8_t {
    Iso,
    Ciso,
    Gcz,
    Wia,
    Rvz,
    Unknown,
};

const char* discFormatName(DiscFormat format);

// Classifies a file by extension only. Unknown means "not a disc image".
DiscFormat discFormatFromPath(const std::string& path);

constexpr int kBannerWidth = 96;
constexpr int kBannerHeight = 32;

// Order of the six BNR2 (PAL) text blocks. BNR1 discs carry one block.
enum class BannerLanguage : uint8_t {
    English = 0,
    German,
    French,
    Spanish,
    Italian,
    Dutch,
};

struct BannerText {
    std::string shortTitle;
    std::string shortMaker;
    std::string longTitle;
    std::string longMaker;
    std::string description;

    bool empty() const {
        return shortTitle.empty() && longTitle.empty() && shortMaker.empty() && longMaker.empty() &&
               description.empty();
    }
};

struct DiscInfo {
    DiscFormat format = DiscFormat::Unknown;
    std::string gameId;    // six characters, e.g. GMPE01
    std::string makerCode; // two characters, e.g. 01
    uint8_t discNumber = 0;
    uint8_t revision = 0;
    std::string internalTitle;        // boot header title, UTF-8
    std::vector<uint8_t> bannerRgba;  // kBannerWidth * kBannerHeight * 4, empty if absent
    std::vector<BannerText> bannerText;
    uint64_t fileSize = 0;

    char regionCode() const { return gameId.size() >= 4 ? gameId[3] : '\0'; }
    bool hasBanner() const { return !bannerRgba.empty(); }

    // Preferred language block, then English, then any non-empty block.
    const BannerText* text(BannerLanguage preferred) const;
};

enum class DiscError : uint8_t {
    None,
    Open,
    Read,
    UnsupportedContainer,
    NotGameCube,
};

DiscError readDisc(const std::string& path, DiscInfo& out);

// Exposed for the unit tests.
std::string discTextToUtf8(const char* text, size_t maxLen, bool shiftJis);
void decodeRgb5a3(const uint8_t* src, int width, int height, uint8_t* rgba);

} // namespace partyboard::launcher
