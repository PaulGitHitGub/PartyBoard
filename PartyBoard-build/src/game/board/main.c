#include "port/determinism.h"
#include "game/board/main.h"
#include "game/board/eightlog.h"
#include "ext_math.h"
#include "game/board/battle.h"
#include "game/board/lottery.h"
#include "game/board/model.h"
#include "game/board/pause.h"
#include "game/board/player.h"
#include "game/board/shop.h"
#include "game/board/space.h"
#include "game/board/start.h"
#include "game/board/tutorial.h"
#include "game/board/ui.h"
#include "game/data.h"
#include "game/objsub.h"
#include "game/disp.h"
#include "game/flag.h"
#include "game/gamework_data.h"
#include "game/hu3d.h"
#include "game/msm.h"
#include "game/object.h"
#include "game/pad.h"
#include "game/wipe.h"
#include "string.h"
#if defined(TARGET_PC) && defined(PARTYBOARD_HD_BETA)
#include <stdio.h>
#include <stdlib.h>
#endif
#ifdef TARGET_PC
#include "port/netplay_runtime.h"
#endif

#include "game/armem.h"
#include "game/audio.h"
#include "game/board/audio.h"
#include "game/board/boo_house.h"
#include "game/board/bowser.h"
#include "game/board/fortune.h"
#include "game/board/mg_setup.h"
#include "game/board/roll.h"
#include "game/board/window.h"
#include "game/chrman.h"

extern void BoardLast5Exec(void);

typedef struct camera_view {
    s16 x_rot;
    s16 zoom;
    s16 fov;
} CameraView;

omObjData *boardMainObj;
u32 boardRandSeed;
static omObjData *last5GfxObj;
static omObjData *confettiObj;
static omObjData *filterObj;
SHARED_SYM BoardTurnStartHook boardTurnStartFunc;
SHARED_SYM BoardBowserHook boardBowserHook;
SHARED_SYM void (*boardStarShowNextHook)(void);
SHARED_SYM void (*boardStarGiveHook)(void);
SHARED_SYM BoardFunc boardTurnFunc;
BoardLightHook boardLightResetHook;
BoardLightHook boardLightSetHook;
static BoardFunc destroyFunc;
static BoardFunc createFunc;
static s32 cameraUseBackup;
static omObjData *tauntObj;
static omObjData *cameraObj;
Process *boardObjMan;
Process *boardMainProc;

BoardCameraData boardCamera;
static BoardCameraData cameraBackup;

static OMOVL nextOvl = DLL_NONE;
/* 8-player party: set when the board is torn down only to be re-entered (see
   eightMGPhase): the board overlay is re-entered in place instead of being
   called on top of itself. */
static BOOL boardReloadF;

static CameraView camViewTbl[] = {
    { 0, 0, 25 },
    { -33, 3200, 25 },
    { -33, 2100, 25 },
    { -33, 1800, 25 },
    { -33, 12640, 25 },
    { -33, 3200, 25 },
};


extern void BoardMGSetupPlayClear(void);

static void InitBoardFunc(omObjData *object);
static void ExecBoardFunc(omObjData *object);
static void KillBoardFunc(omObjData *object);
static BOOL eightBattleArrange;
static BOOL eightSoloArrange;
static void EightPlayerSoloArrange(void);
static void EightPlayerBattleArrange(void);
static void UpdateCamera(omObjData *object);

static void CalcCameraTarget(BoardCameraData *camera);
static void CalcCameraPos(BoardCameraData *camera);

static void MainFunc(void);
static void DestroyMainFunc(void);

static s32 ExecTurnStart(void);

static void CreateBoard(void);
static void DestroyBoard(void);

void BoardObjectSetup(BoardFunc create, BoardFunc destroy)
{
    omSysPauseEnable(FALSE);
    if(!_CheckFlag(FLAG_ID_MAKE(1, 0))) {
        _SetFlag(FLAG_ID_MAKE(1, 0));
        _ClearFlag(FLAG_ID_MAKE(2, 1));
        _ClearFlag(FLAG_ID_MAKE(2, 3));
        _ClearFlag(FLAG_ID_MAKE(2, 0));
        _ClearFlag(FLAG_ID_MAKE(2, 2));
        _ClearFlag(FLAG_ID_MAKE(2, 4));
        _ClearFlag(FLAG_ID_MAKE(2, 5));
        _ClearFlag(FLAG_ID_MAKE(1, 6));
        _ClearFlag(FLAG_ID_MAKE(1, 9));
        _ClearFlag(FLAG_ID_MAKE(0, 8));
        _ClearFlag(FLAG_ID_MAKE(0, 10));
        BoardMGSetupPlayClear();
    }

    nextOvl = DLL_NONE;
    _SetFlag(FLAG_ID_MAKE(1, 28));
    _SetFlag(FLAG_ID_MAKE(1, 14));
    _ClearFlag(FLAG_ID_MAKE(1, 16));
    _ClearFlag(FLAG_ID_MAKE(1, 17));
    _ClearFlag(FLAG_ID_MAKE(1, 18));
    _ClearFlag(FLAG_ID_MAKE(1, 19));
    _ClearFlag(FLAG_ID_MAKE(1, 20));
    _ClearFlag(FLAG_ID_MAKE(1, 21));
    _ClearFlag(FLAG_ID_MAKE(1, 23));
    _ClearFlag(FLAG_ID_MAKE(1, 24));
    _ClearFlag(FLAG_ID_MAKE(1, 25));
    _ClearFlag(FLAG_ID_MAKE(1, 27));
    createFunc = create;
    destroyFunc = destroy;
    boardTurnFunc = NULL;
    boardStarShowNextHook = NULL;
    boardBowserHook = NULL;
    boardStarGiveHook = NULL;
    boardTurnStartFunc = NULL;
    /* The 8-player party needs room for twice the per-player objects. */
    boardObjMan = omInitObjMan(GWPlayerCount() == 8 ? 128 : 64, 8192);
    omSystemKeyCheckSetup(boardObjMan);
    boardMainObj = omAddObjEx(boardObjMan, 0, 0, 0, -1, InitBoardFunc);
    switch(omcurovl) {
        case DLL_w01dll:
            GWSystem.board = BOARD_ID_MAIN1;
            break;

        case DLL_w02dll:
            GWSystem.board = BOARD_ID_MAIN2;
            break;

        case DLL_w03dll:
            GWSystem.board = BOARD_ID_MAIN3;
            break;

        case DLL_w04dll:
            GWSystem.board = BOARD_ID_MAIN4;
            break;

        case DLL_w05dll:
            GWSystem.board = BOARD_ID_MAIN5;
            break;

        case DLL_w06dll:
            GWSystem.board = BOARD_ID_MAIN6;
            break;

        case DLL_w10dll:
            GWSystem.board = BOARD_ID_TUTORIAL;
            break;

        case DLL_w20dll:
            GWSystem.board = BOARD_ID_EXTRA1;
            break;

        case DLL_w21dll:
            GWSystem.board = BOARD_ID_EXTRA2;
            break;
    }
}

static void InitBoardFunc(omObjData *object)
{
    boardMainProc = HuPrcChildCreate(MainFunc, 8194, 0x6000, 0, boardObjMan);
    HuPrcDestructorSet2(boardMainProc, DestroyMainFunc);
    object->func = ExecBoardFunc;
}

static void ExecBoardFunc(omObjData *object)
{
    if(!_CheckFlag(FLAG_ID_MAKE(1, 16))) {
        return;
    }
    if(!_CheckFlag(FLAG_ID_MAKE(1, 17))) {
        if(omSysExitReq) {
            BoardKill();
        } else {
            if(BoardPauseReqCheck()) {
                BoardPauseStart();
            } else {
                return;
            }
        }
    } else {
        if(boardMainProc) {
            HuPrcKill(boardMainProc);
        }
        object->func = KillBoardFunc;
    }
}

static void KillBoardFunc(omObjData *object)
{
    if(boardMainProc) {
        return;
    }
    if(WipeStatGet()) {
        return;
        return_alt:
        return;
    } else {
        if(HuARDMACheck()) {
            goto return_alt;
        }
        if(nextOvl != DLL_NONE && boardReloadF) {
            /* 8-player party: the board re-entered in place (not on top of
               itself in the overlay history), as on the return from a minigame. */
            omOvlHisData *his = omOvlHisGet(0);
            boardReloadF = FALSE;
            omOvlGotoEx(nextOvl, 1, his ? his->event : 0, his ? his->stat : 0);
        } else if(nextOvl != DLL_NONE) {
            if(eightBattleArrange) {
                EightPlayerBattleArrange();
            }
            if(eightSoloArrange) {
                EightPlayerSoloArrange();
            }
            omOvlCallEx(nextOvl, 1, 0, 0);
        } else {
            if(_CheckFlag(FLAG_ID_MAKE(1, 11))) {
                BoardTutorialKill();
                BoardTutorialWorkRestore();
            }
            _ClearFlag(FLAG_ID_MAKE(1, 0));
            HuARDirFree(DATADIR_BOARD);
            if(_CheckFlag(FLAG_ID_MAKE(1, 27))) {
                omOvlReturnEx(2, 1);
            } else {
                omOvlReturnEx(1, 1);
            }
        }
    }
}

void BoardKill(void)
{
    _SetFlag(FLAG_ID_MAKE(1, 28));
    _SetFlag(FLAG_ID_MAKE(1, 14));
    if(_CheckFlag(FLAG_ID_MAKE(1, 11))) {
        BoardTutorialHookExec(29, 0);
    }
    HuAudFXAllStop();
    if(!BoardStartCheck()) {
        WipeCreate(WIPE_MODE_OUT, WIPE_TYPE_NORMAL, -1);
        boardTutorialF = 1;
        while(WipeStatGet()) {
            HuPrcVSleep();
        }
    }
    _SetFlag(FLAG_ID_MAKE(1, 17));
    HuPrcKill(boardMainProc);
    DestroyBoard();
}

s32 BoardIsKill(void)
{
    return (_CheckFlag(FLAG_ID_MAKE(1, 17))) ? 1 : 0;
}

void BoardPauseDisableSet(s32 value)
{
    if(_CheckFlag(FLAG_ID_MAKE(1, 11))) {
        _SetFlag(FLAG_ID_MAKE(1, 25));
        omSysPauseEnable(FALSE);
    } else {
        if(value) {
            _SetFlag(FLAG_ID_MAKE(1, 25));
        } else {
            _ClearFlag(FLAG_ID_MAKE(1, 25));
        }
    }
}

s32 BoardPauseDisableGet()
{
    return (_CheckFlag(FLAG_ID_MAKE(1, 25))) ? 1 : 0;
}

void BoardSaveInit(s32 board)
{
    s32 i;
    GWSystem.board = board;
    _ClearFlag(FLAG_ID_MAKE(1, 0));
    _ClearFlag(FLAG_ID_MAKE(1, 1));
    _ClearFlag(FLAG_ID_MAKE(1, 5));
    _ClearFlag(FLAG_ID_MAKE(1, 4));
    _ClearFlag(FLAG_ID_MAKE(1, 6));
    _ClearFlag(FLAG_ID_MAKE(1, 2));
    _ClearFlag(FLAG_ID_MAKE(1, 9));
    _ClearFlag(FLAG_ID_MAKE(1, 7));
    _ClearFlag(FLAG_ID_MAKE(1, 8));
    _ClearFlag(FLAG_ID_MAKE(0, 8));
    _ClearFlag(FLAG_ID_MAKE(1, 12));
    _ClearFlag(FLAG_ID_MAKE(1, 3));
    GWSystem.turn = 1;
    GWSystem.star_flag = 0;
    GWSystem.star_pos = 0;
    GWSystem.star_total = 1;
    GWSystem.last5_effect = 0;
    GWSystem.player_curr = -1;
    GWSystem.bowser_loss = 0;
    GWSystem.bowser_event = 0;
    GWSystem.lucky_value = 1;
    GWSystem.mg_next = 0;
    GWMGTypeSet(0);
    GWSystem.unk_38 = 0;
    GWSystem.block_pos = 0;
    memset(GWSystem.board_data, 0, sizeof(GWSystem.board_data));
    for(i=0; i<GW_PARTY_PLAYER_MAX; i++) {
        BoardPlayerAutoSizeSet(i, 0);
        GWPlayer[i].draw_ticket = 0;
        GWPlayer[i].color = 0;
        GWPlayer[i].bowser_suit = 0;
        GWPlayer[i].jump = 0;
        GWPlayer[i].space_shock = 0;
        GWPlayer[i].spark = 0;
        GWPlayer[i].show_next = 1;
        BoardPlayerCoinsSet(i, 0);
        if(_CheckFlag(FLAG_ID_MAKE(1, 11))) {
            GWSystem.team = 0;
        }
        GWPlayer[i].blue_count = 0;
        GWPlayer[i].red_count = 0;
        GWPlayer[i].question_count = 0;
        GWPlayer[i].fortune_count = 0;
        GWPlayer[i].bowser_count = 0;
        GWPlayer[i].battle_count = 0;
        GWPlayer[i].mushroom_count = 0;
        GWPlayer[i].warp_count = 0;
        GWPlayer[i].coins_mg = 0;
        GWPlayer[i].coins_total = 0;
        GWPlayer[i].coins_max = 0;
        GWPlayer[i].stars_max = 0;
        GWPlayer[i].coins_battle = 0;
        GWPlayer[i].coin_collect = 0;
        GWPlayer[i].coin_win = 0;
        GWPlayer[i].items[0] = -1;
        GWPlayer[i].items[1] = -1;
        GWPlayer[i].items[2] = -1;
        if(GWPartyGet() == 0 || _CheckFlag(FLAG_ID_MAKE(1, 11))) {
            GWStarsSet(i, 0);
        } else {
            GWStarsSet(i, BoardPlayerHandicapGet(i));
        }
    }
}

void BoardStoryConfigSet(s32 mg_list, s32 diff_story)
{
    GWSystem.party = 0;
    GWSystem.team = 0;
#ifdef TARGET_PC
    GWSystem.eight_players = 0; /* Story Mode is always 4 players */
#endif
    GWSystem.diff_story = diff_story;
    GWSystem.bonus_star = 0;
    GWMGListSet(mg_list);
    GWPlayer[0].handicap = 0;
    GWPlayer[1].handicap = 0;
    GWPlayer[2].handicap = 0;
    GWPlayer[3].handicap = 0;
    GWSystem.max_turn = 15;
    memset(GWPlayer, 0, sizeof(GWPlayer));
    _ClearFlag(FLAG_ID_MAKE(0, 2));
    _ClearFlag(FLAG_ID_MAKE(0, 3));
    _ClearFlag(FLAG_ID_MAKE(0, 4));
    _ClearFlag(FLAG_ID_MAKE(0, 5));
    _ClearFlag(FLAG_ID_MAKE(0, 6));
    _ClearFlag(FLAG_ID_MAKE(0, 7));
    _ClearFlag(FLAG_ID_MAKE(1, 11));
    _SetFlag(FLAG_ID_MAKE(0, 11));
    _SetFlag(FLAG_ID_MAKE(1, 10));
}

void BoardPartyConfigSet(s32 team, s32 bonus_star, s32 mg_list, s32 max_turn, s32 p1_handicap, s32 p2_handicap, s32 p3_handicap, s32 p4_handicap)
{
    GWSystem.party = 1;
    GWSystem.team = team;
#ifdef TARGET_PC
    /* A new party is the normal 4-player party unless the caller (Party
       Mode's 8-player set-up, or the test hook) switches it on after this. */
    GWSystem.eight_players = 0;
    /* ...and nothing is pending from an 8-player save looked at in file select. */
    BoardEightResumeClear();
#endif
    GWSystem.diff_story = 0;
    GWSystem.bonus_star = bonus_star;
    GWMGListSet(mg_list);
    GWSystem.max_turn = max_turn;
#ifdef TARGET_PC
    {
        /* Test-only, and identical on both peers because it comes from the
         * command line. A twenty-turn game runs over two hours, so nothing
         * had ever reached the final turn, the bonus stars, the results or
         * the ending. A shorter game exercises all of them. */
        const int override = PartyBoard_NetplayMaxTurnsOverride();
        if (override > 0) {
            GWSystem.max_turn = (u8)override;
        }
    }
#endif
    memset(GWPlayer, 0, sizeof(GWPlayer));
    GWPlayer[0].handicap = p1_handicap;
    GWPlayer[1].handicap = p2_handicap;
    GWPlayer[2].handicap = p3_handicap;
    GWPlayer[3].handicap = p4_handicap;
    _ClearFlag(FLAG_ID_MAKE(1, 11));
    _SetFlag(FLAG_ID_MAKE(0, 11));
    _SetFlag(FLAG_ID_MAKE(1, 10));
}

static void DestroyMainFunc(void)
{
    boardMainProc = NULL;
}

#define CHECK_LAST5_TURN() ((s32)(GWSystem.max_turn-GWSystem.turn) < 5)

/* 8-player party minigames: the existing 4-player minigames are played twice
   per round, by group A and then group B, with the group that plays sitting in
   slots 0-3 (the only slots a minigame knows).

   The groups rotate every round (see EightPlayerRoundGroups) while the turn
   order stays as rolled at the start. The slots are only rearranged while the
   board is torn down, when no board object depends on the slot order:
   - after everyone's turn the board is torn down and re-entered at once
     (phase 3); on re-entry, before the board is built, the round's group A is
     moved into slots 0-3 (in turn order) and group B into 4-7, and group A's
     minigame setup runs;
   - after group A's minigame the halves are swapped (phase 1 -> 2) and group
     B's setup runs;
   - after group B's minigame every player goes back to the slot of their place
     in the turn order (GWPlayerTurnOrderGet) before the round ends.
   Everything per player that has to survive this travels in the player's own
   GWPlayer/GWPlayerCfg record. The board cards are placed by turn order
   (ui.c), so they never move.
   0 = no minigame in progress, 1 = group A's minigame is set up or running,
   2 = group B's, 3 = the board is being re-entered for group A's setup. */
static s8 eightMGPhase;

/* TRUE while group A's minigame is being set up or played (group B still to go). */
s32 BoardEightMGGroupAPending(void)
{
    return GWPlayerCount() == 8 && eightMGPhase == 1;
}

/* 8-player party battle spaces (user's rules): all 8 pay into one pot, the
   round's two fair groups both play the chosen battle minigame, and each group
   plays for half the pot, paid out within the group by the game's own battle
   shares. Like the minigame rotation, players only change slots while the
   board is torn down:
   - BoardEightBattleBegin (battle.c, just before the board goes to the
     minigame) marks the battle; KillBoardFunc then puts this round's group A
     in slots 0-3 and splits the pot (coins_battle) before the overlay starts;
   - back on the board after group A's results, the halves swap and the
     battle space (battle.c) sends group B to the same minigame, the same way
     it sent group A (eightBattlePhase 1 -> 2);
   - back after group B's results, everyone returns to their turn-order slot
     and the battle space finishes as usual.
   0 = no battle, 1 = group A's battle, 2 = group B's. */
static s8 eightBattlePhase;
/* Which board event sent both groups to a minigame: battle space (FLAG(1,4),
   the pot is split) or Bowser's minigame (FLAG(1,3), each group's loser is
   punished: user's choice, 2026-10-16). */
#define EIGHT_EVENT_BATTLE 0
#define EIGHT_EVENT_BOWSER 1
static s8 eightEventKind;
/* Bowser: each group's loser, by place in the turn order (-1 = none). */
static s8 eightBowserLoser[2] = { -1, -1 };
static s32 eightBattlePlayer;
static s32 eightBattleSavedCurr;
static s32 eightBattleMg;

/* Minigames know players 0-3 and some start from the current player (m440):
   the lander if they are in the group that plays, else its first player. */
static void EightPlayerBattleCurr(void)
{
    s32 i;
    GWSystem.player_curr = 0;
    for (i = 0; i < 4; i++) {
        if (GWPlayerTurnOrderGet(&GWPlayer[i]) == eightBattlePlayer) {
            GWSystem.player_curr = i;
        }
    }
}

static BOOL EightPlayerRoundGroups(s8 *src);
static void EightPlayerArrange(const s8 *src);
static void EightPlayerSwapHalves(void);

/* TRUE once, when the battle space resumes after group A's battle: it then
   sends group B to the same minigame, the same way it sent group A. */
static BOOL eightBattleLaunchB;

s32 BoardEightBattleGroupBTake(void)
{
    BOOL launch = eightBattleLaunchB;
    eightBattleLaunchB = FALSE;
    return launch;
}

static s32 EightEventFlag(void)
{
    return eightEventKind == EIGHT_EVENT_BOWSER ? FLAG_ID_MAKE(1, 3) : FLAG_ID_MAKE(1, 4);
}

static const char *EightEventName(void)
{
    return eightEventKind == EIGHT_EVENT_BOWSER ? "bowser minigame" : "battle";
}

/* Bowser: remember the loser of the group in slots 0-3 (the game marks the
   loser with coin_win 1). */
static void EightPlayerBowserLoserKeep(s32 group)
{
    s32 i;
    eightBowserLoser[group] = -1;
    for (i = 0; i < 4; i++) {
        if (GWPlayerCoinWinGet(i) == 1) {
            eightBowserLoser[group] = GWPlayerTurnOrderGet(&GWPlayer[i]);
            EightLog("bowser minigame group %d of 2: loser %s (turn %d)", (int)group + 1, EightLogName(i),
                (int)eightBowserLoser[group] + 1);
            return;
        }
    }
    EightLog("WARNING: bowser minigame group %d of 2: no loser found", (int)group + 1);
}

/* After Bowser's minigame, back in turn order: the slots of the groups' losers. */
s32 BoardEightBowserLosers(s32 *slots)
{
    s32 n = 0;
    s32 g;
    s32 i;
    for (g = 0; g < 2; g++) {
        for (i = 0; i < GWPlayerCount() && eightBowserLoser[g] >= 0; i++) {
            if (GWPlayerTurnOrderGet(&GWPlayer[i]) == eightBowserLoser[g]) {
                slots[n++] = i;
                break;
            }
        }
    }
    return n;
}

void BoardEightBattleBegin(s32 player);

void BoardEightBowserBegin(s32 player)
{
    if (GWPlayerCount() != 8) {
        return;
    }
    BoardEightBattleBegin(player);
    eightEventKind = EIGHT_EVENT_BOWSER;
    eightBowserLoser[0] = eightBowserLoser[1] = -1;
}

void BoardEightBattleBegin(s32 player)
{
    if (GWPlayerCount() != 8) {
        return;
    }
    eightEventKind = EIGHT_EVENT_BATTLE;
    eightBattlePhase = 1;
    eightBattleArrange = TRUE;
    eightBattlePlayer = GWPlayerTurnOrderGet(&GWPlayer[player]);
    eightBattleSavedCurr = GWSystem.player_curr;
    /* Building the board again clears GWSystem.mg_next, so remember which
       battle minigame both groups play. */
    eightBattleMg = GWSystem.mg_next;
}

/* The battle minigame group A played (index into mgInfoTbl), for group B. */
s32 BoardEightBattleMgGet(void)
{
    return eightBattleMg;
}

/* 8-player party: a board event's minigame that only some players play, once
   (not the two-groups treatment): the minigame knows slots 0-3 only, so while
   the board is torn down (KillBoardFunc) those players are moved there, with
   the lander as the current player; back on the board everyone returns to their
   turn-order slot (EightPlayerMGGroupEnter) and player_curr is restored.
   - Reversal of Fortune (m444, fortune.c): the machine's four players.
   - Bowser's Gnarly Party mega-vs-Bowser (m457/m458, w06): only the lander
     plays; the lander's group of this round fills slots 0-3.
   Events set GWPlayerCfg groups before calling; groups travel with players. */
static s8 eightSoloPhase;
static s8 eightSoloPlayers[4];
static s32 eightSoloCount;
static s32 eightSoloLander;
static s32 eightSoloSavedCurr;
static s32 eightSoloMg;
/* A copy: the caller's text may live in a board overlay, unloaded meanwhile. */
static char eightSoloName[32] = "solo minigame";

void BoardEightSoloBegin(const s32 *players, s32 count, s32 lander, const char *name, s32 mg)
{
    s32 i;
    if (GWPlayerCount() != 8) {
        return;
    }
    eightSoloCount = (players != NULL && count > 0) ? (count > 4 ? 4 : count) : 0;
    for (i = 0; i < eightSoloCount; i++) {
        eightSoloPlayers[i] = (s8)players[i];
    }
    eightSoloLander = lander;
    eightSoloSavedCurr = GWSystem.player_curr;
    eightSoloMg = mg;
    snprintf(eightSoloName, sizeof(eightSoloName), "%s", name ? name : "solo minigame");
    eightSoloArrange = TRUE;
    EightLog("%s: %s (turn %d) lands; minigame m%d, %s", eightSoloName, EightLogName(lander),
        (int)GWPlayerTurnOrderGet(&GWPlayer[lander]) + 1, (int)mg + 401,
        eightSoloCount ? "the chosen players play" : "only the lander plays (their group of this round fills slots 1-4)");
}

static void EightPlayerSoloArrange(void)
{
    s8 src[GW_PARTY_PLAYER_MAX];
    BOOL used[GW_PARTY_PLAYER_MAX] = { FALSE };
    s32 a[4] = { 0, 1, 2, 3 };
    s32 b[4] = { 4, 5, 6, 7 };
    s32 lander = GWPlayerTurnOrderGet(&GWPlayer[eightSoloLander]);
    char label[96];
    s32 n = 0;
    s32 i;
    eightSoloArrange = FALSE;
    if (eightSoloCount > 0) {
        for (i = 0; i < eightSoloCount; i++) {
            s32 p = eightSoloPlayers[i];
            if (p < 0 || p >= 8 || used[p]) {
                EightLog("WARNING: %s: bad player list, playing with slots 1-4", eightSoloName);
                return;
            }
            used[p] = TRUE;
            src[n++] = (s8)p;
        }
        for (i = 0; i < 8; i++) {
            if (!used[i]) {
                src[n++] = (s8)i;
            }
        }
        EightPlayerArrange(src);
    }
    else {
        if (EightPlayerRoundGroups(src)) {
            EightPlayerArrange(src);
        }
        else {
            EightLog("WARNING: %s: no fair groups (turn order or teams missing), using the fixed halves", eightSoloName);
        }
        for (i = 4; i < 8; i++) {
            if (GWPlayerTurnOrderGet(&GWPlayer[i]) == lander) {
                EightPlayerSwapHalves();
                break;
            }
        }
    }
    GWSystem.player_curr = 0;
    for (i = 0; i < 4; i++) {
        if (GWPlayerTurnOrderGet(&GWPlayer[i]) == lander) {
            GWSystem.player_curr = i;
        }
    }
    if (GWPlayerTurnOrderGet(&GWPlayer[GWSystem.player_curr]) != lander) {
        EightLog("WARNING: %s: the lander (turn %d) is not in slots 1-4", eightSoloName, (int)lander + 1);
    }
    eightSoloPhase = 1;
    snprintf(label, sizeof(label), "%s: m%d, slots 1-4 (lander %s in slot %d):", eightSoloName, (int)eightSoloMg + 401,
        EightLogName(GWSystem.player_curr), (int)GWSystem.player_curr + 1);
    EightLogPlayers(label, a, 4);
    EightLogPlayers("  waiting:", b, 4);
    BoardEightInputWire(eightSoloName);
}

/* Save/continue (saveload.c). The results screen saves after every minigame,
   so in the 8-player party a save can come between group A's and group B's
   minigame; with this state the continued game plays group B's next, as it
   would have without the break. */
void BoardEightResumeGet(BoardEightResume *out)
{
    out->mg_phase = (eightMGPhase == 1 || eightMGPhase == 2) ? eightMGPhase : 0;
    out->battle_phase = (eightBattlePhase == 1 || eightBattlePhase == 2) ? eightBattlePhase : 0;
    out->event_kind = eightEventKind;
    out->bowser_loser[0] = eightBowserLoser[0];
    out->bowser_loser[1] = eightBowserLoser[1];
    out->battle_player = (s8)eightBattlePlayer;
    out->battle_saved_curr = (s8)eightBattleSavedCurr;
    out->battle_mg = (s16)eightBattleMg;
}

void BoardEightResumeSet(const BoardEightResume *in)
{
    eightMGPhase = (in->mg_phase == 1 || in->mg_phase == 2) ? in->mg_phase : 0;
    eightBattlePhase = (in->battle_phase == 1 || in->battle_phase == 2) ? in->battle_phase : 0;
    eightEventKind = in->event_kind == EIGHT_EVENT_BOWSER ? EIGHT_EVENT_BOWSER : EIGHT_EVENT_BATTLE;
    eightBowserLoser[0] = in->bowser_loser[0];
    eightBowserLoser[1] = in->bowser_loser[1];
    eightBattlePlayer = in->battle_player;
    eightBattleSavedCurr = in->battle_saved_curr;
    eightBattleMg = in->battle_mg;
    eightBattleArrange = FALSE;
    eightBattleLaunchB = FALSE;
    eightSoloArrange = FALSE;
    eightSoloPhase = 0;
}

void BoardEightResumeClear(void)
{
    BoardEightResume none;
    none.mg_phase = 0;
    none.battle_phase = 0;
    none.event_kind = EIGHT_EVENT_BATTLE;
    none.bowser_loser[0] = none.bowser_loser[1] = -1;
    none.battle_player = 0;
    none.battle_saved_curr = 0;
    none.battle_mg = 0;
    BoardEightResumeSet(&none);
}

/* Minigames know two sides: in Tag Battle number the teams in slots 0-3 as 0
   and 1 (the real team stays in GWPlayer), as mg_setup.c does. */
static void EightPlayerMGSides(void)
{
    s32 i;
    for (i = 0; i < 4; i++) {
        GWPlayerTeamBackupSet(i, GWPlayerTeamGet(i));
        GWPlayerCfg[i].group = GWEightMGSide(i);
    }
}

/* Each group plays for half the pot: scale the group's own stakes so they add
   up to its half (the game pays a battle from the stakes of slots 0-3). */
static void EightPlayerBattleSplitGroup(s32 first, s32 half)
{
    s32 stake = 0;
    s32 given = 0;
    s32 i;
    for (i = first; i < first + 4; i++) {
        stake += GWPlayer[i].coins_battle;
    }
    for (i = first; i < first + 4; i++) {
        s32 share = stake > 0 ? GWPlayer[i].coins_battle * half / stake : half / 4;
        GWPlayer[i].coins_battle = share;
        given += share;
    }
    for (i = first; given < half; i = first + (i - first + 1) % 4) {
        GWPlayer[i].coins_battle++;
        given++;
    }
}

/* Called by KillBoardFunc once the board is gone, before the battle minigame. */
static void EightPlayerBattleArrange(void)
{
    s8 src[GW_PARTY_PLAYER_MAX];
    s32 a[4] = { 0, 1, 2, 3 };
    s32 b[4] = { 4, 5, 6, 7 };
    s32 pot = 0;
    s32 i;
    eightBattleArrange = FALSE;
    for (i = 0; i < 8; i++) {
        pot += GWPlayer[i].coins_battle;
    }
    if (EightPlayerRoundGroups(src)) {
        EightPlayerArrange(src);
    }
    else {
        EightLog("WARNING: battle: no fair groups (turn order or teams missing), using the fixed halves");
    }
    EightPlayerMGSides();
    EightPlayerBattleCurr();
    if (eightEventKind == EIGHT_EVENT_BATTLE) {
        EightPlayerBattleSplitGroup(0, pot / 2);
        EightPlayerBattleSplitGroup(4, pot - pot / 2);
        EightLog("battle space (landed: player %d in turn order): pot %d coins, groups play for %d and %d",
            (int)eightBattlePlayer + 1, (int)pot, (int)(pot / 2), (int)(pot - pot / 2));
        EightLogPlayers("battle group 1 of 2 plays:", a, 4);
    }
    else {
        EightLog("bowser space (landed: player %d in turn order): both groups play Bowser's minigame (m%d)",
            (int)eightBattlePlayer + 1, (int)GWSystem.mg_next + 401);
        EightLogPlayers("bowser minigame group 1 of 2 plays:", a, 4);
    }
    EightLogPlayers("  waiting:", b, 4);
    BoardEightInputWire(eightEventKind == EIGHT_EVENT_BOWSER ? "bowser 1 of 2" : "battle 1 of 2");
}

/* TRUE while a group sits in slots 0-3 with its controllers wired to ports
   0-3 (BoardEightInputWire until BoardEightInputUnwire). */
static BOOL eightInputWired;

s32 BoardEightMGGroupWired(void)
{
    return GWPlayerCount() == 8 && eightInputWired;
}

/* Rearrange the player tables: new slot k gets the player from old slot src[k]. */
static void EightPlayerArrange(const s8 *src)
{
    PlayerState state[GW_PARTY_PLAYER_MAX];
    PlayerConfig cfg[GW_PARTY_PLAYER_MAX];
    s32 i;
    memcpy(state, GWPlayer, sizeof(state));
    memcpy(cfg, GWPlayerCfg, sizeof(cfg));
    for (i = 0; i < GW_PARTY_PLAYER_MAX; i++) {
        GWPlayer[i] = state[src[i]];
        GWPlayerCfg[i] = cfg[src[i]];
        GWPlayerStateIdxSet(&GWPlayer[i], i);
    }
}

static void EightPlayerSwapHalves(void)
{
    static const s8 src[GW_PARTY_PLAYER_MAX] = { 4, 5, 6, 7, 0, 1, 2, 3 };
    EightPlayerArrange(src);
}

/* slotOf[place in turn order] = current slot. FALSE when the turn order was
   never recorded (or is broken), e.g. before the start of the game. */
static BOOL EightPlayerSlotsByTurnOrder(s8 *slotOf)
{
    s32 i;
    for (i = 0; i < 8; i++) {
        slotOf[i] = -1;
    }
    for (i = 0; i < 8; i++) {
        s32 order = GWPlayerTurnOrderGet(&GWPlayer[i]);
        if (order < 0 || order >= 8 || slotOf[order] >= 0) {
            return FALSE;
        }
        slotOf[order] = i;
    }
    return TRUE;
}

/* Put every player back in the slot of their place in the turn order. */
static void EightPlayerTurnOrderRestore(void)
{
    s8 slotOf[GW_PARTY_PLAYER_MAX];
    s32 i;
    if (!EightPlayerSlotsByTurnOrder(slotOf)) {
        return;
    }
    for (i = 0; i < 8; i++) {
        if (slotOf[i] != i) {
            EightPlayerArrange(slotOf);
            return;
        }
    }
}

static s32 EightPlayerBitCount(s32 value)
{
    s32 n = 0;
    for (; value != 0; value >>= 1) {
        n += value & 1;
    }
    return n;
}

/* This round's minigame groups, as src for EightPlayerArrange: group A's slots
   (in turn order) then group B's. Round r = GWSystem.turn.
   - Battle Royale: the players are numbered 0-7 by turn order; split s (0-6)
     with v = s + 1 puts in group A the players x with an even number of bits
     in (x & v). These are the 7 parallel classes of planes of AG(3,2): over 7
     rounds every two players share a group exactly 3 times. Round r uses split
     (r - 1) mod 7.
   - Tag Battle: the teams are lettered A-D in the order they first appear in
     the turn order; round r uses pairing (r - 1) mod 3 of AB|CD, AC|BD, AD|BC,
     so every two teams meet once every 3 rounds.
   Returns FALSE (and leaves src alone) when the players can't be grouped. */
static BOOL EightPlayerRoundGroups(s8 *src)
{
    s8 slotOf[GW_PARTY_PLAYER_MAX];
    BOOL inA[GW_PARTY_PLAYER_MAX];
    s32 round = GWSystem.turn > 0 ? GWSystem.turn - 1 : 0;
    s32 i, n;
    if (!EightPlayerSlotsByTurnOrder(slotOf)) {
        return FALSE;
    }
    if (GWTeamGet()) {
        s8 letter[4] = { -1, -1, -1, -1 };
        s8 members[4] = { 0, 0, 0, 0 };
        s32 letters = 0;
        s32 partner = 1 + round % 3;
        for (i = 0; i < 8; i++) {
            s32 team = GWPlayerTeamGet(slotOf[i]) & 3;
            if (letter[team] < 0) {
                letter[team] = letters++;
            }
            members[team]++;
        }
        for (i = 0; i < 4; i++) {
            if (members[i] != 2) {
                return FALSE;
            }
        }
        for (i = 0; i < 8; i++) {
            s32 l = letter[GWPlayerTeamGet(slotOf[i]) & 3];
            inA[i] = l == 0 || l == partner;
        }
    } else {
        s32 v = 1 + round % 7;
        for (i = 0; i < 8; i++) {
            inA[i] = (EightPlayerBitCount(i & v) & 1) == 0;
        }
    }
    for (n = i = 0; i < 8; i++) {
        if (inA[i]) {
            n++;
        }
    }
    if (n != 4) {
        return FALSE;
    }
    for (n = i = 0; i < 8; i++) {
        if (inA[i]) {
            src[n++] = slotOf[i];
        }
    }
    for (i = 0; i < 8; i++) {
        if (!inA[i]) {
            src[n++] = slotOf[i];
        }
    }
    return TRUE;
}

/* game.log: the four players in the minigame about to be set up (slots 0-3)
   and the four waiting (4-7). */
static void EightPlayerGroupsLog(const char *which)
{
    s32 a[4] = { 0, 1, 2, 3 };
    s32 b[4] = { 4, 5, 6, 7 };
    char label[64];
    snprintf(label, sizeof(label), "round %d minigame %s plays:", (int)GWSystem.turn, which);
    EightLogPlayers(label, a, 4);
    EightLogPlayers("  waiting:", b, 4);
}

/* game.log: after a round, check every player is back in their turn-order slot. */
static void EightPlayerRestoreLog(void)
{
    s32 all[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
    s32 i;
    for (i = 0; i < 8; i++) {
        if (GWPlayerTurnOrderGet(&GWPlayer[i]) != i) {
            EightLogPlayers("WARNING: turn order not restored:", all, 8);
            return;
        }
    }
    {
        char label[64];
        snprintf(label, sizeof(label), "round %d done, turn order restored:", (int)GWSystem.turn);
        EightLogPlayers(label, all, 8);
    }
}

/* ---- Controllers while a minigame group plays (8-player party) ----
   The minigames are the original 4-player code: they read ports 0-3 for the
   players in slots 0-3. While a group is arranged, every slot k uses game port
   k (GWPlayer[k].port and GWPlayerCfg[k].pad_idx) and the pad layer feeds game
   port k from the controller of the player in slot k (HuPadWiringSet), so each
   person keeps driving their own character; rumble follows the same table.
   When the turn order comes back, everyone gets their own port again. Each
   player's own port and seat travel in their record (GWPlayerOwnPortGet). */

/* "seat 6 Wario (human, port 6)" for slot k. */
static void EightPlayerSeatText(s32 k, char *buf, size_t size)
{
    s32 own = GWPlayerOwnPortGet(&GWPlayer[k]);
    if (own < 0) {
        snprintf(buf, size, "%s (%s, no own port)", EightLogName(k), GWPlayer[k].com ? "COM" : "human");
        return;
    }
    snprintf(buf, size, "seat %d %s (%s, port %d)", (int)GWPlayerSeatGet(&GWPlayer[k]) + 1, EightLogName(k),
        GWPlayer[k].com ? "COM" : "human", (int)own + 1);
}

void BoardEightInputWire(const char *which)
{
    s8 wire[GW_PARTY_PLAYER_MAX];
    u32 used = 0;
    char buf[1024];
    s32 len;
    s32 k;
    for (k = 0; k < 8; k++) {
        s32 own = GWPlayerOwnPortGet(&GWPlayer[k]);
        wire[k] = -1;
        if (own >= 0 && !(used & (1u << own))) {
            wire[k] = (s8)own;
            used |= 1u << own;
        }
    }
    for (k = 0; k < 8; k++) {
        if (wire[k] < 0) {
            /* Only for a broken setup (no own port recorded, or two players on
               one port): any free port, so the table stays one-to-one. */
            s32 port = 0;
            while (used & (1u << port)) {
                port++;
            }
            wire[k] = (s8)port;
            used |= 1u << port;
            EightLog("WARNING: minigame wiring: %s has no port of their own (own port %d), using port %d",
                EightLogName(k), (int)GWPlayerOwnPortGet(&GWPlayer[k]) + 1, (int)port + 1);
        }
        GWPlayer[k].port = (s8)k;
        GWPlayerCfg[k].pad_idx = (s16)k;
    }
    if (!HuPadWiringSet(wire)) {
        EightLog("WARNING: minigame wiring (round %d, minigame %s) refused; ports unchanged", (int)GWSystem.turn, which);
    }
    eightInputWired = TRUE;
    for (len = 0, k = 0; k < 8 && len < (s32)sizeof(buf) - 96; k++) {
        char seat[80];
        EightPlayerSeatText(k, seat, sizeof(seat));
        len += snprintf(buf + len, sizeof(buf) - len, "%s%sport %d <- %s", k == 0 ? "" : ", ",
            k == 4 ? "waiting: " : "", (int)k + 1, seat);
    }
    EightLog("minigame wiring (round %d, minigame %s): %s", (int)GWSystem.turn, which, buf);
#ifdef TARGET_PC
    if (PartyBoard_NetplayEnabled()) {
        /* Online: which online seat's controller reaches each game port (seat N
           = device port N on every copy). Every copy must print the same line. */
        for (len = 0, k = 0; k < 8 && len < (s32)sizeof(buf) - 96; k++) {
            const s32 dev = HuPadWiringDeviceGet(k);
            if (dev < PartyBoard_NetplayPlayerCount()) {
                len += snprintf(buf + len, sizeof(buf) - len, "%sgame port %d <- online seat %d (%s)", k == 0 ? "" : ", ",
                    (int)k + 1, (int)dev + 1, EightLogName(k));
            } else {
                len += snprintf(buf + len, sizeof(buf) - len, "%sgame port %d <- computer %s", k == 0 ? "" : ", ", (int)k + 1,
                    EightLogName(k));
            }
        }
        OSReport("[netplay] 8p wiring (round %d, minigame %s): %s\n", (int)GWSystem.turn, which, buf);
    }
#endif
}

/* Everyone back on their own port. Quiet when nothing was wired (game start). */
void BoardEightInputUnwire(void)
{
    char buf[1024];
    s32 len;
    s32 k;
    BOOL changed = HuPadWiringActive() || eightInputWired;
    eightInputWired = FALSE;
    for (k = 0; k < 8; k++) {
        s32 own = GWPlayerOwnPortGet(&GWPlayer[k]);
        if (own >= 0 && (GWPlayer[k].port != own || GWPlayerCfg[k].pad_idx != own)) {
            GWPlayer[k].port = (s8)own;
            GWPlayerCfg[k].pad_idx = (s16)own;
            changed = TRUE;
        }
    }
    HuPadWiringReset();
    if (!changed) {
        return;
    }
    for (len = 0, k = 0; k < 8 && len < (s32)sizeof(buf) - 96; k++) {
        char seat[80];
        EightPlayerSeatText(k, seat, sizeof(seat));
        len += snprintf(buf + len, sizeof(buf) - len, "%s%s", k == 0 ? "" : ", ", seat);
    }
    EightLog("minigame wiring restored (round %d): everyone on their own port: %s", (int)GWSystem.turn, buf);
}

/* game.log: whose turn it is and how they play (8-player party). */
static void EightPlayerTurnLog(s32 player)
{
    char seat[80];
    EightPlayerSeatText(player, seat, sizeof(seat));
    EightLog("turn: round %d, turn %d of 8: %s", (int)GWSystem.turn, (int)player + 1, seat);
}

/* Called when the board is re-entered, before it is built. Returns TRUE when
   a group's minigame should be set up now. */
static s32 EightPlayerMGGroupEnter(void)
{
    if (GWPlayerCount() != 8) {
        eightMGPhase = 0;
        eightBattlePhase = 0;
        return FALSE;
    }
    if (eightBattlePhase == 1 && _CheckFlag(EightEventFlag())) {
        /* Group A's minigame is done: group B plays the same minigame now. */
        s32 a[4] = { 0, 1, 2, 3 };
        s32 b[4] = { 4, 5, 6, 7 };
        char label[64];
        if (eightEventKind == EIGHT_EVENT_BOWSER) {
            EightPlayerBowserLoserKeep(0);
        }
        EightPlayerSwapHalves();
        EightPlayerMGSides();
        EightPlayerBattleCurr();
        eightBattlePhase = 2;
        eightBattleLaunchB = TRUE;
        snprintf(label, sizeof(label), "%s group 2 of 2 plays:", EightEventName());
        EightLogPlayers(label, a, 4);
        EightLogPlayers("  waiting:", b, 4);
        BoardEightInputWire(eightEventKind == EIGHT_EVENT_BOWSER ? "bowser 2 of 2" : "battle 2 of 2");
        return FALSE;
    }
    if (eightBattlePhase == 2 && _CheckFlag(EightEventFlag())) {
        s32 all[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
        char label[64];
        if (eightEventKind == EIGHT_EVENT_BOWSER) {
            EightPlayerBowserLoserKeep(1);
        }
        eightBattlePhase = 0;
        EightPlayerTurnOrderRestore();
        BoardEightInputUnwire();
        GWSystem.player_curr = eightBattleSavedCurr;
        snprintf(label, sizeof(label), "%s done, turn order restored:", EightEventName());
        EightLogPlayers(label, all, 8);
        return FALSE;
    }
    eightBattlePhase = 0;
    if (eightMGPhase == 3) {
        s8 src[GW_PARTY_PLAYER_MAX];
        if (EightPlayerRoundGroups(src)) {
            EightPlayerArrange(src);
        }
        else {
            EightLog("WARNING: round %d: no fair groups (turn order or teams missing), using the fixed halves",
                (int)GWSystem.turn);
        }
        eightMGPhase = 1;
        EightPlayerGroupsLog("1 of 2");
        BoardEightInputWire("1 of 2");
        return TRUE;
    }
    if (eightMGPhase == 1 && _CheckFlag(FLAG_ID_MAKE(1, 2))) {
        EightPlayerSwapHalves();
        eightMGPhase = 2;
        EightPlayerGroupsLog("2 of 2");
        BoardEightInputWire("2 of 2");
        return TRUE;
    }
    /* Back from group B's minigame (or a new or resumed game): turn order. */
    eightMGPhase = 0;
    EightPlayerTurnOrderRestore();
    BoardEightInputUnwire();
    if (eightSoloPhase) {
        s32 all[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
        eightSoloPhase = 0;
        GWSystem.player_curr = eightSoloSavedCurr;
        char label[64];
        snprintf(label, sizeof(label), "%s done, turn order restored:", eightSoloName);
        EightLogPlayers(label, all, 8);
    }
    if (_CheckFlag(FLAG_ID_MAKE(1, 2))) {
        EightPlayerRestoreLog();
    }
    return FALSE;
}

static void MainFunc(void)
{
    s32 i;
    s32 fade_enable, turn_cont, fade_type;
    s32 eight_mg_setup;
    fade_enable = 0;
    turn_cont = 0;
    BoardPauseDisableSet(1);
    /* 8-player party: swap the groups (or back) before anything reads the
       player tables, including the end-of-game check just below. */
    eight_mg_setup = EightPlayerMGGroupEnter();
    if(_CheckFlag(FLAG_ID_MAKE(0, 10))) {
        _ClearFlag(FLAG_ID_MAKE(0, 10));
        _SetFlag(FLAG_ID_MAKE(1, 16));
        BoardKill();
        HuPrcSleep(-1);
    }
    _ClearFlag(FLAG_ID_MAKE(0, 10));
    BoardTutorialInit();
    if(_CheckFlag(FLAG_ID_MAKE(1, 11))) {
        BoardTutorialWorkSave();
        BoardTutorialPlayerInit();
        #if VERSION_PAL
        GWMessSpeedSet(3);
        #else
        GWMessSpeedSet(1);
        #endif
    }
    {
        CreateBoard();
        if(eight_mg_setup) {
            /* A group's turn at this round's minigame: straight to the setup. */
            BoardMusStartBoard();
            BoardMGSetupExec();
            HuPrcSleep(-1);
        }
    }
    if(!_CheckFlag(FLAG_ID_MAKE(1, 1))) {
        GWSystem.player_curr = -1;
        BoardStartExec();
        GWSystem.player_curr = 0;
        fade_enable = 1;
        _SetFlag(FLAG_ID_MAKE(1, 1));
    } else {
        fade_enable = 0;
    }
    if(ExecTurnStart()) {
        turn_cont = 1;
    }
    if(CHECK_LAST5_TURN() && GWSystem.player_curr == 0 && !turn_cont) {
        if(!_CheckFlag(FLAG_ID_MAKE(0, 8))) {
            BoardLast5Exec();
            _SetFlag(FLAG_ID_MAKE(0, 8));
            if(fade_enable) {
                /* Last 5 Turns on the game's first turn (only in games shorter
                   than the original's 10-turn minimum, e.g. tests): the opening
                   already used up the "no fade-in" turn, so the board stayed
                   white under Last 5's wipe-out for the whole first turn. */
                fade_enable = 0;
                fprintf(stderr, "[board] last 5 turns on the first turn: board fades back in after it\n");
            }
        } else {
            BoardLast5GfxInit();
        }
    }
    if(!turn_cont) {
        fade_type = 1;
    } else {
        fade_type = 0;
    }
    _ClearFlag(FLAG_ID_MAKE(1, 28));
    do {
        BoardStatusShowSetAll(1);
        if(GWBoardGet() == BOARD_ID_MAIN6 && GWSystem.player_curr == 0 && !turn_cont && boardTurnFunc) {
            GWSystem.player_curr = -1;
            boardTurnFunc();
            GWSystem.player_curr = 0;
        }
        BoardMusStartBoard();
        for(i=GWSystem.player_curr; i<GWPlayerCount(); i++) {
            if(GWBoardGet() == BOARD_ID_EXTRA1 || GWBoardGet() == BOARD_ID_EXTRA2) {
                if(CHECK_LAST5_TURN() && i == 0 && !turn_cont) {
                    BoardLast5GfxInit();
                    _SetFlag(FLAG_ID_MAKE(0, 8));
                }
            }
            if(GWPlayerCount() == 8) {
                EightPlayerTurnLog(i);
            }
            if(_CheckFlag(FLAG_ID_MAKE(1, 6))) {
                BoardPlayerTurnExec(i);
            } else {
                if(!turn_cont) {
                    BoardCameraMoveSet(0);
                    GWSystem.player_curr = i;
                    BoardCameraTargetPlayerSet(i);
                    BoardCameraMotionWait();
                    {
                        Vec pos;
                        BoardSpacePosGet(0, GWPlayer[i].space_curr, &pos);
                        BoardPlayerPosSetV(i, &pos);
                    }
                    while(WipeStatGet()) {
                        HuPrcVSleep();
                    }
                    if(!fade_enable) {
                        if(!fade_type) {
                            WipeCreate(WIPE_MODE_IN, WIPE_TYPE_CROSS, 30);
                            while(WipeStatGet()) {
                                HuPrcVSleep();
                            }
                        } else {
                            fade_type = 0;
                            WipeCreate(WIPE_MODE_IN, WIPE_TYPE_NORMAL, 21);
                            while(WipeStatGet()) {
                                HuPrcVSleep();
                            }
                        }
                    } else {
                        fade_enable = 0;
                    }
                    BoardPlayerTurnExec(i);
                }
            }
            turn_cont = 0;
            if(_CheckFlag(FLAG_ID_MAKE(1, 11))) {
                BoardTutorialHookExec(9, 0);
            }
            if(i != GWPlayerCount()-1 || _CheckFlag(FLAG_ID_MAKE(1, 11))) {
                WipeColorSet(255, 255, 255);
                WipeCreate(WIPE_MODE_OUT, WIPE_TYPE_CROSS, 1);
                while(WipeStatGet()) {
                    HuPrcVSleep();
                }
                BoardPlayerMoveAwayStartCurr(GWPlayer[i].space_curr, 1);
                fade_type = 0;
            } else {
                if(GWBoardGet() == BOARD_ID_EXTRA1 || GWBoardGet() == BOARD_ID_EXTRA2) {
                    if(GWSystem.turn != GWSystem.max_turn) {
                        WipeColorSet(255, 255, 255);
                        WipeCreate(WIPE_MODE_OUT, WIPE_TYPE_CROSS, 1);
                        while(WipeStatGet()) {
                            HuPrcVSleep();
                        }
                        BoardPlayerMoveAwayStartCurr(GWPlayer[i].space_curr, 1);
                        fade_type = 0;
                    }
                } else {
                    WipeColorSet(0, 0, 0);
                    WipeCreate(WIPE_MODE_OUT, WIPE_TYPE_NORMAL, 21);
                    while(WipeStatGet()) {
                        HuPrcVSleep();
                    }
                }
            }
            BoardPlayerPostTurnHookExec(i);
        }
        if(GWBoardGet() == BOARD_ID_EXTRA1 || GWBoardGet() == BOARD_ID_EXTRA2) {
            GWSystem.player_curr = 0;
            if(BoardTurnNext()) {
                BoardAudSeqFadeOut(0, 500);
                BoardKill();
                HuPrcEnd();
                HuPrcSleep(-1);
            }
        } else {
            _SetFlag(FLAG_ID_MAKE(1, 28));
            _SetFlag(FLAG_ID_MAKE(1, 14));
            BoardPauseDisableSet(1);
            _ClearFlag(FLAG_ID_MAKE(1, 9));
            if(_CheckFlag(FLAG_ID_MAKE(2, 0)) || _CheckFlag(FLAG_ID_MAKE(1, 11)) ) {
                for(i=0; i<GWPlayerCount(); i++) {
                    GWPlayer[i].color = 0;
                }
                GWSystem.player_curr = PB_MOD((GWSystem.player_curr+1), GWPlayerCount());
            } else {
                if(GWPlayerCount() == 8) {
                    /* 8-player party: tear the board down and re-enter it, so
                       this round's groups can be moved into their slots while
                       no board object exists (EightPlayerMGGroupEnter). The
                       flag makes the re-entry work like the return from a
                       minigame (items and teams kept); the setup sets it too. */
                    eightMGPhase = 3;
                    GWSystem.player_curr = 0;
                    _SetFlag(FLAG_ID_MAKE(1, 2));
                    boardReloadF = TRUE;
                    BoardNextOvlSet(omcurovl);
                    HuPrcSleep(-1);
                }
                BoardMGSetupExec();
                HuPrcSleep(-1);
            }
        }
    } while(1);
}

s32 BoardTurnNext(void)
{
    s32 i;
    for(i=0; i<GWPlayerCount(); i++) {
        GWPlayer[i].color = 0;
    }
    GWSystem.turn++;
    if(GWSystem.turn > GWSystem.max_turn) {
        return 1;
    } else {
        return 0;
    }
}

static s32 ExecTurnStart(void)
{
    s32 player;
    s32 space;
    if(!BoardStartCheck()) {
        return 0;
    }
    player = GWSystem.player_curr;
    space = GWPlayer[player].space_curr;
    if(_CheckFlag(FLAG_ID_MAKE(1, 5))) {
        BoardFortuneExec(player, space);
        _ClearFlag(FLAG_ID_MAKE(1, 5));
    } else if(_CheckFlag(FLAG_ID_MAKE(1, 4))) {
        BoardMusStartBoard();
        BoardBattleExec(player, space);
        _ClearFlag(FLAG_ID_MAKE(1, 4));
    } else if(_CheckFlag(FLAG_ID_MAKE(1, 3))) {
        BoardBowserExec(player, space);
        _ClearFlag(FLAG_ID_MAKE(1, 3));
    } else if(_CheckFlag(FLAG_ID_MAKE(1, 2))) {
        s32 turn_end = 0;
        BoardCameraMoveSet(0);
        BoardCameraViewSet(2);
        BoardCameraMotionWait();
        turn_end = BoardTurnNext();
        if(turn_end) {
            BoardKill();
            HuPrcEnd();
        }
        _ClearFlag(FLAG_ID_MAKE(1, 2));
        return 0;
    } else if(_CheckFlag(FLAG_ID_MAKE(1, 6))) {
        boardTurnStartFunc(player, space);
        return 1;
    }
    BoardPlayerZoomRestore(player);
    return 1;
}

void BoardNextOvlSet(OMOVL overlay)
{
    nextOvl = overlay;
    BoardAudSeqFadeOut(0, 1000);
    BoardKill();
}

s32 BoardStartCheck(void)
{
    if(_CheckFlag(FLAG_ID_MAKE(1, 2)) || _CheckFlag(FLAG_ID_MAKE(1, 3)) || _CheckFlag(FLAG_ID_MAKE(1, 4)) || _CheckFlag(FLAG_ID_MAKE(1, 5)) || _CheckFlag(FLAG_ID_MAKE(1, 6))) {
        return 1;
    } else {
        return 0;
    }
}

static void CreateBoard(void)
{
    s32 guest_status;

    GWSystem.mg_next = -1;
    if(!GWGameStat.customPackEnable && GWMGListGet() == 2) {
        GWMGListSet(0);
    }
    GWMessSpeedSet(GWMessSpeedGet());
    if(GWMGExplainGet()) {
        _SetFlag(FLAG_ID_MAKE(0, 11));
    } else {
        _ClearFlag(FLAG_ID_MAKE(0, 11));
    }
    if(!BoardStartCheck()) {
        _ClearFlag(FLAG_ID_MAKE(1, 2));
        _ClearFlag(FLAG_ID_MAKE(1, 3));
        _ClearFlag(FLAG_ID_MAKE(1, 4));
        _ClearFlag(FLAG_ID_MAKE(1, 5));
        _ClearFlag(FLAG_ID_MAKE(1, 6));
    }
    _ClearFlag(FLAG_ID_MAKE(1, 8));
    if(!BoardStartCheck()) {
        GWSystem.lucky_value = 1;
    }
    guest_status = BoardDataDirReadAsync(DATADIR_BGUEST);
    if(guest_status != -1) {
        BoardDataAsyncWait(guest_status);
    }
    BoardAudSeqClear();
    BoardModelInit();
    BoardRandInit();
    BoardWinInit();
    BoardPlayerModelInit();
    createFunc();
    BoardLightSetExec();
    BoardLotteryInit();
    BoardShopInit();
    BoardBooHouseCreate();
    BoardCameraInit();
    BoardStatusCreate();
    CharModelDataClose(-1);
    BoardPlayerInit();
    if(GWSystem.last5_effect == 2) {
        BoardSpaceTypeForce(2, 3);
    }
    if(GWSystem.last5_effect == 3) {
        BoardSpaceTypeForce(2, 7);
    }
    BoardCameraMoveSet(0);
    BoardCameraTargetPlayerSet(0);
    BoardCameraViewSet(2);
    BoardCameraMotionWait();
    BoardTauntInit();
    _SetFlag(FLAG_ID_MAKE(1, 14));
    HuDataDirClose(DATADIR_BKOOPASUIT);
    HuDataDirClose(DATADIR_BBATTLE);
    HuDataDirClose(DATADIR_BKOOPA);
    HuDataDirClose(DATADIR_BKUJIYA);
    HuDataDirClose(DATADIR_BYOKODORI);
    HuDataDirClose(DATADIR_BPAUSE);
    HuDataDirClose(DATADIR_BLAST5);
    HuDataDirClose(DATADIR_EFFECT);
    _SetFlag(FLAG_ID_MAKE(1, 16));
}

static void DestroyBoard(void)
{
    s32 dir_table[] = {
        DATADIR_W01,
        DATADIR_W02,
        DATADIR_W03,
        DATADIR_W04,
        DATADIR_W05,
        DATADIR_W06,
        DATADIR_W10,
        DATADIR_W20,
        DATADIR_W21
    };
    BoardTauntKill();
    BoardAudSeqFadeOutAll();
    HuAudAllStop();
    BoardRollKill();
    BoardStatusKill();
    BoardBooHouseKill();
    BoardShopKill();
    BoardLotteryKill(); 
    BoardSpaceDestroy();
    BoardPlayerModelKill();
    if(destroyFunc) {
        destroyFunc();
    }
    BoardModelKillAll();
    BoardWinKillAll();
    HuDataDirClose(dir_table[GWBoardGet()]);
    HuDataDirClose(DATADIR_EFFECT);
    HuDataDirClose(DATADIR_BGUEST);
    HuDataDirClose(DATADIR_BKOOPASUIT);
    HuDataDirClose(DATADIR_BBATTLE);
    HuDataDirClose(DATADIR_BKOOPA);
    HuDataDirClose(DATADIR_BKUJIYA);
    HuDataDirClose(DATADIR_BYOKODORI);
    HuDataDirClose(DATADIR_BOARD);
    createFunc = destroyFunc = NULL;
}

void BoardLightHookSet(BoardLightHook set, BoardLightHook reset)
{
    boardLightSetHook = set;
    boardLightResetHook = reset;
}

void BoardLightSetExec(void)
{
    Hu3DBGColorSet(0, 0, 0);
    if(boardLightSetHook) {
        boardLightSetHook();
    }
}

void BoardLightResetExec(void)
{
    if(boardLightResetHook) {
        boardLightResetHook();
    }
    Hu3DBGColorSet(0, 0, 0);
    Hu3DFogClear();
    Hu3DReflectNoSet(0);
}

//Fixes order of boardCamera and cameraBackup
static BoardCameraData *BoardCameraGet(void)
{
    return &boardCamera;
}

void BoardCameraBackup(void)
{
    cameraUseBackup = TRUE;
    memcpy(&cameraBackup, &boardCamera, sizeof(BoardCameraData));
}

void BoardCameraRestore(void)
{
    if(cameraUseBackup) {
        memcpy(&boardCamera, &cameraBackup, sizeof(BoardCameraData));
        cameraUseBackup = FALSE;
    }
}

void BoardCameraScissorSet(s32 x, s32 y, s32 w, s32 h)
{
    BoardCameraData *camera = &boardCamera;
    Hu3DCameraScissorSet(camera->mask, x, y, w, h);
}

void BoardCameraViewSet(s32 type)
{
    BoardCameraData *camera = &boardCamera;
    BoardFocusData *focus;
    float size;
    if(!cameraObj) {
        return;
    }
    focus = &camera->focus;
    if(type == 0) {
        focus->view_type = 0;
        return;
    }
    OSs16tof32(&camViewTbl[type].fov, &focus->fov_end);
    focus->fov_start = camera->fov;
    OSs16tof32(&camViewTbl[type].zoom, &focus->zoom_end);
    focus->zoom_start = camera->zoom;
    OSs16tof32(&camViewTbl[type].x_rot, &focus->rot_end.x);
    focus->rot_end.y = 0;
    focus->rot_end.z = 0;
    focus->rot_start = camera->rot;
    focus->target_start = camera->target;
    if(BoardPlayerSizeGet(GWSystem.player_curr) == 2 || BoardPlayerGet(GWSystem.player_curr)->bowser_suit) {
        focus->zoom_end += 400.0f;
        size = 2.5f;
    } else {
        size = 1.0f;
    }
    BoardCameraTargetModelSet(BoardPlayerModelGet(GWSystem.player_curr));
    BoardPlayerPosGet(GWSystem.player_curr, &focus->target_end);
    BoardCameraOffsetSet(0.0f, 100.0f*size, 0.0f);
    focus->target_end.y += 100.0f*size;
    _SetFlag(FLAG_ID_MAKE(1, 21));
    focus->view_type = type;
    focus->time = 0;
    if(camera->moving) {
        focus->max_time = 21;
    } else {
        focus->max_time = 1;
    }
}

s32 BoardCameraPosGet(Vec *dst)
{
    BoardCameraData *camera;
    if(!dst) {
        return -1;
    }
    camera = &boardCamera;
    *dst = camera->pos;
    return 0;
}

s32 BoardCameraTargetGet(Vec *dst)
{
    BoardCameraData *camera;
    if(!dst) {
        return -1;
    }
    camera = &boardCamera;
    *dst = camera->target;
    return 0;
}

s32 BoardCameraRotGet(Vec *dst)
{
    BoardCameraData *camera;
    if(!dst) {
        return -1;
    }
    camera = &boardCamera;
    *dst = camera->rot;
    return 0;
}

float BoardCameraZoomGet()
{
    BoardCameraData *camera;
    camera = &boardCamera;
    return camera->zoom;
}

s32 BoardCameraDirGet(Vec *dst)
{
    BoardCameraData *camera;
    if(!dst) {
        return -1;
    }
    camera = &boardCamera;
    if(!camera) {
        return -1;
    }
    VECSubtract(&camera->target, &camera->pos, dst);
    if(dst->x != 0 || dst->y != 0 || dst->z != 0) {
        VECNormalize(dst, dst);
    }
    return 0;
}

s32 BoardCameraPointDirGet(Vec *point, Vec *dst)
{
    BoardCameraData *camera;
    if(!dst || !point) {
        return -1;
    }
    camera = &boardCamera;
    if(!camera) {
        return -1;
    }
    VECSubtract(point, &camera->pos, dst);
    if(dst->x != 0 || dst->y != 0 || dst->z != 0) {
        VECNormalize(dst, dst);
    }
    return 0;
}

void BoardCameraMaskSet(u16 mask)
{
    BoardCameraData *camera = &boardCamera;
    if(!camera) {
        return;
    }
    camera->mask = mask;
}

void BoardCameraMoveSet(s32 move)
{
    BoardCameraData *camera = &boardCamera;
    if(!camera) {
        return;
    }
    if(!move) {
        camera->moving = 0;
    } else {
        camera->moving = 1;
    }
}

void BoardCameraOffsetSet(float x, float y, float z)
{
    BoardCameraData *camera = &boardCamera;
    if(!camera) {
        return;
    }
    camera->offset.x = x;
    camera->offset.y = y;
    camera->offset.z = z;
}

void BoardCameraTargetPlayerSet(s32 player)
{
    PlayerState *player_ptr = BoardPlayerGet(player);
    if(!player_ptr) {
        BoardCameraTargetModelSet(-1);
        return;
    }
    BoardCameraTargetModelSet(BoardPlayerModelGet(player));
    BoardCameraOffsetSet(0, 100, 0);
}

void BoardCameraTargetModelSet(s16 model)
{
    BoardCameraData *camera = &boardCamera;
    if(!camera) {
        return;
    }
    camera->target_mdl = model;
    camera->target_space = -1;
    camera->offset.x = camera->offset.y = camera->offset.z = 0;
}

void BoardCameraTargetSpaceSet(s32 space)
{
    BoardCameraData *camera = &boardCamera;
    if(!camera) {
        return;
    }
    camera->target_mdl = -1;
    camera->target_space = space;
    camera->offset.x = camera->offset.y = camera->offset.z = 0;
}

void BoardCameraPosCalcFuncSet(BoardCameraPosCalcFunc func)
{
    BoardCameraData *camera = &boardCamera;
    if(!camera) {
        return;
    }
    camera->pos_calc = func;
}

void BoardCameraQuakeSet(s32 duration, float strength)
{
    BoardCameraData *camera = &boardCamera;
    if(!camera) {
        return;
    }
    camera->quaking = 1;
    camera->quake_strength = strength;
    camera->quake_timer = duration;
}

void BoardCameraQuakeReset()
{
    BoardCameraData *camera = &boardCamera;
    camera->quaking = 0;
    camera->quake_strength = 0;
    camera->quake_timer = 0;
}

void BoardCameraTargetSet(float x, float y, float z)
{
    BoardCameraData *camera = &boardCamera;
    camera->target.x = x;
    camera->target.y = y;
    camera->target.z = z;
}

void BoardCameraPosSet(float x, float y, float z)
{
    BoardCameraData *camera = &boardCamera;
    camera->pos.x = x;
    camera->pos.y = y;
    camera->pos.z = z;
}

void BoardCameraXRotZoomSet(float zoom, float x_rot)
{
    BoardCameraData *camera = &boardCamera;
    camera->zoom = zoom;
    camera->rot.x = x_rot;
}

void BoardCameraZoomSet(float zoom)
{
    BoardCameraData *camera = &boardCamera;
    camera->zoom = zoom;
}

void BoardCameraRotSet(float x, float y)
{
    BoardCameraData *camera = &boardCamera;
    camera->rot.x = x;
    camera->rot.y = y;
}

void BoardCameraNearFarSet(float nnear, float ffar)
{
    BoardCameraData *camera = &boardCamera;
    camera->nnear = nnear;
    camera->ffar = ffar;
}

void BoardCameraNearFarGet(float *nnear, float *ffar)
{
    BoardCameraData *camera = &boardCamera;
    if(nnear) {
        *nnear = camera->nnear;
    }
    if(ffar) {
        *ffar = camera->ffar;
    }
}

void BoardCameraMotionStart(s16 model_target, Vec *rot_target, float zoom_target, float fov_target)
{
    BoardCameraMotionStartEx(model_target, rot_target, NULL, zoom_target, fov_target, 21);
}

void BoardCameraMotionStartEx(s16 model_target, Vec *rot_target, Vec *offset_end, float zoom_target, float fov_target, s16 max_time)
{
    BoardCameraData *camera = &boardCamera;
    BoardFocusData *focus;
    if(!cameraObj) {
        return;
    }
    focus = &camera->focus;
    focus->fov_start = camera->fov;
    focus->zoom_start = camera->zoom;
    focus->rot_start = camera->rot;
    focus->target_start = camera->target;
    if(fov_target == -1) {
        focus->fov_end = focus->fov_start;
    } else {
        focus->fov_end = fov_target;
    }
    if(zoom_target == -1) {
        focus->zoom_end = focus->zoom_start;
    } else {
        focus->zoom_end = zoom_target;
    }
    if(!rot_target) {
        focus->rot_end = focus->rot_start;
    } else {
        focus->rot_end = *rot_target;
    }
    if(model_target == -1) {
        focus->target_end = focus->target_start;
    } else {
        BoardCameraTargetModelSet(model_target);
        BoardModelPosGet(model_target, &focus->target_end);
    }
    if(offset_end) {
        BoardCameraOffsetSet(offset_end->x, offset_end->y, offset_end->z);
        VECAdd(offset_end, &focus->target_end, &focus->target_end);
    }
    _SetFlag(FLAG_ID_MAKE(1, 21));
    focus->view_type = 5;
    focus->time = 0;
    if(max_time < 0) {
        max_time = 1;
    }
    focus->max_time = max_time;
}

void BoardCameraFovSet(float fov)
{
    BoardCameraData *camera = &boardCamera;
    camera->fov = fov;
}

s32 BoardCameraCullCheck(Vec *point, float radius)
{
    Vec dir;
    Vec pos;
    float dist;
    float dot;
    BoardCameraData *camera = &boardCamera;
    if(!camera->hide_all) {
        return 0;
    }
    BoardCameraDirGet(&dir);
    VECSubtract(point, &camera->pos, &pos);
    dist = sqrtf((pos.x*pos.x)+(pos.y*pos.y)+(pos.z*pos.z));
    if(25000 < dist-(radius*2.0f)) {
        return 0;
    }
    BoardCameraPointDirGet(point, &pos);
    dot = VECDotProduct(&dir, &pos);
    if(BOARD_FABS(dot) < cosd(camera->fov)) {
        return 0;
    } else {
        return 1;
    }
}

s32 BoardCameraMotionIsDone(void)
{
    return (_CheckFlag(FLAG_ID_MAKE(1, 21))) ? 0 : 1;
}

void BoardCameraMotionWait(void)
{
    while(!BoardCameraMotionIsDone()) {
        HuPrcVSleep();
    }
    HuPrcVSleep();
}

void BoardCameraInit(void)
{
    BoardCameraData *camera;
    cameraUseBackup = FALSE;
    memset(&cameraBackup, 0, sizeof(BoardCameraData));
    memset(&boardCamera, 0, sizeof(BoardCameraData));
    camera = &boardCamera;
    camera->fov = 25;
    camera->nnear = 100;
    camera->ffar = 13000;
    camera->aspect = HU_DISP_ASPECT;
    camera->viewport_x = 0;
    camera->viewport_y = 0;
    camera->viewport_h = HU_FB_HEIGHT;
    camera->viewport_w = HU_FB_WIDTH;
    camera->viewport_near = 0;
    camera->viewport_far = 1;
    camera->up.y = 1;
    camera->moving = 0;
    camera->quaking = 0;
    camera->quake_timer = 0;
    camera->pos.x = 0;
    camera->pos.y = 5000;
    camera->pos.z = 5000;
    camera->offset.x = 0;
    camera->offset.y = 0;
    camera->offset.z = 0;
    camera->pos_calc = NULL;
    BoardSpaceFlagPosGet(0, 0x80000000, &camera->target);
    camera->rot.x = camViewTbl[1].x_rot;
    camera->zoom = camViewTbl[1].zoom;
    camera->hide_all = 1;
    camera->mask = 1;
    Hu3DCameraCreate(1);
    Hu3DCameraScissorSet(1, PB_F2U32(camera->viewport_x), PB_F2U32(camera->viewport_y), PB_F2U32(camera->viewport_w), PB_F2U32(camera->viewport_h));
    Hu3DCameraScissorSet(2, 0, 0, 0, 0);
    Hu3DCameraPerspectiveSet(2, -1, 100, 13000, 1.2);
    cameraObj = omAddObjEx(boardObjMan, 32258, 0, 0, -1, UpdateCamera);
}

static inline void CalcCameraView(void)
{
    BoardCameraData *camera = &boardCamera;
    CalcCameraTarget(camera);
    CalcCameraPos(camera);
}

static void UpdateCamera(omObjData *object)
{
    BoardCameraData *camera;
    Vec *target;
    float x, y, z;
    if(BoardIsKill()) {
        omDelObjEx(HuPrcCurrentGet(), object);
        return;
    }
    CalcCameraView();
    camera = &boardCamera;
    Hu3DCameraPerspectiveSet(camera->mask, camera->fov, camera->nnear, camera->ffar, camera->aspect);
    Hu3DCameraViewportSet(camera->mask, camera->viewport_x, camera->viewport_y, camera->viewport_w, camera->viewport_h, camera->viewport_near, camera->viewport_far);
    target = &camera->target;
    if(camera->pos_calc) {
        camera->pos_calc(camera);
    } else {

        x = camera->rot.x;
        y = camera->rot.y;
        camera->pos.x = target->x+(sind(y)*cosd(x)*camera->zoom);
        camera->pos.y = target->y+(-sind(x)*camera->zoom);
        camera->pos.z = target->z+(cosd(y)*cosd(x)*camera->zoom);
        camera->up.x = sind(y)*sind(x);
        camera->up.y = cosd(x);
        camera->up.z = cosd(y)*sind(x);
        if(camera->quaking) {
            x = BoardRandFloat();
            y = BoardRandFloat();
            z = BoardRandFloat();
            camera->pos.x += (x-0.5f)*camera->quake_strength;
            camera->pos.y += (y-0.5f)*camera->quake_strength;
            camera->pos.z += (z-0.5f)*camera->quake_strength;
            if(--camera->quake_timer <= 0) {
                camera->quaking = 0;
                camera->quake_timer = 0;
                camera->quake_strength = 0;
            }
        }
    }
    Hu3DCameraPosSetV(camera->mask, &camera->pos, &camera->up, target);
}

static void CalcCameraTarget(BoardCameraData *camera)
{
    Vec offset;
    Vec pos = { 0, 0, 0 };
    BoardFocusData *focus = &camera->focus;
    if(camera->target_mdl != -1) {
        BoardModelPosGet(camera->target_mdl, &pos);
    } else {
        if(camera->target_space != -1) {
            BoardSpacePosGet(0, camera->target_space, &pos);
        } else {
            return;
        }
    }
    VECAdd(&camera->offset, &pos, &pos);
    VECSubtract(&pos, &camera->target, &offset);
    if(camera->moving) {
        VECScale(&offset, &offset, 0.15f);
    }
    VECAdd(&offset, &camera->target, &camera->target);
}

#define CAM_LERP(t, x1, x2, out) \
{ \
    float result; \
    float offset; \
    float unit_scale; \
    float lerp_t; \
    lerp_x2 = x2; \
    lerp_x1 = x1; \
    lerp_t = t; \
    if(lerp_t2 == lerp_t1) { \
        result = lerp_x2; \
    } else { \
        unit_scale = (lerp_x1-lerp_x2)/((float)lerp_t1-(float)lerp_t2); \
        offset = lerp_x2-(unit_scale*lerp_t2); \
        result = offset+(unit_scale*lerp_t); \
    } \
    out = result; \
}

#define CAM_LERP_VEC(t, x1, x2, out) \
CAM_LERP(t, (x1).x, (x2).x, (out).x) \
CAM_LERP(t, (x1).y, (x2).y, (out).y) \
CAM_LERP(t, (x1).z, (x2).z, (out).z)

static void CalcCameraPos(BoardCameraData *camera)
{
    //These variables must be volatile to match
    volatile u32 lerp_t1, lerp_t2;
    volatile float lerp_x1, lerp_x2;
    float time;

    BoardFocusData *focus = &camera->focus;
    if(focus->time > focus->max_time) {
        focus->view_type = 0;
        _ClearFlag(FLAG_ID_MAKE(1, 21));
        return;
    }
    OSs16tof32(&focus->time, &time);
    focus->time++;
    lerp_t2 = focus->max_time;
    lerp_t1 = 0;
    CAM_LERP(time, focus->zoom_start, focus->zoom_end, camera->zoom)
    CAM_LERP_VEC(time, focus->rot_start, focus->rot_end, camera->rot)
    CAM_LERP(time, focus->fov_start, focus->fov_end, camera->fov)
    CAM_LERP_VEC(time, focus->target_start, focus->target_end, camera->target)
}

void BoardMGDoneFlagSet(s32 flag)
{
    if(flag) {
        _SetFlag(FLAG_ID_MAKE(1, 20));
    } else {
        _ClearFlag(FLAG_ID_MAKE(1, 20));
    }
}

s32 BoardMGDoneFlagGet()
{
    return (_CheckFlag(FLAG_ID_MAKE(1, 20))) ? 1 : 0;
}

void BoardMGExit(void)
{
    s32 player = GWSystem.player_curr;
    BoardPlayerMoveToAsync(player, GWPlayer[GWSystem.player_curr].space_curr);
    BoardCameraTargetPlayerSet(player);
    BoardCameraMoveSet(1);
    BoardCameraViewSet(1);
    _ClearFlag(FLAG_ID_MAKE(1, 19));
}

static void KillBoardMG(omObjData *object)
{
    if(!BoardMGDoneFlagGet()) {
        BoardEventFlagReset();
        omDelObjEx(HuPrcCurrentGet(), object);
    }
}

static void ExecBoardMG(omObjData *object)
{
    if(_CheckFlag(FLAG_ID_MAKE(1, 19))) {
        return;
    }
    if(GWPlayer[GWSystem.player_curr].moving == 0) {
        BoardPlayerMotionShiftSet(GWSystem.player_curr, 1, 0.0f, 10.0f, HU3D_MOTATTR_LOOP);
        if(!_CheckFlag(FLAG_ID_MAKE(1, 21))) {
            _SetFlag(FLAG_ID_MAKE(1, 20));
            object->func = KillBoardMG;
        }
    }
}

void BoardMGCreate(s32 arg0)
{
    _SetFlag(FLAG_ID_MAKE(1, 19));
    _ClearFlag(FLAG_ID_MAKE(1, 20));
    GWSystem.unk_38 = arg0;
    omAddObjEx(boardObjMan, 0x201, 0, 0, -1, ExecBoardMG);
    BoardEventFlagSet();
    BoardSpaceWalkMiniEventExec();
}

void BoardEventFlagSet(void)
{
    _SetFlag(FLAG_ID_MAKE(1, 18));
}

void BoardEventFlagReset(void)
{
    _ClearFlag(FLAG_ID_MAKE(1, 18));
}

s32 BoardEventFlagGet(void)
{
    return _CheckFlag(FLAG_ID_MAKE(1, 18)) ? 1 : 0;
}

void BoardMTXCalcLookAt(Mtx dest, Vec *eye, Vec *up, Vec *target)
{
    Vec f, u, s;
    f.x = eye->x-target->x;
    f.y = eye->y-target->y;
    f.z = eye->z-target->z;
    VECNormalize(&f, &f);
    VECCrossProduct(up, &f, &u);
    VECNormalize(&u, &u);
    VECCrossProduct(&f, &u, &s);
    dest[0][0] = u.x;
    dest[0][1] = u.y;
    dest[0][2] = u.z;
    dest[0][3] = 0;
    dest[1][0] = s.x;
    dest[1][1] = s.y;
    dest[1][2] = s.z;
    dest[1][3] = 0;
    dest[2][0] = f.x;
    dest[2][1] = f.y;
    dest[2][2] = f.z;
    dest[2][3] = 0;
}

float BoardArcSin(float value)
{
    float result;
    s32 sign;
    if(value < 0) {
        sign = 1;
        value = BOARD_FABS(value);
    } else {
        sign = 0;
    }
    if(value > 1.0f) {
        return 0;
    }
    if(value <= (float)(M_PI/2)) {
        result = atanf(value/sqrtf(1-(value*value)));
    } else {
        result = (float)(M_PI/2)-atanf(sqrtf(1-(value*value))/value);
    }
    if(sign) {
        result = BOARD_FABS(result);
    }
    return result;
}

float BoardArcCos(float value)
{
    if(BOARD_FABS(value) > 1) {
        return 0;
    }
    return (float)(M_PI/2)-BoardArcSin(value);
}

#ifdef TARGET_PC
#include "port/netplay_runtime.h"
extern u32 frand_state_get(void);
#endif

#ifdef TARGET_PC
u32 partyboardBoardRandCalls;
#endif

void BoardRandInit(void)
{
#ifdef TARGET_PC
    if (PartyBoard_NetplayEnabled()) {
        /* Derive from synchronized state without consuming another RNG sample. */
        boardRandSeed = frand_state_get() ^ 0x424f4152u;
        return;
    }
#endif
    boardRandSeed = OSGetTime();
}

u32 BoardRand(void)
{
#ifdef TARGET_PC
    ++partyboardBoardRandCalls;
#endif
    boardRandSeed = (boardRandSeed*0x19660D)+(0x3C6EF35F);
    return boardRandSeed;
}

#ifdef TARGET_PC
#include "port/rollback_scene.h"
/* boardRandSeed lives in the dol's own data, not in a snapshotted heap: a
 * restore that skipped it would replay the board with different draws. */
bool PartyBoard_RollbackBoardRandomRegions(PartyBoardRollbackRegionSink sink, void *context)
{
    return sink && sink(context, &boardRandSeed, sizeof(boardRandSeed))
        && sink(context, &partyboardBoardRandCalls, sizeof(partyboardBoardRandCalls));
}
#endif

u32 BoardRandMod(u32 value)
{
    return PB_UMOD((BoardRand() & 0x7FFFFFFF), value);
}

float BoardRandFloat(void)
{
    float value;
    *((u32 *)&value) = (BoardRand() & 0x7FFFFF)|0x3F800000; //Generate float from 1.0f to 2.0f
    return value-1.0f;
}

float BoardVecDistXZCalc(Vec *vec1, Vec *vec2)
{
    float dx = vec1->x-vec2->x;
    float dz = vec1->z-vec2->z;
    return sqrtf((dx*dx)+(dz*dz));
}

s32 BoardVecMaxDistXZCheck(Vec *vec1, Vec *vec2, float max_dist)
{
    float dz = vec1->z-vec2->z;
    float dx = vec1->x-vec2->x;
    float dist = sqrtf((dx*dx)+(dz*dz));
    if(dist <= max_dist) {
        return 1;
    } else {
        return 0;
    }
}

void BoardDAngleCalcVec(Vec *vec1)
{
    s32 i;
    float *data = &vec1->x;
    for(i=0; i<3; i++) {
        while(*data > 180.0f) {
            *data -= 360.0f;
        }
        while(*data < -180.0f) {
            *data += 360.0f;
        }
        data++;
    }
}

float BoardDAngleCalc(float value)
{
    while(value > 180.0f) {
        value -= 360.0f;
    }
    while(value < -180.0f) {
        value += 360.0f;
    }
    return value;
}

s32 BoardDAngleCalcRange(float *value, float min, float range)
{
    float diff = min-(*value);
    if(diff >= 180.0f) {
        min -= 360.0f;
    }
    if(diff <= -180.0f) {
        min += 360.0f;
    }
    if(min > *value) {
        *value += range;
        if(*value >= min) {
            *value = BoardDAngleCalc(min);
            return 1;
        }
    } else {
        *value -= range;
        if(*value <= min) {
            *value = BoardDAngleCalc(min);
            return 1;
        }
    }
    *value = BoardDAngleCalc(*value);
    return 0;
}

s32 BoardVecMinDistCheck(Vec *vec1, Vec *vec2, float min_dist)
{
    Vec temp;
    VECSubtract(vec1, vec2, &temp);
    if(VECSquareMag(&temp) >= (min_dist*min_dist)) {
        return 0;
    } else {
        return 1;
    }
}

typedef struct filter_work {
    struct {
        u8 kill : 1;
        u8 paused : 1;
    };
    u8 max_alpha;
    s16 time;
    s16 len;
    s16 model;
    GXColor color;
    float speed;
} FilterWork;

static void UpdateFilter(omObjData *object);
static void DrawFilter(HU3DMODEL *model, Mtx matrix);

void BoardFilterFadeOut(s16 len)
{
    FilterWork *work;
    float speed;
    if(!filterObj) {
        return;
    }
    if(len <= 0) {
        len = 1;
    }
    work = OM_GET_WORK_PTR(filterObj, FilterWork);
    work->len = len;
    OSs16tof32(&len, &speed);
    work->speed = -(work->color.a)/speed;
    work->paused = 0;
    work->time = work->len;
}

void BoardFilterFadeInit(s16 len, u8 max_alpha)
{
    FilterWork *work;
    if(filterObj) {
        work = OM_GET_WORK_PTR(filterObj, FilterWork);
        work->kill = 1;
        while(filterObj) {
            HuPrcVSleep();
        }
    }
    filterObj = omAddObjEx(boardObjMan, 32000, 0, 0, -1, UpdateFilter);
    omSetStatBit(filterObj, OM_STAT_NOPAUSE|0x80);
    if(len <= 0) {
        len = 1;
    }
    work = OM_GET_WORK_PTR(filterObj, FilterWork);
    work->kill = 0;
    work->paused = 0;
    work->color.r = 0;
    work->color.g = 0;
    work->color.b = 0;
    work->color.a = 0;
    work->max_alpha = max_alpha;
    work->speed = (float)(max_alpha-work->color.a)/(float)len;
    work->time = len;
    work->len = len;
    work->model = Hu3DHookFuncCreate(DrawFilter);
    Hu3DModelLayerSet(work->model, 1);
}

s32 BoardFilterFadePauseCheck(void)
{
    FilterWork *work;
    if(!filterObj) {
        return 1;
    }
    work = OM_GET_WORK_PTR(filterObj, FilterWork);
    return (work->paused) ? 1 : 0;
}

s32 BoardFilterFadeCheck(void)
{
    return (filterObj != NULL) ? 0 : 1;
}

static void UpdateFilter(omObjData *object)
{
    float alpha;
    FilterWork *work = OM_GET_WORK_PTR(object, FilterWork);
    if(work->kill || BoardIsKill()) {
        if(work->model != -1) {
            Hu3DModelKill(work->model);
        }
        filterObj = NULL;
        omDelObjEx(HuPrcCurrentGet(), object);
        return;
    }
    if(work->paused) {
        return;
    }
    OSu8tof32(&work->color.a, &alpha);
    alpha += work->speed;
    OSf32tou8(&alpha, &work->color.a);
    if(work->time > 0) {
        work->time--;
        return;
    }
    if(work->speed > 0) {
        work->paused = 1;
        work->color.a = work->max_alpha;
    } else {
        work->kill = 1;
    }
}

static void DrawFilter(HU3DMODEL *model, Mtx matrix)
{
    static GXColor colorN = { 0xFF, 0xFF, 0xFF, 0xFF };
    Mtx44 proj;
    Mtx modelview;

    float x1, x2, y1, y2;
    FilterWork *work;
    if(!filterObj) {
        return;
    }
    work = OM_GET_WORK_PTR(filterObj, FilterWork);
    x1 = 0.0f;
    x2 = HU_FB_WIDTH;
    y1 = 0.0f;
    y2 = HU_FB_HEIGHT;
    MTXOrtho(proj, y1, y2, x1, x2, 0, 10);
    GXSetProjection(proj, GX_ORTHOGRAPHIC);
    MTXIdentity(modelview);
    GXLoadPosMtxImm(modelview, GX_PNMTX0);
    GXSetCurrentMtx(GX_PNMTX0);
    GXSetViewport(0, 0, x2, 1.0f+y2, 0, 1);
    GXSetScissor(0, 0, PB_F2U32(x2), PB_F2U32(1.0f+y2));
    GXClearVtxDesc();
    GXSetChanMatColor(GX_COLOR0A0, work->color);
    GXSetNumChans(1);
    GXSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_REG, GX_SRC_REG, 0, GX_DF_NONE, GX_AF_NONE);
    GXSetChanCtrl(GX_COLOR1A1, GX_FALSE, GX_SRC_REG, GX_SRC_REG, 0, GX_DF_NONE, GX_AF_NONE);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    GXSetNumTexGens(0);
    GXSetNumTevStages(1);
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XY, GX_U16, 0);
    GXSetZMode(GX_TRUE, GX_ALWAYS, GX_FALSE);
    GXSetAlphaUpdate(GX_FALSE);
    GXSetColorUpdate(GX_TRUE);
    GXSetAlphaCompare(GX_GEQUAL, 1, GX_AOP_AND, GX_GEQUAL, 1);
    GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_NOOP);
    GXBegin(GX_QUADS, GX_VTXFMT0, 4);
    GXPosition2u16(PB_F2U16(x1), PB_F2U16(y1));
    GXPosition2u16(PB_F2U16(x2), PB_F2U16(y1));
    GXPosition2u16(PB_F2U16(x2), PB_F2U16(y2));
    GXPosition2u16(PB_F2U16(x1), PB_F2U16(y2));
    GXEnd();
    GXSetChanMatColor(GX_COLOR0A0, colorN);

}

typedef struct confetti_particle {
    s16 time;
    u8 alpha;
    u8 light_col;
    Vec pos;
    Vec rot;
    Vec pos_vel;
    Vec rot_vel;
} ConfettiParticle;

typedef struct confetti_work {
    struct {
        u8 kill : 1;
        u8 paused : 1;
    };
    s8 spawn_speed;
    s8 time;
    s8 delay;
    s16 count;
    s16 gfx_mdl;
    s16 draw_mdl;
    ConfettiParticle *data;
} ConfettiWork;

static void UpdateConfetti(omObjData *object);
static void SpawnConfetti(omObjData *object);
static void MoveConfetti(omObjData *object);
static void DrawConfetti(HU3DMODEL *model, Mtx matrix);


void BoardConfettiCreate(Vec *pos, s16 count, float range)
{
    omObjData *object;
    ConfettiWork *work;
    if(confettiObj) {
        BoardConfettiStop();
        HuPrcSleep(17);
    }
    object = omAddObjEx(boardObjMan, 257, 0, 0, -1, UpdateConfetti);
    confettiObj = object;
    work = OM_GET_WORK_PTR(object, ConfettiWork);
    work->kill = 0;
    work->paused = 0;
    work->count = count;
    work->spawn_speed = 1;
    work->time = 0;
    work->delay = 10;
    work->draw_mdl = Hu3DHookFuncCreate(DrawConfetti);
    work->data = HuMemDirectMallocNum(HEAP_SYSTEM, work->count*sizeof(ConfettiParticle), MEMORY_DEFAULT_NUM);
    object->trans.x = pos->x;
    object->trans.y = pos->y;
    object->trans.z = pos->z;
    object->rot.x = range;
    work->gfx_mdl = BoardModelCreate(DATA_MAKE_NUM(DATADIR_BOARD, 7), NULL, 0);
    BoardModelLayerSet(work->gfx_mdl, 2);
    BoardModelVisibilitySet(work->gfx_mdl, 0);
    {
        ConfettiParticle *particle;
        s32 i;
        particle = work->data;
        for(i=0; i<work->count; i++, particle++) {
            particle->time = -1;
        }
    }
    HuAudFXPlay(774);
}

void BoardConfettiKill(void)
{
    if(confettiObj) {
        OM_GET_WORK_PTR(confettiObj, ConfettiWork)->kill = 1;
    }
}

void BoardConfettiStop(void)
{
    if(confettiObj) {
        s32 i;
        ConfettiParticle *particle;
        ConfettiWork *work = OM_GET_WORK_PTR(confettiObj, ConfettiWork);
        work->paused = 1;
        particle = work->data;
        for(i=0; i<work->count; i++, particle++) {
            if(particle->time != -1) { 
                if(particle->time > 16) {
                    particle->time = 16;
                }
            }
        }
    }
}

static void UpdateConfetti(omObjData *object)
{
    ConfettiWork *work = OM_GET_WORK_PTR(object, ConfettiWork);
    if(work->kill || BoardIsKill()) {
        BoardModelKill(work->gfx_mdl);
        Hu3DModelKill(work->draw_mdl);
        HuMemDirectFree(work->data);
        confettiObj = NULL;
        omDelObjEx(HuPrcCurrentGet(), object);
    } else {
        SpawnConfetti(object);
        MoveConfetti(object);
    }
}

static void SpawnConfetti(omObjData *object)
{
    ConfettiWork *work = OM_GET_WORK_PTR(object, ConfettiWork);
    s32 i;
    if(work->paused) {
        return;
    }
    if(work->spawn_speed < 5) {
        if(work->time++ > work->delay) {
            work->time = 0;
            work->spawn_speed++;
        }
    }
    for(i=0; i<work->spawn_speed; i++) {
        ConfettiParticle *particle;
        float angle;
        s32 j;
        particle = work->data;
        for(j=0; j<work->count; j++, particle++) {
            if(particle->time == -1) {
                break;
            }
        }
        if(j == work->count) {
            break;
        }
        particle->time = BoardRandMod(60)+120;
        angle = BoardRandFloat()*360.0f;
        particle->pos.x = (sind(angle)*object->rot.x)+object->trans.x;
        particle->pos.y = object->trans.y;
        particle->pos.z = (cosd(angle)*object->rot.x)+object->trans.z;
        particle->pos_vel.x = 2.0f*(BoardRandFloat()-0.5f);
        particle->pos_vel.y = (-98.00001f/15.0f)*BoardRandFloat();
        particle->pos_vel.z = 2.0f*(BoardRandFloat()-0.5f);
        particle->rot_vel.x = 8.0f+((BoardRandFloat()-0.5f)*20.0f);
        particle->rot_vel.y = 8.0f+((BoardRandFloat()-0.5f)*20.0f);
        particle->rot_vel.z = 8.0f+((BoardRandFloat()-0.5f)*20.0f);
        particle->rot.x = 0;
        particle->rot.y = 0;
        particle->rot.z = 0;
        particle->alpha = 255;
        particle->light_col = BoardRandMod(6);
    }
}

static void MoveConfetti(omObjData *object)
{
    ConfettiWork *work = OM_GET_WORK_PTR(object, ConfettiWork);
    s32 i;
    s32 existF;
    ConfettiParticle *particle;

    existF = 0;
    particle = work->data;

    for(i=0; i<work->count; i++, particle++) {
        if(particle->time == -1) {
            continue;
        }
        if(particle->time <= 0) {
            particle->time = -1;
            continue;
        }
        particle->time--;
        particle->pos.x += particle->pos_vel.x;
        particle->pos.y += particle->pos_vel.y;
        particle->pos.z += particle->pos_vel.z;
        particle->rot.x += particle->rot_vel.x;
        particle->rot.y += particle->rot_vel.y;
        particle->rot.z += particle->rot_vel.z;
        if(particle->time < 16) {
            if(particle->alpha >= 15) {
                particle->alpha -= 15;
            } else {
                particle->alpha = 0;
            }
        }
        if(!existF) {
            existF = 1;
        }
    }
    if(existF == 0 && work->paused) {
        work->kill = 1;
    }
}

static Vec confettiLightTbl[6] = {
    { 0.1, 0.4, 1 },
    { 0.2, 1, 0.1 },
    { 0.3, 1, 1 },
    { 1, 0.2, 0.1 },
    { 1, 0.2, 0.8 },
    { 1, 8, 0.3 }
};

static void DrawConfetti(HU3DMODEL *model, Mtx matrix)
{
    if(!confettiObj || BoardIsKill()) {
        return;
    } else {
        ConfettiWork *work = OM_GET_WORK_PTR(confettiObj, ConfettiWork);
        HU3DMODEL *model = &Hu3DData[work->gfx_mdl];
        ConfettiParticle *particle;
        s32 i;
        if(!model->hsf) {
            return;
        }
        particle = work->data;
        for(i=0; i<work->count; i++, particle++) {
            Mtx result, temp;
            float r, g, b, a;
            if(particle->time == -1) {
                continue;
            }
            MTXRotDeg(temp, 'z', particle->rot.z);
            MTXRotDeg(result, 'x', particle->rot.x);
            MTXConcat(temp, result, result);
            MTXRotDeg(temp, 'y', particle->rot.y);
            MTXConcat(temp, result, result);
            MTXTrans(temp, particle->pos.x, particle->pos.y, particle->pos.z);
            MTXConcat(temp, result, result);
            MTXConcat(matrix, result, result);
            r = confettiLightTbl[particle->light_col].x;
            g = confettiLightTbl[particle->light_col].y;
            b = confettiLightTbl[particle->light_col].z;
            OSu8tof32(&particle->alpha, &a);
            a = a*(1.0f/255.0f);
            Hu3DModelTPLvlSet(BoardModelIDGet(work->gfx_mdl), a);
            Hu3DModelAmbSet(BoardModelIDGet(work->gfx_mdl), r, g, b);
            Hu3DModelObjDraw(BoardModelIDGet(work->gfx_mdl), "grid2", result);
        }
    }
}

typedef struct last5_gfx_work {
    struct {
        u8 kill : 1;
        u8 state : 3;
        u8 is_last : 1;
        u8 : 4;
    };
    u8 stop_time;
    s16 time;
    s16 group;
    s16 sprites[3];
} Last5GfxWork;

static s32 last5GfxSprTbl[3] = {
    DATA_MAKE_NUM(DATADIR_BOARD, 95),
    DATA_MAKE_NUM(DATADIR_BOARD, 97),
    DATA_MAKE_NUM(DATADIR_BOARD, 96),
};

static float last5GfxPosTbl[2][3][2] = {
    {
        { -80, 0 },
        { 0, 0 },
        { 80, 0 }
    },
    {
        { -52, 0 },
        { 0, 0 },
        { 52, 0 }
    }
};

static void UpdateLast5Gfx(omObjData *object);

void BoardLast5GfxInit(void)
{
    Last5GfxWork *work;
    omObjData *object;
    s32 turn_remain;
    s32 lastF;
    turn_remain = GWSystem.max_turn-GWSystem.turn;
    if(turn_remain > 4 || turn_remain < 0) {
        return;
    } else {
        s32 i;
        turn_remain = 4-turn_remain;
        object = omAddObjEx(boardObjMan, 0, 0, 0, -1, UpdateLast5Gfx);
        last5GfxObj = object;
        work = OM_GET_WORK_PTR(object, Last5GfxWork);
        work->kill = 0;
        work->stop_time = 0;
        work->time = 0;
        work->group = HuSprGrpCreate(3);
        if((s32)(GWSystem.max_turn-GWSystem.turn) == 0) {
            work->is_last = 1;
            lastF = 1;
        } else {
            work->is_last = 0;
            lastF = 0;
        }
        for(i=0; i<3; i++) {
            s32 prio;
            s32 spr_file;

            if(i == 1) {
                prio = 1000;
            } else {
                prio = 1400;
            }
#if !VERSION_JP
            spr_file = last5GfxSprTbl[i];
            if(i == 2 && work->is_last && GWLanguageGet() != 0) {
                spr_file = DATA_MAKE_NUM(DATADIR_BOARD, 98);
            }
            BoardSpriteCreate(spr_file, prio, NULL, &work->sprites[i]);
#else
            BoardSpriteCreate(last5GfxSprTbl[i], prio, NULL, &work->sprites[i]);
#endif
            HuSprGrpMemberSet(work->group, i, work->sprites[i]);
            HuSprAttrSet(work->group, i, HUSPR_ATTR_LINEAR);
            HuSprPosSet(work->group, i, last5GfxPosTbl[lastF][i][0], last5GfxPosTbl[lastF][i][1]);
        }
        if(!work->is_last) {
            HUSPRITE *sprite = &HuSprData[HuSprGrpData[work->group].members[1]];
            HuSprBankSet(work->group, 1, 0);
            sprite->animNo = turn_remain;
        } else {
            HuSprAttrSet(work->group, 1, HUSPR_ATTR_DISPOFF);
        }
        HuSprAttrSet(work->group, 1, HUSPR_ATTR_NOANIM);
        object->trans.x = 0.0f;
        HuSprGrpTPLvlSet(work->group, object->trans.x);
        HuSprGrpPosSet(work->group, HU_DISP_CENTERX, 72);
        HuAudFXPlay(838);
        work->time = 0;
    }
}

static void UpdateLast5Gfx(omObjData *object)
{
    Last5GfxWork *work = OM_GET_WORK_PTR(object, Last5GfxWork);
    if(work->kill || BoardIsKill()) {
        HuSprGrpKill(work->group);
        last5GfxObj = NULL;
        omDelObjEx(HuPrcCurrentGet(), object);
        return;
    }
    if(work->stop_time != 0) {
        work->stop_time--;
        return;
    }
    switch(work->state) {
        case 0:
            object->trans.x += 1.0f/30.0f;
            if(object->trans.x > 1.0f) {
                object->trans.x = 1.0f;
                work->state = 1;
            }
            HuSprGrpTPLvlSet(work->group, object->trans.x);
            break;

        case 1:
            if(work->time >= 720) {
                work->state = 2;
                work->stop_time = 90;
                if(work->is_last) {
                    HuSprGrpScaleSet(work->group, 1.0f, 1.0f);
                } else {
                    HuSprScaleSet(work->group, 1, 1.0f, 1.0f);
                }
            } else {
                s16 angle;
                angle = work->time%180;
                OSs16tof32(&angle, &object->trans.y);
                object->trans.y = sind(object->trans.y)+0.5;
                if(work->is_last) {
                    HuSprGrpScaleSet(work->group, object->trans.y, object->trans.y);
                } else {
                    HuSprScaleSet(work->group, 1, object->trans.y, object->trans.y);
                }
                work->time += 9;
            }
            break;

        case 2:
            object->trans.x -= 1.0f/30.0f;
            if(object->trans.x < 0.0f) {
                object->trans.x = 0.0f;
                work->kill = 1;
            }
            HuSprGrpTPLvlSet(work->group, object->trans.x);
            break;
    }
}

void BoardLast5GfxShowSet(s32 show)
{
    s32 i;
    Last5GfxWork *work;
    if(!last5GfxObj) {
        return;
    }

    work = OM_GET_WORK_PTR(last5GfxObj, Last5GfxWork);
    for(i=0; i<3; i++) {
        if(show) {
            HuSprAttrReset(work->group, i, HUSPR_ATTR_DISPOFF);
        } else {
            HuSprAttrSet(work->group, i, HUSPR_ATTR_DISPOFF);
        }
        if(work->is_last) {
            HuSprAttrSet(work->group, 1, HUSPR_ATTR_DISPOFF);
        }
    }
}

/* Per controller port: 8 on PC for the 8-player party (ports 0-7). */
static s32 tauntActiveFXTbl[HU_PAD_PORT_MAX] = { -1, -1, -1, -1,
#ifdef TARGET_PC
    -1, -1, -1, -1
#endif
};
static s32 tauntFXTbl[8] = {
    294,
    358,
    422,
    486,
    550,
    614,
    678,
    742
};

typedef struct taunt_work {
    u8 kill : 1;
} TauntWork;

static void TauntUpdate(omObjData *object);

void BoardTauntInit(void)
{
    s32 i;
    tauntObj = omAddObjEx(boardObjMan, 32258, 0, 0, -1, TauntUpdate);
    for(i=0; i<HU_PAD_PORT_MAX; i++) {
        tauntActiveFXTbl[i] = -1;
    }
    _SetFlag(FLAG_ID_MAKE(1, 14));
}

void BoardTauntKill(void)
{
    TauntWork *work;
    if(!tauntObj) {
        return;
    }
    work = OM_GET_WORK_PTR(tauntObj, TauntWork);
    work->kill = 1;
    _SetFlag(FLAG_ID_MAKE(1, 14));
}

static void TauntUpdate(omObjData *object)
{
    s32 i;
    s32 port;
    s32 character;
    TauntWork *work;
    work = OM_GET_WORK_PTR(object, TauntWork);
    if(work->kill || BoardIsKill()) {
        for(i=0; i<HU_PAD_PORT_MAX; i++) {
            if(tauntActiveFXTbl[i] >= 0) {
                HuAudFXStop(tauntActiveFXTbl[i]);
                tauntActiveFXTbl[i] = -1;
            }
        }
        tauntObj = NULL;
        omDelObjEx(HuPrcCurrentGet(), object);
        return;
    }
    for(i=0; i<HU_PAD_PORT_MAX; i++) {
        if(tauntActiveFXTbl[i] >= 0 && HuAudFXStatusGet(tauntActiveFXTbl[i]) == 0) {
            tauntActiveFXTbl[i] = -1;
        }
    }
    if(BoardPauseActiveCheck()) {
        return;
    }
    if(_CheckFlag(FLAG_ID_MAKE(1, 14))) {
        return;
    }
    if(WipeStatGet()) {
        return;
    }
    if(GWSystem.player_curr == -1) {
        return;
    }
    for(i=0; i<GWPlayerCount(); i++) {
        if(i == GWSystem.player_curr || GWPlayer[i].com) {
            continue;
        }
        port = GWPlayer[i].port & (HU_PAD_PORT_MAX - 1);
        character = GWPlayer[i].character & 0x7;
        if(tauntActiveFXTbl[port] >= 0) {
            MSM_SEPARAM param;
            float vol, pan;
            vol = (64.0f*(HuPadSubStkX[port]/59.0f))+64.0f;
            pan = 8191.0f*(HuPadSubStkY[port]/59.0f);
            memset(&param, 0, sizeof(MSM_SEPARAM));
            param.flag = MSM_SEPARAM_PITCH|MSM_SEPARAM_PAN;
            OSf32tos8(&vol, &param.pan);
            OSf32tos16(&pan, &param.pitch);
            msmSeSetParam(tauntActiveFXTbl[port], &param);
        } else {
            if(HuPadBtnDown[port] & PAD_TRIGGER_L) {
                tauntActiveFXTbl[port] = HuAudFXPlay(tauntFXTbl[character]);
            }
        }
    }
}

s32 BoardDataDirReadAsync(s32 data_num)
{
    s32 status = HuDataDirReadAsync(data_num);
    return status;
}

void BoardDataAsyncWait(s32 status)
{
    if(status == -1) {
        return;
    }
    while(!HuDataGetAsyncStat(status)) {
        HuPrcVSleep();
    }
}

#ifdef TARGET_PC
#include "port/rollback_scene.h"
#include "port/netplay_state.h"
/* The 8-player round state lives in statics of this file (dol data), which no
   other rollback region covers. Listed here so a checkpoint saves and restores
   it with the rest; the rollback engine never runs 8-player sessions today
   (refused at start), so this is coverage for later, not a behaviour change. */
bool PartyBoard_RollbackEightRegions(PartyBoardRollbackRegionSink sink, void *context)
{
    if (!sink) return false;
#define EIGHT_REGION(value) if (!sink(context, &(value), sizeof(value))) return false;
    EIGHT_REGION(boardReloadF) EIGHT_REGION(eightBattleArrange) EIGHT_REGION(eightSoloArrange)
    EIGHT_REGION(eightMGPhase) EIGHT_REGION(eightBattlePhase) EIGHT_REGION(eightEventKind)
    EIGHT_REGION(eightBowserLoser) EIGHT_REGION(eightBattlePlayer) EIGHT_REGION(eightBattleSavedCurr)
    EIGHT_REGION(eightBattleMg) EIGHT_REGION(eightBattleLaunchB) EIGHT_REGION(eightSoloPhase)
    EIGHT_REGION(eightSoloPlayers) EIGHT_REGION(eightSoloCount) EIGHT_REGION(eightSoloLander)
    EIGHT_REGION(eightSoloSavedCurr) EIGHT_REGION(eightSoloMg) EIGHT_REGION(eightSoloName)
    EIGHT_REGION(eightInputWired)
#undef EIGHT_REGION
    return true;
}

/* The same state for the online desync check (canonical hash, BOARD), in
   8-player games only, so 4-player hashes stay exactly what they were. */
void PartyBoard_NetplayEightState(PartyBoardNetplayStateSink sink, void *context)
{
    s32 i;
    if (!GWSystem.eight_players) return;
#define WORD(value) sink(context, #value, (uint32_t)(value))
    WORD(boardReloadF); WORD(eightBattleArrange); WORD(eightSoloArrange);
    WORD(eightMGPhase); WORD(eightBattlePhase); WORD(eightEventKind);
    WORD(eightBowserLoser[0]); WORD(eightBowserLoser[1]);
    WORD(eightBattlePlayer); WORD(eightBattleSavedCurr); WORD(eightBattleMg); WORD(eightBattleLaunchB);
    WORD(eightSoloPhase); WORD(eightSoloCount); WORD(eightSoloLander); WORD(eightSoloSavedCurr); WORD(eightSoloMg);
    for (i = 0; i < 4; i++) {
        WORD(eightSoloPlayers[i]);
    }
    WORD(eightInputWired);
#undef WORD
}
#endif
