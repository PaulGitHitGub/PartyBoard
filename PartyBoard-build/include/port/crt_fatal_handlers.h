#ifndef _PORT_CRT_FATAL_HANDLERS_H
#define _PORT_CRT_FATAL_HANDLERS_H

/* Windows builds link the C runtime statically (/MT), so every module -
 * partyboard.exe, dol.dll and each REL DLL - has its own copy, with its own
 * abort behaviour, SIGABRT handler, invalid-parameter handler and pure-call
 * handler. Only dol.dll's were set (crash_report.cpp). A fatal call in any
 * other module took the runtime's default: __fastfail, exit code 0xC0000409,
 * no crash report, no minidump, nothing in game.log. Include this in one file
 * of a module and call PartyBoard_InstallModuleCrtHandlers() once (DllMain of
 * a REL, main of the exe); the handlers report through dol.dll's crash writer
 * (PartyBoard_CrashFatal: "[crash] fatal ..." in game.log, report, minidump).
 *
 * A real /GS stack-cookie failure or a Control Flow Guard failure still ends in
 * __fastfail, which nothing in the process can intercept. */

#if defined(_WIN32)
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif
void PartyBoard_CrashFatal(const char *reason, const void *moduleAddress);
#ifdef __cplusplus
}
#endif

static void __cdecl PartyBoard_ModuleInvalidParameter(const wchar_t *expression, const wchar_t *function,
    const wchar_t *file, unsigned int line, uintptr_t reserved)
{
    (void)expression; (void)function; (void)file; (void)line; (void)reserved;
    PartyBoard_CrashFatal("CRT_INVALID_PARAMETER", (const void *)&PartyBoard_ModuleInvalidParameter);
}

static void __cdecl PartyBoard_ModulePurecall(void)
{
    PartyBoard_CrashFatal("PURE_VIRTUAL_CALL", (const void *)&PartyBoard_ModulePurecall);
}

static void __cdecl PartyBoard_ModuleAbort(int sig)
{
    (void)sig;
    PartyBoard_CrashFatal("ABORT", (const void *)&PartyBoard_ModuleAbort);
}

static void PartyBoard_InstallModuleCrtHandlers(void)
{
    _set_invalid_parameter_handler(PartyBoard_ModuleInvalidParameter);
    _set_purecall_handler(PartyBoard_ModulePurecall);
    signal(SIGABRT, PartyBoard_ModuleAbort);
    /* abort() raises SIGABRT (above) instead of going to __fastfail. */
    _set_abort_behavior(0, _CALL_REPORTFAULT);
}
#endif

#endif
