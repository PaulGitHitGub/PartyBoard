#include "port/determinism.h"
#include "dolphin.h"
#include "game/msm.h"
#include "game/object.h"
#include "game/objsub.h"
#include "game/pad.h"
#include "game/thpmain.h"
#include "port/rollback.h"
#include <stdint.h>
#include <string.h>
#ifdef TARGET_PC
#include "port/netplay_runtime.h"
#include "port/netplay_state.h"
#include "port/mem_diagnostics.h"
#include "port/pad_ports.h"
#include "game/gamework_data.h"
#include "game/board/eightlog.h"
#define PadGameControlMotor PartyBoard_NetplayControlMotor
#else
#define PadGameControlMotor PADControlMotor
#endif

#ifndef __MWERKS__
#include <stdlib.h>
#include <stdio.h>
#endif

typedef struct pad_rumble {
    s16 duration;
    s16 off;
    s16 on;
    s16 time;
} PadRumble;

static void PadReadVSync(u32 retraceCount);
static void PadADConv(s16 pad, PADStatus *status);

/* Device ("physical") ports: the _Pad* arrays and padStatErrOld, indexed by
   the port a controller/keyboard/phone is on. Game ("logical") ports: the
   HuPad* arrays and rumbleData, indexed the way the game asks. They are the
   same port unless an 8-player minigame has wired them (HuPadWiringSet). */
static int padStatErrOld[HU_PAD_PORT_MAX];
static PadRumble rumbleData[HU_PAD_PORT_MAX];

SHARED_SYM u16 HuPadBtn[HU_PAD_PORT_MAX];
SHARED_SYM u16 HuPadBtnDown[HU_PAD_PORT_MAX];
SHARED_SYM u16 HuPadBtnRep[HU_PAD_PORT_MAX];
SHARED_SYM s8 HuPadStkX[HU_PAD_PORT_MAX];
SHARED_SYM s8 HuPadStkY[HU_PAD_PORT_MAX];
SHARED_SYM s8 HuPadSubStkX[HU_PAD_PORT_MAX];
SHARED_SYM s8 HuPadSubStkY[HU_PAD_PORT_MAX];
SHARED_SYM u8 HuPadTrigL[HU_PAD_PORT_MAX];
SHARED_SYM u8 HuPadTrigR[HU_PAD_PORT_MAX];
SHARED_SYM u8 HuPadDStk[HU_PAD_PORT_MAX];
SHARED_SYM u8 HuPadDStkRep[HU_PAD_PORT_MAX];
s8 HuPadErr[HU_PAD_PORT_MAX];
u16 _PadBtn[HU_PAD_PORT_MAX];
u16 _PadBtnDown[HU_PAD_PORT_MAX];
static u16 _PadRepCnt[HU_PAD_PORT_MAX];
static s8 _PadStkX[HU_PAD_PORT_MAX];
static s8 _PadStkY[HU_PAD_PORT_MAX];
static s8 _PadSubStkX[HU_PAD_PORT_MAX];
static s8 _PadSubStkY[HU_PAD_PORT_MAX];
static u8 _PadTrigL[HU_PAD_PORT_MAX];
static u8 _PadTrigR[HU_PAD_PORT_MAX];
static u8 _PadDStk[HU_PAD_PORT_MAX];
static u8 _PadDStkRep[HU_PAD_PORT_MAX];
static u8 _PadDStkRepCnt[HU_PAD_PORT_MAX];
static u8 _PadDStkRepOld[HU_PAD_PORT_MAX];
static s8 _PadErr[HU_PAD_PORT_MAX];
/* Per device; copied to HuPadBtnRep[game port] each tick. */
static u16 _PadBtnRep[HU_PAD_PORT_MAX];
static u32 RumbleBit;
s32 VCounter;

#ifdef TARGET_PC
/* 8-player party wiring: game port k <- device padWire[k] while padWired. */
static s8 padWire[HU_PAD_PORT_MAX];
static u8 padWired;
#endif

static inline s32 PadWireDevice(s32 port)
{
#ifdef TARGET_PC
    return padWired ? padWire[port] : port;
#else
    return port;
#endif
}

#ifdef TARGET_PC
/* The PC renderer may present more often than the original 60 Hz simulation.
 * Keep PAD edge/repeat bookkeeping and the retrace-driven audio service on the
 * simulation clock, while allowing the simulation to poll SDL immediately
 * before it consumes input. */
static BOOL directSimulationPollingActive;
#endif

#ifdef TARGET_PC
static u32 chanTbl[HU_PAD_PORT_MAX] = { PAD_CHAN0_BIT, PAD_CHAN1_BIT, PAD_CHAN2_BIT, PAD_CHAN3_BIT,
    PAD_CHAN0_BIT >> 4, PAD_CHAN0_BIT >> 5, PAD_CHAN0_BIT >> 6, PAD_CHAN0_BIT >> 7 };
#else
static u32 chanTbl[4] = { PAD_CHAN0_BIT, PAD_CHAN1_BIT, PAD_CHAN2_BIT, PAD_CHAN3_BIT };
#endif
// Explicit simulation-side PAD state only: SDL devices and callbacks are not
// rewound. Restore at a quiescent tick boundary; no rumble command is emitted.
#ifdef TARGET_PC
typedef struct PadSnapshotRegion { void *address; size_t size; } PadSnapshotRegion;
static const PadSnapshotRegion padSnapshotRegions[] = {
    { &padStatErrOld, sizeof(padStatErrOld) },
    { &rumbleData, sizeof(rumbleData) },
    { &HuPadBtn, sizeof(HuPadBtn) },
    { &HuPadBtnDown, sizeof(HuPadBtnDown) },
    { &HuPadBtnRep, sizeof(HuPadBtnRep) },
    { &HuPadStkX, sizeof(HuPadStkX) },
    { &HuPadStkY, sizeof(HuPadStkY) },
    { &HuPadSubStkX, sizeof(HuPadSubStkX) },
    { &HuPadSubStkY, sizeof(HuPadSubStkY) },
    { &HuPadTrigL, sizeof(HuPadTrigL) },
    { &HuPadTrigR, sizeof(HuPadTrigR) },
    { &HuPadDStk, sizeof(HuPadDStk) },
    { &HuPadDStkRep, sizeof(HuPadDStkRep) },
    { &HuPadErr, sizeof(HuPadErr) },
    { &_PadBtn, sizeof(_PadBtn) },
    { &_PadBtnDown, sizeof(_PadBtnDown) },
    { &_PadRepCnt, sizeof(_PadRepCnt) },
    { &_PadStkX, sizeof(_PadStkX) },
    { &_PadStkY, sizeof(_PadStkY) },
    { &_PadSubStkX, sizeof(_PadSubStkX) },
    { &_PadSubStkY, sizeof(_PadSubStkY) },
    { &_PadTrigL, sizeof(_PadTrigL) },
    { &_PadTrigR, sizeof(_PadTrigR) },
    { &_PadDStk, sizeof(_PadDStk) },
    { &_PadDStkRep, sizeof(_PadDStkRep) },
    { &_PadDStkRepCnt, sizeof(_PadDStkRepCnt) },
    { &_PadDStkRepOld, sizeof(_PadDStkRepOld) },
    { &_PadErr, sizeof(_PadErr) },
    { &_PadBtnRep, sizeof(_PadBtnRep) },
    { &padWire, sizeof(padWire) },
    { &padWired, sizeof(padWired) },
    { &RumbleBit, sizeof(RumbleBit) },
    { &VCounter, sizeof(VCounter) },
};
size_t HuPadSnapshotSizeGet(void) {
    size_t size = sizeof(u32) * 2;
    for (size_t i = 0; i < sizeof(padSnapshotRegions)/sizeof(padSnapshotRegions[0]); ++i)
        size += padSnapshotRegions[i].size;
    return size;
}
BOOL HuPadSnapshotSave(void *destination, size_t capacity) {
    const size_t required = HuPadSnapshotSizeGet();
    u32 header[2] = { 0x50414431u, (u32)required };
    u8 *cursor = (u8 *)destination;
    if (!destination || capacity != required) return FALSE;
    memcpy(cursor, header, sizeof(header)); cursor += sizeof(header);
    for (size_t i = 0; i < sizeof(padSnapshotRegions)/sizeof(padSnapshotRegions[0]); ++i) {
        memcpy(cursor, padSnapshotRegions[i].address, padSnapshotRegions[i].size);
        cursor += padSnapshotRegions[i].size;
    }
    return TRUE;
}
BOOL HuPadSnapshotLoad(const void *source, size_t size) {
    u32 header[2];
    const u8 *cursor = (const u8 *)source;
    if (!source || size != HuPadSnapshotSizeGet()) return FALSE;
    memcpy(header, cursor, sizeof(header));
    if (header[0] != 0x50414431u || header[1] != size) return FALSE;
    // Refuse buffers aliasing live state before the first write.
    for (size_t i = 0; i < sizeof(padSnapshotRegions)/sizeof(padSnapshotRegions[0]); ++i) {
        uintptr_t src = (uintptr_t)source, dst = (uintptr_t)padSnapshotRegions[i].address;
        if (src <= dst ? dst - src < size : src - dst < padSnapshotRegions[i].size) return FALSE;
    }
    cursor += sizeof(header);
    for (size_t i = 0; i < sizeof(padSnapshotRegions)/sizeof(padSnapshotRegions[0]); ++i) {
        memcpy(padSnapshotRegions[i].address, cursor, padSnapshotRegions[i].size);
        cursor += padSnapshotRegions[i].size;
    }
    return TRUE;
}
BOOL HuPadSnapshotSelfTest(void) {
    const size_t size = HuPadSnapshotSizeGet();
    u8 *saved = (u8 *)malloc(size), *changed = (u8 *)malloc(size), *check = (u8 *)malloc(size);
    BOOL ok = saved && changed && check;
    if (!ok) { free(saved); free(changed); free(check); return FALSE; }
    ok = HuPadSnapshotSave(saved, size);
    for (size_t i = 0; i < sizeof(padSnapshotRegions)/sizeof(padSnapshotRegions[0]); ++i)
        memset(padSnapshotRegions[i].address, (int)(i + 1), padSnapshotRegions[i].size);
    ok = ok && HuPadSnapshotSave(changed, size);
    saved[0] ^= 1;
    ok = ok && !HuPadSnapshotLoad(saved, size) && HuPadSnapshotSave(check, size)
        && memcmp(check, changed, size) == 0;
    saved[0] ^= 1;
    ok = ok && !HuPadSnapshotLoad(saved, size - 1) && HuPadSnapshotSave(check, size)
        && memcmp(check, changed, size) == 0;
    // Always restore live state, including when an assertion above fails.
    const BOOL restored = HuPadSnapshotLoad(saved, size);
    ok = ok && restored && HuPadSnapshotSave(check, size) && memcmp(check, saved, size) == 0;
    free(saved); free(changed); free(check);
    return ok;
}
#endif

extern int HuDvdErrWait;

#ifdef TARGET_PC
bool PartyBoard_NetplayIsMinigame(void)
{
    return omMgIndexGet(omcurovl) != -1;
}

s32 PartyBoard_NetplayMinigameId(void)
{
    return omMgIndexGet(omcurovl);
}

s32 PartyBoard_NetplayContextId(void)
{
    return (s32)omcurovl;
}

#endif

/* Rumble command for a device port. Ports 4-7 go through the online motor
   routing only in an online 8-player party (they are seats there); in a
   4-player online game they are unplugged. */
static void PadMotor(s32 device, u32 command)
{
#ifdef TARGET_PC
    if (device >= PAD_CHANMAX && !(PartyBoard_NetplayEnabled() && PartyBoard_NetplaySyncedPorts() > PAD_CHANMAX)) {
        if (!PartyBoard_NetplayEnabled()) {
            PADControlMotor((u32)device, command);
        }
        return;
    }
#endif
    PadGameControlMotor(device, command);
}

void HuPadInit(void)
{
    int i;
    BOOL int_level;
    PADSetSpec(PAD_SPEC_5);
    PADInit();
#ifndef TARGET_PC
    SISetSamplingRate(0);
#endif
    int_level = OSDisableInterrupts();
    VISetPostRetraceCallback(PadReadVSync);
    OSRestoreInterrupts(int_level);
    for(i=0; i<HU_PAD_PORT_MAX; i++) {
        padStatErrOld[i] = PAD_ERR_NOT_READY;
    }
    VIWaitForRetrace();
    VIWaitForRetrace();
    HuPadRead();
    for(i=0; i<HU_PAD_PORT_MAX; i++) {
        if(_PadErr[i] == PAD_ERR_NONE) {
            PadMotor(i, PAD_MOTOR_STOP_HARD);
        }
        rumbleData[i].duration = 0;
        _PadRepCnt[i] = 0;
    }
}

/**
 * @brief Read each of the four gamepad states and updates the respective globals
 * 
 * @details Called once per frame, this function reads the state of each gamepad
 * and updates the global HuPad variables accordingly. The available variables are:
 * 
 * - **HuPadBtn**: The current button state for each gamepad
 * - **HuPadBtnDown**: The buttons that were pressed this frame for each gamepad. 
 *                     This is reset to 0 after each frame.
 * - **HuPadStkX**: The X-axis value of the main stick for each gamepad
 * - **HuPadStkY**: The Y-axis value of the main stick for each gamepad
 * - **HuPadSubStkX**: The X-axis value of the C-stick for each gamepad
 * - **HuPadSubStkY**: The Y-axis value of the C-stick for each gamepad
 * - **HuPadTrigL**: The analog left trigger value for each gamepad
 * - **HuPadTrigR**: The analog right trigger value for each gamepad
 * - **HuPadDStk**: The direction of the main stick for each gamepad
 *                  (1 = left, 2 = right, 4 = down, 8 = up) and a combination of 
 *                  these values if the stick is in a diagonal position
 * - **HuPadDStkRep**: The direction of the main stick for each gamepad. Also
 *                     known as the "repeated" stick value, this is used to
 *                     detect when the stick is held in a certain direction
 *                     for a period of 20 frames and repeats at an 
 *                     interval of 2 frames. Typically used for menu navigation.
 * - **HuPadErr**: The error code for each gamepad, if any.
 * 
 * Each of these variables is an array of 4 elements, one for each gamepad. The 
 * values are updated based on the current state of the gamepads at the time 
 * this function is called.
 * 
 * Joystick and C-stick values are signed 8-bit integers, ranging from -128 to 127.
 * 
 * All other values are unsigned 8-bit integers, ranging from 0 to 255.
 * 
 * Button values are bitmasks, where each bit represents a different button. T
 * he available button masks are:
 * 
 * - **PAD_BUTTON_LEFT**: The left D-pad button
 * - **PAD_BUTTON_RIGHT**: The right D-pad button
 * - **PAD_BUTTON_DOWN**: The down D-pad button
 * - **PAD_BUTTON_UP**: The up D-pad button
 * - **PAD_TRIGGER_Z**: The Z trigger button
 * - **PAD_TRIGGER_R**: The R trigger button
 * - **PAD_TRIGGER_L**: The L trigger button
 * - **PAD_BUTTON_A**: The A button
 * - **PAD_BUTTON_B**: The B button
 * - **PAD_BUTTON_X**: The X button
 * - **PAD_BUTTON_Y**: The Y button
 * - **PAD_BUTTON_START**: The start button
 * - **PAD_BUTTON_MENU**: The menu button (same as start button)
 * 
 * **Note:** D-Pad values are not read or stored in this function.
 * 
 */
#ifdef TARGET_PC
/* PARTYBOARD_INPUT_LOG=1: report every button press the game sees, per port,
 * and every port connecting or disconnecting, numbered by game frame. Lines up
 * a crash with the input that preceded it. */
static void HuPadLogFrame(void)
{
    static int enabled = -1;
    static u32 frame;
    static s8 lastErr[HU_PAD_PORT_MAX] = { 99, 99, 99, 99, 99, 99, 99, 99 };
    static const struct { u16 bit; const char *name; } names[] = {
        { PAD_BUTTON_A, "A" }, { PAD_BUTTON_B, "B" }, { PAD_BUTTON_X, "X" }, { PAD_BUTTON_Y, "Y" },
        { PAD_BUTTON_START, "START" }, { PAD_TRIGGER_Z, "Z" }, { PAD_TRIGGER_L, "L" }, { PAD_TRIGGER_R, "R" },
        { PAD_BUTTON_UP, "UP" }, { PAD_BUTTON_DOWN, "DOWN" }, { PAD_BUTTON_LEFT, "LEFT" }, { PAD_BUTTON_RIGHT, "RIGHT" },
    };
    s16 i;
    if (enabled < 0) {
        const char *value = getenv("PARTYBOARD_INPUT_LOG");
        enabled = value != NULL && value[0] != '\0' && value[0] != '0';
    }
    frame++;
    if (!enabled) {
        return;
    }
    for (i = 0; i < HU_PAD_PORT_MAX; i++) {
        if (_PadErr[i] != lastErr[i]) {
            OSReport("[input] frame %u port %d %s (err %d)\n", frame, i,
                _PadErr[i] == PAD_ERR_NONE ? "connected" : "not connected", _PadErr[i]);
            lastErr[i] = _PadErr[i];
        }
        if (_PadBtnDown[i] != 0) {
            char buf[96];
            size_t len = 0;
            size_t n;
            buf[0] = '\0';
            for (n = 0; n < sizeof(names) / sizeof(names[0]); n++) {
                if ((_PadBtnDown[i] & names[n].bit) && len + 8 < sizeof(buf)) {
                    len += (size_t)snprintf(buf + len, sizeof(buf) - len, " %s", names[n].name);
                }
            }
            if (padWired) {
                s32 k;
                for (k = 0; k < HU_PAD_PORT_MAX && padWire[k] != i; k++) {
                }
                OSReport("[input] frame %u port %d (game port %d) down:%s  stick %d,%d\n", frame, i, k, buf,
                    _PadStkX[i], _PadStkY[i]);
            }
            else {
                OSReport("[input] frame %u port %d down:%s  stick %d,%d\n", frame, i, buf, _PadStkX[i], _PadStkY[i]);
            }
        }
    }
}
#endif

#ifdef TARGET_PC
/* ---- 8-player party input (always-on game.log lines, "[8p] input"). ----
   Logging only: it reads the pad and player tables and never changes them. */

/* Ports with no button press logged since the wiring last changed. */
static u8 padFirstPressPending;
/* Frames until the next device check; 0 = check on the next HuPadRead. */
static u32 padDeviceWatchWait;

/* "seat 6 Wario (human)" for the player whose own port is 'device', or
   "no player" (a free port, or before the game recorded the seats). */
static void PadEightSeatText(s32 device, char *buf, size_t size)
{
    s32 i;
    if (GWPlayerCount() == 8) {
        for (i = 0; i < GW_PARTY_PLAYER_MAX; i++) {
            if (GWPlayerOwnPortGet(&GWPlayer[i]) == device) {
                snprintf(buf, size, "seat %d %s (%s)", (int)GWPlayerSeatGet(&GWPlayer[i]) + 1, EightLogName(i),
                    GWPlayer[i].com ? "COM" : "human");
                return;
            }
        }
    }
    snprintf(buf, size, "no player");
}

/* First button press on each game port after the wiring changed: shows whose
   device actually drives which character, on the board and in minigames. */
static void PadEightFirstPressLog(void)
{
    s32 k;
    if (padFirstPressPending == 0) {
        return;
    }
    for (k = 0; k < HU_PAD_PORT_MAX; k++) {
        if ((padFirstPressPending & (1 << k)) && HuPadBtnDown[k] != 0) {
            char seat[64];
            s32 device = PadWireDevice(k);
            s32 slot;
            padFirstPressPending &= ~(1 << k);
            for (slot = 0; slot < GW_PARTY_PLAYER_MAX && GWPlayer[slot].port != k; slot++) {
            }
            PadEightSeatText(device, seat, sizeof(seat));
            EightLog("input: first press %s: game port %d (%s) buttons 0x%04x <- port %d, %s",
                padWired ? "with minigame wiring" : "on the board", (int)k + 1,
                slot < GW_PARTY_PLAYER_MAX ? EightLogName(slot) : "nobody", (unsigned)HuPadBtnDown[k],
                (int)device + 1, seat);
        }
    }
}

/* Logs a line for each port whose device changes (plugged in, unplugged,
   phone joined or left, keyboard set up), checked about once a second. */
static void PadEightDeviceWatch(void)
{
    static char last[HU_PAD_PORT_MAX][160];
    s32 port;
    if (padDeviceWatchWait != 0) {
        padDeviceWatchWait--;
        return;
    }
    padDeviceWatchWait = 59;
    for (port = 0; port < HU_PAD_PORT_MAX; port++) {
        char device[96];
        char now[160];
        if (PartyBoard_NetplayEnabled() && port < PartyBoard_NetplaySyncedPorts()) {
            /* Online: device port N is online seat N on every copy; what is
               plugged into this computer does not reach the game. */
            if (port < PartyBoard_NetplayPlayerCount()) {
                snprintf(device, sizeof(device), "online seat %d%s", (int)port + 1,
                    (PartyBoard_NetplayLocalSeatMask() >> port & 1u) ? " (this copy's controller)" : "");
            } else {
                snprintf(device, sizeof(device), "no online seat (computer player's port)");
            }
        } else {
            PartyBoard_PadPortDescribe((unsigned)port, device, sizeof(device));
        }
        PadEightSeatText(port, now, sizeof(now));
        snprintf(now + strlen(now), sizeof(now) - strlen(now), " <- port %d: %s", (int)port + 1, device);
        if (strcmp(now, last[port]) != 0) {
            EightLog("input: %s", now);
            memcpy(last[port], now, sizeof(now));
        }
    }
}

/* Board code: the seats were just recorded (game start), log them now. */
void HuPadEightSeatsChanged(void)
{
    padDeviceWatchWait = 0;
    padFirstPressPending = 0xFF;
}
#endif

void HuPadRead(void)
{
    s16 i;
#ifdef TARGET_PC
    HuPadLogFrame();
    if (padWired && !GWSystem.eight_players) {
        /* Never carry a minigame wiring out of the 8-player party. */
        OSReport("[8p] WARNING: input wiring still set outside the 8-player party; every port back to its own device\n");
        HuPadWiringReset();
    }
#endif
    for(i=0; i<HU_PAD_PORT_MAX; i++) {
        s32 dev = PadWireDevice(i);
        HuPadBtn[i] = _PadBtn[dev] & ~(PAD_BUTTON_LEFT | PAD_BUTTON_RIGHT | PAD_BUTTON_UP | PAD_BUTTON_DOWN);
        HuPadBtnDown[i] = _PadBtnDown[dev] & ~(PAD_BUTTON_LEFT | PAD_BUTTON_RIGHT | PAD_BUTTON_UP | PAD_BUTTON_DOWN);
        HuPadStkX[i] = _PadStkX[dev];
        HuPadStkY[i] = _PadStkY[dev];
        HuPadSubStkX[i] = _PadSubStkX[dev];
        HuPadSubStkY[i] = _PadSubStkY[dev];
        HuPadTrigL[i] = _PadTrigL[dev];
        HuPadTrigR[i] = _PadTrigR[dev];
        HuPadDStk[i] = _PadDStk[dev];
        HuPadDStkRep[i] = _PadDStkRep[dev];
        HuPadErr[i] = _PadErr[dev];
        _PadBtnDown[dev] = 0;
    }
#ifdef TARGET_PC
    if (GWSystem.eight_players) {
        PadEightFirstPressLog();
        PadEightDeviceWatch();
    }
#endif
}

#ifdef TARGET_PC
BOOL HuPadWiringSet(const s8 *wire)
{
    u32 seen = 0;
    s32 i;
    for (i = 0; i < HU_PAD_PORT_MAX; i++) {
        if (wire[i] < 0 || wire[i] >= HU_PAD_PORT_MAX || (seen & (1u << wire[i]))) {
            OSReport("[8p] WARNING: input wiring refused (port %d <- %d is not a one-to-one table); wiring unchanged\n",
                (int)i + 1, (int)wire[i] + 1);
            return FALSE;
        }
        seen |= 1u << wire[i];
    }
    /* Rumble follows the wiring: stop it on the old devices first. */
    HuPadRumbleAllStop();
    padWired = FALSE;
    for (i = 0; i < HU_PAD_PORT_MAX; i++) {
        padWire[i] = wire[i];
        if (wire[i] != i) {
            padWired = TRUE;
        }
    }
    padFirstPressPending = 0xFF;
    return TRUE;
}

void HuPadWiringReset(void)
{
    s32 i;
    if (padWired) {
        HuPadRumbleAllStop();
    }
    padWired = FALSE;
    for (i = 0; i < HU_PAD_PORT_MAX; i++) {
        padWire[i] = (s8)i;
    }
    padFirstPressPending = 0xFF;
}

BOOL HuPadWiringActive(void)
{
    return padWired != 0;
}

s32 HuPadWiringDeviceGet(s32 port)
{
    return port >= 0 && port < HU_PAD_PORT_MAX ? PadWireDevice(port) : port;
}

/* Online desync check (canonical hash, INPUT): device ports 4-7 and the wiring,
   in 8-player games only, so 4-player hashes stay exactly what they were. */
void PartyBoard_NetplayEightPadState(PartyBoardNetplayStateSink sink, void *context)
{
    s32 i;
    if (!GWSystem.eight_players) return;
    sink(context, "padWired", (uint32_t)padWired);
    for (i = 0; i < HU_PAD_PORT_MAX; i++) {
        sink(context, "padWire", (uint32_t)padWire[i]);
    }
    for (i = PAD_CHANMAX; i < HU_PAD_PORT_MAX; i++) {
        sink(context, "port", (uint32_t)i);
        sink(context, "HuPadBtn", HuPadBtn[i]); sink(context, "HuPadBtnDown", HuPadBtnDown[i]);
        sink(context, "HuPadBtnRep", HuPadBtnRep[i]); sink(context, "HuPadStkX", (uint32_t)HuPadStkX[i]);
        sink(context, "HuPadStkY", (uint32_t)HuPadStkY[i]); sink(context, "HuPadSubStkX", (uint32_t)HuPadSubStkX[i]);
        sink(context, "HuPadSubStkY", (uint32_t)HuPadSubStkY[i]); sink(context, "HuPadTrigL", HuPadTrigL[i]);
        sink(context, "HuPadTrigR", HuPadTrigR[i]); sink(context, "HuPadDStk", HuPadDStk[i]);
        sink(context, "HuPadDStkRep", HuPadDStkRep[i]); sink(context, "HuPadErr", (uint32_t)HuPadErr[i]);
    }
}
#endif

static void PadRecordedControlMotor(BOOL hardware, u32 port, u32 command)
{
    if (hardware) PadMotor(port, command);
}

/* Shared normal-play/replay input processing. Device interaction is optional;
 * all edge, repeat, clamp and virtual rumble bookkeeping stays identical.
 * status holds HU_PAD_PORT_MAX device ports. The first loop works per device
 * port; the second per game port, each following its device (the same port
 * unless an 8-player minigame wired them), so per port the order of steps is
 * the original one. */
static void PadApplySimulationStatus(PADStatus *status, u32 rumble, BOOL hardware)
{
    u32 chan;
    s16 i;
    BOOL reconnected[HU_PAD_PORT_MAX];
    RumbleBit = rumble;
    PADClamp(status);
#ifdef TARGET_PC
    PADClamp(status + PAD_CHANMAX); /* ports 4-7 */
#endif
    chan = 0;
    for(i=0; i<HU_PAD_PORT_MAX; i++) {
        PADStatus *curr_status = &status[i];
        reconnected[i] = padStatErrOld[i] && curr_status->err == PAD_ERR_NONE;
        padStatErrOld[i] = curr_status->err;
        if(curr_status->err != PAD_ERR_NONE) {
            _PadErr[i] = curr_status->err;
            if(curr_status->err != PAD_ERR_TRANSFER && curr_status->err != PAD_ERR_NOT_READY) {
                chan |= chanTbl[i];
            }
            _PadBtnDown[i] = _PadBtn[i] = _PadStkX[i] = _PadStkY[i] = _PadSubStkX[i] = _PadSubStkY[i] = _PadTrigL[i] = _PadTrigR[i] =  _PadDStkRep[i] = _PadDStk[i] =  _PadBtnRep[i] = 0;
        } else {
            u16 button = curr_status->button;
            if(curr_status->triggerLeft & 0xC0) {
                button |= PAD_BUTTON_TRIGGER_L;
            }
            if(curr_status->triggerRight & 0xC0) {
                button |= PAD_BUTTON_TRIGGER_R;
            }
            if(button && _PadBtn[i] == button) {
                if(_PadRepCnt[i] > 20) {
                    _PadBtnRep[i] = button;
                } else {
                    _PadBtnRep[i] = 0;
                    _PadRepCnt[i]++;
                }
            } else {
                _PadRepCnt[i] = 0;
                _PadBtnRep[i] = button;
            }
            PadADConv(i, curr_status);
            _PadBtnDown[i] |= PADButtonDown(_PadBtn[i], button);
            _PadBtn[i] = button;
            _PadStkX[i] = curr_status->stickX;
            _PadStkY[i] = curr_status->stickY;
            _PadSubStkX[i] = curr_status->substickX;
            _PadSubStkY[i] = curr_status->substickY;
            _PadTrigL[i] = curr_status->triggerLeft;
            _PadTrigR[i] = curr_status->triggerRight;
            _PadErr[i] = curr_status->err;
        }
    }
    for(i=0; i<HU_PAD_PORT_MAX; i++) {
        const s32 dev = PadWireDevice(i);
        PadRumble *rumble = &rumbleData[i];
        HuPadBtnRep[i] = _PadBtnRep[dev];
        if(reconnected[dev]) {
            PadRecordedControlMotor(hardware, dev, PAD_MOTOR_STOP_HARD);
            rumble->duration = 0;
        }
        if(status[dev].err == PAD_ERR_NONE && rumble->duration) {
            s16 time = PB_MOD(rumble->time, (rumble->off+rumble->on));
            if(time == 0) {
                PadRecordedControlMotor(hardware, dev, PAD_MOTOR_RUMBLE);
            } else {
                if(time == rumble->off) {
                    PadRecordedControlMotor(hardware, dev, PAD_MOTOR_STOP);
                }
            }
            rumble->time++;
            if(rumble->time > rumble->duration) {
                PadRecordedControlMotor(hardware, dev, PAD_MOTOR_STOP_HARD);
                rumble->duration = 0;
            }
        }
    }
    if(chan && hardware) {
        PADReset(chan);
    }
}

#ifdef TARGET_PC
/* Ports the online session does not synchronize read as unplugged: a local
   controller there must never reach a synchronized game on one machine only.
   4-player sessions: ports 4-7; an online 8-player party synchronizes all 8
   (seat N = device port N). */
static void PadExtraPortsOff(PADStatus *status, u32 *rumble)
{
    s32 i;
    for (i = PartyBoard_NetplaySyncedPorts(); i < HU_PAD_PORT_MAX; i++) {
        memset(&status[i], 0, sizeof(status[i]));
        status[i].err = PAD_ERR_NO_CONTROLLER;
        *rumble &= ~chanTbl[i];
    }
}
#endif

static BOOL PadReadSimulationTick(u32 retraceCount)
{
    PADStatus status[HU_PAD_PORT_MAX];
    if (!HuDvdErrWait) {
#ifdef TARGET_PC
        u32 rumble = PADReadPorts(status, HU_PAD_PORT_MAX);
        if (!PartyBoard_NetplayPreparePads(status, &rumble, !directSimulationPollingActive)) {
            return FALSE;
        }
        if (PartyBoard_NetplayEnabled()) {
            PadExtraPortsOff(status, &rumble);
        }
#else
        u32 rumble = PADRead(status);
#endif
        PadApplySimulationStatus(status, rumble, TRUE);
    }
    msmSysRegularProc();
#ifdef TARGET_PC
    /* Logical audio clock. Kept out of msmSysRegularProc because bank
     * changes call that one repeatedly inside a drain loop. */
    msmStreamLogicalTick();
    /* Same reasoning for movie playback: gameplay blocks on the movie ending,
     * so its position must come from accepted ticks, not from the device. */
    PartyBoard_ThpLogicalTick();
    /* One integrity sweep per accepted simulation tick, at the same point every
     * time, so a violation can be dated to a frame. Off unless
     * PARTYBOARD_MEM_DIAGNOSTICS=1, and the sweep itself stops the run on the
     * first violation rather than letting the damage spread. */
    PartyBoard_MemDiagTick();
    PartyBoard_CoroutineWatchdogTick();
#endif
    VCounter++;
    return TRUE;
}

#ifdef TARGET_PC
bool PartyBoard_RollbackApplyPads(const PartyBoardRollbackInput inputs[4], u8 connectedMask)
{
    PADStatus status[HU_PAD_PORT_MAX];
    u32 rumble = 0;
    if (!inputs || (connectedMask & 0xf0)) return false;
    memset(status, 0, sizeof(status));
    PadExtraPortsOff(status, &rumble);
    for (int i = 0; i < 4; ++i) {
        if (!(connectedMask & (1u << i))) {
            status[i].err = PAD_ERR_NO_CONTROLLER;
            continue;
        }
        status[i].button = inputs[i].buttons;
        status[i].stickX = inputs[i].stickX;
        status[i].stickY = inputs[i].stickY;
        status[i].substickX = inputs[i].substickX;
        status[i].substickY = inputs[i].substickY;
        status[i].triggerLeft = inputs[i].triggerLeft;
        status[i].triggerRight = inputs[i].triggerRight;
        status[i].err = PAD_ERR_NONE;
        rumble |= chanTbl[i];
    }
    PadApplySimulationStatus(status, rumble, FALSE);
    VCounter++;
    HuPadRead();
    return true;
}
#endif

#ifdef TARGET_PC
BOOL HuPadPollSimulationTick(void)
{
    /* From this point on the fixed-step main loop owns PAD polling. The PC VI
     * callback is only retained for startup, before the first simulation tick.
     * This also prevents movie/reset VIWaitForRetrace calls from inserting
     * extra PAD/audio ticks between two 60 Hz game ticks. */
    directSimulationPollingActive = TRUE;
    return PadReadSimulationTick(0);
}
#endif

static void PadReadVSync(u32 retraceCount)
{
#ifdef TARGET_PC
    if (directSimulationPollingActive) {
        return;
    }
#endif
    PadReadSimulationTick(retraceCount);
}

static void PadADConv(s16 pad, PADStatus *status)
{
    s16 stickX, stickY;
    s16 spA, sp8;
    spA = 0;
    sp8 = 0;
    stickX = abs(status->stickX);
    stickY = abs(status->stickY);
    _PadDStk[pad] = 0;
    if(stickY > 20) {
        if(status->stickY > 0) {
            _PadDStk[pad] |= PAD_BUTTON_UP;
        } else {
            _PadDStk[pad] |= PAD_BUTTON_DOWN;
        }
    }
    if(stickX > 30) {
        if(status->stickX < 0) {
            _PadDStk[pad] |= PAD_BUTTON_LEFT;
        } else {
            _PadDStk[pad] |= PAD_BUTTON_RIGHT;
        }
    }
    if(stickX+stickY < 20) {
        _PadDStkRepOld[pad]  =0;
    }
    if(_PadDStkRepCnt[pad]) {
        _PadDStkRepCnt[pad]--;
        if(stickX+stickY < 20) {
            _PadDStkRepCnt[pad] = 0;
        }
        _PadDStkRep[pad] = 0;
    } else {
        _PadDStkRep[pad] = _PadDStk[pad];
        if(_PadDStkRep[pad]) {
            if(_PadDStkRepOld[pad] == _PadDStkRep[pad]) {
                _PadDStkRepCnt[pad] = 2;
            } else {
                _PadDStkRepCnt[pad] = 20;
            }
            _PadDStkRepOld[pad] = _PadDStkRep[pad];
        }
    }
}

void HuPadRumbleSet(s16 pad, s16 duration, s16 off, s16 on)
{
    PadRumble *rumble = &rumbleData[pad];
    if(_PadErr[PadWireDevice(pad)] == PAD_ERR_NONE) {
        rumble->duration = duration;
        rumble->off = off;
        rumble->on = on;
        rumble->time = 0;
    }
}

void HuPadRumbleStop(s16 pad)
{
    PadRumble *rumble = &rumbleData[pad];
    const s32 dev = PadWireDevice(pad);
    if(_PadErr[dev] == PAD_ERR_NONE) {
        rumble->duration = 0;
        PadMotor(dev, PAD_MOTOR_STOP_HARD);
    }
}

void HuPadRumbleAllStop(void)
{
    int i;
    for(i=0; i<HU_PAD_PORT_MAX; i++) {
        const s32 dev = PadWireDevice(i);
        rumbleData[i].duration = 0;
        if(_PadErr[dev] == PAD_ERR_NONE) {
            PadMotor(dev, PAD_MOTOR_STOP_HARD);
        }
    }
}

s16 HuPadStatGet(s16 pad)
{
    return _PadErr[PadWireDevice(pad)];
}

u32 HuPadRumbleGet(void)
{
    return RumbleBit;
}

#ifdef TARGET_PC
#include "port/rollback_scene.h"
bool PartyBoard_RollbackPadRegions(PartyBoardRollbackRegionSink sink, void *context)
{
    if (!sink) return false;
    for (size_t i = 0; i < sizeof(padSnapshotRegions) / sizeof(padSnapshotRegions[0]); ++i)
        if (!sink(context, padSnapshotRegions[i].address, padSnapshotRegions[i].size)) return false;
    return true;
}
#endif
