#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace partyboard::netplay {
constexpr std::uint32_t kStateHashVersion = 1;
constexpr std::uint32_t kNoHashFrame = UINT32_MAX;
constexpr std::size_t kStateHistorySize = 256;
constexpr std::uint32_t kStateLeadLimit = 128;

struct StateDigest {
    std::uint32_t frame = kNoHashFrame, context = 0;
    std::uint32_t frand = 0, rand8 = 0, counter = 0;
    std::uint32_t version = kStateHashVersion;
    std::uint64_t hash = 0;
    bool operator==(const StateDigest &) const = default;
};

// Names are diagnostic metadata only. Integers always occupy four network-order
// bytes; no native object representation, pointer, padding or host endianness.
struct CanonicalState {
    struct Field { const char *name; std::uint32_t value; };
    std::vector<Field> fields;
    std::uint64_t hash = 14695981039346656037ull;
    void add(const char *name, std::uint32_t value) {
        fields.push_back({name, value});
        for (int shift = 24; shift >= 0; shift -= 8)
            hash = (hash ^ static_cast<std::uint8_t>(value >> shift)) * 1099511628211ull;
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
