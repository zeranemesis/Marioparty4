#pragma once

#include <algorithm>
#include <cmath>

namespace quest {
// A minigame seen through a window (StereoView::set_window): each eye's field
// of view is the window's edges as seen from where that eye is, off center
// as soon as the eye is. In the window's frame (its center, +X right, +Y up,
// +Z toward the player), `eye` is the eye's position; the tangents are those
// of the angles to the window's left, right, top and bottom edges, as an
// XrFovf's (left and down negative while the eye is between the edges).
struct WindowTangents {
  float left = 0, right = 0, up = 0, down = 0;
};

inline WindowTangents window_tangents(float halfWidth, float halfHeight, const float eye[3]) {
  // Through or behind the window: as if just in front of it.
  const float distance = std::max(eye[2], 0.05f);
  return {(-halfWidth - eye[0]) / distance, (halfWidth - eye[0]) / distance, (halfHeight - eye[1]) / distance,
          (-halfHeight - eye[1]) / distance};
}
} // namespace quest
