#include "port/rollback_effects.hpp"
#include "port/rollback_confirmed_effects.hpp"

#include <cstdio>
#include "port/rollback_audio.hpp"
#include "../../src/port/rollback_audio_test.inc"

using namespace partyboard::rollback;

static unsigned checks;
static unsigned failures;
#define CHECK(condition) do { ++checks; if (!(condition)) { \
    ++failures; std::printf("FAIL line %d: %s\n", __LINE__, #condition); \
} } while (0)

static EffectCommand fx(std::int64_t id, std::int64_t volume = 127,
    std::int64_t pan = 64)
{
    return EffectCommand::make(EffectKind::FxPlay, { id, volume, pan });
}

static void exactReplay()
{
    EffectJournal journal;
    CHECK(journal.beginRecord(20) == EffectJournalStatus::Ready);
    CHECK(journal.record(fx(839), 77) == EffectJournalStatus::Recorded);
    CHECK(journal.record(EffectCommand::make(EffectKind::SequencePlay, { 12 }), 4)
        == EffectJournalStatus::Recorded);
    CHECK(journal.endRecord() == EffectJournalStatus::Complete);
    CHECK(journal.beginReplay(20) == EffectJournalStatus::Ready);
    const auto first = journal.replay(fx(839));
    CHECK(first.status == EffectJournalStatus::Replayed);
    CHECK(first.ordinal == 0 && first.hasReturnValue && first.returnValue == 77);
    CHECK(!first.shouldEmit);
    const auto second = journal.replay(
        EffectCommand::make(EffectKind::SequencePlay, { 12 }));
    CHECK(second.status == EffectJournalStatus::Replayed);
    CHECK(second.ordinal == 1 && second.returnValue == 4 && !second.shouldEmit);
    CHECK(journal.endReplay() == EffectJournalStatus::Complete);
}

static void divergenceNeverReusesHandle()
{
    EffectJournal journal;
    CHECK(journal.beginRecord(30) == EffectJournalStatus::Ready);
    CHECK(journal.record(fx(10, 100, 64), 91) == EffectJournalStatus::Recorded);
    CHECK(journal.endRecord() == EffectJournalStatus::Complete);
    CHECK(journal.beginReplay(30) == EffectJournalStatus::Ready);
    const auto mismatch = journal.replay(fx(10, 99, 64));
    CHECK(mismatch.status == EffectJournalStatus::Diverged);
    CHECK(!mismatch.hasReturnValue && !mismatch.shouldEmit && mismatch.returnValue == 0);
    const auto afterFailure = journal.replay(fx(10, 100, 64));
    CHECK(afterFailure.status == EffectJournalStatus::Diverged);
    CHECK(!afterFailure.hasReturnValue && afterFailure.returnValue != 91);
    CHECK(journal.endReplay() == EffectJournalStatus::Diverged);

    EffectJournal kindJournal;
    CHECK(kindJournal.beginRecord(31) == EffectJournalStatus::Ready);
    CHECK(kindJournal.record(fx(1), 52) == EffectJournalStatus::Recorded);
    CHECK(kindJournal.endRecord() == EffectJournalStatus::Complete);
    CHECK(kindJournal.beginReplay(31) == EffectJournalStatus::Ready);
    const auto wrongKind = kindJournal.replay(
        EffectCommand::make(EffectKind::StreamPlay, { 1, 127, 64 }));
    CHECK(wrongKind.status == EffectJournalStatus::Diverged);
    CHECK(!wrongKind.hasReturnValue && wrongKind.returnValue != 52);
    CHECK(kindJournal.endReplay() == EffectJournalStatus::Diverged);
}

static void missingAndUnconsumed()
{
    EffectJournal empty;
    CHECK(empty.beginRecord(40) == EffectJournalStatus::Ready);
    CHECK(empty.endRecord() == EffectJournalStatus::Complete);
    CHECK(empty.beginReplay(40) == EffectJournalStatus::Ready);
    const auto newPredictedSound = empty.replay(fx(99));
    CHECK(newPredictedSound.status == EffectJournalStatus::Missing);
    CHECK(!newPredictedSound.hasReturnValue && !newPredictedSound.shouldEmit);
    CHECK(empty.endReplay() == EffectJournalStatus::Missing);

    EffectJournal skipped;
    CHECK(skipped.beginRecord(41) == EffectJournalStatus::Ready);
    CHECK(skipped.record(fx(3), 8) == EffectJournalStatus::Recorded);
    CHECK(skipped.endRecord() == EffectJournalStatus::Complete);
    CHECK(skipped.beginReplay(41) == EffectJournalStatus::Ready);
    CHECK(skipped.endReplay() == EffectJournalStatus::Unconsumed);
    CHECK(skipped.beginReplay(42) == EffectJournalStatus::Missing);
}

static void boundedWindowAndCapacity()
{
    EffectJournal journal(2, 3);
    for (std::uint32_t frame = 100; frame <= 102; ++frame) {
        CHECK(journal.beginRecord(frame) == EffectJournalStatus::Ready);
        CHECK(journal.record(fx(frame), static_cast<std::int32_t>(frame))
            == EffectJournalStatus::Recorded);
        CHECK(journal.endRecord() == EffectJournalStatus::Complete);
    }
    CHECK(journal.eventCount() == 3 && journal.frameCount() == 3);
    CHECK(journal.beginRecord(103) == EffectJournalStatus::Ready);
    CHECK(journal.eventCount() == 2 && journal.frameCount() == 3);
    CHECK(journal.record(fx(103), 103) == EffectJournalStatus::Recorded);
    CHECK(journal.record(fx(104), 104) == EffectJournalStatus::CapacityExceeded);
    CHECK(journal.eventCount() == journal.maximumEvents());
    CHECK(journal.endRecord() == EffectJournalStatus::CapacityExceeded);
    CHECK(journal.beginReplay(103) == EffectJournalStatus::InvalidSequence);
    CHECK(journal.beginReplay(100) == EffectJournalStatus::Expired);
    CHECK(journal.beginReplay(101) == EffectJournalStatus::Ready);
    const auto boundary = journal.replay(fx(101));
    CHECK(boundary.status == EffectJournalStatus::Replayed && boundary.returnValue == 101);
    CHECK(journal.endReplay() == EffectJournalStatus::Complete);
    CHECK(journal.beginRecord(102) == EffectJournalStatus::InvalidSequence);
}

static void invalidCommands()
{
    EffectJournal journal;
    const auto tooMany = EffectCommand::make(EffectKind::FxPlay,
        { 1, 2, 3, 4, 5, 6, 7 });
    CHECK(!tooMany.valid());
    CHECK(!(tooMany == tooMany));
    CHECK(journal.beginRecord(1) == EffectJournalStatus::Ready);
    CHECK(journal.record(tooMany, 1) == EffectJournalStatus::InvalidCommand);
    CHECK(journal.record(fx(1), 99) == EffectJournalStatus::InvalidCommand);
    CHECK(journal.eventCount() == 0);
    CHECK(journal.endRecord() == EffectJournalStatus::InvalidCommand);
    CHECK(journal.beginReplay(1) == EffectJournalStatus::InvalidSequence);
    CHECK(!journal.replay(fx(1)).hasReturnValue);
    CHECK(journal.beginRecord(2) == EffectJournalStatus::Ready);
    CHECK(journal.record(fx(2), 77) == EffectJournalStatus::Recorded);
    CHECK(journal.endRecord() == EffectJournalStatus::Complete);
    CHECK(journal.beginReplay(2) == EffectJournalStatus::Ready);
    CHECK(journal.replay(fx(2)).returnValue == 77);
    CHECK(journal.endReplay() == EffectJournalStatus::Complete);

    EffectCommand badKind = fx(1);
    badKind.kind = static_cast<EffectKind>(999);
    CHECK(!badKind.valid());
}

static void confirmedEffects()
{
    ConfirmedEffects queue;
    std::vector<ConfirmedEffect> output;
    const auto sink = [&](const ConfirmedEffect &e) { output.push_back(e); return true; };
    CHECK(queue.beginFrame(0)); CHECK(queue.append(fx(10))); CHECK(queue.endFrame());
    CHECK(queue.beginFrame(1)); CHECK(queue.append(fx(20))); CHECK(queue.endFrame());
    CHECK(output.empty());
    // An empty correction removes frame 0's sound and invalidates frame 1.
    CHECK(queue.beginFrame(0)); CHECK(queue.endFrame());
    CHECK(queue.frameCount() == 1 && queue.eventCount() == 0);
    CHECK(queue.beginFrame(1)); CHECK(queue.append(fx(30))); CHECK(queue.append(fx(31))); CHECK(queue.endFrame());
    CHECK(queue.confirmThrough(1, sink)); CHECK(output.empty());
    CHECK(queue.confirmThrough(2, sink)); CHECK(output.size() == 2);
    CHECK(output[0].frame == 1 && output[0].ordinal == 0 && output[0].command == fx(30));
    CHECK(output[1].ordinal == 1 && output[1].command == fx(31));
    CHECK(queue.confirmThrough(2, sink)); CHECK(output.size() == 2);
    CHECK(!queue.beginFrame(1)); CHECK(!queue.healthy());
    CHECK(!queue.confirmThrough(2, sink)); CHECK(output.size() == 2);

    ConfirmedEffects incomplete;
    CHECK(incomplete.beginFrame(0)); CHECK(incomplete.append(fx(1)));
    CHECK(!incomplete.confirmThrough(1, sink)); CHECK(output.size() == 2);
    ConfirmedEffects hole;
    CHECK(!hole.beginFrame(1)); CHECK(!hole.healthy());
    ConfirmedEffects future;
    CHECK(future.beginFrame(0)); CHECK(future.append(fx(1))); CHECK(future.endFrame());
    CHECK(!future.confirmThrough(2, sink)); CHECK(output.size() == 2);

    ConfirmedEffects capacity(2, 1);
    CHECK(capacity.beginFrame(0)); CHECK(capacity.append(fx(1))); CHECK(capacity.endFrame());
    CHECK(capacity.beginFrame(0)); CHECK(capacity.append(fx(2))); CHECK(capacity.endFrame());
    CHECK(capacity.eventCount() == 1);
    CHECK(capacity.beginFrame(1)); CHECK(!capacity.append(fx(3)));
    CHECK(!capacity.confirmThrough(1, sink)); CHECK(output.size() == 2);
    ConfirmedEffects frames(1);
    CHECK(frames.beginFrame(0)); CHECK(frames.endFrame()); CHECK(!frames.beginFrame(1));
    ConfirmedEffects invalid;
    CHECK(invalid.beginFrame(0)); auto bad = fx(1); bad.argumentCount = 255;
    CHECK(!invalid.append(bad)); CHECK(!invalid.healthy());

    // A sink might perform its side effect before reporting failure. Never retry.
    for (bool throws : {false, true}) {
        ConfirmedEffects failure;
        CHECK(failure.beginFrame(0)); CHECK(failure.append(fx(1))); CHECK(failure.append(fx(2))); CHECK(failure.endFrame());
        unsigned attempts = 0;
        const auto brokenSink = [&](const ConfirmedEffect &) -> bool {
            ++attempts; if (throws) throw 7; return false;
        };
        CHECK(!failure.confirmThrough(1, brokenSink)); CHECK(attempts == 1);
        CHECK(!failure.confirmThrough(1, brokenSink)); CHECK(attempts == 1);
    }
    ConfirmedEffects reentrant;
    CHECK(reentrant.beginFrame(0)); CHECK(reentrant.append(fx(1))); CHECK(reentrant.endFrame());
    CHECK(!reentrant.confirmThrough(1, [&](const ConfirmedEffect &) {
        CHECK(!reentrant.beginFrame(1)); return true;
    }));
    ConfirmedEffects longRun(2, 2);
    unsigned count = 0;
    for (unsigned frame = 0; frame < 2000; ++frame) {
        CHECK(longRun.beginFrame(frame)); CHECK(longRun.append(fx(frame))); CHECK(longRun.endFrame());
        CHECK(longRun.confirmThrough(frame + 1, [&](const ConfirmedEffect &e) {
            CHECK(e.frame == count); ++count; return true;
        }));
        CHECK(longRun.frameCount() == 0 && longRun.eventCount() == 0);
    }
    CHECK(count == 2000);
}

int main()
{
    CHECK(runAudioAdapterTests(checks));
    confirmedEffects();
    exactReplay();
    divergenceNeverReusesHandle();
    missingAndUnconsumed();
    boundedWindowAndCapacity();
    invalidCommands();
    std::printf("Rollback effect journal: %u checks, %u failures. Standalone; audio integration disabled.\n",
        checks, failures);
    return failures == 0 ? 0 : 1;
}
