#include "port/netplay_lobby.hpp"

#include <cstdio>
#include <cstdlib>

using namespace partyboard::netplay;

static unsigned checks = 0;
#define CHECK(expression) do { ++checks; if (!(expression)) { \
    std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expression); \
    std::exit(1); } } while (false)

static const LobbyCompatibility compatible { 1, "test-build-manifest", "test-disc-identity" };

static void readyEveryone(Lobby& room)
{
    for (const auto& seat : room.seats()) {
        if (seat.connection) {
            CHECK(room.setReady(seat.connection, room.revision(), true) == LobbyResult::Ok);
        }
    }
}

static void membership()
{
    Lobby room(10, compatible);
    CHECK(room.slotFor(10) == 0);
    CHECK(!room.slotFor(0));
    CHECK(room.join(0, compatible) == LobbyResult::InvalidMember);
    CHECK(room.join(20, { 2, compatible.build, compatible.game }) == LobbyResult::Incompatible);
    CHECK(room.join(20, { 1, "another-build", compatible.game }) == LobbyResult::Incompatible);
    CHECK(room.join(20, { 1, compatible.build, "another-disc" }) == LobbyResult::Incompatible);
    CHECK(room.join(20, compatible) == LobbyResult::Ok);
    CHECK(room.slotFor(20) == 1);
    const auto oldRevision = room.revision();
    CHECK(room.join(20, compatible) == LobbyResult::Ok);
    CHECK(room.revision() == oldRevision);
    CHECK(room.setReady(20, oldRevision, true) == LobbyResult::Ok);
    CHECK(room.join(30, compatible) == LobbyResult::Ok);
    CHECK(!room.seats()[1].ready);
    CHECK(room.setReady(20, oldRevision, true) == LobbyResult::Stale);
    CHECK(room.join(40, compatible) == LobbyResult::Ok);
    CHECK(room.join(50, compatible) == LobbyResult::Full);
    CHECK(room.leave(999) == LobbyResult::Unauthorized);
    CHECK(room.leave(20) == LobbyResult::Ok);
    CHECK(room.slotFor(30) == 2);
    CHECK(room.slotFor(40) == 3);
    CHECK(room.join(50, compatible) == LobbyResult::Ok);
    CHECK(room.slotFor(50) == 1);
    CHECK(room.move(30, 40, 1) == LobbyResult::Unauthorized);
    CHECK(room.move(10, 40, 1) == LobbyResult::Occupied);
    CHECK(room.move(10, 40, 4) == LobbyResult::InvalidSlot);
    CHECK(room.move(10, 40, 0) == LobbyResult::InvalidSlot);
    CHECK(room.move(10, 10, 1) == LobbyResult::InvalidSlot);
    CHECK(room.move(10, 999, 1) == LobbyResult::InvalidMember);
    CHECK(room.leave(30) == LobbyResult::Ok);
    readyEveryone(room);
    CHECK(room.move(10, 40, 2) == LobbyResult::Ok);
    CHECK(room.slotFor(40) == 2);
    CHECK(!room.seats()[2].ready);
    CHECK(!room.seats()[3].connection);
    CHECK(room.leave(10) == LobbyResult::Ok);
    CHECK(room.phase() == LobbyPhase::Closed);
    CHECK(!room.slotFor(40));
    CHECK(room.join(60, compatible) == LobbyResult::Busy);
}

static void startBarrier(unsigned players)
{
    Lobby room(1, compatible);
    for (unsigned player = 2; player <= players; ++player) {
        CHECK(room.join(player, compatible) == LobbyResult::Ok);
    }
    CHECK(room.prepare(999, room.revision()) == LobbyResult::Unauthorized);
    CHECK(room.prepare(1, room.revision()) == LobbyResult::NotReady);
    CHECK(room.setReady(999, room.revision(), true) == LobbyResult::Unauthorized);
    readyEveryone(room);
    const auto roster = room.revision();
    CHECK(room.prepare(1, roster - 1) == LobbyResult::Stale);
    CHECK(room.prepare(1, roster) == LobbyResult::Ok);
    const auto attempt = room.revision();
    CHECK(!room.ownsInput(1, 0, attempt));
    CHECK(room.join(99, compatible) == LobbyResult::Busy);
    CHECK(room.setReady(1, roster, false) == LobbyResult::Busy);
    CHECK(room.commitStart(1, attempt) == LobbyResult::NotReady);
    CHECK(room.acknowledge(1, roster) == LobbyResult::Stale);
    CHECK(room.acknowledge(999, attempt) == LobbyResult::Unauthorized);
    CHECK(room.cancelStart(999) == LobbyResult::Unauthorized);
    CHECK(room.cancelStart(1) == LobbyResult::Ok);
    readyEveryone(room);
    CHECK(room.prepare(1, room.revision()) == LobbyResult::Ok);
    CHECK(room.acknowledge(1, attempt) == LobbyResult::Stale);
    for (unsigned player = 1; player <= players; ++player) {
        CHECK(room.acknowledge(player, room.revision()) == LobbyResult::Ok);
    }
    CHECK(room.commitStart(999, room.revision()) == LobbyResult::Unauthorized);
    CHECK(room.commitStart(1, attempt) == LobbyResult::Stale);
    CHECK(room.commitStart(1, room.revision()) == LobbyResult::Ok);
    CHECK(room.phase() == LobbyPhase::Running);
    for (unsigned player = 1; player <= players; ++player) {
        for (std::uint8_t slot = 0; slot < 4; ++slot) {
            CHECK(room.ownsInput(player, slot, room.revision()) == (slot == player - 1));
        }
        CHECK(!room.ownsInput(player, 4, room.revision()));
        CHECK(!room.ownsInput(player, static_cast<std::uint8_t>(player - 1), attempt));
        // First local controller controls assigned network slot, including P4.
        const auto route = lobbyControllerRoute(room, player, 0);
        CHECK(route && route->physicalPad == 0 && route->gameSlot == player - 1);
        CHECK(!lobbyControllerRoute(room, player, 4));
    }
    CHECK(!room.ownsInput(0, 3, room.revision()));
    CHECK(!lobbyControllerRoute(room, 999, 0));
    CHECK(room.leave(players) == LobbyResult::Ok);
    CHECK(room.phase() == LobbyPhase::Closed);
    CHECK(!room.ownsInput(1, 0, room.revision()));
}

static void disconnectWhilePreparing()
{
    Lobby room(1, compatible);
    CHECK(room.join(2, compatible) == LobbyResult::Ok);
    CHECK(room.join(3, compatible) == LobbyResult::Ok);
    readyEveryone(room);
    CHECK(room.prepare(1, room.revision()) == LobbyResult::Ok);
    const auto attempt = room.revision();
    CHECK(room.acknowledge(1, attempt) == LobbyResult::Ok);
    CHECK(room.leave(2) == LobbyResult::Ok);
    CHECK(room.phase() == LobbyPhase::Waiting);
    CHECK(room.slotFor(3) == 2);
    CHECK(!room.seats()[0].acknowledged);
    CHECK(!room.seats()[2].ready);
    CHECK(room.commitStart(1, attempt) == LobbyResult::Busy);
    // A reconnected player is a new authenticated transport connection.
    CHECK(room.join(4, compatible) == LobbyResult::Ok);
    CHECK(room.slotFor(4) == 1);
    CHECK(room.setReady(2, room.revision(), true) == LobbyResult::Unauthorized);
}

int main()
{
    membership();
    for (unsigned players = 1; players <= 4; ++players) startBarrier(players);
    disconnectWhilePreparing();
    bool invalidHostRejected = false;
    try { Lobby invalid(0, compatible); }
    catch (const std::invalid_argument&) { invalidHostRejected = true; }
    CHECK(invalidHostRejected);
    std::printf("Lobby state tests: PASS (%u checks, 1-4 players). No network/game integration tested.\n", checks);
}
