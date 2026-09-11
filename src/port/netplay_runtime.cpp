#include "port/netplay_runtime.h"

#include "dolphin/os.h"
#include "dolphin/pad.h"
#include "port/netplay_transport.hpp"
#include "port/netplay_pad.hpp"
#include "port/netplay_progress.hpp"
#include "port/netplay_canonical.hpp"
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
#include "port/crash_report.h"
#include "port/config.hpp"
#include <filesystem> // port/main.h declares a std::filesystem::path global.
#include "port/main.h"
#include "port/port_version.h"
#include "partyboard_version.h"

extern "C" {
#include "game/gamework.h"
#include "game/gamework_data.h"
#include "game/pad.h"
}

#include <SDL3/SDL_timer.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
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
extern "C" u32 frand(void);
extern "C" s32 rand8(void);
extern "C" void BoardRandInit(void);
extern "C" u32 BoardRand(void);
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
extern "C" BOOL msmStreamLogicalSelfTest(void);
extern "C" BOOL PartyBoard_RetraceCounterSelfTest(void);
extern "C" BOOL PartyBoard_ThpLogicalSelfTest(void);
extern "C" s32 msmMusGetStatus(int musNo);
extern "C" void msmStreamLogicalTick(void);
extern "C" bool msmStreamLogicalProbeInstall(void);
extern "C" void msmStreamLogicalProbeStart(int channel, int samples, int frequency);
extern "C" void msmStreamLogicalProbeFinishPhysical(int channel);
extern "C" s32 msmStreamGetStatus(int streamNo);

namespace partyboard::netplay {
namespace {

constexpr std::uint32_t kSessionId = 0x4d503452u; // "MP4R"
constexpr std::uint32_t kRuntimeConfigMagic = 0x4e500000u; // "NP" + delay
constexpr std::uint32_t kRuntimeFullGameFlag = 0x00010000u;
constexpr std::uint32_t kRuntimeRollbackFlag = 0x00020000u;
constexpr std::uint32_t kRuntimeConfigMagicMask = 0xfffc0000u;
constexpr std::size_t kHistorySize = 256;
constexpr std::uint8_t kDefaultInputDelay = 3;
constexpr std::uint8_t kMaximumInputDelay = 8;
constexpr std::uint32_t kContextMismatchGraceFrames = 120;
enum class MenuProbeOverlay {
#define DLL(name) name,
#include "ovl_table.h"
#undef DLL
};

struct InputSlot {
    std::uint32_t frame = 0;
    PartyBoardRollbackInput input {};
    bool valid = false;
    std::uint32_t captureContext = 0;
};

struct Runtime {
    UdpTransport transport;
    SessionProgress progress;
    StateHistory states;
    // Field-level evidence for the frames a divergence can still be reported
    // at. Digests cover the full 256-frame stream; only the detail is shorter.
    std::array<CanonicalState, kDetailHistorySize> detailHistory {};
    std::array<std::uint32_t, kDetailHistorySize> detailFrames {};
    bool lockstepPrepared = false;
    bool desyncProbe = false;
    bool menuProbe = false;
    std::uint64_t lastStateRepairMs = 0;
    std::uint32_t stateRepairCursor = 0;
    std::string error;
    bool disconnectProbe = false;
    bool contextMismatchProbe = false;
    bool realtimeProbe = false;
    bool audioProbe = false;
    bool walkProbe = false;
    // Input recording and replay. A recording is raw pad samples keyed by the
    // frame they are APPLIED on, so the same file replays correctly at any
    // input delay. Both seats are stored, and each peer replays its own.
    std::string recordPath;
    std::string replayPath;
    std::FILE *recordFile = nullptr;
    std::vector<std::array<PartyBoardRollbackInput, 2>> replaySamples;
    std::vector<bool> replayPresent;
    std::uint32_t replayExhaustedFrame = UINT32_MAX;
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

void serviceStateRepair(bool force = false);

void failSession(const char *reason, bool retainTransport = false)
{
    if (!gRuntime.error.empty()) return;
    gRuntime.error = reason;
    writeDiagnostic(reason, true);
    std::fprintf(stderr, "[NET] ERROR frame=%u: %s. Simulation stopped; close the game to restart the session.\n",
        gRuntime.frame, reason);
    PADControlMotor(gRuntime.localPad, PAD_MOTOR_STOP_HARD);
    if (!retainTransport) gRuntime.transport.close();
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
    gRuntime.states = {};
    for (auto &state : gRuntime.detailHistory) state.reset();
    gRuntime.detailFrames.fill(kNoHashFrame);
    // A network timeline owns its own RNG consumption baseline, exactly like
    // the seeds it agrees on: counters must start at zero on both peers.
    PartyBoard_NetplayRandomCountersReset();
    gRuntime.lockstepPrepared = false;
    gRuntime.lastStateRepairMs = 0;
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

// ---------------------------------------------------------------------------
// Forced rollback probe
//
// SAVE at frame F, let the game run D frames, SAVE again, RESTORE F, replay the
// same D frames, and require the canonical state to come back identical. It is
// the local, deterministic half of rollback: no network, no prediction, no
// remote peer. If a frame cannot be reproduced from a snapshot plus its inputs
// on one machine, no amount of network machinery will make it reproducible on
// two.
//
// The experiment is a side excursion and it is undone. A second snapshot is
// taken at the target frame and restored at the end, so the forward timeline
// continues exactly where it was and two peers running the same probe stay in
// step whatever the result. Nothing is repaired, resynchronized or hidden: a
// failure writes a report naming the first divergent subsystem and field, and
// stops the session.
//
//   PARTYBOARD_FORCE_ROLLBACK=<period>[:<distance,distance,...>]
//
// The distance ladder exists because a one-frame rollback proves very little:
// state the snapshot forgets often only matters once enough frames have been
// re-simulated. Default ladder 1,2,4,8,15,30,60,120, rotated so successive
// tests use different distances.
// ---------------------------------------------------------------------------

struct ForceRollbackProbe {
    bool configured = false;
    std::uint32_t period = 0;
    std::vector<std::uint32_t> distances;
    std::size_t ladderIndex = 0;

    bool recording = false;
    std::uint32_t saveFrame = 0;
    std::uint32_t targetFrame = 0;
    std::vector<std::uint8_t> before;
    std::vector<std::uint8_t> after;
    std::vector<std::array<PartyBoardRollbackInput, rollback::kMaxPlayers>> inputs;
    CanonicalState expected;

    std::uint32_t attempted = 0;
    std::uint32_t refused = 0;
    std::uint32_t passed = 0;
    std::uint32_t failed = 0;
    bool stopped = false;
};

ForceRollbackProbe gForceRollback;

void configureForceRollback()
{
    if (gForceRollback.configured) return;
    gForceRollback.configured = true;
    const char *value = std::getenv("PARTYBOARD_FORCE_ROLLBACK");
    if (value == nullptr || value[0] == 0) return;

    const std::string text(value);
    const auto colon = text.find(':');
    const std::string periodText = colon == std::string::npos ? text : text.substr(0, colon);
    gForceRollback.period = static_cast<std::uint32_t>(std::strtoul(periodText.c_str(), nullptr, 10));
    if (gForceRollback.period == 0) return;

    if (colon != std::string::npos) {
        std::size_t start = colon + 1;
        while (start < text.size()) {
            const auto comma = text.find(',', start);
            const auto piece =
                text.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
            const auto distance = static_cast<std::uint32_t>(std::strtoul(piece.c_str(), nullptr, 10));
            if (distance > 0) gForceRollback.distances.push_back(distance);
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
    }
    if (gForceRollback.distances.empty()) {
        gForceRollback.distances = {1, 2, 4, 8, 15, 30, 60, 120};
    }
    char event[160];
    std::snprintf(event, sizeof(event), "force-rollback armed period=%u distances=%zu",
        gForceRollback.period, gForceRollback.distances.size());
    writeDiagnostic(event, true);
}

// Progress is reported on every outcome, not only on failure: a probe that
// silently refuses every test looks exactly like a probe that silently passes
// every test, and the two must never be confusable. writeDiagnostic throttles
// the repeats, so this stays one line every couple of seconds.
void reportForceRollbackProgress(std::uint32_t distance, const char *outcome,
    std::uint32_t atFrame = 0)
{
    const ForceRollbackProbe &probe = gForceRollback;
    char event[224];
    // A refusal happens at the frame it was refused on, not at the last frame a
    // test was armed on; reporting the stale saveFrame made twenty refusals all
    // look like they happened at frame 900.
    std::snprintf(event, sizeof(event),
        "force-rollback %s frame=%u distance=%u attempted=%u passed=%u failed=%u refused=%u",
        outcome, atFrame != 0 ? atFrame : probe.saveFrame, distance, probe.attempted,
        probe.passed, probe.failed, probe.refused);
    // writeDiagnostic throttles unforced lines to one every two seconds across
    // every diagnostic the runtime writes, and the netplay checkpoint line takes
    // that slot, so an unforced line is an invisible line.
    //
    // Every OUTCOME is therefore written: a pass, a failure, a refusal. Only the
    // arming, which happens once per test and says nothing on its own, is
    // throttled to every tenth.
    //
    // An earlier version forced on `attempted % 10`, which never fired: a test
    // refused before the comparison does not increment `attempted`, so a probe
    // refusing every test sat at attempted=1 and said nothing at all. Silence
    // from a probe has to mean the probe is silent, not that it stopped.
    const bool arming = std::strcmp(outcome, "armed") == 0;
    const bool always = !arming || ((probe.attempted + probe.refused) % 10) == 0;
    writeDiagnostic(event, always);
}

std::array<PartyBoardRollbackInput, rollback::kMaxPlayers> currentAppliedInputs()
{
    std::array<PartyBoardRollbackInput, rollback::kMaxPlayers> inputs {};
    for (std::size_t player = 0; player < rollback::kMaxPlayers; ++player) {
        inputs[player] = inputFromPad(gSynchronizedPads[player]);
    }
    return inputs;
}

CanonicalState captureCanonicalNow(std::uint32_t frame)
{
    const StateDigest stamp {frame,
        static_cast<std::uint32_t>(gRuntime.probeContext >= 0 ? gRuntime.probeContext
                                                             : PartyBoard_NetplayContextId()),
        frand_state_get(), static_cast<std::uint32_t>(rand8_state_get()), GlobalCounter};
    return captureCanonical(stamp);
}

// Names the first field whose value differs. Both captures walk the same code,
// so a length difference is itself a finding and is reported as one.
void describeFirstDivergence(const CanonicalState &expected, const CanonicalState &actual,
    std::string &subsystem, std::string &field, std::uint32_t &expectedValue,
    std::uint32_t &actualValue)
{
    subsystem = "NONE";
    field = "none";
    expectedValue = 0;
    actualValue = 0;
    if (expected.values.size() != actual.values.size()) {
        subsystem = "SHAPE";
        field = "field count";
        expectedValue = static_cast<std::uint32_t>(expected.values.size());
        actualValue = static_cast<std::uint32_t>(actual.values.size());
        return;
    }
    for (std::size_t index = 0; index < expected.values.size(); ++index) {
        if (expected.values[index] == actual.values[index]) continue;
        subsystem = subsystemName(expected.subsystemOf(index));
        field = expected.names[index] != nullptr ? expected.names[index] : "?";
        expectedValue = expected.values[index];
        actualValue = actual.values[index];
        return;
    }
}

// Reports land beside the rest of a session's evidence: the crash directory the
// launcher chose, or failing that the directory of the netplay diagnostic.
std::string rollbackReportDirectory()
{
    if (const char *directory = std::getenv("PARTYBOARD_CRASH_DIR")) {
        if (directory[0] != 0) return directory;
    }
    if (const char *diagnostic = std::getenv("PARTYBOARD_NET_DIAGNOSTIC")) {
        std::string text(diagnostic);
        const auto cut = text.find_last_of("/\\");
        if (cut != std::string::npos) return text.substr(0, cut);
    }
    return ".";
}

void writeRollbackFailureReport(const CanonicalState &expected, const CanonicalState &actual,
    std::uint32_t distance)
{
    std::string subsystem, field;
    std::uint32_t expectedValue = 0, actualValue = 0;
    describeFirstDivergence(expected, actual, subsystem, field, expectedValue, actualValue);

    char name[256];
    std::snprintf(name, sizeof(name), "rollback-failure-peer-%u-frame-%u-distance-%u.txt",
        gRuntime.localPlayer, gForceRollback.saveFrame, distance);
    std::string path = rollbackReportDirectory();
    if (!path.empty()) path += "/";
    path += name;

    if (FILE *file = std::fopen(path.c_str(), "w")) {
        std::fprintf(file, "PARTYBOARD_ROLLBACK_FAILURE version=1\n");
        std::fprintf(file, "peer=%u role=%s\n", gRuntime.localPlayer,
            gRuntime.localPlayer == 0 ? "host" : "client");
        std::fprintf(file, "save_frame=%u\n", gForceRollback.saveFrame);
        std::fprintf(file, "target_frame=%u\n", gForceRollback.targetFrame);
        std::fprintf(file, "replay_length=%u\n", distance);
        std::fprintf(file, "snapshot_bytes=%zu\n", gForceRollback.before.size());
        std::fprintf(file, "first_divergent_subsystem=%s\n", subsystem.c_str());
        std::fprintf(file, "first_divergent_field=%s\n", field.c_str());
        std::fprintf(file, "expected_value=0x%08x actual_value=0x%08x\n", expectedValue,
            actualValue);
        std::fprintf(file, "expected_hash=%016llx actual_hash=%016llx\n",
            static_cast<unsigned long long>(expected.hash),
            static_cast<unsigned long long>(actual.hash));
        std::fprintf(file, "\n[SUBSYSTEM HASHES]\n");
        for (std::size_t index = 0; index < kSubsystemCount; ++index) {
            std::fprintf(file, "%-10s expected=%08x actual=%08x %s\n", subsystemName(index),
                expected.parts[index], actual.parts[index],
                expected.parts[index] == actual.parts[index] ? "" : "<-- DIVERGENT");
        }
        std::fprintf(file, "\n[CONTEXT]\n");
        std::fprintf(file, "game_context=%d minigame=%d\n", PartyBoard_NetplayContextId(),
            PartyBoard_NetplayMinigameId());
        std::fprintf(file, "rng frand=%08x rand8=%08x boardrand=%08x\n", frand_state_get(),
            static_cast<unsigned>(rand8_state_get()), boardRandSeed);
        std::fprintf(file, "global_counter=%u\n", GlobalCounter);
        std::fprintf(file, "audio_bridge_active=%d healthy=%d\n",
            PartyBoard_RollbackAudioBridgeActive() ? 1 : 0,
            PartyBoard_RollbackAudioBridgeHealthy() ? 1 : 0);
        std::fprintf(file, "\n[DIVERGENT FIELDS - first 64]\n");
        std::size_t shown = 0;
        const std::size_t count = std::min(expected.values.size(), actual.values.size());
        for (std::size_t index = 0; index < count && shown < 64; ++index) {
            if (expected.values[index] == actual.values[index]) continue;
            std::fprintf(file, "%-10s %-40s expected=0x%08x actual=0x%08x\n",
                subsystemName(expected.subsystemOf(index)),
                expected.names[index] != nullptr ? expected.names[index] : "?",
                expected.values[index], actual.values[index]);
            ++shown;
        }
        std::fclose(file);
    }

    char event[256];
    std::snprintf(event, sizeof(event),
        "ROLLBACK-FAILURE save_frame=%u target_frame=%u distance=%u subsystem=%s field=%s",
        gForceRollback.saveFrame, gForceRollback.targetFrame, distance, subsystem.c_str(),
        field.c_str());
    writeDiagnostic(event, true);
    PartyBoard_CrashBreadcrumb(PARTYBOARD_CRASH_CAT_WARN, "%s", event);
}

// Runs the restore and the replay, then puts the game back where it was.
void runForceRollbackComparison(std::uint32_t frame)
{
    ForceRollbackProbe &probe = gForceRollback;
    const auto distance = static_cast<std::uint32_t>(probe.inputs.size());

    const auto bytes = PartyBoard_RollbackCheckpointSize();
    probe.after.assign(bytes, 0);
    if (bytes == 0 || !PartyBoard_RollbackCheckpointSave(probe.after.data(), bytes)) {
        // The forward snapshot is what makes the excursion undoable. Without it
        // the experiment is abandoned rather than run: leaving the game on a
        // replayed timeline would be a change nobody asked for.
        ++probe.refused;
        probe.recording = false;
        reportForceRollbackProgress(distance, "refused-forward-save-failed");
        return;
    }

    probe.expected = captureCanonicalNow(frame);
    ++probe.attempted;

    const char *stage = "ok";
    bool ok = PartyBoard_RollbackCheckpointLoad(probe.before.data(), probe.before.size());
    if (!ok) stage = "restore";
    bool startedBridge = false;
    if (ok && !PartyBoard_RollbackAudioBridgeActive()) {
        // The queue only accepts frames from the one the bridge was started on
        // upwards, and the first tick replayed here is the logic for saveFrame+1
        // because the snapshot was taken after the logic for saveFrame.
        startedBridge = PartyBoard_RollbackAudioBridgeStartAtFrame(probe.saveFrame + 1);
        ok = startedBridge;
        if (!ok) stage = "audio-bridge-start";
    }
    if (ok) {
        for (std::uint32_t step = 0; step < distance && ok; ++step) {
            const auto replayFrame = probe.saveFrame + 1 + step;
            if (!PartyBoard_RollbackAudioBridgeFrameBegin(replayFrame)) {
                ok = false;
                stage = "audio-frame-begin";
            } else if (!PartyBoard_RollbackRunGameLogicTick(probe.inputs[step].data(), 3)) {
                ok = false;
                stage = "game-tick";
            } else {
                // The rendered path publishes this after the frame; a replayed
                // tick has no presentation pass, so it is published here,
                // exactly as the real rollback replay does.
                ++GlobalCounter;
            }
            if (!PartyBoard_RollbackAudioBridgeFrameEnd() && ok) {
                ok = false;
                stage = "audio-frame-end";
            }
        }
    }

    if (ok) {
        const auto actual = captureCanonicalNow(frame);
        if (actual.hash == probe.expected.hash && actual.values == probe.expected.values) {
            ++probe.passed;
            reportForceRollbackProgress(distance, "pass");
        } else {
            ++probe.failed;
            writeRollbackFailureReport(probe.expected, actual, distance);
        }
    } else {
        ++probe.refused;
        char outcome[64];
        std::snprintf(outcome, sizeof(outcome), "refused-at-%s", stage);
        reportForceRollbackProgress(distance, outcome);
    }

    // Back to the real timeline, whatever happened above.
    const bool restored =
        PartyBoard_RollbackCheckpointLoad(probe.after.data(), probe.after.size());
    if (startedBridge) PartyBoard_RollbackAudioBridgeStop();
    probe.recording = false;
    probe.ladderIndex = (probe.ladderIndex + 1) % probe.distances.size();

    if (!restored) {
        probe.stopped = true;
        failSession("Forced rollback probe could not restore the live timeline");
        return;
    }
    if (probe.failed != 0) {
        // No repair, no resynchronization, no continuing as if nothing happened.
        probe.stopped = true;
        failSession("Forced rollback replay did not reproduce the state");
    }
}

void forceRollbackTick(std::uint32_t frame)
{
    configureForceRollback();
    ForceRollbackProbe &probe = gForceRollback;
    if (probe.period == 0 || probe.stopped) return;
    // The replay below runs game logic. If anything in there ever reached this
    // point again the probe would be measuring itself, so it is refused rather
    // than nested.
    static bool inside = false;
    if (inside) return;
    struct Guard {
        bool &flag;
        explicit Guard(bool &f) : flag(f) { flag = true; }
        ~Guard() { flag = false; }
    } guard(inside);

    if (probe.recording) {
        probe.inputs.push_back(currentAppliedInputs());
        if (frame >= probe.targetFrame) runForceRollbackComparison(frame);
        return;
    }

    if (frame == 0 || (frame % probe.period) != 0) return;
    const auto bytes = PartyBoard_RollbackCheckpointSize();
    if (bytes == 0) {
        // Not a safe boundary: a wipe, a render callback, an I/O operation or a
        // module transition is in flight. Refused and counted, never forced.
        ++probe.refused;
        char outcome[64];
        std::snprintf(outcome, sizeof(outcome), "refused-%s",
            PartyBoard_RollbackCheckpointRefusal());
        reportForceRollbackProgress(0, outcome, frame);
        return;
    }
    probe.before.assign(bytes, 0);
    if (!PartyBoard_RollbackCheckpointSave(probe.before.data(), bytes)) {
        ++probe.refused;
        reportForceRollbackProgress(0, "refused-save-failed", frame);
        return;
    }
    probe.saveFrame = frame;
    probe.targetFrame = frame + probe.distances[probe.ladderIndex];
    reportForceRollbackProgress(probe.distances[probe.ladderIndex], "armed");
    probe.inputs.clear();
    probe.recording = true;
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

bool sendInput(std::uint32_t frame, const PartyBoardRollbackInput &input, bool requestRetransmit = false,
    const StateDigest *standalone = nullptr)
{
    // Headless regression: drop this sample until the sender has advanced.
    // Only the retained-history repair can then release the waiting peer.
    if (!standalone && gRuntime.probeContext >= 0 && gRuntime.localPlayer == 0
        && frame == 200 && gRuntime.frame <= 200) return true;
    InputPacket outgoing {};
    outgoing.sessionId = kSessionId;
    outgoing.sequence = gRuntime.sequence++;
    outgoing.frame = frame;
    outgoing.player = gRuntime.localPlayer;
    outgoing.input = input;
    outgoing.type = standalone ? PacketType::State
        : requestRetransmit ? PacketType::Retransmit : PacketType::Input;
    if (!gRuntime.rollbackRequested) {
        outgoing.hashAckNext = gRuntime.states.equalThrough();
        const auto *state = standalone ? standalone : gRuntime.states.pending();
        if (state) outgoing.state = *state;
    }
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

void serviceStateRepair(bool force)
{
    if (gRuntime.rollbackRequested) return;
    const auto now = monotonicMs();
    if (!force && now - gRuntime.lastStateRepairMs < 100) return;
    const auto *state = gRuntime.states.error() != StateFailure::None
        ? gRuntime.states.getLocal(gRuntime.states.errorFrame()) : gRuntime.states.pending();
    if (!state && gRuntime.states.captured())
        state = gRuntime.states.getLocal(gRuntime.states.captured() - 1);
    if (state) {
        sendInput(state->frame, {}, false, state);
        if (gRuntime.states.error() == StateFailure::Desync) {
            // The peer may still lack an earlier local hash. Cycle the retained
            // prefix so even loss before the mismatch cannot hide it forever.
            auto &cursor = gRuntime.stateRepairCursor;
            if (cursor < gRuntime.states.peerEqualThrough() || cursor > state->frame)
                cursor = gRuntime.states.peerEqualThrough();
            if (const auto *old = gRuntime.states.getLocal(cursor++))
                if (old->frame != state->frame) sendInput(old->frame, {}, false, old);
        }
        gRuntime.lastStateRepairMs = now;
    }
}

#include "netplay_report.inc"

// A recording is plain text so it can be diffed and read by a human:
//   frame seat buttons stickX stickY substickX substickY triggerL triggerR
// The frame is the one the sample is APPLIED on, never the one it was captured
// on, so a recording made at one input delay replays correctly at another.
bool loadReplaySamples()
{
    std::FILE *file = std::fopen(gRuntime.replayPath.c_str(), "rb");
    if (!file) return false;
    unsigned frame = 0, seat = 0, buttons = 0, triggerL = 0, triggerR = 0;
    int stickX = 0, stickY = 0, substickX = 0, substickY = 0;
    while (std::fscanf(file, "%u %u %u %d %d %d %d %u %u", &frame, &seat, &buttons,
        &stickX, &stickY, &substickX, &substickY, &triggerL, &triggerR) == 9) {
        if (seat > 1 || frame > 60u * 60u * 60u) continue; // Ignore malformed rows.
        if (frame >= gRuntime.replaySamples.size()) {
            gRuntime.replaySamples.resize(frame + 1);
            gRuntime.replayPresent.resize(frame + 1, false);
        }
        PartyBoardRollbackInput &sample = gRuntime.replaySamples[frame][seat];
        sample.buttons = static_cast<u16>(buttons);
        sample.stickX = static_cast<s8>(stickX);
        sample.stickY = static_cast<s8>(stickY);
        sample.substickX = static_cast<s8>(substickX);
        sample.substickY = static_cast<s8>(substickY);
        sample.triggerLeft = static_cast<u8>(triggerL);
        sample.triggerRight = static_cast<u8>(triggerR);
        gRuntime.replayPresent[frame] = true;
    }
    std::fclose(file);
    return !gRuntime.replaySamples.empty();
}

void recordAppliedInputs(std::uint32_t frame, const PartyBoardRollbackInput &local,
    const PartyBoardRollbackInput &remote)
{
    if (!gRuntime.recordFile) return;
    const PartyBoardRollbackInput *seats[2];
    seats[gRuntime.localPlayer] = &local;
    seats[gRuntime.localPlayer ^ 1u] = &remote;
    for (unsigned seat = 0; seat < 2; ++seat) {
        std::fprintf(gRuntime.recordFile, "%u %u %u %d %d %d %d %u %u\n", frame, seat,
            seats[seat]->buttons, seats[seat]->stickX, seats[seat]->stickY,
            seats[seat]->substickX, seats[seat]->substickY,
            seats[seat]->triggerLeft, seats[seat]->triggerRight);
    }
}

bool checkStateFailure()
{
    if (gRuntime.states.error() == StateFailure::None) return true;
    if (!gRuntime.error.empty()) return false;
    const auto frame = gRuntime.states.errorFrame();
    const auto *a = gRuntime.states.getLocal(frame), *b = gRuntime.states.getRemote(frame);
    const char *category = a && b ? subsystemName(a->firstDifferentPart(*b)) : "UNKNOWN";
    // One self-contained file per peer; tools/netplay_compare.py diffs the two
    // and names the first divergent field.
    const auto report = writeDesyncReport(frame, gRuntime.states.error());
    char message[1024];
    std::snprintf(message, sizeof(message),
        "%s frame=%u category=%s context=%u/%u localHash=%016llx remoteHash=%016llx RNG=%08x/%08x:%08x/%08x counter=%u/%u hash_version=%u confirmed_input=%u last_equal_next=%u state_error=%u session=%08x player=%u report=%s",
        gRuntime.states.error() == StateFailure::Desync ? "DESYNC" : "PROTOCOL_STATE",
        frame, category, a ? a->context : UINT32_MAX, b ? b->context : UINT32_MAX,
        static_cast<unsigned long long>(a ? a->hash : 0), static_cast<unsigned long long>(b ? b->hash : 0),
        a ? a->frand : 0, a ? a->rand8 : 0, b ? b->frand : 0, b ? b->rand8 : 0,
        a ? a->counter : 0, b ? b->counter : 0, kStateHashVersion,
        gRuntime.frame ? gRuntime.frame - 1 : UINT32_MAX, gRuntime.states.equalThrough(),
        static_cast<unsigned>(gRuntime.states.error()), kSessionId, gRuntime.localPlayer,
        report.empty() ? "none" : report.c_str());
    // The companion collects this sidecar next to its native log; keep writing
    // it. The per-peer report above holds the field-level evidence, which is
    // too large to flood stderr or the capped routine log with.
    FILE *sidecar = nullptr;
#ifdef _WIN32
    const auto *diagnostic = _wgetenv(L"PARTYBOARD_NET_DIAGNOSTIC");
    if (diagnostic && *diagnostic)
        sidecar = _wfopen((std::wstring(diagnostic) + L".desync").c_str(), L"ab");
#endif
    if (sidecar) std::fprintf(sidecar, "%s\n", message);
    if (a && b) {
        for (std::size_t index = 0; index < kSubsystemCount; ++index) {
            const char *verdict = a->parts[index] == b->parts[index] ? "OK" : "DIFFERENT";
            std::fprintf(stderr, "[STATE] frame=%u %-10s local=%08x remote=%08x %s\n", frame,
                subsystemName(index), a->parts[index], b->parts[index], verdict);
            if (sidecar)
                std::fprintf(sidecar, "SUBSYSTEM frame=%u %-10s local=%08x remote=%08x %s\n",
                    frame, subsystemName(index), a->parts[index], b->parts[index], verdict);
        }
    }
    if (sidecar) std::fclose(sidecar);
    serviceStateRepair(true);
    failSession(message, gRuntime.states.error() == StateFailure::Desync); // Retry failed digest while stopped.
    return false;
}

// Publish the facts a crash report needs, once per accepted simulation tick.
// Everything here is a value the game thread already owns, so the cost is a
// struct copy; the point is that a termination which cannot be intercepted
// in-process still leaves the frame, overlay and network state on disk.
void publishCrashState(const StateDigest &stamp)
{
    static std::int32_t previousOverlay = -1;
    static std::int32_t lastOverlay = -1;
    static std::uint32_t overlayTransitionFrame = 0;

    PartyBoardCrashSimState state {};
    state.simulationFrame = gRuntime.frame;
    state.networkFrame = gRuntime.lastWireFrame;
    state.gameContext = gRuntime.observedContext;
    state.overlay = PartyBoard_NetplayContextId();
    state.minigame = PartyBoard_NetplayMinigameId();
    if (state.overlay != lastOverlay) {
        previousOverlay = lastOverlay;
        lastOverlay = state.overlay;
        overlayTransitionFrame = gRuntime.frame;
    }
    state.overlayPrevious = previousOverlay;
    state.overlayTransitionFrame = overlayTransitionFrame;

    state.lastStateHash = stamp.hash;
    state.lastStateHashFrame = stamp.frame;

    state.frand = frand_state_get();
    state.rand8 = static_cast<u32>(rand8_state_get());
    state.boardRand = boardRandSeed;
    state.frandCalls = PartyBoard_NetplayFrandCalls();
    state.rand8Calls = PartyBoard_NetplayRand8Calls();
    state.boardRandCalls = PartyBoard_NetplayBoardRandCalls();

    state.received = gRuntime.receivedPackets;
    state.rejected = gRuntime.rejectedPackets;
    state.repaired = gRuntime.repairPackets;
    state.sendErrors = gRuntime.sendFailures;
    const auto now = monotonicMs();
    state.packetAgeMs = static_cast<u32>(gRuntime.lastPacketMs ? now - gRuntime.lastPacketMs : 0);
    state.stalledTicks = gRuntime.stalledTicks;
    state.maximumStalledTicks = gRuntime.maximumStalledTicks;
    state.txSequence = gRuntime.sequence;
    state.randomSynchronized = gRuntime.randomSynchronized;
    state.configMismatch = gRuntime.configMismatch;
    state.contextMismatchFrames = gRuntime.contextMismatchFrames;

    const auto &local = gRuntime.localHistory[gRuntime.frame % kHistorySize];
    const auto &remote = gRuntime.remoteHistory[gRuntime.frame % kHistorySize];
    state.localReady = local.valid && local.frame == gRuntime.frame;
    state.remoteReady = remote.valid && remote.frame == gRuntime.frame;
    state.localButtons = local.input.buttons;
    state.remoteButtons = remote.input.buttons;
    state.localStickX = local.input.stickX;
    state.localStickY = local.input.stickY;
    state.remoteStickX = remote.input.stickX;
    state.remoteStickY = remote.input.stickY;

    if (gRuntime.rollbackSession) {
        const auto stats = gRuntime.rollbackSession->stats();
        state.rollbackActive = 1;
        state.rollbackCount = stats.rollbackCount;
        state.rollbackReplayed = stats.resimulatedFrames;
        state.rollbackPredicted = stats.predictedFrames;
    }

    // Audio thread values: diagnostic only, never hashed. See C5/C6 in
    // docs/NETPLAY_DETERMINISM_AUDIT.md.
    for (int channel = 0; channel < 4; ++channel)
        state.musStatus[channel] = msmMusGetStatus(channel);

    PartyBoard_CrashUpdateSimState(&state);
    PartyBoard_CrashHeartbeat();
}
bool captureCommittedState()
{
    const auto frame = gRuntime.frame - 1; // AFTER logic F, BEFORE render/counter publication.
    StateDigest stamp {frame,
        static_cast<std::uint32_t>(gRuntime.probeContext >= 0 ? gRuntime.probeContext : PartyBoard_NetplayContextId()),
        frand_state_get(), static_cast<std::uint32_t>(rand8_state_get()), GlobalCounter};
    auto state = captureCanonical(stamp);
    stamp.hash = state.hash;
    stamp.parts = state.parts;
    gRuntime.detailHistory[frame % kDetailHistorySize] = std::move(state);
    gRuntime.detailFrames[frame % kDetailHistorySize] = frame;
    gRuntime.states.capture(stamp);
    publishCrashState(stamp);
    // New frames stream immediately; input traffic repairs the oldest missing
    // state in parallel. No extra round-trip barrier for each gameplay frame.
    sendInput(frame, {}, false, &stamp);
    if (frame % 120 == 0) {
        char event[160];
        std::snprintf(event, sizeof(event),
            "checkpoint hash_frame=%u state_hash=%016llx hash_version=%u equal_next=%u",
            frame, static_cast<unsigned long long>(stamp.hash), kStateHashVersion, gRuntime.states.equalThrough());
        writeDiagnostic(event, true);
    }
    // After the state for this frame exists and has been published, so the probe
    // compares against exactly what the peer was told.
    forceRollbackTick(frame);
    return checkStateFailure();
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
        if (!gRuntime.rollbackRequested) {
            gRuntime.states.acknowledge(packet.hashAckNext);
            gRuntime.states.receive(packet.state);
            if (!checkStateFailure()) return;
        }
        if (packet.type == PacketType::State) continue;
        // A repair can concern an old, current or already captured future tick.
        // Always answer from immutable history, even while we are also waiting.
        if (packet.type == PacketType::Retransmit) {
            if (const auto *old = findInput(gRuntime.localHistory, packet.frame)) {
                sendInput(packet.frame, old->input);++gRuntime.repairPackets;
            }
        }
        // Validate retained duplicates even after their input frame committed.
        if (const auto *accepted = findInput(gRuntime.remoteHistory, packet.frame)) {
            if (!rollback::inputsEqual(accepted->input, packet.input)
                || accepted->captureContext != packet.captureContext) {
                char reason[192];
                std::snprintf(reason, sizeof(reason),
                    "PROTOCOL input contradiction input_frame=%u player=%u sequence=%u context=%u/%u",
                    packet.frame, packet.player, packet.sequence, accepted->captureContext, packet.captureContext);
                failSession(reason);
                return;
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
        const bool saved = PartyBoard_RollbackCheckpointSave(destination, capacity);
        if (!saved) {
            char reason[160];
            std::snprintf(reason, sizeof(reason),
                "rollback-capture-failed expected_bytes=%zu available_bytes=%zu",
                capacity, PartyBoard_RollbackCheckpointSize());
            writeDiagnostic(reason, true);
        }
        return saved;
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
        // Rollback can arm after lockstep has already advanced the wire clock.
        // Audio confirmation and tickets use that same absolute frame domain.
        if (!PartyBoard_RollbackAudioBridgeStartAtFrame(gRuntime.rollbackBaseFrame)) return false;
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
            // CheckpointSave only reads live state. At a fully reconciled
            // boundary its failure needs no restore and no speculative input
            // is discarded. Keep the current wire frame for lockstep retry.
            if (session.failedAtConfirmedCapture()) {
                if (!PartyBoard_RollbackAudioBridgeConfirm(runtime.frame)
                    || !PartyBoard_RollbackAudioBridgeStop()) {
                    failSession("Rollback capture fallback audio failed");
                    return false;
                }
                writeDiagnostic("rollback-capture-lockstep-fallback", true);
                runtime.rollbackSession.reset();
                runtime.rollbackPrepared = false;
                runtime.rollbackSessionContext = -1;
                runtime.rollbackUnavailable = true;
                return false;
            }
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
        } else if (argument == "--netplay-probe-desync") {
            gRuntime.desyncProbe = true;
        } else if (argument == "--netplay-menu-probe") {
            gRuntime.menuProbe = true;
        } else if (argument == "--netplay-probe-realtime") {
            gRuntime.realtimeProbe = true;
        } else if (argument == "--netplay-probe-audio") {
            gRuntime.audioProbe = true;
        } else if (argument == "--netplay-walk-probe") {
            gRuntime.walkProbe = true;
        } else if (argument == "--netplay-record-input" && index + 1 < argc) {
            gRuntime.recordPath = argv[++index];
        } else if (argument == "--netplay-replay-input" && index + 1 < argc) {
            gRuntime.replayPath = argv[++index];
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
    // The crash reporter is already armed; this only lets a report name the seat.
    PartyBoard_CrashSetPeer(gRuntime.localPlayer, host ? "host" : "client");
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
    if (!gRuntime.recordPath.empty()) {
        gRuntime.recordFile = std::fopen(gRuntime.recordPath.c_str(), "wb");
        if (!gRuntime.recordFile) {
            std::fprintf(stderr, "Netplay: cannot write the input recording %s.\n",
                gRuntime.recordPath.c_str());
            return false;
        }
    }
    if (!gRuntime.replayPath.empty() && !loadReplaySamples()) {
        std::fprintf(stderr, "Netplay: cannot read the input recording %s.\n",
            gRuntime.replayPath.c_str());
        return false;
    }
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

// The simulation frame, for instrumentation that needs to date an event. Zero
// when netplay is not running, which is the honest answer: there is no shared
// timeline to refer to.
extern "C" u32 PartyBoard_NetplayFrameForDiagnostics(void)
{
    return partyboard::netplay::gRuntime.enabled ? partyboard::netplay::gRuntime.frame : 0;
}

extern "C" bool PartyBoard_NetplayEnabled(void)
{
    return partyboard::netplay::gRuntime.enabled;
}

extern "C" void PartyBoard_NetplayTrace(const char *event)
{
    if (partyboard::netplay::gRuntime.enabled && event)
        partyboard::netplay::writeDiagnostic(event, true);
}

extern "C" bool PartyBoard_NetplayAllowsMultipleInstances(void)
{
    return PartyBoard_NetplayEnabled();
}

extern "C" bool PartyBoard_NetplayPreparePads(PADStatus status[4], u32 *rumble, bool startup)
{
    using namespace partyboard::netplay;
    if (!gRuntime.error.empty()) { serviceStateRepair(); return false; }
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
        PartyBoard_CrashBreadcrumb(PARTYBOARD_CRASH_CAT_OVERLAY,
            "context %d at frame %u (overlay %d minigame %d)", gameContext,
            runtime.frame, PartyBoard_NetplayContextId(), PartyBoard_NetplayMinigameId());
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
    serviceStateRepair();
    if (!runtime.error.empty()) return false;

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
        if (!PartyBoard_RollbackAudioBridgeConfirm(confirmedWireFrame)) {
            failSession("Rollback transition audio confirmation failed");
            return false;
        }
        if (!PartyBoard_RollbackAudioBridgeStop()) {
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

    // Bound unvalidated progress without overwriting canonical evidence.
    if (!runtime.rollbackRequested && !runtime.states.canCapture()) {
        ++runtime.stalledTicks;
        return false;
    }
    if (!runtime.localCaptured) {
        runtime.pendingLocal = capturePad(runtime.localPad);
        if (!runtime.replaySamples.empty()) {
            const std::uint32_t applied = runtime.frame + runtime.inputDelay;
            if (applied < runtime.replaySamples.size() && runtime.replayPresent[applied]) {
                runtime.pendingLocal = runtime.replaySamples[applied][runtime.localPlayer];
            } else {
                // Past the end of the recording: hold neutral rather than let
                // a physical controller join a replay half way through.
                runtime.pendingLocal = {};
                if (runtime.replayExhaustedFrame == UINT32_MAX) {
                    runtime.replayExhaustedFrame = applied;
                    std::printf("[NET TEST] replay_exhausted_frame=%u\n", applied);
                }
            }
        }
        if (runtime.menuProbe || runtime.walkProbe) {
            // Explicit real-game regression only. Send the host's input through
            // the normal input timeline; never advance an overlay directly.
            runtime.pendingLocal = {};
            if (runtime.walkProbe) {
                // A deterministic walk, not a hand-written menu script: the
                // point is to cross many overlays, wipes and audio waits while
                // the canonical state proves both peers stayed identical. A
                // fixed menu path would break on the first layout change.
                // Both seats act. Selection screens wait for every player to
                // confirm, so a walk driven by the host alone would stall on the
                // first one. The offset keeps the two seats from always pressing
                // the same button on the same frame.
                const unsigned beat = runtime.frame + runtime.localPlayer * 15u;
                if (beat % 30 < 3) {
                    switch ((beat / 30) % 8) {
                    case 2: runtime.pendingLocal.buttons = PAD_BUTTON_START; break;
                    // The D-pad is masked out by HuPadRead; menus read the
                    // analog stick through PadADConv.
                    case 4: runtime.pendingLocal.stickY = -100; break;
                    // Measured: with no cancel at all the walk settles on the
                    // first screen that needs one; with a cancel every eighth
                    // beat it backs out of everything and covers less. Rare.
                    case 5: if ((beat / 240) % 2) runtime.pendingLocal.buttons = PAD_BUTTON_B; break;
                    case 6: runtime.pendingLocal.stickX = 100; break;
                    default: runtime.pendingLocal.buttons = PAD_BUTTON_A; break;
                    }
                }
            } else if (runtime.localPlayer == 0
                && runtime.observedContext == static_cast<int>(MenuProbeOverlay::bootDll)
                && runtime.frame >= 600 && runtime.frame % 120 < 2) {
                runtime.pendingLocal.buttons = PAD_BUTTON_START;
            }
        }
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
    recordAppliedInputs(runtime.frame, local, remote);
    runtime.lastLocal = local;
    runtime.localCaptured = false;
    runtime.lockstepPrepared = !runtime.rollbackRequested;
    ++runtime.frame;
    runtime.progress.commit(monotonicMs());
    return true;
}

extern "C" bool PartyBoard_NetplayCommitTick(void)
{
    using namespace partyboard::netplay;
    Runtime &runtime = gRuntime;
    if (!runtime.enabled) return true;
    if (runtime.lockstepPrepared) {
        runtime.lockstepPrepared = false;
        return captureCommittedState();
    }
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
    if (gRuntime.recordFile) {
        std::fclose(gRuntime.recordFile);
        gRuntime.recordFile = nullptr;
    }
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

#include "netplay_state_test.inc"

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
    auto &skipBoot = partyboard::getSettings().backend.skipBootSequence;
    const auto savedSkipBoot = skipBoot;
    for (const bool online : {false, true}) {
        gRuntime.enabled = online;
        for (const bool skip : {false, true}) {
            skipBoot.setValue(skip);
            availabilityPassed &= partyboard_settings_skipBootSequence() == (!online && skip)
                && skipBoot.getValue() == skip;
        }
    }
    skipBoot = savedSkipBoot;
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
    const bool passed = runCanonicalStateSelfTest() && partyboard::netplay::timelineSelfTest()
        && partyboard::netplay::progressSelfTest() && HuPadSnapshotSelfTest()
        && PartyBoard_RollbackClockSelfTest() && availabilityPassed
        && runNativePadRollbackSelfTest() && topologyPassed && gamePassed
        && PartyBoard_AnimationRollbackSelfTest() && PartyBoard_RollbackIOSelfTest() && PartyBoard_RollbackSequenceSelfTest() && HuPrcSnapshotExecutionSelfTest() && PartyBoard_RollbackSceneSelfTest() && PartyBoard_RollbackResourcesSelfTest() && PartyBoard_RollbackCheckpointSelfTest() && PartyBoard_RollbackWipeSafetySelfTest() && PartyBoard_RollbackRenderSafetySelfTest() && PartyBoard_RollbackAudioSelfTest() && msmStreamLogicalSelfTest() && PartyBoard_RetraceCounterSelfTest() && PartyBoard_ThpLogicalSelfTest();
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
    // Deterministic audio wait: both peers arm the same one-second stream at
    // frame 100, but their simulated audio threads finish it at very different
    // frames, exactly as two machines with different audio buffering do.
    constexpr unsigned kAudioArmFrame = 100;
    constexpr unsigned kAudioSampleRate = 32000;
    const unsigned physicalFinishFrame = gRuntime.localPlayer == 0 ? 120u : 200u;
    unsigned audioDoneFrame = 0;
    if (gRuntime.audioProbe && !msmStreamLogicalProbeInstall()) {
        std::fputs("[NET TEST] FAIL: a real stream table is already installed\n", stderr);
        return false;
    }
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
            // Real gameplay globals and RNGs, driven by verified logical inputs.
            // This remains a subsystem probe, not a board/minigame replay.
            for (unsigned player = 0; player < 2; ++player) {
                GWPlayer[player].coins = static_cast<s16>((GWPlayer[player].coins
                    + pads[player].triggerLeft + (pads[player].button & PAD_BUTTON_A ? 3 : 0)) % 999);
                GWPlayer[player].roll = static_cast<s8>(pads[player].stickX % 10);
            }
            if (gRuntime.audioProbe) {
                if (frame == kAudioArmFrame)
                    msmStreamLogicalProbeStart(0, kAudioSampleRate, kAudioSampleRate);
                if (frame == physicalFinishFrame) msmStreamLogicalProbeFinishPhysical(0);
                // The real loop advances this from PadReadSimulationTick, once
                // per accepted tick and before the canonical state is captured.
                msmStreamLogicalTick();
                if (frame > kAudioArmFrame && audioDoneFrame == 0
                    && msmStreamGetStatus(0) == 0) audioDoneFrame = frame;
            }
            if (frame == 0 || frame == 400) BoardRandInit();
            frand(); rand8(); BoardRand();
            if (gRuntime.desyncProbe && gRuntime.localPlayer == 1 && frame == 87)
                ++GWPlayer[0].coins;
            PartyBoard_NetplayCommitTick();
            ++GlobalCounter; // The real rendered path publishes this after commit.
            ++frame;
        }
        if (PartyBoard_NetplayHasError()) {
            if (gRuntime.contextMismatchProbe || gRuntime.desyncProbe) {
                const auto expected = gRuntime.desyncProbe ? 87u : 100u;
                const bool passed = gRuntime.states.error() == StateFailure::Desync
                    && gRuntime.states.errorFrame() == expected
                    && gRuntime.states.equalThrough() == expected;
                // Give the peer time to receive the retained failed digest.
                for (unsigned i = 0; i < 30; ++i) {
                    serviceStateRepair(true);
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                std::printf("[NET TEST] %s: first divergence hash_frame=%u stopped_frame=%u\n",
                    passed ? "PASS" : "FAIL", gRuntime.states.errorFrame(), frame);
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
        // Windows coarse Sleep(1) can take 15.6 ms and invalidate the probe's
        // wall-time budget at delay zero. Precise waiting only affects this test.
        // Asymmetric wall clock: one peer runs visibly slower in real time.
        if (gRuntime.audioProbe && gRuntime.localPlayer == 1)
            SDL_DelayPrecise(5000000ull);
        SDL_DelayPrecise(gRuntime.realtimeProbe ? 16000000ull : 1000000ull);
    }
    // Drain final hashes/ACKs without simulating extra input. Keep responding
    // after local equality so the peer can also confirm the terminal frame.
    const auto drainEnd = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    auto settled = drainEnd;
    while (frame == frames && std::chrono::steady_clock::now() < drainEnd) {
        receivePendingPackets();
        serviceStateRepair();
        if (!gRuntime.error.empty()) return false;
        if (gRuntime.states.equalThrough() == frames && gRuntime.states.peerEqualThrough() == frames) {
            if (settled == drainEnd) settled = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
            if (std::chrono::steady_clock::now() >= settled) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (frame == frames && (gRuntime.states.equalThrough() != frames || gRuntime.states.peerEqualThrough() != frames))
        return false;
    if (gRuntime.audioProbe)
        std::printf("[NET TEST] audio_done_frame=%u physical_finish_frame=%u\n",
            audioDoneFrame, physicalFinishFrame);
    std::printf("[NET TEST] equal_states=%u peer_equal_states=%u\n",
        gRuntime.states.equalThrough(), gRuntime.states.peerEqualThrough());
    std::fprintf(stdout, "[NET TEST] %s: %u/%u frames, local physical PAD=%u, game port=%u\n",
        frame == frames ? "PASS" : "FAIL", frame, frames, gRuntime.localPad + 1, gRuntime.localPlayer + 1);
    return frame == frames;
}
