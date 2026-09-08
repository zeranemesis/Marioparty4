// Shared by the standalone test executable and native self-test. No device.
#include <cstdio>
static bool runAudioAdapterTests(unsigned &checks) {
    using namespace partyboard::rollback;
    bool ok = true;
#define FX_CHECK(value) do { ++checks; if (!(value)) { \
    ok = false; std::fprintf(stderr, "Audio adapter check failed at line %d: %s\n", __LINE__, #value); \
} } while (0)
    std::vector<std::array<int, 4>> calls;
    int nextHandle = 701, statusReads = 0, liveStatus = 1;
    FxBackend backend {
        [&](int sound, int volume, int pan) { calls.push_back({0, sound, volume, pan}); return nextHandle++; },
        [&](int handle, int fade) { calls.push_back({1, handle, fade, 0}); return 0; },
        [&](int handle, FxParameter parameter, int value) {
            calls.push_back({2, handle, static_cast<int>(parameter), value}); return 0;
        },
        [&](int) { ++statusReads; return liveStatus; }
    };
    ConfirmedAudioFx audio(backend);
    FX_CHECK(audio.beginFrame(0));
    const auto wrong = audio.play(10);
    FX_CHECK(wrong); FX_CHECK(audio.endFrame()); FX_CHECK(calls.empty());
    FX_CHECK(audio.logicalState(wrong) == FxLogicalState::Playing);
    FX_CHECK(audio.observe(wrong).state == FxObservationState::AwaitConfirmation);
    FX_CHECK(statusReads == 0);
    FX_CHECK(audio.beginFrame(1)); FX_CHECK(audio.play(99)); FX_CHECK(audio.endFrame());
    FX_CHECK(audio.beginFrame(0));
    const auto corrected = audio.play(20, 110, 32);
    FX_CHECK(corrected.value == wrong.value); // Corrected logical slot, never emitted yet.
    FX_CHECK(audio.parameter(corrected, FxParameter::Volume, 75));
    FX_CHECK(audio.endFrame()); FX_CHECK(audio.voiceCount() == 1);
    FX_CHECK(audio.beginFrame(1)); FX_CHECK(audio.stop(corrected, 200)); FX_CHECK(audio.endFrame());
    FX_CHECK(audio.logicalState(corrected) == FxLogicalState::Stopped);
    FX_CHECK(calls.empty());
    FX_CHECK(audio.confirmThrough(2));
    FX_CHECK(calls.size() == 3);
    FX_CHECK((calls[0] == std::array<int, 4>{0, 20, 110, 32}));
    FX_CHECK((calls[1] == std::array<int, 4>{2, 701, 0, 75}));
    FX_CHECK((calls[2] == std::array<int, 4>{1, 701, 200, 0}));
    FX_CHECK(audio.confirmThrough(2)); FX_CHECK(calls.size() == 3);
    FX_CHECK(audio.observe(corrected).status == 1); FX_CHECK(statusReads == 1);
    FX_CHECK(!audio.retire(corrected));
    FX_CHECK(audio.beginFrame(2));
    const auto beforeRead = statusReads;
    FX_CHECK(audio.observe(corrected).state == FxObservationState::AwaitConfirmation);
    FX_CHECK(statusReads == beforeRead);
    FX_CHECK(audio.endFrame()); FX_CHECK(audio.confirmThrough(3));
    liveStatus = 0; FX_CHECK(audio.retire(corrected)); FX_CHECK(audio.voiceCount() == 0);
    FX_CHECK(audio.observe(corrected).state == FxObservationState::UnknownTicket);
    FX_CHECK(audio.logicalState(corrected) == FxLogicalState::Unknown);

    // Removing a speculative play must not call play OR stop on the device.
    const auto beforeRemoval = calls.size();
    FX_CHECK(audio.beginFrame(3)); const auto removed = audio.play(30); FX_CHECK(removed);
    FX_CHECK(audio.endFrame()); FX_CHECK(audio.beginFrame(3)); FX_CHECK(audio.endFrame());
    FX_CHECK(audio.confirmThrough(4)); FX_CHECK(calls.size() == beforeRemoval);
    FX_CHECK(audio.observe(removed).state == FxObservationState::UnknownTicket);

    // Rewinding the frame that contained a stop restores the logical playing
    // state even though no device command was ever emitted for that old path.
    ConfirmedAudioFx correctedStop(backend);
    FX_CHECK(correctedStop.beginFrame(0)); const auto correctedStopTicket = correctedStop.play(1);
    FX_CHECK(correctedStop.endFrame());
    FX_CHECK(correctedStop.beginFrame(1)); FX_CHECK(correctedStop.stop(correctedStopTicket));
    FX_CHECK(correctedStop.endFrame());
    FX_CHECK(correctedStop.logicalState(correctedStopTicket) == FxLogicalState::Stopped);
    FX_CHECK(correctedStop.beginFrame(1)); FX_CHECK(correctedStop.endFrame());
    FX_CHECK(correctedStop.logicalState(correctedStopTicket) == FxLogicalState::Playing);

    FxBackend failedPlay = backend;
    int failedCalls = 0;
    failedPlay.play = [&](int, int, int) { ++failedCalls; return -110; };
    ConfirmedAudioFx failed(failedPlay);
    FX_CHECK(failed.beginFrame(0)); const auto unavailable = failed.play(1); FX_CHECK(unavailable);
    FX_CHECK(failed.stop(unavailable)); FX_CHECK(failed.parameter(unavailable, FxParameter::Pan, 64));
    FX_CHECK(failed.endFrame()); FX_CHECK(failed.confirmThrough(1));
    FX_CHECK(failedCalls == 1); FX_CHECK(calls.size() == beforeRemoval);
    const auto unavailableResult = failed.observe(unavailable);
    FX_CHECK(unavailableResult.state == FxObservationState::Ready && unavailableResult.error == -110);
    FX_CHECK(failed.retire(unavailable)); FX_CHECK(failed.voiceCount() == 0);

    ConfirmedAudioFx bounded(backend, 1);
    FX_CHECK(bounded.beginFrame(0)); FX_CHECK(bounded.play(1));
    FX_CHECK(!bounded.play(2)); FX_CHECK(!bounded.endFrame()); FX_CHECK(!bounded.confirmThrough(1));
    FX_CHECK(calls.size() == beforeRemoval);
    ConfirmedAudioFx invalid(backend);
    FX_CHECK(invalid.beginFrame(0)); FX_CHECK(!invalid.stop(FxTicket{1234})); FX_CHECK(!invalid.healthy());
    ConfirmedAudioFx range(backend);
    FX_CHECK(range.beginFrame(0)); const auto rangeTicket = range.play(1);
    FX_CHECK(!range.parameter(rangeTicket, static_cast<FxParameter>(99), 0));
    FX_CHECK(!range.confirmThrough(1)); FX_CHECK(calls.size() == beforeRemoval);

    int thrownCalls = 0;
    auto throwing = backend;
    throwing.play = [&](int, int, int) -> int { ++thrownCalls; throw 7; };
    ConfirmedAudioFx fault(throwing);
    FX_CHECK(fault.beginFrame(0)); FX_CHECK(fault.play(1)); FX_CHECK(fault.endFrame());
    FX_CHECK(!fault.confirmThrough(1)); FX_CHECK(!fault.confirmThrough(1)); FX_CHECK(thrownCalls == 1);

    ConfirmedAudioFx pendingReference(backend);
    FX_CHECK(pendingReference.beginFrame(0)); const auto stillReferenced = pendingReference.play(1);
    FX_CHECK(pendingReference.endFrame()); FX_CHECK(pendingReference.confirmThrough(1));
    FX_CHECK(pendingReference.beginFrame(1)); FX_CHECK(pendingReference.stop(stillReferenced));
    FX_CHECK(pendingReference.endFrame()); FX_CHECK(!pendingReference.retire(stillReferenced));
    FX_CHECK(pendingReference.confirmThrough(2)); FX_CHECK(pendingReference.retire(stillReferenced));

    ConfirmedAudioFx cleanup(backend);
    FX_CHECK(cleanup.beginFrame(0)); FX_CHECK(cleanup.play(1));
    FX_CHECK(cleanup.endFrame()); FX_CHECK(cleanup.confirmThrough(1));
    const auto beforeClose = calls.size();
    FX_CHECK(cleanup.beginFrame(1)); FX_CHECK(cleanup.play(2));
    FX_CHECK(cleanup.close()); FX_CHECK(calls.size() == beforeClose + 1);
    FX_CHECK(calls.back()[0] == 1); // Stop committed voice; never play prediction.
    FX_CHECK(cleanup.close()); FX_CHECK(calls.size() == beforeClose + 1);
    FX_CHECK(!cleanup.beginFrame(2));

    ConfirmedAudioFx *reentrantPtr = nullptr;
    auto reentrantBackend = backend;
    reentrantBackend.play = [&](int, int, int) {
        FX_CHECK(!reentrantPtr->beginFrame(1)); return 888;
    };
    ConfirmedAudioFx reentrant(reentrantBackend); reentrantPtr = &reentrant;
    FX_CHECK(reentrant.beginFrame(0)); FX_CHECK(reentrant.play(1)); FX_CHECK(reentrant.endFrame());
    FX_CHECK(!reentrant.confirmThrough(1)); FX_CHECK(!reentrant.healthy());
    FX_CHECK(reentrant.close()); FX_CHECK(calls.back()[1] == 888);

    // Explicit completion/retirement keeps a long session's mapping bounded.
    ConfirmedAudioFx longRun(backend, 2);
    for (unsigned frame = 0; frame < 1000; ++frame) {
        FX_CHECK(longRun.beginFrame(frame)); const auto ticket = longRun.play(40); FX_CHECK(ticket);
        FX_CHECK(longRun.endFrame()); FX_CHECK(longRun.confirmThrough(frame + 1));
        FX_CHECK(longRun.retire(ticket)); FX_CHECK(longRun.voiceCount() == 0);
    }
    // A live rollback window starts after lockstep, including after transitions.
    // Replaying its first frame must replace predictions and emit only once.
    for (const unsigned base : {2u, 12000u}) {
        const auto beforeWindow = calls.size();
        ConfirmedAudioFx window(backend, 512, base);
        FX_CHECK(window.confirmThrough(base));
        FX_CHECK(window.beginFrame(base));
        const auto predicted = window.play(10); FX_CHECK(predicted);
        FX_CHECK(window.endFrame());
        FX_CHECK(window.beginFrame(base));
        const auto correctedWindow = window.play(20); FX_CHECK(correctedWindow);
        FX_CHECK(correctedWindow.value == predicted.value);
        FX_CHECK(correctedWindow.value == ((base + 1) << 8));
        FX_CHECK(window.endFrame());
        FX_CHECK(calls.size() == beforeWindow);
        FX_CHECK(window.confirmThrough(base + 1));
        FX_CHECK(calls.size() == beforeWindow + 1 && calls.back()[1] == 20);
        FX_CHECK(window.confirmThrough(base + 1));
        FX_CHECK(calls.size() == beforeWindow + 1);
        FX_CHECK(window.beginFrame(base + 1));
        FX_CHECK(window.stop(correctedWindow)); FX_CHECK(window.endFrame());
        FX_CHECK(window.confirmThrough(base + 2)); FX_CHECK(window.close());

        ConfirmedAudioFx past(backend, 512, base);
        FX_CHECK(!past.beginFrame(base - 1));
        ConfirmedAudioFx gap(backend, 512, base);
        FX_CHECK(!gap.beginFrame(base + 1));
        ConfirmedAudioFx premature(backend, 512, base);
        FX_CHECK(!premature.confirmThrough(base + 1));
    }
#undef FX_CHECK
    return ok;
}
