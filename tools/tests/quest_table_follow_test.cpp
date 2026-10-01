// The room scan's tables and the model following the game camera
// (table_fit.hpp, quest_camera_follow.hpp): plain math, run on a PC.
#include "../../platforms/android/app/src/main/cpp/table_fit.hpp"
#include "../../include/port/quest_camera_follow.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {

bool near(float a, float b, float tolerance = 0.001f) { return std::abs(a - b) <= tolerance; }

// A quaternion turning by `angle` about a unit axis.
void axis_angle(float x, float y, float z, float angle, float out[4]) {
  const float s = std::sin(angle * 0.5f);
  out[0] = x * s;
  out[1] = y * s;
  out[2] = z * s;
  out[3] = std::cos(angle * 0.5f);
}

void test_table_from_plane() {
  using quest::ScannedTable;
  // Meta's table planes: +Z (the normal) up, i.e. -90 degrees about X.
  float up[4];
  axis_angle(1, 0, 0, -partyboard::quest::kPi * 0.5f, up);
  const float at[3] = {1.0f, 0.75f, -2.0f};
  ScannedTable table;
  // 1.6 m along the plane's X, 0.9 m along its Y (which points to -Z in STAGE).
  assert(quest::table_from_plane(at, up, -0.8f, -0.45f, 1.6f, 0.9f, table));
  assert(near(table.centerX, 1.0f) && near(table.centerZ, -2.0f) && near(table.height, 0.75f));
  assert(near(table.halfLength, 0.8f) && near(table.halfWidth, 0.45f));
  assert(near(std::abs(table.axisX), 1.0f) && near(table.axisZ, 0.0f));

  // Bounds off the plane's origin: the center moves with them.
  assert(quest::table_from_plane(at, up, 0.0f, 0.0f, 1.6f, 0.9f, table));
  assert(near(table.centerX, 1.8f) && near(table.centerZ, -2.45f));

  // Deeper than wide: the long axis is the plane's Y, along STAGE Z.
  assert(quest::table_from_plane(at, up, -0.4f, -0.7f, 0.8f, 1.4f, table));
  assert(near(table.halfLength, 0.7f) && near(table.halfWidth, 0.4f));
  assert(near(table.axisX, 0.0f) && near(std::abs(table.axisZ), 1.0f));

  // Normal down (the other convention) is still a table; a wall is not.
  float down[4];
  axis_angle(1, 0, 0, partyboard::quest::kPi * 0.5f, down);
  assert(quest::table_from_plane(at, down, -0.8f, -0.45f, 1.6f, 0.9f, table));
  assert(near(table.height, 0.75f));
  const float wall[4] = {0, 0, 0, 1}; // +Z horizontal
  assert(!quest::table_from_plane(at, wall, -0.8f, -0.45f, 1.6f, 0.9f, table));
  assert(!quest::table_from_plane(at, up, 0, 0, 0.0f, 0.9f, table));
  const float nan[3] = {NAN, 0, 0};
  assert(!quest::table_from_plane(nan, up, 0, 0, 1.6f, 0.9f, table));
}

void test_pick_and_scale() {
  using quest::ScannedTable;
  ScannedTable dining; // 1.6 x 0.9 m at the origin, 0.75 m high
  dining.halfLength = 0.8f;
  dining.halfWidth = 0.45f;
  dining.height = 0.75f;
  ScannedTable coffee; // 0.8 x 0.5 m, turned 90 degrees, 0.45 m high, 3 m away
  coffee.centerX = 3.0f;
  coffee.axisX = 0.0f;
  coffee.axisZ = 1.0f;
  coffee.halfLength = 0.4f;
  coffee.halfWidth = 0.25f;
  coffee.height = 0.45f;
  const std::vector<ScannedTable> tables{dining, coffee};

  assert(quest::pick_table(tables, 0.1f, 0.76f, 0.1f) == &tables[0]);
  assert(quest::pick_table(tables, 0.83f, 0.75f, 0.0f) == &tables[0]);  // within the margin
  assert(quest::pick_table(tables, 0.9f, 0.75f, 0.0f) == nullptr);      // off the edge
  assert(quest::pick_table(tables, 0.0f, 0.95f, 0.0f) == nullptr);      // too high above it
  assert(quest::pick_table(tables, 3.2f, 0.47f, 0.3f) == &tables[1]);   // turned: 0.3 m along its length
  assert(quest::pick_table(tables, 3.28f, 0.47f, 0.0f) == &tables[1]); // within the margin
  assert(quest::pick_table(tables, 3.0f, 0.47f, 0.5f) == nullptr);
  assert(quest::pick_table({}, 0, 0.75f, 0) == nullptr);

  // Two tops at the same place: the one nearer in height.
  ScannedTable shelf = dining;
  shelf.height = 0.83f;
  const std::vector<ScannedTable> stacked{shelf, dining};
  assert(quest::pick_table(stacked, 0, 0.77f, 0) == &stacked[1]);

  // At the center of the dining table: the scene spans its width (0.9 m).
  const float scale = quest::scale_for_table(dining, 0, 0, 0.00003f, 0.002f);
  assert(near(scale * quest::kSceneExtentUnits, 0.95f * 0.9f));
  // Off center, the nearest edge limits it.
  const float off = quest::scale_for_table(dining, 0.6f, 0, 0.00003f, 0.002f);
  assert(near(off * quest::kSceneExtentUnits, 0.95f * 0.4f));
  // Past the edge: the smallest model, never a negative one.
  assert(quest::scale_for_table(dining, 2.0f, 0, 0.00003f, 0.002f) == 0.00003f);
}

// A GX look-at view from a camera `yaw` around the target, `pitch` down.
void look_at(float yaw, float pitch, float view[3][4]) {
  const float back[3] = {std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch)};
  // right = up x back, up = back x right, as C_MTXLookAt.
  float right[3] = {back[2], 0.0f, -back[0]};
  float length = std::hypot(right[0], right[2]);
  if (length < 1e-4f) { // straight down: the screen's top is ahead (-Z turned by yaw)
    right[0] = std::cos(yaw);
    right[2] = -std::sin(yaw);
    length = 1.0f;
  }
  for (float& r : right) r /= length;
  const float up[3] = {back[1] * right[2] - back[2] * right[1], back[2] * right[0] - back[0] * right[2],
                       back[0] * right[1] - back[1] * right[0]};
  for (int c = 0; c < 3; ++c) {
    view[0][c] = right[c];
    view[1][c] = up[c];
    view[2][c] = back[c];
  }
  view[0][3] = view[1][3] = view[2][3] = 0.0f;
}

void test_camera_yaw() {
  using partyboard::quest::camera_yaw;
  using partyboard::quest::kPi;
  float view[3][4];
  float yaw = 99;
  look_at(0.0f, 0.7f, view); // the board camera: behind the player, looking down
  assert(camera_yaw(view, yaw) && near(yaw, 0.0f));
  look_at(kPi * 0.5f, 0.5f, view);
  assert(camera_yaw(view, yaw) && near(yaw, kPi * 0.5f));
  look_at(-2.5f, 0.3f, view);
  assert(camera_yaw(view, yaw) && near(yaw, -2.5f));
  look_at(1.0f, kPi * 0.5f, view); // straight down: from its up axis
  assert(camera_yaw(view, yaw) && near(yaw, 1.0f, 0.01f));
  float broken[3][4] = {};
  assert(!camera_yaw(broken, yaw));
}

void run(partyboard::quest::CameraFollow& follow, float yaw, float seconds) {
  for (float t = 0; t < seconds; t += 1.0f / 60.0f) follow.update(yaw, 0.0f, 1.0f / 60.0f);
}

void test_follow() {
  using partyboard::quest::CameraFollow;
  using partyboard::quest::kPi;
  CameraFollow follow;
  follow.reset(0.5f); // entering a scene: at once
  assert(near(follow.yaw(), 0.5f));

  // An orbit (the camera never rests) leaves the model still.
  for (int frame = 0; frame < 600; ++frame) {
    follow.update(0.5f + frame * 0.02f, 0.0f, 1.0f / 60.0f);
  }
  assert(near(follow.yaw(), 0.5f));

  // A small sway at rest is ignored.
  follow.reset(0.0f);
  run(follow, 0.1f, 2.0f); // under 8 degrees
  assert(near(follow.yaw(), 0.0f));

  // A camera at rest elsewhere: the model turns there, not at once.
  follow.reset(0.0f);
  run(follow, 1.2f, 0.3f);
  assert(near(follow.yaw(), 0.0f)); // not at rest long enough yet
  run(follow, 1.2f, 0.3f);
  assert(follow.yaw() > 0.0f && follow.yaw() < 1.2f);
  run(follow, 1.2f, 3.0f);
  assert(near(follow.yaw(), 1.2f, 0.01f));

  // Across +-180 degrees: the short way round.
  follow.reset(3.0f);
  run(follow, -3.0f, 0.6f);
  assert(follow.yaw() > 3.0f || follow.yaw() < -3.0f);
  run(follow, -3.0f, 3.0f);
  assert(near(follow.yaw(), -3.0f, 0.01f));

  // Never faster than 90 degrees a second.
  follow.reset(0.0f);
  run(follow, 3.0f, 0.4f); // at rest now
  const float before = follow.yaw();
  follow.update(3.0f, 0.0f, 0.1f);
  assert(follow.yaw() - before <= CameraFollow::kMaxTurnSpeed * 0.1f + 0.0001f);

  // No camera (NaN), or bad frame times: nothing moves.
  follow.reset(1.0f);
  run(follow, NAN, 1.0f);
  follow.update(2.0f, 0.0f, NAN);
  follow.update(2.0f, 0.0f, -1.0f);
  assert(near(follow.yaw(), 1.0f));

  // The camera's height above the arena is followed like its side.
  follow.reset(0.0f, 0.7f);
  assert(near(follow.pitch(), 0.7f));
  for (float t = 0; t < 0.3f; t += 1.0f / 60.0f) follow.update(0.0f, 1.2f, 1.0f / 60.0f);
  assert(near(follow.pitch(), 0.7f)); // not at rest long enough yet
  for (float t = 0; t < 3.0f; t += 1.0f / 60.0f) follow.update(0.0f, 1.2f, 1.0f / 60.0f);
  assert(near(follow.pitch(), 1.2f, 0.01f) && near(follow.yaw(), 0.0f));
  // A small change of height at rest is ignored; an unknown one keeps it.
  for (float t = 0; t < 2.0f; t += 1.0f / 60.0f) follow.update(0.0f, 1.25f, 1.0f / 60.0f);
  for (float t = 0; t < 2.0f; t += 1.0f / 60.0f) follow.update(0.0f, NAN, 1.0f / 60.0f);
  assert(near(follow.pitch(), 1.2f, 0.01f));
}

// Turned by camera_turn, the camera looks along -Z: its back axis goes to +Z
// and its up to +Y, whatever its side and height (straight down included).
void test_camera_turn() {
  using partyboard::quest::camera_pitch;
  using partyboard::quest::camera_turn;
  using partyboard::quest::camera_yaw;
  using partyboard::quest::kPi;
  const float cases[][2] = {{0.0f, 0.7f}, {1.3f, 0.4f}, {-2.6f, 1.0f}, {0.8f, kPi * 0.5f}, {3.0f, 0.0f}};
  for (const auto& angles : cases) {
    float view[3][4];
    look_at(angles[0], angles[1], view);
    float yaw = 0, pitch = 0, rows[3][3];
    assert(camera_yaw(view, yaw) && camera_pitch(view, pitch));
    assert(near(pitch, angles[1], 0.01f));
    camera_turn(yaw, pitch, rows);
    for (int axis = 1; axis <= 2; ++axis) { // up, back
      float turned[3];
      for (int r = 0; r < 3; ++r) {
        turned[r] = rows[r][0] * view[axis][0] + rows[r][1] * view[axis][1] + rows[r][2] * view[axis][2];
      }
      assert(near(turned[0], 0.0f, 0.01f));
      assert(near(turned[1], axis == 1 ? 1.0f : 0.0f, 0.01f));
      assert(near(turned[2], axis == 2 ? 1.0f : 0.0f, 0.01f));
    }
  }
  float broken[3][4] = {};
  broken[2][1] = NAN;
  float pitch = 0;
  assert(!camera_pitch(broken, pitch));
}

} // namespace

int main() {
  test_table_from_plane();
  test_pick_and_scale();
  test_camera_yaw();
  test_follow();
  test_camera_turn();
  std::puts("quest table and camera follow: PASS");
  return 0;
}
