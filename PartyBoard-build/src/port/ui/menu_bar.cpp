// Credits: TwilitRealm

#include "player_setup.hpp"
#include "menu_bar.hpp"

#include <RmlUi/Core.h>
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_process.h>

#include "achievements.hpp"
#include "aurora/rmlui.hpp"
#include "cubeshelf.hpp"
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
#if (defined(__APPLE__) && !TARGET_OS_IPHONE) || defined(_WIN32)
#include "port/send_logs.h"
#endif
#include "settings.hpp"
#include "ui.hpp"
#include "window.hpp"

#include <chrono>
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
    // Only when CubeShelf launched the game: it is what knows the friends. Started any other way,
    // the menu is exactly what it was.
    if (cubeshelf::available()) {
        mTabBar->add_tab(cubeshelf::tab_title(), [this] { push(std::make_unique<cubeshelf::FriendsWindow>()); });
    }
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

#if defined(__APPLE__) && !TARGET_OS_IPHONE
    // Zip the latest logs and crash reports into Downloads and open the Share
    // window, so a player can send them in two clicks.
    mTabBar->add_tab("Send Logs", [this] {
        mTabBar->set_active_tab(-1);
        PartyBoard_SendLogs();
        push_toast({
            .type = "info",
            .title = "Send Logs",
            .content = "Your logs are being saved to Downloads. A Share window opens next: pick who to send them to.",
            .duration = std::chrono::seconds(6),
        });
        hide(false);
    });
#elif defined(_WIN32)
    // Windows: zip the newest sessions' logs into Downloads (and the iCloud
    // Drive folder, if set up) and show the zip in File Explorer.
    mTabBar->add_tab("Send Logs", [this] {
        mTabBar->set_active_tab(-1);
        PartyBoard_SendLogs();
        push_toast({
            .type = "info",
            .title = "Send Logs",
            .content = "Your logs are being saved to Downloads. File Explorer opens next with the zip selected.",
            .duration = std::chrono::seconds(6),
        });
        hide(false);
    });
#endif
    mTabBar->add_tab("Achievements", [this] { push(std::make_unique<AchievementsWindow>()); });
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

    // Hide document after transition completion
    listen(mRoot, Rml::EventId::Transitionend, [this](Rml::Event &event) {
        if (event.GetTargetElement() == mRoot && !mRoot->HasAttribute("open") && Document::visible()) {
            Document::hide(mPendingClose);
        }
    });
}

void MenuBar::show()
{
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
    Document::update();
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
    if (auto *close = tabBar->QuerySelector("close")) {
        close->SetProperty(
            Rml::PropertyId::Right, Rml::Property(safeInsets.right + 8.0f * context->GetDensityIndependentPixelRatio(), Rml::Unit::PX));
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
