#ifndef PARTYBOARD_PORT_PHONE_BRIDGE_WIN_HPP
#define PARTYBOARD_PORT_PHONE_BRIDGE_WIN_HPP

// The phone controller on Windows (src/port/phone_bridge_win.cpp): run by the
// session supervisor in partyboard.exe around the game, as the Mac launcher
// ("PartyBoard Phone Controller/PartyBoard-launcher.sh") does on macOS.
#if defined(_WIN32)

#include <filesystem>
#include <string>

namespace partyboard::winphone {

// Writes one "[session] ..." line to game.log; safe from any thread.
using LogFn = void (*)(const std::string &text);

// Before the game starts: when Settings > Input > Phone Controller is on,
// finds Python and the bridge files and gives the game its environment
// (PARTYBOARD_TEST_INPUT, PARTYBOARD_PHONE_CODE/PORT, or
// PARTYBOARD_PHONE_PROBLEM). Returns true when start() should run the bridge.
bool prepare(const std::filesystem::path &sessionDir, const std::filesystem::path &logRoot, LogFn log);

// Once the game is running (process gamePid): starts the bridge.
void start(unsigned long gamePid);

// After the game has ended: stops the bridge and records how it ended.
void stop();

} // namespace partyboard::winphone

#endif // _WIN32
#endif
