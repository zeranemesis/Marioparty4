#pragma once

// GameCube disc image containers: whatever the file looks like on the card,
// a Disc reads plain disc bytes.
//
//   ISO/GCM  raw image
//   CISO     sparse blocks (Wii Backup Manager)
//   GCZ      zlib blocks (Dolphin)
//   WIA/RVZ  Dolphin's formats: zstd, bzip2 or uncompressed groups, RVZ junk
//            rebuilt from seeds. GameCube discs only (no Wii partitions);
//            LZMA/LZMA2 and WIA "purge" are rejected with a clear error.
//
// The container is chosen from the file's magic, never from its extension.

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace nodlite {

// Random-access byte source (a file, or nod_disc_open_stream's callbacks).
class Stream {
public:
    virtual ~Stream() = default;
    // Returns bytes read (short only at end), or -1.
    virtual int64_t readAt(uint64_t offset, void* out, size_t size) = 0;
    virtual int64_t length() = 0;

    bool readExact(uint64_t offset, void* out, size_t size) {
        return readAt(offset, out, size) == static_cast<int64_t>(size);
    }
};

std::unique_ptr<Stream> openFileStream(const char* path, std::string& error);

enum class Format : uint8_t { Iso, Ciso, Gcz, Wia, Rvz };
enum class Compression : uint8_t { None, Bzip2, Deflate, Lzma, Lzma2, Zstandard };

class Disc {
public:
    virtual ~Disc() = default;

    // Reads decoded disc bytes in [offset, offset + size); size must stay
    // within size(). Thread-safe.
    virtual bool read(uint64_t offset, void* out, size_t size) = 0;

    uint64_t size() const { return m_size; }
    Format format() const { return m_format; }
    Compression compression() const { return m_compression; }
    int compressionLevel() const { return m_level; }
    uint32_t blockSize() const { return m_blockSize; }

protected:
    uint64_t m_size = 0;
    Format m_format = Format::Iso;
    Compression m_compression = Compression::None;
    int m_level = 0;
    uint32_t m_blockSize = 0;
};

std::unique_ptr<Disc> openDisc(std::unique_ptr<Stream> stream, std::string& error);

// GameCube single-layer disc size, used when a container does not record it.
constexpr uint64_t kMiniDvdSize = 1459978240ull;

} // namespace nodlite
