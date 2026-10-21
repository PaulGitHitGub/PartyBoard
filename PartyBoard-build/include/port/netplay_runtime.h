#ifndef PARTYBOARD_PORT_NETPLAY_RUNTIME_H
#define PARTYBOARD_PORT_NETPLAY_RUNTIME_H

#include "dolphin/pad.h"

#ifndef __cplusplus
#include <stdbool.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Experimental two-player LAN runtime. Its safe default synchronizes only
 * minigames; --netplay-full extends lockstep to menus and boards. It is enabled
 * only by command-line arguments, so the normal offline game remains untouched. */
bool PartyBoard_NetplayConfigureFromArgs(int argc, char **argv);
bool PartyBoard_NetplayEnabled(void);
/* Online ping display: round-trip time in ms to the given seat (0-7), -1 while
 * unknown or stale, -2 for our own seat or a seat outside the session. */
int PartyBoard_NetplayPeerPing(int seat);
int PartyBoard_NetplayPlayerCount(void);
/* True in an online 8-player party session (--netplay-party8): Party Mode plays
 * the 8-player party with the online seats as its people. */
bool PartyBoard_NetplayParty8(void);
/* Device ports the session synchronizes: 8 in an 8-player party, else 4 (ports
 * 5-8 then read as unplugged online). */
int PartyBoard_NetplaySyncedPorts(void);
// The netplay frame, for tagging diagnostics; defined in netplay_runtime.cpp.
u32 PartyBoard_NetplayFrameForDiagnostics(void);
/* One named boot/menu diagnostic event; no-op outside a network session. */
void PartyBoard_NetplayTrace(const char *event);
/* Rendered frames since boot. Reported, never hashed: it is a property of
   this machine, not of the game state. See docs/netplay_defect_register.md. */
extern unsigned int PartyBoard_RenderedFrames;
void PartyBoard_NetplayWalkMenu(int ready);
/* Non-zero when a draw hook may only run on a frame carrying a simulation
   tick. See D23 in docs/netplay_defect_register.md. */
int PartyBoard_HookTickGateEnabled(void);
/* Test-only override of the party turn count, 0 when unset. */
int PartyBoard_NetplayMaxTurnsOverride(void);
/* Companion-controlled start barrier, called only after game initialization. */
bool PartyBoard_OnlineWaitForStart(void);
bool PartyBoard_NetplayAllowsMultipleInstances(void);
/* False means that the current 60 Hz game tick must wait for its peer. */
bool PartyBoard_NetplayTick(void);
/* Called immediately after physical PADRead, before PADClamp/bookkeeping.
 * False leaves the game tick uncommitted. Startup advertises occupied ports
 * without starting a network timeline before the game has initialized. */
bool PartyBoard_NetplayPreparePads(PADStatus status[8], u32 *rumble, bool startup);
/* After accepted game logic: captures a canonical lockstep state, or completes
 * an experimental rollback-prepared tick. Offline remains a no-op. */
bool PartyBoard_NetplayCommitTick(void);
void PartyBoard_NetplayControlMotor(u32 port, u32 command);
bool PartyBoard_NetplayPadRunProbe(void);
bool PartyBoard_NetplayHasError(void);
/* Online pause: any player can hold the game for everyone; each player's pause
 * is their own to lift (the menu again, or Start). */
bool PartyBoard_NetplayCanPause(void);
void PartyBoard_NetplaySetPaused(bool paused);
/* 3, 2 or 1 while the minigame resume countdown runs after an online pause, else 0. */
int PartyBoard_NetplayResumeCountdown(void);
/* True while the online game is inside a minigame. */
bool PartyBoard_NetplayInMinigame(void);
/* Bit per seat currently pausing, 0 when nobody is. */
unsigned PartyBoard_NetplayPausedSeats(void);
/* This computer's first online seat (0-7). */
int PartyBoard_NetplayLocalSeat(void);
/* Several people on one computer (docs/ONLINE_MULTISEAT.md): how many seats
 * this computer owns (1 offline or with one person), and a bit per seat. */
int PartyBoard_NetplayLocalSeatCount(void);
unsigned PartyBoard_NetplayLocalSeatMask(void);
/* The computer owning `seat`: its first seat and how many seats it has (several
 * people on one computer). 0 for a seat outside the session. */
int PartyBoard_NetplaySeatComputer(int seat, int *firstSeat, int *seatCount);
// 1 when the computer owning online seat `seat` was voted out (drop vote: its seats are computer players now).
int PartyBoard_NetplaySeatDroppedByVote(int seat);
bool PartyBoard_NetplayWaiting(void);
/* Continue without a dropped player (docs/ONLINE_DROP_VOTE.md). While another
 * computer is silent (5 s), the banner's facts: the seats waited for, the time
 * until the game stops for everyone (the 2-minute watchdog), and the vote. */
typedef struct PartyBoardNetplayDropWait {
    unsigned seats;      /* bit per seat waited for */
    int msLeft, msTotal; /* until the game stops; msTotal = 120000 */
    int votes, voters, needed;
    unsigned localSeats; /* this computer's seats that can vote */
    unsigned localVoted; /* ... and those that did */
    int passed;          /* the vote passed: switching to computer players */
    char who[160];       /* "Valentin's PC (Players 5-8)" */
} PartyBoardNetplayDropWait;
bool PartyBoard_NetplayDropWaitGet(PartyBoardNetplayDropWait *out);
/* A seat whose controller is gone (its computer still plays): the first such seat. */
typedef struct PartyBoardNetplayControllerDrop {
    int seat;            /* 0-7 */
    int mine;            /* a seat of this computer */
    int votes, voters, needed;
    unsigned localVoted;
    unsigned localSeats; /* this computer's seats that can vote on it */
    int others;          /* more seats with a disconnected controller */
} PartyBoardNetplayControllerDrop;
bool PartyBoard_NetplayControllerDropGet(PartyBoardNetplayControllerDrop *out);
/* True while the drop banner speaks for a stall (another computer unheard for
 * 2 s or more in a session that can vote): the old waiting toast stays away. */
bool PartyBoard_NetplayDropBannerOwnsWait(void);
/* Seats now played by computer players after a drop (bit per seat). */
unsigned PartyBoard_NetplayComputerSeats(void);
/* The F1 menu's "Make Player N a computer" (this computer's own seat whose controller is gone). */
bool PartyBoard_NetplayDropMenuChoose(void);
/* True once a packet from the peer has ever been accepted. */
bool PartyBoard_NetplayPeerSeen(void);
const char *PartyBoard_NetplayError(void);
void PartyBoard_NetplayShutdown(void);
bool PartyBoard_NetplayRuntimeRunSelfTest(void);

#ifdef __cplusplus
}
#endif

#endif
