#include "port/rollback.hpp"
#include "port/rollback_animation.h"
#include "port/rollback_scene.h"

extern "C" {
#include "game/hu3d.h"
#include "game/sprite.h"
#include "game/init.h"
}

#include <cmath>
#include <cstring>
#include <vector>

extern "C" bool PartyBoard_AnimationRollbackSelfTest(void)
{
    using namespace partyboard::rollback;
    // Fixtures own all referenced animation data for this headless test.
    // These shallow copies are NOT a runtime scene snapshot: a real scene
    // additionally owns geometry, textures, callbacks and allocated buffers.
    SnapshotLayout layout;
    if (!PartyBoard_RollbackSceneRegions([](void *context, void *address, size_t bytes) {
            return static_cast<SnapshotLayout *>(context)->addRegion(address, bytes);
        }, &layout)
        || !layout.addRegion(&minimumVcount, sizeof(minimumVcount))
        || !layout.addRegion(&minimumVcountf, sizeof(minimumVcountf))) return false;
    const auto size = layout.byteSize();
    std::vector<u8> original(size), initial(size), expected(size), observed(size);
    if (!layout.save(original.data(), size)) return false;
    HSFMOTION motion {};
    motion.maxTime = 20.0f;
    HSFDATA hsf {};
    hsf.motion = &motion;
    ANIMFRAME frames[4] {};
    frames[0].time = 2; frames[1].time = 3; frames[2].time = 5; frames[3].time = -1;
    ANIMBANK bank {};
    bank.timeNum = 3; bank.frame = frames;
    ANIMDATA anim {};
    anim.bankNum = 1; anim.bank = &bank;
    bool passed = true;
    u32 corrections = 0, replayed = 0;
    try {
        std::memset(Hu3DData, 0, sizeof(Hu3DData));
        std::memset(Hu3DMotion, 0, sizeof(Hu3DMotion));
        std::memset(HuSprData, 0, sizeof(HuSprData));
        std::memset(Hu3DTexAnimData, 0, sizeof(Hu3DTexAnimData));
        std::memset(Hu3DTexScrData, 0, sizeof(Hu3DTexScrData));
        for (auto &texture : Hu3DTexAnimData) texture.modelId = HU3D_MODELID_NONE;
        for (auto &scroll : Hu3DTexScrData) scroll.modelId = HU3D_MODELID_NONE;
        minimumVcount = 1; minimumVcountf = 1.0f; Hu3DPauseF = 0;
        // Regression: motion initialization must clear the motion table,
        // without touching any byte of the adjacent model table.
        Hu3DData[0].pos.x = 123.0f;
        std::vector<u8> modelsBefore(sizeof(Hu3DData));
        std::memcpy(modelsBefore.data(), Hu3DData, sizeof(Hu3DData));
        Hu3DMotion[0].hsf = &hsf;
        Hu3DMotionInit();
        passed &= std::memcmp(modelsBefore.data(), Hu3DData, sizeof(Hu3DData)) == 0;
        for (const auto &entry : Hu3DMotion) passed &= entry.hsf == nullptr;
        Hu3DMotion[0].hsf = &hsf;
        for (unsigned i = 0; i < 4; ++i) {
            auto &model = Hu3DData[i];
            model.hsf = &hsf;
            model.motId = 0;
            model.motIdOvl = model.motIdShift = model.motIdShape = -1;
            model.motAttr = HU3D_MOTATTR_LOOP & ~HU3D_MOTATTR;
            model.motWork.speed = 1.0f; model.motWork.end = 10.0f;
        }
        Hu3DData[0].motWork.time = 8.0f;
        Hu3DData[1].motWork.time = 1.0f;
        Hu3DData[1].motAttr |= HU3D_MOTATTR_REV & ~HU3D_MOTATTR;
        Hu3DData[2].motWork.time = 4.0f;
        Hu3DData[2].motAttr |= HU3D_MOTATTR_PAUSE & ~HU3D_MOTATTR;
        // Regression: shape-only animation must not dereference motion[-1].
        Hu3DData[3].motId = -1; Hu3DData[3].motIdShape = 0;
        Hu3DData[3].motShapeWork.speed = 2.0f;
        Hu3DData[3].motShapeWork.end = 20.0f;
        Hu3DData[3].motAttr = HU3D_MOTATTR_SHAPE_LOOP & ~HU3D_MOTATTR;
        HuSprData[1].data = &anim; HuSprData[1].speed = 1.0f;
        auto &texture = Hu3DTexAnimData[0];
        texture.modelId = 0; texture.anim = &anim; texture.speed = 1.0f;
        auto &scroll = Hu3DTexScrData[0];
        scroll.modelId = 0;
        scroll.attr = HU3D_TEXSCR_ATTR_POSMOVE | HU3D_TEXSCR_ATTR_ROTMOVE;
        scroll.pos.x = .98f; scroll.posMove.x = .07f;
        scroll.rot = 359.0f; scroll.rotMove = 3.0f;
        PartyBoard_AnimationAdvance();
        passed &= Hu3DData[0].motWork.time == 9.0f && Hu3DData[1].motWork.time == 0.0f
            && Hu3DData[2].motWork.time == 4.0f && Hu3DData[3].motShapeWork.time == 2.0f
            && HuSprData[1].time == 1.0f && texture.time == 1.0f
            && std::fabs(scroll.pos.x - .05f) < .00001f && scroll.rot == 2.0f;
        passed &= layout.save(initial.data(), size);
        Callbacks callbacks;
        callbacks.saveState = [&](void *data, std::size_t bytes) { return layout.save(data, bytes); };
        callbacks.loadState = [&](const void *data, std::size_t bytes) { return layout.load(data, bytes); };
        callbacks.simulateFrame = [&](u32 frame, const auto &inputs) {
            Hu3DPauseF = frame % 50 >= 40;
            HuSprPauseSet((inputs[1].buttons & 1) != 0);
            Hu3DData[0].motWork.speed = (inputs[1].buttons & 1) ? 2.0f : 1.0f;
            HuSprData[1].speed = (inputs[1].buttons & 2) ? 2.0f : 1.0f;
            texture.speed = HuSprData[1].speed;
            PartyBoard_AnimationAdvance();
        };
        const auto sample = [](u32 frame) {
            PartyBoardRollbackInput value {};
            value.buttons = static_cast<u16>((frame / 7) % 4);
            return value;
        };
        constexpr u32 count = 360;
        Session reference({2, 12, size}, callbacks);
        for (u32 frame = 0; frame < count; ++frame) {
            passed &= reference.submitInput(0, frame, {}) && reference.submitInput(1, frame, sample(frame));
            passed &= reference.advance();
        }
        passed &= layout.save(expected.data(), size);
        for (const u32 delay : {3u, 12u}) {
            passed &= layout.load(initial.data(), size);
            Session delayed({2, 12, size}, callbacks);
            for (u32 frame = 0; frame < count; ++frame) {
                passed &= delayed.submitInput(0, frame, {});
                if (frame >= delay) passed &= delayed.submitInput(1, frame - delay, sample(frame - delay));
                passed &= delayed.advance();
            }
            for (u32 frame = count - delay; frame < count; ++frame)
                passed &= delayed.submitInput(1, frame, sample(frame));
            passed &= delayed.reconcile() && delayed.healthy();
            passed &= layout.save(observed.data(), size) && observed == expected;
            passed &= delayed.stats().rollbackCount > 0;
            corrections += delayed.stats().rollbackCount;
            replayed += delayed.stats().resimulatedFrames;
        }
    } catch (...) { passed = false; }
    const bool restored = layout.load(original.data(), size);
    passed &= restored && layout.save(observed.data(), size) && observed == original;
    OSReport("Native animation rollback: %s (%u corrections, %u replayed ticks, %zu bytes/state). Model/sprite/texture clocks, fixture assets; no graphical game.\n",
        passed ? "PASS" : "FAIL", corrections, replayed, size);
    return passed;
}
