#pragma once

#include "window.hpp"

#include <string>

// The "Play Online" window on platforms that cannot start PartyBoardOnline.exe
// (Android, iOS, Linux, macOS). It carries the same lobby as the Windows
// companion's LobbyForm -- nickname, disc, player count, create, join with a
// PB4 invitation, players and status -- inside the game itself, since a phone
// cannot launch a second process next to it.
namespace partyboard::ui {

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
