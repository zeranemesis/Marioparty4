#include "port/rollback.hpp"

#include "dolphin/os.h"
#include "libco/libco.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace {
bool gResimulating = false;

#ifdef TARGET_PC
cothread_t gSnapshotTestHost = nullptr;
cothread_t gSnapshotTestThread = nullptr;
std::uint32_t gSnapshotTestObserved = 0;

void snapshotTestCoroutine()
{
    std::uint32_t localCounter = 41;
    for (;;) {
        gSnapshotTestObserved = localCounter;
        co_switch(gSnapshotTestHost);
        ++localCounter;
    }
}
#endif
}

namespace partyboard::rollback {

bool SnapshotLayout::addRegion(void *address, std::size_t size)
{
    if (address == nullptr || size == 0 || size > SIZE_MAX - mByteSize) {
        return false;
    }
    const auto begin = reinterpret_cast<std::uintptr_t>(address);
    if (begin > UINTPTR_MAX - size) {
        return false;
    }
    const auto end = begin + size;
    for (const Region &region : mRegions) {
        const auto regionBegin = reinterpret_cast<std::uintptr_t>(region.address);
        const auto regionEnd = regionBegin + region.size;
        if (begin < regionEnd && regionBegin < end) {
            return false;
        }
    }
    mRegions.push_back({ static_cast<std::uint8_t *>(address), size });
    mByteSize += size;
    return true;
}

void SnapshotLayout::clear()
{
    mRegions.clear();
    mByteSize = 0;
}

bool SnapshotLayout::save(void *destination, std::size_t capacity) const
{
    if (destination == nullptr || capacity != mByteSize || mByteSize == 0) {
        return false;
    }
    auto *cursor = static_cast<std::uint8_t *>(destination);
    for (const Region &region : mRegions) {
        std::memcpy(cursor, region.address, region.size);
        cursor += region.size;
    }
    return true;
}

bool SnapshotLayout::load(const void *source, std::size_t size) const
{
    if (source == nullptr || size != mByteSize || mByteSize == 0) {
        return false;
    }
    const auto *cursor = static_cast<const std::uint8_t *>(source);
    for (const Region &region : mRegions) {
        std::memcpy(region.address, cursor, region.size);
        cursor += region.size;
    }
    return true;
}

std::uint32_t SnapshotLayout::checksum() const
{
    std::uint32_t hash = 2166136261u;
    for (const Region &region : mRegions) {
        for (std::size_t index = 0; index < region.size; ++index) {
            hash ^= region.address[index];
            hash *= 16777619u;
        }
    }
    return hash;
}

bool inputsEqual(const PartyBoardRollbackInput &lhs, const PartyBoardRollbackInput &rhs)
{
    return std::memcmp(&lhs, &rhs, sizeof(lhs)) == 0;
}

Session::Session(Config config, Callbacks callbacks)
    : mConfig(config)
    , mCallbacks(std::move(callbacks))
{
    mConfig.playerCount = std::clamp<std::size_t>(mConfig.playerCount, 1, kMaxPlayers);
    mConfig.maximumRollback = std::clamp<std::size_t>(mConfig.maximumRollback, 1, kHistoryFrames - 1);
    for (auto &entry : mHistory) {
        entry.snapshot.resize(mConfig.snapshotBytes);
    }
    reset();
}

void Session::reset()
{
    for (auto &entry : mHistory) {
        entry.frame = 0;
        entry.valid = false;
        entry.hasSnapshot = false;
        entry.inputs = {};
    }
    mCurrentFrame = 0;
    mEarliestMismatch = UINT32_MAX;
    mStats = {};
    mHealthy = mConfig.snapshotBytes != 0 && static_cast<bool>(mCallbacks.saveState)
        && static_cast<bool>(mCallbacks.loadState) && static_cast<bool>(mCallbacks.simulateFrame);
}

Session::FrameRecord &Session::record(std::uint32_t frame)
{
    FrameRecord &entry = mHistory[frame % kHistoryFrames];
    if (!entry.valid || entry.frame != frame) {
        entry.frame = frame;
        entry.valid = true;
        entry.hasSnapshot = false;
        entry.inputs = {};
    }
    return entry;
}

const Session::FrameRecord *Session::findRecord(std::uint32_t frame) const
{
    const FrameRecord &entry = mHistory[frame % kHistoryFrames];
    return entry.valid && entry.frame == frame ? &entry : nullptr;
}

bool Session::submitInput(std::size_t player, std::uint32_t frame, const PartyBoardRollbackInput &input)
{
    if (!mHealthy || player >= mConfig.playerCount) {
        return false;
    }

    // Keep the complete rewind window intact. A future frame that aliases an
    // old snapshot in the ring must never evict it. Subtraction after ordering
    // also avoids overflow for hostile frame numbers near UINT32_MAX.
    const auto lookahead = std::min(mConfig.maximumRollback,
        kHistoryFrames - 1 - mConfig.maximumRollback);
    if ((frame < mCurrentFrame && mCurrentFrame - frame > mConfig.maximumRollback)
        || (frame > mCurrentFrame && frame - mCurrentFrame > lookahead)) {
        return false;
    }

    FrameRecord &entry = record(frame);
    PlayerInput &slot = entry.inputs[player];
    if (slot.confirmed) {
        // Retransmission is idempotent; a confirmed input is immutable.
        return inputsEqual(slot.value, input);
    }
    if (slot.present && !inputsEqual(slot.value, input) && frame < mCurrentFrame) {
        mEarliestMismatch = std::min(mEarliestMismatch, frame);
        ++mStats.lateInputs;
    }
    slot.value = input;
    slot.present = true;
    slot.confirmed = true;
    return true;
}

PartyBoardRollbackInput Session::inputFor(std::size_t player, std::uint32_t frame, bool &predicted)
{
    FrameRecord &entry = record(frame);
    PlayerInput &slot = entry.inputs[player];
    if (slot.confirmed) {
        predicted = false;
        return slot.value;
    }

    PartyBoardRollbackInput value {};
    const std::uint32_t oldest = frame > kHistoryFrames - 1 ? frame - (kHistoryFrames - 1) : 0;
    for (std::uint32_t candidate = frame; candidate > oldest; --candidate) {
        const FrameRecord *previous = findRecord(candidate - 1);
        if (previous && previous->inputs[player].confirmed) {
            value = previous->inputs[player].value;
            break;
        }
    }
    slot.value = value;
    slot.present = true;
    predicted = true;
    return value;
}

bool Session::captureSnapshot(std::uint32_t frame)
{
    FrameRecord &entry = record(frame);
    entry.hasSnapshot = mCallbacks.saveState(entry.snapshot.data(), entry.snapshot.size());
    mHealthy = mHealthy && entry.hasSnapshot;
    return entry.hasSnapshot;
}

bool Session::restoreSnapshot(std::uint32_t frame)
{
    const FrameRecord *entry = findRecord(frame);
    if (!entry || !entry->hasSnapshot) {
        mHealthy = false;
        return false;
    }
    mHealthy = mCallbacks.loadState(entry->snapshot.data(), entry->snapshot.size());
    return mHealthy;
}

bool Session::simulate(std::uint32_t frame)
{
    std::array<PartyBoardRollbackInput, kMaxPlayers> inputs {};
    for (std::size_t player = 0; player < mConfig.playerCount; ++player) {
        bool predicted = false;
        inputs[player] = inputFor(player, frame, predicted);
        if (predicted && !gResimulating) {
            ++mStats.predictedFrames;
        }
    }
    mCallbacks.simulateFrame(frame, inputs);
    return true;
}

bool Session::reconcile()
{
    if (!mHealthy || mEarliestMismatch == UINT32_MAX || mEarliestMismatch >= mCurrentFrame) {
        return mHealthy;
    }

    const std::uint32_t firstFrame = mEarliestMismatch;
    const std::uint32_t distance = mCurrentFrame - firstFrame;
    if (distance > mConfig.maximumRollback || !restoreSnapshot(firstFrame)) {
        mHealthy = false;
        return false;
    }

    mEarliestMismatch = UINT32_MAX;
    ++mStats.rollbackCount;
    mStats.maximumRollback = std::max(mStats.maximumRollback, distance);
    gResimulating = true;
    for (std::uint32_t frame = firstFrame; frame < mCurrentFrame; ++frame) {
        if (!captureSnapshot(frame) || !simulate(frame)) {
            gResimulating = false;
            mHealthy = false;
            return false;
        }
        ++mStats.resimulatedFrames;
    }
    gResimulating = false;
    return true;
}

bool Session::advance()
{
    if (!reconcile()) {
        return false;
    }
    if (mCurrentFrame == UINT32_MAX) {
        mHealthy = false; // A new session is required before frame numbering wraps.
        return false;
    }
    if (mCurrentFrame >= mConfig.maximumRollback) {
        const auto oldest = mCurrentFrame - static_cast<std::uint32_t>(mConfig.maximumRollback);
        const FrameRecord* entry = findRecord(oldest);
        if (!entry) {
            mHealthy = false;
            return false;
        }
        for (std::size_t player = 0; player < mConfig.playerCount; ++player) {
            if (!entry->inputs[player].confirmed) {
                // Wait without advancing or becoming unhealthy. Even an input
                // equal to its prediction must be confirmed before discarding
                // the last snapshot capable of correcting that frame.
                return false;
            }
        }
    }
    if (!captureSnapshot(mCurrentFrame) || !simulate(mCurrentFrame)) {
        mHealthy = false;
        return false;
    }
    ++mCurrentFrame;
    mStats.currentFrame = mCurrentFrame;
    return true;
}

namespace {

struct TestState {
    std::int64_t position[2] {};
    std::uint32_t random = 0x13579BDFu;
    std::uint32_t checksum = 0;
};

PartyBoardRollbackInput testInput(std::uint32_t frame, std::size_t player)
{
    PartyBoardRollbackInput input {};
    input.buttons = ((frame + static_cast<std::uint32_t>(player) * 7u) % 11u == 0u) ? 0x0100u : 0u;
    input.stickX = static_cast<s8>((frame * (player == 0 ? 5u : 9u) + 17u) % 101u - 50);
    input.stickY = static_cast<s8>((frame * (player == 0 ? 3u : 7u) + 29u) % 91u - 45);
    input.triggerLeft = static_cast<u8>((frame * 13u + static_cast<std::uint32_t>(player)) & 0xFFu);
    return input;
}

Session makeTestSession(TestState &state)
{
    Callbacks callbacks;
    callbacks.saveState = [&state](void *destination, std::size_t capacity) {
        if (capacity != sizeof(state)) {
            return false;
        }
        std::memcpy(destination, &state, sizeof(state));
        return true;
    };
    callbacks.loadState = [&state](const void *source, std::size_t size) {
        if (size != sizeof(state)) {
            return false;
        }
        std::memcpy(&state, source, sizeof(state));
        return true;
    };
    callbacks.simulateFrame = [&state](std::uint32_t frame, const std::array<PartyBoardRollbackInput, kMaxPlayers> &inputs) {
        state.random = state.random * 1664525u + 1013904223u;
        for (std::size_t player = 0; player < 2; ++player) {
            const auto &input = inputs[player];
            state.position[player] += input.stickX * 3 + input.stickY * 2;
            state.position[player] += (input.buttons & 0x0100u) ? 97 : 0;
            state.position[player] -= input.triggerLeft / 8;
        }
        state.checksum ^= state.random + frame * 2654435761u
            + static_cast<std::uint32_t>(state.position[0] - state.position[1]);
    };
    return Session({ .playerCount = 2, .maximumRollback = 12, .snapshotBytes = sizeof(TestState) }, std::move(callbacks));
}

}

bool runSelfTest()
{
    constexpr std::uint32_t kFrames = 180;
    constexpr std::uint32_t kRemoteDelay = 5;
    TestState referenceState;
    TestState delayedState;
    Session reference = makeTestSession(referenceState);
    Session delayed = makeTestSession(delayedState);

    for (std::uint32_t frame = 0; frame < kFrames; ++frame) {
        const PartyBoardRollbackInput local = testInput(frame, 0);
        const PartyBoardRollbackInput remote = testInput(frame, 1);
        reference.submitInput(0, frame, local);
        reference.submitInput(1, frame, remote);
        delayed.submitInput(0, frame, local);
        if (frame >= kRemoteDelay) {
            delayed.submitInput(1, frame - kRemoteDelay, testInput(frame - kRemoteDelay, 1));
        }
        if (!reference.advance() || !delayed.advance()) {
            return false;
        }
    }
    for (std::uint32_t frame = kFrames - kRemoteDelay; frame < kFrames; ++frame) {
        delayed.submitInput(1, frame, testInput(frame, 1));
    }
    if (!delayed.reconcile()) {
        return false;
    }

    return reference.healthy() && delayed.healthy()
        && std::memcmp(&referenceState, &delayedState, sizeof(referenceState)) == 0
        && delayed.stats().rollbackCount > 0
        && delayed.stats().maximumRollback == kRemoteDelay;
}

bool runCoroutineSnapshotSelfTest()
{
#ifdef TARGET_PC
    constexpr std::size_t kStackBytes = 64 * 1024;
    if (!co_serializable()) {
        return false;
    }

    std::vector<std::uint8_t> stack(kStackBytes);
    std::vector<std::uint8_t> snapshot(kStackBytes);
    gSnapshotTestHost = co_active();
    gSnapshotTestObserved = 0;
    gSnapshotTestThread = co_derive(stack.data(), static_cast<unsigned>(stack.size()),
        snapshotTestCoroutine);
    if (gSnapshotTestThread == nullptr) {
        return false;
    }

    co_switch(gSnapshotTestThread);
    if (gSnapshotTestObserved != 41) {
        return false;
    }
    std::memcpy(snapshot.data(), stack.data(), stack.size());
    co_switch(gSnapshotTestThread);
    co_switch(gSnapshotTestThread);
    if (gSnapshotTestObserved != 43) {
        return false;
    }

    // libco's serializable backends keep the saved registers and stack pointer
    // inside this caller-owned block. Restoring it must rewind the coroutine's
    // local variable as well as its instruction pointer.
    std::memcpy(stack.data(), snapshot.data(), stack.size());
    co_switch(gSnapshotTestThread);
    const bool passed = gSnapshotTestObserved == 42;
    gSnapshotTestThread = nullptr;
    gSnapshotTestHost = nullptr;
    return passed;
#else
    return false;
#endif
}

bool runMemorySnapshotSelfTest()
{
    struct RegionA {
        std::uint32_t counter;
        std::int16_t values[7];
    } regionA { 77, { 1, 2, 3, 4, 5, 6, 7 } };
    std::array<std::uint8_t, 29> regionB {};
    for (std::size_t index = 0; index < regionB.size(); ++index) {
        regionB[index] = static_cast<std::uint8_t>(index * 9u + 3u);
    }

    SnapshotLayout layout;
    if (!layout.addRegion(&regionA, sizeof(regionA))
        || !layout.addRegion(regionB.data(), regionB.size())
        || layout.addRegion(&regionA.values[2], sizeof(regionA.values[2]))) {
        return false;
    }
    const std::uint32_t expectedChecksum = layout.checksum();
    std::vector<std::uint8_t> snapshot(layout.byteSize());
    if (!layout.save(snapshot.data(), snapshot.size())) {
        return false;
    }

    regionA.counter = 9999;
    regionA.values[4] = -123;
    regionB[11] ^= 0xffu;
    if (layout.checksum() == expectedChecksum
        || !layout.load(snapshot.data(), snapshot.size())) {
        return false;
    }
    return regionA.counter == 77 && regionA.values[4] == 5
        && regionB[11] == static_cast<std::uint8_t>(11u * 9u + 3u)
        && layout.checksum() == expectedChecksum;
}

}

extern "C" {

bool PartyBoard_RollbackRunSelfTest(void)
{
    const bool corePassed = partyboard::rollback::runSelfTest();
    const bool coroutinePassed = partyboard::rollback::runCoroutineSnapshotSelfTest();
    const bool memoryPassed = partyboard::rollback::runMemorySnapshotSelfTest();
    OSReport("Rollback core self-test: %s\n", corePassed ? "PASS" : "FAIL");
    OSReport("Rollback coroutine snapshot self-test: %s\n",
        coroutinePassed ? "PASS" : "FAIL");
    OSReport("Rollback memory snapshot self-test: %s\n",
        memoryPassed ? "PASS" : "FAIL");
    return corePassed && coroutinePassed && memoryPassed;
}

bool PartyBoard_RollbackIsResimulating(void)
{
    return gResimulating;
}

}
