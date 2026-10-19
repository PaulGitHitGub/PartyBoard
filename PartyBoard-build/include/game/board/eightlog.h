#ifndef _BOARD_EIGHTLOG_H
#define _BOARD_EIGHTLOG_H

/* 8-player party: short, always-on lines in the session's game.log so a crash
   report from a real 8-player game shows who was grouped and moved where.
   Logging only; never changes gameplay or online state. Lines start "[8p]". */
#include "game/gamework_data.h"
#include <stdio.h>
#include <stdarg.h>

static inline const char *EightLogName(s32 slot)
{
    static const char *names[8] = { "Mario", "Luigi", "Peach", "Yoshi", "Wario", "DK", "Daisy", "Waluigi" };
    return names[GWPlayerCfg[slot].character & 7];
}

static inline void EightLog(const char *fmt, ...)
{
    va_list args;
    fputs("[8p] ", stderr);
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
    fflush(stderr);
}

/* "Mario(turn 1, team 2, COM, 0 stars 10 coins)" style list of the given slots. */
static inline void EightLogPlayers(const char *label, const s32 *slots, s32 count)
{
    char buf[1024];
    s32 len = 0;
    s32 i;
    for (i = 0; i < count && len < (s32)sizeof(buf) - 96; i++) {
        s32 s = slots[i];
        len += snprintf(buf + len, sizeof(buf) - len, "%s%s(turn %d", i ? ", " : "", EightLogName(s),
            (int)GWPlayerTurnOrderGet(&GWPlayer[s]) + 1);
        if (GWTeamGet()) {
            len += snprintf(buf + len, sizeof(buf) - len, ", team %d", (int)GWPlayerTeamGet(s));
        }
        len += snprintf(buf + len, sizeof(buf) - len, "%s, %d stars %d coins)", GWPlayer[s].com ? ", COM" : "",
            (int)GWPlayer[s].stars, (int)GWPlayer[s].coins);
    }
    EightLog("%s %s", label, buf);
}

#endif
