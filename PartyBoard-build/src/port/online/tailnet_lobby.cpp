#include "port/online/tailnet_lobby.hpp"

// winsock2.h has to come before anything that includes windows.h.
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <mbedtls/sha256.h>
#endif

#include <filesystem>

#include "port/config.hpp"
#include "port/main.h"
#include "port/netplay_state.hpp"
#include "port/netplay_transport.hpp"
#if defined(PARTYBOARD_CROSSPLAY_IDENTITY)
// Generated at build time by cmake/sim_fingerprint.cmake (cross-play builds only).
#include "partyboard_sim_fingerprint.h"
#endif

#include <SDL3/SDL_events.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

#if defined(__APPLE__)
#include <CommonCrypto/CommonDigest.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <netinet/in.h>
#include <poll.h>
#include <spawn.h>
#include <sys/socket.h>
#include <unistd.h>

extern char **environ;
#endif

#if defined(__APPLE__) || defined(_WIN32)
#define PARTYBOARD_TAILNET_SUPPORTED 1
#endif

namespace partyboard {
void registerSettings();   // port/settings.h, for the self-test
}

namespace partyboard::online::tailnet {
namespace fs = std::filesystem;

#if defined(PARTYBOARD_TAILNET_SUPPORTED)
namespace {

using Clock = std::chrono::steady_clock;

// Every lobby step leaves one short line in the session's game.log.
void lobby_log(const char *format, ...)
{
    char text[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    std::fprintf(stderr, "[lobby] %s\n", text);
    std::fflush(stderr);
}

// Enough of a fingerprint to tell two apart in a log.
std::string short_hash(const std::string &hex) { return hex.substr(0, 12); }

// Paths travel as UTF-8 text (the settings, the lobby). Windows would read a
// plain std::string path in the ANSI code page instead.
fs::path utf8_path(const std::string &text)
{
#if defined(_WIN32)
    return fs::path(std::u8string(text.begin(), text.end()));
#else
    return fs::path(text);
#endif
}

std::string utf8_text(const fs::path &path)
{
#if defined(_WIN32)
    const auto text = path.u8string();
    return std::string(text.begin(), text.end());
#else
    return path.string();
#endif
}

// ---- Sockets: BSD sockets on the Mac, Winsock on Windows ----------------

#if defined(_WIN32)
using Socket = SOCKET;
constexpr Socket kNoSocket = INVALID_SOCKET;

// Winsock is started once for the lobby (the netplay transport starts its own).
bool sockets_ready()
{
    static const bool started = [] {
        WSADATA data {};
        const int result = WSAStartup(MAKEWORD(2, 2), &data);
        if (result != 0) {
            lobby_log("Winsock could not start (error %d)", result);
        }
        return result == 0;
    }();
    return started;
}

void close_socket(Socket fd) { closesocket(fd); }
int socket_error() { return WSAGetLastError(); }
bool would_block()
{
    const int error = WSAGetLastError();
    return error == WSAEWOULDBLOCK || error == WSAEINTR;
}
bool connect_pending() { return WSAGetLastError() == WSAEWOULDBLOCK; }
long send_some(Socket fd, const char *data, std::size_t size)
{
    return ::send(fd, data, static_cast<int>(std::min<std::size_t>(size, 1u << 30)), 0);
}
long recv_some(Socket fd, char *data, std::size_t size) { return ::recv(fd, data, static_cast<int>(size), 0); }
#else
using Socket = int;
constexpr Socket kNoSocket = -1;

bool sockets_ready() { return true; }
void close_socket(Socket fd) { close(fd); }
int socket_error() { return errno; }
bool would_block() { return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR; }
bool connect_pending() { return errno == EINPROGRESS; }
long send_some(Socket fd, const char *data, std::size_t size) { return send(fd, data, size, 0); }
long recv_some(Socket fd, char *data, std::size_t size) { return recv(fd, data, size, 0); }
#endif

// Waits up to timeoutMs until the socket can be read or, with forWrite,
// written (a connect has finished).
bool wait_socket(Socket fd, bool forWrite, int timeoutMs)
{
#if defined(_WIN32)
    fd_set ready, failed;
    FD_ZERO(&ready);
    FD_ZERO(&failed);
    FD_SET(fd, &ready);
    FD_SET(fd, &failed);
    timeval timeout {timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    // A connect that failed is reported in the exception set, not as writable.
    return select(0, forWrite ? nullptr : &ready, forWrite ? &ready : nullptr, &failed, &timeout) > 0 && FD_ISSET(fd, &ready);
#else
    pollfd p {fd, static_cast<short>(forWrite ? POLLOUT : POLLIN), 0};
    return poll(&p, 1, timeoutMs) == 1;
#endif
}

void set_flags(Socket fd)
{
#if defined(_WIN32)
    u_long on = 1;
    ioctlsocket(fd, FIONBIO, &on);
    // Not inherited by the copy the lobby starts (that start names exactly the
    // handles it passes on; this is the second lock on the door).
    SetHandleInformation(reinterpret_cast<HANDLE>(fd), HANDLE_FLAG_INHERIT, 0);
#else
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    fcntl(fd, F_SETFD, fcntl(fd, F_GETFD, 0) | FD_CLOEXEC);
    const int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
}

bool make_address(const std::string &ip, std::uint16_t port, sockaddr_in &address)
{
    std::memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    return inet_pton(AF_INET, ip.c_str(), &address.sin_addr) == 1;
}

// A non-blocking socket connected within timeoutMs, or kNoSocket.
Socket connect_with_timeout(const std::string &ip, std::uint16_t port, int timeoutMs)
{
    sockaddr_in address;
    if (!sockets_ready() || !make_address(ip, port, address)) {
        return kNoSocket;
    }
    const Socket fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == kNoSocket) {
        return kNoSocket;
    }
    set_flags(fd);
    if (connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 && !connect_pending()) {
        close_socket(fd);
        return kNoSocket;
    }
    int error = 0;
    socklen_t length = sizeof(error);
    if (!wait_socket(fd, true, timeoutMs) || getsockopt(fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&error), &length) != 0
        || error != 0) {
        close_socket(fd);
        return kNoSocket;
    }
    return fd;
}

// One line from a socket, waiting up to timeoutMs. Scanner use only.
std::string read_line(Socket fd, int timeoutMs)
{
    std::string line;
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
    while (Clock::now() < deadline && line.size() < 512) {
        const int left = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count());
        if (!wait_socket(fd, false, std::max(left, 1))) {
            break;
        }
        char c;
        const long n = recv_some(fd, &c, 1);
        if (n <= 0) {
            break;
        }
        if (c == '\n') {
            return line;
        }
        line.push_back(c);
    }
    return {};
}

// ---- SHA-256: CommonCrypto on the Mac, Mbed TLS (already in the game for
// CubeShelf) on Windows. The same bytes give the same fingerprint on both.

class Sha256 {
public:
#if defined(_WIN32)
    Sha256()
    {
        mbedtls_sha256_init(&mContext);
        mbedtls_sha256_starts(&mContext, 0);
    }
    ~Sha256() { mbedtls_sha256_free(&mContext); }
    void update(const void *data, std::size_t size) { mbedtls_sha256_update(&mContext, static_cast<const unsigned char *>(data), size); }
    std::string hex()
    {
        unsigned char digest[32];
        mbedtls_sha256_finish(&mContext, digest);
        return to_hex(digest);
    }
#else
    Sha256() { CC_SHA256_Init(&mContext); }
    void update(const void *data, std::size_t size) { CC_SHA256_Update(&mContext, data, static_cast<CC_LONG>(size)); }
    std::string hex()
    {
        unsigned char digest[CC_SHA256_DIGEST_LENGTH];
        CC_SHA256_Final(digest, &mContext);
        return to_hex(digest);
    }
#endif
    Sha256(const Sha256 &) = delete;
    Sha256 &operator=(const Sha256 &) = delete;

private:
    static std::string to_hex(const unsigned char *digest)
    {
        static const char *digits = "0123456789abcdef";
        std::string out;
        for (int i = 0; i < 32; ++i) {
            out.push_back(digits[digest[i] >> 4]);
            out.push_back(digits[digest[i] & 15]);
        }
        return out;
    }
#if defined(_WIN32)
    mbedtls_sha256_context mContext;
#else
    CC_SHA256_CTX mContext;
#endif
};

bool hash_file(Sha256 &context, const fs::path &path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    std::vector<char> buffer(1 << 20);
    while (in) {
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto n = in.gcount();
        if (n > 0) {
            context.update(buffer.data(), static_cast<std::size_t>(n));
        }
    }
    return true;
}

fs::path executable_path()
{
#if defined(_WIN32)
    std::wstring buffer(32768, L'\0');
    const DWORD size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (size == 0 || size >= buffer.size()) {
        return {};
    }
    buffer.resize(size);
    const fs::path path(buffer);
#else
    char buffer[4096];
    uint32_t size = sizeof(buffer);
    if (_NSGetExecutablePath(buffer, &size) != 0) {
        return {};
    }
    const fs::path path(buffer);
#endif
    std::error_code error;
    const auto resolved = fs::canonical(path, error);
    return error ? path : resolved;
}

// The game's code: the executable and every module it loads. On the Mac those
// are the .dylibs in the app's Frameworks folder; on Windows, the .dlls beside
// partyboard.exe (dol.dll, every board and minigame, and the libraries they use).
std::vector<fs::path> game_files(const fs::path &exe)
{
    std::vector<fs::path> files {exe};
    std::error_code ec;
#if defined(_WIN32)
    for (const auto &entry : fs::directory_iterator(exe.parent_path(), ec)) {
        std::wstring extension = entry.path().extension().wstring();
        for (auto &c : extension) {
            if (c >= L'A' && c <= L'Z') {
                c = static_cast<wchar_t>(c - L'A' + L'a');
            }
        }
        if (extension == L".dll") {
            files.push_back(entry.path());
        }
    }
#else
    const auto frameworks = exe.parent_path().parent_path() / "Frameworks";
    for (const auto &entry : fs::directory_iterator(frameworks, ec)) {
        if (entry.path().extension() == ".dylib") {
            files.push_back(entry.path());
        }
    }
#endif
    std::sort(files.begin() + 1, files.end());
    return files;
}

// ---- The Tailscale command-line tool ------------------------------------

#if defined(_WIN32)
std::wstring widen(const std::string &utf8)
{
    if (utf8.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(std::max(size, 0)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), size);
    return out;
}

std::string narrow(const std::wstring &wide)
{
    return utf8_text(fs::path(wide));
}

bool is_file(const std::wstring &path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

// Installed with Tailscale in Program Files, or anywhere on PATH. Only full
// PATH folders are searched, never the game's or the current folder.
std::wstring tailscale_cli()
{
    std::vector<std::wstring> candidates;
    for (const wchar_t *variable : {L"ProgramW6432", L"ProgramFiles"}) {
        wchar_t folder[MAX_PATH];
        const DWORD size = GetEnvironmentVariableW(variable, folder, MAX_PATH);
        if (size > 0 && size < MAX_PATH) {
            candidates.push_back(std::wstring(folder) + L"\\Tailscale\\tailscale.exe");
        }
    }
    candidates.push_back(L"C:\\Program Files\\Tailscale\\tailscale.exe");
    for (const auto &candidate : candidates) {
        if (is_file(candidate)) {
            return candidate;
        }
    }
    std::wstring path(32768, L'\0');
    path.resize(GetEnvironmentVariableW(L"PATH", path.data(), static_cast<DWORD>(path.size())));
    std::wstringstream folders(path);
    std::wstring folder;
    while (std::getline(folders, folder, L';')) {
        if (folder.size() >= 2 && folder.front() == L'"' && folder.back() == L'"') {
            folder = folder.substr(1, folder.size() - 2);
        }
        if (fs::path(folder).is_absolute() && is_file(folder + L"\\tailscale.exe")) {
            return folder + L"\\tailscale.exe";
        }
    }
    return {};
}

// One argument quoted the way CommandLineToArgvW and the C runtime read it back.
std::wstring quote_argument(const std::wstring &argument)
{
    if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        return argument;
    }
    std::wstring out = L"\"";
    for (auto it = argument.begin();; ++it) {
        std::size_t backslashes = 0;
        while (it != argument.end() && *it == L'\\') {
            ++it;
            ++backslashes;
        }
        if (it == argument.end()) {
            out.append(backslashes * 2, L'\\');
            break;
        }
        if (*it == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
        } else {
            out.append(backslashes, L'\\');
        }
        out.push_back(*it);
    }
    out.push_back(L'"');
    return out;
}

// Starts a program that inherits exactly its three standard handles and
// `inherit`, nothing else: no lobby socket, no controller port. The handle list
// is the Windows form of posix_spawn's close-on-exec default.
bool start_process(const std::wstring &program, std::wstring commandLine, const wchar_t *environment, DWORD flags, HANDLE in,
    HANDLE out, HANDLE err, std::vector<HANDLE> inherit, PROCESS_INFORMATION &info)
{
    for (HANDLE h : {in, out, err}) {
        if (std::find(inherit.begin(), inherit.end(), h) == inherit.end()) {
            inherit.push_back(h);
        }
    }
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<unsigned char> storage(size);
    auto *attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &size)) {
        return false;
    }
    bool started = false;
    if (UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit.data(), inherit.size() * sizeof(HANDLE),
            nullptr, nullptr)) {
        STARTUPINFOEXW startup {};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = in;
        startup.StartupInfo.hStdOutput = out;
        startup.StartupInfo.hStdError = err;
        startup.lpAttributeList = attributes;
        started = CreateProcessW(program.c_str(), commandLine.data(), nullptr, nullptr, TRUE, flags | EXTENDED_STARTUPINFO_PRESENT,
                      const_cast<wchar_t *>(environment), nullptr, &startup.StartupInfo, &info)
            != FALSE;
    }
    const DWORD error = GetLastError();
    DeleteProcThreadAttributeList(attributes);
    SetLastError(error);
    return started;
}

HANDLE open_null(SECURITY_ATTRIBUTES &inheritable)
{
    return CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable, OPEN_EXISTING, 0, nullptr);
}

// What `program arguments` prints, without a console window, waiting at most
// timeoutMs (a stuck Tailscale service must not hang the lobby).
std::string run_program(const std::wstring &program, const std::wstring &arguments, int timeoutMs)
{
    SECURITY_ATTRIBUTES inheritable {sizeof(inheritable), nullptr, TRUE};
    HANDLE readEnd = nullptr;
    HANDLE writeEnd = nullptr;
    if (!CreatePipe(&readEnd, &writeEnd, &inheritable, 0)) {
        lobby_log("scan: couldn't make a pipe for tailscale (error %lu)", GetLastError());
        return {};
    }
    SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = open_null(inheritable);
    PROCESS_INFORMATION info {};
    const bool started = nul != INVALID_HANDLE_VALUE
        && start_process(program, quote_argument(program) + L" " + arguments, nullptr, CREATE_NO_WINDOW, nul, writeEnd, nul, {}, info);
    const DWORD startError = GetLastError();
    CloseHandle(writeEnd);   // the program holds the only write end now
    if (nul != INVALID_HANDLE_VALUE) {
        CloseHandle(nul);
    }
    std::string output;
    if (!started) {
        lobby_log("scan: couldn't run tailscale (error %lu)", startError);
    } else {
        CloseHandle(info.hThread);
        const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
        char buffer[4096];
        for (;;) {
            DWORD available = 0;
            if (!PeekNamedPipe(readEnd, nullptr, 0, nullptr, &available, nullptr)) {
                break;   // it finished and closed the pipe
            }
            if (available > 0) {
                DWORD got = 0;
                if (!ReadFile(readEnd, buffer, std::min<DWORD>(available, static_cast<DWORD>(sizeof(buffer))), &got, nullptr) || got == 0) {
                    break;
                }
                output.append(buffer, got);
                if (output.size() > (16u << 20)) {
                    break;
                }
                continue;
            }
            if (Clock::now() >= deadline) {
                lobby_log("scan: tailscale didn't answer within %d seconds; stopped it", timeoutMs / 1000);
                TerminateProcess(info.hProcess, 1);
                break;
            }
            Sleep(10);
        }
        WaitForSingleObject(info.hProcess, 2000);
        CloseHandle(info.hProcess);
    }
    CloseHandle(readEnd);
    return output;
}

// `tailscale status --json`; cliFound is false when Tailscale isn't installed.
std::string tailscale_status(bool &cliFound)
{
    const std::wstring cli = tailscale_cli();
    cliFound = !cli.empty();
    if (!cliFound) {
        return {};
    }
    lobby_log("scan: asking %s", narrow(cli).c_str());
    return run_program(cli, L"status --json", 15000);
}
#else
// The Tailscale command-line tool: inside the Mac app, or installed separately.
std::string tailscale_cli()
{
    for (const char *path : {"/Applications/Tailscale.app/Contents/MacOS/Tailscale", "/usr/local/bin/tailscale",
             "/opt/homebrew/bin/tailscale"}) {
        if (access(path, X_OK) == 0) {
            return path;
        }
    }
    return {};
}

std::string run_command(const std::string &command)
{
    std::string output;
    if (FILE *pipe = popen(command.c_str(), "r")) {
        char buffer[4096];
        size_t n;
        while ((n = fread(buffer, 1, sizeof(buffer), pipe)) > 0) {
            output.append(buffer, n);
        }
        pclose(pipe);
    }
    return output;
}

std::string tailscale_status(bool &cliFound)
{
    const std::string cli = tailscale_cli();
    cliFound = !cli.empty();
    if (!cliFound) {
        return {};
    }
    lobby_log("scan: asking %s", cli.c_str());
    return run_command("'" + cli + "' status --json 2>/dev/null");
}
#endif

std::string first_ipv4(const nlohmann::json &ips)
{
    if (ips.is_array()) {
        for (const auto &ip : ips) {
            if (ip.is_string() && ip.get<std::string>().find('.') != std::string::npos) {
                return ip.get<std::string>();
            }
        }
    }
    return {};
}

std::string clean_name(std::string name)
{
    for (char &c : name) {
        if (c == '\n' || c == '\r') {
            c = ' ';
        }
    }
    if (name.size() > 48) {
        name.resize(48);
    }
    return name.empty() ? "Player" : name;
}

// Limits on what the other side may send. Anything beyond them closes the
// connection: a lobby line is short, and a GameCube memory card is 16 MiB at most.
constexpr std::size_t kMaxLine = 4096;
constexpr std::size_t kMaxCardBytes = 64u << 20;

// The save's file name comes from the host and is written into this player's
// online profile, so only a bare "MemoryCard....raw" name is accepted: no
// folders, no "..", nothing that could point the write elsewhere.
bool valid_card_name(const std::string &name)
{
    if (name.size() < 14 || name.size() > 64 || name.rfind("MemoryCard", 0) != 0 || name.substr(name.size() - 4) != ".raw") {
        return false;
    }
    if (name.find("..") != std::string::npos) {
        return false;
    }
    return std::all_of(name.begin(), name.end(), [](unsigned char c) { return std::isalnum(c) || c == '.' || c == '_' || c == '-'; });
}

bool valid_ipv4(const std::string &ip)
{
    in_addr address {};
    return ip.size() <= 15 && inet_pton(AF_INET, ip.c_str(), &address) == 1;
}

bool valid_port(long port) { return port > 1024 && port < 65536; }

// "WORD rest of the line" -> {"WORD", "rest of the line"}
std::pair<std::string, std::string> split_first(const std::string &line)
{
    const auto space = line.find(' ');
    if (space == std::string::npos) {
        return {line, {}};
    }
    return {line.substr(0, space), line.substr(space + 1)};
}

// The game-affecting settings, which every seat takes from the host so that
// all copies play the same game. Each player keeps the rest (graphics, sound,
// controls), and the host's values are written into the online profile only,
// never into a player's own config.json. Unlock-all-minigames and the boot
// intro are also forced the same while online (port/settings.cpp).
constexpr const char *kSharedSettings[] = {"game.language", "game.unlockAllMinigames", "game.unlockBowsersGnarlyParty",
    "game.enableQuickTransform", "game.infiniteHearts", "game.speedrunMode", "backend.skipBootSequence", "backend.cardFileType"};

bool is_shared_setting(const std::string &key)
{
    return std::any_of(std::begin(kSharedSettings), std::end(kSharedSettings), [&](const char *name) { return key == name; });
}

// This copy's current values of the shared settings, as config.json writes them.
nlohmann::json shared_settings()
{
    nlohmann::json out = nlohmann::json::object();
    for (const char *key : kSharedSettings) {
        if (const auto *var = config::GetConfigVar(key)) {
            out[key] = var->getImpl()->dumpToJson(*var);
        }
    }
    return out;
}

// Settings from the host: only the shared names, each true/false or a small number.
bool valid_settings(const nlohmann::json &settings)
{
    if (!settings.is_object() || settings.size() > std::size(kSharedSettings)) {
        return false;
    }
    for (const auto &[key, value] : settings.items()) {
        const bool smallNumber = value.is_number_integer() && value.get<long long>() >= 0 && value.get<long long>() < 256;
        if (!is_shared_setting(key) || !(value.is_boolean() || smallNumber)) {
            return false;
        }
    }
    return true;
}

// Writes the host's shared settings into the online profile's copy of this
// player's config.json.
void apply_shared_settings(const fs::path &file, const nlohmann::json &settings)
{
    if (!settings.is_object() || settings.empty()) {
        lobby_log("launch: no game settings from the host; this player's own are used");
        return;
    }
    nlohmann::json config = nlohmann::json::object();
    if (std::ifstream in(file); in) {
        config = nlohmann::json::parse(in, nullptr, false);
        if (config.is_discarded() || !config.is_object()) {
            config = nlohmann::json::object();
        }
    }
    // What this player's online game would otherwise use: their saved value,
    // else the game's current (default) one.
    const auto mine = shared_settings();
    std::string changed;
    for (const auto &[key, value] : settings.items()) {
        const nlohmann::json before = config.contains(key) ? config[key] : mine.contains(key) ? mine[key] : nlohmann::json();
        if (before != value) {
            changed += " " + key + " " + before.dump() + "->" + value.dump();
        }
        config[key] = value;
    }
    std::ofstream out(file, std::ios::trunc);
    out << config.dump(4);
    lobby_log("launch: game settings from the host in %s:%s", utf8_text(file).c_str(),
        changed.empty() ? " all the same as this player's" : changed.c_str());
}

const char *state_name(Lobby::State state)
{
    switch (state) {
    case Lobby::State::Idle: return "idle";
    case Lobby::State::Hosting: return "hosting";
    case Lobby::State::Joining: return "joining";
    case Lobby::State::Waiting: return "waiting";
    case Lobby::State::Starting: return "starting";
    case Lobby::State::Launched: return "launched";
    case Lobby::State::Failed: return "failed";
    }
    return "?";
}

#if defined(_WIN32)
// The copy that plays the online game is partyboard.exe again, started the way
// a double-click starts it: it supervises itself into a new session-log folder
// (port/session_log_win.cpp), so the online game gets its own game.log and crash
// reports. The variables that mark this process as a supervisor's game and name
// its log folder are left out for that reason; the online settings are added.
//
// It inherits nothing but its standard handles and a handle to this process,
// and waits for this process to end before its game starts
// (PARTYBOARD_RELAUNCH_WAIT_HANDLE): the game port, the phone controller's port
// and the controllers are free by then. The Windows form of the Mac's
// "sleep 2; exec".
bool start_online_copy(const fs::path &exe, const std::vector<std::string> &args, const fs::path &profile, const std::string &discPath,
    const std::string &bindIp, DWORD &processId)
{
    const auto name_of = [](const std::wstring &entry) {
        std::wstring name = entry.substr(0, entry.find(L'=', 1));   // "=C:=C:\..." entries keep their leading '='
        for (auto &c : name) {
            if (c >= L'a' && c <= L'z') {
                c = static_cast<wchar_t>(c - L'a' + L'A');
            }
        }
        return name;
    };
    std::vector<std::wstring> environment;
    if (wchar_t *block = GetEnvironmentStringsW()) {
        for (const wchar_t *entry = block; *entry != L'\0'; entry += std::wcslen(entry) + 1) {
            environment.emplace_back(entry);
        }
        FreeEnvironmentStringsW(block);
    }
    const bool supervised = std::any_of(environment.begin(), environment.end(),
        [&](const std::wstring &entry) { return name_of(entry) == L"PARTYBOARD_SESSION_CHILD"; });
    std::vector<std::wstring> drop {L"PARTYBOARD_NETPLAY_TEST_PROFILE", L"PARTYBOARD_DISC_IMAGE", L"PARTYBOARD_NETPLAY_BIND",
        L"PARTYBOARD_RELAUNCH_WAIT_HANDLE"};
    if (supervised) {
        drop.push_back(L"PARTYBOARD_SESSION_CHILD");
        drop.push_back(L"PARTYBOARD_CRASH_DIR");
    }
    environment.erase(std::remove_if(environment.begin(), environment.end(),
                          [&](const std::wstring &entry) { return std::find(drop.begin(), drop.end(), name_of(entry)) != drop.end(); }),
        environment.end());

    HANDLE self = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &self,
            SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, TRUE, 0)) {
        self = nullptr;
        lobby_log("launch: couldn't pass a handle to this copy (error %lu); the new copy won't wait for it", GetLastError());
    }
    environment.push_back(L"PARTYBOARD_NETPLAY_TEST_PROFILE=" + profile.wstring());
    if (!discPath.empty()) {
        environment.push_back(L"PARTYBOARD_DISC_IMAGE=" + utf8_path(discPath).wstring());
    }
    if (valid_ipv4(bindIp)) {
        environment.push_back(L"PARTYBOARD_NETPLAY_BIND=" + widen(bindIp));
    }
    if (self != nullptr) {
        environment.push_back(L"PARTYBOARD_RELAUNCH_WAIT_HANDLE=" + std::to_wstring(reinterpret_cast<std::uintptr_t>(self)));
    }
    // CreateProcess wants the block sorted by name, ignoring case.
    std::stable_sort(environment.begin(), environment.end(),
        [&](const std::wstring &a, const std::wstring &b) { return name_of(a) < name_of(b); });
    std::wstring block;
    for (const auto &entry : environment) {
        block += entry;
        block.push_back(L'\0');
    }
    block.push_back(L'\0');

    SECURITY_ATTRIBUTES inheritable {sizeof(inheritable), nullptr, TRUE};
    HANDLE nul = open_null(inheritable);
    // Its output goes to this game's console window, if it has one.
    HANDLE console = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable,
        OPEN_EXISTING, 0, nullptr);
    HANDLE out = console != INVALID_HANDLE_VALUE ? console : nul;
    std::wstring command = quote_argument(exe.wstring());
    for (const auto &a : args) {
        command += L" " + quote_argument(widen(a));
    }
    std::vector<HANDLE> inherit;
    if (self != nullptr) {
        inherit.push_back(self);
    }
    PROCESS_INFORMATION info {};
    const bool started = nul != INVALID_HANDLE_VALUE
        && start_process(exe.wstring(), command, block.c_str(), CREATE_UNICODE_ENVIRONMENT, nul, out, out, inherit, info);
    const DWORD error = GetLastError();
    if (console != INVALID_HANDLE_VALUE) {
        CloseHandle(console);
    }
    if (nul != INVALID_HANDLE_VALUE) {
        CloseHandle(nul);
    }
    if (self != nullptr) {
        CloseHandle(self);   // the new copy has its own
    }
    if (!started) {
        lobby_log("launch: couldn't start %s (error %lu)", narrow(exe.wstring()).c_str(), error);
        return false;
    }
    processId = info.dwProcessId;
    CloseHandle(info.hThread);
    CloseHandle(info.hProcess);
    lobby_log("launch: %s new session-log folder for the online game", supervised ? "the new copy makes a" : "no supervisor here, so no");
    return true;
}
#endif

} // namespace

std::uint16_t local_game_port()
{
    if (const char *value = std::getenv("PARTYBOARD_ONLINE_GAME_PORT"); value && *value) {
        const long port = std::strtol(value, nullptr, 10);
        if (port > 1024 && port < 65536) {
            return static_cast<std::uint16_t>(port);
        }
    }
    return kGamePort;
}

// ---- Scanner ------------------------------------------------------------

Scanner::~Scanner()
{
    if (mThread.joinable()) {
        mThread.join();
    }
}

void Scanner::start()
{
    {
        std::lock_guard lock(mMutex);
        if (mBusy) {
            return;
        }
        mBusy = true;
    }
    if (mThread.joinable()) {
        mThread.join();
    }
    mThread = std::thread([this] {
        const auto began = Clock::now();
        std::string selfIp, error;
        std::vector<HostOffer> offers;
        bool cliFound = false;
        const std::string out = tailscale_status(cliFound);
        nlohmann::json status;
        if (!cliFound) {
            error = std::string("Tailscale isn't installed on this ") + kComputer + ".";
            lobby_log("scan: Tailscale's command-line tool wasn't found");
        } else {
            status = nlohmann::json::parse(out, nullptr, false);
            if (status.is_discarded() || !status.contains("Self")) {
                error = "Tailscale isn't running. Open Tailscale and connect.";
                lobby_log("scan: tailscale status gave no usable answer (%zu bytes)", out.size());
            } else if (status.value("BackendState", "") != "Running") {
                error = "Tailscale isn't connected. Open Tailscale and sign in.";
                lobby_log("scan: Tailscale is %s, not Running", status.value("BackendState", "?").c_str());
            } else {
                selfIp = first_ipv4(status["Self"].value("TailscaleIPs", nlohmann::json::array()));
                if (selfIp.empty()) {
                    lobby_log("scan: Tailscale is running but this %s has no IPv4 address in it", kComputer);
                }
            }
        }
        if (error.empty()) {
            // Every online computer in the network, this one included (two
            // copies on one computer is how this gets tested), asked whether it
            // hosts a lobby.
            std::vector<std::pair<std::string, std::string>> devices;
            devices.emplace_back(selfIp, status["Self"].value("HostName", std::string("this ") + kComputer));
            if (status.contains("Peer") && status["Peer"].is_object()) {
                for (const auto &[key, peer] : status["Peer"].items()) {
                    if (peer.value("Online", false)) {
                        const auto ip = first_ipv4(peer.value("TailscaleIPs", nlohmann::json::array()));
                        if (!ip.empty()) {
                            devices.emplace_back(ip, peer.value("HostName", ip));
                        }
                    }
                }
            }
            lobby_log("scan: this %s is %s (%s); %zu other device(s) online", kComputer, selfIp.c_str(), devices.front().second.c_str(),
                devices.size() - 1);
            for (const auto &[ip, device] : devices) {
                const Socket fd = connect_with_timeout(ip, kLobbyPort, 800);
                if (fd == kNoSocket) {
                    lobby_log("scan: %s (%s): no lobby", device.c_str(), ip.c_str());
                    continue;
                }
                send_some(fd, "WHO\n", 4);
                const auto [word, rest] = split_first(read_line(fd, 800));
                // A newer host says the party size on a second line; an older one closes.
                const auto [partyWord, partyRest] = word == "HOSTING" ? split_first(read_line(fd, 300))
                                                                      : std::pair<std::string, std::string> {};
                close_socket(fd);
                if (word != "HOSTING") {
                    lobby_log("scan: %s (%s): lobby port open but no game offered", device.c_str(), ip.c_str());
                    continue;
                }
                HostOffer offer;
                offer.ip = ip;
                offer.device = device;
                std::istringstream fields(rest);
                fields >> offer.players >> offer.joined;
                std::getline(fields >> std::ws, offer.name);
                offer.party = partyWord == "PARTY" && std::atoi(partyRest.c_str()) == 8 ? 8 : 4;
                lobby_log("scan: %s (%s): %s hosts a game, %d of %d players, %d-player party", device.c_str(), ip.c_str(),
                    offer.name.c_str(), offer.joined, offer.players, offer.party);
                if (offer.players >= 2) {
                    offers.push_back(offer);
                }
            }
        }
        lobby_log("scan: done in %lld ms: %zu game(s)%s%s", static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - began).count()),
            offers.size(), error.empty() ? "" : "; problem: ", error.c_str());
        std::lock_guard lock(mMutex);
        mSelfIp = selfIp;
        mError = error;
        mOffers = offers;
        mBusy = false;
    });
}

bool Scanner::busy() const
{
    std::lock_guard lock(mMutex);
    return mBusy;
}

std::string Scanner::selfIp() const
{
    std::lock_guard lock(mMutex);
    return mSelfIp;
}

std::string Scanner::error() const
{
    std::lock_guard lock(mMutex);
    return mError;
}

std::vector<HostOffer> Scanner::offers() const
{
    std::lock_guard lock(mMutex);
    return mOffers;
}

// ---- Identity -----------------------------------------------------------

Identity::~Identity()
{
    if (mThread.joinable()) {
        mThread.join();
    }
}

void Identity::start(const std::string &discPath)
{
    {
        std::lock_guard lock(mMutex);
        if (mThread.joinable() && !mDone) {
            return;   // still working
        }
        if (mDone && mDiscPath == discPath && mError.empty()) {
            return;   // already fingerprinted this disc
        }
        mDone = false;
        mError.clear();
        mDiscPath = discPath;
    }
    if (mThread.joinable()) {
        mThread.join();
    }
    mThread = std::thread([this, discPath] {
        const auto began = Clock::now();
        std::string build, disc, error;
        const auto exe = executable_path();
        if (exe.empty()) {
            error = "Couldn't find this copy of PartyBoard.";
            lobby_log("identity: couldn't find this program's own file");
        } else {
            Sha256 context;
            const auto files = game_files(exe);
            for (const auto &file : files) {
                const auto name = utf8_text(file.filename());
                context.update(name.data(), name.size());
                hash_file(context, file);
            }
            build = context.hex();
            lobby_log("identity: build %s from %zu files in %s", short_hash(build).c_str(), files.size(), utf8_text(exe.parent_path()).c_str());
        }
        const std::string simulation = simulation_fingerprint();
        if (simulation.empty()) {
            lobby_log("identity: regular build (no cross-play fingerprint): plays only byte-identical copies");
        } else {
            lobby_log("identity: cross-play simulation %s (%s)", short_hash(simulation).c_str(), simulation_fingerprint_summary().c_str());
        }
        if (error.empty()) {
            Sha256 context;
            if (discPath.empty() || !hash_file(context, utf8_path(discPath))) {
                error = "Choose your disc image first (the same file as your friends).";
                lobby_log("identity: no readable disc image (%s)", discPath.empty() ? "none chosen" : discPath.c_str());
            } else {
                disc = context.hex();
                lobby_log("identity: disc %s (%s), %lld ms in all", short_hash(disc).c_str(), discPath.c_str(),
                    static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - began).count()));
            }
        }
        std::lock_guard lock(mMutex);
        mBuild = build;
        mSimulation = simulation;
        mDisc = disc;
        mError = error;
        mDone = true;
    });
}

bool Identity::ready() const
{
    std::lock_guard lock(mMutex);
    return mDone && mError.empty();
}

bool Identity::failed() const
{
    std::lock_guard lock(mMutex);
    return mDone && !mError.empty();
}

std::string Identity::build() const
{
    std::lock_guard lock(mMutex);
    return mBuild;
}

std::string Identity::disc() const
{
    std::lock_guard lock(mMutex);
    return mDisc;
}

std::string Identity::discPath() const
{
    std::lock_guard lock(mMutex);
    return mDiscPath;
}

std::string Identity::error() const
{
    std::lock_guard lock(mMutex);
    return mError;
}

std::string Identity::simulation() const
{
    std::lock_guard lock(mMutex);
    return mSimulation;
}

// ---- Cross-play identity --------------------------------------------------

std::string simulation_fingerprint_summary()
{
#if defined(PARTYBOARD_CROSSPLAY_IDENTITY)
    char text[512];
    std::snprintf(text, sizeof(text), "sources %.12s from %d files, revision %.12s, %s, protocol %u, state hash %u",
        PARTYBOARD_SIM_SOURCES_SHA256, PARTYBOARD_SIM_SOURCE_FILES, PARTYBOARD_SIM_REVISION, PARTYBOARD_SIM_OPTIONS,
        static_cast<unsigned>(::partyboard::netplay::kProtocolVersion), static_cast<unsigned>(::partyboard::netplay::kStateHashVersion));
    return text;
#else
    return "regular build";
#endif
}

std::string simulation_fingerprint()
{
#if defined(PARTYBOARD_CROSSPLAY_IDENTITY)
    // Everything that decides what the game computes, apart from the disc
    // (checked on its own) and the compiler (whose maths the cross-play
    // options pin down). Platform-only code is part of the same source tree on
    // both computers, so it is hashed the same way on both.
    const std::string text = std::string("partyboard-simulation-1")
        + "|sources=" + PARTYBOARD_SIM_SOURCES_SHA256
        + "|files=" + std::to_string(PARTYBOARD_SIM_SOURCE_FILES)
        + "|revision=" + PARTYBOARD_SIM_REVISION
        + "|options=" + PARTYBOARD_SIM_OPTIONS
        + "|protocol=" + std::to_string(::partyboard::netplay::kProtocolVersion)
        + "|state=" + std::to_string(::partyboard::netplay::kStateHashVersion);
    Sha256 context;
    context.update(text.data(), text.size());
    return context.hex();
#else
    return {};
#endif
}

// ---- Lobby --------------------------------------------------------------
//
// Line protocol over TCP, host authoritative:
//   guest -> host   WHO                       (lobby browser; host answers and closes)
//                   LOBBYVER <n>              (before HELLO: lobby protocol, kLobbyProtocol)
//                   PEOPLE <n> <Mac|PC>       (before HELLO: people on this computer)
//                   READYSEAT <k> <controller 0|1> <ready 0|1> <device>   (this computer's k-th person)
//                   PING / PONG               (both ways, every 2 s: the ready check's ping)
//                   HELLO <build> <disc> <gamePort> <name>
//                   READY                     (memory card received, about to launch)
//   host -> guest   HOSTING <players> <joined> <name>
//                   PARTY <4|8>               (after HOSTING; older scanners read one line)
//                   WELCOME <seat> <players> [<party> [<seats>]]
//                   REJECT <reason>
//                   ROSTER <players> [<party>] / MEMBER <seat> <ip> <gamePort> <seats> <Mac|PC> <name> / END
//                   (several people on one computer: a member owns seats seat..seat+seats-1)
//                   READYSTATE <seat> <controller 0|1> <ready 0|1> <device>   (every taken seat, on change)
//                   SETTINGS <json>           (the host's game-affecting settings)
//                   CARD <filename> <bytes>\n<bytes>    (the host's memory card)
//                   GO

// Several people on one computer: the first seat of the first run of `people`
// free consecutive seats, or -1.
int first_free_block(const std::vector<Member> &members, int players, int people)
{
    for (int first = 0; first + people <= players; ++first) {
        const bool free = std::none_of(members.begin(), members.end(), [&](const Member &m) {
            return m.seat < first + people && first < m.seat + m.seats;
        });
        if (free) return first;
    }
    return -1;
}

std::string fmt_people_left(int left, int people, const std::string &computer)
{
    return "That game has only " + std::to_string(left) + (left == 1 ? " place" : " places") + " left; this "
        + (computer.empty() ? std::string("computer") : computer) + " asked for " + std::to_string(people) + ".";
}

struct Lobby::Impl {
    struct Conn {
        Socket fd = kNoSocket;
        std::string ip;
        std::string in;
        std::string out;
        int seat = -1;
        bool closing = false;
        bool ready = false;
        int people = 1;            // PEOPLE: how many play on that computer
        std::string computer;      // PEOPLE: "Mac" or "PC"
        int lobbyVersion = 1;      // LOBBYVER (none = 1, before the ready check)
        // Ready check: one PING outstanding at a time, every 2 s; round trip in ms.
        int rttMs = -1;
        bool pingOut = false;
        Clock::time_point pingSent {};
        Clock::time_point nextPing {};
    };

    // PING/PONG keepalive with round-trip time (shown in the ready check).
    static void servicePing(Conn &c)
    {
        const auto now = Clock::now();
        if (c.pingOut && now - c.pingSent > std::chrono::seconds(6)) {
            c.pingOut = false;
            c.rttMs = -1;
        }
        if (!c.pingOut && now >= c.nextPing) {
            c.out += "PING\n";
            c.pingOut = true;
            c.pingSent = now;
            c.nextPing = now + std::chrono::seconds(2);
        }
    }
    static void gotPong(Conn &c)
    {
        if (c.pingOut) {
            c.rttMs = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - c.pingSent).count());
            c.pingOut = false;
        }
    }

    Socket listenFd = kNoSocket;
    std::vector<Conn> guests;   // host side
    Conn hostLink;              // guest side
    std::string build, disc, discPath, name, selfIp;
    std::string simulation;   // cross-play fingerprint, empty for a regular build
    int seat = 0;
    int people = 1;   // people on this computer: seats seat .. seat + people - 1
    std::uint16_t gamePort = kGamePort;

    // The host's game settings: sent (host) or received (guest).
    nlohmann::json settings;

    // Guest: the host's memory card, as it arrives.
    std::string cardName;
    std::size_t cardRemaining = 0;
    bool receivingCard = false;
    std::vector<char> card;
    bool goReceived = false;
    Clock::time_point startedAt {};

    ~Impl() { closeAll(); }

    void closeAll()
    {
        if (listenFd != kNoSocket) {
            close_socket(listenFd);
            listenFd = kNoSocket;
        }
        for (auto &g : guests) {
            if (g.fd != kNoSocket) {
                close_socket(g.fd);
            }
        }
        guests.clear();
        if (hostLink.fd != kNoSocket) {
            close_socket(hostLink.fd);
        }
        hostLink = {};
    }

    // Reads what is available; false when the peer closed or sent more than
    // `limit` bytes that have not been consumed yet.
    static bool pump(Conn &c, std::size_t limit)
    {
        char buffer[65536];
        for (;;) {
            const long n = recv_some(c.fd, buffer, sizeof(buffer));
            if (n > 0) {
                c.in.append(buffer, static_cast<size_t>(n));
                if (c.in.size() > limit) {
                    return false;
                }
                continue;
            }
            if (n == 0) {
                return false;
            }
            return would_block();
        }
    }

    static void flush(Conn &c)
    {
        while (!c.out.empty()) {
            const long n = send_some(c.fd, c.out.data(), c.out.size());
            if (n <= 0) {
                return;
            }
            c.out.erase(0, static_cast<size_t>(n));
        }
    }

    static bool takeLine(std::string &in, std::string &line)
    {
        const auto end = in.find('\n');
        if (end == std::string::npos) {
            return false;
        }
        line = in.substr(0, end);
        in.erase(0, end + 1);
        return true;
    }
};

Lobby::Lobby() : mImpl(std::make_unique<Impl>()) {}

Lobby::~Lobby() { leave(); }

void Lobby::fail(std::string message)
{
    mImpl->closeAll();
    lobby_log("problem (was %s): %s", state_name(mState), message.c_str());
    mState = State::Failed;
    mMessage = std::move(message);
}

int Lobby::localSeat() const
{
    return mIsHost ? 0 : mImpl->seat;
}

int Lobby::localPeople() const
{
    return mImpl->people;
}

int Lobby::readyCount() const
{
    int count = 0;
    for (const auto &m : mMembers) {
        for (int s = m.seat; s < m.seat + m.seats && s < kMaxSeats; ++s) {
            count += mReady[static_cast<std::size_t>(s)].ready ? 1 : 0;
        }
    }
    return count;
}

bool Lobby::everyoneReady() const
{
    return taken() == mPlayers && readyCount() == mPlayers;
}

std::string Lobby::waitingFor() const
{
    std::string text;
    for (const auto &m : mMembers) {
        for (int s = m.seat; s < m.seat + m.seats && s < kMaxSeats; ++s) {
            const auto &r = mReady[static_cast<std::size_t>(s)];
            if (r.ready) {
                continue;
            }
            if (!text.empty()) {
                text += ", ";
            }
            text += (m.computer.empty() ? m.name : m.computer) + " - Player " + std::to_string(s + 1)
                + (r.controller ? " (not ready)" : " (no controller)");
        }
    }
    if (taken() < mPlayers) {
        if (!text.empty()) {
            text += ", ";
        }
        const int missing = mPlayers - taken();
        text += std::to_string(missing) + (missing == 1 ? " player to join" : " players to join");
    }
    return text;
}

int Lobby::pingMs(int seat) const
{
    const auto &impl = *mImpl;
    if (!mIsHost) {
        return impl.hostLink.rttMs;
    }
    for (const auto &g : impl.guests) {
        if (g.seat >= 0 && seat >= g.seat && seat < g.seat + g.people) {
            return g.rttMs;
        }
    }
    return -1;
}

void Lobby::setLocalSeat(int k, bool controller, bool ready, const std::string &device)
{
    auto &impl = *mImpl;
    if (k < 0 || k >= impl.people || (mState != State::Waiting && mState != State::Starting)) {
        return;
    }
    const int seat = localSeat() + k;
    if (seat >= kMaxSeats) {
        return;
    }
    auto &r = mReady[static_cast<std::size_t>(seat)];
    ready = ready && controller;
    if (r.controller == controller && r.ready == ready && r.device == device) {
        return;
    }
    if (r.ready != ready || r.controller != controller) {
        lobby_log("ready: player %d %s (%s, this %s)", seat + 1, ready ? "ready" : controller ? "not ready" : "not ready: no controller",
            device.empty() ? "nothing on its controller port" : device.c_str(), kComputer);
    }
    r.controller = controller;
    r.ready = ready;
    r.device = device;
    if (mIsHost) {
        mReadyDirty = true;
    } else {
        impl.hostLink.out += "READYSEAT " + std::to_string(k) + " " + (controller ? "1" : "0") + " " + (ready ? "1" : "0") + " "
            + (device.empty() ? std::string("-") : device) + "\n";
        Impl::flush(impl.hostLink);
    }
}

int Lobby::taken() const
{
    int seats = 0;
    for (const auto &m : mMembers) seats += m.seats;
    return seats;
}

bool Lobby::host(const std::string &selfIp, int players, const std::string &name, const Identity &identity, int party,
    int people)
{
    leave();
    auto &impl = *mImpl;
    sockaddr_in address;
    if (!sockets_ready() || selfIp.empty() || !make_address(selfIp, kLobbyPort, address)) {
        fail(std::string("This ") + kComputer + " isn't connected to Tailscale.");
        return false;
    }
    // Listen on the Tailscale address only: invisible to the home network and
    // the internet.
    impl.listenFd = socket(AF_INET, SOCK_STREAM, 0);
#if !defined(_WIN32)
    // (Windows' SO_REUSEADDR would instead let a second program take the same
    // port, and Windows frees a closed lobby's port without it.)
    const int on = 1;
    setsockopt(impl.listenFd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
#endif
    if (impl.listenFd == kNoSocket || bind(impl.listenFd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0
        || listen(impl.listenFd, 8) != 0) {
        lobby_log("host: couldn't listen on %s:%u (error %d)", selfIp.c_str(), static_cast<unsigned>(kLobbyPort), socket_error());
        fail(std::string("Couldn't open the lobby. Is another PartyBoard already hosting on this ") + kComputer + "?");
        return false;
    }
    set_flags(impl.listenFd);
    impl.build = identity.build();
    impl.simulation = identity.simulation();
    impl.disc = identity.disc();
    impl.discPath = identity.discPath();
    impl.name = clean_name(name);
    impl.selfIp = selfIp;
    impl.seat = 0;
    impl.gamePort = local_game_port();
    mIsHost = true;
    mParty = party == 8 ? 8 : 4;
    mPlayers = std::clamp(players, 2, mParty == 8 ? kMaxSeats : kNormalPartySeats);
    // At least one seat is left for another computer.
    impl.people = std::clamp(people, 1, mPlayers - 1);
    mMembers = {Member {0, impl.name, selfIp, impl.gamePort, impl.people, kComputer}};
    mReady = {};
    mReadyDirty = true;
    mLastReadyLogged = -1;
    mLastWaitingLogged.clear();
    mState = State::Waiting;
    mMessage = "Waiting for players to join.";
    lobby_log("host: %d %s on this %s (%s), %d from other computers", impl.people, impl.people == 1 ? "person" : "people", kComputer,
        impl.people == 1 ? "player 1" : ("players 1-" + std::to_string(impl.people)).c_str(), mPlayers - impl.people);
    lobby_log("host: %d-player party, %d people (%s)", mParty, mPlayers,
        mParty == 8 && mPlayers < 8 ? "the other seats are computer players" : "every seat a person");
    lobby_log("host: %s hosts a %d-player lobby on %s:%u, game port %u, build %s, disc %s, cross-play %s", impl.name.c_str(), mPlayers,
        selfIp.c_str(), static_cast<unsigned>(kLobbyPort), static_cast<unsigned>(impl.gamePort), short_hash(impl.build).c_str(),
        short_hash(impl.disc).c_str(), impl.simulation.empty() ? "no (byte-identical builds only)" : short_hash(impl.simulation).c_str());
    return true;
}

bool Lobby::join(const std::string &hostIp, const std::string &name, const Identity &identity, int people)
{
    leave();
    auto &impl = *mImpl;
    lobby_log("join: connecting to %s:%u", hostIp.c_str(), static_cast<unsigned>(kLobbyPort));
    impl.hostLink.fd = connect_with_timeout(hostIp, kLobbyPort, 1500);
    if (impl.hostLink.fd == kNoSocket) {
        fail("Couldn't reach that game. Is it still hosting?");
        return false;
    }
    impl.hostLink.ip = hostIp;
    impl.build = identity.build();
    impl.simulation = identity.simulation();
    impl.disc = identity.disc();
    impl.discPath = identity.discPath();
    impl.name = clean_name(name);
    impl.gamePort = local_game_port();
    impl.people = std::clamp(people, 1, kMaxSeats - 1);
    mReady = {};
    mLastReadyLogged = -1;
    mLastWaitingLogged.clear();
    impl.hostLink.out = "LOBBYVER " + std::to_string(kLobbyProtocol) + "\n";
    impl.hostLink.out += "PEOPLE " + std::to_string(impl.people) + " " + kComputer + "\n";
    impl.hostLink.out += "HELLO " + hello_build_field(impl.build, impl.simulation) + " " + impl.disc + " " + std::to_string(impl.gamePort) + " " + impl.name + "\n";
    Impl::flush(impl.hostLink);
    mIsHost = false;
    mState = State::Joining;
    mMessage = "Joining...";
    lobby_log("join: %d %s on this %s", impl.people, impl.people == 1 ? "person" : "people", kComputer);
    lobby_log("join: connected; %s asks to join (game port %u, build %s, disc %s, cross-play %s)", impl.name.c_str(),
        static_cast<unsigned>(impl.gamePort), short_hash(impl.build).c_str(), short_hash(impl.disc).c_str(),
        impl.simulation.empty() ? "no" : short_hash(impl.simulation).c_str());
    return true;
}

void Lobby::leave()
{
    auto &impl = *mImpl;
    if (mState != State::Idle && mState != State::Launched) {
        lobby_log("left the lobby (%s, was %s)", mIsHost ? "host" : "guest", state_name(mState));
    }
    if (impl.hostLink.fd != kNoSocket) {
        send_some(impl.hostLink.fd, "BYE\n", 4);
    }
    impl.closeAll();
    impl.settings = nlohmann::json();
    impl.receivingCard = false;
    impl.card.clear();
    impl.goReceived = false;
    mState = State::Idle;
    mMembers.clear();
    mReady = {};
    mPlayers = 0;
    mParty = 4;
    mMessage.clear();
}

// One line whenever the count or the missing list changes (both sides).
void Lobby::logReadySummary()
{
    if (mState != State::Waiting || mMembers.empty()) {
        return;
    }
    const int count = readyCount();
    const std::string waiting = waitingFor();
    if (count == mLastReadyLogged && waiting == mLastWaitingLogged) {
        return;
    }
    mLastReadyLogged = count;
    mLastWaitingLogged = waiting;
    if (waiting.empty()) {
        lobby_log("ready: %d of %d players ready: everyone is ready%s", count, mPlayers, mIsHost ? "; Start game is on" : "");
    } else {
        lobby_log("ready: %d of %d players ready; waiting for: %s", count, mPlayers, waiting.c_str());
    }
}

bool Lobby::canStart() const
{
    // Ready check: every person on every computer pressed A (seats nobody was asked
    // for are computer players in the game and do not count).
    return mIsHost && mState == State::Waiting && taken() == mPlayers && everyoneReady();
}

void Lobby::start()
{
    if (!canStart()) {
        return;
    }
    auto &impl = *mImpl;
    // The host's memory card, sent to everyone so all copies boot from the same save.
    std::string cardName;
    std::string bytes;
    std::error_code ec;
    for (const auto &entry : fs::directory_iterator(PartyBoard_ConfigPath, ec)) {
        const auto file = utf8_text(entry.path().filename());
        if (entry.is_regular_file() && file.rfind("MemoryCard", 0) == 0 && entry.path().extension() == ".raw") {
            std::ifstream in(entry.path(), std::ios::binary);
            bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            cardName = file;
            break;
        }
    }
    impl.cardName = cardName;
    impl.card.assign(bytes.begin(), bytes.end());
    // And the host's game settings, so every copy plays by the same rules.
    impl.settings = shared_settings();
    const std::string settingsLine = "SETTINGS " + impl.settings.dump() + "\n";
    const std::string header = "CARD " + (cardName.empty() ? std::string("-") : cardName) + " " + std::to_string(bytes.size()) + "\n";
    int sent = 0;
    for (auto &g : impl.guests) {
        if (g.seat > 0) {
            g.out += settingsLine;
            g.out += header;
            g.out += bytes;
            g.out += "GO\n";
            Impl::flush(g);
            ++sent;
        }
    }
    impl.startedAt = Clock::now();
    mState = State::Starting;
    mMessage = "Starting: sending the save to everyone...";
    lobby_log("host: start: sending %s (%zu bytes) and the game settings %s to %d player(s)", cardName.empty() ? "no save" : cardName.c_str(),
        bytes.size(), impl.settings.dump().c_str(), sent);
}

void Lobby::poll()
{
    auto &impl = *mImpl;
    if (mState == State::Idle || mState == State::Failed || mState == State::Launched) {
        return;
    }
    if (mIsHost) {
        // New connections: lobby browsers and players.
        for (;;) {
            sockaddr_in from {};
            socklen_t length = sizeof(from);
            const Socket fd = accept(impl.listenFd, reinterpret_cast<sockaddr *>(&from), &length);
            if (fd == kNoSocket) {
                break;
            }
            set_flags(fd);
            Impl::Conn conn;
            conn.fd = fd;
            char text[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &from.sin_addr, text, sizeof(text));
            conn.ip = text;
            impl.guests.push_back(std::move(conn));
        }
        bool rosterChanged = false;
        for (auto &g : impl.guests) {
            if (g.fd == kNoSocket) {
                continue;
            }
            bool open = Impl::pump(g, 4 * kMaxLine);
            std::string line;
            while (Impl::takeLine(g.in, line)) {
                const auto [word, rest] = split_first(line);
                if (word == "WHO") {
                    g.out += "HOSTING " + std::to_string(mPlayers) + " " + std::to_string(taken()) + " " + impl.name + "\n";
                    g.out += "PARTY " + std::to_string(mParty) + "\n";
                    g.closing = true;
                    lobby_log("host: %s looked for games", g.ip.c_str());
                } else if (word == "LOBBYVER" && g.seat < 0) {
                    g.lobbyVersion = std::atoi(rest.c_str());
                } else if (word == "PING") {
                    g.out += "PONG\n";
                } else if (word == "PONG") {
                    Impl::gotPong(g);
                } else if (word == "READYSEAT" && g.seat > 0) {
                    std::istringstream fields(rest);
                    int k = -1, controller = 0, ready = 0;
                    std::string device;
                    fields >> k >> controller >> ready;
                    std::getline(fields >> std::ws, device);
                    if (k >= 0 && k < g.people && g.seat + k < kMaxSeats) {
                        auto &r = mReady[static_cast<std::size_t>(g.seat + k)];
                        const bool nowReady = ready != 0 && controller != 0;
                        if (r.ready != nowReady || r.controller != (controller != 0)) {
                            lobby_log("ready: player %d %s (%s, %s)", g.seat + k + 1,
                                nowReady ? "ready" : controller ? "not ready" : "not ready: no controller",
                                device == "-" ? "nothing" : device.c_str(), g.computer.empty() ? g.ip.c_str() : g.computer.c_str());
                        }
                        r.controller = controller != 0;
                        r.ready = nowReady;
                        r.device = device == "-" ? std::string() : device;
                        mReadyDirty = true;
                    }
                } else if (word == "PEOPLE" && g.seat < 0) {
                    std::istringstream fields(rest);
                    int people = 1;
                    std::string computer;
                    fields >> people >> computer;
                    g.people = std::clamp(people, 1, kMaxSeats - 1);
                    g.computer = computer == "Mac" || computer == "PC" ? computer : std::string();
                } else if (word == "HELLO" && g.seat < 0) {
                    std::istringstream fields(rest);
                    std::string build, disc, guestName;
                    int port = kGamePort;
                    fields >> build >> disc >> port;
                    std::getline(fields >> std::ws, guestName);
                    guestName = clean_name(guestName);
                    std::string reject;
                    const BuildMatch match = match_build(impl.build, impl.simulation, build);
                    if (g.lobbyVersion != kLobbyProtocol) {
                        reject = "Your PartyBoard is a different version from the host's. Everyone needs the same build.";
                    } else if (!valid_port(port)) {
                        reject = "Your PartyBoard sent an invalid game port.";
                    } else if (mState != State::Waiting || taken() >= mPlayers) {
                        reject = "That game is full or already starting.";
                    } else if (taken() + g.people > mPlayers) {
                        const int left = mPlayers - taken();
                        reject = fmt_people_left(left, g.people, g.computer);
                    } else if (match == BuildMatch::Different) {
                        reject = "Your PartyBoard is a different version from the host's. Everyone needs the same build.";
                    } else if (disc != impl.disc) {
                        reject = "Your disc image is a different file from the host's. Everyone needs the same Mario Party 4 file.";
                    }
                    if (!reject.empty()) {
                        g.out += "REJECT " + reject + "\n";
                        g.closing = true;
                        mMessage = guestName + " couldn't join: " + reject;
                        const auto plus = build.find('+');
                        lobby_log("host: turned away %s (%s; build %s, cross-play %s; ours build %s, cross-play %s; disc %s): %s", guestName.c_str(),
                            g.ip.c_str(), short_hash(build.substr(0, plus)).c_str(),
                            plus == std::string::npos ? "none" : short_hash(build.substr(plus + 1)).c_str(), short_hash(impl.build).c_str(),
                            impl.simulation.empty() ? "none" : short_hash(impl.simulation).c_str(), short_hash(disc).c_str(), reject.c_str());
                        continue;
                    }
                    if (match == BuildMatch::CrossPlay) {
                        // Different binaries (another computer type or compiler), same game: both
                        // were built as cross-play builds from the same sources and options.
                        lobby_log("host: %s (%s) runs a different binary (build %s, ours %s) with the same cross-play simulation %s: accepted as cross-play",
                            guestName.c_str(), g.ip.c_str(), short_hash(build).c_str(), short_hash(impl.build).c_str(),
                            short_hash(impl.simulation).c_str());
                    }
                    // The first free block of consecutive seats big enough for that
                    // computer's people (seats are consecutive per computer).
                    const int seat = first_free_block(mMembers, mPlayers, g.people);
                    if (seat < 0) {
                        const std::string reason = "That game has no " + std::to_string(g.people)
                            + " places left side by side. Ask the host to start the lobby again.";
                        g.out += "REJECT " + reason + "\n";
                        g.closing = true;
                        mMessage = guestName + " couldn't join: " + reason;
                        lobby_log("host: turned away %s (%s): %d people, no free block of %d seats", guestName.c_str(), g.ip.c_str(),
                            g.people, g.people);
                        continue;
                    }
                    g.seat = seat;
                    for (int s = seat; s < seat + g.people && s < kMaxSeats; ++s) {
                        mReady[static_cast<std::size_t>(s)] = {};
                    }
                    mReadyDirty = true;
                    mMembers.push_back(Member {seat, guestName, g.ip, static_cast<std::uint16_t>(port), g.people, g.computer});
                    std::sort(mMembers.begin(), mMembers.end(), [](const Member &a, const Member &b) { return a.seat < b.seat; });
                    g.out += "WELCOME " + std::to_string(seat) + " " + std::to_string(mPlayers) + " " + std::to_string(mParty) + " "
                        + std::to_string(g.people) + "\n";
                    mMessage = guestName + " joined.";
                    if (g.people == 1) {
                        lobby_log("host: %s (%s, game port %d) joined as player %d; %d of %d", guestName.c_str(), g.ip.c_str(), port,
                            seat + 1, taken(), mPlayers);
                    } else {
                        lobby_log("host: %s (%s, game port %d, %s) joined with %d people as players %d-%d; %d of %d", guestName.c_str(),
                            g.ip.c_str(), port, g.computer.empty() ? "?" : g.computer.c_str(), g.people, seat + 1, seat + g.people,
                            taken(), mPlayers);
                    }
                    rosterChanged = true;
                } else if (word == "READY") {
                    g.ready = true;
                    lobby_log("host: player %d has the save and is relaunching", g.seat + 1);
                } else if (word == "BYE") {
                    g.closing = true;
                    g.out.clear();
                }
            }
            if (g.in.size() > kMaxLine) {
                open = false;   // a "line" that never ends
            }
            if (!open || (g.closing && g.out.empty())) {
                // A guest that said READY quits its game to relaunch into the
                // session; that disconnect is expected, not the player leaving.
                if (g.seat > 0 && !(mState == State::Starting && g.ready)) {
                    const auto it = std::find_if(mMembers.begin(), mMembers.end(), [&](const Member &m) { return m.seat == g.seat; });
                    if (it != mMembers.end()) {
                        mMessage = it->name + " left.";
                        lobby_log("host: %s (player %d) left", it->name.c_str(), g.seat + 1);
                        for (int s = it->seat; s < it->seat + it->seats && s < kMaxSeats; ++s) {
                            mReady[static_cast<std::size_t>(s)] = {};
                        }
                        mReadyDirty = true;
                        mMembers.erase(it);
                        rosterChanged = true;
                    }
                    if (mState == State::Starting) {
                        mState = State::Waiting;
                        mMessage += " Start again when everyone is back.";
                        lobby_log("host: start cancelled; waiting again");
                    }
                }
                close_socket(g.fd);
                g.fd = kNoSocket;
            }
        }
        impl.guests.erase(std::remove_if(impl.guests.begin(), impl.guests.end(), [](const Impl::Conn &c) { return c.fd == kNoSocket; }),
            impl.guests.end());
        if (rosterChanged) {
            std::string roster = "ROSTER " + std::to_string(mPlayers) + " " + std::to_string(mParty) + "\n";
            for (const auto &m : mMembers) {
                roster += "MEMBER " + std::to_string(m.seat) + " " + m.ip + " " + std::to_string(m.gamePort) + " "
                    + std::to_string(m.seats) + " " + (m.computer.empty() ? std::string("-") : m.computer) + " " + m.name + "\n";
            }
            roster += "END\n";
            for (auto &g : impl.guests) {
                if (g.seat > 0) {
                    g.out += roster;
                }
            }
        }
        // Ready check: everyone's seats to everyone, when anything changed; and the ping.
        if (mReadyDirty) {
            mReadyDirty = false;
            std::string state;
            for (const auto &m : mMembers) {
                for (int s = m.seat; s < m.seat + m.seats && s < kMaxSeats; ++s) {
                    const auto &r = mReady[static_cast<std::size_t>(s)];
                    state += "READYSTATE " + std::to_string(s) + " " + (r.controller ? "1" : "0") + " " + (r.ready ? "1" : "0") + " "
                        + (r.device.empty() ? std::string("-") : r.device) + "\n";
                }
            }
            for (auto &g : impl.guests) {
                if (g.seat > 0) {
                    g.out += state;
                }
            }
        }
        for (auto &g : impl.guests) {
            if (g.seat > 0 && mState == State::Waiting) {
                Impl::servicePing(g);
            }
            Impl::flush(g);
        }
        logReadySummary();
        if (mState == State::Starting) {
            const bool everyoneReady = std::all_of(impl.guests.begin(), impl.guests.end(),
                [](const Impl::Conn &c) { return c.seat <= 0 || c.ready; });
            const bool timedOut = Clock::now() - impl.startedAt > std::chrono::seconds(20);
            if (everyoneReady || timedOut) {
                lobby_log("host: %s; relaunching", everyoneReady ? "everyone has the save" : "20 seconds passed without every READY");
                launch();
            }
        }
        return;
    }

    // Guest.
    auto &h = impl.hostLink;
    const bool open = Impl::pump(h, kMaxCardBytes + 4 * kMaxLine);
    for (;;) {
        if (impl.receivingCard) {
            const std::size_t take = std::min(impl.cardRemaining, h.in.size());
            impl.card.insert(impl.card.end(), h.in.begin(), h.in.begin() + static_cast<std::ptrdiff_t>(take));
            h.in.erase(0, take);
            impl.cardRemaining -= take;
            if (impl.cardRemaining > 0) {
                break;
            }
            impl.receivingCard = false;
            lobby_log("join: save received (%zu bytes)", impl.card.size());
        }
        std::string line;
        if (!Impl::takeLine(h.in, line)) {
            if (h.in.size() > kMaxLine) {
                fail("The host sent invalid lobby data.");
                return;
            }
            break;
        }
        const auto [word, rest] = split_first(line);
        std::istringstream fields(rest);
        if (word == "WELCOME") {
            int party = 4;
            int seats = 1;
            fields >> impl.seat >> mPlayers;
            if (!(fields >> party)) {
                party = 4; // an older host: the normal party
            } else if (!(fields >> seats)) {
                seats = 1;
            }
            if (seats != impl.people) {
                fail("The host sent invalid lobby data.");
                return;
            }
            mParty = party == 8 ? 8 : 4;
            if (impl.seat < 1 || impl.seat >= kMaxSeats || mPlayers < 2 || mPlayers > (mParty == 8 ? kMaxSeats : kNormalPartySeats)
                || impl.seat + impl.people > mPlayers || (party != 4 && party != 8)) {
                fail("The host sent invalid lobby data.");
                return;
            }
            mState = State::Waiting;
            mMessage = "Joined. Waiting for the host to start.";
            if (impl.people == 1) {
                lobby_log("join: in, as player %d of %d (%d-player party)", impl.seat + 1, mPlayers, mParty);
            } else {
                lobby_log("join: in, as players %d-%d of %d (%d-player party, %d people on this %s)", impl.seat + 1,
                    impl.seat + impl.people, mPlayers, mParty, impl.people, kComputer);
            }
        } else if (word == "PING") {
            h.out += "PONG\n";
        } else if (word == "PONG") {
            Impl::gotPong(h);
        } else if (word == "READYSTATE") {
            int seat = -1, controller = 0, ready = 0;
            std::string device;
            fields >> seat >> controller >> ready;
            std::getline(fields >> std::ws, device);
            if (seat >= 0 && seat < kMaxSeats && (seat < impl.seat || seat >= impl.seat + impl.people)) {
                // Our own seats are ours to say; the host's copy of them only echoes.
                auto &r = mReady[static_cast<std::size_t>(seat)];
                r.controller = controller != 0;
                r.ready = ready != 0 && controller != 0;
                r.device = device == "-" ? std::string() : device;
            }
        } else if (word == "REJECT") {
            lobby_log("join: the host turned this player away");
            fail(rest);
            return;
        } else if (word == "ROSTER") {
            int party = mParty;
            fields >> mPlayers;
            if (fields >> party) {
                mParty = party == 8 ? 8 : 4;
            }
            if (mPlayers < 2 || mPlayers > (mParty == 8 ? kMaxSeats : kNormalPartySeats)) {
                fail("The host sent invalid lobby data.");
                return;
            }
            mMembers.clear();
        } else if (word == "MEMBER") {
            Member m;
            long port = 0;
            fields >> m.seat >> m.ip >> port >> m.seats >> m.computer;
            std::getline(fields >> std::ws, m.name);
            if (m.computer != "Mac" && m.computer != "PC") {
                m.computer.clear();
            }
            if (m.seat < 0 || m.seats < 1 || m.seat + m.seats > mPlayers || !valid_ipv4(m.ip) || !valid_port(port)
                || mMembers.size() >= kMaxSeats) {
                fail("The host sent invalid lobby data.");
                return;
            }
            m.name = clean_name(m.name);
            m.gamePort = static_cast<std::uint16_t>(port);
            mMembers.push_back(m);
        } else if (word == "END") {
            std::string who;
            for (const auto &m : mMembers) {
                who += " " + std::to_string(m.seat + 1) + (m.seats > 1 ? "-" + std::to_string(m.seat + m.seats) : std::string()) + "="
                    + m.name + "@" + m.ip + ":" + std::to_string(m.gamePort);
            }
            lobby_log("join: players %d of %d:%s", taken(), mPlayers, who.c_str());
        } else if (word == "SETTINGS") {
            auto settings = nlohmann::json::parse(rest, nullptr, false);
            if (settings.is_discarded() || !valid_settings(settings)) {
                fail("The host sent invalid lobby data.");
                return;
            }
            impl.settings = std::move(settings);
            lobby_log("join: the host's game settings: %s", impl.settings.dump().c_str());
        } else if (word == "CARD") {
            std::string name;
            long long size = -1;
            fields >> name >> size;
            if (name == "-") {
                name.clear();
            }
            if ((!name.empty() && !valid_card_name(name)) || size < 0 || static_cast<unsigned long long>(size) > kMaxCardBytes
                || (name.empty() && size != 0)) {
                fail("The host sent an invalid save file, so the game was not started.");
                return;
            }
            impl.cardName = name;
            impl.card.clear();
            impl.card.reserve(static_cast<std::size_t>(size));
            impl.cardRemaining = size;
            impl.receivingCard = size > 0;
            mState = State::Starting;
            mMessage = "Starting: receiving the host's save...";
            lobby_log("join: the host started; receiving %s (%lld bytes)", name.empty() ? "no save" : name.c_str(), size);
        } else if (word == "GO") {
            impl.goReceived = true;
        }
    }
    if (mState == State::Waiting) {
        Impl::servicePing(h);
    }
    logReadySummary();
    if (impl.goReceived && !impl.receivingCard) {
        h.out += "READY\n";
        Impl::flush(h);
        lobby_log("join: ready; relaunching");
        launch();
        return;
    }
    Impl::flush(h);
    if (!open) {
        fail(mState == State::Starting ? "The connection to the host dropped while starting." : "The host closed the lobby.");
    }
}

void Lobby::launch()
{
    auto &impl = *mImpl;
    // 1. The online profile: this player's own settings and controls, with the
    //    host's memory card. Real saves stay untouched.
    std::error_code ec;
    const fs::path real = PartyBoard_ConfigPath;
    const fs::path online = real.parent_path() / "Party Board Online";
    fs::create_directories(online, ec);
    for (const auto &entry : fs::directory_iterator(online, ec)) {
        if (entry.is_regular_file() && utf8_text(entry.path().filename()).rfind("MemoryCard", 0) == 0) {
            fs::remove(entry.path(), ec);
        }
    }
    int copied = 0;
    for (const auto &entry : fs::directory_iterator(real, ec)) {
        const auto file = utf8_text(entry.path().filename());
        if (entry.is_regular_file() && file.rfind("MemoryCard", 0) != 0 && file.rfind("pipeline_cache", 0) != 0) {
            fs::copy_file(entry.path(), online / entry.path().filename(), fs::copy_options::overwrite_existing, ec);
            ++copied;
        }
    }
    if (!impl.cardName.empty()) {
        const std::string file = fs::path(impl.cardName).filename().string();
        if (!valid_card_name(file) || file != impl.cardName) {
            fail("The host sent an invalid save file, so the game was not started.");
            return;
        }
        std::ofstream out(online / file, std::ios::binary | std::ios::trunc);
        out.write(impl.card.data(), static_cast<std::streamsize>(impl.card.size()));
    }
    lobby_log("launch: online profile %s: %d settings file(s) copied, save %s (%zu bytes)", utf8_text(online).c_str(), copied,
        impl.cardName.empty() ? "none" : impl.cardName.c_str(), impl.card.size());
    // The host's game settings over this player's own, in the online copy only.
    apply_shared_settings(online / "config.json", impl.settings);

    // 2. The command line for PartyBoard's direct full-game netplay.
    int mySeat = mIsHost ? 0 : impl.seat;
    // A block "first-last" when a computer has several people (docs/ONLINE_MULTISEAT.md).
    const auto block = [](int seat, int seats) {
        return seats > 1 ? std::to_string(seat) + "-" + std::to_string(seat + seats - 1) : std::to_string(seat);
    };
    std::vector<std::string> args;
    args.push_back("--netplay-host");
    args.push_back(std::to_string(impl.gamePort));
    args.push_back("--netplay-players");
    args.push_back(std::to_string(mPlayers));
    args.push_back("--netplay-seat");
    args.push_back(block(mySeat, impl.people));
    for (const auto &m : mMembers) {
        if (m.seat != mySeat) {
            args.push_back("--netplay-peer");
            args.push_back(block(m.seat, m.seats) + ":" + m.ip + ":" + std::to_string(m.gamePort));
        }
    }
    for (const auto &m : mMembers) {
        lobby_log("launch: %s: %s (%s, %s)", m.seats > 1 ? ("players " + std::to_string(m.seat + 1) + "-" + std::to_string(m.seat + m.seats)).c_str()
                                                           : ("player " + std::to_string(m.seat + 1)).c_str(),
            m.name.c_str(), m.computer.empty() ? "computer type not sent" : m.computer.c_str(), m.seat == mySeat ? "this one" : m.ip.c_str());
    }
    for (const char *a : {"--netplay-full", "--netplay-pad", "1", "--netplay-delay", "3"}) {
        args.push_back(a);
    }
    if (mParty == 8) {
        // The 8-player party: Party Mode plays it with these seats as its people.
        args.push_back("--netplay-party8");
    }
    // Test aid (the two-computer lobby test): extra game arguments, e.g. the scripted
    // walk, so a lobby-started session can play a whole computer-driven game.
    if (const char *extra = std::getenv("PARTYBOARD_TAILNET_TEST_EXTRA_ARGS"); extra != nullptr && *extra != '\0') {
        std::istringstream words(extra);
        for (std::string word; words >> word;) {
            args.push_back(word);
        }
        lobby_log("launch: TEST extra game arguments: %s", extra);
    }

    // 3. Start the online copy (through the app's launcher, so the phone
    //    controller and session logs come along), once this one has quit,
    //    then quit this one.
    const fs::path exe = executable_path();
    // Game traffic on the Tailscale address only, not every network this
    // computer is on.
    std::string bindIp = mIsHost ? impl.selfIp : std::string();
    for (const auto &m : mMembers) {
        if (!mIsHost && m.seat == mySeat) {
            bindIp = m.ip;
        }
    }
    std::string summary = (impl.people > 1 ? "Online: seats " + std::to_string(mySeat + 1) + "-" + std::to_string(mySeat + impl.people)
                                          : "Online: seat " + std::to_string(mySeat + 1))
        + " of " + std::to_string(mPlayers) + ";";
    for (const auto &a : args) {
        summary += " " + a;
    }
    std::fprintf(stdout, "%s\n", summary.c_str());
    std::fflush(stdout);
#if defined(_WIN32)
    DWORD processId = 0;
    if (!start_online_copy(exe, args, online, impl.discPath, bindIp, processId)) {
        fail("Couldn't start the online game.");
        return;
    }
    lobby_log("launch: started the online copy (process %lu, %s); quitting this one", processId, utf8_text(exe).c_str());
#else
    fs::path program = exe;
    if (exe.filename() == "PartyBoard-bin" && fs::exists(exe.parent_path() / "PartyBoard")) {
        program = exe.parent_path() / "PartyBoard";
    }
    setenv("PARTYBOARD_NETPLAY_TEST_PROFILE", online.c_str(), 1);
    setenv("PARTYBOARD_DISC_IMAGE", impl.discPath.c_str(), 1);
    if (valid_ipv4(bindIp)) {
        setenv("PARTYBOARD_NETPLAY_BIND", bindIp.c_str(), 1);
    }
    std::vector<std::string> shellArgs {"/bin/sh", "-c", "sleep 2; exec \"$@\"", "sh", program.string()};
    shellArgs.insert(shellArgs.end(), args.begin(), args.end());
    std::vector<char *> argv;
    for (auto &a : shellArgs) {
        argv.push_back(a.data());
    }
    argv.push_back(nullptr);
    // Nothing of this process may leak into the next one: an inherited socket
    // (the phone controller's input port, the lobby) would stay bound and block
    // the new copy from opening it. Its output gets its own session log.
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_CLOEXEC_DEFAULT | POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attributes, 0);
    posix_spawn_file_actions_t files;
    posix_spawn_file_actions_init(&files);
    posix_spawn_file_actions_addopen(&files, 0, "/dev/null", O_RDONLY, 0);
    // Test aid (the two-computer lobby test): a launcher-less test app has no
    // session log, so the relaunched copy's output can be sent to a file.
    const char *relaunchLog = std::getenv("PARTYBOARD_TAILNET_RELAUNCH_LOG");
    if (relaunchLog != nullptr && *relaunchLog != '\0') {
        posix_spawn_file_actions_addopen(&files, 1, relaunchLog, O_WRONLY | O_CREAT | O_APPEND, 0644);
        posix_spawn_file_actions_adddup2(&files, 1, 2);
    } else {
        posix_spawn_file_actions_addopen(&files, 1, "/dev/null", O_WRONLY, 0);
        posix_spawn_file_actions_addopen(&files, 2, "/dev/null", O_WRONLY, 0);
    }
    pid_t pid = 0;
    const int spawned = posix_spawn(&pid, "/bin/sh", &files, &attributes, argv.data(), environ);
    posix_spawn_file_actions_destroy(&files);
    posix_spawnattr_destroy(&attributes);
    if (spawned != 0) {
        lobby_log("launch: couldn't start %s (error %d)", program.c_str(), spawned);
        fail("Couldn't start the online game.");
        return;
    }
    lobby_log("launch: started the online copy (process %d, %s, in 2 seconds); quitting this one", static_cast<int>(pid), program.c_str());
#endif
    impl.closeAll();
    mState = State::Launched;
    mMessage = "Starting the online game...";
    SDL_Event quit {};
    quit.type = SDL_EVENT_QUIT;
    SDL_PushEvent(&quit);
}

// ---- Self-test ------------------------------------------------------------
//
// partyboard --tailnet-lobby-self-test runs the whole lobby on this one
// computer without a window: finds Tailscale and this computer's address,
// fingerprints the build and the disc, hosts, finds its own game the way a
// friend's "Find games" would, turns away a player with a different disc,
// seats a second player, starts, and relaunches both seats. The relaunched
// copies only report (PARTYBOARD_TAILNET_RELAUNCH_PROBE, the folder they write
// probe-seat<N>.txt to) or, with PARTYBOARD_TAILNET_SELFTEST_PLAY=1 instead,
// really start the online game against each other.
//
// Needs PARTYBOARD_TEST_PROFILE (a throwaway profile: a stand-in save is
// written there; its other files become the online profile's settings, as a
// player's own settings do) and PARTYBOARD_DISC_IMAGE.

namespace {

void set_env(const char *name, const std::string &value)
{
#if defined(_WIN32)
    _putenv_s(name, value.c_str());
    SetEnvironmentVariableA(name, value.c_str());   // what the relaunch copies
#else
    setenv(name, value.c_str(), 1);
#endif
}

void unset_env(const char *name)
{
#if defined(_WIN32)
    _putenv_s(name, "");
    SetEnvironmentVariableA(name, nullptr);
#else
    unsetenv(name);
#endif
}

template <typename Done, typename Step>
bool wait_for(Done done, int timeoutMs, Step step)
{
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
    while (!done()) {
        if (Clock::now() > deadline) {
            return false;
        }
        step();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
}

bool self_test()
{
    int failures = 0;
    const auto check = [&](const char *what, bool ok, const std::string &detail) {
        std::printf("TAILNET %-40s %s%s%s\n", what, ok ? "PASS" : "FAIL", detail.empty() ? "" : "  ", detail.c_str());
        std::fflush(stdout);
        failures += ok ? 0 : 1;
        return ok;
    };
    const char *profile = std::getenv("PARTYBOARD_TEST_PROFILE");
    const char *disc = std::getenv("PARTYBOARD_DISC_IMAGE");
    const char *probeValue = std::getenv("PARTYBOARD_TAILNET_RELAUNCH_PROBE");
    const char *playValue = std::getenv("PARTYBOARD_TAILNET_SELFTEST_PLAY");
    const std::string probe = probeValue ? probeValue : "";
    const bool play = playValue && std::strcmp(playValue, "1") == 0;
    if (!check("test settings given", profile && *profile && disc && *disc && (!probe.empty() || play),
            "needs PARTYBOARD_TEST_PROFILE, PARTYBOARD_DISC_IMAGE and PARTYBOARD_TAILNET_RELAUNCH_PROBE "
            "(or PARTYBOARD_TAILNET_SELFTEST_PLAY=1)")) {
        return false;
    }
    // Like SDL_GetPrefPath, the profile folder with a trailing separator, so
    // the online profile lands where a real session puts it.
    PartyBoard_ConfigPath = utf8_path(profile) / "";
    // The relaunched seats then use the online profile, as in a real session
    // (PARTYBOARD_TEST_PROFILE would win over it).
    unset_env("PARTYBOARD_TEST_PROFILE");
    std::error_code ec;
    fs::create_directories(PartyBoard_ConfigPath, ec);
    // The settings as the game has them; never loaded from a file here, so the
    // host plays with the defaults.
    partyboard::registerSettings();
    config::FinishRegistration();
    // This "player's own" config.json disagrees with the host on two game
    // settings: the online profile must get the host's, this file must not change.
    const fs::path ownConfig = PartyBoard_ConfigPath / "config.json";
    nlohmann::json own = nlohmann::json::object();
    if (std::ifstream in(ownConfig); in) {
        own = nlohmann::json::parse(in, nullptr, false);
        if (own.is_discarded() || !own.is_object()) {
            own = nlohmann::json::object();
        }
    }
    own["game.language"] = 2;
    own["game.unlockBowsersGnarlyParty"] = true;
    const std::string ownText = own.dump(4);
    {
        std::ofstream out(ownConfig, std::ios::trunc);
        out << ownText;
    }
    std::string card(64 * 1024, '\0');
    for (std::size_t i = 0; i < card.size(); ++i) {
        card[i] = static_cast<char>(i * 7 + 3);
    }
    {
        std::ofstream out(PartyBoard_ConfigPath / "MemoryCardA.USA.raw", std::ios::binary | std::ios::trunc);
        out.write(card.data(), static_cast<std::streamsize>(card.size()));
    }
    const auto idle = [] {};

    Scanner scanner;
    scanner.start();
    wait_for([&] { return !scanner.busy(); }, 30000, idle);
    const std::string selfIp = scanner.selfIp();
    if (!check("Tailscale found and connected", !selfIp.empty(), selfIp.empty() ? scanner.error() : "this computer is " + selfIp)) {
        return false;
    }

    Identity identity;
    identity.start(disc);
    wait_for([&] { return identity.ready() || identity.failed(); }, 600000, idle);
    if (!check("build and disc fingerprints", identity.ready(),
            identity.ready() ? "build " + short_hash(identity.build()) + ", disc " + short_hash(identity.disc()) : identity.error())) {
        return false;
    }

    {
        // The cross-play rules, without a second computer: a byte-identical build always
        // matches; different binaries only when both carry the same full fingerprint.
        const std::string a(64, 'a'), b(64, 'b'), simA(64, '1'), simB(64, '2');
        const bool rules = match_build(a, "", hello_build_field(a, "")) == BuildMatch::Identical
            && match_build(a, simA, hello_build_field(a, simA)) == BuildMatch::Identical
            && match_build(a, "", hello_build_field(b, "")) == BuildMatch::Different
            && match_build(a, simA, hello_build_field(b, simA)) == BuildMatch::CrossPlay
            && match_build(a, simA, hello_build_field(b, simB)) == BuildMatch::Different
            && match_build(a, simA, hello_build_field(b, "")) == BuildMatch::Different
            && match_build(a, "", hello_build_field(b, simA)) == BuildMatch::Different
            && match_build(a, "", "") == BuildMatch::Different
            && match_build(a, simA, "+" + simA) == BuildMatch::CrossPlay
            && match_build(a, simA, b + "+" + simA.substr(0, 63)) == BuildMatch::Different;
        check("cross-play identity rules", rules,
            identity.simulation().empty() ? "this is a regular build (byte-identical copies only)"
                                          : "this cross-play build: " + short_hash(identity.simulation()));
    }

    Lobby host;
    const bool hosting = host.host(selfIp, 2, "Self-test host", identity);
    if (!check("host a 2-player lobby", hosting, host.message())) {
        return false;
    }
    const auto pollHost = [&] { host.poll(); };

    // "Find games", as a friend would press it: this computer's own address is
    // in the list it asks.
    scanner.start();
    wait_for([&] { return !scanner.busy(); }, 30000, pollHost);
    const auto offers = scanner.offers();
    check("Find games lists the lobby",
        std::any_of(offers.begin(), offers.end(),
            [&](const HostOffer &o) { return o.ip == selfIp && o.players == 2 && o.joined == 1 && o.name == "Self-test host"; }),
        std::to_string(offers.size()) + " game(s) found");

    // Hosting again straight away: leaving frees the lobby port.
    host.leave();
    const bool hostingAgain = host.host(selfIp, 2, "Self-test host", identity);
    check("host again right after leaving", hostingAgain, host.message());

    // A player whose disc file differs is turned away (here: this program's
    // own file stands in for a different disc).
    Identity other;
    other.start(utf8_text(executable_path()));
    wait_for([&] { return other.ready() || other.failed(); }, 60000, pollHost);
    Lobby stranger;
    stranger.join(selfIp, "Different disc", other);
    wait_for([&] { return stranger.state() == Lobby::State::Failed; }, 10000, [&] {
        host.poll();
        stranger.poll();
    });
    check("a different disc is turned away",
        stranger.state() == Lobby::State::Failed && stranger.message().find("disc image") != std::string::npos, stranger.message());

    // Several people on one computer (PARTYBOARD_TAILNET_SELFTEST_PEOPLE=<host>:<guest>,
    // e.g. 4:4 or 2:2): the host hosts for everyone with <host> people on its computer,
    // the guest asks for <guest> seats.
    int hostPeople = 1, guestPeople = 1;
    if (const char *people = std::getenv("PARTYBOARD_TAILNET_SELFTEST_PEOPLE"); people != nullptr && *people != '\0') {
        std::sscanf(people, "%d:%d", &hostPeople, &guestPeople);
        hostPeople = std::clamp(hostPeople, 1, kMaxSeats - 1);
        guestPeople = std::clamp(guestPeople, 1, kMaxSeats - hostPeople);
    }
    if (hostPeople + guestPeople > 2) {
        const int party = hostPeople + guestPeople > kNormalPartySeats ? 8 : 4;
        host.leave();
        const bool hostingMany = host.host(selfIp, hostPeople + guestPeople, "Self-test host", identity, party, hostPeople);
        check("host for several people per computer", hostingMany && host.taken() == hostPeople,
            std::to_string(hostPeople) + " here + " + std::to_string(guestPeople) + " on the other computer, "
                + std::to_string(party) + "-player party");
        // A computer asking for more seats than are left is turned away.
        Lobby tooMany;
        tooMany.join(selfIp, "Too many", identity, guestPeople + 1);
        wait_for([&] { return tooMany.state() == Lobby::State::Failed; }, 10000, [&] {
            host.poll();
            tooMany.poll();
        });
        check("too many people are turned away",
            tooMany.state() == Lobby::State::Failed && tooMany.message().find("places left") != std::string::npos, tooMany.message());
    }

    // The second player, on its own game port as a second computer would be.
    set_env("PARTYBOARD_ONLINE_GAME_PORT", std::to_string(kGamePort + 1));
    Lobby guest;
    guest.join(selfIp, "Self-test guest", identity, guestPeople);
    // Ready check: the test's people all "press A" (a controller stands in for each).
    const auto readyAll = [](Lobby &lobby) {
        for (int k = 0; k < lobby.localPeople(); ++k) {
            lobby.setLocalSeat(k, true, true, "self-test controller");
        }
    };
    const auto pollBoth = [&] {
        host.poll();
        guest.poll();
        readyAll(host);
        readyAll(guest);
    };
    wait_for([&] { return host.canStart() && guest.state() == Lobby::State::Waiting && guest.members().size() == 2; }, 10000, pollBoth);
    check("second player gets seat 2", host.canStart() && guest.members().size() == 2, guest.message());
    if (hostPeople + guestPeople > 2) {
        const auto &m = guest.members();
        const bool blocks = m.size() == 2 && m[0].seat == 0 && m[0].seats == hostPeople && m[1].seat == hostPeople
            && m[1].seats == guestPeople && m[0].computer == kComputer && m[1].computer == kComputer;
        check("seat blocks in seat order", blocks,
            m.size() == 2 ? "players 1-" + std::to_string(m[0].seats) + " and " + std::to_string(m[1].seat + 1) + "-"
                    + std::to_string(m[1].seat + m[1].seats)
                          : std::to_string(m.size()) + " member(s)");
    }

    host.start();
    wait_for([&] { return host.state() == Lobby::State::Launched && guest.state() == Lobby::State::Launched; }, 30000, pollBoth);
    check("both seats relaunch", host.state() == Lobby::State::Launched && guest.state() == Lobby::State::Launched,
        host.message() + " / " + guest.message());

    std::ifstream sent(PartyBoard_ConfigPath.parent_path() / "Party Board Online" / "MemoryCardA.USA.raw", std::ios::binary);
    const std::string received((std::istreambuf_iterator<char>(sent)), std::istreambuf_iterator<char>());
    check("online profile has the host's save", received == card, std::to_string(received.size()) + " bytes");

    const auto hostSettings = shared_settings();
    nlohmann::json onlineConfig;
    if (std::ifstream in(PartyBoard_ConfigPath.parent_path() / "Party Board Online" / "config.json"); in) {
        onlineConfig = nlohmann::json::parse(in, nullptr, false);
    }
    bool settingsMatch = onlineConfig.is_object() && !hostSettings.empty();
    for (const auto &[key, value] : hostSettings.items()) {
        settingsMatch = settingsMatch && onlineConfig.contains(key) && onlineConfig[key] == value;
    }
    std::ifstream ownAfter(ownConfig);
    const std::string ownTextAfter((std::istreambuf_iterator<char>(ownAfter)), std::istreambuf_iterator<char>());
    check("online profile has the host's settings", settingsMatch,
        onlineConfig.is_object() ? "language " + onlineConfig.value("game.language", nlohmann::json()).dump() + ", Gnarly Party unlock "
                + onlineConfig.value("game.unlockBowsersGnarlyParty", nlohmann::json()).dump()
                                 : std::string("no online config.json"));
    check("player's own settings untouched", ownTextAfter == ownText, "language 2, Gnarly Party unlock true kept");

    if (play) {
        std::printf("TAILNET failures=%d (the relaunched seats now start the online game against each other)\n", failures);
    } else {
        std::printf("TAILNET failures=%d (the relaunched copies write %s/probe-seat1.txt and probe-seat2.txt once this one has quit)\n",
            failures, probe.c_str());
    }
    std::fflush(stdout);
    return failures == 0;
}

// Whether this computer could open `port` on `ip` right now.
bool port_free(const char *ip, std::uint16_t port, int type)
{
    if (!sockets_ready()) {
        return false;
    }
    sockaddr_in address {};
    if (!make_address(ip && *ip ? ip : "0.0.0.0", port, address)) {
        return false;
    }
    const Socket fd = socket(AF_INET, type, 0);
    if (fd == kNoSocket) {
        return false;
    }
#if !defined(_WIN32)
    // As the lobby itself opens it: the lobby's finished connections keep the
    // port reserved for a while otherwise.
    const int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
#endif
    const bool available = bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0;
    close_socket(fd);
    return available;
}

// Two computers, one real lobby, no window: the cross-play check of the lobby
// (docs/CROSS_PLATFORM_ONLINE.md). Run on each computer, the host first:
//   --tailnet-lobby-pair-test host <players> [8] (this computer hosts; 8 = the 8-player party)
//   --tailnet-lobby-pair-test join <host ip>     (this computer joins it)
// with PARTYBOARD_TEST_PROFILE (a profile with config.json and the memory
// card; its config.json is loaded, so the host sends its own game settings)
// and PARTYBOARD_DISC_IMAGE. The relaunched seats then play for real (or, with
// PARTYBOARD_TAILNET_RELAUNCH_PROBE, write down what they were given).
// PARTYBOARD_ONLINE_GAME_PORT gives each seat on one computer its own port.
// PARTYBOARD_TAILNET_PAIR_PEOPLE=<n>: n people on this computer (several people per computer).
int pair_test(int argc, char *argv[])
{
    const auto say = [](const char *format, auto... values) {
        std::printf(format, values...);
        std::printf("\n");
        std::fflush(stdout);
    };
    const bool hosting = argc >= 4 && std::strcmp(argv[2], "host") == 0;
    const bool joining = argc >= 4 && std::strcmp(argv[2], "join") == 0;
    const char *profile = std::getenv("PARTYBOARD_TEST_PROFILE");
    const char *disc = std::getenv("PARTYBOARD_DISC_IMAGE");
    if ((!hosting && !joining) || !profile || !*profile || !disc || !*disc) {
        say("PAIR FAIL usage: --tailnet-lobby-pair-test host <players> | join <host ip>, "
            "with PARTYBOARD_TEST_PROFILE and PARTYBOARD_DISC_IMAGE");
        return 2;
    }
    PartyBoard_ConfigPath = utf8_path(profile) / "";
    unset_env("PARTYBOARD_TEST_PROFILE");
    partyboard::registerSettings();
    config::FinishRegistration();
    const std::string configFile = utf8_text(PartyBoard_ConfigPath / "config.json");
    config::LoadFromFileName(configFile.c_str());
    const auto idle = [] {};

    Scanner scanner;
    scanner.start();
    wait_for([&] { return !scanner.busy(); }, 30000, idle);
    const std::string selfIp = scanner.selfIp();
    if (selfIp.empty()) {
        say("PAIR FAIL Tailscale: %s", scanner.error().c_str());
        return 3;
    }
    Identity identity;
    identity.start(disc);
    wait_for([&] { return identity.ready() || identity.failed(); }, 600000, idle);
    if (!identity.ready()) {
        say("PAIR FAIL identity: %s", identity.error().c_str());
        return 3;
    }
    say("PAIR identity this %s %s: build %s, cross-play %s, disc %s", kComputer, selfIp.c_str(),
        short_hash(identity.build()).c_str(), identity.simulation().empty() ? "none" : short_hash(identity.simulation()).c_str(),
        short_hash(identity.disc()).c_str());

    Lobby lobby;
    // Ready check: every person here "presses A" unless PARTYBOARD_TAILNET_PAIR_READY=0.
    const char *readyValue = std::getenv("PARTYBOARD_TAILNET_PAIR_READY");
    const bool autoReady = readyValue == nullptr || std::strcmp(readyValue, "0") != 0;
    const auto poll = [&] {
        lobby.poll();
        if (autoReady) {
            for (int k = 0; k < lobby.localPeople(); ++k) {
                lobby.setLocalSeat(k, true, true, "pair test");
            }
        }
    };
    const std::string name = std::string("Pair test ") + kComputer;
    int people = 1;
    if (const char *value = std::getenv("PARTYBOARD_TAILNET_PAIR_PEOPLE"); value != nullptr && *value != '\0') {
        people = std::clamp(std::atoi(value), 1, kMaxSeats - 1);
    }
    say("PAIR %d %s on this %s", people, people == 1 ? "person" : "people", kComputer);
    if (hosting) {
        const int players = std::atoi(argv[3]);
        const int party = argc >= 5 && std::atoi(argv[4]) == 8 ? 8 : 4;
        if (!lobby.host(selfIp, players, name, identity, party, people)) {
            say("PAIR FAIL host: %s", lobby.message().c_str());
            return 4;
        }
        say("PAIR hosting %d players (%d-player party) on %s", lobby.players(), lobby.party(), selfIp.c_str());
        std::size_t seen = 1;
        const bool full = wait_for([&] {
            if (lobby.members().size() != seen) {
                seen = lobby.members().size();
                say("PAIR %zu of %d joined: %s", seen, lobby.players(), lobby.message().c_str());
            }
            return lobby.canStart() && lobby.taken() == lobby.players();
        }, 600000, poll);
        if (!full) {
            say("PAIR FAIL nobody completed the lobby: %s", lobby.message().c_str());
            return 5;
        }
        lobby.start();
    } else {
        lobby.join(argv[3], name, identity, people);
    }
    wait_for([&] { return lobby.state() == Lobby::State::Launched || lobby.state() == Lobby::State::Failed; }, 600000, poll);
    for (const auto &m : lobby.members()) {
        say("PAIR seat %d%s %s %s:%u", m.seat + 1, m.seats > 1 ? ("-" + std::to_string(m.seat + m.seats)).c_str() : "", m.name.c_str(),
            m.ip.c_str(), static_cast<unsigned>(m.gamePort));
    }
    if (lobby.state() != Lobby::State::Launched) {
        say("PAIR FAIL %s: %s", hosting ? "host" : "guest", lobby.message().c_str());
        return 6;
    }
    say("PAIR OK %s launched the online copy: %s", hosting ? "host" : "guest", lobby.message().c_str());
    return 0;
}

// What a relaunched seat would play with, written down instead of playing.
int relaunch_probe(int argc, char *argv[])
{
    int seat = -1;
    long gamePort = 0;
    std::ostringstream report;
    report << "probe: relaunched copy\n";
    for (int i = 1; i < argc; ++i) {
        report << "arg " << argv[i] << "\n";
        if (i + 1 < argc && std::strcmp(argv[i], "--netplay-seat") == 0) {
            seat = std::atoi(argv[i + 1]);
        }
        if (i + 1 < argc && std::strcmp(argv[i], "--netplay-host") == 0) {
            gamePort = std::atol(argv[i + 1]);
        }
    }
    for (const char *name : {"PARTYBOARD_NETPLAY_TEST_PROFILE", "PARTYBOARD_TEST_PROFILE", "PARTYBOARD_DISC_IMAGE", "PARTYBOARD_NETPLAY_BIND",
             "PARTYBOARD_SESSION_CHILD", "PARTYBOARD_CRASH_DIR", "PARTYBOARD_RELAUNCH_WAIT_HANDLE"}) {
        const char *value = std::getenv(name);
        report << "env " << name << "=" << (value ? value : "(unset)") << "\n";
    }
    if (const char *profile = std::getenv("PARTYBOARD_NETPLAY_TEST_PROFILE")) {
        std::error_code ec;
        const auto size = fs::file_size(utf8_path(profile) / "MemoryCardA.USA.raw", ec);
        report << "save " << (ec ? std::string("missing") : std::to_string(size) + " bytes") << "\n";
    }
    // Free now that the copy before this one has quit, and none of its sockets
    // came along.
    const char *bindIp = std::getenv("PARTYBOARD_NETPLAY_BIND");
    if (valid_port(gamePort)) {
        report << "udp game port " << gamePort << (port_free(bindIp, static_cast<std::uint16_t>(gamePort), SOCK_DGRAM) ? " free" : " BUSY")
               << "\n";
    }
    if (seat == 0) {
        report << "tcp lobby port " << kLobbyPort << (port_free(bindIp, kLobbyPort, SOCK_STREAM) ? " free" : " BUSY") << "\n";
    }
    const std::string text = report.str();
    std::fputs(text.c_str(), stdout);
    std::fflush(stdout);
    const char *folder = std::getenv("PARTYBOARD_TAILNET_RELAUNCH_PROBE");
    if (folder == nullptr || *folder == '\0') {
        return 1;
    }
    std::ofstream out(utf8_path(folder) / ("probe-seat" + std::to_string(seat + 1) + ".txt"), std::ios::trunc);
    out << text;
    return out ? 0 : 1;
}

} // namespace

#else // !PARTYBOARD_TAILNET_SUPPORTED

std::uint16_t local_game_port() { return kGamePort; }
Scanner::~Scanner() = default;
void Scanner::start() {}
bool Scanner::busy() const { return false; }
std::string Scanner::selfIp() const { return {}; }
std::string Scanner::error() const { return "Online play over Tailscale is only available on Mac and Windows."; }
std::vector<HostOffer> Scanner::offers() const { return {}; }
Identity::~Identity() = default;
void Identity::start(const std::string &) {}
bool Identity::ready() const { return false; }
bool Identity::failed() const { return true; }
std::string Identity::build() const { return {}; }
std::string Identity::disc() const { return {}; }
std::string Identity::discPath() const { return {}; }
std::string Identity::error() const { return "Online play over Tailscale is only available on Mac and Windows."; }
std::string Identity::simulation() const { return {}; }
std::string simulation_fingerprint() { return {}; }
std::string simulation_fingerprint_summary() { return "regular build"; }
struct Lobby::Impl {};
Lobby::Lobby() : mImpl(std::make_unique<Impl>()) {}
Lobby::~Lobby() = default;
bool Lobby::host(const std::string &, int, const std::string &, const Identity &, int) { return false; }
bool Lobby::join(const std::string &, const std::string &, const Identity &) { return false; }
void Lobby::leave() {}
bool Lobby::canStart() const { return false; }
int Lobby::taken() const { return 0; }
int Lobby::readyCount() const { return 0; }
bool Lobby::everyoneReady() const { return false; }
std::string Lobby::waitingFor() const { return {}; }
int Lobby::pingMs(int) const { return -1; }
int Lobby::localSeat() const { return 0; }
int Lobby::localPeople() const { return 1; }
void Lobby::setLocalSeat(int, bool, bool, const std::string &) {}
void Lobby::logReadySummary() {}
void Lobby::start() {}
void Lobby::poll() {}
void Lobby::fail(std::string message) { mMessage = std::move(message); mState = State::Failed; }
void Lobby::launch() {}

#endif

// Shared by every platform: what travels in HELLO, and the host's verdict on it.
std::string hello_build_field(const std::string &build, const std::string &simulation)
{
    return simulation.empty() ? build : build + "+" + simulation;
}

BuildMatch match_build(const std::string &hostBuild, const std::string &hostSimulation, const std::string &guestField)
{
    const auto plus = guestField.find('+');
    const std::string guestBuild = guestField.substr(0, plus);
    const std::string guestSimulation = plus == std::string::npos ? std::string() : guestField.substr(plus + 1);
    if (!hostBuild.empty() && guestBuild == hostBuild) {
        return BuildMatch::Identical;
    }
    // Both sides must carry a full fingerprint; a regular build never matches by it.
    if (hostSimulation.size() == 64 && guestSimulation == hostSimulation) {
        return BuildMatch::CrossPlay;
    }
    return BuildMatch::Different;
}

} // namespace partyboard::online::tailnet

extern "C" bool PartyBoard_TailnetLobbySelfTest(void)
{
#if defined(PARTYBOARD_TAILNET_SUPPORTED)
    return partyboard::online::tailnet::self_test();
#else
    return false;
#endif
}

extern "C" int PartyBoard_TailnetLobbyPairTest(int argc, char *argv[])
{
#if defined(PARTYBOARD_TAILNET_SUPPORTED)
    return partyboard::online::tailnet::pair_test(argc, argv);
#else
    (void)argc;
    (void)argv;
    return 1;
#endif
}

extern "C" int PartyBoard_TailnetRelaunchProbe(int argc, char *argv[])
{
#if defined(PARTYBOARD_TAILNET_SUPPORTED)
    return partyboard::online::tailnet::relaunch_probe(argc, argv);
#else
    (void)argc;
    (void)argv;
    return 1;
#endif
}
