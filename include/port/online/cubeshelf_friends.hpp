#pragma once

#include "port/online/cubeshelf_social.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// The running side of CubeShelf's friends on a device without CubeShelf: keeps the imported
// profile, reads every friend's presence in the background, and says what the Friends tab shows.
// It mirrors CubeShelf's own ComposeInGameState (MainWindow.InGame.cs) so the tab reads the same
// whether CubeShelf wrote its state.json or this code composed it.
namespace partyboard::online::cubeshelf {

struct FriendView {
    std::string key; // base64 public key
    std::string handle;
    std::string status; // online, ingame, offline, paused
    std::string label;
    std::string inviteId; // changes when the friend opens a new lobby
    bool invitesYou = false;
};

struct NativeView {
    bool hasProfile = false;
    std::string me;
    std::vector<FriendView> friends;
    // Bumped whenever something shown changes, so the tab only rebuilds when it has to.
    long long revision = 0;
};

// Loads the stored profile, if any, and starts reading friends. Safe to call every frame.
void start() noexcept;
NativeView view(bool french);

// Imports a "CSP1-" export (read from a file path or an Android content:// URI, or given as
// text) and replaces any stored profile. Blocking: PBKDF2 takes about a second on a phone.
ImportError import_from_path(const std::string &path, const std::string &passphrase);
ImportError import_from_text(const std::string &text, const std::string &passphrase);
// The decrypted profile document a CubeShelf QR code delivered (no passphrase involved).
ImportError import_from_document(const std::string &json);

void forget_profile() noexcept;
void refresh_now() noexcept;

// The PB4 invitation a friend is inviting us to, while it is live.
std::optional<std::string> join_payload(const std::string &friendKey);

} // namespace partyboard::online::cubeshelf
