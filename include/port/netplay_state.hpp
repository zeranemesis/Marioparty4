#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace partyboard::netplay {
constexpr std::uint32_t kStateHashVersion = 4;
constexpr std::uint32_t kNoHashFrame = UINT32_MAX;
constexpr std::size_t kStateHistorySize = 256;
constexpr std::uint32_t kStateLeadLimit = 128;
// A divergence is always reported at a frame the lead limit still retains, so
// field-level evidence needs a shorter ring than the digest stream itself.
constexpr std::size_t kDetailHistorySize = 136;

// One hash per gameplay subsystem, so a divergence report names the area that
// changed instead of a single opaque digest. Wire order is fixed: never
// reorder or renumber, only append before the count while the protocol allows.
enum class Subsystem : std::uint8_t {
    Meta = 0,   // Frame stamp, context, protocol bookkeeping.
    Rng,        // frand / rand8 / BoardRand seeds and consumption counters.
    Gamework,   // GWSystem and the global save-independent settings.
    Players,    // GWPlayer / GWPlayerCfg.
    Board,      // Board-side logical state (dice, spaces, board globals).
    Objects,    // omObjData tables.
    Processes,  // HuPrc topology and scheduling state.
    Overlay,    // omcurovl / omnextovl / overlay history and module state.
    Animation,  // HSF motion and sprite logical time.
    Sequence,   // Minigame sequence driver.
    Heaps,      // Allocator bookkeeping (counts and sizes, never addresses).
    Scene,      // Wipe and other scene-level presentation gates.
    Input,      // HuPad logical state after clamp/edge/repeat.
    Timers,     // GlobalCounter, VCounter and frame pacing counters.
    Audio,      // Logical audio state the game can observe and block on.
    Minigame,   // Minigame tables and per-minigame shared state.
};
constexpr std::size_t kSubsystemCount = 16;

constexpr const char *subsystemName(std::size_t index)
{
    constexpr const char *names[kSubsystemCount] = {
        "META", "RNG", "GAMEWORK", "PLAYERS", "BOARD", "OBJECTS", "PROCESSES",
        "OVERLAY", "ANIMATION", "SEQUENCE", "HEAPS", "SCENE", "INPUT", "TIMERS",
        "AUDIO", "MINIGAME",
    };
    return index < kSubsystemCount ? names[index] : "UNKNOWN";
}

constexpr std::array<std::uint32_t, kSubsystemCount> initialSubsystemHashes()
{
    std::array<std::uint32_t, kSubsystemCount> value {};
    for (auto &entry : value) entry = 2166136261u;
    return value;
}

struct StateDigest {
    std::uint32_t frame = kNoHashFrame, context = 0;
    std::uint32_t frand = 0, rand8 = 0, counter = 0;
    std::uint32_t version = kStateHashVersion;
    std::uint64_t hash = 0;
    std::array<std::uint32_t, kSubsystemCount> parts = initialSubsystemHashes();
    bool operator==(const StateDigest &) const = default;
    // First subsystem whose hash differs, or kSubsystemCount when only the
    // frame stamp itself differs. Used to label a divergence report.
    std::size_t firstDifferentPart(const StateDigest &other) const {
        for (std::size_t index = 0; index < kSubsystemCount; ++index)
            if (parts[index] != other.parts[index]) return index;
        return kSubsystemCount;
    }
};

// Names are diagnostic metadata only. Integers always occupy four network-order
// bytes; no native object representation, pointer, padding or host endianness.
// Labels are stored as literal pointers, never copied strings, so retaining a
// frame of evidence costs twelve bytes per field.
struct CanonicalState {
    struct Run { std::uint32_t first; std::uint8_t subsystem; };
    std::vector<const char *> names;
    std::vector<std::uint32_t> values;
    std::vector<Run> runs;
    std::uint64_t hash = 14695981039346656037ull;
    std::array<std::uint32_t, kSubsystemCount> parts = initialSubsystemHashes();
    std::uint8_t section = static_cast<std::uint8_t>(Subsystem::Meta);

    void begin(Subsystem subsystem) {
        section = static_cast<std::uint8_t>(subsystem);
        runs.push_back({static_cast<std::uint32_t>(values.size()), section});
    }
    void add(const char *name, std::uint32_t value) {
        names.push_back(name);
        values.push_back(value);
        auto &part = parts[section];
        for (int shift = 24; shift >= 0; shift -= 8) {
            const auto byte = static_cast<std::uint8_t>(value >> shift);
            hash = (hash ^ byte) * 1099511628211ull;
            part = (part ^ byte) * 16777619u;
        }
    }
    std::size_t size() const { return values.size(); }
    // Subsystem that owns field `index`, for reports and comparison tooling.
    std::uint8_t subsystemOf(std::size_t index) const {
        std::uint8_t owner = static_cast<std::uint8_t>(Subsystem::Meta);
        for (const auto &run : runs) {
            if (run.first > index) break;
            owner = run.subsystem;
        }
        return owner;
    }
    void reset() {
        names.clear();
        values.clear();
        runs.clear();
        hash = 14695981039346656037ull;
        parts = initialSubsystemHashes();
        section = static_cast<std::uint8_t>(Subsystem::Meta);
    }
    static void sink(void *context, const char *name, std::uint32_t value) {
        static_cast<CanonicalState *>(context)->add(name, value);
    }
};

enum class StateFailure { None, Desync, Contradiction, InvalidFrame, InvalidAck };

// Both streams are immutable by frame. Compare only contiguous frames so packet
// reorder cannot cause a later mismatch to hide the first divergent checkpoint.
// ACK is the exclusive count of equal states, NOT an input ACK or restore point.
class StateHistory {
public:
    bool capture(const StateDigest &value) {
        if (failure != StateFailure::None) return false;
        if (value.frame != localCount || !canCapture()) return fail(StateFailure::InvalidFrame, value.frame);
        local[value.frame % kStateHistorySize] = value;
        ++localCount;
        return compare();
    }
    bool receive(const StateDigest &value) {
        if (failure != StateFailure::None) return false;
        if (value.frame == kNoHashFrame) return true;
        if (value.version != kStateHashVersion
            || (value.frame >= localCount && value.frame - localCount >= kStateLeadLimit))
            return fail(StateFailure::InvalidFrame, value.frame);
        // Outside retained history: discard, never alias a newer ring slot.
        if (value.frame < nextEqual && nextEqual - value.frame >= kStateHistorySize) return true;
        auto &slot = remote[value.frame % kStateHistorySize];
        if (slot.frame == value.frame) {
            if (!(slot == value)) return fail(StateFailure::Contradiction, value.frame);
        } else {
            if (slot.frame != kNoHashFrame && slot.frame > value.frame) return true;
            slot = value;
        }
        return compare();
    }
    bool acknowledge(std::uint32_t next) {
        if (failure != StateFailure::None) return false;
        if (next > localCount) return fail(StateFailure::InvalidAck, next);
        peerEqual = std::max(peerEqual, next); // Older ACKs may arrive later.
        return true;
    }
    bool canCapture() const {
        return failure == StateFailure::None && localCount != kNoHashFrame
            && localCount - std::min(nextEqual, peerEqual) < kStateLeadLimit;
    }
    const StateDigest *pending() const { return getLocal(peerEqual); }
    const StateDigest *getLocal(std::uint32_t frame) const {
        const auto &value = local[frame % kStateHistorySize];
        return frame != kNoHashFrame && value.frame == frame ? &value : nullptr;
    }
    const StateDigest *getRemote(std::uint32_t frame) const {
        const auto &value = remote[frame % kStateHistorySize];
        return frame != kNoHashFrame && value.frame == frame ? &value : nullptr;
    }
    std::uint32_t captured() const { return localCount; }
    std::uint32_t equalThrough() const { return nextEqual; }
    std::uint32_t peerEqualThrough() const { return peerEqual; }
    StateFailure error() const { return failure; }
    std::uint32_t errorFrame() const { return failedFrame; }
private:
    bool fail(StateFailure reason, std::uint32_t frame) {
        if (failure == StateFailure::None) { failure = reason; failedFrame = frame; }
        return false;
    }
    bool compare() {
        while (nextEqual < localCount) {
            const auto *a = getLocal(nextEqual), *b = getRemote(nextEqual);
            if (!a || !b) break;
            if (!(*a == *b)) return fail(StateFailure::Desync, nextEqual);
            ++nextEqual;
        }
        return true;
    }
    std::array<StateDigest, kStateHistorySize> local {}, remote {};
    std::uint32_t localCount = 0, nextEqual = 0, peerEqual = 0;
    std::uint32_t failedFrame = kNoHashFrame;
    StateFailure failure = StateFailure::None;
};
} // namespace partyboard::netplay
