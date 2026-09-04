#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace partyboard::netplay {

// Host/service-side state machine, NOT a network authentication mechanism.
// Connection IDs must come from the authenticated transport, never a client
// payload. Password checking, rate limiting, TLS and reconnect credentials
// belong to that transport. No password or reconnect secret is stored here.
using LobbyConnection = std::uint64_t;
constexpr std::size_t kLobbySlots = 4;

struct LobbyCompatibility {
    std::uint32_t protocol = 0;
    std::string build;       // Exact build/content manifest identity, not a UI label.
    std::string game;        // Disc region + revision + validated content identity.
    bool operator==(const LobbyCompatibility&) const = default;
};

enum class LobbyPhase { Waiting, Preparing, Running, Closed };
enum class LobbyResult {
    Ok, Unauthorized, InvalidMember, Incompatible, Full, Busy, Stale,
    NotReady, InvalidSlot, Occupied
};

struct LobbySeat {
    LobbyConnection connection = 0; // Zero means vacant.
    bool ready = false;
    bool acknowledged = false;
};

// Slots are zero-based internally and displayed as 1..4. The host always owns
// slot zero. Vacant slots stay vacant: leaving never renumbers other players.
class Lobby {
public:
    Lobby(LobbyConnection host, LobbyCompatibility compatibility)
        : mHost(host), mCompatibility(std::move(compatibility))
    {
        if (!host || !mCompatibility.protocol || mCompatibility.build.empty()
            || mCompatibility.game.empty()) {
            throw std::invalid_argument("A lobby needs a host and exact compatibility identities");
        }
        mSeats[0].connection = host;
    }

    LobbyPhase phase() const { return mPhase; }
    std::uint64_t revision() const { return mRevision; }
    const std::array<LobbySeat, kLobbySlots>& seats() const { return mSeats; }

    std::optional<std::uint8_t> slotFor(LobbyConnection connection) const
    {
        if (!connection || mPhase == LobbyPhase::Closed) return std::nullopt;
        for (std::uint8_t i = 0; i < kLobbySlots; ++i) {
            if (mSeats[i].connection == connection) return i;
        }
        return std::nullopt;
    }

    LobbyResult join(LobbyConnection connection, const LobbyCompatibility& compatibility)
    {
        if (!connection) return LobbyResult::InvalidMember;
        if (compatibility != mCompatibility) return LobbyResult::Incompatible;
        if (mPhase != LobbyPhase::Waiting) return LobbyResult::Busy;
        if (slotFor(connection)) return LobbyResult::Ok; // Idempotent retry.
        for (auto& seat : mSeats) {
            if (!seat.connection) {
                seat.connection = connection;
                rosterChanged();
                return LobbyResult::Ok;
            }
        }
        return LobbyResult::Full;
    }

    LobbyResult leave(LobbyConnection connection)
    {
        const auto slot = slotFor(connection);
        if (!slot) return LobbyResult::Unauthorized;
        if (connection == mHost || mPhase == LobbyPhase::Running) {
            // No silent CPU substitution / host migration during deterministic
            // play. The runtime must stop on Closed, not simulate missing input.
            mPhase = LobbyPhase::Closed;
            mSeats = {};
            ++mRevision;
            return LobbyResult::Ok;
        }
        mSeats[*slot] = {};
        mPhase = LobbyPhase::Waiting; // Also cancels an incomplete start barrier.
        rosterChanged();
        return LobbyResult::Ok;
    }

    LobbyResult move(LobbyConnection actor, LobbyConnection member, std::uint8_t slot)
    {
        if (actor != mHost) return LobbyResult::Unauthorized;
        if (mPhase != LobbyPhase::Waiting) return LobbyResult::Busy;
        const auto old = slotFor(member);
        if (!old) return LobbyResult::InvalidMember;
        if (slot >= kLobbySlots || !slot || member == mHost) return LobbyResult::InvalidSlot;
        if (*old == slot) return LobbyResult::Ok;
        if (mSeats[slot].connection) return LobbyResult::Occupied;
        mSeats[slot] = mSeats[*old];
        mSeats[*old] = {};
        rosterChanged();
        return LobbyResult::Ok;
    }

    LobbyResult setReady(LobbyConnection connection, std::uint64_t rosterRevision, bool ready)
    {
        const auto slot = slotFor(connection);
        if (!slot) return LobbyResult::Unauthorized;
        if (mPhase != LobbyPhase::Waiting) return LobbyResult::Busy;
        if (rosterRevision != mRevision) return LobbyResult::Stale;
        mSeats[*slot].ready = ready;
        return LobbyResult::Ok;
    }

    LobbyResult prepare(LobbyConnection actor, std::uint64_t rosterRevision)
    {
        if (actor != mHost) return LobbyResult::Unauthorized;
        if (mPhase != LobbyPhase::Waiting) return LobbyResult::Busy;
        if (rosterRevision != mRevision) return LobbyResult::Stale;
        for (const auto& seat : mSeats) {
            if (seat.connection && !seat.ready) return LobbyResult::NotReady;
        }
        mPhase = LobbyPhase::Preparing;
        ++mRevision; // Start attempt token; old acknowledgements cannot release it.
        return LobbyResult::Ok;
    }

    LobbyResult acknowledge(LobbyConnection connection, std::uint64_t startRevision)
    {
        const auto slot = slotFor(connection);
        if (!slot) return LobbyResult::Unauthorized;
        if (mPhase != LobbyPhase::Preparing) return LobbyResult::Busy;
        if (startRevision != mRevision) return LobbyResult::Stale;
        mSeats[*slot].acknowledged = true;
        return LobbyResult::Ok;
    }

    LobbyResult commitStart(LobbyConnection actor, std::uint64_t startRevision)
    {
        if (actor != mHost) return LobbyResult::Unauthorized;
        if (mPhase != LobbyPhase::Preparing) return LobbyResult::Busy;
        if (startRevision != mRevision) return LobbyResult::Stale;
        for (const auto& seat : mSeats) {
            if (seat.connection && !seat.acknowledged) return LobbyResult::NotReady;
        }
        // Transport still must reliably distribute the agreed initial state and
        // start command. This local transition alone does not synchronize PCs.
        mPhase = LobbyPhase::Running;
        return LobbyResult::Ok;
    }

    LobbyResult cancelStart(LobbyConnection actor)
    {
        if (actor != mHost) return LobbyResult::Unauthorized;
        if (mPhase != LobbyPhase::Preparing) return LobbyResult::Busy;
        mPhase = LobbyPhase::Waiting;
        rosterChanged();
        return LobbyResult::Ok;
    }

    bool ownsInput(LobbyConnection sender, std::uint8_t claimedSlot,
        std::uint64_t startRevision) const
    {
        return mPhase == LobbyPhase::Running && startRevision == mRevision
            && claimedSlot < kLobbySlots && sender != 0
            && mSeats[claimedSlot].connection == sender;
    }

private:
    void rosterChanged()
    {
        ++mRevision;
        for (auto& seat : mSeats) {
            seat.ready = false;
            seat.acknowledged = false;
        }
    }

    LobbyConnection mHost;
    LobbyCompatibility mCompatibility;
    std::array<LobbySeat, kLobbySlots> mSeats {};
    LobbyPhase mPhase = LobbyPhase::Waiting;
    std::uint64_t mRevision = 1;
};

// Physical controller index is intentionally separate from the assigned game
// slot. The input sampler reads physicalPad; network packets use gameSlot.
struct LobbyControllerRoute {
    std::uint8_t physicalPad;
    std::uint8_t gameSlot;
};

inline std::optional<LobbyControllerRoute> lobbyControllerRoute(
    const Lobby& lobby, LobbyConnection localConnection, std::uint8_t physicalPad)
{
    const auto assigned = lobby.slotFor(localConnection);
    if (!assigned || physicalPad >= kLobbySlots) return std::nullopt;
    return LobbyControllerRoute { physicalPad, *assigned };
}

} // namespace partyboard::netplay
