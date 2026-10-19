#pragma once

// Online play over a Tailscale network (macOS and Windows). A host's game opens
// a small lobby on its Tailscale address only; friends' games find it by asking
// each computer in the network, check that everyone runs the same build and the
// same disc file, and at Start every game relaunches straight into PartyBoard's
// direct full-game netplay (--netplay-host/--netplay-peer), the mode the
// two-copy lockstep test exercised. A Mac and a Windows PC are different builds,
// so the lobby turns them away from each other -- unless both are cross-play
// builds made from the same sources with the same options (see
// simulation_fingerprint below).
//
// Tailscale provides the encryption and the reachability: the lobby and the
// game traffic never listen beyond the private network for the lobby, and the
// game's UDP port only accepts the peers named on its command line.
//
// Everyone plays from a copy of the host's memory card, because the game reads
// it at boot and different saves would put the copies out of sync at once.
// That copy lives in a separate "Party Board Online" profile; nobody's real
// saves are touched by an online session.

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace partyboard::online::tailnet {

constexpr std::uint16_t kLobbyPort = 48770;
constexpr std::uint16_t kGamePort = 48780;
// Eight seats for the 8-player party; a normal party seats at most four.
constexpr int kMaxSeats = 8;
constexpr int kNormalPartySeats = 4;
// The lobby's own line protocol: 2 = ready check (LOBBYVER, READYSEAT, READYSTATE,
// PING/PONG). Sent by the joiner before HELLO; a host turns other versions away.
constexpr int kLobbyProtocol = 2;

// Ready check, per seat (docs/ONLINE_MULTISEAT.md): the computer that owns the seat
// says whether a controller is on it and whether that person pressed A.
struct SeatReady {
    bool controller = false;
    bool ready = false;
    std::string device;   // what feeds that controller port, e.g. "controller 'Xbox ...'"
};

// How messages name this computer ("this Mac", "this PC").
#if defined(_WIN32)
inline constexpr const char *kComputer = "PC";
#else
inline constexpr const char *kComputer = "Mac";
#endif

// A game hosting a lobby somewhere in the user's Tailscale network.
struct HostOffer {
    std::string ip;
    std::string device;   // Tailscale machine name, e.g. "jordans-macbook-air"
    std::string name;     // the host's nickname
    int players = 0;
    int joined = 0;
    int party = 4;        // 4 = normal party, 8 = the 8-player party
};

// Finds this computer's Tailscale address and the lobbies hosted in the network.
// Runs on a background thread; the window polls it.
class Scanner {
public:
    ~Scanner();
    void start();
    bool busy() const;
    std::string selfIp() const;
    std::string error() const;
    std::vector<HostOffer> offers() const;

private:
    mutable std::mutex mMutex;
    std::thread mThread;
    bool mBusy = false;
    std::string mSelfIp;
    std::string mError;
    std::vector<HostOffer> mOffers;
};

// Fingerprints of this copy of the game and of the disc file, compared in the
// lobby so mismatched players are turned away before anything goes out of sync.
class Identity {
public:
    ~Identity();
    void start(const std::string &discPath);
    bool ready() const;
    bool failed() const;
    std::string build() const;   // hex SHA-256
    std::string disc() const;
    std::string discPath() const;
    std::string error() const;
    // The cross-play simulation fingerprint (empty for a regular build).
    std::string simulation() const;

private:
    mutable std::mutex mMutex;
    std::thread mThread;
    bool mDone = false;
    std::string mBuild, mDisc, mDiscPath, mError, mSimulation;
};

// Cross-play identity (docs/CROSS_PLATFORM_ONLINE.md, "Lobby"). The byte-exact
// build check above can never pass between a Mac and a Windows PC. A build made
// with the cross-play maths options (PARTYBOARD_NO_FMA + PARTYBOARD_SHARED_MATH)
// also carries a simulation fingerprint: SHA-256 over the hash of every game
// source file, the simulation-relevant build options, the source revision, the
// netplay protocol version and the canonical state-hash version. Regular builds
// have none (empty string), so they only ever match byte-identical copies.
std::string simulation_fingerprint();
// One log line's worth: what went into the fingerprint.
std::string simulation_fingerprint_summary();

// The HELLO line's build field: "<build>" for a regular build,
// "<build>+<simulation>" for a cross-play build (an older host compares the
// whole field and so still turns a cross-play guest away).
std::string hello_build_field(const std::string &build, const std::string &simulation);

// The host's decision on a guest's HELLO build field.
enum class BuildMatch {
    Identical,   // byte-identical binaries
    CrossPlay,   // different binaries, both cross-play, same simulation fingerprint
    Different,   // anything else: turned away
};
BuildMatch match_build(const std::string &hostBuild, const std::string &hostSimulation, const std::string &guestField);

struct Member {
    int seat = 0;   // the computer's first seat
    std::string name;
    std::string ip;
    std::uint16_t gamePort = kGamePort;
    // Several people on one computer (docs/ONLINE_MULTISEAT.md): this computer
    // owns seats seat .. seat + seats - 1. "Mac" or "PC" ("" from an older copy).
    int seats = 1;
    std::string computer;
};

class Lobby {
public:
    enum class State { Idle, Hosting, Joining, Waiting, Starting, Launched, Failed };

    Lobby();
    ~Lobby();

    // party: 4 (normal party, 2-4 people) or 8 (the 8-player party, 2-8 people;
    // the seats nobody takes are computer players in the game).
    // people: how many of them play on this computer (several people on one
    // computer, each with their own controller); the rest come from other computers.
    bool host(const std::string &selfIp, int players, const std::string &name, const Identity &identity, int party = 4,
        int people = 1);
    bool join(const std::string &hostIp, const std::string &name, const Identity &identity, int people = 1);
    void leave();
    // Host only: once every seat is filled.
    bool canStart() const;
    void start();
    // Non-blocking network work; call every frame while the window is open.
    void poll();

    State state() const { return mState; }
    bool isHost() const { return mIsHost; }
    int players() const { return mPlayers; }
    int party() const { return mParty; }
    // Seats taken so far (all computers' people together).
    int taken() const;

    // Ready check. This computer's k-th person (0-based): is a controller on their port,
    // have they pressed A (ready), and what feeds that port. Sent on change.
    void setLocalSeat(int k, bool controller, bool ready, const std::string &device);
    const SeatReady &seatReady(int seat) const { return mReady[static_cast<std::size_t>(seat & 7)]; }
    int readyCount() const;
    bool everyoneReady() const;
    // "PC - Player 8 (no controller), Mac - Player 3 (not ready)"; empty when everyone is ready.
    std::string waitingFor() const;
    // Round trip in ms: a guest to its host (seat ignored), the host to the computer owning `seat`; -1 unknown.
    int pingMs(int seat) const;
    int localSeat() const;
    int localPeople() const;
    const std::vector<Member> &members() const { return mMembers; }
    const std::string &message() const { return mMessage; }

private:
    struct Impl;
    std::unique_ptr<Impl> mImpl;
    State mState = State::Idle;
    bool mIsHost = false;
    int mPlayers = 0;
    int mParty = 4;
    std::vector<Member> mMembers;
    std::string mMessage;
    std::array<SeatReady, kMaxSeats> mReady {};
    bool mReadyDirty = false;
    int mLastReadyLogged = -1;
    std::string mLastWaitingLogged;

    void fail(std::string message);
    void launch();
    void logReadySummary();
};

// The port this copy's game will use; PARTYBOARD_ONLINE_GAME_PORT overrides it
// so two copies on one computer can be tested against each other.
std::uint16_t local_game_port();

} // namespace partyboard::online::tailnet
