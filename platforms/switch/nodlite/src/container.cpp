#include "container.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <list>

#include <bzlib.h>
#include <zlib.h>
#include <zstd.h>

#include "lfg.hpp"

namespace nodlite {

namespace {

constexpr uint32_t kGameCubeMagic = 0xC2339F3Du;
constexpr uint64_t kSectorSize = 0x8000;

uint32_t be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
uint64_t be64(const uint8_t* p) { return (uint64_t(be32(p)) << 32) | be32(p + 4); }
uint32_t le32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
uint64_t le64(const uint8_t* p) { return uint64_t(le32(p)) | (uint64_t(le32(p + 4)) << 32); }

class FileStream final : public Stream {
public:
    explicit FileStream(FILE* file) : m_file(file) {}
    ~FileStream() override { std::fclose(m_file); }

    int64_t readAt(uint64_t offset, void* out, size_t size) override {
        std::lock_guard lock(m_lock);
        if (fseeko(m_file, static_cast<off_t>(offset), SEEK_SET) != 0)
            return -1;
        const size_t got = std::fread(out, 1, size, m_file);
        return got == size || std::feof(m_file) ? static_cast<int64_t>(got) : -1;
    }

    int64_t length() override {
        std::lock_guard lock(m_lock);
        if (fseeko(m_file, 0, SEEK_END) != 0)
            return -1;
        return static_cast<int64_t>(ftello(m_file));
    }

private:
    FILE* m_file;
    std::mutex m_lock;
};

// A few decoded blocks, most recent first. Callers hold the disc lock.
class BlockCache {
public:
    explicit BlockCache(size_t capacity) : m_capacity(capacity) {}

    const std::vector<uint8_t>* find(uint64_t key) {
        for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
            if (it->first == key) {
                m_entries.splice(m_entries.begin(), m_entries, it);
                return &m_entries.front().second;
            }
        }
        return nullptr;
    }

    const std::vector<uint8_t>& insert(uint64_t key, std::vector<uint8_t> data) {
        m_entries.emplace_front(key, std::move(data));
        if (m_entries.size() > m_capacity)
            m_entries.pop_back();
        return m_entries.front().second;
    }

private:
    size_t m_capacity;
    std::list<std::pair<uint64_t, std::vector<uint8_t>>> m_entries;
};

// Serves byte ranges from fixed-size decoded blocks.
class BlockDisc : public Disc {
public:
    bool read(uint64_t offset, void* out, size_t size) override {
        if (offset + size > m_size || offset + size < offset)
            return false;
        std::lock_guard lock(m_lock);
        auto* dst = static_cast<uint8_t*>(out);
        while (size > 0) {
            const uint64_t block = offset / m_blockSize;
            const uint64_t within = offset % m_blockSize;
            const size_t n = static_cast<size_t>(std::min<uint64_t>(size, m_blockSize - within));
            const std::vector<uint8_t>* data = m_cache.find(block);
            if (!data) {
                std::vector<uint8_t> decoded(m_blockSize, 0);
                if (!decodeBlock(block, decoded))
                    return false;
                data = &m_cache.insert(block, std::move(decoded));
            }
            std::memcpy(dst, data->data() + within, n);
            dst += n;
            offset += n;
            size -= n;
        }
        return true;
    }

protected:
    // Fills `out` (block size bytes, zeroed) with block `index`.
    virtual bool decodeBlock(uint64_t index, std::vector<uint8_t>& out) = 0;

    std::unique_ptr<Stream> m_stream;
    std::mutex m_lock;
    BlockCache m_cache{8};
};

class IsoDisc final : public Disc {
public:
    explicit IsoDisc(std::unique_ptr<Stream> stream, uint64_t size) : m_stream(std::move(stream)) {
        m_size = size;
        m_format = Format::Iso;
    }

    bool read(uint64_t offset, void* out, size_t size) override {
        return offset + size <= m_size && m_stream->readExact(offset, out, size);
    }

private:
    std::unique_ptr<Stream> m_stream;
};

class CisoDisc final : public BlockDisc {
public:
    bool open(std::unique_ptr<Stream> stream, std::string& error) {
        m_stream = std::move(stream);
        std::vector<uint8_t> header(0x8000);
        if (!m_stream->readExact(0, header.data(), header.size()))
            return error = "CISO: truncated header", false;
        m_blockSize = le32(header.data() + 4);
        if (m_blockSize < 0x8000 || m_blockSize > (64u << 20))
            return error = "CISO: invalid block size", false;
        m_index.assign(header.size() - 8, -1);
        int64_t stored = 0;
        uint64_t lastBlock = 0;
        for (size_t i = 0; i < m_index.size(); ++i) {
            if (header[8 + i]) {
                m_index[i] = stored++;
                lastBlock = i + 1;
            }
        }
        // CISO does not record the disc size; nod assumes a GameCube disc.
        const uint64_t used = lastBlock * m_blockSize;
        m_size = used <= kMiniDvdSize ? kMiniDvdSize : used;
        m_format = Format::Ciso;
        return true;
    }

protected:
    bool decodeBlock(uint64_t index, std::vector<uint8_t>& out) override {
        if (index >= m_index.size() || m_index[index] < 0)
            return true; // absent: zeroes
        return m_stream->readExact(0x8000 + uint64_t(m_index[index]) * m_blockSize, out.data(), m_blockSize);
    }

private:
    std::vector<int64_t> m_index;
};

class GczDisc final : public BlockDisc {
public:
    bool open(std::unique_ptr<Stream> stream, std::string& error) {
        m_stream = std::move(stream);
        uint8_t header[32];
        if (!m_stream->readExact(0, header, sizeof(header)))
            return error = "GCZ: truncated header", false;
        m_compressedSize = le64(header + 8);
        m_size = le64(header + 16);
        m_blockSize = le32(header + 24);
        const uint32_t blocks = le32(header + 28);
        if (m_blockSize == 0 || m_blockSize > (16u << 20) || blocks == 0 || blocks > (1u << 24))
            return error = "GCZ: invalid header", false;
        // Block pointers, then one Adler-32 per stored block, then the data.
        std::vector<uint8_t> raw(size_t(blocks) * 12);
        if (!m_stream->readExact(32, raw.data(), raw.size()))
            return error = "GCZ: truncated block table", false;
        m_pointers.resize(blocks);
        m_checksums.resize(blocks);
        for (uint32_t i = 0; i < blocks; ++i) {
            m_pointers[i] = le64(raw.data() + size_t(i) * 8);
            m_checksums[i] = le32(raw.data() + size_t(blocks) * 8 + size_t(i) * 4);
        }
        m_dataOffset = 32 + uint64_t(blocks) * 12;
        m_format = Format::Gcz;
        m_compression = Compression::Deflate;
        return true;
    }

protected:
    bool decodeBlock(uint64_t index, std::vector<uint8_t>& out) override {
        constexpr uint64_t kRaw = 1ull << 63;
        if (index >= m_pointers.size())
            return true;
        const uint64_t start = m_pointers[index] & ~kRaw;
        const uint64_t end = index + 1 < m_pointers.size() ? (m_pointers[index + 1] & ~kRaw) : m_compressedSize;
        if (end < start || end - start > m_blockSize + 0x1000)
            return false;
        std::vector<uint8_t> stored(static_cast<size_t>(end - start));
        if (!m_stream->readExact(m_dataOffset + start, stored.data(), stored.size()))
            return false;
        // Dolphin writes the checksum of the stored bytes; nod refuses a
        // mismatch too, so a damaged image fails loudly instead of feeding
        // the game garbage.
        if (adler32(adler32(0, nullptr, 0), stored.data(), static_cast<uInt>(stored.size())) != m_checksums[index])
            return false;
        // The last block may be shorter than the block size.
        const size_t expected = static_cast<size_t>(std::min<uint64_t>(m_blockSize, m_size - index * m_blockSize));
        if (m_pointers[index] & kRaw) {
            std::memcpy(out.data(), stored.data(), std::min(expected, stored.size()));
            return true;
        }
        uLongf produced = static_cast<uLongf>(out.size());
        return uncompress(out.data(), &produced, stored.data(), static_cast<uLong>(stored.size())) == Z_OK;
    }

private:
    uint64_t m_compressedSize = 0;
    uint64_t m_dataOffset = 0;
    std::vector<uint64_t> m_pointers;
    std::vector<uint32_t> m_checksums;
};

// WIA/RVZ. Field layout from nod's io/wia.rs and Dolphin's docs/WiaAndRvz.md.
// Groups are chunk-sized from the start of their raw data range, which is
// sector- but not necessarily chunk-aligned, so this reader does its own
// addressing instead of BlockDisc's fixed grid.
class WiaDisc final : public Disc {
public:
    bool open(std::unique_ptr<Stream> stream, std::string& error) {
        m_stream = std::move(stream);
        uint8_t head[0x48];
        if (!m_stream->readExact(0, head, sizeof(head)))
            return error = "WIA/RVZ: truncated header", false;
        m_rvz = std::memcmp(head, "RVZ\x01", 4) == 0;
        if (be32(head + 8) < 0x30000)
            return error = "WIA/RVZ: version too old", false;
        const uint32_t discStructSize = be32(head + 0x0C);
        m_size = be64(head + 0x24);

        std::vector<uint8_t> disc(std::max<size_t>(discStructSize, 0xDC), 0);
        if (discStructSize < 0x90 || discStructSize > 0x1000 ||
            !m_stream->readExact(0x48, disc.data(), discStructSize))
            return error = "WIA/RVZ: invalid disc header", false;
        if (be32(disc.data()) != 1)
            return error = "WIA/RVZ: Wii discs are not supported, only GameCube", false;
        switch (be32(disc.data() + 4)) {
        case 0: m_compression = Compression::None; break;
        case 2: m_compression = Compression::Bzip2; break;
        case 5: m_compression = Compression::Zstandard; break;
        case 1: return error = "WIA: purge compression is not supported, convert with Dolphin or nodtool", false;
        default: return error = "WIA/RVZ: LZMA compression is not supported, use zstd (RVZ) or ISO", false;
        }
        m_level = static_cast<int32_t>(be32(disc.data() + 8));
        m_blockSize = be32(disc.data() + 0x0C);
        if (m_blockSize < kSectorSize || m_blockSize % kSectorSize != 0 || m_blockSize > (64u << 20))
            return error = "WIA/RVZ: invalid chunk size", false;
        std::memcpy(m_discHead, disc.data() + 0x10, sizeof(m_discHead));
        if (be32(m_discHead + 0x1C) != kGameCubeMagic)
            return error = "WIA/RVZ: not a GameCube disc", false;

        const uint32_t rawCount = be32(disc.data() + 0xB4);
        const uint64_t rawOffset = be64(disc.data() + 0xB8);
        const uint32_t rawSize = be32(disc.data() + 0xC0);
        const uint32_t groupCount = be32(disc.data() + 0xC4);
        const uint64_t groupOffset = be64(disc.data() + 0xC8);
        const uint32_t groupSize = be32(disc.data() + 0xD0);
        if (rawCount > (1u << 20) || groupCount > (1u << 24))
            return error = "WIA/RVZ: implausible table sizes", false;

        std::vector<uint8_t> rawTable;
        if (!readTable(rawOffset, rawSize, size_t(rawCount) * 0x18, rawTable))
            return error = "WIA/RVZ: cannot read the raw data table", false;
        for (uint32_t i = 0; i < rawCount; ++i) {
            const uint8_t* p = rawTable.data() + size_t(i) * 0x18;
            RawData rd;
            const uint64_t offset = be64(p);
            rd.start = offset / kSectorSize * kSectorSize;
            rd.end = offset + be64(p + 8);
            rd.groupIndex = be32(p + 0x10);
            rd.groupCount = be32(p + 0x14);
            m_raw.push_back(rd);
        }

        const size_t entrySize = m_rvz ? 12 : 8;
        std::vector<uint8_t> groupTable;
        if (!readTable(groupOffset, groupSize, size_t(groupCount) * entrySize, groupTable))
            return error = "WIA/RVZ: cannot read the group table", false;
        for (uint32_t i = 0; i < groupCount; ++i) {
            const uint8_t* p = groupTable.data() + size_t(i) * entrySize;
            Group g;
            g.offset = uint64_t(be32(p)) * 4;
            const uint32_t sizeAndFlag = be32(p + 4);
            if (m_rvz) {
                g.size = sizeAndFlag & 0x7FFFFFFFu;
                g.compressed = (sizeAndFlag & 0x80000000u) != 0;
                g.packedSize = be32(p + 8);
            } else {
                g.size = sizeAndFlag;
                g.compressed = true; // WIA groups always use the disc's method
            }
            m_groups.push_back(g);
        }
        m_format = m_rvz ? Format::Rvz : Format::Wia;
        return true;
    }

    bool read(uint64_t offset, void* out, size_t size) override {
        if (offset + size > m_size || offset + size < offset)
            return false;
        std::lock_guard lock(m_lock);
        auto* dst = static_cast<uint8_t*>(out);
        while (size > 0) {
            const RawData* range = nullptr;
            for (const RawData& rd : m_raw) {
                if (offset >= rd.start && offset < rd.end) {
                    range = &rd;
                    break;
                }
            }
            if (!range) {
                // Outside every raw data range (a short disc tail): zeroes,
                // up to the next range if there is one.
                uint64_t next = m_size;
                for (const RawData& rd : m_raw) {
                    if (rd.start > offset)
                        next = std::min(next, rd.start);
                }
                const size_t n = static_cast<size_t>(std::min<uint64_t>(size, next - offset));
                std::memset(dst, 0, n);
                dst += n;
                offset += n;
                size -= n;
                continue;
            }
            const uint64_t rel = (offset - range->start) / m_blockSize;
            const uint64_t groupIndex = range->groupIndex + rel;
            if (rel >= range->groupCount || groupIndex >= m_groups.size())
                return false;
            const uint64_t groupStart = range->start + rel * m_blockSize;
            const size_t groupSize = static_cast<size_t>(std::min<uint64_t>(range->end - groupStart, m_blockSize));
            const std::vector<uint8_t>* data = m_cache.find(groupIndex);
            if (!data) {
                std::vector<uint8_t> decoded(groupSize, 0);
                if (!decodeGroup(m_groups[groupIndex], groupStart, groupSize, decoded))
                    return false;
                if (groupStart == 0)
                    std::memcpy(decoded.data(), m_discHead, std::min(sizeof(m_discHead), decoded.size()));
                data = &m_cache.insert(groupIndex, std::move(decoded));
            }
            const uint64_t within = offset - groupStart;
            const size_t n = static_cast<size_t>(std::min<uint64_t>(size, data->size() - within));
            std::memcpy(dst, data->data() + within, n);
            dst += n;
            offset += n;
            size -= n;
        }
        return true;
    }

private:
    struct RawData {
        uint64_t start = 0; // rounded down to a sector, as nod and Dolphin do
        uint64_t end = 0;
        uint32_t groupIndex = 0;
        uint32_t groupCount = 0;
    };
    struct Group {
        uint64_t offset = 0;
        uint32_t size = 0;
        bool compressed = false;
        uint32_t packedSize = 0;
    };

    bool decompress(const std::vector<uint8_t>& in, uint8_t* out, size_t capacity, size_t& produced) const {
        switch (m_compression) {
        case Compression::None:
            if (in.size() > capacity)
                return false;
            std::memcpy(out, in.data(), in.size());
            produced = in.size();
            return true;
        case Compression::Zstandard: {
            const size_t n = ZSTD_decompress(out, capacity, in.data(), in.size());
            if (ZSTD_isError(n))
                return false;
            produced = n;
            return true;
        }
        case Compression::Bzip2: {
            unsigned int n = static_cast<unsigned int>(capacity);
            if (BZ2_bzBuffToBuffDecompress(reinterpret_cast<char*>(out), &n,
                                           const_cast<char*>(reinterpret_cast<const char*>(in.data())),
                                           static_cast<unsigned int>(in.size()), 0, 0) != BZ_OK)
                return false;
            produced = n;
            return true;
        }
        default: return false;
        }
    }

    bool readTable(uint64_t offset, uint32_t storedSize, size_t decodedSize, std::vector<uint8_t>& out) {
        std::vector<uint8_t> stored(storedSize);
        if (!m_stream->readExact(offset, stored.data(), stored.size()))
            return false;
        out.assign(decodedSize, 0);
        size_t produced = 0;
        return decompress(stored, out.data(), out.size(), produced) && produced == decodedSize;
    }

    bool decodeGroup(const Group& g, uint64_t sectionOffset, size_t size, std::vector<uint8_t>& out) {
        if (g.size == 0)
            return true; // all zeroes
        std::vector<uint8_t> stored(g.size);
        if (!m_stream->readExact(g.offset, stored.data(), stored.size()))
            return false;

        std::vector<uint8_t> data;
        if (g.compressed) {
            data.resize(std::max<size_t>(m_blockSize, g.packedSize));
            size_t produced = 0;
            if (!decompress(stored, data.data(), data.size(), produced))
                return false;
            data.resize(produced);
        } else {
            data = std::move(stored);
        }

        if (g.packedSize == 0) {
            if (data.size() != size)
                return false;
            std::memcpy(out.data(), data.data(), size);
            return true;
        }

        // RVZ packing: runs of real data, or a 68-byte seed standing for junk.
        size_t pos = 0;
        size_t written = 0;
        LaggedFibonacci lfg;
        while (data.size() - pos >= 4) {
            const uint32_t header = be32(data.data() + pos);
            pos += 4;
            const size_t n = header & 0x7FFFFFFFu;
            if (n > size - written)
                return false;
            if (header & 0x80000000u) {
                if (data.size() - pos < LaggedFibonacci::kSeedBytes)
                    return false;
                lfg.initWithSeedBytes(data.data() + pos);
                pos += LaggedFibonacci::kSeedBytes;
                lfg.skip(static_cast<size_t>((sectionOffset + written) % kSectorSize));
                lfg.fill(out.data() + written, n);
            } else {
                if (data.size() - pos < n)
                    return false;
                std::memcpy(out.data() + written, data.data() + pos, n);
                pos += n;
            }
            written += n;
        }
        return written == size && pos == data.size();
    }

    std::unique_ptr<Stream> m_stream;
    std::mutex m_lock;
    BlockCache m_cache{8};
    bool m_rvz = false;
    uint8_t m_discHead[0x80] = {};
    std::vector<RawData> m_raw;
    std::vector<Group> m_groups;
};

} // namespace

std::unique_ptr<Stream> openFileStream(const char* path, std::string& error) {
    FILE* file = path ? std::fopen(path, "rb") : nullptr;
    if (!file) {
        error = std::string("cannot open ") + (path ? path : "(null)");
        return nullptr;
    }
    return std::make_unique<FileStream>(file);
}

std::unique_ptr<Disc> openDisc(std::unique_ptr<Stream> stream, std::string& error) {
    if (!stream) {
        error = "no stream";
        return nullptr;
    }
    uint8_t head[0x20] = {};
    const int64_t got = stream->readAt(0, head, sizeof(head));
    if (got < 8) {
        error = "file too small to be a disc image";
        return nullptr;
    }
    if (std::memcmp(head, "CISO", 4) == 0) {
        auto disc = std::make_unique<CisoDisc>();
        if (!disc->open(std::move(stream), error))
            return nullptr;
        return disc;
    }
    if (le32(head) == 0xB10BC001u) {
        auto disc = std::make_unique<GczDisc>();
        if (!disc->open(std::move(stream), error))
            return nullptr;
        return disc;
    }
    if (std::memcmp(head, "WIA\x01", 4) == 0 || std::memcmp(head, "RVZ\x01", 4) == 0) {
        auto disc = std::make_unique<WiaDisc>();
        if (!disc->open(std::move(stream), error))
            return nullptr;
        return disc;
    }
    if (got >= 0x20 && be32(head + 0x1C) == kGameCubeMagic) {
        const int64_t length = stream->length();
        if (length <= 0) {
            error = "cannot size the disc image";
            return nullptr;
        }
        return std::make_unique<IsoDisc>(std::move(stream), static_cast<uint64_t>(length));
    }
    if (got >= 0x20 && be32(head + 0x18) == 0x5D1C9EA3u) {
        error = "Wii discs are not supported, only GameCube";
        return nullptr;
    }
    error = "not a GameCube disc image (ISO, GCM, CISO, GCZ, WIA or RVZ)";
    return nullptr;
}

} // namespace nodlite
