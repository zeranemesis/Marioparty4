// Crash and abnormal-termination reporting. See include/port/crash_report.h.
//
// Three properties this file is built around:
//
//   * The writer runs on its own thread. When a libco coroutine stack overflows
//     into its guard page, the faulting thread has no stack left to run a
//     handler on, so the exception handler only copies the context, wakes a
//     pre-created writer thread that owns a full stack, and waits for it.
//
//   * Nothing allocates. A heap-corruption crash is exactly the case where the
//     allocator cannot be trusted, so every buffer is static and the files are
//     written with CreateFile/WriteFile rather than stdio.
//
//   * The exception is never swallowed. The handler returns
//     EXCEPTION_CONTINUE_SEARCH so the process dies of its original cause, with
//     its original exception code, and WER still sees it.

#include "port/crash_report.h"

#include "port/coroutine_stack.h"
#include "partyboard_version.h"

extern "C" bool PartyBoard_IsRunning;

#include <atomic>
#include <cstdarg>
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

namespace {

constexpr std::size_t kBreadcrumbs = 512;
constexpr std::size_t kBreadcrumbText = 152;
constexpr std::size_t kReportBytes = 512 * 1024;
constexpr std::size_t kPathChars = 1024;

// The live state file is rewritten this often. At 60 Hz that is twice a second:
// frequent enough to localise a termination that cannot be intercepted, cheap
// enough to be invisible.
constexpr std::uint32_t kHeartbeatFrames = 30;

// Events kept in the live state file. A crash report keeps all 512.
constexpr std::size_t kHeartbeatEvents = 96;

struct Breadcrumb {
    std::uint32_t frame = 0;
    std::uint64_t monotonicMs = 0;
    char category[16] = {};
    char text[kBreadcrumbText] = {};
};

struct Identity {
    char sessionDir[kPathChars] = {};
    char executable[kPathChars] = {};
    char commandLine[kPathChars] = {};
    char startedAt[32] = {};
    unsigned long processId = 0;
    std::int32_t peerIndex = -1;
    char role[32] = {};
};

Breadcrumb gBreadcrumbs[kBreadcrumbs];
std::atomic<std::uint64_t> gBreadcrumbCount { 0 };
std::mutex gBreadcrumbMutex;

Identity gIdentity;
PartyBoardCrashSimState gSim {};
std::mutex gSimMutex;
std::atomic<u32> gSimFrameFast { 0 };
std::atomic<s32> gSimOverlayFast { -1 };

std::atomic<bool> gInitialised { false };
std::atomic<bool> gUserShutdown { false };
std::atomic<bool> gSupervisorShutdown { false };
std::atomic<bool> gNormalExit { false };
char gShutdownHow[64] = {};

std::atomic<std::uint32_t> gHeartbeatCountdown { 0 };
char gLastReportPath[kPathChars] = {};

// Built in the writer thread only, so one static buffer is enough.
char gReport[kReportBytes];
std::size_t gReportUsed = 0;

std::uint64_t monotonicMs()
{
#ifdef _WIN32
    return static_cast<std::uint64_t>(GetTickCount64());
#else
    return 0;
#endif
}

void timestampNow(char *out, std::size_t capacity, bool fileSafe)
{
    if (capacity == 0) return;
    out[0] = '\0';
#ifdef _WIN32
    SYSTEMTIME time {};
    GetLocalTime(&time);
    std::snprintf(out, capacity, fileSafe ? "%04u-%02u-%02u_%02u%02u%02u" : "%04u-%02u-%02u %02u:%02u:%02u.%03u",
        time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond,
        fileSafe ? 0u : time.wMilliseconds);
#else
    (void)fileSafe;
#endif
}

void copyString(char *destination, std::size_t capacity, const char *source)
{
    if (capacity == 0) return;
    if (!source) { destination[0] = '\0'; return; }
    std::size_t index = 0;
    for (; index + 1 < capacity && source[index]; ++index) destination[index] = source[index];
    destination[index] = '\0';
}

void reportReset() { gReportUsed = 0; gReport[0] = '\0'; }

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

bool writeWholeFile(const char *path, const char *data, std::size_t bytes)
{
#ifdef _WIN32
    HANDLE file = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const BOOL ok = WriteFile(file, data, static_cast<DWORD>(bytes), &written, nullptr);
    CloseHandle(file);
    return ok && written == bytes;
#else
    (void)path; (void)data; (void)bytes;
    return false;
#endif
}

void buildPath(char *out, std::size_t capacity, const char *prefix, const char *stamp,
    const char *extension)
{
    if (gIdentity.sessionDir[0]) {
        if (gIdentity.peerIndex >= 0) {
            std::snprintf(out, capacity, "%s\\%s-peer-%d-%s.%s", gIdentity.sessionDir, prefix,
                static_cast<int>(gIdentity.peerIndex), stamp, extension);
        } else {
            std::snprintf(out, capacity, "%s\\%s-pid-%lu-%s.%s", gIdentity.sessionDir, prefix,
                gIdentity.processId, stamp, extension);
        }
    } else {
        std::snprintf(out, capacity, "%s-pid-%lu-%s.%s", prefix, gIdentity.processId, stamp,
            extension);
    }
}

const char *exceptionName(unsigned long code)
{
    switch (code) {
    case 0xC0000005: return "EXCEPTION_ACCESS_VIOLATION";
    case 0xC0000006: return "EXCEPTION_IN_PAGE_ERROR";
    case 0xC000001D: return "EXCEPTION_ILLEGAL_INSTRUCTION";
    case 0xC0000025: return "EXCEPTION_NONCONTINUABLE_EXCEPTION";
    case 0xC0000026: return "EXCEPTION_INVALID_DISPOSITION";
    case 0xC000008C: return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
    case 0xC000008D: return "EXCEPTION_FLT_DENORMAL_OPERAND";
    case 0xC000008E: return "EXCEPTION_FLT_DIVIDE_BY_ZERO";
    case 0xC0000090: return "EXCEPTION_FLT_INVALID_OPERATION";
    case 0xC0000091: return "EXCEPTION_FLT_OVERFLOW";
    case 0xC0000092: return "EXCEPTION_FLT_STACK_CHECK";
    case 0xC0000093: return "EXCEPTION_FLT_UNDERFLOW";
    case 0xC0000094: return "EXCEPTION_INT_DIVIDE_BY_ZERO";
    case 0xC0000095: return "EXCEPTION_INT_OVERFLOW";
    case 0xC0000096: return "EXCEPTION_PRIV_INSTRUCTION";
    case 0xC00000FD: return "EXCEPTION_STACK_OVERFLOW";
    case 0xC0000374: return "STATUS_HEAP_CORRUPTION";
    case 0xC0000409: return "STATUS_STACK_BUFFER_OVERRUN";
    case 0xC0000602: return "STATUS_FAIL_FAST_EXCEPTION";
    case 0xE06D7363: return "C++ exception (MSVC)";
    default: return "UNKNOWN";
    }
}

// Fatal, non-continuable codes only. A first-chance exception that some library
// handles normally must not produce a crash report, so the vectored handler
// deliberately ignores everything that is not in this list.
bool isFatalCode(unsigned long code)
{
    switch (code) {
    case 0xC0000005: case 0xC0000006: case 0xC000001D: case 0xC000008C:
    case 0xC0000094: case 0xC0000095: case 0xC0000096: case 0xC00000FD:
    case 0xC0000374: case 0xC0000409: case 0xC0000602:
        return true;
    default:
        return false;
    }
}

const char *shutdownIntent()
{
    if (gUserShutdown.load(std::memory_order_acquire)) return "USER_REQUESTED_EXIT";
    if (gSupervisorShutdown.load(std::memory_order_acquire)) return "SUPERVISOR_TERMINATED";
    if (gNormalExit.load(std::memory_order_acquire)) return "NORMAL_GAME_EXIT";
    return "NONE";
}

void appendIdentity()
{
    char now[32];
    timestampNow(now, sizeof(now), false);
    reportLine("PARTYBOARD_CRASH_REPORT version=1\n");
    reportLine("build_describe=%s\n", PARTY_BOARD_WC_DESCRIBE);
    reportLine("build_revision=%s\n", PARTY_BOARD_WC_REVISION);
    reportLine("build_branch=%s\n", PARTY_BOARD_WC_BRANCH);
    reportLine("build_type=%s\n", PARTY_BOARD_BUILD_TYPE);
    reportLine("build_version=%s\n", PARTY_BOARD_VERSION_STRING);
    // The git macros are empty in a local build, so the compile stamp of this
    // translation unit is what actually tells two binaries apart.
    reportLine("build_stamp=%s %s\n", __DATE__, __TIME__);
    reportLine("build_arch=%s\n", PARTY_BOARD_ARCH);
    reportLine("process_id=%lu\n", gIdentity.processId);
    reportLine("peer_index=%d\n", static_cast<int>(gIdentity.peerIndex));
    reportLine("role=%s\n", gIdentity.role[0] ? gIdentity.role : "unknown");
    reportLine("executable=%s\n", gIdentity.executable);
    reportLine("command_line=%s\n", gIdentity.commandLine);
    reportLine("started_at=%s\n", gIdentity.startedAt);
    reportLine("report_written_at=%s\n", now);
    reportLine("shutdown_requested_by_user=%d\n",
        gUserShutdown.load(std::memory_order_acquire) ? 1 : 0);
    reportLine("shutdown_requested_by_supervisor=%d\n",
        gSupervisorShutdown.load(std::memory_order_acquire) ? 1 : 0);
    reportLine("shutdown_intent=%s\n", shutdownIntent());
    if (gShutdownHow[0]) reportLine("shutdown_how=%s\n", gShutdownHow);
}

void appendSimulation()
{
    PartyBoardCrashSimState sim {};
    {
        // A try-lock: the game thread may be inside the update when it faults,
        // and a slightly stale snapshot beats a deadlocked crash handler.
        std::unique_lock<std::mutex> lock(gSimMutex, std::try_to_lock);
        sim = gSim;
    }
    reportLine("\n[SIMULATION]\n");
    reportLine("simulation_frame=%u network_frame=%u\n", sim.simulationFrame, sim.networkFrame);
    reportLine("game_context=%d overlay=%d minigame=%d\n", sim.gameContext, sim.overlay,
        sim.minigame);
    reportLine("overlay_previous=%d overlay_transition_frame=%u frames_since_transition=%d\n",
        sim.overlayPrevious, sim.overlayTransitionFrame,
        static_cast<int>(sim.simulationFrame) - static_cast<int>(sim.overlayTransitionFrame));
    reportLine("last_state_hash=%016llx at_frame=%u\n",
        static_cast<unsigned long long>(sim.lastStateHash), sim.lastStateHashFrame);
    reportLine("rng frand=%08x rand8=%08x boardrand=%08x\n", sim.frand, sim.rand8, sim.boardRand);
    reportLine("rng_calls frand=%u rand8=%u boardrand=%u\n", sim.frandCalls, sim.rand8Calls,
        sim.boardRandCalls);
    reportLine("input local=%04x/%d/%d remote=%04x/%d/%d\n", sim.localButtons,
        static_cast<int>(sim.localStickX), static_cast<int>(sim.localStickY), sim.remoteButtons,
        static_cast<int>(sim.remoteStickX), static_cast<int>(sim.remoteStickY));
    reportLine("rollback active=%d count=%u replayed=%u predicted=%u\n", sim.rollbackActive,
        sim.rollbackCount, sim.rollbackReplayed, sim.rollbackPredicted);

    reportLine("\n[NETWORK]\n");
    reportLine("received=%u rejected=%u repaired=%u send_errors=%u tx_sequence=%u\n",
        sim.received, sim.rejected, sim.repaired, sim.sendErrors, sim.txSequence);
    reportLine("packet_age_ms=%u stalled_ticks=%u longest_stall=%u\n", sim.packetAgeMs,
        sim.stalledTicks, sim.maximumStalledTicks);
    reportLine("rng_sync=%d mismatch=%d context_skew=%u local_ready=%d remote_ready=%d\n",
        sim.randomSynchronized, sim.configMismatch, sim.contextMismatchFrames, sim.localReady,
        sim.remoteReady);

    // Driven by the MusyX audio thread and deliberately NOT part of the
    // canonical hash: an audio difference that no gameplay reads is not a
    // gameplay divergence. Recorded because gameplay does block on some of these
    // (C5/C6 in docs/NETPLAY_DETERMINISM_AUDIT.md).
    reportLine("\n[AUDIO DIAGNOSTIC - not hashed]\n");
    reportLine("audio_thread_context");
    for (int channel = 0; channel < 4; ++channel)
        reportLine(" mus_status[%d]=%d", channel, sim.musStatus[channel]);
    reportLine("\n");
}

void appendBreadcrumbs(std::size_t limit = kBreadcrumbs)
{
    reportLine("\n[RECENT EVENTS - newest last]\n");
    const auto total = gBreadcrumbCount.load(std::memory_order_acquire);
    auto kept = total < kBreadcrumbs ? total : kBreadcrumbs;
    if (kept > limit) kept = limit;
    reportLine("events_recorded=%llu events_kept=%llu\n",
        static_cast<unsigned long long>(total), static_cast<unsigned long long>(kept));
    for (std::uint64_t offset = kept; offset > 0; --offset) {
        const auto index = static_cast<std::size_t>((total - offset) % kBreadcrumbs);
        const Breadcrumb &crumb = gBreadcrumbs[index];
        if (crumb.category[0] == '\0') continue;
        reportLine("EVENT frame=%u t=%llu %s %s\n", crumb.frame,
            static_cast<unsigned long long>(crumb.monotonicMs), crumb.category, crumb.text);
    }
}

void appendCoroutineStacks()
{
    reportLine("\n[COROUTINE STACKS]\n");
    static char summary[128 * 1024];
    if (PartyBoard_CoroutineStackSummary(summary, sizeof(summary)) > 0) {
        reportLine("%s", summary);
    } else {
        reportLine("no coroutine stack data (guard or watermark disabled)\n");
    }
}

#ifdef _WIN32

// --- Minidump ------------------------------------------------------------

using MiniDumpWriteDumpFn = BOOL(WINAPI *)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
    PMINIDUMP_EXCEPTION_INFORMATION, PMINIDUMP_USER_STREAM_INFORMATION,
    PMINIDUMP_CALLBACK_INFORMATION);

HMODULE gDbgHelp = nullptr;
MiniDumpWriteDumpFn gMiniDumpWriteDump = nullptr;

void loadDbgHelp()
{
    if (gDbgHelp) return;
    gDbgHelp = LoadLibraryA("dbghelp.dll");
    if (!gDbgHelp) return;
    gMiniDumpWriteDump = reinterpret_cast<MiniDumpWriteDumpFn>(
        GetProcAddress(gDbgHelp, "MiniDumpWriteDump"));
}

bool writeMinidump(const char *path, EXCEPTION_POINTERS *pointers, DWORD threadId)
{
    loadDbgHelp();
    if (!gMiniDumpWriteDump) return false;
    HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    MINIDUMP_EXCEPTION_INFORMATION info {};
    info.ThreadId = threadId;
    info.ExceptionPointers = pointers;
    info.ClientPointers = FALSE;
    // Enough to debug: every thread's stack, the module list, the exception
    // record, thread info, and the memory the stacks point at. Deliberately not
    // MiniDumpWithFullMemory, which is what makes the WER dumps 37 MB.
    const auto type = static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithThreadInfo
        | MiniDumpWithUnloadedModules | MiniDumpWithIndirectlyReferencedMemory);
    const BOOL ok = gMiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, type,
        pointers ? &info : nullptr, nullptr, nullptr);
    CloseHandle(file);
    if (ok == FALSE) {
        // CreateFileA already made the file, so a failed write leaves a
        // zero-byte .dmp behind. That is worse than no file at all: anything
        // looking for evidence of a crash finds one, and it says nothing. The
        // report still records that the dump was not written.
        DeleteFileA(path);
        return false;
    }
    return true;
}

// --- Exception detail and stack walk -------------------------------------

void appendException(EXCEPTION_POINTERS *pointers, DWORD threadId)
{
    reportLine("\n[EXCEPTION]\n");
    if (!pointers || !pointers->ExceptionRecord) {
        reportLine("no exception record: this report was written for a non-exception "
                   "abnormal termination\n");
        return;
    }
    const auto *record = pointers->ExceptionRecord;
    const auto code = static_cast<unsigned long>(record->ExceptionCode);
    reportLine("exit_code_hex=0x%08lX exit_code_dec=%lu\n", code, code);
    reportLine("exception_name=%s\n", exceptionName(code));
    reportLine("exception_address=0x%llx\n",
        static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(record->ExceptionAddress)));
    reportLine("exception_flags=0x%08lX continuable=%d\n",
        static_cast<unsigned long>(record->ExceptionFlags),
        (record->ExceptionFlags & EXCEPTION_NONCONTINUABLE) ? 0 : 1);
    reportLine("faulting_thread_id=%lu\n", static_cast<unsigned long>(threadId));

    // Which module, and where inside it.
    HMODULE module = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
            | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            static_cast<LPCSTR>(record->ExceptionAddress), &module)
        && module) {
        char name[kPathChars] = {};
        GetModuleFileNameA(module, name, sizeof(name) - 1);
        const auto base = reinterpret_cast<std::uintptr_t>(module);
        const auto address = reinterpret_cast<std::uintptr_t>(record->ExceptionAddress);
        reportLine("faulting_module=%s\n", name);
        reportLine("faulting_module_base=0x%llx faulting_offset=0x%llx\n",
            static_cast<unsigned long long>(base),
            static_cast<unsigned long long>(address - base));
    } else {
        reportLine("faulting_module=<none: address is not inside a loaded module>\n");
    }

    if (code == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2) {
        static const char *kOperation[] = { "read", "write", "execute" };
        const auto operation = record->ExceptionInformation[0];
        const auto target = record->ExceptionInformation[1];
        reportLine("access_violation operation=%s address=0x%llx\n",
            operation < 3 ? kOperation[operation] : "unknown",
            static_cast<unsigned long long>(target));
        // The point of the coroutine guard pages: say immediately whether this
        // address is a stack overflow and whose.
        char description[1024];
        if (PartyBoard_CoroutineStackDescribe(reinterpret_cast<const void *>(target),
                description, sizeof(description))) {
            reportLine("COROUTINE STACK VERDICT: %s\n", description);
        } else {
            reportLine("coroutine_stack_verdict=address is not in or near any known "
                       "coroutine stack\n");
        }
        MEMORY_BASIC_INFORMATION region {};
        if (VirtualQuery(reinterpret_cast<LPCVOID>(target), &region, sizeof(region))
            == sizeof(region)) {
            reportLine("target_region base=0x%llx size=%llu state=0x%lX protect=0x%lX\n",
                static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(region.BaseAddress)),
                static_cast<unsigned long long>(region.RegionSize),
                static_cast<unsigned long>(region.State),
                static_cast<unsigned long>(region.Protect));
        }
    }

    if (code == 0xC0000374) {
        reportLine("note=Windows raises STATUS_HEAP_CORRUPTION through __fastfail, which "
                   "normally bypasses both SetUnhandledExceptionFilter and vectored handlers. "
                   "Reaching this report at all means the corruption surfaced as an "
                   "interceptable exception.\n");
    }
}

void appendStackTrace(EXCEPTION_POINTERS *pointers)
{
    reportLine("\n[STACK TRACE]\n");
    if (!pointers || !pointers->ContextRecord) {
        reportLine("no context record available\n");
        return;
    }
    const HANDLE process = GetCurrentProcess();
    // Shared with the coroutine-stack namer: SymInitialize fails when called
    // twice, and each file doing its own made the second report a failure that
    // had not actually happened.
    if (!PartyBoard_CoroutineSymbolsReady()) {
        reportLine("symbol handler unavailable: frames are module+offset only\n");
    }

    // StackWalk64 modifies the context, so work on a copy: the minidump still
    // needs the original.
    CONTEXT context = *pointers->ContextRecord;
    STACKFRAME64 frame {};
#if defined(_M_AMD64)
    const DWORD machine = IMAGE_FILE_MACHINE_AMD64;
    frame.AddrPC.Offset = context.Rip;
    frame.AddrFrame.Offset = context.Rbp;
    frame.AddrStack.Offset = context.Rsp;
#elif defined(_M_ARM64)
    const DWORD machine = IMAGE_FILE_MACHINE_ARM64;
    frame.AddrPC.Offset = context.Pc;
    frame.AddrFrame.Offset = context.Fp;
    frame.AddrStack.Offset = context.Sp;
#else
    const DWORD machine = IMAGE_FILE_MACHINE_I386;
    frame.AddrPC.Offset = context.Eip;
    frame.AddrFrame.Offset = context.Ebp;
    frame.AddrStack.Offset = context.Esp;
#endif
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Mode = AddrModeFlat;

    alignas(SYMBOL_INFO) static unsigned char symbolStorage[sizeof(SYMBOL_INFO) + MAX_SYM_NAME];
    auto *symbol = reinterpret_cast<SYMBOL_INFO *>(symbolStorage);

    for (unsigned depth = 0; depth < 96; ++depth) {
        if (!StackWalk64(machine, process, GetCurrentThread(), &frame, &context, nullptr,
                SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) {
            break;
        }
        if (frame.AddrPC.Offset == 0) break;
        const auto address = static_cast<std::uintptr_t>(frame.AddrPC.Offset);

        char moduleName[MAX_PATH] = "<unknown>";
        std::uintptr_t moduleBase = 0;
        HMODULE module = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCSTR>(address), &module)
            && module) {
            char full[MAX_PATH] = {};
            GetModuleFileNameA(module, full, sizeof(full) - 1);
            const char *slash = std::strrchr(full, '\\');
            copyString(moduleName, sizeof(moduleName), slash ? slash + 1 : full);
            moduleBase = reinterpret_cast<std::uintptr_t>(module);
        }

        std::memset(symbolStorage, 0, sizeof(symbolStorage));
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = MAX_SYM_NAME;
        DWORD64 displacement = 0;
        const bool named = SymFromAddr(process, frame.AddrPC.Offset, &displacement, symbol) != FALSE;

        IMAGEHLP_LINE64 line {};
        line.SizeOfStruct = sizeof(line);
        DWORD lineDisplacement = 0;
        const bool located
            = SymGetLineFromAddr64(process, frame.AddrPC.Offset, &lineDisplacement, &line) != FALSE;

        reportLine("FRAME %2u 0x%llx %s+0x%llx", depth,
            static_cast<unsigned long long>(address), moduleName,
            static_cast<unsigned long long>(moduleBase ? address - moduleBase : 0));
        if (named) reportLine(" %s+0x%llx", symbol->Name,
            static_cast<unsigned long long>(displacement));
        if (located) reportLine(" [%s:%lu]", line.FileName, static_cast<unsigned long>(line.LineNumber));
        reportLine("\n");
    }
    // Deliberately no SymCleanup: the handler is process-wide and shared, and
    // tearing it down here would break any later lookup.
}

// --- Writer thread -------------------------------------------------------

struct PendingCrash {
    EXCEPTION_POINTERS *pointers = nullptr;
    DWORD threadId = 0;
    const char *reason = nullptr;
    const char *detail = nullptr;
};

PendingCrash gPending;
// Set by the guard-page handler, consumed by the writer thread.
std::atomic<std::uintptr_t> gGuardPageAddress { 0 };
std::atomic<bool> gGuardPageReported { false };
HANDLE gWriterThread = nullptr;
HANDLE gWriterWake = nullptr;
HANDLE gWriterDone = nullptr;
std::atomic<bool> gWriterShouldExit { false };
// Only the first crashing thread may write: a second one would race on the
// report buffer and would have nothing new to say.
std::atomic<bool> gCrashClaimed { false };

void writeCrashArtifacts()
{
    char stamp[32];
    timestampNow(stamp, sizeof(stamp), true);

    char dumpPath[kPathChars] = {};
    bool dumpWritten = false;
    if (gPending.pointers) {
        buildPath(dumpPath, sizeof(dumpPath), "crash", stamp, "dmp");
        dumpWritten = writeMinidump(dumpPath, gPending.pointers, gPending.threadId);
    }

    reportReset();
    appendIdentity();
    if (gPending.reason) {
        reportLine("\n[TERMINATION]\n");
        reportLine("reason=%s\n", gPending.reason);
        if (gPending.detail) reportLine("detail=%s\n", gPending.detail);
    }
    appendException(gPending.pointers, gPending.threadId);
    reportLine("minidump=%s\n", dumpWritten ? dumpPath : "<not written>");
    appendSimulation();
    appendCoroutineStacks();
    // Flush what we have before attempting the stack walk. StackWalk64 can die
    // on a thread running on a libco coroutine stack, and losing the whole
    // report because the last section failed would throw away the faulting
    // address, the module offset and the frame, which are the parts that matter
    // most. The file is rewritten complete if the walk survives.
    char reportPath[kPathChars] = {};
    buildPath(reportPath, sizeof(reportPath), "crash-report", stamp, "txt");
    const std::size_t beforeStackWalk = gReportUsed;
    reportLine("\n[PARTIAL] stack trace and recent events not appended yet\n");
    if (writeWholeFile(reportPath, gReport, gReportUsed)) {
        copyString(gLastReportPath, sizeof(gLastReportPath), reportPath);
    }
    gReportUsed = beforeStackWalk;

    appendStackTrace(gPending.pointers);
    appendBreadcrumbs();
    reportLine("\nEND\n");
    if (writeWholeFile(reportPath, gReport, gReportUsed)) {
        copyString(gLastReportPath, sizeof(gLastReportPath), reportPath);
    }
    // A stack-usage report is useful even when the crash had nothing to do with
    // stacks, because it is the evidence for sizing them later.
    char stackPath[kPathChars] = {};
    buildPath(stackPath, sizeof(stackPath), "stack-usage", stamp, "txt");
    PartyBoard_CoroutineStackWriteReport(stackPath);
}

DWORD WINAPI writerThread(LPVOID)
{
    for (;;) {
        WaitForSingleObject(gWriterWake, INFINITE);
        if (gWriterShouldExit.load(std::memory_order_acquire)) return 0;
        // A guard-page overflow has no exception context to dump: the handler
        // only left an address. Describing it needs symbols and a real stack,
        // which is exactly what this thread has and the faulting one does not.
        const auto guard = gGuardPageAddress.exchange(0, std::memory_order_acq_rel);
        if (guard != 0) {
            static char description[1024];
            const bool known = PartyBoard_CoroutineStackDescribe(
                reinterpret_cast<const void *>(guard), description, sizeof(description));
            static char message[1536];
            std::snprintf(message, sizeof(message),
                "COROUTINE STACK OVERFLOW at frame %u, faulting address 0x%llx: %s",
                gSim.simulationFrame, static_cast<unsigned long long>(guard),
                known ? description : "address is not in any known coroutine stack");
            std::fprintf(stderr, "[STACK OVERFLOW] %s\n", message);
            std::fflush(stderr);
            PartyBoard_CrashBreadcrumb(PARTYBOARD_CRASH_CAT_STACK, "%s", message);
            gPending.pointers = nullptr;
            gPending.threadId = GetCurrentThreadId();
            gPending.reason = "COROUTINE_STACK_OVERFLOW";
            gPending.detail = message;
            writeCrashArtifacts();
            SetEvent(gWriterDone);
            continue;
        }
        writeCrashArtifacts();
        SetEvent(gWriterDone);
    }
}

// Hand the work to the writer thread, which owns a full stack. A coroutine that
// overflowed into its guard page has no stack left to run any of this on.
void dispatchToWriter(EXCEPTION_POINTERS *pointers, const char *reason, const char *detail)
{
    bool expected = false;
    if (!gCrashClaimed.compare_exchange_strong(expected, true)) return;
    gPending.pointers = pointers;
    gPending.threadId = GetCurrentThreadId();
    gPending.reason = reason;
    gPending.detail = detail;
    if (gWriterThread && gWriterWake && gWriterDone) {
        SetEvent(gWriterWake);
        // Bounded: if the writer cannot finish, dying without a report beats
        // hanging a process the user is waiting on.
        WaitForSingleObject(gWriterDone, 20000);
    } else {
        // No writer thread (init never ran): try in place rather than not at all.
        writeCrashArtifacts();
    }
}

// STATUS_GUARD_PAGE_VIOLATION on one of our coroutine guard pages is the only
// chance to report a stack overflow. Windows clears the guard bit before
// dispatching, so by the time this runs the thread has a fresh page of stack and
// the retry would succeed. That is exactly the breathing room a report needs,
// and it is why the guard page is committed with PAGE_GUARD rather than left
// reserved: a reserved page gives a plain access violation on the push of a
// return address, and the kernel then cannot push an exception frame either, so
// nothing is ever reported.
bool handleCoroutineGuardPage(EXCEPTION_POINTERS *pointers)
{
    if (!pointers || !pointers->ExceptionRecord) return false;
    const auto *record = pointers->ExceptionRecord;
    if (record->ExceptionCode != STATUS_GUARD_PAGE_VIOLATION) return false;
    if (record->NumberParameters < 2) return false;

    // Everything here runs on the stack that just overflowed. The guard bit is
    // cleared by now, so a few pages are available, but symbol resolution or
    // report formatting would consume them and fault again, this time fatally.
    // So this does the minimum: remember the address, stop the run, and hand the
    // work to the writer thread, which has a stack of its own.
    gGuardPageAddress.store(record->ExceptionInformation[1], std::memory_order_release);
    bool expected = false;
    if (gGuardPageReported.compare_exchange_strong(expected, true)) {
        PartyBoard_IsRunning = false;
        if (gWriterWake) SetEvent(gWriterWake);
    }
    return true;
}

LONG CALLBACK vectoredHandler(EXCEPTION_POINTERS *pointers)
{
    // Handled and resumed: the guard bit is already cleared, so continuing lets
    // the run stop cleanly instead of dying where nothing can be recorded.
    if (handleCoroutineGuardPage(pointers)) return EXCEPTION_CONTINUE_EXECUTION;

    if (pointers && pointers->ExceptionRecord
        && isFatalCode(static_cast<unsigned long>(pointers->ExceptionRecord->ExceptionCode))) {
        dispatchToWriter(pointers, nullptr, nullptr);
    }
    // Never swallow it: the process must die of its original cause.
    return EXCEPTION_CONTINUE_SEARCH;
}

LONG WINAPI unhandledFilter(EXCEPTION_POINTERS *pointers)
{
    dispatchToWriter(pointers, nullptr, nullptr);
    return EXCEPTION_CONTINUE_SEARCH;
}

#endif // _WIN32

} // namespace

extern "C" void PartyBoard_CrashReportInit(const char *sessionDir, s32 peerIndex, const char *role)
{
    bool expected = false;
    if (!gInitialised.compare_exchange_strong(expected, true)) return;

    gIdentity.peerIndex = peerIndex;
    copyString(gIdentity.role, sizeof(gIdentity.role), role);
    // The launcher knows the seat before the game has parsed its own arguments,
    // so a report written during start-up can still be attributed.
    if (gIdentity.peerIndex < 0) {
        if (const char *fromEnv = std::getenv("PARTYBOARD_CRASH_PEER"))
            gIdentity.peerIndex = std::atoi(fromEnv);
    }
    if (!gIdentity.role[0]) {
        if (const char *fromEnv = std::getenv("PARTYBOARD_CRASH_ROLE"))
            copyString(gIdentity.role, sizeof(gIdentity.role), fromEnv);
    }
    timestampNow(gIdentity.startedAt, sizeof(gIdentity.startedAt), false);

#ifdef _WIN32
    gIdentity.processId = GetCurrentProcessId();
    GetModuleFileNameA(nullptr, gIdentity.executable, sizeof(gIdentity.executable) - 1);
    copyString(gIdentity.commandLine, sizeof(gIdentity.commandLine), GetCommandLineA());

    if (sessionDir && *sessionDir) {
        copyString(gIdentity.sessionDir, sizeof(gIdentity.sessionDir), sessionDir);
    } else if (const char *fromEnv = std::getenv("PARTYBOARD_CRASH_DIR")) {
        copyString(gIdentity.sessionDir, sizeof(gIdentity.sessionDir), fromEnv);
    } else if (const char *diagnostic = std::getenv("PARTYBOARD_NET_DIAGNOSTIC")) {
        // Fall back to the directory the netplay diagnostic already writes to,
        // so reports land beside the session even before a launcher is updated.
        copyString(gIdentity.sessionDir, sizeof(gIdentity.sessionDir), diagnostic);
        char *slash = std::strrchr(gIdentity.sessionDir, '\\');
        if (!slash) slash = std::strrchr(gIdentity.sessionDir, '/');
        if (slash) *slash = '\0'; else gIdentity.sessionDir[0] = '\0';
    }

    gWriterWake = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    gWriterDone = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (gWriterWake && gWriterDone) {
        gWriterThread = CreateThread(nullptr, 1024 * 1024, writerThread, nullptr, 0, nullptr);
    }
    AddVectoredExceptionHandler(1, vectoredHandler);
    SetUnhandledExceptionFilter(unhandledFilter);
#else
    (void)sessionDir;
#endif
    PartyBoard_CrashBreadcrumb(PARTYBOARD_CRASH_CAT_CONTEXT,
        "crash reporter armed peer=%d role=%s dir=%s", static_cast<int>(gIdentity.peerIndex),
        gIdentity.role[0] ? gIdentity.role : "unknown",
        gIdentity.sessionDir[0] ? gIdentity.sessionDir : "<cwd>");
}

extern "C" void PartyBoard_CrashSetPeer(s32 peerIndex, const char *role)
{
    if (peerIndex >= 0) gIdentity.peerIndex = peerIndex;
    if (role && *role) copyString(gIdentity.role, sizeof(gIdentity.role), role);
    PartyBoard_CrashBreadcrumb(PARTYBOARD_CRASH_CAT_CONTEXT, "peer identity peer=%d role=%s",
        static_cast<int>(gIdentity.peerIndex), gIdentity.role[0] ? gIdentity.role : "unknown");
}

extern "C" void PartyBoard_CrashBreadcrumb(const char *category, const char *format, ...)
{
    if (!category || !format) return;
    char text[kBreadcrumbText];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);

    std::uint32_t frame = 0;
    {
        std::unique_lock<std::mutex> lock(gSimMutex, std::try_to_lock);
        if (lock) frame = gSim.simulationFrame;
    }
    const auto slot = gBreadcrumbCount.fetch_add(1, std::memory_order_acq_rel) % kBreadcrumbs;
    std::lock_guard<std::mutex> guard(gBreadcrumbMutex);
    Breadcrumb &crumb = gBreadcrumbs[slot];
    crumb.frame = frame;
    crumb.monotonicMs = monotonicMs();
    copyString(crumb.category, sizeof(crumb.category), category);
    copyString(crumb.text, sizeof(crumb.text), text);
}

extern "C" void PartyBoard_CrashUpdateSimState(const PartyBoardCrashSimState *state)
{
    if (!state) return;
    // Mirrored without the mutex so a diagnostic running on the audio thread can
    // stamp its events with the current frame without ever blocking on the game
    // thread. Two independent atomics can be read a frame apart; for stamping an
    // event that is accurate enough, and it is the only property worth paying a
    // lock for here.
    gSimFrameFast.store(state->simulationFrame, std::memory_order_relaxed);
    gSimOverlayFast.store(state->gameContext, std::memory_order_relaxed);
    std::lock_guard<std::mutex> guard(gSimMutex);
    gSim = *state;
}

extern "C" u32 PartyBoard_CrashCurrentFrame(void)
{
    return gSimFrameFast.load(std::memory_order_relaxed);
}

extern "C" s32 PartyBoard_CrashCurrentOverlay(void)
{
    return gSimOverlayFast.load(std::memory_order_relaxed);
}

extern "C" void PartyBoard_CrashHeartbeat(void)
{
#ifdef _WIN32
    // Kill switch for bisection. The live state file is the one thing this
    // reporter writes while the game is running, so it is also the one thing
    // that could make the reporter a suspect in a crash. Being able to turn it
    // off is how that suspicion gets tested rather than argued about.
    static const bool enabled = [] {
        const char *value = std::getenv("PARTYBOARD_CRASH_HEARTBEAT");
        return !(value && (value[0] == '0' || value[0] == 'n' || value[0] == 'N'));
    }();
    if (!enabled) return;
    if (!gInitialised.load(std::memory_order_acquire) || !gIdentity.sessionDir[0]) return;
    if (gHeartbeatCountdown.load(std::memory_order_relaxed) > 0) {
        gHeartbeatCountdown.fetch_sub(1, std::memory_order_relaxed);
        return;
    }
    gHeartbeatCountdown.store(kHeartbeatFrames, std::memory_order_relaxed);

    // Rewritten in place, every half second. This is the only in-process
    // evidence that survives a termination the handler cannot intercept, which
    // is exactly the case for a __fastfail heap-corruption abort.
    static char buffer[32 * 1024];
    static std::mutex heartbeatMutex;
    std::lock_guard<std::mutex> guard(heartbeatMutex);
    reportReset();
    appendIdentity();
    appendSimulation();
    // Only the newest events here: the live file is rewritten twice a
    // second and is size-capped, and events are emitted oldest first, so a
    // full history would truncate away exactly the entries that matter.
    appendBreadcrumbs(kHeartbeatEvents);
    reportLine("\nEND\n");
    if (gReportUsed >= sizeof(buffer)) gReportUsed = sizeof(buffer) - 1;
    std::memcpy(buffer, gReport, gReportUsed);

    char path[kPathChars];
    if (gIdentity.peerIndex >= 0) {
        std::snprintf(path, sizeof(path), "%s\\live-state-peer-%d.txt", gIdentity.sessionDir,
            static_cast<int>(gIdentity.peerIndex));
    } else {
        std::snprintf(path, sizeof(path), "%s\\live-state-pid-%lu.txt", gIdentity.sessionDir,
            gIdentity.processId);
    }
    writeWholeFile(path, buffer, gReportUsed);
#endif
}

// Stack usage is the evidence for sizing the HuPrc stacks later, so it must not
// depend on crashing to be collected. Written on any orderly shutdown too.
void writeStackUsageReport()
{
#ifdef _WIN32
    if (!gIdentity.sessionDir[0]) return;
    char stamp[32];
    timestampNow(stamp, sizeof(stamp), true);
    char path[kPathChars];
    buildPath(path, sizeof(path), "stack-usage", stamp, "txt");
    PartyBoard_CoroutineStackWriteReport(path);
#endif
}

extern "C" void PartyBoard_CrashNoteUserShutdown(const char *how)
{
    copyString(gShutdownHow, sizeof(gShutdownHow), how ? how : "unspecified");
    gUserShutdown.store(true, std::memory_order_release);
    PartyBoard_CrashBreadcrumb(PARTYBOARD_CRASH_CAT_CONTEXT, "user requested shutdown (%s)",
        how ? how : "unspecified");
    // Flush immediately: from here on the process is expected to disappear, and
    // the live state file is how the supervisor knows this was intended.
    gHeartbeatCountdown.store(0, std::memory_order_relaxed);
    PartyBoard_CrashHeartbeat();
    writeStackUsageReport();
}

extern "C" void PartyBoard_CrashNoteSupervisorShutdown(void)
{
    gSupervisorShutdown.store(true, std::memory_order_release);
    PartyBoard_CrashBreadcrumb(PARTYBOARD_CRASH_CAT_CONTEXT, "supervisor requested shutdown");
    gHeartbeatCountdown.store(0, std::memory_order_relaxed);
    PartyBoard_CrashHeartbeat();
    writeStackUsageReport();
}

extern "C" void PartyBoard_CrashNoteNormalExit(void)
{
    gNormalExit.store(true, std::memory_order_release);
    PartyBoard_CrashBreadcrumb(PARTYBOARD_CRASH_CAT_CONTEXT, "normal game exit reached");
    gHeartbeatCountdown.store(0, std::memory_order_relaxed);
    PartyBoard_CrashHeartbeat();
    writeStackUsageReport();
}

extern "C" bool PartyBoard_CrashShutdownExpected(void)
{
    return gUserShutdown.load(std::memory_order_acquire)
        || gSupervisorShutdown.load(std::memory_order_acquire)
        || gNormalExit.load(std::memory_order_acquire);
}

extern "C" const char *PartyBoard_CrashReportAbnormal(const char *reason, const char *detail)
{
#ifdef _WIN32
    dispatchToWriter(nullptr, reason ? reason : "UNKNOWN_ABNORMAL_EXIT", detail);
    return gLastReportPath[0] ? gLastReportPath : nullptr;
#else
    (void)reason; (void)detail;
    return nullptr;
#endif
}

extern "C" void PartyBoard_CrashReportProvoke(const char *mode)
{
    const char *choice = mode && *mode ? mode : "guard";
    PartyBoard_CrashBreadcrumb(PARTYBOARD_CRASH_CAT_WARN,
        "deliberately provoking a %s fault to validate the crash reporter", choice);
    if (std::strcmp(choice, "guard") == 0) {
        // Stand in for a HuPrc process: the same allocator, the same size the
        // most common game constant produces on PC, then a write past the
        // bottom of the stack. An x86-64 stack grows down, so this is exactly
        // the direction a real overflow takes.
        void *stack = PartyBoard_CoroutineStackAlloc(0x2000 * 2);
        if (!stack) return;
        PartyBoard_CoroutineStackArm(stack, 0x2000 * 2,
            reinterpret_cast<const void *>(&PartyBoard_CrashReportProvoke));
        auto *base = static_cast<volatile unsigned char *>(stack);
        base[-64] = 0x41; // faults on the guard page
        return;
    }
    volatile int *nothing = nullptr;
    (void)*nothing;
}

extern "C" bool PartyBoard_CrashReportRunSelfTest(void)
{
    bool ok = true;

    // Exception codes must map to the names a reader will search for.
    ok = ok && std::strcmp(exceptionName(0xC0000374), "STATUS_HEAP_CORRUPTION") == 0;
    ok = ok && std::strcmp(exceptionName(0xC0000005), "EXCEPTION_ACCESS_VIOLATION") == 0;
    ok = ok && std::strcmp(exceptionName(0xC00000FD), "EXCEPTION_STACK_OVERFLOW") == 0;
    ok = ok && std::strcmp(exceptionName(0x1234), "UNKNOWN") == 0;

    // The vectored handler must ignore exceptions libraries handle themselves,
    // or every C++ throw would write a crash report.
    ok = ok && isFatalCode(0xC0000005);
    ok = ok && isFatalCode(0xC0000374);
    ok = ok && !isFatalCode(0xE06D7363);
    ok = ok && !isFatalCode(0x40010006); // DBG_PRINTEXCEPTION_C

    // A termination is only expected once something says so. This is the whole
    // point: absence of a note means abnormal.
    const bool wasUser = gUserShutdown.load(std::memory_order_acquire);
    const bool wasSupervisor = gSupervisorShutdown.load(std::memory_order_acquire);
    const bool wasNormal = gNormalExit.load(std::memory_order_acquire);
    gUserShutdown.store(false); gSupervisorShutdown.store(false); gNormalExit.store(false);
    ok = ok && !PartyBoard_CrashShutdownExpected();
    ok = ok && std::strcmp(shutdownIntent(), "NONE") == 0;
    gSupervisorShutdown.store(true);
    ok = ok && PartyBoard_CrashShutdownExpected();
    ok = ok && std::strcmp(shutdownIntent(), "SUPERVISOR_TERMINATED") == 0;
    gUserShutdown.store(true);
    // A user request outranks a supervisor one: the user's intent is the thing
    // the validation criterion is about.
    ok = ok && std::strcmp(shutdownIntent(), "USER_REQUESTED_EXIT") == 0;
    gUserShutdown.store(wasUser);
    gSupervisorShutdown.store(wasSupervisor);
    gNormalExit.store(wasNormal);

    // The breadcrumb ring must keep the newest entries and drop the oldest.
    const auto before = gBreadcrumbCount.load(std::memory_order_acquire);
    for (unsigned index = 0; index < kBreadcrumbs + 7; ++index)
        PartyBoard_CrashBreadcrumb(PARTYBOARD_CRASH_CAT_WARN, "selftest crumb %u", index);
    ok = ok && gBreadcrumbCount.load(std::memory_order_acquire) >= before + kBreadcrumbs + 7;
    reportReset();
    appendBreadcrumbs();
    // The last one written must be present and the very first must be gone.
    char needle[64];
    std::snprintf(needle, sizeof(needle), "selftest crumb %u",
        static_cast<unsigned>(kBreadcrumbs + 6));
    ok = ok && std::strstr(gReport, needle) != nullptr;
    ok = ok && std::strstr(gReport, "selftest crumb 0\n") == nullptr;
    reportReset();

    // A report must always carry the build stamp, so two peers' reports can be
    // told apart, and the simulation block must survive an empty state.
    appendIdentity();
    appendSimulation();
    ok = ok && std::strstr(gReport, "build_stamp=") != nullptr;
    ok = ok && std::strstr(gReport, "simulation_frame=") != nullptr;
    ok = ok && std::strstr(gReport, "audio_thread_context") != nullptr;
    reportReset();

    std::printf("Crash report: %s (exception names, fatal-code filter rejecting handled "
                "C++ throws, shutdown intent defaulting to abnormal, %u-entry breadcrumb ring "
                "dropping oldest first, build stamp and audio diagnostic present). Report "
                "assembly only; no real exception raised.\n",
        ok ? "PASS" : "FAIL", static_cast<unsigned>(kBreadcrumbs));
    return ok && PartyBoard_CoroutineStackRunSelfTest();
}
