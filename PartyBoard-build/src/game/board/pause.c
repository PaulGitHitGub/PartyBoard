#include "port/determinism.h"
#include "game/board/pause.h"
#include "game/audio.h"
#include "game/board/main.h"
#include "game/board/model.h"
#include "game/board/player.h"
#include "game/board/roll.h"
#include "game/board/star.h"
#include "game/board/ui.h"
#include "game/data.h"
#include "game/flag.h"
#include "game/gamework_data.h"
#include "game/object.h"
#include "game/objsub.h"
#include "game/pad.h"
#include "game/process.h"
#include "game/saveload.h"
#include "game/sprite.h"
#include "game/window.h"
#include "game/wipe.h"

#include "game/board/audio.h"
#include "game/board/eightlog.h"
#include <stdlib.h>
#include <string.h>
#include "game/hsfex.h"

typedef struct {
    struct {
        u8 unk00_field0 : 1;
        u8 unk00_field1 : 1;
        u8 unk00_field2 : 1;
        u8 unk00_field3 : 3;
        u8 unk00_field4 : 1;
    };
    s8 unk01;
    s8 unk02;
    s8 unk03;
    s8 unk04;
    s8 unk05;
    u8 unk06;
    u8 unk07;
    s8 unk08;
    u8 unk09;
    s16 unk0A;
    u32 unk0C;
} ConfigWork;

static void PauseExit(void);
static void PauseProcess(void);
static void CreatePauseScreen(void);
static void DeletePauseScreen(void);
static void SetBoxVisible(s32 arg0);
static void PauseConfigObjFunc(omObjData *arg0);
static void UpdatePauseText(s32 arg0);
static void InitPauseQuit(omObjData *arg0, ConfigWork *arg1);
static void PauseQuitProcess(void);
static void DeletePauseQuit(void);
static s32 UpdatePadConfig(omObjData *arg0, ConfigWork *arg1);
static void CreatePadConfig(omObjData *arg0, ConfigWork *arg1);
static void ScrollInPadConfig(omObjData *arg0, ConfigWork *arg1);
static void CursorMovePadConfig(omObjData *arg0, ConfigWork *arg1);
static void ChangeDiffPadConfig(omObjData *arg0, ConfigWork *arg1);
static void ScrollOutPadConfig(omObjData *arg0, ConfigWork *arg1);
static s32 UpdatePauseBox(omObjData *arg0, ConfigWork *arg1, s32 arg2);
static s32 UpdatePauseBoxExt(omObjData *arg0, ConfigWork *arg1, s32 arg2);
static s32 ExecPauseConfig(omObjData *arg0, ConfigWork *arg1);
static void CreatePauseControlWin(void);
static void DeletePauseControlWin(void);
static s32 WaitPauseInput(void);
static void ShowPauseConfig(void);
static void CreatePadConfigSprite(void);
static void ShowPadConfigSprite(s32 arg0);
static void PauseCreateNumber(s32 arg0, s8 arg1);
static s32 CheckPort(s32 arg0);
static void RotateBox(float arg0, float arg1, Mtx arg2);

static float boxRot[8];
static Vec padBoxPos;

static s8 pauseQuitF;
static s8 mainScreenF;
static u8 hostOldLayer;
static s16 hostMdl;
static s8 cursorPos;
static s8 boxState[8];
static omObjData *configObj;
static Process *quitProcess;
static Process *mainProcess;

static s8 pausePad = -1;
static s16 pauseSprGrp = -1;
static s16 padConfigSprGrp = -1;
static s16 pauseCursorPos = -1;
static s16 settingsControlWin = -1;
static s16 controlWin = -1;
static s16 quitWin = -1;
static s16 settingsWin = -1;

static s16 boxModelID[8] = { -1, -1, -1, -1, -1, -1, -1, -1 };

static float boxPosTbl[8][2] = { { 120.0f, 120.0f }, { 232.0f, 120.0f }, { 344.0f, 120.0f }, { 456.0f, 120.0f }, { 120.0f, 232.0f },
    { 232.0f, 232.0f }, { 344.0f, 232.0f }, { 456.0f, 232.0f } };

static float padConfigPosTbl[4][2] = { { 170.0f, 160.0f }, { 266.0f, 160.0f }, { 362.0f, 160.0f }, { 458.0f, 160.0f } };

/* 8-player party: the controller page shows all 8 ports, in two rows of four
   (ports 1-4 on top, 5-8 below) at a smaller size, in the original columns.
   Sprite group members: faces 0-7, labels 8-15 (the game's 1P-4P tag or COM
   tag + difficulty), frames 16-23, cork panel 24, and for ports 5-8 a 5P-8P
   tag (approved art, tools/tags8) at 25-28, shown instead of the label when
   that port is a person. Normal games keep the original 13 members. */
#define PAD8_MEMBERS 29
static s8 pause8F;           /* this pause screen is the 8-port page */
static float pad8Scale = 0.75f;
static float pad8RowOfs = 45.0f;
static float pad8PanelScaleY = 1.0f;
static s8 pad8Human[8];      /* the label shown on port N is the person's tag */

static s32 PadNum(void) { return pause8F ? 8 : 4; }
static s32 PadFace(s32 port) { return port; }
static s32 PadLabel(s32 port) { return pause8F ? 8 + port : 4 + port; }
static s32 PadFrame(s32 port) { return pause8F ? 16 + port : 8 + port; }
static s32 PadPanel(void) { return pause8F ? 24 : 12; }
static s32 PadTag8(s32 port) { return 25 + (port - 4); }

static void Pad8LayoutGet(void)
{
    pad8Scale = 0.75f;
    pad8RowOfs = 45.0f;
    pad8PanelScaleY = 1.0f;
#if defined(PARTYBOARD_HD_BETA)
    {
        /* Test aid (beta): PARTYBOARD_TEST_PAUSE8=b shows layout option B
           (larger boxes, cork board stretched taller) for the preview. */
        const char *env = getenv("PARTYBOARD_TEST_PAUSE8");
        if (env != NULL && (env[0] == 'b' || env[0] == 'B')) {
            pad8Scale = 0.82f;
            pad8RowOfs = 49.0f;
            pad8PanelScaleY = 1.12f;
        }
    }
#endif
}

/* Where a port's box is, relative to the sprite group (the original: x 178 +
   96 per port, y 64). */
static void PadBoxPos(s32 port, float *x, float *y)
{
    *x = (port & 3) * 96 + 178;
    *y = 64.0f;
    if (pause8F) {
        *y += (port < 4) ? -pad8RowOfs : pad8RowOfs;
    }
}

/* The pointing hand next to a port's box (screen position). */
static void PadCursorPos(s32 port, float *x, float *y)
{
    if (!pause8F) {
        *x = padConfigPosTbl[port][0];
        *y = padConfigPosTbl[port][1];
        return;
    }
    PadBoxPos(port, x, y);
    *x += 32.0f - 40.0f * pad8Scale;
    *y += 112.0f - 16.0f * pad8Scale;
}

/* The label under/over a port's face: for a person the port's own nP tag
   (bank = the port, as the original), for a computer bank 4-7 = COM tag and
   difficulty. */
static void PadLabelBankSet(s32 port, s32 human, s32 bank, s32 shown)
{
    if (!pause8F || port < 4) {
        HuSprBankSet(padConfigSprGrp, PadLabel(port), bank);
        return;
    }
    pad8Human[port] = human != 0;
    if (human) {
        HuSprBankSet(padConfigSprGrp, PadTag8(port), port - 4);
    }
    else {
        HuSprBankSet(padConfigSprGrp, PadLabel(port), bank);
    }
    if (shown) {
        if (pad8Human[port]) {
            HuSprAttrSet(padConfigSprGrp, PadLabel(port), HUSPR_ATTR_DISPOFF);
            HuSprAttrReset(padConfigSprGrp, PadTag8(port), HUSPR_ATTR_DISPOFF);
        }
        else {
            HuSprAttrReset(padConfigSprGrp, PadLabel(port), HUSPR_ATTR_DISPOFF);
            HuSprAttrSet(padConfigSprGrp, PadTag8(port), HUSPR_ATTR_DISPOFF);
        }
    }
}

#include "tags5to8_pause.inc"

/* The 5P-8P person tags: a copy of the game's own label sprite (BPAUSE 1)
   with banks 0-3 (1P-4P) only and the tag atlas swapped for the approved
   5P-8P atlas (same layout). NULL (logged) if the data has another shape. */
static ANIMDATA *Pad8TagAnim(ANIMDATA *orig)
{
    ANIMDATA *anim;
    ANIMBANK *bank;
    ANIMFRAME *frame;
    ANIMPAT *pat;
    ANIMLAYER *layer;
    ANIMBMP *bmp;
    s32 frameNum = 0;
    s32 layerNum = 0;
    s32 i, j;

    if (orig == NULL || orig->bankNum < 4 || (orig->bmpNum & ANIM_BMP_NUM_MASK) < 1 || orig->bmp[0].sizeX != 200
        || orig->bmp[0].sizeY != 64) {
        EightLog("WARNING: pause menu: 5P-8P tags not made: the game's tag data is %dx%d with %d bank(s), expected 200x64 "
                 "with 8; ports 5-8 show the 1P-4P tags",
            orig ? (int)orig->bmp[0].sizeX : -1, orig ? (int)orig->bmp[0].sizeY : -1, orig ? (int)orig->bankNum : -1);
        return NULL;
    }
    for (i = 0; i < 4; i++) {
        for (j = 0; j < orig->bank[i].timeNum; j++) {
            ANIMPAT *op = &orig->pat[orig->bank[i].frame[j].pat];
            s32 k;
            for (k = 0; k < op->layerNum; k++) {
                if (op->layer[k].bmpNo != 0) {
                    EightLog("WARNING: pause menu: 5P-8P tags not made: the game's 1P-4P tag uses bitmap %d; ports 5-8 "
                             "show the 1P-4P tags",
                        (int)op->layer[k].bmpNo);
                    return NULL;
                }
            }
        }
        frameNum += orig->bank[i].timeNum;
    }
    for (i = 0; i < orig->patNum; i++) {
        layerNum += orig->pat[i].layerNum;
    }
    anim = HuMemDirectMalloc(HEAP_DATA, sizeof(ANIMDATA));
    bank = HuMemDirectMalloc(HEAP_DATA, 4 * sizeof(ANIMBANK) + frameNum * sizeof(ANIMFRAME));
    pat = HuMemDirectMalloc(HEAP_DATA, orig->patNum * sizeof(ANIMPAT) + layerNum * sizeof(ANIMLAYER));
    bmp = HuMemDirectMalloc(HEAP_DATA, sizeof(ANIMBMP));
    frame = (ANIMFRAME *)(bank + 4);
    layer = (ANIMLAYER *)(pat + orig->patNum);
    *anim = *orig;
    anim->bankNum = 4;
    anim->bmpNum = 1 | ANIM_BMP_ALLOC;
    anim->useNum = 0;
    anim->bank = bank;
    anim->pat = pat;
    anim->bmp = bmp;
    for (i = 0; i < 4; i++) {
        bank[i] = orig->bank[i];
        bank[i].frame = frame;
        for (j = 0; j < orig->bank[i].timeNum; j++) {
            *frame++ = orig->bank[i].frame[j];
        }
    }
    for (i = 0; i < orig->patNum; i++) {
        pat[i] = orig->pat[i];
        pat[i].layer = layer;
        for (j = 0; j < orig->pat[i].layerNum; j++) {
            *layer = orig->pat[i].layer[j];
            if (layer->bmpNo != 0) {
                layer->bmpNo = 0; /* only the COM banks use the other bitmap; never shown */
                layer->alpha = 0;
            }
            layer++;
        }
    }
    memset(bmp, 0, sizeof(ANIMBMP));
    bmp->pixSize = 32;
    bmp->dataFmt = ANIM_BMP_RGBA8;
    bmp->palNum = 0;
    bmp->sizeX = 200;
    bmp->sizeY = 64;
    bmp->dataSize = sizeof(pause8TagTex);
    bmp->data = HuMemDirectMalloc(HEAP_DATA, sizeof(pause8TagTex));
    memcpy(bmp->data, pause8TagTex, sizeof(pause8TagTex));
#ifdef OPTIMIZED_TEXTURE_LOADING
    for (i = 0; i < (s32)(sizeof(bmp->texData) / sizeof(bmp->texData[0])); i++) {
        bmp->texData[i].wrap_s = bmp->texData[i].wrap_t = GX_CLAMP;
    }
#endif
    return anim;
}

static s32 boardLogoTbl[] = { DATA_MAKE_NUM(DATADIR_BOARD, 87), DATA_MAKE_NUM(DATADIR_BOARD, 88), DATA_MAKE_NUM(DATADIR_BOARD, 89),
    DATA_MAKE_NUM(DATADIR_BOARD, 90), DATA_MAKE_NUM(DATADIR_BOARD, 91), DATA_MAKE_NUM(DATADIR_BOARD, 92), DATA_MAKE_NUM(DATADIR_BOARD, 87),
    DATA_MAKE_NUM(DATADIR_BOARD, 93), DATA_MAKE_NUM(DATADIR_BOARD, 94) };

static float turnDigitPosTbl[] = { 145.0f, 188.0f, 285.0f, 328.0f };

static s32 boxMdlTbl[] = { DATA_MAKE_NUM(DATADIR_BPAUSE, 8), DATA_MAKE_NUM(DATADIR_BPAUSE, 9), DATA_MAKE_NUM(DATADIR_BPAUSE, 10),
    DATA_MAKE_NUM(DATADIR_BPAUSE, 11), DATA_MAKE_NUM(DATADIR_BPAUSE, 12), DATA_MAKE_NUM(DATADIR_BPAUSE, 13), DATA_MAKE_NUM(DATADIR_BPAUSE, 14),
    DATA_MAKE_NUM(DATADIR_BPAUSE, 15) };

void BoardPauseStart(void)
{
    mainProcess = HuPrcChildCreate(PauseProcess, 0x2001, 0x3800, 0, boardMainProc);
    hostMdl = BoardStarHostMdlGet();
    BoardModelMotionStart(hostMdl, 1, 0x40000001);
    HuPrcVSleep();
    omSysPauseCtrl(1);
    omSysPauseEnable(1);
    HuPrcSetStat(mainProcess, 0xC);
    HuPrcDestructorSet2(mainProcess, PauseExit);
}

static void PauseExit(void)
{
    if (pauseQuitF == 0) {
        BoardRollWinDispSet(1);
        BoardRollDispSet(1);
        BoardStatusItemSet(1);
        BoardRollUPauseSet(1);
        BoardLast5GfxShowSet(1);
        HuAudFXPauseAll(0);
        HuAudSeqPauseAll(0);
    }
    else {
        BoardConfettiKill();
        BoardAudSeqFadeOutAll();
        HuAudAllStop();
    }
    DeletePauseScreen();
    DeletePauseControlWin();
    BoardEventFlagReset();
    HuDataDirClose(DATADIR_BPAUSE);
    pausePad = -1;
    omSysPauseCtrl(0);
    if (GWMGExplainGet()) {
        _SetFlag(FLAG_ID_MAKE(0, 11));
    }
    else {
        _ClearFlag(FLAG_ID_MAKE(0, 11));
    }
    if (GWPartyGet() == 0) {
        GWGameStat.story_pause.explain_mg = GWMGExplainGet();
        GWGameStat.story_pause.show_com_mg = GWMGShowComGet();
        GWGameStat.story_pause.mg_list = GWMGListGet();
        GWGameStat.story_pause.mess_speed = GWMessSpeedGet();
        GWGameStat.story_pause.save_mode = GWSaveModeGet();
    }
    else {
        GWGameStat.party_pause.explain_mg = GWMGExplainGet();
        GWGameStat.party_pause.show_com_mg = GWMGShowComGet();
        GWGameStat.party_pause.mg_list = GWMGListGet();
        GWGameStat.party_pause.mess_speed = GWMessSpeedGet();
        GWGameStat.party_pause.save_mode = GWSaveModeGet();
    }
    mainProcess = NULL;
}

static void PauseProcess(void)
{
    s32 temp_r31;

    pauseQuitF = 0;
    mainScreenF = 1;
    BoardFilterFadeInit(30, 0xA0);
    temp_r31 = BoardDataDirReadAsync(DATADIR_BPAUSE);
    BoardRollWinDispSet(0);
    BoardRollDispSet(0);
    BoardStatusItemSet(0);
    BoardRollUPauseSet(0);
    BoardLast5GfxShowSet(0);
    HuPadRumbleAllStop();
    HuPrcSleep(1);
    HuAudFXPlay(5);
    while (!BoardFilterFadePauseCheck()) {
        HuPrcVSleep();
    }
    BoardDataAsyncWait(temp_r31);
    CreatePauseScreen();
    CreatePauseControlWin();
    if (WaitPauseInput() == 1) {
        ShowPauseConfig();
    }
    DeletePauseScreen();
    DeletePauseControlWin();
    if (pauseQuitF != 0) {
        HuPrcSleep(20);
        WipeColorSet(0, 0, 0);
        BoardKill();
    }
    else {
        BoardFilterFadeOut(30);
        HuPrcSleep(30);
    }
    HuPrcEnd();
}

void CreatePauseScreen(void)
{
    Mtx sp30;
    Vec sp24;
    Vec sp18;
    float temp_f30;
    float var_f31;
    void *sp14;
    omObjData *temp_r27;
    ConfigWork *temp_r29;
    s16 sp8;
    s32 var_r28;
    s32 i;

    sp14 = boardObjMan->user_data;
    temp_r27 = omAddObjEx(boardObjMan, 0x108, 0, 0, -1, PauseConfigObjFunc);
    configObj = temp_r27;
    temp_r29 = OM_GET_WORK_PTR(temp_r27, ConfigWork);
    temp_r29->unk00_field0 = 0;
    temp_r29->unk02 = 0;
    temp_r29->unk03 = 0;
    temp_r29->unk06 = 0;
    temp_r29->unk01 = 0;
    temp_r29->unk00_field1 = 0;
    temp_r29->unk00_field2 = 0;
    temp_r29->unk0A = 0;
    temp_r29->unk00_field4 = (SLSaveFlagGet() != 0) ? 0 : 1;
    temp_r29->unk09 = 0;
    temp_r29->unk0C = 0;
    cursorPos = temp_r29->unk02 + temp_r29->unk03 * 4;
    temp_r27->trans.x = -40.0f + boxPosTbl[cursorPos][0];
    temp_r27->trans.y = -32.0f + boxPosTbl[cursorPos][1];
    pauseCursorPos = HuSprGrpCreate(2);
    HuSprGrpPosSet(pauseCursorPos, 0.0f, 0.0f);
    BoardSpriteCreate(DATA_MAKE_NUM(DATADIR_BOARD, 85), 700, 0, &sp8);
    HuSprGrpMemberSet(pauseCursorPos, 0, sp8);
    HuSprZRotSet(pauseCursorPos, 0, 45.0f);
    HuSprAttrSet(pauseCursorPos, 0, 4);
    HuSprAttrSet(pauseCursorPos, 0, 8);
    HuSprPosSet(pauseCursorPos, 0, temp_r27->trans.x, temp_r27->trans.y);
    BoardSpriteCreate(DATA_MAKE_NUM(DATADIR_BPAUSE, 7), 750, 0, &sp8);
    HuSprGrpMemberSet(pauseCursorPos, 1, sp8);
    HuSprAttrSet(pauseCursorPos, 1, 4);
    HuSprAttrSet(pauseCursorPos, 1, 8);
    pauseSprGrp = HuSprGrpCreate(7);
    HuSprGrpPosSet(pauseSprGrp, 0.0f, 0.0f);
    BoardSpriteCreate(boardLogoTbl[GWBoardGet()], 900, 0, &sp8);
    HuSprGrpMemberSet(pauseSprGrp, 0, sp8);
    HuSprPosSet(pauseSprGrp, 0, 288.0f, 120.0f);
    HuSprAttrSet(pauseSprGrp, 0, 4);
    HuSprAttrSet(pauseSprGrp, 0, 8);
    BoardSpriteCreate(DATA_MAKE_NUM(DATADIR_BPAUSE, 5), 900, 0, &sp8);
    HuSprGrpMemberSet(pauseSprGrp, 1, sp8);
    HuSprPosSet(pauseSprGrp, 1, 288.0f, 302.0f);
    HuSprAttrSet(pauseSprGrp, 1, 8);
    BoardSpriteCreate(DATA_MAKE_NUM(DATADIR_BPAUSE, 4), 800, 0, &sp8);
    HuSprGrpMemberSet(pauseSprGrp, 2, sp8);
    HuSprZRotSet(pauseSprGrp, 2, -30.0f);
    HuSprPosSet(pauseSprGrp, 2, 87.0f, 245.0f);
    HuSprAttrSet(pauseSprGrp, 2, 8);
    if (GWBoardGet() == 7 || GWBoardGet() == 8) {
        HuSprAttrSet(pauseSprGrp, 2, 4);
    }
    if (GWPartyGet() == 1) {
        if (GWTeamGet()) {
            HuSprBankSet(pauseSprGrp, 2, 2);
        }
        else {
            HuSprBankSet(pauseSprGrp, 2, 0);
        }
    }
    else {
        HuSprBankSet(pauseSprGrp, 2, 1);
    }
    for (i = 0; i < 4; i++) {
        BoardSpriteCreate(DATA_MAKE_NUM(DATADIR_BPAUSE, 6), 800, 0, &sp8);
        HuSprGrpMemberSet(pauseSprGrp, i + 3, sp8);
        HuSprPosSet(pauseSprGrp, i + 3, turnDigitPosTbl[i], 301.0f);
        HuSprAttrSet(pauseSprGrp, i + 3, 8);
        HuSprAttrSet(pauseSprGrp, i + 3, 1);
    }
    pause8F = GWPlayerCount() == 8;
    if (pause8F) {
        Pad8LayoutGet();
    }
    padConfigSprGrp = HuSprGrpCreate(pause8F ? PAD8_MEMBERS : 13);
    HuSprGrpPosSet(padConfigSprGrp, 0.0f, 0.0f);
    for (i = 0; i < PadNum(); i++) {
        BoardSpriteCreate(DATA_MAKE_NUM(DATADIR_BPAUSE, 0), 910, 0, &sp8);
        HuSprGrpMemberSet(padConfigSprGrp, PadFace(i), sp8);
        HuSprAttrSet(padConfigSprGrp, PadFace(i), 8);
        HuSprAttrSet(padConfigSprGrp, PadFace(i), 1);
        BoardSpriteCreate(DATA_MAKE_NUM(DATADIR_BPAUSE, 1), 700, 0, &sp8);
        HuSprGrpMemberSet(padConfigSprGrp, PadLabel(i), sp8);
        HuSprAttrSet(padConfigSprGrp, PadLabel(i), 8);
        HuSprAttrSet(padConfigSprGrp, PadLabel(i), 1);
        BoardSpriteCreate(DATA_MAKE_NUM(DATADIR_BPAUSE, 2), 800, 0, &sp8);
        HuSprGrpMemberSet(padConfigSprGrp, PadFrame(i), sp8);
        HuSprAttrSet(padConfigSprGrp, PadFrame(i), 8);
        HuSprAttrSet(padConfigSprGrp, PadFrame(i), 1);
    }
    BoardSpriteCreate(DATA_MAKE_NUM(DATADIR_BPAUSE, 3), 1000, 0, &sp8);
    HuSprGrpMemberSet(padConfigSprGrp, PadPanel(), sp8);
    HuSprAttrSet(padConfigSprGrp, PadPanel(), 4);
    HuSprDrawNoSet(padConfigSprGrp, PadPanel(), 0x40);
    if (pause8F) {
        s16 labelSpr = HuSprGrpData[padConfigSprGrp].members[PadLabel(0)];
        ANIMDATA *tagAnim = Pad8TagAnim((ANIMDATA *)HuSprData[labelSpr].data);
        for (i = 4; i < 8; i++) {
            if (tagAnim != NULL) {
                sp8 = HuSprCreate(tagAnim, 700, 0);
            }
            else {
                BoardSpriteCreate(DATA_MAKE_NUM(DATADIR_BPAUSE, 1), 700, 0, &sp8);
            }
            HuSprGrpMemberSet(padConfigSprGrp, PadTag8(i), sp8);
            HuSprAttrSet(padConfigSprGrp, PadTag8(i), 8);
            HuSprAttrSet(padConfigSprGrp, PadTag8(i), 1);
            HuSprAttrSet(padConfigSprGrp, PadTag8(i), HUSPR_ATTR_DISPOFF);
        }
        for (i = 0; i < 8; i++) {
            HuSprScaleSet(padConfigSprGrp, PadFace(i), pad8Scale, pad8Scale);
            HuSprScaleSet(padConfigSprGrp, PadLabel(i), pad8Scale, pad8Scale);
            HuSprScaleSet(padConfigSprGrp, PadFrame(i), pad8Scale, pad8Scale);
            if (i >= 4) {
                HuSprScaleSet(padConfigSprGrp, PadTag8(i), pad8Scale, pad8Scale);
            }
        }
        HuSprScaleSet(padConfigSprGrp, PadPanel(), 1.0f, pad8PanelScaleY);
    }
    CreatePadConfigSprite();
    ShowPadConfigSprite(0);
    hostOldLayer = BoardModelLayerGet(hostMdl);
    BoardModelLayerSet(hostMdl, 6);
    sp24.x = 68.0f;
    if (GWBoardGet() == 3) {
        sp24.y = 464.0f;
    }
    else {
        sp24.y = 434.0f;
    }
    sp24.z = 1100.0f;
    Hu3D2Dto3D(&sp24, 1, &sp18);
    RotateBox(0.0f, 15.0f, sp30);
    BoardModelMtxSet(hostMdl, &sp30);
    BoardModelPosSetV(hostMdl, &sp18);
    BoardModelVisibilitySet(hostMdl, 0);
    BoardModelRotSet(hostMdl, 0.0f, 0.0f, 0.0f);
    for (i = 0; i < 8; i++) {
        boxModelID[i] = BoardModelCreate(boxMdlTbl[i], NULL, 0);
        sp24.x = boxPosTbl[i][0];
        sp24.y = boxPosTbl[i][1];
        sp24.z = 700.0f;
        Hu3D2Dto3D(&sp24, 1, &sp18);
        temp_f30 = 0.5f;
        BoardModelPosSetV(boxModelID[i], &sp18);
        BoardModelLayerSet(boxModelID[i], 6);
        BoardModelScaleSet(boxModelID[i], temp_f30, temp_f30, temp_f30);
        switch (i) {
            case 0:
            case 7:
                var_r28 = 0;
                var_f31 = 0.0f;
                break;
            case 1:
                var_r28 = GWMGExplainGet();
                var_f31 = (var_r28 != 0) ? 0.0f : -90.0f;
                break;
            case 2:
                var_r28 = GWMGShowComGet();
                var_f31 = (var_r28 != 0) ? 0.0f : -90.0f;
                break;
            case 3:
                var_r28 = GWMGListGet();
                switch (var_r28) {
                    case 1:
                        var_r28 = 0;
                        var_f31 = 90.0f;
                        break;
                    case 0:
                        var_r28 = 1;
                        var_f31 = 0.0f;
                        break;
                    case 2:
                        var_r28 = 2;
                        var_f31 = -90.0f;
                        break;
                }
                break;
            case 4:
                var_r28 = GWRumbleGet();
                var_f31 = (var_r28 != 0) ? 0.0f : -90.0f;
                break;
            case 5:
                var_r28 = GWMessSpeedGet();
                switch (var_r28) {
                    case 2:
                        var_r28 = 0;
                        var_f31 = 90.0f;
                        break;
                    case 1:
                        var_r28 = 1;
                        var_f31 = 0.0f;
                        break;
                    case 0:
                        var_r28 = 2;
                        var_f31 = -90.0f;
                        break;
                }
                break;
            case 6:
                if (temp_r29->unk00_field4 != 0) {
                    GWSystem.save_mode = 1;
                }
                var_r28 = GWSaveModeGet();
                switch (var_r28) {
                    case 1:
                        var_r28 = 0;
                        var_f31 = 90.0f;
                        break;
                    case 0:
                        var_r28 = 1;
                        var_f31 = 0.0f;
                        break;
                    case 2:
                        var_r28 = 2;
                        var_f31 = -90.0f;
                        break;
                }
                break;
        }
        boxState[i] = var_r28;
        boxRot[i] = var_f31;
        RotateBox(3.5f, var_f31, sp30);
        BoardModelMtxSet(boxModelID[i], &sp30);
        BoardModelRotSet(boxModelID[i], 0.0f, 0.0f, 0.0f);
    }
    SetBoxVisible(0);
}

static void DeletePauseScreen(void)
{
    ConfigWork *temp_r30;
    Mtx sp8;
    s32 i;

    if (configObj) {
        temp_r30 = OM_GET_WORK_PTR(configObj, ConfigWork);
        temp_r30->unk00_field0 = 1;
    }
    for (i = 0; i < 8; i++) {
        BoardModelKill(boxModelID[i]);
        boxModelID[i] = -1;
    }
    if (pauseSprGrp != -1) {
        HuSprGrpKill(pauseSprGrp);
        pauseSprGrp = -1;
    }
    if (padConfigSprGrp != -1) {
        HuSprGrpKill(padConfigSprGrp);
        padConfigSprGrp = -1;
    }
    if (pauseCursorPos != -1) {
        HuSprGrpKill(pauseCursorPos);
        pauseCursorPos = -1;
    }
    MTXIdentity(sp8);
    BoardModelLayerSet(hostMdl, hostOldLayer);
    BoardModelVisibilitySet(hostMdl, 1);
    BoardModelMtxSet(hostMdl, &sp8);
    BoardStarHostSet(hostMdl);
    if (GWBoardGet() == 7 || GWBoardGet() == 8) {
        BoardModelVisibilitySet(hostMdl, 0);
    }
}

static void SetBoxVisible(s32 arg0)
{
    s32 i;

    for (i = 0; i < 8; i++) {
        BoardModelVisibilitySet(boxModelID[i], arg0);
    }
}

static void PauseConfigObjFunc(omObjData *arg0)
{
    ConfigWork *temp_r31;
    s16 temp_r28;
    s32 var_r29;

    temp_r31 = OM_GET_WORK_PTR(arg0, ConfigWork);
    if (temp_r31->unk00_field0 != 0 || BoardIsKill()) {
        configObj = NULL;
        omDelObjEx(HuPrcCurrentGet(), arg0);
        return;
    }
    if (mainScreenF == 0) {
        if (temp_r31->unk06 != 0) {
            temp_r31->unk06--;
            return;
        }
        cursorPos = temp_r31->unk02 + temp_r31->unk03 * 4;
        temp_r28 = cursorPos;
        if (temp_r31->unk00_field1 != 0) {
            switch (cursorPos) {
                case 0:
                    if (UpdatePadConfig(arg0, temp_r31) != 0) {
                        UpdatePauseText(cursorPos);
                    }
                    break;
                case 1:
                    if (UpdatePauseBox(arg0, temp_r31, 1) != 0) {
                        GWMGExplainSet(boxState[1]);
                    }
                    UpdatePauseText(cursorPos);
                    break;
                case 2:
                    if (UpdatePauseBox(arg0, temp_r31, 2) != 0) {
                        GWMGShowComSet(boxState[2]);
                    }
                    UpdatePauseText(cursorPos);
                    break;
                case 3:
                    if (UpdatePauseBoxExt(arg0, temp_r31, 3) != 0) {
                        switch (boxState[3]) {
                            case 0:
                                GWMGListSet(1);
                                break;
                            case 1:
                                GWMGListSet(0);
                                break;
                            case 2:
                                GWMGListSet(2);
                                break;
                        }
                    }
                    UpdatePauseText(cursorPos);
                    break;
                case 4:
                    if (UpdatePauseBox(arg0, temp_r31, 4) != 0) {
                        GWRumbleSet(boxState[4]);
                    }
                    UpdatePauseText(cursorPos);
                    break;
                case 5:
                    if (UpdatePauseBoxExt(arg0, temp_r31, 5) != 0) {
                        switch (boxState[5]) {
                            case 0:
                                GWMessSpeedSet(2);
                                break;
                            case 1:
                                GWMessSpeedSet(1);
                                break;
                            case 2:
                                GWMessSpeedSet(0);
                                break;
                        }
                    }
                    UpdatePauseText(cursorPos);
                    break;
                case 6:
                    if (temp_r31->unk00_field4 != 0) {
                        temp_r31->unk00_field1 = 0;
                        break;
                    }
                    if (UpdatePauseBoxExt(arg0, temp_r31, 6) != 0) {
                        switch (boxState[6]) {
                            case 0:
                                GWSaveModeSet(1);
                                break;
                            case 1:
                                GWSaveModeSet(0);
                                break;
                            case 2:
                                GWSaveModeSet(2);
                                break;
                        }
                    }
                    UpdatePauseText(cursorPos);
                    break;
                case 7:
                    InitPauseQuit(arg0, temp_r31);
                    break;
            }
        }
        else {
            cursorPos = ExecPauseConfig(arg0, temp_r31);
            if (temp_r28 != cursorPos) {
                arg0->trans.x = -40.0f + boxPosTbl[cursorPos][0];
                arg0->trans.y = -32.0f + boxPosTbl[cursorPos][1];
            }
            UpdatePauseText(cursorPos);
            HuSprPosSet(pauseCursorPos, 0, arg0->trans.x, arg0->trans.y);
        }
    }
}

#ifdef TARGET_PC
/* "Current Settings:" then "1P HMN   2P COM   3P COM   4P COM" and the same
   for 5P-8P, in the game's message encoding (0x0B start, 0x10 space, 0x7B the
   font's colon as in MAKE_MESSID(16, 28), 0x0A new line, 0x1F n = insert slot
   n-1, here the port's HMN/COM). */
static const u8 *Pause8SettingsText(void)
{
    static u8 text[96];
    static const char head[] = "Current Settings";
    u8 *out = text;
    s32 i;
    const char *c;

    *out++ = 0x0B;
    for (c = head; *c; c++) {
        *out++ = *c == ' ' ? 0x10 : (u8)*c;
    }
    *out++ = 0x7B;
    for (i = 0; i < 8; i++) {
        if ((i & 3) == 0) {
            *out++ = 0x0A;
        }
        else {
            *out++ = 0x10;
            *out++ = 0x10;
            *out++ = 0x10;
        }
        *out++ = (u8)('1' + i);
        *out++ = 'P';
        *out++ = 0x10;
        *out++ = 0x1F;
        *out++ = (u8)(i + 1);
    }
    *out = 0;
    return text;
}
#endif

static void UpdatePauseText(s32 arg0)
{
    s32 var_r28;
    s32 j;
    s32 i;

    switch (arg0) {
        case 0:
#ifdef TARGET_PC
            if (pause8F) {
                /* 8-player party: all 8 ports, four to a line (new text in
                   the style of MAKE_MESSID(16, 28)). */
                HuWinMesSetPtr(settingsWin, (uintptr_t)Pause8SettingsText());
            }
            else
#endif
            HuWinMesSet(settingsWin, MAKE_MESSID(16, 28));
            /* In the 8-player party the port's player can be in any of the 8 slots. */
            for (i = 0; i < PadNum(); i++) {
                for (j = 0; j < GWPlayerCount(); j++) {
                    if (i == GWPlayer[j].port) {
                        if (GWPlayer[j].com) {
                            var_r28 = MAKE_MESSID(16, 30);
                        }
                        else {
                            var_r28 = MAKE_MESSID(16, 29);
                        }
                        HuWinInsertMesSet(settingsWin, var_r28, i);
                        break;
                    }
                }
            }
            break;
        case 1:
            if (boxState[arg0] != 0) {
                HuWinMesSet(settingsWin, MAKE_MESSID(16, 33));
            }
            else {
                HuWinMesSet(settingsWin, MAKE_MESSID(16, 34));
            }
            break;
        case 2:
            if (boxState[arg0] != 0) {
                HuWinMesSet(settingsWin, MAKE_MESSID(16, 35));
            }
            else {
                HuWinMesSet(settingsWin, MAKE_MESSID(16, 36));
            }
            break;
        case 3:
            switch (boxState[arg0]) {
                case 0:
                    HuWinMesSet(settingsWin, MAKE_MESSID(16, 38));
                    break;
                case 1:
                    HuWinMesSet(settingsWin, MAKE_MESSID(16, 37));
                    break;
                case 2:
                    HuWinMesSet(settingsWin, MAKE_MESSID(16, 39));
                    break;
            }
            break;
        case 4:
            if (boxState[arg0] != 0) {
                HuWinMesSet(settingsWin, MAKE_MESSID(16, 40));
            }
            else {
                HuWinMesSet(settingsWin, MAKE_MESSID(16, 41));
            }
            break;
        case 5:
            switch (boxState[arg0]) {
                case 0:
                    HuWinMesSet(settingsWin, MAKE_MESSID(16, 44));
                    break;
                case 1:
                    HuWinMesSet(settingsWin, MAKE_MESSID(16, 43));
                    break;
                case 2:
                    HuWinMesSet(settingsWin, MAKE_MESSID(16, 42));
                    break;
            }
            break;
        case 6:
            switch (boxState[arg0]) {
                case 0:
                    HuWinMesSet(settingsWin, MAKE_MESSID(16, 47));
                    break;
                case 1:
                    HuWinMesSet(settingsWin, MAKE_MESSID(16, 45));
                    break;
                case 2:
                    HuWinMesSet(settingsWin, MAKE_MESSID(16, 46));
                    break;
            }
            break;
        case 7:
            HuWinMesSet(settingsWin, MAKE_MESSID(16, 48));
            break;
    }
}

static void InitPauseQuit(omObjData *arg0, ConfigWork *arg1)
{
    float var_f31;

    switch (arg1->unk01) {
        case 0:
            SetBoxVisible(0);
            quitProcess = HuPrcChildCreate(PauseQuitProcess, 0x2001, 0x3800, 0, boardMainProc);
            HuPrcSetStat(quitProcess, 0xC);
            HuPrcDestructorSet2(quitProcess, DeletePauseQuit);
            arg1->unk01 = 1;
            arg1->unk06 = 10;
            HuSprAttrSet(pauseCursorPos, 0, 4);
            HuSprAttrSet(pauseCursorPos, 1, 4);
            break;
        case 1:
            if (quitWin != -1 && HuWinStatGet(quitWin) == 3) {
                if (HuWinChoiceNowGet(quitWin) != 0) {
                    HuWinMesSet(settingsWin, MAKE_MESSID(16, 51));
                }
                else {
                    HuWinMesSet(settingsWin, MAKE_MESSID(16, 50));
                }
            }
            if (!quitProcess) {
                arg1->unk01 = 2;
                if (pauseQuitF != 0) {
                    arg1->unk00_field0 = 1;
                    break;
                }
            }
            break;
        case 2:
            arg1->unk00_field1 = 0;
            arg1->unk01 = 0;
            arg1->unk04 = 0;
            arg1->unk05 = 0;
            arg1->unk06 = 0;
            SetBoxVisible(1);
            var_f31 = 0.5f;
            BoardModelScaleSet(boxModelID[7], var_f31, var_f31, var_f31);
            HuSprAttrReset(pauseCursorPos, 0, HUSPR_ATTR_DISPOFF);
            if (GWPartyGet() == 0) {
                HuSprAttrReset(pauseCursorPos, 1, HUSPR_ATTR_DISPOFF);
            }
            break;
    }
}

static void PauseQuitProcess(void)
{
    WindowData *temp_r31;
    float sp10[2];
    float sp8[2];

    HuWinMesMaxSizeGet(1, sp8, MAKE_MESSID(16, 49));
    sp10[0] = -10000.0f;
    sp10[1] = 140.0f;
    quitWin = HuWinExCreate(sp10[0], sp10[1], PB_F2S16(sp8[0]), PB_F2S16(sp8[1]), -1);
    temp_r31 = &winData[quitWin];
    temp_r31->active_pad = (1 << pausePad);
    HuWinExAnimIn(quitWin);
    HuWinMesSpeedSet(quitWin, 0);
    HuWinMesSet(quitWin, MAKE_MESSID(16, 49));
    HuWinMesWait(quitWin);
    if (HuWinChoiceGet(quitWin, 0) == 1) {
        HuPrcSleep(60);
        pauseQuitF = 1;
        _SetFlag(0x1001B);
    }
    HuPrcEnd();
}

static void DeletePauseQuit(void)
{
    if (quitWin != -1) {
        HuWinExCleanup(quitWin);
        quitWin = -1;
    }
    HuDataDirClose(DATADIR_BPAUSE);
    quitProcess = NULL;
}

static s32 UpdatePadConfig(omObjData *arg0, ConfigWork *arg1)
{
    switch (arg1->unk01) {
        case 0:
            CreatePadConfig(arg0, arg1);
            break;
        case 1:
            ScrollInPadConfig(arg0, arg1);
            break;
        case 2:
            CursorMovePadConfig(arg0, arg1);
            break;
        case 3:
            ChangeDiffPadConfig(arg0, arg1);
            break;
        case 4:
            ScrollOutPadConfig(arg0, arg1);
            break;
    }
    if (arg1->unk00_field1 != 0) {
        return 0;
    }
    else {
        return 1;
    }
}

static void CreatePadConfig(omObjData *arg0, ConfigWork *arg1)
{
    Vec sp14;
    Vec sp8;
    s32 i;

    HuSprAttrSet(pauseCursorPos, 0, 4);
    for (i = 0; i < 8; i++) {
        if (i != 0) {
            BoardModelVisibilitySet(boxModelID[i], 0);
        }
    }
    arg0->rot.x = 576.0f;
    arg0->rot.y = 112.0f;
    arg0->rot.z = (32.0f - arg0->rot.x) / 20.0f;
    arg1->unk07 = 20;
    ShowPadConfigSprite(1);
    HuSprGrpPosSet(padConfigSprGrp, arg0->rot.x, arg0->rot.y);
    arg1->unk04 = 0;
    arg1->unk05 = 0;
    PadCursorPos(arg1->unk04, &sp14.x, &sp14.y);
    HuSprPosSet(pauseCursorPos, 0, sp14.x, sp14.y);
    HuSprZRotSet(pauseCursorPos, 0, 0.0f);
    padBoxPos.x = boxPosTbl[0][0];
    padBoxPos.y = boxPosTbl[0][1];
    padBoxPos.z = 700.0f;
    sp14.x = 96.0f;
    sp14.y = 176.0f;
    sp14.z = padBoxPos.z;
    VECSubtract(&sp14, &padBoxPos, &sp8);
    arg0->scale.x = sp8.x / 20.0f;
    arg0->scale.y = sp8.y / 20.0f;
    arg1->unk01 = 1;
}

static void ScrollInPadConfig(omObjData *arg0, ConfigWork *arg1)
{
    Vec sp8;
    s32 i;

    if (arg1->unk07 == 0) {
        HuSprAttrReset(pauseCursorPos, 0, HUSPR_ATTR_DISPOFF);
        if (arg1->unk00_field2 != 0) {
            for (i = 0; i < 8; i++) {
                if (i != 0) {
                    BoardModelVisibilitySet(boxModelID[i], 1);
                }
            }
            arg1->unk00_field1 = 0;
            arg1->unk00_field2 = 0;
            arg1->unk04 = 0;
            arg1->unk05 = 0;
            arg1->unk01 = 0;
        }
        else {
            arg1->unk01 = 2;
        }
        return;
    }
    arg0->rot.x += arg0->rot.z;
    HuSprGrpPosSet(padConfigSprGrp, arg0->rot.x, arg0->rot.y);
    padBoxPos.x += arg0->scale.x;
    padBoxPos.y += arg0->scale.y;
    Hu3D2Dto3D(&padBoxPos, 1, &sp8);
    BoardModelPosSetV(boxModelID[0], &sp8);
    arg1->unk07--;
}

static void CursorMovePadConfig(omObjData *arg0, ConfigWork *arg1)
{
    float sp8[4];
    s32 temp_r29;
    u32 temp_r30;

    HuWinMesSet(settingsWin, MAKE_MESSID(16, 31));
    temp_r30 = HuPadDStkRep[pausePad] | HuPadBtnDown[pausePad];
    temp_r29 = CheckPort(arg1->unk04);
    if (temp_r30 == 0x100) {
        arg1->unk01 = 3;
        if (GWPlayer[temp_r29].com) {
            arg1->unk00_field3 = GWPlayer[temp_r29].diff + 1;
            arg1->unk05 = GWPlayer[temp_r29].diff + 1;
        }
        else {
            arg1->unk00_field3 = 0;
            arg1->unk05 = 0;
        }
        HuSprBankSet(padConfigSprGrp, PadFrame(arg1->unk04), 1);
        HuAudFXPlay(2);
    }
    else if (temp_r30 == 0x200) {
        HuAudFXPlay(3);
        arg1->unk01 = 4;
    }
    else {
        if (pause8F) {
            /* Two rows of four: left/right along the row (and on into the
               next row, in port order), up/down between the rows. */
            s32 old = arg1->unk04;
            if (temp_r30 == 1 && arg1->unk04 > 0) {
                arg1->unk04--;
            }
            if (temp_r30 == 2 && arg1->unk04 < 7) {
                arg1->unk04++;
            }
            if (temp_r30 == 8 && arg1->unk04 >= 4) {
                arg1->unk04 -= 4;
            }
            if (temp_r30 == 4 && arg1->unk04 < 4) {
                arg1->unk04 += 4;
            }
            if (arg1->unk04 != old) {
                HuAudFXPlay(0);
                arg1->unk06 = 4;
            }
        }
        else {
        if (temp_r30 == 1) {
            arg1->unk04--;
        }
        if (temp_r30 == 2) {
            arg1->unk04++;
        }
        if (arg1->unk04 < 0) {
            arg1->unk04 = 0;
        }
        else if (arg1->unk04 >= 4) {
            arg1->unk04 = 3;
        }
        else if (temp_r30 == 1 || temp_r30 == 2) {
            HuAudFXPlay(0);
            arg1->unk06 = 4;
        }
        }
        PadCursorPos(arg1->unk04, &sp8[0], &sp8[1]);
        HuSprPosSet(pauseCursorPos, 0, sp8[0], sp8[1]);
    }
}

static void ChangeDiffPadConfig(omObjData *arg0, ConfigWork *arg1)
{
    s32 temp_r29;
    s32 var_r28;
    s32 var_r26;
    s32 var_r27;
    u32 var_r30;

    var_r30 = 0;
    if (HuPadStkX[pausePad] < -20) {
        var_r30 |= 1;
    }
    else if (HuPadStkX[pausePad] > 20) {
        var_r30 |= 2;
    }
    temp_r29 = CheckPort(arg1->unk04);
    if (GWGameStat.veryHardUnlock != 0) {
        var_r27 = 4;
    }
    else {
        var_r27 = 3;
    }
    HuWinMesSet(settingsWin, MAKE_MESSID(16, 32));
    if (var_r30 != 0) {
        if (arg1->unk0C != 0 && arg1->unk0C == var_r30) {
            if (arg1->unk09++ < 10) {
                return;
            }
        }
        else {
            if (arg1->unk09++ < 2) {
                return;
            }
            arg1->unk0C = var_r30;
        }
    }
    else {
        arg1->unk0C = 0;
    }
    arg1->unk09 = 0;
    var_r28 = 0;
    if (var_r30 == 1) {
        var_r28 = -1;
    }
    if (var_r30 == 2) {
        var_r28 = 1;
    }
    if (var_r28 != 0) {
        HuAudFXPlay(0);
        arg1->unk06 = 4;
    }
    else {
        if (HuPadBtnDown[pausePad] == 0x100) {
            arg1->unk01 = 2;
            if (arg1->unk05 != 0) {
                GWPlayer[temp_r29].com = 1;
                GWPlayerCfg[temp_r29].iscom = 1;
                GWPlayer[temp_r29].diff = arg1->unk05 - 1;
                GWPlayerCfg[temp_r29].diff = arg1->unk05 - 1;
            }
            else {
                GWPlayer[temp_r29].com = 0;
                GWPlayerCfg[temp_r29].iscom = 0;
            }
            HuSprBankSet(padConfigSprGrp, PadFrame(arg1->unk04), 0);
            HuAudFXPlay(2);
            arg1->unk0C = 0x100;
            if (pause8F) {
                EightLog("pause menu: port %d %s set to %s", (int)arg1->unk04 + 1, EightLogName(temp_r29),
                    GWPlayer[temp_r29].com ? (const char *[]) { "COM easy", "COM normal", "COM hard", "COM very hard" }[GWPlayer[temp_r29].diff & 3]
                                           : "person");
            }
        }
        if (HuPadBtnDown[pausePad] == 0x200) {
            arg1->unk01 = 2;
            arg1->unk05 = arg1->unk00_field3;
            HuSprBankSet(padConfigSprGrp, PadFrame(arg1->unk04), 0);
            HuAudFXPlay(3);
            arg1->unk0C = 0x200;
        }
        arg1->unk09 = 0;
    }
    arg1->unk05 += var_r28;
    if (GWGameStat.veryHardUnlock == 0 && arg1->unk05 == 4) {
        arg1->unk05 += var_r28;
    }
    if (arg1->unk05 > var_r27) {
        arg1->unk05 = 0;
    }
    else if (arg1->unk05 < 0) {
        arg1->unk05 = var_r27;
    }
    if (arg1->unk05 != 0) {
        var_r26 = arg1->unk05 + 3;
    }
    else {
        var_r26 = arg1->unk04;
    }
    PadLabelBankSet(arg1->unk04, arg1->unk05 == 0, var_r26, 1);
}

static void ScrollOutPadConfig(omObjData *arg0, ConfigWork *arg1)
{
    Vec sp14;
    Vec sp8;

    HuSprAttrSet(pauseCursorPos, 0, 4);
    arg0->rot.x = 32.0f;
    arg0->rot.y = 112.0f;
    arg0->rot.z = (608.0f - arg0->rot.x) / 20.0f;
    arg1->unk07 = 0x14;
    ShowPadConfigSprite(1);
    HuSprGrpPosSet(padConfigSprGrp, arg0->rot.x, arg0->rot.y);
    sp14.x = -40.0f + boxPosTbl[0][0];
    sp14.y = -32.0f + boxPosTbl[0][1];
    HuSprPosSet(pauseCursorPos, 0, sp14.x, sp14.y);
    HuSprZRotSet(pauseCursorPos, 0, 45.0f);
    sp14.x = boxPosTbl[0][0];
    sp14.y = boxPosTbl[0][1];
    sp14.z = 700.0f;
    VECSubtract(&sp14, &padBoxPos, &sp8);
    arg0->scale.x = sp8.x / 20.0f;
    arg0->scale.y = sp8.y / 20.0f;
    arg1->unk01 = 1;
    arg1->unk00_field2 = 1;
}

static s32 UpdatePauseBox(omObjData *arg0, ConfigWork *arg1, s32 arg2)
{
    float var_f31;
    float var_f30;
    float var_f29;
    s32 temp_r27;
    u32 temp_r28;
    Mtx sp8;

    switch (arg1->unk01) {
        case 0:
            arg1->unk08 = arg1->unk04 = boxState[arg2];
            arg0->rot.x = boxRot[arg2];
            arg0->rot.y = 0.0f;
            arg1->unk01 = 1;
            var_f31 = 0.7f;
            BoardModelScaleSet(boxModelID[arg2], var_f31, var_f31, var_f31);
            return 0;
        case 1:
            temp_r28 = HuPadDStk[pausePad] | HuPadBtnDown[pausePad];
            temp_r27 = arg1->unk04;
            if (temp_r28 == 1) {
                arg1->unk04++;
            }
            if (temp_r28 == 2) {
                arg1->unk04--;
            }
            if (arg1->unk04 < 0) {
                arg1->unk04 = 0;
            }
            else if (arg1->unk04 > 1) {
                arg1->unk04 = 1;
            }
            else if (temp_r28 == 1 || temp_r28 == 2) {
                HuAudFXPlay(0);
            }
            if (temp_r28 == 0x100) {
                arg1->unk01 = 3;
                HuAudFXPlay(2);
            }
            else if (temp_r28 == 0x200) {
                arg1->unk01 = 4;
                HuAudFXPlay(3);
            }
            else if (temp_r27 != arg1->unk04) {
                if (arg1->unk04 != 0) {
                    var_f30 = 0.0f;
                }
                else {
                    var_f30 = -90.0f;
                }
                var_f29 = arg0->rot.x;
                arg0->rot.y = (var_f30 - var_f29) / 30.0f;
                arg1->unk07 = 30;
                arg1->unk01 = 2;
            }
            break;
        case 2:
            if (arg1->unk07 != 0) {
                arg0->rot.x += arg0->rot.y;
                arg1->unk07--;
            }
            else {
                arg1->unk01 = 1;
            }
            break;
        case 3:
            boxState[arg2] = arg1->unk04;
            boxRot[arg2] = arg0->rot.x;
            arg1->unk08 = -1;
            arg1->unk01 = 5;
            arg1->unk0A = 0;
            break;
        case 4:
            boxState[arg2] = arg1->unk08;
            arg1->unk04 = boxState[arg2];
            arg0->rot.x = boxRot[arg2];
            arg1->unk01 = 5;
            arg1->unk0A = 0;
            break;
        case 5:
            var_f31 = 0.5f;
            BoardModelScaleSet(boxModelID[arg2], var_f31, var_f31, var_f31);
            arg1->unk00_field1 = 0;
            arg1->unk04 = 0;
            arg1->unk05 = 0;
            arg1->unk01 = 0;
            return 1;
    }
    RotateBox(3.5f, arg0->rot.x, sp8);
    BoardModelMtxSet(boxModelID[arg2], &sp8);
    boxState[arg2] = arg1->unk04;
    return 0;
}

static s32 UpdatePauseBoxExt(omObjData *arg0, ConfigWork *arg1, s32 arg2)
{
    float var_f31;
    float var_f30;
    float var_f29;
    s32 temp_r26;
    s32 var_r27;
    u32 temp_r28;
    Mtx sp8;

    if (arg2 == 3 && GWGameStat.customPackEnable == 0) {
        var_r27 = 1;
    }
    else {
        var_r27 = 2;
    }
    switch (arg1->unk01) {
        case 0:
            arg1->unk08 = arg1->unk04 = boxState[arg2];
            arg0->rot.x = boxRot[arg2];
            arg0->rot.y = 0.0f;
            arg1->unk01 = 1;
            var_f31 = 0.7f;
            BoardModelScaleSet(boxModelID[arg2], var_f31, var_f31, var_f31);
            return 0;
        case 1:
            temp_r28 = HuPadDStk[pausePad] | HuPadBtnDown[pausePad];
            temp_r26 = arg1->unk04;
            if (temp_r28 == 1) {
                arg1->unk04--;
            }
            if (temp_r28 == 2) {
                arg1->unk04++;
            }
            if (arg1->unk04 < 0) {
                arg1->unk04 = 0;
            }
            else if (arg1->unk04 > var_r27) {
                arg1->unk04 = var_r27;
            }
            else if (temp_r28 == 1 || temp_r28 == 2) {
                HuAudFXPlay(0);
            }
            if (temp_r28 == 0x100) {
                arg1->unk01 = 3;
                HuAudFXPlay(2);
            }
            else if (temp_r28 == 0x200) {
                arg1->unk01 = 4;
                HuAudFXPlay(3);
            }
            else if (temp_r26 != arg1->unk04) {
                switch (arg1->unk04) {
                    case 0:
                        var_f30 = 90.0f;
                        break;
                    case 1:
                        var_f30 = 0.0f;
                        break;
                    case 2:
                        var_f30 = -90.0f;
                        break;
                }
                var_f29 = arg0->rot.x;
                arg0->rot.y = (var_f30 - var_f29) / 30.0f;
                arg1->unk07 = 30;
                arg1->unk01 = 2;
            }
            break;
        case 2:
            if (arg1->unk07 != 0) {
                arg0->rot.x += arg0->rot.y;
                arg1->unk07--;
            }
            else {
                arg1->unk01 = 1;
            }
            break;
        case 3:
            boxState[arg2] = arg1->unk04;
            boxRot[arg2] = arg0->rot.x;
            arg1->unk01 = 5;
            arg1->unk0A = 0;
            arg1->unk08 = -1;
            break;
        case 4:
            boxState[arg2] = arg1->unk08;
            arg1->unk04 = boxState[arg2];
            arg0->rot.x = boxRot[arg2];
            arg1->unk01 = 5;
            arg1->unk0A = 0;
            break;
        case 5:
            var_f31 = 0.5f;
            BoardModelScaleSet(boxModelID[arg2], var_f31, var_f31, var_f31);
            arg1->unk00_field1 = 0;
            arg1->unk04 = 0;
            arg1->unk05 = 0;
            arg1->unk01 = 0;
            return 1;
    }
    RotateBox(3.5f, arg0->rot.x, sp8);
    BoardModelMtxSet(boxModelID[arg2], &sp8);
    boxState[arg2] = arg1->unk04;
    return 0;
}

static s32 ExecPauseConfig(omObjData *arg0, ConfigWork *arg1)
{
    s32 var_r27;
    s32 var_r26;
    s32 var_r29;
    s32 var_r28;
    u32 temp_r30;

    temp_r30 = 0;
    temp_r30 = HuPadBtnDown[pausePad];
    temp_r30 |= HuPadDStkRep[pausePad];
    var_r29 = var_r28 = 0;
    if (temp_r30 == 8) {
        var_r28 = -1;
    }
    if (temp_r30 == 4) {
        var_r28 = 1;
    }
    if (temp_r30 == 1) {
        var_r29 = -1;
    }
    if (temp_r30 == 2) {
        var_r29 = 1;
    }
    arg1->unk02 += var_r29;
    arg1->unk03 += var_r28;
    if (arg1->unk03 < 0) {
        arg1->unk03 = 0;
    }
    else if (arg1->unk03 >= 2) {
        arg1->unk03 = 1;
    }
    else if (var_r28 != 0) {
        HuAudFXPlay(0);
        arg1->unk06 = 4;
    }
    if (arg1->unk02 < 0) {
        arg1->unk02 = 0;
    }
    else if (arg1->unk02 >= 4) {
        arg1->unk02 = 3;
    }
    else if (var_r29 != 0) {
        HuAudFXPlay(0);
        arg1->unk06 = 4;
    }
    if (temp_r30 == 0x100) {
        var_r27 = 1;
        var_r26 = 0;
        switch (arg1->unk02 + arg1->unk03 * 4) {
            case 6:
                if (arg1->unk00_field4 != 0) {
                    var_r27 = 0;
                    var_r26 = 1;
                }
                break;
            case 0:
                if (GWPartyGet() == 0) {
                    var_r27 = 0;
                    var_r26 = 1;
                }
                break;
        }
        if (var_r26 != 0) {
            HuAudFXPlay(4);
        }
        else if (var_r27 != 0) {
            HuAudFXPlay(2);
            arg1->unk00_field1 = 1;
        }
    }
    if (temp_r30 == 0x200) {
        HuAudFXPlay(0x1C);
        arg1->unk00_field0 = 1;
    }
    return arg1->unk02 + arg1->unk03 * 4;
}

static void CreatePauseControlWin(void)
{
    float sp8[2];
    float var_f31;
    float var_f30;
    u32 var_r31;

    var_r31 = MAKE_MESSID(16, 53);
    HuWinMesMaxSizeGet(1, sp8, var_r31);
    var_f31 = 152.0f;
    var_f30 = 280.0f;
    settingsControlWin = HuWinCreate(var_f31, var_f30, PB_F2S16(sp8[0]), PB_F2S16(sp8[1]), 0);
    HuWinBGTPLvlSet(settingsControlWin, 0.0f);
    HuWinMesSpeedSet(settingsControlWin, 0);
    HuWinMesSet(settingsControlWin, var_r31);
    HuWinDispOff(settingsControlWin);
    if (GWBoardGet() == 7 || GWBoardGet() == 8) {
        var_r31 = MAKE_MESSID(16, 73);
    }
    else {
        var_r31 = MAKE_MESSID(16, 52);
    }
    HuWinMesMaxSizeGet(1, sp8, var_r31);
    var_f31 = -10000.0f;
    var_f30 = 376.0f;
    controlWin = HuWinCreate(var_f31, var_f30, PB_F2S16(sp8[0]), PB_F2S16(sp8[1]), 0);
    HuWinBGTPLvlSet(controlWin, 0.0f);
    HuWinMesSpeedSet(controlWin, 0);
    HuWinMesSet(controlWin, var_r31);
    HuWinDispOff(controlWin);
    var_f31 = 128.0f;
    var_f30 = 312.0f;
    sp8[0] = 432.0f;
    sp8[1] = 128.0f;
    settingsWin = HuWinCreate(var_f31, var_f30, PB_F2S16(sp8[0]), PB_F2S16(sp8[1]), 0);
    HuWinDispOff(settingsWin);
    HuWinMesSpeedSet(settingsWin, 0);
    var_f31 = 640.0f;
    var_f30 = 112.0f;
    sp8[0] = 504.0f;
    sp8[1] = 128.0f;
}

static void DeletePauseControlWin(void)
{
    if (settingsControlWin != -1) {
        HuWinKill(settingsControlWin);
        settingsControlWin = -1;
    }
    if (controlWin != -1) {
        HuWinKill(controlWin);
        controlWin = -1;
    }
    if (settingsWin != -1) {
        HuWinKill(settingsWin);
        settingsWin = -1;
    }
}

static s32 WaitPauseInput(void)
{
    s32 var_r28;
    s32 var_r31;
    u32 temp_r30;

    mainScreenF = 1;
    if (GWBoardGet() == 7 || GWBoardGet() == 8) {
        var_r28 = 1;
    }
    else {
        var_r28 = 0;
    }
    HuWinDispOn(controlWin);
    SetBoxVisible(0);
    HuSprAttrReset(pauseSprGrp, 0, HUSPR_ATTR_DISPOFF);
    HuSprAttrSet(pauseCursorPos, 0, HUSPR_ATTR_DISPOFF);
    PauseCreateNumber(0, GWSystem.turn);
    PauseCreateNumber(1, GWSystem.max_turn);
    HuPrcSleep(4);
    var_r31 = 0;
    while (1) {
        temp_r30 = HuPadBtnDown[pausePad];
        if (temp_r30 == 0x1000 || temp_r30 == 0x200) {
            var_r31 = 0;
            HuAudFXPlay(0x1C);
            break;
        }
        else if (var_r28 == 0) {
            if (temp_r30 == 0x100) {
                var_r31 = 1;
                HuAudFXPlay(1);
                break;
            }
        }
        else if (temp_r30 == 0x10) {
            pauseQuitF = 1;
            var_r31 = 2;
            mgQuitExtraF = 1;
            HuAudFXPlay(0x1C);
            break;
        }
        HuPrcVSleep();
    }
    HuWinDispOff(controlWin);
    if (pauseSprGrp != -1) {
        HuSprGrpKill(pauseSprGrp);
        pauseSprGrp = -1;
    }
    HuPrcSleep(2);
    return var_r31;
}

static void ShowPauseConfig(void)
{
    float var_f31;
    float var_f30;

    mainScreenF = 0;
    HuWinDispOn(settingsControlWin);
    HuWinDispOn(settingsWin);
    SetBoxVisible(1);
    HuSprAttrReset(pauseCursorPos, 0, HUSPR_ATTR_DISPOFF);
    BoardModelVisibilitySet(hostMdl, 1);
    if (GWPartyGet() == 0) {
        var_f31 = boxPosTbl[0][0];
        var_f30 = boxPosTbl[0][1];
        HuSprPosSet(pauseCursorPos, 1, var_f31, var_f30);
        HuSprAttrReset(pauseCursorPos, 1, HUSPR_ATTR_DISPOFF);
    }
    else {
        HuSprAttrSet(pauseCursorPos, 1, HUSPR_ATTR_DISPOFF);
    }
    while (configObj) {
        HuPrcVSleep();
    }
}

static void CreatePadConfigSprite(void)
{
    float temp_f31;
    float temp_f30;
    s32 var_r29;
    s32 temp_r28;
    s32 temp_r27;
    s32 temp_r26;
    s32 i;
    s32 j;

    for (i = 0; i < PadNum(); i++) {
        for (j = 0; j < GWPlayerCount(); j++) {
            if (i == GWPlayer[j].port) {
                break;
            }
        }
        if (j >= GW_PARTY_PLAYER_MAX) {
            if (pause8F) {
                EightLog("WARNING: pause menu: no player on port %d; showing %s's box", (int)i + 1, EightLogName(0));
            }
            j = 0; /* 8-player party: every port has a player; never reached */
        }
        PadBoxPos(i, &temp_f31, &temp_f30);
        temp_r28 = GWPlayer[j].character;
        temp_r27 = GWPlayer[j].diff;
        temp_r26 = GWPlayer[j].com;
        HuSprBankSet(padConfigSprGrp, PadFace(i), temp_r28);
        if (temp_r26 != 0) {
            var_r29 = temp_r27 + 4;
        }
        else {
            var_r29 = i;
        }
        PadLabelBankSet(i, temp_r26 == 0, var_r29, 0);
        HuSprPosSet(padConfigSprGrp, PadFace(i), temp_f31, temp_f30);
        HuSprPosSet(padConfigSprGrp, PadLabel(i), temp_f31, temp_f30);
        HuSprPosSet(padConfigSprGrp, PadFrame(i), temp_f31, temp_f30);
        if (pause8F && i >= 4) {
            HuSprPosSet(padConfigSprGrp, PadTag8(i), temp_f31, temp_f30);
        }
    }
    HuSprPosSet(padConfigSprGrp, PadPanel(), 260.0f, 64.0f);
    if (pause8F) {
        char buf[320];
        s32 len = 0;
        for (i = 0; i < 8; i++) {
            j = CheckPort(i);
            len += snprintf(buf + len, sizeof(buf) - len, "%s%dP %s %s", i ? ", " : "", (int)i + 1, EightLogName(j),
                GWPlayer[j].com ? (const char *[]) { "COM easy", "COM normal", "COM hard", "COM very hard" }[GWPlayer[j].diff & 3]
                                : "person");
            if (len >= (s32)sizeof(buf)) {
                break;
            }
        }
        EightLog("pause menu: controller page shows 8 ports (two rows, %.2fx): %s", pad8Scale, buf);
    }
    HuSprGrpPosSet(padConfigSprGrp, 0.0f, 0.0f);
}

static void ShowPadConfigSprite(s32 arg0)
{
    s32 temp_r31;
    s32 i;

    for (i = 0; i < PadNum(); i++) {
        /* The boxes of all ports (8 in the 8-player party). */
        temp_r31 = pause8F ? i : GWPlayer[i].port;
        if (arg0 != 0) {
            HuSprAttrReset(padConfigSprGrp, PadFace(temp_r31), HUSPR_ATTR_DISPOFF);
            HuSprAttrReset(padConfigSprGrp, PadLabel(temp_r31), HUSPR_ATTR_DISPOFF);
            HuSprAttrReset(padConfigSprGrp, PadFrame(temp_r31), HUSPR_ATTR_DISPOFF);
            if (pause8F && temp_r31 >= 4 && pad8Human[temp_r31]) {
                HuSprAttrSet(padConfigSprGrp, PadLabel(temp_r31), HUSPR_ATTR_DISPOFF);
                HuSprAttrReset(padConfigSprGrp, PadTag8(temp_r31), HUSPR_ATTR_DISPOFF);
            }
        }
        else {
            HuSprAttrSet(padConfigSprGrp, PadFace(temp_r31), HUSPR_ATTR_DISPOFF);
            HuSprAttrSet(padConfigSprGrp, PadLabel(temp_r31), HUSPR_ATTR_DISPOFF);
            HuSprAttrSet(padConfigSprGrp, PadFrame(temp_r31), HUSPR_ATTR_DISPOFF);
            if (pause8F && temp_r31 >= 4) {
                HuSprAttrSet(padConfigSprGrp, PadTag8(temp_r31), HUSPR_ATTR_DISPOFF);
            }
        }
    }
    if (arg0 != 0) {
        HuSprAttrReset(padConfigSprGrp, PadPanel(), HUSPR_ATTR_DISPOFF);
    }
    else {
        HuSprAttrSet(padConfigSprGrp, PadPanel(), HUSPR_ATTR_DISPOFF);
    }
}

static void PauseCreateNumber(s32 arg0, s8 arg1)
{
    s32 var_r31;
    s32 temp_r30;
    s32 temp_r29;

    temp_r29 = arg1 % 10;
    temp_r30 = arg1 / 10;
    if (arg0 == 0) {
        var_r31 = 3;
    }
    else {
        var_r31 = 5;
    }
    if (temp_r30 != 0) {
        HuSprBankSet(pauseSprGrp, var_r31, temp_r30);
    }
    else {
        HuSprAttrSet(pauseSprGrp, var_r31, 4);
    }
    HuSprBankSet(pauseSprGrp, var_r31 + 1, temp_r29);
}

BOOL BoardPauseActiveCheck(void)
{
    return (mainProcess != NULL) ? TRUE : FALSE;
}

BOOL BoardPauseReqCheck(void)
{
    s32 temp_r30;
    s32 i;

    if (BoardPauseActiveCheck()) {
        return FALSE;
    }
    omSysPauseEnable(0);
    if (BoardPauseDisableGet() != 0) {
        return FALSE;
    }
    /* 8-player party: any of the 8 players on the board (BoardPlayerInputCount). */
    for (i = 0; i < BoardPlayerInputCount(); i++) {
        temp_r30 = GWPlayer[i].port;
        if (HuPadStatGet(temp_r30) == 0 && (GWPartyGet() != 0 || GWPlayer[i].com == 0) && (HuPadBtnDown[temp_r30] & 0x1000)) {
            pausePad = temp_r30;
            return TRUE;
        }
    }
    return FALSE;
}

static s32 CheckPort(s32 arg0)
{
    s32 i;

    /* 8-player party: the player on that port can be in any of the 8 slots. */
    for (i = 0; i < GWPlayerCount(); i++) {
        if (arg0 == GWPlayer[i].port) {
            break;
        }
    }
    if (i >= GW_PARTY_PLAYER_MAX) {
        i = 0; /* every port has a player in the 8-player party; never reached */
    }
    return i;
}

static void RotateBox(float arg0, float arg1, Mtx arg2)
{
    Vec sp14;
    Mtx spB0;
    Mtx sp80;
    Mtx sp50;
    Mtx sp20;

    BoardCameraRotGet(&sp14);
    MTXRotRad(spB0, 'x', MTXDegToRad(sp14.x));
    MTXRotRad(sp80, 'y', MTXDegToRad(sp14.y));
    MTXRotRad(sp50, 'x', MTXDegToRad(arg0));
    MTXRotRad(sp20, 'y', MTXDegToRad(arg1));
    MTXConcat(sp80, spB0, sp80);
    MTXConcat(sp50, sp20, sp20);
    MTXConcat(sp80, sp20, arg2);
}
