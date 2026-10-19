#include "port/determinism.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "game/flag.h"
#include "game/board/main.h"
#include "game/objsub.h"
#include "ext_math.h"
#include "game/audio.h"
#include "game/chrman.h"
#include "game/data.h"
#include "game/disp.h"
#include "game/gamework_data.h"
#include "game/hu3d.h"
#include "game/minigame_seq.h"
#include "game/memory.h"
#include "game/msm.h"
#include "game/object.h"
#include "game/pad.h"
#include "game/saveload.h"
#include "game/process.h"
#include "game/sprite.h"
#include "game/thpmain.h"
#include "game/window.h"
#include "game/wipe.h"
#include "math.h"

#include "data_num/title.h"

#ifdef TARGET_PC
#include "REL/modeseldll.h"
#endif


#ifdef TARGET_PC
#include "port/byteswap.h"
#include "port/port_version.h"
#include "port/settings.h"
#include "port/netplay_runtime.h"
#endif

#define HU_PAD_BTN_ALL (HuPadBtn[0] | HuPadBtn[1] | HuPadBtn[2] | HuPadBtn[3])
#define HU_PAD_BTNDOWN_ALL (HuPadBtnDown[0] | HuPadBtnDown[1] | HuPadBtnDown[2] | HuPadBtnDown[3])
#define HU_PAD_DSTK_ALL (HuPadDStkRep[0] | HuPadDStkRep[1] | HuPadDStkRep[2] | HuPadDStkRep[3])

SHARED_SYM extern int SystemInitF;

static omObjData *debugOutView;
Process *objman;
Vec debugCamRot[2];
Vec debugCamPos[2];
float debugCamZoom[2];

Vec lbl_1_bss_10;

static s16 demoWinId;

static s16 titleMdlId[3];
static s16 bootGrpId;

s16 lbl_1_bss_4;
#if VERSION_PAL || defined(TARGET_PC)
static BOOL initLanguageF;
#endif
s32 debugCamOnF;

void BootExec(void);
static void UpdateDemoMess(void);
#if VERSION_NTSC
void BootProgExec(void);
#endif
void BootTitleCreate(void);
BOOL BootTitleExec(void);

void *NintendoDataDecode(void);

#if defined(TARGET_PC) && defined(PARTYBOARD_HD_BETA)
/* Test aid: PARTYBOARD_TEST_ANIMREAD=1 converts a few sprite animation files
   three times each from one buffer and checks the three results are identical
   and the file is left unchanged (PARTYBOARD_TEST_LOADERS checks every file).
   Before HuSprAnimRead stopped converting in place, the second conversion of a
   buffer read garbage sizes (the w01 item-wheel crash on Windows). Logs
   "[animread] ... PASS/FAIL" lines. */
static BOOL AnimReadSame(ANIMDATA *a, ANIMDATA *b, const char **why)
{
    s32 i;
    if (a->bankNum != b->bankNum || a->patNum != b->patNum || a->bmpNum != b->bmpNum) {
        *why = "counts differ";
        return FALSE;
    }
    for (i = 0; i < a->bankNum; i++) {
        if (a->bank[i].timeNum != b->bank[i].timeNum
            || memcmp(a->bank[i].frame, b->bank[i].frame, a->bank[i].timeNum * sizeof(ANIMFRAME)) != 0) {
            *why = "bank frames differ";
            return FALSE;
        }
    }
    for (i = 0; i < a->patNum; i++) {
        if (a->pat[i].layerNum != b->pat[i].layerNum || a->pat[i].sizeX != b->pat[i].sizeX
            || a->pat[i].sizeY != b->pat[i].sizeY
            || memcmp(a->pat[i].layer, b->pat[i].layer, a->pat[i].layerNum * sizeof(ANIMLAYER)) != 0) {
            *why = "pattern layers differ";
            return FALSE;
        }
    }
    for (i = 0; i < a->bmpNum; i++) {
        if (a->bmp[i].dataFmt != b->bmp[i].dataFmt || a->bmp[i].sizeX != b->bmp[i].sizeX
            || a->bmp[i].sizeY != b->bmp[i].sizeY || a->bmp[i].data != b->bmp[i].data) {
            *why = "bitmaps differ";
            return FALSE;
        }
    }
    return TRUE;
}

static void AnimReadSelfTest(void)
{
    static const struct {
        s32 num;
        const char *name;
    } files[] = {
        { DATA_MAKE_NUM(DATADIR_W01, 28), "w01 item wheel (RoundItemInit)" },
        { DATA_MAKE_NUM(DATADIR_BOARD, 0x29), "board rank picture" },
        { DATA_MAKE_NUM(DATADIR_BOARD, 0x27), "board status card" },
    };
    s32 f, k, pass = 0;
    for (f = 0; f < (s32)(sizeof(files) / sizeof(files[0])); f++) {
        ANIMDATA *anim[3];
        const char *why = "";
        BOOL same = TRUE;
        void *raw = HuDataSelHeapReadNum(files[f].num, MEMORY_DEFAULT_NUM, HEAP_DATA);
        s32 size = HuDataGetSize(files[f].num);
        u8 *copy;
        if (raw == NULL || size < 8) {
            OSReport("[animread] %s: SKIP (could not read the file)\n", files[f].name);
            continue;
        }
        copy = HuMemDirectMalloc(HEAP_DATA, size);
        memcpy(copy, raw, size);
        for (k = 0; k < 3; k++) {
            anim[k] = HuSprAnimRead(raw);
        }
        for (k = 1; k < 3 && same; k++) {
            same = AnimReadSame(anim[0], anim[k], &why);
        }
        /* The file must be unchanged except for the converted mark that
           HuSprAnimRead writes into its unused use-count field (bytes 6-7). */
        if (same && (memcmp(raw, copy, 6) != 0 || memcmp((u8 *)raw + 8, copy + 8, size - 8) != 0)) {
            same = FALSE;
            why = "file data changed";
        }
        HuMemDirectFree(copy);
        OSReport("[animread] %s: %s (banks %d, patterns %d, bitmaps %d)%s%s\n", files[f].name,
            same ? "PASS" : "FAIL", anim[0]->bankNum, anim[0]->patNum, anim[0]->bmpNum, same ? "" : " - ", why);
        pass += same;
        for (k = 0; k < 3; k++) {
            HuSprAnimKill(anim[k]);
        }
        HuMemDirectFree(raw);
    }
    OSReport("[animread] done: %d of %d files PASS\n", (int)pass, (int)(sizeof(files) / sizeof(files[0])));
}
#endif

void ObjectSetup(void)
{
    omOvlHisData *history;
    OSReport("******* Boot ObjectSetup *********\n");
    objman = omInitObjMan(50, 8192);
    debugCamRot[0].x = -67;
    debugCamRot[0].y = 40;
    debugCamRot[0].z = 0;
    debugCamPos[0].x = 260;
    debugCamPos[0].y = -103;
    debugCamPos[0].z = -18;
    debugCamZoom[0] = 2885;
    Hu3DCameraCreate(1);
    Hu3DCameraPerspectiveSet(1, 30, 20, 15000, 1.2);
    Hu3DCameraViewportSet(1, 0, 0, HU_FB_WIDTH, HU_FB_HEIGHT, 0, 1);
    HuPrcCreate(BootExec, 100, 12288, 0);
    Hu3DBGColorSet(0, 0, 0);
    history = omOvlHisGet(0);
    omOvlHisChg(0, history->overlay, 1, history->stat);
    #if VERSION_PAL || defined(TARGET_PC)
    if(SystemInitF == FALSE) {
        initLanguageF = FALSE;
    } else {
        initLanguageF = TRUE;
    }
    #endif
}

 #if VERSION_PAL || defined(TARGET_PC)
 s32 LanguageBootGet(void);
 BOOL LanguageMenuExec(void);
 #endif

 /*
  * Online, a boot wait must cost both peers the same number of simulation
  * ticks. Real elapsed time differs between two machines, and every iteration
  * here yields one lockstep tick, so a wall-clock bound makes the very first
  * screen diverge. 180 ticks is the same three seconds the sibling branches
  * below already count out by hand. Offline keeps the original behaviour.
  */
 static void BootWaitMs(OSTick start, u32 milliseconds)
 {
#ifdef TARGET_PC
     if (PartyBoard_NetplayEnabled()) {
         u32 ticks = milliseconds * 60 / 1000;
         while (ticks-- != 0) {
             HuPrcVSleep();
         }
         return;
     }
#endif
     while (OSTicksToMilliseconds(OSGetTick() - start) < milliseconds) {
         HuPrcVSleep();
     }
 }

#ifdef TARGET_PC
 static void BootInitForSkippedSequence(void)
 {
     if (!SystemInitF) {
         CharInit();
         HuWindowInit();
         MGSeqInit();
         HuWinInit(1);
         /*
          * The boot sequence loads the sample banks in two steps, and only
          * the second one was reproduced here. HuAudSndGrpSet(0) is the only
          * path that reaches msmSysLoadBaseGroup(): a group set deliberately
          * skips every base group (msmSysCheckBaseGroupNo, called from
          * msmSysLoadGroupSet), so without this call the common sound effect
          * banks are never resident and every SE stays silent. Base groups
          * first, exactly as the sequence below does before its own
          * HuAudSndGrpSetSet(0).
          */
         HuAudSndGrpSet(0);
         HuAudSndGrpSetSet(0);
         SystemInitF = TRUE;
     }
     initLanguageF = TRUE;
 }
#endif

 void BootExec(void)
 {
     ANIMDATA *data;
     s16 group;
     s16 sprite_nintendo;
     s16 sprite_hudson;
     s16 i;
     OSTick tick_prev;
     Process *curr = HuPrcCurrentGet();
#ifdef TARGET_PC
     if (partyboard_version_is_pal()) {
         if(SystemInitF == FALSE) {
             GwLanguage = LanguageBootGet();
             if(GwLanguage == -1) {
                 if(LanguageMenuExec()) {
                     SystemInitF = TRUE;
                 }
             } else {
                 GWLanguageSet(GwLanguage);
             }
         }
     }
#else

#if VERSION_NTSC
     if (omovlevtno == 0) {
         BootProgExec();
     }
#else
     if(SystemInitF == FALSE) {
         GwLanguage = LanguageBootGet();
         if(GwLanguage == -1) {
             if(LanguageMenuExec()) {
                 SystemInitF = TRUE;
             }
         } else {
             GWLanguageSet(GwLanguage);
         }
     }
#endif
#endif
#if defined(TARGET_PC) && defined(PARTYBOARD_HD_BETA)
     /* Test aid: PARTYBOARD_TEST_MINIGAME=<index into mgInfoTbl> skips the menus
        and opens that minigame's rules screen with Mario, Luigi, Peach and
        Yoshi (computer players; player 1 human with
        PARTYBOARD_TEST_MINIGAME_HUMAN), teams set for 1-vs-3 and 2-vs-2. Used
        to check every minigame's on-screen button prompts. */
     if (omovlevtno == 0 && getenv("PARTYBOARD_TEST_MINIGAME") != NULL) {
         const int mg = atoi(getenv("PARTYBOARD_TEST_MINIGAME"));
         const int type = mgInfoTbl[mg].type;
         for (i = 0; i < 4; i++) {
             GWPlayerCfg[i].character = i;
             GWPlayerCfg[i].iscom = (getenv("PARTYBOARD_TEST_MINIGAME_HUMAN") != NULL && i == 0) ? 0 : 1;
             GWPlayerCfg[i].pad_idx = i;
             GWPlayerCfg[i].diff = 0;
             GWPlayerCfg[i].group = type == 1 ? (i == 0 ? 0 : 1) : type == 2 ? i / 2 : i;
         }
         BootInitForSkippedSequence();
         GWSystem.mg_next = mg;
         _SetFlag(FLAG_ID_MAKE(0, 11));
         omOvlCallEx(DLL_instdll, 1, 0, 0);
         HuPrcEnd();
         do {
             HuPrcVSleep();
         } while (1);
     }
#endif
#if defined(TARGET_PC) && defined(PARTYBOARD_HD_BETA)
     if (omovlevtno == 0 && getenv("PARTYBOARD_TEST_ANIMREAD") != NULL) {
         AnimReadSelfTest();
     }
     /* Test aid: PARTYBOARD_TEST_BOARD=<1-6> starts a Party Mode board game with
        computer players only, launched the way Party Mode does it (mentDll).
        PARTYBOARD_TEST_EIGHT=1 makes it the 8-player party; PARTYBOARD_TEST_TEAMS=1
        pairs players (0+1, 2+3, ...); PARTYBOARD_TEST_TURNS sets the turn count
        (default 1). PARTYBOARD_TEST_HUMANS=<seats> (e.g. "5,6,7,8", seats 1-8)
        makes those seats human players; seat N plays on controller port N, so a
        test driver can play them with "S <N-1> ..." lines on the test input
        socket. Used to test the 8-player work without menus. */
     if (omovlevtno == 0 && getenv("PARTYBOARD_TEST_BOARD") != NULL) {
         static const s32 boardOvl[6] = { DLL_w01dll, DLL_w02dll, DLL_w03dll, DLL_w04dll, DLL_w05dll, DLL_w06dll };
         const s32 board = atoi(getenv("PARTYBOARD_TEST_BOARD")) - 1;
         const BOOL eight = getenv("PARTYBOARD_TEST_EIGHT") != NULL;
         const BOOL teams = getenv("PARTYBOARD_TEST_TEAMS") != NULL;
         const s32 turns = getenv("PARTYBOARD_TEST_TURNS") != NULL ? atoi(getenv("PARTYBOARD_TEST_TURNS")) : 1;
         const char *humans = getenv("PARTYBOARD_TEST_HUMANS");
         const s32 count = eight ? 8 : 4;
         u32 humanSeats = 0;
         if (humans != NULL) {
             const char *c;
             for (c = humans; *c != '\0'; c++) {
                 if (*c >= '1' && *c <= '0' + count) {
                     humanSeats |= 1u << (*c - '1');
                 }
             }
         }
         for (i = 0; i < count; i++) {
             GWPlayerCfg[i].character = i;
             GWPlayerCfg[i].iscom = (humanSeats >> i) & 1 ? 0 : 1;
             /* Every seat its own port (8-player party: ports 0-7). */
             GWPlayerCfg[i].pad_idx = i;
             /* PARTYBOARD_TEST_DIFF=<0-3>: computer difficulty (default 0, easy). */
             GWPlayerCfg[i].diff = getenv("PARTYBOARD_TEST_DIFF") != NULL ? (atoi(getenv("PARTYBOARD_TEST_DIFF")) & 3) : 0;
             GWPlayerCfg[i].group = teams ? i / 2 : 0;
         }
         if (humanSeats != 0) {
             OSReport("[test] board test: human seats 0x%02x (seat N on controller port N)\n", (unsigned)humanSeats);
         }
         BootInitForSkippedSequence();
         BoardSaveInit(board);
         /* PARTYBOARD_TEST_BONUS=1: bonus stars on (as the Party Mode rules screen). */
         BoardPartyConfigSet(teams, getenv("PARTYBOARD_TEST_BONUS") != NULL && atoi(getenv("PARTYBOARD_TEST_BONUS")) != 0, 0, turns, 0, 0, 0, 0);
         GWSystem.eight_players = eight;
         /* PARTYBOARD_TEST_SHOWCOM=1: computer players play the minigames that
            are normally skipped for them (fortune, Bowser's Gnarly Party). */
         if (getenv("PARTYBOARD_TEST_SHOWCOM") != NULL) {
             GWSystem.show_com_mg = 1;
         }
         for (i = 0; i < count; i++) {
             CharMotionInit(GWPlayerCfg[i].character);
         }
         /* PARTYBOARD_TEST_ENDING=1: no board; the end-of-game ceremony and
            results (mstory3Dll) right away, with the stars and coins of
            PARTYBOARD_TEST_END_STATS ("stars/coins/minigame coins/most coins/
            ? spaces" per seat, comma separated) or a built-in set with ties. */
         if (getenv("PARTYBOARD_TEST_ENDING") != NULL) {
             static const s16 defStats[8][5] = {
                 { 3, 10, 50, 60, 5 }, { 3, 40, 50, 45, 2 }, { 2, 70, 20, 80, 5 }, { 4, 10, 30, 30, 5 },
                 { 4, 10, 50, 20, 1 }, { 1, 90, 10, 95, 5 }, { 2, 25, 50, 40, 0 }, { 0, 5, 15, 10, 5 }
             };
             const char *spec = getenv("PARTYBOARD_TEST_END_STATS");
             for (i = 0; i < count; i++) {
                 s32 v[5] = { defStats[i][0], defStats[i][1], defStats[i][2], defStats[i][3], defStats[i][4] };
                 if (spec != NULL && *spec != '\0') {
                     sscanf(spec, "%d/%d/%d/%d/%d", &v[0], &v[1], &v[2], &v[3], &v[4]);
                     spec = strchr(spec, ',');
                     spec = spec != NULL ? spec + 1 : "";
                 }
                 GWPlayer[i].character = GWPlayerCfg[i].character;
                 GWPlayer[i].com = GWPlayerCfg[i].iscom;
                 GWPlayer[i].port = GWPlayerCfg[i].pad_idx;
                 GWPlayer[i].handicap = 0;
                 GWPlayerTeamSet(i, GWPlayerCfg[i].group);
                 GWPlayerStateIdxSet(&GWPlayer[i], i);
                 GWPlayer[i].stars = GWPlayer[i].stars_max = v[0];
                 GWPlayer[i].coins = v[1];
                 GWPlayer[i].coins_mg = v[2];
                 GWPlayer[i].coins_max = v[3];
                 GWPlayer[i].question_count = v[4];
                 if (eight) {
                     GWPlayerTurnOrderSet(&GWPlayer[i], i);
                 }
             }
             GWSystem.turn = GWSystem.max_turn;
             if (getenv("PARTYBOARD_TEST_BONUS") == NULL) {
                 GWSystem.bonus_star = 1;
             }
             OSReport("[test] ending: %d players, board %d, bonus stars %s\n", (int)count, (int)board + 1, GWSystem.bonus_star ? "on" : "off");
             omOvlCallEx(DLL_mstory3dll, 1, 0, 0);
             HuPrcEnd();
             do {
                 HuPrcVSleep();
             } while (1);
         }
         /* The board returns to the end-of-game ceremony and results, as from
            Party Mode (mstory3Dll event 2 starts the board on top of itself);
            PARTYBOARD_TEST_RESULTS=0 returns straight here instead. */
         if (getenv("PARTYBOARD_TEST_RESULTS") == NULL || atoi(getenv("PARTYBOARD_TEST_RESULTS")) != 0) {
             omOvlCallEx(DLL_mstory3dll, 1, 2, board);
         } else {
             omOvlCallEx(boardOvl[board], 1, 0, 0);
         }
         HuPrcEnd();
         do {
             HuPrcVSleep();
         } while (1);
     }
     /* Test aid: PARTYBOARD_TEST_CONTINUE=<file 1-3> continues the Party Mode
        game saved in that file of memory card slot A, the way file select and
        Party Mode's "continue" do it (SLLoad, SLLoadGameStat, SLLoadBoard),
        without the menus. Used to test saving and continuing 8-player games. */
     if (omovlevtno == 0 && getenv("PARTYBOARD_TEST_CONTINUE") != NULL) {
         static const s32 boardOvl[6] = { DLL_w01dll, DLL_w02dll, DLL_w03dll, DLL_w04dll, DLL_w05dll, DLL_w06dll };
         s32 box = atoi(getenv("PARTYBOARD_TEST_CONTINUE")) - 1;
         if (box < 0 || box > 2) {
             box = 0;
         }
         BootInitForSkippedSequence();
         SLCurSlotNoSet(0);
         SLCurBoxNoSet(box);
         SLSaveFlagSet(1);
         SLLoad();
         if (SLCheckSumCheck()) {
             SLLoadGameStat();
             if (GWGameStat.party_continue) {
                 SLLoadBoard();
                 if (GWSystem.board < 6) {
                     OSReport("[test] continue: file %d, board %d, round %d of %d, %d players\n", (int)box + 1,
                         (int)GWSystem.board + 1, (int)GWSystem.turn, (int)GWSystem.max_turn, (int)GWPlayerCount());
                     saveExecF = 1;
                     for (i = 0; i < GWPlayerCount(); i++) {
                         CharMotionInit(GWPlayerCfg[i].character);
                     }
                     omOvlCallEx(boardOvl[GWSystem.board], 1, 0, 0);
                     HuPrcEnd();
                     do {
                         HuPrcVSleep();
                     } while (1);
                 }
             }
         }
         OSReport("[test] continue: WARNING: file %d has no Party Mode game to continue; normal start\n", (int)box + 1);
     }
#endif
#ifdef TARGET_PC
     if (omovlevtno == 0 && partyboard_settings_skipBootSequence()) {
         for (i = 0; i < 4; i++) {
             GWPlayerCfg[i].pad_idx = i;
         }
         BootInitForSkippedSequence();
         omOvlCallEx(DLL_modeseldll, 1, MODESEL_EVENT_SKIP_BOOT, 0);
         for (i = 0; i < 4; i++) {
             GWPlayerCfg[i].character = -1;
         }
         HuPrcEnd();
         do {
             HuPrcVSleep();
         } while (1);
     }
#endif
     group = HuSprGrpCreate(2);
#if defined(__MWERKS__) || defined(BYTESWAPPING)
     data = HuSprAnimRead(NintendoDataDecode());
     sprite_nintendo = HuSprCreate(data, 0, 0);
     HuSprGrpMemberSet(group, 0, sprite_nintendo);
#endif
     HuSprPosSet(group, 0, 288, 240);
     HuSprAttrSet(group, 0, HUSPR_ATTR_DISPOFF);
     if (omovlevtno != 0) {
         HuAudSndGrpSetSet(0);
         data = HuSprAnimReadFile(TITLE_ANM_VER_ADJUSTED(TITLE_HUDSON_ANM));
         sprite_hudson = HuSprCreate(data, 0, 0);
         HuSprGrpMemberSet(group, 1, sprite_hudson);
         HuSprPosSet(group, 1, 288, 240);
         HuSprAttrSet(group, 1, HUSPR_ATTR_DISPOFF);
         HuWinInit(1);
         BootTitleCreate();
         SystemInitF = 1;
     }
     else {
         s16 group_thp;
         BOOL skip_wait;
         float win_size[2];
         s16 sprite_thp;
         for (i = 0; i < 4; i++) {
             GWPlayerCfg[i].pad_idx = i;
         }
#ifdef TARGET_PC
         // After the PAL language menu SystemInitF is already set: the logo and title still need creating
         if (partyboard_version_is_pal() && SystemInitF) {
             data = HuSprAnimReadFile(TITLE_ANM_VER_ADJUSTED(TITLE_HUDSON_ANM));
             sprite_hudson = HuSprCreate(data, 0, 0);
             HuSprGrpMemberSet(group, 1, sprite_hudson);
             HuSprPosSet(group, 1, 288, 240);
             HuSprAttrSet(group, 1, HUSPR_ATTR_DISPOFF);
             HuAudSndGrpSetSet(0);
             BootTitleCreate();
             HuWinInit(1);
         }
#else
         #if VERSION_PAL
         if(SystemInitF) {
             data = HuSprAnimReadFile(TITLE_ANM_VER_ADJUSTED(TITLE_HUDSON_ANM));
             sprite_hudson = HuSprCreate(data, 0, 0);
             HuSprGrpMemberSet(group, 1, sprite_hudson);
             HuSprPosSet(group, 1, 288, 240);
             HuSprAttrSet(group, 1, HUSPR_ATTR_DISPOFF);
             HuAudSndGrpSetSet(0);
             BootTitleCreate();
             HuWinInit(1);
         }
         #endif
#endif
     repeat:
         HuSprAttrReset(group, 0, HUSPR_ATTR_DISPOFF);
         WipeCreate(WIPE_MODE_IN, WIPE_TYPE_NORMAL, 30);
         while (WipeStatGet()) {
             HuPrcVSleep();
         }
         WipeColorSet(255, 255, 255);
         if (!SystemInitF) {
             tick_prev = OSGetTick();
             CharInit();
             HuWindowInit();
             MGSeqInit();
             HuWinInit(1);
             BootTitleCreate();
             data = HuSprAnimReadFile(TITLE_ANM_VER_ADJUSTED(TITLE_HUDSON_ANM));
             sprite_hudson = HuSprCreate(data, 0, 0);
             HuSprGrpMemberSet(group, 1, sprite_hudson);
             HuSprPosSet(group, 1, 288, 240);
             HuSprAttrSet(group, 1, HUSPR_ATTR_DISPOFF);
             BootWaitMs(tick_prev, 3000);
         }
         else {
             for (i = 0; i < 180; i++) {
#ifdef TARGET_PC
                 if (partyboard_version_is_ntsc()) {
                     if (HuPadBtnDown[0] & (PAD_BUTTON_START | PAD_BUTTON_A)) {
                         break;
                     }
                 } else {
                     if (initLanguageF && (HuPadBtnDown[0] & (PAD_BUTTON_START | PAD_BUTTON_A))) {
                         break;
                     }
                 }
#else
                 #if VERSION_NTSC
                 if (HuPadBtnDown[0] & (PAD_BUTTON_START | PAD_BUTTON_A)) {
                     break;
                 }
                 #else
                 if (initLanguageF && (HuPadBtnDown[0] & (PAD_BUTTON_START | PAD_BUTTON_A))) {
                     break;
                 }
                 #endif
#endif
                 HuPrcVSleep();
             }
         }
         WipeCreate(WIPE_MODE_OUT, WIPE_TYPE_NORMAL, 30);
         while (WipeStatGet()) {
             HuPrcVSleep();
         }
         HuSprAttrSet(group, 0, HUSPR_ATTR_DISPOFF);
         HuSprAttrReset(group, 1, HUSPR_ATTR_DISPOFF);
         HuPrcSleep(60);
         WipeCreate(WIPE_MODE_IN, WIPE_TYPE_NORMAL, 30);
         while (WipeStatGet()) {
             HuPrcVSleep();
         }
         if (!SystemInitF) {
             tick_prev = OSGetTick();
             HuAudSndGrpSet(0);
             BootWaitMs(tick_prev, 3000);
         }
         else {
             for (i = 0; i < 180; i++) {
#ifdef TARGET_PC
                 if (partyboard_version_is_ntsc()) {
                     if (HuPadBtnDown[0] & (PAD_BUTTON_START | PAD_BUTTON_A)) {
                         break;
                     }
                 } else {
                     if (initLanguageF && (HuPadBtnDown[0] & (PAD_BUTTON_START | PAD_BUTTON_A))) {
                         break;
                     }
                 }
#else
                 #if VERSION_NTSC
                 if (HuPadBtnDown[0] & (PAD_BUTTON_START | PAD_BUTTON_A)) {
                     break;
                 }
                 #else
                 if (initLanguageF && (HuPadBtnDown[0] & (PAD_BUTTON_START | PAD_BUTTON_A))) {
                     break;
                 } 
                 #endif
#endif
                 HuPrcVSleep();
             }
         }
         #if VERSION_PAL || defined(TARGET_PC)
         initLanguageF = TRUE;
         #endif
         WipeCreate(WIPE_MODE_OUT, WIPE_TYPE_NORMAL, 30);
         while (WipeStatGet()) {
             HuPrcVSleep();
         }
         tick_prev = OSGetTick();
         if (!SystemInitF) {
             HuAudSndGrpSetSet(0);
             SystemInitF = 1;
         }
         BootWaitMs(tick_prev, 1000);
         HuSprAttrSet(group, 0, HUSPR_ATTR_DISPOFF);
         HuSprAttrSet(group, 1, HUSPR_ATTR_DISPOFF);
         group_thp = HuSprGrpCreate(1);
#ifdef __MWERKS__
         // TODO PC
         sprite_thp = HuTHPSprCreateVol("movie/opmov_a00.thp", 0, 100, 122);
         HuSprGrpMemberSet(group_thp, 0, sprite_thp);
         HuSprPosSet(group_thp, 0, 288, 240);
         HuWinMesMaxSizeBetGet(win_size, MAKE_MESSID(54, 0), MAKE_MESSID(54, 4));
         demoWinId = HuWinCreate(-10000, 448 - win_size[1], win_size[0], win_size[1], 0);
         HuWinMesSpeedSet(demoWinId, 0);
         HuWinBGTPLvlSet(demoWinId, 0);
         HuWinPriSet(demoWinId, 10);
         HuWinAttrSet(demoWinId, 0x800);
#endif
         HuPrcSleep(5);
         WipeCreate(WIPE_MODE_IN, WIPE_TYPE_NORMAL, 10);
         while (WipeStatGet()) {
             HuPrcVSleep();
         }
         skip_wait = FALSE;
         while (!HuTHPEndCheck()) {
             UpdateDemoMess();
             if (HuPadBtnDown[0] & (PAD_BUTTON_START | PAD_BUTTON_A)) {
                 skip_wait = TRUE;
                 break;
             }
             HuPrcVSleep();
         }
         WipeCreate(WIPE_MODE_OUT, WIPE_TYPE_NORMAL, 30);
         while (WipeStatGet()) {
             HuPrcVSleep();
         }
         HuWinKill(demoWinId);
         HuTHPClose();
         HuPrcVSleep();
         HuSprGrpKill(group_thp);
         if (!skip_wait) {
             HuPrcSleep(60);
         }
         skip_wait = FALSE;
     }
     if (!BootTitleExec()) {
         HuPrcSleep(60);
         goto repeat;
     }
     WipeColorSet(255, 255, 255);
     HuAudSStreamAllFadeOut(500);
     WipeCreate(WIPE_MODE_OUT, WIPE_TYPE_NORMAL, 30);
     while (WipeStatGet()) {
         HuPrcVSleep();
     }
     HuPrcVSleep();
     if (omSysExitReq) {
         omOvlReturnEx(1, 1);
     }
     else {
         omOvlCallEx(DLL_modeseldll, 1, 0, 0);
     }
     for (i = 0; i < 4; i++) {
         GWPlayerCfg[i].character = -1;
     }
     HuPrcEnd();
     do {
         HuPrcVSleep();
     } while (1);
 }

 static s16 demoMessTimeTbl[] = {
     1686,
     1785,
     1850,
     1936,
     1993,
     2097,
     2135,
     2245,
     2258,
     2320,
     -1,
     -1,
 };

 static void UpdateDemoMess(void)
 {
     int frame = HuTHPFrameGet();
     int i;
     for (i = 0; demoMessTimeTbl[i * 2] != -1; i++) {
         if (frame == demoMessTimeTbl[i * 2]) {
             HuWinMesSet(demoWinId, MAKE_MESSID(54, i));
         }
         if (frame == demoMessTimeTbl[(i * 2) + 1]) {
             HuWinHomeClear(demoWinId);
         }
     }
 }

 #if VERSION_NTSC

 static u16 progPosTbl[] = { 236, 313, 353, 313 };

 void BootProgExec(void)
 {
     s16 i;
     s16 option;
     s16 group;
     s16 sprite;
     ANIMDATA *data;
#ifdef TARGET_PC
     return;
#else
     option = 0;
     if (OSGetResetCode() != OS_RESET_RESTART) {
         return;
     }
     if (VIGetDTVStatus() != VI_NON_INTERLACE) {
         OSSetProgressiveMode(OS_PROGRESSIVE_MODE_OFF);
         return;
     }
     if (OSGetProgressiveMode() == OS_PROGRESSIVE_MODE_OFF && !(HU_PAD_BTN_ALL & PAD_BUTTON_B)) {
         return;
     }
     if (OSGetProgressiveMode() == OS_PROGRESSIVE_MODE_ON) {
         VIConfigure(&GXNtsc480IntDf);
         VIFlush();
     }
#endif
     group = HuSprGrpCreate(3);
     data = HuSprAnimReadFile(TITLE_PROGRESSIVE_CHOOSE_ANM);
     sprite = HuSprCreate(data, 0, 0);
     HuSprGrpMemberSet(group, 0, sprite);
     HuSprPosSet(group, 0, 288, 240);
     data = HuSprAnimReadFile(TITLE_PROGRESSIVE_CURSOR_ON_ANM);
     sprite = HuSprCreate(data, 0, 0);
     HuSprGrpMemberSet(group, 1, sprite);
     HuSprPosSet(group, 1, progPosTbl[option * 2], progPosTbl[(option * 2) + 1]);
     data = HuSprAnimReadFile(TITLE_PROGRESSIVE_CURSOR_OFF_ANM);
     sprite = HuSprCreate(data, 0, 0);
     HuSprGrpMemberSet(group, 2, sprite);
     HuSprPosSet(group, 2, progPosTbl[option * 2], progPosTbl[(option * 2) + 1]);
     HuSprAttrSet(group, 2, HUSPR_ATTR_DISPOFF);
     WipeCreate(WIPE_MODE_IN, WIPE_TYPE_NORMAL, 30);
     while (WipeStatGet()) {
         HuPrcVSleep();
     }
     for (i = 0; i < 600; i++) {
         if (HU_PAD_DSTK_ALL & (PAD_BUTTON_LEFT | PAD_BUTTON_RIGHT)) {
             option ^= 1;
             HuSprPosSet(group, 1, progPosTbl[option * 2], progPosTbl[(option * 2) + 1]);
             i = 0;
         }
         if (HU_PAD_BTNDOWN_ALL & PAD_BUTTON_A) {
             HuSprAttrSet(group, 1, HUSPR_ATTR_DISPOFF);
             HuSprPosSet(group, 2, progPosTbl[option * 2], progPosTbl[(option * 2) + 1]);
             HuSprAttrReset(group, 2, HUSPR_ATTR_DISPOFF);
             break;
         }
         HuPrcVSleep();
     }
     WipeCreate(WIPE_MODE_OUT, WIPE_TYPE_NORMAL, 20);
     while (WipeStatGet()) {
         HuPrcVSleep();
     }
#ifdef __MWERKS__
     if (!option) {
         OSSetProgressiveMode(OS_PROGRESSIVE_MODE_ON);
         VIConfigure(&GXNtsc480Prog);
         VIFlush();
         HuPrcSleep(100);
     }
     else {
         OSSetProgressiveMode(OS_PROGRESSIVE_MODE_OFF);
     }
#endif
     HuSprGrpKill(group);
     group = HuSprGrpCreate(1);
     if (!option) {
         data = HuSprAnimReadFile(TITLE_PROGRESSIVE_ON_ANM);
     }
     else {
         data = HuSprAnimReadFile(TITLE_PROGRESSIVE_OFF_ANM);
     }
     sprite = HuSprCreate(data, 0, 0);
     HuSprGrpMemberSet(group, 0, sprite);
     HuSprPosSet(group, 0, 288, 240);
     WipeCreate(WIPE_MODE_IN, WIPE_TYPE_NORMAL, 20);
     while (WipeStatGet()) {
         HuPrcVSleep();
     }
     HuPrcSleep(60);
     for (i = 0; i < 180; i++) {
         if (HU_PAD_BTNDOWN_ALL & PAD_BUTTON_A) {
             break;
         }
         HuPrcVSleep();
     }
     WipeCreate(WIPE_MODE_OUT, WIPE_TYPE_NORMAL, 30);
     while (WipeStatGet()) {
         HuPrcVSleep();
     }
     HuSprGrpKill(group);
     HuPrcSleep(30);
 }

 #endif

 static u16 debugCamTbl[] = { 1, 2 };

 void DebugCamOutView(omObjData *obj)
 {
     s16 i;
     for (i = 0; i < 1; i++) {
         Vec pos, target, up;
         float x, y, z;

         x = debugCamRot[i].x;
         y = debugCamRot[i].y;
         z = debugCamRot[i].z;

         pos.x = (((sind(y) * cosd(x)) * debugCamZoom[i]) + debugCamPos[i].x);
         pos.y = (-sind(x) * debugCamZoom[i]) + debugCamPos[i].y;
         pos.z = ((cosd(y) * cosd(x)) * debugCamZoom[i]) + debugCamPos[i].z;
         target.x = debugCamPos[i].x;
         target.y = debugCamPos[i].y;
         target.z = debugCamPos[i].z;
         up.x = sind(y) * sind(x);
         up.y = cosd(x);
         up.z = cosd(y) * sind(x);
         Hu3DCameraPosSet(debugCamTbl[i], pos.x, pos.y, pos.z, up.x, up.y, up.z, target.x, target.y, target.z);
     }
 }

 void DebugCamUpdate(omObjData *obj)
 {
     Vec pos;
     Vec offset;
     Vec dir;
     Vec y_offset;

     f32 z_rot;
     s8 stick_pos;
    
     #if VERSION_NTSC
     if ((HuPadBtnDown[0] & 0x800)) {
         debugCamOnF = (debugCamOnF) ? 0 : 1;
     }
     #endif
     if (debugCamOnF != 0) {
         debugCamRot[0].y += 0.1f * HuPadStkX[0];
         debugCamRot[0].x += 0.1f * HuPadStkY[0];
         debugCamZoom[0] += HuPadTrigL[0] / 2;
         debugCamZoom[0] -= HuPadTrigR[0] / 2;
         if (debugCamZoom[0] < 100.0f) {
             debugCamZoom[0] = 100.0f;
         }
         pos.x = debugCamPos[0].x + (debugCamZoom[0] * (sind(debugCamRot[0].y) * cosd(debugCamRot[0].x)));
         pos.y = (debugCamPos[0].y + (debugCamZoom[0] * -sind(debugCamRot[0].x)));
         pos.z = (debugCamPos[0].z + (debugCamZoom[0] * (cosd(debugCamRot[0].y) * cosd(debugCamRot[0].x))));
         offset.x = debugCamPos[0].x - pos.x;
         offset.y = debugCamPos[0].y - pos.y;
         offset.z = debugCamPos[0].z - pos.z;
         dir.x = (sind(debugCamRot[0].y) * sind(debugCamRot[0].x));
         dir.y = cosd(debugCamRot[0].x);
         dir.z = (cosd(debugCamRot[0].y) * sind(debugCamRot[0].x));
         z_rot = debugCamRot[0].z;
         y_offset.x = dir.x * (offset.x * offset.x + (1.0f - offset.x * offset.x) * cosd(z_rot))
             + dir.y * (offset.x * offset.y * (1.0f - cosd(z_rot)) - offset.z * sind(z_rot))
             + dir.z * (offset.x * offset.z * (1.0f - cosd(z_rot)) + offset.y * sind(z_rot));

         y_offset.y = dir.y * (offset.y * offset.y + (1.0f - offset.y * offset.y) * cosd(z_rot))
             + dir.x * (offset.x * offset.y * (1.0f - cosd(z_rot)) + offset.z * sind(z_rot))
             + dir.z * (offset.y * offset.z * (1.0f - cosd(z_rot)) - offset.x * sind(z_rot));

         y_offset.z = dir.z * (offset.z * offset.z + (1.0f - offset.z * offset.z) * cosd(z_rot))
             + (dir.x * (offset.x * offset.z * (1.0 - cosd(z_rot)) - offset.y * sind(z_rot))
                 + dir.y * (offset.y * offset.z * (1.0 - cosd(z_rot)) + offset.x * sind(z_rot)));

         VECCrossProduct(&dir, &offset, &offset);
         VECNormalize(&offset, &offset);
         stick_pos = (HuPadSubStkX[0] & 0xF8);
         if (stick_pos != 0) {
             debugCamPos[0].x += 0.05f * (offset.x * stick_pos);
             debugCamPos[0].y += 0.05f * (offset.y * stick_pos);
             debugCamPos[0].z += 0.05f * (offset.z * stick_pos);
         }
         VECNormalize(&y_offset, &offset);
         stick_pos = -(HuPadSubStkY[0] & 0xF8);
         if (stick_pos != 0) {
             debugCamPos[0].x += 0.05f * (offset.x * stick_pos);
             debugCamPos[0].y += 0.05f * (offset.y * stick_pos);
             debugCamPos[0].z += 0.05f * (offset.z * stick_pos);
         }
     }
 }

 void BootTitleCreate(void)
 {
     s16 model;
     s16 sprite;
     ANIMDATA *sprite_data;
     s16 i;
     titleMdlId[0] = model = Hu3DModelCreateFile(TITLE_ANM_VER_ADJUSTED(TITLE_CHAR_HSF));
     Hu3DModelAttrSet(model, HU3D_ATTR_DISPOFF);
     Hu3DModelAttrSet(model, HU3D_MOTATTR_LOOP);
     titleMdlId[1] = model = Hu3DModelCreateFile(TITLE_ANM_VER_ADJUSTED(TITLE_CUBE_HSF));
     Hu3DModelAttrSet(model, HU3D_ATTR_DISPOFF);
     Hu3DModelAttrSet(model, HU3D_MOTATTR_LOOP);
     titleMdlId[2] = model = Hu3DModelCreateFile(TITLE_ANM_VER_ADJUSTED(TITLE_SKY_HSF));
     Hu3DModelAttrSet(model, HU3D_ATTR_DISPOFF);
     Hu3DModelAttrSet(model, HU3D_MOTATTR_LOOP);
     Hu3DModelCameraInfoSet(model, 1);
     Hu3DModelLightInfoSet(model, 1);
#ifdef TARGET_PC
     if (partyboard_version_is_ntsc()) {
         bootGrpId = HuSprGrpCreate(4);
     } else {
         bootGrpId = HuSprGrpCreate(13);
     }
#else
     #if VERSION_NTSC
     bootGrpId = HuSprGrpCreate(4);
     #else
     bootGrpId = HuSprGrpCreate(13);
     #endif
#endif
     sprite_data = HuSprAnimReadFile(TITLE_ANM_VER_ADJUSTED(TITLE_BG_ANM));
     sprite = HuSprCreate(sprite_data, 0, 0);
     HuSprGrpMemberSet(bootGrpId, 0, sprite);
     HuSprAttrSet(bootGrpId, 0, HUSPR_ATTR_DISPOFF);
     HuSprDrawNoSet(bootGrpId, 0, 127);
     HuSprPosSet(bootGrpId, 0, 288, 240);
     sprite_data = HuSprAnimReadFile(TITLE_ANM_VER_ADJUSTED(TITLE_COPYRIGHT_ANM));
     sprite = HuSprCreate(sprite_data, 1, 0);
     HuSprGrpMemberSet(bootGrpId, 1, sprite);
     HuSprAttrSet(bootGrpId, 1, HUSPR_ATTR_DISPOFF);
     HuSprPosSet(bootGrpId, 1, 288, 420);
#ifdef TARGET_PC
     if (partyboard_version_is_ntsc()) {
         sprite_data = HuSprAnimReadFile(TITLE_ANM_VER_ADJUSTED(TITLE_PRESS_START_ANM));
         sprite = HuSprCreate(sprite_data, 2, 0);
         HuSprGrpMemberSet(bootGrpId, 2, sprite);
         HuSprAttrSet(bootGrpId, 2, HUSPR_ATTR_DISPOFF | HUSPR_ATTR_LINEAR);
         HuSprPosSet(bootGrpId, 2, 288, 380);
         sprite_data = HuSprAnimReadFile(TITLE_ANM_VER_ADJUSTED(TITLE_LOGO_ANM));
         sprite = HuSprCreate(sprite_data, 0, 0);
         HuSprGrpMemberSet(bootGrpId, 3, sprite);
         HuSprAttrSet(bootGrpId, 3, HUSPR_ATTR_DISPOFF | HUSPR_ATTR_LINEAR);
         HuSprPosSet(bootGrpId, 3, 288, 200);
     } else {
         sprite_data = HuSprAnimReadFile(TITLE_ANM_VER_ADJUSTED(TITLE_LOGO_ANM));
         sprite = HuSprCreate(sprite_data, 0, 0);
         HuSprGrpMemberSet(bootGrpId, 2, sprite);
         HuSprAttrSet(bootGrpId, 2, HUSPR_ATTR_DISPOFF | HUSPR_ATTR_LINEAR);
         HuSprPosSet(bootGrpId, 2, 288, 200);
         for(i=0; i<5; i++) {
             sprite_data = HuSprAnimReadFile(TITLE_ANM_VER_ADJUSTED(TITLE_PRESS_START_EN_ANM+i));
             sprite = HuSprCreate(sprite_data, 3+i, 0);
             HuSprGrpMemberSet(bootGrpId, 3+i, sprite);
             HuSprAttrSet(bootGrpId, 3+i, HUSPR_ATTR_DISPOFF | HUSPR_ATTR_LINEAR);
             HuSprPosSet(bootGrpId, 3+i, 288, 340);
         }
         for(i=0; i<5; i++) {
             sprite_data = HuSprAnimReadFile(TITLE_ANM_VER_ADJUSTED(TITLE_LANGUAGE_EN_ANM+i));
             sprite = HuSprCreate(sprite_data, 8+i, 0);
             HuSprGrpMemberSet(bootGrpId, 8+i, sprite);
             HuSprAttrSet(bootGrpId, 8+i, HUSPR_ATTR_DISPOFF | HUSPR_ATTR_LINEAR);
             HuSprPosSet(bootGrpId, 8+i, 288, 380);
         }
     }
#else
     #if VERSION_NTSC
     sprite_data = HuSprAnimReadFile(TITLE_PRESS_START_ANM);
     sprite = HuSprCreate(sprite_data, 2, 0);
     HuSprGrpMemberSet(bootGrpId, 2, sprite);
     HuSprAttrSet(bootGrpId, 2, HUSPR_ATTR_DISPOFF | HUSPR_ATTR_LINEAR);
     HuSprPosSet(bootGrpId, 2, 288, 380);
     sprite_data = HuSprAnimReadFile(TITLE_LOGO_ANM);
     sprite = HuSprCreate(sprite_data, 0, 0);
     HuSprGrpMemberSet(bootGrpId, 3, sprite);
     HuSprAttrSet(bootGrpId, 3, HUSPR_ATTR_DISPOFF | HUSPR_ATTR_LINEAR);
     HuSprPosSet(bootGrpId, 3, 288, 200);
     #else
     sprite_data = HuSprAnimReadFile(TITLE_LOGO_ANM);
     sprite = HuSprCreate(sprite_data, 0, 0);
     HuSprGrpMemberSet(bootGrpId, 2, sprite);
     HuSprAttrSet(bootGrpId, 2, HUSPR_ATTR_DISPOFF | HUSPR_ATTR_LINEAR);
     HuSprPosSet(bootGrpId, 2, 288, 200);
     for(i=0; i<5; i++) {
        sprite_data = HuSprAnimReadFile(TITLE_PRESS_START_EN_ANM+i);
        sprite = HuSprCreate(sprite_data, 3+i, 0);
        HuSprGrpMemberSet(bootGrpId, 3+i, sprite);
        HuSprAttrSet(bootGrpId, 3+i, HUSPR_ATTR_DISPOFF | HUSPR_ATTR_LINEAR);
        HuSprPosSet(bootGrpId, 3+i, 288, 340);
     }
     for(i=0; i<5; i++) {
         sprite_data = HuSprAnimReadFile(TITLE_LANGUAGE_EN_ANM+i);
         sprite = HuSprCreate(sprite_data, 8+i, 0);
         HuSprGrpMemberSet(bootGrpId, 8+i, sprite);
         HuSprAttrSet(bootGrpId, 8+i, HUSPR_ATTR_DISPOFF | HUSPR_ATTR_LINEAR);
         HuSprPosSet(bootGrpId, 8+i, 288, 380);
     }
     #endif
#endif
 }

 BOOL BootTitleExec(void)
 {
     float scale;
     float scale_time;
     int seNo[32];
     s16 pressStartMemberNo;
     s16 languageMemberNo;
     s16 i;
     s16 choice;
     float temp;
 repeat:
     Hu3DModelAttrReset(titleMdlId[0], HU3D_ATTR_DISPOFF);
     Hu3DModelAttrReset(titleMdlId[1], HU3D_ATTR_DISPOFF);
     HuSprAttrReset(bootGrpId, 0, HUSPR_ATTR_DISPOFF);
     HuSprAttrReset(bootGrpId, 1, HUSPR_ATTR_DISPOFF);
#ifdef __MWERKS__
     OSReport(">>>>>>>>MSM_SE_SEL_01 %d\n", msmSeGetEntryID(2092, seNo));
     OSReport(">>>>>>>>SE Num %d\n", msmSeGetNumPlay(0));
#endif
     HuAudSStreamPlay(20);
     WipeCreate(WIPE_MODE_IN, WIPE_TYPE_NORMAL, 30);
     while (WipeStatGet()) {
         HuPrcVSleep();
     }
#ifdef TARGET_PC
    if (partyboard_version_is_ntsc()) {
        HuSprAttrReset(bootGrpId, 3, HUSPR_ATTR_DISPOFF);
        for (i = 1; i <= 50; i++) {
            scale = (cosd(i * 1.8) * 10.0) + 1.0;
            HuSprScaleSet(bootGrpId, 3, scale, scale);
            HuSprTPLvlSet(bootGrpId, 3, i / 50.0);
            HuPrcVSleep();
        }
        HuSprAttrReset(bootGrpId, 2, HUSPR_ATTR_DISPOFF);
    } else {
        HuSprAttrReset(bootGrpId, 2, HUSPR_ATTR_DISPOFF);
        for (i = 1; i <= 50; i++) {
            scale = (cosd(i * 1.8) * 10.0) + 1.0;
            HuSprScaleSet(bootGrpId, 2, scale, scale);
            HuSprTPLvlSet(bootGrpId, 2, i / 50.0);
            HuPrcVSleep();
        }
        if(GwLanguage == -1) {
            pressStartMemberNo = 3;
            languageMemberNo = 8;
        } else {
            pressStartMemberNo = GwLanguage+3-1;
            languageMemberNo = GwLanguage+8-1;
        }
        HuSprAttrReset(bootGrpId, pressStartMemberNo, HUSPR_ATTR_DISPOFF);
        HuSprAttrReset(bootGrpId, languageMemberNo, HUSPR_ATTR_DISPOFF);
        for(i=1; i<=20; i++) {
            scale = sind(i*4.5);
            HuSprScaleSet(bootGrpId, pressStartMemberNo, scale, scale);
            HuSprScaleSet(bootGrpId, languageMemberNo, scale*0.7, scale*0.7);
            HuPrcVSleep();
        }
        choice = 0;
        scale_time = 0;
    }
#else
     #if VERSION_NTSC
     HuSprAttrReset(bootGrpId, 3, HUSPR_ATTR_DISPOFF);
     for (i = 1; i <= 50; i++) {
         scale = (cosd(i * 1.8) * 10.0) + 1.0;
         HuSprScaleSet(bootGrpId, 3, scale, scale);
         HuSprTPLvlSet(bootGrpId, 3, i / 50.0);
         HuPrcVSleep();
     }
     HuSprAttrReset(bootGrpId, 2, HUSPR_ATTR_DISPOFF);
     #else
     HuSprAttrReset(bootGrpId, 2, HUSPR_ATTR_DISPOFF);
     for (i = 1; i <= 50; i++) {
         scale = (cosd(i * 1.8) * 10.0) + 1.0;
         HuSprScaleSet(bootGrpId, 2, scale, scale);
         HuSprTPLvlSet(bootGrpId, 2, i / 50.0);
         HuPrcVSleep();
     }
     if(GwLanguage == -1) {
         pressStartMemberNo = 3;
         languageMemberNo = 8;
     } else {
         pressStartMemberNo = GwLanguage+3-1;
         languageMemberNo = GwLanguage+8-1;
     }
     HuSprAttrReset(bootGrpId, pressStartMemberNo, HUSPR_ATTR_DISPOFF);
     HuSprAttrReset(bootGrpId, languageMemberNo, HUSPR_ATTR_DISPOFF);
     for(i=1; i<=20; i++) {
         scale = sind(i*4.5);
         HuSprScaleSet(bootGrpId, pressStartMemberNo, scale, scale);
         HuSprScaleSet(bootGrpId, languageMemberNo, scale*0.7, scale*0.7);
         HuPrcVSleep();
     }
     choice = 0;
     scale_time = 0;
     #endif
#endif

#ifdef TARGET_PC
    temp = 0;
#endif

     #if VERSION_NTSC
     for (i = PB_F2S16(scale_time = 0); i < 1800; i++) {
     #else
     for (i = temp = 0; i < 1800; i++) { 
     #endif
#ifdef TARGET_PC
         if (partyboard_version_is_pal()) {
             if((choice == 0 && (HuPadDStkRep[0] & PAD_BUTTON_DOWN)) || (choice != 0 && (HuPadDStkRep[0] & PAD_BUTTON_UP))) {
                 HuAudFXPlay(0);
                 for(i=1; i<=10; i++) {
                     scale = ((1-(i/10.0))*(((cosd(scale_time)*0.1)+0.9)-0.7))+0.7;
                     if(choice == 0) {
                         HuSprScaleSet(bootGrpId, pressStartMemberNo, scale, scale);
                     } else {
                         HuSprScaleSet(bootGrpId, languageMemberNo, scale, scale);
                     }
                     scale = ((1-(i/10.0))*(((cosd(scale_time/3.0)*0.05)+0.65)-1.0))+1.0;
                     if(choice == 0) {
                         HuSprScaleSet(bootGrpId, languageMemberNo, scale, scale);
                     } else {
                         HuSprScaleSet(bootGrpId, pressStartMemberNo, scale, scale);
                     }
                     HuPrcVSleep();
                 }
                 choice ^= 1;
                 scale_time = 0;
             }
             if (HuPadBtnDown[0] & (PAD_BUTTON_START|PAD_BUTTON_A)) {
                 if(choice == 0) {
                     s32 ret = HuAudFXPlay(2092);
                     if (ret < 0) {
                         OSReport(">>>>>Error %d\n", ret);
                     }
                     HuSprAttrSet(bootGrpId, 2, HUSPR_ATTR_DISPOFF);
                     return 1;
                 } else {
                     HuAudFXPlay(1);
                     HuAudSStreamAllFadeOut(500);
                     WipeColorSet(255, 255, 255);
                     WipeCreate(WIPE_MODE_OUT, WIPE_TYPE_NORMAL, 30);
                     while (WipeStatGet()) {
                         HuPrcVSleep();
                     }
                     Hu3DModelAttrSet(titleMdlId[0], HU3D_ATTR_DISPOFF);
                     Hu3DModelAttrSet(titleMdlId[1], HU3D_ATTR_DISPOFF);
                     Hu3DModelAttrSet(titleMdlId[2], HU3D_ATTR_DISPOFF);
                     HuSprAttrSet(bootGrpId, 0, HUSPR_ATTR_DISPOFF);
                     HuSprAttrSet(bootGrpId, 1, HUSPR_ATTR_DISPOFF);
                     HuSprAttrSet(bootGrpId, 2, HUSPR_ATTR_DISPOFF);
                     HuSprAttrSet(bootGrpId, 2, HUSPR_ATTR_DISPOFF);
                     for(i=0; i<5; i++) {
                         HuSprAttrSet(bootGrpId, i+3, HUSPR_ATTR_DISPOFF);
                     }
                     for(i=0; i<5; i++) {
                         HuSprAttrSet(bootGrpId, i+8, HUSPR_ATTR_DISPOFF);
                     }
                     LanguageMenuExec();
                     Hu3DModelCameraInfoSet(titleMdlId[2], 1);
                     Hu3DModelLightInfoSet(titleMdlId[2], 1);
                     HuWinInit(1);
                     goto repeat;
                 }
             }
             scale = (cosd(scale_time) * 0.1) + 0.9;
             if(choice == 0) {
                 HuSprScaleSet(bootGrpId, pressStartMemberNo, scale, scale);
             } else {
                 HuSprScaleSet(bootGrpId, languageMemberNo, scale, scale);
             }
             scale = (cosd(scale_time/3.0) * 0.05) + 0.65;
             if(choice == 0) {
                 HuSprScaleSet(bootGrpId, languageMemberNo, scale, scale);
             } else {
                 HuSprScaleSet(bootGrpId, pressStartMemberNo, scale, scale);
             }
             scale_time += 5;
             HuPrcVSleep();
         } else {
             if (i <= 10) {
                 HuSprTPLvlSet(bootGrpId, 2, i / 10.0);
             }
             if (HuPadBtnDown[0] & PAD_BUTTON_START) {
                 s32 ret = HuAudFXPlay(2092);
                 if (ret < 0) {
                     OSReport(">>>>>Error %d\n", ret);
                 }
                 HuSprAttrSet(bootGrpId, 2, HUSPR_ATTR_DISPOFF);
                 return 1;
             }
             scale = (sind(i * scale_time) * 0.1) + 0.9;
             scale_time += 0.05;
             if (scale_time > 5) {
                 scale_time = 5;
             }
             HuSprScaleSet(bootGrpId, 2, scale, scale);
             HuPrcVSleep();
         }
#else
         #if VERSION_PAL
         if((choice == 0 && (HuPadDStkRep[0] & PAD_BUTTON_DOWN)) || (choice != 0 && (HuPadDStkRep[0] & PAD_BUTTON_UP))) {
             HuAudFXPlay(0);
             for(i=1; i<=10; i++) {
                 scale = ((1-(i/10.0))*(((cosd(scale_time)*0.1)+0.9)-0.7))+0.7;
                 if(choice == 0) {
                     HuSprScaleSet(bootGrpId, pressStartMemberNo, scale, scale);
                 } else {
                     HuSprScaleSet(bootGrpId, languageMemberNo, scale, scale);
                 }
                 scale = ((1-(i/10.0))*(((cosd(scale_time/3.0)*0.05)+0.65)-1.0))+1.0;
                 if(choice == 0) {
                     HuSprScaleSet(bootGrpId, languageMemberNo, scale, scale);
                 } else {
                     HuSprScaleSet(bootGrpId, pressStartMemberNo, scale, scale);
                 }
                 HuPrcVSleep();
             }
             choice ^= 1;
             scale_time = 0;
         }
         if (HuPadBtnDown[0] & (PAD_BUTTON_START|PAD_BUTTON_A)) {
             if(choice == 0) {
                 s32 ret = HuAudFXPlay(2092);
                 if (ret < 0) {
                     OSReport(">>>>>Error %d\n", ret);
                 }
                 HuSprAttrSet(bootGrpId, 2, HUSPR_ATTR_DISPOFF);
                 return 1;
             } else {
                 HuAudFXPlay(1);
                 HuAudSStreamAllFadeOut(500);
                 WipeColorSet(255, 255, 255);
                 WipeCreate(WIPE_MODE_OUT, WIPE_TYPE_NORMAL, 30);
                 while (WipeStatGet()) {
                     HuPrcVSleep();
                 }
                 Hu3DModelAttrSet(titleMdlId[0], HU3D_ATTR_DISPOFF);
                 Hu3DModelAttrSet(titleMdlId[1], HU3D_ATTR_DISPOFF);
                 Hu3DModelAttrSet(titleMdlId[2], HU3D_ATTR_DISPOFF);
                 HuSprAttrSet(bootGrpId, 0, HUSPR_ATTR_DISPOFF);
                 HuSprAttrSet(bootGrpId, 1, HUSPR_ATTR_DISPOFF);
                 HuSprAttrSet(bootGrpId, 2, HUSPR_ATTR_DISPOFF);
                 HuSprAttrSet(bootGrpId, 2, HUSPR_ATTR_DISPOFF);
                 for(i=0; i<5; i++) {
                     HuSprAttrSet(bootGrpId, i+3, HUSPR_ATTR_DISPOFF);
                 }
                 for(i=0; i<5; i++) {
                     HuSprAttrSet(bootGrpId, i+8, HUSPR_ATTR_DISPOFF);
                 }
                 LanguageMenuExec();
                 Hu3DModelCameraInfoSet(titleMdlId[2], 1);
                 Hu3DModelLightInfoSet(titleMdlId[2], 1);
                 HuWinInit(1);
                 goto repeat;
             }
         }
         scale = (cosd(scale_time) * 0.1) + 0.9;
         if(choice == 0) {
             HuSprScaleSet(bootGrpId, pressStartMemberNo, scale, scale);
         } else {
             HuSprScaleSet(bootGrpId, languageMemberNo, scale, scale);
         }
         scale = (cosd(scale_time/3.0) * 0.05) + 0.65;
         if(choice == 0) {
             HuSprScaleSet(bootGrpId, languageMemberNo, scale, scale);
         } else {
             HuSprScaleSet(bootGrpId, pressStartMemberNo, scale, scale);
         }
         scale_time += 5;
         HuPrcVSleep();
         #else
         if (i <= 10) {
             HuSprTPLvlSet(bootGrpId, 2, i / 10.0);
         }
         if (HuPadBtnDown[0] & PAD_BUTTON_START) {
             s32 ret = HuAudFXPlay(2092);
             if (ret < 0) {
                 OSReport(">>>>>Error %d\n", ret);
             }
             HuSprAttrSet(bootGrpId, 2, HUSPR_ATTR_DISPOFF);
             return 1;
         }
         scale = (sind(i * scale_time) * 0.1) + 0.9;
         scale_time += 0.05;
         if (scale_time > 5) {
             scale_time = 5;
         }
         HuSprScaleSet(bootGrpId, 2, scale, scale);
         HuPrcVSleep();
         #endif
#endif
     }
     WipeColorSet(255, 255, 255);
     WipeCreate(WIPE_MODE_OUT, WIPE_TYPE_NORMAL, 30);
     while (WipeStatGet()) {
         HuPrcVSleep();
     }
     Hu3DModelAttrSet(titleMdlId[0], HU3D_ATTR_DISPOFF);
     Hu3DModelAttrSet(titleMdlId[1], HU3D_ATTR_DISPOFF);
     Hu3DModelAttrSet(titleMdlId[2], HU3D_ATTR_DISPOFF);
#ifdef TARGET_PC
    if (partyboard_version_is_ntsc()) {
        HuSprAttrSet(bootGrpId, 0, HUSPR_ATTR_DISPOFF);
        HuSprAttrSet(bootGrpId, 1, HUSPR_ATTR_DISPOFF);
        HuSprAttrSet(bootGrpId, 2, HUSPR_ATTR_DISPOFF);
        HuSprAttrSet(bootGrpId, 3, HUSPR_ATTR_DISPOFF);
    } else {
        HuSprAttrSet(bootGrpId, 0, HUSPR_ATTR_DISPOFF);
        HuSprAttrSet(bootGrpId, 1, HUSPR_ATTR_DISPOFF);
        HuSprAttrSet(bootGrpId, 2, HUSPR_ATTR_DISPOFF);
        for(i=0; i<5; i++) {
            HuSprAttrSet(bootGrpId, i+3, HUSPR_ATTR_DISPOFF);
        }
        for(i=0; i<5; i++) {
            HuSprAttrSet(bootGrpId, i+8, HUSPR_ATTR_DISPOFF);
        }
    }
#else
#if VERSION_NTSC
     HuSprAttrSet(bootGrpId, 0, HUSPR_ATTR_DISPOFF);
     HuSprAttrSet(bootGrpId, 1, HUSPR_ATTR_DISPOFF);
     HuSprAttrSet(bootGrpId, 2, HUSPR_ATTR_DISPOFF);
     HuSprAttrSet(bootGrpId, 3, HUSPR_ATTR_DISPOFF);
#else
     HuSprAttrSet(bootGrpId, 0, HUSPR_ATTR_DISPOFF);
     HuSprAttrSet(bootGrpId, 1, HUSPR_ATTR_DISPOFF);
     HuSprAttrSet(bootGrpId, 2, HUSPR_ATTR_DISPOFF);
     for(i=0; i<5; i++) {
         HuSprAttrSet(bootGrpId, i+3, HUSPR_ATTR_DISPOFF);
     }
     for(i=0; i<5; i++) {
         HuSprAttrSet(bootGrpId, i+8, HUSPR_ATTR_DISPOFF);
     }
#endif
#endif
     return 0;
 }

#ifdef TARGET_PC
#include "port/dolassets.h"
#else
#include "nintendoData.inc"
#endif

 void *NintendoDataDecode(void)
 {
     /* nintendoData is a macro that ALLOCATES on PC, so it is evaluated once
        and the pointer kept: the buffer must be freed by its own address. */
     void *source = (void *)nintendoData;
     u32 *src = (u32 *)source;

     u32 size = *src++;
     void *dst;
     s32 decode_type;
#ifdef TARGET_PC
     byteswap_u32(&size);
#endif
     dst = HuMemDirectMalloc(HEAP_DATA, size);
     decode_type = *src++;
#ifdef TARGET_PC
     byteswap_s32(&decode_type);
#endif
     if(dst) {
         HuDecodeData(src, dst, size, decode_type);
     }
#ifdef TARGET_PC
     /* src has been advanced twice by *src++, so it is source+8. Freeing THAT
        made the allocator read a block header out of the payload: refused on
        every boot ("HuMem>memory free error"), and occasionally accepted,
        which rewrote the neighbour links from a bogus block and crashed in
        HuMemMemoryFree. Seen 2026-09-12 twice, and 2026-09-13 at 13:05.
        The offset is exactly eight: heap 3 based at 97ed7040, first payload
        at 97ed7080, refused address reported as 97ed7088. */
     if (source) {
         HuMemDirectFree(source);
     }
#endif
     return dst;
 }
