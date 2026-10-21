// Saving and continuing an online party (docs/ONLINE_CONTINUE.md): the game side.
//
// The online game makes the offline party save at every results screen, in
// memory (saveload.c SLOnlinePartySave), and hands it here; it is written to the
// "Online Party" folder with a description (party.json). A continue session
// (--netplay-continue <hash>) is given the host's party save by the lobby in the
// online profile; it is checked before the game starts and handed to mode select
// once (saveload.c SLOnlinePartyContinueLoad). Nothing here changes game state.

#include "port/online_party.h"

#include <filesystem> // port/main.h declares a std::filesystem::path global.
#include "port/main.h"
#include "port/netplay_runtime.h"

extern "C" {
#include "game/gamework_data.h"
}

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace partyboard::online::party {
namespace {

std::vector<unsigned char> gContinue;
std::uint64_t gContinueHash = 0;
bool gArmed = false;
bool gHandedOut = false;
// --netplay-continue-map: new seat per saved seat, computer-player flags.
int gMapSeats = 0;
int gMapNew[8] = {};
bool gMapCom[8] = {};

fs::path env_path(const char *name)
{
#if defined(_WIN32)
    std::wstring wide(name, name + std::char_traits<char>::length(name));
    if (const wchar_t *value = _wgetenv(wide.c_str()); value != nullptr && *value != L'\0') {
        return fs::path(value);
    }
    return {};
#else
    const char *value = std::getenv(name);
    return value != nullptr && *value != '\0' ? fs::path(value) : fs::path();
#endif
}

std::string text_of(const fs::path &path)
{
#if defined(_WIN32)
    const auto text = path.u8string();
    return std::string(text.begin(), text.end());
#else
    return path.string();
#endif
}

// The lobby says where (PARTYBOARD_ONLINE_PARTY_DIR: the player's own profile);
// a session started without the lobby keeps it in its own profile.
fs::path store_dir()
{
    if (auto dir = env_path("PARTYBOARD_ONLINE_PARTY_DIR"); !dir.empty()) {
        return dir;
    }
    return PartyBoard_ConfigPath / "Online Party";
}

const char *character_name(int character)
{
    static const char *names[8] = {"Mario", "Luigi", "Peach", "Yoshi", "Wario", "DK", "Daisy", "Waluigi"};
    return character >= 0 && character < 8 ? names[character] : "?";
}

bool write_atomically(const fs::path &file, const void *data, std::size_t size)
{
    std::error_code ec;
    const fs::path temp = file.parent_path() / (file.filename().string() + ".new");
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out.write(static_cast<const char *>(data), static_cast<std::streamsize>(size));
        if (!out) {
            return false;
        }
    }
    fs::rename(temp, file, ec);
    if (ec) {
        // Windows: rename onto an existing file can fail; replace it.
        fs::remove(file, ec);
        fs::rename(temp, file, ec);
    }
    return !ec;
}

std::string seats_text(int first, int count)
{
    return count > 1 ? std::to_string(first + 1) + "-" + std::to_string(first + count) : std::to_string(first + 1);
}

} // namespace

std::uint64_t save_hash(const void *data, std::size_t size)
{
    std::uint64_t h = 14695981039346656037ull;
    const auto *p = static_cast<const unsigned char *>(data);
    for (std::size_t i = 0; i < size; ++i) {
        h = (h ^ p[i]) * 1099511628211ull;
    }
    return h;
}

std::string hash_hex(std::uint64_t hash)
{
    char text[17];
    std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(hash));
    return text;
}

bool arm_continue(const std::string &hashHex)
{
    char *end = nullptr;
    const std::uint64_t expected = std::strtoull(hashHex.c_str(), &end, 16);
    fs::path file = env_path("PARTYBOARD_ONLINE_CONTINUE_FILE");
    if (file.empty()) {
        if (auto profile = env_path("PARTYBOARD_NETPLAY_TEST_PROFILE"); !profile.empty()) {
            file = profile / "online-continue.sav";
        }
    }
    const auto refuse = [&](const std::string &why) {
        std::fprintf(stdout, "[netplay] continue refused: %s; this copy does not start.\n", why.c_str());
        std::fprintf(stderr, "Netplay: --netplay-continue: %s.\n", why.c_str());
        std::fflush(stdout);
        return false;
    };
    if (hashHex.size() != 16 || end == nullptr || *end != '\0') {
        return refuse("--netplay-continue needs the party save's 16-digit hash, got '" + hashHex + "'");
    }
    if (file.empty()) {
        return refuse("no party save to continue (no online profile, no PARTYBOARD_ONLINE_CONTINUE_FILE)");
    }
    std::ifstream in(file, std::ios::binary);
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (!in && bytes.empty()) {
        return refuse("the party save " + text_of(file) + " is missing");
    }
    if (bytes.size() != kSaveBytes) {
        return refuse("the party save " + text_of(file) + " has " + std::to_string(bytes.size()) + " bytes, not "
            + std::to_string(kSaveBytes));
    }
    const std::uint64_t actual = save_hash(bytes.data(), bytes.size());
    if (actual != expected) {
        return refuse("the party save " + text_of(file) + " is a different save (hash " + hash_hex(actual) + ", the session continues "
            + hashHex + ")");
    }
    gContinue = std::move(bytes);
    gContinueHash = actual;
    gArmed = true;
    std::fprintf(stdout, "[netplay] continue: this session continues the saved online party %s (file %s); every copy checked the same save\n",
        hashHex.c_str(), text_of(file).c_str());
    std::fflush(stdout);
    return true;
}

bool continuing() { return gArmed; }

bool arm_continue_map(const std::string &map)
{
    int seats = 0;
    bool used[8] = {};
    std::string text = "[netplay] continue: seat map:";
    for (std::size_t at = 0; at <= map.size();) {
        const std::size_t comma = std::min(map.find(',', at), map.size());
        std::string item = map.substr(at, comma - at);
        const bool com = !item.empty() && item.back() == '*';
        if (com) {
            item.pop_back();
        }
        const int seat = item.size() == 1 && item[0] >= '0' && item[0] <= '7' ? item[0] - '0' : -1;
        if (seats >= 8 || seat < 0 || used[seat]) {
            std::fprintf(stdout, "[netplay] continue refused: --netplay-continue-map '%s' is not a seat map; this copy does not start.\n",
                map.c_str());
            std::fflush(stdout);
            return false;
        }
        used[seat] = true;
        gMapNew[seats] = seat;
        gMapCom[seats] = com;
        text += " saved seat " + std::to_string(seats + 1) + " -> " + (com ? "computer player on port " : "seat ") + std::to_string(seat + 1) + ";";
        ++seats;
        at = comma + 1;
    }
    for (int s = 0; s < seats; ++s) {
        if (!used[s]) {
            std::fprintf(stdout, "[netplay] continue refused: --netplay-continue-map '%s' skips seat %d; this copy does not start.\n",
                map.c_str(), s + 1);
            std::fflush(stdout);
            return false;
        }
    }
    gMapSeats = seats;
    std::fprintf(stdout, "%s\n", text.c_str());
    std::fflush(stdout);
    return true;
}

} // namespace partyboard::online::party

using namespace partyboard::online::party;

extern "C" int PartyBoard_OnlinePartyContinueSave(unsigned char *buf, unsigned size)
{
    if (!gArmed || gHandedOut || buf == nullptr || size != gContinue.size()) {
        return 0;
    }
    // Once: a later return to mode select (after the continued party ends) starts fresh.
    gHandedOut = true;
    std::memcpy(buf, gContinue.data(), gContinue.size());
    return 1;
}

extern "C" int PartyBoard_OnlinePartyContinueSeat(int oldSeat, int *com)
{
    if (com != nullptr) {
        *com = 0;
    }
    if (oldSeat < 0 || oldSeat >= gMapSeats) {
        return -1;
    }
    if (com != nullptr) {
        *com = gMapCom[oldSeat] ? 1 : 0;
    }
    return gMapNew[oldSeat];
}

extern "C" int PartyBoard_OnlinePartyContinueMapSeats(void) { return gMapSeats; }

extern "C" uint32_t PartyBoard_OnlinePartyContinueMapHash(void)
{
    uint32_t h = 0;
    for (int s = 0; s < gMapSeats; ++s) {
        h = h * 31u + static_cast<uint32_t>(gMapNew[s] * 2 + (gMapCom[s] ? 1 : 0) + 1);
    }
    return h;
}

extern "C" uint32_t PartyBoard_OnlinePartyContinueHash32(void)
{
    return gArmed ? static_cast<uint32_t>(gContinueHash ^ (gContinueHash >> 32)) : 0u;
}

extern "C" void PartyBoard_OnlinePartyStore(const unsigned char *buf, unsigned size, const char *where)
{
    const fs::path dir = store_dir();
    std::error_code ec;
    fs::create_directories(dir, ec);
    const std::string hash = hash_hex(save_hash(buf, size));
    const int localSeat = PartyBoard_NetplayLocalSeat();
    const int localSeats = PartyBoard_NetplayLocalSeatCount();
    const bool eight = GWSystem.eight_players != 0;
    const char *mode = GWSystem.team ? "Tag Battle" : "Battle Royale";

    nlohmann::json info = nlohmann::json::object();
    info["version"] = 1;
    info["finished"] = false;
    info["saveHash"] = hash;
    {
        char when[64];
        const std::time_t now = std::time(nullptr);
        std::tm local {};
#if defined(_WIN32)
        localtime_s(&local, &now);
#else
        localtime_r(&now, &local);
#endif
        std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M", &local);
        info["savedAt"] = when;
    }
    info["party"] = PartyBoard_NetplayParty8() ? 8 : 4;
    info["players"] = PartyBoard_NetplayPlayerCount();
    info["thisSeat"] = localSeat;
    info["thisSeats"] = localSeats;
    info["mode"] = mode;
    info["board"] = static_cast<int>(GWSystem.board);
    info["round"] = static_cast<int>(GWSystem.turn);
    info["rounds"] = static_cast<int>(GWSystem.max_turn);
    info["continuesAt"] = where != nullptr ? where : "";
    // The roster the lobby wrote at launch: who had which seats.
    nlohmann::json computers = nlohmann::json::array();
    if (std::ifstream in(PartyBoard_ConfigPath / "online-session.json"); in) {
        auto session = nlohmann::json::parse(in, nullptr, false);
        if (!session.is_discarded() && session.is_object() && session.contains("computers") && session["computers"].is_array()) {
            computers = session["computers"];
        }
    }
    if (computers.empty()) {
        // Started without the lobby (tests): the seat blocks from the session, no names.
        for (int seat = 0; seat < PartyBoard_NetplayPlayerCount();) {
            int first = seat, count = 1;
            if (!PartyBoard_NetplaySeatComputer(seat, &first, &count) || count < 1) {
                count = 1;
            }
            computers.push_back({{"seat", seat}, {"seats", count}, {"name", ""}, {"computer", ""}, {"id", ""}});
            seat += count;
        }
    }
    // A computer the others voted to continue without (drop vote): its people are computer
    // players in this save, so a continue must not wait for it (docs/ONLINE_CONTINUE.md).
    for (auto &c : computers) {
        const int seat = c.value("seat", -1);
        const bool dropped = seat >= 0 && PartyBoard_NetplaySeatDroppedByVote(seat) != 0;
        c["droppedByVote"] = dropped;
        if (dropped) {
            const int seats = c.value("seats", 1);
            const std::string who = seats > 1 ? "players " + std::to_string(seat + 1) + "-" + std::to_string(seat + seats) : "player " + std::to_string(seat + 1);
            std::fprintf(stdout, "[netplay] party save: %s (%s) replaced by computer players in a drop vote; the save marks this, and a continue gives %s to computer players\n",
                who.c_str(), c.value("name", std::string()).c_str(), seats > 1 ? "those seats" : "that seat");
        }
    }
    info["computers"] = computers;
    nlohmann::json characters = nlohmann::json::array();
    const int count = eight ? 8 : 4;
    for (int i = 0; i < count; ++i) {
        const PlayerState &p = GWPlayer[i];
        // Online seat = device port (docs/ONLINE_8_PLAYERS.md): the slot's own port in the
        // 8-player party (slots move between rounds), its port otherwise. 0 = a computer player.
        const int ownPort = eight ? static_cast<int>(GWPlayerOwnPortGet(&p)) : static_cast<int>(p.port);
        characters.push_back({{"slot", i + 1}, {"character", character_name(p.character)}, {"port", static_cast<int>(p.port) + 1},
            {"onlineSeat", p.com == 0 && ownPort >= 0 ? ownPort + 1 : 0}, {"person", p.com == 0}});
    }
    info["characters"] = characters;

    const bool savedOk = write_atomically(dir / "party.sav", buf, size);
    const std::string text = info.dump(2);
    const bool infoOk = savedOk && write_atomically(dir / "party.json", text.data(), text.size());
    if (!savedOk || !infoOk) {
        std::fprintf(stdout, "[netplay] party save: WARNING: couldn't write %s in %s; this party can't be continued from this %s\n",
            savedOk ? "party.json" : "party.sav", text_of(dir).c_str(),
#if defined(_WIN32)
            "PC"
#else
            "Mac"
#endif
        );
    } else {
        std::fprintf(stdout,
            "[netplay] party save: written %s (%u bytes, hash %s): %s%s, board %d, round %d of %d; %s; this copy is seat%s %s (%s)\n",
            text_of(dir / "party.sav").c_str(), size, hash.c_str(), eight ? "8-player " : "", mode, static_cast<int>(GWSystem.board) + 1,
            static_cast<int>(GWSystem.turn), static_cast<int>(GWSystem.max_turn), where != nullptr ? where : "", localSeats > 1 ? "s" : "",
            seats_text(localSeat, localSeats).c_str(), localSeat == 0 ? "host" : "guest");
    }
    std::fflush(stdout);
}

extern "C" void PartyBoard_OnlinePartyFinished(void)
{
    const fs::path dir = store_dir();
    nlohmann::json info;
    if (std::ifstream in(dir / "party.json"); in) {
        info = nlohmann::json::parse(in, nullptr, false);
    }
    if (info.is_discarded() || !info.is_object()) {
        std::fprintf(stdout, "[netplay] party save: the party is over (final results); %s has no party description to mark\n",
            text_of(dir).c_str());
        std::fflush(stdout);
        return;
    }
    info["finished"] = true;
    const std::string text = info.dump(2);
    const bool ok = write_atomically(dir / "party.json", text.data(), text.size());
    std::fprintf(stdout, "[netplay] party save: the party is over (final results); %s %s\n", text_of(dir).c_str(),
        ok ? "marked finished: it can't be continued any more" : "could NOT be marked finished (WARNING)");
    std::fflush(stdout);
}
