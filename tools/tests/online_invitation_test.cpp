// Native PB4 invitation codec, checked against the rules of the Windows
// companion (tools/online/Tests.cs). Standalone: build with
//   c++ -std=c++20 -Iinclude tools/tests/online_invitation_test.cpp src/port/online/invitation.cpp
#include "port/online/invitation.hpp"

#include <cstdio>
#include <cstdlib>

using namespace partyboard::online;

namespace {

int failures = 0;

void check(bool condition, const char *what)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

constexpr std::uint32_t kNow = 1899999000;

Invitation sample()
{
    Invitation invite;
    invite.address = { 8, 8, 4, 4 };
    invite.port = 32000;
    invite.expiresUnix = 1900000000;
    for (int i = 0; i < 16; ++i) {
        invite.fingerprint[i] = static_cast<std::uint8_t>(i);
        invite.token[i] = static_cast<std::uint8_t>(100 + i);
    }
    invite.localAddress = { 192, 168, 1, 14 };
    invite.localPort = 32100;
    invite.maxPlayers = 3;
    return invite;
}

} // namespace

int main()
{
    // Golden vector: the same fields written by .NET BinaryWriter and
    // Convert.ToBase64String, as PartyBoardOnline.exe does.
    const std::string golden = "PB4.CAgEBAB9ALM_cQABAgMEBQYHCAkKCwwNDg9kZWZnaGlqa2xtbm9wcXJzwKgBDmR9Aw==";
    const std::string encoded = encode_invitation(sample());
    check(encoded == golden, "encoding matches the Windows companion byte for byte");
    check(encoded.size() == kInvitationTextLength, "compact invitation length");

    auto decoded = decode_invitation(golden, kNow);
    check(decoded.invitation.has_value(), "golden invitation decodes");
    if (decoded.invitation) {
        const auto &i = *decoded.invitation;
        check(i.port == 32000 && i.maxPlayers == 3 && i.token == sample().token && i.fingerprint == sample().fingerprint,
            "invitation round trip");
        check(i.has_local_path() && i.localPort == 32100, "private local address survives the round trip");
    }
    check(decode_invitation("  " + golden + "\r\n", kNow).invitation.has_value(), "surrounding whitespace is ignored");

    auto invite = sample();
    invite.maxPlayers = 1;
    check(encode_invitation(invite).empty(), "a salon of one cannot be encoded");
    invite.maxPlayers = 5;
    check(encode_invitation(invite).empty(), "a fifth seat cannot be encoded either");
    invite.maxPlayers = 4;
    check(decode_invitation(encode_invitation(invite), kNow).invitation->maxPlayers == 4, "a chosen player count survives");

    check(decode_invitation("PB3." + std::string(64, 'A'), kNow).error == InvitationError::OldVersion, "previous format refused by name");
    check(decode_invitation("PB2." + std::string(56, 'A'), kNow).error == InvitationError::OldVersion, "older format refused by name");
    check(decode_invitation("bad", kNow).error == InvitationError::Malformed, "malformed invite");
    check(decode_invitation(std::string(1000, 'x'), kNow).error == InvitationError::Malformed, "bounded invite");
    check(decode_invitation("PB4." + std::string(68, '!'), kNow).error == InvitationError::Malformed, "invalid base64 refused");

    invite = sample();
    invite.localAddress = { 9, 9, 9, 9 };
    check(!decode_invitation(encode_invitation(invite), kNow).invitation->has_local_path(), "public local address is discarded");
    invite.localAddress = { 127, 0, 0, 1 };
    check(!decode_invitation(encode_invitation(invite), kNow).invitation->has_local_path(), "loopback local address is discarded");

    invite = sample();
    invite.address = { 127, 0, 0, 1 };
    check(decode_invitation(encode_invitation(invite), kNow).error == InvitationError::Malformed, "loopback invite rejected");
    check(decode_invitation(encode_invitation(invite), kNow, true).invitation.has_value(), "loopback allowed for local tests");
    invite.address = { 8, 8, 8, 8 };
    invite.expiresUnix = kNow - 1;
    check(decode_invitation(encode_invitation(invite), kNow).error == InvitationError::Expired, "expired invite");
    invite.expiresUnix = kNow + 32 * 60;
    check(decode_invitation(encode_invitation(invite), kNow).error == InvitationError::Malformed, "too long a lifetime is refused");
    invite = sample();
    invite.port = 0;
    check(decode_invitation(encode_invitation(invite), kNow).error == InvitationError::Malformed, "port zero refused");

    for (const Ipv4 &addr : { Ipv4 { 0, 1, 2, 3 }, Ipv4 { 10, 0, 0, 1 }, Ipv4 { 127, 0, 0, 1 }, Ipv4 { 100, 64, 0, 1 }, Ipv4 { 192, 168, 1, 1 },
             Ipv4 { 169, 254, 1, 1 }, Ipv4 { 198, 18, 0, 1 }, Ipv4 { 203, 0, 113, 1 }, Ipv4 { 224, 0, 0, 1 } }) {
        check(!is_public_address(addr), "public address filter");
    }
    check(is_public_address({ 8, 8, 8, 8 }), "a real public address passes");

    if (failures == 0) {
        std::puts("online invitation tests passed");
        return EXIT_SUCCESS;
    }
    return EXIT_FAILURE;
}
