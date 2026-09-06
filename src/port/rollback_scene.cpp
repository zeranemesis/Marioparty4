#include "port/rollback_scene.h"
#include "port/rollback_io.h"
#include "port/rollback.hpp"
extern "C" {
#include "port/frame_interpolation.h"
#include "game/object.h"
#include "game/objsub.h"
#include "game/hu3d.h"
#include "game/sprite.h"
#include "game/init.h"
extern HU3DCAMERA defCamera;
extern u16 Hu3DCameraExistF;
}
#include <cstring>
#include <vector>
#include <xxhash.h>
#include <chrono>

extern "C" bool PartyBoard_RollbackPresentationRegions(PartyBoardRollbackRegionSink sink, void *context)
{
    return PartyBoard_RollbackWipeRegions(sink, context)
        && PartyBoard_RollbackTextRegions(sink, context);
}

extern "C" bool PartyBoard_RollbackSceneRegions(PartyBoardRollbackRegionSink sink, void *context)
{
    if (!sink || !PartyBoard_RollbackObjectRegions(sink, context)) return false;
    if (!PartyBoard_RollbackModelRegions(sink, context)
        || !PartyBoard_RollbackMotionRegions(sink, context)
        || !PartyBoard_RollbackSpriteRegions(sink, context)
        || !PartyBoard_RollbackTextureRegions(sink, context)
        || !PartyBoard_RollbackPresentationRegions(sink, context)) return false;
#define REGION(value) if (!sink(context, &(value), sizeof(value))) return false;
    REGION(CRot) REGION(Center) REGION(CZoom)
    REGION(CRotM) REGION(CenterM) REGION(CZoomM) REGION(omDBGMenuButton)
    REGION(Hu3DCamera) REGION(defCamera) REGION(Hu3DCameraExistF)
    REGION(Hu3DCameraMtx) REGION(Hu3DCameraMtxXPose)
    REGION(Hu3DCameraNo) REGION(Hu3DCameraBit)
    REGION(mgTypeCurr) REGION(mgBattleStar) REGION(mgBattleStarMax)
    REGION(lbl_801D3E94) REGION(mgRecordExtra) REGION(mgQuitExtraF)
    REGION(mgPracticeEnableF) REGION(mgInstExitEnableF) REGION(mgBoardHostEnableF)
    REGION(mgTicTacToeGrid) REGION(mgIndexList) REGION(mgGameStatBackup)
#undef REGION
    return true;
}

namespace {
using namespace partyboard::rollback;
struct Header {
    uint32_t magic = 0x53434e53u, version = 3, total = 0, regions = 0;
    uint32_t pointerSize = sizeof(void *), kind = 0;
    uint64_t checksum = 0, layoutTag = 0;
    uint64_t processes = 0, modules = 0, io = 0;
};
struct HeapRegion { void *address; size_t bytes; };
using HeapSet = std::array<HeapRegion, 2>;
HeapSet currentHeaps() {
    return {{{HuMemHeapPtrGet(HEAP_SYSTEM), HuMemHeapSizeGet(HEAP_SYSTEM)},
             {HuMemHeapPtrGet(HEAP_DATA), HuMemHeapSizeGet(HEAP_DATA)}}};
}
struct Lease {
    uint64_t epoch = 0;
    bool acquired = PartyBoard_RollbackIOTryCapture(&epoch);
    ~Lease() { if (acquired) PartyBoard_RollbackIORelease(); }
};
struct Builder {
    SnapshotLayout layout;
    const void *buffer = nullptr;
    size_t bytes = 0;
    uint64_t signature = 0;
    static bool add(void *context, void *address, size_t size) {
        auto &self = *static_cast<Builder *>(context);
        const auto data = reinterpret_cast<uintptr_t>(address);
        const auto buffer = reinterpret_cast<uintptr_t>(self.buffer);
        if (data > UINTPTR_MAX - size || buffer > UINTPTR_MAX - self.bytes) return false;
        if (self.buffer && data < buffer + self.bytes && buffer < data + size) return false;
        try {
            if (!self.layout.addRegion(address, size)) return false;
            const std::array<uintptr_t, 2> identity {data, size};
            self.signature = XXH3_64bits_withSeed(identity.data(), sizeof(identity), self.signature);
            return true;
        } catch (...) { return false; }
    }
    bool build(const HeapSet *heaps = nullptr, bool execution = false,
        bool requireReplaySafe = true) {
        if (execution && requireReplaySafe
            && !PartyBoard_RollbackRenderCanReplayWithoutDraw()) return false;
        if (heaps) for (const auto &heap : *heaps)
            if (!add(this, heap.address, heap.bytes)) return false;
        if (!PartyBoard_RollbackSceneRegions(add, this)) return false;
        return !execution || (heaps && PartyBoard_RollbackPadRegions(add, this)
            && PartyBoard_RollbackGameRegions(add, this)
            && PartyBoard_RollbackSequenceRegions(add, this)
            && PartyBoard_RollbackClockRegions(add, this)
            && PartyBoard_RollbackProcessRegions(add, this)
            && PartyBoard_RollbackModuleRegions(add, this));
    }
};
uint64_t hash(const uint8_t *bytes, size_t size) {
    return XXH3_64bits(bytes, size);
}
}

extern "C" size_t PartyBoard_RollbackSceneSize(void)
{
    Builder builder;
    return builder.build() ? sizeof(Header) + builder.layout.byteSize() : 0;
}

static bool saveScene(void *destination, size_t capacity, const HeapSet *heaps, bool execution = false)
{
    if (heaps && HuPrcCurrentGet() != nullptr) return false;
    Lease lease;
    if (!lease.acquired) return false;
    Builder builder;
    builder.buffer = destination; builder.bytes = capacity;
    if (!destination || !builder.build(heaps, execution) || capacity > UINT32_MAX
        || capacity != sizeof(Header) + builder.layout.byteSize()) return false;
    Header header;
    header.total = static_cast<uint32_t>(capacity);
    header.regions = static_cast<uint32_t>(builder.layout.regionCount());
    header.kind = execution ? 2 : heaps ? 1 : 0;
    header.layoutTag = builder.signature;
    header.processes = HuPrcTopologyGenerationGet();
    header.modules = PartyBoard_RollbackModuleGeneration();
    header.io = lease.epoch;
    auto *payload = static_cast<uint8_t *>(destination) + sizeof(Header);
    if (!builder.layout.save(payload, builder.layout.byteSize())) return false;
    header.checksum = hash(payload, builder.layout.byteSize());
    std::memcpy(destination, &header, sizeof(header));
    return true;
}

static bool loadScene(const void *source, size_t size, const HeapSet *heaps, bool execution = false)
{
    if (heaps && HuPrcCurrentGet() != nullptr) return false;
    Lease lease;
    if (!lease.acquired) return false;
    Builder builder;
    builder.buffer = source; builder.bytes = size;
    // Loading rewinds presentation state too. It must remain available when a
    // predicted tick just activated a wipe that was absent in the snapshot.
    if (!source || !builder.build(heaps, execution, false)
        || size != sizeof(Header) + builder.layout.byteSize()) return false;
    Header header;
    std::memcpy(&header, source, sizeof(header));
    if (header.magic != 0x53434e53u || header.version != 3
        || header.total != size || header.regions != builder.layout.regionCount()
        || header.pointerSize != sizeof(void *) || header.io != lease.epoch
        || header.kind != (execution ? 2u : heaps ? 1u : 0u) || header.layoutTag != builder.signature
        || header.processes != HuPrcTopologyGenerationGet()
        || header.modules != PartyBoard_RollbackModuleGeneration()) return false;
    const auto *payload = static_cast<const uint8_t *>(source) + sizeof(Header);
    if (header.checksum != hash(payload, builder.layout.byteSize())) return false;
    // All metadata, lifetimes, ranges and bytes checked before any live write.
    const bool loaded = builder.layout.load(payload, builder.layout.byteSize());
    // Interpolation stores the two most recently rendered transforms outside
    // the logical scene. A rewind must seed that cache from the restored tick.
    if (loaded && execution) PartyBoard_FrameInterpolationReset();
    return loaded;
}

extern "C" bool PartyBoard_RollbackSceneSave(void *destination, size_t capacity)
{ return saveScene(destination, capacity, nullptr); }
extern "C" bool PartyBoard_RollbackSceneLoad(const void *source, size_t size)
{ return loadScene(source, size, nullptr); }
extern "C" size_t PartyBoard_RollbackResourcesSize(void)
{
    const auto heaps = currentHeaps();
    Builder builder;
    return builder.build(&heaps) ? sizeof(Header) + builder.layout.byteSize() : 0;
}
extern "C" bool PartyBoard_RollbackResourcesSave(void *destination, size_t capacity)
{
    const auto heaps = currentHeaps();
    return saveScene(destination, capacity, &heaps);
}
extern "C" bool PartyBoard_RollbackResourcesLoad(const void *source, size_t size)
{
    const auto heaps = currentHeaps();
    return loadScene(source, size, &heaps);
}

extern "C" size_t PartyBoard_RollbackCheckpointSize(void)
{
    if (HuPrcCurrentGet()) return 0;
    Lease lease;
    if (!lease.acquired) return 0;
    const auto heaps = currentHeaps();
    Builder builder;
    return builder.build(&heaps, true) ? sizeof(Header) + builder.layout.byteSize() : 0;
}
extern "C" bool PartyBoard_RollbackCheckpointSave(void *destination, size_t capacity)
{
    const auto heaps = currentHeaps();
    return saveScene(destination, capacity, &heaps, true);
}
extern "C" bool PartyBoard_RollbackCheckpointLoad(const void *source, size_t size)
{
    const auto heaps = currentHeaps();
    return loadScene(source, size, &heaps, true);
}

extern "C" bool PartyBoard_RollbackSceneSelfTest(void)
{
    const auto size = PartyBoard_RollbackSceneSize();
    std::vector<uint8_t> original(size), initial(size), expected(size), observed(size);
    if (!size || !PartyBoard_RollbackSceneSave(original.data(), size)) return false;
    bool passed = true;
    uint32_t corrections = 0, replayed = 0;
    try {
        CRot = {}; Center = {}; CZoom = 100;
        std::memset(CRotM, 0, sizeof(CRotM));
        std::memset(CenterM, 0, sizeof(CenterM));
        CZoomM[0] = CZoomM[1] = 100;
        mgRecordExtra = 0; omovlhisidx = 0;
        omOutView(nullptr);
        // With zero rotation, the real view calculation must look from z=100.
        passed &= Hu3DCamera[0].pos.z == 100 && Hu3DCamera[0].up.y == 1;
        passed &= PartyBoard_RollbackSceneSave(initial.data(), size);
        Callbacks callbacks;
        callbacks.saveState = PartyBoard_RollbackSceneSave;
        callbacks.loadState = PartyBoard_RollbackSceneLoad;
        callbacks.simulateFrame = [](uint32_t frame, const auto &inputs) {
            const int move = (inputs[1].buttons & 1) ? 1 : -1;
            Center.x += move * .5f;
            CRot.y += move * 2.0f;
            CZoom += (inputs[1].buttons & 2) ? .25f : -.25f;
            omOutView(nullptr);
            CRotM[1] = CRot; CenterM[1] = Center; CZoomM[1] = CZoom;
            omObjData view {};
            view.work[0] = 2;
            omOutViewMulti(&view);
            mgRecordExtra += inputs[1].buttons;
            omOvlHisChg(0, static_cast<OMOVL>(frame % 4), inputs[1].buttons, frame);
        };
        const auto sample = [](uint32_t frame) {
            PartyBoardRollbackInput input {};
            input.buttons = static_cast<uint16_t>((frame / 5) % 4);
            return input;
        };
        constexpr uint32_t count = 240;
        Session reference({2, 12, size}, callbacks);
        for (uint32_t frame = 0; frame < count; ++frame) {
            passed &= reference.submitInput(0, frame, {}) && reference.submitInput(1, frame, sample(frame));
            passed &= reference.advance();
        }
        passed &= PartyBoard_RollbackSceneSave(expected.data(), size);
        for (const uint32_t delay : {3u, 12u}) {
            passed &= PartyBoard_RollbackSceneLoad(initial.data(), size);
            Session delayed({2, 12, size}, callbacks);
            for (uint32_t frame = 0; frame < count; ++frame) {
                passed &= delayed.submitInput(0, frame, {});
                if (frame >= delay) passed &= delayed.submitInput(1, frame - delay, sample(frame - delay));
                passed &= delayed.advance();
            }
            for (uint32_t frame = count - delay; frame < count; ++frame)
                passed &= delayed.submitInput(1, frame, sample(frame));
            passed &= delayed.reconcile() && delayed.healthy();
            passed &= PartyBoard_RollbackSceneSave(observed.data(), size) && observed == expected;
            passed &= delayed.stats().rollbackCount > 0;
            corrections += delayed.stats().rollbackCount;
            replayed += delayed.stats().resimulatedFrames;
        }
        auto damaged = initial;
        damaged.back() ^= 1;
        passed &= !PartyBoard_RollbackSceneLoad(damaged.data(), size);
        passed &= !PartyBoard_RollbackSceneLoad(initial.data(), size - 1);
        for (const auto member : {&Header::processes, &Header::modules, &Header::io}) {
            damaged = initial;
            Header header;
            std::memcpy(&header, damaged.data(), sizeof(header));
            header.*member ^= 1;
            std::memcpy(damaged.data(), &header, sizeof(header));
            passed &= !PartyBoard_RollbackSceneLoad(damaged.data(), size);
        }
        passed &= PartyBoard_RollbackSceneSave(observed.data(), size) && observed == expected;
        passed &= !PartyBoard_RollbackSceneSave(&CRot, size);
        passed &= !PartyBoard_RollbackSceneLoad(&CRot, size);
    } catch (...) { passed = false; }
    passed &= PartyBoard_RollbackSceneLoad(original.data(), size);
    passed &= PartyBoard_RollbackSceneSave(observed.data(), size) && observed == original;
    // Exercise the live gate API with an empty tracked activity, never an
    // actual file operation. A failed capture must leave its buffer unchanged.
    try {
        auto untouched = original;
        {
            struct ReadActivity {
                ReadActivity() { PartyBoard_RollbackIOBegin(); }
                ~ReadActivity() { PartyBoard_RollbackIOEnd(); }
            } activity;
            passed &= !PartyBoard_RollbackSceneSave(untouched.data(), size);
            passed &= !PartyBoard_RollbackSceneLoad(original.data(), size);
            passed &= untouched == original;
        }
        passed &= !PartyBoard_RollbackSceneLoad(original.data(), size);
        passed &= PartyBoard_RollbackSceneSave(observed.data(), size);
        // Only the epoch changes: all original logical scene bytes remain.
        passed &= std::memcmp(observed.data() + sizeof(Header), original.data() + sizeof(Header),
            size - sizeof(Header)) == 0;
    } catch (...) { passed = false; }
    OSReport("Native logical scene rollback: %s (%u corrections, %u replayed ticks, %zu bytes). Real view calculations and overlay history; no full scene.\n",
        passed ? "PASS" : "FAIL", corrections, replayed, size);
    return passed;
}

#include "rollback_resources_test.inc"
#include "rollback_checkpoint_test.inc"
