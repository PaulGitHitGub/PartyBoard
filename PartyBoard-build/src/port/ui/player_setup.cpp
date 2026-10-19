#include "player_setup.hpp"

#include "menu_bar.hpp"
#include "pane.hpp"
#include "ui.hpp"

#include <dolphin/pad.h>

#include <algorithm>
#include <port/netplay_runtime.h>
#include <port/test_input.h>

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_timer.h>
#include <fmt/format.h>

namespace partyboard::ui {
namespace {

    // Online, each Mac sends exactly one controller, the one on its first
    // slot, and the lobby decides which player that is in the game. So the
    // window shows one row, "Your controller", instead of Players 1-8.
    bool online()
    {
        return PartyBoard_NetplayEnabled() && PartyBoard_NetplayPlayerCount() > 1;
    }
    // Players 1-8 (controller ports 1-8); 5-8 only play in the 8-player party.
    constexpr int kSlots = 8;
    // Online: one row per seat this computer owns (several people on one
    // computer, docs/ONLINE_MULTISEAT.md); its controller 1 feeds its first seat.
    int slot_count()
    {
        return online() ? std::clamp(PartyBoard_NetplayLocalSeatCount(), 1, kSlots) : kSlots;
    }
    bool several_here()
    {
        return online() && PartyBoard_NetplayLocalSeatCount() > 1;
    }

    constexpr u64 kHoldMs = 750;   // L + R held this long claims a player
    constexpr u64 kFlashMs = 600;  // the full bar stays (green) this long after

    // L + R held: the shoulder buttons, or both triggers past halfway (the
    // GameCube controller and adapters report L/R as triggers).
    bool holding_l_and_r(SDL_Gamepad *gamepad)
    {
        if (gamepad == nullptr) {
            return false;
        }
        const bool shoulders = SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)
            && SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
        const bool triggers = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000
            && SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000;
        return shoulders || triggers;
    }

    Rml::String controller_name(u32 index)
    {
        const char *name = PADGetNameForControllerIndex(index);
        return name != nullptr ? name : fmt::format("Controller {}", index + 1);
    }

} // namespace

PlayerSetupWindow::PlayerSetupWindow()
{
    add_tab("Set Up Players", [this](Rml::Element *content) { build(content); });
}

void PlayerSetupWindow::build(Rml::Element *content)
{
    auto &leftPane = add_child<Pane>(content, Pane::Type::Controlled);
    auto &rightPane = add_child<Pane>(content, Pane::Type::Uncontrolled);
    const auto help = [](Pane &pane) {
        pane.clear();
        if (several_here()) {
            const int first = PartyBoard_NetplayLocalSeat() + 1;
            const int last = first + PartyBoard_NetplayLocalSeatCount() - 1;
            pane.add_text("Everyone on this computer: hold L + R on your controller until the bar fills.");
            pane.add_text(fmt::format("Online, this computer plays Players {} to {}. The first controller becomes Player {}, "
                                      "the next Player {}, and so on. Phones take the players left over.",
                first, last, first, first + 1));
            pane.add_text("Done goes straight back to the game.");
            return;
        }
        if (online()) {
            pane.add_text("Hold L + R on your controller until the bar fills.");
            pane.add_text("Online, this Mac sends one controller: the one shown here. "
                          "The online lobby decides which player you are in the game "
                          "(the host is Player 1, the next to join Player 2, and so on).");
            pane.add_text(fmt::format("In this game you are Player {}.", PartyBoard_NetplayLocalSeat() + 1));
            pane.add_text("Done goes straight back to the game.");
            return;
        }
        pane.add_text("Everyone: hold L + R on your controller until the bar fills. "
                      "The first controller is player 1, the next player 2, and so on.");
        pane.add_text("Phones take the players left over. Players 5-8 only play in the 8-player party.");
    };
    leftPane.add_section(online() ? "Online" : "Players");
    for (int port = 0; port < slot_count(); ++port) {
        auto &row = leftPane.add_select_button({
            .key = several_here() ? fmt::format("Player {}", PartyBoard_NetplayLocalSeat() + port + 1)
                : online()        ? Rml::String("Your controller")
                                  : fmt::format("Player {}", port + 1),
            .getValue = [this, port] { return slot_label(port); },
        });
        row.root()->SetClass("claim-slot", true);
        if (port == 4) {
            // A small gap before players 5-8 (the 8-player party).
            row.root()->SetProperty(Rml::PropertyId::MarginTop, Rml::Property(12.0f, Rml::Unit::DP));
        }
        auto bar = row.root()->GetOwnerDocument()->CreateElement("progress");
        bar->SetClass("claim-bar", true);
        bar->SetAttribute("value", 0.0f);
        mBars[port] = row.root()->AppendChild(std::move(bar));
        leftPane.register_control(row, rightPane, help);
    }
    leftPane.add_section("");
    leftPane.register_control(leftPane.add_button("Start over").on_pressed([this] {
        mClaimed.fill(-1);
        mStarted = false;
        mDoneAt = {};
    }),
        rightPane, help);
    leftPane.register_control(leftPane.add_button("Done").on_pressed([this] {
        // Straight back to the game: close this window and the menu under it,
        // so the controller drives the game again.
        // (request_close() would re-open whatever is underneath: the menu, or
        // Settings when opened from there.) Close every PartyBoard window, this
        // one included, and leave the menu bar hidden.
        for (auto &doc : get_document_stack()) {
            if (auto *window = dynamic_cast<Window *>(doc.get()); window != nullptr && !window->closed()) {
                window->hide(true);
            }
            else if (auto *menu = dynamic_cast<MenuBar *>(doc.get()); menu != nullptr) {
                menu->hide(false);
            }
        }
    }), rightPane, help);
    help(rightPane);
}

Rml::String PlayerSetupWindow::slot_label(int port) const
{
    if (mClaimed[port] >= 0) {
        return controller_name(static_cast<u32>(mClaimed[port]));
    }
    if (PartyBoard_PhoneControllerPads() >> port & 1) {
        return "Phone";
    }
    if (!mStarted) {
        // Before the first claim the old assignment still stands.
        const s32 index = PADGetIndexForPort(static_cast<u32>(port));
        if (index >= 0) {
            return controller_name(static_cast<u32>(index)) + " (current)";
        }
    }
    return "Hold L + R";
}

void PlayerSetupWindow::claim(unsigned controllerIndex)
{
    for (int port = 0; port < kSlots; ++port) {
        if (mClaimed[port] == static_cast<int>(controllerIndex)) {
            return; // already has a player this session
        }
    }
    if (!mStarted) {
        // First claim: replace the old assignment entirely.
        for (u32 port = 0; port < static_cast<u32>(kSlots); ++port) {
            PADClearPort(port);
        }
        mStarted = true;
    }
    for (int port = 0; port < slot_count(); ++port) {
        if (mClaimed[port] < 0) {
            mClaimed[port] = static_cast<int>(controllerIndex);
            mDoneAt[port] = SDL_GetTicks();
            PADSetKeyboardActive(static_cast<u32>(port), FALSE);
            PADSetPortForIndex(controllerIndex, static_cast<u32>(port));
            PADSerializeMappings();
            if (SDL_Gamepad *gamepad = PADGetSDLGamepadForIndex(controllerIndex)) {
                SDL_RumbleGamepad(gamepad, 0x6000, 0x6000, 250);
            }
            return;
        }
    }
}

void PlayerSetupWindow::update()
{
    const u64 now = SDL_GetTicks();
    const u32 count = std::min<u32>(PADCount(), static_cast<u32>(mHoldStart.size()));
    float filling = 0; // furthest hold toward the next free player
    for (u32 index = 0; index < count; ++index) {
        if (!holding_l_and_r(PADGetSDLGamepadForIndex(index))) {
            mHoldStart[index] = 0;
            mFired[index] = false;
            continue;
        }
        if (mHoldStart[index] == 0) {
            mHoldStart[index] = now;
        }
        if (mFired[index] || std::find(mClaimed.begin(), mClaimed.end(), static_cast<int>(index)) != mClaimed.end()) {
            continue; // already has a player: holding again does nothing
        }
        const u64 held = now - mHoldStart[index];
        if (held >= kHoldMs) {
            mFired[index] = true;
            claim(index);
        }
        else {
            filling = std::max(filling, static_cast<float>(held) / kHoldMs);
        }
    }
    const auto next = std::find(mClaimed.begin(), mClaimed.end(), -1);
    for (int port = 0; port < kSlots; ++port) {
        Rml::Element *bar = mBars[port];
        if (bar == nullptr) {
            continue;
        }
        const bool flashing = mDoneAt[port] != 0 && now - mDoneAt[port] < kFlashMs;
        const bool active = flashing || (next - mClaimed.begin() == port && filling > 0);
        bar->SetClass("active", active);
        bar->SetClass("done", flashing);
        if (active) {
            bar->SetAttribute("value", flashing ? 1.0f : filling);
        }
    }
    Window::update();
}

} // namespace partyboard::ui
