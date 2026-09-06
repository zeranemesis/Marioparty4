// A mixed native replay test, not a graphical minigame or Internet test.
#include "port/rollback_confirmed_effects.hpp"
#include "port/rollback_audio.hpp"
#include <stdexcept>
extern "C" {
#include "game/pad.h"
#include "game/gamework.h"
#include "game/flag.h"
#include "game/frand.h"
#include "game/minigame_seq.h"
#include "game/printfunc.h"
#include "game/wipe.h"
extern u32 GlobalCounter;
extern s32 rand8(void);
bool PartyBoard_CheckpointWithFixtureTasks(void *, bool (*)(void *, u32 *), void *);
void PartyBoard_CheckpointSequenceFixture(void);
}

namespace {
bool checkpointStaticRegions(PartyBoardRollbackRegionSink sink, void *context) {
    return PartyBoard_RollbackSceneRegions(sink, context)
        && PartyBoard_RollbackPadRegions(sink, context)
        && PartyBoard_RollbackGameRegions(sink, context)
        && PartyBoard_RollbackSequenceRegions(sink, context)
        && PartyBoard_RollbackClockRegions(sink, context);
}
struct CheckpointFixture {
    HeapSet heaps;
    u32 *moduleState;
    u32 corrections = 0, replayed = 0;
    size_t bytes = 0;
};
bool runCheckpointFixture(void *context, u32 *taskState) {
    auto &fixture = *static_cast<CheckpointFixture *>(context);
    try {
        Builder builder;
        if (!builder.build(&fixture.heaps, true)) return false;
        const auto size = fixture.bytes = sizeof(Header) + builder.layout.byteSize();
        std::vector<u8> initial(size), observed(size), beforeFailure(size);
        // Compare logical state/heap descriptors separately from unused bytes
        // in suspended machine stacks. Replay still restores the full stacks.
        Builder logical;
        for (const auto &heap : fixture.heaps)
            if (!Builder::add(&logical, heap.address, heap.bytes)) return false;
        if (!checkpointStaticRegions(Builder::add, &logical)
            || !PartyBoard_RollbackModuleRegions(Builder::add, &logical)) return false;
        std::vector<u8> expected(logical.layout.byteSize()), actual(expected.size());
        bool ok = saveScene(initial.data(), size, &fixture.heaps, true);
        // A predicted transition may activate a wipe. New captures are then
        // unsafe, but loading the preceding safe boundary must still work.
        const u8 initialWipeMode = wipeData.mode;
        wipeData.mode = WIPE_MODE_IN;
        ok &= PartyBoard_RollbackCheckpointSize() == 0;
        ok &= loadScene(initial.data(), size, &fixture.heaps, true);
        ok &= wipeData.mode == initialWipeMode;
        ConfirmedEffects referenceEffects;
        ConfirmedEffects *effects = &referenceEffects;
        std::vector<ConfirmedEffect> referenceEmitted, delayedEmitted;
        auto *emitted = &referenceEmitted;
        const auto emit = [&](const ConfirmedEffect &event) {
            emitted->push_back(event); return true;
        };
        std::vector<std::array<int, 4>> audioReference, audioDelayed;
        auto *audioCalls = &audioReference;
        int nextNative = 100, statusReads = 0;
        FxBackend backend {
            [&](int sound, int volume, int pan) {
                audioCalls->push_back({0, sound, volume, pan}); return nextNative++;
            },
            [&](int handle, int fade) { audioCalls->push_back({1, handle, fade, 0}); return 0; },
            [&](int handle, FxParameter kind, int value) {
                audioCalls->push_back({2, handle, static_cast<int>(kind), value}); return 0;
            },
            [&](int) { ++statusReads; return 1; }
        };
        ConfirmedAudioFx referenceAudio(backend);
        ConfirmedAudioFx *audio = &referenceAudio;
        Callbacks callbacks;
        callbacks.saveState = [&](void *p, size_t n) { return saveScene(p, n, &fixture.heaps, true); };
        callbacks.loadState = [&](const void *p, size_t n) { return loadScene(p, n, &fixture.heaps, true); };
        callbacks.simulateFrame = [&](u32 frame, const auto &inputs) {
            if (!effects->beginFrame(frame)) throw std::runtime_error("effect frame rejected");
            if (!audio->beginFrame(frame)) throw std::runtime_error("audio frame rejected");
            ok &= PartyBoard_RollbackApplyPads(inputs.data(), 3);
            const auto value = (inputs[1].buttons >> 8) & 3;
            taskState[0] = HuPadBtn[1] + (frand() & 31) + rand8();
            HuPrcCall(1);
            // The overlay owns a pointer into DATA: both that pointer and the
            // native allocator must rewind together with the coroutine state.
            auto **resource = reinterpret_cast<void **>(fixture.moduleState + 2);
            if (*resource) HuMemMemoryFree(*resource, 0);
            const size_t bytes = 32 + 16 * value;
            *resource = HuMemMemoryAlloc(fixture.heaps[1].address, bytes, 0);
            if (!*resource) throw std::bad_alloc();
            std::memset(*resource, static_cast<int>(taskState[1] + frame), bytes);
            GWPlayer[1].coins = static_cast<s16>((GWPlayer[1].coins + ((HuPadBtnDown[1] >> 8) & 3)) % 999);
            if (value & 1) _SetFlag(FLAG_ID_MAKE(0, 3));
            else _ClearFlag(FLAG_ID_MAKE(0, 3));
            ++GlobalCounter;
            Hu3DPauseF = (value & 2) != 0;
            minimumVcount = (value & 1) ? 2 : 1;
            minimumVcountf = static_cast<float>(minimumVcount);
            MGSeqMain();
            *fixture.moduleState = *fixture.moduleState * 17u + taskState[1] + taskState[2];
            Center.x += static_cast<float>(taskState[1] & 7);
            Center.z = static_cast<float>(*fixture.moduleState & 31);
            CZoom = 100; CRot = {}; omOutView(nullptr);
            wipeData.time += 0.25f + static_cast<float>(value);
            wipeData.duration = 60.0f;
            wipeData.stat = static_cast<u8>(frame & 1);
            wipeFadeInF = (value & 1) != 0;
            pfClsScr();
            fontcolor = static_cast<int>((frame + value) & 15);
            char rollbackText[] = "rollback %u %u";
            ok &= print8(12, 18, 1.0f, rollbackText, frame, value) >= 0;
            // Input edges add/remove commands on corrections. Arguments also
            // depend on native RNG/tasks, so stale predicted effects differ.
            if (HuPadBtnDown[1] & PAD_BUTTON_A)
                ok &= effects->append(EffectCommand::make(EffectKind::FxPlay,
                    {839, static_cast<s64>(taskState[1] & 127), 64}));
            if (value & 2)
                ok &= effects->append(EffectCommand::make(EffectKind::FxParameter,
                    {1, static_cast<s64>(taskState[2] & 127)}));
            if (frame % 17 == 0)
                ok &= effects->append(EffectCommand::make(EffectKind::FxStop, {1}));
            // This ticket lives in the captured module, while native handles
            // stay outside snapshots in the adapter. Stop/update after rewind
            // must still target the originally confirmed device voice.
            auto &ticketValue = fixture.moduleState[4];
            if (HuPadBtnDown[1] & PAD_BUTTON_A)
                ticketValue = audio->play(839, static_cast<int>(taskState[1] & 127), 64).value;
            if (ticketValue && (value & 2))
                ok &= audio->parameter(FxTicket{ticketValue}, FxParameter::Volume, taskState[2] & 127);
            if (ticketValue && inputs[1].buttons == 0) {
                ok &= audio->stop(FxTicket{ticketValue}); ticketValue = 0;
            }
            if (!audio->endFrame()) throw std::runtime_error("audio frame incomplete");
            if (!effects->endFrame()) throw std::runtime_error("effect frame incomplete");
        };
        const auto sample = [](u32 frame) {
            PartyBoardRollbackInput input {};
            input.buttons = static_cast<u16>(((frame / 5) % 4) << 8);
            return input;
        };
        constexpr u32 count = 240;
        Session reference({2, 12, size}, callbacks);
        for (u32 frame = 0; frame < count; ++frame) {
            ok &= reference.submitInput(0, frame, {}) && reference.submitInput(1, frame, sample(frame));
            ok &= reference.advance();
            ok &= reference.confirmedFrame() == frame + 1;
            ok &= effects->confirmThrough(reference.confirmedFrame(), emit);
            ok &= audio->confirmThrough(reference.confirmedFrame());
        }
        // Nonzero expected counts make an accidentally unexercised branch fail.
        ok &= referenceEmitted.size() == 159 && audioReference.size() == 155;
        ok &= logical.layout.save(expected.data(), expected.size());
        for (u32 delay : {3u, 12u}) {
            ConfirmedAudioFx delayedAudio(backend);
            audio = &delayedAudio;
            audioCalls = &audioDelayed; audioDelayed.clear(); nextNative = 100;
            ConfirmedEffects delayedEffects;
            effects = &delayedEffects;
            emitted = &delayedEmitted; delayedEmitted.clear();
            const u64 interpolationBeforeLoad = PartyBoard_FrameInterpolationEpoch();
            ok &= loadScene(initial.data(), size, &fixture.heaps, true);
            ok &= PartyBoard_FrameInterpolationEpoch() == interpolationBeforeLoad + 1;
            Session delayed({2, 12, size}, callbacks);
            for (u32 frame = 0; frame < count; ++frame) {
                ok &= delayed.submitInput(0, frame, {});
                if (frame >= delay) ok &= delayed.submitInput(1, frame - delay, sample(frame - delay));
                ok &= delayed.advance();
                ok &= delayed.confirmedFrame() == (frame >= delay ? frame - delay + 1 : 0);
                ok &= effects->confirmThrough(delayed.confirmedFrame(), emit);
                ok &= audio->confirmThrough(delayed.confirmedFrame());
                if (frame < delay) ok &= delayedEmitted.empty();
                if (frame < delay) ok &= audioDelayed.empty();
            }
            const auto beforeCorrection = delayed.confirmedFrame();
            for (u32 frame = count - delay; frame < count; ++frame)
                ok &= delayed.submitInput(1, frame, sample(frame));
            ok &= delayed.confirmedFrame() == beforeCorrection;
            ok &= delayed.reconcile() && delayed.healthy();
            ok &= delayed.confirmedFrame() == count;
            ok &= effects->confirmThrough(delayed.confirmedFrame(), emit);
            ok &= audio->confirmThrough(delayed.confirmedFrame());
            ok &= audioDelayed == audioReference && statusReads == 0;
            ok &= delayedEmitted == referenceEmitted;
            ok &= effects->frameCount() == 0 && effects->eventCount() == 0;
            ok &= logical.layout.save(actual.data(), actual.size()) && actual == expected;
            ok &= delayed.stats().rollbackCount > 0;
            fixture.corrections += delayed.stats().rollbackCount;
            fixture.replayed += delayed.stats().resimulatedFrames;
        }
        OSReport("Native corrected effects: %s (%zu confirmed commands per timeline, delays 3/12, identical ordered sink output, none emitted speculatively). Test sink, not speaker playback.\n",
            ok ? "PASS" : "FAIL", referenceEmitted.size());
        OSReport("Native corrected audio handles: %s (%zu device commands per timeline, delays 3/12, identical play/parameter/stop calls and native handles, zero device status reads). Mock audio backend.\n",
            ok ? "PASS" : "FAIL", audioReference.size());
        ok &= saveScene(beforeFailure.data(), size, &fixture.heaps, true);
        const u64 interpolationBeforeFailure = PartyBoard_FrameInterpolationEpoch();
        auto damaged = initial;
        // Last byte is module data. A late failure must leave even the first
        // SYSTEM bytes, as well as both stacks and all globals, unchanged.
        damaged.back() ^= 1;
        ok &= !loadScene(damaged.data(), size, &fixture.heaps, true);
        ok &= !loadScene(initial.data(), size - 1, &fixture.heaps, true);
        for (auto member : {&Header::processes, &Header::modules, &Header::io}) {
            damaged = initial; Header h;
            std::memcpy(&h, damaged.data(), sizeof(h)); h.*member ^= 1;
            std::memcpy(damaged.data(), &h, sizeof(h));
            ok &= !loadScene(damaged.data(), size, &fixture.heaps, true);
        }
        ok &= !loadScene(initial.data(), size, &fixture.heaps, false);
        ok &= !loadScene(fixture.heaps[0].address, size, &fixture.heaps, true);
        ok &= !saveScene(fixture.heaps[0].address, size, &fixture.heaps, true);
        const auto module = omDLLinfoTbl[0]->hModule;
        omDLLinfoTbl[0]->hModule = nullptr;
        ok &= !loadScene(initial.data(), size, &fixture.heaps, true);
        omDLLinfoTbl[0]->hModule = module;
        ok &= PartyBoard_FrameInterpolationEpoch() == interpolationBeforeFailure;
        ok &= saveScene(observed.data(), size, &fixture.heaps, true) && observed == beforeFailure;
        PartyBoard_RollbackIOBegin();
        ok &= !loadScene(initial.data(), size, &fixture.heaps, true);
        ok &= !saveScene(observed.data(), size, &fixture.heaps, true);
        PartyBoard_RollbackIOEnd();
        ok &= observed == beforeFailure;
        ok &= !loadScene(initial.data(), size, &fixture.heaps, true);
        ok &= PartyBoard_FrameInterpolationEpoch() == interpolationBeforeFailure;
        ok &= saveScene(observed.data(), size, &fixture.heaps, true);
        // The completed activity changed the epoch, never any captured bytes.
        ok &= std::memcmp(observed.data() + sizeof(Header), beforeFailure.data() + sizeof(Header),
            size - sizeof(Header)) == 0;
        return ok;
    } catch (...) { return false; }
}
}

extern "C" bool PartyBoard_RollbackCheckpointSelfTest(void) {
#ifdef _WIN32
    if (HuPrcCurrentGet() || omDLLinfoTbl[0]) return false;
    Builder globals;
    if (!checkpointStaticRegions(Builder::add, &globals)) return false;
    std::vector<u8> original(globals.layout.byteSize()), restored(original.size());
    if (!globals.layout.save(original.data(), original.size())) return false;
    alignas(64) std::array<u8, 65536> system {}, data {};
    alignas(64) std::array<u8, 4096> module {};
    omDllData dll {};
    auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(module.data());
    auto *nt = reinterpret_cast<IMAGE_NT_HEADERS *>(module.data() + 128);
    dos->e_magic = IMAGE_DOS_SIGNATURE; dos->e_lfanew = 128;
    nt->Signature = IMAGE_NT_SIGNATURE;
    nt->FileHeader.SizeOfOptionalHeader = sizeof(nt->OptionalHeader);
    nt->FileHeader.NumberOfSections = 1;
    nt->OptionalHeader.SizeOfImage = static_cast<DWORD>(module.size());
    auto *section = IMAGE_FIRST_SECTION(nt);
    std::memcpy(section->Name, ".data", 6);
    section->VirtualAddress = 2048; section->Misc.VirtualSize = 64;
    section->Characteristics = IMAGE_SCN_MEM_WRITE;
    dll.hModule = reinterpret_cast<HMODULE>(module.data());
    omDLLinfoTbl[0] = &dll; omcurdll = 0;
    HuMemHeapInit(system.data(), system.size()); HuMemHeapInit(data.data(), data.size());
    CheckpointFixture fixture {{{{system.data(), system.size()}, {data.data(), data.size()}}},
        reinterpret_cast<u32 *>(module.data() + 2048)};
    frand_state_set(0x12345678); rand8_state_set(0x1234);
    GlobalCounter = 0; minimumVcount = 1; minimumVcountf = 1;
    std::memset(&wipeData, 0, sizeof(wipeData));
    wipeData.mode = WIPE_MODE_BLANK; wipeData.duration = 60.0f;
    wipeFadeInF = FALSE; pfInit();
    PartyBoard_CheckpointSequenceFixture();
    const bool replay = PartyBoard_CheckpointWithFixtureTasks(system.data(), runCheckpointFixture, &fixture);
    omDLLinfoTbl[0] = nullptr;
    bool ok = globals.layout.load(original.data(), original.size()) && replay;
    ok &= globals.layout.save(restored.data(), restored.size()) && restored == original;
    OSReport("Native coordinated checkpoint: %s (%u corrections, %u replayed ticks, %zu bytes). Two real coroutines, PAD/RNG/game/sequence/view/wipe/text state, fixture heaps and PE module; atomic rejection and interpolation reset.\n",
        ok ? "PASS" : "FAIL", fixture.corrections, fixture.replayed, fixture.bytes);
    return ok;
#else
    return false;
#endif
}
