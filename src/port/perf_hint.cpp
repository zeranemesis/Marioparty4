#include "port/perf_hint.h"

#ifdef __ANDROID__
#include "port/netplay_runtime.h"
#include "port/settings.h"

#include <android/log.h>
#include <dlfcn.h>
#include <time.h>
#include <unistd.h>

#include <cstdint>

extern "C" int PartyBoard_TargetFrameRateFor(bool netplayEnabled, int configured);

namespace {

// <android/performance_hint.h>, API 33. Looked up at run time: the app still runs on Android 9.
struct APerformanceHintManager;
struct APerformanceHintSession;
using GetManager = APerformanceHintManager *(*)();
using CreateSession = APerformanceHintSession *(*)(APerformanceHintManager *, const int32_t *, size_t, int64_t);
using UpdateTarget = int (*)(APerformanceHintSession *, int64_t);
using ReportActual = int (*)(APerformanceHintSession *, int64_t);

struct Hints {
    bool tried = false;
    APerformanceHintSession *session = nullptr;
    UpdateTarget updateTarget = nullptr;
    ReportActual reportActual = nullptr;
    int64_t targetNs = 0;
    int64_t frameStart = 0;
} gHints;

int64_t now_ns()
{
    timespec ts {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

int64_t target_ns()
{
    const int fps = PartyBoard_TargetFrameRateFor(PartyBoard_NetplayEnabled(),
        partyboard::getSettings().video.targetFrameRate.getValue());
    return 1000000000LL / (fps > 0 ? fps : 60);
}

void open_session()
{
    gHints.tried = true;
    void *android = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
    if (android == nullptr) {
        return;
    }
    auto getManager = reinterpret_cast<GetManager>(dlsym(android, "APerformanceHint_getManager"));
    auto createSession = reinterpret_cast<CreateSession>(dlsym(android, "APerformanceHint_createSession"));
    gHints.updateTarget = reinterpret_cast<UpdateTarget>(dlsym(android, "APerformanceHint_updateTargetWorkDuration"));
    gHints.reportActual = reinterpret_cast<ReportActual>(dlsym(android, "APerformanceHint_reportActualWorkDuration"));
    APerformanceHintManager *manager = getManager != nullptr ? getManager() : nullptr;
    if (manager == nullptr || createSession == nullptr || gHints.updateTarget == nullptr || gHints.reportActual == nullptr) {
        return;
    }
    // The game thread: aurora::gx::fifo translation runs here and is what bounds a frame.
    const int32_t tid = static_cast<int32_t>(gettid());
    gHints.targetNs = target_ns();
    gHints.session = createSession(manager, &tid, 1, gHints.targetNs);
    __android_log_print(ANDROID_LOG_INFO, "PartyBoard", "Performance hints %s (target %.2f ms)",
        gHints.session != nullptr ? "on" : "unavailable", static_cast<double>(gHints.targetNs) / 1e6);
}

} // namespace
#endif

extern "C" void PartyBoard_PerfFrameBegin(void)
{
#ifdef __ANDROID__
    gHints.frameStart = now_ns();
#endif
}

extern "C" void PartyBoard_PerfFrameEnd(void)
{
#ifdef __ANDROID__
    if (!gHints.tried) {
        open_session();
    }
    if (gHints.session == nullptr || gHints.frameStart == 0) {
        return;
    }
    const int64_t target = target_ns();
    if (target != gHints.targetNs) {
        gHints.targetNs = target;
        gHints.updateTarget(gHints.session, target);
    }
    const int64_t work = now_ns() - gHints.frameStart;
    if (work > 0) {
        gHints.reportActual(gHints.session, work);
    }
    gHints.frameStart = 0;
#endif
}
