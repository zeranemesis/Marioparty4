#ifndef PARTYBOARD_PORT_ROLLBACK_EFFECTS_HPP
#define PARTYBOARD_PORT_ROLLBACK_EFFECTS_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <initializer_list>
#include <vector>

namespace partyboard::rollback {

enum class EffectKind : std::uint16_t {
    FxPlay,
    FxStop,
    FxParameter,
    SequencePlay,
    SequenceStop,
    StreamPlay,
    StreamStop,
};

struct EffectCommand {
    static constexpr std::size_t kMaximumArguments = 6;
    EffectKind kind = EffectKind::FxPlay;
    std::uint8_t argumentCount = 0;
    std::array<std::int64_t, kMaximumArguments> arguments {};

    static EffectCommand make(EffectKind kind, std::initializer_list<std::int64_t> arguments)
    {
        EffectCommand command;
        command.kind = kind;
        if (arguments.size() > kMaximumArguments) {
            command.argumentCount = 0xff;
            return command;
        }
        command.argumentCount = static_cast<std::uint8_t>(arguments.size());
        std::size_t index = 0;
        for (const auto argument : arguments) command.arguments[index++] = argument;
        return command;
    }

    bool valid() const
    {
        return argumentCount <= kMaximumArguments
            && static_cast<std::uint16_t>(kind)
                <= static_cast<std::uint16_t>(EffectKind::StreamStop);
    }

    bool operator==(const EffectCommand &other) const
    {
        if (!valid() || !other.valid()) return false;
        if (kind != other.kind || argumentCount != other.argumentCount) return false;
        for (std::size_t index = 0; index < argumentCount; ++index) {
            if (arguments[index] != other.arguments[index]) return false;
        }
        return true;
    }
};

enum class EffectJournalStatus {
    Ready,
    Recorded,
    Replayed,
    Complete,
    Missing,
    Diverged,
    Unconsumed,
    Expired,
    CapacityExceeded,
    InvalidCommand,
    InvalidSequence,
};

struct EffectReplayResult {
    EffectJournalStatus status = EffectJournalStatus::InvalidSequence;
    std::int32_t returnValue = 0;
    std::uint32_t ordinal = 0;
    bool hasReturnValue = false;
    bool shouldEmit = false;
};

class EffectJournal {
public:
    explicit EffectJournal(std::uint32_t maximumPastFrames = 12,
        std::size_t maximumEvents = 1024)
        : mMaximumPastFrames(maximumPastFrames)
        , mMaximumEvents(maximumEvents == 0 ? 1 : maximumEvents)
    {
    }

    EffectJournalStatus beginRecord(std::uint32_t frame)
    {
        if (mMode != Mode::Idle || (mHasLatestFrame && frame <= mLatestFrame)) {
            return mLastStatus = EffectJournalStatus::InvalidSequence;
        }
        prune(frame);
        mFrames.push_back({ frame, {} });
        mLatestFrame = frame;
        mHasLatestFrame = true;
        mActiveFrame = frame;
        mOrdinal = 0;
        mMode = Mode::Record;
        return mLastStatus = EffectJournalStatus::Ready;
    }

    EffectJournalStatus record(const EffectCommand &command, std::int32_t returnValue)
    {
        if (mMode != Mode::Record || mFrames.empty()
            || mFrames.back().frame != mActiveFrame) {
            return mLastStatus = EffectJournalStatus::InvalidSequence;
        }
        if (mRecordFailed) return mLastStatus;
        if (!command.valid()) {
            mRecordFailed = true;
            return mLastStatus = EffectJournalStatus::InvalidCommand;
        }
        if (mEventCount >= mMaximumEvents) {
            mRecordFailed = true;
            return mLastStatus = EffectJournalStatus::CapacityExceeded;
        }
        mFrames.back().events.push_back({ mActiveFrame, mOrdinal++, command, returnValue });
        ++mEventCount;
        return mLastStatus = EffectJournalStatus::Recorded;
    }

    EffectJournalStatus endRecord()
    {
        if (mMode != Mode::Record) {
            return mLastStatus = EffectJournalStatus::InvalidSequence;
        }
        mMode = Mode::Idle;
        if (mRecordFailed) return mLastStatus;
        mFrames.back().complete = true;
        return mLastStatus = EffectJournalStatus::Complete;
    }

    EffectJournalStatus beginReplay(std::uint32_t frame)
    {
        if (mMode != Mode::Idle) {
            return mLastStatus = EffectJournalStatus::InvalidSequence;
        }
        mReplayFrame = nullptr;
        for (const FrameRecord &candidate : mFrames) {
            if (candidate.frame == frame) {
                mReplayFrame = &candidate;
                break;
            }
        }
        if (mReplayFrame == nullptr) {
            if (mHasLatestFrame && frame < mLatestFrame
                && mLatestFrame - frame > mMaximumPastFrames) {
                return mLastStatus = EffectJournalStatus::Expired;
            }
            return mLastStatus = EffectJournalStatus::Missing;
        }
        if (!mReplayFrame->complete) {
            mReplayFrame = nullptr;
            return mLastStatus = EffectJournalStatus::InvalidSequence;
        }
        mActiveFrame = frame;
        mOrdinal = 0;
        mReplayFailed = false;
        mMode = Mode::Replay;
        return mLastStatus = EffectJournalStatus::Ready;
    }

    EffectReplayResult replay(const EffectCommand &command)
    {
        EffectReplayResult result;
        result.ordinal = mOrdinal;
        if (mMode != Mode::Replay || mReplayFrame == nullptr) {
            result.status = mLastStatus = EffectJournalStatus::InvalidSequence;
            return result;
        }
        if (mReplayFailed) {
            result.status = mLastStatus;
            return result;
        }
        if (!command.valid()) {
            mReplayFailed = true;
            result.status = mLastStatus = EffectJournalStatus::InvalidCommand;
            return result;
        }
        if (mOrdinal >= mReplayFrame->events.size()) {
            mReplayFailed = true;
            result.status = mLastStatus = EffectJournalStatus::Missing;
            return result;
        }
        const Event &event = mReplayFrame->events[mOrdinal];
        if (event.frame != mActiveFrame || event.ordinal != mOrdinal
            || !(event.command == command)) {
            mReplayFailed = true;
            result.status = mLastStatus = EffectJournalStatus::Diverged;
            return result;
        }
        ++mOrdinal;
        result.status = mLastStatus = EffectJournalStatus::Replayed;
        result.returnValue = event.returnValue;
        result.hasReturnValue = true;
        result.shouldEmit = false;
        return result;
    }

    EffectJournalStatus endReplay()
    {
        if (mMode != Mode::Replay || mReplayFrame == nullptr) {
            return mLastStatus = EffectJournalStatus::InvalidSequence;
        }
        if (!mReplayFailed && mOrdinal != mReplayFrame->events.size()) {
            mLastStatus = EffectJournalStatus::Unconsumed;
        } else if (!mReplayFailed) {
            mLastStatus = EffectJournalStatus::Complete;
        }
        mMode = Mode::Idle;
        mReplayFrame = nullptr;
        return mLastStatus;
    }

    std::size_t eventCount() const { return mEventCount; }
    std::size_t frameCount() const { return mFrames.size(); }
    std::size_t maximumEvents() const { return mMaximumEvents; }
    EffectJournalStatus lastStatus() const { return mLastStatus; }

private:
    struct Event {
        std::uint32_t frame;
        std::uint32_t ordinal;
        EffectCommand command;
        std::int32_t returnValue;
    };
    struct FrameRecord {
        std::uint32_t frame;
        std::vector<Event> events;
        bool complete = false;
    };
    enum class Mode { Idle, Record, Replay };

    void prune(std::uint32_t newestFrame)
    {
        while (!mFrames.empty() && newestFrame > mFrames.front().frame
            && newestFrame - mFrames.front().frame > mMaximumPastFrames) {
            mEventCount -= mFrames.front().events.size();
            mFrames.pop_front();
        }
        mRecordFailed = false;
    }

    std::deque<FrameRecord> mFrames;
    const FrameRecord *mReplayFrame = nullptr;
    std::size_t mEventCount = 0;
    std::size_t mMaximumEvents;
    std::uint32_t mMaximumPastFrames;
    std::uint32_t mLatestFrame = 0;
    std::uint32_t mActiveFrame = 0;
    std::uint32_t mOrdinal = 0;
    EffectJournalStatus mLastStatus = EffectJournalStatus::Ready;
    Mode mMode = Mode::Idle;
    bool mHasLatestFrame = false;
    bool mRecordFailed = false;
    bool mReplayFailed = false;
};

} // namespace partyboard::rollback

#endif
