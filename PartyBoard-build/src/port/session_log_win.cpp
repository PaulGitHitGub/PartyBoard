// Windows session logs: what the Mac launcher (PartyBoard-launcher.sh) does on
// macOS, built into partyboard.exe because a Windows player just double-clicks
// the exe.
//
// partyboard.exe starts as a small supervisor. It makes a folder for this
// launch,
//   %APPDATA%\MarioPartyRD\Party Board\logs\YYYY-MM-DD_HH-MM-SS\
// then starts itself again as the real game with stdout/stderr piped back to
// it. Every line the game (or any of its DLLs) prints is written to game.log
// in that folder with the local time to the millisecond, like the Mac's
// game.log, and echoed to the console. PARTYBOARD_CRASH_DIR is set for the
// game, so a crash report, minidump and stack-usage file land in the same
// folder. When the game ends the supervisor records how (normal exit, or the
// Windows exception code of a crash). The newest 20 sessions are kept.
//
// Why a second process: the Windows build links the C runtime statically
// (CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded), so the exe, dol.dll and every
// REL DLL each have their own stdout and environment, initialised before
// main(). Redirecting inside one process would miss most of the output; a
// child process started with redirected handles misses nothing, including a
// crash before main().
//
// Never stops the game: if anything here fails, the game runs as before.
//
// The phone controller runs from here too (src/port/phone_bridge_win.cpp):
// when it is turned on, the supervisor starts its Python helper beside the
// game and stops it when the game ends, logging each step in game.log.
#if defined(_WIN32)

#include <windows.h>
#include <shlobj.h>

#include "port/phone_bridge_win.hpp"
#include "partyboard_version.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

constexpr int kKeepSessions = 20;
constexpr const char *kChildMarker = "PARTYBOARD_SESSION_CHILD";

HANDLE gLogFile = INVALID_HANDLE_VALUE;
HANDLE gConsole = INVALID_HANDLE_VALUE;
// The phone controller's watcher thread writes to game.log too.
std::mutex gLogMutex;
// Whether the game's output so far ends with a full line.
bool gAtLineStart = true;

void writeAll(HANDLE h, const char *data, DWORD size)
{
    while (h != INVALID_HANDLE_VALUE && h != nullptr && size > 0) {
        DWORD written = 0;
        if (!WriteFile(h, data, size, &written, nullptr) || written == 0) return;
        data += written;
        size -= written;
    }
}

std::string stamp()
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    char out[32];
    std::snprintf(out, sizeof(out), "%02u:%02u:%02u.%03u ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    return out;
}

// One supervisor line in game.log (also shown on the console).
void logLine(HANDLE console, const std::string &text)
{
    std::lock_guard<std::mutex> lock(gLogMutex);
    std::string line = stamp() + "[session] " + text + "\r\n";
    if (!gAtLineStart) {
        // Never inside a line the game hasn't finished; the rest of it gets
        // its own timestamp.
        line.insert(0, "\r\n");
        gAtLineStart = true;
    }
    writeAll(gLogFile, line.data(), static_cast<DWORD>(line.size()));
    const std::string shown = "[session] " + text + "\r\n";
    writeAll(console, shown.data(), static_cast<DWORD>(shown.size()));
}

void phoneLog(const std::string &text)
{
    logLine(gConsole, text);
}

// A test run with its own settings folder (PARTYBOARD_TEST_PROFILE, never set
// by a normal launch) keeps its logs there too, away from the player's.
bool testProfile()
{
    const char *profile = std::getenv("PARTYBOARD_TEST_PROFILE");
    return profile != nullptr && profile[0] != '\0';
}

std::string logRoot()
{
    // Test runs keep their session folders out of the player's own logs (and
    // out of its newest-20 count).
    if (const char *root = std::getenv("PARTYBOARD_SESSION_LOG_ROOT"); root != nullptr && *root != '\0') return root;
    if (testProfile()) return (fs::path(std::getenv("PARTYBOARD_TEST_PROFILE")) / "logs").string();
    PWSTR appData = nullptr;
    std::string root;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData))) {
        root = (fs::path(appData) / "MarioPartyRD" / "Party Board" / "logs").string();
    }
    CoTaskMemFree(appData);
    return root;
}

void pruneOldSessions(const fs::path &root)
{
    std::error_code ec;
    std::vector<fs::path> sessions;
    for (const auto &entry : fs::directory_iterator(root, ec)) {
        if (entry.is_directory(ec)) sessions.push_back(entry.path());
    }
    std::sort(sessions.begin(), sessions.end()); // names are timestamps
    while (sessions.size() > static_cast<size_t>(kKeepSessions)) {
        fs::remove_all(sessions.front(), ec);
        sessions.erase(sessions.begin());
    }
}

std::string windowsVersion()
{
    OSVERSIONINFOEXW v = {};
    v.dwOSVersionInfoSize = sizeof(v);
    using RtlGetVersionFn = LONG(WINAPI *)(OSVERSIONINFOEXW *);
    if (HMODULE ntdll = GetModuleHandleA("ntdll.dll")) {
        if (auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"))) fn(&v);
    }
    char out[64];
    std::snprintf(out, sizeof(out), "%lu.%lu build %lu", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber);
    return out;
}

std::string exitDescription(DWORD code)
{
    const char *name = nullptr;
    switch (code) {
    case 0: return "normally (exit code 0)";
    case 0xC0000005: name = "ACCESS_VIOLATION (bad memory read/write)"; break;
    case 0xC00000FD: name = "STACK_OVERFLOW"; break;
    case 0xC0000409: name = "STACK_BUFFER_OVERRUN / fail-fast"; break;
    case 0xC0000374: name = "HEAP_CORRUPTION"; break;
    case 0xC000001D: name = "ILLEGAL_INSTRUCTION"; break;
    case 0xC0000094: name = "INTEGER_DIVIDE_BY_ZERO"; break;
    case 0x80000003: name = "BREAKPOINT"; break;
    case 0xC0000135: name = "DLL_NOT_FOUND"; break;
    case 0xC0000142: name = "DLL_INIT_FAILED"; break;
    case 3: name = "abort()"; break;
    default: break;
    }
    char out[160];
    if (name) {
        std::snprintf(out, sizeof(out), "with a CRASH: 0x%08lX %s", code, name);
    } else {
        std::snprintf(out, sizeof(out), "with exit code %lu (0x%08lX)", code, code);
    }
    return out;
}

// After a crash (or any abnormal exit), copy the session folder into the
// player's iCloud Drive folder for the Windows build, so the report syncs back
// to the Mac without anyone sending it (the Mac build's Send Logs equivalent).
// Only when iCloud for Windows and that folder already exist; keeps the newest
// 20 reports there.
void shareCrashToICloud(HANDLE console, const fs::path &session)
{
    const char *profile = std::getenv("USERPROFILE");
    if (!profile) return;
    std::error_code ec;
    const fs::path shared = fs::path(profile) / "iCloudDrive" / "Mario Party 4 (Windows)";
    if (!fs::is_directory(shared, ec)) return;
    const fs::path reports = shared / "Crash Reports";
    const fs::path target = reports / session.filename();
    fs::create_directories(target, ec);
    if (ec) return;
    fs::copy(session, target, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    if (ec) {
        logLine(console, "could not copy this crash to iCloud Drive: " + ec.message());
        return;
    }
    pruneOldSessions(reports);
    logLine(console, "crash report copied to " + target.string());
}

// A release zip (dev/windows-build/package.cmd) puts its name in version.txt
// beside partyboard.exe; returns its first line, or "" for a plain build.
std::string packageVersion(const char *exe)
{
    std::error_code ec;
    const fs::path file = fs::path(exe).parent_path() / "version.txt";
    if (!fs::is_regular_file(file, ec)) return {};
    FILE *f = _wfopen(file.c_str(), L"rb");
    if (!f) return {};
    char line[200] = {};
    const bool got = std::fgets(line, sizeof(line), f) != nullptr;
    std::fclose(f);
    if (!got) return {};
    std::string text = line;
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
    return text;
}

// The online lobby (port/online/tailnet_lobby.cpp) starts this copy just
// before the copy that hosted or joined quits, and passes it a handle to that
// process. Waiting for it to end frees what the online game needs: its game
// port, the phone controller's port and the controllers. The variable is
// removed, so the game started from here does not wait again. Returns a line
// for game.log, or "" when this is an ordinary launch.
std::string waitForPreviousCopy()
{
    constexpr const char *kVariable = "PARTYBOARD_RELAUNCH_WAIT_HANDLE";
    char value[32] = {};
    const DWORD size = GetEnvironmentVariableA(kVariable, value, sizeof(value));
    if (size == 0 || size >= sizeof(value)) return {};
    SetEnvironmentVariableA(kVariable, nullptr);
    HANDLE previous = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(std::strtoull(value, nullptr, 10)));
    const DWORD pid = GetProcessId(previous);
    if (pid == 0) return "started by the online lobby, but its handle to the previous copy is not usable; starting now";
    const ULONGLONG began = GetTickCount64();
    const DWORD waited = WaitForSingleObject(previous, 30000);
    const ULONGLONG ms = GetTickCount64() - began;
    CloseHandle(previous);
    if (waited == WAIT_OBJECT_0) {
        return "started by the online lobby; the copy before it (process " + std::to_string(pid) + ") quit after "
            + std::to_string(ms) + " ms";
    }
    return "started by the online lobby; the copy before it (process " + std::to_string(pid)
        + ") is still running after 30 s; starting anyway";
}

} // namespace

// Returns true when this process was the supervisor (the game has already run
// and ended; *exitCode is its exit code). Returns false when this process
// should run the game itself: it is the child, logging is turned off, or the
// supervisor could not start the child.
extern "C" bool PartyBoard_WinSessionSupervise(int *exitCode)
{
    if (std::getenv(kChildMarker) != nullptr) return false;
    const std::string relaunch = waitForPreviousCopy();
    // A test harness that chose its own folder keeps control.
    if (std::getenv("PARTYBOARD_CRASH_DIR") != nullptr || std::getenv("PARTYBOARD_NO_SESSION_LOG") != nullptr) {
        if (!relaunch.empty()) std::fprintf(stderr, "[session] %s\n", relaunch.c_str());
        return false;
    }

    const std::string root = logRoot();
    if (root.empty()) return false;
    SYSTEMTIME t;
    GetLocalTime(&t);
    char name[64];
    std::snprintf(name, sizeof(name), "%04u-%02u-%02u_%02u-%02u-%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
        t.wSecond);
    std::error_code ec;
    fs::create_directories(root, ec);
    // create_directory says whether this call made the folder, so two copies
    // starting in the same second (the online lobby relaunching both seats of
    // a one-PC test) each get their own instead of fighting over one game.log.
    fs::path dir = fs::path(root) / name;
    bool made = fs::create_directory(dir, ec);
    for (int n = 2; !made && !ec && n < 100; n++) {
        dir = fs::path(root) / (std::string(name) + "_" + std::to_string(n));
        made = fs::create_directory(dir, ec);
    }
    if (!made) {
        if (!relaunch.empty()) std::fprintf(stderr, "[session] %s\n", relaunch.c_str());
        return false;
    }
    pruneOldSessions(root);

    gLogFile = CreateFileA((dir / "game.log").string().c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (gLogFile == INVALID_HANDLE_VALUE) return false;

    HANDLE console = GetStdHandle(STD_OUTPUT_HANDLE);
    gConsole = console;
    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH - 1);
    logLine(console, std::string("PartyBoard for Windows, built ") + __DATE__ + " " + __TIME__);
    // The same version, revision and branch as the crash report and the exe's file properties. A
    // copy without .git gets them from the file the Mac wrote when copying it (CMakeLists.txt).
    logLine(console, "version " PARTY_BOARD_WC_DESCRIBE " (" PARTY_BOARD_VERSION_STRING "), branch " PARTY_BOARD_WC_BRANCH
            ", revision " PARTY_BOARD_WC_REVISION ", committed " PARTY_BOARD_WC_DATE ", version from "
            PARTY_BOARD_VERSION_SOURCE);
    logLine(console, "Windows " + windowsVersion());
    logLine(console, std::string("executable ") + exe);
    if (const std::string package = packageVersion(exe); !package.empty()) logLine(console, "package " + package);
    logLine(console, "log folder " + dir.string());
    if (!relaunch.empty()) logLine(console, relaunch);

    // Phone controller: decided (and its environment set) before the game starts.
    const bool phone = partyboard::winphone::prepare(dir, root, phoneLog);

    // Pipe for the game's stdout+stderr; only the write end is inherited.
    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
    HANDLE pipeRead = nullptr, pipeWrite = nullptr;
    if (!CreatePipe(&pipeRead, &pipeWrite, &sa, 1 << 16)) return false;
    SetHandleInformation(pipeRead, HANDLE_FLAG_INHERIT, 0);

    // The child's environment: ours plus the marker, the crash folder and
    // input logging (as the Mac launcher sets PARTYBOARD_INPUT_LOG=1).
    SetEnvironmentVariableA(kChildMarker, "1");
    SetEnvironmentVariableA("PARTYBOARD_CRASH_DIR", dir.string().c_str());
    if (std::getenv("PARTYBOARD_INPUT_LOG") == nullptr) SetEnvironmentVariableA("PARTYBOARD_INPUT_LOG", "1");

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = pipeWrite;
    si.hStdError = pipeWrite;
    PROCESS_INFORMATION pi = {};
    std::wstring cmd = GetCommandLineW();
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi)) {
        logLine(console, "could not start the game as a logged child (error " + std::to_string(GetLastError())
                + "); running without game.log");
        SetEnvironmentVariableA(kChildMarker, nullptr);
        SetEnvironmentVariableA("PARTYBOARD_CRASH_DIR", nullptr);
        CloseHandle(pipeRead);
        CloseHandle(pipeWrite);
        return false;
    }
    CloseHandle(pipeWrite); // the child holds the only write end now
    CloseHandle(pi.hThread);
    logLine(console, "game started, process " + std::to_string(pi.dwProcessId));
    if (phone) partyboard::winphone::start(pi.dwProcessId);

    // Copy the game's output line by line with timestamps until it closes the pipe.
    char buf[8192];
    std::string out;
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(pipeRead, buf, sizeof(buf), &got, nullptr) || got == 0) break;
        std::lock_guard<std::mutex> lock(gLogMutex);
        writeAll(console, buf, got);
        out.clear();
        for (DWORD i = 0; i < got; i++) {
            if (gAtLineStart) {
                out += stamp();
                gAtLineStart = false;
            }
            out += buf[i];
            if (buf[i] == '\n') gAtLineStart = true;
        }
        writeAll(gLogFile, out.data(), static_cast<DWORD>(out.size()));
    }
    CloseHandle(pipeRead);

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    logLine(console, "game ended " + exitDescription(code));
    // Before the crash files and the iCloud copy, so bridge.log and input.log are complete.
    partyboard::winphone::stop();
    std::error_code ec2;
    for (const auto &entry : fs::directory_iterator(dir, ec2)) {
        const std::string file = entry.path().filename().string();
        if (file.rfind("crash-", 0) == 0) logLine(console, "crash file: " + file);
    }
    if (code != 0 && testProfile()) {
        logLine(console, "test profile: not copying this session to iCloud Drive");
        CloseHandle(gLogFile);
    } else if (code != 0) {
        logLine(console, "copying this session to iCloud Drive (if available)");
        CloseHandle(gLogFile); // so the copy includes the whole log
        gLogFile = INVALID_HANDLE_VALUE;
        shareCrashToICloud(INVALID_HANDLE_VALUE, dir);
    } else {
        CloseHandle(gLogFile);
    }
    *exitCode = static_cast<int>(code);
    return true;
}

#endif // _WIN32
