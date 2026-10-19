// macOS window tweaks.
//
// A minimized window on macOS stops the whole game loop, not just drawing
// (aurora_begin_frame fails while the window is not presentable and game_main
// skips the frame). Offline that is a pause. In an online lockstep session it
// silences this player's input, every other player waits, and after two
// minutes the session ends. So online, the window cannot be minimized: the
// yellow button and Cmd-M are disabled. Hiding (Cmd-H) and covering the window
// keep the loop running and stay available. Windows keeps a minimized game
// running, so it needs no equivalent (portmain.cpp logs minimizing online).

#include <TargetConditionals.h>

#if TARGET_OS_OSX
#import <AppKit/AppKit.h>
#include <SDL3/SDL_video.h>

void PartyBoard_DisableWindowMinimize(void)
{
    int count = 0;
    SDL_Window **windows = SDL_GetWindows(&count);
    for (int i = 0; windows != NULL && i < count; ++i) {
        NSWindow *window = (__bridge NSWindow *)SDL_GetPointerProperty(SDL_GetWindowProperties(windows[i]),
            SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, NULL);
        if (window != nil) {
            window.styleMask &= ~NSWindowStyleMaskMiniaturizable;
        }
    }
    SDL_free(windows);
}
#else
void PartyBoard_DisableWindowMinimize(void) {}
#endif
