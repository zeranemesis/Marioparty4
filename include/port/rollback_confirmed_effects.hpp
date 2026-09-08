#ifndef PARTYBOARD_ROLLBACK_CONFIRMED_EFFECTS_HPP
#define PARTYBOARD_ROLLBACK_CONFIRMED_EFFECTS_HPP
#include "port/rollback_effects.hpp"
#include <functional>
#include <utility>

namespace partyboard::rollback {
struct ConfirmedEffect {
    std::uint32_t frame = 0, ordinal = 0;
    EffectCommand command;
    bool operator==(const ConfirmedEffect &other) const {
        return frame == other.frame && ordinal == other.ordinal && command == other.command;
    }
};

// External effects stay outside snapshots. Replaying a frame replaces its
// unconfirmed suffix; only the corrected, contiguous confirmed prefix emits.
// No audio handles/status are fabricated here: those need an audio adapter.
// Single owner thread. A sink must return true only after accepting an event.
// A failed/throwing sink poisons this instance permanently, because emission
// might already have happened and retrying could duplicate external effects.
class ConfirmedEffects {
public:
    explicit ConfirmedEffects(std::size_t maximumFrames = 13, std::size_t maximumEvents = 1024,
        std::uint32_t firstFrame = 0)
        : mMaximumFrames(maximumFrames), mMaximumEvents(maximumEvents),
          mCommitted(firstFrame), mNextFrame(firstFrame) {}
    ConfirmedEffects(const ConfirmedEffects &) = delete;
    ConfirmedEffects &operator=(const ConfirmedEffects &) = delete;

    bool beginFrame(std::uint32_t frame) {
        if (!mHealthy || mOpen || mCommitting || frame == UINT32_MAX
            || frame < mCommitted || frame > mNextFrame) return fail();
        mPrefixEvents = 0;
        std::size_t prefixFrames = 0;
        for (const auto &stored : mFrames) {
            if (stored.frame >= frame) break;
            mPrefixEvents += stored.events.size(); ++prefixFrames;
        }
        if (prefixFrames >= mMaximumFrames) return fail();
        mPending = {}; mPending.frame = frame; mOpen = true;
        return true;
    }
    bool append(const EffectCommand &command) {
        if (!mHealthy || !mOpen || mCommitting || !command.valid()
            || mPrefixEvents + mPending.events.size() >= mMaximumEvents
            || mPending.events.size() >= UINT32_MAX) return fail();
        try {
            mPending.events.push_back({mPending.frame,
                static_cast<std::uint32_t>(mPending.events.size()), command});
        } catch (...) { return fail(); }
        return true;
    }
    bool endFrame() {
        if (!mHealthy || !mOpen || mCommitting) return fail();
        try {
            while (!mFrames.empty() && mFrames.back().frame >= mPending.frame)
                mFrames.pop_back();
            const auto next = mPending.frame + 1;
            mFrames.push_back(std::move(mPending));
            mNextFrame = next; mOpen = false;
        } catch (...) { return fail(); }
        return true;
    }
    // Pass Session::confirmedFrame() after successful reconciliation. The
    // queue validates coverage, but cannot authenticate the owner's boundary.
    bool confirmThrough(std::uint32_t exclusive,
        const std::function<bool(const ConfirmedEffect &)> &sink) {
        if (!mHealthy || mOpen || mCommitting || !sink
            || exclusive < mCommitted || exclusive > mNextFrame) return fail();
        auto expected = mCommitted;
        for (const auto &frame : mFrames) {
            if (frame.frame >= exclusive) break;
            if (frame.frame != expected++) return fail();
        }
        if (expected != exclusive) return fail();
        mCommitting = true;
        try {
            for (const auto &frame : mFrames) {
                if (frame.frame >= exclusive) break;
                for (const auto &event : frame.events)
                    if (!sink(event) || !mHealthy) { mCommitting = false; return fail(); }
            }
        } catch (...) { mCommitting = false; return fail(); }
        mCommitting = false;
        while (!mFrames.empty() && mFrames.front().frame < exclusive) mFrames.pop_front();
        mCommitted = exclusive;
        return true;
    }
    bool healthy() const { return mHealthy; }
    std::uint32_t committedFrame() const { return mCommitted; }
    std::size_t frameCount() const { return mFrames.size(); }
    std::size_t eventCount() const {
        std::size_t count = 0;
        for (const auto &frame : mFrames) count += frame.events.size();
        return count;
    }
private:
    struct Frame { std::uint32_t frame = 0; std::vector<ConfirmedEffect> events; };
    bool fail() { mHealthy = false; return false; }
    std::deque<Frame> mFrames;
    Frame mPending;
    std::size_t mMaximumFrames, mMaximumEvents, mPrefixEvents = 0;
    std::uint32_t mCommitted = 0, mNextFrame = 0;
    bool mHealthy = true, mOpen = false, mCommitting = false;
};
}
#endif
