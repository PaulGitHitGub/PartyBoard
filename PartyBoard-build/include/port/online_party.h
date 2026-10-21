#ifndef PARTYBOARD_PORT_ONLINE_PARTY_H
#define PARTYBOARD_PORT_ONLINE_PARTY_H

/* Saving and continuing an online party (docs/ONLINE_CONTINUE.md). None of
   these change game state; they only read it and write files. */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A continue session (--netplay-continue): copies the party save the lobby gave
   every copy into buf (SAVE_BUF_SIZE bytes) and returns 1, the first time only;
   0 otherwise. */
int PartyBoard_OnlinePartyContinueSave(unsigned char *buf, unsigned size);
/* The 32-bit fold of the continued save's hash (META in the canonical state
   hash), 0 when this session continues nothing. */
uint32_t PartyBoard_OnlinePartyContinueHash32(void);
/* Continuing with another seat layout (--netplay-continue-map): the new online
   seat (= controller port) of the player who had online seat `oldSeat` when the
   party was saved, -1 when the map leaves that seat alone; *com set to 1 when that
   seat's computer is missing and its players continue as computer players. */
int PartyBoard_OnlinePartyContinueSeat(int oldSeat, int *com);
/* How many online seats the saved party had (the map's length), 0 without a map. */
int PartyBoard_OnlinePartyContinueMapSeats(void);
/* A fold of the map for the canonical state hash (META), 0 without a map. */
uint32_t PartyBoard_OnlinePartyContinueMapHash(void);
/* The online game reached a party save point: keep this save image (the
   memory-card file's 16 KB) and the roster in the "Online Party" folder.
   `where`: where a continued game picks up, for the log and party.json. */
void PartyBoard_OnlinePartyStore(const unsigned char *buf, unsigned size, const char *where);
/* The online party reached its final results: it can't be continued any more. */
void PartyBoard_OnlinePartyFinished(void);

#ifdef __cplusplus
}

#include <cstdint>
#include <string>

namespace partyboard::online::party {
constexpr unsigned kSaveBytes = 16384;
// FNV-1a 64 of the save image, as 16 hex digits (lobby and game alike).
std::uint64_t save_hash(const void *data, std::size_t size);
std::string hash_hex(std::uint64_t hash);
// --netplay-continue <hash>: reads and checks the continue file now, before the
// game starts; false (after a "[netplay] continue refused:" line) when it is
// missing, the wrong size or a different save.
bool arm_continue(const std::string &hashHex);
// --netplay-continue-map "4,5,6,7,0,1,2*,3*": the new seat of each saved seat, '*' = a
// computer player from now on. False (after a refusal line) when it isn't a permutation.
bool arm_continue_map(const std::string &map);
bool continuing();
} // namespace partyboard::online::party
#endif

#endif
