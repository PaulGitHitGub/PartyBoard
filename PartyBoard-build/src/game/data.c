#include "game/data.h"
#include "game/armem.h"
#include "game/process.h"
#include "dolphin/dvd.h"
#include "port/rollback_io.h"

#ifdef BYTESWAPPING
#include "port/byteswap.h"
#endif

#define STAT_ID_ARAM 0x10000

#define PTR_OFFSET(ptr, offset) (void *)(((u8 *)(ptr)+(u32)(offset)))
#define DATA_EFF_SIZE(size) (((size)+1) & ~0x1)

void **HuDataReadMultiSub(s32 *dataNum, BOOL use_num, s32 num);

#define DATA_MAX_READSTAT 128

#define DATADIR(name, path) { path, -1 },

static FileListEntry DataDirStat[] = {
    #include "datadir_table.h"
    { NULL, -1 }
};

#undef DATADIR

u32 DirDataSize;
static u32 DataDirMax;
static s32 shortAccessSleep;
static HUDATASTAT ATTRIBUTE_ALIGN(32)
#if TARGET_PC
ReadDataStat[DATA_MAX_READSTAT + 1]; // to avoid bug
#else
ReadDataStat[DATA_MAX_READSTAT];
#endif

void HuDataInit(void)
{
    s32 i = 0;
    FileListEntry *dir_stat = DataDirStat;
    HUDATASTAT *read_stat;
    while(dir_stat->name) {
        if((dir_stat->entryNum = DVDConvertPathToEntrynum(dir_stat->name)) == -1) {
            OSReport("data.c: Data File Error(%s)\n", dir_stat->name);
            OSPanic("data.c", 65, "\n");
        }
        i++;
        dir_stat++;
    }
    DataDirMax = i;
    for(i=0, read_stat = ReadDataStat; i<DATA_MAX_READSTAT; i++, read_stat++) {
        read_stat->dirId = -1;
        read_stat->used = FALSE;
        read_stat->status = 0;
    }
}

static s32 HuDataReadStatusGet(void)
{
    s32 i;
    for(i=0; i<DATA_MAX_READSTAT; i++) {
        if(ReadDataStat[i].dirId == -1) {
            break;
        }
    }
    if(i >= DATA_MAX_READSTAT) {
        i = -1;
    }
    return i;
}

s32 HuDataReadChk(s32 dirNum)
{
    s32 i;
    dirNum >>= 16;
    for(i=0; i<DATA_MAX_READSTAT; i++) {
        if(ReadDataStat[i].dirId == dirNum && ReadDataStat[i].status != 1) {
            break;
        }
    }
    if(i >= DATA_MAX_READSTAT) {
        i = HU_DATA_STAT_NONE;
    }
    return i;
}

HUDATASTAT *HuDataGetStatus(void *dirP)
{
    s32 i;
    for(i=0; i<DATA_MAX_READSTAT; i++) {
        if(ReadDataStat[i].dirP == dirP) {
            break;
        }
    }
    if(i > DATA_MAX_READSTAT) {
        return NULL;
    }
    return &ReadDataStat[i];
}

void *HuDataGetDirPtr(s32 dirNum)
{
    s32 status = HuDataReadChk(dirNum);
    if(status < 0) {
        return NULL;
    }
    return ReadDataStat[status].dirP;
}

HUDATASTAT *HuDataDirRead(s32 dataNum)
{
    HUDATASTAT *readStat;
    s32 statId;
    s32 dirId;
    dirId  = dataNum >> 16;
    if(DataDirMax <= dirId) {
        OSReport("data.c: Data Number Error(%d)\n", dataNum);
        return NULL;
    }

    if((statId = HuDataReadChk(dataNum)) < 0) {
        AMEM_PTR dirAMemP;
        if(dirAMemP = HuARDirCheck(dataNum)) {
            HuAR_ARAMtoMRAM(dirAMemP);
            while(HuARDMACheck());
            statId = HuDataReadChk(dataNum);
            readStat = &ReadDataStat[statId];
        } else {
            if((statId = HuDataReadStatusGet()) == -1) {
                OSReport("data.c: Data Work Max Error\n");
                return NULL;
            }
            readStat = &ReadDataStat[statId];
            readStat->dirP = HuDvdDataFastRead(DataDirStat[dirId].entryNum);
            if(readStat->dirP) {
                readStat->dirId = dirId;
            }
        }
    } else {
        readStat = &ReadDataStat[statId];
    }
    return readStat;
}

static HUDATASTAT *HuDataDirReadNum(s32 dataNum, s32 num)
{
    HUDATASTAT *readStat;
    s32 statId;
    s32 dirId;
    dirId  = dataNum >> 16;
    if(DataDirMax <= dirId) {
        OSReport("data.c: Data Number Error(%d)\n", dataNum);
        return NULL;
    }

    if((statId = HuDataReadChk(dataNum)) < 0) {
        u32 dir_aram;
        if((dir_aram = HuARDirCheck(dataNum))) {
            OSReport("ARAM data num %x\n", dataNum);
            HuAR_ARAMtoMRAMNum(dir_aram, num);
            while(HuARDMACheck());
            statId = HuDataReadChk(dataNum);
            readStat = &ReadDataStat[statId];
            readStat->used = TRUE;
            readStat->num = num;
        } else {
            OSReport("data num %x\n", dataNum);
            if((statId = HuDataReadStatusGet()) == -1) {
                OSReport("data.c: Data Work Max Error\n");
                return NULL;
            }
            readStat = &ReadDataStat[statId];
            readStat->dirP = HuDvdDataFastReadNum(DataDirStat[dirId].entryNum, num);
            if(readStat->dirP) {
                readStat->dirId = dirId;
                readStat->used = TRUE;
                readStat->num = num;
            }
        }
    } else {
        readStat = &ReadDataStat[statId];
    }
    return readStat;
}

HUDATASTAT *HuDataDirSet(void *dirP, s32 dataNum)
{
    HUDATASTAT *readStat = HuDataGetStatus(dirP);
    s32 statId;
    if((statId = HuDataReadChk(readStat->dirId << 16)) >= 0) {
        HuDataDirClose(dataNum);
    }
    if((statId = HuDataReadStatusGet()) == -1) {
        OSReport("data.c: Data Work Max Error\n");
        return NULL;
    } else {
        readStat = &ReadDataStat[statId];
        readStat->dirP = dirP;
        readStat->dirId = dataNum >>16;
        return readStat;
    }
}

void HuDataDirReadAsyncCallBack(s32 result, DVDFileInfo* fileInfo)
{
    HUDATASTAT *readStat;
    s32 i;
    for(i=0; i<DATA_MAX_READSTAT; i++) {
        if(ReadDataStat[i].status == 1 && ReadDataStat[i].dvdFile.startAddr == fileInfo->startAddr) {
            break;
        }
    }
    if(i >= DATA_MAX_READSTAT) {
        OSPanic("data.c", 358, "dvd.c AsyncCallBack Error");
    }
    readStat = &ReadDataStat[i];
    readStat->status = 0;
    DVDClose(&readStat->dvdFile);
    PartyBoard_RollbackIOEnd();
}

s32 HuDataDirReadAsync(s32 dataNum)
{
    HUDATASTAT *readstat;
    s32 statId;
    s32 dirId;
    dirId  = dataNum >> 16;
    if(DataDirMax <= dirId) {
        OSReport("data.c: Data Number Error(%d)\n", dataNum);
        return -1;
    }
    if((statId = HuDataReadChk(dataNum)) < 0) {
        u32 dir_aram;
        if(dir_aram = HuARDirCheck(dataNum)) {
            OSReport("ARAM data num %x\n", dataNum);
            HuAR_ARAMtoMRAM(dir_aram);
            statId = 0x10000;
        } else {
            statId = HuDataReadStatusGet();
            if(statId == -1) {
                OSReport("data.c: Data Work Max Error\n");
                return -1;
            }
            /* Keep both the launch and callback writes outside snapshots,
             * including callbacks that complete before this function returns. */
            PartyBoard_RollbackIOBegin();
            PartyBoard_RollbackIOBegin();
            readstat = &ReadDataStat[statId];
            readstat->status = 1;
            readstat->dirId = dirId;
            readstat->dirP = HuDvdDataFastReadAsync(DataDirStat[dirId].entryNum, readstat);
            PartyBoard_RollbackIOEnd();
        }
    } else {
        statId = -1;
    }
    return statId;
}

s32 HuDataDirReadNumAsync(s32 dataNum, s32 num)
{
    HUDATASTAT *readStat;
    s32 statId;
    s32 dirId;
    dirId  = dataNum >> 16;
    if(DataDirMax <= dirId) {
        OSReport("data.c: Data Number Error(%d)\n", dataNum);
        return -1;
    }
    if((statId = HuDataReadChk(dataNum)) < 0) {
        if((statId = HuDataReadStatusGet()) == -1) {
            OSReport("data.c: Data Work Max Error\n");
            return -1;
        }
        PartyBoard_RollbackIOBegin();
        PartyBoard_RollbackIOBegin();
        ReadDataStat[statId].status = TRUE;
        ReadDataStat[statId].dirId = dirId;
        readStat = &ReadDataStat[statId];
        readStat->used = TRUE;
        readStat->num = num;
        readStat->dirP = HuDvdDataFastReadAsync(DataDirStat[dirId].entryNum, readStat);
        PartyBoard_RollbackIOEnd();
    } else {
        statId = -1;
    }
    return statId;
}

BOOL HuDataGetAsyncStat(s32 statId)
{
    if(statId == STAT_ID_ARAM) {
        return HuARDMACheck() == 0;
    } else {
        return ReadDataStat[statId].status == 0;
    }
}

static void GetFileInfo(HUDATASTAT *readStat, s32 fileNum)
{
    u32 *temp_ptr = (u32 *)PTR_OFFSET(readStat->dirP, (fileNum * 4))+1;
#ifdef BYTESWAPPING
    u32 ofs = *temp_ptr;
    byteswap_u32(&ofs);
    temp_ptr = &ofs;
#endif
    readStat->fileDataP = PTR_OFFSET(readStat->dirP, *temp_ptr);
    temp_ptr = readStat->fileDataP;
    readStat->rawLen = *temp_ptr++;
    readStat->decodeType = *temp_ptr++;
    readStat->fileDataP = temp_ptr;
#ifdef BYTESWAPPING
    byteswap_u32(&readStat->rawLen);
    byteswap_u32(&readStat->decodeType);
#endif
}

void *HuDataRead(s32 dataNum)
{
    HUDATASTAT *readStat;
    s32 statId;
    void *buf;
    if(!HuDataDirRead(dataNum)) {
        (void)dataNum;
        return NULL;
    }
    if((statId = HuDataReadChk(dataNum)) == -1) {
        return NULL;
    }
    readStat = &ReadDataStat[statId];
    GetFileInfo(readStat, dataNum & 0xFFFF);
    buf = HuMemDirectMalloc(0, DATA_EFF_SIZE(readStat->rawLen));
    if(buf) {
        HuDecodeData(readStat->fileDataP, buf, readStat->rawLen, readStat->decodeType);
    }
    return buf;
}

void *HuDataReadNum(s32 dataNum, s32 num)
{
    HUDATASTAT *readStat;
    s32 statId;
    void *buf;
    if(!HuDataDirReadNum(dataNum, num)) {
        return NULL;
    }
    if((statId = HuDataReadChk(dataNum)) == -1) {
        return NULL;
    }
    readStat = &ReadDataStat[statId];
    GetFileInfo(readStat, dataNum & 0xFFFF);
#ifdef TARGET_PC
    // TODO PC why is the allocation invalid if we use HEAP_SYSTEM?
    buf = HuMemDirectMallocNum(HEAP_DATA, DATA_EFF_SIZE(readStat->rawLen), num);
#else
    buf = HuMemDirectMallocNum(HEAP_SYSTEM, DATA_EFF_SIZE(readStat->rawLen), num);
#endif
    if(buf) {
        HuDecodeData(readStat->fileDataP, buf, readStat->rawLen, readStat->decodeType);
    }
    return buf;
}

void *HuDataSelHeapRead(s32 dataNum, HeapID heap)
{
    HUDATASTAT *readStat;
    s32 statId;
    void *buf;
    if(!HuDataDirRead(dataNum)) {
        return NULL;
    }
    if((statId = HuDataReadChk(dataNum)) == -1) {
        return NULL;
    }
    readStat = &ReadDataStat[statId];
    GetFileInfo(readStat, dataNum & 0xFFFF);
    switch(heap) {
        case HEAP_MUSIC:
            buf = HuMemDirectMalloc(HEAP_MUSIC, DATA_EFF_SIZE(readStat->rawLen));
            break;

        case HEAP_DATA:
            buf = HuMemDirectMalloc(HEAP_DATA, DATA_EFF_SIZE(readStat->rawLen));
            break;

        case HEAP_DVD:
            buf = HuMemDirectMalloc(HEAP_DVD, DATA_EFF_SIZE(readStat->rawLen));
            break;

        default:
            buf = HuMemDirectMalloc(HEAP_SYSTEM, DATA_EFF_SIZE(readStat->rawLen));
            break;
    }
    if(buf) {
        HuDecodeData(readStat->fileDataP, buf, readStat->rawLen, readStat->decodeType);
    }
    return buf;
}

void *HuDataSelHeapReadNum(s32 dataNum, s32 num, HeapID heap)
{
    HUDATASTAT *readStat;
    s32 statId;
    void *buf;
    if(!HuDataDirReadNum(dataNum, num)) {
        return NULL;
    }
    if((statId = HuDataReadChk(dataNum)) == -1) {
        return NULL;
    }
    readStat = &ReadDataStat[statId];
    GetFileInfo(readStat, dataNum & 0xFFFF);
    switch(heap) {
        case HEAP_MUSIC:
            buf = HuMemDirectMalloc(HEAP_MUSIC, DATA_EFF_SIZE(readStat->rawLen));
            break;

        case HEAP_DATA:
            buf = HuMemDirectMallocNum(HEAP_DATA, DATA_EFF_SIZE(readStat->rawLen), num);
            break;

        case HEAP_DVD:
            buf = HuMemDirectMallocNum(HEAP_DVD, DATA_EFF_SIZE(readStat->rawLen), num);
            break;

        default:
            buf = HuMemDirectMallocNum(HEAP_SYSTEM, DATA_EFF_SIZE(readStat->rawLen), num);
            break;
    }
    if(buf) {
        HuDecodeData(readStat->fileDataP, buf, readStat->rawLen, readStat->decodeType);
    }
    return buf;
}

void **HuDataReadMulti(s32 *dataNum)
{
    return HuDataReadMultiSub(dataNum, FALSE, 0);
}

void **HuDataReadMultiSub(s32 *dataNum, BOOL use_num, s32 num)
{
    s32 *dirIds;
    char **pathTbl;
    void **dirP;
    void **outList;
    s32 i, count, numFiles;
    u32 dirId;
    for(i=0, count=0; dataNum[i] != HU_DATANUM_NONE; i++) {
        dirId = dataNum[i] >> 16;
        if(DataDirMax <= dirId) {
            OSReport("data.c: Data Number Error(%d)\n", dataNum[i]);
            return NULL;
        }
        if(HuDataReadChk(dataNum[i]) < 0) {
            count++;
        }
    }
    numFiles = i;
    dirIds = HuMemDirectMalloc(HEAP_SYSTEM, (count+1)*sizeof(s32));
    for(i=0; i<count+1; i++) {
        dirIds[i] = HU_DATANUM_NONE;
    }
    pathTbl = HuMemDirectMalloc(HEAP_SYSTEM, (count+1)*sizeof(char *));
    for(i=0, count=0; dataNum[i] != HU_DATANUM_NONE; i++) {
        dirId = dataNum[i] >> 16;
        if(HuDataReadChk(dataNum[i]) < 0) {
            s32 j;
            for(j=0; dirIds[j] != HU_DATANUM_NONE; j++) {
                if(dirIds[j] == dirId){
                    break;
                }
            }
            if(dirIds[j] == HU_DATANUM_NONE) {
                dirIds[j] = dirId;
                pathTbl[count++] = DataDirStat[dirId].name;
            }
        }
    }
    dirP = HuDvdDataReadMulti(pathTbl);
    for(i=0; dirIds[i] != HU_DATANUM_NONE; i++) {
        s32 status;
        if((status = HuDataReadStatusGet()) == HU_DATA_STAT_NONE) {
            OSReport("data.c: Data Work Max Error\n");
            (void)count; //HACK to match HuDataReadMultiSub
            HuMemDirectFree(dirIds);
            HuMemDirectFree(pathTbl);
            return NULL;
        } else {
            ReadDataStat[status].dirP = dirP[i];
            ReadDataStat[status].dirId = dirIds[i];
        }
    }
    HuMemDirectFree(dirIds);
    HuMemDirectFree(pathTbl);
    HuMemDirectFree(dirP);
    if(use_num) {
        outList = HuMemDirectMallocNum(HEAP_SYSTEM, (numFiles+1)*sizeof(void *), num);
    } else {
        outList = HuMemDirectMalloc(HEAP_SYSTEM, (numFiles+1)*sizeof(void *));
    }
    for(i=0; dataNum[i] != HU_DATANUM_NONE; i++) {
        if(use_num) {
            outList[i] = HuDataReadNum(dataNum[i], num);
        } else {
            outList[i] = HuDataRead(dataNum[i]);
        }
    }
    outList[i] = NULL;
    return outList;
}

s32 HuDataGetSize(s32 dataNum)
{
    HUDATASTAT *readStat;
    s32 statId;
    if((statId = HuDataReadChk(dataNum)) == HU_DATA_STAT_NONE) {
        return -1;
    }
    readStat = &ReadDataStat[statId];
    GetFileInfo(readStat, dataNum & 0xFFFF);
    return DATA_EFF_SIZE(readStat->rawLen);
}

void HuDataClose(void *ptr)
{
    if(ptr) {
        HuMemDirectFree(ptr);
    }
}

void HuDataCloseMulti(void **ptrs)
{
    s32 i;
    for(i=0; ptrs[i]; i++) {
        void *ptr = ptrs[i];
        if(ptr) {
            HuMemDirectFree(ptr);
        }
    }
    if(ptrs) {
        HuMemDirectFree(ptrs);
    }
}

void HuDataDirClose(s32 dataNum)
{
    HUDATASTAT *readStat;
    s32 i;
    s32 dirId = dataNum >> 16;
    for(i=0; i<DATA_MAX_READSTAT; i++) {
        if(ReadDataStat[i].dirId == dirId) {
            break;
        }
    }
    if(i >= DATA_MAX_READSTAT) {
        return;
    }
    readStat = &ReadDataStat[i];
    if(readStat->status == 1) {
        OSPanic("data.c", 812, "data.c: Async Close Error\n");
    }
    readStat->dirId = HU_DATANUM_NONE;
    HuDvdDataClose(readStat->dirP);
    readStat->dirP = NULL;
    readStat->used = FALSE;
    readStat->status = 0;
}

void HuDataDirCloseNum(s32 num)
{
    s32 i;
    for(i=0; i<DATA_MAX_READSTAT; i++) {
        if(ReadDataStat[i].used == TRUE && ReadDataStat[i].num == num) {
            HuDataDirClose(ReadDataStat[i].dirId << 16);
        }
    }
}

static s32 HuDataDVDdirDirectOpen(s32 dataNum, DVDFileInfo *fileInfo)
{
	s32 dir = dataNum >> 16;
	if(dir >= (s32)DataDirMax) {
		OSReport("data.c: Data Number Error(0x%08x)\n", dataNum);
		return 0;
	}
	if(!DVDFastOpen(DataDirStat[dir].entryNum, fileInfo)) {
		char panic_str[48];
		sprintf(panic_str, "HuDataDVDdirDirectOpen: File Open Error(%08x)", dataNum);
		OSPanic("data.c", 895, panic_str);
	}
	return 1;
}

static s32 HuDataDVDdirDirectRead(DVDFileInfo *fileInfo, void *dest, s32 len, s32 offset)
{
	s32 result = DVDReadAsync(fileInfo, dest, len, offset, NULL);
	if(result != 1) {
		OSPanic("data.c", 904, "HuDataDVDdirDirectRead: File Read Error");
	}
	while(DVDGetCommandBlockStatus(&fileInfo->cb)) {
		if(shortAccessSleep) {
			HuPrcVSleep();
		}
	}
	return result;
}

static void *HuDataDecodeIt(void *bufP, s32 bufOfs, s32 num, HeapID heap)
{
	void *dataStart;
	s32 *buf;
	s32 rawLen, decodeType;

	void *dest;
	buf =  (s32 *)((u8 *)bufP+bufOfs);
	if((uintptr_t)buf & 0x3) {
		u8 *data = (u8 *)buf;
		rawLen = *data++ << 24;
		rawLen += *data++ << 16;
		rawLen += *data++ << 8;
		rawLen += *data++;
		decodeType = *data++ << 24;
		decodeType += *data++ << 16;
		decodeType += *data++ << 8;
		decodeType += *data++;
		dataStart = data;
	} else {
		s32 *data = buf;
		rawLen = *data++;
		decodeType = *data++;
		dataStart = data;
#ifdef TARGET_PC
        byteswap_s32(&rawLen);
        byteswap_s32(&decodeType);
#endif
	}
	switch(heap) {
        case HEAP_MUSIC:
            dest = HuMemDirectMalloc(HEAP_MUSIC, DATA_EFF_SIZE(rawLen));
            break;

        case HEAP_DATA:
            dest = HuMemDirectMallocNum(HEAP_DATA, DATA_EFF_SIZE(rawLen), num);
            break;

        case HEAP_DVD:
            dest = HuMemDirectMallocNum(HEAP_DVD, DATA_EFF_SIZE(rawLen), num);
            break;

        default:
            dest = HuMemDirectMallocNum(HEAP_SYSTEM, DATA_EFF_SIZE(rawLen), num);
            break;
    }
    if(dest) {
        HuDecodeData(dataStart, dest, rawLen, decodeType);
    }
    return dest;
}

void *HuDataReadNumHeapShortForce(s32 dataNum, s32 num, HeapID heap)
{
    DVDFileInfo fileInfo;
    s32 *dataHdr;
    s32 *fileData;
    void *fileBuf;
    s32 readLen;
    s32 fileNum;
    s32 fileOfs;
    s32 readOfs;
    s32 dataOfs;
    void *ret;
    s32 dir;
    s32 dvdLen;
    s32 fileNumMax;

    if(!HuDataDVDdirDirectOpen(dataNum, &fileInfo)) {
	    return NULL;
    }
    dir = (dataNum >> 16) & 0xFFFF0000;
    fileNum = dataNum & 0xFFFF;
    fileOfs = (fileNum*4)+4;
    dvdLen = OSRoundUp32B(fileOfs+8);
    fileData = HuMemDirectMalloc(HEAP_SYSTEM, dvdLen);
    if(!HuDataDVDdirDirectRead(&fileInfo, fileData, dvdLen, 0)) {
        HuMemDirectFree(fileData);
        DVDClose(&fileInfo);
        return NULL;
    }
    fileNumMax = *fileData;
#ifdef TARGET_PC
    byteswap_s32(&fileNumMax);
#endif
    if(fileNumMax <= fileNum) {
        HuMemDirectFree(fileData);
        OSReport("data.c%d: Data Number Error(0x%08x)\n", 1005, dataNum);
        DVDClose(&fileInfo);
        return NULL;
    }
    dataHdr = fileData;
    dataHdr += fileNum+1;
    fileOfs = *dataHdr;
#ifdef TARGET_PC
    byteswap_s32(&fileOfs);
#endif
    readOfs = OSRoundDown32B(fileOfs);
    if(fileNumMax <= fileNum+1) {
        readLen = fileInfo.length;
        dataOfs = readLen-readOfs;
    } else {
        dataHdr++;
#ifdef TARGET_PC
        dataOfs = *dataHdr;
        byteswap_s32(&dataOfs);
        dataOfs -= readOfs;
#else
        dataOfs = (*dataHdr)-readOfs;
#endif
        readLen = fileInfo.length;
    }
    readLen = OSRoundUp32B(dataOfs);
    HuMemDirectFree(fileData);
    fileBuf = HuMemDirectMalloc(HEAP_SYSTEM, (readLen+4) & ~0x3);
    if(fileBuf == NULL) {
        OSReport("data.c: couldn't allocate read buffer(0x%08x)\n", dataNum);
        DVDClose(&fileInfo);
        return NULL;
    }
    if(!HuDataDVDdirDirectRead(&fileInfo, fileBuf, readLen, readOfs)) {
        HuMemDirectFree(fileBuf);
        DVDClose(&fileInfo);
        return NULL;
    }
    DVDClose(&fileInfo);
    dataOfs = fileOfs-readOfs;
    ret = HuDataDecodeIt(fileBuf, dataOfs, num, heap);
    HuMemDirectFree(fileBuf);
    return ret;
}

char lbl_8011FDA6[] = "** dcnt %d tmp %08x sp1 %08x\n";
char lbl_8011FDC4[] = "** dcnt %d lastNum %08x\n";

#if defined(PARTYBOARD_HD_BETA)
#include "game/sprite.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dolphin/gx/GXAurora.h>
// Test aid (PARTYBOARD_DUMP_ALL_SPRITES=1 with PARTYBOARD_DUMP_TEXTURES=1):
// opens every archive on the disc, and every entry that is a sprite file
// (checked strictly against the sprite header layout) is loaded and each of
// its images written out under its replacement name, with a log line naming
// the archive and entry. Used to find every button prompt image.
static u32 be32(const u8 *p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }
static s16 be16(const u8 *p) { return (s16)((p[0] << 8) | p[1]); }

static BOOL looksLikeAnim(const u8 *d, s32 size)
{
    s32 bank, pat, bmp, i;
    s16 bankNum, patNum, bmpNum;
    if (size < 0x14) return FALSE;
    bankNum = be16(d); patNum = be16(d + 2); bmpNum = be16(d + 4);
    if (bankNum < 1 || bankNum > 2048 || patNum < 1 || patNum > 8192 || bmpNum < 1 || bmpNum > 1024) return FALSE;
    bank = be32(d + 8); pat = be32(d + 12); bmp = be32(d + 16);
    if (pat != 0x14 || bank < 0x14 || bank >= size || bmp < 0x14 || bmp + bmpNum * 20 > size) return FALSE;
    for (i = 0; i < bmpNum; i++) {
        const u8 *b = d + bmp + i * 20;
        s16 w = be16(b + 4), h = be16(b + 6);
        u32 dataSize = be32(b + 8), data = be32(b + 16);
        u8 fmt = b[1] & 0xF;
        if (w < 1 || w > 1024 || h < 1 || h > 1024 || fmt > 11) return FALSE;
        if (data == 0 || data + dataSize > (u32)size || dataSize == 0) return FALSE;
    }
    return TRUE;
}

#include "game/hsfload.h"
#include "game/hsfformat.h"
// Same, for the colour textures inside 3D model (HSF) files.
static void PartyBoard_DumpAllModelTextures(void)
{
    u32 dir;
    s32 models = 0, images = 0;
    const char *fromEnv = getenv("PARTYBOARD_DUMP_DIR_FROM"), *toEnv = getenv("PARTYBOARD_DUMP_DIR_TO");
    u32 from = fromEnv ? (u32)atoi(fromEnv) : 0, to = toEnv ? (u32)atoi(toEnv) : DataDirMax;
    for (dir = from; dir < DataDirMax && dir < to; dir++) {
        HUDATASTAT *stat = HuDataDirRead(dir << 16);
        u32 count, f;
        if (stat == NULL || stat->dirP == NULL) continue;
        count = be32((const u8 *)stat->dirP);
        if (count > 4096) { HuDataDirClose(dir << 16); continue; }
        for (f = 0; f < count; f++) {
            s32 num = (dir << 16) | f;
            s32 size = HuDataGetSize(num);
            u8 *raw;
            HSFDATA *hsf;
            s16 i;
            if (getenv("PARTYBOARD_DUMP_DEBUG")) { fprintf(stderr, "[MDLDBG] read %u size=%d\n", f, size); fflush(stderr); }
            if (size <= 0x40 || size > 0x2000000) continue;
            raw = HuDataReadNum(num, MEMORY_DEFAULT_NUM);
            if (raw == NULL) continue;
            if (memcmp(raw, "HSFV037", 7) != 0) { HuMemDirectFree(raw); continue; }
            if (getenv("PARTYBOARD_DUMP_DEBUG")) { fprintf(stderr, "[MDLDBG] %s %u size=%d\n", DataDirStat[dir].name, f, size); fflush(stderr); }
            hsf = LoadHSF(raw);
            models++;
            for (i = 0; hsf && i < hsf->bitmapNum; i++) {
                HSFBITMAP *b = &hsf->bitmap[i];
                GXTexObj obj;
                GXTlutObj tlut;
                char name[128];
                switch (b->dataFmt) {
                    case 4: GXInitTexObj(&obj, b->data, b->sizeX, b->sizeY, GX_TF_RGB565, GX_CLAMP, GX_CLAMP, GX_FALSE); break;
                    case 5: GXInitTexObj(&obj, b->data, b->sizeX, b->sizeY, GX_TF_RGB5A3, GX_CLAMP, GX_CLAMP, GX_FALSE); break;
                    case 6: GXInitTexObj(&obj, b->data, b->sizeX, b->sizeY, GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, GX_FALSE); break;
                    case 7: GXInitTexObj(&obj, b->data, b->sizeX, b->sizeY, GX_TF_CMPR, GX_CLAMP, GX_CLAMP, GX_FALSE); break;
                    case 9: case 10:
                        GXInitTlutObj(&tlut, b->palData, b->dataFmt == 9 ? GX_TL_RGB565 : GX_TL_RGB5A3, b->palSize);
                        GXLoadTlut(&tlut, 0);
                        GXInitTexObjCI(&obj, b->data, b->sizeX, b->sizeY, b->pixSize < 8 ? GX_TF_C4 : GX_TF_C8,
                            GX_CLAMP, GX_CLAMP, GX_FALSE, 0);
                        break;
                    default: continue;
                }
                GXAuroraProbeTexture(&obj);
                GXAuroraTextureName(&obj, name, sizeof(name));
                fprintf(stderr, "[ALLMDL] %s %u %d %dx%d fmt=%d %s\n", DataDirStat[dir].name, f, i, b->sizeX, b->sizeY,
                    b->dataFmt, name);
                images++;
            }
            if (hsf) KillHSF(hsf);
            HuMemDirectFree(raw);
        }
        HuDataDirClose(dir << 16);
    }
    fprintf(stderr, "[ALLMDL] done: %d models, %d textures\n", models, images);
}

void PartyBoard_DumpAllSprites(void)
{
    u32 dir;
    if (getenv("PARTYBOARD_DUMP_MODELS_ONLY")) { PartyBoard_DumpAllModelTextures(); return; }
    s32 files = 0, anims = 0, images = 0;
    for (dir = 0; dir < DataDirMax; dir++) {
        HUDATASTAT *stat = HuDataDirRead(dir << 16);
        u32 count, f;
        if (stat == NULL || stat->dirP == NULL) continue;
        count = be32((const u8 *)stat->dirP);
        if (count > 4096) { HuDataDirClose(dir << 16); continue; }
        for (f = 0; f < count; f++) {
            s32 num = (dir << 16) | f;
            s32 size = HuDataGetSize(num);
            u8 *raw;
            files++;
            if (getenv("PARTYBOARD_DUMP_DEBUG") && strstr(DataDirStat[dir].name, "/win.bin") && f < 6) {
                u8 *dbg = HuDataRead(num); int k;
                fprintf(stderr, "[DBG] %s %u size=%d:", DataDirStat[dir].name, f, size);
                for (k = 0; dbg && k < 0x30; k++) fprintf(stderr, " %02x", dbg[k]);
                fprintf(stderr, "\n");
                if (dbg) HuMemDirectFree(dbg);
            }
            if (size <= 0 || size > 0x2000000) continue;
            raw = HuDataRead(num);
            if (raw == NULL) continue;
            if (looksLikeAnim(raw, size)) {
                ANIMDATA *anim = HuSprAnimRead(raw);
                s16 i;
                anims++;
                for (i = 0; i < anim->bmpNum; i++) {
                    char name[128];
                    HuSprTexLoad(anim, i, 7, GX_CLAMP, GX_CLAMP, GX_LINEAR);
                    GXAuroraProbeTexture(&anim->bmp[i].texData[7].tex_obj);
                    GXAuroraTextureName(&anim->bmp[i].texData[7].tex_obj, name, sizeof(name));
                    fprintf(stderr, "[ALLSPR] %s %u %d %dx%d fmt=%d %s\n", DataDirStat[dir].name, f, i,
                        anim->bmp[i].sizeX, anim->bmp[i].sizeY, anim->bmp[i].dataFmt & 0xF, name);
                    images++;
                }
                HuSprAnimKill(anim);
            }
            HuMemDirectFree(raw);
        }
        HuDataDirClose(dir << 16);
    }
    fprintf(stderr, "[ALLSPR] done: %d entries, %d sprite files, %d images\n", files, anims, images);
    PartyBoard_DumpAllModelTextures();
}

/* Test aid (PARTYBOARD_TEST_LOADERS=1, see main.c): loads every sprite file and
   every model/motion (HSF) file on the disc the way the game does and checks
   the PC conversion of each:
   - sprite files are converted twice from the same buffer; both results must
     match an independent big-endian reading of the file, and the file must be
     unchanged apart from the converted mark in its unused use-count field;
   - models are loaded with byteswap tracking on, which reports any byte of
     the file the loader converts in place more than once (that swaps it back).
   It also records file layouts the old in-place sprite code got wrong (frames
   or layers shared between banks/patterns were converted twice). One
   "[loadertest]" line per problem and a summary; the game exits afterwards.
   PARTYBOARD_TEST_LOADERS_FROM / _TO limit it to a range of archives. */
typedef struct {
    u32 ofs;
    u32 len;
} LoaderTestRange;

static int LoaderTestRangeCmp(const void *a, const void *b)
{
    const LoaderTestRange *x = a, *y = b;
    if (x->ofs != y->ofs) return x->ofs < y->ofs ? -1 : 1;
    return x->len < y->len ? -1 : x->len > y->len;
}

/* Sorts the ranges; returns TRUE if any two (non-empty) overlap. *inOrder is
   cleared unless every range starts where the one before (in file order of the
   entries) ended. */
static BOOL LoaderTestOverlap(LoaderTestRange *ranges, s32 num, BOOL *inOrder)
{
    s32 i;
    u32 end = 0;
    BOOL first = TRUE;
    for (i = 1; i < num; i++) {
        if (ranges[i].len != 0 && ranges[i - 1].len != 0 && ranges[i].ofs != ranges[i - 1].ofs + ranges[i - 1].len) {
            *inOrder = FALSE;
        }
    }
    qsort(ranges, num, sizeof(*ranges), LoaderTestRangeCmp);
    for (i = 0; i < num; i++) {
        if (ranges[i].len == 0) continue;
        if (!first && ranges[i].ofs < end) return TRUE;
        if (first || ranges[i].ofs + ranges[i].len > end) end = ranges[i].ofs + ranges[i].len;
        first = FALSE;
    }
    return FALSE;
}

/* Compares a converted sprite file with the file read directly (big-endian). */
static const char *LoaderTestAnimCheck(const u8 *file, s32 size, ANIMDATA *anim)
{
    s32 i, j;
    s16 bankNum = be16(file), patNum = be16(file + 2), bmpNum = be16(file + 4);
    u32 bankOfs = be32(file + 8), patOfs = be32(file + 12), bmpOfs = be32(file + 16);
    if (anim->bankNum != bankNum || anim->patNum != patNum || anim->bmpNum != bmpNum) return "counts differ";
    if (bankOfs + bankNum * 8 > (u32)size || patOfs + patNum * 16 > (u32)size) return "tables outside the file";
    for (i = 0; i < bankNum; i++) {
        const u8 *b = file + bankOfs + i * 8;
        s16 timeNum = be16(b);
        u32 frameOfs = be32(b + 4);
        ANIMBANK *bank = &anim->bank[i];
        if (bank->timeNum != timeNum || bank->unk != be16(b + 2)) return "bank differs";
        if (timeNum < 0 || frameOfs + timeNum * 12 > (u32)size) return "frames outside the file";
        for (j = 0; j < timeNum; j++) {
            const u8 *f = file + frameOfs + j * 12;
            ANIMFRAME *fr = &bank->frame[j];
            if (fr->pat != be16(f) || fr->time != be16(f + 2) || fr->shiftX != be16(f + 4) || fr->shiftY != be16(f + 6)
                || fr->flip != be16(f + 8) || fr->pad != be16(f + 10)) {
                return "frame differs";
            }
        }
    }
    for (i = 0; i < patNum; i++) {
        const u8 *p = file + patOfs + i * 16;
        s16 layerNum = be16(p);
        u32 layerOfs = be32(p + 12);
        ANIMPAT *pat = &anim->pat[i];
        if (pat->layerNum != layerNum || pat->centerX != be16(p + 2) || pat->centerY != be16(p + 4)
            || pat->sizeX != be16(p + 6) || pat->sizeY != be16(p + 8)) {
            return "pattern differs";
        }
        if (layerNum < 0 || layerOfs + layerNum * 32 > (u32)size) return "layers outside the file";
        for (j = 0; j < layerNum; j++) {
            const u8 *l = file + layerOfs + j * 32;
            ANIMLAYER *layer = &pat->layer[j];
            s32 k;
            if (layer->alpha != l[0] || layer->flip != l[1] || layer->bmpNo != be16(l + 2) || layer->startX != be16(l + 4)
                || layer->startY != be16(l + 6) || layer->sizeX != be16(l + 8) || layer->sizeY != be16(l + 10)
                || layer->shiftX != be16(l + 12) || layer->shiftY != be16(l + 14)) {
                return "layer differs";
            }
            for (k = 0; k < 8; k++) {
                if (layer->vtx[k] != be16(l + 16 + k * 2)) return "layer vertex differs";
            }
        }
    }
    for (i = 0; i < bmpNum; i++) {
        const u8 *m = file + bmpOfs + i * 20;
        ANIMBMP *bmp = &anim->bmp[i];
        if (bmp->pixSize != m[0] || bmp->dataFmt != m[1] || bmp->palNum != be16(m + 2) || bmp->sizeX != be16(m + 4)
            || bmp->sizeY != be16(m + 6) || bmp->dataSize != be32(m + 8)) {
            return "bitmap differs";
        }
    }
    return NULL;
}

static void LoaderTestPointers(const u8 *raw, ANIMDATA *anim, const char **why)
{
    s32 i;
    for (i = 0; i < anim->bmpNum && *why == NULL; i++) {
        const u8 *m = raw + be32(raw + 16) + i * 20;
        if ((const u8 *)anim->bmp[i].data != raw + be32(m + 16) || (const u8 *)anim->bmp[i].palData != raw + be32(m + 12)) {
            *why = "bitmap data pointer differs";
        }
    }
}

void PartyBoard_TestLoaders(void)
{
    u32 dir;
    const char *fromEnv = getenv("PARTYBOARD_TEST_LOADERS_FROM"), *toEnv = getenv("PARTYBOARD_TEST_LOADERS_TO");
    u32 from = fromEnv ? (u32)atoi(fromEnv) : 0, to = toEnv ? (u32)atoi(toEnv) : DataDirMax;
    s32 anims = 0, animOk = 0, animUnchanged = 0, animSharedFrames = 0, animSharedLayers = 0, animFrameOrder = 0;
    s32 animLayerOrder = 0, animUseNumOther = 0, models = 0, modelsTwice = 0, modelMagic = 0, other = 0;
    size_t bytesTwice = 0, bytesSwapped = 0, animBytesSwapped = 0;
    BOOL markChecked = FALSE;

    OSReport("[loadertest] start: archives %u to %u\n", from, to < DataDirMax ? to : DataDirMax);
    {
        /* The tracker itself: one word converted twice must be reported. */
        u32 words[2] = { 1, 2 };
        size_t swapped, first, twice;
        byteswap_track_begin(words, sizeof(words));
        byteswap_u32(&words[0]);
        byteswap_u32(&words[1]);
        byteswap_u32(&words[1]);
        twice = byteswap_track_end(&swapped, &first);
        OSReport("[loadertest] tracker check: %s (%lu of %lu bytes twice, at %lu)\n",
            (twice == 4 && swapped == 8 && first == 4) ? "PASS" : "FAIL", (unsigned long)twice,
            (unsigned long)swapped, (unsigned long)first);
    }
    for (dir = from; dir < DataDirMax && dir < to; dir++) {
        HUDATASTAT *stat = HuDataDirRead(dir << 16);
        u32 count, f;
        if (stat == NULL || stat->dirP == NULL) continue;
        count = be32((const u8 *)stat->dirP);
        if (count > 4096) { HuDataDirClose(dir << 16); continue; }
        for (f = 0; f < count; f++) {
            s32 num = (dir << 16) | f;
            s32 size = HuDataGetSize(num);
            u8 *raw;
            if (size <= 0 || size > 0x2000000) continue;
            raw = HuDataReadNum(num, MEMORY_DEFAULT_NUM);
            if (raw == NULL) continue;
            if (looksLikeAnim(raw, size)) {
                u8 *copy = malloc(size);
                ANIMDATA *a, *b;
                const char *why;
                s16 bankNum = be16(raw), patNum = be16(raw + 2);
                LoaderTestRange *ranges = malloc(sizeof(LoaderTestRange) * (bankNum > patNum ? bankNum : patNum));
                BOOL inOrder = TRUE;
                s32 i;
                anims++;
                memcpy(copy, raw, size);
                if (raw[6] != 0x41 || raw[7] != 0x00) {
                    /* Every file seen holds 0x4100 here; the converted mark
                       ("PB") must never be a real value. */
                    animUseNumOther++;
                    OSReport("[loadertest] sprite %s %u: use-count field is 0x%02x%02x, not 0x4100%s\n",
                        DataDirStat[dir].name, f, raw[6], raw[7],
                        (raw[6] == 'P' && raw[7] == 'B') ? " (the converted mark is unsound)" : "");
                }
                for (i = 0; i < bankNum; i++) {
                    const u8 *bk = raw + be32(raw + 8) + i * 8;
                    ranges[i].ofs = be32(bk + 4);
                    ranges[i].len = be16(bk) > 0 ? be16(bk) * 12 : 0;
                }
                if (LoaderTestOverlap(ranges, bankNum, &inOrder)) {
                    animSharedFrames++;
                    OSReport("[loadertest] sprite %s %u: banks share frames (the old in-place code converted them "
                             "twice)\n", DataDirStat[dir].name, f);
                }
                if (!inOrder) animFrameOrder++;
                inOrder = TRUE;
                for (i = 0; i < patNum; i++) {
                    const u8 *pt = raw + be32(raw + 12) + i * 16;
                    ranges[i].ofs = be32(pt + 12);
                    ranges[i].len = be16(pt) > 0 ? be16(pt) * 32 : 0;
                }
                if (LoaderTestOverlap(ranges, patNum, &inOrder)) {
                    animSharedLayers++;
                    OSReport("[loadertest] sprite %s %u: patterns share layers (the old in-place code converted "
                             "them twice)\n", DataDirStat[dir].name, f);
                }
                if (!inOrder) animLayerOrder++;
                free(ranges);
                byteswap_track_begin(raw, size);
                a = HuSprAnimRead(raw);
                b = HuSprAnimRead(raw);
                {
                    size_t swapped;
                    byteswap_track_end(&swapped, NULL);
                    animBytesSwapped += swapped;
                }
                why = LoaderTestAnimCheck(copy, size, a);
                if (why == NULL) LoaderTestPointers(raw, a, &why);
                if (why == NULL) why = LoaderTestAnimCheck(copy, size, b);
                if (why == NULL) LoaderTestPointers(raw, b, &why);
                if (why == NULL) {
                    animOk++;
                } else {
                    OSReport("[loadertest] sprite %s %u: FAIL (%s)\n", DataDirStat[dir].name, f, why);
                }
                if (raw[6] == 'P' && raw[7] == 'B' && memcmp(raw, copy, 6) == 0
                    && memcmp(raw + 8, copy + 8, size - 8) == 0) {
                    animUnchanged++;
                } else {
                    OSReport("[loadertest] sprite %s %u: FAIL (file data changed by the conversion)\n",
                        DataDirStat[dir].name, f);
                }
                HuSprAnimKill(a);
                HuSprAnimKill(b);
                free(copy);
            } else if (size > 0x40 && memcmp(raw, "HSF", 3) == 0) {
                HSFDATA *hsf;
                size_t twice, swapped, firstTwice;
                models++;
                if (memcmp(raw, "HSFV", 4) != 0) {
                    modelMagic++;
                    OSReport("[loadertest] model %s %u: magic %.7s, not HSFV (the converted mark would be "
                             "unsound)\n", DataDirStat[dir].name, f, (const char *)raw);
                }
                if (!markChecked) {
                    /* Check the second-load warning once, on a file that has
                       not been converted (so the load itself is normal). */
                    OSReport("[loadertest] next line should be the [hsfload] second-load warning (test of it)\n");
                    raw[3] = 'P';
                    markChecked = TRUE;
                }
                byteswap_track_begin(raw, size);
                hsf = LoadHSF(raw);
                twice = byteswap_track_end(&swapped, &firstTwice);
                bytesSwapped += swapped;
                if (twice != 0) {
                    modelsTwice++;
                    bytesTwice += twice;
                    OSReport("[loadertest] model %s %u: %lu bytes converted twice (first at 0x%lx) of %lu "
                             "converted in place\n", DataDirStat[dir].name, f, (unsigned long)twice,
                        (unsigned long)firstTwice, (unsigned long)swapped);
                }
                if (raw[3] != 'P') {
                    OSReport("[loadertest] model %s %u: FAIL (converted mark missing)\n", DataDirStat[dir].name, f);
                }
                if (hsf) KillHSF(hsf);
            } else {
                other++;
                OSReport("[loadertest] other file %s %u: %d bytes, starts %02x%02x%02x%02x %02x%02x%02x%02x\n",
                    DataDirStat[dir].name, f, (int)size, raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], raw[6], raw[7]);
            }
            /* Frees the file and everything LoadHSF allocated for it. */
            HuMemDirectFreeNum(HEAP_DATA, MEMORY_DEFAULT_NUM);
        }
        HuDataDirClose(dir << 16);
    }
    OSReport("[loadertest] sprites: %d files; conversions match the file: %d; file unchanged: %d; bytes converted "
             "in place: %lu; banks sharing frames: %d; patterns sharing layers: %d; frames not in bank order: %d; "
             "layers not in pattern order: %d; use-count field not 0x4100: %d\n", anims, animOk, animUnchanged,
        (unsigned long)animBytesSwapped, animSharedFrames, animSharedLayers, animFrameOrder, animLayerOrder,
        animUseNumOther);
    OSReport("[loadertest] models: %d files; bytes converted in place: %lu; files with bytes converted twice: %d "
             "(%lu bytes); magic not HSFV: %d; other files: %d\n", models, (unsigned long)bytesSwapped, modelsTwice,
        (unsigned long)bytesTwice, modelMagic, other);
    OSReport("[loadertest] done: %s\n", (animOk == anims && animUnchanged == anims && animBytesSwapped == 0
        && modelsTwice == 0 && modelMagic == 0 && animUseNumOther == 0) ? "PASS" : "FAIL");
}
#endif
