#ifndef _GAME_PAD_H
#define _GAME_PAD_H

#include "dolphin.h"
#include "version.h"

#define PAD_BUTTON_DIR (PAD_BUTTON_LEFT | PAD_BUTTON_RIGHT | PAD_BUTTON_UP | PAD_BUTTON_DOWN)

#define PAD_BUTTON_TRIGGER_L 0x4000
#define PAD_BUTTON_TRIGGER_R 0x2000

/* Number of controller ports the game reads. The original code (minigames,
   menus) only ever uses ports 0-3. On PC, PartyBoard's 8-player party reads up
   to 8: on the board each player uses their own port (GWPlayer[i].port, 0-7);
   during a minigame the four players in it are wired to ports 0-3 (see
   HuPadWiringSet). */
#ifdef TARGET_PC
#define HU_PAD_PORT_MAX 8
#else
#define HU_PAD_PORT_MAX 4
#endif

/* What the game sees, per game ("logical") port. */
SHARED_SYM extern u16 HuPadBtn[HU_PAD_PORT_MAX];
SHARED_SYM extern u16 HuPadBtnDown[HU_PAD_PORT_MAX];
SHARED_SYM extern u16 HuPadBtnRep[HU_PAD_PORT_MAX];
SHARED_SYM extern s8 HuPadStkX[HU_PAD_PORT_MAX];
SHARED_SYM extern s8 HuPadStkY[HU_PAD_PORT_MAX];
SHARED_SYM extern s8 HuPadSubStkX[HU_PAD_PORT_MAX];
SHARED_SYM extern s8 HuPadSubStkY[HU_PAD_PORT_MAX];
SHARED_SYM extern u8 HuPadTrigL[HU_PAD_PORT_MAX];
SHARED_SYM extern u8 HuPadTrigR[HU_PAD_PORT_MAX];
SHARED_SYM extern u8 HuPadDStk[HU_PAD_PORT_MAX];
SHARED_SYM extern u8 HuPadDStkRep[HU_PAD_PORT_MAX];
extern s8 HuPadErr[HU_PAD_PORT_MAX];
/* Per device ("physical") port, before wiring. */
extern u16 _PadBtn[HU_PAD_PORT_MAX];
extern u16 _PadBtnDown[HU_PAD_PORT_MAX];
extern s32 VCounter;

void HuPadInit(void);
void HuPadRead(void);
#ifdef TARGET_PC
BOOL HuPadPollSimulationTick(void);
/* Simulation bookkeeping only. Restore at a quiescent tick boundary; device
 * handles/callbacks are excluded and hardware rumble is not replayed. */
size_t HuPadSnapshotSizeGet(void);
BOOL HuPadSnapshotSave(void *destination, size_t capacity);
BOOL HuPadSnapshotLoad(const void *source, size_t size);
BOOL HuPadSnapshotSelfTest(void);
#endif
void HuPadRumbleSet(s16 pad, s16 duration, s16 off, s16 on);
void HuPadRumbleStop(s16 pad);
void HuPadRumbleAllStop(void);
s16 HuPadStatGet(s16 pad);
u32 HuPadRumbleGet(void);

#ifdef TARGET_PC
/* 8-player party: port wiring. Game port k (HuPadBtn[k] etc., HuPadStatGet(k),
   HuPadRumbleSet(k, ...)) is fed by device port wire[k], and its rumble goes to
   that device. wire must hold each of 0..HU_PAD_PORT_MAX-1 once; a table that
   doesn't is refused (logged, wiring left as it was). Identity (port k <- device
   k) unless set, which keeps 4-player games exactly as before. The wiring only
   applies while GWSystem.eight_players is set; HuPadRead drops it otherwise.
   Set/reset stop all rumble first. Board code (board/main.c) wires the four
   players of a minigame group to ports 0-3 while the board is torn down. */
BOOL HuPadWiringSet(const s8 *wire);
void HuPadWiringReset(void);
BOOL HuPadWiringActive(void);
/* Device port feeding game port 'port' (port itself when not wired). */
s32 HuPadWiringDeviceGet(s32 port);
/* 8-player party: log every seat's device now ("[8p] input: seat N ... <- port
   P: ...") and the next first press on each port. HuPadRead re-checks the
   devices about once a second in 8-player games and logs any change. */
void HuPadEightSeatsChanged(void);
#endif

#endif
