#pragma once

// A small but complete GameCube disc for nodlite's tests: boot header, bi2,
// apploader, DOL, a nested FST, files with varied contents, and the junk
// padding mastering leaves between files (what RVZ stores as seeds).
// Deterministic, so tests/fixtures/*.rvz (made from it with nod's own
// nodtool, see tools/make-fixtures.sh) always match.

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "lfg.hpp"

namespace nodlite::test {

constexpr uint64_t kDiscSize = 4u << 20;
constexpr char kGameId[] = "GNLE01";

struct TestFile {
    const char* path; // '/'-separated, directories implied
    uint32_t offset;
    uint32_t size;
    uint32_t seed;
};

// Root files, then data/ (with data/sub/), then sound/: FST order.
inline const std::vector<TestFile>& testFiles() {
    static const std::vector<TestFile> files = {
        {"Readme.TXT", 0x10000, 0x321, 1},
        {"opening.bnr", 0x11000, 0x1960, 2},
        {"data/board.bin", 0x20000, 0x23456, 3},
        {"data/minigame.bin", 0x50000, 0x9000, 4},
        {"data/sub/x.dat", 0x60000, 0x10, 5},
        {"sound/music.dat", 0x80000, 0x2A234, 6},
        {"sound/empty.dat", 0xF0000, 0, 7},
    };
    return files;
}

inline void put32(std::vector<uint8_t>& d, size_t at, uint32_t v) {
    d[at] = uint8_t(v >> 24);
    d[at + 1] = uint8_t(v >> 16);
    d[at + 2] = uint8_t(v >> 8);
    d[at + 3] = uint8_t(v);
}

// File contents: noise with runs of zeroes and repeats, so every compressor
// and every RVZ path (data, zeroes, junk) is exercised.
inline void fillContents(uint8_t* out, uint32_t size, uint32_t seed) {
    uint32_t x = 0x9E3779B9u * seed + 1;
    for (uint32_t i = 0; i < size; ++i) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        const uint32_t block = i / 4096;
        if (block % 5 == 3)
            out[i] = 0;
        else if (block % 5 == 4)
            out[i] = static_cast<uint8_t>(i & 0x3F);
        else
            out[i] = static_cast<uint8_t>(x);
    }
}

inline std::vector<uint8_t> buildDisc() {
    std::vector<uint8_t> d(kDiscSize, 0);
    const uint8_t* id = reinterpret_cast<const uint8_t*>(kGameId);

    // Boot header.
    std::memcpy(d.data(), kGameId, 6);
    d[6] = 0; // disc number
    d[7] = 1; // revision
    put32(d, 0x1C, 0xC2339F3Du);
    const char title[] = "nodlite test disc";
    std::memcpy(d.data() + 0x20, title, sizeof(title));

    // bi2 and apploader (header: date, entry, size, trailer size).
    for (int i = 0; i < 0x2000; ++i)
        d[0x440 + i] = static_cast<uint8_t>(i * 7);
    std::memcpy(d.data() + 0x2440, "2026/10/01", 10);
    put32(d, 0x2450, 0x81200000u);
    put32(d, 0x2454, 0x1000);
    put32(d, 0x2458, 0x40);
    fillContents(d.data() + 0x2460, 0x1040, 11);

    // DOL at 0x4000: one text and one data section.
    const uint32_t dol = 0x4000;
    put32(d, 0x420, dol);
    put32(d, dol + 0x00, 0x100);      // text0 file offset
    put32(d, dol + 0x1C, 0x900);      // data0 file offset
    put32(d, dol + 0x48, 0x80003100); // text0 address
    put32(d, dol + 0x90, 0x800);      // text0 size
    put32(d, dol + 0xAC, 0x400);      // data0 size
    put32(d, dol + 0xE0, 0x80003100); // entry point
    fillContents(d.data() + dol + 0x100, 0xC00, 12);

    // FST: root, Readme.TXT, opening.bnr, data/, board, minigame, sub/, x.dat,
    // sound/, music.dat, empty.dat.
    struct Node {
        bool dir;
        const char* name;
        uint32_t a; // file offset, or parent index
        uint32_t b; // file size, or end index
    };
    const auto& files = testFiles();
    const Node nodes[] = {
        {true, "", 0, 11},
        {false, "Readme.TXT", files[0].offset, files[0].size},
        {false, "opening.bnr", files[1].offset, files[1].size},
        {true, "data", 0, 8},
        {false, "board.bin", files[2].offset, files[2].size},
        {false, "minigame.bin", files[3].offset, files[3].size},
        {true, "sub", 3, 8},
        {false, "x.dat", files[4].offset, files[4].size},
        {true, "sound", 0, 11},
        {false, "music.dat", files[5].offset, files[5].size},
        {false, "empty.dat", files[6].offset, files[6].size},
    };
    const uint32_t fst = 0x6000;
    std::string strings;
    std::vector<uint32_t> nameOffsets;
    for (const Node& n : nodes) {
        nameOffsets.push_back(static_cast<uint32_t>(strings.size()));
        if (*n.name)
            strings += std::string(n.name) + '\0';
    }
    const uint32_t count = sizeof(nodes) / sizeof(nodes[0]);
    for (uint32_t i = 0; i < count; ++i) {
        const size_t at = fst + size_t(i) * 12;
        put32(d, at, (nodes[i].dir ? 0x01000000u : 0u) | (i == 0 ? 0 : nameOffsets[i]));
        put32(d, at + 4, nodes[i].a);
        put32(d, at + 8, nodes[i].b);
    }
    std::memcpy(d.data() + fst + count * 12, strings.data(), strings.size());
    const uint32_t fstSize = count * 12 + static_cast<uint32_t>(strings.size());
    put32(d, 0x424, fst);
    put32(d, 0x428, fstSize);
    put32(d, 0x42C, fstSize);

    // Files, then junk everywhere after the FST that no file covers.
    std::vector<std::pair<uint64_t, uint64_t>> used;
    for (const TestFile& f : files) {
        fillContents(d.data() + f.offset, f.size, f.seed);
        used.emplace_back(f.offset, uint64_t(f.offset) + f.size);
    }
    // opening.bnr starts like a real banner.
    std::memcpy(d.data() + files[1].offset, "BNR1", 4);
    uint64_t cursor = fst + fstSize;
    used.emplace_back(kDiscSize, kDiscSize);
    for (const auto& [start, end] : used) {
        if (start > cursor)
            LaggedFibonacci::fillSectorChunked(d.data() + cursor, static_cast<size_t>(start - cursor), id, 0, cursor);
        cursor = std::max(cursor, end);
    }
    return d;
}

} // namespace nodlite::test
