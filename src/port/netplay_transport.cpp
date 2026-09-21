#include "port/netplay_transport.hpp"
#include "port/rollback.hpp"

#include "dolphin/os.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <thread>
#include <utility>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace partyboard::netplay {

// The transport seats exactly as many players as the simulation does. If these
// ever drift, a packet is accepted for a seat the rollback session has no slot
// for, and the failure appears somewhere far from here.
static_assert(kMaxNetplayPeers == rollback::kMaxPlayers, "netplay peers and rollback seats must agree");
namespace {

constexpr std::size_t kPacketSize = kNetplayPacketSize;
constexpr std::array<std::uint8_t, 4> kMagic { 'P', 'B', 'R', 'B' };


void put16(std::uint8_t *destination, std::uint16_t value)
{
    destination[0] = static_cast<std::uint8_t>(value >> 8);
    destination[1] = static_cast<std::uint8_t>(value);
}

void put32(std::uint8_t *destination, std::uint32_t value)
{
    destination[0] = static_cast<std::uint8_t>(value >> 24);
    destination[1] = static_cast<std::uint8_t>(value >> 16);
    destination[2] = static_cast<std::uint8_t>(value >> 8);
    destination[3] = static_cast<std::uint8_t>(value);
}

std::uint16_t get16(const std::uint8_t *source)
{
    return static_cast<std::uint16_t>((source[0] << 8) | source[1]);
}

std::uint32_t get32(const std::uint8_t *source)
{
    return (static_cast<std::uint32_t>(source[0]) << 24)
        | (static_cast<std::uint32_t>(source[1]) << 16)
        | (static_cast<std::uint32_t>(source[2]) << 8)
        | static_cast<std::uint32_t>(source[3]);
}

std::uint32_t packetHash(const std::uint8_t *bytes, std::size_t size)
{
    std::uint32_t hash = 2166136261u;
    for (std::size_t index = 0; index < size; ++index) {
        hash = (hash ^ bytes[index]) * 16777619u;
    }
    return hash;
}

std::array<std::uint8_t, kPacketSize> encode(const InputPacket &packet)
{
    std::array<std::uint8_t, kPacketSize> bytes {};
    std::copy(kMagic.begin(), kMagic.end(), bytes.begin());
    put16(bytes.data() + 4, kProtocolVersion);
    bytes[6] = static_cast<std::uint8_t>(packet.type);
    bytes[7] = packet.player;
    put32(bytes.data() + 8, packet.sessionId);
    put32(bytes.data() + 12, packet.sequence);
    put32(bytes.data() + 16, packet.frame);
    put16(bytes.data() + 20, packet.input.buttons);
    bytes[22] = static_cast<std::uint8_t>(packet.input.stickX);
    bytes[23] = static_cast<std::uint8_t>(packet.input.stickY);
    bytes[24] = static_cast<std::uint8_t>(packet.input.substickX);
    bytes[25] = static_cast<std::uint8_t>(packet.input.substickY);
    bytes[26] = packet.input.triggerLeft;
    bytes[27] = packet.input.triggerRight;
    put32(bytes.data() + 28, packet.configSignature);
    put32(bytes.data() + 32, packet.frandSeed);
    put32(bytes.data() + 36, packet.rand8Seed);
    put32(bytes.data() + 40, packet.hashAckNext);
    put32(bytes.data() + 44, packet.captureContext);
    put32(bytes.data() + 48, packet.state.version);
    put32(bytes.data() + 52, packet.state.frame);
    put32(bytes.data() + 56, packet.state.context);
    put32(bytes.data() + 60, packet.state.frand);
    put32(bytes.data() + 64, packet.state.rand8);
    put32(bytes.data() + 68, packet.state.counter);
    put32(bytes.data() + 72, static_cast<std::uint32_t>(packet.state.hash >> 32));
    put32(bytes.data() + 76, static_cast<std::uint32_t>(packet.state.hash));
    // 80..83 reserved, must be zero.
    for (std::size_t index = 0; index < kSubsystemCount; ++index)
        put32(bytes.data() + 84 + index * 4, packet.state.parts[index]);
    put32(bytes.data() + 148, packetHash(bytes.data(), 148));
    return bytes;
}

bool decode(const std::uint8_t *bytes, std::size_t size, InputPacket &packet)
{
    if (size != kPacketSize || !std::equal(kMagic.begin(), kMagic.end(), bytes)
        || get16(bytes + 4) != kProtocolVersion || bytes[6] < 1 || bytes[6] > 3
        || get32(bytes + 80) != 0 || get32(bytes + 48) != kStateHashVersion
        || get32(bytes + 148) != packetHash(bytes, 148)) {
        return false;
    }
    packet.player = bytes[7];
    packet.sessionId = get32(bytes + 8);
    packet.sequence = get32(bytes + 12);
    packet.frame = get32(bytes + 16);
    packet.input.buttons = get16(bytes + 20);
    packet.input.stickX = static_cast<s8>(bytes[22]);
    packet.input.stickY = static_cast<s8>(bytes[23]);
    packet.input.substickX = static_cast<s8>(bytes[24]);
    packet.input.substickY = static_cast<s8>(bytes[25]);
    packet.input.triggerLeft = bytes[26];
    packet.input.triggerRight = bytes[27];
    packet.configSignature = get32(bytes + 28);
    packet.frandSeed = get32(bytes + 32);
    packet.rand8Seed = get32(bytes + 36);
    packet.type = static_cast<PacketType>(bytes[6]);
    packet.hashAckNext = get32(bytes + 40);
    packet.state.version = get32(bytes + 48);
    packet.state.frame = get32(bytes + 52);
    packet.state.context = get32(bytes + 56);
    packet.state.frand = get32(bytes + 60);
    packet.state.rand8 = get32(bytes + 64);
    packet.state.counter = get32(bytes + 68);
    packet.state.hash = (static_cast<std::uint64_t>(get32(bytes + 72)) << 32) | get32(bytes + 76);
    for (std::size_t index = 0; index < kSubsystemCount; ++index)
        packet.state.parts[index] = get32(bytes + 84 + index * 4);
    packet.captureContext = get32(bytes + 44);
    // The author byte indexes seat arrays, so it is bounded here and nowhere else.
    // It said "< 2" -- the single line that made a third player impossible at the
    // wire level, whatever the rest of the stack could do.
    return packet.player < kMaxNetplayPeers
        && (packet.type != PacketType::State || packet.state.frame != kNoHashFrame);
}

#if defined(_WIN32)
using Socket = SOCKET;
constexpr Socket kInvalidSocket = INVALID_SOCKET;
void closeSocket(Socket socket) { closesocket(socket); }
int lastSocketError() { return WSAGetLastError(); }
bool wouldBlock(int error) { return error == WSAEWOULDBLOCK; }
#else
using Socket = int;
constexpr Socket kInvalidSocket = -1;
void closeSocket(Socket socket) { ::close(socket); }
int lastSocketError() { return errno; }
bool wouldBlock(int error) { return error == EAGAIN || error == EWOULDBLOCK; }
#endif

}

struct UdpTransport::Impl {
    struct Peer {
        sockaddr_in address {};
        bool valid = false;
    };
    Socket socket = kInvalidSocket;
    std::array<Peer, kMaxNetplayPeers> peers {};
    std::uint8_t lastSender = kMaxNetplayPeers;
    std::size_t expectedPeers = 1;
    bool discoverPeer = false;
#if defined(_WIN32)
    bool winsockStarted = false;
#endif
    std::uint16_t port = 0;
    std::string error;
};

UdpTransport::UdpTransport()
    : mImpl(std::make_unique<Impl>())
{
}

UdpTransport::~UdpTransport() { close(); }
UdpTransport::UdpTransport(UdpTransport &&) noexcept = default;
UdpTransport &UdpTransport::operator=(UdpTransport &&other) noexcept
{
    if (this != &other) {
        close();
        mImpl = std::move(other.mImpl);
    }
    return *this;
}

bool UdpTransport::open(std::uint16_t localPort, bool loopbackOnly)
{
    close();
    if (!mImpl) mImpl = std::make_unique<Impl>();
    mImpl->error.clear();
#if defined(_WIN32)
    WSADATA data {};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        mImpl->error = "WSAStartup failed";
        return false;
    }
    mImpl->winsockStarted = true;
#endif
    mImpl->socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (mImpl->socket == kInvalidSocket) {
        mImpl->error = "socket creation failed: " + std::to_string(lastSocketError());
        close();
        return false;
    }

    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(loopbackOnly ? INADDR_LOOPBACK : INADDR_ANY);
    address.sin_port = htons(localPort);
    if (::bind(mImpl->socket, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0) {
        mImpl->error = "UDP bind failed: " + std::to_string(lastSocketError());
        close();
        return false;
    }

#if defined(_WIN32)
    u_long nonBlocking = 1;
    if (ioctlsocket(mImpl->socket, FIONBIO, &nonBlocking) != 0) {
#else
    if (fcntl(mImpl->socket, F_SETFL, fcntl(mImpl->socket, F_GETFL, 0) | O_NONBLOCK) != 0) {
#endif
        mImpl->error = "failed to make UDP socket non-blocking";
        close();
        return false;
    }

    sockaddr_in bound {};
#if defined(_WIN32)
    int boundSize = sizeof(bound);
#else
    socklen_t boundSize = sizeof(bound);
#endif
    if (getsockname(mImpl->socket, reinterpret_cast<sockaddr *>(&bound), &boundSize) != 0) {
        mImpl->error = "getsockname failed";
        close();
        return false;
    }
    mImpl->port = ntohs(bound.sin_port);
    return true;
}

bool UdpTransport::addPeer(std::uint8_t player, const std::string &ipv4Address, std::uint16_t port)
{
    if (player >= kMaxNetplayPeers) {
        mImpl->error = "peer seat out of range";
        return false;
    }
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (inet_pton(AF_INET, ipv4Address.c_str(), &address.sin_addr) != 1) {
        mImpl->error = "invalid IPv4 peer address";
        return false;
    }
    mImpl->peers[player].address = address;
    mImpl->peers[player].valid = true;
    return true;
}

bool UdpTransport::setPeer(const std::string &ipv4Address, std::uint16_t port)
{
    for (std::size_t seat = 0; seat < mImpl->peers.size(); ++seat) {
        if (!mImpl->peers[seat].valid) {
            return addPeer(static_cast<std::uint8_t>(seat), ipv4Address, port);
        }
    }
    mImpl->error = "no free peer seat";
    return false;
}

void UdpTransport::enablePeerDiscovery(bool enabled)
{
    mImpl->discoverPeer = enabled;
}

void UdpTransport::expectPeers(std::size_t count)
{
    mImpl->expectedPeers = std::clamp<std::size_t>(count, 1, kMaxNetplayPeers);
}

bool UdpTransport::sendInputTo(std::uint8_t player, const InputPacket &packet)
{
    if (mImpl->socket == kInvalidSocket || player >= kMaxNetplayPeers || !mImpl->peers[player].valid) {
        mImpl->error = "UDP transport is not connected";
        return false;
    }
    const auto bytes = encode(packet);
    const int sent = sendto(mImpl->socket, reinterpret_cast<const char *>(bytes.data()), static_cast<int>(bytes.size()), 0,
        reinterpret_cast<const sockaddr *>(&mImpl->peers[player].address), sizeof(sockaddr_in));
    if (sent != static_cast<int>(bytes.size())) {
        mImpl->error = "UDP send failed: " + std::to_string(lastSocketError());
        return false;
    }
    return true;
}

bool UdpTransport::sendInput(const InputPacket &packet)
{
    if (mImpl->socket == kInvalidSocket) {
        mImpl->error = "UDP transport is not connected";
        return false;
    }
    // One unreachable guest must not stop the frame reaching the other two, so a
    // send counts as done when it reached anybody. The caller learns nothing about
    // which link failed; that is the session layer's job, from missing inputs.
    bool any = false;
    bool anyPeer = false;
    for (std::size_t seat = 0; seat < mImpl->peers.size(); ++seat) {
        if (!mImpl->peers[seat].valid) continue;
        anyPeer = true;
        if (sendInputTo(static_cast<std::uint8_t>(seat), packet)) any = true;
    }
    if (!anyPeer) {
        mImpl->error = "UDP transport is not connected";
        return false;
    }
    return any;
}

bool UdpTransport::receiveInput(InputPacket &packet)
{
    if (!mImpl || mImpl->socket == kInvalidSocket) return false;
    std::array<std::uint8_t, 256> bytes {};
    sockaddr_in sender {};
#if defined(_WIN32)
    int senderSize = sizeof(sender);
#else
    socklen_t senderSize = sizeof(sender);
#endif
    const int received = recvfrom(mImpl->socket, reinterpret_cast<char *>(bytes.data()), static_cast<int>(bytes.size()), 0,
        reinterpret_cast<sockaddr *>(&sender), &senderSize);
    if (received < 0) {
        const int error = lastSocketError();
        if (wouldBlock(error)) {
            return false;
        }
        mImpl->error = "UDP receive failed: " + std::to_string(error);
        return false;
    }
    // Which registered seat this came from. A packet from nobody we know is
    // dropped unless discovery is on, exactly as before -- the difference is that
    // "nobody we know" is now a search rather than a single comparison.
    std::uint8_t from = kMaxNetplayPeers;
    for (std::size_t seat = 0; seat < mImpl->peers.size(); ++seat) {
        const auto &peer = mImpl->peers[seat];
        if (peer.valid && peer.address.sin_addr.s_addr == sender.sin_addr.s_addr
            && peer.address.sin_port == sender.sin_port) {
            from = static_cast<std::uint8_t>(seat);
            break;
        }
    }
    const bool known = from < kMaxNetplayPeers;
    if (!known && !mImpl->discoverPeer) {
        return false;
    }
    if (!decode(bytes.data(), static_cast<std::size_t>(received), packet)) {
        return false;
    }
    if (!known) {
        // Discovery closes as soon as the expected peers are seated. Without this a
        // host would keep accepting strangers for the rest of the session; with two
        // players that was prevented by there being exactly one peer to compare to.
        if (peerCount() >= mImpl->expectedPeers) {
            return false;
        }
        // Discovery seats a new peer by the author byte it announced, which decode
        // has already bounded. A host binds one port and learns each guest from its
        // first packet; this is the same trust as the two-player version had, and
        // the same caveat -- the authenticated channel that carries these is the
        // thing that makes it safe, not this function.
        if (mImpl->peers[packet.player].valid) {
            return false;
        }
        mImpl->peers[packet.player].address = sender;
        mImpl->peers[packet.player].valid = true;
        from = packet.player;
    }
    mImpl->lastSender = from;
    return true;
}

void UdpTransport::close()
{
    if (!mImpl) return;
    if (mImpl->socket != kInvalidSocket) {
        closeSocket(mImpl->socket);
        mImpl->socket = kInvalidSocket;
    }
    mImpl->peers = {};
    mImpl->lastSender = kMaxNetplayPeers;
    mImpl->expectedPeers = 1;
    mImpl->discoverPeer = false;
    mImpl->port = 0;
#if defined(_WIN32)
    if (mImpl->winsockStarted) {
        WSACleanup();
        mImpl->winsockStarted = false;
    }
#endif
}

std::uint16_t UdpTransport::localPort() const { return mImpl->port; }
bool UdpTransport::hasPeer() const { return peerCount() != 0; }
bool UdpTransport::hasPeer(std::uint8_t player) const
{
    return player < kMaxNetplayPeers && mImpl->peers[player].valid;
}
std::size_t UdpTransport::peerCount() const
{
    std::size_t count = 0;
    for (const auto &peer : mImpl->peers) if (peer.valid) ++count;
    return count;
}
std::uint8_t UdpTransport::lastSenderPlayer() const { return mImpl->lastSender; }
const std::string &UdpTransport::lastError() const { return mImpl->error; }

// A real four-player star over loopback: three guests, one host, every guest
// discovered from its own first packet and seated by the author byte it
// announced. This is the shape the lobby will drive, and it is checked here
// because everything above it is easier to debug once this is known good.
//
// It also pins the two properties the two-player code got for free and that a
// peer table has to earn back: a stranger is refused once the expected guests
// are seated, and the host can tell which link a packet arrived on rather than
// trusting the author byte, which is the only way a relay avoids echoing a
// packet back to its sender.
bool runStarSelfTest()
{
    UdpTransport host;
    if (!host.open()) return false;
    host.enablePeerDiscovery(true);
    host.expectPeers(3);

    std::array<UdpTransport, 3> guests;
    for (std::size_t index = 0; index < guests.size(); ++index) {
        if (!guests[index].open() || !guests[index].setPeer("127.0.0.1", host.localPort())) return false;
    }

    // Each guest announces itself once; the host must end up with three distinct
    // seats, in the seats the guests claimed rather than in arrival order.
    for (std::size_t index = 0; index < guests.size(); ++index) {
        InputPacket hello {};
        hello.sessionId = 0x4D503452u;
        hello.frame = 1;
        hello.player = static_cast<std::uint8_t>(index + 1);
        hello.type = PacketType::Input;
        hello.state.frame = kNoHashFrame;
        hello.state.version = kStateHashVersion;
        if (!guests[index].sendInput(hello)) return false;
    }

    std::array<bool, kMaxNetplayPeers> seen {};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    std::size_t received = 0;
    while (received < guests.size() && std::chrono::steady_clock::now() < deadline) {
        InputPacket packet {};
        if (host.receiveInput(packet)) {
            if (packet.player == 0 || packet.player > guests.size()) return false;
            // Sender seat and announced author agree here because the guests are
            // honest; what matters is that the host can name the link at all.
            if (host.lastSenderPlayer() != packet.player) return false;
            if (seen[packet.player]) return false;
            seen[packet.player] = true;
            ++received;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    if (received != guests.size() || host.peerCount() != guests.size()) return false;
    for (std::uint8_t seat = 1; seat <= guests.size(); ++seat) {
        if (!host.hasPeer(seat)) return false;
    }
    if (host.hasPeer(0)) return false;

    // A fourth socket arriving after the expected guests are seated is refused,
    // exactly as the third socket was refused when there were only two players.
    UdpTransport intruder;
    InputPacket forged {};
    forged.sessionId = 0x4D503452u;
    forged.frame = 2;
    forged.player = 0;
    forged.type = PacketType::Input;
    forged.state.frame = kNoHashFrame;
    forged.state.version = kStateHashVersion;
    if (!intruder.open() || !intruder.setPeer("127.0.0.1", host.localPort())
        || !intruder.sendInput(forged)) return false;
    const auto rejectDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(30);
    while (std::chrono::steady_clock::now() < rejectDeadline) {
        InputPacket packet {};
        if (host.receiveInput(packet)) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (host.peerCount() != guests.size()) return false;

    // One broadcast from the host reaches all three guests.
    InputPacket broadcast {};
    broadcast.sessionId = 0x4D503452u;
    broadcast.frame = 3;
    broadcast.player = 0;
    broadcast.sequence = 42;
    broadcast.type = PacketType::Input;
    broadcast.state.frame = kNoHashFrame;
    broadcast.state.version = kStateHashVersion;
    if (!host.sendInput(broadcast)) return false;
    for (std::size_t index = 0; index < guests.size(); ++index) {
        bool got = false;
        const auto guestDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        while (!got && std::chrono::steady_clock::now() < guestDeadline) {
            InputPacket packet {};
            if (guests[index].receiveInput(packet)) {
                if (packet.sequence != broadcast.sequence || packet.player != 0) return false;
                got = true;
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        if (!got) return false;
    }

    // And a targeted send reaches only the guest it names.
    InputPacket direct = broadcast;
    direct.sequence = 43;
    if (!host.sendInputTo(2, direct)) return false;
    bool reachedTwo = false;
    const auto directDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
    while (std::chrono::steady_clock::now() < directDeadline) {
        InputPacket packet {};
        if (guests[1].receiveInput(packet) && packet.sequence == direct.sequence) { reachedTwo = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!reachedTwo) return false;
    for (const std::size_t other : {std::size_t{0}, std::size_t{2}}) {
        InputPacket packet {};
        if (guests[other].receiveInput(packet)) return false;
    }
    return true;
}

bool runTransportSelfTest()
{
    InputPacket codecSample {};
    codecSample.captureContext = 0xFEDCBA98u;
    auto bytes = encode(codecSample);
    InputPacket decoded {};
    if (!decode(bytes.data(), bytes.size(), decoded)
        || decoded.captureContext != codecSample.captureContext) return false;
    bytes[44] ^= 1; // Context metadata is covered by the packet checksum.
    if (decode(bytes.data(), bytes.size(), decoded)) return false;
    bytes = encode(codecSample);
    put16(bytes.data() + 4, 3); // Old timeline protocol, otherwise valid checksum.
    put32(bytes.data() + 148, packetHash(bytes.data(), 148));
    if (decode(bytes.data(), bytes.size(), decoded)) return false;

    for (const auto type : {PacketType::Input, PacketType::Retransmit, PacketType::State}) {
        codecSample.type = type;
        codecSample.hashAckNext = 8;
        codecSample.state = {7, 3, 0x11223344, 0x55667788, 6, kStateHashVersion, 0x0123456789abcdefull};
        // Distinct per-subsystem hashes must survive the round trip intact.
        for (std::size_t index = 0; index < kSubsystemCount; ++index)
            codecSample.state.parts[index] = 0xA5000000u + static_cast<std::uint32_t>(index) * 0x01010101u;
        bytes = encode(codecSample);
        if (!decode(bytes.data(), bytes.size(), decoded) || decoded.type != type
            || decoded.state != codecSample.state || decoded.hashAckNext != 8
            || decoded.state.parts != codecSample.state.parts) return false;
    }
    // A single altered subsystem hash must be both checksum-detected and, once
    // re-signed, visible as a difference rather than silently equal.
    bytes = encode(codecSample);
    bytes[84 + 4 * static_cast<std::size_t>(Subsystem::Board)] ^= 0x40;
    if (decode(bytes.data(), bytes.size(), decoded)) return false;
    put32(bytes.data() + 148, packetHash(bytes.data(), 148));
    if (!decode(bytes.data(), bytes.size(), decoded)
        || decoded.state == codecSample.state
        || decoded.state.firstDifferentPart(codecSample.state)
            != static_cast<std::size_t>(Subsystem::Board)) return false;
    for (const unsigned offset : {5u, 6u, 7u, 48u, 80u}) {
        bytes = encode(codecSample);
        bytes[offset] = 99; // Valid checksum must not authorize an unknown schema/type/player.
        put32(bytes.data() + 148, packetHash(bytes.data(), 148));
        if (decode(bytes.data(), bytes.size(), decoded)) return false;
    }
    bytes = encode(codecSample);
    if (decode(bytes.data(), bytes.size() - 1, decoded)) return false;
    codecSample.state.frame = kNoHashFrame;
    bytes = encode(codecSample);
    if (decode(bytes.data(), bytes.size(), decoded)) return false;

    if (!runStarSelfTest()) return false;

    UdpTransport host;
    UdpTransport client;
    if (!host.open() || !client.open()
        || !client.setPeer("127.0.0.1", host.localPort())) {
        return false;
    }
    host.enablePeerDiscovery(true);

    InputPacket expected;
    expected.sessionId = 0x4D503452u;
    expected.sequence = 73;
    expected.frame = 1204;
    expected.player = 1;
    expected.input = { 0x4120, -37, 82, -11, 29, 201, 144 };
    expected.configSignature = 0x4E500002u;
    expected.frandSeed = 0x13579BDFu;
    expected.rand8Seed = 0x2468ACE0u;
    expected.type = PacketType::Retransmit;
    expected.hashAckNext = 97;
    expected.state = {96, 7, 0x10203040u, 0x50607080u, 95, kStateHashVersion, 0x89abcdef01234567ull};
    expected.captureContext = 0x12345678u;
    if (!client.sendInput(expected)) {
        return false;
    }

    InputPacket actual;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
    while (std::chrono::steady_clock::now() < deadline) {
        if (host.receiveInput(actual)) {
            const bool requestMatches = host.hasPeer()
                && actual.sessionId == expected.sessionId && actual.sequence == expected.sequence
                && actual.frame == expected.frame && actual.player == expected.player
                && actual.configSignature == expected.configSignature
                && actual.frandSeed == expected.frandSeed
                && actual.rand8Seed == expected.rand8Seed
                && actual.type == expected.type && actual.state == expected.state
                && actual.hashAckNext == expected.hashAckNext
                && actual.captureContext == expected.captureContext
                && rollback::inputsEqual(actual.input, expected.input);
            if (!requestMatches) {
                return false;
            }
            // A third socket cannot inject input into an established pair.
            UdpTransport intruder;
            if (!intruder.open() || !intruder.setPeer("127.0.0.1", host.localPort())
                || !intruder.sendInput(expected)) return false;
            const auto rejectDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(20);
            while (std::chrono::steady_clock::now() < rejectDeadline) {
                if (host.receiveInput(actual)) return false;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            InputPacket response = expected;
            response.sequence++;
            response.player = 0;
            if (!host.sendInput(response)) {
                return false;
            }
            const auto responseDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
            while (std::chrono::steady_clock::now() < responseDeadline) {
                if (client.receiveInput(actual)) {
                    return actual.sequence == response.sequence && actual.player == response.player
                        && rollback::inputsEqual(actual.input, response.input);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

}

extern "C" bool PartyBoard_NetTransportRunSelfTest(void)
{
    const bool passed = partyboard::netplay::runTransportSelfTest();
    OSReport("Netplay UDP loopback self-test: %s\n", passed ? "PASS" : "FAIL");
    return passed;
}
