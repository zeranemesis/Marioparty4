// nod's C API (nod.h, v2.0.0-alpha.10) for GameCube discs, without Rust.
//
// Aurora's DVD layer and PartyBoard's disc validation are written against
// nod; nod itself is Rust and does not target libnx. This implements the
// same functions on top of container.cpp, so those sources build unchanged
// on the Switch. Wii discs are refused with an explicit error.

#include <nod.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "container.hpp"

namespace {

constexpr uint32_t kHandleMagic = 0x4E4C4948; // "NLIH"
constexpr size_t kBufferSize = 256 * 1024;

thread_local std::string tLastError;

void setError(std::string message) { tLastError = std::move(message); }
void clearError() { tLastError.clear(); }

uint32_t be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

// Everything the handles of one opened disc share.
struct DiscState {
    std::unique_ptr<nodlite::Disc> disc;
    uint8_t header[0x440] = {};

    // The FST is read when a partition opens (every lookup needs it); the
    // other blobs only when nod_partition_meta asks, so listing a library of
    // discs does not decode each game's DOL.
    std::once_flag fstOnce, metaOnce;
    bool fstOk = false, metaOk = false;
    std::vector<uint8_t> boot, bi2, apploader, dol, fst;
    uint32_t fstEntries = 0;
    size_t fstStrings = 0;

    bool loadFst() {
        std::call_once(fstOnce, [this] { fstOk = readFst(); });
        return fstOk;
    }

    bool loadMeta() {
        std::call_once(metaOnce, [this] { metaOk = loadFst() && readMeta(); });
        return metaOk;
    }

    bool readRange(uint64_t offset, uint64_t size, std::vector<uint8_t>& out) const {
        if (size > (64u << 20) || offset + size > disc->size())
            return false;
        out.resize(static_cast<size_t>(size));
        return size == 0 || disc->read(offset, out.data(), out.size());
    }

    bool readFst() {
        if (!readRange(be32(header + 0x424), be32(header + 0x428), fst) || fst.size() < 12)
            return false;
        fstEntries = be32(fst.data() + 8);
        fstStrings = size_t(fstEntries) * 12;
        return fstEntries > 0 && fstStrings <= fst.size();
    }

    // The rest of the GameCube "data partition" metadata nod exposes as blobs.
    bool readMeta() {
        if (!readRange(0, 0x440, boot) || !readRange(0x440, 0x2000, bi2))
            return false;
        uint8_t loader[0x20];
        if (!disc->read(0x2440, loader, sizeof(loader)))
            return false;
        if (!readRange(0x2440, 0x20ull + be32(loader + 0x14) + be32(loader + 0x18), apploader))
            return false;

        const uint32_t dolOffset = be32(boot.data() + 0x420);
        uint8_t dolHeader[0x100];
        if (!disc->read(dolOffset, dolHeader, sizeof(dolHeader)))
            return false;
        uint64_t dolSize = sizeof(dolHeader);
        for (int i = 0; i < 18; ++i) {
            // 7 text then 11 data sections: file offsets at 0x00, sizes at 0x90.
            const uint64_t end = uint64_t(be32(dolHeader + i * 4)) + be32(dolHeader + 0x90 + i * 4);
            if (be32(dolHeader + 0x90 + i * 4) != 0)
                dolSize = std::max(dolSize, end);
        }
        return readRange(dolOffset, dolSize, dol);
    }

    bool node(uint32_t index, bool& isDir, std::string_view& name, uint32_t& offset, uint32_t& length) const {
        if (index >= fstEntries)
            return false;
        const uint8_t* e = fst.data() + size_t(index) * 12;
        isDir = e[0] != 0;
        const size_t nameOffset = (size_t(e[1]) << 16) | (size_t(e[2]) << 8) | e[3];
        offset = be32(e + 4);
        length = be32(e + 8);
        if (fstStrings + nameOffset >= fst.size()) {
            name = "<invalid>";
            return true;
        }
        const char* text = reinterpret_cast<const char*>(fst.data() + fstStrings + nameOffset);
        name = std::string_view(text, strnlen(text, fst.size() - fstStrings - nameOffset));
        return true;
    }
};

} // namespace

struct NodHandle {
    enum class Kind { Disc, Partition, File };

    uint32_t magic = kHandleMagic;
    Kind kind = Kind::Disc;
    std::shared_ptr<DiscState> state;
    uint64_t start = 0;  // byte range of the disc this handle reads
    uint64_t length = 0;
    uint64_t position = 0;
    std::vector<uint8_t> buffer; // nod_buf_read window starting at bufferStart
    uint64_t bufferStart = 0;

    ~NodHandle() { magic = 0; }
};

namespace {

bool valid(const NodHandle* h) { return h != nullptr && h->magic == kHandleMagic; }

int64_t addSaturating(int64_t a, int64_t b) {
    int64_t r = 0;
    if (__builtin_add_overflow(a, b, &r))
        return b < 0 ? INT64_MIN : INT64_MAX;
    return r;
}

NodHandle* makeHandle(NodHandle::Kind kind, std::shared_ptr<DiscState> state, uint64_t start, uint64_t length) {
    auto* h = new NodHandle();
    h->kind = kind;
    h->state = std::move(state);
    h->start = start;
    h->length = length;
    return h;
}

class CallbackStream final : public nodlite::Stream {
public:
    explicit CallbackStream(const NodDiscStream& s) : m_s(s) {}
    ~CallbackStream() override {
        if (m_s.close)
            m_s.close(m_s.user_data);
    }
    int64_t readAt(uint64_t offset, void* out, size_t size) override {
        // nod's callbacks may return short reads before the end: loop.
        size_t done = 0;
        while (done < size) {
            const int64_t n = m_s.read_at(m_s.user_data, offset + done, static_cast<uint8_t*>(out) + done, size - done);
            if (n < 0)
                return -1;
            if (n == 0)
                break;
            done += static_cast<size_t>(n);
        }
        return static_cast<int64_t>(done);
    }
    int64_t length() override { return m_s.stream_len(m_s.user_data); }

private:
    NodDiscStream m_s;
};

NodResult openDisc(std::unique_ptr<nodlite::Stream> stream, NodHandle** out) {
    std::string error;
    auto disc = nodlite::openDisc(std::move(stream), error);
    if (!disc) {
        setError(error);
        return NOD_RESULT_ERR_FORMAT;
    }
    auto state = std::make_shared<DiscState>();
    state->disc = std::move(disc);
    if (state->disc->size() < sizeof(state->header) || !state->disc->read(0, state->header, sizeof(state->header))) {
        setError("cannot read the disc header");
        return NOD_RESULT_ERR_IO;
    }
    *out = makeHandle(NodHandle::Kind::Disc, state, 0, state->disc->size());
    return NOD_RESULT_OK;
}

NodFormat toNod(nodlite::Format f) {
    switch (f) {
    case nodlite::Format::Iso: return NOD_FORMAT_ISO;
    case nodlite::Format::Ciso: return NOD_FORMAT_CISO;
    case nodlite::Format::Gcz: return NOD_FORMAT_GCZ;
    case nodlite::Format::Wia: return NOD_FORMAT_WIA;
    case nodlite::Format::Rvz: return NOD_FORMAT_RVZ;
    }
    return NOD_FORMAT_ISO;
}

NodCompressionKind toNod(nodlite::Compression c) {
    switch (c) {
    case nodlite::Compression::None: return NOD_COMPRESSION_KIND_NONE;
    case nodlite::Compression::Bzip2: return NOD_COMPRESSION_KIND_BZIP2;
    case nodlite::Compression::Deflate: return NOD_COMPRESSION_KIND_DEFLATE;
    case nodlite::Compression::Lzma: return NOD_COMPRESSION_KIND_LZMA;
    case nodlite::Compression::Lzma2: return NOD_COMPRESSION_KIND_LZMA2;
    case nodlite::Compression::Zstandard: return NOD_COMPRESSION_KIND_ZSTANDARD;
    }
    return NOD_COMPRESSION_KIND_NONE;
}

NodBlob blob(const std::vector<uint8_t>& v) { return NodBlob{v.empty() ? nullptr : v.data(), v.size()}; }

} // namespace

extern "C" {

const char* nod_error_message(void) { return tLastError.empty() ? nullptr : tLastError.c_str(); }

NodResult nod_disc_open(const char* path, const NodDiscOptions*, NodHandle** out) {
    clearError();
    if (!path || !out) {
        setError("null pointer argument");
        return NOD_RESULT_ERR_INVALID_HANDLE;
    }
    std::string error;
    auto stream = nodlite::openFileStream(path, error);
    if (!stream) {
        setError(error);
        return NOD_RESULT_ERR_IO;
    }
    return openDisc(std::move(stream), out);
}

NodResult nod_disc_open_stream(const NodDiscStream* stream, const NodDiscOptions*, NodHandle** out) {
    clearError();
    if (!stream || !out || !stream->read_at || !stream->stream_len || !stream->close) {
        setError("null pointer argument");
        return NOD_RESULT_ERR_INVALID_HANDLE;
    }
    return openDisc(std::make_unique<CallbackStream>(*stream), out);
}

NodResult nod_disc_open_partition(NodHandle* disc, uint32_t index, const NodPartitionOptions*, NodHandle** out) {
    clearError();
    if (!valid(disc) || disc->kind != NodHandle::Kind::Disc || !out) {
        setError("handle is not a disc");
        return NOD_RESULT_ERR_INVALID_HANDLE;
    }
    if (index != 0) {
        setError("disc format error: GameCube discs only have one partition");
        return NOD_RESULT_ERR_FORMAT;
    }
    if (!disc->state->loadFst()) {
        setError("disc format error: cannot read the FST");
        return NOD_RESULT_ERR_FORMAT;
    }
    *out = makeHandle(NodHandle::Kind::Partition, disc->state, 0, disc->state->disc->size());
    return NOD_RESULT_OK;
}

NodResult nod_disc_open_partition_kind(NodHandle* disc, uint32_t kind, const NodPartitionOptions* options,
                                       NodHandle** out) {
    if (kind != NOD_PARTITION_KIND_DATA) {
        clearError();
        setError("disc format error: GameCube discs only have a data partition");
        return NOD_RESULT_ERR_FORMAT;
    }
    return nod_disc_open_partition(disc, 0, options, out);
}

NodResult nod_partition_open_file(NodHandle* partition, uint32_t fst_index, NodHandle** out) {
    clearError();
    if (!valid(partition) || partition->kind != NodHandle::Kind::Partition || !out) {
        setError("handle is not a partition");
        return NOD_RESULT_ERR_INVALID_HANDLE;
    }
    bool isDir = false;
    std::string_view name;
    uint32_t offset = 0, length = 0;
    if (!partition->state->node(fst_index, isDir, name, offset, length)) {
        setError("FST index " + std::to_string(fst_index) + " out of range");
        return NOD_RESULT_ERR_NOT_FOUND;
    }
    if (isDir || fst_index == 0) {
        setError("node is not a file");
        return NOD_RESULT_ERR_FORMAT;
    }
    if (uint64_t(offset) + length > partition->state->disc->size()) {
        setError("file extends past the end of the disc");
        return NOD_RESULT_ERR_FORMAT;
    }
    *out = makeHandle(NodHandle::Kind::File, partition->state, offset, length);
    return NOD_RESULT_OK;
}

void nod_free(NodHandle* handle) {
    if (valid(handle))
        delete handle;
}

int64_t nod_read(NodHandle* handle, uint8_t* buf, size_t len) {
    clearError();
    if (!valid(handle) || (!buf && len)) {
        setError("invalid handle");
        return -1;
    }
    if (handle->position >= handle->length || len == 0)
        return 0;
    const size_t n = static_cast<size_t>(std::min<uint64_t>(len, handle->length - handle->position));
    if (!handle->state->disc->read(handle->start + handle->position, buf, n)) {
        setError("read failed");
        return -1;
    }
    handle->position += n;
    return static_cast<int64_t>(n);
}

int64_t nod_seek(NodHandle* handle, int64_t offset, int32_t whence) {
    clearError();
    if (!valid(handle)) {
        setError("invalid handle");
        return -1;
    }
    // Same rules as nod: a file is a window, so its position is clamped to
    // [0, length]; a disc or partition only refuses positions before 0.
    int64_t target = 0;
    switch (whence) {
    case 0: target = offset; break;
    case 1: target = addSaturating(static_cast<int64_t>(handle->position), offset); break;
    case 2: target = addSaturating(static_cast<int64_t>(handle->length), offset); break;
    default: setError("invalid whence value: " + std::to_string(whence)); return -1;
    }
    if (handle->kind == NodHandle::Kind::File) {
        target = std::clamp<int64_t>(target, 0, static_cast<int64_t>(handle->length));
    } else if (target < 0) {
        if (whence == 0) {
            setError("seek before the start");
            return -1;
        }
        target = 0;
    }
    handle->position = static_cast<uint64_t>(target);
    return target;
}

const void* nod_buf_read(NodHandle* handle, size_t* out_len) {
    clearError();
    if (!out_len) {
        setError("null pointer argument");
        return nullptr;
    }
    *out_len = 0;
    if (!valid(handle)) {
        setError("invalid handle");
        return nullptr;
    }
    if (handle->position >= handle->length)
        return nullptr;
    const bool inWindow = handle->position >= handle->bufferStart &&
                          handle->position < handle->bufferStart + handle->buffer.size();
    if (!inWindow) {
        const size_t n = static_cast<size_t>(std::min<uint64_t>(kBufferSize, handle->length - handle->position));
        handle->buffer.resize(n);
        if (!handle->state->disc->read(handle->start + handle->position, handle->buffer.data(), n)) {
            handle->buffer.clear();
            setError("read failed");
            return nullptr;
        }
        handle->bufferStart = handle->position;
    }
    const size_t skip = static_cast<size_t>(handle->position - handle->bufferStart);
    *out_len = handle->buffer.size() - skip;
    return handle->buffer.data() + skip;
}

void nod_buf_consume(NodHandle* handle, size_t n) {
    if (!valid(handle))
        return;
    handle->position = std::min<uint64_t>(handle->length, handle->position + n);
}

NodResult nod_disc_header(const NodHandle* disc, NodDiscHeader* out) {
    clearError();
    if (!out) {
        setError("null pointer argument");
        return NOD_RESULT_ERR_INVALID_HANDLE;
    }
    if (!valid(disc) || disc->kind != NodHandle::Kind::Disc) {
        setError("handle is not a disc");
        return NOD_RESULT_ERR_INVALID_HANDLE;
    }
    static_assert(sizeof(NodDiscHeader) == 0x400, "nod.h disc header layout");
    std::memcpy(out, disc->state->header, sizeof(NodDiscHeader));
    return NOD_RESULT_OK;
}

NodResult nod_disc_meta(const NodHandle* disc, NodDiscMeta* out) {
    clearError();
    if (!out || !valid(disc) || disc->kind != NodHandle::Kind::Disc) {
        setError("handle is not a disc");
        return NOD_RESULT_ERR_INVALID_HANDLE;
    }
    const nodlite::Disc& d = *disc->state->disc;
    *out = NodDiscMeta{};
    out->format = toNod(d.format());
    out->compression.kind = toNod(d.compression());
    out->compression.level = static_cast<int16_t>(d.compressionLevel());
    out->block_size = d.blockSize();
    out->lossless = true;
    out->disc_size = d.size();
    return NOD_RESULT_OK;
}

uint64_t nod_disc_size(const NodHandle* disc) {
    clearError();
    if (!valid(disc) || disc->kind != NodHandle::Kind::Disc) {
        setError("handle is not a disc");
        return 0;
    }
    return disc->state->disc->size();
}

size_t nod_disc_partitions(const NodHandle* disc, NodPartitionInfo* out, size_t cap) {
    clearError();
    if (!valid(disc) || disc->kind != NodHandle::Kind::Disc) {
        setError("handle is not a disc");
        return 0;
    }
    // Like nod: the partition table lists Wii partitions only; a GameCube
    // disc has none, and its data is opened as partition 0 / kind DATA.
    (void)out;
    (void)cap;
    return 0;
}

bool nod_partition_is_wii(const NodHandle* partition) {
    clearError();
    (void)partition; // nodlite only opens GameCube discs
    return false;
}

NodResult nod_partition_meta(const NodHandle* partition, NodPartitionMeta* out) {
    clearError();
    if (!out || !valid(partition) || partition->kind != NodHandle::Kind::Partition) {
        setError("handle is not a partition");
        return NOD_RESULT_ERR_INVALID_HANDLE;
    }
    *out = NodPartitionMeta{};
    if (!partition->state->loadMeta()) {
        setError("disc format error: cannot read the boot header, apploader or DOL");
        return NOD_RESULT_ERR_FORMAT;
    }
    const DiscState& s = *partition->state;
    out->raw_boot = blob(s.boot);
    out->raw_bi2 = blob(s.bi2);
    out->raw_apploader = blob(s.apploader);
    out->raw_dol = blob(s.dol);
    out->raw_fst = blob(s.fst);
    return NOD_RESULT_OK;
}

uint32_t nod_partition_find_file(const NodHandle* partition, const char* path, NodNodeKind* out_kind,
                                 uint32_t* out_length) {
    clearError();
    if (!path) {
        setError("null pointer argument");
        return NOD_FST_STOP;
    }
    if (!valid(partition) || partition->kind != NodHandle::Kind::Partition) {
        setError("handle is not a partition");
        return NOD_FST_STOP;
    }
    const DiscState& s = *partition->state;
    auto report = [&](uint32_t index, bool isDir, uint32_t length) {
        if (out_kind)
            *out_kind = isDir ? NOD_NODE_KIND_DIRECTORY : NOD_NODE_KIND_FILE;
        if (out_length)
            *out_length = length;
        return index;
    };

    // Case-insensitive, '/'-separated, empty segments ignored (like nod).
    std::vector<std::string_view> parts;
    for (std::string_view rest = path; !rest.empty();) {
        const size_t slash = rest.find('/');
        const std::string_view part = rest.substr(0, slash);
        if (!part.empty())
            parts.push_back(part);
        rest = slash == std::string_view::npos ? std::string_view{} : rest.substr(slash + 1);
    }
    bool isDir = false;
    std::string_view name;
    uint32_t offset = 0, length = 0;
    if (parts.empty()) {
        s.node(0, isDir, name, offset, length);
        return report(0, true, length);
    }
    auto equal = [](std::string_view a, std::string_view b) {
        return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
                   return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
               });
    };
    uint32_t index = 1;
    uint32_t end = s.fstEntries;
    for (size_t p = 0; p < parts.size(); ++p) {
        bool found = false;
        while (index < end && s.node(index, isDir, name, offset, length)) {
            if (equal(name, parts[p])) {
                found = true;
                break;
            }
            index = isDir ? std::max(length, index + 1) : index + 1;
        }
        if (!found)
            return NOD_FST_STOP;
        if (p + 1 == parts.size())
            return report(index, isDir, length);
        if (!isDir)
            return NOD_FST_STOP;
        end = std::min(length, s.fstEntries);
        ++index;
    }
    return NOD_FST_STOP;
}

void nod_partition_iterate_fst(const NodHandle* partition, NodFstCallback callback, void* user_data) {
    clearError();
    if (!valid(partition) || partition->kind != NodHandle::Kind::Partition || !callback) {
        setError("handle is not a partition");
        return;
    }
    const DiscState& s = *partition->state;
    uint32_t index = 1; // the root is not reported
    bool isDir = false;
    std::string_view name;
    uint32_t offset = 0, length = 0;
    while (index != NOD_FST_STOP && s.node(index, isDir, name, offset, length)) {
        const std::string terminated(name);
        index = callback(index, isDir ? NOD_NODE_KIND_DIRECTORY : NOD_NODE_KIND_FILE, terminated.c_str(), length,
                         user_data);
    }
}

} // extern "C"
