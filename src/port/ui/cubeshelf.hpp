#pragma once

#include "window.hpp"

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

struct Friend {
    std::string key;
    std::string handle;
    std::string status;   // online, ingame, offline, paused
    std::string label;    // the status in words, from CubeShelf
    std::string inviteId; // changes when a friend opens a new lobby
    bool invitesYou = false;
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
    std::map<std::string, std::string> text;

    // A string CubeShelf supplied, or the English fallback when it did not.
    std::string t(const char *key, const char *fallback) const;
};

std::optional<State> read_state() noexcept;

// The title of the tab, as CubeShelf names it, read once when the menu is built.
std::string tab_title() noexcept;

// Called every frame from ui::update(). Rate-limits itself.
void tick() noexcept;

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
    void send(const std::string &action, const std::string &key, bool closesGame);

    std::string mPassphrase;
    std::string mImportMessage;
    Rml::Element *mImportStatus = nullptr;
    std::shared_ptr<ImportJob> mImport;

    long long mRevision = -1;
    bool mFresh = false;
    std::string mPendingId;
    bool mPendingCloses = false;
    clock::time_point mPendingSince {};
    clock::time_point mLastRead {};
};

} // namespace partyboard::ui::cubeshelf
