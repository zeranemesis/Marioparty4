#include "port/netplay_runtime.h"

#include "dolphin/os.h"
#include "dolphin/pad.h"
#include "port/netplay_transport.hpp"
#include "port/netplay_pad.hpp"
#include "port/netplay_progress.hpp"
#include "port/rollback.hpp"
#include "port/rollback_audio.hpp"
#include "port/rollback_audio_bridge.h"
#include "port/rollback_clock.h"
#include "port/rollback_game.h"
#include "port/rollback_animation.h"
#include "port/rollback_io.h"
#include "port/rollback_sequence.h"
#include "port/rollback_scene.h"
#include "port/settings.h"
#include "port/config.hpp"

extern "C" {
#include "game/gamework.h"
#include "game/gamework_data.h"
#include "game/pad.h"
}

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <stdexcept>
#include <thread>
#include <vector>

extern "C" bool PartyBoard_NetplayIsMinigame(void);
extern "C" s32 PartyBoard_NetplayMinigameId(void);
extern "C" s32 PartyBoard_NetplayContextId(void);
extern "C" u32 frand_state_get(void);
extern "C" void frand_state_set(u32 state);
extern "C" s32 rand8_state_get(void);
extern "C" void rand8_state_set(s32 state);
extern "C" u32 GlobalCounter;
extern "C" void *HuMemHeapPtrGet(int heap);
extern "C" std::size_t HuMemHeapSizeGet(int heap);
extern "C" std::size_t HuPrcSnapshotSizeGet(void);
extern "C" BOOL HuPrcSnapshotSave(void *destination, std::size_t capacity);
extern "C" u64 HuPrcTopologyGenerationGet(void);
extern "C" BOOL HuPrcSnapshotTopologySelfTest(void);
extern "C" BOOL HuPrcSnapshotExecutionSelfTest(void);
extern "C" std::size_t omDLLSnapshotSizeGet(void);
extern "C" BOOL omDLLSnapshotSave(void *destination, std::size_t capacity);
extern "C" std::size_t HuPadSnapshotSizeGet(void);
extern "C" BOOL HuPadSnapshotSave(void *destination, std::size_t capacity);
extern "C" BOOL HuPadSnapshotSelfTest(void);

namespace partyboard::netplay {
namespace {

constexpr std::uint32_t kSessionId = 0x4d503452u; // "MP4R"
constexpr std::uint32_t kRuntimeConfigMagic = 0x4e500000u; // "NP" + delay
constexpr std::uint32_t kRuntimeFullGameFlag = 0x00010000u;
constexpr std::uint32_t kRuntimeRollbackFlag = 0x00020000u;
constexpr std::uint32_t kRuntimeConfigMagicMask = 0xfffc0000u;
constexpr std::uint32_t kRetransmitRequest = 0x52545831u; // RTX1
constexpr std::size_t kHistorySize = 256;
constexpr std::uint8_t kDefaultInputDelay = 3;
constexpr std::uint8_t kMaximumInputDelay = 8;
constexpr std::uint32_t kContextMismatchGraceFrames = 120;

struct InputSlot {
    std::uint32_t frame = 0;
    PartyBoardRollbackInput input {};
    bool valid = false;
    std::uint32_t captureContext = 0;
};

struct Runtime {
    UdpTransport transport;
    SessionProgress progress;
    std::string error;
    bool disconnectProbe = false;
    bool contextMismatchProbe = false;
    bool realtimeProbe = false;
    std::array<InputSlot, kHistorySize> localHistory {};
    std::array<InputSlot, kHistorySize> remoteHistory {};
    PartyBoardRollbackInput lastRemote {};
    PartyBoardRollbackInput lastLocal {};
    PartyBoardRollbackInput pendingLocal {};
    std::uint32_t frame = 0;
    std::uint32_t sequence = 0;
    std::uint32_t receivedPackets = 0;
    std::uint32_t rejectedPackets = 0;
    std::uint32_t lastWireFrame = UINT32_MAX;
    std::uint32_t repairPackets = 0;
    std::uint32_t sendFailures = 0;
    std::uint64_t lastPacketMs = 0;
    std::uint64_t lastRepairRequestMs = 0;
    std::uint64_t lastDiagnosticMs = 0;
    unsigned diagnosticLines = 0;
    std::uint32_t stalledTicks = 0;
    std::uint32_t consecutiveStalledTicks = 0;
    std::uint32_t maximumStalledTicks = 0;
    std::uint32_t contextMismatchFrames = 0;
    std::uint32_t repairRequestFrame = UINT32_MAX;
    std::uint32_t sessionFrandSeed = 0;
    std::uint32_t sessionRand8Seed = 0;
    std::uint16_t localPort = 0;
    std::uint8_t localPlayer = 0;
    std::uint8_t localPad = 0;
    std::uint8_t inputDelay = kDefaultInputDelay;
    std::uint8_t contextId = 0;
    bool enabled = false;
    bool fullGame = false;
    bool overlayActive = false;
    bool localCaptured = false;
    bool peerTimeoutReported = false;
    bool configMismatch = false;
    bool peerInDifferentContext = false;
    bool randomSynchronized = false;
    bool rollbackProbe = false;
    bool rollbackProbeDone = false;
    bool rollbackRequested = false;
    bool rollbackPrepared = false;
    bool rollbackUnavailable = false;
    bool rollbackContextReady = false;
    std::uint32_t rollbackBaseFrame = 0;
    std::unique_ptr<rollback::Session> rollbackSession;
    int rollbackSessionContext = -1;
    int observedContext = -1;
    int probeContext = -1; // Set only by the headless PAD regression probe.
};

Runtime gRuntime;

std::uint64_t monotonicMs()
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

void writeDiagnostic(const char *event, bool force = false)
{
#ifdef _WIN32
    const auto now = monotonicMs();
    if (gRuntime.diagnosticLines >= 3000 || (!force && gRuntime.lastDiagnosticMs && now - gRuntime.lastDiagnosticMs < 2000)) return;
    gRuntime.lastDiagnosticMs = now;
    const auto *path = _wgetenv(L"PARTYBOARD_NET_DIAGNOSTIC");
    if (!path || !*path) return;
    FILE *file = _wfopen(path, L"ab");
    if (!file) return;
    const auto &local = gRuntime.localHistory[gRuntime.frame % kHistorySize];
    const auto &remote = gRuntime.remoteHistory[gRuntime.frame % kHistorySize];
    const auto utc = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    PartyBoardRollbackClock clock {};
    PartyBoard_RollbackClockSave(&clock);
    PartyBoardRollbackStats rollbackStats {};
    std::uint32_t rollbackConfirmed = 0;
    if (gRuntime.rollbackSession) {
        rollbackStats = gRuntime.rollbackSession->stats();
        rollbackConfirmed = gRuntime.rollbackSession->confirmedFrame();
    }
    u32 available[2] {};
    for (unsigned i = 0; i < 64; ++i)
        if (GWMGAvailGet(401 + i)) available[i / 32] |= 1u << (i % 32);
    std::fprintf(file, "utc_ms=%lld event=%s player=%u frame=%u context=%d wire_frame=%u local_ready=%d remote_ready=%d local_context=%u remote_context=%u received=%u rejected=%u repaired=%u send_errors=%u packet_age_ms=%llu rng_sync=%d seeds=%08x/%08x mismatch=%d context_skew=%u live_rng=%08x/%08x global_counter=%u tx_sequence=%u mg_available=%08x/%08x mg_next=%d language=%d message_speed=%d rollback_active=%d rollback_base=%u rollback_current=%u rollback_confirmed=%u rollback_count=%u rollback_replayed=%u rollback_max=%u rollback_predicted=%u rollback_late=%u\n",
        static_cast<long long>(utc), event, gRuntime.localPlayer, gRuntime.frame, gRuntime.observedContext,
        gRuntime.lastWireFrame, local.valid && local.frame == gRuntime.frame, remote.valid && remote.frame == gRuntime.frame,
        local.captureContext, remote.captureContext, gRuntime.receivedPackets, gRuntime.rejectedPackets,
        gRuntime.repairPackets, gRuntime.sendFailures, static_cast<unsigned long long>(gRuntime.lastPacketMs ? now-gRuntime.lastPacketMs : 0),
        gRuntime.randomSynchronized, gRuntime.sessionFrandSeed, gRuntime.sessionRand8Seed, gRuntime.configMismatch,
        gRuntime.contextMismatchFrames, clock.frandSeed, static_cast<u32>(clock.rand8Seed), clock.globalCounter, gRuntime.sequence,
        available[0], available[1], GWSystem.mg_next, GWGameStat.language, GWSystem.mess_speed,
        gRuntime.rollbackSession != nullptr, gRuntime.rollbackBaseFrame,
        rollbackStats.currentFrame, rollbackConfirmed, rollbackStats.rollbackCount,
        rollbackStats.resimulatedFrames, rollbackStats.maximumRollback,
        rollbackStats.predictedFrames, rollbackStats.lateInputs);
    std::fclose(file);
    ++gRuntime.diagnosticLines;
#else
    (void)event; (void)force;
#endif
}

void failSession(const char *reason)
{
    if (!gRuntime.error.empty()) return;
    gRuntime.error = reason;
    writeDiagnostic(reason, true);
    std::fprintf(stderr, "[NET] ERROR frame=%u: %s. Simulation stopped; close the game to restart the session.\n",
        gRuntime.frame, reason);
    PADControlMotor(gRuntime.localPad, PAD_MOTOR_STOP_HARD);
    gRuntime.transport.close();
    // Keep enabled: loss of a peer must never silently resume offline play.
}
std::array<PADStatus, 4> gPhysicalPads {};
std::array<PADStatus, 4> gSynchronizedPads {};

// Only stages raw values. The original game computes edges, repeats, analog
// thresholds and PAD errors exactly once, after the network tick is accepted.
void PartyBoard_NetplayPadApplyRemote(int pad, const PartyBoardRollbackInput *input,
    const PartyBoardRollbackInput *)
{
    gSynchronizedPads[pad] = padFromInput(*input);
}

bool parsePort(std::string_view text, std::uint16_t &port)
{
    unsigned value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc {} || result.ptr != text.data() + text.size() || value == 0 || value > 65535) {
        return false;
    }
    port = static_cast<std::uint16_t>(value);
    return true;
}

bool parseEndpoint(std::string_view text, std::string &address, std::uint16_t &port)
{
    const auto colon = text.rfind(':');
    if (colon == std::string_view::npos || colon == 0 || colon + 1 == text.size()) {
        return false;
    }
    address.assign(text.substr(0, colon));
    return parsePort(text.substr(colon + 1), port);
}

bool parseDelay(std::string_view text, std::uint8_t &delay)
{
    unsigned value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc {} || result.ptr != text.data() + text.size()
        || value > kMaximumInputDelay) {
        return false;
    }
    delay = static_cast<std::uint8_t>(value);
    return true;
}

bool parseLocalPad(std::string_view text, std::uint8_t &pad)
{
    unsigned value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc {} || result.ptr != text.data() + text.size()
        || value < 1 || value > 4) {
        return false;
    }
    pad = static_cast<std::uint8_t>(value - 1);
    return true;
}

PartyBoardRollbackInput capturePad(int pad)
{
    return inputFromPad(gPhysicalPads[pad]);
}

void resetOverlayTimeline(std::uint8_t contextId)
{
    if (gRuntime.rollbackSession) PartyBoard_RollbackAudioBridgeStop();
    gRuntime.rollbackSession.reset();
    gRuntime.rollbackPrepared = false;
    gRuntime.rollbackUnavailable = false;
    gRuntime.rollbackContextReady = false;
    gRuntime.rollbackBaseFrame = 0;
    gRuntime.rollbackSessionContext = -1;
    gRuntime.localHistory = {};
    gRuntime.remoteHistory = {};
    gRuntime.lastRemote = {};
    gRuntime.lastLocal = {};
    gRuntime.pendingLocal = {};
    gRuntime.frame = 0;
    gRuntime.sequence = 0;
    gRuntime.receivedPackets = 0;
    gRuntime.rejectedPackets = 0;
    gRuntime.stalledTicks = 0;
    gRuntime.consecutiveStalledTicks = 0;
    gRuntime.maximumStalledTicks = 0;
    gRuntime.contextMismatchFrames = 0;
    gRuntime.lastRepairRequestMs = 0;
    gRuntime.repairRequestFrame = UINT32_MAX;
    gRuntime.localCaptured = false;
    gRuntime.peerTimeoutReported = false;
    gRuntime.configMismatch = false;
    gRuntime.rollbackProbeDone = false;
    gRuntime.contextId = contextId;
    if (gRuntime.localPlayer == 0) {
        gRuntime.sessionFrandSeed = frand_state_get();
        gRuntime.sessionRand8Seed = static_cast<std::uint32_t>(rand8_state_get());
        gRuntime.randomSynchronized = true;
    } else {
        gRuntime.sessionFrandSeed = 0;
        gRuntime.sessionRand8Seed = 0;
        gRuntime.randomSynchronized = false;
    }
}

std::uint32_t checksumBytes(std::uint32_t hash, const std::uint8_t *data, std::size_t size)
{
    for (std::size_t index = 0; index < size; ++index) {
        hash ^= data[index];
        hash *= 16777619u;
    }
    return hash;
}

enum class SnapshotProbeResult { Busy, Failed, Captured };

SnapshotProbeResult runRollbackSnapshotProbe()
{
    std::uint64_t ioEpoch = 0;
    if (!PartyBoard_RollbackIOTryCapture(&ioEpoch)) return SnapshotProbeResult::Busy;
    struct CaptureLease {
        ~CaptureLease() { PartyBoard_RollbackIORelease(); }
    } lease;
    const auto started = std::chrono::steady_clock::now();
    const auto processGeneration = HuPrcTopologyGenerationGet();
    constexpr int kHeapSystem = 0;
    constexpr int kHeapData = 2;
    rollback::SnapshotLayout heapLayout;
    const std::size_t processBytes = HuPrcSnapshotSizeGet();
    const std::size_t overlayBytes = omDLLSnapshotSizeGet();
    if (!heapLayout.addRegion(HuMemHeapPtrGet(kHeapSystem), HuMemHeapSizeGet(kHeapSystem))
        || !heapLayout.addRegion(HuMemHeapPtrGet(kHeapData), HuMemHeapSizeGet(kHeapData))
        || !PartyBoard_RollbackSceneRegions([](void *context, void *region, std::size_t size) {
            return static_cast<rollback::SnapshotLayout *>(context)->addRegion(region, size);
        }, &heapLayout)
        || processBytes == 0 || overlayBytes == 0) {
        return SnapshotProbeResult::Failed;
    }

    std::vector<std::uint8_t> heapSnapshot(heapLayout.byteSize());
    std::vector<std::uint8_t> processSnapshot(processBytes);
    std::vector<std::uint8_t> overlaySnapshot(overlayBytes);
    std::vector<std::uint8_t> padSnapshot(HuPadSnapshotSizeGet());
    std::vector<std::uint8_t> gameSnapshot(PartyBoard_RollbackGameSize());
    std::vector<std::uint8_t> sequenceSnapshot(PartyBoard_RollbackSequenceSize());
    PartyBoardRollbackClock clockSnapshot {};
    PartyBoard_RollbackClockSave(&clockSnapshot);
    if (!heapLayout.save(heapSnapshot.data(), heapSnapshot.size())
        || !HuPrcSnapshotSave(processSnapshot.data(), processSnapshot.size())
        || !omDLLSnapshotSave(overlaySnapshot.data(), overlaySnapshot.size())
        || !HuPadSnapshotSave(padSnapshot.data(), padSnapshot.size())
        || !PartyBoard_RollbackGameSave(gameSnapshot.data(), gameSnapshot.size())
        || !PartyBoard_RollbackSequenceSave(sequenceSnapshot.data(), sequenceSnapshot.size())
        || processGeneration != HuPrcTopologyGenerationGet()) {
        return SnapshotProbeResult::Failed;
    }

    std::uint32_t checksum = checksumBytes(2166136261u, heapSnapshot.data(), heapSnapshot.size());
    checksum = checksumBytes(checksum, processSnapshot.data(), processSnapshot.size());
    checksum = checksumBytes(checksum, overlaySnapshot.data(), overlaySnapshot.size());
    checksum = checksumBytes(checksum, padSnapshot.data(), padSnapshot.size());
    checksum = checksumBytes(checksum, gameSnapshot.data(), gameSnapshot.size());
    checksum = checksumBytes(checksum, sequenceSnapshot.data(), sequenceSnapshot.size());
    checksum = checksumBytes(checksum, reinterpret_cast<const std::uint8_t *>(&clockSnapshot), sizeof(clockSnapshot));
    std::fprintf(stdout,
        "Rollback probe: partial snapshot captured (%zu heaps/scene-globals + %zu coroutine + %zu overlay + %zu PAD + %zu game + %zu sequence + %zu clock bytes, checksum 0x%08x, capture_ms=%.3f, process_generation=%llu). Full-game rollback remains disabled.\n",
        heapSnapshot.size(), processSnapshot.size(), overlaySnapshot.size(), padSnapshot.size(), gameSnapshot.size(), sequenceSnapshot.size(), sizeof(clockSnapshot), checksum,
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count(),
        static_cast<unsigned long long>(processGeneration));
    return SnapshotProbeResult::Captured;
}

void storeInput(std::array<InputSlot, kHistorySize> &history, std::uint32_t frame,
    const PartyBoardRollbackInput &input)
{
    InputSlot &slot = history[frame % kHistorySize];
    slot = { frame, input, true };
    slot.captureContext = static_cast<std::uint32_t>(gRuntime.observedContext);
}

const InputSlot *findInput(const std::array<InputSlot, kHistorySize> &history, std::uint32_t frame)
{
    const InputSlot &slot = history[frame % kHistorySize];
    return slot.valid && slot.frame == frame ? &slot : nullptr;
}

std::uint32_t runtimeConfigSignature(std::uint8_t inputDelay, std::uint8_t contextId,
    bool fullGame, bool rollbackRequested)
{
    return kRuntimeConfigMagic | (fullGame ? kRuntimeFullGameFlag : 0u)
        | (rollbackRequested ? kRuntimeRollbackFlag : 0u)
        | (static_cast<std::uint32_t>(contextId) << 8) | inputDelay;
}

bool sendInput(std::uint32_t frame, const PartyBoardRollbackInput &input, bool requestRetransmit = false)
{
    // Headless regression: drop this sample until the sender has advanced.
    // Only the retained-history repair can then release the waiting peer.
    if (gRuntime.probeContext >= 0 && gRuntime.localPlayer == 0
        && frame == 200 && gRuntime.frame <= 200) return true;
    InputPacket outgoing {};
    outgoing.sessionId = kSessionId;
    outgoing.sequence = gRuntime.sequence++;
    outgoing.frame = frame;
    outgoing.player = gRuntime.localPlayer;
    outgoing.input = input;
    outgoing.stateChecksum = requestRetransmit ? kRetransmitRequest : 0;
    const InputSlot *slot = findInput(gRuntime.localHistory, frame);
    outgoing.captureContext = slot ? slot->captureContext : static_cast<std::uint32_t>(gRuntime.observedContext);
    outgoing.configSignature = runtimeConfigSignature(gRuntime.inputDelay, gRuntime.contextId,
        gRuntime.fullGame, gRuntime.rollbackRequested);
    outgoing.frandSeed = gRuntime.randomSynchronized ? gRuntime.sessionFrandSeed : 0;
    outgoing.rand8Seed = gRuntime.randomSynchronized ? gRuntime.sessionRand8Seed : 0;
    const bool sent = !gRuntime.transport.hasPeer() || gRuntime.transport.sendInput(outgoing);
    if (!sent) ++gRuntime.sendFailures;
    return sent;
}

bool contextCatchupRequired(bool sawMatchingContext, bool sawDifferentContext)
{
    return sawDifferentContext && !sawMatchingContext;
}

void receivePendingPackets()
{
    InputPacket packet {};
    bool sawMatchingContext = false;
    bool sawDifferentContext = false;
    // A busy peer must never monopolize the game thread.
    for (unsigned received = 0; received < 64 && gRuntime.transport.receiveInput(packet); ++received) {
        gRuntime.lastWireFrame = packet.frame;
        gRuntime.lastPacketMs = monotonicMs();
        if (packet.sessionId != kSessionId || packet.player != (gRuntime.localPlayer ^ 1u)) {
            ++gRuntime.rejectedPackets;
            continue;
        }
        const bool signatureValid =
            (packet.configSignature & kRuntimeConfigMagicMask) == kRuntimeConfigMagic;
        const bool remoteFullGame = (packet.configSignature & kRuntimeFullGameFlag) != 0;
        const bool remoteRollback = (packet.configSignature & kRuntimeRollbackFlag) != 0;
        const std::uint8_t remoteContext =
            static_cast<std::uint8_t>((packet.configSignature >> 8) & 0xffu);
        const std::uint8_t remoteDelay = static_cast<std::uint8_t>(packet.configSignature & 0xffu);
        if (!signatureValid || remoteFullGame != gRuntime.fullGame
            || remoteRollback != gRuntime.rollbackRequested
            || remoteDelay != gRuntime.inputDelay) {
            if (!gRuntime.configMismatch) {
                std::fprintf(stderr,
                    "Netplay: session mismatch (local mode %s/delay %u, remote signature 0x%08x).\n",
                    gRuntime.fullGame ? "full" : "minigame",
                    static_cast<unsigned>(gRuntime.inputDelay), packet.configSignature);
            }
            gRuntime.configMismatch = true;
            ++gRuntime.rejectedPackets;
            continue;
        }
        if (remoteContext != gRuntime.contextId) {
            // An overlay can take a different amount of wall time to load on
            // each machine. Remember the skew so PartyBoard_NetplayTick can
            // let the slower peer finish its local transition. Merely dropping
            // these packets while keeping lockstep stalled deadlocks both
            // peers: neither can advance far enough to enter the other's
            // context.
            sawDifferentContext = true;
            ++gRuntime.rejectedPackets;
            continue;
        }
        sawMatchingContext = true;
        if (gRuntime.localPlayer == 1 && !gRuntime.randomSynchronized) {
            gRuntime.sessionFrandSeed = packet.frandSeed;
            gRuntime.sessionRand8Seed = packet.rand8Seed;
            frand_state_set(gRuntime.sessionFrandSeed);
            rand8_state_set(static_cast<s32>(gRuntime.sessionRand8Seed));
            gRuntime.randomSynchronized = true;
            std::fprintf(stdout,
                "Netplay: random generators synchronized (0x%08x/0x%08x).\n",
                gRuntime.sessionFrandSeed, gRuntime.sessionRand8Seed);
        } else if (gRuntime.localPlayer == 1
            && (packet.frandSeed != gRuntime.sessionFrandSeed
                || packet.rand8Seed != gRuntime.sessionRand8Seed)) {
            gRuntime.configMismatch = true;
            ++gRuntime.rejectedPackets;
            std::fputs("Netplay: host random seed changed during the session.\n", stderr);
            continue;
        }
        // A repair can concern an old, current or already captured future tick.
        // Always answer from immutable history, even while we are also waiting.
        if (packet.stateChecksum == kRetransmitRequest) {
            if (const auto *old = findInput(gRuntime.localHistory, packet.frame)) {
                sendInput(packet.frame, old->input);++gRuntime.repairPackets;
            }
        }
        const bool retainedLateFrame = packet.frame < gRuntime.frame
            && gRuntime.rollbackSession
            && packet.frame >= gRuntime.rollbackBaseFrame
            && gRuntime.frame - packet.frame <= 6;
        if (packet.frame < gRuntime.frame && !retainedLateFrame) {
            ++gRuntime.rejectedPackets;
            continue;
        }
        if (packet.frame >= gRuntime.frame
            && packet.frame - gRuntime.frame >= kHistorySize) {
            ++gRuntime.rejectedPackets;
            continue;
        }
        InputSlot &slot = gRuntime.remoteHistory[packet.frame % kHistorySize];
        if (!slot.valid || slot.frame != packet.frame) {
            slot.frame = packet.frame;
            slot.input = packet.input;
            slot.valid = true;
            slot.captureContext = packet.captureContext;
        } else if (!rollback::inputsEqual(slot.input, packet.input)
            || slot.captureContext != packet.captureContext) {
            ++gRuntime.rejectedPackets;
            continue;
        }
        if (packet.captureContext == static_cast<std::uint32_t>(gRuntime.observedContext))
            gRuntime.rollbackContextReady = true;
        if (gRuntime.rollbackSession && packet.frame >= gRuntime.rollbackBaseFrame) {
            const auto rollbackFrame = packet.frame - gRuntime.rollbackBaseFrame;
            if (!gRuntime.rollbackSession->submitInput(packet.player, rollbackFrame, packet.input)) {
                ++gRuntime.rejectedPackets;
                continue;
            }
        }
        ++gRuntime.receivedPackets;
        gRuntime.progress.observePeer(monotonicMs());
    }
    gRuntime.peerInDifferentContext = contextCatchupRequired(
        sawMatchingContext, sawDifferentContext);
}

bool ensureRollbackSession()
{
    if (!gRuntime.rollbackRequested || gRuntime.rollbackUnavailable
        || (gRuntime.fullGame && !gRuntime.rollbackContextReady)) return false;
    if (gRuntime.rollbackSession) return true;
    const auto snapshotBytes = PartyBoard_RollbackCheckpointSize();
    // An active wipe, render callback, I/O operation or module transition is
    // temporary. Keep lockstep until the next safe game-tick boundary.
    if (!snapshotBytes) return false;
    gRuntime.rollbackBaseFrame = gRuntime.frame;
    rollback::Callbacks callbacks;
    callbacks.saveState = [](void *destination, std::size_t capacity) {
        return PartyBoard_RollbackCheckpointSave(destination, capacity);
    };
    callbacks.loadState = [](const void *source, std::size_t size) {
        return PartyBoard_RollbackCheckpointLoad(source, size);
    };
    callbacks.simulateFrame = [](std::uint32_t frame, const auto &inputs) {
        const auto wireFrame = gRuntime.rollbackBaseFrame + frame;
        if (!PartyBoard_RollbackAudioBridgeFrameBegin(wireFrame))
            throw std::runtime_error("rollback audio begin failed");
        if (!PartyBoard_RollbackRunGameLogicTick(inputs.data(), 3)) {
            PartyBoard_RollbackAudioBridgeFrameEnd();
            throw std::runtime_error("rollback game tick failed");
        }
        // The rendered path publishes this counter after the frame. Replayed
        // ticks have no presentation pass, so publish it here instead.
        ++GlobalCounter;
        if (!PartyBoard_RollbackAudioBridgeFrameEnd())
            throw std::runtime_error("rollback audio end failed");
    };
    try {
        if (!PartyBoard_RollbackAudioBridgeStart()) return false;
        auto session = std::make_unique<rollback::Session>(
            rollback::Config {2, 6, snapshotBytes}, std::move(callbacks));
        if (!session->healthy()) {
            PartyBoard_RollbackAudioBridgeStop();
            return false;
        }
        gRuntime.rollbackSession = std::move(session);
        gRuntime.rollbackSessionContext = gRuntime.observedContext;
    } catch (...) {
        PartyBoard_RollbackAudioBridgeStop();
        gRuntime.rollbackUnavailable = true;
        std::fputs("Rollback: insufficient memory for the checkpoint window; lockstep retained.\n", stderr);
        return false;
    }
    std::fprintf(stdout,
        "Rollback: live prediction armed at network frame %u (%zu bytes x 7 snapshots, 6-frame correction window).\n",
        gRuntime.rollbackBaseFrame, snapshotBytes);
    return true;
}

bool prepareRollbackTick()
{
    auto &runtime = gRuntime;
    auto &session = *runtime.rollbackSession;
    if (runtime.rollbackPrepared
        || runtime.frame != runtime.rollbackBaseFrame + session.currentFrame()) {
        failSession("Rollback tick boundary mismatch");
        return false;
    }
    const auto rollbackFrame = runtime.frame - runtime.rollbackBaseFrame;
    const InputSlot *localSlot = findInput(runtime.localHistory, runtime.frame);
    const InputSlot *remoteSlot = findInput(runtime.remoteHistory, runtime.frame);
    if (runtime.frame < runtime.inputDelay) {
        const PartyBoardRollbackInput neutral {};
        if (!session.submitInput(0, rollbackFrame, neutral)
            || !session.submitInput(1, rollbackFrame, neutral)) {
            failSession("Rollback startup input rejected");
            return false;
        }
    } else {
        if (!localSlot
            || !session.submitInput(runtime.localPlayer, rollbackFrame, localSlot->input)) {
            failSession("Rollback local input missing");
            return false;
        }
        if (remoteSlot && !session.submitInput(runtime.localPlayer ^ 1u,
            rollbackFrame, remoteSlot->input)) {
            failSession("Rollback remote input rejected");
            return false;
        }
    }
    std::array<PartyBoardRollbackInput, rollback::kMaxPlayers> inputs {};
    if (!session.prepare(inputs)) {
        if (!session.healthy()) {
            failSession("Rollback checkpoint or replay failed");
            return false;
        }
        if (localSlot) sendInput(runtime.frame, localSlot->input, true);
        ++runtime.stalledTicks;
        ++runtime.consecutiveStalledTicks;
        runtime.maximumStalledTicks = std::max(runtime.maximumStalledTicks,
            runtime.consecutiveStalledTicks);
        return false;
    }
    if (!PartyBoard_RollbackAudioBridgeFrameBegin(runtime.frame)) {
        failSession("Rollback audio frame failed");
        return false;
    }
    PartyBoard_NetplayPadApplyRemote(0, &inputs[0], &runtime.lastLocal);
    PartyBoard_NetplayPadApplyRemote(1, &inputs[1], &runtime.lastRemote);
    runtime.lastLocal = inputs[runtime.localPlayer];
    runtime.lastRemote = inputs[runtime.localPlayer ^ 1u];
    runtime.rollbackPrepared = true;
    runtime.consecutiveStalledTicks = 0;
    runtime.repairRequestFrame = UINT32_MAX;
    runtime.peerTimeoutReported = false;
    return true;
}

bool timelineSelfTest()
{
    std::array<InputSlot, 32> history {};
    PartyBoardRollbackInput previous {};
    unsigned edges = 0;
    for (std::uint32_t frame = 0; frame < 12; ++frame) {
        PartyBoardRollbackInput input {};
        if (frame == 3 || frame == 7) {
            input.buttons = PAD_BUTTON_A;
        }
        InputSlot &slot = history[frame % history.size()];
        slot = { frame, input, true };
    }
    for (std::uint32_t frame = 0; frame < 12; ++frame) {
        const InputSlot &slot = history[frame % history.size()];
        if ((slot.input.buttons & ~previous.buttons) & PAD_BUTTON_A) {
            ++edges;
        }
        previous = slot.input;
    }
    if (edges != 2) {
        return false;
    }
    if (!contextCatchupRequired(false, true)
        || contextCatchupRequired(true, true)
        || contextCatchupRequired(false, false)) {
        return false;
    }

    std::array<InputSlot, 32> delayedHistory {};
    constexpr std::uint32_t delay = 2;
    for (std::uint32_t captureFrame = 0; captureFrame < 10; ++captureFrame) {
        PartyBoardRollbackInput input {};
        input.stickX = static_cast<s8>(captureFrame + 1);
        InputSlot &slot = delayedHistory[(captureFrame + delay) % delayedHistory.size()];
        slot = { captureFrame + delay, input, true };
    }
    for (std::uint32_t simulationFrame = 0; simulationFrame < 12; ++simulationFrame) {
        const InputSlot &slot = delayedHistory[simulationFrame % delayedHistory.size()];
        if (simulationFrame < delay) {
            if (slot.valid && slot.frame == simulationFrame) {
                return false;
            }
        } else if (!slot.valid || slot.frame != simulationFrame
            || slot.input.stickX != static_cast<s8>(simulationFrame - delay + 1)) {
            return false;
        }
    }
    std::uint8_t parsedPad = 0xff;
    if (!parseLocalPad("1", parsedPad) || parsedPad != 0
        || !parseLocalPad("4", parsedPad) || parsedPad != 3
        || parseLocalPad("0", parsedPad) || parseLocalPad("5", parsedPad)) {
        return false;
    }
    return runtimeConfigSignature(0, 7, false, false) != runtimeConfigSignature(delay, 7, false, false)
        && runtimeConfigSignature(delay, 7, false, false)
            == (kRuntimeConfigMagic | (7u << 8) | delay)
        && runtimeConfigSignature(delay, 7, false, false) != runtimeConfigSignature(delay, 8, false, false)
        && runtimeConfigSignature(delay, 7, false, false) != runtimeConfigSignature(delay, 7, true, false)
        && runtimeConfigSignature(delay, 7, false, false) != runtimeConfigSignature(delay, 7, false, true);
}

}
}

extern "C" bool PartyBoard_NetplayConfigureFromArgs(int argc, char **argv)
{
    using namespace partyboard::netplay;
    bool host = false;
    bool join = false;
    bool argumentsValid = true;
    std::uint16_t hostPort = 0;
    std::uint16_t joinPort = 0;
    std::string joinAddress;
    std::uint8_t inputDelay = kDefaultInputDelay;
    std::uint8_t localPad = 0;
    bool fullGame = false;
    bool rollbackProbe = false;
    bool rollbackRequested = false;

    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--netplay-host" && index + 1 < argc) {
            host = true;
            argumentsValid = parsePort(argv[++index], hostPort) && argumentsValid;
        } else if (argument == "--netplay-join" && index + 1 < argc) {
            join = true;
            argumentsValid = parseEndpoint(argv[++index], joinAddress, joinPort) && argumentsValid;
        } else if (argument == "--netplay-delay" && index + 1 < argc) {
            argumentsValid = parseDelay(argv[++index], inputDelay) && argumentsValid;
        } else if (argument == "--netplay-pad" && index + 1 < argc) {
            argumentsValid = parseLocalPad(argv[++index], localPad) && argumentsValid;
        } else if (argument == "--netplay-full") {
            fullGame = true;
        } else if (argument == "--netplay-rollback-probe") {
            rollbackProbe = true;
        } else if (argument == "--netplay-rollback") {
            rollbackRequested = true;
        } else if (argument == "--netplay-probe-disconnect") {
            gRuntime.disconnectProbe = true;
        } else if (argument == "--netplay-probe-context-mismatch") {
            gRuntime.contextMismatchProbe = true;
        } else if (argument == "--netplay-probe-realtime") {
            gRuntime.realtimeProbe = true;
        } else if (argument == "--netplay-host" || argument == "--netplay-join"
            || argument == "--netplay-delay" || argument == "--netplay-pad") {
            argumentsValid = false;
        }
    }
    if (!host && !join) {
        return true;
    }
    if (!argumentsValid || host == join) {
        std::fputs("Netplay: choose exactly one of --netplay-host <port> or --netplay-join <IPv4:port>; optional --netplay-delay <0-8>, --netplay-pad <1-4>, --netplay-full, --netplay-rollback and --netplay-rollback-probe.\n", stderr);
        return false;
    }

    gRuntime.localPlayer = host ? 0 : 1;
    // The online companion owns the Internet-facing encrypted transport.
    // Its game subprocess must never expose the raw UDP protocol to the LAN.
    bool loopbackOnly = false;
    for (int i = 1; i < argc; ++i)
        if (std::string_view(argv[i]) == "--netplay-loopback") loopbackOnly = true;
    if (loopbackOnly && join && joinAddress != "127.0.0.1") return false;
    if (!gRuntime.transport.open(host ? hostPort : 0, loopbackOnly)) {
        std::fprintf(stderr, "Netplay: %s\n", gRuntime.transport.lastError().c_str());
        return false;
    }
    if (host) {
        gRuntime.transport.enablePeerDiscovery(true);
    } else if (!gRuntime.transport.setPeer(joinAddress, joinPort)) {
        std::fprintf(stderr, "Netplay: %s\n", gRuntime.transport.lastError().c_str());
        gRuntime.transport.close();
        return false;
    }
    gRuntime.localPort = gRuntime.transport.localPort();
    gRuntime.localPad = localPad;
    gRuntime.inputDelay = inputDelay;
    gRuntime.fullGame = fullGame;
    gRuntime.rollbackRequested = rollbackRequested;
    // Loopback only describes the local socket exposure. The companion now
    // forwards inputs over UDP too, so it needs the same loss recovery.
    gRuntime.rollbackProbe = rollbackProbe;
    gRuntime.enabled = true;
    std::fprintf(stdout,
        "Netplay experimental: %s, UDP port %u, local player %u, physical controller port %u, input delay %u frame(s). %s.\n",
        host ? "host" : "client", gRuntime.localPort,
        static_cast<unsigned>(gRuntime.localPlayer + 1),
        static_cast<unsigned>(gRuntime.localPad + 1),
        static_cast<unsigned>(gRuntime.inputDelay),
        gRuntime.fullGame ? (gRuntime.rollbackRequested
            ? "Full-game rollback requested"
            : "Full-game lockstep")
        : (gRuntime.rollbackRequested ? "Minigame rollback requested" : "Minigame overlays"));
    return true;
}

extern "C" bool PartyBoard_NetplayEnabled(void)
{
    return partyboard::netplay::gRuntime.enabled;
}

extern "C" bool PartyBoard_NetplayAllowsMultipleInstances(void)
{
    return PartyBoard_NetplayEnabled();
}

extern "C" bool PartyBoard_NetplayPreparePads(PADStatus status[4], u32 *rumble, bool startup)
{
    using namespace partyboard::netplay;
    if (!gRuntime.error.empty()) return false;
    if (!gRuntime.enabled || (!startup && !gRuntime.fullGame && !PartyBoard_NetplayIsMinigame())) {
        // Mark minigame-only sessions inactive and suspend their watchdog
        // while the local player is navigating unsynchronized menus.
        if (gRuntime.enabled && !startup) PartyBoard_NetplayTick();
        return true;
    }
    std::copy_n(status, 4, gPhysicalPads.begin());
    gSynchronizedPads = {};
    // This first runtime has two seats; unused physical controllers must not
    // leak into the synchronized game on one machine only.
    gSynchronizedPads[2].err = PAD_ERR_NO_CONTROLLER;
    gSynchronizedPads[3].err = PAD_ERR_NO_CONTROLLER;
    if (!startup && !PartyBoard_NetplayTick()) return false;
    std::copy(gSynchronizedPads.begin(), gSynchronizedPads.end(), status);
    // Deterministic virtual capabilities; actual vibration is routed locally.
    *rumble = PAD_CHAN0_BIT | PAD_CHAN1_BIT;
    return true;
}

extern "C" void PartyBoard_NetplayControlMotor(u32 port, u32 command)
{
    if (PartyBoard_RollbackIsResimulating()) return;
    using namespace partyboard::netplay;
    if (gRuntime.enabled && (gRuntime.fullGame || PartyBoard_NetplayIsMinigame())) {
        if (port != gRuntime.localPlayer) return;
        port = gRuntime.localPad;
    }
    if (port < 4) PADControlMotor(port, command);
}

extern "C" bool PartyBoard_NetplayTick(void)
{
    using namespace partyboard::netplay;
    Runtime &runtime = gRuntime;
    if (!runtime.enabled) {
        return true;
    }
    if (!runtime.error.empty()) return false;

    const bool active = runtime.fullGame || PartyBoard_NetplayIsMinigame();
    if (!active) {
        runtime.progress = SessionProgress {};
        if (runtime.overlayActive) {
            std::fprintf(stdout,
                "Netplay: minigame ended (%u packets, %u rejected, %u waiting ticks, longest wait %u).\n",
                runtime.receivedPackets, runtime.rejectedPackets, runtime.stalledTicks,
                runtime.maximumStalledTicks);
            runtime.overlayActive = false;
        }
        return true;
    }
    runtime.progress.start(monotonicMs());
    writeDiagnostic("tick");
    const int gameContext = runtime.probeContext >= 0 ? runtime.probeContext : (runtime.fullGame
        ? PartyBoard_NetplayContextId()
        : PartyBoard_NetplayMinigameId());
    if (runtime.observedContext != gameContext) {
        runtime.observedContext = gameContext;
        runtime.rollbackProbeDone = false;
        runtime.rollbackContextReady = false;
        if (!runtime.rollbackSession) runtime.rollbackUnavailable = false;
        std::fprintf(stdout, "Netplay: game context %d at network frame %u.\n", gameContext, runtime.frame);
    }
    // A full game owns ONE input timeline and ONE initial RNG agreement.
    // Loading a module must not discard delayed inputs or authorize local ticks.
    // Minigame-only sessions still start a fresh timeline for each minigame.
    const std::uint8_t currentContext = runtime.fullGame ? 0 : static_cast<std::uint8_t>(gameContext);
    if (!runtime.overlayActive || runtime.contextId != currentContext) {
        if (runtime.overlayActive) {
            std::fprintf(stdout,
                "Netplay: context %u ended (%u packets, %u rejected, %u waiting ticks, longest wait %u).\n",
                static_cast<unsigned>(runtime.contextId), runtime.receivedPackets,
                runtime.rejectedPackets, runtime.stalledTicks, runtime.maximumStalledTicks);
        }
        resetOverlayTimeline(currentContext);
        runtime.overlayActive = true;
        std::fprintf(stdout,
            "Netplay: %s synchronization active (context %u, %u-frame jitter buffer).\n",
            runtime.fullGame ? "full-game" : "minigame",
            static_cast<unsigned>(runtime.contextId),
            static_cast<unsigned>(runtime.inputDelay));
    }

    if (runtime.rollbackProbe && !runtime.rollbackProbeDone
        && PartyBoard_NetplayIsMinigame()) {
        const auto probe = runRollbackSnapshotProbe();
        runtime.rollbackProbeDone = probe != SnapshotProbeResult::Busy;
        if (probe == SnapshotProbeResult::Failed) {
            std::fputs("Rollback probe: snapshot capture unavailable; lockstep remains active.\n",
                stderr);
        }
    }

    // Check before receiving: late packets must not revive an expired session.
    const auto failure = runtime.progress.check(monotonicMs());
    if (failure != ProgressFailure::None) {
        failSession(failure == ProgressFailure::NoPeer
            ? "Aucun joueur compatible apres 2 minutes" : "Aucune progression apres 2 minutes : connexion perdue ou synchronisation bloquee");
        return false;
    }
    receivePendingPackets();

    if (runtime.configMismatch) {
        failSession("Incompatible netplay configuration or random seeds");
        ++runtime.stalledTicks;
        return false;
    }

    const bool rollbackContextChanged = runtime.rollbackSession
        && runtime.rollbackSessionContext != runtime.observedContext;
    if (runtime.rollbackSession
        && (rollbackContextChanged || !PartyBoard_RollbackRenderCanReplayWithoutDraw())) {
        if (!runtime.rollbackSession->reconcile()) {
            failSession("Rollback transition correction failed");
            return false;
        }
        if (runtime.rollbackSession->confirmedFrame()
            != runtime.rollbackSession->currentFrame()) {
            const auto missingFrame = runtime.rollbackBaseFrame
                + runtime.rollbackSession->confirmedFrame();
            if (const auto *old = findInput(runtime.localHistory, missingFrame))
                sendInput(missingFrame, old->input, true);
            ++runtime.stalledTicks;
            ++runtime.consecutiveStalledTicks;
            return false;
        }
        const auto confirmedWireFrame = runtime.rollbackBaseFrame
            + runtime.rollbackSession->confirmedFrame();
        if (!PartyBoard_RollbackAudioBridgeConfirm(confirmedWireFrame)
            || !PartyBoard_RollbackAudioBridgeStop()) {
            failSession("Rollback transition audio teardown failed");
            return false;
        }
        runtime.rollbackSession.reset();
        runtime.rollbackPrepared = false;
        runtime.rollbackSessionContext = -1;
        // A wipe stays in lockstep until it reaches the next context. If the
        // context already changed, the matching-context handshake below is
        // sufficient to arm a fresh checkpoint window safely.
        runtime.rollbackUnavailable = !rollbackContextChanged;
        std::fprintf(stdout,
            "Rollback: transition reached at frame %u; confirmed state retained and lockstep resumed.\n",
            runtime.frame);
    }

    if (!runtime.fullGame && runtime.peerInDifferentContext && runtime.progress.connected()) {
        // The transition input was already agreed on in the previous context.
        // Advertise our current context, release any held buttons and allow a
        // local transition tick. Once both peers report the same context,
        // normal lockstep (including RNG synchronization) resumes.
        const PartyBoardRollbackInput neutral {};
        sendInput(runtime.frame + runtime.inputDelay, neutral);
        PartyBoard_NetplayPadApplyRemote(0, &neutral, &runtime.lastLocal);
        PartyBoard_NetplayPadApplyRemote(1, &neutral, &runtime.lastRemote);
        runtime.lastLocal = neutral;
        runtime.lastRemote = neutral;
        runtime.localCaptured = false;
        runtime.consecutiveStalledTicks = 0;
        runtime.peerTimeoutReported = false;
        return true;
    }

    if (!runtime.localCaptured) {
        runtime.pendingLocal = capturePad(runtime.localPad);
        storeInput(runtime.localHistory, runtime.frame + runtime.inputDelay, runtime.pendingLocal);
        runtime.localCaptured = true;
        // Advertise the captured future sample immediately, including while
        // waiting for the remote current tick. Otherwise both ends can withhold
        // information needed to release the other's delay buffer.
        sendInput(runtime.frame + runtime.inputDelay, runtime.pendingLocal);
    }

    if (!runtime.randomSynchronized || runtime.receivedPackets == 0) {
        sendInput(runtime.frame + runtime.inputDelay, runtime.pendingLocal);
        ++runtime.stalledTicks;
        ++runtime.consecutiveStalledTicks;
        return false;
    }

    if (runtime.rollbackRequested && ensureRollbackSession()) {
        return prepareRollbackTick();
    }

    PartyBoardRollbackInput local {};
    PartyBoardRollbackInput remote {};
    const InputSlot *localSlot = findInput(runtime.localHistory, runtime.frame);
    const InputSlot *remoteSlot = findInput(runtime.remoteHistory, runtime.frame);
    const bool startupFrame = runtime.frame < runtime.inputDelay;
    const bool frameReady = startupFrame || (localSlot != nullptr && remoteSlot != nullptr);
    if (!frameReady) {
        // Once a packet is lost, retransmit the exact frame the peer is also
        // waiting for. The future-frame packet was already sent on capture.
        if (localSlot != nullptr) {
            const auto now = monotonicMs();
            // Bound repair traffic while retrying lost UDP requests.
            if (runtime.repairRequestFrame != runtime.frame
                || now - runtime.lastRepairRequestMs >= 100) {
                sendInput(runtime.frame, localSlot->input, true);
                runtime.repairRequestFrame = runtime.frame;
                runtime.lastRepairRequestMs = now;
            }
        } else {
            sendInput(runtime.frame + runtime.inputDelay, runtime.pendingLocal);
        }
        ++runtime.stalledTicks;
        ++runtime.consecutiveStalledTicks;
        runtime.maximumStalledTicks = std::max(runtime.maximumStalledTicks,
            runtime.consecutiveStalledTicks);
        return false;
    }

    if (!startupFrame && runtime.fullGame) {
        if (localSlot->captureContext != remoteSlot->captureContext) {
            // Module unload/load is driven by wall time outside the synchronized
            // input tick. One machine can therefore report -1 or the next module
            // briefly before the other. Only a persistent difference proves a
            // real divergent game path.
            if (++runtime.contextMismatchFrames >= kContextMismatchGraceFrames) {
                sendInput(runtime.frame, localSlot->input);
                failSession("Les contextes du jeu sont restes differents pendant 2 secondes");
                return false;
            }
        } else {
            runtime.contextMismatchFrames = 0;
        }
    }

    // Repeat the current input as lightweight UDP redundancy. This also lets
    // a host that discovered its peer during the startup buffer provide the
    // earliest delayed frame that the client needs.
    if (!startupFrame && localSlot != nullptr) {
        sendInput(runtime.frame, localSlot->input);
    }
    runtime.consecutiveStalledTicks = 0;
    runtime.repairRequestFrame = UINT32_MAX;
    runtime.peerTimeoutReported = false;
    if (!startupFrame) {
        local = localSlot->input;
        remote = remoteSlot->input;
    }
    const PartyBoardRollbackInput previousRemote = runtime.lastRemote;
    runtime.lastRemote = remote;

    if (runtime.localPlayer == 0) {
        PartyBoard_NetplayPadApplyRemote(0, &local, &runtime.lastLocal);
        PartyBoard_NetplayPadApplyRemote(1, &remote, &previousRemote);
    } else {
        PartyBoard_NetplayPadApplyRemote(1, &local, &runtime.lastLocal);
        PartyBoard_NetplayPadApplyRemote(0, &remote, &previousRemote);
    }
    runtime.lastLocal = local;
    runtime.localCaptured = false;
    ++runtime.frame;
    runtime.progress.commit(monotonicMs());
    // Frame-aligned samples permit comparing live RNG/counters across PCs.
    // Initial handshake seeds alone cannot establish simulation determinism.
    if (runtime.frame % 120 == 0) writeDiagnostic("checkpoint", true);
    return true;
}

extern "C" bool PartyBoard_NetplayCommitTick(void)
{
    using namespace partyboard::netplay;
    Runtime &runtime = gRuntime;
    if (!runtime.rollbackPrepared) return true;
    if (!runtime.rollbackSession
        || !PartyBoard_RollbackAudioBridgeFrameEnd()
        || !runtime.rollbackSession->complete()) {
        failSession("Rollback tick commit failed");
        return false;
    }
    runtime.rollbackPrepared = false;
    const auto confirmedWireFrame = runtime.rollbackBaseFrame
        + runtime.rollbackSession->confirmedFrame();
    if (!PartyBoard_RollbackAudioBridgeConfirm(confirmedWireFrame)) {
        failSession("Rollback confirmed audio failed");
        return false;
    }
    runtime.frame = runtime.rollbackBaseFrame
        + runtime.rollbackSession->currentFrame();
    runtime.localCaptured = false;
    runtime.progress.commit(monotonicMs());
    if (runtime.frame % 120 == 0) writeDiagnostic("rollback-checkpoint", true);
    return true;
}

extern "C" bool PartyBoard_NetplayHasError(void)
{
    return !partyboard::netplay::gRuntime.error.empty();
}

extern "C" bool PartyBoard_NetplayWaiting(void) {
    using namespace partyboard::netplay;
    return gRuntime.enabled && gRuntime.progress.waiting(monotonicMs());
}

extern "C" const char *PartyBoard_NetplayError(void)
{
    return partyboard::netplay::gRuntime.error.c_str();
}

extern "C" void PartyBoard_NetplayShutdown(void)
{
    using namespace partyboard::netplay;
    PartyBoard_RollbackAudioBridgeStop();
    gRuntime.transport.close();
    gRuntime = Runtime {};
}

static bool runNativePadRollbackSelfTest()
{
    // A native subsystem test, deliberately NOT a full-game snapshot: it
    // exercises the same PAD clamp/edge/repeat code used by the main loop,
    // real player statistics and both game RNG clocks. Graphics, processes,
    // audio, heaps and overlays require additional snapshot coverage.
    using namespace partyboard::rollback;
    u32 digest = 0;
    const auto gameBytes = PartyBoard_RollbackGameSize();
    const auto padBytes = HuPadSnapshotSizeGet();
    const auto bytes = gameBytes + padBytes + sizeof(PartyBoardRollbackClock) + sizeof(digest);
    Callbacks callbacks;
    callbacks.saveState = [&](void *destination, std::size_t size) {
        if (size != bytes) return false;
        auto *data = static_cast<u8 *>(destination);
        PartyBoardRollbackClock clock {};
        PartyBoard_RollbackClockSave(&clock);
        std::memcpy(data + gameBytes + padBytes, &clock, sizeof(clock));
        std::memcpy(data + bytes - sizeof(digest), &digest, sizeof(digest));
        return PartyBoard_RollbackGameSave(data, gameBytes)
            && HuPadSnapshotSave(data + gameBytes, padBytes);
    };
    callbacks.loadState = [&](const void *source, std::size_t size) {
        if (size != bytes) return false;
        const auto *data = static_cast<const u8 *>(source);
        PartyBoardRollbackClock clock {};
        std::memcpy(&clock, data + gameBytes + padBytes, sizeof(clock));
        if (clock.version != 1) return false;
        if (!HuPadSnapshotLoad(data + gameBytes, padBytes)
            || !PartyBoard_RollbackGameLoad(data, gameBytes) || !PartyBoard_RollbackClockLoad(&clock)) return false;
        std::memcpy(&digest, data + bytes - sizeof(digest), sizeof(digest));
        return true;
    };
    callbacks.simulateFrame = [&](u32 frame, const auto &inputs) {
        if (!PartyBoard_RollbackApplyPads(inputs.data(), 3))
            throw std::runtime_error("recorded PAD tick failed");
        for (int player = 0; player < 2; ++player) {
            GWCoinsAdd(player, (HuPadBtnDown[player] & PAD_BUTTON_A) ? 3 : -1);
            if (HuPadBtnDown[player] & PAD_BUTTON_B) GWStarsAdd(player, 1);
            digest = digest * 31u + HuPadBtnRep[player] + HuPadDStkRep[player]
                + HuPadBtnDown[player] + static_cast<u8>(HuPadStkX[player])
                + HuPadTrigL[player] + static_cast<u32>(GWCoinsGet(player));
        }
        PartyBoardRollbackClock clock {};
        PartyBoard_RollbackClockSave(&clock);
        // Deterministic updates exercise restoring both RNG states and the
        // frame clock; this does not impersonate the minigame simulation.
        clock.frandSeed = clock.frandSeed * 1664525u + 1013904223u + frame;
        clock.rand8Seed = static_cast<s32>(static_cast<u32>(clock.rand8Seed) * 33u + digest);
        ++clock.globalCounter;
        if (!PartyBoard_RollbackClockLoad(&clock))
            throw std::runtime_error("clock tick failed");
    };
    std::vector<u8> initial(bytes), expected(bytes), actual(bytes);
    if (!callbacks.saveState(initial.data(), bytes)) return false;
    bool passed = true;
    u32 replayed = 0, rollbacks = 0;
    const auto sample = [](u32 frame, unsigned player) {
        PartyBoardRollbackInput input {};
        input.buttons = frame % 48 < 30 ? PAD_BUTTON_A : PAD_BUTTON_B;
        input.stickX = static_cast<s8>(static_cast<int>((frame / 24 + player) % 3) * 70 - 70);
        input.stickY = static_cast<s8>(static_cast<int>(frame % 255) - 127);
        input.substickX = -100; input.substickY = 110;
        input.triggerLeft = frame % 256;
        input.triggerRight = (frame * 7) % 256;
        return input;
    };
    try {
        constexpr u32 frames = 720;
        Session reference({2, 12, bytes}, callbacks);
        for (u32 frame = 0; frame < frames; ++frame) {
            passed &= reference.submitInput(0, frame, sample(frame, 0));
            passed &= reference.submitInput(1, frame, sample(frame, 1));
            passed &= reference.advance();
        }
        passed &= callbacks.saveState(expected.data(), bytes);
        for (const u32 delay : {1u, 3u, 8u, 12u}) {
            passed &= callbacks.loadState(initial.data(), bytes);
            Session delayed({2, 12, bytes}, callbacks);
            for (u32 frame = 0; frame < frames; ++frame) {
                passed &= delayed.submitInput(0, frame, sample(frame, 0));
                if (frame >= delay)
                    passed &= delayed.submitInput(1, frame - delay, sample(frame - delay, 1));
                passed &= delayed.advance();
            }
            for (u32 frame = frames - delay; frame < frames; ++frame)
                passed &= delayed.submitInput(1, frame, sample(frame, 1));
            passed &= delayed.reconcile() && delayed.healthy();
            passed &= callbacks.saveState(actual.data(), bytes) && actual == expected;
            passed &= delayed.stats().rollbackCount > 0 && delayed.stats().maximumRollback <= delay;
            rollbacks += delayed.stats().rollbackCount;
            replayed += delayed.stats().resimulatedFrames;
        }
        // Invalid replay input must not partly change live PAD state.
        passed &= !PartyBoard_RollbackApplyPads(nullptr, 3);
        std::array<PartyBoardRollbackInput, 4> neutral {};
        passed &= !PartyBoard_RollbackApplyPads(neutral.data(), 0x80);
        passed &= callbacks.saveState(actual.data(), bytes) && actual == expected;
    } catch (...) { passed = false; }
    const bool restored = callbacks.loadState(initial.data(), bytes);
    passed &= restored && callbacks.saveState(actual.data(), bytes) && actual == initial;
    OSReport("Native PAD rollback: %s (720 frames, delays 1/3/8/12, %u corrections, %u replayed ticks, %zu bytes/state). Subsystems only.\n",
        passed ? "PASS" : "FAIL", rollbacks, replayed, bytes);
    return passed;
}

extern "C" bool PartyBoard_NetplayRuntimeRunSelfTest(void)
{
    // Exercise the actual availability check used by free_play.c, with both
    // saved unlock bits and the per-PC cheat setting deliberately different.
    partyboard::registerSettings();
    partyboard::config::FinishRegistration();
    using namespace partyboard::netplay;
    const bool enabled = gRuntime.enabled;
    auto &unlock = partyboard::getSettings().game.unlockAllMinigames;
    const auto savedUnlock = unlock;
    const auto savedStat = GWGameStat;
    bool availabilityPassed = true;
    for (const bool online : {false, true}) {
        gRuntime.enabled = online;
        for (const bool cheat : {false, true}) {
            unlock.setValue(cheat);
            for (const u32 savedBits : {0u, 0x55555555u, 0xffffffffu}) {
                GWGameStat.mg_avail[0] = savedBits;
                GWGameStat.mg_avail[1] = ~savedBits;
                for (unsigned i = 0; i < 64; ++i) {
                    const bool savedAvailable = (GWGameStat.mg_avail[i / 32] & (1u << (i % 32))) != 0;
                    availabilityPassed &= (GWMGAvailGet(401 + i) != 0) == (online || cheat || savedAvailable);
                }
                availabilityPassed &= static_cast<u32>(GWGameStat.mg_avail[0]) == savedBits
                    && static_cast<u32>(GWGameStat.mg_avail[1]) == ~savedBits
                    && unlock.getValue() == cheat;
            }
        }
    }
    GWGameStat = savedStat;
    unlock = savedUnlock;
    gRuntime.enabled = enabled;
    OSReport("Netplay minigame availability: %s (768 checks, offline preferences preserved)\n",
        availabilityPassed ? "PASS" : "FAIL");
    const bool topologyPassed = HuPrcSnapshotTopologySelfTest();
    const bool gamePassed = PartyBoard_RollbackGameSelfTest();
    OSReport("Rollback process generation: %s\n", topologyPassed ? "PASS" : "FAIL");
    OSReport("Rollback central game state: %s\n", gamePassed ? "PASS" : "FAIL");
    const bool passed = partyboard::netplay::timelineSelfTest()
        && partyboard::netplay::progressSelfTest() && HuPadSnapshotSelfTest()
        && PartyBoard_RollbackClockSelfTest() && availabilityPassed
        && runNativePadRollbackSelfTest() && topologyPassed && gamePassed
        && PartyBoard_AnimationRollbackSelfTest() && PartyBoard_RollbackIOSelfTest() && PartyBoard_RollbackSequenceSelfTest() && HuPrcSnapshotExecutionSelfTest() && PartyBoard_RollbackSceneSelfTest() && PartyBoard_RollbackResourcesSelfTest() && PartyBoard_RollbackCheckpointSelfTest() && PartyBoard_RollbackWipeSafetySelfTest() && PartyBoard_RollbackRenderSafetySelfTest() && PartyBoard_RollbackAudioSelfTest();
    OSReport("Netplay runtime self-test: %s\n", passed ? "PASS" : "FAIL");
    return passed;
}

extern "C" bool PartyBoard_NetplayPadRunProbe(void)
{
    using namespace partyboard::netplay;
    if (!gRuntime.enabled || !gRuntime.fullGame) return false;
    // Two real processes exercise transport -> delay buffer -> raw PAD routing.
    // Synthetic inputs do not launch graphics or read/write a save file.
    // Long enough to expose relay queues that grow slowly under sustained play.
    constexpr unsigned frames = 2400;
    const auto sample = [](unsigned player, unsigned frame) {
        PartyBoardRollbackInput input {};
        input.buttons = (frame % 40 < 28) ? (player == 0 ? PAD_BUTTON_A : PAD_BUTTON_B) : 0;
        input.stickX = static_cast<s8>(static_cast<int>((frame + player * 17) % 161) - 80);
        input.stickY = static_cast<s8>(player == 0 ? 71 : -63);
        input.substickX = -39;
        input.substickY = 58;
        input.triggerLeft = static_cast<u8>(frame % 256);
        input.triggerRight = static_cast<u8>((frame * 3) % 256);
        return input;
    };
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(gRuntime.disconnectProbe ? 135 : gRuntime.realtimeProbe ? 150 : 90);
    unsigned frame = 0;
    bool loadingPauseDone = false;
    while (frame < frames && std::chrono::steady_clock::now() < deadline) {
        // Revisit a context with inputs still in the delay buffer. The client
        // pauses alone to model wall-clock loading without extra simulation.
        gRuntime.probeContext = frame < 100 ? 0 : frame < 200 ? 7 : frame < 400 ? 3 : 7;
        if (gRuntime.contextMismatchProbe && gRuntime.localPlayer == 1 && frame >= 100)
            gRuntime.probeContext = 9;
        if (!loadingPauseDone && frame == 200 && gRuntime.localPlayer == 1) {
            loadingPauseDone = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(12000));
        }
        if (gRuntime.disconnectProbe && gRuntime.localPlayer == 1 && frame == 60) {
            // Abrupt UDP loss: no DISCONNECT packet and no normal shutdown.
            gRuntime.transport.close();
            std::puts("[NET TEST] client transport dropped at frame 60");
            return true;
        }
        PADStatus pads[4] {};
        for (auto& pad : pads) pad.err = PAD_ERR_NO_CONTROLLER;
        pads[gRuntime.localPad] = padFromInput(sample(gRuntime.localPlayer, frame));
        u32 rumble = 0;
        if (PartyBoard_NetplayPreparePads(pads, &rumble, false)) {
            if (gRuntime.frame != frame + 1) {
                std::fprintf(stderr, "[NET TEST] FAIL: timeline reset or uncommitted transition at frame %u\n", frame);
                return false;
            }
            for (unsigned player = 0; player < 2; ++player) {
                const PartyBoardRollbackInput expected = frame < gRuntime.inputDelay
                    ? PartyBoardRollbackInput {} : sample(player, frame - gRuntime.inputDelay);
                if (pads[player].err != PAD_ERR_NONE
                    || !partyboard::rollback::inputsEqual(inputFromPad(pads[player]), expected)) {
                    std::fprintf(stderr, "[NET TEST] FAIL frame=%u port=%u\n", frame, player + 1);
                    return false;
                }
            }
            if (pads[2].err != PAD_ERR_NO_CONTROLLER || pads[3].err != PAD_ERR_NO_CONTROLLER) return false;
            ++frame;
        }
        if (PartyBoard_NetplayHasError()) {
            if (gRuntime.contextMismatchProbe) {
                const auto expected = 100u + gRuntime.inputDelay
                    + kContextMismatchGraceFrames - 1u;
                const bool passed = frame >= expected && frame <= expected + 2u
                    && gRuntime.error == "Les contextes du jeu sont restes differents pendant 2 secondes";
                std::printf("[NET TEST] %s: context divergence stopped at frame %u\n", passed ? "PASS" : "FAIL", frame);
                return passed;
            }
            if (!gRuntime.disconnectProbe || gRuntime.localPlayer != 0 || frame < 60
                || frame > 60u + gRuntime.inputDelay + 1u
                || gRuntime.progress.check(monotonicMs()) != ProgressFailure::Stalled) return false;
            const auto stoppedFrame = gRuntime.frame;
            for (unsigned retry = 0; retry < 100; ++retry) {
                if (PartyBoard_NetplayPreparePads(pads, &rumble, false)
                    || gRuntime.frame != stoppedFrame || !PartyBoard_NetplayEnabled()) return false;
            }
            std::printf("[NET TEST] PASS: peer loss detected, terminal stop at frame %u, no offline fallback\n", frame);
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(gRuntime.realtimeProbe ? 16 : 1));
    }
    std::fprintf(stdout, "[NET TEST] %s: %u/%u frames, local physical PAD=%u, game port=%u\n",
        frame == frames ? "PASS" : "FAIL", frame, frames, gRuntime.localPad + 1, gRuntime.localPlayer + 1);
    return frame == frames;
}
