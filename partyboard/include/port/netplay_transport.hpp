#ifndef PARTYBOARD_PORT_NETPLAY_TRANSPORT_HPP
#define PARTYBOARD_PORT_NETPLAY_TRANSPORT_HPP

#include "port/rollback.h"

#include <cstdint>
#include <memory>
#include <string>

namespace partyboard::netplay {

constexpr std::uint16_t kProtocolVersion = 2;

struct InputPacket {
    std::uint32_t sessionId = 0;
    std::uint32_t sequence = 0;
    std::uint32_t frame = 0;
    std::uint8_t player = 0;
    PartyBoardRollbackInput input {};
    std::uint32_t configSignature = 0;
    std::uint32_t frandSeed = 0;
    std::uint32_t rand8Seed = 0;
    std::uint32_t stateChecksum = 0;
};

class UdpTransport {
public:
    UdpTransport();
    ~UdpTransport();
    UdpTransport(UdpTransport &&) noexcept;
    UdpTransport &operator=(UdpTransport &&) noexcept;
    UdpTransport(const UdpTransport &) = delete;
    UdpTransport &operator=(const UdpTransport &) = delete;

    bool open(std::uint16_t localPort = 0);
    bool setPeer(const std::string &ipv4Address, std::uint16_t port);
    void enablePeerDiscovery(bool enabled);
    bool sendInput(const InputPacket &packet);
    bool receiveInput(InputPacket &packet);
    void close();

    std::uint16_t localPort() const;
    bool hasPeer() const;
    const std::string &lastError() const;

private:
    struct Impl;
    std::unique_ptr<Impl> mImpl;
};

bool runTransportSelfTest();

}

extern "C" bool PartyBoard_NetTransportRunSelfTest(void);

#endif
