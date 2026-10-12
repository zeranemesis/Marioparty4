// The CubeShelf network client (src/port/online/cubeshelf_mesh.cpp) against values the C# code
// produced (tools/tests/fixtures/cubeshelf_mesh_vectors.json, written by CubeShelf.Core's
// MeshCrypto, MeshRecord, MeshFriends and NoiseHandshake): the same hashes, the same derived keys,
// the same record checks, the same pointer, and a whole handshake with every key fixed.
//
//   ./cubeshelf_mesh_test tools/tests/fixtures
//
// With a second argument, a file a running C# network wrote (see the comment at live_check), it
// also reads a friend's presence from that network for real.
#include "port/online/cubeshelf_mesh.hpp"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

using namespace partyboard::online::cubeshelf;
using namespace partyboard::online::cubeshelf::mesh;
using json = nlohmann::json;

namespace {

int failures = 0;

void check(bool ok, const char *what)
{
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

Bytes unhex(const std::string &text)
{
    Bytes out;
    for (std::size_t i = 0; i + 1 < text.size(); i += 2) out.push_back(static_cast<std::uint8_t>(std::stoi(text.substr(i, 2), nullptr, 16)));
    return out;
}

template <std::size_t N> std::array<std::uint8_t, N> fixed(const std::string &text)
{
    std::array<std::uint8_t, N> out {};
    const Bytes bytes = unhex(text);
    if (bytes.size() == N) std::copy(bytes.begin(), bytes.end(), out.begin());
    return out;
}

template <typename A> bool same(const A &a, const std::string &hex)
{
    const Bytes expected = unhex(hex);
    return expected.size() == a.size() && std::equal(a.begin(), a.end(), expected.begin());
}

std::string read(const std::string &path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

void vectors(const json &v)
{
    const std::uint8_t three[] = { 1, 2, 3 };
    check(same(domain_hash("cubeshelf-test", { { three, 3 }, { nullptr, 0 } }), v["hash"]), "MeshCrypto.Hash");

    Bytes secret(32);
    for (int i = 0; i < 32; ++i) secret[i] = static_cast<std::uint8_t>(i);
    check(same(hkdf(secret.data(), secret.size(), "info-test", 48), v["hkdf"]), "MeshCrypto.Hkdf");

    const auto derived = derived_public_key(secret.data(), secret.size(), v["derivedLabel"].get<std::string>());
    check(derived && same(*derived, v["derivedPublic"]), "DeriveSigningKey gives the same public key");
    check(derived && is_on_curve(*derived), "and it is on the curve");
    if (derived) {
        check(same(locator_of(*derived), v["derivedLocator"]), "MeshRecord.LocatorOf");
        check(same(node_id(*derived, v["nodeNonce"].get<std::uint64_t>()), v["nodeId"]), "NodeProof.IdOf");
        check(proof_bits(*derived, v["nodeNonce"].get<std::uint64_t>()) == v["nodeBits"].get<int>(), "the work counted the same");
    }

    // A record C# signed: read, checked, and refused once anything in it changes.
    const Bytes wire = unhex(v["record"]);
    std::size_t used = 0;
    auto record = read_record(wire.data(), wire.size(), used);
    const std::int64_t during = v["recordExpires"].get<std::int64_t>() - 60;
    check(record && used == wire.size(), "a C# record reads whole");
    check(record && record_valid(*record, during), "and verifies");
    check(record && std::string(record->payload.begin(), record->payload.end()) == "{\"hello\":1}" && record->sequence == 7, "with its payload and sequence");
    check(record && !record_valid(*record, v["recordExpires"].get<std::int64_t>() + 1), "an expired record is refused");
    if (record) {
        Record tampered = *record;
        tampered.payload.back() ^= 1;
        check(!record_valid(tampered, during), "a changed payload is refused");
        tampered = *record;
        tampered.sequence += 1;
        check(!record_valid(tampered, during), "a changed sequence is refused");
    }

    // The pointer a friend keeps for us, and what it opens to.
    const auto pair = fixed<32>(v["pair"]);
    const auto owner = fixed<65>(v["pointerOwner"]);
    const NodeId pointer = pointer_locator(pair.data(), owner, v["pointerDay"].get<std::int64_t>());
    check(same(pointer, v["pointerLocator"]), "MeshFriends.PointerKey lands at the same place");
    const auto main = open_pointer(pair.data(), pointer, unhex(v["pointerSealed"]));
    check(main && same(*main, v["pointerMain"]), "the pointer opens to the document's place");
    NodeId elsewhere = pointer;
    elsewhere[0] ^= 1;
    check(!open_pointer(pair.data(), elsewhere, unhex(v["pointerSealed"])), "and not under another place");

    // Noise XX, the initiator in C++ and the responder's messages from C#.
    const auto &n = v["noise"];
    const Bytes prologue = { 'c', 'u', 'b', 'e', 's', 'h', 'e', 'l', 'f', '-', 'm', 'e', 's', 'h', '-', 'v', '1' };
    NoiseInitiator initiator(fixed<32>(n["initiatorStatic"]), prologue, fixed<32>(n["initiatorEphemeral"]));
    Bytes payload1(169, 0);
    payload1[0] = 1;
    check(same(initiator.write_message1(payload1), n["message1"]), "Noise message 1 matches");
    const Bytes m2 = unhex(n["message2"]);
    const auto payload2 = initiator.read_message2(m2.data(), m2.size());
    check(payload2 && same(*payload2, n["payload2"]), "Noise message 2 opens");
    check(same(initiator.remote_static(), n["responderStaticPublic"]), "and reveals the responder's key");
    check(same(initiator.write_message3(unhex(n["payload3"])), n["message3"]), "Noise message 3 matches");
    check(same(initiator.handshake_hash(), n["hash"]), "the handshake hash matches");
    const auto [send, receive] = initiator.split();
    check(same(send, n["send"]) && same(receive, n["receive"]), "the transport keys match");

    Bytes broken = m2;
    broken[100] ^= 1;
    NoiseInitiator again(fixed<32>(n["initiatorStatic"]), prologue, fixed<32>(n["initiatorEphemeral"]));
    again.write_message1(payload1);
    check(!again.read_message2(broken.data(), broken.size()), "a tampered message 2 is refused");

    // A profile CubeShelf hands to a phone: a friend with no address is kept, with the ways in.
    ImportError error = ImportError::None;
    const auto profile = import_profile_document(v["profileDocument"].get<std::string>(), error);
    check(profile.has_value(), "the phone profile imports");
    if (profile) {
        check(profile->friends.size() == 1 && profile->friends[0].publicKey == v["profileFriend"].get<std::string>() && profile->friends[0].presenceUrl.empty(),
            "a friend of the network, with no address, is kept");
        check(profile->meshSeeds.size() == 2 && profile->meshSeeds[0] == "81.2.69.160:47914" && profile->meshDiscovery, "with the ways in and the choice");
        const auto stored = deserialize_profile(serialize_profile(*profile));
        check(stored && stored->meshSeeds == profile->meshSeeds && stored->meshDiscovery && stored->friends.size() == 1, "and they survive being stored");
        for (const auto &seed : profile->meshSeeds) check(Endpoint::parse(seed).has_value(), "every way in parses");
    }

    // The node key and its proof are kept for the day: a second start reuses them.
    {
        const std::string keyFile = "cubeshelf_mesh_test_node.key";
        std::remove(keyFile.c_str());
        ClientOptions options;
        options.nodeKeyFile = keyFile;
        {
            Client first(options);
            check(first.start(), "a client starts and proves an id");
        }
        const std::string kept = read(keyFile);
        {
            Client second(options);
            check(second.start(), "and starts again");
        }
        check(!kept.empty() && read(keyFile) == kept, "the node key is kept, not solved again");
        std::remove(keyFile.c_str());
    }

    check(is_public(*Endpoint::parse("81.2.69.160:47914")) && !is_public(*Endpoint::parse("192.168.1.2:1")) && !is_public(*Endpoint::parse("100.64.0.1:1")),
        "public and private addresses told apart");
    check(Endpoint::parse("[2a01:e0a::1]:47914")->to_string() == "[2a01:e0a::1]:47914", "IPv6 endpoints round-trip");
}

// A C# network on this machine (scratch tool meshlive): its seeds, a friend publishing for "me",
// and what the friend published. The client must find it through the network alone.
void live_check(const json &live)
{
    ClientOptions options;
    options.routable = [](const Endpoint &) { return true; };
    Client client(options);
    check(client.start(), "the client starts");
    std::vector<Endpoint> seeds;
    for (const auto &seed : live["seeds"]) seeds.push_back(*Endpoint::parse(seed.get<std::string>()));
    client.add_seeds(seeds);
    client.join();
    check(client.known_nodes() > 0, "it joined the network through the seeds");

    Profile me;
    me.privateKey = fixed<32>(live["mePrivate"]);
    me.publicKey = fixed<65>(live["mePublic"]);
    const auto friendKey = fixed<65>(live["friendPublic"]);
    const auto read = client.read_presence(me, friendKey, live["now"].get<std::int64_t>());
    check(read.reachable, "the network answered");
    check(read.envelope.has_value(), "the friend's document was found");
    check(read.envelope && *read.envelope == live["envelope"].get<std::string>(), "byte for byte what was published");
    check(read.sequence == live["sequence"].get<std::uint64_t>(), "with its sequence");
    if (read.envelope) {
        const auto opened = open_presence(me, friendKey, *read.envelope);
        check(opened && opened->displayName == live["friendName"].get<std::string>(), "and it opens as the friend's presence");
    }
}

} // namespace

int main(int argc, char **argv)
{
    const std::string dir = argc > 1 ? argv[1] : "tools/tests/fixtures";
    const std::string text = read(dir + "/cubeshelf_mesh_vectors.json");
    check(!text.empty(), "vectors present");
    if (!text.empty()) vectors(json::parse(text));
    if (argc > 2 && std::string(argv[2]) == "--meeting-point") {
        // BitTorrent's real DHT: how many addresses the meeting point gives, and that it does not hang.
        int answered = 0;
        const auto found = find_at_meeting_point(15, is_public, {}, &answered);
        std::printf("meeting point: %d DHT node(s) answered, %zu address(es)\n", answered, found.size());
        check(answered > 0, "the DHT answered");
    }
    else if (argc > 2) {
        live_check(json::parse(read(argv[2])));
    }
    if (failures == 0) std::puts("cubeshelf mesh tests passed");
    return failures == 0 ? 0 : 1;
}
