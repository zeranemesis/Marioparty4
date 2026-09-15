#include <aurora/main.h>
#include "port/netplay_transport.hpp"
#include "port/netplay_runtime.h"
#include "port/rollback.h"
#include "port/crash_report.h"
#include "port/mem_diagnostics.h"
#include "port/audio_lifetime.h"
#include "port/crash_manifest.h"
#include "port/crash_uploader.h"

#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#include <cstdio>
#include <windows.h>
#endif

extern "C" int port_main(int argc, char* argv[]);
extern "C" bool PartyBoard_OnlineCheckDisc(void);
extern "C" bool PartyBoard_OnlineBarrierProbe(void);
extern "C" bool PartyBoard_ModsRunSelfTest(void);

int main(int argc, char *argv[])
{
    // First statement in the process: a fault before this point would leave no
    // report, and the whole point is that no termination goes unexplained. The
    // session directory and seat come from the launcher's environment; both are
    // refined below once the command line has been parsed.
    PartyBoard_CrashReportInit(nullptr, -1, nullptr);

    if (argc == 2 && std::strcmp(argv[1], "--crash-report-self-test") == 0)
        return PartyBoard_CrashReportRunSelfTest() ? 0 : 1;
    if (argc == 2 && std::strcmp(argv[1], "--mem-diagnostics-self-test") == 0)
        return PartyBoard_MemDiagRunSelfTest() ? 0 : 1;
    if (argc == 2 && std::strcmp(argv[1], "--audio-lifetime-self-test") == 0)
        return PartyBoard_AudioLifetimeRunSelfTest() ? 0 : 1;
    if (argc == 2 && std::strcmp(argv[1], "--crash-manifest-self-test") == 0)
        return PartyBoard_CrashManifestRunSelfTest() ? 0 : 1;
    if (argc == 2 && std::strcmp(argv[1], "--crash-uploader-self-test") == 0)
        return PartyBoard_CrashUploaderRunSelfTest() ? 0 : 1;
    if (argc == 3 && std::strcmp(argv[1], "--crash-report-provoke") == 0) {
        // Raises a real exception on purpose; the process is expected to die of
        // it so the reporter can be verified end to end.
        PartyBoard_CrashReportProvoke(argv[2]);
        return 0; // only reached if the fault somehow did not happen
    }
    if (argc == 2 && std::strcmp(argv[1], "--online-disc-check") == 0)
        return PartyBoard_OnlineCheckDisc() ? 0 : 3;
    if (argc == 2 && std::strcmp(argv[1], "--mods-self-test") == 0)
        return PartyBoard_ModsRunSelfTest() ? 0 : 1;
    if (argc == 2 && std::strcmp(argv[1], "--rollback-self-test") == 0)
        return PartyBoard_RollbackRunSelfTest() ? 0 : 1;
    if (argc == 2 && std::strcmp(argv[1], "--netplay-self-test") == 0) {
        // Every component runs, and every component reports. The chain of &&
        // this replaced stopped at the first failure, so one broken component
        // hid an unknown number of others behind it - and a component became
        // silently untested the day anything before it started failing. A
        // release gate that cannot say which of its eight checks ran is not a
        // gate.
        int total = 0;
        int failed = 0;
        auto check = [&](const char* name, bool ok) {
            ++total;
            if (!ok) ++failed;
            std::printf("SELFTEST %-16s %s\n", name, ok ? "PASS" : "FAIL");
            std::fflush(stdout);
        };
        check("rollback", PartyBoard_RollbackRunSelfTest());
        check("transport", PartyBoard_NetTransportRunSelfTest());
        check("netplay-runtime", PartyBoard_NetplayRuntimeRunSelfTest());
        check("crash-report", PartyBoard_CrashReportRunSelfTest());
        check("mem-diagnostics", PartyBoard_MemDiagRunSelfTest());
        check("audio-lifetime", PartyBoard_AudioLifetimeRunSelfTest());
        check("crash-manifest", PartyBoard_CrashManifestRunSelfTest());
        check("crash-uploader", PartyBoard_CrashUploaderRunSelfTest());
        std::printf("SELFTEST total=%d failed=%d\n", total, failed);
        std::fflush(stdout);
        return failed == 0 ? 0 : 1;
    }

    // Anything the previous run left behind becomes a queued incident here, at
    // the next launch rather than while the last one was dying. A no-op in a
    // supervised session, where reports belong to the campaign directory.
    PartyBoard_CrashQueueScan(PartyBoard_CrashReportDirectory());

    if (!PartyBoard_NetplayConfigureFromArgs(argc, argv))
        return 2;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--netplay-start-probe") == 0
            && !PartyBoard_OnlineBarrierProbe()) { PartyBoard_NetplayShutdown(); return 4; }
    }
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--netplay-pad-probe") == 0) {
            const bool passed = PartyBoard_NetplayPadRunProbe();
            PartyBoard_NetplayShutdown();
            return passed ? 0 : 1;
        }
    }

#if defined(_WIN32)
    HANDLE singleInstance = nullptr;
    if (!PartyBoard_NetplayAllowsMultipleInstances())
    {
        singleInstance = CreateMutexW(nullptr, TRUE, L"Local\\MarioPartyRD.PartyBoard.SingleInstance");
        if (singleInstance == nullptr || GetLastError() == ERROR_ALREADY_EXISTS)
        {
            if (singleInstance != nullptr)
                CloseHandle(singleInstance);
            std::fputs("PartyBoard is already running; refusing a second audio instance.\n", stderr);
            return 0;
        }
    }
#endif

    const int result = port_main(argc, argv);
    // Reaching here means the main loop returned of its own accord, so the
    // termination is expected and must not be classified as a crash.
    PartyBoard_CrashNoteNormalExit();

#if defined(_WIN32)
    if (singleInstance != nullptr)
    {
        ReleaseMutex(singleInstance);
        CloseHandle(singleInstance);
    }
#endif
    PartyBoard_NetplayShutdown();
    return result;
}
