#ifndef PARTYBOARD_PORT_NETPLAY_TRANSPORT_HPP
#define PARTYBOARD_PORT_NETPLAY_TRANSPORT_HPP

#include "port/rollback.h"
#include "port/netplay_state.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace partyboard::netplay {

// v6 separates input/RTX/state packets and carries versioned canonical state.
// v7 appends one hash per gameplay subsystem, so the first divergent frame also
// names the subsystem without an extra round trip.
constexpr std::uint16_t kProtocolVersion = 7;
constexpr std::size_t kNetplayPacketSize = 152;
// Four, because the game seats four and rollback.hpp already sizes every input
// array to kMaxPlayers. A static_assert in the implementation keeps the two
// numbers married; this one exists so the header does not have to drag the
// rollback C++ header in behind it.
constexpr std::size_t kMaxNetplayPeers = 4;
enum class PacketType : std::uint8_t { Input = 1, Retransmit = 2, State = 3 };

struct InputPacket {
    std::uint32_t sessionId = 0;
    std::uint32_t sequence = 0;
    std::uint32_t frame = 0;
    std::uint8_t player = 0;
    PartyBoardRollbackInput input {};
    std::uint32_t configSignature = 0;
    std::uint32_t frandSeed = 0;
    std::uint32_t rand8Seed = 0;
    PacketType type = PacketType::Input;
    StateDigest state {};
    std::uint32_t hashAckNext = 0; // Exclusive count of equal canonical states.
    std::uint32_t captureContext = 0;
};

class UdpTransport {
public:
    UdpTransport();
    ~UdpTransport();
    UdpTransport(UdpTransport &&) noexcept;
    UdpTransport &operator=(UdpTransport &&) noexcept;
    UdpTransport(const UdpTransport &) = delete;
    UdpTransport &operator=(const UdpTransport &) = delete;

    bool open(std::uint16_t localPort = 0, bool loopbackOnly = false);

    // Peers are held per seat, 0..kMaxNetplayPeers-1, because that is the only
    // identity the rest of the stack has: InputPacket already names its author,
    // and a star host has to know which link a packet arrived on to avoid
    // relaying it straight back to its sender.
    bool addPeer(std::uint8_t player, const std::string &ipv4Address, std::uint16_t port);
    // The two-player spelling, kept because it says what it means when there is
    // exactly one other player: registers that peer in the first free seat.
    bool setPeer(const std::string &ipv4Address, std::uint16_t port);
    void enablePeerDiscovery(bool enabled);
    // How many peers discovery may seat before it stops accepting strangers. One
    // by default, which is what the two-player host always did implicitly: learn
    // the guest, then refuse everyone else. A star host raises it to the number of
    // guests it agreed to, and not one more.
    void expectPeers(std::size_t count);

    // Sends to every registered peer. A guest in a star has one; the host has up
    // to three, and one failed link must not silence the others, so this reports
    // failure only when no peer at all could be reached.
    bool sendInput(const InputPacket &packet);
    bool sendInputTo(std::uint8_t player, const InputPacket &packet);
    bool receiveInput(InputPacket &packet);
    void close();

    std::uint16_t localPort() const;
    bool hasPeer() const;
    std::size_t peerCount() const;
    bool hasPeer(std::uint8_t player) const;
    // Which seat the last accepted packet arrived from, or kMaxNetplayPeers when
    // no packet has been accepted yet. The host relays by sender, not by author:
    // a guest lies about the author byte far more easily than about its socket.
    std::uint8_t lastSenderPlayer() const;
    const std::string &lastError() const;

private:
    struct Impl;
    std::unique_ptr<Impl> mImpl;
};

bool runTransportSelfTest();

}

extern "C" bool PartyBoard_NetTransportRunSelfTest(void);

#endif
