#include "disc.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <memory>

#include <nod.h>

namespace partyboard::launcher {

namespace {

constexpr uint32_t kGameCubeMagic = 0xC2339F3Du; // boot header 0x1C
constexpr size_t kBootHeaderSize = 0x440;
constexpr size_t kBannerImageOffset = 0x20;
constexpr size_t kBannerImageSize = kBannerWidth * kBannerHeight * 2;
constexpr size_t kBannerTextOffset = kBannerImageOffset + kBannerImageSize; // 0x1820
constexpr size_t kBannerTextSize = 0x140;

uint16_t be16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }

uint32_t be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

std::string lowerExtension(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    const size_t dot = path.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
        return {};
    std::string ext = path.substr(dot + 1);
    for (char& c : ext)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext;
}

uint64_t fileSize(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f)
        return 0;
    const off_t end = fseeko(f, 0, SEEK_END) == 0 ? ftello(f) : 0;
    std::fclose(f);
    return end > 0 ? static_cast<uint64_t>(end) : 0;
}

// Disc access goes through the nod C API: nodlite on the Switch, which is
// where every container (ISO, CISO, GCZ, WIA, RVZ) is decoded.
struct NodDeleter {
    void operator()(NodHandle* h) const { nod_free(h); }
};
using NodPtr = std::unique_ptr<NodHandle, NodDeleter>;

bool readAt(NodHandle* h, uint64_t offset, uint8_t* out, size_t size) {
    if (nod_seek(h, static_cast<int64_t>(offset), 0) != static_cast<int64_t>(offset))
        return false;
    size_t done = 0;
    while (done < size) {
        const int64_t n = nod_read(h, out + done, size - done);
        if (n <= 0)
            return false;
        done += static_cast<size_t>(n);
    }
    return true;
}

DiscFormat formatFromNod(NodFormat format) {
    switch (format) {
    case NOD_FORMAT_ISO: return DiscFormat::Iso;
    case NOD_FORMAT_CISO: return DiscFormat::Ciso;
    case NOD_FORMAT_GCZ: return DiscFormat::Gcz;
    case NOD_FORMAT_WIA: return DiscFormat::Wia;
    case NOD_FORMAT_RVZ: return DiscFormat::Rvz;
    default: return DiscFormat::Unknown;
    }
}

// Windows-1252 0x80-0x9F. Zero marks bytes that are undefined there.
constexpr std::array<uint16_t, 32> kCp1252High = {
    0x20AC, 0,      0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0,      0x017D, 0,
    0,      0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0,      0x017E, 0x0178,
};

void appendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

std::string trimmed(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\n' || s.back() == '\r'))
        s.pop_back();
    size_t start = 0;
    while (start < s.size() && s[start] == ' ')
        ++start;
    return s.substr(start);
}

void parseBootHeader(const uint8_t* header, DiscInfo& out) {
    out.gameId.assign(reinterpret_cast<const char*>(header), 6);
    out.makerCode.assign(reinterpret_cast<const char*>(header + 4), 2);
    out.discNumber = header[6];
    out.revision = header[7];
    const bool japanese = header[3] == 'J';
    out.internalTitle = discTextToUtf8(reinterpret_cast<const char*>(header + 0x20), 0x60, japanese);
}

bool validGameId(const uint8_t* header) {
    for (int i = 0; i < 6; ++i) {
        if (!std::isalnum(header[i]))
            return false;
    }
    return true;
}

// opening.bnr from the root of the data partition, decoded by nod.
void readBanner(NodHandle* disc, DiscInfo& out) {
    NodHandle* rawPartition = nullptr;
    if (nod_disc_open_partition_kind(disc, NOD_PARTITION_KIND_DATA, nullptr, &rawPartition) != NOD_RESULT_OK)
        return;
    const NodPtr partition(rawPartition);
    NodNodeKind kind{};
    uint32_t length = 0;
    const uint32_t index = nod_partition_find_file(partition.get(), "/opening.bnr", &kind, &length);
    if (index == NOD_FST_STOP || kind != NOD_NODE_KIND_FILE || length < kBannerTextOffset + kBannerTextSize)
        return;
    NodHandle* rawFile = nullptr;
    if (nod_partition_open_file(partition.get(), index, &rawFile) != NOD_RESULT_OK)
        return;
    const NodPtr file(rawFile);

    const size_t want = std::min<size_t>(length, kBannerTextOffset + kBannerTextSize * 6);
    std::vector<uint8_t> banner(want);
    if (!readAt(file.get(), 0, banner.data(), banner.size()))
        return;

    const bool bnr1 = std::memcmp(banner.data(), "BNR1", 4) == 0;
    const bool bnr2 = std::memcmp(banner.data(), "BNR2", 4) == 0;
    if (!bnr1 && !bnr2)
        return;

    out.bannerRgba.resize(size_t(kBannerWidth) * kBannerHeight * 4);
    decodeRgb5a3(banner.data() + kBannerImageOffset, kBannerWidth, kBannerHeight, out.bannerRgba.data());

    const bool japanese = out.regionCode() == 'J';
    const size_t blocks = bnr2 ? std::min<size_t>(6, (want - kBannerTextOffset) / kBannerTextSize) : 1;
    for (size_t b = 0; b < blocks; ++b) {
        const char* text = reinterpret_cast<const char*>(banner.data() + kBannerTextOffset + b * kBannerTextSize);
        BannerText block;
        block.shortTitle = discTextToUtf8(text + 0x00, 0x20, japanese);
        block.shortMaker = discTextToUtf8(text + 0x20, 0x20, japanese);
        block.longTitle = discTextToUtf8(text + 0x40, 0x40, japanese);
        block.longMaker = discTextToUtf8(text + 0x80, 0x40, japanese);
        block.description = discTextToUtf8(text + 0xC0, 0x80, japanese);
        out.bannerText.push_back(std::move(block));
    }
}

// nod chooses the container from the file's contents. When it refuses one,
// the extension says what the file meant to be: a raw image that is not a
// GameCube disc, or a container nod cannot read.
DiscError openError(NodResult result, DiscFormat byExtension) {
    if (result == NOD_RESULT_ERR_IO)
        return DiscError::Open;
    const char* message = nod_error_message();
    if (byExtension == DiscFormat::Iso || (message && std::strstr(message, "Wii")))
        return DiscError::NotGameCube;
    return DiscError::UnsupportedContainer;
}

} // namespace

const char* discFormatName(DiscFormat format) {
    switch (format) {
    case DiscFormat::Iso: return "ISO";
    case DiscFormat::Ciso: return "CISO";
    case DiscFormat::Gcz: return "GCZ";
    case DiscFormat::Wia: return "WIA";
    case DiscFormat::Rvz: return "RVZ";
    case DiscFormat::Unknown: break;
    }
    return "?";
}

DiscFormat discFormatFromPath(const std::string& path) {
    const std::string ext = lowerExtension(path);
    if (ext == "iso" || ext == "gcm")
        return DiscFormat::Iso;
    if (ext == "ciso")
        return DiscFormat::Ciso;
    if (ext == "gcz")
        return DiscFormat::Gcz;
    if (ext == "wia")
        return DiscFormat::Wia;
    if (ext == "rvz")
        return DiscFormat::Rvz;
    return DiscFormat::Unknown;
}

const BannerText* DiscInfo::text(BannerLanguage preferred) const {
    const size_t index = static_cast<size_t>(preferred);
    if (index < bannerText.size() && !bannerText[index].empty())
        return &bannerText[index];
    if (!bannerText.empty() && !bannerText.front().empty())
        return &bannerText.front();
    for (const BannerText& block : bannerText) {
        if (!block.empty())
            return &block;
    }
    return nullptr;
}

DiscError readDisc(const std::string& path, DiscInfo& out) {
    out = DiscInfo{};
    out.format = discFormatFromPath(path);
    out.fileSize = fileSize(path);

    NodHandle* rawDisc = nullptr;
    const NodResult result = nod_disc_open(path.c_str(), nullptr, &rawDisc);
    if (result != NOD_RESULT_OK)
        return openError(result, out.format);
    const NodPtr disc(rawDisc);

    NodDiscMeta meta{};
    if (nod_disc_meta(disc.get(), &meta) == NOD_RESULT_OK && formatFromNod(meta.format) != DiscFormat::Unknown)
        out.format = formatFromNod(meta.format);

    std::array<uint8_t, kBootHeaderSize> header{};
    if (!readAt(disc.get(), 0, header.data(), header.size()))
        return DiscError::Read;
    if (be32(header.data() + 0x1C) != kGameCubeMagic || !validGameId(header.data()))
        return DiscError::NotGameCube;
    parseBootHeader(header.data(), out);
    readBanner(disc.get(), out);
    return DiscError::None;
}

std::string discTextToUtf8(const char* text, size_t maxLen, bool shiftJis) {
    std::string out;
    const auto* bytes = reinterpret_cast<const uint8_t*>(text);
    for (size_t i = 0; i < maxLen && bytes[i] != 0; ++i) {
        const uint8_t c = bytes[i];
        if (shiftJis) {
            // The launcher ships no Shift-JIS table: keep ASCII, drop the
            // double-byte and half-width kana runs. Japanese discs fall back to
            // the built-in title for display.
            if ((c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC)) {
                ++i;
                continue;
            }
            if (c >= 0x80)
                continue;
            out.push_back(c == '\n' ? ' ' : static_cast<char>(c));
            continue;
        }
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
        } else if (c < 0xA0) {
            if (const uint16_t mapped = kCp1252High[c - 0x80])
                appendUtf8(out, mapped);
        } else {
            appendUtf8(out, c);
        }
    }
    return trimmed(std::move(out));
}

void decodeRgb5a3(const uint8_t* src, int width, int height, uint8_t* rgba) {
    for (int ty = 0; ty < height; ty += 4) {
        for (int tx = 0; tx < width; tx += 4) {
            for (int y = 0; y < 4; ++y) {
                for (int x = 0; x < 4; ++x, src += 2) {
                    const uint16_t v = be16(src);
                    uint8_t r, g, b, a;
                    if (v & 0x8000) {
                        r = static_cast<uint8_t>(((v >> 10) & 0x1F) * 255 / 31);
                        g = static_cast<uint8_t>(((v >> 5) & 0x1F) * 255 / 31);
                        b = static_cast<uint8_t>((v & 0x1F) * 255 / 31);
                        a = 255;
                    } else {
                        const uint8_t a3 = (v >> 12) & 0x7;
                        a = static_cast<uint8_t>((a3 << 5) | (a3 << 2) | (a3 >> 1));
                        r = static_cast<uint8_t>(((v >> 8) & 0xF) * 17);
                        g = static_cast<uint8_t>(((v >> 4) & 0xF) * 17);
                        b = static_cast<uint8_t>((v & 0xF) * 17);
                    }
                    uint8_t* px = rgba + (size_t(ty + y) * width + (tx + x)) * 4;
                    px[0] = r;
                    px[1] = g;
                    px[2] = b;
                    px[3] = a;
                }
            }
        }
    }
}

} // namespace partyboard::launcher
