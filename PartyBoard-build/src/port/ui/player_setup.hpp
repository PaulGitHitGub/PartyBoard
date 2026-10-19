#pragma once

#include "window.hpp"

#include <SDL3/SDL_stdinc.h>

#include <array>

namespace partyboard::ui {

// Settings > Set Up Players (also on the menu bar): everyone holds L + R on
// their controller, and the first to do so becomes player 1, the next player 2,
// and so on (up to player 8: players 5-8 play in the 8-player party), like the
// Switch's controller screen. The claimed controller
// rumbles to confirm. Holding fills a bar on the player's row (about
// three quarters of a second) so a brush of the shoulders doesn't count. Phones then take the players left over (the phone bridge
// skips players with a controller). Opening and closing the window without a
// claim changes nothing; the first claim replaces the old assignment.
class PlayerSetupWindow : public Window {
public:
    PlayerSetupWindow();

    void update() override;

private:
    void build(Rml::Element *content);
    void claim(unsigned controllerIndex);
    Rml::String slot_label(int port) const;

    // Players 1-8: rows 5-8 are for the 8-player party (controller ports 5-8).
    static constexpr int kPlayers = 8;
    std::array<int, kPlayers> mClaimed {-1, -1, -1, -1, -1, -1, -1, -1}; // controller index per player this session
    bool mStarted = false;                         // a claim has replaced the old assignment
    std::array<Uint64, 16> mHoldStart {};             // when L + R went down (ms), 0 if not held
    std::array<bool, 16> mFired {};                // this hold already claimed
    std::array<Rml::Element *, kPlayers> mBars {}; // hold progress bar per player row
    std::array<Uint64, kPlayers> mDoneAt {};          // when that player was claimed (ms), for the flash
};

} // namespace partyboard::ui
