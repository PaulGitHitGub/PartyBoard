// Credits: TwilitRealm

#include "overlay.hpp"
#include "touch_overlay.hpp"

#include "aurora/lib/logging.hpp"
#include "port/achievements.h"
#include "port/main.h"
#include "port/netplay_runtime.h"
#include "magic_enum.hpp"
#include "window.hpp"
#include "localization.hpp"

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_timer.h>
#include <algorithm>
#include <dolphin/pad.h>
#include <port/settings.h>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#include <cstdio>

namespace partyboard::ui {
namespace {
    aurora::Module Log { "partyboard::ui::overlay" };

    const Rml::String kDocumentSource = R"RML(
<rml>
<head>
    <link type="text/rcss" href="res/rml/overlay.rcss" />
</head>
<body>
    <fps id="fps" />
    <ping id="ping" />
    <paused id="paused" />
    <countdown id="countdown" />
    <menuhint id="menuhint" />
</body>
</rml>
)RML";

    constexpr auto kMenuNotificationDuration = std::chrono::milliseconds(2500);

    constexpr std::array<const char *, 4> kFpsCorners = { "tl", "tr", "bl", "br" };

    Rml::Element *create_toast(Rml::Element *parent, const Toast &toast)
    {
        auto *elem = append(parent, "toast");
        if (!toast.type.empty()) {
            elem->SetClass(toast.type, true);
        }
        {
            auto *heading = append(elem, "heading");
            if (toast.title.starts_with("<")) {
                heading->SetInnerRML(toast.title);
            }
            else {
                auto *span = append(heading, "span");
                span->SetInnerRML(escape(ui_translate(toast.title)));
            }
            if (toast.type == "achievement") {
                auto *icon = append(heading, "icon");
                icon->SetClass("trophy", true);
                // mDoAud_seStartMenu(kSoundAchievementUnlock); // TODO PC
            }
            else if (toast.type == "controller") {
                auto *icon = append(heading, "icon");
                icon->SetClass("controller", true);
            }
        }
        {
            auto *message = append(elem, "message");
            if (toast.content.starts_with("<")) {
                message->SetInnerRML(toast.content);
            }
            else {
                auto *span = append(message, "span");
                span->SetInnerRML(escape(ui_translate(toast.content)));
            }
        }
        {
            auto *progress = append(elem, "progress");
            progress->SetAttribute("value", 1.f);
        }
        return elem;
    }

    Rml::Element *create_controller_warning(Rml::Element *parent)
    {
        auto *elem = append(parent, "toast");
        elem->SetClass("controller-warning", true);

        auto *heading = append(elem, "heading");
        auto *title = append(heading, "span");
        title->SetInnerRML(ui_translate("No controller assigned"));
        auto *icon = append(heading, "icon");
        icon->SetClass("warning", true);

        auto *message = append(elem, "message");
        auto *content = append(message, "span");
        content->SetInnerRML(ui_translate("Configure controller port 1 in Settings."));

        return elem;
    }

    SDL_Gamepad *gamepad_for_port(u32 port) noexcept
    {
        const s32 index = PADGetIndexForPort(port);
        if (index < 0) {
            return nullptr;
        }
        return PADGetSDLGamepadForIndex(static_cast<u32>(index));
    }

    Rml::String back_button_name()
    {
        if (auto *gamepad = gamepad_for_port(PAD_CHAN0)) {
            switch (SDL_GetGamepadType(gamepad)) {
                case SDL_GAMEPAD_TYPE_PS3:
                    return "Select";
                case SDL_GAMEPAD_TYPE_PS4:
                    return "Share";
                case SDL_GAMEPAD_TYPE_PS5:
                    return "Create";
                case SDL_GAMEPAD_TYPE_XBOX360:
                    return "Back";
                case SDL_GAMEPAD_TYPE_XBOXONE:
                    return "View";
                case SDL_GAMEPAD_TYPE_GAMECUBE:
                    return "R + Start";
                default:
                    break;
            }
        }
        return "Back";
    }

#if defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IOS && !TARGET_OS_MACCATALYST)
    constexpr auto kMenuNotificationPrefix = "3-finger tap or";
#else
    constexpr auto kMenuNotificationPrefix = "Press F1 or";
#endif

    Rml::Element *create_menu_notification(Rml::Element *parent)
    {
        auto *elem = append(parent, "toast");
        elem->SetClass("menu-notification", true);

        auto *message = append(elem, "message");
        auto *row = append(message, "row");
        append(row, "span")->SetInnerRML(ui_translate(kMenuNotificationPrefix));
        auto *icon = append(row, "icon");
        icon->SetClass("controller", true);
        append(row, "span")->SetInnerRML(escape(back_button_name()));
        append(row, "span")->SetInnerRML(ui_translate("to open menu"));

        return elem;
    }

    void remove_element(Rml::Element *&elem) noexcept
    {
        if (elem == nullptr) {
            return;
        }
        if (auto *parent = elem->GetParentNode()) {
            parent->RemoveChild(elem);
        }
        elem = nullptr;
    }

} // namespace

// https://vplesko.com/posts/how_to_implement_an_fps_counter.html
void Overlay::advance_fps_counter(float &outFps, Uint64 perfFreq)
{
    if (perfFreq == 0) {
        outFps = 0.f;
        return;
    }

    const Uint64 curr = SDL_GetPerformanceCounter();
    if (!mFpsHavePrevCounter) {
        mFpsPrevCounter = curr;
        mFpsHavePrevCounter = true;
        outFps = 0.f;
        return;
    }

    const Uint64 processingTicks = curr - mFpsPrevCounter;
    mFpsPrevCounter = curr;

    mFpsFrameEvents.push_back({ curr, processingTicks });
    mFpsSumTicks += processingTicks;

    while (!mFpsFrameEvents.empty() && mFpsFrameEvents.front().endCounter + perfFreq < curr) {
        mFpsSumTicks -= mFpsFrameEvents.front().processingTicks;
        mFpsFrameEvents.pop_front();
    }

    const auto n = mFpsFrameEvents.size();
    if (n == 0 || mFpsSumTicks == 0) {
        outFps = 0.f;
        return;
    }

    const double avgSeconds = static_cast<double>(mFpsSumTicks) / static_cast<double>(n) / static_cast<double>(perfFreq);
    outFps = static_cast<float>(1.0 / avgSeconds);
}

Overlay::Overlay()
    : Document(kDocumentSource)
{
    mFpsCounter = mDocument->GetElementById("fps");
    mPing = mDocument->GetElementById("ping");
    mPaused = mDocument->GetElementById("paused");
    mCountdown = mDocument->GetElementById("countdown");
    mMenuHint = mDocument->GetElementById("menuhint");
    if (mMenuHint != nullptr) {
        mMenuHint->SetInnerRML(escape(ui_translate("Menu open: the game ignores your controller until you close it (B or Esc).")));
    }

    listen(mDocument, Rml::EventId::Focus, [](Rml::Event &) { Log.warn("Overlay received focus"); });
    listen(mDocument, Rml::EventId::Transitionend, [this](Rml::Event &event) {
        if (event.GetTargetElement() == mCurrentToast) {
            if (get_toasts().empty() || clock::now() >= mCurrentToastStartTime + get_toasts().front().duration) {
                mCurrentToast->SetPseudoClass("done", true);
            }
        }
        else if (mControllerWarning != nullptr && event.GetTargetElement() == mControllerWarning && !mControllerWarning->HasAttribute("open")) {
            mControllerWarning->SetPseudoClass("done", true);
        }
        else if (mMenuNotification != nullptr && event.GetTargetElement() == mMenuNotification && !mMenuNotification->HasAttribute("open")) {
            mMenuNotification->SetPseudoClass("done", true);
        }
    });
}

void Overlay::show()
{
    if (mDocument != nullptr) {
        mDocument->Show(Rml::ModalFlag::None, Rml::FocusFlag::None, Rml::ScrollFlag::None);
    }
}

void Overlay::update()
{
    Document::update();
    if (mDocument == nullptr) {
        return;
    }

    if (mFpsCounter != nullptr) {
        if (getSettings().video.enableFpsOverlay.getValue()) {
            const int idx = getSettings().video.fpsOverlayCorner.getValue();
            mFpsCounter->SetAttribute("open", "");
            mFpsCounter->SetAttribute("corner", kFpsCorners[idx]);

            const Uint64 perfFreq = SDL_GetPerformanceFrequency();
            float fps = 0.f;
            advance_fps_counter(fps, perfFreq);

            const Uint64 now = SDL_GetPerformanceCounter();
            // Limit updates to twice per second
            const bool refreshLabel
                = perfFreq == 0 || mFpsLastUpdate == 0 || static_cast<double>(now - mFpsLastUpdate) >= 0.5 * static_cast<double>(perfFreq);
            if (refreshLabel) {
                mFpsLastUpdate = now;
                mFpsCounter->SetInnerRML(escape(fmt::format("{:.0f} FPS", fps)));
            }
        }
        else {
            mFpsCounter->RemoveAttribute("open");
            mFpsFrameEvents.clear();
            mFpsSumTicks = 0;
            mFpsHavePrevCounter = false;
            mFpsLastUpdate = 0;
        }
    }

    // While a PartyBoard menu is open the game gets no controller input (on
    // purpose); say so, so a menu left open doesn't look like a dead controller.
    if (mMenuHint != nullptr) {
        const bool show = PartyBoard_IsGameLaunched && any_document_visible() && !is_prelaunch_open();
        if (show != mMenuHintShown) {
            mMenuHintShown = show;
            if (show) mMenuHint->SetAttribute("open", "");
            else mMenuHint->RemoveAttribute("open");
        }
    }

    // Online pause banner: who is holding the game, and how the local player
    // lets go of their own pause.
    if (mPaused != nullptr) {
        const unsigned seats = PartyBoard_NetplayPausedSeats();
        if (seats != mPausedSeats) {
            mPausedSeats = seats;
            if (seats == 0) {
                mPaused->RemoveAttribute("open");
                std::fprintf(stdout, "[netplay] pause banner: hidden\n");
            } else {
                // Every seat of this computer is "you" (several people on one computer).
                const unsigned mine = PartyBoard_NetplayLocalSeatMask();
                std::string who;
                for (int seat = 0; seat < 8; ++seat) { // up to 8 online seats (8-player party)
                    if (!(seats >> seat & 1)) continue;
                    if (!who.empty()) who += " and ";
                    who += fmt::format("Player {}{}", seat + 1, (mine >> seat & 1) ? " (you)" : "");
                }
                std::string rml = fmt::format("<div class=\"title\">Paused by {}</div>", escape(who));
                rml += (seats & mine)
                    ? "<div class=\"hint\">Press Start to continue.</div>"
                    : "<div class=\"hint\">The game continues when they resume.</div>";
                mPaused->SetInnerRML("<div class=\"box\">" + rml + "</div>");
                mPaused->SetAttribute("open", "");
                std::fprintf(stdout, "[netplay] pause banner: Paused by %s (%s)\n", who.c_str(),
                    (seats & mine) ? "Press Start to continue" : "The game continues when they resume");
            }
        }
    }

    // Online: say once how to pause, when pausing first becomes possible.
    if (!mPauseHintShown && PartyBoard_NetplayCanPause()) {
        mPauseHintShown = true;
        push_toast({
            .type = "info",
            .title = "Online play",
            .content = "Hold START to pause the game for everyone.",
            .duration = std::chrono::seconds(6),
        });
        std::fprintf(stdout, "[netplay] pause: hint shown (Hold START to pause the game for everyone)\n");
    }

    // Online, after the last pause is lifted inside a minigame: 3, 2, 1 before play goes on.
    if (mCountdown != nullptr) {
        const int count = PartyBoard_NetplayResumeCountdown();
        if (count != mCountdownShown) {
            mCountdownShown = count;
            if (count == 0) {
                mCountdown->RemoveAttribute("open");
            } else {
                mCountdown->SetInnerRML(fmt::format("<div class=\"digit\">{}</div>", count));
                mCountdown->SetAttribute("open", "");
            }
            std::fprintf(stdout, "[netplay] pause: resume countdown shows %d\n", count);
        }
    }

    // Online only: round-trip time to each other player, in the FPS counter's
    // corner (stacked under or over it when it is showing).
    if (mPing != nullptr) {
        // Not during minigames: their record timer and scores sit in the corners
        // (2026-10-17, the user saw 'P2 --' covering the record timer).
        const bool inMinigame = PartyBoard_NetplayInMinigame();
        if (inMinigame != mPingHiddenForMinigame) {
            mPingHiddenForMinigame = inMinigame;
            std::fprintf(stdout, "[netplay] ping display %s\n", inMinigame ? "hidden during the minigame" : "shown");
        }
        if (PartyBoard_NetplayEnabled() && PartyBoard_NetplayPlayerCount() > 1 && !inMinigame) {
            const bool fpsShown = getSettings().video.enableFpsOverlay.getValue();
            const int idx = fpsShown ? getSettings().video.fpsOverlayCorner.getValue() : 1;
            mPing->SetAttribute("open", "");
            mPing->SetAttribute("corner", kFpsCorners[idx]);
            mPing->SetClass("stacked", fpsShown);
            const Uint64 perfFreq = SDL_GetPerformanceFrequency();
            const Uint64 now = SDL_GetPerformanceCounter();
            if (mPingLastUpdate == 0 || perfFreq == 0 || now - mPingLastUpdate >= perfFreq / 2) {
                mPingLastUpdate = now;
                std::string rml;
                for (int seat = 0; seat < PartyBoard_NetplayPlayerCount(); ++seat) {
                    const int ms = PartyBoard_NetplayPeerPing(seat);
                    if (ms == -2) {
                        continue;
                    }
                    const char *tone = ms < 0 ? "lost" : ms < 50 ? "good" : ms <= 100 ? "fair" : "poor";
                    rml += fmt::format("<span class=\"{}\">P{} {}</span>", tone, seat + 1,
                        ms < 0 ? std::string("--") : fmt::format("{} ms", ms));
                }
                mPing->SetInnerRML(rml);
            }
        }
        else {
            mPing->RemoveAttribute("open");
            mPingLastUpdate = 0;
        }
    }

    u32 buttonCount;
    // On a phone the screen controller is port 1's controller: nothing to warn about.
    const bool showControllerWarning = PADGetIndexForPort(PAD_CHAN0) < 0 && PADGetKeyButtonBindings(PAD_CHAN0, &buttonCount) == nullptr
        && !TouchOverlay::stands_in_for_gamepad()
        && dynamic_cast<Window *>(top_document()) == nullptr && dynamic_cast<WindowSmall *>(top_document()) == nullptr;
    if (showControllerWarning && mControllerWarning == nullptr) {
        mControllerWarning = create_controller_warning(mDocument);
    }
    else if (showControllerWarning && mControllerWarning != nullptr) {
        mControllerWarning->SetAttribute("open", "");
        mControllerWarning->SetPseudoClass("opened", true);
        mControllerWarning->SetPseudoClass("done", false);
    }
    else if (!showControllerWarning && mControllerWarning != nullptr) {
        if (mControllerWarning->IsPseudoClassSet("done") || !mControllerWarning->IsPseudoClassSet("opened")) {
            remove_element(mControllerWarning);
        }
        else {
            mControllerWarning->RemoveAttribute("open");
        }
    }

    if (mMenuNotification != nullptr) {
        if (clock::now() >= mMenuNotificationStartTime + kMenuNotificationDuration) {
            if (mMenuNotification->IsPseudoClassSet("done") || !mMenuNotification->IsPseudoClassSet("opened")) {
                remove_element(mMenuNotification);
            }
            else {
                mMenuNotification->RemoveAttribute("open");
            }
        }
        else {
            mMenuNotification->SetAttribute("open", "");
            mMenuNotification->SetPseudoClass("opened", true);
            mMenuNotification->SetPseudoClass("done", false);
        }
    }
    if (consume_menu_notification_request()) {
        if (mMenuNotification == nullptr) {
            mMenuNotification = create_menu_notification(mDocument);
        }
        mMenuNotificationStartTime = clock::now();
    }

    auto &toasts = get_toasts();
    if (mCurrentToast == nullptr) {
        if (!toasts.empty()) {
            const auto &toast = toasts.front();
            mCurrentToast = create_toast(mDocument, toast);
            // Keep clear of the FPS counter and the online ping box when they sit in the
            // top-right corner too (2026-10-18: the online pause hint toast covered them).
            {
                const bool fpsShown = getSettings().video.enableFpsOverlay.getValue();
                const bool fpsTopRight = fpsShown && getSettings().video.fpsOverlayCorner.getValue() == 1;
                const bool pingTopRight = mPing != nullptr && mPing->HasAttribute("open")
                    && (fpsShown ? getSettings().video.fpsOverlayCorner.getValue() == 1 : true);
                const int boxes = (fpsTopRight ? 1 : 0) + (pingTopRight ? 1 : 0);
                if (boxes > 0) {
                    mCurrentToast->SetClass(boxes == 2 ? "below-two" : "below-one", true);
                }
            }
            mCurrentToastStartTime = clock::now();
        }
    }
    else if (!toasts.empty()) {
        const auto &toast = toasts.front();
        const float duration = std::chrono::duration<float>(toast.duration).count();
        const float elapsed = std::chrono::duration<float>(clock::now() - mCurrentToastStartTime).count();
        const float ratio = duration > 0.0f ? std::clamp(elapsed / duration, 0.0f, 1.0f) : 1.0f;
        const auto remaining = 1.f - ratio;
        Rml::ElementList list;
        mDocument->GetElementsByTagName(list, "progress");
        for (auto *elem : list) {
            elem->SetAttribute("value", remaining);
        }
        if (remaining == 0.f) {
            if (mCurrentToast->IsPseudoClassSet("done") ||
                // Fallback for large gaps in time where we never actually opened it
                !mCurrentToast->IsPseudoClassSet("opened")) {
                remove_element(mCurrentToast);
                toasts.pop_front();
            }
            else {
                mCurrentToast->RemoveAttribute("open");
            }
        }
        else {
            mCurrentToast->SetAttribute("open", "");
            mCurrentToast->SetPseudoClass("opened", true);
        }
    }
}

bool Overlay::handle_nav_command(Rml::Event &event, NavCommand cmd)
{
    Log.warn("Overlay received nav command: {}", magic_enum::enum_name(cmd));
    return false;
}

} // namespace partyboard::ui
