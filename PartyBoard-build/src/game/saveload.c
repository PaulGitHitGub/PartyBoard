#include "port/determinism.h"
#include "game/saveload.h"
#include "data_num/win.h"
#include "game/card.h"
#include "game/data.h"
#include "game/flag.h"
#include "game/gamework_data.h"
#include "game/pad.h"
#include "game/process.h"
#include "game/sprite.h"
#include "game/window.h"
#ifdef TARGET_PC
#include <stdio.h>
#endif

#include <stddef.h>
#include <string.h>

#ifdef TARGET_PC
#include <port/byteswap.h>
#endif
#include "port/version_runtime.h"
#include "port/pad_ports.h"

#if VERSION_ENG
#define SAVE_WRITE_BEGIN _SetFlag(FLAG_ID_MAKE(3, 0));
#define SAVE_WRITE_END _ClearFlag(FLAG_ID_MAKE(3, 0));
#else
#define SAVE_WRITE_BEGIN
#define SAVE_WRITE_END
#endif

#define SAVE_GET_PLAYER(player_idx) &saveBuf.buf[((player_idx) * sizeof(PlayerState)) + offsetof(SaveBufData, player)]
#define SAVE_GET_PLAYER_STORY(player_idx) &saveBuf.buf[((player_idx) * sizeof(PlayerState)) + offsetof(SaveBufData, playerStory)]

static s16 SLCreateSaveWin(void);
static void SLKillSaveWin(void);

extern u8 UnMountCnt;

SHARED_SYM CARDFileInfo curFileInfo;
SHARED_SYM SaveBufAll saveBuf;

u64 SLSerialNo;
SHARED_SYM s32 saveExecF;
u8 curBoxNo;
SHARED_SYM s16 curSlotNo;

#if VERSION_JP
static u8 commentTbl[2][32] = { "マリオパーティ４", "？？月？？日　データ？です。" };
static u8 sjisNumTbl[20] = "０１２３４５６７８９";
#else
static u8 commentTbl[2][32] = { "Mario Party 4", "File 0  00/00/0000" };
#endif

SHARED_SYM char *SaveFileNameTbl[] = { "MARIPA4BOX0", "MARIPA4BOX1", "MARIPA4BOX2" };

s32 SaveEnableF = 1;

static char *SlotNameTbl[] = { "A", "B" };

#if VERSION_JP
static char *slotIconMesTbl[] = { "\x0E\x16", "\x0E\x17" };
static u8 sjisSpace[] = "　";
#endif

static s32 saveMessWin = -1;

s32 SLFileOpen(char *fileName)
{
    s32 result;

    if (SaveEnableF == 0) {
        return 0;
    }
    while (1) {
        result = SLCardMount(curSlotNo);
        if (result < 0) {
            return result;
        }
        result = HuCardOpen(curSlotNo, fileName, &curFileInfo);
        if (result == CARD_RESULT_NOFILE) {
            return CARD_RESULT_NOFILE;
        }
        if (result == CARD_RESULT_WRONGDEVICE) {
            SLMessOut(7);
            return CARD_RESULT_FATAL_ERROR;
        }
        if (result == CARD_RESULT_FATAL_ERROR) {
            SLMessOut(1);
            return CARD_RESULT_FATAL_ERROR;
        }
        if (result == CARD_RESULT_NOCARD) {
            SLMessOut(0);
            return CARD_RESULT_NOCARD;
        }
        if (result == CARD_RESULT_BROKEN) {
            result = HuCardSectorSizeGet(curSlotNo);
            if (result > 0 && result != 8192) {
                SLMessOut(8);
                return CARD_RESULT_WRONGDEVICE;
            }
            UnMountCnt = 0;
            result = SLMessOut(5);
            if (result == 0) {
                result = SLFormat(curSlotNo);
                if (result != 0) {
                    return result;
                }
            }
            else {
                return CARD_RESULT_NOFILE;
            }
        }
        else {
            return CARD_RESULT_READY;
        }
    }
}

s32 SLFileCreate(char *fileName, u32 size, void *addr)
{
    float winSize[2];
    u32 byteNotUsed;
    u32 filesNotUsed;
    s32 window;
    s32 result;

    if (SaveEnableF == 0) {
        return 0;
    }
    result = SLCardMount(curSlotNo);
    if (result < 0) {
        return result;
    }
    result = HuCardSectorSizeGet(curSlotNo);
    if (result < 0 && result != 8192) {
        SLMessOut(8);
        return CARD_RESULT_FATAL_ERROR;
    }
    result = HuCardFreeSpaceGet(curSlotNo, &byteNotUsed, &filesNotUsed);
    if (filesNotUsed == 0 && size > byteNotUsed) {
        SLMessOut(4);
        return CARD_RESULT_INSSPACE;
    }
    if (filesNotUsed == 0) {
        SLMessOut(2);
        return CARD_RESULT_INSSPACE;
    }
    if (size > byteNotUsed) {
        SLMessOut(3);
        return CARD_RESULT_INSSPACE;
    }
    HuWinInit(1);
    HuWinMesMaxSizeGet(1, winSize, MAKE_MESSID(16, 11));
    window = HuWinExCreateStyled(-10000.0f, 200.0f, PB_F2S16(winSize[0]), PB_F2S16(winSize[1]), -1, 2);
    HuWinExAnimIn(window);
    HuWinMesSet(window, MAKE_MESSID(16, 11));
    HuWinMesWait(window);
    SAVE_WRITE_BEGIN
    result = HuCardCreate(curSlotNo, fileName, size, &curFileInfo);
    SAVE_WRITE_END
    if (result < 0) {
        HuWinExAnimOut(window);
        HuWinExCleanup(window);
    }
    if (result == CARD_RESULT_NOCARD) {
        SLMessOut(0);
        return CARD_RESULT_NOCARD;
    }
    if (result < 0) {
        SLMessOut(1);
        return CARD_RESULT_FATAL_ERROR;
    }
    SAVE_WRITE_BEGIN
    result = HuCardWrite(&curFileInfo, addr, size, 0);
    SAVE_WRITE_END
    if (result < 0) {
        HuWinExAnimOut(window);
        HuWinExCleanup(window);
    }
    if (result == CARD_RESULT_NOCARD) {
        SLMessOut(0);
        return CARD_RESULT_NOCARD;
    }
    if (result < 0) {
        SLMessOut(1);
        return CARD_RESULT_FATAL_ERROR;
    }
    SAVE_WRITE_BEGIN
    result = SLStatSet(1);
    SAVE_WRITE_END
    HuWinExAnimOut(window);
    HuWinExCleanup(window);
    if (result < 0) {
        return result;
    }
    return 0;
}

s32 SLFileWrite(s32 length, void *addr)
{
    float winSize[2];
    s32 window;
    s32 result;

    if (SaveEnableF == 0) {
        return 0;
    }
    HuWinInit(1);
    HuWinMesMaxSizeGet(1, winSize, MAKE_MESSID(16, 11));
    window = HuWinExCreateStyled(-10000.0f, 200.0f, PB_F2S16(winSize[0]), PB_F2S16(winSize[1]), -1, 2);
    HuWinExAnimIn(window);
    HuWinMesSet(window, MAKE_MESSID(16, 11));
    HuWinMesWait(window);
    HuPrcSleep(60);
    SAVE_WRITE_BEGIN
    result = HuCardWrite(&curFileInfo, addr, length, 0);
    if (result == 0) {
        result = SLStatSet(1);
    }
    SAVE_WRITE_END
    HuWinExAnimOut(window);
    HuWinExCleanup(window);
    return result;
}

s32 SLFileRead(s32 length, void *addr)
{
    s32 result;

    if (SaveEnableF == 0) {
        return 0;
    }
    result = HuCardRead(&curFileInfo, addr, length, 0);
    if (result == CARD_RESULT_NOCARD) {
        SLMessOut(0);
    }
    else if (result < 0) {
        SLMessOut(1);
    }
    return result;
}

s32 SLFileClose(void)
{
    s32 result;

    if (SaveEnableF == 0) {
        return 0;
    }
    result = HuCardClose(&curFileInfo);
    return result;
}

void SLCurSlotNoSet(s16 slotno)
{
    curSlotNo = slotno;
}

void SLCurBoxNoSet(s16 boxno)
{
    curBoxNo = boxno;
}

void SLSaveFlagSet(s32 flag)
{
    if (flag == 0) {
        GWGameStat.party_continue = 0;
        GWGameStat.story_continue = 0;
    }
    SaveEnableF = flag;
}

s32 SLSaveFlagGet(void)
{
    return SaveEnableF;
}

void SLSaveDataMake(s32 erase, OSTime *time)
{
    ANIMDATA *anim_data;
    u8 *buf;
    s32 i;

    buf = saveBuf.buf;
    if (erase != 0) {
        for (i = 0; i < SAVE_BUF_SIZE; i++) {
            buf[i] = 0xFF;
        }
    }
    for (i = 0; i < 0x20; i++) {
        buf[i] = (&commentTbl[0][0])[i];
    }
    for (i = 0; i < 0x20; i++) {
        (&buf[0x20])[i] = (&commentTbl[0][0])[i + 32];
    }
    anim_data = HuSprAnimReadFile(WIN_CARD_BANNER_ANM);
    memcpy(buf + offsetof(SaveBufData, banner), anim_data->bmp->data, CARD_BANNER_WIDTH * CARD_BANNER_HEIGHT);
    memcpy(buf + offsetof(SaveBufData, bannerTlut), anim_data->bmp->palData, 512);
    anim_data = HuSprAnimReadFile(curBoxNo + WIN_CARD_BOX1_ICON_ANM);
    memcpy(buf + offsetof(SaveBufData, icon), anim_data->bmp->data, CARD_ICON_WIDTH * CARD_ICON_HEIGHT * 4);
    memcpy(buf + offsetof(SaveBufData, iconTlut), anim_data->bmp->palData, 512);
    SLSaveDataInfoSet(time);
}

void SLSaveDataInfoSet(OSTime *time)
{
    s16 year;
    s16 digit;
    OSCalendarTime sp8;

    OSTicksToCalendarTime(*time, &sp8);
#if VERSION_ENG
#ifdef TARGET_PC
    // PAL discs write the date as DD/MM/YYYY in the memory card comment
    if (VERSION_RT_PAL) {
        saveBuf.data.comment[37] = curBoxNo + '1';
        digit = sp8.mday / 10;
        saveBuf.data.comment[40] = digit + '0';
        digit = sp8.mday % 10;
        saveBuf.data.comment[41] = digit + '0';
        digit = (sp8.mon + 1) / 10;
        saveBuf.data.comment[43] = digit + '0';
        digit = (sp8.mon + 1) % 10;
        saveBuf.data.comment[44] = digit + '0';
        year = sp8.year;
        digit = year / 1000;
        saveBuf.data.comment[46] = digit + '0';
        year -= digit * 1000;
        digit = year / 100;
        saveBuf.data.comment[47] = digit + '0';
        year -= digit * 100;
        digit = year / 10;
        saveBuf.data.comment[48] = digit + '0';
        year -= digit * 10;
        saveBuf.data.comment[49] = year + '0';
    }
    else {
#endif
    saveBuf.data.comment[37] = curBoxNo + '1';
    digit = (sp8.mon + 1) / 10;
    saveBuf.data.comment[40] = digit + '0';
    digit = (sp8.mon + 1) % 10;
    saveBuf.data.comment[41] = digit + '0';
    digit = sp8.mday / 10;
    saveBuf.data.comment[43] = digit + '0';
    digit = sp8.mday % 10;
    saveBuf.data.comment[44] = digit + '0';
    year = sp8.year;
    digit = year / 1000;
    saveBuf.data.comment[46] = digit + '0';
    year -= digit * 1000;
    digit = year / 100;
    saveBuf.data.comment[47] = digit + '0';
    year -= digit * 100;
    digit = year / 10;
    saveBuf.data.comment[48] = digit + '0';
    year -= digit * 10;
    saveBuf.data.comment[49] = year + '0';
#ifdef TARGET_PC
    }
#endif
#elif VERSION_PAL
    saveBuf.data.comment[37] = curBoxNo + '1';
    digit = sp8.mday / 10;
    saveBuf.data.comment[40] = digit + '0';
    digit = sp8.mday % 10;
    saveBuf.data.comment[41] = digit + '0';
    digit = (sp8.mon + 1) / 10;
    saveBuf.data.comment[43] = digit + '0';
    digit = (sp8.mon + 1) % 10;
    saveBuf.data.comment[44] = digit + '0';
    year = sp8.year;
    digit = year / 1000;
    saveBuf.data.comment[46] = digit + '0';
    year -= digit * 1000;
    digit = year / 100;
    saveBuf.data.comment[47] = digit + '0';
    year -= digit * 100;
    digit = year / 10;
    saveBuf.data.comment[48] = digit + '0';
    year -= digit * 10;
    saveBuf.data.comment[49] = year + '0';
#else
    saveBuf.data.comment[52] = sjisNumTbl[(curBoxNo + 1) * 2];
    saveBuf.data.comment[53] = sjisNumTbl[(curBoxNo + 1) * 2 + 1];
    digit = (sp8.mon + 1) / 10;
    if (digit == 0) {
        saveBuf.data.comment[32] = sjisSpace[0];
        saveBuf.data.comment[33] = sjisSpace[1];
    }
    else {
        saveBuf.data.comment[32] = sjisNumTbl[digit * 2];
        saveBuf.data.comment[33] = sjisNumTbl[digit * 2 + 1];
    }
    digit = (sp8.mon + 1) % 10;
    saveBuf.data.comment[34] = sjisNumTbl[digit * 2];
    saveBuf.data.comment[35] = sjisNumTbl[digit * 2 + 1];
    digit = sp8.mday / 10;
    if (digit == 0) {
        saveBuf.data.comment[38] = sjisSpace[0];
        saveBuf.data.comment[39] = sjisSpace[1];
    }
    else {
        saveBuf.data.comment[38] = sjisNumTbl[digit * 2];
        saveBuf.data.comment[39] = sjisNumTbl[digit * 2 + 1];
    }
    digit = sp8.mday % 10;
    saveBuf.data.comment[40] = sjisNumTbl[digit * 2];
    saveBuf.data.comment[41] = sjisNumTbl[digit * 2 + 1];
#endif
}

#ifdef TARGET_PC
#include "game/board/main.h"

/* PartyBoard 8-player party: save and continue.

   A save file is one 16 KB memory-card file (SAVE_BUF_SIZE). The game's own
   data (SaveBufData) and its 2-byte checksum fill the first ~9.2 KB; the rest
   is written and read with the file but never looked at. The party slot
   (saveBuf.data.system + player[4]) keeps exactly the original layout: GWSystem
   and players 0-3 as the slots stand. Players 4-7 and the board's round state
   (BoardEightResume) go into an "8-player block" in that unused tail:

     +0  'P','B','8','P'   +4 u16 version   +6 u16 block size
     +8  u32 hash of the party slot it belongs to (system + player[4], as on card)
     +12 u8 Party Mode continue flag (see below)
     +13 mg_phase, battle_phase, event_kind, bowser_loser[2], battle_player,
         battle_saved_curr (s8 each)  +20 s16 battle_mg  +22 reserved
     +24 PlayerState players 4-7 (big-endian, as the party slot)
     +216 u32 hash of bytes 0-215
   all big-endian. No new file, no change to the card layout or to 4-player
   saves (saving a 4-player party only clears the block's tag, if present).

   Older builds: their copy of GameStat.party_continue (in the checksummed
   part) is kept 0 while the party slot holds an 8-player party, and the real
   flag lives in the block. An older build therefore shows no party in progress
   in that file instead of loading half of an 8-player party as a 4-player
   game (8-player teams 0-3, 3-bit player numbers and turn places would not fit
   its 4-player code). It copies the whole 16 KB when it saves or copies the
   file, so the block survives; if it saves a different party into the slot,
   the hash no longer matches and the block is ignored. */
#define SL_EIGHT_OFS ((sizeof(SaveBufData) + 2 + 31) & ~(size_t)31)
#define SL_EIGHT_VERSION 1
#define SL_EIGHT_PLAYERS_OFS 24
#define SL_EIGHT_SUM_OFS (SL_EIGHT_PLAYERS_OFS + 4 * sizeof(PlayerState))
#define SL_EIGHT_SIZE (SL_EIGHT_SUM_OFS + 4)
typedef char SLEightBlockFits[(SL_EIGHT_OFS + SL_EIGHT_SIZE <= SAVE_BUF_SIZE) ? 1 : -1];

static u8 *SLEightBlock(void)
{
    return &saveBuf.buf[SL_EIGHT_OFS];
}

static u32 SLEightHash(const u8 *p, size_t n)
{
    u32 h = 2166136261u;
    size_t i;
    for (i = 0; i < n; i++) {
        h = (h ^ p[i]) * 16777619u;
    }
    return h;
}

static u32 SLEightPartyHash(void)
{
    return SLEightHash(&saveBuf.buf[offsetof(SaveBufData, system)],
        offsetof(SaveBufData, systemStory) - offsetof(SaveBufData, system));
}

static void SLEightPut16(u8 *p, u32 v)
{
    p[0] = (u8)(v >> 8);
    p[1] = (u8)v;
}

static void SLEightPut32(u8 *p, u32 v)
{
    p[0] = (u8)(v >> 24);
    p[1] = (u8)(v >> 16);
    p[2] = (u8)(v >> 8);
    p[3] = (u8)v;
}

static u32 SLEightGet16(const u8 *p)
{
    return ((u32)p[0] << 8) | p[1];
}

static u32 SLEightGet32(const u8 *p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

static BOOL SLEightTagged(void)
{
    const u8 *b = SLEightBlock();
    return b[0] == 'P' && b[1] == 'B' && b[2] == '8' && b[3] == 'P';
}

static void SLEightSumSet(void)
{
    u8 *b = SLEightBlock();
    SLEightPut32(b + SL_EIGHT_SUM_OFS, SLEightHash(b, SL_EIGHT_SUM_OFS));
}

/* NULL when the file's 8-player block is good and belongs to its party slot,
   else why not. */
static const char *SLEightBlockProblem(void)
{
    const u8 *b = SLEightBlock();
    if (!SLEightTagged()) {
        return "the file has no 8-player block";
    }
    if (SLEightGet16(b + 4) != SL_EIGHT_VERSION || SLEightGet16(b + 6) != SL_EIGHT_SIZE) {
        return "the 8-player block is from a newer build (unknown version)";
    }
    if (SLEightGet32(b + SL_EIGHT_SUM_OFS) != SLEightHash(b, SL_EIGHT_SUM_OFS)) {
        return "the 8-player block is damaged (checksum)";
    }
    if (SLEightGet32(b + 8) != SLEightPartyHash()) {
        return "the 8-player block belongs to an earlier save (the party slot was saved again since)";
    }
    if (!((SystemState *)&saveBuf.data.system)->eight_players) {
        return "the saved party is not an 8-player party";
    }
    return NULL;
}

static const char *SLEightCharName(s32 character)
{
    static const char *names[8] = { "Mario", "Luigi", "Peach", "Yoshi", "Wario", "DK", "Daisy", "Waluigi" };
    return names[character & 7];
}

/* Every slot's player, as saved or restored. */
static void SLEightPlayersLog(const char *what)
{
    s32 i;
    for (i = 0; i < 8; i++) {
        const PlayerState *p = &GWPlayer[i];
        fprintf(stderr,
            "[8p] %s:   slot %d %s: turn %d, team %d, seat %d port %d (now port %d), %s diff %d, %d stars %d coins, "
            "space %d, items %d/%d/%d, handicap %d, rank %d\n",
            what, (int)i + 1, SLEightCharName(p->character), (int)GWPlayerTurnOrderGet(p) + 1, (int)GWPlayerTeamGet(i),
            (int)GWPlayerSeatGet(p) + 1, (int)GWPlayerOwnPortGet(p) + 1, (int)p->port + 1, p->com ? "COM" : "human",
            (int)p->diff, (int)p->stars, (int)p->coins, (int)p->space_curr, (int)p->items[0], (int)p->items[1],
            (int)p->items[2], (int)p->handicap, (int)GWPlayerRankGet(i) + 1);
    }
    fflush(stderr);
}

/* Where a continued game picks up, for the log. */
static void SLEightResumeText(const BoardEightResume *r, char *buf, size_t size)
{
    const char *event = r->event_kind == 1 ? "Bowser" : "Battle";
    if (r->battle_phase == 1) {
        snprintf(buf, size, "round %d, between the two groups of a %s space minigame (m%d): continuing plays group B's",
            (int)GWSystem.turn, event, (int)r->battle_mg + 401);
    }
    else if (r->battle_phase == 2) {
        snprintf(buf, size, "round %d, both groups have played a %s space minigame (m%d): continuing finishes that turn",
            (int)GWSystem.turn, event, (int)r->battle_mg + 401);
    }
    else if (r->mg_phase == 1) {
        snprintf(buf, size, "round %d, group A has played the round's minigame: continuing plays group B's minigame",
            (int)GWSystem.turn);
    }
    else if (GWSystem.turn >= GWSystem.max_turn) {
        snprintf(buf, size, "after the last round's minigames (round %d of %d): continuing goes on to the end of the game",
            (int)GWSystem.turn, (int)GWSystem.max_turn);
    }
    else {
        snprintf(buf, size, "after round %d's minigames: continuing starts round %d of %d", (int)GWSystem.turn,
            (int)GWSystem.turn + 1, (int)GWSystem.max_turn);
    }
}

/* SLSaveBoard: write (8-player party) or retire (4-player party) the block. */
static void SLEightSave(void)
{
    BoardEightResume r;
    char where[200];
    u8 *b = SLEightBlock();
    s32 i;
    if (GWPlayerCount() != 8) {
        if (SLEightTagged()) {
            b[0] = b[1] = b[2] = b[3] = 0;
            fprintf(stderr, "[8p] save: 4-player party saved; the file's old 8-player block is cleared\n");
            fflush(stderr);
        }
        return;
    }
    BoardEightResumeGet(&r);
    memset(b, 0, SL_EIGHT_SIZE);
    b[0] = 'P';
    b[1] = 'B';
    b[2] = '8';
    b[3] = 'P';
    SLEightPut16(b + 4, SL_EIGHT_VERSION);
    SLEightPut16(b + 6, SL_EIGHT_SIZE);
    SLEightPut32(b + 8, SLEightPartyHash());
    b[12] = GWGameStat.party_continue;
    b[13] = (u8)r.mg_phase;
    b[14] = (u8)r.battle_phase;
    b[15] = (u8)r.event_kind;
    b[16] = (u8)r.bowser_loser[0];
    b[17] = (u8)r.bowser_loser[1];
    b[18] = (u8)r.battle_player;
    b[19] = (u8)r.battle_saved_curr;
    SLEightPut16(b + 20, (u16)r.battle_mg);
    for (i = 0; i < 4; i++) {
        PlayerState state = GWPlayer[4 + i];
        byteswap_playerstate(&state);
        memcpy(b + SL_EIGHT_PLAYERS_OFS + i * sizeof(PlayerState), &state, sizeof(PlayerState));
    }
    SLEightSumSet();
    SLEightResumeText(&r, where, sizeof(where));
    fprintf(stderr, "[8p] save: 8-player %s, board %d, round %d of %d, file %d; %s\n",
        GWTeamGet() ? "Tag Battle" : "Battle Royale", (int)GWSystem.board + 1, (int)GWSystem.turn, (int)GWSystem.max_turn,
        (int)curBoxNo + 1, where);
    fprintf(stderr, "[8p] save: player_curr %d, mg_phase %d, battle_phase %d, event %d, bowser losers %d/%d, "
        "lander %d, saved curr %d, battle mg %d; players 4-7 in the 8-player block at byte %d\n",
        (int)GWSystem.player_curr, (int)r.mg_phase, (int)r.battle_phase, (int)r.event_kind, (int)r.bowser_loser[0],
        (int)r.bowser_loser[1], (int)r.battle_player, (int)r.battle_saved_curr, (int)r.battle_mg, (int)SL_EIGHT_OFS);
    SLEightPlayersLog("save");
}

/* SLCommonSet: while the party slot holds an 8-player party, the card's own
   continue flag stays 0 (for older builds) and the block keeps the real one. */
static void SLEightContinueFlagSave(void)
{
    if (SLEightBlockProblem() != NULL) {
        return;
    }
    SLEightBlock()[12] = GWGameStat.party_continue;
    SLEightSumSet();
    saveBuf.data.stat.party_continue = 0;
    fprintf(stderr, "[8p] save: Party Mode continue flag %d kept in the 8-player block (file %d); the card's own "
        "flag stays off so older builds don't offer to continue an 8-player party\n",
        (int)GWGameStat.party_continue, (int)curBoxNo + 1);
    fflush(stderr);
}

/* SLLoadGameStat: the real continue flag of an 8-player party. */
static void SLEightContinueFlagLoad(void)
{
    if (SLEightBlockProblem() != NULL) {
        return;
    }
    GWGameStat.party_continue = SLEightBlock()[12] ? 1 : 0;
    if (GWGameStat.party_continue) {
        const SystemState *sys = &saveBuf.data.system;
        fprintf(stderr, "[8p] load: file %d holds an 8-player party in progress (round %d of %d)\n", (int)curBoxNo + 1,
            (int)sys->turn, (int)sys->max_turn);
        fflush(stderr);
    }
}

/* Continue: the saved party keeps its people and their controller ports (as
   the original); say which controllers are there now, so a party continued
   with other controllers connected is clear in the log. Logging only. */
static void SLEightControllersLog(void)
{
    u32 personPorts = 0;
    s32 missing = 0;
    s32 i;
    for (i = 0; i < 8; i++) {
        s32 port = GWPlayerOwnPortGet(&GWPlayer[i]);
        char device[96];
        s32 present;
        if (port < 0 || port >= 8) {
            port = GWPlayer[i].port & 7;
        }
        if (GWPlayer[i].com) {
            continue;
        }
        personPorts |= 1u << port;
        present = HuPadStatGet(port) != -1 && PartyBoard_PadPortDescribe((unsigned)port, device, sizeof(device)) != 0;
        if (!present) {
            PartyBoard_PadPortDescribe((unsigned)port, device, sizeof(device));
        }
        fprintf(stderr, "[8p] load: controllers: seat %d %s is a person on port %d: %s%s\n", (int)GWPlayerSeatGet(&GWPlayer[i]) + 1,
            SLEightCharName(GWPlayer[i].character), (int)port + 1, device, present ? "" : " (nothing there now)");
        if (!present) {
            missing++;
            fprintf(stderr, "[8p] load: WARNING: controllers: seat %d %s is a person but nothing is on port %d now; their turns and "
                "minigames wait for port %d (connect a controller there, or make the seat a computer player in the pause menu)\n",
                (int)GWPlayerSeatGet(&GWPlayer[i]) + 1, SLEightCharName(GWPlayer[i].character), (int)port + 1, (int)port + 1);
        }
    }
    for (i = 1; i < 8; i++) {
        char device[96];
        if (!(personPorts & (1u << i)) && HuPadStatGet(i) != -1 && PartyBoard_PadPortDescribe((unsigned)i, device, sizeof(device)) != 0) {
            fprintf(stderr, "[8p] load: controllers: port %d has %s but no person's seat uses it (the saved party plays it as before)\n",
                (int)i + 1, device);
        }
    }
    fprintf(stderr, "[8p] load: controllers: %s\n", missing ? "some people's controllers are missing (see WARNING lines)"
        : "every person's port has something on it");
}

/* SLLoadBoard, after players 0-3: players 4-7 and the round state. */
static void SLEightLoad(void)
{
    const u8 *b = SLEightBlock();
    BoardEightResume r;
    char where[200];
    s32 seen = 0;
    s32 i;
    for (i = 0; i < 4; i++) {
        s32 slot = 4 + i;
        memcpy(&GWPlayer[slot], b + SL_EIGHT_PLAYERS_OFS + i * sizeof(PlayerState), sizeof(PlayerState));
        byteswap_playerstate(&GWPlayer[slot]);
        GWPlayerCfg[slot].character = GWPlayer[slot].character;
        GWPlayerCfg[slot].pad_idx = GWPlayer[slot].port;
        GWPlayerCfg[slot].diff = GWPlayer[slot].diff;
        GWPlayerCfg[slot].iscom = GWPlayer[slot].com;
        GWPlayerCfg[slot].group = GWPlayerTeamGet(slot);
    }
    r.mg_phase = (s8)b[13];
    r.battle_phase = (s8)b[14];
    r.event_kind = (s8)b[15];
    r.bowser_loser[0] = (s8)b[16];
    r.bowser_loser[1] = (s8)b[17];
    r.battle_player = (s8)b[18];
    r.battle_saved_curr = (s8)b[19];
    r.battle_mg = (s16)SLEightGet16(b + 20);
    BoardEightResumeSet(&r);
    SLEightResumeText(&r, where, sizeof(where));
    fprintf(stderr, "[8p] load: 8-player %s, board %d, round %d of %d, file %d; %s\n",
        GWTeamGet() ? "Tag Battle" : "Battle Royale", (int)GWSystem.board + 1, (int)GWSystem.turn, (int)GWSystem.max_turn,
        (int)curBoxNo + 1, where);
    fprintf(stderr, "[8p] load: player_curr %d, mg_phase %d, battle_phase %d, event %d, bowser losers %d/%d, "
        "lander %d, saved curr %d, battle mg %d\n",
        (int)GWSystem.player_curr, (int)r.mg_phase, (int)r.battle_phase, (int)r.event_kind, (int)r.bowser_loser[0],
        (int)r.bowser_loser[1], (int)r.battle_player, (int)r.battle_saved_curr, (int)r.battle_mg);
    SLEightPlayersLog("load");
    /* The board copes with a missing turn order or port record (it logs and
       uses fixed halves / the slot's port), so only say so here. */
    for (i = 0; i < 8; i++) {
        s32 order = GWPlayerTurnOrderGet(&GWPlayer[i]);
        if (order >= 0 && order < 8) {
            seen |= 1 << order;
        }
        if (GWPlayerOwnPortGet(&GWPlayer[i]) < 0) {
            fprintf(stderr, "[8p] load: WARNING: slot %d %s has no seat/port record\n", (int)i + 1,
                SLEightCharName(GWPlayer[i].character));
        }
    }
    if (seen != 0xFF) {
        fprintf(stderr, "[8p] load: WARNING: the saved turn order is incomplete (places 0x%02x)\n", (unsigned)seen);
    }
    SLEightControllersLog();
    fflush(stderr);
}

#include "port/online_party.h"

/* Continuing an online party (docs/ONLINE_CONTINUE.md): set at online mode select
   when the party save is loaded, cleared when SLLoadBoard has loaded the board. */
static BOOL slOnlineBoardPending = FALSE;

BOOL SLOnlinePartyContinuePending(void)
{
    return slOnlineBoardPending;
}

/* After SLLoadBoard in a continue session with another seat layout (another
   computer hosts, or computer players for a missing computer): each player's
   controller port is their online seat, so move it to the new seat, and make the
   missing computer's people computer players. Same map on every copy. */
static void SLOnlinePartyContinueAdjust(void)
{
    const s32 seats = PartyBoard_OnlinePartyContinueMapSeats();
    const s32 count = GWPlayerCount();
    s32 i;
    if (seats <= 0) {
        fprintf(stdout, "[netplay] continue: the saved seats are used as they are (no seat map)\n");
        fflush(stdout);
        return;
    }
    for (i = 0; i < count; i++) {
        PlayerState *p = &GWPlayer[i];
        s32 com = 0;
        const s32 oldPort = p->port;
        const s32 ownPort = GWPlayerOwnPortGet(p);
        const s32 key = ownPort >= 0 ? ownPort : oldPort;
        const s32 newSeat = PartyBoard_OnlinePartyContinueSeat(key, &com);
        if (newSeat < 0) {
            continue;
        }
        /* The board port follows the seat only where it is the player's own port:
           an 8-player save made between the two groups' minigames keeps the
           wired slot ports ("now port" = the slot), and those stay as they are. */
        if (oldPort < seats && (ownPort < 0 || oldPort == ownPort)) {
            p->port = PartyBoard_OnlinePartyContinueSeat(oldPort, NULL);
        }
        if (ownPort >= 0) {
            GWPlayerPadInfoSet(p, GWPlayerSeatGet(p), newSeat);
        }
        if (com && !p->com) {
            /* Hard, like a player replaced after a drop vote (user's choice, 2026-10-19). */
            p->com = 1;
            p->diff = 2;
        }
        GWPlayerCfg[i].pad_idx = p->port;
        GWPlayerCfg[i].iscom = p->com;
        GWPlayerCfg[i].diff = p->diff;
        fprintf(stdout, "[netplay] continue: slot %d %s: saved online seat %d -> %s %d (port %d -> %d)%s\n", (int)i + 1,
            SLEightCharName(p->character), (int)key + 1, com ? "computer player on port" : "online seat", (int)newSeat + 1,
            (int)oldPort + 1, (int)p->port + 1, com ? ", COM hard" : "");
    }
    fflush(stdout);
}
#endif

void SLCommonSet(void)
{
    OSTime create_time;

    create_time = OSGetTime();
    GWGameStat.create_time = create_time;
    memcpy(&saveBuf.data.stat, &GWGameStat, sizeof(GameStat));
#ifdef TARGET_PC
    byteswap_gamestat(&saveBuf.data.stat);
    SLEightContinueFlagSave();
#endif
    SLSaveDataInfoSet(&create_time);
}

void SLSaveBoard(void)
{
    s16 i;

    memcpy(&saveBuf.data.system, &GWSystem, sizeof(SystemState));
#ifdef TARGET_PC
    byteswap_systemstate(&saveBuf.data.system);
#endif
    for (i = 0; i < 4; i++) {
        memcpy(SAVE_GET_PLAYER(i), &GWPlayer[i], sizeof(PlayerState));
#ifdef TARGET_PC
        byteswap_playerstate((PlayerState*)SAVE_GET_PLAYER(i));
#endif
    }
#ifdef TARGET_PC
    SLEightSave();
#endif
}

void SLSaveBoardStory(void)
{
    s16 i;

    memcpy(&saveBuf.data.systemStory, &GWSystem, sizeof(SystemState));
#ifdef TARGET_PC
    byteswap_systemstate(&saveBuf.data.systemStory);
#endif
    for (i = 0; i < 4; i++) {
        memcpy(SAVE_GET_PLAYER_STORY(i), &GWPlayer[i], sizeof(PlayerState));
#ifdef TARGET_PC
        byteswap_playerstate((PlayerState*)SAVE_GET_PLAYER_STORY(i));
#endif
    }
}

s32 SLSave(void)
{
    s32 result;

    while (1) {
        SLCheckSumSet();
        result = SLFileOpen(SaveFileNameTbl[curBoxNo]);
        if (result == CARD_RESULT_NOFILE) {
            if (!SLSerialNoCheck()) {
                SLMessOut(9);
            }
            else {
                SLCreateSaveWin();
                result = SLFileCreate(SaveFileNameTbl[curBoxNo], 16384, &saveBuf);
                SLKillSaveWin();
                if (result >= 0) {
                    SLSerialNoGet();
                    goto block_32;
                }
            }
        }
        else {
            if (result == CARD_RESULT_NOCARD) {
                result = SLMessOut(10);
                if (result != 0) {
                    SLMessOut(11);
                    continue;
                }
                SLSaveFlagSet(0);
                break;
            }
            if (result >= 0) {
                if (!SLSerialNoCheck()) {
                    SLMessOut(9);
                }
                else {
                    SLCreateSaveWin();
                    result = SLFileWrite(16384, &saveBuf);
                    SLKillSaveWin();
                    if (result == CARD_RESULT_NOCARD) {
                        SLMessOut(0);
                    }
                    else if (result == CARD_RESULT_WRONGDEVICE) {
                        SLMessOut(7);
                    }
                    else if (result == CARD_RESULT_BROKEN) {
                        result = HuCardSectorSizeGet(curSlotNo);
                        if (result > 0 && result != 8192) {
                            SLMessOut(8);
                            goto block_36;
                        }
                        UnMountCnt = 0;
                        result = SLMessOut(5);
                        if (result == 0) {
                            result = SLFormat(curSlotNo);
                            if (result) {
                                return result;
                            }
                            continue;
                        }
                        else {
                            result = CARD_RESULT_BROKEN;
                        }
                    }
                    else if (result < 0) {
                        SLMessOut(1);
                    }
                block_32:
                    SLFileClose();
                    if (result >= 0) {
#ifdef TARGET_PC
                        if (SLEightBlockProblem() == NULL) {
                            fprintf(stderr, "[8p] save: written to memory card slot %s, file %d (8-player block included, continue flag %d)\n",
                                SlotNameTbl[curSlotNo & 1], (int)curBoxNo + 1, (int)SLEightBlock()[12]);
                            fflush(stderr);
                        }
#endif
                        HuCardUnMount(curSlotNo);
                        return 1;
                    }
                }
            }
        }
    block_36:
        result = SLMessOut(10);
        if (result != 0) {
            SLMessOut(11);
        }
        else {
            SLSaveFlagSet(0);
            break;
        }
    }
    HuCardUnMount(curSlotNo);
    return 0;
}

#ifdef TARGET_PC
// PAL messages are taller: the window position follows the loaded disc
#define SAVEWIN_POS (VERSION_RT_PAL ? 120 : 150)
#elif VERSION_NTSC
#define SAVEWIN_POS 150
#else
#define SAVEWIN_POS 120
#endif

#if VERSION_JP
#define SAVEWIN_MESS slotIconMesTbl
#else
#define SAVEWIN_MESS SlotNameTbl
#endif

static s16 SLCreateSaveWin(void)
{
    float size[2];
    s16 window;

    HuWinInit(1);
    HU_WIN_INSERT_MES_SIZE_GET_PTR(MAKE_MESSID_PTR(SAVEWIN_MESS[curSlotNo]), 0);
    HuWinMesMaxSizeGet(1, size, MAKE_MESSID(16, 68));
    window = HuWinExCreateStyled(-10000.0f, SAVEWIN_POS, PB_F2S16(size[0]), PB_F2S16(size[1]), -1, 2);
    saveMessWin = window;
    HuWinExAnimIn(window);
    HU_WIN_INSERT_MES_SET_PTR(window, MAKE_MESSID_PTR(SAVEWIN_MESS[curSlotNo]), 0);
    HuWinMesSet(window, MAKE_MESSID(16, 68));
    HuWinMesWait(window);
    return window;
}

static void SLKillSaveWin(void)
{
    if (saveMessWin != -1) {
        HuWinExAnimOut(saveMessWin);
        HuWinExCleanup(saveMessWin);
        saveMessWin = -1;
    }
}

s32 SLLoad(void)
{
    s32 result;
    u16 *save_checksum;
    u16 checksum;

    result = SLFileOpen(SaveFileNameTbl[curBoxNo]);
    if (result >= 0) {
        result = SLFileRead(16384, &saveBuf);
        SLFileClose();
        if (result >= 0) {
            save_checksum = (u16 *)&saveBuf.buf[sizeof(SaveBufData)];
            checksum = SLCheckSumGet();
            *save_checksum == checksum;
        }
    }
    HuCardUnMount(curSlotNo);
    return 0;
}

void SLLoadGameStat(void)
{
    memcpy(&GWGameStat, &saveBuf.data.stat, sizeof(GameStat));
#ifdef TARGET_PC
    byteswap_gamestat(&GWGameStat);
    SLEightContinueFlagLoad();
#endif
}

#ifdef TARGET_PC
/* The party slot holds 4 players; an 8-player party also needs the file's
   8-player block (players 4-7, see SLEightSave). The 8-player bit sits in what
   was padding, and saves made before the 8-player work can have it set (seen
   2026-10-16: continuing such a save crashed in BoardPlayerModelInit), so
   without a good block a loaded game is the 4-player party. Returns TRUE when
   the game loads as the 8-player party. */
static BOOL SLLoadEightPlayersCheck(const char *which, BOOL blockAllowed)
{
    const char *problem;
    if (!GWSystem.eight_players) {
        return FALSE;
    }
    problem = blockAllowed ? SLEightBlockProblem() : "story saves are 4-player only";
    if (problem == NULL) {
        return TRUE;
    }
    fprintf(stderr, "[8p] WARNING: %s save had the 8-player bit set but %s; loading it as a 4-player game (players 1-4 of the slots)\n",
        which, problem);
    fflush(stderr);
    GWSystem.eight_players = 0;
    return FALSE;
}
#endif

void SLLoadBoard(void)
{
    s16 i;
#ifdef TARGET_PC
    BOOL eight;
#endif

    memcpy(&GWSystem, &saveBuf.data.system, sizeof(SystemState));
#ifdef TARGET_PC
    byteswap_systemstate(&GWSystem);
    eight = SLLoadEightPlayersCheck("party", TRUE);
#endif
    for (i = 0; i < 4; i++) {
        memcpy(&GWPlayer[i], SAVE_GET_PLAYER(i), sizeof(PlayerState));
#ifdef TARGET_PC
        byteswap_playerstate(&GWPlayer[i]);
#endif
        GWPlayerCfg[i].character = GWPlayer[i].character;
        GWPlayerCfg[i].pad_idx = GWPlayer[i].port;
        GWPlayerCfg[i].diff = GWPlayer[i].diff;
        GWPlayerCfg[i].iscom = GWPlayer[i].com;
        GWPlayerCfg[i].group = GWPlayerTeamGet(i);
    }
#ifdef TARGET_PC
    if (eight) {
        SLEightLoad();
    }
    else {
        BoardEightResumeClear();
    }
    if (slOnlineBoardPending) {
        slOnlineBoardPending = FALSE;
        SLOnlinePartyContinueAdjust();
    }
#endif
}

void SLLoadBoardStory(void)
{
    s16 i;

    memcpy(&GWSystem, &saveBuf.data.systemStory, sizeof(SystemState));
#ifdef TARGET_PC
    byteswap_systemstate(&GWSystem);
    SLLoadEightPlayersCheck("story", FALSE);
#endif
    for (i = 0; i < 4; i++) {
        memcpy(&GWPlayer[i], SAVE_GET_PLAYER_STORY(i), sizeof(PlayerState));
#ifdef TARGET_PC
        byteswap_playerstate(&GWPlayer[i]);
#endif
        GWPlayerCfg[i].character = GWPlayer[i].character;
        GWPlayerCfg[i].pad_idx = GWPlayer[i].port;
        GWPlayerCfg[i].diff = GWPlayer[i].diff;
        GWPlayerCfg[i].iscom = GWPlayer[i].com;
        GWPlayerCfg[i].group = 0;
    }
}

s32 SLSerialNoGet(void)
{
    return CARDGetSerialNo(curSlotNo, &SLSerialNo);
}

BOOL SLSerialNoCheck(void)
{
    s32 result;
    u64 serialNo;

    if (SLSerialNo == 0) {
        return TRUE;
    }
    result = CARDGetSerialNo(curSlotNo, &serialNo);
    if (result < 0) {
        return TRUE;
    }
    if (serialNo != SLSerialNo) {
        return FALSE;
    }
    return TRUE;
}

BOOL SLCheckSumCheck(void)
{
    u16 *save_checksum = (u16 *)&saveBuf.buf[sizeof(SaveBufData)];
    u16 checksum = SLCheckSumGet();
#ifdef TARGET_PC
    byteswap_u16(&checksum);
#endif
    if (*save_checksum == checksum) {
        return TRUE;
    }
    return FALSE;
}

u16 SLCheckSumGet(void)
{
    u32 i;
    u32 checksum;

    for (i = checksum = 0; i < sizeof(SaveBufData); i++) {
        checksum += saveBuf.buf[i];
    }
    checksum = ~checksum;
    return (u16)checksum & 0xFFFF;
}

void SLCheckSumSet(void)
{
    u16 checksum = SLCheckSumGet();

    saveBuf.buf[sizeof(SaveBufData)] = (checksum >> 8) & 0xFF;
    saveBuf.buf[sizeof(SaveBufData) + 1] = checksum;
}

s32 SLStatSet(s32 reportF)
{
    CARDStat stat;
    s32 fileNo;
    s32 result;

    fileNo = curFileInfo.fileNo;
    result = CARDGetStatus(curSlotNo, fileNo, &stat);
    if (result == CARD_RESULT_NOCARD) {
        if (reportF != 0) {
            SLMessOut(0);
        }
        return -3;
    }
    if (result < 0) {
        if (reportF != 0) {
            SLMessOut(1);
        }
        return CARD_RESULT_FATAL_ERROR;
    }
    CARDSetCommentAddress(&stat, 0);
    CARDSetIconAddress(&stat, 64);
    CARDSetBannerFormat(&stat, CARD_STAT_BANNER_C8);
    CARDSetIconFormat(&stat, 0, CARD_STAT_ICON_C8);
    CARDSetIconFormat(&stat, 1, CARD_STAT_ICON_C8);
    CARDSetIconFormat(&stat, 2, CARD_STAT_ICON_C8);
    CARDSetIconFormat(&stat, 3, CARD_STAT_ICON_C8);
    CARDSetIconSpeed(&stat, 0, CARD_STAT_SPEED_MIDDLE);
    CARDSetIconSpeed(&stat, 1, CARD_STAT_SPEED_MIDDLE);
    CARDSetIconSpeed(&stat, 2, CARD_STAT_SPEED_MIDDLE);
    CARDSetIconSpeed(&stat, 3, CARD_STAT_SPEED_MIDDLE);
    CARDSetIconSpeed(&stat, 4, CARD_STAT_SPEED_END);
    CARDSetIconAnim(&stat, CARD_STAT_ANIM_LOOP);

    result = CARDSetStatus(curSlotNo, fileNo, &stat);
    if (result == CARD_RESULT_NOCARD) {
        if (reportF != 0) {
            SLMessOut(0);
        }
        return CARD_RESULT_NOCARD;
    }
    if (result < 0) {
        if (reportF != 0) {
            SLMessOut(1);
        }
        return CARD_RESULT_FATAL_ERROR;
    }
    return result;
}

s32 SLCardMount(s16 slotNo)
{
    s32 result;

    while (1) {
        result = HuCardMount(curSlotNo);
        if (result == CARD_RESULT_WRONGDEVICE) {
            SLMessOut(7);
            return result;
        }
        if (result == CARD_RESULT_FATAL_ERROR) {
            SLMessOut(1);
            return CARD_RESULT_FATAL_ERROR;
        }
        if (result == CARD_RESULT_NOCARD) {
            SLMessOut(0);
            return CARD_RESULT_NOCARD;
        }
        if (result == CARD_RESULT_BROKEN) {
            result = HuCardSectorSizeGet(curSlotNo);
            if (result > 0 && result != 8192) {
                SLMessOut(8);
                return CARD_RESULT_WRONGDEVICE;
            }
            UnMountCnt = 0;
            result = SLMessOut(5);
            if (result == 0) {
                result = SLFormat(curSlotNo);
                if (result != 0) {
                    return result;
                }
            }
            else {
                return CARD_RESULT_FATAL_ERROR;
            }
        }
        else {
            break;
        }
    }
    result = HuCardSectorSizeGet(curSlotNo);
    if (result < 0) {
        SLMessOut(1);
        return result;
    }
    if (result != 8192) {
        SLMessOut(8);
        return CARD_RESULT_WRONGDEVICE;
    }
    return 0;
}

s32 SLFormat(s16 slotNo)
{
    float winSize[2];
    s16 result;
    s16 window1;
    s16 window2;
    OSTime time;

    HuWinInit(1);
    HU_WIN_INSERT_MES_SIZE_GET_PTR(MAKE_MESSID_PTR(SAVEWIN_MESS[curSlotNo]), 0);
    HuWinMesMaxSizeGet(1, winSize, MAKE_MESSID(16, 56));
    window1 = HuWinExCreateStyled(-10000.0f, SAVEWIN_POS, PB_F2S16(winSize[0]), PB_F2S16(winSize[1]), -1, 2);
    HuWinExAnimIn(window1);
    HU_WIN_INSERT_MES_SET_PTR(window1, MAKE_MESSID_PTR(SAVEWIN_MESS[curSlotNo]), 0);
    HuWinMesSet(window1, MAKE_MESSID(16, 56));
    HuWinMesMaxSizeGet(1, winSize, MAKE_MESSID(16, 11));
    window2 = HuWinExCreateStyled(-10000.0f, 200.0f, PB_F2S16(winSize[0]), PB_F2S16(winSize[1]), -1, 2);
    HuWinExAnimIn(window2);
    HuWinMesSet(window2, MAKE_MESSID(16, 11));
    HuWinMesWait(window2);
    HuPrcSleep(30);
    if (UnMountCnt & (1 << curSlotNo)) {
        HuWinExAnimOut(window1);
        HuWinExCleanup(window1);
        HuWinExAnimOut(window2);
        HuWinExCleanup(window2);
        SLMessOut(12);
        return 0;
    }
    SAVE_WRITE_BEGIN
    result = HuCardFormat(curSlotNo);
    SAVE_WRITE_END
    if (result < 0) {
        HuWinExAnimOut(window1);
        HuWinExCleanup(window1);
        HuWinExAnimOut(window2);
        HuWinExCleanup(window2);
    }
    if (result == CARD_RESULT_FATAL_ERROR) {
        SLMessOut(6);
        SLMessOut(1);
        return CARD_RESULT_FATAL_ERROR;
    }
    if (result == CARD_RESULT_NOCARD) {
        SLMessOut(0);
        return CARD_RESULT_NOCARD;
    }
    if (result == CARD_RESULT_WRONGDEVICE) {
        SLMessOut(7);
        return result;
    }
    HuWinExAnimOut(window1);
    HuWinExCleanup(window1);
    HuWinExAnimOut(window2);
    HuWinExCleanup(window2);
    CARDGetSerialNo(curSlotNo, &SLSerialNo);
    curBoxNo = 0;
    time = OSGetTime();
    SLSaveDataMake(0, &time);
    SLCheckSumSet();
    return result;
}

s16 SLMessOut(s16 mess)
{
    WindowData *var_r26;
    float size[2];
    uintptr_t slot_mess;
    s32 save_mess;
    s32 has_choice;
    s16 window;
    s16 choice;

    choice = -1;
    slot_mess = 0;
    has_choice = 0;
    HuWinInit(1);
    SLKillSaveWin();
    switch (mess) {
        case 0:
            save_mess = MAKE_MESSID(16, 2);
            break;

        case 1:
            save_mess = MAKE_MESSID(16, 83);
            HU_WIN_INSERT_MES_SIZE_GET_PTR(MAKE_MESSID_PTR(SlotNameTbl[curSlotNo]), 0);
            slot_mess = MAKE_MESSID_PTR(SlotNameTbl[curSlotNo]);
            break;

        case 2:
            HU_WIN_INSERT_MES_SIZE_GET_PTR(MAKE_MESSID_PTR(SlotNameTbl[curSlotNo]), 0);
            slot_mess = MAKE_MESSID_PTR(SlotNameTbl[curSlotNo]);
#ifdef TARGET_PC
            // Message numbers of the loaded disc's message file
            save_mess = VERSION_RT_PAL ? MAKE_MESSID(16, 72) : MAKE_MESSID(16, 74);
#elif VERSION_ENG
            save_mess = MAKE_MESSID(16, 74);
#elif VERSION_PAL
            save_mess = MAKE_MESSID(16, 72);
#else
            save_mess = MAKE_MESSID(16, 5);
#endif
            break;

        case 3:
#ifdef TARGET_PC
            save_mess = VERSION_RT_PAL ? MAKE_MESSID(16, 72) : MAKE_MESSID(16, 74);
#elif VERSION_ENG
            save_mess = MAKE_MESSID(16, 74);
#elif VERSION_PAL
            save_mess = MAKE_MESSID(16, 72);
#else
            save_mess = MAKE_MESSID(16, 6);
#endif
            HU_WIN_INSERT_MES_SIZE_GET_PTR(MAKE_MESSID_PTR(SlotNameTbl[curSlotNo]), 0);
            slot_mess = MAKE_MESSID_PTR(SlotNameTbl[curSlotNo]);
            break;

        case 4:
            save_mess = MAKE_MESSID(16, 74);
            HU_WIN_INSERT_MES_SIZE_GET_PTR(MAKE_MESSID_PTR(SlotNameTbl[curSlotNo]), 0);
            slot_mess = MAKE_MESSID_PTR(SlotNameTbl[curSlotNo]);
            break;

        case 5:
            HU_WIN_INSERT_MES_SIZE_GET_PTR(MAKE_MESSID_PTR(SlotNameTbl[curSlotNo]), 0);
            slot_mess = MAKE_MESSID_PTR(SlotNameTbl[curSlotNo]);
            save_mess = MAKE_MESSID(16, 4);
            has_choice = 1;
            break;

        case 6:
            save_mess = MAKE_MESSID(16, 54);
            break;
        case 7:
            HU_WIN_INSERT_MES_SIZE_GET_PTR(MAKE_MESSID_PTR(SlotNameTbl[curSlotNo]), 0);
            slot_mess = MAKE_MESSID_PTR(SlotNameTbl[curSlotNo]);
            save_mess = MAKE_MESSID(16, 55);
            break;

        case 8:
            HU_WIN_INSERT_MES_SIZE_GET_PTR(MAKE_MESSID_PTR(SlotNameTbl[curSlotNo]), 0);
            slot_mess = MAKE_MESSID_PTR(SlotNameTbl[curSlotNo]);
            save_mess = MAKE_MESSID(16, 57);
            break;

        case 9:
            HU_WIN_INSERT_MES_SIZE_GET_PTR(MAKE_MESSID_PTR(SlotNameTbl[curSlotNo]), 0);
            slot_mess = MAKE_MESSID_PTR(SlotNameTbl[curSlotNo]);
            save_mess = MAKE_MESSID(16, 69);
            break;

        case 10:
            save_mess = MAKE_MESSID(16, 70);
            has_choice = 1;
            break;

        case 11:
            HU_WIN_INSERT_MES_SIZE_GET_PTR(MAKE_MESSID_PTR(SlotNameTbl[curSlotNo]), 0);
            slot_mess = MAKE_MESSID_PTR(SlotNameTbl[curSlotNo]);
#ifdef TARGET_PC
            save_mess = VERSION_RT_ENG ? MAKE_MESSID(16, 72) : MAKE_MESSID(16, 76);
#elif VERSION_ENG
            save_mess = MAKE_MESSID(16, 72);
#else
            save_mess = MAKE_MESSID(16, 76);
#endif
            break;

        case 12:
            save_mess = MAKE_MESSID(16, 80);
            break;
    }
    if (save_mess == MAKE_MESSID(16, 4)) {
        HuWinMesMaxSizeGet(1, size, MAKE_MESSID(16, 78));
    }
    else {
        HuWinMesMaxSizeGet(1, size, save_mess);
    }
    window = HuWinExCreateStyled(-10000.0f, 200.0f, PB_F2S16(size[0]), PB_F2S16(size[1]), -1, 2);
    var_r26 = &winData[window];
    var_r26->active_pad = 1;
    if (slot_mess != 0) {
        HU_WIN_INSERT_MES_SET_PTR(window, slot_mess, 0);
    }
    HuWinAttrSet(window, 0x10);
    HuWinExAnimIn(window);
    HuWinMesSet(window, save_mess);
    HuWinMesWait(window);
    if (has_choice) {
        choice = HuWinChoiceGet(window, 1);
        if (mess == 5 && choice == 0) {
            HuWinMesSet(window, MAKE_MESSID(16, 78));
            HuWinMesWait(window);
            choice = HuWinChoiceGet(window, 1);
        }
    }
    if (mess == 11) {
        while (!(HuPadBtnDown[0] & PAD_BUTTON_A)) {
            HuPrcVSleep();
        }
    }
    HuWinExAnimOut(window);
    HuWinExCleanup(window);
    return choice;
}

#ifdef TARGET_PC
/* Online party save (docs/ONLINE_CONTINUE.md). The results screen of an online
   game makes the offline party save (SaveExec: party_continue = 1, SLSaveBoard,
   the GameStat), but in memory: the image goes to the "Online Party" folder,
   never to a memory card, and the save buffer and GameStat are put back at once,
   so nothing the game (or the online state hash) sees changes and the screen
   keeps its online timing. Built from game state only, on a 0xFF-filled buffer,
   with the two local preferences (sound mode, rumble) written as 0: every copy
   writes the same bytes. */
void SLOnlinePartySave(void)
{
    static SaveBufAll backup;
    static u8 image[SAVE_BUF_SIZE];
    GameStat statBackup;
    char where[200];

    memcpy(&backup, &saveBuf, sizeof(saveBuf));
    memcpy(&statBackup, &GWGameStat, sizeof(GameStat));
    memset(saveBuf.buf, 0xFF, SAVE_BUF_SIZE);
    GWGameStat.party_continue = 1;
    SLSaveBoard();
    memcpy(&saveBuf.data.stat, &GWGameStat, sizeof(GameStat));
    saveBuf.data.stat.sound_mode = 0;
    saveBuf.data.stat.rumble = 0;
    byteswap_gamestat(&saveBuf.data.stat);
    SLEightContinueFlagSave();
    SLCheckSumSet();
    memcpy(image, saveBuf.buf, SAVE_BUF_SIZE);
    if (GWPlayerCount() == 8) {
        BoardEightResume r;
        BoardEightResumeGet(&r);
        SLEightResumeText(&r, where, sizeof(where));
    }
    else if (GWSystem.turn >= GWSystem.max_turn) {
        snprintf(where, sizeof(where), "after the last round's minigame (round %d of %d): continuing goes on to the end of the game",
            (int)GWSystem.turn, (int)GWSystem.max_turn);
    }
    else {
        snprintf(where, sizeof(where), "after round %d's minigame: continuing starts round %d of %d", (int)GWSystem.turn,
            (int)GWSystem.turn + 1, (int)GWSystem.max_turn);
    }
    memcpy(&saveBuf, &backup, sizeof(saveBuf));
    memcpy(&GWGameStat, &statBackup, sizeof(GameStat));
    PartyBoard_OnlinePartyStore(image, SAVE_BUF_SIZE, where);
}

/* Online mode select (the file select is skipped online): a continue session
   loads the host's party save the lobby gave every copy, so Party Mode offers
   the original "continue?" and SLLoadBoard continues it, as offline. Every copy
   has the same bytes, so every copy decides the same way. Sound mode and rumble
   stay this player's own. TRUE when Party Mode will offer to continue. */
BOOL SLOnlinePartyContinueLoad(void)
{
    u8 soundMode = GWGameStat.sound_mode;
    s8 rumble = GWGameStat.rumble;
    if (!PartyBoard_OnlinePartyContinueSave(saveBuf.buf, SAVE_BUF_SIZE)) {
        return FALSE;
    }
    if (!SLCheckSumCheck()) {
        fprintf(stdout, "[netplay] continue: WARNING: the party save's checksum is wrong; a new party starts instead\n");
        fflush(stdout);
        memset(saveBuf.buf, 0, SAVE_BUF_SIZE);
        return FALSE;
    }
    SLLoadGameStat();
    GWGameStat.sound_mode = soundMode;
    GWGameStat.rumble = rumble;
    {
        const SystemState *sys = &saveBuf.data.system;
        SystemState s;
        memcpy(&s, sys, sizeof(s));
        byteswap_systemstate(&s);
        fprintf(stdout, "[netplay] continue: party save loaded at mode select: %s%s, board %d, round %d of %d, continue flag %d; %s\n",
            s.eight_players ? "8-player " : "", s.team ? "Tag Battle" : "Battle Royale", (int)s.board + 1, (int)s.turn,
            (int)s.max_turn, (int)GWGameStat.party_continue,
            GWGameStat.party_continue ? "Party Mode offers to continue it" : "WARNING: it holds no party in progress, so a new party starts");
        fflush(stdout);
    }
    slOnlineBoardPending = GWGameStat.party_continue != 0;
    return GWGameStat.party_continue != 0;
}
#endif
