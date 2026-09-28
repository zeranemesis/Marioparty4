#pragma once

#include "window.hpp"

#include <string>

// The "Play Online" window on platforms that cannot start PartyBoardOnline.exe
// (Android, iOS, Linux, macOS). It carries the same lobby as the Windows
// companion's LobbyForm -- nickname, disc, player count, create, join with a
// PB4 invitation, players and status -- inside the game itself, since a phone
// cannot launch a second process next to it.
namespace partyboard::ui {

// Android: the lobby is the app's own screen in a separate process
// (platforms/android, online/LobbyActivity.java), because the game has to be
// restarted with its --netplay arguments exactly like partyboard.exe is by the
// Windows companion. Returns false if the screen could not be opened; on
// success the caller ends the game, as the Windows "Play Online" entry does.
// With an invitation (a CubeShelf friend's), the lobby joins it by itself.
bool open_android_lobby(const std::string &invitation = {});

class OnlineWindow : public Window {
public:
    // With an invitation -- one a CubeShelf friend sent -- the window opens on joining it.
    explicit OnlineWindow(std::string invitation = {});

private:
    void build(Rml::Element *content);
    void set_status(std::string text);
    void create_lobby();
    void join_lobby();
    void paste_invitation();
    void leave_lobby();

    int mPlayers = 2;
    std::string mInvitation;
    std::string mStatus;
    Rml::Element *mStatusElem = nullptr;
};

} // namespace partyboard::ui
