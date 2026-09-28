#pragma once

// OpenXR set-up shared by the Meta Quest sources: headers, error logging,
// extension function lookup and the little pose math placement needs.

#include <jni.h>
#include <android/log.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <cmath>

#define LOG_TAG "PartyBoardQuest"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

namespace quest {

inline bool check(XrInstance instance, XrResult result, const char* what) {
  if (XR_SUCCEEDED(result)) {
    return true;
  }
  char name[XR_MAX_RESULT_STRING_SIZE] = "?";
  if (instance != XR_NULL_HANDLE) {
    xrResultToString(instance, result, name);
  }
  LOGW("%s failed: %s (%d)", what, name, static_cast<int>(result));
  return false;
}

// An extension function, or nullptr when the runtime does not have it.
template <typename Fn>
Fn proc(XrInstance instance, const char* name) {
  PFN_xrVoidFunction fn = nullptr;
  if (XR_FAILED(xrGetInstanceProcAddr(instance, name, &fn))) {
    return nullptr;
  }
  return reinterpret_cast<Fn>(fn);
}

// --- Poses. Y is up (STAGE space: the floor of the play area). ---

inline XrPosef identity_pose() {
  XrPosef pose{};
  pose.orientation.w = 1.0f;
  return pose;
}

inline XrVector3f add(XrVector3f a, XrVector3f b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline XrVector3f cross(XrVector3f a, XrVector3f b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

inline XrQuaternionf multiply(XrQuaternionf a, XrQuaternionf b) {
  return {
      a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
      a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
      a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
      a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
  };
}

inline XrQuaternionf conjugate(XrQuaternionf q) { return {-q.x, -q.y, -q.z, q.w}; }

inline XrVector3f rotate(XrQuaternionf q, XrVector3f v) {
  const XrVector3f u{q.x, q.y, q.z};
  XrVector3f t = cross(u, v);
  t = {t.x * 2.0f, t.y * 2.0f, t.z * 2.0f};
  const XrVector3f c = cross(u, t);
  return {v.x + q.w * t.x + c.x, v.y + q.w * t.y + c.y, v.z + q.w * t.z + c.z};
}

// b, given in a's frame, expressed in a's parent.
inline XrPosef compose(const XrPosef& a, const XrPosef& b) {
  XrPosef out;
  out.orientation = multiply(a.orientation, b.orientation);
  out.position = add(a.position, rotate(a.orientation, b.position));
  return out;
}

inline XrPosef inverse(const XrPosef& p) {
  XrPosef out;
  out.orientation = conjugate(p.orientation);
  const XrVector3f back = rotate(out.orientation, p.position);
  out.position = {-back.x, -back.y, -back.z};
  return out;
}

// A turn about the vertical axis: +Z goes to (sin yaw, 0, cos yaw).
inline XrQuaternionf yaw_rotation(float yaw) { return {0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f)}; }

inline float yaw_of(XrQuaternionf q) {
  const XrVector3f forward = rotate(q, {0.0f, 0.0f, 1.0f});
  return std::atan2(forward.x, forward.z);
}

// The pose kept upright: only its turn about the vertical axis remains.
inline XrPosef upright(const XrPosef& p) {
  XrPosef out;
  out.orientation = yaw_rotation(yaw_of(p.orientation));
  out.position = p.position;
  return out;
}

// Upright at `at`, its +Z (the screen's front) turned toward `toward`.
inline XrPosef facing(XrVector3f at, XrVector3f toward) {
  XrPosef out;
  out.orientation = yaw_rotation(std::atan2(toward.x - at.x, toward.z - at.z));
  out.position = at;
  return out;
}

} // namespace quest
