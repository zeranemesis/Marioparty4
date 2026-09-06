/* Compile the actual game PAD implementation, with hardware/network doubles.
 * This guards the commit boundary, not a second copy of the PAD algorithm. */
#include <stdio.h>
#include <string.h>
#include "../../src/game/pad.c"

int HuDvdErrWait;
OMOVL omcurovl;
static PADStatus physical[4];
static PADStatus remote;
static bool networkActive, ready = true;
static unsigned audioTicks, clampCalls, checks;
#define CHECK(x) do { ++checks; if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)

s32 omMgIndexGet(s16 overlay) { (void)overlay; return 1; }
BOOL PADInit(void) { return TRUE; }
void PADSetSpec(u32 spec) { (void)spec; }
BOOL OSDisableInterrupts(void) { return TRUE; }
BOOL OSRestoreInterrupts(BOOL level) { return level; }
VIRetraceCallback VISetPostRetraceCallback(VIRetraceCallback cb) { return cb; }
void VIWaitForRetrace(void) {}
BOOL PADReset(u32 mask) { (void)mask; return TRUE; }
u32 PADRead(PADStatus* status) { memcpy(status, physical, sizeof(physical)); return PAD_CHAN0_BIT; }
void PADClamp(PADStatus* status) { (void)status; ++clampCalls; }
void msmSysRegularProc(void) { ++audioTicks; }
void PartyBoard_NetplayControlMotor(u32 port, u32 command) { (void)port; (void)command; }
bool PartyBoard_NetplayPreparePads(PADStatus status[4], u32* rumble, bool startup)
{
    (void)rumble; (void)startup;
    if (!networkActive) return true;
    if (!ready) return false;
    status[1] = remote;
    return true;
}

int main(void)
{
    physical[0].button = PAD_BUTTON_A;
    physical[0].stickX = 67;
    physical[0].stickY = -42;
    physical[0].triggerLeft = 200;
    physical[1].err = PAD_ERR_NO_CONTROLLER;
    physical[2].err = physical[3].err = PAD_ERR_NO_CONTROLLER;
    remote = physical[0];
    networkActive = true;
    for (unsigned frame = 0; frame < 65; ++frame) {
        // Insert arbitrary network stalls: repeat timers and audio must freeze.
        const s32 counter = VCounter;
        const unsigned audio = audioTicks, clamp = clampCalls;
        const u16 repeats = _PadRepCnt[1];
        ready = false;
        for (unsigned stall = 0; stall < frame % 7; ++stall) {
            CHECK(!HuPadPollSimulationTick());
            CHECK(VCounter == counter && audioTicks == audio && clampCalls == clamp);
            CHECK(_PadRepCnt[1] == repeats);
        }
        ready = true;
        CHECK(HuPadPollSimulationTick());
        HuPadRead();
        CHECK(VCounter == counter + 1 && audioTicks == audio + 1 && clampCalls == clamp + 1);
        CHECK(HuPadBtn[0] == HuPadBtn[1]);
        CHECK(HuPadBtnDown[0] == HuPadBtnDown[1]);
        CHECK(HuPadBtnRep[0] == HuPadBtnRep[1]);
        CHECK(HuPadDStk[0] == HuPadDStk[1]);
        CHECK(HuPadDStkRep[0] == HuPadDStkRep[1]);
        CHECK(HuPadStkX[0] == HuPadStkX[1] && HuPadTrigL[0] == HuPadTrigL[1]);
        CHECK(HuPadStatGet(1) == PAD_ERR_NONE);
        if (frame == 0) CHECK(HuPadBtnDown[1] & PAD_BUTTON_A);
        if (frame == 25) CHECK(HuPadBtnRep[1] & PAD_BUTTON_A);
        if (frame == 40) { physical[0].button = 0; remote.button = 0; }
        if (frame == 45) { physical[0].button = PAD_BUTTON_B; remote.button = PAD_BUTTON_B; }
    }
    CHECK(HuPadStatGet(2) == PAD_ERR_NO_CONTROLLER);
    networkActive = false;
    CHECK(HuPadPollSimulationTick());
    HuPadRead();
    CHECK(HuPadStatGet(1) == PAD_ERR_NO_CONTROLLER);
    CHECK(HuPadBtn[0] & PAD_BUTTON_B);
    printf("PAD tick regression: PASS (%u checks)\n", checks);
    return 0;
}
