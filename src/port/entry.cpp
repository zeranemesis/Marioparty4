#include <aurora/main.h>
#include "port/netplay_transport.hpp"
#include "port/netplay_runtime.h"
#include "port/rollback.h"
#include "port/coroutine_stack.h"

#include <cstring>

#if defined(_WIN32)
#include <cstdio>
#include <windows.h>
#endif

extern "C" int port_main(int argc, char* argv[]);
extern "C" bool PartyBoard_OnlineCheckDisc(void);
extern "C" bool PartyBoard_OnlineBarrierProbe(void);

int main(int argc, char *argv[])
{
    if (argc == 2 && std::strcmp(argv[1], "--coroutine-stack-self-test") == 0)
        return PartyBoard_CoroutineStackRunSelfTest() ? 0 : 1;
    if (argc == 2 && std::strcmp(argv[1], "--online-disc-check") == 0)
        return PartyBoard_OnlineCheckDisc() ? 0 : 3;
    if (argc == 2 && std::strcmp(argv[1], "--rollback-self-test") == 0)
        return PartyBoard_RollbackRunSelfTest() ? 0 : 1;
    if (argc == 2 && std::strcmp(argv[1], "--netplay-self-test") == 0)
        return PartyBoard_RollbackRunSelfTest() && PartyBoard_NetTransportRunSelfTest()
                && PartyBoard_NetplayRuntimeRunSelfTest()
                && PartyBoard_CoroutineStackRunSelfTest()
            ? 0
            : 1;

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
