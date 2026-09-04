#include "port/netplay_runtime.h"

#include "dolphin/os.h"
#include "dolphin/pad.h"
#include "port/netplay_transport.hpp"
#include "port/rollback.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

extern "C" bool PartyBoard_NetplayIsMinigame(void);
extern "C" s32 PartyBoard_NetplayMinigameId(void);
extern "C" s32 PartyBoard_NetplayContextId(void);
extern "C" void PartyBoard_NetplayPadCapture(int pad, PartyBoardRollbackInput *input);
extern "C" void PartyBoard_NetplayPadApplyRemote(
    int pad, const PartyBoardRollbackInput *input, const PartyBoardRollbackInput *previous);
extern "C" u32 frand_state_get(void);
extern "C" void frand_state_set(u32 state);
extern "C" s32 rand8_state_get(void);
extern "C" void rand8_state_set(s32 state);
extern "C" void *HuMemHeapPtrGet(int heap);
extern "C" std::size_t HuMemHeapSizeGet(int heap);
extern "C" std::size_t HuPrcSnapshotSizeGet(void);
extern "C" BOOL HuPrcSnapshotSave(void *destination, std::size_t capacity);
extern "C" std::size_t omDLLSnapshotSizeGet(void);
extern "C" BOOL omDLLSnapshotSave(void *destination, std::size_t capacity);

namespace partyboard::netplay {
namespace {

constexpr std::uint32_t kSessionId = 0x4d503452u; // "MP4R"
constexpr std::uint32_t kRuntimeConfigMagic = 0x4e500000u; // "NP" + delay
constexpr std::uint32_t kRuntimeFullGameFlag = 0x00010000u;
constexpr std::uint32_t kRuntimeConfigMagicMask = 0xfffe0000u;
constexpr std::size_t kHistorySize = 256;
constexpr std::uint8_t kDefaultInputDelay = 2;
constexpr std::uint8_t kMaximumInputDelay = 8;
constexpr std::uint32_t kPeerTimeoutTicks = 300;

struct InputSlot {
    std::uint32_t frame = 0;
    PartyBoardRollbackInput input {};
    bool valid = false;
};

struct Runtime {
    UdpTransport transport;
    std::array<InputSlot, kHistorySize> localHistory {};
    std::array<InputSlot, kHistorySize> remoteHistory {};
    PartyBoardRollbackInput lastRemote {};
    PartyBoardRollbackInput lastLocal {};
    PartyBoardRollbackInput pendingLocal {};
    std::uint32_t frame = 0;
    std::uint32_t sequence = 0;
    std::uint32_t receivedPackets = 0;
    std::uint32_t rejectedPackets = 0;
    std::uint32_t stalledTicks = 0;
    std::uint32_t consecutiveStalledTicks = 0;
    std::uint32_t maximumStalledTicks = 0;
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
};

Runtime gRuntime;

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
    PartyBoardRollbackInput input {};
    PartyBoard_NetplayPadCapture(pad, &input);
    return input;
}

void resetOverlayTimeline(std::uint8_t contextId)
{
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

bool runRollbackSnapshotProbe()
{
    constexpr int kHeapSystem = 0;
    constexpr int kHeapData = 2;
    rollback::SnapshotLayout heapLayout;
    const std::size_t processBytes = HuPrcSnapshotSizeGet();
    const std::size_t overlayBytes = omDLLSnapshotSizeGet();
    if (!heapLayout.addRegion(HuMemHeapPtrGet(kHeapSystem), HuMemHeapSizeGet(kHeapSystem))
        || !heapLayout.addRegion(HuMemHeapPtrGet(kHeapData), HuMemHeapSizeGet(kHeapData))
        || processBytes == 0 || overlayBytes == 0) {
        return false;
    }

    std::vector<std::uint8_t> heapSnapshot(heapLayout.byteSize());
    std::vector<std::uint8_t> processSnapshot(processBytes);
    std::vector<std::uint8_t> overlaySnapshot(overlayBytes);
    if (!heapLayout.save(heapSnapshot.data(), heapSnapshot.size())
        || !HuPrcSnapshotSave(processSnapshot.data(), processSnapshot.size())
        || !omDLLSnapshotSave(overlaySnapshot.data(), overlaySnapshot.size())) {
        return false;
    }

    std::uint32_t checksum = checksumBytes(2166136261u, heapSnapshot.data(), heapSnapshot.size());
    checksum = checksumBytes(checksum, processSnapshot.data(), processSnapshot.size());
    checksum = checksumBytes(checksum, overlaySnapshot.data(), overlaySnapshot.size());
    std::fprintf(stdout,
        "Rollback probe: snapshot captured (%zu heap + %zu coroutine + %zu overlay bytes, checksum 0x%08x).\n",
        heapSnapshot.size(), processSnapshot.size(), overlaySnapshot.size(), checksum);
    return true;
}

void storeInput(std::array<InputSlot, kHistorySize> &history, std::uint32_t frame,
    const PartyBoardRollbackInput &input)
{
    InputSlot &slot = history[frame % kHistorySize];
    slot = { frame, input, true };
}

const InputSlot *findInput(const std::array<InputSlot, kHistorySize> &history, std::uint32_t frame)
{
    const InputSlot &slot = history[frame % kHistorySize];
    return slot.valid && slot.frame == frame ? &slot : nullptr;
}

std::uint32_t runtimeConfigSignature(std::uint8_t inputDelay, std::uint8_t contextId,
    bool fullGame)
{
    return kRuntimeConfigMagic | (fullGame ? kRuntimeFullGameFlag : 0u)
        | (static_cast<std::uint32_t>(contextId) << 8) | inputDelay;
}

bool sendInput(std::uint32_t frame, const PartyBoardRollbackInput &input)
{
    InputPacket outgoing {};
    outgoing.sessionId = kSessionId;
    outgoing.sequence = gRuntime.sequence++;
    outgoing.frame = frame;
    outgoing.player = gRuntime.localPlayer;
    outgoing.input = input;
    outgoing.configSignature = runtimeConfigSignature(gRuntime.inputDelay, gRuntime.contextId,
        gRuntime.fullGame);
    outgoing.frandSeed = gRuntime.randomSynchronized ? gRuntime.sessionFrandSeed : 0;
    outgoing.rand8Seed = gRuntime.randomSynchronized ? gRuntime.sessionRand8Seed : 0;
    return !gRuntime.transport.hasPeer() || gRuntime.transport.sendInput(outgoing);
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
    while (gRuntime.transport.receiveInput(packet)) {
        if (packet.sessionId != kSessionId || packet.player != (gRuntime.localPlayer ^ 1u)) {
            ++gRuntime.rejectedPackets;
            continue;
        }
        const bool signatureValid =
            (packet.configSignature & kRuntimeConfigMagicMask) == kRuntimeConfigMagic;
        const bool remoteFullGame = (packet.configSignature & kRuntimeFullGameFlag) != 0;
        const std::uint8_t remoteContext =
            static_cast<std::uint8_t>((packet.configSignature >> 8) & 0xffu);
        const std::uint8_t remoteDelay = static_cast<std::uint8_t>(packet.configSignature & 0xffu);
        if (!signatureValid || remoteFullGame != gRuntime.fullGame
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
        if (packet.frame < gRuntime.frame || packet.frame - gRuntime.frame >= kHistorySize) {
            ++gRuntime.rejectedPackets;
            continue;
        }
        InputSlot &slot = gRuntime.remoteHistory[packet.frame % kHistorySize];
        if (!slot.valid || slot.frame != packet.frame) {
            slot.frame = packet.frame;
            slot.input = packet.input;
            slot.valid = true;
        } else if (std::memcmp(&slot.input, &packet.input, sizeof(packet.input)) != 0) {
            ++gRuntime.rejectedPackets;
            continue;
        }
        ++gRuntime.receivedPackets;
    }
    gRuntime.peerInDifferentContext = contextCatchupRequired(
        sawMatchingContext, sawDifferentContext);
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
    return runtimeConfigSignature(0, 7, false) != runtimeConfigSignature(delay, 7, false)
        && runtimeConfigSignature(delay, 7, false)
            == (kRuntimeConfigMagic | (7u << 8) | delay)
        && runtimeConfigSignature(delay, 7, false) != runtimeConfigSignature(delay, 8, false)
        && runtimeConfigSignature(delay, 7, false) != runtimeConfigSignature(delay, 7, true);
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
        } else if (argument == "--netplay-host" || argument == "--netplay-join"
            || argument == "--netplay-delay" || argument == "--netplay-pad") {
            argumentsValid = false;
        }
    }
    if (!host && !join) {
        return true;
    }
    if (!argumentsValid || host == join) {
        std::fputs("Netplay: choose exactly one of --netplay-host <port> or --netplay-join <IPv4:port>; optional --netplay-delay <0-8>, --netplay-pad <1-4>, --netplay-full and --netplay-rollback-probe.\n", stderr);
        return false;
    }

    gRuntime.localPlayer = host ? 0 : 1;
    if (!gRuntime.transport.open(host ? hostPort : 0)) {
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
    gRuntime.rollbackProbe = rollbackProbe;
    gRuntime.enabled = true;
    std::fprintf(stdout,
        "Netplay experimental: %s, UDP port %u, local player %u, physical controller port %u, input delay %u frame(s). %s.\n",
        host ? "host" : "client", gRuntime.localPort,
        static_cast<unsigned>(gRuntime.localPlayer + 1),
        static_cast<unsigned>(gRuntime.localPad + 1),
        static_cast<unsigned>(gRuntime.inputDelay),
        gRuntime.fullGame ? "Full-game lockstep" : "Minigame overlays");
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

extern "C" bool PartyBoard_NetplayTick(void)
{
    using namespace partyboard::netplay;
    Runtime &runtime = gRuntime;
    if (!runtime.enabled) {
        return true;
    }

    const bool active = runtime.fullGame || PartyBoard_NetplayIsMinigame();
    if (!active) {
        if (runtime.overlayActive) {
            std::fprintf(stdout,
                "Netplay: minigame ended (%u packets, %u rejected, %u waiting ticks, longest wait %u).\n",
                runtime.receivedPackets, runtime.rejectedPackets, runtime.stalledTicks,
                runtime.maximumStalledTicks);
            runtime.overlayActive = false;
        }
        return true;
    }
    const std::uint8_t currentContext = static_cast<std::uint8_t>(runtime.fullGame
        ? PartyBoard_NetplayContextId()
        : PartyBoard_NetplayMinigameId());
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
        runtime.rollbackProbeDone = true;
        if (!runRollbackSnapshotProbe()) {
            std::fputs("Rollback probe: snapshot capture unavailable; lockstep remains active.\n",
                stderr);
        }
    }

    receivePendingPackets();

    if (runtime.configMismatch) {
        ++runtime.stalledTicks;
        return false;
    }

    if (runtime.peerInDifferentContext) {
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
    }

    if (!runtime.randomSynchronized) {
        sendInput(runtime.frame + runtime.inputDelay, runtime.pendingLocal);
        ++runtime.stalledTicks;
        ++runtime.consecutiveStalledTicks;
        return false;
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
            sendInput(runtime.frame, localSlot->input);
        } else {
            sendInput(runtime.frame + runtime.inputDelay, runtime.pendingLocal);
        }
        ++runtime.stalledTicks;
        ++runtime.consecutiveStalledTicks;
        runtime.maximumStalledTicks = std::max(runtime.maximumStalledTicks,
            runtime.consecutiveStalledTicks);
        if (runtime.consecutiveStalledTicks >= kPeerTimeoutTicks
            && !runtime.peerTimeoutReported) {
            std::fprintf(stderr,
                "Netplay: still waiting for frame %u after 5 seconds; peer disconnected or network mismatch.\n",
                runtime.frame);
            runtime.peerTimeoutReported = true;
        }
        return false;
    }

    // Repeat the current input as lightweight UDP redundancy. This also lets
    // a host that discovered its peer during the startup buffer provide the
    // earliest delayed frame that the client needs.
    if (!startupFrame && localSlot != nullptr) {
        sendInput(runtime.frame, localSlot->input);
    }
    sendInput(runtime.frame + runtime.inputDelay, runtime.pendingLocal);
    runtime.consecutiveStalledTicks = 0;
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
    return true;
}

extern "C" void PartyBoard_NetplayShutdown(void)
{
    using namespace partyboard::netplay;
    gRuntime.transport.close();
    gRuntime = Runtime {};
}

extern "C" bool PartyBoard_NetplayRuntimeRunSelfTest(void)
{
    const bool passed = partyboard::netplay::timelineSelfTest();
    OSReport("Netplay runtime self-test: %s\n", passed ? "PASS" : "FAIL");
    return passed;
}
