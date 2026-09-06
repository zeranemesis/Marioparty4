// Uses native allocators and native model/sprite updates with fixture assets.
// The live game's heap roots are never replaced by these test arenas.
extern "C" bool PartyBoard_RollbackResourcesSelfTest(void)
{
    alignas(64) std::array<uint8_t, 65536> systemArena {}, dataArena {};
    const HeapSet heaps {{{systemArena.data(), systemArena.size()}, {dataArena.data(), dataArena.size()}}};
    Builder builder;
    if (!builder.build(&heaps)) return false;
    const auto size = sizeof(Header) + builder.layout.byteSize();
    std::vector<uint8_t> original(PartyBoard_RollbackSceneSize()), initial(size), expected(size), observed(size);
    if (!PartyBoard_RollbackSceneSave(original.data(), original.size())) return false;
    const auto oldWait = minimumVcount;
    const auto oldWaitF = minimumVcountf;
    bool passed = true;
    uint32_t corrections = 0, replayed = 0;
    try {
        HuMemHeapInit(systemArena.data(), systemArena.size());
        HuMemHeapInit(dataArena.data(), dataArena.size());
        const auto allocate = [](void *heap, size_t bytes) {
            void *data = HuMemMemoryAlloc(heap, bytes, 0);
            if (!data) throw std::bad_alloc();
            std::memset(data, 0, bytes);
            return data;
        };
        auto *members = static_cast<s16 *>(allocate(systemArena.data(), sizeof(s16)));
        auto *counter = static_cast<u32 *>(allocate(systemArena.data(), sizeof(u32)));
        auto *anim = static_cast<ANIMDATA *>(allocate(dataArena.data(), sizeof(ANIMDATA)));
        auto *bank = static_cast<ANIMBANK *>(allocate(dataArena.data(), sizeof(ANIMBANK)));
        auto *frames = static_cast<ANIMFRAME *>(allocate(dataArena.data(), sizeof(ANIMFRAME) * 4));
        auto *hsf = static_cast<HSFDATA *>(allocate(dataArena.data(), sizeof(HSFDATA)));
        auto *motion = static_cast<HSFMOTION *>(allocate(dataArena.data(), sizeof(HSFMOTION)));
        frames[0].time = 2; frames[1].time = 3; frames[2].time = 5; frames[3].time = -1;
        bank->timeNum = 3; bank->frame = frames;
        anim->bankNum = 1; anim->bank = bank;
        hsf->motion = motion; motion->maxTime = 20;
        members[0] = 1;
        std::memset(HuSprData, 0, sizeof(HuSprData));
        std::memset(HuSprGrpData, 0, sizeof(HuSprGrpData));
        std::memset(Hu3DData, 0, sizeof(Hu3DData));
        std::memset(Hu3DMotion, 0, sizeof(Hu3DMotion));
        HuSprGrpData[0].capacity = 1; HuSprGrpData[0].members = members;
        HuSprData[1].data = anim; HuSprData[1].speed = 1;
        HuSprPauseSet(FALSE);
        auto &model = Hu3DData[0];
        model.hsf = hsf; model.motId = 0; model.motIdOvl = 0;
        model.motIdShift = model.motIdShape = -1;
        model.motAttr = HU3D_MOTATTR_OVL_LOOP & ~HU3D_MOTATTR;
        model.motWork.speed = model.motOvlWork.speed = 1;
        model.motWork.end = 20;
        Hu3DMotion[0].hsf = hsf;
        minimumVcount = 1; minimumVcountf = 1;
        passed &= saveScene(initial.data(), size, &heaps);
        Callbacks callbacks;
        callbacks.saveState = [&](void *data, size_t bytes) { return saveScene(data, bytes, &heaps); };
        callbacks.loadState = [&](const void *data, size_t bytes) { return loadScene(data, bytes, &heaps); };
        callbacks.simulateFrame = [&](uint32_t frame, const auto &inputs) {
            const auto value = inputs[1].buttons;
            // Free and replace a DATA allocation referenced by the model.
            // Restore must recover allocator links, contents AND this pointer.
            if (model.hookData) HuMemMemoryFree(model.hookData, 0);
            const size_t bytes = 32 + 32 * value;
            model.hookData = allocate(dataArena.data(), bytes);
            std::memset(model.hookData, static_cast<int>(frame + value), bytes);
            *counter = *counter * 33u + value;
            hsf->motion->maxTime = (value & 1) ? 30.0f : 20.0f;
            Hu3DMotionNext(0);
            HuSprSpeedSet(0, 0, (value & 2) ? 2.0f : 1.0f);
            HuSprPosSet(0, 0, static_cast<float>(*counter % 640), static_cast<float>(frame));
            HuSprFinish();
        };
        const auto sample = [](uint32_t frame) {
            PartyBoardRollbackInput input {};
            input.buttons = static_cast<uint16_t>((frame / 5) % 4);
            return input;
        };
        constexpr uint32_t count = 180;
        Session reference({2, 12, size}, callbacks);
        for (uint32_t frame = 0; frame < count; ++frame) {
            passed &= reference.submitInput(0, frame, {}) && reference.submitInput(1, frame, sample(frame));
            passed &= reference.advance();
        }
        passed &= saveScene(expected.data(), size, &heaps);
        const auto used = HuMemUsedMemorySizeGet(dataArena.data());
        for (const uint32_t delay : {3u, 12u}) {
            passed &= loadScene(initial.data(), size, &heaps);
            Session delayed({2, 12, size}, callbacks);
            for (uint32_t frame = 0; frame < count; ++frame) {
                passed &= delayed.submitInput(0, frame, {});
                if (frame >= delay) passed &= delayed.submitInput(1, frame - delay, sample(frame - delay));
                passed &= delayed.advance();
            }
            for (uint32_t frame = count - delay; frame < count; ++frame)
                passed &= delayed.submitInput(1, frame, sample(frame));
            passed &= delayed.reconcile() && delayed.healthy();
            passed &= saveScene(observed.data(), size, &heaps) && observed == expected;
            passed &= HuMemUsedMemorySizeGet(dataArena.data()) == used;
            passed &= delayed.stats().rollbackCount > 0;
            corrections += delayed.stats().rollbackCount;
            replayed += delayed.stats().resimulatedFrames;
        }
        auto damaged = initial;
        damaged.back() ^= 1;
        passed &= !loadScene(damaged.data(), size, &heaps);
        passed &= !loadScene(initial.data(), size - 1, &heaps);
        passed &= !loadScene(initial.data(), size, nullptr);
        passed &= !saveScene(systemArena.data(), size, &heaps);
        passed &= !loadScene(dataArena.data(), size, &heaps);
        std::vector<uint8_t> replacement(systemArena.size());
        auto moved = heaps;
        moved[0].address = replacement.data();
        passed &= !loadScene(initial.data(), size, &moved);
        passed &= saveScene(observed.data(), size, &heaps) && observed == expected;
        for (const auto byte : replacement) passed &= byte == 0;
    } catch (...) { passed = false; }
    passed &= PartyBoard_RollbackSceneLoad(original.data(), original.size());
    std::vector<uint8_t> restored(original.size());
    passed &= PartyBoard_RollbackSceneSave(restored.data(), restored.size()) && restored == original;
    minimumVcount = oldWait; minimumVcountf = oldWaitF;
    // Measure the copy/checksum path at the PC's 9 + 42 MiB arena sizes.
    // These are synthetic arenas: this is not a gameplay FPS measurement.
    try {
        std::vector<uint8_t> largeSystem(9u * 1024 * 1024), largeData(42u * 1024 * 1024);
        const HeapSet large {{{largeSystem.data(), largeSystem.size()}, {largeData.data(), largeData.size()}}};
        Builder largeBuilder;
        passed &= largeBuilder.build(&large);
        std::vector<uint8_t> capture(sizeof(Header) + largeBuilder.layout.byteSize());
        passed &= saveScene(capture.data(), capture.size(), &large);
        constexpr unsigned samples = 12;
        double sum = 0, maximum = 0;
        for (unsigned i = 0; i < samples; ++i) {
            const auto started = std::chrono::steady_clock::now();
            passed &= saveScene(capture.data(), capture.size(), &large);
            const auto milliseconds = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - started).count();
            sum += milliseconds;
            if (milliseconds > maximum) maximum = milliseconds;
        }
        OSReport("Resource capture benchmark: %.3f ms average, %.3f ms maximum, %zu bytes, 12 warm samples. Synthetic arenas; no render/replay/IO.\n",
            sum / samples, maximum, capture.size());
    } catch (...) { passed = false; }
    OSReport("Native resource rollback: %s (%u corrections, %u replayed ticks, %zu bytes). Native SYSTEM/DATA allocator and model/sprite updates, fixture arenas.\n",
        passed ? "PASS" : "FAIL", corrections, replayed, size);
    return passed;
}
