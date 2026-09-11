// Guarded allocator and usage meter for libco coroutine stacks.
// See include/port/coroutine_stack.h for why this exists.

#include "port/coroutine_stack.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#endif

// A coroutine stack comes from VirtualAlloc, which AddressSanitizer does not
// track. The address space it returns can still carry shadow left poisoned by
// whatever lived there before, and the pattern fill below then writes into it
// from instrumented code, which ASan reports as a stack-buffer-underflow that
// has nothing to do with the game. Telling ASan the region is addressable ends
// that, and the call is a no-op in a build without the sanitizer.
#if defined(__SANITIZE_ADDRESS__) || defined(USE_ASAN)
extern "C" void __asan_unpoison_memory_region(void const volatile *addr, size_t size);
#define PARTYBOARD_ASAN_UNPOISON(addr, size) __asan_unpoison_memory_region((addr), (size))
#else
#define PARTYBOARD_ASAN_UNPOISON(addr, size) ((void)0)
#endif

namespace {

// Distinctive enough that a live stack word is very unlikely to match it, which
// keeps the high-water scan from stopping early.
constexpr std::uint64_t kPattern = 0xC0DEC0DEC0DEC0DEull;

// libco keeps its register save area AT THE BASE of the coroutine handle, not on
// the coroutine stack proper. co_swap on Win64 writes, relative to the handle:
// rsp at 0, rbp at 8, rsi at 16, rdi at 24, rbx at 32, r12-r15 at 40..71, then
// xmm6-xmm8 at 80..127, and after its "add rdx,112" the remaining xmm9-xmm15
// land at 128..239 (extern/libco/amd64.c:21-44, SSE enabled because
// LIBCO_NO_SSE is not defined). The area therefore spans offsets 0 to 239.
//
// Reserving only 16 bytes here was a measurement bug: every coroutine that had
// been switched away from at least once had its base overwritten by that save
// area, so the scan found a non-pattern word at offset 16 and reported a peak of
// size-16 for it. That looked like a stack used to the floor and was an
// artifact. 256 clears the save area with room to spare.
constexpr std::size_t kReservedAtBase = 256;

// co_derive sets stack_top to handle+size, subtracts 32, aligns down to 16, then
// pushes two words, so the top 48 bytes belong to libco. 64 covers it.
constexpr std::size_t kReservedAtTop = 64;

constexpr std::size_t kMaxTracked = 2048;
constexpr std::size_t kMaxRetired = 256;
constexpr std::size_t kNameChars = 96;

struct Entry {
    // Written under the lock before `live` is published, so the crash path can
    // read these without taking the lock.
    void *reservation = nullptr;
    unsigned char *usable = nullptr;
    std::size_t reservationBytes = 0;
    std::size_t usableBytes = 0;
    std::size_t requestedBytes = 0;
    const void *entryPoint = nullptr;
    bool armed = false;
    std::atomic<bool> live { false };
};

// A retired stack, keyed by entry point so every instance of the same HuPrc
// process folds into one worst case.
struct Retired {
    const void *entryPoint = nullptr;
    std::size_t requestedBytes = 0;
    std::size_t usableBytes = 0;
    std::size_t peakBytes = 0;
    std::uint32_t samples = 0;
    std::uint32_t saturatedSamples = 0;
    bool used = false;
};

Entry gEntries[kMaxTracked];
Retired gRetired[kMaxRetired];
std::mutex gMutex;
std::atomic<std::uint32_t> gHighestSlot { 0 };
std::atomic<std::uint32_t> gFallbackAllocations { 0 };

std::size_t pageSize()
{
#ifdef _WIN32
    static const std::size_t cached = [] {
        SYSTEM_INFO info {};
        GetSystemInfo(&info);
        return info.dwPageSize ? static_cast<std::size_t>(info.dwPageSize) : std::size_t(4096);
    }();
    return cached;
#else
    return 4096;
#endif
}

bool envDisabled(const char *name)
{
    const char *value = std::getenv(name);
    return value && (value[0] == '0' || value[0] == 'n' || value[0] == 'N');
}

std::size_t roundUp(std::size_t value, std::size_t multiple)
{
    if (multiple == 0) return value;
    const std::size_t remainder = value % multiple;
    return remainder ? value + (multiple - remainder) : value;
}

// Name a process by its entry point. Resolution happens only when a report is
// produced, never on the creation path, and degrades to module+offset and then
// to a bare address rather than failing.
void describeEntryPoint(const void *entryPoint, char *out, std::size_t capacity)
{
    if (capacity == 0) return;
    out[0] = '\0';
    if (!entryPoint) { std::snprintf(out, capacity, "unknown"); return; }
    const auto address = reinterpret_cast<std::uintptr_t>(entryPoint);
#ifdef _WIN32
    alignas(SYMBOL_INFO) static unsigned char storage[sizeof(SYMBOL_INFO) + MAX_SYM_NAME];
    auto *symbol = reinterpret_cast<SYMBOL_INFO *>(storage);
    std::memset(storage, 0, sizeof(storage));
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = MAX_SYM_NAME;
    DWORD64 displacement = 0;
    PartyBoard_CoroutineSymbolsReady();
    if (SymFromAddr(GetCurrentProcess(), address, &displacement, symbol)) {
        if (displacement)
            std::snprintf(out, capacity, "%s+0x%llx", symbol->Name,
                static_cast<unsigned long long>(displacement));
        else
            std::snprintf(out, capacity, "%s", symbol->Name);
        return;
    }
    HMODULE module = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
            | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(address), &module)
        && module) {
        char full[MAX_PATH] = {};
        GetModuleFileNameA(module, full, sizeof(full) - 1);
        const char *slash = std::strrchr(full, '\\');
        std::snprintf(out, capacity, "%s+0x%llx", slash ? slash + 1 : full,
            static_cast<unsigned long long>(address - reinterpret_cast<std::uintptr_t>(module)));
        return;
    }
#endif
    std::snprintf(out, capacity, "0x%llx", static_cast<unsigned long long>(address));
}

// The lowest address that no longer holds the pattern is the deepest point the
// process ever reached, because an x86-64 stack grows downwards.
//
// `saturated` means the scan reached the floor of the measurable window: the
// stack was used at least that deep and the real requirement is unknown, so a
// report must present the number as a lower bound rather than as the answer.
struct Usage {
    std::size_t peak = 0;
    bool saturated = false;
};

Usage scanUsage(const Entry &entry)
{
    Usage usage;
    if (!entry.armed || !entry.usable) return usage;
    if (entry.usableBytes <= kReservedAtBase + kReservedAtTop) return usage;
    const unsigned char *base = entry.usable;
    const std::size_t last = entry.usableBytes - kReservedAtTop;
    for (std::size_t offset = kReservedAtBase; offset + sizeof(std::uint64_t) <= last;
         offset += sizeof(std::uint64_t)) {
        std::uint64_t word = 0;
        std::memcpy(&word, base + offset, sizeof(word));
        if (word != kPattern) {
            usage.peak = entry.usableBytes - offset;
            usage.saturated = offset == kReservedAtBase;
            return usage;
        }
    }
    return usage;
}

std::size_t scanPeak(const Entry &entry) { return scanUsage(entry).peak; }

// Caller must hold gMutex.
Entry *findEntryLocked(const void *usable)
{
    const auto highest = gHighestSlot.load(std::memory_order_acquire);
    for (std::uint32_t index = 0; index < highest && index < kMaxTracked; ++index) {
        if (gEntries[index].live.load(std::memory_order_acquire)
            && gEntries[index].usable == usable) {
            return &gEntries[index];
        }
    }
    return nullptr;
}

// Fold a finished stack's peak into the per-entry-point worst case, so a report
// after a long session shows the worst any instance of that process reached.
// Caller must hold gMutex.
void retireLocked(const Entry &entry, const Usage &usage)
{
    if (usage.peak == 0 || !entry.entryPoint) return;
    Retired *spare = nullptr;
    for (auto &slot : gRetired) {
        if (slot.used && slot.entryPoint == entry.entryPoint) {
            if (usage.peak > slot.peakBytes) slot.peakBytes = usage.peak;
            slot.requestedBytes = entry.requestedBytes;
            slot.usableBytes = entry.usableBytes;
            ++slot.samples;
            if (usage.saturated) ++slot.saturatedSamples;
            return;
        }
        if (!slot.used && !spare) spare = &slot;
    }
    if (!spare) return;
    spare->entryPoint = entry.entryPoint;
    spare->requestedBytes = entry.requestedBytes;
    spare->usableBytes = entry.usableBytes;
    spare->peakBytes = usage.peak;
    spare->samples = 1;
    spare->saturatedSamples = usage.saturated ? 1 : 0;
    spare->used = true;
}

} // namespace

// One symbol-handler initialisation for the whole process. SymInitialize fails
// with ERROR_INVALID_PARAMETER when called twice, and two files needing symbols
// made the second one report a failure that had not actually happened.
extern "C" bool PartyBoard_CoroutineSymbolsReady(void)
{
#ifdef _WIN32
    static const bool ready = [] {
        // AddressSanitizer symbolizes through DbgHelp as well, and DbgHelp is a
        // single-owner API: with both of us initialised it warns that the app is
        // already using it and its report dies during symbolization, leaving only
        // the error header. PARTYBOARD_CRASH_SYMBOLS=0 hands DbgHelp to ASan.
        // Our own reports then carry module+offset instead of names, which is
        // the right trade while ASan is the one doing the reporting. This
        // disables OUR symbolization only; the sanitizer keeps every check.
        const char *value = std::getenv("PARTYBOARD_CRASH_SYMBOLS");
        if (value && (value[0] == '0' || value[0] == 'n' || value[0] == 'N')) return false;
        SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
        return SymInitialize(GetCurrentProcess(), nullptr, TRUE) != FALSE;
    }();
    return ready;
#else
    return false;
#endif
}

extern "C" bool PartyBoard_CoroutineGuardEnabled(void)
{
#ifdef _WIN32
    static const bool enabled = !envDisabled("PARTYBOARD_COROUTINE_GUARD");
    return enabled;
#else
    return false;
#endif
}

extern "C" bool PartyBoard_CoroutineWatermarkEnabled(void)
{
    static const bool enabled = !envDisabled("PARTYBOARD_STACK_WATERMARK");
    return enabled;
}

extern "C" void *PartyBoard_CoroutineStackAlloc(size_t size)
{
#ifdef _WIN32
    if (PartyBoard_CoroutineGuardEnabled() && size != 0) {
        const std::size_t page = pageSize();
        const std::size_t usableBytes = roundUp(size, page);
        // One guard page below catches an overflow, one above catches a write
        // past the top. Both stay reserved but uncommitted, so they cost address
        // space only and still fault on any access.
        const std::size_t total = page + usableBytes + page;
        auto *reservation = static_cast<unsigned char *>(
            VirtualAlloc(nullptr, total, MEM_RESERVE, PAGE_NOACCESS));
        if (reservation) {
            if (VirtualAlloc(reservation + page, usableBytes, MEM_COMMIT, PAGE_READWRITE)) {
                PARTYBOARD_ASAN_UNPOISON(reservation + page, usableBytes);
                std::lock_guard<std::mutex> guard(gMutex);
                for (std::uint32_t index = 0; index < kMaxTracked; ++index) {
                    Entry &entry = gEntries[index];
                    if (entry.live.load(std::memory_order_relaxed)) continue;
                    entry.reservation = reservation;
                    entry.usable = reservation + page;
                    entry.reservationBytes = total;
                    entry.usableBytes = usableBytes;
                    entry.requestedBytes = size;
                    entry.entryPoint = nullptr;
                    entry.armed = false;
                    entry.live.store(true, std::memory_order_release);
                    std::uint32_t expected = gHighestSlot.load(std::memory_order_relaxed);
                    while (index + 1 > expected
                        && !gHighestSlot.compare_exchange_weak(expected, index + 1)) {
                    }
                    return entry.usable;
                }
                // Registry full: release and fall through to malloc rather than
                // leaking a reservation we could never free.
                VirtualFree(reservation, 0, MEM_RELEASE);
            } else {
                VirtualFree(reservation, 0, MEM_RELEASE);
            }
        }
    }
#endif
    gFallbackAllocations.fetch_add(1, std::memory_order_relaxed);
    return std::malloc(size);
}

extern "C" void PartyBoard_CoroutineStackFree(void *pointer)
{
    if (!pointer) return;
#ifdef _WIN32
    void *reservation = nullptr;
    {
        std::lock_guard<std::mutex> guard(gMutex);
        Entry *entry = findEntryLocked(pointer);
        if (entry) {
            retireLocked(*entry, scanUsage(*entry));
            reservation = entry->reservation;
            entry->live.store(false, std::memory_order_release);
            entry->reservation = nullptr;
            entry->usable = nullptr;
            entry->entryPoint = nullptr;
            entry->armed = false;
        }
    }
    if (reservation) {
        VirtualFree(reservation, 0, MEM_RELEASE);
        return;
    }
#endif
    std::free(pointer);
}

extern "C" void PartyBoard_CoroutineStackArm(void *stack, u32 size, const void *entryPoint)
{
    if (!stack) return;
    std::lock_guard<std::mutex> guard(gMutex);
    Entry *entry = findEntryLocked(stack);
    if (!entry) return;
    // Recorded even when the meter is off, so a crash report can still name the
    // process that owns a guard page.
    entry->entryPoint = entryPoint;
    if (!PartyBoard_CoroutineWatermarkEnabled()) return;
    const std::size_t usable = entry->usableBytes < size ? entry->usableBytes : size;
    if (usable <= kReservedAtBase + kReservedAtTop) return;
    unsigned char *base = entry->usable;
    const std::size_t last = usable - kReservedAtTop;
    for (std::size_t offset = kReservedAtBase; offset + sizeof(std::uint64_t) <= last;
         offset += sizeof(std::uint64_t)) {
        std::memcpy(base + offset, &kPattern, sizeof(kPattern));
    }
    entry->armed = true;
}

extern "C" u32 PartyBoard_CoroutineStackPeak(const void *stack, u32 size)
{
    (void)size;
    if (!stack) return 0;
    std::lock_guard<std::mutex> guard(gMutex);
    const Entry *entry = findEntryLocked(stack);
    return entry ? static_cast<u32>(scanPeak(*entry)) : 0u;
}

extern "C" void PartyBoard_CoroutineStackRetire(const void *stack)
{
    if (!stack) return;
    std::lock_guard<std::mutex> guard(gMutex);
    Entry *entry = findEntryLocked(stack);
    if (entry) retireLocked(*entry, scanUsage(*entry));
}

extern "C" bool PartyBoard_CoroutineStackDescribe(const void *address, char *out, size_t outSize)
{
    if (!address || !out || outSize == 0) return false;
    out[0] = '\0';
    // Deliberately lock-free: this runs from a crashing thread that may already
    // hold the lock, and a racy read of a published entry is acceptable here.
    const auto value = reinterpret_cast<std::uintptr_t>(address);
    const auto highest = gHighestSlot.load(std::memory_order_acquire);
    for (std::uint32_t index = 0; index < highest && index < kMaxTracked; ++index) {
        const Entry &entry = gEntries[index];
        if (!entry.live.load(std::memory_order_acquire) || !entry.reservation) continue;
        const auto reservation = reinterpret_cast<std::uintptr_t>(entry.reservation);
        const auto usable = reinterpret_cast<std::uintptr_t>(entry.usable);
        const auto usableEnd = usable + entry.usableBytes;
        const auto reservationEnd = reservation + entry.reservationBytes;
        if (value < reservation || value >= reservationEnd) continue;

        char name[kNameChars];
        describeEntryPoint(entry.entryPoint, name, sizeof(name));
        if (value < usable) {
            std::snprintf(out, outSize,
                "GUARD PAGE BELOW the coroutine stack of %s: stack 0x%llx-0x%llx, size %llu "
                "(game constant %llu doubled by process.c), guard 0x%llx-0x%llx, faulting "
                "address is %llu bytes past the bottom of the stack, peak observed %llu",
                name, static_cast<unsigned long long>(usable),
                static_cast<unsigned long long>(usableEnd),
                static_cast<unsigned long long>(entry.usableBytes),
                static_cast<unsigned long long>(entry.requestedBytes / 2),
                static_cast<unsigned long long>(reservation),
                static_cast<unsigned long long>(usable),
                static_cast<unsigned long long>(usable - value),
                static_cast<unsigned long long>(scanPeak(entry)));
            return true;
        }
        if (value >= usableEnd) {
            std::snprintf(out, outSize,
                "GUARD PAGE ABOVE the coroutine stack of %s: stack 0x%llx-0x%llx size %llu, "
                "faulting address is %llu bytes past the top",
                name, static_cast<unsigned long long>(usable),
                static_cast<unsigned long long>(usableEnd),
                static_cast<unsigned long long>(entry.usableBytes),
                static_cast<unsigned long long>(value - usableEnd + 1));
            return true;
        }
        std::snprintf(out, outSize,
            "inside the coroutine stack of %s: base 0x%llx size %llu (requested %llu), "
            "offset %llu from base, peak observed %llu",
            name, static_cast<unsigned long long>(usable),
            static_cast<unsigned long long>(entry.usableBytes),
            static_cast<unsigned long long>(entry.requestedBytes),
            static_cast<unsigned long long>(value - usable),
            static_cast<unsigned long long>(scanPeak(entry)));
        return true;
    }
    return false;
}

extern "C" u32 PartyBoard_CoroutineStackSummary(char *out, size_t outSize)
{
    if (!out || outSize == 0) return 0;
    out[0] = '\0';
    std::size_t written = 0;
    std::uint32_t lines = 0;
    const auto room = [&]() -> std::size_t {
        return written + 1 < outSize ? outSize - written : 0;
    };
    const auto advance = [&](int count) {
        if (count <= 0) return;
        written += static_cast<std::size_t>(count);
        if (written >= outSize) written = outSize - 1;
        ++lines;
    };

    advance(std::snprintf(out + written, room(),
        "coroutine_guard=%d watermark=%d fallback_allocations=%u\n",
        PartyBoard_CoroutineGuardEnabled() ? 1 : 0,
        PartyBoard_CoroutineWatermarkEnabled() ? 1 : 0,
        gFallbackAllocations.load(std::memory_order_relaxed)));

    std::lock_guard<std::mutex> guard(gMutex);
    char name[kNameChars];
    const auto highest = gHighestSlot.load(std::memory_order_acquire);
    for (std::uint32_t index = 0; index < highest && index < kMaxTracked; ++index) {
        const Entry &entry = gEntries[index];
        if (!entry.live.load(std::memory_order_acquire) || !entry.armed) continue;
        const Usage usage = scanUsage(entry);
        if (!usage.peak || room() == 0) continue;
        describeEntryPoint(entry.entryPoint, name, sizeof(name));
        const double ratio = entry.usableBytes
            ? 100.0 * static_cast<double>(usage.peak) / static_cast<double>(entry.usableBytes) : 0.0;
        advance(std::snprintf(out + written, room(),
            "STACK live process=%s requested=%llu pc_size=%llu peak%s%llu headroom=%lld "
            "usage=%.1f%%%s\n",
            name, static_cast<unsigned long long>(entry.requestedBytes / 2),
            static_cast<unsigned long long>(entry.usableBytes),
            usage.saturated ? ">=" : "=",
            static_cast<unsigned long long>(usage.peak),
            static_cast<long long>(entry.usableBytes) - static_cast<long long>(usage.peak), ratio,
            usage.saturated ? " SATURATED measurement floor reached, real usage unknown" : ""));
    }
    for (const auto &slot : gRetired) {
        if (!slot.used || room() == 0) continue;
        describeEntryPoint(slot.entryPoint, name, sizeof(name));
        const double ratio = slot.usableBytes
            ? 100.0 * static_cast<double>(slot.peakBytes) / static_cast<double>(slot.usableBytes) : 0.0;
        advance(std::snprintf(out + written, room(),
            "STACK retired process=%s requested=%llu pc_size=%llu peak%s%llu headroom=%lld "
            "usage=%.1f%% samples=%u saturated=%u\n",
            name, static_cast<unsigned long long>(slot.requestedBytes / 2),
            static_cast<unsigned long long>(slot.usableBytes),
            slot.saturatedSamples ? ">=" : "=",
            static_cast<unsigned long long>(slot.peakBytes),
            static_cast<long long>(slot.usableBytes) - static_cast<long long>(slot.peakBytes),
            ratio, slot.samples, slot.saturatedSamples));
    }
    return lines;
}

extern "C" void PartyBoard_CoroutineStackWriteReport(const char *path)
{
    if (!path || !*path) return;
    static char buffer[256 * 1024];
    if (PartyBoard_CoroutineStackSummary(buffer, sizeof(buffer)) == 0) return;
    std::FILE *file = std::fopen(path, "wb");
    if (!file) return;
    std::fputs(buffer, file);
    std::fclose(file);
}

extern "C" bool PartyBoard_CoroutineStackRunSelfTest(void)
{
#ifdef _WIN32
    if (!PartyBoard_CoroutineGuardEnabled() || !PartyBoard_CoroutineWatermarkEnabled()) return true;
    // The size HuPrcCreate actually asks libco for on PC, for the most common
    // game constant: 0x2000 doubled by process.c.
    constexpr u32 kSize = 0x2000 * 2;
    const std::size_t page = pageSize();
    void *stack = PartyBoard_CoroutineStackAlloc(kSize);
    if (!stack) return false;
    auto *base = static_cast<unsigned char *>(stack);
    bool ok = true;

    // Use this function's own address as a stand-in entry point, so the name
    // resolution path is exercised too.
    const void *entry = reinterpret_cast<const void *>(&PartyBoard_CoroutineStackRunSelfTest);
    PartyBoard_CoroutineStackArm(stack, kSize, entry);

    // Nothing has run on it, so the pattern must still be intact end to end.
    ok = ok && PartyBoard_CoroutineStackPeak(stack, kSize) == 0;

    // Regression for a measurement bug this meter shipped with. co_swap keeps
    // its register save area at the BASE of the handle, spanning offsets 0 to
    // 239, so every coroutine that has ever been switched away from has those
    // bytes overwritten. With the window starting at 16 the scan landed inside
    // that area and reported a peak of size-16 for an untouched stack, which
    // read as a stack used to the floor. Simulating the save area must now leave
    // the measurement at zero.
    std::memset(base, 0xA5, 240);
    ok = ok && PartyBoard_CoroutineStackPeak(stack, kSize) == 0;

    // A write at a known depth must be reported as exactly that depth, and must
    // not be flagged as saturated, because the floor was not reached.
    const std::size_t offset = 0x400;
    std::memset(base + offset, 0x5A, 8);
    ok = ok && PartyBoard_CoroutineStackPeak(stack, kSize) == kSize - offset;
    {
        std::lock_guard<std::mutex> guard(gMutex);
        const Entry *entry2 = findEntryLocked(stack);
        ok = ok && entry2 && !scanUsage(*entry2).saturated;
    }

    // A write at the very floor of the measurable window must be reported as
    // saturated, so a report presents it as a lower bound instead of an answer.
    std::memset(base + kReservedAtBase, 0x5A, 8);
    {
        std::lock_guard<std::mutex> guard(gMutex);
        const Entry *entry2 = findEntryLocked(stack);
        const Usage usage = entry2 ? scanUsage(*entry2) : Usage {};
        ok = ok && usage.saturated && usage.peak == kSize - kReservedAtBase;
    }

    // The guard page below must be uncommitted, so any access to it faults.
    // VirtualQuery checks this without raising the exception it guards against.
    MEMORY_BASIC_INFORMATION info {};
    if (VirtualQuery(base - page, &info, sizeof(info)) == sizeof(info)) {
        ok = ok && (info.State & MEM_COMMIT) == 0;
    } else {
        ok = false;
    }
    // And the page above as well, which catches a write past the top.
    if (VirtualQuery(base + kSize, &info, sizeof(info)) == sizeof(info)) {
        ok = ok && (info.State & MEM_COMMIT) == 0;
    } else {
        ok = false;
    }

    // An address in the guard page must be named as an overflow, one inside the
    // stack must not be, and an unrelated address must not match at all.
    char text[1024];
    ok = ok && PartyBoard_CoroutineStackDescribe(base - 8, text, sizeof(text));
    ok = ok && std::strstr(text, "GUARD PAGE BELOW") != nullptr;
    ok = ok && PartyBoard_CoroutineStackDescribe(base + kSize + 8, text, sizeof(text));
    ok = ok && std::strstr(text, "GUARD PAGE ABOVE") != nullptr;
    ok = ok && PartyBoard_CoroutineStackDescribe(base + 64, text, sizeof(text));
    ok = ok && std::strstr(text, "inside the coroutine stack") != nullptr;
    ok = ok && !PartyBoard_CoroutineStackDescribe(reinterpret_cast<void *>(0x10), text, sizeof(text));

    PartyBoard_CoroutineStackFree(stack);
    // After the free the address must no longer resolve to a live stack, and the
    // measured peak must have survived into the retired table.
    ok = ok && !PartyBoard_CoroutineStackDescribe(base + 64, text, sizeof(text));
    static char summary[64 * 1024];
    PartyBoard_CoroutineStackSummary(summary, sizeof(summary));
    ok = ok && std::strstr(summary, "STACK retired process=") != nullptr;
    std::printf("Coroutine stack guard: %s (guard page below and above a %u-byte stack, "
                "high-water mark exact at a known depth of %llu, overflow attributed to its "
                "process). Allocator and meter only; no game process ran on it.\n",
        ok ? "PASS" : "FAIL", static_cast<unsigned>(kSize),
        static_cast<unsigned long long>(kSize - offset));
    return ok;
#else
    return true;
#endif
}
