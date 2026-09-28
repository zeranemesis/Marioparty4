#pragma once

#include <cmath>

namespace partyboard::quest {
// The model on the table seen from the game camera's side: it turns so that
// what the camera looks at lies ahead of the player (the stick's "up" goes away
// from them, as on the screen), and on a board the player whose turn it is
// comes to the table's center. Only a camera at rest is followed: fly-overs
// and orbits leave the model still, and small sways are ignored.

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
    // The focus is in the fitted scene's units (2800 across the scene).
    static constexpr float kFocusDeadZone = 100.0f;
    static constexpr float kFocusSeconds = 0.6f;
    static constexpr float kMaxFocusSpeed = 1400.0f; // per second

    // Entering a scene: its first view is taken at once.
    void reset(float yaw, float focusX, float focusZ)
    {
        mYaw = mTargetYaw = mRestYaw = std::isfinite(yaw) ? yaw : 0.0f;
        mRestTime = 0.0f;
        mFocusX = mTargetX = focusX;
        mFocusZ = mTargetZ = focusZ;
    }

    // Each frame: the camera's yaw (NaN when unknown), the point to bring to
    // the table's center, and the seconds since the previous frame.
    void update(float cameraYaw, float focusX, float focusZ, float dt)
    {
        if (!(dt > 0.0f) || !std::isfinite(dt)) {
            return;
        }
        dt = std::fmin(dt, 0.1f);
        if (std::isfinite(cameraYaw)) {
            if (std::abs(angle_between(mRestYaw, cameraYaw)) > kRestTolerance) {
                mRestYaw = cameraYaw;
                mRestTime = 0.0f;
            } else {
                mRestTime += dt;
                if (mRestTime >= kRestSeconds && std::abs(angle_between(mTargetYaw, mRestYaw)) >= kMinTurn) {
                    mTargetYaw = mRestYaw;
                }
            }
        }
        const float ease = 1.0f - std::exp(-dt / kTurnSeconds);
        float turn = angle_between(mYaw, mTargetYaw) * ease;
        const float maxTurn = kMaxTurnSpeed * dt;
        turn = std::fmax(-maxTurn, std::fmin(turn, maxTurn));
        mYaw = std::remainder(mYaw + turn, 2.0f * kPi);

        if (std::isfinite(focusX) && std::isfinite(focusZ)
            && std::hypot(focusX - mTargetX, focusZ - mTargetZ) > kFocusDeadZone) {
            mTargetX = focusX;
            mTargetZ = focusZ;
        }
        const float slide = 1.0f - std::exp(-dt / kFocusSeconds);
        float dx = (mTargetX - mFocusX) * slide, dz = (mTargetZ - mFocusZ) * slide;
        const float step = std::hypot(dx, dz), maxStep = kMaxFocusSpeed * dt;
        if (step > maxStep) {
            dx *= maxStep / step;
            dz *= maxStep / step;
        }
        mFocusX += dx;
        mFocusZ += dz;
    }

    float yaw() const { return mYaw; }
    float focus_x() const { return mFocusX; }
    float focus_z() const { return mFocusZ; }

private:
    float mYaw = 0.0f, mTargetYaw = 0.0f;
    float mRestYaw = 0.0f, mRestTime = 0.0f;
    float mFocusX = 0.0f, mFocusZ = 0.0f, mTargetX = 0.0f, mTargetZ = 0.0f;
};
}
