#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace partyboard::online {

// Native port of the PB4 invitation of the Windows companion
// (tools/online/Connection.cs, class Invitation). The wire form must stay
// byte-identical: a phone and a PC exchange these codes, and either side may
// be the one that encoded it.
//
// Layout, 49 bytes, little-endian integers (.NET BinaryWriter), then
// base64url with its padding kept, behind the "PB4." prefix: 72 characters.
//   address[4] port:u16 expires:u32 fingerprint[16] token[16]
//   localAddress[4] localPort:u16 maxPlayers:u8
using Ipv4 = std::array<std::uint8_t, 4>;

constexpr int kMaxSeats = 4;
constexpr std::size_t kInvitationTextLength = 72;
constexpr std::size_t kInvitationBinaryLength = 49;

struct Invitation {
    Ipv4 address {};
    std::uint16_t port = 0;
    std::uint32_t expiresUnix = 0;
    std::array<std::uint8_t, 16> fingerprint {};
    std::array<std::uint8_t, 16> token {};
    Ipv4 localAddress {};
    std::uint16_t localPort = 0;
    int maxPlayers = 2;

    bool has_local_path() const noexcept;
};

enum class InvitationError {
    None,
    // "PB2." / "PB3.": a code from an older PartyBoard, refused by name.
    OldVersion,
    // Wrong length, prefix, base64, player count, address or port.
    Malformed,
    Expired,
};

struct DecodedInvitation {
    std::optional<Invitation> invitation;
    InvitationError error = InvitationError::None;
};

bool is_private_address(const Ipv4 &address) noexcept;
bool is_public_address(const Ipv4 &address) noexcept;

// Empty when the invitation cannot be encoded (player count out of range).
std::string encode_invitation(const Invitation &invitation);

// nowUnix is the current UTC time in seconds; localTest allows a non-public
// host address, exactly like the companion's own loopback tests.
DecodedInvitation decode_invitation(std::string_view text, std::uint32_t nowUnix, bool localTest = false);

// English message for a decode error, suitable for ui_translate.
const char *invitation_error_message(InvitationError error) noexcept;

} // namespace partyboard::online
