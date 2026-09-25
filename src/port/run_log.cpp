#include "port/run_log.hpp"

#ifdef __ANDROID__
#include "partyboard_version.h"

#include <SDL3/SDL_system.h>

#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unwind.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#endif

namespace partyboard::run_log {

#ifdef __ANDROID__
namespace {

    int gFd = -1;
    std::mutex gMutex;

    constexpr std::array<int, 6> kFatalSignals = { SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL, SIGTRAP };
    struct sigaction gPrevious[NSIG] {};

    // Signal-safe output: no allocation, no stdio.
    void raw(const char *text) noexcept
    {
        if (gFd >= 0 && text != nullptr) {
            ::write(gFd, text, std::strlen(text));
        }
    }

    void raw_hex(std::uintptr_t value) noexcept
    {
        char digits[2 + sizeof(value) * 2 + 1];
        digits[0] = '0';
        digits[1] = 'x';
        for (std::size_t i = 0; i < sizeof(value) * 2; ++i) {
            const unsigned nibble = (value >> ((sizeof(value) * 2 - 1 - i) * 4)) & 0xF;
            digits[2 + i] = static_cast<char>(nibble < 10 ? '0' + nibble : 'a' + nibble - 10);
        }
        digits[sizeof(digits) - 1] = '\0';
        raw(digits);
    }

    void raw_int(long value) noexcept
    {
        char digits[24];
        int length = 0;
        const bool negative = value < 0;
        unsigned long magnitude = negative ? 0UL - static_cast<unsigned long>(value) : static_cast<unsigned long>(value);
        do {
            digits[length++] = static_cast<char>('0' + magnitude % 10);
            magnitude /= 10;
        } while (magnitude != 0 && length < 22);
        if (negative) {
            digits[length++] = '-';
        }
        char text[24];
        for (int i = 0; i < length; ++i) {
            text[i] = digits[length - 1 - i];
        }
        text[length] = '\0';
        raw(text);
    }

    const char *signal_name(int sig) noexcept
    {
        switch (sig) {
            case SIGSEGV:
                return "SIGSEGV";
            case SIGABRT:
                return "SIGABRT";
            case SIGBUS:
                return "SIGBUS";
            case SIGFPE:
                return "SIGFPE";
            case SIGILL:
                return "SIGILL";
            case SIGTRAP:
                return "SIGTRAP";
            default:
                return "signal";
        }
    }

    struct Backtrace {
        std::array<std::uintptr_t, 48> pcs {};
        std::size_t count = 0;
    };

    _Unwind_Reason_Code collect_frame(_Unwind_Context *context, void *arg)
    {
        auto *trace = static_cast<Backtrace *>(arg);
        const std::uintptr_t pc = _Unwind_GetIP(context);
        if (pc != 0) {
            trace->pcs[trace->count++] = pc;
        }
        return trace->count == trace->pcs.size() ? _URC_END_OF_STACK : _URC_NO_REASON;
    }

    // Not strictly signal-safe (the unwinder and dladdr), but the process is dying either way and
    // this is the one place the phone can say where.
    void write_backtrace() noexcept
    {
        Backtrace trace;
        _Unwind_Backtrace(collect_frame, &trace);
        for (std::size_t i = 0; i < trace.count; ++i) {
            const std::uintptr_t pc = trace.pcs[i];
            raw("  #");
            raw_int(static_cast<long>(i));
            raw(" pc ");
            Dl_info info {};
            if (dladdr(reinterpret_cast<void *>(pc), &info) != 0 && info.dli_fbase != nullptr) {
                raw_hex(pc - reinterpret_cast<std::uintptr_t>(info.dli_fbase));
                raw(" ");
                const char *library = info.dli_fname != nullptr ? std::strrchr(info.dli_fname, '/') : nullptr;
                raw(library != nullptr ? library + 1 : (info.dli_fname != nullptr ? info.dli_fname : "?"));
                if (info.dli_sname != nullptr) {
                    raw(" (");
                    raw(info.dli_sname);
                    raw("+");
                    raw_hex(pc - reinterpret_cast<std::uintptr_t>(info.dli_saddr));
                    raw(")");
                }
            }
            else {
                raw_hex(pc);
            }
            raw("\n");
        }
    }

    void on_fatal_signal(int sig, siginfo_t *info, void *context)
    {
        raw("FATAL SIGNAL ");
        raw_int(sig);
        raw(" (");
        raw(signal_name(sig));
        raw(") code ");
        raw_int(info != nullptr ? info->si_code : 0);
        raw(" fault address ");
        raw_hex(info != nullptr ? reinterpret_cast<std::uintptr_t>(info->si_addr) : 0);
        raw("\n");
        write_backtrace();
        ::fsync(gFd);

        // Hand the signal back to whoever had it (Android's crash dumper), then let it happen.
        sigaction(sig, &gPrevious[sig], nullptr);
        if (gPrevious[sig].sa_flags & SA_SIGINFO) {
            if (gPrevious[sig].sa_sigaction != nullptr) {
                gPrevious[sig].sa_sigaction(sig, info, context);
                return;
            }
        }
        else if (gPrevious[sig].sa_handler != SIG_DFL && gPrevious[sig].sa_handler != SIG_IGN
            && gPrevious[sig].sa_handler != nullptr)
        {
            gPrevious[sig].sa_handler(sig);
            return;
        }
        raise(sig);
    }

} // namespace

void open() noexcept
{
    const char *files = SDL_GetAndroidInternalStoragePath();
    if (files == nullptr) {
        return;
    }
    const std::string directory = std::string(files) + "/logs";
    ::mkdir(directory.c_str(), 0700);
    const std::string path = directory + "/last-run.log";
    gFd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_APPEND, 0600);
    if (gFd < 0) {
        return;
    }
    const auto startMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const std::string header = "partyboard-run-start " + std::to_string(startMs) + "\nParty Board "
        + PARTY_BOARD_WC_DESCRIBE + "\n";
    ::write(gFd, header.data(), header.size());

    struct sigaction action {};
    action.sa_sigaction = on_fatal_signal;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    for (const int sig : kFatalSignals) {
        sigaction(sig, &action, &gPrevious[sig]);
    }
}

void write(const char *level, const char *module, const char *message) noexcept
{
    if (gFd < 0) {
        return;
    }
    std::string line;
    line.reserve(64);
    line += '[';
    line += level != nullptr ? level : "?";
    line += " | ";
    line += module != nullptr ? module : "?";
    line += "] ";
    line += message != nullptr ? message : "";
    line += '\n';
    std::lock_guard lock(gMutex);
    ::write(gFd, line.data(), line.size());
}

void mark_clean_exit() noexcept
{
    if (gFd >= 0) {
        raw("partyboard-run-end clean\n");
    }
}

#else

void open() noexcept {}
void write(const char *, const char *, const char *) noexcept {}
void mark_clean_exit() noexcept {}

#endif

} // namespace partyboard::run_log
