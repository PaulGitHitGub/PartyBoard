#pragma once

#include <SDL3/SDL_events.h>

// Windowed or fullscreen, on desktop computers (local only: never part of an
// online game's state). "Fullscreen" is SDL's borderless desktop fullscreen:
// the window covers the whole display it is on, over the Windows taskbar, with
// no title bar; on macOS it is a fullscreen Space with the menu bar and Dock
// hidden. The choice is the video.enableFullscreen setting.
namespace partyboard::window_mode {

// Before the window exists: whether to start fullscreen. A saved choice wins;
// without one, test profiles (PARTYBOARD_TEST_PROFILE /
// PARTYBOARD_NETPLAY_TEST_PROFILE) start windowed so a test game never covers
// the screen, and everyone else gets the default (Fullscreen on desktop).
bool startup_choice();

// After the window exists: log what the startup choice produced, and retry
// once if the window did not come up fullscreen when it should have.
void on_window_created();

// Switch now, save the choice and log it. `source` names who asked
// ("menu", "Alt+Enter", ...).
void set_fullscreen(bool fullscreen, const char *source);

bool is_fullscreen();

// Every SDL event, before the menus see it. Handles Alt+Enter (Windows/Linux)
// and keeps the saved choice in step when the system switched the window
// itself (macOS green button, Cmd+Ctrl+F). True: the event was used up.
bool handle_event(const SDL_Event &event);

} // namespace partyboard::window_mode
