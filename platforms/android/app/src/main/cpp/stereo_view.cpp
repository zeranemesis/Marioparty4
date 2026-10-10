#include "stereo_view.hpp"
#include "window_view.hpp"

#include <GLES2/gl2ext.h>
#include <unistd.h>
#include <poll.h>
#include <linux/sync_file.h>
#include <sys/ioctl.h>
#include <sys/system_properties.h>
#include <cstdlib>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <thread>

namespace quest {

StereoView* g_stereoView = nullptr;

namespace {

// The eyes' depth range, in meters: from a hand's width to across the room.
// 10 cm: the eyes' depth is 16 bits (gfx/stereo.cpp's eye_depth), whose step
// is about z^2 / (near * 65536) with the reversed depth: 0.3 mm at the board's
// 1.4 m, against 0.6 mm with the 5 cm near plane used before.
constexpr float kNear = 0.1f;
constexpr float kFar = 100.0f;

// Dynamic resolution never draws less than half of the eyes' images.

// The head is predicted at most this far ahead (nanoseconds).
constexpr double kMaxPredictionNs = 50e6;
// An early copy (StereoView::early_copy) gives up this long before the next
// look is expected, so xrWaitFrame is not called late.
constexpr int64_t kEarlyMarginNs = 2'000'000;

constexpr GLenum kSrgb8Alpha8 = 0x8C43; // GL_SRGB8_ALPHA8
constexpr GLenum kRgba8 = 0x8058;       // GL_RGBA8

using CopyImageSubData = void(GL_APIENTRYP)(GLuint, GLenum, GLint, GLint, GLint, GLint, GLuint, GLenum, GLint, GLint,
                                            GLint, GLint, GLsizei, GLsizei, GLsizei);

struct Gl {
  PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC getNativeClientBuffer = nullptr;
  PFNEGLCREATEIMAGEKHRPROC createImage = nullptr;
  PFNEGLDESTROYIMAGEKHRPROC destroyImage = nullptr;
  PFNGLEGLIMAGETARGETTEXTURE2DOESPROC imageTargetTexture = nullptr;
  PFNEGLCREATESYNCKHRPROC createSync = nullptr;
  PFNEGLDESTROYSYNCKHRPROC destroySync = nullptr;
  PFNEGLWAITSYNCKHRPROC waitSync = nullptr;
  CopyImageSubData copyImage = nullptr;
  PFNGLGETQUERYOBJECTUI64VEXTPROC queryResult64 = nullptr;
  PFNGLQUERYCOUNTEREXTPROC queryCounter = nullptr;
  PFNEGLDUPNATIVEFENCEFDANDROIDPROC dupNativeFence = nullptr; // optional: the copies' probe

  bool load() {
    dupNativeFence =
        reinterpret_cast<PFNEGLDUPNATIVEFENCEFDANDROIDPROC>(eglGetProcAddress("eglDupNativeFenceFDANDROID"));
    getNativeClientBuffer =
        reinterpret_cast<PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC>(eglGetProcAddress("eglGetNativeClientBufferANDROID"));
    createImage = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
    destroyImage = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
    imageTargetTexture =
        reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    createSync = reinterpret_cast<PFNEGLCREATESYNCKHRPROC>(eglGetProcAddress("eglCreateSyncKHR"));
    destroySync = reinterpret_cast<PFNEGLDESTROYSYNCKHRPROC>(eglGetProcAddress("eglDestroySyncKHR"));
    waitSync = reinterpret_cast<PFNEGLWAITSYNCKHRPROC>(eglGetProcAddress("eglWaitSyncKHR"));
    copyImage = reinterpret_cast<CopyImageSubData>(eglGetProcAddress("glCopyImageSubData"));
    queryResult64 = reinterpret_cast<PFNGLGETQUERYOBJECTUI64VEXTPROC>(eglGetProcAddress("glGetQueryObjectui64vEXT"));
    queryCounter = reinterpret_cast<PFNGLQUERYCOUNTEREXTPROC>(eglGetProcAddress("glQueryCounterEXT"));
    return getNativeClientBuffer && createImage && destroyImage && imageTargetTexture && createSync && destroySync &&
           waitSync && copyImage;
  }
};
Gl g_gl;

// Row-major 4x4, column vectors.
void pose_matrix(const XrPosef& pose, float scale, float out[16]) {
  const XrQuaternionf q = pose.orientation;
  const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
  const float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
  const float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
  const float r[3][3] = {
      {1 - 2 * (yy + zz), 2 * (xy - wz), 2 * (xz + wy)},
      {2 * (xy + wz), 1 - 2 * (xx + zz), 2 * (yz - wx)},
      {2 * (xz - wy), 2 * (yz + wx), 1 - 2 * (xx + yy)},
  };
  const float t[3] = {pose.position.x, pose.position.y, pose.position.z};
  for (int row = 0; row < 3; ++row) {
    for (int col = 0; col < 3; ++col) {
      out[row * 4 + col] = r[row][col] * scale;
    }
    out[row * 4 + 3] = t[row];
  }
  out[12] = out[13] = out[14] = 0.0f;
  out[15] = 1.0f;
}

void view_matrix(const XrPosef& eye, float out[16]) { pose_matrix(inverse(eye), 1.0f, out); }

int64_t steady_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// When a signaled sync file's fences signaled (CLOCK_MONOTONIC, the steady
// clock's), the latest of them; 0 when unknown.
int64_t fence_signal_time_ns(int fd) {
  if (fd < 0) return 0;
  sync_fence_info fences[8]{};
  sync_file_info info{};
  info.num_fences = 8;
  info.sync_fence_info = reinterpret_cast<uintptr_t>(fences);
  if (ioctl(fd, SYNC_IOC_FILE_INFO, &info) != 0 || info.status != 1) return 0;
  int64_t latest = 0;
  for (uint32_t i = 0; i < std::min(info.num_fences, 8u); ++i) {
    latest = std::max(latest, static_cast<int64_t>(fences[i].timestamp_ns));
  }
  return latest;
}

// GX's perspective form (MTXFrustum) for the eye's field of view.
void projection_matrix(const XrFovf& fov, float out[16]) {
  const float l = kNear * std::tan(fov.angleLeft);
  const float r = kNear * std::tan(fov.angleRight);
  const float b = kNear * std::tan(fov.angleDown);
  const float t = kNear * std::tan(fov.angleUp);
  std::fill(out, out + 16, 0.0f);
  out[0] = 2.0f * kNear / (r - l);
  out[2] = (r + l) / (r - l);
  out[5] = 2.0f * kNear / (t - b);
  out[6] = (t + b) / (t - b);
  out[10] = -kNear / (kFar - kNear);
  out[11] = -(kFar * kNear) / (kFar - kNear);
  out[14] = -1.0f;
}

} // namespace

bool StereoView::init(XrInstance instance, XrSession session, XrSystemId system, EGLDisplay display, float scale) {
  mInstance = instance;
  mSession = session;
  mDisplay = display;
  mMaxScale = scale;
  if (!g_gl.load()) {
    LOGW("Stereo: GL extensions missing");
    return false;
  }

  char ringProperty[PROP_VALUE_MAX]{};
  __system_property_get("debug.partyboard.ring", ringProperty);
  if (const int ring = std::atoi(ringProperty); ring >= 2 && ring <= 6) mSlotCount = static_cast<size_t>(ring);
  LOGI("Stereo: ring of %zu images (debug.partyboard.ring=%s)", mSlotCount, ringProperty[0] != '\0' ? ringProperty : "unset");
  char timerProperty[PROP_VALUE_MAX]{};
  __system_property_get("debug.partyboard.gpu_timing", timerProperty);
  const auto* extensions = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
  mGpuTiming = timerProperty[0] == '1' && g_gl.queryResult64 && g_gl.queryCounter && extensions &&
      std::strstr(extensions, "GL_EXT_disjoint_timer_query");
  if (mGpuTiming) for (auto& timer : mCopyTimers) glGenQueries(2, timer.queries);

  uint32_t viewCount = 0;
  XrViewConfigurationView configViews[2]{{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
  if (!check(instance,
             xrEnumerateViewConfigurationViews(instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2,
                                               &viewCount, configViews),
             "xrEnumerateViewConfigurationViews") ||
      viewCount != 2) {
    return false;
  }
  const auto round8 = [](float v) { return static_cast<uint32_t>(std::lround(v / 8.0f)) * 8u; };
  mRecommendedEyeWidth = configViews[0].recommendedImageRectWidth;
  mEyeWidth = std::min(round8(configViews[0].recommendedImageRectWidth * scale),
                       configViews[0].maxImageRectWidth / 8u * 8u);
  mEyeHeight = std::min(round8(configViews[0].recommendedImageRectHeight * scale),
                        configViews[0].maxImageRectHeight / 8u * 8u);

  // The swapchain the images are copied into: sRGB, so the compositor reads
  // the game's colors as they are (RGBA8 bytes, already gamma-encoded).
  uint32_t formatCount = 0;
  xrEnumerateSwapchainFormats(session, 0, &formatCount, nullptr);
  std::vector<int64_t> formats(formatCount);
  xrEnumerateSwapchainFormats(session, formatCount, &formatCount, formats.data());
  const int64_t format =
      std::ranges::find(formats, kSrgb8Alpha8) != formats.end() ? kSrgb8Alpha8 : static_cast<int64_t>(kRgba8);
  mSwapchainFormat = format;
  XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
  info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
  info.format = format;
  info.sampleCount = 1;
  info.width = mEyeWidth * 2;
  info.height = mEyeHeight;
  info.faceCount = 1;
  info.arraySize = 1;
  info.mipCount = 1;
  if (!check(instance, xrCreateSwapchain(session, &info, &mSwapchain), "xrCreateSwapchain (stereo)")) {
    return false;
  }
  uint32_t imageCount = 0;
  xrEnumerateSwapchainImages(mSwapchain, 0, &imageCount, nullptr);
  mSwapchainImages.assign(imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
  xrEnumerateSwapchainImages(mSwapchain, imageCount, &imageCount,
                             reinterpret_cast<XrSwapchainImageBaseHeader*>(mSwapchainImages.data()));

  // The interface's text: 2.5 pixels per game pixel, sharp on the quad.
  mHudPixelsWidth = std::min(1600u, mEyeWidth * 2);
  mHudPixelsHeight = mHudPixelsWidth * 3 / 4;
  info.width = mHudPixelsWidth;
  info.height = mHudPixelsHeight;
  if (!check(instance, xrCreateSwapchain(session, &info, &mHudSwapchain), "xrCreateSwapchain (HUD)")) {
    destroy();
    return false;
  }
  xrEnumerateSwapchainImages(mHudSwapchain, 0, &imageCount, nullptr);
  mHudImages.assign(imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
  xrEnumerateSwapchainImages(mHudSwapchain, imageCount, &imageCount,
                            reinterpret_cast<XrSwapchainImageBaseHeader*>(mHudImages.data()));
  create_hud_screen();

  // The images the game draws into, exactly the size it draws.
  const auto [width, height] = eye_size(mRenderScale);
  if (!allocate_images(width, height)) {
    destroy();
    return false;
  }
  g_stereoView = this;
  LOGI("Stereo: eyes %ux%u, swapchain format 0x%llx", mEyeWidth, mEyeHeight, static_cast<unsigned long long>(format));
  return true;
}

std::pair<uint32_t, uint32_t> StereoView::eye_size(float renderScale) const {
  const auto round8 = [](float v) { return std::max(8u, static_cast<uint32_t>(std::lround(v / 8.0f)) * 8u); };
  return {std::min(mEyeWidth, round8(static_cast<float>(mEyeWidth) * renderScale)),
          std::min(mEyeHeight, round8(static_cast<float>(mEyeHeight) * renderScale))};
}

// The ring's images at the size the game draws: the eyes side by side, and
// the interface in an image of its own. The GPU clears and writes whole
// images out at the end of its pass, so a larger image costs bandwidth for
// pixels nobody drew: with the interface below the eyes, 41% of the eyes'
// multisampled pass was the interface or unused (3360x2960 for 3360x1760 of
// eyes, 2026-09-29). A smaller drawn size uses a part of them for a while
// (ImageSizePolicy).
// XR thread, GL context current, every slot free.
bool StereoView::allocate_images(uint32_t eyeWidth, uint32_t eyeHeight, uint32_t hudWidth) {
  if (!hudWidth) hudWidth = mHudPixelsWidth;
  XrSwapchain hudReplacement = XR_NULL_HANDLE;
  std::vector<XrSwapchainImageOpenGLESKHR> hudImages;
  if (hudWidth != mHudPixelsWidth) {
    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    info.format = mSwapchainFormat;
    info.sampleCount = info.faceCount = info.arraySize = info.mipCount = 1;
    info.width = hudWidth;
    info.height = hudWidth * 3 / 4;
    if (XR_FAILED(xrCreateSwapchain(mSession, &info, &hudReplacement))) return false;
    uint32_t count = 0;
    if (XR_FAILED(xrEnumerateSwapchainImages(hudReplacement, 0, &count, nullptr)) || count == 0) {
      xrDestroySwapchain(hudReplacement);
      return false;
    }
    hudImages.assign(count, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
    if (XR_FAILED(xrEnumerateSwapchainImages(hudReplacement, count, &count,
        reinterpret_cast<XrSwapchainImageBaseHeader*>(hudImages.data())))) {
      xrDestroySwapchain(hudReplacement);
      return false;
    }
  }
  // A shared buffer, its EGL image and the GL texture the copies read.
  const auto make = [this](uint32_t width, uint32_t height, AHardwareBuffer*& buffer, EGLImageKHR& eglImage,
                           GLuint& texture) {
    AHardwareBuffer_Desc desc{};
    desc.width = width;
    desc.height = height;
    desc.layers = 1;
    desc.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
    desc.usage = AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;
    if (AHardwareBuffer_allocate(&desc, &buffer) != 0) {
      LOGW("Stereo: image allocation failed (%ux%u)", width, height);
      return false;
    }
    const EGLint attributes[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
    eglImage = g_gl.createImage(mDisplay, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID,
                                g_gl.getNativeClientBuffer(buffer), attributes);
    if (eglImage == EGL_NO_IMAGE_KHR) {
      return false;
    }
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    g_gl.imageTargetTexture(GL_TEXTURE_2D, static_cast<GLeglImageOES>(eglImage));
    glBindTexture(GL_TEXTURE_2D, 0);
    return true;
  };
  std::vector<Slot> replacement(mSlotCount);
  for (auto& slot : replacement) {
    if (!make(eyeWidth * 2, eyeHeight, slot.buffer, slot.eglImage, slot.texture) ||
        !make(hudWidth, hudWidth * 3 / 4, slot.hudBuffer, slot.hudEglImage, slot.hudTexture)) {
      free_images(replacement);
      if (hudReplacement != XR_NULL_HANDLE) xrDestroySwapchain(hudReplacement);
      return false;
    }
  }
  std::lock_guard lock{mMutex};
  free_images();
  mSlots = std::move(replacement);
  if (hudReplacement != XR_NULL_HANDLE) {
    xrDestroySwapchain(mHudSwapchain);
    mHudSwapchain = hudReplacement;
    mHudImages = std::move(hudImages);
    mHudPixelsWidth = hudWidth;
    mHudPixelsHeight = hudWidth * 3 / 4;
    mHudShown = false;
  }
  mImageEyeWidth = eyeWidth;
  mImageEyeHeight = eyeHeight;
  ++mGeneration; // the game imports the new images (quest_stereo.cpp)
  return true;
}

void StereoView::free_images() { free_images(mSlots); }

void StereoView::free_images(std::vector<Slot>& slots) {
  const auto release = [this](AHardwareBuffer*& buffer, EGLImageKHR& eglImage, GLuint& texture) {
    if (texture != 0) {
      glDeleteTextures(1, &texture);
      texture = 0;
    }
    if (eglImage != EGL_NO_IMAGE_KHR) {
      g_gl.destroyImage(mDisplay, eglImage);
      eglImage = EGL_NO_IMAGE_KHR;
    }
    if (buffer != nullptr) {
      AHardwareBuffer_release(buffer); // Dawn keeps its own reference while it uses it
      buffer = nullptr;
    }
  };
  for (auto& slot : slots) {
    release(slot.buffer, slot.eglImage, slot.texture);
    release(slot.hudBuffer, slot.hudEglImage, slot.hudTexture);
  }
}

// A resize waits for every image to be free: none drawn, shown or copied.
bool StereoView::resize_ready() {
  std::lock_guard lock{mMutex};
  return mResizePending && std::ranges::all_of(mSlots, [](const Slot& slot) { return slot.state == State::Free; });
}

void StereoView::note_miss(int64_t doneNs, int64_t dueDisplayNs) {
  if (doneNs == 0 || dueDisplayNs == 0) return;
  const int64_t dueNs = dueDisplayNs - mPacer.lead_ns(); // the earliest look planned for
  if (doneNs <= dueNs) {
    ++mMissDoneEarly;
    mMissEarlyMaxNs = std::max(mMissEarlyMaxNs, dueNs - doneNs);
  } else {
    ++mMissDoneLate;
    mMissLateSumNs += doneNs - dueNs;
    mMissLateMaxNs = std::max(mMissLateMaxNs, doneNs - dueNs);
  }
}

void StereoView::release_slot(Slot& slot) {
  if (slot.fence >= 0) {
    close(slot.fence);
    slot.fence = -1;
  }
  if (slot.copyDoneFd >= 0) {
    close(slot.copyDoneFd);
    slot.copyDoneFd = -1;
  }
  slot.copyFlushNs = 0;
  slot.state = State::Free;
}

void StereoView::destroy() {
  g_stereoView = nullptr;
  // Session shutdown may wait; the frame loop must not wait for copies.
  glFinish();
  for (auto& timer : mCopyTimers) {
    if (timer.queries[0]) glDeleteQueries(2, timer.queries);
    timer = {};
  }
  mGpuTiming = false;
  std::lock_guard lock{mMutex};
  for (auto& slot : mSlots) {
    if (slot.copyFence != nullptr) {
      glDeleteSync(slot.copyFence);
      slot.copyFence = nullptr;
    }
    release_slot(slot);
  }
  free_images();
  if (mSwapchain != XR_NULL_HANDLE) {
    xrDestroySwapchain(mSwapchain);
    mSwapchain = XR_NULL_HANDLE;
  }
  if (mHudSwapchain != XR_NULL_HANDLE) {
    xrDestroySwapchain(mHudSwapchain);
    mHudSwapchain = XR_NULL_HANDLE;
  }
  if (mHudScreenSwapchain != XR_NULL_HANDLE) {
    xrDestroySwapchain(mHudScreenSwapchain);
    mHudScreenSwapchain = XR_NULL_HANDLE;
  }
  if (mDepthSwapchain != XR_NULL_HANDLE) {
    xrDestroySwapchain(mDepthSwapchain);
    mDepthSwapchain = XR_NULL_HANDLE;
  }
  if (mMotionSwapchain != XR_NULL_HANDLE) {
    xrDestroySwapchain(mMotionSwapchain);
    mMotionSwapchain = XR_NULL_HANDLE;
  }
  if (mMotionDepthSwapchain != XR_NULL_HANDLE) {
    xrDestroySwapchain(mMotionDepthSwapchain);
    mMotionDepthSwapchain = XR_NULL_HANDLE;
  }
  mSpaceWarpShown = false;
  if (mDepthProgram != 0) {
    glDeleteProgram(mDepthProgram);
    mDepthProgram = 0;
  }
  if (mDepthFbo != 0) {
    glDeleteFramebuffers(1, &mDepthFbo);
    mDepthFbo = 0;
  }
  mDepthShown = false;
  mHudShown = false;
  mShown = false;
  mPresentedTag = 0;
}

bool StereoView::ensure_depth_program() {
  if (mDepthProgram != 0) return true;
  // The plane's four corners, from a uniform, through each eye's projection;
  // depth only.
  const char* vertexSource = R"(#version 300 es
uniform mat4 mvp;
uniform vec3 corners[4];
void main() { gl_Position = mvp * vec4(corners[gl_VertexID], 1.0); }
)";
  const char* fragmentSource = R"(#version 300 es
precision mediump float;
layout(location = 0) out vec4 motion;
void main() { motion = vec4(0.0); }
)";
  const auto compile = [](GLenum type, const char* source) {
    const GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
      char log[512]{};
      glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
      LOGW("Depth layer: shader: %s", log);
    }
    return shader;
  };
  const GLuint program = glCreateProgram();
  const GLuint vs = compile(GL_VERTEX_SHADER, vertexSource), fs = compile(GL_FRAGMENT_SHADER, fragmentSource);
  glAttachShader(program, vs);
  glAttachShader(program, fs);
  glLinkProgram(program);
  glDeleteShader(vs);
  glDeleteShader(fs);
  GLint linked = 0;
  glGetProgramiv(program, GL_LINK_STATUS, &linked);
  if (!linked) {
    LOGW("Depth layer: program failed to link");
    glDeleteProgram(program);
    return false;
  }
  mDepthProgram = program;
  mDepthMvpLocation = glGetUniformLocation(program, "mvp");
  mDepthCornersLocation = glGetUniformLocation(program, "corners");
  glGenFramebuffers(1, &mDepthFbo);
  return true;
}

namespace {
// A swapchain of `format` (one of `wanted`, the first offered) for both eyes
// side by side, and its images; XR_NULL_HANDLE when none.
XrSwapchain make_swapchain(XrInstance instance, XrSession session, std::initializer_list<int64_t> wanted,
                           XrSwapchainUsageFlags usage, uint32_t width, uint32_t height,
                           std::vector<XrSwapchainImageOpenGLESKHR>& images, const char* what, int64_t& chosen) {
  uint32_t formatCount = 0;
  xrEnumerateSwapchainFormats(session, 0, &formatCount, nullptr);
  std::vector<int64_t> formats(formatCount);
  xrEnumerateSwapchainFormats(session, formatCount, &formatCount, formats.data());
  chosen = 0;
  for (const int64_t format : wanted) {
    if (std::ranges::find(formats, format) != formats.end()) {
      chosen = format;
      break;
    }
  }
  if (chosen == 0) {
    LOGW("%s: no swapchain format offered", what);
    return XR_NULL_HANDLE;
  }
  XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
  info.usageFlags = usage;
  info.format = chosen;
  info.sampleCount = 1;
  info.width = width;
  info.height = height;
  info.faceCount = 1;
  info.arraySize = 1;
  info.mipCount = 1;
  XrSwapchain swapchain = XR_NULL_HANDLE;
  if (!check(instance, xrCreateSwapchain(session, &info, &swapchain), what)) return XR_NULL_HANDLE;
  uint32_t count = 0;
  xrEnumerateSwapchainImages(swapchain, 0, &count, nullptr);
  images.assign(count, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
  xrEnumerateSwapchainImages(swapchain, count, &count, reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));
  return swapchain;
}

constexpr int64_t kDepth16 = 0x81A5, kDepth24 = 0x81A6; // GL_DEPTH_COMPONENT16/24
constexpr int64_t kRgba16f = 0x881A;                    // GL_RGBA16F

// One swapchain image acquired and waited for; false: none (nothing to release).
bool acquire_image(XrSwapchain swapchain, uint32_t& index) {
  XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
  XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
  wait.timeout = XR_INFINITE_DURATION;
  if (XR_FAILED(xrAcquireSwapchainImage(swapchain, &acquire, &index))) return false;
  if (XR_FAILED(xrWaitSwapchainImage(swapchain, &wait))) {
    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(swapchain, &release);
    return false;
  }
  return true;
}

bool release_image(XrSwapchain swapchain) {
  XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
  return XR_SUCCEEDED(xrReleaseSwapchainImage(swapchain, &release));
}
} // namespace

void StereoView::enable_depth_layer(XrInstance instance, XrSession session) {
  int64_t format = 0;
  mDepthSwapchain = make_swapchain(instance, session, {kDepth16, kDepth24}, XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                                   mEyeWidth * 2, mEyeHeight, mDepthImages, "Depth layer", format);
  if (mDepthSwapchain == XR_NULL_HANDLE) return;
  const bool ready = ensure_depth_program();
  LOGI("Depth layer: %ux%u, format 0x%llx, %zu images, program %s (debug.partyboard.depth_layer 1 to submit it)",
       mEyeWidth * 2, mEyeHeight, static_cast<unsigned long long>(format), mDepthImages.size(), ready ? "ready" : "failed");
  if (!ready) {
    xrDestroySwapchain(mDepthSwapchain);
    mDepthSwapchain = XR_NULL_HANDLE;
  }
}

void StereoView::enable_space_warp(XrInstance instance, XrSystemId system, XrSession session) {
  XrSystemSpaceWarpPropertiesFB spaceWarp{XR_TYPE_SYSTEM_SPACE_WARP_PROPERTIES_FB};
  XrSystemProperties properties{XR_TYPE_SYSTEM_PROPERTIES};
  properties.next = &spaceWarp;
  if (!check(instance, xrGetSystemProperties(instance, system, &properties), "xrGetSystemProperties (space warp)") ||
      spaceWarp.recommendedMotionVectorImageRectWidth == 0) {
    LOGW("Space warp: no recommended motion vector size");
    return;
  }
  mMotionWidth = spaceWarp.recommendedMotionVectorImageRectWidth;
  mMotionHeight = spaceWarp.recommendedMotionVectorImageRectHeight;
  int64_t motionFormat = 0, depthFormat = 0;
  mMotionSwapchain = make_swapchain(instance, session, {kRgba16f}, XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT,
                                    mMotionWidth * 2, mMotionHeight, mMotionImages, "Space warp motion", motionFormat);
  mMotionDepthSwapchain = make_swapchain(instance, session, {kDepth24, kDepth16}, XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                                         mMotionWidth * 2, mMotionHeight, mMotionDepthImages, "Space warp depth", depthFormat);
  const bool ready = mMotionSwapchain != XR_NULL_HANDLE && mMotionDepthSwapchain != XR_NULL_HANDLE && ensure_depth_program();
  LOGI("Space warp: motion vectors %ux%u per eye, depth 0x%llx, %s (debug.partyboard.space_warp 1 to use it)",
       mMotionWidth, mMotionHeight, static_cast<unsigned long long>(depthFormat), ready ? "ready" : "unavailable");
  if (!ready) {
    if (mMotionSwapchain != XR_NULL_HANDLE) xrDestroySwapchain(mMotionSwapchain);
    if (mMotionDepthSwapchain != XR_NULL_HANDLE) xrDestroySwapchain(mMotionDepthSwapchain);
    mMotionSwapchain = mMotionDepthSwapchain = XR_NULL_HANDLE;
  }
}

void StereoView::set_depth_plane(const XrPosef& center, float halfExtent) {
  std::lock_guard lock{mMutex};
  mDepthPlane = center;
  mDepthPlaneHalf = halfExtent;
}

// The plane's corners in stage space, as a triangle strip; false: no plane.
bool StereoView::plane_corners(float corners[12]) {
  XrPosef plane;
  float half;
  {
    std::lock_guard lock{mMutex};
    plane = mDepthPlane;
    half = mDepthPlaneHalf;
  }
  if (half <= 0.0f) return false;
  const float signs[4][2] = {{-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
  for (int i = 0; i < 4; ++i) {
    const XrVector3f p = add(plane.position, rotate(plane.orientation, {signs[i][0] * half, 0.0f, signs[i][1] * half}));
    corners[i * 3] = p.x;
    corners[i * 3 + 1] = p.y;
    corners[i * 3 + 2] = p.z;
  }
  return true;
}

// Stage space -> the eye's GL clip space (column-major; depth 0 at kNear, 1
// at kFar once in the window): what the depth infos tell the compositor.
void StereoView::plane_mvp(const XrView& view, float mvp[16]) const {
  const XrPosef toEye = inverse(view.pose);
  const XrVector3f x = rotate(toEye.orientation, {1, 0, 0}), y = rotate(toEye.orientation, {0, 1, 0}),
                   z = rotate(toEye.orientation, {0, 0, 1});
  const float viewM[16] = {x.x, x.y, x.z, 0, y.x, y.y, y.z, 0, z.x, z.y, z.z, 0,
                           toEye.position.x, toEye.position.y, toEye.position.z, 1};
  const float l = std::tan(view.fov.angleLeft), r = std::tan(view.fov.angleRight);
  const float d = std::tan(view.fov.angleDown), u = std::tan(view.fov.angleUp);
  const float projM[16] = {2.0f / (r - l), 0, 0, 0,
                           0, 2.0f / (u - d), 0, 0,
                           (r + l) / (r - l), (u + d) / (u - d), -(kFar + kNear) / (kFar - kNear), -1,
                           0, 0, -2.0f * kFar * kNear / (kFar - kNear), 0};
  for (int c = 0; c < 4; ++c) {
    for (int row = 0; row < 4; ++row) {
      mvp[c * 4 + row] = projM[0 * 4 + row] * viewM[c * 4 + 0] + projM[1 * 4 + row] * viewM[c * 4 + 1] +
                         projM[2 * 4 + row] * viewM[c * 4 + 2] + projM[3 * 4 + row] * viewM[c * 4 + 3];
    }
  }
}

// The plane's depth into the next depth image, for both eyes of `slot`, at
// the eyes' drawn rectangles. The caller holds no lock; the GL context is the
// XR thread's.
bool StereoView::draw_depth(const Slot& slot) {
  float corners[12];
  uint32_t index = 0;
  if (!plane_corners(corners) || !acquire_image(mDepthSwapchain, index)) return false;
  glBindFramebuffer(GL_FRAMEBUFFER, mDepthFbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, mDepthImages[index].image, 0);
  const GLenum none = GL_NONE;
  glDrawBuffers(1, &none);
  glDisable(GL_SCISSOR_TEST);
  glDepthMask(GL_TRUE);
  glClearDepthf(1.0f);
  glClear(GL_DEPTH_BUFFER_BIT);
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_LESS);
  glDisable(GL_CULL_FACE);
  glUseProgram(mDepthProgram);
  glUniform3fv(mDepthCornersLocation, 4, corners);
  for (uint32_t eye = 0; eye < 2; ++eye) {
    float mvp[16];
    plane_mvp(slot.views[eye], mvp);
    glUniformMatrix4fv(mDepthMvpLocation, 1, GL_FALSE, mvp);
    glViewport(static_cast<GLint>(eye * mEyeWidth), static_cast<GLint>((mEyeHeight - slot.renderHeight) / 2),
               static_cast<GLsizei>(slot.renderWidth), static_cast<GLsizei>(slot.renderHeight));
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  }
  glDisable(GL_DEPTH_TEST);
  glUseProgram(0);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  return release_image(mDepthSwapchain);
}

// Application SpaceWarp's images for `slot`: motion vectors all zero (the
// board stands still in the room; only the head moves, which the compositor
// knows) and the plane's depth, each eye over its whole half (the same field
// of view as the eyes' drawn rectangle).
bool StereoView::draw_space_warp(const Slot& slot) {
  float corners[12];
  uint32_t motionIndex = 0, depthIndex = 0;
  if (!plane_corners(corners)) return false;
  if (!acquire_image(mMotionSwapchain, motionIndex)) return false;
  if (!acquire_image(mMotionDepthSwapchain, depthIndex)) {
    release_image(mMotionSwapchain);
    return false;
  }
  glBindFramebuffer(GL_FRAMEBUFFER, mDepthFbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, mMotionImages[motionIndex].image, 0);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, mMotionDepthImages[depthIndex].image, 0);
  const GLenum color = GL_COLOR_ATTACHMENT0;
  glDrawBuffers(1, &color);
  glDisable(GL_SCISSOR_TEST);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glDepthMask(GL_TRUE);
  glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
  glClearDepthf(1.0f);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_LESS);
  glDisable(GL_CULL_FACE);
  glDisable(GL_BLEND);
  glUseProgram(mDepthProgram);
  glUniform3fv(mDepthCornersLocation, 4, corners);
  for (uint32_t eye = 0; eye < 2; ++eye) {
    float mvp[16];
    plane_mvp(slot.views[eye], mvp);
    glUniformMatrix4fv(mDepthMvpLocation, 1, GL_FALSE, mvp);
    glViewport(static_cast<GLint>(eye * mMotionWidth), 0, static_cast<GLsizei>(mMotionWidth),
               static_cast<GLsizei>(mMotionHeight));
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  }
  glDisable(GL_DEPTH_TEST);
  glUseProgram(0);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  const bool motionReleased = release_image(mMotionSwapchain);
  return release_image(mMotionDepthSwapchain) && motionReleased;
}
void StereoView::load_hidden_area(bool available) {
  if (!available) {
    LOGI("Hidden area: XR_KHR_visibility_mask unavailable, every pixel is drawn");
    return;
  }
  PFN_xrGetVisibilityMaskKHR getMask = nullptr;
  if (XR_FAILED(xrGetInstanceProcAddr(mInstance, "xrGetVisibilityMaskKHR",
                                      reinterpret_cast<PFN_xrVoidFunction*>(&getMask))) ||
      getMask == nullptr) {
    LOGW("Hidden area: xrGetVisibilityMaskKHR missing");
    return;
  }
  // One mask of an eye, its points in index order (empty: none).
  const auto read = [&](uint32_t eye, XrVisibilityMaskTypeKHR type, XrResult& result) {
    std::vector<XrVector2f> points;
    XrVisibilityMaskKHR mask{XR_TYPE_VISIBILITY_MASK_KHR};
    result = getMask(mSession, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, eye, type, &mask);
    if (XR_FAILED(result) || mask.vertexCountOutput == 0 || mask.indexCountOutput == 0) return points;
    std::vector<XrVector2f> vertices(mask.vertexCountOutput);
    std::vector<uint32_t> indices(mask.indexCountOutput);
    mask.vertexCapacityInput = mask.vertexCountOutput;
    mask.vertices = vertices.data();
    mask.indexCapacityInput = mask.indexCountOutput;
    mask.indices = indices.data();
    result = getMask(mSession, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, eye, type, &mask);
    if (XR_FAILED(result)) return points;
    for (const uint32_t index : indices) {
      if (index < vertices.size()) points.push_back(vertices[index]);
    }
    return points;
  };
  // The visible outline (whatever is outside it is hidden), else the hidden
  // triangles: the runtime may give either.
  std::vector<XrVector2f> outline[2], triangles[2];
  for (uint32_t eye = 0; eye < 2; ++eye) {
    XrResult loopResult = XR_SUCCESS, meshResult = XR_SUCCESS;
    outline[eye] = read(eye, XR_VISIBILITY_MASK_TYPE_LINE_LOOP_KHR, loopResult);
    if (outline[eye].size() < 3) {
      outline[eye].clear();
      triangles[eye] = read(eye, XR_VISIBILITY_MASK_TYPE_HIDDEN_TRIANGLE_MESH_KHR, meshResult);
      triangles[eye].resize(triangles[eye].size() / 3 * 3);
    }
    LOGI("Hidden area, eye %u: outline %zu points (result %d), hidden mesh %zu triangles (result %d)", eye,
         outline[eye].size(), loopResult, triangles[eye].size() / 3, meshResult);
    if (outline[eye].empty() && triangles[eye].empty()) {
      LOGW("Hidden area: none given for eye %u, every pixel is drawn", eye);
      return;
    }
  }
  std::lock_guard lock{mMutex};
  for (int eye = 0; eye < 2; ++eye) {
    mVisibleOutline[eye] = std::move(outline[eye]);
    mHiddenTriangles[eye] = std::move(triangles[eye]);
  }
  mHiddenPending = true;
}

namespace {

// Rectangles of an eye's image (0..1, from its top-left) the lenses never
// show, on a grid. With the visible outline, a cell counts when no edge of
// the outline crosses it and its center is outside; with the hidden
// triangles, when one triangle holds its four corners. Each row's runs of
// such cells are one rectangle, and a run equal to the one above extends it.
std::vector<float> hidden_rects(const std::vector<XrVector2f>& outline, const std::vector<XrVector2f>& triangles,
                                const XrFovf& fov, float& hiddenPart) {
  constexpr int kGrid = 48;
  const float l = std::tan(fov.angleLeft), r = std::tan(fov.angleRight);
  const float d = std::tan(fov.angleDown), u = std::tan(fov.angleUp);
  const auto to_image = [&](const XrVector2f& v) { return std::array<float, 2>{(v.x - l) / (r - l), (u - v.y) / (u - d)}; };
  std::vector<std::array<float, 2>> points, corners;
  points.reserve(outline.size());
  for (const auto& v : outline) points.push_back(to_image(v));
  corners.reserve(triangles.size());
  for (const auto& v : triangles) corners.push_back(to_image(v));
  // Whether (x, y) is inside triangle t, edges included.
  const auto in_triangle = [&](size_t t, float x, float y) {
    const auto& a = corners[t];
    const auto& b = corners[t + 1];
    const auto& c = corners[t + 2];
    const float d1 = (x - b[0]) * (a[1] - b[1]) - (a[0] - b[0]) * (y - b[1]);
    const float d2 = (x - c[0]) * (b[1] - c[1]) - (b[0] - c[0]) * (y - c[1]);
    const float d3 = (x - a[0]) * (c[1] - a[1]) - (c[0] - a[0]) * (y - a[1]);
    return !((d1 < 0.f || d2 < 0.f || d3 < 0.f) && (d1 > 0.f || d2 > 0.f || d3 > 0.f));
  };
  const auto inside = [&](float x, float y) {
    bool in = false;
    for (size_t i = 0, j = points.size() - 1; i < points.size(); j = i++) {
      const auto& a = points[i];
      const auto& b = points[j];
      if ((a[1] > y) != (b[1] > y) && x < (b[0] - a[0]) * (y - a[1]) / (b[1] - a[1]) + a[0]) in = !in;
    }
    return in;
  };
  // Liang-Barsky: whether segment a-b reaches the box.
  const auto crosses = [](const std::array<float, 2>& a, const std::array<float, 2>& b, float x0, float y0, float x1,
                          float y1) {
    float t0 = 0.f, t1 = 1.f;
    const float dx = b[0] - a[0], dy = b[1] - a[1];
    const float p[4] = {-dx, dx, -dy, dy};
    const float q[4] = {a[0] - x0, x1 - a[0], a[1] - y0, y1 - a[1]};
    for (int k = 0; k < 4; ++k) {
      if (p[k] == 0.f) {
        if (q[k] < 0.f) return false;
      } else {
        const float t = q[k] / p[k];
        if (p[k] < 0.f) t0 = std::max(t0, t);
        else t1 = std::min(t1, t);
        if (t0 > t1) return false;
      }
    }
    return true;
  };
  std::vector<float> rects;
  std::vector<std::array<int, 2>> above;
  std::vector<size_t> aboveRect;
  int hiddenCells = 0;
  for (int row = 0; row < kGrid; ++row) {
    const float y0 = static_cast<float>(row) / kGrid, y1 = static_cast<float>(row + 1) / kGrid;
    std::vector<std::array<int, 2>> runs;
    int start = -1;
    for (int col = 0; col <= kGrid; ++col) {
      bool hidden = false;
      if (col < kGrid) {
        const float x0 = static_cast<float>(col) / kGrid, x1 = static_cast<float>(col + 1) / kGrid;
        if (!points.empty()) {
          hidden = !inside((x0 + x1) * 0.5f, (y0 + y1) * 0.5f);
          for (size_t i = 0, j = points.size() - 1; hidden && i < points.size(); j = i++) {
            hidden = !crosses(points[j], points[i], x0, y0, x1, y1);
          }
        } else {
          for (size_t t = 0; !hidden && t + 2 < corners.size(); t += 3) {
            hidden = in_triangle(t, x0, y0) && in_triangle(t, x1, y0) && in_triangle(t, x0, y1) && in_triangle(t, x1, y1);
          }
        }
        hiddenCells += hidden;
      }
      if (hidden && start < 0) start = col;
      if (!hidden && start >= 0) {
        runs.push_back({start, col});
        start = -1;
      }
    }
    std::vector<size_t> runRect;
    for (const auto& run : runs) {
      const auto same = std::ranges::find(above, run);
      if (same != above.end()) {
        const size_t index = aboveRect[same - above.begin()];
        rects[index + 3] = y1;
        runRect.push_back(index);
      } else {
        runRect.push_back(rects.size());
        rects.insert(rects.end(), {static_cast<float>(run[0]) / kGrid, y0, static_cast<float>(run[1]) / kGrid, y1});
      }
    }
    above = std::move(runs);
    aboveRect = std::move(runRect);
  }
  hiddenPart = static_cast<float>(hiddenCells) / (kGrid * kGrid);
  return rects;
}

} // namespace

uint32_t StereoView::hidden_area(uint32_t eye, float* rects, uint32_t capacity, uint32_t& version) const {
  std::lock_guard lock{mMutex};
  version = mHiddenVersion;
  if (eye > 1) return 0;
  const auto& all = mHiddenRects[eye];
  const uint32_t count = static_cast<uint32_t>(all.size() / 4);
  if (rects != nullptr) std::copy_n(all.begin(), std::min(count, capacity) * 4, rects);
  return count;
}

void StereoView::update(const XrView (&views)[2], const XrPosef& world, float scale, bool enabled, float hudWidth, float hudHeight) {
  // Headset measurements (debug.partyboard.freeze 1, with the game's frozen
  // simulation): the eyes keep the views they had, so every image of an A/B
  // is the very same work whatever the head does (the compositor still
  // places each image with the poses it was drawn with).
  static bool frozen = false;
  static auto frozenReadAt = std::chrono::steady_clock::time_point{};
  if (const auto now = std::chrono::steady_clock::now(); now - frozenReadAt >= std::chrono::seconds(1)) {
    frozenReadAt = now;
    char value[PROP_VALUE_MAX]{};
    __system_property_get("debug.partyboard.freeze", value);
    const bool wanted = value[0] == '1';
    if (wanted != frozen) LOGI("Stereo: eyes' views %s (debug.partyboard.freeze)", wanted ? "frozen" : "following the head");
    frozen = wanted;
  }
  std::lock_guard lock{mMutex};
  mViewsFrozen = frozen;
  if (!frozen || !(mViews[0].fov.angleRight > mViews[0].fov.angleLeft)) {
    mViews[0] = views[0];
    mViews[1] = views[1];
  }
  if (mHiddenPending && views[0].fov.angleRight > views[0].fov.angleLeft &&
      views[1].fov.angleRight > views[1].fov.angleLeft) {
    mHiddenPending = false;
    float part[2]{};
    for (int eye = 0; eye < 2; ++eye) {
      mHiddenRects[eye] = hidden_rects(mVisibleOutline[eye], mHiddenTriangles[eye], views[eye].fov, part[eye]);
    }
    ++mHiddenVersion;
    LOGI("Hidden area: left %zu rectangles (%.1f%% of the eye), right %zu (%.1f%%)", mHiddenRects[0].size() / 4,
         part[0] * 100.f, mHiddenRects[1].size() / 4, part[1] * 100.f);
  }
  pose_matrix(world, scale, mWorld);
  mEnabled = enabled && mSwapchain != XR_NULL_HANDLE;
  mHudWidth = hudWidth;
  mHudHeight = hudHeight;
}

bool StereoView::game_frame(StereoFrame& out) {
  std::lock_guard lock{mMutex};
  if (!mEnabled || mScreenRequired || mResizePending) {
    mLastRequestAt = {}; // not a hitch of the board: the ring drains, or no world
    return false; // resizing: the ring drains, then new images come (layer())
  }
  // The time between the game's requests: its hitches, for adapt_resolution().
  const auto requestAt = std::chrono::steady_clock::now();
  if (mLastRequestAt != std::chrono::steady_clock::time_point{}) {
    const float ms = std::chrono::duration<float, std::milli>(requestAt - mLastRequestAt).count();
    if (ms < 250.0f) { // longer: loading or paused
      mRequestIntervals.push_back(ms);
    }
  }
  mLastRequestAt = requestAt;
  // Never reclaim Drawing by age: a queued GPU frame can still own it.
  // Cameras without a view cancel explicitly; empty eye passes clear and submit.
  const auto free = std::ranges::find_if(mSlots, [](const Slot& slot) { return slot.state == State::Free; });
  if (free == mSlots.end()) {
    ++mRingFullCount;
    ++mAdaptRingFull;
    for (const auto& slot : mSlots) {
      mFullDrawing += slot.state == State::Drawing;
      mFullReady += slot.state == State::Ready;
      mFullCopying += slot.state == State::Copying;
    }
    return false; // the headset is behind: this frame goes to the flat screen only
  }
  ++mLeaseCount;
  ++mAdaptLeases;
  // The controller's size, within the images (larger: a resize is pending).
  const auto [drawWidth, drawHeight] = eye_size(mRenderScale);
  free->renderWidth = std::min(drawWidth, mImageEyeWidth);
  free->renderHeight = std::min(drawHeight, mImageEyeHeight);
  // Through a window, each eye draws the window's shape, as large as the
  // controller's size allows: its pixels all land on the window.
  const bool mapped = mWindowOn && mWindowSize.width > 0.0f && mWindowSize.height > 0.0f;
  free->window = mapped && mWindowFramed;
  if (free->window) {
    const float aspect = mWindowSize.width / mWindowSize.height;
    if (free->renderWidth > free->renderHeight * aspect) {
      free->renderWidth = std::max(16u, static_cast<uint32_t>(free->renderHeight * aspect));
    } else {
      free->renderHeight = std::max(16u, static_cast<uint32_t>(free->renderWidth / aspect));
    }
    free->windowPose = mWindowPose;
    free->windowSize = mWindowSize;
  }
  free->leaseTime = mFrameTime;
  // The paced start this frame began at (next_frame_start()), for the pacer.
  free->startNs = mStartGiven ? mGivenStartNs : 0;
  free->dueNs = mStartGiven ? mGivenDueNs : 0;
  if (free->dueNs != 0 && mLastDueNs != 0 && mPeriodNs > 0) {
    const int64_t steps = (free->dueNs - mLastDueNs + mPeriodNs / 2) / mPeriodNs;
    ++mDueSteps[std::clamp<int64_t>(steps, 0, 2)];
  }
  if (free->dueNs != 0) mLastDueNs = free->dueNs;
  mStartGiven = false;
  // The interface, about 26 times a second: one image in 3 at 72 Hz. Every
  // other image, it was drawn at 36 Hz for a copy at 30 Hz at most, and its
  // pass (0.8 ms, up to 2.5) made every other image heavier, on a GPU
  // nearly full (2026-10-04). debug.partyboard.hud_rate: `full` in every
  // image, `half` every other one.
  const int64_t displayHz = mPeriodNs > 0 ? 1'000'000'000 / mPeriodNs : 60;
  const uint32_t hudEvery = mHudEveryImage ? 1u : mHudHalf ? 2u
                                                  : static_cast<uint32_t>(std::max<int64_t>(2, (displayHz + 13) / 26));
  // Aurora draws it only when it changed (debug.partyboard.hud_keep), unless
  // the last one it drew never reached the headset.
  free->hud = mHudSinceLease + 1 >= hudEvery;
  mHudSinceLease = free->hud ? 0 : mHudSinceLease + 1;
  out.drawHud = free->hud ? (mHudLost ? 2 : 1) : 0;
  if (free->hud) mHudLost = false;
  out.foveation = mFoveationAuto && mFoveationAvailable ? mFoveation : 0;
  out.generation = mGeneration;
  free->state = State::Drawing;
  free->leaseNs = steady_ns();
  free->submitNs = 0;
  free->tag = mNextTag++;
  free->views[0] = mViews[0];
  free->views[1] = mViews[1];
  free->board = mBoardMode;
  out.image = static_cast<uint32_t>(free - mSlots.begin());
  out.tag = free->tag;
  if (free->window) {
    // Each eye looks through the window: its view keeps the window's axes
    // (the image lies in the window's plane, the compositor turns it with the
    // head), and its field of view is the window's edges as seen from that
    // eye, whatever its own: off center as soon as the eye is.
    const XrPosef toWindow = inverse(free->windowPose);
    const float halfWidth = free->windowSize.width * 0.5f, halfHeight = free->windowSize.height * 0.5f;
    for (int eye = 0; eye < 2; ++eye) {
      const XrVector3f e = compose(toWindow, mViews[eye].pose).position;
      const float position[3]{e.x, e.y, e.z};
      const WindowTangents tangents = window_tangents(halfWidth, halfHeight, position);
      XrFovf fov{};
      fov.angleLeft = std::atan(tangents.left);
      fov.angleRight = std::atan(tangents.right);
      fov.angleUp = std::atan(tangents.up);
      fov.angleDown = std::atan(tangents.down);
      view_matrix(XrPosef{free->windowPose.orientation, mViews[eye].pose.position}, out.eyeView[eye]);
      projection_matrix(fov, out.eyeProj[eye]);
    }
    pose_matrix(free->windowPose, 1.0f, out.world);
  } else {
    // On the table, each eye draws only the box around what the game offered
    // to draw last frame (set_scene_bounds), with a margin: a board or a
    // minigame fills part of the view, and the rest of the eyes' pixels were
    // cleared, multisampled, resolved, copied and composited for nothing.
    // The pixels kept are as dense as before, so the image is as sharp.
    if (!mapped || !mWindowFramed) crop_eyes(*free);
    for (int eye = 0; eye < 2; ++eye) {
      view_matrix(mViews[eye].pose, out.eyeView[eye]);
      projection_matrix(free->views[eye].fov, out.eyeProj[eye]);
    }
    // Open, the window only places the camera's view in the room.
    if (mapped) {
      pose_matrix(mWindowPose, 1.0f, out.world);
    } else {
      std::copy(std::begin(mWorld), std::end(mWorld), out.world);
    }
  }
  out.window = free->window ? 1u : mapped ? 2u : 0u;
  out.windowHeight = mapped ? mWindowSize.height : 0.0f;
  out.windowDistance = mapped ? mWindowViewerDistance : 0.0f;
  out.hudWidth = mHudWidth;
  out.hudHeight = mHudHeight;
  out.eyeWidth = free->renderWidth;
  out.eyeHeight = free->renderHeight;
  return true;
}

void StereoView::set_foveation_available(bool available) {
  std::lock_guard lock{mMutex};
  if (available != mFoveationAvailable) LOGI("Stereo: fragment density map %s", available ? "available" : "unavailable");
  mFoveationAvailable = available;
}

void StereoView::set_locate_space(XrSpace space) {
  std::lock_guard lock{mMutex};
  mLocateSpace = space;
}

namespace {
// A 4x4 row-major matrix's inverse (Gauss-Jordan, partial pivoting); false
// when singular.
bool invert4(const float in[16], float out[16]) {
  double a[4][8];
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      a[r][c] = in[r * 4 + c];
      a[r][c + 4] = r == c ? 1.0 : 0.0;
    }
  }
  for (int c = 0; c < 4; ++c) {
    int pivot = c;
    for (int r = c + 1; r < 4; ++r) {
      if (std::fabs(a[r][c]) > std::fabs(a[pivot][c])) pivot = r;
    }
    if (std::fabs(a[pivot][c]) < 1e-12) return false;
    if (pivot != c) std::swap(a[pivot], a[c]);
    const double scale = 1.0 / a[c][c];
    for (double& v : a[c]) v *= scale;
    for (int r = 0; r < 4; ++r) {
      if (r == c) continue;
      const double f = a[r][c];
      for (int k = 0; k < 8; ++k) a[r][k] -= f * a[c][k];
    }
  }
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) out[r * 4 + c] = static_cast<float>(a[r][c + 4]);
  }
  return true;
}

void multiply4(const float a[16], const float b[16], float out[16]) {
  float result[16];
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      float sum = 0.f;
      for (int k = 0; k < 4; ++k) sum += a[r * 4 + k] * b[k * 4 + c];
      result[r * 4 + c] = sum;
    }
  }
  std::copy(std::begin(result), std::end(result), out);
}
} // namespace

bool StereoView::late_latch(uint64_t tag, float correction[2][16]) {
  XrView old[2]{};
  XrTime display = 0;
  XrSpace space = XR_NULL_HANDLE;
  {
    std::lock_guard lock{mMutex};
    if (!mLateLatch || mViewsFrozen || mSession == XR_NULL_HANDLE || mLocateSpace == XR_NULL_HANDLE) return false;
    const auto slot = std::ranges::find_if(mSlots, [tag](const Slot& s) { return s.state == State::Drawing && s.tag == tag; });
    // Through a window, the projection is the window's from each eye, not the
    // eye's own: the correction does not apply (the compositor turns the quads).
    if (slot == mSlots.end() || slot->window) return false;
    old[0] = slot->views[0];
    old[1] = slot->views[1];
    // The display frame it was located for: its paced one, else the measured delay.
    display = slot->dueNs != 0 ? slot->dueNs
                               : slot->leaseTime + static_cast<XrTime>(std::clamp(mLatencyNs, 0.0, kMaxPredictionNs));
    space = mLocateSpace;
  }
  if (display <= 0 || !(old[0].fov.angleRight > old[0].fov.angleLeft)) return false;
  XrView fresh[2]{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
  XrViewLocateInfo info{XR_TYPE_VIEW_LOCATE_INFO};
  info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
  info.displayTime = display;
  info.space = space;
  XrViewState state{XR_TYPE_VIEW_STATE};
  uint32_t count = 0;
  constexpr XrViewStateFlags valid = XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT;
  if (XR_FAILED(xrLocateViews(mSession, &info, &state, 2, &count, fresh)) || count != 2 ||
      (state.viewStateFlags & valid) != valid) {
    return false;
  }
  // Each eye: the projection the frame was recorded with, P V_old, becomes
  // P' V_new: correction = P' V_new V_old^-1 P^-1.
  for (int eye = 0; eye < 2; ++eye) {
    // The field of view it was drawn with (cropped eyes: narrowed).
    fresh[eye].fov = old[eye].fov;
    float pOld[16], pNew[16], vNew[16], vOldInverse[16], pOldInverse[16], m[16];
    projection_matrix(old[eye].fov, pOld);
    projection_matrix(fresh[eye].fov, pNew);
    view_matrix(fresh[eye].pose, vNew);
    pose_matrix(old[eye].pose, 1.0f, vOldInverse);
    if (!invert4(pOld, pOldInverse)) return false;
    multiply4(vNew, vOldInverse, m);
    multiply4(pNew, m, m);
    multiply4(m, pOldInverse, correction[eye]);
  }
  std::lock_guard lock{mMutex};
  const auto slot = std::ranges::find_if(mSlots, [tag](const Slot& s) { return s.state == State::Drawing && s.tag == tag; });
  if (slot == mSlots.end()) return false;
  // The compositor places the image with the poses it was drawn with.
  slot->views[0] = fresh[0];
  slot->views[1] = fresh[1];
  const auto& a = old[0].pose.position;
  const auto& b = fresh[0].pose.position;
  const double moved = std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
  ++mLateApplied;
  mLateMovedSumMm += moved * 1000.0;
  mLateMovedMaxMm = std::max(mLateMovedMaxMm, moved * 1000.0);
  return true;
}

void StereoView::set_frame_time(XrTime time, XrDuration period) {
  std::lock_guard lock{mMutex};
  // The XR thread's own period, from one frame's display time to the next:
  // with Application SpaceWarp the runtime wakes it every other display frame
  // (60 times a second at 120 Hz), and the pacer, set to the display's period,
  // gave the game's images the wrong looks: 50 new images for 60 frames
  // (2026-10-03).
  static XrTime lastTime = 0;
  if (mSpaceWarpWanted && period > 0 && lastTime > 0 && time > lastTime) {
    const XrDuration step = time - lastTime;
    if (step > period / 2 && step < period * 4) {
      period = static_cast<XrDuration>(std::llround(static_cast<double>(step) / period)) * period;
    }
  }
  lastTime = time;
  mFrameTime = time;
  if (period > 0) mPeriodNs = period;
}

int64_t StereoView::next_frame_start(int64_t nowNs, float targetHz) {
  std::lock_guard lock{mMutex};
  mStartGiven = false;
  if (!(mPacing || mAlign) || !mEnabled || mScreenRequired) return 0;
  const int64_t start = mPacer.next_start(nowNs, targetHz, &mGivenDueNs);
  mStartGiven = start != 0;
  mGivenStartNs = start;
  return start;
}

XrDuration StereoView::prediction() const {
  std::lock_guard lock{mMutex};
  return static_cast<XrDuration>(std::clamp(mLatencyNs, 0.0, kMaxPredictionNs));
}

void StereoView::set_screen_hidden(bool hidden) {
  std::lock_guard lock{mMutex};
  mScreenHidden = hidden;
}

bool StereoView::screen_hidden() const {
  std::lock_guard lock{mMutex};
  return mScreenHidden;
}

void StereoView::set_quality_sample(const QualitySample& sample) {
  std::lock_guard lock{mMutex};
  mQualitySample = sample;
}

void StereoView::set_quality_cap(float recommendedScale) {
  std::lock_guard lock{mMutex};
  mQualityCap = recommendedScale;
  mQuality.set_cap(mMinigameQuality ? AdaptiveQuality::kMaxCap : recommendedScale);
  mDesiredHudWidth = recommendedScale <= 0.8f ? 1280 : recommendedScale <= 1.0f ? 1600 : 1920;
  mRenderScale = std::min(1.0f, mQuality.scale() / mMaxScale);
  // A lower ceiling: drawn in a part of the images until ImageSizePolicy
  // shrinks them (adapt_resolution()).
  const auto [width, height] = eye_size(mRenderScale);
  if (width > mImageEyeWidth || height > mImageEyeHeight || mDesiredHudWidth != mHudPixelsWidth) mResizePending = true;
}

// A minigame's eyes at its start, of the recommended size (set_board_mode):
// 100%, raised by 10% steps when the GPU has room. At 120% Manta Rings, seen
// from its camera's place, ran at 48 to 59 images a second (2026-10-10).
constexpr float kMinigameStartQuality = 1.0f;
constexpr float kMinigameQualityStep = 0.1f;

// One decision per fresh one-second metric window; do not lower resolution
// merely because the CPU or the image bridge is late while the GPU has headroom.
void StereoView::adapt_resolution() {
  const auto now = std::chrono::steady_clock::now();
  if (now - mAdaptAt < std::chrono::seconds(1)) {
    return;
  }
  mAdaptAt = now;
  mPacer.update(); // the start's place, from the images finished this second
  const float before = mRenderScale;
  const uint32_t wanted = mAdaptLeases + mAdaptRingFull;
  // A hitch: an image that came more than 1.5 times the usual interval (the
  // window's quarter-lowest, whatever the render rate) and 4 ms after it.
  uint32_t slow = 0;
  if (mRequestIntervals.size() >= 10) {
    auto sorted = mRequestIntervals;
    std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 4, sorted.end());
    const float usual = sorted[sorted.size() / 4];
    for (const float ms : mRequestIntervals) {
      slow += ms > 1.5f * usual && ms > usual + 4.0f;
    }
  }
  mRequestIntervals.clear();
  mLastSlow = slow;
  // A scene arriving (a world shown again, a board entered or left) hitches
  // while it loads: not the GPU's pressure. Entering the Toad board at 100%,
  // its loading hitches took it to 95% a second later, kept there 20 s
  // (2026-10-04). The controller sits out the first 3 seconds.
  if (mShownHasWorld && !mSettleWorld) mSettleUntil = now + std::chrono::seconds(3);
  mSettleWorld = mShownHasWorld;
  const bool settling = now < mSettleUntil;
  const float scaleBefore = mQuality.scale();
  float measuredScale = settling ? mQuality.scale()
                                 : mQuality.update(mQualitySample, wanted, mAdaptRingFull, slow, mAdaptMissed);
  // Foveation before pixels: the lenses blur the eyes' edges anyway, where it
  // takes its pixels, while a lower resolution blurs the whole board.
  if (!settling && mFoveationAuto && mFoveationAvailable) {
    const auto decision = mQuality.last_decision();
    if (decision == AdaptiveQuality::Decision::Lowered && mFoveation < kMaxAutoFoveation) {
      mQuality.start_at(scaleBefore);
      measuredScale = scaleBefore;
      ++mFoveation;
      LOGI("Stereo: foveation level %u instead of a lower resolution", mFoveation);
    } else if ((decision == AdaptiveQuality::Decision::Raised || decision == AdaptiveQuality::Decision::CalmAtCap) &&
               mFoveation > 0) {
      mQuality.start_at(scaleBefore);
      measuredScale = scaleBefore;
      --mFoveation;
      LOGI("Stereo: foveation level %u before a higher resolution", mFoveation);
    }
  }
  mAdaptMissed = 0;
  mRenderScale = std::min(1.0f, measuredScale / mMaxScale);
  if (mBoardMode && mShownHasWorld) mBoardQuality = measuredScale;
  // debug.partyboard.eye_scale <percent of recommended, 50 to 125>: a fixed
  // resolution instead of the adaptive one, read every second, so an A/B
  // capture (tools/quest_campaign.ps1) compares the same pixels.
  char pinned[PROP_VALUE_MAX] = {};
  __system_property_get("debug.partyboard.eye_scale", pinned);
  if (const float percent = std::strtof(pinned, nullptr); percent >= 50.0f && percent <= 125.0f) {
    mRenderScale = std::min(1.0f, percent / 100.0f / mMaxScale);
  } else if (mSpaceWarpWanted) {
    // Application SpaceWarp: the compositor's own work grows, and an image
    // that misses its look is shown for two of the XR thread's frames, 33 ms
    // at 120 Hz. The adaptive control kept 100% there and the eyes' pass took
    // 13.5 ms: 43 to 52 new images for 60 (2026-10-03). At most
    // debug.partyboard.space_warp_res percent of recommended (85 by default).
    char cap[PROP_VALUE_MAX] = {};
    __system_property_get("debug.partyboard.space_warp_res", cap);
    float percent = std::strtof(cap, nullptr);
    if (percent < 50.0f || percent > 125.0f) percent = 85.0f;
    mRenderScale = std::min(mRenderScale, percent / 100.0f / mMaxScale);
  }
  mAdaptLeases = mAdaptRingFull = 0;
  // New images only when they must grow, or have been far too large for a
  // while; layer() reallocates once the ring is empty.
  const auto [drawWidth, drawHeight] = eye_size(mRenderScale);
  if (mResizePending || now < mResizeRetryAt) {
    mImageSize.reset(); // pending, or failed a moment ago: the ring would empty every second
  } else if (mImageSize.update(drawWidth, drawHeight, mImageEyeWidth, mImageEyeHeight, mRenderScale != before)) {
    mResizePending = true;
    ++mResizeCount;
  }
  if (mRenderScale != before) {
    const auto decision = mQuality.last_decision();
    LOGI("Stereo: resolution %.0f%% of recommended (%ux%u per eye)%s, %u hitches in the last second",
         mRenderScale * mMaxScale * 100.0f, static_cast<unsigned>(mEyeWidth * mRenderScale),
         static_cast<unsigned>(mEyeHeight * mRenderScale),
         decision == AdaptiveQuality::Decision::Restored ? ", restored: fewer pixels brought no more images or hitches"
                                                          : "",
         mLastSlow);
  }
}

bool StereoView::images(void** eyeBuffers, void** hudBuffers, uint32_t capacity, uint32_t& count, uint32_t& width,
                        uint32_t& height, uint32_t& generation, uint32_t& hudWidth, uint32_t& hudHeight) {
  std::lock_guard lock{mMutex};
  if (mSlots.empty() || capacity < mSlots.size() || mSlots[0].buffer == nullptr || mSlots[0].hudBuffer == nullptr) {
    return false;
  }
  // Each buffer gets a reference for the caller, who imports it outside this
  // lock: the XR thread may replace the images meanwhile (a resize, or the
  // interface's size set at session start) and release its own references.
  // A buffer freed under the import aborted the game in Dawn ("Unsupported
  // VkFormat 0", Quest 3, 2026-09-28). The caller hands them back with
  // PartyBoardQuest_StereoReleaseImages once imported.
  for (size_t i = 0; i < mSlots.size(); ++i) {
    eyeBuffers[i] = mSlots[i].buffer;
    AHardwareBuffer_acquire(mSlots[i].buffer);
    hudBuffers[i] = mSlots[i].hudBuffer;
    AHardwareBuffer_acquire(mSlots[i].hudBuffer);
  }
  count = static_cast<uint32_t>(mSlots.size());
  width = mImageEyeWidth * 2;
  height = mImageEyeHeight;
  hudWidth = mHudPixelsWidth;
  hudHeight = mHudPixelsHeight;
  generation = mGeneration;
  return true;
}

float StereoView::resolution_percent() const {
  std::lock_guard lock{mMutex};
  const auto width = mShownHasWorld && mShownWidth ? mShownWidth : mImageEyeWidth;
  return mRecommendedEyeWidth ? 100.0f * width / mRecommendedEyeWidth : 0;
}

uint32_t StereoView::generation() const {
  std::lock_guard lock{mMutex};
  return mGeneration;
}

void StereoView::set_screen_required(bool required) {
  std::lock_guard lock{mMutex};
  mScreenRequired = required;
}

void StereoView::set_board_mode(bool board) {
  std::lock_guard lock{mMutex};
  // A board: never under 90% (AdaptiveQuality::set_floor).
  if (board != mBoardMode) {
    mSettleUntil = std::chrono::steady_clock::now() + std::chrono::seconds(3); // adapt_resolution()
    mQuality.set_floor(board ? 0.9f : 0.65f);
    mRenderScale = std::min(1.0f, mQuality.scale() / mMaxScale);
  }
  if (board && !mBoardMode && mQuality.scale() > mBoardQuality) {
    mQuality.start_at(mBoardQuality);
    mRenderScale = std::min(1.0f, mQuality.scale() / mMaxScale);
    LOGI("Stereo: a board again, resolution back to %.0f%% of recommended", mBoardQuality * 100.0f);
  }
  mBoardMode = board;
  // A minigame: its eyes at 100% from its start, up to 130% when the GPU has
  // room, by 10% steps: limited by its pixels, 5% steps never helped enough.
  // Started at 130%, Three Throw (its world drawn twice, it copies its frame)
  // fell to 60 images a second while the controller came down to 110%
  // (2026-10-06).
  if (const bool minigame = !board && !mScreenRequired; minigame != mMinigameQuality) {
    mMinigameQuality = minigame;
    mQuality.set_cap(minigame ? AdaptiveQuality::kMaxCap : mQualityCap);
    mQuality.set_step(minigame ? kMinigameQualityStep : 0.05f);
    if (minigame) {
      // The resolution this minigame last held, without the seconds of
      // hitches the controller takes to find it again (Manta Rings: 32 in
      // its first 5 s, 2026-10-10).
      const auto known = mSceneQuality.find(mScene);
      mQuality.start_at(known != mSceneQuality.end() ? known->second : kMinigameStartQuality);
    }
    mRenderScale = std::min(1.0f, mQuality.scale() / mMaxScale);
    const auto [width, height] = eye_size(mRenderScale);
    if (width > mImageEyeWidth || height > mImageEyeHeight) mResizePending = true;
    LOGI("Stereo: %s, resolution up to %.0f%% of recommended", minigame ? "a minigame" : "no minigame",
         mQuality.cap() * 100.0f);
  }
}

bool StereoView::world_only() const {
  std::lock_guard lock{mMutex};
  return mEnabled && !mScreenRequired;
}

bool StereoView::screen_required() const {
  std::lock_guard lock{mMutex};
  return mScreenRequired;
}

void StereoView::write_snapshot(GLuint texture, GLint x, GLint y, GLsizei width, GLsizei height, const char* name,
                                uint64_t tag) {
  if (texture == 0 || width <= 0 || height <= 0) return;
  GLuint framebuffer = 0;
  glGenFramebuffers(1, &framebuffer);
  glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
  std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4);
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
    glReadPixels(x, y, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
  }
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glDeleteFramebuffers(1, &framebuffer);
  const std::string path = mSnapshotDir + "/snapshot-" + std::to_string(tag) + "-" + name + ".rgba";
  if (FILE* file = std::fopen(path.c_str(), "wb")) {
    const uint32_t header[2] = {static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
    std::fwrite("RGBA", 1, 4, file);
    std::fwrite(header, sizeof(header), 1, file);
    std::fwrite(pixels.data(), 1, pixels.size(), file);
    std::fclose(file);
    LOGI("Stereo: snapshot %s (%dx%d)", path.c_str(), width, height);
  }
}

void StereoView::set_scene(int scene) {
  std::lock_guard lock{mMutex};
  if (scene == mScene) return;
  // Leaving a minigame: the resolution it held, unless a lowering was still
  // on trial (the controller's last decision undone or not yet judged).
  if (mMinigameQuality && mScene >= 0 && !mQuality.holding()) mSceneQuality[mScene] = mQuality.scale();
  mScene = scene;
}

void StereoView::set_scene_bounds(const float* low, const float* high) {
  std::lock_guard lock{mMutex};
  mBoundsValid = low != nullptr && high != nullptr;
  if (mBoundsValid) {
    for (int axis = 0; axis < 3; ++axis) {
      mBoundsLow[axis] = low[axis];
      mBoundsHigh[axis] = high[axis];
    }
    mBoundsAt = std::chrono::steady_clock::now();
  }
}

// Under mMutex, from game_frame(): each eye's field of view narrowed to the
// scene's box as that eye sees it, and the slot's drawn size by as much.
void StereoView::crop_eyes(Slot& slot) {
  const auto now = std::chrono::steady_clock::now();
  if (now - mCropReadAt >= std::chrono::seconds(2)) {
    mCropReadAt = now;
    char value[PROP_VALUE_MAX]{};
    __system_property_get("debug.partyboard.crop", value);
    const bool wanted = value[0] != '0';
    if (wanted != mCropEnabled) LOGI("Stereo: cropped eyes %s (debug.partyboard.crop)", wanted ? "on" : "off");
    mCropEnabled = wanted;
  }
  if (mCropLogAt == std::chrono::steady_clock::time_point{}) mCropLogAt = now;
  if (now - mCropLogAt >= std::chrono::seconds(5)) {
    if (mCropLeases > 0) {
      LOGI("Stereo crop: %u of %u images cropped, the eyes drew %.0f%% of their full fields' pixels "
           "(not cropped: %u without a box, %u nearly whole; %u boxes reaching behind an eye)",
           mCropFrames, mCropLeases, mCropFull > 0.0 ? 100.0 * mCropDrawn / mCropFull : 100.0, mCropNoBox,
           mCropWide, mCropNearEye);
    }
    mCropLogAt = now;
    mCropFrames = mCropLeases = 0;
    mCropNoBox = mCropNearEye = mCropWide = 0;
    mCropDrawn = mCropFull = 0.0;
  }
  ++mCropLeases;
  const double full = static_cast<double>(slot.renderWidth) * slot.renderHeight;
  mCropFull += full;
  // A stale box (a scene that stopped offering objects), the lenses' mask
  // (drawn in the full fields' pixels), or turned off: the full fields.
  const bool usable = mCropEnabled && mBoundsValid && now - mBoundsAt < std::chrono::milliseconds(300) &&
                      mHiddenRects[0].empty() && mHiddenRects[1].empty();
  if (!usable) {
    ++mCropNoBox;
    mCropDrawn += full;
    return;
  }
  constexpr float kMarginMeters = 0.04f; // a step or a jump past last frame's box
  constexpr float kMarginPart = 0.06f;   // of the cropped field, each side
  XrFovf fovs[2]{};
  for (int eye = 0; eye < 2; ++eye) {
    const XrFovf& whole = mViews[eye].fov;
    const XrPosef toEye = inverse(mViews[eye].pose);
    // The box's corners in front of the eye, and where its edges cross the
    // near plane: a box reaching behind the eye (a sea or a floor passing
    // under the player, Manta Rings 2026-10-10) still leaves out what lies
    // above or beside it, instead of giving the whole field up.
    constexpr float kNear = 0.05f;
    XrVector3f corners[8];
    for (int corner = 0; corner < 8; ++corner) {
      const XrVector3f room{(corner & 1) ? mBoundsHigh[0] + kMarginMeters : mBoundsLow[0] - kMarginMeters,
                            (corner & 2) ? mBoundsHigh[1] + kMarginMeters : mBoundsLow[1] - kMarginMeters,
                            (corner & 4) ? mBoundsHigh[2] + kMarginMeters : mBoundsLow[2] - kMarginMeters};
      corners[corner] = add(toEye.position, rotate(toEye.orientation, room));
    }
    float left = 1e9f, right = -1e9f, down = 1e9f, up = -1e9f;
    bool crossed = false;
    const auto take = [&](const XrVector3f& c) {
      const float depth = -c.z;
      left = std::min(left, c.x / depth);
      right = std::max(right, c.x / depth);
      down = std::min(down, c.y / depth);
      up = std::max(up, c.y / depth);
    };
    for (int corner = 0; corner < 8; ++corner) {
      const XrVector3f& a = corners[corner];
      if (-a.z >= kNear) take(a);
      for (int bit = 1; bit < 8; bit <<= 1) {
        if ((corner & bit) != 0) continue; // each edge once
        const XrVector3f& b = corners[corner | bit];
        const float da = -a.z - kNear, db = -b.z - kNear;
        if ((da < 0.0f) == (db < 0.0f)) continue;
        crossed = true;
        const float t = da / (da - db);
        take({a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, -kNear});
      }
    }
    if (crossed) ++mCropNearEye;
    const float spanX = right - left, spanY = up - down;
    left -= spanX * kMarginPart;
    right += spanX * kMarginPart;
    down -= spanY * kMarginPart;
    up += spanY * kMarginPart;
    const float wl = std::tan(whole.angleLeft), wr = std::tan(whole.angleRight);
    const float wd = std::tan(whole.angleDown), wu = std::tan(whole.angleUp);
    left = std::clamp(left, wl, wr);
    right = std::clamp(right, wl, wr);
    down = std::clamp(down, wd, wu);
    up = std::clamp(up, wd, wu);
    fovs[eye] = whole;
    if (right - left > 0.02f && up - down > 0.02f) {
      fovs[eye] = {std::atan(left), std::atan(right), std::atan(up), std::atan(down)};
    } else {
      // Out of this eye's view: a sliver at its center.
      fovs[eye] = {-0.01f, 0.01f, 0.01f, -0.01f};
    }
  }
  // One drawn size for both eyes (Aurora draws them side by side alike): the
  // larger of the two crops, at the full fields' pixel density.
  float partX = 0.0f, partY = 0.0f;
  for (int eye = 0; eye < 2; ++eye) {
    const XrFovf& whole = mViews[eye].fov;
    partX = std::max(partX, (std::tan(fovs[eye].angleRight) - std::tan(fovs[eye].angleLeft)) /
                                (std::tan(whole.angleRight) - std::tan(whole.angleLeft)));
    partY = std::max(partY, (std::tan(fovs[eye].angleUp) - std::tan(fovs[eye].angleDown)) /
                                (std::tan(whole.angleUp) - std::tan(whole.angleDown)));
  }
  if (partX > 0.95f && partY > 0.95f) {
    ++mCropWide;
    mCropDrawn += full; // nearly all of it anyway
    return;
  }
  slot.renderWidth = std::max(64u, static_cast<uint32_t>(std::ceil(slot.renderWidth * partX / 8.0f)) * 8u);
  slot.renderHeight = std::max(64u, static_cast<uint32_t>(std::ceil(slot.renderHeight * partY / 8.0f)) * 8u);
  // The smaller crop widened to that size, about its center, so both eyes'
  // pixels stay square and as dense.
  for (int eye = 0; eye < 2; ++eye) {
    const XrFovf& whole = mViews[eye].fov;
    const float wx = std::tan(whole.angleRight) - std::tan(whole.angleLeft);
    const float wy = std::tan(whole.angleUp) - std::tan(whole.angleDown);
    float l = std::tan(fovs[eye].angleLeft), r = std::tan(fovs[eye].angleRight);
    float d = std::tan(fovs[eye].angleDown), u = std::tan(fovs[eye].angleUp);
    const float cx = (l + r) * 0.5f, cy = (d + u) * 0.5f;
    const float hx = wx * partX * 0.5f, hy = wy * partY * 0.5f;
    l = cx - hx;
    r = cx + hx;
    d = cy - hy;
    u = cy + hy;
    slot.views[eye].fov = {std::atan(l), std::atan(r), std::atan(u), std::atan(d)};
  }
  ++mCropFrames;
  mCropDrawn += static_cast<double>(slot.renderWidth) * slot.renderHeight;
}

bool StereoView::minigame_mode() const {
  std::lock_guard lock{mMutex};
  return !mScreenRequired && !mBoardMode;
}

void StereoView::set_window(bool on, const XrPosef& pose, XrExtent2Df size, bool framed, float viewerDistance) {
  std::lock_guard lock{mMutex};
  if (on != mWindowOn || (on && framed != mWindowFramed)) {
    LOGI("Stereo: %s (%.2f x %.2f m at %.2f %.2f %.2f)",
         !on ? "no window" : framed ? "the game seen through a window" : "the game in 3D from its camera's place",
         size.width, size.height, pose.position.x, pose.position.y, pose.position.z);
  }
  mWindowOn = on;
  mWindowFramed = framed;
  mWindowViewerDistance = viewerDistance;
  mWindowPose = pose;
  mWindowSize = size;
}

void StereoView::set_hud_crop(float x, float y, float width, float height) {
  std::lock_guard lock{mMutex};
  const auto clamp01 = [](float v) { return std::isfinite(v) ? std::clamp(v, 0.0f, 1.0f) : 0.0f; };
  x = clamp01(x);
  y = clamp01(y);
  width = std::max(0.05f, std::min(clamp01(width), 1.0f - x));
  height = std::max(0.05f, std::min(clamp01(height), 1.0f - y));
  if (std::abs(width - mHudCrop[2]) > 0.001f || std::abs(height - mHudCrop[3]) > 0.001f ||
      std::abs(x - mHudCrop[0]) > 0.001f || std::abs(y - mHudCrop[1]) > 0.001f) {
    LOGI("Stereo: interface shown from %.2f %.2f, %.2f x %.2f of the screen", x, y, width, height);
  }
  mHudCrop = {x, y, width, height};
}

std::array<float, 4> StereoView::hud_crop() const {
  std::lock_guard lock{mMutex};
  return mHudCrop;
}

const XrCompositionLayerBaseHeader* StereoView::window_second_layer() const {
  return window_shown() ? reinterpret_cast<const XrCompositionLayerBaseHeader*>(&mWindowLayers[1]) : nullptr;
}

void StereoView::submitted(uint32_t image, uint64_t tag, int syncFd, bool hasWorld, bool hudDrawn) {
  std::lock_guard lock{mMutex};
  if (image >= mSlots.size() || mSlots[image].state != State::Drawing || mSlots[image].tag != tag) {
    if (syncFd >= 0) {
      close(syncFd);
    }
    // An interface it drew is not coming.
    if (hudDrawn) mHudLost = true;
    return;
  }
  mSlots[image].state = State::Ready;
  mSlots[image].submitNs = steady_ns();
  mSlots[image].fence = syncFd;
  mSlots[image].hasWorld = hasWorld;
  // Unchanged, Aurora kept the one before: the headset keeps showing it.
  if (!hudDrawn) mSlots[image].hud = false;
}

void StereoView::cancelled(uint32_t image, uint64_t tag) {
  std::lock_guard lock{mMutex};
  if (image < mSlots.size() && mSlots[image].state == State::Drawing && mSlots[image].tag == tag) {
    if (mSlots[image].hud) mHudLost = true;
    release_slot(mSlots[image]);
  }
}

std::array<float, 3> StereoView::render_info() const {
  std::lock_guard lock{mMutex};
  return {static_cast<float>(mShownHasWorld && mShownWidth ? mShownWidth : mImageEyeWidth),
          static_cast<float>(mShownHasWorld && mShownHeight ? mShownHeight : mImageEyeHeight),
          mEnabled ? mWorldRate : 0};
}

StereoView::Slot* StereoView::newest_completed(int64_t displayNs) {
  Slot* newest = nullptr;
  for (auto& slot : mSlots) {
    if (slot.state != State::Ready || slot.tag <= mPresentedTag) continue;
    if (mPacing && !mPacer.is_due(slot.dueNs, displayNs)) continue; // finished early: its display frame comes
    pollfd finished{slot.fence, POLLIN, 0};
    const bool complete = slot.fence < 0 ||
        (poll(&finished, 1, 0) > 0 && (finished.revents & POLLIN));
    if (complete && (!newest || slot.tag > newest->tag)) newest = &slot;
  }
  return newest;
}

// Copies a finished image into the headset's images and hands them over:
// from layer() at its look, or early (early_copy()) as soon as it finished,
// for the display frame after. displayTime: the frame it is first shown
// in; holdExtra: display frames the one before stays on show beyond those
// layer() counted.
void StereoView::present(Slot* newest, int fence, int64_t doneNs, int64_t finishedNs, bool shownLate,
                         CopyTimer* copyTimer, XrTime displayTime, uint32_t holdExtra) {
  const auto copyStart = std::chrono::steady_clock::now();
  GLsync copyFence = nullptr;
  bool accepted = false;
  // The GPU waits for Aurora's drawing; no CPU stall here.
  if (fence >= 0) {
    const EGLint attributes[] = {EGL_SYNC_NATIVE_FENCE_FD_ANDROID, fence, EGL_NONE};
    EGLSyncKHR sync = g_gl.createSync(mDisplay, EGL_SYNC_NATIVE_FENCE_ANDROID, attributes);
    if (sync != EGL_NO_SYNC_KHR) {
      g_gl.waitSync(mDisplay, sync, 0);
      g_gl.destroySync(mDisplay, sync);
    } else {
      close(fence);
    }
  }
  uint32_t index = 0;
  XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
  XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
  wait.timeout = XR_INFINITE_DURATION;
  const bool keepBoard = mShown && newest->board && !newest->hasWorld && mShownHasWorld && mShownIsBoard;
  const auto acquireImage = [&](XrSwapchain swapchain, uint32_t& image, size_t destination) {
    const auto start = std::chrono::steady_clock::now();
    const bool acquired = XR_SUCCEEDED(xrAcquireSwapchainImage(swapchain, &acquire, &image));
    const auto waitStart = std::chrono::steady_clock::now();
    const bool ready = acquired && XR_SUCCEEDED(xrWaitSwapchainImage(swapchain, &wait));
    const auto end = std::chrono::steady_clock::now();
    mDestinationAcquireMaxMs[destination] = std::max(mDestinationAcquireMaxMs[destination],
        std::chrono::duration<double, std::milli>(waitStart - start).count());
    if (acquired) {
      mDestinationWaitMaxMs[destination] = std::max(mDestinationWaitMaxMs[destination],
          std::chrono::duration<double, std::milli>(end - waitStart).count());
    }
    mAcquireMaxMs = std::max(mAcquireMaxMs,
        std::chrono::duration<double, std::milli>(end - start).count());
    return ready;
  };
  if (keepBoard || acquireImage(mSwapchain, index, 0)) {
    uint32_t hudIndex = 0;
    const auto hudNow = std::chrono::steady_clock::now();
    // Only from an image that drew it; at every image's rate, 30 Hz at most.
    const bool copyHud = newest->hud && (!mHudShown || !mHudEveryImage ||
                                         hudNow - mHudCopiedAt >= std::chrono::milliseconds(33));
    const bool hudReady = copyHud && acquireImage(mHudSwapchain, hudIndex, 1);
    // Acquire both destinations before queuing copies, then flush once.
    if (copyTimer) g_gl.queryCounter(copyTimer->queries[0], GL_TIMESTAMP_EXT);

    if (!keepBoard) {
      // Only the drawn part of each eye's half (dynamic resolution): at the
      // left, centered vertically (Aurora's common.cpp draws it there, a
      // half being the image's width / 2). The images are replaced only
      // when every slot is free, so this slot has the current images' size.
      const auto y = static_cast<GLint>((mEyeHeight - newest->renderHeight) / 2);
      const auto srcY = static_cast<GLint>((mImageEyeHeight - newest->renderHeight) / 2);
      const uint32_t srcStride = mImageEyeWidth;
      for (uint32_t eye = 0; eye < 2; ++eye) {
        g_gl.copyImage(newest->texture, GL_TEXTURE_2D, 0, static_cast<GLint>(eye * srcStride), srcY, 0,
                       mSwapchainImages[index].image, GL_TEXTURE_2D, 0, static_cast<GLint>(eye * mEyeWidth), y, 0,
                       static_cast<GLsizei>(newest->renderWidth), static_cast<GLsizei>(newest->renderHeight), 1);
      }
    }
    // The model's depth for the compositor (enable_depth_layer).
    if (!keepBoard && mDepthWanted && mDepthSwapchain != XR_NULL_HANDLE) {
      mDepthShown = draw_depth(*newest);
    }
    if (!keepBoard && mSpaceWarpWanted && mMotionSwapchain != XR_NULL_HANDLE) {
      mSpaceWarpShown = draw_space_warp(*newest);
    }
    if (hudReady) {
      mHudCopiedAt = hudNow;
      g_gl.copyImage(newest->hudTexture, GL_TEXTURE_2D, 0, 0, 0, 0,
                    mHudImages[hudIndex].image, GL_TEXTURE_2D, 0, 0, 0, 0,
                    static_cast<GLsizei>(mHudPixelsWidth), static_cast<GLsizei>(mHudPixelsHeight), 1);

    }
    // A snapshot: this image's left eye and its interface (set_snapshot_dir).
    if (mSnapshotWanted && newest->hud && !mSnapshotDir.empty()) {
      mSnapshotWanted = false;
      const auto srcY = static_cast<GLint>((mImageEyeHeight - newest->renderHeight) / 2);
      write_snapshot(newest->texture, 0, srcY, static_cast<GLsizei>(newest->renderWidth),
                     static_cast<GLsizei>(newest->renderHeight), "eye", newest->tag);
      write_snapshot(newest->hudTexture, 0, 0, static_cast<GLsizei>(mHudPixelsWidth),
                     static_cast<GLsizei>(mHudPixelsHeight), "hud", newest->tag);
    }
    // Keep the source lease until this GPU copy completes. The compositor
    // synchronizes the released destination; the game must not reuse its source.
    if (copyTimer) {
      g_gl.queryCounter(copyTimer->queries[1], GL_TIMESTAMP_EXT);
      copyTimer->pending = true;
    }
    copyFence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    // When the GPU finishes the copy ("Copy GPU" probe): it waits behind
    // the game's own drawing unless the driver lets it go first.
    EGLSyncKHR copyDone = EGL_NO_SYNC_KHR;
    if (mProbes && g_gl.dupNativeFence != nullptr && newest->copyDoneFd < 0) {
      const EGLint none[] = {EGL_NONE};
      copyDone = g_gl.createSync(mDisplay, EGL_SYNC_NATIVE_FENCE_ANDROID, none);
    }
    if (copyFence != nullptr) {
      glFlush();
      if (copyDone != EGL_NO_SYNC_KHR) {
        newest->copyDoneFd = g_gl.dupNativeFence(mDisplay, copyDone);
        newest->copyFlushNs = steady_ns();
      }
    } else {
      LOGW("Stereo: copy fence unavailable, using synchronous fallback");
      glFinish();
    }
    if (copyDone != EGL_NO_SYNC_KHR) g_gl.destroySync(mDisplay, copyDone);
    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    if (hudReady) {
      mHudShown = XR_SUCCEEDED(xrReleaseSwapchainImage(mHudSwapchain, &release));
      if (mHudShown) ++mNewHudCount;
    }
    // Drawn but not copied (at most 30 copies a second, or no destination).
    if (newest->hud && !(hudReady && mHudShown)) mHudLost = true;
    accepted = keepBoard || XR_SUCCEEDED(xrReleaseSwapchainImage(mSwapchain, &release));
    if (accepted && !keepBoard) {
      ++mNewWorldCount;
      mShownViews[0] = newest->views[0];
      mShownViews[1] = newest->views[1];
      mShownHasWorld = newest->hasWorld;
      mShownIsBoard = newest->board;
      mShownWindow = newest->window;
      mShownWindowPose = newest->windowPose;
      mShownWindowSize = newest->windowSize;
      mShownWidth = newest->renderWidth;
      mShownHeight = newest->renderHeight;
      if (mHoldCounted) ++mHolds[std::min<uint32_t>(std::max<uint32_t>(mFramesSinceNew + holdExtra, 1), 4) - 1];
      mHoldCounted = true;
      mFramesSinceNew = 0;
    }
    // How long images take from the game's lease to the display.
    if (newest->leaseTime > 0 && displayTime > newest->leaseTime) {
      const double sample = static_cast<double>(displayTime - newest->leaseTime);
      mLatencyNs = mLatencyNs == 0.0 ? sample : mLatencyNs * 0.9 + sample * 0.1;
      mLatencyMinMs = mLatencyMinMs == 0.0 ? sample / 1e6 : std::min(mLatencyMinMs, sample / 1e6);
      mLatencyMaxMs = std::max(mLatencyMaxMs, sample / 1e6);
    }
    if (accepted) {
      mShown = true;
      mPresentedTag = newest->tag;
    }
    static bool loggedFirstImage = false;
    if (!loggedFirstImage) {
      LOGI("Stereo: first eye image copied to OpenXR (tag %llu)", static_cast<unsigned long long>(newest->tag));
      loggedFirstImage = true;
    }
  }
  std::lock_guard lock{mMutex};
  if (finishedNs > newest->startNs) mPacer.finished(finishedNs - newest->startNs);
  if (shownLate) note_miss(doneNs, newest->dueNs);
  {
    const int64_t copyNs = steady_ns();
    if (newest->submitNs > newest->leaseNs && newest->leaseNs != 0) mStageRecord.add(newest->submitNs - newest->leaseNs);
    if (doneNs > newest->submitNs && newest->submitNs != 0) {
      mStageGpu.add(doneNs - newest->submitNs);
      mStageWait.add(copyNs - doneNs);
    }
    if (newest->leaseNs != 0) mStageTotal.add(copyNs - newest->leaseNs);
  }
  mCopyMaxMs = std::max(mCopyMaxMs,
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - copyStart).count());
  if (copyFence != nullptr) {
    newest->copyFence = copyFence;
    newest->state = State::Copying;
  } else {
    release_slot(*newest);
  }
  if (accepted) ++mPresentedCount;
}
void StereoView::early_copy() {
  if (mSwapchain == XR_NULL_HANDLE) return;
  int64_t displayNs = 0, deadlineNs = 0;
  {
    std::lock_guard lock{mMutex};
    if (!mEarlyCopy || !mPacing || !mEnabled || mScreenRequired || !mShown || mResizePending || mPeriodNs <= 0 ||
        mLastLookNs == 0 || mFrameTime == 0) {
      return;
    }
    displayNs = mFrameTime + mPeriodNs;
    // Back before the next look: it comes about a period after the last one,
    // and xrWaitFrame must not be late.
    deadlineNs = mLastLookNs + mPeriodNs - kEarlyMarginNs;
  }
  Slot* due = nullptr;
  uint64_t tag = 0;
  for (;;) {
    int fd = -1;
    bool recording = false;
    {
      std::lock_guard lock{mMutex};
      due = nullptr;
      for (auto& slot : mSlots) {
        if (slot.dueNs != 0 && slot.tag > mPresentedTag && (slot.state == State::Drawing || slot.state == State::Ready) &&
            std::llabs(slot.dueNs - displayNs) < mPeriodNs / 2) {
          due = &slot;
        }
      }
      if (due == nullptr) return;
      recording = due->state == State::Drawing;
      fd = due->fence;
      tag = due->tag;
    }
    const int64_t now = steady_ns();
    if (now >= deadlineNs) return;
    if (recording) {
      // Not sent to the GPU yet (the render worker submits it): look again soon.
      std::this_thread::sleep_for(std::chrono::nanoseconds(std::min<int64_t>(1'000'000, deadlineNs - now)));
      continue;
    }
    if (fd >= 0) {
      // Only this thread closes a ready image's fence (release_slot, layer()).
      pollfd finished{fd, POLLIN, 0};
      const int timeoutMs = static_cast<int>((deadlineNs - now + 999'999) / 1'000'000);
      if (poll(&finished, 1, timeoutMs) <= 0 || !(finished.revents & POLLIN) || steady_ns() > deadlineNs) return;
    }
    break;
  }
  int fence = -1;
  {
    std::lock_guard lock{mMutex};
    if (due->state != State::Ready || due->tag != tag) return;
    fence = due->fence;
    due->fence = -1;
    mEarlyDisplayNs = displayNs;
  }
  const int64_t doneNs = fence_signal_time_ns(fence);
  present(due, fence, doneNs, due->startNs != 0 ? doneNs : 0, false, nullptr, displayNs, 1);
  std::lock_guard lock{mMutex};
  mSkipFrameCount = true;
}

const XrCompositionLayerBaseHeader* StereoView::layer(XrSpace space, const void* next) {
  if (mSwapchain == XR_NULL_HANDLE) {
    return nullptr;
  }
  CopyTimer* copyTimer = nullptr;
  if (mGpuTiming) {
    GLint disjoint = 0;
    glGetIntegerv(GL_GPU_DISJOINT_EXT, &disjoint);
    for (auto& timer : mCopyTimers) {
      if (timer.pending) {
        GLuint available = 0;
        glGetQueryObjectuiv(timer.queries[1], GL_QUERY_RESULT_AVAILABLE, &available);
        if (available) {
          GLuint64 start = 0, end = 0;
          g_gl.queryResult64(timer.queries[0], GL_QUERY_RESULT, &start);
          g_gl.queryResult64(timer.queries[1], GL_QUERY_RESULT, &end);
          if (!disjoint && end >= start) {
            mCopyGpuMaxMs = std::max(mCopyGpuMaxMs, (end - start) / 1e6);
            ++mCopyGpuSamples;
          }
          timer.pending = false;
        }
      }
      if (!timer.pending && !copyTimer) copyTimer = &timer;
    }
    if (disjoint) { mCopyGpuMaxMs = -1; mCopyGpuSamples = 0; }
  }
  Slot* newest = nullptr;
  int fence = -1;
  bool shownLate = false;
  {
    std::lock_guard lock{mMutex};
    // This look's time: the grid the game's frames start on.
    const auto lookAt = std::chrono::steady_clock::now();
    const int64_t lookNs = steady_ns();
    // With early copies, images are planned to finish by the early copy's
    // deadline, not the look: at the look the copy waits behind the game's
    // next eyes' pass. 3 ms before the look, 20% of the images still went
    // the slow way (build 194, 2026-10-04).
    mPacer.look(mEarlyCopy ? lookNs - kEarlyMarginNs : lookNs, mFrameTime, mPeriodNs);
    mJitterMinNs = std::min(mJitterMinNs, mPacer.jitter_ns());
    mJitterMaxNs = std::max(mJitterMaxNs, mPacer.jitter_ns());
    if (lookAt - mLastLayerAt > std::chrono::milliseconds(100)) {
      mHoldCounted = false; // the layer was away: no hold to count
    }
    mLastLayerAt = lookAt;
    mLastLookNs = lookNs;
    // An early copy is first shown in this frame: it already counted it.
    if (!mSkipFrameCount) ++mFramesSinceNew;
    mSkipFrameCount = false;
    for (auto& slot : mSlots) {
      if (slot.state == State::Copying && slot.copyFence != nullptr) {
        const GLenum status = glClientWaitSync(slot.copyFence, 0, 0);
        if (status == GL_ALREADY_SIGNALED || status == GL_CONDITION_SATISFIED) {
          glDeleteSync(slot.copyFence);
          slot.copyFence = nullptr;
          if (const int64_t copiedNs = fence_signal_time_ns(slot.copyDoneFd);
              copiedNs > slot.copyFlushNs && slot.copyFlushNs != 0) {
            mStageCopy.add(copiedNs - slot.copyFlushNs);
          }
          release_slot(slot);
        }
      }

    }
    // Do not queue a GPU wait on unfinished game work into the XR copy stream.
    // Resubmit the previous layer until at least one source is complete.
    if (mPacing && mPeriodNs > 0 && mShown) {
      const Slot* due = nullptr;
      for (const auto& slot : mSlots) {
        if (slot.dueNs != 0 && (slot.state == State::Drawing || slot.state == State::Ready) &&
            std::llabs(slot.dueNs - mFrameTime) < mPeriodNs / 2) {
          due = &slot;
        }
      }
      if (mEarlyDisplayNs != 0 && std::llabs(mEarlyDisplayNs - mFrameTime) < mPeriodNs / 2) {
        ++mDueEarly; // copied and handed over after the last frame
      } else if (due == nullptr) {
        ++mDueNone;
      } else if (due->state == State::Drawing) {
        ++mDueDrawing;
      } else {
        pollfd finished{due->fence, POLLIN, 0};
        const bool complete = due->fence < 0 || (poll(&finished, 1, 0) > 0 && (finished.revents & POLLIN));
        ++(complete ? mDueFinished : mDueUnfinished);
      }
    }
    newest = newest_completed(mFrameTime);
    // Older finished images will never be shown now.
    for (auto& slot : mSlots) {
      if (slot.state == State::Ready &&
          (slot.tag <= mPresentedTag || (newest && slot.tag < newest->tag))) {
        // Ready means submitted, not completed. Dropping a fence does not
        // make its image reusable while Aurora's GPU can still write to it.
        pollfd finished{slot.fence, POLLIN, 0};
        if (slot.fence < 0 || (poll(&finished, 1, 0) > 0 && (finished.revents & POLLIN))) {
          // Never shown: it missed its display frame. The pacer learned only
          // from the images shown, so the slowest ones, those replaced, never
          // counted and its 98% was nearer 85% (2026-10-04).
          if (const int64_t doneNs = fence_signal_time_ns(slot.fence); slot.startNs != 0 && doneNs > slot.startNs) {
            mPacer.finished(doneNs - slot.startNs);
            note_miss(doneNs, slot.dueNs);
          }
          ++mAdaptMissed;
          ++mMissedDropped;
          if (slot.hud) mHudLost = true;
          release_slot(slot);
        }
      }
    }
    if (newest != nullptr) {
      fence = newest->fence;
      newest->fence = -1;
      // Shown, but a display frame or more after the one it was paced for.
      if (mPacing && newest->dueNs != 0 && mFrameTime > newest->dueNs + mPeriodNs / 2) {
        ++mAdaptMissed;
        ++mMissedLate;
        shownLate = true;
      }
    }
  }
  // When the GPU finished it, before EGL takes the fence.
  const int64_t doneNs = newest != nullptr ? fence_signal_time_ns(fence) : 0;
  const int64_t finishedNs = newest != nullptr && newest->startNs != 0 ? doneNs : 0;

  // Dynamic resolution changed: new images at the new size, once every image
  // is back (game_frame() gives no lease meanwhile).
  if (newest == nullptr && resize_ready()) {
    const auto [width, height] = eye_size(mRenderScale);
    const uint32_t previousWidth = mImageEyeWidth, previousHeight = mImageEyeHeight;
    const bool resized = allocate_images(width, height, mDesiredHudWidth);
    if (resized) {
      LOGI("Stereo: images now %ux%u per eye (were %ux%u), HUD %ux%u, %u resizes since start", width, height,
           previousWidth, previousHeight, mHudPixelsWidth, mHudPixelsHeight, mResizeCount);
    } else {
      LOGW("Stereo: resize failed; retaining previous eye images, next try in 10 s");
    }
    std::lock_guard lock{mMutex}; // game_frame() reads the scale
    if (!resized) {
      // Retain the previous valid allocation instead of leaving a dead ring.
      mRenderScale = static_cast<float>(mImageEyeWidth) / mEyeWidth;
      mResizeRetryAt = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    }
    mResizePending = false;
  }

  if (newest != nullptr) {
    present(newest, fence, doneNs, finishedNs, shownLate, copyTimer, mFrameTime, 0);
  }

  {
    std::lock_guard lock{mMutex};
    const auto now = std::chrono::steady_clock::now();
    adapt_resolution();
    const double elapsed = std::chrono::duration<double>(now - mStatsAt).count();
    if (elapsed >= 2.0) {
      unsigned drawing = 0, ready = 0, copying = 0;
      for (const auto& slot : mSlots) {
        drawing += slot.state == State::Drawing;
        ready += slot.state == State::Ready;
        copying += slot.state == State::Copying;
      }
      LOGI("Stereo perf: source=%.1fHz presented=%.1fHz ringFull=%u copyMax=%.2fms slots=%u/%u/%u world=%d "
           "res=%.0f%% latency=%.1fms screenHidden=%d acquireCpuMax=%.2fms worldNew=%.1fHz hudNew=%.1fHz copyGpuMax=%.3fms gpuSamples=%u eye=%ux%u "
           "worldAcquireMax=%.3fms worldWaitMax=%.3fms hudAcquireMax=%.3fms hudWaitMax=%.3fms "
           "holds=%u/%u/%u/%u latencyMin=%.1fms latencyMax=%.1fms paced=%d paceWork=%.1fms paceLooks=%lld pacePhase=%.1fms "
           "missedLate=%u missedDropped=%u",
          mLeaseCount / elapsed, mPresentedCount / elapsed, mRingFullCount, mCopyMaxMs,
          drawing, ready, copying, mShownHasWorld, mRenderScale * mMaxScale * 100.0f, mLatencyNs / 1e6, mScreenHidden, mAcquireMaxMs, mNewWorldCount / elapsed, mNewHudCount / elapsed, mCopyGpuMaxMs, mCopyGpuSamples,
          mShownWidth, mShownHeight, mDestinationAcquireMaxMs[0], mDestinationWaitMaxMs[0],
          mDestinationAcquireMaxMs[1], mDestinationWaitMaxMs[1], mHolds[0], mHolds[1], mHolds[2], mHolds[3],
          mLatencyMinMs, mLatencyMaxMs, mPacing, mPacer.work_ns() / 1e6, static_cast<long long>(mPacer.looks()),
          mPacer.phase_ns() / 1e6, mMissedLate, mMissedDropped);
      if (const uint32_t replans = mPacer.take_replans(); replans != 0) {
        LOGI("Pacing: the plan grew at once %u times (images over budget)", replans);
      }
      LOGI("Pacing: missed %u finished before their look (up to %.1f ms before), %u after it (avg %.1f, max %.1f ms "
           "after); looks %.1f to %.1f ms after the earliest planned, %.1f ms before their display",
           mMissDoneEarly, mMissEarlyMaxNs / 1e6, mMissDoneLate,
           mMissDoneLate ? mMissLateSumNs / 1e6 / mMissDoneLate : 0.0, mMissLateMaxNs / 1e6, mJitterMinNs / 1e6,
           mJitterMaxNs / 1e6, mPacer.lead_ns() / 1e6);
      LOGI("Pacing looks: due image copied early %u, none %u, recording %u, GPU unfinished %u, finished %u; images "
           "due 0/1/2+ display frames after the one before: %u/%u/%u",
           mDueEarly, mDueNone, mDueDrawing, mDueUnfinished, mDueFinished, mDueSteps[0], mDueSteps[1], mDueSteps[2]);
      mDueEarly = mDueNone = mDueDrawing = mDueUnfinished = mDueFinished = 0;
      mEarlyCopies = 0;
      char earlyCopy[PROP_VALUE_MAX] = {};
      __system_property_get("debug.partyboard.early_copy", earlyCopy);
      if (const bool early = earlyCopy[0] != '0'; early != mEarlyCopy) {
        LOGI("Stereo: early copies %s (debug.partyboard.early_copy)", early ? "on" : "off");
        mEarlyCopy = early;
      }
      LOGI("Late latch: %u frames corrected, the eye moved %.1f mm avg / %.1f max meanwhile", mLateApplied,
           mLateApplied ? mLateMovedSumMm / mLateApplied : 0.0, mLateMovedMaxMm);
      mLateApplied = 0;
      mLateMovedSumMm = mLateMovedMaxMm = 0;
      char foveation[PROP_VALUE_MAX] = {};
      __system_property_get("debug.partyboard.foveation", foveation);
      // Automatic with `auto` only; `0` to `3` fix a level in Aurora, off here.
      // Off by default: on the headset (2026-10-05) levels 1 to 3 saved
      // nothing measurable on the Toad board and the player preferred
      // the image without it.
      if (const bool automatic = std::strcmp(foveation, "auto") == 0;
          automatic != mFoveationAuto) {
        LOGI("Stereo: automatic foveation %s (debug.partyboard.foveation)", automatic ? "on" : "off");
        mFoveationAuto = automatic;
        if (!automatic) mFoveation = 0;
      }
      char lateLatch[PROP_VALUE_MAX] = {};
      __system_property_get("debug.partyboard.late_latch", lateLatch);
      if (const bool late = lateLatch[0] != '0'; late != mLateLatch) {
        LOGI("Stereo: late latch %s (debug.partyboard.late_latch)", late ? "on" : "off");
        mLateLatch = late;
      }
      mDueSteps[0] = mDueSteps[1] = mDueSteps[2] = 0;
      mMissDoneEarly = mMissDoneLate = 0;
      mMissLateSumNs = mMissLateMaxNs = mMissEarlyMaxNs = mJitterMinNs = mJitterMaxNs = 0;
      mMissedLate = mMissedDropped = 0;
      LOGI("Stereo pipeline: images=%u record=%.1f/%.1fms gpu=%.1f/%.1fms wait=%.1f/%.1fms total=%.1f/%.1fms "
           "(avg/max) ring=%zu full=%u (at full, images drawing/ready/copying: %u/%u/%u)",
           mStageTotal.count, mStageRecord.avg(), mStageRecord.max, mStageGpu.avg(), mStageGpu.max, mStageWait.avg(),
           mStageWait.max, mStageTotal.avg(), mStageTotal.max, mSlots.size(), mRingFullCount, mFullDrawing, mFullReady,
           mFullCopying);
      if (mProbes) {
        LOGI("Copy GPU: %u copies, sent to finished %.2f ms avg, %.2f max", mStageCopy.count, mStageCopy.avg(),
             mStageCopy.max);
      }
      // Probes (debug.partyboard.probes 1): the copies' GPU end ("Copy GPU"),
      // a native fence made and read for each copy.
      char snapshot[PROP_VALUE_MAX] = {};
      __system_property_get("debug.partyboard.snapshot", snapshot);
      if (snapshot[0] != '\0' && mSnapshotValue != snapshot) {
        mSnapshotValue = snapshot;
        mSnapshotWanted = true;
        mHudLost = true; // the interface drawn again, for the snapshot to have it
      }
      char probes[PROP_VALUE_MAX] = {};
      __system_property_get("debug.partyboard.probes", probes);
      if (const bool on = std::strcmp(probes, "1") == 0; on != mProbes) {
        LOGI("Stereo: probes %s (debug.partyboard.probes)", on ? "on" : "off");
        mProbes = on;
      }
      mStageRecord = mStageGpu = mStageWait = mStageTotal = mStageCopy = Stage{};
      mFullDrawing = mFullReady = mFullCopying = 0;
      mHolds.fill(0);
      mLatencyMinMs = mLatencyMaxMs = 0;
      // A/B switch, read every 2 s: `adb shell setprop debug.partyboard.xr_pacing 0`.
      char pacing[PROP_VALUE_MAX] = {};
      __system_property_get("debug.partyboard.xr_pacing", pacing);
      // On by default (debug.partyboard.xr_pacing 0 turns it off): at 72 Hz,
      // holding each finished image for its look gave them all the same age,
      // 13.9 ms, and 38 ms from motion to photon, the most comfortable; off,
      // their ages went from 14 to 28 ms (2026-10-03). At 90 Hz it cost 10 ms.
      mPacing = std::strcmp(pacing, "0") != 0;
      char align[PROP_VALUE_MAX] = {};
      __system_property_get("debug.partyboard.xr_align", align);
      mAlign = std::strcmp(align, "0") != 0;
      char depthLayer[PROP_VALUE_MAX] = {};
      __system_property_get("debug.partyboard.depth_layer", depthLayer);
      // Off by default (debug.partyboard.depth_layer 1 turns it on): the board
      // shook as much with it as without (2026-10-03, the head's translation is
      // what moves it: covering the tracking cameras stilled it), and its
      // reprojection took the compositor from 2 to 4 ms a displayed frame.
      const bool depthWanted = depthLayer[0] == '1' && mDepthSwapchain != XR_NULL_HANDLE;
      if (depthWanted != mDepthWanted) {
        LOGI("Depth layer: %s (debug.partyboard.depth_layer)", depthWanted ? "submitted" : "off");
        mDepthWanted = depthWanted;
        if (!depthWanted) mDepthShown = false;
      }
      char spaceWarp[PROP_VALUE_MAX] = {};
      __system_property_get("debug.partyboard.space_warp", spaceWarp);
      const bool spaceWarpWanted = spaceWarp[0] == '1' && mMotionSwapchain != XR_NULL_HANDLE;
      if (spaceWarpWanted != mSpaceWarpWanted) {
        LOGI("Space warp: %s (debug.partyboard.space_warp)", spaceWarpWanted ? "on" : "off");
        mSpaceWarpWanted = spaceWarpWanted;
        if (!spaceWarpWanted) mSpaceWarpShown = false;
      }
      char hudRate[PROP_VALUE_MAX] = {};
      __system_property_get("debug.partyboard.hud_rate", hudRate);
      const bool everyImage = std::strcmp(hudRate, "full") == 0;
      const bool half = std::strcmp(hudRate, "half") == 0;
      if (everyImage != mHudEveryImage || half != mHudHalf || !mHudRateLogged) {
        LOGI("Stereo: interface drawn %s (debug.partyboard.hud_rate=%s)",
             everyImage ? "in every image" : half ? "every other image" : "about 26 times a second",
             hudRate[0] != '\0' ? hudRate : "unset");
        mHudEveryImage = everyImage;
        mHudHalf = half;
        mHudRateLogged = true;
      }
      mWorldRate = static_cast<float>(mNewWorldCount / elapsed);
      mStatsAt = now;
      mLeaseCount = mRingFullCount = mPresentedCount = 0;
      mCopyMaxMs = mAcquireMaxMs = 0;
      mDestinationAcquireMaxMs.fill(0);
      mDestinationWaitMaxMs.fill(0);
      mNewWorldCount = mNewHudCount = 0;
      mCopyGpuMaxMs = -1; mCopyGpuSamples = 0;
    }
  }
  if (!mShown) {
    return nullptr;
  }
  if (mShownWindow) {
    // The window: one quad per eye, each showing that eye's half of the image,
    // opaque (the game's background, Aurora's window clear).
    const uint32_t shownWidth = mShownWidth != 0 ? mShownWidth : mEyeWidth;
    const uint32_t shownHeight = mShownHeight != 0 ? mShownHeight : mEyeHeight;
    for (uint32_t eye = 0; eye < 2; ++eye) {
      auto& quad = mWindowLayers[eye];
      quad = {XR_TYPE_COMPOSITION_LAYER_QUAD};
      quad.next = next;
      quad.space = space;
      quad.eyeVisibility = eye == 0 ? XR_EYE_VISIBILITY_LEFT : XR_EYE_VISIBILITY_RIGHT;
      quad.subImage.swapchain = mSwapchain;
      quad.subImage.imageRect = {
          {static_cast<int32_t>(eye * mEyeWidth), static_cast<int32_t>((mEyeHeight - shownHeight) / 2)},
          {static_cast<int32_t>(shownWidth), static_cast<int32_t>(shownHeight)}};
      quad.pose = mShownWindowPose;
      quad.size = mShownWindowSize;
    }
    return reinterpret_cast<const XrCompositionLayerBaseHeader*>(&mWindowLayers[0]);
  }
  for (uint32_t eye = 0; eye < 2; ++eye) {
    auto& view = mProjectionViews[eye];
    view = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
    view.pose = mShownViews[eye].pose;
    view.fov = mShownViews[eye].fov;
    view.subImage.swapchain = mSwapchain;
    const uint32_t shownWidth = mShownWidth != 0 ? mShownWidth : mEyeWidth;
    const uint32_t shownHeight = mShownHeight != 0 ? mShownHeight : mEyeHeight;
    view.subImage.imageRect = {{static_cast<int32_t>(eye * mEyeWidth), static_cast<int32_t>((mEyeHeight - shownHeight) / 2)},
                               {static_cast<int32_t>(shownWidth), static_cast<int32_t>(shownHeight)}};
    if (mDepthWanted && mDepthShown) {
      auto& depth = mDepthInfos[eye];
      depth = {XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR};
      depth.subImage.swapchain = mDepthSwapchain;
      depth.subImage.imageRect = view.subImage.imageRect;
      depth.minDepth = 0.0f;
      depth.maxDepth = 1.0f;
      depth.nearZ = kNear;
      depth.farZ = kFar;
      view.next = &depth;
    }
    if (mSpaceWarpWanted && mSpaceWarpShown) {
      auto& warp = mSpaceWarpInfos[eye];
      warp = {XR_TYPE_COMPOSITION_LAYER_SPACE_WARP_INFO_FB};
      warp.next = view.next;
      warp.motionVectorSubImage.swapchain = mMotionSwapchain;
      warp.motionVectorSubImage.imageRect = {{static_cast<int32_t>(eye * mMotionWidth), 0},
                                             {static_cast<int32_t>(mMotionWidth), static_cast<int32_t>(mMotionHeight)}};
      warp.appSpaceDeltaPose = {{0, 0, 0, 1}, {0, 0, 0}}; // the stage does not move
      warp.depthSubImage.swapchain = mMotionDepthSwapchain;
      warp.depthSubImage.imageRect = warp.motionVectorSubImage.imageRect;
      warp.minDepth = 0.0f;
      warp.maxDepth = 1.0f;
      warp.nearZ = kNear;
      warp.farZ = kFar;
      view.next = &warp;
    }
  }
  mLayer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
  mLayer.next = next;
  // Where nothing of the model was drawn, the room shows through.
  mLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
  mLayer.space = space;
  mLayer.viewCount = 2;
  mLayer.views = mProjectionViews;
  return reinterpret_cast<const XrCompositionLayerBaseHeader*>(&mLayer);
}

const XrCompositionLayerBaseHeader* StereoView::hud_layer(XrSpace space, const XrPosef& pose, XrExtent2Df size,
                                                          const void* next) {
  if (!world_visible() || !mHudShown) return nullptr;
  mHudLayer = {XR_TYPE_COMPOSITION_LAYER_QUAD};
  mHudLayer.next = next;
  mHudLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
  mHudLayer.space = space;
  mHudLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
  mHudLayer.subImage.swapchain = mHudSwapchain;
  // Only the part set_hud_crop gave (rows from the interface's top).
  const auto crop = hud_crop();
  mHudLayer.subImage.imageRect = {
      {static_cast<int32_t>(crop[0] * mHudPixelsWidth), static_cast<int32_t>(crop[1] * mHudPixelsHeight)},
      {std::max(1, static_cast<int32_t>(crop[2] * mHudPixelsWidth)),
       std::max(1, static_cast<int32_t>(crop[3] * mHudPixelsHeight))}};
  mHudLayer.pose = pose;
  mHudLayer.size = size;
  return reinterpret_cast<const XrCompositionLayerBaseHeader*>(&mHudLayer);
}

// The interface's screen: a static image, a bezel around a dark face, drawn
// once (GL context current). Without it the interface shows on its own.
void StereoView::create_hud_screen() {
  constexpr int kWidth = 256, kHeight = 192; // the interface's 4:3
  XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
  info.createFlags = XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT;
  info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
  info.format = mSwapchainFormat;
  info.sampleCount = 1;
  info.width = kWidth;
  info.height = kHeight;
  info.faceCount = 1;
  info.arraySize = 1;
  info.mipCount = 1;
  if (!check(mInstance, xrCreateSwapchain(mSession, &info, &mHudScreenSwapchain), "xrCreateSwapchain (HUD screen)")) {
    mHudScreenSwapchain = XR_NULL_HANDLE;
    return;
  }
  uint32_t count = 0;
  xrEnumerateSwapchainImages(mHudScreenSwapchain, 0, &count, nullptr);
  std::vector<XrSwapchainImageOpenGLESKHR> images(count, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
  xrEnumerateSwapchainImages(mHudScreenSwapchain, count, &count,
                             reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));
  uint32_t index = 0;
  XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
  XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
  wait.timeout = XR_INFINITE_DURATION;
  if (count == 0 || XR_FAILED(xrAcquireSwapchainImage(mHudScreenSwapchain, &acquire, &index)) ||
      XR_FAILED(xrWaitSwapchainImage(mHudScreenSwapchain, &wait))) {
    xrDestroySwapchain(mHudScreenSwapchain);
    mHudScreenSwapchain = XR_NULL_HANDLE;
    return;
  }
  // The bezel's width in pixels: kHudBezel of the interface's width on each
  // side of a quad that is the interface plus its bezel.
  const auto bezel = static_cast<GLint>(std::lround(kWidth * kHudBezel / (1.0f + 2.0f * kHudBezel)));
  GLuint framebuffer = 0;
  glGenFramebuffers(1, &framebuffer);
  glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, images[index].image, 0);
  const bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
  if (complete) {
    // Linear colors (the image is sRGB), alpha premultiplied: a dark grey
    // bezel, and a nearly black face that lets a little of the room through.
    glViewport(0, 0, kWidth, kHeight);
    glClearColor(0.035f, 0.035f, 0.045f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    constexpr float kFaceAlpha = 0.88f;
    glEnable(GL_SCISSOR_TEST);
    glScissor(bezel, bezel, kWidth - 2 * bezel, kHeight - 2 * bezel);
    glClearColor(0.003f * kFaceAlpha, 0.004f * kFaceAlpha, 0.008f * kFaceAlpha, kFaceAlpha);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
  }
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glDeleteFramebuffers(1, &framebuffer);
  glFinish();
  XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
  xrReleaseSwapchainImage(mHudScreenSwapchain, &release);
  if (!complete) {
    LOGW("Stereo: HUD screen unavailable (framebuffer incomplete)");
    xrDestroySwapchain(mHudScreenSwapchain);
    mHudScreenSwapchain = XR_NULL_HANDLE;
  }
}

const XrCompositionLayerBaseHeader* StereoView::hud_screen_layer(XrSpace space, const XrPosef& pose, XrExtent2Df size) {
  if (!world_visible() || !mHudShown || mHudScreenSwapchain == XR_NULL_HANDLE) return nullptr;
  mHudScreenLayer = {XR_TYPE_COMPOSITION_LAYER_QUAD};
  mHudScreenLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
  mHudScreenLayer.space = space;
  mHudScreenLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
  mHudScreenLayer.subImage.swapchain = mHudScreenSwapchain;
  mHudScreenLayer.subImage.imageRect = {{0, 0}, {256, 192}};
  mHudScreenLayer.pose = pose;
  // The interface's size plus the bezel on each side (the same in both
  // directions, as on a real screen).
  const float bezel = size.width * kHudBezel;
  mHudScreenLayer.size = {size.width + 2.0f * bezel, size.height + 2.0f * bezel};
  return reinterpret_cast<const XrCompositionLayerBaseHeader*>(&mHudScreenLayer);
}

} // namespace quest

// The game side (src/port/quest_stereo.cpp) finds these with dlsym.
extern "C" {

__attribute__((visibility("default"))) bool PartyBoardQuest_StereoScreenHidden() {
  return quest::g_stereoView != nullptr && quest::g_stereoView->screen_hidden();
}

__attribute__((visibility("default"))) bool PartyBoardQuest_StereoWorldOnly() {
  return quest::g_stereoView != nullptr && quest::g_stereoView->world_only();
}

__attribute__((visibility("default"))) void PartyBoardQuest_StereoScreenRequired(bool required) {
  if (quest::g_stereoView != nullptr) quest::g_stereoView->set_screen_required(required);
}

__attribute__((visibility("default"))) void PartyBoardQuest_StereoBoardMode(bool board) {
  if (quest::g_stereoView != nullptr) quest::g_stereoView->set_board_mode(board);
}

__attribute__((visibility("default"))) void PartyBoardQuest_StereoCancelled(uint32_t image, uint64_t tag) {
  if (quest::g_stereoView != nullptr) {
    quest::g_stereoView->cancelled(image, tag);
  }
}

__attribute__((visibility("default"))) void PartyBoardQuest_StereoFoveationAvailable(bool available) {
  if (quest::g_stereoView != nullptr) quest::g_stereoView->set_foveation_available(available);
}

__attribute__((visibility("default"))) bool PartyBoardQuest_StereoLateLatch(uint64_t tag, float correction[2][16]) {
  quest::StereoView* view = quest::g_stereoView;
  return view != nullptr && correction != nullptr && view->late_latch(tag, correction);
}

__attribute__((visibility("default"))) bool PartyBoardQuest_StereoFrame(quest::StereoFrame* out) {
  quest::StereoView* view = quest::g_stereoView;
  return view != nullptr && out != nullptr && view->game_frame(*out);
}

// Game thread: `capacity` eye buffers and as many HUD buffers, each with a
// reference the caller gives back (PartyBoardQuest_StereoReleaseImages).
__attribute__((visibility("default"))) bool PartyBoardQuest_StereoImages(void** eyeBuffers, void** hudBuffers,
                                                                         uint32_t capacity, uint32_t* count,
                                                                         uint32_t* width, uint32_t* height,
                                                                         uint32_t* generation, uint32_t* hudWidth,
                                                                         uint32_t* hudHeight) {
  quest::StereoView* view = quest::g_stereoView;
  return view != nullptr && view->images(eyeBuffers, hudBuffers, capacity, *count, *width, *height, *generation,
                                        *hudWidth, *hudHeight);
}

// Game thread: the references PartyBoardQuest_StereoImages gave, once imported.
__attribute__((visibility("default"))) void PartyBoardQuest_StereoReleaseImages(void** buffers, uint32_t count) {
  for (uint32_t i = 0; i < count; ++i) {
    if (buffers[i] != nullptr) {
      AHardwareBuffer_release(static_cast<AHardwareBuffer*>(buffers[i]));
    }
  }
}

// Game thread, its frame done: when its next frame starts (steady-clock
// nanoseconds), on the display's schedule; 0: the game keeps its own clock.
__attribute__((visibility("default"))) int64_t PartyBoardQuest_NextFrameStart(int64_t nowNs, float targetHz) {
  quest::StereoView* view = quest::g_stereoView;
  return view != nullptr ? view->next_frame_start(nowNs, targetHz) : 0;
}

__attribute__((visibility("default"))) uint32_t PartyBoardQuest_StereoHiddenArea(uint32_t eye, float* rects,
                                                                                uint32_t capacity, uint32_t* version) {
  quest::StereoView* view = quest::g_stereoView;
  uint32_t current = 0;
  const uint32_t count = view != nullptr ? view->hidden_area(eye, rects, capacity, current) : 0;
  if (version != nullptr) *version = current;
  return count;
}

__attribute__((visibility("default"))) void PartyBoardQuest_StereoHudCrop(float x, float y, float width,
                                                                          float height) {
  if (quest::StereoView* view = quest::g_stereoView) view->set_hud_crop(x, y, width, height);
}

__attribute__((visibility("default"))) void PartyBoardQuest_StereoScene(int scene) {
  if (quest::StereoView* view = quest::g_stereoView) view->set_scene(scene);
}

__attribute__((visibility("default"))) void PartyBoardQuest_StereoSceneBounds(const float* low, const float* high) {
  if (quest::StereoView* view = quest::g_stereoView) view->set_scene_bounds(low, high);
}

__attribute__((visibility("default"))) uint32_t PartyBoardQuest_StereoGeneration(void) {
  quest::StereoView* view = quest::g_stereoView;
  return view != nullptr ? view->generation() : 0;
}

__attribute__((visibility("default"))) void PartyBoardQuest_StereoSubmitted(uint32_t image, uint64_t tag, int syncFd, bool hasWorld,
                                                                            bool hudDrawn, void*) {
  quest::StereoView* view = quest::g_stereoView;
  if (view != nullptr) {
    view->submitted(image, tag, syncFd, hasWorld, hudDrawn);
  } else if (syncFd >= 0) {
    close(syncFd);
  }
}

} // extern "C"
