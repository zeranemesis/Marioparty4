#ifndef PARTYBOARD_PORT_ROLLBACK_HPP
#define PARTYBOARD_PORT_ROLLBACK_HPP

#include "port/rollback.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace partyboard::rollback {

constexpr std::size_t kMaxPlayers = 4;
constexpr std::size_t kHistoryFrames = 256;

struct Config {
    std::size_t playerCount = 2;
    std::size_t maximumRollback = 12;
    std::size_t snapshotBytes = 0;
};

struct Callbacks {
    std::function<bool(void *destination, std::size_t capacity)> saveState;
    std::function<bool(const void *source, std::size_t size)> loadState;
    std::function<void(std::uint32_t frame, const std::array<PartyBoardRollbackInput, kMaxPlayers> &inputs)> simulateFrame;
};

/* A deterministic snapshot is composed from explicit writable regions. This
 * intentionally avoids copying the whole PC process, which would include SDL,
 * audio-device and socket state that cannot safely be rewound. */
class SnapshotLayout {
public:
    bool addRegion(void *address, std::size_t size);
    void clear();
    std::size_t byteSize() const { return mByteSize; }
    std::size_t regionCount() const { return mRegions.size(); }
    bool save(void *destination, std::size_t capacity) const;
    bool load(const void *source, std::size_t size) const;
    std::uint32_t checksum() const;

private:
    struct Region {
        std::uint8_t *address = nullptr;
        std::size_t size = 0;
    };

    std::vector<Region> mRegions;
    std::size_t mByteSize = 0;
};

class Session {
public:
    Session(Config config, Callbacks callbacks);

    void reset();
    // Reject conflicting retransmissions and inputs outside the retained window.
    bool submitInput(std::size_t player, std::uint32_t frame, const PartyBoardRollbackInput &input);
    // False + healthy() means waiting for confirmation at the prediction limit.
    // False + !healthy() means an unrecoverable state/callback error.
    bool advance();
    bool reconcile();

    std::uint32_t currentFrame() const { return mCurrentFrame; }
    const PartyBoardRollbackStats &stats() const { return mStats; }
    bool healthy() const { return mHealthy; }

private:
    struct PlayerInput {
        PartyBoardRollbackInput value {};
        bool present = false;
        bool confirmed = false;
    };

    struct FrameRecord {
        std::uint32_t frame = 0;
        bool valid = false;
        bool hasSnapshot = false;
        std::array<PlayerInput, kMaxPlayers> inputs {};
        std::vector<std::uint8_t> snapshot;
    };

    FrameRecord &record(std::uint32_t frame);
    const FrameRecord *findRecord(std::uint32_t frame) const;
    PartyBoardRollbackInput inputFor(std::size_t player, std::uint32_t frame, bool &predicted);
    bool captureSnapshot(std::uint32_t frame);
    bool restoreSnapshot(std::uint32_t frame);
    bool simulate(std::uint32_t frame);

    Config mConfig;
    Callbacks mCallbacks;
    std::array<FrameRecord, kHistoryFrames> mHistory;
    std::uint32_t mCurrentFrame = 0;
    std::uint32_t mEarliestMismatch = UINT32_MAX;
    PartyBoardRollbackStats mStats {};
    bool mHealthy = true;
};

bool inputsEqual(const PartyBoardRollbackInput &lhs, const PartyBoardRollbackInput &rhs);
bool runSelfTest();
bool runCoroutineSnapshotSelfTest();
bool runMemorySnapshotSelfTest();

}

#endif
