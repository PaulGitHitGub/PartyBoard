#include "online.hpp"

#include "../file_select.hpp"
#include "localization.hpp"
#include "pane.hpp"
#include "port/config.hpp"
#include "port/online/invitation.hpp"
#include "prelaunch.hpp"
#include "string_button.hpp"
#include "component.hpp"
#include "port/online/tailnet_lobby.hpp"
#include "ui.hpp"

#include <SDL3/SDL_timer.h>
#include <cctype>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_gamepad.h>
#include <dolphin/pad.h>
#include "port/test_input.h"
#include "port/pad_ports.h"

#include <SDL3/SDL_clipboard.h>
#include <SDL3/SDL_stdinc.h>
#include <fmt/format.h>

#include <algorithm>
#include <cstring>
#include <ctime>
#include <functional>
#include <sstream>
#include <cstdlib>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
// Hosting over Tailscale: macOS and Windows (an iPhone cannot run the second copy).
#if (defined(__APPLE__) && !TARGET_OS_IOS) || defined(_WIN32)
#define PARTYBOARD_TAILNET_LOBBY 1
#endif

#ifdef __ANDROID__
#include <SDL3/SDL_system.h>
#include <jni.h>
#endif

namespace partyboard::ui {
namespace {

    // Main actions keep a button look in the settings list (res/rml/window.rcss).
    template <typename T>
    T &primary(T &button)
    {
        button.root()->SetClass("primary", true);
        return button;
    }


    constexpr int kNicknameMaxLength = 24;
    // Same bound as the companion's invitation box.
    constexpr int kInvitationMaxLength = 220;

    // Mirrors PlayerInfo.CleanName: 1 to 24 characters, no control character.
    bool valid_nickname(const std::string &name)
    {
        if (name.empty()) {
            return false;
        }
        std::size_t characters = 0;
        for (const unsigned char c : name) {
            if (c < 0x20 || c == 0x7f) {
                return false;
            }
            if ((c & 0xC0) != 0x80) {
                ++characters;
            }
        }
        return characters <= kNicknameMaxLength;
    }

    std::string trimmed(std::string text)
    {
        const auto isSpace = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
        while (!text.empty() && isSpace(text.back())) {
            text.pop_back();
        }
        std::size_t start = 0;
        while (start < text.size() && isSpace(text[start])) {
            ++start;
        }
        return text.substr(start);
    }

    std::string nickname()
    {
        const auto &value = getSettings().online.nickname.getValue();
        return value.empty() ? ui_translate("Player") : value;
    }

    std::string disc_label()
    {
        const auto &state = prelaunch_state();
        if (state.configuredDiscPath.empty()) {
            return "(none)";
        }
        auto display = display_name_for_path(state.configuredDiscPath);
        return display.empty() ? state.configuredDiscPath : display;
    }

    bool disc_ready()
    {
        const auto &state = prelaunch_state();
        return !state.configuredDiscPath.empty() && state.configuredDiscCanLaunch;
    }

    namespace tn = online::tailnet;

    // Finds the element (or ancestor) a click landed on that carries `attribute`.
    Rml::Element *clicked_with(Rml::Event &event, const char *attribute)
    {
        for (auto *element = event.GetTargetElement(); element != nullptr; element = element->GetParentNode()) {
            if (element->HasAttribute(attribute)) {
                return element;
            }
        }
        return nullptr;
    }

    // Names from Tailscale and other players can carry typographic quotes (macOS names a
    // computer "Alex\u2019s MacBook Pro"); the UI font has no glyph for them and drew a wide
    // gap (2026-10-18). Show the plain ASCII quotes instead.
    std::string plain_quotes(std::string text)
    {
        static const std::pair<const char *, const char *> kMap[] = {
            {"\xE2\x80\x98", "'"}, {"\xE2\x80\x99", "'"}, {"\xE2\x80\x9C", "\""}, {"\xE2\x80\x9D", "\""},
            {"\xC2\xB4", "'"}, {"\xE2\x80\xB2", "'"},
        };
        for (const auto &[from, to] : kMap) {
            for (std::size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + 1)) {
                text.replace(at, std::strlen(from), to);
            }
        }
        return text;
    }

    // A right-pane panel redrawn from RML whose buttons must also work with a controller or
    // the keyboard, not only the mouse (2026-10-18: a player with only an Xbox controller
    // could not join from "Find games"). Focusing the panel focuses its first actionable
    // button (one carrying `attribute`); Up/Down move between them; Confirm acts like a
    // click; the focused row survives a redraw.
    class NavPanel : public Component {
    public:
        // act(button, click): click is true for a mouse click, false for Confirm (A / Return).
        NavPanel(Rml::Element *parent, const char *attribute, std::function<void(Rml::Element *, bool)> act)
            : Component(append(parent, "div"))
            , mAttribute(attribute)
        {
            listen(mRoot, Rml::EventId::Click, [this, act](Rml::Event &event) {
                if (auto *button = clicked_with(event, mAttribute)) {
                    act(button, true);
                }
            });
            listen(mRoot, Rml::EventId::Keydown, [this, act = std::move(act)](Rml::Event &event) {
                const auto cmd = map_nav_event(event);
                const auto buttons = rows();
                const int current = index_of(buttons, event.GetTargetElement());
                if (current < 0) {
                    return;
                }
                if (cmd == NavCommand::Confirm) {
                    act(buttons[current], false);
                    event.StopPropagation();
                } else if (cmd == NavCommand::Down && current + 1 < static_cast<int>(buttons.size())) {
                    buttons[current + 1]->Focus(true);
                    event.StopPropagation();
                } else if (cmd == NavCommand::Up && current > 0) {
                    buttons[current - 1]->Focus(true);
                    event.StopPropagation();
                }
            });
        }

        bool focus() override
        {
            for (auto *button : rows()) {
                if (button->Focus(true)) {
                    return true;
                }
            }
            return false;
        }

    protected:
        // Replaces the panel's content, keeping the focused row (by position) focused.
        void set_rml(const std::string &rml)
        {
            if (rml == mShown) {
                return;
            }
            const auto before = rows();
            auto *focused = mRoot->GetOwnerDocument() ? mRoot->GetOwnerDocument()->GetFocusLeafNode() : nullptr;
            const int current = index_of(before, focused);
            mRoot->SetInnerRML(rml);
            mShown = rml;
            if (current >= 0) {
                const auto after = rows();
                if (!after.empty()) {
                    after[std::min<std::size_t>(current, after.size() - 1)]->Focus(true);
                }
            }
        }

    private:
        std::vector<Rml::Element *> rows() const
        {
            std::vector<Rml::Element *> found;
            Rml::ElementList buttons;
            mRoot->GetElementsByTagName(buttons, "button");
            for (auto *button : buttons) {
                if (button->HasAttribute(mAttribute)) {
                    found.push_back(button);
                }
            }
            return found;
        }

        static int index_of(const std::vector<Rml::Element *> &buttons, Rml::Element *element)
        {
            for (; element != nullptr; element = element->GetParentNode()) {
                const auto it = std::find(buttons.begin(), buttons.end(), element);
                if (it != buttons.end()) {
                    return static_cast<int>(it - buttons.begin());
                }
            }
            return -1;
        }

        const char *mAttribute;
        std::string mShown;
    };

    // Right pane for "Find games": the lobbies hosted in the Tailscale network,
    // each a button that joins it. Redrawn when the list changes; clicks are
    // handled once on the panel, so redrawing never leaves a stale listener.
    class GamesPanel : public NavPanel {
    public:
        GamesPanel(Rml::Element *parent, tn::Scanner &scanner, std::function<void(const std::string &)> onJoin)
            : NavPanel(parent, "data-ip", [onJoin = std::move(onJoin)](Rml::Element *button, bool) {
                onJoin(button->GetAttribute<Rml::String>("data-ip", ""));
            })
            , mScanner(scanner)
        {
            mRoot->SetClass("online-panel", true);
            refresh();
        }

        void update() override
        {
            if (SDL_GetTicks() >= mNext) {
                refresh();
            }
            Component::update();
        }

    private:
        void refresh()
        {
            mNext = SDL_GetTicks() + 400;
            const auto offers = mScanner.offers();
            std::string rml;
            if (mScanner.busy()) {
                rml = "<div>" + escape(ui_translate("Looking for games on your Tailscale network...")) + "</div>";
            } else if (!mScanner.error().empty()) {
                rml = "<div class=\"online-problem\">" + escape(ui_translate(mScanner.error())) + "</div>";
            } else if (offers.empty()) {
                rml = "<div>" + escape(ui_translate("No games found yet. Once a friend presses Host a game, press Find games again.")) + "</div>";
            } else {
                for (const auto &offer : offers) {
                    const bool full = offer.joined >= offer.players;
                    const std::string label = fmt::format("{}'s game: {} of {} players{}{}{}", plain_quotes(offer.name), offer.joined,
                        offer.players, offer.party == 8 ? ", 8-player party" : "",
                        offer.continuing ? fmt::format(", continues a saved party (round {} of {})", offer.round, offer.rounds) : std::string(),
                        full ? " (full)" : "");
                    rml += "<button class=\"online-game\"" + (full ? std::string() : " data-ip=\"" + escape(offer.ip) + "\"")
                        + ">" + escape(label) + "</button>";
                    rml += "<div class=\"online-hint\">" + escape(plain_quotes(offer.device)) + "</div>";
                }
                rml += "<div class=\"online-hint\">" + escape(ui_translate("Choose a game to join it (click it, or move right and press A).")) + "</div>";
            }
            set_rml(rml);
        }

        tn::Scanner &mScanner;
        uint64_t mNext = 0;
    };

    // What feeds a controller port, for people: "controller 'Xbox Wireless Controller'"
    // -> "Xbox Wireless Controller", "keyboard" -> "Keyboard", "phone" -> "Phone".
    std::string device_label(std::string device)
    {
        for (std::size_t at = device.find("controller '"); at != std::string::npos; at = device.find("controller '")) {
            const auto end = device.find('\'', at + 12);
            device = device.substr(0, at) + device.substr(at + 12, end == std::string::npos ? std::string::npos : end - at - 12)
                + (end == std::string::npos ? std::string() : device.substr(end + 1));
        }
        if (!device.empty()) {
            device[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(device[0])));
        }
        return plain_quotes(device);
    }

    // The lobby panel on screen (the ready check reads each person's A/B only while it is).
    class LobbyPanel;
    LobbyPanel *gLobbyPanelShown = nullptr;

    // Right pane for the lobby: who is in, empty seats, the ready check, and Start/Leave.
    class LobbyPanel : public NavPanel {
    public:
        LobbyPanel(Rml::Element *parent, tn::Lobby &lobby, std::function<void()> onStart, std::function<void()> onLeave,
            std::function<void(int)> onToggleReady, std::function<bool()> discFound, std::function<void()> onNewParty,
            std::function<void()> onComputers)
            : NavPanel(parent, "data-action",
                  [onStart = std::move(onStart), onLeave = std::move(onLeave), onToggleReady,
                      onNewParty = std::move(onNewParty), onComputers = std::move(onComputers)](Rml::Element *button, bool click) {
                      const auto action = button->GetAttribute<Rml::String>("data-action", "");
                      if (action == "start") {
                          onStart();
                      } else if (action == "newparty") {
                          onNewParty();
                      } else if (action == "computers") {
                          onComputers();
                      } else if (action == "leave") {
                          onLeave();
                      } else if (action.rfind("ready-", 0) == 0 && click) {
                          // A mouse click toggles that person's ready. A and B on a controller are
                          // read per controller instead (each person's own controller).
                          onToggleReady(std::atoi(action.c_str() + 6));
                      }
                  })
            , mLobby(lobby)
            , mDiscFound(std::move(discFound))
        {
            mRoot->SetClass("online-panel", true);
            // B on a ready row un-readies the person whose controller pressed it (read per
            // controller); it must not also leave the panel.
            listen(mRoot, Rml::EventId::Keydown, [](Rml::Event &event) {
                if (map_nav_event(event) != NavCommand::Cancel) {
                    return;
                }
                for (auto *element = event.GetTargetElement(); element != nullptr; element = element->GetParentNode()) {
                    if (element->GetAttribute<Rml::String>("data-action", "").rfind("ready-", 0) == 0) {
                        event.StopPropagation();
                        return;
                    }
                }
            });
            gLobbyPanelShown = this;
            refresh();
        }
        ~LobbyPanel() override
        {
            if (gLobbyPanelShown == this) {
                gLobbyPanelShown = nullptr;
            }
        }

        void update() override
        {
            if (SDL_GetTicks() >= mNext) {
                refresh();
            }
            Component::update();
        }

    private:
        static std::string check_line(bool ok, const std::string &text)
        {
            return std::string("<div class=\"online-check") + (ok ? " ok" : "") + "\"><span class=\"online-mark\">" + (ok ? "OK" : "No")
                + "</span><span>" + escape(text) + "</span></div>";
        }

        void refresh()
        {
            mNext = SDL_GetTicks() + 250;
            using State = tn::Lobby::State;
            const auto state = mLobby.state();
            std::string rml;
            if (state == State::Idle) {
                rml = "<div>" + escape(ui_translate("You're not in a lobby. Host a game, or find one a friend is hosting.")) + "</div>";
            } else {
                if (!mLobby.message().empty()) {
                    rml += "<div class=\"" + std::string(state == State::Failed ? "online-problem" : "online-status") + "\">"
                        + escape(plain_quotes(ui_translate(mLobby.message()))) + "</div>";
                }
                // Continuing a saved party (docs/ONLINE_CONTINUE.md): what it is, on both sides.
                if (mLobby.continuing()) {
                    const auto &saved = mLobby.saved();
                    rml += "<div class=\"online-hint\">"
                        + escape(fmt::format("Saved party: {}, round {} of {}.", tn::board_name(saved.board), saved.round, saved.rounds))
                        + "</div>";
                } else if (mLobby.hostContinues()) {
                    rml += "<div class=\"online-hint\">"
                        + escape(fmt::format("The host continues a saved party: round {} of {}.", mLobby.continueRound(), mLobby.continueRounds()))
                        + "</div>";
                }
                // One row per computer, in seat order; a computer with several
                // people shows its block ("Player 1-4") and its kind (Mac or PC).
                for (int seat = 0; seat < mLobby.players();) {
                    const auto &members = mLobby.members();
                    const auto it = std::find_if(members.begin(), members.end(), [seat](const tn::Member &m) { return m.seat == seat; });
                    // Continuing: an empty block is the saved computer's, named.
                    const tn::Member *expected = it == members.end() ? mLobby.savedComputerAt(seat) : nullptr;
                    const int seats = it != members.end() ? std::max(1, it->seats) : expected != nullptr ? std::max(1, expected->seats) : 1;
                    rml += "<div class=\"online-seat\"><span class=\"online-seat-number\">"
                        + (seats > 1 ? fmt::format("Player {}-{}", seat + 1, seat + seats) : fmt::format("Player {}", seat + 1))
                        + "</span>";
                    if (it != members.end()) {
                        const std::string where = seats > 1 && !it->computer.empty() ? ", " + it->computer : std::string();
                        rml += "<span>" + escape(plain_quotes(it->name) + where)
                            + (seat == 0 ? escape(ui_translate(" (host)")) : std::string()) + "</span>";
                    } else if (expected != nullptr) {
                        rml += "<span class=\"online-hint\">"
                            + escape("Waiting for " + plain_quotes(expected->name)
                                + (expected->computer.empty() ? std::string() : ", " + expected->computer))
                            + "</span>";
                    } else {
                        rml += "<span class=\"online-hint\">" + escape(ui_translate("Waiting for a player")) + "</span>";
                    }
                    rml += "</div>";
                    seat += seats;
                }
                if (mLobby.party() == 8 && mLobby.players() < 8) {
                    rml += "<div class=\"online-hint\">"
                        + escape(mLobby.players() == 7 ? std::string("Player 8: computer player")
                                                       : fmt::format("Players {} to 8: computer players", mLobby.players() + 1))
                        + "</div>";
                }
                if (state == State::Waiting) {
                    // The count and Start first (always on screen), then this computer's checklist and people.
                    rml += ready_summary();
                    if (mLobby.isHost()) {
                        rml += mLobby.canStart()
                            ? "<button data-action=\"start\">" + escape(ui_translate("Start game")) + "</button>"
                            : "<button class=\"online-start-off\" disabled>" + escape(ui_translate("Start game")) + "</button>";
                        if (mLobby.continuing()) {
                            // A missing computer's people can continue as computer players (needs one other computer back).
                            const std::string missing = mLobby.missingComputers();
                            if (!missing.empty() && mLobby.anyComputerBack()) {
                                rml += "<button data-action=\"computers\">"
                                    + escape(fmt::format("Use computer players for {}", plain_quotes(missing))) + "</button>";
                            }
                            rml += "<button data-action=\"newparty\">" + escape(ui_translate("Start a new party instead")) + "</button>";
                            rml += "<div class=\"online-hint\">"
                                + escape(ui_translate("A new party uses the settings on the left and replaces the saved one at its first save."))
                                + "</div>";
                        }
                    }
                    rml += ready_check();
                }
                if (state != State::Launched) {
                    rml += "<button data-action=\"leave\">" + escape(ui_translate(state == State::Failed ? "Close" : "Leave lobby"))
                        + "</button>";
                }
            }
            set_rml(rml);
        }

        // How many people are ready, everywhere, and (host) who the game waits for.
        std::string ready_summary()
        {
            std::string rml;
            const int ready = mLobby.readyCount();
            const int players = mLobby.players();
            rml += "<div class=\"online-ready-count\">" + escape(fmt::format("{} of {} players ready", ready, players)) + "</div>";
            rml += "<progress class=\"online-ready-bar\" value=\"" + std::to_string(ready) + "\" max=\"" + std::to_string(std::max(1, players))
                + "\"></progress>";
            if (mLobby.isHost()) {
                // Short: who is missing and how many controllers/people aren't ready yet (the log keeps the full list).
                const std::string waiting = mLobby.waitingShort();
                rml += "<div class=\"online-hint\">"
                    + escape(waiting.empty() ? std::string("Everyone is ready.") : "Waiting for: " + plain_quotes(waiting)) + "</div>";
            }
            return rml;
        }

        // The ready check (online lobby only): this computer's checklist, one row per person
        // (this computer's are buttons: A on your own controller, or a click), and the count.
        std::string ready_check()
        {
            const char *computer = tn::kComputer;
            std::string rml = "<div class=\"online-ready-box\"><div class=\"online-ready-title\">" + escape(ui_translate("Ready check")) + "</div>";
            rml += check_line(mDiscFound(), "Disc image found");
            if (mLobby.isHost()) {
                rml += check_line(true, "Same game version (checked as each computer joins)");
                for (const auto &m : mLobby.members()) {
                    if (m.seat == 0) {
                        continue;
                    }
                    const int ms = mLobby.pingMs(m.seat);
                    const std::string who = plain_quotes(m.name) + (m.computer.empty() ? std::string() : ", " + m.computer);
                    rml += check_line(true, ms >= 0 ? fmt::format("Connected: {} ({} ms)", who, ms) : fmt::format("Connected: {}", who));
                }
            } else {
                rml += check_line(true, "Same game version as the host");
                const int ms = mLobby.pingMs(0);
                rml += check_line(true, ms >= 0 ? fmt::format("Connected to the host ({} ms)", ms) : std::string("Connected to the host"));
            }
            const int first = mLobby.localSeat();
            const int people = mLobby.localPeople();
            rml += "<div class=\"online-hint\">"
                + escape(people > 1 ? fmt::format("Everyone on this {}: press A on your own controller when you're ready (B to cancel).", computer)
                                    : std::string("Press A on your controller when you're ready (B to cancel)."))
                + "</div>";
            for (const auto &m : mLobby.members()) {
                if (m.seat + m.seats <= first || m.seat >= first + people) {
                    // Another computer: one line, "PC: 3 of 4 ready" (the host's Waiting line names who).
                    int ready = 0;
                    for (int seat = m.seat; seat < m.seat + m.seats && seat < tn::kMaxSeats; ++seat) {
                        ready += mLobby.seatReady(seat).ready ? 1 : 0;
                    }
                    const std::string who = m.computer.empty() ? plain_quotes(m.name) : plain_quotes(m.name) + ", " + m.computer;
                    const std::string head = m.seats > 1 ? fmt::format("Player {}-{}", m.seat + 1, m.seat + m.seats) : fmt::format("Player {}", m.seat + 1);
                    rml += "<div class=\"online-seat online-ready-remote\"><span class=\"online-seat-number\">" + escape(head)
                        + "</span><span class=\"online-ready-text\">" + escape(who) + "</span><span class=\"online-ready-state"
                        + (ready == m.seats ? " ok" : "") + "\">"
                        + escape(ready == m.seats ? std::string("Ready!") : fmt::format("{} of {} ready", ready, m.seats)) + "</span></div>";
                    continue;
                }
                for (int seat = m.seat; seat < m.seat + m.seats && seat < tn::kMaxSeats; ++seat) {
                    const auto &r = mLobby.seatReady(seat);
                    const bool mine = seat >= first && seat < first + people;
                    std::string text;
                    if (!r.controller) {
                        text = mine ? "no controller (plug one in or pair a phone)" : "no controller";
                    } else {
                        text = r.device.empty() ? std::string("controller") : device_label(r.device);
                    }
                    const std::string where = mine ? std::string() : (m.computer.empty() ? plain_quotes(m.name) : m.computer) + ": ";
                    const std::string head = fmt::format("Player {}", seat + 1);
                    const std::string body = "<span class=\"online-seat-number\">" + escape(head) + "</span><span class=\"online-ready-text\">"
                        + escape(where + text) + "</span><span class=\"online-ready-state" + (r.ready ? " ok" : "") + "\">"
                        + escape(r.ready ? "Ready!" : mine && r.controller ? "Press A" : "") + "</span>";
                    if (mine) {
                        rml += "<button class=\"online-ready\" data-action=\"ready-" + std::to_string(seat - first) + "\">" + body + "</button>";
                    } else {
                        rml += "<div class=\"online-seat online-ready-remote\">" + body + "</div>";
                    }
                }
            }
            rml += "</div>";
            return rml;
        }

        tn::Lobby &mLobby;
        std::function<bool()> mDiscFound;
        uint64_t mNext = 0;
    };

    std::string configured_disc_path()
    {
        const auto &state = prelaunch_state();
        return !state.configuredDiscPath.empty() ? state.configuredDiscPath : getSettings().backend.isoPath.getValue();
    }

} // namespace

bool open_android_lobby([[maybe_unused]] const std::string &invitation)
{
#ifdef __ANDROID__
    auto *env = static_cast<JNIEnv *>(SDL_GetAndroidJNIEnv());
    auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
    if (env == nullptr || activity == nullptr) {
        return false;
    }
    bool opened = false;
    jclass type = env->GetObjectClass(activity);
    jmethodID open = type == nullptr ? nullptr
        : env->GetMethodID(type, "openOnlineLobby", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Z");
    if (open != nullptr) {
        const auto &state = prelaunch_state();
        const std::string disc = state.configuredDiscCanLaunch ? state.configuredDiscPath
                                                              : getSettings().backend.isoPath.getValue();
        const bool french = getSettings().game.language.getValue() == GameLanguage::French;
        jstring name = env->NewStringUTF(getSettings().online.nickname.getValue().c_str());
        jstring path = env->NewStringUTF(disc.c_str());
        jstring language = env->NewStringUTF(french ? "fr" : "en");
        jstring code = env->NewStringUTF(invitation.c_str());
        opened = env->CallBooleanMethod(activity, open, name, path, language, code) == JNI_TRUE;
        env->DeleteLocalRef(code);
        env->DeleteLocalRef(name);
        env->DeleteLocalRef(path);
        env->DeleteLocalRef(language);
    }
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        opened = false;
    }
    if (type != nullptr) {
        env->DeleteLocalRef(type);
    }
    env->DeleteLocalRef(activity);
    return opened;
#else
    return false;
#endif
}

OnlineWindow::OnlineWindow(std::string invitation)
{
    ensure_initialized();
    mStatus = disc_ready() ? "Disc ready. Create a lobby or paste your friend's invitation." : "Choose your disc to get started.";
#ifdef PARTYBOARD_TAILNET_LOBBY
    mScanner = std::make_unique<online::tailnet::Scanner>();
    mIdentity = std::make_unique<online::tailnet::Identity>();
    mLobby = std::make_unique<online::tailnet::Lobby>();
    mScanner->start();
    mIdentity->start(configured_disc_path());
    mSaved = std::make_unique<online::tailnet::SavedParty>(online::tailnet::load_saved_party());
    mStatus = "Host a game, or find one a friend is hosting on your Tailscale network.";
#if defined(PARTYBOARD_HD_BETA)
    if (const char *test = std::getenv("PARTYBOARD_TEST_ONLINE"); test != nullptr) {
        std::string text(test);
        std::replace(text.begin(), text.end(), ',', ' ');
        std::istringstream words(text);
        for (std::string word; words >> word;) {
            const auto eq = word.find('=');
            if (eq == std::string::npos) continue;
            const std::string key = word.substr(0, eq), value = word.substr(eq + 1);
            if (key == "people") mPeople = std::clamp(std::atoi(value.c_str()), 1, 7);
            else if (key == "party") mParty = value == "8" ? 8 : 4;
            else if (key == "players") mPlayers = std::clamp(std::atoi(value.c_str()), 2, 8);
            else if (key == "host") mTestHost = value == "1";
            else if (key == "focus") mTestFocus = value;
            else if (key == "join") mTestJoin = value;
            else if (key == "continue") mTestContinue = value == "1";
        }
        std::fprintf(stdout, "[lobby] TEST preset: %d people here, %d-player party, %d players%s, focus %s\n", mPeople, mParty,
            mPlayers, mTestHost ? ", hosting" : "", mTestFocus.c_str());
    }
#endif
    add_tab("Play Online", [this](Rml::Element *content) { build_tailnet(content); });
    return;
#endif
    add_tab("Online Lobby", [this](Rml::Element *content) { build(content); });
    if (!invitation.empty()) {
        mInvitation = trimmed(std::move(invitation));
        join_lobby();
    }
}

OnlineWindow::~OnlineWindow() = default;

// Ready check, per person on this computer: is a controller on their port (gamepad,
// keyboard, phone), and did that controller press A (ready) or B (not ready). Read straight
// from each controller, since the menu keeps game input blocked, and only while the lobby
// panel is on screen (so an A meant for a menu button elsewhere does not count).
namespace {
    bool port_button(unsigned port, PADButton button)
    {
        if (PartyBoard_TestInputPadButtons(static_cast<int>(port)) & button) {
            return true;   // a phone or a test driver
        }
        u32 count = 0;
        if (const s32 index = PADGetIndexForPort(port); index >= 0) {
            if (SDL_Gamepad *gamepad = PADGetSDLGamepadForIndex(static_cast<u32>(index))) {
                if (const PADButtonMapping *map = PADGetButtonMappings(port, &count)) {
                    for (u32 i = 0; i < count; ++i) {
                        if (map[i].padButton == button && map[i].nativeButton != PAD_NATIVE_BUTTON_INVALID
                            && SDL_GetGamepadButton(gamepad, static_cast<SDL_GamepadButton>(map[i].nativeButton))) {
                            return true;
                        }
                    }
                }
            }
        }
        if (const PADKeyButtonBinding *keys = PADGetKeyButtonBindings(port, &count)) {
            int numKeys = 0;
            const bool *state = SDL_GetKeyboardState(&numKeys);
            for (u32 i = 0; i < count; ++i) {
                if (keys[i].padButton == button && keys[i].scancode > PAD_KEY_INVALID && keys[i].scancode < numKeys && state[keys[i].scancode]) {
                    return true;
                }
            }
        }
        return false;
    }
} // namespace

void OnlineWindow::service_ready_check()
{
    using State = online::tailnet::Lobby::State;
    if (!mLobby || mLobby->state() != State::Waiting) {
        mLocalReady.fill(false);
        return;
    }
    const bool shown = gLobbyPanelShown != nullptr;
    for (int k = 0; k < mLobby->localPeople() && k < static_cast<int>(mLocalReady.size()); ++k) {
        const auto port = static_cast<unsigned>(k);
        char device[160];
        const bool controller = PartyBoard_PadPortDescribe(port, device, sizeof(device)) != 0;
        const bool a = controller && port_button(port, PAD_BUTTON_A);
        const bool b = controller && port_button(port, PAD_BUTTON_B);
        auto &ready = mLocalReady[static_cast<std::size_t>(k)];
        if (shown && a && !mPrevA[static_cast<std::size_t>(k)] && !ready) {
            ready = true;
            std::fprintf(stdout, "[lobby] ready: player %d pressed A on this computer's controller port %d (%s)\n",
                mLobby->localSeat() + k + 1, k + 1, device);
        }
        if (shown && b && !mPrevB[static_cast<std::size_t>(k)] && ready) {
            ready = false;
            std::fprintf(stdout, "[lobby] ready: player %d pressed B: not ready\n", mLobby->localSeat() + k + 1);
        }
        if (!controller && ready) {
            ready = false;
            std::fprintf(stdout, "[lobby] ready: player %d's controller is gone (port %d): not ready\n", mLobby->localSeat() + k + 1, k + 1);
        }
        mPrevA[static_cast<std::size_t>(k)] = a;
        mPrevB[static_cast<std::size_t>(k)] = b;
        mLobby->setLocalSeat(k, controller, ready, controller ? device : "");
    }
}

void OnlineWindow::update()
{
    Window::update();
    service_ready_check();
    if (!mTestFocus.empty() || mTestHost || !mTestJoin.empty()) {
        ++mTestFrames;
        if (!mTestJoin.empty() && mTestFrames > 60 && mLobby && mLobby->state() == online::tailnet::Lobby::State::Idle
            && mIdentity->ready()) {
            const std::string ip = mTestJoin;
            mTestJoin.clear();
            join_tailnet(ip);
        }
        if (mTestHost && mTestFrames > 60 && mLobby && mLobby->state() == online::tailnet::Lobby::State::Idle && !mScanner->busy()
            && mIdentity->ready()) {
            mTestHost = false;
            if (mTestContinue) {
                continue_tailnet();
            } else {
                host_tailnet();
            }
        }
        if (!mTestFocused && mTestFrames > 90 && !mTestHost && mTestJoin.empty()) {
            Component *row = mTestFocus == "people" ? mPeopleRow : mTestFocus == "players" ? mPlayersRow
                : mTestFocus == "lobby" ? mLobbyRow : mTestFocus == "continue" ? mContinueRow : nullptr;
            if (row != nullptr) {
                row->focus();
            }
            mTestFocused = true;
        }
    }
    if (mLobby) {
        mLobby->poll();
        if (!mLobby->message().empty() && mLobby->message() != mStatus) {
            set_status(mLobby->message());
        }
    }
}

void OnlineWindow::build_tailnet(Rml::Element *content)
{
    auto &leftPane = add_child<Pane>(content, Pane::Type::Controlled);
    auto &rightPane = add_child<Pane>(content, Pane::Type::Uncontrolled);
    const auto lobbyPanel = [this](Pane &pane) {
        pane.add_child<LobbyPanel>(
            *mLobby, [this] { mLobby->start(); },
            [this] {
                mLobby->leave();
                set_status("Host a game, or find one a friend is hosting on your Tailscale network.");
            },
            [this](int k) {
                if (k >= 0 && k < static_cast<int>(mLocalReady.size())) {
                    mLocalReady[static_cast<std::size_t>(k)] = !mLocalReady[static_cast<std::size_t>(k)];
                    std::fprintf(stdout, "[lobby] ready: player %d %s by a click\n", mLobby->localSeat() + k + 1,
                        mLocalReady[static_cast<std::size_t>(k)] ? "marked ready" : "marked not ready");
                }
            },
            [this] { return mIdentity && mIdentity->ready(); }, [this] { new_party_instead(); },
            [this] {
                if (mLobby->continueWithComputers()) {
                    set_status(mLobby->message());
                }
            });
    };

    leftPane.add_section("You");
    leftPane.register_control(leftPane.add_child<StringButton>(StringButton::Props {
                                  .key = "Nickname",
                                  .getValue = [] { return nickname(); },
                                  .setValue =
                                      [this](Rml::String value) {
                                          value = trimmed(std::move(value));
                                          if (!valid_nickname(value)) {
                                              set_status("Choose a nickname of 1 to 24 characters, on a single line.");
                                              return;
                                          }
                                          getSettings().online.nickname.setValue(value);
                                          config::Save();
                                      },
                                  .maxLength = kNicknameMaxLength,
                              }),
        rightPane, [](Pane &pane) { pane.add_text("The name the other players see in the lobby."); });
    mPeopleRow = &leftPane.register_control(leftPane.add_select_button({
                                  .key = "On this computer",
                                  .getValue = [this] { return mPeople == 1 ? Rml::String("1 person") : fmt::format("{} people", mPeople); },
                              }),
        rightPane, [this](Pane &pane) {
            const int most = (mParty == 8 ? online::tailnet::kMaxSeats : online::tailnet::kNormalPartySeats) - 1;
            for (int count = 1; count <= most; ++count) {
                pane.add_button({
                                    .text = count == 1 ? "1 person" : fmt::format("{} people", count),
                                    .isSelected = [this, count] { return mPeople == count; },
                                })
                    .on_pressed([this, count] {
                        mPeople = count;
                        const int top = mParty == 8 ? online::tailnet::kMaxSeats : online::tailnet::kNormalPartySeats;
                        mPlayers = std::min(top, std::max(mPlayers, mPeople + 1));
                    });
            }
            pane.add_text(fmt::format("How many people play on this {}, each with their own controller. "
                                      "Give everyone a controller first in Players > Set Up Players: "
                                      "controller 1 is the first of them, controller 2 the next, and so on.",
                online::tailnet::kComputer));
            // (No count here: this text is drawn once and would go stale when the count changes.)
            pane.add_text(fmt::format("When you host, they are the first players (Player 1, 2, ...). When you join, the host "
                                      "gives this {} the next free players.",
                online::tailnet::kComputer));
        });
    leftPane.register_control(leftPane
                                  .add_select_button({
                                      .key = "Disc Image",
                                      .getValue = [] { return disc_label(); },
                                  })
                                  .on_pressed([] { open_iso_picker(); }),
        rightPane, [](Pane &pane) {
            pane.add_text("Everyone needs the same Mario Party 4 file (USA). The lobby compares the whole file and turns away "
                          "anyone whose copy differs, since a different file would put the game out of sync.");
        });

    leftPane.add_section("Host a game");
    leftPane.register_control(leftPane.add_select_button({
                                  .key = "Party",
                                  .getValue = [this] { return Rml::String(mParty == 8 ? "8 players" : "4 players"); },
                              }),
        rightPane, [this](Pane &pane) {
            for (const int party : {4, 8}) {
                pane.add_button({
                                    .text = party == 8 ? "8 players" : "4 players",
                                    .isSelected = [this, party] { return mParty == party; },
                                })
                    .on_pressed([this, party] {
                        mParty = party;
                        mPlayers = std::min(mPlayers, party == 8 ? online::tailnet::kMaxSeats : online::tailnet::kNormalPartySeats);
                        mPeople = std::min(mPeople, mPlayers - 1);
                    });
            }
            pane.add_text("4 players: the normal party. 8 players: the 8-Player Party (Battle Royal or Team Match); "
                          "seats nobody takes are computer players.");
        });
    mPlayersRow = &leftPane.register_control(leftPane.add_select_button({
                                  .key = "Players",
                                  .getValue = [this] { return std::to_string(mPlayers); },
                              }),
        rightPane, [this](Pane &pane) {
            const int most = mParty == 8 ? online::tailnet::kMaxSeats : online::tailnet::kNormalPartySeats;
            for (int count = std::max(2, mPeople + 1); count <= most; ++count) {
                pane.add_button({
                                    .text = std::to_string(count),
                                    .isSelected = [this, count] { return mPlayers == count; },
                                })
                    .on_pressed([this, count] { mPlayers = count; });
            }
            pane.add_text(mParty == 8 ? "How many people, you included. The other seats of the 8-Player Party are computer players."
                                      : "How many players, you included. Computer players are chosen in the game as usual.");
            if (mPeople > 1) {
                pane.add_text(fmt::format("Everyone on every computer counts: {} on this {}, the rest on the others.", mPeople,
                    online::tailnet::kComputer));
            }
        });
    leftPane.register_control(primary(leftPane.add_button("Host a game")).on_pressed([this] { host_tailnet(); }), rightPane, lobbyPanel);
    // Continuing a saved online party (docs/ONLINE_CONTINUE.md): only when this
    // computer hosted one that isn't finished.
    if (mSaved && mSaved->valid) {
        mContinueRow = &leftPane.register_control(primary(leftPane.add_button("Continue party")).on_pressed([this] { continue_tailnet(); }),
            rightPane, [this](Pane &pane) {
                const auto &saved = *mSaved;
                // "2026-10-19 21:40" -> "Oct 19, 21:40"
                std::string when = saved.savedAt;
                if (when.size() == 16) {
                    static const char *months[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
                    const int month = std::atoi(when.substr(5, 2).c_str());
                    if (month >= 1 && month <= 12) {
                        when = fmt::format("{} {}, {}", months[month - 1], std::atoi(when.substr(8, 2).c_str()), when.substr(11));
                    }
                }
                pane.add_rml("<div class=\"online-panel\"><div class=\"online-status\">"
                    + escape(fmt::format("{} - Round {} of {}", online::tailnet::board_name(saved.board), saved.round, saved.rounds))
                    + "</div><div class=\"online-hint\">"
                    + escape(fmt::format("{}{}, saved {}", saved.party == 8 ? "8-Player Party, " : "",
                        saved.mode.empty() ? std::string("Party") : saved.mode, when))
                    + "</div></div>");
                std::string rows;
                for (const auto &c : saved.computers) {
                    std::string who = plain_quotes(c.name) + (c.computer.empty() ? std::string() : ", " + c.computer);
                    if (c.seat == saved.thisSeat) {
                        who += fmt::format(" (this {})", online::tailnet::kComputer);
                    }
                    std::string characters;
                    for (int s = c.seat; s < c.seat + c.seats && s < static_cast<int>(saved.characters.size()); ++s) {
                        const auto &name = saved.characters[static_cast<std::size_t>(s)];
                        if (!name.empty()) {
                            characters += (characters.empty() ? "" : ", ") + name;
                        }
                    }
                    rows += "<div class=\"online-seat\"><span class=\"online-seat-number\">"
                        + escape(c.seats > 1 ? fmt::format("Player {}-{}", c.seat + 1, c.seat + c.seats) : fmt::format("Player {}", c.seat + 1))
                        + "</span><span>" + escape(who) + (characters.empty() ? std::string() : "<br/><span class=\"online-hint\">" + escape(characters) + "</span>")
                        + "</span></div>";
                }
                if (saved.party == 8 && saved.players < 8) {
                    rows += "<div class=\"online-hint\">"
                        + escape(saved.players == 7 ? std::string("Player 8: computer player")
                                                    : fmt::format("Players {} to 8: computer players", saved.players + 1))
                        + "</div>";
                }
                pane.add_rml("<div class=\"online-panel\">" + rows + "</div>");
                pane.add_text("Everyone rejoins from the same computer, with the same number of people.");
            });
    }

    leftPane.add_section("Join a game");
    leftPane.register_control(primary(leftPane.add_button("Find games")).on_pressed([this] { mScanner->start(); }), rightPane,
        [this](Pane &pane) { pane.add_child<GamesPanel>(*mScanner, [this](const std::string &ip) { join_tailnet(ip); }); });

    leftPane.add_section("Lobby");
    mLobbyRow = &leftPane.register_control(leftPane.add_select_button({
                                  .key = "Lobby",
                                  .getValue =
                                      [this] {
                                          using State = online::tailnet::Lobby::State;
                                          switch (mLobby->state()) {
                                          case State::Idle:
                                              return Rml::String("Not in one");
                                          case State::Failed:
                                              return Rml::String("Problem");
                                          case State::Starting:
                                          case State::Launched:
                                              return Rml::String("Starting");
                                          default:
                                              return fmt::format("{} of {}", mLobby->taken(), mLobby->players());
                                          }
                                      },
                              }),
        rightPane, lobbyPanel);
    mStatusElem = leftPane.add_text(mStatus);

    leftPane.finalize();
}

void OnlineWindow::host_tailnet()
{
    if (mLobby->state() != online::tailnet::Lobby::State::Idle && mLobby->state() != online::tailnet::Lobby::State::Failed) {
        set_status("You're already in a lobby. Leave it first.");
        return;
    }
    mIdentity->start(configured_disc_path());
    if (mIdentity->failed()) {
        set_status(mIdentity->error());
        return;
    }
    if (!mIdentity->ready()) {
        set_status("Checking your game files. Try again in a few seconds.");
        return;
    }
    if (mScanner->busy()) {
        set_status("Still connecting to Tailscale. Try again in a moment.");
        return;
    }
    if (mScanner->selfIp().empty()) {
        set_status(mScanner->error().empty() ? std::string("This ") + online::tailnet::kComputer + " isn't connected to Tailscale."
                                             : mScanner->error());
        mScanner->start();
        return;
    }
    if (mLobby->host(mScanner->selfIp(), mPlayers, nickname(), *mIdentity, mParty, mPeople)) {
        set_status(mParty == 8 ? fmt::format("Hosting an 8-Player Party for {} people. Friends can now press Find games.", mPlayers)
                               : fmt::format("Hosting a {}-player game. Friends can now press Find games.", mPlayers));
    }
}

void OnlineWindow::continue_tailnet()
{
    if (!mSaved || !mSaved->valid) {
        set_status("There is no saved online party to continue.");
        return;
    }
    if (mLobby->state() != online::tailnet::Lobby::State::Idle && mLobby->state() != online::tailnet::Lobby::State::Failed) {
        set_status("You're already in a lobby. Leave it first.");
        return;
    }
    mIdentity->start(configured_disc_path());
    if (mIdentity->failed()) {
        set_status(mIdentity->error());
        return;
    }
    if (!mIdentity->ready()) {
        set_status("Checking your game files. Try again in a few seconds.");
        return;
    }
    if (mScanner->busy()) {
        set_status("Still connecting to Tailscale. Try again in a moment.");
        return;
    }
    if (mScanner->selfIp().empty()) {
        set_status(mScanner->error().empty() ? std::string("This ") + online::tailnet::kComputer + " isn't connected to Tailscale."
                                             : mScanner->error());
        mScanner->start();
        return;
    }
    if (mLobby->hostContinue(mScanner->selfIp(), nickname(), *mIdentity, *mSaved)) {
        set_status("Continuing the saved party. The same computers can now press Find games and join.");
        // The lobby (who is back, the ready check, Start) is the Lobby row's panel.
        if (mLobbyRow != nullptr) {
            mLobbyRow->focus();
        }
    }
}

void OnlineWindow::new_party_instead()
{
    mLobby->startNewPartyInstead();
    host_tailnet();
}

void OnlineWindow::join_tailnet(const std::string &ip)
{
    if (mLobby->state() != online::tailnet::Lobby::State::Idle && mLobby->state() != online::tailnet::Lobby::State::Failed) {
        set_status("You're already in a lobby. Leave it first.");
        return;
    }
    mIdentity->start(configured_disc_path());
    if (mIdentity->failed()) {
        set_status(mIdentity->error());
        return;
    }
    if (!mIdentity->ready()) {
        set_status("Checking your game files. Try again in a few seconds.");
        return;
    }
    mLobby->join(ip, nickname(), *mIdentity, mPeople);
}

void OnlineWindow::build(Rml::Element *content)
{
    auto &leftPane = add_child<Pane>(content, Pane::Type::Controlled);
    auto &rightPane = add_child<Pane>(content, Pane::Type::Uncontrolled);

    leftPane.add_section("Your Party Board lobby");
    leftPane.register_control(leftPane.add_child<StringButton>(StringButton::Props {
                                  .key = "Nickname",
                                  .getValue = [] { return nickname(); },
                                  .setValue =
                                      [this](Rml::String value) {
                                          value = trimmed(std::move(value));
                                          if (!valid_nickname(value)) {
                                              set_status("Choose a nickname of 1 to 24 characters, on a single line.");
                                              return;
                                          }
                                          getSettings().online.nickname.setValue(value);
                                          config::Save();
                                      },
                                  .maxLength = kNicknameMaxLength,
                              }),
        rightPane, [](Pane &pane) { pane.add_text("The name the other players see in the lobby."); });

    leftPane.register_control(leftPane
                                  .add_select_button({
                                      .key = "Disc Image",
                                      .getValue = [] { return disc_label(); },
                                  })
                                  .on_pressed([] { open_iso_picker(); }),
        rightPane, [](Pane &pane) {
            pane.add_text("Every player needs the same disc image: Mario Party 4 USA. The whole file is checked before joining.");
        });

    leftPane.register_control(leftPane.add_select_button({
                                  .key = "Players",
                                  .getValue = [this] { return std::to_string(mPlayers); },
                              }),
        rightPane, [this](Pane &pane) {
            for (int count = 2; count <= online::kMaxSeats; ++count) {
                pane.add_button({
                                    .text = std::to_string(count),
                                    .isSelected = [this, count] { return mPlayers == count; },
                                })
                    .on_pressed([this, count] { mPlayers = count; });
            }
            pane.add_text("How many players the lobby waits for, you included. Only the host chooses.");
        });

    leftPane.register_control(leftPane.add_button("Create a lobby").on_pressed([this] { create_lobby(); }), rightPane,
        [](Pane &pane) { pane.add_text("Create the lobby, then send the invitation to your friends. Only you can start the game."); });

    leftPane.add_section("Join a lobby");
    leftPane.register_control(leftPane.add_child<StringButton>(StringButton::Props {
                                  .key = "Invitation",
                                  .getValue = [this] { return mInvitation; },
                                  .setValue = [this](Rml::String value) { mInvitation = trimmed(std::move(value)); },
                                  .maxLength = kInvitationMaxLength,
                              }),
        rightPane, [](Pane &pane) { pane.add_text("The invitation starts with PB4. The host copies it from their lobby, on a PC or a phone."); });
    leftPane.register_control(leftPane.add_button("Paste invitation").on_pressed([this] { paste_invitation(); }), rightPane,
        [](Pane &pane) { pane.add_text("Paste the invitation your friend sent you."); });
    leftPane.register_control(primary(leftPane.add_button("Join")).on_pressed([this] { join_lobby(); }), rightPane,
        [](Pane &pane) { pane.add_text("Join your friend's lobby. The host starts the game for everyone."); });

    leftPane.add_section("Lobby");
    leftPane.register_control(primary(leftPane.add_button("Leave lobby")).on_pressed([this] { leave_lobby(); }), rightPane,
        [](Pane &pane) { pane.add_text("Leave the lobby and clear the invitation."); });
    mStatusElem = leftPane.add_text(mStatus);

    leftPane.finalize();
}

void OnlineWindow::set_status(std::string text)
{
    mStatus = std::move(text);
    if (mStatusElem != nullptr) {
        mStatusElem->SetInnerRML(escape(ui_translate(mStatus)));
    }
}

void OnlineWindow::create_lobby()
{
    if (!disc_ready()) {
        set_status("Choose your disc to get started.");
        return;
    }
    // The lobby itself (TLS control channel, router opening, relay) is being
    // ported from tools/online; until it lands this window says so rather than
    // pretending a lobby exists.
    set_status("Hosting from this device is not available yet: the online connection is still being ported. Create the lobby on a PC for now.");
}

void OnlineWindow::join_lobby()
{
    if (mInvitation.empty()) {
        set_status("Paste your friend's invitation first.");
        return;
    }
    const auto decoded = online::decode_invitation(mInvitation, static_cast<std::uint32_t>(std::time(nullptr)));
    if (!decoded.invitation) {
        set_status(online::invitation_error_message(decoded.error));
        return;
    }
    if (!disc_ready()) {
        set_status("Choose your disc to get started.");
        return;
    }
    const auto &invite = *decoded.invitation;
    mStatus = fmt::format(fmt::runtime(ui_translate("Valid invitation for a lobby of {} players.")), invite.maxPlayers) + " "
        + ui_translate("Joining from this device is not available yet: the online connection is still being ported.");
    if (mStatusElem != nullptr) {
        mStatusElem->SetInnerRML(escape(mStatus));
    }
}

void OnlineWindow::paste_invitation()
{
    if (!SDL_HasClipboardText()) {
        set_status("The clipboard is empty.");
        return;
    }
    char *text = SDL_GetClipboardText();
    if (text == nullptr) {
        set_status("The clipboard is empty.");
        return;
    }
    std::string value = trimmed(text);
    SDL_free(text);
    if (value.size() > static_cast<std::size_t>(kInvitationMaxLength)) {
        value.resize(kInvitationMaxLength);
    }
    mInvitation = std::move(value);
    set_status(mInvitation.empty() ? "The clipboard is empty." : "Invitation pasted. Press Join.");
}

void OnlineWindow::leave_lobby()
{
    mInvitation.clear();
    set_status(disc_ready() ? "Disc ready. Create a lobby or paste your friend's invitation." : "Choose your disc to get started.");
}

} // namespace partyboard::ui
