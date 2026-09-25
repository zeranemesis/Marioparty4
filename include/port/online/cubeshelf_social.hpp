#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// CubeShelf's friends, read by the game itself where CubeShelf cannot run (Android).
//
// CubeShelf exports its profile for a phone (ProfileTransfer.cs, "CSP1-"): the identity key,
// the pseudo and the friends. Imported here, the phone is that same person -- it opens the
// presence documents friends seal for us (SealedPresence.cs) and sees their invitations. It never
// publishes: the computer stays the only writer of our own document.
//
// Everything below is byte-compatible with CubeShelf.Core/Social; both sides change together.
// tools/tests/cubeshelf_social_test.cpp checks it against files written by the C# code.
namespace partyboard::online::cubeshelf {

using PublicKey = std::array<std::uint8_t, 65>;
using PrivateKey = std::array<std::uint8_t, 32>;

struct Friend {
    std::string publicKey; // base64, as CubeShelf stores it
    std::string name;
    std::string presenceUrl;
    std::int64_t lastSequence = 0;
    bool paused = false;
};

struct Profile {
    std::string name;
    PrivateKey privateKey {};
    PublicKey publicKey {};
    std::vector<Friend> friends;
    std::int64_t exportedAtUnix = 0;
};

enum class ImportError {
    None,
    NotAProfile,
    Unreadable,
    WrongPassphrase,
    Unsupported,
    BadKey,
};

// Opens a "CSP1-" export with the passphrase typed on the PC. Whitespace anywhere is ignored.
std::optional<Profile> import_profile(std::string_view text, std::string_view passphrase, ImportError &error);
const char *import_error_message(ImportError error) noexcept;

// The profile as kept on the device, unencrypted like CubeShelf's own identity.key: the app's
// private storage is what protects it.
std::string serialize_profile(const Profile &profile);
std::optional<Profile> deserialize_profile(std::string_view json);

enum class Status { Offline = 0, Online = 1, InGame = 2 };

struct Invite {
    std::string gameId;
    std::string gameTitle;
    std::string joinPayload;
    std::int64_t expiresUnix = 0;
    std::string forFriend; // base64 public key; empty means everyone
};

struct Snapshot {
    std::string displayName;
    std::int64_t publishedUnix = 0;
    std::int64_t sequence = 0;
    Status status = Status::Offline;
    std::string currentGameTitle;
    std::optional<Invite> invite;
};

// Opens a friend's sealed presence document, or nothing if it is not addressed to us, was
// tampered with, or is not a version this build reads.
std::optional<Snapshot> open_presence(const Profile &me, const PublicKey &author, std::string_view envelopeJson);

// PresenceSnapshot.EffectiveStatus with PresencePolicy.FreshnessWindow (15 minutes).
Status effective_status(const Snapshot &snapshot, std::int64_t nowUnix) noexcept;
// PresenceInvite.IsLive && IsFor(us) && the game matches.
bool invites(const Snapshot &snapshot, const Profile &me, std::string_view gameId, std::int64_t nowUnix);

bool decode_public_key(std::string_view base64, PublicKey &out);
std::string encode_base64(const std::uint8_t *data, std::size_t length);

// PeerName.Sanitize, and PeerName.Handle: "Zera#4821", or six digits with longTag.
std::string sanitize_name(std::string_view untrusted);
std::string handle(std::string_view name, const PublicKey &key, bool longTag = false);

// "2026-09-25T12:00:00+00:00", with optional fraction and "Z"; nullopt when unreadable.
std::optional<std::int64_t> parse_timestamp(std::string_view text);

} // namespace partyboard::online::cubeshelf
