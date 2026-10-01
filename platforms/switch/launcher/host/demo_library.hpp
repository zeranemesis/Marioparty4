#pragma once

// Synthetic GameCube disc images for the tests and the host preview. They
// carry a real boot header, FST and opening.bnr (with procedurally drawn
// banner art) but no game data, so they are safe to generate anywhere.

#include <cstdint>
#include <string>
#include <vector>

#include "disc.hpp"

namespace partyboard::launcher::demo {

struct DiscSpec {
    std::string gameId = "GMPE01";
    uint8_t revision = 0;
    std::string headerTitle = "Mario Party 4";
    bool bnr2 = false;
    std::vector<BannerText> texts; // raw bytes are copied as-is (CP1252/ASCII)
    int art = 0;                   // banner artwork variant
};

std::vector<uint8_t> buildIso(const DiscSpec& spec);
std::vector<uint8_t> wrapCiso(const std::vector<uint8_t>& iso, uint32_t blockSize);
std::vector<uint8_t> wrapGcz(const std::vector<uint8_t>& iso, uint32_t blockSize);
std::vector<uint8_t> wrapRvz(const std::vector<uint8_t>& iso);

// The RGBA banner buildIso() encodes for `art`, before RGB5A3 quantisation.
std::vector<uint8_t> bannerArt(int art);

bool writeFile(const std::string& path, const std::vector<uint8_t>& data);

// Populates <root> like an SD card: four discs in four containers, a broken
// file, a cover, and (when withEngine) a placeholder engine NRO.
void makeDemoSdCard(const std::string& root, bool withEngine);

} // namespace partyboard::launcher::demo
