#pragma once

#include "window.hpp"

namespace partyboard::ui {
class Pane;
}

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// The in-game half of CubeShelf's friends: the "Friends" tab of the F1 menu, and the toast that
// says a friend is inviting you.
//
// The game knows nothing about friends. CubeShelf does -- identities, presence, invitations --
// and when it launches Party Board it names a directory in CUBESHELF_INGAME_DIR. Everything
// crosses through two kinds of file there, and nothing else: no socket, no port.
//
//   state.json        written by CubeShelf every few seconds: who you are, your friends and
//                     their status, whether you can host, and every string to show, already in
//                     the language the player chose in CubeShelf.
//   requests/<id>.json  written by the game when the player asks for something:
//                     host, invite a friend, join a friend, withdraw the invitation.
//   requests/<id>.done  CubeShelf's answer, once it has acted.
//
// Launched any other way, the variable is absent and none of this appears.
namespace partyboard::ui::cubeshelf {

// True when CubeShelf launched this game and named a directory that exists.
bool available() noexcept;

// A friend's progress in the RetroAchievements set of this game, as their CubeShelf published it.
struct FriendAchievements {
    std::string user;
    long long raGameId = 0;
    int total = 0;
    int points = 0;
    int totalPoints = 0;
    std::vector<uint32_t> ids;
};

struct Friend {
    std::string key;
    std::string handle;
    std::string status;   // online, ingame, offline, paused
    std::string label;    // the status in words, from CubeShelf
    std::string inviteId; // changes when a friend opens a new lobby
    bool invitesYou = false;
    std::optional<FriendAchievements> achievements;
};

// The player's own online room, as the companion that hosts or joined it reports it.
struct Room {
    std::string state; // creating, joining, waiting, full, playing
    std::string label; // in words, from CubeShelf
    bool isHost = false;
    bool isPublic = false;
    bool canChangeVisibility = false;
    std::vector<std::string> players;
    int maxPlayers = 2;
};

// A public room of the relay's list, for this disc and this build.
struct PublicRoom {
    std::string id; // the relay room, 32 hex digits
    std::string host;
    std::string label;
    std::vector<std::string> players;
    int maxPlayers = 2;
    bool joinable = false;  // still waiting for a player
    bool modsReady = false; // every mod it plays with is installed here
};

struct State {
    bool fresh = false; // rewritten by a running CubeShelf within the last fifteen seconds
    long long revision = 0;
    std::string me;
    std::string notice;
    bool ready = false;
    bool canHost = false;
    bool hosting = false;
    std::vector<Friend> friends;
    std::optional<Room> room;
    std::vector<PublicRoom> rooms;
    bool canHostPublic = false;
    std::map<std::string, std::string> text;

    // A string CubeShelf supplied, or the English fallback when it did not.
    std::string t(const char *key, const char *fallback) const;
};

std::optional<State> read_state() noexcept;

// The title of the tab, as CubeShelf names it, read once when the menu is built.
std::string tab_title() noexcept;

// Called every frame from ui::update(). Rate-limits itself.
void tick() noexcept;

// For the achievements window: which friends have each achievement of the set being played, by
// handle. Empty when CubeShelf did not launch the game or no friend shares their progress.
std::map<uint32_t, std::vector<std::string>> friends_by_achievement() noexcept;

// A string CubeShelf supplied in the player's language, or the fallback.
std::string text(const char *key, const char *fallback) noexcept;

class FriendsWindow : public Window {
public:
    FriendsWindow();
    void update() override;

private:
    struct ImportJob;

    void build(Rml::Element *content);
    // Where the game reads friends itself: no profile yet, so offer to import one.
    void build_import(Rml::Element *content);
    void start_import(std::string source, bool fromFile);
    void set_import_message(std::string message);
    void act(const std::string &action, const std::string &key, bool closesGame);
    // Android: CubeShelf's QR code brings the account and the saves (mode 0), or sends this
    // device's saves to the PC (mode 1). See platforms/android/.../CubeShelfLink.java.
    void add_link_section(Pane &pane);
    void start_link(int mode);
    void poll_link();
    void set_link_message(std::string message);
    void send(const std::string &action, const std::string &key, bool closesGame);

    std::string mPassphrase;
    std::string mImportMessage;
    Rml::Element *mImportStatus = nullptr;
    std::shared_ptr<ImportJob> mImport;
    bool mLinkActive = false;
    std::string mLinkCode;
    std::string mLinkMessage;
    Rml::Element *mLinkStatus = nullptr;

    long long mRevision = -1;
    bool mFresh = false;
    void add_rooms(Pane &pane, const State &state);
    std::string mPendingId;
    bool mPendingCloses = false;
    clock::time_point mPendingSince {};
    clock::time_point mLastRead {};
};

} // namespace partyboard::ui::cubeshelf
