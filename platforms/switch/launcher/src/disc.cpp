#include "disc.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <memory>

#include <zlib.h>

namespace partyboard::launcher {

namespace {

constexpr uint32_t kGameCubeMagic = 0xC2339F3Du; // boot header 0x1C
constexpr uint32_t kGczMagic = 0xB10BC001u;      // little-endian cookie
constexpr size_t kBootHeaderSize = 0x440;
constexpr size_t kBannerImageOffset = 0x20;
constexpr size_t kBannerImageSize = kBannerWidth * kBannerHeight * 2;
constexpr size_t kBannerTextOffset = kBannerImageOffset + kBannerImageSize; // 0x1820
constexpr size_t kBannerTextSize = 0x140;
constexpr size_t kMaxFstSize = 4u * 1024u * 1024u;

uint16_t be16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }

uint32_t be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

uint32_t le32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

uint64_t le64(const uint8_t* p) { return uint64_t(le32(p)) | (uint64_t(le32(p + 4)) << 32); }

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

class File {
public:
    explicit File(const std::string& path) : m_file(std::fopen(path.c_str(), "rb")) {}
    ~File() {
        if (m_file)
            std::fclose(m_file);
    }
    File(const File&) = delete;
    File& operator=(const File&) = delete;

    bool ok() const { return m_file != nullptr; }

    bool read(uint64_t offset, void* dst, size_t size) {
        if (fseeko(m_file, static_cast<off_t>(offset), SEEK_SET) != 0)
            return false;
        return std::fread(dst, 1, size, m_file) == size;
    }

    uint64_t size() {
        if (fseeko(m_file, 0, SEEK_END) != 0)
            return 0;
        const off_t end = ftello(m_file);
        return end > 0 ? static_cast<uint64_t>(end) : 0;
    }

private:
    FILE* m_file;
};

// A view of the decoded disc, whatever the container.
class DiscSource {
public:
    virtual ~DiscSource() = default;
    virtual bool read(uint64_t offset, void* dst, size_t size) = 0;
};

class RawSource final : public DiscSource {
public:
    explicit RawSource(File& file) : m_file(file) {}
    bool read(uint64_t offset, void* dst, size_t size) override { return m_file.read(offset, dst, size); }

private:
    File& m_file;
};

// CISO: 0x8000-byte header ("CISO", LE block size, one presence byte per
// block), followed by the present blocks in order. Absent blocks read as zero.
class CisoSource final : public DiscSource {
public:
    explicit CisoSource(File& file) : m_file(file) {}

    bool open() {
        // Heap buffers: libnx threads have small stacks.
        std::vector<uint8_t> header(0x8000);
        if (!m_file.read(0, header.data(), header.size()) || std::memcmp(header.data(), "CISO", 4) != 0)
            return false;
        m_blockSize = le32(header.data() + 4);
        if (m_blockSize == 0 || m_blockSize > (64u << 20))
            return false;
        m_index.resize(header.size() - 8);
        uint32_t stored = 0;
        for (size_t i = 0; i < m_index.size(); ++i) {
            const bool present = header[8 + i] != 0;
            m_index[i] = present ? static_cast<int32_t>(stored++) : -1;
        }
        return true;
    }

    bool read(uint64_t offset, void* dst, size_t size) override {
        auto* out = static_cast<uint8_t*>(dst);
        while (size > 0) {
            const uint64_t block = offset / m_blockSize;
            const uint64_t within = offset % m_blockSize;
            const size_t chunk = static_cast<size_t>(std::min<uint64_t>(size, m_blockSize - within));
            if (block >= m_index.size())
                return false;
            if (m_index[block] < 0) {
                std::memset(out, 0, chunk);
            } else {
                const uint64_t fileOffset = 0x8000 + uint64_t(m_index[block]) * m_blockSize + within;
                if (!m_file.read(fileOffset, out, chunk))
                    return false;
            }
            out += chunk;
            offset += chunk;
            size -= chunk;
        }
        return true;
    }

private:
    File& m_file;
    uint32_t m_blockSize = 0;
    std::vector<int32_t> m_index;
};

// GCZ (Dolphin CompressedBlob): 32-byte LE header, u64 block pointers, u32
// hashes, then zlib streams. Pointer bit 63 marks a block stored uncompressed.
class GczSource final : public DiscSource {
public:
    GczSource(File& file, uint64_t fileSize) : m_file(file), m_fileSize(fileSize) {}

    bool open() {
        uint8_t header[32];
        if (!m_file.read(0, header, sizeof(header)) || le32(header) != kGczMagic)
            return false;
        m_compressedSize = le64(header + 8);
        m_blockSize = le32(header + 24);
        const uint32_t blocks = le32(header + 28);
        if (m_blockSize == 0 || m_blockSize > (16u << 20) || blocks == 0 || blocks > (1u << 24))
            return false;
        m_pointers.resize(blocks);
        std::vector<uint8_t> raw(size_t(blocks) * 8);
        if (!m_file.read(32, raw.data(), raw.size()))
            return false;
        for (uint32_t i = 0; i < blocks; ++i)
            m_pointers[i] = le64(raw.data() + size_t(i) * 8);
        m_dataOffset = 32 + uint64_t(blocks) * 12;
        return true;
    }

    bool read(uint64_t offset, void* dst, size_t size) override {
        auto* out = static_cast<uint8_t*>(dst);
        while (size > 0) {
            const uint64_t block = offset / m_blockSize;
            const uint64_t within = offset % m_blockSize;
            const size_t chunk = static_cast<size_t>(std::min<uint64_t>(size, m_blockSize - within));
            if (!loadBlock(block))
                return false;
            std::memcpy(out, m_cache.data() + within, chunk);
            out += chunk;
            offset += chunk;
            size -= chunk;
        }
        return true;
    }

private:
    static constexpr uint64_t kUncompressed = 1ull << 63;

    bool loadBlock(uint64_t block) {
        if (block == m_cachedBlock)
            return true;
        if (block >= m_pointers.size())
            return false;

        const uint64_t start = m_pointers[block] & ~kUncompressed;
        const uint64_t end = block + 1 < m_pointers.size() ? (m_pointers[block + 1] & ~kUncompressed)
                                                           : m_compressedSize;
        if (end < start || m_dataOffset + end > m_fileSize)
            return false;

        m_cache.assign(m_blockSize, 0);
        if (m_pointers[block] & kUncompressed) {
            if (!m_file.read(m_dataOffset + start, m_cache.data(), m_blockSize))
                return false;
        } else {
            std::vector<uint8_t> packed(static_cast<size_t>(end - start));
            if (!m_file.read(m_dataOffset + start, packed.data(), packed.size()))
                return false;
            uLongf produced = m_blockSize;
            if (uncompress(m_cache.data(), &produced, packed.data(), static_cast<uLong>(packed.size())) != Z_OK)
                return false;
        }
        m_cachedBlock = block;
        return true;
    }

    File& m_file;
    uint64_t m_fileSize;
    uint64_t m_compressedSize = 0;
    uint32_t m_blockSize = 0;
    uint64_t m_dataOffset = 0;
    std::vector<uint64_t> m_pointers;
    std::vector<uint8_t> m_cache;
    uint64_t m_cachedBlock = ~0ull;
};

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

// Root-level FST lookup of opening.bnr. Returns false when absent.
bool findBanner(DiscSource& source, const uint8_t* header, uint32_t& offset, uint32_t& length) {
    const uint32_t fstOffset = be32(header + 0x424);
    const uint32_t fstSize = be32(header + 0x428);
    if (fstOffset == 0 || fstSize < 12 || fstSize > kMaxFstSize)
        return false;

    std::vector<uint8_t> fst(fstSize);
    if (!source.read(fstOffset, fst.data(), fst.size()))
        return false;

    const uint32_t entries = be32(fst.data() + 8);
    if (entries == 0 || size_t(entries) * 12 > fst.size())
        return false;
    const size_t stringTable = size_t(entries) * 12;

    for (uint32_t i = 1; i < entries;) {
        const uint8_t* entry = fst.data() + size_t(i) * 12;
        const bool directory = entry[0] != 0;
        if (directory) {
            // Skip the whole subtree: the banner always lives at the root.
            const uint32_t next = be32(entry + 8);
            i = next > i ? next : i + 1;
            continue;
        }
        const uint32_t nameOffset = (uint32_t(entry[1]) << 16) | (uint32_t(entry[2]) << 8) | entry[3];
        if (stringTable + nameOffset < fst.size()) {
            const char* name = reinterpret_cast<const char*>(fst.data() + stringTable + nameOffset);
            const size_t maxLen = fst.size() - stringTable - nameOffset;
            const size_t len = strnlen(name, maxLen);
            static constexpr char kBanner[] = "opening.bnr";
            if (len == sizeof(kBanner) - 1) {
                bool match = true;
                for (size_t c = 0; c < len && match; ++c)
                    match = std::tolower(static_cast<unsigned char>(name[c])) == kBanner[c];
                if (match) {
                    offset = be32(entry + 4);
                    length = be32(entry + 8);
                    return true;
                }
            }
        }
        ++i;
    }
    return false;
}

void readBanner(DiscSource& source, const uint8_t* header, DiscInfo& out) {
    uint32_t offset = 0;
    uint32_t length = 0;
    if (!findBanner(source, header, offset, length) || length < kBannerTextOffset + kBannerTextSize)
        return;

    const size_t want = std::min<size_t>(length, kBannerTextOffset + kBannerTextSize * 6);
    std::vector<uint8_t> banner(want);
    if (!source.read(offset, banner.data(), banner.size()))
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

DiscError inspectSource(DiscSource& source, DiscInfo& out) {
    std::array<uint8_t, kBootHeaderSize> header{};
    if (!source.read(0, header.data(), header.size()))
        return DiscError::Read;
    if (be32(header.data() + 0x1C) != kGameCubeMagic || !validGameId(header.data()))
        return DiscError::NotGameCube;
    parseBootHeader(header.data(), out);
    readBanner(source, header.data(), out);
    return DiscError::None;
}

// WIA/RVZ keep the first 0x80 bytes of the disc verbatim in wia_disc_t.
DiscError inspectWia(File& file, DiscInfo& out) {
    uint8_t head[0x58 + 0x80];
    if (!file.read(0, head, sizeof(head)))
        return DiscError::Read;
    if (std::memcmp(head, "WIA\x01", 4) != 0 && std::memcmp(head, "RVZ\x01", 4) != 0)
        return DiscError::UnsupportedContainer;
    const uint8_t* dhead = head + 0x58;
    if (be32(head + 0x48) != 1 || be32(dhead + 0x1C) != kGameCubeMagic || !validGameId(dhead))
        return DiscError::NotGameCube;
    out.gameId.assign(reinterpret_cast<const char*>(dhead), 6);
    out.makerCode.assign(reinterpret_cast<const char*>(dhead + 4), 2);
    out.discNumber = dhead[6];
    out.revision = dhead[7];
    out.internalTitle = discTextToUtf8(reinterpret_cast<const char*>(dhead + 0x20), 0x60, dhead[3] == 'J');
    return DiscError::None;
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

    File file(path);
    if (!file.ok())
        return DiscError::Open;
    out.fileSize = file.size();

    switch (out.format) {
    case DiscFormat::Iso: {
        RawSource source(file);
        return inspectSource(source, out);
    }
    case DiscFormat::Ciso: {
        CisoSource source(file);
        if (!source.open())
            return DiscError::UnsupportedContainer;
        return inspectSource(source, out);
    }
    case DiscFormat::Gcz: {
        GczSource source(file, out.fileSize);
        if (!source.open())
            return DiscError::UnsupportedContainer;
        return inspectSource(source, out);
    }
    case DiscFormat::Wia:
    case DiscFormat::Rvz:
        return inspectWia(file, out);
    case DiscFormat::Unknown:
        break;
    }
    return DiscError::UnsupportedContainer;
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
