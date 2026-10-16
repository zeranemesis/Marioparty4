#pragma once

#include <cstdint>
#include <cstdlib>

#include "port/rollback.h"

namespace partyboard::netplay {

// An online board waits on one player at a time. When that player stops
// touching the controller, everyone else is stuck. After 30 seconds without
// input the screen shows a 60 second countdown, and when it runs out the
// seat becomes a CPU (normal level) and leaves the lockstep: nobody waits for
// its input any more.
//
// Everything here runs on the inputs every peer applied for the same frame and
// on game state every peer holds, so the same seat is replaced at the same frame
// everywhere. Nothing reads a clock: a stalled network tick does not count as
// idleness, and a desync cannot come from here.
constexpr std::uint32_t kIdleTickRate = 60; // netplay always simulates at 60 Hz
constexpr std::uint32_t kIdleGraceFrames = 30 * kIdleTickRate;
constexpr std::uint32_t kIdleCountdownFrames = 60 * kIdleTickRate;

// Any button, or a stick or trigger pushed past what a resting pad drifts to.
inline bool idleInputActive(const PartyBoardRollbackInput &input)
{
    constexpr int kStick = 40, kTrigger = 100;
    return input.buttons != 0 || std::abs(input.stickX) >= kStick || std::abs(input.stickY) >= kStick
        || std::abs(input.substickX) >= kStick || std::abs(input.substickY) >= kStick
        || input.triggerLeft >= kTrigger || input.triggerRight >= kTrigger;
}

class IdleWatch {
public:
    // One call per applied frame. turnSeat is the seat the board is waiting on
    // (-1 outside a board, during a CPU's turn, or between turns); active says
    // whether that seat's input for this frame did anything. Returns the seat
    // to replace on this very frame, or -1.
    int observe(int turnSeat, bool active)
    {
        if (turnSeat < 0 || turnSeat >= 8 || kicked(turnSeat)) {
            seat_ = -1;
            idle_ = 0;
            return -1;
        }
        if (turnSeat != seat_) {
            seat_ = turnSeat;
            idle_ = 0;
        }
        if (active) {
            idle_ = 0;
            return -1;
        }
        if (++idle_ < kIdleGraceFrames + kIdleCountdownFrames) return -1;
        kicked_ |= static_cast<std::uint8_t>(1u << turnSeat);
        seat_ = -1;
        idle_ = 0;
        return turnSeat;
    }
    // The seat being counted down, or -1 while nobody is past the grace period.
    int countdownSeat() const { return seat_ >= 0 && idle_ >= kIdleGraceFrames ? seat_ : -1; }
    // Whole seconds left, rounded up so the last second shows 1, never 0.
    int countdownSeconds() const
    {
        if (countdownSeat() < 0) return -1;
        const auto left = kIdleGraceFrames + kIdleCountdownFrames - idle_;
        return static_cast<int>((left + kIdleTickRate - 1) / kIdleTickRate);
    }
    bool kicked(int seat) const { return seat >= 0 && seat < 8 && ((kicked_ >> seat) & 1u) != 0; }
    std::uint8_t kickedMask() const { return kicked_; }

private:
    int seat_ = -1;
    std::uint32_t idle_ = 0;
    std::uint8_t kicked_ = 0;
};

} // namespace partyboard::netplay
