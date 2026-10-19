// Loading and unloading a game DLL (dlopen/dlclose, LoadLibrary/FreeLibrary)
// on its own thread. objdll.c calls these from the overlay process, which
// runs on a small game coroutine stack (48 KB). The system loader can need
// more than that: on the Mac, dyld's dlopen of modeseldll ran past the bottom
// of the stack (found under Guard Malloc) and, with no guard there, silently
// overwrote the heap block below - the heap corruption behind crashes in Dawn
// and AppKit. A short-lived thread has a full-size stack; the game waits for
// it, so nothing else changes.
#include <cstdio>
#include <cstring>
#include <thread>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {
void noteOnce()
{
    static bool noted = false;
    if (!noted) {
        noted = true;
        std::fprintf(stderr, "objdll> game DLLs load and unload on a loader thread (full-size stack)\n");
    }
}
} // namespace

extern "C" void *PartyBoard_DllOpen(const char *name, char *error, size_t errorSize)
{
    noteOnce();
    void *handle = nullptr;
    if (error && errorSize) error[0] = '\0';
    std::thread loader([&] {
#if defined(_WIN32)
        handle = reinterpret_cast<void *>(LoadLibraryA(name));
        if (!handle && error && errorSize) {
            std::snprintf(error, errorSize, "LoadLibrary error %lu", GetLastError());
        }
#else
        handle = dlopen(name, RTLD_LAZY);
        if (!handle && error && errorSize) {
            const char *message = dlerror();
            std::snprintf(error, errorSize, "%s", message ? message : "dlopen failed");
        }
#endif
    });
    loader.join();
    return handle;
}

extern "C" void PartyBoard_DllClose(void *handle)
{
    if (!handle) return;
    noteOnce();
    std::thread unloader([handle] {
#if defined(_WIN32)
        FreeLibrary(reinterpret_cast<HMODULE>(handle));
#else
        dlclose(handle);
#endif
    });
    unloader.join();
}
