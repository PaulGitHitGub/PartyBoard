/* PartyBoard 8-player party: helpers for the end of the game (mstory3Dll:
   the ceremony in result_seq.c and the final results in result.c).

   The ceremony and the results were written for 4 players. In the 8-player
   party (GWSystem.eight_players) they run for all 8: Ms3Count() is 8 there and
   4 in every other game, so the original code paths (and their random numbers)
   stay exactly as they were in 4-player games. */
#include "REL/mstory3Dll.h"
#include "game/gamework_data.h"
#include "game/window.h"

#include <stdarg.h>
#include <stdio.h>

s32 Ms3Count(void)
{
    return GWPlayerCount();
}

s32 Ms3Eight(void)
{
    return GWPlayerCount() == 8;
}

/* The first human player's slot, or -1 (the original fn_1_36E4 for N players). */
s32 Ms3FirstHuman(void)
{
    s32 i;
    for (i = 0; i < Ms3Count(); i++) {
        if (!GWPlayerCfg[i].iscom) {
            return i;
        }
    }
    return -1;
}

/* Controller ports of the human players (bit p = port p). */
u8 Ms3HumanPadMask(void)
{
    u8 mask = 0;
    s32 i;
    for (i = 0; i < Ms3Count(); i++) {
        if (!GWPlayerCfg[i].iscom && GWPlayerCfg[i].pad_idx >= 0 && GWPlayerCfg[i].pad_idx < 8) {
            mask |= (u8)(1 << GWPlayerCfg[i].pad_idx);
        }
    }
    return mask;
}

/* Does this window advance by itself (computer key presses)? The original uses
   active_pad 0xFF to mean "nobody is human"; in the 8-player party 0xFF is
   also the mask of a game where all eight ports are human, so ask the players. */
s32 Ms3WinIsCom(s32 window)
{
    if (Ms3Eight()) {
        return Ms3FirstHuman() == -1;
    }
    return winData[window].active_pad == 0xFF;
}

const char *Ms3Name(s32 character)
{
    static const char *names[8] = { "Mario", "Luigi", "Peach", "Yoshi", "Wario", "DK", "Daisy", "Waluigi" };
    return names[character & 7];
}

/* New window text in the game's own message encoding (as mentDll's Ment8Text):
   space, punctuation, hyphen and apostrophe have their own codes in the font.
   Colour codes ("\x1e\x05" ...) pass through. */
const u8 *Ms3Text(s32 slot, const char *text)
{
    static u8 buffers[4][256];
    u8 *out = buffers[slot & 3];
    s32 n = 0;

    out[n++] = 0x0B;
    for (; *text != 0 && n < (s32)sizeof(buffers[0]) - 1; text++) {
        u8 c = (u8)*text;
        switch (c) {
            case ' ': c = 0x10; break;
            case ',': c = 0x82; break;
            case '.': c = 0x85; break;
            case '!': c = 0xC2; break;
            case '?': c = 0xC3; break;
            case '-': c = '='; break;
            case '\'': c = '\\'; break;
            case '(': c = ']'; break;
            case ')': c = '^'; break;
            case '\n': c = 0x0A; break;
            default: break;
        }
        out[n++] = c;
    }
    out[n] = 0;
    return out;
}

/* Always-on game.log line, "[8p] ..." (logging only, never changes the game). */
void Ms3Log(const char *fmt, ...)
{
    va_list args;
    fputs("[8p] ", stderr);
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
    fflush(stderr);
}
