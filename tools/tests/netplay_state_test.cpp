#include "port/netplay_state.hpp"
#include <cstdio>
#include <cstdlib>
#include <vector>
using namespace partyboard::netplay;
static unsigned checks = 0;
#define CHECK(value) do { ++checks; if (!(value)) { std::fprintf(stderr, "FAIL line=%d: %s\n", __LINE__, #value); std::exit(1); } } while (false)
static StateDigest digest(std::uint32_t frame) {
    StateDigest value {frame, frame / 137, frame * 1664525u + 1013904223u, frame * 33u, frame};
    CanonicalState state;
    state.add("version", value.version); state.add("frame", frame); state.add("context", value.context);
    state.add("rng", value.frand); state.add("rand8", value.rand8); state.add("counter", value.counter);
    value.hash = state.hash;
    return value;
}
static void invariants() {
    CanonicalState bytes;
    bytes.add("ignored-label", 0x01020304u);
    // Independently computed FNV-1a-64 over bytes 01 02 03 04.
    CHECK(bytes.hash == 0xbe7a5e775165785dull);
    CanonicalState relabelled; relabelled.add("another-address-and-label", 0x01020304u);
    CHECK(bytes.hash == relabelled.hash);
    StateHistory ordered;
    for (unsigned i = 0; i < 8; ++i) CHECK(ordered.capture(digest(i)));
    auto later = digest(6); later.hash ^= 1;
    CHECK(ordered.receive(later)); CHECK(ordered.equalThrough() == 0);
    auto first = digest(3); first.frand ^= 1; first.hash ^= 17;
    CHECK(ordered.receive(first));
    for (unsigned i = 0; i < 3; ++i) {
        CHECK(ordered.receive(digest(i)) == (i != 2));
    }
    CHECK(ordered.error() == StateFailure::Desync && ordered.errorFrame() == 3);
    CHECK(!ordered.acknowledge(3)); // Fatal state is sticky.
    StateHistory duplicates;
    CHECK(duplicates.receive(digest(0))); CHECK(duplicates.receive(digest(0)));
    CHECK(duplicates.capture(digest(0))); CHECK(duplicates.equalThrough() == 1);
    auto changed = digest(0); changed.context++;
    CHECK(!duplicates.receive(changed));
    CHECK(duplicates.error() == StateFailure::Contradiction);
    StateHistory bounded;
    for (unsigned i = 0; i < kStateLeadLimit; ++i) CHECK(bounded.capture(digest(i)));
    CHECK(!bounded.canCapture()); CHECK(!bounded.acknowledge(kStateLeadLimit + 1));
    StateHistory version;
    auto wrong = digest(0); wrong.version++;
    CHECK(!version.receive(wrong));
    StateHistory wrap;
    for (unsigned i = 0; i < 800; ++i) {
        CHECK(wrap.capture(digest(i))); CHECK(wrap.receive(digest(i))); CHECK(wrap.acknowledge(i + 1));
        CHECK(wrap.receive(digest(0))); // An ancient duplicate must not alias current state.
    }
    CHECK(wrap.equalThrough() == 800);
    CHECK(wrap.receive(digest(810)));
    CHECK(wrap.receive(digest(554))); // Same ring slot, old still near the retention edge.
    CHECK(wrap.getRemote(810) != nullptr);
}
struct Message { int due, target; StateDigest state; std::uint32_t ack; };
static void profile(int rtt, int jitter, unsigned loss, bool faults) {
    StateHistory peers[2];
    std::vector<Message> queue;
    std::uint32_t random = 0x17293845;
    auto draw = [&]() { random ^= random << 13; random ^= random >> 17; random ^= random << 5; return random; };
    unsigned dropped = 0, duplicates = 0;
    auto send = [&](int source, int now, const StateDigest &state) {
        if ((faults && ((now >= 3000 && now < 3400) || (now >= 6000 && now < 6800)))
            || draw() % 1000 < loss) { ++dropped; return; }
        const int offset = jitter ? static_cast<int>(draw() % (2 * jitter + 1)) - jitter : 0;
        Message message {now + std::max(1, rtt / 2 + offset), source ^ 1, state, peers[source].equalThrough()};
        queue.push_back(message);
        if (draw() % 100 < 3) { message.due += 11; queue.push_back(message); ++duplicates; }
    };
    constexpr unsigned frames = 900;
    for (int now = 0; now < 60000; ++now) {
        for (int p = 0; p < 2; ++p) {
            if (now % 17 == 0 && peers[p].captured() < frames && peers[p].canCapture()) {
                const auto state = digest(peers[p].captured());
                CHECK(peers[p].capture(state)); send(p, now, state);
                if (const auto *old = peers[p].pending()) send(p, now, *old);
            }
            if (now % 100 == 0 && peers[p].captured()) {
                const auto *state = peers[p].pending();
                if (!state) state = peers[p].getLocal(peers[p].captured() - 1);
                send(p, now, *state);
            }
        }
        for (std::size_t i = 0; i < queue.size();) {
            const auto message = queue[i];
            if (message.due > now) { ++i; continue; }
            queue[i] = queue.back(); queue.pop_back(); // Deliberate arrival reorder.
            CHECK(peers[message.target].acknowledge(message.ack));
            CHECK(peers[message.target].receive(message.state));
        }
        if (peers[0].equalThrough() == frames && peers[1].equalThrough() == frames
            && peers[0].peerEqualThrough() == frames && peers[1].peerEqualThrough() == frames) {
            std::printf("PASS hash-stream rtt=%d jitter=%d loss_permille=%u burst_pause=%d frames=%u dropped=%u duplicates=%u virtual_ms=%d\n",
                rtt,jitter,loss,faults,frames,dropped,duplicates,now);
            return;
        }
    }
    CHECK(false);
}
int main() {
    invariants();
    for (int rtt : {0,50,100,150,250})
        for (int jitter : {0,10,30})
            for (unsigned loss : {0u,5u,10u,30u,50u}) profile(rtt,jitter,loss,false);
    profile(100,30,30,true);
    std::printf("PASS: %u state-history checks; 76 deterministic hash-stream network profiles. Not full-game/input simulation.\n", checks);
}
