// Menu > Send Logs (Windows): zips the newest PartyBoard sessions' logs (the
// folders partyboard.exe writes under %APPDATA%\MarioPartyRD\Party Board\logs,
// see session_log_win.cpp) and any recent PartyBoard crash files Windows kept,
// into Downloads\PartyBoard Logs <date time>.zip, then opens File Explorer with
// the zip selected so a player can send it to whoever is helping. Nothing is
// sent anywhere. When the player has the shared iCloud Drive folder
// "Mario Party 4 (Windows)" (iCloud for Windows), a copy also goes to its
// "Sent Logs" folder, which syncs back to the Mac.
//
// The zip is written here (deflate through the zlib the game already ships,
// zlib1.dll), so there is no PowerShell or other tool involved. Every step
// logs a short line to game.log.
#if defined(_WIN32)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>

#include <zlib.h>

#include <aurora/lib/logging.hpp>

#include "port/send_logs.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;

aurora::Module Log("partyboard::send_logs");

constexpr size_t kSessions = 5;                       // newest launches, this one included
constexpr auto kCrashWindow = std::chrono::hours(72); // Windows crash files this recent
constexpr uint64_t kMaxFileBytes = 512ull << 20;      // larger files are skipped (no zip64 here)
constexpr uint64_t kMaxCrashFileBytes = 64ull << 20;  // full-memory WER dumps are skipped
constexpr uint64_t kMaxArchiveBytes = 0xF0000000ull;  // stay clear of the 4 GB zip limit

std::atomic<bool> gBusy { false };

std::string utf8(const fs::path &path)
{
    const auto text = path.u8string();
    return std::string(text.begin(), text.end());
}

fs::path knownFolder(REFKNOWNFOLDERID id)
{
    PWSTR raw = nullptr;
    fs::path result;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &raw)) && raw) result = raw;
    CoTaskMemFree(raw);
    return result;
}

fs::path envFolder(const wchar_t *name)
{
    const wchar_t *value = _wgetenv(name);
    return value && *value ? fs::path(value) : fs::path();
}

bool isDirectory(const fs::path &path)
{
    std::error_code ec;
    return !path.empty() && fs::is_directory(path, ec);
}

bool containsNoCase(const std::wstring &text, const wchar_t *needle)
{
    std::wstring lower = text;
    for (auto &c : lower) c = static_cast<wchar_t>(std::towlower(c));
    return lower.find(needle) != std::wstring::npos;
}

bool isRecent(const fs::path &path)
{
    std::error_code ec;
    const auto modified = fs::last_write_time(path, ec);
    if (ec) return false;
    return fs::file_time_type::clock::now() - modified < kCrashWindow;
}

// ---- a minimal zip writer (deflate, no zip64) -------------------------------

void put16(std::vector<uint8_t> &out, uint32_t v)
{
    out.push_back(static_cast<uint8_t>(v));
    out.push_back(static_cast<uint8_t>(v >> 8));
}

void put32(std::vector<uint8_t> &out, uint32_t v)
{
    put16(out, v & 0xFFFF);
    put16(out, v >> 16);
}

class ZipWriter {
public:
    explicit ZipWriter(const fs::path &path)
    {
        mFile = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (mFile == INVALID_HANDLE_VALUE) mError = GetLastError();
    }
    ~ZipWriter() { close(); }
    ZipWriter(const ZipWriter &) = delete;
    ZipWriter &operator=(const ZipWriter &) = delete;

    bool opened() const { return mFile != INVALID_HANDLE_VALUE; }
    DWORD openError() const { return mError; }
    size_t count() const { return mEntries.size(); }
    uint64_t bytes() const { return mOffset; }

    // Adds one file, deflated. On failure the archive is left as it was before
    // the call and *error says why. *read is the number of bytes taken from it.
    bool add(const fs::path &source, const std::string &name, uint64_t maxBytes, std::string *error,
        uint64_t *read)
    {
        *read = 0;
        if (mBroken) { *error = "the zip could not be written"; return false; }
        if (mEntries.size() >= 0xFFFF) { *error = "too many files"; return false; }
        if (name.size() >= 0xFFFF) { *error = "name too long"; return false; }
        HANDLE in = CreateFileW(source.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (in == INVALID_HANDLE_VALUE) {
            *error = "could not open it (Windows error " + std::to_string(GetLastError()) + ")";
            return false;
        }
        LARGE_INTEGER size {};
        GetFileSizeEx(in, &size);
        const uint64_t total = static_cast<uint64_t>(size.QuadPart);
        if (total > maxBytes) {
            CloseHandle(in);
            *error = "too large (" + std::to_string(total >> 20) + " MB)";
            return false;
        }
        if (mOffset + total + (64u << 10) > kMaxArchiveBytes) {
            CloseHandle(in);
            *error = "the zip is full";
            return false;
        }

        Entry entry;
        entry.name = name;
        entry.offset = static_cast<uint32_t>(mOffset);
        FILETIME written {}, local {};
        GetFileTime(in, nullptr, nullptr, &written);
        FileTimeToLocalFileTime(&written, &local);
        if (!FileTimeToDosDateTime(&local, &entry.date, &entry.time)) {
            entry.date = (1 << 5) | 1; // 1980-01-01
            entry.time = 0;
        }

        std::vector<uint8_t> header;
        header.reserve(30 + name.size());
        put32(header, 0x04034b50);
        put16(header, 20);     // version needed: deflate
        put16(header, 0x0800); // names are UTF-8
        put16(header, 8);      // deflate
        put16(header, entry.time);
        put16(header, entry.date);
        put32(header, 0); // crc, sizes: filled in below
        put32(header, 0);
        put32(header, 0);
        put16(header, static_cast<uint32_t>(name.size()));
        put16(header, 0);
        header.insert(header.end(), name.begin(), name.end());

        bool ok = write(header.data(), header.size());
        z_stream zs {};
        ok = ok && deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) == Z_OK;
        const bool zsReady = ok;
        std::vector<unsigned char> inBuf(1 << 16), outBuf(1 << 16);
        uLong crc = crc32(0, nullptr, 0);
        uint64_t packed = 0;
        // Only what the file held when we opened it: game.log keeps growing
        // while we read it (this very feature logs to it).
        uint64_t remaining = total;
        while (ok) {
            DWORD got = 0;
            const DWORD want = static_cast<DWORD>(std::min<uint64_t>(remaining, inBuf.size()));
            if (want > 0 && !ReadFile(in, inBuf.data(), want, &got, nullptr)) {
                *error = "could not read it (Windows error " + std::to_string(GetLastError()) + ")";
                ok = false;
                break;
            }
            remaining -= got;
            const bool last = got == 0 || remaining == 0;
            crc = crc32(crc, inBuf.data(), got);
            *read += got;
            zs.next_in = inBuf.data();
            zs.avail_in = got;
            int result = Z_OK;
            do {
                zs.next_out = outBuf.data();
                zs.avail_out = static_cast<uInt>(outBuf.size());
                result = deflate(&zs, last ? Z_FINISH : Z_NO_FLUSH);
                if (result == Z_STREAM_ERROR) { ok = false; break; }
                const size_t have = outBuf.size() - zs.avail_out;
                if (have > 0 && !write(outBuf.data(), have)) { ok = false; break; }
                packed += have;
            } while (zs.avail_out == 0);
            if (last || !ok) break;
        }
        if (zsReady) deflateEnd(&zs);
        CloseHandle(in);

        entry.crc = static_cast<uint32_t>(crc);
        entry.packed = static_cast<uint32_t>(packed);
        entry.size = static_cast<uint32_t>(*read);
        if (ok) {
            std::vector<uint8_t> sizes;
            put32(sizes, entry.crc);
            put32(sizes, entry.packed);
            put32(sizes, entry.size);
            ok = writeAt(entry.offset + 14, sizes.data(), sizes.size());
        }
        if (!ok) {
            if (error->empty()) *error = "could not write it into the zip";
            rewindTo(entry.offset);
            return false;
        }
        mEntries.push_back(std::move(entry));
        return true;
    }

    // Writes the central directory. False if anything along the way failed.
    bool finish()
    {
        if (!opened() || mBroken) return false;
        const uint64_t start = mOffset;
        std::vector<uint8_t> dir;
        for (const auto &e : mEntries) {
            put32(dir, 0x02014b50);
            put16(dir, 20); // made by: MS-DOS/Windows, zip 2.0
            put16(dir, 20);
            put16(dir, 0x0800);
            put16(dir, 8);
            put16(dir, e.time);
            put16(dir, e.date);
            put32(dir, e.crc);
            put32(dir, e.packed);
            put32(dir, e.size);
            put16(dir, static_cast<uint32_t>(e.name.size()));
            put16(dir, 0); // extra
            put16(dir, 0); // comment
            put16(dir, 0); // disk
            put16(dir, 0); // internal attributes
            put32(dir, FILE_ATTRIBUTE_ARCHIVE);
            put32(dir, e.offset);
            dir.insert(dir.end(), e.name.begin(), e.name.end());
        }
        const uint64_t dirSize = dir.size();
        put32(dir, 0x06054b50);
        put16(dir, 0);
        put16(dir, 0);
        put16(dir, static_cast<uint32_t>(mEntries.size()));
        put16(dir, static_cast<uint32_t>(mEntries.size()));
        put32(dir, static_cast<uint32_t>(dirSize));
        put32(dir, static_cast<uint32_t>(start));
        put16(dir, 0);
        const bool ok = write(dir.data(), dir.size()) && SetEndOfFile(mFile) && FlushFileBuffers(mFile);
        close();
        return ok;
    }

    void close()
    {
        if (mFile != INVALID_HANDLE_VALUE) CloseHandle(mFile);
        mFile = INVALID_HANDLE_VALUE;
    }

private:
    struct Entry {
        std::string name;
        uint32_t crc = 0, packed = 0, size = 0, offset = 0;
        WORD time = 0, date = 0;
    };

    bool write(const void *data, size_t size)
    {
        const auto *p = static_cast<const uint8_t *>(data);
        while (size > 0) {
            DWORD written = 0;
            const DWORD chunk = static_cast<DWORD>(std::min<size_t>(size, 1u << 20));
            if (!WriteFile(mFile, p, chunk, &written, nullptr) || written == 0) return false;
            p += written;
            size -= written;
            mOffset += written;
        }
        return true;
    }

    bool seek(uint64_t at)
    {
        LARGE_INTEGER pos;
        pos.QuadPart = static_cast<LONGLONG>(at);
        return SetFilePointerEx(mFile, pos, nullptr, FILE_BEGIN) != 0;
    }

    bool writeAt(uint64_t at, const void *data, size_t size)
    {
        const uint64_t end = mOffset;
        if (!seek(at)) return false;
        mOffset = at;
        const bool ok = write(data, size);
        mOffset = end;
        return seek(end) && ok;
    }

    void rewindTo(uint64_t at)
    {
        if (seek(at) && SetEndOfFile(mFile)) {
            mOffset = at;
        } else {
            mBroken = true;
        }
    }

    HANDLE mFile = INVALID_HANDLE_VALUE;
    DWORD mError = 0;
    uint64_t mOffset = 0;
    bool mBroken = false;
    std::vector<Entry> mEntries;
};

// ---- what goes in ------------------------------------------------------------

struct Item {
    fs::path source;
    std::string name; // inside the zip's top folder, '/'-separated
    uint64_t maxBytes = kMaxFileBytes;
};

struct Options {
    fs::path logsRoot;  // session folders
    fs::path outputDir; // Downloads
    fs::path shared;    // iCloud Drive\Mario Party 4 (Windows); may be missing
    struct CrashFolder {
        fs::path folder;
        std::string label;     // folder name under "Crash reports/" in the zip
        bool crashFilesOnly;   // only crash-* / stack-usage-* (beside partyboard.exe),
                               // otherwise anything named after PartyBoard (Windows' own)
    };
    std::vector<CrashFolder> crashFolders;
    std::wstring baseName; // "PartyBoard Logs YYYY-MM-DD HH-MM"
    bool reveal = true;
};

struct Result {
    bool ok = false;
    fs::path zip;
    fs::path sharedCopy;
    std::vector<std::string> sessions;
    size_t files = 0;
    size_t skipped = 0;
    uint64_t readBytes = 0;
    uint64_t zipBytes = 0;
};

void addTree(std::vector<Item> &items, const fs::path &folder, const std::string &prefix, uint64_t maxBytes)
{
    std::error_code ec;
    std::vector<fs::path> files;
    for (fs::recursive_directory_iterator it(folder, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file(ec)) files.push_back(it->path());
    }
    std::sort(files.begin(), files.end());
    for (const auto &file : files) {
        const auto relative = file.lexically_relative(folder);
        items.push_back({ file, prefix + utf8(relative.generic_wstring()), maxBytes });
    }
}

// The newest session folders, oldest first (names are start times).
std::vector<fs::path> newestSessions(const fs::path &root)
{
    std::error_code ec;
    std::vector<fs::path> sessions;
    for (const auto &entry : fs::directory_iterator(root, ec)) {
        if (entry.is_directory(ec)) sessions.push_back(entry.path());
    }
    std::sort(sessions.begin(), sessions.end());
    if (sessions.size() > kSessions) sessions.erase(sessions.begin(), sessions.end() - kSessions);
    return sessions;
}

// PartyBoard crash files Windows itself kept (Windows Error Reporting, local
// crash dumps) and crash files a run without a session folder left beside
// partyboard.exe, from the last three days; the Mac build adds its
// DiagnosticReports the same way.
void addRecentCrashFiles(std::vector<Item> &items, const Options &options)
{
    std::error_code ec;
    for (const auto &crash : options.crashFolders) {
        if (!isDirectory(crash.folder)) continue;
        for (const auto &entry : fs::directory_iterator(crash.folder, fs::directory_options::skip_permission_denied, ec)) {
            const std::wstring name = entry.path().filename().wstring();
            const bool ours = crash.crashFilesOnly
                ? name.rfind(L"crash-", 0) == 0 || name.rfind(L"stack-usage-", 0) == 0
                : containsNoCase(name, L"partyboard");
            if (!ours || !isRecent(entry.path())) continue;
            const std::string prefix = "Crash reports/" + crash.label + "/";
            if (entry.is_directory(ec)) {
                addTree(items, entry.path(), prefix + utf8(entry.path().filename()) + "/", kMaxCrashFileBytes);
            } else if (entry.is_regular_file(ec)) {
                items.push_back({ entry.path(), prefix + utf8(entry.path().filename()), kMaxCrashFileBytes });
            }
        }
    }
}

bool revealInExplorer(const fs::path &file, std::string *how)
{
    // Let File Explorer come to the front over the game.
    AllowSetForegroundWindow(ASFW_ANY);
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    HRESULT hr = E_FAIL;
    if (PIDLIST_ABSOLUTE item = ILCreateFromPathW(file.c_str())) {
        hr = SHOpenFolderAndSelectItems(item, 0, nullptr, 0);
        ILFree(item);
    }
    if (SUCCEEDED(init)) CoUninitialize();
    if (SUCCEEDED(hr)) {
        *how = "SHOpenFolderAndSelectItems";
        return true;
    }
    char code[16];
    std::snprintf(code, sizeof(code), "0x%08lX", static_cast<unsigned long>(hr));
    const std::wstring args = L"/select,\"" + file.wstring() + L"\"";
    const auto result = reinterpret_cast<INT_PTR>(
        ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL));
    *how = std::string("SHOpenFolderAndSelectItems failed (") + code + "), explorer /select "
        + (result > 32 ? "started" : "failed too (" + std::to_string(result) + ")");
    return result > 32;
}

Result buildAndShare(const Options &options)
{
    Result result;
    const std::string top = utf8(options.baseName) + "/";

    std::vector<Item> items;
    for (const auto &session : newestSessions(options.logsRoot)) {
        const std::string name = utf8(session.filename());
        result.sessions.push_back(name);
        addTree(items, session, top + name + "/", kMaxFileBytes);
    }
    const size_t sessionFiles = items.size();
    addRecentCrashFiles(items, options);
    for (auto it = items.begin() + sessionFiles; it != items.end(); ++it) it->name = top + it->name;

    std::string list;
    for (const auto &name : result.sessions) list += (list.empty() ? "" : ", ") + name;
    Log.info("Send Logs: {} session folder(s) from {}: {}", result.sessions.size(), utf8(options.logsRoot),
        list.empty() ? "(none)" : list);
    if (items.size() > sessionFiles) {
        Log.info("Send Logs: also {} recent Windows crash file(s)", items.size() - sessionFiles);
    }
    if (items.empty()) {
        Log.warn("Send Logs: nothing to zip (no session logs found)");
        return result;
    }

    std::error_code ec;
    if (!isDirectory(options.outputDir)) fs::create_directories(options.outputDir, ec);
    result.zip = options.outputDir / (options.baseName + L".zip");
    const fs::path partial = options.outputDir / (options.baseName + L".zip.part");
    {
        ZipWriter zip(partial);
        if (!zip.opened()) {
            Log.warn("Send Logs: FAILED: could not create {} (Windows error {})", utf8(partial), zip.openError());
            return result;
        }
        for (const auto &item : items) {
            std::string error;
            uint64_t read = 0;
            if (zip.add(item.source, item.name, item.maxBytes, &error, &read)) {
                result.readBytes += read;
            } else {
                result.skipped++;
                Log.warn("Send Logs: skipped {}: {}", utf8(item.source), error);
            }
        }
        result.files = zip.count();
        const bool finished = zip.finish();
        result.zipBytes = zip.bytes();
        if (!finished || result.files == 0) {
            Log.warn("Send Logs: FAILED: could not finish {}", utf8(partial));
            fs::remove(partial, ec);
            return result;
        }
    }
    if (!MoveFileExW(partial.c_str(), result.zip.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        Log.warn("Send Logs: FAILED: could not rename the zip to {} (Windows error {})", utf8(result.zip),
            GetLastError());
        fs::remove(partial, ec);
        return result;
    }
    result.ok = true;
    Log.info("Send Logs: wrote {} ({} files, {} KB, from {} KB of logs{})", utf8(result.zip), result.files,
        (result.zipBytes + 1023) / 1024, (result.readBytes + 1023) / 1024,
        result.skipped ? ", " + std::to_string(result.skipped) + " skipped" : std::string());

    if (isDirectory(options.shared)) {
        // Never pruned. Hidden when the game makes it (only then, so a player
        // who shows it again keeps it shown); iCloud does not carry the
        // attribute to the Mac, which hides its copy separately.
        const fs::path sent = options.shared / "Sent Logs";
        const bool existed = isDirectory(sent);
        fs::create_directories(sent, ec);
        if (!ec && !existed) {
            const DWORD attributes = GetFileAttributesW(sent.c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES
                && SetFileAttributesW(sent.c_str(), attributes | FILE_ATTRIBUTE_HIDDEN)) {
                Log.info("Send Logs: made the hidden iCloud Drive folder {}", utf8(sent));
            } else {
                Log.warn("Send Logs: made {} but could not hide it (Windows error {})", utf8(sent), GetLastError());
            }
        }
        const fs::path copy = sent / result.zip.filename();
        if (!ec && CopyFileW(result.zip.c_str(), copy.c_str(), FALSE)) {
            result.sharedCopy = copy;
            Log.info("Send Logs: copied to iCloud Drive: {}", utf8(copy));
        } else {
            Log.warn("Send Logs: could not copy to iCloud Drive {} (Windows error {})", utf8(copy), GetLastError());
        }
    } else {
        Log.info("Send Logs: no iCloud Drive folder {}; zip kept in Downloads only", utf8(options.shared));
    }

    if (options.reveal) {
        std::string how;
        if (revealInExplorer(result.zip, &how)) {
            Log.info("Send Logs: File Explorer opened with the zip selected ({})", how);
        } else {
            Log.warn("Send Logs: could not open File Explorer: {}", how);
        }
    }
    return result;
}

std::wstring archiveName()
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t name[64];
    std::swprintf(name, 64, L"PartyBoard Logs %04u-%02u-%02u %02u-%02u", t.wYear, t.wMonth, t.wDay, t.wHour,
        t.wMinute);
    return name;
}

Options realOptions()
{
    Options options;
    const fs::path appData = knownFolder(FOLDERID_RoamingAppData);
    if (!appData.empty()) options.logsRoot = appData / "MarioPartyRD" / "Party Board" / "logs";
    options.outputDir = knownFolder(FOLDERID_Downloads);
    const fs::path profile = envFolder(L"USERPROFILE");
    if (!isDirectory(options.outputDir) && !profile.empty()) options.outputDir = profile / "Downloads";
    if (!profile.empty()) options.shared = profile / "iCloudDrive" / "Mario Party 4 (Windows)";
    const fs::path local = knownFolder(FOLDERID_LocalAppData);
    const fs::path programData = knownFolder(FOLDERID_ProgramData);
    for (const auto &[root, label] : { std::pair { local, "Windows Error Reporting" },
             std::pair { programData, "Windows Error Reporting (all users)" } }) {
        if (root.empty()) continue;
        options.crashFolders.push_back({ root / "Microsoft" / "Windows" / "WER" / "ReportArchive", label, false });
        options.crashFolders.push_back({ root / "Microsoft" / "Windows" / "WER" / "ReportQueue", label, false });
    }
    if (!local.empty()) options.crashFolders.push_back({ local / "CrashDumps", "CrashDumps", false });
    wchar_t exe[MAX_PATH] = {};
    const DWORD exeLength = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (exeLength > 0 && exeLength < MAX_PATH) {
        options.crashFolders.push_back({ fs::path(exe).parent_path(), "beside partyboard.exe", true });
    }
    options.baseName = archiveName();
    return options;
}

// ---- self-test -------------------------------------------------------------

bool writeFile(const fs::path &path, const std::string &data)
{
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    FILE *f = _wfopen(path.c_str(), L"wb");
    if (!f) return false;
    const bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    return std::fclose(f) == 0 && ok;
}

bool readFile(const fs::path &path, std::string *out)
{
    FILE *f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    out->clear();
    char buf[1 << 16];
    size_t got;
    while ((got = std::fread(buf, 1, sizeof(buf), f)) > 0) out->append(buf, got);
    std::fclose(f);
    return true;
}

uint32_t le16(const std::string &s, size_t at)
{
    return static_cast<uint8_t>(s[at]) | (static_cast<uint8_t>(s[at + 1]) << 8);
}

uint32_t le32(const std::string &s, size_t at) { return le16(s, at) | (le16(s, at + 2) << 16); }

struct Unzipped {
    std::string name;
    std::string data;
    uint32_t crc = 0;
};

// Reads every entry back through the central directory, inflating each one
// and checking its CRC, as an unzip tool would.
bool readZip(const fs::path &path, std::vector<Unzipped> *entries, std::string *why)
{
    std::string zip;
    if (!readFile(path, &zip) || zip.size() < 22) { *why = "unreadable"; return false; }
    const size_t eocd = zip.size() - 22;
    if (le32(zip, eocd) != 0x06054b50) { *why = "no end record"; return false; }
    const uint32_t count = le16(zip, eocd + 10);
    size_t at = le32(zip, eocd + 16);
    for (uint32_t i = 0; i < count; i++) {
        if (at + 46 > zip.size() || le32(zip, at) != 0x02014b50) { *why = "bad directory entry"; return false; }
        Unzipped e;
        const uint32_t method = le16(zip, at + 10), crc = le32(zip, at + 16), packed = le32(zip, at + 20),
                       size = le32(zip, at + 24), nameLen = le16(zip, at + 28), extra = le16(zip, at + 30),
                       comment = le16(zip, at + 32), local = le32(zip, at + 42);
        e.name = zip.substr(at + 46, nameLen);
        at += 46 + nameLen + extra + comment;
        if (method != 8 || local + 30 > zip.size() || le32(zip, local) != 0x04034b50
            || le32(zip, local + 14) != crc || le32(zip, local + 18) != packed || le32(zip, local + 22) != size
            || zip.compare(local + 30, nameLen, e.name) != 0) {
            *why = "local header does not match for " + e.name;
            return false;
        }
        const size_t dataAt = local + 30 + le16(zip, local + 26) + le16(zip, local + 28);
        e.data.resize(size);
        z_stream zs {};
        inflateInit2(&zs, -MAX_WBITS);
        zs.next_in = reinterpret_cast<Bytef *>(zip.data() + dataAt);
        zs.avail_in = packed;
        zs.next_out = reinterpret_cast<Bytef *>(e.data.data());
        zs.avail_out = size;
        const int r = inflate(&zs, Z_FINISH);
        const bool full = zs.total_out == size;
        inflateEnd(&zs);
        e.crc = static_cast<uint32_t>(crc32(0, reinterpret_cast<const Bytef *>(e.data.data()), size));
        if (r != Z_STREAM_END || !full || e.crc != crc) { *why = "data or CRC wrong for " + e.name; return false; }
        entries->push_back(std::move(e));
    }
    return true;
}

} // namespace

extern "C" void PartyBoard_SendLogs(void)
{
    if (gBusy.exchange(true)) {
        Log.info("Send Logs: already running; ignoring this press");
        return;
    }
    Log.info("Send Logs: started");
    std::thread([] {
        const Result result = buildAndShare(realOptions());
        if (!result.ok) Log.warn("Send Logs: no zip was made");
        gBusy = false;
    }).detach();
}

// partyboard.exe --send-logs-self-test: makes seven fake sessions (one with a
// crash, one whose game.log is still open for writing, as the supervisor holds
// it) plus a fake crash folder in %TEMP%, runs the real archive code against
// them with a fake Downloads and iCloud folder, then reads the zip back and
// compares every byte. Also prints which real folders Send Logs would use,
// without touching them.
extern "C" bool PartyBoard_SendLogsRunSelfTest(void)
{
    bool pass = true;
    auto check = [&](bool ok, const std::string &what) {
        std::printf("send-logs self-test: %s %s\n", ok ? "ok  " : "FAIL", what.c_str());
        pass = pass && ok;
    };

    std::error_code ec;
    const fs::path root = fs::temp_directory_path(ec) / ("PartyBoard send-logs self-test "
        + std::to_string(GetCurrentProcessId()));
    fs::remove_all(root, ec);
    Options options;
    options.logsRoot = root / "logs";
    options.outputDir = root / "Downloads";
    options.shared = root / "iCloudDrive" / "Mario Party 4 (Windows)";
    options.crashFolders.push_back({ root / "WER", "Windows Error Reporting", false });
    options.crashFolders.push_back({ root / "exe", "beside partyboard.exe", true });
    options.baseName = L"PartyBoard Logs 2026-01-07 12-00";
    options.reveal = false;
    fs::create_directories(options.shared, ec);

    // Seven sessions; the newest five must be in the zip.
    std::vector<std::pair<std::string, std::string>> expected; // name in zip -> content
    const std::string top = "PartyBoard Logs 2026-01-07 12-00/";
    uint32_t seed = 12345;
    for (int day = 1; day <= 7; day++) {
        char name[32];
        std::snprintf(name, sizeof(name), "2026-01-0%d_20-00-00", day);
        std::string log;
        for (int line = 0; line < 4000; line++) {
            log += "12:00:00.000 [INFO | partyboard::main] session " + std::to_string(day) + " line "
                + std::to_string(line) + "\r\n";
        }
        bool ok = writeFile(options.logsRoot / name / "game.log", log);
        if (day >= 3) expected.push_back({ top + name + "/game.log", log });
        if (day == 5) {
            std::string dump(300000, '\0');
            for (auto &c : dump) c = static_cast<char>((seed = seed * 1103515245u + 12345u) >> 16);
            ok = ok && writeFile(options.logsRoot / name / "crash-pid-1-x.dmp", dump);
            ok = ok && writeFile(options.logsRoot / name / "crash-report-pid-1-x.txt", "crash report\r\n");
            ok = ok && writeFile(options.logsRoot / name / "empty.txt", "");
            expected.push_back({ top + name + "/crash-pid-1-x.dmp", dump });
            expected.push_back({ top + name + "/crash-report-pid-1-x.txt", "crash report\r\n" });
            expected.push_back({ top + name + "/empty.txt", "" });
        }
        check(ok, std::string("made fake session ") + name);
    }
    const std::string wer = "Version=1\r\nEventType=APPCRASH\r\n";
    check(writeFile(root / "WER" / "AppCrash_partyboard.exe_1234" / "Report.wer", wer)
            && writeFile(root / "WER" / "AppCrash_notepad.exe_5678" / "Report.wer", wer),
        "made fake Windows Error Reporting folders");
    expected.push_back({ top + "Crash reports/Windows Error Reporting/AppCrash_partyboard.exe_1234/Report.wer", wer });
    // Beside the exe only crash files count, not partyboard.exe itself.
    check(writeFile(root / "exe" / "partyboard.exe", "MZ") && writeFile(root / "exe" / "partyboard.pdb", "pdb")
            && writeFile(root / "exe" / "crash-report-pid-9.txt", "loose crash\r\n"),
        "made a fake program folder with a loose crash report");
    expected.push_back({ top + "Crash reports/beside partyboard.exe/crash-report-pid-9.txt", "loose crash\r\n" });

    // The newest game.log stays open for writing, the way the supervisor holds it.
    const fs::path liveLog = options.logsRoot / "2026-01-07_20-00-00" / "game.log";
    HANDLE live = CreateFileW(liveLog.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    check(live != INVALID_HANDLE_VALUE, "held the newest game.log open for writing");

    const Result result = buildAndShare(options);
    if (live != INVALID_HANDLE_VALUE) CloseHandle(live);
    check(result.ok, "zip written: " + utf8(result.zip));
    check(result.sessions.size() == 5 && result.sessions.front() == "2026-01-03_20-00-00"
            && result.sessions.back() == "2026-01-07_20-00-00",
        "picked the newest 5 sessions (" + std::to_string(result.sessions.size()) + ")");
    check(result.skipped == 0, "no file skipped");

    std::vector<Unzipped> entries;
    std::string why;
    const bool readable = result.ok && readZip(result.zip, &entries, &why);
    check(readable, "zip reads back, every entry inflates with the right CRC" + (why.empty() ? "" : ": " + why));
    check(entries.size() == expected.size(), "entry count " + std::to_string(entries.size()) + " (expected "
        + std::to_string(expected.size()) + ")");
    size_t matched = 0;
    for (const auto &[name, data] : expected) {
        const auto it = std::find_if(entries.begin(), entries.end(), [&](const Unzipped &e) { return e.name == name; });
        if (it != entries.end() && it->data == data) {
            matched++;
        } else {
            check(false, "missing or different: " + name);
        }
    }
    check(matched == expected.size(), "every expected file is in the zip byte for byte");

    std::string zipBytes, copyBytes;
    check(!result.sharedCopy.empty() && readFile(result.zip, &zipBytes) && readFile(result.sharedCopy, &copyBytes)
            && zipBytes == copyBytes,
        "iCloud Drive copy is identical: " + utf8(result.sharedCopy));
    check(!fs::exists(options.outputDir / (options.baseName + L".zip.part"), ec), "no partial file left");
    const fs::path sentDir = options.shared / "Sent Logs";
    const DWORD sentAttributes = GetFileAttributesW(sentDir.c_str());
    check(sentAttributes != INVALID_FILE_ATTRIBUTES && (sentAttributes & FILE_ATTRIBUTE_HIDDEN) != 0,
        "the new Sent Logs folder is hidden");

    // Shown again by the player: stays shown; the older zip is kept (never pruned).
    SetFileAttributesW(sentDir.c_str(), sentAttributes & ~static_cast<DWORD>(FILE_ATTRIBUTE_HIDDEN));
    options.baseName = L"PartyBoard Logs 2026-01-07 12-02";
    const Result again = buildAndShare(options);
    const DWORD againAttributes = GetFileAttributesW(sentDir.c_str());
    check(again.ok && !again.sharedCopy.empty() && (againAttributes & FILE_ATTRIBUTE_HIDDEN) == 0
            && fs::exists(sentDir / "PartyBoard Logs 2026-01-07 12-00.zip", ec)
            && fs::exists(sentDir / "PartyBoard Logs 2026-01-07 12-02.zip", ec),
        "an existing Sent Logs folder is left as it is and older zips are kept");
    std::printf("send-logs self-test: zip %llu bytes from %llu bytes of input\n",
        static_cast<unsigned long long>(result.zipBytes), static_cast<unsigned long long>(result.readBytes));

    // Without an iCloud folder: zip only, no error.
    fs::remove_all(root / "iCloudDrive", ec);
    options.baseName = L"PartyBoard Logs 2026-01-07 12-01";
    const Result noShare = buildAndShare(options);
    check(noShare.ok && noShare.sharedCopy.empty() && !fs::exists(root / "iCloudDrive", ec),
        "without iCloud Drive: zip made, nothing copied, no folder created");

    // The real folders, read only.
    const Options real = realOptions();
    std::printf("send-logs self-test: real logs folder %s (%zu session folder(s), newest %zu would be zipped)\n",
        utf8(real.logsRoot).c_str(), isDirectory(real.logsRoot) ? newestSessions(real.logsRoot).size() : 0,
        kSessions);
    std::printf("send-logs self-test: real Downloads %s (%s)\n", utf8(real.outputDir).c_str(),
        isDirectory(real.outputDir) ? "exists" : "MISSING");
    std::printf("send-logs self-test: real iCloud folder %s (%s)\n", utf8(real.shared).c_str(),
        isDirectory(real.shared) ? "exists: a copy would go to Sent Logs" : "absent: Downloads only");
    std::printf("send-logs self-test: real zip name would be \"%s.zip\"\n", utf8(real.baseName).c_str());

    // PARTYBOARD_SEND_LOGS_SELFTEST_KEEP=1 keeps the zips, to open them with
    // other tools (File Explorer, tar, a Mac).
    if (pass && std::getenv("PARTYBOARD_SEND_LOGS_SELFTEST_KEEP") == nullptr) {
        fs::remove_all(root, ec);
    } else {
        std::printf("send-logs self-test: files kept in %s\n", utf8(root).c_str());
    }
    std::printf("SELFTEST send-logs %s\n", pass ? "PASS" : "FAIL");
    std::fflush(stdout);
    return pass;
}

#endif // _WIN32
