#include "port/window_mode.hpp"

#include "port/config.hpp"
#include "port/settings.h"

#include "aurora/lib/logging.hpp"
#include "aurora/lib/window.hpp"

#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_video.h>

#include <cstdlib>
#include <string>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace partyboard::window_mode {
namespace {

aurora::Module Log("partyboard::window");

#if !defined(__ANDROID__) && !(defined(__APPLE__) && (TARGET_OS_IOS || TARGET_OS_TV))
constexpr bool kDesktop = true;
#else
constexpr bool kDesktop = false;
#endif

// What the last request asked for, so the event that follows it can say
// whether the system did what was asked.
bool sPendingRequest = false;
bool sPendingFullscreen = false;
const char *sPendingSource = "";

const char *mode_name(bool fullscreen) { return fullscreen ? "Fullscreen" : "Windowed"; }

// "display 1 'DELL U2720Q' 3840x2160 (scale 1.50, usable 3840x2100 at 0,0)
//  window 3840x2160 at 0,0 (pixels 3840x2160)"
std::string describe(SDL_Window *window)
{
    if (window == nullptr) {
        return "no window";
    }
    std::string text;
    const SDL_DisplayID display = SDL_GetDisplayForWindow(window);
    if (display == 0) {
        text = fmt::format("display unknown ({})", SDL_GetError());
    } else {
        const char *name = SDL_GetDisplayName(display);
        SDL_Rect bounds {};
        SDL_Rect usable {};
        SDL_GetDisplayBounds(display, &bounds);
        SDL_GetDisplayUsableBounds(display, &usable);
        const SDL_DisplayMode *desktop = SDL_GetDesktopDisplayMode(display);
        text = fmt::format("display {} '{}' {}x{}", display, name != nullptr ? name : "?", bounds.w, bounds.h);
        if (desktop != nullptr) {
            text += fmt::format(" ({:.0f} Hz, density {:.2f})", desktop->refresh_rate, desktop->pixel_density);
        }
        text += fmt::format(", scale {:.2f}, usable {}x{} at {},{}", SDL_GetWindowDisplayScale(window), usable.w,
            usable.h, usable.x - bounds.x, usable.y - bounds.y);
    }
    int w = 0, h = 0, pw = 0, ph = 0, x = 0, y = 0;
    SDL_GetWindowSize(window, &w, &h);
    SDL_GetWindowSizeInPixels(window, &pw, &ph);
    SDL_GetWindowPosition(window, &x, &y);
    const SDL_WindowFlags flags = SDL_GetWindowFlags(window);
    text += fmt::format("; window {}x{} at {},{} (pixels {}x{}){}{}{}", w, h, x, y, pw, ph,
        (flags & SDL_WINDOW_FULLSCREEN) != 0u ? ", fullscreen" : "",
        (flags & SDL_WINDOW_MAXIMIZED) != 0u ? ", maximized" : "",
        (flags & SDL_WINDOW_BORDERLESS) != 0u ? ", borderless" : "");
    return text;
}

void save_choice(bool fullscreen)
{
    auto &setting = getSettings().video.enableFullscreen;
    if (setting.getValue() != fullscreen) {
        setting.setValue(fullscreen);
        config::Save();
    }
}

} // namespace

namespace {
const char *sStartupSource = "saved choice";
}

bool startup_choice()
{
    auto &setting = getSettings().video.enableFullscreen;
    if (setting.getLayer() != config::ConfigVarLayer::Default) {
        sStartupSource = "saved choice";
    } else if ((std::getenv("PARTYBOARD_TEST_PROFILE") != nullptr
                   || std::getenv("PARTYBOARD_NETPLAY_TEST_PROFILE") != nullptr)
        // Checking the real default itself (hidden Windows session only).
        && std::getenv("PARTYBOARD_TEST_DEFAULT_FULLSCREEN") == nullptr) {
        // Not saved: the test's settings file stays without the key.
        setting.setOverrideValue(false);
        sStartupSource = "test profile without a saved choice";
    } else {
        sStartupSource = "default, nothing saved";
    }
    return setting.getValue();
}

bool is_fullscreen()
{
    SDL_Window *window = aurora::window::get_sdl_window();
    return window != nullptr && (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0u;
}

void on_window_created()
{
    if constexpr (!kDesktop) {
        return;
    }
#if defined(__APPLE__)
    // In a fullscreen Space the menu bar would otherwise slide down whenever
    // the pointer touches the top of the screen after the green button.
    SDL_SetHint(SDL_HINT_VIDEO_MAC_FULLSCREEN_MENU_VISIBILITY, "0");
#endif
    SDL_Window *window = aurora::window::get_sdl_window();
    const bool wanted = getSettings().video.enableFullscreen.getValue();
    Log.info("[window] startup mode {} ({}); {}", mode_name(wanted), sStartupSource, describe(window));
    if (wanted) {
        // SDL reports the switch when the hidden window is first shown.
        sPendingRequest = true;
        sPendingFullscreen = true;
        sPendingSource = "startup";
    }
    if (window != nullptr && wanted && !is_fullscreen()) {
        // The window is created hidden with the fullscreen flag; if that did
        // not stick, ask again now that it exists.
        if (!SDL_SetWindowFullscreenMode(window, nullptr) || !SDL_SetWindowFullscreen(window, true)) {
            Log.warn("[window] startup fullscreen failed: {}; staying windowed", SDL_GetError());
        } else {
            sPendingRequest = true;
            sPendingFullscreen = true;
            sPendingSource = "startup retry";
            Log.info("[window] startup fullscreen requested again");
        }
    }
}

void set_fullscreen(bool fullscreen, const char *source)
{
    save_choice(fullscreen);
    SDL_Window *window = aurora::window::get_sdl_window();
    if (window == nullptr) {
        Log.warn("[window] {} asked for {}, but there is no window yet", source, mode_name(fullscreen));
        return;
    }
    Log.info("[window] {} asked for {} (was {}); {}", source, mode_name(fullscreen), mode_name(is_fullscreen()),
        describe(window));
    // nullptr mode: borderless fullscreen at the desktop resolution of the
    // display the window is on, never an exclusive video mode change.
    if (fullscreen && !SDL_SetWindowFullscreenMode(window, nullptr)) {
        Log.warn("[window] could not select borderless fullscreen: {}", SDL_GetError());
    }
    if (!SDL_SetWindowFullscreen(window, fullscreen)) {
        Log.warn("[window] switching to {} failed: {}; window stays {}", mode_name(fullscreen), SDL_GetError(),
            mode_name(is_fullscreen()));
        save_choice(is_fullscreen());
        return;
    }
    sPendingRequest = true;
    sPendingFullscreen = fullscreen;
    sPendingSource = source;
}

bool handle_event(const SDL_Event &event)
{
    if constexpr (!kDesktop) {
        return false;
    }
    switch (event.type) {
#if !defined(__APPLE__)
    // Alt+Enter, the Windows convention. (macOS has Cmd+Ctrl+F and the green
    // button, both handled by the system and reported below.)
    case SDL_EVENT_KEY_DOWN:
        if ((event.key.scancode == SDL_SCANCODE_RETURN || event.key.scancode == SDL_SCANCODE_KP_ENTER)
            && (event.key.mod & SDL_KMOD_ALT) != 0 && (event.key.mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI)) == 0) {
            if (!event.key.repeat) {
                set_fullscreen(!is_fullscreen(), "Alt+Enter");
            }
            return true;
        }
        break;
#endif
    case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
    case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN: {
        const bool now = event.type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN;
        SDL_Window *window = aurora::window::get_sdl_window();
        if (sPendingRequest && sPendingFullscreen == now) {
            Log.info("[window] now {} ({}); {}", mode_name(now), sPendingSource, describe(window));
        } else {
            // The system switched by itself: green button, Cmd+Ctrl+F, or the
            // opposite of what was last asked for.
            Log.info("[window] now {} (changed by the system{}); {}", mode_name(now),
                sPendingRequest ? fmt::format(", after a request for {}", mode_name(sPendingFullscreen)) : "",
                describe(window));
        }
        sPendingRequest = false;
        save_choice(now);
        break;
    }
    case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
        Log.info("[window] moved to another display; {}", describe(aurora::window::get_sdl_window()));
        break;
    default:
        break;
    }
    return false;
}

} // namespace partyboard::window_mode
