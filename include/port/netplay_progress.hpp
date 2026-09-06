#pragma once
#include <cstdint>

namespace partyboard::netplay {

enum class ProgressFailure { None, NoPeer, Stalled };

// Monotonic milliseconds, independent of render FPS and poll frequency.
// Duplicate traffic cannot indefinitely extend a blocked simulation.
class SessionProgress {
public:
    void start(std::uint64_t now) {
        if (!mStarted) { mStarted = true; mSince = now; }
    }
    void observePeer(std::uint64_t now) {
        if (!mPeer && mFailure == ProgressFailure::None) {
            mPeer = true; mSince = now;
        }
    }
    void commit(std::uint64_t now) {
        if (mFailure == ProgressFailure::None) mSince = now;
    }
    ProgressFailure check(std::uint64_t now) {
        if (mStarted && now >= mSince && now - mSince >= 120000u) {
            if (mFailure == ProgressFailure::None)
                mFailure = mPeer ? ProgressFailure::Stalled : ProgressFailure::NoPeer;
        }
        return mFailure;
    }
    bool connected() const { return mPeer; }
    bool waiting(std::uint64_t now) const { return mStarted && now >= mSince && now - mSince >= 5000u && mFailure == ProgressFailure::None; }
private:
    std::uint64_t mSince = 0;
    bool mStarted = false;
    bool mPeer = false;
    ProgressFailure mFailure = ProgressFailure::None;
};

inline bool progressSelfTest() {
    SessionProgress waiting;
    waiting.start(100);
    waiting.start(1000); // Repeated polls don't reset the deadline.
    if (waiting.check(120099) != ProgressFailure::None
        || waiting.check(120100) != ProgressFailure::NoPeer) return false;
    waiting.observePeer(120200);
    waiting.commit(120200);
    if (waiting.check(120200) != ProgressFailure::NoPeer) return false;
    SessionProgress playing;
    playing.start(0);
    playing.observePeer(500);
    for (unsigned i = 0; i < 10000; ++i) {
        playing.observePeer(500 + i);
        if (playing.check(500 + i) != ProgressFailure::None) return false;
    }
    if (!playing.waiting(15500) || playing.check(60500) != ProgressFailure::None) return false;
    playing.commit(60500); // Resume safely after a one-minute interruption.
    if (playing.waiting(60501) || playing.check(180499) != ProgressFailure::None
        || playing.check(180500) != ProgressFailure::Stalled) return false;
    playing.commit(180501);
    return playing.check(180502) == ProgressFailure::Stalled;
}

} // namespace partyboard::netplay
