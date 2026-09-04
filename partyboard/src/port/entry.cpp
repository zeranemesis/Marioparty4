#include <aurora/main.h>
#include "port/netplay_transport.hpp"
#include "port/netplay_runtime.h"
#include "port/rollback.h"

#include <cstring>

#if defined(_WIN32)
#include <cstdio>
#include <windows.h>
#include <DbgHelp.h>

#pragma comment(lib, "Dbghelp.lib")

namespace
{
LONG WINAPI write_crash_dump(EXCEPTION_POINTERS *exceptionPointers)
{
    const DWORD threadId = GetCurrentThreadId();
    const DWORD exceptionCode = exceptionPointers != nullptr && exceptionPointers->ExceptionRecord != nullptr
        ? exceptionPointers->ExceptionRecord->ExceptionCode
        : 0;
    const void *exceptionAddress = exceptionPointers != nullptr && exceptionPointers->ExceptionRecord != nullptr
        ? exceptionPointers->ExceptionRecord->ExceptionAddress
        : nullptr;

    if (FILE *summary = std::fopen("partyboard_crash.txt", "w"))
    {
        std::fprintf(summary, "PartyBoard unhandled exception\ncode=0x%08lX\naddress=%p\nthread=%lu\n",
            exceptionCode, exceptionAddress, threadId);
        std::fclose(summary);
    }

    HANDLE dump = CreateFileW(L"partyboard_crash.dmp", GENERIC_WRITE, FILE_SHARE_READ, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (dump != INVALID_HANDLE_VALUE)
    {
        MINIDUMP_EXCEPTION_INFORMATION exceptionInfo {};
        exceptionInfo.ThreadId = threadId;
        exceptionInfo.ExceptionPointers = exceptionPointers;
        exceptionInfo.ClientPointers = FALSE;
        const MINIDUMP_TYPE dumpType = static_cast<MINIDUMP_TYPE>(
            MiniDumpWithDataSegs | MiniDumpWithHandleData | MiniDumpWithThreadInfo);
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), dump, dumpType,
            exceptionPointers != nullptr ? &exceptionInfo : nullptr, nullptr, nullptr);
        CloseHandle(dump);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}
}
#endif

extern "C" int port_main(int argc, char* argv[]);

int main(int argc, char *argv[])
{
    if (argc == 2 && std::strcmp(argv[1], "--rollback-self-test") == 0)
        return PartyBoard_RollbackRunSelfTest() ? 0 : 1;
    if (argc == 2 && std::strcmp(argv[1], "--netplay-self-test") == 0)
        return PartyBoard_RollbackRunSelfTest() && PartyBoard_NetTransportRunSelfTest()
                && PartyBoard_NetplayRuntimeRunSelfTest()
            ? 0
            : 1;

    if (!PartyBoard_NetplayConfigureFromArgs(argc, argv))
        return 2;

#if defined(_WIN32)
    SetUnhandledExceptionFilter(write_crash_dump);
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
