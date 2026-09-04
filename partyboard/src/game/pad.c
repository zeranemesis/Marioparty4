#include "dolphin.h"
#include "game/msm.h"
#include "game/object.h"
#include "game/objsub.h"
#include "game/pad.h"
#include "port/rollback.h"

#ifndef __MWERKS__
#include <stdlib.h>
#endif

typedef struct pad_rumble {
    s16 duration;
    s16 off;
    s16 on;
    s16 time;
} PadRumble;

static void PadReadVSync(u32 retraceCount);
static void PadADConv(s16 pad, PADStatus *status);

static int padStatErrOld[4];
static PadRumble rumbleData[4];

SHARED_SYM u16 HuPadBtn[4];
SHARED_SYM u16 HuPadBtnDown[4];
SHARED_SYM u16 HuPadBtnRep[4];
SHARED_SYM s8 HuPadStkX[4];
SHARED_SYM s8 HuPadStkY[4];
SHARED_SYM s8 HuPadSubStkX[4];
SHARED_SYM s8 HuPadSubStkY[4];
SHARED_SYM u8 HuPadTrigL[4];
SHARED_SYM u8 HuPadTrigR[4];
SHARED_SYM u8 HuPadDStk[4];
SHARED_SYM u8 HuPadDStkRep[4];
s8 HuPadErr[4];
u16 _PadBtn[4];
u16 _PadBtnDown[4];
static u16 _PadRepCnt[4];
static s8 _PadStkX[4];
static s8 _PadStkY[4];
static s8 _PadSubStkX[4];
static s8 _PadSubStkY[4];
static u8 _PadTrigL[4];
static u8 _PadTrigR[4];
static u8 _PadDStk[4];
static u8 _PadDStkRep[4];
static u8 _PadDStkRepCnt[4];
static u8 _PadDStkRepOld[4];
static s8 _PadErr[4];
static u32 RumbleBit;
s32 VCounter;

#ifdef TARGET_PC
/* The PC renderer may present more often than the original 60 Hz simulation.
 * Keep PAD edge/repeat bookkeeping and the retrace-driven audio service on the
 * simulation clock, while allowing the simulation to poll SDL immediately
 * before it consumes input. */
static BOOL directSimulationPollingActive;
#endif

static u32 chanTbl[4] = { PAD_CHAN0_BIT, PAD_CHAN1_BIT, PAD_CHAN2_BIT, PAD_CHAN3_BIT };

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

void PartyBoard_NetplayPadCapture(s32 pad, PartyBoardRollbackInput *input)
{
    input->buttons = HuPadBtn[pad] | HuPadDStk[pad];
    input->stickX = HuPadStkX[pad];
    input->stickY = HuPadStkY[pad];
    input->substickX = HuPadSubStkX[pad];
    input->substickY = HuPadSubStkY[pad];
    input->triggerLeft = HuPadTrigL[pad];
    input->triggerRight = HuPadTrigR[pad];
}

void PartyBoard_NetplayPadCopy(s32 destination, s32 source)
{
    HuPadBtn[destination] = HuPadBtn[source];
    HuPadBtnDown[destination] = HuPadBtnDown[source];
    HuPadBtnRep[destination] = HuPadBtnRep[source];
    HuPadStkX[destination] = HuPadStkX[source];
    HuPadStkY[destination] = HuPadStkY[source];
    HuPadSubStkX[destination] = HuPadSubStkX[source];
    HuPadSubStkY[destination] = HuPadSubStkY[source];
    HuPadTrigL[destination] = HuPadTrigL[source];
    HuPadTrigR[destination] = HuPadTrigR[source];
    HuPadDStk[destination] = HuPadDStk[source];
    HuPadDStkRep[destination] = HuPadDStkRep[source];
    HuPadErr[destination] = HuPadErr[source];
}

void PartyBoard_NetplayPadApplyRemote(s32 pad, const PartyBoardRollbackInput *input,
    const PartyBoardRollbackInput *previous)
{
    const u16 dpadMask = PAD_BUTTON_LEFT | PAD_BUTTON_RIGHT | PAD_BUTTON_UP | PAD_BUTTON_DOWN;
    HuPadBtn[pad] = input->buttons & ~dpadMask;
    HuPadBtnDown[pad] = (input->buttons & ~previous->buttons) & ~dpadMask;
    HuPadBtnRep[pad] = HuPadBtnDown[pad];
    HuPadStkX[pad] = input->stickX;
    HuPadStkY[pad] = input->stickY;
    HuPadSubStkX[pad] = input->substickX;
    HuPadSubStkY[pad] = input->substickY;
    HuPadTrigL[pad] = input->triggerLeft;
    HuPadTrigR[pad] = input->triggerRight;
    HuPadDStk[pad] = (u8)(input->buttons & dpadMask);
    HuPadDStkRep[pad] = (u8)((input->buttons & ~previous->buttons) & dpadMask);
    HuPadErr[pad] = PAD_ERR_NONE;
}
#endif

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
    for(i=0; i<4; i++) {
        padStatErrOld[i] = PAD_ERR_NOT_READY;
    }
    VIWaitForRetrace();
    VIWaitForRetrace();
    HuPadRead();
    for(i=0; i<4; i++) {
        if(_PadErr[i] == PAD_ERR_NONE) {
            PADControlMotor(i, PAD_MOTOR_STOP_HARD);
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
void HuPadRead(void)
{
    s16 i;
    for(i=0; i<4; i++) {
        HuPadBtn[i] = _PadBtn[i] & ~(PAD_BUTTON_LEFT | PAD_BUTTON_RIGHT | PAD_BUTTON_UP | PAD_BUTTON_DOWN);
        HuPadBtnDown[i] = _PadBtnDown[i] & ~(PAD_BUTTON_LEFT | PAD_BUTTON_RIGHT | PAD_BUTTON_UP | PAD_BUTTON_DOWN);
        HuPadStkX[i] = _PadStkX[i];
        HuPadStkY[i] = _PadStkY[i];
        HuPadSubStkX[i] = _PadSubStkX[i];
        HuPadSubStkY[i] = _PadSubStkY[i];
        HuPadTrigL[i] = _PadTrigL[i];
        HuPadTrigR[i] = _PadTrigR[i];
        HuPadDStk[i] = _PadDStk[i];
        HuPadDStkRep[i] = _PadDStkRep[i];
        HuPadErr[i] = _PadErr[i];
        _PadBtnDown[i] = 0;
    }
}

static void PadReadSimulationTick(u32 retraceCount)
{
    u32 chan;
    s16 i;
    PADStatus status[4];
    if(!HuDvdErrWait) {
        RumbleBit = PADRead(status);
        PADClamp(status);
        chan = 0;
        for(i=0; i<4; i++) {
            PADStatus *curr_status = &status[i];
            PadRumble *rumble = &rumbleData[i];
            if(padStatErrOld[i] && curr_status->err == PAD_ERR_NONE) {
                PADControlMotor(i, PAD_MOTOR_STOP_HARD);
                rumble->duration = 0;
            }
            padStatErrOld[i] = curr_status->err;
            if(curr_status->err != PAD_ERR_NONE) {
                _PadErr[i] = curr_status->err;
                if(curr_status->err != PAD_ERR_TRANSFER && curr_status->err != PAD_ERR_NOT_READY) {
                    chan |= chanTbl[i];
                }
                _PadBtnDown[i] = _PadBtn[i] = _PadStkX[i] = _PadStkY[i] = _PadSubStkX[i] = _PadSubStkY[i] = _PadTrigL[i] = _PadTrigR[i] =  _PadDStkRep[i] = _PadDStk[i] =  HuPadBtnRep[i] = 0;
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
                        HuPadBtnRep[i] = button;
                    } else {
                        HuPadBtnRep[i] = 0;
                        _PadRepCnt[i]++;
                    }
                } else {
                    _PadRepCnt[i] = 0;
                    HuPadBtnRep[i] = button;
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
                if(rumble->duration) {
                    s16 time = rumble->time%(rumble->off+rumble->on);
                    if(time == 0) {
                        PADControlMotor(i, PAD_MOTOR_RUMBLE);
                    } else {
                        if(time == rumble->off) {
                            PADControlMotor(i, PAD_MOTOR_STOP);
                        }
                    }
                    rumble->time++;
                    if(rumble->time > rumble->duration) {
                        PADControlMotor(i, PAD_MOTOR_STOP_HARD);
                        rumble->duration = 0;
                    }
                }
            }
        }
        if(chan) {
            PADReset(chan);
        }
    }
    msmSysRegularProc();
    VCounter++;
}

#ifdef TARGET_PC
void HuPadPollSimulationTick(void)
{
    /* From this point on the fixed-step main loop owns PAD polling. The PC VI
     * callback is only retained for startup, before the first simulation tick.
     * This also prevents movie/reset VIWaitForRetrace calls from inserting
     * extra PAD/audio ticks between two 60 Hz game ticks. */
    directSimulationPollingActive = TRUE;
    PadReadSimulationTick(0);
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
    if(_PadErr[pad] == PAD_ERR_NONE) {
        rumble->duration = duration;
        rumble->off = off;
        rumble->on = on;
        rumble->time = 0;
    }
}

void HuPadRumbleStop(s16 pad)
{
    PadRumble *rumble = &rumbleData[pad];
    if(_PadErr[pad] == PAD_ERR_NONE) {
        rumble->duration = 0;
        PADControlMotor(pad, PAD_MOTOR_STOP_HARD);
    }
}

void HuPadRumbleAllStop(void)
{
    int i;
    for(i=0; i<4; i++) {
        rumbleData[i].duration = 0;
        if(_PadErr[i] == PAD_ERR_NONE) {
            PADControlMotor(i, PAD_MOTOR_STOP_HARD);
        }
    }
}

s16 HuPadStatGet(s16 pad)
{
    return _PadErr[pad];
}

u32 HuPadRumbleGet(void)
{
    return RumbleBit;
}
