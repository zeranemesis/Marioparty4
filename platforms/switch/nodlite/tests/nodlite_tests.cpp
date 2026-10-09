// nodlite: the nod C API over every supported container, checked against the
// test disc byte for byte. The RVZ fixtures come from nod's own nodtool
// (tools/make-fixtures.sh): nodtool only stores junk as seeds when it matches
// its generator, so they also prove nodlite's generator is the same.
//
//   nodlite_tests <fixtures dir>        run the tests
//   nodlite_tests --write-iso <path>    write the test disc (for fixtures)
//
// The same file also builds against the real nod library
// (tools/check-against-nod.sh, which defines NODLITE_TESTS_AGAINST_NOD): every
// check runs on both, except the few marked nodlite-only, where nodlite is
// deliberately stricter (Wii discs) or more lenient (old 16 KiB GCZ blocks).

#include <nod.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include <zlib.h>

#include "test_disc.hpp"

namespace {

int g_checks = 0;
int g_failures = 0;

#define CHECK(c)                                                                          \
    do {                                                                                  \
        ++g_checks;                                                                       \
        if (!(c)) {                                                                       \
            ++g_failures;                                                                 \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c);    \
            if (const char* e = nod_error_message())                                      \
                std::fprintf(stderr, "  nod_error_message: %s\n", e);                     \
        }                                                                                 \
    } while (0)

using Bytes = std::vector<uint8_t>;

bool writeFile(const std::string& path, const Bytes& data) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f)
        return false;
    const bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    return std::fclose(f) == 0 && ok;
}

bool fileExists(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (f)
        std::fclose(f);
    return f != nullptr;
}

void put32le(Bytes& out, size_t at, uint32_t v) {
    for (int i = 0; i < 4; ++i)
        out[at + i] = uint8_t(v >> (8 * i));
}

void put64le(Bytes& out, size_t at, uint64_t v) {
    for (int i = 0; i < 8; ++i)
        out[at + i] = uint8_t(v >> (8 * i));
}

Bytes makeCiso(const Bytes& iso, uint32_t blockSize) {
    Bytes out(0x8000, 0);
    std::memcpy(out.data(), "CISO", 4);
    put32le(out, 4, blockSize);
    for (size_t b = 0; b * blockSize < iso.size(); ++b) {
        const auto first = iso.begin() + b * blockSize;
        const auto last = iso.begin() + std::min(iso.size(), (b + 1) * blockSize);
        if (std::all_of(first, last, [](uint8_t v) { return v == 0; }))
            continue;
        out[8 + b] = 1;
        out.insert(out.end(), first, last);
        out.resize(out.size() + (blockSize - (last - first)), 0);
    }
    return out;
}

Bytes makeGcz(const Bytes& iso, uint32_t blockSize) {
    const uint32_t blocks = static_cast<uint32_t>((iso.size() + blockSize - 1) / blockSize);
    Bytes data;
    std::vector<uint64_t> pointers;
    std::vector<uint32_t> checksums;
    for (uint32_t b = 0; b < blocks; ++b) {
        const size_t start = size_t(b) * blockSize;
        const size_t n = std::min<size_t>(blockSize, iso.size() - start);
        uLongf packedSize = compressBound(static_cast<uLong>(n));
        Bytes packed(packedSize);
        const bool raw = b % 4 == 2 || compress2(packed.data(), &packedSize, iso.data() + start, static_cast<uLong>(n), 6) != Z_OK ||
                         packedSize >= n;
        pointers.push_back(data.size() | (raw ? (1ull << 63) : 0));
        const uint8_t* stored = raw ? iso.data() + start : packed.data();
        const size_t storedSize = raw ? n : packedSize;
        checksums.push_back(static_cast<uint32_t>(adler32(adler32(0, nullptr, 0), stored, static_cast<uInt>(storedSize))));
        data.insert(data.end(), stored, stored + storedSize);
    }
    Bytes out(32 + size_t(blocks) * 12, 0);
    put32le(out, 0, 0xB10BC001u);
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

void put32be(Bytes& out, size_t at, uint32_t v) {
    for (int i = 0; i < 4; ++i)
        out[at + i] = uint8_t(v >> (24 - 8 * i));
}

void put64be(Bytes& out, size_t at, uint64_t v) {
    put32be(out, at, uint32_t(v >> 32));
    put32be(out, at + 4, uint32_t(v));
}

// SHA-1, for the hashes a WIA header carries (nod checks them, nodlite does not).
std::array<uint8_t, 20> sha1(const uint8_t* data, size_t size) {
    uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    Bytes msg(data, data + size);
    msg.push_back(0x80);
    while (msg.size() % 64 != 56)
        msg.push_back(0);
    for (int i = 7; i >= 0; --i)
        msg.push_back(uint8_t((uint64_t(size) * 8) >> (8 * i)));
    const auto rol = [](uint32_t v, int n) { return (v << n) | (v >> (32 - n)); };
    for (size_t block = 0; block < msg.size(); block += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i)
            w[i] = uint32_t(msg[block + 4 * i]) << 24 | uint32_t(msg[block + 4 * i + 1]) << 16 |
                   uint32_t(msg[block + 4 * i + 2]) << 8 | msg[block + 4 * i + 3];
        for (int i = 16; i < 80; ++i)
            w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20)
                f = (b & c) | (~b & d), k = 0x5A827999u;
            else if (i < 40)
                f = b ^ c ^ d, k = 0x6ED9EBA1u;
            else if (i < 60)
                f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDCu;
            else
                f = b ^ c ^ d, k = 0xCA62C1D6u;
            const uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d, d = c, c = rol(b, 30), b = a, a = t;
        }
        h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e;
    }
    std::array<uint8_t, 20> out{};
    for (int i = 0; i < 20; ++i)
        out[i] = uint8_t(h[i / 4] >> (24 - 8 * (i % 4)));
    return out;
}

// Uncompressed WIA following docs/WiaAndRvz.md: one raw data range from 0x80
// to the end, 2 MiB groups, group offsets stored divided by 4.
Bytes makeWia(const Bytes& iso) {
    constexpr uint32_t kChunk = 0x200000;
    const uint32_t groups = static_cast<uint32_t>((iso.size() + kChunk - 1) / kChunk);
    const size_t rawTable = 0x48 + 0xDC;
    const size_t groupTable = rawTable + 0x18;
    const size_t dataStart = (groupTable + size_t(groups) * 8 + 3) & ~size_t(3);
    Bytes out(dataStart, 0);
    std::memcpy(out.data(), "WIA\x01", 4);
    put32be(out, 4, 0x01000000);
    put32be(out, 8, 0x00080000);
    put32be(out, 0x0C, 0xDC);
    put64be(out, 0x24, iso.size());
    const size_t disc = 0x48;
    put32be(out, disc + 0x00, 1); // GameCube
    put32be(out, disc + 0x04, 0); // no compression
    put32be(out, disc + 0x0C, kChunk);
    std::memcpy(out.data() + disc + 0x10, iso.data(), 0x80);
    put32be(out, disc + 0x94, 0x30); // partition struct size
    put32be(out, disc + 0xB4, 1);
    put64be(out, disc + 0xB8, rawTable);
    put32be(out, disc + 0xC0, 0x18);
    put32be(out, disc + 0xC4, groups);
    put64be(out, disc + 0xC8, groupTable);
    put32be(out, disc + 0xD0, groups * 8);
    put64be(out, rawTable + 0x00, 0x80);
    put64be(out, rawTable + 0x08, iso.size() - 0x80);
    put32be(out, rawTable + 0x10, 0);
    put32be(out, rawTable + 0x14, groups);
    for (uint32_t g = 0; g < groups; ++g) {
        const size_t start = size_t(g) * kChunk;
        const size_t n = std::min<size_t>(kChunk, iso.size() - start);
        put32be(out, groupTable + size_t(g) * 8, static_cast<uint32_t>(out.size() / 4));
        put32be(out, groupTable + size_t(g) * 8 + 4, static_cast<uint32_t>(n));
        out.insert(out.end(), iso.begin() + start, iso.begin() + start + n);
        out.resize((out.size() + 3) & ~size_t(3), 0);
    }
    // Hashes: the (empty) partition table, the disc struct, then the file head.
    const auto copyHash = [&](size_t at, const std::array<uint8_t, 20>& h) { std::memcpy(out.data() + at, h.data(), 20); };
    copyHash(disc + 0xA0, sha1(nullptr, 0));
    copyHash(0x10, sha1(out.data() + disc, 0xDC));
    put64be(out, 0x2C, out.size());
    copyHash(0x34, sha1(out.data(), 0x34));
    return out;
}

// Aurora's fstCallback (extern/aurora/lib/dolphin/dvd/fst.cpp) in miniature:
// rebuild full paths from the iteration, using the directory end indices.
struct Walk {
    std::vector<std::pair<std::string, uint32_t>> dirs{{"", UINT32_MAX}};
    std::map<std::string, std::pair<uint32_t, uint32_t>> nodes; // path -> index, size
};

uint32_t walkCallback(uint32_t index, NodNodeKind kind, const char* name, uint32_t size, void* user) {
    auto* w = static_cast<Walk*>(user);
    while (index >= w->dirs.back().second)
        w->dirs.pop_back();
    const std::string path = w->dirs.back().first + "/" + name;
    w->nodes[path] = {index, size};
    if (kind == NOD_NODE_KIND_DIRECTORY)
        w->dirs.emplace_back(path, size);
    return index + 1;
}

uint32_t stopAfterTwo(uint32_t index, NodNodeKind, const char*, uint32_t, void* user) {
    ++*static_cast<int*>(user);
    return index >= 2 ? NOD_FST_STOP : index + 1;
}

// Reads `size` bytes through nod_buf_read and compares with `expected`;
// with `mustEnd`, the handle must then be at its end.
bool bufferedEquals(NodHandle* h, const uint8_t* expected, uint64_t size, bool mustEnd) {
    uint64_t pos = 0;
    while (pos < size) {
        size_t len = 0;
        const void* data = nod_buf_read(h, &len);
        if (!data)
            return false;
        // Consume in odd steps to exercise partial consumption.
        const size_t take = static_cast<size_t>(std::min<uint64_t>({len, 70001, size - pos}));
        if (std::memcmp(data, expected + pos, take) != 0)
            return false;
        nod_buf_consume(h, take);
        pos += take;
    }
    size_t len = 0;
    return !mustEnd || nod_buf_read(h, &len) == nullptr;
}

void checkDisc(const std::string& label, NodHandle* disc, const Bytes& iso, NodFormat format, bool exactSize) {
    std::fprintf(stderr, "-- %s\n", label.c_str());
    CHECK(disc != nullptr);
    if (!disc)
        return;

    NodDiscHeader header{};
    CHECK(nod_disc_header(disc, &header) == NOD_RESULT_OK);
    CHECK(std::memcmp(header.game_id, nodlite::test::kGameId, 6) == 0);
    CHECK(header.disc_version == 1);
    CHECK(NOD_MAGIC_EQ(header.gcn_magic, GCN_MAGIC));
    CHECK(std::strncmp(header.game_title, "nodlite test disc", 64) == 0);

    NodDiscMeta meta{};
    CHECK(nod_disc_meta(disc, &meta) == NOD_RESULT_OK);
    CHECK(meta.format == format);
    if (exactSize)
        CHECK(nod_disc_size(disc) == iso.size());
    else
        CHECK(nod_disc_size(disc) >= iso.size());

    // The whole disc, through the zero-copy buffer.
    CHECK(bufferedEquals(disc, iso.data(), iso.size(), exactSize));
    CHECK(nod_seek(disc, 0x2440, 0) == 0x2440);
    uint8_t loader[16];
    CHECK(nod_read(disc, loader, sizeof(loader)) == 16);
    CHECK(std::memcmp(loader, "2026/10/01", 10) == 0);

    // Disc seeks: absolute positions as given, relative ones stop at 0.
    CHECK(nod_seek(disc, 0x123456789ll, 0) == 0x123456789ll);
    CHECK(nod_seek(disc, 0, 0) == 0 && nod_seek(disc, -5, 1) == 0);

    // The partition table lists Wii partitions only; GameCube data is
    // partition 0, kind DATA.
    NodPartitionInfo info{};
    CHECK(nod_disc_partitions(disc, &info, 1) == 0);

    NodHandle* part = nullptr;
    CHECK(nod_disc_open_partition_kind(disc, NOD_PARTITION_KIND_UPDATE, nullptr, &part) == NOD_RESULT_ERR_FORMAT);
    CHECK(part == nullptr);
    CHECK(nod_disc_open_partition(disc, 1, nullptr, &part) == NOD_RESULT_ERR_FORMAT);
    CHECK(part == nullptr);
    CHECK(nod_disc_open_partition_kind(disc, NOD_PARTITION_KIND_DATA, nullptr, &part) == NOD_RESULT_OK);
    if (!part)
        return;
    CHECK(!nod_partition_is_wii(part));

    NodPartitionMeta pm{};
    CHECK(nod_partition_meta(part, &pm) == NOD_RESULT_OK);
    CHECK(pm.raw_boot.size == 0x440 && std::memcmp(pm.raw_boot.data, iso.data(), 0x440) == 0);
    CHECK(pm.raw_bi2.size == 0x2000 && std::memcmp(pm.raw_bi2.data, iso.data() + 0x440, 0x2000) == 0);
    CHECK(pm.raw_apploader.size == 0x20 + 0x1000 + 0x40 &&
          std::memcmp(pm.raw_apploader.data, iso.data() + 0x2440, pm.raw_apploader.size) == 0);
    CHECK(pm.raw_dol.size == 0xD00 && std::memcmp(pm.raw_dol.data, iso.data() + 0x4000, 0xD00) == 0);
    CHECK(pm.raw_fst.data && std::memcmp(pm.raw_fst.data, iso.data() + 0x6000, pm.raw_fst.size) == 0);
    CHECK(pm.raw_ticket.data == nullptr && pm.raw_tmd.size == 0);

    // FST walk, as Aurora rebuilds its tree.
    Walk walk;
    nod_partition_iterate_fst(part, walkCallback, &walk);
    CHECK(walk.nodes.size() == 10);
    CHECK(walk.nodes.count("/data/sub/x.dat") == 1);
    CHECK(walk.nodes["/data"].second == 8);   // directory: end index
    CHECK(walk.nodes["/sound"].first == 8);
    int visited = 0;
    nod_partition_iterate_fst(part, stopAfterTwo, &visited);
    CHECK(visited == 2);

    // Lookup by path: case-insensitive, slashes forgiven.
    NodNodeKind kind{};
    uint32_t length = 0;
    CHECK(nod_partition_find_file(part, "/DATA//Board.BIN", &kind, &length) == 4);
    CHECK(kind == NOD_NODE_KIND_FILE && length == 0x23456);
    CHECK(nod_partition_find_file(part, "sound/", &kind, &length) == 8);
    CHECK(kind == NOD_NODE_KIND_DIRECTORY && length == 11);
    CHECK(nod_partition_find_file(part, "/", &kind, nullptr) == 0);
    CHECK(nod_partition_find_file(part, "/data/music.dat", nullptr, nullptr) == NOD_FST_STOP);
    CHECK(nod_partition_find_file(part, "/Readme.TXT/x", nullptr, nullptr) == NOD_FST_STOP);

    // Every file, read in uneven pieces, and seeking around.
    for (const nodlite::test::TestFile& f : nodlite::test::testFiles()) {
        const std::string path = std::string("/") + f.path;
        CHECK(walk.nodes.count(path) == 1);
        NodHandle* file = nullptr;
        CHECK(nod_partition_open_file(part, walk.nodes[path].first, &file) == NOD_RESULT_OK);
        if (!file)
            continue;
        Bytes got;
        uint8_t chunk[12345];
        int64_t n = 0;
        while ((n = nod_read(file, chunk, sizeof(chunk))) > 0)
            got.insert(got.end(), chunk, chunk + n);
        CHECK(n == 0);
        CHECK(got.size() == f.size && std::equal(got.begin(), got.end(), iso.begin() + f.offset));
        // A file is a window: seeks are clamped to [0, size].
        CHECK(nod_seek(file, 0, 2) == f.size);
        CHECK(nod_seek(file, -1, 0) == 0);
        CHECK(nod_seek(file, int64_t(f.size) + 100, 0) == f.size);
        CHECK(nod_read(file, chunk, sizeof(chunk)) == 0);
        CHECK(nod_seek(file, -int64_t(f.size) - 100, 1) == 0);
        CHECK(nod_seek(file, 1, 2) == f.size);
        if (f.size > 10) {
            CHECK(nod_seek(file, -10, 2) == f.size - 10);
            CHECK(nod_read(file, chunk, sizeof(chunk)) == 10);
            CHECK(std::memcmp(chunk, iso.data() + f.offset + f.size - 10, 10) == 0);
        }
        CHECK(nod_seek(file, 0, 0) == 0);
        CHECK(bufferedEquals(file, iso.data() + f.offset, f.size, true));
        nod_free(file);
    }
    NodHandle* notAFile = nullptr;
    CHECK(nod_partition_open_file(part, 3, &notAFile) == NOD_RESULT_ERR_FORMAT); // a directory
    CHECK(nod_partition_open_file(part, 0, &notAFile) == NOD_RESULT_ERR_FORMAT); // the root
    CHECK(nod_partition_open_file(part, 999, &notAFile) == NOD_RESULT_ERR_NOT_FOUND);
    CHECK(notAFile == nullptr);

    // Concurrent file reads share the disc (Aurora reads from several threads).
    std::atomic<int> mismatches{0};
    auto reader = [&](uint32_t index, const nodlite::test::TestFile& f) {
        for (int round = 0; round < 3; ++round) {
            NodHandle* file = nullptr;
            if (nod_partition_open_file(part, index, &file) != NOD_RESULT_OK) {
                ++mismatches;
                return;
            }
            Bytes got(f.size);
            int64_t total = 0, n = 0;
            while (total < f.size && (n = nod_read(file, got.data() + total, 4096)) > 0)
                total += n;
            if (total != f.size || !std::equal(got.begin(), got.end(), iso.begin() + f.offset))
                ++mismatches;
            nod_free(file);
        }
    };
    const auto& files = nodlite::test::testFiles();
    std::thread a(reader, walk.nodes["/data/board.bin"].first, files[2]);
    std::thread b(reader, walk.nodes["/sound/music.dat"].first, files[5]);
    a.join();
    b.join();
    CHECK(mismatches == 0);

    nod_free(part);
}

NodHandle* openPath(const std::string& path) {
    NodHandle* h = nullptr;
    return nod_disc_open(path.c_str(), nullptr, &h) == NOD_RESULT_OK ? h : nullptr;
}

// nod_disc_open_stream with callbacks over a memory buffer, like Aurora's SDL stream.
struct MemoryStream {
    const Bytes* data;
    bool closed = false;
};

int64_t memRead(void* user, uint64_t offset, void* out, size_t len) {
    auto* m = static_cast<MemoryStream*>(user);
    if (offset >= m->data->size())
        return 0;
    // Short reads on purpose: nod allows them before the end.
    const size_t n = std::min<size_t>({len, m->data->size() - static_cast<size_t>(offset), 50000});
    std::memcpy(out, m->data->data() + offset, n);
    return static_cast<int64_t>(n);
}
int64_t memLen(void* user) { return static_cast<int64_t>(static_cast<MemoryStream*>(user)->data->size()); }
void memClose(void* user) { static_cast<MemoryStream*>(user)->closed = true; }

} // namespace

int main(int argc, char** argv) {
    const Bytes iso = nodlite::test::buildDisc();
    if (argc == 3 && std::string(argv[1]) == "--write-iso")
        return writeFile(argv[2], iso) ? EXIT_SUCCESS : EXIT_FAILURE;
    const std::string fixtures = argc > 1 ? argv[1] : "tests/fixtures";

    const std::string tmp = "nodlite_test_tmp";
    CHECK(writeFile(tmp + ".iso", iso));
    CHECK(writeFile(tmp + ".ciso", makeCiso(iso, 0x8000)));
    CHECK(writeFile(tmp + ".gcz", makeGcz(iso, 0x8000)));

    NodHandle* d = openPath(tmp + ".iso");
    checkDisc("ISO", d, iso, NOD_FORMAT_ISO, true);
    nod_free(d);

    d = openPath(tmp + ".ciso");
    checkDisc("CISO", d, iso, NOD_FORMAT_CISO, false);
    if (d) {
        // CISO does not record the size: a GameCube disc, zeroes past the data.
        CHECK(nod_disc_size(d) == 1459978240ull);
        uint8_t tail[64] = {1};
        CHECK(nod_seek(d, 64ll << 20, 0) == 64ll << 20 && nod_read(d, tail, 64) == 64);
        CHECK(std::all_of(tail, tail + 64, [](uint8_t v) { return v == 0; }));
#ifndef NODLITE_TESTS_AGAINST_NOD
        // nodlite-only: with 32 KiB blocks the CISO map stops short of the end
        // of a GameCube disc; nod reads nothing there, nodlite reads zeroes.
        CHECK(nod_seek(d, 1459978240ll - 64, 0) > 0 && nod_read(d, tail, 64) == 64);
        CHECK(std::all_of(tail, tail + 64, [](uint8_t v) { return v == 0; }));
#endif
    }
    nod_free(d);

    d = openPath(tmp + ".gcz");
    checkDisc("GCZ", d, iso, NOD_FORMAT_GCZ, true);
    nod_free(d);

    // A damaged GCZ block fails the read instead of returning garbage.
    {
        Bytes gcz = makeGcz(iso, 0x8000);
        gcz[gcz.size() - 100] ^= 0x5A;
        CHECK(writeFile(tmp + "-bad.gcz", gcz));
        d = openPath(tmp + "-bad.gcz");
        CHECK(d != nullptr);
        if (d) {
            uint8_t buf[0x8000];
            CHECK(nod_seek(d, iso.size() - sizeof(buf), 0) == int64_t(iso.size() - sizeof(buf)));
            CHECK(nod_read(d, buf, sizeof(buf)) < 0);
            CHECK(nod_seek(d, 0, 0) == 0 && nod_read(d, buf, 0x40) == 0x40);
        }
        nod_free(d);
    }

#ifndef NODLITE_TESTS_AGAINST_NOD
    // nodlite-only: older Dolphin builds wrote GCZ with 16 KiB blocks, which
    // nod refuses (not a multiple of a 32 KiB sector).
    CHECK(writeFile(tmp + "-16k.gcz", makeGcz(iso, 0x4000)));
    d = openPath(tmp + "-16k.gcz");
    checkDisc("GCZ 16 KiB", d, iso, NOD_FORMAT_GCZ, true);
    nod_free(d);
#endif

    // Made by nod's nodtool from this very disc.
    for (const char* name : {"test-zstd.rvz", "test-none.rvz", "test-bzip2.rvz"}) {
        const std::string path = fixtures + "/" + name;
        CHECK(fileExists(path));
        d = openPath(path);
        checkDisc(name, d, iso, NOD_FORMAT_RVZ, true);
        nod_free(d);
    }

    // WIA (8-byte groups, no packing). nodtool's WIA writer fails on a disc this
    // small, so this one is assembled here, uncompressed, with the header
    // hashes nod verifies.
    CHECK(writeFile(tmp + ".wia", makeWia(iso)));
    d = openPath(tmp + ".wia");
    checkDisc("WIA", d, iso, NOD_FORMAT_WIA, true);
    nod_free(d);

    // Stream-backed open, with short reads; close runs exactly once on free.
    {
        MemoryStream mem{&iso};
        const NodDiscStream stream{&mem, memRead, memLen, memClose};
        NodHandle* h = nullptr;
        CHECK(nod_disc_open_stream(&stream, nullptr, &h) == NOD_RESULT_OK);
        checkDisc("stream", h, iso, NOD_FORMAT_ISO, true);
        CHECK(!mem.closed);
        nod_free(h);
        CHECK(mem.closed);
    }

    // Refusals.
    {
        NodHandle* h = nullptr;
#ifndef NODLITE_TESTS_AGAINST_NOD
        // nodlite-only: Wii discs are refused up front (nod decrypts them).
        Bytes wii(0x8000, 0);
        std::memcpy(wii.data(), "RSPE01", 6);
        wii[0x18] = 0x5D, wii[0x19] = 0x1C, wii[0x1A] = 0x9E, wii[0x1B] = 0xA3;
        CHECK(writeFile(tmp + "-wii.iso", wii));
        CHECK(nod_disc_open((tmp + "-wii.iso").c_str(), nullptr, &h) == NOD_RESULT_ERR_FORMAT);
        CHECK(nod_error_message() && std::strstr(nod_error_message(), "Wii"));
#endif
        CHECK(writeFile(tmp + "-junk.bin", Bytes(0x1000, 0x42)));
        CHECK(nod_disc_open((tmp + "-junk.bin").c_str(), nullptr, &h) == NOD_RESULT_ERR_FORMAT);
        CHECK(nod_disc_open("/nonexistent/disc.iso", nullptr, &h) == NOD_RESULT_ERR_IO);
        CHECK(nod_disc_open(nullptr, nullptr, &h) == NOD_RESULT_ERR_INVALID_HANDLE);
        CHECK(nod_read(nullptr, nullptr, 0) == -1);
        nod_free(nullptr);
    }

    for (const char* ext : {".iso", ".ciso", ".gcz", "-bad.gcz", "-16k.gcz", ".wia", "-wii.iso", "-junk.bin"})
        std::remove((tmp + ext).c_str());

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
