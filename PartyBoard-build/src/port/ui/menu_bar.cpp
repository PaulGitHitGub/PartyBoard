// Credits: TwilitRealm

#include "player_setup.hpp"
#include "menu_bar.hpp"

#include <RmlUi/Core.h>
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_process.h>

#include "aurora/rmlui.hpp"
#include "port/main.h"
#include "port/settings.h"
#include "imgui.h"
#include "modal.hpp"
#include "online.hpp"
#include <cstdio>
#include "port/netplay_runtime.h"
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#include "settings.hpp"
#include "ui.hpp"
#include "window.hpp"

#include <chrono>
#include <fmt/format.h>
#include <cmath>

namespace partyboard::ui {
namespace {

    const Rml::String kDocumentSource = R"RML(
<rml>
<head>
    <link type="text/rcss" href="res/rml/tabbing.rcss" />
    <link type="text/rcss" href="res/rml/popup.rcss" />
</head>
<body>
    <popup id="popup" />
</body>
</rml>
)RML";

}

MenuBar::MenuBar()
    : Document(kDocumentSource)
    , mRoot(mDocument->GetElementById("popup"))
{
    mTabBar = std::make_unique<TabBar>(mRoot,
        TabBar::Props {
            .onClose =
                [this] {
                    // mDoAud_seStartMenu(kSoundMenuClose); // TODO
                    hide(false);
                },
            .autoSelect = false,
        });
    // Online only: hold the game for everyone. Choosing it again (or Start)
    // lets go of this player's pause. In every build since 2026-10-17 (tested
    // Mac-vs-PC: board and minigames, both hosts, two pausers, a long pause,
    // state identical after resume); was HD beta only before.
    if (PartyBoard_NetplayCanPause()) {
        mTabBar->add_tab("Pause / Resume (hold START)", [this] {
            mTabBar->set_active_tab(-1);
            const unsigned mine = PartyBoard_NetplayLocalSeatMask();
            PartyBoard_NetplaySetPaused((PartyBoard_NetplayPausedSeats() & mine) == 0);
            hide(false);
        });
    }
    // Online: this computer's own seat whose controller is gone can be made a computer
    // player (docs/ONLINE_DROP_VOTE.md). Shown only while that applies; the title names the seat.
    if (PartyBoard_NetplayCanPause()) {
        mDropTabIndex = mTabBar->tab_count();
        mTabBar->add_tab("Make a computer player", [this] {
            mTabBar->set_active_tab(-1);
            const bool done = PartyBoard_NetplayDropMenuChoose();
            std::fprintf(stdout, "[netplay] drop: F1 menu: 'Make Player N a computer' chosen (%s)\n", done ? "sent" : "nothing to do");
            hide(false);
        });
        mTabBar->set_tab_shown(mDropTabIndex, false);
    }
    mTabBar->add_tab("Players", [this] { push(std::make_unique<PlayerSetupWindow>()); });
    mTabBar->add_tab("Settings", [this] { push(std::make_unique<SettingsWindow>()); });

#if defined(_WIN32)
    // Windows: the Tailscale lobby, as on the Mac (the PartyBoardOnline.exe
    // companion this used to start is not built).
    mTabBar->add_tab("Play Online", [this] {
        std::fprintf(stdout, "[lobby] Play Online opened from the menu\n");
        push(std::make_unique<OnlineWindow>());
    });
#elif defined(__ANDROID__)
    // The phone's companion: the lobby screen restarts the game for the session.
    mTabBar->add_tab("Play Online", [this] {
        if (open_android_lobby()) {
            PartyBoard_IsRunning = false;
            return;
        }
        push_toast({
            .type = "error",
            .title = "Online mode",
            .content = "The online lobby could not be opened.",
            .duration = std::chrono::seconds(5),
        });
    });
#else
    // No companion process on iOS/Linux/macOS: the lobby lives in the game.
    mTabBar->add_tab("Play Online", [this] { push(std::make_unique<OnlineWindow>()); });
#endif
    // mTabBar->add_tab("Warp", [] {
    //     // TODO
    // });

    // TODO PC
    // mTabBar->add_tab("Reset", [this] {
    //     mTabBar->set_active_tab(-1);
    //     const auto dismiss = [](Modal &modal) { modal.pop(); };
    //     push(std::make_unique<Modal>(Modal::Props{
    //         .title = "Reset Game",
    //         .bodyRml = "Unsaved progress will be lost.<br/>"
    //                    "<span class=\"tip\">Tip: You can also reset by holding Start+X+B</span>",
    //         .actions =
    //             {
    //                 ModalAction{
    //                     .label = "Cancel",
    //                     .onPressed =
    //                         [this, dismiss](Modal& modal) {
    //                             // mDoAud_seStartMenu(kSoundWindowClose); // TODO PC
    //                             dismiss(modal);
    //                         },
    //                 },
    //                 // ModalAction{
    //                 //     .label = "Reset",
    //                 //     .onPressed =
    //                 //         [this, dismiss](Modal& modal) {
    //                 //             // mDoAud_seStartMenu(kSoundClick); // TODO PC
    //                 //             // if (fpcM_SearchByName(fpcNm_LOGO_SCENE_e)) {
    //                 //             //     dismiss(modal);
    //                 //             //     return;
    //                 //             // }
    //                 //             dismiss(modal);
    //                 //             hide(false);
    //                 //         },
    //                 // },
    //             },
    //         .onDismiss = dismiss,
    //         .icon = "question-mark",
    //     }));
    // });
    mTabBar->add_tab("Quit", [this] {
        mTabBar->set_active_tab(-1);
        const auto dismiss = [](Modal &modal) { modal.pop(); };
        push(std::make_unique<Modal>(Modal::Props{
            .title = "Quit Party Board",
            .bodyRml = "Unsaved progress will be lost.",
            .actions =
                {
                    ModalAction{
                        .label = "Cancel",
                        .onPressed =
                            [dismiss](Modal& modal) {
                                // mDoAud_seStartMenu(kSoundWindowClose); // TODO PC
                                dismiss(modal);
                            },
                    },
                    ModalAction{
                        .label = "Quit",
                        .onPressed =
                            [dismiss](Modal& modal) {
                                // mDoAud_seStartMenu(kSoundClick); // TODO PC
                                dismiss(modal);
                                PartyBoard_IsRunning = false;
                            },
                    },
                },
            .onDismiss = dismiss,
            .icon = "question-mark",
        }));
    });

    // The band (2026-10-19): online it holds up to eight tabs, more than fit at 4:3 or at the
    // 640-wide minimum window. The tabs scroll in a strip of their own, with an edge fade and a
    // chevron on the side that has more; Quit and the close button sit pinned at the right end,
    // outside the strip, so Quit is always on screen and the button never covers a tab.
    {
        Rml::Element *tabBar = mTabBar->root();
        Rml::Element *strip = mRoot->InsertBefore(mDocument->CreateElement("tab-strip"), tabBar);
        strip->AppendChild(mRoot->RemoveChild(tabBar));
        // Each fade: a solid cap of the band's blue carrying the chevron, then a short ramp.
        mFadeLeft = append(strip, "tab-fade");
        mFadeLeft->SetClass("left", true);
        append(mFadeLeft, "fade-cap");
        append(mFadeLeft, "fade-ramp");
        mFadeRight = append(strip, "tab-fade");
        mFadeRight->SetClass("right", true);
        append(mFadeRight, "fade-cap");
        append(mFadeRight, "fade-ramp");
        mPinned = append(mRoot, "tab-pinned");
        Rml::ElementList tabs;
        tabBar->GetElementsByTagName(tabs, "tab");
        if (!tabs.empty()) {
            mPinned->AppendChild(tabBar->RemoveChild(tabs.back())); // Quit, added last
        }
        if (Rml::Element *close = tabBar->QuerySelector("close")) {
            mPinned->AppendChild(tabBar->RemoveChild(close));
        }
        // Left/Right on the pinned Quit still walk the tabs (the strip's own listener can't see it).
        listen(mPinned, Rml::EventId::Keydown, [this](Rml::Event &event) {
            const auto cmd = map_nav_event(event);
            if (cmd != NavCommand::None && mTabBar->handle_nav_command(event, cmd)) {
                event.StopPropagation();
            }
        });
    }

    // Hide document after transition completion
    listen(mRoot, Rml::EventId::Transitionend, [this](Rml::Event &event) {
        if (event.GetTargetElement() == mRoot && !mRoot->HasAttribute("open") && Document::visible()) {
            Document::hide(mPendingClose);
        }
    });
}

// The drop tab's title and whether it shows, from the current online state.
void MenuBar::refresh_drop_tab()
{
    if (mDropTabIndex < 0) {
        return;
    }
    // One vote per person, by holding START (user, 2026-10-19): the menu only offers
    // this computer's own people to make their seat with a lost controller a computer player.
    PartyBoardNetplayControllerDrop pad {};
    Rml::String title;
    if (PartyBoard_NetplayControllerDropGet(&pad) && pad.mine) {
        title = fmt::format("Make Player {} a computer", pad.seat + 1);
    }
    mTabBar->set_tab_shown(mDropTabIndex, !title.empty());
    if (!title.empty()) {
        mTabBar->set_tab_title(mDropTabIndex, title);
    }
}

void MenuBar::show()
{
    refresh_drop_tab();
    Document::show();
    mRoot->SetAttribute("open", "");
    mTabBar->set_active_tab(-1);
    if (!mTabBar->focus_tab(mFocusedTabIndex)) {
        mTabBar->focus();
    }
}

void MenuBar::hide(bool close)
{
    mFocusedTabIndex = mTabBar->focused_tab_index();
    mRoot->RemoveAttribute("open");
    if (close) {
        mPendingClose = true;
    }
}

void MenuBar::update()
{
    update_safe_area();
    if (visible()) {
        refresh_drop_tab();
    }
    // After the drop tab may have changed: re-measure the scrolling tab strip.
    update_overflow();
    Document::update();
}

// Shows the fade and chevron on each side of the tab strip that has tabs scrolled out of view.
void MenuBar::update_overflow() noexcept
{
    if (mFadeLeft == nullptr || mFadeRight == nullptr || !visible()) {
        return;
    }
    Rml::Element *tabBar = mTabBar->root();
    const float hidden = tabBar->GetScrollWidth() - tabBar->GetClientWidth();
    const float left = tabBar->GetScrollLeft();
    const bool moreLeft = hidden > 0.5f && left > 0.5f;
    const bool moreRight = hidden > 0.5f && left < hidden - 0.5f;
    const int state = (moreLeft ? 1 : 0) | (moreRight ? 2 : 0);
    if (state == mOverflowState) {
        return;
    }
    mOverflowState = state;
    if (moreLeft) mFadeLeft->SetAttribute("open", ""); else mFadeLeft->RemoveAttribute("open");
    if (moreRight) mFadeRight->SetAttribute("open", ""); else mFadeRight->RemoveAttribute("open");
    std::fprintf(stdout, "[menu] tab strip: %s (%.0f of %.0f px shown, scrolled %.0f)\n",
        state == 0 ? "every tab fits" : state == 1 ? "more tabs to the left" : state == 2 ? "more tabs to the right" : "more tabs on both sides",
        tabBar->GetClientWidth(), tabBar->GetScrollWidth(), left);
}

void MenuBar::update_safe_area() noexcept
{
    if (mDocument == nullptr || mTabBar == nullptr) {
        return;
    }

    // Avoid ImGui menu bar if shown
    if (const auto *viewport = ImGui::GetMainViewport(); viewport != nullptr && mTopMargin != viewport->WorkPos.y) {
        mTopMargin = viewport->WorkPos.y;
        mRoot->SetProperty(Rml::PropertyId::MarginTop, Rml::Property(mTopMargin, Rml::Unit::DP));
    }

    Rml::Context *context = mDocument->GetContext();
    Insets safeInsets = safe_area_insets(context);
    safeInsets = {
        0.0f,
        std::round(safeInsets.right),
        0.0f,
        std::round(safeInsets.left),
    };
    if (safeInsets == mTabBarPadding) {
        return;
    }

    mTabBarPadding = safeInsets;
    auto *tabBar = mTabBar->root();
    tabBar->SetProperty(Rml::PropertyId::PaddingRight, Rml::Property(safeInsets.right, Rml::Unit::PX));
    tabBar->SetProperty(Rml::PropertyId::PaddingLeft, Rml::Property(safeInsets.left, Rml::Unit::PX));
    // The right inset belongs to the pinned Quit and close button at the band's right end.
    if (mPinned != nullptr) {
        tabBar->SetProperty(Rml::PropertyId::PaddingRight, Rml::Property(0.0f, Rml::Unit::PX));
        mPinned->SetProperty(Rml::PropertyId::PaddingRight, Rml::Property(safeInsets.right, Rml::Unit::PX));
    }
}

bool MenuBar::visible() const
{
    return mRoot->HasAttribute("open");
}

bool MenuBar::handle_nav_command(Rml::Event &event, NavCommand cmd)
{
    if (!getSettings().backend.wasPresetChosen) {
        return true;
    }
    if (cmd == NavCommand::Cancel && visible()) {
        // mDoAud_seStartMenu(kSoundMenuClose); // TODO PC
        hide(false);
        return true;
    }
    return Document::handle_nav_command(event, cmd);
}

bool MenuBar::focus()
{
    return mTabBar->focus();
}

} // namespace partyboard::ui
