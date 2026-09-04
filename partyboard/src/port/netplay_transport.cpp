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
namespace {

constexpr std::size_t kPacketSize = 48;
constexpr std::array<std::uint8_t, 4> kMagic { 'P', 'B', 'R', 'B' };
constexpr std::uint8_t kInputPacketType = 1;

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
    bytes[6] = kInputPacketType;
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
    put32(bytes.data() + 40, packet.stateChecksum);
    put32(bytes.data() + 44, packetHash(bytes.data(), 44));
    return bytes;
}

bool decode(const std::uint8_t *bytes, std::size_t size, InputPacket &packet)
{
    if (size != kPacketSize || !std::equal(kMagic.begin(), kMagic.end(), bytes)
        || get16(bytes + 4) != kProtocolVersion || bytes[6] != kInputPacketType
        || get32(bytes + 44) != packetHash(bytes, 44)) {
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
    packet.stateChecksum = get32(bytes + 40);
    return packet.player < 4;
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
    Socket socket = kInvalidSocket;
    sockaddr_in peer {};
    bool hasPeer = false;
    bool discoverPeer = false;
    std::uint16_t port = 0;
    std::string error;
};

UdpTransport::UdpTransport()
    : mImpl(std::make_unique<Impl>())
{
}

UdpTransport::~UdpTransport() { close(); }
UdpTransport::UdpTransport(UdpTransport &&) noexcept = default;
UdpTransport &UdpTransport::operator=(UdpTransport &&) noexcept = default;

bool UdpTransport::open(std::uint16_t localPort)
{
    close();
#if defined(_WIN32)
    WSADATA data {};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        mImpl->error = "WSAStartup failed";
        return false;
    }
#endif
    mImpl->socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (mImpl->socket == kInvalidSocket) {
        mImpl->error = "socket creation failed: " + std::to_string(lastSocketError());
        return false;
    }

    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
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

bool UdpTransport::setPeer(const std::string &ipv4Address, std::uint16_t port)
{
    mImpl->peer = {};
    mImpl->peer.sin_family = AF_INET;
    mImpl->peer.sin_port = htons(port);
    if (inet_pton(AF_INET, ipv4Address.c_str(), &mImpl->peer.sin_addr) != 1) {
        mImpl->error = "invalid IPv4 peer address";
        return false;
    }
    mImpl->hasPeer = true;
    return true;
}

void UdpTransport::enablePeerDiscovery(bool enabled)
{
    mImpl->discoverPeer = enabled;
}

bool UdpTransport::sendInput(const InputPacket &packet)
{
    if (mImpl->socket == kInvalidSocket || !mImpl->hasPeer) {
        mImpl->error = "UDP transport is not connected";
        return false;
    }
    const auto bytes = encode(packet);
    const int sent = sendto(mImpl->socket, reinterpret_cast<const char *>(bytes.data()), static_cast<int>(bytes.size()), 0,
        reinterpret_cast<const sockaddr *>(&mImpl->peer), sizeof(mImpl->peer));
    if (sent != static_cast<int>(bytes.size())) {
        mImpl->error = "UDP send failed: " + std::to_string(lastSocketError());
        return false;
    }
    return true;
}

bool UdpTransport::receiveInput(InputPacket &packet)
{
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
    if (!decode(bytes.data(), static_cast<std::size_t>(received), packet)) {
        return false;
    }
    if (mImpl->discoverPeer && !mImpl->hasPeer) {
        mImpl->peer = sender;
        mImpl->hasPeer = true;
    }
    return true;
}

void UdpTransport::close()
{
    if (mImpl->socket != kInvalidSocket) {
        closeSocket(mImpl->socket);
        mImpl->socket = kInvalidSocket;
    }
    mImpl->hasPeer = false;
    mImpl->port = 0;
}

std::uint16_t UdpTransport::localPort() const { return mImpl->port; }
bool UdpTransport::hasPeer() const { return mImpl->hasPeer; }
const std::string &UdpTransport::lastError() const { return mImpl->error; }

bool runTransportSelfTest()
{
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
    expected.stateChecksum = 0x89ABCDEFu;
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
                && actual.stateChecksum == expected.stateChecksum
                && rollback::inputsEqual(actual.input, expected.input);
            if (!requestMatches) {
                return false;
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
