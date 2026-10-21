#ifndef _BOARD_LOTTERY_H
#define _BOARD_LOTTERY_H

#include "dolphin/types.h"

void BoardLotteryHostSet(s16 arg0);
s16 BoardLotteryHostGet(void);
void BoardLotteryExec(void);
void BoardLotteryInit(void);
void BoardLotteryKill(void);
void BoardLotteryTutorialExec(void);
// PC port: the lottery machine's process is running (read-only; the scripted test walk uses it).
s32 BoardLotteryActive(void);
s32 BoardLotteryCrankActive(void);
// Read-only: the scratch-card hand exists; the scripted walker needs a sweep.
s32 BoardLotteryScratchActive(void);

#endif
