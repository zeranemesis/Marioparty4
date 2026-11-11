#pragma once

// Table calibration facing direction, independent of OpenXR and controller
// orientation. +Z on the board points toward the seated player.
#include <cmath>

namespace quest {

// The controller supplies the board center; the user's HEAD supplies yaw.
// Looking at the board from beside the table sets its +Z toward the eyes.
// If the eyes are directly over the center, use the headset's back (+Z)
// direction, which is opposite the direction the user is looking.
// Do not use the controller's yaw: its rotation on the table is arbitrary.
inline bool table_yaw_from_head(float centerX, float centerZ, float eyeX, float eyeZ,
                                float headsetBackYaw, float& yaw) {
  if (std::isfinite(centerX) && std::isfinite(centerZ) &&
      std::isfinite(eyeX) && std::isfinite(eyeZ)) {
    const float dx = eyeX - centerX, dz = eyeZ - centerZ;
    if (std::hypot(dx, dz) >= 0.05f) {
      yaw = std::atan2(dx, dz);
      return true;
    }
  }
  if (std::isfinite(headsetBackYaw)) {
    yaw = headsetBackYaw;
    return true;
  }
  return false;
}

} // namespace quest
