#include <algorithm>
#include <array>
#include <cstring>

#include <switch.h>
#include <dolphin/pad.h>

namespace {

std::array<PadState, PAD_CHANMAX> g_pads{};
bool g_initialized = false;

constexpr u64 npad_mask(HidNpadIdType id) {
    return 1ULL << static_cast<unsigned>(id);
}

s8 stick_to_gc(s32 value) {
    // libnx exposes signed 16-bit-ish stick coordinates. GameCube PADStatus is
    // signed 8-bit and Aurora deliberately avoids -128, so preserve that
    // convention for deterministic behaviour with the existing game code.
    const s32 scaled = value / 256;
    return static_cast<s8>(std::clamp<s32>(scaled, -127, 127));
}

u16 map_buttons(u64 buttons) {
    u16 out = 0;

    // Match PartyBoard's existing SDL "standard" mapping by physical position:
    // south -> GC A, east -> GC B, west -> GC X, north -> GC Y.
    if (buttons & HidNpadButton_B)     out |= PAD_BUTTON_A;
    if (buttons & HidNpadButton_A)     out |= PAD_BUTTON_B;
    if (buttons & HidNpadButton_Y)     out |= PAD_BUTTON_X;
    if (buttons & HidNpadButton_X)     out |= PAD_BUTTON_Y;

    if (buttons & HidNpadButton_Plus)  out |= PAD_BUTTON_START;

    if (buttons & HidNpadButton_Left)  out |= PAD_BUTTON_LEFT;
    if (buttons & HidNpadButton_Right) out |= PAD_BUTTON_RIGHT;
    if (buttons & HidNpadButton_Down)  out |= PAD_BUTTON_DOWN;
    if (buttons & HidNpadButton_Up)    out |= PAD_BUTTON_UP;

    if (buttons & (HidNpadButton_L | HidNpadButton_ZL))
        out |= PAD_TRIGGER_L;
    if (buttons & HidNpadButton_R)
        out |= PAD_TRIGGER_R;
    if (buttons & HidNpadButton_ZR)
        out |= PAD_TRIGGER_Z;

    return out;
}

void neutral_status(PADStatus& status, s8 error) {
    std::memset(&status, 0, sizeof(status));
    status.err = error;
}

} // namespace

extern "C" {

BOOL PADInit() {
    if (g_initialized)
        return TRUE;

    padConfigureInput(4, HidNpadStyleSet_NpadStandard);

    // Player 1 also accepts handheld controls. Players 2-4 are individual
    // Npad slots, which gives Mario Party four independent logical pads.
    padInitializeWithMask(&g_pads[0],
        npad_mask(HidNpadIdType_No1) | npad_mask(HidNpadIdType_Handheld));
    padInitializeWithMask(&g_pads[1], npad_mask(HidNpadIdType_No2));
    padInitializeWithMask(&g_pads[2], npad_mask(HidNpadIdType_No3));
    padInitializeWithMask(&g_pads[3], npad_mask(HidNpadIdType_No4));

    g_initialized = true;
    return TRUE;
}

u32 PADRead(PADStatus* status) {
    if (!status)
        return 0;

    if (!g_initialized)
        PADInit();

    for (u32 i = 0; i < PAD_CHANMAX; ++i) {
        padUpdate(&g_pads[i]);

        if (!padIsConnected(&g_pads[i])) {
            neutral_status(status[i], PAD_ERR_NO_CONTROLLER);
            continue;
        }

        neutral_status(status[i], PAD_ERR_NONE);

        const u64 buttons = padGetButtons(&g_pads[i]);
        const HidAnalogStickState left = padGetStickPos(&g_pads[i], 0);
        const HidAnalogStickState right = padGetStickPos(&g_pads[i], 1);

        status[i].button = map_buttons(buttons);
        status[i].stickX = stick_to_gc(left.x);
        status[i].stickY = stick_to_gc(left.y);
        status[i].substickX = stick_to_gc(right.x);
        status[i].substickY = stick_to_gc(right.y);

        // Standard Switch controllers expose digital shoulders rather than the
        // GameCube's analog travel. Use Aurora's existing digital-trigger value
        // so the game sees the same semantics as the desktop port.
        if (buttons & (HidNpadButton_L | HidNpadButton_ZL))
            status[i].triggerLeft = 180;
        if (buttons & HidNpadButton_R)
            status[i].triggerRight = 180;
    }

    // Rumble is intentionally not advertised during bring-up. Once input and
    // rendering are stable, PADControlMotor will be backed by hid vibration.
    return 0;
}

BOOL PADReset(u32) {
    return TRUE;
}

BOOL PADRecalibrate(u32) {
    return TRUE;
}

void PADClamp(PADStatus* status) {
    if (!status)
        return;

    for (u32 i = 0; i < PAD_CHANMAX; ++i) {
        status[i].stickX = static_cast<s8>(std::clamp<int>(status[i].stickX, -127, 127));
        status[i].stickY = static_cast<s8>(std::clamp<int>(status[i].stickY, -127, 127));
        status[i].substickX = static_cast<s8>(std::clamp<int>(status[i].substickX, -127, 127));
        status[i].substickY = static_cast<s8>(std::clamp<int>(status[i].substickY, -127, 127));
    }
}

void PADClampCircle(PADStatus* status) {
    PADClamp(status);
}

void PADControlMotor(u32, u32) {
    // TODO(switch): map GameCube motor state to libnx vibration values.
}

void PADControlAllMotors(const u32*) {
    // TODO(switch): map GameCube motor state to libnx vibration values.
}

void PADSetSpec(u32) {}
void PADSetAnalogMode(u32) {}

} // extern "C"
