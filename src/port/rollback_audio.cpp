#include "port/rollback_audio.hpp"
#include "port/rollback_audio_bridge.h"
extern "C" {
#include "game/msm.h"
#include "game/audio.h"
#include "dolphin/os.h"
}
#include <memory>
#include "rollback_audio_test.inc"

namespace partyboard::rollback {
FxBackend makeNativeFxBackend() {
    return {
        [](int sound, int volume, int pan) {
            MSM_SEPARAM parameters {};
            parameters.flag = MSM_SEPARAM_VOL | MSM_SEPARAM_PAN;
            parameters.vol = static_cast<s8>(volume); parameters.pan = static_cast<s8>(pan);
            return msmSePlay(sound, &parameters);
        },
        [](int handle, int fade) { return msmSeStop(handle, fade); },
        [](int handle, FxParameter parameter, int value) {
            MSM_SEPARAM parameters {};
            switch (parameter) {
            case FxParameter::Volume: parameters.flag = MSM_SEPARAM_VOL; parameters.vol = static_cast<s8>(value); break;
            case FxParameter::Pan: parameters.flag = MSM_SEPARAM_PAN; parameters.pan = static_cast<s8>(value); break;
            case FxParameter::Pitch: parameters.flag = MSM_SEPARAM_PITCH; parameters.pitch = static_cast<s16>(value); break;
            default: return MSM_ERR_INVALIDSE;
            }
            return msmSeSetParam(handle, &parameters);
        },
        [](int handle) { return msmSeGetStatus(handle); }
    };
}
}

namespace {
using namespace partyboard::rollback;
constexpr u32 kVirtualTag = 0x40000000u;
constexpr u32 kVirtualTagMask = 0xc0000000u;
constexpr u32 kVirtualValueMask = 0x3fffffffu;
std::unique_ptr<ConfirmedAudioFx> gAudioBridge;
bool decodeVirtual(s32 handle, FxTicket &ticket)
{
    const auto bits = static_cast<u32>(handle);
    if ((bits & kVirtualTagMask) != kVirtualTag) return false;
    ticket.value = bits & kVirtualValueMask;
    return ticket.value != 0;
}
FxParameter decodeParameter(s32 parameter)
{
    switch (parameter) {
    case PARTYBOARD_ROLLBACK_FX_VOLUME: return FxParameter::Volume;
    case PARTYBOARD_ROLLBACK_FX_PAN: return FxParameter::Pan;
    case PARTYBOARD_ROLLBACK_FX_PITCH: return FxParameter::Pitch;
    default: return static_cast<FxParameter>(-1);
    }
}
}

extern "C" bool PartyBoard_RollbackAudioBridgeStart(void)
{
    if (gAudioBridge) return false;
    try { gAudioBridge = std::make_unique<ConfirmedAudioFx>(makeNativeFxBackend()); }
    catch (...) { return false; }
    return true;
}
extern "C" bool PartyBoard_RollbackAudioBridgeFrameBegin(u32 frame)
{ return gAudioBridge && gAudioBridge->beginFrame(frame); }
extern "C" bool PartyBoard_RollbackAudioBridgeFrameEnd(void)
{ return gAudioBridge && gAudioBridge->endFrame(); }
extern "C" bool PartyBoard_RollbackAudioBridgeConfirm(u32 exclusiveFrame)
{ return gAudioBridge && gAudioBridge->confirmThrough(exclusiveFrame); }
extern "C" bool PartyBoard_RollbackAudioBridgeStop(void)
{
    if (!gAudioBridge) return true;
    const bool ok = gAudioBridge->close();
    gAudioBridge.reset();
    return ok;
}
extern "C" bool PartyBoard_RollbackAudioBridgeActive(void)
{ return static_cast<bool>(gAudioBridge); }
extern "C" bool PartyBoard_RollbackAudioBridgeHealthy(void)
{ return gAudioBridge && gAudioBridge->healthy(); }
extern "C" bool PartyBoard_RollbackAudioBridgeIsVirtual(s32 handle)
{ FxTicket ticket; return decodeVirtual(handle, ticket); }
extern "C" s32 PartyBoard_RollbackAudioBridgePlay2D(s32 sound, s32 volume, s32 pan)
{
    if (!gAudioBridge) return MSM_ERR_PLAYFAIL;
    const auto ticket = gAudioBridge->play(sound, volume, pan);
    return ticket ? static_cast<s32>(kVirtualTag | ticket.value) : MSM_ERR_PLAYFAIL;
}
extern "C" bool PartyBoard_RollbackAudioBridgeStopFX(s32 handle, s32 fadeMilliseconds)
{
    FxTicket ticket;
    return gAudioBridge && decodeVirtual(handle, ticket)
        && gAudioBridge->stop(ticket, fadeMilliseconds);
}
extern "C" bool PartyBoard_RollbackAudioBridgeParameter(s32 handle, s32 parameter, s32 value)
{
    FxTicket ticket;
    return gAudioBridge && decodeVirtual(handle, ticket)
        && gAudioBridge->parameter(ticket, decodeParameter(parameter), value);
}
extern "C" s32 PartyBoard_RollbackAudioBridgeStatus(s32 handle)
{
    FxTicket ticket;
    if (!gAudioBridge || !decodeVirtual(handle, ticket)) return MSM_SE_DONE;
    return gAudioBridge->logicalState(ticket) == FxLogicalState::Playing
        ? MSM_SE_PLAY : MSM_SE_DONE;
}

extern "C" bool PartyBoard_RollbackAudioSelfTest(void) {
    using namespace partyboard::rollback;
    unsigned checks = 0;
    bool ok = runAudioAdapterTests(checks);
    // Exercise the actual HuAud wrapper and bridge. INT32_MAX reaches MusyX's
    // invalid-ID path without initializing sound or starting a real voice.
    ok &= !PartyBoard_RollbackAudioBridgeActive();
    ok &= PartyBoard_RollbackAudioBridgeStart();
    ok &= PartyBoard_RollbackAudioBridgeFrameBegin(0);
    const s32 handle = HuAudFXPlayVolPan(INT32_MAX, 100, 40);
    ok &= PartyBoard_RollbackAudioBridgeIsVirtual(handle);
    ok &= HuAudFXStatusGet(handle) == MSM_SE_PLAY;
    ok &= HuAudFXVolSet(handle, 70) == 0;
    ok &= PartyBoard_RollbackAudioBridgeFrameEnd();
    ok &= PartyBoard_RollbackAudioBridgeConfirm(1);
    ok &= HuAudFXStatusGet(handle) == MSM_SE_PLAY;
    ok &= PartyBoard_RollbackAudioBridgeFrameBegin(1);
    HuAudFXFadeOut(handle, 100);
    ok &= HuAudFXStatusGet(handle) == MSM_SE_DONE;
    ok &= PartyBoard_RollbackAudioBridgeFrameEnd();
    ok &= PartyBoard_RollbackAudioBridgeConfirm(2);
    ok &= PartyBoard_RollbackAudioBridgeStop();
    ok &= !PartyBoard_RollbackAudioBridgeActive();
    /* A stale ticket after teardown must never reach msmSeStop/status. */
    HuAudFXStop(handle);
    ok &= HuAudFXStatusGet(handle) == MSM_SE_DONE;
    OSReport("Rollback audio adapter: %s (%u mock-backend checks; real HuAud bridge and MusyX invalid-ID path). Confirmed 2D FX, virtual/native handle mapping, deterministic status during simulation; not full audio integration.\n",
        ok ? "PASS" : "FAIL", checks);
    return ok;
}
