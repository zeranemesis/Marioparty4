#pragma once

#include "port/online/cubeshelf_social.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The CubeShelf network, as a phone takes part in it: reading only.
//
// CubeShelf (CubeShelf.Core/Social/Mesh) is a peer-to-peer network with no server: encrypted
// sessions between nodes (Noise_XX_P256_AESGCM_SHA256 over UDP), a Kademlia table of signed
// records, and for each friend a pointer only the two of them can find, naming where today's
// sealed presence document is. A friend who publishes only there has no https address -- and so,
// until this, was invisible to a phone.
//
// This is a client of that network and nothing more: it opens sessions to nodes, asks them for
// records, and reads its friends' documents. It holds nothing for others, relays for nobody and
// announces nothing -- the right part for a phone on a mobile network, behind its operator's NAT,
// on battery. What it reads is opened by the same code that opens a document read over https.
//
// Byte for byte the protocol of the C# code; both sides change together.
// tools/tests/cubeshelf_mesh_test.cpp checks it against values the C# code produced.
namespace partyboard::online::cubeshelf::mesh {

using Bytes = std::vector<std::uint8_t>;
using NodeId = std::array<std::uint8_t, 32>;

// An IPv4 or IPv6 address and a port.
struct Endpoint {
    bool v6 = false;
    std::array<std::uint8_t, 16> address {};
    std::uint16_t port = 0;

    std::string to_string() const;
    static std::optional<Endpoint> parse(std::string_view text); // "1.2.3.4:5", "[2001:db8::1]:5"
    bool operator==(const Endpoint &other) const = default;
    bool operator<(const Endpoint &other) const;
};

// MeshAddresses.IsPublic: an address worth talking to on the Internet.
bool is_public(const Endpoint &endpoint);

// ------------------------------------------------------------------ the primitives (MeshCrypto)

void sha256(const std::uint8_t *data, std::size_t length, std::uint8_t out[32]);
// MeshCrypto.Hash: SHA-256 over the domain and each part, every one prefixed by its length.
Bytes domain_hash(std::string_view domain, std::initializer_list<std::pair<const std::uint8_t *, std::size_t>> parts);
Bytes hkdf(const std::uint8_t *secret, std::size_t secretLength, std::string_view info, std::size_t length);
bool is_on_curve(const PublicKey &key);
// The public half of MeshCrypto.DeriveSigningKey(secret, label).
std::optional<PublicKey> derived_public_key(const std::uint8_t *secret, std::size_t secretLength, std::string_view label);
// MeshRecord.LocatorOf.
NodeId locator_of(const PublicKey &signer);
// NodeProof: the id a key and a nonce name, and whether the nonce carries enough work.
NodeId node_id(const PublicKey &key, std::uint64_t nonce);
int proof_bits(const PublicKey &key, std::uint64_t nonce);
// MeshCrypto.Open: nonce || ciphertext || tag under AES-256-GCM.
std::optional<Bytes> open_sealed(const std::uint8_t key[32], const Bytes &sealed, const std::uint8_t *aad, std::size_t aadLength);

// A signed record (MeshRecord), read and checked: its place is its key's, it has not expired,
// and the signature covers every field.
struct Record {
    NodeId locator {};
    std::uint8_t kind = 0;
    PublicKey signer {};
    std::uint64_t sequence = 0;
    std::int64_t expiresUnix = 0;
    Bytes payload;
    std::array<std::uint8_t, 64> signature {};
};
std::optional<Record> read_record(const std::uint8_t *data, std::size_t length, std::size_t &used);
bool record_valid(const Record &record, std::int64_t nowUnix);
// MeshRecord.WriteTo: the record as it travels.
Bytes write_record(const Record &record);

// MeshFriends: where a friend's pointer for us is on a given day, and how it opens.
NodeId pointer_locator(const std::uint8_t pairKey[32], const PublicKey &owner, std::int64_t day);
std::optional<NodeId> open_pointer(const std::uint8_t pairKey[32], const NodeId &pointerLocator, const Bytes &payload);

// ------------------------------------------------------------------ the handshake (NoiseHandshake)

// The initiator side of Noise XX over P-256, which is all a client needs.
class NoiseInitiator {
public:
    // A static key of its own, and optionally fixed ephemeral key (tests only).
    NoiseInitiator(const std::array<std::uint8_t, 32> &staticPrivate, const Bytes &prologue,
        const std::optional<std::array<std::uint8_t, 32>> &ephemeralPrivate = std::nullopt);
    ~NoiseInitiator();
    NoiseInitiator(const NoiseInitiator &) = delete;
    NoiseInitiator &operator=(const NoiseInitiator &) = delete;

    Bytes write_message1(const Bytes &payload);
    // The responder's payload, or nothing if anything is wrong.
    std::optional<Bytes> read_message2(const std::uint8_t *message, std::size_t length);
    Bytes write_message3(const Bytes &payload);

    const PublicKey &remote_static() const { return remoteStatic_; }
    const std::array<std::uint8_t, 32> &handshake_hash() const { return h_; }
    // Send key, receive key.
    std::pair<std::array<std::uint8_t, 32>, std::array<std::uint8_t, 32>> split() const;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    std::array<std::uint8_t, 32> h_ {};
    PublicKey remoteStatic_ {};
};

// ------------------------------------------------------------------ the client

struct ClientOptions {
    // Which addresses may be dialled: public ones, outside tests.
    std::function<bool(const Endpoint &)> routable = is_public;
    // The work a node id needs, ours and our peers'. Lowered only by tests.
    int proofDifficulty = 22;
    int lookupSeconds = 15;
    // Where the nodes met are kept between runs; empty keeps nothing.
    std::string nodesFile;
    // Where the node key and its proof of work are kept for a day: the proof is seconds of work a
    // phone should not redo at every launch. Empty: a new key every run, as CubeShelf does.
    std::string nodeKeyFile;
};

// What a friend's network presence read gave: the sealed document and its record's sequence.
struct NetworkRead {
    bool reachable = false; // false: we are on no network at all, nothing could be asked
    std::optional<std::string> envelope;
    std::uint64_t sequence = 0;
};

class Client {
public:
    explicit Client(ClientOptions options = {});
    ~Client();
    Client(const Client &) = delete;
    Client &operator=(const Client &) = delete;

    // Opens the socket and proves an id (a few seconds of work, once). False if no socket.
    bool start();
    // More ways in: CubeShelf's entry points, the meeting point, the nodes file.
    void add_seeds(const std::vector<Endpoint> &seeds);
    // How many nodes the client knows and could ask.
    std::size_t known_nodes() const;
    // Joins through the seeds when it knows too few nodes. Blocking, a few seconds at most.
    void join();

    // A friend's presence as published on the network, today's or yesterday's. Blocking.
    NetworkRead read_presence(const Profile &me, const PublicKey &friendKey, std::int64_t nowUnix);

    // Passes on the friends' records read since the last call -- pointers and documents, signed
    // by their authors and unchanged -- to the nodes that should hold them, so a friend whose PC
    // is off stays findable longer. The one thing a client can do for the network without being
    // reachable. Blocking. Returns how many stores were accepted.
    int republish(std::int64_t nowUnix);

    // The raw lookup, for tests: every valid record at a locator.
    std::vector<Record> find_value(const NodeId &locator);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ------------------------------------------------------------------ the meeting point (MainlineRendezvous)

// CubeShelf nodes that announced themselves under the meeting point of BitTorrent's DHT. Looks
// only: a phone never announces. Blocking, within budgetSeconds.
std::vector<Endpoint> find_at_meeting_point(int budgetSeconds = 15, std::function<bool(const Endpoint &)> routable = is_public,
    const std::vector<Endpoint> &startingNodes = {}, int *answered = nullptr);

} // namespace partyboard::online::cubeshelf::mesh
