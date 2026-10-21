// Scripted pad input for automated tests, off unless PARTYBOARD_TEST_INPUT names a port.
//
// A test driver connects to 127.0.0.1:<port> (on Android: `adb forward tcp:<port> tcp:<port>`;
// PartyBoardActivity only passes the variable on in debuggable builds) and sends lines:
//
//   <buttons> <stickX> <stickY> <frames>
//
// buttons is the PADStatus bit mask in hex (A=100, B=200, X=400, Y=800, START=1000,
// Z=10, R=20, L=40, D-pad 1/2/4/8), sticks are -128..127, and the state is held on pad 1
// for that many frames. Commands queue up; an empty queue leaves the pad to the player.
// Every command is answered with one line once its frames have run, so a driver can wait.
//
// The phone-controller bridge (and test drivers) also send, for any of the
// eight controller ports (pads 4-7 are players 5-8 of the 8-player party):
//
//   S <pad 0-7> <buttons> <stickX> <stickY>   hold this state on that pad until changed
//   R <pad 0-7>                               release that pad
//   #phones <mask>                            pads with a phone on them, bit 0 = pad 1
//   #note <text>                              a line for game.log ("Phone controller: <text>")
//
// A held pad is released if no S line refreshes it for a second (the bridge
// repeats them while a phone is connected) or when the client disconnects.
// The game sends "#controllers <mask>" (bit 0 = pad 1, up to 8 bits) whenever
// the set of ports with a real controller changes, and "#players <4|8>" when
// the number of players changes (8 in the 8-player party).

#ifdef _WIN32
// Winsock before anything that might pull in <windows.h> (and its old winsock.h).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include <aurora/aurora.h>
#include "port/test_input.h"
#include "port/netplay_runtime.h"
#include "port/send_logs.h"
#include "port/window_mode.hpp"
extern "C" {
#include "game/gamework_data.h"
}

#include <dolphin/pad.h>
#if defined(PARTYBOARD_HD_BETA)
#include <SDL3/SDL_joystick.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_video.h>
#include <vector>
#endif

#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <cstring>
#include <deque>
#include <string>

#ifndef _WIN32
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#ifndef MSG_NOSIGNAL
// Older macOS SDKs lack it (SO_NOSIGPIPE on the client socket does the same job),
// and Windows has no SIGPIPE at all.
#define MSG_NOSIGNAL 0
#endif

namespace {

// The same socket code on every platform: Windows (Winsock) names its socket
// type, "no socket" value and close call differently, and has no SIGPIPE.
#ifdef _WIN32
using Socket = SOCKET;
const Socket kNoSocket = INVALID_SOCKET;
void close_socket(Socket s)
{
    closesocket(s);
}
#else
using Socket = int;
constexpr Socket kNoSocket = -1;
void close_socket(Socket s)
{
    close(s);
}
#endif

struct Command {
    PADStatus status {};
    int frames = 0;
};

Socket gListen = kNoSocket;
Socket gClient = kNoSocket;
bool gTried = false;
std::string gPending;
std::deque<Command> gQueue;
// Pads with a phone on them, from the bridge's "#phones <mask>" lines.
unsigned gPhoneMask = 0;

struct Held {
    PADStatus status {};
    bool active = false;
    std::chrono::steady_clock::time_point refreshed;
};
Held gHeld[PAD_MAX_CONTROLLERS];
constexpr auto kHeldTimeout = std::chrono::seconds(1);

PADStatus make_status(unsigned buttons, int x, int y)
{
    PADStatus status {};
    status.button = static_cast<u16>(buttons);
    status.stickX = static_cast<s8>(x < -128 ? -128 : x > 127 ? 127 : x);
    status.stickY = static_cast<s8>(y < -128 ? -128 : y > 127 ? 127 : y);
    status.analogA = (buttons & PAD_BUTTON_A) ? 0xFF : 0;
    status.analogB = (buttons & PAD_BUTTON_B) ? 0xFF : 0;
    status.triggerLeft = (buttons & PAD_TRIGGER_L) ? 0xFF : 0;
    status.triggerRight = (buttons & PAD_TRIGGER_R) ? 0xFF : 0;
    return status;
}

void release_held(int pad)
{
    if (gHeld[pad].active) {
        gHeld[pad].active = false;
        PADClearVirtualStatus(static_cast<u32>(pad));
    }
}

void set_nonblocking(Socket fd)
{
#ifdef _WIN32
    u_long on = 1;
    ioctlsocket(fd, FIONBIO, &on);
    // Like FD_CLOEXEC below: no child process inherits the socket (and keeps
    // the port open after the game quits).
    SetHandleInformation(reinterpret_cast<HANDLE>(fd), HANDLE_FLAG_INHERIT, 0);
#else
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
#endif
}

// One whole line to the client; a full send buffer drops it (non-blocking).
bool send_text(const char *text, int size)
{
#ifdef _WIN32
    return send(gClient, text, size, 0) == size;
#else
    return send(gClient, text, static_cast<size_t>(size), MSG_NOSIGNAL) == size;
#endif
}

#ifdef _WIN32
// game.log lines (the Windows supervisor puts the game's output there): who
// is connected to the input socket and which players have phones.
void log_phone_mask(unsigned mask)
{
    static unsigned logged = 0;
    if (mask == logged) {
        return;
    }
    logged = mask;
    std::string players;
    for (int pad = 0; pad < PAD_MAX_CONTROLLERS; ++pad) {
        if (mask >> pad & 1) {
            players += players.empty() ? "" : ", ";
            players += std::to_string(pad + 1);
        }
    }
    std::fprintf(stderr, "Phone controller: %s\n",
        players.empty() ? "no phones connected" : ("phones on players " + players).c_str());
}
#endif

void open_listener()
{
    gTried = true;
    const char *value = std::getenv("PARTYBOARD_TEST_INPUT");
    const int port = value != nullptr ? std::atoi(value) : 0;
    if (port <= 0 || port > 65535) {
        return;
    }
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::fprintf(stderr, "Test input: Winsock did not start; no input socket\n");
        return;
    }
    gListen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (gListen == kNoSocket) {
        std::fprintf(stderr, "Test input: no socket (error %d)\n", WSAGetLastError());
        return;
    }
    // No SO_REUSEADDR: on Windows it would let another program bind the same
    // port. A port left in TIME_WAIT by the last game does not block a new
    // listener here.
#else
#ifdef SOCK_CLOEXEC
    gListen = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
#else
    // macOS has no SOCK_CLOEXEC; set the same flag right after.
    gListen = socket(AF_INET, SOCK_STREAM, 0);
    if (gListen >= 0) {
        fcntl(gListen, F_SETFD, FD_CLOEXEC);
    }
#endif
    if (gListen < 0) {
        return;
    }
    const int yes = 1;
    setsockopt(gListen, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#endif
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<uint16_t>(port));
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(gListen, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 || listen(gListen, 1) != 0) {
#ifdef _WIN32
        // Mostly WSAEADDRINUSE (10048): another program has the port.
        std::fprintf(stderr, "Test input: can't listen on 127.0.0.1:%d (error %d); phones and test scripts can't connect\n",
            port, WSAGetLastError());
#endif
        close_socket(gListen);
        gListen = kNoSocket;
        return;
    }
    set_nonblocking(gListen);
    std::fprintf(stderr, "Test input listening on 127.0.0.1:%d\n", port);
}

// Which players have a real controller (USB or Bluetooth, any make), sent to
// the phone bridge as "#controllers <mask>" whenever it changes, so phones
// join and switch to the players without one.
int gSentControllerMask = -1;
// Players in the game (4, or 8 in the 8-player party), sent as "#players <n>"
// so the phone bridge offers phones only those players.
int gSentPlayerCount = -1;
std::chrono::steady_clock::time_point gNextControllerCheck;

void report_controllers()
{
    if (gClient == kNoSocket) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now < gNextControllerCheck) {
        return;
    }
    gNextControllerCheck = now + std::chrono::milliseconds(500);
    const int players = GWSystem.eight_players ? 8 : 4;
    if (players != gSentPlayerCount) {
        char line[32];
        const int n = std::snprintf(line, sizeof(line), "#players %d\n", players);
        if (send_text(line, n)) {
            gSentPlayerCount = players;
        }
    }
    int mask = 0;
    for (u32 port = 0; port < PAD_MAX_CONTROLLERS; ++port) {
        if (PADGetIndexForPort(port) >= 0) {
            mask |= 1 << port;
        }
    }
    if (mask == gSentControllerMask) {
        return;
    }
    char line[32];
    const int n = std::snprintf(line, sizeof(line), "#controllers %d\n", mask);
    if (send_text(line, n)) {
        gSentControllerMask = mask;
    }
}

void read_commands()
{
    if (gClient == kNoSocket) {
        gClient = accept(gListen, nullptr, nullptr);
        if (gClient == kNoSocket) {
            return;
        }
        set_nonblocking(gClient);
#ifdef _WIN32
        std::fprintf(stderr, "Test input: client connected (the phone controller bridge or a test script)\n");
#endif
#ifdef SO_NOSIGPIPE
        const int on = 1;
        setsockopt(gClient, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
        gPending.clear();
        gSentControllerMask = -1; // tell the new client where controllers are
        gSentPlayerCount = -1;    // and how many players the game has
    }
    char buffer[512];
    bool closeAfterPending = false;
    for (;;) {
        const auto n = recv(gClient, buffer, sizeof(buffer), 0);
#ifdef _WIN32
        // A client that was killed resets the connection instead of closing
        // it, and Winsock then keeps failing the socket: that is the end too.
        const int error = n < 0 ? WSAGetLastError() : 0;
        const bool closed = n == 0 || (n < 0 && error != WSAEWOULDBLOCK);
        if (closed) {
            std::fprintf(stderr, "Test input: client disconnected%s\n", n == 0 ? "" : " (connection reset)");
            log_phone_mask(0);
        }
#else
        const bool closed = n == 0;
#endif
        if (closed) {
            // A client that sends a line and closes at once (nc) must still be
            // heard: run what arrived before the close, then let it go.
            if (gPending.find('\n') != std::string::npos) {
                closeAfterPending = true; // the next call sees the close again and lets it go
                break;
            }
            close_socket(gClient);
            gClient = kNoSocket;
            gPhoneMask = 0;
            for (int pad = 0; pad < PAD_MAX_CONTROLLERS; ++pad) {
                release_held(pad);
            }
            return;
        }
        if (n < 0) {
            break;
        }
        gPending.append(buffer, static_cast<size_t>(n));
    }
    (void)closeAfterPending;
    std::size_t end;
    while ((end = gPending.find('\n')) != std::string::npos) {
        const std::string line = gPending.substr(0, end);
        gPending.erase(0, end + 1);
#if defined(PARTYBOARD_HD_BETA)
        // "V <n>": attach n virtual SDL gamepads; "VB <i> <button> <0|1>":
        // press/release an SDL button on virtual gamepad i (test runs only).
        if (std::getenv("PARTYBOARD_TEST_CAPTURE") != nullptr && line.size() > 2 && line[0] == 'V') {
            static std::vector<SDL_JoystickID> virtualPads;
            int a = 0, b = 0, c = 0;
            if (std::sscanf(line.c_str(), "VB %d %d %d", &a, &b, &c) == 3) {
                if (a >= 0 && a < static_cast<int>(virtualPads.size())) {
                    if (SDL_Joystick *joy = SDL_GetJoystickFromID(virtualPads[a])) {
                        SDL_SetJoystickVirtualButton(joy, b, c != 0);
                    }
                }
            } else if (std::sscanf(line.c_str(), "V %d", &a) == 1) {
                for (int i = 0; i < a; ++i) {
                    SDL_VirtualJoystickDesc desc;
                    SDL_INIT_INTERFACE(&desc);
                    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
                    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
                    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
                    desc.name = "Test Gamepad";
                    const SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
                    if (id != 0) {
                        SDL_OpenJoystick(id);
                        virtualPads.push_back(id);
                    }
                }
            }
            continue;
        }
#endif
#if defined(PARTYBOARD_HD_BETA)
        // "P 1" / "P 0": pause or resume the online game as this player (test runs only).
        if (std::getenv("PARTYBOARD_TEST_CAPTURE") != nullptr && line.size() > 2 && line[0] == 'P' && line[1] == ' ') {
            PartyBoard_NetplaySetPaused(line[2] == '1');
            continue;
        }
        // "F 0" / "F 1": Windowed / Fullscreen as the menu does it; "FK": an
        // Alt+Enter key press through the normal event path (test runs only).
        if (std::getenv("PARTYBOARD_TEST_CAPTURE") != nullptr && (line == "F 0" || line == "F 1")) {
            partyboard::window_mode::set_fullscreen(line[2] == '1', "test");
            continue;
        }
        // "K <scancode>": one key press and release through the normal event path,
        // e.g. K 81 (down), K 82 (up), K 79 (right), K 40 (Return): drives the
        // PartyBoard menus for screenshots (test runs only).
        if (std::getenv("PARTYBOARD_TEST_CAPTURE") != nullptr && line.size() > 2 && line[0] == 'K' && line[1] == ' ') {
            const int code = std::atoi(line.c_str() + 2);
            if (code > 0 && code < SDL_SCANCODE_COUNT) {
                for (const bool down : {true, false}) {
                    SDL_Event key {};
                    key.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
                    key.key.scancode = static_cast<SDL_Scancode>(code);
                    key.key.key = SDL_GetKeyFromScancode(static_cast<SDL_Scancode>(code), SDL_KMOD_NONE, false);
                    key.key.down = down;
                    SDL_PushEvent(&key);
                }
            }
            continue;
        }
        // "M <x> <y>": a left mouse click at that point of the window, x and y as
        // fractions 0..1 (test runs only; drives mouse-only UI such as the lobby's game list).
        if (std::getenv("PARTYBOARD_TEST_CAPTURE") != nullptr && line.size() > 2 && line[0] == 'M' && line[1] == ' ') {
            float fx = 0.0f, fy = 0.0f;
            SDL_Window *window = SDL_GetKeyboardFocus();
            if (window == nullptr) {
                int count = 0;
                if (SDL_Window **windows = SDL_GetWindows(&count)) {
                    if (count > 0) window = windows[0];
                    SDL_free(windows);
                }
            }
            int w = 0, h = 0;
            if (window != nullptr && std::sscanf(line.c_str() + 2, "%f %f", &fx, &fy) == 2 && SDL_GetWindowSize(window, &w, &h)) {
                const float x = fx * static_cast<float>(w), y = fy * static_cast<float>(h);
                SDL_Event motion {};
                motion.type = SDL_EVENT_MOUSE_MOTION;
                motion.motion.windowID = SDL_GetWindowID(window);
                motion.motion.x = x;
                motion.motion.y = y;
                SDL_PushEvent(&motion);
                for (const bool down : {true, false}) {
                    SDL_Event button {};
                    button.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
                    button.button.windowID = SDL_GetWindowID(window);
                    button.button.button = SDL_BUTTON_LEFT;
                    button.button.down = down;
                    button.button.clicks = 1;
                    button.button.x = x;
                    button.button.y = y;
                    SDL_PushEvent(&button);
                }
                std::fprintf(stderr, "Test input: mouse click at %.0f,%.0f of %dx%d\n", x, y, w, h);
            }
            continue;
        }
        // "W <w> <h>": resize the window to w x h (screen points), as dragging its edge would;
        // windowed only (test runs only: menu screenshots at 4:3, widescreen and the minimum size).
        if (std::getenv("PARTYBOARD_TEST_CAPTURE") != nullptr && line.size() > 2 && line[0] == 'W' && line[1] == ' ') {
            int w = 0, h = 0;
            SDL_Window *window = SDL_GetKeyboardFocus();
            if (window == nullptr) {
                int count = 0;
                if (SDL_Window **windows = SDL_GetWindows(&count)) {
                    if (count > 0) window = windows[0];
                    SDL_free(windows);
                }
            }
            if (window != nullptr && std::sscanf(line.c_str() + 2, "%d %d", &w, &h) == 2 && w > 0 && h > 0) {
                const bool ok = SDL_SetWindowSize(window, w, h);
                int nw = 0, nh = 0;
                SDL_GetWindowSize(window, &nw, &nh);
                std::fprintf(stderr, "Test input: window size %dx%d asked, %dx%d now%s\n", w, h, nw, nh, ok ? "" : " (refused)");
            }
            continue;
        }
        if (std::getenv("PARTYBOARD_TEST_CAPTURE") != nullptr && line == "FK") {
            SDL_Event key {};
            key.type = SDL_EVENT_KEY_DOWN;
            key.key.scancode = SDL_SCANCODE_RETURN;
            key.key.key = SDLK_RETURN;
            key.key.mod = SDL_KMOD_LALT;
            key.key.down = true;
            SDL_PushEvent(&key);
            continue;
        }
#if defined(__APPLE__) || defined(_WIN32)
        // "L": Menu > Send Logs (test runs only).
        if (std::getenv("PARTYBOARD_TEST_CAPTURE") != nullptr && line == "L") {
            PartyBoard_SendLogs();
            continue;
        }
#endif
#endif
        // "C <path>": save the next presented frame (test runs only; needs
        // PARTYBOARD_TEST_CAPTURE=1, so the phone bridge's socket can't use it).
        if (line.size() > 2 && line[0] == 'C' && line[1] == ' ') {
            static const bool allowed = std::getenv("PARTYBOARD_TEST_CAPTURE") != nullptr;
            if (allowed) {
                aurora_capture_next_frame(line.c_str() + 2);
            }
            continue;
        }
        // "#note <text>": a line from the phone bridge for game.log (why a
        // phone disconnected, its screen wake lock); bridge.log has the detail.
        if (line.compare(0, 6, "#note ") == 0) {
            std::string text = line.substr(6, 300);
            for (char &c : text) {
                if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) {
                    c = ' ';
                }
            }
            std::fprintf(stderr, "Phone controller: %s\n", text.c_str());
            continue;
        }
        unsigned mask = 0;
        if (std::sscanf(line.c_str(), "#phones %u", &mask) == 1) {
            gPhoneMask = mask & ((1u << PAD_MAX_CONTROLLERS) - 1);
#ifdef _WIN32
            log_phone_mask(gPhoneMask);
#endif
            continue;
        }
        int pad = -1;
        unsigned buttons = 0;
        int x = 0, y = 0, frames = 1;
        if (line.size() > 1 && line[0] == 'S' && line[1] == ' ') {
            if (std::sscanf(line.c_str() + 2, "%d %x %d %d", &pad, &buttons, &x, &y) == 4 && pad >= 0 && pad < PAD_MAX_CONTROLLERS) {
                gHeld[pad].status = make_status(buttons, x, y);
                gHeld[pad].active = true;
                gHeld[pad].refreshed = std::chrono::steady_clock::now();
            }
            continue;
        }
        if (line.size() > 1 && line[0] == 'R' && line[1] == ' ') {
            if (std::sscanf(line.c_str() + 2, "%d", &pad) == 1 && pad >= 0 && pad < PAD_MAX_CONTROLLERS) {
                release_held(pad);
            }
            continue;
        }
        if (std::sscanf(line.c_str(), "%x %d %d %d", &buttons, &x, &y, &frames) >= 1) {
            Command command;
            command.status = make_status(buttons, x, y);
            command.frames = frames < 1 ? 1 : frames;
            gQueue.push_back(command);
        }
    }
}

} // namespace

extern "C" void PartyBoard_TestInputFrame(void)
{
    if (!gTried) {
        open_listener();
    }
    if (gListen == kNoSocket) {
        return;
    }
    read_commands();
    report_controllers();
    const bool scripted = !gQueue.empty();
    if (scripted) {
        Command &command = gQueue.front();
        // A scripted press must reach the game even while one of PartyBoard's
        // own menus is open (they block pad input); test runs only.
        PADBlockInput(false);
        PADSetVirtualStatus(PAD_CHAN0, &command.status);
        if (--command.frames <= 0) {
            gQueue.pop_front();
            if (gQueue.empty()) {
                PADClearVirtualStatus(PAD_CHAN0);
            }
            if (gClient != kNoSocket) {
                send_text("ok\n", 3);
            }
        }
    }
    // Phone pads; a scripted command owns pad 1 while one is running.
    const auto now = std::chrono::steady_clock::now();
    for (int pad = 0; pad < PAD_MAX_CONTROLLERS; ++pad) {
        if (!gHeld[pad].active) {
            continue;
        }
        if (now - gHeld[pad].refreshed > kHeldTimeout) {
            release_held(pad);
        } else if (!(scripted && pad == 0)) {
            PADSetVirtualStatus(static_cast<u32>(pad), &gHeld[pad].status);
        }
    }
}

extern "C" int PartyBoard_PhoneControllerState(void)
{
    if (gListen == kNoSocket) {
        return -2;
    }
    if (gClient == kNoSocket) {
        return -1;
    }
#ifdef _WIN32
    int phones = 0;
    for (unsigned mask = gPhoneMask; mask != 0; mask >>= 1) {
        phones += static_cast<int>(mask & 1);
    }
    return phones;
#else
    return __builtin_popcount(gPhoneMask);
#endif
}

extern "C" unsigned PartyBoard_PhoneControllerPads(void)
{
    return gClient != kNoSocket ? gPhoneMask : 0;
}

// Buttons an "S" line (a phone or a test driver) holds on a pad right now, 0 if none
// (the online lobby's ready check reads them while the menu blocks game input).
extern "C" unsigned PartyBoard_TestInputPadButtons(int pad)
{
    if (pad < 0 || pad >= PAD_MAX_CONTROLLERS || !gHeld[pad].active) {
        return 0;
    }
    return gHeld[pad].status.button;
}

extern "C" unsigned PartyBoard_TestInputHeldPads(void)
{
#ifndef _WIN32
    unsigned mask = 0;
    for (int pad = 0; pad < PAD_MAX_CONTROLLERS; ++pad) {
        if (gHeld[pad].active) {
            mask |= 1u << pad;
        }
    }
    return mask;
#else
    return 0;
#endif
}
