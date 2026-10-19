/* Linked into every REL DLL on Windows: installs this DLL's own C runtime
 * fatal-call handlers when it loads. See include/port/crt_fatal_handlers.h. */
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "port/crt_fatal_handlers.h"

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    (void)instance;
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        PartyBoard_InstallModuleCrtHandlers();
    }
    return TRUE;
}
#endif
