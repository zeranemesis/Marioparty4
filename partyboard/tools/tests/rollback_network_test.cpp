#include "port/rollback.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

using namespace partyboard::rollback;
static unsigned checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; \
    std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); } } while (false)

// Only the host logger is stubbed. Compile and exercise the actual rollback.cpp.
extern "C" void OSReport(const char*, ...) {}

struct State {
    std::array<std::uint32_t, 4> positions {};
    std::uint32_t random = 0x13579bdf;
    std::uint32_t checksum = 0;
    bool operator==(const State&) const = default;
};

static std::unique_ptr<Session> session(State& state, std::size_t players = 4,
    std::size_t maximumRollback = 12)
{
    Callbacks callbacks;
    callbacks.saveState = [&state](void* dest, std::size_t size) {
        if (size != sizeof(state)) return false;
        std::memcpy(dest, &state, size);
        return true;
    };
    callbacks.loadState = [&state](const void* src, std::size_t size) {
        if (size != sizeof(state)) return false;
        std::memcpy(&state, src, size);
        return true;
    };
    callbacks.simulateFrame = [&state](std::uint32_t frame, const auto& inputs) {
        state.random = state.random * 1664525u + 1013904223u;
        for (std::size_t player = 0; player < 4; ++player) {
            state.positions[player] = state.positions[player] * 33u + inputs[player].buttons
                + static_cast<std::uint32_t>(inputs[player].stickX);
            state.checksum = state.checksum * 31u + state.positions[player] + frame + state.random;
        }
    };
    return std::make_unique<Session>(Config { players, maximumRollback, sizeof(state) }, callbacks);
}

static PartyBoardRollbackInput input(std::uint32_t frame, std::size_t player)
{
    PartyBoardRollbackInput value {};
    value.buttons = static_cast<u16>((frame * 17u + player * 31u) % 1024u);
    value.stickX = static_cast<s8>(static_cast<int>((frame + player * 7u) % 101u) - 50);
    return value;
}

static void boundaries()
{
    State state;
    auto engine = session(state);
    const auto first = input(0, 0);
    CHECK(engine->submitInput(0, 0, first));
    CHECK(engine->submitInput(0, 0, first));
    CHECK(!engine->submitInput(0, 0, input(1, 0))); // Conflicting confirmed input.
    CHECK(!engine->submitInput(4, 0, first));
    CHECK(!engine->submitInput(0, 256, first)); // Would overwrite frame zero.
    CHECK(!engine->submitInput(0, UINT32_MAX, first));
    CHECK(engine->healthy());

    // Lost remote connection must exhaust prediction and wait, not keep
    // simulating until its only recoverable state has been overwritten.
    State stalled;
    auto waiting = session(stalled);
    for (std::uint32_t frame = 0; frame < 12; ++frame) CHECK(waiting->advance());
    const auto before = stalled;
    CHECK(!waiting->advance());
    CHECK(waiting->healthy());
    CHECK(waiting->currentFrame() == 12);
    CHECK(stalled == before);
    for (std::size_t player = 0; player < 4; ++player) {
        CHECK(waiting->submitInput(player, 0, input(0, player)));
    }
    CHECK(waiting->advance()); // The missing frame arrived within the window.
    CHECK(waiting->healthy());
    CHECK(waiting->currentFrame() == 13);
    CHECK(!waiting->submitInput(0, 0, first)); // Now outside rollback window.
}

static void fourPeers()
{
    constexpr std::uint32_t frames = 1200;
    struct Message { std::uint32_t delivery, frame; std::size_t player; };
    std::array<State, 4> states {};
    State referenceState;
    auto reference = session(referenceState);
    std::array<std::unique_ptr<Session>, 4> peers;
    std::array<std::vector<Message>, 4> queues;
    for (std::size_t peer = 0; peer < 4; ++peer) peers[peer] = session(states[peer]);
    for (std::uint32_t frame = 0; frame < frames; ++frame) {
        for (std::size_t player = 0; player < 4; ++player) {
            CHECK(reference->submitInput(player, frame, input(frame, player)));
            for (std::size_t peer = 0; peer < 4; ++peer) {
                // Different jitter for every sender/receiver, plus a simulated
                // retry for every 23rd packet. Maximum delivery delay is 8 ticks.
                const auto delay = player == peer ? 0u :
                    static_cast<unsigned>((frame * 7u + player * 3u + peer) % 6u)
                        + (frame % 23u == 0 ? 3u : 0u);
                queues[peer].push_back({ frame + delay, frame, player });
            }
        }
        CHECK(reference->advance());
        for (std::size_t peer = 0; peer < 4; ++peer) {
            auto& queue = queues[peer];
            // Reverse delivery order exercises out-of-order and duplicate inputs.
            for (std::size_t i = queue.size(); i-- > 0;) {
                if (queue[i].delivery > frame) continue;
                const auto message = queue[i];
                CHECK(peers[peer]->submitInput(message.player, message.frame,
                    input(message.frame, message.player)));
                CHECK(peers[peer]->submitInput(message.player, message.frame,
                    input(message.frame, message.player)));
                queue.erase(queue.begin() + static_cast<std::ptrdiff_t>(i));
            }
            CHECK(peers[peer]->advance());
        }
    }
    for (std::size_t peer = 0; peer < 4; ++peer) {
        for (const auto& message : queues[peer]) {
            CHECK(peers[peer]->submitInput(message.player, message.frame,
                input(message.frame, message.player)));
        }
        CHECK(peers[peer]->reconcile());
        CHECK(peers[peer]->healthy());
        CHECK(states[peer] == referenceState);
        CHECK(peers[peer]->stats().rollbackCount > 0);
        CHECK(peers[peer]->stats().maximumRollback <= 8);
    }
}

int main()
{
    CHECK(runSelfTest());
    CHECK(runMemorySnapshotSelfTest());
    CHECK(runCoroutineSnapshotSelfTest());
    boundaries();
    fourPeers();
    std::printf("Rollback network-model tests: %u checks, %u failures. "
        "Synthetic state; no game/Internet validation.\n", checks, failures);
    return failures ? 1 : 0;
}
