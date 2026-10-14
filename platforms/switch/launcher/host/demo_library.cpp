#include "demo_library.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <zlib.h>

#include "image.hpp"
#include "json.hpp"
#include "settings.hpp"

namespace partyboard::launcher::demo {

namespace {

constexpr uint32_t kIsoSize = 0x20000;
constexpr uint32_t kFstOffset = 0x10000;
constexpr uint32_t kBannerOffset = 0x12000;

void put32be(std::vector<uint8_t>& out, size_t at, uint32_t v) {
    out[at] = static_cast<uint8_t>(v >> 24);
    out[at + 1] = static_cast<uint8_t>(v >> 16);
    out[at + 2] = static_cast<uint8_t>(v >> 8);
    out[at + 3] = static_cast<uint8_t>(v);
}

void put32le(std::vector<uint8_t>& out, size_t at, uint32_t v) {
    for (int i = 0; i < 4; ++i)
        out[at + i] = static_cast<uint8_t>(v >> (8 * i));
}

void put64le(std::vector<uint8_t>& out, size_t at, uint64_t v) {
    for (int i = 0; i < 8; ++i)
        out[at + i] = static_cast<uint8_t>(v >> (8 * i));
}

void putText(std::vector<uint8_t>& out, size_t at, const std::string& text, size_t max) {
    std::memcpy(out.data() + at, text.data(), std::min(text.size(), max - 1));
}

// UTF-8 to the single-byte encoding GameCube PAL/NTSC-U banners use.
std::string cp1252(const std::string& utf8) {
    std::string out;
    for (size_t i = 0; i < utf8.size(); ++i) {
        const auto c = static_cast<uint8_t>(utf8[i]);
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
        } else if ((c & 0xE0) == 0xC0 && i + 1 < utf8.size()) {
            const uint32_t cp = ((c & 0x1Fu) << 6) | (static_cast<uint8_t>(utf8[++i]) & 0x3Fu);
            out.push_back(cp < 0x100 ? static_cast<char>(cp) : '?');
        } else {
            out.push_back('?');
        }
    }
    return out;
}

// 3x5 pixel font for the banner lettering.
const uint8_t* glyphRows(char c) {
    static const uint8_t M[5] = {5, 7, 7, 5, 5}, A[5] = {2, 5, 7, 5, 5}, R[5] = {6, 5, 6, 5, 5},
                         I[5] = {7, 2, 2, 2, 7}, O[5] = {7, 5, 5, 5, 7}, P[5] = {6, 5, 6, 4, 4},
                         T[5] = {7, 2, 2, 2, 2}, Y[5] = {5, 5, 2, 2, 2}, F[5] = {5, 5, 7, 1, 1},
                         E[5] = {7, 4, 6, 4, 7}, S[5] = {3, 4, 2, 1, 6}, B[5] = {6, 5, 6, 5, 6};
    switch (c) {
    case 'M': return M;
    case 'A': return A;
    case 'R': return R;
    case 'I': return I;
    case 'O': return O;
    case 'P': return P;
    case 'T': return T;
    case 'Y': return Y;
    case '4': return F;
    case 'E': return E;
    case 'S': return S;
    case 'B': return B;
    default: return nullptr;
    }
}

struct Canvas {
    int w, h;
    std::vector<uint8_t> px;

    Canvas(int width, int height) : w(width), h(height), px(size_t(width) * height * 4, 255) {}

    void set(int x, int y, uint32_t rgb) {
        if (x < 0 || y < 0 || x >= w || y >= h)
            return;
        uint8_t* p = px.data() + (size_t(y) * w + x) * 4;
        p[0] = static_cast<uint8_t>(rgb >> 16);
        p[1] = static_cast<uint8_t>(rgb >> 8);
        p[2] = static_cast<uint8_t>(rgb);
        p[3] = 255;
    }

    void gradient(uint32_t top, uint32_t bottom) {
        for (int y = 0; y < h; ++y) {
            const float t = static_cast<float>(y) / static_cast<float>(h - 1);
            auto ch = [&](int shift) {
                const float a = static_cast<float>((top >> shift) & 0xFF);
                const float b = static_cast<float>((bottom >> shift) & 0xFF);
                return static_cast<uint32_t>(a + (b - a) * t) << shift;
            };
            const uint32_t c = ch(16) | ch(8) | ch(0);
            for (int x = 0; x < w; ++x)
                set(x, y, c);
        }
    }

    void disc(int cx, int cy, int r, uint32_t c) {
        for (int y = -r; y <= r; ++y)
            for (int x = -r; x <= r; ++x)
                if (x * x + y * y <= r * r)
                    set(cx + x, cy + y, c);
    }

    void text(int x, int y, const char* s, int scale, uint32_t c, uint32_t shadow) {
        for (int pass = 0; pass < 2; ++pass) {
            int pen = x;
            for (const char* p = s; *p; ++p) {
                if (const uint8_t* rows = glyphRows(*p)) {
                    for (int ry = 0; ry < 5; ++ry)
                        for (int rx = 0; rx < 3; ++rx)
                            if (rows[ry] & (4 >> rx))
                                for (int sy = 0; sy < scale; ++sy)
                                    for (int sx = 0; sx < scale; ++sx)
                                        set(pen + rx * scale + sx + (pass == 0 ? 1 : 0),
                                            y + ry * scale + sy + (pass == 0 ? 1 : 0), pass == 0 ? shadow : c);
                }
                pen += 4 * scale;
            }
        }
    }
};

} // namespace

std::vector<uint8_t> bannerArt(int art) {
    Canvas c(kBannerWidth, kBannerHeight);
    const uint32_t players[4] = {0xFF4D6D, 0x3B82FF, 0xFFD23B, 0x3BD67A};
    switch (art) {
    case 0:
    case 1:
    case 2: {
        const uint32_t tops[3] = {0x58C8FF, 0xFFB15A, 0x6FE3B4};
        const uint32_t bottoms[3] = {0x1E5FD6, 0xE0457B, 0x13808A};
        c.gradient(tops[art], bottoms[art]);
        c.text(5, 3, "MARIO", 1, 0xFFFFFF, 0x10204A);
        c.text(5, 10, "PARTY", 2, 0xFFE14D, 0x7A2E00);
        c.text(66, 4, "4", 6, 0xFF3B4E, 0xFFFFFF);
        for (int i = 0; i < 4; ++i)
            c.disc(8 + i * 9, 27, 3, players[i]);
        break;
    }
    case 4: {
        // A pitch for the football disc.
        c.gradient(0x3DBB5A, 0x0E5A24);
        for (int x = 0; x < kBannerWidth; x += 12)
            for (int y = 0; y < kBannerHeight; ++y)
                if ((x / 12) % 2 == 0)
                    for (int k = 0; k < 6; ++k)
                        c.set(x + k, y, 0x46C866);
        c.disc(76, 16, 10, 0xFFFFFF);
        c.disc(76, 16, 4, 0x202020);
        c.text(5, 8, "SMS", 3, 0xFFFFFF, 0x0B3A16);
        break;
    }
    default: {
        c.gradient(0x6C5CE7, 0x1C1450);
        for (int i = 0; i < 6; ++i)
            c.disc(80 - i * 3, 16, 12 - i * 2, i % 2 ? 0x8B7DFF : 0x4B3DB8);
        c.text(5, 6, "PB", 3, 0xFFFFFF, 0x10103A);
        c.text(5, 23, "TEST", 1, 0xC9C3F5, 0x10103A);
        break;
    }
    }
    return c.px;
}

std::vector<uint8_t> buildIso(const DiscSpec& spec) {
    std::vector<uint8_t> iso(kIsoSize, 0);
    std::memcpy(iso.data(), spec.gameId.data(), std::min<size_t>(6, spec.gameId.size()));
    iso[6] = 0;
    iso[7] = spec.revision;
    put32be(iso, 0x1C, 0xC2339F3Du);
    putText(iso, 0x20, spec.headerTitle, 0x3E0);

    // FST: root, a sub-directory with one file (exercises the skip), and
    // opening.bnr at the root.
    const size_t bannerSize = spec.bnr2 ? 0x1FA0 : 0x1960;
    const char strings[] = "audio\0x.bin\0opening.bnr";
    const uint32_t entries = 4;
    const uint32_t fstSize = entries * 12 + sizeof(strings);
    put32be(iso, 0x424, kFstOffset);
    put32be(iso, 0x428, fstSize);
    auto entry = [&](uint32_t index, bool dir, uint32_t name, uint32_t a, uint32_t b) {
        const size_t at = kFstOffset + index * 12;
        put32be(iso, at, (dir ? 0x01000000u : 0u) | name);
        put32be(iso, at + 4, a);
        put32be(iso, at + 8, b);
    };
    entry(0, true, 0, 0, entries);
    entry(1, true, 0, 0, 3);
    entry(2, false, 6, 0x11000, 16);
    entry(3, false, 12, kBannerOffset, static_cast<uint32_t>(bannerSize));
    std::memcpy(iso.data() + kFstOffset + entries * 12, strings, sizeof(strings));
    std::memcpy(iso.data() + 0x11000, "PARTYBOARD-DEMO!", 16);

    std::memcpy(iso.data() + kBannerOffset, spec.bnr2 ? "BNR2" : "BNR1", 4);
    const std::vector<uint8_t> art = bannerArt(spec.art);
    size_t at = kBannerOffset + 0x20;
    for (int ty = 0; ty < kBannerHeight; ty += 4)
        for (int tx = 0; tx < kBannerWidth; tx += 4)
            for (int y = 0; y < 4; ++y)
                for (int x = 0; x < 4; ++x, at += 2) {
                    const uint8_t* p = art.data() + (size_t(ty + y) * kBannerWidth + tx + x) * 4;
                    const uint16_t v = static_cast<uint16_t>(0x8000 | ((p[0] >> 3) << 10) | ((p[1] >> 3) << 5) | (p[2] >> 3));
                    iso[at] = static_cast<uint8_t>(v >> 8);
                    iso[at + 1] = static_cast<uint8_t>(v);
                }
    const size_t blocks = spec.bnr2 ? 6 : 1;
    for (size_t b = 0; b < blocks && b < spec.texts.size(); ++b) {
        const size_t base = kBannerOffset + 0x1820 + b * 0x140;
        const BannerText& t = spec.texts[b];
        putText(iso, base + 0x00, t.shortTitle, 0x20);
        putText(iso, base + 0x20, t.shortMaker, 0x20);
        putText(iso, base + 0x40, t.longTitle, 0x40);
        putText(iso, base + 0x80, t.longMaker, 0x40);
        putText(iso, base + 0xC0, t.description, 0x80);
    }
    return iso;
}

std::vector<uint8_t> wrapCiso(const std::vector<uint8_t>& iso, uint32_t blockSize) {
    std::vector<uint8_t> out(0x8000, 0);
    std::memcpy(out.data(), "CISO", 4);
    put32le(out, 4, blockSize);
    const size_t blocks = (iso.size() + blockSize - 1) / blockSize;
    for (size_t b = 0; b < blocks; ++b) {
        const size_t start = b * blockSize;
        const size_t end = std::min(iso.size(), start + blockSize);
        const bool used = std::any_of(iso.begin() + start, iso.begin() + end, [](uint8_t v) { return v != 0; });
        out[8 + b] = used ? 1 : 0;
        if (used) {
            out.insert(out.end(), iso.begin() + start, iso.begin() + end);
            out.resize(out.size() + (blockSize - (end - start)), 0);
        }
    }
    return out;
}

std::vector<uint8_t> wrapGcz(const std::vector<uint8_t>& iso, uint32_t blockSize) {
    const uint32_t blocks = static_cast<uint32_t>((iso.size() + blockSize - 1) / blockSize);
    std::vector<uint8_t> data;
    std::vector<uint64_t> pointers;
    std::vector<uint32_t> checksums; // Adler-32 of each stored block
    for (uint32_t b = 0; b < blocks; ++b) {
        std::vector<uint8_t> raw(blockSize, 0);
        const size_t start = size_t(b) * blockSize;
        std::copy(iso.begin() + start, iso.begin() + std::min(iso.size(), start + blockSize), raw.begin());
        uLongf packedSize = compressBound(blockSize);
        std::vector<uint8_t> packed(packedSize);
        // Store every third block raw so both GCZ paths are exercised.
        const bool storeRaw = b % 3 == 1 ||
                              compress2(packed.data(), &packedSize, raw.data(), blockSize, 6) != Z_OK ||
                              packedSize >= blockSize;
        const uint8_t* stored = storeRaw ? raw.data() : packed.data();
        const size_t storedSize = storeRaw ? blockSize : packedSize;
        pointers.push_back(data.size() | (storeRaw ? (1ull << 63) : 0));
        checksums.push_back(static_cast<uint32_t>(adler32(adler32(0, nullptr, 0), stored, static_cast<uInt>(storedSize))));
        data.insert(data.end(), stored, stored + storedSize);
    }
    std::vector<uint8_t> out(32 + blocks * 12, 0);
    put32le(out, 0, 0xB10BC001u);
    put32le(out, 4, 0);
    put64le(out, 8, data.size());
    put64le(out, 16, iso.size());
    put32le(out, 24, blockSize);
    put32le(out, 28, blocks);
    for (uint32_t b = 0; b < blocks; ++b) {
        put64le(out, 32 + size_t(b) * 8, pointers[b]);
        put32le(out, 32 + size_t(blocks) * 8 + size_t(b) * 4, checksums[b]);
    }
    out.insert(out.end(), data.begin(), data.end());
    return out;
}

// RVZ with uncompressed groups (Dolphin's docs/WiaAndRvz.md): one raw data
// range from 0x80 to the end, 32 KiB chunks, 12-byte group entries with data
// offsets stored divided by 4. The SHA-1 fields are left zero: nodlite does
// not check them (Dolphin and nod would).
std::vector<uint8_t> wrapRvz(const std::vector<uint8_t>& iso) {
    constexpr uint32_t kChunk = 0x8000;
    const uint32_t groups = static_cast<uint32_t>((iso.size() + kChunk - 1) / kChunk);
    const size_t disc = 0x48;
    const size_t rawTable = disc + 0xDC;
    const size_t groupTable = rawTable + 0x18;
    std::vector<uint8_t> out((groupTable + size_t(groups) * 12 + 3) & ~size_t(3), 0);
    std::memcpy(out.data(), "RVZ\x01", 4);
    put32be(out, 0x04, 0x01000000); // version
    put32be(out, 0x08, 0x00030000); // compatible version
    put32be(out, 0x0C, 0xDC);       // disc struct size
    put32be(out, 0x24, static_cast<uint32_t>(uint64_t(iso.size()) >> 32));
    put32be(out, 0x28, static_cast<uint32_t>(iso.size()));
    put32be(out, disc + 0x00, 1); // GameCube
    put32be(out, disc + 0x04, 0); // no compression
    put32be(out, disc + 0x0C, kChunk);
    std::memcpy(out.data() + disc + 0x10, iso.data(), 0x80);
    put32be(out, disc + 0x94, 0x30); // partition struct size
    put32be(out, disc + 0xB4, 1);    // one raw data range
    put32be(out, disc + 0xBC, static_cast<uint32_t>(rawTable));
    put32be(out, disc + 0xC0, 0x18);
    put32be(out, disc + 0xC4, groups);
    put32be(out, disc + 0xCC, static_cast<uint32_t>(groupTable));
    put32be(out, disc + 0xD0, groups * 12);
    put32be(out, rawTable + 0x04, 0x80);
    put32be(out, rawTable + 0x0C, static_cast<uint32_t>(iso.size() - 0x80));
    put32be(out, rawTable + 0x14, groups);
    for (uint32_t g = 0; g < groups; ++g) {
        const size_t start = size_t(g) * kChunk;
        const size_t n = std::min<size_t>(kChunk, iso.size() - start);
        put32be(out, groupTable + size_t(g) * 12, static_cast<uint32_t>(out.size() / 4));
        put32be(out, groupTable + size_t(g) * 12 + 4, static_cast<uint32_t>(n));
        out.insert(out.end(), iso.begin() + start, iso.begin() + start + n);
        out.resize((out.size() + 3) & ~size_t(3), 0);
    }
    return out;
}

bool writeFile(const std::string& path, const std::vector<uint8_t>& data) {
    const size_t slash = path.find_last_of('/');
    if (slash != std::string::npos)
        makeDirectories(path.substr(0, slash));
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f)
        return false;
    const bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    return std::fclose(f) == 0 && ok;
}

void makeDemoSdCard(const std::string& root, bool withEngine) {
    const std::string games = root + "/partyboard/games";

    DiscSpec usa;
    usa.gameId = "GMPE01";
    usa.texts = {{"Mario Party 4", "Nintendo", "Mario Party 4", "Hudson Soft / Nintendo",
                  "Up to four friends race across party boards and battle in fifty minigames."}};
    usa.art = 0;
    writeFile(games + "/Mario Party 4 (USA).iso", buildIso(usa));

    DiscSpec pal;
    pal.gameId = "GMPP01";
    pal.bnr2 = true;
    pal.art = 1;
    const BannerText english{"Mario Party 4", "Nintendo", "Mario Party 4", "Hudson Soft / Nintendo",
                             "Up to four friends race across party boards and battle in fifty minigames."};
    const BannerText french{"Mario Party 4", "Nintendo", "Mario Party 4", "Hudson Soft / Nintendo",
                            cp1252("Jusqu'à quatre amis sur des plateaux de fête et plus de cinquante mini-jeux !")};
    pal.texts = {english, english, french, english, english, english};
    writeFile(games + "/Mario Party 4 [GMPP01]/game.ciso", wrapCiso(buildIso(pal), 0x8000));

    DiscSpec japan;
    japan.gameId = "GMPJ01";
    japan.art = 2;
    japan.headerTitle = "\x83\x7D\x83\x8A\x83\x49\x83\x70\x81\x5B\x83\x65\x83\x42\x82\x53";
    japan.texts = {{japan.headerTitle, "Nintendo", japan.headerTitle, "Nintendo", ""}};
    writeFile(root + "/switch/partyboard/games/Mario Party 4 (Japan).gcz", wrapGcz(buildIso(japan), 0x4000));

    DiscSpec test;
    test.gameId = "GPBEZZ";
    test.headerTitle = "PartyBoard Test Disc";
    test.art = 3;
    writeFile(games + "/PartyBoard Test Disc.rvz", wrapRvz(buildIso(test)));

    DiscSpec strikers;
    strikers.gameId = "G4QP01";
    strikers.headerTitle = "Mario Smash Football";
    strikers.art = 4;
    strikers.texts = {{"Mario Smash Football", "Nintendo", "Mario Smash Football", "Next Level Games / Nintendo",
                       "Five-a-side football with no referee."}};
    writeFile(games + "/Mario Smash Football.iso", buildIso(strikers));

    writeFile(games + "/corrupt.iso", std::vector<uint8_t>(0x1000, 0));
    writeFile(games + "/._Mario Party 4 (USA).iso", std::vector<uint8_t>(0x1000, 0x55));

    // A cover for the test disc, to exercise the PNG path.
    Image cover;
    cover.width = 360;
    cover.height = 500;
    cover.rgba.resize(size_t(cover.width) * cover.height * 4);
    for (int y = 0; y < cover.height; ++y) {
        for (int x = 0; x < cover.width; ++x) {
            uint8_t* p = cover.rgba.data() + (size_t(y) * cover.width + x) * 4;
            const float fx = static_cast<float>(x) / cover.width;
            const float fy = static_cast<float>(y) / cover.height;
            const float rings = 0.5f + 0.5f * std::sin(std::hypot(fx - 0.5f, fy - 0.42f) * 60.0f);
            p[0] = static_cast<uint8_t>(40 + 120 * fx + 40 * rings);
            p[1] = static_cast<uint8_t>(30 + 60 * fy);
            p[2] = static_cast<uint8_t>(120 + 100 * (1.0f - fy) + 30 * rings);
            p[3] = 255;
        }
    }
    makeDirectories(root + "/partyboard/covers");
    writePng(root + "/partyboard/covers/GPBEZZ.png", cover);

    // CubeShelf's Mods folder as copied from a PC: Windows paths in
    // installed.json, one folder per GameBanana id, one of them missing and
    // one switched off from inside the game.
    {
        const std::string mods = root + "/cubeshelf/Mods/GMPE01_00";
        const char* pc = "C:\\Users\\Player\\AppData\\Local\\CubeShelf\\Mods\\GMPE01_00\\";
        struct DemoMod {
            int id;
            const char* name;
            bool enabled;
            int priority;
            bool present;
        };
        const DemoMod list[] = {
            {546878, "Boards HD Retexture", true, 120, true},
            {407132, "Minigame Music Remix", true, 110, true},
            {512340, "Bowser Voice Pack", false, 100, true},
            {499001, "Lost Mod", true, 90, false},
        };
        json::Value installed = json::Value::array();
        for (const DemoMod& mod : list) {
            json::Value item = json::Value::object();
            item.set("Id", json::Value::number(mod.id));
            item.set("Name", json::Value::string(mod.name));
            item.set("Updated", json::Value::number(1727000000));
            item.set("Enabled", json::Value::boolean(mod.enabled));
            item.set("Priority", json::Value::number(mod.priority));
            item.set("ContentRoot", json::Value::string(std::string(pc) + std::to_string(mod.id) + "\\files"));
            item.set("Sha256", json::Value::string("0000000000000000000000000000000000000000000000000000000000000000"));
            installed.push(std::move(item));
            if (mod.present) {
                const std::string file = mods + "/" + std::to_string(mod.id) + "/files/data/demo.bin";
                writeFile(file, std::vector<uint8_t>(16, static_cast<uint8_t>(mod.id & 0xFF)));
            }
        }
        const std::string text = json::serialize(installed);
        writeFile(mods + "/installed.json", std::vector<uint8_t>(text.begin(), text.end()));
        const std::string disabled = "[407132]\n";
        writeFile(mods + "/player-disabled.json", std::vector<uint8_t>(disabled.begin(), disabled.end()));
    }

    if (withEngine) {
        const char placeholder[] = "NRO0 placeholder for the PartyBoard engine";
        writeFile(root + "/switch/partyboard/partyboard.nro",
                  std::vector<uint8_t>(placeholder, placeholder + sizeof(placeholder) - 1));
    }
}

} // namespace partyboard::launcher::demo
