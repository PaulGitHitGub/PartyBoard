#include "phone_controller.hpp"

#include "localization.hpp"
#include "port/test_input.h"
#include "ui.hpp"

#include <aurora/rmlui.hpp>
#include <SDL3/SDL_timer.h>
#include <fmt/format.h>
#include <qrcodegen.hpp>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "port/main.h"

#if defined(__APPLE__) || defined(__linux__)
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#elif defined(_WIN32)
// partyboard.exe starts the phone controller on Windows (src/port/phone_bridge_win.cpp).
#include "port/lan_address_win.hpp"
#endif

namespace partyboard::ui {
namespace {

constexpr std::string_view kScheme = "phone-qr";
// Pixels per QR module, and the light border scanners need around the code.
constexpr int kModulePixels = 10;
constexpr int kQuietModules = 2;
// The window's own blue and text cream: about 8:1 contrast, which phone
// cameras read as reliably as black on white.
constexpr std::byte kDark[3] = {std::byte {37}, std::byte {59}, std::byte {156}};
constexpr std::byte kLight[3] = {std::byte {224}, std::byte {219}, std::byte {200}};

struct QrImage {
    uint32_t size = 0;
    std::vector<std::byte> rgba;
};

std::mutex gQrMutex;
std::unordered_map<std::string, QrImage> gQrImages;
int gQrSerial = 0;

std::optional<aurora::rmlui::RuntimeTexture> provide(std::string_view source)
{
    std::lock_guard lock(gQrMutex);
    const auto it = gQrImages.find(std::string(source));
    if (it == gQrImages.end()) {
        return std::nullopt;
    }
    return aurora::rmlui::RuntimeTexture {
        .width = it->second.size,
        .height = it->second.size,
        .rgba8 = std::span<const std::byte>(it->second.rgba),
        .premultipliedAlpha = false,
    };
}

// Renders url as a QR code texture and returns its RmlUi source name. Each
// link gets its own name, since RmlUi keeps a texture once it has loaded one.
std::string make_qr_texture(const std::string &url)
{
    static std::once_flag registered;
    std::call_once(registered, [] { aurora::rmlui::register_texture_provider(std::string(kScheme), provide); });

    const auto qr = qrcodegen::QrCode::encodeText(url.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
    const int modules = qr.getSize() + kQuietModules * 2;
    QrImage image;
    image.size = static_cast<uint32_t>(modules * kModulePixels);
    image.rgba.resize(static_cast<size_t>(image.size) * image.size * 4);
    for (uint32_t py = 0; py < image.size; ++py) {
        for (uint32_t px = 0; px < image.size; ++px) {
            const int mx = static_cast<int>(px) / kModulePixels - kQuietModules;
            const int my = static_cast<int>(py) / kModulePixels - kQuietModules;
            const bool dark = qr.getModule(mx, my);   // false outside the code
            std::byte *p = &image.rgba[(static_cast<size_t>(py) * image.size + px) * 4];
            std::memcpy(p, dark ? kDark : kLight, 3);
            p[3] = std::byte {255};
        }
    }
    std::lock_guard lock(gQrMutex);
    std::string source = fmt::format("{}://{}", kScheme, ++gQrSerial);
    gQrImages.clear();
    gQrImages.emplace(source, std::move(image));
    return source;
}

const char *env(const char *name)
{
    const char *value = std::getenv(name);
    return value != nullptr && value[0] != '\0' ? value : nullptr;
}

// This Mac's IPv4 address on its Wi-Fi or Ethernet network: en0 first (the
// built-in Wi-Fi on a Mac), then any other en* interface that is up. Skips
// loopback, link-local (169.254.x) and VPN/virtual interfaces.
std::string local_ipv4()
{
#if defined(__APPLE__) || defined(__linux__)
    ifaddrs *list = nullptr;
    if (getifaddrs(&list) != 0) {
        return {};
    }
    std::string best;
    int bestRank = 0;
    for (ifaddrs *it = list; it != nullptr; it = it->ifa_next) {
        if (it->ifa_addr == nullptr || it->ifa_addr->sa_family != AF_INET || (it->ifa_flags & IFF_UP) == 0 ||
            (it->ifa_flags & IFF_LOOPBACK) != 0) {
            continue;
        }
        const auto *addr = reinterpret_cast<const sockaddr_in *>(it->ifa_addr);
        const uint32_t host = ntohl(addr->sin_addr.s_addr);
        if ((host >> 16) == 0xA9FE) {   // 169.254.0.0/16
            continue;
        }
        const std::string_view name(it->ifa_name);
        const int rank = name == "en0" ? 3 : (name.starts_with("en") || name.starts_with("wl") || name.starts_with("eth")) ? 2 : 0;
        if (rank > bestRank) {
            char text[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &addr->sin_addr, text, sizeof(text));
            best = text;
            bestRank = rank;
        }
    }
    freeifaddrs(list);
    return best;
#elif defined(_WIN32)
    // Best first: Wi-Fi, then Ethernet, on a network with a gateway.
    const auto addresses = partyboard::win_lan_addresses();
    return addresses.empty() ? std::string() : addresses.front().ip;
#else
    return {};
#endif
}

struct Snapshot {
    int state = -2;           // PartyBoard_PhoneControllerState()
    unsigned pads = 0;        // players with a phone, bit 0 = player 1
    bool configured = false;  // started through the phone-controller launcher
    bool enabled = false;     // switched on in this panel (takes effect at the next launch)
    bool needsPython = false; // switched on, but this Mac lacks Apple's command line tools
#if defined(_WIN32)
    // Switched on but not started on Windows: "python" (needsPython: not
    // installed), "files" (PhoneController folder missing), "code" (couldn't
    // save the access code).
    std::string problem;
#endif
    std::string url;          // empty when this Mac has no network address
    std::string bookmarkUrl;  // by Mac name, works on any Wi-Fi
    // The bridge connects within seconds of the game opening its input
    // socket; still nothing after this long means it failed to start.
    bool helperMissing = false;
};

constexpr uint64_t kHelperStartupMs = 30000;

Snapshot read_snapshot()
{
    Snapshot s;
    s.state = PartyBoard_PhoneControllerState();
    s.pads = PartyBoard_PhoneControllerPads();
    const char *code = env("PARTYBOARD_PHONE_CODE");
    const char *port = env("PARTYBOARD_PHONE_PORT");
    s.configured = code != nullptr && port != nullptr && s.state != -2;
    std::error_code error;
    s.enabled = std::filesystem::exists(PartyBoard_ConfigPath / "phone-controller-enabled", error);
    const char *problem = env("PARTYBOARD_PHONE_PROBLEM");
    s.needsPython = problem != nullptr && std::strcmp(problem, "python") == 0;
#if defined(_WIN32)
    s.problem = problem != nullptr ? problem : "";
#endif
    if (!s.configured) {
        return s;
    }
    s.helperMissing = s.state == -1 && SDL_GetTicks() > kHelperStartupMs;
    if (const std::string ip = local_ipv4(); !ip.empty()) {
        s.url = fmt::format("http://{}:{}/?k={}", ip, port, code);
    }
    if (const char *host = env("PARTYBOARD_PHONE_HOST")) {
        s.bookmarkUrl = fmt::format("http://{}.local:{}/?k={}", host, port, code);
    }
    return s;
}

// The left pane asks for the status every frame; reading the network
// interfaces that often is wasteful, so keep each reading for half a second.
Snapshot snapshot()
{
    static Snapshot cached;
    static uint64_t expires = 0;
    if (const uint64_t now = SDL_GetTicks(); now >= expires) {
        cached = read_snapshot();
        expires = now + 500;
    }
    return cached;
}

// "Phone connected as player 2", "Phones connected as players 1, 2 and 4".
Rml::String connected_players_text(unsigned pads)
{
    std::vector<std::string> players;
    for (int pad = 0; pad < 4; ++pad) {
        if (pads & (1u << pad)) {
            players.push_back(std::to_string(pad + 1));
        }
    }
    if (players.empty()) {
        return "Phone connected";
    }
    if (players.size() == 1) {
        return "Phone connected as player " + players[0];
    }
    std::string list;
    for (size_t i = 0; i < players.size(); ++i) {
        list += players[i];
        if (i + 2 < players.size()) {
            list += ", ";
        } else if (i + 2 == players.size()) {
            list += " and ";
        }
    }
    return "Phones connected as players " + list;
}

void set_text(Rml::Element *element, const Rml::String &text)
{
    element->SetInnerRML(escape(ui_translate(text)));
}

} // namespace

Rml::String phone_controller_status_label()
{
    const Snapshot s = snapshot();
#if defined(_WIN32)
    if (!s.configured && s.enabled && s.needsPython) {
        return "Needs Python";
    }
    if (!s.configured && s.enabled && !s.problem.empty()) {
        return "Didn't start";
    }
#endif
    if (!s.configured) {
        return s.enabled ? "Reopen to start" : "Off";
    }
    if (s.url.empty()) {
        return "No Wi-Fi";
    }
    if (s.helperMissing) {
        return "Not running";
    }
    if (s.state < 0) {
        return "Starting";
    }
    if (s.state <= 0) {
        return "Not connected";
    }
    return s.state == 1 ? "1 phone" : fmt::format("{} phones", s.state);
}

namespace {
// The launcher starts the phone controller only when this file exists, so the
// Wi-Fi port stays closed for anyone who never turns it on.
void set_phone_controller_enabled(bool enabled)
{
    std::error_code error;
    const auto flag = PartyBoard_ConfigPath / "phone-controller-enabled";
    if (enabled) {
        std::ofstream(flag) << "on\n";
    } else {
        std::filesystem::remove(flag, error);
    }
}
} // namespace

PhoneControllerPanel::PhoneControllerPanel(Rml::Element *parent)
    : Component(append(parent, "div"))
{
    mRoot->SetClass("phone-panel", true);
    listen(mRoot, Rml::EventId::Click, [this](Rml::Event &event) {
        for (auto *element = event.GetTargetElement(); element != nullptr; element = element->GetParentNode()) {
            if (element->HasAttribute("data-phone-toggle")) {
                std::error_code error;
                set_phone_controller_enabled(!std::filesystem::exists(PartyBoard_ConfigPath / "phone-controller-enabled", error));
                mNextRefresh = 0;
                break;
            }
        }
    });

    mStatus = append(mRoot, "div");
    mStatus->SetClass("phone-status", true);
    append(mStatus, "span")->SetClass("phone-status-dot", true);
    mStatusText = append(mStatus, "span");

    mQrTile = append(mRoot, "div");
    mQrTile->SetClass("phone-qr", true);
    mQrImage = append(mQrTile, "img");

    mLinkIntro = append(mRoot, "div");
    mLink = append(mRoot, "div");
    mLink->SetClass("phone-link", true);
    mHelp = append(mRoot, "div");
    mHelp->SetClass("phone-help", true);
    mToggle = append(mRoot, "button");
    mToggle->SetAttribute("data-phone-toggle", "");

    refresh();
}

void PhoneControllerPanel::update()
{
    // Twice a second is plenty to notice a phone joining or a network change.
    if (SDL_GetTicks() >= mNextRefresh) {
        refresh();
    }
    Component::update();
}

void PhoneControllerPanel::refresh()
{
    mNextRefresh = SDL_GetTicks() + 500;
    const Snapshot s = snapshot();

    // Status: one line, coloured like the achievement states.
    Rml::String status;
    Rml::String tone;
#if defined(_WIN32)
    if (!s.configured && s.enabled && s.needsPython) {
        status = "Phone controller needs Python";
        tone = "off";
    } else if (!s.configured && s.enabled && !s.problem.empty()) {
        status = "Phone controller didn't start";
        tone = "off";
    } else if (!s.configured && s.enabled) {
        status = "Phone controller is on: reopen PartyBoard to start it";
        tone = "waiting";
    } else if (!s.configured) {
        status = "Phone controller is off";
        tone = "off";
    } else if (s.url.empty()) {
        status = "This PC isn't on Wi-Fi";
        tone = "off";
    } else if (s.helperMissing) {
        status = "Phone controller didn't start";
        tone = "off";
    } else if (s.state < 0) {
        status = "Starting the phone controller";
        tone = "waiting";
    } else if (s.state == 0) {
        status = "Waiting for a phone";
        tone = "waiting";
    } else {
        status = connected_players_text(s.pads);
        tone = "connected";
    }
#else
    if (!s.configured && s.enabled && s.needsPython) {
        status = "Phone controller needs Apple's command line tools";
        tone = "off";
    } else if (!s.configured && s.enabled) {
        status = "Phone controller is on: reopen PartyBoard to start it";
        tone = "waiting";
    } else if (!s.configured) {
        status = "Phone controller is off";
        tone = "off";
    } else if (s.url.empty()) {
        status = "This Mac isn't on Wi-Fi";
        tone = "off";
    } else if (s.helperMissing) {
        status = "Phone controller didn't start";
        tone = "off";
    } else if (s.state < 0) {
        status = "Starting the phone controller";
        tone = "waiting";
    } else if (s.state == 0) {
        status = "Waiting for a phone";
        tone = "waiting";
    } else {
        status = connected_players_text(s.pads);
        tone = "connected";
    }
#endif
    for (const char *t : {"off", "waiting", "connected"}) {
        mStatus->SetClass(t, tone == t);
    }
    if (status != mShownStatus) {
        set_text(mStatusText, status);
        mShownStatus = status;
    }

    const bool showCode = s.configured && !s.url.empty() && !s.helperMissing;
    const auto display = showCode ? Rml::Style::Display::Block : Rml::Style::Display::None;
    mQrTile->SetProperty(Rml::PropertyId::Display, display);
    mLink->SetProperty(Rml::PropertyId::Display, display);
    if (showCode && s.url != mShownUrl) {
        mQrImage->SetAttribute("src", make_qr_texture(s.url));
        set_text(mLink, s.url);
        mShownUrl = s.url;
    }

    set_text(mToggle, s.enabled ? "Turn off phone controller" : "Turn on phone controller");
#if defined(_WIN32)
    // Windows: partyboard.exe runs the helper with Python from python.org,
    // and its logs are in %APPDATA% (src/port/session_log_win.cpp).
    if (!s.configured && s.enabled && s.needsPython) {
        set_text(mLinkIntro, "Install Python 3 (free, from python.org), then reopen PartyBoard.");
        set_text(mHelp, "");
    } else if (!s.configured && s.enabled && s.problem == "files") {
        set_text(mLinkIntro, "Some PartyBoard files are missing. Reinstall PartyBoard.");
        set_text(mHelp, "");
    } else if (!s.configured && s.enabled && !s.problem.empty()) {
        set_text(mLinkIntro, "Quit PartyBoard and open it again to retry.");
        set_text(mHelp, "The reason is in game.log in the newest folder under "
                        "%APPDATA%\\MarioPartyRD\\Party Board\\logs.");
    } else if (!s.configured && s.enabled) {
        set_text(mLinkIntro, "Quit PartyBoard and open it again; the code and link then appear here.");
        set_text(mHelp, "");
    } else if (!s.configured) {
        set_text(mLinkIntro, "Use phones on your Wi-Fi as controllers. Needs Python 3 (free, from python.org). It "
                             "starts the next time you open PartyBoard.");
        set_text(mHelp, "");
    } else if (s.url.empty()) {
        set_text(mLinkIntro, "Connect this PC to Wi-Fi to get a code for your phone.");
        set_text(mHelp, "");
    } else if (s.helperMissing) {
        set_text(mLinkIntro, "Quit PartyBoard and open it again to restart the phone controller.");
        set_text(mHelp, "If it still doesn't start, see game.log in the newest folder in "
                        "%APPDATA%\\MarioPartyRD\\Party Board\\logs.");
    } else {
        set_text(mLinkIntro, "Scan with your phone's camera, or open this link:");
        set_text(mHelp, "Phones must be on the same Wi-Fi as this PC. If Windows asks about Python, allow it on "
                        "private networks. Up to four phones can join; tap the player number on a phone to switch.");
    }
#else
    if (!s.configured && s.enabled && s.needsPython) {
        set_text(mLinkIntro, "The phone controller runs on Python, which comes with Apple's free command line tools. Open "
                             "Terminal, run: xcode-select --install, then reopen PartyBoard.");
        set_text(mHelp, "");
    } else if (!s.configured && s.enabled) {
        set_text(mLinkIntro, "Quit PartyBoard and open it again; the code and link then appear here.");
        set_text(mHelp, "");
    } else if (!s.configured) {
        set_text(mLinkIntro, "Use iPhones on your Wi-Fi as controllers. Turning it on opens a small page on your Wi-Fi "
                             "network, protected by a code; it starts the next time you open PartyBoard.");
        set_text(mHelp, "");
    } else if (s.url.empty()) {
        set_text(mLinkIntro, "Connect this Mac to Wi-Fi to get a code for your phone.");
        set_text(mHelp, "");
    } else if (s.helperMissing) {
        set_text(mLinkIntro, "Quit PartyBoard and open it again to restart the phone controller.");
        set_text(mHelp, "If it still doesn't start, the reason is in bridge.log in the newest folder under "
                        "~/Library/Logs/PartyBoard.");
    } else {
        set_text(mLinkIntro, "Scan with your iPhone camera, or open this link in Safari:");
        Rml::String help = "Phones need to be on the same Wi-Fi as this Mac. Up to four can join; each takes the next "
                           "free player, and tapping the player number on a phone switches it to another free one. "
                           "Player 1 also stays on the keyboard.";
        if (!s.bookmarkUrl.empty()) {
            help += fmt::format(" For a Home Screen bookmark that works on any Wi-Fi, use {}", s.bookmarkUrl);
        }
        set_text(mHelp, help);
    }
#endif
}

} // namespace partyboard::ui
