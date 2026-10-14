#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace partyboard::launcher {

struct Image {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;

    bool empty() const { return rgba.empty(); }
};

// Loads a PNG as RGBA8. Images larger than maxDimension are box-filtered down
// by an integer factor so a 4K cover cannot exhaust the applet's memory.
bool loadPng(const std::string& path, Image& out, int maxDimension = 1024);

bool writePng(const std::string& path, const Image& image);

} // namespace partyboard::launcher
