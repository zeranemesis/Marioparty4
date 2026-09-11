// Integrity checking for the HuMem allocator. See include/port/mem_diagnostics.h
// for why it exists and docs/humem_layout.md for the block format it relies on.

#include "port/mem_diagnostics.h"

#include "port/crash_report.h"
#include "port/coroutine_stack.h"

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

extern "C" {
u32 PartyBoard_NetplayFrameForDiagnostics(void);
s32 PartyBoard_NetplayContextId(void);
}

extern "C" void PartyBoard_MemDiagWriteReportFile(void);
extern "C" bool PartyBoard_IsRunning;
extern "C" void *HuMemHeapInit(void *ptr, size_t size);
extern "C" void *HuMemMemoryAlloc(void *heap_ptr, size_t size, uintptr_t retaddr);
extern "C" void HuMemMemoryFree(void *ptr, uintptr_t retaddr);

namespace {

// "REDZONE0" in ASCII: distinctive, and not a value the game would plausibly
// write by accident, which keeps the check from firing on ordinary data.
constexpr std::uint64_t kHeadPattern = 0x5245445A4F4E4530ull;

// The allocator's two magic values, from src/game/memory.c.
constexpr u8 kMagicFree = 205;
constexpr u8 kMagicAllocated = 165;

constexpr std::size_t kMaxHeaps = 8;
constexpr std::size_t kMaxTracked = 65536;
constexpr std::size_t kPathChars = 1024;
constexpr std::size_t kReportBytes = 256 * 1024;

struct HeapRegion {
    s32 id = -1;
    unsigned char *base = nullptr;
    std::size_t size = 0;
    bool used = false;
};

// One record per live allocation, kept OUTSIDE MEM1 so it is never part of the
// game state, never snapshotted and never hashed.
struct Record {
    void *block = nullptr;
    std::uint64_t allocationId = 0;
    std::size_t requested = 0;
    std::size_t internal = 0;
    std::uintptr_t retaddr = 0;
    u32 frame = 0;
    s32 context = 0;
    s32 heapId = -1;
    bool active = false;
    bool used = false;
};

HeapRegion gHeaps[kMaxHeaps];
Record gRecords[kMaxTracked];
std::mutex gMutex;
std::atomic<std::uint64_t> gNextAllocationId { 1 };

std::atomic<u32> gLastGoodFrame { 0 };
std::atomic<s32> gFirstBadFrame { -1 };
std::atomic<bool> gCorruptionDetected { false };
std::atomic<bool> gQuiet { false };

std::atomic<u32> gSweepCount { 0 };
std::atomic<u32> gBlockCount { 0 };
double gTotalSweepMs = 0.0;
double gWorstSweepMs = 0.0;

char gReport[kReportBytes];
std::size_t gReportUsed = 0;

bool envEnabled(const char *name)
{
    const char *value = std::getenv(name);
    return value && value[0] != '\0' && value[0] != '0' && value[0] != 'n' && value[0] != 'N';
}

std::size_t headerSize() { return PartyBoard_MemBlockHeaderSize(); }
std::size_t payloadOffset() { return PartyBoard_MemBlockPayloadOffset(); }

HeapRegion *heapOf(const void *address)
{
    const auto value = reinterpret_cast<std::uintptr_t>(address);
    for (auto &heap : gHeaps) {
        if (!heap.used) continue;
        const auto base = reinterpret_cast<std::uintptr_t>(heap.base);
        if (value >= base && value < base + heap.size) return &heap;
    }
    return nullptr;
}

// Caller holds gMutex.
Record *findRecordLocked(const void *block)
{
    for (auto &record : gRecords)
        if (record.used && record.block == block) return &record;
    return nullptr;
}

Record *acquireRecordLocked(const void *block)
{
    if (Record *existing = findRecordLocked(block)) return existing;
    for (auto &record : gRecords)
        if (!record.used) { record = Record {}; record.used = true; return &record; }
    return nullptr;
}

void reportLine(const char *format, ...)
{
    if (gReportUsed + 1 >= kReportBytes) return;
    va_list args;
    va_start(args, format);
    const int count = std::vsnprintf(gReport + gReportUsed, kReportBytes - gReportUsed, format, args);
    va_end(args);
    if (count > 0) {
        gReportUsed += static_cast<std::size_t>(count);
        if (gReportUsed >= kReportBytes) gReportUsed = kReportBytes - 1;
    }
}

double nowMs()
{
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // namespace

// Raised only for the duration of the self-test, which works on a fixture heap
// of its own. Without it the self-test inherited the master switch and returned
// true without running, so a build whose detector was broken reported the same
// thing as a build whose detector was sound.
static std::atomic<bool> gSelfTestForced{false};

extern "C" bool PartyBoard_MemDiagEnabled(void)
{
    static const bool enabled = envEnabled("PARTYBOARD_MEM_DIAGNOSTICS");
    return enabled || gSelfTestForced.load(std::memory_order_acquire);
}

extern "C" bool PartyBoard_MemDiagPoisonFreeEnabled(void)
{
    static const bool enabled = envEnabled("PARTYBOARD_MEM_POISON_FREE");
    return enabled;
}

extern "C" void PartyBoard_MemDiagRegisterHeap(s32 heapId, void *base, size_t size)
{
    if (!PartyBoard_MemDiagEnabled() || !base || size == 0) return;
    std::lock_guard<std::mutex> guard(gMutex);
    for (auto &heap : gHeaps) {
        if (heap.used) continue;
        heap.id = heapId;
        heap.base = static_cast<unsigned char *>(base);
        heap.size = size;
        heap.used = true;
        // A negative result is only worth something if the detector can be
        // shown to have been running, so it says so out loud.
        std::fprintf(stderr, "[MEM DIAG] heap %d registered: base 0x%llx size %llu\n", heapId,
            static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(base)),
            static_cast<unsigned long long>(size));
        return;
    }
}

extern "C" void PartyBoard_MemDiagOnAlloc(
    void *block, size_t requested, size_t internal, uintptr_t retaddr)
{
    if (!PartyBoard_MemDiagEnabled() || !block) return;
    auto *bytes = static_cast<unsigned char *>(block);

    // The head redzone lives in the bytes between the end of the header struct
    // and the start of the payload. They belong to nobody, so filling them
    // changes no address and costs no space. A write below the payload lands
    // here first.
    const std::size_t head = headerSize();
    const std::size_t payload = payloadOffset();
    if (payload > head) {
        for (std::size_t offset = head; offset + sizeof(kHeadPattern) <= payload;
             offset += sizeof(kHeadPattern)) {
            std::memcpy(bytes + offset, &kHeadPattern, sizeof(kHeadPattern));
        }
    }

    std::lock_guard<std::mutex> guard(gMutex);
    Record *record = acquireRecordLocked(block);
    if (!record) return;
    record->block = block;
    record->allocationId = gNextAllocationId.fetch_add(1, std::memory_order_relaxed);
    record->requested = requested;
    record->internal = internal;
    record->retaddr = retaddr;
    record->frame = PartyBoard_NetplayFrameForDiagnostics();
    record->context = PartyBoard_NetplayContextId();
    const HeapRegion *heap = heapOf(block);
    record->heapId = heap ? heap->id : -1;
    record->active = true;
}

extern "C" void PartyBoard_MemDiagOnFree(void *block, uintptr_t retaddr)
{
    if (!PartyBoard_MemDiagEnabled() || !block) return;
    std::lock_guard<std::mutex> guard(gMutex);
    Record *record = findRecordLocked(block);
    if (!record) return;
    record->active = false;
    record->retaddr = retaddr;
    if (PartyBoard_MemDiagPoisonFreeEnabled() && record->internal > payloadOffset()) {
        std::memset(static_cast<unsigned char *>(block) + payloadOffset(), 0xDD,
            record->internal - payloadOffset());
    }
}

extern "C" void PartyBoard_MemDiagOnRetire(void *block)
{
    if (!PartyBoard_MemDiagEnabled() || !block) return;
    std::lock_guard<std::mutex> guard(gMutex);
    if (Record *record = findRecordLocked(block)) *record = Record {};
}

extern "C" u32 PartyBoard_MemDiagLastGoodFrame(void)
{
    return gLastGoodFrame.load(std::memory_order_acquire);
}

extern "C" s32 PartyBoard_MemDiagFirstBadFrame(void)
{
    return gFirstBadFrame.load(std::memory_order_acquire);
}

extern "C" bool PartyBoard_MemDiagCorruptionDetected(void)
{
    return gCorruptionDetected.load(std::memory_order_acquire);
}

extern "C" void PartyBoard_MemDiagStats(u32 *blocks, u32 *sweeps, double *averageMs, double *worstMs)
{
    std::lock_guard<std::mutex> guard(gMutex);
    const auto count = gSweepCount.load(std::memory_order_relaxed);
    if (blocks) *blocks = gBlockCount.load(std::memory_order_relaxed);
    if (sweeps) *sweeps = count;
    if (averageMs) *averageMs = count ? gTotalSweepMs / count : 0.0;
    if (worstMs) *worstMs = gWorstSweepMs;
}

// ---------------------------------------------------------------------------
// Sweep
// ---------------------------------------------------------------------------

namespace {

enum class Violation {
    None,
    HeaderMagic,        // magic is neither of the two allocator values
    HeaderFlagMismatch, // flag and magic disagree
    HeaderSize,         // size implausible, or block runs past the heap
    HeaderAlignment,
    LinkBroken,         // next->prev or prev->next does not point back
    LinkOutOfHeap,
    PhysicalOrder,      // block + size does not reach the next block
    WalkOverrun,        // list did not close, or is longer than any heap allows
    HeadRedzone,        // a write landed under the payload
};

const char *violationName(Violation v)
{
    switch (v) {
    case Violation::HeaderMagic: return "HEADER_MAGIC";
    case Violation::HeaderFlagMismatch: return "HEADER_FLAG_MISMATCH";
    case Violation::HeaderSize: return "HEADER_SIZE";
    case Violation::HeaderAlignment: return "HEADER_ALIGNMENT";
    case Violation::LinkBroken: return "LINK_BROKEN";
    case Violation::LinkOutOfHeap: return "LINK_OUT_OF_HEAP";
    case Violation::PhysicalOrder: return "PHYSICAL_ORDER";
    case Violation::WalkOverrun: return "WALK_OVERRUN";
    case Violation::HeadRedzone: return "HEAD_REDZONE";
    default: return "NONE";
    }
}

// One block as the walk reported it, kept so the report can show the victim's
// neighbours without walking the list a second time through damaged links.
struct Snapshot {
    void *block = nullptr;
    s32 size = 0;
    u8 magic = 0;
    u8 flag = 0;
    void *prev = nullptr;
    void *next = nullptr;
    std::uintptr_t num = 0;
    std::uintptr_t retaddr = 0;
};

struct SweepContext {
    HeapRegion *heap = nullptr;
    Violation violation = Violation::None;
    Snapshot previous;
    Snapshot victim;
    Snapshot following;
    bool haveVictim = false;
    bool expectFollowing = false;
    std::size_t badOffset = 0;
    std::uint64_t observed = 0;
    u32 blocks = 0;
};

bool withinHeap(const HeapRegion &heap, const void *address, std::size_t span)
{
    const auto base = reinterpret_cast<std::uintptr_t>(heap.base);
    const auto value = reinterpret_cast<std::uintptr_t>(address);
    if (value < base || value > base + heap.size) return false;
    return span == 0 || (value + span) <= base + heap.size;
}

// Checks one block. Everything here is derived from docs/humem_layout.md; no
// offset is assumed.
Violation inspect(SweepContext &ctx, const Snapshot &s)
{
    const HeapRegion &heap = *ctx.heap;
    const std::size_t payload = payloadOffset();

    if (s.magic != kMagicFree && s.magic != kMagicAllocated) return Violation::HeaderMagic;
    const bool magicSaysAllocated = s.magic == kMagicAllocated;
    if (magicSaysAllocated != (s.flag == 1)) return Violation::HeaderFlagMismatch;

    if (s.size <= 0 || static_cast<std::size_t>(s.size) < payload) return Violation::HeaderSize;
    if (!withinHeap(heap, s.block, static_cast<std::size_t>(s.size))) return Violation::HeaderSize;
    if (reinterpret_cast<std::uintptr_t>(s.block) % 8 != 0) return Violation::HeaderAlignment;

    if (!withinHeap(heap, s.prev, 0) || !withinHeap(heap, s.next, 0))
        return Violation::LinkOutOfHeap;

    // Physical contiguity: the split in HuMemMemoryAlloc2 places the new block at
    // block + alloc_size, so the next block is exactly there unless this is the
    // last block, whose next wraps back to the heap base.
    auto *end = static_cast<unsigned char *>(s.block) + s.size;
    if (s.next != heap.base && s.next != end) return Violation::PhysicalOrder;

    // The head redzone is only meaningful on a block this module armed, which
    // means an allocated block it has a record for.
    if (magicSaysAllocated) {
        std::lock_guard<std::mutex> guard(gMutex);
        const Record *record = findRecordLocked(s.block);
        if (record && record->active) {
            const std::size_t head = headerSize();
            auto *bytes = static_cast<unsigned char *>(s.block);
            for (std::size_t offset = head; offset + sizeof(kHeadPattern) <= payload;
                 offset += sizeof(kHeadPattern)) {
                std::uint64_t word = 0;
                std::memcpy(&word, bytes + offset, sizeof(word));
                if (word != kHeadPattern) {
                    ctx.badOffset = offset;
                    ctx.observed = word;
                    return Violation::HeadRedzone;
                }
            }
        }
    }
    return Violation::None;
}

bool sweepSink(void *context, void *block, s32 size, u8 magic, u8 flag, void *prev, void *next,
    std::uintptr_t num, std::uintptr_t retaddr)
{
    auto &ctx = *static_cast<SweepContext *>(context);
    Snapshot s;
    s.block = block; s.size = size; s.magic = magic; s.flag = flag;
    s.prev = prev; s.next = next; s.num = num; s.retaddr = retaddr;

    if (ctx.expectFollowing) {
        ctx.following = s;
        ctx.expectFollowing = false;
        return false; // the victim's neighbour is captured; stop here
    }

    ++ctx.blocks;
    if (ctx.blocks > 1u << 20) {
        ctx.violation = Violation::WalkOverrun;
        ctx.victim = s;
        ctx.haveVictim = true;
        return false;
    }

    const Violation v = inspect(ctx, s);
    if (v != Violation::None) {
        ctx.violation = v;
        ctx.victim = s;
        ctx.haveVictim = true;
        ctx.expectFollowing = true; // capture one more block, then stop
        return true;
    }
    ctx.previous = s;
    return true;
}

void describeBlock(const char *label, const Snapshot &s)
{
    if (!s.block) { reportLine("%s: <none>\n", label); return; }
    reportLine("%s header=0x%llx size=%d magic=%u flag=%u payload=0x%llx\n", label,
        static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(s.block)), s.size,
        static_cast<unsigned>(s.magic), static_cast<unsigned>(s.flag),
        static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(s.block) + payloadOffset()));
    reportLine("%s   prev=0x%llx next=0x%llx num=0x%llx retaddr=0x%llx\n", label,
        static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(s.prev)),
        static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(s.next)),
        static_cast<unsigned long long>(s.num), static_cast<unsigned long long>(s.retaddr));
    std::lock_guard<std::mutex> guard(gMutex);
    if (const Record *record = findRecordLocked(s.block)) {
        reportLine("%s   allocation #%llu requested=%llu internal=%llu frame=%u context=%d %s\n",
            label, static_cast<unsigned long long>(record->allocationId),
            static_cast<unsigned long long>(record->requested),
            static_cast<unsigned long long>(record->internal), record->frame, record->context,
            record->active ? "ACTIVE" : "FREED");
    } else {
        reportLine("%s   no diagnostic record (allocated before diagnostics, or already retired)\n",
            label);
    }
}

void writeCorruptionReport(const SweepContext &ctx, u32 frame)
{
    gReportUsed = 0;
    gReport[0] = '\0';
    reportLine("PARTYBOARD_MEM_CORRUPTION_REPORT version=1\n");
    reportLine("violation=%s\n", violationName(ctx.violation));
    reportLine("heap_id=%d heap_base=0x%llx heap_size=%llu\n", ctx.heap ? ctx.heap->id : -1,
        static_cast<unsigned long long>(
            reinterpret_cast<std::uintptr_t>(ctx.heap ? ctx.heap->base : nullptr)),
        static_cast<unsigned long long>(ctx.heap ? ctx.heap->size : 0));
    reportLine("blocks_walked_before_violation=%u\n", ctx.blocks);
    reportLine("\n[FRAMES]\n");
    reportLine("last_known_good_heap_frame=%u\n", gLastGoodFrame.load(std::memory_order_acquire));
    reportLine("first_bad_heap_frame=%u\n", frame);

    if (ctx.violation == Violation::HeadRedzone) {
        reportLine("\n[REDZONE]\n");
        reportLine("offset_in_block=%llu expected=0x%016llx observed=0x%016llx\n",
            static_cast<unsigned long long>(ctx.badOffset),
            static_cast<unsigned long long>(kHeadPattern),
            static_cast<unsigned long long>(ctx.observed));
        std::uint64_t diff = ctx.observed ^ kHeadPattern;
        unsigned changed = 0, firstByte = 0;
        bool seen = false;
        for (unsigned i = 0; i < 8; ++i) {
            if ((diff >> (i * 8)) & 0xFF) {
                ++changed;
                if (!seen) { firstByte = i; seen = true; }
            }
        }
        reportLine("bytes_changed=%u first_changed_byte_offset=%llu address=0x%llx\n", changed,
            static_cast<unsigned long long>(ctx.badOffset + firstByte),
            static_cast<unsigned long long>(
                reinterpret_cast<std::uintptr_t>(ctx.victim.block) + ctx.badOffset + firstByte));
    }

    reportLine("\n[NEIGHBOURHOOD - physical order]\n");
    describeBlock("PREVIOUS", ctx.previous);
    describeBlock("VICTIM  ", ctx.victim);
    describeBlock("FOLLOWING", ctx.following);
    reportLine("\nnote: HuMem blocks are physically contiguous, so a payload overflow from\n");
    reportLine("PREVIOUS lands on VICTIM's header. A damaged VICTIM header with an intact\n");
    reportLine("PREVIOUS payload points at an arbitrary write instead.\n");
    reportLine("\nEND\n");
}

} // namespace

extern "C" bool PartyBoard_MemDiagSweep(void)
{
    if (!PartyBoard_MemDiagEnabled()) return true;
    if (gCorruptionDetected.load(std::memory_order_acquire)) return false;

    const u32 frame = PartyBoard_NetplayFrameForDiagnostics();
    const double started = nowMs();

    SweepContext ctx;
    u32 total = 0;
    for (auto &heap : gHeaps) {
        if (!heap.used) continue;
        ctx = SweepContext {};
        ctx.heap = &heap;
        if (!PartyBoard_MemWalkHeap(heap.base, heap.size, sweepSink, &ctx)) {
            if (ctx.violation == Violation::None) ctx.violation = Violation::WalkOverrun;
        }
        total += ctx.blocks;
        if (ctx.violation != Violation::None) {
            gFirstBadFrame.store(static_cast<s32>(frame), std::memory_order_release);
            gCorruptionDetected.store(true, std::memory_order_release);
            writeCorruptionReport(ctx, frame);
            PartyBoard_MemDiagWriteReportFile();
            PartyBoard_CrashBreadcrumb("MEMORY",
                "HuMem corruption %s in heap %d at frame %u", violationName(ctx.violation),
                heap.id, frame);
            return false;
        }
    }

    const double elapsed = nowMs() - started;
    {
        std::lock_guard<std::mutex> guard(gMutex);
        gTotalSweepMs += elapsed;
        if (elapsed > gWorstSweepMs) gWorstSweepMs = elapsed;
    }
    gBlockCount.store(total, std::memory_order_relaxed);
    gSweepCount.fetch_add(1, std::memory_order_relaxed);
    gLastGoodFrame.store(frame, std::memory_order_release);
    // Periodic proof of life with the cost figures, so "no corruption found"
    // can be read as "checked and clean" rather than "never ran".
    const auto sweeps = gSweepCount.load(std::memory_order_relaxed);
    if (sweeps % 1800 == 1) {
        std::fprintf(stderr,
            "[MEM DIAG] frame %u clean: %u blocks, %u sweeps, avg %.3f ms, worst %.3f ms\n",
            frame, total, sweeps, sweeps ? gTotalSweepMs / sweeps : 0.0, gWorstSweepMs);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Report file and self-test
// ---------------------------------------------------------------------------

extern "C" void PartyBoard_MemDiagSetQuiet(bool quiet)
{
    gQuiet.store(quiet, std::memory_order_release);
}

extern "C" void PartyBoard_MemDiagWriteReportFile(void)
{
#ifdef _WIN32
    if (gQuiet.load(std::memory_order_acquire)) return;
    char stamp[32];
    SYSTEMTIME time {};
    GetLocalTime(&time);
    std::snprintf(stamp, sizeof(stamp), "%04u-%02u-%02u_%02u%02u%02u", time.wYear, time.wMonth,
        time.wDay, time.wHour, time.wMinute, time.wSecond);

    const char *directory = std::getenv("PARTYBOARD_CRASH_DIR");
    const char *peer = std::getenv("PARTYBOARD_CRASH_PEER");
    char path[kPathChars];
    if (directory && *directory) {
        std::snprintf(path, sizeof(path), "%s\\mem-corruption-peer-%s-%s.txt", directory,
            peer && *peer ? peer : "x", stamp);
    } else {
        std::snprintf(path, sizeof(path), "mem-corruption-%s.txt", stamp);
    }

    u32 blocks = 0, sweeps = 0;
    double average = 0.0, worst = 0.0;
    PartyBoard_MemDiagStats(&blocks, &sweeps, &average, &worst);
    reportLine("\n[SWEEP COST]\n");
    reportLine("blocks=%u sweeps=%u average_ms=%.3f worst_ms=%.3f\n", blocks, sweeps, average, worst);

    HANDLE file = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(file, gReport, static_cast<DWORD>(gReportUsed), &written, nullptr);
    CloseHandle(file);
    std::fprintf(stderr, "HuMem corruption report: %s\n", path);
#endif
}

// Stop the run while a coroutine stack still has room, because once it runs out
// nothing can report: the kernel cannot push an exception frame onto an
// exhausted stack, so no handler of ours ever sees the fault. This is the only
// point at which the offending process can still be named.
extern "C" void PartyBoard_CoroutineWatchdogTick(void)
{
    if (!PartyBoard_CoroutineWatchdogEnabled()) return;
    static bool tripped = false;
    if (tripped) return;

    // 2 KB of remaining room. Deep enough to be alarming, shallow enough that a
    // healthy process never reaches it: the measured survivors peak below 30%.
    char detail[512];
    if (PartyBoard_CoroutineStackWatch(2048, detail, sizeof(detail))) return;

    tripped = true;
    char message[768];
    std::snprintf(message, sizeof(message), "coroutine stack nearly exhausted at frame %u: %s",
        PartyBoard_NetplayFrameForDiagnostics(), detail);
    std::fprintf(stderr, "[STACK WATCHDOG] %s\n", message);
    PartyBoard_CrashBreadcrumb("STACK", "%s", message);
    PartyBoard_CrashReportAbnormal("COROUTINE_STACK_EXHAUSTION", message);
    PartyBoard_IsRunning = false;
}

extern "C" void PartyBoard_MemDiagTick(void)
{
    if (!PartyBoard_MemDiagEnabled()) return;
    if (PartyBoard_MemDiagSweep()) return;

    // The heap stopped being intact during this frame. Stop here rather than
    // running on: every further frame widens the damage and buries the one
    // piece of information worth having, which is where it started.
    char detail[256];
    std::snprintf(detail, sizeof(detail),
        "HuMem integrity lost: last good frame %u, first bad frame %d",
        PartyBoard_MemDiagLastGoodFrame(), PartyBoard_MemDiagFirstBadFrame());
    PartyBoard_CrashReportAbnormal("MEMORY_CORRUPTION_DETECTED", detail);
    std::fprintf(stderr, "%s\n", detail);
    PartyBoard_IsRunning = false;
}

extern "C" bool PartyBoard_MemDiagRunSelfTest(void)
{
    // The detector has to be given known corruptions before it can be trusted.
    // A fixture heap is used so nothing here touches a real HuMem heap, which is
    // why this no longer honours the master switch: it used to print SKIPPED and
    // return true whenever PARTYBOARD_MEM_DIAGNOSTICS was unset - which is
    // always, since no script set it - so the one detector that could have
    // explained the STATUS_HEAP_CORRUPTION of session S1 had never once been
    // shown to work. A self-test that passes without running is worse than no
    // self-test, because it reports the same thing as a passing one.
    const bool wasEnabled = PartyBoard_MemDiagEnabled();
    gSelfTestForced.store(true, std::memory_order_release);
    struct ForceGuard {
        ~ForceGuard() { gSelfTestForced.store(false, std::memory_order_release); }
    } forceGuard;

    constexpr std::size_t kArena = 64 * 1024;
    auto *arena = static_cast<unsigned char *>(std::malloc(kArena));
    if (!arena) return false;
    bool ok = true;
    const std::size_t payload = payloadOffset();
    PartyBoard_MemDiagSetQuiet(true);

    const auto reset = [&]() {
        std::lock_guard<std::mutex> guard(gMutex);
        for (auto &heap : gHeaps) heap = HeapRegion {};
        for (auto &record : gRecords) record = Record {};
        gCorruptionDetected.store(false, std::memory_order_release);
        gFirstBadFrame.store(-1, std::memory_order_release);
    };
    const auto build = [&]() {
        reset();
        HuMemHeapInit(arena, kArena);
        PartyBoard_MemDiagRegisterHeap(99, arena, kArena);
    };

    // 1. A healthy heap with several live allocations must sweep clean, and
    //    repeated alloc/free churn must not produce a false positive.
    build();
    void *a = HuMemMemoryAlloc(arena, 96, 0x1111);
    void *b = HuMemMemoryAlloc(arena, 200, 0x2222);
    void *c = HuMemMemoryAlloc(arena, 48, 0x3333);
    ok = ok && a && b && c;
    ok = ok && PartyBoard_MemDiagSweep();
    for (int i = 0; i < 64; ++i) {
        void *t = HuMemMemoryAlloc(arena, 32 + (i % 7) * 16, 0x4444);
        if (t) HuMemMemoryFree(t, 0x4444);
    }
    ok = ok && PartyBoard_MemDiagSweep();
    const bool noFalsePositive = ok;

    // 2. One byte written below the payload must be caught.
    build();
    a = HuMemMemoryAlloc(arena, 96, 0x1111);
    ok = ok && PartyBoard_MemDiagSweep();
    static_cast<unsigned char *>(a)[-1] = 0x41;
    const bool caughtOneByte = !PartyBoard_MemDiagSweep();
    ok = ok && caughtOneByte;

    // 3. A wider underflow must be caught as well.
    build();
    a = HuMemMemoryAlloc(arena, 96, 0x1111);
    std::memset(static_cast<unsigned char *>(a) - 16, 0x42, 16);
    const bool caughtWide = !PartyBoard_MemDiagSweep();
    ok = ok && caughtWide;

    // 4. A corrupted magic must be caught.
    build();
    a = HuMemMemoryAlloc(arena, 96, 0x1111);
    b = HuMemMemoryAlloc(arena, 96, 0x2222);
    static_cast<unsigned char *>(b)[-static_cast<std::ptrdiff_t>(payload) + 4] = 0x7F;
    const bool caughtMagic = !PartyBoard_MemDiagSweep();
    ok = ok && caughtMagic;

    // 5. An implausible size must be caught. This is also what a payload
    //    overflow from the previous block looks like, because the blocks are
    //    physically contiguous and size is the first field of the next header.
    build();
    a = HuMemMemoryAlloc(arena, 96, 0x1111);
    b = HuMemMemoryAlloc(arena, 96, 0x2222);
    {
        s32 huge = 0x7FFFFFF0;
        std::memcpy(static_cast<unsigned char *>(b) - payload, &huge, sizeof(huge));
    }
    const bool caughtSize = !PartyBoard_MemDiagSweep();
    ok = ok && caughtSize;

    // 6. A next pointer outside the heap must be caught.
    build();
    a = HuMemMemoryAlloc(arena, 96, 0x1111);
    {
        void *bogus = reinterpret_cast<void *>(static_cast<std::uintptr_t>(0x1000));
        std::memcpy(static_cast<unsigned char *>(a) - payload + 16, &bogus, sizeof(bogus));
    }
    const bool caughtLink = !PartyBoard_MemDiagSweep();
    ok = ok && caughtLink;

    // 7. A double free must be refused by the allocator itself, which the
    //    detector relies on rather than duplicating.
    build();
    a = HuMemMemoryAlloc(arena, 96, 0x1111);
    HuMemMemoryFree(a, 0x1111);
    HuMemMemoryFree(a, 0x1111); // second free is rejected on magic
    const bool doubleFreeSurvived = PartyBoard_MemDiagSweep();
    ok = ok && doubleFreeSurvived;

    reset();
    PartyBoard_MemDiagSetQuiet(false);
    std::free(arena);
    std::printf("HuMem diagnostics: %s (no false positive over 64 alloc/free cycles: %s; "
                "1-byte underflow: %s; wide underflow: %s; bad magic: %s; bad size: %s; "
                "link out of heap: %s; double free refused: %s). Fixture heap, not a real one; "
                "detector %s for the rest of this run.\n",
        ok ? "PASS" : "FAIL", noFalsePositive ? "yes" : "NO", caughtOneByte ? "caught" : "MISSED",
        caughtWide ? "caught" : "MISSED", caughtMagic ? "caught" : "MISSED",
        caughtSize ? "caught" : "MISSED", caughtLink ? "caught" : "MISSED",
        doubleFreeSurvived ? "yes" : "NO", wasEnabled ? "armed" : "NOT armed");
    return ok;
}
