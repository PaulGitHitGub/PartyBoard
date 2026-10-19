// The phone controller on Windows: what "PartyBoard Phone Controller/
// PartyBoard-launcher.sh" does for it on the Mac, run by the session
// supervisor in partyboard.exe (session_log_win.cpp) around the game.
//
// Only when Settings > Input > Phone Controller is on (it creates
// phone-controller-enabled in the settings folder,
// %APPDATA%\MarioPartyRD\Party Board). Then, before the game starts:
//   * find Python 3.7+: python.org installs in the registry (PEP 514), then
//     py.exe, python.exe and python3.exe on PATH. The Microsoft Store's
//     "python" placeholders (app execution aliases in WindowsApps that open the
//     Store when Python is not installed) are recognised from their alias data
//     and never run; a real Store / Python install manager alias is used;
//   * keep (or make) the access code in phone-controller-code, like the Mac;
//   * give the game PARTYBOARD_TEST_INPUT=7700 (its input socket,
//     src/port/test_input.cpp) and PARTYBOARD_PHONE_CODE/PORT (the link and QR
//     code in Settings), or PARTYBOARD_PHONE_PROBLEM when it can't run.
// Once the game runs: start PhoneController\bridge.py from beside the exe with
// the Mac launcher's arguments (no console window), its output in bridge.log
// and every phone button in input.log in the session folder, and write
// logs\Phone Controller Link.txt. When the game ends the bridge notices within
// a second and stops (it watches the game's process); if it doesn't, it is
// ended. It runs in a job object, so it also ends if the supervisor is killed.
//
// Every step, and every reason for not starting, is a "[session] phone
// controller: ..." line in game.log. The access code is left out of game.log
// (crash reports get shared); it is in the link file and the settings folder.
//
// The first time Python listens on the network, Windows Firewall asks whether
// to allow it. The player must allow Private networks; this code never changes
// firewall settings.
#if defined(_WIN32)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#include <bcrypt.h>
#include <shlobj.h>

#include "port/phone_bridge_win.hpp"
#include "port/lan_address_win.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>

#ifndef IO_REPARSE_TAG_APPEXECLINK
#define IO_REPARSE_TAG_APPEXECLINK 0x8000001BL
#endif

namespace partyboard::winphone {
namespace {

namespace fs = std::filesystem;

constexpr int kHttpPort = 48765; // as on the Mac: clear of popular dev-server ports
constexpr int kGamePort = 7700;
constexpr DWORD kStopWaitMs = 3000; // the bridge checks on the game once a second

struct Python {
    std::wstring exe;
    std::wstring args;   // "-3" for py.exe
    std::string source;  // where it was found, for game.log
};

struct State {
    LogFn log = nullptr;
    fs::path sessionDir;
    fs::path logRoot;
    fs::path controllerDir;
    Python python;
    std::string token;
    bool ready = false;          // prepare() succeeded, start() may run
    HANDLE process = nullptr;    // the bridge (python.exe or py.exe)
    HANDLE job = nullptr;
    HANDLE game = nullptr;
    HANDLE stopEvent = nullptr;
    DWORD pid = 0;
    std::thread watcher;
    std::atomic<bool> exitedEarly {false};
};
State gState;

void say(const std::string &text)
{
    if (gState.log != nullptr) {
        gState.log("phone controller: " + text);
    }
}

std::string utf8(const std::wstring &text)
{
    if (text.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr,
        nullptr);
    std::string out(static_cast<size_t>(size > 0 ? size : 0), '\0');
    if (size > 0) {
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
    }
    return out;
}

std::string u8(const fs::path &path)
{
    return utf8(path.wstring());
}

std::wstring lower(std::wstring text)
{
    for (auto &c : text) {
        c = static_cast<wchar_t>(std::towlower(c));
    }
    return text;
}

fs::path exeDir()
{
    std::wstring buffer(32768, L'\0');
    const DWORD size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    buffer.resize(size);
    return fs::path(buffer).parent_path();
}

// The game's settings folder: SDL_GetPrefPath("MarioPartyRD", "Party Board"),
// or the test profile (calculate_config_path() in src/port/portmain.cpp).
fs::path configDir()
{
    if (const wchar_t *test = _wgetenv(L"PARTYBOARD_TEST_PROFILE"); test != nullptr && test[0] != L'\0') {
        return fs::path(test);
    }
    PWSTR appData = nullptr;
    fs::path dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData))) {
        dir = fs::path(appData) / L"MarioPartyRD" / L"Party Board";
    }
    CoTaskMemFree(appData);
    return dir;
}

bool fileExists(const std::wstring &path)
{
    // Not fs::exists: it follows app execution aliases and fails on them.
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

// ---- finding Python ------------------------------------------------------

enum class Kind {
    File,             // an ordinary file (or symbolic link): run it
    PythonAlias,      // a Store / Python install manager alias for real Python
    StorePlaceholder, // the App Installer's "python": opens the Microsoft Store
    OtherAlias,       // an alias for some other app, or one we can't read
};

// Files in %LOCALAPPDATA%\Microsoft\WindowsApps are app execution aliases:
// reparse points whose data names the Store package they start (version 3,
// then the package family, app id, target exe and app type as UTF-16
// strings). Reading it runs nothing.
Kind classify(const std::wstring &path, std::wstring &package)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
        return Kind::File;
    }
    HANDLE file = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return Kind::OtherAlias;
    }
    std::vector<BYTE> data(16 * 1024);
    DWORD got = 0;
    const BOOL ok = DeviceIoControl(file, FSCTL_GET_REPARSE_POINT, nullptr, 0, data.data(),
        static_cast<DWORD>(data.size()), &got, nullptr);
    CloseHandle(file);
    if (!ok || got < 8) {
        return Kind::OtherAlias;
    }
    DWORD tag = 0;
    std::memcpy(&tag, data.data(), sizeof(tag));
    if (tag == IO_REPARSE_TAG_SYMLINK) {
        return Kind::File;
    }
    if (tag != IO_REPARSE_TAG_APPEXECLINK || got < 16) {
        return Kind::OtherAlias;
    }
    // ReparseTag (4 bytes), ReparseDataLength (2), Reserved (2), version (4), strings.
    const auto *begin = reinterpret_cast<const wchar_t *>(data.data() + 12);
    const auto *end = reinterpret_cast<const wchar_t *>(data.data() + (got & ~1u));
    package.assign(begin, std::find(begin, end, L'\0'));
    const std::wstring name = lower(package);
    if (name.rfind(L"pythonsoftwarefoundation.", 0) == 0) {
        return Kind::PythonAlias;
    }
    if (name.rfind(L"microsoft.desktopappinstaller", 0) == 0) {
        return Kind::StorePlaceholder;
    }
    return Kind::OtherAlias;
}

std::wstring regString(HKEY key, const wchar_t *subKey, const wchar_t *value)
{
    DWORD bytes = 0;
    if (RegGetValueW(key, subKey, value, RRF_RT_REG_SZ, nullptr, nullptr, &bytes) != ERROR_SUCCESS ||
        bytes < sizeof(wchar_t)) {
        return {};
    }
    std::wstring text(bytes / sizeof(wchar_t), L'\0');
    if (RegGetValueW(key, subKey, value, RRF_RT_REG_SZ, nullptr, text.data(), &bytes) != ERROR_SUCCESS) {
        return {};
    }
    text.resize(wcsnlen(text.c_str(), text.size()));
    return text;
}

// python.org installs (and the Store's) register under PythonCore (PEP 514).
// Newest 3.x first; 3.7 is the oldest bridge.py runs on (asyncio.run).
void addRegistered(std::vector<Python> &out)
{
    struct Root {
        HKEY key;
        REGSAM view;
        const char *name;
    };
    const Root roots[] = {
        {HKEY_CURRENT_USER, 0, "this user"},
        {HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY, "all users"},
        {HKEY_LOCAL_MACHINE, KEY_WOW64_32KEY, "all users, 32-bit"},
    };
    struct Found {
        int minor;
        bool freeThreaded;
        Python python;
    };
    std::vector<Found> found;
    for (const Root &root : roots) {
        HKEY core = nullptr;
        if (RegOpenKeyExW(root.key, L"Software\\Python\\PythonCore", 0, KEY_READ | root.view, &core) != ERROR_SUCCESS) {
            continue;
        }
        for (DWORD i = 0;; ++i) {
            wchar_t tag[128];
            DWORD length = 128;
            const LSTATUS status = RegEnumKeyExW(core, i, tag, &length, nullptr, nullptr, nullptr, nullptr);
            if (status == ERROR_NO_MORE_ITEMS) {
                break;
            }
            int major = 0;
            int minor = 0;
            if (status != ERROR_SUCCESS || std::swscanf(tag, L"%d.%d", &major, &minor) != 2 || major != 3 || minor < 7) {
                continue;
            }
            const std::wstring installPath = std::wstring(tag) + L"\\InstallPath";
            std::wstring exe = regString(core, installPath.c_str(), L"ExecutablePath");
            if (exe.empty()) {
                const std::wstring dir = regString(core, installPath.c_str(), nullptr);
                if (!dir.empty()) {
                    exe = (fs::path(dir) / L"python.exe").wstring();
                }
            }
            if (!exe.empty()) {
                found.push_back({minor, std::wcschr(tag, L't') != nullptr,
                    {exe, L"", "Python " + utf8(tag) + " installed for " + root.name}});
            }
        }
        RegCloseKey(core);
    }
    std::stable_sort(found.begin(), found.end(), [](const Found &a, const Found &b) {
        return a.minor != b.minor ? a.minor > b.minor : (!a.freeThreaded && b.freeThreaded);
    });
    for (auto &f : found) {
        out.push_back(f.python);
    }
}

void addOnPath(const wchar_t *name, const wchar_t *args, std::vector<Python> &out)
{
    const DWORD size = GetEnvironmentVariableW(L"PATH", nullptr, 0);
    if (size == 0) {
        return;
    }
    std::wstring path(size, L'\0');
    path.resize(GetEnvironmentVariableW(L"PATH", path.data(), size));
    std::wstringstream entries(path);
    std::wstring dir;
    while (std::getline(entries, dir, L';')) {
        dir.erase(std::remove(dir.begin(), dir.end(), L'"'), dir.end());
        if (!dir.empty()) {
            out.push_back({(fs::path(dir) / name).wstring(), args, utf8(name) + " on PATH"});
        }
    }
}

// Picks the first usable Python; what was passed over goes into skipped.
bool findPython(std::string &skipped)
{
    std::vector<Python> candidates;
    if (const wchar_t *forced = _wgetenv(L"PARTYBOARD_PYTHON"); forced != nullptr && forced[0] != L'\0') {
        // For testing: a python.exe to use, or "none" to act as if there were none.
        if (std::wcscmp(forced, L"none") == 0) {
            skipped = "PARTYBOARD_PYTHON=none";
            return false;
        }
        candidates.push_back({forced, L"", "PARTYBOARD_PYTHON"});
    } else {
        addRegistered(candidates);
        if (const wchar_t *windir = _wgetenv(L"WINDIR")) {
            candidates.push_back({(fs::path(windir) / L"py.exe").wstring(), L"-3", "the Python launcher"});
        }
        if (const wchar_t *local = _wgetenv(L"LOCALAPPDATA")) {
            candidates.push_back({(fs::path(local) / L"Programs" / L"Python" / L"Launcher" / L"py.exe").wstring(), L"-3",
                "the Python launcher"});
        }
        addOnPath(L"py.exe", L"-3", candidates);
        addOnPath(L"python.exe", L"", candidates);
        addOnPath(L"python3.exe", L"", candidates);
    }
    std::vector<std::wstring> seen;
    std::vector<std::string> placeholders;
    std::vector<std::string> others;
    for (const Python &candidate : candidates) {
        const std::wstring key = lower(fs::path(candidate.exe).lexically_normal().wstring());
        if (std::find(seen.begin(), seen.end(), key) != seen.end() || !fileExists(candidate.exe)) {
            continue;
        }
        seen.push_back(key);
        std::wstring package;
        switch (classify(candidate.exe, package)) {
        case Kind::File:
        case Kind::PythonAlias:
            gState.python = candidate;
            if (!package.empty()) {
                gState.python.source += " (Store package " + utf8(package) + ")";
            }
            break;
        case Kind::StorePlaceholder:
            placeholders.push_back(utf8(candidate.exe));
            continue;
        case Kind::OtherAlias:
            others.push_back(utf8(candidate.exe) + (package.empty() ? "" : " (alias for " + utf8(package) + ")"));
            continue;
        }
        break;
    }
    auto join = [](const std::vector<std::string> &items) {
        std::string text;
        for (const auto &item : items) {
            text += (text.empty() ? "" : ", ") + item;
        }
        return text;
    };
    if (!placeholders.empty()) {
        skipped = "skipped the Microsoft Store placeholder" + std::string(placeholders.size() > 1 ? "s " : " ") +
                  join(placeholders) + ", which open the Store instead of running Python; not run";
    }
    if (!others.empty()) {
        skipped += (skipped.empty() ? "" : "; ") + std::string("skipped app aliases that aren't Python: ") + join(others);
    }
    return !gState.python.exe.empty();
}

// ---- access code -----------------------------------------------------------

// The code is kept, so a phone's bookmark keeps working; a new one has 10
// characters (the bridge also blocks a device after a few wrong codes).
bool loadToken(const fs::path &config)
{
    const fs::path file = config / L"phone-controller-code";
    std::string code;
    {
        std::ifstream in(file, std::ios::binary);
        std::stringstream text;
        text << in.rdbuf();
        code = text.str();
    }
    while (!code.empty() && std::isspace(static_cast<unsigned char>(code.back()))) {
        code.pop_back();
    }
    const bool valid = !code.empty() && code.size() <= 64 &&
                       std::all_of(code.begin(), code.end(), [](char c) {
                           return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
                       });
    if (valid) {
        gState.token = code;
        say("access code: the saved one in " + u8(file));
        return true;
    }
    static const char kAlphabet[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    code.clear();
    while (code.size() < 10) {
        unsigned char random[32];
        if (BCryptGenRandom(nullptr, random, sizeof(random), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
            say("NOT started: no random numbers for an access code");
            return false;
        }
        for (unsigned char byte : random) {
            if (byte < 252 && code.size() < 10) { // 252 = 7 * 36: every character equally likely
                code += kAlphabet[byte % 36];
            }
        }
    }
    std::error_code error;
    fs::create_directories(config, error);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << code;
    out.close();
    if (!out) {
        say("NOT started: could not save the access code in " + u8(file));
        return false;
    }
    gState.token = code;
    say("access code: made a new one, saved in " + u8(file));
    return true;
}

// ---- running the bridge ------------------------------------------------------

// One argument for a Windows command line (CommandLineToArgvW rules).
std::wstring quote(const std::wstring &arg)
{
    std::wstring out = L"\"";
    size_t backslashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        out.append(c == L'"' ? backslashes * 2 + 1 : backslashes, L'\\');
        backslashes = 0;
        out += c;
    }
    out.append(backslashes * 2, L'\\');
    return out + L"\"";
}

// The last lines of bridge.log, into game.log, when the bridge ended badly.
void logBridgeTail()
{
    std::ifstream in(gState.sessionDir / L"bridge.log", std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(text, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (!line.empty()) {
            lines.push_back(line);
        }
    }
    if (lines.empty()) {
        say("bridge.log is empty");
        return;
    }
    const size_t first = lines.size() > 8 ? lines.size() - 8 : 0;
    for (size_t i = first; i < lines.size(); ++i) {
        say("  bridge.log: " + lines[i]);
    }
}

// Waits for the bridge; reports it if it ends while the game still runs.
void watch()
{
    HANDLE handles[2] = {gState.process, gState.stopEvent};
    if (WaitForMultipleObjects(2, handles, FALSE, INFINITE) != WAIT_OBJECT_0) {
        return; // stop() takes over
    }
    if (gState.game != nullptr && WaitForSingleObject(gState.game, 0) == WAIT_OBJECT_0) {
        return; // the game ended first: the normal way, stop() reports it
    }
    gState.exitedEarly = true;
    DWORD code = 0;
    GetExitCodeProcess(gState.process, &code);
    say("the bridge stopped by itself while the game is running (exit code " + std::to_string(code) +
        "); phones can't connect until PartyBoard is reopened. Last lines of bridge.log:");
    logBridgeTail();
}

void writeLinkFile()
{
    const auto addresses = win_lan_addresses();
    std::vector<LanAddress> shown;
    for (const auto &address : addresses) {
        // Adapters with a gateway are the real networks; the rest only if
        // there are none (and at most three).
        if ((address.gateway || !addresses.front().gateway) && shown.size() < 3) {
            shown.push_back(address);
        }
    }
    std::string text;
    if (shown.empty()) {
        text = "This PC isn't on Wi-Fi (no network address). Connect it to Wi-Fi, then reopen PartyBoard.\r\n";
        say("no network address: phones can't reach this PC until it is on Wi-Fi");
    } else {
        text = "Open one of these in your phone's browser (the phone must be on the same Wi-Fi as this PC):\r\n\r\n";
        for (const auto &address : shown) {
            const std::string url = "http://" + address.ip + ":" + std::to_string(kHttpPort) + "/?k=";
            text += "  " + url + gState.token + "   (" + address.adapter + ")\r\n";
            const bool namedWifi = address.adapter.find("Wi-Fi") != std::string::npos;
            say("link for phones: " + url + "<code> (" + address.adapter + (address.wifi && !namedWifi ? ", Wi-Fi" : "") +
                (address.gateway ? "" : ", no gateway") + ")");
        }
    }
    text += "\r\nTip: on an iPhone, Share > Add to Home Screen gives a full-screen pad.\r\n"
            "\r\nThe first time, Windows Firewall asks whether Python may use the network:\r\n"
            "allow it on Private networks, or phones can't connect. In Windows Settings >\r\n"
            "Network & internet, this Wi-Fi's network profile type should be Private.\r\n";
    const fs::path file = gState.logRoot / L"Phone Controller Link.txt";
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << text;
    out.close();
    if (out) {
        say("link saved in " + u8(file) + " (with the access code, which game.log leaves out)");
    } else {
        say("could not write " + u8(file));
    }
}

} // namespace

bool prepare(const fs::path &sessionDir, const fs::path &logRoot, LogFn log)
{
    gState.log = log;
    gState.sessionDir = sessionDir;
    gState.logRoot = logRoot;
    const fs::path config = configDir();
    std::error_code error;
    if (config.empty() || !fs::exists(config / L"phone-controller-enabled", error)) {
        say("off (Settings > Input > Phone Controller turns it on)");
        return false;
    }
    // A second copy quits straight away (one PartyBoard at a time); the
    // running one has the phone controller.
    if (HANDLE other = OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\MarioPartyRD.PartyBoard.SingleInstance")) {
        CloseHandle(other);
        say("not started: PartyBoard is already running, and that copy has the phone controller");
        return false;
    }
    gState.controllerDir = exeDir() / L"PhoneController";
    if (!fs::exists(gState.controllerDir / L"bridge.py", error) ||
        !fs::exists(gState.controllerDir / L"controller.html", error)) {
        say("NOT started: bridge.py or controller.html is missing from " + u8(gState.controllerDir) +
            " (the PhoneController folder belongs next to partyboard.exe; reinstall PartyBoard)");
        SetEnvironmentVariableW(L"PARTYBOARD_PHONE_PROBLEM", L"files");
        return false;
    }
    std::string skipped;
    if (!findPython(skipped)) {
        say("NOT started: Python 3 is not installed" + (skipped.empty() ? "" : " (" + skipped + ")") +
            ". Install Python 3 from python.org and reopen PartyBoard; this game runs without the phone controller.");
        SetEnvironmentVariableW(L"PARTYBOARD_PHONE_PROBLEM", L"python");
        return false;
    }
    say("Python: " + utf8(gState.python.exe) + (gState.python.args.empty() ? "" : " " + utf8(gState.python.args)) +
        " (" + gState.python.source + ")" + (skipped.empty() ? "" : "; " + skipped));
    if (!loadToken(config)) {
        SetEnvironmentVariableW(L"PARTYBOARD_PHONE_PROBLEM", L"code");
        return false;
    }
    SetEnvironmentVariableW(L"PARTYBOARD_TEST_INPUT", std::to_wstring(kGamePort).c_str());
    SetEnvironmentVariableA("PARTYBOARD_PHONE_CODE", gState.token.c_str());
    SetEnvironmentVariableW(L"PARTYBOARD_PHONE_PORT", std::to_wstring(kHttpPort).c_str());
    say("the game's input socket is 127.0.0.1:" + std::to_string(kGamePort) + "; the phone page is port " +
        std::to_string(kHttpPort));
    gState.ready = true;
    return true;
}

void start(unsigned long gamePid)
{
    if (!gState.ready) {
        return;
    }
    gState.ready = false;
    const fs::path script = gState.controllerDir / L"bridge.py";
    const fs::path bridgeLog = gState.sessionDir / L"bridge.log";
    const fs::path inputLog = gState.sessionDir / L"input.log";

    // The bridge's output goes to bridge.log, like "> bridge.log 2>&1" on the Mac.
    SECURITY_ATTRIBUTES inherit = {sizeof(inherit), nullptr, TRUE};
    HANDLE out = CreateFileW(bridgeLog.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        &inherit, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE in = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr);
    if (out == INVALID_HANDLE_VALUE || in == INVALID_HANDLE_VALUE) {
        say("NOT started: could not create " + u8(bridgeLog) + " (error " + std::to_string(GetLastError()) + ")");
        if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
        if (in != INVALID_HANDLE_VALUE) CloseHandle(in);
        return;
    }

    // -u unbuffered (bridge.log is current), -I isolated (no PYTHONPATH or
    // user site-packages), -X utf8 (log files in UTF-8).
    const std::wstring pythonArgs = gState.python.args.empty() ? L"" : L" " + gState.python.args;
    const std::wstring before = quote(gState.python.exe) + pythonArgs + L" -u -I -X utf8 " + quote(script.wstring()) +
                                L" --token ";
    const std::wstring after = L" --http-port " + std::to_wstring(kHttpPort) + L" --game-port " +
                               std::to_wstring(kGamePort) + L" --trace " + quote(inputLog.wstring()) +
                               L" --game-pid " + std::to_wstring(gamePid) + L" --log-dir " +
                               quote(gState.sessionDir.wstring());
    std::wstring command = before + std::wstring(gState.token.begin(), gState.token.end()) + after;

    // Only these two handles reach the bridge; in particular not the game's
    // output pipe, which would keep game.log's reader waiting after the game.
    HANDLE handles[2] = {in, out};
    SIZE_T attributeSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeSize);
    std::vector<char> attributeBuffer(attributeSize);
    auto *attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeBuffer.data());
    const bool handleList = attributeSize > 0 && InitializeProcThreadAttributeList(attributes, 1, 0, &attributeSize) &&
                            UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles,
                                sizeof(handles), nullptr, nullptr);
    STARTUPINFOEXW startup = {};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = in;
    startup.StartupInfo.hStdOutput = out;
    startup.StartupInfo.hStdError = out;
    startup.lpAttributeList = handleList ? attributes : nullptr;
    // No console window; suspended until it is in the job.
    const DWORD flags = CREATE_NO_WINDOW | CREATE_SUSPENDED | (handleList ? EXTENDED_STARTUPINFO_PRESENT : 0);
    PROCESS_INFORMATION info = {};
    const std::wstring workDir = gState.controllerDir.wstring();
    const BOOL started = CreateProcessW(gState.python.exe.c_str(), command.data(), nullptr, nullptr, TRUE, flags, nullptr,
        workDir.c_str(), &startup.StartupInfo, &info);
    const DWORD startError = GetLastError();
    if (handleList) {
        DeleteProcThreadAttributeList(attributes);
    }
    CloseHandle(in);
    CloseHandle(out);
    SecureZeroMemory(command.data(), command.size() * sizeof(wchar_t));
    if (!started) {
        say("NOT started: could not run " + utf8(gState.python.exe) + " (error " + std::to_string(startError) + ")");
        return;
    }

    // In a job that ends it when its last handle closes: if the supervisor is
    // killed, the bridge (and python.exe under py.exe) goes too.
    gState.job = CreateJobObjectW(nullptr, nullptr);
    if (gState.job != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(gState.job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) ||
            !AssignProcessToJobObject(gState.job, info.hProcess)) {
            say("note: could not tie the bridge to PartyBoard's lifetime (error " + std::to_string(GetLastError()) +
                "); it still stops when the game ends");
            CloseHandle(gState.job);
            gState.job = nullptr;
        }
    }
    ResumeThread(info.hThread);
    CloseHandle(info.hThread);
    gState.process = info.hProcess;
    gState.pid = info.dwProcessId;
    gState.game = OpenProcess(SYNCHRONIZE, FALSE, gamePid);
    gState.stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    say("bridge started, process " + std::to_string(gState.pid) + ": " + utf8(before) + "<code>" + utf8(after));
    say("bridge output in bridge.log, every phone button in input.log (this log folder)");
    writeLinkFile();
    say("first time only: Windows Firewall asks whether Python may use the network; allow it on Private networks, "
        "or phones can't connect");
    if (gState.stopEvent != nullptr) {
        gState.watcher = std::thread(watch);
    }
}

void stop()
{
    if (gState.process == nullptr) {
        return;
    }
    if (gState.stopEvent != nullptr) {
        SetEvent(gState.stopEvent);
    }
    if (gState.watcher.joinable()) {
        gState.watcher.join();
    }
    DWORD code = 0;
    if (WaitForSingleObject(gState.process, kStopWaitMs) == WAIT_OBJECT_0) {
        GetExitCodeProcess(gState.process, &code);
        if (gState.exitedEarly) {
            say("the bridge had already stopped (exit code " + std::to_string(code) + ")");
        } else {
            say("bridge stopped with the game (exit code " + std::to_string(code) + ")");
            if (code != 0) {
                logBridgeTail();
            }
        }
    } else {
        if (gState.job != nullptr) {
            TerminateJobObject(gState.job, 1);
        } else {
            TerminateProcess(gState.process, 1);
        }
        WaitForSingleObject(gState.process, 2000);
        say("bridge still running " + std::to_string(kStopWaitMs / 1000) + " s after the game ended; ended it. "
            "Last lines of bridge.log:");
        logBridgeTail();
    }
    for (HANDLE *handle : {&gState.process, &gState.game, &gState.stopEvent, &gState.job}) {
        if (*handle != nullptr) {
            CloseHandle(*handle);
            *handle = nullptr;
        }
    }
}

} // namespace partyboard::winphone

#endif // _WIN32
