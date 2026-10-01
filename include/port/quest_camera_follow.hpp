#pragma once

#include <cmath>

namespace partyboard::quest {
// A minigame seen from its camera's point of view: the model floats where the
// board's screen stands (quest_xr.cpp) and turns so that the player sees it
// as the camera does, from the same side and the same height above the arena.
// Only a camera at rest is followed: fly-overs and orbits leave the model
// still, and small sways are ignored. Boards stay as placed: a board sliding
// or turning with the turns made the player sick (2026-09-28).

constexpr float kPi = 3.14159265358979323846f;

// The camera's turn about the vertical axis, as the board camera's yaw
// (src/game/board/main.c): 0 when it stands on the +Z side of what it looks
// at. From a GX view matrix, whose rows are the camera's axes in the world:
// the back axis, or the up axis for a camera looking straight down.
inline bool camera_yaw(const float view[3][4], float& yaw) {
    const float backX = view[2][0], backZ = view[2][2];
    if (std::isfinite(backX) && std::isfinite(backZ) && std::hypot(backX, backZ) >= 0.3f) {
        yaw = std::atan2(backX, backZ);
        return true;
    }
    const float upX = view[1][0], upZ = view[1][2];
    if (std::isfinite(upX) && std::isfinite(upZ) && std::hypot(upX, upZ) >= 0.3f) {
        yaw = std::atan2(-upX, -upZ);
        return true;
    }
    return false;
}

// How high the camera stands above what it looks at: the angle of its back
// axis above the horizontal, pi/2 looking straight down.
inline bool camera_pitch(const float view[3][4], float& pitch) {
    const float backY = view[2][1];
    if (!std::isfinite(backY)) {
        return false;
    }
    pitch = std::asin(std::fmax(-1.0f, std::fmin(backY, 1.0f)));
    return true;
}

// The rotation (rows) that turns the game world so the camera at `yaw` and
// `pitch` looks along -Z: its back axis goes to +Z, its up to +Y. Seen from
// +Z, the model looks as it does through the camera.
inline void camera_turn(float yaw, float pitch, float rows[3][3]) {
    const float c = std::cos(yaw), s = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
    const float turned[3][3] = {{c, 0.0f, -s}, {-sp * s, cp, -sp * c}, {cp * s, sp, cp * c}};
    for (int r = 0; r < 3; ++r) {
        for (int k = 0; k < 3; ++k) {
            rows[r][k] = turned[r][k];
        }
    }
}

// b - a, the short way round, in -pi..pi.
inline float angle_between(float a, float b) {
    return std::remainder(b - a, 2.0f * kPi);
}

class CameraFollow {
public:
    static constexpr float kRestTolerance = 3.0f * kPi / 180.0f;
    static constexpr float kRestSeconds = 0.4f;
    static constexpr float kMinTurn = 8.0f * kPi / 180.0f;
    static constexpr float kTurnSeconds = 0.35f;
    static constexpr float kMaxTurnSpeed = 90.0f * kPi / 180.0f; // per second

    // Entering a scene: its first view is taken at once.
    void reset(float yaw, float pitch = 0.0f)
    {
        mYaw = mTargetYaw = mRestYaw = std::isfinite(yaw) ? yaw : 0.0f;
        mPitch = mTargetPitch = mRestPitch = std::isfinite(pitch) ? pitch : 0.0f;
        mRestTime = 0.0f;
    }

    // Each frame: the camera's yaw and pitch (NaN when unknown), and the
    // seconds since the previous frame.
    void update(float cameraYaw, float cameraPitch, float dt)
    {
        if (!(dt > 0.0f) || !std::isfinite(dt)) {
            return;
        }
        dt = std::fmin(dt, 0.1f);
        if (std::isfinite(cameraYaw)) {
            const float pitch = std::isfinite(cameraPitch) ? cameraPitch : mRestPitch;
            if (std::abs(angle_between(mRestYaw, cameraYaw)) > kRestTolerance
                || std::abs(pitch - mRestPitch) > kRestTolerance) {
                mRestYaw = cameraYaw;
                mRestPitch = pitch;
                mRestTime = 0.0f;
            } else {
                mRestTime += dt;
                if (mRestTime >= kRestSeconds
                    && (std::abs(angle_between(mTargetYaw, mRestYaw)) >= kMinTurn
                        || std::abs(mRestPitch - mTargetPitch) >= kMinTurn)) {
                    mTargetYaw = mRestYaw;
                    mTargetPitch = mRestPitch;
                }
            }
        }
        const float ease = 1.0f - std::exp(-dt / kTurnSeconds);
        const float maxTurn = kMaxTurnSpeed * dt;
        const float turn = std::fmax(-maxTurn, std::fmin(angle_between(mYaw, mTargetYaw) * ease, maxTurn));
        mYaw = std::remainder(mYaw + turn, 2.0f * kPi);
        mPitch += std::fmax(-maxTurn, std::fmin((mTargetPitch - mPitch) * ease, maxTurn));
    }

    float yaw() const { return mYaw; }
    float pitch() const { return mPitch; }

private:
    float mYaw = 0.0f, mTargetYaw = 0.0f, mRestYaw = 0.0f;
    float mPitch = 0.0f, mTargetPitch = 0.0f, mRestPitch = 0.0f;
    float mRestTime = 0.0f;
};
}
