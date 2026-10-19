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
bool PartyBoard_NetplayWaiting(void);
/* True once a packet from the peer has ever been accepted. */
bool PartyBoard_NetplayPeerSeen(void);
const char *PartyBoard_NetplayError(void);
void PartyBoard_NetplayShutdown(void);
bool PartyBoard_NetplayRuntimeRunSelfTest(void);

#ifdef __cplusplus
}
#endif

#endif
