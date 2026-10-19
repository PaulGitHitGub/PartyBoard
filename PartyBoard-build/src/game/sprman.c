#ifdef TARGET_PC
#include <dolphin/gx/GXAurora.h>
#endif
#include "port/determinism.h"
#include <stdlib.h>
#include "game/sprite.h"
#include "game/disp.h"
#include "game/memory.h"
#include "game/init.h"

#include "dolphin/mtx.h"

#ifndef __MWERKS__
#include "game/hu3d.h"
#endif

#ifdef TARGET_PC
#include "port/byteswap.h"
#include "port/crash_report.h"
#include "port/frame_interpolation.h"
#include "port/rollback.h"
#include <string.h>
#if defined(__APPLE__) || defined(__linux__)
#include <dlfcn.h>
#elif defined(_WIN32)
#include <windows.h> /* GetModuleHandleExA: the caller's module, as dladdr gives elsewhere */
#endif
#endif

#define SPRITE_DIRTY_ATTR 0x1
#define SPRITE_DIRTY_XFORM 0x2
#define SPRITE_DIRTY_COLOR 0x4

typedef struct sprite_order {
    u16 group;
    u16 sprite;
    u16 prio;
    u16 next;
} SpriteOrder;

SHARED_SYM HUSPRITE HuSprData[HUSPR_MAX];
SHARED_SYM HUSPRGRP HuSprGrpData[HUSPR_GRP_MAX];
static SpriteOrder HuSprOrder[HUSPR_MAX*2];

static s16 HuSprOrderNum;
static s16 HuSprOrderNo;
static BOOL HuSprPauseF;
#ifdef TARGET_PC
/* Presentation-only group matrices must never replace the authoritative
 * matrices that game code can inspect during the next simulation tick. */
static Mtx HuSprRenderMtx[HUSPR_GRP_MAX];
#endif

static void HuSprOrderEntry(s16 group, s16 sprite);


void HuSprInit(void)
{
    s16 i;
    HUSPRITE *sprite;
    HUSPRGRP *group;
    for(sprite = &HuSprData[1], i=1; i<HUSPR_MAX; i++, sprite++) {
        sprite->data = NULL;
    }
    for(group = HuSprGrpData, i=0; i<HUSPR_GRP_MAX; i++, group++) {
        group->capacity = 0;
    }
    sprite = &HuSprData[0];
    sprite->prio = 0;
    sprite->data = (void *)1;
    HuSprPauseF = FALSE;
#ifdef TARGET_PC
    PartyBoard_FrameInterpolationReset();
#endif
}

void HuSprClose(void)
{
    s16 i;
    HUSPRGRP *group;
    HUSPRITE *sprite;
    
    for(group = HuSprGrpData, i=0; i<HUSPR_GRP_MAX; i++, group++) {
        if(group->capacity != 0) {
            HuSprGrpKill(i);
        }
    }
    for(sprite = &HuSprData[1], i=1; i<HUSPR_MAX; i++, sprite++) {
        if(sprite->data) {
            HuSprKill(i);
        }
    }
    HuSprPauseF = FALSE;
#ifdef TARGET_PC
    PartyBoard_FrameInterpolationReset();
#endif
}

void HuSprExec(s16 draw_no)
{
    HUSPRITE *sprite;
    while(sprite = HuSprCall()) {
        if(!(sprite->attr & HUSPR_ATTR_DISPOFF) && sprite->drawNo == draw_no) {
            HuSprDisp(sprite);
        }
    }
}

void HuSprBegin(void)
{
    Mtx temp, rot;
    s16 i, j;
    Vec axis = {0, 0, 1};
    HUSPRGRP *group;
#ifdef TARGET_PC
    HUSPRGRP renderGroup;
#endif
    group = HuSprGrpData;
    HuSprOrderNum = 1;
    HuSprOrder[0].next = 0;
    HuSprOrder[0].prio = -1;
    for(i=0; i<HUSPR_GRP_MAX; i++, group++) {
        if(group->capacity != 0) {
            MTXTrans(temp, group->center.x*group->scale.x, group->center.y*group->scale.y, 0.0f);
            MTXRotAxisDeg(rot, &axis, group->zRot);
            MTXConcat(rot, temp, group->mtx);
            MTXScale(temp, group->scale.x, group->scale.y, 1.0f);
            MTXConcat(group->mtx, temp, group->mtx);
            mtxTransCat(group->mtx, group->pos.x, group->pos.y, 0);
#ifdef TARGET_PC
            renderGroup = *group;
            PartyBoard_FrameInterpolationSpriteGroup(i, &renderGroup);
            MTXTrans(temp, renderGroup.center.x*renderGroup.scale.x, renderGroup.center.y*renderGroup.scale.y, 0.0f);
            MTXRotAxisDeg(rot, &axis, renderGroup.zRot);
            MTXConcat(rot, temp, HuSprRenderMtx[i]);
            MTXScale(temp, renderGroup.scale.x, renderGroup.scale.y, 1.0f);
            MTXConcat(HuSprRenderMtx[i], temp, HuSprRenderMtx[i]);
            mtxTransCat(HuSprRenderMtx[i], renderGroup.pos.x, renderGroup.pos.y, 0);
#endif
            for(j=0; j<group->capacity; j++) {
                if(group->members[j] != -1) {
                    HuSprOrderEntry(i, group->members[j]);
                }
            }
        }
    }
    HuSprOrderNo = 0;
}

#ifdef TARGET_PC
/* A group member whose sprite has no picture (killed, or made from a NULL
   animation) would crash the draw (HuSprCall reads its banks). Log it and
   leave it out of the draw. Logging only for the first few. */
static BOOL HuSprMissingData(const char *what, s16 group, s16 sprite)
{
    static s32 warned;
    if (warned < 16) {
        warned++;
        OSReport("[sprite] WARNING: %s: sprite %d in group %d has no animation data; skipped\n", what, (int)sprite,
            (int)group);
    }
    return TRUE;
}
#endif

static void HuSprOrderEntry(s16 group, s16 sprite)
{
    SpriteOrder *order = &HuSprOrder[HuSprOrderNum];
    s16 prio = HuSprData[sprite].prio;
    s16 prev, next;
#ifdef TARGET_PC
    if (sprite <= 0 || sprite >= HUSPR_MAX
        || (HuSprData[sprite].data == NULL && !(HuSprData[sprite].attr & HUSPR_ATTR_FUNC))) {
        HuSprMissingData("draw", group, sprite);
        return;
    }
    if (!(HuSprData[sprite].attr & HUSPR_ATTR_FUNC)
        && (HuSprData[sprite].bank < 0 || HuSprData[sprite].bank >= HuSprData[sprite].data->bankNum)) {
        /* A bank the animation does not have (e.g. a digit it has no picture
           for) reads past its bank table and crashes the draw: show bank 0. */
        static s32 warnedBank;
        if (warnedBank < 16) {
            warnedBank++;
            OSReport("[sprite] WARNING: sprite %d in group %d shows bank %d but its animation has %d; bank 0 shown\n",
                (int)sprite, (int)group, (int)HuSprData[sprite].bank, (int)HuSprData[sprite].data->bankNum);
        }
        HuSprData[sprite].bank = 0;
        HuSprData[sprite].animNo = 0;
    }
#endif
    if(HuSprOrderNum >= HUSPR_MAX*2) {
        OSReport("Order Max Over!\n");
        return;
    }
    next = HuSprOrder[0].next;
    for(prev = 0; next != 0; prev = next, next = HuSprOrder[next].next) {
        if(HuSprOrder[next].prio < prio) {
            break;
        }
    }
    order->next = HuSprOrder[prev].next;
    HuSprOrder[prev].next = HuSprOrderNum;
    order->prio = prio;
    order->group = group;
    order->sprite = sprite;
    HuSprOrderNum++;
}

HUSPRITE *HuSprCall(void)
{
    HuSprOrderNo = HuSprOrder[HuSprOrderNo].next;
    if(HuSprOrderNo != 0) {
        SpriteOrder *order = &HuSprOrder[HuSprOrderNo];
        HUSPRITE *sprite = &HuSprData[order->sprite];
#ifdef TARGET_PC
        sprite->groupMtx = &HuSprRenderMtx[order->group];
#else
        sprite->groupMtx = &HuSprGrpData[order->group].mtx;
#endif
        if(sprite->attr & HUSPR_ATTR_FUNC) {
            return sprite;
        }
        sprite->frameP = &sprite->data->bank[sprite->bank].frame[sprite->animNo];
        sprite->patP = &sprite->data->pat[sprite->frameP->pat];
        return sprite;
    } else {
        return NULL;
    }
}

static inline void SpriteCalcFrame(HUSPRITE *sprite, ANIMBANK *bank, ANIMFRAME **frame, s16 loop)
{
    if(sprite->time >= (*frame)->time) {
        sprite->animNo++;
        sprite->time -= (*frame)->time;
        if(sprite->animNo >= bank->timeNum || (*frame)[1].time == -1) {
            if(loop) {
                sprite->animNo = 0;
            } else {
                sprite->animNo = bank->timeNum-1;
            }
        }
        *frame = &bank->frame[sprite->animNo];
    } else if(sprite->time < 0) {
        sprite->animNo--;
        if(sprite->animNo < 0) {
            if(loop) {
                sprite->animNo = bank->timeNum-1;
            } else {
                sprite->animNo = 0;
            }
        }
        *frame = &bank->frame[sprite->animNo];
        sprite->time += (*frame)->time;
    }
}

void HuSprFinish(void)
{
    ANIMDATA *anim;
    ANIMBANK *bank;
    ANIMFRAME *frame;
    HUSPRITE *sprite;
    s16 i;
    s16 j;
    s16 loop;
    s16 dir;
    
    for(sprite = &HuSprData[1], i=1; i<HUSPR_MAX; i++, sprite++) {
        if(sprite->data && !(sprite->attr & HUSPR_ATTR_FUNC)) {
            if(!HuSprPauseF || (sprite->attr & HUSPR_ATTR_NOPAUSE)) {
                anim = sprite->data;
                bank = &anim->bank[sprite->bank];
                frame = &bank->frame[sprite->animNo];
                loop = (sprite->attr & HUSPR_ATTR_LOOP) ? 0 : 1;
                if(!(sprite->attr & HUSPR_ATTR_NOANIM)) {
                    dir = (sprite->attr & HUSPR_ATTR_REVERSE) ? -1 : 1;
                    for(j=0; j<PB_F2S32(sprite->speed)*minimumVcount; j++) {
                        sprite->time += dir;
                        SpriteCalcFrame(sprite, bank, &frame, loop);
                    }
                    sprite->time += (sprite->speed*(float)minimumVcount)-j;
                    SpriteCalcFrame(sprite, bank, &frame, loop);
                }
                sprite->dirty = 0;
            }
        }
    }
}

void HuSprPauseSet(BOOL value)
{
    HuSprPauseF = value;
}

#ifdef BYTESWAPPING
/* Once a sprite file has been converted, HuSprAnimRead writes this mark into
   the file's use-count field (header bytes 6-7). Nothing reads that field on
   PC (each converted ANIMDATA keeps its own count; the GameCube resets it
   too) and every sprite file on the disc holds 0x4100 there (all 1804 checked
   by PARTYBOARD_TEST_LOADERS), so the mark only tells the log when one buffer
   is converted a second time. */
uintptr_t PartyBoard_HuMemBlockNum(void *ptr, s32 *heapOut);

#define ANIM_FILE_CONVERTED_MARK_HI 'P'
#define ANIM_FILE_CONVERTED_MARK_LO 'B'

/* A file's counts come from the disc; a negative one can only mean the
   buffer is not (or no longer) a sprite file. Log it and use 0 rather than
   allocate or copy with it. */
static s16 HuSprAnimCountCheck(const void *data, const char *what, s32 index, s16 count)
{
    if (count < 0) {
        OSReport("[sprite] warning: sprite file at %p has a bad %s count %d (entry %d); using 0\n", data, what,
            (int)count, (int)index);
        return 0;
    }
    return count;
}
#endif

ANIMDATA *HuSprAnimRead(void *data)
{
    s16 i;
    ANIMBMP *bmp;
    ANIMBANK *bank;
    ANIMPAT *pat;

    ANIMDATA *anim = data;
#ifdef BYTESWAPPING
    s16 j;
    s32 frameTotal;
    s32 layerTotal;
    ANIMFRAME *frames;
    ANIMLAYER *layers;
    uintptr_t copyNum = (uintptr_t)-256; /* what HuMemDirectMalloc gives: no num */
    if (anim->valid == ANIM_DATA_ALLOCATION_VALID) {
        anim->useNum++;
        return anim;
    }
    if (anim->valid == ANIM_DATA_ALLOCATION_FREED) {
        /* An animation already freed (see HuSprAnimFree). Before the freed
           mark existed it still read as valid here and was handed back with
           one more use, so that is still what happens; converting it as a
           file would read freed memory as offsets. */
        OSReport("[sprite] warning: HuSprAnimRead(%p) of an animation that was already freed\n", data);
        anim->useNum++;
        return anim;
    }
    /* The file data (big-endian) is not converted in place: every structure is
       converted from a copy into memory this ANIMDATA owns, so the file can be
       read any number of times. The game does read one file more than once
       (w01 RoundItemInit makes four item animations from one buffer);
       converting in place swapped it back on the second read, so its sizes and
       offsets became garbage (out-of-bounds read, a crash on Windows) and the
       first animation's frames were corrupted. Frames and layers live in the
       same allocations as the banks and patterns, so HuSprAnimKill frees them
       with those. The only byte written to the file is the converted mark. */
    {
        AnimData32b header;
        const u8 *mark = (const u8 *)data + 6;
        if (mark[0] == ANIM_FILE_CONVERTED_MARK_HI && mark[1] == ANIM_FILE_CONVERTED_MARK_LO
            && !PartyBoard_RollbackIsResimulating()) {
            OSReport("[sprite] sprite file at %p converted again (safe: each read converts its own copy)\n", data);
        }
        memcpy(&header, data, sizeof(header));
        /* On the GameCube the animation IS its file buffer, so it lived exactly
           as long as the buffer: most files are read with MEMORY_DEFAULT_NUM and
           freed with the overlay's memory when it ends, whether or not the game
           ever killed the animation (result screens never do). The converted
           copy had no num, so on PC every such animation stayed in HEAP_DATA,
           with its GPU textures, for the rest of the session (about ten per
           board turn). The copy now takes the buffer's num where freeing by that
           num frees it together with the buffer: MEMORY_DEFAULT_NUM (freed in
           every heap when an overlay ends) or any num of a buffer in HEAP_DATA. */
        {
            s32 fileHeap;
            const uintptr_t fileNum = PartyBoard_HuMemBlockNum(data, &fileHeap);
            if (fileNum == MEMORY_DEFAULT_NUM || (fileHeap == HEAP_DATA && fileNum != (uintptr_t)-256)) {
                copyNum = fileNum;
            }
        }
        anim = HuMemDirectMallocNum(HEAP_DATA, sizeof(ANIMDATA), copyNum);
        byteswap_animdata(&header, anim);
        anim->valid = ANIM_DATA_ALLOCATION_VALID;
        anim->bankNum = HuSprAnimCountCheck(data, "bank", -1, anim->bankNum);
        anim->patNum = HuSprAnimCountCheck(data, "pattern", -1, anim->patNum);
        anim->bmpNum = HuSprAnimCountCheck(data, "bitmap", -1, anim->bmpNum);
    }
    frameTotal = 0;
    for (i = 0; i < anim->bankNum; i++) {
        AnimBankData32b raw;
        ANIMBANK conv;
        memcpy(&raw, (u8 *)data + (uintptr_t)anim->bank + i * sizeof(AnimBankData32b), sizeof(raw));
        byteswap_animbankdata(&raw, &conv);
        frameTotal += HuSprAnimCountCheck(data, "frame", i, conv.timeNum);
    }
    bank = HuMemDirectMallocNum(HEAP_DATA, anim->bankNum * sizeof(ANIMBANK) + frameTotal * sizeof(ANIMFRAME), copyNum);
    frames = (ANIMFRAME *)(bank + anim->bankNum);
    for (i = 0; i < anim->bankNum; i++) {
        AnimBankData32b raw;
        memcpy(&raw, (u8 *)data + (uintptr_t)anim->bank + i * sizeof(AnimBankData32b), sizeof(raw));
        byteswap_animbankdata(&raw, &bank[i]);
        if (bank[i].timeNum < 0) {
            bank[i].timeNum = 0; /* reported by the counting pass above */
        }
        memcpy(frames, (u8 *)data + (uintptr_t)bank[i].frame, bank[i].timeNum * sizeof(ANIMFRAME));
        for (j = 0; j < bank[i].timeNum; j++) {
            byteswap_animframedata(&frames[j]);
        }
        bank[i].frame = frames;
        frames += bank[i].timeNum;
    }
    anim->bank = bank;
    layerTotal = 0;
    for (i = 0; i < anim->patNum; i++) {
        AnimPatData32b raw;
        ANIMPAT conv;
        memcpy(&raw, (u8 *)data + (uintptr_t)anim->pat + i * sizeof(AnimPatData32b), sizeof(raw));
        byteswap_animpatdata(&raw, &conv);
        layerTotal += HuSprAnimCountCheck(data, "layer", i, conv.layerNum);
    }
    pat = HuMemDirectMallocNum(HEAP_DATA, anim->patNum * sizeof(ANIMPAT) + layerTotal * sizeof(ANIMLAYER), copyNum);
    layers = (ANIMLAYER *)(pat + anim->patNum);
    for (i = 0; i < anim->patNum; i++) {
        AnimPatData32b raw;
        memcpy(&raw, (u8 *)data + (uintptr_t)anim->pat + i * sizeof(AnimPatData32b), sizeof(raw));
        byteswap_animpatdata(&raw, &pat[i]);
        if (pat[i].layerNum < 0) {
            pat[i].layerNum = 0; /* reported by the counting pass above */
        }
        memcpy(layers, (u8 *)data + (uintptr_t)pat[i].layer, pat[i].layerNum * sizeof(ANIMLAYER));
        for (j = 0; j < pat[i].layerNum; j++) {
            byteswap_animlayerdata(&layers[j]);
        }
        pat[i].layer = layers;
        layers += pat[i].layerNum;
    }
    anim->pat = pat;
    bmp = HuMemDirectMallocNum(HEAP_DATA, anim->bmpNum * sizeof(ANIMBMP), copyNum);
    for (i = 0; i < anim->bmpNum; i++) {
        AnimBmpData32b raw;
        memcpy(&raw, (u8 *)data + (uintptr_t)anim->bmp + i * sizeof(AnimBmpData32b), sizeof(raw));
        byteswap_animbmpdata(&raw, &bmp[i]);
#ifdef OPTIMIZED_TEXTURE_LOADING
        for (j = 0; j < sizeof(anim->bmp[0].texData) / sizeof(anim->bmp[0].texData[0]); j++) {
            AnimTexData *tex = &bmp[i].texData[j];
            tex->tex_initialized = FALSE;
            tex->tlut_initialized = FALSE;
            tex->wrap_s = tex->wrap_t = GX_CLAMP;
            memset(&tex->tex_obj, 0, sizeof(tex->tex_obj));
            memset(&tex->tlut_obj, 0, sizeof(tex->tlut_obj));
        }
#endif
        /* Pixel and palette data stay in the file buffer: they are read as
           big-endian GX textures and never swapped. */
        bmp[i].palData = (void *)((uintptr_t)bmp[i].palData + (uintptr_t)data);
        bmp[i].data = (void *)((uintptr_t)bmp[i].data + (uintptr_t)data);
    }
    anim->bmp = bmp;
    ((u8 *)data)[6] = ANIM_FILE_CONVERTED_MARK_HI;
    ((u8 *)data)[7] = ANIM_FILE_CONVERTED_MARK_LO;
#else
    if((uintptr_t)anim->bank & ~0xFFFF) {
        anim->useNum++;
        return anim;
    }
    bank = (void *)((uintptr_t)anim->bank+(uintptr_t)data);
    anim->bank = bank;
    pat = (void *)((uintptr_t)anim->pat+(uintptr_t)data);
    anim->pat = pat;
    bmp = (void *)((uintptr_t)anim->bmp+(uintptr_t)data);
    anim->bmp = bmp;
    for(i=0; i<anim->bankNum; i++, bank++) {
        bank->frame = (ANIMFRAME *)((uintptr_t)bank->frame+(uintptr_t)data);
    }
    for(i=0; i<anim->patNum; i++, pat++) {
        pat->layer = (ANIMLAYER *)((uintptr_t)pat->layer+(uintptr_t)data);
    }
    for(i=0; i<anim->bmpNum; i++, bmp++) {
        bmp->palData = (void *)((uintptr_t)bmp->palData+(uintptr_t)data);
        bmp->data = (void *)((uintptr_t)bmp->data+(uintptr_t)data);
    }
#endif
    anim->useNum = 0;
#ifdef TARGET_PC
    /* Test aid: with PARTYBOARD_DUMP_SPRITES and PARTYBOARD_DUMP_TEXTURES set,
       every image of every sprite file is written out as it loads, named as a
       replacement texture for it must be (used to find button prompts). */
    if (getenv("PARTYBOARD_DUMP_SPRITES") != NULL) {
        for (i = 0; i < anim->bmpNum; i++) {
            HuSprTexLoad(anim, i, 7, GX_CLAMP, GX_CLAMP, GX_LINEAR);
            GXAuroraProbeTexture(&anim->bmp[i].texData[7].tex_obj);
        }
        for (i = 0; i < anim->bankNum; i++) {
            ANIMPAT *pat0 = &anim->pat[anim->bank[i].frame[0].pat];
            char name[128];
            GXAuroraTextureName(&anim->bmp[pat0->layer[0].bmpNo].texData[7].tex_obj, name, sizeof(name));
            OSReport("[SPRITES] anim=%p banks=%d bank %d -> bmp %d %s\n", (void *)anim, anim->bankNum, i,
                pat0->layer[0].bmpNo, name);
        }
    }
#endif
    return anim;
}

void HuSprAnimLock(ANIMDATA *anim)
{
    anim->useNum++;
}

s16 HuSprCreate(ANIMDATA *anim, s16 prio, s16 bank)
{
    HUSPRITE *sprite;
    s16 i;
    for(sprite = &HuSprData[1], i=1; i<HUSPR_MAX; i++, sprite++) {
        if(!sprite->data) {
            break;
        }
    }
    if(i == HUSPR_MAX) {
        return HUSPR_NONE;
    }
    sprite->data = anim;
    sprite->speed = 1.0f;
    sprite->animNo = 0;
    sprite->bank = bank;
    sprite->time = 0.0f;
    sprite->attr = 0;
    sprite->drawNo = 0;
    sprite->r = sprite->g = sprite->b = PB_F2U8(sprite->a = 255);
    sprite->pos.x = sprite->pos.y = sprite->zRot = 0.0f;
    sprite->prio = prio;
    sprite->scale.x = sprite->scale.y = 1.0f;
    sprite->wrapS = sprite->wrapT = GX_CLAMP;
    sprite->uvScaleX = sprite->uvScaleY = 1;
    sprite->bg = NULL;
    sprite->scissorX = sprite->scissorY = 0;
    sprite->scissorW = HU_FB_WIDTH;
    sprite->scissorH = HU_FB_HEIGHT;
    if(anim) {
        HuSprAnimLock(anim);
    }
#ifdef TARGET_PC
    PartyBoard_FrameInterpolationInvalidateSprite(i);
#endif
    return i;
}

s16 HuSprFuncCreate(HUSPRFUNC func, s16 prio)
{
    HUSPRITE *sprite;
    s16 index = HuSprCreate(NULL, prio, 0);
    if(index == HUSPR_NONE) {
        return HUSPR_NONE;
    }
    sprite = &HuSprData[index];
    sprite->func = func;
    sprite->attr |= HUSPR_ATTR_FUNC;
    return index;
}

s16 HuSprGrpCreate(s16 capacity)
{
    HUSPRGRP *group;
    s16 i, j;
    for(group = HuSprGrpData, i=0; i<HUSPR_GRP_MAX; i++, group++) {
        if(group->capacity == 0) {
            break;
        }
    }
    if(i == HUSPR_GRP_MAX) {
        return HUSPR_GRP_NONE;
    }
    group->members = HuMemDirectMalloc(HEAP_SYSTEM, sizeof(s16)*capacity);
    for(j=0; j<capacity; j++) {
        group->members[j] = HUSPR_NONE;
    }
    group->capacity = capacity;
    group->pos.x = group->pos.y = group->zRot = group->center.x = group->center.y = 0.0f;
    group->scale.x = group->scale.y = 1.0f;
#ifdef TARGET_PC
    PartyBoard_FrameInterpolationInvalidateSpriteGroup(i);
#endif
    return i;
}

s16 HuSprGrpCopy(s16 group)
{
    HUSPRGRP *new_group_ptr;
    HUSPRGRP *group_ptr = &HuSprGrpData[group];
    s16 new_group = HuSprGrpCreate(group_ptr->capacity);
    s16 i;
    if(new_group == HUSPR_GRP_NONE) {
        return HUSPR_GRP_NONE;
    }
    new_group_ptr = &HuSprGrpData[new_group];
    new_group_ptr->pos.x = group_ptr->pos.x;
    new_group_ptr->pos.y = group_ptr->pos.y;
    new_group_ptr->zRot = group_ptr->zRot;
    new_group_ptr->scale.x = group_ptr->scale.x;
    new_group_ptr->scale.y = group_ptr->scale.y;
    new_group_ptr->center.x = group_ptr->center.x;
    new_group_ptr->center.y = group_ptr->center.y;
    for(i=0; i<group_ptr->capacity; i++) {
        if(group_ptr->members[i] != HUSPR_NONE) {
            HUSPRITE *old_sprite = &HuSprData[group_ptr->members[i]];
            s16 new_sprite = HuSprCreate(old_sprite->data, old_sprite->prio, old_sprite->bank);
            HuSprData[new_sprite] = *old_sprite;
            HuSprGrpMemberSet(new_group, i, new_sprite);
        }
    }
    return new_group;
}

void HuSprGrpMemberSet(s16 group, s16 member, s16 sprite)
{
    HUSPRGRP *group_ptr = &HuSprGrpData[group];
    HUSPRITE *sprite_ptr = &HuSprData[sprite];
    if(group_ptr->capacity == 0 || group_ptr->capacity <= member || group_ptr->members[member] != HUSPR_NONE) {
        return;
    }
    group_ptr->members[member] = sprite;
}

void HuSprGrpMemberKill(s16 group, s16 member)
{
    HUSPRGRP *group_ptr = &HuSprGrpData[group];
    if(group_ptr->capacity == 0 || group_ptr->capacity <= member || group_ptr->members[member] == HUSPR_NONE) {
        return;
    }
    HuSprKill(group_ptr->members[member]);
    group_ptr->members[member] = HUSPR_NONE;
}

void HuSprGrpKill(s16 group)
{
    HUSPRGRP *group_ptr = &HuSprGrpData[group];
    s16 i;
#ifdef TARGET_PC
    /* Killing a group that is already gone frees its member list a second
       time. The original does it when leaving the end-of-game results
       (mstory3Dll's save step closes all sprites, then leaving the overlay
       kills the window groups again); the GameCube's allocator refused the
       second free. Skip it and say so. */
    if (group < 0 || group >= HUSPR_GRP_MAX || group_ptr->capacity == 0) {
        static s32 warnedKill;
        if (warnedKill < 16) {
            warnedKill++;
            OSReport("[sprite] WARNING: sprite group %d killed again (already gone); skipped\n", (int)group);
        }
        return;
    }
#endif
    for(i=0; i<group_ptr->capacity; i++) {
        if(group_ptr->members[i] != HUSPR_NONE) {
#ifdef TARGET_PC
            if (HuSprData[group_ptr->members[i]].data == NULL && !(HuSprData[group_ptr->members[i]].attr & HUSPR_ATTR_FUNC)) {
                HuSprMissingData("group kill", group, group_ptr->members[i]);
            }
#endif
            HuSprKill(group_ptr->members[i]);
        }
    }
    group_ptr->capacity = 0;
    HuMemDirectFree(group_ptr->members);
#ifdef TARGET_PC
    PartyBoard_FrameInterpolationInvalidateSpriteGroup(group);
#endif
}

void HuSprKill(s16 sprite)
{
    HUSPRITE *sprite_ptr = &HuSprData[sprite];
    if(!sprite_ptr->data) {
        return;
    }
    if(!(sprite_ptr->attr & HUSPR_ATTR_FUNC)) {
        HuSprAnimKill(sprite_ptr->data);
        if(sprite_ptr->bg) {
            HuSprAnimKill(sprite_ptr->bg);
            sprite_ptr->bg = NULL;
        }
    }
    sprite_ptr->data = NULL;
#ifdef TARGET_PC
    PartyBoard_FrameInterpolationInvalidateSprite(sprite);
#endif
}

#ifdef BYTESWAPPING
/* The caller's address; the intrinsic differs per compiler (as in malloc.c). */
#if defined(_MSC_VER)
#include <intrin.h>
#pragma intrinsic(_ReturnAddress)
#define HUSPR_ANIM_CALLER() _ReturnAddress()
#else
#define HUSPR_ANIM_CALLER() __builtin_return_address(0)
#endif

/* Some minigames release an animation that a model still holds: m456 frees
   its particle animations and m411 its screen animations just before leaving,
   and leaving then kills the particle/model, which releases them again. On
   the GameCube the second release decremented a count in freed memory and the
   allocator refused the second free ("HuMem>memory free error"). On PC one
   animation is four blocks plus GPU textures, and the second release read
   freed memory, wrote to it and tried to free all four again; had any block
   been handed out again in between, it would have freed someone else's live
   memory. An animation that is no longer a live, converted ANIMDATA is now
   logged and left alone, which is what the GameCube's refused free amounted
   to. (A block reused for a new animation at the same address cannot be told
   apart; leaving a minigame allocates nothing between the two releases.) */
static BOOL HuSprAnimLive(ANIMDATA *anim, const char *func, void *caller)
{
    const char *why = NULL;
    if (anim == NULL) {
        why = "no animation";
    } else if ((uintptr_t)anim < 0x10000) {
        why = "not an animation"; /* a marker value, never memory */
    } else if (HuMemMemorySizeGet(anim) < sizeof(ANIMDATA)) {
        why = anim->valid == ANIM_DATA_ALLOCATION_FREED ? "already freed" : "its memory is not a live block";
    } else if (anim->valid != ANIM_DATA_ALLOCATION_VALID) {
        why = anim->valid == ANIM_DATA_ALLOCATION_FREED ? "already freed" : "not an animation";
    }
    if (why == NULL) {
        return TRUE;
    }
    if (!PartyBoard_RollbackIsResimulating()) {
        char where[384];
        PartyBoard_DescribeCodeAddress((uintptr_t)caller, where, sizeof(where));
        OSReport("[sprite] %s(%p) skipped: %s (called from %s)\n", func, (void *)anim, why, where);
    }
    return FALSE;
}

static void HuSprAnimFree(ANIMDATA *anim, BOOL freeBmpData)
{
    if(freeBmpData && (anim->bmpNum & ANIM_BMP_ALLOC)) {
        if(anim->bmp->data) {
            HuMemDirectFree(anim->bmp->data);
        }
        if(anim->bmp->palData) {
            HuMemDirectFree(anim->bmp->palData);
        }
    }
#ifdef OPTIMIZED_TEXTURE_LOADING
    {
        int i;
        int j;
        for(i=0; i<(anim->bmpNum & ANIM_BMP_NUM_MASK); i++) {
            for (j = 0; j < sizeof(anim->bmp[0].texData) / sizeof(anim->bmp[0].texData[0]); j++) {
                AnimTexData *tex = &anim->bmp[i].texData[j];
                if (tex->tex_initialized) {
                    GXDestroyTexObj(&tex->tex_obj);
                }
                if (tex->tlut_initialized) {
                    GXDestroyTlutObj(&tex->tlut_obj);
                }
            }
        }
    }
#endif
    HuMemDirectFree(anim->bank);
    HuMemDirectFree(anim->pat);
    HuMemDirectFree(anim->bmp);
    anim->valid = ANIM_DATA_ALLOCATION_FREED;
    HuMemDirectFree(anim);
}

void HuSprAnimRelease(ANIMDATA *anim, BOOL freeBmpData, const char *func, void *caller)
{
    if (!HuSprAnimLive(anim, func, caller)) {
        return;
    }
    if (--anim->useNum <= 0) {
        HuSprAnimFree(anim, freeBmpData);
    }
}

void HuSprAnimKill(ANIMDATA *anim)
{
    HuSprAnimRelease(anim, TRUE, "HuSprAnimKill", HUSPR_ANIM_CALLER());
}
#else
void HuSprAnimKill(ANIMDATA *anim)
{
    if(--anim->useNum <= 0) {
        if(anim->bmpNum & ANIM_BMP_ALLOC) {
            if(anim->bmp->data) {
                HuMemDirectFree(anim->bmp->data);
            }
            if(anim->bmp->palData) {
                HuMemDirectFree(anim->bmp->palData);
            }
        }
#ifdef OPTIMIZED_TEXTURE_LOADING
        int i;
        int j;
        for(i=0; i<anim->bmpNum; i++) {
            for (j = 0; j < sizeof(anim->bmp[0].texData) / sizeof(anim->bmp[0].texData[0]); j++) {
                AnimTexData *tex = &anim->bmp[i].texData[j];
                if (tex->tex_initialized) {
                    GXDestroyTexObj(&tex->tex_obj);
                }
                if (tex->tlut_initialized) {
                    GXDestroyTlutObj(&tex->tlut_obj);
                }
            }
        }
#endif
        HuMemDirectFree(anim);
    }
}
#endif

#ifdef TARGET_PC
// A REL that passes a bad group or member to one of the setters below used to
// crash on a wild read or write (character select on Apple Silicon passed
// members like -3488). Report the values and the calling module+offset in the
// session log and skip the call instead.
static BOOL HuSprMemberArgsBad(const char *func, s16 group, s16 member, BOOL needsAnim, void *caller)
{
    const char *why = NULL;
    s16 sprite = HUSPR_NONE;
    if (group < 0 || group >= HUSPR_GRP_MAX) {
        why = "group out of range";
    } else if (HuSprGrpData[group].members == NULL) {
        why = "group not created";
    } else if (member < 0 || member >= HuSprGrpData[group].capacity) {
        why = "member out of range";
    } else {
        sprite = HuSprGrpData[group].members[member];
        if (sprite < 0 || sprite >= HUSPR_MAX) {
            why = "member has no sprite";
        } else if (needsAnim && HuSprData[sprite].data == NULL) {
            why = "sprite has no animation";
        }
    }
    if (why == NULL) {
        return FALSE;
    }
    const char *module = "?";
    uintptr_t offset = (uintptr_t)caller;
#if defined(__APPLE__) || defined(__linux__)
    Dl_info info;
    if (dladdr(caller, &info) && info.dli_fname != NULL) {
        module = strrchr(info.dli_fname, '/') ? strrchr(info.dli_fname, '/') + 1 : info.dli_fname;
        offset = (uintptr_t)caller - (uintptr_t)info.dli_fbase;
    }
#elif defined(_WIN32)
    /* The DLL and the offset in it, as on the Mac: a bare address changes every launch. */
    char modulePath[MAX_PATH] = { 0 };
    HMODULE callerModule = NULL;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCSTR)caller, &callerModule)
        && callerModule != NULL) {
        if (GetModuleFileNameA(callerModule, modulePath, sizeof(modulePath) - 1) > 0) {
            module = strrchr(modulePath, '\\') ? strrchr(modulePath, '\\') + 1 : modulePath;
        }
        offset = (uintptr_t)caller - (uintptr_t)callerModule;
    }
#endif
    // Some menus do this every time they open (an unused slot), so report each
    // function+caller pair three times, then once more to say it was muted.
    {
        static struct {
            const char *func;
            void *caller;
            int count;
        } seen[64];
        static int seenCount = 0;
        int slot = -1;
        for (int i = 0; i < seenCount; ++i) {
            if (seen[i].func == func && seen[i].caller == caller) {
                slot = i;
                break;
            }
        }
        if (slot < 0 && seenCount < 64) {
            slot = seenCount++;
            seen[slot].func = func;
            seen[slot].caller = caller;
            seen[slot].count = 0;
        }
        const int count = slot >= 0 ? ++seen[slot].count : 99;
        if (count > 4) {
            return TRUE;
        }
        if (count == 4) {
            OSReport("[partyboard] %s from %s+0x%lx: further reports muted\n", func, module, (unsigned long)offset);
            return TRUE;
        }
    }
    OSReport("[partyboard] %s skipped: %s (group %d, member %d, capacity %d, sprite %d) called from %s+0x%lx\n", func, why,
        group, member, (group >= 0 && group < HUSPR_GRP_MAX) ? HuSprGrpData[group].capacity : -1, sprite, module,
        (unsigned long)offset);
    return TRUE;
}
/* The caller's address; the intrinsic differs per compiler (as in malloc.c). */
#if defined(_MSC_VER)
#include <intrin.h>
#pragma intrinsic(_ReturnAddress)
#define HUSPR_CALLER() _ReturnAddress()
#else
#define HUSPR_CALLER() __builtin_return_address(0)
#endif
#define HUSPR_GUARD(group, member, needsAnim) \
    if (HuSprMemberArgsBad(__func__, group, member, needsAnim, HUSPR_CALLER())) \
    return
#else
#define HUSPR_GUARD(group, member, needsAnim)
#endif

void HuSprAttrSet(s16 group, s16 member, s32 attr)
{
    HUSPR_GUARD(group, member, FALSE);
    HUSPRGRP *group_ptr = &HuSprGrpData[group];
    HUSPRITE *sprite_ptr;
    if(group_ptr->capacity == 0 || group_ptr->capacity <= member || group_ptr->members[member] == HUSPR_NONE) {
        return;
    }
    sprite_ptr = &HuSprData[group_ptr->members[member]];
    sprite_ptr->attr |= attr;
    sprite_ptr->dirty |= SPRITE_DIRTY_ATTR;
}

void HuSprAttrReset(s16 group, s16 member, s32 attr)
{
    HUSPR_GUARD(group, member, FALSE);
    HUSPRGRP *group_ptr = &HuSprGrpData[group];
    HUSPRITE *sprite_ptr;
    if(group_ptr->capacity == 0 || group_ptr->capacity <= member || group_ptr->members[member] == HUSPR_NONE) {
        return;
    }
    sprite_ptr = &HuSprData[group_ptr->members[member]];
    sprite_ptr->attr &= ~attr;
    sprite_ptr->dirty |= SPRITE_DIRTY_ATTR;
}

void HuSprPosSet(s16 group, s16 member, float x, float y)
{
    HUSPR_GUARD(group, member, FALSE);
    HUSPRITE *sprite_ptr = &HuSprData[HuSprGrpData[group].members[member]];
    sprite_ptr->pos.x = x;
    sprite_ptr->pos.y = y;
    sprite_ptr->dirty |= SPRITE_DIRTY_XFORM;
}

void HuSprZRotSet(s16 group, s16 member, float z_rot)
{
    HUSPR_GUARD(group, member, FALSE);
    HUSPRITE *sprite_ptr = &HuSprData[HuSprGrpData[group].members[member]];
    sprite_ptr->zRot = z_rot;
    sprite_ptr->dirty |= SPRITE_DIRTY_XFORM;
}

void HuSprScaleSet(s16 group, s16 member, float x, float y)
{
    HUSPR_GUARD(group, member, FALSE);
    HUSPRITE *sprite_ptr = &HuSprData[HuSprGrpData[group].members[member]];
    sprite_ptr->scale.x = x;
    sprite_ptr->scale.y = y;
    sprite_ptr->dirty |= SPRITE_DIRTY_XFORM;
}

void HuSprTPLvlSet(s16 group, s16 member, float tp_lvl)
{
    HUSPR_GUARD(group, member, FALSE);
    HUSPRITE *sprite_ptr = &HuSprData[HuSprGrpData[group].members[member]];
    sprite_ptr->a = tp_lvl*255;
    sprite_ptr->dirty |= SPRITE_DIRTY_COLOR;
}

void HuSprColorSet(s16 group, s16 member, u8 r, u8 g, u8 b)
{
    HUSPR_GUARD(group, member, FALSE);
    HUSPRITE *sprite_ptr = &HuSprData[HuSprGrpData[group].members[member]];
    sprite_ptr->r = r;
    sprite_ptr->g = g;
    sprite_ptr->b = b;
    sprite_ptr->dirty |= SPRITE_DIRTY_COLOR;
}

void HuSprSpeedSet(s16 group, s16 member, float speed)
{
    HUSPR_GUARD(group, member, FALSE);
    HUSPRGRP *group_ptr = &HuSprGrpData[group];
    HuSprData[group_ptr->members[member]].speed = speed;
}

void HuSprBankSet(s16 group, s16 member, s16 bank)
{
    HUSPR_GUARD(group, member, TRUE);
    HUSPRITE *sprite_ptr = &HuSprData[HuSprGrpData[group].members[member]];
    ANIMDATA *anim = sprite_ptr->data;
    ANIMBANK *bank_ptr = &anim->bank[sprite_ptr->bank];
    ANIMFRAME *frame_ptr = &bank_ptr->frame[sprite_ptr->animNo];
    sprite_ptr->bank = bank;
    if(sprite_ptr->attr & HUSPR_ATTR_REVERSE) {
        sprite_ptr->animNo = bank_ptr->timeNum-1;
        frame_ptr = &bank_ptr->frame[sprite_ptr->animNo];
        sprite_ptr->time = frame_ptr->time;
    } else {
        sprite_ptr->time = 0;
        sprite_ptr->animNo = 0;
    }
}

void HuSprGrpPosSet(s16 group, float x, float y)
{
    HUSPRGRP *group_ptr = &HuSprGrpData[group];
    s16 i;
    group_ptr->pos.x = x;
    group_ptr->pos.y = y;
    for(i=0; i<group_ptr->capacity; i++) {
        if(group_ptr->members[i] != -1) {
            HuSprData[group_ptr->members[i]].dirty |= SPRITE_DIRTY_XFORM;
        }
    }
}

void HuSprGrpCenterSet(s16 group, float x, float y)
{
    HUSPRGRP *group_ptr = &HuSprGrpData[group];
    s16 i;
    group_ptr->center.x = x;
    group_ptr->center.y = y;
    for(i=0; i<group_ptr->capacity; i++) {
        if(group_ptr->members[i] != HUSPR_NONE) {
            HuSprData[group_ptr->members[i]].dirty |= SPRITE_DIRTY_XFORM;
        }
    }
}

void HuSprGrpZRotSet(s16 group, float z_rot)
{
    HUSPRGRP *group_ptr = &HuSprGrpData[group];
    s16 i;
    group_ptr->zRot = z_rot;
    for(i=0; i<group_ptr->capacity; i++) {
        if(group_ptr->members[i] != HUSPR_NONE) {
            HuSprData[group_ptr->members[i]].dirty |= SPRITE_DIRTY_XFORM;
        }
    }
}

void HuSprGrpScaleSet(s16 group, float x, float y)
{
    HUSPRGRP *group_ptr = &HuSprGrpData[group];
    s16 i;
    group_ptr->scale.x = x;
    group_ptr->scale.y = y;
    for(i=0; i<group_ptr->capacity; i++) {
        if(group_ptr->members[i] != HUSPR_NONE) {
            HuSprData[group_ptr->members[i]].dirty |= SPRITE_DIRTY_XFORM;
        }
    }
}

void HuSprGrpTPLvlSet(s16 group, float tp_lvl)
{
    HUSPRGRP *group_ptr = &HuSprGrpData[group];
    s16 i;
    for(i=0; i<group_ptr->capacity; i++) {
        if(group_ptr->members[i] != HUSPR_NONE) {
            HuSprData[group_ptr->members[i]].a = tp_lvl*255;
            HuSprData[group_ptr->members[i]].dirty |= SPRITE_DIRTY_COLOR;
        }
    }
}

void HuSprGrpDrawNoSet(s16 group, s32 draw_no)
{
    HUSPRGRP *group_ptr = &HuSprGrpData[group];
    s16 i;
    for(i=0; i<group_ptr->capacity; i++) {
        if(group_ptr->members[i] != HUSPR_NONE) {
            HuSprData[group_ptr->members[i]].drawNo = draw_no;
        }
    }
}

void HuSprDrawNoSet(s16 group, s16 member, s32 draw_no)
{
    HUSPR_GUARD(group, member, FALSE);
    HUSPRITE *sprite_ptr = &HuSprData[HuSprGrpData[group].members[member]];
    sprite_ptr->drawNo = draw_no;
}

void HuSprPriSet(s16 group, s16 member, s16 prio)
{
    HUSPR_GUARD(group, member, FALSE);
    HUSPRITE *sprite_ptr = &HuSprData[HuSprGrpData[group].members[member]];
    sprite_ptr->prio = prio;
}

void HuSprGrpScissorSet(s16 group, s16 x, s16 y, s16 w, s16 h)
{
    HUSPRGRP *group_ptr = &HuSprGrpData[group];
    s16 i;
    for(i=0; i<group_ptr->capacity; i++) {
        if(group_ptr->members[i] != HUSPR_NONE) {
            HuSprScissorSet(group, i, x, y, w, h);
        }
    }
}

void HuSprScissorSet(s16 group, s16 member, s16 x, s16 y, s16 w, s16 h)
{
    HUSPR_GUARD(group, member, FALSE);
    HUSPRITE *sprite_ptr = &HuSprData[HuSprGrpData[group].members[member]];
    sprite_ptr->scissorX = x;
    sprite_ptr->scissorY = y;
    sprite_ptr->scissorW = w;
    sprite_ptr->scissorH = h;
}

static s16 bitSizeTbl[11] = { 32, 24, 16, 8, 4, 16, 8, 8, 4, 8, 4 };

ANIMDATA *HuSprAnimMake(s16 sizeX, s16 sizeY, s16 dataFmt)
{
    ANIMLAYER *layer;
    ANIMBMP *bmp;
    ANIMDATA *anim;
    ANIMPAT *pat;
    ANIMFRAME *frame;
    void *temp;
    ANIMBANK *bank;
    ANIMDATA *new_anim;

#ifdef BYTESWAPPING
    /* Same layout as HuSprAnimRead, which HuSprAnimKill frees: the frame shares
       the bank's allocation and the layer the pattern's. (They were separate
       blocks HuSprAnimKill never freed, so every window background and other
       made sprite left two blocks behind in HEAP_DATA for good.) */
    anim = HuMemDirectMalloc(HEAP_DATA, sizeof(ANIMDATA));
    anim->bank = bank = HuMemDirectMalloc(HEAP_DATA, sizeof(ANIMBANK) + sizeof(ANIMFRAME));
    bank->frame = frame = (ANIMFRAME *)(bank + 1);
    anim->pat = pat = HuMemDirectMalloc(HEAP_DATA, sizeof(ANIMPAT) + sizeof(ANIMLAYER));
    pat->layer = layer = (ANIMLAYER *)(pat + 1);
    anim->bmp = bmp = HuMemDirectMalloc(HEAP_DATA, sizeof(ANIMBMP));
    anim->valid = ANIM_DATA_ALLOCATION_VALID;
#else
    anim = new_anim = HuMemDirectMalloc(HEAP_DATA, sizeof(ANIMDATA)+sizeof(ANIMBANK)+sizeof(ANIMFRAME)
                                            +sizeof(ANIMPAT)+sizeof(ANIMLAYER)+sizeof(ANIMBMP));

    bank = temp = &new_anim[1];
    anim->bank = bank;
    frame = temp = ((char *)temp+sizeof(ANIMBANK));
    bank->frame = frame;
    pat = temp = ((char *)temp+sizeof(ANIMFRAME));
    anim->pat = pat;
    layer = temp = ((char *)temp+sizeof(ANIMPAT));
    pat->layer = layer;
    bmp = temp = ((char *)temp+sizeof(ANIMLAYER));
    anim->bmp = bmp;
#endif
    anim->useNum = 0;
    anim->bankNum = 1;
    anim->patNum = 1;
    anim->bmpNum = (1|ANIM_BMP_ALLOC);
    bank->timeNum = 1;
    bank->unk = 10;
    frame->pat = 0;
    frame->time = 10;
    frame->shiftX = frame->shiftY = frame->flip = 0;
    pat->layerNum = 1;
    pat->centerX = sizeX/2;
    pat->centerY = sizeY/2;
    pat->sizeX = sizeX;
    pat->sizeY = sizeY;
    layer->alpha = 255;
    layer->flip = 0;
    layer->bmpNo = 0;
    layer->startX = layer->startY = 0;
    layer->sizeX = sizeX;
    layer->sizeY = sizeY;
    layer->shiftX = layer->shiftY = 0;
    layer->vtx[0] = layer->vtx[1] = 0;
    layer->vtx[2] = sizeX;
    layer->vtx[3] = 0;
    layer->vtx[4] = sizeX;
    layer->vtx[5] = sizeY;
    layer->vtx[6] = 0;
    layer->vtx[7] = sizeY;
    bmp->pixSize = bitSizeTbl[dataFmt];
    bmp->dataFmt = dataFmt;
    bmp->palNum = 0;
    bmp->sizeX = sizeX;
    bmp->sizeY = sizeY;
    bmp->dataSize = sizeX*sizeY*bitSizeTbl[dataFmt]/8;
    bmp->palData = NULL;
    bmp->data = NULL;
#ifdef OPTIMIZED_TEXTURE_LOADING
    int i;
    for (i = 0; i < sizeof(bmp->texData) / sizeof(bmp->texData[0]); i++) {
        AnimTexData *tex = &bmp->texData[i];
        tex->tex_initialized = FALSE;
        tex->tlut_initialized = FALSE;
        tex->wrap_s = tex->wrap_t = GX_CLAMP;
        memset(&tex->tex_obj, 0, sizeof(tex->tex_obj));
        memset(&tex->tlut_obj, 0, sizeof(tex->tlut_obj));
    }
#endif
    return anim;
}

void HuSprBGSet(s16 group, s16 member,  ANIMDATA *bg, s16 bg_bank)
{
    s16 sprite = HuSprGrpData[group].members[member];
    HuSprSprBGSet(sprite, bg, bg_bank);
}

void HuSprSprBGSet(s16 sprite, ANIMDATA *bg, s16 bg_bank)
{
    HUSPRITE *sprite_ptr = &HuSprData[sprite];
    sprite_ptr->bg = bg;
    sprite_ptr->bgBank = bg_bank;
    sprite_ptr->wrapT = sprite_ptr->wrapS = GX_REPEAT;
    sprite_ptr->attr &= ~HUSPR_ATTR_LINEAR;
}

void AnimDebug(ANIMDATA *anim)
{
    ANIMPAT *pat;
    ANIMLAYER *layer;
    s16 i;
    s16 j;
    ANIMFRAME *frame;
    ANIMBANK *bank;
    ANIMBMP *bmp;
    
    OSReport("patNum %d,bankNum %d,bmpNum %d\n", anim->patNum, anim->bankNum, anim->bmpNum & ANIM_BMP_NUM_MASK);
    pat = anim->pat;
    for(i=0; i<anim->patNum; i++) {
        OSReport("PATTERN%d:\n", i);
        OSReport("\tlayerNum %d,center (%d,%d),size (%d,%d)\n", pat->layerNum, pat->centerX, pat->centerX, pat->sizeX, pat->sizeY);
        layer = pat->layer;
        for(j=0; j<pat->layerNum; j++) {
            OSReport("\t\tfileNo %d,flip %x\n", layer->bmpNo, layer->flip);
            OSReport("\t\tstart (%d,%d),size (%d,%d),shift (%d,%d)\n", layer->startX, layer->startY, layer->sizeX, layer->sizeY, layer->shiftX, layer->shiftY);
            if(j != pat->layerNum-1) {
                OSReport("\n");
            }
            layer++;
        }
        pat++;
    }
    bank = anim->bank;
    for(i=0; i<anim->bankNum; i++) {
        OSReport("BANK%d:\n", i);
        OSReport("\ttimeNum %d\n", bank->timeNum);
        frame = bank->frame;
        for(j=0; j<bank->timeNum; j++) {
            OSReport("\t\tpat %d,time %d,shift(%d,%d),flip %x\n", frame->pat, frame->time, frame->shiftX, frame->shiftY, frame->flip);
            frame++;
        }
        bank++;
    }
    bmp = anim->bmp;
#ifdef NON_MATCHING
    for(i = 0; i < (anim->bmpNum & ANIM_BMP_NUM_MASK); i++) {
#else
    for(i = 0; (i < anim->bmpNum) & ANIM_BMP_NUM_MASK; i++) {
#endif
        OSReport("BMP%d:\n", i);
        OSReport("\tpixSize %d,palNum %d,size (%d,%d)\n", bmp->pixSize, bmp->palNum, bmp->sizeX, bmp->sizeY);
        bmp++;
    }
}

#ifdef TARGET_PC
#include "port/rollback_scene.h"
bool PartyBoard_RollbackSpriteRegions(PartyBoardRollbackRegionSink sink, void *context)
{
    if (!sink) return false;
    if (!sink(context, &HuSprData, sizeof(HuSprData))) return false;
    if (!sink(context, &HuSprGrpData, sizeof(HuSprGrpData))) return false;
    if (!sink(context, &HuSprOrder, sizeof(HuSprOrder))) return false;
    if (!sink(context, &HuSprOrderNum, sizeof(HuSprOrderNum))) return false;
    if (!sink(context, &HuSprOrderNo, sizeof(HuSprOrderNo))) return false;
    if (!sink(context, &HuSprPauseF, sizeof(HuSprPauseF))) return false;
    return true;
}
#endif
