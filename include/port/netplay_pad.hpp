#pragma once

#include "dolphin/pad.h"
#include "port/rollback.h"

namespace partyboard::netplay {

// Wire inputs are raw PAD samples, before PADClamp and the game's edge/repeat
// processing. Never copy PADStatus itself onto the wire (PC adds extButton).
inline PartyBoardRollbackInput inputFromPad(const PADStatus& pad)
{
    if (pad.err != PAD_ERR_NONE) return {};
    return {pad.button, pad.stickX, pad.stickY, pad.substickX, pad.substickY,
        pad.triggerLeft, pad.triggerRight};
}

inline PADStatus padFromInput(const PartyBoardRollbackInput& input)
{
    PADStatus pad {};
    pad.button = input.buttons;
    pad.stickX = input.stickX;
    pad.stickY = input.stickY;
    pad.substickX = input.substickX;
    pad.substickY = input.substickY;
    pad.triggerLeft = input.triggerLeft;
    pad.triggerRight = input.triggerRight;
    pad.err = PAD_ERR_NONE;
    return pad;
}

} // namespace partyboard::netplay
