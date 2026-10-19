#pragma once

#include "window.hpp"

#include <array>
#include <memory>
#include <string>

namespace partyboard::online::tailnet {
class Scanner;
class Identity;
class Lobby;
}

// The "Play Online" window on platforms that cannot start PartyBoardOnline.exe
// (Android, iOS, Linux, macOS). It carries the same lobby as the Windows
// companion's LobbyForm -- nickname, disc, player count, create, join with a
// PB4 invitation, players and status -- inside the game itself, since a phone
// cannot launch a second process next to it. On macOS it hosts and joins over
// the user's Tailscale network instead (port/online/tailnet_lobby.hpp).
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
    ~OnlineWindow() override;
    void update() override;

private:
    void build(Rml::Element *content);
    void set_status(std::string text);
    void create_lobby();
    void join_lobby();
    void paste_invitation();
    void leave_lobby();

    // macOS: hosting and joining over the user's Tailscale network.
    void build_tailnet(Rml::Element *content);
    void host_tailnet();
    void join_tailnet(const std::string &ip);
    std::unique_ptr<online::tailnet::Scanner> mScanner;
    std::unique_ptr<online::tailnet::Identity> mIdentity;
    std::unique_ptr<online::tailnet::Lobby> mLobby;

    int mPlayers = 2;
    // Tailscale lobby: 4 = the normal party, 8 = the 8-player party.
    int mParty = 4;
    // People playing on this computer (several people on one computer), 1 by default.
    int mPeople = 1;
    // Test aid PARTYBOARD_TEST_ONLINE="people=4 party=8 players=8 host=1|join=<ip> focus=people|players|lobby"
    // (HD beta builds, for full-size lobby previews): preset values, host, and the row to focus.
    Component *mPeopleRow = nullptr;
    Component *mPlayersRow = nullptr;
    Component *mLobbyRow = nullptr;
    std::string mTestFocus;
    bool mTestHost = false;
    std::string mTestJoin;
    bool mTestFocused = false;
    int mTestFrames = 0;
    // Ready check: each person on this computer, by local index (controller port k).
    std::array<bool, 8> mLocalReady {};
    std::array<bool, 8> mPrevA {};
    std::array<bool, 8> mPrevB {};
    void service_ready_check();
    std::string mInvitation;
    std::string mStatus;
    Rml::Element *mStatusElem = nullptr;
};

} // namespace partyboard::ui
