#include <aurora/main.h>
#include "port/netplay_transport.hpp"
#include "port/netplay_runtime.h"
#include "port/rollback.h"
#include "port/crash_report.h"
#include "port/mem_diagnostics.h"
#include "port/audio_lifetime.h"
#include "port/crash_manifest.h"
#include "port/crash_uploader.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#include <cstdio>
#include <windows.h>
#include "port/send_logs.h"
#include "port/crt_fatal_handlers.h"
#include <exception>
#endif

extern "C" int port_main(int argc, char* argv[]);
#if defined(_WIN32)
extern "C" bool PartyBoard_WinSessionSupervise(int *exitCode); // port/session_log_win.cpp
extern "C" bool PartyBoard_WinFastFailCatcher(int *exitCode); // port/fastfail_catcher_win.cpp
#endif
extern "C" bool PartyBoard_OnlineCheckDisc(void);
extern "C" bool PartyBoard_TailnetLobbySelfTest(void);
extern "C" int PartyBoard_TailnetRelaunchProbe(int argc, char *argv[]);
extern "C" int PartyBoard_TailnetLobbyPairTest(int argc, char *argv[]);
extern "C" bool PartyBoard_OnlineBarrierProbe(void);
extern "C" bool PartyBoard_ModsRunSelfTest(void);

int main(int argc, char *argv[])
{
    // First statement in the process: a fault before this point would leave no
    // report, and the whole point is that no termination goes unexplained. The
    // session directory and seat come from the launcher's environment; both are
    // refined below once the command line has been parsed.
#if defined(_WIN32)
    // Windows has no launcher script: this process becomes the supervisor that
    // makes the session log folder, runs the game as a child with its output
    // in game.log (crash files beside it) and records how it ended.
    {
        int exitCode = 0;
        // Test aid: PARTYBOARD_CATCH_FASTFAIL=1 (see fastfail_catcher_win.cpp).
        if (PartyBoard_WinFastFailCatcher(&exitCode)) return exitCode;
        if (PartyBoard_WinSessionSupervise(&exitCode)) return exitCode;
        // The game itself: unbuffered output, so game.log timestamps are exact
        // (each module has its own C runtime; dol.dll does the same for its own).
        std::setvbuf(stdout, nullptr, _IONBF, 0);
        std::setvbuf(stderr, nullptr, _IONBF, 0);
    }
#endif
    PartyBoard_CrashReportInit(nullptr, -1, nullptr);
#if defined(_WIN32)
    // partyboard.exe has its own static C runtime; dol.dll's handlers do not
    // cover it (include/port/crt_fatal_handlers.h).
    PartyBoard_InstallModuleCrtHandlers();
    std::set_terminate([] { PartyBoard_CrashFatal("STD_TERMINATE", reinterpret_cast<const void *>(&PartyBoard_ModuleAbort)); });
#endif

    if (argc == 2 && std::strcmp(argv[1], "--crash-report-self-test") == 0)
        return PartyBoard_CrashReportRunSelfTest() ? 0 : 1;
    if (argc == 2 && std::strcmp(argv[1], "--mem-diagnostics-self-test") == 0)
        return PartyBoard_MemDiagRunSelfTest() ? 0 : 1;
    if (argc == 2 && std::strcmp(argv[1], "--audio-lifetime-self-test") == 0)
        return PartyBoard_AudioLifetimeRunSelfTest() ? 0 : 1;
    if (argc == 2 && std::strcmp(argv[1], "--crash-manifest-self-test") == 0)
        return PartyBoard_CrashManifestRunSelfTest() ? 0 : 1;
    if (argc == 2 && std::strcmp(argv[1], "--crash-uploader-self-test") == 0)
        return PartyBoard_CrashUploaderRunSelfTest() ? 0 : 1;
#if defined(_WIN32)
    // Menu > Send Logs' zip code against made-up sessions in %TEMP% (headless).
    if (argc == 2 && std::strcmp(argv[1], "--send-logs-self-test") == 0)
        return PartyBoard_SendLogsRunSelfTest() ? 0 : 1;
#endif
    if (argc == 3 && std::strcmp(argv[1], "--crash-report-provoke") == 0) {
        // Raises a real exception on purpose; the process is expected to die of
        // it so the reporter can be verified end to end.
#if defined(_WIN32)
        // Fatal calls in this module's own C runtime (crt_fatal_handlers.h),
        // and a raw fail-fast that nothing in-process can catch.
        if (std::strcmp(argv[2], "exe-abort") == 0) std::abort();
        if (std::strcmp(argv[2], "exe-invalid-parameter") == 0) {
            char tooSmall[2];
            strcpy_s(tooSmall, sizeof(tooSmall), "too long");
        }
        if (std::strcmp(argv[2], "exe-terminate") == 0) std::terminate();
        if (std::strcmp(argv[2], "fastfail") == 0) __fastfail(7 /* FAST_FAIL_FATAL_APP_EXIT */);
#endif
        PartyBoard_CrashReportProvoke(argv[2]);
        return 0; // only reached if the fault somehow did not happen
    }
    if (argc == 2 && std::strcmp(argv[1], "--online-disc-check") == 0)
        return PartyBoard_OnlineCheckDisc() ? 0 : 3;
    // The Tailscale lobby without a window (port/online/tailnet_lobby.cpp);
    // the seats it relaunches report what they were given instead of playing.
    if (argc == 2 && std::strcmp(argv[1], "--tailnet-lobby-self-test") == 0)
        return PartyBoard_TailnetLobbySelfTest() ? 0 : 1;
    // The same lobby between two real computers (cross-play check), no window.
    if (argc >= 4 && std::strcmp(argv[1], "--tailnet-lobby-pair-test") == 0)
        return PartyBoard_TailnetLobbyPairTest(argc, argv);
    if (const char *probe = std::getenv("PARTYBOARD_TAILNET_RELAUNCH_PROBE"); probe != nullptr && *probe != '\0'
        && std::getenv("PARTYBOARD_NETPLAY_TEST_PROFILE") != nullptr)
        return PartyBoard_TailnetRelaunchProbe(argc, argv);
    if (argc == 2 && std::strcmp(argv[1], "--mods-self-test") == 0)
        return PartyBoard_ModsRunSelfTest() ? 0 : 1;
    if (argc == 2 && std::strcmp(argv[1], "--rollback-self-test") == 0)
        return PartyBoard_RollbackRunSelfTest() ? 0 : 1;
    if (argc == 2 && std::strcmp(argv[1], "--netplay-self-test") == 0) {
        // Every component runs, and every component reports. The chain of &&
        // this replaced stopped at the first failure, so one broken component
        // hid an unknown number of others behind it - and a component became
        // silently untested the day anything before it started failing. A
        // release gate that cannot say which of its nine checks ran is not a
        // gate.
        int total = 0;
        int failed = 0;
        auto check = [&](const char* name, bool ok) {
            ++total;
            if (!ok) ++failed;
            std::printf("SELFTEST %-16s %s\n", name, ok ? "PASS" : "FAIL");
            std::fflush(stdout);
        };
        check("rollback", PartyBoard_RollbackRunSelfTest());
        check("transport", PartyBoard_NetTransportRunSelfTest());
        check("netplay-runtime", PartyBoard_NetplayRuntimeRunSelfTest());
        check("crash-report", PartyBoard_CrashReportRunSelfTest());
        check("mem-diagnostics", PartyBoard_MemDiagRunSelfTest());
        check("audio-lifetime", PartyBoard_AudioLifetimeRunSelfTest());
        check("crash-manifest", PartyBoard_CrashManifestRunSelfTest());
        check("crash-uploader", PartyBoard_CrashUploaderRunSelfTest());
        check("mod-overlay", PartyBoard_ModsRunSelfTest());
        std::printf("SELFTEST total=%d failed=%d\n", total, failed);
        std::fflush(stdout);
        return failed == 0 ? 0 : 1;
    }

    // Anything the previous run left behind becomes a queued incident here, at
    // the next launch rather than while the last one was dying. A no-op in a
    // supervised session, where reports belong to the campaign directory.
    PartyBoard_CrashQueueScan(PartyBoard_CrashReportDirectory());

    if (!PartyBoard_NetplayConfigureFromArgs(argc, argv))
        return 2;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--netplay-start-probe") == 0
            && !PartyBoard_OnlineBarrierProbe()) { PartyBoard_NetplayShutdown(); return 4; }
    }
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--netplay-pad-probe") == 0) {
            const bool passed = PartyBoard_NetplayPadRunProbe();
            PartyBoard_NetplayShutdown();
            return passed ? 0 : 1;
        }
    }

#if defined(_WIN32)
    HANDLE singleInstance = nullptr;
    // Test aid: parallel test games on one PC (each with its own isolated test
    // profile and silent dummy audio, so they can't share the sound device the
    // guard below protects). Needs all three; a normal launch never sets them.
    const char *audioDriver = std::getenv("SDL_AUDIO_DRIVER");
    const bool parallelTest = std::getenv("PARTYBOARD_TEST_MULTI_INSTANCE") != nullptr
        && std::getenv("PARTYBOARD_TEST_PROFILE") != nullptr && audioDriver != nullptr
        && std::strcmp(audioDriver, "dummy") == 0;
    if (parallelTest)
        std::fputs("[session] parallel test copy: single-instance guard skipped (test profile, dummy audio)\n", stderr);
    if (!PartyBoard_NetplayAllowsMultipleInstances() && !parallelTest)
    {
        singleInstance = CreateMutexW(nullptr, TRUE, L"Local\\MarioPartyRD.PartyBoard.SingleInstance");
        if (singleInstance == nullptr || GetLastError() == ERROR_ALREADY_EXISTS)
        {
            if (singleInstance != nullptr)
                CloseHandle(singleInstance);
            std::fputs("PartyBoard is already running; refusing a second audio instance.\n", stderr);
            return 0;
        }
    }
#endif

    const int result = port_main(argc, argv);
    // Reaching here means the main loop returned of its own accord, so the
    // termination is expected and must not be classified as a crash.
    PartyBoard_CrashNoteNormalExit();

#if defined(_WIN32)
    if (singleInstance != nullptr)
    {
        ReleaseMutex(singleInstance);
        CloseHandle(singleInstance);
    }
#endif
    PartyBoard_NetplayShutdown();
    return result;
}
