#ifndef PARTYBOARD_ROLLBACK_AUDIO_BRIDGE_H
#define PARTYBOARD_ROLLBACK_AUDIO_BRIDGE_H

#include "dolphin/types.h"
#include <stddef.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif
#ifdef TARGET_PC
/* Experimental bridge for ordinary 2D HuAudFX calls. The owner starts one
 * bridge per rollback session, opens/closes every simulated tick (including
 * replay), then confirms only Session::confirmedFrame(). */
bool PartyBoard_RollbackAudioBridgeStart(void);
bool PartyBoard_RollbackAudioBridgeFrameBegin(u32 frame);
bool PartyBoard_RollbackAudioBridgeFrameEnd(void);
bool PartyBoard_RollbackAudioBridgeConfirm(u32 exclusiveFrame);
bool PartyBoard_RollbackAudioBridgeStop(void);
bool PartyBoard_RollbackAudioBridgeActive(void);
bool PartyBoard_RollbackAudioBridgeHealthy(void);
bool PartyBoard_RollbackAudioBridgeIsVirtual(s32 handle);
s32 PartyBoard_RollbackAudioBridgePlay2D(s32 sound, s32 volume, s32 pan);
bool PartyBoard_RollbackAudioBridgeStopFX(s32 handle, s32 fadeMilliseconds);
bool PartyBoard_RollbackAudioBridgeParameter(s32 handle, s32 parameter, s32 value);
s32 PartyBoard_RollbackAudioBridgeStatus(s32 handle);
enum {
    PARTYBOARD_ROLLBACK_FX_VOLUME = 0,
    PARTYBOARD_ROLLBACK_FX_PAN = 1,
    PARTYBOARD_ROLLBACK_FX_PITCH = 2
};
#endif
#ifdef __cplusplus
}
#endif
#endif
