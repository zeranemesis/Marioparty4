#include "port/rollback_io.h"
#include <cstdio>
#include <future>
#include <limits>
#include <mutex>
#include <thread>

namespace {
class IOGate {
public:
    void begin() {
        std::lock_guard lock(mutex);
        if (active == std::numeric_limits<uint64_t>::max()) fault = true;
        else ++active;
        advanceEpoch();
    }
    void end() {
        std::lock_guard lock(mutex);
        if (active == 0) fault = true;
        else --active;
        advanceEpoch();
    }
    bool acquire(uint64_t *value) {
        if (!value || !mutex.try_lock()) return false;
        if (active != 0 || fault) { mutex.unlock(); return false; }
        *value = epoch;
        return true;
    }
    void release() { mutex.unlock(); }
private:
    void advanceEpoch() {
        if (epoch == std::numeric_limits<uint64_t>::max()) fault = true;
        else ++epoch;
    }
    std::mutex mutex;
    uint64_t active = 0, epoch = 0;
    bool fault = false;
};
IOGate gate;
}

extern "C" void PartyBoard_RollbackIOBegin(void) { gate.begin(); }
extern "C" void PartyBoard_RollbackIOEnd(void) { gate.end(); }
extern "C" bool PartyBoard_RollbackIOTryCapture(uint64_t *epoch) { return gate.acquire(epoch); }
extern "C" void PartyBoard_RollbackIORelease(void) { gate.release(); }

extern "C" bool PartyBoard_RollbackIOSelfTest(void)
{
    // Isolated gate: self-tests never reset or mutate the live I/O accounting.
    IOGate test;
    uint64_t first = 0, later = 0;
    bool passed = !test.acquire(nullptr);
    bool acquired = test.acquire(&first);
    passed &= acquired;
    if (acquired) test.release();
    test.begin(); test.begin();
    acquired = test.acquire(&later);
    passed &= !acquired;
    if (acquired) test.release();
    test.end();
    acquired = test.acquire(&later);
    passed &= !acquired;
    if (acquired) test.release();
    test.end();
    acquired = test.acquire(&later);
    passed &= acquired && later > first;
    if (acquired) test.release();

    // A second thread cannot acquire a capture during another capture.
    acquired = test.acquire(&later);
    passed &= acquired;
    if (acquired) {
        std::promise<bool> result;
        auto future = result.get_future();
        std::thread contender([&] {
            uint64_t value;
            const bool owned = test.acquire(&value);
            if (owned) test.release();
            result.set_value(owned);
        });
        contender.join();
        passed &= !future.get();
        test.release();
    }
    // An unmatched completion poisons capture rather than wrapping a counter.
    test.end();
    acquired = test.acquire(&later);
    passed &= !acquired;
    if (acquired) test.release();
    std::printf("Rollback directory I/O gate: %s (nested reads, completed epochs, capture exclusion, unmatched completion).\n",
        passed ? "PASS" : "FAIL");
    return passed;
}
