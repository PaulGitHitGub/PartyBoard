#include "port/netplay_runtime.h"

#include "dolphin/os.h"
#include "dolphin/pad.h"
#include "port/netplay_transport.hpp"
#include "port/netplay_pad.hpp"
#include "port/netplay_progress.hpp"
#include "port/netplay_canonical.hpp"
#include "port/rollback.hpp"
#include "port/rollback_audio.hpp"
#include "port/rollback_audio_bridge.h"
#include "port/rollback_clock.h"
#include "port/rollback_game.h"
#include "port/rollback_animation.h"
#include "port/rollback_io.h"
#include "port/rollback_sequence.h"
#include "port/rollback_scene.h"
#include "port/settings.h"
#include "port/crash_report.h"
#include "port/pad_ports.h"
#include "port/config.hpp"
#include "port/online/tailnet_lobby.hpp"
#include <filesystem> // port/main.h declares a std::filesystem::path global.
#include <map>
#include "port/main.h"
#include "port/port_version.h"
#include "partyboard_version.h"

extern "C" {
#include "game/gamework.h"
#include "game/gamework_data.h"
#include "game/pad.h"
}

#include <SDL3/SDL_timer.h>
#include <algorithm>
#include <array>
#include <cfenv>
#include <cfloat>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#if defined(_WIN32)
// For the D30 heap census only: a return address must be reported relative to
// its module, and that needs GetModuleHandleEx. NOMINMAX because this file
// uses std::min/std::max and windows.h would shadow them with macros.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif
#if defined(_M_X64) || defined(__x86_64__)
#include <xmmintrin.h> // _mm_getcsr, for the floating-point environment line
#endif
#if !defined(_WIN32) && (defined(__unix__) || defined(__APPLE__))
#include <dlfcn.h>   // dladdr, pour la meme resolution d adresse hors Windows
#endif
#include <ctime>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <vector>

extern "C" bool PartyBoard_NetplayIsMinigame(void);
extern "C" s32 PartyBoard_NetplayMinigameId(void);
extern "C" s32 PartyBoard_NetplayContextId(void);
extern "C" u32 frand_state_get(void);
extern "C" void frand_state_set(u32 state);
extern "C" s32 rand8_state_get(void);
extern "C" void rand8_state_set(s32 state);
extern "C" u32 GlobalCounter;
extern "C" u32 frand(void);
extern "C" s32 rand8(void);
extern "C" void BoardRandInit(void);
extern "C" u32 BoardRand(void);
extern "C" void *HuMemHeapPtrGet(int heap);
extern "C" std::size_t HuMemHeapSizeGet(int heap);
extern "C" std::size_t HuPrcSnapshotSizeGet(void);
extern "C" BOOL HuPrcSnapshotSave(void *destination, std::size_t capacity);
extern "C" u64 HuPrcTopologyGenerationGet(void);
extern "C" BOOL HuPrcSnapshotTopologySelfTest(void);
extern "C" BOOL HuPrcSnapshotExecutionSelfTest(void);
extern "C" std::size_t omDLLSnapshotSizeGet(void);
extern "C" BOOL omDLLSnapshotSave(void *destination, std::size_t capacity);
extern "C" std::size_t HuPadSnapshotSizeGet(void);
extern "C" BOOL HuPadSnapshotSave(void *destination, std::size_t capacity);
extern "C" BOOL HuPadSnapshotSelfTest(void);
extern "C" BOOL msmStreamLogicalSelfTest(void);
extern "C" BOOL PartyBoard_RetraceCounterSelfTest(void);
extern "C" BOOL PartyBoard_ThpLogicalSelfTest(void);
extern "C" s32 msmMusGetStatus(int musNo);
extern "C" void msmStreamLogicalTick(void);
extern "C" bool msmStreamLogicalProbeInstall(void);
extern "C" void msmStreamLogicalProbeStart(int channel, int samples, int frequency);
extern "C" void msmStreamLogicalProbeFinishPhysical(int channel);
extern "C" s32 msmStreamGetStatus(int streamNo);

namespace partyboard::netplay {
namespace {

constexpr std::uint32_t kSessionId = 0x4d503452u; // "MP4R"
constexpr std::uint32_t kRuntimeConfigMagic = 0x4e500000u; // "NP" + delay
constexpr std::uint32_t kRuntimeFullGameFlag = 0x00010000u;
constexpr std::uint32_t kRuntimeRollbackFlag = 0x00020000u;
// D23. The hook tick gate changes simulation behaviour, so two peers that
// disagree about it are not playing the same game. The mask gives up bit 18
// to carry it, which makes a gate-enabled peer unreadable to an older binary
// - the right outcome, and better than a silent divergence ten minutes in.
// Overlay indices of the minigame modules, from include/ovl_table.h: the
// m3xx/m4xx entries sit between _minigameDLL/bootDll/instDll below and the
// mode and board modules above.
// D23: defini dans src/game/hsfanim.c, diagnostic uniquement, jamais hache.
extern "C" u32 partyboardParManHookRuns;
constexpr int kFirstMinigameOverlay = 4;
constexpr int kLastMinigameOverlay = 70;
constexpr std::uint32_t kRuntimeHookGateFlag = 0x00040000u;
// Protocol 9: bit 19 says "8-player party", and the delay byte's high nibble
// carries the seat count, so two copies that disagree about either refuse each
// other instead of running two different games (docs/ONLINE_8_PLAYERS.md).
constexpr std::uint32_t kRuntimeParty8Flag = 0x00080000u;
constexpr std::uint32_t kRuntimeConfigMagicMask = 0xfff00000u;
constexpr std::size_t kHistorySize = 256;
constexpr std::uint8_t kDefaultInputDelay = 3;
constexpr std::uint8_t kMaximumInputDelay = 8;
constexpr std::uint32_t kContextMismatchGraceFrames = 120;
enum class MenuProbeOverlay {
#define DLL(name) name,
#include "ovl_table.h"
#undef DLL
};

struct InputSlot {
    std::uint32_t frame = 0;
    PartyBoardRollbackInput input {};
    bool valid = false;
    std::uint32_t captureContext = 0;
    // Several people on one computer (docs/ONLINE_MULTISEAT.md): one slot holds
    // the frame's input for every seat of that computer. `input` is its first
    // seat's, extra[k - 1] the seat k places after it.
    std::uint8_t seats = 1;
    std::array<PartyBoardRollbackInput, rollback::kMaxPlayers - 1> extra {};
};

// The input of the k-th seat (0 = the computer's first seat) a slot speaks for.
const PartyBoardRollbackInput &slotInput(const InputSlot &slot, std::size_t k)
{
    return k == 0 ? slot.input : slot.extra[k - 1];
}

// Several people on one computer: what is per seat of this computer. Seat
// `seat` reads this computer's device port `pad` (Set Up Players' assignment).
struct LocalSeat {
    std::uint8_t seat = 0;
    std::uint8_t pad = 0;
    PartyBoardRollbackInput pending {};
    // Online pause, per seat: each person pauses and resumes for themselves.
    bool paused = false;
    std::uint32_t pauseCount = 0;
    bool startHeldSincePause = false; // Start still down from before; ignore it
    bool suppressStart = false;       // Start that resumed: keep it from the game
    // Hold START about 1 s to pause online (user decision 2026-10-17). While
    // START is down it is held back from the game; a release before 1 s sends
    // a one-frame press then (normal input, so every copy sees it on the same
    // frame), reaching 1 s pauses instead and the press never reaches the game.
    bool startHoldActive = false;
    bool startHoldFired = false;
    std::uint64_t startHoldSinceMs = 0;
    // Test aids PARTYBOARD_NETPLAY_TEST_PAUSE / _START (the first seat) and
    // PARTYBOARD_NETPLAY_TEST_PAUSE_SEAT<n> / _START_SEAT<n> (seat n, 1-8).
    struct TestPause { std::uint32_t frame; std::uint32_t seconds; std::uint32_t repauseMs; std::uint32_t repauseSeconds; };
    std::vector<TestPause> testPauses;
    std::size_t testPauseNext = 0;
    std::uint64_t testResumeAtMs = 0;
    std::uint64_t testRepauseAtMs = 0;      // absolute, 0 = none
    std::uint32_t testRepauseDelayMs = 0;   // armed when the scripted resume happens
    std::uint32_t testRepauseSeconds = 0;
    struct TestStart { std::uint32_t frame; std::uint32_t holdMs; std::uint32_t resumeAfterMs; };
    std::vector<TestStart> testStarts;
    std::size_t testStartNext = 0;
    std::uint64_t testStartDownUntilMs = 0;
    std::uint64_t testStartResumeAtMs = 0;
    // The scripted walk's cursor bookkeeping, per seat, so a computer with
    // several seats walks each exactly as a one-seat copy of that seat would.
    int walkModeLeft = 0;
    int walkNotchesLeft = 0;
    std::uint32_t walkPulseStart = UINT32_MAX;
    std::uint32_t walkPulseEnd = 0;
    // What the log last said feeds this seat.
    std::string controller;
};

struct Runtime {
    UdpTransport transport;
    SessionProgress progress;
    // One canonical-state stream per peer, indexed by seat. There used to be one
    // for the whole session, which is right while there is one peer and wrong the
    // moment there are three: their digests landed in the same stream, contradicted
    // each other, and the session died on an InvalidAck at frame 3 -- which is
    // exactly what the four-instance loopback run reported.
    std::array<StateHistory, rollback::kMaxPlayers> states {};
    // Field-level evidence for the frames a divergence can still be reported
    // at. Digests cover the full 256-frame stream; only the detail is shorter.
    std::array<CanonicalState, kDetailHistorySize> detailHistory {};
    std::array<std::uint32_t, kDetailHistorySize> detailFrames {};
    bool lockstepPrepared = false;
    bool desyncProbe = false;
    bool menuProbe = false;
    std::uint64_t lastStateRepairMs = 0;
    std::uint32_t stateRepairCursor = 0;
    std::string error;
    bool disconnectProbe = false;
    bool contextMismatchProbe = false;
    bool realtimeProbe = false;
    bool audioProbe = false;
    bool walkProbe = false;
    // Overlay-aware walk. The free-running beat below picks the game mode
    // and the board by accident: how many of its horizontal pulses land in
    // a menu depends on how long that menu happens to stay open, which
    // moves by hundreds of frames with the input that preceded it. On
    // 2026-09-12 the same beat settled on Party mode in one run and Story
    // mode in another. With a plan, the walk reacts to the overlay instead:
    // silent where a stray pulse would change the mode, and spending an
    // exact number of cursor moves where the board is chosen.
    bool walkPlan = false;
    // Which entry of the mode list to confirm: 0 Party, 1 Story,
    // 2 the minigame mode (src/REL/modeseldll/main.c:210-243).
    int walkMode = 0;
    // Cursor moves to spend in the board list once it says it is reading
    // the stick. Signed: the cursor starts on w01 at index 2 of
    // {1,2,0,3,4,5}, so +1 is w04 and -1 is w03.
    int walkNotches = 0;
    // (The moves still owed and the pulse timing are per seat: LocalSeat::walk*.)
    // Set by the menu itself, through PartyBoard_NetplayWalkMenu. A frame
    // number cannot stand in for this: the list only accepts a move once its
    // six panels have settled, and when that happens depends on load times.
    bool walkMenuReady = false;
    // Test-only override of the party turn count. A twenty-turn game takes
    // over two hours, so the end of a game - final turn, bonus stars, results,
    // ending sequence - had never once run. Shortening the game exercises all
    // of it. Recorded in every result: a ten-turn game must never be read
    // later as a twenty-turn one.
    int maxTurnsOverride = 0;
    // Test-only, see --netplay-inject-heap.
    int injectHeapFrame = 0;
    int injectHeapBlocks = 0;
    std::uint32_t contextEntryFrame = 0;
    // Input recording and replay. A recording is raw pad samples keyed by the
    // frame they are APPLIED on, so the same file replays correctly at any
    // input delay. Both seats are stored, and each peer replays its own.
    std::string recordPath;
    std::string replayPath;
    std::FILE *recordFile = nullptr;
    std::vector<std::array<PartyBoardRollbackInput, 2>> replaySamples;
    std::vector<bool> replayPresent;
    std::uint32_t replayExhaustedFrame = UINT32_MAX;
    std::array<InputSlot, kHistorySize> localHistory {};
    // One history and one last-applied input per seat, indexed by absolute seat
    // number rather than by "the other one". With two players only the seat
    // localPlayer ^ 1 is ever touched, which is what this still does; the shape
    // is what a third and fourth player need, and changing the shape separately
    // from the logic keeps the two reviewable apart.
    std::array<std::array<InputSlot, kHistorySize>, rollback::kMaxPlayers> peerHistory {};
    std::array<PartyBoardRollbackInput, rollback::kMaxPlayers> lastPeer {};
    // How many seats this session runs. Two until the lobby can agree on more.
    std::uint8_t playerCount = 2;
    // The session is an 8-player party (--netplay-party8): device ports 4-7 are
    // synchronized seats too, and Party Mode plays the 8-player party.
    bool party8 = false;
    PartyBoardRollbackInput lastLocal {};
    // Several people on one computer (docs/ONLINE_MULTISEAT.md). This copy owns
    // seats localPlayer .. localPlayer + localSeatCount - 1; local[k] is seat
    // localPlayer + k. Every computer owns a block of consecutive seats and is
    // named on the wire by its first ("lead") seat: seatLead[s] is the lead of
    // seat s's computer, leadSeats[l] how many seats the computer led by l owns
    // (0 for a seat that is not a lead). One seat per computer by default.
    std::array<LocalSeat, rollback::kMaxPlayers> local {};
    std::uint8_t localSeatCount = 1;
    std::array<std::uint8_t, rollback::kMaxPlayers> seatLead {0, 1, 2, 3, 4, 5, 6, 7};
    std::array<std::uint8_t, rollback::kMaxPlayers> leadSeats {1, 1, 1, 1, 1, 1, 1, 1};
    bool seatCountMismatchLogged = false;
    std::uint32_t controllerCheckFrame = 0;
    std::uint32_t frame = 0;
    std::uint32_t sequence = 0;
    std::uint32_t receivedPackets = 0;
    std::uint32_t rejectedPackets = 0;
    std::uint32_t lastWireFrame = UINT32_MAX;
    std::uint32_t repairPackets = 0;
    std::uint32_t sendFailures = 0;
    std::uint64_t lastPacketMs = 0;
    std::uint64_t lastRepairRequestMs = 0;
    std::uint64_t lastDiagnosticMs = 0;
    unsigned diagnosticLines = 0;
    // Named events are budgeted apart from the periodic heartbeat: they are
    // rare, they arrive late, and sharing one cap made the heartbeat silence
    // them. See the 2026-09-13 board run that hit 3000 lines before its
    // final turn.
    unsigned diagnosticEventLines = 0;
    std::uint32_t stalledTicks = 0;
    std::uint32_t consecutiveStalledTicks = 0;
    std::uint32_t maximumStalledTicks = 0;
    std::uint32_t contextMismatchFrames = 0;
    std::uint32_t repairRequestFrame = UINT32_MAX;
    std::uint32_t sessionFrandSeed = 0;
    std::uint32_t sessionRand8Seed = 0;
    std::uint16_t localPort = 0;
    std::uint8_t localPlayer = 0;
    std::uint8_t localPad = 0;
    std::uint8_t inputDelay = kDefaultInputDelay;
    std::uint8_t contextId = 0;
    bool enabled = false;
    bool fullGame = false;
    bool overlayActive = false;
    bool localCaptured = false;
    bool peerTimeoutReported = false;
    bool configMismatch = false;
    bool peerInDifferentContext = false;
    bool randomSynchronized = false;
    bool rollbackProbe = false;
    bool rollbackProbeDone = false;
    bool rollbackRequested = false;
    bool rollbackPrepared = false;
    bool rollbackUnavailable = false;
    bool rollbackContextReady = false;
    // Online pause. Each seat owns its own pause (LocalSeat::paused for this
    // computer's seats); the game holds while any seat has one. Holding is a
    // stall every copy agrees on: no tick runs, so the game state cannot
    // diverge, whichever frame each side stops on.
    std::uint64_t lastPauseHeartbeatMs = 0;
    std::array<std::uint32_t, rollback::kMaxPlayers> peerPauseCount {};
    std::array<bool, rollback::kMaxPlayers> peerPaused {};
    std::array<std::uint64_t, rollback::kMaxPlayers> peerHeardMs {};
    // What the log last said about each seat's pause (every seat logs every change).
    std::array<bool, rollback::kMaxPlayers> pauseLogged {};
    // Seats reported as silent (no packet for 10 s), so the log says once when a
    // player drops and once if they come back.
    std::array<bool, rollback::kMaxPlayers> silentLogged {};
    // Resume countdown: when the last pause is lifted inside a minigame, every
    // copy holds 3 more seconds showing 3, 2, 1. A hold never ticks, so a copy
    // that finishes first only waits for the others' inputs.
    bool wasHolding = false;
    bool countdownActive = false;
    std::uint64_t countdownStartMs = 0;
    // Test aids PARTYBOARD_NETPLAY_TEST_PAUSE="<frame>:<seconds>[:<re-pause after ms>:<seconds>],..."
    // and PARTYBOARD_NETPLAY_TEST_START="<frame>:<hold ms>[:<resume press after ms>],...", and
    // hold START about 1 s to pause online (user decision 2026-10-17): per seat, in LocalSeat.
    std::uint32_t rollbackBaseFrame = 0;
    std::unique_ptr<rollback::Session> rollbackSession;
    int rollbackSessionContext = -1;
    int observedContext = -1;
    int probeContext = -1; // Set only by the headless PAD regression probe.
};

Runtime gRuntime;

// The seat the single remote occupies. It exists so every call site names a
// seat instead of writing "the other one", which is what has to become a loop
// when playerCount goes past two. Asserting the assumption here means the day
// it stops holding, this function is the one thing to change.
// The peer whose state stream is in trouble, or kMaxPlayers when none is. Every
// report below names a seat rather than "the remote", because with three of them
// the frame alone does not say who disagreed.
std::uint8_t stateErrorSeat();
StateHistory &reportedStates();

// Several people on one computer (docs/ONLINE_MULTISEAT.md).
bool isLocalSeat(const Runtime &runtime, std::uint8_t seat)
{
    return seat >= runtime.localPlayer && seat < runtime.localPlayer + runtime.localSeatCount;
}

// The first seat of another computer: what the link, the state stream, the
// sequence numbers and the "silent" detection are kept by.
bool isRemoteLead(const Runtime &runtime, std::uint8_t seat)
{
    return seat < runtime.playerCount && !isLocalSeat(runtime, seat) && runtime.seatLead[seat] == seat;
}

// "seat 5" or "seats 5-8" (1-based), for log lines about a computer.
std::string seatBlockText(std::uint8_t first, std::uint8_t count)
{
    return count <= 1 ? "seat " + std::to_string(first + 1)
                      : "seats " + std::to_string(first + 1) + "-" + std::to_string(first + count);
}

std::uint8_t soleRemoteSeat()
{
    for (std::uint8_t seat = 0; seat < gRuntime.playerCount; ++seat)
        if (isRemoteLead(gRuntime, seat)) return seat;
    return static_cast<std::uint8_t>(gRuntime.localPlayer ^ 1u);
}


std::uint64_t monotonicMs()
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

void writeDiagnostic(const char *event, bool force = false)
{
#ifdef _WIN32
    const auto now = monotonicMs();
    if (force) {
        if (gRuntime.diagnosticEventLines >= 20000) return;
    } else {
        if (gRuntime.diagnosticLines >= 3000) return;
        if (gRuntime.lastDiagnosticMs && now - gRuntime.lastDiagnosticMs < 2000) return;
    }
    gRuntime.lastDiagnosticMs = now;
    const auto *path = _wgetenv(L"PARTYBOARD_NET_DIAGNOSTIC");
    if (!path || !*path) return;
    FILE *file = _wfopen(path, L"ab");
    if (!file) return;
    const auto &local = gRuntime.localHistory[gRuntime.frame % kHistorySize];
    const auto &remote = gRuntime.peerHistory[soleRemoteSeat()][gRuntime.frame % kHistorySize];
    const auto utc = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    PartyBoardRollbackClock clock {};
    PartyBoard_RollbackClockSave(&clock);
    PartyBoardRollbackStats rollbackStats {};
    std::uint32_t rollbackConfirmed = 0;
    if (gRuntime.rollbackSession) {
        rollbackStats = gRuntime.rollbackSession->stats();
        rollbackConfirmed = gRuntime.rollbackSession->confirmedFrame();
    }
    u32 available[2] {};
    for (unsigned i = 0; i < 64; ++i)
        if (GWMGAvailGet(401 + i)) available[i / 32] |= 1u << (i % 32);
    std::fprintf(file, "utc_ms=%lld event=%s player=%u frame=%u context=%d wire_frame=%u local_ready=%d remote_ready=%d local_context=%u remote_context=%u received=%u rejected=%u repaired=%u send_errors=%u packet_age_ms=%llu rng_sync=%d seeds=%08x/%08x mismatch=%d context_skew=%u live_rng=%08x/%08x global_counter=%u tx_sequence=%u mg_available=%08x/%08x mg_next=%d language=%d message_speed=%d rollback_active=%d rollback_base=%u rollback_current=%u rollback_confirmed=%u rollback_count=%u rollback_replayed=%u rollback_max=%u rollback_predicted=%u rollback_late=%u parman_runs=%u rendered=%u turn=%d max_turn=%d board=%d\n",
        static_cast<long long>(utc), event, gRuntime.localPlayer, gRuntime.frame, gRuntime.observedContext,
        gRuntime.lastWireFrame, local.valid && local.frame == gRuntime.frame, remote.valid && remote.frame == gRuntime.frame,
        local.captureContext, remote.captureContext, gRuntime.receivedPackets, gRuntime.rejectedPackets,
        gRuntime.repairPackets, gRuntime.sendFailures, static_cast<unsigned long long>(gRuntime.lastPacketMs ? now-gRuntime.lastPacketMs : 0),
        gRuntime.randomSynchronized, gRuntime.sessionFrandSeed, gRuntime.sessionRand8Seed, gRuntime.configMismatch,
        gRuntime.contextMismatchFrames, clock.frandSeed, static_cast<u32>(clock.rand8Seed), clock.globalCounter, gRuntime.sequence,
        available[0], available[1], GWSystem.mg_next, GWGameStat.language, GWSystem.mess_speed,
        gRuntime.rollbackSession != nullptr, gRuntime.rollbackBaseFrame,
        rollbackStats.currentFrame, rollbackConfirmed, rollbackStats.rollbackCount,
        rollbackStats.resimulatedFrames, rollbackStats.maximumRollback,
        rollbackStats.predictedFrames, rollbackStats.lateInputs,
        partyboardParManHookRuns, PartyBoard_RenderedFrames,
        /* Progress, not just motion. minFrames cannot tell "played twelve
           turns" from "sat in a menu for twelve minutes", and that is how
           nine runs stuck on the end-of-game statistics screens were filed
           for days as a defect of the GAME. A run that reaches its frame
           budget without reaching turns is an ABNORMAL_EXIT, and nothing
           could say so because no log carried the turn. Now every line
           does. */
        GWSystem.turn, GWSystem.max_turn, GWSystem.board);
    std::fclose(file);
    if (force) { ++gRuntime.diagnosticEventLines; } else { ++gRuntime.diagnosticLines; }
#else
    (void)event; (void)force;
#endif
}

void serviceStateRepair(bool force = false);

void failSession(const char *reason, bool retainTransport = false)
{
    if (!gRuntime.error.empty()) return;
    gRuntime.error = reason;
    writeDiagnostic(reason, true);
    std::fprintf(stderr, "[NET] ERROR frame=%u: %s. Simulation stopped; close the game to restart the session.\n",
        gRuntime.frame, reason);
    PADControlMotor(gRuntime.localPad, PAD_MOTOR_STOP_HARD);
    if (!retainTransport) gRuntime.transport.close();
    // Keep enabled: loss of a peer must never silently resume offline play.
}
// One per device port. Seat N is device port N; ports 4-7 are synchronized
// only in an 8-player party session (PartyBoard_NetplaySyncedPorts).
std::array<PADStatus, rollback::kMaxPlayers> gPhysicalPads {};
std::array<PADStatus, rollback::kMaxPlayers> gSynchronizedPads {};

// Only stages raw values. The original game computes edges, repeats, analog
// thresholds and PAD errors exactly once, after the network tick is accepted.
void PartyBoard_NetplayPadApplyRemote(int pad, const PartyBoardRollbackInput *input,
    const PartyBoardRollbackInput *)
{
    gSynchronizedPads[pad] = padFromInput(*input);
}

bool parsePort(std::string_view text, std::uint16_t &port)
{
    unsigned value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc {} || result.ptr != text.data() + text.size() || value == 0 || value > 65535) {
        return false;
    }
    port = static_cast<std::uint16_t>(value);
    return true;
}

bool parseEndpoint(std::string_view text, std::string &address, std::uint16_t &port)
{
    const auto colon = text.rfind(':');
    if (colon == std::string_view::npos || colon == 0 || colon + 1 == text.size()) {
        return false;
    }
    address.assign(text.substr(0, colon));
    return parsePort(text.substr(colon + 1), port);
}

bool parseDelay(std::string_view text, std::uint8_t &delay)
{
    unsigned value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc {} || result.ptr != text.data() + text.size()
        || value > kMaximumInputDelay) {
        return false;
    }
    delay = static_cast<std::uint8_t>(value);
    return true;
}

// How many seats the session runs, and which one we sit in. Two numbers rather
// than one because a star host is always seat 0 while a guest is told its seat by
// the salon; deriving it from host/guest only works while there is exactly one
// guest to derive.
bool parsePlayerCount(std::string_view text, std::uint8_t &count)
{
    unsigned value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc {} || result.ptr != text.data() + text.size()
        || value < 2 || value > rollback::kMaxPlayers) {
        return false;
    }
    count = static_cast<std::uint8_t>(value);
    return true;
}

bool parseSeat(std::string_view text, std::uint8_t &seat)
{
    unsigned value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc {} || result.ptr != text.data() + text.size()
        || value >= rollback::kMaxPlayers) {
        return false;
    }
    seat = static_cast<std::uint8_t>(value);
    return true;
}

// "seat:address:port". A four-player session on one machine has no NAT to
// traverse, so every instance can be handed the other three directly and the
// whole stack becomes testable without a relay, a router or four computers.
// That is what this exists for: making four players verifiable here, rather
// than shipping a star nobody has run.
// "N" (one seat) or "N-M" (seats N..M of one computer, docs/ONLINE_MULTISEAT.md), 0-based.
bool parseSeatBlock(std::string_view text, std::uint8_t &first, std::uint8_t &count)
{
    const auto dash = text.find('-');
    std::uint8_t last = 0;
    if (dash == std::string_view::npos) {
        if (!parseSeat(text, first)) return false;
        count = 1;
        return true;
    }
    if (!parseSeat(text.substr(0, dash), first) || !parseSeat(text.substr(dash + 1), last) || last < first) return false;
    count = static_cast<std::uint8_t>(last - first + 1);
    return true;
}

bool parseSeatEndpoint(std::string_view text, std::uint8_t &seat, std::uint8_t &count, std::string &address, std::uint16_t &port)
{
    const auto colon = text.find(':');
    if (colon == std::string_view::npos || colon == 0) {
        return false;
    }
    if (!parseSeatBlock(text.substr(0, colon), seat, count)) {
        return false;
    }
    return parseEndpoint(text.substr(colon + 1), address, port);
}

// The test pause/START plans, "a:b[:c[:d]],...".
std::vector<std::array<unsigned, 4>> parsePlan(const char *plan)
{
    std::vector<std::array<unsigned, 4>> items;
    if (plan == nullptr) return items;
    std::string text(plan);
    std::size_t start = 0;
    while (start < text.size()) {
        const std::size_t comma = text.find(',', start);
        const std::string item = text.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        std::array<unsigned, 4> v {};
        if (std::sscanf(item.c_str(), "%u:%u:%u:%u", &v[0], &v[1], &v[2], &v[3]) >= 2) items.push_back(v);
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return items;
}

bool parseLocalPad(std::string_view text, std::uint8_t &pad)
{
    unsigned value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc {} || result.ptr != text.data() + text.size()
        || value < 1 || value > 4) {
        return false;
    }
    pad = static_cast<std::uint8_t>(value - 1);
    return true;
}

PartyBoardRollbackInput capturePad(int pad)
{
    return inputFromPad(gPhysicalPads[pad]);
}

void resetOverlayTimeline(std::uint8_t contextId)
{
    if (gRuntime.rollbackSession) PartyBoard_RollbackAudioBridgeStop();
    gRuntime.rollbackSession.reset();
    gRuntime.rollbackPrepared = false;
    gRuntime.rollbackUnavailable = false;
    gRuntime.rollbackContextReady = false;
    gRuntime.rollbackBaseFrame = 0;
    gRuntime.rollbackSessionContext = -1;
    gRuntime.states = {};
    for (auto &state : gRuntime.detailHistory) state.reset();
    gRuntime.detailFrames.fill(kNoHashFrame);
    // A network timeline owns its own RNG consumption baseline, exactly like
    // the seeds it agrees on: counters must start at zero on both peers.
    PartyBoard_NetplayRandomCountersReset();
    gRuntime.lockstepPrepared = false;
    gRuntime.lastStateRepairMs = 0;
    gRuntime.localHistory = {};
    gRuntime.peerHistory = {};
    gRuntime.lastPeer = {};
    gRuntime.lastPeer = {};
    gRuntime.lastLocal = {};
    for (auto &ls : gRuntime.local) ls.pending = {};
    gRuntime.frame = 0;
    gRuntime.sequence = 0;
    gRuntime.receivedPackets = 0;
    gRuntime.rejectedPackets = 0;
    gRuntime.stalledTicks = 0;
    gRuntime.consecutiveStalledTicks = 0;
    gRuntime.maximumStalledTicks = 0;
    gRuntime.contextMismatchFrames = 0;
    gRuntime.lastRepairRequestMs = 0;
    gRuntime.repairRequestFrame = UINT32_MAX;
    gRuntime.localCaptured = false;
    gRuntime.peerTimeoutReported = false;
    gRuntime.configMismatch = false;
    gRuntime.rollbackProbeDone = false;
    gRuntime.contextId = contextId;
    if (gRuntime.localPlayer == 0) {
        gRuntime.sessionFrandSeed = frand_state_get();
        gRuntime.sessionRand8Seed = static_cast<std::uint32_t>(rand8_state_get());
        gRuntime.randomSynchronized = true;
    } else {
        gRuntime.sessionFrandSeed = 0;
        gRuntime.sessionRand8Seed = 0;
        gRuntime.randomSynchronized = false;
    }
}

std::uint32_t checksumBytes(std::uint32_t hash, const std::uint8_t *data, std::size_t size)
{
    for (std::size_t index = 0; index < size; ++index) {
        hash ^= data[index];
        hash *= 16777619u;
    }
    return hash;
}

enum class SnapshotProbeResult { Busy, Failed, Captured };

SnapshotProbeResult runRollbackSnapshotProbe()
{
    std::uint64_t ioEpoch = 0;
    if (!PartyBoard_RollbackIOTryCapture(&ioEpoch)) return SnapshotProbeResult::Busy;
    struct CaptureLease {
        ~CaptureLease() { PartyBoard_RollbackIORelease(); }
    } lease;
    const auto started = std::chrono::steady_clock::now();
    const auto processGeneration = HuPrcTopologyGenerationGet();
    constexpr int kHeapSystem = 0;
    constexpr int kHeapData = 2;
    rollback::SnapshotLayout heapLayout;
    const std::size_t processBytes = HuPrcSnapshotSizeGet();
    const std::size_t overlayBytes = omDLLSnapshotSizeGet();
    if (!heapLayout.addRegion(HuMemHeapPtrGet(kHeapSystem), HuMemHeapSizeGet(kHeapSystem))
        || !heapLayout.addRegion(HuMemHeapPtrGet(kHeapData), HuMemHeapSizeGet(kHeapData))
        || !PartyBoard_RollbackSceneRegions([](void *context, void *region, std::size_t size) {
            return static_cast<rollback::SnapshotLayout *>(context)->addRegion(region, size);
        }, &heapLayout)
        || processBytes == 0 || overlayBytes == 0) {
        return SnapshotProbeResult::Failed;
    }

    std::vector<std::uint8_t> heapSnapshot(heapLayout.byteSize());
    std::vector<std::uint8_t> processSnapshot(processBytes);
    std::vector<std::uint8_t> overlaySnapshot(overlayBytes);
    std::vector<std::uint8_t> padSnapshot(HuPadSnapshotSizeGet());
    std::vector<std::uint8_t> gameSnapshot(PartyBoard_RollbackGameSize());
    std::vector<std::uint8_t> sequenceSnapshot(PartyBoard_RollbackSequenceSize());
    PartyBoardRollbackClock clockSnapshot {};
    PartyBoard_RollbackClockSave(&clockSnapshot);
    if (!heapLayout.save(heapSnapshot.data(), heapSnapshot.size())
        || !HuPrcSnapshotSave(processSnapshot.data(), processSnapshot.size())
        || !omDLLSnapshotSave(overlaySnapshot.data(), overlaySnapshot.size())
        || !HuPadSnapshotSave(padSnapshot.data(), padSnapshot.size())
        || !PartyBoard_RollbackGameSave(gameSnapshot.data(), gameSnapshot.size())
        || !PartyBoard_RollbackSequenceSave(sequenceSnapshot.data(), sequenceSnapshot.size())
        || processGeneration != HuPrcTopologyGenerationGet()) {
        return SnapshotProbeResult::Failed;
    }

    std::uint32_t checksum = checksumBytes(2166136261u, heapSnapshot.data(), heapSnapshot.size());
    checksum = checksumBytes(checksum, processSnapshot.data(), processSnapshot.size());
    checksum = checksumBytes(checksum, overlaySnapshot.data(), overlaySnapshot.size());
    checksum = checksumBytes(checksum, padSnapshot.data(), padSnapshot.size());
    checksum = checksumBytes(checksum, gameSnapshot.data(), gameSnapshot.size());
    checksum = checksumBytes(checksum, sequenceSnapshot.data(), sequenceSnapshot.size());
    checksum = checksumBytes(checksum, reinterpret_cast<const std::uint8_t *>(&clockSnapshot), sizeof(clockSnapshot));
    std::fprintf(stdout,
        "Rollback probe: partial snapshot captured (%zu heaps/scene-globals + %zu coroutine + %zu overlay + %zu PAD + %zu game + %zu sequence + %zu clock bytes, checksum 0x%08x, capture_ms=%.3f, process_generation=%llu). Full-game rollback remains disabled.\n",
        heapSnapshot.size(), processSnapshot.size(), overlaySnapshot.size(), padSnapshot.size(), gameSnapshot.size(), sequenceSnapshot.size(), sizeof(clockSnapshot), checksum,
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count(),
        static_cast<unsigned long long>(processGeneration));
    return SnapshotProbeResult::Captured;
}

// ---------------------------------------------------------------------------
// Forced rollback probe
//
// SAVE at frame F, let the game run D frames, SAVE again, RESTORE F, replay the
// same D frames, and require the canonical state to come back identical. It is
// the local, deterministic half of rollback: no network, no prediction, no
// remote peer. If a frame cannot be reproduced from a snapshot plus its inputs
// on one machine, no amount of network machinery will make it reproducible on
// two.
//
// The experiment is a side excursion and it is undone. A second snapshot is
// taken at the target frame and restored at the end, so the forward timeline
// continues exactly where it was and two peers running the same probe stay in
// step whatever the result. Nothing is repaired, resynchronized or hidden: a
// failure writes a report naming the first divergent subsystem and field, and
// stops the session.
//
//   PARTYBOARD_FORCE_ROLLBACK=<period>[:<distance,distance,...>]
//
// The distance ladder exists because a one-frame rollback proves very little:
// state the snapshot forgets often only matters once enough frames have been
// re-simulated. Default ladder 1,2,4,8,15,30,60,120, rotated so successive
// tests use different distances.
// ---------------------------------------------------------------------------

struct ForceRollbackProbe {
    bool configured = false;
    std::uint32_t period = 0;
    std::vector<std::uint32_t> distances;
    std::size_t ladderIndex = 0;
    /* The frame at which the next test becomes eligible. The period paces the
       tests; it no longer selects the instants at which the gate is asked. */
    std::uint32_t nextEligibleFrame = 1;
    /* The current wait: since the probe now asks every frame until the gate
       opens, a refusal is no longer an event - it is a state. These record how
       long the wait has lasted and which clause is holding it, so the log can
       report the wait once instead of once per frame. */
    std::uint32_t waitingSince = 0;
    std::uint32_t waitRefusals = 0;
    std::string waitClause;
    /* Consecutive frames the gate has been open for. A test needs the gate open
       twice - once to save, once `distance` frames later to save the forward
       state - so arming on a one-frame opening wastes the test. */
    std::uint32_t openStreak = 0;
    std::uint32_t abandonedAtForwardSave = 0;
    /* Which clause refused, and how often, across the whole run. Kept here
       rather than reconstructed by counting log lines, so the histogram
       survives a truncated diagnostic file. */
    std::map<std::string, std::uint32_t> refusalsByClause;

    bool recording = false;
    std::uint32_t saveFrame = 0;
    std::uint32_t targetFrame = 0;
    std::vector<std::uint8_t> before;
    std::vector<std::uint8_t> after;
    std::vector<std::array<PartyBoardRollbackInput, rollback::kMaxPlayers>> inputs;
    CanonicalState expected;

    std::uint32_t attempted = 0;
    std::uint32_t refused = 0;
    std::uint32_t passed = 0;
    std::uint32_t failed = 0;
    bool stopped = false;
};

ForceRollbackProbe gForceRollback;

void configureForceRollback()
{
    if (gForceRollback.configured) return;
    gForceRollback.configured = true;
    const char *value = std::getenv("PARTYBOARD_FORCE_ROLLBACK");
    if (value == nullptr || value[0] == 0) return;

    const std::string text(value);
    const auto colon = text.find(':');
    const std::string periodText = colon == std::string::npos ? text : text.substr(0, colon);
    gForceRollback.period = static_cast<std::uint32_t>(std::strtoul(periodText.c_str(), nullptr, 10));
    if (gForceRollback.period == 0) return;

    if (colon != std::string::npos) {
        std::size_t start = colon + 1;
        while (start < text.size()) {
            const auto comma = text.find(',', start);
            const auto piece =
                text.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
            const auto distance = static_cast<std::uint32_t>(std::strtoul(piece.c_str(), nullptr, 10));
            if (distance > 0) gForceRollback.distances.push_back(distance);
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
    }
    if (gForceRollback.distances.empty()) {
        gForceRollback.distances = {1, 2, 4, 8, 15, 30, 60, 120};
    }
    char event[160];
    std::snprintf(event, sizeof(event), "force-rollback armed period=%u distances=%zu",
        gForceRollback.period, gForceRollback.distances.size());
    writeDiagnostic(event, true);
}

// Progress is reported on every outcome, not only on failure: a probe that
// silently refuses every test looks exactly like a probe that silently passes
// every test, and the two must never be confusable. writeDiagnostic throttles
// the repeats, so this stays one line every couple of seconds.
void reportForceRollbackProgress(std::uint32_t distance, const char *outcome,
    std::uint32_t atFrame = 0)
{
    const ForceRollbackProbe &probe = gForceRollback;
    char event[224];
    // A refusal happens at the frame it was refused on, not at the last frame a
    // test was armed on; reporting the stale saveFrame made twenty refusals all
    // look like they happened at frame 900.
    std::snprintf(event, sizeof(event),
        "force-rollback %s frame=%u distance=%u attempted=%u passed=%u failed=%u refused=%u",
        outcome, atFrame != 0 ? atFrame : probe.saveFrame, distance, probe.attempted,
        probe.passed, probe.failed, probe.refused);
    // writeDiagnostic throttles unforced lines to one every two seconds across
    // every diagnostic the runtime writes, and the netplay checkpoint line takes
    // that slot, so an unforced line is an invisible line.
    //
    // Every OUTCOME is therefore written: a pass, a failure, a refusal. Only the
    // arming, which happens once per test and says nothing on its own, is
    // throttled to every tenth.
    //
    // An earlier version forced on `attempted % 10`, which never fired: a test
    // refused before the comparison does not increment `attempted`, so a probe
    // refusing every test sat at attempted=1 and said nothing at all. Silence
    // from a probe has to mean the probe is silent, not that it stopped.
    const bool arming = std::strcmp(outcome, "armed") == 0;
    const bool always = !arming || ((probe.attempted + probe.refused) % 10) == 0;
    writeDiagnostic(event, always);
}

std::array<PartyBoardRollbackInput, rollback::kMaxPlayers> currentAppliedInputs()
{
    std::array<PartyBoardRollbackInput, rollback::kMaxPlayers> inputs {};
    for (std::size_t player = 0; player < rollback::kMaxPlayers; ++player) {
        inputs[player] = inputFromPad(gSynchronizedPads[player]);
    }
    return inputs;
}

CanonicalState captureCanonicalNow(std::uint32_t frame)
{
    const StateDigest stamp {frame,
        static_cast<std::uint32_t>(gRuntime.probeContext >= 0 ? gRuntime.probeContext
                                                             : PartyBoard_NetplayContextId()),
        frand_state_get(), static_cast<std::uint32_t>(rand8_state_get()), GlobalCounter};
    return captureCanonical(stamp);
}

// Names the first field whose value differs. Both captures walk the same code,
// so a length difference is itself a finding and is reported as one.
void describeFirstDivergence(const CanonicalState &expected, const CanonicalState &actual,
    std::string &subsystem, std::string &field, std::uint32_t &expectedValue,
    std::uint32_t &actualValue)
{
    subsystem = "NONE";
    field = "none";
    expectedValue = 0;
    actualValue = 0;
    if (expected.values.size() != actual.values.size()) {
        subsystem = "SHAPE";
        field = "field count";
        expectedValue = static_cast<std::uint32_t>(expected.values.size());
        actualValue = static_cast<std::uint32_t>(actual.values.size());
        return;
    }
    for (std::size_t index = 0; index < expected.values.size(); ++index) {
        if (expected.values[index] == actual.values[index]) continue;
        subsystem = subsystemName(expected.subsystemOf(index));
        field = expected.names[index] != nullptr ? expected.names[index] : "?";
        expectedValue = expected.values[index];
        actualValue = actual.values[index];
        return;
    }
}

// Reports land beside the rest of a session's evidence: the crash directory the
// launcher chose, or failing that the directory of the netplay diagnostic.
std::string rollbackReportDirectory()
{
    if (const char *directory = std::getenv("PARTYBOARD_CRASH_DIR")) {
        if (directory[0] != 0) return directory;
    }
    if (const char *diagnostic = std::getenv("PARTYBOARD_NET_DIAGNOSTIC")) {
        std::string text(diagnostic);
        const auto cut = text.find_last_of("/\\");
        if (cut != std::string::npos) return text.substr(0, cut);
    }
    return ".";
}

void writeRollbackFailureReport(const CanonicalState &expected, const CanonicalState &actual,
    std::uint32_t distance)
{
    std::string subsystem, field;
    std::uint32_t expectedValue = 0, actualValue = 0;
    describeFirstDivergence(expected, actual, subsystem, field, expectedValue, actualValue);

    char name[256];
    std::snprintf(name, sizeof(name), "rollback-failure-peer-%u-frame-%u-distance-%u.txt",
        gRuntime.localPlayer, gForceRollback.saveFrame, distance);
    std::string path = rollbackReportDirectory();
    if (!path.empty()) path += "/";
    path += name;

    if (FILE *file = std::fopen(path.c_str(), "w")) {
        std::fprintf(file, "PARTYBOARD_ROLLBACK_FAILURE version=1\n");
        std::fprintf(file, "peer=%u role=%s\n", gRuntime.localPlayer,
            gRuntime.localPlayer == 0 ? "host" : "client");
        std::fprintf(file, "save_frame=%u\n", gForceRollback.saveFrame);
        std::fprintf(file, "target_frame=%u\n", gForceRollback.targetFrame);
        std::fprintf(file, "replay_length=%u\n", distance);
        std::fprintf(file, "snapshot_bytes=%zu\n", gForceRollback.before.size());
        std::fprintf(file, "first_divergent_subsystem=%s\n", subsystem.c_str());
        std::fprintf(file, "first_divergent_field=%s\n", field.c_str());
        std::fprintf(file, "expected_value=0x%08x actual_value=0x%08x\n", expectedValue,
            actualValue);
        std::fprintf(file, "expected_hash=%016llx actual_hash=%016llx\n",
            static_cast<unsigned long long>(expected.hash),
            static_cast<unsigned long long>(actual.hash));
        std::fprintf(file, "\n[SUBSYSTEM HASHES]\n");
        for (std::size_t index = 0; index < kSubsystemCount; ++index) {
            std::fprintf(file, "%-10s expected=%08x actual=%08x %s\n", subsystemName(index),
                expected.parts[index], actual.parts[index],
                expected.parts[index] == actual.parts[index] ? "" : "<-- DIVERGENT");
        }
        std::fprintf(file, "\n[CONTEXT]\n");
        std::fprintf(file, "game_context=%d minigame=%d\n", PartyBoard_NetplayContextId(),
            PartyBoard_NetplayMinigameId());
        std::fprintf(file, "rng frand=%08x rand8=%08x boardrand=%08x\n", frand_state_get(),
            static_cast<unsigned>(rand8_state_get()), boardRandSeed);
        std::fprintf(file, "global_counter=%u\n", GlobalCounter);
        std::fprintf(file, "audio_bridge_active=%d healthy=%d\n",
            PartyBoard_RollbackAudioBridgeActive() ? 1 : 0,
            PartyBoard_RollbackAudioBridgeHealthy() ? 1 : 0);
        std::fprintf(file, "\n[DIVERGENT FIELDS - first 64]\n");
        std::size_t shown = 0;
        const std::size_t count = std::min(expected.values.size(), actual.values.size());
        for (std::size_t index = 0; index < count && shown < 64; ++index) {
            if (expected.values[index] == actual.values[index]) continue;
            std::fprintf(file, "%-10s %-40s expected=0x%08x actual=0x%08x\n",
                subsystemName(expected.subsystemOf(index)),
                expected.names[index] != nullptr ? expected.names[index] : "?",
                expected.values[index], actual.values[index]);
            ++shown;
        }
        std::fclose(file);
    }

    char event[256];
    std::snprintf(event, sizeof(event),
        "ROLLBACK-FAILURE save_frame=%u target_frame=%u distance=%u subsystem=%s field=%s",
        gForceRollback.saveFrame, gForceRollback.targetFrame, distance, subsystem.c_str(),
        field.c_str());
    writeDiagnostic(event, true);
    PartyBoard_CrashBreadcrumb(PARTYBOARD_CRASH_CAT_WARN, "%s", event);
}

// Runs the restore and the replay, then puts the game back where it was.
void runForceRollbackComparison(std::uint32_t frame)
{
    ForceRollbackProbe &probe = gForceRollback;
    const auto distance = static_cast<std::uint32_t>(probe.inputs.size());

    const auto bytes = PartyBoard_RollbackCheckpointSize();
    probe.after.assign(bytes, 0);
    if (bytes == 0 || !PartyBoard_RollbackCheckpointSave(probe.after.data(), bytes)) {
        // The forward snapshot is what makes the excursion undoable. Without it
        // the experiment is abandoned rather than run: leaving the game on a
        // replayed timeline would be a change nobody asked for.
        ++probe.refused;
        ++probe.abandonedAtForwardSave;
        probe.recording = false;
        char outcome[96];
        std::snprintf(outcome, sizeof(outcome), "refused-forward-save-failed(total=%u)",
            probe.abandonedAtForwardSave);
        reportForceRollbackProgress(distance, outcome);
        return;
    }

    probe.expected = captureCanonicalNow(frame);
    ++probe.attempted;

    const char *stage = "ok";
    bool ok = PartyBoard_RollbackCheckpointLoad(probe.before.data(), probe.before.size());
    if (!ok) stage = "restore";
    bool startedBridge = false;
    if (ok && !PartyBoard_RollbackAudioBridgeActive()) {
        // The queue only accepts frames from the one the bridge was started on
        // upwards, and the first tick replayed here is the logic for saveFrame+1
        // because the snapshot was taken after the logic for saveFrame.
        startedBridge = PartyBoard_RollbackAudioBridgeStartAtFrame(probe.saveFrame + 1);
        ok = startedBridge;
        if (!ok) stage = "audio-bridge-start";
    }
    if (ok) {
        for (std::uint32_t step = 0; step < distance && ok; ++step) {
            const auto replayFrame = probe.saveFrame + 1 + step;
            if (!PartyBoard_RollbackAudioBridgeFrameBegin(replayFrame)) {
                ok = false;
                stage = "audio-frame-begin";
            } else if (!PartyBoard_RollbackRunGameLogicTick(probe.inputs[step].data(), 3)) {
                ok = false;
                stage = "game-tick";
            } else {
                // The rendered path publishes this after the frame; a replayed
                // tick has no presentation pass, so it is published here,
                // exactly as the real rollback replay does.
                ++GlobalCounter;
            }
            if (!PartyBoard_RollbackAudioBridgeFrameEnd() && ok) {
                ok = false;
                stage = "audio-frame-end";
            }
        }
    }

    if (ok) {
        const auto actual = captureCanonicalNow(frame);
        if (actual.hash == probe.expected.hash && actual.values == probe.expected.values) {
            ++probe.passed;
            reportForceRollbackProgress(distance, "pass");
        } else {
            ++probe.failed;
            writeRollbackFailureReport(probe.expected, actual, distance);
        }
    } else {
        ++probe.refused;
        char outcome[64];
        std::snprintf(outcome, sizeof(outcome), "refused-at-%s", stage);
        reportForceRollbackProgress(distance, outcome);
    }

    // Back to the real timeline, whatever happened above.
    const bool restored =
        PartyBoard_RollbackCheckpointLoad(probe.after.data(), probe.after.size());
    if (startedBridge) PartyBoard_RollbackAudioBridgeStop();
    probe.recording = false;
    probe.ladderIndex = (probe.ladderIndex + 1) % probe.distances.size();

    if (!restored) {
        probe.stopped = true;
        failSession("Forced rollback probe could not restore the live timeline");
        return;
    }
    if (probe.failed != 0) {
        // No repair, no resynchronization, no continuing as if nothing happened.
        probe.stopped = true;
        failSession("Forced rollback replay did not reproduce the state");
    }
}

void forceRollbackTick(std::uint32_t frame)
{
    configureForceRollback();
    ForceRollbackProbe &probe = gForceRollback;
    if (probe.period == 0 || probe.stopped) return;
    // The replay below runs game logic. If anything in there ever reached this
    // point again the probe would be measuring itself, so it is refused rather
    // than nested.
    static bool inside = false;
    if (inside) return;
    struct Guard {
        bool &flag;
        explicit Guard(bool &f) : flag(f) { flag = true; }
        ~Guard() { flag = false; }
    } guard(inside);

    if (probe.recording) {
        probe.inputs.push_back(currentAppliedInputs());
        if (frame >= probe.targetFrame) runForceRollbackComparison(frame);
        return;
    }

    // Arming policy: ask at the first frame the gate is open at or after the
    // deadline, rather than only at exact multiples of the period.
    //
    // The old spelling was `frame % period != 0`, which asked at 159 instants
    // over a 48000-frame replay and abandoned the opportunity until the next
    // multiple whenever it was refused. It captured four times, and that was
    // read as a property of the engine.
    //
    // It is not. Surveyed on every frame over 51000 frames of the same replay,
    // identical to the digit on both peers:
    //
    //     open 1718 frames of 51000 (3.37%), first open at frame 1,
    //     longest open run 312 frames, and 31 distinct open stretches -
    //     15 of length 1, 5 of length 2-3, 3 of length 32-63, 8 of 64 or more.
    //
    // Six captures out of 159 blind samples of a gate open 3.37% of the time is
    // exactly what chance predicts. The old numbers measured the stride. The
    // engine's real behaviour is that the gate opens in USABLE STRETCHES - one
    // of them five seconds long - and a probe that only ever asks on a fixed
    // grid will keep missing them.
    //
    // This changes WHEN the probe asks, never WHAT it accepts: every safety
    // clause below is untouched, and a refusal is still counted and reported.
    if (frame == 0) return;
    if (frame < probe.nextEligibleFrame) return;
    const auto bytes = PartyBoard_RollbackCheckpointSize();
    if (bytes == 0) {
        // Not a safe boundary: a wipe, a render callback, an I/O operation or a
        // module transition is in flight. Refused and counted, never forced.
        //
        // Unlike before, the opportunity is NOT abandoned until the next
        // multiple: the probe keeps asking on the following frames until the
        // gate opens.
        //
        // Which means a refusal is no longer an event, it is a state, and
        // writing a line per refused frame is writing a line per frame. The
        // first run under this policy did exactly that: roughly 700 bytes a
        // frame filled the diagnostic file's 2 MB budget by frame 4449 and
        // every later probe result was lost - the change destroyed the
        // observability of the thing it was meant to improve.
        //
        // So the counters are authoritative and the log is a summary: a line
        // when a wait begins, a line when the clause holding it changes, one
        // every 600 frames of continued waiting, and the full histogram when a
        // test finally arms. Nothing is lost, because refusalsByClause counts
        // every single refusal whether or not it was printed.
        probe.openStreak = 0;
        ++probe.refused;
        const char *clause = PartyBoard_RollbackCheckpointRefusal();
        const std::string clauseText = clause ? clause : "unknown";
        ++probe.refusalsByClause[clauseText];

        const bool waitBegan = probe.waitRefusals == 0;
        const bool clauseChanged = !waitBegan && clauseText != probe.waitClause;
        if (waitBegan) probe.waitingSince = frame;
        ++probe.waitRefusals;
        probe.waitClause = clauseText;

        if (waitBegan || clauseChanged || (probe.waitRefusals % 600) == 0) {
            char outcome[96];
            std::snprintf(outcome, sizeof(outcome), "refused-%s(waited=%u)",
                clauseText.c_str(), probe.waitRefusals);
            reportForceRollbackProgress(0, outcome, frame);
        }
        return;
    }
    // The gate is open. But a test needs it open TWICE: once here, and again
    // `distance` frames later for the forward save that makes the excursion
    // undoable. Arming on a one-frame opening therefore wastes the test, and
    // measurement says that is what was happening: of 23 tests armed on the
    // first open frame after the deadline, 18 died at the forward save.
    //
    // The per-frame survey showed the openings are strikingly bimodal - 15 of
    // length 1, 5 of length 2-3, NOTHING between 4 and 31, then 3 of 32-63 and
    // 8 of 64 or more. So "has the gate been open for four frames already" is a
    // near-perfect discriminator: it rejects every short opening and accepts
    // every long one, without needing to see the future.
    //
    // Four, not more: it is the smallest number that clears the short mode, and
    // the distances tested (1, 2, 4, 8) all fit comfortably inside a 32-frame
    // opening. Still a question about WHEN to ask, not about what to accept.
    ++probe.openStreak;
    constexpr std::uint32_t kMinimumOpenStreak = 4;
    if (probe.openStreak < kMinimumOpenStreak) return;

    probe.before.assign(bytes, 0);
    if (!PartyBoard_RollbackCheckpointSave(probe.before.data(), bytes)) {
        ++probe.refused;
        reportForceRollbackProgress(0, "refused-save-failed", frame);
        return;
    }
    probe.saveFrame = frame;
    probe.targetFrame = frame + probe.distances[probe.ladderIndex];
    // The period now paces the tests rather than selecting the instants: the
    // next one becomes eligible a period after this one was armed.
    probe.nextEligibleFrame = frame + probe.period;
    {
        // How long this test had to wait for an open gate is the number the new
        // policy exists to produce: under the old one the answer was always
        // "it did not wait, it gave up".
        char outcome[96];
        std::snprintf(outcome, sizeof(outcome), "armed(waited=%u)", probe.waitRefusals);
        reportForceRollbackProgress(probe.distances[probe.ladderIndex], outcome, frame);
        std::string histogram;
        for (const auto &entry : probe.refusalsByClause) {
            if (!histogram.empty()) histogram += ' ';
            histogram += entry.first + '=' + std::to_string(entry.second);
        }
        if (!histogram.empty()) {
            char line[224];
            std::snprintf(line, sizeof(line), "force-rollback clauses %s", histogram.c_str());
            writeDiagnostic(line, true);
        }
    }
    probe.waitRefusals = 0;
    probe.waitClause.clear();
    probe.inputs.clear();
    probe.recording = true;
}

// This computer's input for a frame: every local seat's pending input.
void storeLocalInputs(std::array<InputSlot, kHistorySize> &history, std::uint32_t frame)
{
    InputSlot &slot = history[frame % kHistorySize];
    slot = { frame, gRuntime.local[0].pending, true };
    slot.captureContext = static_cast<std::uint32_t>(gRuntime.observedContext);
    slot.seats = gRuntime.localSeatCount;
    for (std::uint8_t k = 1; k < gRuntime.localSeatCount; ++k) slot.extra[k - 1] = gRuntime.local[k].pending;
}

const InputSlot *findInput(const std::array<InputSlot, kHistorySize> &history, std::uint32_t frame)
{
    const InputSlot &slot = history[frame % kHistorySize];
    return slot.valid && slot.frame == frame ? &slot : nullptr;
}

// Which seats have spoken for a frame, and whether all of them have. Pulled out
// of the tick so it can be tested at three and four seats without standing up a
// session: the tick itself needs a live runtime, and this predicate is the only
// thing that decides whether a frame may advance.
//
// Every seat but the local one must have an input. A missing seat is not a slow
// seat -- in lockstep there is no difference, and advancing without it is the
// one mistake that produces a desync instead of a stall.
bool collectPeerInputs(const Runtime &runtime, std::uint32_t frame,
    std::array<const InputSlot *, rollback::kMaxPlayers> &slots)
{
    slots = {};
    bool everyPeerReady = true;
    // One slot per other computer, kept at its first seat; it holds the input
    // of every seat that computer owns.
    for (std::uint8_t seat = 0; seat < runtime.playerCount; ++seat) {
        if (!isRemoteLead(runtime, seat)) continue;
        slots[seat] = findInput(runtime.peerHistory[seat], frame);
        if (slots[seat] == nullptr) everyPeerReady = false;
    }
    return everyPeerReady;
}


std::uint32_t runtimeConfigSignature(std::uint8_t inputDelay, std::uint8_t contextId,
    bool fullGame, bool rollbackRequested, bool hookGate, std::uint8_t players = 0, bool party8 = false)
{
    return kRuntimeConfigMagic | (fullGame ? kRuntimeFullGameFlag : 0u)
        | (rollbackRequested ? kRuntimeRollbackFlag : 0u)
        | (hookGate ? kRuntimeHookGateFlag : 0u)
        | (party8 ? kRuntimeParty8Flag : 0u)
        | (static_cast<std::uint32_t>(contextId) << 8)
        | (static_cast<std::uint32_t>(players & 0x0fu) << 4) | (inputDelay & 0x0fu);
}

bool sendInput(std::uint32_t frame, const PartyBoardRollbackInput &input, bool requestRetransmit = false,
    const StateDigest *standalone = nullptr)
{
    // Headless regression: drop this sample until the sender has advanced.
    // Only the retained-history repair can then release the waiting peer.
    if (!standalone && gRuntime.probeContext >= 0 && gRuntime.localPlayer == 0
        && frame == 200 && gRuntime.frame <= 200) return true;
    InputPacket outgoing {};
    outgoing.sessionId = kSessionId;
    outgoing.sequence = gRuntime.sequence++;
    outgoing.frame = frame;
    outgoing.player = gRuntime.localPlayer;
    outgoing.input = input;
    outgoing.type = standalone ? PacketType::State
        : requestRetransmit ? PacketType::Retransmit : PacketType::Input;
    // The state block is filled per destination below: an acknowledgement counts
    // how far one peer has agreed with us, and the digest we owe it is the oldest it
    // has not confirmed. Broadcasting one peer's numbers to three is what an
    // InvalidAck is made of.
    const InputSlot *slot = findInput(gRuntime.localHistory, frame);
    outgoing.captureContext = slot ? slot->captureContext : static_cast<std::uint32_t>(gRuntime.observedContext);
    // Every seat of this computer, each with its own pause word (protocol 10).
    outgoing.seats = gRuntime.localSeatCount;
    for (std::uint8_t k = 1; k < gRuntime.localSeatCount; ++k) {
        if (slot) outgoing.extraInput[k - 1] = slot->extra[k - 1];
        const LocalSeat &extra = gRuntime.local[k];
        outgoing.extraPause[k - 1] = (extra.paused ? 0x80000000u : 0u) | (extra.pauseCount & 0x7fffffffu);
    }
    outgoing.configSignature = runtimeConfigSignature(gRuntime.inputDelay, gRuntime.contextId,
        gRuntime.fullGame, gRuntime.rollbackRequested,
        PartyBoard_HookTickGateEnabled() != 0, gRuntime.playerCount, gRuntime.party8);
    outgoing.frandSeed = gRuntime.randomSynchronized ? gRuntime.sessionFrandSeed : 0;
    outgoing.rand8Seed = gRuntime.randomSynchronized ? gRuntime.sessionRand8Seed : 0;
    outgoing.pause = (gRuntime.local[0].paused ? 0x80000000u : 0u) | (gRuntime.local[0].pauseCount & 0x7fffffffu);
    if (!gRuntime.transport.hasPeer()) return true;
    bool sent = false;
    for (std::uint8_t seat = 0; seat < gRuntime.playerCount; ++seat) {
        if (!isRemoteLead(gRuntime, seat) || !gRuntime.transport.hasPeer(seat)) continue;
        if (!gRuntime.rollbackRequested) {
            outgoing.hashAckNext = gRuntime.states[seat].equalThrough();
            const auto *state = standalone ? standalone : gRuntime.states[seat].pending();
            outgoing.state = state ? *state : StateDigest {};
        }
        if (gRuntime.transport.sendInputTo(seat, outgoing)) sent = true;
    }
    if (!sent) ++gRuntime.sendFailures;
    return sent;
}

void serviceStateRepair(bool force)
{
    if (gRuntime.rollbackRequested) return;
    const auto now = monotonicMs();
    if (!force && now - gRuntime.lastStateRepairMs < 100) return;
    StateHistory &reported = reportedStates();
    const auto *state = reported.error() != StateFailure::None
        ? reported.getLocal(reported.errorFrame()) : reported.pending();
    if (!state && reported.captured())
        state = reported.getLocal(reported.captured() - 1);
    if (state) {
        sendInput(state->frame, {}, false, state);
        if (reported.error() == StateFailure::Desync) {
            // The peer may still lack an earlier local hash. Cycle the retained
            // prefix so even loss before the mismatch cannot hide it forever.
            auto &cursor = gRuntime.stateRepairCursor;
            if (cursor < reported.peerEqualThrough() || cursor > state->frame)
                cursor = reported.peerEqualThrough();
            if (const auto *old = reported.getLocal(cursor++))
                if (old->frame != state->frame) sendInput(old->frame, {}, false, old);
        }
        gRuntime.lastStateRepairMs = now;
    }
}

#include "netplay_report.inc"

// ---------------------------------------------------------------------------
// What the simulation was compiled as. Two copies only stay in sync when these
// agree as well as the code does (docs/CROSS_PLATFORM_ONLINE.md), so every
// session log and state log names them. Diagnostics only.
std::string buildMathsDescription()
{
    char text[256];
#if defined(__APPLE__)
    const char *system = "macos";
#elif defined(_WIN32)
    const char *system = "windows";
#elif defined(__ANDROID__)
    const char *system = "android";
#elif defined(__linux__)
    const char *system = "linux";
#else
    const char *system = "other";
#endif
#if defined(__aarch64__) || defined(__arm64__) || defined(_M_ARM64)
    const char *arch = "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
    const char *arch = "x86_64";
#else
    const char *arch = "other";
#endif
    char compiler[96];
#if defined(__clang__)
    std::snprintf(compiler, sizeof(compiler), "clang-%d.%d.%d%s", __clang_major__, __clang_minor__,
        __clang_patchlevel__,
#if defined(_MSC_VER)
        "-cl"
#else
        ""
#endif
    );
#elif defined(_MSC_VER)
    std::snprintf(compiler, sizeof(compiler), "msvc-%d", _MSC_FULL_VER);
#elif defined(__GNUC__)
    std::snprintf(compiler, sizeof(compiler), "gcc-%d.%d", __GNUC__, __GNUC_MINOR__);
#else
    std::snprintf(compiler, sizeof(compiler), "unknown");
#endif
#if defined(PARTYBOARD_NO_FMA)
    const int noFma = 1;
#else
    const int noFma = 0;
#endif
#if defined(PARTYBOARD_SHARED_MATH)
    const int sharedMath = 1;
#else
    const int sharedMath = 0;
#endif
#if defined(FLT_EVAL_METHOD)
    const int floatEval = FLT_EVAL_METHOD;
#else
    const int floatEval = -99; // Not defined by this compiler's headers.
#endif
    std::snprintf(text, sizeof(text),
        "platform=%s-%s compiler=%s no_fma=%d shared_math=%d flt_eval=%d long_bits=%u pointer_bits=%u",
        system, arch, compiler, noFma, sharedMath, floatEval, static_cast<unsigned>(sizeof(long) * 8),
        static_cast<unsigned>(sizeof(void *) * 8));
    return text;
}

// The game thread's floating-point environment. Two machines only compute the
// same results with the same rounding and denormal handling; a library that
// switched on flush-to-zero would change results silently. Diagnostics only.
std::string floatEnvironmentDescription()
{
    const int round = std::fegetround();
    const char *rounding = round == FE_TONEAREST ? "nearest"
        : round == FE_TOWARDZERO ? "toward-zero"
        : round == FE_UPWARD ? "upward"
        : round == FE_DOWNWARD ? "downward" : "unknown";
    int flushToZero = -1, denormalsAreZero = -1;
#if defined(_M_X64) || defined(__x86_64__)
    const unsigned csr = _mm_getcsr();
    flushToZero = (csr >> 15) & 1u;
    denormalsAreZero = (csr >> 6) & 1u;
#elif (defined(__aarch64__) || defined(__arm64__)) && (defined(__clang__) || defined(__GNUC__))
    std::uint64_t fpcr = 0;
    __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
    flushToZero = static_cast<int>((fpcr >> 24) & 1u);
    denormalsAreZero = flushToZero; // AArch64 has one FZ bit for both.
#endif
    char text[128];
    std::snprintf(text, sizeof(text), "rounding=%s flush_to_zero=%d denormals_are_zero=%d", rounding,
        flushToZero, denormalsAreZero);
    return text;
}

// Per-frame state log, for comparing two runs that never played each other:
// the same scripted game on a Mac and on a Windows PC, or two builds on one
// machine. Opt-in and read-only:
//   PARTYBOARD_STATE_LOG=<file>         every committed frame's digest, in the
//                                       desync report's DIGEST format;
//   PARTYBOARD_STATE_FIELDS=<a>[-<b>]   also every canonical field of frames
//                                       a..b, in its FIELD format.
// tools/xplat_state_compare.py names the first frame, subsystem and field
// where two logs differ. Never touches game state; one line per frame.
struct StateLog {
    std::FILE *file = nullptr;
    std::uint32_t fieldsFirst = 1;
    std::uint32_t fieldsLast = 0; // Empty range unless PARTYBOARD_STATE_FIELDS is set.
    std::uint32_t frames = 0;
};
StateLog gStateLog;

void openStateLog()
{
    std::string shown;
#ifdef _WIN32
    const wchar_t *path = _wgetenv(L"PARTYBOARD_STATE_LOG");
    if (!path || !*path) return;
    gStateLog.file = _wfopen(path, L"wb");
    {
        const auto u8 = std::filesystem::path(path).u8string();
        shown.assign(reinterpret_cast<const char *>(u8.c_str()), u8.size());
    }
#else
    const char *path = std::getenv("PARTYBOARD_STATE_LOG");
    if (!path || !*path) return;
    gStateLog.file = std::fopen(path, "wb");
    shown = path;
#endif
    if (!gStateLog.file) {
        std::fprintf(stderr, "Netplay: WARNING cannot write the state log %s; continuing without it.\n",
            shown.c_str());
        return;
    }
    if (const char *range = std::getenv("PARTYBOARD_STATE_FIELDS"); range && *range) {
        unsigned first = 0, last = 0;
        const int parsed = std::sscanf(range, "%u-%u", &first, &last);
        if (parsed >= 1) {
            gStateLog.fieldsFirst = first;
            gStateLog.fieldsLast = parsed == 2 ? last : first;
        }
    }
    std::FILE *file = gStateLog.file;
    std::fprintf(file, "PARTYBOARD_STATE_LOG version=1\n");
    std::fprintf(file, "build_describe=%s\n", PARTY_BOARD_WC_DESCRIBE);
    std::fprintf(file, "build_revision=%s\n", PARTY_BOARD_WC_REVISION);
    std::fprintf(file, "build_stamp=%s %s\n", __DATE__, __TIME__);
    std::fprintf(file, "build_version=%s\n", PARTY_BOARD_VERSION_STRING);
    std::fprintf(file, "build_type=%s\n", PARTY_BOARD_BUILD_TYPE);
    std::fprintf(file, "build_maths %s\n", buildMathsDescription().c_str());
    std::fprintf(file, "protocol=%u hash_version=%u\n", static_cast<unsigned>(kProtocolVersion),
        kStateHashVersion);
    std::fprintf(file,
        "session player=%u players=%u input_delay=%u full_game=%d rollback=%d walk_plan=%d walk_mode=%d walk_notches=%d max_turns=%d\n",
        static_cast<unsigned>(gRuntime.localPlayer), static_cast<unsigned>(gRuntime.playerCount),
        static_cast<unsigned>(gRuntime.inputDelay), gRuntime.fullGame, gRuntime.rollbackRequested,
        gRuntime.walkPlan, gRuntime.walkMode, gRuntime.walkNotches, gRuntime.maxTurnsOverride);
    std::fprintf(file, "fields_first=%u fields_last=%u\n", gStateLog.fieldsFirst, gStateLog.fieldsLast);
    std::fflush(file);
    if (gStateLog.fieldsFirst <= gStateLog.fieldsLast)
        std::fprintf(stdout, "Netplay: writing the per-frame state log %s, with every field of frames %u-%u.\n",
            shown.c_str(), gStateLog.fieldsFirst, gStateLog.fieldsLast);
    else
        std::fprintf(stdout, "Netplay: writing the per-frame state log %s.\n", shown.c_str());
    std::fflush(stdout);
}

void stateLogFrame(const StateDigest &stamp, const CanonicalState &state)
{
    std::FILE *file = gStateLog.file;
    if (!file) return;
    if (gStateLog.frames == 0) {
        // The disc header is read after netplay starts, so it is named here.
        const auto &disc = partyboard::version::getDiskID();
        std::fprintf(file, "disc=%.4s%.2s disc_version=%u disc_number=%u\n", disc.gameName,
            disc.company, static_cast<unsigned>(disc.gameVersion),
            static_cast<unsigned>(disc.diskNumber));
    }
    writeDigestLine(file, "local", stamp);
    if (stamp.frame >= gStateLog.fieldsFirst && stamp.frame <= gStateLog.fieldsLast) {
        std::fprintf(file, "FIELDS frame=%u count=%zu\n", stamp.frame, state.size());
        for (std::size_t index = 0; index < state.size(); ++index) {
            std::fprintf(file, "FIELD %zu %s %s %08x\n", index,
                subsystemName(state.subsystemOf(index)), state.names[index], state.values[index]);
        }
    }
    // Flushed every frame: test runs end with SIGKILL, and the last frames
    // before a divergence are the ones that matter.
    std::fflush(file);
    if (++gStateLog.frames % 6000 == 0) {
        std::fprintf(stdout, "Netplay: state log at frame %u (%u frames written).\n", stamp.frame,
            gStateLog.frames);
        std::fflush(stdout);
    }
}

// A recording is plain text so it can be diffed and read by a human:
//   frame seat buttons stickX stickY substickX substickY triggerL triggerR
// The frame is the one the sample is APPLIED on, never the one it was captured
// on, so a recording made at one input delay replays correctly at another.
// ---------------------------------------------------------------------------
// Rollback gate survey. PARTYBOARD_ROLLBACK_GATE_SURVEY=1.
//
// Why this exists, and what it corrects. The forced rollback probe only asks
// whether a capture is possible at multiples of its period, and abandons the
// opportunity until the next multiple when refused. Its numbers - 155 refusals
// out of 159 - were read as "the gate is shut 97% of the time", which they do
// not say: they say the gate was shut at those 159 chosen instants. The 47762
// frames never asked said nothing either way, and the two possible readings call
// for opposite remedies. See docs/ROLLBACK_READINESS.md.
//
// So this asks every frame, and histograms the runs of open and shut frames
// rather than only counting them: "open 4% of frames" means something quite
// different when the open frames come in usable stretches than when they are
// isolated singles.
//
// It is OFF by default and it is a measurement, not a policy: it changes nothing
// about when a capture is attempted or what is accepted.
// PartyBoard_RollbackCheckpointSize() touches no game state - it walks the
// region set and writes a diagnostic string - but it is not free, so a survey
// run is a survey run and not a normal one.
struct GateSurvey {
    bool enabled = false;
    bool initialized = false;
    std::uint32_t frames = 0;
    std::uint32_t open = 0;
    std::uint32_t openRun = 0;      // current consecutive open frames
    std::uint32_t longestOpenRun = 0;
    std::uint32_t firstOpenFrame = 0;
    bool everOpen = false;
    // Runs bucketed by length: 1, 2-3, 4-7, 8-15, 16-31, 32-63, 64+.
    std::uint32_t openBuckets[7] {};
    std::map<std::string, std::uint32_t> refusals;
};
GateSurvey gGateSurvey;

unsigned gateSurveyBucket(std::uint32_t length)
{
    if (length <= 1) return 0;
    if (length <= 3) return 1;
    if (length <= 7) return 2;
    if (length <= 15) return 3;
    if (length <= 31) return 4;
    if (length <= 63) return 5;
    return 6;
}

void gateSurveyReport(std::uint32_t frame, const char *why)
{
    const GateSurvey &g = gGateSurvey;
    char firstOpen[32];
    if (g.everOpen) std::snprintf(firstOpen, sizeof(firstOpen), "%u", g.firstOpenFrame);
    else std::snprintf(firstOpen, sizeof(firstOpen), "never");
    std::printf("GATE SURVEY %s frame=%u asked=%u open=%u (%.2f%%) longest_open_run=%u first_open=%s\n",
        why, frame, g.frames, g.open,
        g.frames ? 100.0 * static_cast<double>(g.open) / static_cast<double>(g.frames) : 0.0,
        g.longestOpenRun, firstOpen);
    static const char *names[7] = {"1", "2-3", "4-7", "8-15", "16-31", "32-63", "64+"};
    for (unsigned i = 0; i < 7; ++i) {
        if (g.openBuckets[i]) std::printf("GATE SURVEY   open runs of %-6s : %u\n", names[i], g.openBuckets[i]);
    }
    for (const auto &entry : g.refusals) {
        std::printf("GATE SURVEY   refused %-28s : %u\n", entry.first.c_str(), entry.second);
    }
    std::fflush(stdout);
}

void gateSurveyTick(std::uint32_t frame)
{
    GateSurvey &g = gGateSurvey;
    if (!g.initialized) {
        g.initialized = true;
        const char *value = std::getenv("PARTYBOARD_ROLLBACK_GATE_SURVEY");
        g.enabled = value && value[0] && value[0] != '0';
        if (g.enabled) {
            std::printf("GATE SURVEY armed: evaluating the capture gate on EVERY frame. "
                        "This measures the engine, not the probe's sampling stride.\n");
            std::fflush(stdout);
        }
    }
    if (!g.enabled) return;

    ++g.frames;
    const bool open = PartyBoard_RollbackCheckpointSize() != 0;
    if (open) {
        ++g.open;
        if (!g.everOpen) { g.everOpen = true; g.firstOpenFrame = frame; }
        ++g.openRun;
        if (g.openRun > g.longestOpenRun) g.longestOpenRun = g.openRun;
    } else {
        if (g.openRun) { ++g.openBuckets[gateSurveyBucket(g.openRun)]; g.openRun = 0; }
        const char *refusal = PartyBoard_RollbackCheckpointRefusal();
        ++g.refusals[refusal ? refusal : "unknown"];
    }
    // Periodic, so a run that is killed still leaves a measurement behind.
    if (g.frames % 3000 == 0) gateSurveyReport(frame, "progress");
}

bool loadReplaySamples()
{
    std::FILE *file = std::fopen(gRuntime.replayPath.c_str(), "rb");
    if (!file) return false;
    unsigned frame = 0, seat = 0, buttons = 0, triggerL = 0, triggerR = 0;
    int stickX = 0, stickY = 0, substickX = 0, substickY = 0;
    while (std::fscanf(file, "%u %u %u %d %d %d %d %u %u", &frame, &seat, &buttons,
        &stickX, &stickY, &substickX, &substickY, &triggerL, &triggerR) == 9) {
        if (seat > 1 || frame > 60u * 60u * 60u) continue; // Ignore malformed rows.
        if (frame >= gRuntime.replaySamples.size()) {
            gRuntime.replaySamples.resize(frame + 1);
            gRuntime.replayPresent.resize(frame + 1, false);
        }
        PartyBoardRollbackInput &sample = gRuntime.replaySamples[frame][seat];
        sample.buttons = static_cast<u16>(buttons);
        sample.stickX = static_cast<s8>(stickX);
        sample.stickY = static_cast<s8>(stickY);
        sample.substickX = static_cast<s8>(substickX);
        sample.substickY = static_cast<s8>(substickY);
        sample.triggerLeft = static_cast<u8>(triggerL);
        sample.triggerRight = static_cast<u8>(triggerR);
        gRuntime.replayPresent[frame] = true;
    }
    std::fclose(file);
    return !gRuntime.replaySamples.empty();
}

std::uint8_t stateErrorSeat()
{
    for (std::uint8_t seat = 0; seat < gRuntime.playerCount; ++seat) {
        if (!isRemoteLead(gRuntime, seat)) continue;
        if (gRuntime.states[seat].error() != StateFailure::None) return seat;
    }
    return rollback::kMaxPlayers;
}

// The stream to read when reporting. A seat in trouble if there is one, otherwise
// the first peer, which with two players is the only peer and reads as it always did.
StateHistory &reportedStates()
{
    const auto seat = stateErrorSeat();
    if (seat < rollback::kMaxPlayers) return gRuntime.states[seat];
    for (std::uint8_t other = 0; other < gRuntime.playerCount; ++other) {
        if (isRemoteLead(gRuntime, other)) return gRuntime.states[other];
    }
    return gRuntime.states[soleRemoteSeat()];
}

// Every seat's applied input, seat by seat (2026-10-17: was written for two
// players; seats 2 and 3 of a four-player game indexed past the end of the
// two-entry table and crashed at the first tick of the first Mac-vs-PC 1+3 test).
void recordAppliedInputs(std::uint32_t frame,
    const std::array<PartyBoardRollbackInput, partyboard::rollback::kMaxPlayers> &applied)
{
    if (!gRuntime.recordFile) return;
    for (unsigned seat = 0; seat < gRuntime.playerCount && seat < applied.size(); ++seat) {
        const PartyBoardRollbackInput &input = applied[seat];
        std::fprintf(gRuntime.recordFile, "%u %u %u %d %d %d %d %u %u\n", frame, seat,
            input.buttons, input.stickX, input.stickY,
            input.substickX, input.substickY,
            input.triggerLeft, input.triggerRight);
    }
}

bool checkStateFailure()
{
    if (stateErrorSeat() >= rollback::kMaxPlayers) return true;
    StateHistory &reported = reportedStates();
    if (!gRuntime.error.empty()) return false;
    const auto frame = reported.errorFrame();
    const auto *a = reported.getLocal(frame), *b = reported.getRemote(frame);
    const char *category = a && b ? subsystemName(a->firstDifferentPart(*b)) : "UNKNOWN";
    // One self-contained file per peer; tools/netplay_compare.py diffs the two
    // and names the first divergent field.
    const auto report = writeDesyncReport(frame, reported.error());
    char message[1024];
    std::snprintf(message, sizeof(message),
        "%s frame=%u category=%s context=%u/%u localHash=%016llx remoteHash=%016llx RNG=%08x/%08x:%08x/%08x counter=%u/%u hash_version=%u confirmed_input=%u last_equal_next=%u state_error=%u session=%08x player=%u report=%s",
        reported.error() == StateFailure::Desync ? "DESYNC" : "PROTOCOL_STATE",
        frame, category, a ? a->context : UINT32_MAX, b ? b->context : UINT32_MAX,
        static_cast<unsigned long long>(a ? a->hash : 0), static_cast<unsigned long long>(b ? b->hash : 0),
        a ? a->frand : 0, a ? a->rand8 : 0, b ? b->frand : 0, b ? b->rand8 : 0,
        a ? a->counter : 0, b ? b->counter : 0, kStateHashVersion,
        gRuntime.frame ? gRuntime.frame - 1 : UINT32_MAX, reportedStates().equalThrough(),
        static_cast<unsigned>(reportedStates().error()), kSessionId, gRuntime.localPlayer,
        report.empty() ? "none" : report.c_str());
    // The companion collects this sidecar next to its native log; keep writing
    // it. The per-peer report above holds the field-level evidence, which is
    // too large to flood stderr or the capped routine log with.
    FILE *sidecar = nullptr;
#ifdef _WIN32
    const auto *diagnostic = _wgetenv(L"PARTYBOARD_NET_DIAGNOSTIC");
    if (diagnostic && *diagnostic)
        sidecar = _wfopen((std::wstring(diagnostic) + L".desync").c_str(), L"ab");
#endif
    if (sidecar) std::fprintf(sidecar, "%s\n", message);
    if (a && b) {
        for (std::size_t index = 0; index < kSubsystemCount; ++index) {
            const char *verdict = a->parts[index] == b->parts[index] ? "OK" : "DIFFERENT";
            std::fprintf(stderr, "[STATE] frame=%u %-10s local=%08x remote=%08x %s\n", frame,
                subsystemName(index), a->parts[index], b->parts[index], verdict);
            if (sidecar)
                std::fprintf(sidecar, "SUBSYSTEM frame=%u %-10s local=%08x remote=%08x %s\n",
                    frame, subsystemName(index), a->parts[index], b->parts[index], verdict);
        }
    }
    if (sidecar) std::fclose(sidecar);
    serviceStateRepair(true);
    failSession(message, reportedStates().error() == StateFailure::Desync); // Retry failed digest while stopped.
    return false;
}

// Publish the facts a crash report needs, once per accepted simulation tick.
// Everything here is a value the game thread already owns, so the cost is a
// struct copy; the point is that a termination which cannot be intercepted
// in-process still leaves the frame, overlay and network state on disk.
void publishCrashState(const StateDigest &stamp)
{
    static std::int32_t previousOverlay = -1;
    static std::int32_t lastOverlay = -1;
    static std::uint32_t overlayTransitionFrame = 0;

    PartyBoardCrashSimState state {};
    state.simulationFrame = gRuntime.frame;
    state.networkFrame = gRuntime.lastWireFrame;
    state.gameContext = gRuntime.observedContext;
    state.overlay = PartyBoard_NetplayContextId();
    state.minigame = PartyBoard_NetplayMinigameId();
    if (state.overlay != lastOverlay) {
        previousOverlay = lastOverlay;
        lastOverlay = state.overlay;
        overlayTransitionFrame = gRuntime.frame;
    }
    state.overlayPrevious = previousOverlay;
    state.overlayTransitionFrame = overlayTransitionFrame;
    state.boardTurn = GWSystem.turn;
    state.boardMaxTurn = GWSystem.max_turn;
    state.boardId = GWSystem.board;

    state.lastStateHash = stamp.hash;
    state.lastStateHashFrame = stamp.frame;

    state.frand = frand_state_get();
    state.rand8 = static_cast<u32>(rand8_state_get());
    state.boardRand = boardRandSeed;
    state.frandCalls = PartyBoard_NetplayFrandCalls();
    state.rand8Calls = PartyBoard_NetplayRand8Calls();
    state.boardRandCalls = PartyBoard_NetplayBoardRandCalls();

    state.received = gRuntime.receivedPackets;
    state.rejected = gRuntime.rejectedPackets;
    state.repaired = gRuntime.repairPackets;
    state.sendErrors = gRuntime.sendFailures;
    const auto now = monotonicMs();
    state.packetAgeMs = static_cast<u32>(gRuntime.lastPacketMs ? now - gRuntime.lastPacketMs : 0);
    state.stalledTicks = gRuntime.stalledTicks;
    state.maximumStalledTicks = gRuntime.maximumStalledTicks;
    state.txSequence = gRuntime.sequence;
    state.randomSynchronized = gRuntime.randomSynchronized;
    state.configMismatch = gRuntime.configMismatch;
    state.contextMismatchFrames = gRuntime.contextMismatchFrames;

    const auto &local = gRuntime.localHistory[gRuntime.frame % kHistorySize];
    const auto &remote = gRuntime.peerHistory[soleRemoteSeat()][gRuntime.frame % kHistorySize];
    state.localReady = local.valid && local.frame == gRuntime.frame;
    state.remoteReady = remote.valid && remote.frame == gRuntime.frame;
    state.localButtons = local.input.buttons;
    state.remoteButtons = remote.input.buttons;
    state.localStickX = local.input.stickX;
    state.localStickY = local.input.stickY;
    state.remoteStickX = remote.input.stickX;
    state.remoteStickY = remote.input.stickY;

    if (gRuntime.rollbackSession) {
        const auto stats = gRuntime.rollbackSession->stats();
        state.rollbackActive = 1;
        state.rollbackCount = stats.rollbackCount;
        state.rollbackReplayed = stats.resimulatedFrames;
        state.rollbackPredicted = stats.predictedFrames;
    }

    // Audio thread values: diagnostic only, never hashed. See C5/C6 in
    // docs/NETPLAY_DETERMINISM_AUDIT.md.
    for (int channel = 0; channel < 4; ++channel)
        state.musStatus[channel] = msmMusGetStatus(channel);

    PartyBoard_CrashUpdateSimState(&state);
    PartyBoard_CrashHeartbeat();
}
bool captureCommittedState()
{
    const auto frame = gRuntime.frame - 1; // AFTER logic F, BEFORE render/counter publication.
    // Test-only divergence, BEFORE the capture so the extra blocks are in the
    // hash of this very frame. Fires once.
    if (gRuntime.injectHeapFrame > 0
        && frame == static_cast<std::uint32_t>(gRuntime.injectHeapFrame)) {
        PartyBoard_NetplayHeapInjectLeak(gRuntime.injectHeapBlocks, 2048);
        std::printf("[NET TEST] heap leak fired at frame %u\n", frame);
        gRuntime.injectHeapFrame = 0;
    }
    if (frame == 0) {
        // Once, from the game thread, after the renderer and audio have started.
        std::fprintf(stdout, "Netplay: floating-point environment at frame 0: %s.\n",
            floatEnvironmentDescription().c_str());
        std::fflush(stdout);
        if (gStateLog.file) std::fprintf(gStateLog.file, "float_env %s\n", floatEnvironmentDescription().c_str());
    }
    StateDigest stamp {frame,
        static_cast<std::uint32_t>(gRuntime.probeContext >= 0 ? gRuntime.probeContext : PartyBoard_NetplayContextId()),
        frand_state_get(), static_cast<std::uint32_t>(rand8_state_get()), GlobalCounter};
    auto state = captureCanonical(stamp);
    stamp.hash = state.hash;
    stamp.parts = state.parts;
    stateLogFrame(stamp, state);
    gRuntime.detailHistory[frame % kDetailHistorySize] = std::move(state);
    gRuntime.detailFrames[frame % kDetailHistorySize] = frame;
    // Every peer compares our digests against its own, so each stream gets them.
    for (std::uint8_t seat = 0; seat < gRuntime.playerCount; ++seat) {
        if (!isRemoteLead(gRuntime, seat)) continue;
        gRuntime.states[seat].capture(stamp);
    }
    publishCrashState(stamp);
    // New frames stream immediately; input traffic repairs the oldest missing
    // state in parallel. No extra round-trip barrier for each gameplay frame.
    sendInput(frame, {}, false, &stamp);
    if (frame % 120 == 0) {
        char event[160];
        std::snprintf(event, sizeof(event),
            "checkpoint hash_frame=%u state_hash=%016llx hash_version=%u equal_next=%u",
            frame, static_cast<unsigned long long>(stamp.hash), kStateHashVersion, reportedStates().equalThrough());
        writeDiagnostic(event, true);
    }
    // After the state for this frame exists and has been published, so the probe
    // compares against exactly what the peer was told.
    gateSurveyTick(frame);
    forceRollbackTick(frame);
    return checkStateFailure();
}

bool contextCatchupRequired(bool sawMatchingContext, bool sawDifferentContext)
{
    return sawDifferentContext && !sawMatchingContext;
}

bool pauseHolding()
{
    if (gRuntime.rollbackRequested) return false; // lockstep only
    for (std::uint8_t k = 0; k < gRuntime.localSeatCount; ++k)
        if (gRuntime.local[k].paused) return true;
    for (std::uint8_t seat = 0; seat < gRuntime.playerCount; ++seat)
        if (!isLocalSeat(gRuntime, seat) && gRuntime.peerPaused[seat]) return true;
    return false;
}

bool contextIsMinigame(int context)
{
    // Overlay table: 4..69 are the minigames (m300..m463); 70 is mentDll.
    return context >= kFirstMinigameOverlay && context < kLastMinigameOverlay;
}

// Every seat logs every pause change it learns of, its own and the others'.
void logPauseChanges(Runtime &runtime)
{
    for (std::uint8_t seat = 0; seat < runtime.playerCount; ++seat) {
        const bool mine = isLocalSeat(runtime, seat);
        const bool paused = mine ? runtime.local[seat - runtime.localPlayer].paused : runtime.peerPaused[seat];
        if (paused != runtime.pauseLogged[seat]) {
            runtime.pauseLogged[seat] = paused;
            std::fprintf(stdout, "[netplay] pause: player %u %s at network frame %u (context %d)%s.\n",
                static_cast<unsigned>(seat + 1), paused ? "paused" : "resumed", runtime.frame,
                runtime.observedContext, mine ? (runtime.localSeatCount > 1 ? ", a seat of this computer" : ", this copy") : "");
        }
    }
}

// This seat's own pause, from its controller, the menu or a test script.
void setSeatPaused(Runtime &runtime, LocalSeat &ls, bool paused, const char *how)
{
    if (!PartyBoard_NetplayCanPause() || ls.paused == paused) return;
    ls.paused = paused;
    ++ls.pauseCount;
    // A Start already down when pausing must be let go before it can resume.
    ls.startHeldSincePause = true;
    if (!paused) ls.suppressStart = true;
    runtime.lastPauseHeartbeatMs = 0; // tell the others on the next tick
    std::fprintf(stdout, "Netplay: %s by this player (player %u%s).\n", paused ? "paused" : "resumed",
        static_cast<unsigned>(ls.seat + 1), how);
}

// Whether this seat's START is down: its controller, or the scripted test START.
bool localStartDown(Runtime &runtime, LocalSeat &ls)
{
    const std::uint64_t now = monotonicMs();
    if (ls.testStartNext < ls.testStarts.size() && ls.testStartDownUntilMs == 0
        && ls.testStartResumeAtMs == 0 && runtime.frame >= ls.testStarts[ls.testStartNext].frame) {
        const auto &t = ls.testStarts[ls.testStartNext++];
        ls.testStartDownUntilMs = now + t.holdMs;
        ls.testStartResumeAtMs = t.resumeAfterMs ? now + t.holdMs + t.resumeAfterMs : 0;
        std::fprintf(stdout, "[NET TEST] scripted START down for %u ms at network frame %u (seat %u)\n", t.holdMs, runtime.frame,
            static_cast<unsigned>(ls.seat + 1));
    }
    bool test = false;
    if (ls.testStartDownUntilMs != 0) {
        if (now < ls.testStartDownUntilMs) test = true;
        else ls.testStartDownUntilMs = 0;
    } else if (ls.testStartResumeAtMs != 0 && now >= ls.testStartResumeAtMs) {
        // A short press to resume: down for 150 ms.
        ls.testStartDownUntilMs = now + 150;
        ls.testStartResumeAtMs = 0;
        std::fprintf(stdout, "[NET TEST] scripted START press (resume) at network frame %u (seat %u)\n", runtime.frame,
            static_cast<unsigned>(ls.seat + 1));
        test = true;
    }
    return test || (capturePad(ls.pad).buttons & PAD_BUTTON_START) != 0;
}

// The scripted test pauses (PARTYBOARD_NETPLAY_TEST_PAUSE...), checked every tick attempt.
void serviceTestPause(Runtime &runtime, LocalSeat &ls)
{
    const std::uint64_t now = monotonicMs();
    if (ls.testResumeAtMs != 0 && now >= ls.testResumeAtMs) {
        ls.testResumeAtMs = 0;
        std::fprintf(stdout, "[NET TEST] scripted resume at network frame %u (seat %u)\n", runtime.frame,
            static_cast<unsigned>(ls.seat + 1));
        setSeatPaused(runtime, ls, false, ", test script");
        if (ls.testRepauseDelayMs != 0) {
            ls.testRepauseAtMs = now + ls.testRepauseDelayMs;
            ls.testRepauseDelayMs = 0;
        }
    }
    if (ls.testResumeAtMs == 0 && ls.testRepauseAtMs != 0 && now >= ls.testRepauseAtMs) {
        ls.testRepauseAtMs = 0;
        std::fprintf(stdout, "[NET TEST] scripted re-pause at network frame %u (countdown %s, seat %u)\n", runtime.frame,
            runtime.countdownActive ? "running" : "not running", static_cast<unsigned>(ls.seat + 1));
        setSeatPaused(runtime, ls, true, ", test script");
        ls.testResumeAtMs = now + ls.testRepauseSeconds * 1000ull;
    }
    if (ls.testPauseNext < ls.testPauses.size() && ls.testResumeAtMs == 0 && !ls.paused
        && runtime.frame >= ls.testPauses[ls.testPauseNext].frame) {
        const auto &p = ls.testPauses[ls.testPauseNext++];
        std::fprintf(stdout, "[NET TEST] scripted pause at network frame %u for %u s (seat %u)\n", runtime.frame, p.seconds,
            static_cast<unsigned>(ls.seat + 1));
        setSeatPaused(runtime, ls, true, ", test script");
        ls.testResumeAtMs = now + p.seconds * 1000ull;
        ls.testRepauseDelayMs = p.repauseMs;   // armed when the resume above happens
        ls.testRepauseSeconds = p.repauseSeconds;
    }
}

// While paused: keep telling the others (our latest input again, which they
// already hold and check as a duplicate), let each seat's own Start resume it,
// and keep the two-minute watchdog quiet as long as every paused seat is still heard.
void servicePause(Runtime &runtime)
{
    const std::uint64_t now = monotonicMs();
    bool anyLocalPaused = false;
    for (std::uint8_t k = 0; k < runtime.localSeatCount; ++k) {
        LocalSeat &ls = runtime.local[k];
        if (ls.paused) {
            const bool start = localStartDown(runtime, ls);
            if (!start) ls.startHeldSincePause = false;
            else if (!ls.startHeldSincePause) {
                ls.paused = false;
                ++ls.pauseCount;
                ls.suppressStart = true;
                std::fprintf(stdout, "Netplay: resumed (Start, seat %u).\n", static_cast<unsigned>(ls.seat + 1));
            }
        }
        anyLocalPaused = anyLocalPaused || ls.paused;
    }
    if (now - runtime.lastPauseHeartbeatMs >= 100) {
        runtime.lastPauseHeartbeatMs = now;
        const InputSlot *latest = nullptr;
        for (const auto &slot : runtime.localHistory)
            if (slot.valid && (!latest || slot.frame > latest->frame)) latest = &slot;
        if (latest) sendInput(latest->frame, latest->input);
    }
    bool heard = true;
    for (std::uint8_t seat = 0; seat < runtime.playerCount; ++seat)
        if (!isLocalSeat(runtime, seat) && runtime.peerPaused[seat] && now - runtime.peerHeardMs[seat] > 10000)
            heard = false;
    if (anyLocalPaused || heard) runtime.progress.commit(now);
}

void receivePendingPackets()
{
    InputPacket packet {};
    bool sawMatchingContext = false;
    bool sawDifferentContext = false;
    // A busy peer must never monopolize the game thread.
    for (unsigned received = 0; received < 64 && gRuntime.transport.receiveInput(packet); ++received) {
        gRuntime.lastWireFrame = packet.frame;
        gRuntime.lastPacketMs = monotonicMs();
        // Any seat in this session other than our own. The old test named the one
        // other player; this one refuses our own seat (a reflection or a forgery)
        // and anything past the agreed player count, which is the same rule.
        // Since protocol 10 the author is another computer's first seat.
        const bool fromKnownSeat = isRemoteLead(gRuntime, packet.player);
        if (packet.sessionId != kSessionId || !fromKnownSeat) {
            ++gRuntime.rejectedPackets;
            continue;
        }
        // Several people on one computer: a computer speaks for exactly the seats
        // this copy was told it owns (docs/ONLINE_MULTISEAT.md).
        if (packet.seats != gRuntime.leadSeats[packet.player]) {
            if (!gRuntime.seatCountMismatchLogged) {
                gRuntime.seatCountMismatchLogged = true;
                std::fprintf(stdout,
                    "[netplay] refused: the computer at seat %u speaks for %u seat(s), this copy expects %u (%s); "
                    "every copy needs the same seats per computer.\n",
                    static_cast<unsigned>(packet.player + 1), static_cast<unsigned>(packet.seats),
                    static_cast<unsigned>(gRuntime.leadSeats[packet.player]),
                    seatBlockText(packet.player, gRuntime.leadSeats[packet.player]).c_str());
                std::fflush(stdout);
            }
            gRuntime.configMismatch = true;
            ++gRuntime.rejectedPackets;
            continue;
        }
        // Pause state rides on every packet, whatever happens to the rest of it,
        // one word per seat of the sending computer.
        for (std::uint8_t k = 0; k < packet.seats; ++k) {
            const std::uint8_t seat = static_cast<std::uint8_t>(packet.player + k);
            const std::uint32_t word = k == 0 ? packet.pause : packet.extraPause[k - 1];
            const std::uint32_t count = word & 0x7fffffffu;
            if (count >= gRuntime.peerPauseCount[seat]) {
                gRuntime.peerPauseCount[seat] = count;
                gRuntime.peerPaused[seat] = (word >> 31) != 0;
            }
            gRuntime.peerHeardMs[seat] = gRuntime.lastPacketMs;
        }
        const bool signatureValid =
            (packet.configSignature & kRuntimeConfigMagicMask) == kRuntimeConfigMagic;
        const bool remoteFullGame = (packet.configSignature & kRuntimeFullGameFlag) != 0;
        const bool remoteRollback = (packet.configSignature & kRuntimeRollbackFlag) != 0;
        const bool remoteHookGate = (packet.configSignature & kRuntimeHookGateFlag) != 0;
        const std::uint8_t remoteContext =
            static_cast<std::uint8_t>((packet.configSignature >> 8) & 0xffu);
        const std::uint8_t remoteDelay = static_cast<std::uint8_t>(packet.configSignature & 0x0fu);
        const std::uint8_t remotePlayers = static_cast<std::uint8_t>((packet.configSignature >> 4) & 0x0fu);
        const bool remoteParty8 = (packet.configSignature & kRuntimeParty8Flag) != 0;
        if (!signatureValid || remoteFullGame != gRuntime.fullGame
            || remoteRollback != gRuntime.rollbackRequested
            || remoteHookGate != (PartyBoard_HookTickGateEnabled() != 0)
            || remoteDelay != gRuntime.inputDelay
            || remotePlayers != gRuntime.playerCount
            || remoteParty8 != gRuntime.party8) {
            if (!gRuntime.configMismatch) {
                std::fprintf(stderr,
                    "Netplay: session mismatch (local mode %s/delay %u/hook-gate %d/%u seats/%s, "
                    "remote seat %u signature 0x%08x: %u seats/%s). PARTYBOARD_HOOK_TICK_GATE must be set "
                    "the same way on both machines.\n",
                    gRuntime.fullGame ? "full" : "minigame",
                    static_cast<unsigned>(gRuntime.inputDelay),
                    PartyBoard_HookTickGateEnabled(), static_cast<unsigned>(gRuntime.playerCount),
                    gRuntime.party8 ? "8-player party" : "4-player party",
                    static_cast<unsigned>(packet.player), packet.configSignature,
                    static_cast<unsigned>(remotePlayers), remoteParty8 ? "8-player party" : "4-player party");
                std::fprintf(stdout,
                    "[netplay] refused: seat %u plays a different session (%u seats, %s; this copy %u seats, %s, or another mode/delay).\n",
                    static_cast<unsigned>(packet.player + 1), static_cast<unsigned>(remotePlayers),
                    remoteParty8 ? "8-player party" : "4-player party", static_cast<unsigned>(gRuntime.playerCount),
                    gRuntime.party8 ? "8-player party" : "4-player party");
                std::fflush(stdout);
            }
            gRuntime.configMismatch = true;
            ++gRuntime.rejectedPackets;
            continue;
        }
        if (remoteContext != gRuntime.contextId) {
            // An overlay can take a different amount of wall time to load on
            // each machine. Remember the skew so PartyBoard_NetplayTick can
            // let the slower peer finish its local transition. Merely dropping
            // these packets while keeping lockstep stalled deadlocks both
            // peers: neither can advance far enough to enter the other's
            // context.
            sawDifferentContext = true;
            ++gRuntime.rejectedPackets;
            continue;
        }
        sawMatchingContext = true;
        // Seat 0 owns the seeds and everyone else adopts them, from seat 0 only.
        // This said "player 1", which at two players is the same sentence and at
        // four is not: seats 2 and 3 never adopted anything, and seat 1 adopted
        // from whoever spoke first -- including a peer still sending zero because
        // it had not been told the seeds yet.
        const bool seedAuthority = packet.player == 0 && gRuntime.localPlayer != 0;
        if (seedAuthority && !gRuntime.randomSynchronized) {
            gRuntime.sessionFrandSeed = packet.frandSeed;
            gRuntime.sessionRand8Seed = packet.rand8Seed;
            frand_state_set(gRuntime.sessionFrandSeed);
            rand8_state_set(static_cast<s32>(gRuntime.sessionRand8Seed));
            gRuntime.randomSynchronized = true;
            std::fprintf(stdout,
                "Netplay: random generators synchronized (0x%08x/0x%08x).\n",
                gRuntime.sessionFrandSeed, gRuntime.sessionRand8Seed);
        } else if (seedAuthority
            && (packet.frandSeed != gRuntime.sessionFrandSeed
                || packet.rand8Seed != gRuntime.sessionRand8Seed)) {
            gRuntime.configMismatch = true;
            ++gRuntime.rejectedPackets;
            std::fputs("Netplay: host random seed changed during the session.\n", stderr);
            continue;
        }
        if (!gRuntime.rollbackRequested) {
            gRuntime.states[packet.player].acknowledge(packet.hashAckNext);
            gRuntime.states[packet.player].receive(packet.state);
            if (!checkStateFailure()) return;
        }
        if (packet.type == PacketType::State) continue;
        // A repair can concern an old, current or already captured future tick.
        // Always answer from immutable history, even while we are also waiting.
        if (packet.type == PacketType::Retransmit) {
            if (const auto *old = findInput(gRuntime.localHistory, packet.frame)) {
                sendInput(packet.frame, old->input);++gRuntime.repairPackets;
            }
        }
        // Validate retained duplicates even after their input frame committed.
        const auto samePacketInputs = [&packet](const InputSlot &slot) {
            if (!rollback::inputsEqual(slot.input, packet.input) || slot.seats != packet.seats) return false;
            for (std::uint8_t k = 1; k < packet.seats; ++k)
                if (!rollback::inputsEqual(slot.extra[k - 1], packet.extraInput[k - 1])) return false;
            return true;
        };
        if (const auto *accepted = findInput(gRuntime.peerHistory[packet.player], packet.frame)) {
            if (!samePacketInputs(*accepted)
                || accepted->captureContext != packet.captureContext) {
                char reason[192];
                std::snprintf(reason, sizeof(reason),
                    "PROTOCOL input contradiction input_frame=%u player=%u sequence=%u context=%u/%u",
                    packet.frame, packet.player, packet.sequence, accepted->captureContext, packet.captureContext);
                failSession(reason);
                return;
            }
        }
        const bool retainedLateFrame = packet.frame < gRuntime.frame
            && gRuntime.rollbackSession
            && packet.frame >= gRuntime.rollbackBaseFrame
            && gRuntime.frame - packet.frame <= 6;
        if (packet.frame < gRuntime.frame && !retainedLateFrame) {
            ++gRuntime.rejectedPackets;
            continue;
        }
        if (packet.frame >= gRuntime.frame
            && packet.frame - gRuntime.frame >= kHistorySize) {
            ++gRuntime.rejectedPackets;
            continue;
        }
        InputSlot &slot = gRuntime.peerHistory[packet.player][packet.frame % kHistorySize];
        if (!slot.valid || slot.frame != packet.frame) {
            slot.frame = packet.frame;
            slot.input = packet.input;
            slot.valid = true;
            slot.captureContext = packet.captureContext;
            slot.seats = packet.seats;
            slot.extra = packet.extraInput;
        } else if (!samePacketInputs(slot)
            || slot.captureContext != packet.captureContext) {
            ++gRuntime.rejectedPackets;
            continue;
        }
        if (packet.captureContext == static_cast<std::uint32_t>(gRuntime.observedContext))
            gRuntime.rollbackContextReady = true;
        if (gRuntime.rollbackSession && packet.frame >= gRuntime.rollbackBaseFrame) {
            const auto rollbackFrame = packet.frame - gRuntime.rollbackBaseFrame;
            if (!gRuntime.rollbackSession->submitInput(packet.player, rollbackFrame, packet.input)) {
                ++gRuntime.rejectedPackets;
                continue;
            }
        }
        ++gRuntime.receivedPackets;
        gRuntime.progress.observePeer(monotonicMs());
    }
    gRuntime.peerInDifferentContext = contextCatchupRequired(
        sawMatchingContext, sawDifferentContext);
}

bool ensureRollbackSession()
{
    if (!gRuntime.rollbackRequested || gRuntime.rollbackUnavailable
        || (gRuntime.fullGame && !gRuntime.rollbackContextReady)) return false;
    if (gRuntime.rollbackSession) return true;
    const auto snapshotBytes = PartyBoard_RollbackCheckpointSize();
    // An active wipe, render callback, I/O operation or module transition is
    // temporary. Keep lockstep until the next safe game-tick boundary.
    if (!snapshotBytes) return false;
    gRuntime.rollbackBaseFrame = gRuntime.frame;
    rollback::Callbacks callbacks;
    callbacks.saveState = [](void *destination, std::size_t capacity) {
        const bool saved = PartyBoard_RollbackCheckpointSave(destination, capacity);
        if (!saved) {
            char reason[160];
            std::snprintf(reason, sizeof(reason),
                "rollback-capture-failed expected_bytes=%zu available_bytes=%zu",
                capacity, PartyBoard_RollbackCheckpointSize());
            writeDiagnostic(reason, true);
        }
        return saved;
    };
    callbacks.loadState = [](const void *source, std::size_t size) {
        return PartyBoard_RollbackCheckpointLoad(source, size);
    };
    callbacks.simulateFrame = [](std::uint32_t frame, const auto &inputs) {
        const auto wireFrame = gRuntime.rollbackBaseFrame + frame;
        if (!PartyBoard_RollbackAudioBridgeFrameBegin(wireFrame))
            throw std::runtime_error("rollback audio begin failed");
        if (!PartyBoard_RollbackRunGameLogicTick(inputs.data(), 3)) {
            PartyBoard_RollbackAudioBridgeFrameEnd();
            throw std::runtime_error("rollback game tick failed");
        }
        // The rendered path publishes this counter after the frame. Replayed
        // ticks have no presentation pass, so publish it here instead.
        ++GlobalCounter;
        if (!PartyBoard_RollbackAudioBridgeFrameEnd())
            throw std::runtime_error("rollback audio end failed");
    };
    try {
        // Rollback can arm after lockstep has already advanced the wire clock.
        // Audio confirmation and tickets use that same absolute frame domain.
        if (!PartyBoard_RollbackAudioBridgeStartAtFrame(gRuntime.rollbackBaseFrame)) return false;
        auto session = std::make_unique<rollback::Session>(
            rollback::Config {2, 6, snapshotBytes}, std::move(callbacks));
        if (!session->healthy()) {
            PartyBoard_RollbackAudioBridgeStop();
            return false;
        }
        gRuntime.rollbackSession = std::move(session);
        gRuntime.rollbackSessionContext = gRuntime.observedContext;
    } catch (...) {
        PartyBoard_RollbackAudioBridgeStop();
        gRuntime.rollbackUnavailable = true;
        std::fputs("Rollback: insufficient memory for the checkpoint window; lockstep retained.\n", stderr);
        return false;
    }
    std::fprintf(stdout,
        "Rollback: live prediction armed at network frame %u (%zu bytes x 7 snapshots, 6-frame correction window).\n",
        gRuntime.rollbackBaseFrame, snapshotBytes);
    return true;
}

bool prepareRollbackTick()
{
    auto &runtime = gRuntime;
    auto &session = *runtime.rollbackSession;
    if (runtime.rollbackPrepared
        || runtime.frame != runtime.rollbackBaseFrame + session.currentFrame()) {
        failSession("Rollback tick boundary mismatch");
        return false;
    }
    const auto rollbackFrame = runtime.frame - runtime.rollbackBaseFrame;
    const InputSlot *localSlot = findInput(runtime.localHistory, runtime.frame);
    // Unlike lockstep, rollback advances on what it has and corrects later, so a
    // peer that has not spoken is not an error here -- it is simply not submitted.
    std::array<const InputSlot *, rollback::kMaxPlayers> peerSlot {};
    collectPeerInputs(runtime, runtime.frame, peerSlot);
    if (runtime.frame < runtime.inputDelay) {
        const PartyBoardRollbackInput neutral {};
        for (std::uint8_t seat = 0; seat < runtime.playerCount; ++seat) {
            if (!session.submitInput(seat, rollbackFrame, neutral)) {
                failSession("Rollback startup input rejected");
                return false;
            }
        }
    } else {
        if (!localSlot
            || !session.submitInput(runtime.localPlayer, rollbackFrame, localSlot->input)) {
            failSession("Rollback local input missing");
            return false;
        }
        for (std::uint8_t seat = 0; seat < runtime.playerCount; ++seat) {
            if (seat == runtime.localPlayer || peerSlot[seat] == nullptr) continue;
            if (!session.submitInput(seat, rollbackFrame, peerSlot[seat]->input)) {
                failSession("Rollback remote input rejected");
                return false;
            }
        }
    }
    std::array<PartyBoardRollbackInput, rollback::kMaxPlayers> inputs {};
    if (!session.prepare(inputs)) {
        if (!session.healthy()) {
            // CheckpointSave only reads live state. At a fully reconciled
            // boundary its failure needs no restore and no speculative input
            // is discarded. Keep the current wire frame for lockstep retry.
            if (session.failedAtConfirmedCapture()) {
                if (!PartyBoard_RollbackAudioBridgeConfirm(runtime.frame)
                    || !PartyBoard_RollbackAudioBridgeStop()) {
                    failSession("Rollback capture fallback audio failed");
                    return false;
                }
                writeDiagnostic("rollback-capture-lockstep-fallback", true);
                runtime.rollbackSession.reset();
                runtime.rollbackPrepared = false;
                runtime.rollbackSessionContext = -1;
                runtime.rollbackUnavailable = true;
                return false;
            }
            failSession("Rollback checkpoint or replay failed");
            return false;
        }
        if (localSlot) sendInput(runtime.frame, localSlot->input, true);
        ++runtime.stalledTicks;
        ++runtime.consecutiveStalledTicks;
        runtime.maximumStalledTicks = std::max(runtime.maximumStalledTicks,
            runtime.consecutiveStalledTicks);
        return false;
    }
    if (!PartyBoard_RollbackAudioBridgeFrameBegin(runtime.frame)) {
        failSession("Rollback audio frame failed");
        return false;
    }
    // Each pad is paired with the previous input of the seat it belongs to. This
    // used to hand pad 0 the local player's previous input unconditionally, which
    // is right for the host and wrong for a guest, whose seat 0 is the remote --
    // the lockstep path has always paired by seat, and now both do.
    for (std::uint8_t seat = 0; seat < runtime.playerCount; ++seat) {
        const PartyBoardRollbackInput &previous =
            seat == runtime.localPlayer ? runtime.lastLocal : runtime.lastPeer[seat];
        PartyBoard_NetplayPadApplyRemote(seat, &inputs[seat], &previous);
    }
    runtime.lastLocal = inputs[runtime.localPlayer];
    for (std::uint8_t seat = 0; seat < runtime.playerCount; ++seat) {
        if (seat == runtime.localPlayer) continue;
        runtime.lastPeer[seat] = inputs[seat];
    }
    runtime.rollbackPrepared = true;
    runtime.consecutiveStalledTicks = 0;
    runtime.repairRequestFrame = UINT32_MAX;
    runtime.peerTimeoutReported = false;
    return true;
}

// Seat readiness at two, three and four players, which is the part the existing
// self-tests cannot reach: they all run two seats, so the loops introduced for
// four are exercised at the one width that behaves exactly as the old code did.
bool seatReadinessSelfTest()
{
    Runtime probe;
    for (std::uint8_t count = 2; count <= rollback::kMaxPlayers; ++count) {
        for (std::uint8_t localSeat = 0; localSeat < count; ++localSeat) {
            probe.playerCount = count;
            probe.localPlayer = localSeat;
            probe.peerHistory = {};
            std::array<const InputSlot *, rollback::kMaxPlayers> slots {};

            // Nobody has spoken: never ready, whatever the seat count.
            if (collectPeerInputs(probe, 10, slots)) return false;

            // Every peer but the last: still not ready. This is the case that
            // separates "all seats" from "any seat", and the one a two-player
            // test can never distinguish.
            std::uint8_t last = rollback::kMaxPlayers;
            for (std::uint8_t seat = 0; seat < count; ++seat) {
                if (seat == localSeat) continue;
                last = seat;
            }
            for (std::uint8_t seat = 0; seat < count; ++seat) {
                if (seat == localSeat || seat == last) continue;
                probe.peerHistory[seat][10 % kHistorySize] = { 10, {}, true };
            }
            if (count > 2 && collectPeerInputs(probe, 10, slots)) return false;

            // The last peer speaks: ready, and every peer slot is populated while
            // the local seat stays empty -- the tick reads its own input elsewhere.
            probe.peerHistory[last][10 % kHistorySize] = { 10, {}, true };
            if (!collectPeerInputs(probe, 10, slots)) return false;
            if (slots[localSeat] != nullptr) return false;
            for (std::uint8_t seat = 0; seat < count; ++seat) {
                if (seat == localSeat) continue;
                if (slots[seat] == nullptr || slots[seat]->frame != 10) return false;
            }

            // A seat beyond the agreed count is never consulted, so a stale entry
            // left there by a larger previous session cannot make a frame ready.
            if (count < rollback::kMaxPlayers) {
                if (slots[count] != nullptr) return false;
            }

            // Another frame is independent: readiness is per frame, not sticky.
            if (collectPeerInputs(probe, 11, slots)) return false;
        }
    }
    // Several people on one computer: 4 + 4 (this copy seats 1-4) and 2 + 1 + 1.
    // A frame is ready when every OTHER computer has spoken, once each, at its
    // first seat; the seats it owns besides are in that one slot.
    {
        Runtime multi;
        multi.playerCount = 8;
        multi.localPlayer = 0;
        multi.localSeatCount = 4;
        multi.seatLead = {0, 0, 0, 0, 4, 4, 4, 4};
        multi.leadSeats = {4, 0, 0, 0, 4, 0, 0, 0};
        std::array<const InputSlot *, rollback::kMaxPlayers> slots {};
        if (collectPeerInputs(multi, 10, slots)) return false;
        InputSlot four {10, {}, true};
        four.seats = 4;
        four.extra[2].buttons = 0x0100;
        multi.peerHistory[4][10 % kHistorySize] = four;
        if (!collectPeerInputs(multi, 10, slots) || slots[4] == nullptr || slots[5] != nullptr) return false;
        if (slotInput(*slots[4], 3).buttons != 0x0100 || !isRemoteLead(multi, 4) || isRemoteLead(multi, 5)
            || !isLocalSeat(multi, 3) || isLocalSeat(multi, 4)) return false;
        multi.playerCount = 4;
        multi.localPlayer = 2;
        multi.localSeatCount = 1;
        multi.seatLead = {0, 0, 2, 3, 4, 5, 6, 7};
        multi.leadSeats = {2, 0, 1, 1, 1, 1, 1, 1};
        multi.peerHistory = {};
        multi.peerHistory[0][10 % kHistorySize] = {10, {}, true};
        if (collectPeerInputs(multi, 10, slots)) return false; // seat 4's computer is still silent
        multi.peerHistory[3][10 % kHistorySize] = {10, {}, true};
        if (!collectPeerInputs(multi, 10, slots) || slots[1] != nullptr) return false;
    }
    return true;
}

#if defined(PARTYBOARD_HD_BETA)
#include <aurora/aurora.h>
// HD beta comparisons: PARTYBOARD_CAPTURE_FRAMES="1200,5000" saves the game
// image shown after those network frames to PARTYBOARD_CAPTURE_DIR as
// frame_<n>.dds. Online test games are deterministic, so a run with the HD
// features off and one with them on capture the same moments.
static void captureFrameIfRequested(std::uint32_t frame)
{
    static const char *frames = std::getenv("PARTYBOARD_CAPTURE_FRAMES");
    static const char *dir = std::getenv("PARTYBOARD_CAPTURE_DIR");
    if (frames == nullptr || dir == nullptr) {
        return;
    }
    for (const char *p = frames; *p != '\0';) {
        char *end = nullptr;
        const unsigned long wanted = std::strtoul(p, &end, 10);
        if (end == p) {
            break;
        }
        if (wanted == frame) {
            const std::string path = std::string(dir) + "/frame_" + std::to_string(frame) + ".dds";
            aurora_capture_next_frame(path.c_str());
            return;
        }
        p = (*end == ',') ? end + 1 : end;
    }
}
#endif

bool timelineSelfTest()
{
    std::array<InputSlot, 32> history {};
    PartyBoardRollbackInput previous {};
    unsigned edges = 0;
    for (std::uint32_t frame = 0; frame < 12; ++frame) {
        PartyBoardRollbackInput input {};
        if (frame == 3 || frame == 7) {
            input.buttons = PAD_BUTTON_A;
        }
        InputSlot &slot = history[frame % history.size()];
        slot = { frame, input, true };
    }
    for (std::uint32_t frame = 0; frame < 12; ++frame) {
        const InputSlot &slot = history[frame % history.size()];
        if ((slot.input.buttons & ~previous.buttons) & PAD_BUTTON_A) {
            ++edges;
        }
        previous = slot.input;
    }
    if (edges != 2) {
        return false;
    }
    if (!contextCatchupRequired(false, true)
        || contextCatchupRequired(true, true)
        || contextCatchupRequired(false, false)) {
        return false;
    }

    std::array<InputSlot, 32> delayedHistory {};
    constexpr std::uint32_t delay = 2;
    for (std::uint32_t captureFrame = 0; captureFrame < 10; ++captureFrame) {
        PartyBoardRollbackInput input {};
        input.stickX = static_cast<s8>(captureFrame + 1);
        InputSlot &slot = delayedHistory[(captureFrame + delay) % delayedHistory.size()];
        slot = { captureFrame + delay, input, true };
    }
    for (std::uint32_t simulationFrame = 0; simulationFrame < 12; ++simulationFrame) {
        const InputSlot &slot = delayedHistory[simulationFrame % delayedHistory.size()];
        if (simulationFrame < delay) {
            if (slot.valid && slot.frame == simulationFrame) {
                return false;
            }
        } else if (!slot.valid || slot.frame != simulationFrame
            || slot.input.stickX != static_cast<s8>(simulationFrame - delay + 1)) {
            return false;
        }
    }
    std::uint8_t parsedPad = 0xff;
    std::uint8_t parsedCount = 0;
    std::uint8_t parsedSeat = 9;
    // Two is the smallest session and eight the largest the game seats (the
    // 8-player party); a zero, a one and a nine are all refused rather than
    // clamped, because a clamped player count silently runs a different session
    // from the one the salon agreed.
    if (!parsePlayerCount("2", parsedCount) || parsedCount != 2
        || !parsePlayerCount("4", parsedCount) || parsedCount != 4
        || !parsePlayerCount("8", parsedCount) || parsedCount != 8
        || parsePlayerCount("1", parsedCount) || parsePlayerCount("9", parsedCount)
        || parsePlayerCount("0", parsedCount) || parsePlayerCount("", parsedCount)
        || parsePlayerCount("3x", parsedCount) || parsePlayerCount("-1", parsedCount)) {
        return false;
    }
    std::uint8_t peerSeat = 9;
    std::uint8_t peerSeats = 0;
    std::string peerAddress;
    std::uint16_t peerPort = 0;
    if (!parseSeatEndpoint("2:127.0.0.1:34567", peerSeat, peerSeats, peerAddress, peerPort)
        || peerSeat != 2 || peerAddress != "127.0.0.1" || peerPort != 34567
        || !parseSeatEndpoint("7:127.0.0.1:1", peerSeat, peerSeats, peerAddress, peerPort) || peerSeat != 7
        || parseSeatEndpoint("8:127.0.0.1:1", peerSeat, peerSeats, peerAddress, peerPort)
        || parseSeatEndpoint("127.0.0.1:1", peerSeat, peerSeats, peerAddress, peerPort)
        || parseSeatEndpoint("2:127.0.0.1", peerSeat, peerSeats, peerAddress, peerPort)
        || parseSeatEndpoint(":127.0.0.1:1", peerSeat, peerSeats, peerAddress, peerPort)
        || parseSeatEndpoint("2:127.0.0.1:0", peerSeat, peerSeats, peerAddress, peerPort)
        // Several people on one computer: "first-last" blocks.
        || !parseSeatEndpoint("4-7:100.1.2.3:48780", peerSeat, peerSeats, peerAddress, peerPort)
        || peerSeat != 4 || peerSeats != 4 || peerAddress != "100.1.2.3" || peerPort != 48780
        || !parseSeatEndpoint("1-1:127.0.0.1:2", peerSeat, peerSeats, peerAddress, peerPort) || peerSeats != 1
        || parseSeatEndpoint("5-4:127.0.0.1:1", peerSeat, peerSeats, peerAddress, peerPort)
        || parseSeatEndpoint("6-8:127.0.0.1:1", peerSeat, peerSeats, peerAddress, peerPort)
        || parseSeatEndpoint("-3:127.0.0.1:1", peerSeat, peerSeats, peerAddress, peerPort)) {
        return false;
    }
    if (!parseSeat("0", parsedSeat) || parsedSeat != 0
        || !parseSeat("3", parsedSeat) || parsedSeat != 3
        || !parseSeat("7", parsedSeat) || parsedSeat != 7
        || parseSeat("8", parsedSeat) || parseSeat("", parsedSeat)
        || parseSeat("1 ", parsedSeat) || parseSeat("-1", parsedSeat)) {
        return false;
    }
    if (!parseLocalPad("1", parsedPad) || parsedPad != 0
        || !parseLocalPad("4", parsedPad) || parsedPad != 3
        || parseLocalPad("0", parsedPad) || parseLocalPad("5", parsedPad)) {
        return false;
    }
    // Every field the signature carries must change it, or a peer that disagrees
    // about that field would be accepted into the session.
    return runtimeConfigSignature(0, 7, false, false, false)
            != runtimeConfigSignature(delay, 7, false, false, false)
        && runtimeConfigSignature(delay, 7, false, false, false)
            == (kRuntimeConfigMagic | (7u << 8) | delay)
        && runtimeConfigSignature(delay, 0, true, false, false, 4, false)
            != runtimeConfigSignature(delay, 0, true, false, false, 8, false)
        && runtimeConfigSignature(delay, 0, true, false, false, 4, false)
            != runtimeConfigSignature(delay, 0, true, false, false, 4, true)
        && (runtimeConfigSignature(8, 0, true, false, true, 8, true) & kRuntimeConfigMagicMask)
            == kRuntimeConfigMagic
        && (runtimeConfigSignature(8, 0, true, false, true, 8, true) & 0x0fu) == 8u
        && runtimeConfigSignature(delay, 7, false, false, false)
            != runtimeConfigSignature(delay, 8, false, false, false)
        && runtimeConfigSignature(delay, 7, false, false, false)
            != runtimeConfigSignature(delay, 7, true, false, false)
        && runtimeConfigSignature(delay, 7, false, false, false)
            != runtimeConfigSignature(delay, 7, false, true, false)
        && runtimeConfigSignature(delay, 7, false, false, false)
            != runtimeConfigSignature(delay, 7, false, false, true)
        && (runtimeConfigSignature(delay, 7, false, false, true) & kRuntimeConfigMagicMask)
            == kRuntimeConfigMagic;
}

}
}

extern "C" bool PartyBoard_NetplayConfigureFromArgs(int argc, char **argv)
{
    using namespace partyboard::netplay;
    bool host = false;
    bool join = false;
    bool argumentsValid = true;
    std::uint16_t hostPort = 0;
    std::uint16_t joinPort = 0;
    std::string joinAddress;
    std::uint8_t inputDelay = kDefaultInputDelay;
    std::uint8_t localPad = 0;
    std::uint8_t playerCount = 2;
    std::uint8_t localSeat = 0;
    std::uint8_t localSeatCount = 1;
    bool seatGiven = false;
    // seat, address, port, seats that computer owns
    std::vector<std::tuple<std::uint8_t, std::string, std::uint16_t, std::uint8_t>> explicitPeers;
    bool fullGame = false;
    bool party8 = false;
    bool rollbackProbe = false;
    bool rollbackRequested = false;

    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--netplay-host" && index + 1 < argc) {
            host = true;
            argumentsValid = parsePort(argv[++index], hostPort) && argumentsValid;
        } else if (argument == "--netplay-join" && index + 1 < argc) {
            join = true;
            argumentsValid = parseEndpoint(argv[++index], joinAddress, joinPort) && argumentsValid;
        } else if (argument == "--netplay-delay" && index + 1 < argc) {
            argumentsValid = parseDelay(argv[++index], inputDelay) && argumentsValid;
        } else if (argument == "--netplay-pad" && index + 1 < argc) {
            argumentsValid = parseLocalPad(argv[++index], localPad) && argumentsValid;
        } else if (argument == "--netplay-peer" && index + 1 < argc) {
            std::uint8_t peerSeat = 0;
            std::uint8_t peerSeats = 1;
            std::string peerAddress;
            std::uint16_t peerPort = 0;
            if (parseSeatEndpoint(argv[++index], peerSeat, peerSeats, peerAddress, peerPort)) {
                explicitPeers.emplace_back(peerSeat, peerAddress, peerPort, peerSeats);
            } else {
                argumentsValid = false;
            }
        } else if (argument == "--netplay-players" && index + 1 < argc) {
            argumentsValid = parsePlayerCount(argv[++index], playerCount) && argumentsValid;
        } else if (argument == "--netplay-seat" && index + 1 < argc) {
            seatGiven = parseSeatBlock(argv[++index], localSeat, localSeatCount);
            argumentsValid = seatGiven && argumentsValid;
        } else if (argument == "--netplay-full") {
            fullGame = true;
        } else if (argument == "--netplay-party8") {
            party8 = true;
        } else if (argument == "--netplay-rollback-probe") {
            rollbackProbe = true;
        } else if (argument == "--netplay-rollback") {
            rollbackRequested = true;
        } else if (argument == "--netplay-probe-disconnect") {
            gRuntime.disconnectProbe = true;
        } else if (argument == "--netplay-probe-context-mismatch") {
            gRuntime.contextMismatchProbe = true;
        } else if (argument == "--netplay-probe-desync") {
            gRuntime.desyncProbe = true;
        } else if (argument == "--netplay-menu-probe") {
            gRuntime.menuProbe = true;
        } else if (argument == "--netplay-probe-realtime") {
            gRuntime.realtimeProbe = true;
        } else if (argument == "--netplay-probe-audio") {
            gRuntime.audioProbe = true;
        } else if (argument == "--netplay-walk-probe") {
            gRuntime.walkProbe = true;
        } else if (argument.rfind("--netplay-inject-heap", 0) == 0) {
            // Test-only: make the heaps diverge on purpose, to prove the
            // census of D30 actually produces its lines.
            const std::size_t equals = argument.find('=');
            if (equals != std::string_view::npos) {
                const std::string value(argument.substr(equals + 1));
                const std::size_t colon = value.find(':');
                gRuntime.injectHeapFrame = std::atoi(value.c_str());
                gRuntime.injectHeapBlocks =
                    colon == std::string::npos ? 7 : std::atoi(value.c_str() + colon + 1);
                std::printf("[NET TEST] heap leak injected at frame %d, %d block(s)\n",
                    gRuntime.injectHeapFrame, gRuntime.injectHeapBlocks);
            }
        } else if (argument.rfind("--netplay-max-turns", 0) == 0) {
            const std::size_t equals = argument.find('=');
            if (equals != std::string_view::npos) {
                gRuntime.maxTurnsOverride =
                    std::atoi(std::string(argument.substr(equals + 1)).c_str());
                std::printf("[NET TEST] max_turns override=%d\n", gRuntime.maxTurnsOverride);
            }
        } else if (argument.rfind("--netplay-walk-plan", 0) == 0) {
            gRuntime.walkProbe = true;
            gRuntime.walkPlan = true;
            const std::size_t equals = argument.find('=');
            if (equals != std::string_view::npos) {
                const std::string value(argument.substr(equals + 1));
                const std::size_t colon = value.find(':');
                if (colon == std::string::npos) {
                    gRuntime.walkNotches = std::atoi(value.c_str());
                } else {
                    gRuntime.walkMode = std::atoi(value.substr(0, colon).c_str());
                    gRuntime.walkNotches = std::atoi(value.substr(colon + 1).c_str());
                }
            }
            // Say it out loud: a run whose log does not carry this line was not
            // planned, whatever the campaign manifest claims it asked for.
            std::printf("[NET TEST] walk_plan armed mode=%d notches=%d\n", gRuntime.walkMode, gRuntime.walkNotches);
        } else if (argument == "--netplay-record-input" && index + 1 < argc) {
            gRuntime.recordPath = argv[++index];
        } else if (argument == "--netplay-replay-input" && index + 1 < argc) {
            gRuntime.replayPath = argv[++index];
        } else if (argument == "--netplay-host" || argument == "--netplay-join"
            || argument == "--netplay-delay" || argument == "--netplay-pad") {
            argumentsValid = false;
        }
    }
    if (!host && !join) {
        return true;
    }
    if (!argumentsValid || host == join) {
        std::fputs("Netplay: choose exactly one of --netplay-host <port> or --netplay-join <IPv4:port>; optional --netplay-delay <0-8>, --netplay-pad <1-4>, --netplay-players <2-8>, --netplay-seat <0-7 or first-last>, --netplay-peer <seat or first-last:ip:port>, --netplay-full, --netplay-party8, --netplay-rollback and --netplay-rollback-probe.\n", stderr);
        return false;
    }

    // A host is seat 0 by construction. A guest used to be seat 1 by construction
    // too, which is exactly the assumption that stops at two players: with three
    // guests the salon decides who sits where, and says so with --netplay-seat.
    if (!seatGiven) localSeat = host ? 0 : 1;
    // Only that the seat exists. "Host" here names a socket that binds a known port
    // and discovers peers, not a place at the table: four instances on one machine
    // each bind their own port, and none of them is seat 0 by virtue of that. The
    // star's rule that the host sits at seat 0 belongs to the salon, and is enforced
    // there; enforcing it here as well made a four-seat run impossible to stand up.
    if (localSeat + localSeatCount > playerCount) {
        std::fputs("Netplay: the seat must be inside the player count.\n", stderr);
        return false;
    }
    // Several people on one computer (docs/ONLINE_MULTISEAT.md): every computer
    // owns a block of consecutive seats, and together they must cover every seat
    // exactly once, so every copy agrees who speaks for whom.
    bool multiSeat = localSeatCount > 1;
    for (const auto &peer : explicitPeers) multiSeat = multiSeat || std::get<3>(peer) > 1;
    std::array<std::uint8_t, partyboard::rollback::kMaxPlayers> seatLead {0, 1, 2, 3, 4, 5, 6, 7};
    std::array<std::uint8_t, partyboard::rollback::kMaxPlayers> leadSeats {1, 1, 1, 1, 1, 1, 1, 1};
    {
        std::array<int, partyboard::rollback::kMaxPlayers> owner;
        owner.fill(-1);
        std::string problem;
        const auto claim = [&](std::uint8_t first, std::uint8_t count, int who) {
            for (unsigned seat = first; seat < static_cast<unsigned>(first) + count; ++seat) {
                if (seat >= playerCount) { problem = "seat " + std::to_string(seat + 1) + " is past the player count"; return; }
                if (owner[seat] >= 0) { problem = "seat " + std::to_string(seat + 1) + " is given to two computers"; return; }
                owner[seat] = who;
                seatLead[seat] = first;
                leadSeats[seat] = seat == first ? count : 0;
            }
        };
        claim(localSeat, localSeatCount, 0);
        for (std::size_t i = 0; i < explicitPeers.size() && problem.empty(); ++i)
            claim(std::get<0>(explicitPeers[i]), std::get<3>(explicitPeers[i]), static_cast<int>(i) + 1);
        if (multiSeat && problem.empty()) {
            for (unsigned seat = 0; seat < playerCount; ++seat)
                if (owner[seat] < 0) { problem = "seat " + std::to_string(seat + 1) + " belongs to no computer"; break; }
        }
        if (multiSeat && problem.empty() && (!fullGame || rollbackRequested))
            problem = "several seats on one computer need --netplay-full and lockstep (no --netplay-rollback)";
        if (multiSeat && problem.empty() && localPad + localSeatCount > partyboard::rollback::kMaxPlayers)
            problem = "this computer's seats need controller ports " + std::to_string(localPad + 1) + "-"
                + std::to_string(localPad + localSeatCount) + ", past port 8";
        if (!problem.empty()) {
            std::fprintf(stdout, "[netplay] refused: the seats per computer do not add up: %s.\n", problem.c_str());
            std::fprintf(stderr, "Netplay: invalid seats per computer: %s.\n", problem.c_str());
            return false;
        }
    }
    // More than four seats exist only in the 8-player party, which needs the
    // whole game synchronized (Party Mode, board and minigames) and lockstep: the
    // experimental rollback session is a two-seat engine.
    if (playerCount > 4 && !party8) {
        std::fprintf(stdout, "[netplay] refused: %u seats need the 8-player party (--netplay-party8).\n",
            static_cast<unsigned>(playerCount));
        std::fputs("Netplay: more than 4 players need --netplay-party8.\n", stderr);
        return false;
    }
    if (party8 && !fullGame) {
        std::fputs("[netplay] refused: the 8-player party needs --netplay-full.\n", stdout);
        std::fputs("Netplay: --netplay-party8 needs --netplay-full.\n", stderr);
        return false;
    }
    if (party8 && rollbackRequested) {
        std::fputs("[netplay] 8p: rollback refused (two-seat engine); this session would use lockstep, start it without --netplay-rollback.\n", stdout);
        std::fputs("Netplay: --netplay-rollback is not available for the 8-player party.\n", stderr);
        return false;
    }
    gRuntime.playerCount = playerCount;
    gRuntime.party8 = party8;
    gRuntime.localPlayer = localSeat;
    gRuntime.localSeatCount = localSeatCount;
    gRuntime.seatLead = seatLead;
    gRuntime.leadSeats = leadSeats;
    // The crash reporter is already armed; this only lets a report name the seat.
    PartyBoard_CrashSetPeer(gRuntime.localPlayer, host ? "host" : "client");
    // The online companion owns the Internet-facing encrypted transport.
    // Its game subprocess must never expose the raw UDP protocol to the LAN.
    bool loopbackOnly = false;
    for (int i = 1; i < argc; ++i)
        if (std::string_view(argv[i]) == "--netplay-loopback") loopbackOnly = true;
    if (loopbackOnly && join && joinAddress != "127.0.0.1") return false;
    if (!gRuntime.transport.open(host ? hostPort : 0, loopbackOnly)) {
        std::fprintf(stderr, "Netplay: %s\n", gRuntime.transport.lastError().c_str());
        return false;
    }
    if (host) {
        gRuntime.transport.enablePeerDiscovery(true);
        // Everyone but us. Without this the host would keep adopting strangers for
        // the whole session once it had more than one seat to put them in.
        // (One per other computer: several seats of one computer share its link.)
        std::size_t computers = 0;
        for (std::uint8_t seat = 0; seat < playerCount; ++seat)
            if (isRemoteLead(gRuntime, seat)) ++computers;
        gRuntime.transport.expectPeers(computers);
    } else if (!gRuntime.transport.setPeer(joinAddress, joinPort)) {
        std::fprintf(stderr, "Netplay: %s\n", gRuntime.transport.lastError().c_str());
        gRuntime.transport.close();
        return false;
    }
    for (const auto &peer : explicitPeers) {
        const auto seat = std::get<0>(peer);
        if (seat >= playerCount || isLocalSeat(gRuntime, seat)) {
            std::fputs("Netplay: a peer seat must be inside the player count and cannot be our own.\n", stderr);
            gRuntime.transport.close();
            return false;
        }
        if (!gRuntime.transport.addPeer(seat, std::get<1>(peer), std::get<2>(peer))) {
            std::fprintf(stderr, "Netplay: %s\n", gRuntime.transport.lastError().c_str());
            gRuntime.transport.close();
            return false;
        }
    }
    gRuntime.localPort = gRuntime.transport.localPort();
    gRuntime.localPad = localPad;
    gRuntime.inputDelay = inputDelay;
    gRuntime.fullGame = fullGame;
    gRuntime.rollbackRequested = rollbackRequested;
    // Loopback only describes the local socket exposure. The companion now
    // forwards inputs over UDP too, so it needs the same loss recovery.
    gRuntime.rollbackProbe = rollbackProbe;
    gRuntime.enabled = true;
    // This computer's seats: seat localSeat + k reads device port localPad + k.
    for (std::uint8_t k = 0; k < localSeatCount; ++k) {
        LocalSeat &ls = gRuntime.local[k];
        ls.seat = static_cast<std::uint8_t>(localSeat + k);
        ls.pad = static_cast<std::uint8_t>(localPad + k);
        ls.walkModeLeft = gRuntime.walkMode;
        ls.walkNotchesLeft = gRuntime.walkNotches;
        // Test plans: the plain variables for the first seat, _SEAT<n> for seat n (1-8).
        const std::string suffix = "_SEAT" + std::to_string(ls.seat + 1);
        const char *pausePlan = std::getenv(("PARTYBOARD_NETPLAY_TEST_PAUSE" + suffix).c_str());
        if (pausePlan == nullptr && k == 0) pausePlan = std::getenv("PARTYBOARD_NETPLAY_TEST_PAUSE");
        for (const auto &v : parsePlan(pausePlan)) ls.testPauses.push_back({v[0], v[1], v[2], v[3]});
        if (pausePlan != nullptr && *pausePlan != '\0')
            std::printf("[NET TEST] %zu scripted pause(s) for seat %u: %s\n", ls.testPauses.size(),
                static_cast<unsigned>(ls.seat + 1), pausePlan);
        const char *startPlan = std::getenv(("PARTYBOARD_NETPLAY_TEST_START" + suffix).c_str());
        if (startPlan == nullptr && k == 0) startPlan = std::getenv("PARTYBOARD_NETPLAY_TEST_START");
        for (const auto &v : parsePlan(startPlan)) ls.testStarts.push_back({v[0], v[1], v[2]});
        if (startPlan != nullptr && *startPlan != '\0')
            std::printf("[NET TEST] %zu scripted START hold(s) for seat %u: %s\n", ls.testStarts.size(),
                static_cast<unsigned>(ls.seat + 1), startPlan);
    }
    if (!gRuntime.recordPath.empty()) {
        gRuntime.recordFile = std::fopen(gRuntime.recordPath.c_str(), "wb");
        if (!gRuntime.recordFile) {
            std::fprintf(stderr, "Netplay: cannot write the input recording %s.\n",
                gRuntime.recordPath.c_str());
            return false;
        }
    }
    if (!gRuntime.replayPath.empty() && !loadReplaySamples()) {
        std::fprintf(stderr, "Netplay: cannot read the input recording %s.\n",
            gRuntime.replayPath.c_str());
        return false;
    }
    std::fprintf(stdout,
        "Netplay experimental: %s, UDP port %u, local player %u, physical controller port %u, input delay %u frame(s). %s.\n",
        host ? "host" : "client", gRuntime.localPort,
        static_cast<unsigned>(gRuntime.localPlayer + 1),
        static_cast<unsigned>(gRuntime.localPad + 1),
        static_cast<unsigned>(gRuntime.inputDelay),
        gRuntime.fullGame ? (gRuntime.rollbackRequested
            ? "Full-game rollback requested"
            : "Full-game lockstep")
        : (gRuntime.rollbackRequested ? "Minigame rollback requested" : "Minigame overlays"));
    {
        // Always on: who sits where. Seat N is device port N in the game (one
        // seat per copy); with the 8-player party the seats are Party Mode's
        // people in seat order and the rest are computer players.
        std::string peers;
        for (const auto &peer : explicitPeers) {
            peers += " " + seatBlockText(std::get<0>(peer), std::get<3>(peer)) + "=" + std::get<1>(peer) + ":"
                + std::to_string(std::get<2>(peer));
        }
        std::fprintf(stdout,
            "[netplay] seats: %u online seat(s), %s, this copy is %s (device port%s %s, own controller port%s %s)%s%s; "
            "%s.\n",
            static_cast<unsigned>(gRuntime.playerCount),
            gRuntime.party8 ? "8-player party" : "4-player party",
            seatBlockText(gRuntime.localPlayer, gRuntime.localSeatCount).c_str(), gRuntime.localSeatCount > 1 ? "s" : "",
            seatBlockText(gRuntime.localPlayer, gRuntime.localSeatCount).substr(gRuntime.localSeatCount > 1 ? 6 : 5).c_str(),
            gRuntime.localSeatCount > 1 ? "s" : "",
            seatBlockText(gRuntime.localPad, gRuntime.localSeatCount).substr(gRuntime.localSeatCount > 1 ? 6 : 5).c_str(),
            peers.empty() ? "" : "; peers:", peers.c_str(),
            gRuntime.party8
                ? (gRuntime.playerCount < 8 ? "seats beyond the online ones are computer players" : "all 8 seats are people")
                : "device ports 5-8 unplugged");
        // Several people on one computer: each machine's seats, in seat order,
        // and which of this computer's controller ports feeds each of its seats.
        std::string machines;
        for (std::uint8_t seat = 0; seat < gRuntime.playerCount; ++seat) {
            if (gRuntime.seatLead[seat] != seat) continue;
            if (!machines.empty()) machines += ", ";
            machines += seatBlockText(seat, gRuntime.leadSeats[seat]);
            machines += isLocalSeat(gRuntime, seat) ? " this " + std::string(partyboard::online::tailnet::kComputer) : " another computer";
        }
        std::fprintf(stdout, "[netplay] computers: %s; this %s owns %s (%u %s on this computer).\n", machines.c_str(),
            partyboard::online::tailnet::kComputer, seatBlockText(gRuntime.localPlayer, gRuntime.localSeatCount).c_str(),
            static_cast<unsigned>(gRuntime.localSeatCount), gRuntime.localSeatCount == 1 ? "person" : "people");
        for (std::uint8_t k = 0; k < gRuntime.localSeatCount; ++k)
            std::fprintf(stdout, "[netplay] seat %u = device port %u in the game, fed by this %s's controller port %u.\n",
                static_cast<unsigned>(gRuntime.local[k].seat + 1), static_cast<unsigned>(gRuntime.local[k].seat + 1),
                partyboard::online::tailnet::kComputer, static_cast<unsigned>(gRuntime.local[k].pad + 1));
    }
    // Always on: the first thing to check when two different computers fall
    // out of sync is whether their game maths were built the same way.
    std::fprintf(stdout, "Netplay: build %s.\n", buildMathsDescription().c_str());
    // The lobby's cross-play identity, so a session started by hand (no lobby) names it too:
    // two copies of different computers should only ever be paired when these agree.
    {
        const std::string simulation = partyboard::online::tailnet::simulation_fingerprint();
        std::fprintf(stdout, "Netplay: cross-play identity %s (%s).\n",
            simulation.empty() ? "none" : simulation.substr(0, 12).c_str(),
            partyboard::online::tailnet::simulation_fingerprint_summary().c_str());
    }
    std::fflush(stdout);
    openStateLog();
    return true;
}

// The simulation frame, for instrumentation that needs to date an event. Zero
// when netplay is not running, which is the honest answer: there is no shared
// timeline to refer to.
extern "C" u32 PartyBoard_NetplayFrameForDiagnostics(void)
{
    return partyboard::netplay::gRuntime.enabled ? partyboard::netplay::gRuntime.frame : 0;
}

extern "C" bool PartyBoard_NetplayEnabled(void)
{
    return partyboard::netplay::gRuntime.enabled;
}

extern "C" int PartyBoard_NetplayPeerPing(int seat)
{
    using namespace partyboard::netplay;
    if (!gRuntime.enabled || seat < 0 || seat >= gRuntime.playerCount || isLocalSeat(gRuntime, static_cast<std::uint8_t>(seat))) {
        return -2;
    }
    // A seat of another computer: that computer's link.
    return gRuntime.transport.peerRoundTripMs(gRuntime.seatLead[seat]);
}

extern "C" int PartyBoard_NetplayPlayerCount(void)
{
    using namespace partyboard::netplay;
    return gRuntime.enabled ? gRuntime.playerCount : 0;
}

extern "C" bool PartyBoard_NetplayParty8(void)
{
    using namespace partyboard::netplay;
    return gRuntime.enabled && gRuntime.party8;
}

extern "C" int PartyBoard_NetplaySyncedPorts(void)
{
    using namespace partyboard::netplay;
    return gRuntime.enabled && gRuntime.party8 ? 8 : 4;
}

// Called by a menu that has a cursor a scripted walk must move, to say
// whether it is reading the stick this frame. Both peers run the same menu
// code on the same simulation tick, so this stays in step by construction -
// which a frame number chosen in advance does not.
// D23. A draw hook advances game state, so it belongs to the tick and not to
// the image: a peer waiting for its partner still renders, and would run every
// hook again on a frame the network refused it. Read once - this is consulted
// from the drawing pass, per model and per sprite.
extern "C" int PartyBoard_HookTickGateEnabled(void)
{
    static int resolved = -1;
    if (resolved < 0) {
        const char *value = std::getenv("PARTYBOARD_HOOK_TICK_GATE");
        resolved = (value && *value && *value != '0') ? 1 : 0;
    }
    return resolved;
}

// Test-only. 0 means the game decides, as it normally does.
extern "C" int PartyBoard_NetplayMaxTurnsOverride(void)
{
    return partyboard::netplay::gRuntime.maxTurnsOverride;
}

extern "C" void PartyBoard_NetplayWalkMenu(int ready)
{
    partyboard::netplay::gRuntime.walkMenuReady = ready != 0;
}

extern "C" void PartyBoard_NetplayTrace(const char *event)
{
    if (partyboard::netplay::gRuntime.enabled && event)
        partyboard::netplay::writeDiagnostic(event, true);
}

extern "C" bool PartyBoard_NetplayAllowsMultipleInstances(void)
{
    return PartyBoard_NetplayEnabled();
}

namespace partyboard::netplay {
namespace {

// One seat of this computer: its controller (or the test script / walk), with
// hold-Start handled per seat. The result is left in ls.pending; true when a
// short Start press is to reach the game on this frame.
bool captureSeatInput(Runtime &runtime, LocalSeat &ls)
{
    const std::uint8_t seat = ls.seat;
    ls.pending = capturePad(ls.pad);
    if (localStartDown(runtime, ls)) ls.pending.buttons |= PAD_BUTTON_START;
    // The Start that ended a pause stays out of the game until released.
    if (ls.suppressStart) {
        if (ls.pending.buttons & PAD_BUTTON_START) ls.pending.buttons &= ~PAD_BUTTON_START;
        else ls.suppressStart = false;
    }
    // Hold START to pause; a short press reaches the game on its release frame.
    bool startTap = false;
    if (PartyBoard_NetplayCanPause()) {
        const std::uint64_t now = monotonicMs();
        const bool down = (ls.pending.buttons & PAD_BUTTON_START) != 0;
        ls.pending.buttons &= ~PAD_BUTTON_START;
        if (down) {
            if (!ls.startHoldActive) {
                ls.startHoldActive = true;
                ls.startHoldFired = false;
                ls.startHoldSinceMs = now;
            } else if (!ls.startHoldFired && now - ls.startHoldSinceMs >= 1000) {
                ls.startHoldFired = true;
                std::fprintf(stdout, "[netplay] pause: Start held 1 s at network frame %u (seat %u): pausing for everyone (the press does not reach the game)\n",
                    runtime.frame, static_cast<unsigned>(seat + 1));
                setSeatPaused(runtime, ls, true, ", held START");
            }
        } else if (ls.startHoldActive) {
            ls.startHoldActive = false;
            if (!ls.startHoldFired) {
                startTap = true;
                std::fprintf(stdout, "[netplay] pause: Start held %llu ms (short): delayed Start press sent at network frame %u\n",
                    static_cast<unsigned long long>(now - ls.startHoldSinceMs), runtime.frame);
            } else {
                std::fprintf(stdout, "[netplay] pause: Start released after the hold that paused (press suppressed) at network frame %u\n",
                    runtime.frame);
            }
        }
    }
    if (!runtime.replaySamples.empty()) {
        const std::uint32_t applied = runtime.frame + runtime.inputDelay;
        if (applied < runtime.replaySamples.size() && runtime.replayPresent[applied]) {
            ls.pending = runtime.replaySamples[applied][seat < 2 ? seat : 0];
            // START inside a minigame is not a pause, it is an abort:
            // MGSeqPauseKill() ends the sequence. Generated inputs send it
            // blind because the instruction screen needs it, and then keep
            // sending it. Strip it once a minigame overlay is running.
            // Context is hashed, so both peers strip the same bit on the
            // same frame. Only replayed input is touched.
            const int overlay = PartyBoard_NetplayContextId();
            if (overlay >= kFirstMinigameOverlay && overlay <= kLastMinigameOverlay) {
                ls.pending.buttons &= ~PAD_BUTTON_START;
            }
        } else {
            // Past the end of the recording: hold neutral rather than let
            // a physical controller join a replay half way through.
            ls.pending = {};
            if (runtime.replayExhaustedFrame == UINT32_MAX) {
                runtime.replayExhaustedFrame = applied;
                std::printf("[NET TEST] replay_exhausted_frame=%u\n", applied);
            }
        }
    }
    if (runtime.menuProbe || runtime.walkProbe) {
        // Explicit real-game regression only. Send the host's input through
        // the normal input timeline; never advance an overlay directly.
        ls.pending = {};
        if (runtime.walkProbe) {
            // A deterministic walk, not a hand-written menu script: the
            // point is to cross many overlays, wipes and audio waits while
            // the canonical state proves both peers stayed identical. A
            // fixed menu path would break on the first layout change.
            // Both seats act. Selection screens wait for every player to
            // confirm, so a walk driven by the host alone would stall on the
            // first one. The offset keeps the two seats from always pressing
            // the same button on the same frame.
            const unsigned beat = runtime.frame + seat * 15u;
            const int context = runtime.observedContext;
            const unsigned held = runtime.frame - runtime.contextEntryFrame;
            const bool modeSelect = runtime.walkPlan
                && context == static_cast<int>(MenuProbeOverlay::modeseldll);
            const bool partySetup = runtime.walkPlan
                && context == static_cast<int>(MenuProbeOverlay::mentDll);
            // The minigame mode is a vertical list, not a horizontal one.
            const bool minigameList = runtime.walkPlan
                && context == static_cast<int>(MenuProbeOverlay::mgmodedll);
            if (modeSelect) {
                // The mode list. Its cursor starts on Party, and a single
                // horizontal pulse moves it; the free beat delivers one
                // every 240 frames, so which mode is chosen depends only on
                // how long this screen stayed open. Measured 2026-09-12:
                // 1102 frames here gave Party, 817 frames gave Story. Send
                // no horizontal at all, and the cursor cannot drift.
                // This list reads the stick on its first iteration and a
                // single A press leaves it, so every owed move must be
                // spent before any A is sent. It also reads pad 0 only
                // (HuPadDStkRep[0]), so seat 1 stays silent throughout.
                if (runtime.walkMenuReady && ls.walkModeLeft != 0) {
                    if (seat == 0) {
                        if (ls.walkPulseStart == UINT32_MAX) {
                            if (runtime.frame - ls.walkPulseEnd >= 60) {
                                ls.walkPulseStart = runtime.frame;
                            }
                        }
                        if (ls.walkPulseStart != UINT32_MAX) {
                            if (runtime.frame - ls.walkPulseStart < 3) {
                                ls.pending.stickX =
                                    ls.walkModeLeft < 0 ? -100 : 100;
                            } else {
                                ls.walkPulseEnd = runtime.frame;
                                ls.walkPulseStart = UINT32_MAX;
                                ls.walkModeLeft +=
                                    ls.walkModeLeft < 0 ? 1 : -1;
                            }
                        }
                    }
                } else if (held > 60 && beat % 30 < 3 && (beat / 30) % 4 == 0) {
                    ls.pending.buttons = PAD_BUTTON_A;
                }
            } else if (minigameList) {
                // Same rule as the board list, and for the same reason: a
                // frame window asked for row 44 and got row 0, because the
                // list was not on screen when the moves were spent.
                // Vertical here - this list scrolls, it does not slide.
                if (runtime.walkMenuReady && ls.walkNotchesLeft != 0) {
                    if (seat == 0) {
                        if (ls.walkPulseStart == UINT32_MAX) {
                            if (runtime.frame - ls.walkPulseEnd >= 60) {
                                ls.walkPulseStart = runtime.frame;
                            }
                        }
                        if (ls.walkPulseStart != UINT32_MAX) {
                            if (runtime.frame - ls.walkPulseStart < 3) {
                                ls.pending.stickY =
                                    ls.walkNotchesLeft < 0 ? 100 : -100;
                            } else {
                                ls.walkPulseEnd = runtime.frame;
                                ls.walkPulseStart = UINT32_MAX;
                                ls.walkNotchesLeft +=
                                    ls.walkNotchesLeft < 0 ? 1 : -1;
                            }
                        }
                    }
                } else if (beat % 30 < 3 && (beat / 30) % 4 == 0) {
                    ls.pending.buttons = PAD_BUTTON_A;
                }
            } else if (partySetup) {
                // Party setup. A carries the walk through its screens; the
                // board list is the one place a cursor has to move, and the
                // first attempt aimed its moves at a frame number. That
                // cannot work: the list reads the stick only once its six
                // panels have settled, and when that happens depends on how
                // long the preceding screens took to load. So the menu says
                // when it is listening, and the walk answers.
                if (runtime.walkMenuReady && ls.walkNotchesLeft != 0) {
                    // Seat 0 only: the two seats are 15 frames apart, and a
                    // shared cursor would move twice per intended notch.
                    // Neither seat presses A while a move is owed, or the
                    // list would be confirmed on the wrong board.
                    if (seat == 0) {
                        if (ls.walkPulseStart == UINT32_MAX) {
                            // 60 frames between pulses: the list needs 0x15
                            // frames to settle after each move before it
                            // accepts the next one.
                            if (runtime.frame - ls.walkPulseEnd >= 60) {
                                ls.walkPulseStart = runtime.frame;
                            }
                        }
                        if (ls.walkPulseStart != UINT32_MAX) {
                            if (runtime.frame - ls.walkPulseStart < 3) {
                                ls.pending.stickX =
                                    ls.walkNotchesLeft < 0 ? -100 : 100;
                            } else {
                                ls.walkPulseEnd = runtime.frame;
                                ls.walkPulseStart = UINT32_MAX;
                                ls.walkNotchesLeft +=
                                    ls.walkNotchesLeft < 0 ? 1 : -1;
                            }
                        }
                    }
                } else if (beat % 30 < 3 && (beat / 30) % 4 == 0) {
                    ls.pending.buttons = PAD_BUTTON_A;
                }
            } else if (runtime.walkPlan
                && context == static_cast<int>(MenuProbeOverlay::m444dll)) {
                // Reversal of Fortune's pinball launcher (m444 pinball.c): power
                // builds only while the stick is held down and drains 0.3 a frame
                // once released, and A shoots only with power above zero. The
                // generic walk taps the stick for 3 frames and presses A 90
                // frames later, so it never shot and a 10-turn test sat here for
                // 80,000 frames (2026-10-17). Hold the stick down 60 frames, then
                // A, every 120 frames; only the landing player's pad is read.
                const unsigned phase = held % 120u;
                static bool m444Noted = false;
                if (!m444Noted) {
                    m444Noted = true;
                    std::printf("[NET TEST] walk_plan: m444 launcher pattern (stick down 60 frames, then A) from frame %u\n", runtime.frame);
                }
                if (held > 60 && phase < 60) {
                    ls.pending.stickY = -100;
                } else if (held > 60 && phase >= 60 && phase < 63) {
                    ls.pending.stickY = -100;
                    ls.pending.buttons = PAD_BUTTON_A;
                } else if (beat % 30 < 3 && (beat / 30) % 4 == 0) {
                    ls.pending.buttons = PAD_BUTTON_A;
                }
            } else if (runtime.walkPlan && runtime.playerCount > 2
                && context == static_cast<int>(MenuProbeOverlay::instDll)) {
                // The instructions screen ORs all human pads and accepts
                // Start only when it is the sole pressed button. With four
                // offset walkers, another seat always sends A alongside
                // Start. Keep the other seats neutral on this screen.
                // This applies only to the explicit scripted test walk.
                // 8-player party: the group playing sits in slots 0-3 wired to its own
                // seats' controllers, so seat 0 may not be among them. Every seat then
                // presses Start alone on the same frames (the OR is still Start alone).
                if ((seat == 0 || runtime.party8) && held > 60 && runtime.frame % 120 < 3) {
                    ls.pending.buttons = PAD_BUTTON_START;
                }
            } else if (beat % 30 < 3) {
                switch ((beat / 30) % 8) {
                case 2:
                    // START every 240 frames was how this walk got past the
                    // title screen - and also how it opened the pause menu on
                    // every board and killed the sequence of every minigame it
                    // entered (objsysobj.c:86, MGSeqPauseKill). Valentin, who
                    // watches these runs, saw the game pausing constantly.
                    //
                    // A planned walk therefore presses START only where it is
                    // NEEDED, and presses A everywhere else.
                    //
                    // Where it is needed, measured: the boot screen, and the
                    // minigame instructions screen. instDll/main.c:295 leaves
                    // only on `btnDown == PAD_BUTTON_START` - an equality, so
                    // START alone and nothing with it - unless all four seats
                    // are CPU. With two human seats there is no other way out,
                    // and a walk without START sat on that screen for 308 000
                    // frames on 2026-09-13 before its budget ran out.
                    if (!runtime.walkPlan
                        || context == static_cast<int>(MenuProbeOverlay::bootDll)
                        || context == static_cast<int>(MenuProbeOverlay::instDll)) {
                        ls.pending.buttons = PAD_BUTTON_START;
                    } else {
                        ls.pending.buttons = PAD_BUTTON_A;
                    }
                    break;
                // The D-pad is masked out by HuPadRead; menus read the
                // analog stick through PadADConv.
                case 4: ls.pending.stickY = -100; break;
                // Measured: with no cancel at all the walk settles on the
                // first screen that needs one; with a cancel every eighth
                // beat it backs out of everything and covers less. Rare.
                case 5: if ((beat / 240) % 2) ls.pending.buttons = PAD_BUTTON_B; break;
                case 6: ls.pending.stickX = 100; break;
                default: ls.pending.buttons = PAD_BUTTON_A; break;
                }
            }
        } else if (seat == 0
            && runtime.observedContext == static_cast<int>(MenuProbeOverlay::bootDll)
            && runtime.frame >= 600 && runtime.frame % 120 < 2) {
            ls.pending.buttons = PAD_BUTTON_START;
        }
    }
    return startTap;
}

// Always on: which of this computer's controllers feeds each of its seats, at
// the start and whenever it changes (a controller plugged in, a phone joining).
void logSeatControllers(Runtime &runtime)
{
    for (std::uint8_t k = 0; k < runtime.localSeatCount; ++k) {
        LocalSeat &ls = runtime.local[k];
        char device[160];
        PartyBoard_PadPortDescribe(ls.pad, device, sizeof(device));
        if (ls.controller != device) {
            ls.controller = device;
            std::fprintf(stdout, "[netplay] seat %u <- this %s's controller port %u: %s (network frame %u).\n",
                static_cast<unsigned>(ls.seat + 1), partyboard::online::tailnet::kComputer,
                static_cast<unsigned>(ls.pad + 1), device, runtime.frame);
        }
    }
}

}
}

extern "C" bool PartyBoard_NetplayPreparePads(PADStatus status[8], u32 *rumble, bool startup)
{
    using namespace partyboard::netplay;
    if (!gRuntime.error.empty()) { serviceStateRepair(); return false; }
    if (!gRuntime.enabled || (!startup && !gRuntime.fullGame && !PartyBoard_NetplayIsMinigame())) {
        // Mark minigame-only sessions inactive and suspend their watchdog
        // while the local player is navigating unsynchronized menus.
        if (gRuntime.enabled && !startup) PartyBoard_NetplayTick();
        return true;
    }
    // The caller's table has 8 device ports (pad.c); a 4-player session reads and
    // writes the first four exactly as before.
    const std::size_t synced = gRuntime.party8 ? gSynchronizedPads.size() : 4u;
    // Every physical port is read: several people on one computer use this
    // computer's ports 1..n whatever the party size (only seats reach the game).
    std::copy_n(status, gPhysicalPads.size(), gPhysicalPads.begin());
    gSynchronizedPads = {};
    // Seats past the agreed player count are not in this session, and an unused
    // physical controller must not leak into the synchronized game on one machine
    // only. The bound follows playerCount rather than naming 2 and 3: with four
    // seats those two pads are players, not leftovers.
    for (std::size_t pad = gRuntime.playerCount; pad < gSynchronizedPads.size(); ++pad) {
        gSynchronizedPads[pad].err = PAD_ERR_NO_CONTROLLER;
    }
    if (!startup && !PartyBoard_NetplayTick()) return false;
    std::copy_n(gSynchronizedPads.begin(), synced, status);
    // Deterministic virtual capabilities; actual vibration is routed locally.
    *rumble = PAD_CHAN0_BIT | PAD_CHAN1_BIT;
    return true;
}

extern "C" void PartyBoard_NetplayControlMotor(u32 port, u32 command)
{
    if (PartyBoard_RollbackIsResimulating()) return;
    using namespace partyboard::netplay;
    if (gRuntime.enabled && (gRuntime.fullGame || PartyBoard_NetplayIsMinigame())) {
        if (port >= partyboard::rollback::kMaxPlayers || !isLocalSeat(gRuntime, static_cast<std::uint8_t>(port))) return;
        port = gRuntime.local[port - gRuntime.localPlayer].pad;
    }
    if (port < 4) PADControlMotor(port, command);
}

extern "C" bool PartyBoard_NetplayTick(void)
{
    using namespace partyboard::netplay;
    Runtime &runtime = gRuntime;
    if (!runtime.enabled) {
        return true;
    }
    if (!runtime.error.empty()) return false;

    const bool active = runtime.fullGame || PartyBoard_NetplayIsMinigame();
    if (!active) {
        runtime.progress = SessionProgress {};
        if (runtime.overlayActive) {
            std::fprintf(stdout,
                "Netplay: minigame ended (%u packets, %u rejected, %u waiting ticks, longest wait %u).\n",
                runtime.receivedPackets, runtime.rejectedPackets, runtime.stalledTicks,
                runtime.maximumStalledTicks);
            runtime.overlayActive = false;
        }
        return true;
    }
    runtime.progress.start(monotonicMs());
    writeDiagnostic("tick");
    const int gameContext = runtime.probeContext >= 0 ? runtime.probeContext : (runtime.fullGame
        ? PartyBoard_NetplayContextId()
        : PartyBoard_NetplayMinigameId());
    if (runtime.observedContext != gameContext) {
        runtime.observedContext = gameContext;
        runtime.contextEntryFrame = runtime.frame;
        // A menu that is no longer on screen is no longer listening, whatever
        // it last reported.
        runtime.walkMenuReady = false;
        runtime.rollbackProbeDone = false;
        runtime.rollbackContextReady = false;
        if (!runtime.rollbackSession) runtime.rollbackUnavailable = false;
        std::fprintf(stdout, "Netplay: game context %d at network frame %u.\n", gameContext, runtime.frame);
        PartyBoard_CrashBreadcrumb(PARTYBOARD_CRASH_CAT_OVERLAY,
            "context %d at frame %u (overlay %d minigame %d)", gameContext,
            runtime.frame, PartyBoard_NetplayContextId(), PartyBoard_NetplayMinigameId());
    }
    // A full game owns ONE input timeline and ONE initial RNG agreement.
    // Loading a module must not discard delayed inputs or authorize local ticks.
    // Minigame-only sessions still start a fresh timeline for each minigame.
    const std::uint8_t currentContext = runtime.fullGame ? 0 : static_cast<std::uint8_t>(gameContext);
    if (!runtime.overlayActive || runtime.contextId != currentContext) {
        if (runtime.overlayActive) {
            std::fprintf(stdout,
                "Netplay: context %u ended (%u packets, %u rejected, %u waiting ticks, longest wait %u).\n",
                static_cast<unsigned>(runtime.contextId), runtime.receivedPackets,
                runtime.rejectedPackets, runtime.stalledTicks, runtime.maximumStalledTicks);
        }
        resetOverlayTimeline(currentContext);
        runtime.overlayActive = true;
        std::fprintf(stdout,
            "Netplay: %s synchronization active (context %u, %u-frame jitter buffer).\n",
            runtime.fullGame ? "full-game" : "minigame",
            static_cast<unsigned>(runtime.contextId),
            static_cast<unsigned>(runtime.inputDelay));
    }

    if (runtime.rollbackProbe && !runtime.rollbackProbeDone
        && PartyBoard_NetplayIsMinigame()) {
        const auto probe = runRollbackSnapshotProbe();
        runtime.rollbackProbeDone = probe != SnapshotProbeResult::Busy;
        if (probe == SnapshotProbeResult::Failed) {
            std::fputs("Rollback probe: snapshot capture unavailable; lockstep remains active.\n",
                stderr);
        }
    }

    // Name the seat that went quiet: with up to 7 others "the connection was lost"
    // alone does not say whose.
    {
        const std::uint64_t now = monotonicMs();
        // Per computer: a computer that drops takes all its seats with it.
        for (std::uint8_t seat = 0; seat < runtime.playerCount; ++seat) {
            if (!isRemoteLead(runtime, seat) || runtime.peerHeardMs[seat] == 0) continue;
            const bool silent = now - runtime.peerHeardMs[seat] > 10000;
            if (silent != runtime.silentLogged[seat]) {
                runtime.silentLogged[seat] = silent;
                const std::uint8_t count = runtime.leadSeats[seat];
                const std::string who = seatBlockText(seat, count) + (count > 1 ? " (one computer)" : "");
                if (silent)
                    std::fprintf(stdout, "[netplay] %s silent for 10 s at network frame %u (lost connection or stopped?); "
                        "the game waits for %s, and stops after 2 minutes without progress.\n",
                        who.c_str(), runtime.frame, count > 1 ? "them" : "it");
                else
                    std::fprintf(stdout, "[netplay] %s heard again at network frame %u.\n", who.c_str(), runtime.frame);
            }
        }
    }
    // Check before receiving: late packets must not revive an expired session.
    const auto failure = runtime.progress.check(monotonicMs());
    if (failure != ProgressFailure::None) {
        const std::uint64_t now = monotonicMs();
        std::string seats;
        for (std::uint8_t seat = 0; seat < runtime.playerCount; ++seat) {
            if (!isRemoteLead(runtime, seat)) continue;
            const std::string who = seatBlockText(seat, runtime.leadSeats[seat]);
            char part[96];
            if (runtime.peerHeardMs[seat] == 0)
                std::snprintf(part, sizeof(part), " %s never heard;", who.c_str());
            else
                std::snprintf(part, sizeof(part), " %s last heard %llu s ago;", who.c_str(),
                    static_cast<unsigned long long>((now - runtime.peerHeardMs[seat]) / 1000));
            seats += part;
        }
        std::fprintf(stdout, "[netplay] stopped at network frame %u:%s\n", runtime.frame, seats.c_str());
        failSession(failure == ProgressFailure::NoPeer
            ? "No compatible player joined within 2 minutes." : "No progress for 2 minutes: the connection was lost or the game got stuck.");
        return false;
    }
    receivePendingPackets();
    serviceStateRepair();
    if (!runtime.error.empty()) return false;

    if (runtime.configMismatch) {
        failSession("Incompatible netplay configuration or random seeds");
        ++runtime.stalledTicks;
        return false;
    }

    const bool rollbackContextChanged = runtime.rollbackSession
        && runtime.rollbackSessionContext != runtime.observedContext;
    if (runtime.rollbackSession
        && (rollbackContextChanged || !PartyBoard_RollbackRenderCanReplayWithoutDraw())) {
        if (!runtime.rollbackSession->reconcile()) {
            failSession("Rollback transition correction failed");
            return false;
        }
        if (runtime.rollbackSession->confirmedFrame()
            != runtime.rollbackSession->currentFrame()) {
            const auto missingFrame = runtime.rollbackBaseFrame
                + runtime.rollbackSession->confirmedFrame();
            if (const auto *old = findInput(runtime.localHistory, missingFrame))
                sendInput(missingFrame, old->input, true);
            ++runtime.stalledTicks;
            ++runtime.consecutiveStalledTicks;
            return false;
        }
        const auto confirmedWireFrame = runtime.rollbackBaseFrame
            + runtime.rollbackSession->confirmedFrame();
        if (!PartyBoard_RollbackAudioBridgeConfirm(confirmedWireFrame)) {
            failSession("Rollback transition audio confirmation failed");
            return false;
        }
        if (!PartyBoard_RollbackAudioBridgeStop()) {
            failSession("Rollback transition audio teardown failed");
            return false;
        }
        runtime.rollbackSession.reset();
        runtime.rollbackPrepared = false;
        runtime.rollbackSessionContext = -1;
        // A wipe stays in lockstep until it reaches the next context. If the
        // context already changed, the matching-context handshake below is
        // sufficient to arm a fresh checkpoint window safely.
        runtime.rollbackUnavailable = !rollbackContextChanged;
        std::fprintf(stdout,
            "Rollback: transition reached at frame %u; confirmed state retained and lockstep resumed.\n",
            runtime.frame);
    }

    for (std::uint8_t k = 0; k < runtime.localSeatCount; ++k) serviceTestPause(runtime, runtime.local[k]);
    logPauseChanges(runtime);
    if (pauseHolding()) {
        if (runtime.countdownActive) {
            runtime.countdownActive = false;
            std::fprintf(stdout, "[netplay] pause: resume countdown cancelled at network frame %u (paused again)\n",
                runtime.frame);
        }
        runtime.wasHolding = true;
        servicePause(runtime);
        logPauseChanges(runtime);
        if (pauseHolding()) {
            ++runtime.stalledTicks;
            return false;
        }
    }
    if (runtime.wasHolding) {
        // The last pause was just lifted on this copy.
        runtime.wasHolding = false;
        if (contextIsMinigame(runtime.observedContext)) {
            runtime.countdownActive = true;
            runtime.countdownStartMs = monotonicMs();
            std::fprintf(stdout, "[netplay] pause: resume countdown started at network frame %u (context %d)\n",
                runtime.frame, runtime.observedContext);
        }
    }
    if (runtime.countdownActive) {
        if (monotonicMs() - runtime.countdownStartMs < 3000) {
            // A hold like the pause: no tick, so nothing can diverge; the others
            // simply wait for this copy's next input.
            ++runtime.stalledTicks;
            runtime.progress.commit(monotonicMs());
            return false;
        }
        runtime.countdownActive = false;
        std::fprintf(stdout, "[netplay] pause: resume countdown finished at network frame %u\n", runtime.frame);
    }

    if (!runtime.fullGame && runtime.peerInDifferentContext && runtime.progress.connected()) {
        // The transition input was already agreed on in the previous context.
        // Advertise our current context, release any held buttons and allow a
        // local transition tick. Once both peers report the same context,
        // normal lockstep (including RNG synchronization) resumes.
        const PartyBoardRollbackInput neutral {};
        sendInput(runtime.frame + runtime.inputDelay, neutral);
        PartyBoard_NetplayPadApplyRemote(0, &neutral, &runtime.lastLocal);
        PartyBoard_NetplayPadApplyRemote(1, &neutral, &runtime.lastPeer[soleRemoteSeat()]);
        runtime.lastLocal = neutral;
        runtime.lastPeer[soleRemoteSeat()] = neutral;
        runtime.localCaptured = false;
        runtime.consecutiveStalledTicks = 0;
        runtime.peerTimeoutReported = false;
        return true;
    }

    // Bound unvalidated progress without overwriting canonical evidence.
    bool everyStreamReady = true;
    for (std::uint8_t seat = 0; seat < runtime.playerCount; ++seat) {
        if (isRemoteLead(runtime, seat) && !runtime.states[seat].canCapture()) everyStreamReady = false;
    }
    if (!runtime.rollbackRequested && !everyStreamReady) {
        ++runtime.stalledTicks;
        return false;
    }
    if (!runtime.localCaptured) {
        if (runtime.frame >= runtime.controllerCheckFrame) {
            runtime.controllerCheckFrame = runtime.frame + 60;
            logSeatControllers(runtime);
        }
        // Seat by seat, in seat order: each with its own controller and pause.
        for (std::uint8_t k = 0; k < runtime.localSeatCount; ++k) {
            LocalSeat &ls = runtime.local[k];
            if (captureSeatInput(runtime, ls)) ls.pending.buttons |= PAD_BUTTON_START;
        }
        storeLocalInputs(runtime.localHistory, runtime.frame + runtime.inputDelay);
        runtime.localCaptured = true;
        // Advertise the captured future sample immediately, including while
        // waiting for the remote current tick. Otherwise both ends can withhold
        // information needed to release the other's delay buffer.
        sendInput(runtime.frame + runtime.inputDelay, runtime.local[0].pending);
    }

    if (!runtime.randomSynchronized || runtime.receivedPackets == 0) {
        sendInput(runtime.frame + runtime.inputDelay, runtime.local[0].pending);
        ++runtime.stalledTicks;
        ++runtime.consecutiveStalledTicks;
        return false;
    }

    if (runtime.rollbackRequested && ensureRollbackSession()) {
        return prepareRollbackTick();
    }

    PartyBoardRollbackInput local {};
    const InputSlot *localSlot = findInput(runtime.localHistory, runtime.frame);
    // A frame advances when every seat has spoken, not when one has. With two
    // players this is the same test written once more; with four it is the whole
    // difference between lockstep and a game that runs ahead of a silent peer.
    std::array<const InputSlot *, partyboard::rollback::kMaxPlayers> peerSlot {};
    const bool everyPeerReady = collectPeerInputs(runtime, runtime.frame, peerSlot);
    const bool startupFrame = runtime.frame < runtime.inputDelay;
    const bool frameReady = startupFrame || (localSlot != nullptr && everyPeerReady);
    if (!frameReady) {
        // Once a packet is lost, retransmit the exact frame the peer is also
        // waiting for. The future-frame packet was already sent on capture.
        if (localSlot != nullptr) {
            const auto now = monotonicMs();
            // Bound repair traffic while retrying lost UDP requests.
            if (runtime.repairRequestFrame != runtime.frame
                || now - runtime.lastRepairRequestMs >= 100) {
                sendInput(runtime.frame, localSlot->input, true);
                runtime.repairRequestFrame = runtime.frame;
                runtime.lastRepairRequestMs = now;
            }
        } else {
            sendInput(runtime.frame + runtime.inputDelay, runtime.local[0].pending);
        }
        ++runtime.stalledTicks;
        ++runtime.consecutiveStalledTicks;
        runtime.maximumStalledTicks = std::max(runtime.maximumStalledTicks,
            runtime.consecutiveStalledTicks);
        return false;
    }

    if (!startupFrame && runtime.fullGame) {
        bool contextDiffers = false;
        for (std::uint8_t seat = 0; seat < runtime.playerCount; ++seat) {
            if (!isRemoteLead(runtime, seat) || peerSlot[seat] == nullptr) continue;
            if (localSlot->captureContext != peerSlot[seat]->captureContext) contextDiffers = true;
        }
        if (contextDiffers) {
            // Module unload/load is driven by wall time outside the synchronized
            // input tick. One machine can therefore report -1 or the next module
            // briefly before the other. Only a persistent difference proves a
            // real divergent game path.
            if (++runtime.contextMismatchFrames >= kContextMismatchGraceFrames) {
                sendInput(runtime.frame, localSlot->input);
                failSession("The players' games were on different screens for 2 seconds.");
                return false;
            }
        } else {
            runtime.contextMismatchFrames = 0;
        }
    }

    // Repeat the current input as lightweight UDP redundancy. This also lets
    // a host that discovered its peer during the startup buffer provide the
    // earliest delayed frame that the client needs.
    if (!startupFrame && localSlot != nullptr) {
        sendInput(runtime.frame, localSlot->input);
    }
    runtime.consecutiveStalledTicks = 0;
    runtime.repairRequestFrame = UINT32_MAX;
    runtime.peerTimeoutReported = false;
    // Every seat's input for this frame, keyed by seat whichever computer owns
    // it: this computer's from its own slot, another's from that computer's slot.
    std::array<PartyBoardRollbackInput, partyboard::rollback::kMaxPlayers> applied {};
    if (!startupFrame) {
        for (std::uint8_t seat = 0; seat < runtime.playerCount; ++seat) {
            if (isLocalSeat(runtime, seat)) {
                applied[seat] = slotInput(*localSlot, seat - runtime.localPlayer);
                continue;
            }
            const std::uint8_t lead = runtime.seatLead[seat];
            if (peerSlot[lead] != nullptr) applied[seat] = slotInput(*peerSlot[lead], seat - lead);
        }
    }
    local = applied[runtime.localPlayer];
    const auto previousPeer = runtime.lastPeer;
    for (std::uint8_t seat = 0; seat < runtime.playerCount; ++seat) {
        if (seat == runtime.localPlayer) continue;
        runtime.lastPeer[seat] = applied[seat];
    }

    // Seats are applied in seat order rather than by role, so adding a third and
    // fourth pad is not a third and fourth branch.
    for (std::uint8_t seat = 0; seat < runtime.playerCount; ++seat) {
        if (seat == runtime.localPlayer) {
            PartyBoard_NetplayPadApplyRemote(seat, &local, &runtime.lastLocal);
        } else {
            PartyBoard_NetplayPadApplyRemote(seat, &applied[seat], &previousPeer[seat]);
        }
    }
    recordAppliedInputs(runtime.frame, applied);
    runtime.lastLocal = local;
    runtime.localCaptured = false;
    runtime.lockstepPrepared = !runtime.rollbackRequested;
    ++runtime.frame;
#if defined(PARTYBOARD_HD_BETA)
    captureFrameIfRequested(runtime.frame);
#endif
    runtime.progress.commit(monotonicMs());
    return true;
}

extern "C" bool PartyBoard_NetplayCommitTick(void)
{
    using namespace partyboard::netplay;
    Runtime &runtime = gRuntime;
    if (!runtime.enabled) return true;
    if (runtime.lockstepPrepared) {
        runtime.lockstepPrepared = false;
        return captureCommittedState();
    }
    if (!runtime.rollbackPrepared) return true;
    if (!runtime.rollbackSession
        || !PartyBoard_RollbackAudioBridgeFrameEnd()
        || !runtime.rollbackSession->complete()) {
        failSession("Rollback tick commit failed");
        return false;
    }
    runtime.rollbackPrepared = false;
    const auto confirmedWireFrame = runtime.rollbackBaseFrame
        + runtime.rollbackSession->confirmedFrame();
    if (!PartyBoard_RollbackAudioBridgeConfirm(confirmedWireFrame)) {
        failSession("Rollback confirmed audio failed");
        return false;
    }
    runtime.frame = runtime.rollbackBaseFrame
        + runtime.rollbackSession->currentFrame();
    runtime.localCaptured = false;
    runtime.progress.commit(monotonicMs());
    if (runtime.frame % 120 == 0) writeDiagnostic("rollback-checkpoint", true);
    return true;
}

extern "C" bool PartyBoard_NetplayCanPause(void)
{
    using namespace partyboard::netplay;
    return gRuntime.enabled && !gRuntime.rollbackRequested && gRuntime.playerCount > 1
        && gRuntime.error.empty();
}

// The menu's Pause / Resume (and the test-input socket): pausing holds the
// game for this computer's first seat; resuming lets go of every pause this
// computer's seats hold.
extern "C" void PartyBoard_NetplaySetPaused(bool paused)
{
    using namespace partyboard::netplay;
    if (!PartyBoard_NetplayCanPause()) return;
    if (paused) {
        setSeatPaused(gRuntime, gRuntime.local[0], true, ", menu");
        return;
    }
    for (std::uint8_t k = 0; k < gRuntime.localSeatCount; ++k)
        setSeatPaused(gRuntime, gRuntime.local[k], false, ", menu");
}

extern "C" unsigned PartyBoard_NetplayPausedSeats(void)
{
    using namespace partyboard::netplay;
    if (!PartyBoard_NetplayCanPause()) return 0;
    unsigned mask = 0;
    for (std::uint8_t seat = 0; seat < gRuntime.playerCount; ++seat) {
        const bool paused = isLocalSeat(gRuntime, seat) ? gRuntime.local[seat - gRuntime.localPlayer].paused
                                                        : gRuntime.peerPaused[seat];
        if (paused) mask |= 1u << seat;
    }
    return mask;
}

// True while the online game is inside a minigame (overlay.cpp hides the ping box there:
// minigames keep their record timer and scores in the corners).
extern "C" bool PartyBoard_NetplayInMinigame(void)
{
    using namespace partyboard::netplay;
    return gRuntime.enabled && contextIsMinigame(gRuntime.observedContext);
}

// 3, 2 or 1 while the resume countdown runs, else 0 (overlay.cpp shows it).
extern "C" int PartyBoard_NetplayResumeCountdown(void)
{
    using namespace partyboard::netplay;
    if (!gRuntime.countdownActive) return 0;
    const std::uint64_t elapsed = monotonicMs() - gRuntime.countdownStartMs;
    return elapsed >= 3000 ? 1 : 3 - static_cast<int>(elapsed / 1000);
}

extern "C" int PartyBoard_NetplayLocalSeat(void)
{
    return partyboard::netplay::gRuntime.localPlayer;
}

extern "C" int PartyBoard_NetplayLocalSeatCount(void)
{
    using namespace partyboard::netplay;
    return gRuntime.enabled ? gRuntime.localSeatCount : 1;
}

extern "C" unsigned PartyBoard_NetplayLocalSeatMask(void)
{
    using namespace partyboard::netplay;
    return ((1u << gRuntime.localSeatCount) - 1u) << gRuntime.localPlayer;
}

extern "C" bool PartyBoard_NetplayHasError(void)
{
    return !partyboard::netplay::gRuntime.error.empty();
}

extern "C" bool PartyBoard_NetplayWaiting(void) {
    using namespace partyboard::netplay;
    return gRuntime.enabled && gRuntime.progress.waiting(monotonicMs());
}

// True once a packet from the other side has ever been accepted. Lets the
// waiting notice say whether nobody has joined yet, or whether the peer was
// there and is gone - two different situations for whoever is watching.
extern "C" bool PartyBoard_NetplayPeerSeen(void)
{
    return partyboard::netplay::gRuntime.progress.connected();
}

extern "C" const char *PartyBoard_NetplayError(void)
{
    return partyboard::netplay::gRuntime.error.c_str();
}

extern "C" void PartyBoard_NetplayShutdown(void)
{
    using namespace partyboard::netplay;
    if (gRuntime.recordFile) {
        std::fclose(gRuntime.recordFile);
        gRuntime.recordFile = nullptr;
    }
    PartyBoard_RollbackAudioBridgeStop();
    gRuntime.transport.close();
    gRuntime = Runtime {};
}

static bool runNativePadRollbackSelfTest()
{
    // A native subsystem test, deliberately NOT a full-game snapshot: it
    // exercises the same PAD clamp/edge/repeat code used by the main loop,
    // real player statistics and both game RNG clocks. Graphics, processes,
    // audio, heaps and overlays require additional snapshot coverage.
    using namespace partyboard::rollback;
    u32 digest = 0;
    const auto gameBytes = PartyBoard_RollbackGameSize();
    const auto padBytes = HuPadSnapshotSizeGet();
    const auto bytes = gameBytes + padBytes + sizeof(PartyBoardRollbackClock) + sizeof(digest);
    Callbacks callbacks;
    callbacks.saveState = [&](void *destination, std::size_t size) {
        if (size != bytes) return false;
        auto *data = static_cast<u8 *>(destination);
        PartyBoardRollbackClock clock {};
        PartyBoard_RollbackClockSave(&clock);
        std::memcpy(data + gameBytes + padBytes, &clock, sizeof(clock));
        std::memcpy(data + bytes - sizeof(digest), &digest, sizeof(digest));
        return PartyBoard_RollbackGameSave(data, gameBytes)
            && HuPadSnapshotSave(data + gameBytes, padBytes);
    };
    callbacks.loadState = [&](const void *source, std::size_t size) {
        if (size != bytes) return false;
        const auto *data = static_cast<const u8 *>(source);
        PartyBoardRollbackClock clock {};
        std::memcpy(&clock, data + gameBytes + padBytes, sizeof(clock));
        if (clock.version != 1) return false;
        if (!HuPadSnapshotLoad(data + gameBytes, padBytes)
            || !PartyBoard_RollbackGameLoad(data, gameBytes) || !PartyBoard_RollbackClockLoad(&clock)) return false;
        std::memcpy(&digest, data + bytes - sizeof(digest), sizeof(digest));
        return true;
    };
    callbacks.simulateFrame = [&](u32 frame, const auto &inputs) {
        if (!PartyBoard_RollbackApplyPads(inputs.data(), 3))
            throw std::runtime_error("recorded PAD tick failed");
        for (int player = 0; player < 2; ++player) {
            GWCoinsAdd(player, (HuPadBtnDown[player] & PAD_BUTTON_A) ? 3 : -1);
            if (HuPadBtnDown[player] & PAD_BUTTON_B) GWStarsAdd(player, 1);
            digest = digest * 31u + HuPadBtnRep[player] + HuPadDStkRep[player]
                + HuPadBtnDown[player] + static_cast<u8>(HuPadStkX[player])
                + HuPadTrigL[player] + static_cast<u32>(GWCoinsGet(player));
        }
        PartyBoardRollbackClock clock {};
        PartyBoard_RollbackClockSave(&clock);
        // Deterministic updates exercise restoring both RNG states and the
        // frame clock; this does not impersonate the minigame simulation.
        clock.frandSeed = clock.frandSeed * 1664525u + 1013904223u + frame;
        clock.rand8Seed = static_cast<s32>(static_cast<u32>(clock.rand8Seed) * 33u + digest);
        ++clock.globalCounter;
        if (!PartyBoard_RollbackClockLoad(&clock))
            throw std::runtime_error("clock tick failed");
    };
    std::vector<u8> initial(bytes), expected(bytes), actual(bytes);
    if (!callbacks.saveState(initial.data(), bytes)) return false;
    bool passed = true;
    u32 replayed = 0, rollbacks = 0;
    const auto sample = [](u32 frame, unsigned player) {
        PartyBoardRollbackInput input {};
        input.buttons = frame % 48 < 30 ? PAD_BUTTON_A : PAD_BUTTON_B;
        input.stickX = static_cast<s8>(static_cast<int>((frame / 24 + player) % 3) * 70 - 70);
        input.stickY = static_cast<s8>(static_cast<int>(frame % 255) - 127);
        input.substickX = -100; input.substickY = 110;
        input.triggerLeft = frame % 256;
        input.triggerRight = (frame * 7) % 256;
        return input;
    };
    try {
        constexpr u32 frames = 720;
        Session reference({2, 12, bytes}, callbacks);
        for (u32 frame = 0; frame < frames; ++frame) {
            passed &= reference.submitInput(0, frame, sample(frame, 0));
            passed &= reference.submitInput(1, frame, sample(frame, 1));
            passed &= reference.advance();
        }
        passed &= callbacks.saveState(expected.data(), bytes);
        for (const u32 delay : {1u, 3u, 8u, 12u}) {
            passed &= callbacks.loadState(initial.data(), bytes);
            Session delayed({2, 12, bytes}, callbacks);
            for (u32 frame = 0; frame < frames; ++frame) {
                passed &= delayed.submitInput(0, frame, sample(frame, 0));
                if (frame >= delay)
                    passed &= delayed.submitInput(1, frame - delay, sample(frame - delay, 1));
                passed &= delayed.advance();
            }
            for (u32 frame = frames - delay; frame < frames; ++frame)
                passed &= delayed.submitInput(1, frame, sample(frame, 1));
            passed &= delayed.reconcile() && delayed.healthy();
            passed &= callbacks.saveState(actual.data(), bytes) && actual == expected;
            passed &= delayed.stats().rollbackCount > 0 && delayed.stats().maximumRollback <= delay;
            rollbacks += delayed.stats().rollbackCount;
            replayed += delayed.stats().resimulatedFrames;
        }
        // Invalid replay input must not partly change live PAD state.
        passed &= !PartyBoard_RollbackApplyPads(nullptr, 3);
        std::array<PartyBoardRollbackInput, 4> neutral {};
        passed &= !PartyBoard_RollbackApplyPads(neutral.data(), 0x80);
        passed &= callbacks.saveState(actual.data(), bytes) && actual == expected;
    } catch (...) { passed = false; }
    const bool restored = callbacks.loadState(initial.data(), bytes);
    passed &= restored && callbacks.saveState(actual.data(), bytes) && actual == initial;
    OSReport("Native PAD rollback: %s (720 frames, delays 1/3/8/12, %u corrections, %u replayed ticks, %zu bytes/state). Subsystems only.\n",
        passed ? "PASS" : "FAIL", rollbacks, replayed, bytes);
    return passed;
}

#include "netplay_state_test.inc"

extern "C" int PartyBoard_TargetFrameRateFor(bool netplayEnabled, int configured);

// Defect D6 - several simulation ticks batched into one rendered frame - can
// only occur above 60 fps, and online the pacer is clamped to 60 on both peers
// whatever the video setting says. That clamp is therefore the only thing
// keeping the frame rate's absence from `runtimeConfigSignature` safe: without
// it two peers on different rates would batch differently and diverge, with no
// signature mismatch to explain it.
//
// So the clamp is pinned here. If someone removes it to let netplay run at 144,
// this goes red at once instead of producing a desync a year later that nobody
// can trace back to a video setting.
static bool netplayFrameRateClampHolds()
{
    bool ok = true;
    for (const int configured : {30, 59, 60, 61, 120, 144, 240, 1000}) {
        ok &= PartyBoard_TargetFrameRateFor(true, configured) == 60;
    }
    // Offline the setting is honoured, within its own bounds. Checked too, so a
    // change that clamps everything to 60 - which would also make this pass -
    // is not mistaken for the property above.
    ok &= PartyBoard_TargetFrameRateFor(false, 144) == 144;
    ok &= PartyBoard_TargetFrameRateFor(false, 30) == 60;
    ok &= PartyBoard_TargetFrameRateFor(false, 1000) == 240;
    OSReport("Netplay frame rate clamp: %s (online forced to 60 for 8 settings; offline honours 144, floors 30, caps 1000 at 240).\n",
        ok ? "PASS" : "FAIL");
    return ok;
}

extern "C" bool PartyBoard_NetplayRuntimeRunSelfTest(void)
{
    // Exercise the actual availability check used by free_play.c, with both
    // saved unlock bits and the per-PC cheat setting deliberately different.
    partyboard::registerSettings();
    partyboard::config::FinishRegistration();
    using namespace partyboard::netplay;
    const bool enabled = gRuntime.enabled;
    auto &unlock = partyboard::getSettings().game.unlockAllMinigames;
    const auto savedUnlock = unlock;
    const auto savedStat = GWGameStat;
    bool availabilityPassed = true;
    auto &skipBoot = partyboard::getSettings().backend.skipBootSequence;
    const auto savedSkipBoot = skipBoot;
    for (const bool online : {false, true}) {
        gRuntime.enabled = online;
        for (const bool skip : {false, true}) {
            skipBoot.setValue(skip);
            availabilityPassed &= partyboard_settings_skipBootSequence() == (!online && skip)
                && skipBoot.getValue() == skip;
        }
    }
    skipBoot = savedSkipBoot;
    for (const bool online : {false, true}) {
        gRuntime.enabled = online;
        for (const bool cheat : {false, true}) {
            unlock.setValue(cheat);
            for (const u32 savedBits : {0u, 0x55555555u, 0xffffffffu}) {
                GWGameStat.mg_avail[0] = savedBits;
                GWGameStat.mg_avail[1] = ~savedBits;
                for (unsigned i = 0; i < 64; ++i) {
                    const bool savedAvailable = (GWGameStat.mg_avail[i / 32] & (1u << (i % 32))) != 0;
                    availabilityPassed &= (GWMGAvailGet(401 + i) != 0) == (online || cheat || savedAvailable);
                }
                availabilityPassed &= static_cast<u32>(GWGameStat.mg_avail[0]) == savedBits
                    && static_cast<u32>(GWGameStat.mg_avail[1]) == ~savedBits
                    && unlock.getValue() == cheat;
            }
        }
    }
    GWGameStat = savedStat;
    unlock = savedUnlock;
    gRuntime.enabled = enabled;
    OSReport("Netplay minigame availability: %s (768 checks, offline preferences preserved)\n",
        availabilityPassed ? "PASS" : "FAIL");
    const bool topologyPassed = HuPrcSnapshotTopologySelfTest();
    const bool gamePassed = PartyBoard_RollbackGameSelfTest();
    OSReport("Rollback process generation: %s\n", topologyPassed ? "PASS" : "FAIL");
    OSReport("Rollback central game state: %s\n", gamePassed ? "PASS" : "FAIL");
    // Every sub-test runs and every sub-test is named. This used to be a chain
    // of twenty-two `&&`, which stopped at the first failure: a single red
    // component hid every one after it, and a component became silently
    // untested the day anything ahead of it started failing. The cost of
    // running them all is a few milliseconds; the cost of not knowing which
    // ones ran is a release gate that cannot say what it checked.
    int total = 0;
    int failed = 0;
    const auto check = [&](const char *name, bool ok) {
        ++total;
        if (!ok) {
            ++failed;
            OSReport("  netplay sub-test FAIL: %s\n", name);
        }
    };
    check("canonical-state", runCanonicalStateSelfTest());
    check("seat-readiness", partyboard::netplay::seatReadinessSelfTest());
    check("timeline", partyboard::netplay::timelineSelfTest());
    check("progress", partyboard::netplay::progressSelfTest());
    check("pad-snapshot", HuPadSnapshotSelfTest());
    check("rollback-clock", PartyBoard_RollbackClockSelfTest());
    check("minigame-availability", availabilityPassed);
    check("native-pad-rollback", runNativePadRollbackSelfTest());
    check("process-topology", topologyPassed);
    check("central-game-state", gamePassed);
    check("frame-rate-clamp", netplayFrameRateClampHolds());
    check("animation-rollback", PartyBoard_AnimationRollbackSelfTest());
    check("rollback-io", PartyBoard_RollbackIOSelfTest());
    check("rollback-sequence", PartyBoard_RollbackSequenceSelfTest());
    check("process-execution", HuPrcSnapshotExecutionSelfTest());
    check("rollback-scene", PartyBoard_RollbackSceneSelfTest());
    check("rollback-resources", PartyBoard_RollbackResourcesSelfTest());
    check("rollback-checkpoint", PartyBoard_RollbackCheckpointSelfTest());
    check("wipe-safety", PartyBoard_RollbackWipeSafetySelfTest());
    check("render-safety", PartyBoard_RollbackRenderSafetySelfTest());
    check("rollback-audio", PartyBoard_RollbackAudioSelfTest());
    check("msm-stream-logical", msmStreamLogicalSelfTest());
    check("retrace-counter", PartyBoard_RetraceCounterSelfTest());
    check("thp-logical", PartyBoard_ThpLogicalSelfTest());
    const bool passed = failed == 0;
    OSReport("Netplay runtime self-test: %s (%d sub-tests, %d failed)\n",
        passed ? "PASS" : "FAIL", total, failed);
    return passed;
}

extern "C" bool PartyBoard_NetplayPadRunProbe(void)
{
    using namespace partyboard::netplay;
    if (!gRuntime.enabled || !gRuntime.fullGame) return false;
    // Two real processes exercise transport -> delay buffer -> raw PAD routing.
    // Synthetic inputs do not launch graphics or read/write a save file.
    // Long enough to expose relay queues that grow slowly under sustained play.
    constexpr unsigned frames = 2400;
    const auto sample = [](unsigned player, unsigned frame) {
        PartyBoardRollbackInput input {};
        input.buttons = (frame % 40 < 28) ? (player == 0 ? PAD_BUTTON_A : PAD_BUTTON_B) : 0;
        input.stickX = static_cast<s8>(static_cast<int>((frame + player * 17) % 161) - 80);
        input.stickY = static_cast<s8>(player == 0 ? 71 : -63);
        input.substickX = -39;
        input.substickY = 58;
        input.triggerLeft = static_cast<u8>(frame % 256);
        input.triggerRight = static_cast<u8>((frame * 3) % 256);
        return input;
    };
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(gRuntime.disconnectProbe ? 135 : gRuntime.realtimeProbe ? 150 : 90);
    unsigned frame = 0;
    bool loadingPauseDone = false;
    // Deterministic audio wait: both peers arm the same one-second stream at
    // frame 100, but their simulated audio threads finish it at very different
    // frames, exactly as two machines with different audio buffering do.
    constexpr unsigned kAudioArmFrame = 100;
    constexpr unsigned kAudioSampleRate = 32000;
    const unsigned physicalFinishFrame = gRuntime.localPlayer == 0 ? 120u : 200u;
    unsigned audioDoneFrame = 0;
    if (gRuntime.audioProbe && !msmStreamLogicalProbeInstall()) {
        std::fputs("[NET TEST] FAIL: a real stream table is already installed\n", stderr);
        return false;
    }
    while (frame < frames && std::chrono::steady_clock::now() < deadline) {
        // Revisit a context with inputs still in the delay buffer. The client
        // pauses alone to model wall-clock loading without extra simulation.
        gRuntime.probeContext = frame < 100 ? 0 : frame < 200 ? 7 : frame < 400 ? 3 : 7;
        if (gRuntime.contextMismatchProbe && gRuntime.localPlayer == 1 && frame >= 100)
            gRuntime.probeContext = 9;
        if (!loadingPauseDone && frame == 200 && gRuntime.localPlayer == 1) {
            loadingPauseDone = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(12000));
        }
        if (gRuntime.disconnectProbe && gRuntime.localPlayer == 1 && frame == 60) {
            // Abrupt UDP loss: no DISCONNECT packet and no normal shutdown.
            gRuntime.transport.close();
            std::puts("[NET TEST] client transport dropped at frame 60");
            return true;
        }
        PADStatus pads[4] {};
        for (auto& pad : pads) pad.err = PAD_ERR_NO_CONTROLLER;
        pads[gRuntime.localPad] = padFromInput(sample(gRuntime.localPlayer, frame));
        u32 rumble = 0;
        if (PartyBoard_NetplayPreparePads(pads, &rumble, false)) {
            if (gRuntime.frame != frame + 1) {
                std::fprintf(stderr, "[NET TEST] FAIL: timeline reset or uncommitted transition at frame %u\n", frame);
                return false;
            }
            // The online rumble mask is a constant, never the physical one.
            // HuPadRumbleGet() feeds RumbleBit, which is part of the rollback
            // snapshot and is NOT part of the canonical hash - so if this ever
            // started reflecting the controllers actually plugged in, two peers
            // would carry different pad state with nothing to report it. The
            // constant is what makes that exclusion safe, which makes it a
            // safety property rather than a detail, and safety properties get
            // pinned. See docs/canonical_hash_exclusions.md.
            if (rumble != static_cast<u32>(PAD_CHAN0_BIT | PAD_CHAN1_BIT)) {
                std::fprintf(stderr,
                    "[NET TEST] FAIL: online rumble mask is 0x%08x, expected the constant 0x%08x. "
                    "A physical-dependent mask diverges between machines and is not hashed.\n",
                    rumble, static_cast<u32>(PAD_CHAN0_BIT | PAD_CHAN1_BIT));
                return false;
            }
            // Every seat, not the first two: at four players the probe was only
            // checking half the session and would have passed a stack that dropped
            // seats 2 and 3 entirely.
            for (unsigned player = 0; player < gRuntime.playerCount; ++player) {
                const PartyBoardRollbackInput expected = frame < gRuntime.inputDelay
                    ? PartyBoardRollbackInput {} : sample(player, frame - gRuntime.inputDelay);
                if (pads[player].err != PAD_ERR_NONE
                    || !partyboard::rollback::inputsEqual(inputFromPad(pads[player]), expected)) {
                    std::fprintf(stderr, "[NET TEST] FAIL frame=%u port=%u\n", frame, player + 1);
                    return false;
                }
            }
            // Every seat in the session must have a controller, and every pad past it
            // must not. Naming 2 and 3 made this a two-player probe by construction:
            // at four seats it failed on the two pads that had just become players.
            for (unsigned pad = 0; pad < 4; ++pad) {
                const bool seated = pad < gRuntime.playerCount;
                if (seated == (pads[pad].err == PAD_ERR_NO_CONTROLLER)) return false;
            }
            // Real gameplay globals and RNGs, driven by verified logical inputs.
            // This remains a subsystem probe, not a board/minigame replay.
            for (unsigned player = 0; player < gRuntime.playerCount; ++player) {
                GWPlayer[player].coins = static_cast<s16>((GWPlayer[player].coins
                    + pads[player].triggerLeft + (pads[player].button & PAD_BUTTON_A ? 3 : 0)) % 999);
                GWPlayer[player].roll = static_cast<s8>(pads[player].stickX % 10);
            }
            if (gRuntime.audioProbe) {
                if (frame == kAudioArmFrame)
                    msmStreamLogicalProbeStart(0, kAudioSampleRate, kAudioSampleRate);
                if (frame == physicalFinishFrame) msmStreamLogicalProbeFinishPhysical(0);
                // The real loop advances this from PadReadSimulationTick, once
                // per accepted tick and before the canonical state is captured.
                msmStreamLogicalTick();
                if (frame > kAudioArmFrame && audioDoneFrame == 0
                    && msmStreamGetStatus(0) == 0) audioDoneFrame = frame;
            }
            if (frame == 0 || frame == 400) BoardRandInit();
            frand(); rand8(); BoardRand();
            if (gRuntime.desyncProbe && gRuntime.localPlayer == 1 && frame == 87)
                ++GWPlayer[0].coins;
            PartyBoard_NetplayCommitTick();
            ++GlobalCounter; // The real rendered path publishes this after commit.
            ++frame;
        }
        if (PartyBoard_NetplayHasError()) {
            if (gRuntime.contextMismatchProbe || gRuntime.desyncProbe) {
                const auto expected = gRuntime.desyncProbe ? 87u : 100u;
                const bool passed = reportedStates().error() == StateFailure::Desync
                    && reportedStates().errorFrame() == expected
                    && reportedStates().equalThrough() == expected;
                // Give the peer time to receive the retained failed digest.
                for (unsigned i = 0; i < 30; ++i) {
                    serviceStateRepair(true);
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                std::printf("[NET TEST] %s: first divergence hash_frame=%u stopped_frame=%u\n",
                    passed ? "PASS" : "FAIL", reportedStates().errorFrame(), frame);
                return passed;
            }
            if (!gRuntime.disconnectProbe || gRuntime.localPlayer != 0 || frame < 60
                || frame > 60u + gRuntime.inputDelay + 1u
                || gRuntime.progress.check(monotonicMs()) != ProgressFailure::Stalled) return false;
            const auto stoppedFrame = gRuntime.frame;
            for (unsigned retry = 0; retry < 100; ++retry) {
                if (PartyBoard_NetplayPreparePads(pads, &rumble, false)
                    || gRuntime.frame != stoppedFrame || !PartyBoard_NetplayEnabled()) return false;
            }
            std::printf("[NET TEST] PASS: peer loss detected, terminal stop at frame %u, no offline fallback\n", frame);
            return true;
        }
        // Windows coarse Sleep(1) can take 15.6 ms and invalidate the probe's
        // wall-time budget at delay zero. Precise waiting only affects this test.
        // Asymmetric wall clock: one peer runs visibly slower in real time.
        if (gRuntime.audioProbe && gRuntime.localPlayer == 1)
            SDL_DelayPrecise(5000000ull);
        SDL_DelayPrecise(gRuntime.realtimeProbe ? 16000000ull : 1000000ull);
    }
    // Drain final hashes/ACKs without simulating extra input. Keep responding
    // after local equality so the peer can also confirm the terminal frame.
    const auto drainEnd = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    auto settled = drainEnd;
    while (frame == frames && std::chrono::steady_clock::now() < drainEnd) {
        receivePendingPackets();
        serviceStateRepair();
        if (!gRuntime.error.empty()) return false;
        // Every peer, both directions. With one peer this is the same test; with
        // three, checking one of them would declare a session sound while two of
        // its streams had never agreed at all.
        bool allAgreed = true;
        for (std::uint8_t seat = 0; seat < gRuntime.playerCount; ++seat) {
            if (seat == gRuntime.localPlayer) continue;
            if (gRuntime.states[seat].equalThrough() != frames
                || gRuntime.states[seat].peerEqualThrough() != frames) allAgreed = false;
        }
        if (allAgreed) {
            if (settled == drainEnd) settled = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
            if (std::chrono::steady_clock::now() >= settled) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    bool allAgreedAtEnd = true;
    for (std::uint8_t seat = 0; seat < gRuntime.playerCount; ++seat) {
        if (seat == gRuntime.localPlayer) continue;
        if (gRuntime.states[seat].equalThrough() != frames
            || gRuntime.states[seat].peerEqualThrough() != frames) allAgreedAtEnd = false;
    }
    if (frame == frames && !allAgreedAtEnd)
        return false;
    if (gRuntime.audioProbe)
        std::printf("[NET TEST] audio_done_frame=%u physical_finish_frame=%u\n",
            audioDoneFrame, physicalFinishFrame);
    std::printf("[NET TEST] equal_states=%u peer_equal_states=%u\n",
        reportedStates().equalThrough(), reportedStates().peerEqualThrough());
    std::fprintf(stdout, "[NET TEST] %s: %u/%u frames, local physical PAD=%u, game port=%u\n",
        frame == frames ? "PASS" : "FAIL", frame, frames, gRuntime.localPad + 1, gRuntime.localPlayer + 1);
    return frame == frames;
}
