#ifndef PARTYBOARD_ROLLBACK_AUDIO_HPP
#define PARTYBOARD_ROLLBACK_AUDIO_HPP
#include "port/rollback_confirmed_effects.hpp"
#include <map>

namespace partyboard::rollback {
// A ticket is deliberately not implicitly convertible to a MusyX handle.
struct FxTicket { std::uint32_t value = 0; explicit operator bool() const { return value != 0; } };
enum class FxParameter : std::int32_t { Volume, Pan, Pitch };
struct FxBackend {
    std::function<std::int32_t(std::int32_t, std::int32_t, std::int32_t)> play;
    std::function<std::int32_t(std::int32_t, std::int32_t)> stop;
    std::function<std::int32_t(std::int32_t, FxParameter, std::int32_t)> parameter;
    std::function<std::int32_t(std::int32_t)> status;
};
enum class FxObservationState { Ready, AwaitConfirmation, UnknownTicket, Fault };
enum class FxLogicalState { Unknown, Playing, Stopped };
struct FxObservation { FxObservationState state; std::int32_t status = 0, error = 0; };

// Experimental 2D FX adapter. One instance per session, outside snapshots.
// The game restores its tickets in its snapshots; deterministic frame/ordinal
// tickets allow corrected, uncommitted plays to replace earlier predictions.
// Backend handles never enter simulation state. Natural completion is an
// external observation: observe() is for presentation/confirmed coordination,
// NEVER input to predicted logic without a separate synchronized observation.
// Stop/fade and parameter commands can refer to a still-unconfirmed play.
// No group loads, music, streams, listeners or existing native voices adopted.
class ConfirmedAudioFx {
public:
    explicit ConfirmedAudioFx(FxBackend backend, std::size_t maxVoices = 512,
        std::uint32_t firstFrame = 0)
        : mBackend(std::move(backend)), mQueue(13, 1024, firstFrame), mMaxVoices(maxVoices) {}
    bool beginFrame(std::uint32_t frame) {
        if (!healthy() || mBusy || mOpen || frame > 0x3ffffeu || !mQueue.beginFrame(frame)) return fail();
        for (auto it = mVoices.begin(); it != mVoices.end();) {
            if (!it->second.committed && it->second.frame >= frame) it = mVoices.erase(it);
            else {
                auto &stops = it->second.stopFrames;
                while (!stops.empty() && stops.back() >= frame) stops.pop_back();
                ++it;
            }
        }
        mFrame = frame; mOrdinal = 0; mOpen = true; return true;
    }
    FxTicket play(std::int32_t sound, std::int32_t volume = 127, std::int32_t pan = 64) {
        if (!healthy() || !mOpen || mBusy || mOrdinal >= 256 || mVoices.size() >= mMaxVoices
            || volume < 0 || volume > 127 || pan < 0 || pan > 127) { fail(); return {}; }
        FxTicket ticket {((mFrame + 1) << 8) | mOrdinal++};
        try {
            if (!mVoices.emplace(ticket.value, Voice {mFrame}).second
                || !mQueue.append(EffectCommand::make(EffectKind::FxPlay,
                    {ticket.value, sound, volume, pan}))) { fail(); return {}; }
        } catch (...) { fail(); return {}; }
        return ticket;
    }
    bool stop(FxTicket ticket, std::int32_t fadeMilliseconds = 0) {
        if (!canCommand(ticket) || fadeMilliseconds < 0) return fail();
        auto &voice = mVoices.find(ticket.value)->second;
        try { voice.stopFrames.push_back(mFrame); }
        catch (...) { return fail(); }
        if (!mQueue.append(EffectCommand::make(EffectKind::FxStop,
                {ticket.value, fadeMilliseconds}))) return fail();
        return true;
    }
    bool parameter(FxTicket ticket, FxParameter kind, std::int32_t value) {
        const bool valid = (kind == FxParameter::Pitch && value >= -8192 && value <= 8191)
            || ((kind == FxParameter::Volume || kind == FxParameter::Pan) && value >= 0 && value <= 127);
        if (!canCommand(ticket) || !valid) return fail();
        return mQueue.append(EffectCommand::make(EffectKind::FxParameter,
            {ticket.value, static_cast<std::int32_t>(kind), value})) || fail();
    }
    bool endFrame() {
        if (!healthy() || !mOpen || mBusy || !mQueue.endFrame()) return fail();
        mOpen = false; return true;
    }
    bool confirmThrough(std::uint32_t boundary) {
        if (!healthy() || mOpen || mBusy || !mBackend.play || !mBackend.stop
            || !mBackend.parameter || !mBackend.status) return fail();
        mBusy = true;
        const bool ok = mQueue.confirmThrough(boundary, [&](const ConfirmedEffect &event) {
            const auto &args = event.command.arguments;
            auto found = mVoices.find(static_cast<std::uint32_t>(args[0]));
            if (found == mVoices.end()) return false;
            auto &voice = found->second;
            switch (event.command.kind) {
            case EffectKind::FxPlay:
                if (voice.committed) return false;
                voice.native = mBackend.play(static_cast<std::int32_t>(args[1]),
                    static_cast<std::int32_t>(args[2]), static_cast<std::int32_t>(args[3]));
                voice.committed = true;
                voice.error = voice.native < 0 ? voice.native : 0;
                break;
            case EffectKind::FxStop:
                if (!voice.committed) return false;
                if (voice.native >= 0) voice.error = mBackend.stop(voice.native, static_cast<std::int32_t>(args[1]));
                break;
            case EffectKind::FxParameter:
                if (!voice.committed) return false;
                if (voice.native >= 0) voice.error = mBackend.parameter(voice.native,
                    static_cast<FxParameter>(args[1]), static_cast<std::int32_t>(args[2]));
                break;
            default: return false;
            }
            return !mFault;
        });
        mBusy = false;
        return ok || fail();
    }
    FxObservation observe(FxTicket ticket) {
        if (!healthy()) return {FxObservationState::Fault};
        // Even a committed voice must not read a changing device while a
        // simulation/replay frame is open.
        if (mOpen || mBusy) return {FxObservationState::AwaitConfirmation};
        auto found = mVoices.find(ticket.value);
        if (found == mVoices.end()) return {FxObservationState::UnknownTicket};
        const auto &voice = found->second;
        if (!voice.committed) return {FxObservationState::AwaitConfirmation};
        if (voice.native < 0) return {FxObservationState::Ready, 0, voice.error};
        mBusy = true;
        try {
            const auto status = mBackend.status(voice.native);
            mBusy = false;
            if (!healthy()) return {FxObservationState::Fault};
            return {FxObservationState::Ready, status, voice.error};
        } catch (...) { mBusy = false; fail(); return {FxObservationState::Fault}; }
    }
    // Explicit retirement only after natural completion or a failed play.
    // No active voice is evicted merely to meet the bound; old tickets cannot
    // control a later sound. The owner must no longer reference a retired one.
    bool retire(FxTicket ticket) {
        // Pending commands/snapshots may still reference this mapping.
        if (mOpen || mBusy || mQueue.frameCount() != 0) return false;
        const auto result = observe(ticket);
        if (result.state != FxObservationState::Ready || result.status != 0) return false;
        mVoices.erase(ticket.value); return true;
    }
    // Explicit session teardown: discard predictions and stop committed
    // voices. Also usable after a fault. Never emit a queued speculative play.
    bool close() {
        if (mBusy) return false;
        if (mClosed) return mCloseResult;
        mClosed = true; mOpen = false; mBusy = true;
        bool ok = true;
        for (auto &entry : mVoices) {
            auto &voice = entry.second;
            if (!voice.committed || voice.native < 0) continue;
            const auto native = voice.native;
            voice.native = -1; // Do not retry an ambiguous backend operation.
            try { if (!mBackend.stop || mBackend.stop(native, 0) < 0) ok = false; }
            catch (...) { ok = false; }
        }
        mBusy = false;
        return mCloseResult = ok;
    }
    bool healthy() const { return !mFault && !mClosed && mQueue.healthy(); }
    FxLogicalState logicalState(FxTicket ticket) const {
        const auto found = mVoices.find(ticket.value);
        if (found == mVoices.end()) return FxLogicalState::Unknown;
        return found->second.stopFrames.empty() ? FxLogicalState::Playing : FxLogicalState::Stopped;
    }
    std::size_t voiceCount() const { return mVoices.size(); }
private:
    struct Voice {
        std::uint32_t frame;
        std::int32_t native = -1, error = 0;
        bool committed = false;
        std::vector<std::uint32_t> stopFrames;
    };
    bool fail() { mFault = true; return false; }
    bool canCommand(FxTicket ticket) const {
        return healthy() && mOpen && !mBusy && mVoices.find(ticket.value) != mVoices.end();
    }
    FxBackend mBackend;
    ConfirmedEffects mQueue;
    std::map<std::uint32_t, Voice> mVoices;
    std::size_t mMaxVoices;
    std::uint32_t mFrame = 0, mOrdinal = 0;
    bool mOpen = false, mBusy = false, mFault = false, mClosed = false;
    bool mCloseResult = true;
};
FxBackend makeNativeFxBackend();
}
extern "C" bool PartyBoard_RollbackAudioSelfTest(void);
#endif
