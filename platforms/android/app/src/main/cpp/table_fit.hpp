#pragma once

// The tables of the headset's room scan (table_scene.cpp), and where the game
// fits on the one the player lays the controller on. Plain math with no
// OpenXR, so tools/tests/quest_table_follow_test.cpp runs it on a PC.

#include <cmath>
#include <vector>

namespace quest {

// A table top in STAGE space (Y up, meters).
struct ScannedTable {
  float centerX = 0, centerZ = 0;
  float height = 0;
  float axisX = 1, axisZ = 0; // unit, along the length
  float halfLength = 0, halfWidth = 0;
};

// A scene plane's pose (position, quaternion x y z w) and its 2D bounds in its
// own XY plane (offset: the lower corner; +Z: the plane's normal) as a table
// top. False unless the plane lies flat (a table's normal points up, but
// either side is taken) and has a size.
inline bool table_from_plane(const float position[3], const float orientation[4], float offsetX, float offsetY,
                             float width, float depth, ScannedTable& out) {
  const float qx = orientation[0], qy = orientation[1], qz = orientation[2], qw = orientation[3];
  const float norm = std::sqrt(qx * qx + qy * qy + qz * qz + qw * qw);
  if (!std::isfinite(norm) || norm < 0.5f || !(width > 0.05f) || !(depth > 0.05f) || !std::isfinite(offsetX) ||
      !std::isfinite(offsetY) || !std::isfinite(position[0]) || !std::isfinite(position[1]) ||
      !std::isfinite(position[2])) {
    return false;
  }
  const float x = qx / norm, y = qy / norm, z = qz / norm, w = qw / norm;
  // The rotation's columns: the plane's X, Y and Z axes in STAGE space.
  const float ax[3] = {1 - 2 * (y * y + z * z), 2 * (x * y + w * z), 2 * (x * z - w * y)};
  const float ay[3] = {2 * (x * y - w * z), 1 - 2 * (x * x + z * z), 2 * (y * z + w * x)};
  const float az[3] = {2 * (x * z + w * y), 2 * (y * z - w * x), 1 - 2 * (x * x + y * y)};
  if (std::abs(az[1]) < 0.9f) {
    return false; // a wall, or a slope
  }
  const float cx = offsetX + width * 0.5f, cy = offsetY + depth * 0.5f;
  out.centerX = position[0] + ax[0] * cx + ay[0] * cy;
  out.height = position[1] + ax[1] * cx + ay[1] * cy;
  out.centerZ = position[2] + ax[2] * cx + ay[2] * cy;
  // The longer side's axis, flattened.
  const float* along = width >= depth ? ax : ay;
  const float flat = std::hypot(along[0], along[2]);
  if (!(flat > 0.1f)) {
    return false;
  }
  out.axisX = along[0] / flat;
  out.axisZ = along[2] / flat;
  out.halfLength = (width >= depth ? width : depth) * 0.5f;
  out.halfWidth = (width >= depth ? depth : width) * 0.5f;
  return true;
}

// How far (x, z) is inside the table's edges: negative outside.
inline float inside_distance(const ScannedTable& table, float x, float z) {
  const float dx = x - table.centerX, dz = z - table.centerZ;
  const float along = dx * table.axisX + dz * table.axisZ;
  const float across = -dx * table.axisZ + dz * table.axisX;
  return std::fmin(table.halfLength - std::abs(along), table.halfWidth - std::abs(across));
}

// The controller lies on this table: its point within the top (up to
// kMarginMeters past an edge) and near its height. Of several, the nearest in
// height; nullptr for none.
constexpr float kTableMarginMeters = 0.05f;
constexpr float kTableHeightToleranceMeters = 0.10f;

inline const ScannedTable* pick_table(const std::vector<ScannedTable>& tables, float x, float y, float z) {
  const ScannedTable* best = nullptr;
  float bestGap = kTableHeightToleranceMeters;
  for (const ScannedTable& table : tables) {
    const float gap = std::abs(table.height - y);
    if (gap <= bestGap && inside_distance(table, x, z) >= -kTableMarginMeters) {
      best = &table;
      bestGap = gap;
    }
  }
  return best;
}

// The model's meters per game unit so that a scene (2800 units across, as
// quest_scene_fit.hpp fits boards and minigames) spans the table from `x, z`
// to its nearest edge on each side, with a little room left.
constexpr float kSceneExtentUnits = 2800.0f;

inline float scale_for_table(const ScannedTable& table, float x, float z, float minimum, float maximum) {
  const float room = std::fmax(inside_distance(table, x, z), 0.0f);
  const float scale = 0.95f * 2.0f * room / kSceneExtentUnits;
  return std::fmin(std::fmax(scale, minimum), maximum);
}

} // namespace quest
