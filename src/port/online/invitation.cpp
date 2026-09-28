#include "port/online/invitation.hpp"

#include <cstring>

namespace partyboard::online {
namespace {

    constexpr std::string_view kPrefix = "PB4.";
    constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    // Same bound as the companion: an invitation cannot claim to be valid for
    // longer than a lobby lives.
    constexpr std::uint32_t kMaxLifetimeSeconds = 31 * 60;

    void put16(std::uint8_t *out, std::uint16_t v) noexcept
    {
        out[0] = static_cast<std::uint8_t>(v);
        out[1] = static_cast<std::uint8_t>(v >> 8);
    }

    void put32(std::uint8_t *out, std::uint32_t v) noexcept
    {
        for (int i = 0; i < 4; ++i) {
            out[i] = static_cast<std::uint8_t>(v >> (8 * i));
        }
    }

    std::uint16_t get16(const std::uint8_t *in) noexcept
    {
        return static_cast<std::uint16_t>(in[0] | (in[1] << 8));
    }

    std::uint32_t get32(const std::uint8_t *in) noexcept
    {
        return static_cast<std::uint32_t>(in[0]) | (static_cast<std::uint32_t>(in[1]) << 8) | (static_cast<std::uint32_t>(in[2]) << 16)
            | (static_cast<std::uint32_t>(in[3]) << 24);
    }

    int base64_value(char c) noexcept
    {
        if (c >= 'A' && c <= 'Z') {
            return c - 'A';
        }
        if (c >= 'a' && c <= 'z') {
            return c - 'a' + 26;
        }
        if (c >= '0' && c <= '9') {
            return c - '0' + 52;
        }
        if (c == '-') {
            return 62;
        }
        if (c == '_') {
            return 63;
        }
        return -1;
    }

    std::string trim(std::string_view text)
    {
        const auto isSpace = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
        while (!text.empty() && isSpace(text.front())) {
            text.remove_prefix(1);
        }
        while (!text.empty() && isSpace(text.back())) {
            text.remove_suffix(1);
        }
        return std::string(text);
    }

} // namespace

bool is_private_address(const Ipv4 &b) noexcept
{
    return b[0] == 10 || (b[0] == 172 && b[1] >= 16 && b[1] <= 31) || (b[0] == 192 && b[1] == 168);
}

bool is_public_address(const Ipv4 &b) noexcept
{
    // Mirrors Gateway.Public: no private, loopback, multicast, CGNAT,
    // link-local, benchmark or documentation range.
    return !is_private_address(b) && b[0] != 0 && b[0] != 127 && b[0] < 224 && !(b[0] == 100 && b[1] >= 64 && b[1] <= 127)
        && !(b[0] == 169 && b[1] == 254) && !(b[0] == 192 && (b[1] == 0 || b[1] == 2))
        && !(b[0] == 198 && (b[1] == 18 || b[1] == 19 || (b[1] == 51 && b[2] == 100))) && !(b[0] == 203 && b[1] == 0 && b[2] == 113);
}

bool Invitation::has_local_path() const noexcept
{
    const Ipv4 any {};
    return localPort > 0 && localAddress != any && localAddress != address;
}

std::string encode_invitation(const Invitation &invitation)
{
    if (invitation.maxPlayers < 2 || invitation.maxPlayers > kMaxSeats) {
        return {};
    }

    std::uint8_t data[kInvitationBinaryLength] {};
    std::uint8_t *p = data;
    std::memcpy(p, invitation.address.data(), 4);
    p += 4;
    put16(p, invitation.port);
    p += 2;
    put32(p, invitation.expiresUnix);
    p += 4;
    std::memcpy(p, invitation.fingerprint.data(), 16);
    p += 16;
    std::memcpy(p, invitation.token.data(), 16);
    p += 16;
    std::memcpy(p, invitation.localAddress.data(), 4);
    p += 4;
    put16(p, invitation.localPort);
    p += 2;
    *p = static_cast<std::uint8_t>(invitation.maxPlayers);

    // .NET Convert.ToBase64String keeps the '=' padding; the companion only
    // swaps '+' and '/' for the URL-safe pair, so the padding stays too.
    std::string out(kPrefix);
    for (std::size_t i = 0; i < kInvitationBinaryLength; i += 3) {
        const std::size_t remaining = kInvitationBinaryLength - i;
        const std::uint32_t chunk = (static_cast<std::uint32_t>(data[i]) << 16)
            | (remaining > 1 ? static_cast<std::uint32_t>(data[i + 1]) << 8 : 0u) | (remaining > 2 ? data[i + 2] : 0u);
        out += kAlphabet[(chunk >> 18) & 63];
        out += kAlphabet[(chunk >> 12) & 63];
        out += remaining > 1 ? kAlphabet[(chunk >> 6) & 63] : '=';
        out += remaining > 2 ? kAlphabet[chunk & 63] : '=';
    }
    return out;
}

DecodedInvitation decode_invitation(std::string_view rawText, std::uint32_t nowUnix, bool localTest)
{
    const std::string text = trim(rawText);
    if (text.starts_with("PB2.") || text.starts_with("PB3.")) {
        return { std::nullopt, InvitationError::OldVersion };
    }
    if (text.size() != kInvitationTextLength || !text.starts_with(kPrefix)) {
        return { std::nullopt, InvitationError::Malformed };
    }

    const std::string_view body = std::string_view(text).substr(kPrefix.size());
    std::uint8_t data[kInvitationBinaryLength + 2] {};
    std::size_t written = 0;
    for (std::size_t i = 0; i < body.size(); i += 4) {
        int values[4];
        int padding = 0;
        for (int j = 0; j < 4; ++j) {
            const char c = body[i + j];
            if (c == '=') {
                // Padding is only legal at the very end.
                if (i + 4 != body.size() || j < 2) {
                    return { std::nullopt, InvitationError::Malformed };
                }
                values[j] = 0;
                ++padding;
                continue;
            }
            if (padding > 0 || (values[j] = base64_value(c)) < 0) {
                return { std::nullopt, InvitationError::Malformed };
            }
        }
        const std::uint32_t chunk = (values[0] << 18) | (values[1] << 12) | (values[2] << 6) | values[3];
        const int bytes = 3 - padding;
        for (int j = 0; j < bytes; ++j) {
            if (written >= sizeof(data)) {
                return { std::nullopt, InvitationError::Malformed };
            }
            data[written++] = static_cast<std::uint8_t>(chunk >> (16 - 8 * j));
        }
    }
    if (written != kInvitationBinaryLength) {
        return { std::nullopt, InvitationError::Malformed };
    }

    Invitation invitation;
    const std::uint8_t *p = data;
    std::memcpy(invitation.address.data(), p, 4);
    p += 4;
    invitation.port = get16(p);
    p += 2;
    invitation.expiresUnix = get32(p);
    p += 4;
    std::memcpy(invitation.fingerprint.data(), p, 16);
    p += 16;
    std::memcpy(invitation.token.data(), p, 16);
    p += 16;
    std::memcpy(invitation.localAddress.data(), p, 4);
    p += 4;
    invitation.localPort = get16(p);
    p += 2;
    invitation.maxPlayers = *p;

    if (invitation.maxPlayers < 2 || invitation.maxPlayers > kMaxSeats) {
        return { std::nullopt, InvitationError::Malformed };
    }
    if (invitation.port == 0 || (!localTest && !is_public_address(invitation.address))) {
        return { std::nullopt, InvitationError::Malformed };
    }
    // The local address only ever points inside the host's own network; any
    // other value is dropped rather than dialled.
    if (invitation.localPort != 0 && !is_private_address(invitation.localAddress)) {
        invitation.localAddress = {};
        invitation.localPort = 0;
    }
    if (invitation.expiresUnix < nowUnix) {
        return { std::nullopt, InvitationError::Expired };
    }
    if (invitation.expiresUnix > nowUnix + kMaxLifetimeSeconds) {
        return { std::nullopt, InvitationError::Malformed };
    }
    return { invitation, InvitationError::None };
}

const char *invitation_error_message(InvitationError error) noexcept
{
    switch (error) {
        case InvitationError::None:
            return "Invitation is valid.";
        case InvitationError::OldVersion:
            return "This invitation comes from an older version of Party Board. Both players need the same version.";
        case InvitationError::Expired:
            return "This invitation has expired. The host must create a new lobby.";
        case InvitationError::Malformed:
        default:
            return "The invitation is incomplete. Copy all of it from your friend's device.";
    }
}

} // namespace partyboard::online
